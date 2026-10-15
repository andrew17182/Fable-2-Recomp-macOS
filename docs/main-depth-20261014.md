# Native main-depth alias clears (2026-10-14)

Branch `agent-1/20261014-1001-main-depth` (from `perf-staging` fa0134d), merged into `perf-staging`. Switch
`FABLE2_MAIN_DEPTH_NATIVE` (default on, needs `FABLE2_ALIAS_CLEAR_NATIVE` on; Dev Launcher → Fixes → Render targets →
"Native main-depth clears"; `=0` draws these clears as before). Evidence: `out/validation/main-depth-20261014/`
(KEEP): `seq.py` (one traced frame as a compact event list), `seq600.txt` / `seq601on4.txt` (off / on), `dumpcheck.py`
and `dump*-on|off/` (EDRAM dumps), `run.sh`, `perf.sh`, `summ.py`, `sitecost.py`, `perf-s.txt`, `parity.sh`,
`parity-*.txt`, `cmp/`.

Goal (approved 2026-10-14): the M3 Pro's 2× frame (benchmark kit 8601508) still spent **1.57 ms** in the census role
"main depth at 4×MSAA alias (clear + no-op points)", 3 render encoders with 1 draw each, after `docs/alias-clear-20261013.md`.

## What was left (Bower Lake 2×, `FABLE2_ALIAS_CLEAR_TRACE=1 FABLE2_SHADOW_TRACE=600`)

After the depth prepass resolve (main depth `1008t <14t> 2×MSAA D24FS8`, 1120×720), the frame does this before the main
colour pass. Tiles are EDRAM tiles at pitch 14.

| # | Guest draw | Alias target (4×MSAA, D24S8) | Tiles | What it writes | Read next by |
|---|---|---|---|---|---|
| P7 | XDK clear, no PS, rect 560×576 at 4×, z = 0.7500001 (`0x3F400001`, exactly unorm24 `0xC00000`), stencil 0 | `0t` | [0, 1008) | depth word `0xC0000000` in every sample | [0, 630): the screen-space shadow mask's colour target `0t <14t> 1× 8888`, as colour (`R=0, G=0, B=0, A=0xC0`); [630, 1008): P8 |
| P8a | XDK clear, no PS, z off, stencil ALWAYS/REPLACE ref 0, rect 560×360 at 4× | `630t` | [630, 1260): P7's word + main depth's first 252 tiles | stencil byte only | the shadow mask's depth/stencil target `630t <14t> 1× D24S8` |
| — | shadow mask: 2 full-screen quads (stencil write, colour), then a resolve of the colour | — | | | |
| P8b | the same stencil-only clear again | `630t` | [630, 1260) | stencil byte only | the main colour pass: main depth [1008, 1260) and the HDR colour target [630, 1008) |

Today each 4× pass binds a 1120×1152 or 1120×720 (×2 scale) 4×MSAA D32S8 host target: P7 with 7 incoming
ownership transfers, P8a with 2, P8b with 1, and every reader transfers back out of them (6 MSAA-change transfers per
frame). The 24 no-op points of the census were already skipped.

**Why alias clears did not take them** (`docs/alias-clear-20261013.md` "Kept cases"):

- P7: the value is neither 0 nor 1 ("depth value"), and the surface has no D24S8 variant at another MSAA level to hold
  it. The word is read back as colour.
- P8a/b: z is off ("depth/stencil state"); only full-word clears were handled.

A second P7-like case sits in the post chain: the half-res `0t <7t>` surface is initialised the same way (z = 0.7500001
through a D24S8 4× alias, 280×176, read next as `k_2_10_10_10`). It was part of "clear quads at the 4×MSAA alias".

## Change

`TryAliasClearDraw` (`metal_command_processor.cc`) accepts two more kinds; `TryAliasClear` (`metal_render_target_cache.cc`)
routes them:

