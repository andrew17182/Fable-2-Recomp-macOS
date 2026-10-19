# Player app: the portable app's launcher window (2026-10-13)

The release app (`docs/release-app.md`) is this same app with a first-launch build step: after the game files it
builds the game on the player's Mac, and Settings > Advanced has Start Fresh and Archives.

The portable `Fable II.app` (`tools/fable2_portable.py`, `docs/portable-build.md`) opens a launcher window before the
game. It is one app with standard macOS windows and needs no Terminal. Nothing is hidden: the extraction log is one
click away, and every setting is in Settings.

| Part | File |
|---|---|
| Main executable `FableIIPlayer` (SwiftUI) | `tools/macos/Player/main.swift` |
| Settings model, defaults, game-environment mapping, auto-resolution table | `tools/macos/Shared/LaunchSettings.swift` (shared with the launcher) |
| Settings tab views (`SettingsTabView`, `FlagRow`, `FlagSection`, `SettingsModel`) | `tools/macos/Shared/SettingsViews.swift` (shared with the launcher) |
| Engine launcher (unchanged role) | `tools/macos/AppLauncher.m` → `FableIILauncher` |
| Extractor | `tools/macos/Prepare/main.swift` → `FableIIPrepare` |

## Windows

**Welcome** (no finished game files yet):

- If a sibling app already has a complete extraction with this build's `default.xex` hash, it is offered as
  "Use These Files": `Fable II Recomp/GameData` (normal app) or `Fable II Recomp Benchmark/GameData` (benchmark kit).
  The checks are AppLauncher.m's: complete marker, `default.xex` + `data/`, hash. The files are APFS-cloned with
  `clonefile`, so they take no space or time, and the same `[portable] reuse: …` lines go to `logs/prepare.log`.
- "Choose Disc Image…" opens a native panel. `FableIIPrepare --headless --progress --stop-on-mismatch` then extracts
  into `GameData`. The window shows:
  - a progress bar, which follows `[progress] <fraction> <text>` lines about five times a second;
  - the current file;
  - Cancel. It stops the extractor; the files copied so far are kept, and the next try resumes;
  - "Show details": the real extractor output, also appended to `logs/prepare.log`;
  - Show Log File.
- On a `default.xex` mismatch, the extractor stops right after hashing it (exit 4). An alert offers
  **Continue Anyway** (run again without the stop; `AllowXexMismatch` is remembered as before),
  **Choose Another Image…** or **Quit**.
- When the files are ready (extracted or reused), the window shows **Preparing shaders…** once per build: a headless
  run of the game per resolution scale (`FABLE2_SHADER_PREP`, no window; Skip goes on), then the Play window
  (`docs/offline-shader-seed.md` §4).

**Play** (every launch):

- Header: the title on the standard window background (2026-10-14: the key-art backdrop read from the player's
  `nxeart` was removed; the windows use native materials and follow light and dark mode).
- **Start:** Continue or Title screen, with the latest save (header display name · quest · level · date, read only
  from `saves/*/4D5307F1/00000001/HeroNNN`).
  - Continue sets `FABLE2_BOOT_SAVE=1`. It skips the title and main menu and loads the latest save shown in
    the window. With one hero save the game's own Continue item loads it. With several, the app also sets
    `FABLE2_BOOT_SAVE_HERO=HeroNNN` (the newest `mainsave.bin`), and boot-save loads that entry from the
    menu's own save list with the same call the game makes for a single save (`src/core/fable2_boot_save.h`).
    If that save is not in the list, the game's slot cards appear. To pick another save, use Title screen.
    No save library is involved: the game's saves stay where they are.
  - With no saves, "New game (no saves yet)" is shown.
- **Resolution:** the shared `InternalResolution` list, including "Auto (N× on this Mac)" from the shared
  `AutoResolution` table (`tests/res_auto_sync_tests.py`).
- **Frame rate**, **Display** (Windowed / Fullscreen), **Settings…** and **Play**.

**Settings (⌘,):** the launcher's tabs, from the same views:

- General, Display, Controls (keyboard map, mouse, hotkeys, controllers; docs/reference.md), Performance, Fixes;
- Advanced, which is the launcher's Diagnostics tab under another name.

