# Apple Silicon Metal backend

The default Apple Silicon Metal backend provides native Metal rendering through a ReXGlue
plugin. It requires Apple Silicon and an Xcode installation with the Metal
Toolchain component. See [backend build details](../backends/metal/README.md)
for dependencies, source provenance and licenses.

## Build and launch

Supply the extracted game content described in the [README](../README.md#getting-started), then run:

```sh
python3 tools/build_macos_metal.py
open 'out/build/mac-arm64-metal-release/Fable II.app'
```

The full build generates guest code, builds the converter, helper shaders and
renderer, and creates or refreshes the app. `--prepare-only` and `--backend-only`
do not produce a game app. Existing app output is replaced only after the new
bundle has been assembled and signature-verified.

The app includes its runtime libraries and can be moved to Applications. It
uses the source checkout as the initial game-content location. If that location
moves, the app asks for the folder containing `default.xex` and `data/` and
remembers the selection. Game content is never copied into the app.

For command-line use, `./run_macos_metal.command` starts Metal directly.
`./run_macos.command` requires Metal on Apple Silicon and uses Vulkan on Intel.
A missing Metal build reports an error instead of silently switching renderers. Set `FABLE2_MACOS_BACKEND=metal` to require Metal, or `vulkan` to
select Vulkan. Intel Macs use Vulkan.

## Saves and configuration

The app stores saves in `~/Library/Application Support/Fable II Recomp/saves`,
with settings, logs and caches in the parent directory. Metal and Vulkan apps
share this state and refuse concurrent game instances. Updating the app does
not replace user state.

Command launchers retain separate state beside their executables:

- Metal: `out/build/mac-arm64-metal-release/saves/`
- Vulkan: `out/build/mac-arm64-release/saves/`

There is no automatic save migration or merging. To migrate, close the game and
copy the entire existing `saves` folder into a new app state folder before first
launch. Preserve a backup; do not merge two existing save trees.
`FABLE2_GAME_DATA_ROOT` selects external game content for command launches without
changing their save location.

## Renderer configuration and limitations

The pinned SDK's guest clock dropped the fractional guest tick on every query,
so the scaled clock ran ~0.96x real time and movie audio got periodic silence
padding. Launchers used to pass `--clock_no_scaling=true` as a workaround. Since
2026-10-05 the executable carries the remainder itself
(`src/core/fable2_guest_clock.h`; source-runtime fix in
`patches/sdk/clock-fractional-ticks.patch`), so the launchers no longer pass it
and clock scaling (needed by turbo, see `docs/turbo.md`) stays enabled. A
`clock_no_scaling = true` line in `fable_2.toml` or a `REX_CLOCK_NO_SCALING`
variable is obsolete and ignored at startup; only an explicit
`--clock_no_scaling=true` command-line argument still selects host time.

The Metal launchers enable sequential register batching and two background
shader compilation workers. Constant-payload caching and forced block-compressed
texture expansion are disabled. The `[patches] hero_dog_texture_readback` setting
in `fable2_config.toml` controls morph readback (default `true`); explicit SDK or
command-line settings such as `--metal_fable_morph_readback=false` take priority.

Morph readback copies native-resolution RGBA8 color resolves at guest destination
`0x12704000` into guest RAM after GPU completion. The selection criteria derive
from [Femtofork for Fable II](https://github.com/just-harry/unofficial-xenia-femtofork-for-fable-ii).
The address is game-version-specific; scaled-resolution readback is unsupported.
Background shader compilation waits for the game-specific morph texture shader
when morph readback is enabled, preserving generated hero/dog textures. Other
pending scene draws may be skipped while compiling to avoid gameplay stalls.
Stored shader and pipeline descriptions are prepared before guest execution.
With `metal_background_pipeline_warmup=true` (the default), cached pipelines
compile on the existing workers while opening videos play. A cached pipeline
needed immediately is moved ahead of remaining warm-up work and waited for,
so skipping the logos does not discard its first draws. Previously unseen
shaders retain the normal background-compilation behavior and may briefly
render incompletely. This overlaps startup work; it does not reduce total
compilation work. The measured 300-pipeline cache prepared in 0.04 seconds.

Compiled pipelines are kept between launches. They are stored in a Metal
binary archive next to the `.metal.artifacts` file:
`cache/shaders/local/metal/<GPU>/<abi>/4D5307F1/*.os<macOS version>.metal.segNNNNNN[.full].binarchive`.
Each launch looks pipelines up in the existing segments first. New pipelines
compile in parallel on the workers and go into a new segment. That segment is
saved when warm-up ends, then every
`metal_pipeline_binary_archive_save_interval` seconds (default 30) while new
pipelines keep arriving. Each save writes a temporary file and renames it over
the segment. A saved segment is then closed and later pipelines go to the next
one: Metal fails a second serialize of the same archive ("expecting 'fragment'
stage", CR-05). `FABLE2_ARCHIVE_SAFE_SAVE=0` restores the old single-segment
behaviour. With `FABLE2_NATIVE_SHADERS` on (the default), pipelines with an
optimized pixel shader use their own `.ns1` segments (`docs/native-shaders.md`). Unreadable segments are deleted and their pipelines are rebuilt.
From 8 segments on, a launch writes a `.full` segment, and the older segments
are deleted on the next launch. macOS also keeps its own per-app Metal cache.
With either cache warm, the 337-pipeline warm-up finishes in about 0.5 seconds;
it took about 13 seconds before this change. With both cold it takes about 7
seconds. `--metal_pipeline_binary_archive_save_interval=0` stops these saves.
`--metal_pipeline_binary_archive=false` turns the archive off.

Use `--metal_background_pipeline_warmup=false` to finish all cached compilation
before starting the videos. The native “Loading Fable II” overlay services Cocoa
events during startup preparation or this full blocking warm-up.
Failed geometry pipelines
are retried with backoff. Complete-playthrough coverage is not established.

Metal uses uniform subdivision at the guest maximum tessellation factor for
Fable II's terrain shader (`5003700B7C9B1C16`) to reduce distant ground cracks.
This workaround leaves other tessellated geometry unchanged. It increases
terrain geometry compared with adaptive subdivision; the tested overlook showed
improved ground coverage with no noticeable frame-rate penalty, but other areas
may differ. Use `--metal_fable_terrain_uniform_tessellation=false` to compare
with adaptive subdivision.

The build applies the private SDK graphics patch from `backends/metal/` without
modifying the installed runtime or Windows SDK submodule. Generated-code hooks
in `tools/apply_startup_handoff.py` target a specific startup lock and validate
function shape before patching. The Metal launcher enables that workaround with
`FABLE_STARTUP_LOCK_HANDOFF=1`; it is not a general SDK locking fix. Measured 2026-10-14: with it off the title takes 12–24 s instead of
about 2 s (workaround audit, 2026-10-14). Diagnostic
render-loop cadence logging is opt-in with `FABLE2_FPS_METER=1` and does not
measure displayed frames.

## App packaging and testing

Standalone packaging is available through `python3 tools/package_macos_app.py`.
It creates `out/app/Fable II.app`; `--replace` permits replacing a generated app.
Packaging includes only selected runtime libraries, default settings and an icon.
Lua helpers are loaded from the external content folder's `data/scripts/recomp`.
No game content, saves, caches or generated guest source are packaged.

Apps are ad-hoc signed for local use, not Apple-notarized for distribution.
Their minimum macOS version is derived from the compiled binaries. Packaging
cannot lower that version. Omit `--game-data-root` when packaging for distribution
to avoid embedding a local checkout path.

Run the [Mac regression tests](macos.md#tests-and-current-limits) and the backend's
[renderer tests](../backends/metal/README.md#tests). For a bounded app startup check
using a **new disposable state folder**:

```sh
python3 tests/macos_app_tests.py --app 'out/build/mac-arm64-metal-release/Fable II.app' \
  --state out/validation/fresh-app-state --game-data-root "$PWD" \
  --report out/validation/app-startup.json
```

This requires desktop access, verifies Metal/input initialization, preserves
preexisting settings, checks bundle signatures and immutability, and stops only
the process it starts. Gameplay testing remains a separate manual check.


## Frame-time recordings

The normal Metal app records `frames-<Unix milliseconds>-<PID>.csv` beside its
main log (normally `~/Library/Application Support/Fable II Recomp/logs/`).
`metal_frame_logging` defaults to true; disable it in the Metal settings and
save the config, then restart, to stop future recordings. Each launch creates
a separate file. Nothing changes the game clock, frame cap, or resolution.

The recorder links game swap submissions, guest outputs, and Metal drawable
presentation timestamps with a frame ID. Repeated presentations of the same
output are counted separately from newly presented game frames. A synthetic
capture swap is excluded from FPS statistics. Metal callbacks with zero
`presentedTime` indicate an unpresented/skipped drawable; they do not count as
displayed frames. Invalid timestamps and dropped log events are reported.
Presentation timestamps describe delivery reported by Metal, not GPU execution
time or proof that every submitted frame contains different pixels.

Events are buffered in bounded memory and written on a background thread every
500ms, outside the rendering callbacks. The app hard-exits at shutdown, so the
last roughly half-second may be missing. Recordings are retained until deleted.
The file marks warm-up begin/end and waits for demanded cached pipelines.

From the repository root, summarize the latest normal-app recording:

```sh
python3 tools/summarize_frame_log.py --last 120
# Or select a known gameplay section, in seconds since recorder startup:
python3 tools/summarize_frame_log.py /absolute/path/to/frames-recording.csv --start 90 --duration 120
# Machine-readable report, including the startup period:
python3 tools/summarize_frame_log.py /absolute/path/to/frames-recording.csv --phase all --json
```

The default excludes warm-up, but does not identify gameplay automatically:
opening videos, menus, pauses, and loading screens can still be present. For a
baseline, enter gameplay, follow a repeatable route for at least two minutes,
and quit immediately afterward. Use the final 120 seconds. Repeat the route
after its shaders have been seen, keeping resolution and settings fixed.
The report includes average FPS, p50/p95/p99 and maximum frame intervals, gaps
over 33/50ms, and 1% low FPS (1000 divided by the mean slowest 1% of intervals
in milliseconds). Guest submission timing and unique-frame presentation timing
are both reported; all-presentation rate includes repeats and is not game FPS.

The opening-video smoke test validates recording, not 60-FPS gameplay capability.
CPU/GPU execution profiling and changes to frame pacing follow a gameplay
baseline; this recorder does not enable the SDK performance-counter overlay.


## Present queue (display latency)

With display sync on (the default), the game renders faster than the display takes frames, so CAMetalLayer's
drawable queue stays full: each new frame waits behind every older queued one. The layer's default of 3
drawables means up to two frames of extra delay between the game's swap and the frame reaching the screen.
The presenter sets `maximumDrawableCount = 2` (`metal_presenter.mm`, `PresentDrawableCount()`), so at most one
older frame is ahead. `FABLE2_PRESENT_DRAWABLES` (read once): unset or `2` = 2 drawables (default);
`3` (or `0`, what the launcher passes when the toggle is off) = 3, the old behaviour. Launcher: Settings >
Dev Options > Fixes > Display > "Lower display latency". It applies with tearing allowed too. Code that sets
its own `maximumDrawableCount` (for example frame-interpolation work that presents extra frames) overrides
this and must be measured on its own.

The presenter logs `drawable_wait` rows (time blocked in `nextDrawable`, microseconds) in the frame CSV and the
flight recorder. With display sync, painting runs on the UI thread and waits there for a free drawable about one
refresh per paint with either count (p50 16.6 ms, p99 17 ms in both arms), so 2 drawables add no UI-thread
stall.

Measured 2026-10-22 (M5 Max, 60 Hz display, `--mode library` sessions, camera turn30 +
headings8, 1-3 runs per arm; guest `swap` to Metal `presentedTime`, median per run):

| Config | 2 drawables | 3 drawables |
|---|---|---|
| 1× 60 FPS | 34, 64, 62 ms | 81, 65, 62 ms |
| 1× 30 FPS | 49, 60, 61 ms | 54, 81, 78 ms |
| 1× uncapped | 34 ms | 67 ms |
| 3× 60 FPS | 33, 47, 45 ms | 61, 64, 65 ms |
| 2× uncapped | 36 ms | 52 ms |

The queue part alone (CPU submit to `presentedTime`) is 32 or 49 ms with 2 drawables and 49 or 65 ms with 3:
one refresh less on average (about -16 ms over all runs), quantized per launch. Unique presents/s and frame
gaps did not change with the count. The input-lag study that found this (stick inject to photon) measured
-14 to -17 ms median and about -30 ms p90.

Separate, pre-existing pacing issue seen in both arms: per launch, the guest swap lands at a random but stable
phase against display vsync. When it falls just after a vsync (14.5-16.6 ms before the next one), swap jitter
flips frames between two refreshes: at 60 FPS 10-23% of guest frames are never shown (46-55 unique FPS), at
30 FPS a 1/2/3-refresh judder. It hit 4 of 11 runs with 2 drawables and 5 of 11 with 3. The frame CSV shows
it as `swap->first present_submit` near 0 or near 16 ms per frame. The vsync phase lock (next section) removes it.

## Vsync phase lock

`FABLE2_VSYNC_PHASE_LOCK` (default on since 2026-10-22, `=0` off; launcher: Fixes > Display > "Sync frames to the
display", an always-on fix that presets don't change) fixes the per-launch phase problem above and removes the random wait between a finished guest frame
and the paint that samples it. Code: `backends/metal/port/rex/graphics/metal/metal_vsync_lock.{h,cc}` (controller),
`MetalGraphicsSystem::StartPhaseLockedVblankWorker` (vblank thread), hooks in `metal_presenter.mm`.

How frames reach the screen with display sync: a UI drawer keeps the UI thread painting once per refresh. Each
paint blocks in `nextDrawable` until the display frees a drawable (about 1.1 ms after a vsync), then takes the
newest guest output, and that drawable reaches the screen a fixed 2 refreshes later (`present_submit` to
`presentedTime` 31.5-32 ms at 60 Hz). The guest output becomes ready at a phase set by the guest vblank timer,
which runs free of the display: anywhere from just before a paint (best) to just after one (a whole refresh of
waiting), and near that edge swap jitter flips frames between two paints (dropped frames at 60, judder at 30).

With the lock on, the guest vblank worker is replaced:

- Same guest-tick schedule as the SDK worker (one vblank per 1/60 guest second, 1 kHz with `vsync` off), so the
  game sees the same vblanks at the same average rate. The schedule's phase moves by at most 1 ms per frame
  (0.5 ms once near the target), the drift between the 60 Hz timer and the display included (tens of
  microseconds per frame).
- It sleeps to each vblank deadline with `mach_wait_until` under a time-constraint (real-time) thread policy. The
  SDK worker polls with 1 ms sleeps at a guest-mapped priority; with a precise sleep but that priority its
  vblanks were 13-21 ms apart (timer coalescing); with the policy 16.651-16.677 ms (p1-p99).
- Feedback per new guest frame: the vblank before it, the time its guest GPU work completed (shared-event
  listener on the output's submission), the paint loop's wake phase (drawable acquisition after a blocked
  `nextDrawable`, relative to the display grid from `presentedTime`), and `presentedTime`.
- Target: guest GPU completion lands `margin` (1.5 ms) before a paint wake, using the 95th percentile of
  vblank-to-completion over the last 240 frames (a maximum would let a few loading frames move typical frames onto
  the edge). When the GPU runs a frame behind, the lead grows by that much so the paint never samples a frame
  whose GPU work is still running.
- Without a UI paint loop (frames painted on arrival), the target is a learned latch time before the vsync, and
  a run of frames shown a refresh later than targeted (an extra queued frame, which rate-locked production never
  drains) delays one vblank by a refresh.
- Inactive, with the plain timer at its last phase: no `presentedTime` feedback (occluded window, no window),
  guest clock scaled (turbo, slow mode: their pacing is unchanged), `vsync` off (uncapped), or a frame interval
  that is not a whole number of refreshes. Display refresh comes from `NSScreen.maximumFramesPerSecond` of the
  window's screen, updated when the window changes screen or the screen configuration changes (120 Hz works the
  same way, phase taken modulo the refresh).
- Log: a `[vsync-lock]` line every 5 s (state, mode, frames, misses, lead, phase error, shift).
  `FABLE2_VSYNC_LOCK_TRACE=1` adds per-frame `vlock_*` rows to the frame CSV (vblank ticks, paints, GPU done,
  phase error). Tuning (experiments only): `FABLE2_VSYNC_LOCK_MARGIN_MS`, `FABLE2_VSYNC_LOCK_GAIN`,
  `FABLE2_VSYNC_LOCK_OFFSET_MS`, `FABLE2_VSYNC_LOCK_LATCH_MS`.
- Other presenters (for example a frame-interpolation present thread) can schedule from the same timeline:
  `vsync_lock::DisplayGrid()` (latest `presentedTime` and refresh period), `NextVsyncAfter(t)` and `Lead()`.
  A presenter that picks display times itself reports each real frame with `OnExternalSchedule(frame, arrival,
  deadline, slipped)` (arrival = when it took the frame, deadline = the latest arrival that still makes the vsync
  it targeted) plus `OnPresented`. The lock then runs in `external` mode: the cliff is that deadline's phase and
  `p95(vblank -> arrival) + margin` lands on it. Frame generation (next section) uses this.

Measured 2026-10-22 (M5 Max, 60 Hz external display, `--mode library` background sessions, 2 drawables, 30 s
after gameplay readiness, steady = last 15 s; `out/validation/vsync-lock-20261022/`):

| 5 launches each | unique FPS (steady) | intervals off cadence | guest done/ready -> paint |
|---|---|---|---|
| 60 FPS, lock off | 60.0, 60.0, 43.8, 57.9, 47.1 | 0, 0, 37, 3.7, 27 % | 0.6-15.8 ms (random per launch) |
| 60 FPS, lock on | 60.0, 59.3, 60.0, 60.0, 60.0 | 0-0.2 % | 2.9-3.2 ms |
| 30 FPS, lock off | 30.0 each | 0, 0, 44, 12, 39 % | 0.8-13.0 ms |
| 30 FPS, lock on | 30.0 each | 0 % | 3.4-3.5 ms |

An earlier 5+5 round on the previous build gave the same split (lock 10/10 clean; off: bad phase in 2/5 at 60 and
3/5 at 30). Over both rounds: lock 20/20 launches clean, off 9/20 in a bad phase, so it became the default. Latency (latrun inject -> photon, 2 sessions per arm, n = 15 stick trials each, all frames):
swap -> `present_submit` p50 12.3/13.3 ms off vs 3.3/3.3 ms locked at 60, 12.6/13.4 vs 3.7/3.7 ms at 30, i.e.
about -9 ms on the queueing part this lock controls. The stick inject -> photon medians (182/182 off,
165/182 locked at 60; 215/215 off, 198/232 locked at 30) are dominated by the game's 30 Hz input poll and 15 Hz
tick and need more trials to resolve 9 ms. Other arms: turbo 2x (lock inactive, 30.0 FPS, max interval 33.3
ms), slow 0.5x (inactive, 60.0 FPS, no gaps), uncapped (inactive, 109 swaps/s), 3x resolution at 60 (off: bad
phase, 48.7 unique FPS; locked: 59.8, 0.3 % off cadence). With the window not shown (all `presentedTime` 0) the
lock stays on the timer: 60.07 / 30.0 swaps/s, no stall.

Not covered by the lock, seen in both arms: `present_submit` -> `presentedTime` switches between 32 and 48.5 ms
(2 or 3 refreshes) mid-session, typically once 8-16 s after load, sometimes back and forth; one 16-run batch
had none. Presenter GPU work ends 0.2-6.5 ms after submit either way (a separate paint queue that made it 0.2 ms
did not change it), so it looks like a WindowServer state. Not tested yet: 120 Hz / ProMotion and VRR
displays, moving the window between displays, a minimized window, and a visible, focused game window (these runs are background test windows).

## Frame generation

MetalFX frame interpolation (`MTLFXFrameInterpolator`) shows one generated frame between every two real frames:
the game renders 30 FPS and the display gets 60. Off by default; it changes how motion looks (moving characters can
smear, very fast pans turn soft) and adds about 20 ms of delay, so it is not part of Original or Enhanced.
Research and quality comparison: `docs/frame-interp-prototype.md`.

- **Turning it on:** `FABLE2_FRAME_INTERP=1` (start state; launcher Settings > Display > Frame rate "Frame
  generation", Off / 30 → 60, Custom preset only). **0** or **F10** toggles it live; the controller action
  `FABLE2_PAD_FRAMEGEN` (Controls > Game controllers, default none) does the same. The toggle shows an on/off notice
  in the turbo-notice style and never changes the launcher setting. The F9 overlay shows "framegen" while it is on.
- **Requirements** (checked once at startup, `src/core/fable2_frame_gen.h`): the 30 FPS cap, macOS 26, a GPU for
  which `MTLFXFrameInterpolatorDescriptor supportsDevice` is true, and the Metal renderer. Otherwise it stays off
  with a `[frame-gen] off: ...` log line, 0 or F10 shows "Frame generation needs 30 FPS" (or the other reason) and does
  nothing, and the launcher disables the picker with that reason. At 60 FPS or uncapped the frame rate is left as
  chosen (not forced to 30). 60 → 120 on 120 Hz displays is not offered: untested (only a 60 Hz display here).
- **Turbo pauses it** (only real frames are shown while turbo is on; it resumes when turbo turns off, with no notice
  of its own; the F9 overlay shows "framegen paused"). With it running, turbo dropped 10-37 generated frames a
  minute (the vsync lock does not steer a scaled guest clock), and fast motion is where interpolation looks worst.
  **Slow mode keeps it running** (about 30 real frames per second at the 30 cap, 0-12 drops a minute, measured
  below).
- **How it works** (`backends/metal/port/rex/ui/metal/metal_frame_interp.{h,mm}`, hooks in `metal_presenter.mm`):
  per new guest frame the presenter copies the guest output into a 2-frame ring and runs the interpolator on
  (previous, current) with colour only: zero motion vectors and a flat depth, so MetalFX estimates the motion
  itself and no game data or guest-side hook is needed. The UI thread paints the generated frame and then the real
  one (with the ImGui overlays) into offscreen targets; a present thread copies each into a drawable and presents
  it at an explicit display time on the vsync grid (`presentDrawable:atTime:`): generated on vsync N, real on N+1.
  Repaints of an unchanged frame wait for the next present or new guest frame (overlays update with the game's
  frames while it is on).
- **Pacing rules** (docs/bug-patterns.md rows 112, 113): commit each present at least 2 refreshes + 1.5 ms before
  its vsync (learned up after a late present, down slowly while on time); a pair too late for its slot drops its
  generated frame and keeps the 2-refresh cadence; a second late pair re-anchors the cadence; pairs a refresh early
  skip one generated frame to catch up. The vsync phase lock stays active in `external` mode: the present thread
  reports each pair's arrival and deadline (`OnExternalSchedule`) and the lock moves the guest vblank so pairs
  arrive just before the deadline.
- **Drawables:** 3 while it is on (with 2 only 20 presents/s get through: each drawable is held from commit, 2+
  refreshes ahead, until the next one is shown). Turning it off (F10) drains the present thread's queue and goes
  back to the 2-drawable UI-thread path (`FABLE2_PRESENT_DRAWABLES`).
- **Dev switches:** `FABLE2_FRAME_INTERP_PRESENT_LOG=<csv>` (one row per present: kind, frame, drawable taken,
  target vsync, `presentedTime`, ready, arrival), `FABLE2_FRAME_INTERP_LEAD_MS=<ms>` (initial lead). A
  `[frame-interp]` line every 300 real frames gives counts, drops, GPU time, gaps, latency and the lead.

Measured 2026-10-22 (M5 Max, 60 Hz external display, macOS 27.0.1, `--mode library` background sessions with the
perf lock held exclusively, Bower Lake, 30 FPS cap, 40 s camera pan + 15 s walk, first 2 s of each phase skipped;
`out/validation/frame-gen-20261022/`, `scripts/stats.py`). Latency = real frame reaches the presenter -> on screen
(`presentedTime`), median; "guest ready -> on screen" from the frame CSV is comparable across arms:

| arm | launches | presents/s | intervals off cadence | drops/min | latency | guest ready -> on screen | GPU per real frame |
|---|---|---|---|---|---|---|---|
| off (2 drawables, lock on; same path as before) | 2 | 30.0 (real) | 0-0.08 % | 0 | | 34.9-35.2 ms | |
| on, 1× | 3 | 59.97-60.0 | 0-0.04 % | 0-1.6 | 54.2-55.0 ms | 54.6-54.7 ms | 1.3 ms (max 2.6) |
| on, 2× (2560×1440) | 3 | 59.89-60.0 | 0-0.18 % | 0-6.3 | 56.2-57.1 ms (73 in the late state) | 56.8-73.2 ms | 5.1 ms (max 5.6) |
| on, slow 0.5× (20 s) | 2 | 60.0 | 0 % | 0 | 55-74 ms | | |
| on, turbo 2× (20 s, paused) | 2 | 30.0 real frames while paused, 60.0 after | 0-0.35 % | 0-3.2 | | | |
| (before the pause rule) on, turbo 2× running | 2 | 59.39-59.83 | 0.3-1.0 % | 10-37 | 61-69 ms | | |

- **Switching** (F10 off -> on -> off -> on in one session, 2 launches at 1×, 1 at 2×; turbo pause and resume, 2
  launches): every switch is seamless apart from at most one longer gap: 50 ms (the previous frame shown one refresh
  longer) at most switches, 66.7 ms once at an F10 switch and at each turbo pause (turbo's own frame-interval change
  lands there too). No black frame (the screen timeline never has a gap over 66.7 ms). Pacing after each switch is
  clean: 60.0 presents/s while on, 30.0 real frames/s and 0 % off cadence while off. F10 at 60 FPS logs
  `toggle: Frame generation needs 30 FPS`, shows that notice and changes nothing.
- About +20 ms over off at 1× (half a real frame, 16.7 ms, for showing the generated frame first, plus the longer
  `atTime` lead). The "late state" (73 ms, every present a refresh after its target, no drops) is the WindowServer
  state described under "Vsync phase lock" (2- vs 3-refresh queue); it came and went within sessions.
- GPU cost is the colour copy plus the interpolation, on the presenter queue; the present thread adds one
  drawable-sized copy per present. Expect roughly 3-4× this on a base M1 (unmeasured); at 2× there that is
  15-20 ms per real frame, inside the 33 ms budget only if the game itself leaves room.
- Not tested: 120 Hz / ProMotion (30 → 60 then targets every 4th refresh for real frames), VRR displays, a
  focused foreground window (these are background test windows), base M1/M3 GPUs.

## Stable 30 FPS and app-owned testing

Metal now defaults to `metal_allow_tearing=false`: CAMetalLayer display
synchronization is enabled, preserving the guest's original 30-FPS pacing.
An existing saved `metal_allow_tearing=true` can override the default. The
initial title/video comparison changed presentation from uneven delivery to
33.333/33.334ms between new game frames, with zero skipped presentations.
Gameplay coverage must be recorded separately; a default change alone does
not guarantee every area sustains 30 FPS.

The opt-in test interface starts only when `FABLE2_TEST_CONTROL_DIR` is set to
an absolute path. It is implemented inside the app: input enters its SDL event
loop, captures read the guest render output via the Metal presenter, and close
uses the normal SDL quit path. It needs neither Accessibility nor macOS Screen
Recording access. No TCP listener is opened. The private session folder holds
request/reply files and rendered-frame PNG/PPM captures. Normal Finder launches
leave the interface disabled.

```sh
python3 tools/fable2_test_control.py launch
# Use the absolute session directory returned by launch for subsequent commands:
python3 tools/fable2_test_control.py capture /absolute/session
python3 tools/fable2_test_control.py press /absolute/session e
python3 tools/fable2_test_control.py press /absolute/session down
python3 tools/fable2_test_control.py press /absolute/session w --hold 1000
python3 tools/fable2_test_control.py close /absolute/session
python3 tools/fable2_test_control.py summary /absolute/session --last 120
```

Launch uses the normal app bundle with a copy of current saves, cache, metadata
and settings under `out/validation/gameplay-tests/`. Progress/autosaves from
tests stay in that copy. Window placement comes from the test-window memory
(see "Test-window placement" in `docs/macos.md`), never from or to the normal
app preferences. With `FABLE2_TEST_WINDOW_MEMORY=0` it is inherited, as before,
from the most recently modified `fable2_window.toml` in the normal app
preferences, the selected save/settings source, or a previous test session. Launch refuses to run while another game is open.
The test forces `vsync=true` and display sync on by default; `--display-sync off`
is available for comparisons. Presses release automatically after the bounded
hold duration (20..10000ms). Key names are physical game key mappings; the
current saved map uses E for A and X for B. Inspect each capture before choosing
a menu action, especially before loading a save. A loading screen or cutscene
is not proof of gameplay: verify visible control response to movement. Captures
are synchronous and can perturb timing, so capture before/after measured routes.

`tools/fable2_test_mcp.py` exposes this same interface as a stdio MCP server:
launch, capture (PNG image content), press, close, summary, and ping. Its launch
returns the session directory required by other tools. Run it with Python 3;
no additional dependencies are required. It does not register itself in a
client or change system permissions. The same source API can be used directly
by the CLI in this chat; MCP clients can configure the stdio bridge as a server.

Register it with Codex using absolute paths (the working directory is not
required):

```sh
codex mcp add fable2 -- /usr/bin/python3 /absolute/project/tools/fable2_test_mcp.py
codex mcp get fable2
```

After registration, reload MCP servers in the desktop app's Settings > MCP
servers, or restart the app if the existing chat does not expose the new tools.
Registration alone does not refresh a running chat's tool list. The `capture`
tool returns the PNG inline, avoiding a separate file-viewing tool call.

Each new capture also saves a matching `.capture.json` file. The MCP image
response includes that metadata as JSON text; the path-only CLI remains
compatible with existing callers. Native `frame_submitted_monotonic_us`,
`capture_started_monotonic_us`, and `capture_completed_monotonic_us` use
`CLOCK_MONOTONIC`, the same microsecond clock as the session input journal.
`frame_t_us` and capture `*_t_us` subtract `FABLE2_SESSION_ORIGIN_US`, so they
can be aligned with recorded input times from launch. Without a session origin,
those relative fields are null. The frame timestamp identifies CPU submission
of the selected guest output, not GPU completion or physical display time.
The mailbox remains acquired through readback, keeping pixels and metadata
from the same output. `submission_id` identifies the submitted output;
`frame_id` is the optional frame logger's identifier (zero if unavailable).

Client `request_sent_monotonic_us`, `reply_received_monotonic_us`, and
`image_ready_monotonic_us` track retrieval and PNG conversion independently.
`delivery_and_conversion_us` ends at PNG readiness, before MCP transport or
model interpretation. Older game builds explicitly report
`native_timestamps_available: false`; file timestamps are never substituted.
Restart/reconnect an already running MCP bridge after updating its Python
source to receive the new metadata fields.

A verified navigation example is: skip/confirm title with E, inspect the menu,
select Continue with Down, confirm E, inspect the hero carousel, confirm the
selected copied Hero 4 save, and wait for Bowerstone Old Town to load. Once
the opening movie is visibly playing, press Start once (`escape` with the
current keyboard map) to skip it. In gameplay the same button pauses or opens
a menu, so inspect the output before sending it. Complete the LT tutorial
with `q` if prompted, then verify the gameplay HUD/trail and movement before
starting timing. Never blindly repeat confirm presses or infer gameplay
solely from elapsed time. Allow the startup logos to finish before sending
title/menu input; an early logo skip hit a guest null-read fault in a test.

Recording replay accepts either an explicit `recording_end` event or EOF.
When the game closes without an explicit stop, EOF ends replay at the last
recorded event's timestamp; the reader adds a terminal event in memory and
leaves the source file unchanged. Replay releases held controller inputs at
completion. `replay` accepts the session's frozen `recording.jsonl` or its
native `test-state/control/events.jsonl`, using the session's initial saves.