1. **Depth word on the unorm24 grid.** For a D24S8 alias, any z in (0, 1) is accepted when
   `n = RoundToNearestEven(z * 16777215.0f)` (float32, exactly the transfer shader's conversion) satisfies
   `UNorm24To32(n) == z` bit for bit. Then every later reader gets the same word, and every depth written back from it
   is z again. Other z stay on the draw path.
2. **`TryDepthWordAliasClear`**, when the surface has no D24S8 variant: the word goes into **existing word-exact
   targets of the same pitch**, in whole tile-row bands:
   - word-exact means D24S8 (depth `UNorm24To32(word >> 8)` = z, stencil = low byte), or 8888 / 10:10:10:2 unorm colour
     (RGBA8Unorm / RGB10A2Unorm hold any word; transfers pack them back with the same layout);
   - all 32bpp keys share one tile layout, so a target whose base lies in the cleared range on a row boundary can hold
     the rows from its base on. Each band goes to the target with the nearest base at or before it. Per base the
     preference is the current owner of that tile, then D24S8, then colour, fewest samples first. A tie between two
     different targets drops that base;
   - no target is grown for this: a band past the rows its target already addresses keeps the draw (rt-shrink, §21 of
     `resolution-scale.md`). One load-time pitch-16 case in Bower stays a draw for this reason;
   - each band is the same resolve-style ownership change and deferred clear as every alias clear
     (`CommitAliasClearSurface`, split out of `TryAliasClear`).

   P7 becomes `[0, 630)` → `0t <14t> 1× 8888` (word `0xC0000000`, the shadow mask's own colour target) and
   `[630, 1008)` → `630t <14t> 1× D24S8` (depth z, stencil 0, the shadow mask's own depth target). The half-res case
   goes to its owner `0t <7t> 1× 10:10:10:2`.
3. **`TryStencilOnlyAliasClear`**: z test and write off, stencil ALWAYS with REPLACE, write mask 0xFF, no colour, whole
   tile rows. The tiles move to the surface's existing same-format variant at another MSAA level, with the transfers
   a draw binding it would do (`ChangeOwnershipForTileConversion`, no clear cutout: the depth is kept). Then only the
   stencil is replaced, as a deferred clear with the new flag `kClearValueStencilOnly` (bit 63 of the 64-bit clear
   value; quad with `GetTransferStencilClearState`, depth write off; never a load action or DontCare load).
   - When the variant's only queued clear is P7's band (its own tiles, so the transfers never touch it), that clear is
     drawn in the same pass as the transfers (`tr_n_md_carried`).
   - P8a: one transfer (main depth [1008, 1260) into `630t 1×`) plus the carried band, then the stencil clear merges
     into the shadow-mask pass. P8b: no transfer; its stencil clear is flushed as a small 1× pass when the main colour
     pass's transfers read `630t`.

No 4×MSAA target is bound for the main depth any more. Every case that does not meet these conditions is drawn as
before.

### Why the bytes are the same

- **Words.** The 4× alias held the word W in every sample of its tiles. Now each band's target holds W, and any key
  reading those tiles gets W through the same transfer shaders: depth → colour packs `RNE(d * 16777215) << 8 | stencil`
  (= W, since d = z), colour → depth unpacks W. Stencil-only: the variant gets the same transfers the 4× alias got (same
  shader code, another sample count), then the same stencil byte.
- **Host depth values.** D24S8 depth read back from W is `UNorm24To32(W >> 8)`, required to equal z. The `[630, 1008)`
  band is a D24S8 claim, so the unorm24 host-depth owner of those tiles is a target holding z, as with the alias.
  The `[0, 630)` band is a colour claim and leaves that owner as it was; it is only consulted for D24S8 transfers out
  of those tiles, and none happen in this sequence (the next claim of those tiles is P7 again).
- **The unorm24 round trip is lossy.** `RNE(UNorm24To32(n) * 16777215.0f) != n` for every odd n in [2^22, 2^23)
  (brute force over all 2^24 values; the result is n − 1, after which it is stable). Main depth's float24 words pass
  through D24S8 here, so about 40% of those words lose one step, in both paths. Today's chain has more such hops
  (2×→4×→1×→4×→2×) than the new one (2×→1×→2×), but the loss happens only on the first hop, so the results agree. The
  dump check below confirms it.

### Exactness check (byte for byte)

`FABLE2_MAIN_DEPTH_DUMP=<frame>` with `FABLE2_MAIN_DEPTH_DUMP_DIR=<dir>` (diagnostic, both paths) dumps the guest words
of the whole EDRAM, as every owner holds them, at three points of that frame:

- (a) before P7;
- (b) before the shadow-mask draw;
- (c) before the first main colour pass draw.

It uses the resolve dump path after flushing deferred clears; the readback is written from the command buffer's
completion handler, with no stall.

`dumpcheck.py` checks the closed form:

- (b) tiles [0, 1008) are `0xC0000000`;
- (c) tiles [630, 1008) are `0xC0000000`;
- (b) and (c) tiles [1008, 1260) are `lossy(a >> 8) << 8`, the main depth after one unorm24 hop with stencil 0.

Result at Bower 2×, frame 600: **PASS in both arms** (off and on), every word of the checked ranges (5,120 words per
tile at 2×). About 910k distinct main-depth words, 363–370k of them changed by the hop.

## Results

M5 Max, Bower Lake `hero1-dog-live_2026-10-11_11-20-20`, headings8-long uncapped, `FABLE2_GPU_TIME_TRACE=1`, pinned
agent cache `c988f63d278a6bb3@16`, arms alternating off/on/on/off on one build (`FABLE2_MAIN_DEPTH_NATIVE=0` vs
default). Every run: verdict ok, 0 AV, contention 0. Roles: `f2diag census-cost` over the sweep window, marginal busy
ms/frame (`perf-s.txt`).

| | FPS | GPU mean ms | render enc. / all enc. per frame | main depth 4× alias | shadow mask | clear quads 4× | transfers / clears |
|---|---|---|---|---|---|---|---|
| 2× off | 130.1 / 131.2 | 7.51 / 7.45 | 97.8 / 170.8 | 0.426 (2.95 enc.) | 0.566 | 0.048 | 0.069 |
| 2× on | 139.5 / 137.4 | 6.86 / 6.93 | 95.5 / 168.5 | **0** (0 enc.) | 0.458 | 0.000 | 0.191 |
| 1× off | 154.5 / 154.8 | 5.51 / 5.49 | 90.5 / 143.6 | 0.156 | 0.190 | 0.027 | 0.031 |
| 1× on | 150.8 / 157.9 | 4.81\* / 5.36 | 88.1 / 139.5 | 0 | 0.131 / 0.154 | 0.001 | 0.069 |

\* `s-bower1-on1` also drew 0.22 ms less in the main colour pass, which this change does not touch. That is a per-boot
scene difference (bug-patterns lessons); use `on2` for 1×.

- **2×: GPU −0.58 ms (−7.8%), FPS +6%.** At 1× (`on2`): −0.13 ms. An earlier batch on the build without the row-band
  split gave −0.47 ms at 2×.
- Per pass at 2× (`sitecost.py`):
  - removed: the three 4× passes (190 + 152 + 84 µs), the half-res 4× pass (47 µs) and the self-depth stores
    before them (61 µs);
  - the shadow-mask pass went from 564 to 460 µs: no incoming transfers, and it loads what is already there;
  - added: one 1× transfer pass with the carried band (92 µs) and P8b's 1× stencil pass (35 µs).
- Per frame: ownership transfers 105.6 → 92.8, MSAA-change transfers 13.8 → 7.9.
- Counters on: `tr_n_md_word` 3 (two P7 bands + the half-res case), `tr_n_md_stencil` 2, `tr_n_md_stencil_xfer` 1,
  `tr_n_md_carried` 1, and `tr_n_ac_fb_value` 2 → 0.

**M3 Pro estimate at 2×** (census model of `docs/pass-merge-20261013.md`: per role, M5 1×/2× fitted to `F + P·s²`, M3 ≈
1.62·F + 4.86·4·P).

- **Calibration.** The model over-predicts the kit's measured M3 roles by a steady factor of 0.84: main depth 4×
  1.87 vs 1.57 measured, shadow mask 2.54 vs 2.14, and the four affected roles together 4.84 vs 4.13.
- **Per role, calibrated:**
  - main depth 4× alias −1.57 ms (to 0);
  - shadow mask about −0.37 (2.14 → 1.77);
  - clear quads 4× −0.13;
  - transfers/clears +0.41 (0.30 → 0.70).
- **About −1.7 ms in total** (20.9 → ~19.2 ms, GPU-bound 47 → ~52 FPS).
- Fitting total GPU instead of roles gives −2 to −3 ms, but that fit is dominated by 1× noise.
- This is an estimate, not measured on an M3 Pro.

**First launch** after the final build (`md-dump6-on`, 2× spin at 60 FPS) was ready in 6.6 s, with 509 warm pipelines
and nothing waiting. No new pipeline kinds: the stencil-only quad reuses the depth clear pipeline with another
depth/stencil state.

### Correctness

Frame-matched stills (`f2diag run still`, 2 runs per arm, arms interleaved, pinned snapshot, `f2diag parity`). Guild =
Thag's Cave (`hero1-dog-live_2026-10-07_14-45-08`), town = Bowerstone (`hero1-dog-live_2026-10-04_14-07-34`).

| Scene | cross median / max | A-A floor median / max | verdict |
|---|---|---|---|
| Bower 2× | 0.166 / 0.271 | 0.178 / 0.279 | within noise |
| Bower 1× (`--at-frame load+800`) | 0.133 / 0.399 | 0.216 / 0.479 | within noise |
| Guild 2× | 0.037 / 0.095 | 0.046 / 0.076 | within noise |
| Guild 1× | 0.047 / 0.097 | 0.042 / 0.094 | within noise |
| Town 2× (`load+700`) | 0.535 / 0.792 | 0.128 / 0.303 | **beyond noise: snow** |
| Town 1× (`load+700`) | 0.286 / 0.863 | 0.255 / 0.674 | beyond noise (2 blocks, 24 vs 19) |

- **Town is snowing.** The ten hottest blocks at 2× all sit in one region where a large near-camera snowflake drifts
  (`cmp/town2x-f807-*.png`). The same frame differs in the same way between two runs of one arm (`m0pair`, `m1pair`).
  The A-A floor is low only because the ±4 shift search found better-aligned frames for those two pairs.
- By eye, the rest of those crops (walls, roofs, the chimney, small flakes aside) is the same.
- **Harness note.** Town and Bower 1× reach `load+240` before the harness asks for it ("passed (game at frame …)"),
  which silently falls back to unmatched stills. Use a later `--at-frame` for these saves.
- All runs: verdict ok, 0 AV.

### Post-merge check (perf-staging 7ae0aba + this branch: real occlusion queries, bloom footprint, no-converter release)

- First launch after the merged build: ready in 6.8 s, verdict ok, 0 AV.
- EDRAM dump closed-form check: PASS.
- Bower 2× headings8-long, one pair: GPU 7.80 → 7.26 ms (−0.54), FPS 127.5 → 136.7.
- Frame-matched parity Bower 2×: within noise (0.281 / 0.427 vs floor 0.153 / 0.225, limit 0.391;
  `parity-merged-bower-2x.txt`).

## Not done

- **P8b's 1× stencil pass (35 µs at 2× on the M5).** It is flushed because the main pass's transfers read `630t`. A
  transfer variant that takes a pending stencil-only clear as a constant would remove it.
- **P8a's transfer pass (92 µs).** Main depth's tiles still move into `630t 1×` in a pass of their own (self host-depth
  store + transfer). Queuing the stencil clear behind the shadow-mask pass's own in-pass transfers would merge it.
- **Pitch-16 alias at load time:** stays a draw (the word-exact target would have to grow).
- **P12** (real 4× small-scene targets cleared in two rectangles): unchanged.

## Switches and counters

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_MAIN_DEPTH_NATIVE=0` | on | Kill switch: depth-word and stencil-only alias clears are drawn as before |
| `FABLE2_MAIN_DEPTH_DUMP=<frame>` + `FABLE2_MAIN_DEPTH_DUMP_DIR=<dir>` | off | Diagnostic: whole-EDRAM guest-word dumps `f<frame>-{a-preclear,b-ssmask,c-mainpass}.bin` (checkpoints keyed to this sequence's shader hashes) |

Counters (frame CSV with `FABLE2_GPU_TIME_TRACE=1`):

- `tr_n_md_word`: depth-word bands written into word-exact targets;
- `tr_n_md_stencil`: stencil-only alias clears;
- `tr_n_md_stencil_xfer`: transfers those moved into the variant;
- `tr_n_md_carried`: queued bands drawn in the stencil clear's transfer pass.

`FABLE2_ALIAS_CLEAR_TRACE=1` logs `stencil-only [...]` lines next to the existing `clear [...]` lines.

Files:

- `backends/metal/port/rex/graphics/metal/metal_command_processor.cc`: `TryAliasClearDraw` (stencil-only and unorm24
  depth words), dump checkpoints, counters;
- `metal_render_target_cache.cc/.h`: `TryDepthWordAliasClear`, `TryStencilOnlyAliasClear`, `CommitAliasClearSurface`,
  `kClearValueStencilOnly` in `PerformTransfersAndResolveClears`, `MainDepthDumpEdram`;
- `metal_shared_memory.h`: counters;
- `tools/macos/Shared/LaunchSettings.swift`: toggle;
- `docs/reference.md`, `docs/resolution-scale.md` (§28), `docs/bug-patterns.md` (row 98).
