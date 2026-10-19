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
    These run `--launch --print-args` against scratch checkout roots, with a throwaway defaults domain;
  - the save menu (`--save-menu`) lists the live slots only (no Title Screen entry: Start at Title Screen is a
    button beside Load Saved Game); library saves (starred, the newest)
    only with Dev Options > "Show starred saves in save menu" (default off);
  - one page button (Settings / Launcher) in the shared page bar, bottom-left on the front page and Settings in
    both roles; Turbo speed and Turbo audio are on the front page, not in Settings > General.
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
        self.assertIn('"Load Saved Game"', sources)
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

    # Save menu (2026-10-19): library saves only with Dev Options > "Show starred saves in save menu".
    def save_menu(self, *extra):
        folder = Path(tempfile.mkdtemp(dir=str(self.root)))
        meta = folder / 'picker.json'
        meta.write_text(json.dumps({'version': 1, 'entries': {'starred-old': {'starred': True},
                                                              'starred-new': {'starred': True}}}))
        listing = {'live': [{'folder': 'Hero000', 'saved_mtime': '2026-10-10 10:00:00'},
                            {'folder': 'Hero001', 'saved_mtime': '2026-10-12 10:00:00'}],
                   'library': [{'name': 'starred-old', 'folder': 'Hero000', 'saved_mtime': '2026-10-01 10:00:00'},
                               {'name': 'starred-new', 'folder': 'Hero000', 'saved_mtime': '2026-10-11 10:00:00'},
                               {'name': 'newest', 'folder': 'Hero000', 'saved_mtime': '2026-10-15 10:00:00'},
                               {'name': 'other', 'folder': 'Hero000', 'saved_mtime': '2026-10-02 10:00:00'}],
                   'picker_meta': str(meta)}
        path = folder / 'list.json'
        path.write_text(json.dumps({'ok': True, 'result': listing}))
        out = subprocess.run([str(self.apps['dev']), '--save-menu', str(path), *extra], capture_output=True,
                             text=True, timeout=60, check=True).stdout
        return json.loads(out)

    def test_save_menu_shows_live_slots_only_by_default(self):
        self.assertEqual(self.save_menu(), ['live:Hero001', 'live:Hero000'])

    def test_save_menu_starred_saves_with_the_dev_setting(self):
        self.assertEqual(self.save_menu('--show-starred'),
                         ['library:newest', 'live:Hero001', 'library:starred-new', 'live:Hero000',
                          'library:starred-old'])

    def test_save_library_pick_shows_while_picked(self):
        self.assertEqual(self.save_menu('--pick', 'library:other'),
                         ['live:Hero001', 'live:Hero000', 'library:other'])

    def test_show_starred_saves_defaults_off(self):
        sources = '\n'.join(p.read_text() for p in APP)
        self.assertIn('showStarredSaves = store.object(forKey: "showStarredSaves") as? Bool ?? false', sources)
        self.assertIn('Toggle("Show starred saves in save menu", isOn: $model.showStarredSaves)', sources)
        self.assertIn('developerMode && showStarredSaves', sources)
        self.assertIn('cli:save-menu', self.caps('dev')['capabilities'])

    def test_one_page_button_in_the_page_bar(self):
        """Settings / Launcher is one button at the bottom-left of the page bar, on both pages in both roles."""
        sources = {p.name: p.read_text() for p in APP}
        everything = '\n'.join(sources.values())
        self.assertEqual(everything.count('PageButton(model: model)'), 1)  # only inside PageBar
        pages = sources['LauncherPages.swift']
        bar = pages[pages.index('struct PageBar'):]
        self.assertLess(bar.index('PageButton(model: model)'), bar.index('        trailing\n'))  # first in the row
        self.assertIn('PageBar(model: model)', pages[pages.index('struct SettingsPage'):])  # Settings page
        self.assertIn('PageBar(model: model)', sources['DevLauncher.swift'])  # repo launcher front page
        self.assertIn('PageBar(model: model)', sources['main.swift'])  # player front page
        button = pages[pages.index('struct PageButton'):pages.index('struct PageBar')]
        self.assertIn('"Launcher" : "Settings"', button)
        self.assertIn('.cancelAction', button)  # Esc on the Settings page
        self.assertIn('.keyboardShortcut(",")', pages)  # ⌘,

    def test_tab_bar_fixed_width_tabs(self):
        """Settings tabs: left-aligned, one fixed width each; Dev Options appends tabs at the right and nothing
        rescales (the segmented control rescaled and once overflowed after Dev Options went off; snapshots:
        out/validation/launcher-ui-20261019/dev-settings-tabs-*.png)."""
        pages = (ROOT / 'tools/macos/Shared/LauncherPages.swift').read_text()
        page = pages[pages.index('struct SettingsPage'):pages.index('struct SettingsPageCommand')]
        self.assertIn('SettingsTabBar(model: model)', page)
        self.assertNotIn('.segmented', page)
        bar = pages[pages.index('struct SettingsTabBar'):pages.index('struct SettingsPage')]
        self.assertIn('.frame(width: Self.tabWidth, height: Self.tabHeight)', bar)
        self.assertIn('alignment: .leading', bar)
        width = float(re.search(r'static let tabWidth: CGFloat = (\d+)', bar).group(1))
        layout = float(re.search(r'static let width: CGFloat = (\d+)', pages).group(1))
        self.assertLessEqual(7 * width + 4, layout - 40)  # seven tabs fit at the same width

    def test_fixed_layout(self):
        """Click targets never move with state (maintainer, 2026-10-19): one window size for both pages and roles,
        fixed-width pop-up menus, the two start buttons fixed, status lines fixed height, Close Game always in place."""
        sources = {p.name: p.read_text() for p in APP}
        self.assertIn('.frame(width: Self.width, height: LauncherLayout.height)', sources['DevLauncher.swift'])
        self.assertEqual(sources['main.swift'].count('.frame(width: LauncherLayout.width, height: LauncherLayout.height)'),
                         2)  # player front page and Settings page
        self.assertIsNone(re.search(r'(?<!Fixed)Picker\("', sources['SettingsViews.swift']))  # all FixedPicker
        pages = sources['LauncherPages.swift']
        self.assertIn('.frame(width: Self.menuWidth', pages)
        self.assertIn('"Load Saved Game").fontWeight(.semibold).frame(width: Self.buttonWidth)', pages)
        self.assertIn('Text("Start at Title Screen").frame(width: Self.buttonWidth)', pages)
        dev = sources['DevLauncher.swift']
        view = dev[dev.index('struct LauncherView'):dev.index('struct LauncherWindowContent')]
        self.assertIn('.frame(height: Self.statusHeight', view)
        self.assertNotIn('if model.running {\n          Button("Close Game")', view)
        self.assertIn('.disabled(!model.running || model.busy)', view)
        self.assertLess(view.index('FrontPageOptions(model: model)'), view.index('"Write saves back to"'))

    def test_start_buttons_above_the_save_menu(self):
        """[Start at Title Screen] [Load Saved Game], then the save menu (saves only). Load Saved Game is the
        default unless there are no saves (then disabled, same size, and Start at Title Screen is the default)."""
        sources = '\n'.join(p.read_text() for p in APP)
        pages = (ROOT / 'tools/macos/Shared/LauncherPages.swift').read_text()
        menu = pages[pages.index('struct SaveMenu'):pages.index('struct FrontPageOptions')]
        self.assertLess(menu.index('"Start at Title Screen"'), menu.index('"Load Saved Game"'))
        self.assertLess(menu.index('"Load Saved Game"'), menu.index('Picker("Save"'))
        self.assertIn('.keyboardShortcut(hasSaves ? nil : .defaultAction)', menu)
        self.assertIn('.keyboardShortcut(hasSaves ? .defaultAction : nil)', menu)
        self.assertIn('.disabled(disabled || !hasSaves)', menu)
        self.assertNotIn('SaveChoice.title)', sources)
        self.assertNotIn('"Load Save"', sources)
        self.assertIn('("--press-title", { model.startTitle() })', sources)

    def test_turbo_speed_and_audio_on_the_front_page(self):
        sources = {p.name: p.read_text() for p in APP}
        pages = sources['LauncherPages.swift']
        front = pages[pages.index('struct FrontPageOptions'):pages.index('struct TurboControls')]
        self.assertLess(front.index('"Skip intro movies"'), front.index('TurboControls(model: model)'))
        settings = sources['SettingsViews.swift']
        self.assertNotIn('"Turbo speed"', settings)
        self.assertNotIn('"Turbo audio"', settings)
        self.assertIn('"Turbo notice"', settings)  # stays in General
        everything = '\n'.join(sources.values())
        self.assertEqual(everything.count('LabeledContent("Turbo speed")'), 1)
        self.assertEqual(everything.count('Picker("Turbo audio"'), 1)

    def test_one_target(self):
        script = (ROOT / 'tools/macos/build_dev_launcher.sh').read_text()
        self.assertIn('tools/macos/Player/main.swift', script)
        self.assertIn('Fable2DevLauncher', script)
        self.assertFalse((ROOT / 'tools/macos/DevLauncher/main.swift').exists())


if __name__ == '__main__':
    unittest.main()
