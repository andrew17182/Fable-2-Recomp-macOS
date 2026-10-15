# Uncapped frame limiter and 60 FPS headroom on an M3 Pro — 2026-10-07

> **Superseded in part by `docs/weak-mac-perf-20261008.md` (2026-10-08).**
> - CR-03's cost was not the export draws: it was a barrier/fence-wait storm (292 + 1,148 per frame) after
>   the pending-write list collapsed. `FABLE2_RENDER_BARRIER_DEDUP` removes it, and the exporters now run
>   export-only. All-on GPU is now 6.2–6.3 ms mean, p95 10.9, heavy frames 10.6 (161 FPS uncapped).
> - The GameThread and 3D Engine spin-waits now block (`FABLE2_GAME_TICK_SLEEP`, `FABLE2_RING_WAIT`). The
>   lr-sampler `game_work` figure no longer applies, because the thread sleeps outside the sampled loop.
> - The M3 Pro prediction below is replaced: the 18-core part is borderline (about 48–73 FPS average,
>   heavy views 29–43); the 14-core part is unlikely (37–57). Busy cores at 60 FPS are 1.45, against 3.08.
> - Levers 1, 2, 5 and 7 below are done; 6 (CR-09) needs an SDK rebuild; 3 (MSAA) is not a toggle-sized change.

This is a measurement only; nothing is fixed. It answers whether the uncapped game is GPU-bound or
CPU-bound on this M5 Max (40-core GPU), and predicts 60 FPS headroom on a MacBook Pro M3 Pro (14- or
18-core GPU, 5–6 P-cores). All data is in `out/validation/gpu-bound-20261007/`. The tree is the CR-fixes
tree (`docs/cr-fixes-20261007.md`) plus the trace below.

## Answer

- **The GPU is the limiter at uncapped rates, with all fixes on.**
  - The GPU has work in flight 99% of wall time.
  - GPU time per frame equals the frame time: mean 7.2–7.5 ms, against a wall time of 7.2–7.5 ms.
  - GPU Commands, the translation thread, spends 25–32% of wall time blocked on GPU completion (the
    3-frames-in-flight slot wait). It is never starved: it waits on an empty ring less than 1% of the time.
  - The 3D Engine and the GameThread spend most of their time in wait loops, waiting for the stage
    downstream of them.
- **With `FABLE2_MEMEXPORT_GEOM_FALLBACK=0` (CR-03 off), the GPU is still formally the limiter, but
  GPU Commands is close behind.** The GPU is busy 98–99%, and GPU Commands is active 93–94% of the time,
  with slot waits of 6%.
- **CR-03 adds about 1.1 ms of GPU time per frame on average.** Most of it lands on heavy-view frames:
  about 4.4 ms extra there (14.8 against 10.4 ms). The share of heavy frames also rises, from 10% to 23%.
- **M3 Pro prediction.** The GPU is what limits it, and the CPU threads fit (details below).
  - **18-core, CR-03 on:** 60 FPS is unlikely.
  - **18-core, CR-03 off:** borderline. Light views reach about 60; heavy views about 30–45 FPS.
  - **14-core, either setting:** 60 FPS is unlikely.

## Instrumentation (trace-only, off by default)

`FABLE2_GPU_TIME_TRACE=1` writes extra events to the existing frame CSV. With the variable unset,
every hook is a no-op and the sampler thread is never started. Events tagged frame F cover the interval
from guest swap F to swap F+1.

| Event | Source |
|---|---|
| `gpu_cb`, `gpu_cb_sa` | `GPUStartTime`/`GPUEndTime` of every frame and standalone command buffer, from a completed handler |
| `tr_slot_wait_us` | GPU Commands blocked in `WaitForFrameSlotSubmission` (GPU completion, 3 frames in flight) |
| `tr_cp_idle_us` | GPU Commands idle from `PrepareForWait` to `ReturnFromWait` (ring empty, or a guest `WAIT_REG_MEM`) |
| `tr_sync_wait_us` | Synchronous GPU waits (`FlushCommandBufferAndWait`, `CommitStandaloneAndWait`); about 0 in gameplay |
| `tcpu_*_us` | Per-frame `thread_info` CPU time of GPU Commands, GameThread, 3D Engine and Audio Worker |
| `smp_3d_*`, `smp_game_*` | Guest wait sampler (below): per-frame sample counts, total and in-wait |

**The guest wait sampler.** CPU% cannot tell work from waiting here: the GameThread and the 3D Engine
are at 90–100% CPU even at 60 FPS capped, because they spin while they wait. A host thread reads each
thread's guest link register (`PPCContext::lr`) about every 0.38 ms (about 2.6 kHz). It never
suspends the threads, and it counts samples that sit in a known guest wait loop:

