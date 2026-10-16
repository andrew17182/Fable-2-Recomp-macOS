#!/usr/bin/env python3
"""Benchmark kit: "Fable II Benchmark Kit.zip" for measuring other Apple Silicon Macs (docs/benchmark-kit.md).

  python3 tools/fable2_benchmark_kit.py build [--out DIR] [--save NAME] [--no-zip]
                                              [--shader-scales 1,2,3 | --shaders CACHE_DIR ... | --no-shaders]
                                              [--seed-source auto|agent-cache|seed-store|prep|warm-up]
                                              [--e2e | --no-e2e] [--iso IMAGE] [--with-converter | --no-rebuild]

The app is the no-converter release build (out/build-release, refreshed first; docs/no-converter.md): no libdxilconv,
no libmetalirconverter, and the package check refuses either. --with-converter packages the dev build (out/build).

The shader seed comes from what is remembered for this build's shader code, without a game run when anything
matches (tools/fable2_portable.py resolve_seed, docs/offline-shader-seed.md). The end-to-end test (the packaged kit
run as on a fresh Mac: tools/fable2_benchmark.py run --kit, every arm, ~5 min) runs only with --e2e, or by default
when the kit or benchmark code changed since the last kit that passed it (E2E_STATE records the inputs' hash).

Makes <DIR>/Fable II Benchmark Kit/ ("Fable II Benchmark.app" + README.txt) and the zip next to it
(default DIR: out/benchmark-kit, ignored). The app is the portable build (tools/fable2_portable.py: the
player's own disc image is extracted on first run; bundled shader seed) whose main executable is
FableIIBenchmark (tools/macos/Benchmark/main.swift): double-clicked, it runs the game's built-in benchmark
(src/core/fable2_benchmark.h) once per arm and writes the report to ~/Desktop/Fable II Benchmark/. No
Terminal, Python or Xcode on the target Mac.

The kit carries one save from the save library (default SAVE, Bower Lake) plus the profile options of the
live state (read only; camera settings must match across Macs). It is the maintainer's own save for the maintainer's own Macs:
the zip lives in ignored out/ and is never committed. The game state on the target Mac goes to
~/Library/Application Support/Fable II Recomp Benchmark, never to the normal "Fable II Recomp" folder.
Runs under /usr/bin/python3 (3.9).
"""
import argparse
import hashlib
import json
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_benchmark as bench  # noqa: E402
import fable2_portable as portable  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / 'out/benchmark-kit'
SAVE = bench.SAVE  # Bower Lake
KIT = 'Fable II Benchmark Kit'
APP = 'Fable II Benchmark.app'
BUNDLE_ID = 'org.fable2recomp.metal.benchmark'
SOURCES = ['tools/macos/Benchmark/main.swift']
# The kit's own code: a change here means the kit end-to-end test runs again (auto mode).
E2E_INPUTS = ['tools/macos/Benchmark/main.swift', 'tools/fable2_benchmark_kit.py', 'tools/fable2_benchmark.py',
              'tools/fable2_portable.py', 'tools/macos/AppLauncher.m', 'tools/macos/Prepare/main.swift',
              'tools/macos/Setup/GDFX.swift', 'tools/macos/Setup/Support.swift', 'src/core/fable2_benchmark.h',
              'src/core/fable2_benchmark_pad.h']
E2E_STATE = bench.common.MAIN_ROOT / 'out/benchmark-kit/e2e-state.json'
ISO_DIR = bench.common.MAIN_ROOT.parent / 'ISO'

README = """Fable II Benchmark {version}

Measures how the Fable II macOS port runs on this Mac. Needs an Apple Silicon Mac (M1 or later),
macOS {min_os} or later, about 8 GB of free space, and your own Fable II disc image (.iso).
No Xcode, Python or Terminal needed.

1. Unzip "Fable II Benchmark Kit.zip" and open the folder.
2. Double-click "Fable II Benchmark". macOS blocks it the first time (the app is not notarized):
   click Done, then open System Settings > Privacy & Security, scroll down, click "Open Anyway"
   next to "Fable II Benchmark", and confirm. (Right-click > Open no longer works on macOS 15+.)
3. Click Start. The first time, choose your Fable II disc image: the game files are copied from it
   (a few minutes; the image is only read). If this Mac already has the portable Fable II with its
   game files, they are reused instead.
4. Plug in the power adapter, close other apps and leave the Mac alone. The game opens and closes by
   itself 10 times (cold first launch; 1x and 2x resolution uncapped and at 60 FPS; 3x uncapped;
   2x uncapped again with one renderer switch on; then traced runs at 1x and 2x, with and without
   that switch, that record per-pass GPU timing), about 4 minutes on an M5 Max, 5-6 minutes on an
   M3 Pro (plus the first-time copy). Cancel stops it.
5. macOS may ask whether "Fable II Benchmark" may access your Desktop: allow it, that is where the
   report goes. At the end, "Show Report" opens the folder
       Desktop/Fable II Benchmark/<chip>-<model>-<date>/
   report.txt is the summary, report.json the full data, raw/ the per-run logs. Send the whole folder.

Everything else stays in ~/Library/Application Support/Fable II Recomp Benchmark (game files, shader
cache, logs). It never touches the saves or settings of a normal Fable II Recomp install. Delete that
folder to free the space (about 7 GB) when you are done.

The app includes one saved game (Bower Lake) and optimized shaders compiled from the game's own
shaders. It contains no other game content. Keep it private; do not redistribute it.
"""


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT)] + list(args), capture_output=True, text=True).stdout.strip()


