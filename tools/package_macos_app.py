#!/usr/bin/env python3
"""Package an existing macOS runtime as a movable, locally signed app.

No game content, saves, caches or local config are included. --game-data-root
optionally embeds a local first-run hint; the launcher remembers the user's choice
outside the bundle. This tool never writes to the user's Application Support.
"""
import argparse
from pathlib import Path
import platform
import plistlib
import re
import shutil
import sys
import subprocess
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_version  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BACKENDS = {
    'metal': ('librexruntime.dylib', 'libTracyClient.dylib', 'librexgpu-metal.dylib',
              'libdxilconv.dylib', 'libmetalirconverter.dylib'),
    'vulkan': ('librexruntime.dylib', 'libTracyClient.dylib', 'librexgpu-xenos.dylib'),
}
# The shader converter libraries (dxilconv, Apple's Metal Shader Converter) are absent from a FABLE2_NO_CONVERTER
# build (docs/no-converter.md); the plugin then does not link them.
CONVERTER_LIBRARIES = ('libdxilconv.dylib', 'libmetalirconverter.dylib')


def links_converter(runtime):
    plugin = runtime / 'librexgpu-metal.dylib'
    if not plugin.is_file():
        return True
    linked = subprocess.check_output(['otool', '-L', str(plugin)], text=True)
    return any(name in linked for name in CONVERTER_LIBRARIES)


# Third-party licence texts copied into Contents/Resources/Licenses (THIRD_PARTY_NOTICES.md). Paths are relative to
# the dependency trees the build downloads: out/sdk-src/source/thirdparty (ReXGlue runtime and its static libraries)
# and out/metal-build/dependencies (Metal plugin toolchain). Missing files are reported, not fatal.
SDK_THIRDPARTY = ROOT / 'out/sdk-src/source/thirdparty'
METAL_DEPENDENCIES = ROOT / 'out/metal-build/dependencies'
LICENSE_FILES = [
    ('SDL3', SDK_THIRDPARTY / 'sdl3/LICENSE.txt'),
    ('FFmpeg', SDK_THIRDPARTY / 'FFmpeg/LICENSE.md'),
    ('FFmpeg-LGPL-2.1', SDK_THIRDPARTY / 'FFmpeg/COPYING.LGPLv2.1'),
    ('libmspack-LGPL-2.1', SDK_THIRDPARTY / 'libmspack/libmspack/COPYING.LIB'),
    ('fmt', SDK_THIRDPARTY / 'fmt/LICENSE'),
    ('spdlog', SDK_THIRDPARTY / 'spdlog/LICENSE'),
    ('o1heap', SDK_THIRDPARTY / 'o1heap/LICENSE'),
    ('imgui', SDK_THIRDPARTY / 'imgui/LICENSE.txt'),
    ('utfcpp', SDK_THIRDPARTY / 'utfcpp/LICENSE'),
    ('xxHash', SDK_THIRDPARTY / 'xxHash/LICENSE'),
    ('Tracy', SDK_THIRDPARTY / 'tracy/LICENSE'),
    ('SIMDe', SDK_THIRDPARTY / 'simde/COPYING'),
    ('tomlplusplus', SDK_THIRDPARTY / 'tomlplusplus/LICENSE'),
    ('CLI11', SDK_THIRDPARTY / 'cli11/LICENSE'),
    ('inja', SDK_THIRDPARTY / 'inja/LICENSE'),
    ('aes_128', SDK_THIRDPARTY / 'aes_128/LICENSE'),
    ('metal-cpp', METAL_DEPENDENCIES / 'metal-cpp/LICENSE.txt'),
    ('DirectX-Headers', METAL_DEPENDENCIES / 'DirectX-Headers/LICENSE'),
    ('MetalShaderConverter-headers', METAL_DEPENDENCIES / 'metal-shader-converter/include/LICENSE.txt'),
    ('MetalShaderConverter-Acknowledgements', METAL_DEPENDENCIES / 'metal-shader-converter/include/Acknowledgements.rtf'),
]


