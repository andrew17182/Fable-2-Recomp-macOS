#!/usr/bin/env python3
"""Shared warm shader cache for agent test sessions (local agent docs, "Agent shader cache").

Agent test sessions used to start from a clone of the player's live cache and never write back, so every test game
redid ~178 native-shader optimizations, NS2/ns-spec learning and native-translator compiles (10-14 s before the
game reported ready). This keeps one cache per shader-generation fingerprint that agent sessions seed from and
grow, without ever writing to the live state.

  MAIN_ROOT/out/agent-cache/<key>/
    meta.json              key, components, created, seeded_from, generation (latest), last_used, last_published
    gen-NNNNNN/            one immutable generation: the session's `cache/` folder content
    gen-NNNNNN.json        its manifest: files {rel: size, mtime_ns, sha256}, content_hash, parent, source, changes
    pins/<name>.json       frozen snapshots (A/B batches): {generation, created, label}; kept FABLE2_AGENT_CACHE_PIN_HOURS
  MAIN_ROOT/out/locks/agent-cache.lock   held (exclusive, briefly) by seeding, publishing, pinning and pruning

Key: sha256 over the shader-generation sources (translator, AIR optimizer, ns-spec, native-shader/pipeline-cache
and shader-storage code, the SDK patch hunks of the DXBC translator) and the toolchain pin
(backends/metal/dependencies.json: Metal shader converter, DXC). build_macos_metal.py records it in
out/build/mac-arm64-metal-release/metal-build.json (`shader_cache_key`), so it describes what was built.
A key is *publishable* when its fingerprint equals the one of an integration branch tip (FABLE2_AGENT_CACHE_BRANCHES,
default perf-staging,macos). Builds with other shader code (experiments) seed read-only from the closest key.

Modes (dev_session --cache-snapshot / f2diag --cache-snapshot / FABLE2_AGENT_CACHE):
  auto (default)  seed from the build's key (latest generation; created from the live cache once), publish on a
                  clean exit when publishable. No key for this build: the closest key, read-only. None: live clone.
  readonly        as auto, never publish
  KEY@GEN | PIN   a frozen generation (pinned: `pin` below) - both arms of an A/B start identical. Publishes only
                  when KEY is the build's own publishable key (growth never changes the pinned generation).
  cold            empty cache (cold-start measurements); never publishes
  off | live      the old behaviour: clone of the live cache; never publishes
  --cache-from DIR (explicit seed) always wins and never publishes.

CLI (python3 tools/fable2_agent_cache.py ...):
  key [--json]                     this checkout's key from source, the built app's key, publishable?
  status [--json]                  keys, generations, pins, sizes
  pin [--label L] [--key K]        freeze the current generation; prints KEY@GEN (use for every arm of a batch)
  unpin NAME|KEY@GEN
  prune [--dry-run]                old generations, old keys, size cap (also runs after every publish)
  watch-publish SESSION_DIR        (internal) wait for a fable2_test_control session's game to exit, then publish
Runs under /usr/bin/python3 (3.9).
"""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_common as common  # noqa: E402

ROOT = common.ROOT
CACHE_ROOT = Path(os.environ.get('FABLE2_AGENT_CACHE_DIR') or common.MAIN_ROOT / 'out/agent-cache')
LIVE_STATE = Path(os.environ.get('FABLE2_LIVE_STATE_ROOT') or
                  Path.home() / 'Library/Application Support/Fable II Recomp')
CLEAN_EXIT_MARKER = 'Title terminated; hard-exiting process.'
EVENT = 'agent-cache'  # writeback.jsonl event name (dev sessions) / agent-cache.jsonl (test_control sessions)

METAL = 'backends/metal/port/rex/graphics/metal/'


def _pair(name):
    return [METAL + name + '.h', METAL + name + '.cc']


