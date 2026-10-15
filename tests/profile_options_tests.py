"""Exercise independent options persistence without touching user saves."""
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import fable2_dev_session as dev

OLD = bytes.fromhex('3f8000003f0000000000000000000000000000003f8000003f8000003f8000000000000200000001')
NEW = bytes.fromhex('400000003f0000000100000000000000000000003f1999983f1999983f3fffff0000000200000001')

class ProfileOptionsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.live = self.root/'live'; self.session = self.root/'sessions/one'
        self.state = self.session/'test-state'; self.backups = self.root/'backups'
        for p in (self.live, self.state):
            dest = p/dev.OPTIONS_FILE; dest.parent.mkdir(parents=True); dest.write_bytes(OLD)
        self.record = dict(pid=123, state=str(self.state), live_state=str(self.live),
                           hero='Hero000', base='fixture', target_slot=None, mode='library',
                           origin={'kind':'library'}, test_control=False, env={}, options_writeback=True,
                           options_live_signature=dev.options_signature(self.live))
        self.logs = []
        self.addCleanup(patch.stopall)
        patch.object(dev, 'BACKUPS', self.backups).start()
        patch.object(dev, 'LIVE_STATE', self.live).start()
        patch.object(dev, 'SESSIONS', self.root/'sessions').start()
    def change(self, data=NEW): (self.state/dev.OPTIONS_FILE).write_bytes(data)
    def save_record(self): (self.session/'session.json').write_text(json.dumps(self.record))
    def test_normal_library_only_options_survive_next_session(self):
        self.change(); dev.commit_options(self.session, self.record, self.logs.append)
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), NEW)
        self.assertEqual(next(self.backups.glob('*/63E83FFF')).read_bytes(), OLD)
        with patch.object(dev.control, 'apply_library_save', return_value={'folder':'Hero000'}):
            _, state, info = dev.prepare_session('library', 'fixture', 'none')
        self.assertIsNone(info['target_slot']); self.assertEqual((state/dev.OPTIONS_FILE).read_bytes(), NEW)
        self.assertFalse((self.live/dev.control.HERO_SLOTS).exists())
    def test_automated_session_never_publishes(self):
        self.change(); self.record['options_writeback'] = False
        dev.commit_options(self.session, self.record, self.logs.append)
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), OLD)
        self.assertFalse(self.backups.exists()); self.assertEqual(self.logs[0]['event'], 'options-isolated')
    def test_policy_is_independent_of_hero_writeback(self):
        self.assertTrue(dev.normal_options_writeback(False, {}))
        for test, env in [(True, {}), (False, {'FABLE2_TEST_BACKGROUND':'1'}),
                          (False, {'FABLE2_TEST_CONTROL_DIR':'owned-test'})]:
            self.assertFalse(dev.normal_options_writeback(test, env))
    def test_other_profiles_and_achievements_are_not_published(self):
        self.change()
        for name in ('63E83FFE','63E83FFD'):
            (self.state/dev.OPTIONS_FILE.parent/name).write_bytes(b'unrelated')
        (self.state/'saves/achievements').mkdir(); (self.state/'saves/achievements/4D5307F1.toml').write_text('changed')
        dev.commit_options(self.session, self.record, self.logs.append)
        self.assertEqual(list((self.live/dev.OPTIONS_FILE.parent).iterdir()), [self.live/dev.OPTIONS_FILE])
        self.assertFalse((self.live/'saves/achievements').exists())
    def test_incomplete_blob_does_not_overwrite(self):
        self.change(NEW[:-1])
        with self.assertRaises(ValueError): dev.commit_options(self.session, self.record, self.logs.append)
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), OLD); self.assertFalse(self.backups.exists())
    def test_newer_normal_preferences_win_conflict(self):
        self.change(); newer = bytes([99])+NEW[1:]; (self.live/dev.OPTIONS_FILE).write_bytes(newer)
        with self.assertRaises(RuntimeError): dev.commit_options(self.session, self.record, self.logs.append)
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), newer); self.assertFalse(self.backups.exists())
    def test_atomic_failure_retains_destination_and_backup(self):
        self.change()
        with patch.object(dev.os, 'replace', side_effect=OSError('fixture failure')):
            with self.assertRaises(OSError): dev.commit_options(self.session, self.record, self.logs.append)
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), OLD)
        self.assertEqual(next(self.backups.glob('*/63E83FFF')).read_bytes(), OLD)
        self.assertFalse(list((self.live/dev.OPTIONS_FILE.parent).glob('*.tmp-*')))
    def test_watcher_publishes_options_without_hero_change_only_when_closed(self):
        self.save_record(); calls = [0]; clock = [0]
        def alive(_):
            calls[0] += 1
            if calls[0] == 1: self.change()
            return calls[0] < 6
        def monotonic(): clock[0] += 1; return clock[0]
        with patch.object(dev, 'pid_alive', side_effect=alive), patch.object(dev, 'STABLE_SECONDS', 2), \
             patch.object(dev.time, 'monotonic', side_effect=monotonic), patch.object(dev.time, 'sleep'), \
             patch.object(dev, 'options_file_open', side_effect=[True, False]) as opened, \
             patch.object(dev, 'commit_save') as hero_commit:
            dev.watch(self.session)
        self.assertEqual(opened.call_count, 2); hero_commit.assert_not_called()
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), NEW)
    def test_source_changed_since_stability_check_is_retried(self):
        expected = dev.options_signature(self.state); self.change()
        with self.assertRaises(OSError):
            dev.commit_options(self.session, self.record, self.logs.append, expected)
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), OLD)
    def test_watcher_retries_transient_publish_failure(self):
        self.save_record(); calls = [0]; clock = [0]
        def alive(_):
            calls[0] += 1
            if calls[0] == 1: self.change()
            return calls[0] < 9
        def monotonic(): clock[0] += 1; return clock[0]
        original = dev.commit_options; attempts = [0]
        def commit(*args):
            attempts[0] += 1
            if attempts[0] == 1: raise OSError('transient fixture failure')
            return original(*args)
        with patch.object(dev, 'pid_alive', side_effect=alive), patch.object(dev, 'STABLE_SECONDS', 1), \
             patch.object(dev.time, 'monotonic', side_effect=monotonic), patch.object(dev.time, 'sleep'), \
             patch.object(dev, 'options_file_open', return_value=False), patch.object(dev, 'commit_options', side_effect=commit):
            dev.watch(self.session)
        self.assertEqual(attempts[0], 2)
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), NEW)
    def test_exit_with_pending_options_does_not_publish(self):
        self.save_record()
        def exited(_): self.change(); return False
        with patch.object(dev, 'pid_alive', side_effect=exited): dev.watch(self.session)
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), OLD)
        self.assertIn('options-discarded-on-exit', (self.session/'writeback.jsonl').read_text())
    def completed_fixture(self):
        self.change(); self.save_record(); now = time.time()
        os.utime(self.live/dev.OPTIONS_FILE, (now-100, now-100))
        os.utime(self.state/dev.OPTIONS_FILE, (now-10, now-10))
        (self.session/'writeback.jsonl').write_text(json.dumps({'event':'game-exited', 'time':time.strftime('%Y-%m-%d %H:%M:%S')})+'\n')
    def test_explicit_completed_user_options_recovery(self):
        self.completed_fixture()
        with patch.object(dev, 'game_pids', return_value=[]), patch.object(dev, 'pid_alive', return_value=False):
            result = dev.recover_completed_options(self.session)
        self.assertTrue(result['backup']); self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), NEW)
    def test_recovery_does_not_overwrite_newer_preferences(self):
        self.completed_fixture(); (self.live/dev.OPTIONS_FILE).write_bytes(OLD)
        with patch.object(dev, 'game_pids', return_value=[]), patch.object(dev, 'pid_alive', return_value=False):
            self.assertIsNone(dev.recover_completed_options(self.session))
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), OLD)
    def test_recovery_rejects_test_or_unfinished_sessions(self):
        self.completed_fixture(); self.record['test_control'] = True; self.save_record()
        with patch.object(dev, 'game_pids', return_value=[]), patch.object(dev, 'pid_alive', return_value=False):
            with self.assertRaises(ValueError): dev.recover_completed_options(self.session)
        self.assertEqual((self.live/dev.OPTIONS_FILE).read_bytes(), OLD)

if __name__ == '__main__': unittest.main()
