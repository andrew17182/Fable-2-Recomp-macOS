# MetalFX frame interpolation prototype (2026-10-22)

Research record. The shipped feature is "Frame generation" in `docs/macos-metal.md` (colour inputs and grid pacing
only, `FABLE2_FRAME_INTERP=1`, launcher Settings > Display). Everything else below (depth and camera inputs, the
ground-truth evaluation, frame dumps, legacy pacing, `=stats`, the drawable-count switch) existed only in the
prototype commits 6d0b174 and 7ead964 and was dropped at the merge; check those commits out to reproduce it.

The prototype showed one MetalFX-generated frame between every two real frames: 30 rendered gives 60 shown. It uses
`MTLFXFrameInterpolator` (macOS 26 MetalFX), which `supportsDevice` reports on the M5 Max (macOS 27.0.1).

## How it works

- **Presenter, per new guest frame:** copy the guest output into a 3-frame colour ring, then run the interpolator
  (previous, current) into a generated texture. This runs in its own command buffer, which waits on the guest
  output copy.
- **Pacing:** the UI thread paints into offscreen targets. A present thread acquires each drawable, copies the
  target in and presents it. Painting both on the UI thread did not work on a 60 Hz display (two presents per
  frame kept every drawable queued, `nextDrawable` blocked the UI thread about 40 ms per frame, 20 FPS real).
  Repaints of an unchanged frame wait for the next present (or the next guest frame) instead of spinning.
  - **Grid pacing (default since 2026-10-22, "Vsync-locked pacing" below):** every present gets an explicit display
    time on the display's vsync grid (`presentDrawable:atTime:`): the generated frame on vsync N, the real one on
    N+1 (30 -> 60 on 60 Hz; N + k/2 for k refreshes per real frame). The vsync phase lock stays active and moves the
    guest vblank so each pair arrives just before its deadline. 3 drawables.
  - **Legacy pacing** (`FABLE2_FRAME_INTERP_SCHED=legacy`, the first prototype): generated then real, each at least
    0.75 x half the measured real interval after the previous present (`afterMinimumDuration`), on the present
    thread's own clock. 2 drawables. It hitches periodically (below).
- **Inputs** (`FABLE2_FRAME_INTERP_INPUTS`):
  - `color` (default): zero motion and a constant depth. MetalFX estimates the motion itself, and no game data is
    used.
  - `depth`: zero motion plus the game's scene depth.
  - `camera`: camera-reprojection motion vectors plus scene depth. The guest-side hooks run only in `depth` and
    `camera` modes:
    - Fable II draws its scene at **1120x720, 2xMSAA, reversed Z** (viewport z scale -1, offset 1). The frontbuffer
      stretches it to 1280x720.
    - The camera view-projection is the most common VS `c0..c3` matrix among depth-tested draws into the full scene
      viewport. `c0..c3` are the clip-space rows, `oPos = c0..c3 · pos`, and world-space geometry has an identity
      world matrix.
    - Scene depth is copied at the game's own full-screen depth resolve after the z prepass. The colour pass later
      clears depth in its resolves, so the swap is too late.
    - Float24 host depth holds the guest window z × 0.5. This was found by fitting reprojection against
      block-matched motion; after the fix the vectors match block matching to sub-pixel accuracy.
    - Moving characters and the HUD get camera motion, so they are wrong by construction.

## Quality (Bower Lake, `out/validation/frame-interp-20261022/`)

Ground-truth check (`FABLE2_FRAME_INTERP_EVAL=2`): the game runs at 60 FPS, frame N-2 → N is interpolated (the
same 33 ms step as 30 → 60), and the result is compared with the real N-1. A second interpolator with other inputs
runs on the same frames. Metric: mean absolute error (0-255) of 4×4-box-averaged colour over the scene region (rows
110-560 of 1280×720, which excludes the quest text and d-pad). Averaged per phase:

| phase | color | camera | repeat previous | 50/50 blend |
|---|---|---|---|---|
| pan (fast) | **7.32** | 9.29 | 12.46 | 10.36 |
| slow pan | **2.55** | 2.71 | 3.50 | 2.74 |
| still | **0.41** | 0.42 | 0.50 | 0.53 |
| walk (hero + dog) | **5.13** | 5.83 | 7.31 | 5.81 |

`color` vs `depth` in the same way: 7.12 vs 8.15 (pan), 4.87 vs 5.10 (walk). Sanity arms: negated camera vectors
score 12.5, about the same as repeating the previous frame, and half-length vectors are worse. So MetalFX does use
the vectors, and their sign and scale are right.

What it looks like (`artifacts/`):

- **Slow and medium pans:** both modes look like a real frame (`modes-slowpan-*`, `pan-prev-gen-next.png`,
  `pan-30fps-vs-interp60-halfspeed.gif`).
