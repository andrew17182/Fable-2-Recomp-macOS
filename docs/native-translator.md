# Native Xenos → Metal translator (`FABLE2_NATIVE_XLATE`, started 2026-10-13)

Engine-options rank 4 (`docs/engine-options-20261011.md` §4.1c). Goal (maintainer, 2026-10-13): "make this as native a
port as possible … emulating as little as possible". Translate Xenos ucode straight to structured MSL, with a
per-shader fallback to today's DXBC → DXIL → Metal Shader Converter (MSC) → `metal-opt` path. Pixel shaders first.

Status (2026-10-13): **default on** (`FABLE2_NATIVE_XLATE=0` restores the converter path; Dev Launcher toggle
"Native shader translator", plus "Native translator: vertex shaders too" for `FABLE2_NATIVE_XLATE_VS`). Pixel and
vertex shaders are native; in game 254/254 with 0 fallbacks. The playtest recheck (2026-10-13) was clean, with
no lag and the dog fur fine. The install warm-up, portable seed and benchmark kit launch the game with its defaults, so
they warm the native shaders too.

- **Coverage:** all 184 Bower Lake PS translate, and all are bit-exact under uniform flow at 1× and 3×.
  - Crossing forward jumps use a flag per jump target (`xe_jN`), with the skipped execs under `if (!(flags))`.
  - Loops (`loop`/`endloop`) become a structured `while` with `aL`, mirroring the DXBC count, start and step rules.
    Relative float constants (`c[aL+n]`, `c[a0+n]`) index the full constant buffer.
  - Point-sprite PsParamGen reads `user(xespritetexcoord0)`.
- **Exactness:** bit-identical to the converter's output under uniform control flow (differential GPU test below).
- **Perf (M5 Max, Bower Lake):** GPU −6 to −9% at every scale; the main HDR pass is −17 to −26%. This is with the NS2
  learned constants baked into the MSL.
  - 3×: 15.2 → 14.3 ms.
  - 2×: 10.42 → 9.62 ms.
  - 1×: 8.07 → 7.38 ms.

**Native first (2026-10-14, `docs/no-converter.md`):** with `FABLE2_NATIVE_FIRST` (default on) a shader's native
translation is its library from the first use; the converter library, the background swap and the emulation prewarm
are gone from normal runs, and a `FABLE2_NO_CONVERTER=1` build leaves DXC and MSC out. The "converter first, native
swapped in" flow described below is what `FABLE2_NATIVE_FIRST=0` restores.

## Route decision

**(a) A direct Xenos ucode → MSL emitter on the SDK's `ShaderTranslator` front end, bound through the existing MSC
ABI.** Main approved it on 2026-10-13. Rejected:

- (b) Direct AIR emitter. AIR is an undocumented bitcode dialect, and this route skips the Metal front-end optimizer that
  gave the -O2 win. It would produce the same code as (a) at higher risk.
- (c) The SDK's `SpirvShaderTranslator` → SPIRV-Cross. It emits Xenia's `while` + PC `switch` and a function-scope
  register array (`spirv_translator.cpp:589ff`), which carries the emulation layer into the output. It also targets
  Vulkan descriptor sets, and SPIRV-Cross would be a new Apache-2.0 dependency.
- (d) XeniOS's `msl_shader_translator.cc` (`preview` branch, 7.5k lines, BSD-3). It falls back to Xenia's PC `switch`
  for every jump, loop or call. It uses its own ABI (buffers 20–29, its own heaps and system constants), and a pipeline
  is native only when both VS and PS are native. Geometry and tessellation emulation pipelines stay on MSC, and those
  hold our two hottest 3× shaders. It compiles without `fp contract(off)`, so fused multiply-adds appear and parity is
  approximate. It has had 4 commits (June 2026) and no tests. **Borrow it as a reference** (VS features, float24 depth
  helpers, texture-sign specialization); do not adopt it.

### Evidence

- **Census** of 144 PS and 79 VS ucode dumps (Bower Lake, Bet A):
  - No loops and no call/ret anywhere. No relative addressing in any PS; 22 VS use `a0` (skinning).
  - PS control flow: `cexec b128–b137` on uniform bools and 14 forward `cjmp !bN`. 75 PS use predication (`setp`/`p0`),
    5 use kill.
  - PS fetches: 1364 `tfetch2D`, 182 `tfetch1D`, 26 `tfetchCube`, 18 `getWeights2D`, no 3D.
  - So every PS maps to straight-line MSL with `if` blocks.
- **ABI.** Disassembly of MSC's library for `AF1E16EE`:
  - Inputs: `user(texcoordN)` float4, centre or centroid by `interpolators_centroid`; `position`; `front_facing`.
  - The top-level argument buffer is a flat array of 64-bit GPU addresses at `buffer(2)`:
    - slot 1: the 2D-array view heap; 2: 3D; 3: cube;
    - slot 9: the sampler heap;
    - slot 10: system CBV; 12: bool/loop CBV;
    - slots 21/22/23: pixel float / fetch / descriptor-index CBVs.
  - Heap entries are `IRDescriptorTableEntry {va, resourceID, meta}`. In MSL that is
    `struct { uint64_t; texture2d_array<float>; uint64_t; }` (samplers: `{ sampler; uint64_t; uint64_t; }`).
  - Output: `color(N)`, plus `early_fragment_tests` when the DXBC shader declares `[earlydepthstencil]`.
- **Exactness settings,** measured with `xcrun metal` and `metal-opt -S`:
  - `#pragma METAL fp math_mode(safe)` + `#pragma METAL fp contract(off)` gives plain `fmul`/`fadd`, the same as MSC.
    Without the pragma, safe mode emits `llvm.fmuladd`.
  - MSC lowers DXBC `exp`/`log`/`rsq`/`sqrt`/`frc`/`round_*`/`min`/`max`/`mov_sat` to `air.fast_*`. MSL `fast::` gives
    the same intrinsics.
  - `rcp` and `div` become a plain `fdiv`; DXBC `mad` becomes `air.fma` (MSL `fma()`).
  - Coarse derivatives become `air.dfdx`/`air.dfdy` (MSL `dfdx`/`dfdy`).

