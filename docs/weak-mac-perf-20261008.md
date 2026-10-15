# Performance on weaker Macs (target: MacBook Pro M3 Pro at 60 FPS) — 2026-10-08

The brief was `docs/archive/perf-headroom-20261007.md`: on the M5 Max the game was GPU-bound uncapped at
7.2–7.5 ms of GPU time per frame (p95 15.3–16.7), and two guest threads each spun a P-core. This
document covers what was changed and measured, and gives an updated M3 Pro prediction.

Every change below is a pure performance fix with no visual change. Each one is on by default and has
an environment kill switch (`=0`); none has a launcher toggle. Each logs one startup line.

| Change | Kill switch | Startup log tag |
|---|---|---|
| Skip redundant render-encoder barriers and fence waits | `FABLE2_RENDER_BARRIER_DEDUP=0` | `[barrier-dedup]` |
| CR-03 exporter VS 695413A9 runs export-only (vertex pipeline, no fragment stage, no geometry emulation) | `FABLE2_MEMEXPORT_GEOM_FALLBACK=3` gives the previous mode-1 behaviour (`=2` plain points, `=0` skip) | `[memexport-geom]` |
| GameThread sleeps until just before its next 30 Hz tick instead of yield-spinning | `FABLE2_GAME_TICK_SLEEP=0` | `[game-tick-sleep]` |
| 3D Engine blocks on command-processor progress instead of spinning on the ring read pointer | `FABLE2_RING_WAIT=0` | `[ring-wait]` |
| GPU Commands thread at user-interactive QoS | `FABLE2_GPU_CMD_QOS=0` | `[gpu-cmd-qos]` |
| Staged shared-memory uploads of pages read only by committed command buffers go to a pre-commit command buffer instead of ending the render pass (step 7) | `FABLE2_UPLOAD_PRECOMMIT=0` | `[upload-precommit]` |
| Texture loads (untile/convert compute + blit) of a draw flush go to the same pre-commit command buffer instead of ending the render pass, when no draw of the open command buffer sampled the texture and its source is not GPU-written in it (step 8) | `FABLE2_TEXTURE_PRECOMMIT=0` | `[texture-precommit]` |
| Per-encoder GPU timing, trace-only (`FABLE2_GPU_TIME_TRACE=1`; `FABLE2_PASS_TRACE=0` turns off just this part) | — | `[pass-trace]` |

**Where things are.**

- Harness, logs, dumps and scripts: `out/validation/weak-mac-perf-20261008/`.
  - `sessions.txt` maps run labels to sessions.
  - `go.sh` runs one sweep and prints its stats.
  - `passstats.py` gives per-pass GPU time.
  - `pacing.py` gives frame intervals.
  - `cmpdump.py` / `dumpread.py` compare draw dumps.
  - `movesave.py` runs the walk check and the F5 save check.
- Restore point: `../restore-points/2026-10-08-weak-mac-perf-before.patch`. It is empty because the tree was clean at 4d2b6e7.
- Nothing was committed.

## Before / after (M5 Max, Old Tomb 30 s camera sweep, window 20–50 s)

**Harness.** `run.sh` from `gpu-bound-20261007`, with the save `hero1-dog-live_2026-10-07_14-45-08`, the
launcher defaults, background test mode, target slot none, the same warm cache, and `FABLE2_GPU_TIME_TRACE=1`.

**Columns.**

- "Old" is the previous behaviour in the same build (B2: `GEOM_FALLBACK=3 RENDER_BARRIER_DEDUP=0`).
- The `perf-headroom` reference is 133–138 FPS and GPU 7.24–7.50 / 15.3–16.7.
- "GPU fixes" is C6: all CPU switches off.
- "All on" is C2 (the final build) for uncapped and E1 for 60-capped.
- CPU ms are per-frame `thread_info` time. "Cores" is the sum of the four named threads' CPU divided by wall time.

| Metric | Old (B2) | GPU fixes only (C6) | All on (C2 / E1) |
|---|---|---|---|
| Uncapped FPS | 134.0 | 160.6 | 158.6 (161.3 in B1, build 2) |
| GPU ms/frame mean / p95 | 7.46 / 15.55 | 6.21 / 10.92 | 6.29 / 10.86 |
| GPU ms, heavy frames (wall > 10 ms) | 13.93 (930 frames) | 10.68 (450) | 10.58 (395) |
| Render barriers / fence waits per frame | 292 / 1148 | ≈ 38 / 82 | 43 / 91 |
| Main 4x pass vertex time per frame | 1.84 ms | 0.53 ms | 0.53 ms |
| GPU Commands active (ms/frame) | 5.14 | 5.47 | 5.36 |
| 3D Engine real work (lr sampler) | 1.92 | 2.03 | 2.05 |
| CPU ms/frame (% of a core), uncapped: GPU Commands | 5.15 (69%) | 5.47 (88%) | 5.37 (85%) |
| CPU, uncapped: GameThread | 7.10 (95%) | 5.88 (94%) | 0.79 (12%) |
| CPU, uncapped: 3D Engine | 7.45 (100%) | 6.21 (100%) | 2.59 (41%) |
| CPU, uncapped: Audio Worker | 5.54 (74%) | 4.62 (74%) | 4.66 (74%) |
| Cores kept busy, uncapped | 3.38 | 3.56 | **2.13** |
| 60-capped: frame-time p99, gaps > 33 ms | — | 18.52 ms, 0 (E2) | **18.37 ms, 0** (E1) |
| 60-capped CPU per frame: GameThread / 3D Engine / GPU Commands / Audio | — | 16.0 / 16.6 / 6.2 / 12.4 | **2.0 / 3.6 / 6.2 / 12.3** |
| Cores kept busy, 60-capped | — | 3.08 | **1.45** |

**Uncapped run-to-run spread.**

- With all CPU switches on, uncapped FPS was 154.5–158.6 (C1–C5). With them off it was 160.6 (C6), and the build-2 run reached 161.3.
- The about ±2% difference is within the spread of identical configurations measured before (133–138).
- The wall p99 tail of 17–19 ms appears in C6 too, which has every CPU change off. It is GPU queueing in the heavy part of the sweep: in those frames the GPU is busy for the whole interval with earlier frames' work. B1 and B4 (build 2) happened not to show it.

**Consumers unchanged.** These are draw dumps of frames 2000–2020 in the static idle view (`cmpdump.py`):

- Old mode (B2) against export-only on the final build (H1).
- All 336 tessellated terrain draws (VS 5003700B7C9B1C16) have identical guest and GPU index-stream hashes.
- The factor ranges are the same (0–15, 0–3.43, 0–0.23).
- All 168 exporter draws run without geometry emulation in H1. In B2 all 168 used it.

**Visual.** Idle-capture pixel differences between runs are within run-to-run animation noise: 237–6,513 pixels > 8 between unrelated configurations. Neither change can alter output by construction:

- The dedup only skips synchronisation that an earlier barrier or wait in the same encoder already provides.
- The exporter writes oPos = (0,0,0,0), so it never produced pixels.

