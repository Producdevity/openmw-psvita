#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SDK_DIR="${OPENMW_VITA_SDK:?Set OPENMW_VITA_SDK to an isolated Linux-host VitaSDK directory}"
GL_SRC="${VITAGL_SRC:?Set VITAGL_SRC to the built vitaGL working tree}"
GL_CONTAINER="${VITAGL_DIR:-/cache/vitagl}"
BUILD_DIR="${OPENMW_VITA_BUILD_DIR:-${SRC_DIR}/build-vita}"
CACHE_DIR="${OPENMW_VITA_CACHE_DIR:-${SRC_DIR}/build-vita-cache}"
JOBS="${OPENMW_VITA_JOBS:-4}"
BAKE_MODS="${OPENMW_VITA_BAKE_MODS:-ON}"
PLATFORM="${OPENMW_VITA_DOCKER_PLATFORM:-linux/$(uname -m)}"
case "${PLATFORM}" in
    linux/aarch64) PLATFORM=linux/arm64 ;;
    linux/x86_64) PLATFORM=linux/amd64 ;;
esac
case "$(printf '%s' "${BAKE_MODS}" | tr '[:lower:]' '[:upper:]')" in
    1|ON|YES|TRUE|Y) BAKE_MODS=ON ;;
    0|OFF|NO|FALSE|N) BAKE_MODS=OFF ;;
    *) echo "OPENMW_VITA_BAKE_MODS must be a boolean value" >&2; exit 2 ;;
esac
IMAGE="${OPENMW_VITA_HOST_IMAGE:-openmw-vita-development:${PLATFORM##*/}}"
CLEAN=0
CONFIGURE=0
WANT_VPK=0
for arg in "$@"; do
    case "${arg}" in
        --clean) CLEAN=1 ;;
        --configure) CONFIGURE=1 ;;
        --vpk) WANT_VPK=1 ;;
        *) echo "Unknown argument: ${arg}" >&2; exit 2 ;;
    esac
done

case "${JOBS}" in
    ''|*[!0-9]*|0) echo "OPENMW_VITA_JOBS must be a positive integer" >&2; exit 2 ;;
