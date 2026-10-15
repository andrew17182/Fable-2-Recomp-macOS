// fable2_benchmark.h - built-in benchmark run (docs/benchmark-kit.md).
//
// FABLE2_BENCHMARK=<result.json> (absolute path; set by the benchmark kit's FableIIBenchmark, which also
// sets FABLE2_BOOT_SAVE=1, FABLE2_PHYSICAL_INPUT=0, the resolution scale and the frame cap):
//   1. waits for gameplay (the boot save's level-load call ended and the flight recorder's `loading` tag
//      cleared: 60 swaps in a row under 100 ms), then until the game is ready to measure (docs/readiness.md):
//      the `world` tag (the world is drawing), `steady` (frame pacing stable, no draw-path pipeline builds) and
//      `bg_busy` == 0 (no background shader work). No fixed settle time: FABLE2_BENCHMARK_READY_TIMEOUT_S
//      (default 60 s) is only a guard, and the result says whether it fired. With the recorder off
//      (FABLE2_TRACE=off) it waits FABLE2_BENCHMARK_WAIT_S (default 15 s) instead;
//   2. drives the camera through tools/f2diag's sweep plan (FABLE2_BENCHMARK_PLAN: `headings8` (default:
//      8 fixed headings, a 0.7 s right-stick turn at RX 24000, then a 0.75 s hold, after a 0.75 s hold) or
//      `headings8-long` (2.5 s holds after a 1.5 s hold, the plan before 2026-10-13), or `flash` (the flash
//      kit, docs/flash-kit.md: turns, then walking with and without turning, ~12 s; terrain tessellation
//      factors change with distance)) by replacing the guest pad at the replay-pad boundary
//      (fable2_benchmark_pad.h). With FABLE2_FLASH_DETECT set, the Metal presenter's flash detector scores
//      the sweep's frames (window opened and closed through its fable2_flash_control export);
//   3. records the guest frame id at every phase boundary (the frame CSV and the recorder share frame
//      ids, so the kit cuts the CSV by frame, not by clock), the memory footprint every 250 ms, process
//      CPU time and per-thread CPU time (by thread name) over the sweep, and the time to gameplay and to ready;
//   4. writes the result JSON (temp file + rename) once the frame CSV has the sweep on disk (the Metal frame
//      log's `csv_frame` tag reaches the last sweep frame; 3 s guard), and quits like the window close button.
// FABLE2_BENCHMARK_GAMEPLAY_TIMEOUT_S (default 300): no gameplay by then = result with "error", quit.
// Unset: nothing here runs.
#pragma once

#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging/macros.h>

#include "fable2_benchmark_pad.h"
#include "fable2_flight_recorder.h"

namespace fable2::bench {

inline const char* ResultPath() {
  static const char* path = [] {
    const char* v = std::getenv("FABLE2_BENCHMARK");
    return v && v[0] == '/' ? v : nullptr;
  }();
  return path;
}
inline bool Enabled() { return ResultPath() != nullptr; }

inline double EnvSeconds(const char* name, double fallback) {
  const char* v = std::getenv(name);
  return v && *v ? std::atof(v) : fallback;
}

struct Phase {
  double seconds;
  int buttons, rx, ry;
  std::string name;
  int lx = 0, ly = 0;
};
// tools/f2diag/run.py SWEEP_PLANS['headings8'] / ['headings8-long'] (keep in sync).
inline std::string PlanName() {
  const char* v = std::getenv("FABLE2_BENCHMARK_PLAN");
  if (v && std::string(v) == "flash") return "flash";
  return v && std::string(v) == "headings8-long" ? "headings8-long" : "headings8";
}
// Flash kit plan (docs/flash-kit.md): camera turns both ways, then walking forward (left stick up) with and
// without turning, so the adaptive terrain factors change with distance as well as with the view.
inline std::vector<Phase> FlashPlan() {
  return {{0.75, 0, 0, 0, "hold0"},
          {0.7, 0, 24000, 0, "turn1"},  {0.5, 0, 0, 0, "hold1"},
          {0.7, 0, 24000, 0, "turn2"},  {0.5, 0, 0, 0, "hold2"},
          {2.0, 0, 0, 0, "walk1", 0, 32000},
          {2.0, 0, 16000, 0, "walkturn1", 0, 32000},
          {0.5, 0, 0, 0, "hold3"},
          {0.7, 0, -24000, 0, "turn3"}, {0.5, 0, 0, 0, "hold4"},
          {2.0, 0, -12000, 0, "walkturn2", 0, 32000},
          {0.75, 0, 0, 0, "hold5"}};
}
// The flash detector's window (backends/metal/port/rex/ui/metal/metal_flash_detect.h): 1 open, 2 close + write.
inline int FlashControl(int op) {
  using Fn = int (*)(int);
  static const Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "fable2_flash_control"));
  return fn ? fn(op) : -1;
}
inline std::vector<Phase> Headings8(bool long_holds) {
  std::vector<Phase> plan{{long_holds ? 1.5 : 0.75, 0, 0, 0, "hold0"}};
  for (int i = 1; i < 8; ++i) {
    plan.push_back({0.7, 0, 24000, 0, "turn" + std::to_string(i)});
    plan.push_back({long_holds ? 2.5 : 0.75, 0, 0, 0, "hold" + std::to_string(i)});
  }
  return plan;
}

