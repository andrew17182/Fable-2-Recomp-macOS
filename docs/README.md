# Documentation index

Start with the [README](../README.md). Dated docs (`name-YYYYMMDD.md`) are investigation logs: the top of each says
what was decided, the rest is the evidence. Finished, superseded investigations are in [archive/](archive/).

## Players

| Doc | What |
|---|---|
| [setup-app.md](setup-app.md) | First-run setup (`Install.command`) |
| [player-app.md](player-app.md) | The app's launcher window (Play and Settings) |
| [release-app.md](release-app.md) | The public release: one `Fable II.app` that builds the game on the player's Mac (packager, first-launch build, Start Fresh, measurements) |
| [portable-build.md](portable-build.md) | Zippable app for another Mac (no-converter release build; asks for the disc image on first launch) |
| [turbo.md](turbo.md) | Turbo mode (F7) |
| [reference.md](reference.md) | Full reference: config, controls, F5/F6 Lua, tracing, Windows/Vulkan notes |

## Building

| Doc | What |
|---|---|
| [macos.md](macos.md) | Native macOS build |
| [macos-metal.md](macos-metal.md) | Metal backend: build, run, tests |
| [sdk-from-source.md](sdk-from-source.md) | ReXGlue SDK built from source, `patches/sdk/` |
| [patches.md](patches.md) | Guest-image patch system (`config/fable2_patches.toml`) |
| [hotfunc_overrides.md](hotfunc_overrides.md) | Strong-symbol copies of recompiled guest functions (upstream) |
| [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) | Third-party components and licences |

## Renderer and performance

| Doc | What |
|---|---|
| [resolution-scale.md](resolution-scale.md) | 2×/3× internal resolution, auto resolution, MetalFX |
| [native-shaders.md](native-shaders.md) | Optimized guest shaders on Metal, shader lab |
| [native-translator.md](native-translator.md) | Xenos → MSL translator, native tessellation, points and rectangles |
| [no-converter.md](no-converter.md) | Native shaders from the first use (no DXIL / Metal Shader Converter at run time); `FABLE2_NO_CONVERTER` build without either library (what releases ship) |
| [pass-merge-20261013.md](pass-merge-20261013.md) | Fewer render-pass breaks |
| [alias-clear-20261013.md](alias-clear-20261013.md) | Native alias clears |
| [tile-resolve-20261013.md](tile-resolve-20261013.md) | Resolves inside the draw pass (default on since 2026-10-13) |
| [pale-root-20261014.md](pale-root-20261014.md) | Bower Lake pale root: game behaviour (texture-size cache filled from the shadow view, load-order race); opt-in fix `FABLE2_FIX_LOD_SIZE_CACHE` (default off) |
| [lake-water-20261014.md](lake-water-20261014.md) | Bower Lake water flat in straight-edged patches at 2×/3×: 16 KB host pages marked valid by scaled resolves without upload; fix `FABLE2_SCALED_RESOLVE_PAGE_FILL` (default on) |
| [gpucmd-cpu-20261014.md](gpucmd-cpu-20261014.md) | GPU Commands thread CPU round 2: texture key memo and per-draw trims (town 2× −13% thread time, +16% FPS; default on) |
| [gpucmd-cpu3-20261014.md](gpucmd-cpu3-20261014.md) | GPU Commands thread CPU round 3: load-check reuse, texture find / sampler memos, packing trims (town 2× −12% thread time, +14% FPS; town 1× +15%; default on); encoder-worker design note |
| [gpucmd-cpu4-20261014.md](gpucmd-cpu4-20261014.md) | GPU Commands thread CPU round 4: "no load, then a load" was a race with guest CPU writes (`FABLE2_LOAD_CHECK_EXACT`, default on, exact, verify + negative control); inline `rex::bit_scan_forward` (SDK patch, −1.1 to −1.4% thread time); draws per flush / flushes per frame and the encoder-worker no-go |
| [tex-rename-20261013.md](tex-rename-20261013.md) | Texture renaming: reloads of sampled textures no longer split the main pass (default on since 2026-10-14) |
| [mip-lod-20261013.md](mip-lod-20261013.md) | Guest mip range on the host; mip-chain resolves inside the draw pass via a per-draw LOD bound (default on) |
| [hle-census-20261013.md](hle-census-20261013.md) | HLE census of draws, passes and EDRAM use |
| [frame-pacing-20261012.md](frame-pacing-20261012.md) | Frame pacing and hitches |
| [prologue-stutter-20261014.md](prologue-stutter-20261014.md) | New-game stutter: native geometry pipelines persisted (`FABLE2_NATIVE_GEOM_PERSIST`); per-slot bundle ids for agent games |
| [engine-options-20261011.md](engine-options-20261011.md) | Engine options for 3× at 60 FPS and base Macs |
| [weak-mac-perf-20261008.md](weak-mac-perf-20261008.md) | Performance on smaller Macs (M3 Pro target) |
| [benchmark-kit.md](benchmark-kit.md) | Benchmark kit for other Macs |
| [FPS_CAP_INVESTIGATION.md](FPS_CAP_INVESTIGATION.md) | 30 FPS cap investigation (upstream) |

## Debugging and testing

| Doc | What |
|---|---|
| [bug-patterns.md](bug-patterns.md) | Bug patterns and triage playbook (read first for any bug) |
| [cr-fixes-20261007.md](cr-fixes-20261007.md) | Code-review fixes CR-01..CR-08 (switches still in code) |
| [telemetry-design-20261008.md](telemetry-design-20261008.md) | Flight recorder and `f2diag` (§15 is the reference) |
| [readiness.md](readiness.md) | Readiness signals; test plans start when the game is ready |
| [boot-hang-20261008.md](boot-hang-20261008.md) | Intermittent pre-title boot hang |
| [main_menu_crash_fix.md](main_menu_crash_fix.md) | D3D12/Vulkan main-menu crash (upstream) |

## Save states (paused)

Runtime save states via continuation codegen are paused. That work (code, manifests and its docs) is kept outside
the public tree.

## Archive

[archive/](archive/): save-load and area-load profiles (2026-10-06/08),
uncapped headroom on an M3 Pro (2026-10-07, superseded by weak-mac-perf), GPU Commands thread CPU (2026-10-12).

## Running the tests

```sh
for t in tests/*.py backends/metal/tests/*.py; do /usr/bin/python3 "$t" || echo "FAILED $t"; done
```

The C++ unit tests (config, fpscr, fctiw, flight recorder, resolution auto, window, input, clock) run as POST_BUILD
steps of every game build. `tests/macos_app_tests.py` is an opt-in live check (arguments in macos-metal.md).
