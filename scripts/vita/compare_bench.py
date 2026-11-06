#!/usr/bin/env python3
"""Compare matched completed Vita3K runs; print provisional metrics as JSON.

Example: compare_bench.py --baseline runs/a1 runs/a2 runs/a3 \
    --candidate runs/b1 runs/b2 runs/b3 --vary-toggle no-scene-stats.txt
Only captured evidence is read. Original installation paths may have changed.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import statistics
import sys

from vita3k_bench import (BenchError, MEMORY_VALIDATION_SCHEMA, MEMORY_VALIDATION_SCOPE,
                         PERSISTENT_CONFIG_FILES, read_samples, scan_memory_api_log, sha256)

CONFIG_OUTPUTS = {"MyGUI.log", "openmw.log"}


def fingerprint(value: dict) -> dict:
    digest, size = value["sha256"], value["bytes"]
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise BenchError("Invalid artifact SHA256")
    if type(size) is not int or size < 0:
        raise BenchError("Invalid artifact size")
    return {"sha256": digest, "bytes": size}


def verify_file(path: Path, value: dict) -> dict:
    expected = fingerprint(value)
    if not path.is_file() or path.stat().st_size != expected["bytes"] or sha256(path) != expected["sha256"]:
        raise BenchError(f"Missing or changed captured artifact: {path}")
    return expected


def local_name(value: str) -> str:
    path = PurePosixPath(value)
    if not value or value != path.name or "\\" in value or value in (".", ".."):
        raise BenchError(f"Expected a single filename: {value!r}")
    return value


def tree_digest(entries: list) -> str:
    data = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(data).hexdigest()


def validate_tree(tree: dict, recorded: dict) -> list:
    entries = tree["files"]
    names = []
    for entry in entries:
        name = entry["path"]
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or not path.parts or "\\" in name:
            raise BenchError("Invalid asset manifest path")
        if set(entry) != {"path", "bytes", "sha256"}:
            raise BenchError("Unexpected asset manifest fields")
        fingerprint(entry)
        names.append(name)
    if names != sorted(set(names)) or tree_digest(entries) != tree["sha256"]:
        raise BenchError("Asset manifest hash or ordering mismatch")
    if (type(tree["present"]) is not bool or (not tree["present"] and entries)
            or tree["sha256"] != recorded["sha256"] or tree["present"] != recorded["present"]):
        raise BenchError("Asset manifest does not match the run manifest")
    return entries


def metrics(durations: list[int]) -> dict:
    ordered = sorted(durations)
    count, total = len(ordered), sum(ordered)
    result = {"frames": count, "elapsed_seconds": total / 1e6, "fps_mean": count * 1e6 / total,
              "mean_ms": total / count / 1000, "max_ms": ordered[-1] / 1000}
    for name, percentile in (("p50_ms", 50), ("p95_ms", 95), ("p99_ms", 99)):
        result[name] = ordered[(percentile * count + 99) // 100 - 1] / 1000
    for name, threshold in (("25_ms", 25000), ("33_333_ms", 33333)):
        misses = sum(value > threshold for value in durations)
        result["over_" + name] = misses
        result["over_" + name + "_percent"] = 100 * misses / count
    return result


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def load_run(directory: Path, vary: set[str]) -> dict:
    directory = directory.expanduser().resolve()
    try:
        manifest = read_json(directory / "manifest.json")
        summary = read_json(directory / "summary.json")
        if (manifest["schema"] != "openmw-vita3k-run-v1" or manifest["status"] != "completed"
                or manifest["platform"] != "vita3k" or manifest["provisional"] is not True
                or manifest["hardware_target_confirmed"] is not False
                or manifest["restored_toggles"] is not True
                or manifest.get("restored_persistent_inputs") is not True or summary["legacy"] is not False):
            raise BenchError("Requires a completed, restored Vita3K run with raw samples")
        validity = manifest.get("measurement_validity", {})
        if (validity.get("schema") != MEMORY_VALIDATION_SCHEMA
                or validity.get("scope") != MEMORY_VALIDATION_SCOPE or validity.get("scan_complete") is not True
                or manifest.get("process_log_truncated", False)):
            raise BenchError("Missing or incomplete memory API validation; capture a new classified run")
        verify_file(directory / "process.log", validity["process_log"])
        observed = scan_memory_api_log(directory / "process.log")
        if observed["missing_memory_apis"]:
            raise BenchError("Unimplemented memory APIs invalidate performance comparison: "
                             + ", ".join(observed["missing_memory_apis"]))
        if (validity.get("status") != "no-known-missing-memory-api"
                or any(validity.get(key) != observed[key] for key in ("missing_memory_apis", "limitations", "evidence"))):
            raise BenchError("Memory API validation does not match captured process log")
        artifacts = manifest["artifacts"]
        controls = {name: fingerprint(artifacts[name]) for name in ("emulator", "save")}
        controls["memory_api_limitations"] = validity["limitations"]
        controls["route"] = verify_file(directory / "route.txt", artifacts["route"])
        controls["config_run"] = verify_file(directory / "vita3k-config.yml", artifacts["config_run"])
        executable = fingerprint(artifacts["executable"])
        controls["host"] = manifest["host"]
        controls["title_id"] = manifest["title_id"]
        parameters = manifest["parameters"]
        controls["parameters"] = {key: parameters[key] for key in ("settle_seconds", "fps")}
        for value in controls["parameters"].values():
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise BenchError("Invalid warmup or frame cap")

        trees = read_json(directory / "game-data-manifest.json")
        if not {"Data Files", "mods", "application", "config"} <= trees.keys():
            raise BenchError("Missing asset/configuration manifests")
        if trees.keys() != manifest["game_data"].keys():
            raise BenchError("Asset manifest tree mismatch")
        controls["assets"] = {}
        for name, tree in trees.items():
            entries = validate_tree(tree, manifest["game_data"][name])
            if name == "application":
                binaries = [entry for entry in entries if entry["path"] == "eboot.bin"]
                if len(binaries) != 1 or fingerprint(binaries[0]) != executable:
                    raise BenchError("Application executable identity mismatch")
                entries = [entry for entry in entries if entry["path"] != "eboot.bin"]
            elif name == "config":
                for entry in entries:
                    verify_file(directory / "game-config" / entry["path"], entry)
                entries = [entry for entry in entries if entry["path"] not in CONFIG_OUTPUTS]
            controls["assets"][name] = {"present": tree["present"], "sha256": tree_digest(entries)}
        config_files = {entry["path"]: entry for entry in trees["config"]["files"]}
        persistent = manifest["persistent_inputs"]
        required_inputs = set(PERSISTENT_CONFIG_FILES) | {name for name in config_files if name.endswith(".cfg")}
        if not required_inputs <= persistent.keys():
            raise BenchError("Missing persistent-input restoration evidence")
        for name, states in persistent.items():
            local_name(name)
            before = states["before"]
            if before != states["restored"] or before["present"] != (name in config_files):
                raise BenchError("Persistent-input restoration mismatch")
            if before["present"] and fingerprint(before) != fingerprint(config_files[name]):
                raise BenchError("Persistent input does not match captured configuration")
            after = states["after"]
            after_path = directory / "persistent-inputs-after" / name
            if after["present"]:
                verify_file(after_path, after)
            elif after_path.exists():
                raise BenchError("Unexpected stale post-run input snapshot")
        controls["root_configs"] = {name: verify_file(directory / "root-config" / local_name(name), value)
                                     for name, value in manifest["game_configs"].items()}
        toggles = {name: verify_file(directory / "runtime-toggles" / local_name(name), value)
                   for name, value in manifest["runtime_toggles"].items()}
        for name, experiment in manifest["experiment_toggles"].items():
            if experiment["present"] != (name in toggles):
                raise BenchError("Experiment/runtime toggle presence mismatch")
            if experiment["present"]:
                evidence = verify_file(directory / local_name(name), experiment["artifact"])
                if evidence != toggles[name]:
                    raise BenchError("Experiment/runtime toggle hash mismatch")
        controls["toggles"] = {name: value for name, value in toggles.items() if name not in vary}

        raw_name = local_name(Path(summary["raw_csv"]["path"]).name)
        raw = directory / raw_name
        verify_file(raw, summary["raw_csv"])
        fields = summary["fields"]
        if fields["raw"] not in ("ux0:data/openmw/bench/" + raw_name, "ux0:/data/openmw/bench/" + raw_name):
            raise BenchError("Summary/raw artifact path mismatch")
        measured = read_samples(raw, int(fields["frames"]))
        metadata = measured["metadata"]
        config = dict(line.split("=", 1) for line in manifest["benchmark_config"].splitlines() if "=" in line)
        expected_route = "ux0:data/openmw/bench/" + local_name(config["route"])
        if (metadata.get("outcome") != "completed"
                or metadata["route"] != expected_route or metadata["save_request"] != config["save"]
                or config["save"] != Path(artifacts["save"]["path"]).name
                or metadata["build"] != fields["build"]
                or metadata["clock"] != "sceKernelGetProcessTimeWide"
                or metadata["boundary"] != "benchTick-after-frame-join"):
            raise BenchError("Raw CSV producer identity does not match this run")
        for metadata_key, config_key, parameter in (("settle_seconds", "settle", "settle_seconds"),
                                                   ("requested_fps", "fps", "fps")):
            values = (float(metadata[metadata_key]), float(config[config_key]), parameters[parameter])
            if not all(math.isfinite(value) and math.isclose(value, values[0], rel_tol=1e-5, abs_tol=1e-6)
                       for value in values):
                raise BenchError("Raw CSV warmup/cap does not match this run")
        streaming_target = float(metadata["streaming_target"])
        if not math.isfinite(streaming_target) or streaming_target <= 0:
            raise BenchError("Invalid streaming target")
        controls["streaming_target"] = streaming_target
        with raw.open() as source:
            rows = csv.DictReader(line for line in source if not line.startswith("#"))
            durations = [int(row["frame_us"]) for row in rows]
        return {"directory": str(directory), "executable": executable, "metrics": metrics(durations),
                "measurement_validity": validity,
                "raw_sha256": summary["raw_csv"]["sha256"], "controls": controls,
                "varied_toggles": {name: toggles.get(name) for name in sorted(vary)},
                "restored_changed_inputs": sorted(name for name, state in persistent.items()
                                                  if state["before"] != state["after"]),
                "run_identity": (manifest["launched_at"], expected_route, metadata["start_us"]),
                "forced_shutdown_after_result": bool(manifest.get("forced_shutdown_after_result", False))}
    except (OSError, KeyError, TypeError, ValueError, BenchError) as error:
        raise BenchError(f"{directory}: {error}") from error


def summarize_group(runs: list[dict]) -> dict:
    medians = {key: statistics.median(run["metrics"][key] for run in runs) for key in runs[0]["metrics"]}
    count = sum(run["metrics"]["frames"] for run in runs)
    elapsed = sum(run["metrics"]["elapsed_seconds"] for run in runs)
    per_run = [{key: value for key, value in run.items() if key not in ("controls", "run_identity")}
               for run in runs]
    return {"runs": per_run, "run_count": len(runs), "total_frames": count, "total_seconds": elapsed,
            "pooled_fps_mean": count / elapsed, "median_of_runs": medians}


def compare(baseline: list[Path], candidate: list[Path], vary: set[str] | None = None) -> dict:
    vary = vary or set()
    for name in vary:
        if not local_name(name).endswith(".txt") or name in ("bench.txt", "benchrec.txt"):
            raise BenchError("--vary-toggle must name a root runtime .txt toggle")
    if not baseline or not candidate:
        raise BenchError("Both groups require at least one run")
    groups = [[load_run(path, vary) for path in paths] for paths in (baseline, candidate)]
    runs = groups[0] + groups[1]
    if len({run["directory"] for run in runs}) != len(runs) or len({run["run_identity"] for run in runs}) != len(runs):
        raise BenchError("Duplicate run evidence cannot count as independent repetitions")
    reference = runs[0]["controls"]
    for run in runs[1:]:
        changed = sorted(key for key in reference if reference[key] != run["controls"][key])
        if changed:
            raise BenchError(f"Unmatched controls in {run['directory']}: {', '.join(changed)}")
    for group in groups:
        for run in group[1:]:
            if run["executable"] != group[0]["executable"] or run["varied_toggles"] != group[0]["varied_toggles"]:
                raise BenchError("Each group must have one executable and consistent experiment toggles")
    warnings = []
    if any(len(group) < 3 for group in groups):
        warnings.append("Insufficient repeatability: fewer than three runs in at least one group; diagnostic comparison only.")
    if any(run["forced_shutdown_after_result"] for run in runs):
        warnings.append("Some emulator processes required forced shutdown after writing their completed result.")
    a, b = (summarize_group(group) for group in groups)
    changes = {key: 100 * (b["median_of_runs"][key] / a["median_of_runs"][key] - 1)
               for key in ("mean_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms")}
    return {"schema": "openmw-vita3k-comparison-v1", "platform": "vita3k", "provisional": True,
            "hardware_target_confirmed": False, "warnings": warnings,
            "repeatability_minimum_met": all(len(group) >= 3 for group in groups),
            "vary_toggles": sorted(vary), "matched_controls": reference, "baseline": a, "candidate": b,
            "excluded_config_outputs": sorted(CONFIG_OUTPUTS),
            "frame_duration_change_percent": changes, "change_sign": "negative is faster; positive is slower",
            "percentiles": "nearest-rank within each run; group summaries are medians of run metrics",
            "budget_miss_rule": "strictly greater than 25000us / 33333us; no frames removed or clipped",
            "limitation": "Relative emulator evidence only; repeated runs do not establish real Vita performance or statistical significance."}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", nargs="+", type=Path, required=True)
    parser.add_argument("--candidate", nargs="+", type=Path, required=True)
    parser.add_argument("--vary-toggle", action="append", default=[])
    args = parser.parse_args(argv)
    try:
        result = compare(args.baseline, args.candidate, set(args.vary_toggle))
    except (BenchError, OSError, ValueError) as error:
        print(f"Comparison failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
