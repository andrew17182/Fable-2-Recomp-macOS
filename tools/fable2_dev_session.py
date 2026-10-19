#!/usr/bin/env python3
"""Dev sessions: boot straight into a save, and write game saves back.

Used by the Fable II Launcher app (tools/macos/DevLauncher) and usable from the CLI.

  list                       live slots + save-library entries (+ running game) as JSON
  launch [--build playtest|latest] ...   which app: the frozen playtest copy (out/playtest, see
                             tools/fable2_playtest.py) or the latest build (default: latest)
  launch --mode title        normal launch on the live state folder (no copying)
  launch --mode live --save Hero000 [--target-slot Hero000|none]
  launch --mode library --save hero1-dog-live [--target-slot Hero000|none]
         [--no-skip-intro] [--no-boot] [--test-control]
         [--game-arg=--metal_pipeline_creation_threads=4 ...] [--cache-from DIR]   (test sessions only)
  status                     the launcher's current game / last write-back / all running instances
  close SESSION              close the game of one session and wait for its process to exit
  close --current            close the launcher's current (normal play) game
                             (only ever the one game; other instances are never touched)
  watch SESSION              (internal) save watcher, started detached by `launch`

A save session copies the live state folder's saves/cache/settings into
out/dev-sessions/<id>/test-state, keeps only the picked hero save (so Continue loads it
directly) and launches the app with FABLE2_APP_STATE_ROOT pointing there, plus
FABLE2_BOOT_SAVE / FABLE2_SKIP_INTRO_MOVIES. The layout matches the test controller's,
so `fable2_test_control.py capture|press|close <session>` work on a session launched
with --test-control.

Write-back: a detached host-side watcher polls the session's hero save files. When the
game writes a save (in-game Save, F5 quick save or an autosave) and the files have been
unchanged for STABLE_SECONDS and none is still open by the game (lsof), it
  1. copies the save into the library as <base>_<YYYY-MM-DD_HH-MM-SS>, and
  2. if the session has a target slot, backs up that live slot to out/save-backups/ and
     copies the save over it.
Nothing is copied because the game closes or crashes (a change still pending when the
process exits is discarded and logged), and title-mode launches are never watched.

Test-session launch parameters (--test-control only; normal play is unchanged):
  --game-arg ARG   extra game argument / cvar after the usual ones (repeatable), e.g.
                   --game-arg=--metal_pipeline_creation_threads=4
  --cache-from DIR replace the session's copied cache/ with DIR (a warm or cold shader-cache seed)
  --cache-snapshot auto|readonly|cold|off|KEY@GEN|PIN   the agent shader cache (tools/fable2_agent_cache.py;
                   default auto, or FABLE2_AGENT_CACHE): test sessions seed cache/ from MAIN/out/agent-cache/<key>
                   (a fingerprint of the shader-generation code) instead of the live cache, and after a clean exit
                   publish their new artifacts back there (publishable builds only). Recorded as session.json
                   agent_cache; the watcher logs an `agent-cache` event. Normal play is unchanged.
Flight recorder: the session's trace/ folder (test-state/logs/trace) gets tags-in.txt with the
booted save's LevelName/QuestName, and the watcher adds the values of every save it copies.

Shader cache: after a normal (non-test) session exits cleanly, shader/pipeline cache files
the session added are copied back to the live cache (append-only check, previous cache
kept in out/save-backups/cache-previous), so later sessions don't recompile them.
Multiple instances (local agent docs): each launch registers its own pid in
out/instances/ (tools/fable2_instances.py); launch/close/watch act only on that pid. Only one
normal-play game at a time (a second one is refused); test sessions may run beside it. The
session's concurrent instances go into session.json (concurrent_instances), instances.jsonl
(changes, written by the watcher) and the flight-recorder 'contention' tag.
Test sessions never write saves back to a live slot (target slot is always none).
Perf lock (local agent docs): --test-control launches hold the global perf lock shared for the game's
life (a detached `fable2_common.py hold` process), waiting while a perf run holds or waits for it; the wait
is session.json perf_lock_shared_wait_s. Normal play never takes it.
Agent game limit: --test-control launches then take one of FABLE2_AGENT_GAMES (2) game slots (and one of
FABLE2_AGENT_3X_GAMES (1) at 3x) for the game's life, queueing first come first served; session.json
agent_game_slots / agent_game_wait_s. Normal play never waits for one and never counts.
Window position: after a normal session exits (cleanly or not), a valid changed
fable2_window.toml (position, and size when present) is copied back to the live state, so the
next session opens where and as large as you left it. Test sessions (--test-control) instead seed and
publish their own placement (tools/fable2_test_windows.py; FABLE2_TEST_WINDOW_MEMORY=0 turns that off), so a
test window never moves the play window.
"""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_test_control as control  # noqa: E402  (shared save-library helpers)
import fable2_instances as instances  # noqa: E402  (running-instance registry)
import fable2_playtest as playtest  # noqa: E402  (frozen playtest copy paths)
import fable2_skip_intro as skip_intro_movies  # noqa: E402  (out/skipintro stand-ins)
import fable2_common as common  # noqa: E402  (main checkout vs agent slot, disk/instance guards)
import fable2_test_windows as test_windows  # noqa: E402  (remembered test-window placement)
import fable2_agent_cache as agent_cache  # noqa: E402  (shared warm shader cache of agent test sessions)
import fable2_save_picker as save_picker  # noqa: E402  (readable library titles, test-save flag)

ROOT = control.ROOT
APP = control.APP
LIBRARY = control.SAVE_LIBRARY
SESSIONS = ROOT / 'out/dev-sessions'
BACKUPS = ROOT / 'out/save-backups'
CURRENT = SESSIONS / 'current.json'
LIVE_STATE = Path(os.environ.get('FABLE2_LIVE_STATE_ROOT') or
                  Path.home() / 'Library/Application Support/Fable II Recomp')
STAMP_RE = re.compile(r'_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d(-\d+)?$')
HERO_RE = re.compile(r'^Hero\d{3}$')
STABLE_SECONDS = 3.0
POLL_SECONDS = 0.5
INSTANCE_POLL_SECONDS = 5.0
# PPC routine 824E7990 writes this exact 40-byte options blob; SDK BinarySetting
# persists payload bytes directly, with no extra header. Other title blobs and
# achievements are deliberately outside this preference write-back.
OPTIONS_FILE = Path('saves/4D5307F1/profile/User/63E83FFF')
OPTIONS_SIZE = 40


def options_signature(state):
    return options_path_signature(Path(state) / OPTIONS_FILE)


def options_path_signature(path):
    path = Path(path)
    try:
        raw = path.read_bytes()
        st = path.stat()
        return dict(size=len(raw), mtime_ns=st.st_mtime_ns,
                    sha256=hashlib.sha256(raw).hexdigest())
    except FileNotFoundError:
        return None


def normal_options_writeback(test_control, env):
    return not (test_control or env.get('FABLE2_TEST_BACKGROUND') == '1' or
                env.get('FABLE2_TEST_CONTROL_DIR'))


def publish_options(source, live_state, expected, reason, source_expected=None):
    """Publish one complete options file; reject changes to normal preferences."""
    source, live_state = Path(source), Path(live_state)
    before = options_path_signature(source)
    if source_expected is not None and before != source_expected:
        raise OSError("Options changed after stability check; retrying")
    raw = source.read_bytes()
    if options_path_signature(source) != before or hashlib.sha256(raw).hexdigest() != before["sha256"]:
        raise OSError("Options changed during read; retrying")
    if len(raw) != OPTIONS_SIZE:
        raise ValueError('Incomplete/unsupported options profile (expected 40 bytes)')
    current = options_signature(live_state)
    incoming_hash = hashlib.sha256(raw).hexdigest()
    if current and current['sha256'] == incoming_hash:
        return None
    if current != expected:
        raise RuntimeError('Normal options changed since this session started; retaining newer preferences')
    backup = BACKUPS / (now_stamp() + '-options-' + uuid.uuid4().hex[:8])
    backup.mkdir(parents=True)
    dest = live_state / OPTIONS_FILE
    if dest.exists(): shutil.copy2(dest, backup / OPTIONS_FILE.name)
    (backup / 'backup.json').write_text(json.dumps(dict(reason=reason, source=str(source),
        destination=str(dest), previous=current), indent=2) + '\n')
    dest.parent.mkdir(parents=True, exist_ok=True)
    temporary = dest.with_name(dest.name + '.tmp-' + uuid.uuid4().hex)
    try:
        with open(temporary, 'wb') as out:
            out.write(raw)
            out.flush()
            os.fsync(out.fileno())
        if options_signature(live_state) != current:
            raise RuntimeError('Normal options changed during publish; retaining newer preferences')
        if options_path_signature(source) != before:
            raise OSError('Options changed during publish; retrying')
        os.replace(temporary, dest)
    finally:
        temporary.unlink(missing_ok=True)
    return str(backup)


