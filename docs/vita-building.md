# Vita development builds

Use `scripts/vita/docker-build.sh` for an isolated Linux build on an ARM64 or
x86-64 host. It builds the `development-host` target in `Dockerfile.vita`, then
runs the normal build scripts with an explicit SDK and a built vitaGL checkout.
It does not install anything into a host SDK or fetch a private graphics fork.

```sh
export OPENMW_VITA_SDK="$PWD/build-perf/dependencies/vitasdk"
export VITAGL_SRC="$PWD/../vitaGL"
export OPENMW_VITA_BUILD_DIR="$PWD/build-perf/build-current"
export OPENMW_VITA_CACHE_DIR="$PWD/build-perf/docker-cache"
export OPENMW_VITA_JOBS=4
export OPENMW_VITA_MEMORY=6g
scripts/vita/docker-build.sh --configure
```

The SDK must contain Linux executables matching the container architecture.
The wrapper defaults to the host architecture; `OPENMW_VITA_DOCKER_PLATFORM`
can select another Docker platform. `OPENMW_VITA_HOST_IMAGE` selects the local
image tag. The wrapper builds that image from this repository before running it.
`VITAGL_DIR` optionally selects the container installation path for the matched
header/library (default `/cache/vitagl`); retain it when reusing a CMake build.
Build directories record the host SDK path and compiler hashes. The wrapper
rejects existing directories with missing or different identity records;
select a fresh output directory for them.

An existing build uses the incremental script. `--configure` reruns CMake;
`--vpk` also packages an incremental build. A fresh build always packages a VPK.
`--clean` runs CMake's generated clean target after checking that the cache
identifies this source tree; unrelated files remain. Use a fresh build directory
when changing SDKs or compilers. Build output, the compiler cache, and archived
ELF symbols remain under the selected build/cache directories. Keep existing
build directories separate.

`OPENMW_VITA_BAKE_MODS` defaults to `ON`, as in CMake. Set it explicitly to `OFF`
for an engine-only test package. Compare the same installed assets and settings;
changing the bundled mods changes the workload.

## Dependencies

The dependency recipe uses VitaSDK GCC 15.2.0 and these versions:

| Dependency | Source/version | Required configuration |
| --- | --- | --- |
| vitaGL | Published default: `Producdevity/vitaGL` at `558221254098d590ec39889a5ee8488b2ef99c5e` | Build with `scripts/vita-deps/build-vitagl.sh`; install matching library/header |
| Boost | 1.78.0 SDK package | Static libraries |
| FFmpeg | 6.1 | `scripts/vita-deps/build-ffmpeg.sh` |
| LuaJIT | `SonicMastr/LuaJIT-Vita` at `c329ddd10691c1875f26087ba23c2ae278515e24` | Static, JIT and FFI disabled |
| LZ4 | v1.10.0, `ebb370ca83af193212df4dcbadcc5d87bc0de2f0` | `scripts/vita-deps/build-lz4.sh` |
| ICU | 70.1 | CMake's bundled source and matching host tools |
| Bullet/MyGUI/OSG/Recast/SQLite/yaml-cpp | Pins and archive hashes in `extern/CMakeLists.txt` | Bundled source, including repository Vita patches |

Copy an SDK into the selected directory before installing dependencies. The
scripts below write to `VITASDK/arm-vita-eabi`, so run them only against that copy.
After building the host image, a dependency command has this form:

```sh
docker run --rm --platform linux/arm64 --cpus 4 --memory 6g \
  --mount "type=bind,src=$PWD,dst=/src" \
  --mount "type=bind,src=$OPENMW_VITA_SDK,dst=/usr/local/vitasdk" \
  --workdir /src openmw-vita-development:arm64 -c \
  'bash scripts/vita-deps/build-ffmpeg.sh /src/build-perf/dependencies/ffmpeg'
```

Use the same command with `build-luajit.sh` and `build-lz4.sh`, selecting separate
work directories. For a different host architecture, change both platform and
image tag. The original `build-all.sh` also builds dependencies that an existing
SDK may already supply; check versions before using it.

The FFmpeg script disables unused avfilter, avdevice, and postproc libraries.
FFmpeg 6.1 needs `-Wno-error=incompatible-pointer-types` with GCC 15 for existing
enum/int pointer uses; diagnostics remain visible. The SDK's FFmpeg 9 headers
are incompatible with this OpenMW source.

Pinned LuaJIT requires 32-bit host generators for its 32-bit target. On ARM64,
the host image supplies `arm-linux-gnueabihf-gcc`, `libc6-dev-armhf-cross`, and
`qemu-user`. The script builds static ARM host generators and invokes them via
`qemu-arm`. On x86-64 it uses `gcc -m32`. Both installed library names
(`libluajit.a` and pkg-config's `libluajit-5.1.a`) and both header locations are
updated together. An SDK-provided LuaJIT library must not silently replace this
JIT/FFI-disabled build.

The default vitaGL script fetches the public pin without discarding local edits.
For a development checkout, use `VITAGL_COMMIT=dev VITAGL_SRC=/path/to/checkout`
and pass a separate installation directory to `build-vitagl.sh`. The Docker
wrapper accepts a built checkout directly and pairs its `libvitaGL.a` with
`source/vitaGL.h`. Keep source revision, build flags, and both file hashes with
benchmark evidence.

## Reusing downloaded sources

`ICU_HOST_BUILD_DIR` can point to existing ICU 70.1 host tools.
`OPENMW_VITA_FETCH_ROOT` can point to directories named `bullet`, `mygui`, `osg`,
`recastnavigation`, `sqlite3`, and `yaml-cpp`. Both overrides are **container
paths**, normally beneath `/src`. These source overrides bypass FetchContent's
patch step: verify the archive hashes from `extern/CMakeLists.txt`, then apply
`patches/osg/vita-combined.patch` and `patches/mygui/vita-gui-dirty.patch` to fresh
sources before configuring. Do not reuse old modified source directories without
checking their contents. Zip extraction must preserve executable permissions,
particularly for ICU's configure tools.

A successful cross-build only establishes build compatibility. Use the process
in [vita-benchmarking.md](vita-benchmarking.md) for runtime comparisons; only
actual Vita hardware can establish the hardware frame-rate target.

## Verified development build, 2026-11-06

An ARM64 Ubuntu 24.04 container completed the ELF, SELF, VPK, and symbol archive
with VitaSDK 2026.08 (package sequence `2026102473`), target GCC 15.2.0, host GCC
13.2, and CMake 3.28.3. The dependency versions above were used, except vitaGL:
the tested local development revision was
`deca3e77fa428617d3499630f74df81d8caf1756`, adding clear-allocation guards. At
validation, the published default remained `558221254098d590ec39889a5ee8488b2ef99c5e`.

Build `654cf50159+dirty-20261106-140716` produced `eboot.bin` SHA256
`34e0b8cfe017a8294a104a550503624fe91963f1c996f75cbb53e40d8ca3a793`.
The VPK contained the same executable. A 60-second Vita3K smoke run completed
with 5,094 valid raw samples, exit code 0, and persistent inputs restored.
Hardware performance remains unmeasured.

Local evidence is retained under `build-perf/evidence/`: `printf-fix-artifacts.json`,
`source-printf-fix/` (exact dirty source snapshot),
`development-toolchain-versions.log`, and `build-current-printf-fix.log`.
The runtime manifest and CSV are in `build-perf/runs/public-guard-sampler-smoke-02/`.

Run the build-workflow regression checks with
`python3 scripts/vita/tests/test_build_workflow.py`. They use temporary fixtures
and mocked Docker commands, without modifying an SDK or existing build.
