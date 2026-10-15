#!/usr/bin/env python3
"""Built-in benchmark on this Mac (docs/benchmark-kit.md); the Dev Launcher's "Run Benchmark…" calls it.

  python3 tools/fable2_benchmark.py run [--build latest|playtest] [--save NAME|live:HeroNNN]
                                        [--arms cold,1u,1c,2u,2c,3u,3c,census1x,census2x] [--out DIR] [--warm-cache DIR|none]
                                        [--progress FILE] [--no-perf-lock]
  python3 tools/fable2_benchmark.py run --kit "<kit>/Fable II Benchmark.app" --iso IMAGE --state DIR [--no-xcode]
                                        (test a packaged kit as on a fresh Mac; game files extracted into DIR)
  python3 tools/fable2_benchmark.py progress     (the running benchmark's last progress line, JSON)

Runs the same thing as the benchmark kit on another Mac: FableIIBenchmark (tools/macos/Benchmark/main.swift,
compiled into out/benchmark/bin when its source changes) starts the chosen build's Fable II.app once per arm
in the game's benchmark mode (src/core/fable2_benchmark.h: boot the save, wait for gameplay, f2diag's
headings8 camera plan, quit) and writes report.json + report.txt.

- State: its own folder, <checkout>/out/benchmark/state (saves reset from the staged save before every arm,
  its own shader cache and logs). The live state is only read: the save library entry or live slot, the
  profile options (camera settings) and, for the warm arms, an APFS clone of the live shader cache
  (--warm-cache none: the warm arms continue from the cold arm's cache). From an agent slot the default is
  --warm-cache agent: a read-only seed from the agent shader cache of the build's shader-code key
  (tools/fable2_agent_cache.py; FABLE2_AGENT_CACHE=KEY@GEN|PIN pins a snapshot), recorded as result agent_cache.
  The main checkout (the Dev Launcher's "Run Benchmark…") keeps live.
- Window: the game window opens at the remembered test-window position (tools/fable2_test_windows.py; position
  only, at the game's default size, so every run measures the same window size) and a move is remembered for the
  next test window. FABLE2_TEST_WINDOW_MEMORY=0 leaves the state folder's own fable2_window.toml as before.
- Report: <checkout>/out/benchmark/reports/<chip>-<model>-<date>/ (--out DIR to change).
- App identity: the chosen Fable II.app runs as an APFS clone in out/benchmark/app with its own bundle id
  (org.fable2recomp.metal.benchmark.dev, re-signed ad hoc), so macOS keeps a separate Metal shader cache for it and
  the cold arm can empty that cache (FableIIBenchmark) without touching the one of normal play and agent tests.
- Refuses while a Fable II game other than an agent test game runs. Takes the global perf lock exclusively
  (local agent docs): running agent test games finish first and none start meanwhile. Then holds one agent game
  slot (and the 3x slot) for the run (FABLE2_AGENT_GAMES).
Prints one JSON object: {"ok": true, "result": {"folder": ..., "report": <report.txt text>}}.
Runs under /usr/bin/python3 (3.9).
"""
import argparse
import contextlib
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_common as common  # noqa: E402
import fable2_instances as instances  # noqa: E402
import fable2_playtest as playtest  # noqa: E402
import fable2_test_control as control  # noqa: E402
import fable2_test_windows as test_windows  # noqa: E402
import fable2_version  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'out/benchmark'
SOURCE = ROOT / 'tools/macos/Benchmark/main.swift'
SAVE = 'hero1-dog-live_2026-10-11_11-20-20'  # Bower Lake
LIVE_STATE = Path.home() / 'Library/Application Support/Fable II Recomp'
PROFILE = Path('saves/4D5307F1/profile/User')
PROGRESS = WORK / 'progress.txt'
# Background test-session environment, as tools/fable2_dev_session.py launch(test_control=True) sets it (keep in
# sync; that file needs the maintainer's approval to change): no activation or focus stealing, no physical input, the
# logo movies skipped, the window titled as an agent test. FableIIBenchmark adds FABLE2_TEST_CONTROL_DIR (App Nap
# exemption) and passes --audio_mute=true.
TEST_SESSION_ENV = dict(FABLE2_TEST_BACKGROUND='1', FABLE2_PHYSICAL_INPUT='0', FABLE2_SKIP_INTRO_MOVIES='1',
                        FABLE2_INSTANCE_ROLE='agent', FABLE2_INSTANCE_ID='benchmark')


