"""Test harness: launch a background test session and drive it (moved out of out/validation).

Replaces the copy-pasted launch/capture/pad code of
  flash-20261007/sweep.py, cr-fixes-20261007/sweep.py, playtest-20261005/sweep.py (camera sweep),
  cr-fixes-20261007/boot.py, load-profile-20261006/boot.py (boot + capture loop),
  area-load-20261008/run.py + press.py (area transition with non-blocking pad hold and presses),
  flash-20261007/movecheck.py (movement check), cr-fixes-20261007/f5save.py (quick save check)
and their monkeypatches of subprocess.run / prepare_session: game args and warm caches are now
fable2_dev_session.launch(game_args=, cache_from=) parameters (test sessions only).

Always background test mode (--test-control, target slot none). Other instances (the player's playtest game)
may run: the harness only ever addresses its own pid. Every result records the instances that ran at the
same time (result.json contention / perf_comparable) and prints a CONTENTION line when there were any;
perf numbers from such runs are not comparable. --build playtest|latest picks the app (default latest).
Output: out/validation/f2diag/<label>/ (launch.json, events.jsonl, caps.jsonl, cpu.jsonl, result.json).

CLI:  python3 tools/f2diag.py run sweep  <label> [--fps uncapped|60|30] [--plan default|fast] [--seconds N]
      python3 tools/f2diag.py run area   <label> [--hold 4000] [--max 70] [--press-at N (legacy fixed time)] [--cpu --cpu-period 0.25]
      python3 tools/f2diag.py run move   <label>
      python3 tools/f2diag.py run still  <label> [--at-frame load+240|clear+N|N|off] [--frames 3] [--frame-step 30]
                    (parity capture, docs/readiness.md "Frame-matched parity": after readiness, bursts of
                    FABLE2_PARITY_BURST (9) consecutive guest frames at fixed frames after the level load, read back
                    by the game (test-control `capture at`), so every arm captures the same guest moments; compare
                    arms with `f2diag parity`. --at-frame off: the old capture once the picture is still)
      python3 tools/f2diag.py run sweep <label> --sweep-plan spin5 --at-frame load+300   (frame-matched sweep)
      python3 tools/f2diag.py run f5     <label>
      python3 tools/f2diag.py run boot   <label> [--max 30]
      python3 tools/f2diag.py run bootloop <label> [--n 10]   (N boots, close at the title; results.jsonl pass/hang/timeout/fail;
                                                            fail = any guest AV or [FATAL] line in the log; other plans set result verdict ok/fail)
      python3 tools/f2diag.py run verdict <session dir> [--no-close-asked]   (recompute a past session's verdict)
      Every verdict also fails when the game exited unexpectedly: process gone before the harness close
      (exited_early), a ~/Library/Logs/DiagnosticReports/fable_2-*.ips crash report for the pid (crashed; path,
      faulting thread and frames in crash_reports), or no clean-exit marker after the harness asked to close.
      common: --save NAME --env K=V ... --game-arg=--cvar=value ... --cache-from DIR --cache-snapshot MODE --no-close
      Shader cache: sessions seed from the agent shader cache of the build's shader-code key and publish back after a
      clean close (tools/fable2_agent_cache.py); result.json agent_cache records the seed
      (key@generation:content hash). A/B batches: SNAP=$(python3 tools/fable2_agent_cache.py pin --label X), then
      --cache-snapshot $SNAP on every arm. --cache-snapshot cold = empty cache (cold-start measurements).
      Start point (docs/readiness.md): sweep/move/f5/area start when the game reports ready (loaded, world drawing,
      steady pacing, no background shader work; test-control `state`), not after a fixed time. FABLE2_READY_TIMEOUT
      (60 s) is a guard; result.json `ready` says when it was ready and whether the guard fired. --wait N is the
      legacy fixed start. Sweep plans: headings8 (8 x 0.75 s holds, ~11 s; the perf standard), headings8-long
      (2.5 s holds, ~24 s; rerun with it when a delta is within ~2%), turn30, spin5, default.
      Perf lock (local agent docs): perf measurement runs (sweep plans headings8/turn30, or --fps uncapped)
      take a global lock shared by the main checkout and every agent slot, so speed runs take turns; the wait
      is printed (PERF LOCK: ...) and recorded as perf_lock_wait_s. --perf-lock forces it, --no-perf-lock skips
      it. Correctness runs (spin5, default sweeps, area, boot, f5, bootloop) hold it shared for each game's life
      (fable2_dev_session.launch): they run beside each other, wait while a perf run holds or waits for the lock,
      and record that wait as perf_lock_shared_wait_s. --no-perf-lock skips the shared hold too.
"""
import argparse
import contextlib
import json
import os
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import fable2_dev_session as dev  # noqa: E402
import fable2_instances as instances  # noqa: E402
import fable2_test_control as control  # noqa: E402
import fable2_common as common  # noqa: E402  (global perf lock across agent slots)
from fable2_replay import atomic_write, clock_us  # noqa: E402

from . import images, pacing, threadcpu  # noqa: E402

DEFAULT_SAVE = 'hero1-dog-live_2026-10-07_18-49-47'  # Thag's Cave, next to the Cullis Gate
OUT = ROOT / 'out/validation/f2diag'


def mono():
    return time.clock_gettime(time.CLOCK_MONOTONIC)


class GameExited(RuntimeError):
    """The game process went away before the harness closed it (window closed by hand, crash, kill).
    Plans stop at the next capture/request; the verdict then says which (user-closed or fail)."""


