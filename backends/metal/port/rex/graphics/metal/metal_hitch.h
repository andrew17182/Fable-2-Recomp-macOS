#pragma once
// [hitch] Long-frame attribution on the GPU Commands thread (docs/frame-pacing-20261012.md).
// FABLE2_HITCH_TRACE (default on, =0 off): the command processor's swap-to-swap interval is split
// into the time spent in a few known blocking or expensive places (exclusive: a nested scope
// pauses its parent). When an interval is a hitch, one log line names where it went:
//   [hitch] f<frame> <interval> ms (cpu <GPU Commands CPU ms>, base <baseline ms>): idle .. slot .. pso_wait ..
// Hitch rule (relative, default): interval >= clamp(FABLE2_HITCH_RATIO (1.5) x median of the previous 32
// intervals, FABLE2_HITCH_MS (40), FABLE2_HITCH_CEIL_MS (100)). Steady GPU-bound frames (e.g. ~47 ms at 3x on
// an M3 Pro, the wait charged to slot/idle) are not hitches; a spike above the recent baseline is, and any
// frame of 100 ms or more always is. Fewer than 8 previous intervals: the floor alone.
// FABLE2_HITCH_RELATIVE=0: the old absolute rule (every interval >= FABLE2_HITCH_MS), line without "base".
// Categories:
//   idle      the command processor had no guest commands (guest-side: game, loading, streaming)
//   slot      waiting for a frame slot (GPU still busy with frame N-3)
//   sync      synchronous command-buffer waits (readbacks, flush-and-wait)
//   pso_wait  waiting for a background pipeline (startup warm-up / morph draws)
//   pso_emu   geometry/tessellation emulation pipelines and their stages, built on the draw path
//   pso_rt    EDRAM load / resolve / other compute pipelines built lazily on the draw path
//   pso_xfer  EDRAM ownership-transfer pipelines (MSL source per key + pipeline), on the draw path
//   pso_xclr  transfer clear pipelines (shared MSL library + pipeline), on the draw path
//   xlate     shader translation on the draw path (DXBC/DXIL/MSC, library install)
//   rt_grow   render-target shrink grows (new texture, flush, copy)
//   tex       texture requests (cache lookups, loads, uploads)
//   present   guest-output refresh (presenter)
//   other     the rest of the interval (draw processing, encoding)
// Cost: two mach_absolute_time reads per scope; scopes are on rare or per-draw-batch paths.
#include <mach/mach.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "rex/logging.h"

namespace rex::graphics::metal::hitch {

enum Cat : int {
  kIdle = 0,
  kSlot,
  kSync,
  kPsoWait,
  kPsoEmu,
  kPsoRt,
  kPsoXfer,
  kPsoXclr,
  kXlate,
  kRtGrow,
  kTex,
  kPresent,
  kCount
};

inline const char* CatName(int c) {
  static const char* const names[kCount] = {"idle",   "slot",   "sync",    "pso_wait",
                                            "pso_emu", "pso_rt", "pso_xfer", "pso_xclr", "xlate",  "rt_grow",
                                            "tex",    "present"};
  return names[c];
}

inline bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_HITCH_TRACE");
    return !(v && v[0] == '0');
  }();
  return on;
}

inline uint64_t ThresholdUs() {
  static const uint64_t us = [] {
    const char* v = std::getenv("FABLE2_HITCH_MS");
    const double ms = v ? std::atof(v) : 40.0;
    return uint64_t((ms > 0 ? ms : 40.0) * 1000.0);
  }();
  return us;
}

inline double EnvMs(const char* name, double fallback) {
  const char* v = std::getenv(name);
  const double ms = v ? std::atof(v) : fallback;
  return ms > 0 ? ms : fallback;
}

inline bool Relative() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_HITCH_RELATIVE");
    return !(v && v[0] == '0');
  }();
  return on;
}

inline double Ratio() {
  static const double r = EnvMs("FABLE2_HITCH_RATIO", 1.5);
  return r;
}

inline uint64_t CeilUs() {
  static const uint64_t us = uint64_t(EnvMs("FABLE2_HITCH_CEIL_MS", 100.0) * 1000.0);
  return us;
}

// Recent swap intervals (µs) for the relative rule.
struct Baseline {
  static constexpr int kSize = 32;
  static constexpr int kMin = 8;
  double ring[kSize] = {};
  int count = 0;
  int next = 0;
  void Push(double us) {
    ring[next] = us;
    next = (next + 1) % kSize;
    if (count < kSize) ++count;
  }
  // Median of the stored intervals; 0 when there are fewer than kMin.
  double Median() const {
    if (count < kMin) return 0.0;
    double v[kSize];
    std::copy(ring, ring + count, v);
    std::nth_element(v, v + count / 2, v + count);
    double m = v[count / 2];
    if (!(count & 1)) m = (m + *std::max_element(v, v + count / 2)) * 0.5;
    return m;
  }
};

inline double TicksToUs(uint64_t ticks) {
  static const double scale = [] {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return double(tb.numer) / double(tb.denom) / 1000.0;
  }();
  return double(ticks) * scale;
}