# Shader-generation inputs, grouped (the grouping picks the closest key for a build with other shader code).
COMPONENTS = {
    'translator': _pair('metal_msl_translator') + _pair('dxbc_to_dxil_converter') + _pair('metal_shader_converter')
                  + ['sdk-patch:shader'],
    'optimizer': _pair('metal_air_optimizer') + _pair('metal_shader'),
    'ns_spec': _pair('metal_ns_specialize'),
    'pipeline': _pair('metal_pipeline_cache') + _pair('metal_shader_cache') + _pair('metal_stage_compile_cache'),
    'toolchain': ['backends/metal/dependencies.json'],
}
SDK_PATCHES = ['backends/metal/sdk-metal.patch', 'backends/metal/sdk-metal-native-scale.patch',
               'backends/metal/sdk-metal-rt-shrink.patch', 'backends/metal/sdk-metal-hle-census.patch']
# sdk-metal-gpucmd.patch (texture cache only, no shader hunks) is left out on purpose: listing a patch
# hashes its name even without shader hunks, which would change the key for no shader change.
# Add it here if it ever touches pipeline/shader/ or format/dxbc.
SHADER_PATCH_PATH = re.compile(r'(/pipeline/shader/|/format/dxbc)')
SEGMENT_RE = re.compile(r'\.metal\.seg[^/]*\.binarchive$')
KEY_RE = re.compile(r'^[0-9a-f]{16}$')


# ---------------------------------------------------------------- fingerprint

def _shader_patch_hunks(text):
    """The per-file sections of an SDK patch that touch the shader translator (pipeline/shader/, format/dxbc)."""
    out = []
    for section in re.split(r'(?m)^(?=--- a/)', text):
        m = re.match(r'--- a/(\S+)', section)
        if m and SHADER_PATCH_PATH.search(m.group(1)): out.append(section)
    return ''.join(out)


def _reader_worktree(root):
    def read(rel):
        try:
            return (Path(root) / rel).read_bytes()
        except OSError:
            return None
    return read


def _reader_git(root, ref):
    def read(rel):
        r = subprocess.run(['git', '-C', str(root), 'show', '%s:%s' % (ref, rel)], capture_output=True)
        return r.stdout if r.returncode == 0 else None
    return read


def fingerprint(read):
    """{'key': 16 hex, 'components': {name: 16 hex}} from a reader rel -> bytes|None."""
    comps = {}
    for name, inputs in COMPONENTS.items():
        h = hashlib.sha256()
        for rel in inputs:
            if rel == 'sdk-patch:shader':
                for patch in SDK_PATCHES:
                    data = read(patch)
                    h.update(patch.encode() + b'\0' + _shader_patch_hunks((data or b'').decode('utf-8', 'replace'))
                             .encode() + b'\0')
                continue
            data = read(rel)
            h.update(rel.encode() + b'\0' + (b'<missing>' if data is None else data) + b'\0')
        comps[name] = h.hexdigest()[:16]
    key = hashlib.sha256(json.dumps(comps, sort_keys=True).encode()).hexdigest()[:16]
    return dict(key=key, components=comps)


def source_fingerprint(root=ROOT):
    return fingerprint(_reader_worktree(root))


def branch_fingerprints(root=ROOT):
    """{branch: fingerprint} for the integration branches (FABLE2_AGENT_CACHE_BRANCHES)."""
    out = {}
    for ref in [b.strip() for b in os.environ.get('FABLE2_AGENT_CACHE_BRANCHES', 'perf-staging,macos').split(',')]:
        if not ref: continue
        if subprocess.run(['git', '-C', str(root), 'rev-parse', '--verify', '--quiet', ref + '^{commit}'],
                          capture_output=True).returncode != 0:
            continue
        out[ref] = fingerprint(_reader_git(root, ref))
    return out


def build_record(root=ROOT):
    """What build_macos_metal.py writes into metal-build.json: the key of the sources being built."""
    fp = source_fingerprint(root)
    return dict(fp, inputs=sorted({i for v in COMPONENTS.values() for i in v} | set(SDK_PATCHES)))


def app_key(app):
    """The shader_cache_key recorded for a built app (metal-build.json next to it), or None (older build)."""
    for folder in (Path(app).parent, Path(app) / 'Contents/Resources'):
        try:
            rec = json.loads((folder / 'metal-build.json').read_text()).get('shader_cache_key')
        except (OSError, ValueError):
            continue
        if rec and KEY_RE.match(str(rec.get('key', ''))): return rec
    return None


