# No shader converter: native first, and a build without DXC / MSC (2026-10-14)

Goal (maintainer, 2026-10-14): "as native as possible". The native Xenos → MSL translator (`docs/native-translator.md`)
already ran every pixel and vertex shader, tessellation, rectangles and points in play. The DXBC → DXIL (dxilconv,
"DXC") → Apple Metal Shader Converter ("MSC") chain was still linked and used, so the MSC licence question
(release decision D3, 2026-10-14) stayed open.

**Result:**
- With `FABLE2_NATIVE_FIRST` (**default on**; `=0` restores the converter-first path) a run touches neither
  converter. Over 9 runs (Bower Lake, Old Tomb and Bowerstone, each at 1×, 2× and 3×) there were no dxilconv calls, no
  MSC calls and no converter libraries. Every draw had native stages.
- `FABLE2_NO_CONVERTER=1` at build time leaves dxilconv and MSC out entirely:
  - the DXC tree is not downloaded or built;
  - neither dylib is linked or bundled;
  - the app shrinks by 65 MB, from about 191 MB to 126 MB;
  - anything that would still need the converter logs an error (below).
- The no-converter build runs the same scenes with 0 refusals, also with `depth_float24_convert_in_pixel_shader` on
  (truncating and rounding).

## Release decision (maintainer, 2026-10-14)

"Go with the no-converter build" (release decision D3, 2026-10-14, decided). Releases, portable zips and
benchmark kits ship the `FABLE2_NO_CONVERTER` build:
- `python3 tools/build_macos_metal.py --release` builds it into `out/build-release/mac-arm64-metal-release`, with its
  own backend tree (`out/metal-build/backend-release`). The dev build (`out/build`, `out/metal-build/backend`) keeps the
  converter as a fallback while testing, and neither build reconfigures the other. The game executable
  (`out/build/mac-arm64-release`) does not depend on the converter and is shared.
- `tools/fable2_portable.py build` and `tools/fable2_benchmark_kit.py build` (and the Dev Launcher's Export Portable
  Build) run that build first (incremental; `make` decides what is stale; log `out/build-release/build.log`), then
  package it. `--no-rebuild` packages it as it is; `--with-converter` packages the dev build with both libraries, as
  before.
- Both packagers refuse a no-converter package that contains `libdxilconv.dylib` or `libmetalirconverter.dylib`,
  links either, holds a `DirectXShaderCompiler*` licence text, or ships a `THIRD_PARTY_NOTICES.md` that names either
  library. The shipped notices file (`package_macos_app.notices_text`) drops the dxilconv row and keeps one row for the
  Apache-2.0 MSC runtime headers, which are still compiled in.

Check (M5 Max, 2026-10-14, evidence `out/validation/noconv-release-20261014/`, KEEP):
- The first `--release` build of a slot whose dev build was current took 25 s. A refresh with nothing to rebuild takes
  15–40 s.
- `fable2_portable.py build --seed-source prep` packaged it in 64 s:
  - app 134 MB, zip 41.0 MB;
  - lean seed: the pipeline lists only;
  - no converter dylib or DirectXShaderCompiler licence in the app.
- The unzipped app was run from a scratch copy, through its player path (`--play`), after the player's own shader prep:
  - spin5 at 2×, Bower Lake: verdict ok, 0 AVs;
  - `[converter-use] totals: none` (676k native draws, 177k native tessellation draws, 0 emulation);
  - no `NO-CONVERTER BUILD … refused` line.

## Inventory before (census, 8601508)

The census is always on (`metal_converter_census.h`, `[converter-use]` lines). It logs the first use per purpose, with
the shader hash, and then a summary at most every 10 s while anything changes. It counts:
- DXIL requests: run, or served from the artifact cache;
- MSC requests: run, or served from the stage cache;
- converter-built `MTL::Library`s;
- draws, by pipeline kind.

Baseline spin5 runs, warm agent cache, 1×/2×/3× in Bower, Old Tomb and town:

| Use | Why | Per run |
|---|---|---|
| Guest VS / PS (70 / 184) | Every shader was converted first; the native translation was compiled later and swapped in | 254 converter libraries. dxilconv 0 runs (cached); MSC 0 runs at 1×/2×, 80 at 3× (agent cache short of 3× stages) |
| Emulation prewarm | The stored pipeline list rebuilt every geometry / tessellation emulation pipeline synchronously on the GPU Commands thread. Play never used tessellation emulation | Geometry VS 13, GS 6; tessellation VS 6, HS 2, DS 6; depth-only PS 1. The cold kit run spent 5.7 s of startup here |
| Geometry emulation in play | No-PS rectangle/point draws (native points needed a fragment function) and the memexport point list of the dog-fur VS `F6C546D6` | 2.4k–4k draws per session |
| NS2 specialization | Learn keys were the hash of the converter metallib; guard entries came from baking the converter's AIR | All specialized shaders |
| Build | dxilconv built from source; the MSC tarball downloaded; both dylibs linked and bundled. No helper shader uses either: the resolve / texture-load / tile helpers are XeSL compiled as MSL by `metal`. The `d3d12_5_1_bytecode` tessellation VS/HS are FXC-made DXBC, used only by the tessellation emulation | 7 MB + 58 MB in the app |

