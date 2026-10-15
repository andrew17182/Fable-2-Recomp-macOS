# ReXGlue SDK built from source (2026-10-09)

Branch `feature/sdk-from-source`. The Metal build now compiles the ReXGlue runtime (`librexruntime.dylib`), the
`rexglue` recompiler, SDL3 and the rest of the SDK package from the pinned v0.10.0 source instead of downloading the
prebuilt release. Phase A was an **equivalence build**: no behaviour change was intended, and no runtime
behaviour patch is applied. Phase B (proposal, last section) lists the workarounds that can now become real SDK fixes.

## Why

With the prebuilt runtime:

- runtime-internal fixes were impossible: callers inside the dylib (kernel dispatch, the audio worker,
  `XThread::Delay`, wait timeouts, `WindowSDL::OpenImpl`) cannot be reached from the executable;
- `patches/sdk/*.patch` never reached the build;
- the executable compiled against the prebuilt headers and the Metal plugin against patched SDK source;
- workarounds had to poke runtime internals from outside (exported variables, `__imp__` overrides, plugin exports).

## Source and version

- The prebuilt v0.10.0 package was built from tag `v0.10.0` = `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`, which is
  the `sdk-source` pin in `backends/metal/dependencies.json`. Evidence: `git ls-remote` maps the tag to that commit,
  and the prebuilt reports `build: rexglue-v0.10.0-mac-arm64-Release@20260821_2215`, 5 minutes after the tag commit
  (22:10 UTC). The release merge on `main` (`c94f5eb`) has the same tree (`93d1bc1`).
- Release CI recipe (`.github/workflows/_build-platform.yaml`): `cmake --preset mac-arm64
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3`, Ninja Multi-Config, `/usr/bin/clang`, `-O3 -march=armv8-a`, C++23, `--target install`.
- So there is no source difference between the release and our pin. The differences that matter are in packaging:
  - The GitHub source archive has **no submodules**. The 19 needed are pinned from the tag's gitlinks in
    `backends/metal/sdk-submodules.json` (commit + sha256; vulkan-loader, MoltenVK and catch2 are not needed).
  - Without git tags, the version would resolve to `0.10.0.0-dev.unknown`, and codegen embeds `<floor>-<channel>`.
    `sdk-build.patch` adds `REXGLUE_VERSION_OVERRIDE`; the build passes `0.10.0`.
  - SDL takes its revision string from git; the build passes the release's (`SDL-3.5.0-release-3.4.0-428-g8bf3b7215`).

## How it works

`tools/build_sdk_from_source.py` (Python 3.9-compatible; called by `tools/build_macos_metal.py`, or run alone):

1. Fetch and verify `sdk-source` (shared with the Metal plugin, cache `out/downloads/metal`) and the 19 submodule
   tarballs (cache `out/downloads/sdk-src`, 58 MB). Extract to `out/sdk-src/source`; each submodule gets a
   `.rexglue-pinned` marker in place of `.git`.
2. Apply `backends/metal/sdk-build.patch` (build-only), then the runtime hunks of `backends/metal/sdk-metal.patch`,
   then `SDK_BEHAVIOUR_PATCHES` (empty in phase A; the six phase B fixes since 2026-10-09, see "Phase B status").
3. Configure like the release CI, but with Unix Makefiles and a single Release config (Ninja is not required), then
   `--target install` into `out/sdk-src/mac-arm64` with `HOME` redirected to `out/sdk-src/home` (the SDK's install
   step registers itself in `~/.cmake/packages`).
4. Write `out/sdk-src/mac-arm64/.fable-sdk-build.json`: an input fingerprint (pins, patch hashes, configure flags,
   script hash), the build time and output hashes. The next build skips the SDK when the fingerprint matches.

The install has the prebuilt layout (`bin/rexglue`, `lib/librexruntime.dylib`, `lib/librexgpu-xenos.dylib`,
`lib/libSDL3.a`, `lib/cmake/{rexglue,SDL3,fmt,spdlog}`, `include/`, `share/rexglue`), so the game's
`find_package(rexglue 0.10.0)` through `REXGLUE_SDK_ROOT` / `CMAKE_PREFIX_PATH` is unchanged. Only Release
libraries are installed (the prebuilt also ships `d`/`rd` variants that nothing here uses).

