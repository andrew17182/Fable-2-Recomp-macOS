import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('summary', Path(__file__).resolve().parents[1] / 'tools/summarize_frame_log.py')
s = importlib.util.module_from_spec(spec)
spec.loader.exec_module(s)

def row(event, frame, time, warmup=0, duration=0):
    return dict(event=event, frame_id=frame, time_us=time, warmup=warmup, duration_us=duration)

class SummaryTests(unittest.TestCase):
    def test_repeats_and_synthetic_frames_do_not_inflate_fps(self):
        rows = [row('synthetic_swap', 9, 0), row('presented', 9, 500)]
        for i in range(1, 5):
            rows += [row('guest_swap', i, i*33333), row('presented', i, i*33333+1000), row('presented', i, i*33333+17667)]
        r = s.analyze(sorted(rows, key=lambda r:r['time_us']))
        self.assertEqual(r['unique_presented_frames']['samples'], 4)
        self.assertEqual(r['repeated_presentations'], 4)
        self.assertAlmostEqual(r['unique_presented_frames']['average_fps'], 30, places=2)
        self.assertGreater(r['all_presentations']['average_fps'], 59)

    def test_ranges_warmup_and_stalls(self):
        rows = [row('guest_swap', 1, 0, 1), row('warmup_end', 0, 100000),
                row('guest_swap', 2, 200000), row('guest_swap', 3, 216667),
                row('guest_swap', 4, 300000), row('guest_swap', 5, 500000)]
        r = s.analyze(rows, start=.2, duration=.2)
        self.assertEqual(r['guest_submissions']['samples'], 3)
        self.assertEqual(r['guest_submissions']['gaps_over_50ms'], 1)
        self.assertAlmostEqual(r['guest_submissions']['one_percent_low_fps'], 12, places=2)
        self.assertEqual(s.analyze(rows, phase='warmup')['guest_submissions']['samples'], 1)

    def test_skipped_and_missing_timestamps_are_separate(self):
        rows = [row('guest_swap', 1, 1), row('present_skipped', 1, 2),
                row('present_unavailable', 1, 3), row('dropped', 0, 4, duration=8)]
        r = s.analyze(rows)
        self.assertEqual(r['skipped_presentations'], 1)
        self.assertEqual(r['present_callbacks_without_timestamp'], 1)
        self.assertEqual(r['dropped_log_events_entire_file'], 8)
        self.assertEqual(r['unique_presented_frames']['samples'], 0)

    def test_empty(self):
        self.assertEqual(s.analyze([])['unique_presented_frames']['samples'], 0)

if __name__ == '__main__': unittest.main()
