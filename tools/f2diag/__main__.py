"""f2diag CLI. Run as `python3 tools/f2diag.py <command> ...` (or `python3 -m f2diag` from tools/).

<target> is a session folder (out/dev-sessions/<id>, or just <id>), its test-state / logs / trace folder, or a dump
folder (trace/dumps/NNN-reason). Dump-based commands use the newest dump unless --dump picks one.
"""
import argparse
import json
import sys
from pathlib import Path

from . import f2t, images, perfetto, report, symbols, timeline
from .session import Session, contention, resolve


def _contention(target):
    """Print the CONTENTION flag (other game instances during this session), if any."""
    try:
        c = contention(target)
    except Exception:
        return
    if c['text']:
        print(c['text'])


def _trace(args):
    p = Path(args.target)
    if (p / 'ring.f2t').exists() or p.suffix == '.f2t':
        return f2t.load(p), p
    s = Session(p)
    d = 'stream' if getattr(args, 'stream', False) else (s.dump(args.dump) if args.dump else s.dump())
    return s.load(d) if d == 'stream' else f2t.load(d), d


def cmd_report(args):
    p = Path(args.target)
    dump = p if (p / 'summary.json').exists() else Session(p).dump(args.dump)
    _contention(dump)
    report.report(dump, args.window)


def cmd_dumps(args):
    s = Session(args.target)
    _contention(args.target)
    print(f"trace: {s.trace}")
    for d in s.dumps():
        try:
            m = json.loads((d / 'summary.json').read_text())
            print(f"  {d.name:24} t={m['t_s']:8.2f}s frame {m['frame']:<6} {m['detail'][:90]}")
        except (OSError, ValueError, KeyError):
            print(f"  {d.name}")
    meta = s.meta
    print(f"session: dumps {meta.get('dumps')}, AV lines {meta.get('av_lines')}, tags {json.dumps(meta.get('tags'))}")
    exited = [e for e in s.writeback() if e.get('event') == 'game-exited']
    if exited and exited[-1].get('how'):
        how = exited[-1]['how']
        label = {'closed-by-user': 'CLOSED BY USER (not a crash/hang)', 'test-control-close': 'closed by the test harness',
                 'unclean': 'UNCLEAN EXIT (crash or kill)'}.get(how, how)
        print(f"exit: {label} - {exited[-1].get('detail')}")
    never = meta.get('hooks', {}).get('never_fired')
    print(f"hooks never fired: {never if never else 'none'}")


def cmd_timeline(args):
    tr, src = _trace(args)
    print(f"# {src}")
    for t, frame, thr, what in timeline.timeline(tr, args.from_s, args.to_s, args.min_wait, opens=not args.no_opens,
                                                  logs=args.logs):
        print(f"{t:9.3f} f{frame:<6} {thr[:24]:24} {what}")
    if args.fps:
        print('\n# per second: swaps, max frame interval ms')
        for sec, n, mx in timeline.frames_per_second(tr, args.from_s, args.to_s):
            print(f"{sec:5d}s {n:4d} {mx:8.1f}")


def cmd_gaps(args):
    tr, src = _trace(args)
    rows = timeline.gaps(tr, args.threshold)
    print(f"# {src}: {len(rows)} swap intervals >= {args.threshold} ms")
    for g in rows:
        print(f"{g['t_s']:9.3f} frame {g['frame']:<6} {g['gap_ms']:8.1f} ms  level_load={g['tags'].get('level_load', 0)} "
              f"loading={g['tags'].get('loading', 0)} region={g['tags'].get('region', '')}")


def cmd_threads(args):
    tr, src = _trace(args)
    r = timeline.per_thread(tr, args.from_s, args.to_s)
    print(f"# {src}\n# sampler shares (always-on all-thread guest sampler)")
    print(f"{'thread':40} {'samples':>8} {'WORK%':>6} {'SPIN%':>6} {'WAIT%':>6}")
    for row in r['sampler']:
        print(f"{row['thread'][:40]:40} {row['samples']:8d} {row['WORK']:6.1f} {row['SPIN']:6.1f} {row['WAIT']:6.1f}")
    print('\n# host CPU ms per thread (1 Hz thread_info)')
    for name, ms in list(r['cpu_ms'].items())[:20]:
        print(f"{name[:40]:40} {ms:10.1f}")
    print('\n# guest waits by call site (ms)')
    for w in r['waits_ms'][:15]:
        print(f"{w['ms']:9.1f} {w['kind']:9} {w['thread'][:24]:24} {w['site'][:150]}")


