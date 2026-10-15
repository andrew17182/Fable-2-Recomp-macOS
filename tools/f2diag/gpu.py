"""GPU time per frame and per pass from a FABLE2_GPU_TIME_TRACE=1 session (moved from
out/validation/weak-mac-perf-20261008/gpustats.py + passstats.py).

Frame F is the interval guest_swap F -> guest_swap F+1; frame-CSV events tagged F belong to it.
- GPU ms/frame = union of the frame's command-buffer GPU intervals (gpu_cb + gpu_cb_sa), the
  "GPU mean / p95" of docs/weak-mac-perf-20261008.md.
- present_ms = the presenter command buffer (gpu_present: MetalFX upscale, output pass, ImGui), recorded
  since 2026-10-12; gpu_ms_incl_present is the union of both (the presenter shares the queue).
- tr_n_* rows are per-frame counters (barrier dedup, shared-memory collapses, upload categories).
- Pass rows (passes-*.csv, [pass-trace]) give per-encoder GPU time, grouped by kind/site/key.
- Main-pass breaks: a render pass of the main key (largest GPU span) whose next render pass has the
  same key, i.e. the pass was ended and then resumed. Grouped by end reason (the [upload-precommit]
  details upload_pre:* / upload_xfer:* / transfer:* split the old upload / transfer reasons) and by
  the cause groups of the upload-break work: staged upload, resolve/EDRAM transfer, texture upload, other.

CLI: python3 tools/f2diag.py gpu <session> [--start 20 --dur 30] [--top 12] [--json out.json]
"""
import collections
import csv
import json
from pathlib import Path

from .pacing import percentile


def _logs(session):
    p = Path(session)
    for c in (p, p / 'logs', p / 'test-state/logs'):
        if list(c.glob('frames-*.csv')):
            return c
    raise FileNotFoundError(f'no frames-*.csv under {p} (run with FABLE2_GPU_TIME_TRACE=1)')


def _newest(logs, pattern):
    files = list(logs.glob(pattern))
    return max(files, key=lambda q: q.stat().st_mtime) if files else None


def _union(iv):
    tot, cb, ce = 0, None, None
    for b, e in sorted(iv):
        if ce is None or b > ce:
            if ce is not None:
                tot += ce - cb
            cb, ce = b, e
        else:
            ce = max(ce, e)
    return tot + (ce - cb if ce is not None else 0)


def cause_group(reason):
    """Map a pass end reason to the cause groups of the upload-break analysis."""
    if reason.startswith('upload_') or reason == 'shared_memory_upload_before_draw_pass':
        return 'staged_upload'
    if reason in ('resolve_needs_boundary', 'transfer:rt_transfer'):
        return 'resolve_edram_transfer'
    if reason == 'texture_upload_before_draw_pass' or reason.startswith('texture:'):
        return 'texture_upload'
    return 'other'


