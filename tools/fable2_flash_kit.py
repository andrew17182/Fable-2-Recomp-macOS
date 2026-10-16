#!/usr/bin/env python3
"""Flash kit: "Fable II Flash Kit.zip", finds white terrain flashes on another Mac without anyone watching
(docs/flash-kit.md).

  python3 tools/fable2_flash_kit.py build [--out DIR] [--save NAME] [--no-zip]
                                          [--shader-scales 2 | --shaders CACHE_DIR ... | --no-shaders]
                                          [--seed-source auto|agent-cache|seed-store|prep|warm-up]
                                          [--e2e | --no-e2e] [--iso IMAGE] [--no-rebuild]

Packaged exactly like the benchmark kit (tools/fable2_benchmark_kit.py: the no-converter release build, portable,
bundled shader seed, one save, ad-hoc signed, zip in ignored out/), with its own names and bundle id and
Info.plist Fable2FlashKit = true, which puts the same orchestrator (FableIIBenchmark, tools/macos/Benchmark/main.swift)
in flash mode: one 2x / 60 FPS run per arm and rep (adapt, adapt-nomatch, adapt-tess3, noadapt; 2 reps)
with the flash plan (src/core/fable2_benchmark.h), the tessellation trace and the in-game flash detector
(FABLE2_FLASH_DETECT, backends/metal/port/rex/ui/metal/metal_flash_detect.h). The report goes to
~/Desktop/Fable II Flash Kit/<chip>-<model>-<date>-flash/ (report.txt, report.json, examples/, raw/); the game state
to ~/Library/Application Support/Fable II Recomp Flash Kit (game files cloned from the portable install or the
benchmark kit when present). Makes <DIR>/Fable II Flash Kit/ and the zip (default DIR: out/flash-kit).
Runs under /usr/bin/python3 (3.9).
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_benchmark_kit as kit  # noqa: E402

ROOT = kit.ROOT
DEFAULT_OUT = ROOT / 'out/flash-kit'
KIT = 'Fable II Flash Kit'
APP = 'Fable II Flash Kit.app'
BUNDLE_ID = 'org.fable2recomp.metal.flashkit'
E2E_INPUTS = kit.E2E_INPUTS + ['tools/fable2_flash_kit.py', 'backends/metal/port/rex/ui/metal/metal_flash_detect.h']
E2E_STATE = kit.bench.common.MAIN_ROOT / 'out/flash-kit/e2e-state.json'
# The Bower Lake tomb overlook, on the lake shore (docs/adaptive-terrain-flashes-20261014.md), where the M3 Pro
# showed the flashes; the benchmark kit's camp save did not show them on either Mac.
SAVE = 'hero1-dog-live_2026-10-07_14-10-08'

README = """Fable II Flash Kit {version}

Looks for the white flashes on the terrain while the camera moves, and tells which tessellation setting
causes them. The game detects the flashes itself: you don't need to watch. Needs an Apple Silicon Mac,
macOS {min_os} or later, about 8 GB of free space, and your Fable II disc image (.iso) unless this Mac
already has the portable Fable II or the benchmark kit with its game files.

1. Unzip "Fable II Flash Kit.zip" and open the folder.
2. Double-click "Fable II Flash Kit". macOS blocks it the first time (the app is not notarized):
   click Done, then System Settings > Privacy & Security > "Open Anyway", and confirm.
3. Click Start. Don't use the Mac until it finishes. The game opens and closes by itself 8 times
   (4 settings x 2 runs, 2x resolution, 60 FPS), each time walking and turning on the Bower Lake shore
   below the tomb for ~12 s, about 4-8 minutes in all.
4. At the end, "Show Report" opens Desktop/Fable II Flash Kit/<chip>-<model>-<date>-flash/.
   report.txt is the summary (flash frames per setting), examples/ the worst moments
   (previous frame | flash frame | next frame | detected pixels in red). Send the whole folder.

The settings: adaptive terrain tessellation on (FABLE2_TERRAIN_ADAPTIVE=1) with the layer fix, without it
(FABLE2_TERRAIN_LAYER_MATCH=0, the build that flashed), with the reference tessellator (FABLE2_NATIVE_TESS=3);
adaptive off.

Game state: ~/Library/Application Support/Fable II Recomp Flash Kit (delete it when done, about 7 GB).
It never touches the saves or settings of a normal Fable II Recomp install. The app includes one saved
game (Bower Lake shore) and shaders compiled from the game's own shaders; keep it private.
"""

VARIANT = dict(kit=KIT, app=APP, bundle_id=BUNDLE_ID, name='Fable II Flash Kit',
               plist={'Fable2FlashKit': True}, readme=README, e2e_state=E2E_STATE, e2e_inputs=E2E_INPUTS,
               e2e_run=dict(flash=True), label='flash kit')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='package the no-converter release build as the flash kit (+ zip)')
    b.add_argument('--out', type=Path, default=DEFAULT_OUT, help='output folder (default out/flash-kit)')
    b.add_argument('--save', default=SAVE, help='save-library entry (default %s, Bower Lake shore)' % SAVE)
    b.add_argument('--no-zip', action='store_true')
    b.add_argument('--shader-scales', default='2', metavar='LIST',
                   help='resolution scales to warm the shader seed for (default 2: the kit runs at 2x only)')
    b.add_argument('--shaders', action='append', metavar='CACHE_DIR', help='shader seed from these caches instead')
    b.add_argument('--no-shaders', action='store_true')
    b.add_argument('--seed-source', default='auto', choices=kit.portable.SEED_SOURCES)
    e = b.add_mutually_exclusive_group()
    e.add_argument('--e2e', dest='e2e', action='store_const', const='yes', help='always run the end-to-end test')
    e.add_argument('--no-e2e', dest='e2e', action='store_const', const='no', help='never run it')
    b.set_defaults(e2e='auto')
    b.add_argument('--iso', help='disc image for the end-to-end test (default: the first ../ISO/*.iso)')
    b.add_argument('--no-rebuild', action='store_true', help='package out/build-release as it is')
    args = ap.parse_args()
    shader_sources = None if args.no_shaders else args.shaders
    warm_scales = None if (args.no_shaders or args.shaders) else [int(x) for x in args.shader_scales.split(',') if x]
    try:
        result = kit.build(args.out, args.save, not args.no_zip, shader_sources, warm_scales, args.seed_source,
                           args.e2e, args.iso, converter=False, rebuild=not args.no_rebuild, variant=VARIANT)
    except Exception as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
    print(json.dumps({'ok': True, 'result': result}, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
