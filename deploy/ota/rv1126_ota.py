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
MAX_BUNDLE_BYTES = 128 * 1024 * 1024
MAX_FILES = 64
MAX_EXPANDED_BYTES = 128 * 1024 * 1024
RSA_SHA256_PREFIX = bytes.fromhex("3031300d060960864801650304020105000420")
SAFE_PATH = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]*$")
VERSION_PATTERN = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
PERSISTENT_FILES = ("runtime.args", "runtime-config.json", "web.token", "audit.jsonl")
PERSISTENT_DIRS = ("events", "recordings", "screenshots")


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


def safe_relative_path(value):
    value = str(value)
    if not SAFE_PATH.fullmatch(value) or value.startswith("/"):
        raise OTAError("unsafe OTA path: {}".format(value))
    parts = value.split("/")
    if any(item in ("", ".", "..") for item in parts):
        raise OTAError("unsafe OTA path: {}".format(value))
    return value


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

    def _current_version(self):
        try:
            return (self.current / "VERSION").read_text(encoding="utf-8").strip()
        except OSError:
            return "0.0.0"

    def _highest_sequence(self):
        active = read_json(self.active_file, {}) or {}
        return int(active.get("highest_sequence", version_sequence(self._current_version())))

    def _validate_manifest(self, manifest, enforce_upgrade=True):
        if not isinstance(manifest, dict) or manifest.get("schema") != 1:
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
        files = manifest["files"]
        if not isinstance(files, list) or not files or len(files) > MAX_FILES:
            raise OTAError("invalid OTA file list")
        seen = set()
        total = 0
        for entry in files:
            if not isinstance(entry, dict):
                raise OTAError("invalid OTA file entry")
            path = safe_relative_path(entry.get("path", ""))
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
            manifest = self.verify_bundle(bundle, True)
            version = manifest["version"]
            final = self.releases / ("v" + version)
            if final.exists():
                raise OTAError("release already exists: v" + version)
            temporary = Path(tempfile.mkdtemp(prefix=".stage-v{}-".format(version),
                                              dir=str(self.releases)))
            try:
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
                            output.flush()
                            os.fsync(output.fileno())
                        if size != int(entry["size"]) or digest.hexdigest() != entry["sha256"]:
                            raise OTAError("OTA payload digest mismatch: " + relative)
                        os.chmod(target, int(entry["mode"]))
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
                }
                atomic_json(self.staged_file, staged)
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
        for name in ("rv1126-web", "rv1126-object"):
            try:
                self._run_controller(name, "stop", check=False)
            except (OSError, subprocess.TimeoutExpired):
                pass
        for pid_file in ("/var/run/rv1126-object-watchdog.pid",
                         "/var/run/rv1126-object.pid",
                         "/var/run/rv1126-web.pid"):
            self._terminate_pid_file(pid_file)

    def _wait_health(self, timeout=60):
        deadline = time.monotonic() + timeout
        last = "health check not attempted"
        while time.monotonic() < deadline:
            try:
                media = self._run_controller("rv1126-object", "health", timeout=20)
                web = self._run_controller("rv1126-web", "health", timeout=10)
                return {"media": media, "web": web}
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
            self._run_controller("rv1126-object", "start", check=False)
            self._run_controller("rv1126-web", "start", check=False)
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
            if not self.current.is_symlink():
                raise OTAError("A/B layout is not bootstrapped")
            staged = read_json(self.staged_file)
            if not staged:
                raise OTAError("no verified OTA release is staged")
            candidate, manifest = self._verify_staged_release(staged)
            if int(manifest["sequence"]) <= self._highest_sequence():
                raise OTAError("OTA downgrade or replay rejected")
            previous = self.current.resolve()
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
                self._run_controller("rv1126-object", "start", check=False)
                self._run_controller("rv1126-web", "start", check=False)
                health = self._wait_health(60)
            except Exception as error:
                self._rollback_after_failure(previous, candidate, str(error))
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
                health = self._wait_health(60)
            except Exception as error:
                self._rollback_after_failure(current, previous, str(error))
                raise OTAError("manual rollback failed; original release restored: {}".format(error))
            active.update({"current": str(previous), "previous": str(current),
                           "updated_at": utc_timestamp()})
            atomic_json(self.active_file, active)
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
            self._history("recover", "success", release=str(previous))
            return {"recovered": True, "release": str(previous)}

    def _prune_releases(self, protected):
        candidates = []
        for item in self.releases.iterdir():
            if item.is_dir() and not item.name.startswith(".stage-") and item.resolve() not in protected:
                candidates.append(item)
        candidates.sort(key=lambda path: path.stat().st_mtime, reverse=True)
        keep_extra = max(0, self.retain - len(protected))
        for item in candidates[keep_extra:]:
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
    arguments = parser.parse_args()
    manager = build_manager(arguments)
    try:
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
        else:
            result = manager.recover()
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    except (OTAError, OSError, subprocess.TimeoutExpired, tarfile.TarError) as error:
        print(json.dumps({"error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
