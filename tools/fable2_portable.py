#!/usr/bin/env python3
"""Portable test build: a self-contained "Fable II.app" for another Apple Silicon Mac (docs/portable-build.md).

  python3 tools/fable2_portable.py build [--out DIR] [--no-zip] [--no-skip-intro]
                                         [--shader-scales 1,2,3] [--shaders CACHE_DIR ...] [--no-shaders]
                                         [--with-converter | --no-rebuild]

Packages the no-converter release build (out/build-release, docs/no-converter.md; release-cleanup D3 decided
2026-10-14): neither libdxilconv nor libmetalirconverter is in the app, and the package check refuses one that is.
The release build is refreshed first (tools/build_macos_metal.py --release, incremental; log
out/build-release/build.log) unless --no-rebuild. --with-converter packages the dev build (out/build) as it is,
converter dylibs included, as before.

Makes <DIR>/Fable II Portable/ (default DIR: out/portable) with "Fable II.app" and README.txt, plus
"Fable II Portable.zip" next to it. The app holds only our compiled code and our own side files:
  Contents/MacOS/      FableIIPlayer (main executable: the launcher window, tools/macos/Player/main.swift +
                       tools/macos/Shared/*.swift, docs/player-app.md), fable_2, its dylibs, FableIILauncher
                       (AppLauncher.m: Play execs it; also used by the benchmark kit) and FableIIPrepare
                       (disc-image extractor built from tools/macos/Setup/GDFX.swift + Support.swift +
                       tools/macos/Prepare/main.swift)
  Contents/Resources/  default configs, recomp-lua/ (src/lua), optional native-shaders/ (see below)
  Info.plist           Fable2Portable, Fable2XexSHA256 (the default.xex this build was compiled from),
                       Fable2SkipIntroDefault, own bundle id org.fable2recomp.metal.portable
No game content: on first launch the app asks for the player's Fable II disc image and extracts it into
~/Library/Application Support/Fable II Recomp/GameData (FableIIPlayer + FableIIPrepare).

native-shaders/ (default): optimized pixel shaders (NS1 records), the translated stages and the shareable
pipeline lists, so a Mac without Xcode still gets the optimized shaders (docs/native-shaders.md). They come
from a package-time warm-up: one background test session per resolution scale (pid-scoped, muted,
input-isolated, closed by session id) with the converter target pinned to MSC_PIN, which the portable
launcher also sets, so the cache keys match on every Apple Silicon Mac with macOS 15+. The live cache is
only read (its pipeline lists seed the warm-up). These files are compiled from the game's shaders:
game-derived, private to the package, never committed (out/ is ignored).
Runs under /usr/bin/python3 (3.9).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_version  # noqa: E402
import package_macos_app  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEV_RUNTIME = ROOT / 'out/build/mac-arm64-metal-release'
RELEASE_RUNTIME = ROOT / 'out/build-release/mac-arm64-metal-release'
RELEASE_LOG = ROOT / 'out/build-release/build.log'
RUNTIME = RELEASE_RUNTIME  # what build() packages (DEV_RUNTIME with --with-converter)
CONVERTER_LIBRARIES = package_macos_app.CONVERTER_LIBRARIES
DEFAULT_OUT = ROOT / 'out/portable'
LIVE_CACHE = Path.home() / 'Library/Application Support/Fable II Recomp/cache'
BUNDLE_ID = 'org.fable2recomp.metal.portable'
PACKAGE_NAME = 'Fable II Portable'
TITLE = '4D5307F1'
# Converter target pinned in the portable build (FABLE2_MSC_MIN_TARGET, metal_pipeline_cache.cc): the plain
# shader libraries, and so every cache key, then match on any Apple Silicon Mac with macOS 15 or later.
MSC_PIN = 'apple7:15.0'
WARM_SAVE = 'hero1-dog-live_2026-10-07_18-49-47'  # f2diag's default save; else the newest save-library entry
# The one Fable II app (player role; the Dev Launcher role is the same binary, tools/macos/build_dev_launcher.sh):
# its entry point, the Dev Launcher sources and every shared file (docs/player-app.md "Developer mode").
PLAYER_SOURCES = (['tools/macos/Player/main.swift']
                  + sorted('tools/macos/DevLauncher/' + p.name for p in (ROOT / 'tools/macos/DevLauncher').glob('*.swift'))
                  + sorted('tools/macos/Shared/' + p.name for p in (ROOT / 'tools/macos/Shared').glob('*.swift')))
PLAYER_EXECUTABLE = 'FableIIPlayer'
PREPARE_SOURCES = ['tools/macos/Prepare/main.swift', 'tools/macos/Prepare/ShaderLists.swift', 'tools/macos/Setup/GDFX.swift',
                   'tools/macos/Setup/Support.swift']
# Anything that looks like disc content must never be inside the package.
GAME_CONTENT = re.compile(r'(^|/)(default\.xex|data|nxeart|\$SystemUpdate|out/skipintro)(/|$)|\.(bik|xex|iso)$', re.I)

README = """Fable II for macOS (portable test build) {version}

