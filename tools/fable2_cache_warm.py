#!/usr/bin/env python3
"""Warm the player's shader cache once, in the background (docs/setup-app.md, "Shader warm-up").

  python3 tools/fable2_cache_warm.py [--state-root DIR] [--new-game] [--scales 1,2,3] [--dwell S] [--game-dwell S]
                                    [--timeout S] [--json]

Starts one background test session of the built game (out/build/...; muted, no window focus, physical
input off, closed by its own session id) on a clone of the state root's saves, settings and shader
cache, with no save loaded: the game boots to the title screen. With --new-game it then presses
through the main menu into a new game for a few seconds. It waits until pipeline warm-up has finished
and the native-shader pool has drained, closes the game, and publishes the session's grown cache into
the state root with the launcher's own append-only write-back (fable2_dev_session.publish_cache).

--scales (default 1,2,3): after that session (at the player's resolution scale), one short title-screen
session per other listed scale builds the scale-specific pipelines (startup warm-up at that scale, fused
resolves, optimized pixel shaders) into the binary archives and Metal's system cache, so the first launch
at 2x or 3x is as fast as at 1x. Each is published like the first. --scales "" skips them.

The cache is compiled on this Mac from the player's own game files and only ever lands in the state
root's cache folder. Nothing goes into the repo.

--state-root defaults to ~/Library/Application Support/Fable II Recomp (the player's own state; the
setup app's install step). Tests must pass a scratch folder. Refuses while a normal (non-test) game
is running (live state only). Runs under /usr/bin/python3 (3.9).
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_dev_session as dev  # noqa: E402
import fable2_instances as instances  # noqa: E402
import fable2_test_control as control  # noqa: E402

LIVE_STATE = Path.home() / 'Library/Application Support/Fable II Recomp'
WARMED = 'background warm-up complete'
# Logged when the "Press A to start" title screen is first up (src/core/fable2_loading_pacing.h); the logo
# movies play before it unless out/skipintro has the trimmed copies.
TITLE_UP = '[title-fast] INTRO_FINISHED sent'
TITLE_FRAMES = 120  # frames drawn on the title screen before checking the pool has drained


def _log_text(log):
    try:
        return log.read_text(errors='replace')
    except OSError:
        return ''


def _counts(text):
    """Shader counts reported by the game log (last values)."""
    out = {}
    for key, pattern in (('translations_indexed', r'indexed (\d+) cached shader translations'),
                         ('warmup_pipelines_compiled', r'warm-up complete in [\d.]+s; .*?compiled (\d+) pipelines'),
                         ('ns_queued', r'\[native-shaders\] background work done .*?queued (\d+)'),
                         ('ns_from_cache', r'\[native-shaders\] background work done .*?\(\+(\d+) from cache'),
                         ('ns_optimize_ms', r'\[native-shaders\] background work done .*?(\d+) ms optimizing')):
        found = re.findall(pattern, text)
        if found: out[key] = int(found[-1])
    return out


def expects_warmup(cache):
    """The game's pipeline warm-up runs (and logs WARMED) only when the cache lists pipelines and has a
    plain binary archive (MetalPipelineCache::InitializeShaderStorageInternal)."""
    shaders = Path(cache) / 'shaders'
    listed = any(p.stat().st_size > 64 for p in shaders.glob('shareable/*.metal.xpso'))
    archive = any('.rtv-bindless.ns' not in p.name for p in shaders.glob('local/metal/*/*/*/*.rtv-bindless.*.binarchive'))
    return listed and archive


def _bg_state(session):
    """(frame, bg_busy, warmup_pending) from the game's readiness state, or None (old build / no recorder)."""
    if session is None: return None
    try:
        st = control.state(session)
    except (RuntimeError, TimeoutError, ValueError):
        return None
    tags = st.get('tags', {})
    if 'bg_busy' not in tags: return None
    try:
        return int(st.get('frame', 0)), int(tags['bg_busy']), bool((st.get('ready') or {}).get('warmup_pending'))
    except (TypeError, ValueError):
        return None


