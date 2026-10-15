# GPU Commands thread CPU (2026-10-12)

Branch `gpucmd-cpu` (agent slot, from `perf-staging` a681951). M5 Max. Saves `hero1-dog-live_2026-10-11_11-20-20`
(Bower Lake) and `hero1-dog-live_2026-10-07_14-45-08` (Old Tomb). Harness, scripts and copied results:
`out/validation/gpucmd-20261012/`:
- run scripts: `arm.sh`, `clean.sh`, `samp.sh` (a run plus a `sample` of our own pid), `ab1.sh`, `ab2.sh`, `final.sh`;
- `cpf.py` (GPU Commands and process CPU per frame over the headings8 window);
- `thread.py` (one thread's call tree from a `sample` file), `incl.py`, `sub.py`, `callers.py <thread.txt> FUNC [levels]`;
- `parity.py`.

Every run: verdict `ok`, 0 AV, and no contention on the perf runs.

At 1× uncapped, the GPU Commands thread (PM4 → Metal translation and encoding) is the frame limit on this Mac.
It is probably the limit on base M1–M3 Macs at any scale.

## Profile (`sample`, GPU Commands thread, 1× uncapped, headings8)

Two Bower Lake runs and one Old Tomb run; share of the thread:

| Category | Share | Notes |
|---|---|---|
| Draw path (`IssueDrawImpl`) | 79–82% | Everything below except the PM4 rows |
| `SubmitPreparedDraw` / `EncodePreparedDraw` | 26–34% | Encoding, queue flushes, materialization |
| `PrepareDrawConstants` | 13–15% | Bindless texture/sampler lookups ~5%, system constants ~1.3% |
| `RenderTargetCache::Update` | 9–13% | Included 4–5% of `getenv` (next row) |
| **`getenv` (`__findenv_locked`)** | **4.9–6.2% Bower Lake, 8.0% Old Tomb** | `FABLE2_RT_SHRINK_TRACE` read on every draw (`RtShrinkNoteDraw`) and every ownership claim (`OnOwnershipClaimed`) |
| `memmove` / `memset` | 9–11% | A third is shared-memory upload copies; most of the rest is `PreparedDraw` copies |
| `PreparedDraw` churn | ~4–5% | Record reset, scratch acquire, queue push, recycle, vertex-binding assign |
| Encoder creation (driver) | ~10% | Render ~6%, compute ~2.3%, blit ~1.5% (structural, §19 of `resolution-scale.md`) |
| Shared-memory uploads | 6–7% | `memcpy` plus `mprotect` for write-watch (`EnableAccessCallbacks` ~3%) |
| PM4 / register writes | 7–8% | `ExecutePacketType0`, `WriteFastRegisterRangeFromRing` (already range-fast) |
| Resolves / texture materialization | 3–5% / ~5% | |
| `PopulateBindlessTables` | 4–6% | `useResources` batches, sampled-texture set |
| Locks | not visible | |

A whole-process `sample` found one other runtime getenv: `FABLE2_DEADBEEF` per UI text item on a guest thread.

## Changes

### A. Env switches read once on the draw path (no switch: same behaviour)

- `MetalRenderTargetCache::RtShrinkTrace()` reads `FABLE2_RT_SHRINK_TRACE` once. It replaces two per-call
  `std::getenv`. A getenv walks the whole environment, so the cost grows with its size.
- `FABLE2_GETENV_PER_CALL=1` is a diagnostic that restores the per-call read, for A/B on any Mac.
- The same pattern was fixed elsewhere:
  - `fable2::deadbeef::enabled()`: per UI text item. It now follows `fable2_text_probe.h`;
    `FABLE2_PROBE_GETENV_CACHE=0` reads it every call.
  - `fps_meter` `enabled()`: every main-loop iteration.
  - `FABLE2_TEX_SCL_LOG` and `FABLE2_RTT_TRACE_BASE`: on the scaled texture-load and scaled-grow paths.
- **`tools/getenv_audit.py`** (Python 3.9) lists every getenv that is not inside a static initializer. Helpers called
  only from static initializers count as static.
  - `--strict` exits 1 on unmarked sites.
  - A deliberate one-time site gets a `getenv-ok: <reason>` comment.
  - `backends/` is at 0 unmarked.
  - `src/` lists ~90 diagnostic and startup sites for review; the runtime `sample` shows none of them on a hot path
    after this change.
- Bug-patterns row 71.

### B. Slim prepared-draw records (`FABLE2_PREPARED_DRAW_SLIM`, default on)

`PreparedDraw` is ~2.6 KB: 96 hazard ranges, 32 vertex ranges, uniforms for 2 stages × 5 CBVs, the dynamic state and
vectors. It was zeroed or copied about six times per draw:
- the local hazard array zeroed, then copied in;
- uniforms, dynamic state and vertex ranges built in locals, then copied in;
- `draw = PreparedDraw{}` on reuse (construct a temporary, then move-assign it);
- the move into the queue vector;
- the move into the spare list;
- the move back into the scratch record.

Now:
- **The queue holds the records by pointer** (`std::vector<std::unique_ptr<PreparedDraw>>`), with a free list (queue
  budget + 8). Queueing, flushing and recycling move a pointer. A draw that is not queued, or an early return, gives
  its record back through a lease.
- **Field-by-field reset** (`ResetPreparedDrawSlim`):
  - The count-bounded arrays (`shared_memory_hazard_ranges`, `vertex_ranges`) are not cleared. Every reader stops at
    the count.
  - The uniforms are not cleared either. `PrepareDrawConstants` assigns them whole (`uniforms_out = {}` first), and
    `IssueDrawImpl` clears them when it does not prepare uniforms.
  - A new `PreparedDraw` field must be added to this reset.
- **Built in place**: the hazard and vertex ranges, uniforms and dynamic state are written into the record.
- `FABLE2_PREPARED_DRAW_SLIM=0` restores the whole-record reset and the build-in-locals-then-copy. The pointer queue
  stays.
- `FABLE2_PREPARED_DRAW_POOL=0` still means a fresh allocation per draw.
- Dev Launcher toggle: "Lighter per-draw bookkeeping (less GPU-thread CPU)".

Exact: the same draws, state and order are encoded. Only host-side bookkeeping changed.

## Results (headings8, `--no-capture --cpu`, same build; "off" = `FABLE2_PREPARED_DRAW_SLIM=0 FABLE2_GETENV_PER_CALL=1`)

| Bower Lake, 1× uncapped | off | A only (`SLIM=0`) | A + B (default) |
|---|---|---|---|
| GPU Commands ms/frame | 8.34 / 8.46 | 7.77 / 7.74 | 7.56 / 7.41 |
| GPU Commands cores | 0.97 / 0.97 | 0.92 / 0.91 | 0.89 / 0.89 |
| Process ms/frame | 17.46 / 17.63 | 17.00 / 16.91 | 16.74 / 16.52 |
| FPS (window) | 116.4 / 114.9 | 118.9 / 117.6 | 118.0 / 120.3 |

- GPU Commands: −0.91 ms/frame (−10.8%). A is −0.65 ms, B another −0.27 ms. The FPS gain is smaller (+3%) because
  the GPU (~8 ms/frame here) now co-limits at 1× in Bower Lake.
- First launch after the rebuild (default build): 7.52 ms/frame, 117.5 FPS, 1% low 49. No extra first-launch cost.

| Old Tomb, 1× uncapped | off | A + B |
|---|---|---|
| GPU Commands ms/frame | 5.15 / 5.03 | 4.55 / 4.24 |
| FPS | 192.8 / 196.6 | 212.0 / 224.5 |

Old Tomb: −0.70 ms/frame (−13.8%), FPS +12%. The GPU is not the limit there, so the CPU saving shows as FPS.

| Bower Lake, 60 FPS cap | off | A + B |
|---|---|---|
| GPU Commands ms/frame | 8.66 / 8.81 | 7.80 / 7.71 |
| GPU Commands cores | 0.519 / 0.528 | 0.467 / 0.462 |
| Process cores | 1.23 / 1.26 | 1.20 / 1.19 |

| Bower Lake, uncapped, 1 run each | 2× off → on | 3× off → on |
|---|---|---|
| GPU Commands ms/frame | 9.26 → 8.06 | 9.21 → 8.24 |
| FPS (GPU-bound) | 93.2 → 93.1 | 64.1 → 63.8 |

2× and 3× do not regress. FPS there is GPU-bound and unchanged; GPU Commands CPU drops by ~1 ms/frame.

Profile after the change (`post/`): `getenv` is gone from every thread. `memmove` is 5.2%, mostly upload copies.
No remaining exact, contained item of our own is above ~3% of the thread. The largest is `GetSamplerParameters` at
2.8% (a per-draw change check). The rest is driver work (encoder creation ~10%, draw calls, AGX state) and inherent
copies.

## Base M1-class Macs (estimate, not measured)

- Fewer and slower P-cores: the GPU Commands thread costs roughly 1.4–1.8× more per frame than here (~0.75–0.95 core
  at 60 FPS in Bower Lake). Wherever it is the limit, which is 1× on any of those Macs and higher scales where the
  GPU keeps up, the −11 to −14% of the thread is about +10–14% FPS uncapped (as Old Tomb here).
- At 60 FPS it is about −0.08 to −0.1 core and lower p99 frame times, because the thread keeps more headroom under
  its 16.7 ms budget.
- getenv's cost scales with the environment size and the per-call string compares. The launcher and harness add
  many `FABLE2_*` variables, so A is at least as large there.

## Validation

- Parity, spin5 1×, on vs off, run in parallel: idle median 0.06% / max 0.75%. idle2 (animated) median 6.1%: the
  spin left the two runs at slightly different headings (crops in `crops/`). By eye, lighting, shadows, fire, the
  breadcrumb trail and the HUD match.
- Full set, default build (`final.sh`): every run ended `ok` with 0 AV.

  | Run | Result |
  |---|---|
  | spin5 1× / 2× / 3× with `FABLE2_RTT_VERIFY=1` | 152,320 / 150,272 / 95,744 kept/fused textures checked, 0 mismatched |
  | area with RTT verify and `FABLE2_NS_SPECIALIZE_VERIFY=1` | 620,544 RTT checks, 0 mismatched; spec guard 253,952 checks, 253,716 verified hits, 0 misses, 0 misbinds |
  | boot | ok |
  | F5 | saved |
  | bootloop | 3/3 pass, 0 hangs |

  - The skip-intro guard is clean: no `playing original` in any log.
  - Correctness runs went two at a time, so they show contention. That only affects perf numbers.

## Switches

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_PREPARED_DRAW_SLIM=0` | on | Whole-record reset and locals-then-copy, as before. Dev Launcher toggle |
| `FABLE2_GETENV_PER_CALL=1` | off | A/B diagnostic: `FABLE2_RT_SHRINK_TRACE` read on every call again |

Files: `metal_command_processor.cc/.h`, `metal_render_target_cache.cc/.h`, `metal_texture_cache.cc`; `getenv-ok`
marks in `dxbc_to_dxil_converter.cc`, `metal_air_optimizer.cc`, `metal_pipeline_cache.cc`, `metal_presenter.mm`,
`metal_test_control.mm`; `src/diagnostics/fable2_deadbeef.h`, `fable2_text_probe.h`, `fps_meter.h`;
`tools/getenv_audit.py`, `.gitignore`, `tools/macos/DevLauncher/main.swift`; `docs/bug-patterns.md`.
Restore points: `../restore-points/2026-10-12-gpucmd-{getenv,slim}.patch`.
