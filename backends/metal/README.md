# Metal backend for Fable II

This plugin adapts XeniOS's Metal backend to the ReXGlue 0.10.0 plugin ABI
on Apple Silicon. The default Apple Silicon build does
not replace the installed SDK or change Windows/Linux renderer defaults.

From the project root, after supplying your own game content:

```sh
python3 tools/build_macos_metal.py
open 'out/build/mac-arm64-metal-release/Fable II.app'
```

Use Python 3.9+ (Xcode's python3 works), CMake 3.25+, Xcode with the Metal Toolchain component and a
C++23 compiler. The backend targets macOS 15 or later; the complete app may
require a newer macOS version depending on the game and SDK deployment targets.
The build downloads checksummed archives pinned in `dependencies.json`, applies
`sdk-metal.patch` to a private SDK source tree and `dxilconv-build.patch` to the
pinned DXIL converter, compiles 185 built-in helper shaders, and builds the plugin
with CMake. The game executable is built through the normal project targets:
guest code is generated locally from the supplied game content.

`--cache PATH` selects an archive cache (every input is hash-checked even on reuse).
`--jobs N` controls backend/dependency compilation; `FABLE2_BUILD_JOBS` controls
the game compilation too. `--prepare-only` checks/extracts dependencies and
applies patches. `--backend-only` skips game compilation and runtime staging,
requiring an already installed SDK. `REXGLUE_SDK_ROOT` selects that SDK.
The default Mac binary SDK location is `out/sdk/mac-arm64/`, outside the
upstream Windows SDK source submodule.

Outputs live under ignored `out/metal-build/` and
`out/build/mac-arm64-metal-release/`. Existing configuration and saves are not
replaced. After modifying a dependency patch, remove only its corresponding
source directory in `out/metal-build/dependencies/` and rerun preparation.
The Metal helper headers are compiled from upstream built-in shaders; guest
shader caches and generated game code are not included in this repository.

## Source and license boundaries

- ReXGlue: `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2` (SDK 0.10.0).
- XeniOS: `87b176a078c316fde3adf67a217f0c44615b0e0d`.
- `port/` is the adapted XeniOS source snapshot; original notices are retained.
- `sdk-metal.patch` includes the complete SDK adaptation and optional register
  batching. It applies only to the private graphics source compiled into this
  plugin; the installed runtime library is unchanged.
- `licenses/` retains the ReXGlue and XeniOS license texts, and the MIT licence of Microsoft's D3D11
  tessellator reference (`licenses/D3D11-Tessellator-LICENSE`; the code is vendored in
  `third_party/d3d11-tessellator/`, see its README for source and commit).
- `dxilconv-build.patch` modifies build files from the pinned
  [DirectXShaderCompiler fork](https://github.com/xenios-jp/DirectXShaderCompiler/tree/55db9bc0bc574816d60073872b6a209f7c3d7f0a).
  Its LLVM University of Illinois/NCSA license and complete third-party notices
  are retained verbatim in `licenses/DirectXShaderCompiler-LICENSE.txt` and
  `licenses/DirectXShaderCompiler-ThirdPartyNotices.txt`. These apply separately
  from the XeniOS license; including them does not resolve binary-distribution
  requirements for the complete app.
- Dependencies retain their own terms, including Apple's Metal shader converter;
  no dependency binaries are committed here. Review their licenses before
  redistributing a built runtime.
- The selective morph readback criteria reference
  [Femtofork for Fable II](https://github.com/just-harry/unofficial-xenia-femtofork-for-fable-ii).

## Known limitations

Native resolution only for morph readback, game-version-specific destination
`0x12704000`, first-use shader compilation may pause rendering, and an existing
geometry-pipeline failure is retried with backoff. The startup handoff is an
optional narrowly scoped game workaround, not a general SDK locking fix.
See [runtime configuration and limitations](../../docs/macos-metal.md).
Metal rendering is not supported on Intel Macs.

The GPU command worker submits pending work asynchronously when entering an idle
or guest polling wait. Explicit readback and shutdown waits are retained. For
diagnosis or performance comparisons, `--metal_async_idle_submission=false`
restores the previous blocking idle behavior.

Background shader compilation remains enabled. With morph readback enabled,
draws using Fable II's morph pixel shader (`E2120FEAFA4CF565`) wait for their
pipeline instead of being skipped. Skipping those draws can leave red pixels
baked into the generated texture. Other pending scene draws retain asynchronous
skipping to avoid compilation stalls. This shader selection is game-version
specific, like the readback address; failures complete the wait with an error.

For GPU capture diagnosis, `--metal_capture_labels=true` labels guest textures
with their base/mip addresses and marks draws with their guest shader hashes.
This option is off by default and does not enable capture itself.

To inspect a CPU texture upload, set `--metal_texture_dump_base=ADDRESS` and
`--metal_texture_dump_prefix=/existing/directory/texture`. Matching uploads write
their staged base/mip bytes to numbered binary files and log their format. Both
options are disabled by default; this does not wait for the GPU. Dumps contain
game data and must remain outside source control.

Addresses are decimal and may change between saves/builds. To find an atlas
without guessing its address, `--metal_texture_dump_width=512` selects unique
square textures of that width. `--metal_texture_dump_limit=128` bounds selected
uploads in the process. Both address and width selection are off by default.
Once identified, `--metal_draw_trace_texture_base=ADDRESS` logs up to 32 distinct
draw binding, guest sampler and depth-state combinations using that texture.
This trace is also off by default and changes no rendering settings.


## Tests

From the repository root:

```sh
python3 backends/metal/tests/type0_batching_test.py
python3 backends/metal/tests/morph_readback_test.py
python3 backends/metal/tests/shader_storage_test.py
python3 backends/metal/tests/pipeline_wait_test.py
python3 backends/metal/tests/pipeline_priority_test.py
python3 tests/frame_log_summary_tests.py
python3 tests/app_test_bridge_tests.py
python3 backends/metal/tests/packed_texture_format_test.py  # Requires a Metal GPU.
clang++ -std=c++23 -I backends/metal/port \
  backends/metal/tests/pipeline_failure_backoff_test.cpp -o /tmp/fable-metal-backoff-test
/tmp/fable-metal-backoff-test
```

These exercise packet batching, readback completion/bounds handling with test
GPU/memory objects, and failed-pipeline retry timing. They do not replace GPU
startup and gameplay checks.
