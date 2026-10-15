"""HLE census reader (FABLE2_HLE_CENSUS=1, docs/hle-census-20261013.md).

Reads the "[hle-census]" per-frame lines and, when the run also had FABLE2_SHADOW_TRACE=<frame>, the
"[shadow-trace]" draw / resolve / xfer lines of the traced frames, and prints:
- draws per frame: packets, predicate-skipped, issued by EDRAM mode, unmatched, distinct headers, replays;
- per call site (builder function, caller lr): issued draws and distinct headers per frame, share;
- pass map of one traced frame: consecutive draws with the same bound targets form a pass; resolves and
  ownership transfers between passes are attached to the pass they follow;
- EDRAM aliasing patterns: distinct ownership-transfer pairs and resolve shapes in the traced frame,
  grouped by kind.

CLI: python3 tools/f2diag.py census <session or log> [--min-packets 500] [--frame N] [--json out.json]
Output holds render-target keys, shader hashes and guest addresses only (no game strings).
"""
import collections
import csv
import json
import re
from pathlib import Path

FRAME = re.compile(r'\[hle-census\] f(\d+) pk=(\d+) skip=(\d+) col=(\d+) dep=(\d+) copy=(\d+) oth=(\d+) '
                   r'unm=(\d+) uniq=(\d+) maxx=(\d+) rep=(\d+) ib=(\d+) ibu=(\d+) unscanned=(\d+) sites(.*)$')
SITE = re.compile(r'\[hle-census\] (?:guest )?site (\d+) builder ([0-9A-F]{8}) lr ([0-9A-F]{8})')
TR = re.compile(r'\[shadow-trace\] f(\d+) (draw|resolve|xfer|hostdepth-drop) (.*)$')
KEY = re.compile(r'\[RT @ (\d+)t, <(\d+)t>, (\d)xMSAA, (k\w+)(, native)?\]')

# Draw-packet builder roles, inferred from their arguments and packets (no symbols exist).
BUILDERS = {
    '8221E0F0': 'indexed draw (prim, base vertex, start index, count)',
    '82207C30': 'indexed draw, index buffer object variant (loops in 65534-index chunks)',
    '82217EE8': 'begin indexed UP draw (caller fills indices/vertices)',
    '822060A0': 'begin UP draw (caller fills vertices)',
    '8221C518': 'non-indexed draw (prim, start, count)',
    '8221C9C8': 'non-indexed draw, auto-index variant',
    '822069C0': 'D3D-internal rect/quad draw (DRAW_INDX_2, inline)',
    '821EF988': 'D3D-internal draw (DRAW_INDX_2, inline)',
    '82B98770': 'D3D-internal draw (DRAW_INDX_2)',
    '82B988C8': 'D3D-internal draw (DRAW_INDX_2)',
    '82BA5448': 'D3D-internal draw (DRAW_INDX_2)',
    '82BA5CE8': 'D3D-internal draw (DRAW_INDX_2)',
    '82BA29B8': 'D3D-internal draw (DRAW_INDX_2)',
    '82B9C068': 'engine init (VdInitializeEngines path)',
    '82B928A8': 'engine init (VdInitializeEngines path)',
}


def _log(target):
    p = Path(target)
    if p.is_file():
        return p
    for c in (p / 'census.log', p / 'test-state/logs/fable_2.log', p / 'logs/fable_2.log', p / 'fable_2.log'):
        if c.exists():
            return c
    raise FileNotFoundError(f'no census.log / fable_2.log under {p}')


def key_parts(k):
    m = KEY.search(k)
    if not m:
        return None
    return dict(base=int(m.group(1)), pitch=int(m.group(2)), msaa=int(m.group(3)), fmt=m.group(4),
                depth=m.group(4).startswith('kD'), native=bool(m.group(5)))


def xfer_kind(src, dst):
    a, b = key_parts(src), key_parts(dst)
    if not a or not b:
        return 'other'
    if a['native'] != b['native'] and (a['depth'], a['msaa'], a['fmt'], a['pitch']) == (b['depth'], b['msaa'], b['fmt'], b['pitch']):
        return 'scale change (native<->scaled)'
    if a['depth'] != b['depth']:
        return 'colour<->depth'
    if a['msaa'] != b['msaa']:
        return 'msaa change'
    if a['fmt'] != b['fmt']:
        return 'format change'
    if a['pitch'] != b['pitch']:
        return 'pitch change'
    return 'same format (base/rows)'