**Movement and save** (`movesave.py`, all on, 60 FPS, session `20261007-203032-b3db8da6`):

- After a 3 s walk and a 2 s strafe, 49.5% of pixels changed, against 0.8% while idle.
- F5 made the library entry `hero1-dog-live_2026-10-07_20-30-57`.
- 0 AVs and a clean close.
- 0 AVs in all 17 sessions; every run was checked with `pgrep -x fable_2` before and after (1).

**First launch after each rebuild.** These are counted, not discarded: A1, B1, C1 and C2 after builds 1–4.

- From the session folder to the first log line takes 1.44–1.94 s, against 0.48–1.07 s otherwise.
- Guest frame 1000 arrives at 5.5–6.1 s, against 5.4–5.5 s.
- So a first launch costs about +1 s to gameplay, as before (one-time verification of the new binary).
- In-game FPS shows no first-launch penalty: B1 at 161.3 is the best uncapped run.

## Step 1 — CR-03 exporters: the cost was synchronisation, not rasterization

**What VS 695413A9 is.** It writes `oPos = sgt(-|r0.x|, 0) = (0,0,0,0)`, so its points are degenerate and
produce no fragments. Rasterizing them therefore cost nothing. Earlier evidence agrees:

- Mode 2 (plain points) cost the same as mode 1.
- `metal_fable_terrain_uniform_tessellation=false` changed nothing.
- Turning readback and preserve off changed nothing.

**What the cost really was.** The 39 exporters per frame write 1 KB-spaced ranges. The 16-entry
pending-shared-memory-write list overflows and collapses to the whole 512 MB buffer. This happens about
115–133 times per frame; the list is rebuilt and re-collapsed all the time, also by resolves and compute
writes. From then on every draw that reads shared memory overlaps "a pending write":

- in the same encoder it gets a `memoryBarrier`;
- in a later encoder it gets a `waitForFence`.

That came to 292 barriers and 1,148 fence waits per frame. In the main 4x MSAA pass, vertex time was
1.84 ms per frame.

**Fix 1: `FABLE2_RENDER_BARRIER_DEDUP`** (`metal_command_processor.cc`, `EncodeSharedMemoryRenderReadDependencies`).

- A barrier orders every earlier command's producer stages before every later command's consumer stages.
- A fence wait covers every fence update encoded before it. Updates happen only at encoder end or in
  compute encoders, so none can happen while the render encoder is open.
- So the encoder records the producer-write generation and the stages already covered. A later barrier or
  wait with no new producer write and no wider stages is skipped.
- Waited ranges are still retired exactly as before.

The effect, at about 90% of the gain:

| Run | Uncapped FPS |
|---|---|
| Without the fix (B3) | 136.6 |
| With it (B1) | 161.3 |

- Skipped per frame: about 260 barriers and about 1,050 fence waits.
- Main 4x pass vertex time: 1.84 → 0.53 ms.
- GPU mean 7.46 → 6.21 ms; heavy frames 13.9 → 10.6 ms. That equals the old "CR-03 off" numbers (6.19 / 10.3), but the tessellation factors stay correct.

**Fix 2: export-only exporters (`FABLE2_MEMEXPORT_GEOM_FALLBACK` mode 1, the new default).**

- Every VS 695413A9 draw runs as a plain vertex pipeline: no fragment function, no geometry emulation,
  no depth-only fallback PS and no depth-attachment requirement. This includes the half of them that have
  no clip plane, which previously also went through mesh-shader emulation.
- Effect with dedup on: B4 (geometry path) 157.6 FPS and 6.35 ms, against B1 161.3 FPS and 6.21 ms. The
  4x pass vertex time fell 0.61 → 0.53 ms.
- Small, but it removes 39 mesh-pipeline draws per frame.
- **Not possible: `rasterizationEnabled = NO`.** Metal rejects it with "RasterizationEnabled is false but
  the vertex shader's return type is not void", and Metal Shader Converter vertex functions are not void.
  Build 1 (run A1) tried it and fell back to the old path.

**Dropped: pending-write coalescing** (`FABLE2_SHMEM_WRITE_COALESCE`, build 2 only).

- It merged adjacent pending ranges before the collapse.
- It did not change the collapse count (115/frame), because other writers cause the collapses.
- B3 against B2 was 136.6 against 134.0 FPS, which is noise. It was removed.

## Step 2 — per-pass GPU timing and what fills the frame

**The instrumentation** (`metal_pass_trace.cc/.h`, trace-only).

- Every render, compute and blit encoder gets `MTLCounterSampleBuffer` stage-boundary timestamps.
  These are pass-descriptor sample attachments: vertex and fragment start/end for render passes, start/end
  for compute and blit.
- Each pass is keyed by attachment format, size, sample count and load/store actions, with its draw count,
  barriers, fence waits and end reason.
- Rows go to `passes-*.csv` beside the frame CSV. `passstats.py` aggregates them.
- Sample buffers are pooled (24 × 4,096 samples). With tracing off, encoders are created exactly as before.
- Note: `sampleTimestamps` CPU and GPU values are both nanoseconds on Apple silicon (measured).

**Top passes, final build, uncapped (C2, per frame).** About 232 encoders: 99 render, 72 compute, 61 blit.
The sum of encoder spans is 7.3 ms against 6.3 ms of GPU time, because passes overlap.

| ms/frame | Passes per frame | What |
|---|---|---|
| 2.18 | 11.5 | Main scene pass, RGBA16F 2x MSAA + D32S8 (EDRAM-shaped 1120×1176), about 62 draws each; vertex 0.46, fragment 1.71 |
| 1.04 | 7.8 | 4x MSAA RGBA8 + D32S8 pass (280×2344×4), about 71 draws each; the exporters and terrain live here |
| 0.47 | 31.6 | Texture upload compute (`tex_upload_compute`) |
| 0.28 | 28.4 | Shared-memory upload blits |
| 0.24 | 26.5 | Direct host resolve compute |
| 0.85 total | about 15 | EDRAM ownership transfers (`rt_transfer`) |
| about 2 | about 120 | Small resolve/transfer/RT passes |

**Why passes end, per frame:**

| Reason | Ends per frame |
|---|---|
| `resolve_needs_boundary` (guest resolves) | 30.4 |
| `rt_update_descriptor_dirty` | 18.8 |
| `shared_memory_upload_before_draw_pass` | 18.3 |
| `transfer_request` | 5.7 |
| `texture_upload_before_draw_pass` | 5.4 |

The main 2x pass alone is broken about 8 times per frame by uploads: 4.5 shared-memory and 3.4 texture.

**Cost of a main-pass break.** A regression over 4,000 frames gives
`span ≈ 118 µs × passes + 2.1 µs × draws` for the main 2x pass. Each extra pass costs a load and a store
of the 2x RGBA16F + D32S8 target, so the upload breaks cost about 0.95 ms per frame here. The 4x pass
costs about 3 µs per extra pass.

This bandwidth cost is the largest remaining GPU lever. It is not cheap: a staged upload must stay
ordered after the earlier draws in the same command buffer that read the old data (WAR), which is why it
ends the pass. Possible approaches:

