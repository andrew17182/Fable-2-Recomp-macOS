# Frame pacing and hitches (2026-10-12)

Branch `hitch-fix` (agent slot, from `perf-staging` = `macos` 74b94ff). M5 Max, Bower Lake save. Evidence, scripts and
logs: `out/validation/hitch-20261012/` (`summ.sh` per-run summary, `wbchain.py` cache write-back chain).

## 1. Cache write-back blocked by the learn file (fixed first)

Found while classifying first-launch hitches: from the second normal session on, the launcher published nothing from the
session cache, so every launch started from the same cold cache. Cause and fix: `docs/native-shaders.md` (auto-learn,
"Append-only learn file"), `docs/bug-patterns.md` row 67.

- Check (`wbchain.py`): a copy of the live cache (read only) gets the caches of 8 chained test sessions published one
  over the other with `publish_cache`. Sessions `learn-c1` 1× cold, `c2` 2× and `c3` 3× from the previous cache, `c4`
  1×, `c5` 1× and `c6` 2×, `c7` 2× with a forced compaction (`FABLE2_NS_LEARN_COMPACT_KB=50`, writes
  `4D5307F1.ns2-learn.1.txt`), `c8` 2× loading generation 1 and appending to it. All 8 publishes succeed
  (`wbchain-learn.txt`). Before the fix, publishing a warm session over a cold one failed with "not an extension of the
  live file".
- Only appended files (`.metal.artifacts`, the learn file) and new files (archive segments, new learn generations)
  changed. Nothing else in the cache is rewritten in place.
- The live cache keeps a superseded generation file after a compaction (publish never deletes non-segment files); the
  game reads the highest generation and deletes older ones in its own session copy.

## 2. Long-frame attribution (`[hitch]`, always on)

`metal_hitch.h`: each command-processor swap interval that is a hitch logs one line that splits it over the places
that can block or cost the GPU Commands thread (exclusive time, nested scopes pause their parent). Hitch rule
(relative, since 2026-10-13): interval ≥ clamp(`FABLE2_HITCH_RATIO` (1.5) × median of the previous 32 intervals,
`FABLE2_HITCH_MS` (40), `FABLE2_HITCH_CEIL_MS` (100)); with fewer than 8 previous intervals the floor alone. Steady
GPU-bound frames are not hitches (an M3 Pro at 3× runs ~47 ms every frame, all `slot`/`idle`: the absolute rule
logged 280–330 lines per benchmark sweep and hit the 400-line cap); a spike over the recent baseline is, and any frame
≥ 100 ms always is. The line then carries the baseline (`base`). `FABLE2_HITCH_RELATIVE=0` restores the absolute
rule (every interval ≥ `FABLE2_HITCH_MS`, line without `base`):

```
[hitch] f604 125.9 ms (cpu 7.1, base 16.7): pso_xfer 120.8(7) other 4.6
```

| Category | Where |
|---|---|
| `idle` | command processor waiting for guest commands (game, loading, streaming) |
| `slot` / `sync` | waiting for a frame slot (GPU behind) / synchronous command-buffer waits |
| `pso_wait` | waiting for a background pipeline (startup warm-up, morph draws) |
| `pso_emu` | geometry/tessellation emulation pipelines built on the draw path |
| `pso_xfer` / `pso_xclr` / `pso_rt` | EDRAM transfer, transfer-clear, other render-target/resolve pipelines built on the draw path |
| `xlate` | shader translation on the draw path |
| `rt_grow` | render-target shrink grows (§21 of `resolution-scale.md`) |
| `tex` / `present` | texture requests and loads / guest-output refresh |
| `other` | the rest (draw processing and encoding) |

`cpu` is the GPU Commands thread's CPU time in the interval. Cost: two `mach_absolute_time` reads per scope; the
lines appear only on long frames (at most 400 per session). `FABLE2_HITCH_TRACE=0` turns it off. `FABLE2_MSL_SALT=<x>`
(diagnostic) prepends a comment to the MSL compiled at run time, so Metal's system shader cache misses as on a first
launch after a transfer-shader change.

## 3. Classification (before the fixes)

Runs: spin5 cold (cache copied from the live cache) and warm (`--cache-from` the cold run) at 1×/2×/3×, headings8 at
60 FPS at 1×/2×/3×, and cold runs with a fresh `FABLE2_MSL_SALT`. All verdict ok, no contention.

| Cause | Seen | Size |
|---|---|---|
| **EDRAM transfer pipelines compiled on the draw path** (`pso_xfer`) | Only when Metal's system shader cache misses: first launch after a change to the transfer MSL (the merged build 74b94ff: merge-smoke 118 ms at 1×, 134 ms at 3×, right after the first use of a new 4×MSAA effect target), a new Mac or an OS update | ~15 ms per key, ~100 keys: load frames of 577 / 214 / 224 ms and one 100–130 ms gameplay hitch per spin5 window (1% low 22–23). Warm: 14 keys in 1.6 ms |
| Level load / boot frames (`idle`) | Every launch, frames 0–120 (logo, level load) | 40–104 ms, guest-side (loading); unchanged, outside gameplay |
| Shader specialization, auto-learn, NS `-O2` swaps | Background threads; draws use the normal pipeline until the variant exists | nothing on the draw path (no `pso_wait`/`xlate` in any hitch) |
| RT shrink grows | ~60 per session, mostly in the first second; also new effect targets during play | under 1 ms per hitch frame (never above the 0.5 ms print threshold) |
| Texture uploads, GPU waits, present | Every run | under 3.5 ms per hitch frame |

