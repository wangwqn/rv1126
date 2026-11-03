import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE = Path(__file__).resolve().parents[1] / "rv1126_time_sync.py"
SPEC = importlib.util.spec_from_file_location("rv1126_time_sync", MODULE)
time_sync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(time_sync)


class TimeSyncTest(unittest.TestCase):
    def test_consensus_uses_median_and_ignores_outlier(self):
        values = {
            "a": (1800000000.0, 1.0, 2),
            "b": (1800000001.0, 2.0, 2),
            "c": (1800001000.0, 3.0, 3),
        }
        with mock.patch.object(time_sync, "query", side_effect=lambda name, _: values[name]):
            timestamp, samples, errors = time_sync.sample(("a", "b", "c"), 1)
        self.assertEqual(timestamp, 1800000000)
        self.assertEqual(len(samples), 3)
        self.assertFalse(errors)

    def test_disagreeing_pair_is_rejected(self):
        values = {"a": (1800000000.0, 1.0, 2), "b": (1800001000.0, 1.0, 2)}
        with mock.patch.object(time_sync, "query", side_effect=lambda name, _: values[name]):
            with self.assertRaises(RuntimeError):
                time_sync.sample(("a", "b"), 1)

    def test_atomic_status_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "status.json"
            time_sync.atomic_json(path, {"ok": True})
            self.assertEqual(path.read_text(encoding="utf-8"), '{"ok": true}\n')


if __name__ == "__main__":
    unittest.main()