class Run:
    """One background test session with an event log and helpers."""

    def __init__(self, label, save=DEFAULT_SAVE, env=(), game_args=(), cache_from=None, fps='60', build='latest',
                 cache_snapshot=None):
        self.out = OUT / label
        self.out.mkdir(parents=True, exist_ok=True)
        env = list(env)
        if not any(e.startswith('FABLE2_WINDOW_TAG=') for e in env):  # window title: "· test agent-2 · <label>"
            env.append('FABLE2_WINDOW_TAG=%s \u00b7 %s' % (dev.common.window_tag(), label.split('/')[0]))
        self.record = dev.launch('library', save, 'none', test_control=True, fps=fps, extra_env=env,
                                 game_args=list(game_args), cache_from=cache_from, build=build,
                                 cache_snapshot=cache_snapshot)
        if self.record.get('contention'):
            print(self.record['contention'], file=sys.stderr, flush=True)
        self.session = Path(self.record['session'])
        self.pid = self.record['pid']
        self.origin = int(self.record['env']['FABLE2_SESSION_ORIGIN_US']) / 1e6
        # The session origin is stamped before the launch waits for a game slot / the perf lock, so plan deadlines
        # (--max, the ready guard) count from here, when the game was spawned; session times (t) stay on the origin.
        self.spawned = mono()
        self.queue_s = round(self.spawned - self.origin, 3)
        self.folder = self.session / 'test-state/control'
        self.pad_path = self.folder / 'replay-pad.txt'
        self._events = open(self.out / 'events.jsonl', 'a')
        self._lock = threading.Lock()
        self._pad_lock = threading.Lock()
        self._stop = threading.Event()
        self.closing = False        # set once the harness itself asks the game to close
        self.exited_early_t = None  # session time at which the process was first seen gone
        # wall_minus_mono: puts the game log's wall-clock lines on the session clock (t = wall - this - origin).
        (self.out / 'launch.json').write_text(json.dumps(dict(launch=self.record, origin=self.origin,
                                                              queue_s=self.queue_s,
                                                              wall_minus_mono=time.time() - mono()), indent=2))
        deadline = mono() + 40
        while not (self.folder / 'pid.txt').exists() and mono() < deadline:
            time.sleep(.02)
        self.event(kind='control-ready')

    # -- logging / clock
    def t(self):
        return mono() - self.origin

    def since_spawn(self):
        """Seconds since the game was spawned (excludes time queued for a game slot or the perf lock)."""
        return mono() - self.spawned

    def wait_ready(self, wait=None, need=control.READY_ALL):
        """Start point of a plan (docs/readiness.md): until the game says it is ready (loaded, world drawing, steady
        pacing, no background shader work), with the FABLE2_READY_TIMEOUT guard (60 s). `wait` (legacy --wait N)
        instead sleeps until N s after launch, as before 2026-10-13. Logged as a 'ready' event; returned for the
        result (ready_s from the call, t = session time)."""
        if wait is not None:
            time.sleep(max(0.0, wait - self.since_spawn()))
            info = dict(mode='fixed-wait', wait=wait, session_t=round(self.t(), 3))
        else:
            info = dict(mode='signals', **control.wait_ready(self.session, need=need, alive=self.alive))
            info['session_t'] = round(self.t(), 3)
            if info.get('timed_out'):
                print('READY: guard fired after %.1f s; missing %s (measuring anyway)'
                      % (info['ready_s'], ', '.join(info['missing'])), file=sys.stderr, flush=True)
        self.event(kind='ready', **{k: v for k, v in info.items() if k != 'detail'})
        self.check()
        return info

    def event(self, **k):
        with self._lock:
            self._events.write(json.dumps(dict(t=round(self.t(), 3), **k)) + '\n')
            self._events.flush()

    # -- requests
    def request(self, cmd, timeout=20):
        self.check()
        reply = control.send(self.session, cmd)
        try:
            return control.wait_reply(reply, timeout)
        except TimeoutError:
            self.check()
            raise

    def check(self):
        """Raise GameExited when the game is gone (e.g. the player closed the window); plans stop cleanly."""
        if not self.closing and not self.alive():
            raise GameExited(f'game pid {self.pid} exited before the harness close (t={self.exited_early_t}s)')

    def capture(self, screen=False, phase=None):
        t0 = mono()
        self.check()
        try:
            path = Path(self.request('capture screen' if screen else 'capture', 5))
        except GameExited:
            raise
        except Exception as e:
            self.check()
            return dict(req_t=round(t0 - self.origin, 3), error=str(e), phase=phase)
        m = {}
        try:
            m = json.loads(path.with_suffix('.capture.json').read_text())
        except (OSError, ValueError):
            pass
        m.update(path=str(path), req_t=round(t0 - self.origin, 3), rep_t=round(self.t(), 3), phase=phase)
        return m

    def capture_at(self, frame, count=1, step=1, timeout=None, phase=None):
        """Guest captures of exactly guest frames frame, frame+step, ... (fable2_test_control.capture_at). Returns
        (captures, error): error is None, or 'passed' / 'timeout' / the message (captures = what was captured)."""
        self.check()
        if timeout is None:
            timeout = float(os.environ.get('FABLE2_FRAME_CAPTURE_TIMEOUT', 30)) + count * step / 30.0
        t0 = mono()
        try:
            caps, err = control.capture_at(self.session, frame, timeout, count, step), None
        except control.FrameMissed as e:
            caps, err = [], 'passed (game at frame %d)' % e.current
        except TimeoutError as e:
            self.check()
            caps, err = getattr(e, 'captures', []), 'timeout: %s' % e
        for m in caps:
            m.update(req_t=round(t0 - self.origin, 3), rep_t=round(self.t(), 3), phase=phase)
        self.event(kind='capture-at', frame=frame, count=count, step=step, got=len(caps), error=err)
        return caps, err

    def press(self, key, hold=150, wait=False):
        """Non-blocking by default (the game holds the key on its own thread)."""
        self.event(kind='press', key=key, hold=hold)
        reply = control.send(self.session, f'press {key} {hold}')
        if wait:
            return control.wait_reply(reply, hold / 1000 + 20)
        return reply

    def dump(self, reason=None):
        path = self.request('trace dump' + (f' {reason}' if reason else ''), 20)
        self.event(kind='dump', path=path)
        return path

    def mark(self, text):
        self.event(kind='mark', text=text)
        return self.request(f'trace mark {text}')

    def state(self):
        return json.loads(self.request('state'))

    def wait_for(self, key, op, value, timeout=60):
        return control.wait_for(self.session, key, op, value, timeout)

    # -- pad (replay-pad channel: buttons, LT, RT, LX, LY, RX, RY)
    def pad(self, buttons=0, lt=0, rt=0, lx=0, ly=0, rx=0, ry=0):
        atomic_write(self.pad_path, f'1 {clock_us() + 500000} {buttons} {lt} {rt} {lx} {ly} {rx} {ry}\n')

    def pad_release(self):
        atomic_write(self.pad_path, f'0 {clock_us()} 0 0 0 0 0 0 0\n')

    def pad_hold(self, ms, background=True, **fields):
        def go():
            self.event(kind='pad-hold-start', ms=ms, **fields)
            end = mono() + ms / 1000
            with self._pad_lock:
                while mono() < end:
                    self.pad(**fields)
                    time.sleep(.02)
                self.pad_release()
            self.event(kind='pad-hold-end')
        if background:
            threading.Thread(target=go, daemon=True).start()
        else:
            go()

    # -- per-thread CPU (libproc; no attach, does not distort timing)
    def cpu_sampler(self, period=1.0):
        def go():
            with open(self.out / 'cpu.jsonl', 'a') as f:
                while not self._stop.is_set():
                    try:
                        th = {str(h): v for h, v in threadcpu.snapshot(self.pid).items()}
                    except Exception as e:
                        th = {'error': str(e)}
                    f.write(json.dumps(dict(t=round(self.t(), 3), threads=th)) + '\n')
                    f.flush()
                    self._stop.wait(period)
        threading.Thread(target=go, daemon=True).start()

    def alive(self):
        """Process check (kill(pid, 0) + zombie check); a death before the harness's own close is recorded."""
        try:
            os.kill(self.pid, 0)
            ok = dev.pid_alive(self.pid)
        except OSError:
            ok = False
        if not ok and not self.closing and self.exited_early_t is None:
            self.exited_early_t = round(self.t(), 3)
            self.event(kind='exited-early')
        return ok

    def close(self):
        self.alive()  # last check before the planned close
        self.closing = True
        self._stop.set()
        try:
            result = control.close(self.session)
        except Exception as e:
            result = f'close error: {e}'
        for _ in range(80):
            if not dev.pid_alive(self.pid):
                break
            time.sleep(.25)
        # 1 = this run's own game exited (like `pgrep` finding nothing); other instances are not counted.
        rc = 0 if dev.pid_alive(self.pid) else 1
        self.event(kind='closed', result=result, own_pid_rc=rc, other_instances=[e['pid'] for e in instances.instances()])
        if rc == 1:  # the next run seeds from what this one published (agent shader cache)
            self.agent_cache_publish = dev.agent_cache.wait_published(self.session)
            if self.agent_cache_publish: self.event(kind='agent-cache', **self.agent_cache_publish)
        return rc