### `sdk-build.patch` (build system only)

- `cmake/rex_version.cmake`: `REXGLUE_VERSION_OVERRIDE`.
- `thirdparty/CMakeLists.txt`: accept `.rexglue-pinned` as an initialised submodule; new option
  `REXGLUE_MACOS_VULKAN_RUNTIME` (default ON upstream, OFF here) that skips the Vulkan loader and MoltenVK.
  The Metal app ships neither, and MoltenVK's CMake fetches four more repositories through CPM at configure time.
- `cmake/rexglue_install.cmake`, `cmake/rexglueConfig.cmake.in`, `cmake/rexglue_helpers.cmake`: install, import and
  stage the loader and MoltenVK only when they are present.

### `sdk-metal.patch` and the "one source" goal

The full patch adapts the graphics sources for the Metal plugin, which compiles them itself with
`backends/metal/compat.h` and its `port/` headers. Applied to the whole SDK tree it breaks `rexgpu-xenos` (the Vulkan
plugin): `trace_writer.h` not found, `FixedVMemVector`, `global_unique_lock_type`. The runtime compiles only the Xenos
format layer from `src/graphics` (`src/core/CMakeLists.txt`), and the executable includes only
`rex/graphics/register_file.h`. So the runtime build takes exactly the hunks for `include/rex/graphics/xenos.h` and
`include/rex/graphics/pipeline/texture/info.h` (`METAL_PATCH_RUNTIME_FILES`; both are additive inline helpers). The
script fails if the patch stops touching them. The executable, the runtime and the plugin now all see the same
`xenos.h` / `info.h`, and the installed headers match the source the runtime was built from.

### Switch

| Setting | Effect |
|---|---|
| default (`FABLE2_SDK_FROM_SOURCE` unset or `1`) | `build_macos_metal.py` builds or reuses `out/sdk-src/mac-arm64` and passes it to `build_macos.sh` as `REXGLUE_SDK_ROOT` |
| `FABLE2_SDK_FROM_SOURCE=0` | prebuilt v0.10.0 download (`out/sdk/mac-arm64`), exactly as before |
| `REXGLUE_SDK_ROOT=<dir>` | that package, either way |

- Only the Metal build uses the switch. The plain Vulkan build (`FABLE2_MACOS_BACKEND=vulkan tools/build_macos.sh`)
  keeps the prebuilt package, because it needs the loader and MoltenVK.
- Switching recompiles the game, because the include paths change. `build_macos.sh` drops the game's `CMakeCache.txt`
  and `build_macos_metal.py` drops the Metal backend's when the SDK root differs. Their caches pin `rexglue_DIR`,
  `SDL3_DIR` and the like to the first SDK they found; before this fix the backend kept linking against the old one.
- It is a build-environment switch, not a runtime setting, so it has no Dev Launcher control.
- **Patch changes rebuild automatically (2026-10-09).** When `sdk-metal.patch` or `dxilconv-build.patch` no longer
  matches a tree's `.fable-patch-sha256` marker, `build_macos_metal.py` logs one `[metal-deps] ... changed` line,
  deletes that tree (only inside `out/metal-build/dependencies/`), re-extracts it from the cached archive, re-applies
  the patch, and deletes the build trees compiled from it (`out/metal-build/backend`; for DXC also
  `dependencies/build-dxilconv`). Restored files keep the archive's old timestamps, so an incremental build would
  otherwise keep stale objects. `compile_metal_helpers.py` reads only the unpatched `xenios-source`, so it is not
  affected. `build_sdk_from_source.py` already re-extracts and rebuilds whenever any input hash changes (its fingerprint
  includes every patch). Checked: a comment line added to `sdk-metal.patch`, then removed; both
  `/usr/bin/python3` builds succeeded with no manual step (the earlier "Patch changed; remove ..." error is gone).
  The Metal build script also now extracts under Python 3.9 (no `filter=` argument there).
- Codegen does not rerun when only the `rexglue` binary changes (its depfile lists the xex and manifests). That is
  fine for v0.10.0, whose output is byte-identical (below). After a codegen change, remove `generated/default/codegen.build.stamp`.