## Design

Code: `backends/metal/port/rex/graphics/metal/metal_msl_translator.{h,cc}`.

- `MslShaderTranslator : ShaderTranslator`. It translates into a private `Translation` object, so the shader's DXBC
  translation is untouched.
- It needs the shader's **DXBC translation first**. The texture and sampler `bindless_descriptor_index` values come
  from `DxbcShader::Get{Texture,Sampler}BindingsAfterTranslation()`, so the descriptor-index buffer the backend fills
  stays valid. Any binding missing there → fallback.
- **Emitted shape:**
  - guest registers as `float4 rN` locals;
  - `ps`, `p0`, `a0` as locals;
  - the guest program inside `do { … } while (false)`, where exec-end is `break`;
  - bool/predicated execs as `if` blocks, using the DXBC translator's merge rules;
  - forward jumps as `if (!taken) { … }` up to the label. Crossing or backward jumps → fallback;
  - texture fetches as direct `sample()` calls on heap entries. The fetch-constant decode (size, LOD bias,
    exp-adjust, signs, resolution scale) mirrors DXBC; specializing it is a later step;
  - the epilogue mirrors `CompletePixelShader` (non-ROV): alpha test, alpha-to-mask, exp bias, gamma, RT0 blend
    premultiply, oDepth.
- **The 0·x = 0 rule is kept** as `select(a*b, 0, fast::min(|a|,|b|) == 0)` on exactly the components DXBC applies it
  to (main, 2026-10-13: keep it; a provably-safe exception can come later as a guarded specialization).
- **Fallback for now:** 3D/stacked fetches, `getCompTexLOD`, `getBCF`, shader polygon offset (float24 depth is native since 2026-10-14, `docs/no-converter.md`),
  point-sprite PsParamGen, memexport, dynamic addressing, loops/calls, VS.

## How it plugs in

- `MetalPipelineCache::TryNativeXlate`, on the native-shader pool, runs after the converter library is installed:
  1. translate (about 0.2 ms);
  2. `newLibraryWithSource` (MSL 3.2, `MathModeSafe`);
  3. `MetalTranslation::InstallNativeMetal`, which publishes it as `optimized_function()`.
- Standard pipelines swap through the existing native-shader swap. Emulation (geometry/tessellation) pipelines rebuild
  through `NsEmuSwap`; their build closures pick the entry point per library (`xe_native_ps` or the converter's `main`).
- On failure, the shader goes down the normal cached / `-O2` path (`FABLE2_NS_KEEP_PLAIN` still applies there).
- **Specialization (NS2):** `NsSpecialize` waits for the shader's native decision. For a native shader it bakes the same
  guard entries into the MSL:
  - the translator replaces each `xe_sys[i]` / `xe_bool_loop[i]` / `xe_fetch[i]` read of a guarded dword with
    `(live & ~mask) | value`, or a literal when the whole dword is guarded;
  - it compiles that variant as `Variant::native`.
  - The draw-time guard is unchanged. A converter variant is never bound for a native shader.
- `FABLE2_NATIVE_XLATE_DUMP=<dir>` writes the MSL (game-derived: keep it in `out/`).
- Offline driver `fable2-msl-xlate` (CMake target, EXCLUDE_FROM_ALL):
  `fable2-msl-xlate <shader.ucode.bin.frag> <mod hex> <out.metal> [--scale N]`. It writes the MSL and a JSON sidecar
  (bindings, system-constant offsets).

## Parity

**Differential GPU test** (`tools/shaderlab/xlate_diff.mm`; no game frames, replays or captures):

- It renders the converter metallib dumped from the game (`FABLE2_NS_DUMP_DIR`) and the native MSL through the same
  ABI with identical synthetic inputs, then compares the output bits. Inputs:
  - random interpolants per 8×8 cell, or one full-screen triangle;
  - random float, bool/loop and fetch constants, with fetch fields chosen from valid values;
  - system constants with valid values for the fields that select code paths;
  - random RGBA16F / RGBA8Snorm textures with mips, and random samplers.
- Knobs: `XD_FLAT` (uniform interpolants per cell), `XD_TEX_CONST`, `XD_FULL`, `XD_FLAGS`, `XD_SIGNS`, `XD_BOOL`,
  `XD_DIM`, `XD_POINT`, `XD_ALL`.
- Bisecting a difference: edit the converter's AIR (`metal-opt -S`, then store an SSA value to the output, then
  `metal-as` and `metallib`), and put an `xe_dbg` probe in the MSL (`out/native-xlate/probe.py`).

**Results (Bower 1× and 3× dumps, 182 shaders each):**

- All 182 are bit-identical with uniform per-quad control flow: tame and wild (inf/NaN/denormal) constants, constant
  and random textures.
- With per-pixel interpolants, 93 are identical everywhere. The rest differ only where a 2×2 quad diverges on `p0`
  predication, or at triangle edges (helper lanes): texture LOD derivatives taken in divergent control flow are
  undefined in D3D and Metal, so restructured code cannot match them. Mapping the differing pixels confirms they lie on
  the `p0` boundary.
- Bugs it found, all fixed:
  - the alpha-to-mask dither index (bit order);
  - `frc(±inf)`: the converter gives NaN (x − floor(x)), MSL `fract()` from source gives 0. The translator now emits
    `x - fast::floor(x)`.

**Live** (spin5, `parity.py` per-region linear-light means, NS2 verify on, 2026-10-13):

| Comparison | idle: median / max | idle2: median / max |
|---|---|---|
| 1× off vs off | 1.21% / 7.4% | 6.7% / 38% |
| 1× off vs on (two off runs) | 1.23% / 12.4%, 0.61% / 8.9% | 1.40% / 7.0%, 7.3% / 45% |
| 2× off vs on | 0.42% / 3.0% | 10.6% / 56% |
| 3× off vs on | 0.46% / 7.0% | 15.7% / 66% |

