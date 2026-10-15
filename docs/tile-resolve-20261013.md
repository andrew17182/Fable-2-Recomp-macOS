# Resolves inside the draw pass (tile resolve), 2026-10-13

Branch `agent-2/20261013-1815-tile-resolve` (from `perf-staging`), follow-up `agent-2/20261013-1921-tile-resolve-2`.
Switch `FABLE2_TILE_RESOLVE` — **default on since the follow-up** (2026-10-13 evening; `=0` kill switch; Dev Launcher →
Fixes → Render targets → "Resolves inside the draw pass"). The follow-up (drawn extent, copy+clear in one pass,
prewarm, hazard fix, default on) is the [Follow-up](#follow-up-tile-resolve-2-default-on) section at the end. Evidence:
`out/validation/tile-resolve-20261013/` and `out/validation/tile-resolve-2/` (`arm.sh`, `summ.py`, `spans2.py`,
`keydiff.py`, `ctr.py`, `parity.py`, one folder per run).

Goal (maintainer, M3 Pro 2× at 60 FPS, faithful): the colour resolves (Xenos EDRAM copy-out) ran as separate compute
encoders after the draw pass. The pass-merge census estimated them at about 4.0 ms of the M3 Pro's 2× frame. Option
(b) of that study, approved by the maintainer: do the copy as a tile function (`dispatchThreadsPerTile`) after the pass's last
draw, reading the attachment from tile memory.

## What changed

- `IssueCopy` (`metal_command_processor.cc`) no longer ends the draw pass before a colour copy when the switch is on
  (`TileResolveOffer`). `Resolve` ends it itself, at the same point as before, unless the copy can run inside it:
  - `TileResolvePrecheck`: nothing before the copy may need GPU work in another encoder (render-target growth,
    a deferred clear of one of the copy's source targets, rt-shrink verify);
  - `TryDirectHostResolveCopy` prepares everything as today (sources, rectangles, destination, fused-texture plan).
    Then `TryTileResolveEncode` (`metal_tile_resolve.cc`) encodes the tile dispatch, or the pass ends and the compute
    resolve runs as before;
  - after the tile dispatch the pass ends (`end_reason resolve:tile`). Every later pass boundary, deferred clear, alias
    clear and transfer therefore sees the same sequence as before. Only the compute encoder and its re-read of the
    attachment from memory go away.
- Shaders: `backends/metal/shaders/resolve_tile.metal` + `resolve_tile_entry.inc`, 36 variants (fast 32/64 and full
  8/16/32/64 bpp, 1×/2×/4× MSAA, scaled and unscaled, float source views) built by `tools/compile_metal_helpers.py`.
  They reuse unmodified code:
  - the XeniOS resolve (`resolve_host_color.xesli`: sample selection, packing, endian, tiling, destination address);
  - the resolve-to-texture store wrapper (`resolve_dest_texture.metal`: the buffer store plus fused texture writes).
  Only two things differ:
  1. the source "texture" is a stand-in whose `read()` has the `texture2d`/`texture2d_ms` signature but reads the
     implicit imageblock at the same texel (and sample), relative to the Metal tile;
  2. threads are laid out over the Metal tile, one per pixel group (`tileWidth / pixels-per-thread` × `tileHeight`).
     Each thread inverts the resolve's thread → texel mapping from its group's first sample, then runs the resolve's
     own forward mapping and proceeds only if it lands on the same sample. So each group is written exactly once, by
     the same code, with the same constants. Half-pixel fill rows (scaled resolves read row 1 for row 0) are written
     by the thread of the row they copy.
- Pipelines: built per (variant, pass colour formats, sample count) on a background thread; the compute path is used
  until one is ready (`tr_n_tres_fb_pso`), so there is no first-use hitch.

### Conditions (otherwise the compute path, counted per frame)

| Counter | Reason |
|---|---|
| `tr_n_tres_fb_precheck` | growth, a deferred clear of a source target, rt-shrink verify |
| `tr_n_tres_fb_shape` | no tile variant (uint views, 128 bpp, depth), or more than one source rectangle owner |
| `tr_n_tres_fb_pass` | the source is not colour attachment 0 of the open pass (same texture object), or the pass ended during preparation |
| `tr_n_tres_fb_area` | (follow-up gate) the part of the render area no draw of the pass touched > 0.5 × the resolve rectangle and > 64K pixels; original gate (`FABLE2_TILE_RESOLVE_DRAWN_GATE=0`): render area > 1.5 × the resolve rectangle and the untouched part > 64K pixels (`FABLE2_TILE_RESOLVE_AREA_RATIO`); with `FABLE2_TILE_RESOLVE_MSAA=0` also MSAA sources (counted as shape) |
| `tr_n_tres_fb_hazard` | a fused target texture is sampled by a draw of the open pass (see below; the follow-up also checks the draws actually encoded in the pass) |
| `tr_n_tres_fb_pso` | tile pipeline still building |
| `tr_n_tres_fb_tile` | Metal tile width not a multiple of the pixels per thread |
| `tr_n_tres_fb_extent` | (follow-up) the resolve reads rows outside the pass's shortened render area |

`tr_n_tres_tile` counts tile resolves, `tr_n_tres_offer` passes kept open.

**Hazard rule.** Tile functions of different tiles run concurrently with other tiles' fragment work. If a draw of the
same pass samples a texture the resolve also writes (a fused target), the result would race. A texture is safe when its
last draw binding (`tres_bind_serial_`, set in `GetBindlessSRVIndexForBinding`) is not later than the last render
encoder end (prepared draws are flushed at every encoder end) **and** (follow-up fix, bug-patterns row 80) no draw
encoded in the open pass has it (or a view of it) in its texture resource set: a draw whose descriptor indices are
still up to date samples textures bound before the pass began without rebinding them, which the serial alone missed. The mip-chain passes sample one level of the texture
whose next level they resolve into; the binding covers all levels and sampler LOD is not clamped to the guest range,
so those stay on compute.

Not covered: depth resolves (no depth in tile functions), uint source views, 128 bpp, the EDRAM-dump path, the morph
readback destination (`0x12704000`).

## Stage 0: microbench (standalone, M5 Max, under the perf lock, ~2 s of GPU)

Tile resolve (imageblock reads) vs compute resolve (texture reads) into the same packed buffer:

- **Bytes identical** in every case, including 2×MSAA. Per-sample reads need `imageblock_data_rate::sample`.
- **Untouched tiles cost.** A tile dispatch visits the whole render area. Drawing only the top 720 rows of a
  2240-wide target, then resolving 2240×576: tile 3.5–6.6 / 18–35 / 44–78 µs per pass at target heights
  720 / 2352 / 4704, compute flat at 11–21 µs. Hence the area gate.
- Where render area ≈ drawn area: 2240×1440 2×MSAA, resolve 2240×1152: tile 11–22 µs vs compute 42–89 µs per pass;
  48 small passes: compute 4.2–13.6 µs per encoder vs tile 0.1–4 µs.
- One thread per pixel group with threads = tile/8 is 3–7× cheaper than one thread per pixel.

## Exactness

`FABLE2_TILE_RESOLVE_VERIFY=1`: after each tile resolve the pass ends, the destination range (and each fused
texture level) is copied to a snapshot, the normal compute resolve runs over the same destination, and its output
is snapshotted and compared byte for byte on completion. The game keeps the compute result.
`FABLE2_TILE_RESOLVE_VERIFY_BREAK=1` (negative control) shifts the tile resolve's source base by one tile.

| Run (spin5) | resolves checked | mismatched |
|---|---|---|
| Bower 1× verify | 40,000 (11.6 GB) | **0** |
| Bower 1× break | 42,000 | 41,544 |
| Bower 2× verify (with half-pixel rows) | 40,000 (45.8 GB) | **0** |
| Old Tomb 2× verify | 14,000 (35.9 GB) | **0** |
| Bower 2× verify (imageblock reads) | 36,000 (41.7 GB) | **0** |
| Bower 2× verify (with MSAA) | 18,000 (45.3 GB) | **0** |
| Old Tomb 2× verify (with MSAA) | 14,000 (68.5 GB) | **0** |
| Bower 1× verify (with MSAA) | 36,000 (24.0 GB) | **0** |

## Results (M5 Max, headings8 uncapped, `f2diag gpu` over the sweep window; same build, `FABLE2_TILE_RESOLVE=0` vs `=1`)

Every run: verdict ok, 0 AV, no contention (contended runs were discarded and rerun). Batches 3–4 used a pinned agent
shader-cache snapshot (`--cache-snapshot`).

| Run | GPU ms/frame off → on | FPS off → on | compute encoders / frame | tile resolves / frame |
|---|---|---|---|---|
| Bower 2× (stage 2, 2 pairs) | 8.92 / 8.89 → **8.56 / 8.53** | 108.2 / 109.6 → 111.0 / 111.4 | 87 → 44 | 50 |
| Bower 2× (after merging perf-staging, batch 3) | 8.09 → **7.66** (−5.3%); MSAA kept on compute 7.77 | 116.0 → 117.8 | 84 → 41 | 50 |
| Bower 1× | 6.89 → **6.65** | 128.2 → 129.1 | 82 → 37 | 51 |
| Old Tomb 2× | 6.76 → **6.51** | 148.5 → 152.7 | 50 → 29 | 20 |
| Bower 3× (2 pairs) | 11.64 → 11.77; 11.65 → 11.68 (flat); MSAA on compute 11.68 | 84.8 → 83.9; 83.9 → 84.5 | 88 → 42 | 51 |

- **2× and 1×: −0.24 to −0.43 ms of GPU per frame on the M5 Max** (4–5%). FPS gains are 1–3% (the GPU Commands
  thread limits uncapped FPS here).
- **3×: flat.** The per-key render-pass spans show where: the main 2×MSAA pass (`3360×2160×2`) grows by about
  0.2 ms per pass. Its first predicated-tiling resolve covers 1728 of 2160 rows (inside the 1.5× area gate), so the tile
  dispatch also loads and stores the untouched rows; at 3× those bytes cost about what the removed compute pass did.
  Keeping MSAA on compute (`FABLE2_TILE_RESOLVE_MSAA=0`) does not change 3× and loses 0.1 ms at 2×, so MSAA stays on.
- Encoders: 43–44 fewer compute encoders per frame in Bower (resolves 64 → 21).

### M3 Pro estimate (not measured)

Census model of `docs/hle-census-20261013.md` §8: M3(2×) ≈ 1.62 × fixed + 4.86 × 4 × per-pixel, with fixed and
per-pixel parts from the M5 1×/2× deltas (−0.24 / −0.36…−0.43 ms): fixed ≈ −0.18 ms, per-pixel ≈ −0.04…−0.06 ms
per 1× of pixels. That gives **about −1.1 to −1.4 ms at 2× in Bower Lake**. Lower than the 1.5–2.4 ms first estimate:

- 12–16 resolves per frame stay on compute (the mip-chain hazard below), and depth, uint and 128 bpp resolves were
  never covered;
- the 3× result shows the untouched-row load/store of partly drawn passes can cancel the per-pixel saving. The M3 Pro
  has a quarter of the M5 Max's bandwidth, so this is the main risk to the estimate; a stricter area gate
  (`FABLE2_TILE_RESOLVE_AREA_RATIO=1.1`) is the knob to try there.

### Parity (spin5, `parity.py`, 4×4 linear-light regions, idle phases)

| Pair | idle median / max | idle2 median / max |
|---|---|---|
| Bower 2× on vs off | 3.07% / 15.5% | 6.32% / 39.7% |
| Bower 2× off vs off (noise) | 2.86% / 14.0% | 3.74% / 12.2% |
| Bower 1× on vs off | 2.41% / 22.1% | 1.88% / 7.8% |
| Old Tomb 2× on vs off | 0.26% / 58% (one dark corner region) | 3.92% / 47.7% |

The spin ends at a different heading each run and Bower animates, so this is statistical; the byte-exact verify above
is the evidence. By eye (`cmp/`): Old Tomb chest glow, HUD, subtitles, torches and dog; Bower dog, trees, gold trail
and fire match.

### Validation (on unless stated)

- spin5 verify at 1× and 2× (Bower, Old Tomb) and the area run with verify at 2×: **132,000 resolves, 0 mismatches**
  (297 GB compared) in the area run; all verdict ok, 0 AV.
- area at 1× off and on, bootloop 2/2 pass with the switch on.
- Off path: with the switch off, `IssueCopy` ends the pass exactly as before and `Resolve` does nothing extra; the
  only always-on additions are the per-binding serial store and one assignment per encoder end.

### First launch

On a cold Metal cache the tile pipelines compile on the background thread in about 0.3 s each; the 16 bpp full
variant took 5.0 s. Until a pipeline is ready that resolve uses the compute path (`tr_n_tres_fb_pso`), so nothing
stalls, but the saving starts a few seconds later. Warm launches build them in under 1 ms. Follow-up if the switch
goes default on: prewarm the common (variant, single-attachment format) pairs at startup like `FABLE2_RTT_PREWARM`.

## Not done / next

- **Mip-chain hazard** (12–16 per frame): the pass that renders mip level N+1 binds the texture whose level N+1 it
  resolves into. Proving the draws cannot read level N+1 needs the shader's LOD (sampler LOD is not clamped to the
  guest mip range here), so they stay on compute.
- Depth resolves (tile functions cannot read depth), uint source views, 128 bpp.
- ~~A drawn-area gate~~ and ~~keeping the pass open / copy+clear in one pass~~: done in the follow-up below.

## Switches

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_TILE_RESOLVE=0` | on (since the follow-up) | Kill switch: colour resolves as tile functions in the draw pass, and the follow-ups below. Dev Launcher toggle |
| `FABLE2_TILE_RESOLVE_EXTENT=0` | on | Draw passes open with the full render area (follow-up). Dev Launcher toggle |
| `FABLE2_TILE_RESOLVE_DRAWN_GATE=0` | on | Original area gate instead of the untouched-area gate. Dev Launcher toggle |
| `FABLE2_TILE_RESOLVE_CLEAR=0` | on | The resolve's clear in a pass of its own, not after the copy in the same pass. Dev Launcher toggle |
| `FABLE2_TILE_RESOLVE_PREWARM=0` | on | No startup build of the tile pipelines. Dev Launcher toggle |
| `FABLE2_TILE_RESOLVE_KEEP_OPEN=1` | off | Keep the pass open after a tile resolve for the next draws (no gain measured). Dev Launcher toggle |
| `FABLE2_TILE_RESOLVE_VERIFY=1` | off | Also run the compute resolve and compare bytes (slow). Dev Launcher toggle |
| `FABLE2_TILE_RESOLVE_VERIFY_BREAK=1` | off | Negative control (shifted source base) |
| `FABLE2_TILE_RESOLVE_AREA_RATIO=<x>` | 1.5 | Area gate |
| `FABLE2_TILE_RESOLVE_MSAA=0` | on | Keep 2×/4× MSAA sources on compute |
| `FABLE2_TILE_RESOLVE_TRACE=1` | off | Log fallback reasons with shapes every 2048 fallbacks, and why passes keep their full render area every 4096 passes |

Files: `backends/metal/port/rex/graphics/metal/metal_tile_resolve.cc` (new), `metal_direct_host_resolve.cc`,
`metal_render_target_cache.cc/.h`, `metal_command_processor.cc/.h`, `metal_texture_cache.cc/.h` (bind serials),
`metal_shared_memory.h` (counters), `backends/metal/metal-sources.cmake`, `backends/metal/shaders/resolve_tile.metal`
and `resolve_tile_entry.inc` (new), `tools/compile_metal_helpers.py`, `tools/macos/Shared/LaunchSettings.swift`,
`docs/reference.md`, `docs/bug-patterns.md` (row 79).

## Progress notes

- 18:15 slot acquired; design sent and approved (K2 correction: K2 was removed for −1.3% FPS, not a crash; the
  crashes were xctrace and GPU captures).
- 18:20 stage 0 microbench (above).
- 18:35 stage 1 built (1× MSAA). Bower 1×: 53 tile resolves / 74 offered per frame; verify 0 mismatches.
- 18:37 half-pixel rows: at 2× nearly every scaled resolve has the half-pixel fill; added (was a fallback).
- 18:45 first perf pairs at 2×: GPU +0.2 ms (worse). The source stand-in cached 8 lanes in a thread array indexed at
  run time (stack spills); replaced by direct imageblock reads (an imageblock can be a struct member). Then 2×:
  GPU 8.95 → 8.69 / 8.67 ms.
- 18:55 stage 2 built (2×/4× MSAA variants). Verify 0 mismatches (Bower 2× 18k, Old Tomb 2× 14k, Bower 1× 36k).
- 19:10 perf batches, parity spins, 3× pairs (results above).
- 19:45 merged perf-staging (with pass-merge) into the branch: verify Bower 2× 20k, Old Tomb 2× 8k, Bower 1× 26k
  resolves, 0 mismatches; Bower 2× GPU 8.03 / 8.09 → 7.69 / 7.65 ms (2 pairs), FPS 117.3 / 118.2 → 118.6 / 119.0.
  Merged into perf-staging (default off).

## Follow-up: tile-resolve-2 (default on)

Branch `agent-2/20261013-1921-tile-resolve-2` (from `perf-staging` c95b78b). Evidence: `out/validation/tile-resolve-2/`.
All numbers are from the M5 Max, `f2diag gpu` GPU ms per frame over the sweep window, with a pinned agent shader cache
(`2f6e8bb03960b865@7`), no contention.

### 1. Render area as tall as drawn, and the drawn-area gate

- **Extent shrink** (`FABLE2_TILE_RESOLVE_EXTENT`, `BeginRenderEncoderForDraw` / `PassExtentChoose`): a draw pass opens with
  `renderTargetHeight` = the rows its draws need. The value comes from the larger of the opening draw's scissor bottom and
  the last 16 passes with the same key (target + opening draw's rows; predicated tiling draws each screen tile into the
  same target). It is rounded up to 32 rows. Only when every attachment is load-and-keep (`Load`, `Store`/`DontCare`)
  and no transfers or in-pass clears are pending. Outside the render area nothing is loaded, cleared, stored or
  resolved, so those rows stay as they are in memory, which is what a pass that never draws there leaves. A later draw
  whose scissor reaches below the area, or pending transfers, ends the pass first (`tr_n_tres_grow`; 0 in every run).
  Scissors are still clamped against the textures, as before.
- **Resolve inside the area**: the tile resolve computes the last source row it reads (dump tiles relative to the
  target's base, in its pitch, × host rows per EDRAM tile) and falls back to compute when it lies outside
  (`tr_n_tres_fb_extent`, about 4/frame at 3×, all small 280×184 resolves of 240×192 passes).
- **Drawn-area gate** (`FABLE2_TILE_RESOLVE_DRAWN_GATE`): fall back when the untouched part of the render area (area
  minus the bounding box of the pass's draw scissors; transfers count as the whole target) exceeds
  `(AREA_RATIO − 1) ×` the resolve rectangle and 64K pixels. The old ratio gate stays behind the switch.
- At 3× Bower the main 3360×2160 2×MSAA pass opens 1728 rows tall (the first predicated tile) and the second tile pass
  448 rows tall. 13–18 passes per frame open shortened.

### 2. Prewarm and first launch

`StartTileResolvePrewarm` (beside the transfer prewarm, before shader-storage init) queues on the tile-pipeline thread
every key in `<title>.tres-keys.txt` in the title's shader cache (each newly requested key is appended) plus the
16 built-in keys of `metal_tile_resolve_keys.inc`: every tile pipeline seen in Bower Lake, Old Tomb and Bowerstone Old
Town at 1×–3×. Scaled variants are skipped at 1×.

| Bower 2×, `--cache-snapshot cold` | level load end (s after start) | tile pipelines ready |
|---|---|---|
| prewarm on (3 runs) | 3.52, 3.24, 3.26 | all 16 within 0.01 s of start, before any resolve asked |
| prewarm off (3 runs) | 3.28, 3.36, 3.34 | built on first use (compute until then) |
| tile resolve off | 3.28 | — |

Time to gameplay is unchanged (noise ±0.15 s). `cold` empties the project cache only. The macOS Metal system cache stayed
warm, so builds took 0.1–0.2 ms. On a Mac with a cold system cache the previous agent measured about 0.3 s per pipeline
and 5 s for the 16 bpp full variant. That runs on one utility thread in the background during boot (about 3 s here), and
resolves use compute until each pipeline is ready. It was not measured: clearing the system cache would also make
the player's next launch cold.

### 3. Copy and clear in one pass; keep-open

- **Copy+clear** (`FABLE2_TILE_RESOLVE_CLEAR`, `Resolve` clear section → `TileResolveDrawClearInPass`): after a tile
  resolve the pass stays open through `Resolve`. The resolve's clear of a target that is an attachment of that pass is
  drawn in it, after the tile dispatch, with the pass-merge in-pass clear code (`EncodePendingDrawPassTransfers`). Metal
  orders a tile dispatch after earlier draws and before later draws in each tile, so each tile is copied, then
  cleared, exactly like the separate passes. In a shortened pass the rows below the area stay a deferred clear of the
  remainder rectangle. Anything in the clear path that encodes GPU work elsewhere (render-target growth, an older
  deferred clear of the same target or of a transfer source, transfers, non-deferred clears) ends the pass first.
  `IssueCopy` ends it after `Resolve`. Mechanism: at 3× Bower the separate 3360×2160×2 colour clear pass (≈600 µs span)
  and depth clear pass (≈160 µs) disappear, 4 in-pass clears per frame (`tr_n_tres_clear_inpass`). The M5 Max frame
  GPU time shows no clear change from it (on 11.59/11.66 vs `CLEAR=0` 11.61/11.56 ms, long plan). It saves a full
  load+store of the main MSAA target per frame, which should count for more on bandwidth-limited Macs.
- **Keep-open** (`FABLE2_TILE_RESOLVE_KEEP_OPEN=1`, default off): the pass also stays open for the next draws. Draws that
  read what the tile resolve wrote (a fused texture, or shared memory written by the tile stage) end it first
  (`tr_n_tres_kept_break`). Result: **no continuation**. In Bower 2× and 3× the next draw after a tile resolve binds
  other targets every time (`tr_n_tres_kept` 0.00–0.03/frame; "resolve:tile" ends just move to
  `rt_update_descriptor_dirty`). Parity matched (idle median 0.14%). It stays off.

### 4. Hazard hole (bug-patterns row 80)

The merged fused-target check used bind serials only. Draws whose descriptor indices were still up to date sampled
textures bound before the pass without rebinding them. The 64×64 and 32×32 mip-chain resolves of Bower (~10/frame each)
therefore ran as tile functions while their own pass sampled the same texture. The verify mode cannot see this: both
paths read the same render target. It produces wrong pixels only if those draws sample the level being written (host
sampler LOD is not clamped to the guest mip range). That was never observed, and can't be ruled out without the shaders'
LOD. The check now also covers the textures of the draws actually encoded in the pass (`TileResolveNotePassSampled`,
bases resolved lazily per pass). This puts about 27 resolves per frame back on compute (tile resolves 50 → 25/frame in
Bower), and most of the merged 1×/3× gain came from them.

### Exactness and parity (final build)

| spin5 verify, current build | resolves | mismatched |
|---|---|---|
| Bower 1× / 2× / 3× | 14,000 / 8,000 / 4,000 | **0** |
| Old Tomb 1× / 2× / 3× | 8,000 / 6,000 / 2,000 | **0** |
| Bowerstone Old Town (`hero1-dog-live_2026-10-04_14-07-34`) 1× / 2× / 3× | 10,000 / 6,000 / 2,000 | **0** |
| Bower 1× negative control (`VERIFY_BREAK`) | 14,000 | 13,516 |

60,000 resolves (≈379 GB) compared, 0 mismatches, all verdicts ok. Verify ends the pass after each tile resolve, so it
does not cover the in-pass clear; parity and the mechanism argument above do. Parity (`parity.py`, idle phase, on vs
off, spin5): Bower 2× 2.05% median / 3.55% max, Old Tomb 2× 0.06% / 0.25%, town 2× 1.48% / 10.6% (falling snow),
Bower 3× 0.38% / 1.69%. Earlier off-vs-off noise was 2.86% / 14.0%. By eye (`cmp/`), Bower 3× and the town match.
Default launch (no env): 23 tile resolves and 4 in-pass clears per frame. Bootloop 2/2 pass; area run at 2× ok, 0 AV.

### Performance (headings8-long unless noted; GPU ms/frame off → on)

| Scene / scale | off | on | Δ |
|---|---|---|---|
| Bower 1× | 6.28, 6.26 (short: 6.17, 6.38, 6.38) | 6.48, 6.24 (short: 6.60, 6.44, 6.49) | flat to +0.1 (noise ±0.15) |
| Bower 2× | 7.81, 7.81, 7.89 | 7.59, 7.61, 7.47 | **−0.21 to −0.42** |
| Bower 3× | 11.82, 11.63 | 11.59, 11.66 | −0.10 (noise) |
| Old Tomb 1× / 2× / 3× | 4.00 / 5.69 / 9.16 | 3.87 / 5.44 / 8.96 | **−0.13 / −0.25 / −0.20** |
| Town 1× / 3× | 9.21 / 12.13 | 9.25 / 12.20 | flat |
| Town 2× (3 pairs) | 10.43, 10.40, 10.57 | 10.54, 10.43, 10.43 | flat (10.47 vs 10.47) |

FPS uncapped moves with the GPU Commands thread, not GPU time (±2%). The per-draw bookkeeping for the hazard check
resolves texture bases lazily, once per pass (an earlier per-draw version cost about 1% FPS).

**Default decision: on.** The verify is exact in every scene and scale, parity is within noise, there is no regression
beyond noise in any scene or scale, and there are gains at 2× (the M3 Pro target) in Bower and Old Tomb. M3 Pro, not measured: the census model above (fixed + per-pixel parts from the
M5 1×/2× Bower deltas, 1× −0.02…+0.1, 2× −0.21…−0.42) gives −1.7 to −2.4 ms at 2×. Nearly all of the saving is
per-pixel, and the removed clear passes and shorter passes save bandwidth the M5 Max does not need. But the 1× delta is
within noise, so treat this as a direction rather than a number. Bower 3× and the town are flat on the M5 Max.

### Not done / next (follow-up)

- ~~Mip-chain resolves (~27/frame)~~: done in `docs/mip-lod-20261013.md` (`FABLE2_MIPLOD_PROOF`). The host already
  clamps to the guest mip range; the guest's own fetch range includes the written level, so the fix bounds each
  draw's LOD and limits its texture view.
- Keep-open has nothing to merge in these scenes; the guest re-binds targets after every tile resolve.
- Cold system-cache prewarm cost (needs a Mac or user account with an empty Metal cache).

### Post-merge check (perf-staging e89f043: native tessellation, rectangles, point sprites)

Verify spin5 with the merged build: Bower 3× 6,000, Bower 2× 8,000, town 2× 6,000, Old Tomb 1× 10,000 resolves, all
**0 mismatches**, verdicts ok. headings8-long (snapshot `e05b84cb5ec9220c@2`): Bower 2× 7.70 → **7.51** ms, Bower 3×
11.77 → 11.76 ms (flat); a default launch runs 22 tile resolves, 4 in-pass clears and 12 shortened passes per frame.
