"""One app, one launcher (docs/player-app.md "Developer mode"): the Dev Launcher is a role of the player app.

Builds the one binary with swiftc into two stand-in bundles: the Dev Launcher role (Info.plist Fable2DevLauncher,
as tools/macos/build_dev_launcher.sh writes it) and the player role (release/portable). `--list-capabilities` lists
what each role can reach, from the same data its windows and menus are built from. Checks:
  - every Dev Launcher capability is reachable in the player role with developer mode on and a source checkout;
  - developer mode off: none of the Dev Launcher's windows or tools are shown;
  - developer mode on without a checkout: the Dev Launcher window is there (with the "needs a source checkout" note),
    the repository tools are not;
  - the separate DevLauncher target is gone: build_dev_launcher.sh builds the player's entry point.
Scratch state folders only. Runs under /usr/bin/python3 (3.9)."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
APP = [ROOT / 'tools/macos/Player/main.swift'] + sorted((ROOT / 'tools/macos/DevLauncher').glob('*.swift')) \
    + sorted((ROOT / 'tools/macos/Shared').glob('*.swift'))


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class UnifiedLauncherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='unified-launcher-')
        cls.root = Path(cls.temp.name)
        binary = cls.root / 'FableIIPlayer'
        subprocess.run(['xcrun', 'swiftc', '-O', '-target', 'arm64-apple-macos15.0', '-o', str(binary)]
                       + [str(s) for s in APP], check=True, capture_output=True)
        cls.apps = {}
        for role, extra in (('dev', dict(Fable2DevLauncher=True, Fable2DeveloperModeDefault=True,
                                         Fable2RepoRoot=str(ROOT))),
                            ('player', dict(Fable2BuildsGame=True, Fable2StateFolder='Fable II test'))):
            app = cls.root / role / 'Test.app'
            (app / 'Contents/MacOS').mkdir(parents=True)
            shutil.copy2(binary, app / 'Contents/MacOS/FableIIPlayer')
            info = dict(CFBundleExecutable='FableIIPlayer', CFBundleIdentifier='org.fable2recomp.test.' + role,
                        CFBundlePackageType='APPL', **extra)
            (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
            cls.apps[role] = app / 'Contents/MacOS/FableIIPlayer'

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def caps(self, role, developer=None, checkout=True):
        state = Path(tempfile.mkdtemp(dir=str(self.root)))
        if developer is not None:
            (state / 'player-settings.plist').write_bytes(plistlib.dumps(dict(developerMode=developer)))
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env['FABLE2_APP_STATE_ROOT'] = str(state)
        if checkout: env['FABLE2_REPO_ROOT'] = str(ROOT)
        out = subprocess.run([str(self.apps[role]), '--list-capabilities'], env=env, capture_output=True, text=True,
                             timeout=60, check=True).stdout
        return json.loads(out)

    def test_every_dev_launcher_capability_is_in_developer_mode(self):
        dev = self.caps('dev')
        self.assertEqual(dev['role'], 'dev-launcher')
        player = self.caps('player', developer=True)
        self.assertEqual(player['role'], 'player')
        missing = sorted(set(dev['capabilities']) - set(player['capabilities']))
        self.assertEqual(missing, [], 'Dev Launcher capabilities missing from the player role in developer mode')
        self.assertIn('window:dev-launcher', player['capabilities'])
        self.assertTrue(any(c.startswith('tool:') for c in player['capabilities']))

    def test_developer_mode_off_hides_the_dev_launcher(self):
        player = self.caps('player', developer=False)
        self.assertFalse([c for c in player['capabilities'] if c.startswith('tool:') or c == 'window:dev-launcher'])

    def test_without_checkout_tools_wait_for_one(self):
        player = self.caps('player', developer=True, checkout=False)
        self.assertIsNone(player['checkout'])
        self.assertIn('window:dev-launcher', player['capabilities'])
        self.assertFalse([c for c in player['capabilities'] if c.startswith('tool:')])

    def test_one_target(self):
        script = (ROOT / 'tools/macos/build_dev_launcher.sh').read_text()
        self.assertIn('tools/macos/Player/main.swift', script)
        self.assertIn('Fable2DevLauncher', script)
        self.assertFalse((ROOT / 'tools/macos/DevLauncher/main.swift').exists())


if __name__ == '__main__':
    unittest.main()
