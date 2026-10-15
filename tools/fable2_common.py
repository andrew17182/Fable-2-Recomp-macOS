#!/usr/bin/env python3
"""Shared paths and resource guards for the main checkout and the agent worktree slots (local agent docs).

Paths
  ROOT        this checkout (the main checkout or an agent slot, <workspace>/agent-slots/agent-N)
  MAIN_ROOT   the main checkout, from the worktree's .git file (FABLE2_MAIN_ROOT overrides)
  WORKSPACE   the main checkout's parent folder (agent-metrics/, restore-points/, agent-slots/)
  SLOT        'agent-N' inside a slot, else None
Global state (shared by every checkout): MAIN_ROOT/out/instances (game registry), MAIN_ROOT/out/save-library
(read), MAIN_ROOT/out/playtest, MAIN_ROOT/out/locks (perf lock, launch lock, build markers),
WORKSPACE/agent-metrics. Per checkout: out/build, out/metal-build, out/sdk-src, out/dev-sessions, out/validation.

Guards
  ensure_free_space(what)  below FABLE2_MIN_FREE_GB (100) free: auto-clean (old sessions in every
                           checkout, old validation evidence, build folders of idle slots), then refuse
  instance_slot(...)       caps concurrent game instances at FABLE2_MAX_INSTANCES (4); waits, then refuses
  perf_lock(...)           one perf measurement run at a time across all checkouts (fcntl LOCK_EX), with a queue;
                           waits for running test games, which hold it shared (shared_lock + `hold`)
  test_game_hold(...)      agent/test game limit: FABLE2_AGENT_GAMES (2) games at once across all checkouts, and
                           FABLE2_AGENT_3X_GAMES (1) of them at 3x (flock'd slot files in out/locks/games/, held
                           for the game's life by the `hold` process); FIFO queue; play games never take one
  build_marker(...)        registers a running build after holding perf.lock shared for its duration (builds wait
                           for an exclusive perf run and perf runs wait for builds; FABLE2_BUILD_ROLE=play skips)
                           and waiting while a heavy-tool run holds heavy.lock (local agent docs);
                           build_jobs() lowers the job count while others build
Runs under /usr/bin/python3 (3.9).
"""
import contextlib
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def _main_root(root):
    env = os.environ.get('FABLE2_MAIN_ROOT')
    if env: return Path(env).expanduser().resolve()
    dotgit = root / '.git'
    if dotgit.is_file():  # linked worktree: "gitdir: <main>/.git/worktrees/<name>"
        try:
            text = dotgit.read_text().strip()
        except OSError:
            return root
        if text.startswith('gitdir:'):
            gitdir = Path(text.split(':', 1)[1].strip())
            if not gitdir.is_absolute(): gitdir = (root / gitdir).resolve()
            # <main>/.git/worktrees/<name> -> <main>/.git -> <main>
            if gitdir.parent.name == 'worktrees' and gitdir.parent.parent.name == '.git':
                return gitdir.parent.parent.parent
    return root


MAIN_ROOT = _main_root(ROOT)
IS_SLOT = ROOT != MAIN_ROOT
WORKSPACE = MAIN_ROOT.parent
SLOTS_DIR = Path(os.environ.get('FABLE2_AGENT_SLOTS_DIR') or WORKSPACE / 'agent-slots')
SLOT = ROOT.name if IS_SLOT else None


# AppKit arguments for every agent/test game launch (approved 2026-10-14): never show macOS's "reopen
# windows?" alert after an earlier crash of org.fable2recomp.metal. That alert is modal on the main thread before
# the renderer starts, so a test game hangs (and the alert appears on the player's screen). AppKit reads `-Key value`
# from the process arguments (NSArgumentDomain); the game's own parser ignores them (no positional options).
# Normal play launches never get them.
TEST_LAUNCH_APPKIT_ARGS = ('-ApplePersistenceIgnoreState', 'YES')


def window_tag(kind='test'):
    """FABLE2_WINDOW_TAG for a game this checkout starts (appended to its window title,
    src/core/fable2_window_title.h): 'test agent-2' in a slot, 'test main' in the main checkout.
    Play launches get no tag."""
    return '%s %s' % (kind, SLOT or 'main')
LOCKS = Path(os.environ.get('FABLE2_LOCKS_DIR') or MAIN_ROOT / 'out/locks')  # override: lock tests only
AGENT_METRICS = WORKSPACE / 'agent-metrics'

GB = 1024 ** 3


def env_float(name, default):
    try:
        return float(os.environ.get(name, default))
    except ValueError:
        return float(default)


def log(text):
    print(text, file=sys.stderr, flush=True)


def checkouts():
    """The main checkout plus every slot folder that exists: [(name, root)]."""
    out = [('main', MAIN_ROOT)]
    if SLOTS_DIR.is_dir():
        for d in sorted(SLOTS_DIR.glob('agent-*')):
            if d.is_dir() and (d / '.git').exists(): out.append((d.name, d))
    return out


def pid_alive(pid):
    try:
        os.kill(int(pid), 0)
    except (OSError, ValueError):
        return False
    result = subprocess.run(['ps', '-p', str(int(pid)), '-o', 'stat='], capture_output=True, text=True)
    return result.returncode == 0 and not result.stdout.strip().startswith('Z')


