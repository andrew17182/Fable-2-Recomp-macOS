# Area-transition load profile, re-run — 2026-10-15

> **Follow-up 2026-10-16** (`docs/load-fixes-20261016.md`):
> - Fix A is done: native save-compressor search, save 592 → 427 ms, byte-identical.
> - C is traced. The render-object holds are a fixed 15-frame fade, and the constant is at **0x820994C0** (= 1.0), not 0x82099440. The loading-frames hold is a readiness wait, so proposal E is dropped.
> - D is an opt-in setting (`FABLE2_LOAD_FADE_FRAMES`, part of the launcher's "Skip cosmetic waits", `FABLE2_FAST_PACING`): about 0.9 s per area change.

Question: after the 2026-10-08 fixes (`docs/archive/area-load-profile-20261008.md`), where do the remaining seconds of the
Thag's Cave Cullis Gate → Bower Lake transition go, and what could still be cut? **Measurement only; nothing in the game
changed.** The only code changes are harness and reader updates (listed at the end).

## Setup

- Same repro and save as 10-08: library entry `hero1-dog-live_2026-10-07_18-49-47`, hold A through the Cullis Gate.
  The game autosaves ("Saving content…"), shows the "Loading Bower Lake" screen, then the letterboxed beetle-fight arrival.
- Current `macos` build (e4210c3), built fresh in an agent slot. M5 Max, 64 GB. No other game during any run
  (`perf_comparable: true`, exclusive perf lock).
- **Launcher defaults of a new install** (`LaunchSettings.gameEnvironment`, controls excluded): frame rate Auto, resolution
  Auto, every default-on switch. On this Mac Auto picked **2× at 60 FPS** (`[res-auto] ... -> 2x`, `[fps-auto] -> 60 FPS`).
  10-08 ran at 1× 60 FPS. The item list is `out/validation/area-load-20261015/env-defaults.txt`.
- Harness: `f2diag run area` (the successor of the 10-08 `run.py`): press A once the game reports ready, 0.25 s raw captures,
  per-thread CPU every 0.25 s (`--cpu --cpu-period 0.25`, new). Wrapper: `out/validation/area-load-20261015/go.sh <label>`.
  The 10-08 readers `logt.py`, `frames.py` and `phasecpu.py` were ported to the f2diag folder layout in the same folder.

| Run | Session | Mode |
|---|---|---|
| u1 | `20261015-092340-bdc58a23` | untraced; **first launch after the build** (kept, see "First launch") |
| u2 | `20261015-092514-7f293bda` | untraced |
| t1 | `20261015-092636-34de42aa` | `FABLE2_LOADING_TRACE=1 FABLE2_GPU_TIME_TRACE=1` |
| s1 | `20261015-093419-284f1781` | `FABLE2_LOADING_TRACE=1` plus `/usr/bin/sample` for 8 s from the A press. **Timing distorted**; call stacks only |

All four closed normally (verdict ok, 0 AVs, `pgrep -x fable_2` returned 1 after each). Logs, frame CSVs, the trace folders
and the f2diag results are kept in `out/validation/area-load-20261015/logs-<run>/` and `f2diag-al15-<run>/`.

## Timeline against the 10-08 "after" column

Seconds from the session origin. Captures are 0.25 s apart, so screen times are ±0.25 s; log times are exact.
t0 = save start (`[save-notice] ... elapsed N ms` minus N: the swaps drop and the screen goes black at the same moment).

| Phase | 10-08 after (g1 / v1) | u1 (first launch) | u2 | t1 (traced) |
|---|---|---|---|---|
| A press (game ready) | ≈10.2 | 11.48 | 5.67 | 5.63 |
| **t0** | 11.55 / 11.6 | 13.01 | 7.09 | 7.16 |
| Save (mount → `XamContentClose`) | 0.73 s | 0.60 s | 0.61 s | 0.59 s |
| "Saving content" on screen | ≈0.9–1.0 s | 13.28 → 14.06 (≈1.0 s) | 7.47 → 7.99 (≈0.8 s) | 7.43 → 8.20 (≈1.0 s) |
| Level-load call `sub_825F3050` | (1.17 s before the fixes) | 14.42–15.02 (0.61 s) | 8.42–9.08 (0.66 s) | 8.48–9.14 (0.67 s) |
| Loading screen visible | 13.4 → 16.0 / 13.24 → 16.0 (≈2.6 s) | 14.57 → 16.90 (≈2.5 s) | 8.50 → 11.07 (≈2.6 s) | 8.72 → 11.03 (≈2.6 s) |
| First new-area frame (HUD up, fade-in starting) | ≈16.4 / ≈16.4 | 17.41 | 11.32 | 11.55 |
| **t0 → first new-area frame** | **≈4.9 s** | **4.40 s** | **4.23 s** | **4.39 s** |

- About **0.5–0.7 s faster than 10-08**, at 2× instead of 1×. The save is 0.13 s shorter and the level-load call is about
  0.6 s instead of ≈0.8 s (inferred: 10-08 gave 1.17 s before the worker-0 drain removed ≈0.39 s; its sessions are pruned).
- Traced and untraced runs agree within the capture step, as on 10-08: tracing does not distort this transition.
- The 5 s streaming stall is gone in all four runs (largest swap gap 265 ms, during the save).

## Where the time goes (t1, t0 → first frame = 4.39 s)

From `f2diag areaload` (new, log-only; output below), the flight-recorder sampler (`f2diag stacks`), `phasecpu.py` and the s1
`sample`. "c/w" = CPU / guest-wait ms in the phase.

```
phase              from      to      ms | Game c/w  3D c/w    VFS c/w   TexS c/w  GPUCmd c  | reads n/MB/ms        |  swaps  gpu ms
save              0.000   0.592     592 |  515/0     319/0       2/931     2/591    81      |    27/  0.1/   0.1 |     13    50.0
render wait 1     0.593   1.099     506 |   28/152   439/0       0/601     1/503    60      |     3/  0.2/   0.0 |     14    42.0
pre-load          1.099   1.312     213 |   43/378    77/0       4/1054   55/160    36      |    31/ 19.8/   1.6 |     12    56.7
level-load        1.312   1.979     667 |  486/171   483/159   138/1277   29/641    80      |   211/ 45.8/   3.6 |     17    48.9
loading frames    1.979   3.666    1687 |  200/1     714/26     86/3005  192/1497  139      |   791/ 36.0/   3.9 |     85   230.4
render wait 2     3.666   4.133     467 |   16/365   376/0       2/1100    1/468    51      |     0/  0.0/   0.0 |     16    33.9
arrival (first swap after render wait 2): 4.134 s after t0; the new area shows about 0.25 s later (fade-in)
```

(Wait columns of neighbouring phases overlap by up to 100 ms because the profiler lines are 100 ms windows.)

### Ranked remaining costs

| # | Cost | ms | Kind | Evidence |
|---|---|---|---|---|
| 1 | **Loading-screen frames** after the level-load call | **1690** | Fixed guest pacing (game logic) | GameThread 200 ms CPU in 1.69 s; the rest is the 30 Hz tick sleep (87% of its samples at `ProcessGameFrame_82276C30`). Same length in every run: 1.69 s (t1), 1.70 s (s1, with `sample` attached and the level load stretched to 7.3 s), 1.63 s in the 2026-10-06 save load. ≈50 game ticks. It includes ≈0.22 s of GameThread CPU in the first ticks (post-load init) |
| 2 | **Save** (GameThread) | **590** | Guest CPU work, ~80% one function | 515 ms GameThread CPU, 0 waits. Sampler: 80% in `sub_82B52630 < sub_82B51900 < sub_82B49CB8`; its body is a byte-compare match loop over a window (LZ-style longest-match search), i.e. **the save is being compressed**. Scales with host CPU speed |
| 3 | **Level-load call** | **670** | Guest CPU work + completion waits | GameThread 486 ms CPU + 171 ms guest waits (17% of its samples poll `ProcessAndQueryWithByteCheck_82C6F848` for read completions; 60 + 34 ms waits at `82A39260`). VFS finaliser 138 ms CPU, guest `ProcessStructFields_82B59D28` (20% of its `sample` time; deserialisation). Worker-0: 17 ops, 21 ms in total (the 10-08 drain holds) |
| 4 | **Render-object wait 1** `8235DB34 <824EC4A4` (after the save) | **506** | Fixed guest pacing (candidate 6) | Exactly 506 ms in t1 and s1 (s1 distorted by `sample`), 496 ms on 10-08. "Saving content" stays on screen, unchanged, for the whole wait |
| 5 | **Render-object wait 2** `8235DB34 <822C20AC <822770B8` (releases the loading screen) | **467** | Fixed guest pacing (candidate 6) | 467 ms in t1 and s1, 456–470 ms on 10-06/10-08. The loading screen stays static for the whole wait, then one black frame |
| 6 | Fade-to-black frames before the level load | ≈210 | Guest pacing | GameThread 43 ms CPU, tick sleep for the rest; worker-0 8 ops ≈10 ms |
| 7 | Black + fade-in until the area shows | ≈250 | Cosmetic guest pacing | Black frame after wait 2, HUD first, scene fades in |
| — | File I/O | ≈9 | I/O | 1,063 reads, ≈102 MB, **≈9 ms** in `NtReadFile` in total (page cache) |
| — | GPU, uploads, pipelines, textures | ≈0 on the critical path | Host (ours) | GPU busy 230 ms in the 1.69 s loading phase (≈14%); GPU Commands CPU 139 ms; 0 pipeline compiles and 0 shader translations during the transition (the 9 specialised pipelines after arrival were archive hits); 0 texture creates; sync waits ≤ 3 ms per 0.25 s |

**By kind** (of 4.39 s): fixed guest pacing ≈2.86 s (#1, #4, #5, #6; 65%), guest CPU work ≈1.26 s (#2, #3; 29%, the only part a
slower Mac stretches), cosmetic fade ≈0.25 s, I/O ≈0.01 s (≈9 ms), host-side GPU/pipeline/upload work ≈0 on the critical path.

**The two render-object waits (candidate 6), read in the code.** `sub_8235DA58` sets a request flag (`+176`) and a float
(`+172`, loaded from `0x82099440`) on the render object, then calls `MutexLock_WaitInfinite` on `+180` (the `8235DB34`
return). During both waits the 3D Engine sits in its command-wait loop `ProcessAndProcessAndProcess1408_822E0508`, yielding in
`YieldAndCheckThreshold` (65% SPIN); that loop compares elapsed time against a threshold derived from the same constant at
`0x82099440`. So the wait is a timed hold run by the 3D Engine, not work: it has the same length with `sample` attached, at
1× and 2×, and across three dates. The value of the constant was not read (inferred ≈0.5 s from the waits).

**What the host threads do** (s1 `sample`, distorted, proportions only): GPU Commands 80% idle (`__psynch_cvwait`);
3D Engine 33% `__psynch_cvwait` + 28% `swtch_pri` (the yield loop) + 6% clock reads; VFS finaliser 75% idle, 20% guest
deserialisation; no Metal compiler, texture or upload frames of note on any thread.

## Proposed fixes, ranked

| # | Proposal | Expected saving | Preservation / pacing | Risk |
|---|---|---|---|---|
| A | **Native longest-match for the save compressor** (`sub_82B52630`): a hand-tuned override, byte-identical output (vectorised compare of the same window), like the existing `src/core/hotfunc/` overrides | ≈0.3–0.4 s per autosave (of 0.52 s CPU); also every F5 and the chapter save | Faithful: same bytes, same order of guest writes. No pacing change | Low–moderate. Validate: saved file byte-identical to the original function on several saves; library entry decodes |
| B | **Level-load CPU**: sampler-guided hotfunc overrides for the level-load call and the VFS finaliser's `ProcessStructFields_82B59D28` | ≈0.1–0.3 s | Faithful | Moderate (needs per-function equivalence checks) |
| C | **Trace-only first: read the 3D Engine hold constant (`0x82099440`) and the end condition of the loading frames** (one-shot log in an override of `sub_8235DA58`; a log of what the GameThread checks on the tick before render wait 2) | none by itself | — | None |
| D | Then, optionally, **shorten the two render-object holds** (an override of `sub_8235DA58`'s float or the 3D Engine threshold) | up to ≈0.9 s | **Pacing change**: the save notice and the loading screen leave sooner. Should be a setting with the original as default (or the maintainer's call, as for the save-notice minimum, which defaults to 500 ms) | Low–moderate once C names the value; must check the fade still runs and nothing waits on the hold |
| E | Then, optionally, **shorten the loading-frames hold** if C shows a fixed tick count or timer (not streaming) | up to ≈1.4 s | **Pacing change**: the arrival scene starts sooner. If the condition turns out to be "streaming complete", do not touch it (pop-in) | Moderate |
| F | Block instead of yield in the 3D Engine's `822E0508` wait loop (like `FABLE2_RING_WAIT`) | 0 s of latency; ≈0.4 CPU-s per 0.5 s hold freed | Faithful | Low; only worth it if an M3 profile shows CPU contention during loads |
| — | File I/O, pipelines, GPU uploads | ≈0 | — | Nothing to do |

A and B are faithful and are the only cuts that keep the original pacing; together about 0.4–0.7 s on the M5, more on a slower
CPU. C–E would cut up to ≈2.3 s but change the game's own loading-screen pacing.

## First launch after the build (kept, not discarded)

u1 was the first launch of the fresh slot build. The transition itself is no slower (4.40 s), but **startup to "ready" took
11.5 s instead of 5.6 s**: the native-first shader install MSL-compiled all 256 native shaders (`[native-first] 256 native
shaders installed ... MSL compile 65430 ms total`, spread over the cores, ≈5 s wall). The next launch compiled nothing (13 ms).
That compile goes through Metal's system shader cache, which was empty for this new app path; whether a rebuild at an
existing path also pays it was not tested here. Worth its own look (startup, not this transition).

## M3 recipe (log-based, no extra tools)

1. Put the save `hero1-dog-live_2026-10-07_18-49-47` (from `out/save-library/`) on the M3 Pro, or use any save that stands
   next to a load zone that autosaves.
2. Launcher → Settings → Diagnostics → **Extra environment**:

   ```
   FABLE2_LOADING_TRACE=1 FABLE2_GPU_TIME_TRACE=1
   ```

   Leave every other setting at its default (Auto resolution and frame rate). Optionally also set the trace preset to
   "Loading"; the Extra environment already covers it.
3. Play the save, hold A through the Cullis Gate, wait until Bower Lake plays (≈10 s), quit. One transition is enough; two
   if a number looks odd. Do not use turbo.
4. Clear the Extra environment field afterwards (the traces add log volume).
5. Read it: `python3 tools/f2diag.py areaload <session folder>` (Dev Launcher sessions: `out/dev-sessions/<id>`), or
   `python3 tools/f2diag.py areaload "$HOME/Library/Application Support/Fable II Recomp/logs"` for the player app.
   Without a checkout, the same numbers are in `fable_2.log`: grep `save-notice`, `level-load sub_825F3050`,
   `8235DB34` and `worker0 op`.

**How to read it.** Compare each row with the t1 table above:

- **save**, **level-load**: guest CPU. Expect these to grow on the M3 (slower cores); their GameThread CPU column says how much.
- **render wait 1/2** (≈506 / 467 ms) and **loading frames** (≈1.69 s): fixed guest pacing. They should match the M5 to
  within a few ms. If they are longer, look at the 3D Engine CPU column (the hold runs on it) and the swaps column (the M3
  dropping below 60 FPS during the loading screen).
- **gpu ms** per phase vs its length: GPU-bound loading would show here (M5: ≈14% in loading frames).
- **reads n/MB/ms**: I/O. ≈100 MB in ≈10 ms here; a large ms value would mean a cold page cache (first run after boot).
- The arrival line plus ≈0.25 s is the first new-area frame; t0 → arrival is the number to compare (M5: 4.13 s arrival,
  4.2–4.4 s first frame).

## Harness and reader changes

- `tools/f2diag/areaload.py` + `f2diag areaload <target>`: the log-only breakdown above (phases from the GameThread's own
  log lines, `[loading-pacing] prof` CPU/wait/reads, frame-CSV swaps and GPU ms). Untraced logs give the save timing only.
  Runs under `/usr/bin/python3` 3.9.
- `f2diag run`: `--cpu-period` (the 10-08 harness sampled CPU every 0.25 s; f2diag had a fixed 1 s), and `launch.json` now
  records `wall_minus_mono`, so log lines can be put on the session clock.
- `out/validation/area-load-20261015/`: `go.sh` (default-launcher run), `env-defaults.txt`, `sample_on_press.sh`
  (`sample` from the A press), `sampleself.py` (self time per thread from a `sample` report), and the ported
  `logt.py` / `frames.py` / `phasecpu.py`.
