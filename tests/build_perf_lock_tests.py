"""Builds don't overlap perf runs (tools/fable2_common.py build_marker / build_perf_hold):
a build holds perf.lock shared, so it waits for an exclusive perf run and a perf run waits for it; play builds
(FABLE2_BUILD_ROLE=play) don't wait but still show as build markers."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'

PERF = '''
import fable2_common as c, time
with c.perf_lock('perf test') as w:
    print('PERF %%.1f' %% w, flush=True)
    time.sleep(%f)
'''
BUILD = '''
import fable2_common as c, time, json
with c.build_marker(1, 'unit build'):
    print('BUILD ' + json.dumps([r.get('what') for r in c.running_builds()]), flush=True)
    time.sleep(%f)
'''


class BuildPerfLockTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.env = dict(os.environ, FABLE2_LOCKS_DIR=self.temp.name, FABLE2_PERF_LOCK_TIMEOUT='20',
                        FABLE2_PERF_LOCK_SHARED_TIMEOUT='20', FABLE2_HEAVY_BUILD_WAIT='5')
        for k in ('FABLE2_BUILD_ROLE', 'FABLE2_BUILD_HELD', 'FABLE2_PERF_LOCK_HOLDER', 'FABLE2_PERF_LOCK_SHARED'):
            self.env.pop(k, None)

    def start(self, code, **extra):
        p = subprocess.Popen([sys.executable, '-c', 'import sys; sys.path.insert(0, %r)\n' % str(TOOLS) + code],
                             env=dict(self.env, **extra), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (p.kill(), p.wait(), p.stdout.close(), p.stderr.close()))
        return p

    def test_build_waits_for_perf_run(self):
        perf = self.start(PERF % 2.5)
        self.assertTrue(perf.stdout.readline().startswith('PERF'))
        started = time.monotonic()
        build = self.start(BUILD % 0)
        line = build.stdout.readline()
        self.assertTrue(line.startswith('BUILD'), line + build.stderr.read())
        self.assertGreater(time.monotonic() - started, 1.5)  # waited for the perf run
        self.assertIn('unit build', line)

    def test_perf_run_waits_for_build(self):
        build = self.start(BUILD % 2.5)
        self.assertTrue(build.stdout.readline().startswith('BUILD'))
        rows = json.loads(subprocess.check_output(
            [sys.executable, '-c', 'import sys; sys.path.insert(0, %r)\nimport fable2_common as c, json\n'
             'print(json.dumps(c.shared_holders("perf")))' % str(TOOLS)], env=self.env, text=True))
        self.assertEqual([r.get('kind') for r in rows], ['build'])  # visible as a shared holder
        perf = self.start(PERF % 0)
        line = perf.stdout.readline()
        self.assertTrue(line.startswith('PERF'), line + perf.stderr.read())
        self.assertGreater(float(line.split()[1]), 1.5)  # the perf run's recorded lock wait

    def test_play_build_does_not_wait(self):
        perf = self.start(PERF % 5)
        self.assertTrue(perf.stdout.readline().startswith('PERF'))
        started = time.monotonic()
        build = self.start(BUILD % 0, FABLE2_BUILD_ROLE='play')
        line = build.stdout.readline()
        self.assertTrue(line.startswith('BUILD'), line + build.stderr.read())
        self.assertLess(time.monotonic() - started, 2.0)
        self.assertIn('unit build', line)  # still a visible build marker

    def test_build_hold_cli_runs_command(self):
        out = subprocess.check_output([sys.executable, str(TOOLS / 'fable2_common.py'), 'build-hold', '--what', 'cli',
                                       '--', sys.executable, '-c', 'import os; print(os.environ["FABLE2_BUILD_HELD"])'],
                                      env=self.env, text=True)
        self.assertEqual(out.strip(), '1')


if __name__ == '__main__':
    unittest.main()
