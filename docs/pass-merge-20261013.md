# Pass merge: encoder-boundary census and fewer pass breaks (2026-10-13)

Branch `agent-3/20261013-1617-pass-merge` (from `perf-staging` be11564), merged into `perf-staging`. M5 Max. Saves:
Bower Lake `hero1-dog-live_2026-10-11_11-20-20`, Old Tomb `hero1-dog-live_2026-10-07_14-45-08` (the census's town save
sits on the title scene, so it was not used). Evidence, scripts and copied CSVs: `out/validation/pass-merge-20261013/`
(KEEP):

- `census.py` (why each render pass ended, draws per pass, what followed, same-RT reopens, marginal busy);
- `m3rank.py` (per boundary class: M5 1×/2× marginal busy, census `F + P·s²` fit, M3 Pro 2× estimate);
- `summ.py` (GPU ms, encoders, tiny passes, main-pass breaks, counters per arm);
- `arm.sh`, `batch2.sh`, `batch3.sh` (stop file `out/locks/agents-paused`), `parity.py`, `parity.txt`;
- `census-*.txt`, `m3rank-bower-off.txt`, `frame3000-b2.txt` (one frame's encoder sequence).

Goal: fewer tiny passes and encoder breaks for the M3 Pro at 2× (census: passes with ≤ 2 draws ≈ 9 ms of its 27 ms,
estimated), faithfully.

## 1. Boundary census

Per frame, `FABLE2_GPU_TIME_TRACE=1` pass trace, headings8, alias clears on (`census-*.txt`). "Tiny" = ≤ 2 draws.
"Reopen" = the next render pass binds the same attachments.

| Render pass ended by | Bower 2× | of which tiny | reopen | Old Tomb 2× | Forced? |
|---|---|---|---|---|---|
| **resolve** (compute dispatch must run outside the pass) | **73.4** | 53.8 | 5.1 | **30.6** | yes: the next draw binds other targets in 68 of 73 |
| RT change (next draw binds other attachments) | 10.6 | 6.7 | 0 | 6.2 | yes |
| separate resolve-clear pass (`rt_transfer_clear`) | 7.1 | 7.1 | 0 | 9.4 | read by a transfer before any pass binds the target (§14 of `resolution-scale.md`) |
| texture upload: sampled by the open command buffer | 3.2 | 1.5 | 2.1 | 2.0 | yes without texture renaming (§4) |
| texture upload: needs a shared-memory upload first (`fallback_upload`) | **1.3** | 0.2 | 1.3 | 0.3 | **no → fixed (§2)** |
| texture upload: resident source written in the open CB | 1.4 | 1.0 | 0.1 | 0.2 | yes (RAW) |
| texture upload: other (scaled, presented) | 0.2 | 0 | 0.2 | — | yes |
| shared-memory upload of a page the open CB read (WAR) | 1.8 | 0.4 | 1.8 | 0.3 | yes without buffer renaming |
| ownership transfer in its own pass | 3.4 + 1.0 | 4.3 | 0 | 6.4 + 2.0 | yes (self-depth / other) |
| command-buffer end, readback | 0.1 | 0 | 0.1 | 0.1 | yes |
| **Render passes** | **103.6** | 75.1 | | **57.4** | |
| Compute encoders (73 resolves, 21 texture untiles, 3 depth stores) | 97.4 | | | 48.4 | |
| Blit encoders (21 texture copies, 11 pre-commit uploads, 5 uploads) | 37.5 | | | 26.4 | |

- Resolves: ~68 colour (48 1×-class `c0`, 9 `c2`, 2.6 `c3`, 0.9 `c14`) and ~2.7 depth; only ~4 are MSAA sources.
- Command buffers: ~3 main ends per frame (PM4 primary buffer end), ~4 pre-commit and the presenter (unchanged).
- The main colour pass (2240×1440 2×MSAA RGBA16F + D32S8) is split into 4.3–4.6 passes per frame in Bower 2×:
  1 forced per-tile resolve, the rest texture/shared-memory upload breaks. Each extra split stores and reloads both
  attachments: ~168 MB of touched-rect traffic per break at 2× (`frame3000-b2.txt` shows a frame with 2).

### Ranking by estimated M3 Pro cost

`m3rank-bower-off.txt` fits each class's marginal busy time on the M5 at 1× and 2× to `F + P·s²` and applies the
census factors (M3 ≈ 1.62·F + 4.86·4·P). These are the classes' whole cost (their draws included), not only the
boundary:

| Class (Bower 2×) | per frame | M3 Pro 2× (est.) | Removable here? |
|---|---|---|---|
| resolve-ended passes (all) | 73.4 | 8.0 ms (tiny ones 4.3) | no: forced by RT changes; see §3 |
| resolve compute dispatches | 73.4 | 4.0 ms | only with a tile-memory resolve (§3) |
| texture-upload-ended passes | 6.1 | 6.5 ms (mostly main-pass draws) | the boundary part: bytes model below |
| RT-change-ended passes | 10.6 | 3.2 ms | no |
| transfer passes | 4.4 | 1.4 ms | no (contained) |
| separate clear passes | 7.1 | 0.7 ms | partly (§2.2) |
| shared-memory WAR breaks | 1.8 | 0.5 ms | no |

The boundary cost of a main-pass split is its extra store + load. Bytes model (§13 of `resolution-scale.md`): 168 MB
per split at 2×, M3 Pro ~150 GB/s → 1.1 ms pure, × 0.25–0.65 (the measured model-to-time ratio) ≈ **0.3–0.7 ms per
split on the M3 Pro at 2×**, ~0.04–0.1 ms on the M5 Max. So removable main-pass splits rank first among the
contained items.

## 2. Changes

Master switch `FABLE2_PASS_MERGE` (default on; `=0` turns both items off). Dev Launcher → Fixes → Render targets:
"Fewer pass breaks" plus one toggle per item. Header: `backends/metal/port/rex/graphics/metal/metal_pass_merge.h`.

### 2.1 Texture loads stay on the pre-commit route across their shared-memory upload (`FABLE2_PASS_MERGE_TEX_DIRECT`)

Before: a draw flush whose texture loads were all eligible for the pre-commit command buffer (weak-mac step 8) gave up
that route as soon as one load found part of its guest range not yet uploaded (`PrepareTextureDataLoadRanges` →
`AbortTexturePrecommit("texture:fallback_upload")`). The render pass ended, the upload and the rest of the loads went
into the open command buffer.

Now (`metal_texture_cache.cc`, `PrepareTextureDataLoadRanges`):

- The invalid pages are classified first (`GetUploadRouteInfo`). The route is kept only when **no page was read by
  the open command buffer or a standalone one**:
  - pages whose last reader has completed take the CPU direct write (a memcpy into the shared buffer, no GPU
    command), exactly as they would after the abort;
  - pages read only by committed command buffers still in flight take a staged copy in the pre-commit command buffer
    (the existing upload-precommit route; it waits for the newest committed main command buffer first). For that
    one request `SharedMemoryPrecommitAllowed` accepts an active texture pre-commit
    (`SetShmemPrecommitInTexturePrecommit`); the copy's blit encoder is closed right after, so the loads open their
    encoders after it in the same command buffer and wait on the fence it updates.
- Any other page, or a page that changes class between the check and the upload and ends up copied by a blit in the
  open command buffer (detected by the staged-copy counter), aborts the route exactly as before.
- Exactness: the loads read the same bytes at the same point in GPU order as before. Direct-written pages had no
  reader that could see the change; pre-commit copies land before the loads (fence chain) and after every committed
  reader (event wait). The existing pre-commit page tracking (`MarkPrecommitCopy`, current-submission access marks)
  is unchanged, so later uploads of those pages still go in-pass. `FABLE2_TEXTURE_PRECOMMIT_VERIFY=1` checks it (§5).

Counters: `tr_n_tex_pre_direct` (flushes kept), `tr_n_tex_pre_staged` (of which with a pre-commit copy),
`tr_n_tex_fb_{cur,inflight,sa}` (page classes seen).

### 2.2 A destination's resolve clear is drawn in its transfer pass (`FABLE2_PASS_MERGE_CLEAR_DST`)

Deferred resolve clears (§14) merge into the next draw pass that binds the target, but only when that target receives
no ownership transfers in the same update; otherwise the clear was flushed as its own pass. The in-pass machinery
already draws clears before transfer quads, the same order as "clear pass, then transfers". Now
(`MetalRenderTargetCache::Update`) the destination's clear stays queued for its draw pass, except when:

- a transfer into it reads the destination's own host depth (K1 stores that before the pass, so it must hold the
  clear);
