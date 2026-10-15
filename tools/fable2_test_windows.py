#!/usr/bin/env python3
"""Remembered window placement for agent/test game windows, separate from the player's play window.

Every test launch (fable2_test_control.py launch / MCP, fable2_dev_session.py launch --test-control, so f2diag
runs and portable warm-ups, plus fable2_benchmark.py runs) seeds its session's fable2_window.toml from a shared
store instead of the live one, and publishes the session's final placement back to the store when the game
exits (cleanly, by crash or by the window's close button). Moving a test window therefore moves the next test
window of the same key, and never the player's play window (the live Application Support file is never read or
written here).

Store: MAIN_ROOT/out/instances/test-windows/<key>.toml (shared by the main checkout and every agent slot).
Key: '<checkout>-<n>': checkout is 'main' or 'agent-N'; n is the concurrent index, the lowest index not held
by another running test game of that checkout (claims/<key>.json, pid-scoped, stale claims ignored). The 1st
and 2nd test game running at the same time from one checkout get different keys and so different positions.
Default (no stored placement for the key): position only (the game's default size), cascaded per key so
concurrent windows don't stack: k = slot_number * 2 + (n - 1) (main = 0, agent-N = N),
x = 60 + 90 * k, y = 60 + 50 * k (screen points, client-area top-left on the main display).
Size: remembered with the position, except for launches that fix the window size (--window_width /
--window_height game args) and benchmark runs, which publish only the position (the stored size is kept) and
seed only the position.
Write-back rules (as the play window write-back in fable2_dev_session.py): the session file must parse as
exactly x/y (int32) [+ width/height 1..16384]; unchanged from the seed -> nothing written; the store entry must
not have changed since the session started (else the newer one is kept); temp file + fsync + os.replace.
Switch: FABLE2_TEST_WINDOW_MEMORY=0 in the launching environment restores the old behaviour (seed from the latest
live/test position, no write-back). FABLE2_TEST_WINDOWS_DIR overrides the store folder (tests only).

  python3 tools/fable2_test_windows.py list            stored placements and live claims
  python3 tools/fable2_test_windows.py watch SESSION   (internal) wait for the session's game, then publish
Runs under /usr/bin/python3 (3.9).
"""
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_common as common  # noqa: E402

WINDOW_FILE = 'fable2_window.toml'
INT32 = range(-2**31, 2**31)
WINDOW_SIZE = range(1, 16384 + 1)
MAX_INDEX = 8
PENDING_SECONDS = 180  # a claim taken before the game's pid is known (launch in progress)
POLL_SECONDS = 1.0


def store():
    return Path(os.environ.get('FABLE2_TEST_WINDOWS_DIR') or common.MAIN_ROOT / 'out/instances/test-windows')


def enabled(env=None):
    return (os.environ if env is None else env).get('FABLE2_TEST_WINDOW_MEMORY', '1') != '0'


def checkout_name():
    return common.SLOT or 'main'


def fixed_size(game_args):
    """True when the launch sets the window size explicitly (the game then ignores a saved size)."""
    return any(re.match(r'--window_(width|height)(=|$)', str(a)) for a in game_args or [])


def parse_window_position(raw):
    """dict(x, y[, width, height]) when raw is a TOML file with exactly the integer keys
    {x, y} or {x, y, width, height} in range, else None (the game's flat format; no tomllib on 3.9)."""
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


def render(position):
    text = '# Test-window placement (tools/fable2_test_windows.py)\nx = %d\ny = %d\n' % (position['x'], position['y'])
    if 'width' in position: text += 'width = %d\nheight = %d\n' % (position['width'], position['height'])
    return text.encode()


def signature(path):
    try:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except FileNotFoundError:
        return None


def atomic_write(path, raw):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp-' + uuid.uuid4().hex)
    try:
        with open(temporary, 'wb') as out:
            out.write(raw)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


@contextlib.contextmanager
def store_lock():
    folder = store()
    folder.mkdir(parents=True, exist_ok=True)
    with open(folder / '.lock', 'a') as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        try:
            yield folder
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def _claim_live(path):
    try:
        claim = json.loads(path.read_text())
    except (OSError, ValueError):
        return False
    if claim.get('pending'):
        return common.pid_alive(claim.get('pid', 0)) and time.time() - claim.get('time', 0) < PENDING_SECONDS
    return common.pid_alive(claim.get('pid', 0))


