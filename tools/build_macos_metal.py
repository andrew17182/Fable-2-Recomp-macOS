#!/usr/bin/env python3
"""Build the Metal runtime and macOS app from pinned dependencies."""
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

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'backends/metal'
WORK = ROOT / 'out/metal-build'
DEPS = WORK / 'dependencies'
# Private CMake downloaded by the setup app (Install.command); used first when present.
PORTABLE_CMAKE = ROOT / 'out/tools/cmake/CMake.app/Contents/bin'
SETUP_HINT = 'Double-click Install.command to set everything up, or see README "Getting started".'

def stage(key, text):
    # Progress marker parsed by the setup app (tools/macos/Setup); harmless on the command line.
    print(f'[fable2-build] stage={key}: {text}', flush=True)

def preflight(need_game):
    """Check game files and tools before downloading anything."""
    problems = []
    if need_game and not ((ROOT / 'default.xex').is_file() and (ROOT / 'data').is_dir()):
        problems.append(f'Game files missing: default.xex and data/ from your Fable II disc must be in {ROOT}')
    for tool in ['cmake', 'clang++', 'curl', 'ditto', 'shasum', 'patch', 'xcodebuild']:
        if not shutil.which(tool):
            problems.append('Missing build tool: ' + tool)
    if shutil.which('cmake'):
        out = subprocess.run(['cmake', '--version'], capture_output=True, text=True).stdout.split()
        version = tuple(int(x) for x in (out[2] if len(out) > 2 else '0').split('-')[0].split('.')[:2] if x.isdigit())
        if version < (3, 25):
            problems.append('CMake 3.25 or later is required (found ' + '.'.join(map(str, version)) + ')')
    if shutil.which('xcodebuild'):
        shown = subprocess.run(['xcodebuild', '-showComponent', 'MetalToolchain', '-json'],
                               capture_output=True, text=True)
        if shown.returncode or '"installed"' not in shown.stdout:
            problems.append('Xcode with the Metal Toolchain is required: install Xcode, then run '
                            '`xcodebuild -downloadComponent MetalToolchain`')
    if problems:
        sys.exit('\n'.join(['Cannot build yet:'] + ['  - ' + p for p in problems] + [SETUP_HINT]))

def run(args, **kw):
    subprocess.run([str(x) for x in args], check=True, **kw)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def unpack(archive, dest):
    # Dependency archives have one top-level directory. Reject escaping members.
    with tarfile.open(archive) as tf:
        for m in tf.getmembers():
            parts = Path(m.name).parts
            if m.name.startswith('/') or '..' in parts:
                raise ValueError('Unsafe archive path: ' + m.name)
        with tempfile.TemporaryDirectory(dir=dest.parent) as tmp:
            try:
                tf.extractall(tmp, filter='data')
            except TypeError:  # Python without extraction filters (system/Xcode 3.9); members checked above
                tf.extractall(tmp)
            entries = list(Path(tmp).iterdir())
            if len(entries) != 1 or not entries[0].is_dir():
                raise ValueError('Expected one archive root')
            shutil.move(entries[0], dest)

# Dependency trees patched in place, and the build trees (relative to WORK) compiled from them.
# compile_metal_helpers.py reads only xenios-source (unpatched), so its output does not depend on these.
# Each entry: (tree, patch or list of patches applied in order).
PATCHED_TREES = [('sdk-source', ['sdk-metal.patch', 'sdk-metal-native-scale.patch', 'sdk-metal-rt-shrink.patch',
                                'sdk-metal-hle-census.patch', 'sdk-metal-gpucmd.patch', 'sdk-metal-ssmask.patch',
                               # same header as the runtime tree (tools/build_sdk_from_source.py)
                               '../../patches/sdk/bit-scan-inline.patch']),
                 ('DirectXShaderCompiler', 'dxilconv-build.patch')]