def analyze(session, start=20.0, dur=30.0, top=12):
    logs = _logs(session)
    fcsv = _newest(logs, 'frames-*.csv')
    t0, t1 = start * 1e6, (start + dur) * 1e6
    swaps, own, ctr = {}, collections.defaultdict(list), collections.defaultdict(lambda: collections.defaultdict(int))
    cbs, waits = [], collections.defaultdict(lambda: collections.defaultdict(int))
    pres, pres_all = collections.defaultdict(list), []
    for r in csv.DictReader(open(fcsv)):
        if r.get('duration_us') is None:  # truncated last row (game closed mid-write)
            continue
        e, f = r['event'], int(r['frame_id'])
        if e == 'guest_swap':
            swaps[f] = int(r['time_us'])
        elif e in ('gpu_cb', 'gpu_cb_sa'):
            t, d = int(r['time_us']), int(r['duration_us'])
            own[f].append((t, t + d))
            cbs.append((t, t + d))
        elif e == 'gpu_present':  # binned by start time below (the UI thread's frame tag is unreliable)
            t, d = int(r['time_us']), int(r['duration_us'])
            pres_all.append((t, t + d))
        elif e in ('tr_slot_wait_us', 'tr_sync_wait_us', 'tr_cp_idle_us'):
            waits[e][f] += int(r['duration_us'])
        elif e.startswith('tr_n_'):
            ctr[e][f] += int(r['duration_us'])
    frames = sorted(f for f, t in swaps.items() if t0 <= t <= t1 and f + 1 in swaps and swaps[f + 1] <= t1)
    if not frames:
        raise ValueError('no frames in the window')
    nf = len(frames)
    starts = [(swaps[f], f) for f in frames]
    for b, e in sorted(pres_all):
        for t, f in starts:  # few thousand rows: a linear scan per row is fine
            if t <= b < swaps[f + 1]:
                pres[f].append((b, e))
                break
    walls = [swaps[f + 1] - swaps[f] for f in frames]
    gpu = [_union(own[f]) for f in frames]
    # GPU occupancy: union of all command-buffer GPU intervals over the frames' wall span, and the
    # idle gaps between them (bubbles where no command buffer was running).
    lo, hi = swaps[frames[0]], swaps[frames[-1] + 1]
    merged = []
    for b, e in sorted(cbs):
        if e <= lo or b >= hi:
            continue
        b, e = max(b, lo), min(e, hi)
        if merged and b <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([b, e])
    busy = sum(e - b for b, e in merged)
    gaps = [b2 - e1 for (_, e1), (b2, _) in zip(merged, merged[1:])]
    wall_total = sum(walls)
    out_gpu = dict(busy_pct=round(100.0 * busy / max(1, hi - lo), 2),
                   idle_ms_per_frame=round((hi - lo - busy) / nf / 1000, 3),
                   idle_gaps_per_frame=round(len(gaps) / nf, 2),
                   idle_gaps_over_100us_per_frame=round(sum(g > 100 for g in gaps) / nf, 3),
                   idle_gap_p95_us=round(percentile(gaps, 95), 1) if gaps else 0)
    blocked = {e: round(100.0 * sum(m.get(f, 0) for f in frames) / wall_total, 2) for e, m in waits.items()}
    out = dict(session=str(session), frames=nf, window_s=[start, start + dur],
               gpu_occupancy=out_gpu, gpu_commands_blocked_pct=blocked,
               fps=round(1e6 * nf / sum(walls), 2),
               wall_ms=dict(mean=round(sum(walls) / nf / 1000, 3), p95=round(percentile(walls, 95) / 1000, 3),
                            p99=round(percentile(walls, 99) / 1000, 3)),
               gpu_ms=dict(mean=round(sum(gpu) / nf / 1000, 3), p50=round(percentile(gpu, 50) / 1000, 3),
                           p95=round(percentile(gpu, 95) / 1000, 3), p99=round(percentile(gpu, 99) / 1000, 3)),
               present_ms=dict(mean=round(sum(_union(pres[f]) for f in frames) / nf / 1000, 3),
                               frames_with_present=sum(1 for f in frames if pres[f]),
                               cb_us_p50=round(percentile([e - b for f in frames for b, e in pres[f]] or [0], 50), 1),
                               cb_us_p90=round(percentile([e - b for f in frames for b, e in pres[f]] or [0], 90), 1),
                               cbs_per_frame=round(sum(len(pres[f]) for f in frames) / nf, 2)),
               gpu_ms_incl_present=dict(
                   mean=round(sum(_union(own[f] + pres[f]) for f in frames) / nf / 1000, 3),
                   p95=round(percentile([_union(own[f] + pres[f]) for f in frames], 95) / 1000, 3)),
               counters={e: round(sum(m.get(f, 0) for f in frames) / nf, 3) for e, m in sorted(ctr.items())})
    pcsv = _newest(logs, 'passes-*.csv')
    if not pcsv:
        return out
    fset = set(frames)
    groups = collections.defaultdict(lambda: dict(n=0, span=0.0, vs=0.0, fs=0.0, draws=0, ends=collections.Counter()))
    tot = collections.Counter()
    renders = []  # (frame, key, end_reason, span) in file order
    for r in csv.DictReader(open(pcsv)):
        if r.get('fs_us') is None:
            continue  # last row cut off when the game was closed mid-write
        f = int(r['frame'])
        if r['kind'] == 'R':
            renders.append((f, r['key'], r['end_reason'], max(0.0, float(r['span_us']))))
        if f not in fset:
            continue
        span = max(0.0, float(r['span_us']))
        g = groups[(r['kind'], r['site'], r['key'])]
        g['n'] += 1
        g['span'] += span
        g['vs'] += max(0.0, float(r['vs_us']))
        g['fs'] += max(0.0, float(r['fs_us']))
        g['draws'] += int(r['draws'])
        g['ends'][r['end_reason']] += 1
        tot['n'] += 1
        tot[r['kind']] += 1
        tot['span'] += span
    out['encoders_per_frame'] = {k: round(tot[k] / nf, 2) for k in ('n', 'R', 'C', 'B')}
    out['encoder_span_ms_per_frame'] = round(tot['span'] / nf / 1000, 3)
    ranked = sorted(groups.items(), key=lambda kv: -kv[1]['span'])
    out['passes'] = [dict(kind=k[0], site=k[1], key=k[2], ms_per_frame=round(d['span'] / nf / 1000, 3),
                          n_per_frame=round(d['n'] / nf, 2), us_per_enc=round(d['span'] / d['n'], 1),
                          vs_ms=round(d['vs'] / nf / 1000, 3), fs_ms=round(d['fs'] / nf / 1000, 3),
                          draws_per_enc=round(d['draws'] / d['n'], 1),
                          ends={e: round(c / nf, 3) for e, c in d['ends'].most_common()})
                     for k, d in ranked[:top]]
    main = next((k for k, _ in ranked if k[0] == 'R'), None)
    if main:
        breaks = collections.Counter()
        for (f, key, reason, _), nxt in zip(renders, renders[1:]):
            if f in fset and key == main[2] and nxt[1] == key:
                breaks[reason] += 1
        by_group = collections.Counter()
        for reason, c in breaks.items():
            by_group[cause_group(reason)] += c
        out['main_pass'] = dict(key=main[2], ms_per_frame=round(groups[main]['span'] / nf / 1000, 3),
                                passes_per_frame=round(groups[main]['n'] / nf, 2),
                                breaks_per_frame=round(sum(breaks.values()) / nf, 3),
                                breaks_by_group={g: round(c / nf, 3) for g, c in by_group.most_common()},
                                breaks_by_reason={e: round(c / nf, 3) for e, c in breaks.most_common()})
    return out