def publishable(build_fp, root=ROOT):
    """(bool, reason): the build's shader code equals an integration branch tip."""
    if not build_fp: return False, 'build has no shader_cache_key (built before the agent cache; rebuild)'
    for ref, fp in branch_fingerprints(root).items():
        if fp['key'] == build_fp['key']: return True, 'matches %s' % ref
    return False, 'shader code differs from %s (experimental: read-only)' % os.environ.get(
        'FABLE2_AGENT_CACHE_BRANCHES', 'perf-staging,macos')


# ---------------------------------------------------------------- storage helpers

@contextlib.contextmanager
def cache_lock(exclusive, what='agent cache', timeout=None):
    """MAIN_ROOT/out/locks/agent-cache.lock (FABLE2_LOCKS_DIR in tests)."""
    if timeout is None: timeout = common.env_float('FABLE2_AGENT_CACHE_LOCK_TIMEOUT', 600)
    common.LOCKS.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(common.LOCKS / 'agent-cache.lock'), os.O_RDWR | os.O_CREAT, 0o644)
    op = fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH
    start, said = time.monotonic(), 0
    try:
        while True:
            try:
                fcntl.flock(fd, op | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                waited = time.monotonic() - start
                if waited > timeout: raise common.LockTimeout('AGENT CACHE: %s waited %.0f s for the lock' % (what, waited))
                if waited - said >= 30:
                    said = waited
                    common.log('AGENT CACHE: %s waiting for the lock (%.0f s)' % (what, waited))
                time.sleep(0.1)
        yield
    finally:
        os.close(fd)


def _atomic_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp-%d-%s' % (os.getpid(), uuid.uuid4().hex[:6]))
    tmp.write_text(json.dumps(data, indent=2) + '\n')
    os.replace(tmp, path)


def _read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return default


def key_dir(key):
    return CACHE_ROOT / key


def gen_dir(key, gen):
    return key_dir(key) / ('gen-%06d' % gen)


def gen_manifest_path(key, gen):
    return key_dir(key) / ('gen-%06d.json' % gen)


def load_meta(key):
    return _read_json(key_dir(key) / 'meta.json')


def keys():
    if not CACHE_ROOT.is_dir(): return []
    return sorted(d.name for d in CACHE_ROOT.iterdir() if d.is_dir() and KEY_RE.match(d.name)
                  and (d / 'meta.json').is_file())


def _sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''): h.update(block)
    return h.hexdigest()


def _prefix_sha(path, size):
    h, left = hashlib.sha256(), size
    with open(path, 'rb') as f:
        while left > 0:
            block = f.read(min(1 << 20, left))
            if not block: break
            h.update(block)
            left -= len(block)
    return h.hexdigest() if left == 0 else None


def _files(root):
    root = Path(root)
    if not root.is_dir(): return []
    return sorted(str(p.relative_to(root)) for p in root.rglob('*')
                  if p.is_file() and not p.is_symlink() and '.tmp-' not in p.name)


def signature(root, known=None):
    """{rel: size, mtime_ns, sha256}; files whose size and mtime match `known` reuse its hash (clones keep mtimes)."""
    out, root = {}, Path(root)
    for rel in _files(root):
        st = (root / rel).stat()
        k = (known or {}).get(rel)
        if k and k['size'] == st.st_size and k['mtime_ns'] == st.st_mtime_ns:
            out[rel] = dict(k)
        else:
            out[rel] = dict(size=st.st_size, mtime_ns=st.st_mtime_ns, sha256=_sha(root / rel))
    return out


def content_hash(files):
    return hashlib.sha256(json.dumps(sorted((r, f['sha256']) for r, f in files.items())).encode()).hexdigest()[:16]


def _clone_file(src, dst):
    common.clone(src, dst)  # cp -cRp: an APFS clone sharing the session file's blocks, mtime kept


def logical_size(path):
    total = 0
    for p in Path(path).rglob('*'):
        try:
            if p.is_file() and not p.is_symlink(): total += p.stat().st_size
        except OSError:
            pass
    return total


# ---------------------------------------------------------------- keys and generations

