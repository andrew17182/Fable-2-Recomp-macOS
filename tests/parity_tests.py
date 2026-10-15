"""f2diag parity (tools/f2diag/parity.py, docs/readiness.md "Frame-matched parity"): diffs, per-burst shift
matching, target alignment and verdicts on synthetic sessions (no game needed). Run with /usr/bin/python3."""
import json
from pathlib import Path
import random
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from f2diag import parity  # noqa: E402

W, H = 96, 64


def scene(t, change=0, noise_seed=None):
    """A frame of a moving gradient at game time t; `change` brightens a still 32x32 corner block (a rendering
    change); noise_seed adds +-1 noise to a few pixels (run-to-run noise)."""
    px = bytearray()
    rnd = random.Random(noise_seed)
    for y in range(H):
        for x in range(W):
            v = (x * 2 + y + t * 3) % 256
            r, g, b = v, (v + 80) % 256, (255 - v)
            if change and x < 32 and y < 32:
                r = min(255, r + change)
            if noise_seed is not None and rnd.random() < 0.02:
                g = max(0, min(255, g + rnd.choice((-1, 1))))
            px += bytes((r, g, b))
    return b'P6\n%d %d\n255\n' % (W, H) + bytes(px)


def make_run(root, label, frames, tick_offset=0, change=0, seed=0, target=None):
    """An f2diag run folder + session with guest captures of the given frame ids; game time = frame + tick_offset."""
    session = root / ('session-' + label)
    control = session / 'test-state/control'
    control.mkdir(parents=True)
    for i, f in enumerate(frames):
        (control / ('c%03d.ppm' % i)).write_bytes(scene(f + tick_offset, change, seed * 1000 + f))
        (control / ('c%03d.capture.json' % i)).write_text(json.dumps(
            dict(source='guest', frame_id=f, requested_frame=f)))
    run = root / label
    run.mkdir()
    fm = dict(spec='load+10', target=target if target is not None else frames[len(frames) // 2])
    (run / 'result.json').write_text(json.dumps(dict(session=str(session), frame_match=fm, verdict='ok')))
    return str(run)


def burst(center, n=9):
    return list(range(center - n // 2, center + n // 2 + 1))


class ParityTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_diff_identical_and_block(self):
        a, b, c = self.root / 'a.ppm', self.root / 'b.ppm', self.root / 'c.ppm'
        a.write_bytes(scene(5))
        b.write_bytes(scene(5))
        c.write_bytes(scene(5, change=40))
        self.assertTrue(parity.diff(a, b)['identical'])
        d = parity.diff(a, c)
        self.assertFalse(d['identical'])
        self.assertEqual(d['block_xy'], [0, 0])
        self.assertGreater(d['block_max'], 10)

    def test_shift_recovers_tick_offset(self):
        frames = burst(100) + burst(130)
        a = make_run(self.root, 'a', frames, seed=1)
        b = make_run(self.root, 'b', frames, tick_offset=2, seed=2)  # B is two ticks ahead at the same id
        res = parity.compare([a], [b], max_shift=4)
        cross = res['pairs'][0]
        self.assertEqual(cross['shifts'], [-2, -2])
        self.assertLess(cross['median'], 0.05)

    def test_targets_align_first(self):
        # B's load ended 3 frames later: its target (and captures) are 3 ids later for the same game time.
        a = make_run(self.root, 'a', burst(100), seed=1, target=100)
        b = make_run(self.root, 'b', burst(103), tick_offset=-3, seed=2, target=103)
        res = parity.compare([a], [b], max_shift=2)
        self.assertEqual(res['pairs'][0]['target_shift'], 3)
        self.assertEqual(res['pairs'][0]['shifts'], [0])

    def test_verdicts(self):
        frames = burst(100) + burst(130)
        a1 = make_run(self.root, 'a1', frames, seed=1)
        a2 = make_run(self.root, 'a2', frames, tick_offset=1, seed=2)
        b1 = make_run(self.root, 'b1', frames, seed=3)
        b2 = make_run(self.root, 'b2', frames, tick_offset=-1, seed=4)
        self.assertEqual(parity.compare([a1, a2], [b1, b2])['verdict'], 'within-noise')
        c1 = make_run(self.root, 'c1', frames, change=30, seed=5)
        c2 = make_run(self.root, 'c2', frames, change=30, tick_offset=1, seed=6)
        res = parity.compare([a1, a2], [c1, c2])
        self.assertEqual(res['verdict'], 'beyond-noise')
        self.assertEqual((res['hot_blocks'][0]['x'], res['hot_blocks'][0]['y']), (0, 0))
        self.assertEqual(parity.compare([a1], [b1])['verdict'], 'no-floor')
        far = make_run(self.root, 'far', burst(500), seed=7, target=104)  # same target, other frames
        self.assertEqual(parity.compare([a1], [far])['verdict'], 'no-match')


if __name__ == '__main__':
    unittest.main()
