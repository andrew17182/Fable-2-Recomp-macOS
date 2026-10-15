"""Capture metadata must remain on the native clock despite client delay."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import fable2_test_control as control
import fable2_test_mcp as mcp


class ScreenshotTimestampTests(unittest.TestCase):
    def test_native_timestamps_survive_conversion_delay(self):
        with tempfile.TemporaryDirectory() as folder:
            ppm = Path(folder) / 'frame.ppm'
            native = dict(schema=1, clock='CLOCK_MONOTONIC',
                          frame_submitted_monotonic_us=100,
                          capture_started_monotonic_us=110,
                          capture_completed_monotonic_us=150,
                          session_origin_monotonic_us=50, frame_t_us=50)
            ppm.with_suffix('.capture.json').write_text(json.dumps(native))
            with patch.object(control, 'request', return_value=str(ppm)), \
                 patch.object(control.subprocess, 'run'), \
                 patch.object(control.time, 'clock_gettime_ns', side_effect=[105000, 200000, 5200000]):
                result = control.capture_with_metadata(folder)
            for key, value in native.items():
                self.assertEqual(result[key], value)
            self.assertEqual(result['capture_duration_us'], 40)
            self.assertEqual(result['delivery_and_conversion_us'], 5050)
            self.assertEqual(result['frame_age_at_image_ready_us'], 5100)
            self.assertEqual(json.loads(ppm.with_suffix('.capture.json').read_text()), result)

    def test_older_app_does_not_fabricate_native_time(self):
        with tempfile.TemporaryDirectory() as folder:
            ppm = Path(folder) / 'frame.ppm'
            with patch.object(control, 'request', return_value=str(ppm)), patch.object(control.subprocess, 'run'):
                result = control.capture_with_metadata(folder)
            self.assertFalse(result['native_timestamps_available'])
            self.assertNotIn('frame_submitted_monotonic_us', result)

    def test_bridge_returns_image_and_metadata(self):
        with tempfile.TemporaryDirectory() as folder:
            png = Path(folder) / 'frame.png'
            png.write_bytes(b'test-image')
            metadata = dict(path=str(png), frame_t_us=123456, clock='CLOCK_MONOTONIC')
            # mcp.call reloads the controller module on every call (live edits), which would drop the patch.
            with patch.object(mcp.importlib, 'reload', side_effect=lambda module: module), \
                 patch.object(control, 'capture_with_metadata', return_value=metadata):
                result = mcp.call('capture', {'session': folder})
            self.assertEqual(result['content'][0]['type'], 'image')
            self.assertEqual(json.loads(result['content'][1]['text']), metadata)


if __name__ == '__main__':
    unittest.main()
