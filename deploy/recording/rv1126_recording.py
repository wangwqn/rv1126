#!/usr/bin/env python3
"""Supervise segmented recording, event retention and bounded disk usage."""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path


DEFAULTS = {
    "enabled": True,
    "segment_seconds": 10,
    "max_storage_mb": 512,
    "min_free_mb": 128,
    "event_pre_seconds": 10,
    "event_post_seconds": 20,
    "event_storage_mb": 192,
}


def atomic_json(path, value, mode=0o600):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(value, handle, ensure_ascii=False, sort_keys=True, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def bounded_integer(value, minimum, maximum, name):
    if isinstance(value, bool):
        raise ValueError(name + " must be an integer")
    result = int(value)
    if result < minimum or result > maximum:
        raise ValueError("{} must be in [{}, {}]".format(name, minimum, maximum))
    return result


def load_config(path):
    value = {}
    try:
        stored = json.loads(Path(path).read_text(encoding="utf-8"))
        value = stored.get("recording", {})
    except (OSError, ValueError, TypeError):
        pass
    if not isinstance(value, dict):
        raise ValueError("recording configuration must be an object")
    unknown = set(value) - set(DEFAULTS)
    if unknown:
        raise ValueError("unknown recording settings: " + ", ".join(sorted(unknown)))
    result = dict(DEFAULTS)
    result.update(value)
    if not isinstance(result["enabled"], bool):
        raise ValueError("recording.enabled must be boolean")
    result["segment_seconds"] = bounded_integer(result["segment_seconds"], 5, 600, "segment_seconds")
    result["max_storage_mb"] = bounded_integer(result["max_storage_mb"], 64, 8192, "max_storage_mb")
    result["min_free_mb"] = bounded_integer(result["min_free_mb"], 32, 4096, "min_free_mb")
    result["event_pre_seconds"] = bounded_integer(result["event_pre_seconds"], 0, 120, "event_pre_seconds")
    result["event_post_seconds"] = bounded_integer(result["event_post_seconds"], 0, 300, "event_post_seconds")
    result["event_storage_mb"] = bounded_integer(result["event_storage_mb"], 32, 4096, "event_storage_mb")
    return result


def file_size(paths):
    total = 0
    for path in paths:
        try:
            total += path.stat().st_size
        except OSError:
            pass
    return total


class RecordingService:
    def __init__(self, args):
        self.args = args
        self.root = Path(args.root)
        self.config = load_config(self.root / "runtime-config.json")
        self.recordings = self.root / "recordings"
        self.continuous = self.recordings / "continuous"
        self.events = self.recordings / "events"
        self.event_log = self.root / "events" / "events.jsonl"
        self.status_path = Path(args.status)
        self.state_path = Path(args.state)
        self.binary = self.root / "rv1126-recorder"
        self.log_path = Path(args.recorder_log)
        self.child = None
        self.child_progress = None
        self.child_progress_at = 0.0
        self.stopping = False
        self.last_error = ""
        self.restart_count = 0
        self.pending = []
        self.event_offset = self._initial_offset()
        self.continuous.mkdir(parents=True, exist_ok=True)
        self.events.mkdir(parents=True, exist_ok=True)

    def _initial_offset(self):
        try:
            value = json.loads(self.state_path.read_text(encoding="utf-8"))
            return max(0, int(value.get("event_offset", 0)))
        except (OSError, ValueError, TypeError):
            try:
                return self.event_log.stat().st_size
            except OSError:
                return 0

    def _save_offset(self):
        atomic_json(self.state_path, {"event_offset": self.event_offset})

    def _start_recorder(self):
        if not self.binary.is_file() or not os.access(str(self.binary), os.X_OK):
            raise RuntimeError("missing recorder binary: " + str(self.binary))
        if int(time.time()) < 1704067200:
            raise RuntimeError("system clock is not synchronised")
        session = time.strftime("%Y%m%dT%H%M%S", time.localtime())
        location = str(self.continuous / (session + "-%05d.mkv"))
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        log = self.log_path.open("ab", buffering=0)
        command = [str(self.binary), "--location", location,
                   "--segment-seconds", str(self.config["segment_seconds"])]
        self.child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                      close_fds=True, preexec_fn=lambda: os.nice(10))
        self.child_progress = None
        self.child_progress_at = time.monotonic()

    def _stop_recorder(self):
        child, self.child = self.child, None
        if not child or child.poll() is not None:
            return
        child.terminate()
        try:
            child.wait(timeout=15)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=5)

    def _watch_recorder_progress(self, now):
        """Restart a live but stalled RTSP client after a bounded grace period."""
        segments = self._segments()
        progress = None
        if segments:
            modified, latest = segments[-1]
            try:
                progress = (latest.name, latest.stat().st_size, modified)
            except OSError:
                progress = None
        if progress != self.child_progress:
            self.child_progress = progress
            self.child_progress_at = now
            return False
        grace = max(20, self.config["segment_seconds"] * 3)
        if self.child and now - self.child_progress_at > grace:
            self.last_error = "recorder made no file progress for {} seconds".format(grace)
            self._stop_recorder()
            self.restart_count += 1
            self.child_progress_at = now
            return True
        return False

    def _segments(self):
        result = []
        for path in self.continuous.glob("*.mkv"):
            try:
                result.append((path.stat().st_mtime, path))
            except OSError:
                pass
        return sorted(result)

    def _segment_window(self, modified, path):
        """Return the estimated media time range for a splitmux segment.

        splitmuxsink may update every segment's mtime while finalising the
        pipeline.  The recorder filename therefore carries the authoritative
        session start and sequence number.  Older/manual files retain the
        mtime fallback.
        """
        match = re.match(r"^(\d{8}T\d{6})-(\d{5})\.mkv$", path.name)
        if match:
            try:
                session = time.mktime(time.strptime(match.group(1), "%Y%m%dT%H%M%S"))
                start = session + int(match.group(2)) * self.config["segment_seconds"]
                return start, start + self.config["segment_seconds"]
            except (OverflowError, ValueError):
                pass
        return modified, modified

    @staticmethod
    def _safe_event_name(event, sequence):
        kind = re.sub(r"[^A-Za-z0-9_-]+", "-", str(event.get("type", "event")))[:32]
        track = int(event.get("track_id", 0) or 0)
        return "{}-{:06d}-{}-track{}".format(
            time.strftime("%Y%m%dT%H%M%S", time.localtime()), sequence, kind, track)

    @staticmethod
    def _link(source, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            return
        try:
            os.link(str(source), str(destination))
        except FileExistsError:
            pass
        except OSError:
            shutil.copy2(str(source), str(destination))

    def _protect_window(self, pending, final=False):
        now = time.time()
        segment = self.config["segment_seconds"]
        lower = pending["seen_at"] - self.config["event_pre_seconds"] - segment
        upper = (pending["seen_at"] + self.config["event_post_seconds"] + segment
                 if final else now)
        for modified, source in self._segments():
            segment_start, segment_end = self._segment_window(modified, source)
            if segment_end >= lower and segment_start <= upper:
                self._link(source, pending["directory"] / source.name)
        if final:
            files = sorted(path.name for path in pending["directory"].glob("*.mkv"))
            atomic_json(pending["directory"] / "event.json",
                        {"event": pending["event"], "recording_files": files,
                         "pre_seconds": self.config["event_pre_seconds"],
                         "post_seconds": self.config["event_post_seconds"]}, 0o644)

    def _read_events(self):
        try:
            size = self.event_log.stat().st_size
        except OSError:
            return
        if size < self.event_offset:  # event log rotated
            self.event_offset = 0
        if size == self.event_offset:
            return
        with self.event_log.open("rb") as handle:
            handle.seek(self.event_offset)
            lines = handle.readlines()
            self.event_offset = handle.tell()
        self._save_offset()
        for line in lines:
            try:
                event = json.loads(line.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                continue
            seen = time.time()
            sequence = len(self.pending) + int(seen * 1000) % 100000
            directory = self.events / self._safe_event_name(event, sequence)
            pending = {
                "event": event, "seen_at": seen, "directory": directory,
                "due": seen + self.config["event_post_seconds"] +
                       self.config["segment_seconds"] * 2 + 2,
                "expires": seen + self.config["event_post_seconds"] + 120,
            }
            directory.mkdir(parents=True, exist_ok=True)
            self._protect_window(pending, False)
            self.pending.append(pending)

    def _finish_events(self):
        now = time.time()
        remaining = []
        for pending in self.pending:
            if now >= pending["due"]:
                self._protect_window(pending, False)
                files = list(pending["directory"].glob("*.mkv"))
                if files or now >= pending["expires"]:
                    self._protect_window(pending, True)
                else:
                    remaining.append(pending)
            else:
                remaining.append(pending)
        self.pending = remaining

    def _cleanup_continuous(self):
        entries = self._segments()
        limit = self.config["max_storage_mb"] * 1024 * 1024
        minimum_free = self.config["min_free_mb"] * 1024 * 1024
        total = file_size(path for _, path in entries)
        # Always retain the two newest segments; one may still be open.
        removable = entries[:-2]
        for _, path in removable:
            try:
                free = shutil.disk_usage(str(self.recordings)).free
            except OSError:
                free = minimum_free
            if total <= limit and free >= minimum_free:
                break
            try:
                size = path.stat().st_size
                path.unlink()
                total = max(0, total - size)
            except OSError:
                pass

    def _cleanup_events(self):
        directories = []
        for path in self.events.iterdir():
            if not path.is_dir():
                continue
            try:
                modified = path.stat().st_mtime
                size = file_size(item for item in path.rglob("*") if item.is_file())
                directories.append((modified, path, size))
            except OSError:
                pass
        directories.sort()
        limit = self.config["event_storage_mb"] * 1024 * 1024
        total = sum(item[2] for item in directories)
        protected = {item["directory"] for item in self.pending}
        for _, path, size in directories:
            if total <= limit:
                break
            if path in protected:
                continue
            try:
                shutil.rmtree(str(path))
                total = max(0, total - size)
            except OSError:
                pass

    def _status(self):
        segments = self._segments()
        event_files = [item for item in self.events.rglob("*.mkv") if item.is_file()]
        child_pid = self.child.pid if self.child and self.child.poll() is None else None
        latest_age = round(max(0, time.time() - segments[-1][0]), 2) if segments else None
        atomic_json(self.status_path, {
            "ok": bool((not self.config["enabled"]) or (child_pid and segments)),
            "enabled": self.config["enabled"],
            "manager_pid": os.getpid(), "recorder_pid": child_pid,
            "segments": len(segments),
            "continuous_bytes": file_size(path for _, path in segments),
            "event_files": len(event_files), "event_bytes": file_size(event_files),
            "pending_events": len(self.pending), "latest_segment_age_seconds": latest_age,
            "restart_count": self.restart_count, "last_error": self.last_error,
            "updated_at": int(time.time()), "config": self.config,
        })

    def run(self):
        if not self.config["enabled"]:
            while not self.stopping:
                self._status()
                time.sleep(1)
            self._status()
            return
        retry_at = 0
        cleanup_at = 0
        while not self.stopping:
            now = time.monotonic()
            if self.child and self.child.poll() is not None:
                self.last_error = "recorder exited with status {}".format(self.child.returncode)
                self.child = None
                self.restart_count += 1
                retry_at = now + min(60, 5 * max(1, self.restart_count))
            if self.child and self._watch_recorder_progress(now):
                retry_at = now + 5
            if not self.child and now >= retry_at:
                try:
                    self._start_recorder()
                    self.last_error = ""
                except Exception as error:
                    self.last_error = str(error)[:256]
                    retry_at = now + 10
            self._read_events()
            self._finish_events()
            if now >= cleanup_at:
                self._cleanup_continuous()
                self._cleanup_events()
                cleanup_at = now + 5
            self._status()
            time.sleep(1)
        self._stop_recorder()
        self._status()


def main():
    parser = argparse.ArgumentParser(description="RV1126 bounded segmented recording service")
    parser.add_argument("--root", default="/root/rv1126_objects")
    parser.add_argument("--status", default="/userdata/rv1126-recording/status.json")
    parser.add_argument("--state", default="/userdata/rv1126-recording/state.json")
    parser.add_argument("--recorder-log", default="/tmp/rv1126-recorder.log")
    args = parser.parse_args()
    service = RecordingService(args)
    signal.signal(signal.SIGTERM, lambda *_: setattr(service, "stopping", True))
    signal.signal(signal.SIGINT, lambda *_: setattr(service, "stopping", True))
    service.run()


if __name__ == "__main__":
    main()
