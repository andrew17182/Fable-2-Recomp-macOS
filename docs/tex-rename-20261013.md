# Texture renaming for `sampled_current` (2026-10-13/14)

Branch `agent-1/20261013-2057-tex-rename` (from `perf-staging` 7733fb3). M5 Max. Saves: Bower Lake
`hero1-dog-live_2026-10-11_11-20-20`, Old Tomb `hero1-dog-live_2026-10-07_14-45-08`, Bowerstone Old Town
`hero1-dog-live_2026-10-04_14-07-34`. Evidence: `out/validation/tex-rename/` (KEEP) — `arm.sh`, `perf.sh`, `summ.py`,
`parity.py` (`PARITY_SKIP=N` drops the first N captures per phase), `lum.py`, one folder per run. All perf arms used the
pinned agent shader-cache snapshot `b86f48d05fb6472a@12`, with no contention.

Goal: the M3 Pro at 2× and 60 FPS, faithfully. The pass-merge census (`docs/pass-merge-20261013.md` §4) left
`texture:sampled_current` as the largest avoidable main-pass break. When a texture load targets a texture that a draw
of the open command buffer has already sampled, the load can't go to the pre-commit command buffer (it would run
before that draw), so the 2240×1440 2×MSAA pass ends. Each split stores and reloads about 168 MB at 2×.

## 1. What the loads were (diagnostic, `FABLE2_TEX_RENAME_TRACE=1`)

Bower 2×, with renaming off: 1.42 `sampled_current` main-pass breaks per frame.
- Main-pass loads are almost all **full** reloads (every level):
  - the 1024² `k_24_8_FLOAT` shadow map (resident source);
  - a 1650×1815 compressed texture;
  - 16×16 CPU-source textures;
  - a few full mip chains.
- Partial loads (base only, of a texture with mips) are about 1 in 4096.
- Two kinds of planned load **never write**, yet the classifier counted them as loads that end the pass:
  - **stale plans:** the texture was already reloaded by an earlier flush;
  - **RTT keeps:** the texture already holds the contents (the 640×384 pass's scaled 2240×1440 texture, about 1 in 4 of
    all `sampled_current` loads).

## 2. Design (approved by main)

Renaming works the way a native driver handles it, but the decision is made when the draw is **prepared**, not when
the queue flushes, so no prepared draw ever needs patching.

- **Where.** In `PrepareTextureMaterialization` (`metal_texture_cache.cc`), which runs before `PrepareDrawConstants`
  writes the draw's bindless descriptor indices. A planned load of a texture whose backing the open command buffer has
  sampled calls `TexRenameTry`. That runs only when a render pass is open and texture pre-commit is on.
- **Identity stays.** The `MetalTexture` keeps its identity: cache entry, bindings, RTT registry, fused plans and the
  tile-resolve checks all reach it through `metal_texture()`. Only its backing `MTL::Texture` is swapped
  (`MetalTexture::RenameBacking`) for a pooled texture with the same descriptor.
- **Fencing comes from the bindless heap.** The old backing's view slots are retired through
  `RetireViewBindlessIndex` and freed after the submission completes. Their heap entries keep pointing at the old
  backing, so every draw already encoded keeps sampling the old contents. The draw being prepared, and every later one,
  gets new slots. The descriptor-index CBVs are invalidated (`TexRenameInvalidateDescriptors`). They use
  pack-and-compare, so the new index forces a new slice, and slices that older draws share are never written.
- **Old backing.** The old backing, its swizzle views and its direct-write view go on a retire list keyed by the
  current submission. On completion they move to a pool (or are freed). Pooled backings idle for 1024 submissions are
  freed. `FABLE2_TEX_RENAME_MB` caps pool plus in-flight at 128 MB; a single texture is renamed only up to 64 MB.
- **The load itself.** At flush the load sees the fresh backing, which nothing has sampled, so it takes the pre-commit
  route and the pass stays open. If another load in the same flush ends the pass anyway, the load writes the fresh
  backing in the command buffer. Both are exact.