## Build time and disk (M5 Max)

| Step | Prebuilt | Source |
|---|---|---|
| SDK, clean, empty download caches, path with spaces, `/usr/bin/python3` 3.9.6, Homebrew CMake 4.4.3, 12 jobs | 2.4 s (69 MB zip) | **76 s** (108 MB of downloads; 50 MB of that is `sdk-source`, which the plugin needs anyway) |
| SDK, clean, downloads cached, path with spaces, 4 jobs (the scripts' default) | — | **92 s** (12 jobs: 57–58 s) |
| SDK, inputs unchanged | 0 s | 0 s (fingerprint match) |
| Game after an SDK-root switch (full recompile) + plugin | 78–85 s | 78–85 s |
| Disk | 301 MB | ~1 GB in `out/sdk-src` (source 553 MB, build tree 390 MB, install 88 MB) |

So a clean first build takes about **75 s longer** with 12 jobs, and about 90 s longer with the default 4 jobs. Rebuilds are unaffected while the inputs stay the
same. The source build needs nothing beyond the current requirements: clang from Xcode, CMake ≥ 3.25, `patch` and `curl`.

## Codegen comparison

`rexglue codegen fable_2_manifest.toml` was run by the prebuilt and by the source-built binary, in two scratch copies
(`out/sdk-src-codegen/{prebuilt,source}`: manifest, switch tables, xex):

- 597 written files each, **byte-identical**, including the regenerated `generated/rexglue.cmake`. The only
  difference is the absolute directory in `codegen.d` (depfile).
- `tools/apply_recomp_patches.py` on the source output: 147 guest barriers + 8406 fast-MSR sections on the first run,
  0 on the second (idempotent). The patched trees from both binaries are identical.
- Codegen time is 21.8 s (prebuilt) and 26.4 s (source), on one run each.

## Export comparison (`nm -gU`)

| Library | Prebuilt | Source | Missing in source | Extra in source |
|---|---|---|---|---|
| `librexruntime.dylib` | 11231 | 11204 | 28 | 1 |
| `librexgpu-xenos.dylib` | 934 | 934 | 0 | 0 |
| `libTracyClient.dylib` | 314 | 314 | 0 | 0 |

- **Missing (28):** SDL's libusb HIDAPI backend (`LIBUSB_hid_*`, `SDL_HIDAPI_DriverSwitch2`, `SDL_InitLibUSB`,
  `SDL_QuitLibUSB`). The CI runner had libusb headers; this Mac has none. The prebuilt only `dlopen`s
  `libusb-1.0.dylib` at run time, so it is inert unless Homebrew libusb is installed. It matters only for a
  Switch 2 controller over USB. To restore it, pin libusb as a header source.
- **Extra (1):** `std::try_to_lock`, an exported libc++ constant from the newer local Xcode.
- **Our bindings, all present:**
  - every one of the executable's 421 imports from `librexruntime` and the Metal plugin's 114 resolve against the
    source runtime;
  - all 23 executable `__imp__*` strong overrides that shadow runtime exports, including `__imp__VdInitializeRingBuffer`
    (ring re-init fix, forwards with `dlsym(RTLD_NEXT)`) and `__imp__XexGetProcedureAddress` (fast content I/O);
  - `rex::chrono::guest_time_scalar_` (turbo wait fix), `Clock::set_guest_time_scalar`, `rex::AddSink`.
  The plugin export `fable2_ring_reinit` is unaffected (it is in `librexgpu-metal`).
- **Installed headers:** identical except `xenos.h` / `info.h` (the hunks above), `version.h` (build timestamp) and
  `rex/system/xmemory.h`. The prebuilt copy in `out/sdk` had an experimental header change applied by hand (inline
  `ContainsCommittedRange`); nothing in this build uses it. The prebuilt `share/vulkan/icd.d` (MoltenVK) is absent by design.

## Phase A validation (M5 Max)

Final state: source build. `pgrep -x fable_2` returned 1 before and after every run. First launches after each rebuild
are counted. Labels are under `out/validation/f2diag/`; logs and extra artifacts are in `out/sdk-src-compare/`.