def ensure_key(build_fp, source_cache=None):
    """Create <key>/gen-000000 from an APFS clone of the live cache (read only) if the key is new. Exclusive lock
    held by the caller. Returns the meta."""
    key = build_fp['key']
    meta = load_meta(key)
    if meta: return meta
    source_cache = Path(source_cache or LIVE_STATE / 'cache')
    kd = key_dir(key)
    kd.mkdir(parents=True, exist_ok=True)
    tmp = kd / ('gen-000000.tmp-' + uuid.uuid4().hex[:8])
    if source_cache.is_dir():
        common.clone(source_cache, tmp)  # cp -cRp: reads the source only
    else:
        tmp.mkdir(parents=True)
    files = signature(tmp)
    os.replace(tmp, gen_dir(key, 0))
    _atomic_json(gen_manifest_path(key, 0), dict(generation=0, created=time.time(), parent=None,
                                                  source=dict(kind='seed', path=str(source_cache)),
                                                  files=files, content_hash=content_hash(files)))
    meta = dict(key=key, components=build_fp['components'], created=time.time(), seeded_from=str(source_cache),
                generation=0, last_used=time.time(), last_published=None)
    _atomic_json(kd / 'meta.json', meta)
    return meta


def closest_key(build_fp):
    """The key sharing the most components with build_fp (toolchain must match), newest first; or None."""
    best = None
    for k in keys():
        if k == build_fp['key']: return k
        meta = load_meta(k) or {}
        comps = meta.get('components') or {}
        if comps.get('toolchain') != build_fp['components'].get('toolchain'): continue
        score = sum(comps.get(n) == v for n, v in build_fp['components'].items())
        when = meta.get('last_published') or meta.get('created') or 0
        cand = (score, when, k)
        if best is None or cand > best: best = cand
    return best[2] if best else None


def resolve_pin(spec):
    """'KEY@GEN' or a pin name -> (key, gen) or raises ValueError."""
    if '@' in spec:
        key, _, gen = spec.partition('@')
        if not KEY_RE.match(key) or not gen.isdigit(): raise ValueError('cache snapshot must be KEY@GEN or a pin name')
        return key, int(gen)
    for k in keys():
        pin = _read_json(key_dir(k) / 'pins' / (spec + '.json'))
        if pin: return k, int(pin['generation'])
    raise ValueError('no agent-cache pin named %r (python3 tools/fable2_agent_cache.py status)' % spec)


def _touch_used(key):
    meta = load_meta(key)
    if meta:
        meta['last_used'] = time.time()
        _atomic_json(key_dir(key) / 'meta.json', meta)


def seed(dest, app, mode=None, root=ROOT, live_state=None):
    """Fill `dest` (a session's cache/ folder, must not exist) for a test session of `app`.
    Returns the agent_cache record for session.json / result.json:
      mode, key, build_key, generation, content_hash, source, publish (bool), reason."""
    dest = Path(dest)
    live_cache = Path(live_state or LIVE_STATE) / 'cache'
    mode = (mode or os.environ.get('FABLE2_AGENT_CACHE') or 'auto').strip()
    build_fp = app_key(app)
    rec = dict(mode=mode, build_key=build_fp['key'] if build_fp else None, key=None, generation=None,
               content_hash=None, source=None, publish=False)
    if mode == 'cold':
        dest.mkdir(parents=True)
        rec.update(source='cold (empty)', reason='cold seed')
        return rec
    if mode in ('off', 'live'):
        if live_cache.exists(): common.clone(live_cache, dest)
        rec.update(source='live clone', reason='agent cache off')
        return rec
    ok, why = publishable(build_fp, root) if build_fp else (False, 'build has no shader_cache_key (rebuild)')
    pinned = mode not in ('auto', 'readonly')
    with cache_lock(exclusive=True, what='seed'):
        if pinned:
            key, gen = resolve_pin(mode)
            if not gen_dir(key, gen).is_dir():
                raise ValueError('agent-cache snapshot %s@%d is gone (pruned?); pin a new one' % (key, gen))
        else:
            key = None
            if build_fp and ok:
                key = ensure_key(build_fp, live_cache)['key']
            elif build_fp:
                key = closest_key(build_fp)
            if key is None:
                if live_cache.exists(): common.clone(live_cache, dest)
                rec.update(source='live clone', reason='no agent-cache key for this build: ' + why)
                return rec
            gen = int(load_meta(key)['generation'])
        manifest = _read_json(gen_manifest_path(key, gen)) or {}
        common.clone(gen_dir(key, gen), dest)
        _touch_used(key)
    own = build_fp is not None and key == build_fp['key']
    publish = own and ok and mode != 'readonly'
    reason = why if own else ('closest key %s (this build: %s): read-only' % (key, why))
    if mode == 'readonly': reason = 'readonly mode'
    rec.update(key=key, generation=gen, content_hash=manifest.get('content_hash'), source=str(gen_dir(key, gen)),
               publish=publish, reason=reason, signature='%s@%d:%s' % (key, gen, manifest.get('content_hash')))
    return rec


