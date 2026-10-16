#!/usr/bin/env python3
"""Agent worktree slots: persistent git worktrees for agents working in parallel
(described in the local agent docs, which are not in the public repo).

  python3 tools/fable2_slots.py acquire --task "fix X" [--base main] [--branch NAME] [--owner WHO] [--json]
  python3 tools/fable2_slots.py release agent-N [--delete-branch] [--discard]
  python3 tools/fable2_slots.py status [--json]
  python3 tools/fable2_slots.py reset agent-N [--base main] [--force]
  python3 tools/fable2_slots.py prune [--days 7] [--dry-run]
  python3 tools/fable2_slots.py resources [--json]
  python3 tools/fable2_slots.py config [--slots N]

Slots live outside the main checkout in <workspace>/agent-slots/agent-1..N (FABLE2_AGENT_SLOTS_DIR overrides).
The number of slots is a setting: FABLE2_AGENT_SLOTS, else `config --slots N` (agent-slots/config.json), else 3.
A slot folder and its build folders persist between tasks, so builds stay incremental; the branch does not:
`acquire` puts the slot on a new branch from --base (default main), `release` detaches it and deletes the branch
when it is merged into the base (or with --delete-branch).

Game content and other big ignored inputs are APFS clones of the main checkout's (copy-on-write: ~0 disk until a
side writes; a slot can regenerate or restage its own copy): data/, default.xex, nxeart, $SystemUpdate, generated/,
out/skipintro, out/downloads, out/sdk, out/sdk-src (the source-built SDK; its fingerprint matches, so no rebuild)
and out/metal-build/dependencies (the pinned DXC build tree is relocated to the slot's path with its timestamps
kept, so it is not rebuilt). The game build (out/build) and the Metal backend (out/metal-build/backend) are built
fresh in each slot. thirdparty/rexglue-sdk (the in-place patched submodule) is not initialised in slots; the macOS
build uses out/sdk-src. The private agent docs (docs-private, gitignored, main checkout only) are symlinked in.
Slot records: agent-slots/agent-N.json; every change runs under the fcntl lock agent-slots/.slots.lock.
Runs under /usr/bin/python3 (3.9).
"""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_common as common  # noqa: E402

MAIN = common.MAIN_ROOT
SLOTS_DIR = common.SLOTS_DIR
CONFIG = SLOTS_DIR / 'config.json'
DEFAULT_SLOTS = 3
DEFAULT_BASE = 'main'
SLOT_RE = re.compile(r'^agent-(\d+)$')
# Ignored inputs cloned from the main checkout into every slot (relative paths).
SEED = ('data', 'default.xex', 'nxeart', '$SystemUpdate', 'generated', 'out/skipintro', 'out/downloads', 'out/sdk',
        'out/sdk-src', 'out/metal-build/dependencies')
SEED_SKIP = {'out/sdk-src/build'}  # a stale CMake tree for main's paths; the SDK build recreates it when needed
# Pinned dependency build trees whose absolute paths are rewritten from the main checkout to the slot.
RELOCATE = ('out/metal-build/dependencies/build-dxilconv', 'out/sdk-src/mac-arm64')
# Per-slot build products and evidence (wiped by prune / low-disk auto-clean of idle slots).
BUILD_DIRS = ('out/build', 'out/metal-build/backend')
EVIDENCE_DIRS = ('out/dev-sessions', 'out/validation')
OWN_DIRS = BUILD_DIRS + EVIDENCE_DIRS + ('out/save-library',)
# Private agent workflow docs: a gitignored folder in the main checkout only, symlinked into every slot by seed().
PRIVATE_DOCS = 'docs-private'


def slot_count():
    env = os.environ.get('FABLE2_AGENT_SLOTS')
    if env:
        return max(1, int(env))
    try:
        return max(1, int(json.loads(CONFIG.read_text()).get('slots', DEFAULT_SLOTS)))
    except (OSError, ValueError):
        return DEFAULT_SLOTS