| Check | Source build | Prebuilt build (`FABLE2_SDK_FROM_SOURCE=0`) / reference |
|---|---|---|
| Build + config / flight-recorder build tests | pass (3 builds, `/usr/bin/python3` included) | pass (2 builds) |
| `run bootloop --n 5` (`sdksrc`, `sdksrc2`; `sdksrc-final --n 2`) | 12/12 pass, 0 hangs, 0 `frame-throttle skew`, 3 `[ring-reinit]` per boot, 0 AVs, early window restore (`... (before first show)`) every boot; title 2.3 s (first launch 2.8–3.1 s) | `rr-fix-on`: 0/20 skew, title 2.45 s |
| `run area` (save `hero1-dog-live_2026-10-07_18-49-47`; `sdksrc-area`, `sdksrc2-area`) | Bower Lake reached (beetle fight); level loads 416/717 and 411/707 ms; trigger → first Bower Lake frame ≈ 4.7–5 s; 0 AVs | `rr-area`: 423/710 ms |
| `run f5` (`sdksrc-f5`, `sdksrc2-f5`) | quick save written both times; 0 AVs | `rr-f5`: written |
| Turbo, `FABLE2_TURBO_SCALE=2` (+ `FABLE2_SAVE_FAST_MOUNT=0`, `FABLE2_LOADING_TRACE=1`; `sdksrc-turbo`) | `Sleep(100)` 50.0–55.0 ms; `Sleep(10)` mean 5.86 ms (6694 calls); log: duration scalar 0.50, so not inverted twice | `docs/turbo.md`: 50.7–54 ms, 5.9 ms |
| Uncapped Old Tomb sweep, save `14-45-08`, `FABLE2_GPU_TIME_TRACE=1`: window FPS / GPU mean / GPU p99 ms | 169.1 / 5.87 / 11.0; 154.8 / 6.41 / 17.5; 167.8 / 5.92 / 10.9 | 157.3 / 6.31 / 12.0 (first launch); 164.9 / 6.01 / 10.8; 169.2 / 5.88 / 10.9 |
| Sweep artifacts (`f2diag images`) | blue 0; dark 52–63 camera-motion frames; dips > 1%: 2 / 4 / 2 | blue 0; dark 46–63; dips > 1%: 1 / 0 / 0 |
| AVs | 0 in all 19 sessions | 0 |

- Performance is within run-to-run noise. Screen sharing was active during these runs, and the one low source run
  is the outlier.
- **Dog fur: 0 flashes.** The dark dips above 1% of the grid, on both builds, were inspected by eye. They are
  near-camera occluders during the rotation (a pillar or wall edge between camera and hero; one frame mostly dark with
  the hero faint). There were no stray or dark translucent polygons. The higher count on the source build is noted; it
  is the same kind of frame, and neither the counters nor the log digests (identical error counts) show a mechanism.

## Phase B status (2026-10-09; approved order B2, B5, B6, B1, B4, B3)

Rule: an SDK fix and its executable workaround are never both active. Every workaround either is removed or detects
the fixed runtime and stands down, and stays active on the prebuilt runtime (`FABLE2_SDK_FROM_SOURCE=0`).