# ---------------------------------------------------------------- publish

def plan_publish(session_files, seed_files, current_files, session_root):
    """Per-file decisions for publishing a session cache grown from `seed_files` into a generation whose files are
    `current_files` (another session may have published since the seed). Append-only, as the launcher's
    publish_cache: a changed file must extend the seed's version; new files are added; archive segments the session
    dropped (compaction) are removed. Conflicts with a concurrent publish keep the current version.
    Returns dict(updates=[rel], deletions=[rel], skipped={rel: reason})."""
    updates, deletions, skipped = [], [], {}
    for rel, s in session_files.items():
        b, c = seed_files.get(rel), current_files.get(rel)
        if b and s['sha256'] == b['sha256']: continue  # unchanged by this session
        if c and s['sha256'] == c['sha256']: continue  # already there
        if b and (s['size'] < b['size'] or _prefix_sha(session_root / rel, b['size']) != b['sha256']):
            skipped[rel] = 'not an extension of the seed file'
            continue
        if (c is None and b is None) or (c and b and c['sha256'] == b['sha256']):
            updates.append(rel)  # new file, or grown from the version still current
        elif c and s['size'] > c['size'] and _prefix_sha(session_root / rel, c['size']) == c['sha256']:
            updates.append(rel)  # another session grew it too, and this one extends that
        elif c is None:
            skipped[rel] = 'removed from the agent cache since the seed'
        else:
            skipped[rel] = 'changed by another session since the seed'
    for rel, b in seed_files.items():
        if SEGMENT_RE.search(rel) and rel not in session_files:
            c = current_files.get(rel)
            if c and c['sha256'] == b['sha256']: deletions.append(rel)
    return dict(updates=sorted(updates), deletions=sorted(deletions), skipped=skipped)


def clean_exit(state):
    try:
        return CLEAN_EXIT_MARKER in (Path(state) / 'logs/fable_2.log').read_text(errors='replace')
    except OSError:
        return False