# FABLE2_NO_CONVERTER=1 (docs/no-converter.md): build without dxilconv and Apple's Metal Shader Converter
# library. The DXC tree is neither downloaded nor built, neither dylib is linked or copied into the app, and the
# plugin compiles stubs that fail loudly if anything still needs the converter. Only the MSC runtime headers
# (Apache-2.0) are used, from the pinned metal-shader-converter tarball.
NO_CONVERTER = os.getenv('FABLE2_NO_CONVERTER', '0') == '1'
CONVERTER_DEPS = {'DirectXShaderCompiler'}
CONVERTER_DYLIBS = ('libdxilconv.dylib', 'libmetalirconverter.dylib')
# --release (docs/portable-build.md, release-cleanup D3 decided 2026-10-14): the build the portable package, the
# benchmark kit and releases ship. Always no-converter, with its own backend tree and runtime folder, so the dev build
# (out/build, converter kept as a fallback) and the release build never reconfigure each other. The game executable
# (out/build/mac-arm64-release, converter-independent) is shared.
RELEASE_RUNTIME = ROOT / 'out/build-release/mac-arm64-metal-release'
RUNTIME = ROOT / 'out/build/mac-arm64-metal-release'

PATCH_DEPENDENTS = {
    'sdk-source': ['backend', 'backend-release'],  # the Metal plugin compiles SDK graphics sources
    'DirectXShaderCompiler': ['dependencies/build-dxilconv', 'backend'],  # backend links libdxilconv
}


def remove_inside(path, parent):
    """Delete path (a directory) only if it lies strictly inside parent."""
    path, parent = Path(os.path.abspath(path)), Path(os.path.abspath(parent))
    if parent not in path.parents:
        raise ValueError('Refusing to delete outside ' + str(parent) + ': ' + str(path))
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def prepare(cache):
    DEPS.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    stage('deps', 'downloading dependencies')
    archives = {}
    for pin in json.loads((SOURCE / 'dependencies.json').read_text()):
        if NO_CONVERTER and pin['name'] in CONVERTER_DEPS:
            continue
        archive = cache / (pin['name'] + '-' + pin['commit'] + '.tar.gz')
        if not archive.exists():
            partial = archive.with_suffix('.part')
            run(['curl', '--fail', '--location', '--show-error', pin['url'], '-o', partial])
            if sha(partial) != pin['sha256']:
                partial.unlink()
                raise ValueError('Download hash mismatch: ' + pin['name'])
            partial.replace(archive)
        if sha(archive) != pin['sha256']:
            raise ValueError('Cache hash mismatch: ' + str(archive))
        dest = DEPS / pin['name']
        archives[pin['name']] = archive
        if not dest.exists():
            unpack(archive, dest)
    stage('patch', 'patching dependencies')
    for tree, patch in PATCHED_TREES:
        if NO_CONVERTER and tree in CONVERTER_DEPS:
            continue
        patches = [patch] if isinstance(patch, str) else list(patch)
        patch = ' + '.join(patches)
        marker = DEPS / tree / '.fable-patch-sha256'
        expected = sha(SOURCE / patches[0])
        if len(patches) > 1:
            # Several patches: one marker over all of them, in order.
            expected = hashlib.sha256(''.join(sha(SOURCE / p) for p in patches).encode()).hexdigest()
        if marker.exists() and marker.read_text() != expected:
            # The tracked patch changed since this tree was patched: start again from the pinned
            # archive, and drop the build trees compiled from the old sources (their objects may be
            # newer than restored files, which keep the archive's timestamps).
            print('[metal-deps] ' + patch + ' changed; re-extracting ' + tree + ' and rebuilding what uses it',
                  flush=True)
            remove_inside(DEPS / tree, DEPS)
            for stale in PATCH_DEPENDENTS[tree]:
                remove_inside(WORK / stale, WORK)
            unpack(archives[tree], DEPS / tree)
        if not marker.exists():
            for p in patches:
                run(['patch', '--batch', '-p1', '-i', SOURCE / p], cwd=DEPS / tree)
            marker.write_text(expected)
    third = DEPS / 'third_party'
    third.mkdir(exist_ok=True)
    for name in ['metal-cpp', 'metal-shader-converter']:
        path = third / name
        if not path.exists():
            path.symlink_to(Path('..') / name, target_is_directory=True)

def shader_cache_key():
    try:
        sys.path.insert(0, str(ROOT / 'tools'))
        import fable2_agent_cache
        return fable2_agent_cache.build_record(ROOT)
    except Exception as error:  # never fail a build over the agent cache
        print('[metal] agent shader cache key not recorded: %r' % (error,), flush=True)
        return None