Player-only additions:

- General:
  - "Skip launcher, play directly";
  - Game files: the folder, Show in Finder, Choose Disc Image….
- Advanced: Open Logs Folder, Open Data Folder, Reset All Settings… (with confirmation).

These stay in the repo launcher (and developer mode) only: the playtest/latest build choice, save picks and write-back slots, test options,
benchmark, export, and the dev-session footers and CLI hints.

A setting added to `LaunchSettings` (a `Flag` with a tab, or a picker in `SettingsTabView`) appears in both apps
without further work.

**Menus:**

- File: Choose Disc Image…, Open Logs Folder, Open Data Folder.
- Fable II > Settings….

## Launching the game

Play does the following:

1. Sets the settings' environment, `LaunchSettings.gameEnvironment`:
   - frame-rate items: the same values `tools/fable2_dev_session.py` sets for `--fps`, and the launcher's
     items for Auto;
   - `FABLE2_SKIP_INTRO_MOVIES=1/0`. This is explicit, because the portable launcher otherwise skips by default;
   - every `--env` item the launcher passes (`settingsEnvironment`, the single copy of that mapping).
2. Adds `FABLE2_BOOT_SAVE=1` for Continue (plus `FABLE2_BOOT_SAVE_HERO` with several saves), and
   `FABLE2_PLAYER_LAUNCH=1`.
3. Runs `Contents/MacOS/FableIILauncher`: as a child process from the Play button, or with `exec` in the same
   process for "Skip launcher" and `--play` (below).

That launcher (`AppLauncher.m`, the same one the benchmark kit uses) does the rest, as before:

- takes the state folder's `app.lock`;
- stages the Lua scripts and skip-intro movies;
- seeds the shader cache;
- sets the converter pin and toolchain;
- execs `fable_2` with the Metal arguments.

With `FABLE2_PLAYER_LAUNCH=1` it ignores the Option key, which is the player's key now.

`tests/launcher_env_sync_tests.py` builds both apps and checks the result for 5 settings combinations. It compares:

- the launcher's `--print-args`, turned into the game environment as `fable2_dev_session.py` does;
- the environment the player's real Play path hands to a stand-in `FableIILauncher`.

They are equal, except for the player's `FABLE2_PLAYER_LAUNCH=1` and an explicit `FABLE2_SKIP_INTRO_MOVIES=0`
(unset in the dev session).