- **3D Engine.** The D3D ring/read-pointer spin: loop `82242628` (`lr = 822426E8`), calling poll
  `82B9BF90` (`lr` in `82B9BF98`, `82B9C010` or `82B9C04C`). The poll spins with `db16cyc`
  on the read-pointer writeback (`[r29+10896]`) and has a 5000 ms GPU-hang timeout. So the 3D Engine is
  waiting for GPU Commands to consume the ring.
- **GameThread.** The yield path of `ProcessGameFrame`: `YieldAndCheckThreshold` (`lr = 82CBD0A8`, then
  `8227720C`).
- Every 10 s the log prints the top `lr` values (`[gpu-time-trace] guest lr top`). These three
  3D Engine addresses make up 64–84% of all its samples, and `82CBD0A8` makes up 78–80% of the GameThread's.

A thread's work in a frame is the wall time multiplied by the fraction of that frame's samples that
were not in a wait loop. With about 20 samples per 7 ms frame, single-frame values are coarse; the
means and window shares are solid.

**`/usr/bin/sample` is unusable for this question.** It suspends the whole task. During each 3 s
sample, the game dropped to 4–28 FPS, the slot waits disappeared, and the wait shares flipped: the
GameThread showed 18% yield under `sample` against 79% under the `lr` sampler. Its stacks did identify
the wait loops above. `sample-on2.txt`, `sample-off2.txt` and `sample-cap60on.txt` are kept, and
`sshare.py` summarizes them.

**Files:**

- `backends/metal/port/rex/graphics/metal/metal_command_processor.cc` and `.h`. The `.h` adds a
  `ReturnFromWait` override that only times the idle wait.
- `metal_frame_log.cc` and `.h` (`RecordUptime`).
- The instrumentation-only diff is `out/validation/gpu-bound-20261007/gpu-time-trace.patch`. The
  restore point is `../restore-points/2026-10-07-gpu-time-before.patch`, plus the
  `2026-10-07-gpu-time-before/` folder for the untracked files.

## Runs

The harness is `run.sh` (the CR-fixes `sweep.py`, a 30 s camera sweep). Save
`hero1-dog-live_2026-10-07_14-45-08` (Old Tomb), launcher defaults, background test mode, target slot
none, and the warm cache of the CR-fixes `after4` sweep session.

- Analysis: `gpustats.py <session>`, over the 20–50 s window, which ends at about 47 s when the sweep
  closes. `summary.json` covers all runs; `sessions.txt` maps run labels to sessions.
- Every launch was checked with `pgrep -x fable_2` beforehand; every run closed with `pgrep` = 1.

| Run | Build | Config | Notes |
|---|---|---|---|
| on0-inert | 1 | all on | Trace inert (enable check cached before the frame log existed); 137.5 FPS. First launch after build 1 |
| on1 | 2 | all on | First launch after build 2 |
| on2 | 2 | all on | `sample` at about 30 s (distorted; excluded from the medians below) |
| on3 | 3 | all on | First launch after build 3; `lr` sampler inert (thread-name bug) |
| on4 | 4 | all on | First launch after build 4; full trace |
| on-notess | 4 | all on, `--metal_fable_terrain_uniform_tessellation=false` | Lever A/B |
| off1 | 2 | `GEOM_FALLBACK=0` | |
| off2 | 2 | `GEOM_FALLBACK=0` | `sample` at about 30 s (distorted) |
| off4 | 4 | `GEOM_FALLBACK=0` | Full trace |
| cap60on | 2 | all on, 60 FPS | `sample` at about 28–31 s (12–28 FPS while sampling) |
| cap60on2 | 4 | all on, 60 FPS | Full trace |

**First launch after a rebuild.** These launches are counted, not discarded. Four of the all-on runs
were first launches (builds 1–4).

- In-game frame rate shows no penalty: 137.5, 134.6, 133.2 and 138.2 FPS, against 138.5 for the
  non-first on-notess run and 135.5 for the CR-fixes reference.
- The extra cost is at startup:
  - From the session folder to the first log line takes about 1.5–2.0 s, against 0.4–0.9 s.
  - Guest frame 1000 arrives at 5.5–6.0 s, against 5.3–5.4 s.
  - In total, about 1.0–1.5 s more to reach gameplay. This matches the one-time verification of a new
    binary.
- Trace overhead: traced all-on runs average 133–138 FPS, against 135.5 untraced (CR-fixes reference).

