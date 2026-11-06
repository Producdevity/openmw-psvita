#!/bin/bash
# Build/install LuaJIT for OpenMW Vita port
# This build uses the interpreter with JIT and FFI disabled.
set -e
SCRIPT_SIGNATURE="$(cksum < "${BASH_SOURCE[0]}")"

# Error cleanup handler
trap 'rm -f "${WORKDIR}/luajit_done"' ERR

VITASDK="${VITASDK:-/usr/local/vitasdk}"
SYSROOT="${VITASDK}/arm-vita-eabi"
JOBS="${OPENMW_VITA_JOBS:-4}"

echo "=== Installing LuaJIT for Vita ==="

WORKDIR="${1:-$(pwd)/vita-deps-build/luajit}"
LUAJIT_COMMIT=c329ddd10691c1875f26087ba23c2ae278515e24

echo "Building LuaJIT from source..."
echo "Work dir: ${WORKDIR}"

mkdir -p "${WORKDIR}"
WORKDIR="$(cd "${WORKDIR}" && pwd)"
cd "${WORKDIR}"

if [ ! -d "LuaJIT-Vita" ]; then
    git clone https://github.com/SonicMastr/LuaJIT-Vita.git
    git -C LuaJIT-Vita checkout "${LUAJIT_COMMIT}"
fi
if [ "$(git -C LuaJIT-Vita rev-parse HEAD)" != "${LUAJIT_COMMIT}" ] \
    || [ -n "$(git -C LuaJIT-Vita status --porcelain --untracked-files=no)" ]; then
    echo "ERROR: LuaJIT-Vita must be clean at ${LUAJIT_COMMIT}; existing checkout was left unchanged." >&2
    exit 1
fi
BUILD_SIGNATURE="${LUAJIT_COMMIT}|${SYSROOT}|${SCRIPT_SIGNATURE}|$(cksum < "${VITASDK}/bin/arm-vita-eabi-gcc")"
LIB_SIGNATURE=missing
if [ -f LuaJIT-Vita/src/libluajit.a ]; then
    LIB_SIGNATURE="$(cksum < LuaJIT-Vita/src/libluajit.a)"
fi
if [ "$(cat luajit_done 2>/dev/null)" != "${BUILD_SIGNATURE}|${LIB_SIGNATURE}" ]; then
    cd LuaJIT-Vita/src

    # This pinned LuaJIT requires 32-bit host generators for its 32-bit target.
    HOST_ARGS=(HOST_CC="gcc -m32")
    case "$(uname -m)" in
        aarch64|arm64)
            command -v arm-linux-gnueabihf-gcc >/dev/null
            command -v qemu-arm >/dev/null
            HOST_ARGS=(
                HOST_CC="arm-linux-gnueabihf-gcc -static"
                MINILUA_X="qemu-arm host/minilua"
                BUILDVM_X="qemu-arm host/buildvm"
            )
            ;;
    esac

    make clean || true
    make \
        "${HOST_ARGS[@]}" \
        CROSS=arm-vita-eabi- \
        TARGET_SYS=PSP2 \
        TARGET_FLAGS="-marm -fno-optimize-sibling-calls" \
        BUILDMODE=static \
        XCFLAGS="-DLUAJIT_DISABLE_JIT -DLUAJIT_DISABLE_FFI" \
        -j"${JOBS}"

    cd ../..
    printf '%s|%s\n' "${BUILD_SIGNATURE}" "$(cksum < LuaJIT-Vita/src/libluajit.a)" > luajit_done
fi

# Keep the pkg-config-selected library and headers paired with this build.
cd LuaJIT-Vita/src
cp luajit.h lua.h lualib.h lauxlib.h lua.hpp luaconf.h lj_arch.h "${SYSROOT}/include/"
cp libluajit.a "${SYSROOT}/lib/"
mkdir -p "${SYSROOT}/include/luajit-2.1"
cp luajit.h lua.h lualib.h lauxlib.h lua.hpp luaconf.h "${SYSROOT}/include/luajit-2.1/"
cp libluajit.a "${SYSROOT}/lib/libluajit-5.1.a"

echo "=== LuaJIT installation complete ==="
