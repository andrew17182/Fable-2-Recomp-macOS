// [tile-resolve] Tile entry, included by resolve_dest_texture.metal through
// XE_RTT_ENTRY from resolve_tile.metal. See resolve_tile.metal.

#if XE_RESOLVE_HOST_COLOR_SOURCE_UINT
#error tile resolve: only float source views are covered
#endif

struct XeTileColor {
  float4 color [[color(0)]];
};

// Stand-in for texture2d<float>: reads the attachment from tile memory (the
// implicit imageblock) at the same texel, relative to this Metal tile.
template <typename T>
struct XeTileSrc {
  imageblock<XeTileColor, imageblock_layout_implicit> block;
  uint2 origin;
  float4 read(uint2 position, uint lod) const {
    return block.read(ushort2(position - origin)).color;
  }
};
// Stand-in for texture2d_ms<float>: the same per sample.
template <typename T>
struct XeTileSrcMs {
  imageblock<XeTileColor, imageblock_layout_implicit> block;
  uint2 origin;
  float4 read(uint2 position, uint sample_index) const {
    return block
        .read(ushort2(position - origin), ushort(sample_index),
              imageblock_data_rate::sample)
        .color;
  }
};

#define texture2d XeTileSrc
#define texture2d_ms XeTileSrcMs
#ifdef XE_RESOLVE_HOST_COLOR_FULL_DEST_BPP
#include "resolve_host_color.xesli"
#define XE_TILE_MAIN XeResolveHostColorFullMain
#else
#include "resolve_host_color.xesli"
#define XE_TILE_MAIN XeResolveHostColorMain
#endif
#undef texture2d
#undef texture2d_ms
#if XE_RESOLVE_HOST_COLOR_MSAA_SAMPLES == 1
#define XE_TILE_SRC XeTileSrc
#else
#define XE_TILE_SRC XeTileSrcMs
#endif

kernel void entry_xe(
    imageblock<XeTileColor, imageblock_layout_implicit> xe_tile_block,
    XE_RESOLVE_PUSH_CONST_BINDING,
    byte_buffer_wo_binding_xe(xe_resolve_dest, buffer(1)),
    XE_RESOLVE_HOST_COLOR_CONSTANTS_BINDING,
    ushort2 xe_tile_lid [[thread_position_in_threadgroup]],
    ushort2 xe_tile_tg [[threadgroup_position_in_grid]],
    ushort2 xe_tile_tpt [[threads_per_threadgroup]]) {
  const uint ppt = XeResolveHostColorPixelsPerThread();
  // Host pixels of this Metal tile: (threads x pixels per thread) x rows.
  const uint2 tile_texels = uint2(uint(xe_tile_tpt.x) * ppt, xe_tile_tpt.y);
  const uint2 tile_origin = uint2(xe_tile_tg) * tile_texels;
  const uint2 host_pixel =
      tile_origin + uint2(uint(xe_tile_lid.x) * ppt, xe_tile_lid.y);

  XeResolveInfo resolve_info = XeResolveGetInfo(push_consts_xe);

  // The group's first source sample in the resolve's sample space (what
  // XeResolveHostColorBaseTexelForLane returns): host pixel to samples, plus
  // the first selected sample's offset.
  const uint2 anchor =
      (host_pixel << XeResolveHostColorSampleInTileShift()) +
      XeResolveHostColorSampleOffsetForIndex(
          XeResolveFirstSampleIndex(resolve_info.sample_select));

  // Inverse of XeResolveHostColorSourceSampleToTexel: render-target tile,
  // then EDRAM tile, then dump tile.
  const uint tile_size_x = XeResolveHostColorTileSizeX(resolve_info);
  const uint tile_size_y = XeResolveHostColorTileSizeY(resolve_info);
  const uint source_pitch_tiles =
      xe_resolve_host_color.xe_resolve_host_color_source_pitch_tiles;
  const uint rt_tile_x = anchor.x / tile_size_x;
  const uint rt_tile_y = anchor.y / tile_size_y;
  if (rt_tile_x >= source_pitch_tiles) {
    return;
  }
  const uint2 in_tile =
      (anchor - uint2(rt_tile_x * tile_size_x, rt_tile_y * tile_size_y)) >>
      XeResolveHostColorSampleInTileShift();
  const uint local_tile =
      rt_tile_y * source_pitch_tiles + rt_tile_x +
      xe_resolve_host_color.xe_resolve_host_color_source_base_tiles -
      xe_resolve_host_color.xe_resolve_host_color_dump_base;
  const uint dump_pitch_tiles =
      xe_resolve_host_color.xe_resolve_host_color_dump_pitch_tiles;
  const uint dump_tile_y = local_tile / dump_pitch_tiles;
  const uint dump_tile_x = local_tile - dump_tile_y * dump_pitch_tiles;
  if ((in_tile.x % ppt) != 0u) {
    return;
  }
  const uint2 thread_index = uint2(
      (dump_tile_x * XeResolveHostColorTilePixelSizeX(resolve_info) +
       in_tile.x) / ppt,
      dump_tile_y * XeResolveHostColorTilePixelSizeY(resolve_info) +
          in_tile.y);

  // Forward check: this thread owns the group only if the resolve itself
  // maps the group's first texel here (range checks included).
  uint2 pixel_index;
  uint check_local_tile;
  uint2 pixel_in_tile;
  if (!XeResolveHostColorGetPixelIndex(resolve_info, thread_index, pixel_index,
                                       check_local_tile, pixel_in_tile,
                                       xe_resolve_host_color)) {
    return;
  }
  XeResolveHostColorSourceBase source_base =
      XeResolveHostColorComputeSourceBase(resolve_info, pixel_index,
                                          check_local_tile, pixel_in_tile,
                                          xe_resolve_host_color);
  const uint2 base_texel = XeResolveHostColorBaseTexelForLane(
      resolve_info, source_base, pixel_index, xe_resolve_host_color);
  if (base_texel.x != anchor.x || base_texel.y != anchor.y) {
    return;
  }

  XE_TILE_SRC<float> source{xe_tile_block, tile_origin};
  XE_TILE_MAIN(thread_index, xe_resolve_dest, xe_rtt_t0, xe_rtt_t1, xe_rtt_p,
               push_consts_xe, xe_resolve_host_color, source);
  // Half-pixel offset fill (scaled resolves): output rows above
  // half_pixel_offset_fill_source.y read the source row fill.y itself (the
  // Y clamp in XeResolveHostColorSourceSampleForSampleIndex), so the thread
  // anchored on that row also writes them. Thread index Y is linear in the
  // dump pixel row, and the lanes are the same texels.
  const uint fill_y = resolve_info.half_pixel_offset_fill_source.y;
  if (fill_y != 0u && pixel_index.y == fill_y) {
    for (uint k = 1u; k <= fill_y; ++k) {
      XE_TILE_MAIN(uint2(thread_index.x, thread_index.y - k), xe_resolve_dest,
                   xe_rtt_t0, xe_rtt_t1, xe_rtt_p, push_consts_xe,
                   xe_resolve_host_color, source);
    }
  }
}
