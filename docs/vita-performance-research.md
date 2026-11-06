# Vita performance investigation

Start with reproducible builds and wall-clock frame measurements, then optimize measured costs. No hardware FPS improvement has been established by this investigation. The historical approximately 24 FPS figure is a user-reported reference, not a measured baseline for current master. Only actual Vita measurements can establish the 40 FPS target.

## Source boundaries, 2026-11-06

| Source | Revision |
| --- | --- |
| Updated Vita fork | `a2660718ab84094e6a1d4f3247fc70a2b9204fc4` |
| Common ancestor with official OpenMW | `a13f8f33942756403f81cfe1a9c170ad5af4134d` |
| Official OpenMW master reviewed | `f2f1f457477a4779456083a0d15c625d92dce72b` |
| PortMaster engine branch | `bmdhacks/openmw@04a6b5e7bf0dcaf6c3dc431f7a0bc3b5783c79cb` |
| PortMaster package | `PortsMaster/PortMaster-New@b5fe1628d47adad6d33358e902507175d8ea1883` |
| Public vitaGL upstream | `e119b3ec158067725659a25b650ebb8f537d1e8f` |

The official history contains 1,940 commits after the common ancestor, including 1,449 non-merge commits. Its changes overlap 145 files changed by the Vita fork. The renderer, ownership, lighting and Lua architecture have diverged substantially; individual backports require Vita-specific review. [Official source](https://gitlab.com/OpenMW/openmw/-/tree/f2f1f457477a4779456083a0d15c625d92dce72b), [Vita source](https://github.com/Producdevity/openmw-psvita/tree/a2660718ab84094e6a1d4f3247fc70a2b9204fc4).


## Upstream intake

| Change | Decision and required evidence |
| --- | --- |
| [Allocation-free Lua iteration](https://gitlab.com/OpenMW/openmw/-/commit/2cc7085015e85403e79c9aa38adc4852f72771b4) | Port the wrapper and semantic tests. The current wrapper allocates an empty table for ordinary `pairs`/`ipairs` calls. Measure actual Lua allocation/GC and whole-frame effects. |
| [Avoid default custom state](https://gitlab.com/OpenMW/openmw/-/commit/0c0f12745aaaf74957e9bd96e57062878a1aba1b) | Candidate for memory/save growth after resting, recharging and respawning. Test modified references, moved actors, levelled lists, containers, eviction and save/reload before adopting. |
| [Weak Lua userdata cache](https://gitlab.com/OpenMW/openmw/-/commit/dbe2d5a0fd8d2da80f750deee88897df2593426e) | Profile wrapper allocation first. Vita evicts CellStores; use stable cell IDs, not upstream's raw cell-pointer keys. Validate local/global identity, GC and reload lifetimes. |
| [Predictive terrain preload](https://gitlab.com/OpenMW/openmw/-/commit/2c8dfb878ab53ff56b0ed1fae76595307e28efd6) | Verify the path remains active alongside Vita streaming. Measure queue churn, peak memory and transition tails. |
| [Parallel Lua GC](https://gitlab.com/OpenMW/openmw/-/commit/ec26c52123006221e9ab869152c3db55598748a6) | Defer until core/subsystem measurements justify it. The fork defaults to zero Lua threads. Include the [menu-race follow-up](https://gitlab.com/OpenMW/openmw/-/commit/d7c2f416aea03c8ee68876f90fecf7d985c8cbf6); simulation/cull overlap changes the safe work window. |
| [Shared light StateSets](https://gitlab.com/OpenMW/openmw/-/commit/d2a5e0799d1290cbf117fa3973207ae6f3c73010) | Current Vita already caches light lists. Check its multi-view correctness and allocation costs before adapting a different implementation. |
| [Clustered lighting](https://gitlab.com/OpenMW/openmw/-/commit/d7be10259ee31a98fcc2c93467fc4d42d06c5154) | A renderer migration with compute/SSBO dependencies, not a compatible small backport. Do not merge wholesale. |

The Lua iteration mechanism was independently checked with host LuaJIT, JIT disabled, 500,000 ordinary-table iterations and eight alternating runs. Median CPU time was 24.965 ms before and 16.843 ms after; allocation with GC stopped was 31,250 KiB before and zero after. Checksums matched, and ordinary/custom/hidden-metatable and readonly-wrapper semantics passed. These figures describe an artificial host workload, **not** OpenMW FPS or a Vita gain.

Upstream profiler-decay and navigation optimizations currently offer little relevant work to remove: Vita already disables Lua instruction profiling and navigation. Large sound caches also need an explicit Vita memory budget.

## PortMaster findings

The current [package build instructions](https://github.com/PortsMaster/PortMaster-New/blob/b5fe1628d47adad6d33358e902507175d8ea1883/ports/openmw/openmw/BUILDING.md) select `bmdhacks/openmw:portmaster-latest`. Its changes include render scaling, compressed-texture lookup and controller input; the reviewed package provides no controlled before/after frame-time dataset.

- The [asset converter](https://github.com/PortsMaster/PortMaster-New/blob/b5fe1628d47adad6d33358e902507175d8ea1883/ports/openmw/openmw/esmm/utils/compressor.txt) preprocesses ASTC/KTX. VitaGL supports native DXT/UBC instead. Reuse the offline-conversion approach only if loading or memory measurements justify it; preserve source assets and mod precedence.
- The [launcher](https://github.com/PortsMaster/PortMaster-New/blob/b5fe1628d47adad6d33358e902507175d8ea1883/ports/openmw/Morrowind.sh) defaults runtime recompression and texture shrinking off. Its GL4ES environment variables do not apply to VitaGL.
- The [base settings](https://github.com/PortsMaster/PortMaster-New/blob/b5fe1628d47adad6d33358e902507175d8ea1883/ports/openmw/openmw/openmw/settings.base.cfg) use resolution scale 1.0 despite an older README claim of 0.5. Vita already disables expensive rendering features and navigation, keeps DXT compressed, and limits background workers. Copying these settings would mostly duplicate existing behavior or reduce quality.
- MOP/Atlas effects must be measured separately from engine changes with identical asset manifests. Do not count reduced draw distance, resolution or texture detail as equivalent-quality engine improvements.

## Public graphics dependency

The old private vitaGL pin is unavailable. The user's public fork had no unique commits and was 100 commits behind upstream. The sibling checkout now starts from the public upstream revision above on `perf/openmw-compat`.

The missing `vglSetStaticVboRam` API is reconstructible from a [published OpenMW patch](https://github.com/Producdevity/openmw-psvita/commit/103aa6a064). Public compressed-upload code also lacks transfer completion before storage reuse and minimum block dimensions for GPU mip uploads. Focused fixes build as a complete ARM library; six actual-source host regression cases pass under ASan/UBSan in staged/direct modes. These tests validate transfer lifetime and branch decisions, not GPU pixels or device performance.

Public standard API signatures remain compatible, with `VGL_MEM_SLOW` renamed `VGL_MEM_PHYCONT`. Private GPU counters have weak zero fallbacks in OpenMW; zero means unavailable until a strong implementation is verified. The tested Vita3K revision (`55239f9c745a0f8d4e7ea5994fe82147ce241ef6`) leaves both `sceClibMspaceMallocStats` and `sceClibMspaceMallocStatsFast` unimplemented. Public vitaGL reads the untouched statistics structure, and OpenMW consumes those invalid free-pool values in its memory governor. `sceClibMspaceMallocUsableSize` is also unimplemented, returning zero; vitaGL uses that size when copying a reallocation across pools. Emulator memory totals and workloads affected by these paths are not reliable until the missing APIs are addressed.

Treat the public-dependency build as a new baseline. The released private-dependency VPK is useful for a gameplay smoke test but cannot isolate engine improvements from dependency changes.

## Measurement order

Follow [the benchmark protocol](vita-benchmarking.md). First capture the current public-dependency build at fixed quality, explicit save/route and uncapped headroom. Quantify instrumentation overhead, including the camera's extra scene-statistics traversal. Then measure one change at a time across exterior, city, interior, combat and NPC-heavy scenarios. Preserve failed trials, raw frame times, artifact/settings/data hashes and visual comparisons.

Vita3K provides functional and relative development evidence. GPU submission time is not GPU execution time; host CPU utilization is not Vita core utilization. Hardware runs with recorded clocks, memory and the same scenarios remain necessary.
