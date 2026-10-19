# Flash kit (2026-10-14)

Finds the white terrain flashes seen on the M3 Pro while the camera moves ([native-translator.md](native-translator.md),
"M3 white flashes") without anyone watching, and says which tessellation setting causes them. Built like the
[benchmark kit](benchmark-kit.md): one self-contained app, no Terminal, Python or Xcode on the target Mac.

| Part | File | What |
|---|---|---|
| Detector | `backends/metal/port/rex/ui/metal/metal_flash_detect.h` (hooked in `metal_presenter.mm`) | `FABLE2_FLASH_DETECT=<folder>`: per-frame reduced readback + transient-white scoring |
| Camera plan | `src/core/fable2_benchmark.h` (`FABLE2_BENCHMARK_PLAN=flash`) | turns both ways, then walking with and without turning (~12 s) |
| Orchestrator | `tools/macos/Benchmark/main.swift` (flash mode: `--flash` or Info.plist `Fable2FlashKit`) | the arms, the report |
| Kit packager | `tools/fable2_flash_kit.py` (uses `tools/fable2_benchmark_kit.py`) | `out/flash-kit/Fable II Flash Kit.zip` |
| This Mac | `tools/fable2_benchmark.py run --flash [--arms …] [--reps N]`, Fable II Launcher (Dev Options) Tools > "Run Flash Kit…" | same runs on a build here; the menu opens the packaged kit |

## Arms

Each arm boots the kit's save, waits until ready, runs the flash plan at 2× with a 60 FPS cap and
`FABLE2_NATIVE_TESS_TRACE=1`, then quits. Every switch is passed explicitly. By default each arm runs twice,
interleaved (A B C D A B C D).
- **Save (since 2026-10-15):** the Bower Lake tomb overlook on the lake shore (`hero1-dog-live_2026-10-07_14-10-08`),
  where the M3 Pro flashed. Its terrain draws the blended layers C30A97D946FA2BE4 / FB68A7F2301210E1 over the
  adaptive terrain 5003700B7C9B1C16.
- **Before:** the camp save, where neither Mac showed anything.

| Arm | Switches |
|---|---|
| `adapt` | `FABLE2_TERRAIN_ADAPTIVE=1` with the layer fix (`FABLE2_TERRAIN_LAYER_MATCH=1`, [adaptive-terrain-flashes-20261014.md](adaptive-terrain-flashes-20261014.md)) |
| `adapt-nomatch` | `FABLE2_TERRAIN_ADAPTIVE=1`, `FABLE2_TERRAIN_LAYER_MATCH=0` (the behaviour that flashed) |
| `adapt-tess3` | adaptive + `FABLE2_NATIVE_TESS=3` (reference tessellator for every non-blended draw) |
| `noadapt` | `FABLE2_TERRAIN_ADAPTIVE=0` (the floor the others are read against) |
| `adapt-snap`, `noadapt-snap` (not default) | + `FABLE2_NATIVE_TESS_SNAP_UV=1` (made no difference on the M3 Pro) |
| `induce` (not default) | `FABLE2_FLASH_INDUCE=1`: a white shape every 40th frame (detector check) |

## Detector

Every guest output refresh, a compute kernel on the presenter queue reduces the guest output to a 480-cell-wide grid
(mean RGB and the whitest pixel, min(R, G, B), per cell); the CPU scores frame N when N+1 arrives. A cell is
transient when it is white (every channel ≥ 0.55), bright (luma ≥ 0.65) and ≥ 0.25 brighter than every cell of its
3×3 neighbourhood in both N−1 and N+1:

- **flash**: by the cell mean (patches a few pixels wide and up);
- **crack**: by the cell's whitest pixel (hairlines and single pixels, e.g. sky seen through a crack between patches).

A frame counts when its largest 8-connected cluster has ≥ 6 cells. Only the ground is scored: the top 30 % (sky
through leaves, fence tops, lamps, quest text, health icon), the d-pad HUD box and a 4 % border (new content during
turns) are skipped. The window is the sweep only (the benchmark calls the exported `fable2_flash_control(1|2)`).
No Metal captures. The cost is one 480×270 readback per frame (no FPS change at the 60 cap on the M5 Max).

Why these rules (M5 Max, Bower Lake): a luma-only detector flagged ~105 frames per run in every arm, from the golden
breadcrumb sparkles, lamps and fire. The white test, then the cluster test and then the ground region brought the
floor down to 4–8 flash frames per run, with the largest flash 6–10 cells. The crack floor is higher (~35 per run,
largest 12 cells: a lamp and white sparkle cores). The floor is the same scene content in every arm, so the report
reads each arm against `noadapt` ("vs off").

Detector check (`induce` arm, M5 Max):
- 1.6 % and 3 % squares: found as flash and crack;
- 1-pixel and 2-pixel lines: found as crack only (30–60 cells);
- 0.4 % squares (10 px at 2×, ~2×2 cells): below the 6-cell cluster;
- shapes that land in a skipped region: not found, as intended.

