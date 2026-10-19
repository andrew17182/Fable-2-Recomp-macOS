#!/usr/bin/env python3
"""Builds the game on the player's Mac from the release app's kit and the player's own disc (docs/release-app.md).

The release "Fable II.app" ships no game code. Its kit (Contents/Resources/kit, made by tools/fable2_release.py) holds
the prebuilt ReXGlue SDK prefix (headers, static libraries, the rexglue recompiler), our sources and build files, and
the runtime libraries. This script, run by the app under /usr/bin/python3 once Apple's Command Line Tools are
installed, turns that plus the player's extracted default.xex into a runnable game:

  1. cmake      find CMake 3.25 or later, else download Kitware's official build (version pinned, SHA-256 checked)
  2. sync       copy the kit into <state>/Build (only files whose content changed, so make stays incremental)
  3. codegen    rexglue init (first time), configure, rexglue codegen, configure again
  4. compile    the recomp patches, then everything with a job count sized by RAM (fable_2_register.cpp, the
                -O0 TU, starts in the first wave); memory pressure or swap growth stops make and resumes with
                fewer jobs. Generated code is compiled without debug info (function names stay in the symbol
                table); --debug-info line-tables-only and --register-first restore the earlier build
  5. link       (part of make)
  6. assemble   <state>/Game/Fable II.bundle: the built fable_2 with the runtime libraries, ad-hoc signed here
  7. done       <state>/Build/build-state.json (what the app checks before it plays)

Nothing is written outside <state> (default ~/Library/Application Support/<the app's folder>), nothing game-derived
ever leaves it. Output is line based for the app's progress window; every command is echoed as "$ <command>" so the
"Show details" log shows exactly what runs:
  [stage] <key> <text>         a step starts (keys above)
  [progress] <0..1> <text>     overall progress
  [eta] <seconds>              estimated time left (compile only)
  [memory] <text>              a job-count change from the memory watch
  [error] <text>               plain-English failure (then exit 1); exit 3 = cancelled (SIGTERM / SIGINT)
  [denied] <errno> <path>      reading <path> was refused (macOS privacy or permissions; before the [error] line)

  /usr/bin/python3 -I -B -u fable2_game_build.py build --kit KIT --state STATE --game-data GAMEDATA
        [--jobs N] [--measure] [--ignore-system-cmake] [--cmake-url URL] [--clean]
  /usr/bin/python3 fable2_game_build.py plan [--kit KIT]          job count and why (JSON)
  /usr/bin/python3 fable2_game_build.py measure-report STATE      per-TU time and peak RSS of a --measure build (JSON)
  /usr/bin/python3 fable2_game_build.py status --kit KIT --state STATE   is the game built for this kit (JSON)

Runs under Python 3.9 with the standard library only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

CMAKE_MIN = (3, 25)
# Kitware's official macOS build (same pin as the setup app, tools/macos/Setup/Support.swift).
CMAKE_PIN = dict(version='4.1.2',
                 url='https://github.com/Kitware/CMake/releases/download/v4.1.2/cmake-4.1.2-macos-universal.tar.gz',
                 sha256='3be85f5b999e327b1ac7d804cbc9acd767059e9f603c42ec2765f6ab68fbd367')
PRESET = 'mac-arm64-release'
# Debug info for the generated code (REXGLUE_RECOMP_DEBUG_INFO): none saves ~7% of the compile CPU; function names
# stay in the symbol table for crash reports, only file:line of generated code is lost (docs/release-app.md).
RECOMP_DEBUG_INFO = 'none'
REGISTER_OBJECT = 'generated/default/fable_2_register.cpp.o'
# Not .app: a .bundle is not listed as an application by Spotlight or Launchpad (same bundle id and Info.plist,
# so the running game is still Fable II in the Dock).
GAME_APP = 'Fable II.bundle'
STATE_FILE = 'build-state.json'
# Overall progress weights per step (sum 1).
WEIGHTS = [('cmake', 0.02), ('sync', 0.02), ('codegen', 0.10), ('compile', 0.82), ('link', 0.02), ('assemble', 0.02)]

CANCELLED = threading.Event()
CHILD = {'proc': None}


class BuildError(Exception):
    """A failure with a plain-English message for the window."""


def say(line):
    sys.stdout.write(line + '\n')
    sys.stdout.flush()


def stage(key, text):
    say('[stage] %s %s' % (key, text))


def progress(step, fraction, text):
    base = 0.0
    for key, weight in WEIGHTS:
        if key == step:
            say('[progress] %.4f %s' % (base + weight * max(0.0, min(fraction, 1.0)), text))
            return
        base += weight


def quote(arg):
    arg = str(arg)
    return arg if re.match(r'^[A-Za-z0-9_./=:,+@%-]+$', arg) else "'" + arg.replace("'", "'\\''") + "'"


def on_signal(signum, _frame):
    CANCELLED.set()
    proc = CHILD['proc']
    if proc and proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except OSError:
            pass


def run(args, cwd=None, env=None, on_line=None, quiet=False):
    """Runs a command in its own process group, echoing it and streaming its output (stdout + stderr) line by line.
    Returns the exit status (-signal when killed)."""
    if CANCELLED.is_set(): raise KeyboardInterrupt
    say('$ ' + ' '.join(quote(a) for a in args) + (' (in %s)' % cwd if cwd else ''))
    proc = subprocess.Popen([str(a) for a in args], cwd=cwd and str(cwd), env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    CHILD['proc'] = proc
    try:
        for raw in proc.stdout:
            line = raw.decode('utf-8', 'replace').rstrip('\n')
            if not quiet: say(line)
            if on_line: on_line(line)
        return proc.wait()
    finally:
        CHILD['proc'] = None
        if CANCELLED.is_set(): raise KeyboardInterrupt


def sysctl(name):
    out = subprocess.run(['/usr/sbin/sysctl', '-n', name], capture_output=True, text=True).stdout.strip()
    return out


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


# --- Plan (job count) ---------------------------------------------------------------------------------

def machine():
    ram = int(sysctl('hw.memsize') or 0) / float(1 << 30)
    cores = int(sysctl('hw.ncpu') or 1)
    perf = int(sysctl('hw.perflevel0.physicalcpu') or cores)
    return dict(ram_gb=round(ram, 1), cores=cores, perf_cores=perf, model=sysctl('hw.model'),
                chip=sysctl('machdep.cpu.brand_string'))


# Peak memory per compile job (measured, docs/release-app.md "Compile memory"): GB per parallel job and the base
# the system keeps. jobs = clamp(min(cores, floor((RAM - BASE) / PER_JOB)), 1, cores).
PER_JOB_GB = 0.75
BASE_GB = 3.5


def plan_jobs(m=None, override=None):
    m = m or machine()
    if override: return dict(jobs=max(1, int(override)), why='set by --jobs / FABLE2_BUILD_JOBS', **m)
    by_ram = int((m['ram_gb'] - BASE_GB) // PER_JOB_GB)
    jobs = max(1, min(m['cores'], by_ram))
    why = '%d cores, %.0f GB RAM: up to %d jobs by memory (%.2f GB per job + %.1f GB for macOS)' % (
        m['cores'], m['ram_gb'], max(by_ram, 1), PER_JOB_GB, BASE_GB)
    return dict(jobs=jobs, why=why, **m)


# --- Memory watch ---------------------------------------------------------------------------------------

def swap_used_mb():
    match = re.search(r'used = ([0-9.]+)([MG])', sysctl('vm.swapusage'))
    if not match: return 0.0
    return float(match.group(1)) * (1024 if match.group(2) == 'G' else 1)


class MemoryWatch(threading.Thread):
    """Polls the kernel's memory-pressure level and swap use once a second. trip() is set when the pressure is
    critical or swap grew by more than SWAP_GROWTH_MB since the watch started: make is then stopped and resumed with
    fewer jobs. Peaks are recorded for build-state.json."""
    SWAP_GROWTH_MB = 1024

    def __init__(self):
        super().__init__(daemon=True)
        self.stop = threading.Event()
        self.tripped = threading.Event()
        self.reason = ''
        self.swap_start = swap_used_mb()
        self.peak_swap_growth = 0.0
        self.peak_pressure = 1

    def run(self):
        while not self.stop.wait(1.0):
            try:
                level = int(sysctl('kern.memorystatus_vm_pressure_level') or 1)
            except ValueError:
                level = 1
            growth = swap_used_mb() - self.swap_start
            self.peak_pressure = max(self.peak_pressure, level)
            self.peak_swap_growth = max(self.peak_swap_growth, growth)
            if level >= 4:
                self.reason = 'memory pressure is critical'
            elif growth > self.SWAP_GROWTH_MB:
                self.reason = 'swap grew by %.0f MB' % growth
            else:
                continue
            self.tripped.set()
            proc = CHILD['proc']
            if proc and proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except OSError:
                    pass

    def rearm(self):
        self.tripped.clear()
        self.reason = ''
        self.swap_start = swap_used_mb()


# --- CMake ----------------------------------------------------------------------------------------------

def cmake_version(path):
    try:
        out = subprocess.run([str(path), '--version'], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    match = re.search(r'cmake version (\d+)\.(\d+)', out)
    return (int(match.group(1)), int(match.group(2))) if match else None


def find_cmake(build, ignore_system):
    """CMake 3.25+: our own download first, then (unless ignore_system) PATH, Homebrew and /Applications/CMake.app."""
    candidates = [build / 'tools/cmake/CMake.app/Contents/bin/cmake']
    if not ignore_system:
        if os.environ.get('FABLE2_CMAKE'): candidates.insert(0, Path(os.environ['FABLE2_CMAKE']))
        search = os.environ.get('PATH', '') + ':/opt/homebrew/bin:/usr/local/bin:/Applications/CMake.app/Contents/bin'
        found = shutil.which('cmake', path=search)
        if found: candidates.append(Path(found))
    for path in candidates:
        if path.is_file():
            version = cmake_version(path)
            if version and version >= CMAKE_MIN:
                return path, version
            say('CMake at %s is %s; 3.25 or later is needed' % (path, '.'.join(map(str, version or ())) or 'unusable'))
    return None, None


def download_cmake(build, url, sha256):
    """Kitware's official macOS CMake into <Build>/tools/cmake (no admin rights, no Homebrew)."""
    tools = build / 'tools'
    tools.mkdir(parents=True, exist_ok=True)
    archive = tools / ('cmake-%s.tar.gz' % CMAKE_PIN['version'])
    partial = tools / (archive.name + '.part')
    if not (archive.is_file() and sha256_file(archive) == sha256):
        say('Downloading CMake %s (Kitware\'s official build) from %s' % (CMAKE_PIN['version'], url))
        last = [0.0]

        def on_curl(text):
            for match in re.finditer(r'(\d+(?:\.\d+)?)%', text):
                value = float(match.group(1)) / 100
                if value - last[0] >= 0.02 or value >= 1:
                    last[0] = value
                    progress('cmake', value * 0.9, 'Downloading CMake… %d%%' % int(value * 100))
        args = ['/usr/bin/curl', '--fail', '--location', '--show-error', '--progress-bar', '--retry', '2',
                '--output', str(partial), url]
        say('$ ' + ' '.join(quote(a) for a in args))
        proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, start_new_session=True)
        CHILD['proc'] = proc
        buffer = b''
        try:
            while True:
                chunk = proc.stderr.read1(4096) if hasattr(proc.stderr, 'read1') else proc.stderr.read(64)
                if not chunk: break
                buffer = (buffer + chunk)[-512:]
                on_curl(chunk.decode('utf-8', 'replace'))
            status = proc.wait()
        finally:
            CHILD['proc'] = None
        if CANCELLED.is_set(): raise KeyboardInterrupt
        if status:
            tail = buffer.decode('utf-8', 'replace').replace('\r', '\n').strip().splitlines()[-1:]
            raise BuildError('CMake could not be downloaded (curl exit %d%s). Check the internet connection and try '
                             'again.' % (status, ': ' + tail[0] if tail else ''))
        digest = sha256_file(partial)
        if digest != sha256:
            partial.unlink()
            raise BuildError('The CMake download did not match its checksum (got %s, expected %s); it was deleted. '
                             'Try again later.' % (digest, sha256))
        partial.replace(archive)
        say('CMake archive verified: SHA-256 %s' % digest)
    staging = Path(tempfile.mkdtemp(prefix='.cmake-unpack-', dir=str(tools)))
    try:
        if run(['/usr/bin/tar', '-xzf', archive, '-C', staging], quiet=True):
            raise BuildError('The CMake download could not be unpacked.')
        tops = [p for p in staging.iterdir() if p.name.startswith('cmake-')]
        if len(tops) != 1 or not (tops[0] / 'CMake.app/Contents/bin/cmake').is_file():
            raise BuildError('The CMake download has an unexpected layout.')
        target = tools / 'cmake'
        if target.exists(): shutil.rmtree(target)
        tops[0].rename(target)
        run(['/usr/bin/xattr', '-dr', 'com.apple.quarantine', target], quiet=True)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    archive.unlink()  # 60+ MB; the unpacked copy is what is kept
    return target / 'CMake.app/Contents/bin/cmake'


