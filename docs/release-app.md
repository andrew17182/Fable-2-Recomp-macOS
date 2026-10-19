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
| First-launch windows (welcome, extraction, build, shader prep) and the launcher | `tools/macos/Player/main.swift` |
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

**Since 2026-10-15 the first install compiles the disc's own shader bank instead** (no recipe, nothing shipped;
`docs/shader-precompile-20261015.md`): the prep step reads `data/Shaders/ShadersRelease.sbk` from the player's
extracted files and translates + compiles every pixel shader variant and the vertex shaders D3D does not patch
(`FABLE2_SHADER_PREP_BANK`, Settings "Prepare all of the disc's shaders at install", default on), at the first prep
scale. M5 Max: ~45 s added at 2×; draw-path translation hitches (`xlate`) in the first minute go from 1.2–1.8 s to
~0, hitch time from 2.1–3.4 s to 0.5–0.9 s. What is left is pipeline builds (`pso_emu`).

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

1. **Welcome**: choose the disc image (open panel or drag and drop onto the window), or reuse a finished extraction
   of another Fable II app on this Mac.
2. **Extract** with `FableIIPrepare` (progress, Cancel, Show details). The app opens the image itself and hands the
   open file to the extractor (`--iso-fd 3`), so the extractor never opens a path in a protected folder. `default.xex` is hashed first; a different
   version **blocks** in the release app (the code translation is specific to that exact file).