def parse(target):
    sites, frames, trace = {}, [], collections.defaultdict(list)
    for line in open(_log(target), errors='replace'):
        m = FRAME.search(line)
        if m:
            g = [int(x) for x in m.groups()[:14]]
            per = {}
            for tok in m.group(15).split():
                sid, _, rest = tok.partition(':')
                n, _, u = rest.partition('/')
                per[int(sid)] = (int(n), int(u or 0))
            frames.append(dict(zip(('f', 'pk', 'skip', 'col', 'dep', 'copy', 'oth', 'unm', 'uniq', 'maxx', 'rep',
                                    'ib', 'ibu', 'unscanned'), g), sites=per))
            continue
        m = SITE.search(line)
        if m:
            sites[int(m.group(1))] = (m.group(2), m.group(3))
            continue
        m = TR.search(line)
        if m:
            trace[int(m.group(1))].append((m.group(2), m.group(3)))
    return sites, frames, trace


def pass_map(events):
    """Group a traced frame's events into passes: consecutive draws with the same bound targets.
    Transfers are built for the draw that follows them; resolves follow the pass they read."""
    passes, cur, pend_x = [], None, []
    for kind, rest in events:
        if kind == 'draw':
            tgt = rest.split(' vs=')[0]
            ps = re.search(r'ps=([0-9A-F]{16})', rest)
            site = re.search(r'site=(-?\d+)', rest)
            vp = re.search(r'vp=(\S+)', rest)
            if cur is None or cur['targets'] != tgt:
                cur = dict(targets=tgt, draws=0, ps=collections.Counter(), sites=collections.Counter(),
                           vp=collections.Counter(), xfers_before=pend_x, xfers_mid=[], resolves=[])
                passes.append(cur)
            else:
                cur['xfers_mid'] += pend_x
            pend_x = []
            cur['draws'] += 1
            cur['ps'][ps.group(1) if ps else '?'] += 1
            cur['sites'][int(site.group(1)) if site else -9] += 1
            cur['vp'][vp.group(1) if vp else '?'] += 1
        elif kind == 'resolve':
            if cur is not None:
                cur['resolves'].append(rest)
                cur = None  # a resolve ends the pass (the next draw starts a new one)
        elif kind == 'xfer':
            sd = re.findall(r'\[[^\]]*\]', rest)
            if len(sd) >= 2:
                pend_x.append((sd[0], sd[1]))
    return passes


def blocks(passes):
    """Find consecutive repeats of the same pass sequence (targets + viewport); mark the repeats
    after the first copy as collapsed so the printed map stays readable."""
    sig = [(p['targets'], p['vp']) for p in passes]
    out, i = [], 0
    while i < len(sig):
        best = (1, 1)  # (len, reps)
        for k in range(1, 9):
            reps = 1
            while sig[i + reps * k:i + (reps + 1) * k] == sig[i:i + k] and i + (reps + 1) * k <= len(sig):
                reps += 1
            if reps > 1 and reps * k > best[0] * best[1]:
                best = (k, reps)
        k, reps = best
        span = passes[i:i + k * reps]
        out.append(dict(start=i, len=k, reps=reps, draws=sum(p['draws'] for p in span),
                        resolves=sum(p['resolves_after'] for p in span),
                        xfers=sum(p['xfers_before'] + p['xfers_mid'] for p in span)))
        for p in passes[i + k:i + k * reps]:
            p['collapsed'] = True
        i += k * reps
    return out


def resolve_shape(rest):
    m = re.search(r'depth=(\d) copy=(\d) clear_d=(\d) clear_c=(\d) (\d+)x(\d+) .*dest_fmt=(\d+).*owners:(.*)$', rest)
    if not m:
        return 'other'
    owner = key_parts(m.group(8)) or {}
    what = 'depth' if m.group(1) == '1' else 'colour'
    clears = ('+clear' if (m.group(3) == '1' or m.group(4) == '1') else '')
    return (f"{what} {m.group(5)}x{m.group(6)} fmt{m.group(7)}{' copy' if m.group(2) == '1' else ' clear-only'}{clears} "
            f"from {owner.get('msaa', '?')}x {owner.get('fmt', '?')}")


