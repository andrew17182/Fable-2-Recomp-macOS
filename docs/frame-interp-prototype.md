# MetalFX frame interpolation prototype (2026-10-22)

Prototype branch only, not merged. `FABLE2_FRAME_INTERP=1` (default off) shows one MetalFX-generated frame between
every two real frames: 30 rendered gives 60 shown. It uses `MTLFXFrameInterpolator` (macOS 26 MetalFX), which
`supportsDevice` reports on the M5 Max (macOS 27.0.1). Code: `backends/metal/port/rex/ui/metal/metal_frame_interp.{h,mm}`,
plus hooks in `metal_presenter.mm` and `metal_command_processor.cc`.

## How it works

- **Presenter, per new guest frame:** copy the guest output into a 3-frame colour ring, then run the interpolator
  (previous, current) into a generated texture. This runs in its own command buffer, which waits on the guest
  output copy.
- **Pacing:** the UI thread paints into offscreen targets. A present thread acquires each drawable, copies the
  target in and presents it: first the generated frame, then the real one, each at least 0.75 x half the measured
  real interval after the previous present. Painting both on the UI thread did not work on a 60 Hz display. Two
  presents per frame keep every drawable queued, so `nextDrawable` blocked the UI thread for about 40 ms per frame
  and guest frames were dropped (20 FPS real). Repaints of an unchanged frame wait for the next present instead of
  spinning. While interpolating, `maximumDrawableCount` is 2 (`FABLE2_FRAME_INTERP_DRAWABLES=3` restores 3): a third
  drawable only adds a refresh of latency.
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
- **Pacing in steady play:** 30 real + 30 generated frames per second, every gap 16.67 ms (±0.00). There are short
  drops or hitches only while the test dumps PNGs.
- **Latency** (real frame handled by the presenter → on screen, `presentedTime`):
  - 1×: 79 ms with interpolation (2 drawables; 94 ms with 3), against a 48.7 ms baseline.
  - 2×: 65 ms against a 65 ms baseline.
  - In the baseline, the UI thread also waits up to 16.6 ms in `nextDrawable` before it handles the frame. That
    makes the added latency about 15-35 ms, close to the unavoidable half real frame (16.7 ms at 30 FPS).
- **Not tested:** 60 → 120. This display is 60 Hz only (`NSScreen.maximumFramesPerSecond` = 60). On a 60 Hz panel
  a 60 FPS cap with interpolation would need 120 presents/s; the present queue drops generated frames instead.

## Switches

| variable | effect |
|---|---|
| `FABLE2_FRAME_INTERP=1` | interpolate (default off); `stats` = latency/pacing stats only |
| `FABLE2_FRAME_INTERP_INPUTS=color\|depth\|camera` | interpolator inputs (default `color`) |
| `FABLE2_FRAME_INTERP_DRAWABLES=2\|3` | drawables while interpolating (default 2) |
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
