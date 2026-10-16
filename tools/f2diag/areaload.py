"""f2diag areaload <target>: area-transition breakdown from the game log alone (docs/area-load-profile-20261015.md).

Log-based so it works on any Mac with no extra tools: run the game with the Extra environment
`FABLE2_LOADING_TRACE=1 FABLE2_GPU_TIME_TRACE=1`, walk through one load zone that autosaves (e.g. the Thag's Cave
Cullis Gate), wait until the new area plays, quit, then point this at the session folder, its logs folder, or
fable_2.log (the player app: ~/Library/Application Support/Fable II Recomp/logs).

Phases (each transition, keyed on the GameThread's own log lines):
  save            save mount ([save-notice] "elapsed N ms" gives the start) -> XamContentClose / [save-notice]
  render wait 1   GameThread wait at 8235DB34 <824EC4A4 (render-object handshake after the save)
  pre-load        -> level-load call enter (worker-0 ops, short waits)
  level-load      sub_825F3050 enter -> exit
  loading frames  -> render wait 2 start (loading-screen frames; the GameThread runs 30 Hz ticks)
  render wait 2   GameThread wait at 8235DB34 <822C20AC <822770B8 (releases the loading screen)
  arrival         first guest swap after it (the black frame before the fade-in; the area shows ~0.25 s later)
Per phase: GameThread / 3D Engine / VFS / Texture Streamer CPU and guest wait ms ([loading-pacing] prof lines,
~100 ms windows), file reads (count, MB, ms in NtReadFile), guest swaps, and with FABLE2_GPU_TIME_TRACE=1 the
frame-CSV GPU ms and per-thread CPU. Untraced logs give the save timing only. Render waits under the 5 ms wait-log
threshold (shortened fades, FABLE2_FAST_PACING=1) need FABLE2_LOAD_HOLD_TRACE=1, whose release lines are used too.
Runs under /usr/bin/python3 (3.9).
"""
import collections
import csv
import datetime
import re
from pathlib import Path

LINE = re.compile(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\] \[(\w+)\] \[(\w+)\] \[(t\d+)\] (.*)')
PROF = re.compile(r't(\d+) (.*?) cpu=([\d.]+) wait=([\d.]+) read=([\d.]+)/(\d+)/(\d+)')
NOTICE = re.compile(r'\[save-notice\] minimum .*elapsed (\d+) ms')
WAIT = re.compile(r'\[loading-pacing\] wait handle=\S+ timeout=\d+ms actual=([\d.]+)ms result=\S+ stack (.*)')
W0 = re.compile(r'\[loading-pacing\] worker0 op actual=([\d.]+)ms')
ENTER = re.compile(r'\[loading-pacing\] level-load sub_825F3050 enter #(\d+)')
# FABLE2_LOAD_HOLD_TRACE=1: every render-object release, also those under the 5 ms wait-log threshold (short fades).
HOLD = re.compile(r'\[load-hold\] t=[\d.]+ render-object release exit: caller (\w+) wait\+teardown ([\d.]+) ms')
EXIT = re.compile(r'\[loading-pacing\] level-load sub_825F3050 exit #(\d+) ([\d.]+)ms')
GROUPS = (('GameThread', 'GameThread'), ('3D Engine', '3D Engine'), ('VFS', 'VFS'), ('Texture Streame', 'TexStreamer'),
          ('GPU Commands', 'GPU Commands'))


def find_log(target):
    p = Path(target)
    for cand in (p, p / 'fable_2.log', p / 'logs/fable_2.log', p / 'test-state/logs/fable_2.log'):
        if cand.is_file():
            return cand
    raise SystemExit('no fable_2.log under %s' % p)


def read_lines(log):
    out = []
    for raw in log.read_text(errors='replace').splitlines():
        m = LINE.match(raw)
        if m:
            t = datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f').timestamp()
            out.append((t, m.group(4), m.group(5)))
    return out


def frame_rows(log, lines):
    """[(wall t, event, duration_us)] from the newest frames-*.csv beside the log (aligned on 'Metal frame log:')."""
    t0 = next((t for t, _, m in lines if m.startswith('Metal frame log:')), None)
    files = sorted(log.parent.glob('frames-*.csv'), key=lambda f: f.stat().st_mtime)
    if t0 is None or not files:
        return []
    rows = []
    with open(files[-1]) as f:
        for r in csv.DictReader(f):
            try:
                rows.append((t0 + int(r['time_us']) / 1e6, r['event'], int(r['duration_us'])))
            except (KeyError, ValueError):
                pass
    return rows


def transitions(lines):
    """One dict of phase boundaries (wall times) per autosaving transition."""
    out = []
    for i, (t, tid, msg) in enumerate(lines):
        m = NOTICE.search(msg)
        if not m:
            continue
        tr = dict(tid=tid, save_start=t - int(m.group(1)) / 1000.0, save_end=t)
        for t2, tid2, msg2 in lines[i + 1:]:
            if t2 - t > 60:
                break
            if tid2 != tid:
                continue
            w = WAIT.search(msg2)
            h = HOLD.search(msg2)
            site, ms = (w.group(2)[:18], float(w.group(1))) if w else (('8235DB34 <' + h.group(1), float(h.group(2))) if h else (None, 0))
            if site == '8235DB34 <824EC4A4' and 'wait1_end' not in tr:
                tr['wait1_end'] = t2
                tr['wait1_start'] = t2 - ms / 1000.0
            e = ENTER.search(msg2)
            if e and 'load_enter' not in tr:
                tr['load_enter'] = t2
            x = EXIT.search(msg2)
            if x and 'load_enter' in tr and 'load_exit' not in tr:
                tr['load_exit'] = t2
            if site == '8235DB34 <822C20AC' and 'load_exit' in tr:
                tr['wait2_end'] = t2
                tr['wait2_start'] = t2 - ms / 1000.0
                break
        out.append(tr)
    return out


