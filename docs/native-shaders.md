# Faster guest shaders on Metal (2026-10-09)

Branch `feature/native-shaders`. This started as a feasibility study for native shader translation
(Xenos microcode → MSL instead of Xenos → DXBC → DXIL → Metal Shader Converter). The measurements
found a cheaper route with most of the gain. Metal Shader Converter output never goes through the
Metal front end's optimizer. Running Apple's `metal-opt -O2` over it makes the hot pixel shaders much
faster. Milestone 1 shipped that route behind `FABLE2_NATIVE_SHADERS=1`. Milestone 2 removed its
first-launch delay, and the switch is now **on by default** (`FABLE2_NATIVE_SHADERS=0` disables it).

## Decision

**GO, via "optimize the existing converter output", not a new translator (yet).** On effort to
gain, A clearly wins: it gets the measured 15–19% for about 400 lines, with no new ABI and no new
dependency. The XeniOS emitter (B) is the long-term option if the Metal Toolchain dependency or the
first-launch cost becomes a problem. It compiles through the Metal front end, so it should get the
same optimization. It is a much larger port, though, with its own binding model.

| | Gain (M5 Max, Old Tomb uncapped turn30, `f2diag gpu`, 20–50 s) | Effort | Risk |
|---|---|---|---|
| A. `metal-opt -O2` on each guest pixel shader's MSC library (milestone 1) | 1×: GPU 5.81 → 4.92 ms (−15%), 173 → 202 FPS. 2×: 9.67 → 7.83 ms (−19%), 103.5 → 128.6 FPS | S (~400 lines) | Low–medium: same Xenos semantics and ABI, per-shader fallback. Needs the Metal Toolchain at run time (the build already does; without it: plain shaders, one log line). No first-launch cost since milestone 2 |
| A-. LLVM scalar + loop passes on the DXIL inside dxilconv (tried, dropped) | 1×: 5.80 → 5.66 ms (−2%) | S | DXC's `PassManagerBuilder` hangs on DXIL (an HL pass), so only a hand-picked list is possible |
| B. XeniOS native MSL emitter (`msl_shader_translator.cc`) | Unknown here; it also compiles through the full Metal front end, so it would get at least A's gain | L–XL: ~7.5k translator lines + ~2.7k command-processor lines, its own binding ABI | High: new ABI, all shader features, upstream is WIP |
| C. Draw-time specialization (fetch-constant formats, signs) | Small on top of A: 2.39 → 2.35 ms main pass in the prototype | M | Shader-variant growth |
| D. Drop Xenos 0·x = 0 multiply emulation | 2.39 → 2.21 ms in the prototype | S | Not semantics-preserving; rejected |
| E. SPIR-V translator + SPIRV-Cross → MSL | Like B | L: Vulkan binding model ≠ our bindless/MSC layout | High |

## Step 0 measurements

### Tools (what is measurable on Apple Silicon)

- `xctrace` "Metal GPU Counters" recorded headless returns empty counter tables here (developer
  mode off); not used.
- **In-process GPU capture + replay profiling works without Xcode UI or entitlements:**
  - `MTL_CAPTURE_ENABLED=1 FABLE2_GPU_CAPTURE_PATH=x.gputrace FABLE2_GPU_CAPTURE_AT_S=25`
    (trace-only, `metal_command_processor.cc` `GpuCaptureTick`) writes one guest frame.
  - `gpudebug -t x.gputrace -c "profile run"` replays it on the local GPU (M3 or later) and fills
    `/performance`: per-encoder vertex/fragment/compute time, per-shader and per-draw cost, hardware
    limiters (instruction issue, F32, integer/conditional, texture, bandwidth, occupancy), and static
    stats of the compiled GPU binary (instructions, temp registers, spilled bytes).
  - `FABLE2_SHADER_LABELS=1` labels pipelines `vs:<hash>/<mod> ps:<hash>/<mod>` (and libraries), so
    the profile names guest shaders. Trace-only; labels are not part of archive lookups.
- Measured directly: per-encoder stage time, per-shader cost share, limiter percentages, native
  instruction/register/spill counts. Inferred: how much of a stage's time is ALU vs memory, from
  the limiter counters (the hardware reports utilisation per unit, not a time split).
- Note: `gpudebug` sometimes returns an empty `/performance` tree right after `profile run`;
  repeat the query a few seconds later.

### Shader share of GPU time

1× frame (replay GPU time 7.9 ms):

- Guest-draw render encoders carry 83.5% of the profiled shader cost; stage time vertex 0.98 ms,
  fragment 3.36 ms. Compute (resolves, texture uploads) 0.9 ms; EDRAM transfer encoders 3.9%.
- Main scene pass (RGBA16F 2x MSAA): fragment 1.64 ms; **instruction-issue limiter 50%**,
  integer/conditional 31%, F32 21%, texture read cache 8%. Not bandwidth-bound (LLC limiter
  frame-wide 30%, texture limiters ≤ 11%).

2× frame (replay GPU time 21.9 ms, a heavier view):

- Main pass fragment 6.08 ms (48% of shader cost); issue limiter 58%, integer/conditional 33%,
  F32 24%, texture read cache 11%.

### Hottest shaders

All on vertex shader `ECD66A10092E6562` (the material shaders of the main pass) unless noted.

| PS | Share of shader cost | Xenos instructions (ALU+fetch) | GPU binary instructions | Temp registers | Spilled bytes |
|---|---|---|---|---|---|
| 356E4E7B9E2B36E6 | 23% (2×) | 253 (19 fetches) | 5053 | 126 | 176 |
| CD2CE6D440B77BCE | 17% (2×) | — | 4014 | 96 | 0 |
| 28E202861D3E2411 | 20% (1×) | 187 (15 fetches) | 4112 | 99 | 0 |
| F2F103F16764043B (VS D4D558DA…, full-screen) | 17% (1×), 4% (2×) | 243 (40 fetches) | 5779 | 126 | 48 |
| DBFD88A80EDBCE36 (VS 29B6506F…) | 4–8% | 5 | 413 | 20 | 0 |

### Code quality of the current output

- A median pixel shader is about 3,100 AIR instructions (540 fragment functions in the cache).
- No register arrays in memory: the only allocas are the output struct.
- Bloat by source:
  - **Xenos multiply semantics** (0·x = 0): `select(min(|a|,|b|) == 0, 0, a·b)` per component;
    hundreds of fabs/fmin/fcmp/select per material shader. Required for correctness.
  - **Runtime fetch-constant decode**: texture size, exp-adjust, LOD bias, signedness, gamma and
    resolution-scale paths read and decoded per fetch, with uniform branches, plus explicit
    gradients (dfdx/dfdy) for every fetch. The same design as Xenia's D3D12 and SPIR-V paths.
  - **Unoptimized IR**: dxilconv runs only its DXIL cleanup pass; Metal Shader Converter converts
    at the binary level. The result has one constant-buffer load and one address computation per
    use, duplicated fabs of the same constant, and redundant descriptor loads. The Metal driver
    compiler does not recover this.
- Translated shaders bake in the draw resolution scale: 188 of 253 metallibs differ between 1× and
  2× with the same modification key (the DXIL hash keeps the caches apart).

### Prototype (one hot shader, in-game A/B)

Trace-only overrides (`FABLE2_NS_DUMP_DIR`, `FABLE2_NS_OVERRIDE_DIR`) swap one shader's metallib.
Static Old Tomb view, 2×, main-pass fragment time from the replay profile:

| Variant of PS 356E4E7B | Main pass fragment | Shader cost share |
|---|---|---|
| Base (MSC output), two runs + archive-off control | 3.20 / 3.21 / 3.22 ms | 19–20% |
| AIR round-trip through `metal-opt -O2`, no other change | 2.39 ms | 9% |
| + `air.extract_bits` lowered | 2.38 ms | 8% |
| + fetch-constant dwords 3/4 specialized | 2.35 ms | 8% |
| + 0·x emulation removed (not semantics-safe) | 2.21 ms | 7% |

The GPU binary's static instruction count barely moves (5053 → 5086); the gain is in what the
driver compiler does with the cleaned-up IR (dynamic work), not in a smaller static count.

### FP semantics of -O2

IR audit of PS 356E4E7B before and after: no new `fast`/`contract`/`nnan`/`ninf` flags, plain
`fmul`/`fadd` stay flag-free, `air.fma` count unchanged (108 → 107 after CSE), the 0·x selects
stay, and module flags are identical. The pass pipeline only removes redundancy (CSE, DCE, CFG
simplification). MSC's own `reassoc nsz arcp afn` flags on intrinsic calls are unchanged.

### Correctness checks of the prototype

- Static Old Tomb, 1×: mean absolute difference 1.2/255 vs 0.8–1.0 for base vs base; blocks away
  from the animated hero/gate effects differ by 0.0–0.1.
- Static Old Tomb, 2×: 0.8–1.2 vs 1.05–1.3 base vs base; no systematic brightness shift.
- Bower Lake (area run, 62–66 s): mean 0.14–0.35/255, 0.004–0.017% of values > 24 (beetle animation);
  identical by eye.
- `f2diag images dark|blue` counts match base within run-to-run spread (area 3/0 vs 3/0).
- A first 1× test used 2×-translated shaders by mistake and showed a lighting difference; the
  bisect (one downsample shader, `87E85199E1E9B260`) led to the resolution-scale finding above.
- Vertex shaders through -O2: no measurable gain (1× 4.93 ms vs 4.86 ms PS-only).
- Compile cost of the AIR route: `metal-opt -O2` 35 ms p50, 162 ms p95, 271 ms max per pixel
  shader including process start.

### Prior art and licences

- **XeniOS** (`xenios-jp/xenios`, BSD-3 like Xenia): branches `preview` and
  `backup/ios-native-msl-20260609` carry `msl_shader_translator.cc` (~7.5k lines), a native Xenos →
  MSL emitter with its own binding ABI (buffers 20–29, separate texture/sampler heaps), texture-sign
  specialization, live-interpolator pruning and per-shader fallback to MSC. Default off, described
  as WIP. Our pinned `xenios-source` (87b176a) does not contain it.
