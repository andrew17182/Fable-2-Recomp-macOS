# Real occlusion queries on Metal (2026-10-14)

Closes CR-04 (the 2026-10-07 code review) and bug pattern 89. Switch: `FABLE2_OCCLUSION_REAL`, **default on**
(`=0` brings back the SDK's fake "1000 samples passed"). Dev Launcher: Performance → Render targets →
"Real occlusion queries". Evidence: `out/validation/occlusion-20261014/` (KEEP).

## Results

Fable II culls objects with about 300 occlusion queries per frame at Bower Lake. Before this change the Metal backend
answered every query with a fake count, so nothing was ever culled. With real answers:

| Scene (headings8, pinned cache, 2 runs per arm, off → on) | FPS | GPU ms/frame | GPU Commands CPU ms/frame | Whole-process CPU ms/frame |
|---|---|---|---|---|
| Bower Lake 2× | 127.6 → 132.3 (+3.7%) | 7.78 → 7.58 | 6.99 → 6.77 | 16.5 → 15.8 |
| Bower Lake 1× | 154.1 → 169.0 (+9.7%) | 5.28 → 5.10 | 6.43 → 5.86 | 14.8 → 13.3 |
| Bowerstone town 2× | 99.8 → 108.4 (+8.6%) | 8.46 → 8.53 | 9.97 → 9.14 | 19.7 → 18.5 |
| Old Tomb 2× | 167.2 → 166.7 (neutral) | 5.87 → 5.88 | 4.45 → 4.61 | 10.7 → 11.0 |

- The share of queries that answer 0 is 35% at Bower 2×, 53% at Bower 1×, 23% in town and 47% in Old Tomb.
  Old Tomb issues only about 7 queries per frame.
- The fake upper bound measured in the workaround audit (every query answers 0, −1.6 ms GPU at Bower 2×) was never
  reachable. Most boxes the game tests really are visible.
- The town is CP-bound, so the saving shows up as GPU Commands CPU rather than GPU time.

**M3 Pro estimate.**
- Bower 2× is GPU-bound there: expect about the GPU saving, +4–5%.
- In CP-bound scenes (town, Bower 1×), the 6–9% GPU Commands CPU saving applies: about +8–10%.
- Base M1 laptops gain mostly from the lower whole-process CPU (−0.7 to −1.5 ms/frame).

**Parity** (frame-matched at `load+720`; `load+240` was too early with two loads in parallel):
- Within noise: Bower 2× and 1×, and Old Tomb 2×.
- Bower 2× frame-matched spin5 (camera turns): medians are equal (0.363 vs 0.364 floor). The one hot block is the
  animated campfire flame, which differs between runs of the same arm too.
- Town 2×/1× and Old Tomb 1× were flagged, but every hot block traced to something that varies between runs of the
  same arm:
  - Old Tomb 1×: a tutorial hint banner whose text changes at a different game time.
  - Town: moving light sprites.
- No missing or popping geometry in any crop.

**Flares are now faithful.** The flare and glow path (`sub_821D8310`) sets brightness from the visible fraction:
samples / (π r² k). With the fake count of 1000 every glow drew at full strength, even when partly hidden. Real counts
dim partly occluded glows, as on the 360. One non-matched Bower pair showed a slightly dimmer campfire glow.

## How the game uses queries (from the recompiled D3D code)

- **Issue(END)** (`sub_822197xx`) stores the sentinel `0xFFFFFEED` in END `ZPass_A`/`ZPass_B`, either by the CPU or
  by MEM_WRITE packets. `sub_82219418` then sets `RB_SAMPLE_COUNT_ADDR` and sends `EVENT_WRITE_ZPD` (initiator 21).
  The BEGIN record is END + 0x20.
- **Predicated tiling.** Each tile replay gets its own record pair through a bin-select-predicated SET_CONSTANT. The
  CP honours predication, so every replay counts into its own record.
- **GetData** (`sub_8220BE78`):
  - It sums END.ZPass(A+B) − BEGIN.ZPass(A+B) over all of a query's records, so the game itself sums the tiles.
  - It checks the sentinel only in the last record.
  - While the sentinel is there it returns `S_FALSE` with `*pData = 1` ("visible"). It also kicks the ring if the
    query is still in the unsubmitted part of the command buffer.
- **Object culling** (`sub_8220BCF8` / `sub_82B26AE0` → `sub_8220BDD8`) **spins on GetData until S_OK**. The game
  waits for the real result; it never guesses.
- **Flares** (`sub_821D8310`, `~0x82279900`) call GetData once. On `S_FALSE` they keep last frame's brightness.
- `sub_82B84060` also spins. It is a flush/shutdown path.

## Design

All in `backends/metal/port/rex/graphics/metal/metal_zpd_real.cc`; hooks are in `metal_command_processor.cc`.

**1. Visibility buffer.** One shared visibility-result buffer with 32768 slots of 8 bytes (the XeniOS-derived
`MetalZPDVisibilityPool`, unused before). It is attached to every render pass descriptor with
`VisibilityResultTypeAccumulate`; a slot is zeroed when it is acquired.

**2. BEGIN.**
- First, flush the prepared-draw queue (reason `kQuery`). The queue refuses draws while a query is open
  (`kZPDActive`), so no query draw is deferred past its END.
- Zero the BEGIN record, as the SDK's fake path does.
- Open a report. A per-record sequence number makes any older in-flight result for that record stale.

**3. Draws inside a query.** Each guest draw gets its own slot:
- `setVisibilityResultMode(Counting, slot)` just before `DispatchDraw`, and `Disabled` just after it.
- Internal draws (transfers, clears) in the same encoder are therefore never counted.
- The segment records the pass's resolution scale, which is 1 for native-scale passes.

**4. END.** Leave the sentinel in place, so the guest sees "not ready" exactly as on Xenos.

**5. Completion handler** (each command buffer, under `zpd_mutex_`):
- Sum the slots.
- Normalise each one: ⌈raw / (sx·sy)⌉, so any covered host sample stays ≥ 1.
- When a report has ended and all its segments are in, write the END record: `Total_A = ZPass_A = n`, other fields 0.
  The `ZPass_A/B` pair goes last as one 64-bit release store.
- A stale report (the guest re-began the record) writes nothing.

**6. Conservative fallbacks: "visible", never 0.** A report is marked uncertain, and so answers at least 1, when any
of these happen:
- a draw inside the query did not reach the GPU (unless its skip reason also produces no samples on Xenos:
  `ds_rast_off`, `ds_no_host_vertices`);
- the draw changed encoder part-way;
- slots ran out;
- the command buffer reported an error.
- An END with no matching BEGIN answers 1 at once.
- An overlapping BEGIN finishes the older report as uncertain.
- None of these happened in any run.

**7. Getting results to a waiting guest.**
- When the game spins, the ring goes idle and `PrepareForWait` (`metal_async_idle_submission`) commits the command
  buffer, so the result arrives as soon as the GPU is done.
- Measured END → write latency: 2.4–7.9 ms on average.

**8. Sleeping instead of spinning** (`src/diagnostics/fable2_zpd_wait.h`).
- With the GPU answering, the culling spin burnt about half a P-core: 540–585 ms/s of `sub_8220BDD8` time, about 25M
  GetData polls per second, and +3.1 ms/frame of whole-process CPU at Bower 2×.
- The Metal backend exports `fable2_zpd_wait_result` / `fable2_zpd_result_generation` /
  `fable2_zpd_real_enabled` (found with `dlsym`). A condition variable is bumped after every result write.
- While `sub_8220BDD8` runs, a GetData that answers "not ready" sleeps until the next write, then returns to the guest
  loop, which polls again. The game sees the same answers in the same order; only the busy loop is gone.
- The flare path is untouched, because it never waits.
- The 250 ms guard is a failure guard only. It logs `[occlusion-real] culling wait: no query result ... (guard ...)`
  when it fires; it fired 0 times in every run.

## Diagnostics

- `FABLE2_ZPD_STATS=1` logs two lines:
  - `[occlusion-real] frame N: begins/ends/results/zero/uncertain/stale/orphan/overlap/segments/skipped_draws/...
    end->write avg/max` every 600 frames;
  - `[zpd-poll]` (GetData calls, not-ready answers, sleeps, culling waits and their time, guard count) every 5 s.
- `FABLE2_ZPD_DIAG=zero` still exists for the fake path (`FABLE2_OCCLUSION_REAL=0`): every query answers 0, which
  gives the upper bound.

## Hazards checked

- **EDRAM tiling:** per-tile records, summed by the game.
- **Resolution scale:** per-segment scale, native-scale passes included, rounded up.
- **MSAA:** Metal counts samples. The occlusion passes (256² and 280×180 depth) are 1×.
- **Pass merge / tile resolve:** counting is enabled only around the guest draw call, so merged or kept-open passes
  and their internal draws do not leak samples into a query.
- **The `ds_depthonly_no_output` skip:** if it ever drops a query draw, the report becomes uncertain ("visible").
  It did not happen.
