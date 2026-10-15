# Higher internal resolution (2× / 3×) — 2026-10-09

**Status (step 2 done, §8):** 2× and 3× render correctly, including the dog's fur (morph readback). Scaled
texture loads no longer break the main pass. It is a setting in `fable2_config.toml` and in the Dev Launcher,
default Native. MetalFX Spatial is an optional, look-changing upscaling mode (§22). "Auto" picks 1×/2×/3× from the
GPU at startup (§24). Fur fixes A/B are on by default in the game.
§1–§7 are the step 1 investigation.

## Step 1: investigation

Branch `feature/resolution-scale` (from `feature/sdk-from-source` 533f237). The scope was investigation and
measurement. The only code changes are an experimental scale switch, one crash fix needed to get 2× running at
all, trace lines, and an f2diag verdict fix. The default (native 1×) behaviour is unchanged.

Test machine: M5 Max. Saves: `hero1-dog-live_2026-10-07_14-45-08` (Old Tomb: sweep, close views, UI) and
`hero1-dog-live_2026-10-07_18-49-47` (Thag's Cave → Cullis Gate → Bower Lake area run). All runs used background
test mode (`f2diag run …`, plus a scratch UI script built on `tools/f2diag/run.py`).
Comparison images (ignored folder): `out/validation/resolution-scale-20261009/comparisons/`.

## 1. Mechanics

**The SDK cvars do nothing on Metal.** `draw_resolution_scale_x/y` and `resolution_scale` are defined in the SDK
(`src/graphics/pipeline/texture/cache.cpp`): `int`, range 1–8, restart-only. `TextureCache::GetConfigDrawResolutionScale`
then clamps them to `kMaxDrawResolutionScaleAlongAxis = 7`. The D3D12 and Vulkan command processors call it.
The Metal command processor built `MetalTextureCache` and `MetalRenderTargetCache` with a literal `1, 1`, so
`--draw_resolution_scale_x=2` was silently ignored.

**Experimental switch (this branch):** `FABLE2_RES_SCALE=N` (N = 1..3, both axes) or `FABLE2_RES_SCALE=cvar`
(honours the SDK cvars). It is clamped to 3 per axis (`metal_command_processor.cc`, `SetupContext`), and the log
gets one `[res-scale] FABLE2_RES_SCALE=… -> draw resolution scale NxN` line. Unset means native, exactly as before.
Since step 2 it is a setting: `[graphics] resolution_scale` in `fable2_config.toml` (the environment
variable wins) and the Dev Launcher's "Internal resolution" picker (§8).

**What already exists in the Metal backend.** These are XeniOS ports and they work once the scale is passed:

- the scaled EDRAM and render targets (main pass becomes 2240×2352×2 at 2× and 3360×3528×2 at 3×);
- scaled resolve buffers (`MetalTextureCache::EnsureScaledResolveBufferRange`, private `MTLBuffer`s grown on demand, ≤ `maxBufferLength`);
- scaled resolve and texture-load pipelines;
- `_scaled` direct-host-resolve variants;
- the scaled viewport, scissor and point size;
- shader translation with the scale baked in (`DxbcShaderTranslator init: … scale=2x2`);
- `RequestSwapTexture` returning the scaled swap texture.

The practical range is integer 1–3. 4–7 would technically pass the SDK clamp, but at 3× the memory footprint is
already 8.8 GB (§4).

**Presenter output.** The guest output (swap) texture becomes 2560×1440 at 2× and 3840×2160 at 3×, and `capture`
returns those sizes.

- The presenter draws it into the `CAMetalLayer` drawable with one bilinear pass (`guest_output_bilinear_ps`;
  dither variant for 10 bpc), aspect-fitted to the window.
- The drawable is the window's size in **pixels**: the SDK surface reports `SDL_GetWindowSizeInPixels`, so on a
  Retina display it is already the full backing size. (Corrected in §9; this line used to say "logical window
  size". `metal_presenter_use_backing_scale` would multiply the pixel size by the scale again, so it stays off.) The
  default window is 1280×720 points, so the drawable was 2560×1440 in these tests (`capture --screen`).
- So 1× is upscaled 2:1, 2× is shown 1:1, and 3× is downscaled 1.5:1 with one bilinear tap.
- In a smaller window, 2× becomes a 2:1 bilinear downsample. That is effectively a 2×2 box, so it acts as clean SSAA.
  3× has no matching filter and aliases.

**Shader cache.** The cache is not keyed by scale, and does not need to be. The translated DXBC differs per scale,
so stage artifacts get new hashes and the stored guest shader and pipeline descriptions are re-translated.

- A first boot at a new scale paid 65 `persistent miss` compiles: boot to gameplay took 7.6 s (2×) and 7.8 s (3×),
  against 4.9 s at 1×.
- With a warm 2× cache (`--cache-from` a 2× session's cache), 2× booted in 4.8 s, the same as 1×.
- Normal sessions write back, so one warm-up per scale is all a player sees.
- The very first 2× launch on this machine also spent about 3 s in renderer init, probably compiling the scaled
  helper pipelines in the Metal driver. Later 2× and 3× launches did not.

## 2. Runs and breakage

| Run | Scale | Session (`out/dev-sessions/…`) | Note |
|---|---|---|---|
| res1-sweep | 1× | 20261009-152709-54722dae | first launch after the rebuild (kept): title at 2 s, normal |
| res2-sweep | 2× | 20261009-152807-2fe6faf9 | **crashed** at about 21 s (row 2 below) |
| res2-boot | 2× | 20261009-153003-e3d4617a | **crashed** at about 5 s, same stack |
| res2-sweep-b | 2× | 20261009-153145-72050948 | first launch after the encoder fix (kept): ok |
| res3-sweep | 3× | 20261009-153357-f3837fd1 | ok |
| res{1,2,3}-ui | 1/2/3× | 20261009-153719-0931fbbf, -153801-8c0f7364, -153828-271ad8ba | HUD, turbo notice, pause, close-up, memory |
| res{1,2,3}-area | 1/2/3× | 20261009-153936-66093d23, -154047-687ef488, -154158-7f4cb331 | loading screens, area load |
| res{1,2,3}-unc | 1/2/3× | 20261009-154402-dc31b562, -154446-3fd48810, -154529-dae9148e | uncapped turn30, GPU trace |
| res2-area-warm | 2× | 20261009-154750-e9ef67ad | warm 2× cache boot |
| res1/2-mexB | 1/2× | 20261009-155115-5a6c2197, 20261009-155615-0b3fa362 | memexport preserve + readback with draw-skip trace |

**Breakage table.** Image paths are relative to `out/validation/resolution-scale-20261009/comparisons/`. Raw
captures are under each session's `test-state/control/`.

| # | Symptom | Scales | Evidence (1× / 2× / 3×) | Cause | Status |
|---|---|---|---|---|---|
| 1 | **Dog renders solid black** (no fur colour or pattern); outline and animation fine | 2×, 3× | `dog-res1-sweep.png` / `dog-res2-sweep-b.png` / `dog-res3-sweep.png`; contact sheets `sheet-res{1,2,3}*.png`; raw: `20261009-152709-54722dae/…/0d2cdb58-….ppm`, `20261009-153145-72050948/…/f1a8005e-….ppm`, `20261009-153357-f3837fd1/…/86714082-….ppm` | Morph readback skipped at scale > 1. Log: `[res-scale] morph readback skipped (scaled)` ×5 per boot (0x12704000, k_8_8_8_8, 1048576 / 491520 / 212992 / 73728 / 4096 bytes); at 1× `Metal morph readback: copied` ×5 | **Fixed in step 2** (§8, item 1) |
| 2 | **SIGABRT on GPU Commands** (Metal: `-[AGXG17XFamilyCommandBuffer blitCommandEncoderCommon:]` → `MTLReportFailure`, from `MetalTextureCache::EnsureScaledResolveBufferRange` / `pass_trace::Blit`, called from `TextureCache::LoadTexturesData` during `SubmitPreparedDraw`) | 2× (any scale > 1) | crash reports `fable_2-2026-10-09-152829.ips` (session 20261009-152807-2fe6faf9), `fable_2-2026-10-09-153009.ips` (20261009-153003-e3d4617a) | When the scaled-resolve buffer grows, the old ranges are blitted on the *current* command buffer while a deferred texture-upload compute encoder (or the render pass) is still open | **Fixed on this branch** (scale > 1 path only): end the pending upload encoders and the render encoder before that blit (`metal_texture_cache.cc`). No crash since, in 13 scaled runs |
| 3 | No flight-recorder dump for those aborts; the sweep verdict still said `ok` | n/a | as above | Abort path not hooked (phase-2 synchronous crash dump) | f2diag verdict fixed (§7). Recorder dump still open |
| 4 | Bloom halo and the green interact outline narrower; torch core brighter and clipped at 3× | 2×, 3× | `crop-torch.png`, `crop-hero.png`, `ui-hud-plain.png` (columns or rows 1×/2×/3×) | The SDK translator divides texture-fetch texel offsets by the scale (`draw_resolution_scaled_texture_offsets`), so blur kernels built from fetch offsets halve (2×) or third (3×) in screen terms | **Fixed 2026-10-10** (§12): `[graphics] effects_native_scale` (default on) |
| 5 | 3× shimmer and aliasing on downscale | 3× | `ui-*.png` row 3 (subtle in stills) | 3840→2560 with one bilinear tap | Presenter downsample filter, §5 |
| 6 | First boot at a new scale about 2.7 s slower | 2×, 3× | `area-res{1,2,3}-area.png` (2× still on the loading map at 6 s, 3× white); `f2diag phases` | Cold stage compiles (65 persistent misses) | Self-heals with cache write-back; warm 2× = 4.8 s |
| 7 | Main-pass breaks 0.38 → 3.9 per frame (perf, not visual) | 2×, 3× | `f2diag gpu` on the `-unc` sessions: `texture:other` 3.79/frame; `tr_n_tex_ld_scaled` 6.06/frame | `ClassifyMaterializationPlan` and the pre-commit route exclude scaled-resolve texture loads (`!scaled`) | **Fixed in step 2** (§8, item 3): 0.54 / 0.44 per frame |

**Checked and unchanged at 2× and 3×** (side by side in the listed images):

- HUD, health icon and the interaction prompt (`ui-hud.png`).
- Subtitle and tutorial text: sharper at 2× and 3× (`crop-text.png`).
- Pause menu, including its render-to-texture scene preview (`ui-pause.png`, `crop-pause.png`).
- Loading map screen and the Cullis Gate transition (`area-res*.png`).
- The ImGui turbo notice. It is drawn at drawable size, so it is identical at every scale (`ui-turbo-on.png`).
- Close views (`ui-closeup.png`).
- Floor and character shadows (`crop-floor.png`).
- Bower Lake terrain and night lighting (`area-res*.png`). Water was not in view.
- The hero: face, skin and clothing look the same as at 1× in this save (`dog-res*.png`, `crop-hero.png`).
  The hero's morph is not visibly affected here, but a regenerated morph (appearance change) was not tested.
- Dog fur: with the black base texture it cannot be judged visually. The memexport side is identical (next item).
- Memexport readback (fix B): 24 `[memexport-readback] applied` lines at both 1× and 2× (the first 24 are logged),
  same addresses (0x1B943440, 0x1B947C40, …); all reported `changed=0`.
- Direct host resolve: 26.7 per frame at 2×, uses the `_scaled` pipelines, no errors.
- No guest AVs, `[FATAL]` lines, hangs or dumps beyond `exit`, apart from the two crashes above. The warning
  digest diff between 1× and 3× is only the `[res-scale]` line and a one-off 1× texture-residency message.
- The automatic dark/blue detectors flag the same rot+2/tiltU2 dark-wall frames at every scale (baseline).

## 3. Morph and other GPU→guest readbacks

**Where morph readback assumes native resolution.** `MetalCommandProcessor::IssueCopy`
(`metal_command_processor.cc`, after `render_target_cache_->Resolve`). It assumes this in two places:

1. The guard `!texture_cache_->IsDrawResolutionScaled()` skips it outright.
2. It copies `shared_memory_->GetBuffer()->contents() + written_address`. At scale > 1, `Resolve` writes **only**
   the scaled resolve buffer (private storage, `dest=scaled_resolve_memory`); it neither writes nor marks the
   shared-memory range. So even without the guard there are no native pixels to copy.

The game reads the morph result from guest RAM on the CPU. The GPU-side scaled texture is fine but unused for
that, which is why the dog goes black (guest RAM keeps its initial contents).

**Fix needed: downscale the scaled resolve to native before the CPU copy.** This is Metal-backend only; no SDK
patch is needed.

- Source: after `Resolve`, the scaled range is current. Get it with `MetalTextureCache::GetCurrentScaledResolveBuffer(buffer, offset, length)`.
- Pass: dispatch the XeniOS `resolve_downscale` compute shader. It is already compiled into the build as
  `rex/graphics/shaders/bytecode/metal/resolve_downscale_cs.h`, but no Metal code uses it yet.
  - Bindings: push constants at `buffer(0)` (`scale_x`, `scale_y`, `pixel_size_log2=2`, `tile_count = bytes / 4096`,
    `half_pixel_offset`), source at `buffer(1)`, destination at `buffer(2)`.
  - One 32×32 threadgroup per tile.
  - The XeniOS version maps the scaled 32×32-tiled layout correctly (`XeDownscaleScaledBlockByte`); the SDK's
    HLSL copy assumes a linear block layout.
  - It point-samples one host pixel per guest pixel: top-left, or centre with `half_pixel_offset`. Use the centre.
    The morph draws are texture-space quads at 1:1, so a point sample is close to the 1× result. A 2×2/3×3 box
    average would need a small shader variant (+30 lines) and is optional.
- Destination: a small shared-storage `MTLBuffer`, grown to the largest morph (1 MB).
- Then use the existing completion wait (`FlushCommandBufferAndWait` + status poll) and `memcpy` into guest RAM at `written_address`.
- Reference implementation: SDK D3D12 `IssueCopy_ReadbackResolvePath` (scaled branch) and XeniOS
  `d3d12_command_processor.cc`.
- **Size:** about 120–160 lines in `metal_command_processor.cc/.h` (pipeline creation, buffer, dispatch, reuse of
  the existing wait and copy), plus a trace counter.
- **Risk: low.** It runs only on the five morph resolves per load (plus any appearance change), on a path that
  already stalls for completion. Failure modes are visible (dog colour) and gated by the existing
  `metal_fable_morph_readback` cvar / `[patches] hero_dog_texture_readback`.
- A "read the 1× equivalent" alternative does not exist. At scale > 1 the EDRAM holds only scaled samples, so a
  native resolve has no source.

**Other GPU→guest paths:**

| Path | Scaled-resolution status |
|---|---|
| Memexport readback B (`metal_shared_memory.cc`, `FABLE2_MEMEXPORT_READBACK=1`) and preserve A | Unaffected. Memexport writes go to shared memory at every scale; verified the same applies at 1× and 2× (sessions 20261009-155115-5a6c2197 and 20261009-155615-0b3fa362) |
| Resolves to guest memory (generic) | At scale > 1, output lives only in the scaled buffer. Guest RAM and shared memory never see it, as on D3D12/Vulkan with `readback_resolve=none`. Only CPU consumers break; morph is the only known one. A generic `readback_resolve` port would reuse the same downscale pass. Not needed for Fable unless another CPU reader turns up |
| Direct host resolve (`metal_direct_host_resolve.cc`) | Has `_scaled` pipeline variants, sizes the tile by `draw_resolution_scale_x`, and writes `scaled_resolve_memory`. Exercised (26.7/frame), no errors |
| CR-10 (resolve output vs stale re-upload) | Changes shape at scale > 1. Resolves no longer touch shared memory, so the stale-upload race on the shared buffer is gone for them. Instead, the SDK's scaled-resolve watch drops a texture's "scaled" status when the CPU writes into its range, and the texture then reloads from shared memory, which never received the resolve (black or stale). Same trigger and rarity as CR-10. Not seen in these runs |
| Swap / screenshots | `RequestSwapTexture` returns the scaled texture; captures and the View screenshot are full internal resolution |

## 4. Performance (M5 Max measured, M3 Pro estimated)

**Uncapped Old Tomb turn30** (`--fps uncapped --sweep-plan turn30`, `FABLE2_GPU_TIME_TRACE=1`, 30 s window):

| Scale | FPS | 1% low | frame p95 | GPU mean | GPU p50 | GPU p95 | main pass ms/frame (passes/frame) | footprint (graphics) |
|---|---|---|---|---|---|---|---|---|
| 1× | 173.7 | 89.7 | 10.1 ms | 5.79 ms | 4.67 | 9.99 | 1.93 (2.36) | 3.3 GB (1.7) |
| 2× | 95.2 | 50.9 | 16.7 ms | 10.41 ms | 8.65 | 16.43 | 4.45 (5.88) | 5.8 GB (3.7) |
| 3× | 52.6 | 33.5 | 28.1 ms | 18.90 ms | 16.43 | 27.59 | 8.42 (5.82) | 8.8 GB (6.6) |

- GPU cost is 1.8× at 2× and 3.3× at 3×, for 4× and 9× the pixels.
- Process RSS is nearly flat: 1.2 / 1.3 / 1.4 GB.
- The growth is GPU memory (`footprint`: "Owned physical footprint (unmapped) (graphics)" plus IOAccelerator, after
  about 30 s of gameplay). It comes from the scaled EDRAM and render targets and the scaled resolve buffers,
  which are sized for the whole range.

**Per-pass GPU cost by scale.** Source: `f2diag gpu <session> --json` on the three `-unc` sessions. Values are
encoder span in ms per frame (time in flight, overlapping, so the column sums of 9.2 / 13.7 / 26.2 exceed the GPU
mean of 5.8 / 10.4 / 18.9). Keys are normalised to 1× size, ranked by growth from 1× to 2×.

Attachment actions are as the pass trace records them (`load`/`store`: L = load, S = store, D = don't care).

| Rank | Pass (1× size) | 1× | 2× | 3× | +2× | Attachments | Memoryless / DontCare possible? |
|---|---|---|---|---|---|---|---|
| 1 | **Main scene pass**: RGBA16F 2x MSAA + D32S8, `c0:115/1120x1176x2`, about 120 draws per pass | 1.93 (2.4 passes) | 4.45 (5.9 passes) | 8.42 (5.8) | **+2.52** | colour LS, depth LS, stencil LS | No. The MSAA samples *are* the EDRAM. They are read after the pass by resolves, ownership transfers and the next pass after every break. Only the *extra* breaks can go (row 7): about 3.5 load+store round trips of a 2240×2352×2 RGBA16F + D32S8 target per frame |
| 2 | **Other render passes**: shadow, blur and bloom chains (`c0:90/1280x2048x1`, `c0:90/320x8192x1`, `c0:70/640x4096x1`, `c0:115/320x8192x1`, …) | 1.72 | 2.68 | 4.33 | +0.96 | mostly colour LS; some DD colour with LS depth | Some colour targets are already `DD` (written fully). The LS ones feed resolves, so they must be stored |
| 3 | **Texture uploads/loads** (`tex_upload_compute` + `tex_upload_blit`, about 31 each per frame) | 0.46 | 1.10 | 2.29 | +0.64 | n/a | About 6 per frame are scaled-resolve loads (`tr_n_tex_ld_scaled`). They untile a 4× (9×) larger resolve into a texture: the scaled-resolve copy cost |
| 4 | **4x MSAA exporter/terrain passes** (`280x2344x4`, `560x1176x4`, `320x2048x4`) | 1.06 | 1.45 | 2.59 | +0.39 | the big one colour LS; others colour DD, depth LS | Colour `DD` already where possible. Depth is kept (EDRAM) |
| 5 | **Resolves** (`direct_host_resolve` 26.7 per frame, `rt_resolve` 4.8 per frame) | 0.28 | 0.59 | 1.17 | +0.31 | compute, writes the scaled resolve buffer | n/a |
| 6 | **Small encoders** (`rt_depth_store`, `shmem_upload`, `shmem_precommit`, `tex_gpu_load`) | 0.16 | 0.27 | 0.58 | +0.11 | n/a | n/a |
| 7 | **EDRAM ownership transfers** (`rt_transfer`, 6–8 per frame; main pass 2 per frame) | 3.57 | 3.13 | 6.79 | (−0.44) | LS | No (they copy EDRAM ownership between host RTs). Their 1× spans include queueing behind earlier work: 1.56 ms each at 1× against 0.99 at 2×. Read the growth from 1× to 3× (+3.2), not 1× to 2× |

- **Main-pass breaks.** 0.38 → 3.89 → 3.87 per frame, nearly all `texture:other` (row 7). They account for most of
  the main pass going from 2.4 to 5.9 encoders per frame.
- Without the breaks, the main pass would scale roughly with its fragment work: 1.58 → 4.04 ms fragment, which is
  2.6× for 4× the pixels.

**Fixing row 7.** Using the 2026-10-08 regression (118 µs per extra main pass at 1×, scaled by pixel count), the
extra 3.5 breaks cost an estimated 0.5–1.8 ms per frame at 2× (5–17%) and more at 3×. The low end is the more
likely, because fragment time grows less than 4×.

**Capped at 60** (default sweep, captures on):

| Scale | Pacing |
|---|---|
| 1× | 60.0 FPS, p95 17.9 ms, 1% low 54.7 |
| 2× | 60.0 FPS, p95 18.0 ms, 1% low 53.7, 0 frames > 33 ms |
| 3× | 50.7 FPS, p95 28.2 ms, 1% low 30.9, 10 frames > 33 ms |

Capped GPU ms (clocks drop at the cap) is 12.1 / 13.9 / 19.5.

**M3 Pro estimate.** This uses the `docs/weak-mac-perf-20261008.md` assumptions (GPU factor 2.2–3.3× for the
18-core, 2.8–4.3× for the 14-core; memory bandwidth about 4× lower). Resolution scaling is bandwidth-heavy, so
expect the high end of each range.

| | 1× GPU mean | 2× GPU mean | 3× GPU mean | Verdict |
|---|---|---|---|---|
| 18-core | 12.7–19.1 ms (52–79 FPS) | 22.9–34.4 ms (29–44 FPS) | 41.6–62.4 ms (16–24 FPS) | 2× only at a 30 FPS cap, and heavy views (p95 about 36–54 ms) dip below 30. 3× not viable |
| 14-core | 16.2–24.9 ms | 29.1–44.8 ms (22–34 FPS) | 52.9–81.3 ms | Native only |

Memory: an 18 GB M3 Pro can hold 2× (about 5.8 GB footprint). 3× (8.8 GB and growing) risks memory pressure.

## 5. Upscaler / downscaler options (size only)

- **Today:** one bilinear (or bilinear + dither) pass in `metal_presenter.mm`.
  - Requested `present_effect` values other than bilinear log `guest output effect N not supported; falling back to bilinear`.
  - A **MetalFX spatial scaler path already exists**: `--metal_presenter_use_metalfx=true`, plus `_color_processing`
    and `_scale_x/y`, macOS 13+. It applies only when the output is larger than the input (the 1× → large-window case).
    It costs zero code to try. It needs a launcher toggle and an A/B look.
- **FSR 1 (EASU + RCAS) and CAS:** the XeniOS shaders exist (`ui/shaders/guest_output_ffx_fsr_easu`, `_rcas`,
  `_cas_resample`, `_cas_sharpen`, with dither variants). The SDK presenter already parses the effects and builds the
  multi-effect `GuestOutputPaintFlow` (the D3D12/Vulkan presenters implement it).
  - `tools/compile_metal_helpers.py` skips every `ffx_*` file, and the Metal presenter only implements the last
    bilinear effect.
  - Work needed:
    1. Compile the `ffx_` shaders (drop the filter; they may need small MSL fixes, which is likely why they were skipped).
    2. Add pipelines and constants (EASU/RCAS/CAS constant setup is in the SDK presenter).
    3. Add one or two intermediate textures and chain the effects.
  - **About 250–400 lines, medium.** CAS sharpen alone is the smallest useful piece (about 120–180 lines). It also
    helps 2× → small-window downsampling.
- **Downsampling at 3×** (internal larger than the drawable): there is no proper filter today. A box or area
  downsample pass (or CAS resample, which handles downscale) is about 80–120 lines. It is only needed if 3× is offered.

## 6. Recommendation for step 2

**Build (needed for 2× to be correct), in order:**

1. **Morph downscale readback** (§3; Metal-only, about 150 lines, low risk). Default on whenever scale > 1, with
   `metal_fable_morph_readback` as the existing switch. Validate: the dog colour matches 1× (`dog-*` crops), and
   `Metal morph readback: copied` ×5 at 2× and 3×.
2. **Keep the encoder fix** from this branch (row 2), with a short comment and a `bug-patterns` row: "encoder opened
   on a command buffer that still has another open encoder".
3. **Scaled-resolve pre-commit eligibility** (row 7; perf, medium). Classify a scaled-resolve load as eligible when
   its scaled range has no pending write in the open command buffer, mirroring the shared-memory check. Measure the
   gain uncapped at 2× (expected 0.5–1.8 ms).
4. **Launcher control.** "Internal resolution: Native / 2× / 3×", passing `FABLE2_RES_SCALE`. Rename it to a real
   option or cvar when it stops being experimental. Default **Native**.
   - 3× labelled "(Max GPUs, high memory)".
   - Defer "Auto by GPU" until 2× has been played. When added, a rule like "≥ 30 GPU cores and ≥ 32 GB → 2×,
     otherwise native" fits the numbers above. The existing fps-auto (`src/core/fable2_fps_auto.h`) already reads
     the GPU core count.
5. **Presenter:** a launcher toggle for MetalFX spatial (exists). CAS sharpen next if wanted. A proper downsample
   filter only if 3× ships.
6. **Pending tooling fix:** the phase-2 synchronous flight-recorder dump on abort (row 3).

### High-resolution FPS levers (proposals only, ranked)

The goal is more FPS *at* 2×. The base is 10.4 ms GPU mean (95 FPS uncapped on M5 Max). Savings are M5 Max GPU
mean at 2×. On an M3 Pro, multiply by 2.2–4 (bandwidth-bound items nearer 4).

| Rank | Lever | Estimated saving at 2× | Size / risk | Notes |
|---|---|---|---|---|
| 1 | **MetalFX spatial upscaling** (`metal_presenter_use_metalfx`, already in the presenter, off) | Render 1× and upscale to the 2560×1440 drawable: GPU ≈ 1× cost + about 0.3–0.5 ms, so **−4 to −4.5 ms** against true 2× (~40%) | **S**: launcher toggle plus an A/B look; code exists | Not true 2× detail (UI and text stay 1×, edges reconstructed), but by far the cheapest "sharp at high resolution" option. It needs output larger than input, so it does nothing with 2× internal in a 2560 window. Best use: an "Upscaled" preset on GPUs that cannot afford 2× (M3 Pro) |
| 2 | **Scaled resolve textures in the texture pre-commit route** (removes about 3.5 of the 3.9 main-pass breaks) | **−0.5 to −1.5 ms** (5–15%). Each break is a load+store of 2240×2352×2 RGBA16F + D32S8 (about 85 MB each way) | **M** (about 100–200 lines in `ClassifyMaterializationPlan` and `LoadTextureData` scaled branches); medium risk (ordering against same-command-buffer resolves) | Eligible only when the scaled range has no pending write in the open command buffer, mirroring the shared-memory test. Already step-2 item 3. Prototype and measure uncapped |
| 3 | **Avoid scaled-resolve buffer copies** (sample the resolved data without untiling it into a separate texture each time; reuse loads when the range did not change) | **−0.3 to −0.7 ms** (most of the +0.64 texture-load growth plus part of the resolve growth) | **L**; medium–high risk | First cheap step: count how many of the ~6 scaled loads per frame re-load an unchanged range (trace counter, S). Skipping those may be most of the win |
| 4 | **Load/store actions** (`DontCare` load on a first pass that fully overwrites, `DontCare` store for stencil or depth never read later) | **−0.1 to −0.4 ms** | **S–M** per case; medium risk (needs proof the attachment is dead or fully overwritten) | Colour already uses `DD` where the trace allows. Candidates: the main pass's stencil `LS`, and depth on post passes. Each change needs a per-pass audit with the draw dump |
| 5 | **Keep blur, bloom and shadow targets at 1×** (per-render-target scale) | **−0.6 to −1.0 ms** (most of the +0.96 "other render passes" growth plus their transfers); also restores the 1× bloom width (row 4) and saves memory | **XL**: an SDK-level change (scale is global across EDRAM addressing, resolve shaders, texture-cache scaled ranges and translated shaders); high risk | Shadow maps at 1× would also lose the sharper shadows. Not before 1–3 |
| 6 | **Memoryless MSAA / in-tile resolve** | **About 0–0.3 ms** | **L**; high risk | The EDRAM model needs the samples in memory (resolves, transfers, pass restarts). Memoryless would only fit a pass that is never broken and whose only consumer is a plain MSAA resolve. Fable's main pass is neither. Not recommended |
| 7 | **Non-integer scales (1.5×)** | 1.5× would cost about 1× + 56% of the 2× increase: GPU about **8 ms (−2.4 ms against 2×)** | **XL**: the SDK scale is an integer multiplier in EDRAM tile addressing, the resolve and load shaders and the translator; high risk | Practical substitutes: 1× + MetalFX (lever 1), or an asymmetric 2×1 (`x=2, y=1`), which the code already supports. It halves the cost of 2× but softens vertical detail. Not recommended as a product option |

Order for step 2: lever 1 (toggle only), then lever 2 (prototype and measure), then the counter for lever 3. Levers
4–7 only if 2× on Max-class GPUs still needs headroom.

**Status (2026-10-10, §11–§12):** lever 1 shipped (toggle), lever 2 shipped (step 2 item 3). Lever 3 measured: no
redundant copies, not adopted. Lever 4 measured: upper bound about 0.1 ms, not adopted. EDRAM transfers: not
avoidable in a contained way. Lever 5: the original effect size is restored by a translator setting (on by
default, §12). Native 1× effect render targets were built and measured (0.4 ms saved at 2×, 0.9 ms at 3×), but
they narrowed the glow again, so they were removed. The patch is kept in `../restore-points/` (§12).

**Not needed for correctness:** SDK patches (only levers 5 and 7 would need them). Shader-cache keying is fine (DXBC differs per scale). Memexport, HUD, text, overlay and
loading screens are fine.

**Default:** Native until the maintainer has played 2× with the morph fix. 2× is the sensible "Enhanced" setting on
Max-class GPUs (holds 60 on M5 Max, 1% low 54). 3× is only for Max GPUs at 30–50 FPS.

## 7. Tooling change made during this step

`f2diag run` verdicts (`tools/f2diag/run.py`, `exit_check`) now fail when the game exits unexpectedly. Reasons:

- `exited_early`: the pid is gone before the harness close; checked at each capture and before close.
- `crashed`: a `~/Library/Logs/DiagnosticReports/fable_2-*.ips` report for the pid; path, faulting thread and frames go in `crash_reports`.
- No clean-exit marker after the harness asked to close.

Captures after a death report `process exited`. To re-check a past session:
`python3 tools/f2diag.py run verdict <session> [--no-close-asked]`.

Validation:

- Both crash sessions above now return `fail` with their `.ips` path and the `blitCommandEncoderCommon` stack.
- Four normal sessions from 1× to 3× reclassify as `ok`.
- Three fresh uncapped runs passed with empty `exit_reasons`.

## 8. Step 2: implementation (2026-10-09)

Harness, logs and images: `out/validation/resolution-step2-20261009/` (`buildN.log`, `cmp/`, `harness/`). All runs
used background test mode on the M5 Max. The first launch after each of the 4 builds is counted. All 19 sessions had
verdict `ok`, 0 AV and 0 `[FATAL]` lines, and `pgrep -x fable_2` returned 1 after each.

Restore points (`../restore-points/`), each saved before its item:

- `2026-10-09-resolution-step2-morph-before.patch` (empty, clean tree)
- `…-encoderfix-before.patch`
- `…-precommit-before.patch`
- `…-launcher-before.patch`
- `…-metalfx-before.patch`
- `…-furdefaults-before.patch`

### Item 1: morph readback at scale > 1

**Change** (`metal_command_processor.cc/.h`, `MorphReadbackScaled`). After the morph resolve (0x12704000,
k_8_8_8_8):

1. Make the written range current in the scaled resolve buffer. The end is aligned to a whole tile (`…, 12`): the
   SDK's scaled range ends at the first scaled byte of the last 1× byte, and the shader reads full tiles.
2. Dispatch the XeniOS `resolve_downscale` compute (`resolve_downscale_cs.h`, already compiled) on the current
   command buffer. Settings: one 128-thread group per 32×32 tile, centre sample (`half_pixel_offset = 1`). It writes
   a 1 MB shared buffer that grows if needed. The other encoders are ended first (row 32).
3. Reuse the 1× completion wait (now `WaitForMorphReadbackSubmission`) and copy to guest RAM.

- The `!IsDrawResolutionScaled()` skip and its trace line are gone.
- The log line now carries an xxh3 of the copied bytes at every scale.
- `FABLE2_MORPH_READBACK_DUMP=<dir>` (validation only) writes the bytes to files.

**Results.**

- `Metal morph readback: copied … (downscaled from 2x2|3x3)` ×5 per boot.
- Bytes against a 1× run of the same load (`morph-{1,2,3}x/`):

| Readback (bytes) | Identical pixels (2× / 3×) | Mean abs. difference per byte (2× / 3×) | Max |
|---|---|---|---|
| 1048576 | **100% / 100%** (byte-identical) | 0 / 0 | 0 |
| 491520 | 59.0% / 59.0% | 1.12 / 1.12 | 76 |
| 212992 | 55.4% / 53.7% | 1.52 / 1.48 | 74 |
| 73728 | 53.2% / 51.6% | 1.84 / 1.78 | 75 |
| 4096 | 9.2% / 3.2% | 5.6 / 5.6 | 65 |

- The smaller levels are rendered by the game from the scaled source, so a centre point sample cannot match 1×
  exactly. A box filter is still possible, but nothing visible calls for it.
- `cmp/sheet-dog-rot.png` (rows 1×/2×/3×, four sweep moments): the dog has its black-brown coat, tan markings and
  white paws at every scale. The hero is unchanged.

### Item 2: encoder fix kept

The comment in `metal_texture_cache.cc` now states the rule. `docs/bug-patterns.md` row 32 describes a second
encoder opened while another is open: Metal aborts, and only the scaled path triggers it.

### Item 3: scaled-resolve textures in the texture pre-commit route

**Change** (`metal_texture_cache.cc/.h`, `metal_direct_host_resolve.cc`, `metal_shared_memory.h` counters). A
scaled-resolve load is eligible when the step 8 rules hold, which means the texture is neither sampled in the open
command buffer nor presented. It also needs all of these:

- **No write in the open command buffer to its scaled source** (RAW: the pre-commit command buffer runs first).
  - `MarkScaledResolveWrite` keeps an exact union of scaled ranges written in the open submission, never collapsed
    (row 26).
  - Writers: both resolve paths, through `PrepareResolveDestinationBuffer`, and the buffer-grow copy.
  - Committed writers finish before the pre-commit command buffer starts (the GPU-only `main_cb_event_` wait from
    step 7). Standalone writers are waited on the CPU.
- **The scaled range is resident**, so the load cannot grow the buffer. As a safeguard, a grow during a pre-commit
  flush aborts the route (`texture:fallback_scaled_grow`).
- **Its shared-memory pages are valid**, so no upload is needed first. At scale > 1 they are used only for
  invalidation.

Further details:

- The same checks run again at encode time (`texture:fallback_scaled`).
- WAR on the scaled source does not arise: loads only read it, and a later resolve in the open command buffer runs
  after the pre-commit work.
- There is no new event or fence. It reuses the step 7/8 GPU-only `MTLEvent` and the fence chain (row 25).
- Switch: `FABLE2_SCALED_TEXTURE_PRECOMMIT=0` (default on; needs `FABLE2_TEXTURE_PRECOMMIT`).
- New counters: `tr_n_tex_ld_scl_{elig,open,nores,inv}`.
- `FABLE2_TEXTURE_PRECOMMIT_VERIFY=1` now also compares scaled sources. The log reports the number of scaled samples.

**Results.** 2×, uncapped turn30, Old Tomb, 20–50 s, `f2diag gpu`:

| | Step 1 base | Switch off | **Switch on** |
|---|---|---|---|
| FPS in window | 95.2 | 95.0 | **98.4** (final build 98.8) |
| GPU ms mean / p95 | 10.41 / 16.43 | 10.45 / 16.44 | **10.09 / 16.41** (final 10.06 / 16.39) |
| Main pass ms/frame (passes per frame) | 4.45 (5.88) | 4.49 (5.85) | **4.12 (2.47)** |
| Main-pass breaks per frame | 3.89 | 3.86 | **0.48** (texture fallback_upload 0.40, command-buffer end 0.09) |
| Scaled loads per frame: eligible / blocked by an open-command-buffer resolve | — | 0 / 0.59 | 5.5 / 0.6 |
| GPU busy, gaps > 100 µs per frame | | 98.77%, 0.39 | 98.85%, 0.36 |

- The gain is 0.35 ms (3.4%), at the low end of the 0.5–1.8 ms estimate. As in step 8, the open pass now waits
  inside its span for the pre-commit work. The `rt_transfer` spans after the main pass grow for the same reason.
  Busy % and idle gaps are unchanged, so there is no bubble.
- Visual checks:
  - `f2diag images dark|blue`, on against off: 91 / 89 motion frames flagged, 0 dips above 1%, 0 blue.
  - The 2× area run (Bower Lake after the load) matches step 1 by eye (`cmp/area40-*.png`).
- Verify (`FABLE2_TEXTURE_PRECOMMIT_VERIFY=1`, 2× sweep):
  - Final build: 19,000 samples, of which **12,527 scaled-resolve**, 0 mismatches (about 37 GB compared).
  - Earlier build: 20,000 samples, 0 mismatches.

### Item 4: setting and launcher control

- `[graphics] resolution_scale = 1` (1–3, clamped). `src/core/fable2_graphics_config.h` exports it as
  `FABLE2_RES_SCALE` unless the environment sets it.
- Dev Launcher "Internal resolution": Native (1280×720) / 2× (2560×1440) / 3× (3840×2160, for Max-class GPUs).
  - Default Native, stored as UserDefaults `resolutionScale`.
  - The launcher always passes `FABLE2_RES_SCALE` explicitly. CLI: `--resolution native|2|3`.
- The build-time config test covers both new keys.
- Checked: a test session whose copied config had `resolution_scale = 2` rendered at 2× with no environment
  variable set (log: `[fable2-config] resolution_scale=2 (fable2_config.toml)`).
- Snapshot: `out/validation/resolution-step2-20261009/launcher-snapshot.png`.

### Item 5: MetalFX upscaler

- `[graphics] metalfx = false`, with `FABLE2_METALFX=1|0` overriding it. It seeds `metal_presenter_use_metalfx`
  after the plugin loads; an explicit cvar still wins.
- Dev Launcher "Upscaler: Bilinear / MetalFX", default Bilinear. CLI: `--upscaler`.
- The presenter now logs `[metalfx] spatial scaler A -> B`, or `not applied: output … is not larger than the
  internal …`.
- 1× in the 2560×1440 test window: `spatial scaler 1280x720 -> 2560x1440 (created)`. The image is visibly sharper
  than bilinear on stone texture and HUD edges; 2× native is sharper again (`cmp/metalfx-scene.png`,
  `cmp/metalfx-text.png`; columns bilinear / MetalFX / 2×).
- Uncapped 1× with MetalFX: 175.5 FPS, GPU 5.66 ms, against 175.1 FPS and 5.69 ms without. The presenter pass was
  outside the traced command buffers, so its cost read as about 0 (measured in §22: about 0.16 ms at 1440p, 0.4 ms
  at 4K per present).
- 2× with MetalFX in the same window: `not applied` (output equals input), so bilinear is used. It only helps when
  the window is larger than the internal resolution.

### Item 6: fur fixes on by default

- `FABLE2_MEMEXPORT_PRESERVE` and `FABLE2_MEMEXPORT_READBACK` are on when unset; `=0` disables them. The log shows
  the source.
- The launcher toggles pass explicit `1`/`0`.
- Checked: unset gives `on (…=unset)` and `=0` gives `off`.
- No test referenced the old defaults. The 2026-10-07 playtest notes and bug-patterns rows 1–2 are updated.

### Final sweeps against step 1

Old Tomb save. Uncapped is turn30 with the GPU trace; capped is the default sweep at 60.

| Scale | Uncapped FPS | 1% low | p95 frame | GPU mean / p95 | Main-pass breaks per frame | 60-capped: FPS, p95, 1% low, frames > 33 ms |
|---|---|---|---|---|---|---|
| 1× step 1 | 173.7 | 89.7 | 10.1 | 5.79 / 9.99 | 0.38 | 60.0, 17.9, 54.7, 0 |
| 1× step 2 | 175.1 | 89.6 | 10.16 | 5.69 / 10.02 | 0.42 | 60.0, 18.0, 53.9, 0 |
| 2× step 1 | 95.2 | 50.9 | 16.7 | 10.41 / 16.43 | 3.89 | 60.0, 18.0, 53.7, 0 |
| **2× step 2** | **99.1** | **55.2** | 16.56 | **10.06 / 16.39** | **0.54** | 60.0, 17.84, 54.4, 0 |
| 3× step 1 | 52.6 | 33.5 | 28.1 | 18.90 / 27.59 | 3.87 | 50.7, 28.2, 30.9, 10 |
| **3× step 2** | 53.4 | 32.9 | 28.11 | **18.53 / 27.51** | **0.44** | 50.7, 28.26, 32.9, 0 |

These are single runs. The 1× differences are within run-to-run spread.

### Switches added or changed in step 2

| Switch | Default | Meaning |
|---|---|---|
| `[graphics] resolution_scale` / `FABLE2_RES_SCALE` | 1 | Internal resolution. The environment variable wins |
| `[graphics] metalfx` / `FABLE2_METALFX` | false / unset | MetalFX spatial upscaler |
| `FABLE2_SCALED_TEXTURE_PRECOMMIT` | on | `=0` keeps scaled loads in the current command buffer |
| `FABLE2_MEMEXPORT_PRESERVE`, `FABLE2_MEMEXPORT_READBACK` | **on** (was off) | `=0` disables |
| `FABLE2_MORPH_READBACK_DUMP=<dir>` | unset | Validation only: dump the morph readback bytes |

**Still open:**

- A box-filter morph downscale (optional).
- A downsample filter for 3× in small windows.
- CAS/FSR.
- The phase-2 abort dump (§6).
- "Auto by GPU" for the resolution.

## 9. Retina check (2026-10-09)

**Report:** switching Native / 2× / 3× showed no visible difference on the maintainer's display, and the log said
`Metal surface connected successfully: 2560x1440 (scale=1, drawable=2560x1440)`.

**Measured** (background test session, 2×; window frame from `CGWindowListCopyWindowInfo` by owner pid, screen from
`NSScreen`/`CGDisplayCopyDisplayMode`; scratch harness and probe in `out/validation/retina-20261009/harness/`):

| | Value |
|---|---|
| Display | 3840×2160 panel (a TV) in "looks like 2304×1296": macOS renders a 4608×2592 framebuffer (backing scale 2) and scales it to the panel by 0.83 |
| Window | content 1280×720 points (frame 1280×752 with the title bar) at 815,104, inside the visible area 2245×1266; not clamped or oversized |
| Window pixels (SDL) | 2560×1440, which is what the SDK surface hands the presenter |
| Presenter drawable | 2560×1440 = window pixels, 1:1 (`capture --screen` 2560×1440) |
| Layer `contentsScale` | **1** (the presenter overwrote the 2 that SDL's Metal view sets) |

- The `scale=1` in the old log line was the presenter's *extra* multiplier, which is 1 because the size is already in
  pixels. It did not mean the window lacked Retina backing.
- With an explicit `drawableSize` and the default resize gravity, `contentsScale = 1` has no visible effect.
- So the window was already Retina-sharp. Resizes and moves between displays already reconnect the presenter with the
  new pixel size (SDL `PIXEL_SIZE_CHANGED` → `Window::OnActualSizeUpdate` → `Presenter::OnSurfaceResizeFromUIThread`).
- Why 2× looks similar there: the 1280×720-point window is 2560 framebuffer pixels, but only about 2133×1200 panel
  pixels. Native is upscaled 1.67× on the panel and 2× is slightly downsampled. The difference exists in the
  drawable (§8 `cmp/metalfx-*.png`) but is small at TV viewing distance.
- **The lever is a bigger window.** At fullscreen on that display (4608×2532 pixels), Native is upscaled 3.6× and 2×
  is upscaled 1.8×. The window is resizable (drag, or the green button for macOS fullscreen). The SDK cvar
  `fullscreen` (in `fable_2.toml`, currently `false`) and `window_width`/`window_height`/`resolution` exist. The
  Dev Launcher exposes none of them, and only the window position is saved, not its size (now addressed, §10).

**Change (correctness only, no change to the pixel path):** `metal_presenter.mm`
`ConnectOrReconnectPaintingToSurfaceFromUIThread` sets `contentsScale` to the window's `backingScaleFactor` (from the
layer's `NSView`, falling back to pixels ÷ layer bounds), and the paint path keeps it there instead of resetting it to 1.
The connect is re-run on every pixel-size change, so it follows display moves. The drawable stays the pixel size.
`FABLE2_RETINA=0` restores `contentsScale = 1`. New log line:

`Metal surface connected successfully: 2560x1440 px = 1280x720 points (backing scale=2, contentsScale=2, extra drawable scale=1, drawable=2560x1440) [retina] on (FABLE2_RETINA=unset)`

**Unchanged by design:**

- ImGui overlays are laid out in drawable pixels and the turbo notice scales with the drawable height, so they are
  the same size as before.
- SDL mouse coordinates and the saved window position are in points and do not touch the layer.

**Validation** (one build, first launch counted; all verdict `ok`, 0 AV, 0 `[FATAL]`, `pgrep -x fable_2` = 1 after each):

| Run | Session | Result |
|---|---|---|
| before, 2× | 20261009-165706-fb8eb484 | measurements above |
| after, 2× (first launch after the build), F7 | 20261009-170043-17d993a4 | `contentsScale=2`, drawable 2560×1440. The turbo notice is the same size and position, and as sharp as step 1 (`out/validation/retina-20261009/cmp/turbo-{before,after}.png`). Window 1280×752 at 815,104 (same) |
| `FABLE2_RETINA=0`, 2× | 20261009-170200-21e55fad | `contentsScale=1`, `[retina] off`; same drawable and window |

No Dev Launcher control was added for `FABLE2_RETINA`: it changes nothing visible and is a diagnostic kill switch only.

## 10. Display options: windowed / fullscreen, window size memory (2026-10-09)

A 2× image only looks sharper when the window is large (§9). There are now two display options: start
fullscreen, and remember the window size.

**Display mode.** `[graphics] fullscreen = false` in `fable2_config.toml`. `FABLE2_FULLSCREEN=1|0` overrides it, and
the Dev Launcher's **Display: Windowed / Fullscreen** picker always passes it (CLI `--display windowed|fullscreen`).
Both seed the SDK `fullscreen` cvar before the window is created (`SeedFullscreen` in
`src/core/fable2_graphics_config.h`). Precedence, highest first:

1. `--fullscreen=...` on the command line.
2. `FABLE2_FULLSCREEN` (Dev Launcher). This also beats a `fullscreen =` line in `fable_2.toml` and `REX_FULLSCREEN`,
   because the SDK writes that line by itself whenever its settings screen (key 4) saves. A stale `fullscreen = false`
   there must not silently override the launcher.
3. `fullscreen =` in `fable_2.toml`, or `REX_FULLSCREEN`. This keeps a toggle saved from the settings screen across
   launches when the game is started without the launcher.
4. `[graphics] fullscreen` (default `false`). The SDK's own default would be `true`.

The `.app` launcher (`tools/macos/AppLauncher.m`) and `run_macos*.command` used to pass `--fullscreen=false`, which
outranked everything. They no longer pass it.

**Fullscreen mode.** The SDK calls `SDL_SetWindowFullscreen(window, true)` with no display mode, which is SDL's
desktop fullscreen. On macOS, SDL3 turns that into **native Spaces fullscreen**: the window gets its own Space, with
the system animation and the auto-hiding menu bar, because `SDL_VIDEO_MAC_FULLSCREEN_SPACES` defaults to on. It is
not a borderless window over the desktop. The display mode is not changed. `fullscreen_exclusive` is a separate,
unchanged cvar.

**Window size memory.** `fable2_window.toml` now holds the last windowed frame:

```
x = 600
y = 200
width = 1700
height = 950
```

`x`/`y` are the client-area position in screen coordinates; `width`/`height` are the client size in points.
- **Saving.** The frame is saved on every move and every resize (`SDL_EVENT_WINDOW_MOVED` / `RESIZED`), only while
  windowed. It is not saved while fullscreen, maximised or minimised, or while the window covers the whole display
  (a native fullscreen entered with the green button resizes the window before SDL sets its fullscreen flag).
- **Restoring.** The frame is applied before the window is first shown:
  - Source-built SDK: `patches/sdk/window-initial-position.patch` (B5) adds `Window::SetInitialSize` next to
    `SetInitialPosition`, applied while the window is hidden.
  - Prebuilt SDK, or `FABLE2_SDK_WINDOW_POSITION=0`: the `SDL_EVENT_WINDOW_SHOWN` watch applies it.
- **Limits.** The size is raised to at least 640×360 and clamped to the usable area of the display that holds the
  title bar, or the current display if the position is off-screen. Sizes outside 1..16384 in the file are ignored.
  Explicit `window_width`/`window_height` cvars win over the saved size, and the `monitor` cvar wins over the
  position.
- **Fullscreen.** When the game starts fullscreen, the frame becomes the windowed frame, so leaving fullscreen
  (settings screen or green button) returns to the saved size and position. `SDL_EVENT_WINDOW_LEAVE_FULLSCREEN` also
  re-applies it.
- **Old files.** Files with only `x`/`y` still restore the position and keep the default size.
- **Dev Launcher write-back.** `tools/fable2_dev_session.py` accepts exactly the keys `{x, y}` or
  `{x, y, width, height}` (ints, int32 position, size 1..16384), still without `tomllib`, because the launcher runs
  the system Python 3.9.

Test sessions get a test-only test-control command (`metal_test_control.mm`):
`window frame | size <w> <h> | move <x> <y> | fullscreen 0|1`. `fullscreen` sets the cvar the way the settings
screen does.

**Validation** (2×, one build, first launch after the rebuild counted). All runs: verdict `ok`, 0 AV, 0 `[FATAL]`,
`pgrep -x fable_2` = 1 after close. Bounds are from CGWindowList by owner at about 200 Hz and include the 32-point title
bar. Harness: `out/validation/display-options-20261009/harness/`.

| Run | Session | Result |
|---|---|---|
| Windowed, test state seeded `815,136 1600×900` (first launch after the rebuild) | 20261009-171442-67a7416c | First on-screen bounds were `704,104 1600×932`, with no earlier frame and no jump. Cocoa pulled `x` left to 704 because 815+1600 is wider than the 2304-point screen. Drawable 3200×1800. `window size 1700 950` gave drawable 3400×1900, then `window move 600 200`; the file then held `600,200 1700×950` |
| `FABLE2_FULLSCREEN=1` over a `fullscreen = false` line in the copied `fable_2.toml`, seeded `600,200 1700×950` | 20261009-171549-501ed7ac | `fullscreen=true (source: FABLE2_FULLSCREEN)`. The window covered the display at `0,0 2304×1296`. `capture --screen` was 4608×2592 (guest 2560×1440). `window fullscreen 0` brought back `600,168 1700×982` (client `600,200 1700×950`), drawable 3400×1900. The saved file was unchanged by the transition |
| Prebuilt-SDK fallback (`FABLE2_SDK_WINDOW_POSITION=0`), seeded `600,200 1700×950` | 20261009-171623-9cbe90cb | `Restored window size: 1700x950 (before first show)`. First on-screen bounds were `600,168 1700×982`, with no jump. A resize to 1500×860 was saved |

The unit test `tests/window_position_tests.cpp` adds checks for size saving on move and resize, size restore, old
x/y-only files, minimum clamping, invalid sizes and the explicit-cvar case. It passes when built against the
source-built runtime. `tests/dev_session_trace_options_tests.py` passes on Python 3.9.6 and 3.14 (21 tests).

## 11. High-resolution FPS levers, step 3 (2026-10-10)

Branch `feature/hires-fps` (from `macos` 85032d0). Native shaders on (default). M5 Max, Old Tomb save
`hero1-dog-live_2026-10-07_14-45-08`, uncapped turn30 with the GPU trace, `f2diag gpu` window 20–50 s, one run per
arm. Harness and logs: `out/validation/hires-fps-20261010/` (`go.sh <label> <scale> <uncapped|60> [--env …]`,
`sessions.txt`, `buildN.log`, `effpasses.py`). Every session: verdict `ok`, 0 AV, `pgrep -x fable_2` = 1 after close.
The first launch after each of the 3 builds is counted.

Restore points: `../restore-points/2026-10-10-hires-fps-lever1-before.patch` (empty, clean tree),
`…-lever1-skip-prototype.patch` (the lever 3 skip, measured and removed) and `…-lever5-before.patch`.

**Baseline (today's macos build):**

| Run | FPS (window) | GPU mean / p95 | 1% low (sweep) | 60-capped: FPS, p95, 1% low |
|---|---|---|---|---|
| 1× | 188.8 | 4.79 / 8.59 | 96.4 | 60.0, 17.88, 54.5 |
| 2× | 124.0 | 8.04 / 12.69 | 69.0 | 60.0, 17.78, 54.8 |
| 3× (counter build) | 69.1 | 14.39 / 20.19 | 43.9 | — |

### Lever 3: re-copying unchanged scaled resolves: no redundant work

- **Counters** (`tr_n_rsv_*`, `tr_n_tex_ld_scl_all`): at 2×, 31.7 copy resolves per frame (all scaled), 30.3 scaled
  texture loads (about one per resolve), 0 scaled-buffer grows.
- **Prototype** (build 1): each host render target carried a content generation, bumped by any draw that binds it
  and by any transfer or resolve clear into it. Each destination range kept the exact signature of its last copy:
  source targets and generations, tile rectangles, copy constants, destination, and the scaled-buffer epoch. The
  signature was dropped by any shared-memory write notification on its pages (CPU or GPU). A copy with an
  unchanged signature would have been skipped, together with its texture invalidation.
- **Result:** **0 redundant copies** in 30 s. Every resolve's source had been drawn since the last copy to that range.
  The game re-renders every target every frame, so the skip never fired. Uncapped 2×: 124.9 FPS off, 124.7 on.
- Removed again (no gain, extra code on the resolve path); kept in the restore point above. The other half of the
  original lever (sampling resolved data without untiling each load) is still L-sized and unmeasured.

### Lever 4: load/store actions: upper bound about 0.1 ms, not adopted

- Stencil is live: 1,335 of 1,643 depth-bound draws per frame have the stencil test on (`tr_n_draw_stencil` /
  `tr_n_draw_ds`).
- Diagnostic only, unsafe: `FABLE2_RT_STORE_DIAG=stencil|depth|both` sets those stores to DontCare. Stencil:
  GPU 8.10 → 8.02 ms (noise level). Depth and stencil: 8.20 ms, no gain (and the image is wrong).
- No pass can prove its stored depth or stencil dead at its end. EDRAM contents persist, and the next consumer,
  often in the next frame, is not known yet. Colour already uses DontCare where the transfer plan allows.

### EDRAM ownership transfers: not avoidable in a contained way

Counters `tr_n_xfer_*`; with `FABLE2_XFER_TRACE=1`, a 10 s log line lists the top source → destination pairs and the
separate-pass reasons.

- Per frame at 2×: 75.8 transfers into 47.3 destinations. By kind: 33.3 colour↔depth, 23.8 MSAA change, 13.3 format
  change, 5.5 same format (base or pitch). 37.4 destinations are folded into the draw pass and 9.9 get a separate
  transfer pass.
- Top pairs are the game reusing the same EDRAM tiles for different targets:
  - base 0 `<2t>` 4xMSAA D24S8 ↔ 1xMSAA 8888, about 3 per frame each way;
  - the main depth at 2xMSAA ↔ 4xMSAA;
  - a 4xMSAA 8888 target → the main depth.
  The source was always drawn in between, so by EDRAM rules the data is live.
- 88% of the separate passes are depth transfers whose host-depth source is the destination itself. Metal cannot
  read the depth attachment being rendered, so they need the `rt_depth_store` compute (8.7 per frame) and their own
  pass.
- Avoiding any of them needs proof that the destination is fully overwritten before it is read, which needs a
  draw-stream lookahead. Not contained.

### Lever 5: effect targets at native 1×: classification only (stage a)

Trace `FABLE2_EFFECTS_NATIVE_TRACE=1` (diagnostic, no behaviour change), counters `tr_n_eff_*`.

- **Eligible:** a colour, 1xMSAA target whose pitch is at most half the widest depth-tested scene pass (14 tiles
  here, so ≤ 7). This rule depends only on the key, so it works for any scale.
- **Classification at 2× (3× in brackets; identical counts):**
  - 65.3 (65.8) draws per frame go to eligible targets with no depth/stencil.
  - 10.3 (10.5) draws bind an eligible colour target together with depth. These targets would conflict.
  - 23.0 copy resolves come only from eligible targets; 3.0 mix eligible and full-scale tiles.
  - Transfers: 11.5 between eligible targets and **32.1 between an eligible and a full-scale target**. That is 42% of
    all transfers, so mixed-scale transfer shaders are required; a fallback to full scale would disable most of the
    lever.
- **GPU time of those passes** (`effpasses.py` on `f2diag gpu --json`; colour-only, 1xMSAA, ≤ half the main width):

| Scale | Span (ms per frame) | Fragment (ms) | Passes per frame |
|---|---|---|---|
| 1× | 0.82 | 0.57 | 25.2 |
| 2× | 1.24 | 0.89 | 25.9 |
| 3× | 1.79 | 1.34 | 25.5 |

- Rendering them at 1× would save about 0.4 ms at 2× and 1.0 ms at 3× in the passes themselves. About 23 of the 31.7
  resolves and their scaled texture loads would shrink as well.
- Estimate: **0.5–0.8 ms at 2×** (6–10%) and **1.0–1.6 ms at 3×** (7–11%), before the cost of the mixed-scale
  transfers. It would also bring back the 1× bloom width.
- Sketch (sent for a decision; XL, high risk):
  - SDK: a key bit plus an `Update` hook; separate render-target and texture scales in the DXBC translator;
    unscaled-resolve marking in the texture cache.
  - Metal: per-target scale for creation, viewport and system constants; a translator variant in the pipeline
    cache; 1× resolves into shared memory; mixed-scale transfer MSL; resolve clears.

### Before / after

No lever changed rendering, so "after" is the final build (counters only).

| Scale | | FPS | GPU mean / p95 | 1% low |
|---|---|---|---|---|
| 1× | before | 188.8 | 4.79 / 8.59 | 96.4 |
| 1× | after | 184.8 | 4.88 / 8.46 | 96.4 |
| 2× | before | 124.0 | 8.04 / 12.69 | 69.0 |
| 2× | after | 123.4 | 8.08 / 12.58 | 69.6 |

These are within run-to-run spread (2× off/on/off runs: 8.02–8.10 ms).

### Switches added

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_XFER_TRACE=1` | off | Log the top transfer pairs and separate-pass reasons every 10 s |
| `FABLE2_EFFECTS_NATIVE_TRACE=1` | off | Lever 5 classification counters |
| `FABLE2_RT_STORE_DIAG=stencil\|depth\|both` | off | **Diagnostic only, unsafe:** DontCare depth/stencil stores |

New always-on per-frame counters: `tr_n_rsv_{copy,scaled,grow}`, `tr_n_tex_ld_scl_all`, `tr_n_xfer_*`,
`tr_n_draw_{stencil,ds}`.

Files: `backends/metal/port/rex/graphics/metal/metal_render_target_cache.cc/.h`, `metal_shared_memory.h`,
`metal_texture_cache.cc`, `metal_command_processor.cc`; `docs/resolution-scale.md`, `docs/bug-patterns.md`.

## 12. Effect size at 2× / 3× (lever 5, 2026-10-10)

Goal (game preservation): the original bloom, blur and glow width at 2× and 3×. FPS is a bonus. Same branch,
harness and folder as §11. Restore points: `../restore-points/2026-10-10-hires-fps-lever5b-before.patch`
(before stages b–d). Crops and diffs: `out/validation/hires-fps-20261010/cmp/` (`cmp.py` picks one capture per
run, by sweep phase or by time, crops a 1280×720-space rectangle and prints the mean abs difference against the
first column).

### Cause

The narrower effects (breakage row 4) are not caused by render-target size. The SDK DXBC translator has the cvar
`draw_resolution_scaled_texture_offsets`, default true. For a resolution-scaled texture it divides every
texture-fetch texel offset (`tfetch … OffsetX/OffsetY`) by the scale, so the offsets count host texels, not
guest texels. Every blur, bloom and glow kernel built from fetch offsets shrinks to ½ (2×) or ⅓ (3×) of its screen
size. Setting the cvar to false keeps offsets in guest texels: the original kernel width at any scale.

### Shipped: `[graphics] effects_native_scale` (default true)

- `src/core/fable2_graphics_config.h` `SeedEffectsNative` sets `draw_resolution_scaled_texture_offsets` to the
  inverse once the plugin has loaded. `FABLE2_EFFECTS_NATIVE=1|0` overrides the config; an explicit cvar wins over
  both. Log: `[fable2-config] effects_native_scale=… (source: …)`.
- Dev Launcher: "Bloom/effects: Native 1× (original look) / Full resolution", stored as UserDefaults `effectsSize`.
  It is disabled at Native resolution. The launcher always passes `FABLE2_EFFECTS_NATIVE`. CLI: `--effects
  original|full`.
- No effect at scale 1. A changed value re-translates the affected shaders once (the first launch after
  switching is slower; steady state costs nothing measurable, below).

**Correctness** (M5 Max, Old Tomb save; every session verdict `ok`, 0 AV, `pgrep` 1 after close):

| Check | Result |
|---|---|
| Green interact glow (`v-outline-zoom.png`: 1× \| 2× full \| 2× original \| 3× full \| 3× original \| 3× native targets) | Original matches 1×: a wide, soft halo. Full narrows it (2×) or nearly removes it (3×). Mean abs difference against 1×: 2× 7.5 original vs 9.0 full; 3× 8.9 vs 13.7 |
| Torch (`v-torch.png`), chest glow (`v-t400-dog.png`, `v-t180-floor.png`) | Halo closer to 1× with original; core unchanged |
| Dog fur (`v-t400-dog.png`) | Correct coat at 1×/2×/3×, both settings |
| Floor and character shadows (`v-t180-floor.png`) | Sharp at both settings |
| Pause menu render-to-texture preview (`ui-pause.png`), loading screen (`ui-loading.png`), close view (`ui-closeup.png`) | Unchanged against 1× |
| Bower Lake area run (`area-t450.png`, `area-t300.png`) | Renders normally at both settings |
| `f2diag images` (capped sweeps) | blue 0 everywhere; dark 55 original vs 64 full at 2×, 50 vs 50 at 3× (the known dark-wall motion frames; 21 at 1×) |

**Performance** (uncapped turn30 or capped default sweep, `f2diag gpu` 20–50 s, one run per arm, first launch after
the build counted):

| Run | FPS | GPU mean / p95 | 1% low | 60-capped FPS, p95, 1% low |
|---|---|---|---|---|
| 1× | 188.8 | 5.04 / 8.57 | 99.2 | 60.0, 17.98, 54.5 |
| 2× original (default) | 121.8 | 8.18 / 12.76 | 69.6 | 60.0, 17.85, 54.4 |
| 2× full | 121.4 | 8.20 / 12.92 | 68.2 | 60.0, 17.82, 54.7 |
| 3× original (default) | 68.5 | 14.54 / 20.20 | 44.0 | 55.5, 22.20, 41.4 |
| 3× full | 68.0 | 14.61 / 20.46 | 43.5 | 55.6, 22.40, 41.3 |

An earlier "+0.7 ms" reading for original effects was the first launch after the change compiling the
re-translated shaders. It is not a steady cost.

3× note: full-resolution effects lose most of the glow at 3×, so the original setting matters more there. Same
default for every scale.

### Removed experiment: native 1× effect render targets

**Removed from the branch on 2026-10-10.** The full change is kept as
`../restore-points/2026-10-10-effects-native-targets-experiment.patch`, with a README beside it on how to re-apply
it and on the open issue. It was built in stages b–c as sketched in §11 (lever 5), switched on with
`FABLE2_EFFECTS_NATIVE_TARGETS=1`:

- **SDK, `backends/metal/sdk-metal-effects-native.patch`.** `tools/build_macos_metal.py` applies it to the
  plugin's SDK tree after `sdk-metal.patch`. It adds:
  - a `native_scale` bit in `RenderTargetKey`;
  - the `UseNativeScaleColorTargets` hook in `RenderTargetCache::Update`, and an accumulation check so one pass never
    mixes scales;
  - resolve clears follow the current owner's scale;
  - `GetTileOwner` and two tile-conversion wrappers;
  - `TextureCache::MarkRangeAsResolvedUnscaled`;
  - a `render_target_native_scale` pixel-shader modification bit, which unscales the pixel position (PsParamGen,
    memexport sample).
- **Metal.** The hook works as follows: colour-only, 1xMSAA passes at most half as wide as the widest depth-tested
  pass (learned) stay 1×. Then:
  - per-target creation size;
  - per-draw viewport, scissor, point size and depth-bias scale;
  - resolves of native targets as at 1× (unscaled copy constants, pipelines, EDRAM dump layout, shared-memory
    destination; the morph readback takes the 1× path);
  - mixed resolves are first moved to the majority scale with ownership transfers;
  - mixed-scale ownership transfers (the transfer MSL maps the destination pixel through the source scale; a
    scaled-to-native colour copy averages the source block).
- **Measured, 2× / 3×:**
  - 75 draws and 21 resolves per frame go native, with 40 mixed-scale transfers per frame;
  - GPU 8.18 → 7.79 ms (+7 FPS) at 2× and 14.54 → 13.60 ms (+4.7 FPS) at 3×; 3× capped 55.5 → 58.7 FPS;
  - 0 AV, verdict ok.
- **Why it was removed.** With these targets the green glow is narrow and dimmer again (column 6 of
  `v-outline-zoom.png`), with or without the averaging downscale, and the cause was not found. The transfer and
  resolve code is back in its pre-experiment form.

### Switches (§12)

| Switch | Default | Meaning |
|---|---|---|
| `[graphics] effects_native_scale` / `FABLE2_EFFECTS_NATIVE` | true / unset | Original effect size at 2×/3× (`draw_resolution_scaled_texture_offsets=false`) |
| `FABLE2_EFFECTS_NATIVE_TRACE=1` | off | Lever 5 classification counters `tr_n_eff_*` (diagnostic, §11) |
| `FABLE2_EFFECTS_FOOTPRINT` | on | 2026-10-14: with original-size effects each offset tap covers its whole guest texel, which removes the striped bloom at 2×/3× (`docs/bloom-banding-20261014.md`) |

Files (§12, after the removal): `src/core/fable2_graphics_config.h`, `fable2_config.cpp/.h`, `fable_2_app.h`;
`config/fable2_config.toml`, `tests/config_defaults_tests.cpp`; `tools/macos/DevLauncher/main.swift`; `README.md`.

## 13. Tile-memory EDRAM transfers (2026-10-11)

Branch `feature/tile-edram` (from `macos` be27a70). Goal: cut the attachment load/store traffic of the separate
EDRAM ownership-transfer passes, keeping EDRAM semantics exact. Same machine (M5 Max) and Old Tomb save as §11.
Harness and scripts: `out/validation/tile-edram-20261011/` (`arm.sh <arm> <runs…>`; `tail.py` marginal GPU time per
pass site; `ctr.py` per-frame counters; `bytes.py` full-texture model, see below). Every run: verdict `ok`, 0 AV, no
CONTENTION, `pgrep -x fable_2` = 1 after close. One or two runs per arm.

### What the 3.84 ms really was

- The perf audit's "two `rt_transfer` passes into the main colour target and two into the main depth target" are
  **resolve clears**, not ownership transfers. After each main-scene resolve (`rt_resolve`), the game clears colour and
  depth over the resolve rectangle, which is only part of the 2240×2352 (2×) EDRAM-height texture. So each clear pass
  loads, draws a clear quad and stores.
- Their 3.4 ms/frame of **span overlaps the main draw pass**: the vertex-stage timestamp of the next encoder starts
  early, and the first clear's span (1.28 ms) almost equals the draw pass's (1.30 ms). The time they add to the busy
  timeline (`tail.py`, how far each encoder pushes the end of the serial GPU timeline) is **0.20 ms/frame at 2×** and
  0.05 ms at 1×.
- The separate passes that do cost time are the other ~11 per frame, 8.7 of them **self-source depth transfers**
  (`host_depth_source` = destination) on the smaller depth targets, plus their `rt_depth_store` compute. Together that is
  0.68 + 0.08 ms/frame at 2× and 0.27 + 0.03 ms at 1×.
- **A full-texture bytes model is wrong on Apple GPUs.** `width × height × samples × bpp` for every Load/Store gives
  8 GB/frame at 2×, which is physically impossible, so untouched tiles of these tall textures are not loaded or stored.
  The new counters therefore use a **touched-rectangle model**: the transfer or clear rectangles × samples × bytes per
  sample (5 for D32S8, 8 for RGBA16F/RG32, 4 otherwise), counted once for a Store and once more for a Load.

### Instrumentation (stage B, always on, trace-only cost)

- Separate transfer passes are labelled by kind in the pass trace: `rt_transfer_clear` (resolve clear only),
  `rt_transfer_selfdepth` (needs `rt_depth_store`) and `rt_transfer` (other).
- Per-frame counters: `tr_n_xfp_{clear,self,other}` (separate passes by kind), `tr_n_xfp_kb` /
  `tr_n_xfp_clear_kb` (touched-rect KB loaded + stored by them), `tr_n_tile_selfdepth` (K1 destinations moved into
  the draw pass).

### K1 (shipped, default on): self-source depth transfers in the draw pass

`FABLE2_TILE_EDRAM_SELFDEPTH` (`=0` off); master `FABLE2_TILE_EDRAM=0` turns every tile-EDRAM change off.

- Before: `CanQueueDrawPassTransfers` rejected a depth transfer whose host-depth source is the destination itself,
  because the transfer shader must read the destination's previous depth and Metal cannot read the depth attachment
  being rendered. The standalone path ran `rt_depth_store` (depth → EDRAM buffer), then its own transfer pass
  (load + store of the destination), then the draw pass loaded it again.
- Now: at queue time in `Update`, before the draw pass opens, the same `rt_depth_store` compute is encoded. This goes
  through `RequestTransferCommandBuffer`, which flushes prepared draws and ends the open render encoder, as the
  standalone path does. The transfer quads then run inside the draw pass with `host_depth_source_is_copy`, reading the
  EDRAM buffer copy with the same shaders and the same constants.
- Any later fallback (a flush before the draw is encoded, a failed preflight) re-runs the store from the unchanged
  destination texture, so it stays exact. It also requires `texture() == draw_texture()` for the destination.
- No new Metal feature: it works on every Apple GPU family. Tile shaders and imageblocks were not needed.
- Exactness: no shader or data change, only where the quads are encoded. Image parity (`perf-pass-20261011/parity.py`,
  4×4 linear-light regions, idle phase) at 1×: K1 vs off median 0.05% / max 0.10%, the same as off vs off (0.04% / 0.12%).
  One earlier K1 pair differed by ~4% only because the game showed a different Theresa subtitle line (checked by eye).
  At 2×, K1 vs off sits inside the same-build spread. Dog fur, the green interact glow, torch bloom, shadows and the HUD
  look the same.
- Validation: sweep at 1× and 2× (captures), area, boot, F5 (saved), bootloop 3/3 pass, every perf run; all 0 AV.
- Encoder state: K1 quads always land in a fresh render encoder, because the store's `RequestTransferCommandBuffer` ends
  the open one. They never inherit a guest draw's rasterizer state. See the §14 note "Transfer quads and the rasterizer state".

| | separate transfer passes / frame | touched-rect MB / frame (separate passes) | GPU ms (trace runs) | busy ms (`tail.py`) | uncapped FPS (clean) | cores 60-capped |
|---|---|---|---|---|---|---|
| 1× off | 9.6 + 5 clears | 72.2 | 5.40 | 5.30 | 180.6 | 1.06 |
| 1× K1 | 1.2 + 5 clears | 48.8 (−23) | 5.49 | 5.35 | 181.4 | 1.04 |
| 2× off | 10.0 + 5 clears | 291 | 8.89 | 8.76 | 114.0 / 112.4 | — |
| 2× K1 | 1.3 + 5 clears | 195 (−96) | 8.79 | 8.49 | 112.5 / 113.6 | — |
| 3× off | 10.0 + 5 clears | 654 | 15.85 | — | 62.6 | — |
| 3× K1 | 1.2 + 5 clears | 439 (−215) | 15.76 | — | 62.7 | — |

- **On the M5 Max, K1 is FPS-neutral** (all deltas within ±1.5% run-to-run). GPU busy dropped 0.27 ms at 2×, the
  per-frame GPU mean only 0.1 ms.
- **Bytes model vs GPU time:** at this machine's ~550 GB/s, the model predicts 0.04 / 0.17 / 0.39 ms saved at
  1× / 2× / 3×. Measured: about 0 / 0.1 / 0.1 ms. The model is an upper bound: the separate pass's load/store
  partly overlapped other work and is partly absorbed by framebuffer compression.
- **M1-class estimate (≈68 GB/s):** the upper bound is 0.34 / 1.4 / 3.2 ms per frame at 1× / 2× / 3×. Scaled by the
  measured/model ratio seen here (0.25–0.6), roughly 0.4–0.8 ms at 2× and 0.8–1.9 ms at 3× on a base M1. That is the
  case for keeping K1 on by default. It is an estimate, not measured on an M1.

### K2 (removed): depth + colour resolve clear in one pass

- Merged the colour and depth clear of the same resolve (same size and samples, no transfers) into one render pass:
  5 → 3 clear passes per frame at 2×.
- No bytes saved: 192.6 MB/frame of clear traffic both ways. Uncapped 2× FPS K1+K2 was 111.6 / 111.7 vs K1 alone
  112.5 / 113.6 (≈ −1.3%, consistent).
- Likely reason: two passes on different targets overlap on the GPU, while one pass with both big attachments has a
  larger tile footprint and serialises the two clears. A merge pays only when it removes a load/store.
- Removed at the maintainer's request. Patch and README: `../restore-points/2026-10-11-tile-edram-k2-clearmerge.patch` / `.README.md`.

### K3 (not done): resolve + clear inside the main draw pass (tile kernel)

Recommendation: **do not build K3 now.**

- What remains is the clear traffic, 48 / 193 / 433 MB/frame at 1× / 2× / 3× (5 clears), plus the resolve compute
  reading the target back. K3 would remove both with a tile kernel (`dispatchThreadsPerTile` over the implicit
  imageblock, Apple4+) that re-implements the resolve format/sample/tiling code bit-exactly, colour only (depth is not
  readable in tile functions).
- Applying K1's measured model-to-time ratio, that is ≈0.1–0.3 ms at 2× on this machine and perhaps 1–2 ms on a base M1.
  It is XL effort with real exactness risk on the guest-visible resolve output.
- A cheaper exact step with most of the same byte saving, should weak-Mac 2×/3× matter later: **deferred resolve
  clears**. Queue the clear rectangle like a draw-pass transfer and draw it at the start of the next pass that binds the
  target; any read of the target before that (resolve, transfer source, texture) flushes it. M effort, no tile shaders.
- Measure it first with `tr_n_xfp_clear_kb` and `tail.py` on the `rt_transfer_clear` site.

### Measurement rules learned here

- Never use xctrace or Instruments on the game. A Metal System Trace recording grew xctrace to ~50 GB and
  kernel-panicked the machine (2026-10-11). Use the pass trace (`FABLE2_GPU_TIME_TRACE=1`, `f2diag gpu`, `tail.py`),
  `sample` for CPU, and the touched-rect counters for bytes.
- Encoder span is not cost. Use the marginal busy time (`tail.py`) or an A/B.

### Switches (§13)

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_TILE_EDRAM=0` | on | Master kill switch for every tile-EDRAM change (today: K1) |
| `FABLE2_TILE_EDRAM_SELFDEPTH=0` | on | K1 off: self-source depth transfers go back to their own pass |

Files (§13): `backends/metal/port/rex/graphics/metal/metal_render_target_cache.cc/.h` (K1, refactored
`EncodeSelfHostDepthStore`, pass-kind labels, `tr_n_xfp_*`), `metal_shared_memory.h` (counters),
`metal_command_processor.cc` (counter records); `docs/resolution-scale.md`, the 2026-10-11 performance audit,
`docs/bug-patterns.md`, `docs/reference.md`. Restore points: `../restore-points/2026-10-11-tile-edram-stage{B,C}.patch`,
`../restore-points/2026-10-11-tile-edram-k2-clearmerge.patch`.

## 14. Deferred resolve clears (2026-10-11)

Branch `feature/deferred-clears` (from `macos` 9f93167). Goal: remove the load/store round trip of the separate
resolve-clear passes (`rt_transfer_clear`, §13), mainly for bandwidth-limited Macs at 2×/3×. The EDRAM semantics are
unchanged. Same machine (M5 Max), save and harness as §13. Scripts, logs and the copied pass/frame CSVs are in
`out/validation/deferred-clears-20261011/`:

- `arm.sh` (§13 harness plus `bootloop`);
- `summ.py` (FPS, GPU mean, `tail.py` busy, counters);
- `parity.txt`, `perf.txt`;
- `cmp/` (captures);
- `sessions/`.

Every run: verdict `ok`, 0 AV, no CONTENTION, `pgrep -x fable_2` = 1 after close. The first launch after each build is
counted.

### What happens per frame

Old Tomb, 2× pass trace:

1. Main pass A draws the 2xMSAA RGBA16F + D32S8 target, then resolve A runs.
2. Resolve A clears depth and colour as two passes over the resolve rectangle, part of the 2240×2352 target, so each
   pass loads and stores it.
3. Main pass B binds the same two targets with Load/Store.
4. Resolve B runs, with its own two clears.
5. Later the UI target (`c0:90` 2560×4096) is resolved and cleared.

That makes 5 clear passes per frame: 48 / 193 / 433 MB per frame of touched-rect traffic at 1× / 2× / 3×.

### Upper bound first (stage 2, diagnostic)

`FABLE2_DEFERRED_CLEARS_DIAG=skip` is unsafe (wrong image) and was used for measurement only. It drops every clear-only
resolve clear.

| | busy (`tail.py`) ms/frame | GPU mean | FPS |
|---|---|---|---|
| 2× off (2 runs) | 8.195 / 8.224 | 8.56 / 8.63 | 115.8 / 114.8 |
| 2× skip | 8.079 | 8.52 | 115.5 |
| 3× off (2 runs) | 14.82 / 14.85 | 15.43 / 15.36 | 64.5 / 64.8 |
| 3× skip | 14.47 (−0.36, −2.4%) | 15.11 | 65.0 |

The `rt_transfer_clear` site's marginal busy time is 0.11 / 0.40 / 1.13 ms per frame at 1× / 2× / 3× in this build. §13
measured 0.20 ms at 2×.

### Implementation (default on, `FABLE2_DEFERRED_CLEARS=0` restores today's path)

`metal_render_target_cache.cc/.h`:

- **Queue.** `Resolve` still changes EDRAM ownership at resolve time (`PrepareHostRenderTargetsResolveClear`). When
  neither clear target has transfers of its own, it queues (target, 64-bit clear value, rectangle) instead of encoding
  the clear passes. `needs_initial_clear` is reset as before.
- **Merge.** `Update` checks each bound target with a queued clear and no transfers into it in this update. If the clear
  can be drawn in this pass, it moves into a draw-pass slot that rides the existing draw-pass transfer machinery
  (`HasPendingDrawPassTransfers`, no prepared-draw queueing, guard). `EncodePendingDrawPassTransfers` then draws the
  clear quad first in the draw pass, before any transfers.
- **Same code as today.** The quad comes from the same `PerformTransfersAndResolveClears` clear code: same shader, same
  constants (clear value conversion), same rectangle and viewport/scissor, same sample count. Only the pipeline's
  attachment formats differ, and the other attachments are write-masked off.
- **Exactness rules:**
  - the attachment must be exactly the texture the separate pass would clear (`texture()` for depth with
    `texture() == draw_texture()`; `transfer_texture() == draw_texture()` for colour);
  - colour attachment 0 only (the clear shader writes `color(0)`);
  - no integer colour formats (same rule as draw-pass transfers);
  - before the quad, the encoder's cull, winding, fill, depth bias and depth clip are reset to the encoder defaults a
    separate pass starts with (new mutation bit; the draw path re-applies its rasterizer state);
  - any check that fails falls back to today's separate pass. Nothing is ever dropped.
- **Flush early, as today's separate pass.** A queued clear is flushed before anything else reads or writes its target:
  - a copy resolve whose EDRAM tiles that target owns (the same `GetResolveCopyRectanglesToDump` owner list the dump and
    direct-host resolve paths read);
  - any ownership transfer whose source, destination or host-depth source is that target. This covers `Update`'s
    queued and fallback transfers, the K1 self-depth store and a later clear's own transfers;
  - another resolve clear of the same target;
  - an EDRAM snapshot restore.

  `ClearCache` drops the queue, because the targets are destroyed. If the draw that bound a target is never encoded, the
  clear goes back to the queue: nothing read the target.
- **Not flushed** on memexport, texture fetch, present or command-buffer end. None of these read host render-target
  textures on this path. Textures and present read shared/scaled memory, and the morph readback reads the resolve
  output, which the resolve rule orders. Encoding order on the one queue is GPU order.
  `FABLE2_DEFERRED_CLEARS_SWAP_FLUSH=1` is a conservative arm that flushes everything at frame start; it was not needed.
- **loadAction=Clear: not built.** Today's clear-only pass always draws the quad: an explicit rule drops the load-action
  clear when there are no transfers. A hardware clear would therefore not be bit-identical to today. Also, no merged clear
  covered a whole target (`tr_n_dc_fullrect` = 0).
- **Pipelines.** The new clear-pipeline variants (multi-attachment formats) are tiny runtime compiles from the transfer
  MSL library, like every render-target-cache pipeline (none of those are in the binary archive): 35–180 µs each, a
  handful per session (`[rt-clear-pipeline]` log lines).

Counters (per frame, frame CSV):

- `tr_n_dc_deferred`, `tr_n_dc_merged`, `tr_n_dc_fullrect`, `tr_n_dc_requeue`;
- `tr_n_dc_fl_{resolve,xfer,reclear,preflight,guard,swap,other}`;
- `tr_n_dc_saved_kb` (touched-rect KB the merged clears' separate passes would have loaded and stored).

The result is identical at 1×, 2× and 3×: 5 deferred, **2 merged** (resolve A's depth and colour into main pass B), 3
flushed by transfers, 0 by any other reason, 0 full-target. The 3 that cannot merge are resolve B's pair (read at the
next frame's start by the main depth 2x→4x ownership transfer) and the UI target's clear (read by a transfer to the
`c0:70` target of the same EDRAM range).

### Results (off = `FABLE2_DEFERRED_CLEARS=0`, same build; 1–2 runs per arm plus the stage-2/3 builds)

| Scale | | clear passes / frame | clear MB / frame | busy ms | GPU mean ms | uncapped FPS | M1-class estimate (below) |
|---|---|---|---|---|---|---|---|
| 1× | off | 5 | 48.2 | 4.85 | 5.21 | 187.5 / 193.1 | — |
| 1× | on | 3 | 15.4 (−32.8) | 4.71 | 5.04 | 193.2 / 188.9 | −0.1 to −0.3 ms (bound 0.5) |
| 2× | off | 5 | 192.6 | 8.20 / 8.22 | 8.56 / 8.63 | 115.8 / 114.8 / 116.3 / 115.9 | — |
| 2× | on | 3 | 61.6 (−131.0) | 8.00 / 8.10 | 8.45 / 8.53 | 117.4 / 115.8 / 116.2 | **−0.5 to −1.3 ms** (bound 1.9) |
| 3× | off | 5 | 433.4 | 14.82 / 14.85 | 15.43 / 15.36 | 64.5 / 64.8 / 65.1 / 65.2 | — |
| 3× | on | 3 | 138.5 (−294.8) | 14.50 / 14.45 | 15.30 / 15.02 | 65.1 / 65.5 / 65.1 | **−1.1 to −2.8 ms** (bound 4.3) |

- **M5 Max:**
  - FPS-neutral at every scale (deltas within the ±1.5% run-to-run spread);
  - GPU busy −0.14 / −0.17 / −0.36 ms at 1× / 2× / 3×. At 3× the merge reaches the whole skip-all upper bound;
  - 60-capped at 1×: 60.0 FPS, p95 17.9 ms, 1% low 54.6 and the same `top` core use, off and on.
- **Measured slope (local proxy for bandwidth sensitivity).** `FABLE2_DEFERRED_CLEARS_DIAG=dup1` encodes each real clear
  pass twice (same image, clears are idempotent). That added 193 / 433 MB per frame at 2× / 3× and moved the clear
  site's marginal time by +0.19 / +0.41 ms. Total busy moved by ~0 / +0.11 ms, because duplicated passes overlap other
  work. Together with the on-vs-off (−0.36 ms for 295 MB at 3×) and skip runs, these passes cost about **0.8–1.2 ms per
  GB** on this machine, or 0.45–0.65 of a pure ~550 GB/s bandwidth model.
- **M1-class estimate (≈68 GB/s, pure model 14.7 ms per GB):**

  | Scale | Upper bound | Scaled by the measured/model ratio |
  |---|---|---|
  | 1× | 0.5 ms/frame | — |
  | 2× | 1.9 ms/frame | ≈0.5–1.3 ms |
  | 3× | 4.3 ms/frame | ≈1.1–2.8 ms |

  The scaled column uses 0.25–0.65, the §13 range widened with this measurement. On a bandwidth-starved GPU less of the
  traffic overlaps other work, so the upper half of the range is the more likely one. This is an estimate, not measured
  on an M1.

**Correctness:**

- Parity (`perf-pass-20261011/parity.py`, 4×4 linear-light regions, `parity.txt`), idle phase:

  | Scale | on vs off, median / max | same-build off vs off, median / max |
  |---|---|---|
  | 1× | 0.25% / 3.26% and 0.21% / 0.90% | 0.04% / 2.98% |
  | 2× | 0.37% / 2.12% and 0.24% / 1.33% | 0.21% / 3.36% |

  On vs off sits inside the off-vs-off noise floor. idle2 is noisy in every pairing, off vs off included (14–193% max).
- Captures checked by eye at 2× (`cmp/`): dog fur, green interact glow, magenta orb bloom, torch glow, floor and
  character shadows, HUD and subtitles all match. The pause-menu render-to-texture preview at 2× is correct.
- Other runs:
  - sweep 1×/2×, area, boot, F5 (saved, `QC060`/Thag's Cave);
  - bootloop 3/3 pass;
  - all perf runs: 0 AV.

### Transfer quads and the rasterizer state (2026-10-11, bug-patterns row 49)

Branch `fix/transfer-quad-raster-state`. Evidence: `out/validation/raster-state-20261011/` (`xq.txt`, `parity.txt`,
`cmp/`, `sessions/`).

- **Gap.** The deferred clears reset cull, winding, fill, depth bias and depth clip before their quad. The ownership-
  transfer quads drawn in the same draw pass did not. They set pipeline, depth/stencil state (every branch), stencil
  reference, viewport and scissor. Blending is off, so the blend colour is unused. Visibility results are not used.
  A reused encoder would therefore pass the last guest draw's rasterizer state to them. A wrong cull drops the whole quad.
  Depth bias does not matter: depth transfers write `[[depth(any)]]`, and colour/stencil transfers do no depth test.
  Depth clip does not matter either, because the quad is at z = 0.
- **Never hit.** An encoder is reused only when the same attachment textures are bound
  (`IsRenderPassDescriptorCompatibleSlow`). Transfers are queued when the binding moves to a different texture, and K1
  ends the encoder (§13). New trace counters (`tr_n_xq_*`, frame CSV) show this. Across sweep 1×, sweep 2× and area, 582k
  in-pass transfer batches ran: **0** of them came after a guest draw in the same encoder, and **0** inherited a
  non-default cull, fill, bias or clip. No `[raster-state] … inherit` log line appeared in 6 sessions.
- **Fix anyway (default on, `FABLE2_TRANSFER_QUAD_STATE_RESET=0` off).** `ResetDrawPassRasterizerState`, shared with the
  deferred clears, runs before the transfer quads and sets `kDrawPassTransferEncoderMutationRasterizer`, so the next
  guest draw re-applies its own state. The reverse direction was already safe: transfers never set the rasterizer, and
  everything else they change carries a mutation bit.
- **Validation.**
  - Every run: verdict `ok`, 0 AV, no contention, `pgrep -x fable_2` = 1 after close. The first launch after the build
    (sweep 1×) is counted; it took no longer than the others.
  - Correctness runs (fix on): sweep 1×/2×, area, boot, F5 (saved), bootloop 3/3.
  - Parity, idle phase (on / on with trace / off):
    - 1×: median 0.35–0.93%;
    - 2×: median 0.04–0.44%;
    - same-build pairs show the same spread.
    - The 1× outlier max (35–55%) is one near-black corner region (linear 0.003) and appears in same-build pairs too.
  - By eye: dog, green interact glow, torch, HUD and subtitles match.
  - Uncapped 2×: 113.6 FPS on vs 113.9 off.
  - With every inherited state already at the defaults, the reset cannot change pixels today.

### Switches (§14)

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_DEFERRED_CLEARS=0` | on | Kill switch: resolve clears as separate passes at resolve time (today's path) |
| `FABLE2_DEFERRED_CLEARS_SWAP_FLUSH=1` | off | Conservative arm: flush every deferred clear at frame start |
| `FABLE2_DEFERRED_CLEARS_DIAG=skip` | off | **Diagnostic only, unsafe:** drop clear-only resolve clears (upper bound) |
| `FABLE2_DEFERRED_CLEARS_DIAG=dup<N>` | off | Diagnostic: encode each clear pass N extra times (same image; bandwidth slope); forces deferral off |
| `FABLE2_TRANSFER_QUAD_STATE_RESET=0` | on | Draw-pass transfer quads no longer reset cull/winding/fill/depth bias/depth clip (bug-patterns row 49) |

Files (§14): `backends/metal/port/rex/graphics/metal/metal_render_target_cache.cc/.h`, `metal_shared_memory.h`
(counters), `metal_command_processor.cc` (counter records, rasterizer mutation bit, guard re-queue);
`docs/resolution-scale.md`, `docs/bug-patterns.md`, `docs/reference.md`, the 2026-10-11 performance audit. Restore points:
`../restore-points/2026-10-11-deferred-clears-stage{1,3,4}.patch`.

## 15. 3× profile in Bower Lake, direct texture loads (2026-10-11)

Branch `feature/perf-3x` (from `macos` 03b0420). M5 Max, Bower Lake save `hero1-dog-live_2026-10-11_11-20-20`
(outdoor camp, QC075). Harness, scripts and copied pass/frame CSVs: `out/validation/perf3x-20261011/`
(`arm.sh`, `keytail.py` per-key marginal busy time across scales, `holds.py` per-`hold` FPS for the `headings8`
plan, `cpuwin.py` cores per thread, `bootstat.py`, `parity-*.txt`). Every run: verdict `ok`, 0 AV, no contention,
`pgrep -x fable_2` = 1 after close.

### Where the 3× frame goes (baseline)

- Uncapped FPS 1× / 2× / 3×: 116.9 / 79.2 / 48.7 (60-capped: 60 / 60 / 48.7). **3× is GPU-bound**: GPU 20.4 ms/frame,
  busy 99%, GPU Commands waits on a frame slot 46% of the time. 1× is close to both limits (GPU 8.2 ms, busy 94%,
  GPU Commands 0.98 core).
- Bower Lake is far heavier than Old Tomb: 364 encoders and 72 resolves per frame (Old Tomb 191 / 32).
- Fit `t = 6.7 ms + 1.52 ms × scale²`: about a third of the 3× frame does not scale with pixels.

Marginal busy time per pass (`keytail.py`, ms/frame, keys at 1× dimensions):

| Pass | 1× | 2× | 3× | Scales with pixels? |
|---|---|---|---|---|
| Main HDR scene, 2xMSAA RGBA16F + D32S8 (`c0:115/1120x1176x2`), ~300 draws × 4 passes | 2.43 | 2.83 | 3.81 | weakly (draw/vertex bound) |
| `direct_host_resolve` (72 per frame) | 0.55 | 0.85 | 1.51 | yes |
| Texture-upload blit (`tex_upload_blit`) | 0.26 | 0.61 | 1.41 | yes (scaled resolve re-loads) |
| Texture-upload untile compute | 0.62 | 0.83 | 1.28 | yes |
| Depth-only 1xMSAA `1040x2528` (shadow-map style, 4 × ~78 draws) | 0.31 | 0.74 | 1.49 | yes |
| Depth-only 4xMSAA `560x1176x4` | 0.18 | 0.60 | 1.34 | yes |
| Full-screen 2-draw pass `1120x2352` + depth | 0.19 | 0.64 | 1.24 | yes |
| 24 one-draw passes on 320-wide targets | 0.61 | 0.82 | 1.05 | partly (per-pass overhead) |
| Depth-only 4xMSAA `520x1264x4`, 1 draw × 2 | 0.12 | 0.39 | 0.83 | yes |
| 160/280-wide 4xMSAA passes | 0.80 | 1.06 | 1.56 | partly |
| UI (`c0:90/1280x2048`) | 0.08 | 0.24 | 0.51 | yes |
| Resolve clears left after §14 | 0.06 | 0.20 | 0.51 | yes |

### Tall EDRAM-height host render targets: not a GPU cost

Host render targets span the whole EDRAM height × scale (a 320-wide effect target is 960×16368 at 3×). An exact
render-area restriction (`renderTargetWidth/Height` = union of the pass's draw scissors) covered only 24 of 129
draw encoders (the guest scissors are mostly the whole surface) and was FPS-neutral. An unsafe diagnostic clamping
every draw pass to the top 1200×3 rows (1.36 → 0.43 Gpx of render area per frame) moved busy time by −0.12 ms:
untouched tiles cost almost nothing on Apple GPUs. It is a memory-footprint issue only. Removed; patch kept as
`../restore-points/2026-10-11-perf3x-renderarea-experiment.patch` (bug-patterns row 55).

### Fix: direct texture loads (`FABLE2_TEX_LOAD_DIRECT`, default on)

- **Cause.** Every resolve output that is then sampled (74 loads per frame here) is re-loaded as a texture: the
  untile compute writes a pooled staging buffer, then a blit copies it into the texture. At 3× that blit moves
  390 MB/frame (`tr_n_tex_blit_scl_kb`). Diagnostic `FABLE2_TEX_BLIT_DIAG=skip` (unsafe) took busy time −2.25 ms
  (+6.3 FPS); `dup1` (same image) +1.64 ms.
- **Change.** Resolution-scaled 1D/2D/stacked loads with no packed mip tail run a variant of the same XeniOS load
  shader that writes the texture through an unsigned-integer view with the host texel size (R8Uint … RGBA32Uint).
  The variant is `backends/metal/shaders/texture_load_dest_texture.metal`, a wrapper compiled by
  `tools/compile_metal_helpers.py` around each unmodified `texture_load_*_scaled.cs.xesl`: only the destination
  binding and the 16-byte store macro change, so each texel receives the bytes the staging buffer held at the same
  position. Scaled textures are created with `ShaderWrite` usage. Anything else (3D, cube, packed mips, no view)
  keeps the staging path. All 74 loads per frame take the direct path; no scaled blits are left.
- **Exactness.** `FABLE2_TEX_LOAD_DIRECT_VERIFY=1` (diagnostic) also runs the staging shader into a reference buffer
  and copies each written level back after the batch; the completion handler compares them byte for byte:
  515k level loads at 2× and 3×, **0 mismatches** (a first version of the check flagged loads overwritten by a later
  load in the same batch; those are skipped as "superseded"). Parity on vs off is inside the same-build spread
  (2× median 0.07% / max 0.61%; `parity-dl.txt`). By eye: shadows, lamp and fire glow, HUD, dog match.

| (Bower Lake, `headings8` hold mean unless noted) | off (`=0`, same build) | on |
|---|---|---|
| 3× uncapped FPS | 47.7 | 49.6 |
| 3× uncapped FPS, pass-trace run | 47.7 | 51.0 |
| 3× GPU ms/frame / busy ms | 20.39 / 20.09 | 19.02 / 18.64 |
| 3× 60-capped FPS | 48.0 | 49.8 |
| 2× uncapped FPS / busy ms (turn30) | 79.9 / 12.47 | 80.3 / 12.17 |
| CPU cores, 3× uncapped / 60-capped | 1.72 / 1.61 | 1.15 / 1.15 |
| CPU cores, 2× uncapped | 1.95 | 1.58 |

- The blit's 1.4 ms is gone; the compute that now writes the texture costs about +0.5 ms at 3× (+0.4 at 2×), so the
  net GPU saving is 1.4 ms at 3× and 0.3 ms at 2× on this machine.
- **CPU: −0.35 to −0.55 core at 2×/3×.** An unnamed driver thread that ran at ~0.58 core at 3× (0.1 at 1×)
  disappears with the blits.
- **Bandwidth:** about 770 MB/frame less at 3× (blit read + write), 340 MB at 2×. M1-class estimate (≈68 GB/s,
  ratio 0.25–0.65 of the pure model as in §14): −2.8 to −7 ms at 3×, −1.3 to −3.3 ms at 2×. Not measured on an M1.
- Validation: sweeps 1×/2×/3× (`spin5`), area, boot, F5 (saved), bootloop 3/3; all 0 AV.

### Other results on this branch

- **Startup gap** (perf-audit §8): the 0.6–1.1 s between "Guest memory arena mapped" and the GPU Commands thread was
  the SDK's startup VFS walk of the game root, which is the whole checkout in dev sessions (179k entries, 162k
  under `out/`). `patches/sdk/vfs-skip-dev-dirs.patch` (`FABLE2_VFS_SKIP_DEV_DIRS`, default on) does not walk the
  top-level `out/`, `.git`, `thirdparty`; opens below them resolve lazily and the lazy lookup now walks each
  directory it adds (bug-patterns row 53: the first version broke skip intro). Gap 0.66–0.93 → 0.017 s; boot to
  gameplay 5.16–5.40 → 4.49–4.56 s (first launch after the build included).
- **`-mcpu=apple-m1`** instead of `-march=armv8-a` for the game, generated code, SDK and Metal plugin
  (`FABLE2_CPU_TARGET=generic` at build time reverts): neutral, as the audit predicted (clang already tuned for
  apple-m1; only feature macros change). 1× uncapped 113.7 / 116.2 vs 114.5 / 113.6 FPS, 2.19–2.20 cores both,
  60-capped 1.31 vs 1.33 cores; parity in the noise floor. Full rebuild 144 s.

## 16. Shadow maps at native 1× (2026-10-11)

Branch `shadows-1x` (agent slot, from `macos` d1b2fcc). M5 Max, Bower Lake save `hero1-dog-live_2026-10-11_11-20-20`.
Harness, scripts, logs, crops and copied pass/frame CSVs: `out/validation/shadows1x-20261011/` (`sp.sh` spin5
captures, `perf.sh` / `cleanperf.sh` headings8 + pass trace, `drift.py` linear-light drift, `cmp.py` crop sheets,
`keytail.py`, `trace3-passes.txt` pass graph). Every run: verdict `ok`, 0 AV, `pgrep -x fable_2` = 1 for our pids.

### What the depth-only passes are (diagnostic `FABLE2_SHADOW_TRACE=<frame>`)

The trace logs, for two frames, every draw (bound targets, shaders, depth control, scissor, viewport, fetched
textures), every resolve (source owners, destination, format) and every ownership transfer.

- **Shadow maps:** `RT @ 0t <13t> 1xMSAA kD24FS8` (the 1040-wide depth-only key of §15, 4 encoders, 200 + 240
  draws, two cascades). Each cascade starts with a z-always clear quad drawn on the 1× and then the 4×MSAA variant
  of the same tiles (the 520-wide 4×MSAA "1 draw × 2" key). It ends with a depth resolve (`k_24_8_FLOAT`, 1024×1024)
  to 0x12BE1000 / 0x12FE1000. Both are **sampled as textures** by the main forward shaders, the screen-space
  shadow-mask pass (the 1120-wide "full-screen 2-draw" key, an 8-bit 1120×720 resolve), a small 1t pass and an
  occlusion-query depth pass. EDRAM aliasing touches them only as tile reuse: the next owner clears the tiles.
- **Not shadows:** the 560-wide 4×MSAA `<14t>` depth/stencil passes alias the main scene depth (`1008t`, 2× ↔ 4×
  MSAA) and stencil. They are a scene prepass and stay scaled; 1× there would upsample the scene depth.

### Change (`FABLE2_SHADOWS_NATIVE`, default on; `=0` = today's scaled shadows)

Built on the removed effect-targets experiment (§12, `../restore-points/2026-10-10-effects-native-targets-experiment.patch`),
re-applied and generalised:

- SDK, `backends/metal/sdk-metal-native-scale.patch` (replaces `sdk-metal-effects-native.patch`; applied after
  `sdk-metal.patch`): `native_scale` key bit; a `NativeScaleRenderTargets(pitch, msaa, used_rts_bits, edram_bases)`
  hook in `RenderTargetCache::Update` that returns the targets of a pass kept at 1× (all or none); the resolve-clear
  owner check for depth and colour; `MarkRangeAsResolvedUnscaled`; the translator's `render_target_native_scale`
  pixel-position bit.
- Rule (Metal): a depth target (EDRAM base + pitch) becomes a shadow map once it is the source of a **square depth
  resolve of at least 512×512**. From then on, depth-only passes on it run at 1× (both MSAA variants). A target
  ever drawn together with colour is scene depth and is never native. Log: `[native-scale] shadow map: depth target
  @ 0t <13t> … drawn at native 1x from now on` (once, on the first frame of gameplay).
- 1× passes: per-target texture size, viewport, scissor, point size and depth-bias scale; the dummy colour target
  follows the depth texture. Resolves of native targets run exactly as at 1× (unscaled copy constants, pipelines,
  dump layout) into shared memory, and the texture cache drops the scaled marking of the range, so the shadow
  texture is the console's 1024×1024. Mixed-scale ownership transfers map pixels through each side's scale (§12
  transfer MSL).
- **Host-depth sources on mixed-scale depth transfers are dropped** (`tr_n_sh_hostdepth_drop`, 11–12 per frame).
  The EDRAM host-depth copy and the transfer shaders address one scale, so such a transfer reads its source's
  guest-format (24-bit) depth instead of the 32-bit host depth. The trace shows that every affected destination
  is overwritten before anything reads it. These are the shadow target itself (9 per frame), the main depth's
  4×MSAA variant and the two quarter/half-resolution occlusion depth targets. In each case the next draw on the
  destination is a z-always, depth-write clear quad over its whole viewport (`zc=00008777`, full scissor). Only
  tiles outside every later viewport and scissor keep the 24-bit value, and those hold stale aliasing data anyway.
- The effect-target experiment is still in the tree, opt-in and unchanged (`FABLE2_EFFECTS_NATIVE_TARGETS=1`). It
  runs (verdict ok, 0 AV), but the §7.2 box-averaged copy of scaled textures for native passes is **not built**. It
  needs an unscaled variant of every scaled-resolve texture a native pass fetches, which is not a cheap change.

Counters per frame at 3× (`tr_n_sh_draw`, `tr_n_eff_rsv_native`, `tr_n_eff_xfer_scaled`): 314 native shadow draws,
2 native depth resolves, 24 mixed-scale transfers. At 1× nothing changes (the hook returns 0 when unscaled).

### Results (headings8 uncapped, hold mean; GPU from the pass trace)

| Scale | Arm | FPS | GPU ms mean / p50 / p95 | Note |
|---|---|---|---|---|
| 2× | off (`=0`) | 77.7 | 12.47 / 12.55 / 14.47 | clean |
| 2× | on | 84.5 | 11.42 / 11.23 / 13.73 | clean |
| 3× | off | 47.9 / 48.8 | 20.03 / 20.16 / 22.59 | another slot's game ran during the run |
| 3× | on | 55.4 | 17.54 / 17.57 / 19.99 | clean |
| 3× | holds 0–3 only (uncontended in every run) | off 50.4, on 57.7 | | +14% |

- Marginal busy time at 3× (`keytail.py`): the scaled shadow draw (1.71 ms) and 4× clear (0.96 ms) passes become
  0.46 + 0.13 ms at 1×. `tex_upload_compute` −0.42 ms, `direct_host_resolve` −0.2 ms. Busy 20.9 → 17.4 ms.
- Bytes (touched-rect model, §13): per cascade at 3× about 0.63 GB/frame (clears, 1×↔4× transfers, two draw
  encoders, the depth resolve and the texture untile, D32S8 at 5 B/sample), so about 1.1 GB/frame less at 3× and
  0.4 GB at 2×. **M1-class estimate** (≈68 GB/s, measured/model ratio 0.25–0.65 as in §13–§14): −4 to −10 ms at 3×,
  −1.6 to −4 ms at 2×, before the 9×/4× fewer rasterized shadow fragments. This is an estimate, not measured on an M1.

### Drift (`drift.py`: box-averaged to 1280×720 in linear light, mean |Δ| of linear RGB in % of full scale)

| Region (1280×720 units) | off vs off (noise) | 3× on vs off | 1× vs 3× off | 1× vs 3× on | 2× on vs off |
|---|---|---|---|---|---|
| Whole frame (idle) | 0.25% | 0.20–0.24% | 0.51% | 0.50% | 0.38% |
| Ground under hero, shadow edge (520,480,280,180) | 0.08% | 0.07% | 0.16% | 0.17% | 0.07% |
| Fence and grass, tree shadows (160,300,300,170) | 0.33% | 0.16% | 0.69% | 0.70% | 0.47% |

The difference is below the run-to-run noise of the animated scene: the game filters its shadows (screen-space
mask, soft edges), so 1024² shadow texels at 3× look the same as the 3072² scaled ones. By eye
(`crops/feet-idle3.png`, `fence-idle3.png`, `treeshadow-idle3.png`, `feet2x-idle3.png`: 1× | scaled | native): no
visible change. Dog fur, the breadcrumb-trail glow, fire bloom, the HUD and the Cullis Gate area run are unchanged
(`dog-idle3.png`, `fire-idle3.png`, `area3-*.png`).

**Recovering authenticity / today's look:** native 1× shadow maps are the console's own resolution, so this is
the faithful setting. `FABLE2_SHADOWS_NATIVE=0` restores the scaled (sharper-than-console) shadow maps exactly as
before this change.

Validation (3×): spin5 sweeps at 1×/2×/3× on and off, area, boot, F5 (saved), bootloop 3/3; skip intro ok. All
0 AV.

### Switches (§16)

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_SHADOWS_NATIVE=0` | on | Shadow maps at the scaled resolution again (today's 2×/3× image) |
| `FABLE2_EFFECTS_NATIVE_TARGETS=1` | off | Experimental effect targets at 1× (§12; glow issue open) |
| `FABLE2_SHADOW_TRACE=<frame>` | off | Diagnostic: pass / resolve / transfer graph for two frames |

New per-frame counters: `tr_n_sh_draw`, `tr_n_sh_hostdepth_drop`, `tr_n_eff_xfer_scaled`, `tr_n_eff_rsv_native`,
`tr_n_eff_rsv_unified`.

Files (§16): `backends/metal/sdk-metal-native-scale.patch` (new), `tools/build_macos_metal.py` (patch list),
`backends/metal/port/rex/graphics/metal/metal_render_target_cache.cc/.h`, `metal_command_processor.cc`,
`metal_direct_host_resolve.cc`, `metal_shared_memory.h`; `docs/resolution-scale.md`, `docs/bug-patterns.md`.
Restore points: `../restore-points/2026-10-11-shadows1x-{trace,stage1,stage2}.patch` (+ `stage2-untracked/`).

## 17. Resolve straight into the sampled texture (2026-10-11)

Branch `resolve-into-texture` (agent slot, from `macos` d1b2fcc, with shadows-1x and bet-a merged in). M5 Max, Bower Lake
save `hero1-dog-live_2026-10-11_11-20-20`. Harness, scripts and copied CSVs: `out/validation/rtt-20261011/`
(`arm.sh`, `clean.sh` = perf run only when no other game runs, `keytail.py`, `holds.py`, `parity.py`, `parity.txt`,
`hang-20261011-165551-sample.txt`). Engine-options rank 3 (4.2a).

### Census (stage 1, `FABLE2_RTT_DIAG=census`, before shadows-1x)

At 3×: 72 copy resolves and 74 scaled texture loads per frame, 388 MB of loads per frame. Loads are grouped by the
resolve that last wrote the texture's base level:

| Class | Loads / frame | MB / frame (3×) | What |
|---|---|---|---|
| 1 plain-copy match (8888, 2_10_10_10, 16_16_16_16_FLOAT, k_8; same base/pitch/bpb, tiled 2D) | 19.7 | 91 | resolve-writable |
| 2 depth match (k_24_8_FLOAT; shadow maps, scene depth) | 3 | 103 | resolve-writable (depth conversion) |
| 3 other conversion (k_1_5_5_5 with mips resolved per level) | 11 | 13 | not done |
| 4 match, texture stale and resolved in two halves (fp16 1120×720) | 1 | 58 | handled once textures stay valid |
| 5 aliasing (another format/size over the same memory) | 8 | 9 | must reload |
| 0 no new data: neighbour or memexport invalidation | 4.4 | 105 | redundant (below) |

- **Class 0 was a redundant re-untile.** Watches fire for whole 16 KB host pages. A resolve into the neighbouring
  range, or a memexport (which never touches the scaled buffer), invalidated the texture, and it re-loaded the same
  bytes.
- Upper bound (`FABLE2_RTT_DIAG=skip`, unsafe: classes 1–2 marked up to date without a write): GPU 19.09 → 18.48 ms
  at 3×, 12.14 → 11.71 ms at 2×.

### Change (`FABLE2_RESOLVE_TO_TEXTURE`, default on)

- **Keep (`FABLE2_RTT_KEEP`).**
  - Each scaled texture carries `rtt_valid_`: its base level holds exactly what a load from the scaled-resolve buffer
    would write at this point in GPU order.
  - A global shared-memory watch clears it only when bytes that can change a load are written:
    - the exact resolve extent, rounded out to 256 B (a multiple of every scaled-addressing group);
    - CPU writes;
    - whole-memory invalidations.
  - Memexport writes leave it set.
  - A load of a valid texture is skipped; the SDK then marks it up to date as usual.
- **Fused (`FABLE2_RTT_FUSED`).**
  - Wrapper variants of the scaled host-resolve shaders (`backends/metal/shaders/resolve_dest_texture.metal`, 33
    metallibs from the unmodified XeniOS entries; colour fast 32/64 bpp, full 8/32/64 bpp, depth, all MSAA, uint/float)
    store the scaled buffer exactly as before.
  - They also write up to two cached scaled textures whose layout matches: same base (or a granular base origin found
    by `Tiled2D`), pitch and bpb, tiled 2D, no mip overlap, texture valid before or fully covered.
  - Each N-byte store goes to the texture as N/bpb texels at the same host pixels. The texels get the 32-bit endian
    swap and the depth transform that `texture_load_*_scaled` applies to the same bytes.
  - Those textures stay valid, so their next load is skipped. The dump (EDRAM buffer) resolve path is not fused.
- The scaled-resolve buffer is still written by every resolve. Morph readback, aliasing textures of other formats
  and every other consumer see the same bytes as before. Nothing reads guest RAM differently.
- **Pre-existing bug found by the verify mode, fixed (`FABLE2_SCALED_GROW_ORDER_FIX`, default on).**
  - When a scaled-buffer grow exceeded the retired-buffer budget, the old → new copy ran in a standalone command buffer
    ahead of the open one.
  - Resolves already encoded in the open command buffer were then missing from the new buffer. Textures loaded
    afterwards showed stale content, e.g. after area loads.
  - The copy now stays in order on the current command buffer whenever a submission is open. The old buffers are
    retired past the budget until it completes.

### Exactness

- `FABLE2_RTT_VERIFY=1` (diagnostic) works at every skipped load (kept or fused). It runs the reference untile of the
  base level into a buffer instead of writing the texture, reads the texture back after the batch, and compares them
  byte for byte. Log: `[rtt] verify: N … checked, M mismatched`.
- Result: **0 mismatches in about 600k checks** at 2× and 3× (spin5, headings8, area and boot runs), including after
  both merges.
- Negative control, `FABLE2_RTT_VERIFY_BREAK=1` (fused write shifted by one texel): 11,262 of 17,920 mismatched.
- Before the grow fix, area runs showed 29–67 mismatches right after a non-retained grow: the bug above, where keep
  held the correct data and a re-load would have read the stale buffer.
- Parity (`parity.txt`, 4×4 linear-light regions): on vs off sits inside the on vs on spread. 3× idle, combined code:
  median 0.12% / max 0.93%. By eye at 3×: dog fur, fire and breadcrumb glow, shadows and HUD are identical.

### Results (headings8 uncapped, hold mean; GPU mean from the pass trace; combined code with shadows-1x and bet-a)

| | off (`FABLE2_RESOLVE_TO_TEXTURE=0`, same build) | on |
|---|---|---|
| 3× FPS (2 runs each) | 57.0 / 57.1 | 58.4 / 58.0 |
| 3× GPU ms/frame | 17.02 / 17.00 | 16.57 / 16.72 |
| 3× scaled-load MB/frame (`tr_n_tex_dl_kb`) | 241 | 34 |
| 2× FPS | 84.1 | 85.4 |
| 2× GPU ms/frame | 11.40 | 11.28 |
| 2× scaled-load MB/frame | 106 | 15 |

- Per pass at 3× (`keytail.py`): `tex_upload_compute` 1.39 → 0.76 ms; `direct_host_resolve` 1.38 → 1.80 ms, since
  the texture writes now happen there.
- Before shadows-1x the same change gave 19.08 → 18.52 ms at 3× (388 → 66 MB of loads).
- **Bytes:**
  - −206 MB of load reads and −206 MB of load writes per frame at 3×, +241 MB of fused writes: net −171 MB.
  - At 2× the net is −76 MB.
  - About 69 MB of fused writes per frame at 3× are invalidated before any sample (`tr_n_rtt_waste_kb`: aliasing
    8888/2_10_10_10 pairs and the first half of the two-part resolve). An adaptive skip of them measured no gain, so it
    was removed.
- **M1 estimate:** 171 MB at about 68 GB/s is 2.5 ms of pure bandwidth at 3×. Using a 0.25–1.0 model-to-time ratio
  (about 1.0 measured here), that is about 0.6–2.5 ms at 3× and 0.3–1.1 ms at 2×. Not measured on an M1.
- Below the 1.3–1.8 ms first estimate for two reasons: shadows-1x already removed the biggest depth loads, and the
  fused texture writes cost what the loads' writes did. Only the buffer read and the dispatch are saved.
- **Next step, not built:** stop writing the scaled buffer for ranges whose only consumers are fused textures
  (lazy buffer). That is about 241 MB/frame more at 3×. It needs proof that no alias, morph readback, mip load or grow
  copy reads the range. High risk.

### Open issue

- One unclean exit in about 60 sessions on this branch (`rtt-ad-h2g`, 2×). The close request hung: guest threads were
  suspended, and the main thread was blocked in `-[CAMetalLayer nextDrawable]`.
  - The `sample` is saved in the validation folder. The process (own session, pid-verified) had to be terminated.
  - Three repeats of the same arm passed, and no GPU error was logged.
  - The cause was not found. It may be unrelated (first unclean exit in the shared agent metrics, 1 of 169).

### Switches (§17)

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_RESOLVE_TO_TEXTURE=0` | on | Master switch for keep + fused |
| `FABLE2_RTT_KEEP=0` / `FABLE2_RTT_FUSED=0` | on | Disable one half |
| `FABLE2_SCALED_GROW_ORDER_FIX=0` | on | Old (standalone, out-of-order) scaled-buffer grow copy |
| `FABLE2_RTT_VERIFY=1` | off | Diagnostic: reference untile at every skipped load, byte compare |
| `FABLE2_RTT_VERIFY_BREAK=1` | off | Negative control for the verify (shifted fused write; image wrong) |
| `FABLE2_RTT_DIAG=census\|skip` | off | Census log / unsafe upper-bound skip |
| `FABLE2_RTT_TRACE_BASE=0x…` | off | Event trace (resolves, watches, loads, keeps) for one texture base |
| `FABLE2_RTT_RESOLVE_MARGIN=N` | 0 | Diagnostic: widen the exact resolve range used for keep |

Counters (frame CSV, `f2diag gpu`): `tr_n_rtt_rsv`, `tr_n_rtt_direct(_kb)`, `tr_n_rtt_keep(_kb)`,
`tr_n_rtt_waste_kb`. The census also fills `tr_n_rtt_t1..5` and `tr_n_rtt_ld0..5(_kb)`.

Files (§17): `backends/metal/port/rex/graphics/metal/metal_resolve_to_texture.cc` (new),
`metal_texture_cache.cc/.h`, `metal_direct_host_resolve.cc`, `metal_render_target_cache.cc`,
`metal_shared_memory.h`, `metal_command_processor.cc`, `backends/metal/metal-sources.cmake`,
`backends/metal/shaders/resolve_dest_texture.metal` (new), `tools/compile_metal_helpers.py`. Restore points:
`../restore-points/2026-10-11-resolve2tex-{census,fused}.patch` (+ folder with the new shader).

## 18. Cheaper texture writes and resolves (2026-10-11)

Branch `cheaper-tex-resolve` (agent slot, from `perf-staging`). M5 Max, Bower Lake save
`hero1-dog-live_2026-10-11_11-20-20`. Harness, scripts and copied CSVs: `out/validation/texres-20261011/`
(`arm.sh`, `clean.sh`, `cmp.sh`, `keytail.py --keys`, `holds.py`, `ctr.py`, `parity.py`). Every perf run: verdict `ok`,
0 AV, no contention (contended attempts were retried).

### Per-resolve view (trace only)

`direct_host_resolve` encoders now carry a pass-trace key: `c<color fmt>|d<depth fmt> w<W>h<H>m<MSAA> <shader> f<fused
targets> r<source rects>` (guest size, so keys line up across scales; `keytail.py --keys`). `FABLE2_TEXRES_DIAG=<frame>`
logs every load and resolve of that frame and the next (format, size, levels, dispatches, base/mips).

At 3× the 72 resolves split into four big ones (~1.06 ms: `c3 w1120h576m2 full64` 0.41, `d1 w1120h720m2 fast32 r2` 0.29,
`c2 w1280h720m1 full32` 0.23, `c0 w1120h720m1 full8` 0.13) and **12 mip-chain loops per frame**: a 320-wide effect target
is resolved into level 0 of a `k_1_5_5_5` 256×256 texture with 4 levels, then 128/64/32 into levels 1–3, each followed by
a draw that samples the previous level. Before this change each level resolve was followed by a reload of the base or of
the whole mips part (45 scaled loads per frame, `tex_upload_compute` 0.85 ms busy): resolve 15 µs → load 11–20 µs → draw →
… — latency-bound, 115 µs per chain.

### Change: fused writes into 16 bpb textures and whole mip levels (`FABLE2_RTT_16BPP`, `FABLE2_RTT_MIPS`, default on)

- `resolve_dest_texture.metal`: 16 bpb targets (16-bit endian swap, then the `R5G5B5A1→B5G5R5A1` / `R5G6B5→B5G6R5`
  transform the load applies), and a mip `level` per target. New `resolve_host_color_full[_uint]_16bpp_*_scaled_rtt_cs`
  variants (6 metallibs).
- `RttCollect` class 6: a resolve whose destination is exactly one stored, non-packed mip level (origin 0, same pitch and
  bpb, rect covering the level, extent rounded to 256 B inside the level's guest range).
- `rtt_mips_valid_` per texture: set by a mips load, cleared by any write over the mips range except the fused resolve's
  own write inside the level it rewrote whole (GPU write, range check). `RttTryKeep` skips the mips part (or a mips-only
  load) while it is valid.
- Exactness: `FABLE2_RTT_VERIFY=1` now also re-loads kept mips and compares every level: **0 mismatches** in 208k checks at
  3× and 418k at 2×.

| Bower Lake, headings8 uncapped, 1 run per arm | off (`FABLE2_RTT_MIPS=0 FABLE2_RTT_16BPP=0`, same build) | on |
|---|---|---|
| 3× FPS (hold mean) / GPU ms | 58.1 / 16.71 | 58.4 / 16.63 |
| 2× FPS / GPU ms | 84.8 / 11.39 | 85.1 / 11.32 |
| scaled loads per frame (`tr_n_tex_dl`), 3× | 45.2 (34 MB) | 0.7 (5 MB) |
| fused mip-level writes / kept mips per frame | 0 / 0 | 32.9 / 40.8 |
| mip chain, first resolve to last resolve (median) | 115 µs | 80 µs |
| `tex_upload_compute` / `direct_host_resolve` busy, 3× | 0.85 / 1.82 ms | 0.21 / 1.99 ms |
| CPU (process), 3× / 2× uncapped | 1.27 / 1.77 cores | 1.25 / 1.67 cores |

- The chains get 35 µs shorter (−0.4 ms of encoder time per frame), but the command-buffer GPU time drops only ~0.08 ms:
  most of the chain overlaps other GPU work, so a shorter chain mostly frees overlap. Small, exact, kept.
- Bytes at 3×: −29 MB of load reads and −29 MB of load writes, +19 MB of fused writes per frame: net about −39 MB
  (2×: about −17 MB). **M1-class estimate** (≈68 GB/s, ratio 0.25–1.0 as in §17): −0.15 to −0.6 ms at 3×, −0.06 to
  −0.25 ms at 2×, plus 44 fewer dispatches and encoders per frame. Not measured on an M1.

### Measured and not adopted

- **Resolve threadgroup shape** (`FABLE2_RESOLVE_TG=WxH`, default 8×8; the resolve shaders use only global thread IDs, so
  any shape is exact): 16×4 2.05, 4×16 2.05, 32×2 2.63 ms of `direct_host_resolve` busy vs 8×8 2.00. Kept as a switch.
  `FABLE2_TEX_LOAD_TG=WxH` does the same for `texture_load_*` (default 4×32); with the loads gone at 2×/3× it was not
  measured.
- **Lazy scaled buffer** (§17 "next step"). Census `FABLE2_LAZY_CENSUS=1` (`tr_n_lz_*`): at 3×, 135 MB/frame of the
  200 MB of resolve writes go to ranges whose fused target 0 holds every resolved pixel through an invertible load;
  99.99% of them are overwritten by the next resolve unread (9 KB/frame read by loads, 0 by morph readback; 2×: 60 of
  89 MB). Upper bound `FABLE2_LAZY_SKIP_DIAG=1` (**unsafe**: those resolves do not store the buffer at all): 3× GPU
  16.63 → 16.79 ms, `direct_host_resolve` 1.99 → 1.98 ms; 2× 11.32 → 11.27 ms — no measurable change **on the M5 Max**,
  whose buffer writes overlap the texture-unit-bound resolve. Not built here: it needs texture → buffer materialization
  at every reader, at texture eviction and before partial overwrites (high risk). **Still a candidate for
  bandwidth-limited Macs**: 135 MB/frame is about 2 ms of pure bandwidth at ≈68 GB/s. Its value there must be measured
  (real GPU counters under heavy-tool mode, or a `dup<N>`-style bandwidth-slope proxy) before deciding.
- **Direct-write loads at 1×** (the §15 compute-writes-texture variant for unscaled loads, plus shader-write usage on
  non-BC textures): verify 0 mismatches in 520k levels, but GPU 8.29–8.33 → 8.69–8.80 ms (−3% FPS, 3 runs per arm) and
  only −0.05 core at 60 FPS. Shader-write usage alone costs nothing (8.23 ms); a typed write through the texture's own
  format (RGBA8 / RGB10A2, exact) costs the same as the uint view (8.80 ms), so the "raw uint view" is not the penalty:
  per-texel compute texture writes cost more than a buffer write plus blit on this GPU when the loads are small.
  Removed; patch `../restore-points/2026-10-11-texresolve-direct1x-typed-experiment.patch`. **M5-specific**: the balance
  can differ on M1-class GPUs and was not measured there.
- Encoder merges around resolves: compute/blit encoders adjacent to a resolve are 2–3 per frame each; not worth a change.

### Validation (on, default build)

- spin5 sweeps at 1× / 2× / 3×, area, boot, F5 (saved), bootloop 3/3: all verdict `ok`, 0 AV.
- Verify (`FABLE2_RTT_VERIFY=1`, kept base and mips levels compared with a reference load): 3× 208k checks, 2× 418k
  checks, 0 mismatches.
- Parity at 3× (spin5, `parity.py`, 4×4 linear-light regions): on vs off idle median 0.17% / max 1.90%, idle2 5.1% / 30%;
  on vs on (noise) idle 0.26% / 1.63%, idle2 8.4% / 37% (idle2 is animated). On vs off sits inside the noise.
- By eye at 3× (idle capture, on vs off): dog fur, breadcrumb glow, fire, shadows and HUD are the same.

### Switches (§18)

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_RTT_MIPS=0` | on | No fused mip-level targets; mips reloaded after any write (before this change) |
| `FABLE2_RTT_16BPP=0` | on | No fused 16 bpb targets |
| `FABLE2_RESOLVE_TG=WxH` | 8x8 | Resolve threadgroup shape (tuning) |
| `FABLE2_TEX_LOAD_TG=WxH` | 4x32 | `texture_load_*` threadgroup shape (powers of two; tuning) |
| `FABLE2_TEXRES_DIAG=<frame>` | off | Diagnostic: log loads and resolves of two frames |
| `FABLE2_LAZY_CENSUS=1` | off | Diagnostic: lazy scaled-buffer census (`tr_n_lz_*`) |
| `FABLE2_LAZY_SKIP_DIAG=1` | off | **Unsafe** upper bound: candidate resolves skip the scaled-buffer store |

New counters: `tr_n_rtt_mip_direct`, `tr_n_rtt_mip_keep`, `tr_n_lz_{cand,dead,read,morph,all}_kb` (census only).

Files (§18): `backends/metal/shaders/resolve_dest_texture.metal`, `tools/compile_metal_helpers.py`,
`backends/metal/port/rex/graphics/metal/metal_resolve_to_texture.cc`, `metal_texture_cache.cc/.h`,
`metal_direct_host_resolve.cc`, `metal_pass_trace.cc/.h`, `metal_shared_memory.h`, `metal_command_processor.cc`
(counter lines, census read hook). Restore points: `../restore-points/2026-10-11-texresolve-{mips16,direct1x-typed-experiment}.patch`.

## 19. Per-pass overhead of the small effect passes (2026-10-11)

Branch `tiny-effect-passes` (agent slot, from `perf-staging` 5dc4870). M5 Max, Bower Lake save
`hero1-dog-live_2026-10-11_11-20-20`. Harness and scripts: `out/validation/tinypasses-20261011/` (`arm.sh`, `clean.sh`,
`seams.py` (why each render pass ends and what follows it), `runs.py` (adjacent non-render encoders), `idle.py` (GPU idle
gaps inside a frame by encoder pair), `cpf.py` (GPU Commands CPU ms per frame over the headings8 window), `samp.sh` +
`incl.py` / `sub.py` (`sample` call-tree summaries), `strace/graph.txt` (pass graph, `FABLE2_SHADOW_TRACE`)). Every run:
verdict `ok`, 0 AV, no contention on perf runs, skip intro ok.

### Where the per-pass cost goes

| Per frame, headings8 | 1× | 3× |
|---|---|---|
| Encoders (render / compute / blit) | 367 (134 / 148 / 85) | 279 (135 / 99 / 45) |
| Render passes with ≤ 1 draw | 102 | 103 |
| Pass ends at a resolve (compute must run outside the pass) | 71.5 | 72.6 |
| Pass ends because the next draw binds other attachments | 31.9 | 31.2 |
| Pass ends and the next pass binds the same attachments with nothing in between | 0.2 | 1.6 |
| GPU gap between adjacent encoders, median | ~0 µs | ~0 µs |
| GPU idle inside the frame (all gaps < 1 ms) | 0.48 ms | 0.66 ms |

- **Render pass begin/end.** Nearly every pass boundary is forced: a resolve is a compute dispatch, and the next pass
  binds other attachments. Only 1.6 passes per frame at 3× (0.2 at 1×) close and reopen on the same target with nothing
  encoded in between. A texture flush ends the pass, then the load is kept (resolve-to-texture) and nothing is encoded.
  Merging those passes is worth at most a few µs.
- **Bubbles.** At 3× the idle time is in pass→pass (0.30 ms, ~6 µs × 50) and resolve→pass (0.18 ms) transitions. They are
  real dependencies: each effect pass samples the previous resolve, or takes EDRAM ownership of the previous pass's tiles.
- **The 4×MSAA one-draw passes** (12.7 per frame, 0.83 ms marginal busy at 3×) are the game's EDRAM clear sequence, from
  the pass graph:
  1. a z-always clear quad on the 1× target;
  2. ownership transfers 1× → 4×;
  3. the same clear quad on the 4×MSAA alias of the same tiles;
  4. transfers 4× → 1×;
  5. the textured draws, a resolve, then the mip chain.

  Skipping a transfer or a load needs proof that the clear quad covers its whole rectangle. That needs the quad's
  geometry, not just its state, so it is not exact or contained (as §11 lever 4 found).
- **Load/store actions.** These passes load and store real EDRAM contents. The colour attachment of depth-only passes is
  already a transient DontCare dummy.
- **CPU (`sample` of GPU Commands, 1×).** At 1× uncapped the GPU Commands thread is the limit: 0.95–0.99 core,
  8.2–8.8 ms/frame. Encoder creation inside the driver costs about 11% of it: render passes ~6%, compute ~3%, blit ~2%,
  plus ~2% for the blit's buffer → texture copy command. Our own per-encoder setup costs ~3%:
  - debug labels: an NSString per encoder and command buffer, plus a formatted debug group per resolve (~1%);
  - the attachment heap walk over 8 colour slots (~1%);
  - `NoteTexturesSampledByCurrentCommandBuffer` (~1%).
- **Compute encoders that could be shared:** a resolve followed directly by the texture load that reads its output.
  That is 46 per frame at 1× and 2 at 3×. Merging them would save about 1% of GPU Commands at 1×. It needs the resolve's
  encoder to stay open across the texture cache's encoder protocol and its shared-memory fence, which is a crash and
  hazard risk (bug-patterns row 32). Not built.

### Change: per-encoder CPU trims (`FABLE2_ENCODER_TRIM`, default on)

- Debug labels and debug groups on per-frame encoders and command buffers are skipped. That covers render passes, resolves,
  texture loads, shared-memory uploads, transfer and depth-store encoders, and the main and standalone command buffers.
  `FABLE2_ENCODER_LABELS=1` or `MTL_CAPTURE_ENABLED=1` keeps them for GPU captures. Nothing in the backend reads an encoder
  label.
- The attachment-heap walk checks colour slots 0–3, the only ones the render-target cache binds.
- Exact: no GPU command changes.

| 1× uncapped, headings8 | off (`=0`, same build), 2 runs | on, 2 runs |
|---|---|---|
| GPU Commands ms/frame | 8.39 / 8.21 | 8.18 / 8.19 |
| Process ms/frame | 18.40 / 18.30 | 18.24 / 18.30 |
| FPS, hold mean | 112.2 / 114.3 | 114.4 / 114.0 |

About −0.1 ms/frame (≈1%) of GPU Commands, inside the run-to-run spread. 3× (on): 58.2 FPS, GPU 16.69 ms/frame
(staging before the change: 57.7, 16.71). Parity at 3× idle, on vs staging: median 1.11% / max 9.6%. Same-build noise:
1.12% / 8.6%. By eye: dog, breadcrumb glow, fire, HUD unchanged.

### Conclusion

On this machine the effect passes are at their structural floor: under 0.1 ms of GPU time and about 1–2% of GPU
Commands CPU can be removed exactly in a contained way. Recommendations:

- **Resolve-to-texture at 1× (not built, L).** This is the 1× lever these traces point to. At 1×, every resolve output a
  later draw samples is reloaded by an untile compute plus a blit:
  - 66 of each per frame (132 encoders);
  - 0.88 ms of GPU busy (`tex_upload_compute` 0.63 + `tex_upload_blit` 0.25);
  - about 12–14% of the GPU Commands thread (texture materialization plus their encoders and blit copies).

  §17's fused resolve writes exist only for the scaled path. A 1× variant would need unscaled wrapper shaders and the
  keep/fused logic on shared-memory addresses. §18 found compute texture writes slower than buffer write + blit for small
  1× loads on this GPU, so the GPU side must be measured. The CPU side (the 1× limiter) would gain either way.
- **Compute-encoder sharing between a resolve and its following loads:** ~1% CPU at 1×, only with an encoder-lifetime
  hook at every encoder creation site.

### Switches (§19)

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_ENCODER_TRIM=0` | on | Labels and debug groups on every encoder again; 8-slot attachment-heap walk |
| `FABLE2_ENCODER_LABELS=1` | off | Keep debug labels with the trim on (GPU captures; `MTL_CAPTURE_ENABLED=1` does the same) |

Files (§19): `backends/metal/port/rex/graphics/metal/metal_encoder_trim.h` (new), `metal_command_processor.cc`,
`metal_render_target_cache.cc`, `metal_direct_host_resolve.cc`, `metal_texture_cache.cc`; `docs/resolution-scale.md`,
`docs/bug-patterns.md`. Restore point: `../restore-points/2026-10-11-tinypasses-before.patch` (clean tree).

## 20. Resolve-to-texture at native 1× (2026-10-11)

Branch `rtt-1x` (agent slot, from `perf-staging` 8f68601). M5 Max, saves `hero1-dog-live_2026-10-11_11-20-20` (Bower
Lake) and `hero1-dog-live_2026-10-07_14-45-08` (Old Tomb). Harness, scripts and copied CSVs:
`out/validation/rtt1x-20261011/` (`arm.sh`, `clean.sh`, `ub.sh` (arms by name), `final.sh`, `census.py` (census rates
per frame), `cpf.py`, `holds.py`, `keytail.py --keys`, `parity.py`, `parity.txt`). Every run: verdict `ok`, 0 AV; perf
runs without contention.

At 1× the GPU Commands thread is the limit (§19). Every resolve output that a later draw samples was reloaded with
an untile compute plus a blit: 67 of each per frame in Bower Lake (0.84 + 0.30 ms of GPU busy), 31 in Old Tomb.

### Census and upper bound (stage 1)

- `FABLE2_RTT_DIAG=census` at 1× (Bower Lake, per frame): 42 base loads of tracked textures (class 0 11, class 1 16,
  class 2 2, class 3 12 (the `k_1_5_5_5` mip chains), class 4 1). Another 37 loads kept the base valid but reloaded the
  mips (resolves into whole mip levels, 37 per frame). Class 0 at 1× is mostly a `k_8_8` 128×128 texture invalidated
  by memexport, which really changes shared memory at 1× and so must reload.
- Upper bound `FABLE2_RTT_DIAG=skip` (**unsafe**, image wrong): keep plus classes 1/2/3 and whole mip levels marked up
  to date without a write. Bower Lake: GPU Commands 8.25 → 7.88 ms/frame (−4.5%), GPU 8.11 → 7.78 ms, hold FPS
  116.0 → 122.6, uploads 67 → 43 per frame; 60-capped process 1.28 → 1.24 cores. Old Tomb: GPU Commands 4.62 → 4.49
  ms/frame, GPU 4.63 → 4.38 ms, FPS 217 → 221.
- Keep alone (no fused writes) measured as noise (Bower Lake GPU 7.94 ms, GPU Commands 8.18 ms/frame): the kept
  textures still reloaded their mips.

### Change (`FABLE2_RTT_1X`, default on)

- **Tracked textures.** At 1×, uncompressed tiled 2D one-layer textures (the shapes a resolve can write) register with
  resolve-to-texture (`rtt_tracked_`; about 99 in Bower Lake). The global watch scans them on each invalidation
  (`tr_n_rtt_inv` ~137, `tr_n_rtt_scan` ~13.9k per frame: negligible). They are created with shader-write usage when
  fused writes are on (§18: costs nothing measurable).
- **Keep (`FABLE2_RTT_1X_KEEP`).** `rtt_valid_` / `rtt_mips_valid_` as in §17/§18, but on shared-memory content: a
  resolve clears them only over its exact extent (rounded to 256 B). Unlike the scaled path, memexport writes **do**
  clear them (at 1× textures load from shared memory, which memexport changes), as do CPU writes and whole-memory
  invalidations.
- **Fused (`FABLE2_RTT_1X_FUSED`).** 33 unscaled variants of the §17 wrapper (`resolve_*_rtt_cs`, without
  `XE_RESOLVE_RESOLUTION_SCALED`; same store pattern: the second store of a thread is the local-X XOR, the next N/bpb
  pixels). The direct host resolve still stores shared memory exactly as before and also writes up to two matching
  textures: base level, whole mip levels (§18) and 16 bpb targets. Guest memory stays the source of truth, so morph
  readback (0x12704000), CPU reads of resolved memory and every aliasing texture see the same bytes. The EDRAM-dump
  resolve path (`rt_resolve`, 1.6–1.8 per frame at 1×) is not fused.
- **Verify (`FABLE2_RTT_VERIFY=1`) now covers the staging path.** At a skipped 1× load, the reference untile goes into
  its own buffer, no blit writes the texture, the deferred flush reads the texture back and the command buffer's
  completion compares them byte for byte (each level, including packed and repacked ones).

### Exactness

- Verify: **0 mismatches** in 138k (spin5 1×), 136k (spin5 2×), 89k (spin5 3×) and 619k checks (area run, 1×).
- Negative control `FABLE2_RTT_VERIFY_BREAK=1` at 1×: 154,872 of 159,488 checks mismatched.
- Parity at 1× (spin5, `parity.py`): on vs off, run alone, idle median 0.22% / max 1.66%, idle2 0.34% / 5.18%; off vs
  off 0.47% / 2.13% and 0.93% / 5.85%. On vs off sits inside the noise. (A pair run beside other test games differed
  more, 1.18% / 4.59% median, but the same build and setting differed as much: 0.74% / 4.87%.) By eye (idle, on vs
  off): dog fur, breadcrumb glow, fire, shadows and HUD are the same.

### Results (1× uncapped headings8; GPU from the pass trace; same build, `FABLE2_RTT_1X=0` vs default)

| Bower Lake | off (2 runs) | on (2 runs) |
|---|---|---|
| GPU Commands ms/frame | 8.06 / 8.17 | 7.71 / 7.59 |
| Process ms/frame | 17.84 / 18.30 | 16.82 / 16.47 |
| FPS, hold mean | 116.5 / 116.2 | 117.6 / 120.3 |
| GPU ms/frame | 8.17 / 8.03 | 8.13 / 8.16 |
| Uploads (untile + blit) per frame | 67.7 | 17.1 |
| Encoders per frame | 372 | 268 |
| 60-capped GPU Commands / process cores | 0.505 / 1.27 | 0.476 / 1.20 |

| Old Tomb, 1 run | off | on |
|---|---|---|
| GPU Commands ms/frame | 4.77 | 4.41 |
| FPS, hold mean | 209 | 223 |
| GPU ms/frame | 4.58 | 4.46 |
| 60-capped process cores | 0.92 | 0.94 |

- **CPU:** −0.45 ms/frame of GPU Commands (−5.6%) and −1.4 ms/frame of process time in Bower Lake, −0.36 ms (−7.6%) in
  Old Tomb.
- **GPU (M5 Max): flat.** Uploads −0.71 ms of busy time, resolves +0.24 ms. The fused per-texel texture writes cost
  most in the four tiny mip-chain resolves (`full16` 256→32, 11.6 each per frame: ~0.06 → ~0.09 ms marginal each),
  which are latency-bound (§18 found the same balance for direct texture writes at 1×). Upper bound GPU was 7.78 ms.
- Fused without keep (`FABLE2_RTT_1X_KEEP=0`, 1 run): GPU 8.46 ms, 113.8 FPS: worse. Keep must stay on with fused
  (a neighbouring resolve would otherwise drop the fused content and reload it).
- `tr_n_rtt_waste_kb` ~7.6 MB/frame of fused writes are invalidated before any sample (aliasing 8888/2_10_10_10 pairs).
- **Base-Mac estimate (not measured):** at 1× those Macs are limited by the GPU Commands thread, so −0.4 to −0.6 ms
  of it is roughly +5–7% FPS uncapped and about −0.03 cores (−6% of that thread) at 60. The GPU side there may differ
  in either direction (bandwidth saved: 47 → 4 MB/frame of blit; per-texel texture writes added).
- 2× / 3×: the scaled path is unchanged (all 1× code is gated on an unscaled draw resolution). Sanity runs on this
  build: 2× 89.1 FPS / GPU 10.79 ms, 3× 61.1 FPS / GPU 15.93 ms, scaled fused writes 67 per frame as before.

### Validation (default build)

spin5 1× / 2× / 3× with verify, area with verify, boot, F5 (saved), bootloop 3/3: all verdict `ok`, 0 AV, skip-intro
guard clean (no `playing original`).

### Switches (§20)

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_RTT_1X=0` | on | No resolve-to-texture at 1× (staging reload of every resolved texture, as before). Dev Launcher toggle |
| `FABLE2_RTT_1X_KEEP=0` | on | A resolve invalidates every tracked texture on its pages (fused targets still kept) |
| `FABLE2_RTT_1X_FUSED=0` | on | No fused writes at 1× (keep only) |
| `FABLE2_RTT_DIAG=skip` | off | At 1× also skips class 3 and whole mip levels (**unsafe** upper bound) |

New counters: `tr_n_rtt_inv`, `tr_n_rtt_scan`, `tr_n_rtt_trk` (global-watch invalidations, tracked textures scanned,
tracked count). The `tr_n_rtt_*` counters and the census now also count at 1×.

Files (§20): `backends/metal/port/rex/graphics/metal/metal_resolve_to_texture.cc`, `metal_texture_cache.cc/.h`,
`metal_direct_host_resolve.cc`, `metal_shared_memory.h`, `metal_command_processor.cc`, `tools/compile_metal_helpers.py`,
`tools/macos/DevLauncher/main.swift`, `docs/resolution-scale.md`, `docs/bug-patterns.md`. Restore points:
`../restore-points/2026-10-11-rtt1x-{upperbound,fused}.patch`.

## 21. Render targets sized to the rows in use (2026-10-11)

Branch `rt-shrink` (agent slot, from `perf-staging` 10d4095). M5 Max, Bower Lake save `hero1-dog-live_2026-10-11_11-20-20`.
Goal: memory footprint for 8 GB Macs, exact. Harness, scripts and logs: `out/validation/rtshrink-20261011/` (`mem.sh`
memory run with `footprint` + `vmmap --summary` of our own pid every 10 s, `perf.sh` headings8 + pass trace, `final.sh`
validation set, `holds.py`, `parity.py`, `parity-x3*.txt`, `crops/`). Every run: verdict `ok`, 0 AV, no contention,
`pgrep -x fable_2` = 1 after close.

### Where the memory went (stage 1, `FABLE2_MEM_REPORT=<seconds>`)

The report logs, every N seconds: `phys_footprint`, `MTLDevice.currentAllocatedSize`, the render targets (count, bytes,
the bytes at full EDRAM height, the largest by key with height / rows needed / full height / grow count), the RT and
texture heap pools (size, used), the EDRAM buffer, cache textures, texture-upload staging pool, scaled-resolve buffers
(current, retired) and the shared-memory buffer. Bower Lake, headings8 at 60 FPS, ~25 s of gameplay, before:

| | 1× | 2× | 3× |
|---|---|---|---|
| phys_footprint | 4.06 GB | 6.41 GB | 9.48 GB |
| Metal allocated (`currentAllocatedSize`) | 1.67 GB | 3.83 GB | 6.73 GB |
| render targets (50 keys) | 602 MB | 2369 MB | 4851 MB |
| RT heap pool (power-of-two heaps, ≤ 1 GB) | 704 MB (602 used) | 1024 MB (864 used) | 1024 MB (916 used) |
| cache textures | 118 MB | 266 MB | 485 MB |
| scaled-resolve buffers (retired) | 0 | 97 MB (0) | 218 MB (0) |
| shared memory / EDRAM buffer | 512 / 10 MB | 512 / 40 MB | 512 / 90 MB |

- The render targets were 59–72% of the Metal allocations at 2×/3×. Each key's texture spans the whole EDRAM height
  times the scale (`GetRenderTargetHeight`: down to the next 11-bit addressing period), e.g. the main 2×MSAA HDR target
  3360×3528×2 (183 MB at 3×) for a 720-row frame, a 4×MSAA D24S8 840×7032 (119 MB) for 360 rows, a 320-wide effect
  target 960×16368 (121 MB) for 192 rows. All pages are resident: Metal private textures are committed on creation.
- Not from the render targets (3×): ~0.9 GB of `IOAccelerator (graphics)` (CPU-visible buffers: shared memory, upload
  pages, bindless tables, shader code), ~0.9 GB `owned unmapped` (driver-side, grows 0.57 → 0.86 GB from 1× to 3×), guest
  memory ~0.55 GB, malloc ~0.3 GB. Binary archives (~128 MB loaded) are clean mapped files, not footprint.

### Change (`FABLE2_RT_SHRINK`, default on; `=0` = EDRAM-height textures; `=census` = full textures, rows tracked only)

- **What can address a row.** Every texel access derives from an EDRAM ownership claim or a draw:
  - SDK hook `OnOwnershipClaimed(key, end_tiles)` at the top of `ChangeOwnership`
    (`backends/metal/sdk-metal-rt-shrink.patch`), called for draws, resolve clears and native-scale tile conversion,
    before the transfers are built. Transfers into and out of a target, host-depth copies, resolves (dump and direct),
    resolve clears and deferred clears all address rows inside ranges the target has claimed. Rows =
    ceil(end / pitch at 32bpp) tile rows × tile height × scale (the 32bpp pitch is conservative for 64bpp keys).
  - A draw's own extent is its claim: the SDK's draw extent estimate (viewport and scissor; for unclipped draws, the
    vertex positions run on the CPU). The guest scissor is the 8192×8192 Direct3D 9 default in nearly every draw, so it
    bounds nothing.
- **Growth.** A target starts one tile row tall. Before the GPU work that addresses more rows (right after the base
  `Update`, at `Resolve`, after `PrepareHostRenderTargetsResolveClear`, after a tile conversion) it gets a new texture
  of the needed height (tile-row granularity, +1/8 headroom on a re-grow): `RequestTransferCommandBuffer` (flushes
  prepared draws, ends the render encoder), a blit of the old rows (exact for MSAA colour and MSAA depth+stencil, checked
  in a standalone Metal test) and, if the first bind had cleared the texture, a clear pass with the same clear value
  first. A target that was never bound or written is simply reallocated. Views are recreated, the render-pass
  descriptor is rebuilt.
- **Render-pass partners.** All attachments of a pass get one common height (clamped to each one's full height), so
  the render area, a first-bind clear and the scissor clamp cover the same rows as before.
- **Separate allocations.** With the shrink, render targets no longer use the RT heap pool: released textures left
  ~200 MB of holes in the never-returned power-of-two heaps at 3×. `FABLE2_RT_SHRINK_HEAPS=1` keeps the pool.
  Texture-cache heaps are unchanged.
- About 60 grows per session, nearly all in the first second of gameplay (2× copies 85 MB once, 3× 191 MB); none
  during the headings8 window. Counters `tr_n_rts_grow`, `tr_n_rts_grow_kb`.

### Exactness

- Ownership, transfers, resolves and clears are unchanged; every row inside a claimed range keeps its bytes across a
  grow. The only rows that differ from before are rows no claim ever covered, which no transfer, resolve or clear reads.
- **What could still differ:** a target claims tiles past its earlier claims that have no owner (so nothing is
  transferred in) after pixels were drawn there outside a draw's extent estimate. Before, those pixels would show; now
  the rows hold the clear value. The verify mode counts such claims ("unowned claims"): 0 in every run.
- **Verify `FABLE2_RT_SHRINK_VERIFY=1`** (diagnostic): before every transfer (destination, source and host-depth source
  rectangles), resolve source (owner rectangles of the dump/direct span), resolve clear and clipped draw (viewport and
  scissor), it checks that the texture reaches the rows; it also counts unclipped draws (bounded only by the vertex
  estimate) and the unowned claims above. Result: **0 misses** in every run (see validation).
- **Negative control** `FABLE2_RT_SHRINK_DIAG=noclaim` (claims ignored, unsafe): 2,566,752 misses in one 1× spin5 run.
- Parity at 3× (spin5, `parity.py`, 4×4 linear-light regions): on vs off idle median 0.53% / max 7.6% and 0.28% / 3.0%;
  off vs off 0.57% / 8.3%. idle2 (animated) on vs off 3.3–4.1% / 18–22%, off vs off 0.74% / 3.2%; §18 measured 8.4% / 37%
  on vs on for idle2. By eye at 3× (`crops/`): dog fur, breadcrumb glow, fire, shadows and HUD are the same.

### Results

Memory (headings8 at 60 FPS, last report of the run; same save and window):

| | 1× before → after | 2× before → after | 3× before → after |
|---|---|---|---|
| phys_footprint | 4.06 → 3.61 GB (−0.45) | 6.41 → 4.41 GB (−2.00) | 9.48 → 5.65 GB (−3.83) |
| Metal allocated | 1.67 → 1.09 GB | 3.83 → 1.80 GB | 6.73 → 2.87 GB |
| render targets | 602 → 125 MB | 2369 → 494 MB | 4851 → 1095 MB |
| RT heaps | 704 → 0 MB | 1024 → 0 MB | 1024 → 0 MB |

Speed (headings8 uncapped, hold mean; GPU ms from the pass trace; same build, `FABLE2_RT_SHRINK=0` vs default):

| | off | on |
|---|---|---|
| 2× FPS (3 runs; last pair = final build) | 89.7 / 89.9 / 89.2 | 91.8 / 91.8 / 92.3 |
| 2× GPU ms/frame | 10.75 / 10.72 / 10.76 | 10.50 / 10.51 / 10.49 |
| 3× FPS (last pair = final build) | 61.4 / 61.0 | 62.7 / 63.0 / 63.3 |
| 3× GPU ms/frame | 15.71 / 15.83 | 15.36 / 15.35 / 15.28 |
| 2× 60-capped: FPS / 1% low / p99 | 60.0 / 53.4 / 18.5 ms | 60.0 / 53.8 / 18.4 ms |

- No regression; about +2% FPS and −0.25/−0.35 ms GPU at 2×/3× (fewer pages to map, smaller attachments).
- 2× uncapped pacing: the 1% low is lower with the shrink in all three pairs (41 / 62 / 60 vs 69 / 69 / 69 FPS; p99
  23 / 15 / 16 vs 14 ms): a few frames per second at 14–23 ms between 8–10 ms frames, no grows in the window, GPU p99
  lower. At 60 FPS (what is played) pacing is the same (table). Recorded, not chased: it is an uncapped-only frame
  scheduling effect of the faster GPU frames.
- **8 GB Macs:** before, a 2× game alone needed 6.4 GB and 3× 9.5 GB; now 4.4 and 5.7 GB.

### Other allocations looked at

- Duplicate MSAA variants (1× and 4×MSAA keys over the same tiles, §16/§19) are part of the EDRAM aliasing: merging
  them is not exact. They shrink with everything else (each now holds only its claimed rows).
- Retired scaled-resolve buffers: 0 in steady play (the retired budget is only used during a grow).
- Texture-upload staging pool (`staging` in the report): 119–136 MB pooled at 2×/3× (its cap is 512 MB). Texture heaps: 160 / 288 / 512 MB at 1×/2×/3×, ≥ 94% used.
- Pipeline archives: ~128 MB of binary archive segments, mapped clean (not footprint); a new segment ~9 MB.
- Shared memory (512 MB, the guest-physical mirror) and the EDRAM buffer (scaled) are needed as they are.

### Validation (default build)

All with `FABLE2_RT_SHRINK_VERIFY=1` and `FABLE2_RTT_VERIFY=1` where it applies (`final.sh`, `final.log`):

| Run | Verdict | RT-shrink misses / unowned claims | RTT verify |
|---|---|---|---|
| spin5 1× / 2× / 3× | ok, 0 AV ×3 | 0 / 0 | 158k / 155k / 86k checked, 0 mismatched |
| area 2× / 1× | ok, 0 AV ×2 | 0 / 0 | 608k / 621k checked, 0 mismatched |
| boot, F5 (saved) | ok, 0 AV | 0 | |
| bootloop | 3/3 pass | | |

Skip-intro guard clean (no `playing original`). Every perf and memory run above: verdict ok, 0 AV, no contention.

### Switches (§21)

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_RT_SHRINK=0` | on | EDRAM-height render targets (before this change). Dev Launcher toggle |
| `FABLE2_RT_SHRINK=census` | off | Full textures; rows tracked for the memory report only |
| `FABLE2_RT_SHRINK_HEAPS=1` | off | Keep the RT heap pool with the shrink |
| `FABLE2_RT_SHRINK_VERIFY=1` | off | Diagnostic: check every addressed row, count unclipped draws and unowned claims |
| `FABLE2_RT_SHRINK_TRACE=1` | off | Diagnostic: log every claim / pass that raises a target's rows |
| `FABLE2_RT_SHRINK_DIAG=noclaim` | off | **Unsafe** negative control: claims ignored |
| `FABLE2_MEM_REPORT=<seconds>` | off | Memory report (log) |

Files (§21): `backends/metal/sdk-metal-rt-shrink.patch` (new), `tools/build_macos_metal.py` (patch list),
`backends/metal/port/rex/graphics/metal/metal_render_target_cache.cc/.h`, `metal_command_processor.cc/.h`,
`metal_texture_cache.cc/.h`, `metal_heap_pool.h`, `metal_shared_memory.h`, `tools/macos/DevLauncher/main.swift`,
`docs/resolution-scale.md`, `docs/bug-patterns.md`. Restore points: `../restore-points/2026-10-11-rtshrink-{stage1,stage2}.patch`.

## 22. Upscaling: MetalFX Spatial as a player choice (2026-10-12)

Branch `metalfx-upscale` (agent slot, from `perf-staging` 0368c00). M5 Max, Bower Lake save
`hero1-dog-live_2026-10-11_11-20-20`. Harness, logs, crops and scores: `out/validation/metalfx-20261012/` of the
slot (`perf.sh` headings8 + GPU trace, `shot.py` still-camera guest + screen captures, `cmp4k.py` crops and
linear-light scores, `sharp.py` edge energy). Every run: verdict `ok`, 0 AV, no contention, `pgrep -x fable_2` = 1.

### What the existing `metalfx` switch did (unchanged)

- `metal_presenter_use_metalfx` (seeded from `[graphics] metalfx` / `FABLE2_METALFX`, §8 item 5). Each present:
  the guest output (internal size: 1280×720 × scale) → MetalFX spatial scaler → a private texture of the
  aspect-fit output rect inside the drawable → the usual bilinear pass, now 1:1, into the drawable → ImGui.
- It runs only when that rect is larger than the internal size; otherwise it logs `[metalfx] not applied` and the
  plain bilinear pass runs. The drawable is the window's pixel size (§9), so:
  - 1280×720-point window (2560×1440 px): only 1× is upscaled; 2× is 1:1; 3× is downsampled (bilinear).
  - Fullscreen on a 4K-class display (3840×2160, or 4608×2592 on the maintainer's TV in "looks like 2304×1296"):
    1× and 2× are upscaled, and 3× too at 4608×2592.
- So "2× internal → 4K output" already worked in fullscreen. What was missing: a clear setting name, a measured
  cost (the presenter command buffer was not in the GPU trace, so §8 read it as about 0), and a way to test a 4K
  drawable without taking over the screen.

### Changes

- **Setting:** `[graphics] upscaling = "off" | "metalfx-spatial"` (default `"off"`). The old boolean `metalfx`
  key is still read when `upscaling` is missing. `FABLE2_UPSCALING=off|metalfx-spatial` overrides it (the old
  `FABLE2_METALFX=1|0` still works when `FABLE2_UPSCALING` is unset). An explicit `metal_presenter_use_metalfx`
  cvar still wins. Off leaves the cvar at its default, so the off path is today's path.
- **Dev Launcher:** "Upscaling: Off (bilinear, original look) / MetalFX Spatial (look-changing; window larger than
  internal res)", always passed as `FABLE2_UPSCALING`. The old saved choice (`upscaler = metalfx`) carries over.
  CLI `--upscaling off|metalfx-spatial` (`--upscaler bilinear|metalfx` still accepted).
- **Presenter:** MetalFX now requires the output to be at least the input on both axes (it only upscales).
- **Test-only drawable size:** `FABLE2_TEST_DRAWABLE=WxH` (honoured only with `FABLE2_TEST_CONTROL_DIR`, i.e. test
  sessions) renders the presenter into a drawable of that size; Core Animation shrinks it into the small background
  window. Log: `[metalfx] test drawable override 3840x2160`. `capture --screen` returns the full drawable.
- **GPU trace:** the presenter command buffer is recorded as `gpu_present` (with `FABLE2_GPU_TIME_TRACE=1`).
  `f2diag gpu` prints `present_ms` (per frame, plus per-command-buffer p50/p90) and `gpu_ms_incl_present`.
  The presenter runs at display rate (about 60 Hz here), so at 90+ FPS uncapped there are about 0.64 presents per
  guest frame.

### Performance (headings8 uncapped, hold mean; GPU = guest command buffers, f2diag window 13–33 s; 1 run per arm)

| Drawable | Arm | FPS | GPU ms mean / p95 | Presenter per command buffer p50 |
|---|---|---|---|---|
| 3840×2160 | 3× native (1:1) | 63.2 | 15.28 / 17.58 | 0.06 ms |
| 3840×2160 | **2× + MetalFX** | **91.6** | 10.53 / 12.50 | 0.55 ms |
| 3840×2160 | 2× bilinear | 92.4 | 10.45 / 12.35 | 0.16 ms |
| 3840×2160 | 1× + MetalFX | 108.9 | 8.34 / 10.11 | 0.41 ms |
| 2560×1440 | 2× native (1:1) | 92.3 | 10.46 / 12.33 | 0.10 ms |
| 2560×1440 | 1× + MetalFX | 110.5 | 8.17 / 9.82 | 0.20 ms |
| 2560×1440 | 1× bilinear | 110.5 | 8.17 / 9.84 | 0.04 ms |
| 2560×1440 | 3× (bilinear downsample) | 63.9 | 15.13 / 17.66 | 0.04 ms |

- **2× + MetalFX at 4K: +45% FPS against native 3×** (−4.8 ms guest GPU). MetalFX itself costs about 0.4 ms per
  present at 2560×1440 → 3840×2160 (0.55 vs 0.16 ms against bilinear), about 1% FPS. 1280×720 → 2560×1440 costs
  about 0.16 ms, below FPS noise.
- Presenter p90s (0.6–1.5 ms in every arm, MetalFX or not) include waiting on the guest frame inside the
  command buffer, so the p50 difference is the better MetalFX estimate.
- Base M1 (estimate, not measured): MetalFX spatial at this size should stay around 1–2 ms per present; the gain
  comes from the internal scale, so 1× + MetalFX → 1440p/4K costs about 1× + that.

### Image comparison (still camera, screen captures at t = 20 s, linear light)

Crops (`crops4k/*.png` side by side; `view4k/*.png` stacked: 3× | 2× MetalFX | 2× bilinear | 1× MetalFX):
HUD quest text, HUD dog icons, dog fur, hero, fence edges, fern, fire glow, breadcrumb-trail glow, bark.

Mean |Δ| of linear RGB against native 3× at 3840×2160, % of full scale (full resolution / box-averaged to 720p).
The scene animates (wind, fire, the dog turns its head), so the second 3× run is the noise floor:

| Crop | 3× rerun (noise) | 2× + MetalFX | 2× bilinear | 1× + MetalFX | 1× bilinear |
|---|---|---|---|---|---|
| HUD text | 0.19 / 0.14 | 1.38 / 0.80 | 1.35 / 1.09 | 1.86 / 1.31 | 1.68 / 1.19 |
| HUD dog icons | 0.22 / 0.22 | 0.78 / 0.54 | 0.50 / 0.39 | 1.14 / 0.60 | 0.94 / 0.61 |
| Hero | 0.24 / 0.23 | 0.41 / 0.28 | 0.38 / 0.28 | 0.53 / 0.37 | 0.54 / 0.39 |
| Fence edges | 1.43 / 1.10 | 1.87 / 1.39 | 1.72 / 1.35 | 2.15 / 1.72 | 2.13 / 1.72 |
| Fern | 0.60 / 0.50 | 0.90 / 0.69 | 0.82 / 0.67 | 1.00 / 0.79 | 0.97 / 0.79 |
| Bark | 0.53 / 0.43 | 0.82 / 0.60 | 0.70 / 0.56 | 0.97 / 0.80 | 0.96 / 0.80 |
| Whole frame | 0.37 / 0.31 | 0.65 / 0.47 | 0.57 / 0.45 | 0.78 / 0.59 | 0.75 / 0.59 |

Dog fur, fire and trail glow are dominated by animation timing (3× runs slower), so they are not listed.

Edge energy (mean |∂x| + |∂y| of linear luminance, % of native 3×):

| Crop | 2× + MetalFX | 2× bilinear | 1× + MetalFX | 1× bilinear |
|---|---|---|---|---|
| HUD text | 97% | 84% | 94% | 82% |
| HUD dog icons | **132%** | 90% | **131%** | 81% |
| Hero | 109% | 57% | 57% | 34% |
| Fence edges | 101% | 66% | 74% | 50% |
| Fern | 109% | 63% | 68% | 40% |
| Bark | 116% | 64% | 65% | 40% |

**How it looks:**
- **2× + MetalFX → 4K looks about as sharp as native 3×**: same or slightly higher edge contrast on geometry,
  bark and foliage, and crisp HUD text. It is not more *faithful* to 3× than bilinear: the linear-light difference
  is slightly larger (0.65 vs 0.57 whole frame), because MetalFX sharpens what is there rather than recovering 3×
  detail. Fine texture detail stays at 2×, and the HUD icons get a visible sharpening halo (132%).
- **1× + MetalFX** sharpens edges and HUD outlines but textures stay at 1× (57–74% of 3× edge energy), and HUD
  icons and text look over-sharpened with stair-stepping on curves. Better than 1× bilinear, clearly below 2×.
- 2560×1440 window, against 2× native 1:1 (noise 0.02%): 1× + MetalFX recovers 76–98% of 2×'s edge energy
  (1× bilinear 45–82%); whole-frame difference 0.55% vs 0.57% for 1× bilinear (`cmp1440.txt`, `sharp1440.txt`).

### HUD and overlays

- The game's HUD is part of the guest frame, so it is rendered at the internal scale and upscaled once with the
  scene (MetalFX output rect = final rect; the following bilinear pass is 1:1). No double scaling.
- The recomp's own ImGui overlays (FPS counter, turbo notice, notifications) are drawn after the upscale at drawable
  resolution, so they are unaffected.

### MetalFX Temporal: not done

`MTLFXTemporalScaler` needs per-pixel motion vectors, depth, and a sub-pixel jitter applied to the scene's
projection. None of these is available in an exact way:
- Fable II computes no motion vectors. Camera-only vectors could be reconstructed from depth plus the previous and
  current view-projection, but that needs the camera matrices found among the guest shader constants. Skinned and
  animated objects (the hero, the dog's fur shells, foliage sway) would get wrong vectors and ghost.
- The jitter would have to be added to scene draws only (not post-processing, shadows or the HUD) in the shader
  translator, which needs a reliable classification of passes.
- The HUD is drawn into the same guest frame, so it would be temporally reconstructed with the scene (text ghosting)
  unless the HUD draws were separated out.
That is an XL change with a high artefact risk (engine options rank 12); spatial gives most of the cost benefit now.

### Validation

spin5 at 2× + MetalFX (4K test drawable), 1× + MetalFX (window) and 2× off; boot and F5 (saved) at 2× + MetalFX:
all verdict `ok`, 0 AV, `skip_intro_missing` 0. The off path logs only `[fable2-config] upscaling=off` and
never touches MetalFX. The config test covers the new key, the legacy `metalfx` key and the precedence between them.

### Switches (§22)

| Switch | Default | Meaning |
|---|---|---|
| `[graphics] upscaling` / `FABLE2_UPSCALING` | `"off"` | `off` or `metalfx-spatial` (look-changing) |
| `[graphics] metalfx` / `FABLE2_METALFX` | — | Legacy aliases, read only when the new ones are missing |
| `FABLE2_TEST_DRAWABLE=WxH` | unset | Test sessions only: presenter drawable size override |

## 23. Character texture-space passes at native 1× (2026-10-12)

Playtest bug: a dark seam down the dog's chest at 2× and 3×, not at 1×. Save `hero1-dog-live_2026-10-12_11-21-00`
(the dog faces the camera about 1.5–2 s after the load; LB held). Evidence: `out/validation/dogseam-20261012/`
(KEEP; `before-after-2x.png` and `before-after-3x.png` = 1× | before | after, morph readback dumps).

### Cause

- Every switch added for 2×/3× off (shadows, RTT, direct loads, RT shrink, specialization, tile EDRAM,
  deferred clears, effects, encoder trims, memexport fixes): the seam stays. It is a basic scaled-path problem.
- Each frame the game lights the dog's coat into a **128×128 texture-space target** (`RT @ 0t <2t>`, PS
  `44B567635EB10620` or `39B39197B46ECA72`, resolved to 0x13401000). A second pass (PS `1B52AA35A311938D`) dilates
  it into the gutters between UV islands with fixed texel-offset fetches and resolves to 0x133E1000 / 0x133F1000.
  The fur shells (VS `7C5710DE…`) sample those textures.
- Scaled, this target is 256² / 384². The UV islands are rasterized finer, and the dilation (offsets of 1 texel,
  scaled or not, see §12) no longer fills the gutter ring that it fills at 1×. Along the chest's mirror seam the
  fur then samples the clear colour: the dark line, about one host pixel wide.
- The morph pass (PS `E2120FEAFA4CF565`, §8) had the same shape: its smaller levels were drawn at the scaled size,
  where texture LOD follows the smaller host pixels. 3 of 5 levels per morph differed from 1× (bug-patterns row 72).
  Fixing only the morph (readbacks byte-identical) did **not** remove the seam; the lighting/dilation pair does.

### Change (`FABLE2_TEXSPACE_NATIVE`, default on; `=0` = the old scaled passes)

- Draws whose pixel shader is one of those four (`IsTexSpacePixelShader`, `metal_render_target_cache.cc`) and
  that bind one 1×MSAA colour target and no depth go through the §16 native-scale hook: the target is drawn and
  resolved at 1× into shared memory, exactly as on the console. Other draws on the same EDRAM range (the clear quads
  before them) stay scaled; the ownership transfer carries the uniform clear. The command processor flushes the
  prepared-draw queue when the hint flips (`last_texspace_hint_`).
- The morph readback now takes the 1× path (`resolve_plan.native`): no downscale compute.
- No target learning: the 560×360 effect passes that share `<7t>` with the morph target stay scaled.

### Results

- **Exactness:** all 50 morph readbacks of the load are **byte-identical to 1×** at 2× and 3×
  (`FABLE2_MORPH_READBACK_DUMP`). Before: 47 of 50 levels differed (up to 3425 texels by more than 40).
  1× is unchanged (50/50 identical to the earlier 1× dump; the hint needs a scaled draw resolution).
- **Image:** no seam at 2× or 3× (crops above). A faint dotted fur line at the same place is also there at 1×.
- `FABLE2_RTT_VERIFY=1`: 0 mismatches; `FABLE2_RT_SHRINK_VERIFY=1`: no misses.
- spin5 2× and 3×, F5 at 2× (saved): verdict ok, 0 AV.
- headings8 at 3×, uncapped, alone: on 63.4 FPS (p95 18.1 ms), off 63.5 FPS (p95 18.1 ms): unchanged.

### Switches (§23)

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_TEXSPACE_NATIVE=0` | on | Texture-space passes (morph, dog lighting, dilation) scaled again (seam back) |
| `FABLE2_SHADOW_TRACE_FRAMES=<n>` | 2 | Diagnostic: length of the `FABLE2_SHADOW_TRACE` window |

Launcher: "Fix dog chest seam at 2×/3× (character textures at native 1×)". Counter: `tr_n_ts_draw`.

## 24. Auto resolution scale (2026-10-13)

`[graphics] resolution_scale = "auto"`, `FABLE2_RES_SCALE=auto`, or Dev Launcher "Internal resolution: Auto (N× on
this Mac)". It picks 1×, 2× or 3× once at startup from the GPU, so players on slower Macs don't choose a scale their
GPU can't hold. It is not a dynamic-resolution controller. **Default since 2026-10-14: Auto** (game config
`resolution_scale = "auto"`, also when the key is missing; the launchers' Auto for fresh installs; a saved choice
is kept). It was 1 / Native until then; 1 is still the console's own resolution and selectable.

**Where:** `src/core/fable2_res_auto.h` holds the rule and the tables (pure, unit-tested by
`tests/res_auto_tests.cpp` at build time). `src/core/fable2_res_auto.cpp` reads the GPU (IORegistry `model` +
`gpu-core-count`, shared with fps-auto `fable2::fps::QueryGpu`), RAM (`hw.memsize`) and the frame target
(`fable2::fps::Use60()`). `fable2_graphics_config.h ApplyResolutionScale` resolves `auto` and exports the number as
`FABLE2_RES_SCALE`, so the Metal plugin, the FPS counter and the logs only ever see 1/2/3. The Dev Launcher
(`AutoResolution` in `tools/macos/DevLauncher/main.swift`) mirrors the tables for its label;
`tests/res_auto_sync_tests.py` fails if the two copies drift.

**Log** (one line, e.g. this M5 Max):
`[res-auto] 'Apple M5 Max' Max, 64 GB RAM -> 2x: 40 cores x 1.10 (M5) = 44.0 effective at 60 FPS target (2x needs 34, 3x 47): meets the 2x row`,
then the plugin's usual `[res-scale] FABLE2_RES_SCALE=2 -> draw resolution scale 2x2`.

### Rule

```
effective cores = gpu-core-count x generation factor x (0.75 on Ultra)
scale = highest of 3x / 2x whose row is met at the frame target and whose RAM floor is met, else 1x
frame target = 60 FPS when fps_60 resolves to 60 (true, auto -> 60 per section 25, or uncapped), else 30 FPS
```

| Generation | M1 | M2 | M3 | M4 | M5 (and newer) |
|---|---|---|---|---|---|
| Factor (per-core speed vs M3) | 0.80 (est.) | 0.90 (est.) | 1.00 (measured) | 1.05 (est.) | 1.10 (measured) |

| Frame target | 2× needs | 3× needs |
|---|---|---|
| 60 FPS (also uncapped) | ≥ 34 effective cores | ≥ 47 |
| 30 FPS | ≥ 17 | ≥ 24 |

RAM floors: 2× needs 16 GB, 3× needs 32 GB (peak footprint 3.3 / 4.4 / 5.3 GB at 1×/2×/3× in the benchmark,
~9 GB at 3× with specialization). Ultra counts 0.75 per core because two dies don't scale perfectly. With no
`gpu-core-count`, the smallest bin of the tier is assumed (base 7, Pro 14, Max 24, Ultra 48). Unknown generation =
M1 factor. Test overrides: `FABLE2_RES_AUTO_CORES=<n>`, `FABLE2_RES_AUTO_NAME="Apple M1"`.

### Where the thresholds come from

The criterion is "**the estimated GPU p95 frame time fits the frame budget**" (16.7 ms at 60, 33.3 ms at 30), which
leaves headroom for the mean. The estimate scales the M5 Max benchmark (Bower Lake headings8 uncapped,
`benchmark-results/M5-Max-…-2026-10-13_13-16`, 40 cores × 1.10 = 44 effective; GPU p95 9.9 / 12.8 / 17.5 ms at
1×/2×/3×) by `44 / effective cores`. So `need(scale) = 44 × p95(scale) / budget`, rounded up: 2× at 60 =
44 × 12.8 / 16.7 = 33.8 → 34; 3× at 60 = 46.2 → 47; 2× at 30 = 16.9 → 17; 3× at 30 = 23.1 → 24.

Check against the second data point, the M3 Pro (14 cores, `…M3-Pro-…02-41`): predicted 2× / 3× p95 40.2 / 55.0 ms,
measured 30.9 / 52.2. The rule is conservative there (the 2× prediction includes the M5 Max's fixed per-frame cost,
which does not scale with cores). M3 vs M5 per core in the GPU-bound 3× arm: 1.02 by mean, 1.05 by p95, so M3 = 1.00
is slightly pessimistic. Memory bandwidth (M3 Pro ~150 GB/s vs M5 Max ~600) is not a separate input: within a
generation it tracks the tier and core count, and the measured factors already include it.

**What it picks** (all in `tests/res_auto_tests.cpp`; RAM at least the floor):

| Mac (GPU cores) | Effective | 60 FPS target | 30 FPS target |
|---|---|---|---|
| M1 (7/8), M2 (8/10), M3 (8/10), M4 (10), M5 (10) | 6–11 | 1× | 1× |
| M1 Pro (14/16) | 11–13 | 1× | 1× |
| M3 Pro (14) — measured: 2× 37 FPS, 3× 21 FPS | 14 | 1× | 1× |
| M3 Pro (18), M2 Pro (19), M4 Pro (20) | 18–21 | 1× | 2× |
| M1 Max (24/32) | 19–26 | 1× | 2× / 3× |
| M2 Max (30) / M3 Max (30) / M4 Max (32) | 27–34 | 1× | 3× |
| M2 Max (38), M3 Max (40), M4 Max (40) | 34–42 | 2× | 3× |
| M5 Max (40) — measured: 2× 93 FPS, 3× 64 FPS uncapped, 59.4 at the 60 cap | 44 | **2×** | 3× |
| M1 Ultra (64), M2 Ultra (60), M3 Ultra (60) | 38–45 | 2× | 3× |
| M2 Ultra (76), M3 Ultra (80) | 51–60 | 3× | 3× |

Notes:
- **M5 Max gets 2× at 60 FPS, not 3×.** At 3× its GPU p95 is 17.5 ms against a 16.7 ms budget: 59.4 FPS at the
  60 cap, 1% low 53.8. That holds 60 most of the time, but with no headroom. 2× sits at 10.7 ms mean / 12.8 p95.
  If the maintainer prefers 3× there, lower the 60 FPS 3× row to 44 (one number, both copies; the sync test catches a
  miss).
- Superseded by §25 (fps-auto no longer uses 24 GPU cores; the frame target is decided first). Old note: with fps-auto (`fps_60 = "auto"`, 60 FPS from 24 GPU cores), Macs it sent to 30 FPS got the
  30 FPS row, so an M4 Pro (20) gets 30 FPS at 2×, and an M3 Pro (14) 30 FPS at 1×.
- Display refresh rate is not an input: the game's frame target is 60 or 30 whatever the display, and uncapped is
  treated as 60.
- When the M1 report lands: set the M1 factor so its measured 2×/3× p95 matches `44 × p95_M5Max / (cores ×
  factor)`, i.e. `factor = 44 × p95_M5Max(scale) / (p95_M1(scale) × cores)` from its GPU-bound arm, in both copies.

### Verification (M5 Max, 2026-10-13)

- `res_auto_tests` (42 scale cases plus name parsing: M1–M5, base/Pro/Max/Ultra, both targets, RAM floors, unknown core count)
  and the config test (`resolution_scale = "auto"` loads, a number leaves it off) run in every build;
  `tests/res_auto_sync_tests.py` (`/usr/bin/python3` 3.9) passes.
- One background test game (`f2diag run boot autores-m5max --env FABLE2_RES_SCALE=auto`, verdict ok, first launch
  after the rebuild, gameplay ~6.4 s after launch): the `[res-auto]` line above, `[res-scale] FABLE2_RES_SCALE=2 ->
  draw resolution scale 2x2`, `DxbcShaderTranslator init: … scale=2x2`, captures 2560×1440. Contention: two agent-3
  games were running (no perf numbers taken).
- Launcher: Display tab shows "Auto (2× on this Mac)" with the frame rate at 60 and "Auto (3× on this Mac)" at 30.
- Evidence: `out/validation/auto-res-20261013/` (KEEP): log excerpt, capture, launcher snapshots, f2diag result.

### Recommendation for release default

Make `"auto"` the player-facing default (portable build and `config/fable2_config.toml`), keeping the Dev Launcher's
explicit choice for development. On every Mac measured or estimated it picks at most what the GPU holds with headroom,
and it never goes below today's default (1×). It is the maintainer's decision; not changed here. Before switching, wait for
the M1 report to confirm 1× (and, if wanted, a run on any Max-class Mac other than the M5 Max to check the 2× row).

## 25. Auto frame rate uses the same GPU model (2026-10-13)

`fps_60 = "auto"` (the default), `FABLE2_FPS_60=auto`, or Dev Launcher "Frame rate: Auto (N FPS on this Mac)". It
used to pick 60 FPS from 24 raw GPU cores, which sent a 14-core M3 Pro to 30 FPS even though that Mac runs 1× at
63 FPS uncapped and 58.5 FPS at the 60 cap (1% low ~48, `benchmark-results/M3-Pro-…`). It now uses §24's effective
cores and an estimated frame time.

### Update 2026-10-14: recalibrated model and the 2x rule

The model below was fitted on 2026-10-13 builds. The game has become much faster since (the M3 Pro's 1× mean went
from 15.65 to 8.9 ms), so it was refitted, and Auto gained a **2x rule**: a Mac that cannot hold 2× at 60 but holds
a smooth 45+ FPS there runs 2× uncapped. The maintainer plays the 14-core M3 Pro at 2× uncapped (~50 FPS, even
pacing) and asked for that as its Auto choice. The tables in the rest of this section are the 2026-10-13 version;
this update supersedes their numbers.

**Measurements** (uncapped headings8, Bower Lake save, `FABLE2_GPU_TIME_TRACE=1`, GPU ms/frame mean over the sweep):

| Mac | 1× | 2× | 3× | Source |
|---|---|---|---|---|
| M5 Max (40 cores, 44 effective) | 4.88 (169 FPS) | 6.83, 6.53 → 6.68 (124, 145 FPS) | 11.20, 10.94 → 11.07 (87, 90 FPS) | macos 6eaf54b, 2026-10-14, no contention; `out/validation/auto2x-20261014/` (KEEP) |
| M3 Pro (14 cores, 14 effective) | 8.9 (110 FPS) | 19.4 (50.7 FPS) | 37.1 (26.5 FPS) | kit report at 51757bd, `benchmark-results/M3-Pro-Mac15_6-2026-10-14_11-58/` |

- The first launch after the rebuild (M5 Max 1×) measured 5.63 ms / 140 FPS and is not in the fit. It booted and
  finished its sweep normally (29 s for the whole run).
- **The M3 Pro numbers are used as is.** The later footprint-variant change (d908bfb, −0.5 ms at 2× on the M5 Max) was
  estimated at −1.2 to −2.0 ms on the M3 Pro at 2× (`docs/ssmask-20261014.md`), but that was not measured. The
  maintainer's ~50 FPS at 2× matches the kit number. So the fit gets no credit for that saving and is slightly
  pessimistic for slower Macs.

**Fit** (two points per scale, exact through both Macs; `tests/res_auto_tests.cpp` checks both anchors to ±0.2 ms):

```
work  = (M3 Pro ms − M5 Max ms) / (1/14 − 1/44)        fixed = M5 Max ms − work / 44
1×: fixed 3.0,  work 82.6    (was 4.7 / 153)     60 FPS (≤ 18.5 ms) needs ≥ 5.3 effective cores
2×: fixed 0.75, work 261.0   (was 3.0 / 339)     60 needs ≥ 14.7; the 2x rule (≤ 22 ms) needs ≥ 12.3
3×: fixed −1.1, work 534.5   (was 0.4 / 655)     60 needs ≥ 27.3
```

A negative `fixed` at 3× is not physical. The M3 Pro loses more at 3× than its core count predicts, because memory
bandwidth limits it there (the M3 Pro vs M5 Max gap grows with scale: 1.8× at 1×, 2.9× at 2×, 3.4× at 3×). The fit puts that into
`work`. It only matters well above 44 effective cores, where the estimate stays small anyway.

**The 2x rule** (`FABLE2_AUTO_2X`, default on; Dev Launcher Display → Frame rate → "Auto: 2× uncapped when 2× can't
hold 60"):

```
frame-rate Auto at an explicit 2×:  est(2×) ≤ 18.5 ms → 60 FPS;  ≤ 22 ms (≥ 45 FPS) → uncapped;  else 30
base chips (no Pro/Max/Ultra) and unknown GPUs: at most 30 from Auto at any scale (base-tier rule, below)
resolution and frame rate both Auto (ChooseBoth):
  1. frame target at 1× (≤ 18.5 ms → 60, else 30), then the §24 p95 rows at that target, as before
  2. if that is below 2×, RAM ≥ 16 GB (or unknown) and est(2×) ≤ 22 ms:
       run 2× at what frame-rate Auto picks for an explicit 2× (60 if ≤ 18.5 ms, else uncapped)
```

- The result is always what frame-rate Auto would pick for the scale that runs. The unit test sweeps 4–96 cores ×
  four chips × four RAM sizes to check this, and checks that no Mac lands at 2×/3× above 22 ms.
- The uncapped band applies at 2× only, the measured case. 3× and 1× keep 60 / 30.
- **Uncapped = the launcher's existing "Uncapped"**: the SDK `vsync` cvar off, which runs the guest vblank timer at
  1 kHz instead of 60 Hz, with `fps_60` false. fps-auto sets the cvar, or `REX_VSYNC=0` when the GPU plugin has not
  registered the cvar yet (resolution Auto resolves before the plugin loads; the SDK reads `REX_VSYNC` when it
  registers a late cvar). An explicit `--vsync` on the command line still wins. Turbo (F7) behaves as with the
  launcher's Uncapped.
- **Preservation.** The original game runs at 30 FPS. This only changes what Auto picks; `fps_60 = false` / launcher
  "30 FPS" stays the faithful choice, and `FABLE2_AUTO_2X=0` (launcher toggle off) restores Auto's choices from
  before this rule. Uncapped changes no game logic: the simulation runs on the guest clock, as with 60 FPS.
- **Logs:**
  `[fps-auto] 'Apple M3 Pro' Pro: 14 cores x 1.00 (M3) = 14.0 effective; est. 8.9 ms mean at 1x (60 needs <= 18.5) -> 60 FPS; 2x rule: est. 19.4 ms at 2x (<= 22.0, RAM ok) -> 2x uncapped (scale from resolution auto (decided after this); picks 2x)`,
  `[fps-auto] uncapped: REX_VSYNC=0 (vsync cvar not registered yet)`,
  `[res-auto] … below the 2x row; 2x rule (see [fps-auto]) -> 2x`. Window title: `2× (Auto) · uncapped (Auto)`.
- **Launcher labels** follow it: "Internal resolution: Auto (2× uncapped on this Mac)", "Frame rate: Auto (uncapped
  at 2× on this Mac)" (or "Auto (uncapped on this Mac)" next to an explicit 2×).

**Presentation when uncapped (60 Hz external display vs 120 Hz ProMotion).** From the presenter code:
- The Metal presenter sets `CAMetalLayer.displaySyncEnabled = YES` unless `metal_allow_tearing` (default false). The
  `vsync` cvar does not touch presentation, so **uncapped never tears**.
- With display sync on, the SDK presenter paints from the UI thread on request, never from the guest output thread
  (`Presenter::GetDesiredPaintModeFromUIThread`). The guest's frames go through a 3-slot mailbox where the newest
  frame wins, and the UI paint tick is limited to the display refresh. So the guest never waits on the display.
  It renders as fast as the GPU allows (the FPS counter and benchmarks count guest frames), and the display shows
  the newest one at each refresh.
- **60 Hz display:** at most 60 frames per second reach the screen. At ~50 FPS, each frame stays up for one or two
  refreshes (16.7 / 33.3 ms), so there is some judder, but no tearing and no added queueing. Above 60 FPS the extra
  frames are dropped.
- **120 Hz ProMotion (the M3 Pro's built-in display):** at ~50 FPS each frame stays up for two or three refreshes
  (16.7 / 25 ms), a finer and smoother cadence. That is where the maintainer saw the even pacing. The layer does not
  request a matching ProMotion rate (no `preferredFrameRateRange`), so the display does not drop to 50 Hz.

**What every Mac gets** (typical GPU-core bins and RAM; estimates except the M3 Pro and M5 Max anchors; FPS 0 =
uncapped). "Both Auto" = resolution and frame rate both Auto, the defaults since 2026-10-14. The last column is Native
1× chosen by hand with frame-rate Auto:

| Mac (GPU cores, RAM) | Effective | Both Auto: before | Both Auto: after | est. ms at that scale (FPS) | Native 1× by hand + frame-rate Auto: before → after |
|---|---|---|---|---|---|
| M1 (7, 8 GB) | 5.6 | 1× 30 | 1× 30 | 17.8 (56) | 30 |
| M1 (8, 16 GB) | 6.4 | 1× 30 | 1× 30 | 15.9 (63) | 30 |
| M1 Pro (14, 16 GB) | 11.2 | 1× 60 | 1× 60 | 10.4 (96) | 60 |
| M1 Pro (16, 32 GB) | 12.8 | 1× 60 | **2× uncapped** | 21.1 (47) | 60 |
| M1 Max (24, 32 GB) | 19.2 | 1× 60 | **2× 60** | 14.3 (70) | 60 |
| M1 Max (32, 64 GB) | 25.6 | 1× 60 | **2× 60** | 10.9 (91) | 60 |
| M1 Ultra (48, 64 GB) | 28.8 | 1× 60 | **2× 60** | 9.8 (102) | 60 |
| M1 Ultra (64, 128 GB) | 38.4 | 2× 60 | 2× 60 | 7.5 (133) | 60 |
| M2 (8, 8 GB) | 7.2 | 1× 30 | 1× 30 | 14.5 (69) | 30 |
| M2 (10, 16 GB) | 9.0 | 1× 30 | 1× 30 | 12.2 (82) | 30 |
| M2 Pro (16, 16 GB) | 14.4 | 1× 60 | **2× uncapped** | 18.9 (53) | 60 |
| M2 Pro (19, 32 GB) | 17.1 | 1× 60 | **2× 60** | 16.0 (62) | 60 |
| M2 Max (30, 32 GB) | 27.0 | 1× 60 | **2× 60** | 10.4 (96) | 60 |
| M2 Max (38, 64 GB) | 34.2 | 2× 60 | 2× 60 | 8.4 (119) | 60 |
| M2 Ultra (60, 64 GB) | 40.5 | 2× 60 | 2× 60 | 7.2 (139) | 60 |
| M2 Ultra (76, 192 GB) | 51.3 | 3× 60 | 3× 60 | 9.3 (107) | 60 |
| M3 (8, 8 GB) | 8.0 | 1× 30 | 1× 30 | 13.3 (75) | 30 |
| M3 (10, 16 GB) | 10.0 | 1× 30 | 1× 30 | 11.3 (89) | 30 |
| M3 Pro (14, 18 GB) | 14.0 | 1× 60 | **2× uncapped** | 19.4 (52) | 60 |
| M3 Pro (18, 36 GB) | 18.0 | 1× 60 | **2× 60** | 15.2 (66) | 60 |
| M3 Max (30, 36 GB) | 30.0 | 1× 60 | **2× 60** | 9.4 (106) | 60 |
| M3 Max (40, 48 GB) | 40.0 | 2× 60 | 2× 60 | 7.3 (137) | 60 |
| M3 Ultra (60, 96 GB) | 45.0 | 2× 60 | 2× 60 | 6.5 (153) | 60 |
| M3 Ultra (80, 256 GB) | 60.0 | 3× 60 | 3× 60 | 7.8 (128) | 60 |
| M4 (8, 16 GB) | 8.4 | 1× 30 | 1× 30 | 12.8 (78) | 30 |
| M4 (10, 16 GB) | 10.5 | 1× 30 | 1× 30 | 10.9 (92) | 30 |
| M4 Pro (16, 24 GB) | 16.8 | 1× 60 | **2× 60** | 16.3 (61) | 60 |
| M4 Pro (20, 48 GB) | 21.0 | 1× 60 | **2× 60** | 13.2 (76) | 60 |
| M4 Max (32, 36 GB) | 33.6 | 1× 60 | **2× 60** | 8.5 (117) | 60 |
| M4 Max (40, 64 GB) | 42.0 | 2× 60 | 2× 60 | 7.0 (144) | 60 |
| M5 (8, 16 GB) | 8.8 | 1× 30 | 1× 30 | 12.4 (81) | 30 |
| M5 (10, 16 GB) | 11.0 | 1× 30 | 1× 30 | 10.5 (95) | 30 |
| M5 Pro (16, 24 GB) | 17.6 | 1× 60 | **2× 60** | 15.6 (64) | 60 |
| M5 Pro (20, 48 GB) | 22.0 | 1× 60 | **2× 60** | 12.6 (79) | 60 |
| M5 Max (32, 36 GB) | 35.2 | 2× 60 | 2× 60 | 8.2 (122) | 60 |
| M5 Max (40, 64 GB) | 44.0 | 2× 60 | 2× 60 | 6.7 (150) | 60 |

- **M3 Pro (14 cores): 2× uncapped**, est. 19.4 ms ≈ 52 FPS (measured 50.7). **M5 Max (40): 2× at 60**, unchanged; its
  3× now runs at 87–90 FPS uncapped, but the §24 3× row (47 effective, from the 2026-10-13 p95) still keeps it at 2×.
  Those p95 rows were not refitted here.
- **Base M1 and M2 never land on 2×** (and the base-tier rule keeps them at 30 anyway). Their 2× estimates are 24–34 FPS (M1 7/8 cores 47.4 / 41.5 ms, M2 8/10 cores
  37.0 / 29.8 ms), far below the 45 FPS band. The 8 GB models also miss the 16 GB floor.
- **Base-tier rule: base chips stay at 30.** The refit alone would move every base chip (M1–M5, no Pro/Max/Ultra in
  the name) from 30 to 60 FPS at 1×: the 7-core M1 at an estimated 17.8 ms (56 FPS), the 8-core M1 at 15.9, the M2 at
  14.5 / 12.2. Those are extrapolations from 14 down to 5.6–11 effective cores; base chips have the least memory
  bandwidth (M1 68 GB/s vs the M3 Pro's 150) and a fanless MacBook Air may throttle in long sessions. So, by the
  maintainer's decision, Auto gives base chips (and unknown GPUs) at most 30 FPS (`kBaseTierMaxAutoFps` in
  `src/core/fable2_res_auto.h`, `baseTierMaxAutoFps` in the launcher) and no 2x-rule uncapped band, until a base
  Mac is measured. The estimate is still logged (`… -> 30 FPS (base-tier rule: …)`). Picking 60 FPS by hand works.
  To lift it after a base-Mac report: set the constant to 60 in both copies (the sync test checks they match).
- Many Pro/Max parts that the stale p95 rows kept at 1× now run **2× at 60** through the 2x rule: est(2×) ≤ 18.5 ms,
  e.g. the M4 Pro (16) at 16.3 ms and the M3 Max (30) at 9.4 ms. The 16-core M1 Pro and M2 Pro run 2× uncapped (21.1 /
  18.9 ms).

### Rule (2026-10-13; numbers superseded by the update above)

```
effective cores = gpu-core-count x generation factor x (0.75 on Ultra)      (same as §24)
est. mean GPU ms = fixed_ms(scale) + work(scale) / effective cores
frame target = 60 FPS if est. mean <= 18.5 ms, else 30 FPS
scale = the explicit resolution_scale / FABLE2_RES_SCALE (1, 2 or 3), or 1 when resolution is Auto
```

| Scale | fixed ms | work (ms × eff. cores) | 60 FPS needs |
|---|---|---|---|
| 1× | 4.7 | 153 | ≥ 11.1 effective cores |
| 2× | 3.0 | 339 | ≥ 21.9 |
| 3× | 0.4 | 655 | ≥ 36.2 |

**Where the model comes from.** It is a two-point fit per scale through the uncapped headings8 GPU means of the two
benchmarked Macs: the M5 Max (44 effective: 8.2 / 10.7 / 15.3 ms at 1×/2×/3×) and the M3 Pro (14 effective: 15.65 /
27.2 / 47.2 ms, the mean of its two reports). `fixed` is the part that does not shrink with more GPU cores. The
pure `44 / effective` scaling of §24 cannot be used for the mean at 1×: it predicts 25.8 ms for the M3 Pro, which
measured 15.7. `tests/res_auto_tests.cpp` checks that the fit reproduces both anchors within 0.2 ms.

**Threshold: 18.5 ms ≈ 54 FPS uncapped average**, in the middle of the 50–55 FPS range the maintainer approved. At the 60 cap
that is a near-60 with some dips, which plays better than a locked 30. The M3 Pro (15.6 ms estimate, 15.7
measured) clears it with room. The base M1 (7–8 cores × 0.80) is estimated at 28.6–32.0 ms, so it stays at 30. The
closest calls are the 14-core M1 Pro (18.4 ms → 60) and the 10-core M5 (18.6 ms → 30); both are estimates until
a report from those chips lands. Moving the threshold changes one number in both copies; the sync test catches a
miss.

### Order with resolution Auto

The frame target is decided first, then the scale.
- **Resolution Auto:** fps-auto estimates at 1×, because Auto never goes below 1×. Then res-auto (§24) picks
  the highest scale whose p95 row is met at that target.
- **Consistency:** res-auto's 60 FPS rows require the estimated p95 to fit 16.7 ms. That is stricter than
  fps-auto's 18.5 ms mean. So a Mac that gets 60 FPS never gets a scale at which fps-auto would have chosen 30. The
  unit test sweeps 4–96 cores for four chips to check this.
- **Code:** `fable2::res_auto::Resolve()` calls `fable2::fps::Use60()` before it picks, and the log shows
  `[fps-auto]` before `[res-auto]`.

**Explicit 2× or 3× set by hand + fps-auto: the estimate uses that scale.** That scale is what will run, so the
frame target should match it. Example: an M3 Pro at 2× by hand measured 36.8 FPS at the 60 cap; a steady 30 paces
better there than an uneven 35–40, so it gets 30. An M5 Max at 3× by hand (15.3 ms, 64 FPS uncapped, 59.4 at the cap)
keeps 60. The scale comes from `FABLE2_RES_SCALE` when it is a number, else `[graphics] resolution_scale`. Explicit
`fps_60 = true/false`, `--fps 30/60/uncapped` and turbo are unchanged and win over Auto.

### What it picked on 2026-10-13 (superseded by the update above)

Estimated mean GPU ms → frame target, at each scale (1× is also what resolution Auto uses):

| Mac (GPU cores) | Effective | 1× | 2× by hand | 3× by hand |
|---|---|---|---|---|
| M1 (7 / 8) | 5.6 / 6.4 | 32.0 / 28.6 ms → **30** | 30 | 30 |
| M1 Pro (14 / 16) | 11.2 / 12.8 | 18.4 / 16.7 → **60** | 30 | 30 |
| M1 Max (24 / 32) | 19.2 / 25.6 | 12.7 / 10.7 → 60 | 20.7 → 30 / 16.2 → 60 | 30 |
| M1 Ultra (48 / 64) | 28.8 / 38.4 | 60 | 60 | 23.1 → 30 / 17.5 → 60 |
| M2 (8 / 10) | 7.2 / 9.0 | 25.9 / 21.7 → 30 | 30 | 30 |
| M2 Pro (16 / 19) | 14.4 / 17.1 | 15.3 / 13.6 → 60 | 30 | 30 |
| M2 Max (30 / 38) | 27.0 / 34.2 | 60 | 60 | 24.7 / 19.6 → 30 |
| M2 Ultra (60 / 76) | 40.5 / 51.3 | 60 | 60 | 16.6 / 13.2 → 60 |
| M3 (8 / 10) | 8.0 / 10.0 | 23.8 / 20.0 → 30 | 30 | 30 |
| **M3 Pro (14)** — measured 15.7 / 27.0 / 47.2 ms | 14.0 | 15.6 → **60** (was 30) | 27.2 → 30 | 47.2 → 30 |
| M3 Pro (18) | 18.0 | 13.2 → 60 (was 30) | 21.8 → 30 | 30 |
| M3 Max (30 / 40) | 30.0 / 40.0 | 60 | 60 | 22.2 → 30 / 16.8 → 60 |
| M3 Ultra (60 / 80) | 45.0 / 60.0 | 60 | 60 | 60 |
| M4 (8 / 10) | 8.4 / 10.5 | 22.9 / 19.3 → 30 | 30 | 30 |
| M4 Pro (16 / 20) | 16.8 / 21.0 | 13.8 / 12.0 → 60 (was 30) | 23.2 / 19.1 → 30 | 30 |
| M4 Max (32 / 40) | 33.6 / 42.0 | 60 | 60 | 19.9 → 30 / 16.0 → 60 |
| M5 (8 / 10) | 8.8 / 11.0 | 22.1 / 18.6 → 30 | 30 | 30 |
| M5 Pro (16 / 20) | 17.6 / 22.0 | 60 | 22.3 → 30 / 18.4 → 60 | 30 |
| **M5 Max (32 / 40)** — measured 8.2 / 10.7 / 15.3 ms | 35.2 / 44.0 | 60 | 60 | 19.0 → 30 / 15.3 → 60 |

With resolution Auto too (frame target, then scale): M1 30 FPS 1×; M3 Pro (14 or 18) and M4 Pro (20) 60 FPS 1×.
Under the old rule the 18-core M3 Pro and the M4 Pro got 30 FPS at 2×. M1 Max (32) gets 60 FPS 1×, M3 Max (40) and
M5 Max (40) 60 FPS 2×, and M3 Ultra (80) 60 FPS 3×. Unknown core count: the tier's smallest bin (§24), so
base → 30 and Pro/Max/Ultra → 60. That replaces the old "60 for Max/Ultra names" fallback.

**What changed for players:** every Pro-class Mac below 24 cores (M1 Pro, M2 Pro, M3 Pro, M4 Pro, M5 Pro) moves from
30 to 60 at 1×. Base chips stay at 30 and Max/Ultra stay at 60. With an explicit 2× or 3×, the estimate at that
scale decides, so weaker Max/Ultra parts can get 30 there (e.g. M1 Max 24 at 2×) and a 20-core M5 Pro gets 60 at 2×.

### Switches, logs, launcher

- **Old rule:** `FABLE2_FPS_AUTO_MIN_CORES=<n>` switches back to the raw-core rule (`24` = exactly the old behavior,
  including the name fallback). It is env-only, a developer switch like the other fps-auto test overrides.
- **Test overrides:** `FABLE2_FPS_AUTO_CORES=<n>` (or `FABLE2_RES_AUTO_CORES`) fakes the core count;
  `FABLE2_RES_AUTO_NAME="Apple M3 Pro"` fakes the name (generation and tier).
- **Log**, one line after `[fps] fps_60=auto (source: …)`, e.g. on this M5 Max:
  `[fps-auto] 'Apple M5 Max' Max: 40 cores x 1.10 (M5) = 44.0 effective; est. 8.2 ms mean at 1x (60 needs <= 18.5) -> 60 FPS (scale from …)`.
- **Code:** `ChooseFrameTarget` and `kFrameModel` / `kFpsAutoMaxMeanMs` live in `src/core/fable2_res_auto.h`
  (pure). `src/core/fable2_fps_auto.cpp` reads the GPU and the planned scale. The Dev Launcher's `AutoFrameRate`
  mirrors the tables, and its label follows the Internal resolution picker. `tests/res_auto_sync_tests.py`
  checks both copies.

### Verification (M5 Max, 2026-10-13)

- `res_auto_tests` (the table above: every M1–M5 base/Pro/Max/Ultra bin at 1×, explicit 2×/3× cases, unknown core
  count, the two model anchors, both-auto cases and the 4–96-core consistency sweep) runs in every build and passes.
  `tests/res_auto_sync_tests.py` (`/usr/bin/python3` 3.9) passes. The Dev Launcher type-checks.
- One background test game, `f2diag run boot fpsauto-m5max --env FABLE2_FPS_60=auto --env FABLE2_RES_SCALE=auto`,
  ended with verdict ok and was the first launch after the rebuild. The log shows the order:
  `[fps] fps_60=auto` → `[fps-auto] 'Apple M5 Max' Max: 40 cores x 1.10 (M5) = 44.0 effective; est. 8.2 ms mean at
  1x (60 needs <= 18.5) -> 60 FPS (scale from resolution auto (decided after this))` → `[res-auto] … -> 2x … at 60
  FPS target` → `[res-scale] FABLE2_RES_SCALE=2`. One other agent game was running (contention; no perf numbers
  taken). Evidence: `out/validation/fps-auto-20261013/` (KEEP).

## 26. Native alias clears (2026-10-13)

The XDK `Clear` draws its rectangle on a 4×MSAA alias of the surface, and the emulation turned that into a 4× host
target, transfers both ways and one pass per clear. `FABLE2_ALIAS_CLEAR_NATIVE` (default on, launcher toggle) does it
as a resolve-style EDRAM clear of the surface's existing MSAA variant. The clear goes through the §14 deferred-clear
queue, and the XDK's no-op point draws are skipped. M5 Max GPU mean off → on:

- Bower: −1.41 / −1.79 / −3.05 ms at 1× / 2× / 3×;
- Old Tomb 2×: −0.69 ms;
- render encoders per frame at Bower 2×: 136 → 103.

M3 Pro estimate: about −4.5 ms at 2×. Details, exactness argument and remaining cases: `docs/alias-clear-20261013.md`.

## 27. Pass merge: boundary census, fewer main-pass splits (2026-10-13)

Census of why every render pass ends (Bower 2×: 104 render passes per frame; 73 end at a resolve, 11 at an RT change,
both forced; 7 separate clear passes; 6 texture-upload and 2 shared-memory-upload splits). `FABLE2_PASS_MERGE`
(default on, launcher toggles) keeps texture loads on the pre-commit route when their shared-memory upload is a CPU
direct write or a pre-commit copy (main-pass `fallback_upload` splits → 0; Bower 2× main-pass splits 4.3 → 2.9), and
draws a destination's deferred clear first in its transfer pass. M5 Max GPU time unchanged within noise; M3 Pro 2×
estimate −0.2 to −0.7 ms (bytes model). Tile-memory resolve costing (~1.5–2.4 ms M3 est., XL) and the remaining
items: `docs/pass-merge-20261013.md`.

## 28. Native main-depth alias clears (2026-10-14)

What alias clears (§26) left in the "main depth at 4×MSAA alias" role: the shadow-mask target initialised by a
z = 0.75 depth clear through a D24S8 4× alias, and two stencil-only clears of the main depth's first tile rows through
another one. `FABLE2_MAIN_DEPTH_NATIVE` (default on, launcher toggle) writes the depth word into existing word-exact
targets and does the stencil-only clears on the existing 1× D24S8 variant, so no 4× alias target is bound any more.
M5 Max Bower 2× GPU −0.47 ms; M3 Pro estimate about −1.7 to −2.2 ms at 2×. Details: `docs/main-depth-20261014.md`.

## Files touched

- `backends/metal/port/rex/graphics/metal/metal_command_processor.cc`: `FABLE2_RES_SCALE` experimental scale; trace line for skipped scaled morph readbacks.
- `backends/metal/port/rex/graphics/metal/metal_texture_cache.cc`: end the open encoders before the scaled-resolve grow blit.
- `tools/f2diag/run.py`: unexpected-exit verdicts, `run verdict`.
- `docs/resolution-scale.md` (this file), `docs/bug-patterns.md` (tools section), `docs/telemetry-design-20261008.md` (§15.6).

Restore point: `../restore-points/2026-10-09-resolution-step1-before.patch`.

Step 2:

- `backends/metal/port/rex/graphics/metal/metal_command_processor.cc/.h`: scaled morph readback, `tr_n_tex_ld_scl_*` counters.
- `backends/metal/port/rex/graphics/metal/metal_texture_cache.cc/.h`: scaled pre-commit route, scaled write record, verify.
- `backends/metal/port/rex/graphics/metal/metal_direct_host_resolve.cc`: record scaled resolve writes.
- `backends/metal/port/rex/graphics/metal/metal_shared_memory.cc/.h`: fur fixes default on, counters.
- `backends/metal/port/rex/ui/metal/metal_presenter.mm`: `[metalfx]` log lines.
- `src/core/fable2_graphics_config.h` (new), `src/core/fable2_config.cpp/.h`, `src/core/fable_2_app.h`.
- `config/fable2_config.toml`, `tests/config_defaults_tests.cpp`.
- `tools/macos/DevLauncher/main.swift`.
- `README.md`, `docs/resolution-scale.md`, `docs/bug-patterns.md`, the 2026-10-07 playtest notes.

Retina check (§9): `backends/metal/port/rex/ui/metal/metal_presenter.mm/.h`, `docs/resolution-scale.md`,
`docs/bug-patterns.md`. Restore point: `../restore-points/2026-10-09-retina-before.patch`.

Display options (§10): `patches/sdk/window-initial-position.patch`, `src/core/fable2_window_position.cpp/.h`,
`src/core/fable2_graphics_config.h`, `src/core/fable2_config.cpp/.h`, `src/core/fable_2_app.h`,
`config/fable2_config.toml`, `tests/config_defaults_tests.cpp`, `tests/window_position_tests.cpp`,
`backends/metal/port/rex/ui/metal/metal_test_control.mm`, `tools/fable2_dev_session.py`,
`tests/dev_session_trace_options_tests.py`, `tools/macos/DevLauncher/main.swift`, `tools/macos/AppLauncher.m`,
`run_macos.command`, `run_macos_metal.command`, `README.md`, `docs/bug-patterns.md`, `docs/resolution-scale.md`.
Restore point: `../restore-points/2026-10-09-display-options-before.patch`.

Upscaling (§22): `backends/metal/port/rex/ui/metal/metal_presenter.mm`, `src/core/fable2_config.cpp/.h`,
`src/core/fable2_graphics_config.h`, `src/core/fable_2_app.h`, `config/fable2_config.toml`,
`tests/config_defaults_tests.cpp`, `tools/f2diag/gpu.py`, `tools/fable2_instances.py`,
`tools/macos/DevLauncher/main.swift`, `README.md`, `docs/bug-patterns.md`, `docs/engine-options-20261011.md`,
`docs/resolution-scale.md`. Restore point: `../restore-points/2026-10-12-metalfx-impl.patch`.

Texture-space passes (§23): `backends/metal/port/rex/graphics/metal/metal_render_target_cache.cc/.h`,
`metal_command_processor.cc/.h`, `metal_shared_memory.h`, `tools/macos/DevLauncher/main.swift`,
`docs/resolution-scale.md`, `docs/bug-patterns.md`. Restore points:
`../restore-points/2026-10-12-dogseam-{before-fix,morphnative-attempt}.patch`.