- **Xenia SPIR-V translator** (in the SDK, BSD-3) + **SPIRV-Cross** (Apache-2.0): mature through
  MoltenVK, but it targets Vulkan descriptor sets, not our bindless MSC layout; same Xenos semantics
  (runtime fetch decode, 0·x emulation), so it would also need -O2-class optimization to win.
- **Metal Shader Converter** (Apple licence, binary): converts at the binary level and does not
  optimize the incoming IR; our pinned build reports IR converter 3.1.1.
- **DirectXShaderCompiler / dxilconv** (NCSA/LLVM licence, already a dependency): contains the
  LLVM 3.7-era optimizer. Running it on the DXIL (route A-) was tried and dropped (−2%).
- **Metal Toolchain `metal-opt` / `metallib`** (Apple, part of Xcode's Metal Toolchain component,
  which the build already requires): used at run time by route A. Nothing is redistributed and no
  new dependency is pinned.

## Why the gain: the dispatch loop

The DXBC translator emits Xenos control flow as a loop around a `switch` on the program counter.
Metal Shader Converter keeps that loop. Every constant-buffer pointer, descriptor-table pointer and
fetch-constant load sits inside it. `metal-opt -O2` runs the Metal front end's pipeline. Its loop
passes (LICM, loop rotation, trivial unswitching) hoist those loads, and the driver compiler then
produces much faster code. The static GPU instruction count barely changes.

Bisect on PS 356E4E7B (static view, 2×, main-pass fragment 3.20 ms base):

| AIR passes | Main pass fragment |
|---|---|
| full `-O2` | 2.39 ms |
| `sroa,early-cse,loop-mssa(licm,loop-rotate,licm,simple-loop-unswitch<trivial>),loop(indvars,loop-deletion)` | 2.38 ms |
| scalar only (early-cse, instcombine, gvn, simplifycfg, adce) | 3.24 ms |
| other single passes: sroa, function-attrs, alias-scope propagation, unused-args | 3.19–3.27 ms |
| no-loop -O2 subset (speculative execution, correlated propagation, reassociate, gvn, …) | 3.15 ms |

The same loop passes on the DXIL do little (−2%). In DXIL the outputs are `storeOutput` calls,
which write memory, so LICM cannot move the read-only loads past them. In AIR the outputs go
through a local alloca, which does not block LICM.

## Milestone 1: optimized pixel shaders (`FABLE2_NATIVE_SHADERS=1`)

**What it does.**

- `metal_air_optimizer.{h,cc}` (new):
  - finds `metal-opt` and `metallib` once with `xcrun --find`;
  - writes the library's single bitcode module to a scratch folder (milestone 2: one per-user
    folder, see below);
  - runs `metal-opt -O2`, then `metallib` (posix_spawn, 30 s timeout), and reads the result back.
- `metal_pipeline_cache.cc` `EnsureMetalTranslationReady`: for guest **pixel** shaders the
  optimized library replaces the converter's before `InstallMetal`.
- Cache: the optimized library is stored in the artifact store (`.metal.artifacts`) as a
  DXIL-bytecode record whose key is:
  - the plain metallib's 128-bit hash (that already covers the DXIL and the baked-in resolution
    scale);
  - a hash of `metal-opt --version`, so a toolchain update re-optimizes;
  - version tag `NS1`.
- Separate binary archive: `4D5307F1.rtv-bindless.ns1.*.binarchive`. Optimized and plain pipelines
  never share an archive.

(Milestone 2 replaced the hook in `EnsureMetalTranslationReady`; see the next section.)

**Fallbacks.** Each fallback keeps the plain library, logs one line and bumps a counter (log prefix
`[native-shaders]`). They cover:

- tools missing;
- a library with more than one module;
- a tool failure or timeout;
- Metal rejecting the optimized library.

Progress lines report installed / from cache / fallbacks / ms spent optimizing.

**Not optimized.** Vertex shaders (no gain measured), geometry/tessellation emulation stages and the
backend's helper shaders. Each of those is under 2% of shader cost.

### Results

All runs are on the M5 Max, Old Tomb (`hero1-dog-live_2026-10-07_14-45-08`), uncapped turn30,
`f2diag gpu`, 20–50 s window, one run per arm. Every run's verdict is ok, including the process-exit
check, and `pgrep` returned 1 after each.

| Run | GPU mean (ms) | GPU p95 (ms) | FPS |
|---|---|---|---|
| 1× off | 5.81 | 9.94 | 172.5 |
| 1× on, cold cache (first launch after the rebuild) | 5.00 | 8.50 | 197.5 |
| 1× on, warm | 4.92 | 8.48 | 201.8 |
| 2× off | 9.67 | 16.11 | 103.5 |
| 2× on, cold | 8.01 | 12.81 | 126.4 |
| 2× on, warm | 7.83 | 12.80 | 128.6 |

**Compile cost.**

- Per pixel shader, `metal-opt` + `metallib`: 35 ms p50, 162 ms p95, 271 ms max, including process
  start.
- First launch with an empty optimized cache:
  - 7.6 s of optimization summed over the compile workers;
  - background warm-up 13.2 s, against 0.2 s;
  - the save's level load starts at 15.8 s, against 2.8 s.
- Warm launches: warm-up 0.27 s and a 2.85 s level-load start, the same as off.
- Shaders first met in play: one background optimization each, then cached.

**Correctness.**

- Static Old Tomb view, against two base runs:
  - 1×: mean absolute difference 1.4/255. Base vs base gives 0.8–1.0. Away from the animated
    hero and gate glow, blocks differ by about 0.1.
  - 2×: 0.7–1.1, against 1.05–1.3 base vs base.
  - No systematic brightness shift.
- Bower Lake (area run): same look by eye. Pixel differences come from the camera and beetle
  positions diverging between runs (optimized prototype vs M1 differ by as much as base vs M1).
- Dog fur: dark dips over 1% match off (1×: 14 on vs 21 off; 2×: 15 vs 14; area 3 vs 3). The
  flagged on-frames are the near-camera pillar; dog fur is normal.
- FP semantics: see "FP semantics of -O2" above.

## Milestone 2: no first-launch delay (default on)

Milestone 1 optimized each pixel shader synchronously when it was installed, so a cold optimized
cache cost about 7.6 s of `metal-opt` work at warm-up: warm-up took 13.2 s instead of 0.2 s, and the
save's level load started at 15.8 s instead of 2.8 s. Milestone 2 never waits for optimization.

**Design** (`metal_pipeline_cache.cc`, `metal_shader.{h,cc}`, `metal_air_optimizer.cc`):

- **Install.** If a pixel shader's optimized library is already in the artifact store (a disk read,
  no tools), it is installed directly. Warm launches therefore build optimized pipelines straight
  from the `.ns1` archive, as in milestone 1. Otherwise the plain library is installed immediately,
  exactly as with the switch off, and the shader is queued for background optimization. Install
  never waits for the tool lookup: if the lookup has not finished, the shader goes the plain route
  too.
- **Two archive sets.** Pipelines with a plain pixel shader use the switch-off archive
  (`rtv-bindless`), so a cold optimized cache still warms up from a warm plain archive. Pipelines
  with an optimized pixel shader use `rtv-bindless.ns1`. `CreatePipelineFromHandle` picks the set
  from the fragment function it actually used.
- **Background pool.** It runs `min(4, cores/2)` threads at utility QoS, and the tools are spawned
  at utility QoS too (`posix_spawnattr_set_qos_class_np`). Swap jobs run before optimize jobs.
  Among the optimize jobs, the hottest shader goes first: each translation counts its pipeline
  lookups, one relaxed atomic per draw. Ties keep warm-up order.
- **Publish and swap.** An optimized library becomes a second function on the translation
  (`optimized_function()`, atomic). Each standard pipeline handle that uses the translation and was
  created plain is registered. Registration and publication are ordered under one mutex, so no
  handle is missed. The pool then:
  - waits until the handle's plain pipeline has been created;
  - builds the optimized pipeline completely (archive lookup, compile on a miss, add to the
    `.ns1` archive);
  - `exchange`s it into `handle->state`.

  The draw path loads `state` once per draw, so the swap takes effect at the next lookup and never
  in the middle of a draw. The plain pipeline stays retained, because in-flight command buffers may
  reference it; it is released with the cache. A draw is never dropped or delayed: the plain
  pipeline stays bound until the optimized one exists, and same attachment formats and same
  semantics mean no visual pop. Handles created after the optimized function is ready use it
  directly.
- **Not swapped.** Geometry- and tessellation-emulation pipelines keep the library that was
  installed: optimized on a warm launch, plain on a cold one. They are under 2% of shader cost.
- **Persisted** as before: an `NS1` artifact record per optimized library, plus the `.ns1` archive.
- **Logs** (`[native-shaders]`):
  - `on: N background optimizer threads`;
  - the tool lookup line (ms, scratch folder);
  - `optimized pixel shaders installed from cache: N` at powers of two;
  - progress lines at powers of two and when the pool drains. They give queued, optimized-ready
    (+ from cache), plain pipelines, swapped, plain-in-use, swap failed, fallbacks and ms spent
    optimizing.
- **Scratch files.** One per-user folder, `$TMPDIR/fable2-native-shaders/`. It exists only while
  jobs run: created by the first in-flight job, removed with the last. Files are named
  `<pid>-<n>.*`. At startup, the lookup removes stale files of dead pids and the old per-pid
  folders of milestone 1.
- **Tool lookup.** It runs on a background thread at renderer init. It uses posix_spawn, because
  popen forks the large game process: 402 ms with popen, 16–50 ms now.
  `FABLE2_NS_TOOL_DIR=<dir>` takes the tools from `<dir>`; `/nonexistent` simulates a missing
  Metal Toolchain.
