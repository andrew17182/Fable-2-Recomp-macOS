# New-game stutter: native geometry pipelines not persisted (2026-10-14)

Report (maintainer, M3 Pro, release app f667634, 2× uncapped): heavy stuttering when a new game starts (childhood
Bowerstone), on the second launch too, and it does not settle. Log: `benchmark-results/fable_2-m3-newgame-stutter.log`
(two sessions, 16:12 cold and 16:20).

## What the M3 log shows

- Hitches are `xlate` and `pso_emu` on the GPU Commands thread: 37 hitches / 12.8 s in session 1 (xlate 6.3 s,
  pso_emu 2.7 s), 21 / 9.8 s in session 2 (xlate 4.6 s, pso_emu 2.3 s); single frames up to 3.6 s.
- Session 2 rebuilt the same work: 52 `[native-points]`/`[native-tess] pipeline` lines (58 in session 1) and 120
  shader translations on the GPU thread, every one of them (hash and size) also translated in session 1.
- Shader storage did persist: session 2 loaded 157 shaders (= every unique shader of session 1), 230 pipeline
  descriptions. The hard exit is not losing writes.
- Native MSL compiles did not hit macOS's shader cache at all on that Mac: session 2 `[native-first] 128 native
  shaders … MSL compile 49220 ms total` (~385 ms each; session 1: 25.7 s), `[startup-par] 135 in 4012 ms`. The same
  build on the M5 Max: 1.1 s and 10 ms (cache hits). The M3 benchmark-kit runs (own bundle id) hit too (`MSL compile
  3 ms total`), so the miss is specific to that install; cause not identified yet (see "Open").

## Cause (both Macs)

In no-converter / native-first builds, point/rectangle lists and quad tessellation draw through native pipelines
(`GetOrCreateNativePointsPipeline`, `GetOrCreateNativeTessPipeline`). These never went through
`GetOrCreateGeometryPipelineState` / `GetOrCreateTessellationPipelineState`, the only places that record a pipeline
description, and they were not in the binary archive. Storage init's native prewarm (`PrewarmNativeGeometry`,
bug-patterns 92/94) prewarms only from recorded geometry/tessellation descriptions; a fresh install records none.
So every launch translated their shaders, compiled the native MSL and built their pipelines on the draw path. Where
Metal's system cache serves the MSL (the M5), this costs ~0 ms; where it does not (the M3 install), it is
50–400 ms per shader, in gameplay.

## Fix: `FABLE2_NATIVE_GEOM_PERSIST` (default on)

`metal_pipeline_cache.cc`:
- A native point/rectangle/tessellation pipeline that is built records its pipeline description (geometry:
  `BuildGeometryPipelineDescription`; tessellation: `BuildTessellationPipelineDescription`, with `auxiliary3 = 1` for
  the reference-pattern (CPU) entry kind) and its shaders in shader storage (`StoreNativeGeomDescription`). These are
  the same records the emulation paths write, so the next launch's storage init queues them to the existing
  background prewarm, which already chooses the native path with the draw path's own checks.
- The pipelines are created through the binary archive (`NewNativeGeomPipelineState`, set 0): looked up without
  compiling in earlier segments, compiled and added on a miss, like `CreatePipelineFromHandle`.
- Same pipelines, same descriptors, same functions; only when they are built changes. `=0`: the old behaviour.
  Dev Launcher toggle "Remember particle and tessellation pipelines".

## Measurements (M5 Max, 2×, new game, ~40 s of prologue after the world draws)

Harness: a scratch state with no saves (the main menu's first entry is New Game), the menu presses of
`tools/fable2_cache_warm.py` (title, New Game, hero card, Esc past the opening movie), then 13 s standing and 27 s
walking with camera turns. Each arm: a cold first launch, then a second launch from its cache with a **fresh
`FABLE2_MSL_SALT`** (applies to native MSL too since this change), which makes every MSL compile miss the system cache
as on the M3. Scripts and results: `out/validation/prologue-stutter/` (main checkout, KEEP).

| Second launch | session hitches (total) | xlate | pso_emu | gameplay window: max ms / 1% low | draw-path translations |
|---|---|---|---|---|---|
| prologue 2× uncapped, off | 15 (4.8 s) | 2832 ms | 6 ms | 217 / 63 | 99 |
| prologue 2× uncapped, on | 6 (0.57 s, all loading `idle`) | 0 | 0 | 15.6 / 106 | 34 |
| prologue 1× uncapped, on | 5 (0.51 s, loading) | 0 | 0 | 19.4 / 134 | — |
| prologue 2× 60 FPS, on | 6 (0.56 s, loading) | 0 | 0 | 18.9 / 54 | — |
| Bower Lake 2× uncapped, off | 18 (5.8 s) | 3414 ms | 6 ms | 219 / 65 | — |
| Bower Lake 2× uncapped, on | 12 (0.96 s) | 0 | 54 ms (one tessellation pipeline first used in this run) | 62 / 101 | — |

On: `[native-prewarm] 42 native tessellation / point / rectangle pipelines prepared in the background … in 228 ms;
binary archive hits 41, misses 0`. Not prologue-specific: Bower Lake has the same pattern.
With the system cache warm (no salt), off and on are both hitch-free on the M5 (149–160 FPS, max ~16 ms).

Image check (Bower Lake 2×, frame-matched stills, `--cache-from` a cache with the archived native pipelines, `ngp-on3/4` vs `ngp-off1/2`, run one at a time): cross median 0.25 vs A-A floor 0.35, and on3 × off2 matches to median 0.066. The tool still says `beyond-noise` because of 16 hot blocks in the one pair on4 × off2 (two small areas, max 18.6). The other three pairs have none. The pipelines are the same descriptors and functions either way.

## Metal system cache and parallel games

A game's MSL compiles are not persisted in macOS's per-bundle-id shader cache while another **game** process with the
same bundle id runs: with a second game alive (same id, other salt), a run compiled 128 native shaders in 22.0 s, and
the next run alone with that salt took 24.5 s again (no hits). One run later, again alone, it took 2.3 s. The release
player app's own window does not cause this: player and game both re-identified to one probe id, player open, the next
run alone hit the cache (1.2 s). Native MSL is deterministic across launches (dumped sources identical except the
learned NS2 variants).

So parallel agent games, which all used `org.fable2recomp.metal`, kept losing each other's compiles. Agent slot builds
now get their own id `org.fable2recomp.metal.agent-N` (`tools/build_macos_metal.py` → `package_macos_app.py
--bundle-id`). The main checkout's build, the playtest copy, the release and portable apps and the kits keep their ids.
The first launch of a slot build after this change starts with an empty system cache for its new id.

## Also fixed

- `tools/fable2_test_control.py launch` always passed `--vsync=true`, which outranks `REX_VSYNC=0`, so its
  "uncapped" runs (MCP, direct launches; not f2diag, which goes through `fable2_dev_session`) ran at the 30 FPS guest
  cap. It now passes `--vsync=false` when `REX_VSYNC=0`.
- `fps_60=false -> 30 FPS` in a log with "uncapped" in the window title is expected: uncapped = guest 30-FPS mode
  with vsync off (`REX_VSYNC=0`); the M3 sessions ran uncapped (baseline 7.3 ms).

## Open

- Why the M3 release install never hit the system cache. Check after a session:
  `ls -laR "$(getconf DARWIN_USER_CACHE_DIR)org.fable2recomp.metal.release/com.apple.metal" | head -40` and
  `tail -30 ~/Library/Application\ Support/Fable\ II\ for\ Mac/logs/player.log` (a second game process with that id,
  such as a shader prep, running at the same time would explain it).
- With no system cache at all, startup still compiles the native MSL of the stored pipelines (M3: `startup-par` 4 s),
  but before gameplay, on the loading screen. A persistent MSL library cache would need the Metal toolchain's
  `metal` compiler (not on every Mac, and not shown to be identical to the runtime compiler); not done.
- The learned NS2 variants that are demoted at once on the next launch (`1024 of the last 1024 guard checks missed`
  in the M3 session 2) cost background compiles only; not looked at.