| # | Status | SDK change | Workaround | Switch / fallback | Validation (source build) |
|---|---|---|---|---|---|
| B2 | **Done** | `patches/sdk/clock-duration-scaling.patch`: `ScaleGuestDuration{Millis,FileTime,Timeval}` and the `XSystemClock`↔`WinSystemClock` casts divide/multiply the right way; signed relative `FileTime`; `Timeval` keeps the fraction | `fable2::turbo::SetActive` probes `ScaleGuestDurationMillis(1000)` after `set_guest_time_scalar(s)` (1000/s = fixed runtime) and skips the `1/s` overwrite there | No runtime switch for the SDK fix itself (a pure bug fix). Prebuilt: probe sees 1000·s, the overwrite applies (`FABLE2_TURBO_WAIT_FIX=0` disables it as before) | 2x: `Sleep(100)` 50.1–59.5 ms, `Sleep(10)` mean 6.45 ms, 37/37 expired 500 ms timeouts 250.0–250.5 ms, `[clock-report]` 2.0000 (13/13), tick-sleep 0 late, 0 AVs (`b2-turbo`) |
| B5 | **Done** | `patches/sdk/window-initial-position.patch`: `Window::SetInitialPosition` (members appended last; `REX_UI_WINDOW_INITIAL_POSITION` macro), applied in `WindowSDL::OpenImpl` before `SDL_ShowWindow` with the on-display check (fullscreen and `monitor` win); `ReXApp::OnPreOpenWindow` hook (in `rex_app.cpp`, which the executable compiles from `share/rexglue`) | `Fable2App::OnPreOpenWindow` passes the saved position (`WindowPosition::LoadForSdk`); the SHOWN watch is not armed and `Attach` does not restore again | `FABLE2_SDK_WINDOW_POSITION=0` uses the SHOWN watch on the fixed SDK; `FABLE2_EARLY_WINDOW_RESTORE=0` restores after showing (both). Prebuilt: no macro, so the watch is compiled in as before | CGWindowList poller (~200 Hz): SDK path first bounds `247, 452` = saved `247, 484` minus the 32 pt title bar, never centred; `=0` watch path: same; `EARLY_WINDOW_RESTORE=0`: centred `541, 271` first, saved 0.54 s later; off-screen `-30000, -30000`: log "not on a connected display; using the default", window centred (`b5-win-*`) |
| B6 | **Done** | `patches/sdk/clock-fractional-ticks.patch` (now applied): fractional remainder carried; plus two fixes found while validating: the host anchor never moves backwards (without it 2x read 2.0003–2.0008), and a ratio change closes the old-ratio interval; `REX_CLOCK_EXACT_GUEST_TICKS` macro | `src/core/fable2_guest_clock.h` compiles its strong `Clock::QueryGuestTickCount` out when the macro exists; all `mftb` sites bind to the runtime (`nm -m`) | Prebuilt: override compiled in as before (`FABLE2_CLOCK_SDK_ROUNDING=1` A/B there). CR-13 (mutex per `mftb`) unchanged, now in the SDK: proposal only (seqlock snapshot) | `[clock-report]` 1.0000 13/13 at 1x incl. loading; 2.0000 13/13 at 2x; turbo timings as B2 (`Sleep(100)` 50.3–55.7, 42/42 timeouts 250.0–250.5 ms, 0 late ticks); level loads 414/707 ms; 0 AVs (`b6b-*`) |
| B1 | **Done** | `patches/sdk/ring-reinit-sync.patch` (runtime tree) + the same hunks appended to `sdk-metal.patch` (plugin): `InitializeRingBuffer` drains (≤ 50 ms), parks, resets read + `CP_RB_WPTR = 0`; the worker publishes the read index after its writeback and checks idle race-free; cvar `gpu_ring_reinit_sync` | `RingReinitFix.cpp` asks plugin op 2 and only forwards while the SDK sync is on | `--gpu_ring_reinit_sync=false` → executable workaround; plus `FABLE2_RING_REINIT_FIX=0` → no fix. The plugin is source-built with either runtime, so the SDK fix is active with the prebuilt runtime too | bootloop 20: 0 skew, 0 hangs, title 2.38 s; trace 47 inserts = 47 callbacks; no-fix A/B 3/3 skew; exe-workaround A/B 0/3; area 419/710 ms, 0 AVs (`b1-*`; `docs/boot-hang-20261008.md` §7) |
| B4 | **Done** | `patches/sdk/wait-multiple-blocking.patch` (CR-09): `WaitMultiple` blocks on a per-wait condition variable notified by every handle signal path, ordered blocking locks on distinct handles, exact deadline (the old code busy-spun: whole-ms truncation inside 1 ms alertable slices → `sleep_for(0)`); audio worker's 500 ms idle sleep removed | None existed (SDK-only) | Cvar `wait_multiple_polling=true` (game arg) = old code, for A/B. Prebuilt: old behaviour | Audio Worker 71.4% → 0.5% of a core, process 215% → 139% (`b4-area`, `b4-area-legacy`); temporary underrun counter (removed): unmuted 0 silence frames after start, min queue 5 frames (legacy 2 at start); standalone semantics test passes, wake p50 16 µs (legacy 760 µs); 60 FPS sweep 60.0 FPS, p99 18.2 ms; turbo 2.0000, 0 late ticks, 42/42 timeouts 250 ms; bootloop 10/10, 0 skew; F5 saved; loads 417/719 ms; 0 AVs |
| B3 | **Done** | `patches/sdk/codegen-memory-barriers.patch`: manifest key `memory_barriers` (`RecompilerConfig::emitMemoryBarriers`, default off); `build_sync` / `build_lwsync` emit `REX_GUEST_SYNC()` / `REX_GUEST_LWSYNC()` after the instruction comment; defaults (`std::atomic_thread_fence` seq_cst / acq_rel) in `rex/ppc/intrinsics.h`; `fable_2_manifest.toml` sets `memory_barriers = true` under `[entrypoint]` | `apply_recomp_patches.py` treats `REX_GUEST_*` lines as fenced (prints `0 inserted now, 0 present (+ 147 emitted by codegen)`; idempotent rerun 0); `fable2_guest_memory.h` redefines the macros to `FABLE2_GUEST_SYNC/LWSYNC` | `FABLE2_GUEST_BARRIERS=0` still disables them (log `[guest-barriers] off`; the fences in the objects load `g_barriers` first). Prebuilt `rexglue` ignores the key, so the post-pass inserts its 147 fences as before. No codegen rerun is needed when switching SDKs (the depfile does not list the `rexglue` binary): either generated tree builds against either SDK, because the executable defines the `REX_GUEST_*` macros itself | Codegen diff vs. the post-passed tree: exactly 147 changed lines (128 lwsync + 19 sync, same positions) + the 2 codegen stamps; full recompile; bootloop 10/10, 0 skew; area 389/718 ms; uncapped sweep 193.5 FPS, p99 10.6 ms, GPU mean 5.29 ms; 0 AVs (`b3-*`) |

