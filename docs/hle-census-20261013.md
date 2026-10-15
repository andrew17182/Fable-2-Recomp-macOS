# HLE census: Gate G0 (2026-10-13)

Branch `agent-2/20261013-1439-hle-census` (agent slot, from `macos` 0ae172c). M5 Max. Diagnostic only: every switch
added here is off by default and costs one cached-bool test when off. Answers §7.3 of `engine-options-20261011.md`
(Gate G0) and feeds Gate G3, framed by the goal **"emulate as little as possible, faithfully"** for an M3 Pro at 2×
and 60 FPS.

Scenes (headings8 plan; 60-capped for census runs, uncapped for timing):
- Bower Lake (outdoor camp): `hero1-dog-live_2026-10-11_11-20-20`;
- Old Tomb (indoor): `hero1-dog-live_2026-10-07_14-45-08`;
- Bowerstone (town): `hero1-dog-live_2026-10-04_14-07-34`. The childhood save sits on a 30 FPS screen with 28 draw
  packets per frame, so it was not used.

Every run ended `ok`, 0 AV, no contention, and our pid was gone after close (`pgrep_after_close` 1). Evidence,
scripts and copied CSVs: `out/validation/hle-census-20261013/` (KEEP).

## 0. Answers in one page

| Question | Answer |
|---|---|
| ≥ 95% of draws through the three draw functions? | **No. 56% (Bower Lake), 77% (Old Tomb), 78% (town).** The game builds draw packets in **13 guest functions** (15 counting two init-only ones), called from **34–39 call sites per scene**. Through all 13: **86–93% of issued draws, and 100% of the draws that write anything.** The rest are 24 static point-list draws with colour and depth writes off (§2). |
| Recognisable mesh / material objects? | **One level up, yes; at the draw call, no.** The draw functions take only (device, primitive, start/base, count). State reaches them as raw Xenos register shadows in the D3D device. The two callers behind 82–86% of the scene draws walk a mesh's sub-mesh records and call a material object's virtual "apply" when the material changes (§3). |
| Distinct EDRAM aliasing / resolve patterns | **64–86 distinct ownership-transfer pairs and 17–21 resolve shapes in one frame**, in about **6 families** (§4). This is far above the "≤ 5 patterns" bar for a full HLE renderer. |
| Main pass per EDRAM tile | **The scene colour pass is drawn twice per frame**: XDK predicated tiling, 2 tiles of 576 + 144 rows. **42–51% of issued draws re-execute a packet already executed that frame** (§2.3). This was not on the options list. |
| M3 Pro 2× (27.0 ms measured) by bucket, **estimated** (§6) | Shader/raster work in the main scene passes **~14.5 ms (53%)**; EDRAM emulation **~9.9 ms (36%)**; post chain + UI **~2.8 ms (10%)**; other fixed overhead **~0.3 ms (1%)**. Cross-cut: render passes with 1–2 draws ≈ **9 ms**. |
| **Gate G3** | **Not met as a whole.** The draw boundary is clean for counting and interception, but not semantically (§3). The post chain measures **1.46 ms at 3× in Bower Lake (< 2 ms)** and **2.19 ms in Old Tomb**; on an M3 Pro at 2× it is an estimated 2.8–4.4 ms, of which only part is removable. |
| Ranking for the M3 Pro at 2× (§8) | (1) EDRAM-side reductions (alias-clear passes, the last resolve-into-texture case, pass merging; #11 in spirit, done piecewise): **~4–5.5 ms est.** (§11 corrects the resolve part) (2) Hybrid post-chain HLE (#5): **~1–2 ms est.** (3) Native shader translator (#4): **~1–2 ms est.**, XL effort. (4) **New:** collapse the XDK's predicated tiling: **upper bound measured at −0.47 / −0.63 ms on the M5 Max at 1× / 2×, so ~0.7 ms est. on the M3 Pro** (§10). Fixed-overhead items (idle, uploads): < 0.5 ms. |

**Recommended next step:** prototype **alias-clear recognition**, behind a switch.
- What it does: a full-surface constant clear quad drawn on a 4×MSAA alias (and the 25-draw 4×MSAA alias depth
  passes that only carry it plus no-op points) becomes a load-action clear of the real target. No alias texture, no
  ownership transfer.
- Exact by construction: the same bytes land in the same target.
- Size: about 20 encoders and a third of the transfers per frame; 1.45 ms at 2× on the M5 Max, an estimated 3–4 ms on
  the M3 Pro.
- Then **resolve straight into the sampled texture** (rank 3 of the options doc). **Tiling collapse** was measured
  afterwards (§10): its upper bound is small (−0.63 ms at 2× on the M5 Max).

## 1. Method and tooling

- **Builder wrappers** (`src/core/hotfunc/gpu/HleCensus.cpp`, `FABLE2_HLE_CENSUS=1`):
  - Strong-symbol wrappers on every guest function that writes a PM4 `DRAW_INDX` (type-3 0x22) or `DRAW_INDX_2`
    (0x36) header. These were found by grepping the generated code for the header constants.
  - The outermost call scans the packets it appended at the device's write pointer `[device+48]`. The "begin UP"
    builders leave the pointer to their "end" call, so for those the pending end at `[device+13444]` is scanned.
  - Each draw header's guest physical address is recorded with a site id = (builder, caller `lr`).
  - The segment-kick helper `821E8D20` is wrapped too: a builder that fills a segment is scanned before the kick.
- **Command-processor hook** (`backends/metal/sdk-metal-hle-census.patch`, trace only):
  - The SDK command processor calls `OnDrawPacketForCensus(header, skipped)` for every draw packet, and
    `OnIndirectBufferForCensus` for every indirect buffer. Both run only when `hle_census_hooks_` is set.
  - `metal_hle_census.h` looks each header up (through `dlsym`) and attributes each issued draw to its site. Once per
    frame it logs `[hle-census] f<N> pk= skip= col= dep= copy= unm= uniq= maxx= rep= ib= ibu= sites id:issued/distinct`.
  - With `FABLE2_SHADOW_TRACE=<frame>`, the per-draw trace line gains `site= prim= n=`.
- **Readers** (Python 3.9, in `tools/f2diag/census.py`):
  - `f2diag census <session|log>`: draws per frame, per-site table, the traced frame's pass map (repeated blocks
    collapsed) and its aliasing patterns.
  - `f2diag census-cost <session|passes.csv>`: marginal GPU busy time per pass role and bucket, from a
    `FABLE2_GPU_TIME_TRACE=1` pass trace (`keytail.py` method).
  - Roles are keyed on the EDRAM pitch encoded in the host key: 80 px per tile, 40 at 4×MSAA.
  - The mapping was checked with `census.align` on frames traced with both switches. Host encoder draws were walked
    against the guest draws in shadow-trace order: Bower Lake 1× 2136 = 2136 draws, Bower Lake 2× 3355 = 3355, Old
    Tomb 2× 1350 = 1350, with **0 encoders spanning two guest target sets**.