def claim(owner_pid=None, checkout=None):
    """The lowest free key '<checkout>-<n>', claimed (pending) for owner_pid until update_claim names the game."""
    checkout = checkout or checkout_name()
    with store_lock() as folder:
        claims = folder / 'claims'
        claims.mkdir(exist_ok=True)
        for n in range(1, MAX_INDEX + 1):
            key = '%s-%d' % (checkout, n)
            if (claims / (key + '.json')).exists() and _claim_live(claims / (key + '.json')): continue
            common._atomic_json(claims / (key + '.json'),
                                dict(pid=owner_pid or os.getpid(), pending=True, time=time.time()))
            return key
    return '%s-%d' % (checkout, MAX_INDEX)  # more games than indices: share the last one


def update_claim(key, pid):
    with store_lock() as folder:
        common._atomic_json(folder / 'claims' / (key + '.json'), dict(pid=int(pid), pending=False, time=time.time()))


def release(key, pid=None):
    """Drop the claim (only when still held by pid, if given)."""
    with store_lock() as folder:
        path = folder / 'claims' / (key + '.json')
        try:
            if pid is None or json.loads(path.read_text()).get('pid') == int(pid): path.unlink()
        except (OSError, ValueError):
            pass


def default_position(key):
    checkout, _, n = key.rpartition('-')
    m = re.fullmatch(r'agent-(\d+)', checkout)
    k = (int(m.group(1)) if m else 0) * 2 + max(int(n) - 1, 0)
    return dict(x=60 + 90 * k, y=60 + 50 * k)


def stored(key):
    path = store() / (key + '.toml')
    try:
        raw = path.read_bytes()
    except FileNotFoundError:
        return None, None
    return parse_window_position(raw), hashlib.sha256(raw).hexdigest()


def prepare(state, position_only=False, owner_pid=None, checkout=None):
    """Claim a key and write the session's fable2_window.toml from the store (or the default).
    Returns the record to keep in session.json (test_window) for commit()."""
    key = claim(owner_pid, checkout)
    position, store_sha = stored(key)
    source = 'stored' if position else 'default'
    if not position: position = default_position(key)
    if position_only: position = dict(x=position['x'], y=position['y'])
    raw = render(position)
    atomic_write(Path(state) / WINDOW_FILE, raw)
    return dict(key=key, store=str(store() / (key + '.toml')), source=source, store_sha256=store_sha,
                seed_sha256=hashlib.sha256(raw).hexdigest(), position_only=bool(position_only), seeded=position)


def commit(state, record, pid=None):
    """Publish the session's placement to the store entry of record['key']. Returns the event to log."""
    key = record['key']
    try:
        path = Path(state) / WINDOW_FILE
        try:
            raw = path.read_bytes()
        except FileNotFoundError:
            return dict(event='test-window-unchanged', key=key, reason='no session window file')
        if hashlib.sha256(raw).hexdigest() == record.get('seed_sha256'):
            return dict(event='test-window-unchanged', key=key)
        position = parse_window_position(raw)
        if position is None: return dict(event='test-window-invalid', key=key)
        with store_lock() as folder:
            dest = folder / (key + '.toml')
            current, current_sha = stored(key)
            if current_sha != record.get('store_sha256'):
                return dict(event='test-window-kept-newer', key=key)
            if record.get('position_only'):
                keep = {k: current[k] for k in ('width', 'height') if current and k in current}
                position = dict(x=position['x'], y=position['y'], **keep)
            new = render(position)
            if current_sha == hashlib.sha256(new).hexdigest():
                return dict(event='test-window-unchanged', key=key, **position)
            atomic_write(dest, new)
        return dict(event='test-window-saved', key=key, **position)
    except Exception as error:
        return dict(event='test-window-error', key=key, error=repr(error))
    finally:
        release(key, pid)


def watch(session):
    """Wait for the session's game (session.json pid) to exit, then publish its placement."""
    session = Path(session).resolve()
    record = json.loads((session / 'session.json').read_text())
    pid, tw = record['pid'], record['test_window']
    while common.pid_alive(pid):
        time.sleep(POLL_SECONDS)
    time.sleep(0.5)
    event = dict(time=time.strftime('%Y-%m-%d %H:%M:%S'), **commit(record['state'], tw, pid))
    with open(session / 'test-window.jsonl', 'a') as f: f.write(json.dumps(event) + '\n')


def spawn_watch(session):
    subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'watch', str(session)],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                     start_new_session=True)


def main(argv):
    if len(argv) == 2 and argv[0] == 'watch':
        watch(argv[1])
        return 0
    if argv == ['list']:
        folder = store()
        out = dict(store=str(folder), enabled=enabled(), placements={}, claims={})
        for path in sorted(folder.glob('*.toml')) if folder.is_dir() else []:
            out['placements'][path.stem] = parse_window_position(path.read_bytes())
        for path in sorted((folder / 'claims').glob('*.json')) if (folder / 'claims').is_dir() else []:
            if _claim_live(path): out['claims'][path.stem] = json.loads(path.read_text())
        print(json.dumps(out, indent=2))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
