# First-run setup (Install.command)

`Install.command` (project root) turns a fresh copy of this project plus your own Fable II
disc image into a playable build, without the command line.

1. It checks for Apple Silicon and the Xcode command-line tools (offering
   `xcode-select --install` if they are missing), compiles the setup app from
   `tools/macos/Setup/*.swift` into `out/setup/Fable II Setup.app` and opens it. The app is
   rebuilt only when its sources change.
2. **Fable II Setup** walks through:
   - **Prerequisites:** Xcode with its Metal Toolchain component (the Metal shader compiler is
     not part of the command-line tools; the app offers `xcodebuild -downloadComponent
     MetalToolchain`), CMake 3.25+, Python 3.9+, curl/ditto/shasum/patch/tar and free disk
     space (about 7 GB of game files plus about 6 GB for the first build). If CMake is missing
     or too old, it downloads the official CMake 4.1.2 macOS archive into `out/tools/cmake`
     (SHA-256 checked; no Homebrew or admin rights). The build scripts use that copy first
     whenever it exists. If `xcode-select` points at the command-line tools but Xcode is
     installed, the app sets `DEVELOPER_DIR` for its child processes instead of asking for an
     admin password.
   - **Disc image:** pick the `.iso`. The app reads the disc's file table at once (and rejects
     images without `default.xex`/`data`), then computes its SHA-256 with a progress bar.
     Known images: the Platinum Hits image used to develop and test this port (`2cdaafea…`), and the
     GOTY image upstream's README names (`685a0d3b…`, not tested on this port). Other images show a warning
     but can continue.
     The hash is cached in `out/setup/state.json` (keyed by path, size and date).
   - **Extract:** copies `default.xex`, `data/`, `$SystemUpdate/` and `nxeart` into the project
     root (all gitignored; no other path is ever written). Files already present with the
     right size are kept, so an interrupted extraction resumes. `data/dir.manifest` counts as
     present once it exists because the build appends its Lua helpers to it.
   - **Build:** runs `python3 tools/build_macos_metal.py` with `FABLE2_BUILD_JOBS` set to the
     CPU count. Stage names come from `[fable2-build] stage=<key>: …` lines printed by the build
     scripts; make's `[ NN%]` lines drive the progress bar. The full output goes to
     `out/setup/build-<timestamp>.log`. On failure the window shows the last error lines with
     **Copy log** / **Reveal log**. The first build also compiles the ReXGlue SDK from source
     (stage `sdksrc`, `docs/sdk-from-source.md`): ~75 s with 12 jobs, ~90 s with 4, 58 MB more
     download, ~1 GB more disk in `out/sdk-src`. Measured on a fresh copy (18 jobs, 2026-10-10):
     deps 20 s, sdksrc 68 s, code generation 25 s, game 61 s, shader compiler 37 s, renderer 15 s,
     packaging 2 s; 3 min 52 s for the whole setup. Later builds reuse it until an SDK input changes.
   - **Launcher:** `tools/macos/build_dev_launcher.sh` (without `--install`), which builds
     into `out/dev-launcher/`.
   - **Place the apps:** copies `Fable II.app` (from `out/build/mac-arm64-metal-release/`) and
     `Fable II Launcher.app` (from `out/dev-launcher/`) into the project folder, next to
     `Install.command`, with `ditto` under a temporary name that is then swapped in. Both
     copies are gitignored. The game app is movable (its game folder is an absolute path in
     `Contents/Resources/LocalDefaults.plist`) and the launcher finds the project through its
     `Fable2RepoRoot` Info.plist key. The builds in `out/` stay where they are;
     `tools/fable2_dev_session.py` and the test tools keep using them.
   - **Shaders** (optional, on by default; checkbox on the disc page, `--no-warm-shaders`): one background run of
     the new game build prepares the shader cache. See "Shader warm-up" below.
   - **Play:** buttons for the two copies in the project folder. Nothing is copied to
     /Applications or outside the project folder.
