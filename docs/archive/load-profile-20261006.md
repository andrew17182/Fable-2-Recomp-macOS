# Save-load profile — 2026-10-06

Question: what fills the ~6.2 s between the boot-save Continue ("load started") and controllable gameplay?
This is measurement only. No speed-up is installed. The only lasting source change is trace-only
instrumentation behind `FABLE2_LOADING_TRACE=1`.

**Setup.** All boots used the following:

- Muted background test mode (`fable2_dev_session.launch(..., test_control=True)`), `--target-slot none`.
- Library save `hero1-dog-live_2026-10-04_15-01-20`, skip intro, fast title on, 60 FPS + vsync.
- A frozen warm shader cache: `out/validation/load-profile-20261006/cache-warm`, seeded from
  `shadercache-20261005/cacheF` plus one seeding run. Warm-up reported 293 archive hits, 0 misses, 0 compiles.

Harness: `out/validation/load-profile-20261006/boot.py`. It does the following:

- takes raw captures every 0.25 s;
- runs `ps -M` alongside;
- at 16 s, holds W for 2 s to confirm control;
- closes the game and checks `pgrep`.

All times are seconds from the session origin, taken just before `open`.

- **Gameplay** is the first capture after the loading screen with mean brightness > 40. That is the BowerLake camp scene with the
  "purple highlight" hint, checked visually in `shots/`.
- **Control** was confirmed in every run: the hero moved, with 54–74% of sampled pixels changing over the 2 s W hold.
- There were 0 guest access violations in any run.
- Every session closed normally, and `pgrep -x fable_2` returned 1 after each.

## Baseline timeline (HEAD 1e64fe7 build, and the same build plus the trace-only hooks)

| Run | Trace | Title ready | Load start | Loading screen | Gameplay | Window | Load FPS | Longest swap gap in window |
|---|---|---|---|---|---|---|---|---|
| p1 | off | 3.00 | 3.62 | 4.29–9.60 | 10.38 | 6.76 | 39.9 | 0.219 s |
| p2 | off | 3.06 | 3.71 | 4.28–9.11 | 9.89 | 6.18 | 38.5 | 0.216 s |
| t1 | on | 3.06 | 3.68 | 4.28–9.11 | 9.89 | 6.21 | 38.5 | 0.218 s |
| t2 | on | 2.98 | 3.60 | 4.30–9.12 | 9.87 | 6.26 | 38.6 | 0.217 s |
| t3 | on | 3.05 | 3.67 | 4.31–8.85 | 9.63 | 5.96 | 37.9 | 0.216 s |
| f1 | on (+profiler) | 3.14 | 3.79 | 4.39–9.28 | 10.06 | 6.26 | 37.8 | 0.226 s |
| f2 | off (final build) | 3.18 | 3.84 | 4.40–9.34 | 10.08 | 6.25 | 38.4 | 0.224 s |
| f3 | on (+profiler) | 3.04 | 3.69 | 4.36–9.22 | 10.04 | 6.35 | 39.1 | 0.218 s |

Tracing does not distort the window. Untraced runs gave 6.18, 6.25 and 6.76 s, traced runs 5.96–6.35 s. Capture resolution is 0.25 s. The
first launch after each rebuild (f0, i1, e0, ex0) starts about 0.7 s later (first log line at 1.0 s instead of
0.32 s). That is one-time macOS page-in/verification of the new 117 MB binary, so those runs are excluded above. There is no
frozen frame: the longest gap between guest swaps during the load is 0.22 s.

**Phases** (f1 / f3, from the new level-load and worker-0 trace hooks):

| Phase | f1 | f3 | What happens |
|---|---|---|---|
| Continue → level-load call | 1.78 | 1.87 | Front-end teardown and loading-screen setup; `chaptersave.bin`, `globals.gdb`…; ~0.6 s of GameThread waits that each last ~100–130 ms |
| Level-load call `sub_825F3050` | 1.70 | 1.69 | One synchronous call on GameThread. Save files, then 4 audio bank/.adb pairs through worker 0 (0.40 s), `locomotion.loco` |
| Loading-screen frames after it | 1.63 | 1.63 | ~65 normal game frames at ~40 FPS (`ProcessGameStateUpdate`; morph readback, world init) |
| Final GameThread wait | 0.47 | 0.41 | `8235DB34 <822C20AC <822770B8` waits on a render object (+180); the loading screen closes during it |
| Black + fade-in | 0.68 | 0.75 | Includes the 0.25 s capture step |