def _atomic_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp-%d' % os.getpid())
    tmp.write_text(json.dumps(data, indent=2) + '\n')
    os.replace(tmp, path)


def clone(src, dst):
    """APFS clone (copy-on-write: ~0 disk until either side writes) of a file or folder; dst must not exist.
    Falls back to a normal copy on a filesystem without clonefile."""
    src, dst = Path(src), Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(['cp', '-cRp', str(src), str(dst)], capture_output=True, text=True)
    if result.returncode != 0:
        if dst.exists(): shutil.rmtree(dst, ignore_errors=True) if dst.is_dir() else dst.unlink()
        if src.is_dir(): shutil.copytree(src, dst, symlinks=True)
        else: shutil.copy2(src, dst)


# ---------------------------------------------------------------- disk space

def free_gb(path=None):
    st = os.statvfs(str(path or MAIN_ROOT))
    return st.f_bavail * st.f_frsize / GB


def min_free_gb():
    return env_float('FABLE2_MIN_FREE_GB', 100)


def auto_clean(reason=''):
    """Free space without touching anything in use: prune sessions and validation evidence in every
    checkout (normal rules), then wipe the build folders of idle slots. Returns a summary."""
    sys.path.insert(0, str(ROOT / 'tools'))
    summary = dict(reason=reason, free_before_gb=round(free_gb(), 1), sessions=0, validation=0, slot_builds=[])
    try:
        import fable2_dev_session as dev
        for name, root in checkouts():
            summary['sessions'] += len(dev.prune_sessions(sessions_dir=root / 'out/dev-sessions',
                                                          slot=None if root == MAIN_ROOT else name))
            summary['validation'] += len(prune_validation(root))
    except Exception as error:
        summary['error'] = repr(error)
    try:
        import fable2_slots as slots
        summary['slot_builds'] = slots.wipe_idle_builds(min_idle_hours=0, reason='low disk')
    except Exception as error:
        summary['slots_error'] = repr(error)
    summary['free_after_gb'] = round(free_gb(), 1)
    return summary


def ensure_free_space(what, refuse=True):
    """Before a build or launch: below the threshold, auto-clean, then refuse (refuse=False only warns:
    the player's own play is never refused)."""
    need, have = min_free_gb(), free_gb()
    if have >= need: return dict(ok=True, free_gb=round(have, 1))
    log('LOW DISK: %.0f GB free (< %.0f GB) before %s; auto-cleaning old sessions, validation and idle slot builds'
        % (have, need, what))
    summary = auto_clean(what)
    log('auto-clean: ' + json.dumps(summary))
    have = free_gb()
    if have >= need: return dict(ok=True, free_gb=round(have, 1), cleaned=summary)
    message = ('Not enough disk space for %s: %.0f GB free, need %.0f GB (FABLE2_MIN_FREE_GB). Auto-clean freed '
               '%.1f GB. Free space by hand (python3 tools/fable2_slots.py resources shows what is big).'
               % (what, have, need, have - summary['free_before_gb']))
    if refuse: raise RuntimeError(message)
    log('WARNING: ' + message)
    return dict(ok=False, free_gb=round(have, 1), cleaned=summary)


# ---------------------------------------------------------------- validation pruning

VALIDATION_KEEP_DAYS_DEFAULT = 7
# Folders under out/validation holding many independent runs: each child is pruned on its own.
VALIDATION_CONTAINERS = ('f2diag', 'gameplay-tests')


def _newest_mtime(path):
    """Newest mtime of a folder, its direct children and grandchildren (cheap; no full walk)."""
    newest = 0.0
    try:
        newest = path.stat().st_mtime
        for child in path.iterdir():
            st = child.lstat()
            newest = max(newest, st.st_mtime)
            if child.is_dir() and not child.is_symlink():
                for grand in child.iterdir():
                    newest = max(newest, grand.lstat().st_mtime)
    except OSError:
        pass
    return newest


def referenced_validation_paths(root):
    """out/validation/<...> paths named in tracked docs (CLAUDE.md, docs/, plans/): never auto-pruned."""
    refs = set()
    files = [root / 'CLAUDE.md', root / 'README.md'] + list((root / 'docs').glob('*.md')) + list((root / 'plans').glob('*.md'))
    for f in files:
        try:
            text = f.read_text(errors='replace')
        except OSError:
            continue
        start = 0
        while True:
            i = text.find('out/validation/', start)
            if i < 0: break
            j = i + len('out/validation/')
            k = j
            while k < len(text) and (text[k].isalnum() or text[k] in '-_./'): k += 1
            parts = [p for p in text[j:k].split('/') if p]
            if parts:
                refs.add(parts[0])
                if parts[0] in VALIDATION_CONTAINERS and len(parts) > 1: refs.add(parts[0] + '/' + parts[1])
            start = k
    return refs


