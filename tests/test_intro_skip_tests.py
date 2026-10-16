"""Test and agent launches always skip the intro movies and run the fast title (2026-10-15), whatever the player
default (off since then). Checks the shared helper and that every test-launch path uses it:
fable2_dev_session.launch(test_control=True) (f2diag, MCP, slot test runs), fable2_test_control.launch, the
benchmark's session environment and the cache warm-up. FABLE2_TEST_INTRO=1 is the one opt-out (intro-path tests).
Runs under /usr/bin/python3 (3.9)."""
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fable2_common as common  # noqa: E402


class ForceSkipTests(unittest.TestCase):
    def setUp(self):
        self.saved = os.environ.pop('FABLE2_TEST_INTRO', None)

    def tearDown(self):
        if self.saved is not None: os.environ['FABLE2_TEST_INTRO'] = self.saved

    def test_overrides_a_launcher_off(self):
        env = common.force_test_intro_skip({'FABLE2_SKIP_INTRO_MOVIES': '0'})
        self.assertEqual((env['FABLE2_SKIP_INTRO_MOVIES'], env['FABLE2_TITLE_FAST']), ('1', '1'))

    def test_intro_tests_opt_out(self):
        env = common.force_test_intro_skip({'FABLE2_SKIP_INTRO_MOVIES': '0', 'FABLE2_TEST_INTRO': '1'})
        self.assertEqual(env['FABLE2_SKIP_INTRO_MOVIES'], '0')
        self.assertNotIn('FABLE2_TITLE_FAST', env)

    def test_makes_stand_ins_under_the_game_root(self):
        with tempfile.TemporaryDirectory() as tmp:  # no videos: nothing made, never raises
            common.force_test_intro_skip({}, Path(tmp))


class PathsTests(unittest.TestCase):
    def test_every_test_launch_path_forces_it(self):
        text = lambda p: (ROOT / p).read_text()
        self.assertIn('common.force_test_intro_skip(env, data_root)', text('tools/fable2_dev_session.py'))
        self.assertIn('common.force_test_intro_skip(env, ROOT)', text('tools/fable2_test_control.py'))
        bench = text('tools/fable2_benchmark.py')
        self.assertIn("FABLE2_SKIP_INTRO_MOVIES='1'", bench)
        self.assertIn("FABLE2_TITLE_FAST='1'", bench)
        self.assertIn("os.environ.setdefault('FABLE2_SKIP_INTRO_MOVIES', '1')", text('tools/fable2_cache_warm.py'))
        self.assertIn('skip_intro=True', text('tools/fable2_benchmark_kit.py'))  # kits: Fable2SkipIntroDefault
        # f2diag and the MCP launch through dev_session.launch(test_control=True) / test_control.launch.
        self.assertIn('test_control=True', text('tools/f2diag/run.py'))
        self.assertIn('control.launch(', text('tools/fable2_test_mcp.py'))


if __name__ == '__main__':
    unittest.main()