def notices_text(converter=True):
    """THIRD_PARTY_NOTICES.md as shipped. Without the converter (FABLE2_NO_CONVERTER, the release build) the dxilconv
    and Metal Shader Converter library rows are left out; the MSC runtime headers (Apache-2.0) are still compiled in,
    so they keep a row of their own."""
    text = (ROOT / 'THIRD_PARTY_NOTICES.md').read_text()
    if converter: return text
    lines = []
    for line in text.splitlines(keepends=True):
        if line.startswith('| DirectXShaderCompiler'): continue
        if line.startswith('| Apple Metal Shader Converter'):
            line = ('| Apple Metal Shader Converter 3.1.1 runtime headers (no converter library in this build) | compiled '
                    'into `librexgpu-metal.dylib` | Apache-2.0; acknowledgements in `Acknowledgements.rtf` |\n')
        lines.append(line)
    return ''.join(lines)


def bundle_licenses(resources, backend, converter=True):
    """Licence texts for everything the app redistributes: this project's LICENSE, THIRD_PARTY_NOTICES.md, the
    tracked backends/metal/licenses/ and the third-party files of the downloaded dependency trees."""
    out = resources / 'Licenses'
    out.mkdir()
    shutil.copy2(ROOT / 'LICENSE', out / 'Fable-II-Recomp-LICENSE.txt')
    (out / 'THIRD_PARTY_NOTICES.md').write_text(notices_text(converter))
    for path in sorted((ROOT / 'backends/metal/licenses').iterdir()):
        if not converter and path.name.startswith('DirectXShaderCompiler'):
            continue  # dxilconv is not shipped
        if path.is_file():
            shutil.copy2(path, out / path.name)
    missing = []
    for name, path in LICENSE_FILES:
        if backend != 'metal' and path.is_relative_to(METAL_DEPENDENCIES):
            continue
        if path.is_file():
            shutil.copy2(path, out / (name + (path.suffix if path.suffix in ('.md', '.rtf') else '.txt')))
        else:
            missing.append(str(path.relative_to(ROOT)))
    if missing:
        print('package: licence files not found (not bundled): ' + ', '.join(missing), file=sys.stderr)


