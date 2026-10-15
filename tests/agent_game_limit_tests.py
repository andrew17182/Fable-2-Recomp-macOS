"""Agent game limit (tools/fable2_common.py acquire_game_slots / test_game_hold / `hold`):
a counting semaphore of flock'd slot files, a separate 3x slot, FIFO queue, and release when the holder dies."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'


def run_py(code, locks, env=None, wait=True):
    e = dict(os.environ, FABLE2_LOCKS_DIR=str(locks), FABLE2_AGENT_GAMES='2', FABLE2_AGENT_3X_GAMES='1', FABLE2_YIELD_TO_PLAY='0',
             FABLE2_AGENT_GAMES_TIMEOUT='3')
    e.update(env or {})
    p = subprocess.Popen([sys.executable, '-c', 'import sys; sys.path.insert(0, %r)\n' % str(TOOLS) + code], env=e,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if not wait: return p
    out, err = p.communicate(timeout=60)
    return p.returncode, out, err


HOLD = '''
import fable2_common as c, time, sys
fds, names, wait = c.acquire_game_slots('test', %d)
print('GOT ' + '+'.join(names), flush=True)
time.sleep(%f)
'''


class AgentGameLimitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.locks = Path(self.temp.name)

    def start_holder(self, scale, seconds):
        p = run_py(HOLD % (scale, seconds), self.locks, wait=False)
        line = p.stdout.readline()
        self.assertTrue(line.startswith('GOT '), line + p.stderr.read() if not line else line)
        self.addCleanup(lambda: (p.kill(), p.wait()))
        return p, line.split()[1]

    def test_two_slots_then_third_waits_and_times_out(self):
        _, a = self.start_holder(1, 30)
        _, b = self.start_holder(1, 30)
        self.assertEqual({a, b}, {'slot-1', 'slot-2'})
        rc, out, err = run_py(HOLD % (1, 0), self.locks)
        self.assertNotEqual(rc, 0)
        self.assertIn('AGENT GAMES', err)  # queue position logged, then LockTimeout

    def test_crash_releases_slot(self):
        p, _ = self.start_holder(1, 30)
        self.start_holder(1, 30)
        q = run_py(HOLD % (1, 0), self.locks, env=dict(FABLE2_AGENT_GAMES_TIMEOUT='20'), wait=False)
        time.sleep(1.0)
        p.kill()  # a crashed holder: the kernel drops its flock
        out, err = q.communicate(timeout=30)
        self.assertEqual(q.returncode, 0, err)
        self.assertIn('GOT slot-', out)

    def test_only_one_3x_game(self):
        _, a = self.start_holder(3, 30)
        self.assertEqual(a, 'slot-1+3x-1')
        rc, out, err = run_py(HOLD % (3, 0), self.locks)
        self.assertNotEqual(rc, 0, out)  # second 3x game waits (and times out here)
        rc, out, err = run_py(HOLD % (2, 0), self.locks)
        self.assertEqual(rc, 0, err)  # a 2x game still gets the other slot
        self.assertIn('GOT slot-2', out)

    def test_hand_off_holds_until_game_exits(self):
        # The `hold` process keeps the slot fds until the game pid is gone, and lists itself as a holder.
        game = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(4)'])
        self.addCleanup(lambda: (game.kill(), game.wait()))
        code = '''
import fable2_common as c
c.shared_lock = lambda what: c.SharedHold(None, None, what)
h = c.test_game_hold('handoff', 1)
print('HOLDER', h.hand_off(%d), flush=True)
''' % game.pid
        rc, out, err = run_py(code, self.locks)
        self.assertEqual(rc, 0, err)
        holder = int(out.split()[-1])
        for _ in range(50):  # the detached holder writes its record once it runs
            rc, out, err = run_py('import fable2_common as c, json; print(json.dumps(c.game_holders()))', self.locks)
            rows = json.loads(out)
            if rows: break
            time.sleep(0.1)
        self.assertEqual([r['pid'] for r in rows], [holder])
        self.assertEqual(rows[0]['games'], ['slot-1'])
        game.wait()
        for _ in range(50):
            rc, out, err = run_py('import fable2_common as c, json; print(json.dumps(c.game_holders()))', self.locks)
            if json.loads(out) == []: break
            time.sleep(0.2)
        self.assertEqual(json.loads(out), [])
        _, a = self.start_holder(1, 1)  # the slot is free again
        self.assertEqual(a, 'slot-1')

    def test_fifo_order(self):
        # Both slots busy; A queues before B; when one slot frees, A (older ticket) gets it, B keeps waiting.
        self.start_holder(1, 30)
        p2, _ = self.start_holder(1, 2.5)
        a = run_py(HOLD % (1, 30), self.locks, env=dict(FABLE2_AGENT_GAMES_TIMEOUT='20'), wait=False)
        self.addCleanup(lambda: (a.kill(), a.wait()))
        time.sleep(0.5)
        b = run_py(HOLD % (1, 0), self.locks, env=dict(FABLE2_AGENT_GAMES_TIMEOUT='6'), wait=False)
        line = a.stdout.readline()
        self.assertTrue(line.startswith('GOT slot-'), line)
        out, err = b.communicate(timeout=30)
        self.assertNotEqual(b.returncode, 0, out)  # B never cut ahead and timed out behind A
        self.assertIn('position', err)

    def test_launch_scale(self):
        import importlib
        sys.path.insert(0, str(TOOLS))
        c = importlib.import_module('fable2_common')
        self.assertEqual(c.launch_scale({'FABLE2_RES_SCALE': '3'}), 3)
        state = Path(self.temp.name) / 'state'
        state.mkdir()
        (state / 'fable2_config.toml').write_text('[graphics]\nresolution_scale = 2  # x\n')
        self.assertEqual(c.launch_scale({}, state), 2)
        (state / 'fable2_config.toml').write_text('resolution_scale = "auto"\n')
        self.assertEqual(c.launch_scale({}, state), 3)
        self.assertEqual(c.launch_scale({}, None), 1)


if __name__ == '__main__':
    unittest.main()
