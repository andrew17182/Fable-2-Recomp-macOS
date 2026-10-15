"""Frame-matched parity: compare guest captures of two arms at the same guest frames (docs/readiness.md,
"Frame-matched parity"), against the A-A noise floor of each arm.

    python3 tools/f2diag.py parity <armA>[,<armA2>...] <armB>[,<armB2>...] [--json out.json] [--heat DIR]

An arm is an f2diag run label (out/validation/f2diag/<label>), a run folder with a result.json, or a session folder.
Every guest capture of the session with a frame_id counts (the frame-matched captures of `run still` / `run sweep
--at-frame`, and any other guest capture), matched by guest frame id. Pairs:
  cross = A_i x B_j, floor = A_i x A_j and B_i x B_j (needs two runs of an arm; one run per arm gives no floor).
Per matched frame: mean absolute difference (0-255 per channel), the fraction of pixels whose largest channel
difference exceeds 8 / 32, and the mean absolute difference of every 32x32 block.
Verdict:
  identical     every cross pair is bit-identical at every matched frame;
  within-noise  the cross median is <= 2 x the largest floor-pair median + 0.02 (the noise limit) and no block of a cross pair exceeds the same
                block's floor max by more than --block-tol (4.0) - so animated regions (torch flicker, foliage)
                that also differ A-A do not count, while a change in a still region does;
  beyond-noise  otherwise: the hot blocks are listed (x, y, cross, floor) and --heat writes heat maps;
  no-floor      differences, but no A-A pair to judge them against (run each arm twice);
  no-match      no guest frame in common.
Shift: at the same guest frame id two runs can be a few game ticks apart, because the game advances by wall time
and the hitches after a load differ (Old Tomb A-A: 0.14 at the same id, 0.03 at the best shift; Bower Lake 0.37
within +-2, 0.09 at the best of +-6), so each burst of a pair is compared at the frame shift within +-4
(--max-shift; the 9-frame bursts of `run still` give the room) that matches best at the burst's middle frame; arms with frame_match targets are
first aligned on their targets. A rendering change raises every shift; timing noise only some.
Pure Python (no numpy/PIL): rows and 32-pixel block slices that are equal byte-for-byte are skipped, so
deterministic frames cost little; a 1280x720 pair that differs everywhere takes about a second.
"""
import itertools
import json
import os
import statistics
import sys
from operator import sub
from pathlib import Path

from .images import read_ppm

BLOCK = 32


def quick_diff(a, b, row_step=8):
    """Mean absolute difference over every row_step-th row (the shift search; about 1/8 of a full diff)."""
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    if (wa, ha) != (wb, hb):
        return 1e9
    rb = wa * 3
    tot = n = 0
    for y in range(0, ha, row_step):
        o = y * rb
        ra, rb_ = pa[o:o + rb], pb[o:o + rb]
        n += rb
        if ra != rb_:
            tot += sum(map(abs, map(sub, ra, rb_)))
    return tot / n if n else 0.0


