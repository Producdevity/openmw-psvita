#!/bin/bash
# Build OpenMW for PS Vita
#
# Prerequisites:
#   - VitaSDK installed (VITASDK env or /usr/local/vitasdk)
#   - vitaGL built (VITAGL_DIR env or ~/vitaGL)
#   - Dependencies installed: scripts/vita-deps/build-all.sh
#
# Usage: scripts/vita/build.sh [clean]
#
# scripts/vita/docker-build.sh invokes this same configuration in Docker.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${OPENMW_VITA_BUILD_DIR:-${SRC_DIR}/build-vita}"
VITASDK="${VITASDK:-/usr/local/vitasdk}"
VITAGL_DIR="${VITAGL_DIR:-${HOME}/vitaGL}"
JOBS="${OPENMW_VITA_JOBS:-4}"
BUILD_DIR="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "${BUILD_DIR}")"
export OPENMW_VITA_BUILD_DIR="${BUILD_DIR}"

export PATH="${VITASDK}/bin:${PATH}"
# Must match build-fast.sh: a reconfigure under a different pkg-config
# changes dependency flags and recompiles the whole tree.
export PKG_CONFIG="${PKG_CONFIG:-arm-vita-eabi-pkg-config}"

echo "=== OpenMW Vita Build ==="
echo "Source:  ${SRC_DIR}"
echo "Build:   ${BUILD_DIR}"
echo "VitaSDK: ${VITASDK}"
echo "vitaGL:  ${VITAGL_DIR}"

# Validate
if [ ! -d "${VITASDK}" ]; then
    echo "ERROR: VitaSDK not found at ${VITASDK}"
    echo ""
    echo "Please install VitaSDK from https://vitasdk.org/"
    echo "Or set the VITASDK environment variable to your installation path."
    exit 1
fi

if [ ! -f "${VITAGL_DIR}/libvitaGL.a" ]; then
    echo "ERROR: vitaGL not found at ${VITAGL_DIR}/libvitaGL.a"
    echo ""
    echo "Please build vitaGL first:"
    echo "  scripts/vita-deps/build-vitagl.sh ${VITAGL_DIR}"
    echo ""
    echo "Or if you've already built it elsewhere, set VITAGL_DIR:"
    echo "  export VITAGL_DIR=/path/to/vitaGL"
    exit 1
fi

# Clean generated targets without deleting unrelated files in the build directory.
if [ "${1:-}" = "clean" ] && [ -d "${BUILD_DIR}" ]; then
    if [ "${BUILD_DIR}" = "${SRC_DIR}" ] || [ "${BUILD_DIR}" = / ] \
        || ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=${SRC_DIR}" "${BUILD_DIR}/CMakeCache.txt"; then
        echo "ERROR: refusing to clean a directory not configured for this source tree: ${BUILD_DIR}"
        exit 1
    fi
    echo "Cleaning generated build targets..."
    cmake --build "${BUILD_DIR}" --target clean --parallel "${JOBS}"
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# ICU host build (needed for data filtering during cross-compile)
ICU_HOST="${ICU_HOST_BUILD_DIR:-${BUILD_DIR}/extern/icu-host-build}"
if [ ! -f "${ICU_HOST}/bin/icupkg" ]; then
    echo "=== Building ICU host tools ==="
    ICU_SRC_URL="https://github.com/unicode-org/icu/archive/refs/tags/release-70-1.zip"
    ICU_FETCH_DIR="${BUILD_DIR}/extern/fetched/icu"

    # Fetch ICU source directly if not present
    if [ ! -f "${ICU_FETCH_DIR}/icu4c/source/configure" ]; then
        echo "Fetching ICU source..."
        mkdir -p "${BUILD_DIR}/extern/fetched"
        cd "${BUILD_DIR}/extern/fetched"

        if ! command -v wget &> /dev/null; then
            echo "ERROR: wget not found. Please install wget."
            exit 1
        fi

        wget -q -O icu-src.zip "${ICU_SRC_URL}"
        unzip -q icu-src.zip
        mv icu-release-70-1 icu
        rm icu-src.zip
        cd "${BUILD_DIR}"
    fi

    # ICU doesn't recognize arm-vita-eabi platform - copy mh-linux as mh-unknown
    # (This is needed for the Vita cross-compile that CMake will do later)
    cp -f "${ICU_FETCH_DIR}/icu4c/source/config/mh-linux" "${ICU_FETCH_DIR}/icu4c/source/config/mh-unknown"

    # Build host tools
    mkdir -p "${ICU_HOST}"
    cd "${ICU_HOST}"
    "${ICU_FETCH_DIR}/icu4c/source/configure" \
        --enable-static --disable-shared \
        --disable-extras --disable-tests --disable-samples --disable-icuio
    make -j"${JOBS}"

    # Validate host build
    if [ ! -f "${ICU_HOST}/bin/icupkg" ]; then
        echo "ERROR: ICU host tools build failed"
        exit 1
    fi

    cd "${BUILD_DIR}"