### Prebuilt fallback (`FABLE2_SDK_FROM_SOURCE=0`, one build, barriers on)

Every workaround re-activated, with no phase B runtime fix present (`build: rexglue-v0.10.0-mac-arm64-Release@20260821_2215`):

- B2: `duration scalar 0.50 (turbo wait fix ...)`; `Sleep(100)` 50.0–55.0 ms, 47/47 expired 500 ms timeouts at 250.0 ms,
  `[clock-report]` 2.0000 (13/13), 0 late ticks (`pb-turbo`).
- B6: `[clock] exact anchored guest clock (executable override ...)`; `nm -m`: `QueryGuestTickCount` defined in `fable_2`.
- B5: `Restored window position: 247, 484 (before first show)`; poller first bounds `247, 452` (`pb-win`).
- B1: the Metal plugin is source-built in both modes, so the SDK ring sync stays active and the override stands down
  (`[ring-reinit] SDK synchronises ...`).
- B3: the generated tree with `REX_GUEST_*` lines builds against the prebuilt headers (the executable defines the macros).
  The prebuilt `rexglue` ignores `memory_barriers`, so a codegen rerun with it gets the 147 post-pass fences.
- bootloop 20/20, 0 AV boots, 0 skew (`pb-on`).

### Kill-switch finding: `FABLE2_GUEST_BARRIERS=0` is diagnostic-only

One barriers-off boot on the source build (`b3-barriers-off`, session `20261009-144547-b8c73b0d`) AV-stormed 0.1 s
after the boot-save title accept. It wrote into the XEX image (guest `0x820F60F9` on GameThread, `0x82A5E268` on
0xF8000074), then hit `[FATAL] Call to invalid or unregistered function at guest address 0x00000942` and SIGABRT on the
3D Engine. 20 barriers-off boots on the prebuilt runtime were clean (`pb-bar0`), and every barriers-on session today
(about 100, both runtimes) had 0 AVs. That fits why barriers are on by default: the switch is for A/B diagnosis only.
The 1/1 vs 0/20 split is too small to blame or clear the source runtime's timing changes. Two gaps followed from it:

- `f2diag run` verdicts now fail on any `Unhandled guest access violation` or `[FATAL]` line in the log. bootloop rows
  get `verdict: fail` plus `av_lines` / `fatal_lines` / `first_fault`, the summary gets `fails`, and the other plans get
  `verdict: ok|fail`. Before, that boot counted as "pass": it reached the title just before the storm.
