# First-play shader hitches: compiling the disc's shader bank at install (2026-10-15)

Goal: remove the once-per-new-effect hitches of a first play (~150–220 ms each on an M3 Pro: `xlate` and `pso_emu`
when a shader is first seen) without shipping anything derived from the game. The release ships no shader recipe
(decision of 2026-10-14, `docs/offline-shader-seed.md` §6), so everything has to come from the player's own disc on
their Mac.

Result: the install's headless shader prep now reads the disc's shader bank and compiles every pixel shader variant
the draw path is likely to ask for. At the first play, draw-path translation (`xlate`) is gone and hitch time in the
first minute drops by about three quarters. Pipeline builds (`pso_emu`) are what remains.

## 1. The shader bank can be enumerated without a recipe

`data/Shaders/ShadersRelease.sbk` holds 147 zlib entries with 583 D3D shader containers (451 pixel, 132 vertex)
(format: `tools/fable2_shader_bank.py`, `backends/metal/port/rex/graphics/metal/metal_shader_bank.h`). Each container
is big-endian:

| Offset | Content |
|---|---|
| +0 | magic `0x102A1100` (pixel) / `0x102A1101` (vertex) |
| +4, +8 | offset and size of the data block: the literal constants, then the microcode |
| +24 | offset of the shader info block: word 1 = microcode size; word 2 = `SQ_PROGRAM_CNTL`, word 3 = `SQ_CONTEXT_MISC`, word 6 = `SQ_INTERPOLATOR_CNTL`-like (low half: interpolators the shader reads, high half: centroid sampling pattern) as D3D sets them with the shader |

Against the shaders the game used on the development Mac (all play to date: 144 pixel, 78 vertex shaders):

- every pixel shader is a byte-identical microcode slice of a container (144/144);
- 6 vertex shaders are byte-identical; 70 are a container's microcode with D3D's bind-time vertex-fetch patching
  (they cannot be predicted from the disc); 2 are not in the bank (D3D-internal).

The register words make the pixel-shader *modifications* (the translation key besides the hash) predictable: param
gen on/off and its interpolator, the interpolator mask and the centroid mask under MSAA come from the container.
Checked offline against the remembered pipeline list: 215 of 218 (pixel shader, modification) pairs fall in the
predicted set; the misses are point-sprite param gen (1) and the native-scale bit (2).

What stays draw state, so each pixel shader gets a few variants: MSAA or not (centroids), the depth mode (early-Z
hint unless alpha test / alpha to coverage; float24 conversion with a D24FS8 depth buffer), and at 2×/3× the effects
footprint variant for shaders with offset fetches (post-processing, no MSAA). The 439 distinct pixel shaders give
~1,850 translations at 2× (~1,300 at 1×).

Vertex shaders: only the 11 without vertex fetches cannot be patched; they are compiled as plain vertex shaders and
as their native expansions (rectangles, point sprites, terrain domain shaders with and without one clip plane).
Three of them are the terrain domain shaders. The other 121 are left to the draw path (`FABLE2_SHADER_PREP_BANK_VS=all`
compiles them unpatched, which the game never uses: ~95 s of extra compiles for nothing).

## 2. Implementation (`FABLE2_SHADER_PREP_BANK`, default on)

- `metal_shader_bank.h`: reads the bank (zlib), returns microcode + the three register words per container.
- `MetalPipelineCache::StartBankPrecompile` (headless prep only): loads each shader (`LoadShader` + analysis; not
  added to the shader list `.xsh`, which stays "what this Mac played"), builds the predicted modifications, and runs
  the translations on N threads (`FABLE2_SHADER_PREP_BANK_THREADS`, default cores − 1, at most 11): pixel shaders are
  installed exactly as the draw path does (`EnsureMetalTranslationReady`, native MSL); expansions are compiled and
  discarded. The point is macOS's Metal cache (per bundle id): at the first play the same MSL compiles hit it. Work
  is ordered most-common first (no-MSAA pixel variants, then MSAA and vertex shaders, then float24, then footprint)
  under a time budget (`FABLE2_SHADER_PREP_BANK_BUDGET_S`, default 120 s; 0 = none). Pending work counts in the
  prep's readiness (`ns_pending`); the status file has a `bank` object (shaders, translations, ready, failed,
  skipped, pending, ms).
