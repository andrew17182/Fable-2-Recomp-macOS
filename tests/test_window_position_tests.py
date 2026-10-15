"""Isolated launches inherit desktop geometry without changing source saves."""
from pathlib import Path
import os
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import fable2_test_control as control


class TestLaunchWindowPositionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.home = self.root / 'home'
        self.source = self.root / 'source'
        self.state = self.root / 'new-test-state'
        self.state.mkdir()
        self.root_patch = patch.object(control, 'ROOT', self.root)
        self.home_patch = patch.object(control.Path, 'home', return_value=self.home)
        self.root_patch.start()
        self.home_patch.start()

    def tearDown(self):
        self.home_patch.stop()
        self.root_patch.stop()
        self.temporary.cleanup()

    def geometry(self, folder, x, timestamp):
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / control.WINDOW_POSITION
        path.write_text('x = %d\ny = 100\n' % x)
        os.utime(path, ns=(timestamp, timestamp))
        return path

    def test_first_test_inherits_normal_app_position(self):
        normal = self.geometry(self.home / 'Library/Application Support/Fable II Recomp', 120, 100)
        self.assertEqual(control.copy_latest_window_position(self.state, self.source), str(normal))
        self.assertEqual((self.state / control.WINDOW_POSITION).read_bytes(), normal.read_bytes())

    def test_latest_test_position_wins_over_recording_settings(self):
        recorded = self.geometry(self.source, 20, 100)
        prior = self.geometry(self.root / 'out/validation/gameplay-tests/previous/test-state', 450, 200)
        control.copy_latest_window_position(self.state, self.source)
        self.assertEqual((self.state / control.WINDOW_POSITION).read_bytes(), prior.read_bytes())
        self.assertIn('x = 20', recorded.read_text())

    def test_newer_normal_app_move_takes_priority(self):
        self.geometry(self.root / 'out/validation/gameplay-tests/previous/test-state', 450, 100)
        normal = self.geometry(self.home / 'Library/Application Support/Fable II Recomp', 220, 200)
        control.copy_latest_window_position(self.state, self.source)
        self.assertEqual((self.state / control.WINDOW_POSITION).read_bytes(), normal.read_bytes())

    def test_missing_preferences_keep_default_placement(self):
        self.assertIsNone(control.copy_latest_window_position(self.state, self.source))
        self.assertFalse((self.state / control.WINDOW_POSITION).exists())


if __name__ == '__main__':
    unittest.main()
