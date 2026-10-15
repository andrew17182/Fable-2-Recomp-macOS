# Fable II Recomp for macOS

**Based on [himdo/Fable-2-Recomp](https://github.com/himdo/Fable-2-Recomp).** If you're on Windows or Linux, use that project.

**This fork is mostly AI-generated.** Issues, comments and pull requests are welcome, but they
will be processed by AI. In other words, don't expect much human interaction. This is mostly my
own experiment in single developer/AI coding. **Forks are encouraged:** take it wherever you like.

A native Apple Silicon port of the Fable II (Xbox 360) static recompilation, with a Metal renderer.
**It is very much an early work in progress.** Not yet tested all the way to the game's ending.

## Features beyond the original game

- **Turbo mode:** F7 runs the whole game 2× faster (configurable).
- **Quick save / quick load:** Launcher boots straight into any save
  and keeps a timestamped library of every save.
- **Faster loading**
- **60 FPS or uncapped,** with an Auto mode that picks 30 or 60 FPS based on your Mac's GPU.
- **Higher internal resolution** (2× or 3×, or `"auto"` to pick from the GPU, `[graphics] resolution_scale`), with bloom and glow kept at their original size (`effects_native_scale`).
- **Windowed or fullscreen** (`[graphics] fullscreen`); the window reopens at its last size and position.
- **Skip intros, fast title screen, shorter save notices.**
- **Keyboard and mouse support,** plus an in-game Lua script menu (F6) for tinkering.
- **FPS counter:** F9 shows frame rate and frame time in a corner (off by default).

## Play (download)

> **Not yet tested through to the ending.** Testing so far covers the early and mid game (Bower Lake,
> Bowerstone, the Heroes' Guild, Old Tomb). Later parts of the story may have bugs. Keep backup copies of
> your saves, and please report problems in the issues.

You need an Apple Silicon Mac with macOS 15 or later, about 8 GB of free disk space, and **your own
Fable II disc image** (`.iso`). The port is tested only with one disc image, the Platinum Hits release; the app
checks its `default.xex` by hash and explains if your disc is a different version. The download contains
no game content: on first launch the app copies the game files from your disc and builds the game on
your Mac.

1. Download **`Fable II.zip`** from the [Releases page](https://github.com/andrew17182/Fable-2-Recomp-macOS/releases), unzip it, and drag **Fable II** to Applications.
2. Open it. macOS says it "could not verify" the app, because it is not notarized. Click **Done**, then
   open **System Settings → Privacy & Security**, scroll to Security, click **Open Anyway** next to
   "Fable II", and confirm with your password. This is needed once per downloaded version.
3. Choose your disc image. The game files are copied into `~/Library/Application Support/Fable II for Mac`.
4. If asked, install Apple's free **Command Line Tools** (macOS shows its own installer; Xcode is not
   needed). The app downloads CMake by itself if it is missing.
5. The app translates and compiles the game code once. "Show details" shows every command it runs. After
   that it opens to the Play window. The first time you play, shaders are compiled as they are needed, so
   expect a few short hitches once; they are remembered after that.

First-launch build time (after the disc copy, which takes a few seconds to a few minutes):

| Mac | Build |
|---|---|
| M5 Max, 18 cores (measured) | about 1.5 min |
| 16 GB, 10–12 cores (estimate) | about 3 min |
| M1, 8 GB (estimate) | about 5–6 min |


## Build from source

### Requirements

- An Apple Silicon Mac. An M-series Max (24+ GPU cores) is recommended for 60 FPS; Auto picks 30 FPS on smaller GPUs.
- Xcode (free, App Store) with its Metal Toolchain component. The setup app checks the rest
  and can download CMake and the Metal Toolchain for you.
- About 13 GB of free disk space.
- **Your own copy of Fable II.** The port is tested only with the Platinum Hits disc image, whose
  `default.xex` the build checks by hash. No game content is included or will be accepted in this repo.

### Getting started

1. Double-click **`Install.command`**. It asks for your Fable II disc image and builds
   everything (a few minutes; about 4 on an 18-core Mac). If macOS blocks it, right-click it
   and choose Open.
2. Play: click **Play** in the setup window, or open **`Fable II.app`**, which setup places in
   this folder. **`Fable II Dev Launcher.app`** (same folder) picks a save and boots straight into it.

Manual alternative: extract `default.xex`, `data/`, `$SystemUpdate/` and `nxeart` from your
disc image into the repo root, then run `python3 tools/build_macos_metal.py`.

Another Mac (no Xcode needed): `python3 tools/fable2_portable.py build` makes a zippable app that
asks for the disc image on first launch ([Portable build](docs/portable-build.md)).

Details ([all docs](docs/README.md)): [Setup app](docs/setup-app.md) · [macOS build](docs/macos.md) · [Metal renderer](docs/macos-metal.md) ·
[Turbo](docs/turbo.md) · [Full reference: config, controls, Lua, tracing, Windows/Vulkan
notes](docs/reference.md)

## Origin

From the upstream author (himdo):

> First and for most this project was made to test the capabilities of local and open source AI.
> This project was made using Qwen3.8 27b. After about an hour it got the game running. Then after
> a large amount of human trial and error I was able to get enough of the functions mapped to be
> able to complete the game. I currently do not consider this to be fully complete as I have yet
> to 100% the game. The sha256 of the iso that started this project is:
> "685a0d3bea9718812f17bcd155907a5359a548b6d3d8342dd2a6c944f45e35ff" and its the Fable 2 GOTY for
> USA and Europe.

Built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). Fable II is © Microsoft /
Lionhead Studios. This project is unofficial and not affiliated with them.
