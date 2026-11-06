# Vita benchmark evidence

Use real PS Vita measurements to establish the hardware baseline and confirm the 40 FPS target. Vita3K measurements are provisional relative comparisons. The reported historical 24 FPS is not a measured baseline for the current source revision.

## Timing

Replay samples `sceKernelGetProcessTimeWide()` at the same main-thread boundary after the previous simulation/cull join. The first boundary starts the window. Subsequent boundary differences include pacing, waits, loading stalls and instrumentation overhead. No frame is clipped to the simulation's 200 ms timestep limit or scaled by simulation time.

The result line reports mean FPS as frames divided by measured elapsed time, frame duration mean/median/p90/p95/p99/maximum, and hitch counts. Percentiles use nearest rank: sorted sample `ceil(p*N)`, numbered from one. `residual` subtracts only the previous recorded vblank wait, clamped at zero. Frame-limiter sleeps and other waits remain included. It is residual wall time, not CPU time or isolated GPU cost; overlapping work can cross these boundaries.

After the measured window, each completed run writes `ux0:data/openmw/bench/frames-<start>-<suffix>.csv`. Existing files are preserved. Its comment header identifies the build, route, selected save, warmup, requested cap, streaming target and clock. Data rows retain integer microseconds for every measured frame and recorded vblank wait. Output failure produces a failed benchmark rather than a completed result. Clock anomalies are counted; reject runs with nonzero `clock_rejected` pending investigation.

An ended game or GUI mode after placement aborts replay. Available partial samples have `outcome=failed`; completed samples have `outcome=completed`. The host runner retains partial CSV evidence but rejects it as a successful trial. Record route coordinates after normal physics settling: repeatedly forcing a door's above-floor position can accumulate falling distance and kill the player.

The CSV records `runtime=unclassified`: a successful run does not prove physical hardware execution. Collect a run manifest alongside it containing:

- `vita3k` or `hardware`, device/emulator version, host and renderer where relevant;
- executable, route, save, settings and game/mod data hashes;
- actual CPU/GPU/bus clocks on hardware, resolution, view distance, texture settings and active experiment toggles;
- requested frame cap and effective streaming budget, plus warmup and scenario;
- errors, missing metrics, screenshots and visible regressions.

Current `fps=<n>` also changes streaming target (`fps=0` selects 60). Keep this coupling identical across a comparison until the controls are separated. Use `fps=0` for headroom tests: the current pacing implementation rounds a requested 40 FPS to two 60 Hz vblanks, nominally 30 FPS. A negative target in CSV means the benchmark did not override it; record the effective settings in the manifest. GPU audit counters may be weak zero fallbacks: verify instrumentation availability before interpreting zero as no GPU work.

## Runs

Select an explicit save with `save=<name.omwsave>` and a fixed route with `route=<name.txt>` in `ux0:data/openmw/bench.txt`. Avoid newest-save selection for comparisons. Use `settle=<seconds>` to exclude initial placement/warmup deliberately. Keep weather, time, actors, mods and visual settings fixed. Do not open menus during a timed run. Use separate routes for each interior/exterior scenario: the current wall-clock replay can skip an intermediate cell after a long stall, so crossing-route workload equivalence must be checked. Use separate scenarios for outdoor traversal, a dense city, interiors, combat and an NPC-heavy scene. Preserve initial saves and user toggles.

Run at least three alternating baseline/candidate trials under identical conditions. Retain raw data and failed trials. Compare frame-time distributions and 25 ms/33.3 ms budget misses; an average alone cannot establish stable 40 FPS or locked 30 FPS. Label cell transitions and loading stalls rather than removing them. Run screenshots separately from timed trials. Compare diagnostic instrumentation enabled/disabled to quantify observer overhead.

The Vita3K runner hashes assets before launch, warming the host filesystem cache. Its loading measurements are not cold-storage measurements. Guest memory faults abort a run, and process-log output is limited to 64 MiB; any discarded polling overshoot is recorded in the manifest.

