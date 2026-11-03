#!/usr/bin/env python3
"""Dependency-free management API and static UI for the RV1126 pipeline."""

import argparse
import collections
import hashlib
import hmac
import ipaddress
import io
import json
import os
import re
import secrets
import shutil
import subprocess
import tarfile
import tempfile
import threading
import time
import urllib.parse
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


MAX_JSON = 64 * 1024
MAX_MODEL = 64 * 1024 * 1024
MAX_OTA = 128 * 1024 * 1024
OTA_UPLOAD_BYTES_PER_SECOND = max(1024 * 1024, int(os.environ.get(
    "RV1126_OTA_UPLOAD_BYTES_PER_SECOND", str(4 * 1024 * 1024))))
AUTH_FAILURE_LIMIT = 5
AUTH_FAILURE_WINDOW = 60
AUTH_BLOCK_SECONDS = 300
STATIC_FILES = {
    "/": ("web-index.html", "text/html; charset=utf-8"),
    "/index.html": ("web-index.html", "text/html; charset=utf-8"),
    "/app.js": ("web-app.js", "application/javascript; charset=utf-8"),
    "/styles.css": ("web-styles.css", "text/css; charset=utf-8"),
}
MODEL_FILES = {
    "rknn": "objects.rknn",
    "metadata": "objects.json",
    "labels": "labels.png",
}


