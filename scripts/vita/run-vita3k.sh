#!/bin/bash
# Build + install + launch OpenMW under the local Vita3K emulator (WSLg).
#
# Why this exists: heap instrumentation ([VitaAudit] lines in boot.log) can be
# read directly from the emulated ux0 as plain files — no device flashing.
#
# This Vita3K fork cannot run libshacccg.suprx (module_start SIGSEGVs under
# dynarmic), so runtime shader compilation is unavailable. vitaGL must be
# built with HAVE_VITA3K_SUPPORT=1 (drops sceGxmVshInitialize) and
# HAVE_SHADER_CACHE=1 (loads pre-compiled .gxp from
# ux0:data/shader_cache/OMWV00001/ instead of compiling). The cache must be
# primed by ONE run on real hardware with a HAVE_SHADER_CACHE=1 vitaGL build
# (see --build-hw-cache-vgl), then copied from the Vita's
# ux0:data/shader_cache/OMWV00001/ into the emulated ux0 path below.
# Without the cache the engine still boots and runs the whole ESM load
# (enough for the startup memory audit); it dies at the first 3D draw.
#
# Usage: scripts/vita/run-vita3k.sh [--rebuild-vgl] [--no-launch] [--build-hw-cache-vgl] [--keep-3k-eboot]
#   --rebuild-vgl        force rebuild of the Vita3K-variant vitaGL
#   --no-launch          build + install only, don't start the emulator
#   --build-hw-cache-vgl (re)build the HARDWARE vitaGL with HAVE_SHADER_CACHE=1
#                        so your next device run writes the shader cache; then exit
#   --keep-3k-eboot      skip the final hardware relink (build-vita/ keeps the
#                        emulator-only eboot; do NOT flash it to a device)
#
# IMPORTANT: by default this script re-links a HARDWARE eboot into build-vita/
# after installing the emulator one, so build-vita/apps/openmw/eboot.bin is
# always safe to flash.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${SRC_DIR}/build-vita"
if [ -z "${VITASDK}" ]; then
    [ -d "${HOME}/vitasdk" ] && VITASDK="${HOME}/vitasdk" || VITASDK=/usr/local/vitasdk
fi
VITAGL_DIR="${VITAGL_DIR:-${HOME}/vitaGL}"
TITLE_ID="OMWV00001"
V3K_BIN="${V3K_BIN:-${HOME}/Vita3K/build/linux-ninja-clang/bin/RelWithDebInfo/Vita3K}"
V3K_UX0="${HOME}/.local/share/Vita3K/Vita3K/ux0"
APP_DIR="${V3K_UX0}/app/${TITLE_ID}"
RUN_LOG="${HOME}/vita3k_openmw.log"

export PATH="${VITASDK}/bin:${PATH}"

REBUILD_VGL=0
NO_LAUNCH=0
KEEP_3K_EBOOT=0
for arg in "$@"; do
    case "${arg}" in
        --rebuild-vgl) REBUILD_VGL=1 ;;
        --no-launch) NO_LAUNCH=1 ;;
        --keep-3k-eboot) KEEP_3K_EBOOT=1 ;;
        --build-hw-cache-vgl)
            echo "=== Building HARDWARE vitaGL with shader cache (for priming on device) ==="
            cd "${VITAGL_DIR}"
            cp -f libvitaGL.a libvitaGL.prev.a 2>/dev/null || true
            make clean >/dev/null
            make HAVE_SHADER_CACHE=1 -j"$(nproc)" >/tmp/vgl_hw_cache.log 2>&1 \
                || { echo "vitaGL HW build FAILED"; tail -20 /tmp/vgl_hw_cache.log; exit 1; }
            cp -f libvitaGL.a libvitaGL.hw.a
            echo "Done. Rebuild the eboot (build-fast.sh), flash it, run the game once on"
            echo "the Vita, then copy ux0:data/shader_cache/${TITLE_ID}/ from the device into:"
            echo "  ${V3K_UX0}/data/shader_cache/${TITLE_ID}/"
            exit 0
            ;;
        *) echo "Unknown arg: ${arg}"; exit 1 ;;
    esac
done

# OpenMW's canonical vitaGL flags (must match scripts/vita-deps/build-vitagl.sh
# and Dockerfile.vita). Other flag combinations produced visual artifacts or
# device hangs — NEVER trust a pre-existing libvitaGL.a of unknown provenance
# (this machine's FNA project rebuilds ~/vitaGL with different flags).
OPENMW_VGL_FLAGS="DEPTH_STENCIL_HACK=1 DRAW_SPEEDHACK=1 MATH_SPEEDHACK=1 \
TEXTURES_SPEEDHACK=1 BUFFERS_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 UNIFORMS_SPEEDHACK=1 \
PRIMITIVES_SPEEDHACK=1 PHYCONT_ON_DEMAND=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1"

build_vgl_variant() { # $1 = output archive name, $2 = extra flags
    make clean >/dev/null
    # shellcheck disable=SC2086
    make ${OPENMW_VGL_FLAGS} $2 -j"$(nproc)" >/tmp/vgl_openmw_build.log 2>&1 \
        || { echo "vitaGL build FAILED ($1)"; tail -20 /tmp/vgl_openmw_build.log; exit 1; }
    cp -f libvitaGL.a "$1"
    echo "vitaGL variant built: $1 ($(stat -c%s "$1") bytes)"
}