struct ThreadState {
  uint64_t ticks[kCount] = {};
  uint32_t counts[kCount] = {};
  int stack[16] = {};
  uint64_t started[16] = {};
  int depth = 0;
};

inline ThreadState& State() {
  thread_local ThreadState state;
  return state;
}

inline void Begin(int cat) {
  if (!Enabled()) return;
  ThreadState& s = State();
  const uint64_t now = mach_absolute_time();
  if (s.depth > 0 && s.depth <= 16) {
    // Pause the parent: charge it up to now.
    const int parent = s.stack[s.depth - 1];
    s.ticks[parent] += now - s.started[s.depth - 1];
  }
  if (s.depth < 16) {
    s.stack[s.depth] = cat;
    s.started[s.depth] = now;
  }
  ++s.depth;
  ++s.counts[cat];
}

inline void End() {
  if (!Enabled()) return;
  ThreadState& s = State();
  if (s.depth <= 0) return;
  --s.depth;
  const uint64_t now = mach_absolute_time();
  if (s.depth < 16) {
    s.ticks[s.stack[s.depth]] += now - s.started[s.depth];
  }
  if (s.depth > 0 && s.depth <= 16) {
    s.started[s.depth - 1] = now;  // resume the parent
  }
}

struct Scope {
  explicit Scope(int cat) { Begin(cat); }
  ~Scope() { End(); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

// A scope that only counts when `active` (e.g. only on a cache miss).
struct ScopeIf {
  bool active;
  ScopeIf(int cat, bool a) : active(a) {
    if (active) Begin(cat);
  }
  ~ScopeIf() {
    if (active) End();
  }
};

inline uint64_t ThreadCpuUs() {
  thread_basic_info_data_t info;
  mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
  mach_port_t self = mach_thread_self();
  const kern_return_t kr = thread_info(self, THREAD_BASIC_INFO, thread_info_t(&info), &n);
  mach_port_deallocate(mach_task_self(), self);
  if (kr != KERN_SUCCESS) return 0;
  return uint64_t(info.user_time.seconds + info.system_time.seconds) * 1000000 +
         info.user_time.microseconds + info.system_time.microseconds;
}

// Called on the GPU Commands thread at every guest swap (before the frame id advances).
inline void OnSwap(uint64_t frame) {
  if (!Enabled()) return;
  static uint64_t last_swap = 0;
  static uint64_t last_cpu = 0;
  static uint32_t logged = 0;
  static Baseline baseline;
  ThreadState& s = State();
  const uint64_t now = mach_absolute_time();
  // Close the open scopes at the swap boundary (charge them to this frame).
  if (s.depth > 0 && s.depth <= 16) {
    s.ticks[s.stack[s.depth - 1]] += now - s.started[s.depth - 1];
    s.started[s.depth - 1] = now;
  }
  const uint64_t cpu = ThreadCpuUs();
  if (last_swap) {
    const double interval_us = TicksToUs(now - last_swap);
    const double floor_us = double(ThresholdUs());
    bool hitch = interval_us >= floor_us;
    double base_us = -1.0;  // < 0: absolute rule (no "base" in the line)
    if (hitch && Relative()) {
      // Only long frames pay for the median.
      base_us = baseline.Median();
      const double threshold =
          std::min(std::max(base_us * Ratio(), floor_us), std::max(double(CeilUs()), floor_us));
      hitch = interval_us >= threshold;
    }
    if (hitch && logged < 400) {
      ++logged;
      double sum = 0;
      char buf[512];
      int len = 0;
      for (int c = 0; c < kCount; ++c) {
        const double us = TicksToUs(s.ticks[c]);
        sum += us;
        if (us >= 500.0) {
          len += std::snprintf(buf + len, sizeof(buf) - size_t(len), " %s %.1f(%u)", CatName(c),
                               us / 1000.0, s.counts[c]);
          if (len >= int(sizeof(buf)) - 32) break;
        }
      }
      const double other = interval_us > sum ? interval_us - sum : 0.0;
      const double cpu_ms = last_cpu && cpu >= last_cpu ? double(cpu - last_cpu) / 1000.0 : 0.0;
      if (base_us >= 0.0) {
        REXLOG_INFO("[hitch] f{} {:.1f} ms (cpu {:.1f}, base {:.1f}):{} other {:.1f}", frame,
                    interval_us / 1000.0, cpu_ms, base_us / 1000.0, len ? buf : "", other / 1000.0);
      } else {
        REXLOG_INFO("[hitch] f{} {:.1f} ms (cpu {:.1f}):{} other {:.1f}", frame, interval_us / 1000.0, cpu_ms,
                    len ? buf : "", other / 1000.0);
      }
    }
    if (Relative()) baseline.Push(interval_us);
  }
  last_swap = now;
  last_cpu = cpu;
  for (int c = 0; c < kCount; ++c) {
    s.ticks[c] = 0;
    s.counts[c] = 0;
  }
}

}  // namespace rex::graphics::metal::hitch