3. Running it again skips finished steps (`out/setup/state.json`); **Rebuild** forces the build
   and launcher steps. Every run refreshes the project-folder copies when they are missing or
   older than the builds in `out/`. The normal dev commands (`python3 tools/build_macos_metal.py`,
   `build_dev_launcher.sh --install`) do not touch those copies; run Install.command again to
   refresh them.

## Shader warm-up

(2026-10-14: the portable player app instead runs a headless shader preparation after extraction, with no game
session; `docs/offline-shader-seed.md` compares the two. Install.command still uses the session below.)

`tools/fable2_cache_warm.py --new-game`, run by the setup app after the apps are placed (about 3 minutes with the 2× and 3× sessions; never fatal:
on failure the step shows a warning and the game compiles those shaders during play as before).

- It starts one agent-style test session of `out/build/…/Fable II.app` (`fable2_test_control.launch`): a background
  window that never takes focus, muted, physical input off, its own clone of the state folder's saves, settings and
  cache, closed by its own session id. The window is visible behind the front app.
- It waits for the title screen, presses A, A (New Game), Left + A (hero card), then Start to skip the opening movie
  into the in-engine prologue (young Sparrow in Bowerstone Slums), and presses A every 8 s for 45 s. With a save in
  the state folder the first menu entry is Continue, so the newest save loads instead.
- Pipelines are compiled before their first draw (`--async_shader_compilation=false`): a cold cache otherwise skips
  about 110,000 draws in that session (blue/white flashing in its window, `docs/bug-patterns.md` row 3).
- It waits until the optimizer pool has been quiet for 10 s, closes the game, and publishes the session's grown cache
  into the state folder with the launcher's append-only write-back (`fable2_dev_session.publish_cache`; a backup of
  the previous cache goes to `out/save-backups/cache-previous`).
