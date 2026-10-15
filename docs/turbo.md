# Turbo mode (macOS)

Emulator-style speed-up: the whole game (simulation, animation, AI, timers,
cutscenes) runs faster while the screen stays at ~30 FPS.

## Use

- **F7** or number-row **7** toggles turbo on/off during play (keyboard; F7 works in
  menus and gameplay, 7 is ignored while an overlay captures typing). The SDK
  achievements overlay moved from 7 to 8. Turbo is **off** at every start. A toggle shows "Turbo 2x on" /
  "Turbo 2x off" at the top right for ~2 s and logs a `[turbo]` line (style: see "On-screen notice").
- Speed: `[patches] turbo_scale` in `fable2_config.toml` (state directory,
  `~/Library/Application Support/Fable II Recomp/` for the app). Default 2.0;
  1.1 to 4.0 in 0.1 steps (the SDK quantizes the clock scalar to tenths);
  1.0 disables F7.
- Tests: `FABLE2_TURBO_SCALE=<x>` overrides the speed **and starts turbo on**.
  The background test channel accepts key `f7`
  (`python3 tools/fable2_test_control.py press <session> f7`).
- `FABLE2_CLOCK_REPORT=<seconds>` logs `[clock-report] guest/real ...` lines
  (guest clock rate against real time) for validation.

Known behaviour: audio runs at real-time speed (host callbacks), so music and
speech can drift or gap while turbo is on. Turbo overrides `fps_60`.

## On-screen notice

`FABLE2_TURBO_NOTICE=overlay|game` (Dev Launcher "Turbo notice"; default `overlay`).

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
- `Sleep(1)` under 2x rounds down to 0 ms (a yield), because the SDK converts in whole milliseconds.

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

## Caveats

On the prebuilt runtime, the relative-time branch of `ScaleGuestDurationFileTime` still casts a negative duration to
unsigned before scaling. No caller in the runtime uses it. The source-built runtime fixes it (above).