def commit_options(session, record, log, source_expected=None):
    if not record.get('options_writeback', False):
        log(dict(event='options-isolated'))
        return
    backup = publish_options(Path(record['state']) / OPTIONS_FILE,
                             record['live_state'], record.get('options_live_signature'),
                             'dev-session options write-back', source_expected)
    record['options_live_signature'] = options_signature(record['live_state'])
    log(dict(event='options-saved', backup=backup, file=str(OPTIONS_FILE)))


def recover_completed_options(session):
    """Explicit recovery of one verified completed normal-play session."""
    session = Path(session)
    record = json.loads((session / 'session.json').read_text())
    if pid_alive(record['pid']) or [e for e in instances.instances() if e['role'] != 'agent']:
        raise RuntimeError('Close the game before recovering options')
    if record.get('test_control') is not False or not normal_options_writeback(False, record.get('env', {})):
        raise ValueError('Cannot recover options from an automated session')
    events = [json.loads(x) for x in (session / 'writeback.jsonl').read_text().splitlines()]
    ended = [x['time'] for x in events if x.get('event') == 'game-exited']
    if not ended: raise ValueError('Session has no completed exit record')
    source = Path(record['state']) / OPTIONS_FILE
    sig, current = options_signature(record['state']), options_signature(LIVE_STATE)
    end_time = time.mktime(time.strptime(ended[-1], '%Y-%m-%d %H:%M:%S'))
    if not sig or sig['size'] != OPTIONS_SIZE or sig['mtime_ns'] / 1e9 + STABLE_SECONDS > end_time:
        raise ValueError('Options were incomplete or still pending at exit')
    if current and sig['mtime_ns'] <= current['mtime_ns']: return None
    backup = publish_options(source, LIVE_STATE, current, 'recover completed normal-play options')
    return dict(source=str(source), backup=backup)


# Window-position write-back. Sessions start from a copy of the live fable2_window.toml and
# the game saves moves into that copy (atomically: temp file + rename, on each move), so
# without write-back every Dev Launcher session reopens at the last position from a title-mode
# launch. Published once after the game exits, for normal sessions only, and also after a
# crash: the game's own atomic rename means the session file is always a complete earlier
# save, and it must still parse as exactly `x`/`y` (int32), optionally with `width`/`height`
# (1..16384, the game's own limit; files from before size memory have only x/y).
WINDOW_FILE = Path(control.WINDOW_POSITION)
INT32 = range(-2**31, 2**31)
WINDOW_SIZE = range(1, 16384 + 1)


def normal_window_writeback(test_control, env):
    return normal_options_writeback(test_control, env)


def window_signature(state):
    return options_path_signature(Path(state) / WINDOW_FILE)


def parse_window_position(raw):
    """dict(x, y[, width, height]) when raw is a TOML file with exactly the integer keys
    {x, y} or {x, y, width, height} in range, else None."""
    # The Dev Launcher runs this script with the system Python (3.9), which has no tomllib, so
    # parse the game's flat format directly: comments, blank lines and exactly one integer
    # assignment per key.
    try:
        text = raw.decode('utf-8')
    except UnicodeDecodeError:
        return None
    table = {}
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if not line: continue
        m = re.fullmatch(r'([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([+-]?\d+)', line)
        if not m or m.group(1) in table: return None
        table[m.group(1)] = int(m.group(2))
    if set(table) not in ({'x', 'y'}, {'x', 'y', 'width', 'height'}): return None
    if table['x'] not in INT32 or table['y'] not in INT32: return None
    if 'width' in table and (table['width'] not in WINDOW_SIZE or table['height'] not in WINDOW_SIZE):
        return None
    return table


def publish_window_position(state, live_state, expected):
    """Copy the session window position to the live state. Returns the event to log."""
    source, dest = Path(state) / WINDOW_FILE, Path(live_state) / WINDOW_FILE
    before = options_path_signature(source)
    if before is None: return dict(event='window-position-unchanged', reason='no session window position')
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != before['sha256']: raise OSError('Window position changed during read')
    position = parse_window_position(raw)
    if position is None: return dict(event='window-position-invalid', file=str(WINDOW_FILE))
    current = window_signature(live_state)
    if current and current['sha256'] == before['sha256']:
        return dict(event='window-position-unchanged', **position)
    if current != expected:
        raise RuntimeError('Live window position changed since this session started; keeping it')
    dest.parent.mkdir(parents=True, exist_ok=True)
    temporary = dest.with_name(dest.name + '.tmp-' + uuid.uuid4().hex)
    try:
        with open(temporary, 'wb') as out:
            out.write(raw)
            out.flush()
            os.fsync(out.fileno())
        shutil.copymode(dest if dest.exists() else source, temporary)
        if window_signature(live_state) != current:
            raise RuntimeError('Live window position changed during publish; keeping it')
        os.replace(temporary, dest)
    finally:
        temporary.unlink(missing_ok=True)
    return dict(event='window-position-saved', **position)


def commit_window_position(record, log):
    if not record.get('window_writeback', False):
        log(dict(event='window-position-isolated'))
        return
    try:
        log(publish_window_position(record['state'], record['live_state'], record.get('window_live_signature')))
    except Exception as error:
        log(dict(event='window-position-error', error=repr(error)))


# Shader/pipeline cache write-back. Sessions start from a copy of the live cache; without
# write-back every Dev Launcher session recompiles the same shaders, and while a pipeline
# compiles in the background the renderer skips its draws (seen as solid blue/white
# geometry flashes when the camera turns toward new scenery). The SDK cache files are
# append-only, so a session file is published only if it still begins with the exact
# live file the session started from, the game exited cleanly, and the live cache is
# unchanged since launch (e.g. no title-mode play in between).
CACHE_DIR = Path('cache')
CLEAN_EXIT_MARKER = 'Title terminated; hard-exiting process.'
CACHE_BACKUP_NAME = 'cache-previous'
# Metal pipeline-archive segments (<title>.<...>.metal.segNNNNNN[.full].binarchive). After a
# compaction the game deletes superseded segments at its next launch, so a segment that was
# live at launch but is gone from the session cache at exit is removed from live too.
SEGMENT_RE = re.compile(r'\.metal\.seg[^/]*\.binarchive$')


def is_segment(rel):
    name = Path(rel).name
    return bool(SEGMENT_RE.search(name)) and '.tmp-' not in name