# ---------------------------------------------------------------- plans

SWEEP_PLANS = {
    # (seconds, buttons, RX, RY, name) — from flash-20261007/sweep.py
    'default': [(3, 0, 0, 0, 'idle'), (8, 0, 22000, 0, 'rot+'), (8, 0, -22000, 0, 'rot-'), (2, 0, 0, 20000, 'tiltU'),
                (2.5, 0, 0, -20000, 'tiltD'), (1.5, 256, 0, -25000, 'lbclose'), (7, 0, 24000, 0, 'rot+2'),
                (2, 0, 0, 22000, 'tiltU2'), (2, 0, 0, 0, 'idle2')],
    'fast': [(3, 0, 0, 0, 'idle'), (6, 0, 32000, 0, 'fast+'), (6, 0, -32000, 0, 'fast-'), (3, 0, 12000, 0, 'slow+'),
             (3, 0, -12000, 0, 'slow-'), (6, 0, 32000, 0, 'fast+2'), (2, 0, 0, 0, 'idle2')],
    # 30 s steady camera turn (overhead A/B): no captures during it unless asked
    'turn30': [(30, 0, 24000, 0, 'turn')],
    # Standard perf plan (2026-10-11): 8 fixed headings, a short turn then a 2.5 s still hold each (~26 s).
    # Steadier than a continuous spin and the same coverage; compare per-'hold' phases across arms.
    # 8 x 0.75 s holds (~11 s): over 86 recorded on/off pairs the A/B deltas match the 2.5 s plan within 0.15 pp
    # median (p90 0.43), under repeat noise (~0.9%); no settle discard is needed after a turn (docs/readiness.md).
    'headings8': [(0.75, 0, 0, 0, 'hold0')] + [p for i in range(1, 8) for p in
                                               ((0.7, 0, 24000, 0, f'turn{i}'), (0.75, 0, 0, 0, f'hold{i}'))],
    # The plan before 2026-10-13 (~24 s): rerun with it when a headings8 delta is within ~2%. Both arms of a
    # comparison must use the same plan.
    'headings8-long': [(1.5, 0, 0, 0, 'hold0')] + [p for i in range(1, 8) for p in
                                                   ((0.7, 0, 24000, 0, f'turn{i}'), (2.5, 0, 0, 0, f'hold{i}'))],
    # Standard correctness spin: one short turn (catches rotation-only bugs like the dog-fur flashes).
    'spin5': [(1, 0, 0, 0, 'idle'), (5, 0, 24000, 0, 'spin'), (1, 0, 0, 0, 'idle2')],
}
# Sweep plans that measure speed: these runs take the global perf lock (one at a time across checkouts).
PERF_PLANS = ('headings8', 'headings8-long', 'turn30')


