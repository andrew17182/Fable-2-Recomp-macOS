"""Run Playback in the player app (tools/macos/Player/main.swift PlaybackSession; docs/reference.md "Input recording
and replay"), the native counterpart of tools/fable2_dev_session.py --mode replay (tests/launcher_playback_tests.py):
  - `--print-env --playback REC`: the current settings, FABLE2_REPLAY_INPUT, recording forced off, intros skipped,
    the game on a temporary copy of the state (FABLE2_APP_STATE_ROOT=<state>/playback-session) with the real game files
    and built game (FABLE2_GAME_DATA_ROOT, FABLE2_GAME_BUILD_ROOT); boots the recording's own start (copy, older
    recordings: the named save as it is now, else the title);
  - the whole button path (`--press-playback`, a stand-in game): the copy holds the recording's starting saves, the
    game's writes never reach the real saves or the recording's copy, the copy is removed after the game and its
    logs are kept in logs/playback;
  - FableIILauncher (AppLauncher.m) runs the release app's built game from FABLE2_GAME_BUILD_ROOT;
  - the player role shows the Play recording picker (playbackSupported) and the front page slot; headless snapshots.
Scratch folders only; no real game is started. Runs under /usr/bin/python3 (3.9)."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ([ROOT / 'tools/macos/Player/main.swift'] + sorted((ROOT / 'tools/macos/DevLauncher').glob('*.swift'))
           + sorted((ROOT / 'tools/macos/Shared').glob('*.swift')))
SLOTS = 'B13EBABEBABEBABE/4D5307F1/00000001'
HEADERS = 'B13EBABEBABEBABE/4D5307F1/Headers/00000001'
REC = 'Fable II input 2026-10-22 10-00-00'


def write_hero(saves, hero, text):
    """saves: a saves folder (<state>/saves, or a recording's "<stem>.save" copy)."""
    slots, headers = Path(saves) / SLOTS, Path(saves) / HEADERS
    (slots / hero).mkdir(parents=True, exist_ok=True)
    (slots / hero / 'mainsave.bin').write_text(text)
    headers.mkdir(parents=True, exist_ok=True)
    (headers / (hero + '.header')).write_text('header')


def write_recording(folder, name, meta):
    path = Path(folder) / (name + '.jsonl')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(dict(schema=3, seq=0, type='state', t_us=0, state='AppLaunch', anchor=0, dt_us=0,
                                    g_us=0, gdt_us=0, meta=meta)) + '\n')
    return path