def cmd_stacks(args):
    tr, src = _trace(args)
    hist = timeline.stack_hist(tr, args.from_s, args.to_s, args.thread, args.depth, args.top)
    print(f"# {src}: sampler stack histogram")
    for name, rows in sorted(hist.items(), key=lambda kv: -sum(r[0] for r in kv[1])):
        print(f"\n[{name}]")
        for n, pct, state, chain in rows:
            print(f"  {pct:5.1f}% {n:6d} {state:4} {chain}")


def cmd_digest(args):
    s = Session(args.target)
    for r in s.digest():
        if args.grep and args.grep.lower() not in r['first']['line'].lower():
            continue
        print(f"{r['first']['t_s']:9.3f} x{r['count']:<7} {r['level']:8} {r['first']['line'][:200]}")


def cmd_export(args):
    tr, src = _trace(args)
    out = Path(args.out or (Path(src if isinstance(src, (str, Path)) else '.') / 'trace.perfetto.json'))
    if args.jsonl:
        with open(out.with_suffix('.jsonl'), 'w') as f:
            for e in tr.events:
                f.write(json.dumps(dict(t_s=round(tr.secs(e.t), 6), name=e.name, cat=e.cat, thread=tr.thread_name(e.thread),
                                        frame=e.frame, a=e.a, b=e.b, v=e.v, stack=[f'{x:08X}' for x in e.stack] if e.stack else None,
                                        text=e.text)) + '\n')
        print(out.with_suffix('.jsonl'))
    else:
        n = perfetto.export(tr, out)
        print(f"{out} ({n} trace events; open in https://ui.perfetto.dev)")


def cmd_sym(args):
    for a in args.addresses:
        print(f"{a} {symbols.name(int(a, 16))}")


def cmd_images(args):
    data = json.loads(Path(args.frames).read_text())
    frames = data['frames'] if isinstance(data, dict) else data
    rows = images.dark_dips(frames, args.threshold) if args.kind == 'dark' else images.blue_spikes(frames)
    flagged = [r for r in rows if r.get('flagged')]
    print(f"{args.kind}: {len(rows)} frames checked, {len(flagged)} flagged")
    for r in flagged:
        print(json.dumps(r))


def cmd_phases(args):
    tr, src = _trace(args)
    print(f"# {src}: level loads and recovery (loading tag clears after 60 swaps < 100 ms)")
    tl = timeline.timeline(tr, opens=False, logs='none', min_wait_ms=1e9)
    for t, frame, thr, what in tl:
        if what.startswith(('LEVEL-LOAD', 'TAG loading', 'TAG region', 'TAG level_name', 'TAG save_notice', 'GAP', 'MARK')):
            print(f"{t:9.3f} f{frame:<6} {what}")


def cmd_census(args):
    from . import census
    census.main(args)


def cmd_gpu(args):
    from . import gpu
    out = gpu.analyze(args.target, args.start, args.dur, args.top)
    c = contention(args.target)
    out['contention'], out['perf_comparable'] = c, not c['text']
    if c['text']:
        print(c['text'])
    if args.json:
        Path(args.json).write_text(json.dumps(out, indent=1))
    gpu.print_report(out)


