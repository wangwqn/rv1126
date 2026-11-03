#!/usr/bin/env python3
"""Signed application OTA manager for the RV1126 vision service.

The OTA trust root and transaction state live outside the application release.
Only regular files authenticated by an RSA-3072/SHA-256 manifest are extracted.
boot, kernel and rootfs partitions are deliberately out of scope.
"""

import argparse
import base64
import contextlib
import fcntl
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path


PRODUCT = "rv1126-vision"
DEVICE_MODEL = "ATK-DLRV1126"
ARCHITECTURE = "armv7l"
OTA_RUNTIME_VERSION = "2.0.0"
MAX_BUNDLE_BYTES = 128 * 1024 * 1024
MAX_FILES = 64
MAX_EXPANDED_BYTES = 128 * 1024 * 1024
RSA_SHA256_PREFIX = bytes.fromhex("3031300d060960864801650304020105000420")
SAFE_PATH = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]*$")
VERSION_PATTERN = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
PERSISTENT_FILES = ("runtime.args", "runtime-config.json", "analytics.rules",
                    "web.token", "audit.jsonl")
PERSISTENT_DIRS = ("events", "recordings", "screenshots")
CHANNELS = ("stable", "beta", "development")
CHANNEL_ACCEPTS = {
    "stable": {"stable"},
    "beta": {"stable", "beta"},
    "development": set(CHANNELS),
}
COMPONENTS = ("full", "application", "model", "web")
COMPONENT_PATHS = {
    "model": {"objects.rknn", "objects.json", "labels.png"},
    "web": {"rv1126-web", "rv1126-web.py", "web-index.html",
            "web-app.js", "web-styles.css"},
    "application": {"object_image", "rv1126-object",
                    "rkmedia_object_detection_rtsp", "rv1126-recorder",
                    "rv1126-recording", "rv1126-recording.py",
                    "rv1126-time-sync", "rv1126-time-sync.py", "GIT_COMMIT"},
}


class OTAError(RuntimeError):
    pass


def utc_timestamp():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


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


def atomic_json(path, value, mode=0o600):
    atomic_write(path, json.dumps(value, ensure_ascii=False, sort_keys=True,
                                  indent=2) + "\n", mode)


def read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return default


def canonical_json(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True,
                       separators=(",", ":")) + "\n").encode("utf-8")


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_version(value):
    match = VERSION_PATTERN.fullmatch(str(value))
    if not match:
        raise OTAError("invalid semantic version: {}".format(value))
    return tuple(int(item) for item in match.groups())


def version_sequence(value):
    major, minor, patch = parse_version(value)
    if minor > 999 or patch > 999:
        raise OTAError("version components exceed OTA sequence range")
    return major * 1000000 + minor * 1000 + patch


def validate_timestamp(value, field="timestamp"):
    value = str(value)
    try:
        parsed = time.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
    except (TypeError, ValueError):
        raise OTAError("invalid {}: {}".format(field, value))
    # Refuse obviously incorrect release dates while allowing reproducible old builds.
    if parsed.tm_year < 2024 or parsed.tm_year > 2100:
        raise OTAError("invalid {}: {}".format(field, value))
    return value


def safe_relative_path(value):
    value = str(value)
    if not SAFE_PATH.fullmatch(value) or value.startswith("/"):
        raise OTAError("unsafe OTA path: {}".format(value))
    parts = value.split("/")
    if any(item in ("", ".", "..") for item in parts):
        raise OTAError("unsafe OTA path: {}".format(value))
    return value


def component_path_allowed(component, path):
    if component == "full":
        return True
    return path in COMPONENT_PATHS.get(component, set())


def load_public_key(path):
    key = read_json(path)
    if not isinstance(key, dict):
        raise OTAError("trusted public key is missing or invalid")
    if key.get("schema") != 1 or key.get("algorithm") != "rsa-pkcs1v15-sha256":
        raise OTAError("unsupported public key format")
    try:
        modulus = int(key["modulus_hex"], 16)
        exponent = int(key.get("exponent", 65537))
    except (KeyError, TypeError, ValueError):
        raise OTAError("invalid RSA public key")
    if modulus.bit_length() < 3072 or exponent < 3 or exponent % 2 == 0:
        raise OTAError("OTA key must be RSA-3072 or stronger")
    return key, modulus, exponent


def verify_rsa_sha256(message, signature, key_path):
    _key, modulus, exponent = load_public_key(key_path)
    length = (modulus.bit_length() + 7) // 8
    if len(signature) != length:
        return False
    encoded = pow(int.from_bytes(signature, "big"), exponent, modulus).to_bytes(length, "big")
    digest_info = RSA_SHA256_PREFIX + hashlib.sha256(message).digest()
    padding_length = length - len(digest_info) - 3
    if padding_length < 8:
        return False
    expected = b"\x00\x01" + b"\xff" * padding_length + b"\x00" + digest_info
    return hmac.compare_digest(encoded, expected)


