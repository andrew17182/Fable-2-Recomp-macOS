# Engine options for 3× at 60 FPS (and base M1–M3) — 2026-10-11

This is a read-only study. Nothing was built, launched or edited apart from this document. It ranks the ways to make
the renderer much faster, from "stay a GPU emulator and go further" up to "write a new engine for drawing", and
gives a path with go/no-go gates.

**Goal (maintainer):** hold 60 FPS at 3× on the M5 Max (3840×2160 guest output, the resolution the Xbox Series X renders
Fable II at). Also run well on base M1–M3 Macs. Preservation comes first: the faithful path is the default, and
anything that changes the look is a labelled extra that comes after it.

**Evidence labels used below:**

- **measured**: a number from an existing doc or run in this repo;
- **estimate**: derived from measured numbers by a stated model;
- **speculative**: an informed guess with no measurement behind it. Treat these as hypotheses for the experiments
  in §7.

## Contents

0. Where we stand (the gap)
1. Where the GPU time goes, and how far it is from ideal
2. What the game's renderer looks like from the recompiled code (new findings)
3. How other projects did it (Xenia, XeniOS, Unleashed Recompiled, RT64)
4. The options, one by one
5. Ranked comparison table
6. Recommended path with gates
7. First experiments for the top two bets (plus the HLE census)
8. Sources
9. Bet A result: shader ceiling test (2026-10-11)

---

## 0. Where we stand (the gap)

| Scene, M5 Max | 1× | 2× | 3× | Source |
|---|---|---|---|---|
| Old Tomb, uncapped turn30 | ~190 FPS, GPU ~5.0 ms | ~116 FPS, GPU ~8.5 ms | **~65 FPS, GPU mean ~15.0–15.4 ms, p95 ~20 ms** | measured, `resolution-scale.md` §12/§14 |
| Old Tomb, 60-capped | 60.0 | 60.0 | **55.5 FPS, 1% low 41** | measured, §12 |
| Bower Lake (outdoor), uncapped, perf3x agent (in progress) | 117 FPS | 79 FPS | **49 FPS** | preliminary, `out/validation/perf3x-20261011/base.out` |

- **On the M5 Max**, holding 60 at 3× means GPU p95 at or below about 16 ms in the heavy outdoor views. Today that
  view sits at about 20 ms mean. **We need roughly 25–35% less GPU time at 3×**, more at p95. This is an estimate:
  the Bower Lake runs are a different plan from the Old Tomb baselines, and the perf3x agent's per-pass breakdown is
  not finished.
- **On a base M1** (8-core GPU, about 68 GB/s), nothing has been measured. Scaling the M5 Max by about 6–9× for
  compute and about 8× for bandwidth (the method of `weak-mac-perf-20261008.md`, widened to the base chip) gives
  **about 30–45 ms per frame at native 1×**, so about 22–33 FPS. This is an estimate.
  - The game shipped locked at 30 FPS on the Xbox 360 (`FPS_CAP_INVESTIGATION.md`). So **a locked 30 at 1× on a
    base M1 is the faithful target and is probably within reach** with the near-term levers.
  - **60 at 1× on a base M1 needs the GPU cost per frame roughly halved.** None of the incremental levers does that.
    Only the large shader bet (§4.1) or a look-changing upscaler does.
  - Base M2/M3 (10-core, 100 GB/s) sit about 1.4–1.6× better than M1, which is still short of 60 at 1×
    (estimate).

## 1. Where the GPU time goes, and how far it is from ideal

**Measured breakdown at 3×** (`resolution-scale.md` §4; these are encoder spans, which overlap, from a build before
native shaders, K1 and deferred clears):

| Item | Span at 3× (ms/frame) |
|---|---|
| Main scene pass, 2×MSAA RGBA16F + D32S8 | 8.42 |
| Other render passes (shadow, blur, bloom, effects) | 4.33 |
| 4×MSAA exporter/terrain passes | 2.59 |
| Texture uploads/loads (about 6 per frame are scaled-resolve untiles) | 2.29 |
| Resolves (direct host resolve + rt_resolve) | 1.17 |
| EDRAM transfers (mostly resolve clears; since reduced by K1 and deferred clears) | 6.79 (overlapping) |

**Since then:**

- Native shaders took 15–19% off GPU time (`native-shaders.md`).
- K1 and deferred clears removed most of the separate transfer and clear traffic: about −0.36 ms of busy time at 3× on
  the M5 Max (§13, §14).
- Every lever on the EDRAM side measured "FPS-neutral on the M5 Max" (lever 3, lever 4, K1, K2, deferred clears).
  **The M5 Max is limited by shading, not by EDRAM traffic.**

**Shaders are the cost, and they are far from ideal** (`native-shaders.md`, step 0):

- Guest-draw render encoders carry 83.5% of the profiled shader cost.
- The main pass is limited by **instruction issue (50–58%)**, not by bandwidth (texture limiters ≤ 11%).
- **The hottest pixel shader, 356E4E7B, is 253 Xenos instructions (19 fetches) and compiles to 5,053 GPU
  instructions with 126 registers and 176 bytes spilled.** Even with vec4 → scalar (×4) and the 0·x multiply
  emulation, a straight port would be about 1,000–1,500 instructions. The excess comes from the generic emulation
  machinery:
  - runtime fetch-constant decode on every fetch;
  - explicit gradients for every fetch;
  - the PC dispatch loop;
  - per-use constant loads.
  - `-O2` hoists some of it, but the static count barely moved (5053 → 5086).
  - Static count is not dynamic count (uniform branches skip decode paths), so how much of this bloat actually runs
    is **unknown**. Measuring it is the first experiment in §7.
- Four pixel shaders carry about 60–75% of all shader cost (23%, 17%, 20% and 17% in the step-0 profiles).

**How far from ideal (speculative, order of magnitude only):**

- Xenos is about 240 GFLOPS and ran this content at 720p30, with EDRAM giving it nearly free MSAA and blending.
- A base M1 is about 2.6 TFLOPS, about 10× the shader throughput, yet the estimate above puts it at about 30 FPS at
  the same resolution.
- FLOP ratios are crude, but the size of the gap (about 5–10×) says the translation overhead, not the content, is
  what limits weak Macs. It is the main argument for the bigger bets.

## 2. What the game's renderer looks like from the recompiled code (new findings)

These were found for this study with grep over `generated/default/` and the shader bank. They have not been checked
at run time.

1. **There are no real symbols.** Names such as `MainRenderLoop_82B9CD68` and `ProcessAndProcessAndProcess523_…` come
   from a heuristic namer (5,269 of 60,752 functions are named). None of them comes from Lionhead.