Warm steady play has no long frames: headings8 at 60 FPS, 1×/2×/3×, 0 frames over 33 ms (max 18.8 / 19.2 /
20.4 ms). The 60-FPS 1% low (50–54) is the ordinary 16.7 ± 2 ms swap jitter, not hitches. Cold-only, but it hits
every user after every transfer-shader change.

## 4. Fix: transfer pipelines prebuilt at startup (`FABLE2_XFER_PREWARM`, default on)

`metal_render_target_cache.cc` (`[xfer-prewarm]`):

- The pipeline build is split out of `GetOrCreateTransferPipelines` into `BuildTransferPipeline` (key → MSL defines →
  library → pipeline). It reads only the key and settings fixed at initialization, so it runs on any thread.
- Keys built on the draw path are appended to `<shader storage>/<title>.xfer-keys.txt` (append-only, so the launcher's
  cache write-back accepts it; keys built before storage init are written when it starts).
- Right after shader storage init, two utility-QoS threads build the keys of that file, then the built-in default
  list (`metal_xfer_keys_default.h`, 101 keys recorded at 1×/2×/3×; transfer modes, MSAA counts, Xenos render-target
  format enums and Metal pixel formats only, no game data), so a first session prewarms as well.
- A draw that needs a key takes the prebuilt pipeline, waits for one being built, or builds it itself when no thread
  has started it. A pipeline is only taken for the exact key and output type.
- Write-back: 4 chained sessions (`xfer-1x-rec` creating the key file, `xfer-1x-pre`, `seed-2x` and `seed-3x` appending)
  published one over the other with `publish_cache`: all succeed (`wbchain-xfer.txt`).
- Log: `[xfer-prewarm] building N transfer pipelines (M from the built-in list)` and, when done, the count, summed
  build time and time after storage init.