## 1. Where the window goes (measured)

The trace-only profiler sums each thread's CPU, its blocked time in guest kernel waits, and NtReadFile time.

Per-thread totals over the load window (run f1, 6.26 s):

| Thread | CPU | Blocked in guest waits/sleeps |
|---|---|---|
| GameThread | 4.45 s | 1.79 s |
| 3D Engine (render) | 5.74 s | 0.20 s |
| Audio Worker (host) | 4.54 s | 1.71 s |
| GPU Commands | 1.11 s | 0 |
| Texture Streamer | 0.74 s | 5.52 s |
| VFS Request M | 0.13 s | 6.3 s; 1,850 NtReadFile, 118.8 MB, **14 ms** total in-call |

- **The critical path is the GameThread, which is mostly CPU.** It runs at about 71% CPU and is blocked about 29% of the time.
- **File I/O is negligible.** Reads come from the page cache, and the VFS threads sit idle.
- **Guest waits on the GameThread total about 1.8 s.** The ≥5 ms waits seen by the existing trace break down as:
  - worker-0 marshalled ops: 0.40 s;
  - the final render-object wait: 0.41–0.47 s;
  - about six ~100–130 ms waits around Continue: about 0.65 s
    (`82CBC59C <824ED048 <824ECE20 <824EA7DC`, and `82B49B74 <82A39260 <82A49380` with a 1000 ms timeout that returns signaled after ~120 ms);
  - one 0.10 s wait at `824EA1A0 <8245EE00`.

  The rest, about 0.3 s, is short waits.
- **The window is not paced by guest time.** Two 2x turbo boots (tb1, tb2) gave windows of 5.86 and 5.95 s, against about 6.25 s.
  - The level-load call shrinks from 1.70 to 1.16 s, and worker-0 ops halve to 0.19 s.
  - The loading-screen frame phase grows from 1.63 to 2.55 s, because turbo holds about 28 FPS. The phase looks bound by a frame count of roughly 65–70 frames, not by time.
  - The ~100 ms waits are unchanged under turbo, so something on the host clock paces them. This is inferred, not proven: they look like
    render-thread syncs at host frame cadence.

  The earlier 2x result applied to the white screen (the boot loader), not to the save load.
- **Sampling (`/usr/bin/sample`, s1 and es2).**
  - **What it costs:** attaching it adds 1.7–4 s to the load, so the samples give proportions only, not timing.
  - **Inside the level-load call:** the GameThread spends ~10–14% in `SpinLock_Acquire_822D7408` →
    `std::recursive_mutex` (the global critical region). That comes from the allocator pool (`Pool_AcquireNode` / `Pool_FreeNode`).
  - **In the frame phase:** ~45% `ProcessGameStateUpdate`, ~24% the guest's own frame-end yield (`YieldAndCheckThreshold_82CBD098`),
    and ~12% global-mutex waits.
  - **On the 3D Engine thread:** it spins in critical-section try-enter plus yield (`822E0508` → `CriticalSection_EnterOrTry`).

### The big host overhead: `getenv` on every critical section (measured)

`src/diagnostics/fable2_checkpoint.h` is compiled into the normal build. It overrides `__imp__RtlEnterCriticalSection`,
`__imp__RtlTryEnterCriticalSection` and `__imp__RtlLeaveCriticalSection` (about 1,000 import sites). Each call runs
`ObserveLock()`, and that calls `Enabled()`, which is `std::getenv("FABLE2_CHECKPOINT_DIR")`. It is never cached.

`getenv` takes the libc environment lock and scans `environ`. The sampled share of time in it:

- **GameThread:** 9% in s1, 27% in s2.
- **3D Engine:** 36%. Its try-enter spin calls `getenv` twice per iteration, which contends with GameThread's calls (`__ulock_wait2`).

**Temporary A/B.** `Enabled()` was changed to read the variable once (`static const bool`). This was built, measured, then **reverted
byte-identical** to `out/validation/load-profile-20261006/fable2_checkpoint.h.orig`, and the app was rebuilt.

