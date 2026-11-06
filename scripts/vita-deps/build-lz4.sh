#!/usr/bin/env bash
set -euo pipefail
SCRIPT_SIGNATURE="$(cksum < "${BASH_SOURCE[0]}")"

VITASDK="${VITASDK:-/usr/local/vitasdk}"
SYSROOT="${VITASDK}/arm-vita-eabi"
JOBS="${OPENMW_VITA_JOBS:-4}"
WORKDIR="${1:-$(pwd)/vita-deps-build/lz4}"
LZ4_COMMIT=ebb370ca83af193212df4dcbadcc5d87bc0de2f0
export PATH="${VITASDK}/bin:${PATH}"

mkdir -p "${WORKDIR}"
cd "${WORKDIR}"
if [ ! -d lz4 ]; then
    git clone --depth=1 --branch=v1.10.0 https://github.com/lz4/lz4.git lz4
fi
if [ "$(git -C lz4 rev-parse HEAD)" != "${LZ4_COMMIT}" ] \
    || [ -n "$(git -C lz4 status --porcelain --untracked-files=no)" ]; then
    echo "ERROR: lz4 must be clean at ${LZ4_COMMIT}; existing checkout was left unchanged." >&2
    exit 1
fi

BUILD_SIGNATURE="${LZ4_COMMIT}|${SYSROOT}|${SCRIPT_SIGNATURE}|$(cksum < "${VITASDK}/bin/arm-vita-eabi-gcc")"
LIB_SIGNATURE=missing
if [ -f lz4/lib/liblz4.a ]; then LIB_SIGNATURE="$(cksum < lz4/lib/liblz4.a)"; fi
if [ "$(cat lz4_done 2>/dev/null || true)" != "${BUILD_SIGNATURE}|${LIB_SIGNATURE}" ]; then
    make -C lz4 clean
fi
make -C lz4 -j"${JOBS}" CC=arm-vita-eabi-gcc AR=arm-vita-eabi-ar \
    PREFIX="${SYSROOT}" BUILD_SHARED=no lib
cp lz4/lib/liblz4.a "${SYSROOT}/lib/"
cp lz4/lib/lz4.h lz4/lib/lz4hc.h lz4/lib/lz4frame.h lz4/lib/lz4frame_static.h "${SYSROOT}/include/"
printf '%s|%s\n' "${BUILD_SIGNATURE}" "$(cksum < lz4/lib/liblz4.a)" > lz4_done