- **Run script**: `out/validation/hle-census-20261013/run.sh <label> <save> <scale> <60|uncapped> census|timing|align`.

Time and cost figures are encoder busy time, which is not span: each encoder is charged for how far it pushes the end of
the serial GPU timeline. All of them come from `headings8`.

## 2. Draw boundary (Q1)

### 2.1 The builders

| Builder | Packet | Role (inferred from arguments and packets; no symbols) | Bower Lake 2× | Old Tomb 2× | Town 2× |
|---|---|---|---|---|---|
| `8221E0F0` | DRAW_INDX (4 dw) | indexed draw (prim, base vertex, start index, count) | 47.0% | 68.0% | 74.5% |
| `8221C518` | DRAW_INDX (2 dw) | non-indexed draw (prim, start, count) | 12.5% | 4.8% | 4.2% |
| `8221C9C8` | DRAW_INDX (2 dw) | non-indexed draw, auto-index variant | 11.3% | 0.3% | 8.0% |
| `82207C30` | DRAW_INDX (4 dw) | indexed draw from an index-buffer object, 65534-index chunks | 5.5% | 6.3% | 2.2% |
| `82217EE8` | DRAW_INDX (4 dw) | "begin" indexed UP draw (caller fills indices and vertices) | 3.3% | 2.5% | 1.6% |
| `822069C0` | DRAW_INDX_2 | resolve (copy) rectangles; issued count = copies per frame | 2.5% | 1.6% | 0.9% |
| `821EF988` | DRAW_INDX_2 | clear quads (one site at 1×, one at the 4×MSAA alias) | 1.8% | 1.5% | 0.8% |
| `822060A0` | DRAW_INDX (2 dw) | "begin" UP draw | 1.6% | 0.7% | 0.5% |
| `82B98770`, `82B988C8` | DRAW_INDX_2 | D3D-internal | 0.8% | 0.4% | 0.2% |
| `82BA5448`, `82BA5CE8`, `82BA29B8` | DRAW_INDX_2 | D3D-internal (not seen in gameplay frames) | 0 | 0 | 0 |
| `82B9C068`, `82B928A8` | DRAW_INDX / _2 | engine init path (`VdInitializeEngines` caller), once | 0 | 0 | 0 |
| **unattributed** | — | **24 static point-list draws** (below) | 13.8% | 13.9% | 7.2% |

Percentages are of issued draws (draws that reach the backend, resolves included).

| Per frame (mean over gameplay frames) | Bower Lake 1× | Bower Lake 2× | Old Tomb 2× | Town 2× |
|---|---|---|---|---|
| Draw packets parsed | 3358 | 3411 | 2074 | 5332 |
| Dropped by the bin predicate | 297 | 302 | 34 | 272 |
| Issued (colour+depth / depth only / resolve) | 3061 (2211 / 774 / 76) | 3109 (2247 / 786 / 77) | 2040 (1691 / 316 / 33) | 5060 |
| Distinct draw headers | 1760 | 1790 | 1141 | 2486 |
| Unattributed (distinct) | 429 (24) | 430 (24) | 283 (24) | 364 (24) |
| Call sites seen | 38 | 38 | 34 | 39 |
| Indirect buffers executed / distinct | 84 / 27 | 84 / 27 | 58 / 24 | 80 / 29 |
| **Through the doc's three functions** (`82207C30`, `82217EE8`, `8221E0F0`) | 55.9% | 55.8% | 76.8% | 78.4% |
| **Through all 13 builders** | 86.0% | 86.2% | 86.1% | 92.8% |

- **The unattributed draws do nothing visible.** There are 24 distinct headers per frame, re-executed 12–18 times
  each, all point lists with one vertex shader (`B6C9863F710683EC`), colour mask 0, depth control 0 and a 16×16
  scissor. Checked on every unattributed draw in all four traced frames: 0 exceptions.
  - They sit at the start of each 4×MSAA alias depth pass and each occlusion pass (24 per pass).
  - No builder writes them, so they come from a static, prebuilt command buffer (D3D-internal; most likely
    query/fence plumbing).
- **"~1,600 draws per frame" undercounts.** Bower Lake issues ~3,100 draws per frame, Old Tomb ~2,000 and the town
  ~5,000, before the predicate drops ~300. About half are replays (§2.3).

### 2.2 Call sites (Bower Lake 2×, traced frame; Old Tomb and town similar)

| Builder / caller `lr` | Issued / distinct per frame | Feeds (traced frame) |
|---|---|---|
| `8221E0F0` / `8221BE20` | 1076 / 549 (town 3294 / 1503; Old Tomb 1270 / 774) | main colour pass, shadow cascades, depth prepass, the 256² 4×MSAA scene render |
| `8221C9C8` / `82220590` | 350 / 309 | main colour pass only (auto-index geometry) |
| `8221C518` / `82219928` | 287 / 287 | occlusion-query passes (256² and 280×180 depth), bracketed by query begin/end |
| `8221E0F0` / `82225F0C` | 171 / 64 | particle/sprite batcher: main pass, prepass, shadows |
| `82207C30` / `82A82FFC` | 156 / 70 | main pass + prepass (+ the 256² 4×MSAA render in Old Tomb and the town) |
| `8221E0F0` / `82B2153C` | 146 / 86 | second mesh walker: shadows, main pass, prepass |
| `82217EE8` / `8220A588` | 93 / 85 | full-screen and UI quads: texture-space mip chain, post chain, final composite + HUD |
| `8221C518` / `8222EDB8` | 68 / 68 | memexport passes (no render target) and the 256² render |
| `822069C0` / 10 sites | 77 / 77 | resolve rectangles |
| `821EF988` / `821EF938`, `821EF8CC` | 28 + 24 | clear quads at the 4×MSAA alias / at 1× |
| `822060A0` / `82242D70`, `821EBFD4` | 51 | UP quads: post chain (quarter-res ping-pong, luminance), shadow-mask quads |
| the rest (19 sites) | < 30 each | small effects, HUD, D3D internals |