# The packaging differs per kit (tools/fable2_flash_kit.py passes its own): names, bundle id, Info.plist flags,
# README, e2e state and the e2e run's arguments.
BENCHMARK_VARIANT = dict(kit=KIT, app=APP, bundle_id=BUNDLE_ID, name='Fable II Benchmark', plist={'Fable2Benchmark': True},
                         readme=None, e2e_state=None, e2e_inputs=None, e2e_run={}, label='benchmark kit')


def e2e_inputs_hash(inputs=None):
    h = hashlib.sha256()
    for rel in inputs or E2E_INPUTS:
        path = ROOT / rel
        h.update(rel.encode() + b'\0' + (path.read_bytes() if path.is_file() else b'<missing>') + b'\0')
    return h.hexdigest()[:16]


def e2e_needed(state_path=None, inputs=None):
    """(bool, reason): auto mode runs the kit end-to-end test only when the kit/benchmark code changed since the last
    kit that passed it."""
    state_path = state_path or E2E_STATE
    try:
        state = json.loads(state_path.read_text())
    except (OSError, ValueError):
        return True, 'no earlier tested kit (%s)' % state_path
    digest = e2e_inputs_hash(inputs)
    if state.get('inputs') != digest:
        return True, 'kit/benchmark code changed since the kit tested %s (%s)' % (state.get('when'), state.get('git_sha', '?')[:7])
    return False, 'kit/benchmark code unchanged since the kit tested %s (%s)' % (state.get('when'), state.get('git_sha', '?')[:7])


def default_iso():
    images = sorted(ISO_DIR.glob('*.iso')) if ISO_DIR.is_dir() else []
    return images[0] if images else None


def run_e2e(app, iso, out_dir, **run_args):
    """The packaged kit as on a fresh Mac (empty state folder, disc image read only, no Xcode): every arm."""
    work = Path(out_dir) / 'e2e'
    if work.exists(): shutil.rmtree(work)
    work.mkdir(parents=True)
    started = time.monotonic()
    result = bench.run(kit=str(app), iso=str(iso), state=str(work / 'state'), out=str(work / 'reports'), no_xcode=True,
                       **run_args)
    shutil.rmtree(work / 'state/GameData', ignore_errors=True)  # 7 GB of extracted game files
    return dict(result, seconds=round(time.monotonic() - started, 1))