3. **Command Line Tools**: `xcode-select -p` must name a developer folder with `clang`. If not, the window explains
   why and **Install Command Line Tools…** runs `xcode-select --install` (Apple's own dialog). The window then checks
   every 2 s for that readiness signal and continues by itself.
4. **Build**: the app first copies its kit out of the bundle with its own process (`Build/kit`, an APFS clone when it
   can, removed after a successful build), then runs
   (`/usr/bin/python3 -I -B -u <state>/Build/kit/game-src/tools/fable2_game_build.py build --kit <state>/Build/kit --state … --game-data …`),
   shown as a checklist with one progress bar, an estimate, Cancel and "Show details" (every real command echoed as
   `$ …` with its output, also in `logs/build.log`):
   - **CMake**: our own copy, then PATH, Homebrew and `/Applications/CMake.app` (3.25+); otherwise Kitware's
     official 4.1.2 macOS archive is downloaded into `Build/tools/cmake` (pinned URL and SHA-256, no admin rights).
   - **Copy the build kit** into `Build/` (only changed files; see Updates).
   - **Translate the game code**: `rexglue init` (first time), configure, `rexglue codegen`, configure again, recomp
     patches.
   - **Compile**: everything with `jobs = clamp(min(cores, floor((RAM − 3.5 GB) / 0.75 GB)), 1, cores)`
     (`fable_2_register.cpp`, the one -O0 unit, needs no PCH and starts in the first wave). Generated code is
     compiled without debug info (`REXGLUE_RECOMP_DEBUG_INFO=none`; see "Build time"). A memory watch polls `kern.memorystatus_vm_pressure_level` and
     swap once a second; critical pressure or more than 1 GB of swap growth stops make and resumes with half the jobs.
   - **Link**, then **Sign on this Mac**: `Game/Fable II.bundle` is assembled beside the old one (fable_2 + runtime
     libraries + the app's Info.plist with fable_2 as executable, same bundle id), `codesign --sign -` each binary and
     the bundle, `codesign --verify --strict`, then swapped in.
   - Errors show a plain sentence, **Copy Details** (error, Mac, app version, last 200 log lines), **Show Log
     File** and **Try Again** (continues where it stopped: the build tree is kept).
5. **Preparing shaders** (as in the portable app: one headless run per scale, Skip allowed). On a first install
   (no lists remembered yet) it now runs too: the first scale compiles the disc's shader bank
   (`docs/shader-precompile-20261015.md`).
6. **The launcher** (its front page, `docs/player-app.md`), with "The game is built and ready" (or "updated and
   ready" after an update). A build never starts the game by itself, so the player can review Settings, Controls and resolution first. Only the test flag
   `--play` (used by the end-to-end harness) plays as soon as the game is ready.

## Protected folders

macOS asks the player before an app reads Downloads, Desktop, Documents, iCloud Drive, other cloud storage
(`~/Library/CloudStorage`) or removable and network volumes (System Settings → Privacy & Security → Files and
Folders). A process reading its own app bundle is not asked; helper processes from elsewhere are, and the request is
made in the app's name.

**The reported failure (2026-10-15, release zip on an M3 Pro, app left in `~/Downloads`).** Extraction worked
(`FableIIPrepare` lives in the bundle), then the build step ran `/usr/bin/python3 …/Fable II.app/Contents/Resources/kit/game-src/tools/fable2_game_build.py`.
`/usr/bin/python3` is not part of the bundle, so opening that script was a read of `~/Downloads` in Fable II's name
(the app was confirmed to still be in Downloads; a translocated copy is a read-only view of that same folder): "Fable II would like
to access files in your Downloads folder". After **Don't Allow**, python exited 2 with `can't open file …: [Errno 1]
Operation not permitted`, the window said only "The build stopped (exit 2)", and Try Again repeated the same command,
which macOS refused again at once (it remembers the answer), so it looked as if nothing happened.

What the app does now (`tools/macos/Shared/AppAccess.swift`, `tools/macos/Player/main.swift`):

- **No Move to Applications (removed 2026-10-19).** The app runs wherever the player put it and never copies,
  moves or trashes its own bundle. The old first-launch offer (2026-10-15) copied the app to `/Applications` and put
  the original in the Trash; trashing a file in `~/Downloads` made macOS ask for Downloads access. Rule: the app must
  not trigger macOS permission prompts. The README asks the player to drag Fable II to Applications.
- **Nothing outside the bundle reads the bundle**: the kit is copied out by the app's own process before python runs.
- **The disc image comes from the open panel or drag and drop** (a user pick grants access to that file even in a
  protected folder or after Don't Allow) and is handed to the extractor as an open file. A path that was never picked
  in this session (relaunch while an extraction was incomplete) brings back the welcome window.
- **A refused access is explained** (EPERM/EACCES, classified by folder): "macOS is not letting Fable II read its own
  files" (build kit; **Open Privacy Settings**, **Try Again**) or "… read the disc image"
  (**Choose Disc Image…**, **Open Privacy Settings**, **Try Again**). The build driver reports a refused read as
  `[denied] <errno> <path>`; python's own "can't open file" is recognized too. **Try Again** really retries: it
  opens the image or copies the kit again and, if macOS still refuses, says "Checked again at <time>: still no
  access." Copy Details includes the refused path and the app's location.
- With ad-hoc signing every new version is a new app to macOS privacy, so an answer given to an old version does
  not carry over.

Tests: `tests/release_access_tests.py` (classification, translocation by path, no Trash/Apple-event/move calls in
the app sources, an app in a stand-in Downloads building in place with its bundle untouched,
the extractor reading a handed-over image from a folder it cannot open, the driver's `[denied]` line, and the real
windows in background mode with a chmod-ed stand-in Downloads folder: refused, still refused after Try Again, then
going on after access returns; a finished build waits on the Play window).
Snapshots: `out/validation/release-access/` (KEEP): `denied-disc-image.png`,
`denied-build-kit.png`, `play-ready-after-build.png`.

**Manual check (a real privacy prompt cannot be automated):**

1. Download the release zip in Safari (so it is quarantined), let it unzip in `~/Downloads`, approve it with Open
   Anyway, open it from Downloads (from a script: `open "Fable II.app" --args -ApplePersistenceIgnoreState YES`). Expect
   the welcome window first (no Move offer), no Downloads prompt through the build, and the app still in Downloads.
2. Drag Fable II to Applications and open it there: the same, with no Gatekeeper question for this copy.
3. Start Fresh, open the copy in Downloads again and choose a disc image in Downloads.
   If macOS asks about Downloads, click **Don't Allow**: the build or the image window explains it; **Try Again**
   says "still no access"; turning Fable II on in Files and Folders (then Try Again) continues. For the image,
   **Choose Disc Image…** with the same file continues even with access off.
4. Reset the answer afterwards: `tccutil reset SystemPolicyDownloadsFolder org.fable2recomp.metal.release`.

The build environment is cleaned: no `FABLE2_*`, `REXGLUE_*` or `CMAKE_*` variables from the caller, PATH is
CMake + `/usr/bin:/bin:/usr/sbin:/sbin`, and CMake's Python steps (recomp patches, version header) are pinned to
`/usr/bin/python3` (`-DPython3_EXECUTABLE`), the Command Line Tools' 3.9 the driver itself runs under: with a
Homebrew CMake its `bin/` is on PATH and CMake otherwise picks Homebrew's Python. The CMake option `FABLE2_GPU_PLUGIN_XENOS=OFF` keeps the Xenos plugin
(not in the kit) from being staged. CLI: `fable2_game_build.py plan` (job count and why), `status`,
`measure-report STATE` (for `--measure` builds).

## Gatekeeper (2026-10-19)

The app is ad-hoc signed and not notarized. Downloaded and unzipped, it opens with **"Fable II" Not Opened**; the
player clicks **Done**, then **System Settings → Privacy & Security → Open Anyway**, and confirms. No Terminal.

Test on an M5, macOS 27: each zip downloaded the Safari way (quarantined), extracted with `ditto`, then opened.

| Arm | Signing | Zip | Result |
|---|---|---|---|
| A | sealed ad-hoc (`codesign --sign -`: runtime libraries, helpers, then the bundle) | no `__MACOSX/._*` entries | "Not Opened"; Open Anyway, confirm: it runs |
| B | sealed ad-hoc + hardened runtime (`--signing runtime`) | no `__MACOSX/._*` entries | "Not Opened" |
| C | unsealed (each executable signed alone, no `_CodeSignature`) | no `__MACOSX/._*` entries | **"damaged"** (Move to Trash only) |
| previous release | sealed ad-hoc | `ditto --sequesterRsrc`: 1,724 `__MACOSX/._*` entries | **"damaged"** |

Cause: the zip, not the signature. `ditto -c -k --sequesterRsrc` stored the build Mac's extended attributes
(`com.apple.provenance`) as AppleDouble `__MACOSX/._*` entries; extracted back onto the app, macOS 27 rejected it as
damaged. Every user-facing zip (release, portable, benchmark and flash kits) is now made by
`fable2_portable.zip_bundle`: `ditto -c -k --norsrc --noextattr --noacl --keepParent`, then `check_zip` refuses any
`__MACOSX/` or `._` entry (`tools/fable2_release.py check` runs it on a zip; `tests/release_build_tests.py`). The
signing stays sealed ad-hoc; `--signing runtime` is kept for testing; an unsealed bundle reads as damaged, so it is
gone. If Open Anyway is not offered, `xattr -dr com.apple.quarantine "/Applications/Fable II.app"` is the fallback.

**Test launches** of the release app (`open "Fable II.app"` or the executable) need `-ApplePersistenceIgnoreState
YES`: otherwise macOS restores the saved window state of an earlier run, and that can leave no window open.

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

## Release checks

`fable2_release.py build` refuses a package unless:

- `check`: no game content, nothing game-derived, no converter, no local path, and the kit has every file its
  `CMakeLists.txt` uses in the player's build (blocks the player build skips: Windows, Debug, the test options);
- `verify-build`: a full player build from the zip, as the Player runs it, in `out/release-verify/<time>` (removed
  when it passes, kept with its logs when it fails). Needs `default.xex` and `data/` (default: this checkout).
  `--update-from OLD_ZIP` also builds the older release first and this one over its tree. `--no-verify-build` skips it.

The player build passes `-DFABLE2_BUILD_TIME_TESTS=OFF`: the developer build-time checks that `fable_2` depends on
are not built there (0.1.0 (91cfcdb) failed every player build: one of them runs a tool the kit did not ship).

## Start Fresh

Settings → General → Logs and data → **Start Fresh…** (also File menu) moves everything the app owns into
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
| `-ApplePersistenceIgnoreState YES` (argument, not a variable) | always pass it on test launches: saved window state from an earlier run can otherwise leave no window |
| `FABLE2_APP_STATE_ROOT` | state folder (tests never use the real one) |
| `FABLE2_PORTABLE_REUSE_FROM=` | no reuse of other apps' extractions |
| `FABLE2_SIMULATE_NO_CLT=<file>` | the Command Line Tools count as missing until `<file>` exists; Install only logs |
| `FABLE2_TEST_AUTO_INSTALL_TOOLS=1` | press Install at once |
| `FABLE2_TEST_PROTECTED_DIRS=<dir>[:…]` | these folders count as Downloads (a chmod-ed one then shows the privacy texts) |
| `FABLE2_TEST_RETRY_WHEN=<file>` | while access is refused, press Try Again when the file appears (it is removed) |
| `FABLE2_GAME_BUILD_ARGS="…"` | extra driver arguments: `--measure`, `--jobs N`, `--ignore-system-cmake`, `--cmake-url URL`, `--clean`, `--debug-info line-tables-only` and `--register-first` (the build before 2026-10-15) |

## Build time (2026-10-15)

Clean first-launch builds of the packaged kit (`fable2_game_build.py build --clean --measure`, real disc, perf lock
held exclusively for the 11-job runs, an idle Mac for the 18-job runs; M5 Max: 6 super + 12 performance cores). Evidence:
`out/validation/build-speed/` (KEEP): logs, CPU samples, per-unit `/usr/bin/time` data, `timeline.py`.

| Build | Jobs | Codegen step | Compile + link | Total |
|---|---|---|---|---|
| Before (kit 03558af) | 18 | 24.4 s | 57.2 s | **82.4 s** |
| After | 18 | 7.4 s | 52.3 s | **60.3 s** (−27%) |
| Before | 11 | 31.4 s | 73.0 s | **105.2 s** |
| After | 11 | 8.8 s | 64.7 s | **74.1 s** (−30%) |
| M3 Pro 11 cores, before (maintainer's `build.log`, 2026-10-14) | 11 | 40.3 s | 146.3 s | **188.4 s** |
| M3 Pro, after (estimate: the M5 11-job ratios) | 11 | ~11 s | ~130 s | **~2.4 min** (−25%) |
| M1 8 GB, after (estimate) | 6 | ~13 s | ~210 s | **~3.8 min** (was ~5.6) |

What changed (all exact: same generated code, same machine code):

- **Codegen 4.5× faster** (`patches/sdk/codegen-graph-index.patch`): `rexglue codegen` is single-threaded and
  spent ~13 of its 20 s in two quadratic loops. `FunctionGraph::notifyFunctionAdded` tried every one of ~61,000
  functions against each new one, and `cleanupAbsorbedGapFills` called `containsAddress` for every pair. Both are now
  indexed (unresolved-jump target → functions; base-sorted interval walk). All 591 generated files are
  byte-identical to the old rexglue's output (only `codegen.d` names the folder); 24.6 s → 5.4 s alone.
- **Recomp patches in worker processes** (`tools/apply_recomp_patches.py`): the per-file passes run in a spawn pool
  (`FABLE2_RECOMP_PATCH_JOBS`, 1 = one process); same files and counts; 3.6 s → 0.7 s.
- **No debug info for generated code** (`--debug-info none`, default): −6.6% compile CPU (698 → 652 s for the
  generated units), objects ~45% smaller. The machine code is identical (`otool -t` of units with and without
  `-gline-tables-only`; the linked `__text` has the same size, its only differences are string addresses from the
  build folder's name in SDK `__FILE__` strings). Function names stay in the symbol table, so crash reports and
  `f2diag` stacks are unchanged; only file:line for generated frames is gone (the objects were on the player's Mac
  only anyway).
- **`fable_2_register.cpp` no longer alone first**: it is in the first wave of the parallel build (5–7 s of one core
  instead of the whole Mac). `--register-first` restores the old step.

Measured and not taken:

- **-O2 for generated code**: per-unit compile time is the same as -O3 (unit 141: 9.5 vs 9.4 s; typical units
  1.47–1.50 vs 1.51–1.53 s), so there is no build win to weigh against a performance risk. -O1 is slower on the
  largest unit (13.3 s). Time per typical unit: frontend 0.4 s, optimiser 0.7 s, code generation 0.5 s.
- **Largest units first / one job pool for both targets** (a driver pre-pass that compiled the recomp units by size
  and `fable_2`'s own objects in one make): 74.5 vs 74.1 s at 11 jobs, 57.7 vs 60.3 s at 18. Not worth a second
  build path. At 18 jobs the unit with the 2.5 MB guest function (`sub_82242ED0`, 72,500 lines; ~9 s on a super
  core, 22 s on a performance core) is the critical path; at 11 jobs and below it is not.
- **Splitting large units**: the slow unit is one guest function, which codegen cannot split
  (`max_file_size_bytes` only groups functions), so no gain.
- **Ninja**: not in the Command Line Tools; with no ordering gain measured above, not worth bundling.
- **Job count**: the rule already uses every core (P + E) on 16 GB and up (M3 Pro: 11 jobs); the memory watch did
  not trip in any run (peak pressure normal, no swap growth).

Updates: the kit-update rebuild of 2026-10-14 recompiled everything (150 s on the M3) because SDK headers in the PCH
changed. This release changes the generated code's flags too, so the first update to it rebuilds once.

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
were not run) and a real 8 GB Mac. Gatekeeper on a downloaded zip was tested by hand (see Gatekeeper).

## Import saves (2026-10-15)

The release app keeps its own state folder, so on a Mac that already ran the portable or dev build it started with
no saves (maintainer, M3). **Import Saves…** (File menu, and Settings > General > Logs and data) copies hero saves from another
install on this Mac (`~/Library/Application Support/Fable II Recomp/saves`) or from a chosen folder (a `saves`
folder or the data folder holding one) into this app's `saves/`:

- it copies, never moves; profile data (options, achievements) is copied only when this app has none yet;
- a hero save this app already has is kept unless the player chooses **Replace**; replaced saves are moved to
  `saves-backup-<stamp>` next to `saves/` first;
- the result lists what was imported, replaced and kept (also in `logs/player.log`).

On the first launcher front page with no saves of its own, the app offers the import once, and only when another install's
saves are found. `--import-saves FOLDER [--replace]` runs the same code without the window
(`tests/import_saves_tests.py`, stand-in folders; `FABLE2_IMPORT_SAVES_FROM` stands in for the other install).