Switches: `FABLE2_XFER_PREWARM=0` (off: draw-path builds, nothing recorded; Dev Launcher toggle "Prebuild
render-target transfer pipelines at startup"), `FABLE2_XFER_PREWARM_DEFAULTS=0` (cache keys only).

First results (1× spin5, cold system cache via a fresh salt):

| Run | max ms | 1% low | `pso_xfer` |
|---|---|---|---|
| no key file, prewarm without defaults (first build of the list) | 116.4 | 23.2 | 1.4 s in 9 frames |
| key file from that run | 19.3 | 52.6 | none; 100 built in the background, done 0.78 s after storage init |
| no key file, built-in defaults (1×) | 19.0 | 53.1 | none; 101 built, done 0.78 s after storage init |
| no key file, built-in defaults (3×) | 18.7 | 53.6 | none; done 0.90 s after storage init |

## 5. Before / after (same build, `FABLE2_XFER_PREWARM=0` vs default)

`out/validation/hitch-20261012/matrix.sh` → `matrix.jsonl`: 60 FPS, Bower Lake, 2 runs per arm (run 1 / run 2), arms
interleaved. "Cold" = cache copied from the live cache plus a fresh `FABLE2_MSL_SALT` per run (Metal's system shader
cache misses, as on a first launch after a transfer-shader change, a new Mac or an OS update). "Warm" = `--cache-from`
a seed cache with the key file (`seed/`). 36 runs: verdict ok, 0 contention.

| Run | off: 1% low | p99 ms | max ms | > 33 ms | on: 1% low | p99 ms | max ms | > 33 ms |
|---|---|---|---|---|---|---|---|---|
| spin5 cold (fresh salt) 1× | 23.0 / 23.1 | 18.31 / 18.38 | 118.6 / 117.2 | 1 / 1 | 54.3 / 53.7 | 18.29 / 18.46 | 18.6 / 18.9 | 0 / 0 |
| spin5 cold (fresh salt) 2× | 22.7 / 23.2 | 18.47 / 18.42 | 120.5 / 116.8 | 1 / 1 | 53.6 / 53.7 | 18.52 / 18.46 | 18.8 / 19.0 | 0 / 0 |
| spin5 cold (fresh salt) 3× | 23.0 / 23.0 | 18.38 / 18.55 | 118.4 / 118.2 | 1 / 1 | 54.1 / 53.2 | 18.45 / 18.53 | 18.6 / 19.0 | 0 / 0 |
| spin5 warm 1× | 53.6 / 54.4 | 18.39 / 18.27 | 19.3 / 18.5 | 0 / 0 | 54.0 / 53.9 | 18.37 / 18.31 | 18.7 / 18.7 | 0 / 0 |
| spin5 warm 2× | 54.1 / 53.9 | 18.36 / 18.34 | 18.5 / 18.7 | 0 / 0 | 53.5 / 53.6 | 18.43 / 18.48 | 18.9 / 18.8 | 0 / 0 |
| spin5 warm 3× | 54.3 / 54.2 | 18.34 / 18.29 | 18.5 / 18.7 | 0 / 0 | 54.0 / 53.4 | 18.42 / 18.42 | 18.6 / 19.2 | 0 / 0 |
| headings8 warm 1× | 54.1 / 53.7 | 18.28 / 18.39 | 18.9 / 18.8 | 0 / 0 | 53.5 / 54.3 | 18.39 / 18.3 | 19.0 / 18.7 | 0 / 0 |
| headings8 warm 2× | 53.7 / 53.7 | 18.47 / 18.5 | 18.9 / 19.0 | 0 / 0 | 53.8 / 54.0 | 18.4 / 18.42 | 19.0 / 18.8 | 0 / 0 |
| headings8 warm 3× | 53.5 / 53.8 | 18.47 / 18.37 | 19.0 / 19.0 | 0 / 0 | 54.2 / 53.8 | 18.32 / 18.41 | 18.6 / 19.1 | 0 / 0 |

- Cold off: 1.33–1.36 s of `pso_xfer` per session (all in `[hitch]` lines), about 1.2 s of it in the load frames
  and one 117–121 ms gameplay hitch per spin5 window. Cold on: no `pso_xfer` at all.
- Warm: equal within noise (the draw-path builds then cost ~1.5 ms per session). Steady 60-FPS pacing (headings8) is
  equal; the remaining 1% low (~54) is the normal swap jitter of a 60-FPS cap, not hitches (max 19 ms).
- Uncapped 2× with the RT shrink (rt-shrink agent's observation, `resolution-scale.md` §21): headings8, 2 pairs,
  warm seed cache, `FABLE2_HITCH_MS=12`. Shrink on 1% low 84.2 / 83.4 FPS, p99 11.6 / 11.7 ms; `FABLE2_RT_SHRINK=0`
  81.2 / 82.1, 12.1 / 12.0 ms; max 12.7–13.4 ms in all four. The lower 1% low with the shrink no longer shows on this
  build; no grows or hitch lines in the windows.

Validation (final build, defaults): spin5 1×/2×/3× cold and warm (matrix), area 2×, boot, F5 (saved), bootloop 3/3:
verdict ok, 0 AV, `skip_intro_missing` 0, `pgrep -x fable_2` = 1 after every run. Image parity: the prewarmed
pipeline is built by the same function from the same key as the draw-path one; dark-dip / blue detectors on the 3×
spin5 captures flag the same counts on and off (warm 21/21 dark, 20/24 blue; cold 19/23, 21/24).

## 6. Not done / open

- ~~Specialized (NS2) pipelines are not in a binary archive yet~~: done, `FABLE2_NS2_ARCHIVE` (`docs/native-shaders.md`,
  "Specialized pipeline archive"). The `.ns2` set existed, but short sessions ended before its periodic save.
- After a learn-file compaction the live cache keeps the superseded generation (publish never deletes non-segment
  files). Harmless, and left as is, because removing it needs a `publish_cache` change, which waits for the maintainer's approval.
- The load-screen long frames (40–104 ms, `idle`) are guest-side loading and were left alone.

## Switches

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_XFER_PREWARM` | on | Build recorded + built-in transfer pipelines in the background after storage init; `=0` draw-path builds, nothing recorded. Dev Launcher toggle |
| `FABLE2_XFER_PREWARM_DEFAULTS=0` | off | Only the cache's own key file, not the built-in list |
| `FABLE2_NS_LEARN_APPEND` | on | Append-only learn file (§1). Dev Launcher toggle |
| `FABLE2_HITCH_TRACE` / `FABLE2_HITCH_MS` | on / 40 | `[hitch]` lines (§2) |
| `FABLE2_HITCH_RELATIVE` / `FABLE2_HITCH_RATIO` / `FABLE2_HITCH_CEIL_MS` | on / 1.5 / 100 | Relative hitch rule (§2); `=0` absolute |
| `FABLE2_MSL_SALT=<x>` | off | Diagnostic: miss Metal's system shader cache for run-time MSL (since 2026-10-14 also the native translator's MSL) |

Files: `backends/metal/port/rex/graphics/metal/metal_hitch.h`, `metal_xfer_keys_default.h` (new),
`metal_render_target_cache.cc/.h`, `metal_command_processor.cc/.h`, `metal_pipeline_cache.cc/.h`,
`metal_texture_cache.cc`, `metal_direct_host_resolve.cc`, `metal_resolve_to_texture.cc`, `metal_ns_specialize.cc`,
`tools/macos/DevLauncher/main.swift`, `docs/native-shaders.md`, `docs/bug-patterns.md`. Restore points:
`../restore-points/2026-10-12-hitch-{instrument,xfer-prewarm}.patch`.
