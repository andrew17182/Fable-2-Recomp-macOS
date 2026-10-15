# Portable test build

A zippable `Fable II.app` for another Apple Silicon Mac that has only the player's own Fable II disc image.
That Mac does not need the repo, Xcode or extracted game files.

## Make the package (build Mac)

```sh
python3 tools/fable2_portable.py build            # or Dev Launcher > Tools > "Export Portable Build…"
```

The package is the **no-converter release build** (release decision D3, 2026-10-14,
`docs/no-converter.md`): no `libdxilconv.dylib`, no `libmetalirconverter.dylib`. The command first refreshes
`out/build-release/mac-arm64-metal-release` with `tools/build_macos_metal.py --release` (incremental: 25 s for the first one
in a checkout whose dev build was current, 15–40 s when nothing changed; log `out/build-release/build.log`), then
packages it. It writes:

- `out/portable/Fable II Portable/`: `Fable II.app` and `README.txt`;
- `out/portable/Fable II Portable.zip`.

Options:

- `--no-rebuild`: package `out/build-release` as it is.
- `--with-converter`: package the dev build `out/build/mac-arm64-metal-release` as it is, with both converter
  libraries (the old default; build it first).
- `--out DIR`: write somewhere else.
- `--no-zip`.
- `--no-skip-intro`: play the logo movies by default.
- `--shader-scales 1,2,3` (default): resolution scales of the shader seed. Since 2026-10-14 the seed is taken from
  what is remembered for the build's shader code, with no game run: the agent cache entry of its
  `shader_cache_key`, else the last package's seed (seed store), else an offline headless prep of the remembered
  lists, and only then the old warm-up sessions (`--seed-source`, `docs/offline-shader-seed.md`).
- `--shaders CACHE_DIR` (repeatable): take the seed from these cache folders, read only, instead of a warm-up.
  This is only useful for caches made with the pin (every build since 2026-10-12, unless set to `native`).
- `--no-shaders`: no seed and no warm-up session.

Times and sizes on 2026-10-11:

| Package | Time | App | Zip | Seed |
|---|---|---|---|---|
| `--no-shaders` | 10 s | 164 MB | 51 MB | none |
| `--shader-scales 1` | 25 s | 191 MB | 67 MB | 27 MB: 752 records, 183 optimized pixel shaders |
| `--shader-scales 1,2` | 39 s | 201 MB (+10) | 74 MB (+6.4) | 37 MB: 995 records, 264 shaders |
| Default `1,2,3` | 53–57 s | 211 MB (+10) | 80 MB (+6.4) | 47 MB: 1236 records, 344 shaders |
| No-converter release, `--seed-source prep` (2026-10-14) | 64 s incl. release refresh | 134 MB | 41 MB | pipeline lists only |

The warm-up sessions are agent test sessions: background, muted, input-isolated, target slot none. Each is closed by
its own session id. They can run while the playtest copy is open, but they need a save in the save library.

What is in the app:

| Path | What |
|---|---|
| `Contents/MacOS/` | `FableIIPlayer`, the main executable: the launcher window (`tools/macos/Player/main.swift` + `tools/macos/Shared/*.swift`, `docs/player-app.md`); `fable_2` and its dylibs (all `@rpath` + `@loader_path`; SDL is static in `librexruntime`); `FableIILauncher` (`tools/macos/AppLauncher.m`, exec'd by Play and used by the benchmark kit); `FableIIPrepare` (`tools/macos/Prepare/main.swift` + the setup app's `GDFX.swift` and `Support.swift`) |
| `Contents/Resources/recomp-lua/` | `src/lua/*.lua` |
| `Contents/Resources/native-shaders/` | optional shader seed (below) |
| `Info.plist` | `Fable2Portable`, `Fable2XexSHA256` (from `src/core/xex_verify.h`), `Fable2SkipIntroDefault`, `Fable2MSCMinTarget` (`apple7:15.0`), `Fable2ShaderSeedID`, `Fable2NSToolchainHash`; bundle id `org.fable2recomp.metal.portable` |

There is no `LocalDefaults.plist`, so the app holds no paths into this checkout. The tool refuses to finish if any of
these is found in the app:

