#!/usr/bin/env python3
import hashlib
import io
import json
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rv1126_web import ManagementServer, ManagementState  # noqa: E402


class WebApiTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "app"
        self.root.mkdir()
        (self.root / "token").write_text("test-token\n", encoding="utf-8")
        self.state = ManagementState(self.root, self.root / "token")
        self.server = ManagementServer(("127.0.0.1", 0), self.state)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:{}".format(self.server.server_port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temporary.cleanup()

    def request(self, path, method="GET", body=None, authorized=True, content_type="application/json"):
        headers = {"Content-Type": content_type}
        if authorized:
            headers["Authorization"] = "Bearer test-token"
        request = urllib.request.Request(self.base + path, data=body, method=method, headers=headers)
        return urllib.request.urlopen(request, timeout=3)

    def test_health_does_not_require_token(self):
        result = json.load(self.request("/api/health", authorized=False))
        self.assertTrue(result["ok"])

    def test_protected_status_rejects_missing_token(self):
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request("/api/status", authorized=False)
        self.assertEqual(context.exception.code, 401)

    def test_config_is_validated_and_written_as_one_argument_per_line(self):
        payload = self.state.defaults()
        payload["media"].update({"bitrate": 5000000, "gop": 60, "threshold": 0.42})
        payload["analytics"]["regions"] = [
            {"name": "desk", "x": 0.1, "y": 0.2, "width": 0.3, "height": 0.4}
        ]
        body = json.dumps(payload).encode()
        result = json.load(self.request("/api/config", "POST", body))
        self.assertFalse(result["applied"])
        self.assertEqual(result["config"]["media"]["gop"], 60)
        arguments = self.state.runtime_args.read_text().splitlines()
        self.assertEqual(arguments[arguments.index("--bitrate") + 1], "5000000")
        self.assertEqual(arguments[arguments.index("--gop") + 1], "60")

    def test_invalid_configuration_is_rejected(self):
        payload = self.state.defaults()
        payload["media"]["gop"] = 0
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request("/api/config", "POST", json.dumps(payload).encode())
        self.assertEqual(context.exception.code, 400)

    def test_download_cannot_escape_archive_directory(self):
        (self.root / "recordings").mkdir()
        (self.root / "secret").write_text("no")
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request("/api/files/download?kind=recordings&name=../secret")
        self.assertEqual(context.exception.code, 400)

    def test_model_upload_verifies_sha256_and_metadata(self):
        metadata = json.dumps({"target": "rv1126", "classes": ["book"]}).encode()
        digest = hashlib.sha256(metadata).hexdigest()
        result = json.load(self.request(
            "/api/model/upload?kind=metadata&sha256=" + digest,
            "POST", metadata, content_type="application/octet-stream"))
        self.assertEqual(result["sha256"], digest)
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request("/api/model/upload?kind=metadata&sha256=" + "0" * 64,
                         "POST", metadata, content_type="application/octet-stream")
        self.assertEqual(context.exception.code, 400)

    def test_device_metrics_have_memory_and_npu_fields(self):
        metrics = self.state.metrics()
        self.assertGreater(metrics["memory_total_kb"], 0)
        self.assertIn("npu_load_percent", metrics)
        self.assertIn("npu_frequency_mhz", metrics)

    def test_health_wait_retries_during_media_warmup(self):
        attempts = []

        def controller(action, timeout=30):
            attempts.append((action, timeout))
            if len(attempts) < 3:
                raise RuntimeError("media process is still warming up")
            return "Healthy: ready"

        self.state.run_controller = controller
        result = self.state.wait_controller_health(timeout=1, interval=0)
        self.assertEqual(result, "Healthy: ready")
        self.assertEqual([item[0] for item in attempts], ["health", "health", "health"])

    def test_restart_api_waits_for_media_health(self):
        actions = []

        def controller(action, timeout=30):
            actions.append(action)
            return "restart requested"

        def wait_health(timeout=60, interval=2):
            actions.append("wait-health")
            return "Healthy: ready"

        self.state.run_controller = controller
        self.state.wait_controller_health = wait_health
        result = json.load(self.request("/api/restart", "POST", b"{}"))
        self.assertTrue(result["restarted"])
        self.assertEqual(actions, ["restart", "wait-health"])


if __name__ == "__main__":
    unittest.main()
