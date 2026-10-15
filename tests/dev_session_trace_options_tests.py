"""Telemetry phase 1: test-session launch parameters and trace tags in tools/fable2_dev_session.py,
and the new test-control helpers in tools/fable2_test_control.py. Normal play must be unchanged:
the same `open` command, no cache seeding, and no extra game arguments."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch, MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import fable2_dev_session as dev  # noqa: E402
import fable2_test_control as control  # noqa: E402


class LaunchOptionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'Fable II.app'
        self.app.mkdir()
        self.sessions = self.root / 'sessions'
        self.session = self.sessions / 's1'
        self.state = self.session / 'test-state'
        (self.state / 'cache').mkdir(parents=True)
        (self.state / 'cache' / 'live.bin').write_text('live')
        self.seed = self.root / 'seed'
        self.seed.mkdir()
        (self.seed / 'warm.bin').write_text('warm')
        self.info = dict(hero='Hero000', base='fixture', origin={'kind': 'library'}, target_slot=None,
                         options_live_signature=None, cache_live_signature={})
        self.commands = []
        self.addCleanup(patch.stopall)
        patch.object(dev, 'APP', self.app).start()
        patch.object(dev, 'SESSIONS', self.sessions).start()
        patch.object(dev, 'CURRENT', self.sessions / 'current.json').start()
        patch.object(dev, 'prepare_session', return_value=(self.session, self.state, self.info)).start()
        patch.object(dev, 'game_pids', side_effect=[[], [4242]]).start()
        patch.object(dev.subprocess, 'run', side_effect=lambda cmd, **k: self.commands.append(cmd)).start()
        patch.object(dev.subprocess, 'Popen', MagicMock()).start()
        patch.object(dev.control, 'describe_save',
                     return_value=dict(LevelName='Caves\\BowerLake\\ThagsCave', QuestName='QC060_NewBeginning')).start()
        # Isolate the shared state launch() touches (local agent docs): the global instance registry
        # (MAIN_ROOT/out/instances, plus pgrep/ps through the mocked subprocess.run), the perf lock, the
        # instance-cap launch lock and the disk guard. Without this the test sees (and could register a
        # fake pid next to) other agents' and the player's running games.
        patch.object(dev.instances, 'REGISTRY', self.root / 'instances').start()
        patch.object(dev.instances, 'game_pids', return_value=[]).start()
        patch.object(dev.instances, 'process_info', return_value=None).start()
        patch.object(dev.instances, 'register', MagicMock()).start()
        patch.object(dev.common, 'ensure_free_space', MagicMock()).start()
        # Agent shader cache (tools/fable2_agent_cache.py): scratch folders, and no shader-cache key unless a test
        # sets one (the real latest app's key would otherwise pick the real out/agent-cache).
        patch.object(dev.agent_cache, 'CACHE_ROOT', self.root / 'agent-cache').start()
        patch.object(dev.common, 'LOCKS', self.root / 'locks').start()
        patch.object(dev.agent_cache, 'app_key', return_value=None).start()
        patch.object(dev, 'LIVE_STATE', self.root / 'live').start()
        (self.root / 'live/cache').mkdir(parents=True)
        (self.root / 'live/cache/live.bin').write_text('live')
        patch.object(dev.common, 'shared_lock', MagicMock(return_value=MagicMock(wait=0.0))).start()
        patch.object(dev.common, 'test_game_hold',
                     MagicMock(return_value=MagicMock(wait=0.0, games=[], games_wait=None, scale=1))).start()
        patch.object(dev.common, 'instance_slot', MagicMock()).start()
        patch.object(dev, 'find_own_pid', return_value=4242).start()
        patch.object(dev, 'launch_ancestry', return_value=[]).start()
        patch.dict(os.environ, FABLE2_TEST_WINDOWS_DIR=str(self.root / 'test-windows')).start()
        patch.object(dev.common, 'clone', side_effect=lambda src, dst: shutil.copytree(src, dst) if Path(src).is_dir()
                     else shutil.copy2(src, dst)).start()

    def open_command(self):
        return next(c for c in self.commands if c and c[0] == 'open')

    def test_normal_session_launch_is_unchanged(self):
        record = dev.launch('library', 'fixture', 'none')
        cmd = self.open_command()
        self.assertNotIn('--args', cmd)  # normal play: no game arguments at all
        self.assertNotIn('-g', cmd)
        self.assertEqual((self.state / 'cache' / 'live.bin').read_text(), 'live')
        self.assertFalse((self.state / 'cache' / 'warm.bin').exists())
        self.assertTrue(record['options_writeback'] and record['cache_writeback'] and record['window_writeback'])
        self.assertIsNone(record['cache_from'])

    def test_persistence_alert_args_only_for_test_launches(self):
        # The player's game: the open command carries no AppKit arguments (and no --args at all).
        dev.launch('library', 'fixture', 'none')
        play = self.open_command()
        self.assertNotIn('-ApplePersistenceIgnoreState', play)
        self.commands.clear()
        dev.game_pids.side_effect = [[], [4242]]
        dev.launch('library', 'fixture', 'none', test_control=True)
        test = self.open_command()
        args = test[test.index('--args') + 1:]
        self.assertEqual(args[-2:], list(dev.common.TEST_LAUNCH_APPKIT_ARGS))

    def test_game_args_and_cache_seed_need_a_test_session(self):
        for kwargs in (dict(game_args=['--metal_pipeline_creation_threads=4']), dict(cache_from=str(self.seed))):
            with self.assertRaises(ValueError):
                dev.launch('library', 'fixture', 'none', **kwargs)
            with self.assertRaises(ValueError):
                dev.launch('title', **kwargs)
        self.assertEqual(self.commands, [])

    def test_test_session_game_args_and_cache_seed(self):
        record = dev.launch('library', 'fixture', 'none', test_control=True, fps='uncapped',
                            game_args=['--metal_pipeline_creation_threads=4'], cache_from=str(self.seed))
        cmd = self.open_command()
        args = cmd[cmd.index('--args') + 1:]
        self.assertEqual(args, ['--audio_mute=true', '--metal_pipeline_creation_threads=4',
                                '-ApplePersistenceIgnoreState', 'YES'])
        self.assertTrue((self.state / 'cache' / 'warm.bin').exists())
        self.assertFalse((self.state / 'cache' / 'live.bin').exists())
        self.assertFalse(record['options_writeback'] or record['cache_writeback'] or record['window_writeback'])
        self.assertEqual(record['cache_from'], str(self.seed))
        saved = json.loads((self.session / 'session.json').read_text())
        self.assertEqual(saved['game_args'], args)
        # Test windows use their own remembered placement, never the copied live one.
        self.assertEqual(saved['test_window']['key'], (dev.common.SLOT or 'main') + '-1')
        self.assertIn(b'Test-window placement', (self.state / 'fable2_window.toml').read_bytes())

    def test_test_session_seeds_and_publishes_the_agent_cache(self):
        fp = dict(key='0123456789abcdef', components=dict(toolchain='t'))
        patch.object(dev.agent_cache, 'app_key', return_value=fp).start()
        patch.object(dev.agent_cache, 'branch_fingerprints', return_value={'perf-staging': fp}).start()
        shutil.rmtree(self.state / 'cache')  # the launch replaces the live clone with the agent cache seed
        record = dev.launch('library', 'fixture', 'none', test_control=True)
        rec = record['agent_cache']
        self.assertEqual((rec['key'], rec['generation'], rec['publish']), (fp['key'], 0, True))
        self.assertEqual((self.state / 'cache' / 'live.bin').read_text(), 'live')
        self.assertTrue((self.root / 'agent-cache' / fp['key'] / 'gen-000000' / 'live.bin').is_file())
        self.assertEqual(json.loads((self.session / 'session.json').read_text())['agent_cache']['key'], fp['key'])
        # After a clean exit the watcher's commit_cache publishes there (never the live cache).
        (self.state / 'cache' / 'new.bin').write_text('new')
        (self.state / 'logs').mkdir(parents=True, exist_ok=True)
        (self.state / 'logs/fable_2.log').write_text(dev.CLEAN_EXIT_MARKER + '\n')
        events = []
        dev.commit_cache(dict(record, state=str(self.state)), events.append)
        self.assertEqual((events[0]['event'], events[0]['result'], events[0]['generation']), ('agent-cache', 'published', 1))
        self.assertEqual(sorted(p.name for p in (self.root / 'live/cache').iterdir()), ['live.bin'])

    def test_normal_play_never_uses_the_agent_cache(self):
        record = dev.launch('library', 'fixture', 'none')
        self.assertIsNone(record['agent_cache'])
        self.assertFalse((self.root / 'agent-cache').exists())

    def test_missing_cache_seed_is_rejected(self):
        with self.assertRaises(ValueError):
            dev.launch('library', 'fixture', 'none', test_control=True, cache_from=str(self.root / 'missing'))

    def test_trace_tags_written_inside_the_session(self):
        dev.launch('library', 'fixture', 'none', test_control=True)
        tags = (self.state / 'logs/trace/tags-in.txt').read_text().splitlines()
        # contention: the launch's concurrent-instance tag (none in the isolated registry).
        self.assertEqual(tags, ['level_name=Caves\\BowerLake\\ThagsCave', 'quest=QC060_NewBeginning', 'contention=0'])

    def test_trace_tags_never_raise(self):
        dev.write_trace_tags(self.root / 'nonexistent' / '\0bad', dict(LevelName='x'))
        dev.write_trace_tags(self.state, {})
        self.assertFalse((self.state / 'logs/trace/tags-in.txt').exists())


class TestControlHelperTests(unittest.TestCase):
    """send / wait_reply / state / wait_for / dump against a fake control folder."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.session = Path(self.temp.name)
        (self.session / 'session.json').write_text('{"background": true}')
        self.folder = self.session / 'test-state/control'
        self.folder.mkdir(parents=True)
        self.loading = 1
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.responder, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop.set)

    def responder(self):
        while not self.stop.is_set():
            for req in self.folder.glob('*.req'):
                cmd = req.read_text()
                if cmd == 'state':
                    reply = 'ok ' + json.dumps(dict(frame=10, tags=dict(loading=self.loading, region='BowerLake')))
                    self.loading = 0
                elif cmd.startswith('trace dump'):
                    reply = 'ok /tmp/dumps/001-' + ('user-' + cmd.split()[2] if len(cmd.split()) > 2 else 'user')
                elif cmd.startswith('press'):
                    reply = 'ok pressed ' + cmd.split()[1]
                else:
                    reply = 'error unsupported command'
                (self.folder / (req.stem + '.reply')).write_text(reply)
                req.unlink()
            time.sleep(.01)

    def test_state_and_wait_for(self):
        self.assertEqual(control.state(self.session)['tags']['region'], 'BowerLake')
        matched = control.wait_for(self.session, 'loading', '==', 0, timeout=5, poll=0.01)
        self.assertEqual(matched['tags']['loading'], 0)
        with self.assertRaises(TimeoutError):
            control.wait_for(self.session, 'loading', '==', 7, timeout=0.3, poll=0.05)

    def test_dump_and_non_blocking_press(self):
        self.assertTrue(control.dump(self.session, 'probe').endswith('001-user-probe'))
        reply = control.press(self.session, 'w', 3000, wait=False)
        self.assertEqual(control.wait_reply(reply, 5), 'pressed w')

    def test_errors_raise(self):
        with self.assertRaises(RuntimeError):
            control.request(self.session, 'bogus', timeout=5)


