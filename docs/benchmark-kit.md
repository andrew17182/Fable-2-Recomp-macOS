# Benchmark kit (2026-10-12)

Measures the Metal port on any Apple Silicon Mac, including Macs with no Xcode, command-line tools or Python
(`/usr/bin/python3` is only a stub there). Everything that runs on the target Mac is native code inside one app.

| Part | File | What |
|---|---|---|
| In-game benchmark mode | `src/core/fable2_benchmark.h`, `fable2_benchmark_pad.h`, hook in `fable2_session_telemetry.h` | `FABLE2_BENCHMARK=<result.json>`: boot save → gameplay → headings8 → result → quit |
| Orchestrator | `tools/macos/Benchmark/main.swift` (`FableIIBenchmark`) | Runs the arms, computes the statistics, writes the report |
| Kit packager | `tools/fable2_benchmark_kit.py` | Portable app + orchestrator + save → `Fable II Benchmark Kit.zip` |
| This Mac | `tools/fable2_benchmark.py`, Fable II Launcher (Dev Options) Tools > "Run Benchmark…" | Same benchmark on the Playtest or Latest build |

## In-game mode (`FABLE2_BENCHMARK`)

Unset, nothing runs. With `FABLE2_BENCHMARK=<absolute path of result.json>`:

1. **Gameplay, then ready.** Waits until the boot save's level-load call has ended and the flight recorder's `loading`
   tag has cleared (60 swaps in a row under 100 ms), then until the game reports it is ready to measure
   ([readiness.md](readiness.md)): `world` (the world is drawing), `steady` (frame pacing stable, no draw-path
   pipeline builds) and `bg_busy` = 0 (no background shader work). There is no fixed settle time;
   `FABLE2_BENCHMARK_READY_TIMEOUT_S` (60 s) is a guard and `ready_timeout` in the result says if it fired. With the
   recorder off it waits `FABLE2_BENCHMARK_WAIT_S` (15 s). No gameplay within `FABLE2_BENCHMARK_GAMEPLAY_TIMEOUT_S`
   (300 s) writes an `error` and quits.
2. **headings8.** The same phases as `tools/f2diag/run.py` `SWEEP_PLANS['headings8']`: a 0.75 s hold, then 7 × (0.7 s
   right-stick turn at RX 24000, 0.75 s hold), about 11 s (`FABLE2_BENCHMARK_PLAN=headings8-long`: 1.5 s, then
   2.5 s holds, about 24 s, the plan before 2026-10-13; reports from the two plans are not directly comparable). The pad of user 0 is replaced at the guest
   `XamInputGetState` wrapper (`fable2::session::ObservePad`), the boundary the test-control replay-pad channel
   uses, so the camera moves exactly as in f2diag. In memory, no files; physical input is off in benchmark runs
   (`FABLE2_PHYSICAL_INPUT=0`, set by the orchestrator), so a stray mouse or key cannot move the camera.
3. **Recorded** (`result.json`): the guest frame id at every phase boundary (the frame CSV and the recorder share
   frame ids, so the window is cut by frame, not by clock), memory footprint (`phys_footprint`) every 250 ms,
   process CPU time and per-thread CPU time by thread name over the sweep, time from process start to gameplay and
   to ready (`ready_s`, `ready_after_gameplay_s`, `ready_timeout`).
