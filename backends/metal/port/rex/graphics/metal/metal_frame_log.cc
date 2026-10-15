#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_readiness.h"
#include "rex/graphics/metal/metal_trace.h"
#include "rex/cvar.h"
#include "rex/logging.h"
#include <mach/mach_time.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

DEFINE_bool(metal_frame_logging, true,
            "Record timestamped frame CSV files beside the main log. "
            "Changes apply on the next launch.", "Metal");
REXCVAR_DECLARE(std::string, log_file);

namespace rex::graphics::metal::frame_log {
namespace {
struct Event { const char* name; uint64_t frame, time, duration; bool warmup; };
struct State {
  std::mutex mutex;
  std::vector<Event> pending;
  std::atomic<bool> enabled{false}, warmup{true};
  std::atomic<uint64_t> next_frame{0};
  // Flush acknowledgement: requests / completed (by request number), newest flushed swap frame and cut time.
  std::condition_variable wake;
  std::atomic<uint64_t> flush_requests{0}, flushes_done{0}, flushed_frame{0}, flushed_us{0};
  uint64_t origin_us = 0, dropped = 0;
  FILE* file = nullptr;
};
// The runtime hard-exits at shutdown. Keep state alive for late Metal callbacks;
// the writer flushes every 500ms, so at most the final half-second is unsaved.
State& Get() { static State* state = new State; return *state; }
thread_local uint64_t current_frame = 0;
thread_local bool synthetic = false;
uint64_t UptimeUs() {
  static const auto scale = [] { mach_timebase_info_data_t s; mach_timebase_info(&s); return s; }();
  return static_cast<uint64_t>((static_cast<long double>(mach_absolute_time()) * scale.numer / scale.denom) / 1000);
}
void Append(const char* name, uint64_t frame, uint64_t time, uint64_t duration) {
  auto& s = Get();
  if (!s.enabled.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.pending.size() == 32768) { ++s.dropped; return; }
  s.pending.push_back({name, frame, time, duration, s.warmup.load(std::memory_order_relaxed)});
}
}  // namespace
bool Enabled() { return Get().enabled.load(std::memory_order_relaxed); }
uint64_t NowUs() { return UptimeUs() - Get().origin_us; }
void Initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!REXCVAR_GET(metal_frame_logging)) return;
    auto& s = Get();
    auto main_log = std::filesystem::path(REXCVAR_GET(log_file));
    auto folder = main_log.empty() ? std::filesystem::current_path() / "logs" : main_log.parent_path();
    if (folder.empty()) folder = std::filesystem::current_path();
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto path = folder / ("frames-" + std::to_string(stamp) + "-" + std::to_string(getpid()) + ".csv");
    s.file = std::fopen(path.c_str(), "wx");
    if (!s.file) { REXLOG_WARN("Metal frame log: could not create {}", path.string()); return; }
    s.origin_us = UptimeUs();
    s.pending.reserve(32768);
    std::fputs("event,frame_id,time_us,duration_us,warmup\n", s.file);
    std::fflush(s.file);
    s.enabled.store(true, std::memory_order_relaxed);
    std::thread([] {
      auto& s = Get();
      std::vector<Event> batch;
      batch.reserve(32768);
      uint64_t last_tag_frame = 0;
      while (s.enabled.load(std::memory_order_relaxed)) {
        uint64_t dropped, request, cut_us;
        {
          std::unique_lock<std::mutex> lock(s.mutex);
          s.wake.wait_for(lock, std::chrono::milliseconds(500), [&s] {
            return s.flush_requests.load(std::memory_order_relaxed) > s.flushes_done.load(std::memory_order_relaxed);
          });
          request = s.flush_requests.load(std::memory_order_relaxed);
          cut_us = NowUs();
          batch.swap(s.pending);
          dropped = s.dropped; s.dropped = 0;
        }
        uint64_t newest_swap = 0;
        for (auto& e : batch) {
          std::fprintf(s.file, "%s,%llu,%llu,%llu,%d\n", e.name,
              (unsigned long long)e.frame, (unsigned long long)e.time,
              (unsigned long long)e.duration, int(e.warmup));
          if (e.name[0] == 'g' && std::strcmp(e.name, "guest_swap") == 0) newest_swap = std::max(newest_swap, e.frame);
        }
        if (dropped) std::fprintf(s.file, "dropped,0,%llu,%llu,0\n",
            (unsigned long long)NowUs(), (unsigned long long)dropped);
        batch.clear();
        if (std::fflush(s.file) != 0 || std::ferror(s.file)) {
          s.enabled.store(false, std::memory_order_relaxed);
          REXLOG_WARN("Metal frame log: write failed; recording disabled");
        }
        if (newest_swap) s.flushed_frame.store(newest_swap, std::memory_order_relaxed);
        s.flushed_us.store(cut_us, std::memory_order_relaxed);
        s.flushes_done.store(request, std::memory_order_release);
        // Harness attached: publish the flushed frame (recorder tag csv_frame), so the benchmark and
        // f2diag know the window is on disk instead of sleeping past the 500 ms cadence.
        if (newest_swap != last_tag_frame && readiness::Enabled()) {
          last_tag_frame = newest_swap;
          if (const Fable2FrApi* api = trace::Api()) {
            char kv[48];
            std::snprintf(kv, sizeof kv, "csv_frame=%llu", (unsigned long long)newest_swap);
            api->tag(kv);
          }
        }
      }
    }).detach();
    REXLOG_INFO("Metal frame log: {} (500ms flush interval)", path.string());
  });
}
// The frame id comes from the flight recorder when it is present, so CSV rows and recorder events
// share frame numbers (both count every guest swap from 1).
uint64_t BeginFrame() {
  const uint32_t recorder_frame = trace::Swap();
  if (!Enabled()) return 0;
  current_frame = recorder_frame ? recorder_frame : ++Get().next_frame;
  Record(synthetic ? "synthetic_swap" : "guest_swap", current_frame);
  return current_frame;
}
uint64_t CurrentFrame() { return current_frame; }
void SetSynthetic(bool value) { synthetic = value; }
void Record(const char* event, uint64_t frame, uint64_t duration_us) {
  // Ring bridge (independent of the CSV cvar); the swap itself is a recorder event already.
  if (event[0] != 'g' || std::strcmp(event, "guest_swap") != 0) trace::Csv(event, frame, trace::NowTicks(), duration_us);
  if (Enabled()) Append(event, frame, NowUs(), duration_us);
}
uint64_t FlushedFrame() { return Get().flushed_frame.load(std::memory_order_relaxed); }
uint64_t FlushedUs() { return Get().flushed_us.load(std::memory_order_relaxed); }
bool RequestFlush(int timeout_ms) {
  auto& s = Get();
  if (!Enabled()) return false;
  uint64_t request;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    request = s.flush_requests.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  s.wake.notify_all();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (s.flushes_done.load(std::memory_order_acquire) < request) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}
void Warmup(bool active) {
  Get().warmup.store(active, std::memory_order_relaxed);
  Record(active ? "warmup_begin" : "warmup_end", 0);
}
void Presented(uint64_t frame, double uptime_seconds) {
  if (uptime_seconds > 0) trace::Csv("presented", frame, trace::UptimeSecondsToTicks(uptime_seconds), 0);
  if (!Enabled()) return;
  if (uptime_seconds == 0) { Record("present_skipped", frame); return; }
  const double us = uptime_seconds * 1000000.0;
  if (!(us >= double(Get().origin_us))) { Record("present_unavailable", frame); return; }
  Append("presented", frame, uint64_t(us) - Get().origin_us, 0);
}
void RecordUptime(const char* event, uint64_t frame, double uptime_seconds, uint64_t duration_us) {
  trace::Csv(event, frame, trace::UptimeSecondsToTicks(uptime_seconds), duration_us);
  if (!Enabled()) return;
  const double us = uptime_seconds * 1000000.0;
  const double origin = double(Get().origin_us);
  Append(event, frame, us >= origin ? uint64_t(us - origin) : 0, duration_us);
}
}  // namespace rex::graphics::metal::frame_log