esac
case "${GL_CONTAINER}" in
    /*) ;;
    *) echo "VITAGL_DIR must be an absolute container path" >&2; exit 2 ;;
esac
test -x "${SDK_DIR}/bin/arm-vita-eabi-gcc"
test -f "${GL_SRC}/libvitaGL.a"
test -f "${GL_SRC}/source/vitaGL.h"

canonical_path() {
    python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1"
}
SDK_DIR="$(canonical_path "${SDK_DIR}")"
GL_SRC="$(canonical_path "${GL_SRC}")"
BUILD_DIR="$(canonical_path "${BUILD_DIR}")"
CACHE_DIR="$(canonical_path "${CACHE_DIR}")"
if [ "${BUILD_DIR}" = "${SRC_DIR}" ] || [ "${BUILD_DIR}" = / ]; then
    echo "Refusing to use the source root or filesystem root as a build directory" >&2
    exit 2
fi

BUILD_CONTAINER=/work/build
MOUNTS=(--mount "type=bind,src=${SRC_DIR},dst=/src")
case "${BUILD_DIR}" in
    "${SRC_DIR}"/*) BUILD_CONTAINER="/src/${BUILD_DIR#"${SRC_DIR}"/}" ;;
    *) MOUNTS+=(--mount "type=bind,src=${BUILD_DIR},dst=${BUILD_CONTAINER}") ;;
esac
if [ -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    cache_value() {
        sed -n "s/^$1:[^=]*=//p" "${BUILD_DIR}/CMakeCache.txt"
    }
    if [ "$(cache_value VITAGL_DIR)" != "${GL_CONTAINER}" ] \
        || [ "$(cache_value OPENMW_VITA_BAKE_MODS)" != "${BAKE_MODS}" ]; then
        CONFIGURE=1
    fi
    if [ -n "${ICU_HOST_BUILD_DIR:-}" ] \
        && [ "$(cache_value ICU_HOST_BUILD_DIR)" != "${ICU_HOST_BUILD_DIR}" ]; then
        CONFIGURE=1
    fi
    if [ -n "${OPENMW_VITA_FETCH_ROOT:-}" ]; then
        for dependency in BULLET MYGUI OSG RECASTNAVIGATION SQLITE3 YAML-CPP; do
            directory="$(printf '%s' "${dependency}" | tr '[:upper:]' '[:lower:]')"
            if [ "$(cache_value "FETCHCONTENT_SOURCE_DIR_${dependency}")" \
                != "${OPENMW_VITA_FETCH_ROOT}/${directory}" ]; then
                CONFIGURE=1
            fi
        done
    fi
fi
COMMAND=(scripts/vita/build.sh)
if [ "${CLEAN}" = 1 ] && [ -d "${BUILD_DIR}" ]; then
    COMMAND+=(clean)
elif [ "${CONFIGURE}" = 0 ] && [ -f "${BUILD_DIR}/Makefile" ]; then
    COMMAND=(scripts/vita/build-fast.sh)
    if [ "${WANT_VPK}" = 1 ]; then COMMAND+=(--vpk); fi
fi
mkdir -p "${BUILD_DIR}" "${CACHE_DIR}"
python3 - "${SDK_DIR}" "${BUILD_DIR}" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

sdk, build = map(Path, sys.argv[1:])
identity = {
    "sdk_root": str(sdk),
    "compiler_sha256": {
        name: hashlib.sha256((sdk / "bin" / name).read_bytes()).hexdigest()
        for name in ("arm-vita-eabi-gcc", "arm-vita-eabi-g++")
    },
}
path = build / ".openmw-vita-sdk.json"
configured = (build / "CMakeCache.txt").exists() or (build / "Makefile").exists()
previous = json.loads(path.read_text()) if path.exists() else None
if configured and previous != identity:
    sys.exit("SDK/compiler identity differs or was not recorded; select a fresh OPENMW_VITA_BUILD_DIR")
path.write_text(json.dumps(identity, sort_keys=True, indent=2) + "\n")
PY

docker build --platform "${PLATFORM}" --target development-host \
    --tag "${IMAGE}" --file "${SRC_DIR}/Dockerfile.vita" "${SRC_DIR}"

ENV_ARGS=(--env "OPENMW_VITA_BAKE_MODS=${BAKE_MODS}")
for name in ICU_HOST_BUILD_DIR OPENMW_VITA_FETCH_ROOT; do
    if [ -n "${!name:-}" ]; then ENV_ARGS+=(--env "${name}=${!name}"); fi
done

docker run --rm --platform "${PLATFORM}" --cpus "${JOBS}" --memory "${OPENMW_VITA_MEMORY:-6g}" \
    --user "$(id -u):$(id -g)" \
    "${MOUNTS[@]}" \
    --mount "type=bind,src=${SDK_DIR},dst=/usr/local/vitasdk" \
    --mount "type=bind,src=${GL_SRC},dst=/vitagl-source,readonly" \
    --mount "type=bind,src=${CACHE_DIR},dst=/cache" \
    --env "VITAGL_DIR=${GL_CONTAINER}" \
    --env "OPENMW_VITA_BUILD_DIR=${BUILD_CONTAINER}" \
    --env "OPENMW_VITA_JOBS=${JOBS}" \
    --env CCACHE_DIR=/cache/ccache --env CCACHE_MAXSIZE=2G --env VITA_ELF_ARCHIVE=/cache/elfs \
    "${ENV_ARGS[@]}" --workdir /src "${IMAGE}" -c '
        set -e
        mkdir -p "${VITAGL_DIR}/include"
        cmp -s /vitagl-source/libvitaGL.a "${VITAGL_DIR}/libvitaGL.a" \
            || cp /vitagl-source/libvitaGL.a "${VITAGL_DIR}/libvitaGL.a"
        cmp -s /vitagl-source/source/vitaGL.h "${VITAGL_DIR}/include/vitaGL.h" \
            || cp /vitagl-source/source/vitaGL.h "${VITAGL_DIR}/include/vitaGL.h"
        exec bash "$@"
    ' openmw-vita-build "${COMMAND[@]}"