The runner records versioned `measurement_validity` evidence from the fresh process log through child shutdown and hashes the full retained log. An `Unimplemented ... import called` warning for `sceClibMspaceMallocStats`, `sceClibMspaceMallocStatsFast`, or `sceClibMspaceMallocUsableSize` leaves a successful functional smoke test completed, but excludes its timings from performance comparison: missing memory accounting can change allocator decisions. `compare_bench.py` requires the classification, verifies the log hash, and rescans the captured log. Older unclassified runs need a new capture; they cannot be promoted by assuming the missing API did not matter.

An adapter that reports valid capacity/current usage but explicitly lacks `peak_in_use` is recorded as `peak_in_use_unavailable`; comparisons with the same limitation are allowed. Do not interpret the unsupported zero as a measured peak. No known missing-API warning is not proof of full emulator conformance, and every result remains provisional until real hardware testing.

## Scene-statistics overhead experiment

Compare the same executable with `ux0:data/openmw/no-scene-stats.txt` absent and present. Restart between trials; the toggle is read once when frame statistics initialize, and `[SceneStats] on/off` records the chosen state. Preserve any existing file and include its presence/content hash in the run manifest. The default remains on.

This disables only the camera's OSG `scene` statistics, which walk visible leaves and primitive sets every cull. Rendering and engine timers, raw frame samples, and game settings remain enabled. Ignore visible drawable/triangle/bin counts in `[Scene]` while disabled; they are unavailable. The OSG cull timer ends before scene-statistics collection, so use raw frame distributions and whole-worker cull timing to assess this experiment. Run alternating trials in a fixed dense scene and verify matching screenshots. No speedup is established until those trials complete.

## Vita3K runner

Use an existing configured installation and an explicit save basename that occurs in exactly one character folder:

```sh
python3 scripts/vita/vita3k_bench.py \
  --emulator /path/to/Vita3K --config /path/to/config.yml \
  --pref-path "$HOME/Library/Application Support/Vita3K/Vita3K/fs" \
  --route /path/to/static-route.txt --save baseline.omwsave \
  --output build-perf/evidence/vita3k-trial-001 --settle 5 --timeout 900
```

The output directory must not exist. The runner copies the YAML, changes its `pref-path`, selects the SDL frontend, and requests uncapped replay (`fps=0`). It restores prior `bench.txt` and `benchrec.txt` contents and removes its unique route after success, failure, timeout or a handled interruption. A filesystem lock excludes another runner using the same installation. Only its own emulator child is terminated.

Each trial also restores persistent game inputs: `config/player_storage.bin`, `global_storage.bin`, existing `*.cfg` plus `settings.cfg`, `shaders.yaml`, and `input_v2.xml`, `input_v3.xml` and its pending `.new` file. Files absent initially are removed if the game creates them. The manifest records before, after and restored hashes; post-run copies remain in the evidence directory. Logs and caches are preserved as outputs. This requires exclusive access to the installation during the trial, including no concurrent configuration edits.

Evidence includes executable/emulator, settings, save, route and game-data hashes; the `Data Files`, `mods`, `config` and installed application trees; preserved game configuration and root text toggles; scene-statistics toggle state; process output; and whole-file game-log snapshots, which can include older runs. Hashing finishes before the emulator timeout starts. A completed current-format run requires a fresh CSV matching the route, save and uncapped request. Results remain explicitly provisional Vita3K measurements. `--allow-legacy-summary` permits an older release's summary for diagnostics and labels its potentially clipped timings unusable for target validation.

Runner tests use fake executables and temporary files:

```sh
python3 -m unittest discover -s scripts/vita/tests -p 'test_vita3k_bench.py'
```

## Portable regression test

From the repository root:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror -I. \
  scripts/vita/tests/bench_timing_test.cpp -o /tmp/openmw-bench-timing-test
/tmp/openmw-bench-timing-test
```

This checks raw-clock intervals, 500 ms and 1.5 s stalls, rejected timestamps, pacing subtraction, frame-rate arithmetic and percentile boundaries. It does not validate Vita compilation, replay behavior, rendering, or hardware performance. Those require the cross-build and runtime scenario checks.
