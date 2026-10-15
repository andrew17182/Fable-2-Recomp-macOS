/**
 * [resolve-to-texture] Resolution-scaled copy resolves and the cached scaled
 * textures that sample their output (docs/resolution-scale.md §16).
 *
 * At scale > 1 a copy resolve writes the scaled-resolve buffer, and every
 * texture over that range is invalidated and later re-loaded (untiled) from
 * the buffer. This file classifies, per resolve, the cached scaled textures
 * whose base level the resolve writes:
 *   1 geometry and layout match, load is a plain copy (8..128 bpb)
 *   2 geometry and layout match, load is the depth conversion
 *   3 geometry and layout match, other load conversion
 *   4 match, but the texture was already outdated and the resolve does not
 *     cover all of it (its other texels still need a load)
 *   5 no match (pitch, bits per block, base, tiling, dimension, mips)
 */

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dispatch/dispatch.h>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <mutex>
#include <thread>
#include <set>
#include <unordered_map>
#include <pthread.h>
#include <pthread/qos.h>

#include "rex/graphics/metal/metal_hitch.h"
#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/graphics/metal/metal_texture_cache.h"
#include "rex/logging.h"
#include "rex/math.h"
#include "rex/graphics/pipeline/texture/util.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_32bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_32bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_32bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_64bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_64bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_64bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_depth_32bpp_1xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_depth_32bpp_2xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_depth_32bpp_4xmsaa_scaled_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_32bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_32bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_32bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_64bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_64bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_uint_64bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_8bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_32bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_64bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_uint_16bpp_4xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_depth_32bpp_1xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_depth_32bpp_2xmsaa_rtt_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_depth_32bpp_4xmsaa_rtt_cs.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {
uint64_t RttNowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
// texture_address::Tiled2D (XeniOS src/xenia/gpu/texture_address.h).
int32_t RttTiled2D(int32_t x, int32_t y, uint32_t pitch_aligned,
                   uint32_t bpb_log2) {
  const int32_t outer_blocks =
      ((y >> 5) * int32_t(pitch_aligned >> 5) + (x >> 5)) << 6;
  const int32_t inner_blocks = (((y >> 1) & 0b111) << 3) | (x & 0b111);
  const int32_t oib = (outer_blocks | inner_blocks) << bpb_log2;
  const uint32_t bank = (y >> 4) & 0b1;
  const uint32_t pipe = ((x >> 3) & 0b11) ^ (((y >> 3) & 0b1) << 1);
  const uint32_t y_lsb = uint32_t(y) & 1;
  return int32_t((y_lsb << 4) | (pipe << 6) | (bank << 11)) | (oib & 0b1111) |
         (((oib >> 4) & 0b1) << 5) | (((oib >> 5) & 0b111) << 8) |
         (oib >> 8 << 12);
}

// The texel origin (bx, by) of an adjusted resolve base inside a texture:
// Tiled2D(bx, by) == delta, with bx / by multiples of the base granularity.
bool RttFindBaseOrigin(uint32_t delta, uint32_t pitch_aligned,
                       uint32_t height_blocks, uint32_t bpb_log2,
                       uint32_t& bx_out, uint32_t& by_out) {
  if (!delta) {
    bx_out = by_out = 0;
    return true;
  }
  const uint32_t gx = 1u << (7 - std::min(2u, bpb_log2));
  const uint32_t gy = gx;
  for (uint32_t by = 0; by < height_blocks; by += gy) {
    for (uint32_t bx = 0; bx < pitch_aligned; bx += gx) {
      if (uint32_t(RttTiled2D(int32_t(bx), int32_t(by), pitch_aligned,
                              bpb_log2)) == delta) {
        bx_out = bx;
        by_out = by;
        return true;
      }
    }
  }
  return false;
}
uint32_t RttTraceBase() {
  static const uint32_t b = [] {
    const char* v = std::getenv("FABLE2_RTT_TRACE_BASE");
    return v ? uint32_t(std::strtoul(v, nullptr, 0)) : 0u;
  }();
  return b;
}
uint32_t& RttTraceCount() {
  static uint32_t n = 0;
  return n;
}
void RttTraceGlobalWatch(const global_unique_lock_type&, void*,
                         uint32_t address_first, uint32_t address_last,
                         bool invalidated_by_gpu) {
  uint32_t b = RttTraceBase();
  static uint32_t last_rsv = 0;
  uint32_t rsv = UploadTrace().rtt_rsv;
  bool from_resolve = invalidated_by_gpu && rsv != last_rsv;
  last_rsv = rsv;
  (void)from_resolve;
  if (address_first <= b + 0x3FFFFF && address_last >= b &&
      RttTraceCount()++ < 4000) {
    REXLOG_INFO("[rtt-trace] watch {:08X}-{:08X} gpu {} rsv# {}",
                address_first, address_last, invalidated_by_gpu ? 1 : 0,
                UploadTrace().rtt_rsv);
  }
}
}  // namespace

uint32_t MetalTextureCache::RttDiagMode() {
  static const uint32_t mode = [] {
    const char* v = std::getenv("FABLE2_RTT_DIAG");
    uint32_t m = 0;
    if (v && (v[0] == '1' || std::string(v) == "census")) m = 1;
    if (v && (v[0] == '2' || std::string(v) == "skip")) m = 2;
    if (m == 1) {
      REXLOG_WARN("[rtt] FABLE2_RTT_DIAG=census (trace only)");
    } else if (m == 2) {
      REXLOG_WARN(
          "[rtt] FABLE2_RTT_DIAG=skip: matching textures are NOT reloaded "
          "after a resolve (image wrong, measurement only)");
    }
    return m;
  }();
  return mode;
}

namespace {
bool RttEnvFlag(const char* name, bool default_value) {
  const char* v = std::getenv(name);
  if (!v || !v[0]) return default_value;
  return v[0] != '0';
}
}  // namespace