- Classify staged uploads by whether their pages were last read by the current submission or only by
  earlier, already committed ones. Encode the second kind into a small command buffer committed ahead of
  the current one. The shared buffer uses tracked hazards, so Metal orders it after the earlier readers.
- Or upload the data inside the pass with a vertex-only copy draw. That needs a `void` vertex function
  plus a fragment→vertex barrier, which on TBDR is a pass split anyway.

**Done 2026-10-08 (step 7, `FABLE2_UPLOAD_PRECOMMIT`).** The first option was implemented. The counter showed that
5.47 of the 5.49 upload breaks per frame were avoidable, and they are now gone.

**Checked, not a lever:**

- Terrain tessellation. `--metal_fable_terrain_uniform_tessellation=false` (G1) gave 162.2 FPS and
  6.14 / 10.34 ms, against 158.6 FPS and 6.29 / 10.86 ms (C2). About 0.15 ms mean, and adaptive factors
  bring back the terrain cracks that the uniform clamp fixes.
- No pass with a redundant clear was found.
- No load/store action was found that is safely DontCare. Every `L/S` belongs to an EDRAM-backed target
  that a later pass or resolve reads.

## Step 3 — the two spin-waits

**GameThread (`FABLE2_GAME_TICK_SLEEP`; `src/core/fable2_spin_wait.h`, called from `src/core/hotfunc/frame/ProcessGameFrame_82276C30.cpp`).**

- The ProcessGameFrame loop is a guest-timebase tick wait at 30 Hz (rate f25 = 30, divisor f13 = 50 MHz).
  It runs game work when `r30 = r29 − r28 > 0`, and otherwise calls `YieldAndCheckThreshold` and loops.
- In the yield branch, the time to the next tick is exact from the loop's own registers:
  `(r28 + 1 − f6) / f25 × f13` guest ticks, converted to host time with `guest_tick_frequency` and
  the guest speed (`fable2::guestspeed::Get()`, the turbo scale; since the turbo wait fix the SDK's
  `guest_time_scalar` holds 1/scale, see `docs/turbo.md`), so turbo is handled.
- The thread sleeps until 1 ms before that time, at most 8 ms per sleep. Then the unchanged guest loop
  yields and re-checks as before.
- **macOS timer leeway.** `sleep_for`, `mach_wait_until` and condition-variable timeouts all wake about
  50% of the duration late on this machine (scratch test): a 5 ms sleep returned 2.5 ms late and a 20 ms
  one 10 ms late, at every QoS.
  - Build 3 used `sleep_for`: 750 of 859 sleeps per 10 s woke after the tick (up to 4 ms).
  - The fix is a one-shot kqueue timer with `NOTE_CRITICAL` (p50 overshoot 7–23 µs, rare outliers about 1–2 ms).
  - Build 4: 0 of about 1,200 sleeps per 10 s woke after the tick; maximum overshoot about 0.8 ms.
- **Effect:** GameThread CPU 5.9 → 0.8 ms per frame uncapped (94% → 12%), and 16.0 → 2.0 ms at 60 FPS.

**3D Engine (`FABLE2_RING_WAIT`).**

- New override `src/core/hotfunc/gpu/RingReadPointerPoll_82B9BF90.cpp` wraps the read-pointer poll. It
  waits only when called from the D3D ring loop 82242628 (`lr == 822426E8`). Other callers poll other
  conditions and are never delayed.
- After 30 µs without read-pointer progress, it blocks on `fable2_ring_progress_wait`, exported by the
  Metal plugin and found with `dlsym`.
- The plugin signals in `MetalCommandProcessor::OnPrimaryBufferEnd`. The SDK stores the read-pointer
  writeback immediately after that returns.
- Race handling: the event precedes the store, so the waiter spins up to 200 µs after a wake, or after
  seeing a new event, for the store. A 2 ms timeout is the safety net.
- **Effect:** 3D Engine CPU 6.2 → 2.6 ms per frame uncapped (100% → 41%), and 16.6 → 3.6 ms at 60 FPS.
  Its real work (lr sampler) is unchanged at 2.05 ms.

Pacing at 60 FPS: p99 18.37 ms with all on, against 18.52 ms off, with 0 gaps over 33 ms in both.
Uncapped FPS is unchanged within noise.

## Step 4 — GPU Commands QoS (`FABLE2_GPU_CMD_QOS`)

- `MetalCommandProcessor::SetupContext`, which runs on the GPU Commands worker, requests
  `QOS_CLASS_USER_INTERACTIVE`. The log shows `QoS 0x15 -> user-interactive: ok`.
- No regression: C4 (off) gave 156.0 FPS against 158.6 with it on, and GPU Commands active time is the same.
- The point is P-core placement on a 5–6 P-core Mac. That can't be shown on the M5 Max, where nothing contends.

## Step 5 — CR-09 Audio Worker (not fixable from the executable)

> **Done 2026-10-09** with the source-built runtime (phase B4, `docs/sdk-from-source.md`): Audio Worker 71.4% → 0.5% of
> a core. The burn was a `sleep_for(0)` busy loop (remaining time truncated to whole ms inside 1 ms alertable slices),
> not the `try_lock` spin.

**Measured** with `FABLE2_LOADING_TRACE=1` (run D1, 20–21 s window) and the extended lr sampler:

- The Audio Worker uses 74–83 ms of CPU per 100 ms.
- Only 23–30 ms per 100 ms is inside guest `KeWaitForMultipleObjects`. The executable can hook that
  (`__imp__KeWaitForMultipleObjects` in `fable2_loading_pacing.h`), but its 1 ms poll costs only about
  0.5% CPU. A scratch test of a 1 ms `sleep_for` loop gave 9.9 ms of CPU in 2 s.
- 74% of the lr samples show `lr = 0`, which means host code outside the guest callback.

So the burn is in the SDK's own `AudioSystem::WorkerThreadMain` → `WaitAny` → `PosixConditionBase::WaitMultiple`
loop inside the prebuilt `librexruntime.dylib`. The most likely spot is the `try_lock` / `yield; continue`
spin (`threading_posix.cpp` about l.345–371), because its sleep path is nearly free. The executable cannot hook that
loop.

**Proposed `patches/sdk/` change** (not built; the runtime is the prebuilt v0.10.0 release):

1. `src/core/threading_posix.cpp`, about 50–70 lines.
   - Add a process-wide "any condition signalled" generation counter plus a condition variable. Every
     `PosixConditionBase` signal path (event set/pulse, semaphore release, mutant release, timer fire) bumps
     it, and calls `notify_all` only when a multi-wait is registered.
   - In `WaitMultiple`, replace `sleep_for(1 ms)` with a wait on that condition variable until the
     generation changes or the deadline passes.
   - Replace the `try_lock` / `yield` spin with locking in address order, or a bounded spin and then a
     blocking `lock`.
2. `src/audio/audio_system.cpp:166-169`, 3 lines: drop the extra `Sleep(500 ms)` when nothing was pumped,
   since `WaitAny` already waited.

