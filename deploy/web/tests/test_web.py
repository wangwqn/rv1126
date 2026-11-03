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

    def request(self, path, method="GET", body=None, authorized=True,
                content_type="application/json", token="test-token"):
        headers = {"Content-Type": content_type}
        if authorized:
            headers["Authorization"] = "Bearer " + token
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
        payload["recording"].update({"max_storage_mb": 256, "event_pre_seconds": 15})
        payload["analytics"]["regions"] = [
            {"name": "desk", "x": 0.1, "y": 0.2, "width": 0.3, "height": 0.4}
        ]
        body = json.dumps(payload).encode()
        result = json.load(self.request("/api/config", "POST", body))
        self.assertFalse(result["applied"])
        self.assertEqual(result["config"]["media"]["gop"], 60)
        self.assertEqual(result["config"]["recording"]["max_storage_mb"], 256)
        self.assertEqual(result["config"]["recording"]["event_pre_seconds"], 15)
        arguments = self.state.runtime_args.read_text().splitlines()
        self.assertEqual(arguments[arguments.index("--bitrate") + 1], "5000000")
        self.assertEqual(arguments[arguments.index("--gop") + 1], "60")
        self.assertNotIn("--analytics-rules", arguments)
        self.assertNotIn("--event-log", arguments)
        rules = self.state.analytics_rules.read_text(encoding="utf-8")
        self.assertIn("region 6465736b 0.10000000 0.20000000 0.30000000 0.40000000", rules)

    def test_invalid_configuration_is_rejected(self):
        payload = self.state.defaults()
        payload["media"]["gop"] = 0
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request("/api/config", "POST", json.dumps(payload).encode())
        self.assertEqual(context.exception.code, 400)

    def test_invalid_recording_configuration_is_rejected(self):
        payload = self.state.defaults()
        payload["recording"]["segment_seconds"] = 1
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request("/api/config", "POST", json.dumps(payload).encode())
        self.assertEqual(context.exception.code, 400)

    def test_degenerate_analytics_line_is_rejected(self):
        payload = self.state.defaults()
        payload["analytics"]["lines"] = [
            {"name": "bad", "x1": 0.5, "y1": 0.5, "x2": 0.5, "y2": 0.5}
        ]
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

    def test_dma_metrics_parse_latest_pipeline_audit(self):
        log = self.root / "dma.log"
        log.write_text(
            "dma_buf handles(live=2, peak=8, acquired=100, released=98, "
            "overdue=0, max_live_ms=3.00) fd(live=2, peak=8, missing=80, "
            "required_error=0, release_error=0) cpu(map=90, active=0, peak=1, bytes=4096, "
            "nv12=0, bgr=10, encoded=80, sync_error=0) "
            "process_dma(fds=49, bytes=123456) kernel_dma(objects=52, bytes=234567)\n"
            "dma_wait_ms p50/p95/p99/max ai_rga2(get=1/2/3/4)\n"
            "dma_graph main=VI_NV12_DMABUF->RGA0_NV12->VENC0\n",
            encoding="utf-8")
        metrics = self.state._latest_dma(log)
        self.assertEqual(metrics["dma_live_handles"], 2)
        self.assertEqual(metrics["dma_nv12_cpu_maps"], 0)
        self.assertEqual(metrics["dma_bgr_cpu_maps"], 10)
        self.assertEqual(metrics["dma_encoded_cpu_maps"], 80)
        self.assertEqual(metrics["dma_process_fds"], 49)
        self.assertEqual(metrics["dma_kernel_objects"], 52)
        self.assertEqual(metrics["dma_missing_fds"], 80)
        self.assertEqual(metrics["dma_required_fd_errors"], 0)

    def test_event_metrics_parse_latest_pipeline_summary(self):
        log = self.root / "events.log"
        log.write_text(
            "video main(venc=300, pts_gap=0, pts_reset=0) sub(venc=300, pts_gap=0, "
            "pts_reset=0) ai(source=30, sampled=30, completed=30, stale_drop=0) "
            "osd(boxes=2, truncated=0) rtsp(sent=600, queue_high_water=2/8, "
            "shutdown_abandoned=0) audio(captured=500, sent=500, queue_drop=0, "
            "recoveries=0) events(processed=30, generated=3, written=3, dropped=0, "
            "write_error=0, tracks=2, regions=1, lines=1)\n", encoding="utf-8")
        metrics = self.state._latest_video(log)
        self.assertEqual(metrics["event_generated"], 3)
        self.assertEqual(metrics["event_written"], 3)
        self.assertEqual(metrics["event_dropped"], 0)
        self.assertEqual(metrics["event_write_errors"], 0)
        self.assertEqual(metrics["event_tracks"], 2)

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

    def test_authentication_failures_are_rate_limited(self):
        codes = []
        for _ in range(5):
            with self.assertRaises(urllib.error.HTTPError) as context:
                self.request("/api/status", token="wrong-token")
            codes.append(context.exception.code)
        self.assertEqual(codes[:4], [401, 401, 401, 401])
        self.assertEqual(codes[4], 429)

    def test_token_rotation_invalidates_old_token_and_is_audited(self):
        response = self.request(
            "/api/auth/token/rotate", "POST", b'{"confirm":true}')
        result = json.load(response)
        self.assertTrue(result["rotated"])
        self.assertEqual(len(result["token"]), 64)
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request("/api/status")
        self.assertEqual(context.exception.code, 401)
        status = json.load(self.request("/api/security/status", token=result["token"]))
        self.assertTrue(status["token_active"])
        audit = self.state.audit_file.read_text(encoding="utf-8")
        self.assertIn("/api/auth/token/rotate", audit)

    def test_token_can_be_invalidated(self):
        result = json.load(self.request(
            "/api/auth/token/invalidate", "POST", b'{"confirm":true}'))
        self.assertTrue(result["invalidated"])
        self.assertEqual(self.state.token(), "")

    def test_ota_upload_requires_and_verifies_sha256(self):
        self.state.ota_bundle = self.root / "update.rvota"
        content = b"signed ota placeholder"
        digest = hashlib.sha256(content).hexdigest()
        result = json.load(self.request(
            "/api/ota/upload?sha256=" + digest, "POST", content,
            content_type="application/octet-stream"))
        self.assertTrue(result["uploaded"])
        self.assertEqual(self.state.ota_bundle.read_bytes(), content)
        with self.assertRaises(urllib.error.HTTPError) as context:
            self.request("/api/ota/upload?sha256=" + "0" * 64, "POST", content,
                         content_type="application/octet-stream")
        self.assertEqual(context.exception.code, 400)

    def test_ota_api_routes_only_fixed_actions(self):
        calls = []

        def ota_command(action, limit=100, timeout=150):
            calls.append(action)
            return {"action": action}

        def start_action(action):
            calls.append(action)
            return {"accepted": True, "action": action}

        self.state.ota_command = ota_command
        self.state.start_ota_action = start_action
        self.assertEqual(json.load(self.request("/api/ota/status"))["action"], "status")
        self.assertEqual(json.load(self.request(
            "/api/ota/verify", "POST", b""))["action"], "stage")
        accepted = json.load(self.request(
            "/api/ota/apply", "POST", b'{"confirm":true}'))
        self.assertTrue(accepted["accepted"])
        self.assertEqual(calls, ["status", "stage", "apply"])


if __name__ == "__main__":
    unittest.main()