## Measured: per-frame GPU time against CPU thread time (M5 Max, ms)

"GPU" is the union of the `GPUStartTime`–`GPUEndTime` intervals of that frame's command buffers: the
time the GPU had that frame's work in flight. The plain sum is 0.6–1.5 ms higher, because consecutive
command buffers overlap.

- **CP active** is the GPU Commands wall time minus its idle, slot and sync waits. It matches that
  thread's `thread_info` CPU time within 0.1 ms, so the thread is on-CPU then.
- **3D Engine work** and **GameThread work** come from the `lr` sampler (wait loops excluded).
- The raw CPU columns include spinning: CPU per frame is about equal to wall time for GameThread and
  3D Engine.

**All fixes on, uncapped.** Runs on1, on3 and on4; ranges are across runs.

| Metric | Mean | p50 | p95 | p99 |
|---|---|---|---|---|
| Wall frame time | 7.24–7.51 | 5.46–5.97 | 15.4–16.9 | 16.9–17.9 |
| **GPU time** | **7.24–7.50** | **5.58–5.99** | **15.3–16.7** | **16.7–17.6** |
| GPU Commands active (translation and encoding) | 5.05–5.34 | 4.44–4.65 | 9.4–10.0 | 10.1–10.6 |
| GPU Commands blocked on GPU (slot wait) | 1.95–2.35 | 1.1–1.6 | 6.3–6.9 | 7.2–7.8 |
| GPU Commands idle, ring empty | 0.02–0.04 | 0.02–0.03 | 0.05–0.17 | 0.14–0.28 |
| 3D Engine work (on4) | 2.05 | 1.90 | 3.76 | 3.87 |
| GameThread work (on4) | 1.50 | 0.81 | 5.62 | 7.38 |
| Audio Worker CPU (polling) | 5.3–5.4 | 4.5–4.9 | 12.7–13.2 | 14.8–15.4 |

Window shares:

- The GPU has work in flight 99.0–99.2% of the time.
- GPU Commands slot wait is 27–32% of wall time.
- In on4, the 3D Engine spends 71.7% of its samples in the ring-wait spin, and the GameThread 79.3% in
  the yield path.

**`FABLE2_MEMEXPORT_GEOM_FALLBACK=0`, uncapped.** Runs off1 and off4.

| Metric | Mean | p50 | p95 | p99 |
|---|---|---|---|---|
| Wall frame time | 6.19–6.22 | 5.14–5.16 | 10.4–10.5 | 11.1–11.2 |
| **GPU time** | **6.19** | **5.12–5.14** | **10.4** | **11.0–11.1** |
| GPU Commands active | 5.75–5.82 | 4.80–4.87 | 10.0–10.2 | 10.5–10.7 |
| GPU Commands slot wait | 0.36–0.40 | 0.0–0.1 | 1.2–1.3 | 1.7–1.8 |
| 3D Engine work (off4) | 2.24 | 1.96 | 3.79 | 3.92 |
| GameThread work (off4) | 1.34 | 0.80 | 4.77 | 6.54 |

Window shares: GPU busy 98.4–98.9%; slot wait 5.8–6.5%; 3D Engine wait 63.8%; GameThread wait 78.4%.

**All fixes on, 60 FPS capped.** Run cap60on2.

- The wall time is 16.67 ms mean (p95 18.2).
- GPU time is **12.6 ms mean** (p95 16.0). This is not a cost figure: the GPU is busy only 75% of the
  time, and the M5 Max lowers its GPU clock when it has slack, so each frame's work stretches to fit.
  The uncapped numbers, at full clock, are the cost.
- GPU Commands active: 6.73 ms (p95 10.4); its idle share is 60%.
- 3D Engine work: 2.65 ms (p95 3.86).
- GameThread work: 3.26 ms (p95 7.64). This is about twice the uncapped figure, either because the game
  does more work per frame when frames are longer or because the CPU clock is lower; this run cannot tell
  which.

**The frames are bimodal.** In the all-on runs, 23% of frames take more than 10 ms; these are the
heavy-view phases of the sweep.

| Frame group | Wall | GPU | CP active | 3D Engine work | GameThread work |
|---|---|---|---|---|---|
| Heavy, all on (on4) | 14.9 | 14.8 | 9.2 | 3.3 | 3.0 |
| Heavy, CR-03 off (off4) | 10.5 | 10.3 | 9.8 | 3.5 | 2.4 |
| Light, all on (on4) | 4.9 | 4.9 | 4.0 | 1.7 | 1.0 |
| Light, CR-03 off (off4) | 5.7 | 5.8 | 5.3 | 2.1 | 1.2 |

