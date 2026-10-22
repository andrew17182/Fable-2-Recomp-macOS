"""Playback of input recordings (launcher Run Playback; docs/reference.md "Input recording and replay").

tools/fable2_dev_session.py launch --mode replay:
  - the session boots the recording's own starting saves ("<stem>.save/", made by the game at record start);
    older recordings boot the save their metadata names (live slot or library entry, copied), else the title;
  - it never writes back (no live slot, no save-library copy, no options write-back), recording is forced off
    even when the caller's settings turn it on, intros are skipped, and the current settings (--env) are passed;
  - normal play otherwise: visible (no `open -g`), sound on (no --audio_mute), the launcher's current game;
  - status reports the game's replay status (logs/replay-status.json).
The launcher (Swift): `--launch --replay X --print-args` gives those arguments with the current settings, never
FABLE2_RECORD_INPUT=1; `--recordings DIR` lists the picker's entries newest first.
Scratch folders only; no game is started. Runs under /usr/bin/python3 (3.9)."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch, MagicMock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fable2_dev_session as dev  # noqa: E402
import fable2_test_control as control  # noqa: E402

CONTAINER = 'B13EBABEBABEBABE/4D5307F1'


def write_hero(saves, hero, text='hero'):
    folder = Path(saves) / CONTAINER / '00000001' / hero
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'mainsave.bin').write_text(text)
    headers = Path(saves) / CONTAINER / 'Headers/00000001'
    headers.mkdir(parents=True, exist_ok=True)
    (headers / (hero + '.header')).write_text('header')


def write_recording(path, meta):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(dict(schema=3, seq=0, type='state', t_us=0, state='AppLaunch', anchor=0, dt_us=0,
                                    g_us=0, gdt_us=0, meta=meta)) + '\n'
                    + json.dumps(dict(schema=3, seq=1, type='recording_end', t_us=1, anchor=0, dt_us=1, g_us=1,
                                      gdt_us=1, reason='quit')) + '\n')
    return path


class ReplayStartTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.live = self.root / 'live'
        write_hero(self.live / 'saves', 'Hero000', 'live now')
        self.recordings = self.live / 'logs/recordings'
        self.addCleanup(patch.stopall)
        patch.object(dev.control, 'library_dirs', return_value=[self.root / 'library']).start()
        patch.object(dev.control, 'SAVE_LIBRARY', self.root / 'library').start()

    def test_save_copy_boots_its_save(self):
        rec = write_recording(self.recordings / 'Fable II input 2026-10-22 10-00-00.jsonl',
                              dict(save='Hero000', start='save', save_copy='Fable II input 2026-10-22 10-00-00.save'))
        write_hero(self.recordings / 'Fable II input 2026-10-22 10-00-00.save', 'Hero000', 'at record start')
        start = dev.replay_start(rec, self.live)
        self.assertEqual((start['kind'], start['boot'], start['hero'], start['note']), ('copy', True, 'Hero000', None))

    def test_save_copy_title_start(self):
        rec = write_recording(self.recordings / 'r.jsonl', dict(save='', start='title', save_copy='r.save'))
        write_hero(self.recordings / 'r.save', 'Hero000')
        write_hero(self.recordings / 'r.save', 'Hero001')
        start = dev.replay_start(rec, self.live)
        self.assertEqual((start['kind'], start['boot'], start['note']), ('copy', False, None))

    def test_old_recording_boots_named_live_slot_with_note(self):
        rec = write_recording(self.recordings / 'old.jsonl', dict(save='Hero000', boot_save='1'))
        start = dev.replay_start(rec, self.live)
        self.assertEqual((start['kind'], start['save'], start['boot']), ('live', 'Hero000', True))
        self.assertIn('No save copy', start['note'])

    def test_old_recording_without_its_save_starts_at_title_with_note(self):
        rec = write_recording(self.recordings / 'old.jsonl', dict(save='Hero007', boot_save='1'))
        start = dev.replay_start(rec, self.live)
        self.assertEqual((start['kind'], start['boot']), ('title', False))
        self.assertEqual(start['note'], 'No save copy: started at the title screen.')


class ReplayLaunchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'Fable II.app'
        self.app.mkdir()
        self.live = self.root / 'live'
        write_hero(self.live / 'saves', 'Hero000', 'live now')
        write_hero(self.live / 'saves', 'Hero001', 'other live')
        (self.live / 'fable2_config.toml').write_text('# live config\n')
        self.recordings = self.live / 'logs/recordings'
        self.rec = write_recording(self.recordings / 'Fable II input 2026-10-22 10-00-00.jsonl',
                                   dict(save='Hero000', start='save',
                                        save_copy='Fable II input 2026-10-22 10-00-00.save'))
        write_hero(self.recordings / 'Fable II input 2026-10-22 10-00-00.save', 'Hero000', 'at record start')
        self.sessions = self.root / 'sessions'
        self.commands = []
        self.addCleanup(patch.stopall)
        patch.object(dev, 'APP', self.app).start()
        patch.object(dev.playtest, 'app_for', return_value=self.app).start()
        patch.object(dev.playtest, 'data_root_for', return_value=self.root / 'data').start()
        patch.object(dev.skip_intro_movies, 'ensure', MagicMock()).start()
        patch.object(dev, 'SESSIONS', self.sessions).start()
        patch.object(dev, 'CURRENT', self.sessions / 'current.json').start()
        patch.object(dev, 'LIVE_STATE', self.live).start()
        patch.object(dev, 'prune_sessions', MagicMock()).start()
        patch.object(dev.common, 'prune_validation', MagicMock()).start()
        patch.object(dev, 'game_pids', side_effect=[[], [4242]]).start()
        patch.object(dev.subprocess, 'run', side_effect=lambda cmd, **k: self.commands.append(cmd)).start()
        patch.object(dev.subprocess, 'Popen', MagicMock()).start()
        patch.object(dev.instances, 'REGISTRY', self.root / 'instances').start()
        patch.object(dev.instances, 'game_pids', return_value=[]).start()
        patch.object(dev.instances, 'instances', return_value=[]).start()
        patch.object(dev.instances, 'process_info', return_value=None).start()
        patch.object(dev.instances, 'register', MagicMock()).start()
        patch.object(dev.instances, 'contention', return_value=dict(others=[], text='')).start()
        patch.object(dev.common, 'ensure_free_space', MagicMock()).start()
        patch.object(dev.common, 'LOCKS', self.root / 'locks').start()
        patch.object(dev.common, 'test_game_hold',
                     MagicMock(return_value=MagicMock(wait=0.0, games=[], games_wait=None, scale=1))).start()
        patch.object(dev.common, 'instance_slot', MagicMock()).start()
        patch.object(dev.agent_cache, 'seed', return_value=None).start()
        patch.object(dev, 'find_own_pid', return_value=4242).start()
        patch.object(dev, 'pid_alive', return_value=True).start()
        patch.object(dev, 'launch_ancestry', return_value=[]).start()
        patch.dict(os.environ, FABLE2_TEST_WINDOWS_DIR=str(self.root / 'test-windows')).start()
        patch.object(dev.common, 'clone', side_effect=lambda src, dst: shutil.copytree(src, dst) if Path(src).is_dir()
                     else shutil.copy2(src, dst)).start()

    def open_command(self):
        return next(c for c in self.commands if c and c[0] == 'open')

    def env(self):
        cmd = self.open_command()
        return dict(cmd[i + 1].split('=', 1) for i, a in enumerate(cmd) if a == '--env')

    def live_snapshot(self):
        return {str(p.relative_to(self.live)): p.read_bytes() for p in self.live.rglob('*') if p.is_file()}

    def test_playback_boots_the_copy_with_current_settings_and_never_records(self):
        before = self.live_snapshot()
        record = dev.launch('replay', None, None, extra_env=['FABLE2_RECORD_INPUT=1', 'FABLE2_STICK_DEADZONE_LEFT=6',
                                                            'FABLE2_RES_SCALE=2'],
                            skip_intro=False, recording=str(self.rec))
        env = self.env()
        self.assertEqual(env['FABLE2_REPLAY_INPUT'], str(self.rec.resolve()))
        self.assertEqual(env['FABLE2_RECORD_INPUT'], '0')
        self.assertNotIn('FABLE2_RECORD_INPUT_PATH', env)
        self.assertEqual(env['FABLE2_SKIP_INTRO_MOVIES'], '1')  # intros skipped whatever the setting
        self.assertEqual(env['FABLE2_BOOT_SAVE'], '1')
        self.assertEqual((env['FABLE2_STICK_DEADZONE_LEFT'], env['FABLE2_RES_SCALE']), ('6', '2'))
        cmd = self.open_command()
        self.assertNotIn('-g', cmd)  # visible, in front
        self.assertNotIn('--audio_mute=true', cmd)  # normal sound
        self.assertNotIn('FABLE2_PHYSICAL_INPUT=0', cmd)
        state = Path(record['state'])
        slots = state / 'saves' / CONTAINER / '00000001'
        self.assertEqual(sorted(p.name for p in slots.iterdir()), ['Hero000'])
        self.assertEqual((slots / 'Hero000/mainsave.bin').read_text(), 'at record start')
        self.assertIsNone(record['target_slot'])
        self.assertFalse(record['options_writeback'])
        self.assertEqual(record['replay']['start']['kind'], 'copy')
        self.assertEqual(json.loads((self.sessions / 'current.json').read_text())['replay']['recording'],
                         str(self.rec.resolve()))
        self.assertEqual(self.live_snapshot(), before)  # the live state is only read

    def test_playback_refuses_a_write_back_target(self):
        with self.assertRaises(ValueError):
            dev.launch('replay', None, 'Hero000', recording=str(self.rec))
        with self.assertRaises(ValueError):
            dev.launch('replay', None, None, recording=str(self.root / 'missing.jsonl'))

    def test_saves_made_during_playback_stay_in_the_session(self):
        record = dev.launch('replay', None, None, recording=str(self.rec))
        library = self.root / 'library'
        events = []
        with patch.object(dev, 'LIBRARY', library):
            dev.commit_save(record['session'], record, 'Hero000', events.append)
        self.assertFalse(library.exists())
        self.assertEqual(events[0]['reason'], 'playback session')

    def test_status_reports_the_replay(self):
        record = dev.launch('replay', None, None, recording=str(self.rec))
        self.assertEqual(dev.status()['replay']['status'], 'starting')
        logs = Path(record['state']) / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        (logs / 'replay-status.json').write_text('{"status":"running","driver":"game"}\n')
        self.assertEqual(dev.status()['replay']['status'], 'running')

    def test_old_recording_note_reaches_the_launcher(self):
        old = write_recording(self.recordings / 'Fable II input 2026-10-01 10-00-00.jsonl',
                              dict(save='Hero001', boot_save='1'))
        record = dev.launch('replay', None, None, recording=str(old))
        slots = Path(record['state']) / 'saves' / CONTAINER / '00000001'
        self.assertEqual(sorted(p.name for p in slots.iterdir()), ['Hero001'])
        self.assertIn('No save copy: booted Hero001', record['note'])

    def test_title_start_does_not_boot(self):
        title = write_recording(self.recordings / 'Fable II input 2026-10-02 10-00-00.jsonl',
                                dict(save='', start='title', save_copy='Fable II input 2026-10-02 10-00-00.save'))
        write_hero(self.recordings / 'Fable II input 2026-10-02 10-00-00.save', 'Hero000')
        write_hero(self.recordings / 'Fable II input 2026-10-02 10-00-00.save', 'Hero001')
        dev.launch('replay', None, None, recording=str(title))
        self.assertNotIn('FABLE2_BOOT_SAVE', self.env())


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode or the command-line tools')
class LauncherPlaybackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='launcher-playback-')
        cls.root = Path(cls.temp.name)
        sources = [ROOT / 'tools/macos/Player/main.swift'] + sorted((ROOT / 'tools/macos/DevLauncher').glob('*.swift')) \
            + sorted((ROOT / 'tools/macos/Shared').glob('*.swift'))
        app = cls.root / 'Test.app'
        (app / 'Contents/MacOS').mkdir(parents=True)
        subprocess.run(['xcrun', 'swiftc', '-O', '-target', 'arm64-apple-macos15.0', '-o',
                        str(app / 'Contents/MacOS/FableIIPlayer')] + [str(s) for s in sources],
                       check=True, capture_output=True)
        cls.domain = 'org.fable2recomp.test.playback%d' % os.getpid()
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
            CFBundleExecutable='FableIIPlayer', CFBundleIdentifier=cls.domain, CFBundlePackageType='APPL',
            Fable2DevLauncher=True, Fable2RepoRoot=str(ROOT))))
        cls.binary = app / 'Contents/MacOS/FableIIPlayer'

    @classmethod
    def tearDownClass(cls):
        subprocess.run(['defaults', 'delete', cls.domain], capture_output=True)
        cls.temp.cleanup()

    def run_app(self, *args):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('FABLE2_', 'REX_'))}
        env.update(FABLE2_LIVE_STATE_ROOT=str(self.root / 'live'), FABLE2_REPO_ROOT=str(ROOT))
        return json.loads(subprocess.run([str(self.binary)] + list(args), env=env, capture_output=True, text=True,
                                         timeout=60, check=True).stdout)

    def test_replay_launch_arguments_use_current_settings_and_never_record(self):
        rec = '/tmp/x/Fable II input 2026-10-22 10-00-00.jsonl'
        args = self.run_app('--launch', '--print-args', '--replay', rec, '--record-input', '--resolution', '2',
                            '--pick', 'live:Hero000')
        self.assertEqual(args[args.index('--mode') + 1], 'replay')
        self.assertEqual(args[args.index('--recording') + 1], rec)
        self.assertEqual(args[args.index('--target-slot') + 1], 'none')
        envs = [args[i + 1] for i, a in enumerate(args) if a == '--env']
        self.assertIn('FABLE2_RES_SCALE=2', envs)
        self.assertFalse([e for e in envs if e.startswith('FABLE2_RECORD_INPUT=')])
        self.assertNotIn('--save', args)
        self.assertNotIn('--no-skip-intro', args)
        # Without --replay the same settings record as usual.
        normal = self.run_app('--launch', '--print-args', '--record-input', '--pick', 'live:Hero000')
        self.assertIn('FABLE2_RECORD_INPUT=1', normal)

    def test_recordings_newest_first(self):
        folder = self.root / 'live/logs/recordings'
        for name, meta in (('Fable II input 2026-10-21 09-01-02.jsonl', dict(save='', start='title')),
                           ('Fable II input 2026-10-22 10-24-15.jsonl', dict(save='Hero000', start='save')),
                           ('Fable II input 2026-10-22 08-00-00 2.jsonl', dict(save='hero1-dog-live'))):
            write_recording(folder / name, meta)
        (folder / 'Fable II input 2026-10-22 10-24-15.save').mkdir(exist_ok=True)  # the save copy: not listed
        rows = self.run_app('--recordings', str(folder))
        self.assertEqual([r['label'] for r in rows], ['2026-10-22 10:24:15 · Hero000',
                                                      '2026-10-22 08:00:00 · hero1-dog-live',
                                                      '2026-10-21 09:01:02 · Title'])

    def test_front_page_slot_is_fixed(self):
        """Run Playback, "▶ Replaying" and the recording note share one fixed slot right of the start buttons."""
        pages = (ROOT / 'tools/macos/Shared/LauncherPages.swift').read_text()
        menu = pages[pages.index('struct SaveMenu'):pages.index('/// "● Recording input"')]
        self.assertLess(menu.index('"Load Saved Game"'), menu.index('PlaybackButton('))
        self.assertLess(menu.index('PlaybackButton('), menu.index('Picker("Save"'))  # same row as the buttons
        self.assertIn('.frame(width: RecordingBadge.width, alignment: .trailing)', menu)
        button = pages[pages.index('struct PlaybackButton'):pages.index('struct ReplayingBadge')]
        self.assertIn('.opacity(label == nil ? 0 : 1)', button)  # hidden, never removed


if __name__ == '__main__':
    unittest.main()