# --- Kit sync -------------------------------------------------------------------------------------------

def load_kit(kit):
    path = kit / 'kit.json'
    if not path.is_file(): raise BuildError('The app\'s build kit is missing (%s). Download the app again.' % path)
    return json.loads(path.read_text())


def sync_tree(source, dest, files, record_path):
    """Copies `files` ({relative: sha256}) from source to dest. Unchanged files (same recorded hash, file present
    with the recorded size) are left alone, keeping their mtimes; changed ones get the current time so make sees
    them as newer than any object built from the old version. Files of an earlier kit that are gone are removed.
    Anything else in dest (generated code, the build folder, links to the game files) is never touched."""
    record = json.loads(record_path.read_text()) if record_path.is_file() else {}
    copied = 0
    for relative, digest in sorted(files.items()):
        src, dst = source / relative, dest / relative
        old = record.get(relative)
        if old and old[0] == digest and dst.is_file() and dst.stat().st_size == old[1]:
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name('.' + dst.name + '.sync')
        shutil.copyfile(src, tmp)  # data only: no extended attributes (quarantine) are carried over
        os.chmod(tmp, src.stat().st_mode & 0o777)
        os.replace(tmp, dst)  # the new file's mtime is now
        record[relative] = [digest, dst.stat().st_size]
        copied += 1
    removed = 0
    for relative in sorted(set(record) - set(files)):
        path = dest / relative
        if path.is_file() or path.is_symlink():
            path.unlink()
            removed += 1
        del record[relative]
    record_path.write_text(json.dumps(record, indent=0, sort_keys=True))
    return copied, removed