- its transfers fall back to a separate pass (the clear is flushed first, as before);
- the draw is never encoded (`FlushPendingDrawPassTransfers` with requeue: a clear with pending transfers is encoded
  before them instead of being re-queued).

Counter `tr_n_dc_dst_merged`. It merges only **0.3 clears per frame** in Bower (0 in Old Tomb): the other ~6.8 clear
passes are flushed because a transfer reads the cleared target as its **source** (the main depth after the second
tile's resolve, read by the next frame's 2×→4× transfer; the UI target; small post targets), which needs the clear's
value forwarded through the transfer's format conversion (not done, §4).

## 3. Resolve-ended passes: what a tile-memory resolve could remove (estimate only, not built)

Asked by main as a costing: 73 resolves end a render pass per frame in Bower 2× (54 of them ≤ 2 draws).

| Option | Covers | M3 Pro 2× est. | Notes |
|---|---|---|---|
| MSAA resolve via a store action | ≤ 4/frame (MSAA sources only) | < 0.2 ms | Xenos resolves still write guest/scaled memory with format, tiling and endian conversion, so a compute pass stays. No pass removed |
| Tile shader (imageblock) resolve inside the pass | ~68 colour resolves (depth is not readable from tile functions) | **1.5–2.4 ms** (40–60% of the 4.0 ms resolve cost: the read-back half and the per-encoder fixed cost) | Removes ~68 compute encoders and the attachment read-back; the store stays (EDRAM contents persist). XL: ~33 resolve variants as bit-exact tile functions; §13 K3 judged it the same |
| Defer/batch the resolve dispatches | ~5/frame reopen the same RT, and later draws overwrite the resolved pixels | < 0.1 ms | Batching computes into one encoder removes no render pass |

