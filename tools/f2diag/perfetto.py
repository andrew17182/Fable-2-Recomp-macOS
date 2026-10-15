"""Chrome trace-event JSON (opens in ui.perfetto.dev or chrome://tracing) from a Trace."""
import json

from . import symbols
from .f2t import F_BEGIN, STATES
from .timeline import TAG_NAMES

DURATION_EVENTS = {'wait', 'long_wait', 'delay', 'poll_wait', 'worker0', 'content_close', 'read'}


def export(tr, out_path):
    ev = []
    pid = 1
    for idx, t in tr.threads.items():
        ev.append(dict(ph='M', pid=pid, tid=idx, name='thread_name', args=dict(name=t.get('name') or f'ring{idx}')))
    guest_tid0 = 1000
    for idx, g in tr.guest_threads.items():
        ev.append(dict(ph='M', pid=pid, tid=guest_tid0 + idx, name='thread_name', args=dict(name='sampler: ' + g['name'])))
    us = lambda t: (t - tr.start['mach']) * tr.ns_per_tick / 1000.0
    for e in tr.events:
        ts = us(e.t)
        if e.name == 'swap':
            ev.append(dict(ph='i', s='t', pid=pid, tid=e.thread, ts=ts, name='swap', args=dict(frame=e.a, interval_ms=round(tr.ms(e.v), 2))))
            if e.v:
                ev.append(dict(ph='C', pid=pid, ts=ts, name='frame ms', args=dict(ms=round(tr.ms(e.v), 2))))
        elif e.name in DURATION_EVENTS:
            dur = e.v * tr.ns_per_tick / 1000.0 if e.name != 'read' else e.v * tr.ns_per_tick / 1000.0
            args = dict(a=hex(e.a), b=e.b)
            if e.stack:
                args['stack'] = symbols.chain(e.stack[:8])
            ev.append(dict(ph='X', pid=pid, tid=e.thread, ts=ts - dur, dur=dur, name=e.name, args=args))
        elif e.name == 'level_load':
            ev.append(dict(ph='B' if e.flags & F_BEGIN else 'E', pid=pid, tid=e.thread, ts=ts, name='level_load'))
        elif e.name == 'sample':
            idx = e.b & 0xFFFF
            ev.append(dict(ph='i', s='t', pid=pid, tid=guest_tid0 + idx, ts=ts, name=STATES.get((e.b >> 16) & 0xFF, '?'),
                           args=dict(stack=symbols.chain((e.stack or [e.a])[:10]))))
        elif e.name == 'tag':
            name = TAG_NAMES[e.a] if e.a < len(TAG_NAMES) else str(e.a)
            ev.append(dict(ph='i', s='g', pid=pid, ts=ts, name=f'{name}={e.text if e.text is not None else e.v}'))
        elif e.name in ('mark', 'trigger'):
            ev.append(dict(ph='i', s='g', pid=pid, ts=ts, name=f'{e.name}: {e.text}'))
        elif e.name == 'gap':
            ev.append(dict(ph='i', s='g', pid=pid, ts=ts, name=f'GAP {tr.ms(e.v):.0f} ms'))
        elif e.name == 'log':
            ev.append(dict(ph='i', s='t', pid=pid, tid=0, ts=ts, name=(e.text or '')[:120]))
        elif e.name == 'open':
            ev.append(dict(ph='i', s='t', pid=pid, tid=e.thread, ts=ts, name='open ' + (e.text or '')))
        elif e.name == 'cpu':
            pass
        elif e.cat in ('frame', 'gpu', 'cpu', 'sampler', 'draw', 'shmem', 'tex'):  # frame-CSV bridge rows
            ev.append(dict(ph='i', s='t', pid=pid, tid=e.thread, ts=ts, name=e.name, args=dict(frame=e.a, value=e.v)))
    with open(out_path, 'w') as f:
        json.dump(dict(traceEvents=ev, displayTimeUnit='ms'), f)
    return len(ev)
