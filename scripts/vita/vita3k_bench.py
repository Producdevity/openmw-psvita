#!/usr/bin/env python3
"""Run an existing OpenMW Vita3K installation; results remain provisional.

The supplied YAML is copied and only its top-level pref-path is replaced.
No firmware setup or package installation is performed. Game-data hashing may
read several GiB before launch. The selected save is never copied or changed.
"""
from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
import hashlib
import io
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import signal
import stat
import subprocess
import sys
import time
import uuid


TITLE = "OMWV00001"
PROCESS_LOG_LIMIT_BYTES = 64 * 1024 * 1024
PROCESS_LOG_READ_BYTES = 1024 * 1024
PERSISTENT_CONFIG_FILES = (
    "global_storage.bin", "player_storage.bin", "settings.cfg", "shaders.yaml",
    "input_v2.xml", "input_v3.xml", "input_v3.xml.new",
)


class BenchError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def artifact(path: Path) -> dict:
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": sha256(path)}


def file_state(path: Path) -> dict:
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise BenchError(f"Persistent input must be a regular file: {path}")
    if not path.exists():
        return {"present": False}
    return {"present": True, "bytes": path.stat().st_size, "sha256": sha256(path)}


def write_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def resolve_save(saves: Path, name: str) -> Path:
    if Path(name).name != name or not name.endswith(".omwsave") or any(c in name for c in "\\\r\n"):
        raise BenchError("--save must be an existing .omwsave basename, without directories")
    matches = sorted(path for path in saves.glob("*/*") if path.name == name and path.is_file())
    if len(matches) != 1:
        raise BenchError(f"Save {name!r} must match exactly one character folder; found {len(matches)}")
    result = matches[0].resolve()
    if not inside(result, saves.resolve()):
        raise BenchError("Selected save resolves outside the saves directory")
    return result


def hash_tree(root: Path) -> dict:
    entries = []
    if root.is_symlink() or (root.exists() and not root.is_dir()):
        raise BenchError(f"Data hashing requires a directory without a symlink: {root}")
    if root.exists():
        for directory, folders, files in os.walk(root, followlinks=False):
            for name in sorted(folders + files):
                path = Path(directory) / name
                if path.is_symlink():
                    raise BenchError(f"Data hashing refuses symlink: {path}")
            for name in sorted(files):
                path = Path(directory) / name
                if not path.is_file():
                    raise BenchError(f"Data hashing requires regular files: {path}")
                entries.append({"path": path.relative_to(root).as_posix(),
                                "bytes": path.stat().st_size, "sha256": sha256(path)})
    entries.sort(key=lambda value: value["path"])
    encoded = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode()
    return {"path": str(root), "present": root.exists(), "files": entries,
            "sha256": hashlib.sha256(encoded).hexdigest()}


def private_config(source: Path, destination: Path, pref_path: Path) -> None:
    original = source.read_text(encoding="utf-8")
    replacement = "pref-path: " + json.dumps(str(pref_path))
    copied, count = re.subn(r"^pref-path:[^\r\n]*", lambda _: replacement, original, flags=re.MULTILINE)
    if count != 1:
        raise BenchError("Existing config must contain exactly one top-level pref-path YAML key")
    destination.write_text(copied, encoding="utf-8")