| Arm | Runs | Title ready | Load start | Gameplay | Window |
|---|---|---|---|---|---|
| HEAD (excluding first-after-build) | p1 p2 t1–t3 f1–f3 | 2.98–3.18 | 3.60–3.84 | **9.63–10.38** | 5.96–6.76 |
| `getenv` cached | e1 e2 e3 et1 et2 ex1 | 2.67–2.77 | 3.28–3.40 | **8.26–8.59** | 4.95–5.20 |

- **Gameplay comes about 1.5 s sooner.** The title is about 0.35 s sooner, and the load window is about 1.1 s shorter.
- **The profiler confirms where the time comes from.**
  - GameThread CPU over the window drops from 4.45 to 3.21–3.45 s, while its waits stay at about 1.8 s.
  - Continue → level-load drops from 1.8 to 1.1–1.2 s.
  - The level-load call drops from 1.70 to 1.21–1.25 s.
  - The frame phase is unchanged at 1.63 s.
- **Correctness:** the hero moved in all runs, and there were no guest access violations.

## 2. Worker-0 queue during the save load (measured)

Yes, the same path is used. Inside the level-load call the GameThread makes 9 marshalled calls to job worker 0
(queue `83334A18`, worker struct `83334BFC`, 50 ms period):

- 4 × `82BFE30C <82BFC9EC <829F860C <82341AC8 <825F3500`: a bank load posted to worker 0, which then opens the `.bnk` on its own thread;
- 4 × `82BFF4F4 <82BFD5DC <82BFD730 <829F8534 <82341B38 <825F3500`: the matching `.adb` open;
- 1 × `82BFE408 <829F8DB8 <8245F0D0` near the end.

The banks are `atmos_bowerlake`, `region_specific_bowerlake`, `creature_wood_troll` and `hero_hammer`. Each op takes 47–52 ms (the
first takes 3–29 ms, depending on tick phase), and the ops run back to back. The total is **0.37–0.40 s** at 1x (0.19 s under turbo).

The current drain doesn't apply, for two reasons:

- `InBootLoader()` limits it to the boot-loader thread;
- it only recognises return address `82BFF4F4`, not `82BFE30C` or `82BFE408`.

## 3. Other fixed throttles

| Throttle | Where | Cost in the window | Evidence |
|---|---|---|---|
| Worker-0 50 ms tick | above | 0.37–0.40 s | measured |
| ~100 ms waits around Continue | `82CBC59C <824ED048…` ×4–5, `82A39260` ×2 (1000 ms timeout, signaled after ~120 ms) | about 0.6 s | measured; the cause (likely render-thread frame sync) is inferred |
| Final render-object wait | `8235DB34 <822C20AC <822770B8` | 0.41–0.51 s | measured; the same in every arm, cause not identified |
| Loading-screen frame count | ~65 frames after the level-load call, at ~40 FPS | 1.63 s | measured; "frame-count bound" is inferred from the turbo run |
| Audio-bank 400 ms throttle | already reduced to 10 ms (front end only) | ~0 | 29 × 10 ms, before the load |

No other fixed-duration sleep loop runs on the GameThread during the load. The only repeated sleep there is
`XrnmThread`'s 10 ms poll (`824E3434`), which is idle networking and off the critical path.

## 4. Host work

- **Shaders.** Warm-up had 0 misses and no `persistent miss` lines. A vertex shader (`695413A9831D88DA`) is translated during the
  final wait. Its geometry pipeline then fails `materializeAll` and is retried at 1, 2, 4 and 8 s. That is a known, pre-existing issue that
  happens after gameplay starts. GameThread doesn't wait on it.
- **GPU and textures.** GPU Commands uses ~1.1 s CPU and waits on condition variables most of the time. Texture Streamer uses 0.7 s CPU and is
  idle for the rest. No GameThread wait points at them, except possibly the final render-object wait, which is unresolved.
- **Audio.** These are muted test sessions. The host `Audio Worker` thread uses 4.5 s CPU during the window, but `sample` shows it inside
  `KeWaitForMultipleObjects` → `XObject::WaitMultiple` → `sleep_for` polling. That is wasted host CPU, not GameThread blocking.
  This is an inference: unmuted play could differ.

## 5. Candidates, ranked