**When the game quits, the launcher comes back** (2026-10-14, maintainer: "when you close the game, it should
return to the portable launcher").

- The Play button starts `FableIILauncher` as a child process with the app's own launch environment plus the
  Play items, so nothing from an earlier Play (for example `FABLE2_BOOT_SAVE`) leaks into the next one.
- While the game runs, the launcher's windows are hidden and the app switches to `.accessory`, so the Dock
  shows one icon, the game's. The app yields activation to the game (same bundle id), so the game comes to
  the front.
- When the game exits, the app switches back to `.regular`, shows the Play window again and refreshes the
  latest-save line. ⌘Q in the game therefore returns to the launcher; ⌘Q in the launcher quits the app.
- A crash, kill or non-zero exit shows "The game closed unexpectedly (…)" with **Show Logs** next to Play.
  `logs/player.log` records each start and how the game ended (`game ended: exit 0`).
- Play is disabled while the game runs.
- **Skip launcher** (and `--play`) still `exec` the launcher in the app's process: the launcher was never
  shown, so the app ends with the game, like any Mac game. Return to the launcher happens only when it was
  shown at start. Hold Option while opening for the launcher.

**Lock.** While the launcher window is open it holds `app.lock` with close-on-exec. A second copy on the same data
folder is refused ("Fable II is already running"). Play releases it, `FableIILauncher` takes it for the game,
and the app takes it back when the game exits.

## Developer mode: one app, one launcher (2026-10-14)

There is no separate Launcher program any more: the launcher is a role of this app. One binary is built from
`tools/macos/Player/main.swift` (entry point), `tools/macos/DevLauncher/*.swift` and `tools/macos/Shared/*.swift`:

- **Player role** (the release and portable `Fable II.app`): Settings > Advanced > **Developer mode** (off by
  default, stored in `player-settings.plist`). When on:
  - **Window > Fable II Launcher** (⌥⌘D) opens the launcher's main window: build choice (playtest / latest),
    save picker with stars and titles, boot and write-back options, Launch, Close Game, Update Playtest Build;
  - **Window > Developer** has every Tools item: Run Benchmark, Run Flash Kit, Export Portable Build, Update
    Playtest Build, Close Game, Choose Save, Refresh, and the logs / save library / sessions / benchmark report
    folders;
  - Settings shows the launcher's wording and CLI hints, and the **Source checkout** row.
- **Source checkout.** Everything that runs repository tools (`tools/fable2_dev_session.py`, the playtest and latest
  builds, kits, exports, `out/` folders) needs one. It is, in order: `FABLE2_REPO_ROOT`, the build's
  `Fable2RepoRoot`, the folder chosen with **Source Checkout…** (remembered), or the first checkout found in the
  folders above the app (auto-detect, e.g. an app inside or next to the repo). Without one, the launcher window
  shows "Needs a source checkout" with the picker, and Window > Developer offers to choose one.
- **Launcher role**: `tools/macos/build_dev_launcher.sh` builds the same binary as `Fable II Launcher.app`
  with `Fable2DevLauncher` and `Fable2DeveloperModeDefault` in its Info.plist and this checkout as `Fable2RepoRoot`.
  It opens the launcher window as before. Name, bundle id (`local.fable2recomp.devlauncher`), executable
  (`FableIIDevLauncher`), install path (`--install`), UserDefaults keys and command-line modes (`--launch`,
  `--print-args`, `--picker-rows`, `--snapshot --tab …`) are unchanged, so the agent tools that quit, install and
  reopen it keep working. Settings > Diagnostics shows Developer mode (always on) and the checkout.
- Both roles take `--launch` and `--picker-rows`. In the player role the launcher window launches with the player's
  own settings (one settings file); the launcher role keeps its UserDefaults as before.
- Start Fresh / Archives act on the player role's own state folder; the launcher role has no state folder of its
  own (its dev sessions keep theirs), so it does not offer them.
- `--list-capabilities` prints what a role can reach; `tests/unified_launcher_tests.py` checks that every Launcher
  capability is reachable in the player role with developer mode and a checkout, and that none shows with developer
  mode off.

## Skip launcher

With "Skip launcher, play directly" on and finished game files, opening the app plays at once with the saved
settings, and no window appears. Holding **Option** while opening it (or `--show-launcher`) shows the window.
Without finished game files the Welcome window always appears.

## Storage

Everything lives in the app's state folder, `~/Library/Application Support/Fable II Recomp/`
(`FABLE2_APP_STATE_ROOT` in tests). The portable bundle id is `org.fable2recomp.metal.portable`.

- `player-settings.plist`: the settings. They use the launcher's keys, written only when changed, so defaults
  follow new builds. It also holds `skipLauncher` and `startMode`. No `UserDefaults` are used.
- `launcher.plist`: `GameDataRoot`, `AllowXexMismatch`, read by `FableIILauncher` as before.
- `logs/player.log`: one line per Play with the environment it set.
- `logs/prepare.log`, `logs/launcher.log`, `logs/fable_2.log`: as before.

## Command line (tests and tooling)

`Contents/MacOS/FableIIPlayer`:

| Flag | What |
|---|---|
| `--play` | start the game as soon as the game files are ready (exec, as "Skip launcher"; the app ends with the game) |
| `--press-play` | press the Play window's button: the game runs as a child and the launcher returns when it exits |
| `--print-env` | print the environment items Play would set (JSON) and exit |
| `--iso IMAGE` | on the Welcome window, extract IMAGE at once (as if chosen in the panel) |
| `--reuse` | on the Welcome window, use the offered sibling files at once |
| `--show-launcher` | as holding Option |
| `--snapshot PATH.png [--window play\|welcome\|progress\|general\|display\|performance\|fixes\|advanced]` | show that window without taking focus, save it as PNG and quit; `progress` needs `--iso` |

Other arguments are passed to the game. The environment overrides of the portable build still apply:

- `FABLE2_APP_STATE_ROOT`;
- `FABLE2_PORTABLE_GAME_DATA`;
- `FABLE2_PORTABLE_REUSE_FROM`;
- `FABLE2_GAME_DATA_ROOT`: a per-launch game folder, always "ready" and never remembered.

`FABLE2_TEST_BACKGROUND=1` keeps the app from taking focus.

## Verification (2026-10-13, M5 Max)

Evidence is in `out/validation/player-app/` (KEEP).

- **Reuse path.** Scratch state, `FABLE2_PORTABLE_REUSE_FROM` set to the benchmark kit's GameData.
  - "Use These Files" (`--reuse`) cloned it in 0.00 s; `prepare.log` has the `[portable] reuse: game data cloned …`
    line.
  - The window switched to Play with the latest save (scratch APFS copy of the saves).
- **Disc path.** The real ISO, read-only, into scratch GameData:
  - 451 files, 7 GB, `default.xex` hash matched;
  - 10 Lua scripts staged, 3 skip-intro movies made;
  - the window switched to Play.

  The progress screenshot was taken mid-copy. The AppKit progress bar lagged far behind a copy running at
  ~10 GB/s from the page cache, so the window draws its own bar.
- **Test game from the player.** A background test session ran the portable app with `--play` and
  `player-settings.plist` set to 2×, 30 FPS and the FPS counter on. The game log showed:
  - `[res-scale] FABLE2_RES_SCALE=2 -> draw resolution scale 2x2`;
  - `[fps] fps_60=false -> 30 FPS (source: FABLE2_FPS_60)`;
  - `[fps-counter] shown at start`;
  - `fullscreen=false (source: FABLE2_FULLSCREEN)`;
  - the skip-intro redirects.

  It reached gameplay in Thag's Cave at game 29 FPS (2560×1440 capture, "2x res" in the counter) and was closed by
  its session id.
- **Launcher.**
  - `--print-args` is byte-identical before and after the refactor for 6 settings combinations.
  - The main window and all six Settings tabs, snapshotted before and after, are pixel-identical.
- **Benchmark kit.** `tools/fable2_benchmark_kit.py build --no-shaders`:
  - `CFBundleExecutable` = FableIIBenchmark;
  - `FableIILauncher` and `FableIIPrepare` are present; the player is removed from the kit;
  - `codesign --verify --deep --strict` passes, and the package check passes.

  A kit benchmark run was not repeated. Its launcher path is unchanged: only `FABLE2_PLAYER_LAUNCH`, which the kit
  never sets, changes `FableIILauncher`.

## Continue fix and return to the launcher (2026-10-14, M5 Max)

Evidence: `out/validation/continue-loop/` (KEEP). Scratch portable exports (`--no-shaders`), scratch state and
`GameData` (an APFS clone of the benchmark kit's), background test sessions; nothing in Application Support.

- **Bug reproduced** with the 8601508 zip and 6 hero saves: boot-save sent Continue 50 times over 21 s (menu state
  0 -> 5 on the first send, "load started=false"), then gave up; the cards appear only then
  (`6saves-before-fix-boot-save.log`). Dev sessions hold one hero, so this path had not run before.
- **Fixed:** one Continue event. 6 saves: `saves listed by the menu: …`, `direct load of Hero000 (entry 4 of 6): load
  started=true` 0.52 s after the menu came up; in gameplay in that save (Thag's Cave, QC060, screenshot). 3 saves:
  entry 1 of 3, same result. 1 save: the game's own path, `load started=true`, unchanged.
- **Return to the launcher** (`--press-play`, the button's path): the game ran as the app's child; the app was a
  UIElement (no Dock icon) and the game Foreground. A normal close logged `game ended: exit 0`, the Play window
  (560 pt wide) was back on screen and the app held `app.lock` again. Killing the game (`kill -9` on its own pid)
  logged `game ended: signal 9` and the window came back taller by the note line. Not checked here: activation
  hand-off and the Dock switch outside background test mode (test sessions start the app as an accessory).


## The launcher is the way to play (2026-10-15)

Maintainer reports from an M3 Pro with a fresh build from source: the Playtest build failed, Skip intro still
played the logos, the save library stayed empty, and the launcher's name said "Dev". Changes:

- **Name.** The repo launcher is **Fable II Launcher.app** (window, menus, Setup). Kept for tools and agents:
  bundle id `local.fable2recomp.devlauncher` (and so its saved settings), executable `FableIIDevLauncher`,
  `tools/macos/build_dev_launcher.sh`, `out/dev-launcher/` and every CLI flag. `--install` puts the new name next
  to the repo and removes `Fable II Dev Launcher.app`; Setup does the same.
- **Build choice defaults to Latest.** A fresh checkout has no `out/playtest`, so the old Playtest default failed
  every launch ("No playtest copy yet"). A saved `build=playtest` becomes Latest once (`buildChoiceMigration=1`);
  a later deliberate Playtest pick is kept. Without a playtest copy the Playtest item is disabled with a note
  ("Update Playtest Build creates one"), and a Playtest launch falls back to Latest with a note (Swift and
  `fable2_dev_session.py launch`). Tests: `tests/unified_launcher_tests.py`.
- **Continue from Last Save** on the front page, in every role. Repo launcher: the newest save by save time among
  the live slots and the library's play saves (agent test saves skipped; a tie goes to the live slot); it launches
  that pick with boot on through the usual session path (`FABLE2_BOOT_SAVE`), writing back to the live slot it
  came from (library saves: the slot they were written back to, else library only). Player: the Continue button
  is the old Continue start mode (`FABLE2_BOOT_SAVE`, `FABLE2_BOOT_SAVE_HERO`). Continue is the default (Return)
  button when a save exists; otherwise Launch / Play (title screen) is. One click, no confirmation.
- **Skip intro movies** is on the front page in every role (the same setting as Settings > General) and is **off
  by default**: the original logo movies and title intro play. Settings are stored only when changed, so an install
  that never touched it follows the new default; a saved choice is kept. Fast title (`FABLE2_TITLE_FAST`) only
  runs with skip intro on, so with the default the title screen's own fade-in/out is the original too.
- **Skip intro on a fresh clone.** The game redirects the logo opens to `<game root>/out/skipintro/{ms,lh,mw}.bik`;
  a missing file silently plays the full movie. The release app makes them (`FableIIPrepare --stage`), but a
  source checkout only had them from an old validation script. `tools/fable2_skip_intro.py ensure` makes them
  (byte-identical to Prepare's trim); `fable2_dev_session.py launch`, `fable2_playtest.py update` and
  `build_macos_metal.py` call it. The game logs the decision: `[skip-intro] on: logo movies redirected … (3/3
  stand-ins)`, `[skip-intro] off …`, or a warning naming the missing stand-ins. `Fable II.app` opened directly
  (Finder) follows the launcher's saved choice (`AppLauncher.m` reads `skipIntro` from
  `local.fable2recomp.devlauncher`). The M3 log showed none of the launcher's environment (for example
  `FABLE2_MEMEXPORT_PRESERVE=unset`, which every launcher launch sets), so that run was most likely a direct open; Setup's default button now opens the launcher.
- **Save library on a fresh clone.** Only save-pick sessions copied saves into `out/save-library`; title-screen
  play (the only choice with no saves yet) ran on the live state and never reached the library. A title-play
  watcher (`fable2_dev_session.py watch-live`, log `out/dev-sessions/title-watch.log`) now copies each save the game
  makes into the library as `live-heroNNN_<stamp>` (no write-back: it is the live save). `list` creates
  `out/save-library` on first need. Tests: `tests/fresh_clone_tests.py`.
- **Back to the launcher.** When a play game ends (the launcher's own, or one opened another way while the
  launcher runs: Fable II.app, `fable_2`, a `run_macos_*.command`; never an agent test game), the launcher comes to
  the front with its window, reopening it if it was closed. A crash or kill shows "The game closed unexpectedly
  (…)" with **Show Logs**. The player's Play button already did this (above). "Skip launcher, play directly" stays
  off by default; with it the app ends with the game, and a running repo launcher comes forward.
- **Version of a GitHub ZIP.** `tools/fable2_archive_commit.txt` is filled in by `git archive` (`.gitattributes`
  export-subst), so a ZIP download reports `0.1.0 (<commit>)` instead of `0.1.0 (source)`.