Needs: a Mac with Apple Silicon (M1 or later), macOS {min_os} or later, about 7 GB of free space,
and your own Fable II disc image (.iso). Xcode is not needed.

FIRST LAUNCH
1. Copy the "Fable II.app" folder to the Mac (e.g. into Applications).
2. macOS blocks apps that are not notarized (this one is only ad-hoc signed). Open the app,
   click Done in the warning, then System Settings > Privacy & Security > "Open Anyway"
   (macOS 15 and later no longer offer right-click > Open for this). Or, once, in Terminal:
       xattr -dr com.apple.quarantine "/path/to/Fable II.app"
3. The Welcome window asks for your Fable II disc image (or offers game files already on this
   Mac, e.g. from the Fable II Benchmark). The game files (default.xex, data, nxeart,
   $SystemUpdate) are copied from it into
       ~/Library/Application Support/Fable II Recomp/GameData
   with a progress bar ("Show details" shows the log). The image itself is only read. If it
   differs from the one this build was compiled from, you are warned and can continue anyway.

EVERY LAUNCH shows the Fable II window: Continue or title screen, resolution (Auto picks one for
this Mac), frame rate, windowed or fullscreen, and Play. Fable II > Settings (Cmd-,) has every
other option. When you quit the game, the app quits; open it again for this window.
- "Skip launcher, play directly" (Settings > General) starts the game at once; hold Option while
  opening the app to see the window again.
- Choose another disc image: File > Choose Disc Image... (or Settings > General).
- Saves, settings and logs: ~/Library/Application Support/Fable II Recomp/
  (logs/fable_2.log, logs/launcher.log, logs/prepare.log; Settings > Advanced > Open Logs Folder).

