#!/usr/bin/env python3
"""Heavy-tool mode: the ONLY sanctioned way to run Instruments/xctrace, Metal GPU frame captures (.gputrace)
and gpudebug/GPUTools replays on this Mac (local agent docs). Agents ask main before any heavy run.

  fable2_heavy.py run --kind xctrace|gputrace|replay [options] -- <command...>
  fable2_heavy.py status [--json]          the running heavy run, its watchdog and the last samples
  fable2_heavy.py kill [--reason TEXT]     tear the running heavy tool tree down now
  fable2_heavy.py guard --pid PID [...]    start a standalone watchdog on an already running process tree
  fable2_heavy.py watchdog --run-dir DIR   (internal) the external watchdog

Why: 2026-10-10 a 5 s xctrace Metal System Trace grew xctrace to ~20 GB + 33 GB compressed and the Mac
kernel-panicked; 2026-10-11 8 GB .gputrace captures plus a 22-28 GB replay service filled 72 GB of swap while
the shader lab's own guard silently never started.

What `run` does:
  1. Takes heavy.lock (refuses if another heavy run holds it) and the perf lock EXCLUSIVE (waits for test games).
     Refuses if any fable_2 runs (registry and pgrep, the player's included), a build runs, or a GPUTools / xctrace /
     gpudebug process already exists. Builds wait while heavy.lock is held (fable2_common.build_marker).
  2. Preflight: free RAM (free+inactive+speculative+purgeable) >= 30 GB, swap used <= 4 GB, free disk >= 150 GB,
     and the per-kind command rules (see validate_*).
  3. Starts the external watchdog (detached, own session, inherits the lock fds) and refuses unless its heartbeat
     is fresher than 2 s. The watchdog samples every second and kills the tool tree on: free RAM < 12 GB, tree
     memory > 30 GB, swap growth > 2 GB, free disk < 150 GB, runtime over the per-kind limit, or the wrapper
     disappearing. It writes kill-report.json.
  4. Runs the tool in its own process group. {out} in the command is the run's artifact folder
     (out/heavy/<run id>/artifacts); {game_pid} is the 60 FPS test game the wrapper launched (xctrace record).
  5. Afterwards: closes its game, kills leftover tool processes (replay services), sizes the artifacts and deletes
     those over FABLE2_HEAVY_KEEP_MAX_GB (2) unless --keep all, and appends a line to
     <workspace>/agent-metrics/heavy-runs.jsonl.

Test-only environment (validation with fake tools; never for real runs): FABLE2_HEAVY_TEST_FAKE_TOOL=1 (skip the
per-kind command rules), FABLE2_HEAVY_TEST_IGNORE_GAMES=1 (with FAKE_TOOL only), FABLE2_HEAVY_TEST_WATCHDOG_FAIL=1
(watchdog exits before its first heartbeat; with FAKE_TOOL, <run dir>/limits-override.json lowers
thresholds mid-run), FABLE2_HEAVY_TEST_SERVICE_MARK=<text> (processes whose command line
holds it count as tool services), FABLE2_HEAVY_METRICS=<file>. Threshold overrides: see LIMITS.
Runs under /usr/bin/python3 (3.9).
"""
import argparse
import contextlib
import ctypes
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_common as common  # noqa: E402

ROOT = common.ROOT
GB = common.GB
HEAVY_DIR = ROOT / 'out/heavy'  # ignored (out/); the only place in the repo where heavy artifacts may go
LOCKS = common.LOCKS
WATCHDOG_PID = LOCKS / 'heavy-watchdog.pid'
CURRENT = LOCKS / 'heavy-run.json'  # points at the running run's folder (any checkout)
KINDS = ('xctrace', 'gputrace', 'replay')

# (env name, default). Preflight refusals and watchdog kill thresholds; env overrides exist for validation.
LIMITS = dict(
    min_free_ram_gb=('FABLE2_HEAVY_MIN_FREE_RAM_GB', 30.0),     # preflight
    max_swap_gb=('FABLE2_HEAVY_MAX_SWAP_GB', 4.0),              # preflight
    min_disk_gb=('FABLE2_HEAVY_MIN_DISK_GB', 150.0),            # preflight
    kill_free_ram_gb=('FABLE2_HEAVY_KILL_FREE_RAM_GB', 12.0),   # watchdog
    kill_tree_gb=('FABLE2_HEAVY_KILL_TREE_GB', 30.0),           # watchdog
    kill_swap_growth_gb=('FABLE2_HEAVY_KILL_SWAP_GROWTH_GB', 2.0),
    kill_disk_gb=('FABLE2_HEAVY_KILL_DISK_GB', 150.0),
    keep_max_gb=('FABLE2_HEAVY_KEEP_MAX_GB', 2.0),              # --keep auto deletes artifacts above this
)
MAX_RUNTIME_S = dict(xctrace=300.0, gputrace=600.0, replay=900.0, guard=1800.0)  # FABLE2_HEAVY_MAX_RUNTIME_S
HEARTBEAT_MAX_AGE_S = 2.0
TERM_GRACE_S = 3.0

# Processes that belong to these tools but are not children of the command (XPC/launchd services). Counted in the
# tree only when they appeared after the run started (baseline), and the run refuses if any exist beforehand.
SERVICE_NAMES = ('GPUToolsReplayService', 'GPUToolsDebugService', 'GPUToolsCaptureService', 'GPUToolsAgentService',
                 'gpudebug', 'xctrace', 'DTServiceHub')
BLOCKING_SERVICES = ('GPUToolsReplayService', 'GPUToolsDebugService', 'GPUToolsCaptureService', 'gpudebug',
                     'xctrace')

XCTRACE_TEMPLATES = ('Metal System Trace', 'Time Profiler', 'Game Performance', 'Metal GPU Counters')
XCTRACE_INSTRUMENTS = ('Metal Application', 'Metal GPU Counters', 'GPU', 'Time Profiler', 'os_signpost',
                       'Points of Interest', 'Thread State Trace', 'Virtual Memory Trace')