fi

# ccache, when installed: unchanged files are cache hits even when a full
# rebuild is forced (branch switch, clean build, toolchain change).
CCACHE_ARGS=()
if command -v ccache &> /dev/null; then
    CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    echo "ccache: on ($(ccache -s 2>/dev/null | grep -iE '^ *(cache size|Cache size)' | head -1 | sed 's/^ *//'))"
fi

FETCH_ARGS=()
if [ -n "${OPENMW_VITA_FETCH_ROOT:-}" ]; then
    for dependency in BULLET MYGUI OSG RECASTNAVIGATION SQLITE3 YAML-CPP; do
        directory="$(printf '%s' "${dependency}" | tr '[:upper:]' '[:lower:]')"
        FETCH_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_${dependency}=${OPENMW_VITA_FETCH_ROOT}/${directory}")
    done
fi

# Configure
echo "=== Configuring ==="
cmake -DCMAKE_TOOLCHAIN_FILE="${SRC_DIR}/cmake/VitaToolchain.cmake" \
      "${CCACHE_ARGS[@]}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DVITAGL_DIR="${VITAGL_DIR}" \
      -DICU_HOST_BUILD_DIR="${ICU_HOST}" \
      -DOPENMW_RESOURCES_ROOT="${BUILD_DIR}" \
      -DBUILD_OPENMW=ON \
      -DBUILD_LAUNCHER=OFF \
      -DBUILD_WIZARD=OFF \
      -DBUILD_ESSIMPORTER=OFF \
      -DBUILD_BSATOOL=OFF \
      -DBUILD_ESMTOOL=OFF \
      -DBUILD_NIFTEST=OFF \
      -DBUILD_BULLETOBJECTTOOL=OFF \
      -DBUILD_NAVMESHTOOL=OFF \
      -DBUILD_OPENCS=OFF \
      -DOPENMW_USE_SYSTEM_BULLET=OFF \
      -DOPENMW_USE_SYSTEM_OSG=OFF \
      -DOPENMW_USE_SYSTEM_MYGUI=OFF \
      -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=OFF \
      -DOPENMW_USE_SYSTEM_SQLITE3=OFF \
      -DOPENMW_USE_SYSTEM_YAML_CPP=OFF \
      -DOPENMW_USE_SYSTEM_ICU=OFF \
      -DOPENMW_VITA_BAKE_MODS="${OPENMW_VITA_BAKE_MODS:-ON}" \
      -D_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE=0 \
      "${FETCH_ARGS[@]}" \
      "${SRC_DIR}"

# Build full OSG (osgParticle/osgShadow needed but not in USED_OSG_COMPONENTS)
echo "=== Building OSG ==="
make -C _deps/osg-build -j"${JOBS}"

# Build eboot.bin
BUILD_ID="$(bash "${SCRIPT_DIR}/build-id.sh")"
echo "=== Building OpenMW (id ${BUILD_ID}) ==="
make -j"${JOBS}" eboot.bin-self

# Build VPK (needed for ATTRIBUTE2 extra memory mode)
echo "=== Building VPK ==="
make -j"${JOBS}" openmw.vpk-vpk

bash "${SCRIPT_DIR}/archive-build.sh" "${BUILD_ID}"

echo ""
echo "=== Build Complete (${BUILD_ID}) ==="
echo "eboot.bin: ${BUILD_DIR}/apps/openmw/eboot.bin"
echo "VPK:       ${BUILD_DIR}/apps/openmw/openmw.vpk"
echo ""
echo "Deploy eboot.bin for quick iteration."
echo "Deploy VPK for first install (includes param.sfo with extra memory mode)."