## Report

`~/Desktop/Fable II Flash Kit/<chip>-<model>-<date>-flash/`; the folder can be dropped into `benchmark-results/`.
- `report.txt`: per arm, flash and crack frames per run, the difference to adaptive off, each run's counts, the
  largest flash and crack; then the switches the game log confirms per arm (`[tess-trace] device …` line,
  `snap-uv: on|off` and how many domain shaders were compiled snapped); the worst moments; the detector check when
  the `induce` arm ran.
- `report.json`: everything, plus each run's `flash.json` and tessellation trace lines.
- `examples/<arm>-r<n>-flash-<rank>-f<frame>.png`: previous | frame | next | detected cells (red = flash,
  orange = crack) over the darkened frame.
- `raw/<arm>-r<n>/`: `result.json`, `flash.json`, `flash-frames.csv`, `window-frames.csv`, `fable_2.log`,
  `launcher.log`.

Crashes and abnormal exits (2026-10-14, after an M3 Pro run crashed at quit, bug-patterns #101). The game should
exit with status 0 after each arm's quit. Otherwise (a signal, another status, the arm timeout):
- the arm's data was already written (result.json with the sweep, and flash.json): the data is kept, the run is
  listed under "Run problems" in report.txt ("data kept: the game crashed (signal 5) after the arm's data was written");
- no data: the arm runs once more; the first attempt is kept as `raw/<run>/attempt1/`. A second failure records the
  run as FAILED (left out of the averages, "failed" in the per-run column) and the kit continues with the next arm.
For any such run, `raw/<run>/` holds the whole run folder (logs, flight-recorder dumps) and the macOS crash reports
of that game pid (`crash-*.ips`, copied from `~/Library/Logs/DiagnosticReports` once ReportCrash has written them);
report.json has `outcome`, `abnormal_exit`, `exit_signal`, `crash_summaries`, `first_attempt`. The same policy applies
to the benchmark kit. Test: `tests/benchmark_failure_policy_tests.py` (a fake game that crashes after the data,
before it once, and always).

State: `~/Library/Application Support/Fable II Recomp Flash Kit/` (game files APFS-cloned from the portable install
or the benchmark kit's state when either is complete, otherwise extracted from the disc image).

## Build

```sh
python3 tools/fable2_flash_kit.py build [--save NAME] [--out DIR] [--shader-scales 2] [--e2e | --no-e2e] [--iso IMAGE] [--no-rebuild]
```

The packaging matches the benchmark kit: no-converter release build, shader seed resolved without a game run when
possible, ad-hoc signed, bundle id `org.fable2recomp.metal.flashkit`, Info.plist `Fable2FlashKit = true`. The zip is
in ignored `out/flash-kit/` and contains the maintainer's save, so it is never committed. The end-to-end test (the kit run
as on a fresh Mac) runs with `--e2e`, or automatically when the kit code changed (`out/flash-kit/e2e-state.json`).

## Results

M5 Max (apple10), 2026-10-14, the shipped kit (45d8c10) end to end: packaged no-converter kit, fresh state, disc
image read only, 5 arms x 2 runs in 4.0 min (`out/flash-kit/e2e/reports/…13-11-flash`, KEEP). Flash / crack frames
per ~700-frame run, then each run's counts:

| Arm | flash | crack | runs (flash+crack) | largest flash/crack (cells) |
|---|---|---|---|---|
| adapt | 7.0 | 35.5 | 6+35, 8+36 | 9/14 |
| adapt-snap | 7.0 | 34.5 | 9+35, 5+34 | 9/13 |
| adapt-tess3 | 6.0 | 36.0 | 6+36, 6+36 | 8/14 |
| noadapt | 7.0 | 29.0 | 7+30, 7+28 | 20/12 |
| noadapt-snap | 9.0 | 31.0 | 8+26, 10+36 | 19/11 |

The previous kit's end-to-end run (13:05, same detector) gave adapt 3+33, adapt-snap 6+28, adapt-tess3 6+35.5,
noadapt 5.5+31.5, noadapt-snap 9+36.5. So run-to-run noise is about ±3 flash and ±6 crack frames per run.
`TERRAIN_ADAPTIVE=1` does **not** reproduce flashes on the M5 Max: every arm is at the floor, the same scene events
in every arm (frames ~360–460 and ~700 of the window). The build runs before packaging agree (1 run per arm,
`out/validation/flash-kit/`, KEEP): adapt 4+32, adapt-snap 7+35, adapt-tess3 6+38, noadapt 8+35, noadapt-snap 5+37.

How to read the M3 Pro report (kit since 2026-10-15):
- `adapt` and `adapt-tess3` at the `noadapt` floor while `adapt-nomatch` flashes: the layer fix works;
- `adapt` still above the floor: another mechanism; look at `examples/`.

The results above are from the earlier camp-save kit with the old arm set. For the lake overlook on the M5 Max
see [adaptive-terrain-flashes-20261014.md](adaptive-terrain-flashes-20261014.md).
