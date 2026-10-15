import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import fable2_checkpoint as control
import fable2_test_control as game

class CheckpointControlTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.session = Path(self.temporary.name)
        self.folder = self.session / 'test-state/control'
        self.folder.mkdir(parents=True)
        (self.session / 'session.json').write_text(json.dumps({'checkpoint': True}))

    def tearDown(self):
        self.temporary.cleanup()

    def test_disabled_session_is_rejected_without_request(self):
        (self.session / 'session.json').write_text('{}')
        with self.assertRaisesRegex(ValueError, '--checkpoint'):
            control.checkpoint(self.session, 'save')
        self.assertFalse((self.folder / 'checkpoint-request.txt').exists())

    def test_native_failure_is_reported(self):
        def deliver(_):
            self.assertEqual((self.folder / 'checkpoint-request.txt').read_text(), 'load\n')
            (self.folder / 'checkpoint-status.txt').write_text('error Guest mappings changed; refusing load\n')
        with patch.object(control.time, 'sleep', side_effect=deliver):
            with self.assertRaisesRegex(RuntimeError, 'Guest mappings changed'):
                control.checkpoint(self.session, 'load')

    def test_timeout_removes_unconsumed_request(self):
        with self.assertRaises(TimeoutError):
            control.checkpoint(self.session, 'save', timeout=0)
        self.assertFalse((self.folder / 'checkpoint-request.txt').exists())

    def test_busy_native_operation_cannot_be_overwritten(self):
        (self.folder / 'checkpoint-status.txt').write_text('busy Pausing subsystems\n')
        with self.assertRaisesRegex(RuntimeError, 'still running'):
            control.checkpoint(self.session, 'load')
        self.assertFalse((self.folder / 'checkpoint-request.txt').exists())

class CloseVerificationTests(unittest.TestCase):
    def test_missing_reply_is_accepted_only_after_verified_exit(self):
        with tempfile.TemporaryDirectory() as folder:
            session=Path(folder)
            (session/'test-state/control').mkdir(parents=True)
            (session/'test-state/control/pid.txt').write_text('1234')
            exited=type('Result',(),dict(returncode=1,stdout='',stderr=''))()
            with patch.object(game,'request',side_effect=TimeoutError), patch.object(game.subprocess,'run',return_value=exited):
                self.assertEqual(game.close(session),'closed normally')

    def test_process_check_error_is_not_reported_as_exit(self):
        with tempfile.TemporaryDirectory() as folder:
            session=Path(folder)
            (session/'test-state/control').mkdir(parents=True)
            (session/'test-state/control/pid.txt').write_text('1234')
            denied=type('Result',(),dict(returncode=1,stdout='',stderr='Operation not permitted'))()
            with patch.object(game,'request',return_value='closing'), patch.object(game.subprocess,'run',return_value=denied):
                with self.assertRaisesRegex(RuntimeError,'Could not verify game exit'):
                    game.close(session)

if __name__ == '__main__': unittest.main()