The site ids printed by the tools are assigned in order of first use per run; the table uses (builder, `lr`), which is
stable.

### 2.3 Predicated tiling: the main pass is drawn twice

- The main colour pass (2×MSAA, 7e3 HDR + D24FS8, 1120×720 viewport) does not fit the 10 MB EDRAM: 1120 × 720 × 2
  samples × 8 bytes is about 13 MB, plus depth. So the XDK runs it as **two tiles**:
  - one indirect buffer replayed per tile, with draws predicated off in tiles they miss;
  - a resolve per tile: 1120×576 then 1120×144, `copy+clear`.
- The census sees it directly:
  - the main mesh site issues 1076 draws from 549 headers;
  - the busiest header runs 18 times (the static no-ops); ordinary scene draws run twice;
  - **1,319 / 899 / 2,574 issued draws per frame are re-executions** (Bower Lake / Old Tomb / town), 42–51% of the
    total; 913 / 640 / 2,234 after removing the 24 no-ops.
- On the host, both tiles render into the same full-size host texture. EDRAM has no such limit in our renderer, so
  the second tile's vertex and per-draw work and its separate resolve are pure emulation cost. The main pass is
  per-draw bound on the M5 Max: 2.38 ms at 1×, of which ~2.25 ms does not scale with pixels.

## 3. What the callers hand over (Q2)

- **At the draw function:** (device, primitive type, base vertex / start, count). Everything else is already in the
  D3D device structure as Xenos state:
  - context-register shadows from `device+10368`, with 64-bit dirty masks at `device+8…+40`;
  - shader-constant shadows (e.g. `device+2112…`, written inline by engine code, which then ORs the dirty bit);
  - fetch constants (vertex and texture descriptors), and the bound shader microcode.

  The draw function flushes the dirty ranges as PM4 (§2 of the options doc, confirmed here).
- **One level up there are engine objects:**
  - `8221BCC8` (site `8221BE20`) and `82B21448` (`82B2153C`) walk a mesh's 64-byte sub-mesh records. Each record
    holds a material index, a strip/list flag, an index count and a start index.
  - On a material change they call the material's virtual apply (`vtable+44`, 180-byte material records). They call
    `8221E5D0` (flushes an engine render-state cache: blend enables from D3D9-style enum values), then
    `8221E0F0(device, prim 4|6, 0, start, count)`.
  - Together these two walkers carry 82–86% of the scene draws in Old Tomb and the town, and ~54% in Bower Lake, where
    the auto-index and occlusion sites add more.
  - `82225CC8` (particles) batches quads and writes constants straight into the device shadow (`device+2112`) before
    each `8221E0F0`.
  - UP draws (`82217EE8`, `822060A0`) carry their vertices inline: post, HUD and text quads.