def build(out_dir, save=SAVE, zip_it=True, shader_sources=None, warm_scales=None, seed_source='auto', e2e='auto',
          iso=None, converter=False, rebuild=True, variant=None):
    v = dict(BENCHMARK_VARIANT, **(variant or {}))
    started = time.monotonic()
    out_dir = out_dir.absolute()
    out_dir.mkdir(parents=True, exist_ok=True)
    final, zip_path = out_dir / v['kit'], out_dir / (v['kit'] + '.zip')
    with tempfile.TemporaryDirectory(prefix='.kit-', dir=out_dir) as temporary:
        work = Path(temporary)
        result = portable.build(work, zip_it=False, skip_intro=True, shader_sources=shader_sources,
                                warm_scales=warm_scales, seed_source=seed_source, converter=converter, rebuild=rebuild)
        folder = work / v['kit']
        folder.mkdir()
        app = folder / v['app']
        Path(result['app']).rename(app)
        macos, resources = app / 'Contents/MacOS', app / 'Contents/Resources'
        with bench.common.build_marker(1, v['label']):  # builds don't overlap perf runs (compile only)
            run('xcrun', 'swiftc', '-O', '-swift-version', '5', '-parse-as-library', '-target', 'arm64-apple-macos15.0',
                '-o', macos / 'FableIIBenchmark', *[ROOT / s for s in SOURCES])
        # The portable app's launcher window (FableIIPlayer) is not used by the kit: FableIIBenchmark is the main
        # executable and runs FableIILauncher itself.
        (macos / portable.PLAYER_EXECUTABLE).unlink()
        saved = bench.stage_save(resources / 'benchmark-save', save)
        plist_path = app / 'Contents/Info.plist'
        info = plistlib.loads(plist_path.read_bytes())
        sha = git('rev-parse', 'HEAD')
        info.update(CFBundleExecutable='FableIIBenchmark', CFBundleIdentifier=v['bundle_id'],
                    CFBundleName=v['name'], CFBundleDisplayName=v['name'], **v['plist'])
        info.update(Fable2BenchmarkSave='%s (%s)' % (save, saved['level']),
                    Fable2GitSHA=sha, Fable2GitDirty=bool(git('status', '--porcelain', '--untracked-files=no')))
        plist_path.write_bytes(plistlib.dumps(info))
        for binary in ('FableIIBenchmark', 'FableIILauncher'):  # the launcher was signed bound to the old Info.plist
            run('codesign', '--force', '--sign', '-', macos / binary, stderr=subprocess.DEVNULL)
        run('codesign', '--force', '--sign', '-', app, stderr=subprocess.DEVNULL)
        run('codesign', '--verify', '--deep', '--strict', app)
        portable.check_package(app, converter)
        (folder / 'README.txt').write_text((v['readme'] or README).format(
            version=info.get('Fable2VersionString', ''), min_os=info.get('LSMinimumSystemVersion', '15.0')))
        if final.exists(): shutil.rmtree(final)
        folder.rename(final)
    if zip_it:
        if zip_path.exists(): zip_path.unlink()
        run('ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', final, zip_path)
    app = final / v['app']
    package_seconds = round(time.monotonic() - started, 1)
    out = dict(folder=str(final), app=str(app), zip=str(zip_path) if zip_it else None, save=saved,
               app_bytes=sum(p.stat().st_size for p in app.rglob('*') if p.is_file() and not p.is_symlink()),
               zip_bytes=zip_path.stat().st_size if zip_it else None, version=info.get('Fable2VersionString'),
               git_sha=sha, shaders=result.get('shaders'), shader_converter=bool(converter),
               runtime=result.get('runtime'), seconds=package_seconds)
    e2e_state = v['e2e_state'] or E2E_STATE
    run_it, why = ((True, '--e2e') if e2e == 'yes' else (False, '--no-e2e') if e2e == 'no'
                   else e2e_needed(e2e_state, v['e2e_inputs']))
    print('[kit] end-to-end test: %s (%s)' % ('running' if run_it else 'skipped', why), file=sys.stderr, flush=True)
    out['e2e'] = dict(run=run_it, reason=why)
    if run_it:
        iso = Path(iso) if iso else default_iso()
        if not iso or not iso.is_file(): raise RuntimeError('the kit end-to-end test needs --iso IMAGE (none in %s)' % ISO_DIR)
        e2e_result = run_e2e(app, iso, out_dir, **v['e2e_run'])
        out['e2e'].update(folder=e2e_result['folder'], seconds=e2e_result['seconds'])
        e2e_state.parent.mkdir(parents=True, exist_ok=True)
        e2e_state.write_text(json.dumps(dict(inputs=e2e_inputs_hash(v['e2e_inputs']), git_sha=sha, when=time.strftime('%Y-%m-%d %H:%M'),
                                             report=e2e_result['folder'], kit=str(app)), indent=2) + '\n')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='package the no-converter release build as the benchmark kit (+ zip)')
    b.add_argument('--out', type=Path, default=DEFAULT_OUT, help='output folder (default out/benchmark-kit)')
    b.add_argument('--save', default=SAVE, help='save-library entry to benchmark (default %s, Bower Lake)' % SAVE)
    b.add_argument('--no-zip', action='store_true')
    b.add_argument('--shader-scales', default='1,2,3', metavar='LIST',
                   help='resolution scales to warm the shader seed for (tools/fable2_portable.py)')
    b.add_argument('--shaders', action='append', metavar='CACHE_DIR', help='shader seed from these caches instead')
    b.add_argument('--no-shaders', action='store_true')
    b.add_argument('--seed-source', default='auto', choices=portable.SEED_SOURCES,
                   help='where the shader seed comes from (tools/fable2_portable.py; default auto)')
    e = b.add_mutually_exclusive_group()
    e.add_argument('--e2e', dest='e2e', action='store_const', const='yes',
                   help='always run the kit end-to-end test after packaging')
    e.add_argument('--no-e2e', dest='e2e', action='store_const', const='no', help='never run it')
    b.set_defaults(e2e='auto')
    b.add_argument('--iso', help='disc image for the end-to-end test (default: the first ../ISO/*.iso)')
    c = b.add_mutually_exclusive_group()
    c.add_argument('--with-converter', action='store_true',
                   help='package the dev build (out/build) as it is, with dxilconv and Metal Shader Converter')
    c.add_argument('--no-rebuild', action='store_true', help='package out/build-release as it is')
    args = ap.parse_args()
    shader_sources = None if args.no_shaders else args.shaders
    warm_scales = None if (args.no_shaders or args.shaders) else [int(x) for x in args.shader_scales.split(',') if x]
    try:
        result = build(args.out, args.save, not args.no_zip, shader_sources, warm_scales, args.seed_source, args.e2e,
                       args.iso, converter=args.with_converter, rebuild=not args.no_rebuild)
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps({'ok': True, 'result': result}, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