def cmd_run(args):
    from . import run
    run.main(args.rest)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == 'run':  # the harness has its own parser (and -h)
        from . import run
        return run.main(argv[1:])
    p = argparse.ArgumentParser(prog='f2diag', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)

    def target(q, dump=True):
        q.add_argument('target')
        if dump:
            q.add_argument('--dump', help='dump number or reason substring (default: newest)')
            q.add_argument('--stream', action='store_true', help='read trace/stream.f2t instead of a dump')

    q = sub.add_parser('report', help='what to read first about a dump'); target(q); q.add_argument('--window', type=float, default=10.0)
    q.set_defaults(fn=cmd_report)
    q = sub.add_parser('dumps', help='list dumps, hooks, tags of a session'); target(q, False); q.set_defaults(fn=cmd_dumps)
    q = sub.add_parser('timeline', help='events: tags, marks, loads, gaps, long waits, opens, warnings'); target(q)
    q.add_argument('--from', dest='from_s', type=float); q.add_argument('--to', dest='to_s', type=float)
    q.add_argument('--min-wait', type=float, default=50.0); q.add_argument('--no-opens', action='store_true')
    q.add_argument('--logs', default='warning', choices=['info', 'warning', 'error', 'none']); q.add_argument('--fps', action='store_true')
    q.set_defaults(fn=cmd_timeline)
    q = sub.add_parser('gaps', help='swap intervals above a threshold'); target(q); q.add_argument('--threshold', type=float, default=100.0)
    q.set_defaults(fn=cmd_gaps)
    q = sub.add_parser('threads', help='per-thread WORK/SPIN/WAIT, host CPU, waits by site'); target(q)
    q.add_argument('--from', dest='from_s', type=float); q.add_argument('--to', dest='to_s', type=float); q.set_defaults(fn=cmd_threads)
    q = sub.add_parser('stacks', help='sampler stack histogram per thread'); target(q)
    q.add_argument('--from', dest='from_s', type=float); q.add_argument('--to', dest='to_s', type=float)
    q.add_argument('--thread'); q.add_argument('--depth', type=int, default=8); q.add_argument('--top', type=int, default=8)
    q.set_defaults(fn=cmd_stacks)
    q = sub.add_parser('phases', help='level loads, loading/region tags, gaps, marks'); target(q); q.set_defaults(fn=cmd_phases)
    q = sub.add_parser('areaload', help='area-transition breakdown from the game log (FABLE2_LOADING_TRACE=1 FABLE2_GPU_TIME_TRACE=1)')
    q.add_argument('target', help='session folder, logs folder or fable_2.log')
    q.set_defaults(fn=lambda a: __import__('f2diag.areaload', fromlist=['report']).report(a.target))
    q = sub.add_parser('digest', help='log digest (first occurrence + count of each warning/error)'); target(q, False)
    q.add_argument('--grep'); q.set_defaults(fn=cmd_digest)
    q = sub.add_parser('export', help='Perfetto/Chrome trace JSON (or --jsonl)'); target(q)
    q.add_argument('--out'); q.add_argument('--jsonl', action='store_true'); q.set_defaults(fn=cmd_export)
    q = sub.add_parser('sym', help='symbolise guest addresses'); q.add_argument('addresses', nargs='+'); q.set_defaults(fn=cmd_sym)
    q = sub.add_parser('images', help='dark-dip / blue-flash detectors over a sweep JSON (frames with paths)')
    q.add_argument('kind', choices=['dark', 'blue']); q.add_argument('frames'); q.add_argument('--threshold', type=float, default=50.0)
    q.set_defaults(fn=cmd_images)
    q = sub.add_parser('gpu', help='GPU ms/frame, per-pass GPU time, main-pass breaks (FABLE2_GPU_TIME_TRACE=1 sessions)')
    q.add_argument('target'); q.add_argument('--start', type=float, default=20.0); q.add_argument('--dur', type=float, default=30.0)
    q.add_argument('--top', type=int, default=12); q.add_argument('--json'); q.set_defaults(fn=cmd_gpu)
    q = sub.add_parser('census', help='HLE census: draws per call site, pass map, EDRAM aliasing (FABLE2_HLE_CENSUS=1 sessions)')
    q.add_argument('target'); q.add_argument('--min-packets', type=int, default=500); q.add_argument('--frame', type=int)
    q.add_argument('--json'); q.set_defaults(fn=cmd_census)
    q = sub.add_parser('census-cost', help='GPU busy time per pass role and cost bucket (pass trace of a FABLE2_GPU_TIME_TRACE=1 run)')
    q.add_argument('targets', nargs='+'); q.add_argument('--from-frame', type=int, default=600); q.add_argument('--json')
    q.set_defaults(fn=lambda a: __import__('f2diag.census', fromlist=['main_costs']).main_costs(a))
    q = sub.add_parser('parity', help='frame-matched A/B parity of guest captures against the A-A noise floor')
    q.add_argument('arm_a', help='run label(s), comma-separated (two runs give the A-A floor)')
    q.add_argument('arm_b', help='run label(s), comma-separated')
    q.add_argument('--max-bursts', type=int, default=8, help='capture bursts compared per pair (spread evenly)')
    q.add_argument('--block-tol', type=float, default=4.0, help='block excess over its floor that counts as hot')
    q.add_argument('--max-shift', type=int, default=4, help='frame shift searched per burst (0 = exact frame ids)')
    q.add_argument('--json'); q.add_argument('--heat', help='folder for per-pair heat maps')
    q.set_defaults(fn=lambda a: sys.exit(__import__('f2diag.parity', fromlist=['main']).main(a)))
    q = sub.add_parser('run', help='harness plans (sweep, area, move, still, f5, boot, bootloop): f2diag run -h', add_help=False)
    q.add_argument('rest', nargs=argparse.REMAINDER); q.set_defaults(fn=cmd_run)
    args = p.parse_args(argv)
    if getattr(args, 'target', None):  # a bare session id works too (out/dev-sessions/<id> in any checkout)
        args.target = str(resolve(args.target))
    if getattr(args, 'targets', None):
        args.targets = [str(resolve(t)) for t in args.targets]
    args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