def atomic_write(path, data, mode=0o600):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as handle:
            if isinstance(data, str):
                data = data.encode("utf-8")
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def run(command, timeout=15, check=False):
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        check=False,
    )
    if check and result.returncode:
        raise RuntimeError("command failed: " + result.stdout.strip())
    return result.returncode, result.stdout.strip()


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class ManagementState:
    def __init__(self, root, token_file):
        # Keep the stable A/B symlink as the logical root. Resolving it would
        # make model/state paths unexpectedly follow a particular release.
        self.root = Path(root).absolute()
        self.token_file = Path(token_file)
        self.controller = self.root / "rv1126-object"
        self.runtime_config = self.root / "runtime-config.json"
        self.runtime_args = self.root / "runtime.args"
        self.events_file = self.root / "events" / "events.jsonl"
        self.analytics_rules = self.root / "analytics.rules"
        default_model_root = (Path("/root/rv1126_models")
                              if self.root == Path("/root/rv1126_objects")
                              else self.root.parent / "rv1126_models")
        self.model_root = Path(os.environ.get("RV1126_MODEL_ROOT", str(default_model_root)))
        self.model_stage = self.model_root / "staging"
        self.model_active = self.model_root / "active"
        self.model_history = self.model_root / "history"
        self.lock = threading.RLock()
        self.auth_lock = threading.Lock()
        self.auth_failures = {}
        self.auth_blocks = {}
        self.audit_file = Path(os.environ.get(
            "RV1126_AUDIT_FILE", str(self.root / "audit.jsonl")))
        self.cloud_credential = Path(os.environ.get(
            "RV1126_CLOUD_CREDENTIAL", "/userdata/rv1126-security/device-credential.env"))
        self.ota_manager = Path(os.environ.get(
            "RV1126_OTA_MANAGER", "/root/rv1126_ota/rv1126_ota.py"))
        self.ota_bundle = Path(os.environ.get(
            "RV1126_OTA_BUNDLE", "/userdata/rv1126-ota/incoming/update.rvota"))
        self.ota_log = Path(os.environ.get(
            "RV1126_OTA_LOG", "/tmp/rv1126-ota.log"))
        self.cpu_sample = None

    @staticmethod
    def defaults():
        return {
            "media": {
                "bitrate": 4000000,
                "sub_bitrate": 600000,
                "gop": 30,
                "ai_fps": 3,
                "threshold": 0.25,
                "result_ttl_ms": 500,
                "max_osd_boxes": 8,
                "rtsp_client_buffer_kb": 512,
            },
            "analytics": {"regions": [], "lines": []},
        }

    def token(self):
        try:
            return self.token_file.read_text(encoding="utf-8").strip()
        except OSError:
            return ""

    def authorized(self, header):
        expected = self.token()
        return bool(expected) and hmac.compare_digest(header, "Bearer " + expected)

    def check_authorization(self, header, client):
        now = time.monotonic()
        with self.auth_lock:
            blocked_until = self.auth_blocks.get(client, 0)
            if blocked_until > now:
                return False, max(1, int(blocked_until - now)), True
            if self.authorized(header):
                self.auth_failures.pop(client, None)
                self.auth_blocks.pop(client, None)
                return True, 0, False
            failures = self.auth_failures.setdefault(client, collections.deque())
            while failures and failures[0] < now - AUTH_FAILURE_WINDOW:
                failures.popleft()
            failures.append(now)
            if len(failures) >= AUTH_FAILURE_LIMIT:
                self.auth_blocks[client] = now + AUTH_BLOCK_SECONDS
                failures.clear()
                return False, AUTH_BLOCK_SECONDS, True
            return False, 0, False

    def rotate_token(self):
        value = secrets.token_hex(32)
        atomic_write(self.token_file, value + "\n", 0o600)
        return value

    def invalidate_token(self):
        atomic_write(self.token_file, b"", 0o600)

    def audit(self, action, client, result, detail=""):
        record = {
            "time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "client": str(client)[:64],
            "action": str(action)[:96],
            "result": str(result)[:32],
        }
        if detail:
            record["detail"] = str(detail).replace("\n", " ")[:256]
        self.audit_file.parent.mkdir(parents=True, exist_ok=True)
        line = json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n"
        with self.lock:
            with self.audit_file.open("a", encoding="utf-8") as handle:
                handle.write(line)
                handle.flush()
                os.fsync(handle.fileno())
            os.chmod(self.audit_file, 0o600)

    def security_status(self):
        return {
            "token_active": bool(self.token()),
            "https_required": True,
            "cloud_credential_provisioned": self.cloud_credential.is_file(),
            "cloud_credential_separate": self.cloud_credential != self.token_file,
        }

    def _parse_saved_args(self, config):
        source = self.runtime_args
        watchdog_args = Path("/var/run/rv1126-object-args")
        if not source.is_file() and watchdog_args.is_file():
            source = watchdog_args
        try:
            values = source.read_text(encoding="utf-8").splitlines()
        except OSError:
            return config
        option_map = {
            "--bitrate": ("bitrate", int),
            "--sub-bitrate": ("sub_bitrate", int),
            "--gop": ("gop", int),
            "--ai-fps": ("ai_fps", int),
            "--threshold": ("threshold", float),
            "--result-ttl-ms": ("result_ttl_ms", int),
            "--max-osd-boxes": ("max_osd_boxes", int),
            "--rtsp-client-buffer-kb": ("rtsp_client_buffer_kb", int),
        }
        index = 0
        while index + 1 < len(values):
            key = values[index]
            if key in option_map:
                name, converter = option_map[key]
                try:
                    config["media"][name] = converter(values[index + 1])
                except ValueError:
                    pass
                index += 2
            else:
                index += 1
        return config

    def config(self):
        config = self.defaults()
        try:
            stored = json.loads(self.runtime_config.read_text(encoding="utf-8"))
            config["media"].update(stored.get("media", {}))
            config["analytics"].update(stored.get("analytics", {}))
        except (OSError, ValueError, TypeError):
            config = self._parse_saved_args(config)
        return self.validate_config(config)

    @staticmethod
    def _number(value, minimum, maximum, name, integer=True):
        if isinstance(value, bool):
            raise ValueError(name + " must be numeric")
        try:
            result = int(value) if integer else float(value)
        except (TypeError, ValueError):
            raise ValueError(name + " must be numeric")
        if result < minimum or result > maximum:
            raise ValueError("{} must be in [{},{}]".format(name, minimum, maximum))
        return result

    @classmethod
    def _shape(cls, shape, kind):
        if not isinstance(shape, dict):
            raise ValueError(kind + " must be an object")
        name = str(shape.get("name", "")).strip()[:48]
        if not name:
            raise ValueError(kind + " name is required")
        keys = ("x", "y", "width", "height") if kind == "region" else (
            "x1", "y1", "x2", "y2")
        result = {"name": name}
        for key in keys:
            result[key] = cls._number(shape.get(key), 0, 1, kind + "." + key, False)
        if kind == "region" and (result["width"] <= 0 or result["height"] <= 0 or
                                  result["x"] + result["width"] > 1.000001 or
                                  result["y"] + result["height"] > 1.000001):
            raise ValueError("region must fit normalized video coordinates")
        if kind == "line" and ((result["x2"] - result["x1"]) ** 2 +
                                (result["y2"] - result["y1"]) ** 2) ** 0.5 < 0.01:
            raise ValueError("line endpoints must be distinct")
        return result

    @classmethod
    def validate_config(cls, config):
        if not isinstance(config, dict):
            raise ValueError("configuration must be an object")
        media = config.get("media", {})
        if not isinstance(media, dict):
            raise ValueError("media must be an object")
        defaults = cls.defaults()["media"]
        unknown = set(media) - set(defaults)
        if unknown:
            raise ValueError("unknown media settings: " + ", ".join(sorted(unknown)))
        result_media = {
            "bitrate": cls._number(media.get("bitrate", defaults["bitrate"]), 200000, 12000000, "bitrate"),
            "sub_bitrate": cls._number(media.get("sub_bitrate", defaults["sub_bitrate"]), 50000, 4000000, "sub_bitrate"),
            "gop": cls._number(media.get("gop", defaults["gop"]), 1, 300, "gop"),
            "ai_fps": cls._number(media.get("ai_fps", defaults["ai_fps"]), 1, 30, "ai_fps"),
            "threshold": cls._number(media.get("threshold", defaults["threshold"]), 0.01, 1.0, "threshold", False),
            "result_ttl_ms": cls._number(media.get("result_ttl_ms", defaults["result_ttl_ms"]), 100, 10000, "result_ttl_ms"),
            "max_osd_boxes": cls._number(media.get("max_osd_boxes", defaults["max_osd_boxes"]), 1, 8, "max_osd_boxes"),
            "rtsp_client_buffer_kb": cls._number(media.get("rtsp_client_buffer_kb", defaults["rtsp_client_buffer_kb"]), 128, 4096, "rtsp_client_buffer_kb"),
        }
        analytics = config.get("analytics", {})
        if not isinstance(analytics, dict):
            raise ValueError("analytics must be an object")
        regions = analytics.get("regions", [])
        lines = analytics.get("lines", [])
        if not isinstance(regions, list) or len(regions) > 16:
            raise ValueError("regions must contain at most 16 items")
        if not isinstance(lines, list) or len(lines) > 16:
            raise ValueError("lines must contain at most 16 items")
        return {
            "media": result_media,
            "analytics": {
                "regions": [cls._shape(item, "region") for item in regions],
                "lines": [cls._shape(item, "line") for item in lines],
            },
        }

    def _model_arguments(self):
        if (self.model_active / "MANIFEST.json").is_file():
            return [
                "--model", str(self.model_active / "objects.rknn"),
                "--metadata", str(self.model_active / "objects.json"),
                "--labels", str(self.model_active / "labels.png"),
            ]
        return []

    def arguments(self, config):
        media = config["media"]
        arguments = [
            "--sensor-mode", "1080p", "--width", "1920", "--height", "1080",
            "--sub-width", "640", "--sub-height", "360", "--fps", "30",
            "--ai-fps", str(media["ai_fps"]), "--max-osd-boxes", str(media["max_osd_boxes"]),
            "--bitrate", str(media["bitrate"]), "--sub-bitrate", str(media["sub_bitrate"]),
            "--gop", str(media["gop"]), "--pool-buffers", "8", "--packet-queue", "8",
            "--rotation", "270", "--threshold", str(media["threshold"]),
            "--result-ttl-ms", str(media["result_ttl_ms"]),
            "--rtsp-client-buffer-kb", str(media["rtsp_client_buffer_kb"]),
            "--audio-device", "plughw:0,0", "--audio-source", "main",
            "--audio-period-ms", "20", "--dma-audit",
        ]
        return arguments + self._model_arguments()

    @staticmethod
    def analytics_rules_text(config):
        lines = ["# rv1126 analytics rules schema=1"]
        for region in config["analytics"]["regions"]:
            name = region["name"].encode("utf-8").hex()
            lines.append("region {} {:.8f} {:.8f} {:.8f} {:.8f}".format(
                name, region["x"], region["y"], region["width"], region["height"]))
        for rule in config["analytics"]["lines"]:
            name = rule["name"].encode("utf-8").hex()
            lines.append("line {} {:.8f} {:.8f} {:.8f} {:.8f}".format(
                name, rule["x1"], rule["y1"], rule["x2"], rule["y2"]))
        return "\n".join(lines) + "\n"

    def save_config(self, payload):
        apply_now = bool(payload.get("apply", False)) if isinstance(payload, dict) else False
        candidate = dict(payload)
        candidate.pop("apply", None)
        validated = self.validate_config(candidate)
        encoded = json.dumps(validated, ensure_ascii=False, indent=2) + "\n"
        args_encoded = "\n".join(self.arguments(validated)) + "\n"
        rules_encoded = self.analytics_rules_text(validated)
        with self.lock:
            old_config = self.runtime_config.read_bytes() if self.runtime_config.exists() else None
            old_args = self.runtime_args.read_bytes() if self.runtime_args.exists() else None
            old_rules = self.analytics_rules.read_bytes() if self.analytics_rules.exists() else None
            atomic_write(self.runtime_config, encoded)
            atomic_write(self.runtime_args, args_encoded)
            atomic_write(self.analytics_rules, rules_encoded)
            if apply_now:
                try:
                    self.run_controller("restart", timeout=90)
                    self.wait_controller_health()
                except Exception as apply_error:
                    if old_config is None:
                        self.runtime_config.unlink(missing_ok=True)
                    else:
                        atomic_write(self.runtime_config, old_config)
                    if old_args is None:
                        self.runtime_args.unlink(missing_ok=True)
                    else:
                        atomic_write(self.runtime_args, old_args)
                    if old_rules is None:
                        self.analytics_rules.unlink(missing_ok=True)
                    else:
                        atomic_write(self.analytics_rules, old_rules)
                    try:
                        self.run_controller("restart", timeout=90)
                        self.wait_controller_health()
                    except Exception as rollback_error:
                        raise RuntimeError(
                            "configuration apply failed: {}; rollback failed: {}".format(
                                apply_error, rollback_error))
                    raise apply_error
        return {"config": validated, "applied": apply_now}

    def run_controller(self, action, timeout=30):
        if action not in {"start", "stop", "restart", "status", "health"}:
            raise ValueError("unsupported controller action")
        if not self.controller.is_file():
            raise RuntimeError("media controller is unavailable")
        code, output = run([str(self.controller), action], timeout=timeout)
        if code:
            raise RuntimeError(output or "media controller failed")
        return output

    def wait_controller_health(self, timeout=60, interval=2):
        """Wait for the camera/media graph to finish its asynchronous warm-up."""
        deadline = time.monotonic() + timeout
        attempts = 0
        last_error = None
        while True:
            attempts += 1
            try:
                return self.run_controller("health", timeout=20)
            except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
                last_error = error
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(
                    "media pipeline did not become healthy within {}s after {} attempts: {}".format(
                        timeout, attempts, last_error))
            time.sleep(min(interval, remaining))

    @staticmethod
    def _read_int(path, default=0):
        try:
            return int(Path(path).read_text().strip())
        except (OSError, ValueError):
            return default

    def _pid(self):
        pid = self._read_int("/var/run/rv1126-object.pid")
        return pid if pid > 0 and Path("/proc/{}".format(pid)).is_dir() else 0

    def _temperatures(self):
        result = {"cpu_c": None, "npu_c": None}
        for zone in Path("/sys/class/thermal").glob("thermal_zone*"):
            try:
                name = (zone / "type").read_text().strip().lower()
                value = int((zone / "temp").read_text().strip()) / 1000.0
            except (OSError, ValueError):
                continue
            if name.startswith("cpu"):
                result["cpu_c"] = value
            elif name.startswith("npu"):
                result["npu_c"] = value
        return result

    @staticmethod
    def _process_stats(pid):
        result = {"rss_kb": 0, "hwm_kb": 0, "threads": 0, "fd_count": 0}
        if not pid:
            return result
        try:
            for line in Path("/proc/{}/status".format(pid)).read_text().splitlines():
                if line.startswith("VmRSS:"):
                    result["rss_kb"] = int(line.split()[1])
                elif line.startswith("VmHWM:"):
                    result["hwm_kb"] = int(line.split()[1])
                elif line.startswith("Threads:"):
                    result["threads"] = int(line.split()[1])
            result["fd_count"] = len(list(Path("/proc/{}/fd".format(pid)).iterdir()))
        except (OSError, ValueError):
            pass
        return result

    @staticmethod
    def _device_memory():
        values = {}
        try:
            for line in Path("/proc/meminfo").read_text().splitlines():
                if line.startswith(("MemTotal:", "MemAvailable:", "MemFree:",
                                    "Buffers:", "Cached:")):
                    values[line.split(":", 1)[0]] = int(line.split()[1])
        except (OSError, ValueError, IndexError):
            return {"memory_total_kb": 0, "memory_available_kb": 0,
                    "memory_used_percent": 0.0}
        total = values.get("MemTotal", 0)
        available = values.get("MemAvailable")
        if available is None:
            available = (values.get("MemFree", 0) + values.get("Buffers", 0) +
                         values.get("Cached", 0))
        used = max(0, total - available)
        return {
            "memory_total_kb": total,
            "memory_available_kb": available,
            "memory_used_percent": round(100.0 * used / total, 2) if total else 0.0,
        }

    @staticmethod
    def _npu_metrics():
        result = {"npu_load_percent": None, "npu_frequency_mhz": None}
        paths = list(Path("/sys/class/devfreq").glob("*.npu"))
        if not paths:
            paths = list(Path("/sys/class/devfreq").glob("*npu*"))
        if not paths:
            return result
        directory = paths[0]
        try:
            result["npu_frequency_mhz"] = round(
                int((directory / "cur_freq").read_text().strip()) / 1000000.0, 1)
        except (OSError, ValueError):
            pass
        try:
            load = (directory / "load").read_text().strip()
            match = re.match(r"(\d+(?:\.\d+)?)\s*(?:%|@|$)", load)
            if match:
                result["npu_load_percent"] = min(100.0, float(match.group(1)))
        except OSError:
            pass
        return result

    def _cpu_percent(self, pid):
        if not pid:
            return 0.0
        try:
            stat = Path("/proc/{}/stat".format(pid)).read_text()
            fields = stat[stat.rfind(")") + 2:].split()
            process = int(fields[11]) + int(fields[12])
            total = sum(int(value) for value in Path("/proc/stat").read_text().splitlines()[0].split()[1:])
        except (OSError, ValueError, IndexError):
            return 0.0
        now = time.monotonic()
        with self.lock:
            previous = self.cpu_sample
            self.cpu_sample = (now, process, total)
        if not previous or total <= previous[2]:
            return 0.0
        return round(100.0 * (process - previous[1]) / (total - previous[2]), 2)

    def _latest_video(self, path=Path("/tmp/rv1126-object.log")):
        try:
            with path.open("rb") as handle:
                handle.seek(max(0, path.stat().st_size - 256 * 1024))
                lines = handle.read().decode("utf-8", "replace").splitlines()
        except OSError:
            return {}
        line = next((item for item in reversed(lines) if item.startswith("video ")), "")
        patterns = {
            "main_venc": r"main\(venc=(\d+)", "sub_venc": r"sub\(venc=(\d+)",
            "main_pts_gap": r"main\(venc=\d+, pts_gap=(\d+)",
            "sub_pts_gap": r"sub\(venc=\d+, pts_gap=(\d+)",
            "ai_completed": r"completed=(\d+)", "osd_boxes": r"osd\(boxes=(\d+)",
            "audio_sent": r"audio\(captured=\d+, sent=(\d+)",
            "event_generated": r"events\(processed=\d+, generated=(\d+)",
            "event_written": r"events\(processed=\d+, generated=\d+, written=(\d+)",
            "event_dropped": r"events\([^)]*dropped=(\d+)",
            "event_write_errors": r"events\([^)]*write_error=(\d+)",
            "event_tracks": r"events\([^)]*tracks=(\d+)",
        }
        result = {"raw": line}
        for name, pattern in patterns.items():
            match = re.search(pattern, line)
            result[name] = int(match.group(1)) if match else 0
        result["backpressure_recoveries"] = sum(
            1 for item in lines if "backpressure recovery" in item)
        return result

    @staticmethod
    def _latest_dma(path=Path("/tmp/rv1126-object.log")):
        try:
            with path.open("rb") as handle:
                handle.seek(max(0, path.stat().st_size - 256 * 1024))
                lines = handle.read().decode("utf-8", "replace").splitlines()
        except OSError:
            return {}
        summary = next((item for item in reversed(lines)
                        if item.startswith("dma_buf ")), "")
        wait = next((item for item in reversed(lines)
                     if item.startswith("dma_wait_ms ")), "")
        graph = next((item for item in reversed(lines)
                      if item.startswith("dma_graph ")), "")
        patterns = {
            "dma_live_handles": r"handles\(live=(\d+)",
            "dma_peak_handles": r"handles\(live=\d+, peak=(\d+)",
            "dma_overdue_handles": r"overdue=(\d+)",
            "dma_live_fds": r"fd\(live=(\d+)",
            "dma_missing_fds": r"missing=(\d+)",
            "dma_required_fd_errors": r"required_error=(\d+)",
            "dma_release_errors": r"release_error=(\d+)",
            "dma_cpu_maps": r"cpu\(map=(\d+)",
            "dma_nv12_cpu_maps": r"nv12=(\d+)",
            "dma_bgr_cpu_maps": r"bgr=(\d+)",
            "dma_encoded_cpu_maps": r"encoded=(\d+)",
            "dma_sync_errors": r"sync_error=(\d+)",
            "dma_process_fds": r"process_dma\(fds=(\d+)",
            "dma_process_bytes": r"process_dma\(fds=\d+, bytes=(\d+)",
            "dma_kernel_objects": r"kernel_dma\(objects=(\d+)",
            "dma_kernel_bytes": r"kernel_dma\(objects=\d+, bytes=(\d+)",
        }
        result = {"dma_raw": summary, "dma_wait_raw": wait,
                  "dma_graph_raw": graph}
        for name, pattern in patterns.items():
            match = re.search(pattern, summary)
            result[name] = int(match.group(1)) if match else 0
        return result

    def _addresses(self):
        code, output = run(["ip", "-4", "-o", "addr", "show"], timeout=3)
        addresses = []
        if code == 0:
            for line in output.splitlines():
                match = re.search(r"^\d+:\s+(\S+).*\sinet\s+(\d+\.\d+\.\d+\.\d+)/", line)
                if match and match.group(2) != "127.0.0.1":
                    addresses.append({"interface": match.group(1), "address": match.group(2)})
        return addresses

    def status(self):
        pid = self._pid()
        version = "unknown"
        try:
            version = (self.root / "VERSION").read_text().strip()
        except OSError:
            pass
        git_commit = "unknown"
        try:
            git_commit = (self.root / "GIT_COMMIT").read_text().strip()
        except OSError:
            pass
        return {
            "version": version,
            "git_commit": git_commit,
            "running": bool(pid),
            "pid": pid,
            "watchdog_pid": self._read_int("/var/run/rv1126-object-watchdog.pid"),
            "watchdog_restarts": self._read_int("/var/run/rv1126-object-watchdog.state"),
            "addresses": self._addresses(),
            "rtsp": {"port": 8554, "main": "/live/main_stream", "sub": "/live/sub_stream"},
            "config": self.config(),
        }

    def metrics(self):
        pid = self._pid()
        result = self._process_stats(pid)
        result.update(self._device_memory())
        result.update(self._npu_metrics())
        result.update(self._temperatures())
        result.update(self._latest_video())
        result.update(self._latest_dma())
        result["cpu_total_percent"] = self._cpu_percent(pid)
        result["errcnt"] = 0
        try:
            text = Path("/proc/rkispp0").read_text()
            match = re.search(r"ErrCnt:(\d+)", text)
            result["errcnt"] = int(match.group(1)) if match else 0
        except OSError:
            pass
        return result

    def events(self, limit):
        if not self.events_file.is_file():
            return []
        result = []
        try:
            with self.events_file.open(encoding="utf-8") as handle:
                lines = collections.deque(handle, maxlen=limit)
            for line in reversed(lines):
                try:
                    result.append(json.loads(line))
                except ValueError:
                    continue
        except OSError:
            pass
        return result

    def list_files(self, kind):
        if kind not in {"recordings", "snapshots"}:
            raise ValueError("unsupported file kind")
        directory = self.root / kind
        result = []
        if not directory.is_dir():
            return result
        for path in sorted(directory.rglob("*"), key=lambda item: item.stat().st_mtime if item.exists() else 0, reverse=True):
            if not path.is_file():
                continue
            relative = path.relative_to(directory).as_posix()
            result.append({"name": relative, "size": path.stat().st_size,
                           "mtime": int(path.stat().st_mtime), "kind": kind})
            if len(result) == 200:
                break
        return result

    def resolve_download(self, kind, name):
        if kind not in {"recordings", "snapshots"}:
            raise ValueError("unsupported file kind")
        base = (self.root / kind).resolve()
        target = (base / name).resolve()
        if base not in target.parents or not target.is_file():
            raise ValueError("invalid download path")
        return target

    def log_bundle(self):
        files = {
            "application.log": Path("/tmp/rv1126-object.log"),
            "watchdog.log": Path("/tmp/rv1126-object-watchdog.log"),
            "web.log": Path("/tmp/rv1126-web.log"),
            "ota.log": self.ota_log,
            "security-audit.jsonl": self.audit_file,
            "runtime-config.json": self.runtime_config,
            "model-manifest.json": self.model_active / "MANIFEST.json",
        }
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
            for name, path in files.items():
                try:
                    data = path.read_bytes()[-2 * 1024 * 1024:]
                except OSError:
                    continue
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mtime = int(time.time())
                archive.addfile(info, io.BytesIO(data))
            diagnostics = json.dumps(
                {"status": self.status(), "metrics": self.metrics()},
                ensure_ascii=False, indent=2).encode("utf-8")
            info = tarfile.TarInfo("diagnostics.json")
            info.size = len(diagnostics)
            info.mtime = int(time.time())
            archive.addfile(info, io.BytesIO(diagnostics))
        return buffer.getvalue()

    def save_upload(self, kind, expected_sha, source, length):
        if kind not in MODEL_FILES:
            raise ValueError("unsupported model file kind")
        if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha or ""):
            raise ValueError("a SHA-256 digest is required")
        if length < 1 or length > MAX_MODEL:
            raise ValueError("invalid upload size")
        self.model_stage.mkdir(parents=True, exist_ok=True)
        target = self.model_stage / MODEL_FILES[kind]
        temporary = target.with_suffix(target.suffix + ".upload")
        digest = hashlib.sha256()
        remaining = length
        with temporary.open("wb") as handle:
            while remaining:
                chunk = source.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise ValueError("incomplete upload")
                handle.write(chunk)
                digest.update(chunk)
                remaining -= len(chunk)
            handle.flush()
            os.fsync(handle.fileno())
        actual = digest.hexdigest()
        if actual.lower() != expected_sha.lower():
            temporary.unlink(missing_ok=True)
            raise ValueError("SHA-256 mismatch")
        os.replace(temporary, target)
        self._validate_model_file(kind, target)
        return {"kind": kind, "name": target.name, "size": target.stat().st_size,
                "sha256": actual}

    @staticmethod
    def _validate_model_file(kind, path):
        if kind == "rknn" and path.stat().st_size < 1024:
            raise ValueError("RKNN model is too small")
        if kind == "labels":
            if path.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
                raise ValueError("labels must be a PNG image")
        if kind == "metadata":
            data = json.loads(path.read_text(encoding="utf-8"))
            if data.get("target") != "rv1126" or not isinstance(data.get("classes"), list) or not data["classes"]:
                raise ValueError("metadata must target rv1126 and define classes")

    def model_status(self):
        def describe(directory):
            result = {}
            for kind, name in MODEL_FILES.items():
                path = directory / name
                if path.is_file():
                    result[kind] = {"name": name, "size": path.stat().st_size,
                                    "sha256": file_sha256(path)}
            return result
        histories = []
        if self.model_history.is_dir():
            histories = [item.name for item in sorted(self.model_history.iterdir(), reverse=True) if item.is_dir()][:10]
        return {"active": describe(self.model_active), "staged": describe(self.model_stage),
                "history": histories}

    def activate_model(self):
        for kind, name in MODEL_FILES.items():
            path = self.model_stage / name
            if not path.is_file():
                raise ValueError("missing staged " + kind)
            self._validate_model_file(kind, path)
        manifest = {kind: {"file": name, "sha256": file_sha256(self.model_stage / name)}
                    for kind, name in MODEL_FILES.items()}
        manifest["activated_at"] = int(time.time())
        atomic_write(self.model_stage / "MANIFEST.json",
                     json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        self.model_history.mkdir(parents=True, exist_ok=True)
        backup = None
        with self.lock:
            self.run_controller("stop", timeout=40)
            try:
                if self.model_active.is_dir():
                    backup = self.model_history / time.strftime("%Y%m%d-%H%M%S")
                    os.replace(self.model_active, backup)
                os.replace(self.model_stage, self.model_active)
                self.model_stage.mkdir(parents=True, exist_ok=True)
                config = self.config()
                atomic_write(self.runtime_args, "\n".join(self.arguments(config)) + "\n")
                self.run_controller("start", timeout=40)
                self.wait_controller_health()
            except Exception:
                failed = self.model_history / (time.strftime("%Y%m%d-%H%M%S") + "-failed")
                if self.model_active.is_dir():
                    os.replace(self.model_active, failed)
                if backup and backup.is_dir():
                    os.replace(backup, self.model_active)
                self.model_stage.mkdir(parents=True, exist_ok=True)
                config = self.config()
                atomic_write(self.runtime_args, "\n".join(self.arguments(config)) + "\n")
                try:
                    self.run_controller("start", timeout=40)
                    self.wait_controller_health()
                except Exception as rollback_error:
                    raise RuntimeError("model activation rollback failed: {}".format(rollback_error))
                raise
        return {"activated": True, "manifest": manifest}

    def save_ota_upload(self, expected_sha, source, length):
        if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha or ""):
            raise ValueError("a SHA-256 digest is required")
        if length < 1 or length > MAX_OTA:
            raise ValueError("invalid OTA upload size")
        self.ota_bundle.parent.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(str(self.ota_bundle.parent)).free < length + 64 * 1024 * 1024:
            raise ValueError("insufficient free storage for OTA upload")
        temporary = self.ota_bundle.with_suffix(self.ota_bundle.suffix + ".upload")
        digest = hashlib.sha256()
        remaining = length
        received = 0
        started = time.monotonic()
        try:
            with temporary.open("wb") as handle:
                while remaining:
                    chunk = source.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError("incomplete OTA upload")
                    handle.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
                    received += len(chunk)
                    delay = received / OTA_UPLOAD_BYTES_PER_SECOND - (time.monotonic() - started)
                    if delay > 0:
                        time.sleep(min(delay, 0.25))
                handle.flush()
                os.fsync(handle.fileno())
            actual = digest.hexdigest()
            if not hmac.compare_digest(actual.lower(), expected_sha.lower()):
                raise ValueError("OTA SHA-256 mismatch")
            os.chmod(temporary, 0o600)
            os.replace(temporary, self.ota_bundle)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
        return {"uploaded": True, "size": length, "sha256": actual,
                "name": self.ota_bundle.name}

    def ota_command(self, action, limit=100, timeout=150):
        if action not in {"status", "history", "stage"}:
            raise ValueError("unsupported synchronous OTA action")
        if not self.ota_manager.is_file():
            raise RuntimeError("OTA manager is not installed")
        command = ["/usr/bin/python3", str(self.ota_manager)]
        if action == "history":
            command.extend(["history", "--limit", str(max(1, min(int(limit), 500)))])
        elif action == "stage":
            if not self.ota_bundle.is_file():
                raise RuntimeError("no OTA bundle has been uploaded")
            command.extend(["stage", str(self.ota_bundle)])
        else:
            command.append("status")
        code, output = run(command, timeout=timeout)
        if code:
            try:
                message = json.loads(output).get("error", output)
            except ValueError:
                message = output
            raise RuntimeError(message or "OTA command failed")
        try:
            return json.loads(output)
        except ValueError:
            raise RuntimeError("OTA manager returned invalid JSON")

    def start_ota_action(self, action):
        if action not in {"apply", "rollback"}:
            raise ValueError("unsupported OTA action")
        if not self.ota_manager.is_file():
            raise RuntimeError("OTA manager is not installed")
        self.ota_log.parent.mkdir(parents=True, exist_ok=True)
        log_handle = self.ota_log.open("ab", buffering=0)
        try:
            process = subprocess.Popen(
                ["/usr/bin/python3", str(self.ota_manager), "--delay", "2", action],
                stdin=subprocess.DEVNULL, stdout=log_handle, stderr=subprocess.STDOUT,
                start_new_session=True, close_fds=True)
        finally:
            log_handle.close()
        return {"accepted": True, "action": action, "pid": process.pid,
                "status_endpoint": "/api/ota/status"}