def validation_candidates(root=ROOT, days=None, now=None):
    """[(rel, path, age_days)] of out/validation folders older than `days` without a KEEP file."""
    days = env_float('FABLE2_VALIDATION_KEEP_DAYS', VALIDATION_KEEP_DAYS_DEFAULT) if days is None else days
    base = Path(root) / 'out/validation'
    if not base.is_dir(): return []
    now = now or time.time()
    refs = referenced_validation_paths(Path(root)) | referenced_validation_paths(MAIN_ROOT)
    out = []

    def consider(rel, path):
        if (path / 'KEEP').exists() or rel in refs: return
        age = (now - _newest_mtime(path)) / 86400
        if age >= days: out.append((rel, path, round(age, 1)))

    for top in sorted(base.iterdir()):
        if not top.is_dir() or top.is_symlink() or (top / 'KEEP').exists(): continue
        if top.name in VALIDATION_CONTAINERS:
            for child in sorted(top.iterdir()):
                if child.is_dir() and not child.is_symlink(): consider(top.name + '/' + child.name, child)
        elif top.name not in refs:
            consider(top.name, top)
    return out


def prune_validation(root=ROOT, days=None, dry_run=False):
    """Delete out/validation evidence older than FABLE2_VALIDATION_KEEP_DAYS (7) unless the folder (or its
    top-level folder) has a KEEP file or a tracked doc names it. FABLE2_VALIDATION_PRUNE=0 disables."""
    if os.environ.get('FABLE2_VALIDATION_PRUNE') == '0': return []
    removed = []
    for rel, path, age in validation_candidates(root, days):
        if not dry_run: shutil.rmtree(path, ignore_errors=True)
        removed.append(dict(path=rel, age_days=age))
    return removed


# ---------------------------------------------------------------- locks

class LockTimeout(RuntimeError):
    pass


# name -> open fd of each global_lock this process holds right now (fable2_heavy.py hands them to its watchdog,
# so the locks outlive a killed wrapper until the watchdog has torn the tool down).
HELD_FDS = {}


def _holder_text(info):
    if not info: return 'unknown holder'
    return '%s (pid %s, %s, since %s)' % (info.get('what'), info.get('pid'), info.get('checkout'),
                                          time.strftime('%H:%M:%S', time.localtime(info.get('since', 0))))


def read_holder(name):
    try:
        info = json.loads((LOCKS / (name + '.holder.json')).read_text())
    except (OSError, ValueError):
        return None
    return info if info.get('pid') and pid_alive(info['pid']) else None


def queue(name):
    folder = LOCKS / (name + '-queue')
    rows = []
    for path in sorted(folder.glob('*.json')) if folder.is_dir() else []:
        try:
            row = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        if pid_alive(row.get('pid', 0)): rows.append(row)
        else: path.unlink(missing_ok=True)
    rows.sort(key=lambda r: r.get('since', 0))
    return rows


def _try_flock(fd, op):
    try:
        fcntl.flock(fd, op | fcntl.LOCK_NB)
        return True
    except BlockingIOError:
        return False


def shared_holders(name='perf'):
    """Live shared holders of <name>.lock (test games): rows from <name>-shared/<holder pid>.json."""
    folder = LOCKS / (name + '-shared')
    rows = []
    for path in sorted(folder.glob('*.json')) if folder.is_dir() else []:
        try:
            row = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        if pid_alive(row.get('pid', 0)): rows.append(row)
        else: path.unlink(missing_ok=True)
    return rows


def _shared_text(rows):
    if not rows: return 'unknown holder'
    names = ', '.join('%s (game pid %s, %s)' % (r.get('session') or r.get('what'), r.get('game_pid'), r.get('checkout'))
                      for r in rows[:4])
    return '%d test game%s: %s%s' % (len(rows), '' if len(rows) == 1 else 's', names, ' ...' if len(rows) > 4 else '')


@contextlib.contextmanager
def global_lock(name, what, timeout, poll=2.0, quiet=False, gate=False):
    """Exclusive fcntl lock MAIN_ROOT/out/locks/<name>.lock shared by every checkout. Yields the wait in
    seconds. While waiting the caller is listed in <name>-queue/ (shown by fable2_slots.py resources).
    gate=True (perf lock): also hold <name>-gate.lock while waiting, so no new shared holder (test game,
    shared_lock) gets in ahead of this run; the gate is released once the main lock is held."""
    LOCKS.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(LOCKS / (name + '.lock')), os.O_RDWR | os.O_CREAT, 0o644)
    gfd = os.open(str(LOCKS / (name + '-gate.lock')), os.O_RDWR | os.O_CREAT, 0o644) if gate else None
    have_gate = False
    me = dict(pid=os.getpid(), what=what, checkout=SLOT or 'main', since=time.time(), mode='exclusive')
    ticket = LOCKS / (name + '-queue') / ('%d.json' % os.getpid())
    started, last_note = time.monotonic(), 0.0
    try:
        while True:
            if gfd is not None and not have_gate: have_gate = _try_flock(gfd, fcntl.LOCK_EX)
            if (gfd is None or have_gate) and _try_flock(fd, fcntl.LOCK_EX): break
            if not ticket.exists(): _atomic_json(ticket, me)
            waited = time.monotonic() - started
            holder = read_holder(name)
            holder_text = _holder_text(holder) if holder or not gate else _shared_text(shared_holders(name))
            if waited >= timeout:
                raise LockTimeout('%s lock: waited %.0f s for %s; giving up (timeout %.0f s)'
                                  % (name, waited, holder_text, timeout))
            if not quiet and time.monotonic() - last_note >= 30:
                last_note = time.monotonic()
                ahead = [r for r in queue(name) if r['since'] < me['since'] and r.get('mode') != 'shared']
                log('%s LOCK: waiting for %s; %d ahead in the queue (waited %.0f s, timeout %.0f s)'
                    % (name.upper(), holder_text, len(ahead), waited, timeout))
            time.sleep(poll)
        if have_gate:
            fcntl.flock(gfd, fcntl.LOCK_UN)
            have_gate = False
        ticket.unlink(missing_ok=True)
        wait = round(time.monotonic() - started, 1)
        _atomic_json(LOCKS / (name + '.holder.json'), dict(me, since=time.time(), waited_s=wait))
        if wait and not quiet: log('%s LOCK: acquired after %.1f s' % (name.upper(), wait))
        HELD_FDS[name] = fd
        try:
            yield wait
        finally:
            HELD_FDS.pop(name, None)
            try:
                (LOCKS / (name + '.holder.json')).unlink()
            except FileNotFoundError:
                pass
            fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        ticket.unlink(missing_ok=True)
        if gfd is not None: os.close(gfd)
        os.close(fd)