- **Only safe cases are renamed;** everything else keeps today's split. Each fallback has a counter:

  | Fallback | Counter |
  |---|---|
  | Partial load | `tr_n_trn_fb_partial` |
  | RTT keep possible (it would leave the fresh backing unwritten) | `fb_keep` |
  | Another eligibility blocker (scaled or resident source written in the open command buffer) | `fb_other` |
  | A queued, not yet encoded draw samples the old backing (in the split path those draws see the new contents) | `fb_queued`, `fb_queued_load` |
  | Memory cap | `fb_mem` |
  | Presented, 3D, 3D-as-2D, or non-private storage | `fb_kind` |
  | No pass open | `fb_nopass` |

  A second load sharing a pending rename is counted as `trn_share`.
- **Settle (safety net).** If a renamed texture's load didn't write the fresh backing (failed, kept or skipped), or
  its draw never flushed (an orphan, settled at the next resolve or command-buffer end), the old backing is copied
  into it before anything samples it (ending the pass). That gives exactly the split path's state. In every
  validation run: `tr_n_trn_settle` 0, `tr_n_trn_orphan` 0.

### No-op loads (`FABLE2_TEX_RENAME_NOOP`, sub-switch, default on with renaming)

`ClassifyMaterializationPlan` now leaves out planned loads that will not write (`PlannedLoadIsNoop`):
- the texture is up to date again (`TextureCache::LoadTexturesData` re-reads the flags and skips it);
- RTT keeps it whole (same conditions as `RttTryKeep`).

A flush made only of eligible and no-op loads takes the pre-commit route. If a no-op turns into a real write between
classification and execution (an asynchronous CPU invalidation), the existing guard in `TryGpuLoadTexture` ends the
pre-commit route before writing a texture the open command buffer sampled. That guard fired 0.002 times per frame.

### Interactions

- **RTT fused writes and keeps** go through `metal_texture()`, so they write the new backing. Resolves flush the
  prepared-draw queue first, so pending loads land before any fused write.
- **Tile-resolve hazard check:** after a rename, the pass's sampled set holds the old backing and the fused target is
  the new one, so it is correctly not a hazard. The bind serial stays conservative.
- **memexport** only invalidates shared memory, which leads to a normal reload.
- **Texture-cache resolve tracking** (`rtt_valid_` and the mips flag) is per `MetalTexture` and stays correct, because
  only full loads are renamed.

## 3. Exactness

`FABLE2_TEX_RENAME_VERIFY=1` (`TexRenameSettleEncode`, ends the pass): after the flush, the split path's load is run
again into the **old** backing at the original position (after every draw that sampled it). Both backings are read back
(every level and slice) and compared byte for byte on completion.

| spin5 verify | renamed loads compared | mismatched |
|---|---|---|
| Bower 1× / 2× / 3× | ≥1024 each (2.8 MB / 1.44 GB / 1.52 GB) | **0** |
| Old Tomb 1× / 2× | ≥2048 each | **0** |
| Town 2× / 3× | ≥1024 each (1.46 GB / 1.39 GB) | **0** |
| Bower 2×, earlier build | 1024 | 0 |
| Negative control (`FABLE2_TEX_RENAME_VERIFY_BREAK=1`: the fresh backing's level 0 shifted by one 4-texel block) | 1451 | **1027** |

The ok count under the negative control (about 420) is uniform or tiny textures, where a shift changes nothing.

**Finding: almost every renamed reload rewrites identical bytes.** With no shift (an earlier control that compared the
old contents with the new), 0 of 1024 pairs differed, although the shadow map's hash changes between frames. These
same-command-buffer reloads come from page-granular invalidations by writes next to the texture (§6).

Parity (spin5, 60 FPS, 4×4 linear-light regions, idle phases, first 3 captures dropped; see below):

| Pair | idle median / max | idle2 median / max |
|---|---|---|
| Bower 2× on vs off | 0.19% / 1.18% | 12.4% / 61.6% |
| Bower 2× off vs off (noise) | 0.46% / 2.12% | 12.7% / 62.5% |
| Old Tomb 2× on vs off | 1.10% / 11.0% | 0.88% / 4.29% |
| Town 2× on vs off (two on runs) | 3.00% / 10.0%; 0.78% / 3.31% | 6.5% / 101%; 7.4% / 111% |
| Town 2× off vs off (noise) | 1.01% / 11.0% | 6.2% / 95.8% |

**Why the first 3 captures are dropped.** In town the idle phase can start during the area's fade-in from black, and
the start time jitters by about 0.3 s. One "on" run's first captures were dark (luminance 0.018 → 0.176 over 4
captures, HUD unaffected), which looked like a missing-lighting bug. The same shape appears in no-op-off and repeated
runs, and per-capture luminance (`lum.py`) shows the fade.