Expected effect: about 0.7 of a core freed, and up to about 1 ms less latency on every guest multi-object
wait. It needs the runtime built from source, which is the same blocker as
`patches/sdk/clock-fractional-ticks.patch`.

## Step 6 — visual trade-offs: none implemented

- **MSAA downgrade.** In the Metal render-target cache the host texture's sample count is the guest's
  EDRAM sample count, and texture dimensions are EDRAM-tile-shaped per sample count (`280×2344×4`,
  `1120×1176×2`). A 4x→2x or 2x→1x host mapping would need a new sample-remap path through the RT keys,
  the transfer, resolve, EDRAM dump/load and depth-store shaders. That is a multi-day, high-risk change,
  not an option toggle. Not attempted.
- **30 FPS** is already in the Dev Launcher (frame-rate picker: "Use game settings" / Auto / 30 / 60 / Uncapped). It doubles
  the budget to 33 ms, which covers every predicted M3 Pro mean and most heavy frames.
- **Terrain tessellation** is not worth a quality option (see step 2).

**"Auto" mode (proposed here; implemented 2026-10-08 after the maintainer approved it).** Only the frame-rate choice has an effect large
enough to matter. `fps_60 = "auto"` / `FABLE2_FPS_60=auto` (`src/core/fable2_fps_auto.cpp`) and the launcher's
"Auto" entry (the default only for new launcher installs) implement the proposal below, logging
`[fps-auto] gpu-core-count=N -> 60|30 FPS (threshold 24)`. **Replaced 2026-10-13** by an estimated-frame-time rule after the M3 Pro benchmark (14 cores: 63 FPS uncapped, 58.5 at the 60 cap), see `docs/resolution-scale.md` §25; `FABLE2_FPS_AUTO_MIN_CORES=24` restores this one. The original proposal:

- Read the GPU core count with IOKit (`AGXAccelerator` `gpu-core-count`; `MTLDevice` does not expose it).
- Choose 60 FPS when there are ≥ 24 GPU cores, and 30 FPS when there are fewer. This would cover the
  M3 Pro 14- and 18-core parts and the base M-series chips.
- It changes the player-facing frame rate, so it needs the maintainer's decision.

## Step 7 — staged-upload breaks of the main pass (`FABLE2_UPLOAD_PRECOMMIT`)

**Restore point.** `../restore-points/2026-10-08-upload-breaks-before.patch`. It is empty because the tree was clean
at 101b5c1. Harness and logs are in `out/validation/upload-breaks-20261008/`: `go.sh` runs `f2diag run sweep` plus
`f2diag gpu`, and `sessions.txt` maps labels to sessions.

### Measurement first (build 1, trace counters only, no behaviour change)

**New trace output** (with `FABLE2_GPU_TIME_TRACE=1`).

- Pass-trace end reasons now carry the upload cause: `upload_pre:*` / `upload_xfer:*` with `war_current`,
  `standalone`, `inflight`, `guard` or `other`, and `transfer:index_copy|rt_transfer`.
- Per-frame `tr_n_upl_*` rows:
  - invalid pages uploaded, by who last read them (`pg_current`, `pg_inflight`, `pg_completed`, `pg_standalone`);
  - pass ends by cause (`brk_*`);
  - pre-commit command buffers and pages.
- New reader: `f2diag gpu <session>` (`tools/f2diag/gpu.py`, which replaces `gpustats.py` / `passstats.py`).
  It reports:
  - GPU ms/frame;
  - GPU busy %, and idle gaps between command buffers;
  - GPU Commands blocked shares;
  - per-pass time;
  - main-pass breaks. A break is a pass that ends and is then resumed with the same key. They are grouped by cause.

**Baseline, Old Tomb sweep, uncapped, 20–48 s (A1, first launch after build 1, counted).**

The main 2x RGBA16F pass had 9.54 breaks per frame:

| Break cause | Breaks per frame | Notes |
|---|---|---|
| Staged upload | 5.49 | 5.47 touch only pages last read by committed, still-running command buffers (avoidable). 0.02 are WAR against the current command buffer. None are standalone. |
| Texture upload compute | 3.89 | |
| Command-buffer end and other | 0.16 | |
| Resolve / EDRAM transfer | 0 | These end the pass for real; it is not resumed. |

- There is no "size" or "ring space" reason in this path. Staging is decided only by who last read the page.
- Uploaded pages per frame:
  - 341 last read by a committed, in-flight command buffer;
  - 1.8 by the current one;
  - 15 by completed ones, which already use the direct-write path.

### Design

`metal_shared_memory.cc` `UploadRanges`, and `metal_command_processor.cc`.

**Routing.**

- An invalid page whose last main-command-buffer reader is committed but not yet complete (`PageAccess::kInflight`)
  is copied in a **pre-commit command buffer**, provided all of these hold:
  - no standalone command buffer is using it;
  - a render pass is open;
  - no prepared draws are queued;
  - no texture-upload work is pending.
- There is one pre-commit command buffer per main command buffer. It is created at the first such copy, collects
  every copy made while the current command buffer is being encoded, and is committed just before the current one
  (`EncodeMainCommandBufferEnd`).
- Nothing in the current command buffer runs before it is committed, so later copies still land before the
  draws that need them.
- Pages that the current command buffer already read (WAR) keep the in-pass blit. So do pages that a standalone
  command buffer is still using.

**Hazards.**

- **WAR.** Each main command buffer signals its submission index on an `MTLEvent` when it ends. The pre-commit
  command buffer waits for the newest committed index before its blit, so the copy starts only after every earlier
  reader has finished.
  - No main command buffer is committed while the current one is open, so that index covers every page it takes.
  - This guarantee is explicit. The in-pass path relies on encoder order inside a command buffer instead.
- **RAW.**
  - The blit updates a fence. The open render encoder waits on it once, for all graphics stages. The pre-commit
    command buffer is committed first, as fences require.
  - After the render encoder ends, the current command buffer waits on the pre-commit `MTLEvent`. This covers
    every later encoder, and also completion: the staging buffers are tagged with the current submission.
  - New main and standalone command buffers wait for pre-commit command buffers that are committed and not yet complete.
- **Guest data.**
  - Pages are validated before the copy, as on the staged path and CR-02.
  - Fix A's memexport carve runs before routing, so preserved ranges are never copied.
  - Fix B's readback is unchanged.
  - No direct-write path was added for in-flight pages, because the GPU may still be reading them.

**Bubble found and fixed during development.** These were real behaviour changes:

| Variant | Uncapped FPS | GPU busy | Idle per frame |
|---|---|---|---|
| One pre-commit command buffer per upload call (21.7/frame), `MTLSharedEvent` | 68.9 | 82% | 2.5 ms |
| One per main command buffer, `MTLSharedEvent` | 116.5 | 89.6% | 0.9 ms |
| Same, WAR wait skipped (diagnostic only, `FABLE2_UPLOAD_PRECOMMIT_DIAG=nowar`, unsafe) | 160.7 | 98.5% | |
| One per main command buffer, GPU-only `MTLEvent` (final) | 161.2 | 98.65% | 0.08 ms |