Removing the tiny passes themselves would need rendering straight into the sampled texture with the EDRAM store
skipped, which needs draw-stream lookahead to prove the EDRAM contents dead. This is now a separate task (tile-resolve
agent), waiting on the maintainer.

## 4. Not done

- ~~**Texture renaming for `sampled_current`**~~ done in `docs/tex-rename-20261013.md` (default on 2026-10-14; main-pass texture splits 1.6–1.7 → < 0.1 per frame). Original note: (1.5–3.2 breaks per frame, 1.4–1.7 of them in the main pass): upload the
  new contents into a fresh texture in the pre-commit command buffer. Prepared draws already hold bindless slot indices
  of the old view when the flush runs, and the RTT keep/fused state points at the texture object, so renaming touches
  the slot allocator, the prepared-draw records and RTT. Estimated M3 Pro 2×: 1.4 splits × 0.3–0.7 ms ≈ 0.4–1 ms. The
  largest contained-looking item left; L effort.
- **Shared-memory WAR breaks** (1.8/frame): need buffer renaming of guest pages; a part of the fallback breaks fixed
  in §2.1 resurfaces here (+0.4/frame in Bower 2×), later in the same pass.
- **Clear value forwarding through a transfer** (§2.2): the ~6.8 remaining clear passes per frame (0.7 ms est. on the
  M3 Pro, all included).
- The remaining 4×MSAA one-draw passes are the game's own z = 0.75 clear and stencil draws (alias-clear P7, and
  real shaders), not XDK clear quads.

## 5. Results

### Performance (M5 Max, headings8, uncapped, `FABLE2_GPU_TIME_TRACE=1`; off = `FABLE2_PASS_MERGE=0` on the same build)

Batch 2 (short headings8, 8 × 0.75 s; window = the sweep), one run per arm, all `ok`, 0 AV, contention 0, no build
running:

| Run | GPU ms off → on | p95 | render / compute / blit encoders | tiny render | main-pass breaks | `fallback_upload` breaks |
|---|---|---|---|---|---|---|
| Bower 1× | 6.94 → 6.82 | 8.53 → 8.37 | 101.5/89.6/24.5 → 100.0/89.1/23.8 | 73.2 → 72.0 | 2.98 → 2.69 | 0.34 → 0 |
| Bower 2× | 8.93 → 8.97 | 10.46 → 10.64 | 103.7/97.5/37.6 → 102.2/95.6/35.0 | 75.0 → 74.4 | 4.30 → 2.94 | 1.31 → 0 |
| Bower 3× | 12.86 → 12.65 | 14.63 → 14.46 | 101.5/97.6/35.8 → 100.4/96.8/35.7 | 74.7 → 74.2 | 2.86 → 2.67 | 0.20 → 0 |
| Old Tomb 1× | 4.45 → 4.38 | 7.94 → 8.07 | 53.5/47.3/22.4 → 53.5/47.3/21.8 | 40.8 → 41.1 | 1.72 → 1.43 | 0.28 → 0 |
| Old Tomb 2× | 6.61 → 6.66 | 11.16 → 11.42 | 57.4/48.4/26.3 → 57.4/48.6/26.7 | 45.8 → 46.1 | 1.37 → 1.08 | 0.28 → 0 |

