# Native alias clears (2026-10-13)

Branch `agent-3/20261013-1510-alias-clear` (from `perf-staging`), merged into `perf-staging`. Switch
`FABLE2_ALIAS_CLEAR_NATIVE` (default on; Dev Launcher → Fixes → Render targets → "Native alias clears"; `=0` draws
the clears as before). Evidence: `out/validation/alias-clear-20261013/` (`arm.sh`, `parity.txt`, `m3est.txt`,
per-run `gpu.txt`/`passes-*.csv`, `cmp/`). The goal came from `docs/hle-census-20261013.md`: on the M3 Pro at 2×, about
4.3 ms of 27 ms went to "clear quads at the 4×MSAA alias" and "main depth at the 4×MSAA alias".

## What the game does

Every clear goes through the Xbox 360 Direct3D `Clear`. It is a `DRAW_INDX_2` rectangle list (builder `821EF988`)
with a fixed shader pair:

- VS `1E6883FCCDE1F688`: screen-space `(x, y, z, 1)` and a per-vertex colour from fetch constant 95, stride 7 dwords;
- PS `A4A965C189287B99` (`oC0 = colour`), or no pixel shader for depth/stencil-only clears.

The state is VTE scaling off, clip disabled, D3D pixel centres, overwrite blend and depth/stencil ALWAYS + REPLACE.
To clear about 4× faster, it splits the clear into two draws:

1. **the aliased rectangle** (call site `lr 821EF938`): the whole tiles of the surface, drawn with `RB_SURFACE_INFO`
   set to **4×MSAA**, at a quarter of the pixels. Every sample gets the same value.
2. **a strip at the real MSAA level** (`lr 821EF8CC`): the columns or rows that do not fill a 4× tile. For example,
   a 256×256 1× target is cleared by a 4× rectangle of 120×128 (240×256 samples) plus a 16×256 strip at 1×.

After many clears the XDK also emits **24 point draws** (VS `B6C9863F710683EC`, `oPos = (0,0,0,1)`, no PS, colour
mask 0, depth/stencil off, no memexport). They bind, write and export nothing. Occlusion queries in this port report a
fixed sample count, so draws never feed them.