BENCH_BUNDLE_ID = 'org.fable2recomp.metal.benchmark.dev'  # FableIIBenchmark clears only org.fable2recomp.metal.benchmark[.*]


def benchmark_app(app):
    """An APFS clone of `app` under WORK/app with BENCH_BUNDLE_ID (ad-hoc re-signed, as tools/fable2_playtest.py does):
    its own macOS Metal shader cache, which the cold arm may clear. Re-made when the source app changes."""
    app = Path(app)
    dest = WORK / 'app' / app.name
    stamp_path = WORK / 'app/source.json'
    files = [app / 'Contents/Info.plist', app / 'Contents/MacOS/fable_2', app / 'Contents/MacOS/FableIILauncher']
    stamp = dict(app=str(app.resolve()), files=[[f.name, f.stat().st_size, f.stat().st_mtime_ns] for f in files])
    try:
        if dest.is_dir() and json.loads(stamp_path.read_text()) == stamp: return dest
    except (OSError, ValueError):
        pass
    if dest.exists(): shutil.rmtree(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cp', '-cR', str(app), str(dest)], check=True)  # clonefile: shares blocks
    plist_path = dest / 'Contents/Info.plist'
    with open(plist_path, 'rb') as f: plist = plistlib.load(f)
    plist['CFBundleIdentifier'] = BENCH_BUNDLE_ID
    with open(plist_path, 'wb') as f: plistlib.dump(plist, f)
    subprocess.run(['codesign', '--force', '--sign', '-', str(dest)], check=True, stderr=subprocess.DEVNULL)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(dest)], check=True)
    stamp_path.write_text(json.dumps(stamp))
    return dest


def stage_save(dest, save):
    """dest/saves: one hero save (library entry NAME, or live:HeroNNN read from the live state) as the only
    save, plus the live profile options (read only: the camera settings must match f2diag sessions)."""
    dest = Path(dest)
    if dest.exists(): shutil.rmtree(dest)
    dest.mkdir(parents=True)
    if save.startswith('live:'):
        hero = save[5:]
        source = LIVE_STATE / control.HERO_SLOTS / hero
        header = LIVE_STATE / control.HERO_HEADERS / (hero + '.header')
        if not source.is_dir() or not header.is_file(): raise RuntimeError(f'No live save {hero}')
        (dest / control.HERO_SLOTS).mkdir(parents=True)
        (dest / control.HERO_HEADERS).mkdir(parents=True)
        shutil.copytree(source, dest / control.HERO_SLOTS / hero)
        shutil.copy2(header, dest / control.HERO_HEADERS / header.name)
        meta = control.describe_save(source, header)
    else:
        if not (control.library_entry(save) / 'save.json').is_file():
            raise RuntimeError(f'No save {save!r} in the save library')
        meta = control.apply_library_save(dest, save)
    profile = LIVE_STATE / PROFILE
    if profile.is_dir(): shutil.copytree(profile, dest / PROFILE)
    return dict(name=save, level=meta.get('LevelName'), quest=meta.get('QuestName'), profile=profile.is_dir())


def orchestrator():
    """out/benchmark/bin/FableIIBenchmark, rebuilt when main.swift changes (needs Xcode on the build Mac)."""
    digest = hashlib.sha256(SOURCE.read_bytes()).hexdigest()[:16]
    binary, stamp = WORK / 'bin/FableIIBenchmark', WORK / 'bin/FableIIBenchmark.src'
    if binary.is_file() and stamp.is_file() and stamp.read_text() == digest: return binary
    binary.parent.mkdir(parents=True, exist_ok=True)
    with common.build_marker(1, 'benchmark orchestrator'):  # builds don't overlap perf runs
        subprocess.run(['xcrun', 'swiftc', '-O', '-swift-version', '5', '-parse-as-library', '-target',
                        'arm64-apple-macos15.0', '-o', str(binary), str(SOURCE)], check=True)
    stamp.write_text(digest)
    return binary