def perf_lock(what):
    """Serialise perf measurement runs across all checkouts, and wait for running test games (their shared
    holds) to finish (FABLE2_PERF_LOCK_TIMEOUT, default 1800 s). Games launched by this process (and its
    children) while it holds the lock skip the shared hold (FABLE2_PERF_LOCK_HOLDER)."""
    @contextlib.contextmanager
    def held():
        with global_lock('perf', what, env_float('FABLE2_PERF_LOCK_TIMEOUT', 1800), poll=1.0, gate=True) as wait:
            previous = os.environ.get('FABLE2_PERF_LOCK_HOLDER')
            os.environ['FABLE2_PERF_LOCK_HOLDER'] = str(os.getpid())
            try:
                yield wait
            finally:
                if previous is None: os.environ.pop('FABLE2_PERF_LOCK_HOLDER', None)
                else: os.environ['FABLE2_PERF_LOCK_HOLDER'] = previous
    return held()


def perf_shared_skip_reason():
    """Why a test launch takes no shared perf hold, or None: FABLE2_PERF_LOCK_SHARED=0 (f2diag --no-perf-lock),
    or this process tree already holds the perf lock exclusively (the perf run's own game)."""
    if os.environ.get('FABLE2_PERF_LOCK_SHARED') == '0': return 'FABLE2_PERF_LOCK_SHARED=0'
    holder_env = os.environ.get('FABLE2_PERF_LOCK_HOLDER')
    holder = read_holder('perf')
    if holder_env and holder and str(holder.get('pid')) == holder_env: return 'perf run holds the lock'
    return None


class SharedHold:
    """Locks a test launch holds for its game's life: the shared perf hold (an open fd on perf.lock with LOCK_SH;
    None when skipped) plus any game-slot locks (add_games). hand_off(game_pid) passes the fds to a detached holder
    process (`fable2_common.py hold`) that lives as long as the game, then closes this process's copies. close()
    without a hand-off releases them (launch failed). fcntl locks go with the last fd, so a killed holder (or a
    crashed launcher) releases them."""

    def __init__(self, fd, wait, what):
        self.fd, self.wait, self.what = fd, wait, what
        self.game_fds, self.games, self.games_wait, self.scale = [], [], None, None

    def add_games(self, fds, names, wait, scale):
        self.game_fds, self.games, self.games_wait, self.scale = list(fds), list(names), wait, scale

    def hand_off(self, game_pid, session=None):
        fds = ([self.fd] if self.fd is not None else []) + self.game_fds
        if not fds: return None
        info = dict(game_pid=int(game_pid), what=self.what, checkout=SLOT or 'main',
                    session=Path(session).name if session else None, waited_s=self.wait,
                    perf=self.fd is not None, games=self.games, games_waited_s=self.games_wait, scale=self.scale)
        args = [sys.executable, str(Path(__file__).resolve()), 'hold', '--info', json.dumps(info)]
        for fd in fds: args += ['--fd', str(fd)]
        proc = subprocess.Popen(args, pass_fds=tuple(fds), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, start_new_session=True)
        self.close()
        return proc.pid

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        for fd in self.game_fds: os.close(fd)
        self.game_fds = []


