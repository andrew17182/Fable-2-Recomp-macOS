"""Agent games yield to the player's game (tools/fable2_common.py play_games / acquire_game_slots):
new agent launches wait while a play game runs, dead play pids never block, FABLE2_YIELD_TO_PLAY=0 turns it off.
Scratch lock folder only. Run: /usr/bin/python3 tests/play_yield_tests.py"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import fable2_common as common  # noqa: E402
import fable2_instances as instances  # noqa: E402

OLD = time.strftime('%a %b %d %H:%M:%S %Y', time.localtime(time.time() - 3600))
NEW = time.strftime('%a %b %d %H:%M:%S %Y', time.localtime(time.time() - 2))

# A launch whose play check is driven by a flag file (a fake play game "runs" while the file exists).
ACQUIRE = '''
import fable2_common as c, os, sys, time
from pathlib import Path
flag = Path(%r)
c.play_games = lambda current=None: [dict(pid=4242, role='play')] if flag.exists() else []
fds, names, wait = c.acquire_game_slots('test', 1, poll=0.1)
print('GOT ' + '+'.join(names), flush=True)
'''


class PlayYieldTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.tmp = Path(self.temp.name)

    def test_which_games_count_as_play(self):
        slot_exe = str(common.SLOTS_DIR / 'agent-1/out/build/x/fable_2')
        main_exe = '/Applications/Fable II.app/Contents/MacOS/fable_2'
        current = [
            dict(pid=1, role='play', test_control=False),
            dict(pid=2, role='playtest', test_control=False),
            dict(pid=3, role='agent', test_control=True),
            dict(pid=4, role='untracked', exe=main_exe, lstart=OLD, test_control=None),   # player app / hand start
            dict(pid=5, role='untracked', exe=main_exe, lstart=NEW, test_control=None),   # a launch about to register
            dict(pid=6, role='untracked', exe=slot_exe, lstart=OLD, test_control=None),   # an agent slot's build
            dict(pid=7, role='untracked', exe=str(common.MAIN_ROOT / 'out/benchmark/app/x/fable_2'), lstart=OLD),
            dict(pid=8, role='play', test_control=True),                                   # launcher test session
        ]
        self.assertEqual([e['pid'] for e in common.play_games(current)], [1, 2, 4])

    def test_dead_and_reused_pids_never_block(self):
        saved = instances.REGISTRY
        instances.REGISTRY = self.tmp / 'instances'
        instances.REGISTRY.mkdir()
        try:
            dead = subprocess.Popen(['true'])
            dead.wait()
            (instances.REGISTRY / ('%d.json' % dead.pid)).write_text(json.dumps(dict(pid=dead.pid, role='play')))
            # A live pid that is not a fable_2 (reused pid): pruned too.
            (instances.REGISTRY / ('%d.json' % os.getpid())).write_text(json.dumps(dict(pid=os.getpid(), role='play')))
            current = [e for e in instances.instances() if e['role'] != 'untracked']  # ignore real games on this Mac
            self.assertEqual(common.play_games(current), [])
            self.assertEqual(list(instances.REGISTRY.glob('*.json')), [])
        finally:
            instances.REGISTRY = saved

    def run_acquire(self, flag, env=None):
        e = dict(os.environ, FABLE2_LOCKS_DIR=str(self.tmp / 'locks'), FABLE2_AGENT_GAMES='2',
                 FABLE2_AGENT_GAMES_TIMEOUT='20')
        e.update(env or {})
        return subprocess.Popen([sys.executable, '-c', 'import sys; sys.path.insert(0, %r)\n' % str(TOOLS) +
                                 ACQUIRE % str(flag)], env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def test_launch_waits_while_playing_and_starts_after(self):
        flag = self.tmp / 'playing'
        flag.touch()
        p = self.run_acquire(flag)
        queue = self.tmp / 'locks/games/queue'
        deadline = time.monotonic() + 10
        ticket = None
        while time.monotonic() < deadline:  # wait on the ticket (a readiness signal), not a fixed time
            tickets = list(queue.glob('*.json')) if queue.is_dir() else []
            if tickets:
                ticket = json.loads(tickets[0].read_text())
                if ticket.get('play_wait_since'): break
            time.sleep(0.05)
        self.assertTrue(ticket and ticket.get('play_wait_since'), ticket)
        self.assertEqual(ticket.get('waiting_for_play'), [4242])
        self.assertIsNone(p.poll())  # still waiting: no slot taken while a play game runs
        flag.unlink()  # the play game exits
        out, err = p.communicate(timeout=20)
        self.assertEqual(p.returncode, 0, err)
        self.assertIn('GOT slot-1', out)
        self.assertIn("waiting for the play session (pid 4242, play)", err)
        self.assertIn('play session ended', err)

    def test_switch_off(self):
        flag = self.tmp / 'playing'
        flag.touch()
        p = self.run_acquire(flag, dict(FABLE2_YIELD_TO_PLAY='0'))
        out, err = p.communicate(timeout=20)
        self.assertIn('GOT slot-1', out)
        self.assertNotIn('play session', err)


if __name__ == '__main__':
    unittest.main()
