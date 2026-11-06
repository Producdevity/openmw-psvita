"""Metric arithmetic and captured-run matching; no emulator or game data required."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
import compare_bench as compare


def record(content, path):
    return {"path": str(path), "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()}


def write_json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.counter = 0

    def run_dir(self, name, durations=(20000, 30000), *, binary=b"binary", toggles=None, settle=5, fps=0,
                emulator=b"emulator", route=b"cell #0 0\n0 0 0 0 0 0\n10 0 0 0 0 0\n", save=b"save",
                emulator_config=b"pref-path: shared\nrenderer: Vulkan\n", asset=b"game data",
                settings=b"[Video]\nresolution x=960\n", storage=b"Lua state", log=b"diagnostics",
                resource=b"app resource", process_log=b"emulator diagnostics\n"):
        self.counter += 1
        directory = self.root / name
        directory.mkdir()
        route_name = f"vita3k-{name}.txt"
        start = str(1000000 + self.counter)
        raw_name = f"frames-{start}-0.csv"
        artifacts = {"emulator": record(emulator, self.root / "Vita3K"),
                     "executable": record(binary, self.root / "fs/ux0/app/OMWV00001/eboot.bin"),
                     "save": record(save, self.root / "fs/ux0/data/openmw/saves/character/baseline.omwsave")}
        for kind, filename, content in (("route", "route.txt", route),
                                        ("config_run", "vita3k-config.yml", emulator_config)):
            (directory / filename).write_bytes(content)
            artifacts[kind] = record(content, directory / filename)
        tree_inputs = {"Data Files": {"Morrowind.esm": asset}, "mods": {},
                       "application": {"eboot.bin": binary, "resources/ui.xml": resource},
                       "config": {"settings.cfg": settings, "player_storage.bin": storage,
                                  "MyGUI.log": log, "openmw.log": log}}
        trees = {}
        for name_, contents in tree_inputs.items():
            entries = [record(content, path) for path, content in sorted(contents.items())]
            trees[name_] = {"path": str(self.root / name_), "present": True,
                            "files": entries, "sha256": compare.tree_digest(entries)}
        write_json(directory / "game-data-manifest.json", trees)
        (directory / "game-config").mkdir()
        for filename, content in tree_inputs["config"].items():
            (directory / "game-config" / filename).write_bytes(content)
        persistent = {}
        for filename in compare.PERSISTENT_CONFIG_FILES:
            content = tree_inputs["config"].get(filename)
            state = {"present": content is not None}
            if content is not None:
                state.update({key: value for key, value in record(content, filename).items() if key != "path"})
                (directory / "persistent-inputs-after").mkdir(exist_ok=True)
                (directory / "persistent-inputs-after" / filename).write_bytes(content)
            persistent[filename] = {"before": state, "after": state, "restored": state}
        runtime = {}
        for filename, content in (toggles or {}).items():
            (directory / "runtime-toggles").mkdir(exist_ok=True)
            (directory / "runtime-toggles" / filename).write_bytes(content)
            runtime[filename] = record(content, directory / "runtime-toggles" / filename)
        scene = "no-scene-stats.txt"
        experiment = {scene: {"present": scene in runtime}}
        if scene in runtime:
            (directory / scene).write_bytes(toggles[scene])
            experiment[scene]["artifact"] = record(toggles[scene], directory / scene)
        metadata = {"schema": "openmw-vita-bench-v1", "outcome": "completed", "clock": "sceKernelGetProcessTimeWide",
                    "boundary": "benchTick-after-frame-join", "runtime": "unclassified", "build": "build-id",
                    "build_stamp": "build-stamp", "route": "ux0:data/openmw/bench/" + route_name,
                    "save_request": "baseline.omwsave", "save_path": "ux0:data/openmw/saves/character/baseline.omwsave",
                    "settle_seconds": f"{settle:.6f}", "requested_fps": f"{fps:.6f}",
                    "streaming_target": f"{fps if fps else 60:.6f}", "start_us": start,
                    "rejected_clock_samples": "0", "percentiles": "nearest-rank"}
        raw = "".join(f"# {key}={value}\n" for key, value in metadata.items())
        raw += "frame,frame_us,vblank_wait_us,residual_us\n"
        raw += "".join(f"{i},{duration},0,{duration}\n" for i, duration in enumerate(durations))
        raw = raw.encode()
        (directory / raw_name).write_bytes(raw)
        summary = {"legacy": False, "fields": {"frames": str(len(durations)), "build": "build-id",
                                               "raw": "ux0:data/openmw/bench/" + raw_name},
                   "raw_csv": record(raw, directory / raw_name)}
        manifest = {"schema": "openmw-vita3k-run-v1", "status": "completed", "platform": "vita3k",
                    "provisional": True, "hardware_target_confirmed": False, "restored_toggles": True,
                    "restored_persistent_inputs": True, "host": {"platform": "test", "machine": "arm64"},
                    "persistent_inputs": persistent,
                    "title_id": "OMWV00001", "launched_at": f"2026-11-06T00:00:{self.counter:02d}Z",
                    "parameters": {"settle_seconds": settle, "fps": fps, "timeout_seconds": 900},
                    "artifacts": artifacts,
                    "game_data": {key: {k: v for k, v in value.items() if k != "files"}
                                  for key, value in trees.items()},
                    "game_configs": {}, "runtime_toggles": runtime, "experiment_toggles": experiment,
                    "benchmark_config": f"save=baseline.omwsave\nroute={route_name}\nsettle={settle}\nfps={fps}\nexit\n"}
        process_path = directory / "process.log"
        process_path.write_bytes(process_log)
        validity = compare.scan_memory_api_log(process_path)
        validity.update(scan_complete=True, process_log=record(process_log, process_path),
                        status="missing-memory-api" if validity["missing_memory_apis"] else "no-known-missing-memory-api")
        manifest["measurement_validity"] = validity
        write_json(directory / "summary.json", summary)
        write_json(directory / "manifest.json", manifest)
        return directory

    def test_stalls_percentiles_fps_and_exact_budget_boundaries(self):
        durations = [10000, 20000, 25000, 33333, 33334, 500000, 1500000]
        a, b = self.run_dir("a", durations), self.run_dir("b", durations)
        result = compare.compare([a], [b])
        metrics = result["baseline"]["runs"][0]["metrics"]
        self.assertEqual(metrics["frames"], 7)
        self.assertEqual(metrics["elapsed_seconds"], sum(durations) / 1e6)
        self.assertAlmostEqual(metrics["fps_mean"], 7e6 / sum(durations))
        self.assertAlmostEqual(metrics["mean_ms"], sum(durations) / 7000)
        self.assertEqual(metrics["p50_ms"], 33.333)
        self.assertEqual(metrics["p95_ms"], 1500)
        self.assertEqual(metrics["p99_ms"], 1500)
        self.assertEqual(metrics["max_ms"], 1500)
        self.assertEqual(metrics["over_25_ms"], 4)
        self.assertEqual(metrics["over_33_333_ms"], 3)
        self.assertFalse(result["repeatability_minimum_met"])
        self.assertIn("Insufficient repeatability", result["warnings"][0])
        self.assertFalse(result["hardware_target_confirmed"])

    def test_medians_of_runs_and_signed_relative_change(self):
        baseline = [self.run_dir(f"a{i}", [value] * 2) for i, value in enumerate((10000, 20000, 30000))]
        candidate = [self.run_dir(f"b{i}", [value] * 2) for i, value in enumerate((8000, 16000, 24000))]
        result = compare.compare(baseline, candidate)
        self.assertTrue(result["repeatability_minimum_met"])
        self.assertEqual(result["warnings"], [])
        self.assertEqual(result["baseline"]["total_frames"], 6)
        self.assertEqual(result["baseline"]["pooled_fps_mean"], 50)
        self.assertEqual(result["baseline"]["median_of_runs"]["mean_ms"], 20)
        self.assertEqual(result["candidate"]["median_of_runs"]["mean_ms"], 16)
        self.assertAlmostEqual(result["frame_duration_change_percent"]["mean_ms"], -20)

    def test_binary_may_differ_but_application_resources_must_match(self):
        a = self.run_dir("a", binary=b"baseline")
        b = self.run_dir("b", binary=b"candidate")
        result = compare.compare([a], [b])
        self.assertNotEqual(result["baseline"]["runs"][0]["executable"], result["candidate"]["runs"][0]["executable"])
        c = self.run_dir("c", binary=b"candidate", resource=b"changed resource")
        with self.assertRaisesRegex(compare.BenchError, "Unmatched controls.*assets"):
            compare.compare([a], [c])

    def test_runtime_toggle_difference_requires_explicit_exception(self):
        a, b = self.run_dir("a"), self.run_dir("b", toggles={"no-scene-stats.txt": b""})
        with self.assertRaisesRegex(compare.BenchError, "Unmatched controls.*toggles"):
            compare.compare([a], [b])
        result = compare.compare([a], [b], {"no-scene-stats.txt"})
        self.assertEqual(result["vary_toggles"], ["no-scene-stats.txt"])
        c = self.run_dir("c", toggles={"no-scene-stats.txt": b"", "luagc.txt": b"1"})
        with self.assertRaisesRegex(compare.BenchError, "Unmatched controls.*toggles"):
            compare.compare([a], [c], {"no-scene-stats.txt"})

    def test_control_mismatches_fail(self):
        baseline = self.run_dir("a")
        variants = ({"emulator": b"new"}, {"route": b"different"}, {"save": b"different"},
                    {"emulator_config": b"renderer: OpenGL"}, {"asset": b"different"},
                    {"settings": b"resolution x=640"}, {"storage": b"different"}, {"settle": 10}, {"fps": 30})
        for index, variant in enumerate(variants):
            with self.subTest(variant=variant):
                candidate = self.run_dir(f"b{index}", **variant)
                with self.assertRaisesRegex(compare.BenchError, "Unmatched controls"):
                    compare.compare([baseline], [candidate])

    def test_only_known_config_logs_excluded_but_integrity_checked(self):
        a, b = self.run_dir("a", log=b"old"), self.run_dir("b", log=b"new log")
        compare.compare([a], [b])
        (b / "game-config/MyGUI.log").write_bytes(b"tampered")
        with self.assertRaisesRegex(compare.BenchError, "changed captured artifact"):
            compare.compare([a], [b])

    def test_mixed_binaries_or_experiment_states_within_group_rejected(self):
        a, b, c = self.run_dir("a"), self.run_dir("b", binary=b"changed"), self.run_dir("c")
        with self.assertRaisesRegex(compare.BenchError, "Each group"):
            compare.compare([a, b], [c])
        d = self.run_dir("d", toggles={"no-scene-stats.txt": b""})
        with self.assertRaisesRegex(compare.BenchError, "Each group"):
            compare.compare([a, d], [c], {"no-scene-stats.txt"})

    def test_failed_legacy_unrestored_and_missing_samples_rejected(self):
        a = self.run_dir("a")
        for index, changes in enumerate(({"status": "failed"}, {"status": "legacy_summary_only"},
                                         {"restored_toggles": False}, {"restored_persistent_inputs": False})):
            b = self.run_dir(f"b{index}")
            manifest = compare.read_json(b / "manifest.json")
            manifest.update(changes)
            write_json(b / "manifest.json", manifest)
            with self.assertRaisesRegex(compare.BenchError, "Requires a completed"):
                compare.compare([a], [b])
        c = self.run_dir("c")
        next(c.glob("frames-*.csv")).unlink()
        with self.assertRaisesRegex(compare.BenchError, "Missing or changed"):
            compare.compare([a], [c])

    def test_raw_hash_and_producer_identity_rejected_if_stale(self):
        a, b = self.run_dir("a"), self.run_dir("b")
        raw = next(b.glob("frames-*.csv"))
        raw.write_text(raw.read_text().replace("vita3k-b.txt", "vita3k-older.txt"))
        with self.assertRaisesRegex(compare.BenchError, "changed captured artifact"):
            compare.compare([a], [b])
        summary = compare.read_json(b / "summary.json")
        summary["raw_csv"] = record(raw.read_bytes(), raw)
        write_json(b / "summary.json", summary)
        with self.assertRaisesRegex(compare.BenchError, "producer identity"):
            compare.compare([a], [b])

    def test_asset_manifest_tampering_and_duplicate_run_rejected(self):
        a, b = self.run_dir("a"), self.run_dir("b")
        with self.assertRaisesRegex(compare.BenchError, "Duplicate run"):
            compare.compare([a], [a])
        trees = compare.read_json(b / "game-data-manifest.json")
        trees["Data Files"]["files"][0]["bytes"] += 1
        write_json(b / "game-data-manifest.json", trees)
        with self.assertRaisesRegex(compare.BenchError, "Asset manifest hash"):
            compare.compare([a], [b])

    def test_nearest_rank_for_even_run_count(self):
        self.assertEqual(compare.metrics([10000, 20000, 30000, 40000])["p50_ms"], 20)

    def test_persistent_restoration_claim_must_match_evidence(self):
        a, b = self.run_dir("a"), self.run_dir("b")
        manifest = compare.read_json(b / "manifest.json")
        manifest["persistent_inputs"]["player_storage.bin"]["restored"]["sha256"] = "a" * 64
        write_json(b / "manifest.json", manifest)
        with self.assertRaisesRegex(compare.BenchError, "restoration mismatch"):
            compare.compare([a], [b])

    def test_partial_failed_csv_cannot_be_compared_as_completed(self):
        a, b = self.run_dir("a"), self.run_dir("b")
        raw = next(b.glob("frames-*.csv"))
        raw.write_text(raw.read_text().replace("# outcome=completed", "# outcome=failed"))
        summary = compare.read_json(b / "summary.json")
        summary["raw_csv"] = record(raw.read_bytes(), raw)
        write_json(b / "summary.json", summary)
        with self.assertRaises(compare.BenchError):
            compare.compare([a], [b])

    def test_unimplemented_memory_apis_reject_even_completed_functional_runs(self):
        a = self.run_dir("baseline")
        for name in ("sceClibMspaceMallocStats", "sceClibMspaceMallocStatsFast", "sceClibMspaceMallocUsableSize"):
            with self.subTest(api=name):
                b = self.run_dir(name, process_log=f"Unimplemented {name} import called.\n".encode())
                with self.assertRaisesRegex(compare.BenchError, "Unimplemented memory APIs"):
                    compare.compare([a], [b])

    def test_unclassified_or_incomplete_runs_are_rejected(self):
        a = self.run_dir("baseline")
        for name in ("missing", "wrong-schema", "incomplete"):
            with self.subTest(case=name):
                b = self.run_dir(name)
                manifest = compare.read_json(b / "manifest.json")
                if name == "missing":
                    manifest.pop("measurement_validity")
                elif name == "wrong-schema":
                    manifest["measurement_validity"]["schema"] = "unclassified"
                else:
                    manifest["measurement_validity"]["scan_complete"] = False
                write_json(b / "manifest.json", manifest)
                with self.assertRaisesRegex(compare.BenchError, "memory API validation"):
                    compare.compare([a], [b])

    def test_process_log_hash_and_classification_are_both_checked(self):
        a, b = self.run_dir("a"), self.run_dir("b")
        log = b / "process.log"
        log.write_text("Unimplemented sceClibMspaceMallocStats import called.\n")
        with self.assertRaisesRegex(compare.BenchError, "changed captured artifact"):
            compare.compare([a], [b])
        manifest = compare.read_json(b / "manifest.json")
        manifest["measurement_validity"]["process_log"] = record(log.read_bytes(), log)
        write_json(b / "manifest.json", manifest)
        with self.assertRaisesRegex(compare.BenchError, "Unimplemented memory APIs"):
            compare.compare([a], [b])

    def test_matching_partial_peak_support_is_reported_and_allowed(self):
        warning = b"Mspace stats: capacity/current_in_use use allocator accounting; peak_in_use is unsupported and reported as zero\n"
        a = self.run_dir("a", process_log=warning)
        b = self.run_dir("b", process_log=warning)
        result = compare.compare([a], [b])
        self.assertEqual(result["matched_controls"]["memory_api_limitations"], ["peak_in_use_unavailable"])
        c = self.run_dir("c")
        with self.assertRaisesRegex(compare.BenchError, "memory_api_limitations"):
            compare.compare([a], [c])


if __name__ == "__main__":
    unittest.main()