def diff(a, b, heat=None):
    """Difference of two PPM captures (see the module doc). Returns a dict; blocks = per-block mean abs list."""
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    if (wa, ha) != (wb, hb):
        return dict(error='size %dx%d vs %dx%d' % (wa, ha, wb, hb))
    bw, bh = (wa + BLOCK - 1) // BLOCK, (ha + BLOCK - 1) // BLOCK
    if pa == pb:
        return dict(identical=True, mean_abs=0.0, frac_gt8=0.0, frac_gt32=0.0, block_max=0.0, block_xy=[0, 0],
                    w=wa, h=ha, bw=bw, bh=bh, blocks=[0.0] * (bw * bh))
    row_bytes = wa * 3
    span = BLOCK * 3
    blk = [0] * (bw * bh)
    tot = gt8 = gt32 = 0
    for y in range(ha):
        o = y * row_bytes
        ra, rb = pa[o:o + row_bytes], pb[o:o + row_bytes]
        if ra == rb:
            continue
        by = (y // BLOCK) * bw
        for bx in range(bw):
            sa, sb = ra[bx * span:(bx + 1) * span], rb[bx * span:(bx + 1) * span]
            if sa == sb:
                continue
            d = list(map(abs, map(sub, sa, sb)))
            s = sum(d)
            tot += s
            blk[by + bx] += s
            for i in range(0, len(d), 3):
                m = max(d[i], d[i + 1], d[i + 2])
                if m > 8:
                    gt8 += 1
                    if m > 32:
                        gt32 += 1
    vals = []
    for byi in range(bh):
        rows = min(BLOCK, ha - byi * BLOCK)
        for bx in range(bw):
            cols = min(BLOCK, wa - bx * BLOCK)
            vals.append(blk[byi * bw + bx] / (3.0 * rows * cols))
    k = max(range(len(vals)), key=vals.__getitem__)
    if heat:
        write_heat(heat, vals, bw, bh)
    n = wa * ha
    return dict(identical=False, mean_abs=round(tot / (3.0 * n), 4), frac_gt8=round(gt8 / n, 5),
                frac_gt32=round(gt32 / n, 5), block_max=round(vals[k], 2), block_xy=[(k % bw) * BLOCK, (k // bw) * BLOCK],
                w=wa, h=ha, bw=bw, bh=bh, blocks=vals)


def write_heat(path, vals, bw, bh, scale=8):
    """Per-block heat map (one cell per 32x32 block, x scale, brightness = 8 x mean abs diff), PPM."""
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (bw * scale, bh * scale))
        for byi in range(bh):
            line = b''.join(bytes([min(255, int(vals[byi * bw + bx] * 8))] * 3) * scale for bx in range(bw))
            f.write(line * scale)


# ---------------------------------------------------------------- arms

def _session_of(arm):
    p = Path(arm)
    if not p.exists():
        root = Path(__file__).resolve().parents[2]
        for cand in (root / 'out/validation/f2diag' / arm, root / 'out/dev-sessions' / arm):
            if cand.exists():
                p = cand
                break
    if (p / 'result.json').exists():
        return Path(json.loads((p / 'result.json').read_text())['session']), p
    if (p / 'test-state/control').exists():
        return p, None
    raise SystemExit('parity: %s is not an f2diag run (result.json) or a session folder' % arm)


def captures(arm):
    """{frame_id: ppm path} of an arm's guest captures (frame-matched ones win over plain captures of the same
    frame), plus the arm's run record (frame_match anchors)."""
    session, run_dir = _session_of(arm)
    out, exact = {}, set()
    for j in sorted((session / 'test-state/control').glob('*.capture.json')):
        try:
            m = json.loads(j.read_text())
        except (OSError, ValueError):
            continue
        f = m.get('frame_id')
        if m.get('source') != 'guest' or not f:
            continue
        ppm = j.with_name(j.name[:-len('.capture.json')] + '.ppm')
        if not ppm.exists():
            continue
        is_exact = m.get('requested_frame') == f
        if f not in out or (is_exact and f not in exact):
            out[f] = str(ppm)
            if is_exact:
                exact.add(f)
    rec = {}
    if run_dir is not None:
        try:
            r = json.loads((run_dir / 'result.json').read_text())
            fm = r.get('frame_match') or {}
            rec = dict(label=run_dir.name, spec=fm.get('spec'), target=fm.get('target'),
                       load_end_frame=fm.get('load_end_frame'),
                       loading_clear_frame=fm.get('loading_clear_frame'), error=fm.get('error'),
                       agent_cache=(r.get('agent_cache') or {}).get('signature'), verdict=r.get('verdict'))
        except (OSError, ValueError, KeyError):
            pass
    rec.update(session=str(session), frames=len(out), exact_frames=len(exact))
    return out, rec


def _bursts(frames):
    """Runs of consecutive frame ids (the capture bursts of `run still` / `run sweep --at-frame`)."""
    out = []
    for f in sorted(frames):
        if out and f == out[-1][-1] + 1:
            out[-1].append(f)
        else:
            out.append([f])
    return out


def _pair(ca, cb, max_frames=None, max_shift=4, center=0):
    """Diffs of A's frames against B's frames at the best shift, chosen per burst: two runs can be a game tick or
    two apart at the same guest frame id, and a sweep's stick input can start a frame later in one run, so each
    burst is matched at the shift d in center +- max_shift (B frame = A frame + d) whose diff at the burst's middle
    frame is smallest. Returns (rows [(A frame, diff)], shifts [per burst], scores [per burst])."""
    bursts = _bursts(ca)
    if max_frames and len(bursts) > max_frames:  # spread over the matched range
        idx = sorted({round(i * (len(bursts) - 1) / (max_frames - 1)) for i in range(max_frames)}) if max_frames > 1 else [0]
        bursts = [bursts[i] for i in idx]
    rows, shifts, scores = [], [], []
    for burst in bursts:
        mid = burst[len(burst) // 2]
        score = {}
        for d in range(center - max_shift, center + max_shift + 1):
            if (mid + d) in cb:
                score[d] = quick_diff(ca[mid], cb[mid + d])
        if not score:
            continue
        best = min(score, key=lambda d: (score[d], abs(d - center)))
        shifts.append(best)
        scores.append({k: round(v, 4) for k, v in score.items()})
        for f in burst:
            if (f + best) in cb:
                rows.append((f, diff(ca[f], cb[f + best])))
    return rows, shifts, scores


def compare(arms_a, arms_b, max_frames=8, block_tol=4.0, heat_dir=None, max_shift=4):
    caps, recs = {}, {}
    for arm in arms_a + arms_b:
        caps[arm], recs[arm] = captures(arm)
    pairs = []
    for kind, combos in (('cross', list(itertools.product(arms_a, arms_b))),
                         ('floor', list(itertools.combinations(arms_a, 2)) + list(itertools.combinations(arms_b, 2)))):
        for x, y in combos:
            # Arms captured at an anchored spec (load+N) are aligned on their targets first: the level load can
            # end a frame or two later in one run (Old Tomb: load_end_frame 100-102).
            tx, ty = recs[x].get('target'), recs[y].get('target')
            center = ty - tx if tx is not None and ty is not None else 0
            rows, shifts, scores = _pair(caps[x], caps[y], max_frames, max_shift, center)
            pairs.append(dict(kind=kind, a=x, b=y, rows=rows, shifts=[d - center for d in shifts], target_shift=center,
                              shift_scores=scores))
    # Block floor: the largest per-block difference any floor pair shows at any frame.
    floor_blocks = None
    for p in pairs:
        if p['kind'] != 'floor':
            continue
        for _, d in p['rows']:
            if 'blocks' not in d:
                continue
            floor_blocks = list(d['blocks']) if floor_blocks is None else list(map(max, floor_blocks, d['blocks']))
    summary = []
    hot = {}
    for p in pairs:
        ms = [d['mean_abs'] for _, d in p['rows'] if 'mean_abs' in d]
        row = dict(kind=p['kind'], a=p['a'], b=p['b'], shifts=p['shifts'], target_shift=p['target_shift'],
                   shift_scores=p['shift_scores'], edge_shifts=sum(abs(d) == max_shift for d in p['shifts']),
                   frames=[f for f, _ in p['rows']], matched=len(ms),
                   identical=sum(1 for _, d in p['rows'] if d.get('identical')),
                   median=round(statistics.median(ms), 4) if ms else None, max=round(max(ms), 4) if ms else None,
                   block_max=max((d.get('block_max', 0) for _, d in p['rows']), default=None),
                   frac_gt8_max=max((d.get('frac_gt8', 0) for _, d in p['rows']), default=None),
                   errors=[d['error'] for _, d in p['rows'] if 'error' in d])
        summary.append(row)
        if p['kind'] == 'cross':
            for f, d in p['rows']:
                if 'blocks' not in d:
                    continue
                for i, v in enumerate(d['blocks']):
                    fl = floor_blocks[i] if floor_blocks else 0.0
                    if v > fl + block_tol and v - fl > hot.get(i, (0, 0, 0))[0]:
                        hot[i] = (round(v - fl, 2), round(v, 2), round(fl, 2), f, p['a'], p['b'])
                if heat_dir and not d.get('identical'):
                    Path(heat_dir).mkdir(parents=True, exist_ok=True)
                    write_heat(Path(heat_dir) / ('%s__%s__f%d.ppm' % (Path(p['a']).name, Path(p['b']).name, f)),
                               d['blocks'], d['bw'], d['bh'])
    cross = [r for r in summary if r['kind'] == 'cross' and r['matched']]
    floor = [r for r in summary if r['kind'] == 'floor' and r['matched']]
    bw = next((d['bw'] for p in pairs for _, d in p['rows'] if 'bw' in d), 1)
    hot_rows = [dict(x=(i % bw) * BLOCK, y=(i // bw) * BLOCK, excess=v[0], cross=v[1], floor=v[2], frame=v[3],
                     a=v[4], b=v[5]) for i, v in sorted(hot.items(), key=lambda kv: -kv[1][0])]
    cross_med = round(statistics.median([r['median'] for r in cross]), 4) if cross else None
    cross_max = max((r['max'] for r in cross), default=None)
    floor_med = round(statistics.median([r['median'] for r in floor]), 4) if floor else None
    floor_max = max((r['max'] for r in floor), default=None)
    # The cross median may be at most twice the largest floor-pair median (+ 0.02): pair medians, not single
    # frames, because one frame at a burst's edge can sit a tick off (Old Tomb floor: medians 0.04-0.06, one
    # frame 0.36).
    noise_limit = round(2 * max(r['median'] for r in floor) + 0.02, 4) if floor else None
    if not cross:
        verdict = 'no-match'
    elif all(r['identical'] == r['matched'] for r in cross):
        verdict = 'identical'
    elif not floor:
        verdict = 'no-floor'
    elif cross_med <= noise_limit and not hot_rows:
        verdict = 'within-noise'
    else:
        verdict = 'beyond-noise'
    return dict(verdict=verdict, cross_median=cross_med, cross_max=cross_max, floor_median=floor_med, noise_limit=noise_limit,
                floor_max=floor_max, floor_identical=bool(floor) and all(r['identical'] == r['matched'] for r in floor),
                hot_blocks=hot_rows[:20], hot_block_count=len(hot_rows), block_tol=block_tol, max_shift=max_shift,
                pairs=summary,
                arms={k: v for k, v in recs.items()})


def print_report(res):
    for arm, r in res['arms'].items():
        print('arm %-28s %3d guest frames (%d frame-matched)  anchors load=%s clear=%s  %s'
              % (Path(arm).name, r['frames'], r['exact_frames'], r.get('load_end_frame'), r.get('loading_clear_frame'),
                 ('frame-match error: %s' % r['error']) if r.get('error') else ''))
    for r in res['pairs']:
        print('%-5s %-22s x %-22s shifts %s %3d frames %s median %s  max %s  block max %s  >8 %s%s'
              % (r['kind'], Path(r['a']).name, Path(r['b']).name, ','.join('%+d' % d for d in r['shifts']) or '-',
                 r['matched'],
                 ('(%d identical)' % r['identical']).ljust(15), r['median'], r['max'], r['block_max'], r['frac_gt8_max'],
                 ('  errors %s' % r['errors']) if r['errors'] else ''))
    specs = {r.get('spec') for r in res['arms'].values() if r.get('spec')}
    if len(specs) > 1:
        print('WARNING: the arms captured at different frame specs (%s): they show different game moments; '
              'rerun with the same --at-frame' % ', '.join(sorted(specs)))
    edge = sum(r['edge_shifts'] for r in res['pairs'])
    if edge:
        print('note: %d burst(s) matched at the edge of the shift search (+-%d): the runs may be further apart in '
              'game time; a wider burst (FABLE2_PARITY_BURST) and --max-shift would tell' % (edge, res['max_shift']))
    print('cross median %s max %s | floor median %s max %s | noise limit %s%s'
          % (res['cross_median'], res['cross_max'], res['floor_median'], res['floor_max'], res['noise_limit'],
             ' (floor bit-identical)' if res['floor_identical'] else ''))
    for h in res['hot_blocks'][:10]:
        print('  hot block x=%d y=%d: cross %.2f vs floor %.2f (frame %d, %s x %s)'
              % (h['x'], h['y'], h['cross'], h['floor'], h['frame'], Path(h['a']).name, Path(h['b']).name))
    if res['hot_block_count'] > 10:
        print('  ... %d hot blocks in all' % res['hot_block_count'])
    print('PARITY: %s' % res['verdict'])


def main(args):
    arms_a = [x for x in args.arm_a.split(',') if x]
    arms_b = [x for x in args.arm_b.split(',') if x]
    res = compare(arms_a, arms_b, args.max_bursts, args.block_tol, args.heat, args.max_shift)
    if args.json:
        Path(args.json).write_text(json.dumps(res, indent=1))
    print_report(res)
    return 0 if res['verdict'] in ('identical', 'within-noise') else 1