def publish(state, rec, session_name=None):
    """Publish a finished session's new cache artifacts as a new generation of rec['key']. Returns a summary dict
    (event fields). Never raises for expected refusals; those come back as {'result': 'skipped', 'reason': ...}."""
    if not rec or not rec.get('publish'):
        return dict(result='skipped', reason=(rec or {}).get('reason') or 'not an agent-cache session')
    if not clean_exit(state):
        return dict(result='skipped', reason='game did not exit cleanly')
    key, seed_gen = rec['key'], int(rec['generation'])
    session_cache = Path(state) / 'cache'
    seed_files = (_read_json(gen_manifest_path(key, seed_gen)) or {}).get('files')
    if seed_files is None: return dict(result='skipped', reason='seed manifest %s@%d missing' % (key, seed_gen))
    started = time.monotonic()
    session_files = signature(session_cache, seed_files)
    with cache_lock(exclusive=True, what='publish'):
        meta = load_meta(key)
        if not meta: return dict(result='skipped', reason='key %s was pruned' % key)
        cur = int(meta['generation'])
        current_files = (_read_json(gen_manifest_path(key, cur)) or {}).get('files') or {}
        plan = plan_publish(session_files, seed_files, current_files, session_cache)
        base = dict(key=key, seed_generation=seed_gen, base_generation=cur, skipped=plan['skipped'] or None)
        if not plan['updates'] and not plan['deletions']:
            return dict(base, result='unchanged', generation=cur, s=round(time.monotonic() - started, 2))
        new = cur + 1
        tmp = key_dir(key) / ('gen-%06d.tmp-%s' % (new, uuid.uuid4().hex[:8]))
        common.clone(gen_dir(key, cur), tmp)
        files = dict(current_files)
        for rel in plan['updates']:
            dst = tmp / rel
            if dst.exists(): dst.unlink()
            _clone_file(session_cache / rel, dst)
            st = dst.stat()
            files[rel] = dict(session_files[rel], size=st.st_size, mtime_ns=st.st_mtime_ns)
        for rel in plan['deletions']:
            (tmp / rel).unlink(missing_ok=True)
            files.pop(rel, None)
        os.replace(tmp, gen_dir(key, new))
        _atomic_json(gen_manifest_path(key, new), dict(
            generation=new, created=time.time(), parent=cur, content_hash=content_hash(files), files=files,
            source=dict(kind='session', session=session_name, seed_generation=seed_gen),
            updated=plan['updates'], deleted=plan['deletions']))
        meta.update(generation=new, last_published=time.time(), last_used=time.time())
        _atomic_json(key_dir(key) / 'meta.json', meta)
        pruned = prune_locked()
        return dict(base, result='published', generation=new, updated=plan['updates'], deleted=plan['deletions'],
                    content_hash=content_hash(files), pruned=pruned or None, s=round(time.monotonic() - started, 2))


def publish_logged(state, rec, log, session_name=None):
    try:
        log(dict(event=EVENT, **publish(state, rec, session_name)))
    except Exception as error:
        log(dict(event=EVENT, result='error', error=repr(error)))


def wait_published(session, timeout=None):
    """Block until the session's watcher logged its agent-cache event (a readiness signal: the next session seeds
    from the new generation). Returns the event, or None when the guard fires / the session doesn't publish."""
    session = Path(session)
    rec = (_read_json(session / 'session.json') or {}).get('agent_cache') or {}
    if not rec.get('publish'): return None
    if timeout is None: timeout = common.env_float('FABLE2_AGENT_CACHE_PUBLISH_WAIT', 120)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for name in ('writeback.jsonl', 'agent-cache.jsonl'):
            try:
                for line in (session / name).read_text().splitlines():
                    ev = json.loads(line)
                    if ev.get('event') == EVENT: return ev
            except (OSError, ValueError):
                pass
        time.sleep(0.1)
    common.log('AGENT CACHE: no publish event from %s after %.0f s (guard)' % (session.name, timeout))
    return None


# ---------------------------------------------------------------- pins

def pin(label=None, key=None, app=None):
    if key is None:
        fp = app_key(app or default_app())
        if not fp: raise ValueError('the built app has no shader_cache_key; rebuild, or pass --key')
        key = fp['key'] if load_meta(fp['key']) else closest_key(fp)
        if key is None: raise ValueError('no agent-cache key yet; run one test session first')
    with cache_lock(exclusive=True, what='pin'):
        meta = load_meta(key)
        if not meta: raise ValueError('no agent-cache key %s' % key)
        gen = int(meta['generation'])
        name = label or ('pin-%s' % time.strftime('%Y%m%d-%H%M%S'))
        if not re.match(r'^[A-Za-z0-9._-]+$', name): raise ValueError('pin label: letters, digits, . _ - only')
        _atomic_json(key_dir(key) / 'pins' / (name + '.json'), dict(generation=gen, created=time.time(), label=name,
                                                                    checkout=common.SLOT or 'main'))
    return dict(key=key, generation=gen, name=name, snapshot='%s@%d' % (key, gen))


def unpin(spec):
    removed = []
    with cache_lock(exclusive=True, what='unpin'):
        for k in keys():
            for p in (key_dir(k) / 'pins').glob('*.json'):
                pin_ = _read_json(p) or {}
                if p.stem == spec or '%s@%s' % (k, pin_.get('generation')) == spec:
                    p.unlink()
                    removed.append('%s/%s' % (k, p.stem))
    return removed


