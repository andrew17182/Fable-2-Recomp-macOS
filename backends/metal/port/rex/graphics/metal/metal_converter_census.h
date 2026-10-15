// Converter census (always on; docs/no-converter.md): counts every use of the
// DXBC -> DXIL (dxilconv) -> Metal Shader Converter (MSC) chain at run time,
// per purpose, and every draw whose pipeline still holds a converter-built
// stage. Logged as `[converter-use]` lines: the first use per purpose (with
// the shader hash), then a summary at most every 10 s while anything changes.
//
// "Uses" are counted at three levels:
//   dxil  = a DXBC -> DXIL request (cached = served by the artifact store,
//           run = dxilconv actually ran);
//   msc   = a DXIL -> metallib request (run = MSC actually ran; the rest came
//           from the stage compile cache, memory or disk);
//   lib   = a converter metallib turned into an MTL::Library (what the GPU
//           eventually runs, cached or not).
// A converter-free run has all three at 0 and no converter draws.
//
// FABLE2_NO_CONVERTER builds (no dxilconv / MSC linked): every converter
// request fails, counted as `refused`, and the first one per purpose is
// logged at error level with the shader hash.
#ifndef REX_GRAPHICS_METAL_METAL_CONVERTER_CENSUS_H_
#define REX_GRAPHICS_METAL_METAL_CONVERTER_CENSUS_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "rex/logging.h"

namespace rex::graphics::metal::converter_census {

enum Purpose : uint32_t {
  kGuestVS,     // guest vertex shader, standard pipelines
  kGuestPS,     // guest pixel shader
  kDepthOnlyPS, // depth-only fragment for no-PS emulation draws
  kGeomVS,      // geometry emulation: guest VS as the object stage
  kGeomGS,      // geometry emulation: generated geometry shader
  kTessVS,      // tessellation emulation: fixed control-point VS
  kTessHS,      // tessellation emulation: fixed hull shader
  kTessDS,      // tessellation emulation: guest VS as the domain shader
  kOther,
  kPurposeCount,
};

inline const char* PurposeName(uint32_t p) {
  static const char* names[] = {"guest-vs", "guest-ps", "depth-only-ps",
                                "geom-vs",  "geom-gs",  "tess-vs",
                                "tess-hs",  "tess-ds",  "other"};
  return p < kPurposeCount ? names[p] : "?";
}

enum Draw : uint32_t {
  kDrawStdNative,      // standard pipeline, every stage native
  kDrawStdConvVS,      // standard pipeline, converter vertex function
  kDrawStdConvPS,      // standard pipeline, converter pixel function
  kDrawNativeGeom,     // native points / rectangles
  kDrawNativeGeomConvPS,
  kDrawNativeTess,     // native tessellation
  kDrawNativeTessConvPS,
  kDrawGeomEmu,        // MSC geometry emulation
  kDrawTessEmu,        // MSC tessellation emulation
  kDrawCount,
};

struct Counters {
  std::array<std::atomic<uint64_t>, kPurposeCount> dxil_cached{};
  std::array<std::atomic<uint64_t>, kPurposeCount> dxil_run{};
  std::array<std::atomic<uint64_t>, kPurposeCount> msc_requests{};
  std::array<std::atomic<uint64_t>, kPurposeCount> msc_run{};
  std::array<std::atomic<uint64_t>, kPurposeCount> libs{};
  std::array<std::atomic<uint64_t>, kPurposeCount> refused{};
  std::array<std::atomic<bool>, kPurposeCount> first_logged{};
  std::array<std::atomic<uint64_t>, kDrawCount> draws{};
  std::atomic<uint64_t> emu_pipelines{0};
  std::atomic<uint64_t> changes{0};
};

inline Counters& Get() {
  static Counters c;
  return c;
}

// The purpose of the converter work on this thread (set by the callers that
// know it; the converter entry points read it).
inline uint32_t& CurrentPurpose() {
  static thread_local uint32_t p = kOther;
  return p;
}
inline uint64_t& CurrentHash() {
  static thread_local uint64_t h = 0;
  return h;
}

struct Scope {
  uint32_t saved_p;
  uint64_t saved_h;
  Scope(uint32_t p, uint64_t hash) : saved_p(CurrentPurpose()), saved_h(CurrentHash()) {
    CurrentPurpose() = p;
    CurrentHash() = hash;
  }
  ~Scope() {
    CurrentPurpose() = saved_p;
    CurrentHash() = saved_h;
  }
};

inline void NoteFirst(const char* what) {
  const uint32_t p = CurrentPurpose();
  Counters& c = Get();
  c.changes.fetch_add(1, std::memory_order_relaxed);
  if (!c.first_logged[p].exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[converter-use] first {} use: {} (shader {:016X})",
                PurposeName(p), what, CurrentHash());
  }
}

