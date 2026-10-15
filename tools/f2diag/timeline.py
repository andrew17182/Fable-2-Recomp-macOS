"""Analyses over a flight-recorder Trace: timeline, frame gaps, per-thread breakdown, stack histogram."""
import collections

from . import symbols
from .f2t import STATES, LOG_LEVELS, F_BEGIN, F_END

TAG_NAMES = ['level_load', 'loading', 'save_notice', 'region', 'movie', 'turbo', 'fps_mode', 'level_name',
             'quest', 'focus', 'save', 'selftest', 'contention']


def _window(tr, t_from, t_to):
    lo = -1e18 if t_from is None else t_from
    hi = 1e18 if t_to is None else t_to
    return lambda e: lo <= tr.secs(e.t) < hi


def tag_events(tr):
    """[(t_s, tag, value)] from the rings (value: text for string tags)."""
    out = []
    for e in tr.by_name('tag'):
        name = TAG_NAMES[e.a] if e.a < len(TAG_NAMES) else f'tag{e.a}'
        out.append((tr.secs(e.t), name, e.text if e.text is not None else e.v))
    return out


def tags_at(tr, t_s):
    tags = {}
    for t, k, v in tag_events(tr):
        if t > t_s:
            break
        tags[k] = v
    return tags


def gaps(tr, threshold_ms=100.0):
    """Swap-to-swap intervals above threshold: [{t_s, frame, gap_ms, in_level_load, tags}]."""
    out = []
    for e in tr.by_name('swap'):
        ms = tr.ms(e.v)
        if ms >= threshold_ms and e.v:
            t = tr.secs(e.t)
            out.append(dict(t_s=round(t, 3), frame=e.a, gap_ms=round(ms, 1), tags=tags_at(tr, t)))
    for e in tr.by_name('gap'):
        for g in out:
            if g['frame'] == e.a:
                g['in_level_load'] = bool(e.b)
    return out


def frames_per_second(tr, t_from=None, t_to=None):
    """[(second, swaps, max_interval_ms)]"""
    inside = _window(tr, t_from, t_to)
    bins = collections.defaultdict(lambda: [0, 0.0])
    for e in tr.by_name('swap'):
        if not inside(e):
            continue
        b = bins[int(tr.secs(e.t))]
        b[0] += 1
        b[1] = max(b[1], tr.ms(e.v))
    return [(k, v[0], round(v[1], 1)) for k, v in sorted(bins.items())]


def samples(tr, t_from=None, t_to=None):
    """Sampler records with weights: [(t_s, guest_idx, state, stack, weight)] (repeats folded in)."""
    inside = _window(tr, t_from, t_to)
    last, out = {}, []
    for e in tr.events:
        if e.name == 'sample':
            idx, state = e.b & 0xFFFF, (e.b >> 16) & 0xFF
            rec = [tr.secs(e.t), idx, STATES.get(state, '?'), tuple(e.stack or [e.a]), 1]
            last[idx] = rec
            if inside(e):
                out.append(rec)
        elif e.name == 'sample_repeat':
            rec = last.get(e.a)
            if rec is not None and inside(e):
                rec[4] += e.b
    return out


def per_thread(tr, t_from=None, t_to=None):
    """Per guest thread: sample shares WORK/SPIN/WAIT, plus host CPU ms per thread name."""
    share = collections.defaultdict(collections.Counter)
    for t, idx, state, stack, w in samples(tr, t_from, t_to):
        share[tr.guest_thread_name(idx)][state] += w
    rows = []
    for name, c in share.items():
        n = sum(c.values())
        rows.append(dict(thread=name, samples=n, **{k: round(100.0 * c[k] / n, 1) for k in ('WORK', 'SPIN', 'WAIT')}))
    rows.sort(key=lambda r: -r['samples'])
    inside = _window(tr, t_from, t_to)
    cpu = collections.Counter()
    names = {h['tid']: h['name'] for h in tr.header.get('host_threads', [])}
    for e in tr.by_name('cpu'):
        if inside(e):
            cpu[names.get(e.a | (e.b << 32), str(e.a))] += e.v / 1e6
    waits = collections.Counter()
    for e in tr.by_name('wait', 'long_wait', 'delay', 'poll_wait'):
        if inside(e) and e.stack:
            waits[(tr.thread_name(e.thread), e.name, symbols.chain(e.stack[:4]))] += tr.ms(e.v)
    return dict(sampler=rows, cpu_ms=dict(cpu.most_common()),
                waits_ms=[dict(thread=k[0], kind=k[1], site=k[2], ms=round(v, 1)) for k, v in waits.most_common(25)])


