#!/usr/bin/env python3
"""Release package: one "Fable II.app" that builds the game on the player's Mac (docs/release-app.md).

  python3 tools/fable2_release.py build [--out DIR] [--no-rebuild] [--no-zip] [--signing sealed|runtime]
  python3 tools/fable2_release.py check APP_OR_ZIP          run the package checks on an existing app or zip

Writes <DIR>/Fable II.app, <DIR>/Fable II.zip and <DIR>/manifest.json (default DIR: out/release).

The app holds no game code and nothing derived from the game:
  Contents/MacOS/        FableIIPlayer (main executable: welcome, first-launch build, Play, Settings), FableIILauncher
                         (AppLauncher.m: starts the built game), FableIIPrepare (disc-image extractor)
  Contents/Resources/kit/
    sdk/                 the prebuilt ReXGlue SDK prefix of the release build (headers, static libraries, cmake
                         package files, the rexglue recompiler); the Vulkan plugin, glslang and the Vulkan registry
                         are left out
    game-src/            our sources and build files (CMakeLists.txt, src/, config/, tests the build runs, manifests,
                         build helpers, tools/fable2_game_build.py)
    runtime/             librexruntime.dylib, librexgpu-metal.dylib (no-converter), libTracyClient.dylib
    game-bundle/         Info.plist, PkgInfo and icon of the game bundle the player's Mac assembles
    kit.json             version, kit_id (hash of every kit file), expected default.xex hash, CMake pin, file hashes
  Contents/Resources/Licenses/   LICENSE, THIRD_PARTY_NOTICES.md and every bundled licence text
  Contents/Resources/release-manifest.json   every file of the app with its size and SHA-256

The no-converter release build is refreshed first (tools/build_macos_metal.py --release; docs/no-converter.md).
The package is refused if it contains game content or anything game-derived (fable_2, generated code, shader caches
or seeds, data/, .xex/.iso/.bik), a shader-converter library, a local path or a personal identifier
(fable2_portable.check_package, plus the release checks here). Runs under /usr/bin/python3 (3.9).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_portable as portable  # noqa: E402
import fable2_version  # noqa: E402
import package_macos_app  # noqa: E402
import fable2_game_build as game_build  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / 'out/release'
APP_NAME = 'Fable II.app'
ZIP_NAME = 'Fable II.zip'
BUNDLE_ID = 'org.fable2recomp.metal.release'
# The release app's own folder in ~/Library/Application Support: never the dev app's "Fable II Recomp" (saves,
# caches and launcher.plist of development and playtest builds live there).
STATE_FOLDER = 'Fable II for Mac'
RUNTIME_LIBRARIES = ('librexruntime.dylib', 'librexgpu-metal.dylib', 'libTracyClient.dylib')
# SDK prefix parts not needed to build the game: the Vulkan plugin and glslang (Vulkan path only, GPL-3 bison
# exception in glslang), the Vulkan XML registry, pkg-config files.
SDK_EXCLUDE = re.compile(r'^(\.fable-sdk-build\.json|lib/librexgpu-xenos\.dylib|lib/lib(glslang|SPIRV|GenericCodeGen|MachineIndependent|'
                         r'OGLCompiler|OSDependent)\.a|lib/pkgconfig/.*|share/vulkan/.*)$')
# Our build inputs (tracked files only): what tools/fable2_game_build.py compiles on the player's Mac.
GAME_SRC_FILES = ['CMakeLists.txt', 'CMakePresets.json', 'VERSION.txt', 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md',
                  'fable_2_manifest.toml', 'fable2_codegen.toml', 'fable2_switch_tables.toml',
                  'tools/apply_recomp_patches.py', 'tools/apply_startup_handoff.py', 'tools/fable2_version.py',
                  'tools/ensure_recomp_manifest.cmake', 'tools/fable2-functrace.cmd', 'tools/fable2_game_build.py']
GAME_SRC_TREES = ['src', 'config/fable2_config.toml', 'config/fable2_patches.toml', 'tests', 'thirdparty/tomlplusplus/include']
GAME_SRC_TREE_SUFFIXES = {'tests': ('.cpp', '.h', '.hpp')}
# Game-derived or build-output names that must never be in the package (beyond fable2_portable.GAME_CONTENT).
# Upstream headers (SDK and its third-party libraries): their author credits are not personal identifiers of ours.
THIRD_PARTY = ('Contents/Resources/kit/sdk/include/',)
FORBIDDEN = re.compile(r'(^|/)(fable_2|generated|native-shaders|cache|saves|GameData)(/|$)|'
                       r'\.(xex|iso|bik|metallib|binarchive|xsh|xpso|artifacts|gch|pch|o)$', re.I)


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def sha256(path):
    return game_build.sha256_file(path)


def tracked(paths):
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z', '--', *paths], capture_output=True, check=True).stdout
    return [p for p in out.decode().split('\0') if p]


def game_src_files():
    files = list(GAME_SRC_FILES)
    for tree in GAME_SRC_TREES:
        suffixes = GAME_SRC_TREE_SUFFIXES.get(tree)
        files += [p for p in tracked([tree]) if not suffixes or p.endswith(suffixes)]
    missing = [p for p in files if not (ROOT / p).is_file()]
    if missing: raise RuntimeError('game-src files missing: ' + ', '.join(missing))
    return sorted(set(files))


def sdk_root():
    record = json.loads((portable.RELEASE_RUNTIME / 'metal-build.json').read_text())
    sdk = record.get('rexglue_sdk', {})
    path = Path(sdk.get('path', 'out/sdk-src/mac-arm64'))
    return path if path.is_absolute() else ROOT / path


def copy_sdk(source, dest):
    """The SDK prefix without SDK_EXCLUDE; rexglueTargets-release.cmake loses the import checks of what was left
    out (those targets are never linked by the game)."""
    files = {}
    for path in sorted(source.rglob('*')):
        if path.is_dir() or path.name == '.DS_Store': continue
        relative = path.relative_to(source).as_posix()
        if SDK_EXCLUDE.match(relative): continue
        if path.is_symlink(): raise RuntimeError('symlink in the SDK prefix: ' + relative)
        target = dest / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    excluded = [p.relative_to(source).as_posix() for p in source.rglob('*')
                if p.is_file() and SDK_EXCLUDE.match(p.relative_to(source).as_posix())]
    targets = dest / 'lib/cmake/rexglue/rexglueTargets-release.cmake'
    text = targets.read_text()
    kept = []
    for line in text.splitlines(keepends=True):
        if line.startswith('list(APPEND _cmake_import_check_files_for_') and \
                any('"${_IMPORT_PREFIX}/%s"' % e in line for e in excluded):
            continue
        kept.append(line)
    targets.write_text(''.join(kept))
    for path in sorted(dest.rglob('*')):
        if path.is_file(): files[path.relative_to(dest).as_posix()] = sha256(path)
    return files, excluded


def write_kit(kit, app, version):
    """Contents/Resources/kit (see the module docstring)."""
    kit.mkdir()
    sdk_files, excluded = copy_sdk(sdk_root(), kit / 'sdk')
    src_files = {}
    for relative in game_src_files():
        target = kit / 'game-src' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, target)
    # Version stamp for a tree without git (tools/fable2_version.py reads it).
    (kit / 'game-src/.fable2-version.json').write_text(json.dumps(version) + '\n')
    for path in sorted((kit / 'game-src').rglob('*')):
        if path.is_file(): src_files[path.relative_to(kit / 'game-src').as_posix()] = sha256(path)
    runtime = kit / 'runtime'
    runtime.mkdir()
    macos = app / 'Contents/MacOS'
    for name in RUNTIME_LIBRARIES:
        os.replace(macos / name, runtime / name)
    (macos / 'fable_2').unlink()
    # The game bundle's Info.plist: the app's own (same bundle id and name: the Dock and activation treat the game
    # as Fable II), with fable_2 as its executable.
    bundle = kit / 'game-bundle'
    (bundle / 'Resources').mkdir(parents=True)
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    info.update(CFBundleExecutable='fable_2', Fable2GameBundle=True)
    (bundle / 'Info.plist').write_bytes(plistlib.dumps(info))
    shutil.copy2(app / 'Contents/PkgInfo', bundle / 'PkgInfo')
    shutil.copy2(app / 'Contents/Resources/FableII.icns', bundle / 'Resources/FableII.icns')
    hashes = dict(sdk=sdk_files, **{'game-src': src_files},
                  runtime={n: sha256(runtime / n) for n in RUNTIME_LIBRARIES},
                  **{'game-bundle': {p.relative_to(bundle).as_posix(): sha256(p) for p in sorted(bundle.rglob('*'))
                                     if p.is_file()}})
    kit_id = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()[:16]
    record = dict(format=1, version=version['string'], git_sha=version['git_sha'], kit_id=kit_id,
                  xex_sha256=portable.expected_xex_sha256(), runtime=list(RUNTIME_LIBRARIES), cmake=game_build.CMAKE_PIN,
                  sdk_excluded=excluded, built=time.strftime('%Y-%m-%d %H:%M'), files=hashes)
    (kit / 'kit.json').write_text(json.dumps(record, indent=1, sort_keys=True) + '\n')
    return record


# How the release app is signed (--signing; docs/release-app.md "Gatekeeper"). Both are ad-hoc (no Developer ID),
# nested code first: the kit's runtime libraries, then the helpers, then the bundle (Contents/_CodeSignature).
#   sealed    the shipped default. Downloaded and unzipped, macOS shows "Not Opened"; Open Anyway in System Settings
#             then runs it.
#   runtime   as sealed, with the hardened runtime (`--options runtime`) on the executables and the bundle. Same
#             Gatekeeper result; kept for testing. The executables load only system libraries and spawn/exec
#             separately signed programs, so no entitlement is needed (RUNTIME_ENTITLEMENTS).
# Leaving the bundle unsealed makes macOS call the download "damaged"; do not ship it.
SIGNING_MODES = ('sealed', 'runtime')
# Entitlements for --signing runtime. None so far: the executables load only system libraries (otool -L). Kept as a
# hook in case a hardened-runtime run check needs one.
RUNTIME_ENTITLEMENTS = {}


def sign_app(app, work, mode='sealed'):
    """Sign the app as --signing MODE says (SIGNING_MODES), nested code first, then verify it."""
    if mode not in SIGNING_MODES: raise ValueError('unknown signing mode: %s' % mode)
    macos, resources = app / 'Contents/MacOS', app / 'Contents/Resources'
    # The kit's runtime libraries are copied into the game bundle and re-signed on the player's Mac; they are signed
    # plainly in both modes (the hardened runtime applies to executables only).
    for library in sorted((resources / 'kit/runtime').iterdir()):
        run('codesign', '--force', '--sign', '-', library, stderr=subprocess.DEVNULL)
    options = []
    if mode == 'runtime':
        options = ['--options', 'runtime']
        if RUNTIME_ENTITLEMENTS:
            entitlements = work / 'entitlements.plist'
            entitlements.write_bytes(plistlib.dumps(RUNTIME_ENTITLEMENTS))
            options += ['--entitlements', entitlements]
    main = plistlib.loads((app / 'Contents/Info.plist').read_bytes())['CFBundleExecutable']
    for helper in sorted(macos.iterdir()):
        if helper.name != main: run('codesign', '--force', '--sign', '-', *options, helper, stderr=subprocess.DEVNULL)
    # The bundle last: this signs the main executable and seals everything else.
    run('codesign', '--force', '--sign', '-', *options, app, stderr=subprocess.DEVNULL)
    run('codesign', '--verify', '--deep', '--strict', app)


def check_release(app):
    """Release-only checks on top of fable2_portable.check_package."""
    problems = []
    for path in app.rglob('*'):
        relative = path.relative_to(app).as_posix()
        inside = relative.split('Contents/Resources/kit/', 1)[-1]
        if FORBIDDEN.search(inside) and not relative.startswith('Contents/Resources/kit/game-src/src/'):
            problems.append('game-derived or build output in package: ' + relative)
        if path.name in package_macos_app.CONVERTER_LIBRARIES: problems.append('shader converter: ' + relative)
    kit = app / 'Contents/Resources/kit'
    runtime = kit / 'runtime'
    for library in sorted(runtime.iterdir()) if runtime.is_dir() else []:
        listing = subprocess.check_output(['otool', '-L', str(library)], text=True).splitlines()[1:]
        for line in listing:
            dependency = line.strip().split(' (compatibility')[0]
            if dependency.startswith(('/usr/lib/', '/System/Library/')): continue
            if not dependency.startswith('@rpath/') or not (runtime / Path(dependency).name).is_file():
                problems.append('%s: unbundled dependency %s' % (library.name, dependency))
    try:
        record = json.loads((kit / 'kit.json').read_text())
        if not re.fullmatch(r'[0-9a-f]{64}', record.get('xex_sha256', '')): problems.append('kit.json: no xex hash')
    except (OSError, ValueError) as error:
        problems.append('kit.json unreadable: %s' % error)
    if problems: raise RuntimeError('Release check failed:\n  ' + '\n  '.join(sorted(set(problems))))


def manifest(app):
    files = {}
    for path in sorted(app.rglob('*')):
        if path.is_file() and not path.is_symlink():
            files[path.relative_to(app.parent).as_posix()] = dict(bytes=path.stat().st_size, sha256=sha256(path))
    return files


def write_shader_recipe(resources):
    """Resources/shader-recipe.json from the newest remembered lists (docs/offline-shader-seed.md §3): per shader its
    hash, location in the disc's shader bank, the patched vertex-fetch dwords and a SHA-256, plus the pipeline list.
    No microcode: the player's Mac rebuilds the lists from its own disc. Derived metadata: shipped only with
    --shader-recipe (release decision, docs/offline-shader-seed.md §6)."""
    import fable2_shader_bank as bank
    import fable2_shader_prep as prep
    cache, where = prep.newest_lists()
    if cache is None: raise RuntimeError('no remembered shader lists for --shader-recipe')
    found = prep.lists_in(cache) or prep.lists_in_seed(cache)
    recipe = bank.recipe(found[0], found[1], root=None, log=lambda *a: None)
    recipe['source_xsh'] = where  # not a local path (package check)
    (resources / 'shader-recipe.json').write_text(json.dumps(recipe, separators=(',', ':')) + '\n')
    return dict(source=where, shaders=len(recipe['shaders']), unresolved=len(recipe['unresolved']),
                bytes=(resources / 'shader-recipe.json').stat().st_size)


def build(out_dir, rebuild=True, zip_it=True, shader_recipe=False, signing='sealed'):
    started = time.monotonic()
    if rebuild: portable.ensure_release_build()
    portable.RUNTIME = portable.RELEASE_RUNTIME
    record = json.loads((portable.RELEASE_RUNTIME / 'metal-build.json').read_text())
    if record.get('shader_converter', True): raise RuntimeError('out/build-release is not a no-converter build')
    out_dir = out_dir.absolute()
    out_dir.mkdir(parents=True, exist_ok=True)
    version = fable2_version.info()
    with tempfile.TemporaryDirectory(prefix='.release-', dir=out_dir) as temporary:
        work = Path(temporary)
        log = open(work / 'package.log', 'w')
        saved = os.dup(1), os.dup(2)
        os.dup2(log.fileno(), 1); os.dup2(log.fileno(), 2)
        try:
            kwargs = {}
            import inspect
            if 'strip_debug' in inspect.signature(package_macos_app.package).parameters: kwargs['strip_debug'] = True
            app = package_macos_app.package(portable.RELEASE_RUNTIME, work / APP_NAME, None, 'metal', **kwargs)
        except Exception:
            os.dup2(saved[0], 1); os.dup2(saved[1], 2)
            sys.stderr.write((work / 'package.log').read_text()[-4000:])
            raise
        finally:
            sys.stdout.flush(); os.dup2(saved[0], 1); os.dup2(saved[1], 2); log.close()
        macos, resources = app / 'Contents/MacOS', app / 'Contents/Resources'
        import fable2_common as common
        with common.build_marker(1, 'release helpers'):
            run('xcrun', 'swiftc', '-O', '-swift-version', '5', '-parse-as-library', '-target', 'arm64-apple-macos15.0',
                '-o', macos / 'FableIIPrepare', *[ROOT / s for s in portable.PREPARE_SOURCES])
            run('xcrun', 'swiftc', '-O', '-swift-version', '5', '-target', 'arm64-apple-macos15.0',
                '-o', macos / portable.PLAYER_EXECUTABLE, *[ROOT / s for s in portable.PLAYER_SOURCES])
        recipe_info = write_shader_recipe(resources) if shader_recipe else None
        (resources / 'recomp-lua').mkdir()
        for lua in sorted((ROOT / 'src/lua').glob('*.lua')): shutil.copy2(lua, resources / 'recomp-lua' / lua.name)
        plist_path = app / 'Contents/Info.plist'
        info = plistlib.loads(plist_path.read_bytes())
        info.update(CFBundleExecutable=portable.PLAYER_EXECUTABLE, CFBundleIdentifier=BUNDLE_ID, Fable2Portable=True,
                    Fable2Release=True, Fable2BuildsGame=True, Fable2StateFolder=STATE_FOLDER,
                    Fable2XexSHA256=portable.expected_xex_sha256(), Fable2XexMismatchBlocks=True,
                    Fable2MSCMinTarget=portable.MSC_PIN, Fable2SkipIntroDefault=False,
                    Fable2PortableBuilt=time.strftime('%Y-%m-%d %H:%M'))
        info.pop('LSArchitecturePriority', None)
        plist_path.write_bytes(plistlib.dumps(info))
        kit_record = write_kit(resources / 'kit', app, version)
        info['Fable2KitID'] = kit_record['kit_id']
        plist_path.write_bytes(plistlib.dumps(info))
        # Every file but itself; written before signing so the seal covers it.
        (resources / 'release-manifest.json').write_text(json.dumps(manifest(app), indent=1, sort_keys=True) + '\n')
        sign_app(app, work, signing)
        portable.check_package(app, third_party=THIRD_PARTY)
        check_release(app)
        final = out_dir / APP_NAME
        if final.exists(): shutil.rmtree(final)
        app.rename(final)
    zip_path = out_dir / ZIP_NAME
    if zip_it:
        if zip_path.exists(): zip_path.unlink()
        zip_app(final, zip_path)
    files = manifest(final)
    result = dict(app=str(final), zip=str(zip_path) if zip_it else None,
                  app_bytes=sum(f['bytes'] for f in files.values()),
                  zip_bytes=zip_path.stat().st_size if zip_it else None,
                  zip_sha256=sha256(zip_path) if zip_it else None, version=version['string'],
                  kit_id=kit_record['kit_id'], xex_sha256=kit_record['xex_sha256'], files=len(files),
                  shader_recipe=recipe_info, signing=signing, seconds=round(time.monotonic() - started, 1))
    (out_dir / 'manifest.json').write_text(json.dumps(dict(result, contents=files), indent=1) + '\n')
    return result


def zip_app(app, zip_path):
    """The release zip (fable2_portable.zip_bundle: no __MACOSX/._* entries, which made macOS call it "damaged")."""
    portable.zip_bundle(app, zip_path)


def check(target):
    target = Path(target)
    if target.suffix == '.zip':
        portable.check_zip(target)
        with tempfile.TemporaryDirectory() as temporary:
            run('ditto', '-x', '-k', target, temporary)
            app = Path(temporary) / APP_NAME
            portable.check_package(app, third_party=THIRD_PARTY)
            check_release(app)
    else:
        portable.check_package(target, third_party=THIRD_PARTY)
        check_release(target)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--out', type=Path, default=DEFAULT_OUT)
    b.add_argument('--no-rebuild', action='store_true', help='package out/build-release as it is')
    b.add_argument('--no-zip', action='store_true')
    b.add_argument('--shader-recipe', action='store_true',
                   help='ship the disc shader recipe (hashes, bank locations, patched fetch dwords, pipeline list; no '
                        'microcode) so the first launch prepares shaders; derived metadata, off by default')
    b.add_argument('--signing', choices=SIGNING_MODES, default='sealed',
                   help='sealed (default): ad-hoc, bundle sealed; runtime: the same with the hardened runtime '
                        '(docs/release-app.md "Gatekeeper")')
    c = sub.add_parser('check')
    c.add_argument('target')
    args = ap.parse_args()
    try:
        if args.cmd == 'check':
            check(args.target)
            print(json.dumps({'ok': True}))
            return 0
        result = build(args.out, not args.no_rebuild, not args.no_zip, args.shader_recipe, args.signing)
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps({'ok': True, 'result': result}, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