inline void Dxil(bool cached) {
  Counters& c = Get();
  (cached ? c.dxil_cached : c.dxil_run)[CurrentPurpose()].fetch_add(
      1, std::memory_order_relaxed);
  NoteFirst(cached ? "dxil (cached)" : "dxilconv");
}
inline void MscRequest() {
  Get().msc_requests[CurrentPurpose()].fetch_add(1, std::memory_order_relaxed);
  NoteFirst("msc request");
}
inline void MscRun() {
  Get().msc_run[CurrentPurpose()].fetch_add(1, std::memory_order_relaxed);
}
inline void Library(uint32_t purpose) {
  Counters& c = Get();
  c.libs[purpose].fetch_add(1, std::memory_order_relaxed);
  c.changes.fetch_add(1, std::memory_order_relaxed);
}
inline void Refused() {
  const uint32_t p = CurrentPurpose();
  Counters& c = Get();
  const uint64_t n = c.refused[p].fetch_add(1, std::memory_order_relaxed) + 1;
  c.changes.fetch_add(1, std::memory_order_relaxed);
  if (n == 1 || (n & (n - 1)) == 0) {
    REXLOG_ERROR(
        "[converter-use] NO-CONVERTER BUILD: {} shader {:016X} needs the "
        "converter (refused #{}); this draw path has no native route",
        PurposeName(p), CurrentHash(), n);
  }
}
inline void CountDraw(uint32_t kind) {
  Get().draws[kind].fetch_add(1, std::memory_order_relaxed);
}

// Called once per frame (command-processor thread).
inline void MaybeLog(bool force = false) {
  using clock = std::chrono::steady_clock;
  static clock::time_point last = clock::now();
  static uint64_t last_changes = 0;
  static std::array<uint64_t, kDrawCount> last_draws{};
  Counters& c = Get();
  const auto now = clock::now();
  if (!force && now - last < std::chrono::seconds(10)) return;
  const uint64_t changes = c.changes.load(std::memory_order_relaxed);
  std::array<uint64_t, kDrawCount> d;
  bool conv_draws = false;
  for (uint32_t i = 0; i < kDrawCount; ++i) {
    d[i] = c.draws[i].load(std::memory_order_relaxed);
  }
  const uint64_t window_conv =
      (d[kDrawStdConvVS] - last_draws[kDrawStdConvVS]) +
      (d[kDrawStdConvPS] - last_draws[kDrawStdConvPS]) +
      (d[kDrawNativeGeomConvPS] - last_draws[kDrawNativeGeomConvPS]) +
      (d[kDrawNativeTessConvPS] - last_draws[kDrawNativeTessConvPS]) +
      (d[kDrawGeomEmu] - last_draws[kDrawGeomEmu]) +
      (d[kDrawTessEmu] - last_draws[kDrawTessEmu]);
  conv_draws = window_conv != 0;
  // Always one line once draws have happened, so a converter-free run says
  // so ("totals: none;").
  static bool logged_once = false;
  uint64_t all_draws = 0;
  for (uint32_t i = 0; i < kDrawCount; ++i) all_draws += d[i];
  if (!force && changes == last_changes && !conv_draws &&
      (logged_once || !all_draws)) {
    last = now;
    return;
  }
  last = now;
  last_changes = changes;
  logged_once = true;
  std::string per;
  for (uint32_t p = 0; p < kPurposeCount; ++p) {
    const uint64_t dc = c.dxil_cached[p].load(std::memory_order_relaxed);
    const uint64_t dr = c.dxil_run[p].load(std::memory_order_relaxed);
    const uint64_t mq = c.msc_requests[p].load(std::memory_order_relaxed);
    const uint64_t mr = c.msc_run[p].load(std::memory_order_relaxed);
    const uint64_t lb = c.libs[p].load(std::memory_order_relaxed);
    const uint64_t rf = c.refused[p].load(std::memory_order_relaxed);
    if (!(dc | dr | mq | mr | lb | rf)) continue;
    per += " ";
    per += PurposeName(p);
    per += "=dxil " + std::to_string(dc + dr) + "(run " + std::to_string(dr) +
           ") msc " + std::to_string(mq) + "(run " + std::to_string(mr) +
           ") lib " + std::to_string(lb);
    if (rf) per += " REFUSED " + std::to_string(rf);
    per += ";";
  }
  REXLOG_INFO(
      "[converter-use] totals:{} draws: std native {} conv-vs {} conv-ps {} | "
      "native geom {} (conv ps {}) native tess {} (conv ps {}) | geom emu {} "
      "tess emu {} | converter draws in last window {}",
      per.empty() ? " none;" : per, d[kDrawStdNative], d[kDrawStdConvVS],
      d[kDrawStdConvPS], d[kDrawNativeGeom], d[kDrawNativeGeomConvPS],
      d[kDrawNativeTess], d[kDrawNativeTessConvPS], d[kDrawGeomEmu],
      d[kDrawTessEmu], window_conv);
  last_draws = d;
}

}  // namespace rex::graphics::metal::converter_census

#endif  // REX_GRAPHICS_METAL_METAL_CONVERTER_CENSUS_H_