The cost was the latency of a GPU wait on a shared event (pattern 25 in `docs/bug-patterns.md`), not the copies.

### Before / after (M5 Max, uncapped, 20–48 s, `f2diag gpu`)

| Metric | A1 base (build 1) | D2 switch off (build 4) | D1 on (build 4, first launch) | D3 on |
|---|---|---|---|---|
| FPS in window | 156.1 | 159.2 | 161.2 | 161.4 |
| GPU ms/frame mean / p95 | 6.42 / 10.84 | 6.30 / 11.02 | **6.14 / 10.54** | **6.14 / 10.57** |
| Sum of encoder spans (ms/frame) | 7.53 | 7.49 | 7.17 | 7.07 |
| Encoders per frame | 233 | 232 | 199 | 200 |
| Main 2x pass ms/frame, passes per frame | 2.24, 11.5 | 2.25, 12.1 | 2.10, 8.2 | 2.15, 8.8 |
| Main-pass breaks per frame (staged upload / texture / other) | 9.54 (5.49 / 3.89 / 0.16) | 10.09 (5.82 / 4.06 / 0.21) | **6.18 (0.04 / 5.98 / 0.16)** | 6.84 (0.03 / 6.61 / 0.20) |
| 4x pass ms/frame, passes per frame | 1.11, 8.25 | 1.00, 7.7 | 0.97, 5.3 | 0.98, 5.7 |
| GPU busy %, idle ms/frame, gaps > 100 µs per frame | 99.1, 0.057, 0.22 | 99.1, 0.058, 0.24 | 98.65, 0.084, 0.31 | 98.8, 0.076, 0.28 |
| GPU Commands frame-slot wait (% of wall) | 10.4 | 10.6 | 7.5 | 9.4 |
| Whole-sweep FPS, frame p99 (`result.json` pacing) | 161.3, 11.60 | 161.9, 11.69 | 166.2, 11.38 | 165.5, 11.26 |
| Pre-commit command buffers / pages per frame | — | — | 3.95 / 313 | 4.1 / 318 |

**Reading the table.**

- Staged-upload breaks are gone: 5.5–5.8 → 0.04 per frame.
- About 2 per frame of those draw batches are now ended by the texture-upload compute that follows them instead.
  So net main-pass breaks fall by about 3.4–3.9, not 5.5.
- GPU time is down 0.15–0.28 ms mean and 0.3–0.45 ms p95. Encoder work is down 0.32–0.46 ms per frame.
- **The next lever is the texture-upload breaks:** now 6–6.6 per frame on the main pass.

**60-capped pacing** (two pairs, interleaved):

| Run | p99 | max | Gaps > 33 ms | 1% low |
|---|---|---|---|---|
| On (E1, E4) | 18.40 / 18.48 ms | 18.8 / 19.2 | 0 / 0 | 53.9 / 53.6 |
| Off (E2, E3) | 18.47 / 18.43 ms | 18.8 / 19.0 | 0 / 0 | 53.8 / 53.7 |

- Pacing is unchanged.
- At 60 FPS the GPU runs at a lower clock, and in-flight GPU time per frame reads higher with the switch on: 12.6–13.0
  against 12.1–12.2 ms. Main-pass span is 4.86–4.97 against 4.61–4.66.
  - Cause: the render pass now waits inside its span for the pre-commit copy, which waits for the previous command buffer.
  - It is slack time, not work: pacing is identical.
  - In the GPU-bound regime (uncapped, or an M3 Pro at 60) it is a net gain.

**No visual change.**

- `f2diag images dark|blue` on the eight sweeps of save `hero1-dog-live_2026-10-07_14-45-08`:
  - Blue: 0 flagged in every run.
  - Dark: the default threshold flags 25–59 camera-motion frames per run, the same on and off.
  - Dips above 1% of the grid: 0 in every run except one in D1 (frame 3749). Inspected: it is the dog moving
    (pose change). There are no stray or dark polygons.
  - So: **0 dog-fur flashes**.
- **GPU data check** (`FABLE2_UPLOAD_VERIFY=1`, trace-only, new). After each copy's submission completes, the GPU
  buffer must match guest RAM wherever the guest bytes are unchanged.
  - On (F1): in-pass 3,608 ok and 0 mismatches; pre-commit 16,302 ok and 0 mismatches.
  - Off (F2): 32,547 ok and 0 mismatches.
  - A draw dump can't show this ordering: its GPU hashes are taken on the CPU at encode time, before any GPU copy.

**Area transition** (`f2diag run area ubG1-area`, save `hero1-dog-live_2026-10-07_18-49-47`):

- Level load #2 took 744 ms, and the loading tag cleared at 14.8 s.
- Gameplay ran in Bower Lake to 69.7 s (last capture checked).
- The only dump was `exit`, with 0 AVs.

**All runs.**

- 0 AV lines in all 14 sessions.
- `pgrep -x fable_2` returned 1 before and after each run.
- First launches after builds 1–5 (A1, B1, C1, D1, F1) are counted, not discarded. Build 5 added only the
  trace-only verify check.

## Step 8 — texture-upload breaks of the main pass (`FABLE2_TEXTURE_PRECOMMIT`)

**Restore point.** `../restore-points/2026-10-08-texture-breaks-before.patch`, plus `tools/f2diag/gpu.py` in the
matching folder. It holds the uncommitted step 7 work. Harness and logs are in `out/validation/texture-breaks-20261008/`:

- `go.sh` runs one sweep plus `f2diag gpu`. It is the step 7 script with the folder changed.
- `sum.sh <label>` prints the key lines.
- `sessions.txt` maps labels to sessions.
- `buildN.log` are the builds.

### Measurement first (builds 1–3, trace counters only, no behaviour change)

**New trace output** (with `FABLE2_GPU_TIME_TRACE=1`, read by `f2diag gpu`).

- Per-frame `tr_n_tex_*` rows. Texture-load flushes that ended an open render pass (`brk`), and those where every
  load was eligible for the pre-commit route (`brk_elig`) or blocked (`brk_blk_samp` / `brk_blk_res`). Each texture
  in a flush is counted once:
  - loads by source: CPU snapshot of guest RAM (`cpu`), resident shared memory (`res`), scaled resolve;
  - first load of the texture (`first`);
  - sampled by a draw already encoded in the open command buffer (`samp_cur`);
  - resident source written in the open command buffer (`res_cur`; split later into `res_gpuw` GPU write, `res_copy`
    in-command-buffer staged copy, `res_sa` standalone, `res_inv` invalid, `res_pend` pending-list overlap);
  - KB, ≤ 16 KB / > 256 KB, eligible, loads in flushes without an open pass (`nopass`);
  - pre-commit flushes, loads and fallbacks (`pre_flush`, `pre_ld`, `pre_fallback`).
- Pass-trace end details `texture:eligible|sampled_current|resident_current|fallback_*|verify`. `f2diag gpu` groups them
  as `texture_upload`.