def run(build='latest', save=SAVE, arms=None, out=None, warm_cache=None, progress=PROGRESS, perf_lock=True, verbose=False,
        kit=None, iso=None, state=None, no_xcode=False, flash=False, reps=None):
    def refuse(entries):
        if entries:
            raise RuntimeError('A Fable II game is running (%s); close it first.' % instances.describe(entries[0]))
    # Agent test games end on their own: the perf lock below waits for them. Any other game: refuse now.
    refuse([e for e in instances.instances() if not (perf_lock and e.get('test_control'))])
    # Absolute paths: the kit orchestrator runs with its own working directory, so a relative --state/--out/--iso
    # (or --kit) failed there.
    absolute = lambda value: str(Path(value).expanduser().resolve()) if value else value
    state = Path(absolute(state)) if state else WORK / 'state'
    out, iso, kit = absolute(out), absolute(iso), absolute(kit)
    if warm_cache is None: warm_cache = 'agent' if common.IS_SLOT else 'live'
    if kit:
        # A packaged kit app (tools/fable2_benchmark_kit.py) run as on a fresh Mac: its own orchestrator, bundled
        # save and shader seed, game files extracted from the disc image into its state folder (no clone).
        app = Path(kit)
        binary = app / 'Contents/MacOS/FableIIBenchmark'
        if not binary.is_file(): raise RuntimeError(f'Not a benchmark kit app: {app}')
        if not iso or not Path(iso).is_file(): raise RuntimeError('--kit needs --iso IMAGE')
        staged = dict(name='bundled')
        args = [str(binary), '--headless', '--state', str(state), '--out', str(out or WORK / 'reports'), '--iso', str(iso),
                '--no-clone']
        warm_cache = None
    else:
        app = playtest.app_for(build)
        data_root = playtest.data_root_for(build)
        if not (app / 'Contents/MacOS/FableIILauncher').is_file(): raise RuntimeError(f'No app for build {build}: {app}')
        binary = orchestrator()
        staged = stage_save(WORK / 'save', save)
        args = [str(binary), '--headless', '--app', str(benchmark_app(app)), '--game-data', str(data_root),
                '--save-dir', str(WORK / 'save/saves'), '--state', str(state),
                '--out', str(out or WORK / 'reports'),
                '--label', '%s build %s, save %s (%s)' % (build, instances.app_version(app) or '?', save, staged['level'])]
    if arms: args += ['--arms', arms]
    # Flash kit mode (docs/flash-kit.md): its arms, 2x, the flash plan and the in-game flash detector. A flash kit
    # app (--kit) is in flash mode by itself (Info.plist Fable2FlashKit).
    if flash: args.append('--flash')
    if reps: args += ['--reps', str(reps)]
    agent_seed = None
    if warm_cache == 'agent':
        # Read-only clone (never published from: the benchmark's arms change the cache in their own state folder).
        import fable2_agent_cache as agent_cache
        seed_dir = WORK / 'agent-cache-seed'
        shutil.rmtree(seed_dir, ignore_errors=True)
        mode = os.environ.get('FABLE2_AGENT_CACHE') or 'auto'
        agent_seed = agent_cache.seed(seed_dir, app, 'readonly' if mode == 'auto' else mode)
        agent_seed['publish'] = False
        warm_cache = str(seed_dir)
    if warm_cache == 'live': warm_cache = str(LIVE_STATE / 'cache')
    if warm_cache and warm_cache != 'none':
        if not Path(warm_cache).is_dir(): raise RuntimeError(f'--warm-cache: no folder {warm_cache}')
        args += ['--warm-cache', absolute(warm_cache)]
    progress = Path(progress)
    progress.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.ExitStack() as stack:
        wait = stack.enter_context(common.perf_lock('fable2_benchmark run %s' % build)) if perf_lock else None
        # Agent game limit (local agent docs): its games run one at a time, so one game slot (and the 3x slot
        # when a 3x arm runs) for the whole run, after the perf lock (with it held, no agent game holds a slot).
        big = not flash and any(a.strip().startswith('3') for a in (arms or '3u').split(','))
        stack.enter_context(common.game_slots_held('fable2_benchmark run %s' % build, 3 if big else 1))
        refuse(instances.instances())
        env = dict(os.environ, **TEST_SESSION_ENV)
        if kit: env['FABLE2_PORTABLE_HEADLESS'] = '1'  # FableIIPrepare without its window
        if no_xcode: env['FABLE2_NS_ASSUME_NO_XCODE'] = '1'  # behave as on a Mac without the Metal toolchain
        env.pop('FABLE2_GAME_DATA_ROOT', None)
        test_window = test_windows.prepare(state, position_only=True) if test_windows.enabled() else None
        if test_window:
            test_windows.update_claim(test_window['key'], os.getpid())
            stack.callback(lambda: test_windows.commit(state, test_window, os.getpid()))
        started = time.monotonic()
        with open(progress, 'w') as log:
            log.write(json.dumps(dict(t=time.time(), text='Starting the benchmark…', pct=0)) + '\n')
            log.flush()
            proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            for line in proc.stderr:  # "[NN%] text" progress, "[game-pid] PID ARM", "[time] ..." log lines
                line = line.rstrip('\n')
                if line.startswith('[game-pid] '):  # instance registry: contention checks and the launcher see it
                    try:
                        pid = int(line.split()[1])
                        for _ in range(100):  # FableIILauncher execs fable_2 in the same process
                            info = instances.process_info(pid)
                            if info is None or Path(info['exe']).name == 'fable_2': break
                            time.sleep(0.1)
                        instances.register(pid, 'agent', app, state, True, env, state)
                    except Exception as error:
                        print('register failed: %s' % error, file=sys.stderr)
                    continue
                if line.startswith('[') and '%] ' in line[:7]:
                    pct, text = line[1:].split('%] ', 1)
                    log.write(json.dumps(dict(t=time.time(), text=text, pct=int(pct))) + '\n')
                    log.flush()
                    if verbose: print(line, file=sys.stderr, flush=True)
            folder = proc.stdout.read().strip()
            proc.wait()
            log.write(json.dumps(dict(t=time.time(), text='done' if proc.returncode == 0 else 'failed',
                                      pct=100, done=True, ok=proc.returncode == 0, folder=folder)) + '\n')
    if proc.returncode != 0 or not folder:
        raise RuntimeError('the benchmark failed (log: %s)' % (state / 'benchmark.log'))
    report = (Path(folder) / 'report.txt').read_text()
    return dict(folder=folder, report=report, seconds=round(time.monotonic() - started, 1), perf_lock_wait_s=wait,
                save=staged, build=build, git=fable2_version.info()['string'], agent_cache=agent_seed)


