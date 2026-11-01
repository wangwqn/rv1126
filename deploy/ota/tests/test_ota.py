#!/usr/bin/env python3
import base64
import hashlib
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rv1126_ota import OTAError, OTAManager, canonical_json, version_sequence  # noqa: E402


class TestManager(OTAManager):
    def _install_boot_links(self):
        return None


class OTATest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keys_temp = tempfile.TemporaryDirectory()
        cls.key_dir = Path(cls.keys_temp.name)
        cls.private_key = cls.key_dir / "private.pem"
        subprocess.run([
            "openssl", "genpkey", "-algorithm", "RSA",
            "-pkeyopt", "rsa_keygen_bits:3072", "-out", str(cls.private_key),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        modulus_line = subprocess.check_output([
            "openssl", "rsa", "-in", str(cls.private_key), "-noout", "-modulus",
        ], text=True).strip()
        cls.key_id = "test-key-0001"
        cls.key = {
            "schema": 1,
            "algorithm": "rsa-pkcs1v15-sha256",
            "key_id": cls.key_id,
            "exponent": 65537,
            "modulus_hex": modulus_line.split("=", 1)[1].lower(),
        }

    @classmethod
    def tearDownClass(cls):
        cls.keys_temp.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.current = self.base / "current"
        self.releases = self.base / "releases"
        self.state = self.base / "state"
        self.old_release = self.releases / "v1.10.1"
        self.old_release.mkdir(parents=True)
        self._write_release(self.old_release, "1.10.1")
        self.current.symlink_to(self.old_release, target_is_directory=True)
        self.manager = TestManager(self.current, self.releases, self.state)
        self.manager.prepare()
        (self.manager.keys / (self.key_id + ".json")).write_text(
            json.dumps(self.key), encoding="utf-8")
        self.manager._link_persistent_state(self.old_release, migrate=True)
        with self.manager.active_file.open("w", encoding="utf-8") as handle:
            json.dump({"current": str(self.old_release), "previous": None,
                       "highest_sequence": version_sequence("1.10.1")}, handle)

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def _write_release(root, version):
        (root / "VERSION").write_text(version + "\n", encoding="utf-8")
        script = "#!/bin/sh\ncase \"${1:-}\" in health) echo 'Healthy: test' ;; esac\nexit 0\n"
        for name in ("rv1126-object", "rv1126-web"):
            (root / name).write_text(script, encoding="utf-8")
            os.chmod(root / name, 0o755)

    def _bundle(self, version="1.11.0", sequence=None, device="ATK-DLRV1126",
                mutate_after_sign=False, broken_controller=False):
        sequence = sequence or version_sequence(version)
        bundle = self.base / (version + ".rvota")
        work = self.base / ("work-" + version.replace(".", "-"))
        payload = work / "payload"
        payload.mkdir(parents=True, exist_ok=True)
        self._write_release(payload, version)
        if broken_controller:
            (payload / "rv1126-object").write_text("not an executable format\n",
                                                    encoding="utf-8")
        entries = []
        for target in sorted(payload.iterdir()):
            data = target.read_bytes()
            entries.append({
                "path": target.name,
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
                "mode": 0o755 if os.access(target, os.X_OK) else 0o644,
            })
        manifest = {
            "schema": 1, "product": "rv1126-vision", "device_model": device,
            "architecture": "armv7l", "version": version, "sequence": sequence,
            "min_version": "1.10.1", "key_id": self.key_id, "files": entries,
        }
        manifest_path = work / "manifest.json"
        manifest_path.write_bytes(canonical_json(manifest))
        signature = work / "signature.raw"
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(self.private_key),
                        "-out", str(signature), str(manifest_path)], check=True)
        (work / "manifest.sig").write_bytes(base64.b64encode(signature.read_bytes()))
        if mutate_after_sign:
            (payload / "VERSION").write_text("9.9.9\n", encoding="utf-8")
        with tarfile.open(bundle, "w:") as archive:
            archive.add(manifest_path, arcname="manifest.json")
            archive.add(work / "manifest.sig", arcname="manifest.sig")
            for entry in entries:
                archive.add(payload / entry["path"], arcname="payload/" + entry["path"])
        return bundle

    def test_valid_bundle_stages_and_applies(self):
        result = self.manager.stage(self._bundle())
        self.assertTrue(result["verified"])
        applied = self.manager.apply()
        self.assertTrue(applied["applied"])
        self.assertEqual((self.current / "VERSION").read_text().strip(), "1.11.0")
        self.assertEqual(self.manager.status()["highest_sequence"], version_sequence("1.11.0"))

    def test_modified_payload_is_rejected(self):
        with self.assertRaisesRegex(OTAError, "(size|digest) mismatch"):
            self.manager.stage(self._bundle(mutate_after_sign=True))

    def test_wrong_device_is_rejected(self):
        with self.assertRaisesRegex(OTAError, "device model mismatch"):
            self.manager.stage(self._bundle(device="OTHER-BOARD"))

    def test_replay_sequence_is_rejected(self):
        with self.assertRaisesRegex(OTAError, "downgrade or replay"):
            self.manager.stage(self._bundle(version="1.10.2",
                                             sequence=version_sequence("1.10.1")))

    def test_manual_rollback_switches_to_previous_release(self):
        self.manager.stage(self._bundle())
        self.manager.apply()
        result = self.manager.rollback()
        self.assertTrue(result["rolled_back"])
        self.assertEqual((self.current / "VERSION").read_text().strip(), "1.10.1")

    def test_broken_candidate_controller_restores_previous_release(self):
        self.manager.stage(self._bundle(broken_controller=True))
        with self.assertRaisesRegex(OTAError, "previous release restored"):
            self.manager.apply()
        self.assertEqual((self.current / "VERSION").read_text().strip(), "1.10.1")
        transaction = json.loads(self.manager.transaction_file.read_text())
        self.assertEqual(transaction["status"], "rolled_back")

    def test_power_loss_recovery_uses_transaction_previous(self):
        candidate = self.releases / "v1.11.0"
        candidate.mkdir()
        self._write_release(candidate, "1.11.0")
        self.manager._switch(candidate)
        self.manager.transaction_file.write_text(json.dumps({
            "status": "validating", "previous": str(self.old_release),
            "candidate": str(candidate),
        }), encoding="utf-8")
        result = self.manager.recover()
        self.assertTrue(result["recovered"])
        self.assertEqual((self.current / "VERSION").read_text().strip(), "1.10.1")


if __name__ == "__main__":
    unittest.main()
