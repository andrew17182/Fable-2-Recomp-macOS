// FABLE2_EMU_DRAW_STATS=1 (diagnostic, default off; docs/native-translator.md,
// "Geometry and tessellation emulation"): per-frame census of the draws that
// go through geometry or tessellation emulation, logged as averages every
// FABLE2_EMU_DRAW_STATS_FRAMES frames (default 300) as `[emu-stats]` lines.
//
// Counts per kind (points / rectangles / quads; tessellation by mode) the
// draws, guest vertices or patches, and, for tessellation, the effective
// factors (after the +1 and the min/max clamp) and an estimate of the
// tessellated triangles (2 x inside segments, fractional-even rounding up).
// Adaptive factors are read from the guest index buffer on the CPU; the other
// modes use the uniform maximum. Per vertex shader totals list the top ones.
#ifndef REX_GRAPHICS_METAL_METAL_EMU_DRAW_STATS_H_
#define REX_GRAPHICS_METAL_METAL_EMU_DRAW_STATS_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "rex/logging.h"

namespace rex::graphics::metal::emu_stats {

enum Kind : uint32_t {
  kGeomPoint,
  kGeomRect,
  kGeomQuad,
  kTessDiscrete,
  kTessContinuous,
  kTessAdaptive,
  kKindCount,
};

inline const char* KindName(uint32_t k) {
  static const char* names[] = {"geom-point", "geom-rect", "geom-quad",
                                "tess-discrete", "tess-continuous",
                                "tess-adaptive"};
  return k < kKindCount ? names[k] : "?";
}

inline bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_EMU_DRAW_STATS");
    return v && v[0] == '1';
  }();
  return on;
}

inline uint32_t WindowFrames() {
  static const uint32_t n = [] {
    const char* v = std::getenv("FABLE2_EMU_DRAW_STATS_FRAMES");
    long x = v ? std::strtol(v, nullptr, 10) : 0;
    return uint32_t(x > 0 ? x : 300);
  }();
  return n;
}

// Segments per side for fractional-even partitioning (rounded up to even, at
// least 2), used only for the triangle estimate.
inline double EvenSegments(float f) {
  double s = std::ceil(std::max(1.0f, f) * 0.5) * 2.0;
  return std::max(2.0, s);
}

struct KindAcc {
  uint64_t draws = 0;
  uint64_t items = 0;  // guest vertices (geometry) or patches (tessellation)
  double tris = 0.0;   // tessellation estimate
  double factor_sum = 0.0;  // sum of effective edge factors
  uint64_t factor_n = 0;
  float factor_min = 1e30f;
  float factor_max = 0.0f;
  uint64_t hist[17] = {};  // effective edge factor, floor, 16 = >=16
};

struct VsAcc {
  uint64_t draws = 0;
  uint64_t items = 0;
  double tris = 0.0;
  uint32_t kind = 0;
};

class Stats {
 public:
  static Stats& Get() {
    static Stats s;
    return s;
  }

  // Call once per issued emulated draw.
  void NoteGeometry(uint64_t frame, uint32_t kind, uint64_t vs_hash,
                    uint32_t guest_vertices) {
    Frame(frame);
    KindAcc& a = acc_[kind];
    ++a.draws;
    a.items += guest_vertices;
    VsAcc& v = vs_[vs_hash];
    ++v.draws;
    v.items += guest_vertices;
    v.kind = kind;
  }

  // factors: 4 effective edge factors per patch (adaptive), or empty with
  // uniform_factor for the uniform modes.
  void NoteTessellation(uint64_t frame, uint32_t kind, uint64_t vs_hash,
                        uint32_t patches, const float* factors,
                        float uniform_factor) {
    Frame(frame);
    KindAcc& a = acc_[kind];
    VsAcc& v = vs_[vs_hash];
    ++a.draws;
    a.items += patches;
    ++v.draws;
    v.items += patches;
    v.kind = kind;
    double tris = 0.0;
    for (uint32_t p = 0; p < patches; ++p) {
      float e[4];
      for (uint32_t i = 0; i < 4; ++i) {
        e[i] = factors ? factors[p * 4 + i] : uniform_factor;
        a.factor_sum += e[i];
        ++a.factor_n;
        a.factor_min = std::min(a.factor_min, e[i]);
        a.factor_max = std::max(a.factor_max, e[i]);
        uint32_t b = uint32_t(std::min(16.0f, std::max(0.0f, e[i])));
        ++a.hist[b];
      }
      // Inside factors as the adaptive hull shader makes them.
      float iu = std::min(e[2], e[0]), iv = std::min(e[1], e[3]);
      tris += 2.0 * EvenSegments(iu) * EvenSegments(iv);
    }
    a.tris += tris;
    v.tris += tris;
  }

 private:
  void Frame(uint64_t frame) {
    if (frame == last_frame_) {
      return;
    }
    if (last_frame_ != 0) {
      ++frames_;
    }
    last_frame_ = frame;
    if (frames_ >= WindowFrames()) {
      Log();
      frames_ = 0;
      for (KindAcc& a : acc_) {
        a = KindAcc();
      }
      vs_.clear();
    }
  }

  void Log() {
    const double n = double(frames_);
    for (uint32_t k = 0; k < kKindCount; ++k) {
      const KindAcc& a = acc_[k];
      if (!a.draws) {
        continue;
      }
      if (k < kTessDiscrete) {
        REXLOG_INFO("[emu-stats] {} frames: {} {:.1f} draws/frame, {:.0f} vertices/frame",
                    frames_, KindName(k), a.draws / n, a.items / n);
        continue;
      }
      std::string hist;
      for (uint32_t b = 0; b < 17; ++b) {
        if (a.hist[b]) {
          hist += fmt::format(" {}{}:{:.0f}%", b, b == 16 ? "+" : "",
                              100.0 * double(a.hist[b]) / double(a.factor_n));
        }
      }
      REXLOG_INFO(
          "[emu-stats] {} frames: {} {:.1f} draws/frame, {:.0f} patches/frame, "
          "~{:.0f} tris/frame; edge factor mean {:.2f} min {:.2f} max {:.2f};{}",
          frames_, KindName(k), a.draws / n, a.items / n, a.tris / n,
          a.factor_n ? a.factor_sum / double(a.factor_n) : 0.0,
          a.factor_n ? a.factor_min : 0.0f, a.factor_max, hist);
    }
    std::vector<std::pair<uint64_t, VsAcc>> top(vs_.begin(), vs_.end());
    std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) {
      return x.second.tris + x.second.items > y.second.tris + y.second.items;
    });
    for (size_t i = 0; i < top.size() && i < 8; ++i) {
      const VsAcc& v = top[i].second;
      REXLOG_INFO(
          "[emu-stats]   VS {:016X} {}: {:.1f} draws/frame, {:.0f} items/frame, "
          "~{:.0f} tris/frame",
          top[i].first, KindName(v.kind), v.draws / n, v.items / n, v.tris / n);
    }
  }

  uint64_t last_frame_ = 0;
  uint32_t frames_ = 0;
  KindAcc acc_[kKindCount];
  std::map<uint64_t, VsAcc> vs_;
};

}  // namespace rex::graphics::metal::emu_stats

#endif  // REX_GRAPHICS_METAL_METAL_EMU_DRAW_STATS_H_