This package contains no game content: only the compiled port and its own files.
{shader_note}Do not redistribute it.
"""
SHADER_NOTE = """It does include optimized shaders compiled from the game's own shaders on the build Mac
(Contents/Resources/native-shaders). They are derived from the game: keep this package private.
"""


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def expected_xex_sha256():
    """The default.xex this build was compiled against (src/core/xex_verify.h)."""
    text = (ROOT / 'src/core/xex_verify.h').read_text()
    match = re.search(r'kExpectedSha256\[\]\s*=\s*"([0-9a-f]{64})"', text)
    if not match: raise RuntimeError('kExpectedSha256 not found in src/core/xex_verify.h')
    expected = match.group(1)
    local = ROOT / 'default.xex'
    if local.is_file() and sha256(local) != expected:
        print('warning: the checkout\'s default.xex differs from xex_verify.h; using xex_verify.h', file=sys.stderr)
    return expected


# --- Native shader seed -----------------------------------------------------------------------------

def _constant(path, pattern):
    match = re.search(pattern, (ROOT / path).read_text())
    if not match: raise RuntimeError(f'{pattern} not found in {path}')
    return int(match.group(1), 0)


def shader_abi_tag():
    """Same folder name as MetalPipelineCache::GetShaderStorageAbiTag()."""
    msc = _constant('out/metal-build/dependencies/xenios-source/src/xenia/gpu/dxbc_shader_translator.h',
                    r'kVersion\s*=\s*(0x[0-9A-Fa-f]+)')
    ps = _constant('backends/metal/port/rex/graphics/metal/metal_pipeline_cache.h', r'kVersion\s*=\s*(0x[0-9A-Fa-f]+)')
    sh = _constant('backends/metal/port/rex/graphics/metal/metal_shader_cache.h', r'kStorageVersion\s*=\s*(\d+)')
    return 'msc%08X-ps%08X-sh%08X' % (msc, ps, sh)


TOOLCHAIN_HASH_C = r'''
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <stdio.h>
int main(void) { static char b[1 << 16]; size_t n = fread(b, 1, sizeof b, stdin);
  printf("%016llX\n", (unsigned long long)(XXH3_64bits(b, n) ^ 0x4E53314F32ULL)); return 0; }
'''


def toolchain_hash(work):
    """Same value as AirOptimizerToolchainHash() on this Mac: xxh3(`metal-opt --version`) ^ "NS1O2"."""
    metal_opt = subprocess.run(['xcrun', '--find', 'metal-opt'], capture_output=True, text=True).stdout.strip()
    headers = [p for p in (ROOT / 'out/sdk-src/source/thirdparty/xxHash', ROOT / 'out/sdk/mac-arm64/include')
               if (p / 'xxhash.h').is_file()]
    if not metal_opt or not headers: return None
    source, binary = work / 'tchash.c', work / 'tchash'
    source.write_text(TOOLCHAIN_HASH_C)
    run('clang', '-O1', '-I', headers[0], source, '-o', binary)
    version = subprocess.run([metal_opt, '--version'], capture_output=True).stdout
    return subprocess.run([str(binary)], input=version, capture_output=True, check=True).stdout.decode().strip()


FILE_MAGIC, RECORD_MAGIC, STORE_VERSION = 0x46414D58, 0x52414D58, 3
RECORD = struct.Struct('<IIQQIIIIQ')  # ArtifactRecordHeader (metal_shader_cache.cc)
KIND_DXIL = 4
NS1_VERSION = 0x4E530001
# Hot-shader specialization (FABLE2_NS_SPECIALIZE): specialized library and its guard-list index record.
NS2_VERSIONS = (0x4E530002, 0x4E530003)


def read_records(path):
    """Valid records of a .metal.artifacts file (stops at a torn tail, as LoadIndex does)."""
    data = path.read_bytes()
    if len(data) < 8 or struct.unpack_from('<II', data, 0) != (FILE_MAGIC, STORE_VERSION): return []
    records, at = [], 8
    while at + RECORD.size <= len(data):
        magic, version, primary, modification, stage, kind, size, _, _ = RECORD.unpack_from(data, at)
        if magic != RECORD_MAGIC or version != STORE_VERSION or at + RECORD.size + size > len(data): break
        records.append(((primary, modification, stage, kind), data[at:at + RECORD.size + size]))
        at += RECORD.size + size
    return records


def ns1_toolchain(record, versions=(NS1_VERSION,) + NS2_VERSIONS):
    """The toolchain hash of an NS1 (optimized library) or NS2 (specialized) record, else None."""
    (_, _, _, kind), raw = record
    if kind != KIND_DXIL: return None
    payload = raw[RECORD.size:]
    if len(payload) < 8 + 56: return None
    key_size, _ = struct.unpack_from('<II', payload, 0)
    if key_size != 56: return None
    words = struct.unpack_from('<7Q', payload, 8)
    return words[4] if words[5] in versions else None


def build_shader_seed(sources, resources, abi, toolchain, drop_optimized=False):
    """Merge the artifact stores of `sources` (cache roots) into native-shaders/; returns stats or None.
    drop_optimized: leave out NS1/NS2 (metal-opt optimized / specialized) records. A long-lived cache (the agent cache)
    carries hundreds of them from before the native translator (default on since 2026-10-13), which replaces them:
    a warm-up or prep cache of this build has none."""
    seed = resources / 'native-shaders'
    merged, stores, shareable = {}, [], {}
    for cache in sources:
        shaders = cache / 'shaders'
        stores += sorted(shaders.glob(f'local/metal/*/{abi}/{TITLE}/{TITLE}.metal.artifacts'))
        for name in (f'{TITLE}.xsh', f'{TITLE}.metal.xpso'):
            path = shaders / 'shareable' / name
            if path.is_file() and (name not in shareable or path.stat().st_size > shareable[name].stat().st_size):
                shareable[name] = path
    other_toolchains = dropped_optimized = 0
    for store in stores:
        for key, raw in read_records(store):
            tc = ns1_toolchain((key, raw))
            if tc is not None and drop_optimized:
                dropped_optimized += 1
                continue
            if tc is not None and toolchain and tc != int(toolchain, 16):
                other_toolchains += 1
                continue
            merged[key] = raw  # later records win, as in LoadIndex
    if not merged and not shareable: return None
    if merged:
        out = seed / 'local' / abi / TITLE / f'{TITLE}.metal.artifacts'
        out.parent.mkdir(parents=True)
        with open(out, 'wb') as f:
            f.write(struct.pack('<II', FILE_MAGIC, STORE_VERSION))
            for raw in merged.values(): f.write(raw)
    if shareable:
        (seed / 'shareable').mkdir(parents=True)
        for name, path in shareable.items(): shutil.copy2(path, seed / 'shareable' / name)
    ns1 = sum(1 for item in merged.items() if ns1_toolchain(item, (NS1_VERSION,)) is not None)
    ns2 = sum(1 for item in merged.items() if ns1_toolchain(item, NS2_VERSIONS[:1]) is not None)
    size = sum(p.stat().st_size for p in seed.rglob('*') if p.is_file())
    return dict(abi=abi, toolchain=toolchain, stores=[str(s) for s in stores], records=len(merged),
                optimized_pixel_shaders=ns1, specialized_pixel_shaders=ns2,
                skipped_other_toolchains=other_toolchains, dropped_optimized=dropped_optimized,
                shareable=sorted(shareable), bytes=size)


# --- Package-time shader warm-up -------------------------------------------------------------------

def _ns_lines(log):
    try:
        return [l for l in log.read_text(errors='replace').splitlines() if '[native-shaders]' in l]
    except OSError:
        return []


def warm_up_shaders(scales, scratch, timeout=900):
    """One background test session per resolution scale with the pinned converter target, seeded with the
    shareable pipeline lists of the live cache (read only). Warm-up translates every known pipeline and the
    optimizer pool optimizes every pixel shader; each session is closed (by its session id) once the pool has
    drained. Returns the last session's cache folder (it accumulates all scales) and per-scale stats."""
    import fable2_dev_session as dev
    import fable2_instances as instances
    import fable2_test_control as control
    names = [e['name'] for e in dev.library_entries()]
    save = WARM_SAVE if WARM_SAVE in names else (names[0] if names else None)
    if not save: raise RuntimeError('No save in the save library to boot for the shader warm-up; use --shaders or --no-shaders')
    seed = scratch / 'seed-cache'
    (seed / 'shaders/shareable').mkdir(parents=True)
    for name in (f'{TITLE}.xsh', f'{TITLE}.metal.xpso'):
        if (LIVE_CACHE / 'shaders/shareable' / name).is_file():
            shutil.copy2(LIVE_CACHE / 'shaders/shareable' / name, seed / 'shaders/shareable' / name)
    cache, stats = seed, []
    for scale in scales:
        started = time.monotonic()
        record = dev.launch('library', save=save, target_slot='none', test_control=True, cache_from=str(cache),
                            extra_env=['FABLE2_MSC_MIN_TARGET=' + MSC_PIN, 'FABLE2_RES_SCALE=%d' % scale,
                                       'FABLE2_WINDOW_TAG=warm-up %dx' % scale])  # window title
        session = Path(record['session'])
        log = session / 'test-state/logs/fable_2.log'
        done = None
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if not dev.pid_alive(record['pid']): raise RuntimeError('the warm-up game exited early (see %s)' % log)
                lines = _ns_lines(log)
                # Drained: pipeline warm-up finished, the newest native-shaders line is a "background work done"
                # and it stayed the newest for 10 s.
                warmed = 'background warm-up complete' in log.read_text(errors='replace') if log.is_file() else False
                if warmed and lines and 'background work done' in lines[-1]:
                    if done and done[0] == len(lines) and time.monotonic() - done[1] > 10: break
                    if not done or done[0] != len(lines): done = (len(lines), time.monotonic())
                else:
                    done = None
                time.sleep(1)
            else:
                raise RuntimeError('the shader warm-up did not finish in %d s' % timeout)
        finally:
            try:
                control.close(session)
            except Exception:
                pass
        if dev.pid_alive(record['pid']): raise RuntimeError('could not close the warm-up game %d' % record['pid'])
        stats.append(dict(scale=scale, session=session.name, seconds=round(time.monotonic() - started, 1),
                          last=_ns_lines(log)[-1].split('[native-shaders] ', 1)[-1],
                          contention=record.get('contention')))
        cache = session / 'test-state/cache'
    return cache, stats


