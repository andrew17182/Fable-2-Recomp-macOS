# Release app: one download, the game built on the player's Mac (2026-10-14)

The public release is a source repository plus one downloadable **`Fable II.app`** (`Fable II.zip`). The app holds
only non-game parts. On first launch it extracts the player's own disc, then translates and compiles the game code on
that Mac. Plan and background: the 2026-10-12 split-build release plan. Decisions: converter-free build only
(`docs/no-converter.md`), ad-hoc signed (no Developer ID, no notarization), players need only Apple's Command Line
Tools, never Xcode.

| Part | File |
|---|---|
| Packager | `tools/fable2_release.py build` → `out/release/Fable II.app`, `Fable II.zip`, `manifest.json` |
| Build driver (runs on the player's Mac, `/usr/bin/python3` 3.9, standard library only) | `tools/fable2_game_build.py` |
| First-launch windows (welcome, extraction, build, shader prep, Play) | `tools/macos/Player/main.swift` |
| Start Fresh / Restore / Delete archive | `tools/macos/Shared/StartFresh.swift` |
| Engine launcher (finds the built game) | `tools/macos/AppLauncher.m` |

## What is in the app

```
Fable II.app/Contents/
  MacOS/        FableIIPlayer (main executable), FableIILauncher, FableIIPrepare
  Resources/kit/
    sdk/        prebuilt ReXGlue SDK prefix: headers, static libraries, CMake package files, the rexglue recompiler
                (the Vulkan plugin, glslang and the Vulkan registry are left out; their import checks are removed
                from rexglueTargets-release.cmake)
    game-src/   CMakeLists.txt, CMakePresets.json, src/, config/, tests/*.cpp the build runs, the manifests,
                thirdparty/tomlplusplus, the build helpers and tools/fable2_game_build.py
    runtime/    librexruntime.dylib, librexgpu-metal.dylib (no converter), libTracyClient.dylib
    game-bundle/  Info.plist, PkgInfo and icon for the game bundle assembled on the player's Mac
    kit.json    version, kit_id (hash of every kit file), expected default.xex SHA-256, CMake pin, file hashes
  Resources/Licenses/            LICENSE, THIRD_PARTY_NOTICES.md and every bundled licence text
  Resources/release-manifest.json  every file of the app with size and SHA-256
```

Nothing game-derived is in the app or the zip: no `fable_2`, no generated code, no shader seed or cache, no
`data/`. `fable2_release.py` refuses a package that has any of these, a shader-converter library, a local path or a
personal identifier (`fable2_portable.check_package`, which also scans binaries, plus `check_release`). Run the
checks on any app or zip with `python3 tools/fable2_release.py check "out/release/Fable II.zip"`.

Info.plist: bundle id `org.fable2recomp.metal.release`, `Fable2BuildsGame`, `Fable2KitID`, `Fable2StateFolder`
(`Fable II for Mac`), `Fable2XexSHA256`, `Fable2XexMismatchBlocks`.

## Shaders on the first launch (`--shader-recipe`: not shipped)

Decided 2026-10-14 (maintainer): the release does **not** ship the recipe. The first play translates and compiles
shaders as they are needed (a few hitches once, measured below); the game remembers them, and later launches and new
versions prepare them ahead (the headless prep runs from the lists this Mac remembered). The option stays for
private test packages. With
`python3 tools/fable2_release.py build --shader-recipe` the app carries `Resources/shader-recipe.json` (149 KB): per
shader its hash, the location of its microcode in the disc's shader bank (`data/Shaders/ShadersRelease.sbk`), the
1,099 vertex-fetch dwords D3D patches at bind time and a SHA-256, plus the pipeline list (`.xpso`: shader hashes,
render-target formats, blend state). It holds no microcode. After the build, `FableIIPrepare --shader-lists` rebuilds
the shader list from the player's own disc in 0.03 s (220 shaders; 2 D3D-internal vertex shaders are not in the bank),
then the headless prep (`FABLE2_SHADER_PREP`, `docs/offline-shader-seed.md`) runs at the scale the Play window will
use (Auto's pick) and 1×.

This metadata is derived from the game (hashes, offsets, render state and the patched instruction words), so it is
not shipped (`docs/offline-shader-seed.md` §6).

Measured (M5 Max, packaged app, fresh test state, the app's macOS Metal cache moved aside first so both arms are cold,
Old Tomb save, headings8 + a 30 s camera turn, hitches counted for the first 60 s after the game was ready;
`out/validation/release-shaders/`, KEEP):

| Arm | Extra first-launch time | Game ready after start | Hitches in the first 60 s | Of which |
|---|---|---|---|---|
| No recipe (default) | 0 | 16.4 s | **5, 1,321 ms** (worst 410 ms) | xlate 791 ms, pso_emu 287 ms |
| `--shader-recipe` | lists 0.03 s + prep 1× 10.0 s (459 pipelines, 288 native shaders) | 11.1 s | **0** | — |

## Where it keeps its files

`~/Library/Application Support/Fable II for Mac/` (its own folder: development and playtest builds use
`Fable II Recomp`, which the release app never reads or writes, except to offer a finished extraction found there as
an APFS clone, as the portable app does):

| Folder | What | Size |
|---|---|---|
| `GameData/` | files extracted from the disc | ~7 GB |
| `Build/` | kit copy (`sdk/`, `src/`), generated code, objects, our CMake if downloaded, `build-state.json` | ~0.75 GB (+65 MB CMake) |
| `Game/Fable II.bundle` | the built game with the runtime libraries, ad-hoc signed on this Mac | ~130 MB |
| `saves/`, `cache/`, `logs/` (`build.log`, `prepare.log`, `fable_2.log`) | as in the portable app | — |
| `Archive/` | Start Fresh archives | — |

Disk needed on first launch: about 8 GB (7 GB game files + 0.75 GB build + game bundle).

The game bundle is named `.bundle`, not `.app`, so Spotlight and Launchpad do not list it as a second app (it has the app's Info.plist and bundle id, so the running game is Fable II in the Dock). The bundle is
built and signed locally, so it carries no quarantine attribute and Gatekeeper does not assess it.

## First launch

1. **Welcome**: choose the disc image (or reuse a finished extraction of another Fable II app on this Mac).
2. **Extract** with `FableIIPrepare` (progress, Cancel, Show details). `default.xex` is hashed first; a different
   version **blocks** in the release app (the code translation is specific to that exact file).
3. **Command Line Tools**: `xcode-select -p` must name a developer folder with `clang`. If not, the window explains
   why and **Install Command Line Tools…** runs `xcode-select --install` (Apple's own dialog). The window then checks
   every 2 s for that readiness signal and continues by itself.
4. **Build** (`/usr/bin/python3 -I -B -u …/kit/game-src/tools/fable2_game_build.py build --kit … --state … --game-data …`),
   shown as a checklist with one progress bar, an estimate, Cancel and "Show details" (every real command echoed as
   `$ …` with its output, also in `logs/build.log`):
   - **CMake**: our own copy, then PATH, Homebrew and `/Applications/CMake.app` (3.25+); otherwise Kitware's
     official 4.1.2 macOS archive is downloaded into `Build/tools/cmake` (pinned URL and SHA-256, no admin rights).
   - **Copy the build kit** into `Build/` (only changed files; see Updates).
   - **Translate the game code**: `rexglue init` (first time), configure, `rexglue codegen`, configure again, recomp
     patches.
   - **Compile**: `fable_2_register.cpp` alone first, then everything with `jobs = clamp(min(cores,
     floor((RAM − 3.5 GB) / 0.75 GB)), 1, cores)`. A memory watch polls `kern.memorystatus_vm_pressure_level` and
     swap once a second; critical pressure or more than 1 GB of swap growth stops make and resumes with half the jobs.
   - **Link**, then **Sign on this Mac**: `Game/Fable II.bundle` is assembled beside the old one (fable_2 + runtime
     libraries + the app's Info.plist with fable_2 as executable, same bundle id), `codesign --sign -` each binary and
     the bundle, `codesign --verify --strict`, then swapped in.
   - Errors show a plain sentence, **Copy Details** (error, Mac, app version, last 200 log lines), **Show Log
     File** and **Try Again** (continues where it stopped: the build tree is kept).
5. **Preparing shaders** (as in the portable app: one headless run per scale, Skip allowed).
6. **Play window**.

The build environment is cleaned: no `FABLE2_*`, `REXGLUE_*` or `CMAKE_*` variables from the caller, PATH is
CMake + `/usr/bin:/bin:/usr/sbin:/sbin`. The CMake option `FABLE2_GPU_PLUGIN_XENOS=OFF` keeps the Xenos plugin
(not in the kit) from being staged. CLI: `fable2_game_build.py plan` (job count and why), `status`,
`measure-report STATE` (for `--measure` builds).

## Updates

`Build/build-state.json` records the kit_id it was built from, the compiler identity and the rexglue hash. A new app
version has a new kit_id, so the next launch rebuilds in the same tree:

- the kit sync copies only files whose content changed and gives them the current time, so make recompiles only
  what depends on them (an SDK header change recompiles everything through the PCH; a `src/` change only its units);
- a changed `rexglue` forces codegen (its depfile does not list the recompiler);
- a changed `clang --version` / SDK removes the build folder (stale PCH);
- the game files and saves are reused as they are.

Players replace the app (drag the new one over the old). Gatekeeper's "Open Anyway" is needed again for a new
download.

## Start Fresh

Settings → Advanced → **Start Fresh…** (also File menu) moves everything the app owns into
`Archive/<yyyy-MM-dd_HH-mm-ss>/` and relaunches into the welcome window, so a first launch can be tested again
without a new macOS user. The sheet lists exactly what moves:

- every entry of the state folder except `Archive/` and `app.lock`: game files, build tree and generated code, the
  built game, logs, shader caches, settings and launcher plists, configs;
- the saves only with **Keep my saves** off (default on: they stay in place);
- the app's UserDefaults domain (`defaults export` into the archive, then removed), its saved window state, its
  `~/Library/Caches/<id>` folder and its per-user Metal cache (`DARWIN_USER_CACHE_DIR/<id>`), only for a bundle id
  under `org.fable2recomp.`.

It does not reset the Command Line Tools, tools on PATH (Homebrew CMake) or Gatekeeper's approval; the sheet says so.
It is refused while a game holds the state folder's lock. **Archives…** lists archives: **Restore…** archives the
current state first, then moves the archive back and relaunches; **Delete Archive…** is separate and confirmed.
`fresh.json` in each archive lists every moved item and where it came from.

Tests use `FABLE2_APP_STATE_ROOT` (and `FABLE2_FRESH_SYSTEM_ROOT` for stand-ins of the system folders; without it a
test never touches system folders). CLI (same code): `FableIIPlayer --start-fresh [--archive-saves]`,
`--list-archives`, `--restore-archive NAME`, `--delete-archive NAME`. `tests/start_fresh_tests.py`.

## Test switches

| Variable | Effect |
|---|---|
| `FABLE2_APP_STATE_ROOT` | state folder (tests never use the real one) |
| `FABLE2_PORTABLE_REUSE_FROM=` | no reuse of other apps' extractions |
| `FABLE2_SIMULATE_NO_CLT=<file>` | the Command Line Tools count as missing until `<file>` exists; Install only logs |
| `FABLE2_TEST_AUTO_INSTALL_TOOLS=1` | press Install at once |
| `FABLE2_GAME_BUILD_ARGS="…"` | extra driver arguments: `--measure`, `--jobs N`, `--ignore-system-cmake`, `--cmake-url URL`, `--clean` |

## Measurements (M5 Max, 18 cores, 64 GB, macOS 27.0.1, Apple clang 21)

Evidence: `out/validation/release-app/` (KEEP): `e2e.py` (the harness), `e2e*-report.json`, build logs,
`series-j*.log`, `measure-j18.json`, gameplay captures.

**End to end, packaged app, test-scoped state folder, real disc image** (`e2e.py`: background, input-isolated agent
test instance holding the shared perf lock and a 3× game slot; closed by its own pid):

| Run | Steps | App start → gameplay |
|---|---|---|
| Fresh state | extract 1 s (image in the page cache), Homebrew CMake found, codegen 26 s, compile + link 57 s (18 jobs), sign 0.5 s: **built in 84 s**; shader prep 11 s (nothing to prepare, now skipped); game ready 16 s later (Old Tomb save) | **113 s** |
| Start Fresh (saves kept), then first launch with the Command Line Tools simulated missing and system CMake ignored | Start Fresh moved 12 items; the window waited for the tools' readiness signal; CMake 4.1.2 downloaded from Kitware, SHA-256 verified and unpacked in 4 s; **built in 94 s** | **107 s** |
| Update (new kit, one source file changed) | kit sync copied 1 file, nothing recompiled, game bundle re-signed: **built in 4 s** | 18 s |

The built game reports `backing scale=2 [retina]` (the bundle's Info.plist applies), its window title is the normal
one, the `.bundle` is not in the LaunchServices database and Spotlight finds no application bundle in `Game/`. Files
in `Game/` carry no quarantine attribute.

**Job count calibration** (clean builds from the kit with `fable2_game_build.py build --clean --jobs N`, codegen
included; another agent's 4-job build overlapped part of these runs):

| Jobs | Total | Codegen | Compile + link | Swap growth, pressure |
|---|---|---|---|---|
| 18 | 108 s | 37 s | 70 s | 0, normal |
| 8 | 163 s | 55 s | 107 s | 0, normal |
| 4 | 201 s | 41 s | 159 s | 0, normal |
| 2 | 306 s | 31 s | 274 s | 0, normal |

**Compile memory** (`--measure`: `/usr/bin/time -l` as compiler and linker launcher, 344 units): CPU 745 s in
total; peak RSS per unit median 258 MB, p90 266 MB, max 835 MB (`fable_2_recomp.141.cpp`, also the slowest at
22–29 s); `fable_2_register.cpp` 634 MB, 5–6 s; `src/main.cpp` 824 MB; the final link 647 MB. The plan's guess of
1.5 GB per job was about 6× too high, so the job rule is `(RAM − 3.5 GB) / 0.75 GB`: 8 GB → 6 jobs, 16 GB → all
cores. Compiling `fable_2_register.cpp` alone first costs about 6 s.

**Estimates for other Macs** (not measured; the app's own estimate uses the same model: 745 s of M5 compile CPU and
~55 s of codegen and the longest file, scaled by chip generation (M1 1.8×, M2 1.6×, M3 1.4×, M4 1.15×) over
performance cores + 0.4 × efficiency cores):

| Mac | Jobs | Build | Peak compile memory |
|---|---|---|---|
| M1, 8 GB (4P + 4E) | 6 | ~5–6 min | ~2.5 GB (6 × 0.26 GB + one 0.8 GB unit) |
| M1 Pro / M2 Pro, 16 GB (8–10P + 2–4E) | 10–12 | ~3 min | ~3–4 GB |
| M3 Pro, 18 GB (6P + 6E) | 12 | ~3 min | ~3–4 GB |

Not tested here: a Mac without the Command Line Tools (simulated only: `xcode-select --install` and Apple's dialog
were not run), Gatekeeper on a downloaded zip, and a real 8 GB Mac.
