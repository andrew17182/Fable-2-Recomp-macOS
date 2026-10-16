# Adaptive terrain white flashes (2026-10-14/15)

With `FABLE2_TERRAIN_ADAPTIVE=1` the M3 Pro showed white flashes on the terrain while the camera moved, mostly by
the Bower Lake shore. With the switch off there were none. The M5 Max did not show them. The maintainer's runs on
the M3 Pro (converter-free portable 9cc020c, by the lake):

| Run | Settings | Result |
|---|---|---|
| 1 | `FABLE2_TERRAIN_ADAPTIVE=1` | white flashes |
| 2 | adaptive off | none |
| 3 | adaptive + `FABLE2_NATIVE_TESS_SNAP_UV=1` | flashes, as in run 1 |
| 4 | adaptive + `FABLE2_NATIVE_TESS=3` | flashes, plus blue flashes |

**Fix:** `FABLE2_TERRAIN_LAYER_MATCH` (default on; it only acts when adaptive terrain is on). Every pass over a
terrain patch now tessellates with the same factors on the same path. Adaptive terrain stays default off.

## How the game draws terrain

`FABLE2_TESS_FRAME_DUMP` (below) gives the draw order of one frame at the lake overlook (save
`hero1-dog-live_2026-10-07_14-10-08`). Into the main depth target (`RB_DEPTH_INFO` 000103F0) go:

1. **Depth prepass**: VS `5003700B7C9B1C16`, no PS, adaptive quad patches, z write, zfunc GreaterEqual. Depth is
   reversed: viewport z scale −1, offset 1.
