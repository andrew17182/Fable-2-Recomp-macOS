"""The player app (tools/macos/Player/main.swift, the portable app's launcher window) must give the game the
same environment as the Dev Launcher for the same settings: both use LaunchSettings in
tools/macos/Shared/LaunchSettings.swift, and this checks the end result.

Dev Launcher side: `--launch --print-args` (the tools/fable2_dev_session.py arguments), turned into the game
environment the way tools/fable2_dev_session.py launch() does (checked against its source below).
Player side: the real Play path (`--play`) with the settings in <state>/player-settings.plist, exec'ing a stand-in
FableIILauncher that records its environment and arguments. Also checks "Skip launcher, play directly".

Builds both apps with swiftc (skipped without Xcode). Runs under /usr/bin/python3 (3.9)."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHARED = sorted((ROOT / 'tools/macos/Shared').glob('*.swift'))
# One app, two roles (docs/player-app.md "Developer mode"): both binaries are the same sources.
APP = [ROOT / 'tools/macos/Player/main.swift'] + sorted((ROOT / 'tools/macos/DevLauncher').glob('*.swift')) + SHARED
DEV_SESSION = (ROOT / 'tools/fable2_dev_session.py').read_text()

# Settings combinations: player-settings.plist keys (the same keys the Dev Launcher stores) and the Dev
# Launcher CLI flags that set the same values.
CASES = [
    ({}, []),
    ({'resolutionScale': '2', 'frameRate': '30', 'fpsCounter': True, 'skipIntro': True},
     ['--resolution', '2', '--fps', '30', '--fps-counter', '--skip-intro']),
    ({'resolutionScale': 'auto', 'frameRate': 'uncapped', 'displayMode': 'fullscreen', 'upscaling': 'metalfx-spatial',
      'effectsSize': 'full', 'effectsDefaultsVersion': 1, 'turboNotice': 'game', 'turboSpeed': 3.5, 'slowSpeed': 0.35, 'turboAudio': 'high-pitch',
      'skipIntro': False},
     ['--resolution', 'auto', '--fps', 'uncapped', '--display', 'fullscreen', '--upscaling', 'metalfx-spatial',
      '--effects', 'full', '--turbo-notice', 'game', '--turbo-speed', '3.5', '--slow-speed', '0.35',
      '--turbo-audio', 'high-pitch',
      '--no-skip-intro']),
    ({'frameRate': '60', 'furFix': False, 'nativeShaders': False, 'rttVerify': True, 'tracePreset': 'gpu',
      'portableShaderCache': False, 'hitchTrace': False, 'extraEnv': 'FABLE2_SYNC_TEST=1 REX_SYNC_TEST=x'},
     ['--fps', '60', '--no-fur-fix', '--no-native-shaders', '--rtt-verify', '--trace-preset', 'gpu',
      '--no-portable-shader-cache', '--no-hitch-trace', '--extra-env', 'FABLE2_SYNC_TEST=1 REX_SYNC_TEST=x']),
    ({'frameRate': 'settings', 'unlockExtras': False, 'ringReinitFix': False, 'tileEdram': False, 'resolutionScale': '3'},
     ['--fps', 'settings', '--no-unlock-extras', '--no-ring-reinit-fix', '--no-tile-edram', '--resolution', '3']),
    # Frame generation (FABLE2_FRAME_INTERP, docs/macos-metal.md "Frame generation"); always explicit.
    ({'frameGeneration': 'on', 'frameRate': '30'}, ['--frame-generation', 'on', '--fps', '30']),
    # A "Full resolution" effects size stored before 2026-10-22 is reset to native once (effectsDefaultsVersion).
    ({'effectsSize': 'full'}, []),
    # "Skip cosmetic waits" (docs/load-fixes-20261016.md section F).
    ({'fastPacing': True}, ['--fast-pacing']),
    # Controls tab (tools/macos/Shared/ControlsView.swift).
    ({'keyboardMap': 'Return:Pause,E:A,LMB:X', 'mouseLook': True, 'mouseLookScale': 700,
      'hotkeyBinds': {'bind_settings': 'F4', 'bind_console': 'None'}, 'viewButtonToGame': True},
     ['--keyboard-map', 'Return:Pause,E:A,LMB:X', '--mouse-look', '--mouse-look-scale', '700',
      '--bind', 'bind_settings=F4', '--bind', 'bind_console=None', '--view-button-to-game']),
    # Fixes > Turbo: precise sleeps (default on, passed only when off).
    ({'turboFineDelay': False}, ['--no-turbo-fine-delay']),
    # Fixes > Display > Lower display latency (FABLE2_PRESENT_DRAWABLES, default on, passed only when off).
    ({'presentDrawables2': False}, ['--no-present-drawables-2']),
    # Fixes > Display > Sync frames to the display (FABLE2_VSYNC_PHASE_LOCK, default on, passed only when off).
    ({'vsyncPhaseLock': False}, ['--no-vsync-phase-lock']),
    # Controls > Game controllers > Works in background (default on).
    ({'padBackground': False}, ['--no-pad-background']),
    # Diagnostics > Record input (src/diagnostics/fable2_session_journal.h).
    ({'recordInput': True}, ['--record-input']),
    # Controller hotkeys (FABLE2_PAD_*).
    ({'padTurbo': 'rs', 'padScreenshot': 'view'}, ['--pad-turbo', 'rs', '--pad-screenshot', 'view']),
    ({'padTurbo': 'none', 'padScreenshot': 'none'}, ['--pad-turbo', 'none', '--pad-screenshot', 'none']),
    ({'padSlow': 'ls'}, ['--pad-slow', 'ls']),
    # Presets (docs/reference.md "Presets"): a stored preset applies its values; --preset does the same for one launch.
    ({'preset': 'original'}, ['--preset', 'original']),
    ({'preset': 'original', 'resolutionScale': '2'}, ['--preset', 'original']),  # stored Original wins over stale keys
    ({'preset': 'custom', 'customPreset': {'resolutionScale': '3', 'turboSpeed': 1.0}},
     ['--resolution', '3', '--turbo-speed', '1.0']),
    # Stick deadzones (FABLE2_STICK_*, src/input/stick_deadzone.h).
    ({'stickDeadzoneLeft': 5, 'stickDeadzoneRight': 0}, ['--stick-deadzone-left', '5', '--stick-deadzone-right', '0']),
    # A stored value of the old "Skip game deadzone" toggle (until 2026-10-22) is ignored by both apps.
    ({'stickDeadzoneLeft': 5, 'stickAntiDeadzone': False}, ['--stick-deadzone-left', '5']),
    # Walk deadzone (FABLE2_WALK_DEADZONE): the left-stick turn-on-the-spot band.
    ({'walkDeadzone': 5, 'stickDeadzoneLeft': 6}, ['--walk-deadzone', '5', '--stick-deadzone-left', '6']),
]
# Set only by the player: Play's own marker for FableIILauncher. Skip intro is explicit (1 or 0) in both.
PLAYER_ONLY = {'FABLE2_PLAYER_LAUNCH': '1'}


def dev_session_env(args):
    """The game environment tools/fable2_dev_session.py launch() builds from these arguments (title mode)."""
    env = {}
    if '--fps' in args:
        fps = args[args.index('--fps') + 1]
        env.update(FABLE2_FPS_60='1' if fps == '60' else '0', REX_VSYNC='0' if fps == 'uncapped' else '1')
    env['FABLE2_SKIP_INTRO_MOVIES'] = '0' if '--no-skip-intro' in args else '1'
    for i, arg in enumerate(args):
        if arg == '--env':
            key, _, value = args[i + 1].partition('=')
            env[key] = value
    return env


def swiftc(output, sources, target):
    subprocess.run(['xcrun', 'swiftc', '-O', '-target', target, '-o', str(output)] + [str(s) for s in sources],
                   check=True, capture_output=True)


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class LauncherEnvSyncTests(unittest.TestCase):
    maxDiff = None
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='launcher-env-sync-')
        cls.root = Path(cls.temp.name)
        # A fresh binary name = a fresh UserDefaults domain, so the Dev Launcher starts from the defaults.
        cls.dev = cls.root / 'dev/LauncherEnvSyncDevLauncher'
        cls.player = cls.root / 'app/MacOS/FableIIPlayer'
        cls.dev.parent.mkdir(parents=True)
        cls.player.parent.mkdir(parents=True)
        swiftc(cls.dev, APP, 'arm64-apple-macos15.0')
        swiftc(cls.player, APP, 'arm64-apple-macos15.0')
        # Stand-in for FableIILauncher (AppLauncher.m): records what Play hands it.
        fake = cls.player.parent / 'FableIILauncher'
        fake.write_text('#!/bin/sh\n/usr/bin/env > "$RECORD_DIR/env.txt"\nprintf "%s\\n" "$@" > "$RECORD_DIR/args.txt"\n')
        fake.chmod(0o755)
        cls.game = cls.root / 'GameData'
        (cls.game / 'data').mkdir(parents=True)
        (cls.game / 'default.xex').write_bytes(b'stand-in')
        # ShaderPrepRunner.stamp of a bare binary (no Info.plist keys): version|seed id|bundle version.
        cls.prep_stamp = '||'

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def dev_env(self, flags):
        out = subprocess.run([str(self.dev), '--launch', '--print-args', '--pick', 'title'] + flags,
                             check=True, capture_output=True, text=True).stdout
        return dev_session_env(json.loads(out))

    def player_run(self, settings, args, name):
        state = self.root / 'state' / name
        record = self.root / 'record' / name
        state.mkdir(parents=True)
        record.mkdir(parents=True)
        (state / 'player-settings.plist').write_bytes(plistlib.dumps(settings))
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env.update(FABLE2_APP_STATE_ROOT=str(state), FABLE2_GAME_DATA_ROOT=str(self.game), RECORD_DIR=str(record))
        # The shader step (docs/offline-shader-seed.md) would run the stand-in launcher first: not under test here.
        (state / 'cache').mkdir()
        (state / 'cache/shader-prep.json').write_text(json.dumps({'stamp': self.prep_stamp}))
        subprocess.run([str(self.player)] + args, env=env, check=True, capture_output=True, timeout=60)
        recorded = {}
        for line in (record / 'env.txt').read_text().splitlines():
            key, sep, value = line.partition('=')
            if sep and key.startswith(('FABLE2_', 'REX_')): recorded[key] = value
        for key in ('FABLE2_APP_STATE_ROOT', 'FABLE2_GAME_DATA_ROOT'): recorded.pop(key, None)
        return recorded, (record / 'args.txt').read_text().splitlines()

    def test_same_environment_as_dev_launcher(self):
        for i, (settings, flags) in enumerate(CASES):
            with self.subTest(settings=settings):
                expected = self.dev_env(flags)
                player, args = self.player_run(settings, ['--play', '--audio_mute=true'], 'case%d' % i)
                for key, value in PLAYER_ONLY.items(): self.assertEqual(player.pop(key), value)
                self.assertEqual(player, expected)
                self.assertEqual(args, ['--audio_mute=true'])  # player flags consumed, the rest reach the game

    def test_fast_pacing_umbrella(self):
        # Default off and always explicit; the sub-switches are left to the game (an explicit value of one wins there).
        self.assertEqual(self.dev_env([])['FABLE2_FAST_PACING'], '0')
        for name, settings in (('fp-on', {'fastPacing': True}), ('fp-legacy', {'shortLoadFades': True}),
                               ('fp-off', {'fastPacing': False, 'shortLoadFades': True})):
            with self.subTest(settings=settings):
                player, _ = self.player_run(settings, ['--play'], name)
                self.assertEqual(player['FABLE2_FAST_PACING'], '0' if name == 'fp-off' else '1')
                self.assertNotIn('FABLE2_LOAD_FADE_FRAMES', player)
                self.assertNotIn('FABLE2_SAVE_NOTICE_MS', player)

    def test_frame_generation(self):
        # Default off and always explicit (docs/macos-metal.md "Frame generation").
        self.assertEqual(self.dev_env([])['FABLE2_FRAME_INTERP'], '0')
        self.assertEqual(self.dev_env(['--frame-generation', 'on'])['FABLE2_FRAME_INTERP'], '1')
        player, _ = self.player_run({'frameGeneration': 'on'}, ['--play'], 'fg-on')
        self.assertEqual(player['FABLE2_FRAME_INTERP'], '1')

    def test_extra_env_never_makes_play_background(self):
        # A saved FABLE2_TEST_BACKGROUND in Extra environment must not start Play games in the background (note 21).
        extra = 'FABLE2_TEST_BACKGROUND=1 FABLE2_SYNC_TEST=1'
        dev = self.dev_env(['--extra-env', extra])
        player, _ = self.player_run({'extraEnv': extra}, ['--play'], 'extra-bg')
        for env in (dev, player):
            self.assertNotIn('FABLE2_TEST_BACKGROUND', env)
            self.assertEqual(env['FABLE2_SYNC_TEST'], '1')

    def test_play_hands_activation_to_the_game(self):
        # Both roles' Play activate the started game by pid (not by bundle id), except in background test mode.
        player = (ROOT / 'tools/macos/Player/main.swift').read_text()
        dev = (ROOT / 'tools/macos/DevLauncher/DevLauncher.swift').read_text()
        self.assertNotIn('yieldActivation(toApplicationWithBundleIdentifier', player)
        self.assertIn('GameActivation.handOff(to: process.processIdentifier)', player)
        self.assertIn('GameActivation.handOff(to: pid_t(pid))', dev)

    def test_skip_launcher_plays_directly(self):
        player, _ = self.player_run({'skipLauncher': True, 'resolutionScale': '2'}, [], 'skip')
        self.assertEqual(player['FABLE2_RES_SCALE'], '2')

    def test_dev_session_mapping_unchanged(self):
        # dev_session_env() above mirrors these lines of launch(); update both together.
        self.assertIn("env.update(FABLE2_FPS_60='1' if fps=='60' else '0', REX_VSYNC='0' if fps=='uncapped' else '1')",
                      DEV_SESSION)
        self.assertIn("env['FABLE2_SKIP_INTRO_MOVIES'] = '1' if skip_intro else '0'", DEV_SESSION)


if __name__ == '__main__':
    unittest.main()
