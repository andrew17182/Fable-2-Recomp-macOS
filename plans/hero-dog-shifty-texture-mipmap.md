# Fixing the shifty / "multiple textures" dog — step-by-step (v2)

The v1 plan (Option A / B / C) is superseded. This version breaks the work into small,
verifiable steps so we stop guessing and start reading the ground truth.

> **Current state (fable_2_018.log):** the "multiple textures" is **gone** (Step 2 fetch-layout
> offsets landed the 512×512 mips correctly); the dog is now **1 texture but shimmering**. Full
> attempt-by-attempt history is in the **Attempt-by-attempt log** section below.

## Status (what we know for certain — updated after Step 1 + A/B)

The "two alternating mipmap chains" theory from v2 is **WRONG**. Step 1's HERODOG_FETCH +
HERODOG_READBACK plus two A/B toggles give the real picture:

1. **Hero + dog are visible** with the targeted EDRAM-resolve readback at `0x12704000`.
2. **The forced address `0x12704000` hosts TWO textures:**
   - **Dog = a 512×512, fmt=6, 5-mip chain**, resolved **once** (frame ~1506):
     1 MiB / 480 KB / 208 KB / 72 KB / 4 KB.
   - **A 256×256, fmt=3, packed-mips texture**, resolved **every frame** (128 KB mip0, hash changes
     per frame → dynamic). It is the texture actually **bound + sampled** (HERODOG_FETCH: sampler
     slots 13 & 0, `base=0x12704000 mip=0x12724000 max=3 packed=1 tiled=1`).
3. **A/B: skip the 512×512 chain → dog is PITCH BLACK.** ⇒ The 512×512 5-mip chain **is** the
   dog's texture.
4. **A/B: skip the 256×256 every-frame resolve → dog is STILL "multiple textures"** (the original
   bug, unchanged). ⇒ The 256×256 is **not** the clobberer; the "multiple textures" is the
   **512×512 chain's own mips landing at the wrong guest offsets**.
5. **Readback is synchronous + in-order** (the forced path uses `readback_resolve = kDisabled`,
   so `use_delayed_sync = false` → `AwaitAllQueueOperationsCompletion()` then an immediate memcpy).
6. **Copy source stays at the EDRAM base** per mip (correct — EDRAM holds the current mip).
7. **Destination is TILED** (`GetResolveInfo` uses `GetTiledAddressLowerBound2D`); every mip
   resolves to the tiled base `0x12704000` because each is a full-frame resolve from origin
   (tiled offset `0`).

## The open question (Step 1 answered; Step 2 must fix)

- **Answered:** the dog is the 512×512 fmt=6 5-mip chain; the 256×256 fmt=3 is a separate
  dynamic, sampled texture at the same base.
- **Remaining:** our **cumulative** per-mip guest offset (base + running sum of `written_length`)
  does **not** match where the dog's mips must live. A *tiled* 5-mip chain's mips are not a
  contiguous pack of `written_length`s — the fetch layout (`mip_offsets_bytes[level]` from
  `GetGuestTextureLayout`) is the ground truth for each mip's byte offset. **Step 2 = place each
  mip at its fetch-computed offset, not the cumulative one.**

`TextureInfo`/`TextureKey` (built from `xe_gpu_texture_fetch_t`) already carries the full fetch
layout: `fetch.base_address`, `fetch.mip_address` (full 32-bit), `width`, `height`, `format`,
`tiled`, `packed_mips`, `mip_max_level`, and a `guest_layout()` → `TextureGuestLayout` with
`mip_offsets_bytes[level]`.

## Steps

### Step 1 — Instrument the fetch (sample) side + enrich the resolve-side HERODOG
Goal: capture the guest fetch layout for the hero/dog textures and the per-mip resolve sizes.

- **(a) Resolve side** (d3d12 + vulkan HERODOG): add `copy_dest_pitch` (mip width),
  `copy_dest_height` (mip height), a per-burst mip-level counter, and the raw
  `RB_COPY_DEST_BASE`. Log alongside the existing `base`/`dest`/`len`.
- **(b) Fetch side** (texture cache, after `BindingInfoFromFetchConstant`): when a binding's
  `fetch.base_address` is inside the forced window, log `base_address`, `mip_address`,
  `width`, `height`, `mip_max_level`, `tiled`, `packed_mips`, `format`, plus a frame/sequence
  marker so it lines up against the resolve burst.

