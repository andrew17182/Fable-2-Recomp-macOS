# Remembered and offline shader seeds (2026-10-14)

Maintainer (2026-10-14): "why don't we just remember what it needs? Why are we even checking? We have access to the
code. Can't we preemptively do this?" Before this, every portable/kit package ran three gameplay sessions to warm a
shader seed, and the install flow played ~45 s of a new game to warm the player's cache. Now:

1. **Packaging remembers.** The seed comes from what is already known for the build's shader code; no game runs.
2. **Offline preparation.** When nothing is remembered, and at install time, a headless run of the game binary builds
   every remembered shader and pipeline with no save, no title screen and no game loop.
3. **The shader lists can be rebuilt from the player's own disc** with a small recipe (no microcode).

## 1. Packaging: remembered seed (`tools/fable2_portable.py`, `tools/fable2_benchmark_kit.py`)

`resolve_seed` picks the first match and logs it (`[seed] source: …` on stderr, `shaders.source` in the result and in
`native-shaders/manifest.json`):

| Order | Source | When |
|---|---|---|
| 1 | `agent-cache` | the agent shader cache (`tools/fable2_agent_cache.py`) has the build's `shader_cache_key` (`metal-build.json`): its latest generation, cloned under the shared lock. NS1/NS2 (metal-opt) records are dropped: that cache carries hundreds from before the native translator, which replaced them |
| 2 | `seed-store` | `MAIN/out/shader-seed-store/<key>/native-shaders` (ignored): every package stores its seed there; newest 4 keys kept |
| 3 | `prep` | the newest remembered shader/pipeline lists (any key: they hold only game content) → `tools/fable2_shader_prep.py` (section 2) at each `--shader-scales` |
| 4 | `warm-up` | the old warm-up sessions (a save from the save library, gameplay at each scale) |

`--seed-source auto|agent-cache|seed-store|prep|warm-up` forces one (tests). `--shaders DIR` and `--no-shaders` are
unchanged. `FABLE2_SEED_KEY=<key>` pretends another key (tests).

**Kit end-to-end test.** `fable2_benchmark_kit.py build` runs the packaged kit as on a fresh Mac
(`fable2_benchmark.py run --kit`, empty state, disc image, no Xcode, every arm, ~5 min) only with `--e2e`, or by
default (`auto`) when the kit/benchmark code changed since the last kit that passed it. The inputs
(`E2E_INPUTS`: benchmark orchestrator, kit/benchmark/portable packagers, `AppLauncher.m`, `FableIIPrepare`,
`fable2_benchmark*.h`) are hashed into `MAIN/out/benchmark-kit/e2e-state.json`. `--no-e2e` never runs it. `--iso`
defaults to the first `../ISO/*.iso`.

