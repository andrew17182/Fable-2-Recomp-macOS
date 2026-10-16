# Area-load fixes — 2026-10-16

Follow-up to `docs/area-load-profile-20261015.md` (the Thag's Cave Cullis Gate → Bower Lake transition, 4.39 s from the
save start to the first frame of the new area). That profile proposed fixes A–F. This round covers:

- **A**: native save-compressor search. Faithful; default on.
- **C**: tracing what ends each hold. No behaviour change.
- **D**: shorter render-object fades, as an opt-in setting.
- **E** is dropped: the loading-frames hold is a readiness wait.
- **F**: one opt-in "Skip cosmetic waits" setting (`FABLE2_FAST_PACING`) for every proven pure-pacing wait.
- **G**: what the loading-frames CPU is (the game's own zlib inflate; the entities wait is frame-paced streaming), a
  native inflate_fast (exact, default on, about 0.1 s per area change), the cold-I/O cost and the remaining candidates.

All runs: agent slot build of `perf-staging` 20637bc plus this branch, M5 Max, at the launcher defaults of a new
install (`out/validation/area-load-20261015/env-defaults.txt`), with no other game running.
Evidence is in `out/validation/load-fixes-20261016/` (KEEP).

## A. Native save-compressor search (`FABLE2_NATIVE_SAVE_COMPRESS`, default on)

### What the function is

The game compresses its saves with its own copy of **zlib 1.2.3**.

- The `deflate_state` layout matches: `w_size` +44, `window` +56, `prev` +64, `strstart` +108, `match_start` +112,
  `lookahead` +116, `prev_length` +120, `max_chain_length` +124, `good_match` +140, `nice_match` +144.
- The callers `sub_82B52D40` / `sub_82B531B8` (deflate_slow / deflate_fast) dispatch on `strategy` 2 and 3 exactly
  like 1.2.3, where `longest_match_fast` handles RLE.
- The search is `sub_82B52630`, zlib's `longest_match`. That is the 80% of save CPU found in the profile.
- Captured settings (`params.txt`): **level 9**, `w_bits` 15, `hash_bits` 15, good 32, lazy 258, nice 258,
  **chain 4096**.
- The save wrapper `sub_82B49CB8` compresses one 8 KB buffer per call, plus some single large buffers. Calls continue
  zlib streams (`78 DA` headers, sync flushes). One area save is about 161 calls in 13 streams, about 3.2 MB in total.

### The override

- `src/core/hotfunc/save/native_longest_match.h` holds the native version. Its header comment spells out the exact
  guest semantics: all arithmetic is 32-bit, comparisons are signed or unsigned exactly as the guest code does them,
  index 2 is never compared, and a full compare reads indices 3–258 of both strings.
  - The fast path is used when every byte it can reach is host-contiguous.
    - It checks a candidate with two 16-bit loads: the bytes at `best-1..best`, then the bytes at `0..1`. These are
      the same bytes the guest compares.
    - It measures a match with 8-byte XOR + count-trailing-zeros.
  - Any other state (an address wrap, a range that straddles the 0xE0000000 physical step, odd field values) takes a
    literal transcription of the guest loop over 32-bit guest addresses.
- `src/core/hotfunc/save/SaveCompress_82B52630.cpp` is the strong override. It writes the guest's stack save area
  (`__savegprlr_23` bytes) and returns with the same `r3`, `r12`, `lr` and `r23–r31` as the original. Only the
  volatile `r4–r11` differ; they are dead under the PPC ABI, and both callers read `r3` only.
  - `FABLE2_NATIVE_SAVE_COMPRESS=0` runs the recompiled function (`__imp__sub_82B52630`).
  - `=verify` runs the recompiled function for the game and the native one beside it on every call, and logs
    `[save-compress] verify totals: N calls, M mismatches`.
  - Launcher: Performance → Startup and CPU → **"Native save compressor"** (default on).
- `FABLE2_SAVE_COMPRESS_CAPTURE=1` (or a folder path) makes the wrapper write every call's input and output plus
  `params.txt` to `<session>/test-state/control/save-compress/`. These files are game data: never commit them.
- Every compress call logs `[save-compress] #n in -> out bytes ... ms`.

### Byte-identical evidence

1. **Build-time test** (`tests/save_compress_tests.cpp`, target `fable_2_save_compress_tests`, runs on every build in
   about 7 s).
   - It runs the recompiler's literal output for `sub_82B52630` against the override's exact code path, on a 4 GB fake
     guest memory.
   - After each call it compares `r3`, `r1`, `r2`, `r12`, `r13–r31`, `lr`, the whole 148-byte deflate state, the
     80-byte stack save area, and a periodic window checksum.
   - Inputs come from a zlib 1.2.3 deflate_slow / deflate_fast driver (window sliding, hash chains) at levels 1–9,
     `w_bits` 9–15 and memLevel 1–9.
     - Buffer kinds: random, 2- and 4-symbol, zeros, periodic, runs, text-like, far copies, near-258 matches.
     - Sizes: 0 to 200 000 bytes.
     - Three memory layouts: normal heap, physical range (+0x1000 host offset), and straddling 0xE0000000.
   - A state fuzz covers tame states and wild ones (any field values, addresses wrapping at the top of the address
     space, `chain` 0).
   - `--full` plus captured folders is the evidence run. **0 mismatches** over:
     - 1,817,450 deflate searches;
     - 200,402 fuzz states;
     - 638 captured real save streams, 12.7 MB from four test sessions, each replayed in all three layouts.
2. **In-game verify** (`FABLE2_NATIVE_SAVE_COMPRESS=verify`, real call sequence, real inputs): **0 mismatches** in
   every run.

   | Run | Calls compared | Mismatches |
   |---|---|---|
   | Area save `lf-verify1` (before the 16-bit-load change) | 545,177 | 0 |
   | Area save `lf-verify2` (final code) | 545,244 | 0 |
   | F5 quick save `lf-f5-verify` (final code) | 537,072 | 0 |

   Matching every search result means the compressed output is identical by construction: deflate is deterministic
   given those results.
3. **Arm captures** (`FABLE2_SAVE_COMPRESS_CAPTURE`). Each call's output inflates to exactly that call's input, in
   both arms (161/161 and 158/158 calls).
   - Native area save `lf-nat-cap` against the recompiled `lf-verify1` capture: **every stream with identical input
     has byte-identical output**. That is 10 of 13 streams, 98–126 KB each.
   - F5 native against F5 recompiled: 9 of 10 streams identical.
   - The main 1.2 MB save stream never has identical input between two sessions. The bytes differ from offset 37 on
     (header, play time and world state; about 10.8 K differing bytes in the area runs). So that stream is covered
     by verify mode (2) rather than by comparing arms.
   - Read with `decodecap.py` in the evidence folder: `python3 decodecap.py <armA> [<armB>]`.

### Timings

`f2diag areaload`, 2 runs per arm, default launcher settings, no tracing.

| Arm | Save phase (mount → `XamContentClose`) | Sum of compress calls |
|---|---|---|
| native `lf-nat-t1` / `t2` | **427 / 427 ms** | 325 / 325 ms |
| recompiled `lf-orig-t1` / `t2` | 590 / 594 ms | 488 / 494 ms |

- The autosave is about **0.165 s shorter** (592 → 427 ms, −28%). Every F5 and chapter save gains the same, scaled by the save size.
- Offline bench of the search alone on the 638 captured streams at level 9: 1112 → 613 ms (1.8x).
- The native search is bound by memory latency: each chain step is one dependent `prev[]` load, about 1.6 ns. Level 9
  walks up to 4096 steps per position, so this is close to the floor for an exact search.
- The other ~170 ms is the rest of the recompiled deflate: hashing, lazy matching and Huffman coding. A native deflate
  (the whole of `sub_82B51900` and its callees, byte-identical) is the next candidate if saves matter more.

## C. What ends each hold (`FABLE2_LOAD_HOLD_TRACE=1`, default off; no behaviour change)

`src/core/hotfunc/load/LoadHoldTrace.cpp` adds trace-only wrappers. Each calls `__imp__` unchanged and only reads
guest memory. Wrapped functions:

- the loading-screen controller `sub_822C1FB8`;
- the render-object release `sub_8235DA58`, with a 0.5 ms watcher of obj+160..199 during the wait;
- the two writers of the loading screen's "finished" flag, `sub_8245F260` and `sub_8245E3F8`;
- the area-change state machine `sub_8245D088`;
- its state-7 check `sub_8236BCD8`;
- the world-entry update `sub_82A4F520`.

Log lines start with `[load-hold]`. Runs: `lf-trace3`…`lf-trace7`, with logs in the evidence folder.

### Correction: the constant is at 0x820994C0, not 0x82099440

`lis r11,-32246; lfs f0,-27456(r11)` is `0x820A0000 - 0x6B40 = 0x820994C0`. The 2026-10-15 profile subtracted
wrongly. 0x82099440 holds 0.3, which is unrelated.

The constants involved:

| Address | Value | Role |
|---|---|---|
| `[0x820994C0]` | 1.0 (float) | start alpha written to obj+172 |
| `[0x8209FFD8]` / `[0x83319510]` | 1/15 (double ratio) | per-frame step |
| `[0x820994B4]` | 0 | clamp |
| `[0x82099AE0]` | 0.0001 | end threshold |

### Render-object holds 1 and 2 (`8235DB34 <824EC4A4`, `8235DB34 <822C20AC`): a fixed 15-frame fade, pure pacing

1. `sub_8235DA58` sets obj+172 = 1.0 and obj+176 = 1, then waits (infinite) on the event at obj+180.
2. Each 3D Engine frame, the render object's update `sub_8235ABA0` subtracts 1/15 from obj+172 and clamps it at 0.
3. Once obj+172 is at or below 0.0001, it frees the object's resources (lists +340, +248, +200, +184), clears +176,
   sets +177 and signals +180.

Measured (watcher):

- 15 steps of exactly 0.0667. The step stays the same when the frame interval changes from 16.7 to 33 ms, so the
  hold counts frames, not time.
- At the loading screen's 30 Hz this is 0.44–0.50 s: 506 / 500 / 499 ms after the save, 468 / 469 ms before the
  arrival.
- The boot release (`<826C8898`) is the same fade, about 0.41 s.
- Nothing else is involved:
  - no other code reads obj+172 (the only readers are `sub_8235ABA0` and the release itself);
  - the GameThread just waits;
  - no file reads happen during hold 2 (the profile's "render wait 2" row: 0 reads);
  - the screen stays static ("Saving content…", then the loading screen) for the whole hold.

**Shortening the fade (proposal D) is pure pacing.** The teardown runs the same code, only fewer frames later. See
section D below.

### Loading-frames hold (≈1.69 s after the level-load call): a readiness wait, not a timer

1. The loading screen ends when `ls+34` is set (`ls = [[0x834968A4]+64]`). For the area change that happens in
   `sub_8245E3F8`, the entry of the next game mode. It is called once, from the area-change state machine
   `sub_8245D088` (state at this+44, called every second tick), state 9.
2. In `lf-trace4`…`lf-trace7`, states 5 and 6 take about 0.18 s after the level-load call, state 8 takes 67 ms and
   state 9 one call. **State 7 takes 1.20–1.27 s.**
3. State 7 waits until `sub_8236BCD8` returns 0 (`double +152 > double +176 and byte +600 == 0`) and a request
   counter is 0.
   - The time half passes within about 70 ms; +176 is a short minimum.
   - The wait is **+600**. It mirrors the global busy byte `0x83496C3B`, which `ProcessAndProcessAndProcess1414_821815B0`
     sets while the game mode `[0x83496C74]` is not 0 or 5 (it is 4 for about 2 ticks), or while an entry of the
     world-entry list at `0x8349F498` is unfinished.
4. In every run exactly one entry is unfinished (vtable `820F5F04`). It goes through two steps:
   - **An async request** (handle at +3404, polled by `sub_82AAA818`, states 3 → 4 → 6 → 0): done about 0.45 s after
     the entry's start time.
   - **Entity readiness**: `sub_82A4F520` marks the entry finished only when every entity in its set (+3412)
     reports ready through `sub_82A4F7D0`, or the set is empty, or a limit at +3704 (about 45 s) passes. In all
     runs it finished **"entities ready (before the timeout)"**, 1.05–1.18 s after the start time.

**Do not shorten it (proposal E).** This hold waits until the arrival scene's entities and their data are ready. Ending
it early would start the arrival with entities missing or still streaming in (pop-in). That is consistent with its
fixed length under `sample` in the profile: the wait is mostly the request and the entities, not CPU on the
GameThread. A faithful speed-up would have to make the request or the entity preparation itself faster; the next
step there would be to trace what the request at +3404 is.

## D. Shorter load fades (`FABLE2_LOAD_FADE_FRAMES`, opt-in, default off: a pacing change)

### Implementation

`src/core/hotfunc/load/LoadFade_8235ABA0.cpp` overrides the 3D Engine's per-frame update of the render object,
`sub_8235ABA0`.

- While a release is pending (obj+176 = 1, +177 = 0), it caps the fade value obj+172 at N × the guest's own step
  (1/15) and then runs the original. The teardown and the event therefore follow after N frames instead of 15.
- Values:
  - `FABLE2_LOAD_FADE_FRAMES=0..14` shortens the fade.
  - Unset or 15 is the original: the override is then a plain call of the original.
- Launcher: now part of **"Skip cosmetic waits"** (section F), which makes 1 the default. The earlier separate toggle
  "Shorter load fades" is gone; a saved "on" carries over.
- Affected holds: the release after the save notice (`<824EC4A4`), the release before the arrival (`<822C20AC`), and
  the boot release (`<826C8898`).

### Nothing else waits on obj+172

- The object's class code reads obj+172 only in `sub_8235ABA0`; `sub_8235DA58` and the constructors only write it.
- During the original fade the GameThread is blocked on obj+180. The 3D Engine only steps the value.
- During hold 2 no file reads happen.
- The teardown path (free the lists, clear +176, set +177, signal) is unchanged; it just runs earlier.

### Measured

`f2diag areaload`, traced runs (`FABLE2_LOADING_TRACE=1 FABLE2_GPU_TIME_TRACE=1`), same build, no contention
(`perf_comparable: true`).

| Run | Render wait 1 | Loading frames | Render wait 2 | t0 → arrival |
|---|---|---|---|---|
| `lf-fade15-a` (original) | 504 ms | 1616 ms | 473 ms | 3.936 s |
| `lf-fade1-b` (N = 1) | 31 ms | 1617 ms | 7 ms | **3.038 s** |
| `lf-fade1-a` (N = 1) | 31 ms | n/a | below the 2 ms wait-log threshold | n/a: the reader finds no wait-2 line |

- About **0.9 s faster per area change**: 0.47 s after the save notice and 0.47 s at the arrival.
- The waits logged at boot also shrink: the boot release (`<826C8898`) 434 → 7 ms, and the release of the first
  loading screen (`<822C20AC`) 441 → 7 ms. Loading a save is therefore about 0.86 s faster as well.
- The loading frames are unchanged at 1.62 s: that is the readiness wait from section C.
- Both arms reach the same arrival: the Bower Lake beetle fight with HUD, hero, dog and beetles. The last captures are
  `lf-fade1-b-last.png` and `lf-fade15-a-last.png`.
- 0 AVs and verdict ok in all three runs.
- What changes for the player: "Saving content…" and the loading screen leave sooner. The fade itself was not visible
  (the screen stays static for the whole original hold).

### E is not offered

The loading-frames hold is the readiness wait described in section C. Shortening it would start the arrival before its
entities are ready.

## F. Skip cosmetic waits (`FABLE2_FAST_PACING`, opt-in, default off: a pacing change)

One launcher setting, General → Game → **"Skip cosmetic waits (changes original pacing)"**, default off in every role
(Dev Launcher and player app share `LaunchSettings`). It passes `FABLE2_FAST_PACING=1` (always explicit, `0` when
off). In the game (`src/core/fable2_fast_pacing.h`) the umbrella only changes the **default** of each sub-switch below;
an explicit value of a sub-switch wins (`FABLE2_FAST_PACING=1 FABLE2_LOAD_FADE_FRAMES=15` keeps the original fades).
The launcher no longer passes the sub-switches, so a value in Extra environment overrides the umbrella.

### Inventory

| Wait | Time | Pure pacing? Evidence | Switch, default | Under the umbrella |
|---|---|---|---|---|
| Render-object release fades after the save notice (`<824EC4A4`), before the arrival (`<822C20AC`), at boot (`<826C8898`) | 15 frames, ≈0.44–0.50 s each | Yes: section C. A fixed 15-step frame count; nothing else reads obj+172; no reads during the hold; screen static | `FABLE2_LOAD_FADE_FRAMES`, 15 (original) | **Included**: 1 frame |
| "Saving content" minimum display (`DelayZero` at `824EC440`) | game: max(0, 3.1 s − save time); our default: max(0, 0.5 s − save time) = ≈70 ms now the save takes ≈0.43 s | Yes: `docs/archive/area-load-profile-20261008.md` §2. `XamContentClose` returns before the sleep, so the save is written; a TCR display minimum | `FABLE2_SAVE_NOTICE_MS`, 500 (already shorter than the game's 3100 since 2026-10-08) | **Included**: 0 |
| `Sleep(100)` before the save mount (`824EC274`) | 0.1 s | Yes (same doc §5) | `FABLE2_SAVE_FAST_MOUNT`, already skipped by default | Nothing left to do |
| Front-end audio-bank preload delay (`82354ABC`) | 400 ms per bank in the game | Throttle between bank loads | `FABLE2_AUDIO_PRELOAD_DELAY_MS`, already 10 by default | Excluded: already near zero, and it paces a loader, not the screen |
| SDK overlapped content-call delay | 100 ms per call | Host emulation delay, not the game's | `FABLE2_FAST_CONTENT_IO`, already on | Not a game wait |
| Fast title (title intro ≈2.76 s, outro ≈2.0 s, first title screen only) | ≈4.8 s at boot | The game work behind its events runs sooner; the cut is a visible title animation | `FABLE2_TITLE_FAST`, on whenever skip intro is on | **Excluded**: stays with Skip intro movies (below) |
| Fade to black before the level load (profile 2026-10-15 row 6) | ≈0.21 s | Not proven: the phase does file reads (≈20–40 MB) and worker-0 ops; no fixed count found | — | Excluded |
| Black frame + fade-in after the arrival (row 7) | ≈0.25 s | Not proven a fixed count; it is a visible fade the player sees, after the first arrival frame | — | Excluded |
| Loading-frames hold (≈1.6 s) | — | **No**: a readiness wait (section C, entities ready) | — | Never (section E) |
| Streaming window budget | — | A DVD-era bandwidth throttle, already lifted by default | `FABLE2_STREAM_BUDGET` | Not pacing |

**Fast title stays with Skip intro movies.** It cuts the title screen's own intro and outro animation, which the player
sees, the same kind of thing as the logo movies; it applies only to the first title screen of the process and was only
validated with the logos skipped. Players who choose to watch the intros get the original title too. Test tooling
forces both on, as before.

### Measured

`f2diag run area` (Thag's Cave Cullis Gate → Bower Lake, from the save boot) at the new-install defaults, traced
(`FABLE2_LOADING_TRACE=1 FABLE2_GPU_TIME_TRACE=1`; the b runs also `FABLE2_LOAD_HOLD_TRACE=1`, because a 1-frame release
can fall under the 5 ms wait-log threshold, and `f2diag areaload` now reads those `[load-hold]` lines too). Same build,
no contention (`perf_comparable: true`). Evidence: `out/validation/fast-pacing-20261016/` (KEEP).

| Run | Save | Notice sleep | Render wait 1 | Loading frames | Render wait 2 | **t0 → arrival** | **Log start → boot loading screen released** |
|---|---|---|---|---|---|---|---|
| `fp-off-a` | 430 ms | 70 ms | 503 ms | 1618 ms | 471 ms | **3.914 s** | 4.87 s |
| `fp-off-b` | 425 ms | 75 ms | 497 ms | 1615 ms | 467 ms | **3.948 s** | 4.71 s |
| `fp-on-a` | 431 ms | 0 | 33 ms | n/a (wait 2 unlogged) | < 5 ms | n/a | 3.90 s |
| `fp-on-b` | 427 ms | 0 | 33 ms | 1551 ms | 11 ms | **2.928 s** | 3.82 s |

- **Area change: ≈1.0 s faster** (3.93 → 2.93 s): 0.07 s notice minimum, 0.46 s render wait 1, 0.46 s render wait 2.
- **Save load: ≈0.9 s faster** (4.79 → 3.86 s to the end of the boot loading screen): the boot release 433 → 7 ms and the
  first loading screen's release 443 → 13 ms.
- The loading-frames readiness hold is unchanged (1.55–1.62 s).
- Both arms reach the same arrival: the Bower Lake beetle fight with HUD, hero and dog (`fp-off-b-last.png`,
  `fp-on-b-last.png`). 0 AVs, verdict ok and `pgrep -x fable_2` = 1 after every run.


## G. Where the loading-frames CPU goes, and native decompression (`FABLE2_NATIVE_INFLATE`, default on)

Question: in the 1.6–1.7 s loading-frames hold (section C), state 7 of `sub_8245D088` waits for one world-entry object
(vtable 820F5F04): first an async request (~0.45 s), then until every entity of its set is ready (~0.7 s). What is the
CPU work, and can it be made faster without changing the game's behaviour? Runs: `f2diag run area` at the new-install
launcher defaults (`out/validation/load-cpu-20261016/go.sh`), M5 Max, no other game, traced
(`FABLE2_LOADING_TRACE=1 FABLE2_LOAD_HOLD_TRACE=1`). Windows on the flight recorder's 1 kHz sampler stream with
`winprof.py` (per thread WORK/SPIN/WAIT per time window). Evidence: `out/validation/load-cpu-20261016/` (KEEP: logs,
sampler streams, test logs).

### What the work is

The loader's data is **zlib-compressed and inflated by the game's own zlib 1.2.3**:

| Guest function | zlib 1.2.3 | How it was identified |
|---|---|---|
| `ProcessStructFields_82B59D28` | `inflate_fast` | `state` fields at +40..+88 (wsize, whave, write, window, hold, bits, lencode, distcode, lenbits, distbits), `in = next_in - 1` / `out = next_out - 1` (PPC pre-increment build), mode 11 (TYPE) on end of block and 27 (BAD) with three message pointers |
| `ProcessStructureValidation_82B53A40` | `inflate` | 29-state jump table, gzip magic 0x8B1F, calls the three below |
| `ProcessHistogram_82B597E8` | `inflate_table` | per-length counts (`count[len]++`), MAXBITS 15, LENS/DISTS/CODES with lbase/lext/dbase/dext at 820F9FE0..820FA0A0 |
| `ComputeHash2_82B57438`, `sub_82B57748`/`sub_82B57760`, `ComputeAndCacheFlag_82B538F0` | adler32, crc32, updatewindow | callees of inflate |

Static tables in the image: lenfix 820F92F8, distfix 820F9AF8, order 820F9B78.

Per phase (run `lc-trace1`, sampler ms; ± a few samples):

| Phase | Thread doing the work | What |
|---|---|---|
| Level-load call (656 ms) | VFS Request Finalizer, ~285 ms busy | ~265 ms `inflate_fast` (+14 ms `inflate_table`). The GameThread waits for these reads (~170 ms guest waits at `82A39260`); its own ~420 ms of CPU is spread over the level setup with no single hot leaf |
| Async request, state 3 (~0.2 s) | 3D Engine, called synchronously from `sub_82AAA818` state 3 → `sub_82A50EF0` → … → `82A1D440` → inflate | ~140 of 200 ms `inflate_fast` (~44 MB out). States 4–6 (~0.2 s) wait for the request's reads; the finaliser inflates again there (~35 ms) |
| Entities ready (~0.7 s) | 3D Engine world update `82181270 → sub_82A56428 → sub_82A69728 → sub_83008E98 / sub_82D0F8A8` | **Not CPU-bound**: the 3D Engine is ~40% WORK, ~60% SPIN (its frame wait); VFS idle; only ~40 reads. No inflate |
| Texture Streamer (level-load) | `82B8D010`/`82B8C1C8` under `82B67950` | ~370–570 ms of streaming bookkeeping (bit arrays, per-mip lists; no decompression). Not on the hold's critical path |

**The entity check.** `sub_82A4F520` marks the entry finished when, for every entity of its set (+3412) with
+72 set, a walk of a 4-way world-region tree (`sub_82A4F7D0`, children at +84, flags at +72, bounds test
`sub_82A4F8C8`, 16 layers 448 bytes apart) finds the regions around the entity loaded. So it waits for the world
streaming around the arrival's entities. That streaming advances with the 3D Engine's frames: at 30 FPS (`lc-fps30`)
the entities finish 1.68 s after the entry's start, against 1.11–1.15 s at 60 FPS, while the async request takes about
the same time. The phase is paced by frames, not by CPU. Speeding it up would mean more streaming steps per frame
(a pacing change), not faster code.

### Native inflate_fast (`src/core/hotfunc/load/`)

- `native_inflate_fast.h` is a transcription of the **recompiled guest code**, not of the zlib source:
  - registers are 64-bit values with the recompiler's operations (slw/srw with the 0x20 rule, 64-bit add/subf,
    32-bit compares);
  - every guest load and store is at the same address, in the same order, with the same value;
  - on return r4–r11, r12, lr and r16–r31 hold what the original leaves (r16–r31 reloaded from the save area as
    `__restgprlr_16` does).
- What changes is only how memory is reached:
  - the input bytes and literal stores use one host pointer per region (checked once at entry: no wrap, one side of the
    0xE0000000 physical step), instead of a volatile access with a per-byte offset select;
  - each match copy is one `CopyForward`. It gives the result of the guest's forward byte loop for any overlap: memcpy
    when disjoint, 8-byte chunks when the source is ahead or at least 8 behind, memset for distance 1, otherwise the
    byte loop. Ranges that wrap or straddle 0xE0000000 use the byte loop with per-byte mapping.
- `Eligible()` covers what zlib's `inflate()` guarantees before it calls `inflate_fast`: avail_in ≥ 6, avail_out ≥ 258,
  start ≥ avail_out, plus the region checks. Any other state runs the recompiled function.
- `InflateFast_82B59D28.cpp` overrides `ProcessStructFields_82B59D28`:
  - `FABLE2_NATIVE_INFLATE` unset / 1: native (default);
  - `=0`: the recompiled function;
  - `=verify`: every eligible call runs the native code, records what it wrote and its registers, puts memory back,
    runs the recompiled code for the game and compares the save area, `z_stream`, state header, the whole output buffer
    and all registers. Logs `[inflate] verify totals`.
  - `FABLE2_INFLATE_STATS=1`: per thread, an `[inflate]` line every 0.25 s while it inflates (calls, bytes out, ms).
  - Launcher: Performance → Startup and CPU → **"Native decompression"** (default on).

### Identical output: evidence

1. **Build-time test** (`tests/inflate_fast_tests.cpp`, target `fable_2_inflate_fast_tests`, ~1 s per build).
   - The game's own recompiled `inflate` and its callees are copied out of `generated/default` at build time by
     `tools/extract_guest_funcs.py` into the build folder (game-derived, never committed). The image's zlib tables
     are seeded at their guest addresses; valid streams must decompress to the original data, which checks the
     driver.
   - The guest `inflate` runs in lockstep on two fake 4 GB guest memories: one with the recompiled `inflate_fast`, one
     with the override's native path.
   - After every `inflate()` call it compares the return value, every register after every `inflate_fast` call, the
     `z_stream`, the whole 9520-byte state (with tables), the window, output, input and 8 KB of stack.
   - Streams: host zlib at levels 0–9, five strategies, window bits 9–15, raw / zlib / gzip, random flushes, random
     chunk sizes; random, text, run, short-period (distance 1–9 overlaps), zero, far-copy and mixed data; plus
     bit-flipped copies for the error paths.
   - Layouts: normal heap, physical (≥ 0xE0000000), and input / output / window straddling 0xE0000000.
   - Results:
     - default run: 239 streams, 22,676 calls, **0 mismatches**;
     - `--full`: 5,982 streams, 517,807 `inflate_fast` calls (507,041 native, 10,766 recompiled fallbacks),
       824 MB out, **0 mismatches**.
2. **In-game verify** (`lc-verify1`: boot + Cullis Gate transition, real data): ≥ 8,192 calls and ~170 MB compared,
   **0 mismatches**, 0 ineligible calls.

Matching every call's writes and registers means the decompressed data, and everything the game does with it, is
identical.

### Measured

Offline (`--bench`, same streams through the guest inflate): `inflate_fast` 1.39 → 0.76 ns/byte (1.8×). On game data
the gain is smaller (literal-heavy streams; decode, not copies, dominates):

| Run | Inflate ms in the transition (3D Engine / VFS finaliser) | Boot inflate | Level-load | Loading frames | Entry finished after | t0 → arrival |
|---|---|---|---|---|---|---|
| native `lc-nat1` | 247 (134 / 99), 2.9 ns/B | 178 ms | 617 ms | 1549 ms | 1.113 s | 3.852 s |
| native `lc-nat2` | 233 (128 / 92), 2.8 ns/B | 171 ms | 611 ms | 1549 ms | n/a | 3.834 s |
| recompiled `lc-orig1` | 327 (190 / 135), 4.3 ns/B | 255 ms | 649 ms | 1615 ms | 1.149 s | 3.975 s |
| recompiled `lc-orig2` | 338 (188 / 131), 4.0 ns/B | 248 ms | 648 ms | 1615 ms | n/a | 3.915 s |

- Inflate CPU drops ~30% (≈ 90 ms per area change, ≈ 75 ms at boot).
- **Level-load call −35 ms**. Loading frames −66 ms: the entry finishes ~35 ms sooner, and the state machine (checked
  every second 30 Hz tick) then moves one 67 ms step earlier. Together level-load + loading frames
  2.26 → 2.16 s (**about 0.1 s per area change**).
- The async request ends at 0.28 s instead of 0.45 s after the entry starts, but the entity check (frame-paced, above)
  now ends the wait. It finishes 1.11 vs 1.15 s after the start.
- 0 AVs and verdict ok in every run.

### Cold page cache (I/O)

- The transition reads 1,366 times, 106 MB: 69 MB from `levels.bnk` (474 reads) and 25 MB from `streaming.bnk` (685
  reads, mostly 2 KB); the rest from `Globals/*.bnk` and audio banks. Warm, `NtReadFile` time is about 10 ms in total.
- `cold_io.py` replays the same read sizes serially (as the VFS request manager does) with `F_NOCACHE` (no unified
  buffer cache) at offsets the game had not just read:
  - random offsets: **120 ms** (≈ 880 MB/s);
  - sequential runs: **64 ms** (≈ 1.65 GB/s).
- So a cold transition costs about **+50–110 ms** of I/O, spread over pre-load, level-load (~51 MB) and the request's
  states 4–6 (~25 MB). Boot reads ~150 MB: about +90–170 ms cold.
- No sudo was used, so the first load after a reboot was not measured. The estimate is SSD-bound; the reads are serial,
  so a slower SSD scales it directly.

### Ranked remaining candidates

| # | Candidate | Estimated saving (M5) | Faithful? | Risk |
|---|---|---|---|---|
| 1 | **Native inflate_fast** (done, default on) | about 0.1 s per area change, ≈ 75 ms CPU at boot; more on slower Macs (scales with CPU) | Exact (see the evidence above) | Low; `=0` and verify mode |
| 2 | Native deflate for the save, the rest of `sub_82B51900` (deflate_slow, fill_window, hashing, `_tr_*` Huffman coding) | ≈ 80–100 ms per save. After section A the save is 427 ms with ~325 ms of compress calls; ~200 ms of that is the (native) search, ~120 ms the rest of deflate | Exact if transcribed like A (several functions, state across calls: sync-flushed streams) | Moderate: about 10× the code of A; test pattern from A plus this part's lockstep driver |
| 3 | Entities-ready phase (~0.7–0.8 s) | up to ~0.6 s | **Pacing change**: the world streaming advances per 3D Engine frame. More steps per frame changes the game's streaming cadence | Moderate; needs the per-frame streaming budget located first (`sub_82A69728` subtree) |
| 4 | Rest of the level-load call (GameThread ~420 ms CPU, no single hot leaf) | ≤ 0.1 s, per function | Faithful per override | High effort per ms |
| 5 | Cold I/O: read-ahead of the next level's ranges | ≤ 0.1 s, cold only | Faithful (I/O only) | Low value on SSDs |
| — | `inflate` / `inflate_table` slow paths | < 20 ms | — | Not worth it |