- **Archive fix found on the way (CR-05 root cause).** The cold runs saved the `.ns1` archive at
  warm-up and again at the 30 s interval, and the second save always failed ("expecting 'fragment'
  stage in pipeline no. N"). The next launch then recompiled 224 pipelines. Every such failure in
  the logs is a second serialize of the same in-memory archive; no session has ever saved one
  archive twice. A saved archive is now closed, and later pipelines go to the next segment
  (`FABLE2_ARCHIVE_SAFE_SAVE=0` restores the old behaviour). Check: with a 2 s save interval, three
  saves in one session all succeeded, and the next launch hit 389 of 392.

### Results (M5 Max, Old Tomb, uncapped turn30, `f2diag gpu`, 20–50 s, one run per arm)

The machine ran about 8% slower in this session than in the milestone 1 session, for both arms
(1× off 6.29–6.34 ms here vs 5.81 ms then), so compare within the table.

| Run | Ctrl ready | Level load starts | Warm-up | Background done (after storage init) | GPU mean / p95 (ms) | FPS |
|---|---|---|---|---|---|---|
| 1× off | 1.20 / 1.17 s | 2.85 / 2.93 s | 0.18 / 0.25 s | — | 6.34 / 10.89, 6.29 / 10.83 | 158 / 159 |
| 1× off (`=0`, default build) | 1.14 s | 2.82 s | 0.16 s | — | 5.77 / 9.90 | 171 |
| 1× on, cold (first launch after a rebuild) | 1.89 s ¹ | 2.86 s | 0.17 s | 4.92 s | 5.26 / 9.19 | 190 |
| 1× on, cold (archive fix, first after rebuild) | 1.87 s ¹ | 2.94 s | 0.28 s | 5.16 s | 5.30 / 9.26 | 188 |
| 1× default (on), cold (first after rebuild) | 1.84 s ¹ | 2.82 s | 0.16 s | 5.10 s | 4.93 / 8.48 | 198 |
| 1× on, warm | 1.19 / 1.23 s | 2.96 / 2.89 s | 0.26 / 0.20 s | — (128+ installed from cache) | 5.32 / 9.19, 4.84 / 8.42 | 188 / 206 |
| 2× off | 1.19 s | 3.03 s | 0.27 s | — | 10.40 / 17.77 | 97 |
| 2× on, cold | 1.19 / 1.17 s | 2.96 / 2.88 s | 0.21 / 0.15 s | 4.93 / 4.90 s | 8.60 / 13.90, 8.32 / 13.59 | 118 / 122 |
| 2× on, warm | 1.18 / 1.23 s | 3.00 / 2.93 s | 0.26 / 0.18 s | — | 8.62 / 14.12, 8.55 / 13.80 | 118 / 119 |
| Boot plan (60 fps), off / off / on cold / area on cold | 1.18–1.20 s | 3.44 / 3.36 / 3.35 / 3.36 s | 0.17–0.25 s | 4.55 / 4.36 s | — | — |

¹ The first launch after every rebuild starts about 0.7 s later, before any renderer code runs
(the process start to the pipeline cache). That happens with the switch off too; level-load start
is unaffected.

- **Gain.** −15 to −17% GPU at 1× and −17 to −20% at 2×, the same as milestone 1.
- **Cold first launch.** The same as off: warm-up 0.15–0.28 s (milestone 1: 13.2 s), level-load start
  within ±0.1 s of off (milestone 1: +13 s).
- **Swap timing.** All 183 queued pixel shaders were optimized and all 366 plain pipelines swapped
  by 4.4–5.2 s after storage init, about 1.5–2.5 s into gameplay, with 0 swap failures and 0
  fallbacks. CPU for that: about 19–23 s summed over the 4 utility threads, once per new
  shader/toolchain.
- **GPU per window, 1×.**

  | Window | Default, cold | Warm | Off |
  |---|---|---|---|
  | 30–43 s | 4.86 ms | 4.76 ms | 5.68 ms |

  Windows before 12 s are a static view and too noisy to compare.

**Correctness.**

- Static Old Tomb view, last capture at ~25 s, 1×:
  - on (cold, swapped) vs off: mean absolute difference 0.96 and 0.50/255;
  - off vs off: 0.86/255;
  - brightness shift −0.1; block-mean differences 0.13–0.21 median.
- Dark dips over 1% (`images.dark_dips`), on vs off:
  - 1×: 22–25 vs 15–26;
  - 2×: 10–11 vs 11;
  - Bower Lake area run, cold: 2, against 3 for the base and milestone 1.
- Blue-transient frames: no more than off (1×: 2–4 vs 6–8; 2×: 9–14 vs 20).
- White transients: only the boot loading frame at 3.0 s, also present with the switch off.
- No shader-miss flash during the swap window.
- 0 AVs, every verdict ok, and `pgrep` returned 1 after each of the 20 runs.

**Toolchain missing** (`FABLE2_NS_TOOL_DIR=/nonexistent`, default build):

- two log lines: "on: 4 background optimizer threads", then "metal-opt/metallib not found … plain
  shaders are used";
- nothing is queued;
- the boot completes (level load at 3.24 s), verdict ok.

**Temp folder.** No `fable2-native-shaders*` entry is left in `$TMPDIR` after the runs.

### Default decision: on

The maintainer's criteria for default on all hold:

- (a) The cold first launch is no slower than with the switch off, within noise.
- (b) Correctness: pixel differences are within base-vs-base noise, the dog-fur/dark detector is
  clean, and there are 0 AVs.
- (c) The toolchain-missing fallback works.

The remaining costs are one-off and invisible:

- about 5 s of plain-speed rendering on the first launch with new shaders;
- about 20 s of utility-QoS CPU, once.

It is an internal default-on improvement, so it gets no Fable II Launcher toggle (`FABLE2_NATIVE_SHADERS=0`
disables it).

### Part 2 (build-time pre-optimization): deferred

A build step that pre-optimizes the cached shaders was not added:

- **Where the cache lives.** The optimized cache sits in the app-support state, per GPU, ABI and
  OS key. A build step would have to write into the player's live state, which the build must not
  touch.
- **What it needs.** Producing the plain metallibs to optimize takes the full translation chain
  (Xenos → DXBC → DXIL → Metal Shader Converter) for every stored shader and modification. That
  only exists inside the game process with a Metal device, so this would be a headless mode of the
  game binary.
- **What it would gain.** With part 1, a cold cache costs nothing visible, so it would only save
  about 5 s of slower GPU frames once.
- **Build time added:** 0.
- **Since 2026-10-12** the cache keys no longer carry the OS (converter target pinned, below), and the setup app can
  warm the player's own cache after the build with one hidden game session instead of a headless mode
  (`docs/setup-app.md`, "Shader warm-up"). The first-play hitches it removes are the draw-path translation and
  emulation-pipeline builds of a cold cache, not the optimizer's background work.

## Emulation pipelines (`FABLE2_NS_EMU_PIPELINES`, 2026-10-11)

Found by the Bet A shader ceiling test (`docs/engine-options-20261011.md` §9).

**Problem.** Milestone 2 swapped only standard pipelines. Geometry- and tessellation-emulation pipelines
(`GetOrCreateGeometryPipelineState`, `GetOrCreateTessellationPipelineState`) took
`pixel_translation->metal_library()` when they were created and kept it for the whole session. A shader
installed before the tool lookup finished (about 0.4 s into startup) is installed plain, so its emulation
pipelines stayed plain, even when the optimized library was in the cache. The milestone 2 note "under 2% of shader
cost" held for Old Tomb but not outdoors. At 3× in Bower Lake, **18% of all shader cost ran in plain libraries, all
of it in emulation pipelines**. Two cases dominate:

- the tessellated terrain PS `AF1E16EE57E4BB06` (7.6%, 57 draws, 31% of the main pass);
- the full-screen PS `0F99810F072BF0FC` (5.0%, one draw).

Plainness was checked offline: `metal-opt -O2` shrinks a plain module and leaves an optimized one unchanged.

**Fix** (`FABLE2_NS_EMU_PIPELINES`, default on, `=0` restores the old behaviour):

- A new emulation pipeline uses the optimized library when it is ready (`MetalTranslation::optimized_library()`).
- Otherwise the pipeline gets a rebuild closure (stages, formats, blend, fragment entry point) and is registered
  per translation.
- When the pool publishes the optimized library, from the optimizer or the cache, it rebuilds those pipelines on
  the utility threads. It then publishes each one atomically in `NsEmuSwap::optimized`.
- The draw path binds `current_pipeline()`. The plain pipeline stays alive until the cache is destroyed. The pool
  stops before the stage cache is released.
- Log: the `[native-shaders]` progress lines end with `emulation pipelines plain N, swapped N, failed N`.
- 3× Bower Lake: 35 emulation pipelines were created plain, and all 35 were swapped by 3.1 s after storage init,
  0 failed.

**Shaders kept plain** (`FABLE2_NS_KEEP_PLAIN=<hash>[,…]`; unset = the built-in list; `none` = optimize all).

- `-O2` made `0F99810F072BF0FC` slower:
  - replay: +34% shader cost;
  - live 3×: its full-screen pass went from 1.24 to 1.39 ms.
- Static IR size does not predict this: the `-O2` module is 25% smaller. GPU instruction and register counts are
  not available without a GPU replay, so the guard is a measured per-hash list rather than a heuristic. The
  shader is never queued and its library stays plain everywhere. Log: `PS … kept plain`.
- **Sample of standard pipelines:** see "-O2 regressions among standard pipelines" below.

**Auto-learn results** (2026-10-11, M5 Max, `headings8` uncapped, `f2diag gpu` 13–33 s, no contention). The fresh cache
was a copy of a played cache with no learn file and no NS2 records. Sessions are chained: each starts from the
previous one's cache. Off = `FABLE2_NS_SPECIALIZE=0` from the fresh cache.

| Session | GPU ms | vs off | Guard | Notes |
|---|---|---|---|---|
| Bower Lake 3× off | 16.82 | | | |
| 1. Bower Lake 3× | 16.00 | −4.9% | 506k hits, 793 misses, 2 demoted | 16 of 17 sampled shaders converged; 11 variants generated about 11 s after storage init (1.3 s of background work) |
| 2. Bower Lake 3× | 15.68 | −6.8% | 745k hits, 0 misses | 14 from cache, 2 regenerated (the demoted ones); same as a manual learn session (15.59) |
| Old Tomb 3× off | 13.40 | | | |
| 3. Old Tomb 3× | 11.37 | −15.2% | 324k hits, 3.8k misses, 3 demoted | Bower-learned variants of shared shaders (`648B965C` ×2, `44B56763`) missed 100% there and were demoted; Tomb-only shaders learned in the session |
| 4. Bower Lake 3× | 15.82 | −6.0% | 737k hits, 0 misses | the variants regenerated with the Tomb miss bits still hit 100% in Bower |
| 5. Old Tomb 3× | 11.04 | −17.6% | 605k hits, 780 misses (0.13%), 0 demoted | manual learn gave 11.01 |

- So specialization starts in the first session in an area, about 10 s into gameplay, and is complete from the
  second. A shader shared between areas may be demoted once in the second area, then fits both.
- Cross-scale: Bower 2× and 1× `spin5` sessions on 3×-only learn data had all variants at launch (some generated,
  the scale-independent libraries from cache), 155k hits each, 0 misses, 0 misbinds.
- CPU cost, Bower Lake 1× uncapped, two runs per arm. The learn arm sets `…_MIN_SAMPLES` so high that the 17 shaders
  sample all session (worst case):

  | Arm | GPU Commands ms/frame | FPS |
  |---|---|---|
  | `FABLE2_NS_SPECIALIZE_AUTOLEARN=0` | 8.59, 8.96 | 113.6, 110.1 |
  | sampling all session | 8.62, 8.55 | 112.9, 113.2 |

  - The difference is inside run-to-run noise.
  - The internal counter: 336k sampled draws took 76 ms, 0.23 µs each, about 0.025 ms/frame (0.3% of the thread).
  - In normal use, sampling stops at convergence: 4–11 ms in total in a first session, under 0.3 ms afterwards.
- Footprint:
  - Memory: about 4 KB of statistics per sampled shader, freed at convergence.
  - Disk: learn file 78 KB after one area and 108 KB (22 records) after two. A learn session's file was about 2 MB.
  - The background writer rewrites the file only when something changed.
- Correctness (`FABLE2_NS_SPECIALIZE_VERIFY=1`):
  - 0 misbinds over about 1.2 M verified hits: `spin5` 1×/2×/3×, Old Tomb 3×, area, boot.
  - Area, boot, F5 and bootloop 3/3 pass, 0 AVs, `skip_intro_missing` 0. Area and boot ran from a clone of a played
    cache, so they exercised learning too.
  - Live parity, Bower 3× `spin5`, idle phase: off vs off median 0.07% / max 0.72%; off vs on (two runs each)
    median 0.14–0.16% / max 1.6–2.3%, within the off-vs-off range measured for manual learn data above (max 6.8%).
  - Parity runs with 4 games in parallel captured during the loading screen at different times. Run the arms one
    after another.
- Without tools (`FABLE2_NS_ASSUME_NO_XCODE=1` + `FABLE2_NS_CACHE_TOOLCHAIN`, the portable launcher's mode), Bower 3×
  on the learned cache: all 20 variants from cache, no sampling, 139k hits, 0 misses, 0 misbinds.

**Exactness.** Replay of the same captured frame with `-O2` libraries for both shaders: the 97 MB scaled resolve
buffer after the main pass and the main-pass colour attachment are **byte-identical** to the plain run
(`tools/shaderlab`, below). The FP-semantics audit of `-O2` above applies unchanged.

**Results** (M5 Max, Bower Lake `hero1-dog-live_2026-10-11_11-20-20`, `headings8` uncapped, pass trace, 13–33 s,
no contention unless noted; `out/betA/perf/`; marginal busy ms/frame from `keytail.py`):

| Arm | GPU ms/frame | Busy | Main HDR pass | Full-screen `1120x2352` pass |
|---|---|---|---|---|
| 3× off (`=0`), 2 runs | 19.16 / 19.15 | 19.07 / 19.05 | 3.90 / 3.92 | 1.25 / 1.23 |
| 3× on, no keep-plain list, 2 runs | 18.84 / 18.58 | 18.77 / 18.54 | 3.43 / 3.33 | 1.40 / 1.39 |
| 3× on + keep-plain (default) | 18.52 | 18.46 | 3.46 | 1.18 |
| 2× off | 11.95 | 12.28 | 2.89 | 0.64 |
| 2× on, no list | 12.07 | 12.38 | 2.82 | 0.64 |
| 2× on + keep-plain (default), 2 runs | 12.02 / 11.89 | 12.34 / 12.21 | 2.85 / 2.89 | 0.55 / 0.55 |

After merging `macos` 0e3aa52 (shadow maps at native 1×) into the branch, the same 3× plan with one clean run per
arm gave:

- off: GPU 17.38 ms, busy 17.39, main pass 3.93, full-screen pass 1.34, 56.1 FPS;
- default: GPU 16.90 ms, busy 16.92, main pass 3.51, full-screen pass 1.30, 57.7 FPS.

That is **−0.49 ms (−2.8%)**. Parity at 3× (spin5, on vs off): idle median 0.60% / max 3.7%, inside the same-build
spread.

- **3×: −0.6 ms GPU (−3.2%), FPS 51.6 → 52.7**, almost all of it in the main pass (−0.45 ms, −12%).
- 2×: neutral within noise.

**Cold first launch.** The rebuild is background work on the existing pool; nothing waits for it. First 3× run after
the change:

- the pool drained at 3.15 s after storage init, against 1.91 s with `=0`;
- background warm-up took 1.50 s, against 1.48 s.

**Pinned target / cache-only (portable build).** Run A: `FABLE2_MSC_MIN_TARGET=apple7:15.0` with tools, which
optimized and cached the pinned libraries. Run B: `--cache-from` run A, the same pin,
`FABLE2_NS_ASSUME_NO_XCODE=1` and `FABLE2_NS_CACHE_TOOLCHAIN`. Results of run B:

- the log showed `come only from the cache`;
- 128+ libraries were installed from the cache;
- no xcrun ran;
- verdict ok.

Emulation pipelines of cached shaders are created optimized directly. Shaders missing from the cache stay plain, as
standard pipelines do in that mode. Keys are unchanged: the emulation path uses the same NS1 records.

### -O2 regressions among standard pipelines

**Open; not measured.**

- `0F99810F` shows that `-O2` can make an already-lean shader slower. The milestone 1/2 A/Bs were frame-level only
  (−15–19%), so a regression in a single standard-pipeline shader could hide inside that net gain.
- The per-shader check was prepared: a 1× capture, plain libraries dumped with `FABLE2_NATIVE_SHADERS=0`, and 81
  standard `-O2` libraries swapped back to plain in a cloned trace. It was stopped before any result because the
  replay exhausted memory (see Shader lab).
- The next try should use a smaller capture (1×, an indoor scene), and only on a machine with nothing else running.
- Candidates found that way go into `FABLE2_NS_KEEP_PLAIN` after one live A/B each.

## Shader lab (`tools/shaderlab`, diagnostic)

Bet A tooling (all diagnostic, default path untouched). Work folder `out/shaderlab` (`SHADERLAB_DIR`); everything in
it is game-derived and stays ignored.

- `cap.sh`: one 3× Bower Lake frame with `FABLE2_GPU_CAPTURE_*`, shader labels, `FABLE2_NS_DUMP_DIR`,
  `--dump_shaders` (ucode + disassembly) and the pass trace. `watchdog.py` closes the session (by id) if the game
  goes over 16 GB RSS or free disk drops below 150 GB.
  - The 3× trace is 8.1 GB, and the game's RSS peaks at 2.2 GB.
  - **The `gpudebug` replay service peaks at 22–28 GB RSS for a 3× trace.**
  - **Incident (2026-10-11):** the first version of `abrun.sh` overwrote its tools-folder variable with the trace
    argument, so the guard (`memwatch.sh`) never started. A 1× replay launched at about 25 GB free pushed the
    64 GB Mac to 72.6 of 73.7 GB of swap, and the replay processes had to be killed by hand.
  - `abrun.sh` now refuses to start when:
    - free memory is under `SHADERLAB_MIN_FREE_GB` (default 40);
    - swap used is over 4 GB;
    - the guard is missing or not running.
  - The guard terminates this lab's sessions when free memory drops under 8 GB, replay RSS goes over 24 GB, or
    swap goes over 8 GB.
  - Never replay while other games or agents run. Delete traces when done (they are game-derived and several GB).
  - **Since 2026-10-11 captures and replays run only under `tools/fable2_heavy.py` (guarded heavy-tool mode):**
    exclusive machine, external watchdog, 1× one-frame captures. `abrun.sh` refuses outside it and `cap.sh` (3×) is retired.
- `rank.py`, `agg_enc.py`, `dyn.py`: per-guest-shader cost from `/performance/shaders`. The pipeline's library
  labels give the hashes, and emulation pipelines are identified through `info @rpsN`.
- `plaincheck.py`: is each dumped PS library plain or already `-O2`?
- **Replay A/B with byte compare** (`abrun.sh`, `cmp.py`):
  - a `.gputrace` stores each library as a file holding the exact metallib bytes;
  - APFS-clone the trace (`cp -cR`) and overwrite that file with a variant (same entry point and ABI);
  - `abrun.sh` replays it (retrying the occasional empty 6 s profile), exports the perf tables, and fetches the
    raw scaled-resolve buffer after the main pass plus the main-pass colour PNG;
  - two base replays are byte-identical, so a byte compare against base is an exactness test.
  - `cmp.py` normalises a shader's cost by the unchanged shaders' cost, because other GPU work skews absolute
    replay times.
- `FABLE2_NS_LOG_CONSTS=<PS>[,…]` (+ `FABLE2_NS_LOG_CONSTS_N`, default 256), diagnostic: logs system, bool/loop and
  all fetch constants per draw for every pipeline kind. `consts.py` reports which dwords are invariant.
- `bake.py`: replaces loads of invariant constants in an AIR module with literals before `-O2` (exact for draws with
  equal live values; a product version needs a CPU-side guard).
  - MSC buffer mapping, by first use in the DXBC translator: CB0 float, CB1 system, CB2 fetch, CB3 descriptor
    indices, CB4 bool/loop.
  - The byte compare confirmed the mapping.
- `airtool.py`: extract / disassemble / `-O2` / re-link a metallib's AIR with the Metal Toolchain.

## Hot-shader specialization (`FABLE2_NS_SPECIALIZE`, 2026-10-11)

Built after the Bet A ceiling test (`docs/engine-options-20261011.md` §9.5). Code: `metal_ns_specialize.{h,cc}` (learn,
bake, guard, records), `metal_air_optimizer.cc` (`SpecializeMetallibAir`), `metal_pipeline_cache.cc` (background jobs,
NS2 records, specialized pipelines), `metal_command_processor.cc` (learn hook and guard in `IssueDrawImpl`).

**Idea.** A guest pixel shader reads the system, bool/loop and fetch constants at run time and decodes them per pixel:
texture formats, signs, exp-adjust, sizes, branch flags. For the hot shaders most of those bits never change. The
specialized variant has them as literals; a per-draw guard binds it only when the bound constants have exactly those
bits, else the normal (`-O2`, or plain for the keep-plain list) pipeline.

**1. Learn.** Normally automatic during play (**auto-learn**, below). A learn session
(`FABLE2_NS_SPECIALIZE_LEARN=1`) records every draw of every pixel shader instead:

- Per pixel-shader translation, keyed by the plain (converter) library's 128-bit hash, and per dword of the constant
  bytes bound for the pixel stage (system 116 dwords, bool/loop 40, pixel fetch 192): the first value, the bits that
  ever differed from it, and how many draws saw it.
- Written to `<shader cache>/…/4D5307F1/4D5307F1.ns2-learn.txt` every 10 s (merged with what was there; about 2 MB
  for four sessions). It is game-derived and per install; nothing goes into the repo.
- It costs CPU on the GPU Commands thread (every dword of every draw) and a synchronous file write every 10 s: a
  diagnostic mode, not for play.
- Plain library hashes include the resolution scale and the converter target, but the learn data is used across
  them (below), so one scale's data serves the others. The portable seed still needs a learn session with
  `FABLE2_MSC_MIN_TARGET` so its records carry the pinned keys.

**Auto-learn** (`FABLE2_NS_SPECIALIZE_AUTOLEARN`, default on; off in a learn session):

- Only listed shaders (`FABLE2_NS_SPECIALIZE_LIST`) that have no variant are sampled, and only when the toolchain is
  there. The background specialization marks a translation when it found no converged learn data; the draw path then
  samples it.
- Sampling: every 4th draw of such a translation (`FABLE2_NS_SPECIALIZE_AUTOLEARN_SAMPLE`) notes the bound bytes
  (first value, varying bits, draws), as a learn session does.
- Convergence rule: at least 64 samples (`FABLE2_NS_SPECIALIZE_AUTOLEARN_MIN_SAMPLES`) over at least 5 s, and no new
  varying bit in the last 48 samples and 3 s.
  - The record goes into the learn data, flagged converged, and the translation is specialized again in the
    background: generation, then the specialized pipeline on the next matching draw.
  - Sampling stops for that shader. From then on its guard is the learning signal (demotion, below).
- Records still sampling are kept as partial records (not used for variants) and merged with the next session's
  samples. So a shader seen only briefly converges over several sessions.
- The learn file is written by a detached background thread, at each convergence or demotion and every 15 s while
  something samples. The draw thread only copies the records still sampling (a few KB). File columns: `flags`
  (converged) and `demotions` after the draw counts. Files from learn sessions (without those columns) count as
  converged.
- **Append-only learn file** (`FABLE2_NS_LEARN_APPEND`, default on, 2026-10-12; `=0` rewrites one snapshot in place as
  before). The launcher publishes a normal session's grown cache to the live cache only when every changed file is an
  extension of the live one (`tools/fable2_dev_session.py` `publish_cache`). The rewritten learn file failed that
  check from the second session on, so nothing was published (archive segments, NS1/NS2 records, the learn data).
  Each save now appends the records whose text changed; every record is a complete snapshot and the loader keeps the
  last one per key. Past max(1 MB, 4× the compacted size) the compacted records go into a new generation file,
  `<title>.ns2-learn.<gen>.txt` (generation 0 is `<title>.ns2-learn.txt`); startup reads the highest generation and
  deletes older ones in its own cache. Checked by publishing 8 chained session caches (1×→2×→3×→…, including a forced
  compaction with `FABLE2_NS_LEARN_COMPACT_KB=50`) one over the other in scratch copies: every publish succeeded
  (`docs/frame-pacing-20261012.md`). Fable II Launcher: "Keep learned shader data across sessions".
- Cost: see Results ("Auto-learn").

**Scale-independent use.** A variant of plain library *L* is made from *L*'s own converged record merged with the
converged records of the same shader (ucode hash + modification) at other scales or converter targets:

- Fetch and bool/loop constants are the raw guest registers; system constants are filled from guest state.
  Comparing the 1×/2×/3× records of the hot shaders showed only scale-dependent system dwords differing for a
  scale reason: `textures_resolution_scaled` (0 at 1×).
- Others differed between records for reasons that have nothing to do with the scale (unused user-clip-plane
  dwords holding stale data, the alpha-test reference); merging makes those varying.
- The scale-dependent system dwords (`ndc_*`/point sizes, `textures_resolution_scaled`, the `*_scaled` EDRAM
  pitch/bases) come only from *L*'s own record. Without one they are never baked.
- So learning at one scale specializes all scales from the next launch. The guard still compares every baked bit.

**Guard-miss tracking and demotion** (`FABLE2_NS_SPECIALIZE_DEMOTE_PCT`, default 5; 0 = never):

- Each variant counts checks and misses in windows of 1024 checks. A miss also ORs the guarded bits that differed
  into the variant's miss mask.
- More than 5% misses in a window demotes the variant: it is not bound again this session (normal pipeline, no
  guard work). The miss bits are written into the learn record as varying, with the demotion count.
  - The next session's learn hash differs, so the variant is regenerated without those bits.
  - A shader demoted 3 times is no longer specialized until the learn file is deleted.
- A demoted variant with no learn data behind it (from the portable seed) starts sampling instead, so the next
  session has data.
- A variant is never replaced within a session (pipelines built for it stay tied to it). Regeneration happens at
  the next launch.

**2. Generate** (background pool, utility QoS, after the optimize jobs):

- Candidates: the built-in list of hot pixel shaders (`FABLE2_NS_SPECIALIZE_LIST`, below) that have learn data.
- `metal-opt -O2 -S` of the shader, then the bake (C++ port of `tools/shaderlab/bake.py`), then `metal-opt -O2` and
  `metallib`. 50–240 ms per shader; 22 shaders in about 2.7 s summed, once.
- The bake finds the constant buffers by their slot in the top-level argument buffer (byte 80 system, 96 bool/loop,
  176 pixel fetch), not by MSC's `CBn` names, which follow first use. It follows bitcast / constant-index GEP chains
  to every `i32`/`float` load at a constant offset, then:
  - **folds the uses**: `air.extract_bits.{u,s}`, `and` with a constant, `lshr`/`ashr` by a constant and `trunc` of an
    invariant bit range become literals. `metal-opt` does not constant-fold `air.extract_bits` on its own, so this
    is what removes the decode (AF1E16EE: 1579 IR lines after `-O2`, against 2510 with Bet A's whole-dword bake and
    3430 for `-O2` alone);
  - replaces a whole load only when it has another kind of use and all 32 bits are invariant.
- The guard is the set of (buffer, dword, bit mask, value) actually folded: 6–39 dwords per shader.
- Records in the artifact store, beside NS1:
  - library `NS2` (`0x4E530002`): key = plain library hash + size, toolchain hash (`metal-opt --version`), hash of the
    guarded values;
  - index (`0x4E530003`): key = plain library hash + toolchain; payload = the guard list and a hash of the learn data
    it came from. A changed learn file (or bake version) regenerates the variant. Without a learn file (portable
    build) the index alone finds the variant.
- A shader with nothing to bake gets an empty index (not retried).

**3. Guard** (`IssueDrawImpl`, after `PrepareDrawConstants`, where the bound constant bytes are final):

- One atomic load of the translation's variant per draw; null for all but the specialized shaders.
- `(bound dword & mask) == value` for each guarded dword, reading the payload bytes actually bound
  (`current_payload_*`), so the specialized code only ever runs with the inputs it was built for. Direct compares, not a
  hash: no collision can misbind.
- On a match the draw binds the specialized pipeline: standard pipelines get it on the handle (`ns_spec_state`),
  emulation pipelines on their `NsEmuSwap` (`spec`). It is built in the background on the first matching draw
  (`.ns2` binary archive set for standard pipelines); until then the normal pipeline draws, no wait.
- `FABLE2_NS_SPECIALIZE_VERIFY=1` re-checks every hit against the guest registers and system constants (misbind
  counter). The first miss of each variant is logged with the dwords that differ
  (`FABLE2_NS_SPECIALIZE_LOG_MISSES=N`).
- Log: `[ns-spec] guard: checks, hits, misses, hit without pipeline yet, verified, misbinds; variants (hits/checks)`
  every 10 s, plus generation and pipeline progress lines.

**Why bit masks** (bug-patterns row 60): the first version guarded whole dwords. In about 2 of 10 Old Tomb sessions every
main-pass material variant missed. The miss log showed system dwords 46/47 (`texture_swizzled_signs`): the bytes of
fetch constants a draw does not use keep values from earlier draws, so the whole dword depends on draw history. Fetch
dword 1/5 address bits have the same problem. With per-bit learn and masked guards: 0 misses at 1×/2×/3×, except for
legitimate texture-size changes.

**Per-shader A/B** (`FABLE2_PASS_TRACE_PS=1` tags each pass of the pass trace with its pixel shaders, so single-shader
passes give per-shader numbers):

| PS | Where | Normal → specialized (marginal ms/frame, 3×) |
|---|---|---|
| `44B567635EB10620` (the 5.5k-instruction spiller) | its own passes | Bower 0.096 → 0.035 (−64%), Old Tomb 0.224 → 0.071 (−68%) |
| `0F99810F072BF0FC` + `1B4B5AE12EF270CA` | full-screen `1120x2352` pass | 1.31 → 0.93 (−29%) |
| `0542FFDF…`, `686DE15F…`, `577CB526…` | `c0:90/1280x2048` | −24% Bower, −10% Old Tomb |
| `06547412E4B72251` | 80/160-wide strips | −5 to −6% |
| Old Tomb materials (`356E4E7B`, `28E20286`, `CD2CE6D4`, `648B965C`, `F2F103F1`, `44B56763`) | main HDR pass | 3.59 → 2.19 (−39%) |
| Bower materials (`AF1E16EE`, `7CD57B81`, `648B965C`, `04F23292`, `F1EB2497`, …) | main HDR pass | 3.37 → 3.26 (−3%; ±0.15 run to run). Skipping them lost about 0.24 ms in total, so they stay |
| `4BE4344C27016E04` | its own pass | +5–6% at 1×/2×/3×: **built-in skip list** |

Single-shader passes of shaders that are not specialized moved by 0–2% between the arms (the noise floor).

**Results** (M5 Max, `headings8` uncapped, `f2diag gpu` 13–33 s, one clean run per arm, no contention, variants from a
warm cache; `out/shaderspec/runs/F-*` in the slot):

| Scene, scale | GPU ms off → on | FPS off → on | GPU Commands CPU ms/frame off → on |
|---|---|---|---|
| Bower Lake 3× | 16.70 → **15.59** (−6.7%) | 59.2 → 63.5 | 8.71 → 8.57 |
| Bower Lake 2× | 11.35 → 10.62 (−6.5%) | 87.3 → 93.5 | — |
| Bower Lake 1× | 8.30 → 8.02 (−3.4%) | 118.3 → 120.4 | 8.36 → 8.47 |
| Old Tomb 3× | 13.22 → **11.01** (−16.7%) | 74.6 → 89.6 | 5.50 → 5.53 |

- 3× Bower Lake drops below the 16.67 ms of 60 FPS (mean; p95 18.0 ms).
- An earlier run set (whole-dword guards) gave 3× 16.65 → 15.94, 2× 11.27 → 10.90, 1× 8.24 → 7.93, with GPU Commands
  8.42 → 8.37 ms at 1×. Taken together, the guard's CPU cost is within run-to-run noise (±0.15 ms/frame). It does
  about 300–400 checks per frame, most of them a handful of masked compares.
- Guard hit rate in these runs: 99.98% (Bower 1×: 1,367,850 of 1,368,064), 0 misbinds in every verify run.

**Exactness.**

- A literal replaces a value only when the guard proved it equal for this draw (bits outside the guarded mask stay
  live loads), and the folded uses implement the AIR semantics (`extract_bits` offset/width, signed or not). The
  `-O2` FP audit above applies; baking can enable more constant folding of float math with those literals.
- Live parity, `spin5`, linear-light per-region means, against the same build's off-vs-off noise floor:

  | Run | off-a vs off-b (idle median / max) | off vs on (idle median / max) |
  |---|---|---|
  | Bower Lake 3× | 0.79% / 6.8% | 0.07% / 1.4%, 0.61% / 6.1% |
  | Old Tomb 3× | 1.12% / 2.1% | 0.96% / 2.0%, 0.05% / 2.0% |
  | Bower Lake 1× | 0.12% / 1.3% | 0.09% / 1.9%, 0.09% / 1.0% |

  (`idle2` is dominated by where the camera stops after the spin under parallel runs; on is inside off-vs-off there too.)
- Dark-dip detector (dog fur): on 5 / 4 / 4 flagged frames against off 6–7 / 5–7 / 3 (Bower 3×, Old Tomb 3×, Bower 1×);
  blue transients 0 everywhere. Dog fur, torch glow, the green interact outline, bloom, shadows and the HUD are the
  same by eye.
- Verify mode (`FABLE2_NS_SPECIALIZE_VERIFY=1`) on every parity, area and F5 run: 0 misbinds over about 1.3 M hits.
- Not done: a GPU-side render-both-ways compare of a sampled draw. It would need the draw re-encoded into a scratch
  copy of the EDRAM render target, which is not cheap here; GPU captures and replays are banned.

**Validation set** (final build, specialization on, warm variant cache): `spin5` 1×/2×/3× ok; area ok; F5 ok; boot ok;
bootloop 3/3 pass; 0 AVs; skip-intro guard ok (`skip_intro_missing` 0); `pgrep` 1 after every run.

**Portable seed.** Run A: learn with `FABLE2_MSC_MIN_TARGET=apple7:15.0`. Run B: generate (tools). Run C: `--cache-from`
run B, the same pin, `FABLE2_NS_ASSUME_NO_XCODE=1`, `FABLE2_NS_CACHE_TOOLCHAIN`. Run C logged `come only from the
cache`, 17 of 17 variants from cache, 31 specialized pipelines, 0 misses. `tools/fable2_portable.py` keeps NS2 records
with the NS1 toolchain filter and reports `specialized_pixel_shaders` (43 in a test seed). The seed needs a learn
session with the pin; its warm-up sessions do not learn.

**First launch after a change.** Variant generation runs on the existing pool after the optimize jobs; nothing waits.
Cold (new learn data): background work done 0.8–0.9 s after storage init, specialized pipelines by about 6 s (they are
built on the first matching draw, during gameplay). Warm: variants load from the store in under 0.1 s.

**Specialized pipeline archive** (`FABLE2_NS2_ARCHIVE`, default on, 2026-10-12). Specialized standard pipelines have
their own binary archive set, `<title>.rtv-bindless.ns2.*.seg*.binarchive`. Before this switch it was written only by
the 30 s periodic archive save. The app hard-exits at window close without the renderer shutdown, and specialized
pipelines are built on their first matching draw (1–20 s into play), so a session shorter than about 30 s past its last
specialized pipeline saved none, and the next launch compiled them all again (`loaded 0 segment(s)` for the `.ns2`
set in every chained test run).

- **Settle save.** Once no specialized pipeline has been added to the `.ns2` set for 10 s
  (`FABLE2_NS2_ARCHIVE_SETTLE_MS`), only that set is saved, on the existing background save thread. The periodic
  schedule of the other sets is not moved.
- **Window-close flush.** `OnWindowCloseRequested` (`src/core/fable_2_app.h`) calls the plugin's exported
  `fable2_pipeline_archive_flush` (found with `dlsym`), which saves every dirty archive set on a helper thread. The
  close waits at most 500 ms (`FABLE2_NS2_ARCHIVE_QUIT_MS`). A save cut off leaves only a `.tmp-<pid>` file, which the
  next launch removes; segments are written to a temporary file and then renamed. Measured: 6–77 ms. This also keeps
  plain and NS1 pipelines compiled after the last periodic save.
- Write-back: only new segment files, so `publish_cache` accepts them. Checked with three chains published one over
  the other in scratch copies (seed → session → warm session; on, smoke, off): all succeed
  (`out/validation/ns2archive-20261012/wbchain-ns2.txt`).
- Side effect: the extra close-time segment moves the plain/NS1 sets to the 8-segment compaction sooner. A compacting
  launch writes one `.full` segment, and the next launch deletes the older ones, which `publish_cache` then removes.
- Logs: `[ns2-archive] specialized pipeline #N at T s …: archive hits H, compiled C (ms), build CPU`,
  `settle save`, `window close: saved … in X ms (limit 500 ms; …)`.

Results (`out/validation/ns2archive-20261012/`, M5 Max, Bower Lake 3×, spin5 at 60 FPS, 2 chains per arm, no
contention). Each chain: launch 1 from a seed with no `.ns2` segments, then launch 2 (warm) from launch 1's cache.

| Warm launch 2 | `.ns2` segments loaded | archive hits / compiled | compile ms | build CPU ms | last specialized pipeline | process CPU at ~19.9 s | 1% low / p99 / max |
|---|---|---|---|---|---|---|---|
| off (`=0`) | 0 / 0 | 0/29, 0/28 | 0 / 0 | 7.2 / 6.7 | 15.60 / 15.53 s | 30.39 / 30.54 s | 52.8 / 18.63 / 19.2, 52.5 / 18.54 / 19.2 |
| on | 1 / 1 | 29/0, 27/3 | 0 / 52 | 5.3 / 5.8 | 15.49 / 15.57 s | 30.66 / 30.57 s | 53.4 / 18.45 / 18.9, 53.1 / 18.45 / 19.1 |

- The mechanism works: the specialized pipelines now load from the archive (27–29 of 29–30). The 2–3 compiled ones
  in chain 2 were new pipelines (new draw combinations), saved by that session's settle save.
- **The measured gain is negligible on this Mac.** Compiling a specialized pipeline cost under 1 ms each with the
  archive off, because Metal's own system shader cache (outside the app's cache) already held them. The build CPU
  difference is about 2 ms per session, and process CPU and pacing are equal within noise. Time until all variants are
  ready (about 15.5 s) is set by when their draws first appear in the camera spin, not by build time.
- The archive matters when Metal's system cache misses: after it is purged, on another Mac with a copied cache, or
  after a cache move. Then each pipeline is a full compile in the compiler service, about 15 ms each for comparable
  transfer pipelines (`docs/frame-pacing-20261012.md` §3), so roughly 0.4 s of background compile per launch for
  about 30 pipelines. It never hit the draw path either way: until the pipeline exists, the normal one draws.
- Correctness, verify mode (`FABLE2_NS_SPECIALIZE_VERIFY=1`) from a warm cache: spin5 3× 155,503 hits verified, spin5
  1× 155,462, F5 3× 147,213, all with 0 misbinds and 0 guard misses. An archive-loaded pipeline is the same compiled
  binary for the same descriptor, so image parity holds by construction. Pacing is equal within noise (table).
- Validation (defaults): spin5 1×/3×, boot, F5 (saved), bootloop 3/3: verdict ok, 0 AV, `skip_intro_missing` 0.

Old learn generations: after a learn-file compaction the live cache keeps the superseded `<title>.ns2-learn[.N].txt`
(`publish_cache` deletes only archive segments that the session dropped). This is harmless: the game reads only the
highest generation and deletes the older ones in its own session copy, and the file is at most about 1 MB. Removing
it from the live cache would need a change to `tools/fable2_dev_session.py` (`publish_cache`), so it is left until
the maintainer approves that change.

**How to use it.** On by default, including auto-learn: play, and the hot shaders of the scenes you play are
specialized during the first session in them and from launch afterwards. To learn everything at once (diagnostic,
or the portable seed):

```sh
# learn session (all shaders, every draw; test sessions write their own cache copy)
python3 tools/f2diag.py run sweep learn3 --sweep-plan headings8 --no-perf-lock \
  --env FABLE2_RES_SCALE=3 --env FABLE2_NS_SPECIALIZE_LEARN=1
# later launches from that cache generate the variants in the background and use them
```

Deleting `<shader cache>/…/4D5307F1/4D5307F1.ns2-learn.txt` resets the learning (stored variants without learn data
are still used).

## Converter target pinned by default (2026-10-12)

**What changed.** Metal Shader Converter builds its output for a minimum target. That target used to be this Mac's
GPU family and its exact macOS version (`SetMinimumTarget`, `metal_pipeline_cache.cc`). Every stage-compile record and
every NS1/NS2 record is keyed by that output, so each macOS update (and each other Mac) threw the whole translated and
optimized cache away. Every build now pins the target to `apple7:15.0` (M1 and later, macOS 15 and later) unless told
otherwise:

- `[graphics] shader_cache_target = "portable"` (default) | `"native"` (this Mac, the old behaviour) |
  `"<family>:<macOS>"`, in `fable2_config.toml`. `src/core/fable2_graphics_config.h` exports it as
  `FABLE2_MSC_MIN_TARGET`.
- `FABLE2_MSC_MIN_TARGET` wins over the config: `portable`, `native`, or `<family>:<macOS>` (families
  `apple6`–`apple10`, `metal3`). An unparsable value logs a warning and uses the default.
- Fable II Launcher: "Portable shader cache (survives macOS updates; off = tied to this Mac)", on by default (passes
  `portable` / `native`).
- Log: `Metal shader converter: minimum target pinned to apple7 macOS 15.0 (default; native would be family 1010 macOS
  27.0.1)`, or `minimum target native, …`.

The driver still compiles every library for the real GPU. The pinned output depends only on the build: DXIL from the
bundled translator and dxilconv, the pinned target, fixed options and the bundled converter (`docs/portable-build.md`,
"Why the pin"). The stage key in a miss line shows it: `min_target=1007/0:<hash of "15.0">`.

**What survives a macOS update now, and what doesn't:**

| Cache part | Keyed by | After a macOS update |
|---|---|---|
| DXIL records (`.metal.artifacts`, kind 4, converter version 1) | DXBC + translator options | survive (never had an OS input) |
| Stage-compile records (converter output, kind 3) | DXIL + converter target + options | **survive** (before: all missed) |
| NS1 optimized libraries, NS2 specialized libraries and their index | plain library hash + `metal-opt --version` | **survive** (before: all missed). An Xcode / Metal Toolchain update that changes `metal-opt --version` still re-optimizes them, independent of macOS |
| Learn data (`.ns2-learn*.txt`), pipeline lists (`shareable/*.xsh`, `*.metal.xpso`), transfer keys (`.xfer-keys.txt`) | game content only | survive |
| Binary archives (`*.os<X.Y.Z>.metal.seg*.binarchive`) | GPU code from the OS's Metal compiler; the file name carries the OS version | **invalidated**: the first launch after an update compiles every warm-up pipeline from the cached libraries (driver compile only, no translation or optimization) into new segments. The old-OS segments stay on disk; nothing deletes them |
| Metal's system shader cache (run-time MSL: EDRAM transfer pipelines) | OS | rebuilt by Metal; `FABLE2_XFER_PREWARM` builds them in the background at startup (`docs/frame-pacing-20261012.md`) |

**Another Mac.** The records stay valid on any Apple Silicon Mac with macOS 15+, but the folder is named after the GPU
(`cache/shaders/local/metal/<GPU name>/<abi>/`). To reuse a cache on another Mac, copy the `<abi>` folder into that Mac's
`<GPU name>` folder (the portable launcher does this for its seed); the shareable lists need no change.

**One-time migration of an existing cache.** Records keyed by the native target are not converted; they miss once and
the pinned ones are written beside them in the background. Measured on a read-only clone of the player's live cache
(2,537 records: 838 DXIL, 844 stage-compile, 700 NS1, 155 NS2; all keyed Apple10 / macOS 27.0.1), Bower Lake 3×
(`hero1-dog-live_2026-10-11_11-20-20`), `spin5` at 60 FPS, the first launch after the rebuild, no contention
(`out/validation/f2diag/cachepin-mig-*`, `out/validation/cachepin-20261012/`):

| | native (same build, control) | pinned, first launch (migration) | pinned, next launch |
|---|---|---|---|
| Pipeline warm-up | 0.27 s: 285 archive hits, 113 compiled | **2.40 s**: 173 hits, 379 compiled (every stage-compile record misses) | 0.32 s: 394 hits, 4 compiled |
| Title screen up, after process start | 1.49 s | **3.67 s** | 1.57 s |
| Level load begins (trace time) | 2.82 s | 5.02 s | 2.90 s |
| Optimizer pool | 183 PS from cache, 0 ms | 183 PS re-optimized in the background: **17.9 s of utility-QoS CPU**, drained 6.2 s after storage init (about 2.5 s into gameplay), 0 fallbacks, 0 swap failures | 183 from cache |
| Specialized (NS2) pipelines ready | 1.2 s after storage init | 8.1 s (variants regenerated from the existing learn data) | 1.2 s |
| `spin5` pacing: 1% low, p99, max, frames > 33 ms | 50.9, 19.12, 20.3 ms, 0 | 53.1, 18.66, 19.1 ms, 0 | 53.1, 18.70, 18.9 ms, 0 |
| `[hitch]` lines | loading (`idle`) only | the same loading lines, no `pso_*`/`xlate` | the same |

So the migration costs about 2.2 s more before the title screen, once, plus about 18 s of background CPU while the
first seconds of gameplay draw with plain (unoptimized) shaders, with no gameplay hitch. Warm-up blocks the title
screen, which is why its 2.1 s shows there. The superseded native records stay in the append-only artifact store
(about 63 MB in that cache): `publish_cache` accepts only appended files, so compacting the store needs a
`publish_cache` change first (maintainer approval).

**Cost of the pin, same build, warm caches** (each arm's cache warmed at 1× and 3× first; `headings8` uncapped,
`f2diag gpu` 13–33 s, no contention; `out/validation/f2diag/cachepin-h8-*`):

| Scale | pinned: GPU ms/frame, FPS | native: GPU ms/frame, FPS |
|---|---|---|
| 1× | 8.08, 121.5 | 8.07, 121.4 |
| 3×, pair a (pinned first) | 15.36, 64.2 | 15.05, 65.6 |
| 3×, pair b (native first) | 15.26, 64.7 | 15.29, 64.5 |

Mean at 3×: 15.31 vs 15.17 ms, inside the native arm's own run-to-run range (15.05–15.29). With the 2026-10-11 1×/2×
turn30 A/B (`docs/portable-build.md`): the pin is free.

## First launch: cold Metal system cache (2026-10-13)

The cross-Mac benchmark kit's true first install (M3 Pro, `benchmark-results/M3-Pro-Mac15_6-2026-10-13_02-28`) showed
what a launch with an empty macOS Metal cache (`$(getconf DARWIN_USER_CACHE_DIR)<bundle id>/com.apple.metal`) costs:

- 12.3 s of "Metal startup: preparation";
- transfer prewarm starting only after that;
- 0.5–0.7 s each of draw-path `pso_xfer` and `pso_rt` on load frames, and one `pso_xfer` gameplay hitch;
- 7.0 s / 2.9 s of preparation on the first 2× / 3× launch.

Every create of the backend's own pipelines, and every install of a cached game-shader library, is a driver
compile there (10–500 ms). Warm, the same work takes milliseconds.

Changes (all default on, Fable II Launcher toggles in Performance > Startup and CPU):

| Switch | What |
|---|---|
| `FABLE2_STARTUP_PARALLEL` (`metal_startup_parallel.h`) | SetupContext's own compute pipelines (80 texture loads, resolves, host depth stores, direct host resolves, 15 EDRAM dumps) and the cached translation installs of the startup warm-up run on the GCD worker pool. Each item writes its own slot, so the pipelines are the same objects as before |
| `FABLE2_XFER_PREWARM_EARLY` (env only; `=0` restores the old order) | `[xfer-prewarm]` starts before shader storage init instead of after the warm-up preparation. Its first thread compiles the shared transfer-clear library (`pso_xclr`). The built-in list grew from 101 to 134 keys: the union of every recorded key file. Draw-path builds and new keys are logged (`[xfer-prewarm] draw-path build`, `new key`) |
| `FABLE2_RTT_PREWARM` | `[rtt-prewarm]`: the 39 (1×) / 78 (scaled) fused resolve pipelines (`pso_rt`) are built on two utility threads from SetupContext on. The draw path takes them by name, or builds one itself if it isn't ready yet (logged) |
| `FABLE2_WARMUP_THREADS=<total>` | The launcher pins `metal_pipeline_creation_threads` to 2. While the startup warm-up queue is non-empty, extra creation threads help; they exit once it is empty. The total is one per performance core (`hw.perflevel0.physicalcpu`), clamped to 2–6: base M1 4, M3 Pro 5, M5 Max 6. `0` adds none |

Install warm-up (`tools/fable2_cache_warm.py`, setup app): after the new-game session at the player's scale, one
short title-screen session per other scale (`--scales 1,2,3`, the default) builds that scale's warm-up pipelines,
fused resolves and optimized pixel shaders into the archives and Metal's system cache. The portable seed already
held 1×/2×/3× translations. Binary archives are GPU-, OS- and Mac-specific, so they are never shipped.

### Results (M5 Max, macOS 27.0.1, 2026-10-13)

Setup:

- The slot app was re-identified as `org.fable2recomp.metal.coldcache`, so only its macOS Metal cache was cleared.
- The seed cache was a clone of the live cache without `.binarchive` files, `xfer-keys` or the learn file, like the
  kit's seed.
- spin5 at 60 FPS, Bower Lake save, `FABLE2_HITCH_RELATIVE=0`.
- Each 1× run cleared the OS cache. Each 2× and 3× run continued from the previous run's cache and OS cache, as the
  kit does.
- Gameplay = the flight recorder's `loading = 0`.
- Evidence: `out/validation/cold-cache/` (KEEP), labels `base-*` and `d6-*` / `h-*` / `m4-*` / `A-*` / `B-*`.

| Run | SetupContext | Preparation | Transfer prewarm done | Gameplay at | Draw-path pso ms (whole run) | pso hitches in gameplay |
|---|---|---|---|---|---|---|
| Cold 1×, before | 4.86 s | 12.57 s | 14.72 s | 18.8 s | xfer 477, rt 695, xclr 40, wait 72 | 120 ms (`pso_rt`) |
| Cold 1×, after (6 creation threads) | 0.46 s | 6.30 s | 2.90 s | 10.5 s | xclr 5 | none |
| Cold 1×, after, 4 threads (base M1 value) | 0.48 s | 6.30 s | 2.86 s | 10.4 s | xclr 6 | none |
| Cold 1×, after, no extra threads | 0.48 s | 6.31 s | 2.88 s | 10.4 s | wait 422 | 4, worst 153 ms (`pso_wait`) |
| Cold 1×, after, warm-up held on the loading screen (4 threads) | 0.48 s | 8.87 s | 2.92 s | 12.9 s | xclr 5 | none |
| First 2×, before | 3.07 s | 6.73 s | 6.88 s | | rt 727 | |
| First 2×, after | 0.40 s | 3.47 s | 0.56 s | 7.4 s | none | none |
| First 3×, before | | 3.71 s | 3.85 s | | none | |
| First 3×, after | | 3.04 s | 0.14 s | 7.0 s | none | none |
| First 2× after install warm-up: 1× only / 1×,2×,3× | | 1.45 s / 0.16 s | | 5.4 / 4.0 s | | |
| First 3× after install warm-up: 1× only / 1×,2×,3× | | 1.02 s / 0.16 s | | 4.9 / 4.1 s | | |

Warm launch, all switches on vs all off: preparation 0.33 s / 0.35 s, no difference.

Why the extra threads, and not holding the loading screen until the queue drains:

- On a cold launch the warm-up compiles ~420 pipelines, about 27 s of driver time summed.
- On 2 threads that finishes about 3.5 s after gameplay starts. Draws then wait for the pipelines they need
  (`pso_wait` hitches).
- Holding the loading screen until the queue drains removes the hitches but costs 2.5 s before gameplay. Extra
  threads remove them at no cost.
- The threads mostly wait on Metal's compiler service. They exist only while the startup queue is non-empty, and
  they stop at the performance-core count, so the efficiency cores stay free for the game.
- A slower Mac compiles more slowly. If its queue is still non-empty at gameplay, only the pipelines a draw needs
  wait, rather than the whole remaining queue.

Cost of the 2× and 3× install sessions:

- time: 28.5 s and 27.7 s on top of the 120 s new-game session, 176 s in all;
- cache: 47 MB instead of 30 MB.

On a base M1 expect roughly twice the time. The sessions are background and muted, like the first.

Left on the first launch: about 5.8 s of preparation (cold 1×, OS cache empty) and about 3 s (first 2×/3× from the
kit seed). Both are geometry/tessellation emulation pipelines, built serially in `PrewarmStoredPipelines`. They are
not in a binary archive ("emulation, no archive"), so only the OS cache helps them. Parallelizing them needs locking
in the geometry stage caches: a follow-up.

The Bower Lake `pso_emu`/`xlate` hitches after the install warm-up (`A-*`, `B-*`) come from the area. Its shaders
are not in the prologue-based warm-up, at any scale.

## Switches and tools

| Switch | Default | What |
|---|---|---|
| `FABLE2_NATIVE_SHADERS` | on | Optimized guest pixel shaders, built in the background and swapped in (needs the Metal Toolchain; otherwise logs once and stays plain). `=0` disables |
| `FABLE2_NS_TOOL_DIR=<dir>` | unset | Take `metal-opt`/`metallib` from `<dir>` instead of `xcrun` (testing: `/nonexistent` = toolchain missing) |
| `FABLE2_NS_ASSUME_NO_XCODE=1` | off | Testing: behave as on a Mac without Xcode or the command-line tools. The lookup never spawns `/usr/bin/xcrun` when no developer directory exists, because the stub would open Apple's install dialog (`docs/portable-build.md`) |
| `FABLE2_NS_CACHE_TOOLCHAIN=<16 hex>` | unset | Without tools: use cached optimized libraries of that toolchain (cache-only, nothing optimized). Set by the portable build's launcher |
| `FABLE2_MSC_MIN_TARGET=portable\|native\|<family>:<macOS>` | `apple7:15.0` (2026-10-12) | Metal Shader Converter's minimum target. Pinned by default so translated and optimized shader cache keys survive macOS updates and match across Macs; `native` = this Mac's GPU family and exact OS version (before 2026-10-12). Overrides `[graphics] shader_cache_target`; Fable II Launcher "Portable shader cache". Free on the M5 Max at 1×/2×/3× ("Converter target pinned by default") |
| `FABLE2_SHADER_LABELS=1` | off | Trace-only: pipeline/library labels with guest shader hashes |
| `MTL_CAPTURE_ENABLED=1 FABLE2_GPU_CAPTURE_PATH=… FABLE2_GPU_CAPTURE_AT_S=… [FABLE2_GPU_CAPTURE_FRAMES=n]` | off | Trace-only: in-process GPU trace for `gpudebug` |
| `FABLE2_NS_DUMP_DIR=…`, `FABLE2_NS_OVERRIDE_DIR=…` | off | Trace-only: dump / replace a shader's metallib by `<vs|ps>_<hash>_<mod>.metallib` |
| `FABLE2_NS_LOG_FETCH=<PS hash>` | off | Trace-only: log the fetch constants of that pixel shader's first 16 draws |
| `FABLE2_NS_EMU_PIPELINES` | on | Geometry/tessellation-emulation pipelines also get the optimized pixel shader (rebuilt in the background, swapped atomically). `=0` restores the old plain-forever behaviour |
| `FABLE2_NS_KEEP_PLAIN=<hash>[,…]` | built-in list (`0F99810F072BF0FC`) | Pixel shaders that stay plain because `-O2` measured slower; `none` optimizes all |
| `FABLE2_NS_LOG_CONSTS=<PS hash>[,…]`, `FABLE2_NS_LOG_CONSTS_N` | off | Diagnostic: log system, bool/loop and fetch constants per draw (all pipeline kinds) |
| `FABLE2_NS_SPECIALIZE` | on | Hot-shader specialization: specialized variants of the listed hot pixel shaders, generated in the background from learn data, bound per draw only when the guard matches. Needs `FABLE2_NATIVE_SHADERS`. `=0` off |
| `FABLE2_NS_SPECIALIZE_LEARN=1` | off | Learn session: record per-bit constant invariance per pixel shader into the cache's `.ns2-learn.txt` |
| `FABLE2_NS_SPECIALIZE_AUTOLEARN` | on | Auto-learn during play: sample 1 in N draws of listed shaders without a variant until the statistics converge, then specialize in the background. `=0` off. Fable II Launcher: "Learn hot shaders while playing" |
| `FABLE2_NS_LEARN_APPEND` | on | Learn file as an append-only journal with generation files, so the launcher's cache write-back keeps working. `=0` rewrites it in place (blocks the write-back from the second session on). `FABLE2_NS_LEARN_COMPACT_KB=<n>` (testing) compacts past n KB |
| `FABLE2_NS_SPECIALIZE_AUTOLEARN_SAMPLE=N` | 4 | Auto-learn samples every Nth draw of a shader |
| `FABLE2_NS_SPECIALIZE_AUTOLEARN_MIN_SAMPLES=N` | 64 | Samples needed before a shader can converge (testing: a huge value keeps it sampling all session) |
| `FABLE2_NS_SPECIALIZE_DEMOTE_PCT=N` | 5 | Demote a variant whose guard misses more than N% of a 1024-check window (0 = never) |
| `FABLE2_NS_SPECIALIZE_LIST=<hash>[,…]` \| `all` | built-in list (18 hot shaders) | Which pixel shaders are specialized |
| `FABLE2_NS_SPECIALIZE_SKIP=<hash>[,…]` | built-in list (`4BE4344C27016E04`) | Never specialize (measured not faster); set to empty to clear |
| `FABLE2_NS_SPECIALIZE_MIN_DRAWS` | 16 | Learned draws needed before a buffer's dwords count |
| `FABLE2_NS_SPECIALIZE_FLOATS=0` | on | Bake only `i32` loads (A/B for float constant folding) |
| `FABLE2_NS_SPECIALIZE_VERIFY=1` | off | Re-check every guard hit against the guest registers (misbind counter) |
| `FABLE2_NS_SPECIALIZE_LOG_MISSES=N` | 1 | Log the first N guard misses per variant with the differing dwords |
| `FABLE2_NS2_ARCHIVE` | on | Save the `.ns2` archive set (specialized pipelines) 10 s after the last add, and every dirty archive set at window close (at most 500 ms), so the next launch loads them instead of compiling them. `=0`: periodic saves only, as before. Fable II Launcher: "Save specialized shader pipelines for the next launch". `FABLE2_NS2_ARCHIVE_SETTLE_MS`, `FABLE2_NS2_ARCHIVE_QUIT_MS` tune it |
| `FABLE2_PASS_TRACE_PS=1` | off | Trace-only (with `FABLE2_GPU_TIME_TRACE=1`): each pass-trace row lists its pixel shaders (`ps:` hash high halves) |

## Next steps (not done)

1. ~~First-launch cost~~ (done in milestone 2).
2. Run on an M3 Pro and other GPUs. The gain is in instruction issue, so it should carry over. Also
   check the background pool's CPU time on a smaller machine.
3. Playtesting across several sessions and areas, now that it is on by default.
4. Longer term: the XeniOS native MSL emitter, if the toolchain dependency is unwanted.
5. Hot-shader specialization: auto-learn is done. Still open: extend the list from pass-trace attribution in other
   areas; regenerate a demoted variant within the session (needs per-variant pipeline slots).