- Off vs on is within the off-vs-off spread. `idle2` is dominated by where the camera stops after the spin.
- The NS2 guard verified 120k / 139k / 147k native-variant hits (1× / 2× / 3×) with 0 misbinds and 0 demotions.
- Renders correctly by eye.

## Results

M5 Max, Bower Lake `hero1-dog-live_2026-10-11_11-20-20`, `headings8` uncapped, pass trace, 13–33 s window, perf
lock, no contention. Warm cache: one off and one native spin5 first, so NS2 data and the OS Metal cache are warm.

| 3× arm | GPU ms/frame | FPS |
|---|---|---|
| Off (converter + `-O2` + NS2, today's default) | 15.31, 15.13, 15.19 | about 64.3 |
| Native, no specialization | 15.26, 15.33 | about 64.5 |
| **Native + specialization** | **14.40, 14.40, 14.27** | **67.4–68.1** |

| 2× / 1× arm | GPU ms/frame | FPS | Main pass (marginal ms) |
|---|---|---|---|
| 2× off | 10.44, 10.40 | 92.7, 93.2 | 2.77, 2.80 |
| 2× native + spec | 9.65, 9.59 (−7.7%) | 100.2, 100.9 | 2.14, 2.11 (−24%) |
| 1× off | 8.00, 8.13 | 118.8, 116.7 | 2.36, 2.37 |
| 1× native + spec | 7.32, 7.43 (−8.6%) | 125.5, 123.0 | 1.75, 1.77 (−26%) |

At 2× the full-screen 1120×720 pass goes 0.40 → 0.31 ms (−23%) and the 320×256 passes 0.51 → 0.48 ms; the other passes are
unchanged.

**M3 Pro projection (estimate).** The M3 Pro is GPU-bound at 2× (27 ms). If its pass mix matches the M5 Max at 2× (the
main pass about 27% of GPU time), −7.7% of the frame is about −2 ms, so roughly 25 ms. That is not 60 FPS on its own:
the main pass is the only large shader-bound item; resolves and the other passes are unchanged by this work.

Per pass at 3× (marginal busy ms/frame, off → native + specialization):

| Pass | Off | Native + spec | Change |
|---|---|---|---|
| Main HDR (1120×720×2) | 3.33–3.41 | 2.75–2.79 | −17% |
| Full-screen 1120×720 | 0.865 | 0.668 | −23% |
| 1280×720 | 0.388 | 0.355 | −8% |
| 320×256 | 0.71 | 0.67 | −5% |

Native without specialization wins the main pass (−10%) but loses the passes where NS2 had removed the most decode
(full-screen +28%).

**Second scene and town (2×, 2026-10-13)** — Old Tomb `hero1-dog-live_2026-10-07_14-45-08`, Bowerstone
`hero1-dog-live_2026-10-04_14-07-34`; headings8, perf lock, warm caches (`CTAG=cache-tomb2/cache-town2 prep.sh 2`):

- **Old Tomb 2×** (2 runs per arm): GPU 6.72, 6.73 → 6.59, 6.46 ms (−3%); busy 6.06 → 5.96; FPS 151 → 153. The main
  pass is −1%: NS2 had already cut it by 39%, and native + spec lands at about the same.
- **Town 2×** (4 runs per arm, interleaved): GPU 11.28 / 12.69 / 11.26 / 11.92 → 11.80 / 12.35 / 11.35 / 12.37. The
  totals are within the scene's noise: crowds move, and two off runs differ by 12%.
  - Per pass, the shader passes are faster: main pass 2.87 → 2.62 ms (−9%), full-screen 0.49 → 0.41 ms (−15%).
  - The non-shader passes, which this work does not touch (`direct_host_resolve`, `tex_upload`), were about 5% higher
    in the native runs. Those runs caught heavier moments of the scene.
- **Live parity** (spin5, idle median / max):
  - Old Tomb: off vs off 0.19% / 0.75%; off vs on 0.40% / 0.88% and 0.31% / 0.63%.
  - Town: off vs off 0.09% / 0.76%; off vs on 0.07% / 0.43% and 0.16% / 0.64%.
  - NS2 verify: 0 misbinds (Old Tomb 48.7k hits, town 163k hits).

**Compile cost:**

- Cold: about 180–200 ms per shader (max 0.86 s) on 4 utility threads. 184 shaders at 3× finish 13.8 s after
  storage init. The converter/`-O2` pipelines are used until each native shader is ready.
- Warm (the macOS Metal cache keeps compiled source): 6 ms in total.
- **Cold first launch** (1×, `out/validation/cold-cache-nx/`, same method as `native-shaders.md` "First launch": a
  re-identified app with its macOS Metal cache cleared, and a seed cache without archives, xfer keys or learn data):

  | Run | Preparation | Warm-up done | Gameplay at | Gameplay hitches |
  |---|---|---|---|---|
  | Off | 6.10 s | 8.83 s (400 compiled) | 10.41 s | none |
  | Native | 6.62 s | 10.01 s (442 compiled) | 10.73 s (+0.33 s) | none |

  - The native compiles (27 s of CPU on the pool) compete with the warm-up. They finish about 30 s in; the cached `-O2`
    shaders run until then.
  - To fold this away on players' Macs: the install warm-up (`tools/fable2_cache_warm.py`) already runs a title
    session per scale. With the switch on, it would fill the OS cache with the native compiles too, so the first real
    launch is warm. `wait_drained` now waits for the pool's `background work done` line after the last
    `[native-xlate]` line, because native compiles log only at powers of two.
  - Tested on scratch state (`--state-root out/validation/warmfold-nx/state`; a re-identified app with its Metal
    cache cleared; the seed cache as above; `FABLE2_NATIVE_XLATE=1`, 1×):
    - the warm-up session waited until all native compiles were done (10.2 s after storage init) and took 51 s;
    - the first game launch after it reached gameplay at 4.37 s, with 7 ms of MSL compile in total and no hitches.

## Vertex shaders

- **Translator:** mirrors DXBC `StartVertexShader_LoadVertexIndex` (line-loop closing index, index endian swap,
  offset, 24-bit mask, min/max clamp), `ProcessVertexFetchInstruction` (shared-memory loads, bounds-checked like the
  converter, endian swap, every format and signed-RF mode, exp adjust, vfetch_mini) and
  `CompleteVertexOrDomainShader` (W/XY/Z flags, user clip planes, NDC scale/offset, vertex kill).
  - Outputs: `user(texcoordN)`, the position `[[position, invariant]]` (compiled with `preserveInvariance`), and
    `[[clip_distance]]`.
  - Like the converter's code, ±Infinity or NaN in position XYZ becomes the canonical NaN.
  - **Not covered** (fallback): domain / tessellation and emulated-primitive host types, point size, cull
    distances, memexport.
- **Wiring:** standard pipelines only. Emulation pipelines use the converter's vertex stage libraries.
  - `CreatePipelineFromHandle` takes `best_function()` for the vertex stage.
  - A pipeline can be swapped twice (native PS, then native VS) through `NativeShaderSwap`. Replaced states are kept
    alive until the cache goes.
- **Exactness** (`tools/shaderlab/xlate_diff_vs.mm`):
  - Setup: random shared-memory vertex data and constants. A probe kernel runs the native vertex code to choose NDC
    scale/offset, so most vertices land on screen. Triangles are drawn with a generated fragment shader that writes
    every output flat to RGBA32F targets, plus depth.
  - **Results:** 61 of 62 Bower VS are bit-identical with random flags. With the shared-memory-as-UAV flag clear, all
    62 are. 48 of them got on-screen coverage; the other 14 put every vertex behind the camera with random constants.
  - **Converter bug found:** in VS `7D1C707013A556BB`'s converter output, the UAV branch of the vertex fetch drops
    the `index * stride` term (it reads `fetch_constant >> 2` directly). It only runs when the shared memory is bound
    as a UAV (memexport draws), which this shader never is. The native code follows the DXBC semantics.
- **Perf** (Bower 3×, native PS in both arms, VS off → on, after merging perf-staging):
  - headings8: 11.76 / 11.70 → 11.49 / 11.50 ms, FPS 82.8 → 84.6;
  - headings8-long: 11.73 / 11.74 → 11.66 / 11.67 ms (−0.6%), so about neutral. The value here is nativeness, not
    speed. The converter's VS was already cheap; vertex work is about 2% of the main pass.
- **Live parity** (spin5, VS on vs off): 1× idle 0.20% / 1.07%; 3× 0.14% / 3.12%. That is within the earlier
  off-vs-off spread at 1×, 1.21% / 7.4%. One arm per scale captured during loading and was discarded.
- **Memexport** (2026-10-14): `ExportToMemory` is ported (eA checks, red/blue swap, every packed / 16-bit / float
  format, endian incl. 8-in-64/128, bounds, sub-dword atomics, 32/64/128-bit stores).
  - The harness compares the shared memory after the draws. `XD_FCONST` crafts a valid stream with a per-vertex
    index.
  - Exact against the converter for every unsigned and float format. `f32tof16` rounds toward zero, as the
    converter's code does; MSL `half()` rounds to nearest, so the translator converts by hand.
  - **Intentional drift — signed normalized/integer formats:** the DXBC translator's rounding step writes its
    `copysign(0.5)` into the data register and then adds an *uninitialized* temporary
    (`dxbc_translator_memexport.cpp`, the `round_bias_temp` BFI). The converter output therefore stores garbage that
    depends on register allocation. The native code applies the intended round-half-away-from-zero. This is not
    faithful to today's output, but it is to the format; flagged here.
  - **Used in play** (2026-10-14, `FABLE2_LOG_MEMEXPORT_FORMATS=1`, 3 spin5 sessions on the Bower, Old Tomb and
    town saves): the same two memexport VS run in all three. 695413A9 exports one stream, `k_32_32_32_32_FLOAT`;
    identical in practice. F6C546D6 (the dog-fur exporter, bug pattern #2) exports three: `k_32_FLOAT` (c8),
    `k_16_16_FLOAT` (c47) and **`k_2_10_10_10` signed integer (c46)**. For c46 the converter's BFI overwrites the
    data with ±0.5 before adding the uninitialized temporary, so today's build loses those values; the native VS
    writes them. This is a faithfulness fix (bug pattern #77), not drift. Dog fur rendered correctly by eye with the
    native VS on; watch it when playtesting the VS toggle.
- **All 70 Bower VS now translate** (loops inside crossing-jump flag ranges and memexport added). With the
  shared-memory-as-SRV flag, all 70 are bit-identical in the harness (42 with on-screen coverage).
- **In game** (Bower 1× spin5, before the last two additions): 62 of 70 VS native. Fallbacks: 7 with a loop inside crossing forward jumps, 1
  memexport. 722 pipeline swaps, 0 failures, verdict ok, renders correctly by eye.

## Geometry and tessellation emulation (scope, 2026-10-13; design only, nothing built)

**What runs today.** Draws that need a geometry or tessellation stage go through MSC's mesh-shader emulation.
- Geometry (`GetOrCreateGeometryPipelineState`, `IRRuntimeNewGeometryEmulationPipeline`): the object stage runs the
  converter VS; the mesh stage runs the generated DXBC GS (`dxbc_geometry_shader.cc`).
- Tessellation (`GetOrCreateTessellationPipelineState`, `IRRuntimeNewGeometryTessellationEmulationPipeline`): the
  object stage runs a fixed VS and HS (`d3d12_5_1_bytecode/*_hs.h`). The mesh stage
  (`irconverter_domain_shader_triangle_passthrough`) runs MSC's software tessellator, the guest VS as a domain
  shader, and a passthrough GS.
- Neither path uses the native VS. Neither is in a binary archive.

**What the game uses** (the stored pipeline list of the Bower cache: 398 standard, 71 geometry, 39 tessellation):
- Geometry: 51 rectangle-list pipelines (full-screen and post passes, mostly one rectangle per draw) and 20
  point-list pipelines. No quad lists. No user clip planes or kill-AND.
- Tessellation: all are quad patches, patch-indexed.
  - Continuous: VS `FB68A7F2`, `C30A97D9`; terrain materials, including `AF1E16EE`. The factor is uniform:
    `xe_tessellation_factor_range.y` on every edge, fractional-even partitioning.
  - Adaptive: VS `5003700B`, `201C64D2`; PS `7DDAD5C2`, `2F8A0341` and depth-only, likely the lake.
    - Edge factor: `clamp(asfloat(endian-swapped index) + 1, min, max)`.
    - Inside factor: the minimum of the two opposite edges.

**Cost** (Bet A 3× Bower capture, `out/validation/betA-20261011/perf-shaders-all.json`; share of the frame's shader
cost):

| Path | Share | ≈ M5 Max 3× | Notes |
|---|---|---|---|
| Adaptive tessellation, mesh stage | **5.1%** (2.67 + 1.10 + 1.09 depth-only + 0.25) | ~0.8 ms | Fixed per frame: does not scale with resolution |
| Continuous tessellation, mesh stage | 0.65% | ~0.1 ms | `AF1E16EE`'s own mesh stage is 0.10%; its 7.6% is fragment work |
| Geometry, object + mesh stages | 0.9% | ~0.15 ms | `0F99810F`'s stages are negligible; its 5.0% is fragment work |

- So the two named hot shaders gain almost nothing from native geometry or tessellation. Their cost is the pixel
  shader, which is already native.
- The lever is adaptive tessellation: about 1 ms fixed on the M5 Max. Scaled by the measured M3 Pro/M5 Max ratio
  (about 2.6×), that is roughly 2–2.5 ms on the M3 Pro at any scale.

**Options.**

1. *Domain mode in the translator* (needed by every tessellation option).
   - `r0.x` = patch index as a float (from the HS control-point phase: `(prim + offset) & 0xFFFFFF`, clamped to
     min/max, `utof`).
   - `r0.yz` = domain (u, v); `r1.x` = 0.
   - The epilogue is the same as the VS.
2. *Uniform-factor tessellation (continuous, discrete) as instanced draws.* Run an exact port of the D3D11
   reference tessellator (public, MIT) on the CPU once per (partitioning, factor bits). Upload the (u, v) list and
   the triangle list. Draw indexed-instanced: one instance per patch, the native DS as the vertex function.
   - Exact if MSC's tessellator is the reference one. Check with a harness that writes the domain location from
     both paths.
   - Small gain (~0.1 ms). It makes the terrain fully native and archivable.
3. *Adaptive, Metal hardware tessellation.*
   - A compute kernel writes `MTLQuadTessellationFactorsHalf` from the index buffer (swap, +1, clamp, inside =
     min).
   - Draw with `drawPatches`/`drawIndexedPatches`, `fractionalEven`, and the native DS as the post-tessellation
     vertex function.
   - Largest gain (estimate 70–85% of the 5.1%). **Not bit-exact**:
     - the factors are half precision;
     - Apple's tessellator is not specified to match the D3D reference (point placement in the fractional
       region);
     - triangle order inside a patch may change blending where translucent water overlaps itself.
   - Needs a faithful switch and a measured diff (preservation first).
4. *Adaptive, compute tessellator + indirect draw* (the exact route).
   - Port the reference tessellator to an MSL kernel. Per patch it writes (u, v, patch) vertices and indices
     through an atomic allocator, then an indirect indexed draw with the native DS.
   - Exact if the port matches MSC. Estimated 40–60% of the 5.1%. Larger work.
5. *Points and rectangles as instanced expansion in a native VS.*
   - Points: 4 corners. Rectangles: 6 vertices. Each corner runs the guest VS body for the vertices it needs:
     1× for points, 3× for rectangles, all single-rectangle draws.
   - Replicate the GS math in the same operation order, with contract off:
     - point radius = diameter × screen-to-NDC × W, corners TL/TR/BL/BR, UV;
     - longest-edge choice and v3 = −v0 + v1 + v2;
     - primitive-wide NaN discard and cull: every corner sees the same inputs.
   - Hardware point sprites are rejected: points are clipped by centre and the size limit is 511 px, so not exact.
   - Small gain (~0.15 ms M5, ~0.4 ms M3 Pro). It removes the last converter VS and the mesh pipelines.

**Recommended order.**
1. Add domain mode.
2. Prototype option 3 behind a switch. Measure with headings8 runs, plus a harness that diffs DS positions between
   MSC and Metal tessellation.
3. If the drift is only low-bit and parity holds, ask the maintainer to accept it, with option 4 as the faithful fallback.
   Otherwise go straight to option 4.
4. Options 2 and 5 last.
5. Before building: a per-scene counter of emulated draws, patches and factor ranges, because the adaptive cost may
   be specific to Bower Lake.

## Geometry and tessellation: results (2026-10-13)

**Census** (`FABLE2_EMU_DRAW_STATS=1`, spin5 1×; `[emu-stats]` averages per frame):

| Scene | Adaptive tess (VS `5003700B`) | Continuous tess | Geometry |
|---|---|---|---|
| Bower Lake | 1.6–2.9k patches, ~0.8–1.5M tris | 290–310 patches, ~150–160k tris | ~45 rectangle draws |
| Old Tomb | 0.3–3.4k patches, up to ~1.7M tris | negligible | rectangles only |
| Bowerstone Old Town (child, winter) | ~1.7k patches, ~0.85M tris | ~415 patches, ~210k tris | **~200k points/frame: the falling snow** (VS `19A01C01`, 48 draws) |

- Every tessellation factor in play is 16. The guest maximum is 15 + 1, and `metal_fable_terrain_uniform_tessellation`
  (default on, a crack workaround) clamps `5003700B`'s adaptive factors to that maximum.
- The snow was identified with `FABLE2_SKIP_VS=<hash>` (diagnostic: skips every draw of that vertex shader).

**Diff harnesses.** `FABLE2_NATIVE_TESS_DUMP=<dir>` dumps the converter's emulation stages:
- tessellation: `tessvs_*`, `hs_*`, `ds_*`, `tess_*.json`;
- geometry: `geomvs_*`, `gs_k*`, `geom_*.json`.

The harnesses build the converter's own emulation pipeline from those dumps and compare it, pixel for pixel, with
the native path under the same synthetic state: `tools/shaderlab/tess_diff.mm` and `tools/shaderlab/geom_diff.mm`.
`fable2-msl-xlate` takes `--points 1|2` and `--rects 1|2` for the expansion variants. Domain shaders translate
with their own modification.

**Native tessellation** (`FABLE2_NATIVE_TESS`, **default 1 = exact only**; `=0` emulation, `=2` every adaptive draw):
- Domain mode in the translator: quad domain, patch-indexed. The entry `xe_native_ds` is a post-tessellation vertex
  function: `r0.x` = patch index (the adaptive hull shader's control-point phase), `r0.yz` = uv, `r1.x` = 0.
- Factors are computed on the CPU from the guest index buffer, exactly as `tessellation_adaptive_vs` and
  `adaptive_quad_hs` do: endian swap, +1, NaN-safe clamp; edge i = control point (i + 3) & 3; inside = minimum of
  the opposite edges. They are written as per-patch half factors. Metal's tessellator is fractional-even with
  **counter-clockwise** output: the hull shader declares CW, but CW culls the opposite side.
- Exactness (harness, all 3 dumped domain shaders, with culling):
  - **Bit-identical** whenever every patch has four equal factors that are exact in half precision. Tested at 16,
    9, 7.5, 7.25, 6, 4 and 2.5, and with per-patch-uniform random factors.
  - 7.3 differs only through half rounding.
  - Non-uniform edges differ in a few percent of pixels near the transition edges for every one of the 24 edge
    mappings. The game's mapping is best: 94% identical with one odd edge. That is Metal's own stitching of
    transition regions, which cannot be made exact.
  - Mode 1 therefore keeps any draw that is not uniform and half-exact on emulation. In play today every draw
    qualifies.
- Live parity (spin5 1×): Bower idle 0.64% / 3.7%, idle2 6.4% / 28%; Old Tomb idle 0.83% / 5.7%, idle2 5.1% /
  35%. Both are within the off-vs-off spread.
- Perf (M5 Max, GPU ms/frame, off → on):

| Scene / scale | headings8 | headings8-long |
|---|---|---|
| Bower 3× | 13.03 → 12.82 | 11.59 → 11.34 (−2.2%) |
| Bower 2× | 9.38 → 9.07 | 7.71 → 7.44 (−3.5%) |
| Old Tomb 2× | 4.74 → 4.95 | 4.94 → 4.98, then 5.03 → 5.00 |

  - Bower main pass: vertex+mesh time 1.08 → 0.86 ms at 3×. Old Tomb is neutral.
  - The domain shader still runs for every tessellated vertex (~1M per frame at factor 16). Only the emulation
    overhead goes away.

**Continuous tessellation on the same path** (option 2, quick version; part of `FABLE2_NATIVE_TESS=1`):
- `continuous_quad_1cp_hs` gives every edge and the inside the maximum factor. The patch index comes from the guest
  index buffer through `tessellation_indexed_vs`: endian swap, offset, 24-bit mask, clamp. The native domain entry
  takes it from `XeTessDraw` at buffer(3): index buffer address, format, and indexed/adaptive.
- Harness: terrain `C30A97D9` and `FB68A7F2` are bit-identical. The selector constant is c254 = 0,1,2,3 for these
  shaders (c255 for `5003700B`).
- **Primitive order caveat:** inside one draw, the tessellator and the emulation emit triangles in different orders.
  Pixels only differ where a draw's own triangles tie in depth or blend over each other. The harness shows this with
  `TD_CLAMP=1`: depth-clamped geometry ties. Opaque terrain patches do not overlap.
- Bower spin5 parity is in noise. The largest idle region difference is the animated breadcrumb trail, present in
  one arm only.
- **Blend/depth state of every tessellated draw** (`[emu-stats] tess state`; Bower, Old Tomb, town):
  - The adaptive terrain `5003700B` is opaque in all its passes (`00010001`, colour or depth-only), with z write
    and zfunc GreaterEqual.
  - Every continuous draw (`C30A97D9`, `FB68A7F2`: the terrain material layers) blends with z write on. The blend
    is `01060106` (src·srcA + dst, additive) or `00060006` (src·srcA + 0).
  - Where a heightfield folds over itself on screen, an additive layer's result depends on which of its own
    triangles draws first, and the order differs.
  - So **mode 1 keeps blended draws on emulation**: in play, every continuous draw. Only the opaque adaptive
    terrain goes native, as measured before. `=2` takes blended draws too.
  - **Decision (maintainer, 2026-10-14): accept the tessellator's triangle order for the blended layers.** Under forced
    depth ties the hardware path differs from the emulation in 2 of 38,298 pixels. `FABLE2_NATIVE_TESS_BLENDED`
    (default on; Dev Launcher "Native tessellation: blended terrain layers") sends blended draws whose factors are
    uniform and half-exact to Metal's tessellator in modes 1 and 3. That is every continuous layer in play.
    Non-uniform blended draws stay emulated, because the reference order was not accepted. `=0` restores the
    emulation for all blended draws.
  - Validation (2026-10-14, `FABLE2_NATIVE_TESS_BLENDED` 0 → 1, 2 `f2diag run still` runs per arm at 2×, pinned
    snapshot):
    - Bower: within noise (cross median 0.30 against a floor of 0.34).
    - Town: cross median 0.51 against a floor of 0.51. Its hot blocks are snow and NPCs; the on/on floor's block
      maximum (146) exceeds the cross maximum (111).
    - Old Tomb: cross median 0.033 against a floor of 0.049. The one hot block (x 1408, y 288, one pair) is a torch
      flame.
    - `[native-tess] N blended draws on the hardware tessellator` counts them, and the "kept on emulation" counter
      no longer appears.
    - Perf, Bower headings8-long, GPU ms/frame (main pass):

| Scale | Off | On |
|---|---|---|
| 2× | 7.41 (2.04) | 7.39 (1.99) |
| 3× | 11.65 (3.22) | 11.59 (3.20) |
- Perf with continuous draws native (mode 2 behaviour), headings8-long, all tessellation off → on: main pass
  1.95 → 1.81 ms (2×) and 2.68 → 2.51 ms (3×). Frame totals are 7.71 → 7.80 and 11.95 → 11.77 ms; the 2× frame
  total is within run-to-run noise in this scene. With mode 1 these draws stay emulated.

**Reference tessellator and a converter bug** (option 4 groundwork, 2026-10-13):
- Microsoft's D3D11 tessellator reference is vendored in `backends/metal/third_party/d3d11-tessellator/` (MIT; see its
  README for the source and commit). It compiles inside a namespace, because its own `fmin`/`fmax`/`min`/`max` clash
  with libc++.
- The domain shader has a third entry, `xe_native_ds_cpu`: a plain vertex function fed from the CPU pattern
  (`XeTessVert` = uv + patch offset at buffer(4); patch = instance + offset). Use `fable2-msl-xlate --ds-cpu 1`.
  `tess_diff` runs it against the converter with the reference's points and indices.
- Uniform factors: the reference reproduces the emulation **bit for bit, including triangle order under forced depth
  ties** (`5003700B`, factor 16 and 7.3; 7.3 is exact here because there is no half rounding).
- Non-uniform factors: neither the reference nor Metal's tessellator matched, for any edge permutation. The edge map
  `2222` matched **100%**.
- **Converter bug:** the converter's emulation of `adaptive_quad_hs` gives all four edges control point 2's factor.
  The fork phase writes `o[r0.y]` with a dynamic output index, and only the last fork instance's value
  (cp[(3 + 3) & 3] = cp2) survives on every edge.
  - The inside factors are right.
  - So with adaptive factors, two patches sharing an edge each tessellate it with their own cp2: T-junctions, which
    are the terrain cracks behind `metal_fable_terrain_uniform_tessellation`.
  - The native paths map edges as the hull shader says: edge i = cp[(i + 3) & 3].
- **Adaptive factors natively** (`FABLE2_NATIVE_TESS=2` with `--metal_fable_terrain_uniform_tessellation=false`):
  - Bower's terrain factors are 2–16, mean 8.7. About 0.5M tessellated triangles per frame, against 0.85–1.5M at the
    clamped maximum.
  - Bower 2× headings8-long: 7.67 → 7.45 ms (main pass 1.84 → 1.75) against today's default.
  - No cracks were visible in the Bower camp spin. The overlook where the cracks were seen still needs a look in play.
  - Switch: `FABLE2_TERRAIN_ADAPTIVE=1` (Dev Launcher "Adaptive terrain tessellation (fixes cracks natively)", default
    off) drops the clamp and sends opaque adaptive draws to the tessellator whatever the factors.
  - Metal's tessellator differs from the reference only in how it stitches transition regions. The reference
    pattern path (option 4) would make that exact as well.
- Triangle order in blended continuous draws: under forced depth ties the hardware path differs from the emulation
  in 2 of 38,298 pixels, and the reference order in 1,376. The emulation's order is therefore neither, and order
  equality cannot be proven either way. Those draws stay emulated in mode 1.

**Exact tessellation with the reference** (option 4; part of `FABLE2_NATIVE_TESS=1`, all draws with `=3`):
- In mode 1, a draw whose patches are not all uniform and half-exact (in practice: `FABLE2_TERRAIN_ADAPTIVE=1`) is
  drawn from the D3D11 reference tessellator's patterns, not Metal's tessellator. This gives exact D3D semantics and
  correct edges.
- Patterns are cached per factor tuple (`metal_ref_tessellator.cc`). Their points live in a 1M-point uv arena, and
  each pattern keeps its local triangle list. Bower uses ~400 patterns.
- Each guest draw becomes **one** indexed draw: index = (patch << 20) | arena point, uploaded per draw. The native
  domain entry `xe_native_ds_cpu` splits the index into the patch and the point. Draws too large for one upload fall
  back to instanced runs of patches that share a pattern.
- Harness: with the converter's edge bug reproduced (`TD_EDGE_MAP=2222`), the reference path matches the emulation
  for non-uniform factors (1 differing pixel in 1,610). It is bit-identical at uniform 16 and 7.3, culling included.
- Bower 2× headings8-long, GPU ms / FPS:

| Configuration | GPU ms | FPS |
|---|---|---|
| Default (clamp on) | 7.58 | 121 |
| `TERRAIN_ADAPTIVE` + mode 1 | 7.77 | 117 |
| `TERRAIN_ADAPTIVE` + mode 3 | 7.47 | 120 |
| `TERRAIN_ADAPTIVE` + mode 2 (hardware, stitching drift) | 7.38 | 124 |

  - The exact path costs ~0.3 ms of render-thread CPU per frame, writing ~1.6M indices.
  - GPU time is between the default and the hardware path, within run-to-run noise of both.

**M3 white flashes (fixed 2026-10-15, [adaptive-terrain-flashes-20261014.md](adaptive-terrain-flashes-20261014.md)).**
- Cause: with `FABLE2_TERRAIN_ADAPTIVE=1` the blended terrain layers kept the uniform maximum on Metal's
  tessellator, while the opaque terrain and its prepass under them used the guest's adaptive factors on the
  reference path. Wherever a layer fell behind the prepass depth (GreaterEqual), the unlayered base showed, and it
  is white. Fix: `FABLE2_TERRAIN_LAYER_MATCH` (default on); the layers take the opaque patch's factors and path.
  The notes below are the investigation as it went.
- The M3 Pro test Mac (apple9) shows white flashes on geometry while the camera moves, with native tessellation on. The
  M5 Max (apple10) does not, and turning native tessellation off fixes it.
- Diagnostic: `FABLE2_NATIVE_TESS_TRACE=1`, which logs the path per VS/PS combination, factor ranges, depth state and
  per-path counts.
- Hypothesis: the blended terrain layers and the depth-only prepass depend on bit-identical positions across
  pipelines (zfunc GreaterEqual). Apple GPUs run tessellation through per-pipeline driver code, so the last bits of
  the uv could differ between pipelines on one family.
- Prototype: `FABLE2_NATIVE_TESS_SNAP_UV=1` snaps the uv to the D3D11 1/65536 grid. Harness on the M5: identical
  with and without it, for adaptive and continuous, at factors 16, 9, 7.25 and 2.5. It stays default off until there
  is M3 evidence.
- Flash kit ([flash-kit.md](flash-kit.md), 2026-10-14): detects the flashes automatically per arm (adaptive on /
  + snap / + reference tessellator, adaptive off / + snap). The M5 Max does not reproduce them with
  `FABLE2_TERRAIN_ADAPTIVE=1`: all arms are at the detector floor. SNAP_UV now logs `[tess-trace] snap-uv: on`
  and each domain shader compiled snapped. The earlier M3 Pro Snap log already showed it took effect: the 5003700B
  domain-shader translation grew from 20733 to 20919 bytes.

**Native rectangles** (`FABLE2_NATIVE_RECTS`, **default on**, `=0` emulation):
- Entry `xe_native_vs_rects`. Each strip instance runs the guest VS body for the rectangle's three vertices
  (index buffer read in the shader) and replicates the geometry shader in the same operation order:
  - longest-edge choice from squared edge lengths;
  - strip `o.x, o.y, o.z` and the fourth vertex `(-a + b) + c` for every output;
  - primitive-wide NaN discard.
- Harness: bit-identical for all 6 rectangle VS variants, full-screen coverage.
- Parity: in noise (idle 0.74% / 2.8%).
- Perf, Bower 2× headings8-long: 7.62 → 7.64 ms (neutral).

**Native point sprites** (`FABLE2_NATIVE_POINTS`, **default 2**; `=1` per-corner, `=0` emulation):
- Mode 2 is a native **mesh function**, with no object stage: 64 points per threadgroup. Each thread runs the guest
  VS once for its point (index buffer read in the shader) and emits the sprite with the point-list geometry
  shader's math, in its order:
  - NaN or size ≤ 0 drops the sprite;
  - radius = (diameter × screen-to-NDC) × W;
  - corners TL, TR, BL, BR as triangles (0, 1, 2), (2, 1, 3); point coordinates per corner.
- Entry `xe_native_ms_points`. An object/mesh pair with the outputs in the payload crashed the Metal compiler
  (XPC interrupted) for the larger point shaders; the mesh-only design does not.
- Mode 1 (`xe_native_vs_points`, triangle-strip instances) runs the VS per corner. The snow VS (~3,300 MSL lines)
  then costs 4× and the town main pass got slower (2.19 → 2.40 ms at 1×).
- Harness (`geom_diff`, 300 points across threadgroups, large sizes, zero-size culling): **bit-identical** for all 3
  point VS (snow `19A01C01` with size and coordinates, `218AD033`, `B6C9863F`) in both modes.
- Town parity is in noise. The early `idle` captures fall in the load fade-in, whose timing varies per run; that
  dominates the region means. Per-capture brightness converges to ~0.185 in every arm.
- Perf, town, headings8-long (off → mode 2): 1× 9.35 → 9.23 ms (main pass 2.93 → 2.91); 2× 10.70 → 10.35 ms (main
  pass 3.07 → 3.02).
- Skipping the snow entirely saves only ~0.06 ms of the main pass, so the emulation's own cost was small.
- Diagnostic: `FABLE2_NATIVE_POINTS_ONLY=<VS hash>` limits native points and rectangles to one VS.

## Plan

1. Offline tool target: translate the dumped ucode, then `xcrun metal` compile and AIR diff against MSC output.
   Report the coverage rate.
2. Backend hook: after the MSC install, translate, compile with `newLibraryWithSource` on the native-shader pool, and
   publish through the `optimized_function()` swap (standard and emulation pipelines). Behind `FABLE2_NATIVE_XLATE`
   (default off) with a Dev Launcher toggle; per-shader fallback.
3. Parity: live spin5 captures, on vs off (no gputrace replays without heavy-mode approval).
4. Perf: headings8 at 1×/2×/3× under the perf lock; per-pass share for the M3 Pro projection; cold compile cost and
   cold-launch delta (baseline: cold 1× launch 10.5 s).
5. Grow: specialization in MSL (learned constants as literals), 3D fetch, float24 depth, then VS, then memexport.

## Log

- 2026-10-13: slot agent-1; route study; census; ABI and exactness probes; emitter draft (ALU, CF, 1D/2D/cube fetch,
  OM). Paused for the XeniOS evaluation (borrow, not adopt).
- 2026-10-13: wired in (pool, swap, emulation pipelines); differential test; 2 exactness fixes; native NS2 variants;
  3× perf.
- 2026-10-13: crossing forward jumps (flag mode); backward jumps are refused explicitly; 182/184 exact at 1× and 3×.
- 2026-10-13: install warm-up fold tested on scratch state; loops, relative constants, point sprites → 184/184.
- 2026-10-13: Old Tomb and town at 2×: perf and parity. VS translator started.
- 2026-10-14: **vertex shaders**, wired into standard pipelines (`FABLE2_NATIVE_XLATE_VS`, on with the translator; Dev
  Launcher toggle "Native translator: vertex shaders too"). See "Vertex shaders" below.