inline double WallNow() {
  timeval t{};
  gettimeofday(&t, nullptr);
  return double(t.tv_sec) + t.tv_usec / 1e6;
}
inline uint32_t Frame() { return fable2::fr::g_frame.load(std::memory_order_relaxed); }

inline double ProcessStartWall() {
  kinfo_proc info{};
  size_t size = sizeof(info);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return 0;
  return double(info.kp_proc.p_starttime.tv_sec) + info.kp_proc.p_starttime.tv_usec / 1e6;
}

inline uint64_t FootprintBytes() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&info), &count) != KERN_SUCCESS) return 0;
  return info.phys_footprint;
}

inline double ProcessCpuSeconds() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

// CPU time (µs) per thread name; threads with the same name are summed.
inline std::map<std::string, uint64_t> ThreadCpu() {
  std::map<std::string, uint64_t> out;
  thread_act_array_t list = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return out;
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    char name[64] = {};
    if (pthread_t pt = pthread_from_mach_thread_np(list[i])) pthread_getname_np(pt, name, sizeof(name));
    thread_basic_info_data_t info{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(list[i], THREAD_BASIC_INFO, thread_info_t(&info), &n) == KERN_SUCCESS) {
      // Group "GameThread (F8000040)" and truncated "XThread1BB03 (F" alike: drop the " (F…" guest-id suffix.
      std::string key = name[0] ? name : "(unnamed)";
      if (const size_t at = key.find(" ("); at != std::string::npos && at > 0) key.resize(at);
      if (key.rfind("XThread", 0) == 0) key = "XThread (other guest threads)";
      out[key] +=
          uint64_t(info.user_time.seconds + info.system_time.seconds) * 1000000 + info.user_time.microseconds +
          info.system_time.microseconds;
    }
    mach_port_deallocate(mach_task_self(), list[i]);
  }
  vm_deallocate(mach_task_self(), vm_address_t(list), count * sizeof(thread_act_t));
  return out;
}

inline std::string Quote(const std::string& s) {
  std::string r = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') r += '\\';
    if (static_cast<unsigned char>(c) >= 32) r += c;
  }
  return r + "\"";
}

inline void WriteResult(const std::string& json) {
  const std::filesystem::path path(ResultPath());
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << json;
  }
  std::filesystem::rename(tmp, path, error);
}

