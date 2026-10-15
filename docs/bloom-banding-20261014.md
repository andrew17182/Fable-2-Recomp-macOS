# Bloom grating at 2× / 3× (2026-10-14)

Report (playtest, portable 76a148f, M3 Pro, 2×): glowing mushrooms in a dark cave show a stepped, striped
("grating") bloom instead of a smooth glow, and it switches back and forth between smooth and striped.
Evidence: `out/validation/bloom-banding-20261014/` (KEEP; crops, the report screenshot and the scripts used).

## Cause

The cause is not M3-specific, and it is not render-target precision. It reproduces on the M5 Max at 2× (Old Tomb,
the blue glow at the base of the light beam). 1× is smooth, and so are "Full resolution" effects.

`[graphics] effects_native_scale` (default on, `docs/resolution-scale.md` §12) keeps texture-fetch texel offsets
in guest texels, so blur, bloom and glow kernels keep their original width.

At 2× a guest texel is 2×2 host texels (3×3 at 3×), but each tap still reads one host texel. The taps land 2 (3)
host texels apart, so the kernel has holes between them. A thin bright feature (a mushroom stem, the beam's blue
line) reaches an output pixel only when its host-pixel parity matches, so the glow comes out as stripes. Which
pixels match depends on the feature's sub-pixel position, so the pattern appears and disappears as the camera or
the object moves.

With "Full resolution" effects the offsets count host texels: no holes, but the glow is ½ (2×) or ⅓ (3×) as wide.

Where the stripes come from: limiting the fix to textures narrower than the full screen
(`beam-2x-footprint-below-fullres-only.png`) removes them. Limiting it to full-screen textures
(`beam-2x-footprint-fullres-only.png`) leaves them. So they come from the reduced-resolution bloom stages.

## Measurement

The frames come from spin5 sweeps in Old Tomb (save `hero1-dog-live_2026-10-07_14-45-08`) at matching headings.
The profile is the blue channel averaged over 100 columns of the glow above the beam's blue line, over 80 rows.

| Arm | Profile | Local extrema / roughness (mean \|2nd difference\|) | Crop |
|---|---|---|---|
| 2× default (effects original) | ripple, period ≈ 7–8 host px | 21 / 6.8 | `beam-2x-default.png` |
| 2× effects full | smooth, narrower | 4 / 4.0 | `beam-2x-effects-full.png` |
| 1× | smooth | — | `beam-1x.png` |
| 2× with the fix | smooth, original width (2-px plateaus, no ripple) | 36 / 2.6 | `beam-2x-footprint.png` |
| 3× with the fix | smooth | — | `beam-3x-footprint.png` |

The extrema count of the fixed arm is high only because of ±1 flicker between the 2-px plateaus. Its roughness is
the lowest of all arms.

## Fix: `FABLE2_EFFECTS_FOOTPRINT` (default on, `=0` = the old path)

The fix is in the native translator (`metal_msl_translator.cc`, `MslTranslatorConfig::effects_footprint`).

It applies to a pixel-shader 2D fetch with a nonzero texel offset, when:
- offsets are in guest texels;
- the texture is resolution-scaled;
- the texture holds colour data.

Each such tap then covers its whole guest texel along every offset axis:

- **Bilinear fetches** (the game's sampler):
  - At 2× the coordinate moves to the host-texel corner on the kernel-centre side of the tap (floor for positive
    offsets, ceil for negative). At that corner the bilinear filter gives the 2-texel box.
  - Taps already on a corner (2:1 downsampling) stay put, and a symmetric kernel stays centred.
  - At 3× the tap takes two samples at ±⅔ host texel, which together make the 3-texel box.
- **Point fetches on clamp-to-edge textures** (most of the bloom chain): the tap reads the guest texel the point
  sample falls in, averaged exactly by an in-shader bilinear sampler at its centre (one sample at 2×, two at 3×).
  That is the 1× texel value of the box-downsampled image.
- **Excluded:**
  - other point fetches;
  - depth formats and single-channel depth-like formats (`k_24_8`, `k_24_8_FLOAT`, `k_16`, `k_16_FLOAT`,
    `k_32_FLOAT`), i.e. shadow-map PCF taps that the game compares itself.

  These keep one host texel.
- At 2× there is no extra sample: only the coordinate and the sampler change. Fetches that the rule does not apply to
  at run time keep the exact original coordinate.
- The DXBC/converter path is unchanged; native-first covers play.

Launcher: Fixes → Render targets → "Smooth bloom at 2x/3x (no grating)".

## Results (M5 Max)

- **Look:**
  - The beam glow and the green interact glow have no grating.
  - The glow keeps its original width.
- **Parity against `=0`** (Old Tomb still, frame-matched, 2×, 2 runs per arm): the arms differ by design (cross
  1.62–1.75 vs floor 0.06 on / 0.24 off). The differences sit in glow and bloom regions.
- **Stability:**
  - 0 AV and verdict ok in every run (2×, 3×).
  - 0 translator fallbacks and 0 compile failures.
- **First launch after the change:**
  - Ready at 5.6–6.1 s at 2× and 10.6 s at 3×, with one-off MSL recompiles of about 5 s of thread time.
  - It was 5.6 s before the change at 2×.
- **Perf** (Bower Lake 2×, headings8 uncapped, pinned cache, no contention):

| Arm | FPS (runs) | GPU ms (pass trace) |
|---|---|---|
| off (`=0`) | 129.9, 130.3, 131.3, 131.3, 131.8, 132.0, 132.1 | 7.97 |
| on (final) | 122.6, 123.1, 124.0 | 8.64 |
| on, only textures ≥ 1280 guest texels wide | 122.7 | — |
| on, only textures ≥ 640 wide | 122.2, 123.2 | — |
| on, only textures ≤ 1279 wide | 122.6, 124.1 | — |
| code present, never active | 131.1, 132.0 | — |
| earlier variants: 4-sample box, corner snap + box | 118.4–123.5 | 8.78 |

- **Where the cost comes from:**
  - The cost is about 0.6–0.7 ms of GPU time per frame at 2× (−6%).
  - It is not code size (the never-active arm matches off) and barely depends on sample count.
  - The pass trace spreads it over the effect passes: the 640×384 bloom passes, the 2560×1440 composite, the 80×64
    luminance chain and a 560×512 4×MSAA pass. Each is 20–40% slower when it reads guest-texel boxes instead of
    single point texels of fp16 targets.
  - Limiting the fix by texture size does not help: each half alone costs about as much as both.
  - Expect a larger absolute cost on bandwidth-limited Macs (M1/M3).
- **Update (`docs/ssmask-20261014.md`):** most of that cost was footprint code compiled into main-pass and
  texture-space shaders where it never applies (offset PCF on depth or unscaled textures).
  - `FABLE2_EFFECTS_FOOTPRINT_VARIANT` (default on) gives only the draws that can use it the footprint code.
  - Bower 2×: 7.21 → 6.70 ms; footprint off is 6.52.
  - What is left is the footprint's own work in the bloom and composite passes: about 0.18 ms at 2×, 1.0 ms at 3×.
- **Options for the maintainer:**
  - default on (faithful look, −6% at 2× here);
  - off (`=0` / launcher toggle), today's look with the grating.

## Switches

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_EFFECTS_FOOTPRINT` | on | offset fetches cover their guest texel at 2×/3× when effects keep their original size |