XCTRACE_MAX_TIME_LIMIT_S = 1.0


def limit(key):
    name, default = LIMITS[key]
    return common.env_float(name, default)


def max_runtime(kind):
    return common.env_float('FABLE2_HEAVY_MAX_RUNTIME_S', MAX_RUNTIME_S.get(kind, 600.0))


def log(text):
    print('HEAVY: ' + text, file=sys.stderr, flush=True)


def scrub(text):
    """No PII in logs that leave the run folder: the home folder becomes ~."""
    return str(text).replace(str(Path.home()), '~')


def write_json(path, data):
    common._atomic_json(Path(path), data)


def read_json(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------- machine samples

def vm_free_gb():
    """free + inactive + speculative + purgeable pages (vm_stat) in GB."""
    out = subprocess.run(['vm_stat'], capture_output=True, text=True).stdout
    m = re.search(r'page size of (\d+) bytes', out)
    page = int(m.group(1)) if m else 16384
    pages = 0
    for key in ('Pages free', 'Pages inactive', 'Pages speculative', 'Pages purgeable'):
        m = re.search(r'^%s:\s+(\d+)' % key, out, re.M)
        if m: pages += int(m.group(1))
    return pages * page / GB


def swap_used_gb():
    out = subprocess.run(['sysctl', '-n', 'vm.swapusage'], capture_output=True, text=True).stdout
    m = re.search(r'used = ([\d.]+)([KMG])', out)
    if not m: return 0.0
    return float(m.group(1)) * {'K': 1 / 1024 ** 2, 'M': 1 / 1024, 'G': 1.0}[m.group(2)]


def disk_free_gb():
    return common.free_gb()


class _RusageV2(ctypes.Structure):
    _fields_ = [('uuid', ctypes.c_uint8 * 16)] + [(n, ctypes.c_uint64) for n in (
        'user_time', 'system_time', 'pkg_idle_wkups', 'interrupt_wkups', 'pageins', 'wired_size', 'resident_size',
        'phys_footprint', 'proc_start_abstime', 'proc_exit_abstime', 'child_user_time', 'child_system_time',
        'child_pkg_idle_wkups', 'child_interrupt_wkups', 'child_pageins', 'child_elapsed_abstime',
        'diskio_bytesread', 'diskio_byteswritten')]


try:
    _libproc = ctypes.CDLL('/usr/lib/libproc.dylib')
except OSError:
    _libproc = None


def footprint_bytes(pid):
    """phys_footprint (includes compressed and swapped pages, unlike RSS), or None."""
    if _libproc is None: return None
    buf = _RusageV2()
    if _libproc.proc_pid_rusage(int(pid), 2, ctypes.byref(buf)) != 0: return None
    return int(buf.phys_footprint)


def process_table():
    """{pid: dict(ppid, pgid, rss_kb, args)} for every process."""
    out = subprocess.run(['ps', '-axww', '-o', 'pid=,ppid=,pgid=,rss=,args='], capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split(None, 4)
        if len(parts) < 4: continue
        try:
            pid, ppid, pgid, rss = (int(x) for x in parts[:4])
        except ValueError:
            continue
        table[pid] = dict(ppid=ppid, pgid=pgid, rss_kb=rss, args=parts[4] if len(parts) > 4 else '')
    return table


def _service_patterns():
    pats = [re.compile(r'(^|/)%s(\s|$)' % re.escape(n)) for n in SERVICE_NAMES]
    mark = os.environ.get('FABLE2_HEAVY_TEST_SERVICE_MARK')
    if mark: pats.append(re.compile(re.escape(mark)))
    return pats


def service_pids(table, names=None):
    """pids of GPUTools / xctrace / gpudebug / DTServiceHub processes (plus the test mark)."""
    if names is None: pats = _service_patterns()
    else: pats = [re.compile(r'(^|/)%s(\s|$)' % re.escape(n)) for n in names]
    me = os.getpid()
    return sorted(pid for pid, row in table.items() if pid != me and any(p.search(row['args']) for p in pats))


def run_games(run_id):
    """Registry entries of test games launched by this heavy run (settings.FABLE2_HEAVY_RUN_ID)."""
    out = []
    for path in (common.MAIN_ROOT / 'out/instances').glob('*.json'):
        entry = read_json(path)
        if entry and (entry.get('settings') or {}).get('FABLE2_HEAVY_RUN_ID') == run_id and common.pid_alive(entry['pid']):
            out.append(entry)
    return out


def tool_tree(run, table=None):
    """{pid: row} of everything this run owns: the tool's descendants and process group, services that appeared
    after the run started, and this run's own test games (with their descendants). Never the wrapper/watchdog,
    never a fable_2 that is not this run's."""
    table = table if table is not None else process_table()
    roots = set()
    tool = run.get('tool_pid')
    if tool and tool in table: roots.add(tool)
    pgid = run.get('tool_pgid')
    if pgid: roots.update(pid for pid, row in table.items() if row['pgid'] == pgid)
    baseline = set(run.get('baseline_services') or [])
    if run.get('include_all_services'): baseline = set()
    roots.update(pid for pid in service_pids(table) if pid not in baseline)
    games = set(int(g['pid']) for g in run_games(run['id']))
    games.update(int(p) for p in run.get('game_pids') or [] if int(p) in table)
    roots.update(p for p in games if p in table)
    children = {}
    for pid, row in table.items(): children.setdefault(row['ppid'], []).append(pid)
    tree, todo = set(), list(roots)
    while todo:
        pid = todo.pop()
        if pid in tree: continue
        tree.add(pid)
        todo.extend(children.get(pid, []))
    exclude = {os.getpid(), run.get('wrapper_pid'), run.get('watchdog_pid'), 1, 0}
    out = {}
    for pid in tree:
        if pid in exclude: continue
        row = table[pid]
        if re.search(r'(^|/)fable_2(\s|$)', row['args']) and pid not in games and pid != tool:
            continue  # someone else's game (never touch it)
        out[pid] = row
    return out


def tree_memory_gb(tree):
    total = 0
    for pid, row in tree.items():
        fp = footprint_bytes(pid)
        total += max(row['rss_kb'] * 1024, fp or 0)
    return total / GB


def sample(run, swap0):
    table = process_table()
    tree = tool_tree(run, table)
    swap = swap_used_gb()
    return dict(t=round(time.time(), 2), free_ram_gb=round(vm_free_gb(), 2), swap_gb=round(swap, 2),
                swap_growth_gb=round(swap - swap0, 2), disk_free_gb=round(disk_free_gb(), 1),
                tree_gb=round(tree_memory_gb(tree), 2), tree_pids=sorted(tree)), tree


def check_limits(run, s):
    lim = run['limits']
    if s['free_ram_gb'] < lim['kill_free_ram_gb']:
        return 'free RAM %.1f GB < %.1f GB' % (s['free_ram_gb'], lim['kill_free_ram_gb'])
    if s['tree_gb'] > lim['kill_tree_gb']:
        return 'tool tree memory %.1f GB > %.1f GB' % (s['tree_gb'], lim['kill_tree_gb'])
    if s['swap_growth_gb'] > lim['kill_swap_growth_gb']:
        return 'swap grew %.2f GB > %.2f GB' % (s['swap_growth_gb'], lim['kill_swap_growth_gb'])
    if s['disk_free_gb'] < lim['kill_disk_gb']:
        return 'free disk %.0f GB < %.0f GB' % (s['disk_free_gb'], lim['kill_disk_gb'])
    started = run.get('tool_start') or run.get('start')
    if started and time.time() - started > run['max_runtime_s']:
        return 'runtime %.0f s > %.0f s (%s limit)' % (time.time() - started, run['max_runtime_s'], run['kind'])
    return None


def teardown(run, reason, grace=TERM_GRACE_S):
    """SIGTERM the whole tool tree (and the tool's process group), SIGKILL survivors after `grace` s.
    Returns dict(killed=[...], survivors=[...])."""
    table = process_table()
    tree = tool_tree(run, table)
    victims = {pid: dict(args=scrub(row['args'])[:200], mem_gb=round(max(row['rss_kb'] * 1024,
                         footprint_bytes(pid) or 0) / GB, 2)) for pid, row in tree.items()}
    pgid = run.get('tool_pgid')
    if pgid:
        with contextlib.suppress(OSError): os.killpg(pgid, signal.SIGTERM)
    for pid in tree:
        with contextlib.suppress(OSError): os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if not any(common.pid_alive(p) for p in tree): break
        time.sleep(0.2)
    # Recompute: the tool may have spawned more while dying.
    table = process_table()
    tree2 = tool_tree(run, table)
    stubborn = set(p for p in tree if common.pid_alive(p)) | set(tree2)
    if pgid:
        with contextlib.suppress(OSError): os.killpg(pgid, signal.SIGKILL)
    for pid in stubborn:
        victims.setdefault(pid, dict(args=scrub(table.get(pid, {}).get('args', ''))[:200]))
        with contextlib.suppress(OSError): os.kill(pid, signal.SIGKILL)
    time.sleep(0.3)
    survivors = [p for p in victims if common.pid_alive(p)]
    return dict(reason=reason, killed=[dict(pid=p, **v) for p, v in sorted(victims.items())],
                sigkilled=sorted(stubborn), survivors=survivors)


# ---------------------------------------------------------------- watchdog (separate detached process)

def watchdog_main(run_dir, lock_fds=()):
    run_dir = Path(run_dir)
    if os.environ.get('FABLE2_HEAVY_TEST_WATCHDOG_FAIL') == '1':
        return 3  # simulated failure: no heartbeat ever
    run = read_json(run_dir / 'run.json')
    if not run: return 2
    LOCKS.mkdir(parents=True, exist_ok=True)
    WATCHDOG_PID.write_text('%d\n' % os.getpid())
    hb_path, samples_path = run_dir / 'heartbeat.json', run_dir / 'samples.jsonl'
    swap0 = run.get('swap0_gb', swap_used_gb())
    peaks = dict(tree_gb=0.0, swap_growth_gb=0.0, min_free_ram_gb=1e9, min_disk_free_gb=1e9)
    life_limit = run['max_runtime_s'] + 180  # the watchdog never outlives a stuck run by much
    outcome = None
    try:
        with open(samples_path, 'a') as samples:
            while True:
                tick = time.monotonic()
                tool = read_json(run_dir / 'tool.json') or {}
                run.update(tool)
                game = read_json(run_dir / 'game.json') or {}
                if game.get('pid'): run['game_pids'] = [game['pid']]
                if run.get('test'):  # validation only: lower a threshold mid-run (limits-override.json)
                    run['limits'].update(read_json(run_dir / 'limits-override.json') or {})
                s, tree = sample(run, swap0)
                peaks['tree_gb'] = max(peaks['tree_gb'], s['tree_gb'])
                peaks['swap_growth_gb'] = max(peaks['swap_growth_gb'], s['swap_growth_gb'])
                peaks['min_free_ram_gb'] = min(peaks['min_free_ram_gb'], s['free_ram_gb'])
                peaks['min_disk_free_gb'] = min(peaks['min_disk_free_gb'], s['disk_free_gb'])
                write_json(hb_path, dict(s, pid=os.getpid(), peaks=peaks))
                samples.write(json.dumps(s) + '\n')
                samples.flush()
                if (run_dir / 'stop').exists():
                    outcome = 'stopped'
                    break
                reason = check_limits(run, s)
                if not reason and (run_dir / 'kill-request').exists():
                    reason = 'kill requested: ' + ((run_dir / 'kill-request').read_text().strip() or 'manual')
                wrapper = run.get('wrapper_pid')
                if not reason and wrapper and not common.pid_alive(wrapper):
                    reason = 'wrapper pid %d disappeared' % wrapper
                if not reason and run.get('guard') and run.get('tool_pid') and not common.pid_alive(run['tool_pid']) \
                        and not tree:
                    outcome = 'guarded process exited'
                    break
                if not reason and time.time() - run['start'] > life_limit:
                    reason = 'watchdog lifetime %.0f s exceeded' % life_limit
                if reason:
                    # First the reason (the wrapper reads it as soon as its tool dies), then the full report.
                    write_json(run_dir / 'kill-report.json', dict(reason=reason, sample=s, t=time.time(), partial=True))
                    report = teardown(run, reason)
                    report.update(sample=s, peaks=peaks, t=time.time(), run_id=run['id'], kind=run['kind'])
                    write_json(run_dir / 'kill-report.json', report)
                    outcome = 'killed: ' + reason
                    wrapper_alive = wrapper and common.pid_alive(wrapper)
                    if not wrapper_alive and not run.get('guard'):
                        finalize(run_dir, run, outcome, peaks)  # the wrapper can't any more
                    if run.get('guard'): finalize(run_dir, run, outcome, peaks)
                    break
                time.sleep(max(0.05, 1.0 - (time.monotonic() - tick)))
    finally:
        write_json(run_dir / 'watchdog.json', dict(pid=os.getpid(), outcome=outcome, peaks=peaks, end=time.time()))
        with contextlib.suppress(OSError):
            if WATCHDOG_PID.read_text().strip() == str(os.getpid()): WATCHDOG_PID.unlink()
        wrapper = run.get('wrapper_pid')
        if lock_fds and wrapper and not common.pid_alive(wrapper):
            for name in ('heavy', 'perf'):  # the dead wrapper's holder records
                info = read_json(LOCKS / (name + '.holder.json'))
                if info and info.get('pid') == wrapper:
                    with contextlib.suppress(OSError): (LOCKS / (name + '.holder.json')).unlink()
            cur = read_json(CURRENT)
            if cur and cur.get('run_dir') == str(run_dir):
                with contextlib.suppress(OSError): CURRENT.unlink()
        for fd in lock_fds:
            with contextlib.suppress(OSError): os.close(fd)  # last holder: releases heavy/perf locks
    return 0


def spawn_watchdog(run_dir, lock_fds):
    log_file = open(run_dir / 'watchdog.log', 'a')
    cmd = [sys.executable, str(Path(__file__).resolve()), 'watchdog', '--run-dir', str(run_dir)]
    for fd in lock_fds: cmd += ['--lock-fd', str(fd)]
    proc = subprocess.Popen(cmd, pass_fds=tuple(lock_fds), stdin=subprocess.DEVNULL, stdout=log_file,
                            stderr=subprocess.STDOUT, start_new_session=True, close_fds=True)
    log_file.close()
    return proc


def wait_heartbeat(run_dir, proc, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        hb = read_json(run_dir / 'heartbeat.json')
        if hb and hb.get('pid') == proc.pid and time.time() - hb['t'] < HEARTBEAT_MAX_AGE_S:
            return hb
        if proc.poll() is not None: return None
        time.sleep(0.1)
    return None


def heartbeat_age(run_dir):
    hb = read_json(run_dir / 'heartbeat.json')
    return time.time() - hb['t'] if hb else 1e9


# ---------------------------------------------------------------- command rules

class Refused(RuntimeError):
    pass


def _opt_values(cmd, *names):
    """Values of --name V / --name=V in argv."""
    out = []
    for i, a in enumerate(cmd):
        for n in names:
            if a == n and i + 1 < len(cmd): out.append(cmd[i + 1])
            elif a.startswith(n + '='): out.append(a[len(n) + 1:])
    return out


def parse_duration_s(text):
    m = re.fullmatch(r'\s*([\d.]+)\s*(ms|s|m|h|min)?\s*', text or '')
    if not m: raise Refused('cannot parse --time-limit %r' % text)
    return float(m.group(1)) * {'ms': 0.001, 's': 1, None: 1, 'm': 60, 'min': 60, 'h': 3600}[m.group(2)]


def _under_out(value):
    return value.startswith('{out}') or Path(value).resolve().as_posix().startswith(HEAVY_DIR.resolve().as_posix() + '/')


def validate_xctrace(cmd):
    """Returns 'record' (needs a game) or 'other'."""
    i = 0
    if Path(cmd[0]).name == 'xcrun': i = 1
    if len(cmd) <= i + 1 or Path(cmd[i]).name != 'xctrace':
        raise Refused('--kind xctrace runs only `xctrace record|export|list ...` (or `xcrun xctrace ...`)')
    sub, rest = cmd[i + 1], cmd[i + 2:]
    if sub == 'record':
        for bad in ('--launch', '--all-processes', '--device', '--append-run', '--window'):
            if any(a == bad or a.startswith(bad + '=') for a in rest):
                raise Refused('xctrace record: %s is not allowed (attach to {game_pid} only)' % bad)
        templates, instruments = _opt_values(rest, '--template'), _opt_values(rest, '--instrument')
        if not templates and not instruments: raise Refused('xctrace record needs --template or --instrument')
        for t in templates:
            if t not in XCTRACE_TEMPLATES:
                raise Refused('template %r not in the allowlist %s' % (t, XCTRACE_TEMPLATES))
        for t in instruments:
            if t not in XCTRACE_INSTRUMENTS:
                raise Refused('instrument %r not in the allowlist %s' % (t, XCTRACE_INSTRUMENTS))
        limits = _opt_values(rest, '--time-limit')
        if not limits: raise Refused('xctrace record needs --time-limit (at most 1s)')
        if any(parse_duration_s(v) > XCTRACE_MAX_TIME_LIMIT_S for v in limits):
            raise Refused('--time-limit must be <= 1s (2026-10-10: 5 s made a 2.4 GB trace and a kernel panic)')
        if _opt_values(rest, '--attach') != ['{game_pid}']:
            raise Refused('xctrace record must use --attach {game_pid} (the 60 FPS test game this wrapper launches)')
        outs = _opt_values(rest, '--output')
        if len(outs) != 1 or not _under_out(outs[0]):
            raise Refused('xctrace record needs --output {out}/<name>.trace')
        return 'record'
    if sub == 'export':
        if not _opt_values(rest, '--xpath') and '--toc' not in rest:
            raise Refused('xctrace export must be targeted: --xpath <query> (or --toc); never the whole trace')
        outs = _opt_values(rest, '--output')
        if outs and not _under_out(outs[0]): raise Refused('xctrace export --output must be under {out}')
        return 'other'
    if sub in ('list', 'version', 'help', '--help'): return 'other'
    raise Refused('xctrace %s is not allowed (record, export, list)' % sub)


def _env_args(cmd):
    env = {}
    for v in _opt_values(cmd, '--env'):
        k, _, val = v.partition('=')
        env[k] = val
    return env


def validate_gputrace(cmd, allow_scale):
    if not any(Path(a).name == 'f2diag.py' or a.endswith('tools/f2diag') for a in cmd[:3]) or 'run' not in cmd:
        raise Refused('--kind gputrace runs only `python3 tools/f2diag.py run ... --env MTL_CAPTURE_ENABLED=1 '
                      '--env FABLE2_GPU_CAPTURE_PATH={out}/x.gputrace --env FABLE2_GPU_CAPTURE_AT_S=N`')
    env = _env_args(cmd)
    if env.get('MTL_CAPTURE_ENABLED') != '1': raise Refused('gputrace: needs --env MTL_CAPTURE_ENABLED=1')
    path = env.get('FABLE2_GPU_CAPTURE_PATH', '')
    if not path or not _under_out(path): raise Refused('gputrace: FABLE2_GPU_CAPTURE_PATH must be {out}/<name>.gputrace')
    if 'FABLE2_GPU_CAPTURE_AT_S' not in env: raise Refused('gputrace: needs --env FABLE2_GPU_CAPTURE_AT_S=<seconds>')
    try:
        frames = int(env.get('FABLE2_GPU_CAPTURE_FRAMES', '1'))
    except ValueError:
        frames = 99
    if frames != 1: raise Refused('gputrace: one frame only (FABLE2_GPU_CAPTURE_FRAMES=1)')
    try:
        scale = float(env.get('FABLE2_RES_SCALE', '1'))
    except ValueError:
        scale = 99
    if scale > 1 and not allow_scale:
        raise Refused('gputrace: 1x capture preferred (a 3x frame is 8 GB and its replay 22-28 GB); '
                      'pass --allow-scale after main confirms')
    fps = _opt_values(cmd, '--fps')
    if fps and fps[-1] == 'uncapped': raise Refused('gputrace: --fps uncapped is not allowed (use 60)')
    plans = _opt_values(cmd, '--sweep-plan')
    if any(p in ('headings8', 'turn30') for p in plans) or '--perf-lock' in cmd:
        raise Refused('gputrace: perf plans would wait for the perf lock this wrapper already holds')
    if '--no-close' in cmd: raise Refused('gputrace: --no-close is not allowed (the game must close)')


def validate_replay(cmd):
    exe = Path(cmd[0]).name
    if exe == 'gpudebug' or (exe in ('zsh', 'bash', 'sh') and len(cmd) > 1 and cmd[1].endswith('shaderlab/abrun.sh')) \
            or cmd[0].endswith('shaderlab/abrun.sh'):
        return
    raise Refused('--kind replay runs only `gpudebug ...` or tools/shaderlab/abrun.sh')


# ---------------------------------------------------------------- run

def exclusivity_problems(ignore_games=False):
    import fable2_instances as instances
    problems = []
    if not ignore_games:
        games = instances.instances()
        pids = set(instances.game_pids())
        listed = set(int(e['pid']) for e in games)
        for e in games: problems.append('a game is running: ' + instances.describe(e))
        for p in sorted(pids - listed): problems.append('an unregistered fable_2 is running (pid %d)' % p)
    for b in common.running_builds():
        if b.get('pid') != os.getpid(): problems.append('a build is running (%s, pid %s)' % (b.get('checkout'), b.get('pid')))
    table = process_table()
    for pid in service_pids(table, BLOCKING_SERVICES):
        problems.append('a GPU/Instruments tool is already running: pid %d %s' % (pid, scrub(table[pid]['args'])[:120]))
    return problems


def preflight_problems():
    problems, free, swap, disk = [], vm_free_gb(), swap_used_gb(), disk_free_gb()
    if free < limit('min_free_ram_gb'):
        problems.append('free RAM %.1f GB < %.0f GB (free+inactive+speculative+purgeable)' % (free, limit('min_free_ram_gb')))
    if swap > limit('max_swap_gb'):
        problems.append('swap used %.1f GB > %.0f GB (a reboot clears stale swap)' % (swap, limit('max_swap_gb')))
    if disk < limit('min_disk_gb'):
        problems.append('free disk %.0f GB < %.0f GB' % (disk, limit('min_disk_gb')))
    return problems, dict(free_ram_gb=round(free, 1), swap_gb=round(swap, 1), disk_free_gb=round(disk, 1))


def dir_size(path):
    path = Path(path)
    if path.is_file() or path.is_symlink(): return path.lstat().st_size
    total = 0
    for dirpath, _, files in os.walk(str(path)):
        for f in files:
            with contextlib.suppress(OSError): total += os.lstat(os.path.join(dirpath, f)).st_size
    return total


def handle_artifacts(run_dir, keep):
    art = Path(run_dir) / 'artifacts'
    rows = []
    if not art.is_dir(): return rows
    for item in sorted(art.iterdir()):
        size = dir_size(item) / GB
        delete = keep == 'none' or (keep == 'auto' and size > limit('keep_max_gb'))
        if delete:
            if item.is_dir() and not item.is_symlink(): shutil.rmtree(item, ignore_errors=True)
            else: item.unlink()
        rows.append(dict(name=item.name, gb=round(size, 3), deleted=delete))
        log('artifact %s: %.2f GB%s' % (item.name, size, ' (deleted, --keep %s)' % keep if delete else ''))
    return rows


def metrics_path():
    env = os.environ.get('FABLE2_HEAVY_METRICS')
    if env: return Path(env)
    return common.AGENT_METRICS / 'heavy-runs.jsonl' if common.AGENT_METRICS.is_dir() else None


def record_metrics(row):
    path = metrics_path()
    if not path: return
    with contextlib.suppress(OSError):
        with open(path, 'a') as f:
            f.write(json.dumps(row) + '\n')


def finalize(run_dir, run, outcome, peaks=None):
    """Artifacts + metrics; once per run (whoever gets there first: wrapper or watchdog)."""
    run_dir = Path(run_dir)
    try:
        fd = os.open(str(run_dir / 'finalized'), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        os.close(fd)
    except FileExistsError:
        return read_json(run_dir / 'result.json')
    if peaks is None:
        peaks = (read_json(run_dir / 'heartbeat.json') or {}).get('peaks') or {}
    artifacts = handle_artifacts(run_dir, run.get('keep', 'auto'))
    start = run.get('tool_start') or run.get('start')
    row = dict(t=time.strftime('%Y-%m-%dT%H:%M:%S'), id=run['id'], kind=run['kind'], checkout=run.get('checkout'),
               test=bool(run.get('test')), command=scrub(' '.join(shlex.quote(a) for a in run.get('command', [])))[:400],
               duration_s=round(time.time() - start, 1) if start else None, outcome=outcome,
               peak_tree_gb=peaks.get('tree_gb'), peak_swap_growth_gb=peaks.get('swap_growth_gb'),
               min_free_ram_gb=peaks.get('min_free_ram_gb'), preflight=run.get('preflight'),
               artifacts=artifacts, kill_report=(read_json(run_dir / 'kill-report.json') or {}).get('reason'))
    write_json(run_dir / 'result.json', row)
    record_metrics(row)
    with contextlib.suppress(OSError):
        cur = read_json(CURRENT)
        if cur and cur.get('run_dir') == str(run_dir): CURRENT.unlink()
    return row


def refuse(kind, command, reasons, test, extra=None):
    for r in reasons: log('REFUSED: ' + r)
    record_metrics(dict(t=time.strftime('%Y-%m-%dT%H:%M:%S'), kind=kind, checkout=common.SLOT or 'main', test=test,
                        command=scrub(' '.join(shlex.quote(a) for a in command))[:400], outcome='refused',
                        reasons=[scrub(r) for r in reasons], preflight=extra))
    return 2


@contextlib.contextmanager
def heavy_lock(what):
    try:
        with common.global_lock('heavy', what, timeout=0, poll=0.1, quiet=True) as wait:
            yield wait
    except common.LockTimeout as e:
        raise Refused('another heavy run holds heavy.lock: %s' % e)


def launch_game(run, save, wait_s):
    env = dict(os.environ, FABLE2_FPS_60='1', REX_VSYNC='1', FABLE2_HEAVY_RUN_ID=run['id'])
    cmd = [sys.executable, str(ROOT / 'tools/fable2_test_control.py'), 'launch']
    if save: cmd += ['--save', save]
    result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=180)
    if result.returncode != 0:
        raise Refused('test game launch failed: ' + (result.stderr.strip() or result.stdout.strip())[-400:])
    session = Path(json.loads(result.stdout)['result'])
    pid = int(json.loads((session / 'session.json').read_text())['pid'])
    entry = read_json(common.MAIN_ROOT / 'out/instances' / ('%d.json' % pid)) or {}
    settings = entry.get('settings') or {}
    if settings.get('FABLE2_FPS_60') != '1' or settings.get('REX_VSYNC') != '1':
        close_game(session)
        raise Refused('the test game is not capped at 60 FPS (settings %s)' % settings)
    return session, pid


def close_game(session):
    subprocess.run([sys.executable, str(ROOT / 'tools/fable2_test_control.py'), 'close', str(session)],
                   capture_output=True, text=True, timeout=60)


def cmd_run(a):
    command = list(a.command)
    if command and command[0] == '--': command = command[1:]
    if not command: sys.exit('fable2_heavy.py run: give the tool command after --')
    test = os.environ.get('FABLE2_HEAVY_TEST_FAKE_TOOL') == '1'
    ignore_games = test and os.environ.get('FABLE2_HEAVY_TEST_IGNORE_GAMES') == '1'
    needs_game = False
    try:
        if not test:
            if a.kind == 'xctrace': needs_game = validate_xctrace(command) == 'record'
            elif a.kind == 'gputrace': validate_gputrace(command, a.allow_scale)
            else: validate_replay(command)
        elif any('{game_pid}' in c for c in command):
            needs_game = True
    except Refused as e:
        return refuse(a.kind, command, [str(e)], test)
    run_id = time.strftime('%Y%m%d-%H%M%S') + '-' + a.kind + '-' + uuid.uuid4().hex[:6]
    run_dir = HEAVY_DIR / run_id
    art = run_dir / 'artifacts'
    what = 'heavy %s run %s' % (a.kind, run_id)
    try:
        with heavy_lock(what), contextlib.ExitStack() as stack:
            # Builds/agents see heavy.lock now; wait for running test games through the perf lock.
            os.environ.setdefault('FABLE2_PERF_LOCK_TIMEOUT', str(common.env_float('FABLE2_HEAVY_PERF_WAIT', 600)))
            try:
                stack.enter_context(common.perf_lock(what))
            except common.LockTimeout as e:
                return refuse(a.kind, command, ['perf lock: %s' % e], test)
            problems = exclusivity_problems(ignore_games)
            pre_problems, pre = preflight_problems()
            if problems or pre_problems:
                return refuse(a.kind, command, problems + pre_problems, test, pre)
            art.mkdir(parents=True)
            baseline = service_pids(process_table())
            run = dict(id=run_id, kind=a.kind, command=command, start=time.time(), wrapper_pid=os.getpid(),
                       checkout=common.SLOT or 'main', root=str(ROOT), keep=a.keep, test=test, preflight=pre,
                       swap0_gb=swap_used_gb(), baseline_services=baseline, max_runtime_s=max_runtime(a.kind),
                       limits={k: limit(k) for k in LIMITS})
            write_json(run_dir / 'run.json', run)
            write_json(CURRENT, dict(run_dir=str(run_dir), id=run_id, kind=a.kind, wrapper_pid=os.getpid()))
            fds = [common.HELD_FDS['heavy'], common.HELD_FDS['perf']]
            wd = spawn_watchdog(run_dir, fds)
            hb = wait_heartbeat(run_dir, wd)
            if not hb:
                with contextlib.suppress(OSError): wd.kill()
                with contextlib.suppress(OSError): CURRENT.unlink()
                return refuse(a.kind, command, ['the watchdog did not start (no heartbeat fresher than %.0f s); '
                                                'not starting the tool' % HEARTBEAT_MAX_AGE_S], test, pre)
            run['watchdog_pid'] = wd.pid
            write_json(run_dir / 'run.json', run)
            log('run %s: watchdog pid %d up (free RAM %.1f GB, swap %.1f GB, disk %.0f GB); folder %s'
                % (run_id, wd.pid, hb['free_ram_gb'], hb['swap_gb'], hb['disk_free_gb'], scrub(run_dir)))
            return _run_tool(a, run, run_dir, art, command, needs_game, wd)
    except Refused as e:
        return refuse(a.kind, command, [str(e)], test)


def _run_tool(a, run, run_dir, art, command, needs_game, wd):
    session = None
    outcome = None
    tool = None
    try:
        if needs_game:
            session, game_pid = launch_game(run, a.save, a.game_wait)
            write_json(run_dir / 'game.json', dict(pid=game_pid, session=str(session)))
            log('test game pid %d (60 FPS) session %s; waiting %.0f s' % (game_pid, session.name, a.game_wait))
            end = time.monotonic() + a.game_wait
            while time.monotonic() < end:
                if heartbeat_age(run_dir) > 3 or not common.pid_alive(game_pid): break
                if (run_dir / 'kill-report.json').exists(): break
                time.sleep(0.5)
            kill = read_json(run_dir / 'kill-report.json')
            if kill: raise Refused('the watchdog tripped before the tool started: ' + kill.get('reason', '?'))
            if not common.pid_alive(game_pid): raise Refused('the test game exited before the tool started')
            run['game_pids'] = [game_pid]
        else:
            game_pid = None
        subst = dict(out=str(art), game_pid=str(game_pid or ''), run_dir=str(run_dir))
        argv = [c.replace('{out}', subst['out']).replace('{game_pid}', subst['game_pid'])
                .replace('{run_dir}', subst['run_dir']) for c in command]
        if a.kind == 'gputrace' and not run.get('test'):
            argv += ['--env', 'FABLE2_HEAVY_RUN_ID=' + run['id']]  # registry tags the game as this run's
        env = dict(os.environ, FABLE2_HEAVY_RUN_ID=run['id'], FABLE2_HEAVY_OUT=str(art))
        if heartbeat_age(run_dir) > HEARTBEAT_MAX_AGE_S + 1 or wd.poll() is not None:
            raise Refused('the watchdog is not running (heartbeat stale) before the tool started')
        if (run_dir / 'kill-report.json').exists():
            raise Refused('the watchdog tripped before the tool started: '
                          + (read_json(run_dir / 'kill-report.json') or {}).get('reason', '?'))
        tool = subprocess.Popen(argv, env=env, start_new_session=True, cwd=str(ROOT))
        run.update(tool_pid=tool.pid, tool_pgid=tool.pid, tool_start=time.time())
        write_json(run_dir / 'tool.json', dict(tool_pid=tool.pid, tool_pgid=tool.pid, tool_start=run['tool_start']))
        log('tool pid %d: %s' % (tool.pid, scrub(' '.join(shlex.quote(x) for x in argv))[:300]))
        while tool.poll() is None:
            tripped = (run_dir / 'kill-report.json').exists()
            if heartbeat_age(run_dir) > 3.0 or (wd.poll() is not None and not tripped):
                # The watchdog is gone (or its teardown missed something): the wrapper tears down itself.
                reason = 'watchdog heartbeat lost' if not tripped else 'tool survived the watchdog teardown'
                log(reason + ': tearing the tool down from the wrapper')
                report = teardown(run, reason)
                if not tripped: write_json(run_dir / 'kill-report.json', report)
                break
            time.sleep(0.5)
        rc = tool.wait()
        if rc < 0:  # killed by a signal: give the watchdog a moment to name the reason
            for _ in range(10):
                if (run_dir / 'kill-report.json').exists(): break
                time.sleep(0.2)
        kill = read_json(run_dir / 'kill-report.json')
        outcome = ('killed: ' + kill['reason']) if kill else ('ok' if rc == 0 else 'tool exit %d' % rc)
    except Refused as e:
        outcome = 'refused: ' + str(e)
        log('REFUSED: ' + str(e))
    except KeyboardInterrupt:
        outcome = 'interrupted'
    finally:
        if session: close_game(session)
        leftovers = tool_tree(run)
        if leftovers:
            rep = teardown(run, 'end-of-run cleanup')
            write_json(run_dir / 'cleanup.json', rep)
            log('cleaned up %d leftover tool process(es)' % len(rep['killed']))
        (run_dir / 'stop').touch()
        try:
            wd.wait(timeout=5)
        except subprocess.TimeoutExpired:
            wd.kill()
        result = finalize(run_dir, run, outcome or 'error')
    log('done: %s; peak tree %.2f GB, peak swap growth %.2f GB, min free RAM %.1f GB'
        % (result['outcome'], result.get('peak_tree_gb') or 0, result.get('peak_swap_growth_gb') or 0,
           result.get('min_free_ram_gb') or 0))
    print(json.dumps(result, indent=1))
    return 0 if outcome == 'ok' else 1


# ---------------------------------------------------------------- status / kill / guard

def current_run():
    cur = read_json(CURRENT)
    if not cur: return None, None
    run_dir = Path(cur['run_dir'])
    return run_dir, read_json(run_dir / 'run.json')


def cmd_status(a):
    run_dir, run = current_run()
    wd_pid = None
    with contextlib.suppress(OSError, ValueError): wd_pid = int(WATCHDOG_PID.read_text())
    holder = common.read_holder('heavy')
    out = dict(heavy_lock=holder, watchdog_pid=wd_pid, watchdog_alive=bool(wd_pid and common.pid_alive(wd_pid)),
               run_dir=str(run_dir) if run_dir else None)
    if run:
        tool = read_json(run_dir / 'tool.json') or {}
        hb = read_json(run_dir / 'heartbeat.json') or {}
        out.update(kind=run['kind'], id=run['id'], wrapper_alive=common.pid_alive(run.get('wrapper_pid') or 0),
                   tool_pid=tool.get('tool_pid'), heartbeat_age_s=round(time.time() - hb['t'], 1) if hb else None,
                   last_sample={k: hb.get(k) for k in ('free_ram_gb', 'swap_gb', 'swap_growth_gb', 'disk_free_gb',
                                                        'tree_gb', 'tree_pids')} if hb else None,
                   peaks=hb.get('peaks'), kill_report=read_json(run_dir / 'kill-report.json'))
    out['machine'] = dict(free_ram_gb=round(vm_free_gb(), 1), swap_gb=round(swap_used_gb(), 1),
                          disk_free_gb=round(disk_free_gb(), 1))
    if a.json:
        print(json.dumps(out, indent=1))
    else:
        if not run: print('no heavy run (heavy.lock %s)' % ('held by ' + common._holder_text(holder) if holder else 'free'))
        else:
            print('heavy %s run %s: wrapper %s, watchdog pid %s %s, heartbeat %s s ago' % (
                out['kind'], out['id'], 'alive' if out['wrapper_alive'] else 'GONE', wd_pid,
                'alive' if out['watchdog_alive'] else 'NOT RUNNING', out['heartbeat_age_s']))
            if out['last_sample']: print('last sample: ' + json.dumps(out['last_sample']))
            if out['kill_report']: print('kill report: ' + out['kill_report']['reason'])
        print('machine: ' + json.dumps(out['machine']))
    return 0


def cmd_kill(a):
    run_dir, run = current_run()
    if not run:
        print('no heavy run')
        return 0
    (run_dir / 'kill-request').write_text(a.reason + '\n')
    deadline = time.monotonic() + TERM_GRACE_S + 3
    while time.monotonic() < deadline:
        if (run_dir / 'kill-report.json').exists():
            print('watchdog tore the run down: ' + read_json(run_dir / 'kill-report.json')['reason'])
            return 0
        time.sleep(0.2)
    run.update(read_json(run_dir / 'tool.json') or {})
    report = teardown(run, 'kill (watchdog did not answer): ' + a.reason)
    write_json(run_dir / 'kill-report.json', report)
    print('tore the run down directly: %d process(es)' % len(report['killed']))
    return 0


def cmd_guard(a):
    """Standalone watchdog on an existing process tree (no locks): main's emergency guard."""
    if not common.pid_alive(a.pid): sys.exit('pid %d is not running' % a.pid)
    run_id = time.strftime('%Y%m%d-%H%M%S') + '-guard-' + uuid.uuid4().hex[:6]
    run_dir = HEAVY_DIR / run_id
    (run_dir / 'artifacts').mkdir(parents=True)
    run = dict(id=run_id, kind=a.kind, guard=True, command=['guard', str(a.pid)], start=time.time(), wrapper_pid=None,
               checkout=common.SLOT or 'main', keep='all', tool_pid=a.pid, tool_pgid=None, tool_start=time.time(),
               swap0_gb=swap_used_gb(), baseline_services=[] if a.services else service_pids(process_table()),
               include_all_services=a.services, max_runtime_s=a.max_s or max_runtime('guard'),
               limits={k: limit(k) for k in LIMITS})
    write_json(run_dir / 'run.json', run)
    wd = spawn_watchdog(run_dir, [])
    if not wait_heartbeat(run_dir, wd): sys.exit('guard watchdog did not start')
    print(json.dumps(dict(run_dir=str(run_dir), watchdog_pid=wd.pid)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog='fable2_heavy.py', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    q = sub.add_parser('run')
    q.add_argument('--kind', required=True, choices=KINDS)
    q.add_argument('--keep', default='auto', choices=['auto', 'all', 'none'],
                   help='artifacts: auto deletes those over FABLE2_HEAVY_KEEP_MAX_GB (2)')
    q.add_argument('--save', help='xctrace record: library save for the test game')
    q.add_argument('--game-wait', type=float, default=20.0, help='xctrace record: seconds from launch to the tool')
    q.add_argument('--allow-scale', action='store_true', help='gputrace: allow FABLE2_RES_SCALE > 1 (main confirms)')
    q.add_argument('command', nargs=argparse.REMAINDER)
    q = sub.add_parser('status'); q.add_argument('--json', action='store_true')
    q = sub.add_parser('kill'); q.add_argument('--reason', default='manual')
    q = sub.add_parser('guard'); q.add_argument('--pid', type=int, required=True)
    q.add_argument('--kind', default='guard'); q.add_argument('--max-s', type=float)
    q.add_argument('--services', action='store_true', help='also count GPUTools/xctrace services already running')
    q = sub.add_parser('watchdog'); q.add_argument('--run-dir', required=True)
    q.add_argument('--lock-fd', type=int, action='append', default=[])
    a = ap.parse_args(argv)
    if a.cmd == 'run': return cmd_run(a)
    if a.cmd == 'status': return cmd_status(a)
    if a.cmd == 'kill': return cmd_kill(a)
    if a.cmd == 'guard': return cmd_guard(a)
    if a.cmd == 'watchdog': return watchdog_main(a.run_dir, a.lock_fd)
    return 2


if __name__ == '__main__':
    sys.exit(main())