def sdk_record(sdk):
    stamp = sdk / '.fable-sdk-build.json'
    if stamp.is_file():
        return {'kind': 'source', 'path': sdk.relative_to(ROOT).as_posix() if ROOT in sdk.parents else str(sdk),
                'inputs': json.loads(stamp.read_text()).get('inputs')}
    return {'kind': 'prebuilt', 'version': '0.10.0',
            'path': sdk.relative_to(ROOT).as_posix() if ROOT in sdk.parents else str(sdk)}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cache', type=Path, default=ROOT / 'out/downloads/metal')
    ap.add_argument('--prepare-only', action='store_true')
    ap.add_argument('--backend-only', action='store_true')
    ap.add_argument('--release', action='store_true',
                    help='the no-converter release build (implies FABLE2_NO_CONVERTER=1) in '
                         'out/build-release/mac-arm64-metal-release, with its own backend tree')
    ap.add_argument('--jobs', type=int, default=int(os.getenv('FABLE2_BUILD_JOBS', '4')))
    ap.add_argument('--cmake-args', nargs=argparse.REMAINDER, default=[],
                    help='Remaining arguments are passed to game CMake configuration')
    args = ap.parse_args()
    if args.release:
        global NO_CONVERTER
        NO_CONVERTER = True
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        ap.error('Apple Silicon macOS is required')
    if args.jobs < 1:
        ap.error('--jobs must be positive')
    if (PORTABLE_CMAKE / 'cmake').is_file():
        os.environ['PATH'] = str(PORTABLE_CMAKE) + os.pathsep + os.environ.get('PATH', '')
    preflight(need_game=not (args.prepare_only or args.backend_only))
    # Agent slots (local agent docs): disk guard, and fewer jobs while another checkout is building.
    import fable2_common as common
    try:
        common.ensure_free_space('a build')
    except RuntimeError as error:
        sys.exit(str(error))
    args.jobs, others = common.build_jobs(args.jobs)
    if others:
        print('[build] %d other build(s) running (%s); using %d jobs' % (
            len(others), ', '.join(b['checkout'] for b in others), args.jobs), flush=True)
    os.environ['FABLE2_BUILD_JOBS'] = str(args.jobs)  # tools/build_macos.sh and the SDK build
    with common.build_marker(args.jobs, 'game'):
        build(args)