2. **The XDK Direct3D library is linked in and can be located:**
   - **Kernel video imports.** The `Vd*` calls all sit at 0x82B9xxxx–0x82BAxxxx:
     - `VdSwap` and `VdGetSystemCommandBuffer` in `MainRenderLoop_82B9CD68` (the present/swap path);
     - `VdInitializeRingBuffer` and `VdEnableRingBufferRPtrWriteBack` in `sub_82B9BA58`;
     - the interrupt callback in `sub_82BA04A8`;
     - `VdInitializeEngines` in `sub_82BA0220`.
   - **Draw packets.** Xenos `DRAW_INDX` packet headers (type-3 opcode 0x22, built as `lis -16384; ori 8705`) are built
     in exactly **three functions**: `0x82207C30` (called from `0x82A82F10`), `0x82217EE8` (4 callers) and
     `0x8221E0F0` (8 callers). Their callers range from 0x821A… to 0x82FE…, so engine code calls them directly. This
     is an API boundary.
   - **Render state is raw Xenos registers, not D3D9 enums.** `0x8221E0F0` works through a device structure (r31):
     - 64-bit dirty masks at `device+8…+40`;
     - shadow copies of Xenos context registers from `device+10368` (register ids such as 0x2000/0x2100/0x2200 are
       passed to a flush helper);
     - the ring read-pointer writeback at `device+10896` (already used by `RingReadPointerPoll_82B9BF90.cpp`).
   - This is the XDK D3D design: `Set*` calls are mostly inline, write the shadow state and set dirty bits, and the
     draw call flushes the dirty registers as PM4. **So intercepting the draw functions gives Xenos register state,
     not a D3D9-style call stream.** Turning it into native draws needs the same Xenos decoding (formats, EDRAM bases,
     fetch constants) that the command processor already does.
3. **The shader bank names the passes.** `data/Shaders/ShadersRelease.sbk` (653 KB) holds the constant-binding names:
   - post-processing: `g_BloomSampler`, `g_BloomFactor`, `g_DepthOfFieldBlurSampler`, `g_RadialBlurSampler`,
     `g_ToneMapSampler`, `g_FixedGuassBlurWeights`;
   - fur: `g_FurTexture`, `g_PrelitFurTexture`, `g_FurBrightnessMultiplier`;
   - shadows: `g_DirectionalLightShadowMapSampler0/1`, `g_ScreenShadowMap`, `g_DownsampledShadowMap`,
     `g_ShadowFilterRange`;
   - water, landscape/terrain blend, particles, mist, atmospheric scattering.

   A small bank parser that maps each shader's microcode hash to its bindings would give every guest shader hash a
   human-readable role. It is the cheapest piece of HLE knowledge available and helps every option below. It is
   game-derived, so the mapping lives in ignored output only and is never committed.
4. **The game relies on EDRAM aliasing a lot.** At 2× (§11), each frame has 75.8 ownership transfers into 47.3
   destinations:
   - 33.3 colour↔depth;
   - 23.8 MSAA changes;
   - 13.3 format changes.

   The top pairs are the game reusing EDRAM base 0 as 4×MSAA D24S8 and as 1×MSAA 8888, and the main depth switching
   between 2× and 4× MSAA. A renderer that does not emulate EDRAM must find and re-implement every one of these tricks.

## 3. How other projects did it

### Xenia (D3D12/Vulkan) and XeniOS (Metal)

- Xenia has two EDRAM paths:
  - **RTV/host render targets** with ownership transfers (ours);
  - **ROV / fragment-shader interlock**, which does blending and depth/stencil in the pixel shader directly into a
    10 MB EDRAM buffer.
- Triang3l's 2021 write-up gives the trade-offs:
  - ROV is the most accurate: every format bit-exact, including 7e3 and 20e4.
  - ROV loses early-Z and hierarchical depth and needs long, slow-to-compile shaders.
  - The new RTV path was faster than ROV in all four games measured.
  - RTV "naturally has various accuracy flaws".
  - The resolution-scaling limit is 3×3. [1]
- Our Metal backend is the XeniOS port of the RTV path (`rtv-bindless` archives; `edram_rov_used = false` in
  `metal_pipeline_cache.cc`). XeniOS also has a WIP native Xenos→MSL emitter (`msl_shader_translator.cc`, about 7.5k
  lines, own binding ABI, per-shader fallback to MSC). Our pinned source does not contain it (`native-shaders.md`).

### Unleashed Recompiled (Sonic Unleashed, Xbox 360): the closest precedent

What they did (source read for this study [2][3][4]):

- **No Xenos emulation at all.** "A new renderer was written from scratch", built on plume (a D3D12/Vulkan HAL).
  There is no PM4 parsing and no EDRAM model.