- something that looks like disc content (`default.xex`, `data/`, `nxeart`, `$SystemUpdate`, `.bik`, `.xex`,
  `.iso`);
- a local absolute path (this checkout or the home folder) in any file, binaries included;
- a personal identifier (`tools/fable2_name_guard.py`: the maintainer's name, a personal email, a `/Users/<name>`
  path; allowlist `tests/name_scrub_allowlist.txt`);
- a dependency that isn't bundled;
- unless `--with-converter`: `libdxilconv.dylib` or `libmetalirconverter.dylib` anywhere in the app, a binary that
  links either, a `DirectXShaderCompiler*` licence text, or a `Licenses/THIRD_PARTY_NOTICES.md` that names either
  library. The shipped notices file leaves out the dxilconv row and keeps one row for the Apache-2.0 Metal Shader
  Converter runtime headers, which are still compiled in (their licence and acknowledgements stay in `Licenses/`).

The binaries carry no build-machine paths: the game, the Metal plugin and the source-built SDK compile with
`-ffile-prefix-map=<checkout>=.` and `-ffile-prefix-map=<home>=~` (`CMakeLists.txt`, `backends/metal/CMakeLists.txt`,
`tools/build_sdk_from_source.py`), so `__FILE__` in log and assert messages reads `./src/…`, and the packager strips
the debug map (`strip -S`: object-file paths; symbol names stay for crash reports). The release build
(`tools/build_macos_metal.py --release`) is stripped the same way.

**The shader seed is game-derived.** It is compiled from the game's shaders, so it belongs only in this private
package, which lives in ignored `out/`. Never commit it or copy it into a tracked folder. `README.txt` says the same.

## On the test Mac

1. Unzip it and copy `Fable II.app` anywhere.
2. The app is ad-hoc signed (`spctl --assess` rejects it) and a downloaded or AirDropped zip is quarantined.
   Run `xattr -dr com.apple.quarantine "/path/to/Fable II.app"` once, or open it, dismiss the warning and use
   System Settings > Privacy & Security > Open Anyway. macOS 15 removed the right-click > Open bypass. No
   notarization.
Since 2026-10-13 the app opens a launcher window first (`docs/player-app.md`): a Welcome window on first launch
(reuse offer or disc image, extraction progress with "Show details" and Cancel), then the Play window on every
launch (Continue, resolution, frame rate, display, Play; Settings with ⌘,). The steps below describe what happens
underneath; the reuse and extraction now run from that window (`FableIIPlayer`), with the same checks and log lines.