- `RunShaderPrep` (`metal_graphics_system.cc`) starts it on the command-processor thread; the bank path is
  `$FABLE2_GAME_DATA_ROOT/data/Shaders/ShadersRelease.sbk` (the game sets that variable from its data root when it is
  unset) or `FABLE2_SHADER_PREP_BANK=<file.sbk>`; `=0` turns it off.
- `[xlate-trace]` (`FABLE2_XLATE_TRACE`, default on): one log line per translation ≥ 10 ms with shader, modification
  and thread, so first-play hitches can be attributed.
- **Player app** (`ShaderPrepRunner`): the prep now also runs on a first install (no lists yet) when the disc's bank
  is there and the setting is on; the bank is compiled at the first prep scale only (the Play window's scale), the
  other scale prepares the renderer's own pipelines in ~1 s. Progress shows "N of M from the game disc"; the marker
  records `bank: true`, so turning the setting on later runs it once. Skip still works.
- **Launcher / source install**: the shared setting "Prepare all of the disc's shaders at install" (Settings,
  Performance, Startup and CPU; `FABLE2_SHADER_PREP_BANK`); the setup app's shader step
  (`tools/fable2_cache_warm.py --headless`, since 2026-10-19) runs the bank prep at the player's scale on a clone of
  the player's cache and publishes it (`--no-bank`, the setting or `=0` skip it); the older session modes run it on a
  scratch cache that is discarded. `tools/fable2_shader_prep.py run --empty` does it by hand.

Nothing in the repo, the app or the release zip holds game data: the bank is read from the player's extracted files
at install, and what is compiled lands only in macOS's Metal cache on their Mac.

## 3. Install time

M5 Max (18 cores, 11 threads), cold macOS Metal cache, `tools/fable2_shader_prep.py run --empty` (another agent's
game was running; in-process times, the tool's wall times include agent lock waits):

| Step | Translations | Time | Notes |
|---|---|---|---|
| prep at 2× with the bank | 2,313 jobs (~1,850 pixel translations, the rest vertex installs and expansions) | **45 s** | 489 s of MSL compiles summed; the renderer's own transfer / resolve / tile pipelines (78 + 134 + 16) build alongside |
| prep at 1×, bank off (the player app's second scale) | — | 0.6 s | |
| same 2× prep again (warm Metal cache, e.g. a new version with the same shaders) | 2,313 | 0.3 s | all cache hits |
| bank at 1× instead (if a player's first scale is 1×) | 1,581 | 11–12 s after a 2× run (cold not measured) | no footprint variants at 1× |

Before this change a first install of the release app ran no prep at all (nothing remembered), so this is +~47 s on
the M5 Max. **M3 Pro estimate:** its cold MSL compiles ran ~385 ms each against ~120 ms on the M5 (first-play logs),
with 12 cores instead of 18: 2.5–3.5× slower, ~110–160 s for the full set, so the 120 s budget applies and it ends
with the most common variants done. Base M1 (8 cores, 7 threads): the budget, 2 min. Tests: a 5 s budget left 1,958
of 2,313 for the draw path as designed.

## 4. Measurements: cold first play, before and after

Harness and evidence: `out/validation/shader-precompile-20261015/` in the main checkout (KEEP; `ab.py` from the prologue-stutter harness, `result.json` and the game log per arm, the prep logs). Each arm
starts with the build's macOS Metal cache removed (its own bundle id, `org.fable2recomp.metal.agent-5`) and an empty
game cache; arm B first restores the Metal cache as the 2× + 1× prep left it and uses the prep's game cache. 2×,
60 FPS, first 60 s after the game reports ready. Old Tomb = library save `hero1-dog-live_2026-10-07_18-49-47`;
prologue = new game from the title (opening movie skipped). One other agent game ran during every arm (contention:
frame pacing is indicative; the hitch causes are not affected).

| Area | Arm | Hitches (first 60 s) | of which xlate / pso_emu | Max frame | 1% low | Session total (incl. load) | Ready after |
|---|---|---|---|---|---|---|---|
| Old Tomb | A, no prep (today) | 8, **2,100 ms** | 1,229 / 476 ms | 472 ms | 13 fps | 23, 6,984 ms (xlate 3,483) | 9.2 s |
| Old Tomb | B, bank prep | 6, **526 ms** | 5 / 488 ms | 214 ms | 31 fps | 19, 2,699 ms (xlate 11) | 5.7 s |
| Prologue | A | 7, **3,366 ms** | 1,754 / 978 ms | 316 ms (1,966 ms in an earlier A run) | 18 fps | 21, 7,916 ms (xlate 3,867) | 38.1 s |
| Prologue | B | 4, **913 ms** | 8 / 863 ms | 134 ms | 37 fps | 15, 3,320 ms (xlate 122) | 36.3 s |

Repeat runs agree (A-tomb 2,183 ms, B5-tomb 589 ms; A-prologue 3,279 ms, B5-prologue 1,020 ms). Frames with draws
skipped while a pipeline compiles (`FABLE2_DRAW_SKIP_TRACE=1`, `ds_async_compiling`, whole session): Old Tomb 491 → 304,
prologue 448 → 275.

- **Removed:** every draw-path pixel-shader translation in these runs (Old Tomb 89/89 pixel keys predicted). The
  one prologue miss is a point sprite with param gen (`param_gen_point`), a variant left out on purpose (it would
  add ~450 compiles for one shader seen so far).
- **What still compiles at first use:** pipeline builds (`pso_emu`, 20–90 ms each: native rectangle, point and
  terrain pipelines for a given render-target format and blend state; up to 11 in one frame when an area first
  draws), the patched vertex shaders (background threads for normal draws; the draw path waits only in the native
  expansions), and the draws skipped while a normal pipeline compiles in the background (a long-standing behaviour,
  `ds_async_compiling`).
- A second launch already prebuilds the native pipelines it saw (`FABLE2_NATIVE_GEOM_PERSIST`) and the normal ones
  from the pipeline list, so these are first-play-only.

## 5. Can the remaining pipeline builds start earlier or off the GPU thread?

Looked at, nothing implemented:

- A pipeline needs the vertex function, the pixel function, the render-target formats, the sample count and the
  blend state. On the GPU Commands thread these are known only at the draw, and the draw is encoded right then;
  building it on another thread and waiting is the same stall, and not waiting skips the draw.
- Several new pipelines in one frame come from different draws, so they cannot run in parallel without knowing the
  later draws early. Doing that exactly would need a lookahead over the command ring with a shadow copy of the
  register and render-target state, which is close to a second command processor. Not attempted.
- Prebuilding at install would need the format / blend combinations, which come only from play (the remembered
  pipeline list; shipping it was declined) or from guessing (mostly misses: on Apple GPUs formats and blending are
  compiled into the pipeline).
- Within one native draw the vertex expansion and the pixel shader are compiled one after the other; after the bank
  prep the pixel shader is a Metal cache hit, so running them side by side would save ~nothing.

So the exact option is the one already in place: the first play records the pipelines and every later launch
prebuilds them in the background.

## 6. Switches

| Variable / setting | Default | Effect |
|---|---|---|
| `FABLE2_SHADER_PREP_BANK`, Settings "Prepare all of the disc's shaders at install" | on | `=0` off; `=<file.sbk>` another bank |
| `FABLE2_SHADER_PREP_BANK_THREADS` | cores − 1, ≤ 11 | compile threads |
| `FABLE2_SHADER_PREP_BANK_VS` | fetchless | `0` none, `all` every vertex shader |
| `FABLE2_SHADER_PREP_BANK_BUDGET_S` | 120 | no new work after this; 0 = none |
| `FABLE2_XLATE_TRACE` | on | `[xlate-trace]` lines |
| `tools/fable2_cache_warm.py --no-bank` | — | skip the bank prep in the setup app's shader step |