# ---------------------------------------------------------------- prune / status

def _pinned_gens(key, now):
    hours = common.env_float('FABLE2_AGENT_CACHE_PIN_HOURS', 48)
    out = set()
    for p in (key_dir(key) / 'pins').glob('*.json'):
        pin_ = _read_json(p) or {}
        if now - pin_.get('created', 0) > hours * 3600:
            p.unlink(missing_ok=True)
            continue
        out.add(int(pin_.get('generation', -1)))
    return out


def _gens(key):
    return sorted(int(p.name[4:]) for p in key_dir(key).glob('gen-*') if p.is_dir() and p.name[4:].isdigit())


def prune_locked(dry_run=False, now=None):
    """Exclusive lock held. Per key keep the newest FABLE2_AGENT_CACHE_KEEP_GENS (2) generations plus pinned ones;
    drop keys unused for FABLE2_AGENT_CACHE_KEY_DAYS (14); then, while the logical total exceeds
    FABLE2_AGENT_CACHE_MAX_GB (30), drop the least recently used key (never the most recently used one).
    Manifests stay with their key (publishing needs the seed manifest). Stale .tmp folders go too."""
    now = now or time.time()
    keep_gens = max(1, int(common.env_float('FABLE2_AGENT_CACHE_KEEP_GENS', 2)))
    removed = []

    def rm(path, why):
        removed.append('%s (%s)' % (Path(path).relative_to(CACHE_ROOT), why))
        if not dry_run: shutil.rmtree(path, ignore_errors=True)

    for k in keys():
        for tmp in key_dir(k).glob('gen-*.tmp-*'):
            if now - tmp.stat().st_mtime > 3600: rm(tmp, 'stale temporary')
        meta = load_meta(k)
        latest = int(meta['generation'])
        pinned = _pinned_gens(k, now)
        for g in _gens(k):
            if g != latest and g not in pinned and g < latest - keep_gens + 1: rm(gen_dir(k, g), 'old generation')
    by_use = sorted(keys(), key=lambda k: (load_meta(k) or {}).get('last_used') or 0)
    days = common.env_float('FABLE2_AGENT_CACHE_KEY_DAYS', 14)
    for k in by_use[:-1]:
        if now - ((load_meta(k) or {}).get('last_used') or 0) > days * 86400: rm(key_dir(k), 'unused %.0f days' % days)
    cap = common.env_float('FABLE2_AGENT_CACHE_MAX_GB', 30) * common.GB
    alive = [k for k in by_use if not any(r.startswith(k + ' ') for r in removed)]
    sizes = {k: logical_size(key_dir(k)) for k in alive}
    total = sum(sizes.values())
    for k in alive[:-1]:
        if total <= cap: break
        rm(key_dir(k), 'size cap %.0f GB' % (cap / common.GB))
        total -= sizes[k]
    return removed


def prune(dry_run=False):
    if not CACHE_ROOT.is_dir(): return []
    with cache_lock(exclusive=True, what='prune'):
        return prune_locked(dry_run)


def default_app():
    return ROOT / 'out/build/mac-arm64-metal-release/Fable II.app'


def status():
    rows = []
    for k in keys():
        meta = load_meta(k)
        gens = _gens(k)
        latest = int(meta['generation'])
        man = _read_json(gen_manifest_path(k, latest)) or {}
        pins = [dict(name=p.stem, generation=(_read_json(p) or {}).get('generation'))
                for p in sorted((key_dir(k) / 'pins').glob('*.json'))]
        rows.append(dict(key=k, generation=latest, generations=gens, content_hash=man.get('content_hash'),
                         files=len(man.get('files') or {}),
                         latest_bytes=sum(f['size'] for f in (man.get('files') or {}).values()),
                         logical_bytes=logical_size(key_dir(k)), last_used=meta.get('last_used'),
                         last_published=meta.get('last_published'), pins=pins, components=meta.get('components')))
    return dict(root=str(CACHE_ROOT), keys=rows, total_logical_bytes=sum(r['logical_bytes'] for r in rows),
                cap_gb=common.env_float('FABLE2_AGENT_CACHE_MAX_GB', 30))