1. **Cache the `getenv` in `fable2::checkpoint::Enabled()`** (or remove the checkpoint CS hooks from the normal build).
   - **Status: applied and validated** (approved by the maintainer; applied in the working tree by the main agent on 2026-10-06; built
     and validated 00:54–00:57). Results are in "Validation of the applied getenv fix" below.
   - **Savings:** about 1.5 s to gameplay (measured, 6 runs). It probably also frees in-game CPU on the GameThread and 3D Engine thread; that is untested.
   - **Risk:** very low. The variable is only read; nothing in `src/` calls `setenv` on it.
   - **Change:** `inline bool Enabled() { static const bool on = std::getenv("FABLE2_CHECKPOINT_DIR") != nullptr; return on; }`.
     An alternative is to compile the three `__imp__Rtl*CriticalSection` overrides only when the experimental checkpoint is wanted.
   - **Validation:** repeat `boot.py` ×5 per arm, plus a 30 s sweep for frame-time and CPU deltas.
2. **Extend the worker-0 drain to the save load.**
   - **Savings:** ~0.35–0.40 s (measured cost; the savings are an estimate).
   - **Risk:** low to moderate. It is the same mechanism as the boot drain, but it now runs during scene load, where the earlier walk bug hung.
   - **Change:** in `ProcessAndWaitMapStatus_82B49B50`, flag the drain for return addresses `82BFF4F4`, `82BFE30C` and
     `82BFE408` from any thread (drop `InBootLoader()`), or require `825F3500`/`8245F0D0` in the back chain.
   - **Validation:** the `worker0 op` trace should show ~1 ms ops, then about 20 boots per arm checking for AV storms and crashes, and audio present in
     an unmuted manual check.
3. **Turbo or clock scaling only during the load** (b).
   - **Savings:** about 0.3 s measured with full 2x turbo, because the frame phase gets slower at 28 FPS. Scaling the clock only inside
     `sub_825F3050` might save ~0.5 s (estimate: 1.70 → 1.16 s) without that penalty.
   - **Risk:** moderate. Guest-time-dependent simulation during world init, and audio and timers.
   - **Validation:** the `level-load` trace duration, plus a gameplay sanity check (NPC positions, quest timers).
4. **Higher loading-screen frame rate** (new; cause not found). The frame phase is about 65 frames at about 40 FPS. At 60 FPS it would save
   ~0.5 s, if it is really frame-count bound. That needs a look at why loading frames don't reach 60. Guesses: global-mutex contention
   (the getenv fix did not change this phase) or the guest's own budget yield.
5. **Launch to first frame** (d). Nothing obvious. The first log line comes at 0.32 s and the boot drain starts at ~1.75 s. The first launch after
   every rebuild costs +0.7 s, which is one-time macOS binary verification. The getenv fix also brings the title forward by ~0.35 s.
6. **Not worth chasing:** file I/O (14 ms) and shader compiles (0 misses).

The ~100 ms Continue waits (~0.6 s) and the final ~0.45 s render wait are real, but their signal sources are not identified. They
would need an `NtSetEvent` trace on those handles.

## Instrumentation added (trace-only, kept)

`src/core/fable2_loading_pacing.h`, active only with `FABLE2_LOADING_TRACE=1`:

- **`[loading-pacing] prof`.** A host sampler logs every 100 ms for 30 s. For each thread it gives CPU (mach `thread_info`), time in guest
  waits/sleeps of any length (KeWaitForSingleObject, NtWaitForSingleObjectEx, KeWaitForMultipleObjects,
  NtWaitForMultipleObjectsEx and KeDelayExecutionThread imports), and NtReadFile time/count/bytes.
- **`[loading-pacing] worker0 op`.** Every wait for a closure marshalled to worker 0 (return 82BFE000–82BFF600), with duration
  and stack.
- **`[loading-pacing] level-load sub_825F3050 enter/exit`.** With the duration.

The import hooks forward through `dlsym(RTLD_NEXT)`, like the skip-intro `NtCreateFile` hook. With the variable unset, each hook
forwards immediately after one cached-bool check. Untraced timings are unchanged (f2: 6.25 s window).

**Tools** (in `out/validation/load-profile-20261006/`):

