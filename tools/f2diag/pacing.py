"""Frame pacing from the Metal frame CSV (works with FABLE2_TRACE=off too).

Replaces flash-20261007/health.py, cr-fixes-20261007/sweepstats.py and the per-window parts of
tools/summarize_frame_log.py (kept as the CSV summary CLI). The CSV clock is mach uptime minus the
frame log's origin; the origin is the "Metal frame log:" line's wall time in fable_2.log, so a window
given in wall time (time.time()) maps onto CSV time within a few ms.
"""
import csv
import datetime
import re
from pathlib import Path

_LINE = re.compile(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\] .*Metal frame log: ')


def csv_origin_wall(logs):
    """Wall time (s) of the frame CSV's origin, from the 'Metal frame log:' log line."""
    for line in (Path(logs) / 'fable_2.log').read_text(errors='replace').splitlines():
        m = _LINE.match(line)
        if m:
            return datetime.datetime.strptime(m[1], '%Y-%m-%d %H:%M:%S.%f').timestamp()
    return None


def percentile(values, pct):
    v = sorted(values)
    if not v:
        return None
    pos = (len(v) - 1) * pct / 100
    lo = int(pos)
    return v[lo] + (v[min(lo + 1, len(v) - 1)] - v[lo]) * (pos - lo)


def window(session_or_logs, wall_from=None, wall_to=None, event='guest_swap'):
    """FPS, p50/p95/p99/max frame interval (ms) and 1% low over a wall-time window."""
    p = Path(session_or_logs)
    logs = next((c for c in (p, p / 'logs', p / 'test-state/logs') if (c / 'fable_2.log').exists()), None)
    if logs is None:
        raise FileNotFoundError(f'no fable_2.log under {p}')
    files = sorted(logs.glob('frames-*.csv'))
    if not files:
        raise FileNotFoundError(f'no frame CSV in {logs}')
    origin = csv_origin_wall(logs) or 0.0
    times = []
    for r in csv.DictReader(open(files[-1])):
        if r['event'] != event:
            continue
        t = origin + int(r['time_us']) / 1e6
        if (wall_from is None or t >= wall_from) and (wall_to is None or t < wall_to):
            times.append(t)
    gaps = [(b - a) * 1000 for a, b in zip(times, times[1:])]
    if not gaps:
        return dict(frames=len(times))
    worst = sorted(gaps, reverse=True)[:max(1, len(gaps) // 100)]
    return dict(frames=len(times), seconds=round(times[-1] - times[0], 3),
                fps=round(len(gaps) / (times[-1] - times[0]), 2), one_percent_low_fps=round(1000 / (sum(worst) / len(worst)), 1),
                p50_ms=round(percentile(gaps, 50), 2), p95_ms=round(percentile(gaps, 95), 2),
                p99_ms=round(percentile(gaps, 99), 2), max_ms=round(max(gaps), 1),
                over_33ms=sum(g > 33.4 for g in gaps), over_50ms=sum(g > 50 for g in gaps))