def shared_lock(what, timeout=None, poll=1.0):
    """Take perf.lock shared for a test game (local agent docs). Waits while a perf run holds the lock or is
    queued for it (it holds perf-gate.lock), up to FABLE2_PERF_LOCK_SHARED_TIMEOUT (1800 s). Returns a
    SharedHold (fd None when skipped, see perf_shared_skip_reason); its wait is .wait seconds."""
    skip = perf_shared_skip_reason()
    if skip: return SharedHold(None, None, what)
    if timeout is None: timeout = env_float('FABLE2_PERF_LOCK_SHARED_TIMEOUT', 1800)
    LOCKS.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(LOCKS / 'perf.lock'), os.O_RDWR | os.O_CREAT, 0o644)
    gfd = os.open(str(LOCKS / 'perf-gate.lock'), os.O_RDWR | os.O_CREAT, 0o644)
    me = dict(pid=os.getpid(), what=what, checkout=SLOT or 'main', since=time.time(), mode='shared')
    ticket = LOCKS / 'perf-queue' / ('%d.json' % os.getpid())
    started, last_note, ok = time.monotonic(), 0.0, False
    try:
        while True:
            if _try_flock(gfd, fcntl.LOCK_SH):
                got = _try_flock(fd, fcntl.LOCK_SH)
                fcntl.flock(gfd, fcntl.LOCK_UN)
                if got: break
            if not ticket.exists(): _atomic_json(ticket, me)
            waited = time.monotonic() - started
            holder = read_holder('perf')
            waiting = [r for r in queue('perf') if r.get('mode') != 'shared']
            text = ('perf run %s' % _holder_text(holder) if holder else
                    'queued perf run %s' % _holder_text(waiting[0]) if waiting else 'a perf run')
            if waited >= timeout:
                raise LockTimeout('PERF LOCK: "%s" waited %.0f s for %s; giving up (timeout %.0f s, '
                                  'FABLE2_PERF_LOCK_SHARED_TIMEOUT). Try again when the perf run is done.'
                                  % (what, waited, text, timeout))
            if time.monotonic() - last_note >= 30:
                last_note = time.monotonic()
                log('PERF LOCK: "%s" waiting for %s (waited %.0f s, timeout %.0f s)' % (what, text, waited, timeout))
            time.sleep(poll)
        ok = True
    finally:
        ticket.unlink(missing_ok=True)
        os.close(gfd)
        if not ok: os.close(fd)
    wait = round(time.monotonic() - started, 1)
    if wait >= 1: log('PERF LOCK: "%s" went ahead after %.1f s' % (what, wait))
    return SharedHold(fd, wait, what)


def _hold_main(argv):
    """`fable2_common.py hold --fd N [--fd M ...] --info JSON`: keep the inherited locks (shared perf hold, game
    slots) until the game exits."""
    import argparse
    ap = argparse.ArgumentParser(prog='fable2_common.py hold')
    ap.add_argument('--fd', type=int, action='append', required=True)
    ap.add_argument('--info', required=True)
    a = ap.parse_args(argv)
    info = json.loads(a.info)
    records = []
    if info.get('perf', True): records.append(LOCKS / 'perf-shared' / ('%d.json' % os.getpid()))
    if info.get('games'): records.append(GAMES / 'held' / ('%d.json' % os.getpid()))
    for record in records: _atomic_json(record, dict(info, pid=os.getpid(), since=time.time()))
    started = time.time()
    try:
        while pid_alive(info['game_pid']):
            time.sleep(1.0)
    except BaseException as error:  # a holder that dies early releases its locks: leave a trace
        with open(LOCKS / 'hold-errors.log', 'a') as f:
            f.write('%s holder %d game %s: %r\n' % (time.strftime('%F %T'), os.getpid(), info.get('game_pid'), error))
        raise
    finally:
        with open(LOCKS / 'hold.log', 'a') as f:
            f.write('%s holder %d game %s %s released after %.1f s\n' % (time.strftime('%F %T'), os.getpid(),
                    info.get('game_pid'), '+'.join(info.get('games') or []) or 'perf', time.time() - started))
        for record in records: record.unlink(missing_ok=True)
        for fd in a.fd: os.close(fd)
    return 0


# ---------------------------------------------------------------- agent game limit

GAMES = LOCKS / 'games'


def agent_game_limit():
    return max(1, int(env_float('FABLE2_AGENT_GAMES', 2)))


def agent_3x_limit():
    return max(1, int(env_float('FABLE2_AGENT_3X_GAMES', 1)))


def launch_scale(env=None, state=None):
    """Resolution scale a test launch will run at: FABLE2_RES_SCALE in its environment, else resolution_scale in
    the state's fable2_config.toml, else 1. 'auto' counts as 3 (it picks 3 on big Apple Silicon GPUs)."""
    value = (env or {}).get('FABLE2_RES_SCALE')
    if not value and state:
        try:
            for line in (Path(state) / 'fable2_config.toml').read_text(errors='replace').splitlines():
                key, _, rest = line.partition('=')
                if key.strip() == 'resolution_scale':
                    value = rest.split('#')[0].strip().strip('"')
                    break
        except OSError:
            pass
    if not value: return 1
    if value == 'auto': return 3
    try:
        return int(float(value))
    except ValueError:
        return 1


def game_holders():
    """Live game-slot holders (test games): rows from games/held/<holder pid>.json."""
    folder = GAMES / 'held'
    rows = []
    for path in sorted(folder.glob('*.json')) if folder.is_dir() else []:
        try:
            row = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        if pid_alive(row.get('pid', 0)): rows.append(row)
        else: path.unlink(missing_ok=True)
    return rows


def game_queue():
    """Launches waiting for a game slot, oldest first."""
    folder = GAMES / 'queue'
    rows = []
    for path in sorted(folder.glob('*.json')) if folder.is_dir() else []:
        try:
            row = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        if pid_alive(row.get('pid', 0)): rows.append(row)
        else: path.unlink(missing_ok=True)
    rows.sort(key=lambda r: (r.get('since', 0), r.get('pid', 0)))
    return rows


