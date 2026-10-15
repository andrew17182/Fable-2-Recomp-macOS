// [resolve-to-texture] Resolution-scaled host resolve (resolve_host_color /
// resolve_host_depth) that also writes its output into up to two cached
// scaled textures sampling the destination (docs/resolution-scale.md §16).
//
// Compiled by tools/compile_metal_helpers.py once per scaled host resolve
// variant, with XE_RTT_ENTRY naming the unmodified XeniOS entry file
// ("resolve_host_color_entry.xesli", ...) and the variant's usual defines.
// The resolve itself is unchanged: its scaled-resolve buffer store still
// happens with the same address and value. Only the store macros are wrapped:
// each N-byte store at address A also goes, as N / bytes-per-block texels, to
// the texture position of the same host pixels, after the transformation the
// matching texture_load_*_scaled shader applies to the same bytes (32-bit
// endian swap for 32/64 bpb, raw for 8 bpb; the depth conversions). So each
// texel receives what a load from the buffer would write.
//
// Store pattern of every host resolve variant: the first store of a thread
// is at XeResolveDestPixelAddress(pixel_index), the second (if any) at the
// local-X XOR, covering the next N / bpb pixels along X.

#ifndef XE_RTT_ENTRY
#error XE_RTT_ENTRY must name the resolve entry file
#endif

#include "endian.xesli"
#include "pixel_formats.xesli"
#include "resolve.xesli"

struct XeRttTarget {
  int origin_x;          // host texel X of destination host position 0
  int origin_y;
  uint width;            // host (scaled) texels of the written level
  uint height;
  uint texel_bytes;      // 1, 2, 4 or 8
  uint endian;           // xenos::Endian of the texture (load endian_32)
  uint transform;        // 0 copy, 1 depth float (20e4), 2 depth unorm24,
                         // 3 R5G5B5A1 -> B5G5R5A1, 4 R5G6B5 -> B5G6R5
  uint level;            // mip level written ([tex-resolve] mips targets)
};

struct XeRttParams {
  XeRttTarget t[2];
  uint count;
  uint dest_bpb_log2;
  uint skip_buffer;  // [tex-resolve] lazy scaled buffer: texture writes only
  uint pad1;
};

#define XE_RTT_TEX texture2d_array<uint, access::write>

static inline void XeRttWriteTarget(XE_RTT_TEX tex, constant XeRttTarget& t,
                                    uint2_xe host_position, uint4_xe value,
                                    uint texels) {
  uint4_xe words = value;
  if (t.texel_bytes == 2u) {
    // texture_load_16bpb (and its _r5g5b5a1 / _r5g6b5 variants): 16-bit
    // endian swap, then the packed-pair transform.
    words = XeEndianSwap16(words, t.endian);
    if (t.transform == 3u) {
      words = XeR5G5B5A1ToB5G5R5A1(words);
    } else if (t.transform == 4u) {
      words = XeR5G6B5ToB5G6R5(words);
    }
  } else if (t.texel_bytes >= 4u) {
    words = XeEndianSwap32(words, t.endian);
    if (t.transform == 1u) {
      words = XeFloat20e4To32(words >> 8u);
    } else if (t.transform == 2u) {
      words = float_bits_to_uint_xe(XeUNorm24To32(words >> 8u));
    }
  }
  const int y = int(host_position.y) + t.origin_y;
  if (y < 0 || uint(y) >= t.height) {
    return;
  }
  const int x0 = int(host_position.x) + t.origin_x;
  for (uint i = 0u; i < texels; ++i) {
    const int x = x0 + int(i);
    if (x < 0 || uint(x) >= t.width) {
      continue;
    }
    uint4_xe texel;
    if (t.texel_bytes == 8u) {
      texel = uint4_xe(words[2u * i], words[2u * i + 1u], 0u, 0u);
    } else if (t.texel_bytes == 4u) {
      texel = uint4_xe(words[i], 0u, 0u, 0u);
    } else if (t.texel_bytes == 2u) {
      texel = uint4_xe((words[i >> 1u] >> ((i & 1u) * 16u)) & 0xFFFFu, 0u, 0u,
                       0u);
    } else {
      texel = uint4_xe((words[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu, 0u, 0u, 0u);
    }
    tex.write(texel, uint2(uint(x), uint(y)), 0u, t.level);
  }
}

static inline void XeRttStore(device char* dest, uint address, uint4_xe value,
                              uint bytes, uint2_xe pixel_index,
                              XeResolveInfo resolve_info, XE_RTT_TEX t0,
                              XE_RTT_TEX t1, constant XeRttParams& p) {
  if (p.skip_buffer == 0u) {
    if (bytes == 16u) {
      *reinterpret_cast<device uint4_xe*>(&dest[address]) = value;
    } else if (bytes == 8u) {
      *reinterpret_cast<device uint2_xe*>(&dest[address]) = value.xy;
    } else {
      *reinterpret_cast<device uint*>(&dest[address]) = value.x;
    }
  }
  if (!p.count) {
    return;
  }
  const uint texels = bytes >> p.dest_bpb_log2;
  const uint first =
      XeResolveDestPixelAddress(resolve_info, pixel_index, p.dest_bpb_log2);
  uint2_xe host_position = pixel_index + resolve_info.dest_xy_offset_scaled;
  if (address != first) {
    host_position.x += texels;
  }
  XeRttWriteTarget(t0, p.t[0], host_position, value, texels);
  if (p.count > 1u) {
    XeRttWriteTarget(t1, p.t[1], host_position, value, texels);
  }
}

#undef byte_buffer_align4_store4_xe
#define byte_buffer_align4_store4_xe(name, byte_address, value)            \
  XeRttStore((name), uint(byte_address), uint4_xe((value), 0u, 0u, 0u), 4u, \
             pixel_index, resolve_info, xe_rtt_t0, xe_rtt_t1, xe_rtt_p)
#undef byte_buffer_align8_store8_xe
#define byte_buffer_align8_store8_xe(name, byte_address, value)            \
  XeRttStore((name), uint(byte_address), uint4_xe((value), 0u, 0u), 8u,     \
             pixel_index, resolve_info, xe_rtt_t0, xe_rtt_t1, xe_rtt_p)
#undef byte_buffer_align16_store16_xe
#define byte_buffer_align16_store16_xe(name, byte_address, value)          \
  XeRttStore((name), uint(byte_address), (value), 16u, pixel_index,         \
             resolve_info, xe_rtt_t0, xe_rtt_t1, xe_rtt_p)

// The destination buffer parameter carries the two textures and the params
// through the main function; the entry binds them next to it.
#undef param_byte_buffer_wo_xe
#define param_byte_buffer_wo_xe(name)                                       \
  device char* name, XE_RTT_TEX xe_rtt_t0, XE_RTT_TEX xe_rtt_t1,           \
      constant XeRttParams& xe_rtt_p
#undef pass_byte_buffer_xe
#define pass_byte_buffer_xe(name) (name), xe_rtt_t0, xe_rtt_t1, xe_rtt_p
#undef byte_buffer_wo_binding_xe
#define byte_buffer_wo_binding_xe(name, msl_buffer)                         \
  device char* name [[msl_buffer]],                                         \
      XE_RTT_TEX xe_rtt_t0 [[texture(4)]],                                  \
      XE_RTT_TEX xe_rtt_t1 [[texture(5)]],                                  \
      constant XeRttParams& xe_rtt_p [[buffer(6)]]

#include XE_RTT_ENTRY