3. First launch, when the app's own `GameData` folder does not exist yet: it reuses a finished extraction from a
   sibling app-data folder in the same Application Support folder. It checks these in order:

   1. `Fable II Recomp/GameData` (the normal app; skipped when that is the portable app's own folder);
   2. `Fable II Recomp Benchmark/GameData` (the benchmark kit, `docs/benchmark-kit.md`).

   A source is used only when its `.fable2-portable.json` says `complete: true`, `default.xex` and `data/` exist, and its
   `xexSHA256` equals this build's `Fable2XexSHA256`. A source accepted with "Continue Anyway" has a different hash, so
   it is skipped. The launcher APFS-clones the source with `clonefile`, as the kit does in the other direction, so it
   takes no extra space and no time, and it only reads the source. It then checks the marker of the clone. On any
   mismatch or error (other volume, clone failed, incomplete clone, which it removes) it falls back to the disc image.
   Every decision is logged to `logs/prepare.log` (`[portable] reuse: …`). Holding Option, `--choose-iso` and
   `--iso` skip the reuse. Test-only: `FABLE2_PORTABLE_REUSE_FROM` replaces the list with colon-separated `GameData`
   folders; set but empty, it disables reuse.

   Otherwise a native "Choose your Fable II disc image (.iso)" panel appears. `FableIIPrepare` then:
   - reads the disc's file table (rejecting images without `default.xex` and `data`);
   - checks free space;
   - extracts `default.xex` and hashes it. On a mismatch with `Fable2XexSHA256` it warns, with **Continue Anyway** /
     **Choose Another Image** / **Quit**. It never blocks;
   - copies `data/`, `$SystemUpdate/` and `nxeart` with a "Preparing game files…" progress window into
     `~/Library/Application Support/Fable II Recomp/GameData`. Files already the right size are kept, so an
     interrupted copy resumes. A marker from a different image forces a full copy;
   - stages the side files (below) and writes `GameData/.fable2-portable.json` (`complete: true`).

   The launcher remembers the folder in `launcher.plist`. With "Continue Anyway" it also stores
   `AllowXexMismatch`, and later launches then set `FABLE2_XEX_MISMATCH_OK=1`.
4. Later launches show the Play window (or go straight to the game with "Skip launcher, play directly"). Every launch
   runs `FableIIPrepare --stage`, which is fast and idempotent.
5. To choose the image again: File > Choose Disc Image… (or Settings > General). Holding **Option** while opening
   the app now shows the launcher window when "Skip launcher" is on.

Logs are in `~/Library/Application Support/Fable II Recomp/logs/`: `prepare.log`, `launcher.log` and `fable_2.log`.
Saves and settings live in the normal Application Support folder of that Mac.

## Side files the game root needs (recreated, never shipped)

`docs/bug-patterns.md` rows 38–39 explain why: a relocated game root must also get everything the hooks open
through `game:\`.

- **Lua scripts:**
  - `data/scripts/recomp/*.lua` come from the bundle. The game rewrites `recomp_notify_text.lua` at runtime, so an
    existing copy is kept.
  - Their `scripts\recomp\<name>` lines are appended to `data/dir.manifest` with CRLF, as
    `tools/ensure_recomp_manifest.cmake` does. The engine's startup self-heal (`src/core/dir_manifest_heal.h`) is a
    second net.
- **Skip-intro movies:** `out/skipintro/{ms,lh,mw}.bik` are 2-frame trims of the disc's logo movies, which makes
  them game content. `FableIIPrepare` makes them from the extracted `data/art/videos/*.bik` with a Swift port of
  `trim_bik.py`. The output is byte-identical to the checkout's files.
- **Skip intro on by default:** the portable launcher sets `FABLE2_SKIP_INTRO_MOVIES=1` unless the environment sets
  it (package option `--no-skip-intro`).
- **Screenshots and cache:** `Screenshots/` (View button) is created under the game data folder at runtime. The
  shader cache is under `<state>/cache`.

## No Xcode on the test Mac

- **Optimized shaders without Xcode.** Optimized pixel shaders normally run `metal-opt`/`metallib` from Xcode's
  Metal Toolchain (`docs/native-shaders.md`). On a Mac without Xcode or the command-line tools, `/usr/bin/xcrun` is
  a stub that opens Apple's "install developer tools" dialog. So the lookup first checks for a developer directory:
  - `DEVELOPER_DIR`;
  - `/var/db/xcode_select_link`;
  - `/Applications/Xcode.app`;
  - `/Library/Developer/CommandLineTools`.

  If there is none, it never spawns xcrun.
- **What gets logged.** One warning line, then the plain translated shaders are used:
  `metal-opt/metallib not found (no Xcode or command-line tools installed)`.
- **Cache-only mode.** If `FABLE2_NS_CACHE_TOOLCHAIN` names a toolchain hash (the portable launcher sets it from
  `Fable2NSToolchainHash`), the cache-only mode applies. Optimized libraries already in the shader cache are used;
  nothing new is optimized. The log says:
  `optimized pixel shaders come only from the cache (toolchain …)`.
- **Simulate a Mac without Xcode:** `FABLE2_NS_ASSUME_NO_XCODE=1`.
- No other code path shells out. `grep posix_spawn|popen|system(` in `src/` and `backends/` finds only
  `metal_air_optimizer.cc`.

## Shader seed

**No-converter packages (the default since 2026-10-14).** Nothing in them runs the converter, so the pin below has
nothing to act on; the launcher still sets it. The NS2/native cache keys come from the DXBC translation, which does not
depend on the Mac. The history below applies to `--with-converter` packages and to the dev build.

**Why the pin.** The translated shaders' cache keys depended on the build Mac:

- Metal Shader Converter builds its output with a minimum target, which was this Mac's GPU family and its exact
  macOS version (`SetMinimumTarget` in `metal_pipeline_cache.cc`).
- The stage-compile records and the optimized (NS1) records are keyed by that output.

So a cache made on an M5 Max (Apple10) under macOS 27.0.1 would miss on an M1–M4 or under another macOS version.
`FABLE2_MSC_MIN_TARGET=<family>:<macOS>` pins the target (families `apple6`–`apple10`, `metal3`; `native` or unset
keeps this Mac's). Since 2026-10-12 **every build pins `apple7:15.0` by default** (`[graphics] shader_cache_target`,
`docs/native-shaders.md`, "Converter target pinned by default"); `native` restores the per-Mac target. The portable
launcher still sets `apple7:15.0` from `Fable2MSCMinTarget` unless the environment sets it, which now equals the default.
The log says `Metal shader converter: minimum target pinned to apple7 macOS 15.0 (… native would be family 1010
macOS 27.0.1)`.

With the pin, the converter output depends only on the build and its settings, not on the Mac:

- its inputs are the DXIL, which comes from the DXBC translator and the bundled dxilconv;
- the pinned target;
- fixed compatibility and root-signature options;
- the bundled libmetalirconverter.

The translator's inputs are fixed too. The vendor is always Apple, and `msaa_2x_supported` is
`supportsTextureSampleCount(2)`, which every Apple Silicon Mac supports. The gamma storage comes from cvars, and the
resolution scale is a setting.

Evidence from the A/B caches below, same Mac:

- The DXIL records are identical in both arms (163/163).
- Every converter-output key differs between the native and the pinned arm: stage-compile 0/166 shared,
  optimized-shader plain-library keys 0/113.

So the target is the only Mac-dependent input that reaches the keys. Proving the hits on a second Mac still needs
one.

**Cost of pinning (A/B, 2026-10-11).** Setup: M5 Max, the same build, save `hero1-dog-live_2026-10-07_14-45-08`.
One run per arm, background test sessions, no contention, verdict ok, 0 AVs. Each arm warmed its own cache from empty
at 1× and 2× before measuring, so its optimizer pool had drained. Labels: `out/validation/f2diag/pin-{native,pin}-*`.

| | native (Apple10, macOS 27.0.1) | pinned apple7:15.0 |
|---|---|---|
| 1× uncapped turn30: FPS, GPU ms mean / p95 (20–50 s, GPU time trace) | 193.3, 4.91 / 8.43 | 194.5, 4.88 / 8.48 |
| 2× uncapped turn30: FPS, GPU ms mean / p95 | 124.2, 7.98 / 12.54 | 125.1, 7.92 / 12.48 |
| 60-capped sweep: cores, p95 ms, 1% low | 1.02, 17.81, 54.9 | 1.02, 17.85, 54.8 |
| Cold first launch: level load begins; optimizer pool drained (CPU spent) | 3.57 s; 15.2 s (7.3 s) | 3.62 s; 16.4 s (6.6 s) |

Every difference is within run-to-run noise: **pinning is free on this Mac**. The driver still compiles each library
for the real GPU. The cold first launch was the first launch after the rebuild that added the switch. A 3× `headings8`
A/B on 2026-10-12 (two pairs) agreed: 15.31 vs 15.17 ms GPU, inside run-to-run noise (`docs/native-shaders.md`). The pin
became the default for all builds then. Shaders are still never shipped with a normal install (they are game-derived);
the setup app warms the player's own cache on their Mac instead (`docs/setup-app.md`, "Shader warm-up").

**Package-time warm-up** (`warm_up_shaders` in `tools/fable2_portable.py`; since 2026-10-14 the last resort after the
remembered sources and the offline prep, `docs/offline-shader-seed.md`):

1. It copies the live cache's shareable pipeline lists (`4D5307F1.xsh`, `4D5307F1.metal.xpso`), read only, into a
   seed cache.
2. For each scale it launches one test session of the latest build with `FABLE2_MSC_MIN_TARGET=apple7:15.0`,
   `FABLE2_RES_SCALE=<n>` and `--cache-from` the previous cache. The session boots f2diag's default save, or the
   newest save-library entry.
3. Warm-up translates every known pipeline (506 on 2026-10-11), and the optimizer pool optimizes every pixel shader.
4. It waits until `background warm-up complete` is logged and a `[native-shaders] background work done` line has stayed
   the newest native-shaders line for 10 s. It then closes the session by id.
5. The last session's cache, which holds every scale, is compacted into the bundle:
   - the artifact store's records: last record per key, NS1 records of other toolchains dropped. It goes to
     `Contents/Resources/native-shaders/local/<abi>/4D5307F1/`. The ABI tag `msc…-ps…-sh…` is computed from the
     version constants;
   - the shareable lists;
   - a `manifest.json` (counts, warm-up sessions).

   The toolchain hash is computed like `AirOptimizerToolchainHash()`, as xxh3 of `metal-opt --version`.

**On the test Mac.** `AppLauncher.m` copies the seed once per seed id into
`<state>/cache/shaders/local/metal/<this Mac's GPU name>/…`, the same folder name as `GetShaderStorageDeviceTag()`,
plus the shareable lists. It never replaces existing files. Warm-up then rebuilds the pipelines from the seeded
translations. With Xcode present, shaders the seed lacks are optimized as usual. Without Xcode they stay plain.

Not shipped: the `.binarchive` pipeline archives. They hold GPU- and driver-specific compiled code, and their names
carry the OS version (`os27.0.1`). The test Mac builds its own on the first launch.

## Testing on the build Mac (sandboxed)

The test harness used on 2026-10-11 lived in the session scratchpad and was not committed. It called
`tools/fable2_dev_session.launch` (test control, background, muted, input-isolated, target slot none) with the app
switched to the portable package, plus:

- `FABLE2_GAME_DATA_ROOT=` (empty counts as unset);
- `FABLE2_PORTABLE_GAME_DATA=<scratch>/GameData` (test-only override of the extraction folder);
- `--game-arg --iso <image>` (no dialog);
- `--cache-from <empty dir>`;
- `FABLE2_NS_ASSUME_NO_XCODE=1`.

The state folder is the session's `test-state` (`FABLE2_APP_STATE_ROOT`), so nothing touches the real Application
Support. Set `FABLE2_PORTABLE_HEADLESS=1` to run `FableIIPrepare` without its window.

Results (M5 Max, macOS 27.0.1, no other instance running):

1. First run:
   - extracted 451 files (7 GB) in 2 s (fast SSD, image in page cache);
   - default.xex hash matched;
   - staged 10 Lua scripts and made 3 skip-intro movies.
2. The game, in cache-only mode:
   - no xcrun ran;
   - the seed was copied in, and 128+ optimized pixel shaders came from it.

     This first run used the earlier unpinned seed taken from the live cache, which hits only on this Mac. The
     pinned package was checked the same way: empty cache, `FABLE2_NS_ASSUME_NO_XCODE=1`, launcher-set pin. The
     log showed `minimum target pinned to apple7 macOS 15.0` and `optimized pixel shaders installed from cache` up
     to the 128 line; it logs at powers of two and did not reach 256. Warm-up then rebuilt 506 pipelines from the
     seed;
   - at 3× (`FABLE2_RES_SCALE=3`, same setup, the default 1,2,3 package): the log showed
     `scale=3x3`, the pin, and `installed from cache` up to the 128 line. The game was in gameplay at Bower Lake
     at 45 FPS, 60-capped. The normal latest app on the same save at 3× also gave 45 FPS; the scene is GPU-bound.
     With the A/B save at 3×, 60-capped, native and pinned both gave 57.3 FPS (`pin3x-*`);
   - both logo movies were redirected;
   - boot-save reached BowerLake at 60 FPS;
   - the F6 menu opened.
3. Second launch (marker complete) went straight to the game. With a modified `default.xex` and
   `FABLE2_XEX_MISMATCH_OK=1`, the log shows the warning and the game continued to BowerLake.
4. `FableIIPrepare --headless` with a wrong expected hash warned, finished and reported `XexMatches=false`.
   Running `--stage` twice is idempotent: the manifest gains no lines.

Not testable here (needs the second Mac):

- Gatekeeper on a downloaded or AirDropped zip;
- the real "no developer tools" xcrun stub;
- the file panel and Option-key re-pick by hand;
- extraction speed;
- seed hit rate on a different GPU family (expected to match this Mac's with the pin; check the `installed from cache`
  lines in that Mac's `fable_2.log`).