def _holders_text(rows):
    if not rows: return 'agent games'
    return ', '.join('%s (%s, %sx, game pid %s)' % (r.get('session') or r.get('what'), r.get('checkout'),
                                                   r.get('scale'), r.get('game_pid')) for r in rows[:4])


def yield_to_play():
    return os.environ.get('FABLE2_YIELD_TO_PLAY', '1') != '0'


def _lstart_age(lstart):
    try:
        return time.time() - time.mktime(time.strptime(lstart, '%a %b %d %H:%M:%S %Y'))
    except (TypeError, ValueError):
        return None


def play_games(current=None):
    """The player's running play games, which agent launches yield to (FABLE2_YIELD_TO_PLAY, default on): registry
    entries with role play/playtest that are not test sessions, and fable_2 processes nobody registered (the player
    app, a hand start), except those from an agent slot or the benchmark's app copy, and except an unregistered
    process younger than FABLE2_PLAY_YIELD_GRACE_S (15 s), which is usually a test launch about to register.
    Only live processes count (fable2_instances prunes dead and reused pids)."""
    if current is None:
        import fable2_instances as instances
        current = instances.instances()
    grace = env_float('FABLE2_PLAY_YIELD_GRACE_S', 15)
    agent_dirs = [str(SLOTS_DIR) + '/', str(MAIN_ROOT / 'out/benchmark') + '/']
    out = []
    for e in current:
        if e.get('test_control'): continue
        if e.get('role') in ('play', 'playtest'):
            out.append(e)
        elif e.get('role') == 'untracked':
            exe = e.get('exe') or ''
            if any(exe.startswith(d) for d in agent_dirs): continue
            age = _lstart_age(e.get('lstart'))
            if age is not None and age < grace: continue
            out.append(e)
    return out


def _play_text(rows):
    return ', '.join('pid %s, %s' % (r.get('pid'), r.get('role')) for r in rows[:3])


def _try_slots(prefix, count):
    """Take the first free <prefix>-<k>.lock of count; (fd, name) or (None, None)."""
    for k in range(count):
        name = '%s-%d' % (prefix, k + 1)
        fd = os.open(str(GAMES / (name + '.lock')), os.O_RDWR | os.O_CREAT, 0o644)
        if _try_flock(fd, fcntl.LOCK_EX): return fd, name
        os.close(fd)
    return None, None


def _any_free(prefix, count):
    fd, _ = _try_slots(prefix, count)
    if fd is None: return False
    os.close(fd)
    return True


def acquire_game_slots(what, scale, timeout=None, poll=0.5):
    """Agent game limit (local agent docs): take one of FABLE2_AGENT_GAMES (2) game slots, plus one of
    FABLE2_AGENT_3X_GAMES (1) 3x slots for a 3x game, across every checkout. First come, first served (a ticket in
    games/queue/; only the oldest waiter tries, and a non-3x launch may pass 3x waiters while the 3x slot is taken). Waits up to FABLE2_AGENT_GAMES_TIMEOUT (1800 s), logging the queue
    position. Returns (fds, names, wait_s); the caller keeps the fds for the game's life (SharedHold.add_games)."""
    if timeout is None: timeout = env_float('FABLE2_AGENT_GAMES_TIMEOUT', 1800)
    GAMES.mkdir(parents=True, exist_ok=True)
    (GAMES / 'queue').mkdir(exist_ok=True)
    big = scale >= 3
    me = dict(pid=os.getpid(), what=what, checkout=SLOT or 'main', since=time.time(), scale=scale)
    ticket = GAMES / 'queue' / ('%d.json' % os.getpid())
    _atomic_json(ticket, me)
    started, last_note, play_waited, play_checked, playing = time.monotonic(), -1e9, 0.0, -1e9, []
    try:
        while True:
            ahead = [r for r in game_queue() if (r.get('since', 0), r.get('pid', 0)) < (me['since'], me['pid'])]
            # Agent games yield to the player's game (approved 2026-10-13): no new agent game while a play game runs.
            # Running agent games finish their run; the queue order is kept.
            if time.monotonic() - play_checked >= 1.0:  # a registry + ps scan; at most once a second
                play_checked = time.monotonic()
                playing = play_games() if yield_to_play() else []
            if playing:
                if not me.get('play_wait_since'):
                    me['play_wait_since'] = time.time()
                    me['waiting_for_play'] = [r.get('pid') for r in playing]
                    _atomic_json(ticket, me)
                    last_note = -1e9
                waited = time.monotonic() - started
                play_timeout = env_float('FABLE2_PLAY_YIELD_TIMEOUT', 4 * 3600)
                if time.time() - me['play_wait_since'] >= play_timeout:
                    raise LockTimeout('AGENT GAMES: "%s" waited %.0f s for the play session to end (%s); '
                                      'giving up (FABLE2_PLAY_YIELD_TIMEOUT %.0f s)'
                                      % (what, time.time() - me['play_wait_since'], _play_text(playing), play_timeout))
                if time.monotonic() - last_note >= 30:
                    last_note = time.monotonic()
                    log('AGENT GAMES: "%s" waiting for the play session (%s); position %d in the queue '
                        '(waited %.0f s)' % (what, _play_text(playing), len(ahead) + 1, waited))
                time.sleep(max(poll, 1.0))
                continue
            if me.get('play_wait_since'):
                play_waited += time.time() - me['play_wait_since']
                log('AGENT GAMES: "%s" play session ended after %.0f s; continuing' % (what, time.time() - me['play_wait_since']))
                me.pop('play_wait_since'), me.pop('waiting_for_play', None)
                _atomic_json(ticket, me)
            # Strict FIFO: only the oldest waiter tries, except that a non-3x launch may pass 3x waiters while the
            # 3x slot is taken (they could not use a free game slot anyway).
            may_try = not ahead or (not big and all((r.get('scale') or 1) >= 3 for r in ahead)
                                    and not _any_free('3x', agent_3x_limit()))
            if may_try:
                fd, name = _try_slots('slot', agent_game_limit())
                if fd is not None:
                    if not big:
                        names, fds = [name], [fd]
                        break
                    fd3, name3 = _try_slots('3x', agent_3x_limit())
                    if fd3 is not None:
                        names, fds = [name, name3], [fd, fd3]
                        break
                    os.close(fd)  # don't hold a slot while waiting for the 3x one
            waited = time.monotonic() - started
            if waited - play_waited >= timeout:
                raise LockTimeout('AGENT GAMES: "%s" waited %.0f s for a game slot (%s); giving up '
                                  '(FABLE2_AGENT_GAMES_TIMEOUT %.0f s)' % (what, waited, _holders_text(game_holders()),
                                                                           timeout))
            if time.monotonic() - last_note >= 30:
                last_note = time.monotonic()
                log('AGENT GAMES: "%s" (%dx) waiting: %d of %d agent game%s running%s (%s); position %d in the queue '
                    '(waited %.0f s)' % (what, scale, len(game_holders()), agent_game_limit(),
                                         '' if agent_game_limit() == 1 else 's',
                                         ', 3x limit %d' % agent_3x_limit() if big else '',
                                         _holders_text(game_holders()), len(ahead) + 1, waited))
            time.sleep(poll)
    finally:
        ticket.unlink(missing_ok=True)
    wait = round(time.monotonic() - started, 1)
    if wait >= 1: log('AGENT GAMES: "%s" got %s after %.1f s' % (what, '+'.join(names), wait))
    return fds, names, wait