- The recorder lost its `first_av` dump to the abort 9 ms later (gap noted in `docs/telemetry-design-20261008.md` §15.8).

### Final validation (source build, all six fixes, first launch after the rebuild counted)

| Check | Result | Phase A source build |
|---|---|---|
| Uncapped Old Tomb sweep (`hero1-dog-live_2026-10-07_14-45-08`, `FABLE2_GPU_TIME_TRACE=1`; `fin-sweep-unc`) | 177.5 FPS, 1% low 87.0, p99 11.0 ms; GPU mean 5.81 ms, p99 10.8 | 175.1 FPS, 1% low 70.2, p99 11.2 ms; GPU mean 5.87 |
| 60 FPS sweep, same save (`fin-sweep60`) | 60.0 FPS, p99 18.2 ms, max 18.7, 0 frames > 33 ms; process 136% of a core | 60.0 FPS, p99 18.4 (`ov-on-60`) |
| Dog fur / artifacts (`f2diag images`) | blue 0; dark 59 camera-motion frames; 2 dips > 1%: a near-camera pillar (`lbclose`) and one `rot+2` frame (8034) with a dark translucent wedge with a straight edge over the left half, gone in the next capture (the same class as phase A's occluder frames; the dog renders normally). **0 dog-fur flashes** | blue 0; dark 52–63; dips 2 / 4 / 2 |
| Area load (`fin-area`) | 416.6 / 717.9 ms | 416 / 717 |
| F5 (`fin-f5`) | saved `hero1-dog-live_2026-10-09_14-57-57` | written |
| AVs | 0 in every final run (verdict ok) | 0 |

## Phase B proposal (approved 2026-10-09)

Each item is a tracked patch in `patches/sdk/`, listed in `SDK_BEHAVIOUR_PATCHES`. Each changes the runtime
fingerprint, so the SDK rebuilds by itself. Rule: **an SDK fix and its executable workaround never both stay active.**
Either remove the workaround in the same change, or make it detect the fixed runtime and switch itself off.

| # | SDK fix | Replaces | Remove the workaround at the same time? | Risk | Validation |
|---|---|---|---|---|---|
| B1 | `CommandProcessor::InitializeRingBuffer` (`src/graphics/command_processor.cpp:314`): reset read and write index together while the worker is quiescent (lock the worker's load-write → execute → store-read step, or hand the reset to the worker), make `read_ptr_index_` atomic | Ring re-init fix: `__imp__VdInitializeRingBuffer` override (`src/core/hotfunc/gpu/RingReinitFix.cpp`) + `fable2_ring_reinit` plugin export | **Mostly no.** `CommandProcessor` is compiled into the Metal plugin from `sdk-source` + `sdk-metal.patch`, not into the runtime, so the fix belongs in `sdk-metal.patch` (plugin) and, for Vulkan, in the runtime tree. With the fix in place, the override's park/finish becomes a no-op; retire it afterwards. | Low–medium: it touches the GPU worker loop | `bootloop --n 50`: 0 hangs, 0 skew; `FABLE2_THROTTLE_TRACE=1`: inserts = callbacks; area + F5 |
| B2 | Turbo: `Clock::ScaleGuestDurationMillis` / `FileTime` / `Timeval` (`src/core/clock.cpp:188–240`) and the `chrono.h` cast divide by the scalar; fix the relative-time `FileTime` sign | `fable2::turbo::SetActive` writing `1/s` into `guest_time_scalar_` (`src/core/fable2_turbo.h`, `FABLE2_TURBO_WAIT_FIX`) | **Yes, never both.** Both together invert twice (`Sleep(100)` → 200 ms at 2x). Drop the scalar overwrite; keep `fable2::guestspeed` | Low | `FABLE2_TURBO_SCALE=2`: `Sleep(100)` ≈ 50 ms, 500 ms timeouts 250 ms, `[clock-report]` 2.0000; 1x unchanged |
| B3 | Codegen: emit `std::atomic_thread_fence` for `sync` (seq_cst) and `lwsync` (acq_rel) on non-x86 (`src/codegen/builders/system.cpp:34–50`), behind a codegen flag | `apply_recomp_patches.py` guest-barrier step + `FABLE2_GUEST_BARRIERS` macro | Keep the post-pass, but make it skip lines that already have a fence (idempotent). Keep the runtime kill switch by making the codegen emit the macro rather than a bare fence | Medium: codegen changes all 597 files (full recompile); fence placement must match the post-pass exactly | Codegen diff = exactly the 147 barrier sites; litmus test; area + sweep; `FABLE2_CRASH_DIAG=1` 0 lines |
| B4 | CR-09: `PosixConditionBase::WaitMultiple` (`src/core/threading_posix.cpp:323–420`) waits on a condition variable instead of yield-spinning and 1 ms `sleep_for`; `AudioSystem::WorkerThreadMain` (`src/audio/audio_system.cpp:105–169`) drops the extra 500 ms idle sleep | Nothing in the executable (impossible before); proposal in `docs/weak-mac-perf-20261008.md` step 5 | n/a | Medium: shared by every multi-object wait | Audio Worker CPU per 100 ms (was 74–83 ms); audio starts after silence; boot + area + sweep FPS |
| B5 | Window placement: let `WindowSDL::OpenImpl` (`src/ui/window_sdl.cpp:136–186`) take an initial position before `SDL_ShowWindow` (or create the window hidden and let the app show it) | `FABLE2_EARLY_WINDOW_RESTORE` SDL event watch (`src/core/fable2_window_position.cpp`) | Yes, or keep the watch as a fallback that finds the window already in place | Low | CGWindowList timeline: first bounds = saved position; `=0` A/B |
| B6 | `patches/sdk/clock-fractional-ticks.patch` (`UpdateGuestClock` carries the fractional ticks) | `src/core/fable2_guest_clock.h` covers only the executable's `mftb` path; runtime callers (KeQuery*, timers) still truncate | Not strictly: the two cover different callers. Once the runtime is fixed, make the executable read the runtime clock (this also removes CR-13's mutex) | Low–medium: guest time base | `[clock-report]` 1.0000 at 1x over 60 s; movie audio; turbo 2.0000 |

Tracked `patches/sdk/` file at the time (it still applies cleanly to the pinned source; not applied in phase A):

- `clock-fractional-ticks.patch`: B6.

Suggested order: B2 (smallest, clearest A/B), then B5, B6, B1, B4, B3. Also candidates once the runtime is source-built:
CR-14 (absolute-deadline waits), CR-01's deferred overlapped delay (currently a thunk swap), and the libusb headers.

## Files

- `tools/build_sdk_from_source.py` (new), `backends/metal/sdk-submodules.json` (new), `backends/metal/sdk-build.patch` (new)
- `tools/build_macos_metal.py` (switch, SDK record in `metal-build.json`, backend reconfigure on SDK change)
- `tools/build_macos.sh` (game reconfigure on SDK change)

Phase B (2026-10-09):

- New: `patches/sdk/clock-duration-scaling.patch` (B2), `patches/sdk/window-initial-position.patch` (B5),
  `patches/sdk/ring-reinit-sync.patch` (B1), `patches/sdk/wait-multiple-blocking.patch` (B4),
  `patches/sdk/codegen-memory-barriers.patch` (B3).
- Changed: `patches/sdk/clock-fractional-ticks.patch` (B6, now applied, plus the anchor fixes), `backends/metal/sdk-metal.patch`
  (B1 hunks appended), `tools/build_sdk_from_source.py` (`SDK_BEHAVIOUR_PATCHES`), `tools/build_macos_metal.py` (automatic
  re-extraction on a patch change; Python 3.9 extraction), `tools/apply_recomp_patches.py` (B3), `tools/f2diag/run.py`
  (AV/FATAL verdict), `fable_2_manifest.toml` (`memory_barriers`).
- Executable: `src/core/fable2_turbo.h`, `fable2_guest_speed.h` (B2), `fable_2_app.h`, `fable2_window_position.{h,cpp}` (B5),
  `fable2_guest_clock.h` (B6), `hotfunc/gpu/RingReinitFix.cpp` (B1), `fable2_guest_memory.h` (B3); plugin
  `backends/metal/port/rex/graphics/metal/metal_graphics_system.cc`, `metal_command_processor.h` (B1).
