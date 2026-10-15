# Bower Lake water loses its ripples in straight-edged patches at 2×/3× (2026-10-14)

**Fixed:** `FABLE2_SCALED_RESOLVE_PAGE_FILL` (default on, `=0` restores the old behaviour; Dev Launcher → Fixes →
Render targets → "2×/3× resolves: keep neighbouring data"). Bug-patterns row 93.

## Symptom

Playtest on the M3 Pro test Mac (portable app 8601508, 2×), during "The Birth of a Hero: Enter Bower Lake Tomb":
- From the overlook, the lake splits along a hard straight edge. On one side the water has ripples and reflections;
  on the other it is flat and pale.
- On the tomb island, one rectangular patch of water has ripples and the rest is flat.
- Near the shore, the water surface is missing around the hero's ripple ring.

## Reproduction

- Library save `hero1-dog-live_2026-10-07_14-10-08` (QC060, "Birth of a Hero") boots at the overlook. The bug
  shows every time at 2× on the M5 Max as well.
- `f2diag run still` at 2× and 1×, frame-matched. Evidence is in `out/validation/lake-water-20261014/` (KEEP):
  - `img/ab-2x-fixoff-fixon.png`: top is the fix off, bottom is the fix on.
  - `img/lw-on1x-crop.png`: the 1× reference.
  - `img/bisect-2x-on-off-switches.png`.
  - `dumps/`: the resolve dumps (game-derived; out/ only).

## Bisect

- **Not a regression from 2026-10-13/14.** Every switch from those two days off still shows the edge at 2×:
  - `NATIVE_TESS`, `NATIVE_TESS_BLENDED`, `NATIVE_RECTS`, `NATIVE_POINTS`, `NATIVE_XLATE`, `NATIVE_XLATE_VS`;
  - `TILE_RESOLVE`, `MIPLOD_PROOF`, `TEX_RENAME`, `ALIAS_CLEAR_NATIVE`;
  - `DRAW_PREP_LITE`, `DRAW_PREP_LITE2`, `TEX_KEY_MEMO`, `LOAD_CHECK_EXACT`, `PASS_MERGE`, `DHR_STORAGE_FORMAT`.

  The same holds for each older resolution-scale switch: `EFFECTS_NATIVE`, `SHADOWS_NATIVE`, `RESOLVE_TO_TEXTURE`,
  `TEX_LOAD_DIRECT`, `TILE_EDRAM`, `DEFERRED_CLEARS`, and `RTT_MIPS`/`RTT_16BPP`/`RTT_1X`.