4. Writes the result once the frame CSV has the sweep on disk (the Metal frame log's `csv_frame` tag reaches the
   sweep's last frame; 3 s guard) and quits through an SDL quit event on the UI thread, like the window close button.

## Orchestrator (`FableIIBenchmark`)

Arms, one game launch each (scale and cap cannot change live):

| Arm | Cache | Scale | Cap |
|---|---|---|---|
| `cold` | emptied first: only the app's bundled shader seed (kit) or nothing (this Mac) | 1× | 60 |
| `1u` `1c` `2u` `2c` `3u` (`3c` on request) | warm (the cold arm's cache, or `--warm-cache`) | 1×/2×/3× | uncapped / 60 |
| `<arm>-ssm`: `2u-ssm`, `census2x-ssm` by default | warm | as `<arm>` | as `<arm>`, plus `FABLE2_SSM_MERGE=1` |

Default order (2026-10-16): `cold 1u 1c 2u 2u-ssm 2c 3u census1x census2x-ssm census2x`. `3c` left the default: on a
GPU-bound Mac it measures the same as `3u` (M3 Pro 26.6 vs 26.5 FPS). **Switch arms** (`<arm>-<switch>`,
`SwitchArms` in `main.swift`) run a warm arm with one renderer switch set, so a kit run decides that default on the
tester's Mac; the switch's env is recorded in the arm's `env`, and `report.txt` ends with a "Switch arms" line per
pair (FPS, GPU ms, p99 against the base arm). The SSM-merge pair (`docs/ssm-merge-20261015.md`) runs A-B
(`2u`, `2u-ssm`) and B-A (`census2x-ssm`, `census2x`, per-pass GPU trace) so slow drift hits both sides.

**Truly cold (2026-10-13).** Emptying our own cache is not enough: macOS keeps the GPU code it compiled for an app in
`$(getconf DARWIN_USER_CACHE_DIR)/<bundle id>/com.apple.metal` (+ `com.apple.gpuarchiver`, `com.apple.metalfe`),
and Metal has no setting to move or skip it (no env var; checked: only `MTL_SHADER_CACHE_SIZE`). An M3 Pro's second
kit run compiled 398 pipelines in 0.18 s summed (0.46 ms each, vs 23.5 s / 53 ms each the first time) and booted in
6.0 s instead of 19.4 s. The cold arm therefore deletes those three folders first, only for a benchmark-only bundle
id (`org.fable2recomp.metal.benchmark`, the kit app, or `org.fable2recomp.metal.benchmark.dev`: on this Mac
`tools/fable2_benchmark.py` runs an APFS clone of the chosen build with that id, re-signed ad hoc, in
`out/benchmark/app`), after checking the resolved path (`/var/folders/…/C/<id>`, plain folders, no symlinks), and logs
what it removed in `benchmark.log`. Any other bundle id (a normal `Fable II.app` shares its cache with normal play) is
left alone. `FABLE2_BENCHMARK_KEEP_OS_CACHE=1` skips the clearing. The report records it per arm:
`os_shader_cache` (`cleared`, `bundle_id`, `path`, `removed_mb` or `reason`), `truly_cold`, and `shader_warmup`
(from the game log: pipelines compiled + ms summed + ms each, archive hits, transfer pipelines built + ms);
`report.txt` prints one line for the cold arm, and flags a "cold" warm-up under 2 ms per pipeline as served from
macOS's cache.

Check (M5 Max, 2026-10-13, `out/validation/bench-accuracy/`, KEEP): the kit's cold arm removed 113.7 MB of
`org.fable2recomp.metal.benchmark` caches and compiled 399 pipelines in 16.8 s summed (42 ms each; the M5 Max kit run
of 13:16 the same day, cache kept: 501 in 3.5 s, 7 ms each) plus 101 transfer pipelines in 3.3 s; boot 15.2 s (was
7.3 s); one 56 ms `pso_xfer` hitch in the sweep. The dev-flow cold arm (`benchmark.dev` clone) left the 10 GB
`org.fable2recomp.metal` cache untouched. Re-parsing the M3 Pro reports: 3× arms 284 / 281 "hitches" → 0 (the old
lines had also hit the 400-line cap: 491 frames ≥ 40 ms), the 2× 51 ms `slot` spike and the first run's cold 72 ms
`pso_xfer` hitch stay.

Per arm: the saves folder is reset to the bundled save (plus profile options: the camera settings must match), logs
are cleared, and `FableIILauncher` starts with `FABLE2_BENCHMARK`, `FABLE2_BOOT_SAVE=1`, `FABLE2_RES_SCALE`,
`FABLE2_FPS_60`/`REX_VSYNC` (as f2diag `--fps 60` / `uncapped`), `FABLE2_GPU_TIME_TRACE=1` with
`FABLE2_TRACE=gpu:1,cpu:1,sampler:1,frame:1` (GPU command-buffer times and key-thread CPU in the frame CSV, recorder
levels back at their defaults) and `FABLE2_PASS_TRACE=0`, `--audio_mute=true`. Arm timeout 420 s.

Statistics over the sweep's frames (same definitions as `f2diag` `pacing.window` and `gpu.analyze`): FPS mean,
1% low (1000 / mean of the worst 1% frame intervals), p50/p95/p99/max frame ms, frames over 33.4 / 50 ms, GPU ms per
frame (union of the frame's command buffers) mean/p95/p99, key-thread CPU (GPU Commands, GameThread, 3D Engine,
Audio Worker) in cores, process CPU cores, footprint mean/peak, FPS in turns vs holds, hitches in the window (by
frame id) and in the whole session, hitch ms by cause. A hitch is a frame ≥ 1.5 × the median of the previous 32
frames and ≥ 40 ms, or any frame ≥ 100 ms (`metal_hitch.h`, `docs/frame-pacing-20261012.md` §2): steady slow frames
of a GPU-bound Mac are not hitches (before 2026-10-13 every frame ≥ 40 ms counted: an M3 Pro at 3× reported 280–330
"hitches" that were its normal ~47 ms frames). `[hitch]` lines of an older build (no `base`) are reclassified with the
same rule from `window-frames.csv` (`window_count_absolute` keeps the old count). `FableIIBenchmark --reanalyze
<report folder> [--out DIR]` applies the current rule and warm-up summary to an existing report's `raw/` files.

Report: `report.json` (machine, app, arms), `report.txt` (summary table), `raw/<arm>/` (`result.json`,
`window-frames.csv` per-frame interval and GPU ms, `fable_2.log`, `launcher.log`), `benchmark.log`.
Machine info: model identifier, chip, P/E cores, GPU cores (IORegistry `gpu-core-count`), RAM, macOS version and
build, Metal device and working set, every display (points, pixels, max refresh), power source, low power mode,
thermal state (at start and after every arm).

**Census arms** (`census1x`, `census2x`, run last; 2026-10-13): 1× and 2× uncapped headings8 with the per-pass
GPU trace (`FABLE2_PASS_TRACE=1`) and the HLE census (`FABLE2_HLE_CENSUS=1`); window tag `benchmark census2x`.
Their `raw/<arm>/` also keeps `passes-*.csv` and `frames-*.csv`. Tracing costs a little, so their FPS/GPU rows
are not comparable with the other arms. Analysis here (the tester's Mac has no repo):
`python3 tools/f2diag.py census-cost <report folder>` prints that Mac's measured GPU busy time per pass role and per
bucket (main passes / EDRAM emulation / post chain / fixed) over each census arm's sweep window
(`docs/hle-census-20261013.md` §6 replaces its estimates with these).

## Kit (another Mac)

```sh
python3 tools/fable2_benchmark_kit.py build [--save NAME] [--out DIR] [--shader-scales 1,2,3 | --no-shaders]
                                           [--seed-source auto|…] [--e2e | --no-e2e] [--iso IMAGE]
                                           [--with-converter | --no-rebuild]
```

Since 2026-10-14 the kit app is the **no-converter release build** (`out/build-release`, refreshed first, as the
portable package; `docs/no-converter.md`, release decision D3, 2026-10-14): no `libdxilconv` or
`libmetalirconverter`, and the package check refuses either. `--with-converter` packages the dev build `out/build` as
it is; `--no-rebuild` packages `out/build-release` as it is. Kits made before then carried both libraries.

First no-converter kit (M5 Max, 2026-10-14, `out/validation/noconv-release-20261014/kit-e2e/`, KEEP):
- packaged in 35 s, seed from the agent cache;
- app 217 MB, zip 93 MB;
- the end-to-end test passed all 9 arms in 3.3 min;
- every arm logged `[converter-use] totals: none` and no refusals;
- headline numbers: 1× uncapped 155 FPS, 2× uncapped 129 FPS, 3× uncapped 81.8 FPS;
- the cold arm compiled 399 pipelines in 21.9 s summed, with 0 hitches.

The shader seed comes from what is remembered (`docs/offline-shader-seed.md`). The kit's end-to-end test (the kit run
as on a fresh Mac) runs with `--e2e`, or automatically when the kit/benchmark code changed since the last kit that
passed it (`out/benchmark-kit/e2e-state.json`); `--no-e2e` skips it.

`out/benchmark-kit/Fable II Benchmark Kit/` (`Fable II Benchmark.app`, `README.txt`) and the zip. The app is the
portable build (`tools/fable2_portable.py`, `docs/portable-build.md`) with `CFBundleExecutable = FableIIBenchmark`,
bundle id `org.fable2recomp.metal.benchmark`, `Fable2GitSHA`, `Fable2BenchmarkSave`, and
`Contents/Resources/benchmark-save/saves/` (the save-library entry, default `hero1-dog-live_2026-10-11_11-20-20`
Bower Lake, plus the live profile options, read only). **The save is the maintainer's own, for the maintainer's own Macs: the kit only
lives in ignored `out/`.**

On the target Mac:

- Double-click → intro dialog → Start → progress window (Cancel) → "Show Report" opens
  `~/Desktop/Fable II Benchmark/<chip>-<model>-<date>/` (falls back to the state folder's `reports/` without Desktop
  access).
- State: `~/Library/Application Support/Fable II Recomp Benchmark/` (`GameData`, `cache`, `saves`, `runs/<arm>`,
  `benchmark.log`). Never the normal `Fable II Recomp` folder: its `GameData` is only read and APFS-cloned
  (`clonefile`) when complete, so a Mac with the portable game needs no second extraction. Otherwise the first run
  asks for the disc image (`FableIIPrepare`, as the portable build). The portable app does the reverse: on its first
  launch it clones the kit's complete `GameData` when the xex hash matches (`docs/portable-build.md`).
- Gatekeeper: ad-hoc signed, so "Open Anyway" in System Settings > Privacy & Security once.

Test flags (`FableIIBenchmark` in `Contents/MacOS`): `--headless`, `--state DIR`, `--out DIR`, `--iso IMAGE`,
`--arms LIST`, `--no-clone`, `--arm-timeout S`; environment `FABLE2_BENCHMARK_STATE_ROOT`, `FABLE2_BENCHMARK_OUT`;
the rest of the environment reaches the game (`FABLE2_NS_ASSUME_NO_XCODE=1`, `FABLE2_PORTABLE_HEADLESS=1`).

## This Mac

```sh
python3 tools/fable2_benchmark.py run [--build latest|playtest] [--save NAME|live:HeroNNN] [--arms LIST]
                                      [--warm-cache live|none|DIR] [--out DIR] [--verbose]
```

Or the launcher's **Run Benchmark…** (chosen build; chosen save, Bower Lake for the title pick; disabled while a
game runs; progress in the status line; opens the report at the end). It runs `FableIIBenchmark --app <build's
Fable II.app> --game-data <checkout or playtest data> --save-dir <staged save>` with its own state folder
`out/benchmark/state`; reports go to `out/benchmark/reports/`. The warm arms start from an APFS clone of the live
shader cache (read only), as f2diag sessions do. It refuses while a game other than an agent test game runs and takes
the perf lock exclusively (agent test games finish first).

## Validation (M5 Max, macOS 27.0.1, 2026-10-12)

Evidence: `out/validation/benchkit-20261012/` (`local/` CLI reports, `kit-e2e/` the kit report; KEEP).

**Agreement with f2diag headings8**, same build, Bower Lake save, same env (`FABLE2_GPU_TIME_TRACE=1`,
`FABLE2_TRACE=gpu:1,cpu:1,sampler:1,frame:1`, `FABLE2_PASS_TRACE=0`, `--metal_fable_morph_readback=false`: f2diag
sessions otherwise inherit the live `fable_2.toml`, where it is on), f2diag `--no-capture` (captures during a sweep cost
frames), warm live cache, no contention. GPU for f2diag from `f2diag gpu` over the sweep window.

| Arm | benchmark: FPS / 1% low / p99 ms / GPU ms | f2diag: FPS / 1% low / p99 ms / GPU ms |
|---|---|---|
| 1× uncapped | 118 / 92.7 / 10.6 / 8.3; rerun 118 / 92.2 / 10.7 / 8.4 | 114.9 / 90.2 / 10.8 / 8.6 |
| 1× 60 cap | 60.0 / 54.6 / 18.2 / 13.6 | 60.0 / 54.0 / 18.4 / 13.7 |
| 3× uncapped | 63.2 / 51.6 / 19.0 / 15.6 | 62.5 / 52.9 / 18.7 / 15.8 |

Within noise (the benchmark is ~3% faster at 1× uncapped: no harness pad writes or control polling). The first
version started the sweep 3 s after gameplay (~8 s after launch) and caught a CPU-side stall cluster in the 1×
uncapped run (1% low 36, frames to 38 ms with GPU at 8 ms); since the sweep starts no earlier than 12 s after launch,
as f2diag's does, the two reruns match f2diag. GPU ms at a 60 FPS cap is higher than uncapped (13.6 vs 8.3 at 1×):
the GPU clocks down when it has slack, so compare GPU ms between uncapped arms.

**Kit end to end** (`tools/fable2_benchmark.py run --kit … --iso ../ISO/<image> --state <scratch> --no-xcode`): kit
packaged to a scratch folder (58 s incl. the 3-scale shader warm-up; app 215 MB, zip 83 MB), unzipped, signature
verified, run with an empty state folder, the disc image read only, `FABLE2_NS_ASSUME_NO_XCODE=1`. Extraction 451
files in 2 s (image in page cache), then 7 arms in 4.6 min, every exit status 0, skip-intro redirected (no
"playing original"), the pinned bundled shaders installed from the seed (`installed from cache: 128`).

| Run | FPS | 1% low | p99 ms | max ms | > 33 ms | GPU ms mean / p95 | CPU cores | Mem MB | boot s |
|---|---|---|---|---|---|---|---|---|---|
| Cold, 1×, 60 cap | 59.9 | 46.7 | 18.4 | 58.6 | 1 | 13.5 / 14.8 | 1.3 | 3556 | 15.6 |
| 1× uncapped | 113 | 88.0 | 11.1 | 11.8 | 0 | 8.8 / 10.6 | 2.0 | 3602 | 4.0 |
| 1× 60 cap | 60.0 | 53.8 | 18.4 | 18.8 | 0 | 14.3 / 15.2 | 1.2 | 3463 | 4.8 |
| 2× uncapped | 86.7 | 66.7 | 14.4 | 22.0 | 0 | 11.4 / 13.7 | 1.7 | 4356 | 9.5 |
| 2× 60 cap | 60.0 | 53.8 | 18.4 | 18.8 | 0 | 14.0 / 15.1 | 1.3 | 4242 | 4.8 |
| 3× uncapped | 59.7 | 49.9 | 19.8 | 20.5 | 0 | 16.5 / 19.2 | 1.3 | 5556 | 6.2 |
| 3× 60 cap | 57.7 | 50.3 | 19.7 | 20.2 | 0 | 16.6 / 19.0 | 1.3 | 5724 | 4.9 |

- The cold arm's one gameplay hitch (58.6 ms) is `pso_xfer` 48 ms: a transfer pipeline built on the draw path
  although `FABLE2_XFER_PREWARM` is on. Cold boot also had 7 load-time frames over 100 ms (worst 373 ms).
  Worth a look (`docs/frame-pacing-20261012.md` §4); not changed here.
- 3× is GPU-bound at ~16.5 ms, so the 60 cap gives 57.7 FPS, as in `docs/portable-build.md` (57.3).
- Kit 1× uncapped (113) vs the local CLI (118): pinned converter target, the bundled seed cache instead of the live
  one, and no local optimizer; within ~4%.

Not testable here: Gatekeeper on a downloaded zip, the Desktop permission prompt, a real Mac without developer tools,
other GPU families. Those are the M1 / M3 Pro runs.
