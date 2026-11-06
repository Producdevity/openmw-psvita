"""Filesystem/process tests using tiny fake emulators, never Vita3K itself."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock

MODULE = Path(__file__).resolve().parents[1] / "vita3k_bench.py"
SPEC = importlib.util.spec_from_file_location("vita3k_bench", MODULE)
bench = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bench
SPEC.loader.exec_module(bench)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.pref = self.root / "fs"
        self.data = self.pref / "ux0/data/openmw"
        self.bench_dir = self.data / "bench"
        self.bench_dir.mkdir(parents=True)
        self.save = self.data / "saves/character/baseline.omwsave"
        self.save.parent.mkdir(parents=True)
        self.save.write_bytes(b"original save\x00")
        app = self.pref / "ux0/app/OMWV00001"
        app.mkdir(parents=True)
        (app / "eboot.bin").write_bytes(b"installed executable")
        assets = self.data / "Data Files"
        assets.mkdir()
        (assets / "Morrowind.esm").write_bytes(b"test game data")
        (self.data / "config").mkdir()
        (self.data / "config/openmw.cfg").write_text("content=Morrowind.esm\n")
        (self.data / "config/settings.cfg").write_text("[Video]\nresolution x=960\n")
        (self.data / "config/res640.txt").write_text("on\n")
        (self.data / "config/player_storage.bin").write_bytes(b"initial player storage")
        (self.data / "luagc.txt").write_text("existing-gc-option\n")
        self.config = self.root / "config.yml"
        self.config.write_text("pref-path: old-path\naudio-volume: 0\n")
        self.route = self.root / "route.txt"
        self.route.write_text("cell #0 0\n0 0 0 0 0 0\n1 0 0 0 0 0\n")
        self.emulator = self.root / "fake-emulator"
        self.set_emulator("success")
        self.args = argparse.Namespace(emulator=str(self.emulator), config=str(self.config),
            pref_path=str(self.pref), route=str(self.route), save=self.save.name,
            output=str(self.root / "result"), timeout=3.0, settle=0.0, allow_legacy_summary=False)

    def set_emulator(self, mode):
        source = "#!" + sys.executable + "\n" + textwrap.dedent('''
            import json, os, pathlib, sys, time
            mode = MODE
            assert sys.argv[1:5] == ["--frontend", "sdl", "--keep-config", "--config-location"]
            assert sys.argv[6:] == ["-r", "OMWV00001"]
            config = pathlib.Path(sys.argv[5]).read_text()
            pref = pathlib.Path(json.loads(next(x.split(": ", 1)[1] for x in config.splitlines() if x.startswith("pref-path:"))))
            data = pref / "ux0/data/openmw"
            (data / "boot.log").write_text("current-run diagnostic\\n")
            config = data / "config"
            (config / "player_storage.bin").write_bytes(b"mutated player storage")
            (config / "global_storage.bin").write_bytes(b"new global storage")
            (config / "settings.cfg").write_text("changed settings\\n")
            (config / "input_v3.xml").write_text("new bindings\\n")
            (config / "input_v3.xml.new").unlink(missing_ok=True)
            (config / "shaders.yaml").write_text("new shaders\\n")
            (config / "openmw.log").write_text("new engine log\\n")
            (config / "MyGUI.log").write_text("new gui log\\n")
            (config / "scan_cache.bin").write_bytes(b"new cache")
            with (data / "bench/results.txt").open("a") as stream:
                stream.write("current-run diagnostic\\n")
            values = dict(x.split("=", 1) for x in (data / "bench.txt").read_text().splitlines() if "=" in x)
            assert values["fps"] == "0"
            assert not (data / "benchrec.txt").exists()
            assert (data / "bench" / values["route"]).read_bytes()
            (data / "child.pid").write_text(str(os.getpid()))
            if mode == "memory-fault":
                print("[13:45:33.465] |E| [log_invalid_access]: Invalid read at 0x80 by thread 103 (pc 0x8276a918)", flush=True)
                os.write(sys.stdout.fileno(), b"repeated emulator diagnostic\\n" * 4096)
                time.sleep(60)
            if mode == "log-flood":
                os.ftruncate(sys.stdout.fileno(), 64 * 1024 * 1024 + 4096)
                time.sleep(60)
            if mode == "timeout":
                time.sleep(60)
            if mode == "no-result":
                sys.exit(0)
            if mode == "missing-memory-api":
                print("Unimplemented sceClibMspaceMallocStats import called.", flush=True)
            if mode == "partial-memory-api":
                print("Mspace stats: capacity/current_in_use use allocator accounting; peak_in_use is unsupported and reported as zero", flush=True)
            if mode == "legacy":
                print("[Bench] done build=test frames=2 avg=20.000ms", flush=True)
            else:
                raw = data / "bench/frames-fresh.csv"
                outcome = "failed" if mode.startswith("failed") or mode == "done-failed-outcome" else "completed"
                metadata = ("# schema=openmw-vita-bench-v1\\n# outcome=" + outcome + "\\n# rejected_clock_samples=0\\n"
                    + "# route=ux0:data/openmw/bench/" + values["route"] + "\\n# save_request=" + values["save"]
                    + "\\n# requested_fps=0.000000\\n")
                if mode == "done-missing-outcome":
                    metadata = metadata.replace("# outcome=completed\\n", "")
                raw.write_text(metadata + "frame,frame_us,vblank_wait_us,residual_us\\n0,20000,1000,19000\\n1,30000,0,30000\\n")
                reference = "ux0:data/openmw/bench/frames-fresh.csv"
                if mode in ("bad-path", "failed-bad-path"):
                    reference = "ux0:data/openmw/bench/../saves/secret.csv"
                if mode == "wrong-route":
                    raw.write_text(raw.read_text().replace(values["route"], "different-route.txt"))
                if mode == "failed-output-exists":
                    pathlib.Path("frames-fresh.csv").write_bytes(b"do not overwrite")
                if mode.startswith("failed"):
                    suffix = "" if mode == "failed-no-partial" else " partial_raw=" + reference
                    print("[Bench] failed: menu during timed window" + suffix, flush=True)
                    time.sleep(60)
                else:
                    print("[Bench] done build=test frames=2 avg=25.000ms raw=" + reference, flush=True)
                if mode == "fault-after-result":
                    time.sleep(0.2)
                    print("[log_invalid_access]: Invalid write after result", flush=True)
                    time.sleep(60)
                if mode == "missing-api-after-result":
                    time.sleep(0.2)
                    sys.stdout.write("Unimplemented sceClibMspaceMallocUsableSize import called.")
                    sys.stdout.flush()
            if mode == "bad-exit":
                sys.exit(3)
            ''').replace("mode = MODE", "mode = " + repr(mode))
        self.emulator.write_text(source)
        self.emulator.chmod(0o700)

    def read_manifest(self):
        return json.loads((Path(self.args.output) / "manifest.json").read_text())

    def original_toggles(self):
        original = {"bench.txt": b"user benchmark\x00\n", "benchrec.txt": b"record original\n"}
        for name, content in original.items():
            path = self.data / name
            path.write_bytes(content)
            path.chmod(0o640)
            os.utime(path, ns=(1000000000, 2000000000))
        return original

    def assert_restored(self, originals=None):
        for name in ("bench.txt", "benchrec.txt"):
            path = self.data / name
            if originals is None:
                self.assertFalse(path.exists())
            else:
                self.assertEqual(path.read_bytes(), originals[name])
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o640)
                self.assertEqual(path.stat().st_mtime_ns, 2000000000)
        self.assertFalse(list(self.bench_dir.glob("vita3k-*.txt")))
        self.assertEqual(self.save.read_bytes(), b"original save\x00")

    def test_success_restores_existing_files_and_hashes_artifacts(self):
        originals = self.original_toggles()
        existing_route = self.bench_dir / "my-route.txt"
        existing_route.write_bytes(b"keep this route")
        original_config = self.config.read_bytes()
        result = bench.run_benchmark(self.args)
        self.assertEqual(result["status"], "completed")
        self.assertTrue(result["provisional"])
        self.assertFalse(result["hardware_target_confirmed"])
        self.assertEqual(result["platform"], "vita3k")
        self.assertEqual(result["process_log_limit_bytes"], 64 * 1024 * 1024)
        self.assertTrue(result["restored_toggles"])
        self.assertLessEqual(result["started_at"], result["ended_at"])
        self.assert_restored(originals)
        self.assertEqual(existing_route.read_bytes(), b"keep this route")
        self.assertEqual(self.config.read_bytes(), original_config)
        for value in result["artifacts"].values():
            self.assertEqual(value["sha256"], hashlib.sha256(Path(value["path"]).read_bytes()).hexdigest())
        summary = json.loads((Path(self.args.output) / "summary.json").read_text())
        self.assertEqual(summary["measured"]["fps_mean"], 40.0)
        self.assertEqual(summary["measured"]["elapsed_seconds"], 0.05)
        self.assertEqual(summary["raw_csv"]["sha256"], bench.sha256(Path(summary["raw_csv"]["path"])))
        trees = json.loads((Path(self.args.output) / "game-data-manifest.json").read_text())
        self.assertEqual(trees["Data Files"]["files"][0]["sha256"], hashlib.sha256(b"test game data").hexdigest())
        self.assertIn("application", trees)
        self.assertTrue(trees["config"]["present"])
        self.assertEqual({item["path"] for item in trees["config"]["files"]},
                         {"openmw.cfg", "settings.cfg", "res640.txt", "player_storage.bin"})
        for name in ("openmw.cfg", "settings.cfg", "res640.txt"):
            self.assertEqual((Path(self.args.output) / "game-config" / name).read_bytes(),
                             (self.data / "config" / name).read_bytes())
        self.assertEqual(result["runtime_toggles"]["luagc.txt"]["sha256"],
                         bench.sha256(self.data / "luagc.txt"))
        self.assertNotIn("bench.txt", result["runtime_toggles"])
        self.assert_persistent_inputs_restored()
        self.assertTrue(result["restored_persistent_inputs"])
        inputs = result["persistent_inputs"]
        self.assertNotEqual(inputs["player_storage.bin"]["before"], inputs["player_storage.bin"]["after"])
        self.assertEqual(inputs["player_storage.bin"]["before"], inputs["player_storage.bin"]["restored"])
        self.assertFalse(inputs["global_storage.bin"]["before"]["present"])
        self.assertTrue(inputs["global_storage.bin"]["after"]["present"])
        self.assertFalse(inputs["global_storage.bin"]["restored"]["present"])
        self.assertEqual((Path(self.args.output) / "persistent-inputs-after/player_storage.bin").read_bytes(),
                         b"mutated player storage")
        self.assertEqual((self.data / "config/openmw.log").read_text(), "new engine log\n")
        self.assertEqual((self.data / "config/scan_cache.bin").read_bytes(), b"new cache")
        self.assertEqual((Path(self.args.output) / "game-boot.log").read_text(), "current-run diagnostic\n")
        self.assertIn("may contain older runs", result["game_log_snapshots"]["results.txt"]["scope"])
        self.assertEqual(summary["measured"]["metadata"]["route"],
                         "ux0:data/openmw/bench/" + result["benchmark_config"].split("route=", 1)[1].splitlines()[0])

    def test_absent_toggles_are_absent_after_success(self):
        result = bench.run_benchmark(self.args)
        self.assertFalse(result["experiment_toggles"]["no-scene-stats.txt"]["present"])
        self.assert_restored()

    def assert_persistent_inputs_restored(self):
        self.assertEqual((self.data / "config/player_storage.bin").read_bytes(), b"initial player storage")
        self.assertEqual((self.data / "config/settings.cfg").read_text(), "[Video]\nresolution x=960\n")
        for name in ("global_storage.bin", "input_v3.xml", "shaders.yaml"):
            self.assertFalse((self.data / "config" / name).exists())

    def test_scene_stats_toggle_preserved_and_recorded(self):
        toggle = self.data / "no-scene-stats.txt"
        toggle.write_bytes(b"experiment A\n")
        result = bench.run_benchmark(self.args)
        evidence = result["experiment_toggles"][toggle.name]
        self.assertTrue(evidence["present"])
        self.assertEqual(evidence["artifact"]["sha256"], bench.sha256(toggle))
        self.assertEqual(toggle.read_bytes(), b"experiment A\n")

    def test_pending_bindings_restored_and_created_storage_removed(self):
        (self.data / "config/player_storage.bin").unlink()
        pending = self.data / "config/input_v3.xml.new"
        pending.write_bytes(b"original pending bindings")
        pending.chmod(0o640)
        os.utime(pending, ns=(1000000000, 2000000000))
        result = bench.run_benchmark(self.args)
        self.assertFalse((self.data / "config/player_storage.bin").exists())
        self.assertFalse((self.data / "config/input_v3.xml").exists())
        self.assertEqual(pending.read_bytes(), b"original pending bindings")
        self.assertEqual(stat.S_IMODE(pending.stat().st_mode), 0o640)
        self.assertEqual(pending.stat().st_mtime_ns, 2000000000)
        self.assertTrue(result["restored_persistent_inputs"])

    def test_post_run_input_copy_failure_still_restores_inputs(self):
        original_copy = bench.shutil.copyfile
        def failing_copy(source, destination, *args, **kwargs):
            if Path(destination).parent.name == "persistent-inputs-after":
                raise OSError("intentional persistent input copy failure")
            return original_copy(source, destination, *args, **kwargs)
        with mock.patch.object(bench.shutil, "copyfile", side_effect=failing_copy):
            with self.assertRaisesRegex(bench.BenchError, "persistent input copy failure"):
                bench.run_benchmark(self.args)
        self.assert_persistent_inputs_restored()
        self.assert_restored()
        self.assertTrue(self.read_manifest()["restored_persistent_inputs"])
        self.assertEqual(self.read_manifest()["status"], "failed")

    def test_failed_launch_restores_files(self):
        originals = self.original_toggles()
        def failed_launch(*args, **kwargs):
            raise OSError("intentional launch failure")
        with self.assertRaisesRegex(OSError, "intentional"):
            bench.run_benchmark(self.args, popen=failed_launch)
        self.assert_restored(originals)
        self.assertEqual(self.read_manifest()["status"], "failed")
        self.assertTrue(self.read_manifest()["restored_toggles"])

    def test_interrupt_restores_files(self):
        originals = self.original_toggles()
        def interrupted(*args, **kwargs):
            raise KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            bench.run_benchmark(self.args, popen=interrupted)
        self.assert_restored(originals)

    def test_timeout_terminates_child_and_restores_files(self):
        originals = self.original_toggles()
        self.set_emulator("timeout")
        self.args.timeout = 0.3
        with self.assertRaisesRegex(bench.BenchError, "timed out"):
            bench.run_benchmark(self.args)
        pid = int((self.data / "child.pid").read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)
        self.assert_restored(originals)
        self.assertEqual(self.read_manifest()["status"], "timeout")
        self.assert_persistent_inputs_restored()
        self.assertEqual((Path(self.args.output) / "game-boot.log").read_text(), "current-run diagnostic\n")

    def test_sigterm_with_running_child_restores_files(self):
        originals = self.original_toggles()
        self.set_emulator("timeout")
        command = [sys.executable, str(MODULE)]
        for name in ("emulator", "config", "pref_path", "route", "save", "output", "timeout", "settle"):
            command.extend(["--" + name.replace("_", "-"), str(getattr(self.args, name))])
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(bench.stop_child, process)
        deadline = time.monotonic() + 3
        pid_file = self.data / "child.pid"
        while not pid_file.exists() and time.monotonic() < deadline and process.poll() is None:
            time.sleep(0.02)
        self.assertTrue(pid_file.exists(), "fake emulator did not start")
        child_pid = int(pid_file.read_text())
        process.terminate()
        _, error = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 130, error.decode())
        with self.assertRaises(ProcessLookupError):
            os.kill(child_pid, 0)
        self.assert_restored(originals)
        self.assertTrue(self.read_manifest()["restored_toggles"])
        self.assert_persistent_inputs_restored()

    def test_guest_memory_fault_aborts_child_and_restores_inputs(self):
        originals = self.original_toggles()
        self.set_emulator("memory-fault")
        self.args.timeout = 10
        start = time.monotonic()
        with self.assertRaisesRegex(bench.BenchError, "guest memory fault"):
            bench.run_benchmark(self.args)
        self.assertLess(time.monotonic() - start, 3)
        with self.assertRaises(ProcessLookupError):
            os.kill(int((self.data / "child.pid").read_text()), 0)
        manifest = self.read_manifest()
        self.assertEqual(manifest["status"], "failed")
        self.assertIn("pc 0x8276a918", manifest["fatal_process_log"])
        self.assertTrue((Path(self.args.output) / "process.log").is_file())
        self.assert_restored(originals)
        self.assert_persistent_inputs_restored()

    def test_log_flood_aborts_child_caps_retained_log_and_restores_inputs(self):
        originals = self.original_toggles()
        self.set_emulator("log-flood")
        self.args.timeout = 10
        start = time.monotonic()
        with self.assertRaisesRegex(bench.BenchError, "64 MiB limit"):
            bench.run_benchmark(self.args)
        self.assertLess(time.monotonic() - start, 3)
        with self.assertRaises(ProcessLookupError):
            os.kill(int((self.data / "child.pid").read_text()), 0)
        result = self.read_manifest()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["process_log_truncated"])
        self.assertGreater(result["process_log_bytes_before_truncation"], bench.PROCESS_LOG_LIMIT_BYTES)
        self.assertEqual((Path(self.args.output) / "process.log").stat().st_size, bench.PROCESS_LOG_LIMIT_BYTES)
        self.assert_restored(originals)
        self.assert_persistent_inputs_restored()

    def test_fault_during_shutdown_invalidates_completed_result(self):
        self.set_emulator("fault-after-result")
        with self.assertRaisesRegex(bench.BenchError, "guest memory fault"):
            bench.run_benchmark(self.args)
        self.assertEqual(self.read_manifest()["status"], "failed")
        self.assert_restored()
        self.assert_persistent_inputs_restored()

    def test_process_log_limit_checked_before_reading_contents(self):
        path = self.root / "oversized-process.log"
        with path.open("wb") as stream:
            stream.truncate(bench.PROCESS_LOG_LIMIT_BYTES + 1)
        reader = bench.FreshLog(path)
        with mock.patch.object(reader, "lines") as read:
            with self.assertRaisesRegex(bench.BenchError, "64 MiB limit"):
                bench.check_process_log(reader, {})
        read.assert_not_called()

    def test_fault_marker_spanning_bounded_reads_is_detected(self):
        path = self.root / "split-process.log"
        path.write_bytes(b"")
        reader = bench.FreshLog(path)
        path.write_bytes(b"x" * (bench.PROCESS_LOG_READ_BYTES - 8) + b"[log_invalid_access]: split fault")
        self.assertEqual(bench.check_process_log(reader, {}), [])
        self.assertTrue(reader.has_unread())
        evidence = {}
        with self.assertRaisesRegex(bench.BenchError, "guest memory fault"):
            bench.check_process_log(reader, evidence)
        self.assertIn("[log_invalid_access]", evidence["fatal_process_log"])
        self.assertLess(len(evidence["fatal_process_log"]), 1100)

    def test_no_warning_run_records_complete_hashed_memory_api_evidence(self):
        result = bench.run_benchmark(self.args)
        validity = result["measurement_validity"]
        self.assertEqual(validity["schema"], bench.MEMORY_VALIDATION_SCHEMA)
        self.assertTrue(validity["scan_complete"])
        self.assertEqual(validity["status"], "no-known-missing-memory-api")
        self.assertEqual(validity["missing_memory_apis"], [])
        self.assertEqual(validity["process_log"], bench.artifact(Path(self.args.output).resolve() / "process.log"))

    def test_missing_memory_api_keeps_functional_result_but_marks_measurements(self):
        self.set_emulator("missing-memory-api")
        result = bench.run_benchmark(self.args)
        self.assertEqual(result["status"], "completed")
        validity = result["measurement_validity"]
        self.assertEqual(validity["status"], "missing-memory-api")
        self.assertEqual(validity["missing_memory_apis"], ["sceClibMspaceMallocStats"])
        self.assertTrue(validity["scan_complete"])
        self.assert_restored()
        self.assert_persistent_inputs_restored()

    def test_shutdown_warning_without_newline_is_classified(self):
        self.set_emulator("missing-api-after-result")
        result = bench.run_benchmark(self.args)
        self.assertEqual(result["status"], "completed")
        validity = result["measurement_validity"]
        self.assertEqual(validity["status"], "missing-memory-api")
        self.assertEqual(validity["missing_memory_apis"], ["sceClibMspaceMallocUsableSize"])
        self.assertEqual(validity["process_log"], bench.artifact(Path(self.args.output).resolve() / "process.log"))

    def test_memory_api_markers_split_across_reads_are_detected(self):
        path = self.root / "split-memory-api.log"
        path.write_bytes(b"")
        reader = bench.FreshLog(path)
        apis = ["sceClibMspaceMallocStats", "sceClibMspaceMallocStatsFast", "sceClibMspaceMallocUsableSize"]
        path.write_text("\n".join(f"Unimplemented {api} import called." for api in apis))
        evidence = {}
        with mock.patch.object(bench, "PROCESS_LOG_READ_BYTES", 17):
            while reader.has_unread():
                bench.check_process_log(reader, evidence)
            captured = bench.scan_memory_api_log(path)
        self.assertEqual(evidence["measurement_validity"]["missing_memory_apis"], sorted(apis))
        self.assertEqual(captured["missing_memory_apis"], sorted(apis))
        self.assertEqual(len(captured["evidence"]), 3)

    def test_partial_peak_metric_is_recorded_without_missing_current_accounting(self):
        self.set_emulator("partial-memory-api")
        result = bench.run_benchmark(self.args)
        validity = result["measurement_validity"]
        self.assertEqual(validity["status"], "no-known-missing-memory-api")
        self.assertEqual(validity["limitations"], ["peak_in_use_unavailable"])
        self.assertEqual(validity["missing_memory_apis"], [])

    def test_log_copy_failure_does_not_skip_toggle_restoration(self):
        originals = self.original_toggles()
        original_copy = bench.shutil.copyfile
        def failing_copy(source, destination, *args, **kwargs):
            if Path(source).name == "boot.log":
                raise OSError("intentional log copy failure")
            return original_copy(source, destination, *args, **kwargs)
        with mock.patch.object(bench.shutil, "copyfile", side_effect=failing_copy):
            with self.assertRaisesRegex(bench.BenchError, "intentional log copy failure"):
                bench.run_benchmark(self.args)
        self.assert_restored(originals)
        self.assertEqual(self.read_manifest()["status"], "failed")
        self.assertTrue(self.read_manifest()["restored_toggles"])

    def test_stale_done_is_not_accepted(self):
        self.set_emulator("no-result")
        (self.data / "boot.log").write_text("[Bench] done build=old frames=2\n")
        (self.bench_dir / "results.txt").write_text("build=old frames=2\n")
        self.args.allow_legacy_summary = True
        with self.assertRaisesRegex(bench.BenchError, "without a fresh"):
            bench.run_benchmark(self.args)
        self.assert_restored()

    def test_legacy_requires_explicit_opt_in(self):
        self.set_emulator("legacy")
        with self.assertRaisesRegex(bench.BenchError, "no raw CSV"):
            bench.run_benchmark(self.args)
        self.assert_restored()

    def test_legacy_is_labeled_unusable_for_target(self):
        self.set_emulator("legacy")
        self.args.allow_legacy_summary = True
        result = bench.run_benchmark(self.args)
        self.assertEqual(result["status"], "legacy_summary_only")
        self.assertIn("unusable for target", result["limitation"])
        self.assertTrue(result["provisional"])
        self.assert_restored()

    def test_rejects_raw_path_escape(self):
        self.set_emulator("bad-path")
        with self.assertRaisesRegex(bench.BenchError, "single filename"):
            bench.run_benchmark(self.args)
        self.assert_restored()

    def test_rejects_raw_path_variants_and_symlink(self):
        for value in ("/tmp/frames-x.csv", "ux0:data/openmw/bench/./frames-x.csv",
                      "ux0:data/openmw/bench//frames-x.csv", "ux0:data/openmw/bench/frames-x.csv/",
                      "ux0:data/openmw/bench/../frames-x.csv", "ux0:data/openmw/bench/frames-\\x.csv"):
            with self.subTest(value=value), self.assertRaises(bench.BenchError):
                bench.raw_csv_path(value, self.bench_dir)
        (self.bench_dir / "frames-link.csv").symlink_to(self.save)
        with self.assertRaises(bench.BenchError):
            bench.raw_csv_path("ux0:data/openmw/bench/frames-link.csv", self.bench_dir)

    def test_rejects_existing_raw_artifact(self):
        (self.bench_dir / "frames-fresh.csv").write_text("old")
        with self.assertRaisesRegex(bench.BenchError, "fresh raw"):
            bench.run_benchmark(self.args)
        self.assert_restored()

    def test_rejects_wrong_route_metadata(self):
        self.set_emulator("wrong-route")
        with self.assertRaisesRegex(bench.BenchError, "does not match"):
            bench.run_benchmark(self.args)

    def test_failed_benchmark_retains_partial_and_stops_child_immediately(self):
        originals = self.original_toggles()
        self.set_emulator("failed-partial")
        self.args.timeout = 10
        start = time.monotonic()
        with self.assertRaisesRegex(bench.BenchError, "menu during timed window"):
            bench.run_benchmark(self.args)
        self.assertLess(time.monotonic() - start, 3)
        result = self.read_manifest()
        self.assertEqual(result["status"], "failed")
        self.assertIn("menu during timed window", result["benchmark_failure"])
        partial = result["partial_raw_csv"]
        self.assertEqual(partial["sha256"], bench.sha256(Path(partial["path"])))
        self.assertIn("# outcome=failed", Path(partial["path"]).read_text())
        self.assertFalse((Path(self.args.output) / "summary.json").exists())
        with self.assertRaises(ProcessLookupError):
            os.kill(int((self.data / "child.pid").read_text()), 0)
        self.assert_restored(originals)
        self.assert_persistent_inputs_restored()
        self.assertTrue((Path(self.args.output) / "game-boot.log").is_file())

    def test_failed_benchmark_without_partial_does_not_wait_for_timeout(self):
        self.set_emulator("failed-no-partial")
        start = time.monotonic()
        with self.assertRaisesRegex(bench.BenchError, "Benchmark reported failure"):
            bench.run_benchmark(self.args)
        self.assertLess(time.monotonic() - start, 2)
        self.assertNotIn("partial_raw_csv", self.read_manifest())
        self.assert_restored()

    def test_failed_partial_escape_is_rejected_without_copying_outside_file(self):
        self.set_emulator("failed-bad-path")
        outside = self.data / "saves/secret.csv"
        outside.write_bytes(b"private outside file")
        with self.assertRaisesRegex(bench.BenchError, "partial CSV rejected"):
            bench.run_benchmark(self.args)
        self.assertEqual(outside.read_bytes(), b"private outside file")
        self.assertFalse(list(Path(self.args.output).glob("*.csv")))
        self.assertNotIn("partial_raw_csv", self.read_manifest())
        self.assert_restored()

    def test_failed_partial_must_be_fresh(self):
        self.set_emulator("failed-partial")
        (self.bench_dir / "frames-fresh.csv").write_bytes(b"old raw file")
        with self.assertRaisesRegex(bench.BenchError, "fresh raw CSV"):
            bench.run_benchmark(self.args)
        self.assertFalse(list(Path(self.args.output).glob("*.csv")))
        self.assert_restored()

    def test_failed_partial_does_not_overwrite_existing_output_artifact(self):
        self.set_emulator("failed-output-exists")
        with self.assertRaisesRegex(bench.BenchError, "partial CSV rejected"):
            bench.run_benchmark(self.args)
        self.assertEqual((Path(self.args.output) / "frames-fresh.csv").read_bytes(), b"do not overwrite")
        self.assert_restored()

    def test_completed_result_rejects_failed_csv_outcome(self):
        self.set_emulator("done-failed-outcome")
        with self.assertRaisesRegex(bench.BenchError, "completed benchmark"):
            bench.run_benchmark(self.args)
        self.assertEqual(self.read_manifest()["status"], "failed")
        self.assert_restored()

    def test_completed_result_rejects_missing_csv_outcome(self):
        self.set_emulator("done-missing-outcome")
        with self.assertRaisesRegex(bench.BenchError, "completed benchmark"):
            bench.run_benchmark(self.args)
        self.assert_restored()

    def test_rejects_crash_after_result(self):
        self.set_emulator("bad-exit")
        with self.assertRaisesRegex(bench.BenchError, "unsuccessfully"):
            bench.run_benchmark(self.args)
        self.assertEqual(self.read_manifest()["status"], "failed")
        self.assert_restored()

    def test_ambiguous_save_is_rejected_without_changes(self):
        duplicate = self.data / "saves/another" / self.save.name
        duplicate.parent.mkdir()
        duplicate.write_bytes(b"another save")
        with self.assertRaisesRegex(bench.BenchError, "found 2"):
            bench.run_benchmark(self.args)
        self.assertFalse(Path(self.args.output).exists())
        self.assert_restored()

    def test_save_directory_escape_is_rejected(self):
        self.args.save = "../baseline.omwsave"
        with self.assertRaisesRegex(bench.BenchError, "basename"):
            bench.run_benchmark(self.args)

    def test_existing_output_is_never_overwritten(self):
        output = Path(self.args.output)
        output.mkdir()
        (output / "keep").write_text("keep")
        with self.assertRaisesRegex(bench.BenchError, "already exists"):
            bench.run_benchmark(self.args)
        self.assertEqual((output / "keep").read_text(), "keep")
        self.assert_restored()

    def test_lock_prevents_concurrent_benchmarks(self):
        with bench.ExclusiveLock(self.pref / ".openmw-vita3k-bench.lock"):
            with self.assertRaisesRegex(bench.BenchError, "Another benchmark"):
                bench.run_benchmark(self.args)
        self.assertFalse(Path(self.args.output).exists())

    def test_nonfinite_and_negative_values_rejected(self):
        for field, value in (("timeout", 0), ("timeout", -1), ("timeout", float("nan")),
                             ("timeout", float("inf")), ("settle", -1), ("settle", float("nan"))):
            with self.subTest(field=field, value=value):
                args = argparse.Namespace(**vars(self.args))
                setattr(args, field, value)
                with self.assertRaises(bench.BenchError):
                    bench.run_benchmark(args)
        self.assertFalse(Path(self.args.output).exists())

    def test_data_hash_changes_when_asset_changes(self):
        first = bench.hash_tree(self.data / "Data Files")["sha256"]
        (self.data / "Data Files/Morrowind.esm").write_bytes(b"changed")
        self.assertNotEqual(first, bench.hash_tree(self.data / "Data Files")["sha256"])

    def test_fresh_log_handles_truncation_and_partial_lines(self):
        path = self.root / "log"
        path.write_text("old result\n")
        reader = bench.FreshLog(path)
        self.assertEqual(reader.lines(), [])
        path.write_text("new")
        self.assertEqual(reader.lines(), [])
        with path.open("a") as stream:
            stream.write(" result\n")
        self.assertEqual(reader.lines(), ["new result"])

    def test_duplicate_summary_fields_keep_frame_statistics(self):
        self.assertEqual(bench.summary_fields("avg=40ms residual avg=20") ["avg"], "40ms")


if __name__ == "__main__":
    unittest.main()