- **HUD:** `color` keeps the quest text and icons sharp. `camera` moves them with the scene and smears them
  (`hud-truth-color-camera.png`). This is the most visible difference.
- **Characters:** the dog and the hero's legs ghost in both modes, worse with `camera` (`walk-dog-*`,
  `walk-dog-*.gif`). Breadcrumb sparkles break up.
- **Very fast pans** (hundreds of pixels per real frame): both fail (`modes-fastpan-*`). `color` turns to
  double-image mush. `camera` stays sharper but lands short of the truth. Tree bark shows dark speckles in `color`
  (`pan-detail-prev-gen-next.png`).

The box-averaged error favours soft results. Read it together with the images.

## Cost, pacing, latency (M5 Max, 60 Hz external display, 30 FPS cap)

- **GPU per real frame** (colour copy + interpolation, presenter command buffer): about 1.1-1.5 ms at 1× (1280×720
  output), 3.5-4.0 ms at 2× (2560×1440), 4.8-6.5 ms at 3× (3840×2160). The present thread adds one drawable-sized
  copy per present. Expect roughly 3-4× this on a base M1 (unmeasured).
- **Pacing in steady play (legacy pacing, first measurement):** 30 real + 30 generated frames per second, every gap
  16.67 ms (±0.00). There are short drops or hitches only while the test dumps PNGs. See "Vsync-locked pacing" for
  the current numbers.
- **Latency** (real frame handled by the presenter → on screen, `presentedTime`):
  - 1×: 79 ms with interpolation (2 drawables; 94 ms with 3), against a 48.7 ms baseline.
  - 2×: 65 ms against a 65 ms baseline.
  - In the baseline, the UI thread also waits up to 16.6 ms in `nextDrawable` before it handles the frame. That
    makes the added latency about 15-35 ms, close to the unavoidable half real frame (16.7 ms at 30 FPS).
- **Not tested:** 60 → 120. This display is 60 Hz only (`NSScreen.maximumFramesPerSecond` = 60). On a 60 Hz panel
  a 60 FPS cap with interpolation would need 120 presents/s; the present queue drops generated frames instead.

## Vsync-locked pacing (2026-10-22)

A playtest of the first prototype showed it run smoothly for 0.5-1 s, then drop 1-2 frames, repeatedly. The present thread paced
presents on its own clock (`afterMinimumDuration`), the guest 30 Hz timer ran free of the display, and a repaint of
an unchanged frame slept until the next present, so a new guest frame waited up to a refresh before it was
interpolated (a per-launch phase, like the one the vsync phase lock fixes for normal play).

Grid pacing, the default now:

- **Display times:** every present gets one with `presentDrawable:atTime:` on the display grid (`presentedTime` of
  earlier presents; refresh from `vsync_lock::DisplayGrid()`). Per real frame the generated frame goes on vsync N and
  the real one on N+1. N is the next slot of a fixed 2-refresh cadence. A pair too late for its slot but in time for
  N+1 drops its generated frame and keeps the cadence (one refresh of the previous frame repeated). A second late
  pair in a row re-anchors the cadence a refresh later: the lock aligns arrivals modulo one refresh only, so a
  launch can settle with pairs a refresh off the 2-refresh cadence (seen once: every generated frame dropped).
  Pairs that keep arriving a refresh earlier than needed skip one generated frame to move the cadence earlier
  (backlog recovery, with back-off).
- **Lead:** WindowServer needs a present a little over two refreshes before its display time with `atTime`
  (32 ms lead: a refresh late; 33.3+: on time). The lead starts at 2 refreshes + 1.5 ms, grows 1 ms after a late
  present with a lead near it, and shrinks 0.25 ms per 10 s on time (not below 2 refreshes). Some launches need about
  2.5 refreshes (the WindowServer state in docs/macos-metal.md "Vsync phase lock"): those run smoothly with every
  present a refresh after its target (74 ms latency in 1 of 8 launches). Chasing that with a longer lead starved
  the drawables (stuck at 30 FPS), so it is not done.
- **Drawables: 3.** With 2, a drawable is held from commit (2+ refreshes before display) until the next one is shown,
  so the present thread hands over only 20 presents/s (measured: 30 unique/s, 136 ms latency).
