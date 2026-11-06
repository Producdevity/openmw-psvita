# Vita3K memory measurement adapter

Use this isolated emulator build for provisional development comparisons. The installed public build returns no mspace statistics and returns zero usable size. Consequently, vitaGL reads uninitialized statistics, OpenMW's memory governor receives invalid pressure signals, and vitaGL's cross-pool realloc fallback can copy zero bytes. Earlier affected runs are not valid performance comparisons.

This adapter does not establish Vita hardware performance. Keep the same patched emulator binary and build configuration for every comparison.

## Source and behavior

- Emulator: [Vita3K 55239f9c745a0f8d4e7ea5994fe82147ce241ef6](https://github.com/Vita3K/Vita3K/tree/55239f9c745a0f8d4e7ea5994fe82147ce241ef6), matching the installed version.
- Allocator submodule: [Vita3K/dlmalloc e98f4ee160380d7c39dc1f04e7488bcf0770d391](https://github.com/Vita3K/dlmalloc/tree/e98f4ee160380d7c39dc1f04e7488bcf0770d391).
- ABI reference: [VitaSDK clib.h](https://github.com/vitasdk/vita-headers/blob/e66ebe90b73d1fa4cce005a5b2072cec27322544/include/psp2/kernel/clib.h).
- Patch: [0001-mspace-memory-adapter.patch](../patches/vita3k/0001-mspace-memory-adapter.patch). Original Vita3K copyright and GPL-2.0-or-later notices remain in the patched source. The existing dlmalloc implementation is unchanged.
- macOS build fix: [0002-macos-remove-nonexistent-d64-library.patch](../patches/vita3k/0002-macos-remove-nonexistent-d64-library.patch) removes the revision's invalid `-ld64` library argument. It retains the default Apple linker and both runtime search paths.

The three missing exports use the existing allocator and kernel mutex. Stats writes the 16-byte guest structure; capacity comes from `mspace_footprint`, and current use from `mspace_mallinfo().uordblks`. Capacity minus current use equals the allocator's free chunk bytes, including its metadata accounting. Usable-size returns `mspace_usable_size`.

Limitations are explicit:

- `peak_in_use` is unsupported and reported as zero, with one warning. The allocator's peak system footprint is not peak live use. OpenMW/vitaGL's free-space calculation does not use this field.
- StatsFast currently performs the same scan as Stats. No scan is added to allocation operations. This is a development adapter, not complete firmware API conformance.
- The host allocator has different metadata sizes from Vita's allocator. Its absolute usage is not hardware-equivalent. Do not use emulator pool utilization to claim hardware headroom.
- The pinned allocator disables mmap/morecore growth. It cannot expand the guest's fixed pools using host memory.

An empty 1 MiB host pool consumed 944 bytes in the test; a one-byte request consumed another 32 bytes and provided 24 usable bytes. Requested-byte counters cannot replace allocator accounting.

## Reproduce the isolated build

Run from this OpenMW checkout, using new source/build directories. These commands do not modify the configured Vita3K app, firmware or user data. On macOS, Xcode, CMake, Ninja and Homebrew are prerequisites. The tested build used arm64 and Qt 6.12; this Vita3K revision requires Qt >=6.11.

```sh
project_root=$(pwd -P)
git init build-perf/vita3k-src
git -C build-perf/vita3k-src remote add origin https://github.com/Vita3K/Vita3K.git
git -C build-perf/vita3k-src fetch --depth 1 origin 55239f9c745a0f8d4e7ea5994fe82147ce241ef6
git -C build-perf/vita3k-src checkout --detach FETCH_HEAD
git -C build-perf/vita3k-src submodule update --init --recursive --depth 1 --jobs 4
git -C build-perf/vita3k-src apply --check "$project_root/patches/vita3k/0001-mspace-memory-adapter.patch"
git -C build-perf/vita3k-src apply "$project_root/patches/vita3k/0001-mspace-memory-adapter.patch"
git -C build-perf/vita3k-src apply --check "$project_root/patches/vita3k/0002-macos-remove-nonexistent-d64-library.patch"
git -C build-perf/vita3k-src apply "$project_root/patches/vita3k/0002-macos-remove-nonexistent-d64-library.patch"

brew install qtbase qtsvg qtmultimedia qttools boost openssl
cmake -S build-perf/vita3k-src -B build-perf/vita3k-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DUSE_LTO=NEVER -DUSE_DISCORD_RICH_PRESENCE=OFF \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DVITA3K_FORCE_SYSTEM_BOOST=ON
cmake --build build-perf/vita3k-build --target vita3k module-tests mem-tests -j 4
ctest --test-dir build-perf/vita3k-build --output-on-failure
python3 scripts/vita/tests/vita3k_mspace_exports.py --source-root build-perf/vita3k-src
```

Retain the commit/submodule IDs, patch hash, CMake cache and emulator binary hash with runs. Monitor free storage before dependency downloads and compilation. Avoid performance trials while builds run.

Xcode's icon compilation needs access to its standard cache/log services. A sandbox that blocks those services can fail packaging after successful compilation; build with that access instead of altering the icon or emulator behavior.

The local arm64 build, existing CTest `mem`/`module` suites, actual-export tests and strict app-signature verification passed. The executable is `build-perf/vita3k-build/bin/Vita3K.app/Contents/MacOS/Vita3K`; build provenance is retained at `build-perf/evidence/vita3k-memory-adapter-build.json`. Homebrew OpenSSL emitted macOS 27 versus 13.3 deployment-target warnings, so this artifact is for this development host, not a portable release.

## Test coverage and runtime gate

The actual-export test extracts the patched functions and compiles them against the pinned dlmalloc source. It checks guarded 16-byte output boundaries, normal/Fast agreement, capacity/current/free conservation, malloc/calloc/alignment, realloc growth/shrink/failure and preserved contents, cross-pool copying, concurrent allocation/stat calls, and release back to initial usage. `--probe-expansion` checks an oversized allocation. `--reproduce-stub` restores the old usable-size return expression and must fail its allocation-size assertion.

ASan and UBSan are enabled with fatal errors. Only the pre-existing dlmalloc null-pointer alignment expression has its `pointer-overflow` sanitizer disabled for that translation unit; the exported functions retain all checks. Guest pointer translation and export dispatch use small host stand-ins in this test. The full build and existing module/memory tests provide separate integration checks.

Before comparisons, run a functional smoke test through [vita3k_bench.py](../scripts/vita/vita3k_bench.py) with the isolated executable. Verify the missing-import warnings disappear, each pool's free value remains within its configured total, workload state remains valid, and the adapter's unsupported-peak warning is recorded. A successful compile or host test alone does not satisfy this runtime gate. The configured installed app should remain unchanged.

A 60-second Seyda Neen smoke test completed with 6,928 valid wall-clock frame samples and a normal exit. All 66 reported VRAM-free samples stayed within the configured pool (59–60 MiB free of 88 MiB); no missing-memory-import warnings occurred, and the unsupported-peak warning was retained. Game rendering was visually inspected. This smoke included UI inspection and an idle extra emulator window, so it is functional evidence only; use exclusive repeated trials for performance comparisons.