def git(*args, cwd=MAIN, check=True):
    result = subprocess.run(['git'] + [str(a) for a in args], cwd=str(cwd), capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError('git %s failed: %s' % (' '.join(map(str, args)), (result.stderr or result.stdout).strip()))
    return result.stdout.strip()


@contextlib.contextmanager
def state_lock():
    SLOTS_DIR.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(SLOTS_DIR / '.slots.lock'), os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


def slot_path(name):
    return SLOTS_DIR / name


def record_path(name):
    return SLOTS_DIR / (name + '.json')


def read_record(name):
    try:
        return json.loads(record_path(name).read_text())
    except (OSError, ValueError):
        return dict(slot=name, task=None)


def write_record(name, record):
    common._atomic_json(record_path(name), record)


def slot_names(include_extra=True):
    names = {'agent-%d' % i for i in range(1, slot_count() + 1)}
    if include_extra and SLOTS_DIR.is_dir():
        for p in SLOTS_DIR.iterdir():
            if SLOT_RE.match(p.name) and p.is_dir(): names.add(p.name)
    return sorted(names, key=lambda n: int(SLOT_RE.match(n).group(1)))


def resolve_slot(arg):
    name = arg if SLOT_RE.match(arg) else ('agent-' + arg if arg.isdigit() else Path(arg).resolve().name)
    if not SLOT_RE.match(name):
        raise ValueError('Not a slot: %s (expected agent-N)' % arg)
    return name


def dirty(path):
    """Tracked or untracked (not ignored) changes in a slot worktree."""
    return git('status', '--porcelain', '--untracked-files=normal', '--ignore-submodules=all', cwd=path)


def slot_games(name):
    """Running instances started from this slot (its app or its sessions)."""
    import fable2_instances as instances
    prefix = str(slot_path(name)) + '/'
    return [e for e in instances.instances()
            if any(str(e.get(k) or '').startswith(prefix) for k in ('exe', 'app', 'session'))]


def slot_builds(name):
    return [b for b in common.running_builds() if b.get('checkout') == name]


def busy_reason(name):
    games, builds = slot_games(name), slot_builds(name)
    if games: return 'game running: ' + ', '.join('pid %d' % e['pid'] for e in games)
    if builds: return 'build running: pid %d' % builds[0]['pid']
    return None


# ---------------------------------------------------------------- seeding

def relocate(tree, old, new):
    """Rewrite absolute paths old -> new in the text files and symlinks of a build tree, keeping timestamps
    (a CMake/Make tree then neither reconfigures nor rebuilds). Returns the number of files changed."""
    old_b, new_b, changed = str(old).encode(), str(new).encode(), 0
    for folder, dirs, files in os.walk(str(tree)):
        for entry in files + dirs:
            path = os.path.join(folder, entry)
            if os.path.islink(path):
                target = os.readlink(path)
                if target.startswith(str(old) + '/') or target == str(old):
                    os.unlink(path)
                    os.symlink(str(new) + target[len(str(old)):], path)
                    changed += 1
                continue
            if entry in dirs: continue
            try:
                st = os.stat(path)
                if st.st_size > 32 * 1024 * 1024: continue
                with open(path, 'rb') as f: data = f.read()
            except OSError:
                continue
            if old_b not in data or b'\0' in data[:8192]: continue  # binaries keep main's paths (debug info only)
            with open(path, 'wb') as f: f.write(data.replace(old_b, new_b))
            os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
            changed += 1
    return changed


def seed(path, refresh=False, log=print):
    """Clone the main checkout's ignored inputs into a slot (missing items, or all with refresh=True)."""
    done = []
    for rel in SEED:
        src, dst = MAIN / rel, path / rel
        if not src.exists():
            log('seed: %s missing in the main checkout; skipped' % rel)
            continue
        if dst.exists() or dst.is_symlink():
            if not refresh: continue
            shutil.rmtree(dst) if dst.is_dir() and not dst.is_symlink() else dst.unlink()
        if any(s.startswith(rel + '/') for s in SEED_SKIP):
            dst.mkdir(parents=True, exist_ok=True)
            for child in src.iterdir():
                if '%s/%s' % (rel, child.name) not in SEED_SKIP: common.clone(child, dst / child.name)
        else:
            common.clone(src, dst)
        done.append(rel)
    for rel in RELOCATE:
        if (path / rel).exists() and any(rel == d or rel.startswith(d + '/') for d in done):
            n = relocate(path / rel, MAIN, path)
            log('seed: relocated %d file(s) in %s' % (n, rel))
    (path / 'Screenshots').mkdir(exist_ok=True)
    link_private_docs(path, log=log)
    return done


def link_private_docs(path, log=print):
    """Symlink the main checkout's private agent docs (PRIVATE_DOCS, gitignored, never in git) into a slot, so agents
    there read and update the one copy. Replaces a stale link; leaves a real folder alone."""
    src, dst = MAIN / PRIVATE_DOCS, path / PRIVATE_DOCS
    if not src.is_dir():
        log('seed: %s missing in the main checkout; not linked' % PRIVATE_DOCS)
        return False
    if dst.is_symlink():
        if os.readlink(str(dst)) == str(src): return True
        dst.unlink()
    elif dst.exists():
        log('seed: %s is a real folder in %s; left as is' % (PRIVATE_DOCS, path))
        return False
    os.symlink(str(src), str(dst))
    return True


# ---------------------------------------------------------------- commands

def _owner(owner):
    if owner: return owner
    if os.environ.get('FABLE2_AGENT_OWNER'): return os.environ['FABLE2_AGENT_OWNER']
    try:
        parent = subprocess.run(['ps', '-o', 'command=', '-p', str(os.getppid())], capture_output=True,
                                text=True).stdout.strip()
    except OSError:
        parent = ''
    return 'pid %d (%s)' % (os.getppid(), parent[:80])


def _slug(text):
    return re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')[:32] or 'task'


def acquire(task, base=DEFAULT_BASE, branch=None, owner=None, log=print):
    if not task: raise ValueError('--task is required')
    git('rev-parse', '--verify', base + '^{commit}')
    with state_lock():
        free, empty = [], []
        for name in slot_names(include_extra=False):
            rec, path = read_record(name), slot_path(name)
            if rec.get('task'): continue
            if (path / '.git').exists(): free.append((rec.get('last_used') or 0, name))
            else: empty.append(name)
        if not free and not empty:
            raise RuntimeError('All %d slots are busy (%s). Wait, or raise the count: fable2_slots.py config --slots N'
                               % (slot_count(), '; '.join('%s: %s' % (n, read_record(n).get('task'))
                                                         for n in slot_names(False))))
        # Prefer the most recently used existing slot (warmest build), else create the next one.
        name = sorted(free, reverse=True)[0][1] if free else empty[0]
        path = slot_path(name)
        branch = branch or '%s/%s-%s' % (name, time.strftime('%Y%m%d-%H%M'), _slug(task))
        started = time.time()
        common.ensure_free_space('acquiring slot ' + name)
        created = False
        if not (path / '.git').exists():
            if path.exists() and any(path.iterdir()):
                raise RuntimeError('%s exists but is not a worktree; move it away first' % path)
            git('worktree', 'prune')
            git('worktree', 'add', '--detach', path, base)
            created = True
        else:
            changes = dirty(path)
            if changes:
                raise RuntimeError('%s has uncommitted changes from an earlier task; run fable2_slots.py reset %s '
                                   '--force to discard them:\n%s' % (name, name, changes))
        seeded = seed(path, log=log)
        if git('rev-parse', '--verify', '--quiet', 'refs/heads/' + branch, check=False):
            git('checkout', branch, cwd=path)  # resume an existing branch
        else:
            git('checkout', '-b', branch, base, cwd=path)
        rec = read_record(name)
        rec.update(slot=name, path=str(path), task=task, owner=_owner(owner), branch=branch, base=base,
                   acquired=time.strftime('%Y-%m-%d %H:%M:%S'), acquired_unix=time.time(), last_used=time.time(),
                   created=rec.get('created') or (time.strftime('%Y-%m-%d %H:%M:%S') if created else None))
        rec.pop('released', None)
        write_record(name, rec)
    return dict(slot=name, path=str(path), branch=branch, base=base, created=created, seeded=seeded,
                seconds=round(time.time() - started, 1))


def _merged(branch, base, cwd):
    return subprocess.run(['git', 'merge-base', '--is-ancestor', branch, base], cwd=str(cwd)).returncode == 0


def release(name, delete_branch=False, discard=False, log=print):
    name = resolve_slot(name)
    path = slot_path(name)
    with state_lock():
        rec = read_record(name)
        if not (path / '.git').exists(): raise RuntimeError('%s has no worktree' % name)
        reason = busy_reason(name)
        if reason: raise RuntimeError('%s is in use (%s); close/finish it first' % (name, reason))
        changes = dirty(path)
        if changes and not discard:
            raise RuntimeError('%s has uncommitted changes; commit them on the branch for main to merge, or pass '
                               '--discard:\n%s' % (name, changes))
        if changes:
            git('reset', '--hard', cwd=path)
            git('clean', '-fd', cwd=path)  # untracked files only; ignored build folders and clones stay
        branch = git('symbolic-ref', '--quiet', '--short', 'HEAD', cwd=path, check=False)
        git('checkout', '--detach', cwd=path)
        deleted = None
        base = rec.get('base') or DEFAULT_BASE
        if branch and branch == rec.get('branch'):
            if delete_branch or _merged(branch, base, path):
                git('branch', '-D', branch, cwd=path)
                deleted = branch
        rec.update(task=None, owner=None, branch=None, released=time.strftime('%Y-%m-%d %H:%M:%S'), last_used=time.time(),
                   last_branch=branch, last_branch_deleted=bool(deleted))
        write_record(name, rec)
    return dict(slot=name, branch=branch, deleted_branch=deleted,
                kept_branch=None if deleted else (branch or None), discarded=bool(changes))


def reset(name, base=DEFAULT_BASE, force=False):
    name = resolve_slot(name)
    path = slot_path(name)
    rec = read_record(name)
    if rec.get('task') and not force:
        raise RuntimeError('%s is acquired for "%s"; pass --force to reset it anyway' % (name, rec['task']))
    if dirty(path) and not force:
        raise RuntimeError('%s has uncommitted changes; pass --force to discard them' % name)
    result = release(name, delete_branch=force, discard=force)
    with state_lock():
        git('checkout', '--detach', base, cwd=path)
    result['head'] = git('rev-parse', '--short', 'HEAD', cwd=path)
    return result


def du_gb(path):
    if not Path(path).exists(): return 0.0
    out = subprocess.run(['du', '-sk', str(path)], capture_output=True, text=True).stdout.split()
    return round(int(out[0]) / 1024 / 1024, 2) if out else 0.0


def wipe_builds(name, include_evidence=True):
    path = slot_path(name)
    freed = 0.0
    for rel in BUILD_DIRS + (EVIDENCE_DIRS if include_evidence else ()):
        target = path / rel
        if target.exists():
            freed += du_gb(target)
            common_path = Path(os.path.abspath(target))
            if path not in common_path.parents: raise ValueError('refusing to delete outside the slot: %s' % target)
            shutil.rmtree(target, ignore_errors=True)
    return round(freed, 2)


def idle_hours(rec):
    return (time.time() - (rec.get('last_used') or 0)) / 3600


def wipe_idle_builds(min_idle_hours=0, reason='', dry_run=False):
    """Wipe build folders + evidence of slots that are free (no task, nothing running) and idle long enough."""
    rows = []
    for name in slot_names():
        rec, path = read_record(name), slot_path(name)
        if rec.get('task') or not (path / '.git').exists() or busy_reason(name): continue
        if idle_hours(rec) < min_idle_hours: continue
        if not any((path / rel).exists() for rel in BUILD_DIRS + EVIDENCE_DIRS): continue
        rows.append(dict(slot=name, freed_gb=0.0 if dry_run else wipe_builds(name), reason=reason))
    return rows


def remove_slot(name):
    path = slot_path(name)
    git('worktree', 'remove', '--force', path)
    git('worktree', 'prune')
    record_path(name).unlink(missing_ok=True)


def prune(days=7, remove_days=30, dry_run=False):
    """Idle slots: older than `days` -> wipe build folders and evidence; older than `remove_days`, or beyond the
    configured slot count -> remove the worktree. Also prunes each slot's sessions and validation by the
    normal rules. Busy slots (task, game or build) are never touched."""
    import fable2_dev_session as dev
    result = dict(wiped=[], removed=[], sessions={}, validation={}, dry_run=dry_run)
    with state_lock():
        for name in slot_names():
            rec, path = read_record(name), slot_path(name)
            if not (path / '.git').exists(): continue
            n = int(SLOT_RE.match(name).group(1))
            if not (rec.get('task') or busy_reason(name)):
                idle = idle_hours(rec) / 24
                if n > slot_count() or idle >= remove_days:
                    if not dry_run: remove_slot(name)
                    result['removed'].append(dict(slot=name, idle_days=round(idle, 1),
                                                  why='beyond slot count' if n > slot_count() else 'idle'))
                    continue
                if idle >= days and any((path / rel).exists() for rel in BUILD_DIRS + EVIDENCE_DIRS):
                    result['wiped'].append(dict(slot=name, idle_days=round(idle, 1),
                                                freed_gb=0.0 if dry_run else wipe_builds(name)))
                    continue
            removed = dev.prune_sessions(sessions_dir=path / 'out/dev-sessions', dry_run=dry_run)
            if removed: result['sessions'][name] = len(removed)
            val = common.prune_validation(path, dry_run=dry_run)
            if val: result['validation'][name] = [v['path'] for v in val]
        if not dry_run: git('worktree', 'prune')
    return result


def status():
    rows = []
    for name in slot_names():
        rec, path = read_record(name), slot_path(name)
        exists = (path / '.git').exists()
        row = dict(slot=name, exists=exists, task=rec.get('task'), owner=rec.get('owner'), path=str(path))
        if exists:
            row.update(branch=git('symbolic-ref', '--quiet', '--short', 'HEAD', cwd=path, check=False) or
                       'detached@' + git('rev-parse', '--short', 'HEAD', cwd=path),
                       dirty=bool(dirty(path)), busy=busy_reason(name),
                       acquired=rec.get('acquired') if rec.get('task') else None,
                       idle_hours=None if rec.get('task') else round(idle_hours(rec), 1),
                       built=(path / 'out/build/mac-arm64-metal-release/Fable II.app').is_dir())
        rows.append(row)
    return dict(slots=rows, count=slot_count(), slots_dir=str(SLOTS_DIR))


def resources():
    import fable2_instances as instances
    rows = []
    for name, root in common.checkouts():
        own = {rel: du_gb(root / rel) for rel in OWN_DIRS}
        rows.append(dict(checkout=name, own_gb=round(sum(own.values()), 2), detail={k: v for k, v in own.items() if v},
                         task=None if name == 'main' else read_record(name).get('task')))
    running = instances.instances()
    return dict(free_gb=round(common.free_gb(), 1), min_free_gb=common.min_free_gb(), checkouts=rows,
                games=[dict(pid=e['pid'], role=e['role'], build=e.get('build'), checkout=e.get('checkout'),
                            session=Path(e['session']).name if e.get('session') else None) for e in running],
                max_instances=common.max_instances(), builds=common.running_builds(),
                perf_lock=dict(holder=common.read_holder('perf'), shared=common.shared_holders('perf'),
                               queue=common.queue('perf')),
                launch_queue=common.queue('launch'),
                agent_games=dict(limit=common.agent_game_limit(), limit_3x=common.agent_3x_limit(),
                                 holders=common.game_holders(), queue=common.game_queue(),
                                 yield_to_play=common.yield_to_play(),
                                 play_games=[dict(pid=e['pid'], role=e['role'], build=e.get('build'))
                                             for e in common.play_games(running)]),
                agent_cache=_agent_cache_status(),
                note='own_gb counts each checkout\'s build, session, validation and library folders; cloned inputs '
                     '(data/, generated/, SDK, DXC) share blocks with the main checkout and are not counted')


def _agent_cache_status():
    try:
        import fable2_agent_cache as agent_cache
        return dict(agent_cache.status(), lines=agent_cache.status_lines())
    except Exception as error:
        return dict(error=repr(error), lines=['Agent shader cache: status failed (%r)' % (error,)])


def print_status(st):
    print('slots: %d configured, folder %s' % (st['count'], st['slots_dir']))
    for r in st['slots']:
        if not r['exists']:
            print('  %-8s (not created)' % r['slot'])
            continue
        state = ('BUSY "%s" since %s, %s' % (r['task'], r['acquired'], r['owner']) if r['task']
                 else 'free, idle %.1f h' % r['idle_hours'])
        print('  %-8s %s | %s%s%s%s' % (r['slot'], state, r['branch'], ' DIRTY' if r['dirty'] else '',
                                        ' | ' + r['busy'] if r['busy'] else '', '' if r['built'] else ' | not built'))


def print_resources(res):
    print('disk: %.1f GB free (guard: auto-clean, then refuse builds/test launches, below %.0f GB)'
          % (res['free_gb'], res['min_free_gb']))
    for r in res['checkouts']:
        detail = ', '.join('%s %.1f' % (k.replace('out/', ''), v) for k, v in r['detail'].items())
        print('  %-8s %6.1f GB own  (%s)%s' % (r['checkout'], r['own_gb'], detail or 'nothing built',
                                              '  task: ' + r['task'] if r.get('task') else ''))
    print('  (%s)' % res['note'])
    print('games: %d running (cap %d)' % (len(res['games']), res['max_instances']))
    for g in res['games']:
        print('  pid %d %s build=%s checkout=%s session=%s' % (g['pid'], g['role'], g['build'], g['checkout'], g['session']))
    print('builds: %d running' % len(res['builds']))
    for b in res['builds']:
        print('  %s %s pid %d, %d jobs, %.0f s%s' % (b['checkout'], b.get('what', 'build'), b['pid'], b['jobs'],
                                                 time.time() - b['since'],
                                                 ' (play build: no perf wait)' if b.get('role') == 'play' else ''))
    h = res['perf_lock']['holder']
    shared = res['perf_lock'].get('shared') or []
    print('perf lock: ' + ('held exclusive by %s (pid %s, %s) for %.0f s' % (h['what'], h['pid'], h['checkout'], time.time() - h['since'])
                           if h else 'held shared by %d test game%s' % (len(shared), '' if len(shared) == 1 else 's')
                           if shared else 'free'))
    for r in shared:
        if r.get('kind') == 'build':
            print('  shared: %s (pid %s) for %.0f s' % (r.get('what'), r['pid'], time.time() - r['since']))
            continue
        print('  shared: %s game pid %s (%s, holder pid %s) for %.0f s'
              % (r.get('session') or r.get('what'), r.get('game_pid'), r.get('checkout'), r['pid'], time.time() - r['since']))
    for q in res['perf_lock']['queue']:
        print('  waiting %s: %s (pid %s, %s) for %.0f s' % (q.get('mode', 'exclusive'), q['what'], q['pid'], q['checkout'],
                                                          time.time() - q['since']))
    if res['launch_queue']: print('launch queue: %d waiting' % len(res['launch_queue']))
    ag = res.get('agent_games') or {}
    print('agent games: %d of %d slots held (3x: %d of %d)' % (
        len(ag.get('holders', [])), ag.get('limit', 0),
        sum(1 for h in ag.get('holders', []) if any(n.startswith('3x') for n in h.get('games') or [])), ag.get('limit_3x', 0)))
    for h in ag.get('holders', []):
        print('  %s: %s (%s, %sx, game pid %s, holder pid %s) for %.0f s' % (
            '+'.join(h.get('games') or []), h.get('session') or h.get('what'), h.get('checkout'), h.get('scale'),
            h.get('game_pid'), h['pid'], time.time() - h['since']))
    play = ag.get('play_games') or []
    if play:
        print('  A play game is running (%s): new agent games wait until it exits%s' % (
            ', '.join('pid %s %s' % (p['pid'], p['role']) for p in play),
            '' if ag.get('yield_to_play', True) else ' - OFF (FABLE2_YIELD_TO_PLAY=0)'))
    for i, q in enumerate(ag.get('queue', [])):
        print('  queued %d: %s (%s, %sx, pid %s) for %.0f s%s' % (i + 1, q['what'], q['checkout'], q.get('scale'), q['pid'],
                                                               time.time() - q['since'],
                                                               ' - waiting for the play session' if q.get('play_wait_since') else ''))
    for line in (res.get('agent_cache') or {}).get('lines', []):
        print(line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    q = sub.add_parser('acquire')
    q.add_argument('--task', required=True)
    q.add_argument('--base', default=DEFAULT_BASE)
    q.add_argument('--branch')
    q.add_argument('--owner')
    q.add_argument('--json', action='store_true')
    q = sub.add_parser('release')
    q.add_argument('slot')
    q.add_argument('--delete-branch', action='store_true', help='drop the branch even if it is not merged')
    q.add_argument('--discard', action='store_true', help='discard uncommitted changes')
    q = sub.add_parser('reset')
    q.add_argument('slot')
    q.add_argument('--base', default=DEFAULT_BASE)
    q.add_argument('--force', action='store_true', help='also when acquired or dirty (discards changes and the branch)')
    q = sub.add_parser('status')
    q.add_argument('--json', action='store_true')
    q = sub.add_parser('prune')
    q.add_argument('--days', type=float, default=7)
    q.add_argument('--remove-days', type=float, default=30)
    q.add_argument('--dry-run', action='store_true')
    q = sub.add_parser('resources')
    q.add_argument('--json', action='store_true')
    q = sub.add_parser('config')
    q.add_argument('--slots', type=int)
    args = ap.parse_args()
    try:
        if args.cmd == 'acquire':
            result = acquire(args.task, args.base, args.branch, args.owner,
                             log=(lambda t: print(t, file=sys.stderr)))
            if not args.json:
                print(result['path'])
                print('slot %s on branch %s (from %s)%s' % (result['slot'], result['branch'], result['base'],
                                                             ', created' if result['created'] else ''), file=sys.stderr)
                return 0
        elif args.cmd == 'release': result = release(args.slot, args.delete_branch, args.discard)
        elif args.cmd == 'reset': result = reset(args.slot, args.base, args.force)
        elif args.cmd == 'status':
            result = status()
            if not args.json:
                print_status(result)
                return 0
        elif args.cmd == 'prune': result = prune(args.days, args.remove_days, args.dry_run)
        elif args.cmd == 'resources':
            result = resources()
            if not args.json:
                print_resources(result)
                return 0
        else:
            if args.slots is not None:
                if args.slots < 1: raise ValueError('--slots must be at least 1')
                common._atomic_json(CONFIG, dict(slots=args.slots))
            result = dict(slots=slot_count(), source='FABLE2_AGENT_SLOTS' if os.environ.get('FABLE2_AGENT_SLOTS')
                          else ('config.json' if CONFIG.is_file() else 'default'))
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps({'ok': True, 'result': result}, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