- **They hook the XDK D3D API.** There are 42 `GUEST_FUNCTION_HOOK`s in `gpu/video.cpp`, 39 of them D3D/D3DX:
  - device and buffers: `CreateDevice`, `Lock/UnlockTextureRect`, `CreateTexture`, `CreateSurface`;
  - render targets and resolves: `SetRenderTarget`, `SetDepthStencilSurface`, `StretchRect` (the resolve), `Clear`;
  - draws and shaders: `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, `Create/SetVertexShader`,
    `Create/SetPixelShader`, `SetTexture`, `SetStreamSource`, `SetIndices`, `SetViewport`, `SetScissorRect`;
  - present: `Video::Present`.
  - Render state goes through the device's function-pointer table (`setRenderStateFunctions[state/4]`), which they
    fill with host functions. Sonic Unleashed's XDK build calls render-state setters through that table, which made
    it hookable.
- **Resolves** (`StretchRect`) link the destination texture to the current render target and are executed as
  hardware or shader MSAA resolves, which are plain copies into a host texture. No untiling and no guest-memory round
  trip.
- **Shaders** are compiled offline by XenosRecomp:
  - Xenos → HLSL → DXIL/SPIR-V, looked up by XXH3 hash at `CreateShader`.
  - It still uses a `while`/`switch` PC loop, and it relies on the reflection data embedded in the game's shader
    containers.
  - Its README says MSAA, EDRAM and resolves are out of scope, and memexport is unimplemented. Instancing and other
    pieces had to be written for that one game. [2]
- **They patch the game where it relied on EDRAM aliasing.** Their shadow-map patch changes the format "so surfaces
  are not shared" and re-renders the terrain shadow map instead of copying it.
- **They swap a few shaders by hash** (two: the DoF blur gets a higher-tap Gaussian, plus an "enhanced" motion blur).
  These are labelled enhancements.
- **They compile pipelines asynchronously during loading.** The game's "made all" checks are hooked so a load waits
  for compilation, which gives no stutters.
- **Results:**
  - it runs far faster than emulation and is CPU-bound first on modern PCs;
  - it scales to high resolutions and MSAA beyond 2×;
  - it runs on an Intel HD 510-class GPU;
  - they say the output is **not identical** to the original (colours, extra MSAA, a reverse-engineered DoF), with
    an optional filter to recreate the look. [3]
- **They have no native Metal backend.** macOS goes through MoltenVK and "depends on plume gaining Metal support". [3][4]

**Could the same approach work for Fable II on our base?** Partly, and at a much higher price:

| Precondition in Unleashed | Fable II |
|---|---|
| A D3D9-like call stream reachable by hooking about 40 functions | Draws go through 3 functions (§2), but state reaches them as Xenos register shadows with dirty masks. The HLE layer would decode Xenos registers anyway, so it saves only the PM4 parse, which is a CPU cost |
| Few EDRAM aliasing tricks, each patched in the game | About 75 ownership transfers per frame of several kinds, including colour↔depth reinterpretation. Each trick needs reverse engineering and a game patch. Getting one wrong is a visible preservation bug |
| Offline shader recompilation with embedded reflection | Possible: the shader bank has reflection names. XenosRecomp itself needs per-game work (integer constants, bools, memexport, which Fable II uses for fur, and vertex fetch) |
| The renderer was CPU-limited under emulation | Fable II at 3× is GPU/shader-limited. A new renderer helps the GPU side only if the shaders also get faster |

### RT64 / N64 recompilations

- RT64 is a high-level N64 renderer built from display lists, used by Zelda 64: Recompiled and others. It renders the
  game's commands natively on modern APIs.
- It makes **true extra frames**: it interpolates between the game's draw calls for a given matrix. That needs
  per-game tagging, and the game logic never sees the extra frames. [5][6]
- The lesson for us: high-level rendering pays off where the source GPU model is simple and the command layer is
  semantic (N64 display lists). The Xbox 360 GPU is a programmable shader GPU, so the "semantic" layer is the game's
  own shaders. HLE does not remove them.

### MetalFX / Metal 4

- **MetalFX spatial** (macOS 13+) is already in our presenter.
- **MetalFX temporal** needs colour, depth, motion vectors and a sub-pixel jitter.
- **MetalFX frame interpolation** (`MTLFXFrameInterpolator`, macOS 26+) takes two rendered frames plus motion and
  depth, and makes one in between. [7][8]
- **Metal 4** adds:
  - `MTL4CommandBuffer` with explicit command allocators;
  - argument tables in place of per-encoder binding;
  - residency sets as the residency model;
  - a separate compiler object with flexible/unspecialised pipelines.
  - Apple's material says it supports M1 and later on macOS 26 [7][9]. Whether Metal Shader Converter output works
    unchanged with MTL4 argument tables has to be checked (speculative).

## 4. The options, one by one

Effort: **S** < 1 day, **M** a few days, **L** about a week, **XL** several weeks, **XXL** months.

Gains are GPU ms per frame at 3× on the M5 Max (against about 15 ms Old Tomb / 20 ms Bower Lake), and at 1× on a
base M1 (against about 30–45 ms, estimate).

### 4.1 Shader quality (emulation level). The biggest lever

**a. Hot-shader specialization and hand-tuned replacement by hash (option 3 of the brief).**

- Take the top 4–8 pixel shaders (about 60–75% of shader cost). Bake in what is constant for their draws:
  - fetch-constant formats, signs, exp-adjust and dimensions;
  - bool and loop constants;
  - the resolution scale.
- Replace the generic fetch decode with direct `sample` calls. Keep Xenos arithmetic semantics: the 0·x rule, fma
  placement and evaluation order.
- **Infrastructure that already exists:**
  - `FABLE2_NS_DUMP_DIR` / `FABLE2_NS_OVERRIDE_DIR` swap one shader's metallib by `<ps>_<hash>_<mod>` (trace-only
    today);
  - `FABLE2_NS_LOG_FETCH` logs a shader's fetch constants;
  - `FABLE2_SHADER_LABELS` and in-process `gputrace` + `gpudebug profile run` give per-shader cost;
  - the native-shaders prototype already specialized fetch dwords 3/4 for one shader.
- **Missing pieces:**
  1. A shipped override store keyed by (ucode hash, modification bits, scale, MSC target). Overrides are built at
     build time from source we write, never from game data.
  2. A guard. A draw uses the override only when its live fetch constants match the baked ones (a cheap compare on
     the CPU); otherwise it uses the generic shader. That keeps it exact by construction.
  3. **An authoring route that keeps the ABI.** Write each replacement as HLSL against the `DxbcShaderTranslator`
     binding layout (system constants, float constants, bindless indices), or as specialised DXBC, and send it through
     the same DXIL → MSC → `metal-opt` chain. Hand-writing MSL against MSC's top-level argument buffer is possible
     but fragile.
  4. **A bit-exactness harness.** Replay a captured frame with and without the override and compare the attachment
     bytes (`gpudebug` replay), plus `parity.py` in game.
- **Gain:**
  - speculative: −20 to −40% on each replaced shader, so about −1.5 to −4 ms at 3× on the M5 Max;
  - about −15 to −25% of the M1 1× frame;
  - the §7 ceiling test turns this into a measurement.
- **Preservation:** exact if the arithmetic is kept and guarded; otherwise none.
- **Note:** the same mechanism carries labelled extras later (a higher-tap DoF, as Unleashed did). Those would be
  off by default.

**b. Draw-time specialization for all shaders (function constants).**

- Specialize fetch formats and bools per pipeline.
- Measured on top of `-O2` for one shader: −1.6% of main-pass time (2.39 → 2.35 ms). Probably −2 to −5% overall.
- M effort. The cost is pipeline-variant growth.
- Worth folding into (c) rather than doing alone.

**c. A native Xenos → MSL translator (skip DXBC/DXIL/MSC).**

- What it gains:
  - structured control flow in place of the PC loop;
  - typed constant buffers;
  - interpolator pruning;
  - specialization built in;
  - no run-time dependency on the Metal Toolchain for `metal-opt`.
  - Portability gain: Macs without Xcode get optimised shaders, and the cache key stops depending on the toolchain
    version.
- Routes:
  - port XeniOS's WIP emitter (about 7.5k lines plus about 2.7k command-processor lines, its own ABI);
  - write our own on top of the SDK's shader analysis (`Shader::AnalyzeUcode`), as XenosRecomp did for HLSL.
  - **Going straight to Metal IR/AIR instead of MSL is not recommended.** AIR is an undocumented bitcode dialect, and
    it skips the Metal front-end optimiser that delivered the native-shaders gain. MSL through
    `newLibraryWithSource` or offline `metal` gets that optimiser for free.
- **Gain:**
  - speculative: −10 to −30% of fragment time beyond today's `-O2` route;
  - fragment is the larger part of the frame, so about −1.5 to −4.5 ms at 3× on the M5 Max, and −15 to −30% of the
    M1 frame.
  - It is the only faithful lever with M1-at-60 potential, together with (a).
- XL. High risk: the whole shader feature surface, memexport and the fur path.
- Preservation: exact if Xenos semantics are kept, and verified per shader against the DXBC route, which stays as
  the fallback.

### 4.2 EDRAM model redesign for TBDR

**a. Resolve straight into the sampled texture (skip scaled buffer → untile → texture).**

- **Status 2026-10-11: built** (`FABLE2_RESOLVE_TO_TEXTURE`, `docs/resolution-scale.md` §17). Exact: 0 verify
  mismatches. Measured −0.37 ms at 3× and −0.12 ms at 2× on top of shadows-1x; scaled-load bytes per frame 241 → 34 MB
  at 3×; M1 estimate −0.6 to −2.5 ms at 3×. The scaled buffer is still written; skipping it is the next, riskier step.

- At scale > 1, every resolve writes the scaled resolve buffer, and each later sample of that range untiles it into a
  texture first. That is about 30 scaled loads per frame (§11). Measured: 0 of them are redundant, so the work is
  real.
- The idea: when the destination's guest format and tiling match a host texture the texture cache already holds,
  write that texture directly as well. Keep the buffer for any other consumer (the morph readback, memexport readers).
- **Gain:** estimate −0.5 to −1.5 ms at 3× on the M5 Max (most of the scaled part of the 2.29 ms upload span). It is
  bandwidth, so it scales up on an M1 (about −2 to −4 ms at 2×/3×; little at 1×, where loads come from shared
  memory).
- L. Medium risk (CR-10-style invalidation ordering). Exact (same bytes).

**b. Whole EDRAM in tile memory (imageblocks / tile shaders / programmable blending; a "no-transfer" aliasing
model; memoryless).**

- The measured remainder is small:
  - K1 and deferred clears already moved the transfers and clears into draw passes without tile shaders;
  - K3 (resolve in a tile kernel) was estimated at 0.1–0.3 ms at 2×;
  - lever 4 (DontCare/memoryless) had an upper bound of about 0.1 ms.
- Tile memory does not persist across passes, while EDRAM contents persist across passes and frames. So the "no
  transfer" aliasing model only helps inside one pass. The ownership transfers left are between passes.
- Gain: under 0.5 ms at 3× on the M5 Max; perhaps 1–2 ms on an M1. XL, high risk. **Not recommended** before 4.1
  and 4.2a.

**c. A ROV-style EDRAM path (raster order groups, EDRAM as a buffer, blending and depth in the shader).**

- It removes all transfers and makes 7e3 colour and D24FS8 depth bit-exact. That would be a *preservation* gain:
  today RGBA16F approximates 7e3.
- It loses hardware depth test, early-Z and Apple's hidden-surface removal, which is the main TBDR advantage. It also
  makes every sample read-modify-write device memory at 3×.
- Xenia measured RTV faster than ROV on desktop GPUs [1]. On a bandwidth-starved M1 it would very likely be slower.
- Speculative gain: negative at 3×. Keep it in mind as an **accuracy mode** if a 7e3/D24FS8 artefact ever matters,
  not as a performance option.

### 4.3 Metal 4, encoding and pipeline caches (CPU side)

- **Little help on the M5 Max at 3×.** At 2×/3× the GPU Commands thread waits on the GPU (22% slot wait at 2×), and
  every CPU win in the perf pass showed up as cores, not FPS.
- **It matters for the M1 at 60** (estimate): GPU Commands is about 5.4 ms per frame on the M5 Max, so 7–11 ms on a
  base M1 with a p99 near 15 ms.

| Item | Gain | Effort / risk | Note |
|---|---|---|---|
| MTL4 command buffers + allocators + argument tables | GPU Commands −10 to −25% CPU (speculative); 0 GPU ms | L / medium; needs macOS 26 (an `@available` path, so the 15.0 floor stays) and an MSC compatibility check | Encoder creation is about 12% of the thread; bindless tables 5.5% |
| Multi-threaded encoding (parallel render encoder for the main pass, or translation/encoding split) | CPU only; −20–40% of the critical thread at high FPS | XL / medium | Big bet D of the perf audit; after the cheaper wins |
| GPU-driven / indirect encoding (ICB) | About 0 at 3×; PM4 state changes per draw defeat ICB reuse | XL / high | Not recommended |
| Shipped pipeline cache per build (the pinned `apple7:15.0` seed from the portable work) | No steady-state FPS; removes first-launch and new-area compile hitches on every Mac | M / low | Already half built (`portable-build.md`). Ship the seed with each build. Pairs with Unleashed-style "compile during load" if hitches remain |

### 4.4 Resolution-aware tricks that keep the look

**a. Guest-viewport-sized host targets instead of EDRAM-height ones.**

- The main target is 3360×3528×2 samples at 3× for a 1280×720 guest output.
- Apple GPUs only load and store touched tiles: the §13 touched-rectangle model matched the measurements, and the
  full-texture model gave an impossible 8 GB/frame. So **the GPU-time gain is about 0**.
- The gain is memory: 8.8 GB footprint at 3×. Fitting 2×/3× on 8–16 GB Macs is the real benefit.
- L (render-target keys, EDRAM addressing, resolve extents). Exact.

**b. Faithful-scale auxiliary targets (new candidate; partly measured).** Render the targets the game treats as
fixed-resolution buffers at 1×, while the scene renders at 3×:

- **Effects** (bloom/blur/glow chains): already built and measured, then removed. GPU at 3× went from 14.54 to 13.60 ms
  (−0.94 ms) and 3× at the 60 cap from 55.5 to 58.7 FPS. It was removed because the green interact glow came out
  narrow and dim (`resolution-scale.md` §12, `../restore-points/2026-10-10-effects-native-targets-experiment.*`).
  - **New hypothesis for the root cause:**
    - The first 1× effect pass samples a full-scale (3×) resolved scene texture with the original bilinear tap
      positions.
    - Each tap now averages 2×2 *host* texels, which is 1/9 of the area a 1× tap averaged.
    - So thin bright features (the one-guest-pixel outline that feeds the glow) are under-sampled or missed: a
      narrower, dimmer glow.
  - Fix idea: a box-filtered guest-resolution copy (3×3 average) of any full-scale texture that a native-scale pass
    samples, made once per resolve. The experiment's box filter applied only to the mixed-scale *transfers*, not to
    texture fetches of scaled resolves, which fits this hypothesis.
- **Shadow maps** (`c0:90/1280x2048`-class depth targets):
  - At 3× they are rendered at 9× the original texel count. That is sharper than the console, not faithful.
  - At 1× they keep the original texel density, filtered at 3× screen resolution.
  - Speculative gain: −1 to −2 ms at 3× on the M5 Max, part of the 4.33 ms "other passes" span.
  - Needs depth support in the per-target-scale patch (the experiment was colour-only).
- **Preservation:** this *moves toward* the original look (original effect width, original shadow softness), but it
  changes the current 3× image, so the maintainer judges it by eye. Default on only after a by-eye pass.
- L (the patch exists; root cause plus depth support).

**c. MSAA choices.**

- The 2× MSAA samples *are* EDRAM state that later passes and resolves read, so they must be stored. No bit-equivalent
  shortcut was found.
- An in-pass `storeAndMultisampleResolve` would only match where the guest resolve is a plain average into a format
  with identical rounding, and the samples still have to be stored. Gain about 0. Not recommended.

**d. Variable-rate shading and rasterization rate maps.** Out: they change pixels, which conflicts with preservation.

### 4.5 Upscaling (look-changing, labelled extras)

| Option | Feasibility | Expected | Look |
|---|---|---|---|
| **MetalFX spatial, 2× internal → 4K output** | Code exists (`metal_presenter_use_metalfx`). Today it only engages when the output is larger than the input, so it needs a "target output size" setting (S–M) | M5 Max: about 8.5 ms (2×) + about 0.5–1 ms → **60 locked with headroom** (estimate). M1: 1× → 1440p/4K at about 1× cost + about 1–2 ms | Softer than native 3×; UI and text upscaled. **Labelled extra.** The faithful 3× path stays |
| **MetalFX temporal** | Needs (1) motion vectors: no per-object motion exists in an emulator. Camera-only vectors can be reconstructed from depth plus the previous and current view-projection, but that needs the camera matrices found in the guest constants (shader-bank names help). Skinned characters and the dog would ghost. (2) Sub-pixel jitter: add it to clip-space position in the translator for scene passes only, not UI or post. (3) A clean list of which passes count as "scene" | XL; quality uncertain | Look-changing, with ghosting risk. **Not before spatial has been judged** |
| **Frame interpolation** (MetalFX interpolator, or RT64-style transform interpolation) | MetalFX needs the same motion and depth as temporal and adds a frame of latency. RT64-style needs HLE knowledge of transforms per draw: XXL | Doubles presented FPS | Look- and latency-changing; game logic runs on a 30 Hz tick. **Lowest priority** |

### 4.6 HLE renderer: a "new engine" for drawing

**Full HLE (Unleashed-style, bypass PM4 entirely).**

- What it would remove:
  - the PM4 parse and the GPU Commands thread's per-draw translation (a big CPU win, mainly for M1 at 60);
  - EDRAM transfers;
  - the resolve → buffer → untile chain;
  - first-use pipeline compiles (compile at load).
- What it would not remove: the cost of the game's own shaders, which dominate at 3×, unless they are rewritten too.
- Why it is harder for Fable II than for Sonic Unleashed (§2, §3): state is raw Xenos registers, there are about 75
  EDRAM aliasing events per frame, memexport drives the fur, and there are no symbols.
- **Estimate:**
  - GPU at 3× on the M5 Max: −20 to −30% (the EDRAM/resolve/untile share plus offline-specialised shaders);
  - M1 CPU: GPU Commands mostly gone.
- **Effort XXL** (person-months). **Risk very high.**
- **Preservation risk: high.** Every misread aliasing trick or format is a visible difference, and Unleashed itself
  ships "not identical" output.
- **Verdict: not now.** Gate G0 (§7.3) decides whether it is ever worth starting.

**Hybrid pass-level HLE (the realistic "new engine" bet).**

- Keep emulation for the scene. Recognise selected whole passes by their shader hash sequence and render-target keys,
  named through the shader bank, and render them natively.
- **Best candidate: the post-processing chain** (bloom, DoF, radial blur, tone map, the glow mask chain):
  - about 25 effect passes per frame;
  - about 23 of the 31.7 resolves come only from effect targets (§11);
  - each step today is draw → resolve → scaled buffer → untile → texture → next draw.
  - A native chain can read the scene's host render target directly and keep intermediates in host textures, or fuse
    passes in one compute or tile dispatch.
  - It must write the same outputs to the same guest-visible places: final resolve destinations, and anything the game
    reads back.
- **Gain** (estimate, from the measured parts at 3×: effect passes 1.79 ms + most of their resolves and scaled loads):
  - **−1.5 to −3 ms at 3× on the M5 Max**;
  - more on M1 (bandwidth).
- **Preservation:** exact only if each pass's arithmetic is ported faithfully. Start from the game's own shaders
  translated mechanically (§4.1c output), then fuse. Hand-"improved" filters are labelled extras.
- **Shadow filtering** (`g_ShadowFilterRange`, `g_ScreenShadowMap`, downsampled shadow map) is the second candidate.
- **Dog fur** (memexport-driven shells) is the worst candidate. It is the most fragile path in the backend
  (bug-patterns rows 1–2), and its GPU cost is not known to be large. Leave it emulated.
- XL per chain. High risk (guest-visible ordering, resolve semantics).

## 5. Ranked comparison table

Gains are GPU ms per frame unless noted. M5 Max at 3× (baseline about 15 ms Old Tomb / about 20 ms Bower Lake); base
M1 at 1× (about 30–45 ms, estimate). **M** = measured, **E** = estimate, **S** = speculative.

| Rank | Option | What it is | M5 Max, 3× | Base M1, 1× | Effort | Risk | Preservation impact | Prerequisites |
|---|---|---|---|---|---|---|---|---|
| 1 | **Hot-shader specialization / replacement by hash** (4.1a) | Guarded, specialized versions of the top 4–8 pixel shaders, same ABI and same Xenos arithmetic | −1.5 to −4 (S) | −15 to −25% (S) | M (first shader) → L (top 8) | Medium; the guard keeps it exact by construction | None if bit-exact (harness required) | Bank parser for names; replay-compare harness; ceiling test (§7.1) |
| 2 | **Faithful-scale auxiliary targets** (4.4b): effects (revive the patch, fix the glow) + shadow maps at 1× | Fixed-resolution buffers stay at the console's resolution | −0.94 (M, effects) + −1 to −2 (S, shadows) | n/a at 1× (matters at 2×) | L | Medium–high (mixed-scale transfers) | **Closer to the original look**; changes current 3×, so judge by eye | Root cause of the glow (§7.2); depth support |
| 3 | **Resolve straight into the sampled texture** (4.2a) | Skip the scaled buffer → untile → texture round trip | −0.5 to −1.5 (E) | ≈0 at 1×; −2 to −4 at 2×/3× (E) | L | Medium (invalidation ordering, CR-10 class) | None (same bytes) | Counter: scaled-load bytes per frame and marginal busy time |
| 4 | **Native Xenos → MSL translator** (4.1c), absorbing 4.1b | Replace DXBC → DXIL → MSC → metal-opt | −1.5 to −4.5 (S) | −15 to −30% (S) | XL | High (whole feature surface) | None if semantics kept; per-shader fallback | Go from bet 1's ceiling test (gate G2) |
| 5 | **Hybrid pass-level HLE: post-processing chain** (4.6) | Native, fused implementation of the named post chain | −1.5 to −3 (E) | −1 to −3 (E) | XL | High | Exact only if faithfully ported; extras labelled | Bank names, bet 1's harness, rank 3's resolve plumbing |
| 6 | **MetalFX spatial 2× → 4K** (4.5) | Upscale a 2× render to the 4K output | −6 to −7 vs native 3× (E) | 1× → high-res output for about +1–2 | S–M | Low | **Look-changing, labelled extra** | Target-output-size setting; maintainer's by-eye A/B |
| 7 | **Shipped pipeline seed per build** (4.3) | Universal warm cache from the pinned target | 0 steady; no compile hitches | Same | M | Low | None | Portable-build seed work |
| 8 | **Metal 4 encoding** (4.3) | MTL4 command buffers, allocators, argument tables | ≈0 | CPU −10–25% of GPU Commands (S) | L | Medium | None | macOS 26 `@available` path; MSC check |
| 9 | **Guest-viewport-sized host targets** (4.4a) | Shrink EDRAM-height targets | ≈0 ms; −2–3 GB at 3× (E) | Memory headroom on 8 GB Macs | L | Medium | None | — |
| 10 | **Multi-threaded encoding** (4.3) | Parallel main-pass encoding | ≈0 | CPU only | XL | Medium | None | After rank 8 |
| 11 | **Whole EDRAM in tile memory** (4.2b) | Imageblocks, tile shaders, in-tile aliasing | < 0.5 (E) | 1–2 at 2×+ (E) | XL | High | None if exact | — |
| 12 | **MetalFX temporal** (4.5) | Jittered render + reconstructed motion | Large (S) | Large (S) | XL | High (ghosting) | Look-changing | Camera matrices from guest constants; pass classification |
| 13 | **Full HLE renderer** (4.6) | Unleashed-style; no PM4 or EDRAM | −20 to −30% (S) | CPU large, GPU −20–30% (S) | XXL | Very high | High risk | G0 census, G2, every aliasing trick catalogued |
| 14 | **ROV / raster-order EDRAM** (4.2c) | EDRAM as a buffer, shader blending and depth | Likely slower (S) | Likely slower | XL | High | **More exact** (7e3, D24FS8) | Only as an accuracy mode |
| — | Frame interpolation; VRS/rate maps; GPU-driven ICB | — | — | — | — | — | Look- or latency-changing / not useful | Not recommended |

**Can 3× at 60 be reached faithfully on the M5 Max?**

- Ranks 1 + 2 + 3 together: −3 to −8 ms at 3× (mixed M/E/S).
- From about 15 ms (Old Tomb) that gives 7–12 ms, which holds 60.
- From about 20 ms (Bower Lake) it gives 12–17 ms, which is borderline at p95.
- **Plausible but not certain without rank 4 or 5.** The perf3x agent's per-pass breakdown of the outdoor scene
  should be the next input. If shadows or terrain dominate outdoors, rank 2's shadow part and rank 1's choice of
  shaders change.

## 6. Recommended path with gates

**Near term (1–2 weeks of agent time, all faithful):**

1. **Shader bank parser + names in the pass trace** (S).
   - Map ucode hash → constant-binding names. Print the role of each shader in `f2diag gpu` output.
   - Output stays in ignored folders; game-derived data is never committed.
   - Every later step uses it.
2. **Bet B: faithful-scale auxiliary targets** (§7.2). Revive the effects patch and test the glow hypothesis first.
   - **Gate G1:** the glow matches 1× by eye and by crop diff (within same-build noise), on the green interact glow,
     torch and orb bloom.
   - If that passes: default-on candidate, plus a launcher toggle "Effects/shadows at original resolution". Then add
     shadow maps.
   - If the glow cannot be fixed in about 3 days, drop it again (its −0.94 ms is not worth a look regression).
3. **Bet A step 1: shader ceiling test** (§7.1). It decides how much is left in the shaders.

**Medium term (gated on G2):**

4. **Gate G2** (from §7.1):
   - If the fully specialized top shader runs **≥ 25% faster** than today's `-O2` version on its main-pass fragment
     time, build rank 1 as a product feature: guarded overrides for the top 4–8 shaders.
   - If it is **≥ 35%**, also start rank 4 (the native MSL translator), beginning with pixel shaders only, with the DXBC
     route as fallback.
   - If it is **< 15%**, the shader-bloat theory is wrong. Stop the shader bets and put the effort into ranks 3 and 5.
5. **Rank 3, resolve straight into the texture.** Measure first: add a counter for scaled-load bytes and use
   `tail.py` marginal busy time on `tex_upload_*` at 3×.
6. **Rank 6, MetalFX spatial 2× → 4K**, as a labelled "Upscaled 4K" option for the maintainer to A/B (done 2026-10-12 as
   `[graphics] upscaling = "metalfx-spatial"`: 91.6 vs 63.2 FPS against native 3× at 4K, `docs/resolution-scale.md` §22). It is cheap, and it is
   the only route on base M1–M3 to high-resolution output at 60 if the faithful levers fall short.
7. **Rank 7, ship the pipeline seed.**

**"New engine" bet (gated on G0 and G3):**

8. **Gate G0, HLE census** (§7.3, S–M, diagnostic only):
   - Does ≥ 95% of the ~1,600 draws per frame go through the three draw functions?
   - Do the callers hand over recognisable mesh/material objects?
   - How many distinct EDRAM aliasing patterns are there per frame and per area?
9. **Gate G3:**
   - Build the hybrid post-chain HLE (rank 5) only if all of these hold:
     - G0 shows a clean draw boundary;
     - the post chain (named by the bank) costs ≥ 2 ms at 3× in the perf3x breakdown;
     - bet A's replay-compare harness exists, so it can be proved exact.
   - A full HLE renderer (rank 13) is not recommended unless G0 finds ≤ 5 distinct aliasing patterns *and* G2 says the
     shaders can be made fast. Even then, budget it in months, keep emulation as the fallback per pass, and expect
     preservation regressions.

**What to tell base-M1–M3 owners today (estimate):** native 1× at the original 30 FPS cap should hold once ranks 1–3
land. 60 at 1× needs bet A's full form (ranks 1 + 4) or the labelled upscaler. Both should be measured on a real
base M1 before anything is promised.

## 7. First experiments for the top two bets

Constraints for whoever runs these:

- background test mode, the agent's own pid only, `pgrep -x fable_2` = 1 after close;
- the first launch after each build counts;
- **never** xctrace Metal System Trace (the `gpudebug` replay of an in-process `gputrace` is the allowed GPU tool);
- restore point before edits;
- one or two runs per arm.

### 7.1 Bet A: shader ceiling test (about 3 days)

**Question:** how much faster can the hottest pixel shader get while staying bit-exact with today's output?

1. **Day 1: capture and dump.**
   - Old Tomb save, 3×, static view: `MTL_CAPTURE_ENABLED=1 FABLE2_GPU_CAPTURE_PATH=… FABLE2_GPU_CAPTURE_AT_S=25
     FABLE2_SHADER_LABELS=1`.
   - Also `FABLE2_NS_DUMP_DIR` to dump the metallibs of the top shaders (356E4E7B, CD2CE6D4, 28E20286, F2F103F1;
     re-rank from the 3× `gpudebug profile run`).
   - Also `FABLE2_NS_LOG_FETCH=<hash>` for each, to log the fetch constants of their draws. Record whether they are
     constant across draws, which tells whether one specialised variant covers them or a few do.
   - Record per-shader **dynamic** cost and static instructions, registers and spills from `gpudebug`.
2. **Day 2: build three variants of the top shader** offline, from the dumped DXIL/AIR with the toolchain the build
   already uses:
   - (i) today's `-O2` (control);
   - (ii) fetch constants and bool/loop constants baked as literals, then `-O2` (exact by construction for the logged
     draws). This extends the native-shaders prototype that already baked dwords 3/4;
   - (iii) as (ii), plus implicit-gradient `sample` where the fetch sits in uniform control flow (**diagnostic**:
     exactness has to be checked, because hardware derivatives and `dfdx`/`dfdy` may differ).
   - If baking at the AIR level is impractical, bake at the DXBC level in the translator, behind a trace-only
     environment variable.
3. **Day 3: measure and compare.**
   - Install each variant with `FABLE2_NS_OVERRIDE_DIR` and replay the same capture with `gpudebug profile run`. Record
     main-pass fragment time and the shader's cost share.
   - **Exactness:** compare the replayed attachment contents of (ii) and (iii) against (i) byte for byte, then
     `parity.py` in game at 1× and 3×.
   - Report: the speed-up of (ii) and (iii), the exactness of each, and the dynamic vs static instruction ratio.
4. **Output:** gate G2's number, written as a new section of this document. Remove the trace-only bake switch
   afterwards if G2 fails.

### 7.2 Bet B: faithful-scale effect targets, glow root cause (about 3–4 days)

**Question:** is the narrow and dim glow with native-scale effect targets caused by full-scale textures being
sampled at 1× tap footprints?

1. **Day 1: re-apply the patch.**
   - Restore point first. Then apply `../restore-points/2026-10-10-effects-native-targets-experiment.patch` onto
     current `macos`. It was cut from 85032d0 plus the lever5b patch, so expect conflicts with K1 and deferred clears
     in `metal_render_target_cache.cc`; resolve them keeping both.
   - Build; the first launch counts.
   - Reproduce column 6 of `v-outline-zoom.png` (green interact glow, 3×, `FABLE2_EFFECTS_NATIVE_TARGETS=1`) against
     1× and today's 3×.
2. **Day 2: find the producer.**
   - With `FABLE2_EFFECTS_NATIVE_TRACE=1` and the shader labels, list the glow chain: which native-scale pass first
     samples a **full-scale** resolved texture, and which shader does it (name it with the bank parser if step 1 of
     §6 is done).
   - Check whether the glow mask arrives through a mixed resolve or a mixed-scale transfer (the README's two
     suspects).
3. **Day 3: test the hypothesis.**
   - For textures whose resolved range is scaled and that a native-scale pass fetches: make a 3×3 box-averaged
     guest-resolution copy at resolve time, and point those fetches at it.
   - Diagnostic arm first (environment variable, trace-only): crop-compare the glow against 1×.
   - If the glow is restored, the fix is that copy. Its cost is small: 23 resolves per frame at 1/9 the size.
4. **Day 4: measure and decide.**
   - GPU at 2× and 3× (expect about the old −0.4 / −0.94 ms minus the copy cost).
   - 3× at the 60 cap.
   - Parity and by-eye checks on glow, torch, orb, dog fur, shadows and the pause-menu preview; area, F5 and bootloop
     runs.
   - **G1 decision.** If it passes: a launcher toggle defaulting on, plus a `bug-patterns` row. Then scope the
     shadow-map extension (depth-target support, mixed-scale depth transfers).

### 7.3 Gate G0: HLE census (diagnostic only, about 1–2 days; run before any HLE work)

- Add trace-only wrappers (hotfunc wrapper style, as `src/diagnostics/fps_probe.h`, forwarding to `__imp__`) on
  `0x82207C30`, `0x82217EE8` and `0x8221E0F0`. Count calls per frame and per caller `lr`, and compare with the command
  processor's `tr_n_draw_ds` and draw counts.
- Also count PM4 `INDIRECT_BUFFER` executions: XDK precompiled command buffers would bypass these functions.
- Pull the EDRAM aliasing pattern list per area from `FABLE2_XFER_TRACE=1` (Old Tomb, Bower Lake, a town).
- Result: the share of draws covered, the number of distinct caller sites, and the number of distinct aliasing
  patterns. That decides G3.

## 8. Sources

1. Triang3l, "Leaving No Pixel Behind: New Render Target Cache, 3x3 Resolution Scaling", Xenia blog, 2021-04-27 —
   https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html ;
   Xenia ROV wiki — https://github.com/xenia-project/xenia/wiki/ROV
2. XenosRecomp README (hedge-dev) — https://github.com/hedge-dev/XenosRecomp
3. Unleashed Recompiled README — https://github.com/hedge-dev/UnleashedRecomp (mirror read:
   https://cdn.jsdelivr.net/gh/hedge-dev/unleashedrecomp@main/README.md)
4. Unleashed Recompiled renderer source, `UnleashedRecomp/gpu/video.cpp` —
   https://raw.githubusercontent.com/hedge-dev/UnleashedRecomp/main/UnleashedRecomp/gpu/video.cpp ; architecture
   overview — https://mintlify.wiki/hedge-dev/UnleashedRecomp/building/architecture
5. Zelda 64: Recompiled — https://github.com/Zelda64Recomp/Zelda64Recomp ; RT64 — https://github.com/rt64/rt64
6. RT64 interpolation described in the Snowboard Kids 2 recompilation write-up —
   https://blog.chrislewis.au/snowboard-kids-2-is-recompiled/
7. WWDC25 "Go further with Metal 4 games" (MetalFX upscaling, frame interpolation, denoising) —
   https://developer.apple.com/videos/play/wwdc2025/211/
8. `MTLFXFrameInterpolatorDescriptor` —
   https://developer.apple.com/documentation/metalfx/mtlfxframeinterpolatordescriptor
9. "WWDC 2025 – Discover Metal 4" summary — https://dev.to/arshtechpro/wwdc-2025-discover-metal-4-23f2

In-repo: the 2026-10-11 performance audit, `docs/resolution-scale.md` §4, §11–14, `docs/native-shaders.md`,
`docs/weak-mac-perf-20261008.md`, `docs/hotfunc_overrides.md`, `docs/portable-build.md`, `docs/FPS_CAP_INVESTIGATION.md`,
`src/core/hotfunc/gpu/RingReadPointerPoll_82B9BF90.cpp`, `generated/default/fable_2_recomp.*.cpp` (grep only),
`data/Shaders/ShadersRelease.sbk` (strings only; game data, never committed), `out/validation/perf3x-20261011/`
(another agent's in-progress runs).

## 9. Bet A result: shader ceiling test (2026-10-11)

Branch `bet-a-shader-ceiling`. Tools: `tools/shaderlab` (`docs/native-shaders.md`, "Shader lab"). Scene: Bower Lake
`hero1-dog-live_2026-10-11_11-20-20` at 3×, M5 Max. Per-shader numbers come from a one-frame capture replayed in
`gpudebug`; frame numbers come from live `headings8` arms with the pass trace.

### 9.1 Where the shader time is at 3× outdoors

- **The shader cost is flat, not concentrated.** The hottest guest PS is 7.6% of the frame's shader cost, against
  23% for `356E4E7B` in the old 2× Old Tomb profile, which is not in the top 25 here.
  - Stage split: fragment 66%, compute 19%, vertex + mesh 12%.
  - Backend helper shaders (EDRAM `transfer_ps` and clears) are about 15% of the cost; no guest shader is touched
    there.
- **Main HDR pass:** 3.70 ms in the replay (3.90 ms live). 85% of it is fragment and 2% vertex. Limiters: issue 57%,
  int/cond 41%, F32 35%.
  - So it is not vertex-bound. It scales weakly with resolution because much of its fragment work is fixed per
    draw (905 commands), not because vertex work dominates.
- **Ranked pixel shaders** (share of the frame's shader cost; static numbers from the GPU binary of the library that
  actually ran):

| # | PS (role, inferred) | Share | Pipeline | Library at run time | GPU instr / regs / spill |
|---|---|---|---|---|---|
| 1 | `AF1E16EE57E4BB06` (tessellated terrain material, 57 draws, 20 fetches) | 7.6% | tessellation emulation | **plain** | 1515–1636 / 47 / 0 |
| 2 | `0F99810F072BF0FC` (full-screen 3× pass, 36 fetches, one draw) | 5.0% | geometry emulation | **plain** | 1646 / 43 / 0 |
| 3 | `F1EB2497108F159D` (55 small draws) | 3.2% | standard | -O2 | 143 / 8 / 0 |
| 4 | `577CB5267A8C92C3` (one draw) | 2.3% | standard | from cache | 578 / 22 / 0 |
| 5 | `7CD57B81550F19E3` (material, several VS) | 2.0% | standard | -O2 | 841–845 / 47 / 0 |
| 6 | `1B4B5AE12EF270CA` | 1.5% | emulation | plain | 541 / 31 / 0 |
| — | `44B567635EB10620` | 0.95% | standard | from cache | 5466 / 126 / 64 (the only big spiller) |

  - **Top vertex work:** geometry-emulation mesh stages 2.9%, 1.1%, 1.1%. The memexport VS `695413A9` 1.7% is
    export-only.
  - **Names:** the shader bank cannot name these cheaply. The constant-binding names in `ShadersRelease.sbk` are a
    global table, and the shader bodies are compressed: entropy 8.0, not zlib, no ucode match. Mapping hashes to
    names needs an XMem/LZX decoder. The roles above come from pass, draw count and fetches.

### 9.2 How much of the bloat executes

- **Statically, most of the bloat is gone already for the swapped shaders.** The 5,053-instruction figure was the
  plain library. With `-O2` the hot material shaders here are 0.5–3k GPU instructions with no spills; only
  `44B56763` (5.5k, 126 regs, 64 B spilled) is still in the old class.
- **Dynamically.** `gpudebug`'s per-encoder instruction counts vary by about 7% between identical replays, so they are
  only indicative. The main pass executed 1.83–1.97 G instructions with the two plain shaders, and 1.34–1.54 G with
  both at `-O2`, a 22–32% reduction. Most of the bloat that runs was the dispatch-loop overhead that `-O2` hoists;
  the fetch-decode paths that stay are mostly behind uniform branches.
- **Specialization on top of `-O2`:** baking the invariant constants removes 27% of the IR lines and 30% of the
  branches in `AF1E16EE` (3430 → 2510 lines, 178 → 125 branches). It removed little executed work (below).

### 9.3 Specialized variants

**Method.**

- `FABLE2_NS_LOG_CONSTS` showed which inputs never change:
  - `AF1E16EE`, over 239 draws: all system and bool/loop dwords it reads, and 19 of its 23 fetch dwords. Only the
    texture size and mip/LOD dwords of two fetch slots change.
  - `0F99810F`: all of its inputs, over 4 draws.
- `bake.py` turns those loads into literals, then `-O2` runs again.
- The variant library replaces the original inside a cloned trace, which is then replayed.
- Cost is normalised by the cost of the unchanged shaders. Base and `-O2` were each replayed twice.

| PS | plain | -O2 | specialized + -O2 | spec vs -O2 | spec vs plain | Output vs base |
|---|---|---|---|---|---|---|
| `AF1E16EE` (#1) | 8.6 | 3.26 | 2.71 | **−17%** | −69% | byte-identical |
| `0F99810F` (#2) | 5.85 | 7.84 (**+34%**) | 4.53 | **−42%** | −23% | byte-identical |

Byte-identical means the 97 MB scaled resolve buffer after the main pass and the main-pass colour attachment are
identical to the base replay. Two base replays are identical too.

**Live, 3× `headings8`** (marginal busy ms/frame):

| Arm | Busy | Main pass | Full-screen pass |
|---|---|---|---|
| Today (`FABLE2_NS_EMU_PIPELINES=0`) | 19.06 | 3.91 | 1.24 |
| `-O2` on both shaders | 18.65 | 3.38 | 1.39 |
| Specialized on both, `-O2` for 11 more plain emulation shaders | **18.04** | 3.29 | **0.90** |

### 9.4 Gate G2

- **The top shader, `AF1E16EE`, is −17% specialized vs `-O2`: inside the 15–25% band.** That is below the 25% needed
  to build the hot-shader specialization feature, and above the 15% at which the shader bets would be dropped.
- `0F99810F` reaches −42% vs `-O2`, but most of that undoes an `-O2` regression. Against plain it is −23%.
- **Frame effect:**
  - specializing both hot shaders on top of the fixed default saves about 0.4–0.6 ms at 3× (18.46 → 18.04 busy,
    about 2–3%);
  - a top-8 product feature extrapolates to about −0.6 to −1.0 ms at 3×. The §4.1a estimate was −1.5 to −4 ms.
- **The large exact win was elsewhere:** 18% of the shader cost ran unoptimized in emulation pipelines. Fixing that
  (`FABLE2_NS_EMU_PIPELINES` plus the keep-plain list) gives −0.6 ms at 3× (−3.2%), and −0.49 ms (−2.8%, 56.1 → 57.7 FPS)
  after merging shadow maps at 1×. It is neutral at 2× and now default on.

**Decision:**

1. **Do not build hot-shader specialization as a product feature now, and do not start our own Xenos→MSL
   translator.** G2's measured value is 17%, below both build thresholds.
2. Keep the shader bets alive, because the result is above 15%. Two follow-ups are cheap and exact:
   - extend the keep-plain list as measured (`-O2` can regress shaders that are already lean);
   - re-run the ceiling test if `44B56763`, the 5.5k-instruction spiller, or a scene with a more concentrated
     shader profile shows up in a 3× profile.
3. Put the next effort into the non-shader levers: the post chain and resolves (§4.2a, §4.6), and the faithful-scale
   auxiliary targets (§4.4b).

### 9.5 What the full feature would take (if a later scene passes G2)

- **Keys:** the override is keyed by the plain metallib's hash, which already covers the ucode, the modification
  bits and the resolution scale, plus the MSC target (pinned `apple7:15.0` for the portable seed) and the
  `metal-opt` toolchain hash. These are the same parts as the NS1 key, with a version tag `NS2`.
- **Generated, not hand-written.** Bake the invariant system, bool/loop and fetch dwords at the AIR level as
  `bake.py` does, then `-O2`.
  - The constants come from a "learn" session (`FABLE2_NS_LOG_CONSTS`).
  - They are stored as a per-shader record: dword offsets, values, and a 64-bit hash of the guarded values.
  - No game data is committed: the records are generated per install into the shader cache, like NS1.
- **Guard:** at draw time, hash the guarded fetch, system and bool/loop dwords. These live in buffers that are
  already written, so this is about 20–40 dword loads per draw. Bind the specialized pipeline only on a match, and
  the generic `-O2` pipeline otherwise. One specialized pipeline per (pipeline, constant-set) pair, built in the
  background like the native-shader swap.
- **Cache and seed:** stored as artifact records next to NS1. The portable seed can carry them for the pinned
  target, where they hit on any Mac.
- **Cost:** M for the generator and guard. A per-shader keep-plain or keep-`-O2` A/B is needed, because
  specialization did not always help in Metal's compiler (see `0F99810F` under plain `-O2`).

### 9.6 Built anyway (2026-10-11)

The maintainer approved the feature despite G2 (3× Bower Lake sat at 16.6–16.9 ms GPU, just above 60 FPS). Built as
sketched in §9.5, with one change: per-bit learn and masked guards, and the bake folds the bit-field uses
(`air.extract_bits`, masks, shifts) itself, which removes far more decode than Bet A's whole-dword bake. Results
(`docs/native-shaders.md`, "Hot-shader specialization"), off → on:

- Bower Lake 3×: 16.70 → 15.59 ms GPU (−6.7%), 59 → 63.5 FPS; 2×: −6.5%; 1×: −3.4%.
- Old Tomb 3×: 13.22 → 11.01 ms (−16.7%), 75 → 90 FPS; main pass −39%.
- The 5.5k-instruction spiller `44B56763` is −64 to −68% on its own passes.
- No measurable guard cost on the GPU Commands thread; 0 misbinds; parity inside the off-vs-off noise floor.

The live gain is about twice the §9.4 extrapolation (−0.6 to −1.0 ms at 3×), because the bake now folds the decode that
`metal-opt` cannot fold, and Old Tomb's profile is concentrated in its material shaders.

### 9.7 Open

- The per-shader `-O2` vs plain check for standard pipelines (`docs/native-shaders.md`) was stopped by the
  replay-memory incident.
- Static GPU stats (instructions, registers) of the `-O2` and specialized variants were not captured for the same
  reason. Only cost shares were measured.
- The shader bank naming needs an XMem/LZX decoder. (2026-10-14: the bank entries are plain zlib streams, not LZX;
  reader in `tools/fable2_shader_bank.py`, `docs/offline-shader-seed.md` §3.)