The kit's `pso_emu` hitches (cold arm 122 ms over 3 hitches) were not converter pipelines. They were native
rectangle / point pipelines built on the draw path. They were keyed on the PS function pointer, so they were built
again when a PS swapped from the converter to native (A4A965 was built 4 times).

## What changed

All behind switches, with Dev Launcher toggles ("Native shaders from the first frame (no converter)", "Native point
sprites: memory-exporting shaders too").

1. **`FABLE2_NATIVE_FIRST`** (`MetalPipelineCache::TryNativeFirst`, `MetalTranslation::InstallNativePrimary`).
   - When a guest shader is first needed, it is translated to MSL and compiled. That library is its only one,
     published as the optimized function too, so no DXIL, no MSC, no swap and no second pipeline build.
   - The DXBC translation (the SDK's own translator) still runs: the texture/sampler binding lists come from it.
   - A shader the translator refuses takes the converter path, or is refused in a no-converter build.
   - In play, none is refused.
2. **NS2 specialization for native-first shaders.**
   - Key: the tagged hash of the DXBC translation. It encodes ucode, modification and scale, as the converter
     library did.
   - Guard entries: the learned candidates the native source reads (`MslTranslationResult::baked_used`). Where
     converter-era variants of the same shader exist (NS2 index records of the sibling keys), they are narrowed to
     the dwords and bits those had proven live. Without the narrowing, dead reads were guarded and missed.
   - A key with only sibling (other-scale) learn data first auto-learns its own statistics. Specializing from other
     scales baked the wrong render-target texture sizes into fetch constants: about 1,000 guard misses and 1 demotion
     per Bower 2× run before this fix, 0 after.
   - Needs no `metal-opt`.
3. **Native depth-only PS.** The DXBC translator's depth-only shader for render-target (non-ROV) output is empty. Native
   points / rectangles without a PS now bind the same empty MSL fragment instead of falling back to emulation.
4. **Memexport point lists** (`FABLE2_NATIVE_POINTS_MEMEXPORT`, default on).
   - The mesh expansion runs the VS once per point (threads past the count skip it), exactly as the emulation's object
     stage does. So the dog-fur exporter is native too.
   - It writes the signed `k_2_10_10_10` stream the converter dropped (bug pattern #77, already the case for the
     native VS).
5. **Background native-geometry prewarm** (`PrewarmNativeGeometry`).
   - Under native-first, the stored geometry/tessellation records whose draws take the native path are prepared as
     native pipelines on a background thread at storage init. They are not built as emulation pipelines on the GPU
     Commands thread.
   - The native tessellation / point / rectangle maps are now guarded by `native_geom_mutex_`.
   - The pending count is part of `ReadinessBackground().pipelines_pending` (used by the offline-seed scan).
6. **Float24 depth output** (`depth_float24_convert_in_pixel_shader`, truncating or rounding with
   `depth_float24_round`). The translator converts the depth in the epilogue exactly as
   `CompletePixelShader_DSV_DepthTo24Bit` does:
   - truncation drops the low mantissa bits (`[[depth(less)]]` when the shader does not write depth);
   - rounding goes through `PreClampedDepthTo20e4` and `Depth20e4To32`.
   The DXBC declares sample-rate position for this, but the converter's output converts the pixel-centre depth; the
   native code does the same (with 4x MSAA only edge pixels hold per-sample depths in both).
   - Exactness (`xlate_diff`, with new `XD_MSAA=4` per-sample depth readback and `XD_ZWIDE` depth over 0…0.5 incl.
     tiny values): all 65 truncating and 62 rounding pixel shaders dumped from two sessions are bit-identical at 1×
     and 4× MSAA. The 184 non-float24 shaders of the same dumps stay identical.
   - Negative control: a 1-ulp change of the truncation threshold is caught (3 differing samples).
7. **`FABLE2_NO_CONVERTER=1` build.**
   - `tools/build_macos_metal.py` skips the DXC download and build, and passes `-DFABLE2_NO_CONVERTER=ON`.
   - CMake compiles `metal_converter_stubs.cc` instead of the two converter wrappers, and links neither dylib.
   - `tools/package_macos_app.py` sees from `otool -L` that the plugin does not link them, bundles neither, and skips
     the DirectXShaderCompiler licence texts.
   - `metal-build.json` records `"shader_converter": false`.
   - The MSC runtime headers (Apache-2.0: types, and the header-only geometry/tessellation emulation runtime) are
     still compiled in, so their licence and acknowledgements stay in `Licenses/`.
   - **Loud failure:** every converter request in such a build is refused. The first refusal per purpose, and then
     every power of two, logs `[converter-use] NO-CONVERTER BUILD: <purpose> shader <hash> needs the converter
     (refused #n)` at error level. The summary line shows `REFUSED n`.
   - The native-path switches are forced on there (below, "Forced switches").

## Results

**Census with native-first** (spin5, 3 scenes × 3 scales):
- no converter use at all;
- standard draws all native; native geometry 15–31k and native tessellation 54–174k per session;
- 0 geometry or tessellation emulation draws;
- `pso_emu` hitches 0 ms in all 9 runs (baseline 0–124 ms);
- the native-geometry prewarm prepares 102–110 pipelines in 0.3–0.8 s (warm OS cache).

**Parity** (frame-matched `f2diag run still`, 2 runs per arm, 1×, `FABLE2_NATIVE_FIRST` 0 vs 1, pinned snapshot):
- Bower: within noise (cross median 0.50, floor 0.46).
- Old Tomb: within noise (cross 0.048, floor 0.029, noise limit 0.086).
- Town: the verdict is "beyond-noise", but every hot block involves one run (nf1-1) whose bursts matched at the ±4
  frame shift edge (crowd timing). The time-aligned pair nf0-2 × nf1-2 has median 0.15 and block max 8.9, the
  cleanest of all pairs; the floors are 0.73 and 1.37. The cross median (1.04) equals the floor median (1.05).

- Town rerun (`--at-frame load+700`, because native-first first auto-learns NS2 on new keys, so readiness came after
  `load+240`): again "beyond-noise" only through one run (nf1-1) whose bursts matched at the ±4 shift edge (its floor
  pair with nf1-2 has block max 166). Cross median 0.43 is below the floor median 0.83, and nf0-1 × nf1-2 has
  median 0.18, the lowest pair. Crowd timing, not rendering.

**Cold start** (Bower 1×, `--cache-snapshot cold`, 2 runs per arm, the OS Metal cache warm):

| Arm | Ready (s) | Draw-path pipeline time (`pso_*` in hitch lines) |
|---|---|---|
| Converter first (`=0`) | 16.7, 16.6 | 1134 ms, 64 ms |
| Native first | 12.0, 12.1 | 0.9 ms, 0.8 ms |

**No-converter build** (`FABLE2_NO_CONVERTER=1`, default settings): Bower, Old Tomb and town at 1× and 3× all
clean, every draw native. With the float24 cvar on (truncating and rounding) Bower 1× is clean too, with no
refusals. The first boot of this build crashed at startup (converter initialisation was still fatal); fixed, init is
now only fatal in a converter build.

**Perf** (M5 Max, Bower 2× headings8-long, uncapped):

| Arms | Cache | FPS (pacing) | GPU ms/frame (12–30 s) |
|---|---|---|---|
| nf0 / nf1, NS2 off | shared learned cache | 129.6, 131.8 / 128.7, 130.2 | 7.46, 7.28 / 7.50, 7.46 |
| nf0 / nf1 | cache learned by a native-first session | 131.1, 132.6 / 130.3, 130.3 | 7.22, 7.18 / 7.23, 7.42 |

- Native-first is about 1% lower in these runs, close to run-to-run noise.
- Its readiness comes 0.4–0.6 s later (spec auto-learn), so the sweep starts at a later game moment.
- Shader code is identical in both arms (the same native translation), apart from the NS2 guard sets.

## What still needs the converter

Nothing in the three test scenes. Paths that would still refuse in a no-converter build (none seen in play):
- **geometry emulation** for quad lists, clip-plane cull or vertex kill-AND, rectangle lists from a memexport VS, and
  no-PS emulated draws when native-first is off;
- **tessellation emulation** for triangle patches, discrete mode, non-indexed control points, memexport domain
  shaders, and non-uniform blended draws;
- shaders the native translator refuses: none in play. The translator still refuses shader polygon offset
  (`kPolygonOffset` modes; the Metal backend never selects them) and dynamic register addressing. Fallback reasons
  are logged as `[native-xlate] … fallback`.

Closing these is case by case, when a scene shows them (the census logs the VS/PS/key of each emulated draw once).

## Forced switches (2026-10-14)

The M3 Pro playtest of the no-converter portable (51757bd): with `FABLE2_NATIVE_XLATE=0` it showed the white
opening, then black; with `FABLE2_NATIVE_TESS=0` boot and save load were normal, but gameplay was black (audio and
input fine). Without a converter those native paths are the only shader paths.

- **The game.** A no-converter build ignores `=0` for `FABLE2_NATIVE_SHADERS`, `_XLATE`, `_XLATE_VS`, `_FIRST`,
  `_TESS`, `_TESS_BLENDED`, `_POINTS`, `_POINTS_MEMEXPORT` and `_RECTS` (`NativeSwitchValue`,
  `metal_air_optimizer.cc`). It also ignores `FABLE2_NATIVE_POINTS_ONLY`. Each logs one line, e.g.
  `[no-converter] FABLE2_NATIVE_TESS=0 ignored: no converter fallback in this build`.
  - Non-uniform blended tessellated draws (only with `FABLE2_TERRAIN_ADAPTIVE=1`) take Metal's tessellator, as in
    `FABLE2_NATIVE_TESS=2`, instead of the emulation they would be refused on.
  - Other values (`FABLE2_NATIVE_TESS=2/3`, `FABLE2_NATIVE_POINTS=1`) are native and still honoured.
  - `FABLE2_NOCONV_ALLOW_OFF=1` (diagnostic, no launcher toggle) honours the `=0`s anyway, to test refused draws.
  - Converter builds are unchanged.
- **Player app and Dev Launcher** (`LaunchSettings.requiredWithoutConverter`, `SettingsModel.converterProbe`).
  - When the app they launch has no `libmetalirconverter.dylib` in `Contents/MacOS`, those toggles show on and
    disabled, with "Required in this build: it has no shader converter to fall back to."
  - Their `=0` is not passed. The stored choice is kept for converter builds.
  - The player app checks its own bundle; the Dev Launcher checks the build it launches (playtest copy or latest).
- **Why tessellation off blacked out the whole frame.** It was not EDRAM or resolve state.
  - A refused tessellation-emulation pipeline was not skipped as the geometry path's is. `DispatchDraw` then
    dereferenced the null `TessellationPipelineState`.
  - The guest exception handler returned, the load faulted again, and so on forever. The GPU Commands thread spun
    at 100% inside the first such draw, and no guest frame was submitted again.
  - The screen kept the last presented frame, and audio and input went on.
  - Now such a draw is skipped (`ds_tessellation_pipeline_failed`, one warning), and `DispatchDraw` null-checks.
  - With the diagnostic override and `FABLE2_NATIVE_TESS=0` at the Old Tomb cave, 2,166 tessellation draws were
    refused. The frame rendered and there was no hang.
- **Check** (M5 Max, Old Tomb cave save, frame-matched stills; evidence in the main checkout, KEEP: `out/validation/noconv-force-20261014/`):

  | Build | Setting | Result |
  |---|---|---|
  | No converter | `FABLE2_NATIVE_TESS=0` | Gameplay renders, one "ignored" line, `[converter-use] totals: none` |
  | No converter | `FABLE2_NATIVE_XLATE=0` | Gameplay renders, one "ignored" line, `[converter-use] totals: none` |
  | Converter (dev) | `FABLE2_NATIVE_TESS=0` | Honoured: native tess 0, emulated tessellation through the converter |

  The `--release` build also compiles with the new log line.

## Switches and tools

- `FABLE2_NATIVE_FIRST` (default 1), `FABLE2_NATIVE_POINTS_MEMEXPORT` (default 1): Dev Launcher toggles.
- `FABLE2_NATIVE_GEOM_PERSIST` (default 1, Dev Launcher toggle): native point/rectangle/tessellation pipelines are recorded for the next launch's prewarm and archived; without it they were rebuilt on the draw path every launch (`docs/prologue-stutter-20261014.md`).
- `python3 tools/build_macos_metal.py --release`: the no-converter release build in `out/build-release` (what the
  packagers ship). `FABLE2_NO_CONVERTER=1 python3 tools/build_macos_metal.py` still turns the dev build in `out/build`
  into a no-converter build; building it again without the variable restores both dylibs.
- Census: `grep '\[converter-use\]' fable_2.log`. `totals: none;` means a converter-free run.

Evidence (main checkout, KEEP): `out/validation/no-converter-20261014/` (census summaries, parity outputs, perf and cold-start logs, the float24 harness script, every run's `result.json`).
