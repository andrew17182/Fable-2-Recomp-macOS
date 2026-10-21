# Fable 2 recomp — full reference (previous README)

> This is the original, detailed README, kept for reference (Windows build, config, controls, Lua, tracing, Vulkan, manifest). The short project README is [../README.md](../README.md).

First and for most this project was made to test the capabilities of local and open source AI. This project was made using Qwen3.8 27b. After about an hour it got the game running. Then after a large amount of human trial and error I was able to get enough of the functions mapped to be able to complete the game. I currently do not consider this to be fully complete as I have yet to 100% the game. The sha256 of the iso that started this project is: "685a0d3bea9718812f17bcd155907a5359a548b6d3d8342dd2a6c944f45e35ff" and its the Fable 2 GOTY for USA and Europe.

Recompilation of Fable 2 (Xbox 360, title ID 4D5307F1) using the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0. Guest PPC code is statically recompiled to C++ at build time by `rexglue codegen`, driven by `fable_2_manifest.toml`.

# Current and planned features
[x] Can be used to beat the game\
[x] Guild chest fully unlocked\
[x] Uncapped framerate / increased framerate\
[ ] Higher Resolution support\
[ ] Built in Debug Menu
  - [x] Enabling custom lua to run in game

[x] Keyboard / Mouse Support\
[ ] In game Text changed to respect keyboard and mouse
  - [ ] Automatic swapping between text

[ ] Increased performance / framerate\
[ ] Hero / Dog Texture bug fix\
[ ] Vulkan support\
[ ] Linux Builds\
[ ] Custom commands to aid in debugging\
[ ] Improved Graphics rendering
[ ] Custom menu(s) / modifying menus for extra functionality (like closing the game)