# --- Remembered seed (docs/offline-shader-seed.md) ----------------------------------------------------

SEED_SOURCES = ('auto', 'agent-cache', 'seed-store', 'prep', 'warm-up')
SEED_STORE_KEEP = 4  # newest keys kept in the seed store


def _seed_log(text):
    print('[seed] ' + text, file=sys.stderr, flush=True)


def build_key():
    """The shader-generation fingerprint the build recorded (metal-build.json), or None (old build)."""
    import fable2_agent_cache as agent_cache
    if os.environ.get('FABLE2_SEED_KEY'): return os.environ['FABLE2_SEED_KEY']  # testing: pretend another key
    rec = agent_cache.app_key(RUNTIME / 'Fable II.app')
    return rec['key'] if rec else None


def seed_store_dir(key):
    import fable2_shader_prep as prep
    return prep.SEED_STORE / key


def store_seed(key, resources, info):
    """Remember this package's seed under its key (MAIN/out/shader-seed-store, ignored), so the next package of the same
    shader code reuses it without a game run. Keeps the newest SEED_STORE_KEEP keys."""
    if not key or not (resources / 'native-shaders').is_dir(): return
    dest = seed_store_dir(key)
    tmp = dest.with_name(dest.name + '.tmp-%d' % os.getpid())
    if tmp.exists(): shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    import fable2_common as common
    common.clone(resources / 'native-shaders', tmp / 'native-shaders')
    (tmp / 'source.json').write_text(json.dumps(dict(info, stored=time.strftime('%Y-%m-%d %H:%M:%S')), indent=2) + '\n')
    if dest.exists(): shutil.rmtree(dest)
    tmp.rename(dest)
    kept = sorted((p for p in dest.parent.iterdir() if p.is_dir() and '.tmp-' not in p.name),
                  key=lambda p: p.stat().st_mtime, reverse=True)
    for old in kept[SEED_STORE_KEEP:]: shutil.rmtree(old, ignore_errors=True)