class ExclusiveLock:
    def __init__(self, path: Path):
        self.path = path
        self.handle = None

    def __enter__(self):
        if self.path.is_symlink():
            raise BenchError("Benchmark lock must not be a symlink")
        self.handle = self.path.open("a+b")
        try:
            fcntl.flock(self.handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            self.handle.close()
            self.handle = None
            raise BenchError("Another benchmark owns this Vita3K filesystem") from error
        return self

    def __exit__(self, *_):
        fcntl.flock(self.handle, fcntl.LOCK_UN)
        self.handle.close()
        # Keep the inode: unlinking a lock file allows two independent owners.


@dataclass
class SavedFile:
    path: Path
    content: bytes | None
    mode: int = 0o600
    atime_ns: int = 0
    mtime_ns: int = 0

    @classmethod
    def capture(cls, path: Path):
        if path.is_symlink() or (path.exists() and not path.is_file()):
            raise BenchError(f"Toggle must be a regular file: {path}")
        if not path.exists():
            return cls(path, None)
        info = path.stat()
        return cls(path, path.read_bytes(), stat.S_IMODE(info.st_mode), info.st_atime_ns, info.st_mtime_ns)

    def restore(self):
        if self.content is None:
            self.path.unlink(missing_ok=True)
            return
        temporary = self.path.with_name(self.path.name + ".restore-" + uuid.uuid4().hex)
        try:
            temporary.write_bytes(self.content)
            temporary.chmod(self.mode)
            os.utime(temporary, ns=(self.atime_ns, self.mtime_ns))
            temporary.replace(self.path)
        finally:
            temporary.unlink(missing_ok=True)


class FreshLog:
    def __init__(self, path: Path):
        self.path = path
        self.position = path.stat().st_size if path.is_file() else 0
        self.prefix = self._prefix()
        self.pending = ""

    def _prefix(self):
        if not self.path.is_file():
            return b""
        with self.path.open("rb") as source:
            return source.read(256)

    def lines(self, max_bytes=None):
        if not self.path.is_file():
            return []
        prefix = self._prefix()
        if self.path.stat().st_size < self.position or not prefix.startswith(self.prefix):
            self.position = 0
            self.pending = ""
        self.prefix = prefix
        with self.path.open("rb") as source:
            source.seek(self.position)
            chunk = source.read(-1 if max_bytes is None else max_bytes)
            self.position = source.tell()
        text = self.pending + chunk.decode("utf-8", errors="replace")
        lines = text.split("\n")
        self.pending = lines.pop()
        return lines

    def has_unread(self):
        return self.path.is_file() and self.path.stat().st_size > self.position


def check_process_log(reader: FreshLog, manifest: dict) -> list[str]:
    size = reader.path.stat().st_size
    if size > PROCESS_LOG_LIMIT_BYTES:
        manifest["process_log_limit_exceeded_bytes"] = size
        raise BenchError("Emulator process log exceeded the 64 MiB limit")
    lines = reader.lines(max_bytes=PROCESS_LOG_READ_BYTES)
    for line in (*lines, reader.pending):
        if "[log_invalid_access]" in line:
            index = line.index("[log_invalid_access]")
            evidence = line[max(0, index - 64):index + 1024]
            manifest["fatal_process_log"] = evidence
            raise BenchError(f"Emulator reported a guest memory fault: {evidence}")
    return lines


def raw_csv_path(value: str, bench: Path) -> Path:
    prefixes = ("ux0:data/openmw/bench/", "ux0:/data/openmw/bench/")
    prefix = next((candidate for candidate in prefixes if value.startswith(candidate)), None)
    if prefix is None:
        raise BenchError("Result raw path is outside the benchmark directory")
    suffix = value[len(prefix):]
    relative = PurePosixPath(suffix)
    if suffix != relative.name or "\\" in suffix:
        raise BenchError("Result raw path must be a single filename")
    if len(relative.parts) != 1 or not relative.name.startswith("frames-") or relative.suffix != ".csv":
        raise BenchError("Result raw path must be a frames-*.csv directly in bench/")
    path = bench / relative.name
    if path.is_symlink() or path.resolve().parent != bench.resolve():
        raise BenchError("Result raw path escapes the benchmark directory")
    return path


def copy_fresh_raw(reference: str, bench: Path, output: Path, existing_names: set[str]) -> Path:
    source = raw_csv_path(reference, bench)
    if source.name in existing_names or not source.is_file():
        raise BenchError("Result does not reference a fresh raw CSV")
    destination = output / source.name
    with source.open("rb") as stream, destination.open("xb") as copy:
        shutil.copyfileobj(stream, copy)
    return destination


def read_samples(path: Path, expected_frames: int) -> dict:
    text = path.read_text(encoding="utf-8")
    metadata = {}
    data = []
    for line in text.splitlines():
        if line.startswith("# "):
            key, _, value = line[2:].partition("=")
            metadata[key] = value
        elif line and not line.startswith("#"):
            data.append(line)
    if metadata.get("schema") != "openmw-vita-bench-v1":
        raise BenchError("Unknown or missing raw CSV schema")
    if metadata.get("outcome") != "completed":
        raise BenchError("Raw CSV does not describe a completed benchmark")
    if metadata.get("rejected_clock_samples") != "0":
        raise BenchError("Raw CSV has missing or rejected clock samples")
    reader = csv.DictReader(io.StringIO("\n".join(data)))
    if reader.fieldnames != ["frame", "frame_us", "vblank_wait_us", "residual_us"]:
        raise BenchError("Unexpected raw CSV columns")
    durations = []
    for index, row in enumerate(reader):
        if None in row:
            raise BenchError("Raw CSV contains extra columns")
        try:
            frame, duration, vblank_wait, residual = (int(row[name]) for name in reader.fieldnames)
        except (TypeError, ValueError) as error:
            raise BenchError("Raw CSV contains an invalid integer sample") from error
        if frame != index or duration <= 0 or vblank_wait < 0 or residual != max(0, duration - vblank_wait):
            raise BenchError("Raw CSV contains an invalid frame sample")
        durations.append(duration)
    if not durations or len(durations) != expected_frames:
        raise BenchError("Raw CSV frame count does not match the completed result")
    return {"frames": len(durations), "elapsed_seconds": sum(durations) / 1e6,
            "fps_mean": len(durations) * 1e6 / sum(durations), "metadata": metadata}


def stop_child(process) -> None:
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def summary_fields(line: str) -> dict:
    fields = {}
    for name, value in re.findall(r"([a-zA-Z_]+)=([^\s]+)", line):
        fields.setdefault(name, value)
    return fields


def run_benchmark(args, popen=subprocess.Popen) -> dict:
    emulator, config, pref, route, output = (Path(getattr(args, key)).expanduser().resolve()
                                            for key in ("emulator", "config", "pref_path", "route", "output"))
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        raise BenchError("--timeout must be finite and positive")
    if not math.isfinite(args.settle) or args.settle < 0:
        raise BenchError("--settle must be finite and nonnegative")
    if output.exists():
        raise BenchError("Output directory already exists; choose a unique --output")
    if inside(output, pref):
        raise BenchError("Output must be outside the Vita3K filesystem")
    for path in (emulator, config, route):
        if not path.is_file():
            raise BenchError(f"Required file not found: {path}")
    if not os.access(emulator, os.X_OK):
        raise BenchError("--emulator must be executable")
    data = pref / "ux0/data/openmw"
    app = pref / "ux0/app" / TITLE
    binary = app / "eboot.bin"
    if not binary.is_file() or not data.is_dir():
        raise BenchError("Existing Vita3K OpenMW installation was not found")
    if not inside(data.resolve(), pref) or not inside(app.resolve(), pref):
        raise BenchError("Installation directories must resolve inside --pref-path")
    bench = data / "bench"
    if bench.is_symlink() or (bench.exists() and not bench.is_dir()):
        raise BenchError("Benchmark directory must be a directory without a symlink")
    save = resolve_save(data / "saves", args.save)
    with ExclusiveLock(pref / ".openmw-vita3k-bench.lock"):
        output.mkdir(parents=True, exist_ok=False)
        manifest = {"schema": "openmw-vita3k-run-v1", "platform": "vita3k", "provisional": True,
                    "hardware_target_confirmed": False, "status": "preparing", "started_at": utc_now(),
                    "host": {"platform": platform.platform(), "machine": platform.machine(),
                             "python": platform.python_version()}, "title_id": TITLE,
                    "parameters": {"settle_seconds": args.settle, "fps": 0, "timeout_seconds": args.timeout},
                    "process_log_limit_bytes": PROCESS_LOG_LIMIT_BYTES,
                    "restored_toggles": False}
        snapshots = []
        persistent_snapshots = []
        process = None
        owned_route = None
        log_path = output / "process.log"
        try:
            copied_config = output / "vita3k-config.yml"
            private_config(config, copied_config, pref)
            copied_route = output / "route.txt"
            shutil.copyfile(route, copied_route)
            manifest["artifacts"] = {name: artifact(path) for name, path in
                                     (("emulator", emulator), ("executable", binary), ("config_source", config),
                                      ("config_run", copied_config), ("route", copied_route), ("save", save))}
            manifest["route_source"] = str(route)
            trees = {name: hash_tree(data / name) for name in ("Data Files", "mods", "config")}
            trees["application"] = hash_tree(app)
            write_json(output / "game-data-manifest.json", trees)
            manifest["game_data"] = {name: {k: v for k, v in tree.items() if k != "files"}
                                     for name, tree in trees.items()}
            manifest["game_configs"] = {p.name: artifact(p) for p in sorted(data.glob("*.cfg")) if p.is_file()}
            if (data / "config").exists():
                shutil.copytree(data / "config", output / "game-config")
                manifest["game_config_snapshot"] = str(output / "game-config")
            persistent_names = set(PERSISTENT_CONFIG_FILES) | {p.name for p in (data / "config").glob("*.cfg")}
            persistent_snapshots = [SavedFile.capture(data / "config" / name) for name in sorted(persistent_names)]
            manifest["persistent_inputs"] = {
                snapshot.path.name: {"before": {"present": False} if snapshot.content is None else {
                    "present": True, "bytes": len(snapshot.content),
                    "sha256": hashlib.sha256(snapshot.content).hexdigest()}}
                for snapshot in persistent_snapshots}
            root_configs = list(data.glob("*.cfg"))
            if root_configs:
                (output / "root-config").mkdir()
                for source in root_configs:
                    shutil.copyfile(source, output / "root-config" / source.name)
            manifest["runtime_toggles"] = {}
            for source in sorted(data.glob("*.txt")):
                if source.name in ("bench.txt", "benchrec.txt"):
                    continue
                if source.is_symlink() or not source.is_file():
                    raise BenchError(f"Runtime toggle must be a regular file: {source}")
                destination = output / "runtime-toggles" / source.name
                destination.parent.mkdir(exist_ok=True)
                shutil.copyfile(source, destination)
                manifest["runtime_toggles"][source.name] = artifact(destination)
            experiment = data / "no-scene-stats.txt"
            if experiment.is_symlink() or (experiment.exists() and not experiment.is_file()):
                raise BenchError("Scene statistics toggle must be a regular file")
            manifest["experiment_toggles"] = {experiment.name: {"present": experiment.exists()}}
            if experiment.exists():
                copied_experiment = output / experiment.name
                shutil.copyfile(experiment, copied_experiment)
                manifest["experiment_toggles"][experiment.name]["artifact"] = artifact(copied_experiment)
            bench.mkdir(exist_ok=True)
            raw_before = {p.name for p in bench.glob("frames-*.csv")}
            snapshots = [SavedFile.capture(data / name) for name in ("bench.txt", "benchrec.txt")]
            owned_route = bench / ("vita3k-" + uuid.uuid4().hex + ".txt")
            with owned_route.open("xb") as destination:
                destination.write(copied_route.read_bytes())
            (data / "benchrec.txt").unlink(missing_ok=True)
            (data / "bench.txt").write_text(
                f"save={args.save}\nroute={owned_route.name}\nsettle={args.settle:g}\nfps=0\nexit\n", encoding="utf-8")
            manifest["benchmark_config"] = (data / "bench.txt").read_text()
            command = [str(emulator), "--frontend", "sdl", "--keep-config", "--config-location",
                       str(copied_config), "-r", TITLE]
            manifest["command"] = command
            manifest["status"] = "running"
            manifest["launched_at"] = utc_now()
            write_json(output / "manifest.json", manifest)
            readers = [(FreshLog(data / "boot.log"), False), (FreshLog(bench / "results.txt"), True)]
            selected = None
            with log_path.open("wb") as log:
                process_reader = FreshLog(log_path)
                readers.append((process_reader, False))
                process = popen(command, cwd=output, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                deadline = time.monotonic() + args.timeout
                exited = False
                while True:
                    for reader, plain in readers:
                        lines = check_process_log(reader, manifest) if reader is process_reader else reader.lines()
                        for line in lines:
                            if "[Bench] failed:" in line:
                                failure = line.split("[Bench] failed:", 1)[1].strip()
                                manifest["benchmark_failure"] = failure
                                reference = summary_fields(failure).get("partial_raw")
                                if reference:
                                    try:
                                        partial = copy_fresh_raw(reference, bench, output, raw_before)
                                        manifest["partial_raw_csv"] = artifact(partial)
                                    except (BenchError, OSError) as error:
                                        manifest["partial_capture_error"] = str(error)
                                        raise BenchError(f"Benchmark failed; partial CSV rejected: {error}") from error
                                raise BenchError(f"Benchmark reported failure: {failure}")
                            if "[Bench] done " in line:
                                selected = line.split("[Bench] done ", 1)[1].strip()
                            elif plain and line.startswith("build=") and "frames=" in line:
                                selected = line.strip()
                    if process_reader.has_unread():
                        if time.monotonic() >= deadline:
                            manifest["status"] = "timeout"
                            raise BenchError("Emulator benchmark timed out")
                        continue
                    if selected is not None:
                        break
                    if exited:
                        raise BenchError(f"Emulator exited without a fresh benchmark result (exit {process.returncode})")
                    if process.poll() is not None:
                        exited = True
                        continue
                    if time.monotonic() >= deadline:
                        manifest["status"] = "timeout"
                        raise BenchError("Emulator benchmark timed out")
                    time.sleep(0.1)
                fields = summary_fields(selected)
                try:
                    count = int(fields.get("frames", ""))
                except ValueError as error:
                    raise BenchError("Result has no valid frame count") from error
                if count <= 0:
                    raise BenchError("Result has no positive frame count")
                if "raw" not in fields:
                    if not args.allow_legacy_summary:
                        raise BenchError("Completed result has no raw CSV; use --allow-legacy-summary only for legacy diagnostics")
                    manifest["status"] = "legacy_summary_only"
                    manifest["limitation"] = "Legacy simulation deltas can clip stalls; summary is unusable for target validation"
                    result = {"text": selected, "fields": fields, "legacy": True}
                else:
                    local_raw = copy_fresh_raw(fields["raw"], bench, output, raw_before)
                    measured = read_samples(local_raw, count)
                    metadata = measured["metadata"]
                    expected_route = "ux0:data/openmw/bench/" + owned_route.name
                    if (metadata.get("route") != expected_route or metadata.get("save_request") != args.save
                            or metadata.get("requested_fps") != "0.000000"):
                        raise BenchError("Raw CSV does not match this run's route, save and uncapped FPS request")
                    result = {"text": selected, "fields": fields, "legacy": False,
                              "raw_csv": artifact(local_raw), "measured": measured}
                    manifest["status"] = "completed"
                write_json(output / "summary.json", result)
                # The benchmark requests exit; stop only this child if shutdown stalls.
                shutdown_deadline = time.monotonic() + min(5.0, args.timeout)
                while process.poll() is None:
                    check_process_log(process_reader, manifest)
                    if time.monotonic() >= shutdown_deadline:
                        manifest["forced_shutdown_after_result"] = True
                        stop_child(process)
                        break
                    if not process_reader.has_unread():
                        time.sleep(0.1)
                while process_reader.has_unread():
                    check_process_log(process_reader, manifest)
                manifest["emulator_exit_code"] = process.returncode
                if process.returncode and not manifest.get("forced_shutdown_after_result"):
                    raise BenchError(f"Emulator exited unsuccessfully after result (exit {process.returncode})")
            if sha256(save) != manifest["artifacts"]["save"]["sha256"]:
                raise BenchError("Selected save changed during the run")
        except BaseException as error:
            if manifest["status"] != "timeout":
                manifest["status"] = "failed"
            manifest["error"] = str(error)
            raise
        finally:
            restoration_errors = []
            try:
                stop_child(process)
            except (OSError, subprocess.SubprocessError) as error:
                restoration_errors.append(f"Child shutdown: {error}")
            log_errors = []
            if log_path.exists():
                try:
                    if log_path.is_symlink() or not log_path.is_file():
                        raise OSError("Process log must be a regular file")
                    size = log_path.stat().st_size
                    if size > PROCESS_LOG_LIMIT_BYTES:
                        manifest["process_log_bytes_before_truncation"] = size
                        with log_path.open("r+b") as log:
                            log.truncate(PROCESS_LOG_LIMIT_BYTES)
                        manifest["process_log_truncated"] = True
                    manifest["process_log_final_bytes"] = log_path.stat().st_size
                except OSError as error:
                    log_errors.append(f"{log_path}: {error}")
            manifest["game_log_snapshots"] = {}
            for source in (data / "boot.log", bench / "results.txt", data / "config/openmw.log",
                           data / "config/MyGUI.log"):
                if source.exists():
                    try:
                        if source.is_symlink() or not source.is_file():
                            raise OSError("Log snapshot source must be a regular file")
                        destination = output / ("game-" + source.name)
                        shutil.copyfile(source, destination)
                        manifest["game_log_snapshots"][source.name] = {
                            "scope": "whole file; may contain older runs", "artifact": artifact(destination)}
                    except OSError as error:
                        log_errors.append(f"{source}: {error}")
            input_capture_errors = []
            for snapshot in persistent_snapshots:
                try:
                    state = file_state(snapshot.path)
                    manifest["persistent_inputs"][snapshot.path.name]["after"] = state
                    if state["present"]:
                        destination = output / "persistent-inputs-after" / snapshot.path.name
                        destination.parent.mkdir(exist_ok=True)
                        shutil.copyfile(snapshot.path, destination)
                except (OSError, BenchError) as error:
                    input_capture_errors.append(f"{snapshot.path}: {error}")
            for snapshot in snapshots:
                try:
                    snapshot.restore()
                except OSError as error:
                    restoration_errors.append(f"{snapshot.path}: {error}")
            persistent_restore_errors = []
            for snapshot in persistent_snapshots:
                try:
                    snapshot.restore()
                    state = file_state(snapshot.path)
                    evidence = manifest["persistent_inputs"][snapshot.path.name]
                    evidence["restored"] = state
                    if state != evidence["before"]:
                        raise BenchError("Restored input differs from its initial state")
                except (OSError, BenchError) as error:
                    persistent_restore_errors.append(f"{snapshot.path}: {error}")
            if owned_route is not None:
                try:
                    owned_route.unlink(missing_ok=True)
                except OSError as error:
                    restoration_errors.append(f"{owned_route}: {error}")
            manifest["restored_toggles"] = not restoration_errors
            manifest["restored_persistent_inputs"] = not persistent_restore_errors
            manifest["ended_at"] = utc_now()
            if restoration_errors:
                manifest["status"] = "failed"
                manifest["restoration_errors"] = restoration_errors
            if log_errors:
                manifest["status"] = "failed"
                manifest["log_capture_errors"] = log_errors
            if input_capture_errors or persistent_restore_errors:
                manifest["status"] = "failed"
                manifest["persistent_input_errors"] = input_capture_errors + persistent_restore_errors
            write_json(output / "manifest.json", manifest)
            cleanup_errors = restoration_errors + log_errors + input_capture_errors + persistent_restore_errors
            if cleanup_errors:
                raise BenchError("Benchmark cleanup failed: " + "; ".join(cleanup_errors))
        return manifest


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("emulator", "config", "pref-path", "route", "save", "output"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--settle", type=float, default=5.0)
    parser.add_argument("--allow-legacy-summary", action="store_true")
    args = parser.parse_args(argv)
    def terminate(_signum, _frame):
        raise KeyboardInterrupt()
    previous_handler = signal.signal(signal.SIGTERM, terminate)
    try:
        result = run_benchmark(args)
    except KeyboardInterrupt:
        print("Benchmark interrupted; cleanup attempted", file=sys.stderr)
        return 130
    except (BenchError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Benchmark failed: {error}", file=sys.stderr)
        return 1
    finally:
        signal.signal(signal.SIGTERM, previous_handler)
    print(f"{result['status']}: {Path(args.output).resolve()} (Vita3K; provisional)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