Deliverable: one log that shows (i) each chain's per-mip width/height (→ mip level), (ii) each
character's fetch `base_address` + `mip_address`, and (iii) the ordering of resolves vs the
sampling draws. **Then stop and read the log before changing any placement logic.**

### Step 2 — Place each mip at its fetch-computed guest offset (IN PROGRESS)
A/B proved the cumulative offset is wrong for a *tiled* 5-mip chain. Fix: for the forced
dog readback, look up the texture's fetch layout (`TextureGuestLayout.mip_offsets_bytes[level]`
via `GetGuestTextureLayout` / the texture cache) and write each mip to
`fetch.base + mip_offsets_bytes[level]` instead of `base + cumulative(written_length)`.
- Detect the mip level from the resolve (width/height shrink per mip, or a per-burst counter
  reset on a larger-than-previous mip).
- Keep the 512×512 (dog) readback; the 256×256 every-frame resolve is skipped (it is not the dog
  and reading it back does not fix the dog).

### Step 3 — Verify the 256×256 sampled texture is not a visible regression
The 256×256 fmt=3 is bound on sampler slots 13/0. If skipping it leaves a visible black region,
reconsider (read it back too, or relocate it). Confirm with the user.

### Step 4 — Verify + clean up
Rebuild, stage, run; confirm HERODOG matches the fetch layout and the dog is stable and not
"multiple textures." Remove diagnostics, update the credit section, mark the plan done.

## Files
- `thirdparty/rexglue-sdk/src/graphics/d3d12/command_processor.cpp` — HERODOG enrich (a)
- `thirdparty/rexglue-sdk/src/graphics/vulkan/command_processor.cpp` — HERODOG enrich (a)
- `thirdparty/rexglue-sdk/src/graphics/command_processor.{h,cpp}` — mip-level counter (a)
- `thirdparty/rexglue-sdk/src/graphics/pipeline/texture/cache.cpp` — fetch-side capture (b)

## Credit
Mechanism (targeted EDRAM-resolve readback of the player/dog texture at `0x12704000`) is from
**just-harry's** Xenia femtofork ("black-texture-bug"). This plan extends that fix to the
mipmap-chain + two-character case the fork leaves imperfect.

## Attempt-by-attempt log (full history)

Chronological record of every change and its observed in-game effect. Release logs live at
`out/build/win-amd64-release/logs/fable_2_NNN.log`. "Effect" = what was observed in-game.

### Phase 0 — Make the black hero/dog visible (plan: `hero-dog-texture-readback.md`)
Starting point: hero + dog render **black** (no readback).

| # | Change | Effect |
|---|--------|--------|
| 0.1 | New hot-reload SDK cvar `readback_resolve_force_addresses`; `CommandProcessor::ShouldForceReadbackResolve(base)`; an `IssueCopy()` force-gate in **both** D3D12 + Vulkan that takes the existing readback path when the resolve dest base is in the list (mode stays `kDisabled` → immediate sync readback). | Hero + dog no longer **black** (main bug fixed). Dog had residual artifacts. |
| 0.2 | Gate also requires the blit shape: `VGT_DRAW_INITIATOR.prim_type == kRectangleList (0x08)` && `num_indices == 0x03`. | Dog still **glitchy / shifting** — partial/extra copies to the same region still fired the readback mid-update. |
| 0.3 | Shrink the address-match window **1 MiB → 64 KiB**. | `0x12704000` is surrounded by dense EDRAM-resolve targets (`0x12724000`, `0x1272c000`, …); the 1 MiB window caught all of them → per-copy GPU stalls + **glitchy / partly-red / shifting** dog. 64 KiB stops the neighbor catches. |
| 0.4 | **Key the gate on the `PM4_DRAW_INDX_2` opcode** via a `may_require_readback_resolve_` member (false on `PM4_DRAW_INDX`, true on `_2`). | **The real fix.** Regular `PM4_DRAW_INDX` draws sample the texture mid-frame (also a `kRectangleList`/3 copy to the same base); reading those back captured a mid-update state. Only the EDRAM-resolve copy (`PM4_DRAW_INDX_2`) now triggers readback → hero **and** dog stable + visible. |
| 0.5 | App: `[patches] hero_dog_texture_readback` toggle (default `true`); seed the cvar in `Fable2App::OnPostSetup()`; credit just-harry. | A/B-able at runtime (F3 console `readback_resolve_force_addresses`) with no rebuild. |