def resolve_seed(work, scales, source='auto'):
    """Where the shader seed comes from, without a game run when anything matches (maintainer, 2026-10-14):
      1. agent-cache: the agent shader cache's latest generation for this build's shader_cache_key;
      2. seed-store: the seed of the last package with the same key;
      3. prep: an offline headless run (FABLE2_SHADER_PREP, no save, no game loop) of the newest remembered
         shader/pipeline lists, any key, one short process per scale;
      4. warm-up: today's warm-up sessions (a save in the save library, gameplay at each scale).
    Returns dict(kind, detail, key, caches=[cache roots] | seed=<native-shaders dir>, prep|warm stats)."""
    import fable2_agent_cache as agent_cache
    import fable2_shader_prep as prep
    key = build_key()
    if source in ('auto', 'agent-cache') and key and key in agent_cache.keys():
        meta = agent_cache.load_meta(key) or {}
        gen = int(meta.get('generation', 0))
        cache = agent_cache.gen_dir(key, gen)
        if cache.is_dir() and prep.lists_in(cache):
            with agent_cache.cache_lock(exclusive=False, what='portable seed'):
                clone = work / 'agent-cache-seed'
                import fable2_common as common
                common.clone(cache, clone)  # a stable copy: other agents may publish meanwhile
            return dict(kind='agent-cache', detail='agent cache %s@%d' % (key, gen), key=key, caches=[clone])
    if source == 'agent-cache': raise RuntimeError('no agent-cache entry for shader_cache_key %s' % key)
    if source in ('auto', 'seed-store') and key and (seed_store_dir(key) / 'native-shaders').is_dir():
        return dict(kind='seed-store', detail='seed store %s' % key, key=key, seed=seed_store_dir(key) / 'native-shaders')
    if source == 'seed-store': raise RuntimeError('no stored seed for shader_cache_key %s' % key)
    if source in ('auto', 'prep'):
        lists, where = prep.newest_lists()
        if lists is not None:
            base = prep.lists_only_cache(lists, work / 'prep-lists/cache')
            _seed_log('no remembered seed for key %s; offline prep from %s at %s' % (
                key, where, ','.join('%dx' % s for s in scales)))
            # The build being packaged runs the prep (the release build by default).
            result = prep.run(base, work / 'prep-out', scales, log_fn=_seed_log, app=RUNTIME / 'Fable II.app')
            # Session folders are local paths: the package check refuses them in the manifest.
            stats = [{k: v for k, v in st.items() if k not in ('session', 'pid')} for st in result['scales']]
            return dict(kind='prep', detail='offline prep of %s' % where.split(' /')[0], key=key,
                        caches=[work / 'prep-out'], prep=stats)
        if source == 'prep': raise RuntimeError('no remembered shader lists for an offline prep')
    cache, warm = warm_up_shaders(scales, work)
    return dict(kind='warm-up', detail='warm-up sessions', key=key, caches=[cache], warm_up=warm)


# --- Package ----------------------------------------------------------------------------------------

def zip_bundle(source, zip_path):
    """Every user-facing zip (release app, portable, benchmark and flash kits). No extended attributes, resource forks
    or ACLs: --sequesterRsrc stored the build Mac's com.apple.provenance as __MACOSX/._* entries, and macOS 27 called
    the downloaded app "damaged" (docs/release-app.md "Gatekeeper")."""
    run('ditto', '-c', '-k', '--norsrc', '--noextattr', '--noacl', '--keepParent', source, zip_path)
    check_zip(zip_path)


def metadata_entries(zip_path):
    """AppleDouble entries in a zip (__MACOSX/ or ._name): the ones zip_bundle must never write."""
    with zipfile.ZipFile(str(zip_path)) as archive:
        return [name for name in archive.namelist()
                if name.startswith('__MACOSX/') or any(part.startswith('._') for part in name.split('/'))]


