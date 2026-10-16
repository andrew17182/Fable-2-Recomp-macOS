#!/usr/bin/env python3
"""Build the ReXGlue SDK package (runtime, rexglue tool, SDL3, headers) from pinned source.

Produces out/sdk-src/mac-arm64 with the same layout as the prebuilt v0.10.0 release
(bin/, lib/, lib/cmake/, include/, share/), so the game consumes it unchanged through
REXGLUE_SDK_ROOT / CMAKE_PREFIX_PATH. Inputs, all pinned and checksummed:
  - backends/metal/dependencies.json  "sdk-source" (tag v0.10.0 = f5337cdc)
  - backends/metal/sdk-submodules.json (the tag's submodule gitlinks)
  - backends/metal/sdk-build.patch     (build-only: version pin, archive submodule marker,
                                        optional macOS Vulkan loader/MoltenVK)
  - backends/metal/sdk-metal.patch     (the hunks of the Metal plugin's graphics patch that
                                        reach the runtime; METAL_PATCH_RUNTIME_FILES)
  - SDK_BEHAVIOUR_PATCHES below        (patches/sdk/*.patch: phase B runtime fixes)
The tree is rebuilt only when an input changes (fingerprint in the install's
.fable-sdk-build.json). See docs/sdk-from-source.md.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
METAL = ROOT / 'backends/metal'
WORK = ROOT / 'out/sdk-src'
SOURCE = WORK / 'source'
BUILD = WORK / 'build'
PREFIX = WORK / 'mac-arm64'
STAMP_NAME = '.fable-sdk-build.json'
SUBMODULE_CACHE = ROOT / 'out/downloads/sdk-src'
VERSION = '0.10.0'
# Runtime behaviour patches from patches/sdk/, applied after the two patches above, in order.
# Each one retires an executable-side workaround in the same change, or the workaround detects
# the fixed runtime and stands down (docs/sdk-from-source.md, phase B). FABLE2_SDK_FROM_SOURCE=0
# (the prebuilt runtime) is the fallback; the workarounds stay active there.
SDK_BEHAVIOUR_PATCHES = [
    'patches/sdk/clock-duration-scaling.patch',  # B2: turbo waits divide by the guest speed
    'patches/sdk/window-initial-position.patch',  # B5: place the window before it is shown
    'patches/sdk/clock-fractional-ticks.patch',  # B6: guest clock keeps fractional ticks
    # B1: ring re-init synchronised with the GPU worker. The same hunks are in sdk-metal.patch for
    # the Metal plugin; here they reach the runtime tree's rexgpu-xenos (Vulkan plugin).
    'patches/sdk/ring-reinit-sync.patch',
    'patches/sdk/wait-multiple-blocking.patch',  # B4 (CR-09): multi-object waits block, no polling
    'patches/sdk/codegen-memory-barriers.patch',  # B3: codegen emits sync/lwsync fences (opt-in)
    'patches/sdk/fpscr-rounding-per-arch.patch',  # mffs rounding mode decoded in the ARM FPCR order
    'patches/sdk/timer-queue-blocking.patch',  # TimerQueue blocks in kevent, no yield spin
    'patches/sdk/thread-priority-map.patch',  # guest priorities mapped, not raw SCHED_FIFO
    'patches/sdk/codegen-fctiw-rounding.patch',  # fctiw/fctid round per FPSCR[RN] (REX_FCTIW/REX_FCTID)
    'patches/sdk/vfs-skip-dev-dirs.patch',  # startup VFS scan skips the dev checkout's out/.git/thirdparty
    'patches/sdk/shm-unlink-on-create.patch',  # guest memory shm name removed at create: no leak on hard exit
    'patches/sdk/bit-scan-inline.patch',  # rex::bit_scan_forward inline (no cross-dylib call per bit)
    'patches/sdk/thread-exit-signal-safe.patch',  # no TLV in the thread signal handler; signals blocked at exit; quit outside SDL's watch lock
    'patches/sdk/codegen-graph-index.patch',  # codegen: indexed pending-jump and GAP_FILL lookups (same output, 4.5x faster)
    'patches/sdk/audio-playback-rate.patch',  # turbo audio: output consumes guest audio at the turbo rate
]
# sdk-metal.patch adapts the graphics sources for the Metal plugin, which compiles them itself
# with backends/metal/compat.h and its port/ headers; the Vulkan plugin (rexgpu-xenos) cannot
# build from the fully patched tree. Only these files of it are compiled into librexruntime
# (src/core/CMakeLists.txt: the Xenos format layer) or included by the executable, so the
# runtime build takes exactly these hunks. Both are additive inline helpers.
METAL_PATCH_RUNTIME_FILES = ['include/rex/graphics/xenos.h', 'include/rex/graphics/pipeline/texture/info.h']
CPU_FLAGS = '-march=armv8-a' if os.getenv('FABLE2_CPU_TARGET') == 'generic' else '-mcpu=apple-m1'
# No build-machine paths in librexruntime (FFmpeg/SDK asserts embed __FILE__; debug info): the checkout becomes "./...",
# the rest of the home folder "~/...". The checkout map goes last: __FILE__ takes the longest matching prefix, debug
# info the last matching option. Release decision D5 (2026-10-14).
PATH_MAP_FLAGS = ' -ffile-prefix-map=%s=~ -ffile-prefix-map=%s=.' % (Path.home(), ROOT)
CONFIGURE = [
    '-G', 'Unix Makefiles',
    '-DCMAKE_BUILD_TYPE=Release',
    # Match the release CI (cmake --preset mac-arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3).
    '-DCMAKE_C_COMPILER=/usr/bin/clang', '-DCMAKE_CXX_COMPILER=/usr/bin/clang++',
    # CPU baseline: -mcpu=apple-m1 (the A14/M1 feature set every Apple Silicon Mac
    # has); FABLE2_CPU_TARGET=generic restores the old -march=armv8-a.
    '-DCMAKE_C_FLAGS=' + CPU_FLAGS + PATH_MAP_FLAGS, '-DCMAKE_CXX_FLAGS=' + CPU_FLAGS + PATH_MAP_FLAGS,
    '-DCMAKE_OBJC_FLAGS=' + PATH_MAP_FLAGS.strip(), '-DCMAKE_OBJCXX_FLAGS=' + PATH_MAP_FLAGS.strip(),
    '-DCMAKE_CXX_STANDARD=23', '-DCMAKE_OSX_ARCHITECTURES=arm64',
    '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3',
    '-DREXGLUE_VERSION_OVERRIDE=' + VERSION,
    # SDL takes its revision string from git; match the release's (the archive has no .git).
    '-DSDL_REVISION=SDL-3.5.0-release-3.4.0-428-g8bf3b7215',
    # The Metal app ships neither; the Vulkan build keeps the prebuilt package.
    '-DREXGLUE_MACOS_VULKAN_RUNTIME=OFF',
]


def log(text):
    print('[sdk-src] ' + text, flush=True)


def run(args, **kw):
    subprocess.run([str(x) for x in args], check=True, **kw)


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch(pin, cache):
    archive = cache / (pin['name'] + '-' + pin['commit'] + '.tar.gz')
    if not archive.exists():
        cache.mkdir(parents=True, exist_ok=True)
        partial = archive.with_suffix('.part')
        run(['curl', '--fail', '--location', '--silent', '--show-error', pin['url'], '-o', partial])
        if sha(partial) != pin['sha256']:
            partial.unlink()
            raise ValueError('Download hash mismatch: ' + pin['name'])
        partial.replace(archive)
    if sha(archive) != pin['sha256']:
        raise ValueError('Cache hash mismatch: ' + str(archive))
    return archive


def unpack(archive, dest):
    """Extract an archive with one top-level directory into dest (which must not exist)."""
    with tarfile.open(archive) as tf:
        for m in tf.getmembers():
            parts = Path(m.name).parts
            bad = m.name.startswith('/') or '..' in parts
            if m.issym() or m.islnk():
                target = os.path.normpath(os.path.join(os.path.dirname(m.name), m.linkname))
                bad = bad or m.linkname.startswith('/') or target == '..' or target.startswith('../')
            if bad:
                raise ValueError('Unsafe archive path: ' + m.name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=dest.parent) as tmp:
            try:
                tf.extractall(tmp, filter='data')
            except TypeError:  # Python without extraction filters (system 3.9); members checked above
                tf.extractall(tmp)
            entries = list(Path(tmp).iterdir())
            if len(entries) != 1 or not entries[0].is_dir():
                raise ValueError('Expected one archive root: ' + str(archive))
            if dest.exists():
                dest.rmdir()  # empty submodule placeholder from the parent archive
            shutil.move(str(entries[0]), str(dest))


def inputs():
    pins = json.loads((METAL / 'dependencies.json').read_text())
    sdk = next(p for p in pins if p['name'] == 'sdk-source')
    subs = json.loads((METAL / 'sdk-submodules.json').read_text())
    patches = [METAL / 'sdk-build.patch', METAL / 'sdk-metal.patch'] + [ROOT / p for p in SDK_BEHAVIOUR_PATCHES]
    fingerprint = {
        'sdk_source': sdk['commit'],
        'submodules': {s['path']: s['commit'] for s in subs},
        'patches': {p.relative_to(ROOT).as_posix(): sha(p) for p in patches},
        'configure': CONFIGURE,
        'metal_patch_runtime_files': METAL_PATCH_RUNTIME_FILES,
        'script': sha(Path(__file__)),
    }
    return sdk, subs, patches, fingerprint


def prepare_source(sdk, subs, patches, cache):
    if SOURCE.exists():
        shutil.rmtree(SOURCE)
    log('extracting sdk-source ' + sdk['commit'][:10] + ' and %d submodules' % len(subs))
    unpack(fetch(sdk, cache), SOURCE)
    for sub in subs:
        dest = SOURCE / sub['path']
        unpack(fetch(sub, SUBMODULE_CACHE), dest)
        (dest / '.rexglue-pinned').write_text(sub['commit'] + '\n')
    for patch in patches:
        log('applying ' + patch.relative_to(ROOT).as_posix())
        if patch.name == 'sdk-metal.patch':
            patch = filter_patch(patch, METAL_PATCH_RUNTIME_FILES, WORK / 'sdk-metal.runtime.patch')
        run(['patch', '--batch', '--silent', '-p1', '-i', patch], cwd=SOURCE)


def filter_patch(patch, files, out):
    """Write the per-file sections of a unified diff that touch `files` to `out`."""
    keep, found, current = [], set(), None
    for line in patch.read_text().splitlines(keepends=True):
        if line.startswith('--- '):
            current = None
            pending = line
            continue
        if line.startswith('+++ '):
            name = line[4:].split('\t')[0].strip()
            name = name[2:] if name.startswith('b/') else name
            current = name if name in files else None
            if current:
                found.add(current)
                keep.extend([pending, line])
            continue
        if current:
            keep.append(line)
    missing = set(files) - found
    if missing:
        raise ValueError('sdk-metal.patch no longer touches: ' + ', '.join(sorted(missing)))
    out.write_text(''.join(keep))
    return out


def up_to_date(fingerprint):
    stamp = PREFIX / STAMP_NAME
    try:
        old = json.loads(stamp.read_text())
    except (OSError, ValueError):
        return False
    return old.get('inputs') == fingerprint and (PREFIX / 'bin/rexglue').is_file()


def build(fingerprint, cache, jobs):
    sdk, subs, patches, _ = inputs()
    start = time.time()
    prepare_source(sdk, subs, patches, cache)
    for path in (BUILD, PREFIX):
        if path.exists():
            shutil.rmtree(path)
    home = WORK / 'home'  # SDK install registers itself in ~/.cmake/packages; keep that inside out/
    home.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, HOME=str(home))
    log('configuring')
    run(['cmake', '-S', SOURCE, '-B', BUILD, '-DCMAKE_INSTALL_PREFIX=' + str(PREFIX)] + CONFIGURE,
        env=env, stdout=subprocess.DEVNULL)
    log('building runtime, rexglue, SDL3 (%d jobs)' % jobs)
    run(['cmake', '--build', BUILD, '--target', 'install', '--parallel', str(jobs)], env=env,
        stdout=subprocess.DEVNULL)
    if not (PREFIX / 'bin/rexglue').is_file() or not (PREFIX / 'lib/librexruntime.dylib').is_file():
        raise RuntimeError('SDK install incomplete: ' + str(PREFIX))
    seconds = round(time.time() - start)
    (PREFIX / STAMP_NAME).write_text(json.dumps({
        'inputs': fingerprint,
        'build_seconds': seconds,
        'runtime_sha256': {n: sha(PREFIX / n) for n in ['bin/rexglue', 'lib/librexruntime.dylib',
                                                         'lib/librexgpu-xenos.dylib', 'lib/libSDL3.a']},
    }, indent=2) + '\n')
    log('built %s in %d s' % (PREFIX, seconds))


def ensure(cache=None, jobs=None, force=False):
    """Build out/sdk-src/mac-arm64 if any input changed; return the install prefix."""
    cache = Path(cache or ROOT / 'out/downloads/metal').resolve()
    jobs = jobs or int(os.getenv('FABLE2_BUILD_JOBS', '4'))
    fingerprint = inputs()[3]
    if not force and up_to_date(fingerprint):
        log('up to date: ' + str(PREFIX))
    else:
        build(fingerprint, cache, jobs)
    return PREFIX


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cache', type=Path, default=ROOT / 'out/downloads/metal')
    ap.add_argument('--jobs', type=int, default=int(os.getenv('FABLE2_BUILD_JOBS', '4')))
    ap.add_argument('--force', action='store_true', help='rebuild even when the inputs are unchanged')
    args = ap.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        ap.error('Apple Silicon macOS is required')
    if args.jobs < 1:
        ap.error('--jobs must be positive')
    print(ensure(args.cache, args.jobs, args.force))


if __name__ == '__main__':
    main()