def analyze(target, min_packets=500, frame=None):
    sites, frames, trace = parse(target)
    play = [f for f in frames if f['pk'] >= min_packets]
    out = dict(log=str(_log(target)), frames=len(play))
    if play:
        n = len(play)
        out['per_frame'] = {k: round(sum(f[k] for f in play) / n, 1)
                            for k in ('pk', 'skip', 'col', 'dep', 'copy', 'oth', 'unm', 'uniq', 'maxx', 'rep',
                                      'ib', 'ibu')}
        issued = sum(f['col'] + f['dep'] + f['copy'] + f['oth'] for f in play) / n
        out['per_frame']['issued'] = round(issued, 1)
        tot = collections.defaultdict(lambda: [0, 0])
        for f in play:
            for sid, (c, u) in f['sites'].items():
                tot[sid][0] += c
                tot[sid][1] += u
        rows = []
        for sid, (c, u) in sorted(tot.items(), key=lambda kv: -kv[1][0]):
            b, lr = sites.get(sid, ('-', '-'))
            rows.append(dict(site=sid, builder=b, lr=lr, role=BUILDERS.get(b, 'unmatched' if sid == -1 else '?'),
                             issued=round(c / n, 1), distinct=round(u / n, 1),
                             share_pct=round(100.0 * c / n / issued, 2) if issued else 0))
        out['sites'] = rows
        byb = collections.defaultdict(float)
        for r in rows:
            byb[r['builder']] += r['issued']
        out['builders'] = {b: dict(issued=round(v, 1), share_pct=round(100.0 * v / issued, 2), role=BUILDERS.get(b, ''))
                           for b, v in sorted(byb.items(), key=lambda kv: -kv[1])}
    if trace:
        f = frame if frame in trace else min(trace)
        ev = trace[f]
        out['trace_frame'] = f
        ps = pass_map(ev)
        out['passes'] = [dict(i=i, targets=p['targets'], draws=p['draws'], distinct_ps=len(p['ps']),
                              vp=p['vp'].most_common(1)[0][0], sites=dict(p['sites'].most_common(4)),
                              xfers_before=len(p['xfers_before']), xfers_mid=len(p['xfers_mid']),
                              resolves_after=len(p['resolves']),
                              resolve_shapes=sorted(set(resolve_shape(r) for r in p['resolves'])))
                         for i, p in enumerate(ps)]
        out['pass_blocks'] = blocks(out['passes'])
        pairs = collections.Counter()
        kinds = collections.Counter()
        for kind, rest in ev:
            if kind == 'xfer':
                sd = re.findall(r'\[[^\]]*\]', rest)
                if len(sd) >= 2:
                    pairs[(sd[0], sd[1])] += 1
                    kinds[xfer_kind(sd[0], sd[1])] += 1
        res = collections.Counter(resolve_shape(rest) for kind, rest in ev if kind == 'resolve')
        out['aliasing'] = dict(
            xfers=sum(pairs.values()), distinct_pairs=len(pairs),
            by_kind=dict(kinds.most_common()),
            distinct_pairs_by_kind=dict(collections.Counter(xfer_kind(s, d) for s, d in pairs).most_common()),
            top_pairs=[dict(n=c, src=s, dst=d, kind=xfer_kind(s, d)) for (s, d), c in pairs.most_common(30)],
            resolves=sum(res.values()), distinct_resolve_shapes=len(res),
            resolve_shapes=[dict(n=c, shape=s) for s, c in res.most_common(40)],
            draws=sum(1 for k, _ in ev if k == 'draw'),
            distinct_targets=len(set(p['targets'] for p in ps)))
    return out