Emulated literally (xenia's EDRAM model), each aliased rectangle:

- creates a 4×MSAA host target;
- transfers the surface's previous owners into it;
- draws one quad in its own render pass;
- transfers the result back to the 1×/2× target in the next pass.

The 1× strip lands in another pass of its own, because the 4× pass sits between it and the real pass.

### Patterns per frame (Bower Lake, `FABLE2_ALIAS_CLEAR_TRACE=1`, frame 600–700)

| # | Surface (real key → alias) | Clear | Count | Read afterwards by | Today |
|---|---|---|---|---|---|
| P1 | texture-space 256² 8888 @0t + D24FS8 @64t, `<4t>` 1× → 4× | colour 0, depth 0, stencil 0 | 12–14 | the texture-space pass at 1×, resolved 256/128/64/32 | **native** |
| P2 | shadow cascade 1024² D24FS8 @0t `<13t>` 1× native → 4× | depth 0, stencil 0 | 2 | shadow depth draws, depth resolve | **native** |
| P3 | 256² D24FS8 `<4t>` (small scene depth) | depth 0 | 1 | depth draws | **native** |
| P5 | quarter-res 280×180 8888 @0t + D24FS8 @48t/@630t | colour (1,1,1,0) or 0, depth 0 | 2 | quarter-res post | **native** |
| P6 | main depth 1120×720 D24FS8 @1008t 2× → 4× (+24 no-op points) | depth 0, stencil 0 | 1 | depth prepass and main pass at 2× | **native** (whole target) |
| P10 | half-res 560×360 colour @0t through a **D24S8 alias at 4×** (colour cleared by writing depth) | z ≈ −3.7e-9 → word 0 | 1–2 | half-res post (8888, 10:10:10:2) | **native** (zero word, cross-format) |
| P11 | 128² 8888 @0t `<2t>` through a D24S8 4× alias | word 0 | 4 | small post chain | **native** (zero word, cross-format) |
| P7 | 0t `<14t>` D24S8 at 4×, z = 0.75 | depth word read later as **8888 colour** (shadow-mask target) | 1 | 1× 8888 via a depth→colour transfer | draw (value not exact, owner is another format) |
| P8 | 630t `<14t>` D24S8 at 4× | **stencil only** (zc 8701) | 2 | screen-space shadow mask | draw |
| P12 | 256² @4×MSAA small scene (a real 4× target, two rectangles) | depth 0, colour (0,0,0,1) | 2 | the 4× small-scene pass | draw (no other MSAA variant) |
| — | 1× strips and other 1× clears | | ~20–25 | | draw (they now share the next pass's encoder) |
| — | no-op points | | ~390 | nothing | **skipped** |

Tomb has the same families (fewer texture-space blocks): 11 native clears per frame, 6 of them cross-format zero
clears, and 256 no-op points.

GPU time of the census roles (M5 Max, pass trace, marginal busy ms/frame, off → on):

| Scale | "clear quads at 4×MSAA alias" | "main depth at 4×MSAA alias" | texture-space passes (the strips' encoders) |
|---|---|---|---|
| 1× Bower | 0.77 → 0.03 | 0.21 → 0.15 | 1.17 → 0.98 |
| 2× Bower | 0.87 → 0.05 | 0.58 → 0.37 | 1.30 → 1.08 |
| 3× Bower | 1.42 → 0.09 | 1.19 → 0.85 | 1.56 → 1.19 |

The main-depth remainder is P7 and P8.

## Change

`MetalCommandProcessor::TryAliasClearDraw` (`metal_command_processor.cc`) runs before the render-target update of
each draw:

- **No-op skip.** No colour write after normalisation, no depth/stencil enable and no memexport means the draw has no
  effect. The render-target cache would bind nothing for it either.
- **Alias-clear recognition.** It takes over a draw only when all of these hold:
  - the VS hash is the XDK clear's, the PS is the XDK clear PS or none, it is a 3-vertex rectangle list with auto
    index, no index offset and no memexport;
  - the surface is aliased: `RB_SURFACE_INFO` MSAA is 2× or 4×. A 1× clear is an ordinary draw and stays one;
  - fixed-function state: VTE scale/offset off with screen-space xy/z, `clip_disable` and no user planes, no cull,
    poly mode or depth bias, D3D pixel centres, AA mask all samples, window offset 0. Alpha test and alpha-to-mask
    must be off when a PS runs (with no PS they do nothing);
  - geometry: the three vertices are distinct corners of an axis-aligned rectangle on half-pixel edges, inside the
    pitch and both scissors. z and colour are identical in all three vertices;
  - depth/stencil: either untouched, or z write ALWAYS plus stencil ALWAYS/REPLACE with write mask 0xFF, so the whole
    32-bit word is written. The value must be exact: z = 0 (or z < 0, which the draw path's depth clamp makes 0), or
    z = 1 for D24S8;
  - colour: render target 0 only, all four channels, ONE/ZERO/ADD blend, a 32bpp non-gamma format. Either all zero
    (bit-exact +0), or 8888 / 10:10:10:2 with every channel on an exact step and exponent bias 0;
  - colour and depth do not overlap in EDRAM.
- **`MetalRenderTargetCache::TryAliasClear`** (`metal_render_target_cache.cc`) does the clear on the surface's
  **existing** MSAA variant:
  - Which variant: the owner of the surface's first tile when that owner has the same base, pitch and format at
    another MSAA level. Otherwise, the only other MSAA variant created so far. For an all-zero word, any 32bpp owner
    of the same base and pitch is accepted, colour or depth, because zero is zero in every format.
  - It never creates the aliased target. With no variant it returns false and the draw is drawn as before.
  - The rectangle is mapped through EDRAM samples (4×: 2×2 samples per pixel, 2×: 1×2, 1×: 1×1, identical tiles of
    80×16 samples). It must start at the origin and be a multiple of 8 pixels.
  - It then calls the same `PrepareHostRenderTargetsResolveClear` a resolve clear uses: the same ownership change,
    with transfers only for partly covered tiles of other owners, and the native-scale variant if that owns the
    tiles.
  - The clear goes into the `FABLE2_DEFERRED_CLEARS` queue (§14 of `docs/resolution-scale.md`). It is drawn first
    inside the next pass that binds the target, or flushed as a separate clear pass before any resolve or transfer
    reads it. With transfers, or with deferred clears off, it is encoded at once.
  - The prepared-draw queue is flushed first, as before a resolve.

### Why the bytes are the same

The aliased draw writes one 32-bit word W into every sample of a tile range. EDRAM tiles are 80×16 samples in every
32bpp format and MSAA level, so any key with the same base and pitch reads W wherever it reads that range:

- **Host values.** The resolve-clear path converts W into exactly the host value the quad produced:
  - depth: 0 is 0.0 in both formats (D24FS8 is stored at half range); 1.0 is `UNorm24To32(0xFFFFFF)`;
  - stencil: the reference value;
  - colour: an exact unorm step, written and read back through the same RGBA8/RGB10A2 attachment;
  - zero: zero bits in every format.
- **Ownership.** For every tile outside the rectangle, ownership and contents are what the draw path would have left.
  Tiles fully inside it now belong to a target that holds W. Later transfers, resolves and draws read the same
  bytes; the 4× intermediate is never observed.
- **Kept cases.** Where that argument does not hold, the draw stays as before: z = 0.75 read back as colour, a
  stencil-only clear, partial channel masks, gamma and 64bpp formats, no existing variant.

## Results

M5 Max, headings8 uncapped, `FABLE2_GPU_TIME_TRACE=1`, `f2diag gpu` window 13–33 s, one run per arm. The off and on
arms of each row ran on the same build (`FABLE2_ALIAS_CLEAR_NATIVE=0` vs default). Every run: verdict ok, 0 AV,
contention 0. The first launch after each build was kept; the merged build's first launch prepared Metal in 0.75 s.

| Run | FPS off → on | GPU mean ms off → on | p95 | render encoders / frame | draw encoders with ≤ 2 draws |
|---|---|---|---|---|---|
| Bower 1× | 117.5 → 130.3 | 8.05 → 6.65 (**−1.41**) | 9.89 → 8.21 | 135 → 101 | 109 → 67 |
| Bower 2× | 92.6 → 110.2 | 10.48 → 8.70 (**−1.79**) | 12.51 → 10.59 | 136 → 103 | 110 → 67 |
| Bower 3× | 63.3 → 78.9 | 15.40 → 12.34 (**−3.05**) | 17.71 → 14.18 | 136 → 102 | 110 → 68 |
| Old Tomb 2× | 133.0 → 148.7 | 7.58 → 6.89 (**−0.69**) | 11.93 → 11.55 | 67 → 58 | 48 → 35 |

All passes per frame (render, compute and blit) at Bower 2×: 287 → 235. An earlier build without the cross-format
zero clears gave 10.50 → 8.97 ms at Bower 2×.

**M3 Pro estimate** (`m3est.txt`, the census model: the M5 1×/2× GPU split into fixed and per-pixel cost, scaled by
the census factors fitted to the M3 Pro's measured 15.7 / 27.0 ms):

- The off arm models to 15.7 / 27.5 ms (measured 15.7 / 27.0).
- The on arm models to **13.0 ms at 1× and 23.0 ms at 2×**, about **−4.5 ms at 2×** (−2.7 ms at 1×).
- That is in line with the census's 4.3 ms for these two roles. Most of the saving is per-pass fixed cost (encoders,
  load/store), which is why it holds at 1× too. This is an estimate, not measured on an M3 Pro.

**Bowerstone:** the census's "town" save (`hero2-childhood`) did not reach gameplay in these harness runs. It stayed
on the in-engine title scene at 30 FPS (GPU 1.5 ms both arms), so it gave no perf number. It did give the
byte-identical parity pairs below.

### Correctness

spin5 at 60 FPS with captures, 1× and 2×, on vs off, for Bower, Tomb and the town save. All runs: verdict ok, 0 AV.
Per-region linear-light parity (`parity.py`, `parity.txt`):

| Scene | on vs off | off vs off (noise floor) |
|---|---|---|
| Bower 1× idle | median 0.19%, max 5.5% | 0.21%, 4.6% |
| Bower 2× idle | 1.35%, 10.6% | 0.27%, 3.0% |
| Town save (title scene) 1× idle | **6 of 15 pairs byte-identical**, median 1.1% | — |
| Town save 2× idle / spin | **2 of 9 and 6 of 44 byte-identical** | **0 of 9, 0 of 44**, median 5.9% / 6.9% |
| Tomb 1×/2× | captures at different load/heading points (white loading screen for the first 10–14 captures, see `strip.py`) | — |

- Byte-identical frames between on and off appear only where the scene is deterministic (the title scene). There
  they are more frequent on-vs-off than off-vs-off.
- Bower animates (wind, water, NPCs), and the spin ends at a different heading in every run, so its parity is
  statistical. At 1× the idle phase matches the same-build spread (0.19% vs 0.21%). At 2× it is above that pair's
  floor (1.35% vs 0.27%), but below the spin-phase floor of the same two off runs (5.5%). The 2× runs are short and
  their idle captures fall at different animation times.
- By eye (`cmp/`): dog, character and tree shadows, the green chest glow, the gold trail, torches and the light
  shaft, HUD and subtitles all match at 2× in Bower and Tomb.

## Not done (candidates)

- **P8, stencil-only clears** (2 per frame, main size at 4×): needs a stencil-only clear quad (depth write off) in
  the deferred-clear path.
- **P7, z = 0.75 read as colour**: needs the transfer shader's exact float→unorm24 rounding and the format of the
  next binding (no lookahead). About 0.2–0.4 ms of the remaining "main depth 4× alias" role.
- **P12, real 4× targets cleared in two rectangles**: a same-key deferred clear with merging of adjacent rectangles
  would save their two clear passes.
- **Load-action clears**: merged clears covering a whole target (P6 at 2×) could use `loadAction = Clear` instead of
  an in-pass quad. The deferred-clear path draws a quad today (§14).

## Switches

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_ALIAS_CLEAR_NATIVE=0` | on | Kill switch: XDK alias clears and no-op draws are drawn as before |
| `FABLE2_ALIAS_CLEAR_TRACE=1` | off | With `FABLE2_SHADOW_TRACE=<frame>`: one `[alias-clear] f<N> cand …` line per clear-like draw (full register state, vertices, constants), plus `clear […]` / `draw kept: <reason>` lines |

Counters (frame CSV, with `FABLE2_GPU_TIME_TRACE=1`):

- `tr_n_ac_clear` and `tr_n_ac_surfaces`: draws and surfaces taken over;
- `tr_n_ac_deferred` and `tr_n_ac_immediate`;
- `tr_n_ac_cross_format`: zero clears to another format or kind;
- `tr_n_ac_noop`;
- fallbacks `tr_n_ac_fb_{state,value,owner,rect}`.

Files:

- `backends/metal/port/rex/graphics/metal/metal_command_processor.cc/.h` (`TryAliasClearDraw`, no-op skip, trace,
  counters);
- `metal_render_target_cache.cc/.h` (`TryAliasClear`, switch functions);
- `metal_shared_memory.h` (counters);
- `tools/macos/Shared/LaunchSettings.swift` (toggle);
- `docs/reference.md`, `docs/resolution-scale.md` (§26 pointer), `docs/bug-patterns.md` (row 75).