class ApiHandler(BaseHTTPRequestHandler):
    server_version = "RV1126Web/1.0"

    @property
    def state(self):
        return self.server.state

    @property
    def client_ip(self):
        peer = self.client_address[0]
        forwarded = self.headers.get("X-Real-IP", "").strip()
        if peer in ("127.0.0.1", "::1") and forwarded:
            try:
                return str(ipaddress.ip_address(forwarded))
            except ValueError:
                pass
        return peer

    def log_message(self, template, *args):
        print("{} {}".format(self.address_string(), template % args), flush=True)

    def _headers(self, status, content_type, length=None, download=None, extra=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Strict-Transport-Security", "max-age=31536000")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self'; script-src 'self'; img-src 'self' blob: data:; connect-src 'self'")
        if length is not None:
            self.send_header("Content-Length", str(length))
        if download:
            self.send_header("Content-Disposition", 'attachment; filename="{}"'.format(download.replace('"', '')))
        for name, value in (extra or {}).items():
            self.send_header(name, str(value))
        self.end_headers()

    def json_response(self, status, payload, extra=None):
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self._headers(status, "application/json; charset=utf-8", len(body), extra=extra)
        self.wfile.write(body)

    def error_response(self, status, message, extra=None):
        self.json_response(status, {"error": str(message)}, extra=extra)

    def _authorized(self, path):
        if path == "/api/health":
            return True
        authorized, retry_after, blocked = self.state.check_authorization(
            self.headers.get("Authorization", ""), self.client_ip)
        if not authorized:
            self.state.audit("authentication", self.client_ip,
                             "rate_limited" if blocked else "denied")
            if blocked:
                self.error_response(HTTPStatus.TOO_MANY_REQUESTS,
                                    "too many authentication failures",
                                    {"Retry-After": retry_after})
            else:
                self.error_response(HTTPStatus.UNAUTHORIZED, "authorization required")
            return False
        return True

    def _json_body(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            raise ValueError("invalid Content-Length")
        if length < 1 or length > MAX_JSON:
            raise ValueError("JSON request is empty or too large")
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        if path in STATIC_FILES:
            name, content_type = STATIC_FILES[path]
            target = self.state.root / name
            if not target.is_file():
                self.error_response(HTTPStatus.NOT_FOUND, "web asset missing")
                return
            body = target.read_bytes()
            self._headers(HTTPStatus.OK, content_type, len(body))
            self.wfile.write(body)
            return
        if not path.startswith("/api/"):
            self.error_response(HTTPStatus.NOT_FOUND, "not found")
            return
        if not self._authorized(path):
            return
        query = urllib.parse.parse_qs(parsed.query)
        try:
            if path == "/api/health":
                self.json_response(HTTPStatus.OK, {"ok": True, "service": "rv1126-web"})
            elif path == "/api/status":
                self.json_response(HTTPStatus.OK, self.state.status())
            elif path == "/api/metrics":
                self.json_response(HTTPStatus.OK, self.state.metrics())
            elif path == "/api/config":
                self.json_response(HTTPStatus.OK, self.state.config())
            elif path == "/api/events":
                limit = min(500, max(1, int(query.get("limit", ["100"])[0])))
                self.json_response(HTTPStatus.OK, {"events": self.state.events(limit)})
            elif path == "/api/files":
                kind = query.get("kind", ["recordings"])[0]
                self.json_response(HTTPStatus.OK, {"files": self.state.list_files(kind)})
            elif path == "/api/files/download":
                kind = query.get("kind", [""])[0]
                name = query.get("name", [""])[0]
                target = self.state.resolve_download(kind, name)
                self._headers(HTTPStatus.OK, "application/octet-stream", target.stat().st_size, target.name)
                with target.open("rb") as handle:
                    shutil.copyfileobj(handle, self.wfile, 1024 * 1024)
            elif path == "/api/logs/export":
                body = self.state.log_bundle()
                self._headers(HTTPStatus.OK, "application/gzip", len(body), "rv1126-logs.tar.gz")
                self.wfile.write(body)
            elif path == "/api/models":
                self.json_response(HTTPStatus.OK, self.state.model_status())
            elif path == "/api/security/status":
                self.json_response(HTTPStatus.OK, self.state.security_status())
            elif path == "/api/ota/status":
                self.json_response(HTTPStatus.OK, self.state.ota_command("status"))
            elif path == "/api/ota/history":
                limit = min(500, max(1, int(query.get("limit", ["100"])[0])))
                self.json_response(HTTPStatus.OK, self.state.ota_command("history", limit=limit))
            else:
                self.error_response(HTTPStatus.NOT_FOUND, "unknown API")
        except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
            self.error_response(HTTPStatus.BAD_REQUEST, error)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        if not self._authorized(path):
            return
        try:
            status = HTTPStatus.OK
            if path == "/api/config":
                result = self.state.save_config(self._json_body())
            elif path == "/api/restart":
                output = self.state.run_controller("restart", timeout=90)
                health = self.state.wait_controller_health()
                result = {"restarted": True, "output": output, "health": health}
            elif path == "/api/model/upload":
                query = urllib.parse.parse_qs(parsed.query)
                kind = query.get("kind", [""])[0]
                digest = query.get("sha256", [""])[0]
                length = int(self.headers.get("Content-Length", "0"))
                result = self.state.save_upload(kind, digest, self.rfile, length)
            elif path == "/api/model/activate":
                payload = self._json_body()
                if payload.get("confirm") is not True:
                    raise ValueError("model activation requires confirm=true")
                result = self.state.activate_model()
            elif path == "/api/auth/token/rotate":
                payload = self._json_body()
                if payload.get("confirm") is not True:
                    raise ValueError("token rotation requires confirm=true")
                value = self.state.rotate_token()
                result = {"rotated": True, "token": value}
            elif path == "/api/auth/token/invalidate":
                payload = self._json_body()
                if payload.get("confirm") is not True:
                    raise ValueError("token invalidation requires confirm=true")
                self.state.audit(path, self.client_ip, "success")
                self.state.invalidate_token()
                self.json_response(HTTPStatus.OK, {"invalidated": True})
                return
            elif path == "/api/ota/upload":
                query = urllib.parse.parse_qs(parsed.query)
                digest = query.get("sha256", [""])[0]
                length = int(self.headers.get("Content-Length", "0"))
                result = self.state.save_ota_upload(digest, self.rfile, length)
            elif path == "/api/ota/verify":
                result = self.state.ota_command("stage")
            elif path in {"/api/ota/apply", "/api/ota/rollback"}:
                payload = self._json_body()
                if payload.get("confirm") is not True:
                    raise ValueError("OTA action requires confirm=true")
                action = "apply" if path.endswith("/apply") else "rollback"
                result = self.state.start_ota_action(action)
                status = HTTPStatus.ACCEPTED
            else:
                self.error_response(HTTPStatus.NOT_FOUND, "unknown API")
                return
            self.state.audit(path, self.client_ip, "success")
            self.json_response(status, result)
        except json.JSONDecodeError:
            self.state.audit(path, self.client_ip, "failure", "invalid JSON")
            self.error_response(HTTPStatus.BAD_REQUEST, "invalid JSON")
        except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
            self.state.audit(path, self.client_ip, "failure", error)
            self.error_response(HTTPStatus.BAD_REQUEST, error)


class ManagementServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, state):
        super().__init__(address, ApiHandler)
        self.state = state


def main():
    parser = argparse.ArgumentParser(description="RV1126 management service")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--root", default="/root/rv1126_objects")
    parser.add_argument("--token-file", default="/root/rv1126_objects/web.token")
    arguments = parser.parse_args()
    state = ManagementState(arguments.root, arguments.token_file)
    if not state.token():
        raise SystemExit("web token is missing or empty")
    server = ManagementServer((arguments.bind, arguments.port), state)
    print("RV1126 web management listening on {}:{}".format(arguments.bind, arguments.port), flush=True)
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