def print_report(out):
    print(f"{out['log']}: {out['frames']} gameplay frames")
    if 'per_frame' in out:
        pf = out['per_frame']
        print('per frame: ' + ' '.join(f'{k}={v}' for k, v in pf.items()))
        print(f"\n{'issued':>8} {'distinct':>8} {'share%':>7}  site builder  lr        role")
        for r in out['sites']:
            print(f"{r['issued']:8.1f} {r['distinct']:8.1f} {r['share_pct']:7.2f}  {r['site']:>4} {r['builder']} "
                  f"{r['lr']}  {r['role']}")
        print('\nby builder:')
        for b, v in out['builders'].items():
            print(f"  {b}  {v['issued']:8.1f}  {v['share_pct']:6.2f}%  {v['role']}")
    if 'passes' in out:
        print(f"\npass map, frame {out['trace_frame']} ({len(out['passes'])} passes; repeated blocks collapsed)")
        for blk in out['pass_blocks']:
            if blk['reps'] > 1:
                print(f"  [x{blk['reps']}] block of {blk['len']} passes (#{blk['start']}..#{blk['start'] + blk['reps'] * blk['len'] - 1}), "
                      f"{blk['draws']} draws, {blk['resolves']} resolves, {blk['xfers']} transfers in total:")
        for p in out['passes']:
            if p.get('collapsed'):
                continue
            print(f"  #{p['i']:<3} {p['draws']:4d} draws {p['distinct_ps']:3d} PS vp {p['vp']:>10} xin {p['xfers_before']:2d} "
                  f"res {p['resolves_after']:2d}  {p['targets']}  sites {p['sites']}")
            for s in p['resolve_shapes']:
                print(f"         -> {s}")
        a = out['aliasing']
        print(f"\naliasing: {a['xfers']} transfers, {a['distinct_pairs']} distinct pairs {a['distinct_pairs_by_kind']}; "
              f"by kind {a['by_kind']}; {a['resolves']} resolves, {a['distinct_resolve_shapes']} distinct shapes; "
              f"{a['draws']} draws, {a['distinct_targets']} distinct target sets")
        for t in a['top_pairs']:
            print(f"  {t['n']:4d}  {t['kind']:22} {t['src']} -> {t['dst']}")
        for t in a['resolve_shapes']:
            print(f"  {t['n']:4d}  {t['shape']}")


def main(args):
    out = analyze(args.target, args.min_packets, args.frame)
    print_report(out)
    if args.json:
        Path(args.json).write_text(json.dumps(out, indent=1))


# --- Pass costs by role (pass trace of a FABLE2_GPU_TIME_TRACE=1 run) ---------------------------
# Host encoder keys are "c0:<mtl format>/WxHxS/<load-store> d:<fmt>/WxHxS/..". W is the EDRAM pitch in
# host pixels (tiles x 80 at 1x/2x MSAA, x 40 at 4x), so a key maps back to the guest target. Roles
# come from the HLE census pass map (docs/hle-census-20261013.md section 4); keys are divided by the
# run's resolution scale first. Bucket = cost class for the M3 Pro breakdown.
HKEY = re.compile(r'c0:(\d+)/(\d+)x(\d+)x(\d)')
DKEY = re.compile(r'd:(\d+)/(\d+)x(\d+)x(\d)')

ROLE_BUCKET = {
    'main colour pass (2xMSAA HDR, per EDRAM tile)': 'main passes',
    'depth prepass (2xMSAA)': 'main passes',
    'shadow maps': 'main passes',
    'small scene render (256 @ 4xMSAA)': 'main passes',
    'texture-space passes (256/128/64/32 chains)': 'main passes',
    'memexport passes (no render target)': 'main passes',
    'screen-space shadow mask': 'main passes',
    'occlusion-query passes (depth only)': 'main passes',
    'main depth at 4xMSAA alias (clear + no-op points)': 'EDRAM emulation',
    'clear quads at 4xMSAA alias': 'EDRAM emulation',
    'half-res post (560x360)': 'post chain',
    'quarter-res post (280x180)': 'post chain',
    'final composite + UI (1280x720)': 'post chain',
    'other small passes': 'fixed overhead',
    'resolves (EDRAM -> memory)': 'EDRAM emulation',
    'resolved-texture reloads (untile/load)': 'EDRAM emulation',
    'EDRAM ownership transfers / clears': 'EDRAM emulation',
    'EDRAM depth store': 'EDRAM emulation',
    'shared-memory uploads': 'fixed overhead',
    'other non-draw': 'fixed overhead',
}