**Baseline** (A2/A3, Old Tomb sweep, uncapped, 20–48 s; first launches after builds 2 and 3, counted):

| | Per frame |
|---|---|
| Texture-load flushes ending an open render pass | 10.1–10.3. Main 2x pass 6.0–6.2, 4x pass 2.9, others about 1.2 |
| Loads in those flushes | 27–28 (about 2.7 per break), 5.9 MB guest data. 17 are ≤ 16 KB, 3.2 are > 256 KB |
| First-use loads | **0.02**. Almost every load is a reload of a texture that a guest write invalidated |
| By source | 19.5 CPU snapshot, 9.0 resident shared memory, 0 scaled |
| Loads in flushes without an open pass (no break) | 29 |
| Main-pass breaks, eligible / sampled by the open command buffer / resident source "touched" | 3.76 / 0.31 / 2.13 (first classification, which used the page *access* record, reads included) |

**What a load is.** Untile/endian/format conversion compute (`texture_load_*`) into a staging buffer, then a
buffer-to-texture blit. Both are encoded in the current command buffer, and the draw needs the texture now, so the
render pass ends.

**Resident sources are not render-to-texture this frame.** With an exact per-page record of GPU *writes* (build 7),
only about 0.8 loads per frame have a source written by the open command buffer (`res_gpuw`), and almost none of
them ended a main pass. The 2.1 "touched" were reads: an earlier texture load of the same pages, or vertex fetch.

A first attempt used the pending-write list (`PendingSharedMemoryWritesOverlapRanges`) and blocked 7.9 of the 8.5
resident loads per frame. That list collapses to the whole buffer after 16 entries, 125 times per frame
(`tr_n_shmem_collapse`). This is pattern 26 in `docs/bug-patterns.md`.

### Design

`metal_texture_cache.cc` (`ClassifyMaterializationPlan`, `SetPrecommitTarget`, the `kPrecommit` mode in
`TryGpuLoadTexture`). `metal_command_processor.cc` (`ClassifyTextureMaterialization`, `Begin/End/AbortTexturePrecommit`,
the flush sites in `FlushPreparedDrawQueue` and `SubmitPreparedDraw`). `metal_shared_memory.cc` (page write records).

**Routing.**

- A draw flush whose loads are **all** eligible, while a render pass is open, encodes them into the step 7 pre-commit
  command buffer. The render pass stays open. That command buffer is committed just before the current one.
- There is still one pre-commit command buffer per main command buffer. The shared-memory pre-commit blit is ended
  first, and a later staged copy opens a new blit encoder in it.
- A load is eligible when all of these hold:
  - **No draw already encoded in the open command buffer samples the texture.** Every cache texture a draw samples
    reaches the encoder through the texture resource set (`ApplyRenderEncoderResourceSet`, once per encoder). Its
    root texture (through views) is recorded per command buffer (`cb_sampled_textures_`).
  - It is not a scaled-resolve texture, and not a texture handed to the presenter (`RequestSwapTexture`).
  - Its source is a **CPU snapshot** (copied at encode time; no GPU source at all), or **resident shared memory**
    whose pages are valid and were not written by the open command buffer and are not in use by a standalone one.
    "Written" covers resolves, memexport and in-command-buffer staged copies (`MarkGpuWrite`).
  - Blit upload mode is on (the default). The non-blit path waits on the CPU.

**Fallback.** If a load turns out not to be eligible at encode time, the route is abandoned for the rest of that
flush (`AbortTexturePrecommit`, end detail `texture:fallback_*`). This happens when it needs a shared-memory upload
first (`PrepareTextureDataLoadRanges`), or when its pages or texture fail the re-check. The render pass then ends
as before. Loads already encoded stay in the pre-commit command buffer; they were eligible.

**Hazards.**

- **WAR, other command buffers.** The pre-commit command buffer waits on the GPU-only `main_cb_event_` for the newest
  committed main command buffer before any work (step 7). So committed readers of the texture, and committed GPU
  writers of the source, have finished. Cache textures live in tracked heaps, so Metal also orders them.
- **WAR, open command buffer.** A texture sampled by an already encoded draw is never overwritten ahead of it (rule
  above). Resident-source loads mark their pages as accessed by the current submission, as the in-command-buffer
  path does. A later re-upload of those pages therefore goes in-pass, after the open command buffer's work, never
  through the CPU direct write or the pre-commit blit.
- **RAW, source.** CPU snapshots are taken at encode time, as before. A resident source must not have a GPU write in
  the open command buffer. Staged pre-commit copies in the same pre-commit command buffer come earlier in encoder
  order on a tracked buffer.
- **RAW, draws.**
  - Pre-commit encoders are fence-chained: each waits on `precommit_fence_` if an earlier one updated it, and updates
    it at its end. So the last update implies all of them.
  - The open render encoder waits on that fence once per encoder (`WaitPrecommitInRenderEncoder`).
  - Later encoders and command buffers wait on the pre-commit `MTLEvent` (step 7: `EncodePrecommitWait` and
    `EncodePrecommitWaitCommitted`). The texture is also `useResource`d by the render encoder (tracked).
- **Latent step 7 hole closed.** Pages copied by an in-command-buffer staged blit are now marked as accessed (and
  written) by the current submission. Before this, a later re-upload of the same page in the same command buffer
  could take the pre-commit route and land *before* the older in-command-buffer copy. The staged-upload break counter
  is unchanged (0.02–0.03 per frame).

**GPU data check** (`FABLE2_TEXTURE_PRECOMMIT_VERIFY=1`, trace-only).

- For each pre-commit load with a resident source, the shared-memory range the compute reads is copied inside the
  pre-commit command buffer, right before the compute (fence-chained).
- The same range is copied again in the current command buffer at the load's original position: after the render
  pass ends at the end of the flush, or at a fallback.
- The two are compared when the command buffer completes (`[texture-precommit-verify] ok N mismatch M`, with page
  write reasons on a mismatch).
- CPU-snapshot loads read the same snapshot on both routes, so they are identical by construction.
- Verify runs end the render pass, so they are not perf runs.

### Before / after (M5 Max, Old Tomb sweep, 20–48 s, `f2diag gpu`)

