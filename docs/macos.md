# Native macOS build

The standard build script defaults to native Metal on Apple Silicon. This is the
primary development and testing backend. Intel Macs retain Vulkan/MoltenVK.
Set `FABLE2_MACOS_BACKEND=vulkan` to build or launch Vulkan explicitly.
See [Metal runtime details](macos-metal.md) for prerequisites and limitations.

## Build and launch

The easiest route is `Install.command` in the project root: it checks the
prerequisites, extracts the game files from your disc image and runs the build
below with a progress window. See [First-run setup](setup-app.md).

To build by hand, install Xcode with its Metal Toolchain component
(`xcodebuild -downloadComponent MetalToolchain`), CMake 3.25 or later, and
Python 3. The SDK requires a C++23-capable Clang toolchain; Apple Clang 21 was
used for testing. Supply your own disc content as described in the
[README](../README.md#getting-started): `default.xex`, `data/`,
`$SystemUpdate/`, and `nxeart` belong in the project root. `nxeart` may be a
file rather than a directory. Both build scripts check for these files and the
required tools before downloading anything. A CMake downloaded by the setup app
(`out/tools/cmake/`) is used first when present. Paths containing spaces work.

```sh
./tools/build_macos.sh
open 'out/build/mac-arm64-metal-release/Fable II.app'
```

On Apple Silicon, a successful standard script build produces
`out/build/mac-arm64-metal-release/Fable II.app`. Double-click it to play, or
move it to Applications. Rebuilding refreshes the app and preserves external
saves and settings. Vulkan remains available with:

```sh
FABLE2_MACOS_BACKEND=vulkan ./tools/build_macos.sh
open 'out/build/mac-arm64-release/Fable II.app'
```

Direct CMake builds still produce the shared game executable and Vulkan app;
the build script additionally builds and packages the Metal renderer. Intel
uses `mac-amd64-release` and has not been runtime-tested.

App saves/settings/logs are under `~/Library/Application Support/Fable II Recomp`.
The command launcher remains available with its separate per-build state folders.

The build script downloads and verifies the matching SDK archive, creates
the missing `generated/rexglue.cmake` integration file with the SDK, generates
guest code from the project's manifest, and builds Release using Make.
Set `FABLE2_BUILD_JOBS` to change the default of four compiler jobs.
`REXGLUE_SDK_ROOT` can select an existing compatible SDK installation.
Downloaded Mac SDK binaries live in `out/sdk/mac-arm64/` (or `mac-amd64/`),
separate from upstream's tracked `thirdparty/rexglue-sdk` source submodule.
Additional arguments to the build script are passed to CMake configuration.

Only the generated function-registration file uses `-O0` and skips the
precompiled header. Its single function registers roughly 61,000 guest entry
points and otherwise takes Clang many minutes to process. On macOS it also
uses `-fno-global-isel` to avoid a slow Apple Silicon instruction-selection
pass. The game functions retain Release optimization. Mac Release builds omit
per-function tracing checks by default; see the diagnostics options below.

The launcher can also be double-clicked in Finder. It starts in a window;
`./run_macos.command --fullscreen=true` overrides that default. For Vulkan, saves, cache,
configuration, and logs live beside the executable in
`out/build/mac-arm64-release/` on Apple Silicon. Disc content is loaded from
the project root without copying it beside the executable.

Player saves and profiles live in the runtime's `saves/` folder. This folder
is ignored by Git and must not be included in contributions.

The window remembers its last position in `fable2_window.toml` beside the
executable. Moving it saves the position immediately, including when the SDK
exits without normal cleanup. Fullscreen, maximized, and minimized positions
do not replace the saved windowed position. Window size still follows the
existing settings. An explicit monitor selection takes priority; a saved
position outside the connected displays falls back to normal placement.
Delete `fable2_window.toml` while the game is closed to reset the position.

### Test-window placement

Dev Launcher play sessions copy the live `fable2_window.toml` and write a moved
window back to it when the game exits. Agent/test games keep their own placement
instead (`tools/fable2_test_windows.py`), so moving a test window never moves the
play window and the next test window opens where you left the last one:

- Covers every test launch: `fable2_test_control.py launch` and the MCP `launch`,
  `fable2_dev_session.py launch --test-control` (so `f2diag run ...` and the
  portable warm-up), `fable2_cache_warm.py`, and `fable2_benchmark.py run`
  (the Dev Launcher's Run Benchmark).
- Store: `<main checkout>/out/instances/test-windows/<key>.toml`, shared by the
  main checkout and every agent slot. Key `<checkout>-<n>`: `main` or `agent-N`,
  plus the concurrent index `n` (the 1st, 2nd... test game running at the same
  time from that checkout, claimed in `claims/` per game pid). Two test games
  running together therefore never share a position.
- Default when a key has no stored placement: the game's default size at a
  cascaded position, `k = N * 2 + (n - 1)` (main = 0), `x = 60 + 90k`,
  `y = 60 + 50k` (points, main display).
- Write-back when the game exits, including a crash or a hand close: the
  session file must parse as exactly `x`/`y` (+ `width`/`height`), an unmoved
  window writes nothing, a store entry changed since launch is kept, and the
  file is replaced atomically. The result is logged in the session's
  `writeback.jsonl` (dev sessions), `test-window.jsonl` (test-control sessions).
- Size is remembered too, except for launches that fix it with
  `--window_width`/`--window_height` and benchmark runs: those seed and write
  back only the position (the stored size is kept), so benchmark runs always use
  the game's default window size.
- `FABLE2_TEST_WINDOW_MEMORY=0` (launching environment, or
  `out/test-control-env.json` for MCP launches) turns it off: old behaviour.
  `python3 tools/fable2_test_windows.py list` shows the stored placements and
  live claims; delete a `<key>.toml` to reset that key.

Code generation reserves the Xbox 360 guest address space. If a restricted
environment reports `Unable to reserve the 4gb guest address space`, run the
build from an ordinary Terminal session.

## Keyboard and mouse

The cursor stays free by default. Set `[input] mouse_look = true` in
`fable2_config.toml` to opt into captured mouse camera control; controller
camera input works independently. The Mac driver uses SDK window events and,
when enabled, relative mouse input. It
preserves the existing configurable gamepad mappings and physical controller
support. Input is cleared on focus loss and blocked while an overlay captures
input. Closing the window releases mouse capture.

| Control | Default action |
| --- | --- |
| W / A / S / D | Movement |
| Mouse motion | Camera only when mouse-look is enabled |
| E | A / confirm |
| Z / X / C | X / B / Y |
| Q / Tab | Left / right trigger |
| Escape / M | Start / Back |
| Arrow keys | D-pad |
| 3 | Debug overlay |
| 7 | Achievements |
| 4 | Settings / release mouse capture; excluded from gamepad input |
| 5 | Queue the existing external Lua runner on the guest thread |

Backtick opens the console. Shortcuts use the number row, not the numeric keypad. Mouse buttons can be
mapped but have no default gamepad binding.

Edit `[input]` in `fable2_config.toml` beside the executable for
`keyboard_gamepad_map`, `mouse_look`, and `mouse_look_scale`. Host key names
are case-sensitive: `Space`, `Shift`, `Control`, `Alt`, `LMB`, `RMB`, and
`MMB` are examples. The SDK combines left and right modifier keys.

## Terrain correction and SDK builds

ReXGlue 0.10.0's Vulkan tessellation helpers request system constants from
descriptor set 0, which contains guest memory. The constants actually occupy
set 1. In the first playable Fable II scene after character selection, this
read zero tessellation limits and removed the ground. Correcting the binding
restored the ground in an Apple M3 Pro test.

For the prebuilt Apple Silicon SDK, the build script and launcher run
`tools/macos_terrain_fix.py` on the staged `librexgpu-xenos.dylib`. It accepts
only the original verified 0.10.0 library or the verified corrected library,
changes the embedded shader declaration, signs and checks a temporary copy,
then replaces the runtime copy atomically. An already-corrected library is
left alone. The installed SDK library is not modified. This workaround does
not apply to Intel Mac, Windows, or Linux binaries.

For the legacy pinned SDK source, the equivalent optional correction is
included in `thirdparty/rexglue-sdk-local.patch`. The legacy
`tools/setup_sdk_src.cmd` applies it to its separate `rexglue-sdk-src` tree.
Current upstream Windows builds use the `thirdparty/rexglue-sdk` submodule;
the legacy patch does not automatically modify that submodule. The Mac
prebuilt-library correction above is independent of both source layouts.
The new `vulkan_tessellation_constants_fix` SDK setting requires a restart:

| Renderer/platform | Default with the source patch | Override |
| --- | --- | --- |
| macOS Vulkan | Enabled | `--vulkan_tessellation_constants_fix=false` |
| Windows/Linux Vulkan | Disabled | `--vulkan_tessellation_constants_fix=true` |
| Direct3D 12 | Unaffected | None |

The source setting requires a rebuilt SDK and is not added to prebuilt
libraries by the binary helper. When using a custom SDK that already fixes
the shader, set `FABLE2_MACOS_TERRAIN_FIX=0` for both build and launch to skip
the version-specific binary helper. With the original prebuilt SDK, skipping
the helper leaves the terrain bug in an uncorrected runtime library.

## Performance and diagnostics

Mac Release builds default to `FABLE2_ENABLE_FUNC_TRACE=OFF`. This removes the
trace-enable check from generated guest function entries and the per-frame
trace-window clock/environment check. It preserves the original guest-function
prologue and the functional F5 and game-patch hooks. Enable tracing support
with `./tools/build_macos.sh -DFABLE2_ENABLE_FUNC_TRACE=ON`, then use the usual
`FABLE2_FUNC_TRACE=1` or trace-window controls when launching. Mac Debug and
other-platform builds keep tracing support enabled by default.

The allocator-watch diagnostic is excluded on macOS because its Windows/Linux
memory queries cannot inspect the Mac guest arena. The game allocator itself
is unchanged. The Mac launcher suppresses the repeated Vulkan debug callback
and MoltenVK warning stream while preserving normal runtime errors. Restore
verbose graphics diagnostics for a run with:

```sh
REX_VULKAN_LOG_DEBUG_MESSAGES=1 MVK_CONFIG_LOG_LEVEL=3 FABLE2_MACOS_BACKEND=vulkan ./run_macos.command
```

These settings reduce diagnostic overhead without changing graphics quality or
game timing. On macOS, `FABLE2_FPS_METER=1` logs per-thread guest render-loop
cadence. It is off by default and does not measure displayed frames.

### Guest memory and process exit

The 4.5 GB guest address space is one POSIX shared-memory object (`shm_open`, then `MAP_SHARED`
views). The SDK used to remove its name only in `Memory`'s destructor. The game never runs that
destructor: it hard-exits at the title, and the test tools terminate it. So every exit left the
object, with every guest page the game had touched, in the kernel with no owner until reboot.
It showed up as free memory dropping ~1.3 GB per short boot, then as compressor and swap growth
(40+ GB) with no visible process. `patches/sdk/shm-unlink-on-create.patch` removes the name right
after creating the object. The fd and the views keep it alive, and the kernel frees it when the
process ends, however it ends. Objects leaked before the fix stay until a reboot: their names
(`/xenia_memory_<host tick>`) cannot be listed. The prebuilt SDK (`FABLE2_SDK_FROM_SOURCE=0`)
still leaks. `FABLE2_SHM_UNLINK_EARLY=0` brings back the old behaviour (diagnostic only).

## Tests and current limits

```sh
./tools/build_macos.sh -DFABLE2_BUILD_INPUT_TESTS=ON
cmake --build out/build/mac-arm64-release
ctest --test-dir out/build/mac-arm64-release --output-on-failure
python3 tests/macos_terrain_fix_tests.py
```

Input tests exercise the SDK event dispatcher without a desktop window,
including mappings, relative motion, focus/overlay transitions, modifier
releases, F5 requests, concurrent consumption, and deferred teardown. The
Python tests verify that the binary helper preserves the original file on
validation/signing failures and leaves an already-corrected file untouched.
Window tests verify saving/restoring positions, fullscreen filtering, monitor
override, unchanged window size, and invalid or unwritable state files. They
create hidden Cocoa windows and require a normal macOS desktop session; the
prebuilt SDK has no dummy video driver. Run
`out/build/mac-arm64-release/fable_2_window_tests --desktop` to repeat the
position round trip with a visible test window (without entering fullscreen).

Windows-only diagnostic probes are disabled outside Windows while their
normal guest-function forwarding remains. No measured frame-rate improvement
is claimed from the diagnostic cleanup.
Windows, Linux, and Intel Mac builds, full gameplay, save/load, and execution
of F5 Lua scripts during a Mac playthrough still require validation.

## Supported macOS versions

We design and build with the current macOS and Xcode. The game targets macOS 15.0 as its
deployment target (the game executable and the Metal plugin), so a build made on a newer macOS
also starts on Sequoia, but older versions are **not** a maintained compatibility target and are
not tested. We only look at older macOS versions if we have to. Building on the Mac you play on
(Install.command) always works on that Mac's own version.