2. **Opaque terrain colour**: the same VS with PS `7DDAD5C2` or `2F8A0341`, the same patches and factors (always
   identical to the prepass's). Render target format 3, `2_10_10_10_FLOAT`.
3. **Material layers**: VS `C30A97D946FA2BE4` / `FB68A7F2301210E1`, continuous mode, one auto-indexed patch per
   draw (patch = `VGT_INDX_OFFSET`), factor = guest maximum (16).
   - They go into the same EDRAM colour (format 12 = the same bits viewed as 16_16_16_16).
   - z write is on, zfunc GreaterEqual, no polygon offset (`PA_SU_SC_MODE_CNTL` 00218002, offsets 0).
   - The first layer of a patch replaces the colour (blend `00060006`); the next ones add (`01060106`).

The opaque pass's colour is not the ground's look: **with both layer shaders skipped (`FABLE2_SKIP_VS=C30A…,FB68…`)
the terrain is plain white or black** (`out/validation/adaptive-flashes/evidence/lake-layers-skipped.png`). So any
pixel where the layers fail the depth test shows as white. That is the flash.

Each domain shader turns a patch index into ground, as the ucode does (patch sizes 8, 32 and 64):
- opaque: `idx = patch + VGT_INDX_OFFSET + c113.x`, `row = floor(idx·c11.y)`, `col = idx − row·c11.x`,
  `origin = (col, row)·c8.xy + c47.xy`;
- layers: the same, without c113, with `origin = (col, row)·c46.xy + c113.xy`.

Same ground = same depth target, same heightmap (tf16 address), same origin and same size. Pairing every layer
patch with the opaque patch under it (`tools/tess_frame_match.py`) left 0 of 74,852 layer patches unpaired.

The guest's adaptive factors are crack-free by design: a patch's cp1 equals cp3 of its +x neighbour (100% of 428k
pairs), and its cp2 equals cp0 of its +y neighbour (99.95%). So the factor read and the edge map
(pattern 78) are right.

## Mechanism

Before the fix (`FABLE2_TERRAIN_LAYER_MATCH=0`), adaptive on, overlook, 49 dumped frames: only **20%** of layer
patches had the opaque patch's factors *and* its tessellation path.

1. **Factors (16%).** The guest gives patches crossing or outside the view's edge factor 2 on their outer edges
   (and so inside). Layered patches inside the view are at 16. The layers stay at the uniform 16, so on those
   edge patches the layer surface is not the surface the prepass wrote. Wherever the fine surface lies behind the
   coarse one, every layer fails GreaterEqual and the white base shows. While the camera turns these patches
   enter the view for a frame or two: transient white at the screen edge and near the camera.
2. **Path (the other 64%).** In `FABLE2_NATIVE_TESS=1`, an opaque draw with any non-uniform patch (almost every
   adaptive draw) takes the reference pattern path. The layers took Metal's tessellator.
   - The reference path's vertex function is a plain vertex function (`xe_native_ds_cpu`); Metal's is a
     post-tessellation one (`xe_native_ds`). Same math, same uv at factor 16, but compiled separately.
   - GreaterEqual against the prepass needs bit-identical depth, which the default (adaptive off) gets because
     every pass is `xe_native_ds`.
   - On the M5 Max the two compile to the same results; on the M3 Pro (apple9) they evidently do not: one
     rounding step behind is a failed layer. That fits the evidence: the uv snap (run 3) changes only the uv,
     which was never the difference.
3. **`FABLE2_NATIVE_TESS=3` (run 4)** puts every opaque draw on the reference path and leaves the layers on
   Metal's tessellator: **0%** of layer patches share the opaque path (dump, TESS=3 without the fix). The
   comparison is then cross-path on the whole visible terrain, hence more flashes.
   - **Blue flashes**, an inference, not seen here: the layers are separate pipelines too (one per PS). Where the
     replacing layer fails and an additive layer passes, the base plus one coloured layer shows (e.g. the
     blue-tinted `AF1E16EE`) instead of white.

Why the M5 Max did not show it, at the camp spot or at the lake: mechanism 2 needs per-pipeline rounding
differences, which apple10 does not produce. Mechanism 1 affects only patches at the view's edge for a frame, in
the region the detector skips (the 4% border), and stays within the run-to-run noise of the detector there (below).

**The original game.** The Xbox 360 draws the layers at the continuous maximum over the adaptive opaque terrain,
the same data as here. There is no depth bias that would hide a mismatch. All of its passes share one fixed-function
tessellator, so mechanism 2 cannot happen there. Mechanism 1 would exist on the 360 for edge patches. Whether it
was visible there could not be determined: no hardware or footage was compared. The fix gives the visually
intended result (layers exactly on the ground). For those edge patches that is a small drift from the 360's
geometry, and only with adaptive terrain on, itself an opt-in setting.

## Fix

`MatchTerrainPatchFactors` (`metal_command_processor.cc`), when `FABLE2_TERRAIN_ADAPTIVE=1` and
`FABLE2_TERRAIN_LAYER_MATCH` is not `0`:
- Every adaptive opaque terrain draw (VS `5003700B`) records its per-patch factors for the frame, keyed by depth
  target, heightmap, origin and size.
- Every layer draw (VS `C30A97D9` / `FB68A7F2`, continuous) takes the factors of the opaque patch under each of its
  patches. They use the adaptive edge map: edge i = cp[(i + 3) & 3], inside = minimum of the opposite edges. A
  patch with no opaque patch keeps 16.
- In `FABLE2_NATIVE_TESS` modes 1 and 3, all those terrain draws (opaque and layers) take the reference pattern
  path. One vertex function type and one cached pattern per factor tuple give the same patch bit-identical
  positions in every pass. Mode 2 keeps everything on Metal's tessellator, which is consistent too.
- The layers' triangle order is the reference's. Within one patch it only matters where a patch overlaps itself on
  screen (`docs/native-translator.md`, "Primitive order caveat").

Logs: `FABLE2_NATIVE_TESS_TRACE=1` adds `layer-match=0|1` to the device line, then every 600 frames:
`[tess-trace] terrain layers: N patches took the opaque pass's factors, M had no opaque patch`. Dev Launcher:
"Adaptive terrain: layers follow the ground's detail (no white flashes)".

## Validation (M5 Max, lake overlook, 2×, `out/validation/adaptive-flashes/`, KEEP)

**Consistency** (`tools/tess_frame_match.py`, dumps of 40 frames):

| Build / arm | Same factors | Same path | Both |
|---|---|---|---|
| before (adaptive, mode 1) | 83.9% | 20.4% | 20.4% |
| `LAYER_MATCH=0`, TESS=3 | 83.2% | 0% | 0% |
| fix, mode 1 | 100% | 100% | 100% |
| fix, TESS=3 | 100% | 100% | 100% |

**Flash detector** (flash mode, 2 runs per arm, interleaved; flash + crack frames per run):

| Arm | per run | vs off |
|---|---|---|
| noadapt | 1+10, 0+8 | — |
| adapt-nomatch (before) | 3+14, 1+14 | +1.5 / +5.0 |
| adapt (fix) | 0+11, 1+9 | 0.0 / +1.0 |
| adapt-tess3 (fix) | 2+10, 2+7 | +1.5 / −0.5 |

- The worst moments are the same breadcrumb-sparkle frames in every arm.
- The before arm is the highest, but within the ±3 / ±6 noise in `docs/flash-kit.md`.
- The M5 Max does not reproduce the M3 flashes. The M3 Pro run of the retargeted kit is the real check.

**Frame-matched parity** (`f2diag run still`, 2×, lake save):
- match vs nomatch: cross median 0.52 against a floor of 0.51; noise limit 1.18; best pair 0.20, below both
  floors. The verdict line says beyond-noise, but all of its hot blocks come from one pairing, in the animated
  lake water.
- On screen at this spot the layered patches are at 16, so nothing changes there, as expected.
- One capture (`af-match-2`, load anchor 125 instead of ~107) was an outlier against every arm and was replaced by
  `af-match-3`.
- adaptive vs off: cross 1.02 against a floor of 0.29. This is the known drift of adaptive terrain itself
  (distant terrain at the guest's lower detail), not this fix.

## Diagnostics added

- `FABLE2_TESS_FRAME_DUMP=<file>` (+ `FABLE2_TESS_FRAME_DUMP_FRAMES=first:every:count`, default 600:60:20) writes
  one JSON line per tessellated draw of the chosen frames. Each line has:
  - shaders, path, blend and depth state, depth/colour/surface info, polygon offset and viewport z;
  - VGT index offset, minimum and maximum;
  - terrain constants c0–3, 8, 11, 14, 46, 47, 72, 113–115, 253–255, and the tf16–19 addresses;
  - the factors each patch was drawn with, and the guest indices.
- `tools/tess_frame_match.py <dump>`: per-frame layer/opaque pairing (factors, path), plus the guest's
  neighbour-edge agreement.
- `FABLE2_SKIP_VS` takes a comma-separated list.

## Flash kit

Retargeted to this spot (`tools/fable2_flash_kit.py`, save `hero1-dog-live_2026-10-07_14-10-08`, the tomb
overlook on the shore). The arms are now `adapt`, `adapt-nomatch` (before the fix), `adapt-tess3`, `noadapt`; the
snap arms can still be passed with `--arms`. Reading the M3 report:
- `adapt` and `adapt-tess3` at the `noadapt` floor while `adapt-nomatch` flashes: the fix works;
- `adapt` still flashing: a third mechanism (look at `examples/`).

Kit rebuilt 2026-10-15 (f5da126): the packaged no-converter kit, end to end on the M5 Max, took 2.8 minutes
(`out/flash-kit/e2e/reports/M5-Max-Mac17_14-2026-10-15_10-11-flash`, KEEP). All arms are at the floor, flash + crack
per run:

| Arm | Run 1 | Run 2 |
|---|---|---|
| adapt | 0+0 | 2+9 |
| adapt-nomatch | 0+3 | 0+12 |
| adapt-tess3 | 0+14 | 0+10 |
| noadapt | 1+12 | 0+9 |

The report's arm column cuts the `adapt-nomatch` title ("…before the fi").