| Metric | G1 off (build 10) | C2 off (build 7) | **G2 on (build 10)** | D1 on (build 9) | C1 / C3 on (build 7) |
|---|---|---|---|---|---|
| FPS in window | 165.4 | 162.5 | **168.2** | 166.9 | 166.5 / 164.5 |
| GPU ms/frame mean / p95 | 5.99 / 10.44 | 6.10 / 10.47 | **5.88 / 10.29** | 5.94 / 10.23 | 5.95 / 10.22, 6.03 / 10.30 |
| Main 2x pass ms/frame, passes per frame | 2.08, 8.3 | 2.11, 8.4 | **1.89, 3.5** | 1.90, 2.5 | 1.86, 2.8 / 1.98, 3.8 |
| Main-pass breaks per frame | 6.30 | 6.36 | **1.55** | 0.56 | 0.78 / 1.82 |
| … of which texture: eligible / sampled-current / fallback | 5.82 / 0.30 / — | 5.71 / 0.43 / — | — / 0.42 / 0.92 | — / 0.22 / 0.15 | — / 0.39 / 0.20, — / 0.83 / 0.76 |
| Texture flushes ending any pass | 9.97 | 9.95 | 3.33 | 2.33 | 2.41 / 3.51 |
| Pre-commit texture flushes / loads | — | — | 7.9 / 16.2 | 7.8 / 19.5 | 8.2 / 18.5 |
| GPU busy %, idle ms/frame, gaps > 100 µs per frame | 98.76, 0.075, 0.28 | 98.65, 0.083, 0.32 | 98.47, 0.091, 0.37 | 98.71, 0.077, 0.29 | 98.64, 0.082, 0.32 / 98.7, 0.079, 0.31 |
| GPU Commands frame-slot wait (% of wall) | 7.3 | 7.2 | **5.0** | 4.6 | 5.4 / 5.9 |
| Whole-sweep FPS, frame p99, 1% low (`result.json`) | 169.8, 11.27, 86.0 | — | 172.9, 10.98, 88.5 | 174.5, 11.0, 88.4 | — |

**Reading the table.**

- Main-pass breaks fall from about 6.3 to 0.6–1.8 per frame.
  - What is left: texture flushes whose texture was already sampled in the open command buffer (0.2–0.4).
  - Fallbacks: 0.15–0.9, a load that needs a shared-memory upload mid-flush because a CPU-snapshot source became
    partly valid.
  - The unchanged command-buffer end (0.15).
- The main pass costs 0.19 ms less per frame. GPU time is 0.1–0.15 ms lower in mean and 0.15–0.25 ms lower at p95
  than with the switch off on the same build. That is less than 118 µs × 5 breaks, because the open pass now waits
  inside its span for the pre-commit work, as in step 7.
- **Bubble check.** Idle per frame is 0.077–0.091 ms against 0.075–0.083, and gaps over 100 µs are 0.29–0.37 against
  0.28–0.32. That is at most +0.016 ms per frame, inside run-to-run spread and well under the GPU time saved. There is
  no `MTLSharedEvent` (pattern 25): the waits are the step 7 GPU-only event and the fence.
- Frame-slot wait drops from 7.2–7.3% to 4.6–5.9% of wall.

**60-capped pacing** (build 10):

| Run | p99 | max | Gaps > 33 ms | 1% low | GPU ms/frame mean |
|---|---|---|---|---|---|
| On (E1) | 18.49 ms | 18.8 | 0 | 53.7 | 12.11 |
| Off (E3) | 18.26 ms | 18.5 | 0 | 54.5 | 12.46 |

- Pacing is unchanged (the step 7 pairs were 18.40–18.48 and 18.43–18.47).
- E2, the first off run at 60, hung at boot before the title screen (frame 9, no `[title-fast]` / `[boot-save]` lines,
  GameThread in the load path, VFS idle). Nothing in the texture route had run yet: the classifier is passive with
  the switch off, and no render pass with barriers had started. E3 with the same settings was normal. It is counted,
  not discarded, and reported as a separate boot stall.

**Visual and texture correctness.**

- `f2diag images dark|blue` on the sweeps:
  - Blue: 0 flagged in every run.
  - Dark: the default threshold flags camera-motion frames, the same on and off: 52–64 per uncapped run, and 23–25
    at 60 FPS. Dips above 1% of the grid: 0 in every run.
    So **0 dog-fur flashes**.
  - Sweep and area captures were inspected by eye (chest, dog fur, tomb walls, Bower Lake rocks and beetles): no
    wrong, stale or garbage textures.
- **GPU data check:**
  - F2 (60 FPS), F3 (60 FPS) and F4 (uncapped, about 45,000 samples, 34 GB compared): **0 mismatches**.
  - F1 (build 7, the first verify build) reported 4 mismatches: two 256 KB ranges in two consecutive frames.
  - Two changes followed. Memexport writes are now recorded per page even when no write stage is known, which
    previously made no pending entry. And at a fallback, the verify reference is taken before the fallback's upload,
    because the reference had included that upload.
  - Whether F1 was a real stale read or a verify artifact is not certain. All later runs are clean.

**Area transition** (`f2diag run area tbH1-area`, save `hero1-dog-live_2026-10-07_18-49-47`, GPU trace on):

- Level load #2 took 728 ms (step 7: 744). Gameplay ran in Bower Lake ("Bower Lake" banner, Theresa line, beetles)
  to 69.8 s. The only dump was `exit`, with 0 AVs.
- During the arrival (13–21 s): 0.5 first-use loads per frame in pass-ending flushes, 30.6 pre-commit loads per frame,
  main-pass breaks 1.3 per frame. Over the whole run: 452 first-use loads in such flushes and 145k pre-commit loads.
- Captures look the same as the step 7 run, with the same capture-mean trajectory.
- The `loading` tag never returned to 0 in this run (it did at 14.8 s in step 7). The game reached gameplay
  regardless, so this looks like a tag-hook timing difference, not a stall.

**All runs.**

- 0 AV lines in all 21 sessions.
- Build 11 (final) only skips the classifier when there is nothing to decide or count. I1, its first launch
  (on, uncapped), gave 167.0 FPS, GPU 5.94 / 10.24 ms, main-pass breaks 1.25 (fallback 0.57, sampled 0.47),
  busy 98.8%, gaps over 100 µs 0.27 per frame, 0 AVs.
- `f2diag images` reads only sweep results (`frames`). The area run was checked by eye.
- `pgrep -x fable_2` returned 1 before and after every run.
- First launches after builds 1–11 are counted, not discarded: A1, A2, A3, B1, B2, B3, C1, F2, D1, F4, I1.

## Updated M3 Pro prediction

**Measured inputs** (M5 Max, full clock, uncapped, final build; ranges over B1 and C2, CPU from C2/E1):

- GPU per frame: mean 6.2–6.3 ms, p50 5.1–5.2, p95 10.9, heavy-frame mean 10.55–10.6.
  - With step 7 (D1/D3): mean 6.14, p50 4.95–5.08, p95 10.54–10.57. The same build with the switch off (D2):
    6.30 / 5.21 / 11.02.
- GPU Commands: 5.4 ms mean (6.2 at 60), p95 9.6, p99 10.1.
- 3D Engine work: 2.05 (2.5 at 60).
- GameThread CPU: 0.8 (2.0 at 60).

**Same assumptions as the 2026-10-07 doc** (estimates from public Geekbench figures as recalled, about ±25%):

- GPU factor: 2.2–3.3× for the 18-core part, 2.8–4.3× for the 14-core part.
- CPU factor: 1.35–1.45× per thread.
- Memory bandwidth is about 4× lower. The roughly 1 ms per frame of upload-induced load/store traffic
  (step 2) therefore scales nearer 4× than the mean factor, which pushes toward the high end of each range.