# --- Build ----------------------------------------------------------------------------------------------

def clean_env(cmake_dir):
    """A predictable environment: no FABLE2_*/REXGLUE_* switches from the caller (e.g. FABLE2_RECOMP_PATCHES=0 or a
    conservative codegen would silently change the game), PATH with CMake and the system tools only."""
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('FABLE2_', 'REXGLUE_', 'CMAKE_', 'MAKEFLAGS', 'MFLAGS'))}
    env['PATH'] = ':'.join([str(cmake_dir), '/usr/bin', '/bin', '/usr/sbin', '/sbin'])
    return env


def clang_identity(env):
    out = subprocess.run(['/usr/bin/xcrun', 'clang++', '--version'], capture_output=True, text=True, env=env)
    if out.returncode: raise BuildError('Apple\'s Command Line Tools are not installed or not working (clang++ did not '
                                        'run). Install them and try again.')
    sdk = subprocess.run(['/usr/bin/xcrun', '--show-sdk-version'], capture_output=True, text=True, env=env).stdout
    return out.stdout.splitlines()[0].strip() + ' / SDK ' + sdk.strip()


MEASURE_LAUNCHER = r'''#!/bin/bash
# Compiler launcher for --measure builds (tools/fable2_game_build.py): /usr/bin/time -l per compile.
source=""
prev=""
output=""
for a in "$@"; do [[ "$prev" == "-c" ]] && source="$a"; [[ "$prev" == "-o" ]] && output="$a"; prev="$a"; done
[[ -z "$source" ]] && source="link:$output"
out="$(dirname "$0")/$$-$RANDOM.txt"
echo "source=$source" > "$out"
echo "start=$(/bin/date +%s)" >> "$out"
exec /usr/bin/time -l -a -o "$out" "$@"
'''