- **Could a native renderer consume them?**
  - Partly. Mesh, material and render-state objects exist above the draw functions, but:
    - materials are applied through engine code that writes Xenos state;
    - the shaders are Xenos microcode (the bank's names need an XMem/LZX decoder);
    - vertex formats are Xenos fetch constants.
  - A native renderer at the mesh level therefore still has to:
    - translate every guest shader;
    - decode fetch constants and render state, as the command processor does today;
    - reproduce memexport (the fur) and the occlusion queries.
  - So the boundary is clean for interception and counting, but it hands over emulation-shaped data, not D3D9-style
    objects. The one cheap thing it adds is **identity**: a site (builder, `lr`) tells the backend which engine path
    issued a draw (mesh walker, particle batcher, UP quad, clear, resolve), which the PM4 stream alone does not.

## 4. EDRAM aliasing and resolves (Q3)

One traced frame per area (a 2-frame window; counts per frame):

| | Bower Lake 1× | Bower Lake 2× | Old Tomb 2× | Town 2× |
|---|---|---|---|---|
| Ownership transfers | 175 | 178 | 86 | 127 |
| Distinct (source key → destination key) pairs | 71 | 73 | 64 | 86 |
| … by kind: MSAA change / colour↔depth / pitch change / format change / other | 26 / 22 / 14 / 7 / 2 | 26 / 22 / 15 / 7 / 3 | 21 / 23 / 4 / 15 / 1 | 34 / 26 / 14 / 9 / 3 |
| Resolves / distinct shapes (what, size, format, source MSAA) | 73 / 18 | 73 / 18 | 36 / 17 | 45 / 21 |
| Distinct guest target sets / passes | 32 / 130 | 33 / 130 | 27 / 65 | 35 / 85 |

The pairs fall into **six families** (passes covered, Bower Lake 2×):
1. **4×MSAA fast-clear alias** (1× ↔ 4× on the same tiles, colour and depth). The engine clears a target by drawing
   one quad on a 4×MSAA alias of it, then goes back to 1×. Every small target does this: texture-space targets,
   shadow cascades, half- and quarter-res post targets. **About 30 passes; about 50% of all transfers.**
2. **Mip-chain pitch changes** (256 → 128 → 64 → 32 render-to-texture at EDRAM base 0, pitch 4 → 2 → 1 tiles, then
   back). These are the texture-space chains: **12–14 blocks of 6 passes per frame in Bower Lake, 3 in the town, 2 in
   Old Tomb; 48–56 of the 73 Bower Lake resolves.**
3. **Colour ↔ depth reuse of EDRAM base 0**: the 1280-wide 10:10:10:2 final target ↔ shadow-map depth ↔ 256² depth.
   Plain memory sharing between passes that never overlap. **About 10 passes.**
4. **Main-depth MSAA alias** (2× ↔ 4× depth on the main depth tiles, D24FS8 ↔ D24S8). This covers the 4×MSAA alias
   depth passes and their clears/no-ops, the screen-space shadow pass's stencil, and the occlusion passes. **4–6 passes.**
5. **Post-chain format ping-pong** on the same quarter/half-res tiles: 8888 ↔ 10:10:10:2 ↔ 7e3 ↔ 32F (luminance).
   **About 15 passes.**
6. **Native ↔ scaled** (our own native-scale feature; 1–3 per frame).

- **Resolves per frame:**
  - texture-space chain 48–56 (Bower Lake);
  - shadow cascades 2 (depth, 1024²);
  - depth prepass 1;
  - shadow mask 1;
  - main colour 2 (per tile);
  - post 10–14;
  - final 1 (to the front buffer, `copy+clear`).
- Old Tomb has no cascades or shadow mask, but two 256² 4×MSAA scene renders (~420 draws each).

## 5. Pass map with costs (Q4)

### 5.1 One frame, Bower Lake 2× (structure is the same at 1×; Old Tomb and the town differ as noted)

| # | Pass (role) | Draws | Targets (guest) | Resolves / transfers |
|---|---|---|---|---|
| 1 | Texture-space chains, ×12–14 blocks of 6 passes | 90 | 256² 8888 + D24FS8 at 1×, cleared via a 4× alias; then 128/64/32 by UP quads | 48 resolves, 96 transfers |
| 2 | Memexport passes (fur), no target | 17–21 | — | — |
| 3 | Shadow cascades ×2 (clear 1×, clear 4× alias, depth draws) | ~450 | 1024² D24FS8 (native 1×) | 2 depth resolves, 13 transfers |
| 4 | Occlusion-query pass, 256² depth (24 no-op points + ~70 boxes) ×1, plus 64² tests ×4 | ~110 | 256² D24FS8; 64² 8888 | 4 resolves, 12 transfers |
| 5 | Quarter-res colour/depth clear (via 4× alias) | 2 | 280×180 8888 + D | 1 resolve |
| 6 | Main depth at 4×MSAA alias (1 clear + 24 no-ops) | 25 | 1120×720 4× D24FS8 | 6 transfers |
| 7 | **Depth prepass** | ~250–390 | 1120×720 2×MSAA D24FS8 | depth resolve 1120×720 |
| 8 | 4× alias depth/stencil pass + **screen-space shadow mask** (2 UP quads) | 25 + 1 + 2 | 1120×720 4× D24S8 → 1× 8888 | resolve 1120×720 |
| 9 | **Main colour pass, tile 1 / tile 2** | ~550 + ~390 (39 PS) | 1120×720 2×MSAA 7e3 + D24FS8 | resolves 1120×576 and 1120×144 (`copy+clear`) |
| 10 | Occlusion-query pass, 280×180 depth | ~90–115 | 280×180 D24FS8 | — |
| 11 | **Post chain**: half-res (560×360) and quarter-res (280×180) passes, each cleared through a 4× alias, UP quads; 32F luminance and 7e3 targets | ~30–45 in ~25 passes | 560×360 / 280×180, 8888, 10:10:10:2, 7e3, 32F | 10–14 resolves |
| 12 | **Final composite + UI** | 15–45 (5 PS) | 1280×720 10:10:10:2 | front-buffer resolve |

- **Old Tomb:**
  - no cascades and no shadow mask;
  - two 256² 4×MSAA scene renders (passes 24 and 28, ~420 draws each), resolved to 256² 8888, probably a reflection
    or environment target;
  - a 640×360 pass;
  - 129 memexport draws.
- **Town:** 1,100–1,330 draws per main tile, three texture-space blocks, ~295-draw cascades.

### 5.2 GPU busy per role, M5 Max (ms/frame, measured)

| Role | Bucket | Bower 1× | Bower 2× | Bower 3× | Tomb 1× | Tomb 2× | Tomb 3× | Town 1× | Town 2× |
|---|---|---|---|---|---|---|---|---|---|
| main colour pass (2×MSAA HDR, both tiles) | main | 2.38 | 2.78 | 3.36 | 1.12 | 2.05 | 3.93 | 2.78 | 2.90 |
| texture-space chains | main | 1.17 | 1.29 | 1.59 | 0.59 | 0.64 | 0.80 | 0.72 | 0.68 |
| resolves (EDRAM → memory) | EDRAM | 0.79 | 1.23 | 2.15 | 0.39 | 0.71 | 1.33 | 0.75 | 1.12 |
| clear quads at the 4×MSAA alias | EDRAM | 0.76 | 0.88 | 1.43 | 0.33 | 0.37 | 0.61 | 0.78 | 0.80 |
| main depth at the 4×MSAA alias (clear + no-ops) | EDRAM | 0.21 | 0.57 | 1.17 | 0.08 | 0.23 | 0.75 | 0.28 | 0.63 |
| 256² 4×MSAA scene render | main | 0.41 | 0.45 | 0.55 | 0.48 | 0.58 | 0.75 | 0.57 | 0.55 |
| depth prepass | main | 0.34 | 0.45 | 0.69 | 0.13 | 0.23 | 0.42 | 0.41 | 0.46 |
| quarter-res post (280×180) | post | 0.27 | 0.40 | 0.67 | 0.35 | 0.52 | 0.75 | 0.48 | 0.59 |
| screen-space shadow mask | main | 0.12 | 0.40 | 0.87 | — | — | — | 0.18 | 0.48 |
| memexport passes | main | 0.33 | 0.34 | 0.33 | 0.16 | 0.18 | 0.21 | 0.23 | 0.19 |
| resolved-texture reloads (untile/load) | EDRAM | 0.28 | 0.32 | 0.32 | 0.28 | 0.35 | 0.48 | 0.33 | 0.35 |
| shadow maps | main | 0.29 | 0.32 | 0.43 | — | — | — | 0.47 | 0.40 |
| half-res post (560×360) | post | 0.14 | 0.23 | 0.40 | 0.24 | 0.44 | 0.83 | 0.32 | 0.42 |
| final composite + UI | post | 0.07 | 0.19 | 0.40 | 0.11 | 0.30 | 0.62 | 0.12 | 0.26 |
| occlusion-query passes | main | 0.14 | 0.18 | 0.24 | 0.06 | 0.08 | 0.14 | 0.17 | 0.19 |
| ownership transfers / clears | EDRAM | 0.06 | 0.16 | 0.33 | 0.04 | 0.10 | 0.19 | 0.08 | 0.15 |
| EDRAM depth store | EDRAM | 0.06 | 0.08 | 0.16 | 0.06 | 0.10 | 0.17 | 0.08 | 0.09 |
| shared-memory uploads | fixed | 0.06 | 0.07 | 0.07 | 0.03 | 0.04 | 0.04 | 0.07 | 0.07 |
| **encoder busy (sum)** | | **7.89** | **10.34** | **15.15** | **4.47** | **6.92** | **12.02** | **8.82** | **10.33** |
| GPU ms/frame (command buffers, mean) | | 8.13 | 10.46 | 15.41 | 5.02 | 7.56 | 13.06 | 10.15 | 11.15 |
| FPS uncapped | | 120 | 95 | 64 | 191 | 131 | 76 | 82 | 73 |
| encoders per frame R / C / B | | 132/94/37 | 132/98/46 | 131/99/47 | 67/55/26 | 66/53/29 | 68/55/30 | 94/65/28 | 94/70/34 |

| Bucket (M5 Max, measured) | Bower 1× | Bower 2× | Bower 3× | Tomb 1× | Tomb 2× | Tomb 3× | Town 1× | Town 2× |
|---|---|---|---|---|---|---|---|---|
| main scene passes | 5.19 | 6.21 | 8.06 | 2.55 | 3.77 | 6.25 | 5.52 | 5.85 |
| EDRAM emulation | 2.16 | 3.24 | 5.56 | 1.18 | 1.87 | 3.53 | 2.31 | 3.14 |
| **post chain + UI** | **0.48** | **0.82** | **1.46** | **0.71** | **1.26** | **2.19** | **0.92** | **1.28** |
| fixed: uploads | 0.06 | 0.07 | 0.07 | 0.03 | 0.04 | 0.04 | 0.07 | 0.07 |
| fixed: GPU idle / unattributed (GPU mean − busy) | 0.24 | 0.12 | 0.26 | 0.56 | 0.64 | 1.04 | 1.34 | 0.82 |

The town at 1× is partly CPU-bound (GPU Commands), hence its idle.

### 5.3 The post-processing chain

- **What it contains:** passes 11–12 above.
  - half-res (560×360) downsample/blur passes, some depth-tested;
  - quarter-res (280×180) ping-pong over 8888, 10:10:10:2, 7e3 and a 32F target (luminance for tone mapping);
  - 10–14 resolves;
  - the final 1280×720 composite, which also draws the HUD and text.
- Every step is an UP full-screen quad (`82217EE8`/`8220A588`, `822060A0`) with a clear through a 4×MSAA alias in
  front.
- Without bank names, bloom, depth of field, radial blur and tone map cannot be told apart, so these are identified
  structurally.
- **Share at 2×:**

  | Scene | Post chain + UI | Share of encoder busy |
  |---|---|---|
  | Bower Lake | 0.82 ms | 8% |
  | Old Tomb | 1.26 ms | 18% |
  | Town | 1.28 ms | 12% |

  - Its share of the clear-quad, transfer and resolve rows adds roughly 0.3–0.5 ms at 2× (estimate: about 25 of the
    ~100 small encoders and 10–14 of the resolves).
  - At 3×: 1.46 ms in Bower Lake and 2.19 ms in Old Tomb.

### 5.4 Fixed per-frame overhead

**Encoders by size** (busy ms/frame, M5 Max; `smallenc.py`):

| | R ≤ 2 draws | R 3–50 | R > 50 | C (resolves, loads, depth store) | B (uploads) |
|---|---|---|---|---|---|
| Bower Lake 1× | 113 enc, 2.52 ms (22 µs each) | 13, 1.04 | 10, 3.14 | 95, 1.08 | 35, 0.11 |
| Bower Lake 2× | 114, 3.35 (29 µs) | 14, 1.60 | 9, 3.68 | 102, 1.56 | 48, 0.14 |
| Old Tomb 2× | 54, 2.10 (39 µs) | 7, 0.83 | 6, 2.79 | 54, 1.12 | 29, 0.08 |
| Town 2× | 71, 3.22 (45 µs) | 15, 1.75 | 10, 3.74 | 71, 1.49 | 34, 0.14 |

- Render passes with one or two draws, mostly clears, mip steps and post steps, take **a third of the M5 Max frame**.
- Each one loads and stores whole attachments, often MSAA. On Xenos the same work was free inside EDRAM.
- **Command buffers:** about 8 per frame (3 primary-buffer ends + 4 upload pre-commits + the presenter).
- **Main-pass breaks:** 2.8–3.0 per frame in Bower Lake (1.7–2.0 texture uploads, 1.0 per-tile resolve); 1.3 in Old
  Tomb.
- **GPU idle:** 0.1–0.2 ms/frame in Bower Lake (5.6 gaps per frame, p95 150–190 µs).
- **Presenter:** 0.13–0.19 ms.
- **Sync points are not the problem:** busy is 98–99% in every GPU-bound run.
- **What does not scale with resolution:** fitting `GPU = F + P × scale²` over 1×/2×/3× gives F = 7.36 ms (M5 Max,
  Bower Lake). That is the "fixed per-frame cost":
  - mostly the main pass's per-draw work (both tiles), about 2.2 ms;
  - texture-space chains, 1.1 ms;
  - clear quads, 0.7 ms;
  - resolve dispatch, 0.6 ms.

## 6. Where 27 ms at 2× goes on an M3 Pro (Q5)

**Measured** (benchmark-results, headings8, Bower Lake):

| | 1× | 2× | 3× |
|---|---|---|---|
| M3 Pro GPU ms/frame | 15.7 | 27.0 | 47.2 |
| M5 Max (this branch) | 8.13 | 10.46 | 15.41 |

- Fitting `F + P × scale²`:
  - M5 Max: F 7.36, P 0.78;
  - M3 Pro: F 11.9, P 3.77.
- So the M3 Pro is **1.62× slower on work that does not scale with pixels** and **4.86× slower per pixel**. That is
  close to the bandwidth ratio (about 4×, 150 vs ~600 GB/s), not the core ratio (2.9×).
- **Estimated** per role: M3(2×) ≈ 1.62 × F_role + 4.86 × 4 × P_role. F_role and P_role come from the measured M5
  Max 1×/2× pair; the result is normalised to the measured 27.0 (the raw sum is 27.5).

| Role (Bower Lake) | M5 2× (measured) | M3 Pro 2× (estimated) |
|---|---|---|
| main colour pass (both tiles) | 2.78 | 6.1 |
| resolves | 1.23 | 3.8 |
| texture-space chains | 1.29 | 2.6 |
| main depth at the 4×MSAA alias (clear + no-ops) | 0.57 | 2.5 |
| clear quads at the 4×MSAA alias | 0.88 | 1.9 |
| screen-space shadow mask | 0.40 | 1.8 |
| depth prepass | 0.45 | 1.2 |
| quarter-res post | 0.40 | 1.2 |
| 256² 4×MSAA scene render | 0.45 | 0.9 |
| final composite + UI | 0.19 | 0.8 |
| half-res post | 0.23 | 0.8 |
| transfers + depth store | 0.24 | 0.9 |
| resolved-texture reloads | 0.32 | 0.7 |
| shadow maps | 0.32 | 0.6 |
| memexport | 0.34 | 0.6 |
| occlusion passes | 0.18 | 0.4 |
| idle, uploads | 0.19 | 0.3 |

| Bucket (M3 Pro 2×, **estimated**) | Bower Lake | Old Tomb* | Town* |
|---|---|---|---|
| Shader/raster work in the main scene passes | **14.5 ms (53%)** | 11.4 (50%) | 11.9 (46%) |
| EDRAM emulation: resolves, reloads/untile, transfers, depth store, 4×MSAA alias clear/no-op passes | **9.9 ms (36%)** | 6.0 (26%) | 8.7 (34%) |
| Post chain + UI | **2.8 ms (10%)** | 4.4 (19%) | 3.6 (14%) |
| Fixed overhead: idle, uploads, presenter gap | **0.3 ms (1%)** | 1.1 (5%) | 1.4 (6%) |

\* Old Tomb and the town were never run on the M3 Pro. Their columns apply Bower Lake's factors and are not normalised
(totals 22.9 and 25.6 ms).

- **Cross-cut by encoder size (Bower Lake, estimated):**
  - passes with ≤ 2 draws ≈ **9.0 ms**;
  - compute (resolves, loads) ≈ 4.6;
  - 3–50 draws ≈ 5.0;
  - more than 50 draws (the real scene content) ≈ **8.3 ms**.
- **About half of the M3 Pro's 2× frame is small passes and resolves.** These are EDRAM-shaped work that the
  original hardware did nearly for free.
- **Caveat:** a single pair of factors is crude. Bandwidth-heavy roles (resolves, MSAA load/store) probably scale
  worse than ALU-heavy ones, so the EDRAM share is more likely under-estimated than over-estimated. One M3 Pro run
  with `FABLE2_GPU_TIME_TRACE=1` and `f2diag census-cost` would replace this table with measurements.

## 7. "Emulate as little as possible": part by part

How much of each part could become native Metal that consumes the game's own draw data, what blocks it, and the
estimated M3 Pro 2× time at stake (§6).

| Part | M3 2× (est.) | Could become native | Blocked by |
|---|---|---|---|
| **Resolves and EDRAM aliasing** (copies, untile, transfers, depth store, 4× alias clears) | 9.9 ms | **Most of it.** Alias clears → load-action clears of the real target. GPU-only resolves → render straight into the sampled texture. Same-pitch ping-pong and mip chains → plain textures. Per-tile resolves → one. | CPU-visible resolves must still land in guest memory (the texture-space morph levels are read back: bug-patterns row 72). D24FS8 has no Metal format (the resolve converts). Guest-visible ordering (CR-10 class). It needs per-family recognition, not a general EDRAM model. |
| **Main scene passes** (prepass, main colour, 256² renders, occlusion, memexport) | 14.5 ms | **Draw issue: yes.** Collapse the two tiles into one full-height pass (no replay, one resolve). Skip the 24 no-op points per pass. **Shading: no.** It stays translated Xenos microcode (the specialization already shipped is the faithful form). | Xenos register and fetch-constant state at the draw boundary (§3). Shaders are microcode with no names. Memexport (fur). Occlusion-query results. Tiling collapse needs the tiled sequence recognised (predicate, window offset, per-tile resolve). |
| **Shadows** | 0.6 ms + share of clears/resolves | Native 1× already (`FABLE2_SHADOWS_NATIVE`). The remaining emulation is the 4× alias clear and the depth resolve. Render into a sampled depth texture. | D24FS8 → D32F exactness (the resolve's conversion must be kept or proven equal). |
| **Post chain** | 2.8 ms (+ its clears/resolves) | **Whole chain, in principle** (hybrid HLE #5). Keep intermediates in host textures and fuse passes. | No bank names (LZX). Exact ports of ~25 small shaders. 7e3 / 10:10:10:2 float formats. The luminance target is reused across frames. Small payoff (< 2 ms at 3× in Bower Lake). |
| **UI** | ≈ 0.3–0.5 ms (part of the final pass) | Could be drawn natively from the UP quads. | Shares the final composite pass and the UP builders with post; ~15–25 draws. **Not worth doing for speed.** |

## 8. Verdicts (Q6)

### Gate G0

- **Clean draw boundary?**
  - **For counting and interception: yes.** 13 builders and about 40 (builder, `lr`) sites carry 100% of the draws
    that write anything. The wrappers cost one bool test when off.
  - **For a native renderer: no.** The draw boundary hands over raw Xenos state; the engine-level objects (mesh,
    material, render-state cache) sit one level up and are applied through code that writes that state.
- **Distinct aliasing patterns:** 64–86 key pairs per frame in 6 families, 17–21 resolve shapes. **A full HLE
  renderer stays "not recommended"** (options doc rank 13).

### Gate G3: hybrid post-chain HLE

| Condition | Result |
|---|---|
| Clean boundary | Partial (above). The post chain can be recognised at the command-processor level by (PS hash, targets) without the draw boundary, so this condition matters less than the options doc assumed. |
| Post chain ≥ 2 ms at 3× (M5 Max, measured) | **Bower Lake 1.46 ms: no. Old Tomb 2.19 ms: yes.** |
| Equivalent on the M3 Pro at 2× (estimated) | 2.8 ms (Bower Lake), 3.6–4.4 ms (town, Old Tomb). The removable part is less, because the chain's own shading stays. Estimate 1–2 ms. |
| Replay-compare harness | Exists (Bet A shaderlab). |

**Verdict: not met as a whole. Do not start #5 now.** It is XL effort for an estimated 1–2 ms on the M3 Pro. Three
cheaper exact levers below touch the same passes first: alias clears, resolve-into-texture and pass merging. Revisit
after them.

### Ranking of native-ward options, by expected saving on the M3 Pro at 2× (all estimates)

| Rank | Option | Saving, M3 Pro 2× | Effort | Faithfulness |
|---|---|---|---|---|
| 1 | **EDRAM-side reductions, piecewise (#11 in spirit):** (a) alias-clear recognition, (b) resolve straight into the sampled texture (rank 3 of the options doc), (c) merge consecutive small passes on the same tiles | (a) ~3–4 ms, (b) mostly done before this census, the remaining case ~0.3–0.45 ms in Old Tomb only (§11), (c) ~1 ms; together **~4–5.5 ms** of the 9.9 | M (a), L (b), L (c) | Exact by construction for (a); (b) needs ordering care |
| 4 | **Predicated-tiling collapse** (new): run the tiled main pass once at full height, one resolve, no replays | **~0.7 ms** (measured upper bound on the M5 Max: −0.47 ms at 1×, −0.63 ms at 2× with the second tile skipped outright; the real collapse must still draw those 144 rows; §10) | XL (EDRAM address-space conflict, §10) | Exact only with per-bracket recognition and resolve remapping |
| 2 | **Hybrid post-chain HLE (#5)** | ~1–2 ms | XL | Exact only with faithful shader ports |
| 3 | **Native Xenos→MSL translator (#4)** | ~1–2 ms (main-pass shading on the M3 Pro is ~6 ms est.; G2 measured −17% on the top shader; specialization already took the easy part) | XL | Exact if semantics are kept |
| 5 | Fixed-overhead items (idle gaps, upload pre-commit buffers, presenter) | < 0.5 ms | S–M | Exact |
| — | Whole EDRAM in tile memory as one redesign; full HLE renderer | Covered by 1–2 at much lower risk / not recommended | XXL | — |

- **Gap check:** 27.0 ms − 16.7 ms (60 FPS) = 10.3 ms needed. Rank 1 gives an estimated 4–5.5 ms; with 2–4 (≈3–5 ms more) it could close, and MetalFX spatial stays the labelled fallback.
- **These are estimates on a crude two-factor model.** The first prototype (alias clears) should be measured on the
  M3 Pro itself, with the pass trace on.

## 9. Switches and files

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_HLE_CENSUS=1` | off | Builder wrappers + command-processor hooks + `[hle-census]` per-frame line; site ids in `[shadow-trace]` draw lines |
| `FABLE2_SHADOW_TRACE=<frame>` (existing) | off | Per-draw / resolve / transfer trace for 2 frames; pass map input |
| `FABLE2_GPU_TIME_TRACE=1` (existing) | off | Pass trace; `census-cost` input |

- **Log volume:** with `FABLE2_HLE_CENSUS=1` at uncapped frame rates, the 5 MB log rotation drops early frames. Put
  `FABLE2_SHADOW_TRACE` late in the run (frame ~1900 at 60 FPS), or use 60 FPS for traced frames.
- **Files:**
  - `src/core/hotfunc/gpu/HleCensus.cpp`;
  - `backends/metal/port/rex/graphics/metal/metal_hle_census.h`;
  - `metal_command_processor.cc/.h` (hooks, site in the shadow-trace line);
  - `backends/metal/sdk-metal-hle-census.patch` (two virtual hooks + a flag in the SDK command processor), applied by
    `tools/build_macos_metal.py`;
  - `tools/f2diag/census.py`, `tools/f2diag/__main__.py` (`census`, `census-cost`).
- No behaviour change when off: the same packets and draws; the wrappers forward to `__imp__` unchanged.

## 10. Follow-up: predicated tiling, structure and upper bound (2026-10-13)

Branch `agent-2/20261013-tiling-collapse`. Diagnostic only; no faithful collapse was built.

### Structure (`FABLE2_TILING_TRACE=<frame>` with `FABLE2_HLE_CENSUS=1`; Bower Lake 1×)

The trace lists every indirect buffer, type-3 packet, non-constant register write and draw, with the tiling registers.

- **The main pass is one recorded command list, replayed per tile** through `INDIRECT_BUFFER`:
  - **Tile 1:** bin select `80000003` → a 20-dword setup buffer (window offset 0, window scissor rows 0–576) → select
    `2` → the recorded list (3.9k dwords, ~260 draws) → select `80000003` → the tail (~240 draws, translucents) and
    the resolve (`copy+clear`, 1120×576).
  - **Tile 2:** select `C` → setup (window offset y = −576, window scissor rows 576–720) → select `8` → **the same
    recorded list** → select `C` → the tail (152 of ~240 draws dropped by the predicate) → resolve (1120×144).
- **Bin masks are written before each draw:** `3` / `C` for single-tile draws, `15555555` / `2AAAAAAA` /
  `FFFFFFFF` for draws in every tile. The recorded list's ~260 draws carry every-tile masks, so the second tile
  re-runs them all.
- **The depth prepass is not tiled:** depth only, rows 0–720 in one pass.
- **The 24 no-op point draws** come from one static 48-dword indirect buffer, executed once per depth/occlusion pass.

### Upper bound (`FABLE2_TILING_COLLAPSE=skip2`, UNSAFE diagnostic: tile 2 skipped, bottom 144 rows stale)

Bower Lake, headings8 uncapped, one run per arm. All `ok`, 0 AV, no contention.

| | 1× off | 1× skip2 | 2× off | 2× skip2 |
|---|---|---|---|---|
| GPU ms/frame | 8.07 | 7.60 (−0.47) | 10.50 | 9.87 (−0.63) |
| Main colour pass busy ms | 2.43 | 1.92 | 2.80 | 2.20 |
| Main colour draws / encoders per frame | 1318 / 4.2 | 782 / 3.0 | 1333 / 4.2 | 794 / 2.9 |
| FPS | 117 | 128 | 93 | 98 |

- **A faithful collapse still has to draw the second tile's 144 rows,** just in the same pass. So its saving is about
  the scale-independent part of the bound, ~0.42 ms on the M5 Max (fit over 1×/2×), plus one fewer pass load/store.
- **On the M3 Pro at 2× that is ~0.7 ms (estimate, ×1.62),** about 2.5% of 27 ms. The §8 estimate of 1.5–2.5 ms
  was too high: the replayed draws are cheap per draw once the shaders are specialized.
- **What a faithful collapse would need**, beyond the trace:
  - **Look-ahead recognition of the bracket**: the select / setup / list / tail / resolve sequence, the same list
    pointer in both tiles, and a scissor/offset-only difference.
  - **Union bin select and scissor, offset 0**, then the tile-2 indirect buffers skipped.
  - **An EDRAM address-space workaround.** A 720-row 2×MSAA 64bpp colour target needs 1260 tiles from base 0, which
    overlaps the depth target at base 1008. The render-target cache's ownership tracking would transfer depth into
    the colour target unless ownership is clamped to the tile-1 rows while the host renders all 720.
  - **A remapped second resolve**: it must read host rows 576–720 instead of EDRAM rows 0–144.
  - **The final EDRAM depth contents rebuilt**: tile 2 leaves rows 576–720 of the depth at rows 0–144, which later
    passes could observe.
  - **Effort and risk:** XL, high (shared SDK render-target cache and resolve code).
- **Recommendation: do not build it now.** Spend the effort on alias-clear recognition and resolve-into-texture
  first; re-measure this bound after them, since a cheaper main pass makes the replay share larger.

| Switch | Default | Meaning |
|---|---|---|
| `FABLE2_TILING_TRACE=<frame>` (+ `_FRAMES`, needs `FABLE2_HLE_CENSUS=1`) | off | Packet / register / draw trace of the tiling bracket |
| `FABLE2_TILING_COLLAPSE=skip2` | off | **Unsafe diagnostic**: skip indirect buffers while the bin select names only the second tile's bins (stale bottom rows). Bounds the saving; never a player setting, no launcher toggle |

## 11. Follow-up: extending resolve-into-texture (2026-10-13)

Branch `agent-2/20261013-rtt-extend`. M5 Max. Every run `ok`, 0 AV. Perf runs had no contention; the 2× parity
pairs ran two at a time beside other agents' test games (contention 1–2, which only affects perf). Evidence:
`out/validation/rtt-cover-20261013/` and `out/validation/hle-census-20261013/rtt-census*`, `dsfp-*` (KEEP).

### What was left (RTT census at 2×, `FABLE2_RTT_DIAG=census`)

- **Resolve-to-texture (§17/§18/§20 of `resolution-scale.md`) already covers almost every resolve.** Resolved data
  still reloaded through the scaled buffer was about 1 load per frame:

  | Scene | Data reloaded per frame |
  |---|---|
  | Bower Lake | 2.6 MB |
  | Town | 3.2 MB |
  | Old Tomb | **22.6 MB** |

- **The Old Tomb load** is the 1120×720 `16_16_16_16_FLOAT` texture of the main HDR colour pass. It is resolved in
  the two EDRAM-tile halves (§10).
- **The §5.2 "reloads" role** (~0.3 ms at 2×) is mostly CPU-written or streamed textures (~20 of ~45 loads per
  frame), not resolve outputs.
- **Corrections to §8:**
  - "resolve straight into the sampled texture" was mostly done before this census;
  - the 1.5–2.5 ms M3 Pro estimate was wrong;
  - the resolve dispatches themselves (~1.2 ms at 2×) are the copy and stay.
- **Why that texture still reloaded:**
  - Both halves (`k_2_10_10_10_FLOAT_AS_16_16_16_16`, the 7e3 format with 16-bit blending precision) were rejected
    by `TryDirectHostResolveCopy`'s format check. The render-target key holds the *storage* format
    (`k_2_10_10_10_FLOAT`), and the check compared it with the resolve's precision variant.
  - So they took the EDRAM-dump path (`rt_resolve`). That path is never fused into textures, so the texture reloaded
    every frame.
  - Bower Lake's main pass uses the plain `k_2_10_10_10_FLOAT` and was already direct and fused.
  - The new reject-reason trace (`FABLE2_DHR_REJECT_TRACE=1`) shows the other dump-path fallbacks are aliasing:
    colour resolves of tiles a depth target owns, or another format owns. Those are EDRAM-alias families
    (alias-clear's area) and cost little (`rt_resolve` ≈ 0.01 ms).
- **Multi-resolve coverage was built and dropped.** It let a texture become valid from two partial fused writes.
  Once the format check was fixed, the texture is valid before the first half, so both halves fuse as ordinary class
  1 and the coverage path never fired (`tr_n_rtt_cover` 0 in all three scenes). It was removed to keep the code
  simple.

### Change (`FABLE2_DHR_STORAGE_FORMAT`, default on; Dev Launcher toggle "Direct resolves for precision-variant formats")

- `TryDirectHostResolveCopy` compares `GetStorageColorFormat(resolve format)` with the render-target key's format.
  This is what `GetColorResourceFormat` stores for non-gamma formats; gamma is rejected earlier. The packability check
  uses the storage format too.
- **Exact by construction.** The direct shaders pack host texels with `XeResolveHostColorPack32Float`, which handles
  `_FLOAT` and `_FLOAT_AS_16_16_16_16` in the same case (7e3). The copy constants are the same on both paths. The
  EDRAM-dump path packs the same host texels the same way, then reads them back.
- **Verify** (`FABLE2_RTT_VERIFY=1`, Old Tomb 2× spin5): 7,424 kept/fused texture checks, 0 mismatched.
- **Parity** (spin5, idle, linear light):

  | Comparison | Median | Max |
  |---|---|---|
  | Old Tomb 2×, on vs off | 0.08% | 0.38% |
  | Old Tomb 2×, on vs on (noise) | 0.04% | 2.56% |
  | Town 2×, on vs off | 0.41% | 7.09% |
  | Bower Lake 2×, on vs off | 0.19% | 2.82% |

  - At 1×, 13 of 13 idle pairs were byte-identical in Old Tomb, and 13 of 14 in the town and Bower Lake.
  - A first 2× attempt with three games running at once captured the loading screen in one arm. It was discarded
    and re-run.

### Results (Old Tomb, headings8 uncapped)

| | off (`=0`, same build) | on |
|---|---|---|
| 2× GPU ms/frame (2 runs) | 7.77 / 7.66 | 7.62 / 7.66 |
| 2× FPS | 127.3 / 129.3 | 129.8 / 129.4 |
| 2× scaled reload MB/frame (`tr_n_tex_dl_kb`) | 22.3 / 22.6 | 0.23 |
| 2× fused resolves per frame (`tr_n_rtt_direct`) | 21.6 / 21.9 | 23.6 |
| 1× GPU ms/frame (2nd pair) | 4.91 | 4.98 |

- **M5 Max:** neutral, as every bandwidth-only lever on this GPU (−0.06 ms mean at 2×; 1× unchanged).
- **Old Tomb at 2×, per frame:**
  - removed: the 22 MB reload (read + write) and the 1120×720 EDRAM dump and readback;
  - added: one ~26 MB fused texture write;
  - net: about −70 MB of memory traffic.
- **M3 Pro 2× estimate:** about −0.3 to −0.45 ms in Old Tomb (pixel-proportional work ×4.86, or 70 MB at
  ~150 GB/s). Zero in Bower Lake and the town, whose main pass already took this path.
- Bug-patterns row 76.
