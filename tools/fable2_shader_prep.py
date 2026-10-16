#!/usr/bin/env python3
"""Headless shader preparation: build a shader cache from remembered lists, no game loop (docs/offline-shader-seed.md).

  python3 tools/fable2_shader_prep.py run --from CACHE_DIR --out CACHE_DIR [--scales 1,2,3] [--json]
  python3 tools/fable2_shader_prep.py run --empty --out CACHE_DIR [--scales 2,1]   disc shader bank only (first install)
  python3 tools/fable2_shader_prep.py lists              newest remembered pipeline lists (agent cache / seed store)

`run` starts the built game (out/build/...) once per resolution scale with FABLE2_SHADER_PREP set, as a background
test session (pid-scoped, muted, input-isolated, never the player's state). The game loads the shader and pipeline lists
of --from (cache/shaders/shareable/*.xsh + *.metal.xpso, plus any translations already there), runs its normal
startup warm-up (translation, native translator, optimizer, pipeline creation into the binary archives, transfer /
resolve / tile prewarms), never starts the guest, and exits by itself once every queue is empty
(metal_graphics_system.cc RunShaderPrep). Each scale continues from the previous one's cache; --out receives the last.
No save, no title screen, no input. The converter target is pinned (apple7:15.0), as for every build.

The lists are game-derived: everything here stays in ignored out/. Runs under /usr/bin/python3 (3.9).
"""
import argparse
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_common as common  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
TITLE = '4D5307F1'
MSC_PIN = 'apple7:15.0'
SEED_STORE = common.MAIN_ROOT / 'out/shader-seed-store'


def lists_in(cache):
    shareable = Path(cache) / 'shaders/shareable'
    xsh, xpso = shareable / ('%s.xsh' % TITLE), shareable / ('%s.metal.xpso' % TITLE)
    return (xsh, xpso) if xsh.is_file() and xpso.is_file() else None


def newest_lists():
    """(cache dir, source text) of the newest remembered shareable lists: the agent cache's latest generations and the
    seed store. The lists hold only game content (shader microcode, pipeline descriptions), so any build's are valid
    input; the biggest pipeline list wins (lists only grow), newest on a tie."""
    import fable2_agent_cache as agent_cache
    best = None
    for key in agent_cache.keys():
        meta = agent_cache.load_meta(key) or {}
        gen = meta.get('generation')
        if gen is None: continue
        cache = agent_cache.gen_dir(key, int(gen))
        found = lists_in(cache)
        if found:
            cand = (found[1].stat().st_size, found[1].stat().st_mtime, str(cache), 'agent cache %s@%s' % (key, gen))
            if best is None or cand > best: best = cand
    for seed in sorted(SEED_STORE.glob('*/native-shaders')) if SEED_STORE.is_dir() else []:
        found = lists_in_seed(seed)
        if found:
            cand = (found[1].stat().st_size, found[1].stat().st_mtime, str(seed), 'seed store %s' % seed.parent.name)
            if best is None or cand > best: best = cand
    return (Path(best[2]), best[3]) if best else (None, None)


def lists_in_seed(seed):
    xsh, xpso = Path(seed) / 'shareable' / ('%s.xsh' % TITLE), Path(seed) / 'shareable' / ('%s.metal.xpso' % TITLE)
    return (xsh, xpso) if xsh.is_file() and xpso.is_file() else None


def lists_only_cache(source, dest):
    """A cache folder holding only the shareable lists of `source` (a cache dir or a native-shaders seed dir)."""
    found = lists_in(source) or lists_in_seed(source)
    if not found: raise RuntimeError('no shader/pipeline lists in %s' % source)
    shareable = Path(dest) / 'shaders/shareable'
    shareable.mkdir(parents=True, exist_ok=True)
    for path in found: shutil.copy2(path, shareable / path.name)
    return Path(dest)


