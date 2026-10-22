# Turbo and slow mode (macOS)

Emulator-style speed-up: the whole game (simulation, animation, AI, timers,
cutscenes) runs faster while the screen stays at ~30 FPS.

## Use

- Number-row **7** or **F8** toggles turbo on/off at the **Turbo speed** (keyboard; F8 works in menus and
  gameplay, 7 is ignored while an overlay captures typing). It turns off from any speed (so 7 after 2 returns to
  1x). On MacBooks the F keys need fn (or the macOS setting "Use F1, F2, etc. keys as standard function keys"), so
  the number row is the main set; the F keys mirror it one to the right (` = F1 … 0 = F11). Turbo is **off** at
  every start. A toggle shows "Turbo 2x on" / "Turbo 2x off" at the top right for ~2 s and logs a `[turbo]` line
  (style: see "On-screen notice").
- Speed keys (see "Speed keys"): **`** slow, **1** normal, **2** / **3** / **4** for 2x / 3x / 4x.
- Controller: Settings > Controls > Game controllers > **Turbo** puts the same toggle on View, Share or a
  stick click (`FABLE2_PAD_TURBO=none|view|share|ls|rs`, default view: the View button; docs/reference.md "Controller hotkeys").
- Speed: launcher front page → **Turbo speed** (slider and stepper, Off or 1.1× to 4.0×
  in 0.1 steps, default 2.0×), passed as `FABLE2_TURBO_SPEED=<x>`. Off (1.0) is the Original preset: 7/F8, the
  speed keys 2-4 and the controller button do nothing (2-4 show "Turbo is off in settings"); **Slow speed** Off
  (`FABLE2_SLOW_SPEED=1.00`) does the same for ` and the slow button ("Slow is off in settings")
  (docs/reference.md "Presets"). Without a launcher:
  `[patches] turbo_scale` in `fable2_config.toml` (state directory,
  `~/Library/Application Support/Fable II Recomp/` for the app). Default 2.0;
  1.1 to 4.0 in 0.1 steps (the SDK quantizes the clock scalar to tenths);
  1.0 disables turbo. `FABLE2_TURBO_SPEED` overrides the TOML and does not start turbo on.
- Audio: launcher front page → **Turbo audio** (`FABLE2_TURBO_AUDIO`, `[patches] turbo_audio`):
  "Sped up, natural pitch" (`natural`, default), "Sped up, high pitch" (`high-pitch`) or
  "Normal speed" (`normal`, the old behaviour). See "Turbo audio" below.
- Tests: `FABLE2_TURBO_SCALE=<x>` overrides the speed **and starts turbo on**.
  The background test channel accepts the keys (`7`, `f8`, `grave`, `1`-`4`, `f1`-`f5`;
  `python3 tools/fable2_test_control.py press <session> 7`).
- `FABLE2_CLOCK_REPORT=<seconds>` logs `[clock-report] guest/real ...` lines
  (guest clock rate against real time) for validation.

Turbo overrides `fps_60`.

## Speed keys (2026-10-22)

Keys that set a speed instead of toggling one (`fable2::turbo::SetSpeed`, src/core/fable2_turbo.h):

| Key | F key | Speed |
|---|---|---|
| ` | F1 | slow, at the **Slow speed** (default 0.5x) |
| 1 | F2 | normal (1x): turbo and slow off |
| 2 | F3 | 2x |
| 3 | F4 | 3x |
| 4 | F5 | 4x |

- 2, 3 and 4 ignore the Turbo speed slider (fixed 2.0, 3.0, 4.0). A speed already in effect does nothing (2 at
  2x, 1 at 1x).
- State model: one mode (off, turbo, slow) plus the turbo speed, which is the slider's unless a speed key chose
  2/3/4. 7/F8 toggles: on = turbo at the slider speed, off from any turbo speed. Turbo audio, frame pacing
  (`FrameInterval`: 2 x speed vblanks), the frame-generation pause and the FPS counter ("turbo 3x") follow the
  speed in use.
- Notices: "Speed 2x", "Normal speed", "Slow 0.5x". With turbo or slow disabled in settings (speed 1.0, the
  Original preset) the keys for it only show "Turbo is off in settings" / "Slow is off in settings".
- Number keys are ignored while an overlay captures typing; the F keys always work.
- Checked 2026-10-22 (test channel, `FABLE2_CLOCK_REPORT=1`): ` 1 2 2 3 4 7 7 2 7 then F1-F5, F8, F2 gave
  `[clock-report]` guest/real 0.5000, 1.0000, 2.0000, (2 again: no change), 3.0000, 4.0000, 1.0000 (7 off),
  2.0000 (7 on), 2.0000 (2: no change), 1.0000, and the same through the F keys.

## Slow mode (2026-10-21)

Turbo below 1x: the whole game runs slower while the screen keeps its frame rate.

- **`** or **F1** sets it (` is ignored while an overlay captures typing; **1** / **F2** returns to 1x). Off at every
  start. Notice "Slow 0.5x", FPS counter field `slow 0.5x`. Controller: Settings > Controls > Game controllers >
  **Slow motion** toggles it (`FABLE2_PAD_SLOW=view|share|ls|rs`, default none; notice "Slow 0.5x on/off"). Test
  channel keys `grave`, `f1`. (Until 2026-10-22, 8/F8 toggled slow mode; 8 is achievements now.)
- Speed: launcher front page **Slow speed** (0.25× to 0.9× in 0.05 steps, default 0.5×; `FABLE2_SLOW_SPEED`), or
  `[patches] slow_scale` (same range; 1.0 disables slow mode). `FABLE2_SLOW_SCALE=<x>` also starts it on (tests).
- Turbo and slow mode are one speed setting (`fable2::turbo::SetMode`): turning one on turns the other off; 7/F8 in
  slow mode goes straight to turbo.
- Clock: the same `set_guest_time_scalar`, below 1. The SDK kept the scalar as tenths above 1 and as
  `10 / trunc(10 / s)` below 1, so 0.75 ran at 0.769 and 0.9 at 0.909;
  `patches/sdk/clock-scalar-precision.patch` makes it `llround(s x 1000) / 1000` for every scalar (turbo's tenths
  reduce to the same ratio as before). Guest sleeps and waits get longer (`Sleep(1)` at 0.5x = 2 ms, still in
  microseconds, see "Precise sleeps"); nothing truncates, and a millisecond duration that would pass `UINT32_MAX`
  saturates to infinite, as before.
- Frames: vblanks follow the guest clock, so at 0.5x a 60 cap (one vblank per frame) would show 30 FPS. At a cap
  (`vsync` on), the frame-dispatch hook (`fable2_hook_fps_60`) asks for an immediate present (interval 0, the
  game's own `PRESENT_INTERVAL_IMMEDIATE` value) and paces frames by real time to 60 (or 30 with `fps_60` off):
  `SlowPaceFrame`, a kqueue timer sleep on the guest render thread, no spin; a frame later than one period
  re-anchors. Uncapped (`vsync` off: 1 kHz guest vblanks) runs as at 1x.
- Audio: the turbo audio stage at rate s < 1 (`natural` = AUNewTimePitch, slower at the same pitch;
  `high-pitch` = AUVarispeed, slower and lower, rate clamped to its 0.25 minimum; `normal` = 1x, the old path).
  The guest credit stays at the 1x base (`audio_maxqframes` x s x headroom is below it). Turbo -> slow drops the
  turbo backlog beyond the new credit, as turning turbo off does.

**Measured 2026-10-21** (Bower Lake dog save, muted background test sessions, 2x resolution, M5 Max, perf lock, no
contention; 15 s still then 6 s walking with a camera turn; process CPU in % of one core; frame interval = guest
swaps):

| Arm | Clock (guest/real, every 5 s window) | FPS still / move | p99 / max frame (ms) | CPU still / move | Audio (5 s windows) |
|---|---|---|---|---|---|
| 1x, 60 cap | 1.0000 | 60.0 / 60.0 | 18.3 / 19.1 | 119 / 120 | direct |
| 0.75x, 60 cap | 0.7500 | 60.0 / 60.0 | 16.9 / 18.3 | 119 / 122 | 0.750x in and out, 0 starved |
| 0.5x, 60 cap | 0.5000 | 60.0 / 60.0 | 16.9 / 17.2 | 111 / 114 | 0.500x, 0 starved |
| 0.25x, 60 cap | 0.2500 | 60.0 / 60.0 | 17.0 / 17.4 | 105 / 107 | 0.250x, 0 starved |
| 0.5x, 30 cap | 0.5000 | 30.0 / 30.0 | 33.6 / 33.7 | 72 / 72 | 0.500x, 0 starved |
| 1x, uncapped | 1.0000 | 81.0 / 81.3 | 13.4 / 14.4 | 152 / 156 | direct |
| 0.5x, uncapped | 0.5000 | 80.6 / 80.9 | 13.1 / 13.3 | 145 / 136 | 0.500x, 0 starved |
| 0.25x, uncapped | 0.2500 | 79.8 / 80.7 | 13.3 / 14.0 | 138 / 139 | 0.250x, 0 starved |

Pacing at the cap is steadier than at 1x (p95 16.8 against 17.9 ms), because the frame waits on a real-time
deadline instead of the vblank thread's 1 ms sleep loop. Toggles (F8 -> F7 -> F8 -> F8 through the test channel,
60 cap): 60.0 / 30.0 (turbo, clock 2.0000) / 60.0 / 60.0 FPS, guest ticks continuous across each change, no
silent chunks. Evidence: `out/validation/f2diag/slow-mode-20261021/` (one `arm.json` per arm). Not checked yet: by
ear on speakers, a pad bound to Slow motion (the dispatch path is the turbo one).

## Turbo audio

**Why audio lagged.** The guest's audio engine is pulled by the host: the SDK's `AudioSystem` worker runs the
game's audio callback (one 256-sample, 5.1 frame each time) only when the SDL device callback has played a
frame and released a credit on the client semaphore (8 credits, `audio_maxqframes`). The device plays 48,000
samples per real second, so the guest's audio clock ran at 1x whatever the guest clock did. There was no
growing backlog (the credit bounds it), but at 2x speech and music played at half the game's speed: lines
overlapped or were cut off, and music lagged scene changes.

**Fix** (`patches/sdk/audio-playback-rate.patch`, the SDK output stage; the game calls
`rex::audio::SetPlaybackRate(rate, mode)` from `fable2::turbo::SetActive`):

- At rate r the device callback consumes guest frames r times faster than real time and squeezes them into
  real time with an Apple audio unit rendered offline inside the callback (no extra device, no AUGraph):
  - `natural`: **AUNewTimePitch** (`kNewTimePitchParam_Rate` = r): faster, same pitch;
  - `high-pitch`: **AUVarispeed** (`kVarispeedParam_PlaybackRate` = r): resampled, faster and higher.
  The unit pulls its input through a render callback straight from the guest frame queue.
- The guest's credit grows to `audio_maxqframes` x r x `audio_rate_headroom` (default 2; 8 → 32 at 2x, 64 at
  4x, capped at 64, the semaphore maximum). The buffered real time is constant for a given rate, so latency does not
  grow while turbo runs; it is about twice the 1x buffer (~85 ms at 2x and 4x, plus the AUNewTimePitch latency of
  85 ms). The headroom absorbs short stalls of the guest's audio worker, which competes with a game running r times
  faster: with headroom 1 a 4x run starved about 1% of its input.
- Rate trims (`drift` and `unit rate` in the stats line):
  - drift: AUNewTimePitch adapts to the content and does not pull exactly r x its output. Offline, with a
    recorded game capture as input, it pulled 3.85–3.99x per 5 s window at rate 4 and 1.96–2.01x at rate 2; with a
    sine wave it was exact. AUVarispeed is exact. The stage compares the input pulled since switch-on with r x output
    and pulls up to 8% faster, so the guest's audio clock stays with its game clock. The error is held within ±100 ms,
    so it cannot wind up.
  - starvation: when the guest's audio worker can't keep up (seen at 4x while another game ran on the same Mac:
    up to 20% of the input missing), a queue below a quarter of the credit slows the pull by up to 20% instead
    of feeding the unit silence. With natural pitch that is slightly slower speech; with high pitch also a lower pitch.
- Toggle on: the 1x path fades out over one 5.3 ms chunk, the unit starts clean (`AudioUnitReset`) and fades
  in. Toggle off: the unit's last chunk fades out, the extra credit becomes debt (consumed frames repay it
  instead of releasing), the frames queued beyond the base credit are dropped (they were produced at the turbo
  rate and would otherwise play late), and the direct path fades in. Back at 1x the queue holds the usual 8 frames.
- Rate 1, `normal`, or a unit that fails to load (logged, `[audio-rate] ... unavailable`): the callback runs the
  original direct path. Its output is the same as before the patch (same conversion, same frame order); the
  stage adds two atomic loads per callback.
- Drift (preservation note): turbo is opt-in and off at every start, so none of this touches 1x play. With
  `natural` the time-stretch changes the sound (phase-vocoder artefacts, especially at 3–4x); `normal` keeps
  the exact old output during turbo.

Diagnostics (SDK cvars, pass with `--game-arg=` in test sessions):

- `--audio_rate_headroom=<x>` (default 2) scales the turbo credit (1 = the 1x buffered time).
- `--audio_rate_stats=<s>` logs `[audio-rate] stats` every s seconds: path, rate, guest frames produced and
  consumed per real second (as a multiple of real time), output rate, queue depth now/min/avg/max, credit, debt,
  unit input starvation, silent (underrun) chunks, drift and the trimmed unit rate.
- `--audio_capture=<file.wav>` writes the device output (32-bit float WAV, before `audio_mute`, so muted test
  sessions still record it). Diagnostics only; the file grows ~23 MB/min.
- Log lines at each toggle: `[audio-rate] on: AUNewTimePitch rate 2.00, guest credit 8 -> 32 frames` and
  `[audio-rate] off: direct path, guest credit 32 -> 8 frames, dropped N queued frames ...`.

### Validation 2026-10-15 (muted background test sessions, dog save, 6-channel endpoint)

`--audio_rate_stats=5`, F7 through the test channel. Output folder `out/validation/turbo-audio-20261015/`
(per-run `audio-rate.log`, `capture.wav`; listening clips `clip-2x-natural-on-off.wav`, `clip-4x-highpitch-on.wav`).

| Run | Turbo window | Guest audio produced / consumed (x real) | Queue (frames) | Starved | After F7 off |
| --- | --- | --- | --- | --- | --- |
| 2x natural | 30 s | 2.000 / 2.000, output 1.000 | 24–32 of 32, avg 29 | 0 | 1.000x, 5–8 of 8, within one 5 s window |
| 4x natural | 60 s | 4.000 / 4.000 (unit pulled 4.000 x output) | 52–64 of 64, avg 59 | 0 | 1.000x, 5–8 of 8 |
| 4x high pitch | 25 s | 4.000 / 4.000 | 52–64 of 64 | 0 | 1.000x |
| 2x normal | 30 s | 1.000 / 1.000, direct path, credit 8 | 5–8 of 8 | 0 | unchanged |

- Toggles: the captures show no sample jumps or silent 50 ms windows at the on/off points (only at startup before
  the first guest audio). Off drops the turbo backlog (128 ms of guest audio at 2x, 299 ms at 4x) under the fade.
- Earlier 4x runs, before the headroom and trims, starved 1–20% of the unit's input. The 20% case was while another agent game shared the Mac.
- The units' own cost isn't measured separately. One AU render per 5.3 ms chunk on the SDL audio thread.
- Not checked yet: listening by ear on real speakers, a stereo-only output device (the units are created
  for the device's channel count; on failure the log says so and audio stays at 1x).

## On-screen notice

`FABLE2_TURBO_NOTICE=overlay|game` (Fable II Launcher "Turbo notice"; default `overlay`).

- **overlay** (Fable style, `src/core/fable2_turbo_notice.h`): cream text in the game's own
  UI font with a soft shadow, on a feathered dark band with a thin gold rule, fading in/out
  over ~200 ms. It sits right-aligned just under the game's own top banner, and the text is
  sized like the game's text (32 px glyph cell per 720 px of window height). The font is
  `data/language/en-uk/fonts/gui_large.bft`, read from the user's data/ at runtime and decoded
  in memory by `src/core/fable2_bft_font.h`. Nothing from the game is stored in the repo. If the
  file can't be loaded, the notice uses the ImGui font in the same style. Test-control captures
  show only the guest frame, so the overlay can't be captured there; see
  `out/validation/turbo-notice-20261007/overlay-reconstruction-*.png` for an offline render
  that uses the same drawing math and the decoded atlas.
- **game** (experimental): asks the game to show the text in its own info box (see
  "In-game messages" below). If the game declines (not in normal gameplay) or doesn't
  answer within 1.5 s, the notice falls back to the overlay.

### Game UI font (.bft), format notes

Bitmap fonts in `data/language/<culture>/fonts/*.bft` (en-uk `gui_large/normal/small` = 32/27/24 px;
also `language/fonts/timesnewroman_*`, `couriernew_16`). All values are big-endian:
25-byte header (version 1, pixel size, line height, ascent, glyph count N), N x 22-byte glyph
records (f32 u0 v0 u1 v1, i16 x_offset width advance), a u16 first/last pair followed by a
codepoint -> glyph map (0x3434 = none; en-uk covers up to U+2122), then a 0x58-byte texture
header (+0x10 width, +0x14 height) and a BC4 (DXT5A) alpha atlas in Xbox 360 tiled layout
with 8-in-16 byte swap.

## In-game messages (reusable entry point)

`src/core/fable2_game_message.h`: `fable2::gamemsg::Request(text, lifetime_s, ticket)`, then
poll `fable2::gamemsg::Poll(ticket)` to get `kPending/kShown/kDeclined/kFailed`. It is callable
from any thread, and no guest code runs on the caller's thread.

1. The host writes `return [==[text]==], lifetime` into
   `data/scripts/recomp/recomp_notify_text.lua`, padded to exactly 512 bytes so the size the
   VFS indexed at boot never changes. Text is limited to 400 bytes, and `]` becomes `)`.
2. `fable2::f5lua::request_script("scripts/recomp/recomp_notify.lua")` queues a run. The next
   MainRenderLoop frame calls the game's `CScriptManager::RunScript` on the guest main thread
   (the F6 mechanism in `src/core/fable2_f5_lua.h`). Generic requests always get a frame of
   their own; F5/F6 take priority.
3. `src/lua/recomp_notify.lua` gates first: hero present, no screen fade or loading, and
   `GUI.IsUIActive()` false. It then calls
   `GUI.DisplayInfoBoxParams({ShowAButton=false, ShowYButton=false, LifeTime=n,
   DisplayBoxStyle=EDisplayBoxStyle.DBS_INFO_BOX}, text)` and acks by `RunScript`-ing one of
   the no-op files `recomp_notify_shown.lua` or `recomp_notify_declined.lua`. The RunScript
   dispatcher probe counts those acks. `recomp_notify_mark.lua` marks progress between calls,
   logged as `[notify] ack` in `fable2_f5_lua.log`.

All of these scripts live in `src/lua/` and are staged by the build into `data/scripts/recomp/`
and `dir.manifest`. To show a different message, call `Request` with any string; no new
script is needed.

**Status (2026-10-07): the plumbing works, but the message has not been seen on screen.**
- The whole chain runs synchronously inside RunScript, and every call returns.
  `DisplayInfoBoxParams` accepts literal text without a Lua error, so the script acks "shown".
- In four test states, no box became visible: Thag's Cave, with the tutorial banner or the
  quest-objective banner up, and Bower Lake, with the objective banner up. In Bower Lake the
  objective banner disappeared for a moment right after the call and then came back. Our box
  most likely queues behind the game's own top banner, or a literal string doesn't render in
  that box style.
- "shown" therefore only means accepted by the API, not displayed. No Lua query we know of
  says whether an info box is active or queued (none of `GUI.IsUIActive`,
  `IsScreenFading` or `IsScreenFullyFaded` covers it), so the overlay fallback can't yet
  detect the queued case.
- Safety: 1 of 4 game-mode runs ended in a null-read AV storm (`read of guest 0x00000000` on
  guest thread 0xF800002C), about 12 s after the request. The session is
  `out/dev-sessions/20261007-170802-166cd0fa`; log rotation lost the first AV. The same
  signature occurred in a 2026-10-05 session that predates this code (thread 0xF800006C), and
  three later game-mode runs with `FABLE2_CRASH_DIAG=1` and 2-3 requests each were clean.
  Treat it as unproven either way.
- Next steps: find the native info-box queue or active-box state (the HUD message manager
  behind `GUI.DisplayInfoBox`) to gate on "nothing else showing", or try a box style or API
  that bypasses the queue. Test where no objective banner is up, e.g. after the objective
  banner times out in a town.

## How it works

- Every guest time source goes through the SDK clock (`rex::chrono::Clock`):
  `mftb` (`REX_QUERY_TIMEBASE` -> `Clock::QueryGuestTickCount`, 653 sites),
  the GPU vblank timer, kernel system time and timed waits.
  `Clock::set_guest_time_scalar(scale)` changes the guest/host tick ratio
  (`src/core/fable2_turbo.h`).
- The frame-dispatch hook (`fable2_hook_fps_60`, guest 0x82B9C8E8) waits
  `2 x scale` vblanks per frame while turbo is on, keeping ~30 real FPS.
- A toggle only changes the ratio; the accumulated guest tick count is kept,
  so the guest clock never jumps (log line shows guest ticks before/after).

## Guest clock fix (prerequisite)

The pinned SDK's `UpdateGuestClock` converts every host-tick delta with
truncating integer math. On Apple Silicon (24 MHz host, 50 MHz guest, ratio
25/12) with very frequent `mftb` polling the scaled clock ran ~0.96x real
time, which starved movie audio. Launchers therefore used
`--clock_no_scaling=true`, which also disables `set_guest_time_scalar`.

The loaded runtime is the prebuilt ReXGlue 0.10.0 `librexruntime.dylib`, so
the fix lives in the executable (`src/core/fable2_guest_clock.h`): a strong
`Clock::QueryGuestTickCount` that all recompiled code binds to (verified with
`nm -m`). It updates the SDK's exported clock state under `tick_mutex_` and
computes the count exactly against an anchor (`AnchoredClock` in
`src/core/fable2_clock_math.h`). The anchor also re-absorbs the losses of
callers that still use the SDK path inside the dylib (a per-call remainder
alone still dipped to ~0.988x during loading). `FABLE2_CLOCK_SDK_ROUNDING=1`
restores the SDK path for A/B tests.

**Source-built runtime (2026-10-09, phase B6 of `docs/sdk-from-source.md`).** The runtime now carries the fractional
ticks itself (`patches/sdk/clock-fractional-ticks.patch`, applied by `tools/build_sdk_from_source.py`). The patch also
fixes two things that only mattered once every caller used the SDK path:

- `last_host_tick_count_` no longer moves backwards. A thread that read the host clock before another thread's update
  could take the lock after it and rewind the anchor, so the next update counted that interval twice. With only the
  remainder fix the clock read 2.0003–2.0008 at 2x (cumulative 2.0005 over 65 s); with this fix it reads 2.0000.
- A ratio change (turbo toggle) first counts the time since the last update at the old ratio.

It defines `REX_CLOCK_EXACT_GUEST_TICKS` in `clock.h`. With that macro `src/core/fable2_guest_clock.h` compiles its
override out, so every caller, including the 653 recompiled `mftb` sites, binds to the runtime's
`Clock::QueryGuestTickCount` (`nm -m fable_2`: undefined, from `librexruntime`). The log reads
`[clock] SDK guest clock carries fractional ticks (runtime fix; ratio 25/12)`. On the prebuilt runtime
(`FABLE2_SDK_FROM_SOURCE=0`) the override is compiled in as before, and `FABLE2_CLOCK_SDK_ROUNDING=1` still works there.

Validation (source build, area runs with `FABLE2_CLOCK_REPORT=5`): `[clock-report]` 1.0000 in 13/13 windows at 1x,
loading included (cumulative 1.0000 over 65 s), and 2.0000 in 13/13 at 2x (cumulative 2.0000 over 65 s).

The runtime path still takes `tick_mutex_` on every call (`try_lock`, then a blocking `lock` under contention), like the
old override, so CR-13 is unchanged. It has only moved into the SDK.

Consequences:
- Launchers (`tools/macos/AppLauncher.m`, `run_macos_metal.command`) no longer
  pass `--clock_no_scaling=true`.
- A `clock_no_scaling = true` line in `fable_2.toml` (older setups) or
  `REX_CLOCK_NO_SCALING` is obsolete and ignored at startup with a `[clock]`
  log line; the file is not modified. Only an explicit
  `--clock_no_scaling=true` command-line argument still selects host time
  (and blocks turbo).

Tests: `tests/clock_math_tests.cpp` (CMake target `fable_2_clock_math_tests`,
or `clang++ -std=c++20 -O2 -Isrc/core tests/clock_math_tests.cpp`). Exact
1.000000 rate at 1x/1.5x/2x/3x over 50M small deltas (SDK rounding:
0.990-0.997 in the same test), no overflow on huge deltas, exactness while
interleaved with lossy SDK-path updates, continuity across ratio changes.

## Validation 2026-10-05 (muted background, dog save)

| Session | Setup | Guest/real clock | FPS | Notes |
| --- | --- | --- | --- | --- |
| `20261005-150238-67ec4217` | 1x, then F7 on/off | 1.0000 every 2 s window incl. loading; 2.0000 while on; 1.0000 after | 30.0 throughout | ticks continuous across toggles; 2x walk reached the bridge |
| `20261005-150419-94574851` | `FABLE2_TURBO_SCALE=2` | 2.0000 (22/22 windows) | 30.0 | walk ~2x vs 1x |
| `20261005-150514-f7612df6` | `FABLE2_TURBO_SCALE=3` | 3.0000 (22/22 windows) | 30.0 | no crash; walk blocked at the same bridge spot as 2x |

Before the fix (scaling enabled) the clock ran ~0.960x. Not yet checked: audio
by ear, the on-screen notice (test captures show the guest frame only, not the
ImGui overlay), a real F7 key press.

## Turbo wait fix (2026-10-08)

**Bug.** The prebuilt SDK uses `guest_time_scalar_` the wrong way round for durations.
`set_guest_time_scalar(s)` speeds the guest tick ratio up by `s`, but `Clock::ScaleGuestDurationMillis` and
`ScaleGuestDurationFileTime` (`clock.cpp:188–223`) and the `XSystemClock → WinSystemClock` cast (`chrono.h`) *multiply* a guest
duration by `s` to get host time. At 2x, a guest `Sleep(100)` took 203 ms instead of 50, `Sleep(10)` took 22 ms, and 500 ms timeouts
took 1000 ms (measured, `docs/archive/area-load-profile-20261008.md`). Everything the guest paces with sleeps or timed waits ran at a quarter
of the intended speed relative to guest time.

The callers live inside `librexruntime`:

- `XThread::Delay` (`KeDelayExecutionThread`);
- `XObject::Wait`, `WaitMultiple` and `SignalAndWait` timeouts;
- `XTimer` due times and periods;
- the `xam_net` select timeouts.

Overriding the two functions in the executable would not bind for those calls, and at the time the build linked the
downloaded v0.10.0 release, so no SDK patch could reach the runtime. (Since 2026-10-09 the runtime is source-built and
the SDK has the real fix; see "SDK fix" below. The workaround stands down there.)

**Fix** (`src/core/fable2_turbo.h`, default on, `FABLE2_TURBO_WAIT_FIX=0` restores the SDK behaviour):

- After `set_guest_time_scalar(s)`, `fable2::turbo::SetActive` overwrites the exported `rex::chrono::guest_time_scalar_` with `1/s`.
- Nothing else reads that variable. The tick ratio is computed once inside `set_guest_time_scalar`, and the guest clock
  (`fable2_guest_clock.h`), system time and uptime all come from the tick count. So the guest clock keeps running at `s`.
- Code that needs the guest speed reads `fable2::guestspeed::Get()` (`src/core/fable2_guest_speed.h`), not `Clock::guest_time_scalar()`
  (which now returns `1/s`). That covers the `FABLE2_GAME_TICK_SLEEP` next-tick math and the turbo and clock-report log lines.
- `Sleep(1)` under 2x rounds down to 0 ms (a yield), because the SDK converts in whole milliseconds (fixed since
  2026-10-21, see "Precise sleeps" below).

**Validation** (`FABLE2_TURBO_SCALE=2`; sessions `out/validation/area-load-20261008/tb1`, `tb2`):

- `[clock-report]` reads guest/real **2.0000** in every 5 s window over 25 s.
- `Sleep(100)` took 50.7–54 ms. The mean of 4,642 `Sleep(10)` calls was 5.9 ms. Every expired 500 ms VFS timeout took **250.00 ms**.
- `[game-tick-sleep]`: 0 of 1,075 and 0 of 1,200 sleeps woke after the tick, with a maximum overshoot of 93 µs. The sample computes the next tick at
  16,666 µs, which is 1/30 of a guest second at 2x.
- The save notice under turbo computed its remaining time from the guest timebase, with no 2x stretch.

## SDK fix (2026-10-09, phase B2 of `docs/sdk-from-source.md`)

The Metal build now compiles the runtime from source, so the real fix is in the SDK:
`patches/sdk/clock-duration-scaling.patch` (applied by `tools/build_sdk_from_source.py`).

- `ScaleGuestDurationMillis`, `ScaleGuestDurationFileTime` and `ScaleGuestDurationTimeval` divide a guest duration by the
  scalar. The `XSystemClock -> WinSystemClock` cast divides, and the reverse cast multiplies.
- The relative (negative) `FileTime` branch now scales a signed value. `Timeval` scales the whole duration, so the
  fraction of a second is kept. At scalar 1.0 the behaviour is unchanged.
- `Clock::guest_time_scalar()` returns the guest speed again (2.0 at 2x). `fable2::guestspeed` stays the source of the
  guest speed in our code.

**Workaround retirement.** `fable2::turbo::SetActive` probes `Clock::ScaleGuestDurationMillis(1000)` right after
`set_guest_time_scalar(s)` and before any overwrite. The fixed runtime returns 1000/s and the prebuilt one 1000·s. On
the fixed runtime the `1/s` overwrite is skipped, and the log reads `duration scalar 2.00 (SDK divides durations; turbo
wait fix not needed)`. On the prebuilt runtime (`FABLE2_SDK_FROM_SOURCE=0`) the overwrite still applies, as before.
`FABLE2_TURBO_WAIT_FIX=0` only matters on the prebuilt runtime. Both fixes together would invert the scalar twice
(`Sleep(100)` → 200 ms at 2x), and the probe rules that out.

**Validation** (source build; `f2diag run area b2-turbo` with `FABLE2_TURBO_SCALE=2`, `FABLE2_SAVE_FAST_MOUNT=0`,
`FABLE2_LOADING_TRACE=1`, `FABLE2_CLOCK_REPORT=5`, `FABLE2_GPU_TIME_TRACE=1`; first launch after the rebuild):

- `Sleep(100)` 50.1–59.5 ms (mean 53.4, n = 10). `Sleep(10)` mean 6.45 ms (n = 10,497, min 5.01).
- Every expired 500 ms timeout took 250.0–250.5 ms (n = 37).
- `[clock-report]` 2.0000 in all 13 windows. `[game-tick-sleep]`: 0 late ticks in every 10 s window, maximum overshoot 338 µs.
- 0 AVs.

## Precise sleeps (2026-10-21)

**Bug.** `XThread::Delay`, the `XObject` wait timeouts (`Wait`, `WaitMultiple`, `SignalAndWait`) and the `XTimer`
period converted the guest duration to whole milliseconds, then divided by the turbo speed and truncated again. At
any turbo speed a guest `Sleep(1)` became 0 ms, a yield. Five Fable II worker threads sleep 1 ms in a loop, so each
one spun a core: about 580% process CPU at 2x/60 against about 120% at 1x, at the same frame rate. A 1 ms wait
timeout became a poll, and a 1 ms timer period became 0 (a one-shot timer).

**Fix** (`patches/sdk/turbo-fine-delay.patch`; `FABLE2_TURBO_FINE_DELAY`, default on, `=0` restores the old path;
launcher Fixes > Turbo: precise sleeps). Under turbo these durations are scaled in microseconds: a 1 ms sleep at
2x takes 500 µs. 0 stays a poll, an infinite timeout stays infinite, and anything else is at least 1 µs. Waits use
new `rex::thread::WaitMicros` / `SignalAndWaitMicros` / `WaitMultipleMicros` entry points, and timer periods are
microseconds. At 1x every caller takes the old millisecond path unchanged. Unit test:
`tests/turbo_fine_delay_tests.cpp` (runs with the build).

**Measured** (Bower Lake dog save, muted background test sessions, 20 s still then 8 s walking with a camera turn,
M5 Max, no contention; process CPU in % of one core):

| Arm | Before (still / move) | After (still / move) | FPS after |
|---|---|---|---|
| Turbo off, 60 cap | 118 / 117 | 117 / 121 | 60 |
| 2x, 60 cap | 581 / 576 | 96 / 100 | 30 |
| 4x, 60 cap | 606 / 571 | 136 / 149 | 30 |
| 2x, uncapped | 655 / 616 | 178 / 180 | 79 / 81 |
| 4x, uncapped | 684 / 665 | 221 / 234 | 80 / 85 |
| 2x, 60 cap, `FABLE2_TURBO_FINE_DELAY=0` | | 581 / 575 | 30 |

The five spinning workers drop from ~99% each to under 3%. `[clock-report]` reads exactly 2.0000 / 4.0000 in every
window. Turbo audio keeps pace (guest audio produced and consumed at the turbo rate, output 1.000x). The only
starvation is in the windows where `sample` pauses the process, and it is lower than before. FPS and frame
pacing do not change (the uncapped move window before the fix fell to 49 FPS at 2x; now 81). Evidence:
`out/validation/turbo-cpu-20261021/` (before) and `out/validation/turbo-fine-delay-20261021/` (this patch).

## Caveats

On the prebuilt runtime, the relative-time branch of `ScaleGuestDurationFileTime` still casts a negative duration to
unsigned before scaling. No caller in the runtime uses it. The source-built runtime fixes it (above).
