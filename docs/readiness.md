# Readiness signals (2026-10-13)

Test tools no longer wait a fixed time for the game to be "probably ready". They ask the game, and a fixed time is
only a guard that is reported when it fires. Perf sweeps are also shorter: `headings8` holds each heading for 0.75 s
instead of 2.5 s.

## The signals

The Metal plugin evaluates them on the GPU Commands thread at every guest swap
(`backends/metal/port/rex/graphics/metal/metal_readiness.{h,cc}`). They are on only when a harness is attached:
`FABLE2_TEST_CONTROL_DIR` (test sessions) or `FABLE2_BENCHMARK` is set, or `FABLE2_READINESS=1`. `FABLE2_READINESS=0`
turns them off. When off, the per-draw hook is one increment and the per-swap hook returns at once.

| Signal | Recorder tag | Meaning |
|---|---|---|
| loaded | `loading` = 0, `level_load` = 0 (existing) | The level-load call ended and 60 swaps in a row came under 100 ms. On its own this fires about 0.76 s early (p90 0.93, max 1.24): the loading screen is still drawn. |
| world | `world` = 1 | The world is drawing: at least `FABLE2_READY_WORLD_DRAWS` (500) depth-target draws per frame for 10 frames in a row. The loading screen draws 90–250; Bower Lake about 2700 (p1 1600); Old Tomb about 1300 (p1 860). It is cleared after 30 frames in a row below the threshold (an area-transition loading screen). |
| steady | `steady` = 1 | Frame pacing is stable. The median of the last 15 swap intervals is within 5% of the 15 before. No interval of the last 15 is over 2× that median. No frame of the last 15 spent 1 ms or more (`FABLE2_READY_PSO_MS`) building pipelines or translating shaders on the draw path (hitch scopes `pso_*` and `xlate`; time, because those scopes are also entered on cache hits). |
| bg | `bg_busy` = 0 | No background shader work is pending. That covers the native-shaders optimizer and native translator (queue, swaps and busy workers), the ns-spec specialize/build queues, async pipeline creation, the startup pipeline warm-up, and ns-spec auto-learn still sampling a shader that has not converged (a sample in the last 1 s). `bg_busy` is the count, probed every 8 swaps. |
| csv | `csv_frame` | The newest guest frame whose frame-CSV rows are on disk. The test-control command `flush [timeout_ms]` writes the CSV now and replies `{flushed_frame, flushed_us}`. |

The test-control `state` reply carries the tags, plus a `ready` object with the detail: `draws`, `world_run`,
`stable_frames`, `med_ms`, `prev_med_ms`, `max_ms`, `frames_since_pso`, `ns_pending`, `ns_spec_pending`,
`ns_spec_sampling`, `pipelines_pending`, `warmup_pending` and `csv_flushed_frame/us`.

GPU time is not part of `steady`, because it needs `FABLE2_GPU_TIME_TRACE`. Uncapped frames track GPU time anyway,
and a capped frame pinned at the cap is stable by definition. The camera-yaw signal ("turn reached its heading") is
not built yet. Turns stay a 0.7 s stick input, which defines the heading; they are not a wait.

## Tools

- `fable2_test_control.wait_ready(session, need=('loaded','world','steady','bg'), timeout=FABLE2_READY_TIMEOUT (60))`
  waits until every needed condition holds at once. It returns `ready_s`, `timed_out`, `missing`, `first_s` (when each
  condition first held) and the `ready` detail. A build without the tags returns `missing: ['unsupported']` after 2 s.