def wait_drained(log, pid, timeout, expect_warmup, quiet_s=10, session=None, quiet_frames=60):
    """Until shader storage is up, pipeline warm-up (logged only when the cache already lists pipelines) has
    finished, and the background shader work has drained (docs/readiness.md): the game's bg_busy readiness tag
    (optimizer / native-translator / ns-spec queues, async pipeline creation, warm-up) has stayed 0 while
    quiet_frames frames were drawn, so draws found no new work. timeout is a guard. Games without the readiness
    state (older builds) fall back to the log: no [native-shaders] / [ns-spec] progress line for quiet_s seconds."""
    deadline, stable, zero_since = time.monotonic() + timeout, None, None
    while time.monotonic() < deadline:
        if not dev.pid_alive(pid): raise RuntimeError('the warm-up game exited early (see %s)' % log)
        text = _log_text(log)
        warmed = 'Metal binary archive: loaded' in text and (WARMED in text or not expect_warmup)
        bg = _bg_state(session)
        if bg is not None:
            frame, busy, warmup_pending = bg
            if warmed and busy == 0 and not warmup_pending:
                if zero_since is None: zero_since = frame
                if frame - zero_since >= quiet_frames: return
            else:
                zero_since = None
            time.sleep(0.1)
            continue
        # Pool progress only: the [ns-spec] guard / auto-learn status lines repeat every 10 s.
        pool = [l for l in text.splitlines() if ('[native-shaders]' in l or '[ns-spec]' in l or '[native-xlate]' in l)
                and '] guard:' not in l and '] auto-learn:' not in l]
        mark = len(pool)
        # FABLE2_NATIVE_XLATE: native MSL compiles log sparsely (powers of two) and can take tens of seconds cold,
        # so with the translator on, wait for the pool's idle line after its last native line.
        if any('[native-xlate]' in l for l in pool):
            last_native = max(i for i, l in enumerate(pool) if '[native-xlate]' in l)
            warmed = warmed and any('background work done' in l for l in pool[last_native:])
        if warmed:
            if stable and stable[0] == mark and time.monotonic() - stable[1] >= quiet_s: return
            if not stable or stable[0] != mark: stable = (mark, time.monotonic())
        else:
            stable = None
        time.sleep(1)
    raise RuntimeError('the shader warm-up did not settle in %d s' % timeout)


def wait_frames(session, frames, timeout=60):
    """Until the game has drawn `frames` more frames (test-control state frame id); timeout is a guard. Returns
    False when the game has no state (then the caller's legacy dwell applies)."""
    try:
        start = int(control.state(session).get('frame', 0))
    except (RuntimeError, TimeoutError, ValueError):
        return False
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if int(control.state(session).get('frame', 0)) - start >= frames: return True
        except (RuntimeError, TimeoutError, ValueError):
            return False
        time.sleep(0.1)
    return True


def start_new_game(session, seconds):
    """From the title screen: A (start), A on the main menu's first entry (New Game), Left + A on the
    hero cards, then Start once the opening movie runs, which skips it into the in-engine prologue
    (young Sparrow in Bowerstone Slums). A every 8 s then advances its dialogue. A is the E key and Start
    is Escape in the default keyboard map. With a save in the state root the first entry is Continue and
    the newest save loads instead, which warms that area; the session's saves are a clone and are never
    written back."""
    # Menu steps: the test-control state has no menu-screen signal yet (src/diagnostics/fable2_state_probe.h runs only
    # on the remote-control channel), so these keep short fixed pauses.
    for key, pause in (('e', 4), ('e', 4), ('left', 2)):
        control.press(session, key, 150)
        time.sleep(pause)
    # The hero card's A starts the opening movie: Start (Escape) as soon as the `movie` tag says it plays (guard 20 s),
    # then wait for the prologue to load and draw (guard 60 s).
    control.press(session, 'e', 150)
    try:
        control.wait_for(session, 'movie', '==', 1, timeout=20)
    except TimeoutError:
        pass
    control.press(session, 'escape', 150)
    control.wait_ready(session, need=('loaded', 'world'), timeout=60)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        time.sleep(min(8, max(0, end - time.monotonic())))
        control.press(session, 'e', 150)