- `boot.py`: the harness. `WP_SAMPLE=<start>,<dur>` and `WP_SAMPLE_MS` attach `/usr/bin/sample`.
- `runall.sh`, `lp_analyze.py` (timeline), `phases.py`, `prof.py`, `timeline.py`, `moved.py`, `sampletree.py`, `callers.py`.
- The per-run JSON files, the `*.sample.txt` files, and `trace-instrumentation.patch`.

**Restore point:** `../restore-points/2026-10-06-load-profile-before.patch`. It is empty, because the tree was clean at HEAD.

**Sessions** (all `out/dev-sessions/`):

- Seed: `20261006-002140-c99cdb8c`.
- Baseline:
  - p1 `-002304-70e8a1cc`, t1 `-002326-292fb4e4`, p2 `-002348-0b69df29`, t2 `-002409-25d205b9`, t3 `-002431-e6634d0b`;
  - final build: f0 `-004259-647f7e45`, f1 `-004321-36e45e2a`, f2 `-004343-27063580`, f3 `-004405-3f286e2e`.
- Turbo: tb1 `-002633-5f56d8a0`, tb2 `-004427-3320f2bc`.
- Profiler: i1 `-003000-41d1139f`.
- Sample runs (timing distorted): s1 `-003100-3c0f9ec1`, s2 `-003249-cafcd317`, es1 `-003846-ff364f55`, es2 `-004145-b1b2283d`.
- getenv-cached experiment: e0 `-003616-e93523d5`, e1 `-003638-0afb3f41`, e2 `-003700-2e324d85`, e3 `-003722-165c89fe`,
  et1 `-003744-a875ef6b`, et2 `-003806-35981476`, ex0 `-004044-0055d20d`, ex1 `-004106-b46b6acd`.

## Validation of the applied getenv fix (00:54–00:57)

The build contains maintainer-approved `Enabled()` caching in `src/diagnostics/fable2_checkpoint.h`, applied by the main agent, plus
the trace-only hooks above. Build log: `out/validation/load-profile-20261006/build5-getenv-fix.log`. Harness and conditions are the same as above,
untraced. v0 was the first boot after the rebuild and is excluded.

| Run | Session | Title ready | Load start | Loading screen | Gameplay | Window | Hero moved | Guest AV |
|---|---|---|---|---|---|---|---|---|
| v1 | `20261006-005444-89dd193e` | 2.61 | 3.20 | 3.83–7.36 | 8.14 | 4.95 | 0.58 | 0 |
| v2 | `20261006-005506-5b94c43c` | 2.64 | 3.26 | 3.86–8.18 | 8.94 | 5.68 | 0.55 | 0 |
| v3 | `20261006-005528-eb95c6ca` | 2.63 | 3.25 | 3.82–7.60 | 8.15 | 4.90 | 0.59 | 0 |
| v0 (first after build, excluded) | `20261006-005422-57cd7054` | 3.30 | 3.95 | 4.52–8.32 | 8.83 | 4.88 | 0.54 | 0 |
| HEAD baseline (8 runs) | above | 2.98–3.18 | 3.60–3.84 | | 9.63–10.38 | 5.96–6.76 | | 0 |

- **Compared with HEAD:** gameplay comes 8.14–8.94 s after launch, against 9.63–10.38 s. Title ready is about 0.4 s earlier and the load window is
  about 1.0–1.3 s shorter. v2's loading screen stayed up 0.6 s longer than in v1/v3; the window varies by that much from run to run, as p1 does in the baseline.
- **Gameplay check.** One run of the 30 s right-stick camera sweep (`out/validation/crash-vfs-20261005/perf.py`), session
  `20261006-005555-a680afef`. That harness prints its stats scaled ×1000, so they are corrected here:

  | | Average FPS | p50 / p95 / p99 | Max | Gaps > 33 ms | Sweep CPU |
  |---|---|---|---|---|---|
  | This build | 60.0 | 16.67 / 18.03 / 18.47 ms | 18.74 ms | 0 | 120.0 s |
  | Barriers-on baseline (2026-10-05) | 59.86–60.01 | 16.62–16.72 / 18.04–18.22 / 18.40–18.92 ms | 19.3–21.7 ms | | 120.2–121.4 s |

  The run had no crash, 0 guest AVs and no FATAL lines, and it closed normally. Frame pacing is normal. The game is vsync-capped, so any in-game
  CPU saving does not show up as FPS.
- `pgrep -x fable_2` returned 1 after every run.