## The limiter, and what each thread waits on

The pipeline is GameThread → 3D Engine (guest D3D, writes the ring) → GPU Commands (translates the
ring to Metal) → GPU. The stages run in parallel, so the slowest one sets the frame rate.

- **All fixes on.** The GPU is saturated (99%). GPU Commands waits on GPU completion for 27–32% of the
  wall time. The 3D Engine spins on the ring read pointer (72%), because GPU Commands is not draining
  the ring while it waits. The GameThread yields (79%), waiting on the 3D Engine.
- **CR-03 off.** The GPU is still at 98–99%, but GPU Commands is active 93–94% of the time, with only 6%
  slot wait. Both are close to the limit; the GPU just wins. The next limiter after the GPU would be
  GPU Commands' translation cost: 5.8 ms mean and 10 ms p95 on heavy frames.
- **CP active time grows when the GPU wait shrinks.** Mean CP active is 5.05–5.34 ms with CR-03 on,
  5.75–5.82 ms with it off, and 6.7 ms at 60 FPS, although CR-03 off has fewer draws to translate. Part
  of the "active" time is therefore probably elastic: the CPU clock, or Metal calls that spin on the
  CPU. So 5.1 ms is the better estimate of its real cost, and 6.7 ms is pessimistic.
- **The GameThread and the 3D Engine are far from limiting**: 1.3–3.3 and 2.0–2.7 ms of work per frame.
- **Audio Worker** uses 73–77% of a core while about 98% of its samples are in `nanosleep`; this is the
  1 ms polling of CR-09.
- **CR-03's cost is not in the tessellated terrain.**
  - Adaptive tessellation instead of the uniform maximum (`--metal_fable_terrain_uniform_tessellation=false`)
    measured the same: GPU mean 7.22 against 7.24 ms; p95 15.4 against 16.7 ms is within the
    run-to-run spread of 15.3–16.7 ms.
  - The CR-fixes work found that mode 2 (plain points, no geometry emulation) costs the same as mode 1.
  - So the cost is most likely in running the 39 export-only draws per frame against the 4x MSAA D32S8
    target, plus the barriers and pass breaks around them. This was not verified per pass.

## Prediction for the M3 Pro at 60 FPS

### Measured inputs (M5 Max, full clock, uncapped)

| | GPU mean | GPU p50 | GPU p95 | GPU heavy-frame mean |
|---|---|---|---|---|
| All on | 7.3 | 5.7 | 16.0 | 14.8 |
| CR-03 off | 6.2 | 5.1 | 10.4 | 10.3 |

CPU work per frame (both configurations; the high end is from the 60 FPS run):

- GPU Commands: 5.1–6.7 ms mean, 9.4–10.4 ms p95, 10.1–10.8 ms p99.
- 3D Engine: 2.0–2.7 ms mean, 3.8–3.9 ms p95.
- GameThread: 1.3–3.3 ms mean, 4.8–7.6 ms p95.

### Assumptions (estimates, not measured here)

- **GPU factor (M5 Max 40-core to M3 Pro):**
  - Core ratio: 40/18 = 2.22 and 40/14 = 2.86.
  - Per-core M5 against M3: about 1.3–1.5×. This is an estimate from public Geekbench 6 Metal results
    as I recall them: roughly 70–75k for the M3 Pro 18-core and roughly 220–240k for the M5 Max
    40-core, which is about 4k against 5.5–6k per core. Clock and architecture both contribute.
  - Range used: **18-core 2.2–3.3×**, **14-core 2.8–4.3×**. The low end assumes this small 720p
    workload does not use the 40-core GPU fully, so a smaller GPU loses less than the core ratio.
  - Memory bandwidth (M3 Pro 150 GB/s against about 550–600 GB/s, also an estimate) is about 4×. That
    would push bandwidth-heavy passes (4x MSAA depth, EDRAM transfers) toward the high end.
- **CPU factor (M5 P-core to M3 P-core, single thread): 1.35–1.45×.** This is an estimate from public
  Geekbench 6 single-core results as I recall them: M3 about 3,050–3,100, M5 about 4,100–4,300. It
  assumes each critical thread gets a P-core.

### Predicted M3 Pro times per frame (ms, measured × factor)