### Phase 1 — The "shifty dog" (this plan)
Dog was now **constantly shifting** (hero stable). Root-cause hunting on the mipmap layout.

| # | Change | Effect |
|---|--------|--------|
| 1.1 | **Option A** — read back **only mip0** (skip the non-largest mips). | **FAILED — made shifting worse.** The dog samples lower mips, so dropping them left the sampler reading stale/garbage. |
| 1.2 | **Option B** — read back the whole chain at **sequential (cumulative)** guest offsets (mip N at base + running sum of the previous mips' `written_length`). | **Partially works.** Offsets consistent per chain, but the dog still **shifts / shows "multiple textures"** — cumulative offsets do not match where a *tiled* mipmap chain's mips live. |
| 1.3 | **Burst reset on base-mip-size change** — track `force_readback_burst_prev_length_`; a mip larger than the previous starts a new chain (reset the running offset). | Stops a new chain from continuing an old chain's offset; still **shifting** (root cause not yet fixed). |

### Phase 2 — Instrument the ground truth (Step 1)

| # | Change | Effect |
|---|--------|--------|
| 2.1 | Enrich **HERODOG_READBACK** (frame, raw `RB_COPY_DEST_BASE`, cumulative `dest`, `len`, mip `w`/`h`, `fmt`, hash) + add **HERODOG_FETCH** (the sampled texture's fetch layout for any binding whose `base` is in the forced window). | Ground truth (fable_2_013.log): (a) the **dog = 512×512 fmt=6 5-mip chain, resolved once** (frame ~1506: 1 MiB / 480 KB / 208 KB / 72 KB / 4 KB); (b) a **256×256 fmt=3 packed-mips texture, resolved every frame** (128 KB mip0, hash changes) — the one actually **bound + sampled** on slots 13/0 (`base=0x12704000 mip=0x12724000 max=3 packed=1 tiled=1`). The 512×512 is **not** in the fetch log. |

### Phase 3 — A/B: which texture is the dog? (fix attempts)

| # | Change | Effect |
|---|--------|--------|
| 3.1 | **Skip the 512×512 chain** (gate readback on `w==256 && h==256`). | Dog **PITCH BLACK**, hero fine ⇒ the **512×512 5-mip chain IS the dog's texture**. |
| 3.2 | **Skip the 256×256 every-frame resolve** (gate readback on `w==512 && h==512`). | Dog **still "multiple textures"**, hero fine ⇒ the 256×256 is **not** the clobberer; the "multiple textures" is the **512×512 chain's own mips at the wrong guest offsets**. |

**Conclusion:** the **cumulative** per-mip offset is wrong for a *tiled* 5-mip chain; the mips must go to the fetch layout's `mip_offsets_bytes[level]`.

### Phase 4 — Place each mip at its fetch-layout offset (Step 2 — current)

| # | Change | Effect |
|---|--------|--------|
| 4.1 | Compute the forced-dog-readback destination from the texture's **fetch layout**: level 0 → `base`; level 1+ → `mip_address + mip_offsets_bytes[level]` (via `GetGuestTextureLayout`). API discovery: `mip_offsets_bytes[level]` is measured from the **mip_address**, not the base (confirmed against `TextureInfo::GetMipLocation`). Added `TextureCache::GetMipAddressForBaseAddress(base)` to pull the mip_address from the bound `TextureKey`; mip level derived from the burst's base width vs the current mip width. | **Dog is back to 1 texture (the "multiple textures" is gone)**, but it now **shimmers**. The wrong-offset placement is fixed; a residual shimmer remains to diagnose (below). |

**Current state (fable_2_018.log):** dog = **1 texture, shimmering**. HERODOG_FETCH still shows the sampled texture is the 256×256 fmt=3 (slots 13/0); the 512×512 readbacks are now at the fetch-layout offsets.

### Phase 5 — Export the dog texture to a BMP for visual review (in progress)
Instead of reasoning about the shimmer from hashes, dump the actual resolved pixels so the
user can see them.

| # | Change | Effect |
|---|--------|--------|
| 5.1 | New SDK diagnostic (D3D12 `IssueCopy_ReadbackResolvePath`): after the guest memcpy, for a **forced** readback, de-tile the readback buffer with `texture_util::GetTiledOffset2D(x, y, pitch, bpp_log2)` (byte 0 == tile origin, since it's a full-frame resolve from origin) and write a 24-bit BMP via a small inline encoder (`ExportTiledTextureToBmp`). Handles the two formats at `0x12704000`: **k_8_8_8_8 (fmt=6, dog)** and **k_1_5_5_5 (fmt=3, dynamic 256×256)**; first **3 occurrences** of each unique (w×h×fmt) are written to `<exe dir>/texdump/fable2_tex_<W>x<H>_fmt<F>_<n>.bmp`. | Pending in-game test. Should yield the 512×512 dog chain (5 mips, fmt=6) + the dynamic 256×256 (fmt=3, 3 frames). |

Notes:
- Formats: dog mip0 = 512×512×4 = 1 MiB (fmt=6 k_8_8_8_8); dynamic = 256×256×2 = 128 KB
  (fmt=3 k_1_5_5_5) — both confirmed against the HERODOG sizes.
- The BMP is written bottom-up BGR; k_8_8_8_8 is read as byte[0]=R, byte[1]=G, byte[2]=B
  (Xenos 0xAABBGGRR). If the dump looks R/B-swapped, flip the two.

### Residual: the shimmer (open hypotheses)
1. The **sampled** descriptor is the 256×256 (`base=0x12704000 mip=0x12724000`), whose mip0 **overlaps** the 512×512's mip0 (both at `0x12704000`). The 256×256 is updated **every frame** (dynamic), so if the sampler reads mip0 from there the dog shows dynamic content → shimmer.
2. Mip selection / LOD flickering near a mip boundary.
3. A residual content mismatch between the 512×512 chain and the 256×256 descriptor's mip layout.

The Phase 5 BMP dump should settle this: if the 512×512 fmt=6 mip is a clean static dog image and
the 256×256 fmt=3 changes between frames, the shimmer is the dynamic 256×256 being sampled.

Note: the Phase 5 BMP exporter wrote **no files** on fable_2_019 (the `texdump/` dir is created but
empty) — `ExportTiledTextureToBmp` is returning false; needs a which-check-fails diagnostic. The
HERODOG_READBACK on that run shows only the 512×512 chain (5 mips, fmt=6, frame 1034) — the 256×256
is not read back (gated off), only the sampled 256×256 fires HERODOG_FETCH.

### A/B test: is the HERODOG_FETCH diagnostic the cause? (user hypothesis)
| # | Change | Expectation |
|---|--------|-------------|
| A/B 1 | Limit **HERODOG_FETCH** to fire **1 time** (`s_herodog_fetch < 1`). | **Result: shimmer persisted.** Confirmed HERODOG_FETCH is a **no-op log** (reads the fetch constant + `REXGPU_INFO`; does not touch texture state). The diagnostic is **ruled out**; the shimmer is from the real rendering. |

### Phase 6 — The real root cause of the shimmer: mip level was always 0 (IN PROGRESS)
The HERODOG_READBACK showed the 512×512 chain's 5 mips all report **`w=512 h=512`** (the
render-target size, NOT the mip size) and **`dest=0x12704000`** (the same base). The old mip-level
derivation used the mip **width** vs the base width (`for (w = base; w > width; w >>= 1)`), so with a
constant 512 width the level was **always 0** → **all 5 mips were written to the same base,
overlapping** (each smaller mip clobbering part of the larger one). That mix of resolutions is the
shimmer (the dog reads 1 texture, but its mip0 is a blend of mip0..mip4).

| # | Change | Expectation |
|---|--------|-------------|
| 6.1 | Mip level is now the **running index within the burst** (mips arrive largest-first, so the Nth mip is level N). `GetForceReadbackDestAddress` gains a `len` param; the burst resets when the address/frame changes **or a larger `len` arrives** (a new base mip). Mip 0 → base; mips 1+ → `mip_address + mip_offsets_bytes[level]`. Both backends pass `written_length`. | The 5 mips land at **distinct** fetch-layout offsets (no overlap) → the dog's mip0 is the clean 512×512, no shimmer. |

Open risk: the `mip_address` used for mips 1+ comes from `GetMipAddressForBaseAddress(0x12704000)`,
which resolves to whichever texture is **bound** at that base (HERODOG_FETCH shows the 256×256
fmt=3, not the 512×512 fmt=6). If the 512×512 chain's true mip region differs, mips 1+ may still
be off — check the dog at a distance (lower mips) after this build.