def _read_status(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return None


def prep_scale(cache_from, scale, work, timeout=1800, log_fn=print, progress=None, app=None):
    """One headless prep session at `scale` seeded from cache_from. Returns (session cache dir, status dict).
    app: another Fable II.app to run (default: this checkout's build), e.g. one with its own bundle id, whose macOS
    Metal cache the prep then fills."""
    import fable2_dev_session as dev
    import fable2_test_control as control
    state_root = Path(work) / ('state-%dx' % scale)
    if state_root.exists(): shutil.rmtree(state_root)
    state_root.mkdir(parents=True)
    common.clone(cache_from, state_root / 'cache')
    status_path = Path(work) / ('prep-%dx.json' % scale)
    if status_path.exists(): status_path.unlink()
    keys = ('FABLE2_SHADER_PREP', 'FABLE2_RES_SCALE', 'FABLE2_MSC_MIN_TARGET', 'FABLE2_WINDOW_TAG')
    saved = {k: os.environ.get(k) for k in keys}
    os.environ.update(FABLE2_SHADER_PREP=str(status_path), FABLE2_RES_SCALE=str(scale),
                      FABLE2_MSC_MIN_TARGET=MSC_PIN, FABLE2_WINDOW_TAG='shader prep %dx' % scale)
    started = time.monotonic()
    default_app = control.APP
    if app: control.APP = Path(app)
    try:
        session = Path(control.launch(state_source=state_root, save=None))
    finally:
        control.APP = default_app
        for k, v in saved.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
    meta = json.loads((session / 'session.json').read_text())
    pid = meta['pid']
    log_fn('shader prep %dx: session %s (pid %d)' % (scale, session.name, pid))
    status, last_phase = None, None
    deadline = time.monotonic() + timeout
    # The game exits by itself once its queues are empty; waiting on its pid is the readiness signal (the status file
    # carries progress and the result). The timeout is a guard only.
    while dev.pid_alive(pid):
        if time.monotonic() > deadline:
            try:
                control.close(session)
            except Exception:
                pass
            raise RuntimeError('shader prep %dx did not finish in %d s (see %s)' % (scale, timeout, session))
        status = _read_status(status_path) or status
        if status and progress: progress(scale, status)
        if status and status.get('phase') != last_phase:
            last_phase = status.get('phase')
        time.sleep(0.25)
    status = _read_status(status_path) or {}
    if not status.get('done'):
        raise RuntimeError('shader prep %dx exited without finishing (see %s/test-state/logs/fable_2.log)' % (scale, session))
    status['session'] = str(session)
    status['wall_seconds'] = round(time.monotonic() - started, 1)
    status['scale'] = scale
    return session / 'test-state/cache', status


def run(cache_from, out, scales=(1, 2, 3), timeout=1800, log_fn=print, progress=None, app=None):
    """Prep every scale in turn (each from the previous cache); copy the final cache to `out`. Returns stats."""
    started = time.monotonic()
    out = Path(out)
    work = Path(tempfile.mkdtemp(prefix='shader-prep-', dir=str(ROOT / 'out')))
    try:
        cache, stats = Path(cache_from), []
        for scale in scales:
            cache, status = prep_scale(cache, scale, work, timeout, log_fn, progress, app)
            stats.append(status)
            log_fn('shader prep %dx: %.1f s, %s pipelines compiled (%s ms), %s archive hits, %s native, %s optimized'
                   % (scale, status['wall_seconds'], status.get('pipelines_compiled'), status.get('pipeline_compile_ms'),
                      status.get('archive_hits'), status.get('native_xlate'), status.get('ns_ready')))
        if out.exists(): shutil.rmtree(out)
        common.clone(cache, out)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return dict(out=str(out), scales=stats, seconds=round(time.monotonic() - started, 1))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--from', dest='source', help='cache folder (or native-shaders seed) with the lists; default: '
                   'the newest remembered lists')
    r.add_argument('--lists-only', action='store_true', help='start from the lists alone (no translations)')
    r.add_argument('--empty', action='store_true', help='start from an empty cache: only the disc shader bank '
                   '(FABLE2_SHADER_PREP_BANK, docs/shader-precompile-20261015.md), as on a first install')
    r.add_argument('--out', required=True)
    r.add_argument('--scales', default='1,2,3')
    r.add_argument('--timeout', type=float, default=1800)
    r.add_argument('--app', help='Fable II.app to run (default: out/build); e.g. a clone with its own bundle id')
    r.add_argument('--json', action='store_true')
    sub.add_parser('lists')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'lists':
            cache, source = newest_lists()
            result = dict(cache=str(cache) if cache else None, source=source)
        else:
            tmp = None
            if a.empty:
                tmp = Path(tempfile.mkdtemp(prefix='lists-', dir=str(ROOT / 'out')))
                source = tmp / 'cache'
                source.mkdir()
            else:
                source = Path(a.source) if a.source else newest_lists()[0]
                if source is None: raise RuntimeError('no remembered shader lists (agent cache or seed store)')
            if not a.empty and (a.lists_only or lists_in(source) is None):
                tmp = Path(tempfile.mkdtemp(prefix='lists-', dir=str(ROOT / 'out')))
                source = lists_only_cache(source, tmp / 'cache')
            try:
                scales = [int(x) for x in a.scales.split(',') if x.strip()]
                result = run(source, a.out, scales, a.timeout,
                             log_fn=(lambda s: print(s, file=sys.stderr)) if a.json else print, app=a.app)
            finally:
                if tmp: shutil.rmtree(tmp, ignore_errors=True)
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps({'ok': True, 'result': result}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