def status_lines():
    s = status()
    if not s['keys']: return ['Agent shader cache: empty (%s)' % s['root']]
    out = ['Agent shader cache: %d key(s), %.1f GB logical (clones share blocks; cap %.0f GB), %s'
           % (len(s['keys']), s['total_logical_bytes'] / common.GB, s['cap_gb'], s['root'])]
    now = time.time()
    for r in sorted(s['keys'], key=lambda r: -(r['last_used'] or 0)):
        pins = ', '.join('%s@%s' % (p['name'], p['generation']) for p in r['pins'])
        out.append('  %s gen %d (%d kept), %d files %.0f MB, used %s ago%s'
                   % (r['key'], r['generation'], len(r['generations']), r['files'], r['latest_bytes'] / 1e6,
                      _ago(now - (r['last_used'] or now)), ('; pins ' + pins) if pins else ''))
    return out


def _ago(s):
    return '%.0f s' % s if s < 90 else ('%.0f min' % (s / 60) if s < 5400 else '%.1f h' % (s / 3600))


# ---------------------------------------------------------------- test_control sessions

def spawn_watch(session):
    """Detached publisher for a fable2_test_control session (no dev-session watcher there)."""
    log = open(Path(session) / 'agent-cache-watch.log', 'ab')
    subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'watch-publish', str(session)],
                     stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)


def watch_publish(session):
    session = Path(session)
    meta = _read_json(session / 'session.json') or {}
    pid, rec = meta.get('pid'), meta.get('agent_cache')

    def log(ev):
        ev = dict(time=time.strftime('%Y-%m-%d %H:%M:%S'), **ev)
        with open(session / 'agent-cache.jsonl', 'a') as f: f.write(json.dumps(ev) + '\n')
    if not rec or not rec.get('publish') or not pid:
        log(dict(event=EVENT, result='skipped', reason=(rec or {}).get('reason') or 'no pid'))
        return
    while common.pid_alive(pid): time.sleep(0.5)
    deadline = time.monotonic() + 5  # the final log line lands just before the process ends
    while not clean_exit(meta['state']) and time.monotonic() < deadline: time.sleep(0.2)
    publish_logged(meta['state'], rec, log, session.name)


# ---------------------------------------------------------------- CLI

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    k = sub.add_parser('key')
    k.add_argument('--json', action='store_true')
    s = sub.add_parser('status')
    s.add_argument('--json', action='store_true')
    q = sub.add_parser('pin')
    q.add_argument('--label')
    q.add_argument('--key')
    u = sub.add_parser('unpin')
    u.add_argument('spec')
    r = sub.add_parser('prune')
    r.add_argument('--dry-run', action='store_true')
    w = sub.add_parser('watch-publish')
    w.add_argument('session')
    a = p.parse_args(argv)
    if a.cmd == 'key':
        src, built = source_fingerprint(), app_key(default_app())
        ok, why = publishable(built)
        out = dict(source=src, built=built, publishable=ok, reason=why,
                   branches={b: f['key'] for b, f in branch_fingerprints().items()})
        if a.json: print(json.dumps(out, indent=2))
        else:
            print('source key %s; built app key %s; %s (%s)' % (src['key'], built and built['key'],
                                                               'publishable' if ok else 'read-only', why))
    elif a.cmd == 'status':
        print(json.dumps(status(), indent=2) if a.json else '\n'.join(status_lines()))
    elif a.cmd == 'pin':
        res = pin(a.label, a.key)
        common.log('pinned %s (use --cache-snapshot %s or FABLE2_AGENT_CACHE=%s for every arm)'
                   % (res['snapshot'], res['snapshot'], res['snapshot']))
        print(res['snapshot'])
    elif a.cmd == 'unpin':
        print(json.dumps(unpin(a.spec)))
    elif a.cmd == 'prune':
        print('\n'.join(prune(a.dry_run)) or 'nothing to prune')
    elif a.cmd == 'watch-publish':
        watch_publish(a.session)
    return 0


if __name__ == '__main__':
    sys.exit(main())
