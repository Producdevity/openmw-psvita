#!/bin/bash
# Build vitaGL for the OpenMW Vita port from our fork.
#
# Usage: scripts/vita-deps/build-vitagl.sh [vitagl_dir]   (default: ~/vitaGL)
#
# Pinned build (default): checks out VITAGL_COMMIT from VITAGL_REPO (the
# public fork Producdevity/vitaGL) into vitagl_dir and builds it there.
# Existing local changes are preserved; use the dev mode to build them.
#
# Dev build: VITAGL_SRC=~/Dev/vita/vitaGL VITAGL_COMMIT=dev builds that
# working tree as-is (uncommitted changes included) and installs the
# library and header into vitagl_dir, where CMake looks for them.
#
# Changes to vitaGL are commits in the fork, pushed to its remote: vitaGL
# changes that lived only in a local tree were lost once.
set -e
SCRIPT_SIGNATURE="$(cksum < "${BASH_SOURCE[0]}")"

VITAGL_DIR="${1:-${HOME}/vitaGL}"
VITASDK="${VITASDK:-/usr/local/vitasdk}"
VITAGL_REPO="${VITAGL_REPO:-https://github.com/Producdevity/vitaGL.git}"
# Public e119b3ec plus the static-VBO API and texture lifetime fixes.
VITAGL_COMMIT="${VITAGL_COMMIT:-558221254098d590ec39889a5ee8488b2ef99c5e}"
JOBS="${OPENMW_VITA_JOBS:-4}"

export VITASDK
export PATH="${VITASDK}/bin:${PATH}"

if [ ! -d "${VITASDK}" ]; then
    echo "ERROR: VitaSDK not found at ${VITASDK}"
    exit 1
fi

if ! command -v arm-vita-eabi-gcc &> /dev/null; then
    echo "ERROR: arm-vita-eabi-gcc not on PATH (VITASDK=${VITASDK})"
    exit 1
fi

if [ "${VITAGL_COMMIT}" = "dev" ]; then
    SRC="${VITAGL_SRC:?VITAGL_COMMIT=dev needs VITAGL_SRC=<working tree>}"
    echo "=== Building vitaGL (dev tree ${SRC}) ==="
    cd "${SRC}"
    STAMP="dev $(git rev-parse HEAD 2>/dev/null) $(git diff HEAD 2>/dev/null | shasum | cut -d' ' -f1)"
else
    echo "=== Building vitaGL ${VITAGL_COMMIT:0:10} from ${VITAGL_REPO} ==="
    if [ ! -d "${VITAGL_DIR}/.git" ]; then
        git clone "${VITAGL_REPO}" "${VITAGL_DIR}"
    fi
    cd "${VITAGL_DIR}"
    if ! git cat-file -e "${VITAGL_COMMIT}^{commit}" 2>/dev/null; then
        git fetch -q "${VITAGL_REPO}" "${VITAGL_COMMIT}"
    fi
    if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
        echo "ERROR: local changes in ${VITAGL_DIR}; use VITAGL_COMMIT=dev and VITAGL_SRC to build them." >&2
        exit 1
    fi
    git checkout -q --detach --no-overwrite-ignore "${VITAGL_COMMIT}"
    SRC="${VITAGL_DIR}"
    STAMP="$(git rev-parse HEAD)"
fi

STAMP="${STAMP}|${VITASDK}|${SCRIPT_SIGNATURE}|$(cksum < "${VITASDK}/bin/arm-vita-eabi-gcc")"
LIB_SIGNATURE=missing
if [ -f libvitaGL.a ]; then LIB_SIGNATURE="$(cksum < libvitaGL.a)"; fi

# vitaGL's Makefile tracks no header dependencies: objects from another
# commit would mix versions inside one libvitaGL.a. Rebuild clean whenever
# the source changes.
if [ "$(cat .omw-build-stamp 2>/dev/null)" != "${STAMP}|${LIB_SIGNATURE}" ]; then
    echo "vitaGL source changed: clean build"
    make clean >/dev/null 2>&1 || true
fi

# NOTE: NO_TILE_CLIPPER and USE_SCRATCH_MEMORY were tried and produced visual artifacts
# NO_SPLASHSCREEN: splash thread races GXM init (launch crash).
make -j"${JOBS}" \
    DEPTH_STENCIL_HACK=1 \
    DRAW_SPEEDHACK=1 MATH_SPEEDHACK=1 \
    TEXTURES_SPEEDHACK=1 BUFFERS_SPEEDHACK=1 \
    SAMPLERS_SPEEDHACK=1 UNIFORMS_SPEEDHACK=1 \
    PRIMITIVES_SPEEDHACK=1 \
    PHYCONT_ON_DEMAND=1 \
    NO_DEBUG=1 \
    NO_SPLASHSCREEN=1 \
    HAVE_SHADER_CACHE=1

if [ ! -f "${SRC}/libvitaGL.a" ]; then
    echo "ERROR: build completed but libvitaGL.a not found in ${SRC}"
    exit 1
fi
printf '%s|%s\n' "${STAMP}" "$(cksum < "${SRC}/libvitaGL.a")" > .omw-build-stamp

# Install where CMake looks: the library, and the header that matches it
# (CMake puts VITAGL_DIR/include ahead of the SDK's stock vitaGL.h).
mkdir -p "${VITAGL_DIR}/include"
if [ "${SRC}" != "${VITAGL_DIR}" ]; then
    cp -f "${SRC}/libvitaGL.a" "${VITAGL_DIR}/libvitaGL.a"
fi
# Copy only on change: nearly every TU includes vitaGL.h (via the GL
# headers), so a fresh timestamp alone would recompile most of the tree.
cmp -s "${SRC}/source/vitaGL.h" "${VITAGL_DIR}/include/vitaGL.h" \
    || cp -f "${SRC}/source/vitaGL.h" "${VITAGL_DIR}/include/vitaGL.h"

echo "[OK] libvitaGL.a at ${VITAGL_DIR}/libvitaGL.a (headers in ${VITAGL_DIR}/include)"