Full set on the default build:
- spin5 Bower 2× ok;
- area at 2×: ok, 0 AV;
- bootloop: 2/2 pass.

## 4. Performance (M5 Max, headings8-long, uncapped, `f2diag gpu` over the sweep window; off = `FABLE2_TEX_RENAME=0`)

| Scene / scale | GPU ms off → on | FPS off → on | main-pass splits per frame | texture-caused splits | render passes per frame |
|---|---|---|---|---|---|
| Bower 1× | 6.11 → **5.88** | 139.7 → 140.3 | 2.90 → 1.36 | 1.59 → 0.08 | 96.2 → 88.8 |
| Bower 2× | 7.65 → **7.45**; 7.57 → **7.32** | 121.8 → 123.7; 119.4 → 121.5 | 2.91 → 1.35; 3.14 → 1.22 | 1.69 → 0.09 | 97.3 → 92.6 |
| Bower 3× | 12.04 → **11.69** | 81.9 → 84.4 | 2.78 → 1.10 | 1.74 → 0.02 | 99.2 → 92.2 |
| Old Tomb 2× | 5.35 → **5.22** | 171.4 → 174.2 | 1.22 → 1.21 | 0.12 → 0.10 | 56.8 → 54.5 |
| Town 2× (3 pairs) | 9.65 → 9.80 (+0.15) | 82.3 → 81.5 (−0.8%) | 2.50 → 1.27 | 1.30 → 0.06 | 75.7 → 72.4 |
| Town 3× (2 pairs) | 12.43 → **12.34** | 74.3 → 75.5 | 2.74 → 1.30 | 1.54 → 0.09 | 76.5 → 73.3 |

- **Gains everywhere the GPU limits the frame.** Bower: −0.20 to −0.35 ms; Old Tomb 2×: −0.13 ms; town 3×: −0.09 ms.
  Up to about 7 fewer render passes per frame. `upload_pre:war_current` resurfaces from 0.08 to 0.23 per frame (pattern
  78).
- **Town 2× on the M5 is CPU-limited.** The GPU is 80% busy, and the GPU Commands thread is never blocked and at 99.5%
  of a core. There the feature costs about 1% of the GPU Commands thread (12.07 → 12.21 ms per frame, 3 pairs each) and
  FPS drops 0.8%. GPU ms reads 1.5% higher, but every pass, including untouched ones, reads about 3% longer, which
  points to the GPU clock in a non-GPU-bound scene rather than added work. Town 3× is GPU-bound and shows a gain.
- **Memory:** pool plus in-flight old backings is about 12 MB in Bower 2×, 13 MB at 3× and 6–10 MB in town
  (`tr_n_trn_pool_kb`, `tr_n_trn_retired_kb`). The cap is 128 MB.

**After merging perf-staging 9158b86 (mip-lod, soak).**
- Bower 2×, default build vs `FABLE2_TEX_RENAME=0`, snapshot `b86f48d05fb6472a@66`, 2 pairs: GPU 7.48 / 7.47 →
  **7.28 / 7.29 ms**, FPS 123.3 / 123.5 → 124.8 / 125.2, main-pass splits 2.95 / 2.90 → 1.43 / 1.37.
