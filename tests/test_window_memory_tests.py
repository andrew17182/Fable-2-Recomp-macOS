"""Remembered test-window placement (tools/fable2_test_windows.py): keys, defaults, seed and write-back."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import fable2_test_windows as tw


class TestWindowMemoryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.env = patch.dict(os.environ, FABLE2_TEST_WINDOWS_DIR=str(self.root / 'store'))
        self.env.start()
        self.states = []

    def tearDown(self):
        self.env.stop()
        self.temporary.cleanup()

    def state(self):
        path = self.root / ('state%d' % len(self.states))
        path.mkdir()
        self.states.append(path)
        return path

    def game_moves(self, state, x, y, w=1280, h=720):
        (state / tw.WINDOW_FILE).write_text('# game\nx = %d\ny = %d\nwidth = %d\nheight = %d\n' % (x, y, w, h))

    def test_concurrent_launches_get_different_keys_and_defaults(self):
        a, b = self.state(), self.state()
        ra = tw.prepare(a, checkout='agent-2')
        rb = tw.prepare(b, checkout='agent-2')
        self.assertEqual((ra['key'], rb['key']), ('agent-2-1', 'agent-2-2'))
        self.assertEqual(ra['source'], 'default')
        self.assertNotEqual(tw.parse_window_position((a / tw.WINDOW_FILE).read_bytes()),
                            tw.parse_window_position((b / tw.WINDOW_FILE).read_bytes()))
        self.assertEqual(tw.default_position('main-1'), dict(x=60, y=60))

    def test_move_is_remembered_for_the_next_launch_of_that_key(self):
        a = self.state()
        record = tw.prepare(a, checkout='main')
        self.game_moves(a, 500, 300)
        self.assertEqual(tw.commit(a, record)['event'], 'test-window-saved')
        b = self.state()
        again = tw.prepare(b, checkout='main')  # the claim was released at commit
        self.assertEqual((again['key'], again['source']), ('main-1', 'stored'))
        self.assertEqual(tw.parse_window_position((b / tw.WINDOW_FILE).read_bytes()),
                         dict(x=500, y=300, width=1280, height=720))

    def test_live_claim_holds_the_index_and_dead_claim_is_reused(self):
        a = self.state()
        record = tw.prepare(a, checkout='main')
        tw.update_claim(record['key'], os.getpid())
        self.assertEqual(tw.claim(checkout='main'), 'main-2')
        dead = subprocess.Popen(['true'])
        dead.wait()
        tw.update_claim('main-1', dead.pid)
        self.assertEqual(tw.claim(checkout='main'), 'main-1')

    def test_unmoved_window_writes_nothing(self):
        a = self.state()
        record = tw.prepare(a, checkout='main')
        self.assertEqual(tw.commit(a, record)['event'], 'test-window-unchanged')
        self.assertFalse((self.root / 'store/main-1.toml').exists())

    def test_invalid_session_file_is_not_published(self):
        a = self.state()
        record = tw.prepare(a, checkout='main')
        (a / tw.WINDOW_FILE).write_text('x = 1\ny = 2\nz = 3\n')
        self.assertEqual(tw.commit(a, record)['event'], 'test-window-invalid')
        self.assertFalse((self.root / 'store/main-1.toml').exists())

    def test_newer_store_entry_is_kept(self):
        a = self.state()
        record = tw.prepare(a, checkout='main')
        (self.root / 'store/main-1.toml').write_text('x = 9\ny = 9\n')
        self.game_moves(a, 500, 300)
        self.assertEqual(tw.commit(a, record)['event'], 'test-window-kept-newer')
        self.assertIn('x = 9', (self.root / 'store/main-1.toml').read_text())

    def test_fixed_size_launch_remembers_position_only(self):
        self.assertTrue(tw.fixed_size(['--audio_mute=true', '--window_width=1280']))
        self.assertFalse(tw.fixed_size(['--window_widthx=1']))
        a = self.state()
        first = tw.prepare(a, checkout='main')
        self.game_moves(a, 100, 100, 900, 600)
        tw.commit(a, first)
        b = self.state()
        record = tw.prepare(b, position_only=True, checkout='main')
        self.assertEqual(tw.parse_window_position((b / tw.WINDOW_FILE).read_bytes()), dict(x=100, y=100))
        self.game_moves(b, 700, 200, 1920, 1080)
        self.assertEqual(tw.commit(b, record)['event'], 'test-window-saved')
        self.assertEqual(tw.parse_window_position((self.root / 'store/main-1.toml').read_bytes()),
                         dict(x=700, y=200, width=900, height=600))

    def test_switch(self):
        self.assertTrue(tw.enabled({}))
        self.assertFalse(tw.enabled({'FABLE2_TEST_WINDOW_MEMORY': '0'}))


if __name__ == '__main__':
    unittest.main()