Packaging time (M5 Max, 2026-10-14, slot build; another agent's perf sweeps ran meanwhile, so lock waits are listed):

| Source | Package time | Of which lock waits | App / zip | Seed |
|---|---|---|---|---|
| `warm-up` (before; 3 gameplay sessions) | 133.6 s | ~68 s behind another agent's perf runs, 14 s build marker | 194 / 70 MB | 33 MB, 915 records |
| `prep` (offline, warm OS cache) | 34.8 s | 1–3 s game-slot waits | 194 / 70 MB | 33 MB, 894 records; prep 6.2 / 3.3 / 6.5 s wall at 1×/2×/3× |
| `seed-store` | 30.6–34.0 s | 14–18 s build marker | 194 / 70 MB | 33 MB (the stored one) |
| `agent-cache` | 19–36 s | 0–18 s build marker | 251 / 105 MB | 90 MB, 2,247 records after dropping 1,175 NS1/NS2 |

Without lock waits a package is ~16–20 s (two `swiftc` compiles, signing, the zip); the warm-up alone used to add
~55 s. The agent-cache seed is larger: generation 0 of every key is a clone of the player's live cache, so it also holds
records from before the converter pin (2026-10-12) that this build never looks up. They are harmless; compacting
the agent cache would need a pass that keeps only records a session read (not done). A seed-store hit is preferred on
the next package anyway, because every package stores what it shipped.

## 2. Headless shader preparation (`FABLE2_SHADER_PREP`)

The game's shaders are not in the recompiled code. They are Xenos microcode in the disc's shader bank (section 3),
loaded by the game and handed to the GPU at draw time; the port remembers each one it sees in the shareable lists
(`cache/shaders/shareable/4D5307F1.xsh`: microcode; `4D5307F1.metal.xpso`: pipeline descriptions = shader hashes,
modifications, render-target formats, blend state). At startup `MetalPipelineCache::InitializeShaderStorage` already
rebuilds everything in those lists: translations (native MSL first, converter as fallback), the native-shader pool,
pipeline creation into the binary archives, the transfer / fused-resolve / tile prewarms, and (agent-2, 2026-10-14) the
native rect/point/tess pipelines from the stored geometry list.

So the offline step is the game binary itself in a mode that stops before the game: `FABLE2_SHADER_PREP=1` (log only)
or `=<status.json>`:

- runs the normal storage init and warm-up, hides its window and Dock icon, and **never resumes the guest** (no game
  loop, no save, no input, no title screen);
- waits on the UI thread until every queue is empty (`ReadinessBackground()`: pipelines incl. native prewarm, native
  shaders, ns-spec; `XferPrewarmPending`, `TileResolvePrewarmPending`, `RttPrewarmPending`), held for a 0.5 s settle
  because work hands over between queues; `FABLE2_SHADER_PREP_TIMEOUT_S` (1800) is a guard;
- saves the binary archives and shader storage (`ShutdownShaderStorage`), writes the status file, logs the clean-exit
  marker and exits 0 (3 on timeout). The status file (rewritten every 0.25 s) has `phase`, `done`, `ok`, `pending`
  per queue and the counters (pipelines compiled + ms, archive hits, native shaders, optimizer work).
- Under the app's bundle id it also fills macOS's own Metal cache for that app
  (`$(getconf DARWIN_USER_CACHE_DIR)/<bundle id>/com.apple.metal`), which is what the emulation and native
  rect/point/tess pipelines (not archivable) and the run-time MSL compiles rely on.

Could this be done without the game binary? Pipelines can only be created in-process (`MTLDevice`), and
`MTLBinaryArchive` / the OS cache are per process and bundle id, so a separate tool would duplicate the whole
pipeline cache. The translator front end alone (`fable2-msl-xlate`) produces MSL but not the runtime's cache format
(native MSL is compiled each launch with `newLibraryWithSource`; only the OS cache and the binary archives persist it).
The game binary in this mode is the "tiny headless process": it starts the runtime only up to storage init.

`tools/fable2_shader_prep.py run --from CACHE --out CACHE [--scales 1,2,3] [--lists-only] [--app APP]` runs it as a
background test session per scale (pid-scoped, input-isolated, never the player's state), each scale continuing from the
previous cache. `lists` prints the newest remembered lists.

## 3. The shader bank and the recipe (`tools/fable2_shader_bank.py`, `tools/macos/Prepare/ShaderLists.swift`)

`data/Shaders/ShadersRelease.sbk` (653 KB; `Shaders.sbk` is a debug twin): `ShaderBankFile` header, a constant-name
table (`g_BloomSampler`, …), then 147 entries, each a zlib stream (no final block) of D3D shader containers (magic
`0x102A1101`; 583 in the release bank). Not LZX, as earlier notes assumed (`docs/engine-options-20261011.md` §2, §9).

Against the remembered list (222 shaders: 144 pixel, 78 vertex):

| | Count | How it maps to the bank |
|---|---|---|
| Pixel shaders | 144 / 144 | byte-identical slices of a bank entry |
| Vertex shaders | 70 | a bank slice with D3D's bind-time vertex-fetch patching: 1,099 dwords in all differ (≤ 40% of a shader) |
| Vertex shaders | 6 | byte-identical |
| Vertex shaders | 2 (`1E6883FC…` 27 dwords, `B6C9863F…` 24 dwords) | not in the bank (most likely D3D-internal shaders in `default.xex`); their pipelines are built at first use |

A **recipe** lists per shader: hash, type, bank + entry + byte offset + length, the patched dword values, and a
SHA-256 of the result; plus the `.xpso` (base64). 145 KB JSON (29 KB gzipped). `rebuild` (Python) and
`FableIIPrepare --target GameData --shader-lists recipe.json --cache DIR` (Swift, Compression + CryptoKit, 0.12 s)
write a `.xsh` byte-identical to the original for the 220 shaders, and the `.xpso` unchanged.

## 4. Install time (player app)

After the game files are ready (extraction or reuse), the player app (`tools/macos/Player/main.swift`,
`ShaderPrepRunner`) shows **Preparing shaders…** in the welcome window's progress UI: one `FableIILauncher` run per
scale (1×, 2×, 3×) with `FABLE2_SHADER_PREP=<state>/logs/shader-prep-<n>x.json`, `FABLE2_RES_SCALE=n`, the player's
settings environment and `FABLE2_TEST_BACKGROUND=1`. The bar follows the status file (pending queues per scale);
**Skip** stops it (the game then compiles during play, as before). It releases the state folder's `app.lock` while
the child runs and takes it back after. A finished run writes `<state>/cache/shader-prep.json` with a stamp (version,
seed id, bundle version), so it runs once per build/seed. `FABLE2_PLAYER_SHADER_PREP=0` disables it. Lines go to
`logs/prepare.log` (`[player] shader prep 2x: done in … s`).

The private portable app gets its lists from the bundled seed (`AppLauncher.m` copies it before the game starts).
A public release would ship the recipe instead (section 6) and run `FableIIPrepare --shader-lists` first; the Swift
side is built and tested. The release packager has it as `tools/fable2_release.py build --shader-recipe`, but the
release does not ship it (decided 2026-10-14; measured in `docs/release-app.md`, "Shaders on the first launch").

Check (2026-10-14, packaged portable app, scratch state, `FABLE2_TEST_BACKGROUND=1`): with files already in place and no
stamp, the player went straight to the shader step: 1× 20.6 s (984 pipelines compiled), 2× 10.9 s, 3× 10.7 s, 42.3 s in
all, no game window; it then wrote the stamp and showed the Play window. A new build or seed (stamp mismatch) runs it
again before Play; with "Skip launcher" or `--play` it runs first and then plays.

`tools/fable2_cache_warm.py` (the Install.command new-game session until 2026-10-19; Setup now runs this headless
prep too, `--headless`, `docs/setup-app.md` "Shaders"); section 5 compares the two.

2026-10-15: with no lists at all (a first install of the release app) the prep still runs and compiles the disc's
own shader bank (`FABLE2_SHADER_PREP_BANK`, `docs/shader-precompile-20261015.md`); the recipe below is not needed
for the pixel shaders.

## 5. Validation (M5 Max, macOS 27.0.1, 2026-10-14)

Truly cold: each arm cleared macOS's Metal cache of the benchmark identity `org.fable2recomp.metal.benchmark.dev` first
(as the kit's cold arm does), then the benchmark (`FableIIBenchmark`, Bower Lake save, headings8, 60 FPS cap) ran 1×,
then 2× and 3× continuing from that cache, as a player's first launches would. Other agents' games ran during the prep
sessions (prep times are pessimistic); the benchmark arms held the perf lock alone. Evidence:
`out/validation/offline-seed-20261014/` (KEEP; `validate.py` is the harness, `record.json` per arm).

| Arm | Install step | Install time | First launch 1×: boot / ready after gameplay / sweep hitches (pso ms) | First 2×: boot / hitches | First 3×: boot / hitches | Binary-archive hits at warm-up (1×/2×/3×) |
|---|---|---|---|---|---|---|
| A: nothing (empty cache) | — | 0 | 7.2 s / 19.6 s / 4 (pso_emu 168, xlate 10) | 5.5 s / 1 (pso_emu 51) | 5.1 s / 1 (pso_emu 51) | — / 126 / 132 |
| B: offline prep from the disc recipe | rebuild lists 0.01 s + prep 1×/2×/3× | **47.9 s** (26.4 + 10.6 + 10.9 wall; 18.2 / 5.9 / 6.6 s in-process) | 5.8 s / 9.4 s / 2 (pso_emu 91, **no xlate**) | 5.2 s / 1 (pso_emu 50) | 5.1 s / 1 (pso_emu 51) | **392 / 399 / 426** |
| C: today's install warm-up (`fable2_cache_warm --new-game`, scales 1,2,3) | title + 45 s of new game + 2 title sessions | **169.6 s** | 4.9 s / 9.9 s / 3 (pso_emu 134, xlate 10) | 4.9 s / 1 (pso_emu 51) | 5.1 s / 1 (pso_emu 50) | 131 / 165 / 170 |

- **Coverage.** The prep built every pipeline in the remembered list (511 queued, 0 skipped; 825 pipelines compiled
  cold at 1×, 45 s of driver time summed, then 212 per extra scale), 254/254 shaders native (0 fallbacks). At the first
  launches 392–426 warm-up pipelines came from the binary archive, against 126–170 after today's warm-up, whose
  new-game prologue does not reach Bower Lake. B had no `xlate` (shader translation) stall in gameplay; A and C had one.
- **What is left: the native rect/point/tess pipelines.** Every arm still builds them on the draw path: B 19 tess
  (0.8–1.0 s summed per launch) + 23 point pipelines, A/C 32–36 + 41–44. They are the remaining `pso_emu` hitches
  (one ~50 ms hitch at 2× and 3× in every arm). They are not in the stored list's prewarm in this build. Agent-2
  (no-converter, 2026-10-14) is adding a prewarm of them from the stored geometry list, counted in
  `ReadinessBackground().pipelines_pending`, so the prep run builds them too without a change here (and fills the OS
  cache they depend on, since they cannot go into a binary archive).
- **Boot.** Boot times are within run-to-run noise between B and C (4.9–5.8 s); A's 7.2 s is the cold driver work.
  "Ready after gameplay" (no background shader work left) halves with either install step (19.6 → 9.4–9.9 s).
- **Time.** B takes 48 s against 170 s for today's warm-up, needs no save and no input, shows no game window, and covers
  every remembered area instead of the prologue. Estimated base M1 (4 performance cores against 6, slower cores; the
  driver compiles dominate, ~45 s summed at 1×): 2–3× the M5 Max, about 1.5–2.5 min in all.

Warm OS cache (the build Mac, the normal bundle id): 1× prep in 1.2 s in-process (4.5 s wall), 400 pipelines in 5 ms
summed; packaging prep runs are 3–7 s per scale.

## 6. Release question (decided 2026-10-14: ship nothing)

Can the remembered lists ship in the public repo or app? What they are:

- `.xpso` (61 KB): pipeline descriptions: guest shader hashes (XXH3 of microcode), translator modification bits,
  render-target formats, sample counts, blend/write-mask state. Derived metadata; no code or assets.
- recipe (145 KB): per shader its hash, bank location, the patched vertex-fetch dwords (1,099 dwords: these are
  instruction words D3D generated from the game's vertex declarations) and a SHA-256.
- `.xsh` (232 KB): the microcode itself. Game content: never ship.

Options:

1. **Ship nothing** (today). Installs run the new-game warm-up (Install.command) or nothing (player app); first
   launches compile.
2. **Ship the `.xpso` + recipe** (hashes, offsets, render state, patched fetch dwords). The install rebuilds the
   `.xsh` from the player's disc and runs the headless prep: full coverage of every area played on the build Mac,
   no game run visible to the player. Closest to "derived metadata"; the fetch dwords are the one part that is
   instruction data rather than hashes.
3. **Ship only the `.xpso` + bank locations for exact matches** (no patched dwords): pixel shaders and 6 vertex
   shaders rebuild; the 70 patched vertex shaders cannot, so most pipelines (which need both) would not prewarm.
   Little value.
4. **Download on first run** from a project server instead of bundling (same content as 2, separate from the repo).

Either way the rebuild checks every shader against its SHA-256 and leaves out any that differ (another disc revision;
tested with a synthetic bank in `tests/shader_seed_tests.py`); those fall back to option 1. The recipe also records the
banks' SHA-256 for diagnosis.