- `flush_frames(session)` sends the `flush` command (returns None on old builds).
- f2diag `run`:
  - sweep, move, f5 and area start when ready, not at `--wait 12` or `--press-at 10`. Those flags remain as legacy
    fixed starts.
  - `result.json` has `ready` (and `plan`); `events.jsonl` has a `ready` event.
  - The sweep's frame CSV is flushed with `flush` instead of a 0.6 s sleep.
  - `run still <label>` captures at fixed guest frames by default since 2026-10-14 (see "Frame-matched parity"
    below). The text that follows describes the older still-wait capture, which `--at-frame off` still selects.
  - `run still <label> --at-frame off` (parity captures, 2026-10-13): after readiness it also waits until the picture is still,
    because `world`/`steady` can hold while the post-load fade-in is still brightening the screen. Still means 3
    captures of distinct frames in a row with mean luma within 0.5 and at most 2% of cells changed
    (`FABLE2_STILL_FRAMES`, `FABLE2_STILL_MEAN_TOL`, `FABLE2_STILL_CHANGED_TOL`); `FABLE2_STILL_TIMEOUT` (20 s) is a
    guard. Then one guest and one screen capture; `result.json` has `ready`, `still`, `guest`, `screen`.
  - Plan deadlines (`--max`, the area plan's ready guard, legacy `--wait`/`--press-at`) count from when the game was
    spawned, not from the session origin, which is stamped before the launch queues for a game slot or the perf
    lock. `result.json` records that queue time as `queue_s`.
- Benchmark mode (`src/core/fable2_benchmark.h`):
  - After gameplay it waits for loaded ∧ world ∧ steady ∧ bg. `FABLE2_BENCHMARK_SETTLE_S` (3 s) and
    `FABLE2_BENCHMARK_MIN_START_S` (12 s) are gone.
  - `FABLE2_BENCHMARK_READY_TIMEOUT_S` (60 s) is a guard; the result records `ready_s`, `ready_after_gameplay_s` and
    `ready_timeout`.
  - The result is written once `csv_frame` reaches the sweep's last frame (3 s guard), instead of after a 1.2 s sleep.
  - `FABLE2_BENCHMARK_PLAN=headings8-long` selects the old plan.
- `fable2_cache_warm.py`:
  - Drained means `bg_busy` stayed 0 while 60 frames were drawn, instead of no log line for 10 s. Older builds fall
    back to the log rule.
  - The title dwell is 120 drawn frames (`--dwell` adds seconds, default 0).
  - The new-game path presses Start as soon as the `movie` tag says the opening movie plays, then waits for loaded ∧
    world.

## Hard-coded waits and what replaced them

| Where | Was | Now |
|---|---|---|
| f2diag `camera_sweep` / `move_check` / `f5_save` | sleep until 12 / 12 / 14 s after launch | `wait_ready` (sweep: all four; move/f5: loaded, world, steady) |
| f2diag `move_check` | sleep 0.5 s after moving | `wait_ready(world, steady)`, 10 s guard |
| f2diag `area_transition` | press at 10 s | press when loaded ∧ world ∧ steady (guard 60 s after launch) |
| f2diag `_main` | sleep 0.6 s for the CSV writer | `flush` acknowledgement (old builds: 0.6 s) |
| f2diag `SWEEP_PLANS['headings8']` | 1.5 s + 7 × (0.7 s turn + 2.5 s hold) | 0.75 s + 7 × (0.7 + 0.75 s); `headings8-long` keeps the old plan |
| `fable2_benchmark.h` | settle 3 s, not before 12 s after launch, 1.2 s before writing | loaded ∧ world ∧ steady ∧ bg; `csv_frame` |
| `fable2_cache_warm.py` `wait_drained` | no pool log line for 10 s | `bg_busy` = 0 over 60 frames |
| `fable2_cache_warm.py` title dwell | 3 s / 5 s | 120 frames drawn (`--dwell` extra, default 0) |
| `fable2_cache_warm.py` new game | 10 s, then Escape, 5 s | `movie` tag = 1, Escape, then loaded ∧ world |
| `fable2_cache_warm.py` menu keys | 4 s / 4 s / 2 s pauses | unchanged: the test-control state has no menu-screen signal (`fable2_state_probe.h` runs only on the remote-control channel) |
| `fable2_cache_warm.py` `--game-dwell` 45 s, A every 8 s | prologue content coverage | unchanged: it chooses how much of the prologue to warm, not a wait for readiness |
| `fable2_portable.py` warm-up | already signal-based (`background warm-up complete`, `background work done`) | unchanged |
| `tools/macos/Benchmark/main.swift` | arm timeout, 5 s kill grace, 0.5 s poll | guards; unchanged |
| `fable2_test_control.py` | reply polls and timeouts | guards; unchanged |

## Why 0.75 s holds (analysis of 189 recorded headings8 runs)

These are recorded frames from 168 f2diag runs and 21 benchmark arms (Bower Lake, Old Tomb and town; 1×/2×/3×;
capped and uncapped), re-cut to simulate the shorter plans. Repeat noise between identical runs: FPS 0.9% median,
GPU 1.0%, p95 1.3%; 1% low is 2.2% median, with p75 6–8% at any length.

| Plan (sweep s) | FPS error vs full, med/p90 | A/B ΔFPS error vs full (pp), med/p90/max | A/B ΔGPU error (pp), med/p90/max |
|---|---|---|---|
| 8 × 1.25 (14.9) | 0.19 / 0.78 | 0.10 / 0.30 / 0.90 | 0.09 / 0.21 / 0.44 |
| 8 × 1.0 (12.9) | 0.14 / 1.04 | 0.11 / 0.35 / 1.06 | 0.12 / 0.30 / 0.65 |
| **8 × 0.75 (10.9)** | 0.14 / 1.06 | 0.15 / 0.43 / 1.22 | 0.13 / 0.38 / 0.86 |
| 8 × 0.5 (8.9) | 0.23 / 1.48 | 0.19 / 0.55 / 1.27 | 0.18 / 0.51 / 0.89 |
| 4 headings × 2 s | 0.8–1.9 / 2.4–2.9 | 0.31–0.35 / 0.9–1.05 / 1.9 | 0.25 / 0.7 / 1.7 |

- Over 86 on/off pairs (alias-clear, native-xlate, rt-shrink, RTT, tiling-collapse, NS_SPECIALIZE, ...), the sign
  agreed in 86 of 86 for every 8-heading plan.
- Dropping headings biases the result, because headings differ in cost.
- After a turn there is no systematic transient: the median frame and GPU time in the first 0.1 s of a hold are within
  0.2% of the hold's steady state. Discarding the first 0.5 s gains nothing.
- After load, the first steady gameplay frames come about 1.2 s after the `loading` tag clears. The old 12 s start
  idled about 6.8 s after that.
- Some features change perf over seconds after load, which `steady` alone would miss and `bg` covers:
  - ns-spec auto-learn converges about 8 s after Continue in learn sessions;
  - Old Tomb and town GPU time drifted 6–7% between "steady" and the old start.
- Risky cases:
  - 1% low is noisy at any length; use `turn30` or a hitch plan for hitch work.
  - Old Tomb and town have larger per-heading scatter.
  - When a `headings8` delta is within ~2%, rerun with `headings8-long`.

Validation of the implementation: see the end of this file. Frame-matched parity: the next section.

## Frame-matched parity (2026-10-14)

Before this, parity captures were taken "when the picture is still". Arms that reached that point a few frames
apart then compared different game moments: torch flicker, fades and animation showed up as on-vs-off
differences. Today's still-wait captures of two identical Old Tomb runs (guest frames 262 and 283) differ by a
mean of **3.28** (0–255 per channel). That is large enough to hide real changes and to raise false alarms.

**The rule now:** parity compares captures of the same guest moment.

### How it works

- **Capture at a guest frame.**
  - Test-control command `capture at <frame> [timeout_ms] [count] [step]` (Metal presenter,
    `CaptureGuestOutputAtFrames`). It reads back exactly the guest output of those guest frames on the GPU thread,
    as each frame's output is produced, ordered after the copy on the presenter queue. So no timing on the
    harness side can miss or shift the frame.
  - It runs on its own thread like `press`, so `state` and other commands keep working.
  - Replies:
    - `ok <ppm> ...`;
    - `error passed <frame>` when the first frame has already been shown (it never captures a later frame instead);
    - `error timeout|partial <frame> <ppm> ...`.
  - The `.capture.json` has `requested_frame` beside `frame_id`.
  - Python: `fable2_test_control.capture_at(session, frame, timeout, count, step)`, or the CLI
    `capture <session> --at-frame N`.
  - It costs nothing when no request is pending (one atomic load per guest frame).
- **Anchors.**
  - The recorder `state` reports `load_end_frame` (the guest frame when the last level-load call returned) and
    `loading_clear_frame` (the frame when the `loading` tag cleared).
  - The boot path before the load is not quite deterministic: Old Tomb's load ended at frame 97–102 in eight runs.
    Targets are therefore given relative to the load.
- **Frame spec** (`--at-frame`):
  - `load+N` (default `load+240`, 4 s after the load at 60 FPS). With a warm cache, readiness came at load+97 to
    load+154 in 26 runs (Old Tomb about 100, Bower Lake about 153);
  - `clear+N`;
  - an absolute guest frame `N`;
  - `off` (the old still-wait).
  - `FABLE2_PARITY_AT_FRAME` changes the default.
- **Bursts and shift.**
  - The game advances by wall time, and the hitches after a load differ from run to run. So two runs can be a few
    game ticks apart at the same load-relative frame.
  - Measured: Old Tomb A-A 0.14 at the same frame id against 0.03 at the best shift. Bower Lake 0.37 within ±2
    against 0.09 at its best shift of a ±6 search. Three of twelve arms were 2 ticks off.
  - `run still` therefore captures `--frames 3` bursts of `FABLE2_PARITY_BURST` (9) consecutive frames, centred on
    target, target + 30 and target + 60 (`--frame-step`).
  - `f2diag parity` matches each burst at its best shift within ±4 (`--max-shift`), judged at the burst's middle
    frame, after aligning the arms' targets.
  - A rendering change raises every shift; timing noise only some. A burst that matches at the edge of the search
    is reported (`note: ... edge`).
  - If the target has already passed when the game is ready (cutscene saves, cold caches), the plan prints
    `FRAME MATCH: ... passed` and falls back to the still-wait capture. Use a larger `load+N` for those saves.
- **No still-wait** in frame mode: both arms see the same moment of any fade or flicker.
- **Game time.** After readiness, the game runs about 2.5 s (Bower Lake) to 3.5 s (Old Tomb) until the last burst.
  The still-wait took 0.9–1.3 s. That is the cost of a fixed target that every arm reaches.
- **Sweeps:** `run sweep <label> --sweep-plan spin5 --at-frame load+300`.
  - The sweep starts when the game shows the target frame: the first burst is the sync.
  - Bursts follow every `--frame-step` (60) frames during the spin (`--frames`, auto up to 8).
  - The stick still runs on wall time, so a spin's A-A floor stays coarse (Bower Lake spin5 A-A: 2.07, shifts −1).
  - Use it to look for artifacts at matched angles, not for fine parity. Still captures are the fine check.

### The parity command

```sh
python3 tools/f2diag.py run still pA1 --save <save> [arm A switches]     # twice per arm: the A-A floor
python3 tools/f2diag.py run still pA2 --save <save> [arm A switches]
python3 tools/f2diag.py run still pB1 --save <save> [arm B switches]
python3 tools/f2diag.py run still pB2 --save <save> [arm B switches]
python3 tools/f2diag.py parity pA1,pA2 pB1,pB2 [--json out.json] [--heat DIR]
```

- **Arms.** Run labels, run folders or session folders. All guest captures with a frame id count, and
  frame-matched captures win.
- **Pairs.** Cross A×B; floor A×A and B×B. Per matched frame it reports:
  - the mean absolute difference;
  - the fraction of pixels over 8 and over 32;
  - every 32×32 block's mean difference.
- **Verdicts:**
  - `identical`;
  - `within-noise`: the cross median is ≤ the floor max + 0.05, and no block exceeds the same block's floor max by
    more than `--block-tol` (4.0). Animated regions that also differ A-A do not count; a change in a still region
    does;
  - `beyond-noise`: the hot blocks are listed, and `--heat` writes heat maps;
  - `no-floor`: only one run per arm;
  - `no-match`.
- **Exit code.** 0 for identical or within-noise, otherwise 1.
- **Speed.** Pure Python (no numpy): byte-equal rows and block slices are skipped. About 0.15 s per 1280×720 pair.
- Based on the image-diff and frame-match scripts of the 2026-10-13 correctness soak.
- **Scaled runs.** At 2×/3× each guest capture is 4×/9× larger (3840×2160 PPM ≈ 25 MB). Use `--frames 1` there.

### Validation (2026-10-14, agent-2, M5 Max, Old Tomb `hero1-dog-live_2026-10-07_14-45-08`, 1×)

Evidence: `out/validation/parity-tools/` (KEEP) in slot agent-2. Runs: `out/validation/f2diag/pt-*`.

**Floor and detection** (`f2diag parity`, two runs per arm). These are mean absolute differences, 0–255 per
channel. "Floor" is A-A.

| Comparison | Method | Cross median (max) | Floor median (max) | Verdict |
|---|---|---|---|---|
| default × default | old still-wait, guest frames 262 vs 283 | — | **3.28** (one pair) | — |
| default × default | same guest frame id, no shift | — | 0.14 (0.35), 48 frames | — |
| default × default | frame-matched, `load+300`, 9-frame bursts | — | **0.045 (0.11)** | — |
| default × `--anisotropic_override=2` (deliberate change: 2:1 anisotropic filtering, about 1% of pixels > 8) | frame-matched | **0.81 (0.97)** | 0.055 (0.36); noise limit 0.15 | **beyond-noise** |
| default × `FABLE2_NATIVE_TESS=2` | frame-matched, 5-frame bursts | 0.035 (0.33) | 0.028 (0.08) | within-noise (no adaptive terrain indoors) |
| default × all nine soak switches `=0` | frame-matched | 0.042 (0.23) | 0.043 (0.27) | within-noise |

- The soak judged the same save against a frame-matched floor of 0.35–0.80 (its matched still-wait captures) and
  an unmatched one of up to 0.97. A change of the anisotropic size (0.8) would have read as noise there. It is now
  15× the noise median.
- Every arm reached its frames exactly (`requested_frame == frame_id` for every capture). Load ends ranged from
  frame 97 to 102.
- **Bower Lake** (`hero1-dog-live_2026-10-11_11-20-20`, the perf save, camp fire and foliage):
  - A-A 0.092 (max 0.11) at a −1 shift with 13-frame bursts;
  - 0.37 when the search was limited to ±2. That run pair was at least 2 ticks apart, which is why the defaults
    are now 9-frame bursts and ±4.
- **Bower Lake spin5** with `--at-frame load+300`, A-A: 2.07 (max 5.5). The spin's stick timing is wall-clock, so
  frame-matched spins are only good for spotting artifacts at matched angles.
- **Speed.** A 4-arm parity (6 pairs, 3 bursts of 9 frames) takes about 25 s.


## Validation (2026-10-13, agent-4, M5 Max)

- **NATIVE_XLATE 0→1, Bower Lake 2× uncapped, one run per arm, GPU time trace on**
  (`out/validation/f2diag/rdy-nx*`):

  | Plan | ΔFPS | ΔGPU ms |
  |---|---|---|
  | headings8 | +4.6% (110.5 → 115.5) | −8.9% |
  | headings8-long | +4.6% (109.6 → 114.7) | −9.2% |

  - The first nx1 headings8 run gave +1.8% FPS and −10.2% GPU. It was the first launch with native MSL in a cold
    macOS shader cache (worse p95 and 1% low). The repeat matches.
  - Readiness came 10.1–13.8 s after launch and the guard never fired.
  - What held it each time was background shader work: the native-shaders optimizer (about 178 shaders per
    test session, because test sessions don't write their cache back), cold native-xlate compiles (184 shaders,
    about 10 s), and one ns-spec auto-learn shader converging about 12 s in.
  - The old fixed 12 s start therefore measured part of the sweep before those optimizations were swapped in.
  - Since 2026-10-13 test sessions start from a shared warm shader cache keyed by shader code and publish their
    work back, so this background work is done once per shader-code key, not once per test game.
- **Game time per run**: launch to close took 23.8–26.2 s with headings8, against about 37.7 s for the old plan with
  the 12 s start (and 36.8–39.1 s for headings8-long with readiness).
- **Benchmark mode**, one 2× uncapped arm:
  - `ready 13.4 s after gameplay` (native-shaders work done at 17.3 s after launch, ns-spec converged at 22.2 s), with
    no guard firing;
  - the sweep took 10.95 s;
  - the result was written 0.18 s after the sweep (`csv_frame`), instead of after a fixed 1.2 s.