def cache_signature(state):
    root = Path(state) / CACHE_DIR
    result = {}
    if root.is_dir():
        for path in sorted(p for p in root.rglob('*') if p.is_file() and '.tmp-' not in p.name):
            st = path.stat()
            result[str(path.relative_to(root))] = dict(size=st.st_size, mtime_ns=st.st_mtime_ns,
                                                       sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    return result


def normal_cache_writeback(test_control, env):
    # Automated sessions stay isolated. FABLE2_CACHE_WRITEBACK=1 (in this script's own
    # environment) exercises the write-back only against an overridden live-state fixture.
    if normal_options_writeback(test_control, env): return True
    return os.environ.get('FABLE2_CACHE_WRITEBACK') == '1' and bool(os.environ.get('FABLE2_LIVE_STATE_ROOT'))


def clean_exit(state):
    try:
        return CLEAN_EXIT_MARKER in (Path(state) / 'logs/fable_2.log').read_text(errors='replace')
    except OSError:
        return False


def exit_kind(state):
    """How a finished game exited: dict(kind, detail).
      test-control-close  the test channel's `close` (harness / fable2_test_control.py close)
      closed-by-user      clean exit otherwise: window close button, Cmd+Q, Dock Quit, or a SIGTERM
                          (Dev Launcher Close) - not a crash or hang
      unclean             no clean-exit marker in the log: crash, kill, or still exiting"""
    state = Path(state)
    detail = None
    for summary in sorted((state / 'logs/trace/dumps').glob('*-exit/summary.json')):
        try:
            detail = json.loads(summary.read_text()).get('detail')
        except (OSError, ValueError):
            pass
    if detail == 'test-control close': return dict(kind='test-control-close', detail=detail)
    if clean_exit(state):
        return dict(kind='closed-by-user', detail=detail or 'clean quit without a window-close dump (Cmd+Q, Dock Quit or SIGTERM)')
    return dict(kind='unclean', detail='no clean-exit marker in logs/fable_2.log')


def publish_cache(state, live_state, expected):
    """Copy grown/new session cache files to the live cache. Returns a summary dict."""
    source_root, live_root = Path(state) / CACHE_DIR, Path(live_state) / CACHE_DIR
    if expected is None: raise RuntimeError('No live cache signature recorded at launch')
    current = cache_signature(live_state)
    if current != expected:
        raise RuntimeError('Live shader cache changed since this session started; keeping it')
    updates = []
    for rel, sig in cache_signature(state).items():
        old = expected.get(rel)
        if old is None:
            updates.append(rel)
            continue
        if sig['sha256'] == old['sha256']: continue
        with open(source_root / rel, 'rb') as f: prefix = f.read(old['size'])
        if sig['size'] < old['size'] or hashlib.sha256(prefix).hexdigest() != old['sha256']:
            raise RuntimeError(f'Session cache file {rel} is not an extension of the live file; keeping live cache')
        updates.append(rel)
    session_files = cache_signature(state)
    deletions = sorted(rel for rel in expected if is_segment(rel) and rel not in session_files)
    if not updates and not deletions: return dict(updated=[], deleted=[])
    backup = BACKUPS / CACHE_BACKUP_NAME
    if live_root.is_dir():
        if backup.exists(): shutil.rmtree(backup)
        shutil.copytree(live_root, backup)
    for rel in updates:
        dest = live_root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        temporary = dest.with_name(dest.name + '.tmp-' + uuid.uuid4().hex)
        try:
            shutil.copyfile(source_root / rel, temporary)
            with open(temporary, 'rb+') as f: os.fsync(f.fileno())
            live_sig = expected.get(rel)
            if dest.exists() != (live_sig is not None) or (
                    live_sig and (dest.stat().st_size, dest.stat().st_mtime_ns) != (live_sig['size'], live_sig['mtime_ns'])):
                raise RuntimeError(f'Live cache file {rel} changed during publish; stopping')
            os.replace(temporary, dest)
        finally:
            temporary.unlink(missing_ok=True)
    deleted, kept = [], []
    for rel in deletions:  # the whole live cache (these segments included) is already in backup
        dest, live_sig = live_root / rel, expected[rel]
        try:
            st = dest.stat()
            unchanged = ((st.st_size, st.st_mtime_ns) == (live_sig['size'], live_sig['mtime_ns']) and
                         hashlib.sha256(dest.read_bytes()).hexdigest() == live_sig['sha256'])
        except FileNotFoundError:
            continue
        if not unchanged or not (backup / rel).is_file():
            kept.append(rel)
            continue
        dest.unlink()
        deleted.append(rel)
    result = dict(updated=updates, deleted=deleted, backup=str(backup))
    if kept: result['kept_changed'] = kept
    return result


def agent_cache_summary(rec):
    if not rec: return 'not used'
    where = ('%s@%s (content %s)' % (rec['key'], rec['generation'], rec.get('content_hash'))
             if rec.get('key') else rec.get('source'))
    return '%s seed %s; %s (%s)' % (rec['mode'], where, 'publishes on a clean exit' if rec.get('publish')
                                    else 'read-only', rec.get('reason'))


def commit_cache(record, log):
    if record.get('agent_cache'):  # test session: grow the agent shader cache, never the live one
        agent_cache.publish_logged(record['state'], record['agent_cache'], log, Path(record['session']).name)
        return
    if not record.get('cache_writeback', False):
        log(dict(event='cache-isolated'))
        return
    if not clean_exit(record['state']):
        log(dict(event='cache-discarded', reason='game did not exit cleanly'))
        return
    try:
        result = publish_cache(record['state'], record['live_state'], record.get('cache_live_signature'))
        log(dict(event='cache-saved' if result['updated'] or result['deleted'] else 'cache-unchanged', **result))
    except Exception as error:
        log(dict(event='cache-error', error=repr(error)))


def now_stamp():
    return time.strftime('%Y-%m-%d_%H-%M-%S')


def game_pids():
    result = subprocess.run(['pgrep', '-x', 'fable_2'], capture_output=True, text=True)
    if result.returncode not in (0, 1): raise RuntimeError('pgrep failed: ' + result.stderr.strip())
    return [int(p) for p in result.stdout.split()]


def pid_alive(pid):
    result = subprocess.run(['ps', '-p', str(pid), '-o', 'stat='], capture_output=True, text=True)
    return result.returncode == 0 and not result.stdout.strip().startswith('Z')


# ---------------------------------------------------------------- listing

def live_slots(state=LIVE_STATE):
    slots_dir, headers = Path(state) / control.HERO_SLOTS, Path(state) / control.HERO_HEADERS
    slots = []
    for hero in sorted(slots_dir.glob('Hero*')) if slots_dir.is_dir() else []:
        header = headers / (hero.name + '.header')
        if not HERO_RE.match(hero.name) or not header.is_file() or not (hero / 'mainsave.bin').is_file(): continue
        try:
            info = control.describe_save(hero, header)
        except Exception as error:  # a damaged slot must not hide the others
            info = dict(folder=hero.name, error=str(error))
        slots.append(info)
    return slots


def library_entries():
    entries, seen = [], set()
    roles = save_picker.session_roles()
    # This checkout's library, then (in an agent slot) the main checkout's, read-only.
    for meta_path in (m for folder in control.library_dirs() for m in folder.glob('*/save.json')):
        if meta_path.parent.name in seen: continue
        seen.add(meta_path.parent.name)
        try:
            data = json.loads(meta_path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        data.setdefault('name', meta_path.parent.name)
        data['sort_key'] = data.get('created') or data.get('imported') or data.get('saved_mtime') or ''
        entries.append({k: data.get(k) for k in ('name', 'folder', 'header_display_name', 'QuestName', 'LevelName',
                                                 'saved_mtime', 'created', 'imported', 'note', 'origin', 'sort_key',
                                                 'session', 'written_back_to', 'Chapter')})
        save_picker.annotate(entries[-1], roles)  # friendly title, group, test flag (Dev Launcher picker)
    entries.sort(key=lambda e: e['sort_key'], reverse=True)
    return entries


# ---------------------------------------------------------------- sessions

def keep_only_hero(state, hero_dir, header):
    """Make one hero save the only one in a session state (profile/achievements kept)."""
    slots, headers = Path(state) / control.HERO_SLOTS, Path(state) / control.HERO_HEADERS
    for folder in (slots, headers):
        if folder.exists(): shutil.rmtree(folder)
        folder.mkdir(parents=True)
    shutil.copytree(hero_dir, slots / Path(hero_dir).name)
    shutil.copy2(header, headers / Path(header).name)


def library_base(name):
    return STAMP_RE.sub('', name)


SESSION_NAME_RE = re.compile(r'^(\d{8}-\d{6})-[0-9a-f]{8}$')
PRUNE_KEEP_NEWEST = 20
# Agent (test-control) sessions are disposable sooner: their evidence is copied to out/validation.
AGENT_KEEP_HOURS = 24
AGENT_KEEP_NEWEST = 3


def _env_hours(name, default):
    try:
        return float(os.environ.get(name, default))
    except ValueError:
        return float(default)


# Flight-recorder dumps (trace/dumps/NNN-<reason>) decide how long a finished session is kept:
#   the on-close dump (exit; older builds: close) is in every session and keeps nothing;
#   routine diagnostics (shader_miss = persistent miss, frame_gap) keep it FABLE2_SESSION_KEEP_ROUTINE_DUMP_HOURS (48);
#   real faults (first_av, av_storm, hang, gpu_timeout, crash, crash_diag, pipeline_fail, manual `trace dump`
#   reasons) keep it FABLE2_SESSION_KEEP_DUMP_HOURS (168).
ON_CLOSE_DUMPS = ('exit', 'close')
ROUTINE_DUMPS = ('shader_miss', 'frame_gap')


def dump_tier(session):
    """'fault' | 'routine' | None: the most serious flight-recorder dump of a session."""
    dumps = session / 'test-state/logs/trace/dumps'
    tier = None
    try:
        for d in dumps.iterdir():
            if not d.is_dir(): continue
            reason = d.name.split('-', 1)[1] if '-' in d.name else d.name
            if reason in ON_CLOSE_DUMPS or 'close' in reason: continue
            if reason in ROUTINE_DUMPS: tier = tier or 'routine'
            else: return 'fault'
    except OSError:
        pass
    return tier


def _has_fault_dump(session):
    """True if the flight recorder wrote a dump other than the routine on-close one (NNN-exit)."""
    return dump_tier(session) is not None


def _is_agent_session(session):
    """Test-control (agent) session? A folder without session.json is a launch that never started: agent-like."""
    try:
        record = json.loads((session / 'session.json').read_text())
    except (OSError, ValueError):
        return True
    return bool(record.get('test_control') or record.get('env', {}).get('FABLE2_TEST_CONTROL_DIR'))


def prune_sessions(log=None, sessions_dir=None, slot=common.SLOT, dry_run=False):
    """Delete old session folders. Saves are already written back to their slot and the save library, so a
    finished session is disposable. Never deleted: sessions of a running instance (any checkout) and the
    launcher's current one.
      Normal play sessions: older than FABLE2_SESSION_KEEP_HOURS (48), keeping the newest PRUNE_KEEP_NEWEST (20).
      Agent (test-control) sessions: older than FABLE2_AGENT_SESSION_KEEP_HOURS (24), keeping the newest
      FABLE2_AGENT_SESSION_KEEP_NEWEST (3) of this checkout (per slot).
      Either kind with a flight-recorder dump: kept FABLE2_SESSION_KEEP_DUMP_HOURS (168) for a real fault,
      FABLE2_SESSION_KEEP_ROUTINE_DUMP_HOURS (48) for shader_miss/frame_gap (dump_tier).
    FABLE2_SESSION_PRUNE=0 disables. Returns the removed folder names."""
    sessions_dir = Path(sessions_dir) if sessions_dir else SESSIONS
    if os.environ.get('FABLE2_SESSION_PRUNE') == '0' or not sessions_dir.is_dir(): return []
    keep_s = _env_hours('FABLE2_SESSION_KEEP_HOURS', 48) * 3600
    agent_keep_s = _env_hours('FABLE2_AGENT_SESSION_KEEP_HOURS', AGENT_KEEP_HOURS) * 3600
    agent_newest = int(_env_hours('FABLE2_AGENT_SESSION_KEEP_NEWEST', AGENT_KEEP_NEWEST))
    dump_s = _env_hours('FABLE2_SESSION_KEEP_DUMP_HOURS', 168) * 3600
    routine_dump_s = _env_hours('FABLE2_SESSION_KEEP_ROUTINE_DUMP_HOURS', 48) * 3600
    try:
        protected = {Path(e['session']).resolve() for e in instances.instances() if e.get('session')}
    except Exception:
        return []  # can't tell what's running: delete nothing
    for current in {CURRENT, sessions_dir / 'current.json'}:
        try:
            protected.add(Path(json.loads(current.read_text()).get('session', '')).resolve())
        except (OSError, ValueError, AttributeError, TypeError):
            pass
    found = []
    for d in sessions_dir.iterdir():
        m = SESSION_NAME_RE.match(d.name)
        if not (m and d.is_dir()): continue
        try:
            started = time.mktime(time.strptime(m.group(1), '%Y%m%d-%H%M%S'))
        except ValueError:
            continue
        found.append((started, d, _is_agent_session(d)))
    found.sort(key=lambda x: x[0], reverse=True)
    normal = [x for x in found if not x[2]]
    agent = [x for x in found if x[2]]
    now, removed = time.time(), []
    for group, keep_newest, keep_age in ((normal, PRUNE_KEEP_NEWEST, keep_s), (agent, agent_newest, agent_keep_s)):
        for started, d, _ in group[keep_newest:]:
            try:
                age = now - max(started, d.stat().st_mtime)
            except OSError:
                continue
            if d.resolve() in protected or age < keep_age: continue
            tier = dump_tier(d) if age < max(dump_s, routine_dump_s) else None
            if (tier == 'fault' and age < dump_s) or (tier == 'routine' and age < routine_dump_s): continue
            if not dry_run: shutil.rmtree(d, ignore_errors=True)
            removed.append(d.name)
    if removed and log: log(f'pruned {len(removed)} old session folder(s)')
    return removed


def prepare_session(mode, save, target_slot):
    prune_sessions()
    try:
        # out/validation evidence older than 7 days without a KEEP file, in the checkout these sessions belong to
        # (SESSIONS = <checkout>/out/dev-sessions; tests point SESSIONS at a temporary folder).
        common.prune_validation(SESSIONS.parent.parent)
    except Exception:
        pass
    session = SESSIONS / (time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    state = session / 'test-state'
    state.mkdir(parents=True)
    # APFS clones (copy-on-write): the session costs ~0 disk until the game writes, and its writes never
    # reach the live state (the shader cache and pipeline archive were ~0.8 GB per session as full copies).
    for folder in ('saves', 'cache', 'metadata'):
        if (LIVE_STATE / folder).exists(): common.clone(LIVE_STATE / folder, state / folder)
    for name in ('fable_2.toml', 'fable2_config.toml', 'fable2_patches.toml', control.WINDOW_POSITION):
        if (LIVE_STATE / name).exists(): shutil.copy2(LIVE_STATE / name, state / name)
    if mode == 'live':
        if not HERO_RE.match(save): raise ValueError('Live save must be a HeroNNN folder name')
        hero_dir = LIVE_STATE / control.HERO_SLOTS / save
        header = LIVE_STATE / control.HERO_HEADERS / (save + '.header')
        if not hero_dir.is_dir() or not header.is_file(): raise ValueError(f'No live save {save}')
        keep_only_hero(state, hero_dir, header)
        hero, base, origin = save, 'live-' + save.lower(), dict(kind='live', slot=save, state=str(LIVE_STATE))
        default_target = save
    else:
        meta = control.apply_library_save(state, save)
        hero, base, origin = meta['folder'], library_base(save), dict(kind='library', name=save)
        default_target = meta['folder']
    if target_slot is None: target_slot = default_target
    if target_slot.lower() == 'none': target_slot = None
    elif not HERO_RE.match(target_slot): raise ValueError('Target slot must be HeroNNN or none')
    return session, state, dict(hero=hero, base=base, origin=origin, target_slot=target_slot,
                               options_live_signature=options_signature(LIVE_STATE),
                               cache_live_signature=cache_signature(LIVE_STATE),
                               window_live_signature=window_signature(LIVE_STATE))


def write_trace_tags(state, info):
    """Flight-recorder tags for the session (trace/tags-in.txt, polled by the game once a second).
    Only inside the session's own state folder; never fails the caller."""
    try:
        tags = {key: info[field] for key, field in (('level_name', 'LevelName'), ('quest', 'QuestName'))
                if info.get(field)}
        write_tags_in(state, tags)
    except Exception:
        pass


def write_tags_in(state, tags):
    """Merge key=value tags into trace/tags-in.txt (the game applies every line when the file changes)."""
    if not tags: return
    folder = Path(state) / 'logs/trace'
    folder.mkdir(parents=True, exist_ok=True)
    merged = {}
    try:
        for line in (folder / 'tags-in.txt').read_text().splitlines():
            key, sep, value = line.partition('=')
            if sep: merged[key] = value
    except OSError:
        pass
    merged.update({k: str(v).replace('\n', ' ') for k, v in tags.items()})
    tmp = folder / 'tags-in.txt.tmp'
    tmp.write_text(''.join(f'{k}={v}\n' for k, v in merged.items()))
    os.replace(tmp, folder / 'tags-in.txt')


def find_own_pid(app, before, state, control_dir=None, timeout=30):
    """The pid of the game this launch started: a fable_2 that was not running before `open`, whose
    executable is inside `app`; with several candidates, the test-control pid.txt or the holder of the
    state folder's app.lock (AppLauncher.m keeps it through exec) decides. Never another instance."""
    prefix = str(Path(app)) + '/'
    lock = Path(state) / 'app.lock' if state else None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if control_dir and (Path(control_dir) / 'pid.txt').is_file():
            try:
                return int((Path(control_dir) / 'pid.txt').read_text())
            except ValueError:
                pass
        candidates = []
        for pid in game_pids():
            if pid in before: continue
            info = instances.process_info(pid)
            if info and info['exe'].startswith(prefix): candidates.append(pid)
        if len(candidates) == 1: return candidates[0]
        if len(candidates) > 1 and lock and lock.exists():
            held = subprocess.run(['lsof', '-t', str(lock)], capture_output=True, text=True).stdout.split()
            owners = [p for p in candidates if str(p) in held]
            if len(owners) == 1: return owners[0]
        time.sleep(0.2)
    return None


def launch(mode, save=None, target_slot=None, skip_intro=True, boot=True, test_control=False, extra_env=None, fps=None,
           game_args=None, cache_from=None, build='latest', cache_snapshot=None):
    if mode not in ('title', 'live', 'library'): raise ValueError('mode must be title, live or library')
    if mode != 'title' and not save: raise ValueError('--save is required for live/library launches')
    # Test sessions never write back into a live save slot (the player may be playing it).
    if test_control:
        if target_slot not in (None, 'none', 'None'):
            raise ValueError('--test-control sessions never write back to a live slot; use --target-slot none')
        target_slot = 'none'
    # Playtest without a playtest copy (a fresh checkout has none) launches the latest build with a note instead of
    # failing (2026-10-15).
    build_note = None
    if build == 'playtest' and not playtest.app_for('playtest').is_dir():
        build, build_note = 'latest', 'No playtest copy: used the latest build.'
    app, data_root = playtest.app_for(build), playtest.data_root_for(build)
    # Test-harness parameters (telemetry phase 1): extra game args/cvars and a cache seed. Only for
    # automated test sessions; normal play keeps its exact launch and cache write-back.
    game_args, cache_from = list(game_args or []), (Path(cache_from).expanduser() if cache_from else None)
    if (game_args or cache_from or cache_snapshot) and not (test_control and mode != 'title'):
        raise ValueError('--game-arg/--cache-from/--cache-snapshot are for test sessions (--test-control with a save) only')
    if cache_from and cache_snapshot:
        raise ValueError('--cache-from and --cache-snapshot are alternatives')
    if cache_snapshot is None and test_control:  # FABLE2_AGENT_CACHE: this process's environment or --env
        cache_snapshot = next((i.partition('=')[2] for i in extra_env or [] if i.startswith('FABLE2_AGENT_CACHE=')),
                              None) or os.environ.get('FABLE2_AGENT_CACHE')
    if cache_from and not cache_from.is_dir(): raise ValueError(f'--cache-from: no such folder {cache_from}')
    if not app.is_dir():
        raise RuntimeError(f'Game app not built: {app}')
    # Multiple instances are fine (agents test while the player plays). Only one normal-play game at a time:
    # two would write back into the same live state.
    # Disk guard: auto-clean below FABLE2_MIN_FREE_GB, then refuse test launches (play only warns).
    common.ensure_free_space('a game launch', refuse=bool(test_control))
    running = instances.instances()
    if not test_control or mode == 'title':
        normal = [e for e in running if e['role'] in ('play', 'playtest') and not e.get('test_control')]
        if normal:
            raise RuntimeError('A Fable II game is already open for play (%s). Close it before launching another.'
                               % instances.describe(normal[0]))
    env = {}
    if fps is not None:
        if fps not in ('30','60','uncapped'): raise ValueError('fps must be 30, 60 or uncapped')
        env.update(FABLE2_FPS_60='1' if fps=='60' else '0', REX_VSYNC='0' if fps=='uncapped' else '1')
    env['FABLE2_SKIP_INTRO_MOVIES'] = '1' if skip_intro else '0'  # explicit both ways, as the player app
    if skip_intro:
        # The stand-in movies the game redirects to; without them the full movies play (fresh clone, 2026-10-15).
        skip_intro_movies.ensure(data_root)
    session = info = test_window = agent_cache_record = None
    if mode != 'title':
        session, state, info = prepare_session(mode, save, target_slot)
        if test_control and test_windows.enabled():  # replaces the copied live placement
            test_window = test_windows.prepare(state, position_only=test_windows.fixed_size(game_args))
        if cache_from:
            shutil.rmtree(state / 'cache', ignore_errors=True)
            common.clone(cache_from, state / 'cache')
        elif test_control:
            # Agent shader cache (tools/fable2_agent_cache.py): seed from the build's key instead of the live cache.
            shutil.rmtree(state / 'cache', ignore_errors=True)
            try:
                agent_cache_record = agent_cache.seed(state / 'cache', app, cache_snapshot, live_state=LIVE_STATE)
            except Exception:
                shutil.rmtree(session, ignore_errors=True)
                raise
            print('AGENT CACHE: %s' % agent_cache_summary(agent_cache_record), file=sys.stderr, flush=True)
        hero_dir = state / control.HERO_SLOTS / info['hero']
        header = state / control.HERO_HEADERS / (info['hero'] + '.header')
        try:
            write_trace_tags(state, control.describe_save(hero_dir, header))
        except Exception:
            pass
        env.update(FABLE2_APP_STATE_ROOT=str(state), FABLE2_GAME_DATA_ROOT=str(data_root))
        if boot: env['FABLE2_BOOT_SAVE'] = '1'
        if test_control:
            control_dir = state / 'control'
            control_dir.mkdir(mode=0o700)
            env.update(FABLE2_TEST_CONTROL_DIR=str(control_dir), FABLE2_SESSION_DIR=str(control_dir),
                       FABLE2_SESSION_ORIGIN_US=str(time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000))
            # Automated tests run in background test mode: no activation, and test
            # presses work without focus (src/input/test_background.h).
            env['FABLE2_TEST_BACKGROUND'] = '1'
            # Test-only instance: no physical controller/keyboard/mouse input at all; only the
            # test-control channel drives it (the player's input reaches only the player's own game).
            env['FABLE2_PHYSICAL_INPUT'] = '0'
    for item in extra_env or []:  # diagnostics, e.g. FABLE2_SKIP_INTRO_TRACE=1
        key, _, value = item.partition('=')
        if not key: raise ValueError('--env expects NAME=VALUE')
        env[key] = value
    if test_control:  # tests always skip the intros + fast title (FABLE2_TEST_INTRO=1: intro-path tests only)
        common.force_test_intro_skip(env, data_root)
    if test_control:  # window-title tag (src/core/fable2_window_title.h); --env / the caller's wins; play: none
        env.setdefault('FABLE2_WINDOW_TAG', os.environ.get('FABLE2_WINDOW_TAG') or common.window_tag())
    role = 'playtest' if build == 'playtest' else ('agent' if test_control else 'play')
    env['FABLE2_INSTANCE_ROLE'] = role  # window title (src/core/fable_2_app.h WindowTitle)
    if not test_control:  # Character Editor mod backups outlive the pruned session folder (docs/character-editor.md)
        env.setdefault('FABLE2_CHARACTER_EDITOR_BACKUP_DIR', str(BACKUPS / 'character-editor'))
    if session: env['FABLE2_INSTANCE_ID'] = session.name
    if mode == 'title' and build == 'playtest':
        env['FABLE2_GAME_DATA_ROOT'] = str(data_root)  # the playtest copy's own game data (not persisted)
    if mode == 'title' and os.environ.get('FABLE2_LIVE_STATE_ROOT'):
        env.setdefault('FABLE2_APP_STATE_ROOT', str(LIVE_STATE))  # an overridden live state (tests): title play uses it
    # LaunchServices launch, like double-clicking the app; only these variables are added.
    command = ['open', '-n']
    if env.get('FABLE2_TEST_BACKGROUND') == '1': command.append('-g')  # do not bring the app forward
    for key, value in env.items(): command += ['--env', f'{key}={value}']
    args = []
    # Background test sessions are silent (SDK cvar audio_mute); normal play keeps sound.
    if env.get('FABLE2_TEST_BACKGROUND') == '1': args.append('--audio_mute=true')
    args += game_args
    if test_control:  # no "reopen windows?" alert after a crash (fable2_common.TEST_LAUNCH_APPKIT_ARGS)
        args += list(common.TEST_LAUNCH_APPKIT_ARGS)
    state_root = state if session else LIVE_STATE
    # Instance cap (FABLE2_MAX_INSTANCES, 4) for test sessions, checked under a global launch lock so two
    # checkouts can't both take the last place; Play games are never held back.
    # Perf lock (local agent docs): test games hold MAIN_ROOT/out/locks/perf.lock shared for their whole
    # life, so perf runs wait for them and they wait for (and never cut ahead of) a queued perf run. Taken
    # before the instance gate (never wait for a perf run while holding the launch lock). Play games never
    # takes it.
    hold = None
    if test_control:
        try:
            # Agent game limit (FABLE2_AGENT_GAMES / _3X_GAMES): after the shared perf hold.
            hold = common.test_game_hold('test launch %s' % (session.name if session else mode),
                                         common.launch_scale(env, state if session else LIVE_STATE))
        except common.LockTimeout:
            if session: shutil.rmtree(session, ignore_errors=True)
            raise
    gate = (common.instance_slot('test launch', lambda: len(instances.instances())) if test_control
            else contextlib.nullcontext())
    try:
        gate.__enter__()
    except common.InstanceCapError:
        if hold: hold.close()
        if session: shutil.rmtree(session, ignore_errors=True)  # nothing started: drop the prepared folder
        raise
    with contextlib.ExitStack() as stack:
        if hold: stack.callback(hold.close)  # released here unless handed to the holder process
        stack.push(gate)
        before = set(game_pids())
        subprocess.run(command + [str(app)] + (['--args'] + args if args else []), check=True)
        pid = find_own_pid(app, before, state_root, env.get('FABLE2_TEST_CONTROL_DIR'))
        if pid is None: raise RuntimeError('The game did not start; see the logs folder of the state root')
        instances.register(pid, role, app, session, test_control, env, state_root)
        if test_window: test_windows.update_claim(test_window['key'], pid)
        if hold: hold.hand_off(pid, session)
    others = instances.contention(pid)
    record = dict(mode=mode, pid=pid, start_unix=time.time(), launched_by=launch_ancestry(), env=env, game_args=args, session=str(session) if session else None,
                  build=build, app=str(app), version=instances.app_version(app), role=role,
                  input='test-only' if env.get('FABLE2_PHYSICAL_INPUT') == '0' else 'physical',
                  concurrent_instances=others['others'], contention=others['text'] or None,
                  cache_from=str(cache_from) if cache_from else None, checkout=common.SLOT or 'main',
                  agent_cache=agent_cache_record,
                  perf_lock_wait_s=_float_or_none(os.environ.get('FABLE2_PERF_LOCK_WAIT_S')),
                  perf_lock_shared_wait_s=hold.wait if hold else None,
                  agent_game_slots=hold.games if hold else None, agent_game_wait_s=hold.games_wait if hold else None,
                  resolution_scale_counted=hold.scale if hold else None,
                  save=save, skip_intro=skip_intro, boot=boot,
                  options_writeback=normal_options_writeback(test_control, env),
                  cache_writeback=normal_cache_writeback(test_control, env),
                  window_writeback=normal_window_writeback(test_control, env), test_window=test_window)
    if session:
        record.update(info, state=str(session / 'test-state'), live_state=str(LIVE_STATE),
                      executable_sha256=None, test_control=test_control)
        (session / 'session.json').write_text(json.dumps(record, indent=2) + '\n')
        with open(session / 'instances.jsonl', 'a') as f:
            f.write(json.dumps(dict(time=time.strftime('%Y-%m-%d %H:%M:%S'), t=0.0, event='launch',
                                    others=others['others'])) + '\n')
        write_tags_in(state, dict(contention=instances.tag_value(others['others'])))
        log = open(session / 'watcher.log', 'ab')
        subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'watch', str(session)],
                         stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    elif not test_control:
        # Title-screen play runs on the live state: saves made there also go to the save library (2026-10-15: a fresh
        # clone has no saves to pick, so every game was title play and the library stayed empty).
        SESSIONS.mkdir(parents=True, exist_ok=True)
        log = open(SESSIONS / 'title-watch.log', 'ab')
        subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'watch-live', '--pid', str(pid),
                          '--state', str(LIVE_STATE)],
                         stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    if build_note: record['note'] = build_note
    SESSIONS.mkdir(parents=True, exist_ok=True)
    if not test_control:  # the Dev Launcher's own game; test sessions are addressed by session folder
        CURRENT.write_text(json.dumps(record, indent=2) + '\n')
    return record


# ---------------------------------------------------------------- write-back

def save_files(state):
    """Signature of every hero save file: {relative path: (size, mtime_ns)}."""
    state = Path(state)
    files = {}
    for base in (state / control.HERO_SLOTS, state / control.HERO_HEADERS):
        if not base.is_dir(): continue
        for path in base.rglob('*'):
            try:
                if path.is_file():
                    st = path.stat()
                    files[str(path.relative_to(state))] = (st.st_size, st.st_mtime_ns)
            except FileNotFoundError:
                pass
    return files


def changed_heroes(before, after):
    heroes = set()
    for rel in set(before) | set(after):
        if before.get(rel) == after.get(rel) or rel not in after: continue
        parts = Path(rel).parts
        name = Path(rel).stem if rel.endswith('.header') else next((p for p in parts if HERO_RE.match(p)), None)
        if name and HERO_RE.match(name): heroes.add(name)
    return sorted(heroes)


def files_open(pid, folder):
    """True when the game still has a file under `folder` open (lsof)."""
    result = subprocess.run(['lsof', '-n', '-P', '-p', str(pid), '-Fn'], capture_output=True, text=True)
    prefix = 'n' + str(Path(folder).resolve())
    return any(line.startswith(prefix) for line in result.stdout.splitlines())


def options_file_open(pid, state):
    result = subprocess.run(['lsof', '-n', '-P', '-p', str(pid), '-Fn'], capture_output=True, text=True)
    if result.returncode not in (0, 1) or (result.returncode == 1 and result.stderr.strip()):
        raise OSError('Unable to verify options file is closed: ' + result.stderr.strip())
    return 'n' + str((Path(state) / OPTIONS_FILE).resolve()) in result.stdout.splitlines()


def patch_header(raw, slot):
    """Rewrite a header's display name ("Hero N+1") and folder name for another slot."""
    raw = bytearray(raw)
    display = ('Hero %d' % (int(slot[4:]) + 1)).encode('utf-16-be')
    raw[8:0x108] = display + bytes(0x100 - len(display))
    raw[0x108:0x138] = slot.encode('ascii') + bytes(0x30 - len(slot))
    return bytes(raw)


def write_back_live(session_hero_dir, session_header, target, live_state, stamp):
    live_slots, live_headers = Path(live_state) / control.HERO_SLOTS, Path(live_state) / control.HERO_HEADERS
    dest, dest_header = live_slots / target, live_headers / (target + '.header')
    backup = BACKUPS / (stamp + '-' + target)
    n = 2
    while backup.exists(): backup, n = BACKUPS / (stamp + '-' + target + '-' + str(n)), n + 1
    backup.mkdir(parents=True)
    if dest.is_dir(): shutil.copytree(dest, backup / target)
    if dest_header.is_file(): shutil.copy2(dest_header, backup / dest_header.name)
    (backup / 'backup.json').write_text(json.dumps(dict(slot=target, live_state=str(live_state),
        existed=dest.is_dir(), reason='dev-session write-back', time=stamp), indent=2) + '\n')
    dest.mkdir(parents=True, exist_ok=True)
    live_headers.mkdir(parents=True, exist_ok=True)
    for path in Path(session_hero_dir).iterdir():  # extra live files (e.g. Fable2PubInfo.xml) are kept
        if path.is_file(): shutil.copy2(path, dest / path.name)
    raw = Path(session_header).read_bytes()
    if Path(session_hero_dir).name != target: raw = patch_header(raw, target)
    tmp = dest_header.with_suffix('.header.tmp')
    tmp.write_bytes(raw)
    os.replace(tmp, dest_header)
    return str(backup)


def commit_save(session, record, hero, log):
    state = Path(record['state'])
    hero_dir = state / control.HERO_SLOTS / hero
    header = state / control.HERO_HEADERS / (hero + '.header')
    if not (hero_dir / 'mainsave.bin').is_file() or not header.is_file():
        log(dict(event='skip', hero=hero, reason='incomplete save (no mainsave.bin or header)'))
        return
    stamp = now_stamp()
    base = record['base'] if hero == record['hero'] else record['base'] + '-' + hero.lower()
    name, n = base + '_' + stamp, 2
    while (LIBRARY / name).exists(): name, n = base + '_' + stamp + '-' + str(n), n + 1
    entry = LIBRARY / name
    entry.mkdir(parents=True)
    shutil.copytree(hero_dir, entry / hero)
    shutil.copy2(header, entry / header.name)
    target = record.get('target_slot') if hero == record['hero'] else None
    backup = write_back_live(entry / hero, entry / header.name, target, record['live_state'], stamp) if target else None
    meta = dict(name=name, source=str(hero_dir), created=time.strftime('%Y-%m-%d %H:%M:%S'),
                origin=record['origin'], session=Path(session).name, written_back_to=target,
                live_backup=backup, **control.describe_save(entry / hero, entry / header.name))
    (entry / 'save.json').write_text(json.dumps(meta, indent=2) + '\n')
    log(dict(event='saved', hero=hero, library=name, written_back_to=target, backup=backup,
             QuestName=meta.get('QuestName'), LevelName=meta.get('LevelName')))
    write_trace_tags(state, meta)  # flight-recorder level_name / quest tags (session folder only)


# ---------------------------------------------------------------- agent metrics
# One line per finished game session in <workspace>/agent-metrics/game-sessions.jsonl (only if that folder exists),
# so agent time reports get exact game wall time. launched_by = ancestor command lines at launch, which the
# report matches against the hook log to attribute the run to the agent whose tool call started it.
AGENT_METRICS = common.AGENT_METRICS  # the workspace's, also from an agent slot


def _float_or_none(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def launch_ancestry(depth=6):
    out, pid = [], os.getppid()
    for _ in range(depth):
        try:
            line = subprocess.run(['ps', '-o', 'ppid=,command=', '-p', str(pid)], capture_output=True,
                                  text=True, timeout=2).stdout.strip()
        except Exception:
            break
        if not line: break
        ppid, _, command = line.partition(' ')
        command = command.strip()
        if " eval '" in command: command = command.split(" eval '", 1)[1]  # the tool's own command, not the shell preamble
        out.append(command[:300])
        if not ppid.strip().isdigit() or int(ppid) <= 1: break
        pid = int(ppid)
    return out


def write_game_metric(record, end_unix, how):
    if not AGENT_METRICS.is_dir(): return
    try:
        line = dict(session=Path(record['session']).name if record.get('session') else None, pid=record.get('pid'),
                    role=record.get('role'), mode=record.get('mode'), build=record.get('build'),
                    start=round(record['start_unix'], 3), end=round(end_unix, 3),
                    seconds=round(end_unix - record['start_unix'], 1), exit=how,
                    contention=record.get('contention'), launched_by=record.get('launched_by'),
                    checkout=record.get('checkout'), perf_lock_wait_s=record.get('perf_lock_wait_s'),
                    perf_lock_shared_wait_s=record.get('perf_lock_shared_wait_s'))
        with open(AGENT_METRICS / 'game-sessions.jsonl', 'a') as f:
            f.write(json.dumps(line, separators=(',', ':')) + '\n')
    except Exception:
        pass


def watch(session):
    session = Path(session).resolve()
    record = json.loads((session / 'session.json').read_text())
    pid, state = record['pid'], Path(record['state'])
    events = session / 'writeback.jsonl'

    def log(event):
        event = dict(time=time.strftime('%Y-%m-%d %H:%M:%S'), **event)
        with open(events, 'a') as f: f.write(json.dumps(event) + '\n')
        print(json.dumps(event), flush=True)

    committed = save_files(state)
    options_committed = options_signature(state)
    options_pending_since = options_last_sig = None
    log(dict(event='watching', pid=pid, files=len(committed), hero=record['hero'], target_slot=record.get('target_slot')))
    pending_since = last_sig = None
    # Concurrent instances: re-checked every INSTANCE_POLL_SECONDS; each change goes to instances.jsonl
    # and the flight-recorder 'contention' tag, so perf numbers can be checked for overlap.
    known_others = {(e['pid'], e['role']) for e in record.get('concurrent_instances') or []}
    next_instance_poll = time.monotonic() + INSTANCE_POLL_SECONDS
    while True:
        alive = pid_alive(pid)
        if alive and time.monotonic() >= next_instance_poll:
            next_instance_poll = time.monotonic() + INSTANCE_POLL_SECONDS
            try:
                others = instances.contention(pid)['others']
                key = {(e['pid'], e['role']) for e in others}
                if key != known_others:
                    known_others = key
                    with open(session / 'instances.jsonl', 'a') as f:
                        f.write(json.dumps(dict(time=time.strftime('%Y-%m-%d %H:%M:%S'),
                                                t=round(time.time() - record['start_unix'], 1),
                                                event='change', others=others)) + '\n')
                    write_tags_in(state, dict(contention=instances.tag_value(others)))
            except Exception as error:
                log(dict(event='instances-error', error=repr(error)))
        current = save_files(state)
        try:
            options_current = options_signature(state)
        except OSError as error:
            log(dict(event='options-error', error=repr(error)))
            options_current = options_committed  # next poll retries without abandoning the watcher
        if not alive:
            # Never copy because the game closed or crashed: a save still pending at exit
            # may be half-written (seen when the game crashed mid-save).
            if current != committed:
                log(dict(event='discarded-on-exit', heroes=changed_heroes(committed, current)))
            if options_current != options_committed:
                log(dict(event='options-discarded-on-exit'))
            ended_at = time.time()
            instances.unregister(pid)
            time.sleep(1.0)  # let the final log lines land before checking for a clean exit
            how = exit_kind(state)
            log(dict(event='game-exited', how=how['kind'], detail=how['detail']))
            write_game_metric(record, ended_at, how['kind'])
            commit_window_position(record, log)
            if record.get('test_window'): log(test_windows.commit(state, record['test_window'], pid))
            commit_cache(record, log)
            return
        if options_current != options_committed:
            if options_current != options_last_sig:
                options_last_sig, options_pending_since = options_current, time.monotonic()
            if time.monotonic() - options_pending_since >= STABLE_SECONDS:
                try:
                    if not options_file_open(pid, state) and pid_alive(pid):
                        commit_options(session, record, log, options_current)
                        options_committed = options_current
                        options_pending_since = options_last_sig = None
                except Exception as error:
                    log(dict(event='options-error', error=repr(error)))
                    if isinstance(error, OSError):
                        options_pending_since = time.monotonic()  # retry transient I/O after another stable interval
                    else:
                        options_committed = options_current  # rejected conflict/unsupported payload; wait for a new write
                        options_pending_since = options_last_sig = None
        if current != committed:
            if current != last_sig:
                last_sig, pending_since = current, time.monotonic()
            stable = time.monotonic() - pending_since >= STABLE_SECONDS
            if stable and not files_open(pid, state / 'saves') and pid_alive(pid):
                heroes = changed_heroes(committed, current)
                log(dict(event='save-detected', heroes=heroes))
                for hero in heroes:
                    try:
                        commit_save(session, record, hero, log)
                    except Exception as error:
                        log(dict(event='error', hero=hero, error=repr(error)))
                committed, pending_since, last_sig = current, None, None
                continue
        time.sleep(POLL_SECONDS)


def capture_live_save(state, hero, log):
    """Copy one live hero save (title-screen play) into the save library. Nothing is written back: it is live."""
    state = Path(state)
    hero_dir = state / control.HERO_SLOTS / hero
    header = state / control.HERO_HEADERS / (hero + '.header')
    if not (hero_dir / 'mainsave.bin').is_file() or not header.is_file():
        log(dict(event='skip', hero=hero, reason='incomplete save (no mainsave.bin or header)'))
        return None
    stamp = now_stamp()
    base = 'live-' + hero.lower()
    name, n = base + '_' + stamp, 2
    while (LIBRARY / name).exists(): name, n = base + '_' + stamp + '-' + str(n), n + 1
    entry = LIBRARY / name
    entry.mkdir(parents=True)
    shutil.copytree(hero_dir, entry / hero)
    shutil.copy2(header, entry / header.name)
    meta = dict(name=name, source=str(hero_dir), created=time.strftime('%Y-%m-%d %H:%M:%S'),
                origin=dict(kind='title', slot=hero, state=str(state)), session=None, written_back_to=None,
                **control.describe_save(entry / hero, entry / header.name))
    (entry / 'save.json').write_text(json.dumps(meta, indent=2) + '\n')
    log(dict(event='saved', hero=hero, library=name, QuestName=meta.get('QuestName'), LevelName=meta.get('LevelName')))
    return name


def watch_live(pid, state):
    """Title-screen play (no session): each save the game makes in the live state goes to the library once it has
    been stable for STABLE_SECONDS and the game has closed the file. Stops when the game exits (a save still pending
    then is captured only after a clean exit)."""
    state = Path(state)

    def log(event):
        print(json.dumps(dict(time=time.strftime('%Y-%m-%d %H:%M:%S'), pid=pid, **event)), flush=True)

    committed = save_files(state)
    log(dict(event='watching-live', state=str(state), files=len(committed)))
    pending_since = last_sig = None
    while True:
        alive = pid_alive(pid)
        current = save_files(state)
        if current != committed:
            if current != last_sig: last_sig, pending_since = current, time.monotonic()
            stable = time.monotonic() - pending_since >= STABLE_SECONDS
            if not alive:
                time.sleep(1.0)
                stable = clean_exit(state)
                if not stable: log(dict(event='discarded-on-exit', heroes=changed_heroes(committed, current)))
            if stable and (not alive or not files_open(pid, state / 'saves')):
                for hero in changed_heroes(committed, current):
                    try:
                        capture_live_save(state, hero, log)
                    except Exception as error:
                        log(dict(event='error', hero=hero, error=repr(error)))
                committed, pending_since, last_sig = current, None, None
        if not alive:
            log(dict(event='game-exited'))
            return
        time.sleep(POLL_SECONDS)


# ---------------------------------------------------------------- status / close

def current_record():
    """The Dev Launcher's current normal-play game (out/dev-sessions/current.json) while its pid runs."""
    try:
        record = json.loads(CURRENT.read_text())
    except (OSError, ValueError):
        return None
    return record if record.get('pid') and pid_alive(record['pid']) else None


def status():
    current = current_record()
    last = None
    if current and current.get('session'):
        path = Path(current['session']) / 'writeback.jsonl'
        if path.is_file():
            saved = [json.loads(l) for l in path.read_text().splitlines() if '"saved"' in l]
            last = saved[-1] if saved else None
    running = instances.instances()
    # running: the launcher's own game (only that one enables its Close button).
    return dict(running=bool(current), pids=[e['pid'] for e in running], session=current, last_save=last,
                instances=running, others=[instances.describe(e) for e in running
                                           if not current or e['pid'] != current['pid']],
                last_exit=None if current else last_exit())


def last_exit():
    """How the launcher's last game ended, once it has: dict(pid, kind, detail, logs) or None. The launcher shows a
    note with Show Logs for kind 'unclean' (crash or kill) when it comes back after the game."""
    try:
        record = json.loads(CURRENT.read_text())
    except (OSError, ValueError):
        return None
    if not record.get('pid') or pid_alive(record['pid']): return None
    state = Path(record.get('state') or LIVE_STATE)
    return dict(pid=record['pid'], logs=str(state / 'logs'), **exit_kind(state))


def close(session=None, current=False, timeout=20):
    """Close exactly one game: the one of `session`, or (current=True) the Dev Launcher's current game."""
    if bool(session) == bool(current): raise ValueError('close needs a SESSION folder or --current')
    if session:
        record = json.loads((Path(session) / 'session.json').read_text())
    else:
        record = current_record()
        if record is None: return 'not running'
    pid = record['pid']
    if not pid_alive(pid): return 'not running'
    info = instances.process_info(pid)
    if info is None or Path(info['exe']).name != 'fable_2':
        raise RuntimeError(f'pid {pid} is not a Fable II game any more; not touching it')
    if record.get('session') and (record.get('test_control') or record.get('env', {}).get('FABLE2_TEST_CONTROL_DIR')):
        try:
            control.request(record['session'], 'close', timeout=2)
        except Exception:
            pass
    else:
        os.kill(pid, signal.SIGTERM)  # SDL turns SIGTERM into a normal quit event
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not pid_alive(pid):
            instances.unregister(pid)
            return 'closed'
        time.sleep(0.2)
    raise TimeoutError(f'Close requested but fable_2 pid {pid} is still running')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('list')
    q = sub.add_parser('launch')
    q.add_argument('--mode', choices=['title', 'live', 'library'], required=True)
    q.add_argument('--save', help='HeroNNN (live) or library entry name')
    q.add_argument('--target-slot', help='live slot to write saves back to (HeroNNN or none); '
                   'default: the live slot picked, or the library entry\'s own HeroNNN folder')
    q.add_argument('--no-skip-intro', action='store_true')
    q.add_argument('--no-boot', action='store_true', help='stop at the title instead of loading the save')
    q.add_argument('--test-control', action='store_true', help='enable the test-control endpoint (capture/press)')
    q.add_argument('--fps', choices=['30','60','uncapped'], help='guest frame pacing; omitted keeps existing settings')
    q.add_argument('--env', action='append', metavar='NAME=VALUE', help='extra environment variable for the game')
    q.add_argument('--game-arg', action='append', metavar='ARG', dest='game_args',
                   help='test sessions only: extra game argument/cvar, e.g. --game-arg=--metal_pipeline_creation_threads=4')
    q.add_argument('--cache-from', metavar='DIR', help='test sessions only: use DIR as the session cache (warm/cold seed)')
    q.add_argument('--cache-snapshot', metavar='MODE',
                   help='test sessions only: agent shader cache auto|readonly|cold|off|KEY@GEN|PIN (default auto)')
    q.add_argument('--build', choices=['playtest', 'latest'], default='latest',
                   help='playtest = frozen copy in out/playtest (tools/fable2_playtest.py), latest = out/build (default)')
    sub.add_parser('status')
    q = sub.add_parser('close')
    q.add_argument('session', nargs='?', help='session folder whose game to close')
    q.add_argument('--current', action='store_true', help="the launcher's current normal-play game")
    q = sub.add_parser('watch'); q.add_argument('session', type=Path)
    q = sub.add_parser('watch-live'); q.add_argument('--pid', type=int, required=True); q.add_argument('--state', type=Path, required=True)
    q = sub.add_parser('prune', help='delete old session folders now (also runs on every launch)')
    q.add_argument('--dry-run', action='store_true', help='list what would be deleted')
    args = p.parse_args()
    try:
        if args.cmd == 'list':
            # A fresh clone has no library yet: create it (and the main library holding .picker.json) on first need.
            for folder in (LIBRARY, save_picker.picker_meta_path().parent):
                try:
                    folder.mkdir(parents=True, exist_ok=True)
                except OSError:
                    pass
            result = dict(live=[save_picker.annotate(e, {}) for e in live_slots()], library=library_entries(), running=bool(current_record()),
                          picker_meta=str(save_picker.picker_meta_path()),
                          live_state=str(LIVE_STATE), playtest=playtest.info(),
                          latest_version=instances.app_version(playtest.LATEST_APP))
        elif args.cmd == 'launch':
            result = launch(args.mode, args.save, args.target_slot, not args.no_skip_intro, not args.no_boot,
                            args.test_control, args.env, args.fps, args.game_args, args.cache_from, args.build,
                            args.cache_snapshot)
        elif args.cmd == 'status': result = status()
        elif args.cmd == 'close': result = close(args.session, args.current)
        elif args.cmd == 'prune': result = dict(removed=prune_sessions(dry_run=args.dry_run), dry_run=args.dry_run)
        elif args.cmd == 'watch-live':
            watch_live(args.pid, args.state)
            return
        else:
            watch(args.session)
            return
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        sys.exit(1)
    print(json.dumps({'ok': True, 'result': result}, indent=2))


if __name__ == '__main__':
    main()