- Verify spins (Bower 2× / 3×, town 2×): 1024 renamed loads each, 0 mismatches.
- Default spins (Bower 2×, Old Tomb 2×): ok.

### M3 Pro estimate (not measured)

- **Bytes model** (`docs/pass-merge-20261013.md` §1): about 0.3–0.7 ms per removed main-pass split at 2×.
  - Bower 2×: −1.55 splits per frame ≈ **−0.5 to −1.1 ms**.
  - Town 2×: −1.25 splits ≈ −0.4 to −0.9 ms.
  - Old Tomb: about 0.
- **Census model** (`1.62·F + 4.86·4·P` from the Bower 1×/2× deltas, −0.24 / −0.23 ms): almost all of the delta falls
  into the fixed part (F) ≈ **−0.4 ms**. As in pass-merge, the M5 does not feel the split's bandwidth, so this model is
  the floor.
- **Range: −0.4 to −1.1 ms at 2× in Bower Lake.**
- **Risk:** the town CPU cost above. On the M3 Pro the GPU Commands thread is slower, but 2× there is GPU-bound
  (27 ms GPU in Bower).

**Default: on.** The verify is exact (0 mismatches in 7 runs across three scenes and 1×–3×), the negative control
catches errors, parity is within noise, there are gains in every GPU-bound case, and memory stays under 15 MB. The one
cost is the CPU-bound town 2× on the M5 (−0.8% FPS).

## 5. Fix found on the way: resolve narrowing dropped a CPU write (bug-patterns row 83)

`RttOnInvalidate` (default-on RTT keep and the 1× narrowing) narrows an invalidation to the exact resolve extent while
`rtt_in_resolve_` is set. That flag is set outside the global lock. A CPU write fault on another thread can take the
lock during a resolve, and its invalidation (for example `1C900000-1C9FFFFF`, a streamed DXT1 region) was then narrowed
to the resolve's range and dropped for every texture outside it.

Seen as 4 mismatches out of 1280 in the §6 verify (DXT1 textures at `1C995000` and `1C9E4000`). Fix: only GPU
invalidations are narrowed (`invalidated_by_gpu`). `FABLE2_RTT_NARROW_CPU_FIX=0` restores the old condition (Dev
Launcher toggle). After the fix: 0 mismatches in 8 verify runs.

## 6. Skipping unchanged reloads (`FABLE2_TEX_UNCHANGED`, default off): costed and built, no gain measured

Main asked whether reloads of byte-identical data could be skipped outright, as a native driver would, since §3 showed
the renamed reloads rewrite the same bytes.

- **Why bug-pattern 58's mechanism didn't cover them.** RTT keep / RTT_1X tracks scaled textures, and unscaled ones only
  at 1×. The native-scale shadow map at 2×/3× is unscaled, so it isn't tracked.
- **Measurement first.** `FABLE2_TEX_NX_DIAG=1` tracks every unscaled, tiled 2D texture at scale > 1 with exact ranges
  (`nx_valid_`). Any write over its exact guest ranges clears it: resolves narrowed to their extent, everything else
  page-granular. Result in Bower 2× and town 2×: **2.1–2.2 reloads per frame (8.2 MB of guest data) had no write since
  their last load**, about 1 per frame of them a rename.
- **Built.** `FABLE2_TEX_UNCHANGED=1`: such a load returns without writing. It is also a no-op for classification and
  never renamed. Only textures that were rename candidates are tracked (`NxTrack`), so the per-invalidation scan stays
  short; tracking every candidate cost about 0.1–0.17 ms of GPU Commands per frame.
- **Exactness.** `FABLE2_TEX_UNCHANGED_VERIFY=1` runs a reference load for each kept texture and compares it with the
  texture (the existing `[rtt] verify` readback).
  - Before the §5 fix: 4 mismatches out of 1280, which is how the race was found.
  - After it: about 8,600 kept textures compared in Bower 2× (3 runs) and 3×, town 2× (2 runs) and 3×, and Old Tomb
    2× (none kept): **0 mismatches**.