def snapshot(folder):
    return {str(p.relative_to(folder)): p.read_bytes() for p in Path(folder).rglob('*') if p.is_file()}


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class PlayerPlaybackTests(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='player-playback-')
        cls.root = Path(cls.temp.name)
        cls.app = cls.root / 'Fable II.app'
        macos = cls.app / 'Contents/MacOS'
        macos.mkdir(parents=True)
        cls.player = macos / 'FableIIPlayer'
        subprocess.run(['xcrun', 'swiftc', '-O', '-target', 'arm64-apple-macos15.0', '-o', str(cls.player)]
                       + [str(s) for s in SOURCES], check=True, capture_output=True)
        (cls.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleExecutable='FableIIPlayer', CFBundleIdentifier='org.fable2recomp.test.playback%d' % os.getpid(),
            CFBundlePackageType='APPL', CFBundleShortVersionString='0.0.0')))
        # Stand-in for FableIILauncher + the game: records its environment in its state's logs, writes the replay
        # status, and writes over the saves it was given (they must be the copy).
        fake = macos / 'FableIILauncher'
        fake.write_text('#!/bin/sh\nS="$FABLE2_APP_STATE_ROOT"\nmkdir -p "$S/logs"\n/usr/bin/env > "$S/logs/env.txt"\n'
                        'find "$S/saves" -name mainsave.bin > "$S/logs/saves.txt"\n'
                        'find "$S/saves" -name mainsave.bin -exec sh -c \'cat "$1" >> "$2/logs/saves-content.txt"; '
                        'echo "written by the game" > "$1"\' _ {} "$S" \\;\n'
                        'echo \'{"status":"complete"}\' > "$S/logs/replay-status.json"\n')
        fake.chmod(0o755)
        cls.game = cls.root / 'GameData'
        (cls.game / 'data').mkdir(parents=True)
        (cls.game / 'default.xex').write_bytes(b'stand-in')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def make_state(self, name, settings=None):
        state = self.root / 'state' / name
        state.mkdir(parents=True)
        (state / 'player-settings.plist').write_bytes(plistlib.dumps(settings or {}))
        write_hero(state / 'saves', 'Hero000', 'real now')
        write_hero(state / 'saves', 'Hero001', 'real other')
        (state / 'cache').mkdir()
        (state / 'cache/shader.bin').write_text('cache')
        (state / 'fable2_config.toml').write_text('# real config\n')
        self.recordings = state / 'logs/recordings'
        return state

    def env(self, state):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env.update(FABLE2_APP_STATE_ROOT=str(state), FABLE2_GAME_DATA_ROOT=str(self.game),
                   FABLE2_PLAYER_SHADER_PREP='0')
        return env

    def print_env(self, state, recording):
        out = subprocess.run([str(self.player), '--print-env', '--playback', recording], env=self.env(state),
                             check=True, capture_output=True, text=True, timeout=60).stdout
        return dict(item.partition('=')[::2] for item in json.loads(out))

    def test_replay_environment(self):
        state = self.make_state('env', {'recordInput': True, 'resolutionScale': '2', 'skipIntro': False})
        rec = write_recording(self.recordings, REC, dict(save='Hero001', start='save', boot_hero='Hero001',
                                                         save_copy=REC + '.save'))
        write_hero(self.recordings / (REC + '.save'), 'Hero000', 'start 0')
        write_hero(self.recordings / (REC + '.save'), 'Hero001', 'start 1')
        before = snapshot(state)
        before.pop('player-settings.plist')  # settings migrations may rewrite it
        env = self.print_env(state, rec.name)  # a file name in logs/recordings
        self.assertEqual(env['FABLE2_REPLAY_INPUT'], str(rec))
        self.assertEqual(env['FABLE2_RECORD_INPUT'], '0')
        for key in ('FABLE2_RECORD_INPUT_PATH', 'FABLE2_RECORD_INPUT_SAVE'): self.assertNotIn(key, env)
        self.assertEqual(env['FABLE2_SKIP_INTRO_MOVIES'], '1')  # intros skipped whatever the setting
        self.assertEqual(env['FABLE2_APP_STATE_ROOT'], str(state / 'playback-session'))
        self.assertEqual(env['FABLE2_GAME_DATA_ROOT'], str(self.game))
        self.assertEqual(env['FABLE2_GAME_BUILD_ROOT'], str(state))
        self.assertEqual((env['FABLE2_BOOT_SAVE'], env['FABLE2_BOOT_SAVE_HERO']), ('1', 'Hero001'))
        self.assertEqual(env['FABLE2_PLAYER_LAUNCH'], '1')
        self.assertEqual(env['FABLE2_WINDOW_TAG'], 'playback')
        self.assertNotIn('FABLE2_TEST_BACKGROUND', env)  # a normal window
        self.assertNotIn('FABLE2_PHYSICAL_INPUT', env)
        # Current settings otherwise: the same items Play sets.
        play = subprocess.run([str(self.player), '--print-env'], env=self.env(state), check=True,
                              capture_output=True, text=True, timeout=60).stdout
        play = dict(item.partition('=')[::2] for item in json.loads(play))
        self.assertEqual(play['FABLE2_RECORD_INPUT'], '1')
        self.assertEqual(env['FABLE2_RES_SCALE'], play['FABLE2_RES_SCALE'])
        after = snapshot(state)
        after.pop('player-settings.plist')
        self.assertEqual(after, before)  # --print-env copies nothing

    def test_old_recordings_fall_back(self):
        state = self.make_state('old')
        named = write_recording(self.recordings, 'Fable II input 2026-10-01 10-00-00', dict(save='Hero001',
                                                                                            boot_save='1'))
        env = self.print_env(state, str(named))  # a path
        self.assertEqual(env['FABLE2_BOOT_SAVE'], '1')
        self.assertNotIn('FABLE2_BOOT_SAVE_HERO', env)  # the copy keeps only that hero
        gone = write_recording(self.recordings, 'Fable II input 2026-10-02 10-00-00', dict(save='Hero007',
                                                                                           boot_save='1'))
        self.assertNotIn('FABLE2_BOOT_SAVE', self.print_env(state, gone.name))
        title = write_recording(self.recordings, 'Fable II input 2026-10-03 10-00-00',
                                dict(save='', start='title', save_copy='x.save'))
        self.assertNotIn('FABLE2_BOOT_SAVE', self.print_env(state, title.name))

    def run_playback(self, state, recording):
        proc = subprocess.Popen([str(self.player), '--press-playback', '--recording', recording],
                                env=dict(self.env(state), FABLE2_TEST_BACKGROUND='1'),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.time() + 60
            kept = state / 'logs/playback/replay-status.json'
            while time.time() < deadline and not (kept.exists() and not (state / 'playback-session').exists()):
                time.sleep(0.1)
            self.assertTrue(kept.exists(), 'the playback never finished')
        finally:
            proc.terminate()
            proc.wait(timeout=10)

    def test_playback_runs_on_a_copy_and_writes_nothing_back(self):
        state = self.make_state('press', {'recordInput': True})
        rec = write_recording(self.recordings, REC, dict(save='', start='save', boot_hero='', save_copy=REC + '.save'))
        write_hero(self.recordings / (REC + '.save'), 'Hero000', 'at record start')
        before = snapshot(state)
        self.run_playback(state, rec.name)
        kept = state / 'logs/playback'
        game_env = dict(l.partition('=')[::2] for l in (kept / 'env.txt').read_text().splitlines() if '=' in l)
        self.assertEqual(game_env['FABLE2_REPLAY_INPUT'], str(rec))
        self.assertEqual(game_env['FABLE2_RECORD_INPUT'], '0')
        self.assertEqual(game_env['FABLE2_BOOT_SAVE'], '1')
        # The game saw only the recording's starting save, and wrote into the copy.
        self.assertEqual((kept / 'saves-content.txt').read_text(), 'at record start')
        self.assertEqual(len((kept / 'saves.txt').read_text().splitlines()), 1)
        self.assertFalse((state / 'playback-session').exists())  # the copy is gone
        after = snapshot(state)
        added = {k for k in after if k not in before}
        self.assertTrue(all(k.startswith(('logs/playback/', 'logs/player.log', 'app.lock')) for k in added), added)
        for key, data in before.items():
            if key == 'player-settings.plist': continue  # the picked recording is remembered
            self.assertEqual(after[key], data, key)  # real saves, cache, the recording and its copy untouched
        self.assertIn(b'playbackRecording', after['player-settings.plist'])

    def test_launcher_runs_the_built_game_from_the_real_folder(self):
        """AppLauncher.m in a release app (Fable2BuildsGame): FABLE2_GAME_BUILD_ROOT holds Game/."""
        app = self.root / 'Release.app'
        (app / 'Contents/MacOS').mkdir(parents=True)
        launcher = app / 'Contents/MacOS/FableIILauncher'
        subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'AppKit', '-framework', 'Metal', '-o', str(launcher),
                        str(ROOT / 'tools/macos/AppLauncher.m')], check=True, capture_output=True)
        (app / 'Contents/Resources').mkdir()
        for name in ('fable2_config.toml', 'fable2_patches.toml'):  # the defaults it copies into a new state
            (app / 'Contents/Resources' / name).write_text('# default\n')
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleExecutable='FableIILauncher', CFBundleIdentifier='org.fable2recomp.test.buildroot',
            CFBundlePackageType='APPL', Fable2BuildsGame=True)))
        real = self.root / 'release-real'
        engine = real / 'Game/Fable II.bundle/Contents/MacOS/fable_2'
        engine.parent.mkdir(parents=True)
        marker = self.root / 'engine-ran.txt'
        engine.write_text('#!/bin/sh\necho "$FABLE2_APP_STATE_ROOT" > "%s"\n' % marker)
        engine.chmod(0o755)
        copy = real / 'playback-session'
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env.update(FABLE2_APP_STATE_ROOT=str(copy), FABLE2_GAME_BUILD_ROOT=str(real),
                   FABLE2_GAME_DATA_ROOT=str(self.game), FABLE2_SKIP_INTRO_MOVIES='1')
        subprocess.run([str(launcher)], env=env, check=True, timeout=20, capture_output=True)
        self.assertEqual(marker.read_text().strip(), str(copy))
        self.assertFalse((copy / 'Game').exists())
        self.assertFalse((real / 'launcher.plist').exists())  # the per-launch game folder is not remembered

    def test_player_role_shows_playback(self):
        settings = (ROOT / 'tools/macos/Shared/SettingsViews.swift').read_text()
        self.assertIn('if model.playbackSupported {', settings)
        player = (ROOT / 'tools/macos/Player/main.swift').read_text()
        model = player[player.index('final class PlayerModel'):player.index('// MARK: - Views')]
        self.assertIn('override var playbackSupported: Bool { true }', model)
        self.assertIn('playback: model.playbackSlot, runPlayback: { model.runPlayback() }', player)

    def test_snapshots(self):
        """Headless pictures of Diagnostics (the picker) and the front page (Run Playback, Replaying)."""
        state = self.make_state('snap', {'developerMode': True})
        rec = write_recording(self.recordings, REC, dict(save='Hero000', start='save', save_copy=REC + '.save'))
        out = self.root / 'snapshots'
        out.mkdir(exist_ok=True)
        for name, args in (('diagnostics', ['--window', 'diagnostics', '--dev-options', 'on',
                                            '--recording', rec.name]),
                           ('front', ['--window', 'play', '--recording', rec.name]),
                           ('replaying', ['--window', 'play', '--recording', rec.name, '--replaying'])):
            with self.subTest(name):
                png = out / (name + '.png')
                subprocess.run([str(self.player), '--snapshot', str(png)] + args, env=self.env(state), check=True,
                               capture_output=True, timeout=60)
                self.assertGreater(png.stat().st_size, 10000)
                if os.environ.get('PLAYBACK_SNAPSHOT_DIR'):
                    shutil.copy2(png, Path(os.environ['PLAYBACK_SNAPSHOT_DIR']) / png.name)


if __name__ == '__main__':
    unittest.main()