def player_scale(state_root):
    """The resolution scale the player's sessions use (FABLE2_RES_SCALE wins, as in the game); fallback when
    the session log has no translator line. "auto" is resolved by the game, so it counts as 1 here."""
    env = os.environ.get('FABLE2_RES_SCALE', '')
    if env.isdigit(): return max(1, min(3, int(env)))
    try:
        text = (Path(state_root) / 'fable2_config.toml').read_text(errors='replace')
    except OSError:
        return 1
    m = re.search(r'^\s*resolution_scale\s*=\s*(\d+)', text, re.M)
    return max(1, min(3, int(m.group(1)))) if m else 1


def warm_scale(state_root, scale, dwell=0, timeout=600, log_fn=print):
    """One title-screen session at another resolution scale: its startup warm-up builds that scale's
    pipelines from the pipeline list; then the optimizer pool drains; published like warm()."""
    expected = dev.cache_signature(state_root)
    started = time.monotonic()
    saved = {k: os.environ.get(k) for k in ('FABLE2_RES_SCALE', 'FABLE2_WINDOW_TAG')}
    os.environ['FABLE2_RES_SCALE'] = str(scale)
    os.environ['FABLE2_WINDOW_TAG'] = 'warm-up %dx' % scale  # window title (src/core/fable2_window_title.h)
    try:
        session = Path(control.launch(state_source=state_root, save=None,
                                      game_args=['--async_shader_compilation=false']))
    finally:
        for key, value in saved.items():
            if value is None: os.environ.pop(key, None)
            else: os.environ[key] = value
    meta = json.loads((session / 'session.json').read_text())
    pid, log = meta['pid'], session / 'test-state/logs/fable_2.log'
    log_fn('warm-up session %s at %dx (pid %d)' % (session.name, scale, pid))
    try:
        expect = expects_warmup(session / 'test-state/cache')
        wait_drained(log, pid, timeout, expect, session=session)
        deadline = time.monotonic() + 120
        while TITLE_UP not in _log_text(log) and time.monotonic() < deadline:
            if not dev.pid_alive(pid): raise RuntimeError('the warm-up game exited early (see %s)' % log)
            time.sleep(1)
        if not wait_frames(session, TITLE_FRAMES): time.sleep(3)  # title frames drawn (old builds: 3 s)
        time.sleep(dwell)
        wait_drained(log, pid, timeout, expect, session=session)
    finally:
        try:
            control.close(session)
        except Exception as error:  # verified below
            log_fn('close: %r' % error)
    if dev.pid_alive(pid): raise RuntimeError('could not close the warm-up game %d' % pid)
    text = _log_text(log)
    if not dev.clean_exit(session / 'test-state'):
        raise RuntimeError('the %dx warm-up game did not exit cleanly; the cache was not published' % scale)
    result = dict(scale=scale, session=str(session), seconds=round(time.monotonic() - started, 1), **_counts(text))
    result['publish'] = dev.publish_cache(session / 'test-state', state_root, expected)
    return result