def test_game_hold(what, scale):
    """Locks for one agent/test game launch, in this order: the shared perf hold (shared_lock; waits for a perf
    run), then a game slot (acquire_game_slots). Always perf before game slots, so a perf run's own game (which
    skips the shared hold) can never wait on a slot held by a game that waits for the perf run. Returns a SharedHold;
    hand it off to the holder process once the game's pid is known."""
    hold = shared_lock(what)
    try:
        fds, names, wait = acquire_game_slots(what, scale)
    except BaseException:
        hold.close()
        raise
    hold.add_games(fds, names, wait, scale)
    return hold


@contextlib.contextmanager
def game_slots_held(what, scale):
    """In-process variant for a tool that runs its own games one after another (the benchmark orchestrator):
    hold a game slot (and a 3x slot for scale >= 3) until the block ends."""
    fds, names, wait = acquire_game_slots(what, scale)
    record = GAMES / 'held' / ('%d.json' % os.getpid())
    _atomic_json(record, dict(pid=os.getpid(), game_pid=os.getpid(), what=what, checkout=SLOT or 'main',
                              games=names, games_waited_s=wait, scale=scale, since=time.time()))
    try:
        yield wait
    finally:
        record.unlink(missing_ok=True)
        for fd in fds: os.close(fd)


# ---------------------------------------------------------------- instance cap

def max_instances():
    return int(env_float('FABLE2_MAX_INSTANCES', 4))


class InstanceCapError(RuntimeError):
    pass


@contextlib.contextmanager
def instance_slot(what, count_running):
    """Hold the global launch lock while checking the instance cap and starting the game (so two
    checkouts can't both take the last place). Waits up to FABLE2_INSTANCE_WAIT (600 s) for a free place."""
    timeout = env_float('FABLE2_INSTANCE_WAIT', 600)
    started, last_note = time.monotonic(), 0.0
    while True:
        with global_lock('launch', what, timeout=300, poll=0.5, quiet=True):
            running = count_running()
            if running < max_instances():
                yield running
                return
        waited = time.monotonic() - started
        if waited >= timeout:
            raise InstanceCapError('Instance cap: %d games running (FABLE2_MAX_INSTANCES=%d); waited %.0f s. '
                               'Try again when one closes.' % (running, max_instances(), waited))
        if time.monotonic() - last_note >= 30:
            last_note = time.monotonic()
            log('INSTANCE CAP: %d games running (max %d); waiting for one to close (%.0f s)'
                % (running, max_instances(), waited))
        time.sleep(3)


def wait_instance_room(what, count_running):
    """Unlocked variant of instance_slot for launch paths that can't hold the launch lock."""
    with instance_slot(what, count_running) as running:
        return running


# ---------------------------------------------------------------- builds

BUILDS = LOCKS / 'builds'


def running_builds():
    rows = []
    for path in sorted(BUILDS.glob('*.json')) if BUILDS.is_dir() else []:
        try:
            row = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        if pid_alive(row.get('pid', 0)): rows.append(row)
        else: path.unlink(missing_ok=True)
    return rows