def last_progress(progress=PROGRESS):
    try:
        lines = Path(progress).read_text().splitlines()
        return json.loads(lines[-1]) if lines else {}
    except (OSError, ValueError):
        return {}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run', help='run the benchmark on this Mac')
    r.add_argument('--build', default='latest', choices=['latest', 'playtest'])
    r.add_argument('--save', default=SAVE, help='save-library entry or live:HeroNNN (default %s, Bower Lake)' % SAVE)
    r.add_argument('--arms', help='comma list of cold,1u,1c,2u,2c,3u,3c,census1x,census2x (default all)')
    r.add_argument('--out', help='report parent folder (default out/benchmark/reports)')
    r.add_argument('--warm-cache', default=None,
                   help='cache the warm arms start from: live (default in the main checkout), agent (default in an '
                        'agent slot: the agent shader cache), none, or a folder')
    r.add_argument('--progress', default=str(PROGRESS))
    r.add_argument('--no-perf-lock', action='store_true')
    r.add_argument('--verbose', action='store_true', help='progress lines on stderr')
    r.add_argument('--kit', help='run a packaged kit app (Fable II Benchmark.app) as on a fresh Mac instead of a build')
    r.add_argument('--iso', help='--kit: disc image to extract the game files from (read only)')
    r.add_argument('--state', help='state folder (default out/benchmark/state)')
    r.add_argument('--no-xcode', action='store_true', help='FABLE2_NS_ASSUME_NO_XCODE=1 (cache-only optimized shaders)')
    r.add_argument('--flash', action='store_true',
                   help='flash kit mode (docs/flash-kit.md): arms adapt,adapt-snap,adapt-tess3,noadapt,noadapt-snap'
                        '(,induce), 2x, flash plan + flash detector')
    r.add_argument('--reps', type=int, help='--flash: runs per arm (default 2)')
    sub.add_parser('progress', help='last progress line of the running benchmark')
    a = ap.parse_args()
    if a.cmd == 'progress':
        print(json.dumps({'ok': True, 'result': last_progress()}))
        return 0
    try:
        result = run(a.build, a.save, a.arms, a.out, a.warm_cache, a.progress, not a.no_perf_lock, a.verbose,
                     a.kit, a.iso, a.state, a.no_xcode, a.flash, a.reps)
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps({'ok': True, 'result': result}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