def warm(state_root=LIVE_STATE, new_game=False, dwell=0, game_dwell=45, timeout=600, log_fn=print, scales=(1, 2, 3)):
    state_root = Path(state_root).expanduser()
    # A normal game writes the same live cache when it exits; a scratch state root (tests) is ours alone.
    normal = [e for e in instances.instances() if not e.get('test_control')]
    if normal and state_root.resolve() == Path(dev.LIVE_STATE).expanduser().resolve():
        raise RuntimeError('A Fable II game is open (%s); close it first' % instances.describe(normal[0]))
    expected = dev.cache_signature(state_root)
    started = time.monotonic()
    # The session reads the player's settings (resolution scale), so it warms the scale they play at.
    os.environ.setdefault('FABLE2_SKIP_INTRO_MOVIES', '1')
    os.environ.setdefault('FABLE2_WINDOW_TAG', 'warm-up')  # window title (src/core/fable2_window_title.h)
    # Draws wait for their pipeline instead of being skipped while it compiles (SDK cvar
    # async_shader_compilation). A cold cache otherwise skips ~110k draws in this session, which shows
    # as blue/white flashing in its window (background, but visible); stalls here cost nothing.
    session = Path(control.launch(state_source=state_root, save=None,
                                  game_args=['--async_shader_compilation=false']))
    meta = json.loads((session / 'session.json').read_text())
    pid, log = meta['pid'], session / 'test-state/logs/fable_2.log'
    log_fn('warm-up session %s (pid %d)' % (session.name, pid))
    result = dict(session=str(session), state_root=str(state_root), new_game=new_game)
    try:
        expect = expects_warmup(session / 'test-state/cache')
        wait_drained(log, pid, timeout, expect, session=session)
        deadline = time.monotonic() + 120
        while TITLE_UP not in _log_text(log) and time.monotonic() < deadline:
            if not dev.pid_alive(pid): raise RuntimeError('the warm-up game exited early (see %s)' % log)
            time.sleep(1)
        # The title screen (and its menus) render: TITLE_FRAMES frames (old builds: 5 s), plus --dwell extra seconds.
        if not wait_frames(session, TITLE_FRAMES): time.sleep(5)
        time.sleep(dwell)
        if new_game:
            start_new_game(session, game_dwell)
        wait_drained(log, pid, timeout, expect, session=session)
    finally:
        try:
            control.close(session)
        except Exception as error:  # verified below
            log_fn('close: %r' % error)
    if dev.pid_alive(pid): raise RuntimeError('could not close the warm-up game %d' % pid)
    text = _log_text(log)
    result.update(_counts(text), seconds=round(time.monotonic() - started, 1),
                  minimum_target=next((l.split('] ')[-1] for l in text.splitlines() if 'minimum target' in l), None))
    if not dev.clean_exit(session / 'test-state'):
        raise RuntimeError('the warm-up game did not exit cleanly; the cache was not published')
    result['publish'] = dev.publish_cache(session / 'test-state', state_root, expected)
    # The other resolution scales (Settings > Resolution): their pipelines differ (scaled shaders,
    # resolves), so the first launch at a new scale would otherwise build them during its startup.
    # The scale that session actually drew at (also resolves resolution_scale = "auto").
    found = re.findall(r'DxbcShaderTranslator init: .*?scale=(\d)x', text)
    main_scale = int(found[-1]) if found else player_scale(state_root)
    result['scale'] = main_scale
    result['scales'] = []
    for scale in [s for s in scales if s != main_scale]:
        result['scales'].append(warm_scale(state_root, scale, timeout=timeout, log_fn=log_fn))
    result['seconds_total'] = round(time.monotonic() - started, 1)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--state-root', default=str(LIVE_STATE), help='state folder whose cache/ is warmed')
    p.add_argument('--new-game', action='store_true', help='also start a new game from the title screen')
    p.add_argument('--dwell', type=float, default=0,
                   help='extra seconds on the title screen after its first %d frames (default 0)' % TITLE_FRAMES)
    p.add_argument('--game-dwell', type=float, default=45, help='seconds of the new game (prologue)')
    p.add_argument('--scales', default='1,2,3',
                   help='resolution scales to warm (the player\'s own first, then short title sessions); "" = only the player\'s')
    p.add_argument('--timeout', type=float, default=600)
    p.add_argument('--json', action='store_true')
    a = p.parse_args()
    scales = tuple(sorted({max(1, min(3, int(x))) for x in a.scales.split(',') if x.strip()}))
    result = warm(a.state_root, a.new_game, a.dwell, a.game_dwell, a.timeout,
                  log_fn=(lambda s: print(s, file=sys.stderr)) if a.json else print, scales=scales)
    print(json.dumps(result, indent=2) if a.json else
          'shader warm-up done in %.0f s: %s' % (result['seconds_total'], {k: v for k, v in result.items()
                                                                     if k not in ('session', 'publish', 'scales')})
          + ''.join('\n  %dx: %.0f s, %s' % (r['scale'], r['seconds'], {k: v for k, v in r.items()
                    if k not in ('session', 'publish', 'scale', 'seconds')}) for r in result['scales']))


if __name__ == '__main__':
    main()