- Then one short title-screen session per other resolution scale (`--scales 1,2,3` default; the first session runs
  at the player's own scale): about 28 s each on an M5 Max, +17 MB of cache. The first launch at 2× or 3× then
  prepares in 0.16 s instead of 1–1.5 s (`docs/native-shaders.md`, "First launch").
- The cache is compiled on the player's Mac from their own game files and lives only in
  `~/Library/Application Support/Fable II Recomp/cache`. Nothing game-derived is shipped or committed.
- It refuses while a normal (non-test) Fable II game is open. It runs once per setup (`shadersWarmed` in
  `out/setup/state.json`); **Rebuild** runs it again. Since the shader cache is keyed by a pinned converter target
  (`docs/native-shaders.md`, "Converter target pinned by default"), it stays valid across macOS updates.
- Tests: `--state-root DIR` (tool) / `--warm-state-root DIR` (setup app) warm a scratch folder instead.

Coverage (M5 Max, empty cache, 1×, 2026-10-12, `out/validation/cachepin-20261012/warm-*`):

| Session | Translations | Optimized pixel shaders | Optimizer CPU | Cache |
|---|---|---|---|---|
| Title screen only | 32 | 18 | 0.6 s | 1.2 MB |
| Title + new game, ~45 s of prologue (default) | 163–175 | 83–99 | 5–8 s | 10–12 MB |
| For scale: a played cache (the maintainer's, 1×/2×/3×) | 838 | 700 (183 per scale in Bower Lake) | | 98 MB store |

The title alone is too little to matter. The new game covers what a new player meets first. The same new-game flow
as a player would see it (normal asynchronous pipeline creation, `FABLE2_DRAW_SKIP_TRACE=1`):

| Cache | Draws skipped while compiling (`ds_async_compiling`) | Gameplay stalls from draw-path builds (`pso_*`/`xlate` in `[hitch]`) |
|---|---|---|
| Empty (no warm-up) | 112,088 in 239 frames: blue/white flashing | 8, worst 190 ms (a probe run: 1.26 s) |
| After the warm-up | 0 | 0 (only loading-screen frames) |

## Extractor

`tools/macos/Setup/GDFX.swift` is a Swift port of the GDFX (Xbox 360 disc) reader in upstream's
`tools/x360extract` (himdo/Fable-2-Recomp, "made a custom xex extractor to help with first
time running of the game"; that tool is C#/.NET, which a fresh Mac does not have). It tries the
known XGD2/XGD3 header offsets before scanning, follows directories across sector batches, and
copies files byte for byte (STFS packages such as `nxeart` stay raw files, as the game expects).
List an image without extracting:

```sh
"out/setup/Fable II Setup.app/Contents/MacOS/FableIISetup" --list "Fable II.iso"
```

## Unattended runs (testing)

```sh
./Install.command --iso "path/to/Fable II.iso" --auto [--extract-only] [--rebuild] [--clean-env] [--no-warm-shaders] [--warm-state-root DIR]
```

`--auto` drives the same window and code as the buttons, mirrors progress to stdout and exits
0/1. `--no-warm-shaders` skips the shader warm-up; `--warm-state-root DIR` points it at a scratch state folder
(tests must never warm the real Application Support folder). `--clean-env` ignores the login-shell PATH and Homebrew, which simulates a fresh Mac
(Xcode's Python, downloaded CMake).

## Build scripts on fresh copies

- `tools/build_macos_metal.py` and `tools/build_macos.sh` check for the game files and the
  required tools (CMake version and Metal Toolchain included) before downloading anything,
  and point to Install.command when something is missing.
- Dependency archives unpack on Python versions without tarfile extraction filters (Xcode's
  3.9); member paths are still validated first.
- Paths containing spaces are tested end to end (fresh copy in a folder with a space).

## Phase 2 (not implemented): reading data/ straight from the disc image

Goal: extract only `default.xex` (needed at build time for code generation and at run time
for the image/resources) and serve `data/` from the `.iso`, saving about 7 GB.

Findings:

- The ReXGlue SDK already has Xenia's disc device: `rex::filesystem::DiscImageDevice`
  (memory-maps the image, parses GDFX, read-only). It is compiled into and exported by the
  prebuilt `librexruntime.dylib` 0.10.0 (`DiscImageDevice::Initialize`, `ResolvePath`), so the
  app could create one without rebuilding the SDK. `StfsContainerDevice` is there too.
- `Runtime::SetupVfs` (SDK) always mounts `game_data_root` as a `HostPathDevice` at
  `\Device\Harddisk0\Partition1` and links `game:` and `d:` to it. The VFS picks the first
  device whose mount path is a prefix of the request; there is no per-file fallback between
  devices.
- So a disc mount needs app-side code after `SetupVfs`: register a `DiscImageDevice` (e.g. at
  `\Device\Cdrom0`) and re-point `game:`/`d:`, or wrap both in a small union device.
- A union (overlay) device is required in practice, because the port adds and changes files
  under `data/`: the build stages `src/lua/*.lua` into `data/scripts/recomp/` and appends
  them to `data/dir.manifest`, and `src/core/dir_manifest_heal.h` repairs that manifest on
  start-up. Those must come from a host folder layered over the disc.
- App code also touches `data/` directly on the host, bypassing the VFS: it reads
  `data/language/en-uk/fonts/gui_large.bft` (turbo notice, `src/core/fable2_turbo_notice.h`),
  and the in-game message helper (`src/core/fable2_game_message.h`) rewrites
  `data/scripts/recomp/recomp_notify_text.lua` at run time. The font would need a VFS read;
  the Lua file belongs in the writable overlay.
- Screenshots and the default cache folder are also placed under `game_data_root`; they would
  move to the app state folder.

Estimate: a union device (host overlay + disc), re-pointing the symbolic links in
`fable_2_app.h`, and VFS-based reads for the font. Startup and area-load times should be
measured against extracted files, since every read would go through the memory-mapped image.
