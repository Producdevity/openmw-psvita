#!/bin/bash
# Build vitaGL for OpenMW Vita port.
# Usage: ./build-vitagl.sh [vitagl_dir]    (default: ~/vitaGL)
set -e

VITAGL_DIR="${1:-${HOME}/vitaGL}"
VITASDK="${VITASDK:-/usr/local/vitasdk}"

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

echo "=== Building vitaGL ==="
echo "Target: ${VITAGL_DIR}"

if [ ! -d "${VITAGL_DIR}" ]; then
    git clone https://github.com/Rinnegatamante/vitaGL.git "${VITAGL_DIR}"
fi

cd "${VITAGL_DIR}"

# Flag set must match Dockerfile.vita.
# NOTE: NO_TILE_CLIPPER and USE_SCRATCH_MEMORY were tried and produced visual artifacts
# NO_SPLASHSCREEN: splash thread races GXM init (launch crash).
make -j"$(nproc)" \
    DEPTH_STENCIL_HACK=1 \
    DRAW_SPEEDHACK=1 MATH_SPEEDHACK=1 \
    TEXTURES_SPEEDHACK=1 BUFFERS_SPEEDHACK=1 \
    SAMPLERS_SPEEDHACK=1 UNIFORMS_SPEEDHACK=1 \
    PRIMITIVES_SPEEDHACK=1 \
    PHYCONT_ON_DEMAND=1 \
    NO_DEBUG=1 \
    NO_SPLASHSCREEN=1 \
    HAVE_SHADER_CACHE=1

if [ ! -f "${VITAGL_DIR}/libvitaGL.a" ]; then
    echo "ERROR: build completed but libvitaGL.a not found in ${VITAGL_DIR}"
    exit 1
fi

echo "[OK] libvitaGL.a produced at ${VITAGL_DIR}/libvitaGL.a"