- **Vsync lock:** stays active. The present thread reports each pair (`OnExternalSchedule`: arrival = the UI
  thread's submit, deadline = slot minus lead, minus 2 ms for arrival jitter) and the real frame's `presentedTime`;
  the lock runs in `external` mode and puts arrivals just before the deadline. The UI thread wakes on each new guest
  frame (`NotifyNewGuestFrame`) instead of at the next present.

Measured (M5 Max, 60 Hz display, `--mode library` background test windows with `presentedTime` (screen unlocked),
Bower Lake, 30 FPS cap, 60 s camera pan + 20 s walk, first 2 s of each phase skipped;
`out/validation/frame-interp-vsync-20261022/`, `scripts/stats.py`). Latency = real frame handled by the presenter ->
on screen (`presentedTime`), median per launch:

| arm | launches | unique presents/s | intervals off 16.7 ms | drops/min | longest gap | latency (pan) |
|---|---|---|---|---|---|---|
| plain 30 FPS (lock on, no interpolation) | 2 | 30.0 | 0-0.06 % (vs 33.3) | 0 | 33.3 (one 50) | 31.5 ms |
| old: legacy pacing, lock off (first prototype; 2 launches of its actual binary) | 7 + 2 | 60.0 | 0 % | 0 | 16.7 ms | 64-96 ms (per launch) |
| legacy pacing + vsync lock | 3 | 59.98-60.0 | 0-0.03 % | 0-1 | 16.7-33.3 ms | 41-43 ms |
| new: grid pacing (final build) | 4 | 60.0 | 0 % | 0 | 16.7 ms | 54.8-55.8 ms |
| new, previous round (same scheduling rules) | 4 | 59.98-60.0 | 0-0.03 % | 0-1 | 16.7-33.3 ms | 54.7-74.2 ms |
| new with 2 drawables | 1 | 30.0 | 100 % | 1800 | 33.3 ms | 137 ms |

- **The hitch did not reproduce in test windows:** 9 launches of the old pacing (2 with the old binary) had no
  steady-state drop, and the guest timer drifted by only ~0.06 ms per 5 s against this display. Its latency moved
  by launch (64-96 ms), which is the per-launch phase effect. What differs in the playtest session is not known (a
  focused, frontmost window, sound, physical input); please compare "new pacing" and "old pacing" in the app.
- **Latency:** grid pacing adds ~23 ms over plain 30 FPS with the lock (55 vs 31.5 ms; the generated frame is shown
  half a real frame before the real one, 16.7 ms, plus the longer `atTime` lead) and is 10-40 ms below the old
  pacing. Legacy pacing with the lock on is ~13 ms lower still (41-43 ms): an ASAP present reaches the screen
  ~25 ms after commit, `atTime` needs 33+ ms. It is in the app as "old pacing + frame sync".
- Not tested: 120 Hz / ProMotion (k = 4 refreshes per real frame: generated on N, real on N+2), a focused
  foreground window, very long sessions.

## Switches (prototype commits only)

| variable | effect |
|---|---|
| `FABLE2_FRAME_INTERP=1` | interpolate (default off); `stats` = latency/pacing stats only |
| `FABLE2_FRAME_INTERP_INPUTS=color\|depth\|camera` | interpolator inputs (default `color`) |
| `FABLE2_FRAME_INTERP_DRAWABLES=2\|3` | drawables while interpolating (default 3 with grid pacing, 2 with legacy) |
| `FABLE2_FRAME_INTERP_SCHED=legacy` | the first prototype's pacing (min-duration presents on the present thread's clock) |
| `FABLE2_FRAME_INTERP_LEAD_MS=<ms>` | grid pacing: initial commit -> display lead (default 2 refreshes + 1.5 ms; learned from late presents) |
| `FABLE2_FRAME_INTERP_AT_OFFSET=<f>` | grid pacing: `atTime` is the target vsync minus f refreshes (default 0.1; experiments) |
| `FABLE2_FRAME_INTERP_PRESENT_LOG=<file>` | CSV row per present: kind, frame, drawable taken, target, presentedTime, ready, arrival |
| `FABLE2_FRAME_INTERP_DUMP=<dir>` | write a count to `<dir>/trigger` to dump that many frames (real, generated, motion, depth) |
| `FABLE2_FRAME_INTERP_EVAL=1\|2` | ground-truth mode (needs DUMP): interpolate N-2 → N and dump the real N-1; 2 = also a B arm with `FABLE2_FRAME_INTERP_EVAL_B` inputs |
| `FABLE2_TRACE_VSC=*:...` | the existing VS-constant trace for every vertex shader (`*`), now also logging viewport, VTE and depth control |

`[frame-interp]` log lines every 150 real frames give real and generated counts, drops, interpolation GPU time,
the gap before each present, latency, and `nextDrawable` waits.

## What it would take to go further

- **HUD split:** not needed for `color`. For `camera` vectors, the HUD needs its own texture (MetalFX `uiTexture`)
  or zero motion where it is drawn. The composite and HUD draws did not show in the per-draw trace window (the
  frontbuffer is not written by a 1280×720 resolve in the traced frame), so that needs more tracing first.
- **Hybrid:** camera vectors for the static world (sharper on very fast pans) with zero motion on HUD and
  character pixels. That needs a mask: skinned-draw stencil or HUD-pass tracking.
- **Productising:** a launcher toggle, pausing interpolation in menus or cutscenes if they artefact, and a check on
  120 Hz panels (30 → 60 then has 2× slack; 60 → 120 becomes possible).
