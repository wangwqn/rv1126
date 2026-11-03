import importlib.util
import json
import os
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace


MODULE = Path(__file__).resolve().parents[1] / "rv1126_recording.py"
SPEC = importlib.util.spec_from_file_location("rv1126_recording", MODULE)
recording = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(recording)


class RecordingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.root = self.base / "app"
        self.root.mkdir()
        (self.root / "events").mkdir()
        (self.root / "runtime-config.json").write_text(json.dumps({
            "recording": dict(recording.DEFAULTS)
        }), encoding="utf-8")
        self.args = SimpleNamespace(
            root=str(self.root), status=str(self.base / "status.json"),
            state=str(self.base / "state.json"),
            recorder_log=str(self.base / "recorder.log"))

    def tearDown(self):
        self.temporary.cleanup()

    def test_configuration_is_bounded(self):
        value = recording.load_config(self.root / "runtime-config.json")
        self.assertTrue(value["enabled"])
        self.assertEqual(value["segment_seconds"], 10)
        value["segment_seconds"] = 1
        (self.root / "runtime-config.json").write_text(
            json.dumps({"recording": value}), encoding="utf-8")
        with self.assertRaises(ValueError):
            recording.load_config(self.root / "runtime-config.json")

    def test_event_links_pre_and_post_segments(self):
        event_log = self.root / "events" / "events.jsonl"
        event_log.write_text("", encoding="utf-8")
        service = recording.RecordingService(self.args)
        now = time.time()
        for index, age in enumerate((12, 5, 0)):
            path = service.continuous / "segment-{}.mkv".format(index)
            path.write_bytes(b"segment" + bytes([index]))
            os.utime(str(path), (now - age, now - age))
        event = {"type": "region_enter", "track_id": 7, "label": "book"}
        with event_log.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(event) + "\n")
        service._read_events()
        self.assertEqual(len(service.pending), 1)
        directory = service.pending[0]["directory"]
        self.assertGreaterEqual(len(list(directory.glob("*.mkv"))), 2)
        service.pending[0]["due"] = 0
        service._finish_events()
        manifest = json.loads((directory / "event.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["event"]["track_id"], 7)
        self.assertTrue(manifest["recording_files"])

    def test_event_uses_filename_time_when_splitmux_updates_mtime(self):
        event_log = self.root / "events" / "events.jsonl"
        event_log.write_text("", encoding="utf-8")
        service = recording.RecordingService(self.args)
        now = time.time()
        session = time.strftime("%Y%m%dT%H%M%S", time.localtime(now - 10))
        paths = []
        for index in range(3):
            path = service.continuous / "{}-{:05d}.mkv".format(session, index)
            path.write_bytes(b"segment")
            # Model splitmuxsink finalising all segments well after the event.
            os.utime(str(path), (now + 60, now + 60))
            paths.append(path)
        event_log.write_text(json.dumps({"type": "object", "track_id": 2}) + "\n",
                             encoding="utf-8")
        service._read_events()
        linked = list(service.pending[0]["directory"].glob("*.mkv"))
        self.assertTrue(linked)
        self.assertLessEqual(len(linked), 2)

    def test_disabled_service_stays_healthy_until_stopped(self):
        service = recording.RecordingService(self.args)
        service.config["enabled"] = False
        service.stopping = True
        service.run()
        status = json.loads(Path(self.args.status).read_text(encoding="utf-8"))
        self.assertTrue(status["ok"])
        self.assertFalse(status["enabled"])

    def test_empty_event_waits_for_delayed_segment(self):
        event_log = self.root / "events" / "events.jsonl"
        event_log.write_text("", encoding="utf-8")
        service = recording.RecordingService(self.args)
        event_log.write_text(json.dumps({"type": "line_cross"}) + "\n",
                             encoding="utf-8")
        service._read_events()
        service.pending[0]["due"] = 0
        service._finish_events()
        self.assertEqual(len(service.pending), 1)
        self.assertFalse((service.pending[0]["directory"] / "event.json").exists())

    def test_final_window_does_not_include_waiting_period(self):
        event_log = self.root / "events" / "events.jsonl"
        event_log.write_text("", encoding="utf-8")
        service = recording.RecordingService(self.args)
        seen = time.time()
        session = time.strftime("%Y%m%dT%H%M%S", time.localtime(seen - 20))
        for index in range(8):
            (service.continuous / "{}-{:05d}.mkv".format(session, index)).write_bytes(b"x")
        event_log.write_text(json.dumps({"type": "object"}) + "\n", encoding="utf-8")
        service._read_events()
        service.pending[0]["seen_at"] = seen
        service.pending[0]["due"] = 0
        service._finish_events()
        manifest = json.loads((service.events / next(service.events.iterdir()).name /
                               "event.json").read_text(encoding="utf-8"))
        # Filename timestamps have one-second precision, so this 30-second
        # window intersects four segments, never the later grace-period files.
        self.assertEqual(len(manifest["recording_files"]), 4)

    def test_continuous_cleanup_keeps_newest_segments(self):
        value = dict(recording.DEFAULTS)
        value.update({"max_storage_mb": 64, "min_free_mb": 32})
        (self.root / "runtime-config.json").write_text(
            json.dumps({"recording": value}), encoding="utf-8")
        service = recording.RecordingService(self.args)
        now = time.time()
        for index in range(3):
            path = service.continuous / "large-{}.mkv".format(index)
            with path.open("wb") as handle:
                handle.truncate(30 * 1024 * 1024)
            os.utime(str(path), (now + index, now + index))
        service._cleanup_continuous()
        remaining = sorted(path.name for path in service.continuous.glob("*.mkv"))
        self.assertEqual(remaining, ["large-1.mkv", "large-2.mkv"])


if __name__ == "__main__":
    unittest.main()