def camera_sweep(run, plan='default', wait=None, capture=True, at_frame=None, step=60, count=None,
                 burst=None):
    """Stick-driven camera sweep with captures (sweep.py), started when the game is ready (Run.wait_ready).
    Returns frames (+ fps from the frame CSV), the plan and the readiness record.
    at_frame (frame-matched parity, e.g. spin5 with 'load+300'): the sweep starts as soon as the game shows the
    target frame (a capture burst there is the sync), and the captures are bursts of consecutive frames at
    target + k * step instead of back-to-back captures, so every arm captures the same guest frames; the stick
    input still runs on wall time, so arms can differ by a frame or two of camera turn (f2diag parity's shift)."""
    phases = SWEEP_PLANS[plan]
    ready = run.wait_ready(wait)
    frames, cur, stop = [], [0, 0, 0], threading.Event()
    fm, fm_thread = None, None
    if at_frame:
        burst = burst or DEFAULT_BURST
        total = sum(p[0] for p in phases)
        count = count or max(1, min(8, int(total * 60 / step) - 1))
        st = run.state()
        try:
            target, anchor = frame_target(st, at_frame)
        except ValueError as e:
            target, anchor = None, dict(spec=at_frame, error=str(e))
        fm = dict(target=target, count=count, step=step, burst=burst, frame_at_request=st.get('frame'),
                  load_end_frame=st.get('load_end_frame'), loading_clear_frame=st.get('loading_clear_frame'),
                  **anchor)
        errors = [anchor['error']] if 'error' in anchor else []
        if target is not None:
            got, err = run.capture_at(target - burst // 2, burst, 1, phase='sync')
            frames += got
            if err: errors.append(err)

            def burster():
                for k in range(1, count + 1):
                    if stop.is_set(): break
                    g, e = run.capture_at(target + k * step - burst // 2, burst, 1, phase='frame-match')
                    frames.extend(g)
                    if e: errors.append(e)
            fm_thread = threading.Thread(target=burster, daemon=True)
        fm['errors'] = errors
        capture = False

    def writer():
        while not stop.is_set():
            run.pad(buttons=cur[0], rx=cur[1], ry=cur[2])
            time.sleep(.03)
    w = threading.Thread(target=writer, daemon=True)
    w.start()
    t_start, wall_start = run.t(), time.time()
    if fm_thread: fm_thread.start()
    try:
        for secs, buttons, rx, ry, name in phases:
            cur[:] = [buttons, rx, ry]
            run.event(kind='phase', name=name)
            end = mono() + secs
            while mono() < end:
                if capture:
                    frames.append(run.capture(phase=name))
                else:
                    run.check()
                    time.sleep(0.05)
    finally:
        if fm_thread: fm_thread.join(timeout=30)
        stop.set()
        w.join()
        run.pad_release()
    res = dict(frames=frames, t_start=round(t_start, 3), t_end=round(run.t(), 3), wall_start=wall_start,
               wall_end=time.time(), plan=plan, ready=ready)
    if fm is not None:
        fm['captures'] = len(frames)
        fm['error'] = '; '.join(fm['errors']) or None
        res['frame_match'] = fm
    return res


def area_transition(run, press_at=None, hold_ms=4000, max_s=70.0, step=0.25):
    """Hold A (pad channel) through the Cullis Gate once the game is in play (loaded, world drawing, steady; the
    FABLE2_READY_TIMEOUT guard, 60 s after launch, presses anyway), or at the legacy fixed --press-at time;
    capture every step s from launch (area-load run.py)."""
    pressed, prev, caps = False, None, []
    end = run.spawned + max_s
    guard = float(os.environ.get('FABLE2_READY_TIMEOUT', 60))
    while mono() < end and run.alive():
        if not pressed:
            if press_at is not None:
                go, why = run.since_spawn() >= press_at, 'fixed press-at'
            else:
                try:
                    conds = control.ready_conditions(control.state(run.session))
                except (RuntimeError, TimeoutError, ValueError):
                    conds = {}
                go = all(conds.get(k) for k in ('loaded', 'world', 'steady')) or run.since_spawn() >= guard
                why = 'ready' if go and run.since_spawn() < guard else 'ready guard'
            if go:
                run.event(kind='press', why=why)
                run.pad_hold(hold_ms, buttons=4096)
                pressed = True
        t = mono()
        m = run.capture()
        if 'path' in m:
            try:
                m['mean'], m['diff'], prev = images.stats(m['path'], prev)
            except Exception as e:
                m['stat_error'] = str(e)
        caps.append(m)
        time.sleep(max(0.0, t + step - mono()))
    return caps


def move_check(run, wait=None):
    """movecheck.py: capture, walk forward 3 s + strafe 2 s, capture once pacing is steady again; True when the
    view changed."""
    ready = run.wait_ready(wait, need=('loaded', 'world', 'steady'))
    a = run.capture(phase='before')
    run.pad_hold(3000, background=False, ly=26000)
    run.pad_hold(2000, background=False, lx=26000)
    settled = control.wait_ready(run.session, need=('world', 'steady'), timeout=10, alive=run.alive)
    b = run.capture(phase='after')
    moved, frac = images.moved(a['path'], b['path'])
    return dict(before=a, after=b, moved=moved, changed_fraction=frac, ready=ready,
                settled_s=settled['ready_s'], settled_timed_out=settled['timed_out'])


def f5_save(run, wait=None, timeout=45.0):
    """f5save.py: quick save (F5) once the game is in play (loaded, world drawing, steady) and wait for the session
    watcher's 'saved' event."""
    run.ready = run.wait_ready(wait, need=('loaded', 'world', 'steady'))
    run.press('f5', 200, wait=True)
    deadline = time.time() + timeout
    events = []
    while time.time() < deadline:
        wb = run.session / 'writeback.jsonl'
        events = [json.loads(x) for x in wb.read_text().splitlines()] if wb.exists() else []
        if any(e['event'] == 'saved' for e in events):
            break
        run.check()
        time.sleep(0.5)
    saved = [e for e in events if e['event'] == 'saved']
    return dict(saved=saved, events=events)


FRAME_ANCHORS = {'load': 'load_end_frame', 'clear': 'loading_clear_frame'}


def frame_target(st, spec):
    """Absolute guest frame for a frame spec (docs/readiness.md, "Frame-matched parity"): 'N' (guest frame N since
    boot) or 'load+N' / 'clear+N' (N frames after the last level-load call returned / the loading tag cleared).
    st = a test-control `state` reply. Returns (frame, anchor record)."""
    spec = str(spec).strip()
    if '+' in spec:
        name, off = spec.split('+', 1)
        key = FRAME_ANCHORS.get(name)
        if key is None:
            raise ValueError('frame spec %r: anchor must be one of %s' % (spec, ', '.join(FRAME_ANCHORS)))
        base = st.get(key)
        if not base:
            raise ValueError('frame spec %r: the game reports no %s (old build or no level load yet)' % (spec, key))
        return int(base) + int(off), dict(spec=spec, anchor=name, anchor_frame=int(base), offset=int(off))
    return int(spec), dict(spec=spec, anchor='boot', anchor_frame=0, offset=int(spec))


DEFAULT_AT_FRAME = os.environ.get('FABLE2_PARITY_AT_FRAME', 'load+240')  # warm ready: load+97..154
# Consecutive frames around each target: at the same guest frame id (relative to the level-load end) two runs can
# still be a few game ticks apart, because the game advances by wall time and the hitches after a load differ
# (2026-10-14: Old Tomb A-A 0.14 at the same id, 0.03 at the best shift; Bower Lake 0.37 within +-2, 0.09 at -1 of a
# +-6 search, 3 of 12 runs off by 2), so `f2diag parity` matches each burst at its best shift within +-4.
DEFAULT_BURST = int(os.environ.get('FABLE2_PARITY_BURST', 9))


def frame_matched_captures(run, spec, count, step, burst=DEFAULT_BURST):
    """Captures at fixed guest frames for parity (f2diag parity): `count` bursts of `burst` consecutive frames centred
    on target, target+step, ...; the target comes from the frame spec, so every arm captures the same guest moments.
    Returns the frame_match record (captures, target, anchor, error)."""
    st = run.state()
    try:
        target, anchor = frame_target(st, spec)
    except ValueError as e:
        return dict(spec=spec, error=str(e), captures=[], frame_at_request=st.get('frame'))
    rec = dict(target=target, count=count, step=step, burst=burst, frame_at_request=st.get('frame'),
               load_end_frame=st.get('load_end_frame'), loading_clear_frame=st.get('loading_clear_frame'), **anchor)
    caps, errors = [], []
    for k in range(count):
        got, err = run.capture_at(target + k * step - burst // 2, burst, 1, phase='frame-match')
        caps += got
        if err:
            errors.append(err)
            if not got:
                break
    rec.update(captures=caps, error='; '.join(errors) or None,
               exact=bool(caps) and all(m.get('frame_id') == m.get('requested_frame') for m in caps))
    if errors:
        print('FRAME MATCH: %s (target %d, game was at %s): %s' % (spec, target, st.get('frame'), rec['error']),
              file=sys.stderr, flush=True)
    return rec


def still_capture(run, wait=None, at_frame=None, count=3, step=30):
    """Parity capture with a still camera: wait until the game is ready (Run.wait_ready), then until the picture
    itself is still, because `world`/`steady` can hold while the post-load fade-in is still brightening the screen.
    Still = FABLE2_STILL_FRAMES (3) captures of distinct frames in a row whose mean luma moved <= FABLE2_STILL_MEAN_TOL
    (0.5 of 255) and whose changed-cell fraction (images.stats) is <= FABLE2_STILL_CHANGED_TOL (0.02; idle animation
    and foliage stay under it, a fade moves every cell). No fixed wait: captures run back to back, and
    FABLE2_STILL_TIMEOUT (20 s) is only a guard, reported as still.timed_out. Then one guest and one screen capture."""
    ready = run.wait_ready(wait)
    if at_frame:
        # Frame-matched parity (the default since 2026-10-14): the same guest frames in every arm; no still-wait,
        # because both arms see the same moment of any fade or flicker.
        fm = frame_matched_captures(run, at_frame, count, step)
        if fm['captures']:
            return dict(ready=ready, frame_match=fm, guest=fm['captures'][min(len(fm['captures']) - 1, DEFAULT_BURST // 2)],
                        screen=run.capture(screen=True, phase='still'))
        print('FRAME MATCH: no frame-matched capture; falling back to the still-wait capture', file=sys.stderr,
              flush=True)
        res = still_capture(run, None, None)
        res['frame_match'] = fm
        return res
    need = int(os.environ.get('FABLE2_STILL_FRAMES', 3))
    mean_tol = float(os.environ.get('FABLE2_STILL_MEAN_TOL', 0.5))
    changed_tol = float(os.environ.get('FABLE2_STILL_CHANGED_TOL', 0.02))
    guard = float(os.environ.get('FABLE2_STILL_TIMEOUT', 20))
    started, prev, prev_mean, last_frame, streak, samples, timed_out = mono(), None, None, None, 0, [], False
    while streak < need:
        if mono() - started > guard:
            timed_out = True
            break
        m = run.capture(phase='still-wait')
        if 'path' not in m or (m.get('frame_id') is not None and m.get('frame_id') == last_frame):
            continue
        last_frame = m.get('frame_id')
        try:
            mean, diff, prev = images.stats(m['path'], prev)
        except Exception as e:
            run.event(kind='still-stat-error', error=str(e))
            continue
        still = prev_mean is not None and abs(mean - prev_mean) <= mean_tol and diff is not None and diff <= changed_tol
        streak = streak + 1 if still else 0
        samples.append(dict(t=m.get('req_t'), frame_id=last_frame, mean=mean, diff=diff))
        prev_mean = mean
    info = dict(still_s=round(mono() - started, 3), timed_out=timed_out, frames=need, mean_tol=mean_tol,
                changed_tol=changed_tol, samples=samples)
    run.event(kind='still', **{k: v for k, v in info.items() if k != 'samples'})
    if timed_out:
        print('STILL: guard fired after %.1f s (picture still changing); capturing anyway' % info['still_s'],
              file=sys.stderr, flush=True)
    return dict(ready=ready, still=info, guest=run.capture(phase='still'), screen=run.capture(screen=True, phase='still'))


def boot_capture(run, max_s=30.0, step=0.25):
    """boot.py: capture loop from launch (load-phase timing comes from the trace: f2diag phases)."""
    caps, prev = [], None
    while run.since_spawn() < max_s and run.alive():
        t = mono()
        m = run.capture()
        if 'path' in m:
            try:
                m['mean'], m['diff'], prev = images.stats(m['path'], prev)
            except Exception as e:
                m['stat_error'] = str(e)
        caps.append(m)
        time.sleep(max(0.0, t + step - mono()))
    return caps


FAULT_MARKERS = ('Unhandled guest access violation', '[FATAL]')


def log_faults(session):
    """Count guest access violations and fatal errors in a session's log (read after close, so the
    whole run is covered). Any one of them fails the run, whatever else it reached."""
    log = Path(session) / 'test-state/logs/fable_2.log'
    counts, first = {'av': 0, 'fatal': 0}, None
    skip_intro_missing = 0
    if log.exists():
        for line in log.read_text(errors='replace').splitlines():
            if '[trace]' in line or '[trace-selftest]' in line:
                continue  # recorder summaries and the synthetic self-test AV
            # Skip intro was requested (the line is only logged then) but its replacement movie
            # did not open, so the real intro played (bug-patterns row 53).
            if '[skip-intro]' in line and 'playing original' in line:
                skip_intro_missing += 1
                first = first or line[:240]
            for key, marker in zip(('av', 'fatal'), FAULT_MARKERS):
                if marker in line:
                    counts[key] += 1
                    first = first or line[:240]
    return dict(av_lines=counts['av'], fatal_lines=counts['fatal'], first_fault=first,
                skip_intro_missing=skip_intro_missing)


CRASH_REPORTS = Path.home() / 'Library/Logs/DiagnosticReports'


def crash_reports(pid, start_unix):
    """macOS crash reports (read only) for this game process: fable_2-*.ips newer than the session start
    whose pid matches. Returns [{path, exception, thread, frames}] with the faulting thread's top frames."""
    rows = []
    for ips in sorted(CRASH_REPORTS.glob('fable_2-*.ips')):
        try:
            if ips.stat().st_mtime < start_unix - 1:
                continue
            head, _, body = ips.read_text(errors='replace').partition('\n')
            d = json.loads(body)
        except (OSError, ValueError):
            continue
        if pid and d.get('pid') not in (None, pid):
            continue
        row = dict(path=str(ips), exception=d.get('exception'), indicator=d.get('termination', {}).get('indicator'))
        try:
            th = d['threads'][d['faultingThread']]
            imgs = d.get('usedImages', [])
            row['thread'] = th.get('name') or th.get('queue')
            row['frames'] = ['%s %s' % (imgs[f['imageIndex']].get('name', '?') if f.get('imageIndex') is not None
                                        and f['imageIndex'] < len(imgs) else '?', f.get('symbol', hex(f.get('imageOffset', 0))))
                             for f in th.get('frames', [])[:10]]
        except (KeyError, IndexError, TypeError):
            pass
        rows.append(row)
    return rows


def exit_check(session, pid=None, start_unix=None, asked_close=True, exited_early_t=None):
    """Did the game exit unexpectedly? Evidence: the process died before the harness's close, a crash report
    for this pid, or (after the harness asked to close) no clean-exit marker in the log.
    Returns dict(exit_reasons=[...], crash_reports=[...]) - any reason fails the run."""
    session = Path(session)
    if pid is None or start_unix is None:
        try:
            rec = json.loads((session / 'session.json').read_text())
            pid, start_unix = pid or rec.get('pid'), start_unix or rec.get('start_unix', 0)
        except (OSError, ValueError):
            start_unix = start_unix or 0
    reasons, reports = [], crash_reports(pid, start_unix or 0)
    how = dev.exit_kind(session / 'test-state')
    # A clean quit the harness did not ask for (window close button, Cmd+Q, Dock Quit) is the
    # maintainer closing the window by hand: not a crash or hang, reported as closed_by_user.
    # (A harness close shows as test-control-close, so closed-by-user always means someone else quit it.)
    closed_by_user = not reports and how['kind'] == 'closed-by-user'
    if exited_early_t is not None and not closed_by_user:
        reasons.append('exited_early at t=%ss (before the harness close)' % exited_early_t)
    if reports:
        r = reports[0]
        reasons.append('crashed: %s %s, thread %s, %s' % (Path(r['path']).name, r.get('indicator') or r.get('exception'),
                                                          r.get('thread'), ' <- '.join(r.get('frames', [])[4:8])))
    if asked_close and not closed_by_user:
        log = session / 'test-state/logs/fable_2.log'
        try:
            clean = dev.CLEAN_EXIT_MARKER in log.read_text(errors='replace')
        except OSError:
            clean = False
        if not clean:
            reasons.append('no clean-exit marker (%r) in the log' % dev.CLEAN_EXIT_MARKER)
    return dict(exit_reasons=reasons, crash_reports=reports, exit_how=how['kind'], exit_detail=how['detail'],
                closed_by_user=closed_by_user)


def verdict_of(res):
    if res['av_lines'] or res['fatal_lines'] or res['exit_reasons'] or res.get('skip_intro_missing'):
        return 'fail'
    return 'user-closed' if res.get('closed_by_user') else 'ok'


USER_CLOSED_TEXT = 'GAME CLOSED BY USER (window close / Cmd+Q / Quit) - run stopped early; not a crash or hang'


def boot_until_title(run, max_s=20.0, hang_dwell=4.5):
    """One boot: pass once the fast title fires ([title-fast] INTRO_FINISHED / [boot-save] title ready);
    'hang' when a hang dump is written (no guest swap for 3 s), then dwell so the dump lands; 'timeout'
    when neither happens by max_s. A manual `trace dump hang-manual` is taken on hang/timeout."""
    log = run.session / 'test-state/logs/fable_2.log'
    verdict, t_end, hang_at = 'timeout', None, None
    while run.since_spawn() < max_s and run.alive():
        text = log.read_text(errors='replace') if log.exists() else ''
        if '[title-fast] INTRO_FINISHED' in text or '[boot-save] t=' in text:
            verdict, t_end = 'pass', round(run.t(), 2)
            break
        if '-hang written' in text and hang_at is None:
            hang_at = run.t()
        if hang_at is not None and run.t() - hang_at >= hang_dwell:
            verdict, t_end = 'hang', round(run.t(), 2)
            break
        time.sleep(0.25)
    if verdict != 'pass' and run.alive():
        try:
            run.dump('hang-manual')
        except Exception as e:
            run.event(kind='dump-error', error=str(e))
    return dict(verdict=verdict, t=t_end, alive=run.alive())


def contention_summary(session):
    """Instances that ran beside this session (session.json at launch + the watcher's instances.jsonl)."""
    session = Path(session)
    seen = {}
    try:
        rec = json.loads((session / 'session.json').read_text())
        for e in rec.get('concurrent_instances') or []: seen[e['pid']] = e
    except (OSError, ValueError):
        pass
    try:
        for line in (session / 'instances.jsonl').read_text().splitlines():
            for e in json.loads(line).get('others', []): seen[e['pid']] = e
    except (OSError, ValueError):
        pass
    others = sorted(seen.values(), key=lambda e: e['pid'])
    return dict(contention=dict(count=len(others), others=[instances.describe(e) for e in others],
                                text=instances.contention_text(others) or None),
                perf_comparable=not others)


def boot_loop(label, n, save, env, game_args, cache_from, fps, max_s, build='latest', cache_snapshot=None):
    """N boots of one save with the same settings; results.jsonl gets one row per boot."""
    out = OUT / label
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for i in range(n):
        run = Run(f'{label}/{i:02d}', save, env, game_args, cache_from, fps, build, cache_snapshot)
        try:
            r = boot_until_title(run, max_s)
        except GameExited as e:
            r = dict(verdict='stopped', t=None, alive=False, stopped=str(e))
        finally:
            rc = run.close()
        faults = log_faults(run.session)
        faults.update(exit_check(run.session, run.pid, run.record.get('start_unix'), True, run.exited_early_t))
        if faults['av_lines'] or faults['fatal_lines'] or faults['exit_reasons'] or faults['skip_intro_missing']:
            r['verdict'] = 'fail'  # AV/FATAL in the log, or the process died / crashed / exited uncleanly
        elif faults.get('closed_by_user'):
            r['verdict'] = 'user-closed'
        row = dict(i=i, session=str(run.session), pgrep_after_close=rc, env=list(env), **r, **faults,
                   perf_lock_shared_wait_s=run.record.get('perf_lock_shared_wait_s'), queue_s=run.queue_s,
                   agent_cache=agent_cache_result(run), **contention_summary(run.session))
        rows.append(row)
        with open(out / 'results.jsonl', 'a') as f:
            f.write(json.dumps(row) + '\n')
        print(json.dumps(row), flush=True)
        if rc != 1:
            raise RuntimeError(f'game still running after close (pgrep rc {rc})')
        if r['verdict'] == 'user-closed':
            print(USER_CLOSED_TEXT + ' - stopping the boot loop', flush=True)
            break
    hangs = sum(r['verdict'] in ('hang', 'timeout') for r in rows)
    fails = sum(r['verdict'] == 'fail' for r in rows)
    user_closed = any(r['verdict'] == 'user-closed' for r in rows)
    return dict(boots=len(rows), requested=n, hangs=hangs, fails=fails, user_closed=user_closed, rows=rows)


def agent_cache_result(run):
    """result.json agent_cache: the seed (key, generation, content hash = the A/B cache signature; or the explicit
    --cache-from / live clone) and what the session published afterwards."""
    rec = dict(run.record.get('agent_cache') or dict(mode='cache-from', source=run.record.get('cache_from')))
    pub = getattr(run, 'agent_cache_publish', None)
    if pub: rec['published'] = {k: pub.get(k) for k in ('result', 'generation', 'reason', 'error') if pub.get(k) is not None}
    return rec


def thread_cpu_share(out_dir, name_prefix, t_from, t_to):
    """CPU % of one core used by threads whose name starts with name_prefix, between two cpu.jsonl samples."""
    rows = [json.loads(x) for x in (Path(out_dir) / 'cpu.jsonl').read_text().splitlines() if x.strip()]
    rows = [r for r in rows if 'error' not in r['threads'] and t_from <= r['t'] <= t_to]
    if len(rows) < 2:
        return None
    def total(r):
        return sum(v[1] for v in r['threads'].values() if v[0].startswith(name_prefix))
    a, b = rows[0], rows[-1]
    return round(100.0 * (total(b) - total(a)) / (b['t'] - a['t']), 3)


def process_cpu_share(out_dir, t_from, t_to):
    rows = [json.loads(x) for x in (Path(out_dir) / 'cpu.jsonl').read_text().splitlines() if x.strip()]
    rows = [r for r in rows if 'error' not in r['threads'] and t_from <= r['t'] <= t_to]
    if len(rows) < 2:
        return None
    a, b = rows[0], rows[-1]
    # Threads present in both samples (new threads count from 0).
    cpu = sum(v[1] - (a['threads'][h][1] if h in a['threads'] else 0.0) for h, v in b['threads'].items())
    return round(100.0 * cpu / (b['t'] - a['t']), 1)


def reclassify(argv):
    """f2diag run verdict <session dir> [--no-close-asked]: recompute the fault/exit verdict of a past session."""
    q = argparse.ArgumentParser(prog='f2diag run verdict')
    q.add_argument('session')
    q.add_argument('--no-close-asked', action='store_true', help='the harness did not close the game (--no-close)')
    b = q.parse_args(argv)
    res = log_faults(b.session)
    res.update(exit_check(b.session, asked_close=not b.no_close_asked))
    res.update(contention_summary(b.session))
    res['verdict'] = verdict_of(res)
    if res['closed_by_user']: print(USER_CLOSED_TEXT)
    print(json.dumps(res, indent=1))
    return 0 if res['verdict'] in ('ok', 'user-closed') else 1


def main(argv):
    if argv and argv[0] == 'verdict':
        return reclassify(argv[1:])
    p = argparse.ArgumentParser(prog='f2diag run', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('plan', choices=['sweep', 'area', 'move', 'still', 'f5', 'boot', 'bootloop'])
    p.add_argument('--n', type=int, default=10, help='bootloop: number of boots')
    p.add_argument('label')
    p.add_argument('--save', default=DEFAULT_SAVE)
    p.add_argument('--env', action='append', default=[])
    p.add_argument('--game-arg', action='append', default=[], dest='game_args')
    p.add_argument('--cache-from')
    p.add_argument('--cache-snapshot', help='agent shader cache: auto (default) | readonly | cold | off | KEY@GEN | PIN '
                   '(tools/fable2_agent_cache.py pin: the same frozen snapshot for every arm of an A/B batch)')
    p.add_argument('--fps', default='60', choices=['30', '60', 'uncapped'])
    p.add_argument('--sweep-plan', default='default', choices=sorted(SWEEP_PLANS))
    p.add_argument('--wait', type=float, default=None,
                   help='legacy: start sweep/move/f5 at this many seconds after launch instead of when the game '
                        'reports ready (docs/readiness.md; guard FABLE2_READY_TIMEOUT, 60 s)')
    p.add_argument('--press-at', type=float, default=None,
                   help='legacy: press at this time after launch instead of when the game is in play')
    p.add_argument('--at-frame', default=None,
                   help="frame-matched parity captures (docs/readiness.md): 'load+N' (default for still: %s), "
                        "'clear+N', or an absolute guest frame; 'off' = the old still-wait capture" % DEFAULT_AT_FRAME)
    p.add_argument('--frames', type=int, default=None, help='frame-matched captures per run (still: 3, sweep: 14)')
    p.add_argument('--frame-step', type=int, default=None, help='guest frames between them (still: 30, sweep: 30)')
    p.add_argument('--hold', type=int, default=4000)
    p.add_argument('--max', type=float, default=70.0)
    p.add_argument('--no-capture', action='store_true')
    p.add_argument('--cpu', action='store_true', help='per-thread CPU snapshots every --cpu-period s (libproc)')
    p.add_argument('--cpu-period', type=float, default=1.0, help='--cpu snapshot period in s (area loads: 0.25)')
    p.add_argument('--no-close', action='store_true')
    p.add_argument('--build', default='latest', choices=['latest', 'playtest'])
    p.add_argument('--perf-lock', action='store_true', help='take the global perf lock (auto for perf plans)')
    p.add_argument('--no-perf-lock', action='store_true', help='never take the global perf lock')
    a = p.parse_args(argv)
    perf = not a.no_perf_lock and (a.perf_lock or a.fps == 'uncapped' or
                                   (a.plan == 'sweep' and a.sweep_plan in PERF_PLANS))
    if a.no_perf_lock: os.environ['FABLE2_PERF_LOCK_SHARED'] = '0'  # nor the shared test-game hold
    with contextlib.ExitStack() as stack:
        wait = None
        if perf:
            wait = stack.enter_context(common.perf_lock('f2diag run %s %s' % (a.plan, a.label)))
            os.environ['FABLE2_PERF_LOCK_WAIT_S'] = str(wait)  # into session.json + agent-metrics game line
        return _main(a, wait)


def _main(a, perf_lock_wait):
    if a.plan == 'bootloop':  # correctness: never under the perf lock
        # Each boot closes as soon as the title is reached (the 2026-10-08 boot hang is pre-title).
        res = boot_loop(a.label, a.n, a.save, a.env, a.game_args, a.cache_from, a.fps, min(a.max, 30.0), a.build,
                        a.cache_snapshot)
        res['perf_comparable'] = all(r.get('perf_comparable', True) for r in res['rows'])
        for r in res['rows']:
            if r.get('contention', {}).get('text'): print(f"run {r['i']}: {r['contention']['text']}")
        (OUT / a.label / 'result.json').write_text(json.dumps(res, indent=1))
        print(json.dumps({k: v for k, v in res.items() if k != 'rows'}))
        return 0
    run = Run(a.label, a.save, a.env, a.game_args, a.cache_from, a.fps, a.build, a.cache_snapshot)
    print(run.session, flush=True)
    if a.cpu:
        run.cpu_sampler(a.cpu_period)
    result = {}
    try:
        if a.plan == 'sweep':
            at = None if a.at_frame in (None, 'off') else a.at_frame
            result = camera_sweep(run, a.sweep_plan, a.wait, capture=not a.no_capture, at_frame=at,
                                  step=a.frame_step or 60, count=a.frames)
            # Frame CSV on disk through the window: the game's flush acknowledgement (old builds: the 500 ms cadence).
            result['csv_flush'] = control.flush_frames(run.session)
            if result['csv_flush'] is None: time.sleep(0.6)
            result['pacing'] = pacing.window(run.session, result['wall_start'], result['wall_end'])
            if a.cpu:
                result['recorder_cpu_pct_of_core'] = thread_cpu_share(run.out, 'FlightRecorder', result['t_start'], result['t_end'])
                result['process_cpu_pct'] = process_cpu_share(run.out, result['t_start'], result['t_end'])
        elif a.plan == 'area':
            result = dict(captures=area_transition(run, a.press_at, a.hold, a.max))
        elif a.plan == 'move':
            result = move_check(run, a.wait)
        elif a.plan == 'still':
            at = DEFAULT_AT_FRAME if a.at_frame is None else (None if a.at_frame == 'off' else a.at_frame)
            result = still_capture(run, a.wait, at, a.frames or 3, a.frame_step or 30)
        elif a.plan == 'f5':
            result = f5_save(run, a.wait)
            result['ready'] = getattr(run, 'ready', None)
        elif a.plan == 'boot':
            result = dict(captures=boot_capture(run, a.max))
    except GameExited as e:
        result['stopped'] = str(e)
    finally:
        result['session'] = str(run.session)
        result['queue_s'] = run.queue_s  # launch wait before the game was spawned (not counted by --max)
        if perf_lock_wait is not None:
            result['perf_lock_wait_s'] = perf_lock_wait  # global perf lock wait (local agent docs)
        if run.record.get('perf_lock_shared_wait_s') is not None:  # correctness run: waited for a perf run
            result['perf_lock_shared_wait_s'] = run.record['perf_lock_shared_wait_s']
        if not a.no_close:
            result['pgrep_after_close'] = run.close()
        result.update(log_faults(run.session))
        result.update(exit_check(run.session, run.pid, run.record.get('start_unix'), not a.no_close, run.exited_early_t))
        result['verdict'] = verdict_of(result)
        result['agent_cache'] = agent_cache_result(run)
        result.update(contention_summary(run.session))
        if result['verdict'] == 'user-closed':
            print(USER_CLOSED_TEXT, flush=True)
        (run.out / 'result.json').write_text(json.dumps(result, indent=1))
        if result['contention']['text']:
            print(result['contention']['text'], flush=True)
    summary = {k: v for k, v in result.items() if k not in ('frames', 'captures', 'events')}
    print(json.dumps(summary, indent=1)[:2000])