def phases_of(tr):
    keys = [('save', 'save_start', 'save_end'), ('render wait 1', 'wait1_start', 'wait1_end'),
            ('pre-load', 'wait1_end', 'load_enter'), ('level-load', 'load_enter', 'load_exit'),
            ('loading frames', 'load_exit', 'wait2_start'), ('render wait 2', 'wait2_start', 'wait2_end')]
    return [(name, tr[a], tr[b]) for name, a, b in keys if a in tr and b in tr]


def group_of(name):
    for prefix, label in GROUPS:
        if name.startswith(prefix):
            return label
    return None


def report(target, file=None):
    log = find_log(target)
    lines = read_lines(log)
    rows = frame_rows(log, lines)
    swaps = [t for t, e, _ in rows if e == 'guest_swap']
    prof = []
    for t, _, msg in lines:
        if '[loading-pacing] prof' in msg:
            prof.append((t, [(g.group(2).strip(), float(g.group(3)), float(g.group(4)), float(g.group(5)),
                              int(g.group(6)), int(g.group(7))) for g in PROF.finditer(msg)]))
    trs = transitions(lines)
    pr = lambda *a: print(*a, file=file)
    pr('log: %s' % log)
    traced = bool(prof)
    if not trs:
        pr('no autosaving transition found ([save-notice] minimum ... line).')
        return []
    if not traced:
        pr('untraced log: save timing only. For the full breakdown run with FABLE2_LOADING_TRACE=1 FABLE2_GPU_TIME_TRACE=1.')
    results = []
    for n, tr in enumerate(trs, 1):
        base = tr['save_start']
        rel = lambda x: x - base
        ph = phases_of(tr)
        arrival = None
        if 'wait2_end' in tr:
            arrival = next((s for s in swaps if s >= tr['wait2_end']), None)
        pr('\n== transition %d (t0 = save start, log %s)' % (n, datetime.datetime.fromtimestamp(base).strftime('%H:%M:%S.%f')[:-3]))
        pr('   %-15s %7s %7s %7s | %-9s %-9s %-9s %-9s %-9s | %-20s | %6s %7s' % (
            'phase', 'from', 'to', 'ms', 'Game c/w', '3D c/w', 'VFS c/w', 'TexS c/w', 'GPUCmd c', 'reads n/MB/ms',
            'swaps', 'gpu ms'))
        res = dict(transition=n, phases=[])
        for name, a, b in ph:
            g = collections.defaultdict(lambda: [0.0, 0.0])
            rd = [0, 0, 0.0]
            for t, prow in prof:
                # a prof line covers the ~100 ms before it; weight it by its overlap with the phase
                w = (min(t, b) - max(t - 0.1, a)) / 0.1
                if w > 0:
                    for name_, c, wt, rms, rn, rb in prow:
                        k = group_of(name_)
                        if k:
                            g[k][0] += c * w
                            g[k][1] += wt * w
                        rd[0] += rn * w
                        rd[1] += rb * w
                        rd[2] += rms * w
            sw = sum(1 for s in swaps if a <= s < b)
            gpu = sum(d for t, e, d in rows if e == 'gpu_cb' and a <= t < b) / 1000.0
            cw = lambda k: ('%4.0f/%-4.0f' % tuple(g[k])) if traced else '   -     '
            pr('   %-15s %7.3f %7.3f %7.0f | %-9s %-9s %-9s %-9s %-9s | %5.0f/%5.1f/%6.1f | %6d %7.1f' % (
                name, rel(a), rel(b), (b - a) * 1000, cw('GameThread'), cw('3D Engine'), cw('VFS'), cw('TexStreamer'),
                ('%4.0f' % g['GPU Commands'][0]) if traced else '-', rd[0], rd[1] / 1e6, rd[2], sw, gpu))
            res['phases'].append(dict(name=name, start_s=round(rel(a), 3), end_s=round(rel(b), 3),
                                      ms=round((b - a) * 1000), cpu_wait_ms={k: [round(v[0]), round(v[1])] for k, v in g.items()},
                                      reads=rd[0], read_mb=round(rd[1] / 1e6, 1), read_ms=round(rd[2], 1), swaps=sw,
                                      gpu_ms=round(gpu, 1)))
        if arrival is not None:
            pr('   arrival (first swap after render wait 2): %.3f s after t0; the new area shows about 0.25 s later (fade-in)'
               % rel(arrival))
            res['arrival_s'] = round(rel(arrival), 3)
        w0 = [float(W0.search(m).group(1)) for t, tid, m in lines
              if tid == tr['tid'] and W0.search(m) and tr['save_start'] <= t <= tr.get('wait2_end', tr['save_end'] + 10)]
        long_waits = [(t, float(w.group(1)), w.group(2)) for t, tid, m in lines for w in [WAIT.search(m)]
                      if w and tid == tr['tid'] and tr['save_start'] <= t <= tr.get('wait2_end', tr['save_end'] + 10)
                      and float(w.group(1)) >= 20]
        if traced:
            pr('   GameThread worker-0 ops: %d, %.0f ms in total' % (len(w0), sum(w0)))
            for t, ms, stack in long_waits:
                pr('   GameThread wait %6.0f ms ending %.3f s: %s' % (ms, rel(t), ' '.join(stack.split()[:4])))
            pr('   columns: c/w = CPU / guest-wait ms per thread group in the phase; GameThread CPU + wait < phase ms '
               'means host-side blocking (e.g. the 30 Hz tick sleep in loading frames)')
        results.append(res)
    return results
