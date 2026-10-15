"""Continue in the player app (tools/macos/Player/main.swift) with 0, 1 and several hero saves.

2026-10-14 bug: with several saves the main menu cycled for ~21 s before the game's slot cards appeared,
because boot-save (src/core/fable2_boot_save.h) re-sent Continue every 100 ms while the menu was already
handling it (docs/bug-patterns.md). Now the player names the newest save (FABLE2_BOOT_SAVE_HERO) and boot-save
loads it directly; an accepted event (menu state changed) is never re-sent.

Checks `--print-env` (the items Play sets) against fake save folders, and the boot-save source for the
no-resend rule. Builds the player with swiftc (skipped without Xcode). Runs under /usr/bin/python3 (3.9)."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ([ROOT / 'tools/macos/Player/main.swift'] + sorted((ROOT / 'tools/macos/DevLauncher').glob('*.swift'))
           + sorted((ROOT / 'tools/macos/Shared').glob('*.swift')))  # the one app (docs/player-app.md)
SLOTS = 'saves/B13EBABEBABEBABE/4D5307F1/00000001'


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class PlayerContinueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='player-continue-')
        cls.root = Path(cls.temp.name)
        cls.player = cls.root / 'app/MacOS/FableIIPlayer'
        cls.player.parent.mkdir(parents=True)
        subprocess.run(['xcrun', 'swiftc', '-O', '-target', 'arm64-apple-macos15.0', '-o', str(cls.player)]
                       + [str(s) for s in SOURCES], check=True, capture_output=True)
        cls.game = cls.root / 'GameData'
        (cls.game / 'data').mkdir(parents=True)
        (cls.game / 'default.xex').write_bytes(b'stand-in')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def play_env(self, heroes, start='continue'):
        """heroes: {HeroNNN: age in seconds}; returns the Play environment as a dict."""
        state = self.root / ('state-%s-%s' % (start, '-'.join(sorted(heroes)) or 'none'))
        (state / SLOTS).mkdir(parents=True)
        now = time.time()
        for hero, age in heroes.items():
            (state / SLOTS / hero).mkdir()
            save = state / SLOTS / hero / 'mainsave.bin'
            save.write_bytes(b'x')
            os.utime(save, (now - age, now - age))
        import plistlib
        (state / 'player-settings.plist').write_bytes(plistlib.dumps({'startMode': start}))
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env.update(FABLE2_APP_STATE_ROOT=str(state), FABLE2_GAME_DATA_ROOT=str(self.game))
        out = subprocess.run([str(self.player), '--print-env'], env=env, check=True, capture_output=True,
                             text=True, timeout=60).stdout
        return dict(item.partition('=')[::2] for item in json.loads(out))

    def test_no_saves_never_boot_saves(self):
        env = self.play_env({})
        self.assertNotIn('FABLE2_BOOT_SAVE', env)
        self.assertNotIn('FABLE2_BOOT_SAVE_HERO', env)

    def test_one_save_uses_the_games_own_continue(self):
        env = self.play_env({'Hero002': 10})
        self.assertEqual(env['FABLE2_BOOT_SAVE'], '1')
        self.assertNotIn('FABLE2_BOOT_SAVE_HERO', env)

    def test_several_saves_name_the_newest(self):
        env = self.play_env({'Hero000': 300, 'Hero003': 5, 'Hero005': 60})
        self.assertEqual(env['FABLE2_BOOT_SAVE'], '1')
        self.assertEqual(env['FABLE2_BOOT_SAVE_HERO'], 'Hero003')

    def test_title_screen_sets_nothing(self):
        env = self.play_env({'Hero000': 300, 'Hero001': 5}, start='title')
        self.assertNotIn('FABLE2_BOOT_SAVE', env)
        self.assertNotIn('FABLE2_BOOT_SAVE_HERO', env)

    def test_boot_save_never_resends_an_accepted_event(self):
        source = (ROOT / 'src/core/fable2_boot_save.h').read_text()
        self.assertIn('const bool accepted = state_after != state_before;', source)
        self.assertIn('if (started || left_menu || accepted) s.menu_done = true;', source)
        self.assertIn('FABLE2_BOOT_SAVE_HERO', source)


if __name__ == '__main__':
    unittest.main()