def print_report(out):
    print(f"{Path(out['session']).name} frames {out['frames']} fps {out['fps']} window {out['window_s']}")
    print(f"GPU ms/frame mean {out['gpu_ms']['mean']} p50 {out['gpu_ms']['p50']} p95 {out['gpu_ms']['p95']} "
          f"p99 {out['gpu_ms']['p99']} | wall mean {out['wall_ms']['mean']} p95 {out['wall_ms']['p95']} p99 {out['wall_ms']['p99']}")
    if out['present_ms']['frames_with_present']:
        pm = out['present_ms']
        print(f"presenter GPU ms/frame {pm['mean']} (per command buffer p50 {pm['cb_us_p50']} us, p90 {pm['cb_us_p90']} us, "
              f"{pm['cbs_per_frame']}/frame) | GPU incl. presenter mean "
              f"{out['gpu_ms_incl_present']['mean']} p95 {out['gpu_ms_incl_present']['p95']}")
    print(f"GPU occupancy {out['gpu_occupancy']} | GPU Commands blocked % of wall {out['gpu_commands_blocked_pct']}")
    for e, v in out['counters'].items():
        print(f"  {e}: {v}/frame")
    if 'passes' not in out:
        return
    print(f"encoders/frame {out['encoders_per_frame']} sum span {out['encoder_span_ms_per_frame']} ms")
    m = out.get('main_pass')
    if m:
        print(f"main pass {m['key']}: {m['ms_per_frame']} ms/frame, {m['passes_per_frame']} passes/frame, "
              f"breaks {m['breaks_per_frame']}/frame")
        print('  by group: ' + ', '.join(f'{g} {c}' for g, c in m['breaks_by_group'].items()))
        print('  by reason: ' + ', '.join(f'{g} {c}' for g, c in m['breaks_by_reason'].items()))
    print(f"{'ms/frame':>8} {'n/fr':>6} {'us/enc':>7} {'vs':>6} {'fs':>6} {'dr/enc':>6}  kind site key [end reasons/frame]")
    for p in out['passes']:
        ends = ','.join(f'{e}:{c}' for e, c in list(p['ends'].items())[:4] if e)
        print(f"{p['ms_per_frame']:8.3f} {p['n_per_frame']:6.2f} {p['us_per_enc']:7.1f} {p['vs_ms']:6.3f} {p['fs_ms']:6.3f} "
              f"{p['draws_per_enc']:6.1f}  {p['kind']} {p['site']} {p['key']} [{ends}]")
