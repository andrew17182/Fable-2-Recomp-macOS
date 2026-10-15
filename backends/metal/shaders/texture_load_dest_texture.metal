// [tex-load-direct] Resolution-scaled texture_load_* variant that writes the
// host texture directly instead of a staging buffer that a blit then copies
// into the texture (FABLE2_TEX_LOAD_DIRECT, docs/resolution-scale.md §15).
//
// Compiled by tools/compile_metal_helpers.py once per scaled load shader with
// XE_TEX_LOAD_SOURCE naming the unmodified XeniOS source, e.g.
// "texture_load_32bpb_scaled.cs.xesl". The load shader itself is unchanged:
// only its two macros for the destination are replaced, so every texel gets
// exactly the bytes the staging buffer would have held at the same position.
//
// The destination is bound as an unsigned-integer view whose texel size equals
// the host format's (R8Uint ... RGBA32Uint), so the write stores raw bits.
// xe_texture_load_dest_params[0] = (mip level, array slice, host bytes per
// texel, 0); [1] = (level width, level height, 0, 0) in host texels. The
// host-linear byte address the shader computes (z = 0: 2D levels only) is
// y * host_pitch + x * bytes_per_texel; host_offset is 0 on this path.

#ifndef XE_TEX_LOAD_SOURCE
#error XE_TEX_LOAD_SOURCE must name the scaled texture_load source
#endif

#define XE_TEXTURE_LOAD_RESOLUTION_SCALED
#include "texture_load.xesli"

#undef XE_TEXTURE_LOAD_ENTRY
#define XE_TEXTURE_LOAD_ENTRY(source_byte_buffer_alignment)                 \
  byte_buffer_align##source_byte_buffer_alignment##_declare_xe(             \
      xe_texture_load_source, set=1, binding=0, t0, space0)                 \
  entry_bindings_begin_compute_xe                                           \
    XE_TEXTURE_LOAD_PUSH_CONST_BINDING                                      \
    entry_binding_next_xe                                                   \
    texture2d_array<uint, access::write> xe_texture_load_dest_tex           \
        [[texture(0)]]                                                      \
    entry_binding_next_xe                                                   \
    constant uint4_xe* xe_texture_load_dest_params [[buffer(3)]]            \
    entry_binding_next_xe                                                   \
    byte_buffer_binding_xe(xe_texture_load_source, buffer(2))               \
  entry_bindings_end_inputs_begin_compute_xe                                \
    entry_in_global_thread_id_xe                                            \
  entry_inputs_end_code_begin_compute_xe

static inline void XeTextureLoadStoreTexture(
    texture2d_array<uint, access::write> dest, constant uint4_xe* params,
    uint host_pitch, uint byte_address, uint4_xe value) {
  const uint level = params[0].x;
  const uint slice = params[0].y;
  const uint texel_bytes = params[0].z;
  const uint width = params[1].x;
  const uint height = params[1].y;
  const uint y = byte_address / host_pitch;
  const uint x_bytes = byte_address - y * host_pitch;
  if (y >= height) {
    return;
  }
  switch (texel_bytes) {
    case 16u: {
      const uint x = x_bytes >> 4u;
      if (x < width) {
        dest.write(value, uint2(x, y), slice, level);
      }
    } break;
    case 8u: {
      const uint x = x_bytes >> 3u;
      for (uint i = 0u; i < 2u; ++i) {
        if (x + i < width) {
          dest.write(uint4_xe(value[2u * i], value[2u * i + 1u], 0u, 0u),
                     uint2(x + i, y), slice, level);
        }
      }
    } break;
    case 4u: {
      const uint x = x_bytes >> 2u;
      for (uint i = 0u; i < 4u; ++i) {
        if (x + i < width) {
          dest.write(uint4_xe(value[i], 0u, 0u, 0u), uint2(x + i, y), slice,
                     level);
        }
      }
    } break;
    case 2u: {
      const uint x = x_bytes >> 1u;
      for (uint i = 0u; i < 8u; ++i) {
        if (x + i < width) {
          dest.write(
              uint4_xe((value[i >> 1u] >> ((i & 1u) * 16u)) & 0xFFFFu, 0u, 0u,
                       0u),
              uint2(x + i, y), slice, level);
        }
      }
    } break;
    default: {
      const uint x = x_bytes;
      for (uint i = 0u; i < 16u; ++i) {
        if (x + i < width) {
          dest.write(
              uint4_xe((value[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu, 0u, 0u, 0u),
              uint2(x + i, y), slice, level);
        }
      }
    } break;
  }
}

#undef byte_buffer_align16_store16_xe
#define byte_buffer_align16_store16_xe(name, byte_address, value)       \
  XeTextureLoadStoreTexture(xe_texture_load_dest_tex,                    \
                            xe_texture_load_dest_params,                 \
                            load_info.host_pitch, uint(byte_address), (value))

#include XE_TEX_LOAD_SOURCE