bool MetalTextureCache::RttEnabled() {
  static const bool on = [] {
    bool e = RttEnvFlag("FABLE2_RESOLVE_TO_TEXTURE", true);
    REXLOG_INFO("[rtt] resolve-to-texture {} (FABLE2_RESOLVE_TO_TEXTURE)",
                e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalTextureCache::RttKeepEnabled() {
  static const bool on =
      RttEnabled() && RttEnvFlag("FABLE2_RTT_KEEP", true) && RttDiagMode() != 2;
  return on;
}

bool MetalTextureCache::RttFusedEnabled() {
  static const bool on =
      RttEnabled() && RttEnvFlag("FABLE2_RTT_FUSED", true) && RttDiagMode() != 2;
  return on;
}

bool MetalTextureCache::RttMipsEnabled() {
  // [tex-resolve] fused writes into whole stored mip levels, and mips kept
  // valid across them (FABLE2_RTT_MIPS=0: base level only, as before).
  static const bool on = [] {
    const bool e = RttFusedEnabled() && RttKeepEnabled() &&
                   RttEnvFlag("FABLE2_RTT_MIPS", true);
    REXLOG_INFO("[rtt] mip-level targets {} (FABLE2_RTT_MIPS)",
                e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalTextureCache::Rtt16Enabled() {
  // [tex-resolve] fused writes into 16 bpb textures (plain, 1_5_5_5 and
  // 5_6_5 loads; FABLE2_RTT_16BPP=0: not fused, as before).
  static const bool on = [] {
    const bool e = RttFusedEnabled() && RttEnvFlag("FABLE2_RTT_16BPP", true);
    REXLOG_INFO("[rtt] 16 bpb targets {} (FABLE2_RTT_16BPP)", e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalTextureCache::Rtt1xEnabled() {
  // [rtt-1x] keep / fused writes for unscaled textures at 1x.
  static const bool on = [] {
    const bool e = RttEnabled() && RttEnvFlag("FABLE2_RTT_1X", true);
    REXLOG_INFO("[rtt] 1x resolve-to-texture {} (FABLE2_RTT_1X)",
                e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalTextureCache::Rtt1xFusedEnabled() {
  // [rtt-1x] unscaled host resolves also write the matching cached textures.
  static const bool on = [] {
    const bool e = Rtt1xEnabled() && RttDiagMode() != 2 &&
                   RttEnvFlag("FABLE2_RTT_1X_FUSED", true);
    REXLOG_INFO("[rtt] 1x fused resolve writes {} (FABLE2_RTT_1X_FUSED)",
                e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalTextureCache::Rtt1xNarrowEnabled() {
  // [rtt-1x] a resolve invalidates only the textures over its exact extent,
  // not every texture on its 16 KB pages.
  static const bool on = [] {
    const bool e = Rtt1xEnabled() && RttEnvFlag("FABLE2_RTT_1X_KEEP", true);
    REXLOG_INFO("[rtt] 1x keep across unrelated invalidations {} "
                "(FABLE2_RTT_1X_KEEP)",
                e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalTextureCache::RttKeepActive() const {
  if (IsDrawResolutionScaled()) return RttKeepEnabled();
  // [rtt-1x] a load is skipped while the texture is valid: kept across
  // unrelated invalidations (FABLE2_RTT_1X_KEEP) or written by a fused
  // resolve (FABLE2_RTT_1X_FUSED). The upper bound (DIAG=skip) keeps too.
  static const bool on =
      Rtt1xEnabled() && (Rtt1xNarrowEnabled() || Rtt1xFusedEnabled());
  return on;
}

bool MetalTextureCache::RttMipsKeepActive() const {
  if (IsDrawResolutionScaled()) return RttMipsEnabled();
  static const bool on = RttEnvFlag("FABLE2_RTT_MIPS", true);
  return on && RttKeepActive();
}

bool MetalTextureCache::RttVerify() {
  static const bool on = [] {
    bool e = RttEnvFlag("FABLE2_RTT_VERIFY", false);
    if (e) {
      REXLOG_WARN("[rtt] FABLE2_RTT_VERIFY=1: every kept or fused texture is "
                  "compared with a reference load (diagnostic)");
    }
    return e;
  }();
  return on;
}

void MetalTextureCache::RttGlobalWatchThunk(
    const global_unique_lock_type& global_lock, void* context,
    uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu) {
  static_cast<MetalTextureCache*>(context)->RttOnInvalidate(
      address_first, address_last, invalidated_by_gpu);
}

void MetalTextureCache::RttNoteFusedUnused(MetalTexture& t) {
  if (!t.rtt_fused_pending_) return;
  t.rtt_fused_pending_ = false;
  UploadTrace().rtt_waste_kb += uint32_t(
      (uint64_t(t.GetGuestBaseSize()) * draw_resolution_scale_x() *
       draw_resolution_scale_y()) >>
      10);
}

static uint32_t NxTraceBase() {
  static const uint32_t b = [] {
    const char* v = std::getenv("FABLE2_TEX_NX_TRACE");
    return v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0u;
  }();
  return b;
}

void MetalTextureCache::RttOnInvalidate(uint32_t address_first,
                                        uint32_t address_last,
                                        bool invalidated_by_gpu) {
  if (NxTraceBase() && address_first <= NxTraceBase() + 0xFFFF &&
      address_last >= (NxTraceBase() & ~0xFFFFu)) {
    REXLOG_INFO("[nx-trace] inval {:08X}-{:08X} gpu {} rsv {} rsv-range "
                "{:08X}-{:08X}",
                address_first, address_last, invalidated_by_gpu,
                rtt_in_resolve_, rtt_resolve_first_, rtt_resolve_last_);
  }
  // Called under the global lock, before the per-range watches. A GPU write
  // outside a resolve is a memexport (shared memory only): the scaled buffer,
  // which scaled textures load from, is unchanged. Anything else (resolve,
  // CPU write, whole-memory invalidation) may change what a load returns.
  // A resolve changes the scaled buffer only inside its exact destination
  // extent; the watch range is rounded out to host pages.
  // Rounded out to 256 bytes, a multiple of every scaled-addressing group
  // (at most 128 guest bytes), so whole groups are covered.
  // [tex-unchanged] only the resolve's own (GPU) invalidation is narrowed: a
  // CPU write fault on another thread can take the global lock while a
  // resolve is being marked (rtt_in_resolve_ is set outside the lock), and
  // narrowing it to the resolve's extent would drop it
  // (FABLE2_RTT_NARROW_CPU_FIX=0 restores the old condition).
  static const bool cpu_fix = RttEnvFlag("FABLE2_RTT_NARROW_CPU_FIX", true);
  if (rtt_in_resolve_ && (invalidated_by_gpu || !cpu_fix) &&
      (IsDrawResolutionScaled() || Rtt1xNarrowEnabled())) {
    static const uint32_t margin = [] {
      const char* v = std::getenv("FABLE2_RTT_RESOLVE_MARGIN");
      return v ? uint32_t(std::strtoul(v, nullptr, 0)) : 0u;
    }();
    const uint32_t lo = (rtt_resolve_first_ & ~255u);
    address_first = std::max(address_first, lo > margin ? lo - margin : 0u);
    address_last = std::min(address_last, (rtt_resolve_last_ | 255u) + margin);
    if (address_first > address_last) return;
  }
  // [tex-rename nx-diag] unscaled textures load from shared memory: any
  // write over their exact range (memexport and CPU included) changes them.
  for (MetalTexture* t : nx_textures_) {
    if (!t->nx_valid_) continue;
    const uint32_t b = uint32_t(t->key().base_page) << 12;
    const uint32_t bs = t->GetGuestBaseSize();
    const uint32_t m = uint32_t(t->key().mip_page) << 12;
    const uint32_t ms = t->GetGuestMipsSize();
    if ((bs && b <= address_last && b + bs - 1 >= address_first) ||
        (ms && m <= address_last && m + ms - 1 >= address_first)) {
      t->nx_valid_ = false;
    }
  }
  // [rtt-1x] at 1x textures load from shared memory, which a memexport
  // does change: only the scaled path may ignore it.
  const bool keep = invalidated_by_gpu && !rtt_in_resolve_ &&
                    IsDrawResolutionScaled() &&
                    (address_last - address_first) < (256u << 20);
  if (RttDiagMode() == 1) {
    rtt_census_sigs_[fmt::format("inval gpu{} rsv{} keep{}",
                                 invalidated_by_gpu ? 1 : 0,
                                 rtt_in_resolve_ ? 1 : 0, keep ? 1 : 0)]++;
  }
  if (keep) return;
  {
    UploadTraceCounters& u = UploadTrace();
    ++u.rtt_inv;
    u.rtt_scan += uint32_t(rtt_textures_.size());
    u.rtt_trk = uint32_t(rtt_textures_.size());
  }
  for (MetalTexture* t : rtt_textures_) {
    if (t->rtt_mips_valid_) {
      // [tex-resolve] mips: cleared like the base, except by the fused
      // resolve's own write inside the one level it rewrote completely.
      const uint32_t ms = uint32_t(t->key().mip_page) << 12;
      const uint32_t msize = t->GetGuestMipsSize();
      if (msize && ms <= address_last && ms + msize - 1 >= address_first) {
        bool exempt = false;
        if (rtt_in_resolve_ && invalidated_by_gpu) {
          for (const RttMipExempt& e : rtt_mip_exempt_) {
            exempt = exempt || (e.texture == t && address_first >= e.start &&
                                address_last < e.end);
          }
        }
        if (!exempt) t->rtt_mips_valid_ = false;
      }
    }
    if (!t->rtt_valid_) continue;
    const uint32_t base = uint32_t(t->key().base_page) << 12;
    const uint32_t size = t->GetGuestBaseSize();
    if (size && base <= address_last && base + size - 1 >= address_first) {
      if (RttTraceBase() == base && RttTraceCount()++ < 4000) {
        REXLOG_INFO("[rtt-trace] invalid {:08X}-{:08X} gpu {} inrsv {}",
                    address_first, address_last, invalidated_by_gpu,
                    rtt_in_resolve_);
      }
      t->rtt_valid_ = false;
      RttNoteFusedUnused(*t);
    }
  }
}


namespace {
struct RttFusedLibrary {
  const char* name;
  const uint8_t* data;
  size_t size;
};
const RttFusedLibrary kRttFusedLibraries[] = {
    {"resolve_host_color_32bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_32bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_32bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_32bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_32bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_32bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_32bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_32bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_32bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_64bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_64bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_64bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_64bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_64bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_64bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_64bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_64bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_64bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_8bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_full_8bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_8bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_8bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_full_8bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_8bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_8bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_full_8bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_8bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_32bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_full_32bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_32bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_32bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_full_32bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_32bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_32bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_full_32bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_32bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_64bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_full_64bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_64bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_64bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_full_64bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_64bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_64bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_full_64bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_64bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_uint_32bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_uint_32bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_uint_32bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_uint_32bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_uint_32bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_uint_32bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_uint_32bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_uint_32bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_uint_32bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_uint_64bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_uint_64bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_uint_64bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_uint_64bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_uint_64bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_uint_64bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_uint_64bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_uint_64bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_uint_64bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_8bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_8bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_8bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_8bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_8bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_8bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_8bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_8bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_8bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_32bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_32bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_32bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_32bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_32bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_32bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_32bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_32bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_32bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_64bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_64bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_64bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_64bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_64bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_64bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_64bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_64bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_64bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_16bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_full_16bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_16bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_16bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_full_16bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_16bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_16bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_full_16bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_16bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_16bpp_1xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_16bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_16bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_16bpp_2xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_16bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_16bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_16bpp_4xmsaa_scaled_rtt_cs", resolve_host_color_full_uint_16bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_16bpp_4xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_depth_32bpp_1xmsaa_scaled_rtt_cs", resolve_host_depth_32bpp_1xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_depth_32bpp_1xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_depth_32bpp_2xmsaa_scaled_rtt_cs", resolve_host_depth_32bpp_2xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_depth_32bpp_2xmsaa_scaled_rtt_cs_metallib)},
    {"resolve_host_depth_32bpp_4xmsaa_scaled_rtt_cs", resolve_host_depth_32bpp_4xmsaa_scaled_rtt_cs_metallib, sizeof(resolve_host_depth_32bpp_4xmsaa_scaled_rtt_cs_metallib)},
    // [rtt-1x] unscaled (shared-memory destination) variants.
    {"resolve_host_color_32bpp_1xmsaa_rtt_cs", resolve_host_color_32bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_32bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_32bpp_2xmsaa_rtt_cs", resolve_host_color_32bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_32bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_32bpp_4xmsaa_rtt_cs", resolve_host_color_32bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_32bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_64bpp_1xmsaa_rtt_cs", resolve_host_color_64bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_64bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_64bpp_2xmsaa_rtt_cs", resolve_host_color_64bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_64bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_64bpp_4xmsaa_rtt_cs", resolve_host_color_64bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_64bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_8bpp_1xmsaa_rtt_cs", resolve_host_color_full_8bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_8bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_8bpp_2xmsaa_rtt_cs", resolve_host_color_full_8bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_8bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_8bpp_4xmsaa_rtt_cs", resolve_host_color_full_8bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_8bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_32bpp_1xmsaa_rtt_cs", resolve_host_color_full_32bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_32bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_32bpp_2xmsaa_rtt_cs", resolve_host_color_full_32bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_32bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_32bpp_4xmsaa_rtt_cs", resolve_host_color_full_32bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_32bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_64bpp_1xmsaa_rtt_cs", resolve_host_color_full_64bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_64bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_64bpp_2xmsaa_rtt_cs", resolve_host_color_full_64bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_64bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_64bpp_4xmsaa_rtt_cs", resolve_host_color_full_64bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_64bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_uint_32bpp_1xmsaa_rtt_cs", resolve_host_color_uint_32bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_uint_32bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_uint_32bpp_2xmsaa_rtt_cs", resolve_host_color_uint_32bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_uint_32bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_uint_32bpp_4xmsaa_rtt_cs", resolve_host_color_uint_32bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_uint_32bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_uint_64bpp_1xmsaa_rtt_cs", resolve_host_color_uint_64bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_uint_64bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_uint_64bpp_2xmsaa_rtt_cs", resolve_host_color_uint_64bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_uint_64bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_uint_64bpp_4xmsaa_rtt_cs", resolve_host_color_uint_64bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_uint_64bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_8bpp_1xmsaa_rtt_cs", resolve_host_color_full_uint_8bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_8bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_8bpp_2xmsaa_rtt_cs", resolve_host_color_full_uint_8bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_8bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_8bpp_4xmsaa_rtt_cs", resolve_host_color_full_uint_8bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_8bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_32bpp_1xmsaa_rtt_cs", resolve_host_color_full_uint_32bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_32bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_32bpp_2xmsaa_rtt_cs", resolve_host_color_full_uint_32bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_32bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_32bpp_4xmsaa_rtt_cs", resolve_host_color_full_uint_32bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_32bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_64bpp_1xmsaa_rtt_cs", resolve_host_color_full_uint_64bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_64bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_64bpp_2xmsaa_rtt_cs", resolve_host_color_full_uint_64bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_64bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_64bpp_4xmsaa_rtt_cs", resolve_host_color_full_uint_64bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_64bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_16bpp_1xmsaa_rtt_cs", resolve_host_color_full_16bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_16bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_16bpp_2xmsaa_rtt_cs", resolve_host_color_full_16bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_16bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_16bpp_4xmsaa_rtt_cs", resolve_host_color_full_16bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_16bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_16bpp_1xmsaa_rtt_cs", resolve_host_color_full_uint_16bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_16bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_16bpp_2xmsaa_rtt_cs", resolve_host_color_full_uint_16bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_16bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_color_full_uint_16bpp_4xmsaa_rtt_cs", resolve_host_color_full_uint_16bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_color_full_uint_16bpp_4xmsaa_rtt_cs_metallib)},
    {"resolve_host_depth_32bpp_1xmsaa_rtt_cs", resolve_host_depth_32bpp_1xmsaa_rtt_cs_metallib, sizeof(resolve_host_depth_32bpp_1xmsaa_rtt_cs_metallib)},
    {"resolve_host_depth_32bpp_2xmsaa_rtt_cs", resolve_host_depth_32bpp_2xmsaa_rtt_cs_metallib, sizeof(resolve_host_depth_32bpp_2xmsaa_rtt_cs_metallib)},
    {"resolve_host_depth_32bpp_4xmsaa_rtt_cs", resolve_host_depth_32bpp_4xmsaa_rtt_cs_metallib, sizeof(resolve_host_depth_32bpp_4xmsaa_rtt_cs_metallib)},
};

MTL::ComputePipelineState* BuildRttFusedPipeline(MTL::Device* device,
                                                 const RttFusedLibrary& lib,
                                                 NS::Error** error) {
  dispatch_data_t data = dispatch_data_create(
      lib.data, lib.size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* library = device->newLibrary(data, error);
  dispatch_release(data);
  MTL::Function* fn =
      library ? library->newFunction(
                    NS::String::string("entry_xe", NS::UTF8StringEncoding))
              : nullptr;
  MTL::ComputePipelineState* fused = nullptr;
  if (fn) {
    MTL::ComputePipelineDescriptor* desc =
        MTL::ComputePipelineDescriptor::alloc()->init();
    desc->setComputeFunction(fn);
    desc->setLabel(NS::String::string(lib.name, NS::UTF8StringEncoding));
    fused = device->newComputePipelineState(
        desc, MTL::PipelineOptionNone,
        static_cast<MTL::AutoreleasedComputePipelineReflection*>(nullptr),
        error);
    desc->release();
    fn->release();
  }
  if (library) library->release();
  return fused;
}
}  // namespace

// [rtt-prewarm] On a first launch (empty Metal system cache) each fused
// resolve pipeline compiled on the draw path cost 50-310 ms (`pso_rt` in
// [hitch] lines, load screens and early gameplay). They are a fixed set of
// the backend's own shaders, so two utility threads build them at startup;
// the draw path takes the prebuilt one by name, or builds it as before if it
// is not ready yet. Same function, same label, same descriptor.
struct MetalTextureCache::RttPrewarm {
  std::mutex mutex;
  std::unordered_map<std::string, MTL::ComputePipelineState*> built;
  std::vector<const RttFusedLibrary*> todo;
  size_t next = 0;
  bool stop = false;
  uint32_t taken = 0, done = 0;
  double build_ms = 0.0;
  std::chrono::steady_clock::time_point start;
  std::vector<std::thread> threads;
};

void MetalTextureCache::StartRttPrewarm() {
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_RTT_PREWARM");
    return !(v && v[0] == '0');
  }();
  if (!enabled || rtt_prewarm_) return;
  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) return;
  auto state = std::make_shared<RttPrewarm>();
  state->start = std::chrono::steady_clock::now();
  const bool scaled = IsDrawResolutionScaled();
  for (const RttFusedLibrary& lib : kRttFusedLibraries) {
    // Scaled variants only when drawing scaled; unscaled ones always ([rtt-1x],
    // and unscaled resolves while scaling with [effects-native]).
    if (!scaled && std::strstr(lib.name, "_scaled_")) continue;
    state->todo.push_back(&lib);
  }
  rtt_prewarm_ = state;
  for (int t = 0; t < 2; ++t) {
    state->threads.emplace_back([state, device]() {
      pthread_setname_np("RTT Prewarm");
      pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
      for (;;) {
        const RttFusedLibrary* lib;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (state->stop || state->next >= state->todo.size()) break;
          lib = state->todo[state->next++];
        }
        const auto t0 = std::chrono::steady_clock::now();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        NS::Error* error = nullptr;
        MTL::ComputePipelineState* fused =
            BuildRttFusedPipeline(device, *lib, &error);
        pool->release();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        std::lock_guard<std::mutex> lock(state->mutex);
        state->build_ms += ms;
        if (fused) state->built.emplace(lib->name, fused);
        if (++state->done == state->todo.size()) {
          REXLOG_INFO(
              "[rtt-prewarm] {} fused resolve pipelines built in background "
              "({:.0f} ms summed, done {:.2f} s after start), {} already "
              "built by draws",
              state->todo.size(), state->build_ms,
              std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            state->start)
                  .count(),
              state->taken);
        }
      }
    });
  }
  REXLOG_INFO("[rtt-prewarm] building {} fused resolve pipelines on 2 "
              "background threads (FABLE2_RTT_PREWARM)",
              state->todo.size());
}

uint32_t MetalTextureCache::RttPrewarmPending() {
  // [shader-prep] fused resolve pipelines not built yet.
  RttPrewarm* state = rtt_prewarm_.get();
  if (!state) return 0;
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->stop ? 0 : uint32_t(state->todo.size() - state->done);
}

MTL::ComputePipelineState* MetalTextureCache::RttFusedPipeline(
    MTL::ComputePipelineState* pipeline) {
  if (!pipeline) return nullptr;
  auto it = rtt_fused_pipelines_.find(pipeline);
  if (it != rtt_fused_pipelines_.end()) return it->second;
  MTL::ComputePipelineState* fused = nullptr;
  std::string label =
      pipeline->label() ? pipeline->label()->utf8String() : std::string();
  // [rtt-1x] "<x>_scaled_cs" -> "<x>_scaled_rtt_cs", "<x>_cs" -> "<x>_rtt_cs".
  const std::string suffix = "_cs";
  if (label.size() > suffix.size() &&
      label.compare(label.size() - suffix.size(), suffix.size(), suffix) == 0) {
    std::string name =
        label.substr(0, label.size() - suffix.size()) + "_rtt_cs";
    for (const RttFusedLibrary& lib : kRttFusedLibraries) {
      if (name != lib.name) continue;
      if (rtt_prewarm_) {
        std::lock_guard<std::mutex> lock(rtt_prewarm_->mutex);
        auto it = rtt_prewarm_->built.find(name);
        if (it != rtt_prewarm_->built.end()) {
          fused = it->second;
          rtt_prewarm_->built.erase(it);
          break;
        }
        ++rtt_prewarm_->taken;
      }
      hitch::Scope hitch_scope(hitch::kPsoRt);
      MTL::Device* device = command_processor_->GetMetalDevice();
      NS::Error* error = nullptr;
      const auto build_start = std::chrono::steady_clock::now();
      fused = BuildRttFusedPipeline(device, lib, &error);
      static uint32_t logged = 0;
      if (logged < 32) {
        ++logged;
        REXLOG_INFO("[rtt-prewarm] draw-path build {:.1f} ms: {}",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - build_start)
                        .count(),
                    lib.name);
      }
      if (!fused) {
        REXLOG_WARN("[rtt] no fused pipeline {}: {}", lib.name,
                    error ? error->localizedDescription()->utf8String()
                          : "unknown");
      }
      break;
    }
  }
  rtt_fused_pipelines_[pipeline] = fused;
  return fused;
}

void MetalTextureCache::RttShutdown() {
  if (std::shared_ptr<RttPrewarm> state = std::move(rtt_prewarm_)) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->stop = true;
    }
    for (auto& t : state->threads) {
      if (t.joinable()) t.join();
    }
    for (auto& e : state->built) {
      if (e.second) e.second->release();
    }
    state->built.clear();
  }
  for (auto& e : rtt_fused_pipelines_) {
    if (e.second) e.second->release();
  }
  rtt_fused_pipelines_.clear();
  if (rtt_global_watch_) {
    shared_memory().UnregisterGlobalWatch(rtt_global_watch_);
    rtt_global_watch_ = nullptr;
  }
}

bool MetalTextureCache::NxKeepEnabled() {
  // [tex-unchanged] FABLE2_TEX_UNCHANGED (docs/tex-rename-20261013.md): at
  // scale > 1, unscaled textures (loaded from shared memory) keep their
  // contents when no write touched their exact guest ranges since their last
  // load; the reload after a page-wide invalidation is skipped.
  static const bool on = [] {
    const bool e = RttEnvFlag("FABLE2_TEX_UNCHANGED", false);
    REXLOG_INFO("[tex-unchanged] {} (FABLE2_TEX_UNCHANGED)", e ? "on" : "off");
    return e;
  }();
  return on;
}

bool MetalTextureCache::NxDiagAllEnabled() {
  // FABLE2_TEX_NX_DIAG=1: track every candidate texture (counts only).
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TEX_NX_DIAG");
    const bool diag = v && v[0] == '1';
    if (diag) REXLOG_WARN("[tex-rename] FABLE2_TEX_NX_DIAG=1 (diagnostic)");
    return diag;
  }();
  return on;
}

void MetalTextureCache::NxTrack(MetalTexture& texture) {
  // Unscaled, tiled, 2D, one layer, at scale > 1 (loads from shared memory).
  const TextureKey& k = texture.key();
  if (texture.nx_tracked_ || !IsDrawResolutionScaled() || k.scaled_resolve ||
      !k.tiled || k.dimension != xenos::DataDimension::k2DOrStacked ||
      k.GetDepthOrArraySize() != 1) {
    return;
  }
  auto global_lock = AcquireGlobalLock();
  texture.nx_tracked_ = true;
  texture.nx_valid_ = false;  // valid from its next full load
  if (!rtt_global_watch_) {
    rtt_global_watch_ =
        shared_memory().RegisterGlobalWatch(RttGlobalWatchThunk, this);
  }
  nx_textures_.push_back(&texture);
}

bool MetalTextureCache::Rtt1xCandidateKey(const TextureKey& k) const {
  // [rtt-1x] resolve outputs are uncompressed, tiled, 2D, one layer.
  if (k.scaled_resolve || IsDrawResolutionScaled() || !Rtt1xEnabled()) {
    return false;
  }
  const FormatInfo* fi = FormatInfo::Get(k.format);
  return k.tiled && k.dimension == xenos::DataDimension::k2DOrStacked &&
         k.GetDepthOrArraySize() == 1 && fi && fi->block_width == 1 &&
         fi->block_height == 1;
}

void MetalTextureCache::RttRegister(MetalTexture* texture) {
  if (!texture) return;
  const TextureKey& k = texture->key();
  if (NxDiagAllEnabled()) NxTrack(*texture);
  if (!k.scaled_resolve && !Rtt1xCandidateKey(k)) return;
  auto global_lock = AcquireGlobalLock();
  texture->rtt_tracked_ = true;
  if (!rtt_global_watch_ && RttEnabled()) {
    rtt_global_watch_ =
        shared_memory().RegisterGlobalWatch(RttGlobalWatchThunk, this);
  }
  rtt_textures_.push_back(texture);
}

void MetalTextureCache::RttUnregister(MetalTexture* texture) {
  if (texture && texture->nx_tracked_) {
    auto global_lock = AcquireGlobalLock();
    auto it = std::find(nx_textures_.begin(), nx_textures_.end(), texture);
    if (it != nx_textures_.end()) nx_textures_.erase(it);
  }
  if (!texture || !texture->rtt_tracked_) return;
  auto global_lock = AcquireGlobalLock();
  for (size_t i = 0; i < rtt_textures_.size(); ++i) {
    if (rtt_textures_[i] == texture) {
      rtt_textures_[i] = rtt_textures_.back();
      rtt_textures_.pop_back();
      return;
    }
  }
}

void MetalTextureCache::RttCensusLoad(MetalTexture& texture,
                                      uint64_t scaled_bytes) {
  uint32_t c = texture.rtt_class_ < 6 ? texture.rtt_class_ : 0;
  if (RttTraceBase() &&
      (uint32_t(texture.key().base_page) << 12) == RttTraceBase() &&
      RttTraceCount()++ < 4000) {
    REXLOG_INFO("[rtt-trace] load tex {} class {} frame-ish {}",
                static_cast<void*>(&texture), c, UploadTrace().rtt_rsv);
  }
  UploadTraceCounters& u = UploadTrace();
  ++u.rtt_ld[c];
  u.rtt_ld_kb[c] += uint32_t(scaled_bytes >> 10);
  if (RttDiagMode() == 1) {
    const TextureKey& k = texture.key();
    rtt_census_sigs_[fmt::format("load c{} fmt{} {}x{} p{} e{} mips{} t{}", c,
                                 uint32_t(k.format), k.GetWidth(),
                                 k.GetHeight(), uint32_t(k.pitch),
                                 uint32_t(k.endianness),
                                 uint32_t(k.mip_max_level),
                                 uint32_t(k.tiled))]++;
  }
  if (RttDiagMode() == 1 && c == 0 && scaled_bytes > (1u << 20)) {
    static uint32_t logged = 0;
    if (logged < 60) {
      ++logged;
      const TextureKey& k = texture.key();
      uint64_t raw[2] = {};
      std::memcpy(raw, &k, std::min(sizeof(raw), sizeof(k)));
      REXLOG_INFO("[rtt-c0] tex {} key {:016X}{:016X} fmt{} {}x{} base {:08X} "
                  "mip {:08X} created_new {}",
                  static_cast<void*>(&texture), raw[1], raw[0],
                  uint32_t(k.format), k.GetWidth(), k.GetHeight(),
                  uint32_t(k.base_page) << 12, uint32_t(k.mip_page) << 12,
                  texture.loaded_once() ? 0 : 1);
    }
  }
  texture.rtt_class_ = 0;
}

void MetalTextureCache::RttTraceLoadResult(Texture& texture, bool load_base,
                                           bool load_mips, bool ok) {
  if (RttTraceBase() &&
      (uint32_t(texture.key().base_page) << 12) == RttTraceBase() &&
      RttTraceCount()++ < 4000) {
    REXLOG_INFO("[rtt-trace] load result base {} mips {} ok {} outdated {} "
                "precommit {} rsv# {}",
                load_base, load_mips, ok, texture.base_outdated_lockless(),
                precommit_target_cb_ ? 1 : 0, UploadTrace().rtt_rsv);
  }
}

bool MetalTextureCache::RttTryKeep(MetalTexture& texture, bool load_base,
                                   bool load_mips) {
  if ((!load_base && !load_mips) || !texture.rtt_tracked_ ||
      (texture.key().scaled_resolve != IsDrawResolutionScaled()) ||
      !RttKeepActive()) {
    return false;
  }
  // [tex-resolve] with FABLE2_RTT_MIPS the stored mip levels are kept too
  // while rtt_mips_valid_ (a mips-only load is skipped as a whole).
  const bool mips_on = RttMipsKeepActive();
  if (!load_base && !mips_on) return false;
  bool keep_base = false, keep_mips = false;
  {
    auto global_lock = AcquireGlobalLock();
    if (RttDiagMode() == 1) {
      rtt_census_sigs_[fmt::format("trykeep valid{} mips{}/{}",
                                   texture.rtt_valid_, load_mips,
                                   texture.rtt_mips_valid_)]++;
    }
    if (RttTraceBase() == (uint32_t(texture.key().base_page) << 12) &&
        RttTraceCount()++ < 4000) {
      REXLOG_INFO("[rtt-trace] trykeep valid {} mips {}", texture.rtt_valid_,
                  texture.rtt_mips_valid_);
    }
    keep_base = load_base && texture.rtt_valid_;
    keep_mips = load_mips && mips_on && texture.rtt_mips_valid_;
    if (load_base ? !keep_base : !keep_mips) return false;
  }
  UploadTraceCounters& u = UploadTrace();
  const uint64_t kb = (uint64_t(texture.GetGuestBaseSize()) *
                       draw_resolution_scale_x() * draw_resolution_scale_y()) >>
                      10;
  if (RttVerify()) {
    // Reference load of the kept levels compared with the kept content; the
    // texture itself is not written.
    rtt_verify_only_ = true;
    bool ok = TryGpuLoadTexture(texture, keep_base, keep_mips);
    rtt_verify_only_ = false;
    if (!ok) return false;
  }
  if (keep_base) {
    ++u.rtt_keep;
    u.rtt_keep_kb += uint32_t(kb);
    texture.rtt_fused_pending_ = false;
  }
  if (keep_mips) ++u.rtt_mip_keep;
  if (load_mips && !keep_mips) {
    if (mips_on) {
      auto global_lock = AcquireGlobalLock();
      texture.rtt_mips_valid_ = true;
    }
    if (!TryGpuLoadTexture(texture, false, true)) {
      if (mips_on) {
        auto global_lock = AcquireGlobalLock();
        texture.rtt_mips_valid_ = false;
      }
      return false;
    }
  }
  return true;
}

bool MetalTextureCache::LazyCensusEnabled() {
  static const bool on = [] {
    const bool e = RttEnvFlag("FABLE2_LAZY_CENSUS", false);
    if (e) REXLOG_WARN("[tex-resolve] FABLE2_LAZY_CENSUS=1 (trace only)");
    return e;
  }();
  return on;
}

namespace {
// Removes [s, e) from the map; returns the removed byte count.
uint64_t LazyCensusSubtract(std::map<uint32_t, uint32_t>& m, uint32_t s,
                            uint32_t e) {
  uint64_t removed = 0;
  auto it = m.upper_bound(s);
  if (it != m.begin()) --it;
  while (it != m.end() && it->first < e) {
    const uint32_t rs = it->first, re = it->second;
    if (re <= s) {
      ++it;
      continue;
    }
    it = m.erase(it);
    const uint32_t os = std::max(rs, s), oe = std::min(re, e);
    removed += oe - os;
    if (rs < s) m[rs] = s;
    if (re > e) m[e] = re;
  }
  return removed;
}
}  // namespace

void MetalTextureCache::LazyCensusWrite(uint32_t start, uint32_t length,
                                        bool candidate) {
  if (!LazyCensusEnabled() || !length) return;
  const uint64_t area =
      uint64_t(draw_resolution_scale_x()) * draw_resolution_scale_y();
  start &= 0x1FFFFFFF;
  const uint32_t end = start + length;
  UploadTraceCounters& u = UploadTrace();
  u.lz_dead_kb += uint32_t(
      (LazyCensusSubtract(lazy_census_ranges_, start, end) * area) >> 10);
  u.lz_all_kb += uint32_t((uint64_t(length) * area) >> 10);
  if (candidate) {
    lazy_census_ranges_[start] = end;
    u.lz_cand_kb += uint32_t((uint64_t(length) * area) >> 10);
  }
}

void MetalTextureCache::LazyCensusRead(uint32_t start, uint32_t length,
                                       bool morph) {
  if (!LazyCensusEnabled() || !length) return;
  const uint64_t area =
      uint64_t(draw_resolution_scale_x()) * draw_resolution_scale_y();
  start &= 0x1FFFFFFF;
  const uint32_t kb = uint32_t(
      (LazyCensusSubtract(lazy_census_ranges_, start, start + length) * area) >>
      10);
  UploadTraceCounters& u = UploadTrace();
  (morph ? u.lz_morph_kb : u.lz_read_kb) += kb;
}

void MetalTextureCache::RttCensusTick() {
  if (RttDiagMode() != 1) return;
  uint64_t now = RttNowUs();
  if (!rtt_census_last_log_us_) {
    rtt_census_last_log_us_ = now;
    return;
  }
  if (now - rtt_census_last_log_us_ < 10000000) return;
  rtt_census_last_log_us_ = now;
  std::vector<std::pair<uint64_t, std::string>> sorted;
  sorted.reserve(rtt_census_sigs_.size());
  for (auto& e : rtt_census_sigs_) sorted.emplace_back(e.second, e.first);
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  REXLOG_INFO("[rtt-census] {} signatures (10 s window)", sorted.size());
  size_t n = 0;
  for (auto& e : sorted) {
    if (++n > 60) break;
    REXLOG_INFO("[rtt-census] {:7} {}", e.first, e.second);
  }
  rtt_census_sigs_.clear();
}

namespace {
struct RttResolveGeometry {
  uint32_t dest_base, dest_bpb, pitch32, off_x, off_y, rect_w, rect_h;
  uint32_t w_start;
  uint64_t w_end;
  xenos::TextureFormat dest_format;
  bool dest_array;
};
RttResolveGeometry RttGetGeometry(const draw_util::ResolveInfo& ri,
                                  uint32_t written_address,
                                  uint32_t written_length) {
  RttResolveGeometry g;
  g.dest_base = ri.copy_dest_base & 0x1FFFFFFF;
  const auto& dci = ri.copy_dest_coordinate_info;
  g.dest_format = xenos::TextureFormat(ri.copy_dest_info.copy_dest_format);
  const FormatInfo* dest_info = FormatInfo::Get(g.dest_format);
  g.dest_bpb = dest_info ? dest_info->bytes_per_block() : 0;
  g.pitch32 = dci.pitch_aligned_div_32;
  g.off_x = uint32_t(dci.offset_x_div_8) << 3;
  g.off_y = uint32_t(dci.offset_y_div_8) << 3;
  g.rect_w = uint32_t(ri.coordinate_info.width_div_8) << 3;
  g.rect_h = ri.height_div_8 << 3;
  g.w_start = written_address & 0x1FFFFFFF;
  g.w_end = uint64_t(g.w_start) + written_length;
  g.dest_array = ri.copy_dest_info.copy_dest_array != 0;
  return g;
}
}  // namespace

// Classifies the cached scaled textures whose base or mips bytes the resolve
// writes (exact bytes; RttOnInvalidate narrows the page-wide watches to the
// same range). Caller holds the global lock.
void MetalTextureCache::RttCollect(const draw_util::ResolveInfo& resolve_info,
                                   uint32_t written_address,
                                   uint32_t written_length,
                                   std::vector<RttCandidate>& out) {
  out.clear();
  if (!written_length || (!IsDrawResolutionScaled() && !Rtt1xEnabled())) {
    return;
  }
  // [rtt-1x] the 1x upper bound also skips whole mip-level resolves.
  const bool mips_classify =
      RttMipsEnabled() || (!IsDrawResolutionScaled() && RttDiagMode() == 2);
  const RttResolveGeometry g =
      RttGetGeometry(resolve_info, written_address, written_length);
  for (MetalTexture* t : rtt_textures_) {
    const TextureKey& k = t->key();
    const uint32_t base = uint32_t(k.base_page) << 12;
    const uint32_t base_size = t->GetGuestBaseSize();
    bool mips_hit = false;
    if (k.mip_max_level && t->GetGuestMipsSize()) {
      uint32_t ms = uint32_t(k.mip_page) << 12;
      uint64_t me = uint64_t(ms) + t->GetGuestMipsSize();
      mips_hit = ms < g.w_end && me > g.w_start;
    }
    bool base_hit =
        base_size && base < g.w_end && uint64_t(base) + base_size > g.w_start;
    if (!base_hit && !mips_hit) continue;
    const FormatInfo* fi = FormatInfo::Get(k.format);
    uint32_t bx = 0, by = 0;
    bool geometry =
        base_hit && !mips_hit && k.tiled &&
        k.dimension == xenos::DataDimension::k2DOrStacked &&
        k.GetDepthOrArraySize() == 1 && !t->force_load_3d_tiling() &&
        !g.dest_array && fi && fi->bytes_per_block() == g.dest_bpb &&
        fi->block_width == 1 && fi->block_height == 1 &&
        uint32_t(k.pitch) == g.pitch32 && g.dest_base >= base &&
        g.w_start >= base && g.w_end <= uint64_t(base) + base_size &&
        RttFindBaseOrigin(g.dest_base - base, uint32_t(k.pitch) << 5,
                          (k.GetHeight() + 31) & ~31u,
                          rex::log2_floor(g.dest_bpb), bx, by);
    RttCandidate c;
    c.texture = t;
    c.cls = 5;
    c.bx = bx;
    c.by = by;
    if (!base_hit && mips_hit && mips_classify && k.tiled &&
        k.dimension == xenos::DataDimension::k2DOrStacked &&
        k.GetDepthOrArraySize() == 1 && !t->force_load_3d_tiling() &&
        !g.dest_array && fi && fi->bytes_per_block() == g.dest_bpb &&
        fi->block_width == 1 && fi->block_height == 1 && g.off_x == 0 &&
        g.off_y == 0) {
      // [tex-resolve] one whole stored mip level, origin (0, 0), same pitch:
      // the resolve's tiled addresses are the level's (same Tiled2D).
      const texture_util::TextureGuestLayout& gl = t->guest_layout();
      const uint32_t mips_base = uint32_t(k.mip_page) << 12;
      for (uint32_t level = 1;
           level <= gl.max_level && level < gl.packed_level &&
           level < xenos::kTextureMaxMips;
           ++level) {
        const texture_util::TextureGuestLayout::Level& ll = gl.mips[level];
        const uint32_t ls = mips_base + gl.mip_offsets_bytes[level];
        const uint64_t le = uint64_t(ls) + ll.level_data_extent_bytes;
        if (g.dest_base != ls || !ll.level_data_extent_bytes) continue;
        const uint32_t lw = std::max(k.GetWidth() >> level, 1u);
        const uint32_t lh = std::max(k.GetHeight() >> level, 1u);
        if (ll.row_pitch_bytes != g.pitch32 * 32u * g.dest_bpb ||
            g.rect_w < lw || g.rect_h < lh || g.w_start < ls ||
            (g.w_start & ~255u) < ls || g.w_end > le ||
            ((g.w_end - 1) | 255u) >= le) {
          break;
        }
        LoadShaderIndex ls_index = GetLoadShaderIndexForKey(k);
        const bool supported =
            ls_index == kLoadShaderIndex8bpb ||
            ls_index == kLoadShaderIndex32bpb ||
            ls_index == kLoadShaderIndex64bpb ||
            (Rtt16Enabled() &&
             (ls_index == kLoadShaderIndex16bpb ||
              ls_index == kLoadShaderIndexR5G5B5A1ToB5G5R5A1 ||
              ls_index == kLoadShaderIndexR5G6B5ToB5G6R5));
        if (supported) {
          c.cls = kRttClassMipLevel;
          t->miplod_candidate_ = true;
          c.full = true;
          c.level = level;
          c.level_start = ls;
          c.level_end = uint32_t(le);
          c.load_shader = uint32_t(ls_index);
        }
        break;
      }
      out.push_back(c);
      continue;
    }
    if (geometry) {
      LoadShaderIndex ls = GetLoadShaderIndexForKey(k);
      bool identity = ls == kLoadShaderIndex8bpb ||
                      ls == kLoadShaderIndex16bpb ||
                      ls == kLoadShaderIndex32bpb ||
                      ls == kLoadShaderIndex64bpb ||
                      ls == kLoadShaderIndex128bpb;
      bool depth = ls == kLoadShaderIndexDepthUnorm ||
                   ls == kLoadShaderIndexDepthFloat;
      c.full = bx + g.off_x == 0 && by + g.off_y == 0 &&
               g.rect_w >= k.GetWidth() && g.rect_h >= k.GetHeight();
      if (!t->rtt_valid_ && !c.full) {
        c.cls = 4;
      } else {
        c.cls = identity ? 1 : (depth ? 2 : 3);
      }
      c.load_shader = uint32_t(ls);
    }
    out.push_back(c);
  }
}

bool MetalTextureCache::RttPrepareFused(
    const draw_util::ResolveInfo& resolve_info, RttFusedPlan& plan) {
  plan = RttFusedPlan();
  if (!(IsDrawResolutionScaled() ? RttFusedEnabled() : Rtt1xFusedEnabled()) ||
      rtt_textures_.empty() || !resolve_info.copy_dest_extent_length) {
    return false;
  }
  const RttResolveGeometry g =
      RttGetGeometry(resolve_info, resolve_info.copy_dest_extent_start,
                     resolve_info.copy_dest_extent_length);
  static thread_local std::vector<RttCandidate> candidates;
  {
    auto global_lock = AcquireGlobalLock();
    RttCollect(resolve_info, resolve_info.copy_dest_extent_start,
               resolve_info.copy_dest_extent_length, candidates);
  }
  const uint32_t sx = draw_resolution_scale_x(), sy = draw_resolution_scale_y();
  for (const RttCandidate& c : candidates) {
    if (plan.params.count >= 2) break;
    if (c.cls != 1 && c.cls != 2 && c.cls != 3 && c.cls != kRttClassMipLevel) {
      continue;
    }
    MetalTexture* t = c.texture;
    // A presented (swap) texture is fine too: the write lands in the
    // current command buffer, where its load would have run.
    if (!t->metal_texture()) continue;
    uint32_t texel_bytes = 0, transform = 0;
    switch (LoadShaderIndex(c.load_shader)) {
      case kLoadShaderIndex8bpb: texel_bytes = 1; break;
      case kLoadShaderIndex16bpb:
        if (Rtt16Enabled()) texel_bytes = 2;
        break;
      case kLoadShaderIndexR5G5B5A1ToB5G5R5A1:
        if (Rtt16Enabled()) { texel_bytes = 2; transform = 3; }
        break;
      case kLoadShaderIndexR5G6B5ToB5G6R5:
        if (Rtt16Enabled()) { texel_bytes = 2; transform = 4; }
        break;
      case kLoadShaderIndex32bpb: texel_bytes = 4; break;
      case kLoadShaderIndex64bpb: texel_bytes = 8; break;
      case kLoadShaderIndexDepthFloat: texel_bytes = 4; transform = 1; break;
      case kLoadShaderIndexDepthUnorm: texel_bytes = 4; transform = 2; break;
      default: break;
    }
    // Census (FABLE2_RTT_DIAG=census): why a fused candidate is dropped.
    auto reject = [&](const char* why) {
      if (RttDiagMode() == 1) {
        auto global_lock = AcquireGlobalLock();
        rtt_census_sigs_[fmt::format("fused-reject c{} ls{} {}x{} {}", c.cls,
                                     c.load_shader, t->key().GetWidth(),
                                     t->key().GetHeight(), why)]++;
      }
    };
    if (!texel_bytes ||
        GetLoadShaderInfo(LoadShaderIndex(c.load_shader))
                .bytes_per_host_block != texel_bytes) {
      reject(texel_bytes ? "host-block" : "load-shader");
      continue;
    }
    MTL::Texture* view = t->GetOrCreateDirectWriteView(texel_bytes);
    if (!view) {
      reject("no-view");
      continue;
    }
    const TextureKey& k = t->key();
    RttTargetParams& tp = plan.params.t[plan.params.count];
    tp.origin_x = int32_t(c.bx * sx);
    // Negative control for the verify mode only (shifts the fused write).
    static const bool rtt_break = RttEnvFlag("FABLE2_RTT_VERIFY_BREAK", false);
    if (rtt_break) tp.origin_x += 1;
    tp.origin_y = int32_t(c.by * sy);
    tp.width = std::max(k.GetWidth() >> c.level, 1u) * sx;
    tp.height = std::max(k.GetHeight() >> c.level, 1u) * sy;
    tp.texel_bytes = texel_bytes;
    tp.endian = uint32_t(k.endianness);
    tp.transform = transform;
    tp.level = c.level;
    plan.textures[plan.params.count] = t;
    plan.views[plan.params.count] = view;
    plan.level_start[plan.params.count] = c.level_start;
    plan.level_end[plan.params.count] = c.level_end;
    plan.kb += (uint64_t(c.level ? c.level_end - c.level_start
                                 : t->GetGuestBaseSize()) *
                sx * sy) >>
               10;
    ++plan.params.count;
  }
  if (!plan.params.count) return false;
  plan.params.dest_bpb_log2 = rex::log2_floor(g.dest_bpb);
  static const bool lazy_skip_diag = RttEnvFlag("FABLE2_LAZY_SKIP_DIAG", false);
  if (LazyCensusEnabled() || lazy_skip_diag) {
    // Target 0 covers every resolved pixel, and its load is invertible.
    const RttTargetParams& t0 = plan.params.t[0];
    const uint32_t lw = t0.width / sx, lh = t0.height / sy;
    const int32_t x0 = t0.origin_x / int32_t(sx) + int32_t(g.off_x);
    const int32_t y0 = t0.origin_y / int32_t(sy) + int32_t(g.off_y);
    plan.lazy_ok = (t0.transform == 0 || t0.transform >= 3) &&
                   t0.texel_bytes >= 2 && x0 >= 0 && y0 >= 0 &&
                   uint32_t(x0) + g.rect_w <= lw &&
                   uint32_t(y0) + g.rect_h <= lh;
    // DIAGNOSTIC ONLY, UNSAFE (upper bound): such resolves do not write the
    // scaled buffer at all; a later reader of the range sees stale bytes.
    if (lazy_skip_diag && plan.lazy_ok) plan.params.skip_buffer = 1;
  }
  if (plan.params.count == 1) {
    plan.views[1] = plan.views[0];
  }
  return true;
}

void MetalTextureCache::MarkRangeAsResolvedUnscaledRtt(
    uint32_t written_address, uint32_t written_length) {
  if (IsDrawResolutionScaled()) ++UploadTrace().rtt_rsv;
  rtt_in_resolve_ = true;
  rtt_resolve_first_ = written_address & 0x1FFFFFFF;
  rtt_resolve_last_ =
      rtt_resolve_first_ + (written_length ? written_length - 1 : 0);
  MarkRangeAsResolvedUnscaled(written_address, written_length);
  rtt_in_resolve_ = false;
}

void MetalTextureCache::MarkRangeAsResolvedRttFused(
    const draw_util::ResolveInfo& resolve_info, uint32_t written_address,
    uint32_t written_length, const RttFusedPlan* plan) {
  const uint32_t mode = RttDiagMode();
  if (RttTraceBase() && written_length &&
      (written_address & 0x1FFFFFFF) <= RttTraceBase() + 0x3FFFFF &&
      (written_address & 0x1FFFFFFF) + written_length > RttTraceBase() &&
      RttTraceCount()++ < 4000) {
    REXLOG_INFO("[rtt-trace] any-resolve {:08X}+{} dest_base {:08X} rsv# {}",
                written_address, written_length, resolve_info.copy_dest_base,
                UploadTrace().rtt_rsv);
  }
  static bool trace_watch = false;
  if (RttTraceBase() && !trace_watch) {
    trace_watch = true;
    shared_memory().RegisterGlobalWatch(RttTraceGlobalWatch, nullptr);
  }
  UploadTraceCounters& u = UploadTrace();
  const bool rtt_1x = !IsDrawResolutionScaled() && Rtt1xEnabled();
  if (IsDrawResolutionScaled() || rtt_1x) ++u.rtt_rsv;
  const RttResolveGeometry g =
      RttGetGeometry(resolve_info, written_address, written_length);
  if (IsDrawResolutionScaled()) {
    LazyCensusWrite(written_address, written_length, plan && plan->lazy_ok);
  }

  // The census needs the classes before the resolve invalidates.
  static thread_local std::vector<RttCandidate> candidates;
  candidates.clear();
  if (mode || RttTraceBase()) {
    auto global_lock = AcquireGlobalLock();
    RttCollect(resolve_info, written_address, written_length, candidates);
  }
  if (!candidates.empty()) ++u.rtt_rsv_hit;

  if (plan) {
    for (uint32_t i = 0; i < plan->params.count; ++i) {
      if (plan->params.t[i].level) {
        rtt_mip_exempt_[i].texture =
            static_cast<MetalTexture*>(plan->textures[i]);
        rtt_mip_exempt_[i].start = plan->level_start[i];
        rtt_mip_exempt_[i].end = plan->level_end[i];
      }
    }
  }
  rtt_in_resolve_ = true;
  rtt_resolve_first_ = g.w_start;
  rtt_resolve_last_ = written_length ? uint32_t(g.w_end - 1) : g.w_start;
  MarkRangeAsResolved(written_address, written_length);
  rtt_in_resolve_ = false;
  rtt_mip_exempt_[0] = RttMipExempt();
  rtt_mip_exempt_[1] = RttMipExempt();

  if (plan) {
    // The fused dispatch wrote these textures exactly as a load from the
    // buffer would (they were valid before, or fully covered): keep them
    // valid; their next load is skipped (or verified) by RttTryKeep.
    // [tex-resolve] a mip-level target keeps whatever mips validity it had:
    // the other levels' bytes did not change, this one is rewritten whole.
    auto global_lock = AcquireGlobalLock();
    for (uint32_t i = 0; i < plan->params.count; ++i) {
      if (plan->params.t[i].level) {
        ++u.rtt_mip_direct;
        continue;
      }
      MetalTexture* t = static_cast<MetalTexture*>(plan->textures[i]);
      t->rtt_valid_ = true;
      t->rtt_fused_pending_ = true;
    }
    ++u.rtt_direct;
    u.rtt_direct_kb += uint32_t(plan->kb);
  }

  for (const RttCandidate& c : candidates) {
    MetalTexture* t = c.texture;
    if (c.cls < 6) ++u.rtt_t[c.cls];
    t->rtt_class_ = c.cls;
    if (RttTraceBase() &&
        (uint32_t(t->key().base_page) << 12) == RttTraceBase() &&
        RttTraceCount()++ < 4000) {
      REXLOG_INFO("[rtt-trace] resolve {:08X}+{} tex {} class {} rsv# {}",
                  written_address, written_length, static_cast<void*>(t),
                  c.cls, u.rtt_rsv);
    }
    if (mode == 1) {
      const TextureKey& k = t->key();
      rtt_census_sigs_[fmt::format(
          "rsv c{} {} dfmt{} e{} p{} off{},{} at{},{} rect{}x{} -> fmt{} {}x{} "
          "e{} mips{} ls{} fused{}",
          c.cls, resolve_info.IsCopyingDepth() ? "depth" : "color",
          uint32_t(g.dest_format),
          uint32_t(resolve_info.copy_dest_info.copy_dest_endian), g.pitch32,
          g.off_x, g.off_y, c.bx, c.by, g.rect_w, g.rect_h,
          uint32_t(k.format), k.GetWidth(), k.GetHeight(),
          uint32_t(k.endianness), uint32_t(k.mip_max_level),
          int(GetLoadShaderIndexForKey(k)), plan ? 1 : 0)]++;
    }
    if (mode == 2 && rtt_1x && (c.cls == 3 || c.cls == kRttClassMipLevel)) {
      // [rtt-1x] upper bound: also other conversions and whole mip levels.
      auto global_lock = AcquireGlobalLock();
      if (c.cls == 3) {
        t->MakeLoadedDataUpToDateAndWatch(global_lock, true, false);
      } else if (c.full) {
        t->MakeLoadedDataUpToDateAndWatch(global_lock, false, true);
      }
      t->rtt_class_ = 0;
      ++u.rtt_direct;
    }
    if (mode == 2 && (c.cls == 1 || c.cls == 2)) {
      auto global_lock = AcquireGlobalLock();
      t->MakeLoadedDataUpToDateAndWatch(global_lock, true, false);
      t->rtt_class_ = 0;
      ++u.rtt_direct;
      u.rtt_direct_kb += uint32_t(
          (uint64_t(t->GetGuestBaseSize()) * draw_resolution_scale_x() *
           draw_resolution_scale_y()) >>
          10);
    }
  }
  if (mode == 1 && candidates.empty()) {
    rtt_census_sigs_[fmt::format(
        "rsv nohit {} dfmt{} p{} rect{}x{} len{}",
        resolve_info.IsCopyingDepth() ? "depth" : "color",
        uint32_t(g.dest_format), g.pitch32, g.rect_w, g.rect_h,
        written_length)]++;
  }
  RttCensusTick();
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex

namespace rex {
namespace graphics {
namespace metal {

void MetalTextureCache::MipLodNoteSample(const void* texture,
                                         int32_t max_read) {
  auto* t = static_cast<MetalTexture*>(
      static_cast<Texture*>(const_cast<void*>(texture)));
  if (!t || !t->miplod_candidate_) return;
  if (t->miplod_epoch_ != miplod_epoch_) {
    t->miplod_epoch_ = miplod_epoch_;
    t->miplod_max_read_ = -1;
    t->miplod_unbounded_ = false;
  }
  if (max_read < 0) {
    t->miplod_unbounded_ = true;
  } else {
    t->miplod_max_read_ = std::max(t->miplod_max_read_, max_read);
  }
}

void MetalTextureCache::MipLodMarkCandidate(const void* texture) {
  auto* t = static_cast<MetalTexture*>(
      static_cast<Texture*>(const_cast<void*>(texture)));
  if (t) t->miplod_candidate_ = true;
}

bool MetalTextureCache::MipLodIsCandidate(const void* texture) {
  const auto* t =
      static_cast<const MetalTexture*>(static_cast<const Texture*>(texture));
  return t && t->miplod_candidate_;
}

int MetalTextureCache::MipLodPassSafeForLevel(const void* texture,
                                              uint32_t level) const {
  const auto* t =
      static_cast<const MetalTexture*>(static_cast<const Texture*>(texture));
  if (!t || !t->miplod_candidate_) return 0;
  if (t->miplod_epoch_ != miplod_epoch_) return 1;
  if (t->miplod_unbounded_) return 0;
  return t->miplod_max_read_ < int32_t(level) ? 2 : 0;
}

bool MetalTextureCache::MipLodTraceEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_MIPLOD_TRACE");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  return on;
}

void MetalTextureCache::MipLodTraceNoteDraw(const Shader& pixel_shader,
                                            uint64_t vs_hash, uint32_t prim) {
  const RegisterFile& regs = register_file();
  for (const Shader::TextureBinding& b : pixel_shader.texture_bindings()) {
    const TextureBinding* tb = GetValidTextureBinding(b.fetch_constant);
    if (!tb || !tb->texture || !tb->key.mip_max_level) continue;
    MipLodTraceEntry& e = miplod_trace_[tb->texture];
    const uint32_t* f = &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                     b.fetch_constant * 6];
    std::memcpy(e.fetch, f, sizeof(e.fetch));
    e.ps_hash = pixel_shader.ucode_data_hash();
    e.vs_hash = vs_hash;
    e.prim = prim;
    e.fetch_constant = b.fetch_constant;
    const auto& a = b.fetch_instr.attributes;
    e.computed_lod = a.use_computed_lod;
    e.register_lod = a.use_register_lod;
    e.register_gradients = a.use_register_gradients;
    e.instr_lod_bias = a.lod_bias;
    e.instr_mip_filter = uint32_t(a.mip_filter);
  }
}

void MetalTextureCache::MipLodTraceHazard(const void* fused_texture,
                                          uint32_t level, const char* what) {
  static std::set<std::string> seen;
  static uint64_t total = 0;
  ++total;
  if (!fused_texture || seen.size() >= 256) return;
  const auto* t = static_cast<const Texture*>(fused_texture);
  const TextureKey& k = t->key();
  std::string line = fmt::format(
      "{} key base=0x{:08X} mip=0x{:08X} {}x{} fmt={} maxlvl={} packed={} "
      "write_level={}",
      what, uint32_t(k.base_page) << 12, uint32_t(k.mip_page) << 12,
      k.GetWidth(), k.GetHeight(), uint32_t(k.format), uint32_t(k.mip_max_level),
      uint32_t(k.packed_mips), level);
  auto it = miplod_trace_.find(fused_texture);
  if (it == miplod_trace_.end()) {
    line += " sampled-entry=none";
  } else {
    const MipLodTraceEntry& e = it->second;
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, e.fetch, sizeof(fetch));
    uint32_t smin = 0, smax = 0;
    texture_util::GetSubresourcesFromFetchConstant(
        fetch, nullptr, nullptr, nullptr, nullptr, nullptr, &smin, &smax);
    line += fmt::format(
        " vs={:016X} prim={} ps={:016X} tf{} fetch_mip={}-{} eff={}-{} base=0x{:08X} "
        "mipaddr=0x{:08X} filt(min/mag/mip)={}/{}/{} lod_bias={} "
        "instr(computed={} reglod={} reggrad={} bias={} mipf={})",
        e.vs_hash, e.prim, e.ps_hash, e.fetch_constant,
        uint32_t(fetch.mip_min_level),
        uint32_t(fetch.mip_max_level), smin, smax,
        uint32_t(fetch.base_address) << 12, uint32_t(fetch.mip_address) << 12,
        uint32_t(fetch.min_filter), uint32_t(fetch.mag_filter),
        uint32_t(fetch.mip_filter), int32_t(fetch.lod_bias),
        e.computed_lod ? 1 : 0, e.register_lod ? 1 : 0,
        e.register_gradients ? 1 : 0, e.instr_lod_bias, e.instr_mip_filter);
  }
  if (seen.insert(line).second) {
    REXLOG_INFO("[mip-lod] #{} {}", total, line);
  }
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