# --- 1. Ensure both vitaGL variant archives exist, built by US with known flags ---
cd "${VITAGL_DIR}"
if [ ! -f libvitaGL.hw.a ] || [ "${REBUILD_VGL}" -eq 1 ]; then
    echo "=== Building HARDWARE vitaGL (OpenMW canonical flags) ==="
    build_vgl_variant libvitaGL.hw.a ""
fi
if [ ! -f libvitaGL.3k.a ] || [ "${REBUILD_VGL}" -eq 1 ]; then
    echo "=== Building Vita3K-variant vitaGL (canonical flags + 3K support) ==="
    build_vgl_variant libvitaGL.3k.a "HAVE_VITA3K_SUPPORT=1 NO_SPLASHSCREEN=1"
fi

# --- 2. Link the eboot against the 3K variant, then restore the HW archive ---
cp -f libvitaGL.3k.a libvitaGL.a

restore_hw_lib() {
    if [ -f "${VITAGL_DIR}/libvitaGL.hw.a" ]; then
        cp -f "${VITAGL_DIR}/libvitaGL.hw.a" "${VITAGL_DIR}/libvitaGL.a"
    fi
}
trap restore_hw_lib EXIT

echo "=== Relinking eboot against Vita3K vitaGL ==="
# CMake doesn't track libvitaGL.a as a dependency — force the relink.
find "${BUILD_DIR}/apps/openmw" -maxdepth 1 \( -name "openmw" -o -name "*.self" -o -name "eboot.bin" -o -name "*.velf" \) -delete 2>/dev/null || true
cd "${BUILD_DIR}"
make -j"$(nproc)" eboot.bin-self >/tmp/openmw_3k_link.log 2>&1 \
    || { echo "eboot relink FAILED"; tail -30 /tmp/openmw_3k_link.log; exit 1; }

# --- 3. Install into the emulated ux0 ---
if [ ! -f "${APP_DIR}/sce_sys/param.sfo" ]; then
    echo "=== First install: building VPK for sce_sys ==="
    make -j"$(nproc)" openmw.vpk-vpk >>/tmp/openmw_3k_link.log 2>&1 \
        || { echo "VPK build FAILED"; tail -30 /tmp/openmw_3k_link.log; exit 1; }
    mkdir -p "${APP_DIR}"
    unzip -oq "${BUILD_DIR}/apps/openmw/openmw.vpk" -d "${APP_DIR}"
fi
cp -f "${BUILD_DIR}/apps/openmw/eboot.bin" "${APP_DIR}/eboot.bin"
echo "eboot installed to ${APP_DIR}"

# --- 3b. Restore a HARDWARE-flashable eboot in build-vita ---
if [ "${KEEP_3K_EBOOT}" -eq 0 ]; then
    echo "=== Relinking HARDWARE eboot (build-vita stays flashable) ==="
    restore_hw_lib
    find "${BUILD_DIR}/apps/openmw" -maxdepth 1 \( -name "openmw" -o -name "*.self" -o -name "eboot.bin" -o -name "*.velf" \) -delete 2>/dev/null || true
    make -j"$(nproc)" eboot.bin-self >/tmp/openmw_hw_relink.log 2>&1 \
        || { echo "HW eboot relink FAILED"; tail -30 /tmp/openmw_hw_relink.log; exit 1; }
    echo "HW eboot: ${BUILD_DIR}/apps/openmw/eboot.bin"
else
    echo "WARNING: build-vita/apps/openmw/eboot.bin is the EMULATOR eboot — do not flash it."
fi

# --- 4. Preflight warnings ---
if [ ! -f "${V3K_UX0}/data/openmw/Data Files/Morrowind.esm" ]; then
    echo "WARNING: no Morrowind data at ${V3K_UX0}/data/openmw/Data Files/"
    echo "         Copy 'Data Files' from your Vita SD or a Morrowind install."
fi
if [ ! -d "${V3K_UX0}/data/shader_cache/${TITLE_ID}" ]; then
    echo "WARNING: no shader cache at ux0:data/shader_cache/${TITLE_ID}/ —"
    echo "         3D rendering will fail; ESM-load memory audit still works."
    echo "         Prime it with --build-hw-cache-vgl + one device run."
fi

[ "${NO_LAUNCH}" -eq 1 ] && { echo "Skipping launch (--no-launch)"; exit 0; }

# --- 5. Launch Vita3K (WSLg needs the d3d12 GL passthrough, not Zink) ---
echo "=== Launching Vita3K ==="
export DISPLAY="${DISPLAY:-:0}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export MESA_LOADER_DRIVER_OVERRIDE=d3d12
export GALLIUM_DRIVER=d3d12
export LIBGL_ALWAYS_SOFTWARE=0
export BROWSER=/bin/true
rm -f "${RUN_LOG}"
nohup "${V3K_BIN}" -r "${TITLE_ID}" >"${RUN_LOG}" 2>&1 &
echo $! > /tmp/v3k_openmw_pid
echo "Vita3K pid $(cat /tmp/v3k_openmw_pid); log: ${RUN_LOG}"
echo "Audit output: ${V3K_UX0}/data/openmw/boot.log  (grep VitaAudit)"
