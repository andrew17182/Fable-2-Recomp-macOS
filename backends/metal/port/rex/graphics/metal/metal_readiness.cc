#include "rex/graphics/metal/metal_readiness.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_trace.h"
#include "rex/logging.h"

namespace rex::graphics::metal::readiness {

uint32_t g_frame_draws = 0;

namespace {
constexpr int kWindow = 15;          // frames per stability window
constexpr int kRing = 64;
constexpr int kWorldFrames = 10;     // frames in a row over the draw threshold
constexpr int kWorldLostFrames = 30; // frames in a row under it
constexpr int kProbeEvery = 8;       // swaps between background probes
constexpr int64_t kSampleQuietNs = 1000000000;  // ns-spec: no unconverged sample for 1 s

uint32_t WorldDraws() {
  static const uint32_t n = [] {
    const char* v = std::getenv("FABLE2_READY_WORLD_DRAWS");
    const long x = v && *v ? std::strtol(v, nullptr, 10) : 500;
    return uint32_t(x > 0 ? x : 500);
  }();
  return n;
}

double PsoUs() {
  static const double us = [] {
    const char* v = std::getenv("FABLE2_READY_PSO_MS");
    const double ms = v && *v ? std::atof(v) : 1.0;
    return (ms > 0 ? ms : 1.0) * 1000.0;
  }();
  return us;
}

double TicksToUs(uint64_t ticks) {
  static const double scale = [] {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return double(tb.numer) / double(tb.denom) / 1000.0;
  }();
  return double(ticks) * scale;
}

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<BackgroundProbe> g_probe{nullptr};
std::atomic<void*> g_probe_owner{nullptr};
std::atomic<int64_t> g_last_unconverged_ns{0};

// Snapshot for StateJson (written on the GPU Commands thread, read by the test-control thread).
struct Snapshot {
  std::atomic<uint64_t> frame{0};
  std::atomic<uint32_t> draws{0}, world_run{0}, frames_since_pso{0}, stable_run{0};
  std::atomic<bool> world{false}, steady{false};
  std::atomic<uint32_t> med_us{0}, prev_med_us{0}, max_us{0};
  std::atomic<uint32_t> ns_pending{0}, ns_spec_pending{0}, pipelines_pending{0};
  std::atomic<bool> warmup_pending{false}, spec_sampling{false};
  std::atomic<int32_t> bg_busy{-1};  // -1: not probed yet
};
Snapshot& Snap() {
  static Snapshot* s = new Snapshot;  // outlives late callbacks at the SDK's hard exit
  return *s;
}

void PublishTag(const char* name, int64_t value) {
  const Fable2FrApi* api = trace::Api();
  if (!api) return;
  char kv[64];
  std::snprintf(kv, sizeof kv, "%s=%lld", name, static_cast<long long>(value));
  api->tag(kv);
}

double Median(double* v, int n) {
  std::nth_element(v, v + n / 2, v + n);
  return v[n / 2];
}
}  // namespace

bool Enabled() {
  static const bool on = [] {
    const char* force = std::getenv("FABLE2_READINESS");
    if (force && force[0] == '0') return false;
    if (force && force[0] == '1') return true;
    const char* control = std::getenv("FABLE2_TEST_CONTROL_DIR");
    const char* bench = std::getenv("FABLE2_BENCHMARK");
    return (control && *control) || (bench && *bench);
  }();
  return on;
}

void SetBackgroundProbe(BackgroundProbe probe, void* owner) {
  g_probe_owner.store(owner, std::memory_order_relaxed);
  g_probe.store(probe, std::memory_order_release);
}

void ClearBackgroundProbe(void* owner) {
  if (g_probe_owner.load(std::memory_order_relaxed) != owner) return;
  g_probe.store(nullptr, std::memory_order_release);
  g_probe_owner.store(nullptr, std::memory_order_relaxed);
}

void NoteUnconvergedSample(int64_t steady_ns) {
  g_last_unconverged_ns.store(steady_ns, std::memory_order_relaxed);
}

void OnSwap(uint64_t frame, double pso_us) {
  const uint32_t draws = g_frame_draws;
  g_frame_draws = 0;
  if (!Enabled()) return;
  static uint64_t last_swap = 0;
  static double ring[kRing] = {};
  static int count = 0, next = 0;
  static uint32_t over = 0, under = 0, since_pso = kWindow * 4, stable_run = 0, swaps = 0;
  static bool world = false, steady = false;
  static int32_t bg_busy = -1;
  Snapshot& s = Snap();
  const uint64_t now = mach_absolute_time();
  if (last_swap) {
    ring[next] = TicksToUs(now - last_swap);
    next = (next + 1) % kRing;
    if (count < kRing) ++count;
  }
  last_swap = now;

  // world
  if (draws >= WorldDraws()) { ++over; under = 0; } else { ++under; over = 0; }
  const bool world_now = world ? under < uint32_t(kWorldLostFrames) : over >= uint32_t(kWorldFrames);

  // steady
  since_pso = pso_us >= PsoUs() ? 0 : std::min<uint32_t>(since_pso + 1, 1000000);
  bool steady_now = false;
  double med = 0, prev_med = 0, max_us = 0;
  if (count >= 2 * kWindow) {
    double a[kWindow], b[kWindow];
    for (int i = 0; i < kWindow; ++i) {
      a[i] = ring[(next - 1 - i + 2 * kRing) % kRing];
      b[i] = ring[(next - 1 - kWindow - i + 2 * kRing) % kRing];
      max_us = std::max(max_us, a[i]);
    }
    med = Median(a, kWindow);
    prev_med = Median(b, kWindow);
    steady_now = med > 0 && prev_med > 0 && std::abs(med / prev_med - 1.0) <= 0.05 && max_us < 2.0 * med &&
                 since_pso >= uint32_t(kWindow);
  }
  stable_run = steady_now ? stable_run + 1 : 0;

  // bg_busy (probe every kProbeEvery swaps)
  if (++swaps % kProbeEvery == 1) {
    Background bg;
    if (BackgroundProbe probe = g_probe.load(std::memory_order_acquire)) {
      bg = probe(g_probe_owner.load(std::memory_order_relaxed));
    }
    const int64_t last_sample = g_last_unconverged_ns.load(std::memory_order_relaxed);
    const bool sampling = last_sample && SteadyNs() - last_sample < kSampleQuietNs;
    const int64_t busy = int64_t(bg.ns_pending) + bg.ns_spec_pending + bg.pipelines_pending +
                         (bg.warmup_pending ? 1 : 0) + (sampling ? 1 : 0);
    s.ns_pending.store(bg.ns_pending, std::memory_order_relaxed);
    s.ns_spec_pending.store(bg.ns_spec_pending, std::memory_order_relaxed);
    s.pipelines_pending.store(bg.pipelines_pending, std::memory_order_relaxed);
    s.warmup_pending.store(bg.warmup_pending, std::memory_order_relaxed);
    s.spec_sampling.store(sampling, std::memory_order_relaxed);
    const int32_t busy_now = int32_t(std::min<int64_t>(busy, 9999));
    // Publish 0 <-> non-zero changes, and the count at most every probe.
    if (busy_now != bg_busy) {
      PublishTag("bg_busy", busy_now);
      bg_busy = busy_now;
      s.bg_busy.store(busy_now, std::memory_order_relaxed);
    }
  }

  if (world_now != world) {
    world = world_now;
    PublishTag("world", world ? 1 : 0);
    REXLOG_INFO("[ready] world {} at frame {} ({} draws)", world ? "drawing" : "lost", frame, draws);
  }
  if (steady_now != steady) {
    steady = steady_now;
    PublishTag("steady", steady ? 1 : 0);
  }
  s.frame.store(frame, std::memory_order_relaxed);
  s.draws.store(draws, std::memory_order_relaxed);
  s.world_run.store(over, std::memory_order_relaxed);
  s.world.store(world, std::memory_order_relaxed);
  s.steady.store(steady, std::memory_order_relaxed);
  s.stable_run.store(stable_run, std::memory_order_relaxed);
  s.frames_since_pso.store(since_pso, std::memory_order_relaxed);
  s.med_us.store(uint32_t(med), std::memory_order_relaxed);
  s.prev_med_us.store(uint32_t(prev_med), std::memory_order_relaxed);
  s.max_us.store(uint32_t(max_us), std::memory_order_relaxed);
}

std::string StateJson() {
  Snapshot& s = Snap();
  char buf[1024];
  std::snprintf(
      buf, sizeof buf,
      "{\"enabled\":%s,\"frame\":%llu,\"world\":%s,\"draws\":%u,\"world_run\":%u,\"world_draws\":%u,"
      "\"steady\":%s,\"stable_frames\":%u,\"med_ms\":%.3f,\"prev_med_ms\":%.3f,\"max_ms\":%.3f,"
      "\"frames_since_pso\":%u,\"bg_busy\":%d,\"ns_pending\":%u,\"ns_spec_pending\":%u,"
      "\"ns_spec_sampling\":%s,\"pipelines_pending\":%u,\"warmup_pending\":%s,"
      "\"csv_flushed_frame\":%llu,\"csv_flushed_us\":%llu}",
      Enabled() ? "true" : "false", static_cast<unsigned long long>(s.frame.load()),
      s.world.load() ? "true" : "false", s.draws.load(), s.world_run.load(), WorldDraws(),
      s.steady.load() ? "true" : "false", s.stable_run.load(), s.med_us.load() / 1000.0,
      s.prev_med_us.load() / 1000.0, s.max_us.load() / 1000.0, s.frames_since_pso.load(), s.bg_busy.load(),
      s.ns_pending.load(), s.ns_spec_pending.load(), s.spec_sampling.load() ? "true" : "false",
      s.pipelines_pending.load(), s.warmup_pending.load() ? "true" : "false",
      static_cast<unsigned long long>(frame_log::FlushedFrame()),
      static_cast<unsigned long long>(frame_log::FlushedUs()));
  return buf;
}

}  // namespace rex::graphics::metal::readiness