- **1× is correct.**
- So this is a pre-existing resolution-scale bug.
- (The M3's white flashes are a different bug, in native tessellation. Another agent is working on it.)

## What the water draws

- **The ripple layer.** `FABLE2_SKIP_VS=BEAD84BD72072E0E` at 1× turns the whole lake flat and pale. That VS draws the
  ripple and reflection layer: 10 chunk draws per pass, PS `F6D98C7B4D98438B`, then blended PS `A17D8AEC3A817D45`.
- **The VS** fetches a per-chunk lake-bed heightmap as a vertex texture (fetch 16).
- **The PS** samples the same heightmap (fetch 1) and computes the water depth from it.
  - Where the bed reads as above the water, it discards.
  - A zero heightmap therefore removes the layer on the whole chunk. The straight edges are chunk boundaries.
- **The heightmaps** are small tiled `k_8` textures: 64×64, 32×32 and 16×32, at `0x1B6D0000`–`0x1BD1B000`. Each is
  logged with `FABLE2_DIAG_TEX_VS=<vs>` (fetch constants and the scaled flag per draw).

## Root cause

**The game builds the heightmaps in place.** At area load (one frame), for each chunk it:
1. draws a full-screen rectangle (VS `B2D99AC2`, PS `5F329DB1`). The PS samples the chunk's CPU-loaded data at
   address X.
2. resolves the result back to X.
3. repeats steps 1–2 three times.

`FABLE2_DIAG_RESOLVE_DUMP` shows how this goes wrong. At 2×, 4 of the 10 water heightmaps (`1BD03000`, `1BD0E000`,
`1BD17000`, `1BD1B000`) came back all zero. At 1× they hold real data (means 133, 11, 95, 53).

Each one is the **second heightmap in a 16 KB page**: `1BD01000`→`1BD03000`, `1BD0C000`→`1BD0E000`,
`1BD15000`→`1BD17000`, `1BD19000`→`1BD1B000`. The mechanism:

- **Resolves mark whole host pages valid.** Every resolve ends in `MarkRangeAsResolved` →
  `SharedMemory::RangeWrittenByGpu` → `MakeRangeValid`. That marks whole host pages as valid shared-memory data.
  Host pages are 16 KB on Apple silicon (the watch fires on `1BD0C000-1BD0FFFF` for a 4 KB resolve).
- **At 1× the pages are uploaded first.** The resolve's destination prep (`PrepareResolveDestinationBuffer`) first
  requests the destination range (`RequestSharedMemoryRange(kResolveCopyDest)`). That uploads the whole page, so the
  neighbour's CPU data is in the shared buffer before the page is called valid.
- **At scale > 1 the upload was skipped.** The scaled branch returned before the request, because the resolve writes
  only the scaled-resolve buffer. The neighbour's CPU data was marked valid without ever being uploaded.
- **The neighbour's texture then reads zeros.**
  - Its texture is unscaled. The CPU-guest-memory load path (`LoadTextureDataFromCpuGuestMemory`) refuses, because
    the range is no longer invalid.
  - So the texture loads from the shared buffer, which never received those bytes (zeros).
  - The game renders and resolves a zero heightmap, and the water PS discards the chunk.
- **Why Xenia on Windows never hits this.** Its host pages are 4 KB, the same as the guest's, so a page holds only
  the resolve's own bytes.

The bug is not specific to water: any texture that shares a 16 KB page with a small scaled resolve and has not been
uploaded yet loads zeros at 2×/3×. The water heightmaps are the visible case.

## Fix

The scaled branch of `PrepareResolveDestinationBuffer` (`metal_direct_host_resolve.cc`; used by the compute, direct
host and tile resolve paths) now requests the destination range too, as the 1× branch does.

- Pages that are already valid cost nothing, so this is a one-time upload per page.
- The resolve's own bytes in the shared buffer become the old guest data instead of undefined. Nothing reads them
  unscaled at scale.
- `FABLE2_SCALED_RESOLVE_PAGE_FILL=0` restores the old behaviour.

## Validation (M5 Max, 2×)

- **Heightmaps.** `tools/resolve_dump_diff.py` compares the 1× dumps with the 2× dumps (downscaled, centre sample):
  - before: 12 of 30 matched resolves fail (all zero at scale);
  - after: 0 fail, with a mean absolute difference of 1.2–7.8 per byte.
  - `1BD17000` (64×32, 3072-byte extent) has one all-zero 1 KB tile. It is a hole in the written extent that the
    resolve never writes; the tool reports it as `hole?`.
- **Picture.** The overlook matches 1×: continuous ripples, the pillar reflections, and no edge behind the island
  (`img/ab-2x-fixoff-fixon.png`).
- **Elsewhere.** Bower camp (`hero1-dog-live_2026-10-11_11-20-20`), 2 frame-matched stills per arm, fix on vs `=0`:
  - cross median 0.23, against an A-A floor of 0.26 (noise limit 0.71);
  - the only hot block is the animated camp fire (`img/stack-camp.png`).
- **Perf.** Bower camp 2× headings8 uncapped, no contention, pinned cache: on 129.56 FPS, off 129.67 FPS (neutral).

## Diagnostics added

| Switch | What it does |
|---|---|
| `FABLE2_DIAG_RESOLVE_DUMP=<dir>` + `FABLE2_DIAG_RESOLVE_RANGE=<lo>-<hi>` (hex) | Writes every resolve into the range to `<dir>`, plus a `[diag-resolve]` log line with the copy and surface registers. At 1× it writes the bytes; at scale > 1 it writes the scaled result downscaled to 1× (whole-tile lengths only; guest RAM is not written). Compare with `tools/resolve_dump_diff.py <1x dir> <2x dir>` |
| `FABLE2_DIAG_TEX_VS=<vs hash>` (+ `FABLE2_DIAG_TEX_FRAME=<n>`) | `[diag-tex]` log lines: the fetch constants and the scaled flag of every texture used by the first 64 draws of that VS, in frame `n` or every 120th frame |
