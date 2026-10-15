// [tile-resolve] Host colour resolve as a TILE function inside the render pass
// that drew the source (docs/tile-resolve-20261013.md).
//
// Compiled by tools/compile_metal_helpers.py once per covered resolve variant,
// with the same defines as the compute variant. It reuses, unmodified:
// - the XeniOS resolve code (resolve_host_color.xesli: tiling, packing,
//   endian, destination addressing), and
// - the resolve-to-texture store wrapper (resolve_dest_texture.metal: buffer
//   store plus the fused texture writes).
// Only two things differ from the compute resolve:
// 1. The source "texture" is an XeTileSrc holding the texels this thread reads,
//    pre-read from the implicit imageblock (tile memory) instead of the
//    attachment in device memory. Its read() has the same signature as
//    texture2d::read, so the XeniOS fetch macros are unchanged.
// 2. Threads are laid out over the Metal tile, not over the EDRAM dump: each
//    thread takes the pixel group whose first source texel is its anchor
//    texel (inverse of the resolve's thread -> texel mapping) and runs the
//    unmodified main for that group's thread index. A group is processed only
//    by the thread whose anchor equals the group's own first texel (checked
//    with the forward mapping), so each group is written exactly once.
// Host conditions (metal_direct_host_resolve.cc): float source view (1x/2x/4x MSAA),
// one source render target bound as colour attachment 0, Metal tile width a
// multiple of the pixels per thread. Half-pixel fill rows (scaled resolves)
// are written by the thread of the source row they copy.

#define XE_RTT_ENTRY "resolve_tile_entry.inc"
#include "resolve_dest_texture.metal"