def check_zip(zip_path):
    found = metadata_entries(zip_path)
    if found:
        raise RuntimeError('%s has %d __MACOSX/._ entries (e.g. %s)' % (Path(zip_path).name, len(found), found[0]))


# Name-guard labels ignored in third-party source trees (check_package third_party): author credits and examples in
# upstream headers (simde, SDL, spdlog, ...). The surname check and the build-machine path check still apply.
THIRD_PARTY_IGNORED = ('first name', 'personal email', 'home path')


def check_package(app, converter=False, third_party=()):
    """Refuses disc content, local paths, personal identifiers (tools/fable2_name_guard.py: maintainer name, personal
    email, /Users/<name> paths; release-cleanup D5), unbundled dependencies and, unless converter=True
    (--with-converter), any shader-converter library or its notices (the release ships without dxilconv and Metal
    Shader Converter). Every file is checked, binaries included (__FILE__ strings, debug info, rpaths).
    third_party: app-relative path prefixes of upstream source trees (the release kit's SDK headers), where
    THIRD_PARTY_IGNORED identifier hits are other people's credits, not ours."""
    import fable2_name_guard
    allow = fable2_name_guard.load_allowlist()
    local_paths = [str(ROOT).encode(), str(Path.home()).encode() + b'/']
    problems = []
    for path in app.rglob('*'):
        relative = path.relative_to(app).as_posix()
        if not converter and (path.name in CONVERTER_LIBRARIES or path.name.startswith('DirectXShaderCompiler')):
            problems.append('shader converter in a no-converter package: ' + relative)
        if GAME_CONTENT.search(relative) and 'native-shaders' not in relative:
            problems.append('game content in package: ' + relative)
        # The converter libraries (--with-converter dev packages only) are third-party builds; not checked.
        if path.is_file() and path.name not in CONVERTER_LIBRARIES:
            data = path.read_bytes()
            if any(local in data for local in local_paths):
                problems.append('absolute local path in ' + relative)
            found = fable2_name_guard.find(data, relative, allow)
            if any(relative.startswith(prefix) for prefix in third_party):
                found = [hit for hit in found if hit[0] not in THIRD_PARTY_IGNORED]
            hits = sorted({'%s %r' % hit for hit in found})
            if hits:
                problems.append('personal identifier in %s: %s' % (relative, ', '.join(hits[:5])))
    for binary in (app / 'Contents/MacOS').iterdir():
        listing = subprocess.check_output(['otool', '-L', str(binary)], text=True).splitlines()[1:]
        for line in listing:
            dependency = line.strip().split(' (compatibility')[0]
            if dependency.startswith(('/usr/lib/', '/System/Library/')): continue
            if not dependency.startswith('@rpath/') or not (app / 'Contents/MacOS' / Path(dependency).name).is_file():
                problems.append(f'{binary.name}: unbundled dependency {dependency}')
            if not converter and Path(dependency).name in CONVERTER_LIBRARIES:
                problems.append(f'{binary.name}: links the shader converter {dependency}')
    notices = app / 'Contents/Resources/Licenses/THIRD_PARTY_NOTICES.md'
    if not converter and notices.is_file() and re.search(r'libdxilconv|libmetalirconverter', notices.read_text()):
        problems.append('Licenses/THIRD_PARTY_NOTICES.md lists a shader converter library')
    if problems: raise RuntimeError('Package check failed:\n  ' + '\n  '.join(problems))


def ensure_release_build():
    """Refresh out/build-release (tools/build_macos_metal.py --release, incremental: make is the staleness check) in a
    login shell, as fable2_test_control.build does (PATH with cmake etc. also from the Dev Launcher). Output goes to
    RELEASE_LOG so stdout stays one JSON object."""
    RELEASE_LOG.parent.mkdir(parents=True, exist_ok=True)
    jobs = os.environ.get('FABLE2_BUILD_JOBS') or '12'
    print('[portable] refreshing the no-converter release build (log %s)' % RELEASE_LOG.relative_to(ROOT),
          file=sys.stderr, flush=True)
    started = time.monotonic()
    with open(RELEASE_LOG, 'w') as log:
        status = subprocess.run(['/bin/zsh', '-lc', 'export FABLE2_BUILD_JOBS=%s; python3 tools/build_macos_metal.py '
                                 '--release' % jobs], cwd=ROOT, stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT).returncode
    if status:
        tail = RELEASE_LOG.read_text(errors='replace')[-3000:]
        raise RuntimeError('release build failed (exit %d, %s):\n%s' % (status, RELEASE_LOG, tail))
    record = json.loads((RELEASE_RUNTIME / 'metal-build.json').read_text())
    if record.get('shader_converter', True):
        raise RuntimeError('%s is not a no-converter build' % RELEASE_RUNTIME)
    print('[portable] release build ready (%.0f s)' % (time.monotonic() - started), file=sys.stderr, flush=True)