class WindowPositionWritebackTests(unittest.TestCase):
    """Session fable2_window.toml -> live state after a normal session, against temp fixtures only."""

    OLD = b'# Last windowed position (screen coordinates).\nx = 100\ny = 200\n'
    NEW = b'# Last windowed position (screen coordinates).\nx = -1440\ny = 57\n'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.live = root / 'live'
        self.state = root / 'session/test-state'
        self.live.mkdir()
        self.state.mkdir(parents=True)
        self.addCleanup(patch.stopall)
        patch.object(dev, 'LIVE_STATE', self.live).start()  # never the real Application Support folder
        self.live_file = self.live / control.WINDOW_POSITION
        self.live_file.write_bytes(self.OLD)
        os.chmod(self.live_file, 0o640)
        (self.state / control.WINDOW_POSITION).write_bytes(self.OLD)
        self.record = dict(state=str(self.state), live_state=str(self.live),
                           window_writeback=dev.normal_window_writeback(False, {}),
                           window_live_signature=dev.window_signature(self.live))
        self.logs = []

    def session_writes(self, raw):
        (self.state / control.WINDOW_POSITION).write_bytes(raw)

    def commit(self):
        dev.commit_window_position(self.record, self.logs.append)
        return self.logs[-1]

    def test_changed_position_written_back_atomically(self):
        self.session_writes(self.NEW)
        event = self.commit()
        self.assertEqual((event['event'], event['x'], event['y']), ('window-position-saved', -1440, 57))
        self.assertEqual(self.live_file.read_bytes(), self.NEW)
        self.assertEqual(self.live_file.stat().st_mode & 0o777, 0o640)  # live permissions kept
        self.assertEqual(sorted(p.name for p in self.live.iterdir()), [control.WINDOW_POSITION])  # no temp left

    def test_missing_live_file_is_created(self):
        self.live_file.unlink()
        self.record['window_live_signature'] = None
        self.session_writes(self.NEW)
        self.assertEqual(self.commit()['event'], 'window-position-saved')
        self.assertEqual(self.live_file.read_bytes(), self.NEW)

    def test_unchanged_position_not_written(self):
        mtime = self.live_file.stat().st_mtime_ns
        self.assertEqual(self.commit()['event'], 'window-position-unchanged')
        self.assertEqual(self.live_file.stat().st_mtime_ns, mtime)

    SIZED = (b'# Last windowed frame: client-area position (screen coordinates)\n'
             b'# and size (window coordinates, points on macOS).\nx = -1440\ny = 57\nwidth = 1920\nheight = 1080\n')

    def test_size_written_back(self):
        self.session_writes(self.SIZED)
        event = self.commit()
        self.assertEqual((event['event'], event['x'], event['y'], event['width'], event['height']),
                         ('window-position-saved', -1440, 57, 1920, 1080))
        self.assertEqual(self.live_file.read_bytes(), self.SIZED)

    def test_old_position_only_file_still_valid(self):
        # Live has a sized file; an x/y-only session file (older game build) still publishes.
        self.live_file.write_bytes(self.SIZED)
        self.record['window_live_signature'] = dev.window_signature(self.live)
        self.session_writes(self.NEW)
        event = self.commit()
        self.assertEqual(event['event'], 'window-position-saved')
        self.assertNotIn('width', event)
        self.assertEqual(self.live_file.read_bytes(), self.NEW)

    def test_parse_window_position(self):
        parse = dev.parse_window_position
        self.assertEqual(parse(b'x = 1\ny = -2\n'), dict(x=1, y=-2))
        self.assertEqual(parse(b'width = 640\nheight = 360\nx = 0\ny = 0\n'),
                         dict(x=0, y=0, width=640, height=360))
        self.assertEqual(parse(b'x = 0\ny = 0\nwidth = 16384\nheight = 1\n')['width'], 16384)

    def test_invalid_toml_skipped(self):
        for raw in (b'x = 1\ny = \n', b'x = 1\n', b'x = 1.5\ny = 2\n', b'x = "1"\ny = 2\n',
                    b'x = 1\ny = 2\nz = 3\n', b'x = 99999999999\ny = 2\n', b'x = true\ny = 2\n', b'\xff\xfe',
                    # Size keys: both or neither, positive integers up to 16384, no extras.
                    b'x = 1\ny = 2\nwidth = 800\n', b'x = 1\ny = 2\nheight = 600\n',
                    b'x = 1\ny = 2\nwidth = 0\nheight = 600\n', b'x = 1\ny = 2\nwidth = 800\nheight = -600\n',
                    b'x = 1\ny = 2\nwidth = 16385\nheight = 600\n', b'x = 1\ny = 2\nwidth = 800.5\nheight = 600\n',
                    b'x = 1\ny = 2\nwidth = "800"\nheight = 600\n', b'width = 800\nheight = 600\n',
                    b'x = 1\ny = 2\nwidth = 800\nheight = 600\nwidth = 900\n',
                    b'x = 1\ny = 2\nwidth = 800\nheight = 600\nfullscreen = 1\n'):
            self.session_writes(raw)
            self.assertEqual(self.commit()['event'], 'window-position-invalid', raw)
            self.assertEqual(self.live_file.read_bytes(), self.OLD)

    def test_test_session_skipped(self):
        self.record['window_writeback'] = dev.normal_window_writeback(True, {'FABLE2_TEST_BACKGROUND': '1'})
        self.assertFalse(self.record['window_writeback'])
        self.assertFalse(dev.normal_window_writeback(False, {'FABLE2_TEST_CONTROL_DIR': '/x'}))
        # The cache's fixture opt-in does not extend to window position.
        with patch.dict('os.environ', {'FABLE2_CACHE_WRITEBACK': '1', 'FABLE2_LIVE_STATE_ROOT': str(self.live)}):
            self.assertFalse(dev.normal_window_writeback(True, {'FABLE2_TEST_BACKGROUND': '1'}))
        self.session_writes(self.NEW)
        self.assertEqual(self.commit()['event'], 'window-position-isolated')
        self.assertEqual(self.live_file.read_bytes(), self.OLD)

    def test_old_session_record_without_flag_is_isolated(self):
        del self.record['window_writeback']
        self.session_writes(self.NEW)
        self.assertEqual(self.commit()['event'], 'window-position-isolated')

    def test_live_changed_since_launch_kept(self):
        self.session_writes(self.NEW)
        self.live_file.write_bytes(b'x = 5\ny = 6\n')
        self.assertEqual(self.commit()['event'], 'window-position-error')
        self.assertEqual(self.live_file.read_bytes(), b'x = 5\ny = 6\n')

    def test_failed_publish_leaves_no_temp(self):
        self.session_writes(self.NEW)
        with patch.object(dev.os, 'replace', side_effect=OSError('disk full')):
            self.assertEqual(self.commit()['event'], 'window-position-error')
        self.assertEqual(self.live_file.read_bytes(), self.OLD)
        self.assertEqual(sorted(p.name for p in self.live.iterdir()), [control.WINDOW_POSITION])

    def test_watcher_publishes_after_exit_even_without_clean_exit(self):
        session = self.state.parent
        self.record.update(pid=4242, hero='Hero000', cache_writeback=False)
        (session / 'session.json').write_text(json.dumps(self.record))
        self.session_writes(self.NEW)  # no logs/fable_2.log: not a clean exit
        with patch.object(dev, 'pid_alive', return_value=False), patch.object(dev.time, 'sleep'):
            dev.watch(session)
        events = [json.loads(l)['event'] for l in (session / 'writeback.jsonl').read_text().splitlines()]
        self.assertEqual(events, ['watching', 'game-exited', 'window-position-saved', 'cache-isolated'])
        self.assertEqual(self.live_file.read_bytes(), self.NEW)


if __name__ == '__main__':
    unittest.main()