def wait_heavy_idle(what, timeout=None, poll=2.0):
    """Wait while a heavy-tool run (tools/fable2_heavy.py: xctrace, GPU capture, replay) holds heavy.lock, up to
    FABLE2_HEAVY_BUILD_WAIT (1800 s), then refuse. Returns an fd holding heavy.lock SHARED: the caller registers
    itself (build marker) and then closes it, so a heavy run either sees the marker or starts before the check."""
    if timeout is None: timeout = env_float('FABLE2_HEAVY_BUILD_WAIT', 1800)
    LOCKS.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(LOCKS / 'heavy.lock'), os.O_RDWR | os.O_CREAT, 0o644)
    started, last_note = time.monotonic(), -1e9
    while not _try_flock(fd, fcntl.LOCK_SH):
        waited = time.monotonic() - started
        holder = _holder_text(read_holder('heavy'))
        if waited >= timeout:
            os.close(fd)
            raise LockTimeout('HEAVY RUN: %s waited %.0f s for %s; giving up (FABLE2_HEAVY_BUILD_WAIT)'
                              % (what, waited, holder))
        if time.monotonic() - last_note >= 30:
            last_note = time.monotonic()
            log('HEAVY RUN: %s waiting for %s (waited %.0f s)' % (what, holder, waited))
        time.sleep(poll)
    return fd


@contextlib.contextmanager
def build_perf_hold(what='build'):
    """Builds don't overlap perf runs (local agent docs): hold perf.lock SHARED for the build's duration, so a
    build waits while an exclusive perf run holds (or waits for) the lock and a perf run waits for running builds.
    The wait is logged and yielded. FABLE2_BUILD_ROLE=play (play builds: Setup app, Dev Launcher) doesn't wait
    or hold; its build marker still shows in `fable2_slots.py resources`. Listed in perf-shared/<pid>.json as a
    'build' holder."""
    # Nested builds (a build script inside a held build) never take it twice: a second shared try would queue
    # behind a waiting perf run that waits for the first.
    if os.environ.get('FABLE2_BUILD_ROLE') == 'play' or os.environ.get('FABLE2_BUILD_HELD') == '1' or \
            perf_shared_skip_reason():
        yield None
        return
    hold = shared_lock('build %s (%s)' % (what, SLOT or 'main'))
    os.environ['FABLE2_BUILD_HELD'] = '1'
    record = LOCKS / 'perf-shared' / ('%d.json' % os.getpid())
    if hold.fd is not None:
        _atomic_json(record, dict(pid=os.getpid(), kind='build', what=what, checkout=SLOT or 'main', game_pid=None,
                                  session=None, waited_s=hold.wait, since=time.time()))
    try:
        yield hold.wait
    finally:
        os.environ.pop('FABLE2_BUILD_HELD', None)
        record.unlink(missing_ok=True)
        hold.close()


@contextlib.contextmanager
def build_marker(jobs, what='build'):
    """Registers a running build in out/locks/builds/ (resources, build_jobs staggering), after the shared perf hold
    (build_perf_hold) and a heavy-tool check (wait_heavy_idle)."""
    path = BUILDS / ('%s-%d.json' % (SLOT or 'main', os.getpid()))
    with build_perf_hold(what) as perf_wait:
        heavy_fd = wait_heavy_idle(what)
        try:
            _atomic_json(path, dict(pid=os.getpid(), checkout=SLOT or 'main', root=str(ROOT), jobs=jobs, since=time.time(),
                                    what=what, role=os.environ.get('FABLE2_BUILD_ROLE') or 'agent',
                                    perf_lock_shared_wait_s=perf_wait))
        finally:
            os.close(heavy_fd)
        try:
            yield
        finally:
            path.unlink(missing_ok=True)


def _build_hold_main(argv):
    """`fable2_common.py build-hold --what NAME -- CMD ...`: run CMD as a build (build_marker: shared perf hold,
    heavy check, build marker); used by shell build scripts (tools/macos/build_dev_launcher.sh). Sets
    FABLE2_BUILD_HELD=1 for CMD."""
    import argparse
    ap = argparse.ArgumentParser(prog='fable2_common.py build-hold')
    ap.add_argument('--what', default='build')
    ap.add_argument('cmd', nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    cmd = a.cmd[1:] if a.cmd[:1] == ['--'] else a.cmd
    if not cmd: ap.error('no command')
    with build_marker(1, a.what):
        return subprocess.call(cmd, env=dict(os.environ, FABLE2_BUILD_HELD='1'))


def build_jobs(requested):
    """Stagger parallel builds: with N other builds running, use requested // (N + 1) jobs (at least 2).
    FABLE2_BUILD_STAGGER=0 keeps the requested count."""
    others = [b for b in running_builds() if b['pid'] != os.getpid()]
    if not others or os.environ.get('FABLE2_BUILD_STAGGER') == '0': return requested, others
    return max(2, requested // (len(others) + 1)), others


if __name__ == '__main__':
    if sys.argv[1:2] == ['hold']: sys.exit(_hold_main(sys.argv[2:]))
    if sys.argv[1:2] == ['build-hold']: sys.exit(_build_hold_main(sys.argv[2:]))
    sys.exit('usage: fable2_common.py hold --fd N --info JSON (internal; started by test launches)')
