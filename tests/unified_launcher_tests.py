"""One app, one launcher (docs/player-app.md "Developer mode"): the Dev Launcher is a role of the player app.

Builds the one binary with swiftc into two stand-in bundles: the Dev Launcher role (Info.plist Fable2DevLauncher,
as tools/macos/build_dev_launcher.sh writes it) and the player role (release/portable). `--list-capabilities` lists
what each role can reach, from the same data its windows and menus are built from. Checks:
  - with Dev Options on (2026-10-19, was "developer mode"), every Dev Launcher capability is reachable in the player
    role with a source checkout;
  - Dev Options off: the player role shows none of the Dev Launcher's windows or tools; neither role shows the
    Performance, Fixes or Diagnostics tabs (or their switches) or the dev-only tools; Dev Options defaults to off in
    both roles, the repo launcher included; the everyday tabs (General, Display, Controls, Mods) always show;
  - `--launch --print-args` does not depend on Dev Options (agent tooling);
  - Dev Options on without a checkout: the Dev Launcher window is there (with the "needs a source checkout" note),
    the repository tools are not;
  - the separate DevLauncher target is gone: build_dev_launcher.sh builds the player's entry point;
  - the build choice defaults to Latest; a saved "playtest" (the old default) becomes Latest once (version flag),
    a later deliberate Playtest is kept; with no playtest copy (a fresh clone) Playtest launches Latest with a note.
    These run `--launch --print-args` against scratch checkout roots, with a throwaway defaults domain.
Scratch state folders only. Runs under /usr/bin/python3 (3.9)."""
import json
import os
from pathlib import Path
import plistlib
import re
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
        # devprefs: the Dev Launcher role under a throwaway bundle id, so build-choice tests own their defaults domain.
        cls.domain = 'org.fable2recomp.test.devprefs%d' % os.getpid()
        for role, extra in (('dev', dict(Fable2DevLauncher=True, Fable2RepoRoot=str(ROOT))),
                            ('devprefs', dict(Fable2DevLauncher=True)),
                            ('player', dict(Fable2BuildsGame=True, Fable2StateFolder='Fable II test'))):
            app = cls.root / role / 'Test.app'
            (app / 'Contents/MacOS').mkdir(parents=True)
            shutil.copy2(binary, app / 'Contents/MacOS/FableIIPlayer')
            bundle_id = cls.domain if role == 'devprefs' else 'org.fable2recomp.test.' + role
            info = dict(CFBundleExecutable='FableIIPlayer', CFBundleIdentifier=bundle_id,
                        CFBundlePackageType='APPL', **extra)
            (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
            cls.apps[role] = app / 'Contents/MacOS/FableIIPlayer'

    @classmethod
    def tearDownClass(cls):
        subprocess.run(['defaults', 'delete', cls.domain], capture_output=True)
        subprocess.run(['defaults', 'delete', 'org.fable2recomp.test.dev'], capture_output=True)
        cls.temp.cleanup()

    def caps(self, role, developer=None, checkout=True):
        state = Path(tempfile.mkdtemp(dir=str(self.root)))
        if role == 'dev':  # the Dev Launcher role stores Dev Options in its defaults domain (a throwaway test id)
            subprocess.run(['defaults', 'delete', 'org.fable2recomp.test.dev'], capture_output=True)
            if developer is not None:
                subprocess.run(['defaults', 'write', 'org.fable2recomp.test.dev', 'developerMode', '-bool',
                                'true' if developer else 'false'], check=True)
        elif developer is not None:
            (state / 'player-settings.plist').write_bytes(plistlib.dumps(dict(developerMode=developer)))
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env['FABLE2_APP_STATE_ROOT'] = str(state)
        if checkout: env['FABLE2_REPO_ROOT'] = str(ROOT)
        out = subprocess.run([str(self.apps[role]), '--list-capabilities'], env=env, capture_output=True, text=True,
                             timeout=60, check=True).stdout
        return json.loads(out)

    def test_every_dev_launcher_capability_is_in_developer_mode(self):
        dev = self.caps('dev', developer=True)
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

    DEV_TABS = ('settings:performance', 'settings:fixes', 'settings:diagnostics')
    EVERYDAY_TABS = ('settings:general', 'settings:display', 'settings:controls', 'settings:mods')

    def test_dev_options_default_off_in_both_roles(self):
        for role in ('dev', 'player'):
            caps = self.caps(role)['capabilities']
            for tab in self.DEV_TABS:
                self.assertNotIn(tab, caps, role)
            for tab in self.EVERYDAY_TABS:
                self.assertIn(tab, caps, role)
            # Switches of hidden tabs are hidden too; the Mods tab has the Character Editor.
            self.assertNotIn('flag:furFix', caps, role)
            self.assertIn('flag:characterEditor', caps, role)
        self.assertNotIn('window:dev-launcher', self.caps('player')['capabilities'])
        dev_tools = [c for c in self.caps('dev')['capabilities'] if c.startswith('tool:')]
        self.assertNotIn('tool:benchmark', dev_tools)
        self.assertNotIn('tool:choose-save', dev_tools)  # the save library is a Dev Options tool
        self.assertIn('tool:close-game', dev_tools)

    def test_dev_options_on_shows_dev_tabs_in_both_roles(self):
        for role in ('dev', 'player'):
            caps = self.caps(role, developer=True)['capabilities']
            for tab in self.DEV_TABS + self.EVERYDAY_TABS:
                self.assertIn(tab, caps, role)
            self.assertIn('flag:furFix', caps, role)

    def test_print_args_ignores_dev_options(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env['FABLE2_REPO_ROOT'] = str(ROOT)
        outs = []
        for developer in ('true', 'false'):
            subprocess.run(['defaults', 'delete', self.domain], capture_output=True)
            subprocess.run(['defaults', 'write', self.domain, 'developerMode', '-bool', developer], check=True)
            r = subprocess.run([str(self.apps['devprefs']), '--launch', '--print-args', '--pick', 'live:Hero000',
                                '--test-control', '--no-fur-fix'], env=env, capture_output=True, text=True,
                               timeout=60, check=True)
            outs.append(json.loads(r.stdout))
        self.assertEqual(outs[0], outs[1])
        self.assertIn('--test-control', outs[0])
        self.assertIn('FABLE2_MEMEXPORT_PRESERVE=0', outs[0])

    def test_settings_live_in_the_launcher_window(self):
        """No separate Settings window or save-picker button: Settings is a page of the launcher window (⌘,)."""
        sources = '\n'.join(p.read_text() for p in APP)
        self.assertIsNone(re.search(r'(?<![A-Za-z])Settings \{', sources))  # no Settings scene
        self.assertNotIn('SettingsLink', sources)
        self.assertNotIn('Button("Choose Save…")', sources)
        self.assertNotIn('Continue from Last Save', sources)
        self.assertIn('CommandGroup(replacing: .appSettings)', sources)
        self.assertIn('"Load Save"', sources)
        self.assertIn('"Upscaling (window scaling)"', sources)

    def test_without_checkout_tools_wait_for_one(self):
        player = self.caps('player', developer=True, checkout=False)
        self.assertIsNone(player['checkout'])
        self.assertIn('window:dev-launcher', player['capabilities'])
        self.assertFalse([c for c in player['capabilities'] if c.startswith('tool:')])

    # Build choice (2026-10-15: a fresh clone has no out/playtest; the old Playtest default failed every launch).
    def roots(self):
        fresh = Path(tempfile.mkdtemp(dir=str(self.root)))  # a fresh clone: no out/playtest
        frozen = Path(tempfile.mkdtemp(dir=str(self.root)))
        (frozen / 'out/playtest/Fable II.app/Contents/MacOS').mkdir(parents=True)
        (frozen / 'out/playtest/playtest.json').write_text('{"version": "test"}')
        return fresh, frozen

    def prefs(self, **values):
        subprocess.run(['defaults', 'delete', self.domain], capture_output=True)
        for key, value in values.items():
            kind = '-int' if isinstance(value, int) else '-string'
            subprocess.run(['defaults', 'write', self.domain, key, kind, str(value)], check=True)

    def read_pref(self, key):
        r = subprocess.run(['defaults', 'read', self.domain, key], capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else None

    def launch_build(self, root, *extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env['FABLE2_REPO_ROOT'] = str(root)
        r = subprocess.run([str(self.apps['devprefs']), '--launch', '--print-args', *extra], env=env,
                           capture_output=True, text=True, timeout=60, check=True)
        args = json.loads(r.stdout)
        return args[args.index('--build') + 1], r.stderr

    def test_build_defaults_to_latest(self):
        fresh, frozen = self.roots()
        for root in (fresh, frozen):
            self.prefs()
            self.assertEqual(self.launch_build(root)[0], 'latest')

    def test_saved_playtest_migrates_to_latest_once(self):
        _, frozen = self.roots()
        self.prefs(build='playtest')  # the old default, saved before the migration existed
        self.assertEqual(self.launch_build(frozen)[0], 'latest')
        self.assertEqual(self.read_pref('build'), 'latest')
        self.assertEqual(self.read_pref('buildChoiceMigration'), '1')
        # A deliberate Playtest pick after the migration is kept.
        self.prefs(build='playtest', buildChoiceMigration=1)
        self.assertEqual(self.launch_build(frozen)[0], 'playtest')
        self.assertEqual(self.read_pref('build'), 'playtest')

    def test_missing_playtest_copy_launches_latest_with_note(self):
        fresh, frozen = self.roots()
        self.prefs(build='playtest', buildChoiceMigration=1)
        build, note = self.launch_build(fresh)
        self.assertEqual(build, 'latest')
        self.assertIn('No playtest copy', note)
        self.prefs()
        build, note = self.launch_build(fresh, '--build', 'playtest')
        self.assertEqual(build, 'latest')
        self.assertIn('No playtest copy', note)
        self.assertEqual(self.launch_build(frozen, '--build', 'playtest'), ('playtest', ''))

    def test_one_target(self):
        script = (ROOT / 'tools/macos/build_dev_launcher.sh').read_text()
        self.assertIn('tools/macos/Player/main.swift', script)
        self.assertIn('Fable2DevLauncher', script)
        self.assertFalse((ROOT / 'tools/macos/DevLauncher/main.swift').exists())


if __name__ == '__main__':
    unittest.main()