def build(args):
    prepare(args.cache.resolve())
    if args.prepare_only:
        return
    # ReXGlue SDK: built from the pinned source by default (docs/sdk-from-source.md);
    # FABLE2_SDK_FROM_SOURCE=0 uses the prebuilt v0.10.0 download. An explicit
    # REXGLUE_SDK_ROOT always wins.
    env = dict(os.environ, FABLE2_MACOS_BACKEND='vulkan', FABLE2_MACOS_GAME_ONLY='1')
    if os.getenv('REXGLUE_SDK_ROOT'):
        sdk = Path(os.environ['REXGLUE_SDK_ROOT']).resolve()
    elif os.getenv('FABLE2_SDK_FROM_SOURCE', '1') != '0':
        stage('sdksrc', 'building the ReXGlue SDK from source (first time only)')
        import build_sdk_from_source
        sdk = build_sdk_from_source.ensure(args.cache.resolve(), args.jobs)
        env['REXGLUE_SDK_ROOT'] = str(sdk)
    else:
        sdk = (ROOT / 'out/sdk/mac-arm64').resolve()
    print('ReXGlue SDK:', sdk, flush=True)
    if not args.backend_only:
        # Build the shared game executable without re-entering Metal orchestration.
        run(['bash', ROOT / 'tools/build_macos.sh', *args.cmake_args], env=env)
    if not (sdk / 'bin/rexglue').is_file():
        raise ValueError('Install ReXGlue 0.10.0 via tools/build_macos.sh or set REXGLUE_SDK_ROOT')
    if NO_CONVERTER:
        print('[metal] FABLE2_NO_CONVERTER=1: building without dxilconv and Metal Shader Converter', flush=True)
    else:
        stage('dxc', 'building the shader compiler (slow, first time only)')
        cmd = json.loads((SOURCE / 'dxilconv-configure.json').read_text())
        run([x.replace('{deps}', str(DEPS)) for x in cmd])
        run(['cmake', '--build', DEPS / 'build-dxilconv', '--target', 'dxilconv', '--parallel', args.jobs])
    stage('renderer', 'building the Metal renderer')
    run([sys.executable, ROOT / 'tools/compile_metal_helpers.py', DEPS])
    # The cache pins rexglue_DIR to the SDK it first found; start over when the SDK root changes.
    backend = WORK / ('backend-release' if args.release else 'backend')
    backend_cache = backend / 'CMakeCache.txt'
    if backend_cache.is_file() and ('CMAKE_PREFIX_PATH:UNINITIALIZED=' + str(sdk)) not in backend_cache.read_text().splitlines():
        print('ReXGlue SDK root changed to', sdk, '- reconfiguring the Metal backend', flush=True)
        backend_cache.unlink()
    cpu_flags = '-march=armv8-a' if os.getenv('FABLE2_CPU_TARGET') == 'generic' else '-mcpu=apple-m1'
    run(['cmake', '-S', SOURCE, '-B', backend, '-G', 'Unix Makefiles', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_PREFIX_PATH=' + str(sdk), '-DFABLE2_METAL_DEPS=' + str(DEPS),
         '-DCMAKE_C_FLAGS=' + cpu_flags, '-DCMAKE_CXX_FLAGS=' + cpu_flags,
         '-DFABLE2_NO_CONVERTER=' + ('ON' if NO_CONVERTER else 'OFF')])
    run(['cmake', '--build', backend, '--parallel', args.jobs])
    if args.backend_only:
        return
    stage('package', 'packaging Fable II.app')
    runtime = RELEASE_RUNTIME if args.release else RUNTIME
    runtime.mkdir(parents=True, exist_ok=True)
    game = ROOT / 'out/build/mac-arm64-release'
    sources = [game / 'fable_2', backend / 'librexgpu-metal.dylib']
    if NO_CONVERTER:
        for name in CONVERTER_DYLIBS:  # left over from a converter build
            if (runtime / name).exists():
                (runtime / name).unlink()
    else:
        sources += [DEPS / 'build-dxilconv/lib/libdxilconv.dylib',
                    DEPS / 'metal-shader-converter/lib/libmetalirconverter.dylib']
    sources.extend(p for p in game.glob('*.dylib') if p.name != 'librexgpu-xenos.dylib')
    for config in ['fable2_config.toml', 'fable2_patches.toml']:
        if not (runtime / config).exists():
            shutil.copy2(ROOT / 'config' / config, runtime / config)
    shutil.copytree(ROOT / 'src/lua', runtime / 'data/scripts/recomp', dirs_exist_ok=True)
    for src in sources:
        dst = runtime / src.name
        temporary = dst.with_suffix(dst.suffix + '.next')
        shutil.copy2(src, temporary)
        os.replace(temporary, dst)
    (runtime / 'metal-build.json').write_text(json.dumps({
        'dependencies': json.loads((SOURCE / 'dependencies.json').read_text()),
        'rexglue_sdk': sdk_record(sdk),
        'shader_converter': not NO_CONVERTER,
        'release': bool(args.release),
        'runtime_sha256': {p.name: sha(p) for p in sources},
        # Shader-generation fingerprint of the sources built: the agent shader cache key (tools/fable2_agent_cache.py).
        'shader_cache_key': shader_cache_key(),
    }, indent=2) + '\n')
    print('Built:', runtime, '\nLaunch run_macos_metal.command. Saves are not copied or merged.')
    # Agent slots: a bundle id per slot. macOS keeps one Metal shader cache per bundle id, and a game's compiles are
    # not persisted while another game with the same id runs (docs/bug-patterns.md row 102), so parallel slot games
    # would keep recompiling. The main checkout keeps org.fable2recomp.metal.
    sys.path.insert(0, str(ROOT / 'tools'))
    import fable2_common as common
    slot_id = ['--bundle-id', 'org.fable2recomp.metal.' + common.SLOT] if common.IS_SLOT and common.SLOT else []
    run([sys.executable, ROOT / 'tools/package_macos_app.py',
         '--runtime', runtime, '--output', runtime / 'Fable II.app',
         '--game-data-root', ROOT, '--backend', 'metal', '--replace']
        + (['--strip-debug'] if args.release else []) + slot_id)

if __name__ == '__main__':
    main()
