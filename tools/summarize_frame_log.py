#!/usr/bin/env python3
"""Summarize Metal frame recordings. Times are seconds since recorder startup."""
import argparse
import csv
import json
import math
from pathlib import Path


def percentile(values, percent):
    values = sorted(values)
    pos = (len(values) - 1) * percent / 100
    low = int(pos)
    return values[low] + (values[min(low + 1, len(values) - 1)] - values[low]) * (pos - low)


def summarize_times(times):
    times = sorted(times)
    gaps = [(b - a) / 1000 for a, b in zip(times, times[1:]) if b > a]
    if not gaps:
        return {'samples': len(times), 'intervals': 0}
    worst = sorted(gaps, reverse=True)[:max(1, math.ceil(len(gaps) * .01))]
    return dict(samples=len(times), intervals=len(gaps),
                average_fps=1000 * len(gaps) / sum(gaps),
                one_percent_low_fps=1000 / (sum(worst) / len(worst)),
                p50_ms=percentile(gaps, 50), p95_ms=percentile(gaps, 95),
                p99_ms=percentile(gaps, 99), max_ms=max(gaps),
                gaps_over_33ms=sum(t > 33 for t in gaps),
                gaps_over_50ms=sum(t > 50 for t in gaps))


def read_rows(path):
    rows, malformed = [], 0
    with path.open() as stream:
        reader = csv.DictReader(stream)
        for row in reader:
            try:
                rows.append(dict(event=row['event'], **{k: int(row[k]) for k in
                    ('frame_id', 'time_us', 'duration_us', 'warmup')}))
            except (ValueError, TypeError, KeyError):
                malformed += 1
    return sorted(rows, key=lambda r: r['time_us']), malformed


def analyze(rows, start=0, duration=None, phase='post-warmup'):
    # A synthetic capture swap must never contribute to gameplay FPS.
    guest_ids = {r['frame_id'] for r in rows if r['event'] == 'guest_swap'}
    end = (start + duration) * 1e6 if duration is not None else math.inf
    selected = [r for r in rows if start * 1e6 <= r['time_us'] < end and
                (phase == 'all' or r['warmup'] == (phase == 'warmup'))]
    valid = [r for r in selected if r['frame_id'] in guest_ids]
    displayed = [r for r in valid if r['event'] == 'presented']
    unique = {}
    for r in displayed:
        unique.setdefault(r['frame_id'], r['time_us'])
    waits = [r['duration_us'] / 1000 for r in valid if r['event'] == 'pipeline_wait']
    return {
        'phase': phase, 'start_seconds': start, 'duration_seconds': duration,
        'warmup_end_seconds': next((r['time_us'] / 1e6 for r in rows if r['event'] == 'warmup_end'), None),
        'guest_submissions': summarize_times([r['time_us'] for r in valid if r['event'] == 'guest_swap']),
        'unique_presented_frames': summarize_times(list(unique.values())),
        'all_presentations': summarize_times([r['time_us'] for r in displayed]),
        'repeated_presentations': len(displayed) - len(unique),
        'skipped_presentations': sum(r['event'] == 'present_skipped' for r in valid),
        'present_callbacks_without_timestamp': sum(r['event'] == 'present_unavailable' for r in valid),
        'pipeline_wait_count': len(waits), 'pipeline_wait_total_ms': sum(waits),
        'pipeline_wait_max_ms': max(waits, default=0),
        'dropped_log_events_entire_file': sum(r['duration_us'] for r in rows if r['event'] == 'dropped'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', nargs='?', type=Path, help='Defaults to latest normal-app frame recording')
    window = parser.add_mutually_exclusive_group()
    window.add_argument('--start', type=float, default=0, help='Seconds since recorder startup')
    window.add_argument('--last', type=float, help='Analyze the last N recorded seconds')
    parser.add_argument('--duration', type=float, help='Seconds to analyze')
    parser.add_argument('--phase', choices=['all', 'warmup', 'post-warmup'], default='post-warmup')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    if args.start < 0 or (args.duration is not None and args.duration <= 0) or (args.last is not None and args.last <= 0):
        parser.error('start must be nonnegative and duration must be positive')
    path = args.csv
    if path is None:
        folder = Path.home() / 'Library/Application Support/Fable II Recomp/logs'
        paths = list(folder.glob('frames-*.csv'))
        if not paths:
            parser.error('No frame recordings found; launch the updated app first')
        path = max(paths, key=lambda p: p.stat().st_mtime)
    rows, malformed = read_rows(path)
    if args.last is not None:
        args.start = max(0, max((r['time_us'] for r in rows), default=0) / 1e6 - args.last)
    report = analyze(rows, args.start, args.duration, args.phase)
    report.update(file=str(path.resolve()), malformed_rows=malformed)
    if args.json:
        print(json.dumps(report, indent=2))
        return
    print(f'File: {path}\nPhase: {args.phase}; range starts at {args.start:g}s')
    print('Post-warmup can still include videos, menus, pauses and loading screens.')
    for label, key in [('Guest submissions', 'guest_submissions'), ('Unique frames presented', 'unique_presented_frames'), ('All presentations (includes repeats)', 'all_presentations')]:
        s = report[key]
        if not s.get('intervals'):
            print(f'{label}: insufficient samples ({s["samples"]})')
            continue
        print(f'{label}: {s["average_fps"]:.2f} FPS average; {s["one_percent_low_fps"]:.2f} FPS 1% low; {s["samples"]} samples')
        print(f'  Frame times: p50 {s["p50_ms"]:.2f}, p95 {s["p95_ms"]:.2f}, p99 {s["p99_ms"]:.2f}, max {s["max_ms"]:.2f} ms')
        print(f'  Gaps >33ms: {s["gaps_over_33ms"]}; >50ms: {s["gaps_over_50ms"]}')
    print(f'Repeated presentations: {report["repeated_presentations"]}; pipeline waits: {report["pipeline_wait_count"]} ({report["pipeline_wait_total_ms"]:.2f}ms total)')
    print(f'Skipped presentations (Metal reported zero presentedTime): {report["skipped_presentations"]}')
    print('1% low = reciprocal of mean slowest 1% intervals. Final ~0.5s may be unsaved at shutdown.')
    if malformed or report['dropped_log_events_entire_file']:
        print(f'INCOMPLETE LOG: malformed rows={malformed}, dropped events={report["dropped_log_events_entire_file"]}')
    if report['present_callbacks_without_timestamp']:
        print('Some Metal presentation timestamps were unavailable; displayed-frame statistics may be incomplete.')


if __name__ == '__main__':
    main()
