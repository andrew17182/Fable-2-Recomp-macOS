# Area-transition load profile — 2026-10-08

Question: what fills an in-game area transition, i.e. walking through a load zone into a new area?
The profile below was measurement only. Candidates 1–5 were then fixed the same day; see **Results** at the end.

**Repro** (the maintainer's):

- Save library entry `hero1-dog-live_2026-10-07_18-49-47`. The hero stands on the Thag's Cave Cullis Gate.
- Hold A, then go through the Cullis Gate to Bower Lake.
- On the way the game **autosaves** ("Saving content…", the session's `LevelName` is `Caves\BowerLake\ThagsCave`), then shows the
  "Loading Bower Lake" screen. You arrive in a scripted beetle fight, with a letterboxed intro and a line from Theresa.

**Setup:**

- Muted background test mode, `--target-slot none`, 60 FPS + vsync, and the maintainer's launcher defaults (`FABLE2_MEMEXPORT_PRESERVE=1
  FABLE2_MEMEXPORT_READBACK=1 FABLE2_TURBO_NOTICE=overlay FABLE2_UNLOCK_EXTRAS=0`).
- The shader cache is the live cache, copied by `prepare_session`. It already covers Bower Lake.

Harness (in `out/validation/area-load-20261008/`):

- `run.py`: launch, raw capture every 0.25 s, per-thread CPU through libproc every 0.25 s, and A held for 4 s at +10 s.
  - The A hold goes through the **non-blocking `replay-pad.txt` channel**. A test `press e --hold` blocks captures for the
    whole hold (run t1).
  - It also sends forward-stick pulses to time when control returns, optionally an F7 turbo toggle or a `/usr/bin/sample` run, and then
    closes the game.
- Analysis scripts:
  - `logt.py`: the log on the session clock;
  - `frames.py`: the frame CSV binned on the session clock (`metal_frame_logging` is on by default, so every run has it);
  - `phasecpu.py`: CPU and guest waits by phase;
  - `press.py`: a manual press;
  - `sheet.py`: needs PIL, which is not installed.

Sessions (`out/dev-sessions/`):

| Run | Session | Mode |
|---|---|---|
| t1 | `20261008-022120-19ec837e` | `FABLE2_LOADING_TRACE=1 FABLE2_GPU_TIME_TRACE=1 FABLE2_DRAW_SKIP_TRACE=1`; A held through the blocking test press |
| u1 | `20261008-022636-14a64120` | untraced; A held through the pad channel; forward pulses from 23 s |
| s1 | `20261008-022920-2bfd4f7c` | `FABLE2_LOADING_TRACE=1`, turbo 2x (F7 at 9 s), `sample` 16–24 s. **The timing is distorted**; use it only for turbo and call stacks |
| k1 | `20261008-023523-6b0bda76` | untraced, `FABLE2_RING_WAIT=0 FABLE2_GAME_TICK_SLEEP=0` (the log confirms both "off"); the stall A/B |

All four runs closed normally, and `pgrep -x fable_2` returned 1 after each. There were 0 GPU-timeout and 0 `Failed` lines.

**Side effect.** The session watcher copies the autosave into the save library: `hero1-dog-live_2026-10-08_02-21-36` (t1) and `_02-26-51`
(u1). This is the normal behaviour of `fable2_dev_session.py` for test sessions; nothing was written to a live slot.

## Timeline

All times are seconds from the session origin. Capture resolution is 0.25 s; frame times come from the frame CSV.

| Phase | t1 (traced) | u1 (untraced) | Notes |
|---|---|---|---|
| A hold starts | 10.21 | 10.26 | |
| **t0: transition triggers** (swaps drop, save starts) | ≈11.5 | ≈11.6 | About 1.3 s into the hold. The game does not need the full 4 s |
| Fade-out | ≤11.8 | 11.55 → 11.81 (black) | Under 0.3 s |
| Guest `Sleep(100)` before the save mount | 11.66–11.76 | — | `824EC274 <824E8AC4`, guest code |
| `XamContentCreateInternal` (CR-01 fast path) | 11.76, 0.18 ms | — | |
| Save writes and close | ≈11.76–12.49 | — | ≈0.6 s (inferred from the guest's own elapsed-time computation, below) |
| "Saving content" visible | ≈11.9 → 15.2 | 12.07 → 15.16 | ≈3.1 s on screen |
| **Minimum-display sleep** | 12.49–14.86 (**2374 ms**) | — | `824EC440 <824E8C70` |
| Render-object wait after the save | 14.86–15.36 (496 ms) | — | `8235DB34 <824EC4A4` |
| Black | 15.36–15.9 | 15.42–15.68 | |
| Worker-0 ops before the level load | 15.46–15.86 (8 ops, 0.39 s) | — | `82BFE240` ×4, `82BFE408` ×4 |
| Loading screen visible | 15.88 → 24.16 | 15.94 → 29.11 | Static image; captures differ by 0% |
| Level-load call `sub_825F3050` | 15.905–17.071 (**1.17 s**) | — | Includes 8 worker-0 ops (0.39 s) and 63 MB read in 12.7 ms |
| Loading-screen frames | 17.07–17.75 | 17.0–17.75 | |
| **No guest swap at all (stall)** | **17.75–22.87 (5.12 s gap)** | **17.7–27.8 (10.11 s gap)** | See below |
| Loading frames after the stall | 22.87–24.2 | 27.8–29.1 | Morph readbacks; a 456 ms render wait `8235DB34 <822C20AC <822770B8` (23.92–24.38) |
| Black → fade-in | 24.68 | 29.37–29.63 | |
| **First frame of Bower Lake** | **25.19** | **29.89** | HUD (health bar, "New quest available") is already up; a letterboxed arrival intro follows |
| Controllable | moved at 45 (tested late) | hero moved during the pulse at 32.2 (visible at 33.5) | Upper bound: ≤ +2.6 s after the first frame. The letterbox intro is scripted content |
| **t0 → first new-area frame** | **13.7 s** | **18.3 s** | The whole 4.6 s difference is the stall (5 vs 10 s) |

The other phases agree between traced and untraced runs to within about 0.1 s: loading screen at 15.88 vs 15.94, stall start at 17.75 vs 17.75. So tracing does
not distort this transition. The stall length is the only difference, and it is a multiple of about 5 s.

## Answers

### 1. Where the time goes

Per-thread CPU (libproc) and guest waits (`[loading-pacing] prof`), run t1:

| Phase (t1) | Wall | GameThread CPU | 3D Engine CPU | Other |
|---|---|---|---|---|
| Save and fade (10.2–14.3) | 4.1 s | 0.86 | 2.26 | GameThread asleep through the 2.37 s delay |
| Save tail and pre-load (14.3–15.9) | 1.6 s | 0.11 | 1.36 | GameThread wait 3.2 s (the delay plus the render wait) |
| Level-load call (15.9–17.07) | 1.17 s | 0.53 | 0.75 | VFS reads: 209 reads, 63 MB, 12.7 ms in-call |
| **Stall (17.75–22.75)** | 5.0 s | **0.36** | **5.04 (100%)** | Texture Streamer 0.08, VFS idle, GPU Commands 0.55 |
| Post-stall frames (22.75–24.2) | 1.45 s | 0.07 | 0.62 | |

- **Guest sleeps and waits by call site** (GameThread, the critical path):
  - the save minimum-display sleep: 2374 ms;
  - the render-object waits: 496 ms after the save and 456 ms before the fade-in;
  - worker-0 ops: 17 × ≈50 ms = 0.78 s;
  - the guest `Sleep(100)` before the content mount: 100 ms;
  - a 30 ms wait `82A39260` inside the level load.

  No other GameThread wait is ≥ 5 ms. The background `XrnmThread` 10 ms poll and the VFS idle waits are off the critical path.
- **File I/O:** negligible, about 30 ms in total across the transition. Reads come from the page cache.
- **GPU:** idle during the stall (0 GPU callbacks), and 60–130 ms per 0.5 s during loading frames. No `tr_sync_wait` stall.
  The 5 s Metal waits in `metal_command_processor.cc` (`PrepareForWait` and the morph-readback `FlushCommandBufferAndWait`) would log
  "GPU timeout"; none did.
- **Shaders and pipelines:**
  - Before the transition: all 271 shader translations happen in the boot warm-up (0–5 s): 387 archive hits, 0 misses, 0 compiles.
  - During the transition and in Bower Lake (up to 60 s), in t1 and u1: 0 translations, 0 `persistent miss`, 0 archive saves.
  - With `FABLE2_DRAW_SKIP_TRACE=1`, there were 0 pending-pipeline skip events in the whole t1 session.
  - s1 (turbo) saved one 289 KB archive segment at 34.5 s, after arrival. It most likely holds turbo-overlay pipelines; this is not
    verified.
  - So the 2-thread `metal_pipeline_creation_threads` cap is **not on this load's critical path with the maintainer's cache**. A
    cold-cache first visit was not tested.

### 2. Do the save-load patterns apply here?

**a) Job-worker-0 50 ms ops.**

- Yes: there are 17 per transition, totalling **0.78 s** at 1x (t1) and 0.39 s under turbo.
- They are:
  - 4 × `82BFE240 <82BFD49C <823418A8` and 4 × `82BFE408 <82341914`, before the level load;
  - inside `sub_825F3050`, the same 4 bank + 4 `.adb` pairs as the save load (`82BFE30C <…825F3500`, `82BFF4F4 <…825F3500`);
  - 1 × `82BFE408 <829F8DB8 <8245DCFC`, at 23.9.
- All have `bootloader=0`, so today's drain misses every one. Two reasons: `InBootLoader()`, and the return-address test
  only accepts `82BFF4F4`.

**b) Fixed sleeps, throttles and polled waits.**

- Guest `Sleep(100)` before the content mount: 100 ms (`824EC274 <824E8AC4`).
- **The save minimum-display sleep: 2374 ms.**
  - The code at `824EC3xx`–`824EC440` computes `max(0, K − (now − start))` in seconds and multiplies it by a scale. It is a fixed minimum on-screen
    time for the save notice.
  - With ≈0.63 s already elapsed, K ≈ 3.0 s. That K comes from the arithmetic, not from reading the constant; it matches the
    Xbox TCR minimum for a storage-write message.
- The SDK's 100 ms deferred overlapped delay is **not** involved in the autosave.
  - `XamContentClose` and `XamContentFlush` use `CompleteOverlappedImmediate` (`sdk-source/src/kernel/xam/xam_content.cpp:271–286`).
  - `XamContentCreateInternal` is on the CR-01 fast path (0.18 ms).
  - No deferred `xam_ui` call (device selector) appears in this transition; no ~100 ms GameThread wait shows up.
- The audio-bank 400 ms throttle does not fire here (it is front-end only).
- **The stall is a polled wait:**
  - The 3D Engine thread runs at 100% CPU for 5.0 s (t1), 10.1 s (u1) and 5.9 s (s1, distorted by `sample`), with no swap at all.
  - The guest lr sampler for that window shows the 3D Engine in `82CC207C` (inside `DelayMilliseconds`, for requests < 2 ms, so these are not logged)
    at 34.6%, `82CBD0A8` (`YieldAndCheckThreshold`) at 31.6% and `82CBC68C` (the Sleep wrapper) at 9%. That is a Sleep(0)/yield poll loop.
  - The GameThread is idle, and the VFS, texture streamer and GPU are idle.
  - The VFS "Request F" thread sat idle for exactly 5000.76 ms, from 17.907 to 22.908. Its next request arrived as the
    frames resumed.
  - Lengths of 5.0 and 10.1 s fit a 5 s timeout/retry that runs once or twice. That is **inferred; the call site is not identified**.
  - **Not caused by yesterday's hooks.** With `FABLE2_RING_WAIT=0 FABLE2_GAME_TICK_SLEEP=0` (k1, both confirmed "off" in the log), the
    stall is still there at **10.11 s**. The 3D Engine used 9.85 s of CPU in it; the GameThread now spins on its own tick poll at 9.17 s, but does no work.
  - **It predates them** (scan of all 407 pre-2026-10-08 sessions for guest-swap gaps above 4.5 s after 8 s):
    - `20261007-185002-0791b7c3`: The maintainer's own session from this same save, with no test control and before the hooks existed. A 6.09 s gap at 36.2 s,
      right after the ThagsCave autosave (18:50:37). This is the same transition.
    - `20261004-145830-120dae0a`: 5.07 s at 12.7 s.
    - `20261005-152611-82dd3db1`: 5.05 s at 12.8 s. Both boot-loaded Bower Lake saves, so **some save loads into Bower Lake hit it too**.
    - `20261004-144238-e2724467`: 5.12 s at 809 s, during play that went from the slums to Bower Lake.
  - **Not the Metal backend's 5 s waits.** `PrepareForWait` and the morph-readback `FlushCommandBufferAndWait(5 s)` in
    `metal_command_processor.cc` log "GPU timeout" when they expire; none did. `tr_sync_wait_us` is small, and the GPU is idle during the stall.

**c) Guest-time pacing and turbo.**

- Turbo shortens what is paced by the guest timebase: worker-0 ops went from 50 to 25 ms, and the level-load call from 1.17 to 0.90 s.
- **Separate finding: turbo makes guest sleeps and timed waits 2x longer instead of 2x shorter.** It is measured, and the cause was read in the code.
  - Setup check: s1's log shows `[turbo] ON (scale 2.0x, guest scalar 2.00 …)` at 9.14 s. "Actual" is the host `std::chrono::steady_clock`,
    measured around the call in the trace hooks.
  - Same run, before and after turbo:

    | Call | Before turbo (3–9 s) | After turbo (≥ 9.14 s) |
    |---|---|---|
    | `Sleep(10)` (XrnmThread, about 130 per 3 s) | 11.5–11.6 ms | 22.1–26.0 ms |
    | `Sleep(100)` | 101.7 ms | 202.9 ms |
    | VFS 500 ms timeouts that expire (result 0x102) | 500.0 ms (also 500.0 in t1 at 1x) | 1000.0 ms |
    | Save-notice sleep | — | requested 1590 ms, took 3183 ms |

  - **Cause.** In the SDK, `rex::chrono::Clock::ScaleGuestDurationMillis` and `ScaleGuestDurationFileTime` *multiply* the guest duration by
    `guest_time_scalar_` (`sdk-source/src/core/clock.cpp:188–223`). At a scalar of 2 a guest 100 ms becomes 200 ms of host time.
    For the guest clock to run 2x fast it should divide.
  - **Who calls them:**
    - `XThread::Delay` (`xthread.cpp:1014`), which is `KeDelayExecutionThread`;
    - `XObject::Wait`/`WaitMultiple`/`SignalAndWait` timeouts (`xobject.cpp:211/238/271`);
    - `XTimer` periods (`xtimer.cpp:49`);
    - `xam_net` select timeouts.
  - `patches/sdk/clock-fractional-ticks.patch` doesn't touch these functions, and neither do `src/core/fable2_guest_clock.h` or
    `fable2_turbo.h`. Polling on `mftb` is scaled correctly, which is why worker-0 ops halve.
  - **Effect.** Under turbo, everything the guest paces with Sleep or timed waits runs at 1/4 of the intended speed relative to guest time.
    That includes the save notice, `Sleep(0/1)` polls and timer periods. It would explain why turbo helped less than expected in earlier load A/Bs (inferred).
  - **Fix sketch.** Divide by the scalar in those functions. Do it through a tracked SDK patch in `patches/sdk/`, or as an override of `ScaleGuestDurationMillis`/`FileTime`
    if they are exported. Validation: the turbo run's `Sleep(100)` takes about 50 ms and the 500 ms timeouts take 250 ms; at 1x nothing changes.
- The fade-out (< 0.3 s) and fade-in (≈0.5 s) are short and cosmetic. The save-notice minimum (≈2.4 s of pure sleep) is cosmetic guest
  pacing. It is a console certification rule, not loading work.

**d) Shader misses.** None in this repro (see 1). Whether a first visit is expensive with a cold cache is untested; this
cache had Bower Lake already. The archive persists new pipelines (s1 saved one segment).

**e) New findings.**

- The 5/10 s no-swap stall, the largest single cost.
- Turbo's inverted delay/timeout scaling.
- The save-notice minimum sleep.
- The level-load call is shorter here than in the save load. The final render-object wait (`8235DB34 <822C20AC <822770B8`,
  0.46 s) is the same one the save load has.

### 3. Repeat transition

This was not possible. You arrive in a scripted beetle fight, and the return gate is not usable at once (t1 tried walking back). Steady-state vs first-visit
therefore isn't separated in-process. Across processes, every session starts from the same live cache and the same page cache, and the
non-stall phases repeat to within 0.1 s. The stall varies (5 vs 10 s), which points to a race or timeout, not first-visit work.

### Trace window

- **Where the limits are.** In `src/core/fable2_loading_pacing.h`:
  - the profiler: `seconds(30)` at l.73, from its start, ≈2.2 s;
  - delay logging: `at<30.0` at l.159;
  - wait logging: `<30.0` at l.180;
  - worker-0 logging: `<30.0` at l.239, measured from each hook's first call, near boot;
  - line caps: 6000 per hook, which were not hit (2584 delays and 3724 waits).
- **This run fit.** The t1 transition (11.5–25.2) fits; the last prof line is at 32.25.
- **A later transition would be cut off**, for example after playing for a while; the level-load enter/exit lines are capped at 50 calls, not by time.
  A longer window needs those constants raised or turned into an env var (for example `FABLE2_LOADING_TRACE_SECONDS`). Nothing else
  changes. The XrnmThread 10 ms poll and the `82D0135C` waits produce about 200 lines/s, so a longer window also needs those two
  call sites filtered, or the 6000 caps raised.

## 4. Speed-up candidates, ranked

| # | Candidate | Saves (per transition) | Kind | Risk | Evidence |
|---|---|---|---|---|---|
| 1 | **Find and fix the 5 s poll/timeout stall** | **5–10 s** | Real (a missed completion or timeout; nothing is working) | Unknown until the site is found; likely low if it is a lost signal | Measured in all 4 runs, including with ring-wait and tick-sleep off (k1: 10.1 s), and present in the maintainer's 10-07 session (6.1 s) and two 10-04/10-05 Bower Lake boots. The cause is inferred |
| 2 | Shorten the save-notice minimum (`824EC440` sleep) | ≈2.3 s | **Cosmetic** guest pacing (TCR) | Low: the save is already written when the sleep starts. The notice then flashes briefly | Measured: 2374 ms sleep; K≈3 s inferred |
| 3 | Extend the worker-0 drain to these 17 ops | ≈0.75 s | Real pacing artefact | Low to moderate (same mechanism as the boot drain) | Measured: 17 × ≈46–53 ms |
| 4 | Fix turbo's inverted delay/timeout scaling | Turbo only: makes turbo help instead of hurt | Bug fix | Low to moderate (every timed wait under turbo) | Measured (s1): 100→203 ms, 10→22 ms, 500→1000 ms. Cause read in the code: `Clock::ScaleGuestDurationMillis` multiplies by the scalar |
| 5 | Guest `Sleep(100)` before the save mount | 0.1 s | Guest pacing | Low | Measured |
| 6 | The two `8235DB34` render-object waits (0.50 + 0.46 s) | Up to ≈0.9 s | Unknown (likely frame-synchronised) | Unknown | Measured; the signal source is not identified, as in the save-load profile |
| — | Raise `metal_pipeline_creation_threads` | ≈0 here | — | — | 0 compiles and 0 pending skips. Revisit only with a cold cache |
| — | File I/O | ≈0 | — | — | ≈30 ms total |

**What each would need** (proposals only; nothing was changed).

1. **The stall: instrumentation first.** I stopped here, as instructed, because the call site cannot be identified with existing tools. The guest lr sampler gives only the leaf
   return address, and `/usr/bin/sample` in s1 was dominated by post-load work and distorted by attaching. What I would add, trace-only:
   - In `DelayMilliseconds_82CC2028` and `YieldAndCheckThreshold_82CBD098`, when no guest swap has happened for more than 500 ms, log
     `fable2::skipintro::guest_stack()` once per distinct stack (rate-limited) for the 3D Engine thread. That gives the polling loop's
     caller chain.
   - Log `[loading-pacing] wait` entries with timeout ≥ 1000 ms that time out (result 0x102), on any thread, outside the 30 s window. That
     catches a 5000 ms timed wait if one is involved.
   - Optionally, log `NtSetEvent`/`KeSetEvent` on the handle the poll checks once it is known.

   **Can the existing `FABLE2_GPU_TIME_TRACE` lr sampler answer it? No.**
   - It samples only `ctx.lr`, the return address of the thread's most recent `bl`, and dumps a 10 s histogram.
   - In the stall, the top entries are the helpers themselves: `82CC207C` inside `DelayMilliseconds`, `82CBD0A8` in `YieldAndCheckThreshold`, and `82CBC68C`
     in the Sleep wrapper.
   - The loop's own frame shows only when the sample lands between calls: `82C6F880` at 1.0%, and `822E05DC` at 3.7%. `822E05DC` is inside
     `822E0508`, the 3D Engine function that calls `YieldAndCheckThreshold` in the `sample` call graph.
   - The minimal extension: walk 3–4 guest back-chain frames from `ctx.r1` in that sampler, the same walk as `InBootLoader()`. That
     gives caller chains with no new hooks and no log spam. A 1 s dump interval would separate the stall from the frames around it.

   Then reproduce the transition 2–3 times (one launch each; the stall length varies). A candidate site is the generic
   "Sleep(0) until `obj->vfunc+12()` != 0" loop at `82C6F874`, which appears in the 3D Engine lr list (1%). This is unconfirmed. Validation:
   no swap gap above 200 ms in `frames.py`, and t0 → first frame ≈ 8.6 s, in 3+ runs.
2. **Save notice.**
   - Change: an override of the function containing `824EC440` (called from `824E8C70`) that clamps the computed delay. A cleaner
     route is to override the constant K load, with an env var, for example `FABLE2_SAVE_NOTICE_MS`, default ≈500 and `=3000` to restore.
   - Validation: the `[loading-pacing] delay` line at `824EC440` shows the new value; the save is still written (the watcher logs `saved`, and the
     library entry decodes); there is no hang, and the next phase (`8235DB34` wait) is unchanged.
3. **Worker-0 drain.**
   - Change: in `ProcessAndWaitMapStatus_82B49B50`, also flag `DrainPending()` for return addresses `82BFE240`, `82BFE30C` and `82BFE408`
     (plus `82BFF4F4`) when the caller is on the GameThread. Either drop `InBootLoader()`, or require `8233F4B8`/`825F3500`/`829F8DB8` in the back chain.
   - Validation: the `worker0 op` lines drop to about 1 ms; 3–5 transitions with no AV storm or crash; audio present in one unmuted
     check. This is shared with the save-load candidate.
4. **Turbo.** Divide instead of multiply in `Clock::ScaleGuestDurationMillis`/`ScaleGuestDurationFileTime` (`sdk-source/src/core/clock.cpp:188–223`),
   through a tracked SDK patch. Validation: under turbo, `Sleep(100)` takes about 50 ms; at 1x nothing changes.
5. **`Sleep(100)`.** Change: an override at the `824E8AC4` → `824EC274` call path. The saving is small, so it is only worth doing alongside #2.

**Pipeline-thread A/B, if a cold cache is ever tested.**

- Why the obvious routes don't work: the cvar `metal_pipeline_creation_threads` is set to 2 on the **command line** by
  `tools/macos/AppLauncher.m:105`. The command line outranks both `REX_METAL_PIPELINE_CREATION_THREADS` (environment) and `fable_2.toml`
  (`sdk-source/include/rex/cvar.h:144`, `Source` order). So neither `--env` nor editing the session's copied `fable_2.toml` would change it.
- A lean route that needs no tool edits: a harness in `out/validation/` that wraps `subprocess.run` in `fable2_dev_session.launch` to append
  `--metal_pipeline_creation_threads=N` after `--args`. The AppLauncher appends its own argv after its defaults, and the later
  flag should win, since equal-source flags overwrite. That last part is inferred; check that the startup line "spawned N async pipeline creation threads" shows the new value.
- Pair it with `FABLE2_DRAW_SKIP_TRACE=1` and a cold cache copy in place of the session cache, as `boot.py`'s `CRF_CACHE` override does.

## Files

- Harness and data: `out/validation/area-load-20261008/` (`run.py`, `press.py`, `logt.py`, `frames.py`, `phasecpu.py`,
  `sheet.py`, `{t1,u1,s1}.{launch.json,caps.jsonl,cpu.jsonl,events.jsonl}`, `s1.sample.txt`).
- Logs: `out/dev-sessions/<session>/test-state/logs/fable_2.log`, `frames-*.csv` (and `passes-*.csv` for t1).

## Results: fixes for candidates 1–5 (2026-10-08)

All five fixes are on by default, have an env `=0` kill switch (or a value that restores the original), and have no launcher toggle.
Restore point: `../restore-points/2026-10-08-area-load-before.patch` (+ folder for untracked files).

### Before and after

| Phase | Before: t1 (traced) / u1 (untraced) | After: g1 (traced) / v1 (untraced) |
|---|---|---|
| t0 (transition triggers) | 11.5 / 11.6 | 11.55 / 11.6 |
| `Sleep(100)` before the save mount | 100 ms | skipped |
| "Saving content" on screen | ≈3.1 s | ≈0.9–1.0 s (the save itself takes 0.73 s) |
| Minimum-display sleep | 2374 ms | **0 ms** |
| Worker-0 marshalled ops | 17 × ≈50 ms | 17 × 1–2.7 ms |
| Loading screen | 15.88 → 24.16 / 15.94 → 29.11 | 13.4 → 16.0 / 13.24 → 16.0 |
| No-swap stall | **5.12 s / 10.11 s** | **none** (5 transitions: f1, f2, g1, v1, plus tb1 under turbo) |
| First Bower Lake frame | 25.19 / 29.89 | ≈16.4 / ≈16.4 |
| **t0 → first new-area frame** | **13.7 s / 18.3 s** | **≈4.9 s / ≈4.9 s** (with only fix 1: ≈7.9 s in f1 and f2) |

After arrival, v1 runs at 60 FPS (30 swaps per 0.5 s, max gap ≈18 ms). The forward pulses from 20.2 s moved the hero (captures at 19.5 and 24.4 s).
F5 at 32 s wrote a `BowerLake` library entry that decodes fully (187 chunks, 1,521,041 bytes). The transition's autosave decodes too
(149 chunks, 1,209,245 bytes, the same size as the pre-fix autosave). There were 0 AVs in all runs, and `pgrep -x fable_2` returned 1 after each.

Remaining gaps over 200 ms in the transition:

- ≈320–340 ms at the save start, while the GameThread serialises the save. It was already there before: 323 ms in t2.
- ≈200 ms at the level-load call.
- An F5 quick save shows one 456 ms gap.

Candidate 6 (the two `8235DB34` render-object waits) was not touched.

### 1. The stall: a streaming bandwidth throttle, not a lost signal

**How it was located** (trace-only, `FABLE2_LOADING_TRACE=1`):

- `[stack-sample]` (new, `src/core/fable2_stack_sample.h`; also `FABLE2_STACK_SAMPLE=1`) samples caller chains at the entry of the Sleep wrapper,
  `DelayMilliseconds` and `YieldAndCheckThreshold`. It found the 3D Engine for the whole gap at
  `sleep 82C6F880 <82AB7D4C <8221D10C <8221D154 <82B317A4 <82B315B8`.
- `sub_82AB7CD0` stops a stream object (`vfunc+36(3)`) and then waits in `82C6F848`, "`Sleep(0)` until `obj+48 != 0`". The object has vtable `8200E110`
  and is an async file request, built in `ProcessAndConstructWithCsInit_82C6FF50`.
- The `poll-wait`, `long kernel wait` and `dispatcher` traces showed the rest:
  - the "VFS - Request F" completion thread waited exactly 5000.6 ms (or 5000 + 4983 ms), and the result was *signalled*, not a timeout;
  - the dispatcher (`sub_82B44D10`, 500 ms alertable waits) woke every 500 ms;
  - device `40120AA0` had **23 pending requests, 0 in flight, max 1**, for the whole gap.

**Cause.**

- Every issue (`ProcessAndProcessAndProcess2114_82B4ED00`) asks a global limiter: singleton `834C3510`, built in `sub_82B4DF88`. It returns -3 ("busy, keep pending") when either:
  - the bytes issued in a **5.00 s sliding window** of guest time (`+56`, aged in `ProcessAndTimebaseAndProcess2_82B4E508`) exceed **3 MB** (`+64`), or
  - the bytes in flight exceed 15 MB (`+68`).
- On a DVD the level load's reads spread over seconds, so the window drained as they went. Here the level load reads 63 MB in ≈13 ms, the window is full, and
  every streaming request waits until those entries age out. That is 5 s, or 10 s when a second burst refills the window.
- This also explains the older 5 s gaps on save boots into Bower Lake.

**Fix** (`src/core/fable2_loading_pacing.h`, `ProcessAndAllocateAndProcess11_82B4DF08` override; `FABLE2_STREAM_BUDGET=0` restores):

- Raise only the window budget `+64` to `0x7FFFFFFF`. The in-flight cap, the window bookkeeping and every request are unchanged.
- Log: `[stream-budget] on: streaming window budget 3145728 bytes per 5.00 s raised to unlimited`.

**Proof:**

- f1 (traced) and f2 (untraced): no stall, and the largest gap in the loading phase is 200 ms.
- g1 and v1 (all fixes) show the same.
- A boot of the Bower Lake save `hero1-dog-live_2026-10-04_15-00-21` (b1) had no 5 s gap and ran at 60 FPS from 8 s. Earlier sessions of this save showed 5.07 s and 5.05 s gaps.

### 2. Save-notice minimum

- `sub_824EC3B0` sleeps `max(0, K − elapsed)`, with **K = 3.1 s** (the double at `8209E728`), through `DelayZero_82CBC6B0`, return `824EC440`.
- The override shortens the minimum to `FABLE2_SAVE_NOTICE_MS`: default 500, and `=3100` restores the original.
- Log: `[save-notice] minimum 500 ms (game 3100 ms…): elapsed 728 ms, sleep 0 ms instead of 2372 ms`.
- `XamContentClose` (trace line) returns before the sleep, so the save is fully written. The notice stays on screen ≈0.9 s, because the save itself takes 0.73 s.

### 3. Worker-0 drain for save and area loads

- `ProcessAndWaitMapStatus_82B49B50` now flags the drain for the GameThread's waits at `82BFE240`, `82BFE30C`, `82BFE408` and `82BFF4F4`, in addition to the boot loader.
- g1: all 64 worker-0 ops (save boot and transition) took 0.5–2.7 ms, 63.7 ms in total. Before, they took ≈50 ms each.
- `FABLE2_WORKER0_DRAIN=0` limits the drain to the boot loader again. `FABLE2_BOOT_LOADER_KICK=0` turns all of it off.

### 4. Turbo duration scaling

Fixed from the executable (`FABLE2_TURBO_WAIT_FIX=0` restores). Details and validation are in `docs/turbo.md`. Under 2x:

- `Sleep(100)` takes ≈51 ms, `Sleep(10)` ≈5.9 ms and 500 ms timeouts 250.00 ms;
- the guest clock stays at 2.0000;
- `[game-tick-sleep]` has 0 late ticks.

`patches/sdk/clock-fractional-ticks.patch` does not reach the runtime: the build uses the downloaded v0.10.0 `librexruntime.dylib`. No SDK rebuild was needed.

### 5. `Sleep(100)` before the save mount

- The same `DelayZero_82CBC6B0` override passes 0 for the call at `824EC274`.
- The notice is already requested before that sleep, and the 3D Engine draws it on its own thread.
- `FABLE2_SAVE_FAST_MOUNT=0` restores the sleep.

### Sessions (`out/validation/area-load-20261008/`)

| Label | Purpose |
|---|---|
| t2–t6 | Instrumentation (stack sampler, poll-wait, set-event ring, dispatcher, enqueue traces) |
| f1, f2 | Fix 1 only, traced / untraced |
| b1 | Bower Lake save boot with fix 1 |
| g1 | All fixes, traced |
| tb1, tb2 | Turbo 2x: traced, then with `FABLE2_GPU_TIME_TRACE` for the tick-sleep stats |
| v1 | Final validation: untraced, movement pulses, F5 |

`run.py` now takes `AL_SAVE=<library entry>`. The new trace lines are all trace-only:

- `[stack-sample]`;
- `poll-wait`, `poll-wait end-150ms`;
- `long kernel wait`, `long wait`;
- `dispatcher dev=`, `stall completion`, `stall enqueue`;
- `XamContentClose`.

The worker-0 trace window is now 60 s.