def write_state(build, state):
    tmp = build / (STATE_FILE + '.tmp')
    tmp.write_text(json.dumps(state, indent=2) + '\n')
    os.replace(tmp, build / STATE_FILE)


def read_state(build):
    try:
        return json.loads((build / STATE_FILE).read_text())
    except (OSError, ValueError):
        return {}


def status(kit, state_root):
    """Whether the game is built for this kit: build-state.json names this kit and the engine exists."""
    build = state_root / 'Build'
    state = read_state(build)
    try:
        kit_id = load_kit(kit)['kit_id']
    except BuildError:
        kit_id = None
    engine = state_root / 'Game' / GAME_APP / 'Contents/MacOS/fable_2'
    ready = bool(state.get('ok') and kit_id and state.get('kit_id') == kit_id and engine.is_file())
    return dict(ready=ready, kit_id=kit_id, built_kit_id=state.get('kit_id'), engine=str(engine),
                built=state.get('finished'), seconds=state.get('seconds'))


def assemble(kit, kit_info, build_dir, state_root, binary):
    """<state>/Game/Fable II.bundle: the built fable_2 next to the runtime libraries, with the app's Info.plist (same
    bundle id, so the Dock and activation treat it as Fable II), ad-hoc signed on this Mac. Built beside the old one
    and swapped in only when complete and verified."""
    game = state_root / 'Game'
    game.mkdir(parents=True, exist_ok=True)
    legacy = game / 'Fable II.app'  # earlier test builds named it .app (listed by Spotlight as a second app)
    if legacy.is_dir(): shutil.rmtree(legacy)
    final = game / GAME_APP
    work = game / ('.%s.new' % GAME_APP)
    if work.exists(): shutil.rmtree(work)
    macos, resources = work / 'Contents/MacOS', work / 'Contents/Resources'
    macos.mkdir(parents=True)
    resources.mkdir()
    shutil.copyfile(binary, macos / 'fable_2')
    os.chmod(macos / 'fable_2', 0o755)
    for name in kit_info['runtime']:
        shutil.copyfile(kit / 'runtime' / name, macos / name)
        os.chmod(macos / name, 0o755)
    for name in ('Info.plist', 'PkgInfo'):
        shutil.copyfile(kit / 'game-bundle' / name, work / 'Contents' / name)
    for path in sorted((kit / 'game-bundle/Resources').iterdir()):
        shutil.copyfile(path, resources / path.name)
    if run(['/usr/bin/xattr', '-cr', work], quiet=True): raise BuildError('Could not clear file attributes of %s.' % work)
    for name in sorted(kit_info['runtime']) + ['fable_2']:
        if run(['/usr/bin/codesign', '--force', '--sign', '-', macos / name]):
            raise BuildError('Signing %s failed (codesign). See the details.' % name)
    if run(['/usr/bin/codesign', '--force', '--sign', '-', work]):
        raise BuildError('Signing the game bundle failed (codesign). See the details.')
    if run(['/usr/bin/codesign', '--verify', '--strict', work]):
        raise BuildError('The signed game bundle did not verify (codesign --verify). See the details.')
    old = game / ('.%s.old' % GAME_APP)
    if old.exists(): shutil.rmtree(old)
    if final.exists(): final.rename(old)
    work.rename(final)
    if old.exists(): shutil.rmtree(old)
    return final