- **Performance** (2 pairs each, renaming on, unchanged off vs on):
  - Bower 2×: GPU 7.40 / 7.38 → 7.42 / 7.44 ms;
  - town 2×: 10.51 / 10.49 → 10.41 / 10.49 ms;
  - GPU Commands: flat within noise.
  - Renames fall from 2.7 to 1.7–2.1 per frame, and the pool from about 5 MB to under 1 MB.
- **M3 Pro estimate:** about 16 MB per frame of load traffic ≈ 0.03–0.07 ms.
- **Default: off.** It is exact but brings no measurable gain. It shares RTT_1X's residual assumption: a CPU write
  between an upload and the texture's watch re-arm could go unseen, as it can for the base texture cache.

## 7. Not done / next

- Partial loads (base only, with mips): about 1 in 4096. They would need the old backing's other levels copied into the
  new one, which is only safe in the pre-commit command buffer if the open command buffer has not written the old
  backing.
- Queued draws that sample the old backing (`fb_queued`, about 0.1 per frame) and the chained `fb_queued_load`.
- The remaining main-pass breaks in Bower 2×:
  - `resolve:tile` 1.0 (the per-tile resolve, forced);
  - `upload_pre:war_current` 0.2 (needs buffer renaming of guest pages);
  - `texture:resident_current` 0.02–0.06.
- The town 2× CPU cost (1%): rename bookkeeping (views, slots, descriptor re-upload) and about 4 more pre-commit
  flushes per frame.

## Switches

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_TEX_RENAME=0` | **on** | Old split path (Dev Launcher → Fixes → Render targets → "Texture renaming") |
| `FABLE2_TEX_RENAME_NOOP=0` | on | Planned loads that write nothing end the pass again (launcher toggle) |
| `FABLE2_TEX_UNCHANGED=1` | off | Skip reloads of unscaled textures nothing wrote since their last load (launcher toggle, experimental) |
| `FABLE2_RTT_NARROW_CPU_FIX=0` | on | §5 race fix off (launcher toggle) |
| `FABLE2_TEX_RENAME_MB=<n>` | 128 | Cap for pooled + in-flight old backings |
| `FABLE2_TEX_RENAME_VERIFY=1` | off | Split-path reference load + byte compare (launcher, Verify modes) |
| `FABLE2_TEX_RENAME_VERIFY_BREAK=1` | off | Negative control |
| `FABLE2_TEX_UNCHANGED_VERIFY=1` | off | Reference load for each skipped reload (launcher, Verify modes) |
| `FABLE2_TEX_RENAME_TRACE=1` | off | Histogram of `sampled_current` loads and rename fallbacks |
| `FABLE2_TEX_NX_DIAG=1`, `FABLE2_TEX_NX_TRACE=<hex base>` | off | Unchanged-reload census; invalidation trace for one texture |

Counters (frame CSV, `FABLE2_GPU_TIME_TRACE=1`):
- `tr_n_trn_rename`, `pool_hit`, `share`, `settle`, `orphan`, `wasted`;
- the fallbacks `fb_partial`, `fb_keep`, `fb_queued`, `fb_queued_load`, `fb_other`, `fb_mem`, `fb_kind`, `fb_nopass`;
- `noop_stale`, `noop_keep`;
- the gauges `pool_kb`, `retired_kb`;
- `tr_n_tex_brk_samp_only`;
- `tr_n_nx_same`, `tr_n_nx_same_kb`, `tr_n_nx_same_renamed`, `tr_n_nx_changed`.

Files:
- `backends/metal/port/rex/graphics/metal/metal_tex_rename.cc` (new);
- `metal_texture_cache.cc/.h`;
- `metal_resolve_to_texture.cc` (narrowing fix, unchanged tracking);
- `metal_command_processor.cc/.h`;
- `metal_shared_memory.h` (counters);
- `backends/metal/metal-sources.cmake`;
- `tools/macos/Shared/LaunchSettings.swift`;
- `docs/bug-patterns.md` (rows 82, 83), `docs/reference.md`, `docs/README.md`, `docs/pass-merge-20261013.md`.
