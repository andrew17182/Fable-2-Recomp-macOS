#pragma once
// [occlusion-real] Guest waits on occlusion-query results
// (docs/occlusion-queries-20261014.md).
//
// Fable II's object culling (sub_8220BDD8) spins on D3D's GetData
// (sub_8220BE78) until the query's END record loses its 0xFFFFFEED sentinel.
// With real queries (FABLE2_OCCLUSION_REAL=1, Metal backend) the result arrives
// when the GPU finishes, so that spin burnt about half a P-core. While the
// culling helper runs, a "not ready" GetData now sleeps until the backend
// writes another result (fable2_zpd_wait_result, exported by the Metal
// backend), then returns to the guest loop, which polls again. The guest sees
// the same answers in the same order; only the busy loop goes. The flare path
// (sub_821D8310) calls GetData once and does not wait, so it is untouched.
// The wait has a guard timeout for failures only; it is logged when it fires.
//
// FABLE2_ZPD_STATS=1 also logs a [zpd-poll] line every ~5 s: GetData calls,
// not-ready answers, culling waits that had to wait, and the time they took.
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <rex/logging/macros.h>

namespace fable2::zpdwait {
inline bool StatsEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_ZPD_STATS");
    return v && v[0] == '1';
  }();
  return enabled;
}
inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

struct Backend {
  bool (*real_enabled)() = nullptr;
  uint64_t (*generation)() = nullptr;
  bool (*wait)(uint64_t, uint32_t) = nullptr;
};
inline const Backend& backend() {
  static const Backend b = [] {
    Backend r;
    r.real_enabled = reinterpret_cast<bool (*)()>(dlsym(RTLD_DEFAULT, "fable2_zpd_real_enabled"));
    r.generation =
        reinterpret_cast<uint64_t (*)()>(dlsym(RTLD_DEFAULT, "fable2_zpd_result_generation"));
    r.wait =
        reinterpret_cast<bool (*)(uint64_t, uint32_t)>(dlsym(RTLD_DEFAULT, "fable2_zpd_wait_result"));
    if (!r.real_enabled || !r.generation || !r.wait) r = Backend{};
    return r;
  }();
  return b;
}
inline bool WaitAvailable() {
  const Backend& b = backend();
  return b.real_enabled && b.real_enabled();
}

// Failure guard only: a result normally arrives within a frame or two.
constexpr uint32_t kGuardUs = 250000;

struct Stats {
  std::atomic<uint64_t> getdata{0}, getdata_not_ready{0}, sleeps{0}, guard_fired{0};
  std::atomic<uint64_t> waits{0}, waits_waited{0}, wait_ns{0}, wait_max_ns{0};
  std::atomic<uint64_t> last_log_ns{0};
};
inline Stats& stats() {
  static Stats s;
  return s;
}
inline thread_local int in_culling_wait = 0;

inline void MaybeLog() {
  Stats& s = stats();
  const uint64_t now = NowNs();
  uint64_t last = s.last_log_ns.load(std::memory_order_relaxed);
  if (!last) {
    s.last_log_ns.compare_exchange_strong(last, now);
    return;
  }
  if (now - last < 5000000000ull || !s.last_log_ns.compare_exchange_strong(last, now)) {
    return;
  }
  const double secs = double(now - last) / 1e9;
  const uint64_t waits = s.waits.exchange(0);
  const uint64_t waited = s.waits_waited.exchange(0);
  const uint64_t wait_ns = s.wait_ns.exchange(0);
  const uint64_t wait_max = s.wait_max_ns.exchange(0);
  const uint64_t gd = s.getdata.exchange(0);
  const uint64_t nr = s.getdata_not_ready.exchange(0);
  const uint64_t sleeps = s.sleeps.exchange(0);
  REXSYS_INFO(
      "[zpd-poll] {:.1f}s: GetData {} ({} not ready, {} slept); culling waits {} ({} waited), "
      "wait total {:.2f} ms/s, max {:.3f} ms; guard fired {} (total)",
      secs, gd, nr, sleeps, waits, waited, double(wait_ns) / 1e6 / secs,
      double(wait_max) / 1e6, s.guard_fired.load());
}
}  // namespace fable2::zpdwait

extern "C" void __imp__sub_8220BE78(PPCContext&, uint8_t*);
extern "C" void sub_8220BE78(PPCContext& ctx, uint8_t* base) {
  using namespace fable2::zpdwait;
  const bool sleep_if_not_ready = in_culling_wait && WaitAvailable();
  const bool log_stats = StatsEnabled();
  if (!sleep_if_not_ready && !log_stats) {
    __imp__sub_8220BE78(ctx, base);
    return;
  }
  // Read the generation first: a result written after this read wakes the wait.
  const uint64_t seen = sleep_if_not_ready ? backend().generation() : 0;
  __imp__sub_8220BE78(ctx, base);
  const bool not_ready = ctx.r3.u32 == 1;  // S_FALSE
  if (log_stats) {
    stats().getdata.fetch_add(1, std::memory_order_relaxed);
    if (not_ready) stats().getdata_not_ready.fetch_add(1, std::memory_order_relaxed);
  }
  if (sleep_if_not_ready && not_ready) {
    if (log_stats) stats().sleeps.fetch_add(1, std::memory_order_relaxed);
    if (!backend().wait(seen, kGuardUs)) {
      const uint64_t fired = stats().guard_fired.fetch_add(1, std::memory_order_relaxed) + 1;
      if (fired <= 5 || (fired & (fired - 1)) == 0) {
        REXSYS_WARN(
            "[occlusion-real] culling wait: no query result for {} ms (guard; fired {} times). "
            "The game polls again.",
            kGuardUs / 1000, fired);
      }
    }
  }
}

extern "C" void __imp__sub_8220BDD8(PPCContext&, uint8_t*);
extern "C" void sub_8220BDD8(PPCContext& ctx, uint8_t* base) {
  using namespace fable2::zpdwait;
  const bool log_stats = StatsEnabled();
  const uint64_t not_ready_before =
      log_stats ? fable2::zpdwait::stats().getdata_not_ready.load(std::memory_order_relaxed) : 0;
  const uint64_t start = log_stats ? NowNs() : 0;
  ++in_culling_wait;
  __imp__sub_8220BDD8(ctx, base);
  --in_culling_wait;
  if (!log_stats) return;
  auto& s = fable2::zpdwait::stats();
  const uint64_t ns = NowNs() - start;
  s.waits.fetch_add(1, std::memory_order_relaxed);
  if (s.getdata_not_ready.load(std::memory_order_relaxed) != not_ready_before) {
    s.waits_waited.fetch_add(1, std::memory_order_relaxed);
    s.wait_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t prev = s.wait_max_ns.load(std::memory_order_relaxed);
    while (ns > prev && !s.wait_max_ns.compare_exchange_weak(prev, ns)) {
    }
  }
  MaybeLog();
}