def build(args):
    started = time.monotonic()
    kit, state_root = Path(args.kit).resolve(), Path(args.state).resolve()
    game_data = Path(args.game_data).resolve()
    build_dir = state_root / 'Build'
    build_dir.mkdir(parents=True, exist_ok=True)
    kit_info = load_kit(kit)
    xex = game_data / 'default.xex'
    if not xex.is_file() or not (game_data / 'data').is_dir():
        raise BuildError('The game files are not ready (%s has no default.xex or data folder). Choose your disc image '
                         'again.' % game_data)
    times, notes = {}, {}
    mark = [time.monotonic()]

    def done(key):
        now = time.monotonic()
        times[key] = round(now - mark[0], 1)
        mark[0] = now

    # 1. CMake --------------------------------------------------------------------------------------------
    stage('cmake', 'Checking for CMake')
    progress('cmake', 0, 'Checking for CMake…')
    cmake, version = find_cmake(build_dir, args.ignore_system_cmake)
    if cmake is None:
        cmake = download_cmake(build_dir, args.cmake_url or CMAKE_PIN['url'], args.cmake_sha256 or CMAKE_PIN['sha256'])
        version = cmake_version(cmake)
        notes['cmake_downloaded'] = True
    say('Using CMake %s at %s' % ('.'.join(map(str, version)), cmake))
    env = clean_env(cmake.parent)
    clang = clang_identity(env)
    say('Compiler: ' + clang)
    done('cmake')

    # 2. Sync ---------------------------------------------------------------------------------------------
    stage('sync', 'Copying the build kit')
    progress('sync', 0, 'Copying the build kit…')
    previous = read_state(build_dir)
    src = build_dir / 'src'
    binary_dir = src / 'out/build' / PRESET
    if args.clean or (previous.get('clang') and previous.get('clang') != clang):
        if binary_dir.exists():
            say('Removing the old build folder (%s)' % ('--clean' if args.clean else 'the compiler changed: ' + clang))
            shutil.rmtree(binary_dir)
    if args.clean and (src / 'generated').exists():
        say('Removing the generated code (--clean)')
        shutil.rmtree(src / 'generated')
    copied_sdk, removed_sdk = sync_tree(kit / 'sdk', build_dir / 'sdk', kit_info['files']['sdk'],
                                        build_dir / '.sync-sdk.json')
    copied_src, removed_src = sync_tree(kit / 'game-src', src, kit_info['files']['game-src'],
                                        build_dir / '.sync-src.json')
    say('Kit synced: SDK %d copied, %d removed; sources %d copied, %d removed' % (
        copied_sdk, removed_sdk, copied_src, removed_src))
    sdk = build_dir / 'sdk'
    rexglue = sdk / 'bin/rexglue'
    rexglue_sha = sha256_file(rexglue)
    xex_sha = sha256_file(xex)
    # A copy, not a link: rexglue refuses an entrypoint that resolves outside the project root. Kept (with its
    # mtime) while the content is the same, so codegen does not rerun.
    copy = src / 'default.xex'
    if copy.is_symlink() or (copy.exists() and sha256_file(copy) != xex_sha): copy.unlink()
    if not copy.exists():
        shutil.copyfile(xex, copy)
    if kit_info.get('xex_sha256') and xex_sha != kit_info['xex_sha256']:
        raise BuildError('This disc image\'s default.xex (SHA-256 %s) is not the version this release is made for. The '
                         'game code is translated from that exact file, so it cannot be built from this image.' % xex_sha)
    if previous.get('rexglue_sha256') and previous.get('rexglue_sha256') != rexglue_sha:
        # The codegen depfile does not list the recompiler itself: force codegen (and rexglue.cmake) after it changed.
        for stale in (src / 'generated/default/codegen.build.stamp', src / 'generated/rexglue.cmake'):
            if stale.exists(): stale.unlink()
        say('The recompiler changed: code generation runs again')
    progress('sync', 1, 'Build kit ready')
    done('sync')

    # 3. Codegen ------------------------------------------------------------------------------------------
    stage('codegen', 'Translating the game code (code generation)')
    progress('codegen', 0, 'Translating the game code…')
    if not (src / 'generated/rexglue.cmake').is_file():
        scaffold = Path(tempfile.mkdtemp(prefix='.scaffold-', dir=str(build_dir)))
        try:
            if run([rexglue, 'init', '--project-name', 'fable_2', '--xex-path', xex, '--project-root', scaffold], env=env):
                raise BuildError('The recompiler could not read default.xex (rexglue init). See the details.')
            (src / 'generated').mkdir(exist_ok=True)
            shutil.copyfile(scaffold / 'generated/rexglue.cmake', src / 'generated/rexglue.cmake')
        finally:
            shutil.rmtree(scaffold, ignore_errors=True)
    configure = [cmake, '--preset', PRESET, '-G', 'Unix Makefiles', '-DCMAKE_PREFIX_PATH=' + str(sdk),
                 '-DREXGLUE_SDK_ROOT=' + str(sdk), '-DCMAKE_C_FLAGS=-mcpu=apple-m1', '-DCMAKE_CXX_FLAGS=-mcpu=apple-m1',
                 '-DFABLE2_GPU_PLUGIN_XENOS=OFF', '-DREXGLUE_RECOMP_DEBUG_INFO=' + args.debug_info,
                 # The build's own Python steps (recomp patches, version header) run under the same /usr/bin/python3
                 # (Command Line Tools) as this script, never a Homebrew Python that happens to be next to CMake.
                 '-DPython3_EXECUTABLE=/usr/bin/python3']
    measure_dir = build_dir / 'measure'
    if args.measure:
        if measure_dir.exists(): shutil.rmtree(measure_dir)
        measure_dir.mkdir()
        launcher = measure_dir / 'launch.sh'
        launcher.write_text(MEASURE_LAUNCHER)
        launcher.chmod(0o755)
        configure += ['-DCMAKE_CXX_COMPILER_LAUNCHER=' + str(launcher), '-DCMAKE_CXX_LINKER_LAUNCHER=' + str(launcher)]
    else:
        configure += ['-DCMAKE_CXX_COMPILER_LAUNCHER=', '-DCMAKE_CXX_LINKER_LAUNCHER=']
    if run(configure, cwd=src, env=env):
        raise BuildError('Configuring the build failed (cmake). See the details.')
    progress('codegen', 0.15, 'Translating the game code…')
    lines = [0]

    def on_codegen(_line):
        lines[0] += 1
        if lines[0] % 20 == 0: progress('codegen', min(0.15 + lines[0] / 4000.0, 0.85), 'Translating the game code…')
    if run([cmake, '--build', binary_dir, '--target', 'fable_2_codegen'], cwd=src, env=env, on_line=on_codegen):
        raise BuildError('Code generation failed (rexglue codegen). See the details.')
    progress('codegen', 0.9, 'Configuring the build…')
    if run(configure, cwd=src, env=env):
        raise BuildError('Configuring the build after code generation failed (cmake). See the details.')
    if run([cmake, '--build', binary_dir, '--target', 'fable_2_recomp_patches'], cwd=src, env=env):
        raise BuildError('Applying the recomp patches to the generated code failed. See the details.')
    progress('codegen', 1, 'Game code translated')
    done('codegen')

    # 4./5. Compile and link ----------------------------------------------------------------------------
    plan = plan_jobs(override=args.jobs or os.environ.get('FABLE2_BUILD_JOBS'))
    jobs = plan['jobs']
    say('Jobs: %d (%s)' % (jobs, plan['why']))
    stage('compile', 'Compiling the game (%d jobs)' % jobs)
    progress('compile', 0, 'Compiling the game…')
    watch = MemoryWatch()
    watch.start()
    compile_started = time.monotonic()
    state = {'pct': 0, 'linking': False, 'eta_at': 0.0}

    def on_make(line):
        match = re.match(r'^\[\s*(\d+)%\]', line)
        if match:
            pct = int(match.group(1))
            state['pct'] = max(state['pct'], pct)
            elapsed = time.monotonic() - compile_started
            text = 'Compiling the game… %d%%' % state['pct']
            if re.search(r'Linking CXX executable fable_2$', line) and not state['linking']:
                state['linking'] = True
                done('compile')
                stage('link', 'Linking')
                progress('link', 0, 'Linking the game…')
                return
            if not state['linking']:
                progress('compile', state['pct'] / 100.0, text)
                if state['pct'] >= 5 and time.monotonic() - state['eta_at'] > 2:
                    state['eta_at'] = time.monotonic()
                    say('[eta] %d' % int(elapsed / state['pct'] * (100 - state['pct'])))
    try:
        if args.register_first:
            # The earlier build: the register TU (one -O0 function that registers ~61,000 entry points) alone
            # first. By default it is one of the first jobs of the parallel build instead (it is listed before the
            # recomp units and needs no PCH), which keeps the other cores busy during its ~5-7 s.
            status_code = run([cmake, '--build', binary_dir, '--target', REGISTER_OBJECT], cwd=src, env=env)
            if status_code and not watch.tripped.is_set():
                raise BuildError('Compiling fable_2_register.cpp failed. See the details.')
        backoffs = []
        while True:
            watch.rearm()
            status_code = run([cmake, '--build', binary_dir, '--target', 'fable_2', '--parallel', str(jobs)], cwd=src,
                              env=env, on_line=on_make)
            if status_code == 0: break
            if watch.tripped.is_set() and jobs > 1:
                new_jobs = max(1, jobs // 2)
                say('[memory] %s: stopping and continuing with %d jobs instead of %d' % (watch.reason, new_jobs, jobs))
                backoffs.append(dict(reason=watch.reason, jobs_from=jobs, jobs_to=new_jobs,
                                     at_s=round(time.monotonic() - compile_started, 1)))
                jobs = new_jobs
                continue
            raise BuildError('Compiling the game failed (make exit %d). See the details; "Try Again" continues '
                             'where it stopped.' % status_code)
    finally:
        watch.stop.set()
    if not state['linking']: done('compile')
    progress('link', 1, 'Linked')
    done('link')

    # 6. Assemble and sign ------------------------------------------------------------------------------
    stage('assemble', 'Signing the game on this Mac (ad hoc)')
    progress('assemble', 0, 'Signing the game on this Mac…')
    app = assemble(kit, kit_info, build_dir, state_root, binary_dir / 'fable_2')
    done('assemble')
    result = dict(ok=True, kit_id=kit_info['kit_id'], version=kit_info.get('version'), clang=clang,
                  rexglue_sha256=rexglue_sha, xex_sha256=xex_sha, jobs=plan['jobs'], final_jobs=jobs, plan=plan,
                  memory=dict(backoffs=backoffs, peak_pressure_level=watch.peak_pressure,
                              peak_swap_growth_mb=round(watch.peak_swap_growth)),
                  debug_info=args.debug_info, register_first=bool(args.register_first),
                  steps=times, seconds=round(time.monotonic() - started, 1), engine=str(app / 'Contents/MacOS/fable_2'),
                  finished=time.strftime('%Y-%m-%d %H:%M:%S'), measured=bool(args.measure), **notes)
    write_state(build_dir, result)
    stage('done', 'The game is built')
    say('[progress] 1.0000 Built in %d:%02d' % divmod(int(result['seconds']), 60))
    say('[result] ' + json.dumps(result))
    return result


# --- Measurement report ---------------------------------------------------------------------------------

def measure_report(state_root):
    rows = []
    for path in sorted((Path(state_root) / 'Build/measure').glob('*.txt')):
        text = path.read_text(errors='replace')
        source = re.search(r'^source=(.*)$', text, re.M)
        real = re.search(r'([0-9.]+) real\s+([0-9.]+) user\s+([0-9.]+) sys', text)
        rss = re.search(r'(\d+)\s+maximum resident set size', text)
        footprint = re.search(r'(\d+)\s+peak memory footprint', text)
        start = re.search(r'^start=(\d+)', text, re.M)
        if not (source and real and rss): continue
        rows.append(dict(source=source.group(1).split('/src/', 1)[-1].split('/Build/', 1)[-1], real_s=float(real.group(1)),
                         cpu_s=float(real.group(2)) + float(real.group(3)), rss_mb=round(int(rss.group(1)) / 1048576),
                         footprint_mb=round(int(footprint.group(1)) / 1048576) if footprint else None,
                         start=int(start.group(1)) if start else None))
    rows.sort(key=lambda r: -r['rss_mb'])
    generated = [r for r in rows if 'generated/' in r['source']]
    def pct(values, p):
        values = sorted(values)
        return values[min(len(values) - 1, int(len(values) * p))] if values else None
    return dict(units=len(rows), cpu_s=round(sum(r['cpu_s'] for r in rows)),
                rss_mb=dict(max=rows[0]['rss_mb'] if rows else None, p50=pct([r['rss_mb'] for r in rows], 0.5),
                            p90=pct([r['rss_mb'] for r in rows], 0.9),
                            generated_p50=pct([r['rss_mb'] for r in generated], 0.5)),
                top_rss=rows[:12], top_time=sorted(rows, key=lambda r: -r['real_s'])[:8])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--kit', required=True)
    b.add_argument('--state', required=True)
    b.add_argument('--game-data', required=True)
    b.add_argument('--jobs', type=int)
    b.add_argument('--measure', action='store_true', help='per-TU time and peak RSS via /usr/bin/time -l')
    b.add_argument('--ignore-system-cmake', action='store_true', help='use or download our own CMake only (testing)')
    b.add_argument('--cmake-url', help='CMake archive URL (testing: a file:// copy)')
    b.add_argument('--cmake-sha256')
    b.add_argument('--clean', action='store_true', help='remove the build folder first (full recompile)')
    b.add_argument('--debug-info', choices=('none', 'line-tables-only', 'full'), default=RECOMP_DEBUG_INFO,
                   help='debug info for the generated code (default %(default)s; line-tables-only = the earlier build)')
    b.add_argument('--register-first', action='store_true',
                   help='compile fable_2_register.cpp alone before the parallel build (the earlier build)')
    p = sub.add_parser('plan')
    p.add_argument('--jobs', type=int)
    m = sub.add_parser('measure-report')
    m.add_argument('state')
    s = sub.add_parser('status')
    s.add_argument('--kit', required=True)
    s.add_argument('--state', required=True)
    args = ap.parse_args()
    if args.cmd == 'plan':
        print(json.dumps(plan_jobs(override=args.jobs), indent=2))
        return 0
    if args.cmd == 'measure-report':
        print(json.dumps(measure_report(args.state), indent=2))
        return 0
    if args.cmd == 'status':
        print(json.dumps(status(Path(args.kit), Path(args.state))))
        return 0
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    try:
        build(args)
        return 0
    except KeyboardInterrupt:
        say('[cancelled] Stopped. What was built so far is kept; the next try continues from there.')
        return 3
    except BuildError as error:
        say('[error] ' + str(error))
        return 1
    except PermissionError as error:
        # macOS privacy (Downloads, Desktop, Documents, iCloud Drive, other volumes) or file permissions: the app
        # explains it and offers Privacy Settings / Try Again (docs/release-app.md).
        say('[denied] %d %s' % (error.errno or 1, error.filename or ''))
        say('[error] macOS did not allow reading %s (%s).' % (error.filename or 'a file', error.strerror))
        return 1
    except Exception as error:  # unexpected: still a readable line for the window
        say('[error] Unexpected problem: %s: %s' % (type(error).__name__, error))
        import traceback
        traceback.print_exc(file=sys.stdout)
        return 1


if __name__ == '__main__':
    sys.exit(main())