// Runs the benchmark on its own thread; `quit` closes the game (called once, at the end).
inline void Start(std::function<void()> quit) {
  if (!Enabled()) return;
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  REXSYS_INFO("[benchmark] on: result {}", ResultPath());
  std::thread([quit] {
    pthread_setname_np("Benchmark");
    const auto mono = [] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const double proc_start = ProcessStartWall();
    std::ostringstream js;
    js.setf(std::ios::fixed);
    js.precision(3);
    const std::string plan_name = PlanName();
    js << "{\n \"schema\": 1,\n \"plan\": " << Quote(plan_name) << ",\n \"pid\": " << getpid()
       << ",\n \"process_start_unix\": " << proc_start;
    const char* recorder = fable2::fr::Active() ? "loading-tag" : "fixed-wait";
    // 1. Gameplay.
    const double timeout = EnvSeconds("FABLE2_BENCHMARK_GAMEPLAY_TIMEOUT_S", 300);
    const double t_begin = mono();
    bool seen_load = false, gameplay = false;
    double t_level_load = -1;
    if (fable2::fr::Active()) {
      while (mono() - t_begin < timeout) {
        const int64_t loading = fable2::fr::GetTag(fable2::fr::kTagLoading);
        const int64_t in_load = fable2::fr::GetTag(fable2::fr::kTagLevelLoad);
        if ((loading > 0 || in_load) && !seen_load) {
          seen_load = true;
          t_level_load = WallNow() - proc_start;
        }
        if (seen_load && loading == 0 && !in_load) { gameplay = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    } else {
      std::this_thread::sleep_for(std::chrono::duration<double>(EnvSeconds("FABLE2_BENCHMARK_WAIT_S", 15)));
      gameplay = true;
    }
    const double t_gameplay = WallNow() - proc_start;
    js << ",\n \"gameplay_detection\": \"" << recorder << "\",\n \"level_load_seen_s\": " << t_level_load
       << ",\n \"gameplay_s\": " << t_gameplay << ",\n \"gameplay_frame\": " << Frame();
    if (!gameplay) {
      REXSYS_WARN("[benchmark] no gameplay after {:.0f} s; quitting", timeout);
      js << ",\n \"error\": \"gameplay not reached within " << int(timeout) << " s\"\n}\n";
      WriteResult(js.str());
      quit();
      return;
    }
    REXSYS_INFO("[benchmark] gameplay at {:.2f} s since launch (frame {}); waiting until ready", t_gameplay,
                Frame());
    // Ready to measure (docs/readiness.md): world drawing, steady pacing, no background shader work.
    bool ready_timeout = false;
    if (fable2::fr::Active()) {
      using fable2::fr::GetTag;
      const double guard = EnvSeconds("FABLE2_BENCHMARK_READY_TIMEOUT_S", 60), t0 = mono();
      while (!(GetTag(fable2::fr::kTagLoading) == 0 && GetTag(fable2::fr::kTagWorld) == 1 &&
               GetTag(fable2::fr::kTagSteady) == 1 && GetTag(fable2::fr::kTagBgBusy) == 0)) {
        if (mono() - t0 >= guard) { ready_timeout = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    const double t_ready = WallNow() - proc_start;
    js << ",\n \"ready_s\": " << t_ready << ",\n \"ready_after_gameplay_s\": " << (t_ready - t_gameplay)
       << ",\n \"ready_timeout\": " << (ready_timeout ? "true" : "false") << ",\n \"ready_frame\": " << Frame();
    if (ready_timeout) {
      REXSYS_WARN("[benchmark] not ready after {:.0f} s (world {} steady {} bg_busy {}); measuring anyway",
                  EnvSeconds("FABLE2_BENCHMARK_READY_TIMEOUT_S", 60), fable2::fr::GetTag(fable2::fr::kTagWorld),
                  fable2::fr::GetTag(fable2::fr::kTagSteady), fable2::fr::GetTag(fable2::fr::kTagBgBusy));
    }
    REXSYS_INFO("[benchmark] ready at {:.2f} s since launch ({:.2f} s after gameplay)", t_ready, t_ready - t_gameplay);
    // 2./3. Sweep.
    const auto threads0 = ThreadCpu();
    const double cpu0 = ProcessCpuSeconds(), wall0 = WallNow(), m0 = mono();
    const uint32_t frame0 = Frame();
    std::vector<uint64_t> footprint;
    std::ostringstream phases;
    phases.setf(std::ios::fixed);
    phases.precision(3);
    REXSYS_INFO("[benchmark] {} start (frame {})", plan_name, frame0);
    const int flash = FlashControl(1);  // 1 = detector on (FABLE2_FLASH_DETECT), 0 = off, -1 = no export
    bool first = true;
    for (const auto& p : plan_name == "flash" ? FlashPlan() : Headings8(plan_name == "headings8-long")) {
      g_pad.buttons.store(p.buttons, std::memory_order_relaxed);
      g_pad.rx.store(p.rx, std::memory_order_relaxed);
      g_pad.ry.store(p.ry, std::memory_order_relaxed);
      g_pad.lx.store(p.lx, std::memory_order_relaxed);
      g_pad.ly.store(p.ly, std::memory_order_relaxed);
      g_pad.active.store(true, std::memory_order_relaxed);
      const uint32_t f_start = Frame();
      const double t_start = mono() - m0, end = mono() + p.seconds;
      while (mono() < end) {
        footprint.push_back(FootprintBytes());
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(0.25, std::max(0.0, end - mono()))));
      }
      phases << (first ? "" : ",") << "\n  {\"name\": " << Quote(p.name) << ", \"t\": " << t_start
             << ", \"seconds\": " << p.seconds << ", \"rx\": " << p.rx << ", \"ly\": " << p.ly
             << ", \"frame_start\": " << f_start
             << ", \"frame_end\": " << Frame() << "}";
      first = false;
    }
    g_pad.active.store(false, std::memory_order_relaxed);
    const uint32_t frame1 = Frame();
    if (flash == 1) FlashControl(2);  // waits for in-flight frames (1 s guard), writes flash.json + PNGs
    const double cpu1 = ProcessCpuSeconds(), wall1 = WallNow();
    const auto threads1 = ThreadCpu();
    REXSYS_INFO("[benchmark] {} done (frames {}..{}, {:.2f} s)", plan_name, frame0, frame1, wall1 - wall0);
    std::vector<std::pair<uint64_t, std::string>> per_thread;
    for (const auto& [name, us] : threads1) {
      const auto it = threads0.find(name);
      const uint64_t before = it == threads0.end() ? 0 : it->second;
      if (us > before) per_thread.push_back({us - before, name});
    }
    std::sort(per_thread.rbegin(), per_thread.rend());
    uint64_t peak = 0, sum = 0;
    for (uint64_t b : footprint) { peak = std::max(peak, b); sum += b; }
    js << ",\n \"sweep\": {\"wall_start_unix\": " << wall0 << ", \"wall_end_unix\": " << wall1
       << ", \"seconds\": " << (wall1 - wall0) << ", \"frame_start\": " << frame0 << ", \"frame_end\": " << frame1
       << ",\n  \"process_cpu_s\": " << (cpu1 - cpu0) << ", \"cpu_cores\": " << (cpu1 - cpu0) / (wall1 - wall0)
       << ",\n  \"footprint_mb_mean\": " << (footprint.empty() ? 0.0 : sum / double(footprint.size()) / 1048576.0)
       << ", \"footprint_mb_peak\": " << peak / 1048576.0 << ", \"footprint_samples\": " << footprint.size()
       << ",\n  \"threads\": [";
    for (size_t i = 0; i < per_thread.size() && i < 16; ++i) {
      js << (i ? "," : "") << "\n   {\"name\": " << Quote(per_thread[i].second)
         << ", \"cpu_cores\": " << per_thread[i].first / 1e6 / (wall1 - wall0) << "}";
    }
    js << "]},\n \"flash_detector\": " << flash << ",\n \"phases\": [" << phases.str() << "]";
    // 4. Wait until the frame CSV has the sweep on disk (csv_frame tag), then write and quit.
    {
      const double t0 = mono();
      bool flushed = !fable2::fr::Active();
      while (!flushed && mono() - t0 < 3.0) {
        flushed = fable2::fr::GetTag(fable2::fr::kTagCsvFrame) >= int64_t(frame1);
        if (!flushed) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      if (!fable2::fr::Active()) std::this_thread::sleep_for(std::chrono::milliseconds(1200));
      js << ",\n \"csv_flush_wait_s\": " << (mono() - t0) << ",\n \"csv_flushed\": " << (flushed ? "true" : "false");
    }
    js << ",\n \"footprint_mb_end\": " << FootprintBytes() / 1048576.0 << ",\n \"result_unix\": " << WallNow()
       << "\n}\n";
    WriteResult(js.str());
    REXSYS_INFO("[benchmark] result written; quitting");
    quit();
  }).detach();
}

}  // namespace fable2::bench