def build(out_dir, zip_it=True, skip_intro=True, shader_sources=None, warm_scales=None, seed_source='auto',
          converter=False, rebuild=True):
    """converter=False (default): package the no-converter release build, refreshed first unless rebuild=False.
    converter=True: package the dev build out/build as it is (converter dylibs included)."""
    global RUNTIME
    started = time.monotonic()
    RUNTIME = DEV_RUNTIME if converter else RELEASE_RUNTIME
    if not converter and rebuild: ensure_release_build()
    for name in ('fable_2', 'librexgpu-metal.dylib', 'librexruntime.dylib'):
        if not (RUNTIME / name).is_file():
            raise RuntimeError(f'No build in {RUNTIME.relative_to(ROOT)}: run tools/build_macos_metal.py'
                               + ('' if converter else ' --release') + ' first')
    if not converter:
        linked = subprocess.check_output(['otool', '-L', str(RUNTIME / 'librexgpu-metal.dylib')], text=True)
        if any(name in linked for name in CONVERTER_LIBRARIES):
            raise RuntimeError('%s links the shader converter; rebuild it with tools/build_macos_metal.py --release'
                               % RUNTIME.relative_to(ROOT))
    out_dir = out_dir.absolute()
    out_dir.mkdir(parents=True, exist_ok=True)
    final = out_dir / PACKAGE_NAME
    zip_path = out_dir / (PACKAGE_NAME + '.zip')
    with tempfile.TemporaryDirectory(prefix='.portable-', dir=out_dir) as temporary:
        work = Path(temporary)
        folder = work / PACKAGE_NAME
        folder.mkdir()
        # package_macos_app's tools are chatty (install_name_tool/codesign warnings): keep them in a log.
        log = open(work / 'package.log', 'w')
        saved = os.dup(1), os.dup(2)
        os.dup2(log.fileno(), 1); os.dup2(log.fileno(), 2)
        try:
            app = package_macos_app.package(RUNTIME, folder / 'Fable II.app', None, 'metal', strip_debug=True)
        except Exception:
            os.dup2(saved[0], 1); os.dup2(saved[1], 2)
            sys.stderr.write((work / 'package.log').read_text()[-4000:])
            raise
        finally:
            sys.stdout.flush(); os.dup2(saved[0], 1); os.dup2(saved[1], 2); log.close()
        macos, resources = app / 'Contents/MacOS', app / 'Contents/Resources'
        import fable2_common as common
        with common.build_marker(1, 'portable helpers'):  # builds don't overlap perf runs (compiles only)
            run('xcrun', 'swiftc', '-O', '-swift-version', '5', '-parse-as-library', '-target', 'arm64-apple-macos15.0',
                '-o', macos / 'FableIIPrepare', *[ROOT / s for s in PREPARE_SOURCES])
            run('xcrun', 'swiftc', '-O', '-swift-version', '5', '-target', 'arm64-apple-macos15.0',
                '-o', macos / PLAYER_EXECUTABLE, *[ROOT / s for s in PLAYER_SOURCES])
        (resources / 'recomp-lua').mkdir()
        for lua in sorted((ROOT / 'src/lua').glob('*.lua')): shutil.copy2(lua, resources / 'recomp-lua' / lua.name)
        shaders, resolved = None, None
        if warm_scales:
            resolved = resolve_seed(work, warm_scales, seed_source)
            _seed_log('source: %s (%s)' % (resolved['kind'], resolved['detail']))
            if resolved.get('seed'):
                shutil.copytree(resolved['seed'], resources / 'native-shaders')
                stored = json.loads((resources / 'native-shaders/manifest.json').read_text())
                shaders = dict(stored, stores=[])
            else:
                shader_sources = [str(c) for c in resolved['caches']]
        if shader_sources and not shaders:
            toolchain = toolchain_hash(work)
            shaders = build_shader_seed([Path(s).expanduser() for s in shader_sources], resources, shader_abi_tag(), toolchain,
                                        drop_optimized=bool(resolved and resolved['kind'] == 'agent-cache'))
            if shaders and resolved:
                shaders['warm_up'] = resolved.get('warm_up')
                shaders['prep'] = resolved.get('prep')
        if shaders:
            shaders['source'] = dict(kind=resolved['kind'], detail=resolved['detail'], key=resolved['key']) if resolved \
                else dict(kind='shaders-arg', detail=', '.join(map(str, shader_sources)), key=build_key())
        version = fable2_version.info()
        plist_path = app / 'Contents/Info.plist'
        info = plistlib.loads(plist_path.read_bytes())
        info.update(CFBundleExecutable=PLAYER_EXECUTABLE, CFBundleIdentifier=BUNDLE_ID, Fable2Portable=True, Fable2XexSHA256=expected_xex_sha256(),
                    Fable2MSCMinTarget=MSC_PIN,
                    Fable2SkipIntroDefault=bool(skip_intro), Fable2PortableBuilt=time.strftime('%Y-%m-%d %H:%M'))
        if shaders:
            info['Fable2ShaderSeedID'] = '%s-%s-%s' % (version['git_sha'] or 'source', shaders['abi'], shaders['records'])
            if shaders['toolchain']: info['Fable2NSToolchainHash'] = shaders['toolchain']
            (resources / 'native-shaders/manifest.json').write_text(json.dumps(
                {k: v for k, v in shaders.items() if k != 'stores'}, indent=2) + '\n')
            if resolved and resolved['kind'] in ('agent-cache', 'prep', 'warm-up'):
                store_seed(resolved['key'], resources, shaders['source'])
        plist_path.write_bytes(plistlib.dumps(info))
        for helper in ('FableIIPrepare', PLAYER_EXECUTABLE, 'FableIILauncher'):
            run('codesign', '--force', '--sign', '-', macos / helper, stderr=subprocess.DEVNULL)
        run('codesign', '--force', '--sign', '-', app, stderr=subprocess.DEVNULL)
        run('codesign', '--verify', '--deep', '--strict', app)
        check_package(app, converter)
        (folder / 'README.txt').write_text(README.format(
            version=info.get('Fable2VersionString', version['string']), min_os=info.get('LSMinimumSystemVersion', '15.0'),
            shader_note=SHADER_NOTE if shaders else ''))
        if final.exists(): shutil.rmtree(final)
        folder.rename(final)
    if zip_it:
        if zip_path.exists(): zip_path.unlink()
        zip_bundle(final, zip_path)
    app = final / 'Fable II.app'
    size = sum(p.stat().st_size for p in app.rglob('*') if p.is_file() and not p.is_symlink())
    return dict(folder=str(final), app=str(app), zip=str(zip_path) if zip_it else None,
                app_bytes=size, zip_bytes=zip_path.stat().st_size if zip_it else None,
                version=plistlib.loads((app / 'Contents/Info.plist').read_bytes()).get('Fable2VersionString'),
                xex_sha256=expected_xex_sha256(), skip_intro_default=skip_intro, shaders=shaders,
                shader_converter=bool(converter), runtime=str(RUNTIME.relative_to(ROOT)),
                seconds=round(time.monotonic() - started, 1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='package the no-converter release build as a portable app (+ zip)')
    b.add_argument('--out', type=Path, default=DEFAULT_OUT, help='output folder (default out/portable)')
    b.add_argument('--no-zip', action='store_true')
    b.add_argument('--no-skip-intro', action='store_true', help='play the logo movies by default')
    b.add_argument('--shader-scales', default='1,2,3', metavar='LIST',
                   help='resolution scales to warm the shader seed for (default 1,2,3; one test session each)')
    b.add_argument('--shaders', action='append', metavar='CACHE_DIR',
                   help='take the shader seed from these cache folders (read only) instead of warming one up; '
                        'only useful when they were made with FABLE2_MSC_MIN_TARGET=' + MSC_PIN)
    b.add_argument('--no-shaders', action='store_true', help='ship no optimized shaders (no warm-up session)')
    b.add_argument('--seed-source', default='auto', choices=SEED_SOURCES,
                   help='where the seed comes from (default auto: agent cache of this build\'s shader key, then the '
                        'seed store, then an offline prep run, then warm-up sessions)')
    c = b.add_mutually_exclusive_group()
    c.add_argument('--with-converter', action='store_true',
                   help='package the dev build (out/build) as it is, with dxilconv and Metal Shader Converter')
    c.add_argument('--no-rebuild', action='store_true',
                   help='package out/build-release as it is (no incremental release build first)')
    args = ap.parse_args()
    shader_sources = None if args.no_shaders else args.shaders
    warm_scales = None if (args.no_shaders or args.shaders) else [int(x) for x in args.shader_scales.split(',') if x]
    try:
        result = build(args.out, not args.no_zip, not args.no_skip_intro, shader_sources, warm_scales, args.seed_source,
                       converter=args.with_converter, rebuild=not args.no_rebuild)
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps({'ok': True, 'result': result}, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