# Notes for running the game
- **How to extract:** rip the disc to an ISO, then open it with **[XboxImageExtractor](https://github.com/dromex1/XboxImageExtractor)** — a GUI tool for Xbox 360 game images. It lists the image's filesystem; select `default.xex`, `data`, and `$SystemUpdate` and extract them into the project root (`data` and `$SystemUpdate` extract as folders). You can also grab `nxeart` and anything else the tool lists.
- **You must supply the game content yourself** (it is not in the repo): rip the Fable 2 GOTY (USA/EU) disc (the one with SHA-256 above; upstream's image: this macOS port is tested only with the Platinum Hits image, whose `default.xex` is checked by hash) and put `default.xex`, `data/`, `nxeart/`, and `$SystemUpdate/` in the project root. The build does not copy this into the build directories   
- **Saves live in `<build dir>\saves\`** — back that folder up to keep your progress, and copy it between build trees (Debug/Release) or machines to carry a save over.
- **Contributions must exclude local game data:** never include disc images, files extracted from them, generated game code, or player saves/profiles. Git ignores ISO images, Xbox executables, `data/`, `nxeart`, `$SystemUpdate/`, `generated/`, and `saves/`. Keep any other extracted disc files in the ignored `disc-content/` folder.
- The `--game_data_root <path>` override still points the content root at a different tree (e.g. to run from a shared content copy without staging); saves/cache still land next to the exe.


## Running

For the native macOS build and launcher, see [macOS instructions](macos.md).

To run the game you must:
1) Extract a downloaded copy release of the game 
2) Extract the "data" and "default.xex" from your copy of the game, See "How to extract" for more info on that
3) Run the fable.exe program and if no errors pop up then the game should launch and you are good to go
  - If an error pops up about the hash being wrong try to extract a different version of the game and then try again.


Optional command-line overrides (all normal `--cvar value` args):

| Argument | Effect |
|---|---|
| `--game_data_root <path>` | Use a different content directory (beats the exe-dir-only search) |
| `--update_data_root <path>` | Optional update content root |
| `--window_width` / `--window_height` / `--fullscreen` | Presentation options |
| `--keyboard_gamepad_map <map>` | Host keyboard -> guest gamepad button map (see below) |

## User config (fable2_config.toml)

The recomp has its own human-readable user config, `fable2_config.toml`, in the
game's state folder (Application Support for the Mac app, next to the exe for
command-line builds) — separate from `fable_2.toml`, which is the ReXGlue SDK's cvar
config. The defaults live in one file, `config/fable2_config.toml`: the build
stages it (once; user edits survive rebuilds), the Mac app copies it into
Application Support on first launch, and the game embeds the same bytes at
build time and writes them out if the file is ever missing. Loaded in
`Fable2App::OnPostInitLogging()`; code in `src/core/fable2_config.{h,cpp}`.

- Missing keys -> built-in defaults; wrong types / unknown sections -> logged
  warnings, defaults used; a syntax error -> dialog + defaults (never blocks
  launch).
- To add a setting: add a member to `fable2::config::Values` (with default),
  read it in `Load()`, document the key with the same default in
  `config/fable2_config.toml`, and list it in `tests/config_defaults_tests.cpp`.
  That test runs during the macOS build and fails it if the file and the
  built-in defaults disagree, so a missing key always behaves like the
  shipped default.
- Settings that back an SDK cvar (currently the `[input]` section:
  `keyboard_gamepad_map`, `mouse_look`, `mouse_look_scale`) are seeded into
  the cvar at startup — but only when no higher-priority
  source set it: `fable_2.toml` (cvar config), `REX_*` env vars, and the
  command line (e.g. `--keyboard_gamepad_map`) still win, and the console
  (Backtick) can still change the value live.
- The `[patches]` section holds runtime toggles for the recomp-level
  (mid-asm hook) patches, consulted by the hook bodies on every call
  (`src/core/fable2_hooks.cpp`) — flip one and relaunch to A/B a patch with no
  rebuild. Currently: `fps_60` (frame rate: `true` = 60 FPS target using one vblank per frame, `false` = 30 FPS, the console's rate; default `"auto"`, also used when the key is missing, picks 60 FPS when the GPU is estimated to average about 54 FPS or more at the resolution scale that will run and 30 FPS otherwise; at 2×, and with resolution `"auto"` too, the 2x rule picks uncapped (vsync off) when 2× is estimated at 45 FPS or more but under 54 (e.g. a 14-core M3 Pro runs 2× uncapped), see `docs/resolution-scale.md` §25; `FABLE2_AUTO_2X=0` turns the 2x rule off; base chips (M1–M5 without Pro/Max/Ultra) stay at 30 on Auto until a base Mac is measured; hardware/scene performance may vary). The original game runs at 30 FPS (`fps_60 = false`). Use `vsync=true` with this hook. `FABLE2_FPS_60=0/1/auto` overrides the TOML setting (`FABLE2_FPS_AUTO_MIN_CORES=<n>` switches back to the old rule, 60 FPS from n GPU cores; `FABLE2_FPS_AUTO_CORES=<n>` / `FABLE2_RES_AUTO_NAME=<name>` fake the core count / GPU name for testing). Guest-image data patches are a different file:
  `fable2_patches.toml` (see above).
- `[perf] hotfunc_yield_every` (default `8`) batches guest yields in the
  hot-function overrides: every Nth yield is a real `SwitchToThread`
  (`1` = original, `0` = never yield; see `src/core/hotfunc/hotfunc_yield.h`).



## Keyboard controls

The game normally reads a gamepad via the Xbox 360 `XamInputGetState` API. A
synthetic "keyboard gamepad" input driver (`src/input/keyboard_gamepad.h`) is added
on top of the default SDL driver, so host keyboard keys can drive the guest on
top of (OR-merged with) whatever a real gamepad reports. The physical pad
keeps working; the keyboard just adds buttons. It is wired up in
`Fable2App::OnPreSetup` via `config.input_factory`.

The mapping is the `keyboard_gamepad_map` cvar, format `Key:Button,Key:Button,...`. Its default is no longer hardcoded in the binary: it comes from `[input] keyboard_gamepad_map` in `fable2_config.toml` (see User config above), which you can edit to remap permanently. The command line and the console (Backtick) still override it per-launch / live.

- **Key** — a host key name understood by `rex::ui::ParseVirtualKey`
  (`E`, `Space`, `LeftShift`, `F1`, ...).
- **Button** — a guest gamepad input: `A`, `B`, `X`, `Y`, `LB`/`RB` (shoulders),
  `LT`/`RT` (triggers), `Up`/`Down`/`Left`/`Right` (dpad), `Pause` (Start),
  `Select` (Back), `L3`/`R3` (thumb clicks), `StickUp`/`StickDown`/
  `StickLeft`/`StickRight` (left thumbstick, full deflection while held), and
  `RStickUp`/`RStickDown`/`RStickLeft`/`RStickRight` (right thumbstick, the
  camera; full deflection while held, added to mouse look and clamped).

The default layout is:

| Key(s) | Guest input |
|---|---|
| `E` / `X` / `Z` / `C` | `A` / `B` / `X` / `Y` |
| `W` `A` `S` `D` | Left stick (up / left / down / right) |
| `Escape` | Pause (Start) |
| `M` | Select (Back) |
| `Q` / `Tab` | Left / Right trigger |
| `I` `J` `K` `L` | Dpad (up / left / down / right) |
| Arrow keys | Right stick (camera) |

**Migration (2026-10-19).** Before this, the arrow keys were the dpad and no key reached the right stick.
A map equal to that old default (saved in `fable_2.toml` or an old `fable2_config.toml`) is replaced by the
new default when the game starts (logged as `[input] keyboard_gamepad_map: old default replaced`);
environment and command-line maps are kept. The launchers migrate their stored map once
(`keyboardMapVersion` = 2): a map that binds the arrows to the dpad exactly as the old default did gets the
arrows on the right stick and `I`/`J`/`K`/`L` on the dpad, and keeps the rest of its bindings. None of the new
keys is a hotkey.

Remap at launch without recompiling, e.g.

```
fable_2.exe --keyboard_gamepad_map "E:A,B:B,Space:L3,Return:Start"
```

Key names are case-sensitive and must be in the SDK's `ParseVirtualKey` list (`src/ui/keybinds.cpp`:
letters, digits, `F1`–`F24`, `Space`, `Return`, `Escape`, `Tab`, `Backspace`, arrows, `Shift`, `Control`,
`Alt` (Option), `Backtick`, `Minus`, `Plus`, `LBracket`, `Numpad0`…, `LMB`/`RMB`/`MMB`, …); entries that do not
parse are skipped. The game logs the map in use once per change:
`[input] keyboard_gamepad_map (<source>, N of M entries valid): "..."; mouse_look <on> (scale <n>)`.

**Precedence.** `fable_2.toml` is written with every cvar, so once it holds `keyboard_gamepad_map` that
value (source `config`) shadows `[input]` in `fable2_config.toml`. `REX_KEYBOARD_GAMEPAD_MAP` (environment)
beats both, and the command line beats the environment. The launchers' **Controls** tab uses the
environment route (below).

### Settings > Controls (Launcher and player app)

`tools/macos/Shared/ControlsView.swift`, shared by both launchers (tests: `tests/controls_tab_tests.py`).

- **Keyboard → controller:** one row per guest input (A/B/X/Y, LB/RB, LT/RT, d-pad, Start, Back, L3/R3,
  left stick ×4, right stick ×4). Click a row and press a key to rebind it; **+** adds another key, a mouse button or any key
  the game reads (also ones a Mac keyboard lacks); ✕ removes one; **Restore Defaults**. A key already bound
  to another input asks: **Swap** (the other input gets this row's old key) or **Replace** (the other input
  loses it). A warning mark shows keys that are also a hotkey or bound twice. ⌘ shortcuts still work while a
  row waits for a key; Command itself can't be bound.
- **Mouse:** mouse look on/off and sensitivity (`mouse_look_scale`, 1–4096, default 256; the slider spans
  32–2048). There is no invert-Y setting in the game.
- **Hotkeys:** the SDK overlay keys are cvars and can be rebound here: console (`bind_console`, Backtick),
  settings menu (`bind_settings`, 4; also sets `mouse_unlock_key`, the key that frees the mouse), debug
  overlay (`bind_debug_overlay`, 3), achievements (`bind_achievements`, 8); **None** unbinds. Fixed in code
  and only listed: F5/5 quick save, F6/6 Lua script menu, F7/7 turbo, F9 FPS counter, ⌃⌘F full screen
  (SDL's window menu), ⌘Q quit; the controller hotkeys show their button next to the keys.
- **Game controllers:** controllers work directly; macOS remaps controller buttons system-wide in System
  Settings > Game Controllers (**Open Game Controller Settings…** opens it,
  `x-apple.systempreferences:com.apple.Game-Controller-Settings.extension`). The tab lists connected
  controllers (GameController framework), the controller hotkeys (**Turbo**, **Quick save**, **Screenshot**:
  None / View / Share / Left stick click / Right stick click; see "Controller hotkeys" below) and
  **View button reaches the game as Back** (`FABLE2_VIEW_BUTTON_TO_GAME=1`).

Every launch passes `REX_KEYBOARD_GAMEPAD_MAP=<map>`, `REX_MOUSE_LOOK=true|false` and
`REX_MOUSE_LOOK_SCALE=<n>`; changed hotkeys add `REX_BIND_<NAME>=<key>` (and `REX_MOUSE_UNLOCK_KEY` with the
settings key); a changed controller hotkey adds all three `FABLE2_PAD_*`. Stored keys: `keyboardMap` (+ `keyboardMapVersion`),
`mouseLook`, `mouseLookScale`, `hotkeyBinds`, `viewButtonToGame`, `padTurbo`, `padQuickSave`, `padScreenshot`
(Launcher: UserDefaults; player app: `player-settings.plist`). Launcher `--launch` options:
`--keyboard-map MAP`, `--mouse-look` / `--no-mouse-look`, `--mouse-look-scale N`, `--bind CVAR=KEY`,
`--view-button-to-game`, `--pad-turbo|--pad-quicksave|--pad-screenshot none|view|share|ls|rs`.

### Mouse look (right stick)

Mouse movement is mapped to the guest **right stick** for camera control. The
movement since the previous poll is converted into stick deflection, so you
**sweep the mouse to look and stop to stop**. Two cvars control it:

| Argument | Effect |
|---|---|
| `--mouse_look <bool>` | Enable/disable mouse look (default `false`: free cursor; enabling it captures the cursor while playing) |
| `--mouse_look_scale <n>` | Sensitivity: right-stick units per pixel of mouse movement (default `256`; larger = more sensitive) |

The defaults come from `[input] mouse_look` / `[input] mouse_look_scale` in
`fable2_config.toml` (edit there to change them permanently), the command
line overrides per launch, and both cvars are hot-reloadable from the in-game
console, so you can dial in the sensitivity live. Example: `fable_2.exe
--mouse_look_scale 512` for a more sensitive camera.

All cvars above are hot-reloadable, so they can also be changed from the in-game console.

## F5 — quick save, F6 — run an external Lua script

**F5** (or number-row **5**) saves the current game immediately, exactly like the
pause menu's **Save → Save and Continue** (same slot, same files), without opening
the menu. It is ignored outside gameplay and for 3 s after the previous quick save.

- **How it works:** `src/core/fable2_quick_save.h` hooks the world update
  (`sub_82275F00`, game thread) and makes the menu's own save call,
  `sub_824E8990(saveManager, 0, 0, 0, 1)`. The log shows `[quick-save] #N ...`.
- The Lua `AutoSaveGame()` is **not** used: called from a host-run script it crashed
  the game once and left it on a loading screen with a half-written `mainsave.bin`
  the other time (2026-10-03).
- `f5_lua_path` (default empty) makes F5 run a Lua file instead of saving.

**F6** (or number-row **6**) runs an external Lua file in the in-game Lua
state, exactly the way the game's own `RunScript(path)` global does — but
triggered from the host. This lets you drop a plain `.lua` file on disk and run
it against the live game (no recompile of the scripts needed). (This was F5
before F5 became quick save.)

- **Default file:** `data/scripts/recomp/F6.lua` (the build stages
  `src/lua/*.lua` into `data/scripts/recomp/` next to the exe). The shipped
  `F6.lua` opens a script selector menu (Get Player Position, Modify Money,
  Morality / Purity, and Character Editor when that mod is on: `docs/character-editor.md`).
- **Path:** set by the `f6_lua_path` cvar (default `scripts/recomp/F6.lua`,
  resolved relative to the VFS root `data/`). Override per-launch with
  `fable_2.exe --f6_lua_path "scripts/other/MyScript.lua"`.
- **How it works:** `src/core/fable2_f5_lua.h` captures the
  `CScriptManager::RunScript` callable the first time the game loads a `.lua`
  script (via a probe on the LuaPlus bound-method dispatcher), then replays that
  call with your path when F6 is pressed. The file is loaded fresh on each press,
  so you can edit it live (the VFS re-reads it).
- **The script runs in the game's global Lua environment**, so it has the full
  game API (`QuestManager`, `Debug`, `GUI`, `Creature`, `Player`, ...). Plain
  text is fine — `RunScript`/`loadfile` compile it for you.

Implementation: key handling in `src/input/window_keyboard_mouse.h` (macOS) and
`src/input/keyboard_gamepad.h` (Windows polling), a per-frame replay from the
`MainRenderLoop` hook in `src/diagnostics/fps_meter.h`, and the
string-build + `RunScript` call in `src/core/fable2_f5_lua.h`.

### Controller hotkeys

Three actions, one controller button each (`none`, `view`, `share`, `ls`, `rs` = stick clicks):

| Action | Env | Default |
|---|---|---|
| Turbo on/off (same as F7) | `FABLE2_PAD_TURBO` | none |
| Quick save (same as F5) | `FABLE2_PAD_QUICKSAVE` | share |
| Screenshot | `FABLE2_PAD_SCREENSHOT` | view |

A screenshot is the game frame as `Screenshots/Fable II YYYY-MM-DD HH-MM-SS.png` in the game data root
(override with `FABLE2_SCREENSHOT_DIR` or `--screenshot_dir`). A bound button no longer reaches the game
(View = Back, stick clicks = L3/R3); an unbound one does. `FABLE2_VIEW_BUTTON_TO_GAME=1` passes Back
through while View is bound. One button, one action: a set action takes its button and a default on
that button is dropped (`FABLE2_PAD_TURBO=view` alone moves View from screenshot to turbo). With Share
unbound the GameController Share watcher is not installed. `FABLE2_PAD_DEBUG=1` logs every pad button.
Code: `src/input/controller_hotkeys.h`. Tests: `fable2_test_control.py press <session> view|share|ls|rs`.

**Controllers in the background:** controllers keep working while the game window is not in front
(Settings > Controls > Game controllers > **Works in background**, default on; `FABLE2_PAD_BACKGROUND=0` turns it off).
Keyboard and mouse still need focus. The game sets `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` before `SDL_Init`
and `GCController.shouldMonitorBackgroundEvents`. The SDK pad driver stays active with no keyboard focus; when
focused, an ImGui overlay that captures the mouse still blocks it. The controller hotkeys (including the
GameController Share watcher) act in the background too. Test-only instances (`FABLE2_PHYSICAL_INPUT=0`) never set
any of this. Code: `src/input/test_background.h`, `src/input/controller_hotkeys.h`.

### Frame pacing (macOS)

Use `tools/fable2_dev_session.py launch --mode library --save hero1-dog-live --target-slot none --fps 60` to try 60 FPS without writing back to live saves. `--fps 30` selects original pacing, and `--fps uncapped` removes the guest vblank limit. For the GPU-based choice, pass `--env FABLE2_FPS_60=auto --env REX_VSYNC=1` instead of `--fps`. Omit `--fps` to retain existing preferences (the copied config's `fps_60`, which defaults to `"auto"` when unset). 60 FPS mode targets one guest vblank per frame with the normal guest clock; it does not accelerate simulation time. The tested childhood market scene produced about 58–60 unique gameplay frames/s, with one settled window at 60.00; demanding scenes may run slower.

Front-end audio preloading uses a 10 ms yield between banks instead of the original 400 ms throttle. It retains every bank load and completion check, shortening the forest painting pause when booting into a save. Set `FABLE2_AUDIO_PRELOAD_DELAY_MS=400` to compare original pacing; accepted overrides are 1–400 ms.

### Turbo (macOS)

**F7** or **7** toggles turbo (the achievements overlay moved to **8**): the whole game runs faster (default 2x) while the screen stays at ~30 FPS. Turbo is off at every start; a short "Turbo 2x on/off" notice appears in the top-right corner. Set the speed on the launcher front page (**Turbo speed** 1.1×–4.0×, default 2.0×; `FABLE2_TURBO_SPEED`) or with `[patches] turbo_scale` in `fable2_config.toml` (1.1–4.0 in 0.1 steps; 1.0 disables F7). **Turbo audio** (`FABLE2_TURBO_AUDIO`, `[patches] turbo_audio`) sets the sound during turbo: "Sped up, natural pitch" (default; Apple AUNewTimePitch time-stretch), "Sped up, high pitch" (AUVarispeed) or "Normal speed" (1x, drifts behind the game, the old behaviour). The audio output then consumes guest audio at the turbo rate, so speech and music keep pace with the game; at 1x the audio path is unchanged. Guest sleeps and timed waits run at the turbo speed too (turbo wait fix, `FABLE2_TURBO_WAIT_FIX=0` restores the SDK behaviour, where they ran 2x long). Turbo needs SDK clock scaling, so launchers no longer pass `--clock_no_scaling=true`; an old `clock_no_scaling = true` line in `fable_2.toml` is ignored. Details: [docs/turbo.md](turbo.md).

### FPS counter (macOS)

**F9** shows or hides a small counter in the bottom-left corner. The headline is the game's own frame
rate and average frame time over the last ~0.5 s (guest swaps, e.g. `52 FPS · 19.2 ms`). Extra fields, in
order: `display N`, the presented rate, always shown (while any overlay is shown, this one included, the
presenter repaints every UI tick, so the presented rate tends toward the display refresh), `2× res` at a
higher internal resolution, `turbo 2.5x` (the turbo speed) while turbo is on, and the upscaling filter in
use for presenting, `Bilinear` or `MetalFX` (MetalFX only when it is actually applied: the window must be
larger than the internal size, otherwise the presenter logs `[metalfx] not applied` and uses bilinear; read
from the Metal plugin's `fable2_present_filter` export). Example: `52 FPS · 19.2 ms · display 120 · 2× res ·
turbo 2.5x · MetalFX`. Digits are tabular, so the box width changes only with the digit count. It uses the
game's UI font (the monospaced ImGui font as fallback) and is off by
default: `[graphics] fps_counter = true` in `fable2_config.toml`, `FABLE2_FPS_COUNTER=1|0`, or the
Launcher's **Show FPS counter** sets the start state. Code: `src/core/fable2_fps_counter.h`.

### Version and window title

`VERSION.txt` holds the release number (not `VERSION`: the repo root is on the compiler's include
path, and a `VERSION` file would shadow the C++ `<version>` header on a case-insensitive disk). The build stamps it with the short git commit
(`tools/fable2_version.py` -> generated `fable2_version.h`; `-dirty` when tracked files have
uncommitted changes, `(source)` for a copy without git history, e.g. a GitHub ZIP). The window
title is `Fable II Recomp <version> (<commit>) · Metal`; the log's first lines, `Fable II.app`'s
Info.plist (`Fable2VersionString`), the launcher footer and the setup app's finish screen show
the same string.


### Input recording and replay

**Record:** Settings > Diagnostics (Dev Options) > **Record input** (`FABLE2_RECORD_INPUT=1`, off by default). While it
is on, the launcher's front page shows a red "Recording input" note. Each game writes one file,
`logs/recordings/Fable II input YYYY-MM-DD HH-MM-SS.jsonl`, in the state folder (Dev Launcher: the live
`~/Library/Application Support/Fable II Recomp`; player app: its own folder). **Show Recordings** opens that folder.
`FABLE2_RECORD_INPUT_PATH` sets another file (`*.jsonl`) or folder. A normal quit ends the file with
`recording_end`. Every line is flushed as it is written, so after a crash the file is still a valid prefix.
The first line (`AppLaunch`) has a `meta` object: build version, save name (Dev Launcher), resolution and
frame-rate settings.

**What is recorded:** the final pad state the game reads (`XamInputGetState`, after keyboard mapping, mouse look
and the controller-hotkey filter), one `pad` event per change. Raw `key` events are recorded too, for information
only. State anchors are `LevelLoaded` (end of each level load), `AfterNewGameVideo`, and UI fingerprints of frames
that draw UI items. Turbo and speed changes are `speed` events.

**Replay:**
- Test session: `python3 tools/fable2_test_control.py replay <recording> [--env FABLE2_TURBO_SCALE=2]`. This needs a
  test-session recording (`launch --record`, then `stop-recording`), whose `initial-state/` snapshot holds the
  saves.
- Any launch: `FABLE2_REPLAY_INPUT=<recording>`, for example in the launcher's Extra environment. Boot the same save
  the recording started from. A player recording has no state snapshot.

Status is logged as `[replay]` lines in `fable_2.log` (test sessions also write `control/replay-status.json`).
`fable2_replay.py` validates recordings (`load_recording`) and keeps the old host-clock driver
(`replay --host-clock`).

**Timing (schema 2):** every event has host time (`t_us`, `dt_us`) and guest time (`g_us`, `gdt_us`). The guest
clock runs at the turbo (or slow-mode) speed. The in-game driver matches the recorded anchors in order, then applies
each pad change when the guest clock reaches the live anchor time plus `gdt_us`. So a recording made at 2× replays
at 1× (and the other way round) at the same game moments. Recorded speed changes are not replayed: the replay runs
at the speed you launch it with. Schema 1 files (host time only) still load and are exact only at 1×.

Measured 2026-10-21 (boot into a save, about 7 s of walking and an A press, 2 runs per arm, 7–12 pad changes
each):

| Recorded at | Replayed at | Pad changes, guest offset from recorded |
|---|---|---|
| 1× | 1× | 0 to +33 ms |
| 1× | 2× | +6 to +35 ms |
| 2× | 1× | +0.6 to +6 ms |

The game reads the pad once per frame, so a change lands at the next frame's read: at most one frame late (33 ms
of guest time at 30 FPS, 67 ms at 2×).

**Caveat:** replay is state-anchored, not frame-exact. Physics, AI, streaming and load times are not
deterministic, so long replays drift and can diverge (a missed hit, a different path). Inputs before the first
`LevelLoaded` are timed from launch, and load time depends on the Mac and the speed. A recorded anchor that never
happens fails the replay after `FABLE2_REPLAY_TIMEOUT_S` (60 s) and returns control to the player. Code:
`src/diagnostics/fable2_session_journal.h`.

### Fable II Launcher (macOS: the way to play from source)

Normal launcher play retains in-game subtitles and audio preferences across launches, including library-only play with no hero write-back. The options profile is saved independently of hero saves after a completed, stable write; previous preferences are backed up under `out/save-backups`. Automated test sessions keep their options isolated.

`Fable II Launcher.app` (source `tools/macos/DevLauncher/*.swift` with the player app's entry point, build with
`tools/macos/build_dev_launcher.sh [--install]`; named "Fable II Dev Launcher" before 2026-10-15) picks a save and
boots straight into it. Opening `Fable II.app`, `fable_2` or `run_macos_*.command` directly still works (advanced).
The front page (2026-10-19, docs/player-app.md): **Start at Title Screen** and **Load Saved Game** (the default
button, boots straight in), the save menu (newest first), **Resolution** and **Frame rate**, **Skip intro movies**, **Turbo speed**
and **Turbo audio**, then the running
status (with **Dev Options**: **Build**, "Write saves back to" for library saves, and **Update Playtest Build** when
shown). When a game ends the launcher comes back to the front; after a crash it shows a note with **Show Logs**.
**Settings** is a page of the same window (the Settings / Launcher button, bottom-left on both pages; ⌘,), with these
tabs: General (intro, extras, turbo notice), Display (resolution, upscaling, effects, window, frame rate, FPS
counter), Controls (keyboard map, mouse look, hotkeys, controllers; see **Settings > Controls** above), Mods
(Character Editor), and with Dev Options on: Performance (shader cache and specialization switches), Fixes (the
correctness switches, all on by default; **Reset All to Defaults**) and Diagnostics (trace preset, hitch trace,
verify modes, extra env, source checkout). The **Tools** menu has Close Game, Refresh and Open Logs Folder, and with
Dev Options: Save Library…, Run Benchmark…, Run Flash Kit…, Export Portable Build…, Update Playtest Build and the
save library / sessions / benchmark report folders. Default-on kill switches
(`FABLE2_NATIVE_SHADERS`, `FABLE2_NS_SPECIALIZE`, `FABLE2_SHADOWS_NATIVE`, `FABLE2_DEFERRED_CLEARS`, `FABLE2_ALIAS_CLEAR_NATIVE`, `FABLE2_MAIN_DEPTH_NATIVE`, `FABLE2_EFFECTS_FOOTPRINT_VARIANT`, `FABLE2_CLEAR_LOAD_ACTION`, `FABLE2_SSM_STENCIL_FOLD`, `FABLE2_CLEAR_CONST_SOURCE`, `FABLE2_TILE_EDRAM`,
`FABLE2_RING_REINIT_FIX`, `FABLE2_TURBO_FINE_DELAY`, `FABLE2_HITCH_TRACE`, `FABLE2_NATIVE_XLATE`, `FABLE2_NATIVE_XLATE_VS`, `FABLE2_NATIVE_TESS`, `FABLE2_NATIVE_RECTS`, `FABLE2_NATIVE_POINTS`, `FABLE2_NATIVE_TESS_BLENDED`) are passed only when turned off (`=0`). Verify modes are passed only
when turned on (`=1`). Every on/off setting also has `--<name>` / `--no-<name>` for `--launch`, and `--print-args`
prints the backend arguments without launching.

- **Start at Title Screen** uses the live save folder, exactly like `Fable II.app`, nothing copied. The **save
  menu** lists the live save slots, newest first; with Dev Options > Diagnostics > Developer > **Show starred saves
  in save menu** it also lists starred **save-library entries** (`out/save-library/`).
- **Frame rate:** *Use game settings*, **Auto** (the default for new installs: 60 FPS when the GPU is estimated to average about 54 FPS or more at the chosen internal resolution, 30 FPS otherwise; the picker shows what it chooses on this Mac), **30 FPS**, **60 FPS**, or **Uncapped**. The selection is saved between launches and applies to title, live saves and library saves. 60 FPS is a target; scene performance can vary.
- **Skip intro movies** (default off since 2026-10-15: the original intros play; on the front page and in Settings >
  General). Load Saved Game always boots straight into the save (`--boot` / `--no-boot` remain for `--launch`).
  Title-screen play also copies each save into the library.
- **Fast title** (on whenever intro movies are skipped, so off by default): the title's white intro and fade and its
  2 s outro after A are skipped on the first title screen, so "Press A" works right away and the
  main menu appears immediately. Save boots reach the loading screen about 4.5 s sooner. Startup
  file opens also no longer wait for a 50 ms worker tick. Disable with `FABLE2_TITLE_FAST=0` and
  `FABLE2_BOOT_LOADER_KICK=0` (details: `src/core/fable2_loading_pacing.h`).
- **Skip cosmetic waits (changes original pacing)** (General → Game, default off; `FABLE2_FAST_PACING=1`): shortens
  fixed waits that only pace the screen, never a readiness or streaming wait. It sets the defaults of
  `FABLE2_LOAD_FADE_FRAMES` (15 → 1: the holds after "Saving content", before the arrival and at boot) and
  `FABLE2_SAVE_NOTICE_MS` (500 → 0: the notice's minimum time on screen); an explicit value of either still wins.
  Fast title stays with **Skip intro movies**. It replaces the earlier "Shorter load fades" toggle (a saved "on"
  carries over). Inventory, what is excluded and measurements: `docs/load-fixes-20261016.md` section F.
- A save launch runs in a session copy (`out/dev-sessions/<id>/test-state`) holding only that
  save. Whenever the game writes a save there (menu Save, F5, autosave), the save is copied into
  the library as `<name>_<YYYY-MM-DD_HH-MM-SS>` and written back to the chosen live slot (the old
  slot is first backed up to `out/save-backups/`). Nothing is copied when the game closes.
- **Close game** quits the running game; a second copy is refused.
- The launcher app's `--launch` mode uses its saved frame-rate choice; `--fps settings|auto|30|60|uncapped` overrides it for that launch.
- Same logic from the command line: `python3 tools/fable2_dev_session.py list|launch|status|close`.
- **Automated tests / agents** should launch with
  `python3 tools/fable2_dev_session.py launch --mode library --save <entry> --target-slot none --test-control`
  (then `tools/fable2_test_control.py capture|press|close <session>`). `--test-control` turns on
  **background test mode** (`FABLE2_TEST_BACKGROUND=1`, launched with `open -g`): the game opens
  behind the frontmost app and never takes the keyboard/mouse; test `press` keys (including
  F5/F6/F7/F9/5/6/7/8) go through the app's own test channel and work without focus, while real
  keyboard/mouse input stays focus-gated; an App Nap exemption keeps frames at full rate when the
  window is covered. Background test launches are also muted (`--audio_mute=true`).
  Normal Launcher play does not set either. `fable2_test_control.py launch|replay`
  (and the `fable2` MCP tools, which use it) also run in background mode through `open -g`;
  `--foreground` restores the old direct launch with focus on launch and before each press.
- `FABLE2_BOOT_SAVE_MENU_DELAY_MS` (default 0) adds a wait before boot-save sends Continue at the main menu.\
<br>
<br>
<br>
<br>

---


# Notes for dev who want to work on the build:

## Guest-image patches (fable2_patches.toml)

Data patches for the loaded `default.xex` guest image (Xenia game-patches
format), applied before the guest module launches: `Fable2App::OnPostLoadXexImage()`
→ `fable2::patches::Load()` + `ApplyAll()` (code in `src/core/fable2_patches.{h,cpp}`).
Same lifecycle as the user config: staged by the build, recreated with the
built-in defaults if missing, and a broken file falls back to the built-ins
(dialog + log) so it never blocks launch. Each `[[patch]]` has an `enabled`
toggle (default true) — flip it in the file and relaunch to A/B a patch with
no rebuild. **Scope: data patches only** — code-region ops are inert in this
recomp (guest `.text` is never executed); code patches are mid-asm hooks
instead. Full details, the current patch list, and how code patches work:
`docs/patches.md`.

## Guest memory barriers and crash diagnostics

ReXGlue 0.10.0 codegen drops PowerPC `sync`/`lwsync` because it assumes x86 ordering, but Apple
Silicon is weakly ordered. `tools/apply_recomp_patches.py` adds a host fence after each of the
147 guest barriers (`src/core/fable2_guest_memory.h`). The fences are **on by default**, and
startup logs `[guest-barriers] on`. Set `FABLE2_GUEST_BARRIERS=0` to turn them off for A/B tests.
`FABLE2_CRASH_DIAG=1` (opt-in) logs `[crash-diag]` lines to `fable_2.log` before an invalid guest
indirect call aborts. Those lines give the target, call site, thread, and the object and vtable
words involved. Background: the 2026-10-05 playtest notes.

## Perf-pass switches (2026-10-11)

Results and evidence: the 2026-10-11 performance audit, "Results". All are on by default; set the
variable to `0` to restore the old behaviour (env only, no launcher toggles).

| Switch | What it does | Where |
|---|---|---|
| `FABLE2_FCTIW_ROUNDING` | `fctiw`/`fctid` round per the guest rounding mode (nearest-even by default, as on Xenon) instead of SIMDe's round-half-away-from-zero | `src/core/fable2_guest_memory.h`, `patches/sdk/codegen-fctiw-rounding.patch` |
| `FABLE2_PREPARED_DRAW_POOL` | reuse the per-draw `PreparedDraw` record, queue storage and resource-set storage | `metal_command_processor.cc` |
| `FABLE2_RING_WAIT_NOSPIN` | ring progress is signalled after the read-pointer writeback store; the 3D Engine wait has no yield loop | `metal_command_processor.cc`, `sdk-metal.patch`, `src/core/fable2_spin_wait.h` |
| `FABLE2_GAME_TICK_FINE` | second precise sleep inside the GameThread tick margin | `src/core/fable2_spin_wait.h` |
| `FABLE2_TURBO_FINE_DELAY` | under turbo, guest sleeps, wait timeouts and timer periods are scaled in microseconds (a 1 ms sleep at 2x no longer rounds to 0 and spins); launcher Fixes > Turbo: precise sleeps | `patches/sdk/turbo-fine-delay.patch`, `docs/turbo.md` |
| `FABLE2_TIMER_QUEUE_BLOCKING` | SDK TimerQueue blocks in `kevent` instead of yield/1 ms-sleep spinning | `patches/sdk/timer-queue-blocking.patch` |
| `FABLE2_GUEST_THREAD_QOS` | user-interactive QoS for the 3D Engine and GameThread | `src/core/fable2_spin_wait.h` |
| `FABLE2_THREAD_PRIORITY_MAP` | guest thread priorities mapped to SCHED_OTHER levels instead of raw SCHED_FIFO values | `patches/sdk/thread-priority-map.patch` |
| `FABLE2_PROBE_GETENV_CACHE` | diagnostic probes read their environment switches once | `src/diagnostics/fable2_thread_trace.h`, `fable2_text_probe.h` |
| `FABLE2_TEX_UPLOAD_MERGE` | pre-commit texture flushes share their untile compute and copy blit encoders | `metal_texture_cache.cc` |
| `FABLE2_TILE_EDRAM` / `FABLE2_TILE_EDRAM_SELFDEPTH` | master kill switch for the tile-EDRAM work / self-source depth transfers inside the draw pass (`docs/resolution-scale.md` §13) | `metal_render_target_cache.cc` |
| `FABLE2_DEFERRED_CLEARS` | resolve clears are drawn first inside the next draw pass that binds the target instead of as separate load/store passes; flushed as before when anything else reads the target (`docs/resolution-scale.md` §14). `FABLE2_DEFERRED_CLEARS_SWAP_FLUSH=1` conservative arm; `FABLE2_DEFERRED_CLEARS_DIAG=skip\|dup<N>` diagnostics | `metal_render_target_cache.cc` |
| `FABLE2_ALIAS_CLEAR_NATIVE` | default on; the XDK clear quads drawn on a 4×MSAA alias of a surface become EDRAM clears of the surface's existing MSAA variant (deferred into its next pass), and draws that bind, write and export nothing are skipped. `=0` draws them as before. `FABLE2_ALIAS_CLEAR_TRACE=1` logs candidates and clears in the `FABLE2_SHADOW_TRACE` window; counters `tr_n_ac_*` (`docs/alias-clear-20261013.md`) | `metal_command_processor.cc` (`TryAliasClearDraw`), `metal_render_target_cache.cc` (`TryAliasClear`) |
| `FABLE2_MAIN_DEPTH_NATIVE` | default on (needs alias clears on); two more exact alias-clear kinds: a nonzero depth word through a D24S8 4× alias (z on the unorm24 grid, the shadow-mask init z = 0.75) is written in row bands into existing word-exact targets of the same pitch (D24S8, 8888, 10:10:10:2 unorm), and a stencil-only clear moves the tiles to the existing same-format variant with the transfers a draw would do, then replaces only the stencil. No 4× alias target is bound for the main depth any more. `=0` draws them as before. Diagnostic `FABLE2_MAIN_DEPTH_DUMP=<frame>` + `FABLE2_MAIN_DEPTH_DUMP_DIR=<dir>`: whole-EDRAM guest-word dumps at three points of that frame. Counters `tr_n_md_*` (`docs/main-depth-20261014.md`) | `metal_command_processor.cc` (`TryAliasClearDraw`), `metal_render_target_cache.cc` (`TryDepthWordAliasClear`, `TryStencilOnlyAliasClear`) |
| `FABLE2_TILE_RESOLVE` | default **on** (2026-10-13); colour resolves run as tile functions inside the draw pass that rendered the source (reads the attachment from tile memory, no compute encoder), falling back per resolve to the compute path. `=0` turns it and the follow-ups off. Follow-ups (kill switches, default on): `FABLE2_TILE_RESOLVE_EXTENT=0` (draw passes open with the full render area instead of only as tall as their draws reach), `FABLE2_TILE_RESOLVE_DRAWN_GATE=0` (gate on render area vs resolve rectangle instead of the untouched area), `FABLE2_TILE_RESOLVE_CLEAR=0` (the resolve's clear gets its own pass instead of being drawn after the copy in the same pass), `FABLE2_TILE_RESOLVE_PREWARM=0` (no startup build of the tile pipelines; keys in `<title>.tres-keys.txt` in the shader cache plus built-in defaults). `FABLE2_TILE_RESOLVE_KEEP_OPEN=1` (default off, no gain measured) keeps the pass open for the next draws. Diagnostics: `FABLE2_TILE_RESOLVE_VERIFY=1` also runs the compute resolve and compares bytes; `FABLE2_TILE_RESOLVE_VERIFY_BREAK=1` negative control; `FABLE2_TILE_RESOLVE_AREA_RATIO=<x>` (1.5); `FABLE2_TILE_RESOLVE_MSAA=0`; `FABLE2_TILE_RESOLVE_TRACE=1` logs fallback and render-area reasons; counters `tr_n_tres_*` (`docs/tile-resolve-20261013.md`) | `metal_tile_resolve.cc`, `metal_tile_resolve_keys.inc`, `metal_direct_host_resolve.cc`, `metal_render_target_cache.cc` (resolve clear), `metal_command_processor.cc` (`IssueCopy`, `EncodePreparedDraw`, `BeginRenderEncoderForDraw`), `backends/metal/shaders/resolve_tile*.{metal,inc}` |
| `FABLE2_TEX_KEY_MEMO` / `FABLE2_DRAW_PREP_LITE` | default **on** (2026-10-14); GPU Commands thread CPU: generation-checked host texture key memo and fetch-decode memo (SDK patch `sdk-metal-gpucmd.patch`), and per-draw trims (unchanged fetch constants keep bindings in sync, no vertex-binding copy, sampler parameters built once, queue budget totals, flat sampled-texture sets); `=0` restores each. `FABLE2_TEX_KEY_MEMO_VERIFY=1` checks every memo hit (`docs/gpucmd-cpu-20261014.md`) | SDK `pipeline/texture/cache.cpp`, `metal_command_processor.cc`, `metal_texture_cache.cc`, `metal_ptr_set.h` |
| `FABLE2_DRAW_PREP_LITE2` | default **on** (2026-10-14); more GPU Commands thread trims: the texture load check reused when a state snapshot (load-check epoch, scaled-resolve generation, in-sync bits) is unchanged, texture find memo and sampler parameter memo, pending-write list in reused storage, float constants packed by runs, cached texture properties, flat residency set; `=0` restores each. `FABLE2_DRAW_PREP_LITE2_VERIFY=1` recomputes every reused check (`docs/gpucmd-cpu3-20261014.md`) | SDK `pipeline/texture/cache.cpp`, `metal_command_processor.cc`, `metal_texture_cache.cc` |
| `FABLE2_FIX_LOD_SIZE_CACHE` | default **off** (game-logic fix, changes original behaviour); the texture streamer's per-model size cache (`82B197C8`) tests the minimum distance it computes from, so a model first sized from the shadow view (the pale Bower Lake root) is recomputed on the next frame. Launcher: Fixes → "Game bug fixes (changes original behaviour)". Trace: `FABLE2_PALE_ROOT_TRACE=1` / `_WATCH=1` (`docs/pale-root-20261014.md`) | `src/core/hotfunc/gpu/LodSizeCacheFix.cpp`, `src/core/hotfunc/gpu/PaleRootTrace.cpp` |
| `FABLE2_FAST_PACING` | default **off** (pacing change). `1`: cosmetic pacing waits shortened by changing sub-switch defaults: `FABLE2_LOAD_FADE_FRAMES` 15 → 1 (render-object release fades), `FABLE2_SAVE_NOTICE_MS` 500 → 0 (save-notice minimum; the game's own is 3100). An explicit sub-switch value wins. Launcher: General → Game → "Skip cosmetic waits" (`docs/load-fixes-20261016.md` section F) | `src/core/fable2_fast_pacing.h`, `src/core/hotfunc/load/LoadFade_8235ABA0.cpp`, `src/core/fable2_loading_pacing.h` |
| `FABLE2_CHARACTER_EDITOR` | default **off** (mod). `1`: F6 runs `F6_character_editor.lua`, which adds "Character Editor" to the F6 menu (morality, purity, gold, renown, fat, strength/skill/will morph through the game's `Stats`/`Money` API; save backed up first). Launcher: General → Mods (`docs/character-editor.md`) | `src/core/fable2_character_editor.h`, `src/lua/character_editor.lua` |
| `FABLE2_CHARACTER_EDITOR_BACKUP_DIR` | Character Editor backup folder; default `<state dir>/character-editor-backups` (Fable II Launcher play sessions: `out/save-backups/character-editor`) | `src/core/fable2_character_editor.h` |
| `FABLE2_LOAD_CHECK_EXACT` | default **on** (2026-10-14); a texture request after a "no load" check defers only loads caused by a guest CPU write after the check (the next draw that uses the texture loads it in its batch), so no load runs outside the upload batch; `=0` old behaviour, `=2` diagnostic (filter, no deferral). `FABLE2_LOAD_CHECK_EXACT_VERIFY=1` tracks every deferred texture until it loads (violations counted; `=2` negative control), `FABLE2_LOAD_CHECK_TRACE=1` logs loads after a no-load check with their call path (`docs/gpucmd-cpu4-20261014.md`) | SDK `pipeline/texture/cache.cpp`, `metal_texture_cache.cc` |
| `FABLE2_TEX_RENAME` | default **on** (2026-10-14); a full reload of a texture the open command buffer already sampled goes into a fresh backing (pooled, new bindless slots; the old slots are retired so encoded draws keep the old contents) instead of ending the render pass; `=0` is the old split path. `FABLE2_TEX_RENAME_NOOP=0` (default on): planned loads that write nothing (stale plan, RTT keep) end the pass again. `FABLE2_TEX_UNCHANGED=1` (default off, no gain measured): skip reloads of unscaled textures at scale > 1 that no write touched. `FABLE2_RTT_NARROW_CPU_FIX=0` (default on) restores the resolve-narrowing race. `FABLE2_TEX_RENAME_MB` (128) caps pool + in-flight old backings. Diagnostics: `FABLE2_TEX_RENAME_VERIFY=1` (+`_BREAK=1`), `FABLE2_TEX_UNCHANGED_VERIFY=1`, `FABLE2_TEX_RENAME_TRACE=1`, `FABLE2_TEX_NX_DIAG=1`, `FABLE2_TEX_NX_TRACE=<hex>`; counters `tr_n_trn_*`, `tr_n_nx_*` (`docs/tex-rename-20261013.md`) | `metal_tex_rename.cc`, `metal_texture_cache.cc` (`PrepareTextureMaterialization`, `ClassifyMaterializationPlan`), `metal_resolve_to_texture.cc` (`RttOnInvalidate`, `NxTrack`), `metal_command_processor.cc` (`TexRenameAfterLoads`, orphan settle in `IssueCopy` / `EndCommandBuffer`) |
| `FABLE2_EFFECTS_FOOTPRINT` | default **on** (2026-10-14); with original-size effects at 2×/3×, a pixel-shader offset texture fetch on a scaled colour texture covers its whole guest texel (bilinear: corner snap at 2×, ±⅔ pair at 3×; point on clamp-to-edge: in-shader bilinear at the guest-texel centre). Removes the bloom grating; about −6% FPS at 2× on the M5 Max (`docs/bloom-banding-20261014.md`) | `metal_msl_translator.cc` (`effects_footprint`) |
| `FABLE2_EFFECTS_FOOTPRINT_VARIANT` | default **on** (2026-10-14); a pixel shader gets the footprint code only in the modification variant of draws where one of its 2D offset fetches reads a resolution-scaled, non-depth texture (Metal-only modification bit, decided per draw with the texture cache's scaled-resolve key); other draws use the translation without it, which samples identically. About −0.5 ms GPU at Bower 2× on the M5 Max. `=0`: every shader keeps the footprint code. Diagnostic `FABLE2_EFFECTS_FOOTPRINT_TRACE=1`; counters `tr_n_fp_variant`, `tr_n_fp_variant_miss` (stays 0) (`docs/ssmask-20261014.md`) | `metal_command_processor.cc` (`FootprintVariantNeeded`), `metal_pipeline_cache.cc`, `metal_shader.cc`, `backends/metal/sdk-metal-ssmask.patch` |
| `FABLE2_CLEAR_LOAD_ACTION` | default **on** (2026-10-14); a deferred clear drawn at the start of a new draw pass that covers its whole attachment becomes the attachment's clear load action (no load, no quad; same value conversion as the transfer pass; depth, 8888, 10:10:10:2, 7e3 only). Counter `tr_n_ssm_clear_la` (`docs/ssmask-20261014.md`) | `metal_render_target_cache.cc` (`ApplyClearLoadActions`), `metal_command_processor.cc` (`BeginRenderEncoderForDraw`) |
| `FABLE2_SSM_STENCIL_FOLD` | default **on** (2026-10-16); the shadow mask's stencil reset (P8b), whose target hands all its tiles to the main colour pass, is not drawn: those transfers read the cleared stencil byte from a constant texture and the dead clear is dropped. `=0` draws it as its own pass. [ssm-merge-20261015.md](ssm-merge-20261015.md) |
| `FABLE2_CLEAR_CONST_SOURCE` | default **on** (2026-10-16); a transfer whose source colour target has a pending whole-target clear reads a constant texture holding exactly that clear (made once with the same load-action clear), so the clear stays deferred: the final composite's resolve clear becomes the composite pass's load action instead of a full-screen pass at the next frame start. Also lets colour clears whose top bit is set become load actions. `=0` restores both. [ssm-merge-20261015.md](ssm-merge-20261015.md) |
| `FABLE2_SSM_MERGE=1` | opt-in, default off (+0.1 ms GPU at 2× on the M5 Max); the main-depth copy, carried clear and stencil clear before the screen-space shadow mask (P8a) are drawn inside the mask pass, whose depth load becomes DontCare. Meant for bandwidth-bound Macs; needs an M3/M1 A/B. [ssm-merge-20261015.md](ssm-merge-20261015.md) |
| `FABLE2_MIPLOD_PROOF` | default **on** (2026-10-13); per-draw bound of the mip levels a downsample (mip-chain) pass can read, from its passthrough shaders, vertices and viewport; such draws sample a texture view limited to those levels, and the resolve into the next stored mip level runs as a tile function (`FABLE2_TILE_RESOLVE`). `=0` keeps those resolves on compute. Diagnostic `FABLE2_MIPLOD_TRACE=1`; counters `tr_n_tres_miplod`, `tr_n_miplod_bounded/view/unbounded` (`docs/mip-lod-20261013.md`) | `metal_miplod_proof.{h,cc}`, `metal_miplod_draw.cc`, `metal_tile_resolve.cc`, `metal_texture_cache.cc` (level-range views) |
| `FABLE2_NATIVE_XLATE` / `FABLE2_NATIVE_XLATE_VS` | default on (2026-10-13); the game's pixel and vertex shaders are translated from Xenos ucode straight to MSL instead of DXBC → DXIL → Metal Shader Converter, with a per-shader fallback. `=0` restores the converter path (Launcher: Performance → "Native shader translator", "Native translator: vertex shaders too"). Diagnostics: `FABLE2_NATIVE_XLATE_DUMP=<dir>` writes the MSL; `FABLE2_LOG_MEMEXPORT_FORMATS=1` (`docs/native-translator.md`) | `metal_msl_translator.cc`, `metal_pipeline_cache.cc` (`TryNativeXlate`) |
| `FABLE2_NATIVE_TESS` | default 1 (exact: Metal's tessellator for uniform half-exact factors, the vendored D3D11 reference tessellator's patterns otherwise; `=3` the reference for all); adaptive and continuous quad tessellation draws whose patches all have uniform, half-exact factors use Metal's tessellator with the native domain shader (bit-identical to the emulation); `=2` all adaptive draws (drift at transition edges), `=0` emulation (Launcher "Native tessellation") | `metal_command_processor.cc`, `metal_pipeline_cache.cc` (`GetOrCreateNativeTessPipeline`) |
| `FABLE2_NATIVE_TESS_TRACE=1` | diagnostic, default off: logs the GPU family and tessellator setup once, then `[tess-trace]` once per (VS, PS, path) combination with the path each tessellated draw took (`tessellator`, `reference`, `emulation:<reason>`), its patch count, factor range (flags factors outside [1, 64]), depth function and depth write, plus per-path counts every 600 frames and dropped native draws | `metal_command_processor.cc` (`NativeTessTrace`) |
| `FABLE2_NATIVE_TESS_SNAP_UV=1` | prototype, default off (M3 white flashes): the native domain shader snaps Metal's tessellator uv to the 1/65536 grid on which every D3D11 domain point lies (the reference works in 16.16 fixed point). A no-op where the tessellator is exact (harness: identical on the M5), and the positions of a patch are then identical in every pipeline. The log confirms it: `[tess-trace] snap-uv: on|off` once, then `[tess-trace] snap-uv: native domain shader VS … compiled with the uv snapped` per domain shader | `metal_msl_translator.cc` (`snap_domain_uv`) |
| `FABLE2_FLASH_DETECT=<folder>` / `FABLE2_FLASH_INDUCE=1` | diagnostics for the flash kit ([flash-kit.md](flash-kit.md)), default off: per-frame reduced readback of the guest output and a transient-white detector (flash = cell mean, crack = whitest pixel of the cell) scored inside the built-in benchmark's sweep window (`fable2_flash_control` export); writes `flash.json`, `flash-frames.csv` and example PNGs to the folder. `FABLE2_FLASH_INDUCE=1` writes a white shape into every 40th frame (detector check) | `metal_flash_detect.h`, `metal_presenter.mm`, `src/core/fable2_benchmark.h` (`FABLE2_BENCHMARK_PLAN=flash`) |
| `FABLE2_NATIVE_TESS_BLENDED` | default on (2026-10-14, maintainer's decision); blended tessellated draws (the continuous terrain layers) with uniform, half-exact factors take Metal's tessellator in `FABLE2_NATIVE_TESS` modes 1 and 3; the triangle order inside such a draw can differ from the emulation where its own triangles tie in depth. `=0` keeps blended draws emulated (Launcher "Native tessellation: blended terrain layers") | `metal_command_processor.cc` |
| `FABLE2_TERRAIN_ADAPTIVE=1` | default off (testing); the terrain keeps the guest's adaptive tessellation factors instead of the `metal_fable_terrain_uniform_tessellation` maximum, and its opaque adaptive draws go to the native tessellator whatever the factors (correct edge mapping, so no cracks; the converter gives every edge control point 2's factor, bug pattern 78). Launcher "Adaptive terrain tessellation (fixes cracks natively)" | `metal_command_processor.cc` (`GetTessellationFactorRange`) |
| `FABLE2_TERRAIN_LAYER_MATCH` | default on, acts only with `FABLE2_TERRAIN_ADAPTIVE=1` (white flashes on the M3 Pro, `docs/adaptive-terrain-flashes-20261014.md`): the blended terrain layers (VS `C30A97D9`/`FB68A7F2`) take the per-patch factors of the opaque terrain patch under them, and in `FABLE2_NATIVE_TESS` modes 1 and 3 every terrain pass takes the reference pattern path, so all passes over a patch get bit-identical positions. `=0`: layers at the uniform maximum on Metal's tessellator (the flashes). Fable II Launcher "Adaptive terrain: layers follow the ground's detail (no white flashes)"; `[tess-trace]` logs `layer-match=` and matched/unmatched counts | `metal_command_processor.cc` (`MatchTerrainPatchFactors`) |
| `FABLE2_NATIVE_RECTS` / `FABLE2_NATIVE_POINTS` | rectangle lists (default on, `=0` off) expanded by the native vertex shader; point sprites (default 2: a native mesh function running the VS once per point; `=1` per-corner vertex expansion; `=0` off) instead of geometry-shader emulation; bit-identical (Launcher "Native rectangles", "Native point sprites") | `metal_msl_translator.cc`, `metal_pipeline_cache.cc` (`GetOrCreateNativePointsPipeline`) |
| `FABLE2_EMU_DRAW_STATS=1` / `FABLE2_NATIVE_TESS_DUMP=<dir>` / `FABLE2_SKIP_VS=<hash>[,<hash>…]` | diagnostics: `[emu-stats]` census of emulated draws; dump of the converter's emulation stages for `tools/shaderlab/tess_diff.mm` / `geom_diff.mm`; skip every draw of a vertex shader | `metal_emu_draw_stats.h`, `metal_pipeline_cache.cc`, `metal_command_processor.cc` |
| `FABLE2_TESS_FRAME_DUMP=<file>` (+ `FABLE2_TESS_FRAME_DUMP_FRAMES=first:every:count`, default 600:60:20) | diagnostic, default off: one JSON line per tessellated draw of the chosen frames (shaders, path, depth/blend state, polygon offset, VGT index range, terrain constants, tf16–19 addresses, per-patch factors, guest indices); `tools/tess_frame_match.py` pairs the terrain layers with the opaque patches under them | `metal_command_processor.cc` |
| `FABLE2_NATIVE_GEOM_PERSIST` | default on (2026-10-14, [prologue-stutter-20261014.md](prologue-stutter-20261014.md)): native point/rectangle/tessellation pipelines record their pipeline description (so the next launch prewarms them in the background) and are created through the binary archive; `=0` builds them on the draw path every launch. Launcher toggle |
| `FABLE2_SHADER_PREP_BANK` | default on (2026-10-15, [shader-precompile-20261015.md](shader-precompile-20261015.md)): the headless shader prep (`FABLE2_SHADER_PREP`) also translates and MSL-compiles every shader of the disc's own shader bank (`<game data>/data/Shaders/ShadersRelease.sbk`, read at run time; the game sets `FABLE2_GAME_DATA_ROOT`), so macOS's Metal cache is warm before the first play. `=0` off; `=<file.sbk>` another bank. `_THREADS=<n>` (default cores − 1, at most 11), `_VS=0\|all` (default: vertex shaders without vertex fetches), `_BUDGET_S=<s>` (default 120; 0 = none: no new work after this, most common variants first). Settings "Prepare all of the disc's shaders at install"; the player app runs it at the first prep scale, `tools/fable2_cache_warm.py` before its sessions (`--no-bank`) | `metal_pipeline_cache.cc` (`StartBankPrecompile`), `metal_shader_bank.h`, `metal_graphics_system.cc` (`RunShaderPrep`) |
| `FABLE2_XLATE_TRACE` | default on; `[xlate-trace] VS\|PS <hash>/<modification> <ms>` for every shader translation that took ≥ 10 ms, with its thread (first-play hitch attribution); `=0` off | `metal_pipeline_cache.cc` (`EnsureMetalTranslationReady`) |
| `FABLE2_ZPD_STATS=1` / `FABLE2_ZPD_DIAG=zero` | diagnostics (2026-10-14 workaround audit): `[zpd-stats]` counts of the game's occlusion queries; `zero` makes every finished query report 0 passed samples (shows what the game culls, upper bound of real queries' gain; never for play). Occlusion results are otherwise the SDK's fake 1000 (CR-04, the 2026-10-14 workaround audit) | `metal_command_processor.cc` (`ExecutePacketType3_EVENT_WRITE_ZPD`) |
| `FABLE2_TRANSFER_QUAD_STATE_RESET` | default on; `=0` stops resetting cull, winding, fill, depth bias and depth clip to the encoder defaults before EDRAM transfer quads drawn inside a draw pass (`docs/resolution-scale.md` §14, bug-patterns row 49) | `metal_render_target_cache.cc` |

| `FABLE2_TEX_LOAD_DIRECT` | resolution-scaled texture loads (resolve outputs at 2×/3×) write the texture from the untile compute through a raw uint view instead of a staging buffer + blit; byte-identical. `FABLE2_TEX_LOAD_DIRECT_VERIFY=1` diagnostic compares both paths; `FABLE2_TEX_BLIT_DIAG=skip\|dup<N>` diagnostics (`docs/resolution-scale.md` §15) | `metal_texture_cache.cc`, `backends/metal/shaders/texture_load_dest_texture.metal` |
| `FABLE2_RESOLVE_TO_TEXTURE` / `FABLE2_RTT_KEEP` / `FABLE2_RTT_FUSED` | at 2×/3×, scaled resolves also write the cached textures that sample them, and scaled textures are not re-untiled after neighbour/memexport invalidations; byte-identical. `FABLE2_RTT_VERIFY=1` diagnostic compares with a reference load; `FABLE2_RTT_DIAG=census\|skip`, `FABLE2_RTT_TRACE_BASE` diagnostics (`docs/resolution-scale.md` §17) | `metal_resolve_to_texture.cc`, `backends/metal/shaders/resolve_dest_texture.metal` |
| `FABLE2_SCALED_GROW_ORDER_FIX` | scaled-resolve buffer grows copy old contents in order on the open command buffer (`=0`: standalone copy that could miss resolves already encoded) | `metal_texture_cache.cc` |
| `FABLE2_SCALED_RESOLVE_PAGE_FILL` | default **on** (2026-10-14); at 2×/3× a resolve first uploads its destination range to shared memory, as at 1×, so the rest of each 16 KB host page it marks valid holds the guest data (a texture sharing the page with a small resolve loaded zeros: Bower Lake water heightmaps). `=0` old behaviour. Diagnostics: `FABLE2_DIAG_RESOLVE_DUMP=<dir>` + `FABLE2_DIAG_RESOLVE_RANGE=<lo>-<hi>` (compare with `tools/resolve_dump_diff.py`), `FABLE2_DIAG_TEX_VS=<hash>` (+`FABLE2_DIAG_TEX_FRAME`) (`docs/lake-water-20261014.md`) | `metal_direct_host_resolve.cc` (`PrepareResolveDestinationBuffer`), `metal_command_processor.cc` |
| `FABLE2_VFS_SKIP_DEV_DIRS` | the startup VFS walk of the game root does not descend into top-level `out/`, `.git`, `thirdparty` (dev checkout); opens below them resolve lazily (−0.6 to −0.9 s per boot) | `patches/sdk/vfs-skip-dev-dirs.patch` |
| `FABLE2_SHM_UNLINK_EARLY` | default on; the guest memory shm object's name is removed right after it is created, so the kernel frees it when the process ends (hard exit, kill, crash). `=0` restores unlink-at-destructor, which leaks the object until reboot on every exit (`docs/macos.md`, bug-patterns row 66) | `patches/sdk/shm-unlink-on-create.patch` |

Build-time: `FABLE2_CPU_TARGET=generic` (build environment) compiles the game, SDK and Metal plugin with
the old `-march=armv8-a` instead of `-mcpu=apple-m1` (the A14/M1 feature set every Apple Silicon Mac has).

Build-time: `FABLE2_CODEGEN_CONSERVATIVE=1` (build environment) or `-DFABLE2_CODEGEN_CONSERVATIVE=ON`
recompiles with CR/CTR/XER/reservation kept in `PPCContext` (see "Manifest highlights"). The FPSCR
rounding table fix (`patches/sdk/fpscr-rounding-per-arch.patch`) has no switch; build-time tests
`tests/fpscr_rounding_tests.cpp` and `tests/fctiw_rounding_tests.cpp` run with every build.

## Guest function-call tracing (fable2_func_trace.log)

To figure out what each recompiled function does, every guest function entry
can be logged by name. Codegen emits `REX_FUNC_PROLOGUE()` at the top of
every function in `generated/default/fable_2_recomp.*.cpp`; the build hooks
that one macro (via `src/core/fable2_func_trace.h`, appended to the recompiled
PCH after the generated pch — no generated files are modified) so each entry
logs its name to `fable2_func_trace.log` next to the exe. Consecutive calls
of the same function are run-length encoded (per thread) to keep the file
small:

```
GetNewGameLoadingGlobal
sub_82189708 x 4821
LoadingScreen_Virtual43
sub_82CC1BC0 x 3
...
```

(`x N` means that function was called N times in a row; a bare name is a run
of one.)

**Session summary:** call counts are accumulated separately and written to
`fable2_func_summary.log` (same folder), one line per function, sorted by
total count:

```
18422331 x MainRenderLoop_82B9CD68
9711204 x __restgprlr_28
54 x Story_FirstChildCombat
```

It's refreshed every 5 s while tracing (and once more on clean exit), so you
can watch it in another window to see at a glance what's being called; the
last write is at most ~5 s stale even if the game exits via ExitProcess.
Counts respect the same on/off + filter as the text log.

Off by default (one atomic load per call when off). Enable with:

```
set FABLE2_FUNC_TRACE=1            rem every guest function entry
set FABLE2_FUNC_TRACE_FILTER=LoadingScreen   rem optional: only names containing this
```

Either output can be turned off independently (default: both on):
`FABLE2_FUNC_TRACE_LOG=0` skips `fable2_func_trace.log` (summary only),
`FABLE2_FUNC_TRACE_SUMMARY=0` skips `fable2_func_summary.log` (trace only).

or at runtime from a named-function override (see `src/diagnostics/fps_probe.h` for the
override pattern): `Fable2FuncTraceSetEnabled(true)` /
`Fable2FuncTraceSetFilter("LoadingScreen")` /
`Fable2FuncTraceSetSubsOnly(true)` /
`Fable2FuncTraceSetLogEnabled(false)` /
`Fable2FuncTraceSetSummaryEnabled(false)` / `Fable2FuncTraceFlush()`
(declared `extern "C"` in `src/core/fable2_func_trace.h`).

**Naming mode** (`FABLE2_FUNC_TRACE_SUBS_ONLY=1`): log only the unnamed
guest functions - names matching `sub_` + hex digits - dropping named
functions, the `__savegprlr_*`/`__restgprlr_*`/`__savevmx_*` register
helpers, and `xstart`. Both the text log and the summary honor it, so the
summary becomes a ranked list of the unnamed hot functions to name next.
Composes with the substring filter (`FABLE2_FUNC_TRACE_FILTER=82B9` narrows
to an address range).

**Launcher:** `fable2-functrace.cmd` (staged next to the exe) does the env
var dance for you and renames the previous session's log to
`fable2_func_trace_prev.log` first:

Arguments are keywords in any order (`subs` enables naming mode,
`trace`/`summary` pick which output file(s) are written, the backend word
picks the GPU path, anything else is the substring filter):

```
fable2-functrace.cmd                  D3D12, trace every call
fable2-functrace.cmd vulkan           Vulkan, trace every call
fable2-functrace.cmd subs             naming mode: only sub_<hex> functions
fable2-functrace.cmd subs LoadingScreen   naming mode + substring filter
fable2-functrace.cmd d3d12 82B9 subs  D3D12, address-range naming mode
fable2-functrace.cmd summary          summary file only, no trace log
fable2-functrace.cmd trace            trace log only, no summary file
```

With no `trace`/`summary` keyword both files are written (the default).
`summary` is the cheap option for long sessions - it avoids the multi-GB
sequential log while still accumulating the call counts.

It writes its own lightweight log (same pattern as `fps_probe.log`) rather
than the SDK spdlog logger, which would be far too slow at Fable 2's call
rate. To remove the feature: delete the `target_precompile_headers` block in
CMakeLists.txt + `src/core/fable2_func_trace.h` and rebuild.



## Remote control (automated input channel)

`fable_2.exe` runs a localhost TCP **remote control server** so an external
automation harness can drive the guest gamepad over JSON-lines messages —
no human at the keyboard. Design doc: `plans/ai-remote-input-control.md`.

**Debug builds only.** This is a debugging/automation channel — it opens a
localhost TCP port and injects guest input — so it must not ship to players.
It is gated on the `FABLE2_REMOTE_CONTROL` macro, defined only for the Debug
build; Release / RelWithDebInfo builds compile out the pad driver + server
entirely (no port is opened and `fable2_control.py` is not staged).

- **Where:** `127.0.0.1:8791` by default (configurable, see below). One JSON
  object per line; every request gets exactly one response line; keep-alive
  connections are supported.
- **How:** a second synthetic pad driver
  (`src/input/remote_gamepad_driver.h`) is registered next to the keyboard driver,
  fed by the server (`src/input/remote_control_server.h`). It OR-merges with the
  human pads and is **not** gated on window focus, so the AI can drive the
  game while it's in the background.

Quick start (from the build dir, game running):

```
python tools\fable2_control.py ping
python tools\fable2_control.py press A --hold 120
python tools\fable2_control.py get-state
python tools\fable2_control.py script --file repro.json
```

Or speak the protocol directly (any language):

```
> {"cmd":"press","input":"RT","hold_ms":900}
< {"ok":true,"input":"RT","release_in_ms":900}
> {"cmd":"script","steps":[
      {"delay_ms":500,"op":"press","input":"LB","hold_ms":2000},
      {"delay_ms":1000,"op":"press","input":"A","hold_ms":80}]}
< {"ok":true,"duration_ms":2500}
```

`hold_ms` ≤ 0 (or omitted) means *hold until released/cleared*; a positive value
auto-releases after that many ms. In a `script`, each step's `delay_ms` is a **gap
since the previous step** on a single clock — the example presses LB at t=500 (held to
t=2500) and A at t=1500 (held to t=1580), so `duration_ms` (when the last input stops
being active) is 2500.

Commands: `ping`, `info`, `auth`, `press` (`input`, `hold_ms`, `value`),
`release`, `stick` (`input` = `StkLx`/`StkLy`/`StkRx`/`StkRy`, `value`,
`hold_ms`), `state` (sticky baseline: `buttons[]`, `triggers{LT,RT}`,
`stk{lx,ly,rx,ry}`), `clear`, `script` (atomic timed sequence), `get_state`,
`game_state` (current boot/menu state, see below), `cvar` (get/set any cvar
by name), `enable`/`disable`. Input names match the
keyboard-gamepad vocabulary (`A`/`B`/`X`/`Y`, `LB`/`RB`, `LT`/`RT`,
`Up`/`Down`/`Left`/`Right`, `Start`/`Back`, `L3`/`R3`, stick direction
shorthands). `StkLy` positive = forward (Fable 2 convention). `script` is a
single atomic message, so a repro sequence runs with no network round-trips
between steps.

### Game state

`game_state` reports which boot/menu screen the game is on so the AI can
navigate and verify its actions:

```
python tools\fable2_control.py game-state
> {"cmd":"game_state"}
< {"ok":true,"state":{"code":2,"name":"PressAScreen"}}
```

The classifier samples once per second on a background thread (independent of
the render loop, so it keeps working during the movie, which renders video with
no UI text) and reports one of:

| State | Meaning |
|---|---|
| `?` (Unknown) | Arena not mapped yet / undetermined (first ~1 s). |
| `PreMainMenu` | Splash / intro, before the "Press A" prompt. |
| `PressAScreen` | The "Press A to start" prompt is on screen. |
| `MainMenuMovie` | The idle movie is playing (no prompt / menu). |
| `MainMenu` | The main menu (reached by pressing A on the prompt). |

This follows the game's own state machine:
`PreMainMenu →(time)→ PressAScreen →(time)→ MainMenuMovie →(time)→
PressAScreen`, with `PressAScreen →(A)→ MainMenu` and
`MainMenuMovie →(A)→ PressAScreen`. The prompt vs. the menu are visually
indistinguishable (same element-list draw rate), so the transition into the
menu is driven by the A-press, observed on the final merged pad state (remote +
keyboard + physical — it works no matter which input drives A) and latched with
its exact timestamp. State changes are logged to the in-game logger
(`REXSYS_INFO`, same channel as the remote control) on transition only, and
every sample is written to `fable2_state_probe.log` when
`FABLE2_STATE_PROBE=1` is set.

Config (built-in defaults; `fable2_config.toml` has no `[remote]` section):
`enabled` (default `true`), `host` (`127.0.0.1`; `0.0.0.0` exposes it on all interfaces), `port`
(`8791`; if busy, `+1..+9` are tried, the bound port is logged at startup),
`token` (empty = no auth; when set, each connection's first line must be
`{"cmd":"auth","token":"..."}`). Every received command is logged (audit
trail) to `logs/`.

## Building

Everything the build needs is either in this repo or auto-fetched — **no
hardcoded paths**. Prerequisites (all standard tools):

- **CMake** ≥ 3.25 (on PATH)
- **LLVM** (clang/clang++/lld) — on PATH or the default `C:\Program Files\LLVM` install
- **Ninja** (on PATH, `%USERPROFILE%\bin`, or the WinGet package dir)
- **Internet** on first build — `build.cmd` auto-downloads the prebuilt
  ReXGlue SDK v0.10.0 (~100 MB) from the
  [official release](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.0)
  into `thirdparty\rexglue-sdk\`. A pre-existing SDK is used instead if found:
  `thirdparty\rexglue-sdk\win-amd64` first, then a sibling
  `..\rexglue-sdk-0.10.0-win-amd64\win-amd64`.

```
build.cmd               # configure + fable_2_codegen (runs rexglue codegen on the manifest)
build.cmd fable_2       # configure + build the full executable
build.cmd <target>      # any other CMake target
build.cmd -release [t]  # build as Release (-O3) instead of Debug (-r is short form)
```

The first `build.cmd` run fetches the SDK if needed, then configures and
builds — that is all a fresh checkout requires (plus the game content above).

Manual/advanced setup (normally not needed):

```
tools\setup_sdk.cmd      # fetch the prebuilt SDK only (what build.cmd auto-runs)
tools\setup_sdk_src.cmd  # fetch the SDK SOURCE for the Vulkan plugin: clones
                         # rexglue/rexglue-sdk at the pinned commit
                         # (f5337cdc), pins its submodules to the SHAs in
                         # thirdparty/rexglue-sdk-submodule-pins.txt, and
                         # applies thirdparty/rexglue-sdk-local.patch. LONG
                         # (several GB).
git                     # only needed for setup_sdk_src.cmd
```

The build also accepts explicit overrides if you keep the SDK elsewhere:
`cmake -DREXGLUE_SDK_ROOT=<prebuilt SDK root> -DREXGLUE_SDK_SOURCE=<SDK source>`
(`build.cmd` sets `REXGLUE_SDK_ROOT`/`REXGLUE_SDK_SOURCE` from its own search).

- Toolchain: the `win-amd64-debug` preset (Ninja + clang) by default. The SDK's public headers use clang builtins (`__builtin_bswap32`, `__VA_OPT__`), so plain MSVC `cl` cannot compile the generated code — a `win-msvc-debug` preset exists but only works for targets that don't compile `generated/`.
- **Release builds:** `build.cmd` defaults to the `win-amd64-debug` preset (`-O0`, `out\build\win-amd64-debug`). Pass the `-release` flag (short form: `-r`) to select the `win-amd64-release` preset (`-O3`, `out\build\win-amd64-release`):

  ```
  build.cmd -release fable_2_codegen
  build.cmd -release fable_2
  ```

  The release build stages the release runtime/GPU plugins (`rexruntime.dll`, `rexgpu-xenos.dll`) instead of the debug ones (`rexruntimed.dll`, `rexgpu-xenosd.dll`). **Debug builds are drastically slower at runtime** — the recompiled guest code, the runtime, and the Xenos GPU emulator all run at `-O0` with assertions enabled — so use Release for any performance-sensitive run. Omit the flag to build Debug again; the two build trees are independent and can coexist.
- Codegen runs as part of the build and re-runs automatically when `fable_2_manifest.toml` or `default.xex` change (tracked via the generated DEPFILE).
- A full clean build recompiles ~291 generated translation units (60,462 guest functions).

## Vulkan renderer

The goal is a Vulkan rendering path for the game (the SDK's Windows GPU plugin only ships a
D3D12 Xenos backend; the SDK's own Vulkan backend lives in `rexglue-sdk-src/src/graphics/vulkan/`
but is not compiled into the shipped plugin). Per the project constraint, all Vulkan work lives in
this repo. The SDK source itself is built as stock upstream **plus this
project's tracked local patch** (`thirdparty/rexglue-sdk-local.patch` —
see below); the upstream repo is never forked or pushed to.

**Stage 1 — foundation (DONE, verified).** A self-contained Vulkan pipeline that creates a window,
inits Vulkan, and presents a clear color, proving the whole stack on a real GPU:

```
build.cmd -release fable_2_vulkan_smoke
out\build\win-amd64-release\fable_2_vulkan_smoke.exe   # -> stable blue window
```

Pipeline: dynamic `vulkan-1.dll` loader (no import lib needed) → `VkInstance` → Win32 surface →
device (graphics+present queue) → swapchain (3 images) → render pass → per-image framebuffers +
command buffers (clear-color subpass) → semaphores/fences → present loop. Logs to
`vulkan_smoke.log` next to the exe.

Key implementation notes (hard-won):
- **Window via the SDK's UI framework, not raw Win32/SDL.** The window is created with
  `rex::ui::SDLWindowedAppContext` + `rex::ui::Window` from the *shared* `rexruntime.dll` — the
  same mechanism the game uses. Raw `CreateWindowExW` and the *static* SDL build both fail to
  create/init a window here; only the shared-SDL path (the `rex::ui::*` exports) works. The target
  links the `rexruntime.lib` import lib and stages `rexruntime.dll` next to the exe.
- `#define NOMINMAX` before `<windows.h>` is required (the SDK's `math.h` uses `std::min` /
  `std::numeric_limits::max`, which the Windows `min`/`max` macros would break).
- Image views backing the swapchain **must outlive the framebuffers** (destroying them early makes
  the driver fault in `vkCmdBeginRenderPass`).
- Vendored Khronos headers live in `thirdparty/vulkan` + `thirdparty/vk_video` (Vulkan 1.3.282).

**Stage 2 — render the game via Vulkan (in progress).** The SDK's Vulkan backend is a complete,
~1.4 MB renderer compiled into `rexgpu-xenos` only when `REXGLUE_USE_VULKAN=ON` (OFF by default on
Windows). Rather than re-implementing it, we build the SDK source **plus the
project's local patch** (`thirdparty/rexglue-sdk-local.patch`) into a Vulkan GPU
plugin and load it via the `gpu_plugin` mechanism, so the renderer can be **swapped at launch, no
rebuild of the game**:

```
tools\build_sdk_vulkan.cmd     # builds the SDK source with REXGLUE_USE_VULKAN=ON
                               # (auto-fetches it first via tools\setup_sdk_src.cmd:
                               # clone at the pinned commit + submodule pins +
                               # thirdparty\rexglue-sdk-local.patch, into
                               # thirdparty\rexglue-sdk-src) -> the plugin lands
                               # in <SDK source>\out\win-amd64\Release\rexgpu-xenos.dll
```

The SDK source is the stock `rexglue/rexglue-sdk` repo **plus this project's
local modifications** (`thirdparty/rexglue-sdk-local.patch`: the main-menu
crash fix in `exception_handler_win.cpp` + `xmemory.cpp`, and the Vulkan/
FPS instrumentation — see `docs/main_menu_crash_fix.md` and the patch
header). `tools/setup_sdk_src.cmd` reproduces that exact tree from the
tracked patch + submodule pins, so the source never has to live outside
this repo.

Two plugins are staged next to the exe, selected by the `gpu_plugin` argument:

```
out\build\win-amd64-release\fable_2.exe --gpu_plugin=xenos           # D3D12 (default; prebuilt plugin)
out\build\win-amd64-release\fable_2.exe --gpu_plugin=xenos-vulkan    # Vulkan (source-built plugin)
```

- `rexgpu-xenos.dll` is the prebuilt D3D12 plugin (staged by `GPU_PLUGINS xenos`).
- `rexgpu-xenos-vulkan.dll` is the source-built plugin (staged by CMake from the SDK build tree,
  Release-only). `Fable2App::OnPreSetup` loads it with `LoadGpuPlugin("xenos-vulkan", "vulkan")`,
  forcing the Vulkan backend (the plugin is compiled with both; the default `"any"` would pick D3D12).
- The source plugin must match the game's runtime: the Release source plugin imports `rexruntime.dll`,
  so it is staged for the Release game only (the Debug game links `rexruntimed.dll`).
- The SDK source location is resolved relative to this repo (`thirdparty/rexglue-sdk-src`,
  or a sibling `..\rexglue-sdk-src` for pre-existing setups) and can be overridden with
  `-DREXGLUE_SDK_SOURCE=<path>`.

## Manifest highlights (fable_2_manifest.toml)

Migrated from the XenonRecomp config (`XenonRecomp/fable2.toml`):

- 87 manual function boundaries. ReXGlue forbids the overlapping boundaries XenonRecomp allowed: three nested pairs were trimmed to end where the inner function begins, and `0x82C8D3F0` keeps its original full size with the nested `0x82C8D4E8` entry removed (the gap-fill otherwise extended the outer and emitted an illegal cross-function `goto`). Original sizes are recorded in the file's comments and in `XenonRecomp/fable2.toml`.
- 13 "gap seed" function entries (no size — the scanner sizes them) for branch targets the auto-discovery never registered: a chain of 8-byte thunks at `0x82C000F0`–`0x82C00128` plus stragglers (`0x82BEA25C`, `0x82CD7948`, `0x82C14D58`, `0x82F279D8`, `0x82E7E4F8`, ...).
- `setjmp_address = 0x83000200`, `longjmp_address = 0x82CA9260`, and two `[[invalid_instructions]]` data-table skips, carried over verbatim.
- The XenonRecomp register save/restore helper addresses (`restgprlr_14` etc.) have no ReXGlue equivalent and are preserved only as comments.
- Register options live in `fable2_codegen.toml` (included from `[entrypoint] includes`): `cr_as_local`, `ctr_as_local`, `xer_as_local`, `reserved_as_local` are on since 2026-10-11 (guest CPU about -10% at 60 FPS); `skip_lr`, `skip_msr`, `non_argument_as_local`, `non_volatile_as_local` stay off. `generated/codegen-override.toml`, written by CMake, turns the four back off when `FABLE2_CODEGEN_CONSERVATIVE` is on. (Before, these keys sat after the last `[[entrypoint.midasm_hook]]`, where TOML made them keys of that hook, so codegen never read them.)
- Known runtime risk: codegen logs a handful of non-fatal "Unresolved conditional branch" warnings (e.g. `0x82C8D408`, `0x82C99E74`, `0x82C9A0BC`); those paths emit `REX_FATAL` if hit.

## Function naming rule (important)

A manifest `name` becomes a C symbol: codegen emits the function body as
`__imp__<name>` (extern "C"). A name must therefore NOT equal an XAPI export
of `rexruntimed.dll` (the DLL exports `__imp__<XAPI name>` for every
xboxkrnl/xam hook). If it does, the local definition silently shadows the
DLL import at link time (no linker error), and the generated import-thunk
registration (`fable_2_register.cpp`) routes XAPI calls into the guest
function body instead of the SDK kernel hook. That is what broke audio when
the game-internal KeWait re-implementations were named
`KeWaitForSingleObject` / `KeWaitForMultipleObjects`: the xboxkrnl import
thunks (0x832B28AC / 0x832B2CDC) started landing in the guest wrappers,
whose timeout field is milliseconds while kernel callers pass 100ns units
(0 = infinite). They are named `..._Guest` for this reason — keep it that
way. Check before committing name changes (upstream's tool; not in this repo):

```
python tools/check_manifest_collisions.py
```
