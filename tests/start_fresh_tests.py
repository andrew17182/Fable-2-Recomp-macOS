"""Start Fresh / Restore / Delete archive in the player app (tools/macos/Shared/StartFresh.swift, docs/release-app.md).

Builds the player with swiftc into a stand-in .app (bundle id org.fable2recomp.metal.release-test) and drives the CLI
of the same code the sheets use (--start-fresh [--archive-saves], --list-archives, --restore-archive NAME,
--delete-archive NAME) on a scratch state folder (FABLE2_APP_STATE_ROOT) with stand-in system folders
(FABLE2_FRESH_SYSTEM_ROOT). Never touches the real Application Support, caches or preferences.
Runs under /usr/bin/python3 (3.9)."""
import fcntl
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ([ROOT / 'tools/macos/Player/main.swift'] + sorted((ROOT / 'tools/macos/DevLauncher').glob('*.swift'))
           + sorted((ROOT / 'tools/macos/Shared').glob('*.swift')))  # the one app (docs/player-app.md)
BUNDLE_ID = 'org.fable2recomp.metal.release-test'
SAVE = 'saves/B13EBABEBABEBABE/4D5307F1/00000001/Hero000/mainsave.bin'


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class StartFreshTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='start-fresh-')
        cls.root = Path(cls.temp.name)
        app = cls.root / 'Fable II.app'
        (app / 'Contents/MacOS').mkdir(parents=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleExecutable='FableIIPlayer', CFBundleIdentifier=BUNDLE_ID, CFBundlePackageType='APPL',
            Fable2BuildsGame=True, Fable2StateFolder='Fable II test')))
        cls.player = app / 'Contents/MacOS/FableIIPlayer'
        subprocess.run(['xcrun', 'swiftc', '-O', '-target', 'arm64-apple-macos15.0', '-o', str(cls.player)]
                       + [str(s) for s in SOURCES], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.case = Path(tempfile.mkdtemp(prefix='case-', dir=str(self.root)))
        self.state = self.case / 'state'
        self.system = self.case / 'system'
        for rel, text in [('GameData/default.xex', 'xex'), ('GameData/data/x.bnk', 'data'),
                          ('Build/src/generated/default/a.cpp', 'gen'), ('Game/Fable II.bundle/Contents/MacOS/fable_2', 'bin'),
                          ('logs/build.log', 'log'), ('cache/shaders/x', 'cache'), ('player-settings.plist', 'p'),
                          ('launcher.plist', 'l'), (SAVE, 'save')]:
            path = self.state / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        (self.state / 'app.lock').write_text('')
        for rel in ['Saved Application State/%s.savedState/windows.plist' % BUNDLE_ID, 'Caches/%s/x' % BUNDLE_ID,
                    'DarwinUserCache/%s/com.apple.metal/x' % BUNDLE_ID, 'Preferences/%s.plist' % BUNDLE_ID,
                    'Caches/org.fable2recomp.metal/other-app']:
            path = self.system / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('sys')

    def cli(self, *args, expect_ok=True):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env.update(FABLE2_APP_STATE_ROOT=str(self.state), FABLE2_FRESH_SYSTEM_ROOT=str(self.system))
        done = subprocess.run([str(self.player), *args], env=env, capture_output=True, text=True, timeout=60)
        result = json.loads(done.stdout)
        self.assertEqual(result['ok'], expect_ok, result)
        return result

    def test_start_fresh_keeps_saves_and_moves_everything_else(self):
        result = self.cli('--start-fresh')
        archive = Path(result['archive'])
        self.assertEqual(archive.parent, self.state / 'Archive')
        self.assertEqual(sorted(p.name for p in self.state.iterdir()), ['Archive', 'app.lock', 'saves'])
        self.assertEqual((self.state / SAVE).read_text(), 'save')
        for rel in ['state/GameData/default.xex', 'state/Build/src/generated/default/a.cpp', 'state/logs/build.log',
                    'state/Game/Fable II.bundle/Contents/MacOS/fable_2', 'state/cache/shaders/x',
                    'system/saved-state/windows.plist', 'system/darwin-user-cache/com.apple.metal/x', 'system/caches/x',
                    'system/%s.defaults.plist' % BUNDLE_ID]:
            self.assertTrue((archive / rel).is_file(), rel)
        self.assertFalse((self.system / 'DarwinUserCache' / BUNDLE_ID).exists())
        self.assertFalse((self.system / ('Preferences/%s.plist' % BUNDLE_ID)).exists())
        # Another app's folder is never touched.
        self.assertTrue((self.system / 'Caches/org.fable2recomp.metal/other-app').is_file())
        record = json.loads((archive / 'fresh.json').read_text())
        self.assertTrue(record['keptSaves'])
        self.assertNotIn(str(self.state / 'saves'), [i['original'] for i in record['items']])

    def test_archive_saves_then_restore_and_delete(self):
        first = Path(self.cli('--start-fresh', '--archive-saves')['archive'])
        self.assertFalse((self.state / 'saves').exists())
        self.assertTrue((first / 'state' / SAVE).is_file())
        # A new first launch writes new files; restoring archives them first, so nothing is lost.
        (self.state / 'logs').mkdir()
        (self.state / 'logs/new.log').write_text('new')
        listing = self.cli('--list-archives')['archives']
        self.assertEqual([a['name'] for a in listing], [first.name])
        restored = self.cli('--restore-archive', first.name)
        self.assertEqual((self.state / SAVE).read_text(), 'save')
        self.assertEqual((self.state / 'GameData/default.xex').read_text(), 'xex')
        self.assertTrue((self.system / ('Preferences/%s.plist' % BUNDLE_ID)).is_file())
        previous = Path(restored['previousStateArchive'])
        self.assertEqual((previous / 'state/logs/new.log').read_text(), 'new')
        # The restored archive is marked, not deleted; deleting is explicit.
        self.assertTrue((first / 'restored.json').is_file())
        names = [a['name'] for a in self.cli('--list-archives')['archives']]
        self.assertEqual(names, [previous.name])
        self.cli('--delete-archive', previous.name)
        self.assertFalse(previous.exists())
        self.cli('--delete-archive', 'no-such-archive', expect_ok=False)

    def test_refused_while_the_game_holds_the_lock(self):
        with open(self.state / 'app.lock', 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.cli('--start-fresh', expect_ok=False)
        self.assertIn('running', result['error'])
        self.assertTrue((self.state / 'GameData/default.xex').is_file())

    def test_system_folders_skipped_without_stand_ins(self):
        """With only FABLE2_APP_STATE_ROOT (tests), real caches/preferences are never in the plan."""
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env.update(FABLE2_APP_STATE_ROOT=str(self.state))
        out = json.loads(subprocess.run([str(self.player), '--start-fresh'], env=env, capture_output=True, text=True,
                                        timeout=60).stdout)
        self.assertTrue(out['ok'], out)
        self.assertTrue(all(m.startswith(str(self.state)) for m in out['moved']), out['moved'])
        self.assertTrue((self.system / ('Preferences/%s.plist' % BUNDLE_ID)).is_file())


if __name__ == '__main__':
    unittest.main()