def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def localize(binary, libraries):
    # Resolve dependencies beside the loading library rather than in the build
    # machine's SDK. Vulkan's separately loaded MoltenVK lives in Contents/lib.
    output = subprocess.check_output(['otool', '-l', str(binary)], text=True)
    paths = set(re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset', output))
    for path in sorted(paths):
        run('install_name_tool', '-delete_rpath', path, binary)
    run('install_name_tool', '-add_rpath', '@loader_path', binary)
    output = subprocess.check_output(['otool', '-L', str(binary)], text=True)
    for line in output.splitlines():
        if not line.startswith('\t'):
            continue
        dependency = line.strip().split(' (compatibility')[0]
        if dependency.startswith(('/usr/lib/', '/System/Library/')):
            continue
        name = Path(dependency).name
        if name not in libraries:
            raise ValueError(f'Unbundled dependency: {dependency}')
        if dependency != '@rpath/' + name:
            run('install_name_tool', '-change', dependency, '@rpath/' + name, binary)
    if binary.suffix == '.dylib':
        run('install_name_tool', '-id', '@rpath/' + binary.name, binary)
    run('codesign', '--force', '--sign', '-', binary)

def publish(app, output, replace=False):
    """Keep a previous build intact until the new bundle is fully verified."""
    if output.is_symlink():
        raise ValueError('Refusing to replace an app symlink')
    if not output.exists():
        app.rename(output)
        return
    if not replace:
        raise FileExistsError(f'{output} already exists; use --replace to rebuild it')
    info = plistlib.loads((output / 'Contents/Info.plist').read_bytes())
    # Our own builds: org.fable2recomp.metal / .vulkan, or an agent slot's org.fable2recomp.metal.agent-N.
    identifier = info.get('CFBundleIdentifier') or ''
    if not (identifier in {'org.fable2recomp.metal', 'org.fable2recomp.vulkan'}
            or identifier.startswith('org.fable2recomp.metal.agent-')):
        raise ValueError('Refusing to replace an unrelated app')
    previous = app.parent / 'previous.app'
    output.rename(previous)
    try:
        app.rename(output)
    except BaseException:
        previous.rename(output)
        raise
    shutil.rmtree(previous)


def package(runtime, output, game_data_root=None, backend='metal', replace=False, sdk_lib_dir=None, configuration='Release',
            strip_debug=False, bundle_id=None):
    runtime, output = runtime.resolve(), output.absolute()
    if output.suffix != '.app':
        raise ValueError('Output must end in .app')
    if (output.exists() or output.is_symlink()) and not replace:
        raise FileExistsError(f'{output} already exists; use --replace to rebuild it')
    libraries = BACKENDS[backend]
    converter = backend != 'metal' or links_converter(runtime)
    if not converter:
        libraries = tuple(name for name in libraries if name not in CONVERTER_LIBRARIES)
        print('package: no shader converter in this build (FABLE2_NO_CONVERTER); dxilconv and Metal Shader '
              'Converter are not bundled', flush=True)
    if backend == 'vulkan':
        postfix = {'Debug': 'd', 'RelWithDebInfo': 'rd'}.get(configuration, '')
        libraries = tuple(name.replace('.dylib', postfix + '.dylib') for name in libraries)
        if sdk_lib_dir is None:
            raise ValueError('Vulkan packaging requires --sdk-lib-dir for its bundled MoltenVK runtime')
    archs = subprocess.check_output(['lipo', '-archs', str(runtime / 'fable_2')], text=True).split()
    if len(archs) != 1 or archs[0] not in {'arm64', 'x86_64'}:
        raise ValueError('Build one Mac architecture at a time')
    arch = archs[0]
    if backend == 'metal' and arch != 'arm64':
        raise ValueError('The Metal backend requires Apple Silicon')
    for name in ('fable_2', *libraries):
        if not (runtime / name).is_file():
            raise FileNotFoundError(runtime / name)
    if game_data_root and not ((game_data_root / 'default.xex').is_file() and (game_data_root / 'data').is_dir()):
        raise ValueError('Game folder must contain default.xex and data/')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fable-app-', dir=output.parent) as temporary:
        work = Path(temporary)
        app = work / output.name
        macos = app / 'Contents/MacOS'
        resources = app / 'Contents/Resources'
        macos.mkdir(parents=True)
        resources.mkdir()
        minimum_versions = ["15.0"]
        for name in ('fable_2', *libraries):
            dst = macos / name
            shutil.copy2(runtime / name, dst)
            # Match the executable architecture even if a dependency is universal.
            archs = subprocess.check_output(['lipo', '-archs', str(dst)], text=True).split()
            if arch not in archs:
                raise ValueError(f'{name} has no {arch} slice')
            if len(archs) > 1:
                thin = work / name
                run('lipo', dst, '-thin', arch, '-output', thin)
                shutil.copy2(thin, dst)
            if strip_debug:
                # Release: drop the debug map (stabs naming every object file by its absolute build path). Symbol
                # names stay, so crash reports and `sample` still name functions. Release decision D5 (2026-10-14).
                run('strip', '-S', dst)
            if backend == 'vulkan' and arch == 'arm64' and name == 'librexgpu-xenos.dylib':
                run(sys.executable, ROOT / 'tools/macos_terrain_fix.py', dst)
            localize(dst, libraries)
            commands = subprocess.check_output(['otool', '-l', str(dst)], text=True)
            minimum_versions.extend(re.findall(r'\bminos ([0-9.]+)', commands))
        if backend == 'vulkan':
            # SDK detection checks Contents/lib before any system Vulkan SDK.
            lib = app / 'Contents/lib'
            lib.mkdir()
            molten = lib / 'libMoltenVK.dylib'
            shutil.copy2(sdk_lib_dir / molten.name, molten)
            localize(molten, ('libMoltenVK.dylib', 'libMoltenVK.1.dylib'))
            commands = subprocess.check_output(['otool', '-l', str(molten)], text=True)
            minimum_versions.extend(re.findall(r'\bminos ([0-9.]+)', commands))
        run('clang', '-fobjc-arc', '-arch', arch, '-mmacosx-version-min=15.0',
            '-framework', 'AppKit', '-framework', 'Metal', ROOT / 'tools/macos/AppLauncher.m', '-o', macos / 'FableIILauncher')
        run('clang', '-fobjc-arc', '-framework', 'AppKit', ROOT / 'tools/macos/MakeIcon.m', '-o', work / 'make-icon')
        icons = work / 'FableII.iconset'
        icons.mkdir()
        run(work / 'make-icon', icons)
        run('iconutil', '-c', 'icns', icons, '-o', resources / 'FableII.icns')
        for name in ('fable2_config.toml', 'fable2_patches.toml'):
            shutil.copy2(ROOT / 'config' / name, resources / name)
        bundle_licenses(resources, backend, converter)
        if game_data_root:
            (resources / 'LocalDefaults.plist').write_bytes(plistlib.dumps({'GameDataRoot': str(game_data_root.resolve())}))
        version = fable2_version.info()  # VERSION + git commit (tools/fable2_version.py)
        info = {
            'CFBundleExecutable': 'FableIILauncher', 'CFBundleIdentifier': bundle_id or 'org.fable2recomp.' + backend,
            'Fable2Backend': backend,
            'CFBundleName': 'Fable II', 'CFBundleDisplayName': 'Fable II', 'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': version['version'], 'CFBundleVersion': '1',
            'Fable2VersionString': version['string'],
            'CFBundleIconFile': 'FableII', 'LSMinimumSystemVersion': max(minimum_versions, key=lambda v: tuple(map(int, v.split('.')))),
            # No LSMultipleInstancesProhibited: AppLauncher.m's per-state-folder lock is the single-instance
            # rule, so a playtest copy and test sessions can run side by side (local agent docs).
            'LSArchitecturePriority': [arch],
            'NSHighResolutionCapable': True,
        }
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        (app / 'Contents/PkgInfo').write_bytes(b'APPL????')
        run('codesign', '--force', '--sign', '-', macos / 'FableIILauncher')
        run('codesign', '--force', '--sign', '-', app)
        run('codesign', '--verify', '--deep', '--strict', app)
        publish(app, output, replace)
    print(f'Created {output}')
    return output

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=ROOT / 'out/build/mac-arm64-metal-release')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/app/Fable II.app')
    parser.add_argument('--game-data-root', type=Path)
    parser.add_argument('--sdk-lib-dir', type=Path)
    parser.add_argument('--configuration', default='Release')
    parser.add_argument('--backend', choices=BACKENDS, default='metal')
    parser.add_argument('--replace', action='store_true', help='Replace a previous generated app after successful packaging')
    parser.add_argument('--strip-debug', action='store_true',
                        help='Strip debugging symbols (the debug map with absolute build paths) from the bundled binaries')
    parser.add_argument('--bundle-id', help='CFBundleIdentifier (default org.fable2recomp.<backend>; agent slots: '
                        'org.fable2recomp.metal.agent-N, so parallel slot games keep their own macOS Metal shader cache)')
    args = parser.parse_args()
    if args.bundle_id and not (args.bundle_id == 'org.fable2recomp.' + args.backend
                               or args.bundle_id.startswith('org.fable2recomp.metal.agent-')):
        parser.error('--bundle-id must be org.fable2recomp.<backend> or org.fable2recomp.metal.agent-N')
    if platform.system() != 'Darwin':
        parser.error('macOS is required')
    package(args.runtime, args.output, args.game_data_root, args.backend, args.replace, args.sdk_lib_dir, args.configuration,
            args.strip_debug, args.bundle_id)

if __name__ == '__main__':
    main()