| | GPU mean | GPU p50 | GPU heavy-frame mean | Verdict for 60 FPS |
|---|---|---|---|---|
| 18-core, all on | 16–24 | 12.5–19 | 33–49 | **Unlikely.** The average is at or below 60 at best; heavy views run at about 20–30 FPS |
| 18-core, CR-03 off | 13.6–20.5 | 11.3–17 | 23–34 | **Borderline.** Light views run at about 50–60+ FPS; heavy views at about 30–45 FPS |
| 14-core, all on | 20–31 | 16–24.5 | 41–64 | **Unlikely.** Average about 30–50 FPS |
| 14-core, CR-03 off | 17–27 | 14–22 | 29–44 | **Unlikely.** Average about 37–58 FPS |

CPU on the M3 Pro (the same for both GPUs):

- GPU Commands: about 6.9–9.7 ms mean, 12.7–15.1 ms p95, 13.6–15.7 ms p99.
- 3D Engine: 2.7–3.9 ms mean, about 5.5 ms p95.
- GameThread: 1.8–4.8 ms mean, 6.5–11 ms p95.

All three fit within 16.7 ms, so **the CPU is achievable for 60 FPS**. GPU Commands has the least
margin: its p99 is within about 1–3 ms of the budget. The CPU risk is scheduling, not speed:

- The GameThread and the 3D Engine each spin a full core even while they wait.
- Audio Worker takes about 0.75 of a core.
- Unnamed threads take about 0.5 of a core.

That is about 3.3 cores busy before GPU Commands runs, on a chip with 5 or 6 P-cores. If GPU Commands
lands on an E-core, which is about 2–3× slower, it misses 16.7 ms.

**Caveats:**

- One scene (Old Tomb) and one 30 s sweep; heavier areas such as towns, combat and particles were not
  measured.
- The GPU time is "work in flight", so it is an upper bound on pure GPU execution.
- The scaling factors are the main source of uncertainty: roughly ±25%.

## Cheap levers for a weaker Mac (list only; nothing implemented)

Status 2026-10-08 (`docs/weak-mac-perf-20261008.md`):
- 1 is done: export-only exporters plus the barrier dedup, which was the real cost.
- 2 is done: `passes-*.csv`.
- 5 is done.
- 7 is done.
- 6 needs an SDK patch, which is proposed there.
- 3 needs a render-target-cache redesign.
- 4 already exists in the launcher.

1. **CR-03's export-only draws** (VS 695413A9, about 39 per frame). They rasterize depth-only into the
   game's 4x MSAA D32S8 target, although only their memexport matters.
   - Options: draw them with rasterization discarded or no depth attachment, batch them, or run them as
     compute.
   - Upper bound of the gain: CR-03 off, which is about 1.1 ms mean and 4.4 ms on heavy frames on the
     M5 Max, probably 2.5–4.5× that on an M3 Pro.
   - Cheapest stopgap: default to mode 0 on low-end GPUs, accepting tessellation factor 1.
2. **Measure the GPU per pass before optimizing further.** Add `MTLCounterSampleBuffer` stage-boundary
   timestamps per encoder, trace-only, to see what fills the remaining 5–6 ms: EDRAM/render-target
   transfers, resolves, 4x MSAA passes, tessellation.
3. **An MSAA downgrade option.** Draw guest 4x MSAA targets at 2x or 1x on low-end GPUs. This cuts fill
   and bandwidth, which counts more at the M3 Pro's 150 GB/s. It loses edge quality.
4. **A 30 FPS cap on weak Macs** (`FABLE2_FPS_60=0`, the console's rate; it already exists). This
   doubles the GPU budget to 33 ms, which covers every predicted mean and most heavy frames.
5. **Stop the spin-waits from burning P-cores.**
   - 3D Engine: in the D3D read-pointer poll `82B9BF90`, block on a command-processor read-pointer event,
     or sleep about 0.1 ms after N spins.
   - GameThread: in the yield loop (`hotfunc_yield_every` / `BatchedYield`), sleep instead of
     `swtch_pri` after a threshold.
   - This frees about 1.5–2 P-cores for GPU Commands and the GPU driver.
6. **Audio Worker 1 ms polling (CR-09).** An event-driven wait frees about 0.75 of a core.
7. **Keep GPU Commands on a P-core.** Give the thread user-interactive QoS. It is the CPU critical path,
   at 5–6.7 ms per frame and about 10 ms p95.
8. **Reduce the per-draw CPU cost of GPU Commands.** In the distorted sample, about 78% of its time is
   in `IssueDrawImpl`: constant payloads, bindless tables and descriptor writes. This matters only once
   the GPU is fixed, but it sets the M3 Pro's p99.
9. **Not a lever:** `metal_fable_terrain_uniform_tessellation=false`. No measurable change (one run).