def stack_hist(tr, t_from=None, t_to=None, thread=None, depth=8, top=10):
    """{thread: [(weight, state, symbolised stack)]} from the always-on sampler."""
    hist = collections.defaultdict(collections.Counter)
    for t, idx, state, stack, w in samples(tr, t_from, t_to):
        name = tr.guest_thread_name(idx)
        if thread and thread.lower() not in name.lower():
            continue
        hist[name][(state, tuple(stack[:depth]))] += w
    out = {}
    for name, c in hist.items():
        total = sum(c.values())
        out[name] = [(n, round(100.0 * n / total, 1), st, symbols.chain(stk)) for (st, stk), n in c.most_common(top)]
    return out


NOTABLE = {'tag', 'mark', 'trigger', 'level_load', 'gap', 'long_wait', 'save_notice', 'content_close', 'open'}


def timeline(tr, t_from=None, t_to=None, min_wait_ms=50.0, opens=True, logs='warning'):
    """Readable event list: [(t_s, frame, thread, what)]."""
    inside = _window(tr, t_from, t_to)
    min_level = {'info': 2, 'warning': 3, 'error': 4}.get(logs, 9)
    # A wait through the guest wrapper (KeWaitForSingleObject_Guest, flags END, a = handle) is also
    # recorded by the kernel import underneath; show only the wrapper's row.
    wrapped = {(e.thread, e.t // 50000) for e in tr.events if e.name == 'wait' and e.flags & F_END}
    rows = []
    for e in tr.events:
        if not inside(e):
            continue
        if e.name in ('wait', 'long_wait') and not e.flags & F_END and (e.thread, e.t // 50000) in wrapped:
            continue
        t = round(tr.secs(e.t), 3)
        thr = tr.thread_name(e.thread)
        n = e.name
        if n == 'tag':
            name = TAG_NAMES[e.a] if e.a < len(TAG_NAMES) else e.a
            rows.append((t, e.frame, thr, f'TAG {name} = {e.text if e.text is not None else e.v}'))
        elif n == 'mark':
            rows.append((t, e.frame, thr, f'MARK {e.text}'))
        elif n == 'level_load':
            rows.append((t, e.frame, thr, 'LEVEL-LOAD begin #%d' % e.b if e.flags & F_BEGIN
                         else 'LEVEL-LOAD end (%.0f ms)' % tr.ms(e.v)))
        elif n == 'gap':
            rows.append((t, e.frame, thr, f'GAP {tr.ms(e.v):.0f} ms before frame {e.a}' + (' (in level load)' if e.b else '')))
        elif n in ('wait', 'long_wait', 'delay', 'poll_wait', 'worker0') and tr.ms(e.v) >= min_wait_ms:
            site = symbols.chain(e.stack[:5]) if e.stack else f'{e.a:08X}'
            extra = f' handle {e.a:#x} result {e.b:#x}' if n == 'wait' and e.flags & F_END else ''
            rows.append((t, e.frame, thr, f'{n.upper()} {tr.ms(e.v):.0f} ms at {site}{extra}'))
        elif n == 'save_notice':
            rows.append((t, e.frame, thr, f'SAVE-NOTICE requested {e.a} ms, slept {e.b} ms'))
        elif n == 'content_close':
            rows.append((t, e.frame, thr, f'CONTENT-CLOSE {tr.ms(e.v):.1f} ms result {e.a:#x}'))
        elif n == 'open' and opens:
            rows.append((t, e.frame, thr, f'OPEN {e.text}'))
        elif n == 'log' and e.a >= min_level:
            rows.append((t, e.frame, 'log', f'LOG {LOG_LEVELS.get(e.a, e.a)}: {(e.text or "")[:200]}'))
    return rows
