"""`f2diag report <dump>`: what to read first about a dump."""
import json
from pathlib import Path

from . import f2t, symbols, timeline


def report(dump, window_s=10.0, out=print):
    dump = Path(dump)
    summary = json.loads((dump / 'summary.json').read_text())
    tr = f2t.load(dump)
    trig_t = summary['t_s']
    out(f"== {dump.name}: {summary['reason']} — {summary['detail']}")
    out(f"   at {trig_t:.3f} s after recorder start, frame {summary['frame']}, "
        f"{summary['ms_since_last_swap']:.0f} ms since the last swap, in level load: {summary['in_level_load']}")
    out(f"   tags: {json.dumps(summary['tags'])}")
    rec = summary.get('recorder', {})
    out(f"   recorder: {rec.get('events_per_s', 0):.0f} events/s, thread CPU {rec.get('recorder_thread_cpu_pct_of_core', 0):.2f}% of a core, "
        f"{rec.get('guest_threads_sampled')} guest threads sampled at {rec.get('sampler_hz')} Hz, {rec.get('rings')} rings")
    hooks = summary.get('hooks', {})
    if hooks.get('never_fired'):
        out(f"   WARNING hooks that never fired: {', '.join(hooks['never_fired'])}")
    else:
        out(f"   hooks fired: {len(hooks.get('fired', []))} (none silent)")
    if summary.get('first_av'):
        out(f"   first AV: {summary['first_av']}  (AV lines so far: {summary.get('av_lines')})")
    iv = summary.get('last_frame_intervals_ms', [])
    if iv:
        out(f"   last {len(iv)} frame intervals: max {max(iv):.1f} ms, mean {sum(iv) / len(iv):.1f} ms")
    out('\n-- guest stacks at the trigger (3 snapshots, common outer frames):')
    for s in summary['stacks']:
        snap = s['snapshots'][0]
        states = '/'.join(x['state'] for x in s['snapshots'])
        out(f"  [{s['thread']}] {states} lr={symbols.name(snap['lr'])}")
        out(f"      {symbols.chain(snap['chain'][:10])}")
    out(f'\n-- sampler: top stacks per thread in the {min(2.0, window_s):.0f} s before the trigger')
    hist = timeline.stack_hist(tr, trig_t - 2.0, trig_t + 0.01, depth=8, top=3)
    for name, rows in sorted(hist.items(), key=lambda kv: -sum(r[0] for r in kv[1])):
        for n, pct, state, chain in rows:
            out(f"  {name[:28]:28} {pct:5.1f}% {state:4} {chain[:160]}")
    out(f'\n-- timeline, {window_s:.0f} s before the trigger')
    for t, frame, thr, what in timeline.timeline(tr, trig_t - window_s, trig_t + 0.5, opens=False):
        out(f"  {t:9.3f} f{frame:<6} {thr[:22]:22} {what[:180]}")
    digest = dump.parent.parent / 'log-digest.jsonl'
    if digest.exists():
        rows = [json.loads(x) for x in digest.read_text().splitlines() if x.strip()]
        near = [r for r in rows if abs(r['first']['t_s'] - trig_t) <= window_s]
        out(f'\n-- log digest: {len(rows)} warning/error templates; {len(near)} first seen within {window_s:.0f} s of the trigger')
        for r in near[:15]:
            out(f"  {r['first']['t_s']:9.3f} x{r['count']:<6} {r['level']}: {r['first']['line'][:160]}")
