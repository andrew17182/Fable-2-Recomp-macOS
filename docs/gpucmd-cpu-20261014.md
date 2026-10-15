# GPU Commands thread CPU, round 2 (2026-10-14)

Branch `agent-1/20261014-0037-gpucmd-cpu` (from `perf-staging` 7d0b6cc, merged with 87d99d2 / parity-tools). M5 Max.

Saves:
- Bowerstone Old Town: `hero1-dog-live_2026-10-04_14-07-34`;
- Bower Lake: `hero1-dog-live_2026-10-11_11-20-20`.

Evidence, scripts and copied results: `out/validation/gpucmd-20261014/` (KEEP):
- `run.sh` (`prof` = headings8-long plus a `sample` of our own pid, started on the plan's first hold event, with no fixed wait), `ab.sh`, `par.sh`, `bstate.sh`;
- `incl.py`, `self.py`, `sub.py`, `callers.py`, `thread.py`, `cpf.py`;
- `burst.py` / `region.py` (region diff per burst and frame shift), `label.py` / `crop.py`;
- `regen-patch.sh` and `sdk-orig/` (to rebuild the SDK patch);
- one folder per run.

Every run ended `ok` with 0 AV and no contention, and our pid was gone after close.

Why: town 2× is CPU-bound on the M5 Max. The GPU Commands thread is ~12.5 ms/frame at 99.5% of a core, and FPS is ~80. On weaker P-cores (base M1, M3 Pro) the same thread is the frame limit in busy scenes.

## 1. Profile (`sample`, GPU Commands thread, headings8-long, uncapped)

`sample` slows the game (p95 frame time ~170 ms while it runs), so these profiles rank hotspots only. They are not timings.

Profiles: `p-t1`, `p-t2`, `p-b2` before; `q-t1`, `q-t2` after. Shares of the thread:

| Item | Town 1× | Town 2× | Bower 2× | Notes |
|---|---|---|---|---|
| `IssueDrawImpl` (whole draw path) | 80% | 82% | 76% | |
| Encode / submit prepared draws | 32% | 29% | 30% | Driver encoder work ~10% (structural) |
| `PrepareDrawConstants` | 21% | 19% | 15% | Bindless lookups, system constants, sampler checks |
| **`TextureCache::GetHostTextureKey`** (SDK) | 0.1% | **9.4%** | **4.9%** | At scale > 1 only. Guest layout recomputed (`GetGuestTextureLayout` 4.4%), then `IsRangeScaledResolved` under the **contended** global lock (`__psynch_mutexwait` 1.5%). Called 2–3× per changed binding per draw: `MayRequestTexturesLoadData` from `IssueDrawImpl`, again from `MetalTextureCache::RequestTextures`, then `FindOrCreateTexture` |
| `BindingInfoFromFetchConstant` (+ subresources, swizzle signs) | 2.7% | 2.3% | 1.5% | Fetch constants re-decoded per draw for every out-of-sync slot, up to 3× |
| Register writes (`TryWriteKnownRegisterRangeFromMem`) | 3.7% | 3.2% | 3.2% | Includes the copy |
| `GetSamplerParameters` | 2.9% | 2.6% | 1.3% | Built twice per sampler when descriptors change |
| `RetireFenceWaitedSharedMemoryWrites` | 1.6% | 1.3% | 0.5% | Copies the pending list into a 64-entry stack array |
| `NoteTexturesSampledByCurrentCommandBuffer` + pass sampled sets | 1.2% + ~1% | 1.2% + ~1% | 0.9% | `std::unordered_set` node malloc/free on every refill |
| `CanQueuePreparedDraw` | 1.2% | 1.0% | 0.6% | Re-sums every queued draw's ranges for each new draw (quadratic in queue length) |
| `vector<VertexBinding>` copy per draw | ~1% | ~1% | ~1% | Deep copy (nested attribute vectors). `DispatchDraw` only reads `empty()` |
| Shared-memory uploads, `mprotect`, `memmove` | 6–7% | 6–7% | 7% | Inherent |
| tex-rename (`TexRenameTry` and helpers) | 0.2% | 0.3% | 0.2% | The "1% town cost" of tex-rename is not in its own code (§5) |
| Native tess/points CPU paths | <0.1% | <0.1% | <0.1% | |

## 2. Changes

All of them are exact: the same draws, state, bindings and order are encoded. Only host-side bookkeeping changed.

### A. Texture key memo (`FABLE2_TEX_KEY_MEMO`, default on; SDK patch `backends/metal/sdk-metal-gpucmd.patch`)

**Host key memo** (`TextureCache::GetHostTextureKey`):
- A 1024-entry direct-mapped table: guest key → host key, plus the generation it was computed at.
- New `scaled_resolve_generation_` (atomic). It is bumped under the global lock, after the bits change, whenever a page's scaled-resolve bit **actually** changes:
  - `MarkRangeAsResolved` (only when a page was not already marked, so repeated resolves to the same pages keep the memo hot);
  - `MarkRangeAsResolvedUnscaled`;
  - `ScaledResolveGlobalWatchCallback`.
- A hit needs `generation == current`, read with acquire before the lookup. It returns what the locked check would have returned at that moment, because any interleaving of the memo is also an interleaving of the original code. No lock is taken on a hit.
- At 1× (and for untiled or unsupported formats) the function returns as before.
- Command-processor thread only, like `textures_` and the bindings.

**Fetch-decode memo** (`BindingInfoFromFetchConstantMemo`):
- A 512-entry direct-mapped table keyed by the six fetch constant dwords. `BindingInfoFromFetchConstant` is a pure function of them.
- Only valid results are memoized, so the invalid-constant warnings still fire every time.
- It is used by `RequestTextures`, `MayRequestTexturesLoadData` and `MetalTextureCache::PrepareTextureMaterialization`.
- An earlier per-slot version hit 69% of lookups; the hashed one hits 97%.

**Verify** (`FABLE2_TEX_KEY_MEMO_VERIFY=1`): every hit also runs the uncached path and compares. A `[tex-key-memo]` line every 2^20 lookups gives hits, misses and mismatches.

| Verify run | Host key hits / misses | Fetch hits / misses | Mismatches |
|---|---|---|---|
| Town 2× spin5 | 7.49 M / 0.20 M | 8.79 M / 0.30 M | **0 / 0** |
| Bower 3× spin5 | 2.60 M / 0.17 M | 3.36 M / 0.17 M | **0 / 0** |
| Area transition at 2× (`run area`) | 13.9 M / 0.89 M | 18.5 M / 1.30 M | **0 / 0** |

Build note: the patch is the 5th entry of `PATCHED_TREES` in `tools/build_macos_metal.py`. Changing it re-extracts the SDK tree and rebuilds the backend once. It is left out of `SDK_PATCHES` in `tools/fable2_agent_cache.py` on purpose: it has no shader hunks, and listing it would change the agent shader-cache key for no shader change.

### B. Per-draw trims (`FABLE2_DRAW_PREP_LITE`, default on; Metal backend)

1. **Unchanged fetch constants keep their binding in sync.**
   - `WriteFetchConstantsFromMem` and the single-register path clear a slot's `texture_bindings_in_sync_` bit only if one of its dwords really changes.
   - The game rewrites identical texture fetch constants per material. Re-parsing them gives the same key, signedness and swizzle.
   - The shader-side `DirtyFetchConstantDwords` is unchanged.
2. **No vertex-binding vector copy.** `PreparedDraw::has_vertex_bindings` replaces the per-draw deep copy, since `DispatchDraw` reads only `empty()`.
3. **Sampler parameters built once per draw.** The bindless sampler lookup reuses `current_samplers_*` (`GetBindlessSamplerIndexForParameters`) instead of rebuilding the parameters from the registers.
4. **Queue budget from running totals.** `CanQueuePreparedDraw` adds the new draw to `prepared_draw_queue_range_total_` / `_byte_total_`. They are kept at push and at queue take, and queued records are not modified. It no longer re-sums the whole queue.
5. **Early exit in `RetireFenceWaitedSharedMemoryWrites`** when no fence-waited entry overlaps and no collapse is due.
   - The rebuilt list would equal the old one.
   - In practice an overlapping entry usually exists, so this barely fires: the function is still 1.5% after the change.
6. **`FlatPtrSet`** (`metal_ptr_set.h`): an open-addressing pointer set that keeps its storage on `clear()`.
   - It replaces `std::unordered_set` for `cb_sampled_textures_`, `pass_sampled_textures_`, `pass_sampled_bases_` and `pass_tile_written_`.
   - These sets only use insert, count, clear and empty; nothing iterates or erases them.
   - With the switch off it forwards to `std::unordered_set`.

## 3. Results (headings8-long, uncapped, `--cpu`, pinned agent cache, no contention)

GPU Commands ms/frame and FPS over the sweep window. "Off" = `FABLE2_TEX_KEY_MEMO=0 FABLE2_DRAW_PREP_LITE=0`, same build.

Round A (pre-merge build, per-slot fetch memo, cache `b86f48d05fb6472a@67`) and round B (final build, merged 87d99d2, cache `@69`):

| Scene | Off ms/frame | On ms/frame | Δ | Off FPS | On FPS | Δ FPS |
|---|---|---|---|---|---|---|
| Town 2× (3 pairs: A, A, B) | 12.69 / 12.48 / 12.52 | 10.95 / 10.95 / **10.75** | **−1.68 ms (−13.4%)** | 78.4 / 79.7 / 79.5 | 91.0 / 91.1 / **92.7** | **+15.7%** |
| Town 1× (A, B) | 10.99 / 11.03 | 10.40 / 10.50 | −0.56 ms (−5.1%) | 90.5 / 90.4 | 95.8 / 94.9 | +5.4% |
| Bower 2× (A, B) | 7.91 / 8.21 | 7.32 / 7.50 | −0.65 ms (−8.1%) | 124.9 / 120.6 | 131.0 / 129.6 | +6.2% |

Process CPU per frame drops with the thread: town 2× 22.2 → 19.8–20.1 ms.

- **Town 2× is CPU-bound here, so FPS rises more than the thread time falls** (+15.7% FPS for −13.4% thread). It is still CPU-bound after the change (thread at 99.7% of a core). Town 1× and Bower 2× gain less: the host-key memo only matters at scale > 1, and the GPU co-limits Bower.
- **Profile after** (`q-t2`, town 2×):
  - `GetHostTextureKey` went from 9.4% to 1.6%, with 0.6% uncached;
  - `BindingInfoFromFetchConstant` from 2.3% to 0.9% (memo);
  - mutex wait from 1.5% to 0.7%;
  - `CanQueuePreparedDraw` from 1.0% to 0.2%.
- **The largest remaining items are structural or inherent:**
  - encoder and draw calls in the driver (~10%);
  - uploads and `mprotect` (~6%);
  - `PrepareDrawConstants` self (~4.4%);
  - `memmove` (5.4%: upload copies and prepared-draw building);
  - `MayRequestTexturesLoadData`, still called twice per draw (4.3% at 2×).
- **First launch after each rebuild was kept** (the verify spins): ready in 4.9 s and 5.1 s, 59.6–59.9 FPS at the 60 cap, 1% low 48.7.

## 4. Base M1 and M3 Pro (estimate, not measured)

**Assumption:** this thread is mostly single-thread integer work, pointer chasing and driver calls, so it scales with P-core single-thread speed. Ratios from Geekbench 6 single-core figures as recalled (±15%):
- base M1 (Firestorm, 3.2 GHz) is about **1.6–1.9×** slower than an M5 Max P-core;
- M3 Pro is about 1.35–1.45× slower.

Memory latency and the Metal driver may scale differently.

| | M5 Max off → on | Base M1 (×1.6–1.9) | M3 Pro (×1.35–1.45) |
|---|---|---|---|
| Town 2× | 12.56 → 10.88 ms | 20.1–23.9 → **17.4–20.7 ms** (−2.7 to −3.2 ms) | 17.0–18.2 → **14.7–15.8 ms**: under the 16.7 ms budget |
| Town 1× | 11.01 → 10.45 ms | 17.6–20.9 → 16.7–19.9 ms | 14.9–16.0 → 14.1–15.2 ms |
| Bower 2× | 8.06 → 7.41 ms | 12.9–15.3 → 11.9–14.1 ms | 10.9–11.7 → 10.0–10.7 ms |

- **M3 Pro 2×/60 target:** the town's GPU Commands thread moves from at or above the 60 FPS budget to ~1–2 ms under it. On that Mac the GPU (≈27 ms at 2× in Bower) still limits the frame, so the CPU stops being a second limit.
- **Base M1:** at 2× the GPU dominates (8-core GPU). At 1× in town the thread drops ~0.9–1.1 ms, to about the 60 FPS budget. Busy crowds and snow are still likely to dip below 60 on the CPU side.
- **Lock contention** (`__psynch_mutexwait`) costs more on Macs with fewer, slower cores. The memo removes most of the per-draw global-lock acquisitions, which helps beyond the ratio.

## 5. The tex-rename town cost

tex-rename's own code is 0.2–0.3% of the thread in every scene. Its measured +1% in town 2× (12.07 → 12.21 ms) comes from about 4 more pre-commit flushes per frame and the descriptor re-uploads after a rename. Each one triggers the normal texture-request path, which used to hit the uncached `GetHostTextureKey`. With the memo that path is cheap, so the rename's indirect cost should shrink with it. This was not re-measured with renaming off; no rename-specific change was made.

## 6. Correctness: frame-matched parity (`f2diag parity`, 2 `still` runs per arm, `--frames 1` at 2×)

- **Town 2×:** cross median 0.53, max 1.50; floor median 0.71, max 1.14; noise limit 1.95.
  - The tool says "beyond-noise" because of hot blocks on walking NPCs, with cross values 1–2× the floor.
  - A region check shows no on/off split: on1 vs off2 is 0.64, below the off–off floor of 1.50 (`burst.py`).
- **Town 1×:** cross median 0.52, max 1.27; floor median 0.67, max 1.14; noise limit 1.62. The same NPC hot blocks.
- **Bower 2×: a pre-existing bimodal state, not this change.**
  - The first two "on" runs both showed the pale tree root to the right of the camp fire lit differently: mean abs 5.7 in that region against a 0.06 floor.
  - The state lasts the whole run (a 3-burst run stayed in it to frame 942).
  - Bisect with 8 runs per arm:

    | Arm | Runs in the pale state |
    |---|---|
    | Off | 2/13 |
    | On | 4/11 |
    | Memo only | 2/8 |
    | Lite only | 2/8 |

  - It is one state chosen per boot at roughly 25%, independent of the switches. Crops: `crops/root-labelled.png` and `crops/root-labelled-4.png` (OFF | ON matches | ON differs | OFF differs, guest frame 346).
  - **State-matched parity is within noise:**
    - pale vs pale (`BB-parity.txt`): cross median 0.19, max 0.36; floor median 0.19, max 0.25;
    - normal vs normal (`BA-parity.txt`): cross median 0.40, max 0.53; floor median 0.40, max 0.55. Its only hot blocks are the fire flicker and the dog's breadcrumb sparkles, which differ between arms of either kind.
- Full set: area at 2× with verify (above), spin5 town 2× and Bower 3× with verify, all `ok` with 0 AV. Boot, F5 and bootloop were not run: there is no startup or save-path change.

## Switches

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_TEX_KEY_MEMO=0` | **on** | Host texture key memo + fetch-decode memo off. Dev Launcher → Performance → "Remember texture lookups" |
| `FABLE2_TEX_KEY_MEMO_VERIFY=1` | off | Uncached reference on every memo hit; mismatches logged and counted |
| `FABLE2_DRAW_PREP_LITE=0` | **on** | The six per-draw trims of §2B off (`std::unordered_set` sets, vertex binding copy, etc.). Dev Launcher → "Skip unchanged per-draw work" |

Files:
- `backends/metal/sdk-metal-gpucmd.patch` (new; SDK `pipeline/texture/cache.h/.cpp`);
- `tools/build_macos_metal.py`, `tools/fable2_agent_cache.py` (comment only);
- `metal_command_processor.cc/.h`, `metal_texture_cache.cc/.h`, `metal_ptr_set.h` (new);
- `tools/macos/Shared/LaunchSettings.swift`;
- `docs/bug-patterns.md` (row 85 + a lesson), `docs/reference.md`, `docs/README.md`.

## Next

- `MayRequestTexturesLoadData` runs twice per draw (from `IssueDrawImpl` and again inside `MetalTextureCache::RequestTextures`). The first result could be passed down: about 2% at 2×.
- `RetireFenceWaitedSharedMemoryWrites` still copies the pending list on most calls. An in-place compaction would remove the 1.5 KB stack array init.
- `PrepareDrawConstants` self time (4.4%): system-constant and descriptor packing per draw.
- Measure on a real base M1 (benchmark kit) to replace the ratio estimate.

Round 3 took the first three items: [gpucmd-cpu3-20261014.md](gpucmd-cpu3-20261014.md).