Batch 1 (headings8-long, 2.5 s holds, item 2.1 only): Bower 1× 6.79 → 6.74, 2× 8.78 → 8.74, 3× 12.50 → 12.35, Old Tomb
2× 6.95 → 6.86; main-pass breaks 3.0 → 2.7, 3.5 → 2.9, 3.6 → 2.8, 2.3 → 1.1. (The Bower 2× pair overlapped a build in
another checkout; its FPS is not comparable, its GPU time is.)

- **On the M5 Max the GPU change is within run-to-run noise** (−0.13 to +0.05 ms), as expected: a removed split saves
  ~0.04–0.1 ms of bandwidth there. Command buffers per frame are unchanged (~3 main + pre-commit + presenter).
- Mechanism: `fallback_upload` breaks go to 0 in every run; main-pass splits fall by 0.3–1.4 per frame (Bower 2×:
  −1.4 in batch 2, −0.6 in batch 1); render passes −0 to −1.5 per frame. About 0.4 per frame of the removed breaks
  come back later in the same pass as `upload_pre:war_current`.

### M3 Pro estimate

- Census whole-frame model (M5 1×/2× GPU → `1.62·F + 4.86·4·P`): 22.8 → 22.8 ms (batch 1), 23.0 → 23.8 ms (batch 2).
  The M5 deltas are noise-sized, so this model cannot resolve the change.
- Bytes model for the removed main-pass splits: Bower 2× nets ~−0.6 to −1.0 splits per frame (after the WAR
  resurfacing) × 0.3–0.7 ms ≈ **−0.2 to −0.7 ms on the M3 Pro at 2×** (≈ −0.05 to −0.2 ms at 1×). Old Tomb: −0.1 to
  −0.4 ms. The clear-dst merge (0.3 small clears) adds < 0.05 ms. This is an estimate; an M3 Pro run of the
  benchmark kit with `FABLE2_PASS_MERGE=0` vs default would measure it.

### Correctness

spin5 at 60 FPS with captures, on vs off and a second off run (noise floor), same build (after merging the readiness
tooling, so captures start when the game reports ready). All 14 sessions: verdict `ok`, 0 AV. Per-region linear-light
parity (`parity.py`, `parity.txt`), idle phase median / max:

| Scene | on vs off | off2 vs off (noise floor) |
|---|---|---|
| Bower 1× | 0.32% / 1.39% | 1.10% / 7.72% |
| Bower 2× | 0.66% / 5.36% | 0.91% / 12.91% |
| Old Tomb 1× | 1.60% / 79.6% (one near-black region, 0.008 vs 0.015 linear) | 1.60% / 33.0% (same region) |
| Old Tomb 2× | 1.37% / 20.2% | 2.15% / 20.2% |

On vs off sits inside the off-vs-off spread everywhere (the Old Tomb max is the same dark corner region in both
pairings). Data check: `FABLE2_TEXTURE_PRECOMMIT_VERIFY=1` spins with the change on (it copies each pre-commit
load's source in the pre-commit command buffer and again at the load's original position, then compares): Bower 2×
**11,000 ok, 0 mismatch** (537 MB compared), Old Tomb 2× **4,096 ok, 0 mismatch** (420 MB); both runs took the new
route (direct-write and pre-commit-copy flushes counted). An earlier spin set on the pre-readiness tooling captured
loading screens in some arms and was discarded (`old-spins/`).

## Switches

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_PASS_MERGE=0` | on | Master: both items below off |
| `FABLE2_PASS_MERGE_TEX_DIRECT=0` | on | §2.1 off: a texture flush needing a shared-memory upload leaves the pre-commit route |
| `FABLE2_PASS_MERGE_CLEAR_DST=0` | on | §2.2 off: a destination's deferred clear is flushed as its own pass when it receives transfers |

Counters (frame CSV with `FABLE2_GPU_TIME_TRACE=1`): `tr_n_tex_pre_direct`, `tr_n_tex_pre_staged`,
`tr_n_tex_fb_cur`, `tr_n_tex_fb_inflight`, `tr_n_tex_fb_sa`, `tr_n_dc_dst_merged`.

Files: `metal_pass_merge.h` (new), `metal_texture_cache.cc`, `metal_command_processor.cc/.h`,
`metal_render_target_cache.cc`, `metal_shared_memory.h`, `tools/macos/Shared/LaunchSettings.swift`,
`docs/bug-patterns.md` (row 78).