def scale_of(rows):
    w = 0
    for r in rows:
        if r['kind'] == 'R' and r['site'] == 'draw' and r['key'].startswith('c0:115/'):
            m = HKEY.match(r['key'])
            if m and m.group(4) == '2':
                w = max(w, int(m.group(2)))
    return max(1, w // 1120)


def _tiles(w, msaa, s):
    """EDRAM pitch in tiles of a host key width (80 px per tile at 1x/2x MSAA, 40 at 4x), trying the
    run's scale first and then 1x (native-scale targets)."""
    tw = 40 if msaa == 4 else 80
    for sc in (s, 1):
        if w % (tw * sc) == 0 and w // (tw * sc) in (1, 2, 4, 7, 8, 13, 14, 16):
            return w // (tw * sc), sc
    return None, s


def role_of(site, key, draws, s):
    """Pass role from a host encoder key. Mapping checked against an aligned frame
    (census.align: host encoder draws vs FABLE2_SHADOW_TRACE guest draws, 0 mixed encoders)."""
    if site in ('direct_host_resolve', 'rt_resolve'):
        return 'resolves (EDRAM -> memory)'
    if site.startswith('tex_'):
        return 'resolved-texture reloads (untile/load)'
    if site.startswith('rt_transfer'):
        return 'EDRAM ownership transfers / clears'
    if site == 'rt_depth_store':
        return 'EDRAM depth store'
    if site.startswith('shmem'):
        return 'shared-memory uploads'
    if site != 'draw':
        return 'other non-draw'
    m = HKEY.match(key)
    if not m:
        return 'other small passes'
    fmt, w, msaa = int(m.group(1)), int(m.group(2)), int(m.group(4))
    has_d = ' d:' in key
    dummy_c0 = key.split(' ')[0].endswith('/DD')  # colour attachment that is never stored
    t, _ = _tiles(w, msaa, s)
    clear = draws <= 1.5
    if not has_d and dummy_c0:
        return 'memexport passes (no render target)'
    if t == 14 and msaa == 2:
        return 'main colour pass (2xMSAA HDR, per EDRAM tile)' if fmt == 115 else 'depth prepass (2xMSAA)'
    if t == 14 and msaa == 4:
        return 'main depth at 4xMSAA alias (clear + no-op points)'
    if t == 14 and msaa == 1:
        return 'screen-space shadow mask'
    if t == 13:
        return 'clear quads at 4xMSAA alias' if (msaa == 4 or clear) else 'shadow maps'
    if t == 16:
        return 'final composite + UI (1280x720)'
    if msaa == 4 and clear:
        return 'clear quads at 4xMSAA alias'
    if t == 7 and msaa == 4:
        return 'small scene render (256 @ 4xMSAA)'
    if t in (7, 8):
        return 'half-res post (560x360)'
    if t == 4 and has_d and dummy_c0:
        return 'occlusion-query passes (depth only)'
    if t == 4 and fmt in (90, 115, 55):
        return 'quarter-res post (280x180)'
    if t in (1, 2, 4):
        return 'texture-space passes (256/128/64/32 chains)'
    return 'other small passes'


def costs(passes_csv, from_frame=600, to_frame=None):
    """Marginal busy ms/frame per role (how far each encoder pushes the end of the serial GPU
    timeline, as keytail.py) plus encoder span, encoders and draws per frame."""
    rows = list(csv.DictReader(open(passes_csv)))
    s = scale_of(rows)
    ev = []
    for r in rows:
        try:
            st, sp, fr = float(r['start_us']), float(r['span_us']), int(r['frame'])
        except ValueError:
            continue
        if st < 0 or sp < 0 or fr < from_frame or (to_frame is not None and fr > to_frame):
            continue
        ev.append((st, st + sp, fr, r['site'], r['key'], int(r['draws'] or 0)))
    ev.sort()
    # draws per encoder by key decides clear-quad vs real pass
    per_key = collections.defaultdict(lambda: [0, 0])
    for _, _, _, site, key, d in ev:
        per_key[(site, key)][0] += 1
        per_key[(site, key)][1] += d
    agg = collections.defaultdict(lambda: dict(marg=0.0, span=0.0, enc=0, draws=0))
    end, frames = 0.0, set()
    for st, en, fr, site, key, d in ev:
        n, dd = per_key[(site, key)]
        role = role_of(site, key, dd / max(1, n), s)
        a = agg[role]
        a['marg'] += max(0.0, en - max(st, end))
        end = max(end, en)
        a['span'] += en - st
        a['enc'] += 1
        a['draws'] += d
        frames.add(fr)
    nf = max(1, len(frames))
    out = dict(csv=str(passes_csv), scale=s, frames=nf, roles={})
    for role, a in sorted(agg.items(), key=lambda kv: -kv[1]['marg']):
        out['roles'][role] = dict(bucket=ROLE_BUCKET.get(role, '?'), busy_ms=round(a['marg'] / nf / 1e3, 3),
                                  span_ms=round(a['span'] / nf / 1e3, 3), encoders=round(a['enc'] / nf, 2),
                                  draws=round(a['draws'] / nf, 1))
    out['busy_ms'] = round(sum(v['busy_ms'] for v in out['roles'].values()), 3)
    buckets = collections.defaultdict(float)
    for v in out['roles'].values():
        buckets[v['bucket']] += v['busy_ms']
    out['buckets'] = {k: round(v, 3) for k, v in sorted(buckets.items(), key=lambda kv: -kv[1])}
    return out


def print_costs(out):
    print(f"{out['csv']}: scale {out['scale']}x, {out['frames']} frames, busy {out['busy_ms']} ms/frame")
    print(f"{'busy ms':>8} {'span ms':>8} {'enc/f':>6} {'draws/f':>8}  role [bucket]")
    for role, v in out['roles'].items():
        print(f"{v['busy_ms']:8.3f} {v['span_ms']:8.3f} {v['encoders']:6.1f} {v['draws']:8.1f}  {role} [{v['bucket']}]")
    print('buckets: ' + ', '.join(f'{k} {v}' for k, v in out['buckets'].items()))


def kit_report(folder, as_json=None):
    """A benchmark-kit report folder (docs/benchmark-kit.md): every census arm's pass trace
    (raw/census<N>x/passes-*.csv) over the arm's headings8 window, with the arm's GPU ms from the
    report. These are measured on the tester's Mac (no estimate)."""
    import glob
    folder = Path(folder)
    rep = json.loads((folder / 'report.json').read_text())
    machine = rep.get('machine', {})
    print(f"{machine.get('chip', '?')} ({machine.get('model_identifier', '?')}, {machine.get('gpu_cores', '?')} GPU cores), "
          f"app {rep.get('app', {}).get('Fable2VersionString', '?')}")
    arms = {a.get('arm'): a for a in rep.get('arms', [])}
    for a in rep.get('arms', []):
        sw = a.get('sweep') or {}
        gpu = sw.get('gpu_ms_mean')
        print(f"  {a.get('arm'):9} {a.get('title', '?'):34} fps {sw.get('fps_mean', '?')} GPU ms {gpu}")
    outs = {}
    for d in sorted(glob.glob(str(folder / 'raw/census*'))):
        arm = Path(d).name
        files = sorted(glob.glob(d + '/passes-*.csv'))
        if not files:
            print(f"{arm}: no passes CSV")
            continue
        f0, f1 = 600, None
        try:
            res = json.loads((Path(d) / 'result.json').read_text())
            f0, f1 = res['sweep']['frame_start'], res['sweep']['frame_end']
        except (OSError, ValueError, KeyError, TypeError):
            pass
        out = costs(files[-1], f0, f1)
        sw = (arms.get(arm) or {}).get('sweep') or {}
        gpu = sw.get('gpu_ms_mean')
        out['arm'] = arm
        out['report_gpu_ms_mean'] = gpu
        print(f"\n{arm} (frames {f0}-{f1}):")
        print_costs(out)
        if isinstance(gpu, (int, float)):
            idle = gpu - out['busy_ms']
            tot = gpu
            print(f"GPU ms/frame (report) {gpu}; encoder busy {out['busy_ms']}; idle/unattributed {idle:.2f}")
            print('measured split: ' + ', '.join(f"{k} {v:.2f} ms ({100 * v / tot:.0f}%)" for k, v in out['buckets'].items())
                  + f", idle/unattributed {idle:.2f} ms ({100 * idle / tot:.0f}%)")
        outs[arm] = out
    if as_json:
        Path(as_json).write_text(json.dumps(outs, indent=1))


def main_costs(args):
    import glob
    for t in args.targets:
        p = Path(t)
        if p.is_dir() and (p / 'report.json').exists():
            kit_report(p, args.json)
            continue
        files = sorted(glob.glob(str(p / 'passes-*.csv'))) if p.is_dir() else [t]
        if not files:
            files = sorted(glob.glob(str(p / 'test-state/logs/passes-*.csv')))
        for f in files:
            out = costs(f, args.from_frame)
            print_costs(out)
            if args.json:
                Path(args.json).write_text(json.dumps(out, indent=1))