class OTAManager:
    def __init__(self, current="/root/rv1126_objects",
                 releases="/root/rv1126_releases",
                 state="/userdata/rv1126-ota",
                 product=PRODUCT, device_model=DEVICE_MODEL,
                 architecture=ARCHITECTURE, retain=3):
        self.current = Path(current)
        self.releases = Path(releases)
        self.state = Path(state)
        self.product = product
        self.device_model = device_model
        self.architecture = architecture
        self.retain = max(2, int(retain))
        self.keys = self.state / "trusted_keys"
        self.incoming = self.state / "incoming"
        self.persistent = self.state / "persistent"
        self.lock_file = self.state / "ota.lock"
        self.staged_file = self.state / "staged.json"
        self.active_file = self.state / "active.json"
        self.transaction_file = self.state / "transaction.json"
        self.history_file = self.state / "history.jsonl"
        self.revocations_file = self.state / "revocations.json"
        self.policy_file = self.state / "policy.json"
        self.failure_file = self.state / "failure-state.json"
        self.release_state_file = self.state / "release-state.json"

    def _policy(self):
        policy = read_json(self.policy_file, {}) or {}
        if not isinstance(policy, dict):
            policy = {}
        channel = str(policy.get("channel", "stable"))
        if channel not in CHANNELS:
            channel = "stable"
        def integer(name, default, minimum, maximum):
            try:
                return max(minimum, min(int(policy.get(name, default)), maximum))
            except (TypeError, ValueError):
                return default
        def number(name, default, minimum, maximum):
            try:
                return max(minimum, min(float(policy.get(name, default)), maximum))
            except (TypeError, ValueError):
                return default
        return {
            "channel": channel,
            "max_failures": integer("max_failures", 3, 1, 10),
            "min_free_bytes": integer("min_free_bytes", 64 * 1024 * 1024,
                                      16 * 1024 * 1024, 2 * 1024 * 1024 * 1024),
            "max_load_per_cpu": number("max_load_per_cpu", 1.5, 0.5, 8.0),
            "stage_write_mbps": number("stage_write_mbps", 8.0, 1.0, 64.0),
        }

    def _revocations(self):
        value = read_json(self.revocations_file, {}) or {}
        return value if isinstance(value, dict) else {}

    def _failure_state(self):
        value = read_json(self.failure_file, {}) or {}
        if not isinstance(value, dict):
            value = {}
        value.setdefault("consecutive", 0)
        value.setdefault("locked", False)
        return value

    def prepare(self):
        for path in (self.state, self.keys, self.incoming, self.persistent, self.releases):
            path.mkdir(parents=True, exist_ok=True)
        for name in PERSISTENT_DIRS:
            (self.persistent / name).mkdir(parents=True, exist_ok=True)

    @contextlib.contextmanager
    def locked(self):
        self.prepare()
        with self.lock_file.open("a+") as handle:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
            yield

    def _history(self, action, result, **extra):
        record = {"time": utc_timestamp(), "action": action, "result": result}
        record.update(extra)
        self.history_file.parent.mkdir(parents=True, exist_ok=True)
        with self.history_file.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")
            handle.flush()
            os.fsync(handle.fileno())

    def history(self, limit=100):
        try:
            lines = self.history_file.read_text(encoding="utf-8").splitlines()
        except OSError:
            return []
        result = []
        for line in lines[-max(1, min(int(limit), 500)):]:
            try:
                result.append(json.loads(line))
            except ValueError:
                pass
        return result

    def install_key(self, source):
        with self.locked():
            key, _modulus, _exponent = load_public_key(source)
            key_id = str(key.get("key_id", ""))
            if not re.fullmatch(r"[A-Za-z0-9._-]{8,64}", key_id):
                raise OTAError("invalid OTA key id")
            target = self.keys / (key_id + ".json")
            if target.exists() and canonical_json(read_json(target)) != canonical_json(key):
                raise OTAError("a different public key already uses this key id")
            atomic_write(target, canonical_json(key), 0o644)
            self._history("key_install", "success", key_id=key_id)
            return {"installed": True, "key_id": key_id}

    def revoke_key(self, key_id, reason):
        with self.locked():
            if not re.fullmatch(r"[A-Za-z0-9._-]{8,64}", str(key_id)):
                raise OTAError("invalid OTA key id")
            if not (self.keys / (str(key_id) + ".json")).is_file():
                raise OTAError("trusted key does not exist")
            reason = str(reason).strip()
            if not reason or len(reason) > 256:
                raise OTAError("key revocation reason is required")
            values = self._revocations()
            values[str(key_id)] = {"revoked_at": utc_timestamp(), "reason": reason}
            atomic_json(self.revocations_file, values, 0o600)
            self._history("key_revoke", "success", key_id=str(key_id), reason=reason)
            return {"revoked": True, "key_id": str(key_id)}

    def unrevoke_key(self, key_id):
        with self.locked():
            values = self._revocations()
            if str(key_id) not in values:
                raise OTAError("key is not revoked")
            del values[str(key_id)]
            atomic_json(self.revocations_file, values, 0o600)
            self._history("key_unrevoke", "success", key_id=str(key_id))
            return {"unrevoked": True, "key_id": str(key_id)}

    def _record_apply_failure(self, reason):
        state = self._failure_state()
        try:
            consecutive = int(state.get("consecutive", 0))
        except (TypeError, ValueError):
            consecutive = 0
        state["consecutive"] = consecutive + 1
        state["last_failure_at"] = utc_timestamp()
        state["last_reason"] = str(reason)[:1024]
        if state["consecutive"] >= self._policy()["max_failures"]:
            state["locked"] = True
            state["locked_at"] = utc_timestamp()
        atomic_json(self.failure_file, state, 0o600)
        self._history("failure_guard", "locked" if state["locked"] else "failure",
                      consecutive=state["consecutive"], reason=str(reason))

    def _clear_apply_failures(self):
        state = {"consecutive": 0, "locked": False,
                 "last_success_at": utc_timestamp()}
        atomic_json(self.failure_file, state, 0o600)

    def unlock(self):
        with self.locked():
            previous = self._failure_state()
            self._clear_apply_failures()
            self._history("failure_guard", "unlocked",
                          previous_failures=int(previous.get("consecutive", 0)))
            return {"unlocked": True}

    def set_channel(self, channel):
        with self.locked():
            if channel not in CHANNELS:
                raise OTAError("invalid OTA release channel")
            current = read_json(self.policy_file, {}) or {}
            if not isinstance(current, dict):
                current = {}
            current["channel"] = channel
            atomic_json(self.policy_file, current, 0o600)
            self._history("policy", "channel_changed", channel=channel)
            return {"channel": channel}

    def _ensure_apply_allowed(self):
        failure = self._failure_state()
        if failure.get("locked"):
            raise OTAError("OTA is locked after repeated failures; local unlock is required")

    def _release_states(self):
        value = read_json(self.release_state_file, {}) or {}
        return value if isinstance(value, dict) else {}

    def _mark_release(self, release, status, **extra):
        release = str(Path(release).resolve())
        values = self._release_states()
        record = values.get(release, {}) if isinstance(values.get(release), dict) else {}
        record.update({"status": status, "updated_at": utc_timestamp()})
        if status == "verified":
            record["verified_at"] = utc_timestamp()
        record.update(extra)
        values[release] = record
        atomic_json(self.release_state_file, values, 0o600)

    def _resource_guard(self, required_bytes=0):
        policy = self._policy()
        usage = shutil.disk_usage(str(self.releases))
        required = int(required_bytes) + policy["min_free_bytes"]
        if usage.free < required:
            raise OTAError("insufficient free storage for OTA staging")
        try:
            load = os.getloadavg()[0]
            cpus = max(1, os.cpu_count() or 1)
            if load / cpus > policy["max_load_per_cpu"]:
                raise OTAError("system load is too high for OTA staging")
        except (AttributeError, OSError):
            pass

    def _current_version(self):
        try:
            return (self.current / "VERSION").read_text(encoding="utf-8").strip()
        except OSError:
            return "0.0.0"

    def _highest_sequence(self):
        active = read_json(self.active_file, {}) or {}
        return int(active.get("highest_sequence", version_sequence(self._current_version())))

    def _validate_manifest(self, manifest, enforce_upgrade=True):
        if not isinstance(manifest, dict) or manifest.get("schema") not in (1, 2):
            raise OTAError("unsupported OTA manifest schema")
        required = ("product", "device_model", "architecture", "version", "sequence",
                    "min_version", "key_id", "files")
        missing = [name for name in required if name not in manifest]
        if missing:
            raise OTAError("manifest fields missing: " + ", ".join(missing))
        if manifest["product"] != self.product:
            raise OTAError("OTA product mismatch")
        if manifest["device_model"] != self.device_model:
            raise OTAError("OTA device model mismatch")
        if manifest["architecture"] != self.architecture:
            raise OTAError("OTA architecture mismatch")
        parse_version(manifest["version"])
        parse_version(manifest["min_version"])
        if parse_version(self._current_version()) < parse_version(manifest["min_version"]):
            raise OTAError("current version is below package minimum compatible version")
        try:
            sequence = int(manifest["sequence"])
        except (TypeError, ValueError):
            raise OTAError("invalid OTA sequence")
        if sequence < 1:
            raise OTAError("invalid OTA sequence")
        if enforce_upgrade and sequence <= self._highest_sequence():
            raise OTAError("OTA downgrade or replay rejected")
        key_id = str(manifest["key_id"])
        if not re.fullmatch(r"[A-Za-z0-9._-]{8,64}", key_id):
            raise OTAError("invalid OTA key id")
        if key_id in self._revocations():
            raise OTAError("OTA signing key is revoked")
        schema = int(manifest["schema"])
        component = "full"
        remove = []
        if schema == 2:
            required_v2 = ("published_at", "channel", "component", "package_type",
                           "dependencies", "remove")
            missing_v2 = [name for name in required_v2 if name not in manifest]
            if missing_v2:
                raise OTAError("manifest fields missing: " + ", ".join(missing_v2))
            validate_timestamp(manifest["published_at"], "published_at")
            channel = str(manifest["channel"])
            if channel not in CHANNELS:
                raise OTAError("invalid OTA release channel")
            if channel not in CHANNEL_ACCEPTS[self._policy()["channel"]]:
                raise OTAError("OTA release channel is not accepted by device policy")
            component = str(manifest["component"])
            if component not in COMPONENTS:
                raise OTAError("invalid OTA component")
            package_type = str(manifest["package_type"])
            if package_type not in ("full", "overlay"):
                raise OTAError("invalid OTA package type")
            if component != "full" and package_type != "overlay":
                raise OTAError("component OTA must use overlay package type")
            if package_type == "overlay":
                parse_version(manifest.get("base_version"))
                if manifest["base_version"] != self._current_version():
                    raise OTAError("OTA overlay base version mismatch")
            dependencies = manifest["dependencies"]
            if not isinstance(dependencies, dict) or len(dependencies) > 32:
                raise OTAError("invalid OTA dependencies")
            available = {"ota-runtime": OTA_RUNTIME_VERSION,
                         "application": self._current_version()}
            configured = read_json(self.state / "dependencies.json", {}) or {}
            if isinstance(configured, dict):
                available.update({str(k): str(v) for k, v in configured.items()})
            for name, minimum in dependencies.items():
                if not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", str(name)):
                    raise OTAError("invalid OTA dependency name")
                parse_version(minimum)
                if name not in available or parse_version(available[name]) < parse_version(minimum):
                    raise OTAError("OTA dependency is not satisfied: " + str(name))
            remove = manifest["remove"]
            if not isinstance(remove, list) or len(remove) > MAX_FILES:
                raise OTAError("invalid OTA remove list")
            remove = [safe_relative_path(path) for path in remove]
            if len(set(remove)) != len(remove):
                raise OTAError("duplicate OTA remove path")
            for path in remove:
                if not component_path_allowed(component, path):
                    raise OTAError("path is outside OTA component: " + path)
        files = manifest["files"]
        if not isinstance(files, list) or not files or len(files) > MAX_FILES:
            raise OTAError("invalid OTA file list")
        seen = set()
        total = 0
        for entry in files:
            if not isinstance(entry, dict):
                raise OTAError("invalid OTA file entry")
            path = safe_relative_path(entry.get("path", ""))
            if not component_path_allowed(component, path):
                raise OTAError("path is outside OTA component: " + path)
            if path in remove:
                raise OTAError("OTA path cannot be both replaced and removed: " + path)
            if path in seen:
                raise OTAError("duplicate OTA file: " + path)
            seen.add(path)
            digest = str(entry.get("sha256", ""))
            if not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise OTAError("invalid file digest: " + path)
            try:
                size = int(entry["size"])
                mode = int(entry["mode"])
            except (KeyError, TypeError, ValueError):
                raise OTAError("invalid size or mode: " + path)
            if size < 0 or mode not in (0o644, 0o755):
                raise OTAError("invalid size or mode: " + path)
            total += size
        if total > MAX_EXPANDED_BYTES:
            raise OTAError("expanded OTA package is too large")
        return manifest

    def verify_bundle(self, bundle, enforce_upgrade=True):
        bundle = Path(bundle)
        if not bundle.is_file() or bundle.stat().st_size < 1:
            raise OTAError("OTA bundle is missing")
        if bundle.stat().st_size > MAX_BUNDLE_BYTES:
            raise OTAError("OTA bundle is too large")
        try:
            # The production Buildroot Python omits zlib. Keep the signed
            # container uncompressed so verification needs no optional module.
            archive = tarfile.open(str(bundle), "r:")
        except (tarfile.TarError, OSError) as error:
            raise OTAError("invalid OTA archive: {}".format(error))
        with archive:
            members = archive.getmembers()
            if len(members) > MAX_FILES + 2:
                raise OTAError("OTA archive contains too many entries")
            by_name = {}
            for member in members:
                if not member.isfile():
                    raise OTAError("OTA archive may contain regular files only")
                safe_relative_path(member.name)
                if member.name in by_name:
                    raise OTAError("duplicate archive entry: " + member.name)
                by_name[member.name] = member
            if "manifest.json" not in by_name or "manifest.sig" not in by_name:
                raise OTAError("OTA manifest or signature is missing")
            if by_name["manifest.json"].size > 256 * 1024:
                raise OTAError("OTA manifest is too large")
            if by_name["manifest.sig"].size > 8 * 1024:
                raise OTAError("OTA signature is too large")
            manifest_raw = archive.extractfile(by_name["manifest.json"]).read()
            try:
                manifest = json.loads(manifest_raw.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                raise OTAError("invalid OTA manifest JSON")
            if canonical_json(manifest) != manifest_raw:
                raise OTAError("OTA manifest is not canonical")
            self._validate_manifest(manifest, enforce_upgrade)
            signature_text = archive.extractfile(by_name["manifest.sig"]).read()
            try:
                signature = base64.b64decode(signature_text, validate=True)
            except (ValueError, TypeError):
                raise OTAError("invalid OTA signature encoding")
            key_path = self.keys / (manifest["key_id"] + ".json")
            trusted_key, _modulus, _exponent = load_public_key(key_path)
            if trusted_key.get("key_id") != manifest["key_id"]:
                raise OTAError("trusted key id mismatch")
            if manifest.get("schema") == 2:
                published = manifest["published_at"]
                if trusted_key.get("valid_from") and published < validate_timestamp(
                        trusted_key["valid_from"], "key valid_from"):
                    raise OTAError("OTA package predates signing key validity")
                if trusted_key.get("valid_until") and published > validate_timestamp(
                        trusted_key["valid_until"], "key valid_until"):
                    raise OTAError("OTA package exceeds signing key validity")
            if not verify_rsa_sha256(manifest_raw, signature, key_path):
                raise OTAError("OTA signature verification failed")
            expected = {"manifest.json", "manifest.sig"}
            for entry in manifest["files"]:
                name = "payload/" + entry["path"]
                expected.add(name)
                member = by_name.get(name)
                if member is None or member.size != int(entry["size"]):
                    raise OTAError("OTA payload size mismatch: " + entry["path"])
            if set(by_name) != expected:
                raise OTAError("OTA archive contains undeclared files")
            return manifest

    def _link_persistent_state(self, release, migrate=False):
        release = Path(release)
        for name in PERSISTENT_FILES:
            source = release / name
            target = self.persistent / name
            if migrate and source.exists() and not source.is_symlink() and not target.exists():
                shutil.move(str(source), str(target))
            if source.exists() or source.is_symlink():
                if source.is_dir() and not source.is_symlink():
                    shutil.rmtree(source)
                else:
                    source.unlink()
            if name == "web.token" and not target.exists():
                atomic_write(target, secrets.token_hex(32) + "\n", 0o600)
            source.symlink_to(target)
        for name in PERSISTENT_DIRS:
            source = release / name
            target = self.persistent / name
            if migrate and source.exists() and not source.is_symlink() and not target.exists():
                shutil.move(str(source), str(target))
            target.mkdir(parents=True, exist_ok=True)
            if source.exists() or source.is_symlink():
                if source.is_dir() and not source.is_symlink():
                    shutil.rmtree(source)
                else:
                    source.unlink()
            source.symlink_to(target, target_is_directory=True)

    def stage(self, bundle):
        with self.locked():
            self._resource_guard(Path(bundle).stat().st_size * 2)
            manifest = self.verify_bundle(bundle, True)
            version = manifest["version"]
            final = self.releases / ("v" + version)
            if final.exists():
                raise OTAError("release already exists: v" + version)
            temporary = Path(tempfile.mkdtemp(prefix=".stage-v{}-".format(version),
                                              dir=str(self.releases)))
            try:
                overlay = manifest.get("schema") == 2 and manifest.get("package_type") == "overlay"
                base_release = self.current.resolve() if overlay else None
                if overlay:
                    shutil.rmtree(temporary)
                    shutil.copytree(str(base_release), str(temporary), symlinks=True)
                    for relative in manifest.get("remove", []):
                        target = temporary / relative
                        if target.is_dir() and not target.is_symlink():
                            shutil.rmtree(target)
                        elif target.exists() or target.is_symlink():
                            target.unlink()
                rate = self._policy()["stage_write_mbps"] * 1024 * 1024
                written = 0
                started = time.monotonic()
                with tarfile.open(str(bundle), "r:") as archive:
                    for entry in manifest["files"]:
                        relative = entry["path"]
                        target = temporary / relative
                        target.parent.mkdir(parents=True, exist_ok=True)
                        digest = hashlib.sha256()
                        size = 0
                        source = archive.extractfile("payload/" + relative)
                        with target.open("wb") as output:
                            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                                size += len(chunk)
                                digest.update(chunk)
                                output.write(chunk)
                                written += len(chunk)
                                delay = written / rate - (time.monotonic() - started)
                                if delay > 0:
                                    time.sleep(min(delay, 0.25))
                            output.flush()
                            os.fsync(output.fileno())
                        if size != int(entry["size"]) or digest.hexdigest() != entry["sha256"]:
                            raise OTAError("OTA payload digest mismatch: " + relative)
                        os.chmod(target, int(entry["mode"]))
                if overlay:
                    atomic_write(temporary / "VERSION", version + "\n", 0o644)
                elif (temporary / "VERSION").read_text(encoding="utf-8").strip() != version:
                    raise OTAError("payload VERSION does not match manifest")
                atomic_json(temporary / "OTA_MANIFEST.json", manifest, 0o644)
                with tarfile.open(str(bundle), "r:") as archive:
                    signature_data = archive.extractfile("manifest.sig").read()
                atomic_write(temporary / "OTA_MANIFEST.sig", signature_data, 0o644)
                self._link_persistent_state(temporary)
                os.rename(temporary, final)
                staged = {
                    "version": version,
                    "sequence": int(manifest["sequence"]),
                    "release": str(final),
                    "bundle_sha256": sha256_file(bundle),
                    "verified_at": utc_timestamp(),
                    "component": manifest.get("component", "full"),
                    "channel": manifest.get("channel", "legacy"),
                    "base_release": str(base_release) if base_release else None,
                }
                atomic_json(self.staged_file, staged)
                self._mark_release(final, "staged", version=version,
                                   sequence=int(manifest["sequence"]),
                                   component=manifest.get("component", "full"))
                self._history("verify", "success", version=version,
                              sequence=int(manifest["sequence"]))
                return {"verified": True, "manifest": manifest, "staged": staged}
            except Exception:
                shutil.rmtree(temporary, ignore_errors=True)
                raise

    def _run_controller(self, name, action, timeout=45, check=True):
        controller = self.current / name
        environment = os.environ.copy()
        if name == "rv1126-web":
            # Security policy belongs to the stable OTA runtime, not only to
            # the selected release.  This also protects rollback to a legacy
            # controller whose historical default was 0.0.0.0:8080.
            environment["RV1126_WEB_BIND"] = "127.0.0.1"
        result = subprocess.run([str(controller), action], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=timeout,
                                check=False, env=environment)
        if check and result.returncode:
            raise OTAError("{} {} failed: {}".format(name, action, result.stdout.strip()))
        return result.stdout.strip()

    def _run_optional_controller(self, name, action, timeout=45, check=True):
        controller = self.current / name
        if not controller.is_file():
            return ""
        return self._run_controller(name, action, timeout, check)

    @staticmethod
    def _terminate_pid_file(path):
        path = Path(path)
        try:
            pid = int(path.read_text().strip())
        except (OSError, ValueError):
            # On a read-only /var/run, unlink(missing_ok=True) may still fail
            # with EROFS even when the path is absent.  Avoid the syscall when
            # there is nothing to remove.
            if path.exists() or path.is_symlink():
                path.unlink()
            return
        if pid <= 1 or not Path("/proc/{}".format(pid)).is_dir():
            if path.exists() or path.is_symlink():
                path.unlink()
            return
        try:
            os.kill(pid, 15)
        except ProcessLookupError:
            if path.exists() or path.is_symlink():
                path.unlink()
            return
        deadline = time.monotonic() + 10
        while Path("/proc/{}".format(pid)).is_dir() and time.monotonic() < deadline:
            time.sleep(0.2)
        if Path("/proc/{}".format(pid)).is_dir():
            try:
                os.kill(pid, 9)
            except ProcessLookupError:
                pass
        if path.exists() or path.is_symlink():
            path.unlink()

    def _stop_services(self):
        # A malformed candidate may not contain executable controllers. Never
        # make rollback depend solely on code from the candidate release.
        for name in ("rv1126-recording", "rv1126-time-sync",
                     "rv1126-web", "rv1126-object"):
            try:
                self._run_optional_controller(name, "stop", check=False)
            except (OSError, subprocess.TimeoutExpired):
                pass
        for pid_file in ("/var/run/rv1126-object-watchdog.pid",
                         "/var/run/rv1126-object.pid",
                         "/var/run/rv1126-web.pid",
                         "/var/run/rv1126-recording.pid",
                         "/var/run/rv1126-time-sync.pid"):
            self._terminate_pid_file(pid_file)

    def _wait_health(self, timeout=60):
        deadline = time.monotonic() + timeout
        last = "health check not attempted"
        while time.monotonic() < deadline:
            try:
                media = self._run_controller("rv1126-object", "health", timeout=20)
                web = self._run_controller("rv1126-web", "health", timeout=10)
                recording = self._run_optional_controller(
                    "rv1126-recording", "health", timeout=10)
                return {"media": media, "web": web, "recording": recording}
            except (OTAError, OSError, subprocess.TimeoutExpired) as error:
                last = str(error)
            time.sleep(2)
        raise OTAError("release did not become healthy within {}s: {}".format(timeout, last))

    def _switch(self, target):
        target = Path(target).resolve()
        if target.parent != self.releases.resolve() or not target.is_dir():
            raise OTAError("release target is outside the release store")
        temporary = self.current.parent / (".rv1126-current-" + secrets.token_hex(6))
        temporary.symlink_to(target, target_is_directory=True)
        try:
            os.replace(temporary, self.current)
        finally:
            if temporary.exists() or temporary.is_symlink():
                temporary.unlink()
        os.sync()

    def _install_boot_links(self):
        for link, target in (("/etc/init.d/S97rv1126-ota-recover",
                              "/root/rv1126_ota/rv1126-ota-recover"),
                             ("/etc/init.d/S98rv1126-web",
                              "/root/rv1126_ota/rv1126-web-secure"),
                             ("/etc/init.d/S99rv1126-object",
                              "/root/rv1126_objects/rv1126-object")):
            path = Path(link)
            temporary = path.parent / ("." + path.name + ".new")
            if temporary.exists() or temporary.is_symlink():
                temporary.unlink()
            temporary.symlink_to(target)
            os.replace(temporary, path)
        # Recovery must run before any controller reached through the mutable
        # /root/rv1126_objects link.  Recording must start after the media
        # service has opened RTSP port 8554.
        for link, target in (("/etc/init.d/S97zrv1126-time-sync",
                              "/root/rv1126_objects/rv1126-time-sync"),
                             ("/etc/init.d/S99zrv1126-recording",
                              "/root/rv1126_objects/rv1126-recording")):
            path = Path(link)
            temporary = path.parent / ("." + path.name + ".new")
            if temporary.exists() or temporary.is_symlink():
                temporary.unlink()
            if Path(target).is_file():
                temporary.symlink_to(target)
                os.replace(temporary, path)
            elif path.exists() or path.is_symlink():
                path.unlink()
        for legacy in ("/etc/init.d/S95rv1126-time-sync",
                       "/etc/init.d/S96rv1126-recording"):
            path = Path(legacy)
            if path.is_symlink():
                path.unlink()

    def bootstrap(self):
        with self.locked():
            if self.current.is_symlink():
                current_release = self.current.resolve()
            else:
                if not self.current.is_dir():
                    raise OTAError("current application directory is missing")
                version = self._current_version()
                parse_version(version)
                current_release = self.releases / ("v" + version)
                if current_release.exists():
                    raise OTAError("bootstrap release already exists")
                self._stop_services()
                os.rename(self.current, current_release)
                self._link_persistent_state(current_release, migrate=True)
                self.current.symlink_to(current_release, target_is_directory=True)
            self._install_boot_links()
            version = (current_release / "VERSION").read_text().strip()
            active = read_json(self.active_file, {}) or {}
            active.update({
                "current": str(current_release),
                "previous": active.get("previous"),
                "highest_sequence": max(int(active.get("highest_sequence", 0)),
                                        version_sequence(version)),
                "updated_at": utc_timestamp(),
            })
            atomic_json(self.active_file, active)
            self._mark_release(current_release, "verified", version=version,
                               sequence=version_sequence(version))
            self._run_controller("rv1126-object", "start", check=False)
            self._run_controller("rv1126-web", "start", check=False)
            self._run_optional_controller("rv1126-time-sync", "start", check=False)
            self._run_optional_controller("rv1126-recording", "start", check=False)
            health = self._wait_health(60)
            self._history("bootstrap", "success", version=version)
            return {"bootstrapped": True, "active": active, "health": health}

    def _verify_staged_release(self, staged):
        release = Path(staged.get("release", ""))
        if release.parent.resolve() != self.releases.resolve() or not release.is_dir():
            raise OTAError("staged release is missing")
        manifest = read_json(release / "OTA_MANIFEST.json")
        self._validate_manifest(manifest, False)
        if manifest["version"] != staged.get("version") or int(manifest["sequence"]) != int(staged.get("sequence", 0)):
            raise OTAError("staged metadata mismatch")
        for entry in manifest["files"]:
            target = release / safe_relative_path(entry["path"])
            if not target.is_file() or target.stat().st_size != int(entry["size"]):
                raise OTAError("staged file missing: " + entry["path"])
            if sha256_file(target) != entry["sha256"]:
                raise OTAError("staged file digest mismatch: " + entry["path"])
        return release, manifest

    def _rollback_after_failure(self, previous, candidate, reason):
        try:
            self._stop_services()
            self._switch(previous)
            self._install_boot_links()
            self._run_controller("rv1126-object", "start", check=False)
            self._run_controller("rv1126-web", "start", check=False)
            self._run_optional_controller("rv1126-time-sync", "start", check=False)
            self._run_optional_controller("rv1126-recording", "start", check=False)
            health = self._wait_health(60)
            atomic_json(self.transaction_file, {
                "status": "rolled_back", "previous": str(previous),
                "failed": str(candidate), "reason": reason, "time": utc_timestamp(),
            })
            self._history("apply", "rolled_back", candidate=str(candidate), reason=reason)
            return health
        except Exception as rollback_error:
            atomic_json(self.transaction_file, {
                "status": "rollback_failed", "previous": str(previous),
                "failed": str(candidate), "reason": reason,
                "rollback_error": str(rollback_error), "time": utc_timestamp(),
            })
            raise OTAError("upgrade failed and rollback failed: {}; {}".format(reason, rollback_error))

    def apply(self):
        with self.locked():
            self._ensure_apply_allowed()
            if not self.current.is_symlink():
                raise OTAError("A/B layout is not bootstrapped")
            staged = read_json(self.staged_file)
            if not staged:
                raise OTAError("no verified OTA release is staged")
            candidate, manifest = self._verify_staged_release(staged)
            if int(manifest["sequence"]) <= self._highest_sequence():
                raise OTAError("OTA downgrade or replay rejected")
            previous = self.current.resolve()
            if staged.get("base_release") and Path(staged["base_release"]).resolve() != previous:
                raise OTAError("staged overlay base release is no longer active")
            transaction = {
                "status": "switching", "previous": str(previous),
                "candidate": str(candidate), "version": manifest["version"],
                "sequence": int(manifest["sequence"]), "started_at": utc_timestamp(),
            }
            atomic_json(self.transaction_file, transaction)
            try:
                self._stop_services()
                self._switch(candidate)
                self._install_boot_links()
                transaction["status"] = "validating"
                atomic_json(self.transaction_file, transaction)
                # Explicit, one-shot physical power-loss test hook. It is
                # disabled unless both a root-owned marker and an environment
                # delay are present; normal OTA can never enter this pause.
                test_marker = self.state / "allow-power-cut-test"
                try:
                    test_hold = int(os.environ.get("RV1126_OTA_POWER_CUT_HOLD_SECONDS", "0"))
                except ValueError:
                    test_hold = 0
                if test_marker.is_file() and 1 <= test_hold <= 300:
                    test_marker.unlink()
                    self._history("power_cut_test", "armed", hold_seconds=test_hold,
                                  version=manifest["version"])
                    time.sleep(test_hold)
                self._run_controller("rv1126-object", "start", check=False)
                self._run_controller("rv1126-web", "start", check=False)
                self._run_optional_controller("rv1126-time-sync", "start", check=False)
                self._run_optional_controller("rv1126-recording", "start", check=False)
                health = self._wait_health(60)
            except Exception as error:
                try:
                    self._rollback_after_failure(previous, candidate, str(error))
                finally:
                    self._mark_release(candidate, "failed", reason=str(error))
                    self._record_apply_failure(str(error))
                raise OTAError("upgrade failed; previous release restored: {}".format(error))
            active = {
                "current": str(candidate), "previous": str(previous),
                "highest_sequence": int(manifest["sequence"]),
                "updated_at": utc_timestamp(),
            }
            atomic_json(self.active_file, active)
            transaction.update({"status": "committed", "committed_at": utc_timestamp()})
            atomic_json(self.transaction_file, transaction)
            self.staged_file.unlink(missing_ok=True)
            self._mark_release(candidate, "verified", version=manifest["version"],
                               sequence=int(manifest["sequence"]),
                               channel=manifest.get("channel", "legacy"),
                               component=manifest.get("component", "full"))
            self._mark_release(previous, "verified")
            self._clear_apply_failures()
            self._history("apply", "success", version=manifest["version"],
                          sequence=int(manifest["sequence"]))
            self._prune_releases({candidate.resolve(), previous.resolve()})
            return {"applied": True, "active": active, "health": health}

    def rollback(self):
        with self.locked():
            active = read_json(self.active_file, {}) or {}
            previous = Path(active.get("previous", ""))
            current = self.current.resolve() if self.current.is_symlink() else None
            if not current or previous.parent.resolve() != self.releases.resolve() or not previous.is_dir():
                raise OTAError("no verified previous release is available")
            atomic_json(self.transaction_file, {
                "status": "switching", "previous": str(current),
                "candidate": str(previous), "manual_rollback": True,
                "started_at": utc_timestamp(),
            })
            try:
                self._stop_services()
                self._switch(previous)
                self._install_boot_links()
                self._run_controller("rv1126-object", "start", check=False)
                self._run_controller("rv1126-web", "start", check=False)
                self._run_optional_controller("rv1126-time-sync", "start", check=False)
                self._run_optional_controller("rv1126-recording", "start", check=False)
                health = self._wait_health(60)
            except Exception as error:
                self._rollback_after_failure(current, previous, str(error))
                raise OTAError("manual rollback failed; original release restored: {}".format(error))
            active.update({"current": str(previous), "previous": str(current),
                           "updated_at": utc_timestamp()})
            atomic_json(self.active_file, active)
            self._mark_release(previous, "verified")
            atomic_json(self.transaction_file, {"status": "committed",
                        "manual_rollback": True, "committed_at": utc_timestamp()})
            self._history("rollback", "success", release=str(previous))
            return {"rolled_back": True, "active": active, "health": health}

    def recover(self):
        with self.locked():
            transaction = read_json(self.transaction_file, {}) or {}
            if transaction.get("status") not in ("switching", "validating"):
                return {"recovered": False, "reason": "no interrupted transaction"}
            previous = Path(transaction.get("previous", ""))
            if previous.parent.resolve() != self.releases.resolve() or not previous.is_dir():
                raise OTAError("interrupted OTA has no valid rollback release")
            self._switch(previous)
            self._install_boot_links()
            transaction.update({"status": "power_loss_rolled_back",
                                "recovered_at": utc_timestamp()})
            atomic_json(self.transaction_file, transaction)
            active = read_json(self.active_file, {}) or {}
            active.update({"current": str(previous), "updated_at": utc_timestamp()})
            atomic_json(self.active_file, active)
            candidate = transaction.get("candidate")
            if candidate:
                self._mark_release(candidate, "failed", reason="power loss during OTA")
            self._mark_release(previous, "verified")
            self._history("recover", "success", release=str(previous))
            return {"recovered": True, "release": str(previous)}

    def _prune_releases(self, protected):
        states = self._release_states()
        verified = []
        failed = []
        for item in self.releases.iterdir():
            if item.is_dir() and not item.name.startswith(".stage-") and item.resolve() not in protected:
                record = states.get(str(item.resolve()), {})
                if record.get("status") == "verified":
                    verified.append((record.get("verified_at", ""), item))
                elif record.get("status") == "failed":
                    failed.append((record.get("updated_at", ""), item))
                # Unknown legacy directories are deliberately retained until an
                # administrator or successful health check marks them verified.
        verified.sort(key=lambda value: value[0], reverse=True)
        failed.sort(key=lambda value: value[0], reverse=True)
        keep_extra = max(0, self.retain - len(protected))
        for _stamp, item in verified[keep_extra:]:
            if (item / "VERSION").is_file():
                shutil.rmtree(item)
        # Keep the most recent failed candidate for forensics; older failed
        # candidates are not rollback targets and may be reclaimed.
        for _stamp, item in failed[1:]:
            if (item / "VERSION").is_file():
                shutil.rmtree(item)

    def status(self):
        current = None
        if self.current.is_symlink():
            current = str(self.current.resolve())
        elif self.current.is_dir():
            current = str(self.current)
        return {
            "product": self.product,
            "device_model": self.device_model,
            "architecture": self.architecture,
            "current": current,
            "current_version": self._current_version(),
            "ab_bootstrapped": self.current.is_symlink(),
            "highest_sequence": self._highest_sequence(),
            "staged": read_json(self.staged_file),
            "active": read_json(self.active_file),
            "transaction": read_json(self.transaction_file),
            "trusted_keys": sorted(path.stem for path in self.keys.glob("*.json")),
            "revoked_keys": self._revocations(),
            "policy": self._policy(),
            "failure_guard": self._failure_state(),
            "release_state": self._release_states(),
            "ota_runtime_version": OTA_RUNTIME_VERSION,
        }


def build_manager(arguments):
    return OTAManager(arguments.current, arguments.releases, arguments.state,
                      arguments.product, arguments.device_model,
                      arguments.architecture, arguments.retain)


def main():
    parser = argparse.ArgumentParser(description="RV1126 signed application OTA manager")
    parser.add_argument("--current", default="/root/rv1126_objects")
    parser.add_argument("--releases", default="/root/rv1126_releases")
    parser.add_argument("--state", default="/userdata/rv1126-ota")
    parser.add_argument("--product", default=PRODUCT)
    parser.add_argument("--device-model", default=DEVICE_MODEL)
    parser.add_argument("--architecture", default=ARCHITECTURE)
    parser.add_argument("--retain", type=int, default=3)
    parser.add_argument("--delay", type=int, default=0,
                        help="delay a destructive action so an HTTP response can flush")
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("status")
    subparsers.add_parser("history").add_argument("--limit", type=int, default=100)
    stage_parser = subparsers.add_parser("stage")
    stage_parser.add_argument("bundle")
    subparsers.add_parser("bootstrap")
    subparsers.add_parser("apply")
    subparsers.add_parser("rollback")
    subparsers.add_parser("recover")
    install_key_parser = subparsers.add_parser("key-install")
    install_key_parser.add_argument("public_key")
    revoke_parser = subparsers.add_parser("key-revoke")
    revoke_parser.add_argument("key_id")
    revoke_parser.add_argument("--reason", required=True)
    unrevoke_parser = subparsers.add_parser("key-unrevoke")
    unrevoke_parser.add_argument("key_id")
    unlock_parser = subparsers.add_parser("unlock")
    unlock_parser.add_argument("--confirm", action="store_true")
    channel_parser = subparsers.add_parser("set-channel")
    channel_parser.add_argument("channel", choices=CHANNELS)
    arguments = parser.parse_args()
    manager = build_manager(arguments)
    try:
        if arguments.command == "stage":
            try:
                os.nice(10)
            except OSError:
                pass
        if arguments.delay < 0 or arguments.delay > 30:
            raise OTAError("delay must be between 0 and 30 seconds")
        if arguments.delay and arguments.command in ("apply", "rollback"):
            time.sleep(arguments.delay)
        if arguments.command == "status":
            result = manager.status()
        elif arguments.command == "history":
            result = {"history": manager.history(arguments.limit)}
        elif arguments.command == "stage":
            result = manager.stage(arguments.bundle)
        elif arguments.command == "bootstrap":
            result = manager.bootstrap()
        elif arguments.command == "apply":
            result = manager.apply()
        elif arguments.command == "rollback":
            result = manager.rollback()
        elif arguments.command == "recover":
            result = manager.recover()
        elif arguments.command == "key-install":
            result = manager.install_key(arguments.public_key)
        elif arguments.command == "key-revoke":
            result = manager.revoke_key(arguments.key_id, arguments.reason)
        elif arguments.command == "key-unrevoke":
            result = manager.unrevoke_key(arguments.key_id)
        elif arguments.command == "unlock":
            if not arguments.confirm:
                raise OTAError("unlock requires --confirm")
            result = manager.unlock()
        else:
            result = manager.set_channel(arguments.channel)
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    except (OTAError, OSError, subprocess.TimeoutExpired, tarfile.TarError) as error:
        print(json.dumps({"error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