| | GPU mean | GPU p50 | GPU heavy-frame mean | 60 FPS verdict |
|---|---|---|---|---|
| 18-core (was: all on) | 13.6–20.8 ms | 11.2–17.2 | 23–35 | **Borderline.** About 48–73 FPS average; light views reach 60, heavy views about 29–43 FPS |
| 14-core | 17.4–27.1 ms | 14.3–22.4 | 30–45 | **Unlikely.** About 37–57 FPS average, heavy views about 22–34 FPS |
| Old prediction, 18-core all on | 16–24 | 12.5–19 | 33–49 | Unlikely (superseded) |
| **18-core, with step 7** | **13.0–20.0 ms** | 10.9–16.7 | 22–33 | **Borderline, slightly better.** About 50–77 FPS average; heavy views about 30–45 FPS |
| **14-core, with step 7** | **16.7–26.0 ms** | 13.9–21.8 | 29–43 | **Unlikely.** About 38–60 FPS average |
| **18-core, with steps 7 + 8** | **12.5–19.4 ms** | 10.5–16.2 | 21–32 | **Borderline, slightly better again.** About 52–80 FPS average; heavy views about 31–47 FPS |
| **14-core, with steps 7 + 8** | **16.1–25.3 ms** | 13.4–21.2 | 28–42 | **Unlikely.** About 40–62 FPS average |

**How step 7 was scaled.**

- On the M5 Max it removes 0.15–0.28 ms of GPU mean and 0.3–0.45 ms of p95.
- About 3.5 load/store round trips of the 2x RGBA16F + D32S8 target are gone per frame. That is bandwidth, so it
  is scaled by about 4×, not by the 2.2–4.3× compute factor:
  - about −0.6 to −1.1 ms on the M3 Pro mean;
  - about −1.2 to −1.8 ms on heavy frames.
- The 2.5–4 ms estimate below assumed all ~8 upload breaks were removed. The texture-upload breaks (now about
  6 per frame) remain, and they are the next lever.

**How step 8 was scaled.**

- On the M5 Max it removes 4.5–5.7 main-pass breaks per frame, 0.19 ms of main-pass time, 0.1–0.15 ms of GPU mean
  and 0.15–0.25 ms of p95.
- The removed work is again load/store traffic of the 2x RGBA16F + D32S8 target, so it is scaled by about 4× for
  bandwidth:
  - about −0.4 to −0.6 ms on the M3 Pro mean;
  - about −0.6 to −1.0 ms on heavy frames.
- This is lower than "118 µs × 5 breaks × 4" would suggest. The measured M5 Max gain is smaller than the break
  regression predicts, because the open pass now waits inside its span for the pre-commit work. Scaling the measured
  gain is the cautious choice.

**CPU on the M3 Pro:**

- GPU Commands about 7.3–9.0 ms mean, p95 13–14, p99 about 14–15.
- 3D Engine work 2.8–3.6 ms.
- GameThread about 1.1–2.9 ms of CPU per frame.
- All fit in 16.7 ms.

**Busy cores at 60 FPS: 1.45 (was 3.08).** GPU Commands is 0.37, GameThread 0.12, 3D Engine 0.22 and
Audio Worker 0.74. A 5- or 6-P-core chip now has room for GPU Commands and the Metal driver threads on
P-cores, and user-interactive QoS asks for exactly that. **The CPU side is no longer the risk; the GPU is.**

**Bottom line.**

- An 18-core M3 Pro is about 60 FPS in light views and 30–45 in heavy ones.
- A 14-core M3 Pro sits below 60.
- The 30 FPS launcher setting covers both.
- The shared-memory half of the upload-induced main-pass breaks is removed (step 7): about −0.6 to −1.1 ms
  mean on an M3 Pro.
- The texture-upload half is removed too (step 8): main-pass breaks are about 0.6–1.8 per frame, from 6.3. That is
  about −0.4 to −0.6 ms mean on an M3 Pro. Together with step 7, the main pass went from about 9.5 breaks per frame
  to about 1.

**Caveats:**

- One scene (Old Tomb) and one 30 s sweep.
- GPU time is time in flight.
- The scaling factors are estimates.

## Files

- **Step 1:**
  - `backends/metal/port/rex/graphics/metal/metal_command_processor.cc`: barrier/fence dedup, export-only
    mode, collapse counters.
  - `metal_command_processor.h`: dedup state.
- **Step 2:**
  - `backends/metal/port/rex/graphics/metal/metal_pass_trace.cc` and `.h` (new).
  - `backends/metal/metal-sources.cmake`.
  - Encoder sites in `metal_command_processor.cc`, `metal_render_target_cache.cc`,
    `metal_texture_cache.cc` and `metal_direct_host_resolve.cc`.
  - The trace sampler now also samples the Audio Worker lr (`metal_command_processor.cc`).
- **Step 3:**
  - `src/core/fable2_spin_wait.h` (new).
  - `src/core/hotfunc/gpu/RingReadPointerPoll_82B9BF90.cpp` (new).
  - `src/core/hotfunc/frame/ProcessGameFrame_82276C30.cpp`.
  - `metal_command_processor.cc`: ring progress event exports and the notify in `OnPrimaryBufferEnd`.
- **Step 4:** `metal_command_processor.cc` (`SetupContext`).
- **Step 7:**
  - `backends/metal/port/rex/graphics/metal/metal_shared_memory.cc` and `.h`: page classification, pre-commit
    route, trace counters, `FABLE2_UPLOAD_VERIFY`.
  - `metal_command_processor.cc` and `.h`:
    - pre-commit command buffer, events and fence;
    - main command buffer end signal;
    - break-cause details and counters.
  - `metal_texture_cache.h`: `HasPendingUploadWork`.
  - `tools/f2diag/gpu.py` (new) and `tools/f2diag/__main__.py` (`f2diag gpu`).
- **Step 8:**
  - `backends/metal/port/rex/graphics/metal/metal_texture_cache.cc` and `.h`: plan classification, the pre-commit
    load mode, fence-chained encoders, `FABLE2_TEXTURE_PRECOMMIT_VERIFY`, loaded/presented flags.
  - `metal_command_processor.cc` and `.h`:
    - pre-commit command buffer split into `EnsurePrecommitCommandBuffer` / `EndPrecommitBlit` / `WaitPrecommitInRenderEncoder`;
    - `Begin/End/AbortTexturePrecommit`;
    - per-command-buffer sampled-texture set;
    - per-page GPU write marks for pending writes and memexport;
    - `tr_n_tex_*` rows and `texture:*` end details.
  - `metal_shared_memory.cc` and `.h`:
    - `MarkGpuWrite` / `RangeOpenWriteReasons` (exact per-page write records, including the diagnostic pre-commit copy record);
    - in-command-buffer staged copies marked as accessed and written by the current submission;
    - texture trace counters.
  - `tools/f2diag/gpu.py`: `texture:*` end details belong to the `texture_upload` group.
- **Docs:**
  - This file.
  - `docs/bug-patterns.md` (patterns 18–21).
  - `docs/archive/perf-headroom-20261007.md` (superseded notes).
  - the 2026-10-07 code review (CR-09 note).
