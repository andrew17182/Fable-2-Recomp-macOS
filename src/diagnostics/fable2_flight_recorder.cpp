// Flight recorder core (see fable2_flight_recorder.h and docs/telemetry-design-20261008.md).
#include "fable2_flight_recorder.h"

#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/sink.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "fable2_guest_stack.h"

extern char** environ;

#ifndef FABLE2_GIT_DESCRIBE
#define FABLE2_GIT_DESCRIBE "unknown"
#endif

namespace fable2::fr {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- globals

std::atomic<uint64_t> g_levels{0};
std::atomic<uint32_t> g_frame{0};
thread_local Ring* t_ring = nullptr;
std::atomic<uint8_t> g_hook_fired[128];

namespace {

constexpr const char* kCatNames[kCatCount] = {
    "frame", "gpu", "cpu", "sampler", "wait", "signal", "io", "worker0", "load", "ui", "shader",
    "draw", "clock", "input", "log", "mark", "tag", "crash", "tex", "shmem", "save"};
constexpr const char* kTagNames[kTagCount] = {"level_load", "loading", "save_notice", "region",
                                              "movie", "turbo", "fps_mode", "level_name",
                                              "quest", "focus", "save", "selftest", "contention",
                                              "world", "steady", "bg_busy", "csv_frame"};

struct TypeInfo {
  std::string name, cat, a, b, v;
  uint8_t cat_id = 0;
};

constexpr int kMaxRings = 250;    // ring index is a uint8_t; 255 = dummy
constexpr uint8_t kLogRing = 0;   // global multi-producer ring for log lines
constexpr uint8_t kSamplerRing = 1;

struct Config {
  bool active = false;
  uint32_t gap_ms = 1000, hang_ms = 3000;
  int sampler_hz = 100;
  bool selftest_gap = false, selftest_av = false;
  double selftest_gap_at = 30.0, selftest_av_at = 20.0;
  double log_seconds = 0;  // 0 = unlimited
  int max_dumps = 20;
  uint64_t max_dump_bytes = 200ull << 20;
  std::string spec, expanded, aliases;
  fs::path dir;
};

struct ClockPair {
  uint64_t mach = 0;
  int64_t monotonic_us = 0, realtime_us = 0, uptime_raw_us = 0;
};

ClockPair TakeClockPair() {
  ClockPair p;
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  p.monotonic_us = int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
  p.mach = mach_absolute_time();
  clock_gettime(CLOCK_UPTIME_RAW, &ts);
  p.uptime_raw_us = int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
  clock_gettime(CLOCK_REALTIME, &ts);
  p.realtime_us = int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
  return p;
}

mach_timebase_info_data_t Timebase() {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t{};
    mach_timebase_info(&t);
    return t;
  }();
  return tb;
}

struct DigestEntry {
  std::atomic<uint64_t> hash{0};
  std::atomic<int> state{0};  // 0 empty, 1 claiming, 2 ready
  std::atomic<uint64_t> count{0};
  uint64_t first_t = 0;
  uint32_t first_frame = 0;
  uint64_t tid = 0;
  int level = 0;
  int64_t tags[kTagCount] = {};
  int32_t tag_str[kTagCount] = {};
  char logger[24] = {};
  char line[320] = {};
  char templ[200] = {};
};
constexpr int kDigestSize = 2048;

struct GThread {
  rex::system::object_ref<rex::system::XThread> ref;
  PPCContext* ctx = nullptr;
  uint32_t guest_tid = 0;
  std::string name;
  uint32_t lo = 0, hi = 0;
  bool range_ok = false;
  uint64_t last_lr = ~0ull;
  uint32_t last_sp = ~0u;
  uint32_t same = 0;
  uint32_t chain[24] = {};
  int chain_n = 0;
  bool torn = false;
  int legacy_slot = -1;
  uint16_t idx = 0;
  Ring* ring = nullptr;
  uint8_t state = 0;
};

struct Pending {
  std::string reason, detail;
  bool sync = false;
  uint64_t id = 0;
};

struct State {
  Config cfg;
  ClockPair start;
  std::mutex reg_mu;
  std::array<Ring*, 256> rings{};
  std::atomic<int> ring_count{0};
  Ring dummy;
  Event dummy_buf[16];
  // types
  std::mutex type_mu;
  std::vector<TypeInfo> types = std::vector<TypeInfo>(kTDynamic);
  std::unordered_map<std::string, uint16_t> type_by_name;
  // hooks
  std::mutex hook_mu;
  std::vector<std::string> hooks;
  uint64_t hook_first_t[128] = {};
  // tags
  std::atomic<int64_t> tag_val[kTagCount];
  std::atomic<int32_t> tag_str[kTagCount];
  std::mutex str_mu;
  std::vector<std::string> strings;
  // swap / paint
  std::atomic<uint64_t> last_swap_t{0}, last_paint_t{0}, first_swap_t{0};
  uint64_t swap_intervals[256] = {};  // ticks, written by the swap thread
  std::atomic<uint32_t> swap_pos{0};
  std::atomic<uint64_t> last_gap_ticks{0};
  std::atomic<bool> in_level_load{false};
  std::atomic<uint32_t> level_loads{0};
  // Guest frame at which the last level-load call returned / the loading tag cleared: anchors for
  // frame-matched parity captures (f2diag `still --at-frame load+N`, docs/readiness.md).
  std::atomic<uint32_t> load_end_frame{0};
  std::atomic<uint32_t> loading_clear_frame{0};
  uint32_t load_streak = 0;
  bool background_test = false;
  // triggers
  std::mutex trig_mu;
  std::condition_variable trig_cv, done_cv;
  std::vector<Pending> pending;
  uint64_t next_sync_id = 1, done_sync_id = 0;
  std::map<uint64_t, std::string> sync_results;
  std::atomic<bool> trig_flag{false};
  // signal-safe trigger bits from the log sink
  std::atomic<uint32_t> sink_bits{0};
  std::atomic<uint64_t> av_count{0};
  char first_av_line[320] = {};
  std::atomic<bool> first_av_claimed{false};
  char sink_detail[4][240] = {};
  // dumps
  int dumps = 0;
  uint64_t dump_bytes = 0;
  std::map<std::string, uint64_t> last_dump_t;
  std::map<std::string, int> per_reason, suppressed;
  bool exit_dumped = false;
  // digest
  DigestEntry* digest = nullptr;
  std::atomic<uint64_t> digest_dirty{0};
  uint64_t digest_flushed = ~0ull;
  // sampler
  std::vector<GThread> gthreads;
  uint8_t* membase = nullptr;
  std::atomic<uint32_t> legacy_total[3], legacy_wait[3];
  std::unordered_map<uint64_t, uint32_t> stack_hist;  // sampler:3 [stack-sample] lines
  std::unordered_map<uint64_t, std::vector<uint32_t>> stack_keys;
  std::unordered_map<uint32_t, uint32_t> lr_hist[3];  // sampler:3 [gpu-time-trace] guest lr top lines
  // stream
  FILE* stream = nullptr;
  std::vector<uint64_t> stream_pos = std::vector<uint64_t>(256, 0);
  uint64_t stream_overruns = 0;
  // misc
  std::atomic<bool> recorder_running{false};
  mach_port_t recorder_port = 0;
  std::string binary_sha;
  std::vector<std::string> level_names;
  uint64_t last_session_write = 0;
};

State& S() {
  static State* s = new State;  // leaked: the runtime hard-exits; late callbacks stay valid
  return *s;
}

int64_t EnvInt(const char* name, int64_t def) {
  const char* v = std::getenv(name);
  if (!v || !*v) return def;
  char* end = nullptr;
  const long long p = std::strtoll(v, &end, 10);
  return end && !*end ? p : def;
}

std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out += char(c);
        }
    }
  }
  return out;
}
std::string Q(std::string_view s) { return "\"" + JsonEscape(s) + "\""; }

std::string Hex32(uint32_t v) {
  char b[12];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}

bool WriteFileAtomic(const fs::path& path, const std::string& text) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), std::streamsize(text.size()));
    if (!f) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  return !ec;
}

bool SafeCopy(const void* src, void* dst, size_t n) {
  mach_vm_size_t out = 0;
  return mach_vm_read_overwrite(mach_task_self(), mach_vm_address_t(uintptr_t(src)), n,
                                mach_vm_address_t(uintptr_t(dst)), &out) == KERN_SUCCESS &&
         out == n;
}

// ---------------------------------------------------------------- config

void ParseSpec(Config& cfg, uint64_t& levels) {
  auto set_all = [&](int lvl) {
    levels = 0;
    for (int c = 0; c < kCatCount; ++c) levels |= uint64_t(lvl & 3) << (2 * c);
  };
  auto set = [&](int c, int lvl) {
    levels &= ~(uint64_t(3) << (2 * c));
    levels |= uint64_t(lvl & 3) << (2 * c);
  };
  // Defaults: level 1 everywhere except per-texture detail (tex: 3 only).
  set_all(1);
  set(kTex, 0);
  // Old switches expand into FABLE2_TRACE items (docs/telemetry-design-20261008.md §8).
  std::string spec;
  auto alias = [&](const char* env, const char* items) {
    const char* v = std::getenv(env);
    if (v && *v == '1') {
      if (!spec.empty()) spec += ',';
      spec += items;
      if (!cfg.aliases.empty()) cfg.aliases += "; ";
      cfg.aliases += std::string(env) + "=1 -> " + items;
    }
  };
  alias("FABLE2_LOADING_TRACE", "load:3,wait:3,io:2,worker0:2,signal:1,cpu:2,sampler:3");
  alias("FABLE2_STACK_SAMPLE", "sampler:3");
  alias("FABLE2_GPU_TIME_TRACE", "gpu:3,cpu:2,sampler:3,frame:2");
  alias("FABLE2_DRAW_SKIP_TRACE", "draw:2,tex:3,shmem:2");
  alias("FABLE2_SHMEM_CHECK", "shmem:3");
  alias("FABLE2_CRASH_DIAG", "crash:3");
  alias("FABLE2_SKIP_INTRO_TRACE", "io:3");
  alias("FABLE2_SAVE_TRACE", "save:3");
  alias("FABLE2_BOOT_SAVE_TRACE", "load:3");
  if (const char* v = std::getenv("FABLE2_CLOCK_REPORT"); v && *v) {
    if (!spec.empty()) spec += ',';
    spec += "clock:3";
    cfg.aliases += std::string(cfg.aliases.empty() ? "" : "; ") + "FABLE2_CLOCK_REPORT -> clock:3";
  }
  const char* user = std::getenv("FABLE2_TRACE");
  cfg.spec = user ? user : "";
  if (user && *user) {
    if (!spec.empty()) spec += ',';
    spec += user;
  }
  cfg.expanded = spec;
  cfg.active = true;
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t comma = spec.find(',', pos);
    if (comma == std::string::npos) comma = spec.size();
    std::string item = spec.substr(pos, comma - pos);
    pos = comma + 1;
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (item.empty()) continue;
    if (item == "off" || item == "0") {
      cfg.active = false;
      levels = 0;
      continue;
    }
    if (item == "on") continue;
    if (item.rfind("gap=", 0) == 0) { cfg.gap_ms = uint32_t(std::max(100L, std::atol(item.c_str() + 4))); continue; }
    if (item.rfind("hang=", 0) == 0) { cfg.hang_ms = uint32_t(std::max(500L, std::atol(item.c_str() + 5))); continue; }
    if (item == "nohost" || item == "noring") continue;  // phase 2 (host stacks)
    std::string name = item;
    int lvl = 1;
    if (const size_t colon = item.find(':'); colon != std::string::npos) {
      name = item.substr(0, colon);
      lvl = std::clamp(std::atoi(item.c_str() + colon + 1), 0, 3);
    }
    if (name == "all") {
      for (int c = 0; c < kCatCount; ++c) set(c, lvl);
      continue;
    }
    for (int c = 0; c < kCatCount; ++c)
      if (name == kCatNames[c]) set(c, lvl);
  }
  cfg.gap_ms = uint32_t(EnvInt("FABLE2_TRACE_GAP_MS", cfg.gap_ms));
  cfg.hang_ms = uint32_t(EnvInt("FABLE2_TRACE_HANG_MS", cfg.hang_ms));
  cfg.max_dumps = int(EnvInt("FABLE2_TRACE_MAX_DUMPS", cfg.max_dumps));
  if (const char* v = std::getenv("FABLE2_TRACE_LOG_SECONDS"); v && *v) cfg.log_seconds = std::atof(v);
  const int sl = int((levels >> (2 * kSampler)) & 3);
  cfg.sampler_hz = sl == 0 ? 0 : sl >= 3 ? 1000 : 100;
  cfg.sampler_hz = int(EnvInt("FABLE2_TRACE_SAMPLER_HZ", cfg.sampler_hz));
  // Self-test (validation only): FABLE2_TRACE_SELFTEST=gap,av or gap@40,av@20 (seconds after the
  // first guest swap). gap blocks the swap thread once for 1.5 s; av logs one synthetic AV line.
  if (const char* v = std::getenv("FABLE2_TRACE_SELFTEST"); v && *v) {
    std::string st = v;
    size_t p = 0;
    while (p <= st.size()) {
      size_t c = st.find(',', p);
      if (c == std::string::npos) c = st.size();
      std::string it = st.substr(p, c - p);
      p = c + 1;
      double at = -1;
      if (const size_t a = it.find('@'); a != std::string::npos) {
        at = std::atof(it.c_str() + a + 1);
        it = it.substr(0, a);
      }
      if (it == "gap") { cfg.selftest_gap = true; if (at >= 0) cfg.selftest_gap_at = at; }
      if (it == "av") { cfg.selftest_av = true; if (at >= 0) cfg.selftest_av_at = at; }
    }
  }
}

// ---------------------------------------------------------------- rings

Event* AllocEvents(size_t n) {
  void* p = mmap(nullptr, n * sizeof(Event), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? nullptr : static_cast<Event*>(p);
}

Ring* NewRing(uint8_t index, size_t cap, const char* name) {
  Ring* r = new Ring;
  r->buf = AllocEvents(cap);
  if (!r->buf) {
    delete r;
    return nullptr;
  }
  r->mask = cap - 1;
  r->index = index;
  pthread_threadid_np(nullptr, &r->host_tid);
  r->mach_port = pthread_mach_thread_np(pthread_self());
  if (name) std::snprintf(r->name, sizeof r->name, "%s", name);
  else pthread_getname_np(pthread_self(), r->name, sizeof r->name);
  return r;
}

size_t CapacityFor(const char* name) {
  // The recorder thread's ring holds the all-thread sampler (100 Hz x changed threads x 1-3 records),
  // CPU and tag events: 8 MB, ~60 s at the measured rate.
  if (std::strncmp(name, "FlightRecorder", 14) == 0) return 262144;
  static const char* hot[] = {"GameThread", "3D Engine", "GPU Commands", "Audio Worker", "Main"};
  for (const char* h : hot)
    if (std::strncmp(name, h, std::strlen(h)) == 0) return 32768;
  return 8192;
}

// ---------------------------------------------------------------- tags / strings

int32_t Intern(const std::string& s) {
  State& st = S();
  std::lock_guard<std::mutex> lock(st.str_mu);
  for (size_t i = 0; i < st.strings.size(); ++i)
    if (st.strings[i] == s) return int32_t(i);
  if (st.strings.size() >= 8192) return -1;
  st.strings.push_back(s);
  return int32_t(st.strings.size() - 1);
}
std::string StringAt(int32_t id) {
  State& st = S();
  std::lock_guard<std::mutex> lock(st.str_mu);
  return id >= 0 && size_t(id) < st.strings.size() ? st.strings[id] : std::string();
}

std::string TagsJson() {
  State& st = S();
  std::string out = "{";
  for (int i = 0; i < kTagCount; ++i) {
    if (i) out += ',';
    out += Q(kTagNames[i]) + ":";
    const int32_t sid = st.tag_str[i].load(std::memory_order_relaxed);
    if (sid >= 0) out += Q(StringAt(sid));
    else out += std::to_string(st.tag_val[i].load(std::memory_order_relaxed));
  }
  return out + "}";
}

// ---------------------------------------------------------------- log sink + digest

uint64_t Fnv(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h ? h : 1;
}

// Message template: digit runs and 0x-hex runs become '#', so "AV at 0x1234 #5" and "AV at 0x99 #6"
// share one digest row. No allocation (may run in the SDK's SIGSEGV path).
size_t MakeTemplate(const char* s, size_t n, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < cap;) {
    const char c = s[i];
    if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
      i += 2;
      while (i < n && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
      out[o++] = '#';
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      while (i < n && (std::isxdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
      out[o++] = '#';
      continue;
    }
    // Long hex words (addresses printed without 0x, e.g. 82CBD098): 6+ hex chars with a digit.
    if (std::isxdigit(static_cast<unsigned char>(c)) && (i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1])))) {
      size_t j = i;
      bool digit = false;
      while (j < n && std::isxdigit(static_cast<unsigned char>(s[j]))) digit |= std::isdigit(static_cast<unsigned char>(s[j])) != 0, ++j;
      if (j - i >= 6 && digit && (j == n || !std::isalnum(static_cast<unsigned char>(s[j])))) {
        out[o++] = '#';
        i = j;
        continue;
      }
    }
    out[o++] = c;
    ++i;
  }
  out[o] = 0;
  return o;
}

void DigestAdd(int level, std::string_view logger, const char* text, size_t n, uint64_t tid, uint64_t t) {
  State& st = S();
  if (!st.digest) return;
  char templ[200];
  const size_t tn = MakeTemplate(text, std::min<size_t>(n, 400), templ, sizeof templ);
  const uint64_t h = Fnv(std::string_view(templ, tn)) ^ (uint64_t(level) << 56);
  for (int probe = 0; probe < 64; ++probe) {
    DigestEntry& e = st.digest[(h + probe) & (kDigestSize - 1)];
    uint64_t cur = e.hash.load(std::memory_order_acquire);
    if (cur == h) {
      e.count.fetch_add(1, std::memory_order_relaxed);
      st.digest_dirty.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (cur == 0) {
      int expected = 0;
      if (!e.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        // Another thread is claiming this slot; re-read after it publishes.
        for (int spin = 0; spin < 1000 && e.state.load(std::memory_order_acquire) == 1; ++spin) {}
        if (e.hash.load(std::memory_order_acquire) == h) {
          e.count.fetch_add(1, std::memory_order_relaxed);
          st.digest_dirty.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        continue;
      }
      e.first_t = t;
      e.first_frame = g_frame.load(std::memory_order_relaxed);
      e.tid = tid;
      e.level = level;
      for (int i = 0; i < kTagCount; ++i) {
        e.tags[i] = st.tag_val[i].load(std::memory_order_relaxed);
        e.tag_str[i] = st.tag_str[i].load(std::memory_order_relaxed);
      }
      std::snprintf(e.logger, sizeof e.logger, "%.*s", int(std::min<size_t>(logger.size(), 23)), logger.data());
      const size_t ln = std::min<size_t>(n, sizeof e.line - 1);
      std::memcpy(e.line, text, ln);
      e.line[ln] = 0;
      std::memcpy(e.templ, templ, tn + 1);
      e.count.store(1, std::memory_order_relaxed);
      e.hash.store(h, std::memory_order_release);
      e.state.store(2, std::memory_order_release);
      st.digest_dirty.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

std::atomic<uint64_t> g_log_head{0};
Ring* g_log_ring = nullptr;

// Log line into the global multi-producer log ring: reserve slots with one fetch_add, write them,
// readers validate each record by its stored position (no lock; safe in signal context).
void LogRingWrite(int level, const char* text, size_t n, uint64_t tid, uint64_t t) {
  Ring* r = g_log_ring;
  if (!r) return;
  n = std::min<size_t>(n, 240);
  const uint32_t conts = uint32_t((n + 23) / 24);
  const uint64_t pos = r->head.fetch_add(1 + conts, std::memory_order_acq_rel);
  Event& e = r->buf[pos & r->mask];
  for (uint32_t i = 0; i < conts; ++i) {
    Cont& c = reinterpret_cast<Cont&>(r->buf[(pos + 1 + i) & r->mask]);
    c.type = kTText;
    c.thread = kLogRing;
    c.flags = 0;
    c.seq = uint8_t(i);
    c.aux = uint16_t(pos);
    const size_t off = size_t(i) * 24, len = std::min<size_t>(24, n - off);
    c.n = uint8_t(len);
    std::memcpy(c.w, text + off, len);
  }
  e.thread = kLogRing;
  e.flags = conts ? kHasText : 0;
  e.frame = g_frame.load(std::memory_order_relaxed);
  e.t = t;
  e.a = uint32_t(level);
  e.b = uint32_t(tid);
  e.type = kTLog;
  std::atomic_thread_fence(std::memory_order_release);
  std::atomic_ref<uint64_t>(e.v).store(pos, std::memory_order_release);
}

enum SinkBit : uint32_t { kBitFirstAv = 1, kBitGpuTimeout = 2, kBitShaderMiss = 4, kBitCrashDiag = 8, kBitPipelineFail = 16 };

class FrSink final : public spdlog::sinks::sink {
 public:
  void log(const spdlog::details::log_msg& msg) override {
    State& st = S();
    const int level = int(msg.level);
    if (level < int(spdlog::level::info)) return;
    const char* text = msg.payload.data();
    const size_t n = msg.payload.size();
    const uint64_t t = mach_absolute_time();
    if (On(kLog)) LogRingWrite(level, text, n, msg.thread_id, t);
    const std::string_view sv(text, n);
    if (sv.find("Unhandled guest access violation") != std::string_view::npos && sv.rfind("[trace]", 0) != 0) {
      st.av_count.fetch_add(1, std::memory_order_relaxed);
      bool expected = false;
      if (st.first_av_claimed.compare_exchange_strong(expected, true)) {
        std::snprintf(st.first_av_line, sizeof st.first_av_line, "%.*s", int(std::min<size_t>(n, 300)), text);
        st.sink_bits.fetch_or(kBitFirstAv, std::memory_order_release);
        st.trig_flag.store(true, std::memory_order_release);
      }
    } else if (sv.find("GPU timeout") != std::string_view::npos || sv.find("GPU sync timeout") != std::string_view::npos) {
      Flag(st, kBitGpuTimeout, 0, sv);
    } else if (sv.find("persistent miss") != std::string_view::npos) {
      Flag(st, kBitShaderMiss, 1, sv);
    } else if (sv.find("[crash-diag]") != std::string_view::npos) {
      Flag(st, kBitCrashDiag, 2, sv);
    } else if (sv.find("Failed to create pipeline state") != std::string_view::npos ||
               sv.find("materializeAll") != std::string_view::npos) {
      Flag(st, kBitPipelineFail, 3, sv);
    }
    if (level >= int(spdlog::level::warn)) DigestAdd(level, std::string_view(msg.logger_name.data(), msg.logger_name.size()), text, n, msg.thread_id, t);
  }
  void flush() override {}
  void set_pattern(const std::string&) override {}
  void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

 private:
  static void Flag(State& st, uint32_t bit, int slot, std::string_view sv) {
    if (st.sink_bits.load(std::memory_order_relaxed) & bit) return;  // keep the first detail until handled
    std::snprintf(st.sink_detail[slot], sizeof st.sink_detail[slot], "%.*s", int(std::min<size_t>(sv.size(), 230)), sv.data());
    st.sink_bits.fetch_or(bit, std::memory_order_release);
    st.trig_flag.store(true, std::memory_order_release);
  }
};

}  // namespace

// ---------------------------------------------------------------- public: rings / emit

bool Active() { return S().cfg.active; }

uint64_t TicksPerMs() {
  const auto tb = Timebase();
  return uint64_t(1000000.0 * tb.denom / tb.numer);
}
double TicksToMs(uint64_t ticks) {
  const auto tb = Timebase();
  return double(ticks) * tb.numer / tb.denom / 1e6;
}

Ring* RegisterThisThread() {
  State& st = S();
  if (!st.cfg.active) {
    t_ring = &st.dummy;
    return t_ring;
  }
  std::lock_guard<std::mutex> lock(st.reg_mu);
  const int n = st.ring_count.load(std::memory_order_relaxed);
  if (n >= kMaxRings) {
    t_ring = &st.dummy;
    return t_ring;
  }
  char name[48] = {};
  pthread_getname_np(pthread_self(), name, sizeof name);
  Ring* r = NewRing(uint8_t(n), CapacityFor(name), nullptr);
  if (!r) {
    t_ring = &st.dummy;
    return t_ring;
  }
  st.rings[n] = r;
  st.ring_count.store(n + 1, std::memory_order_release);
  t_ring = r;
  return r;
}

void EmitStack(uint16_t type, uint32_t a, uint32_t b, uint64_t v, uint8_t flags, const uint32_t* frames,
               int n, uint64_t t) {
  Ring* r = MyRing();
  n = std::clamp(n, 0, 24);
  const uint32_t conts = uint32_t((n + 5) / 6);
  const uint64_t h = r->head.load(std::memory_order_relaxed);
  Event& e = r->buf[h & r->mask];
  e.type = type;
  e.thread = r->index;
  e.flags = uint8_t(flags | (n ? kHasStack : 0));
  e.frame = g_frame.load(std::memory_order_relaxed);
  e.t = t ? t : Now();
  e.a = a;
  e.b = b;
  e.v = v;
  for (uint32_t i = 0; i < conts; ++i) {
    Cont& c = reinterpret_cast<Cont&>(r->buf[(h + 1 + i) & r->mask]);
    c.type = kTStack;
    c.thread = r->index;
    c.flags = 0;
    c.seq = uint8_t(i);
    c.aux = 0;
    const int m = std::min(6, n - int(i) * 6);
    c.n = uint8_t(m);
    for (int k = 0; k < 6; ++k) c.w[k] = k < m ? frames[i * 6 + k] : 0;
  }
  r->head.store(h + 1 + conts, std::memory_order_release);
}

void EmitText(uint16_t type, uint32_t a, uint32_t b, uint64_t v, const char* text, size_t len, uint64_t t) {
  Ring* r = MyRing();
  len = std::min<size_t>(len, 240);
  const uint32_t conts = uint32_t((len + 23) / 24);
  const uint64_t h = r->head.load(std::memory_order_relaxed);
  Event& e = r->buf[h & r->mask];
  e.type = type;
  e.thread = r->index;
  e.flags = conts ? kHasText : 0;
  e.frame = g_frame.load(std::memory_order_relaxed);
  e.t = t ? t : Now();
  e.a = a;
  e.b = b;
  e.v = v;
  for (uint32_t i = 0; i < conts; ++i) {
    Cont& c = reinterpret_cast<Cont&>(r->buf[(h + 1 + i) & r->mask]);
    c.type = kTText;
    c.thread = r->index;
    c.flags = 0;
    c.seq = uint8_t(i);
    c.aux = 0;
    const size_t off = size_t(i) * 24, m = std::min<size_t>(24, len - off);
    c.n = uint8_t(m);
    std::memset(c.w, 0, sizeof c.w);
    std::memcpy(c.w, text + off, m);
  }
  r->head.store(h + 1 + conts, std::memory_order_release);
}

uint16_t RegisterType(const char* name, const char* cat, const char* a, const char* b, const char* v) {
  State& st = S();
  int cat_id = -1;
  for (int c = 0; c < kCatCount; ++c)
    if (cat && std::strcmp(cat, kCatNames[c]) == 0) cat_id = c;
  if (cat_id < 0) cat_id = kFrame;
  if (!st.cfg.active || !On(Cat(cat_id))) return 0;
  std::lock_guard<std::mutex> lock(st.type_mu);
  if (auto it = st.type_by_name.find(name); it != st.type_by_name.end()) return it->second;
  if (st.types.size() >= 4000) return 0;
  TypeInfo ti{name ? name : "?", kCatNames[cat_id], a ? a : "", b ? b : "", v ? v : "", uint8_t(cat_id)};
  st.types.push_back(ti);
  const uint16_t id = uint16_t(st.types.size() - 1);
  st.type_by_name[ti.name] = id;
  return id;
}

void SetTag(Tag tag, int64_t value, const char* text) {
  State& st = S();
  if (!st.cfg.active || tag >= kTagCount) return;
  const int32_t sid = text ? Intern(text) : -1;
  const int64_t old = st.tag_val[tag].exchange(value, std::memory_order_relaxed);
  const int32_t old_sid = st.tag_str[tag].exchange(sid, std::memory_order_relaxed);
  if (old == value && old_sid == sid) return;
  if (On(kTag)) {
    if (text) EmitText(kTTag, tag, uint32_t(sid), uint64_t(value), text, std::strlen(text));
    else Emit(kTTag, tag, 0xFFFFFFFFu, uint64_t(value));
  }
}
int64_t GetTag(Tag tag) { return tag < kTagCount ? S().tag_val[tag].load(std::memory_order_relaxed) : 0; }

int HookId(const char* name) {
  State& st = S();
  std::lock_guard<std::mutex> lock(st.hook_mu);
  for (size_t i = 0; i < st.hooks.size(); ++i)
    if (st.hooks[i] == name) return int(i);
  if (st.hooks.size() >= 128) return -1;
  st.hooks.push_back(name);
  return int(st.hooks.size() - 1);
}
void FirstFire(int id) {
  State& st = S();
  st.hook_first_t[id] = mach_absolute_time();
  g_hook_fired[id].store(1, std::memory_order_relaxed);
}

bool LogWindowOpen() {
  const State& st = S();
  if (st.cfg.log_seconds <= 0) return true;
  return TicksToMs(mach_absolute_time() - st.start.mach) < st.cfg.log_seconds * 1000.0;
}

void Trigger(const char* reason, const char* detail) {
  State& st = S();
  if (!st.cfg.active) return;
  {
    std::lock_guard<std::mutex> lock(st.trig_mu);
    if (st.pending.size() < 32) st.pending.push_back({reason ? reason : "user", detail ? detail : "", false, 0});
  }
  st.trig_flag.store(true, std::memory_order_release);
  st.trig_cv.notify_one();
}

namespace {

std::string DumpSync(const char* reason, const char* detail, uint32_t timeout_ms) {
  State& st = S();
  if (!st.cfg.active || !st.recorder_running.load()) return "";
  std::unique_lock<std::mutex> lock(st.trig_mu);
  const uint64_t id = st.next_sync_id++;
  st.pending.push_back({reason ? reason : "user", detail ? detail : "", true, id});
  st.trig_flag.store(true, std::memory_order_release);
  st.trig_cv.notify_one();
  st.done_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return st.sync_results.count(id) != 0; });
  auto it = st.sync_results.find(id);
  if (it == st.sync_results.end()) return "";
  std::string out = it->second;
  st.sync_results.erase(it);
  return out;
}

}  // namespace

uint32_t FrameSwap() {
  State& st = S();
  if (!st.cfg.active) return g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t now = mach_absolute_time();
  const uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t prev = st.last_swap_t.exchange(now, std::memory_order_acq_rel);
  if (!prev) st.first_swap_t.store(now, std::memory_order_relaxed);
  const uint64_t dt = prev ? now - prev : 0;
  st.swap_intervals[st.swap_pos.fetch_add(1, std::memory_order_relaxed) & 255] = dt;
  if (On(kFrame)) Emit(kTSwap, frame, 0, dt, 0, now);
  const uint64_t gap_ticks = uint64_t(st.cfg.gap_ms) * TicksPerMs();
  if (prev && dt >= gap_ticks) {
    st.last_gap_ticks.store(dt, std::memory_order_relaxed);
    if (On(kFrame)) Emit(kTGap, frame, st.in_level_load.load() ? 1 : 0, dt, 0, now);
  }
  // loading tag: cleared once 60 swaps in a row arrive < 100 ms apart after the level-load call.
  if (st.tag_val[kTagLoading].load(std::memory_order_relaxed) && !st.in_level_load.load(std::memory_order_relaxed)) {
    st.load_streak = dt && dt < 100 * TicksPerMs() ? st.load_streak + 1 : 0;
    if (st.load_streak >= 60) {
      st.load_streak = 0;
      st.loading_clear_frame.store(frame, std::memory_order_relaxed);
      SetTag(kTagLoading, 0);
    }
  }
  // Self-test gap: block this (GPU Commands) thread once for 1.5 s.
  if (st.cfg.selftest_gap) {
    const uint64_t first = st.first_swap_t.load(std::memory_order_relaxed);
    if (first && TicksToMs(now - first) >= st.cfg.selftest_gap_at * 1000.0) {
      st.cfg.selftest_gap = false;
      SetTag(kTagSelfTest, 1, "gap");
      REXSYS_INFO("[trace-selftest] blocking the guest swap thread for 1500 ms (FABLE2_TRACE_SELFTEST=gap)");
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
  }
  return frame;
}

void Paint() {
  S().last_paint_t.store(mach_absolute_time(), std::memory_order_relaxed);
}

std::vector<std::pair<std::string, std::string>> ScanLevelNames(const uint8_t* membase, uint32_t sp, uint32_t hi);
struct ScanRequest { const void* ctx; uint32_t sp; uint64_t due; uint8_t purpose; uint8_t load; };
std::mutex g_scan_mu;
std::vector<ScanRequest> g_scans;

void RequestRegionScan(const void* guest_ctx, uint32_t sp, int purpose) {
  State& st = S();
  if (!st.cfg.active || !guest_ctx || !sp || st.level_names.empty()) return;
  const uint64_t now = mach_absolute_time(), tpm = TicksPerMs();
  {
    std::lock_guard<std::mutex> lock(g_scan_mu);
    if (g_scans.size() > 16) return;
    const uint8_t load = uint8_t(st.level_loads.load());
    (void)tpm;
    g_scans.push_back({guest_ctx, sp, now, uint8_t(purpose), load});
  }
  st.trig_flag.store(true, std::memory_order_release);
  st.trig_cv.notify_one();
}

void LevelLoadBegin(uint32_t r3, const void* guest_ctx, uint32_t sp) {
  State& st = S();
  if (!st.cfg.active) return;
  st.in_level_load.store(true, std::memory_order_relaxed);
  const uint32_t n = st.level_loads.fetch_add(1, std::memory_order_relaxed) + 1;
  RequestRegionScan(guest_ctx, sp, 0);
  SetTag(kTagLevelLoad, 1);
  SetTag(kTagLoading, n);
  if (On(kLoad)) Emit(kTLevelLoad, r3, n, 0, kBegin);
}
void LevelLoadEnd(uint64_t dur_ticks) {
  State& st = S();
  if (!st.cfg.active) return;
  st.in_level_load.store(false, std::memory_order_relaxed);
  st.load_streak = 0;
  st.load_end_frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
  SetTag(kTagLevelLoad, 0);
  if (On(kLoad)) Emit(kTLevelLoad, 0, st.level_loads.load(), dur_ticks, kEnd);
}

// ---------------------------------------------------------------- recorder thread

namespace {

struct ThreadRow {
  uint64_t tid;
  std::string name;
  uint64_t cpu_ns;
};

std::vector<ThreadRow> TaskThreads() {
  std::vector<ThreadRow> out;
  thread_act_array_t list = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return out;
  out.reserve(count);
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    thread_identifier_info_data_t id{};
    mach_msg_type_number_t n = THREAD_IDENTIFIER_INFO_COUNT;
    thread_extended_info_data_t ex{};
    mach_msg_type_number_t ne = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(list[i], THREAD_IDENTIFIER_INFO, thread_info_t(&id), &n) == KERN_SUCCESS &&
        thread_info(list[i], THREAD_EXTENDED_INFO, thread_info_t(&ex), &ne) == KERN_SUCCESS) {
      out.push_back({id.thread_id, ex.pth_name, ex.pth_user_time + ex.pth_system_time});
    }
    mach_port_deallocate(mach_task_self(), list[i]);
  }
  vm_deallocate(mach_task_self(), vm_address_t(list), count * sizeof(thread_act_t));
  return out;
}

uint64_t SelfCpuNs() {
  thread_extended_info_data_t ex{};
  mach_msg_type_number_t ne = THREAD_EXTENDED_INFO_COUNT;
  const mach_port_t self = mach_thread_self();
  uint64_t v = 0;
  if (thread_info(self, THREAD_EXTENDED_INFO, thread_info_t(&ex), &ne) == KERN_SUCCESS) v = ex.pth_user_time + ex.pth_system_time;
  mach_port_deallocate(mach_task_self(), self);
  return v;
}

bool IsSpinLr(uint32_t lr) {
  switch (lr) {
    case 0x822426E8u: case 0x82B9BF98u: case 0x82B9C010u: case 0x82B9C04Cu:  // 3D Engine ring poll
    case 0x8227720Cu: case 0x82CBD0A8u:                                    // GameThread yield
    case 0x82CC207Cu: case 0x82CBC68Cu:                                    // Sleep helpers
      return true;
    default:
      return false;
  }
}
bool LegacyIsWait(int slot, uint32_t lr) {
  if (slot == 0) return lr == 0x822426E8u || lr == 0x82B9BF98u || lr == 0x82B9C010u || lr == 0x82B9C04Cu;
  if (slot == 1) return lr == 0x8227720Cu || lr == 0x82CBD0A8u;
  return false;
}

// Fault-safe loader over another thread's guest stack: copies 4 KB windows with mach_vm_read_overwrite.
struct SafeStack {
  const uint8_t* membase = nullptr;
  gstack::Range range;
  uint32_t win_lo = 0, win_len = 0;
  alignas(16) uint8_t buf[4096];
  bool operator()(uint32_t addr, uint32_t* v) {
    if (addr < range.lo || addr + 4 > range.hi) return false;
    if (addr < win_lo || addr + 4 > win_lo + win_len) {
      win_lo = addr & ~3u;
      win_len = std::min<uint32_t>(sizeof buf, range.hi - win_lo);
      if (!SafeCopy(membase + win_lo, buf, win_len)) {
        win_len = 0;
        return false;
      }
    }
    uint32_t raw;
    std::memcpy(&raw, buf + (addr - win_lo), 4);
    *v = __builtin_bswap32(raw);
    return true;
  }
};

int WalkOther(const GThread& g, uint32_t sp, uint32_t* out, int max, bool* torn) {
  State& st = S();
  if (!g.range_ok || !st.membase) return 0;
  static SafeStack loader;  // recorder thread only
  loader.membase = st.membase;
  loader.range = {g.lo, g.hi};
  loader.win_len = 0;
  int n = 0;
  const gstack::Stop stop = gstack::WalkBackChain(sp, max, loader.range, loader, [&](uint32_t lr) {
    out[n++] = lr;
    return true;
  });
  *torn = stop == gstack::Stop::kBadChain || (stop == gstack::Stop::kLoad && n == 0);
  return n;
}

void RefreshGuestThreads() {
  State& st = S();
  auto* ks = rex::system::kernel_state();
  if (!ks) return;
  if (!st.membase && ks->memory()) st.membase = ks->memory()->virtual_membase();
  // Drop threads that stopped: never touch their stacks again.
  std::erase_if(st.gthreads, [](const GThread& g) { return !g.ref || !g.ref->is_running(); });
  for (auto& t : ks->object_table()->GetObjectsByType<rex::system::XThread>()) {
    if (!t || !t->is_guest_thread() || !t->is_running() || !t->thread_state() || !t->thread()) continue;
    PPCContext* ctx = t->thread_state()->context();
    bool known = false;
    for (auto& g : st.gthreads) known |= g.ctx == ctx;
    if (known) continue;
    GThread g;
    g.ref = t;
    g.ctx = ctx;
    g.guest_tid = t->thread_id();
    g.name = t->thread()->name();
    g.idx = uint16_t(g.guest_tid & 0xFFFF);
    static const char* slots[3] = {"3D Engine", "GameThread", "Audio Worker"};
    for (int k = 0; k < 3; ++k)
      if (g.name.rfind(slots[k], 0) == 0) g.legacy_slot = k;
    // Stack range from the guest PCR (stack_base_ptr +0x70 high, stack_end_ptr +0x74 low).
    uint32_t words[2] = {};
    if (st.membase && t->pcr_ptr() && SafeCopy(st.membase + t->pcr_ptr() + 0x70, words, sizeof words)) {
      const uint32_t hi = __builtin_bswap32(words[0]), lo = __builtin_bswap32(words[1]);
      if (lo < hi && hi - lo <= (16u << 20) && lo >= rex::system::XThread::kStackAddressRangeBegin &&
          hi <= rex::system::XThread::kStackAddressRangeEnd) {
        g.lo = lo;
        g.hi = hi;
        g.range_ok = true;
      }
    }
    st.gthreads.push_back(std::move(g));
  }
  // Rings by guest context (set by the wait hooks) for the WAIT state.
  const int n = st.ring_count.load(std::memory_order_acquire);
  for (auto& g : st.gthreads) {
    if (g.ring) continue;
    for (int i = 0; i < n; ++i)
      if (st.rings[i] && st.rings[i]->guest_ctx.load(std::memory_order_relaxed) == g.ctx) g.ring = st.rings[i];
  }
}

uint64_t StackKey(uint16_t idx, uint32_t lr, const uint32_t* chain, int n) {
  uint64_t h = 1469598103934665603ull ^ idx;
  h = (h ^ lr) * 1099511628211ull;
  for (int i = 0; i < n; ++i) h = (h ^ chain[i]) * 1099511628211ull;
  return h;
}

void SampleTick(bool hist) {
  State& st = S();
  Ring* r = MyRing();
  (void)r;
  const uint64_t now = mach_absolute_time();
  for (auto& g : st.gthreads) {
    if (!g.ref->is_running()) continue;
    const uint32_t lr = uint32_t(*reinterpret_cast<volatile uint64_t*>(&g.ctx->lr));
    const uint32_t sp = *reinterpret_cast<volatile uint32_t*>(&g.ctx->r1.u32);
    const bool waiting = g.ring && g.ring->wait_since.load(std::memory_order_relaxed) != 0;
    const uint8_t state = waiting ? 2 : IsSpinLr(lr) ? 1 : 0;
    if (g.legacy_slot >= 0) {
      st.legacy_total[g.legacy_slot].fetch_add(1, std::memory_order_relaxed);
      if (LegacyIsWait(g.legacy_slot, lr)) st.legacy_wait[g.legacy_slot].fetch_add(1, std::memory_order_relaxed);
      if (hist) ++st.lr_hist[g.legacy_slot][lr];
    }
    if (lr == g.last_lr && sp == g.last_sp && state == g.state) {
      ++g.same;
    } else {
      if (g.same) Emit(kTSampleRepeat, g.idx, g.same, 0, 0, now);
      g.same = 0;
      if (sp != g.last_sp) g.chain_n = WalkOther(g, sp, g.chain, 14, &g.torn);
      g.last_lr = lr;
      g.last_sp = sp;
      g.state = state;
      uint32_t frames[15];
      frames[0] = lr;
      std::memcpy(frames + 1, g.chain, sizeof(uint32_t) * g.chain_n);
      EmitStack(kTSample, lr, uint32_t(g.idx) | (uint32_t(state) << 16), sp, g.torn ? kTorn : 0, frames,
                1 + g.chain_n, now);
    }
    if (hist && state != 2) {  // [stack-sample]: running and polling threads (blocked ones are WAIT)
      const uint64_t key = StackKey(g.idx, lr, g.chain, std::min(g.chain_n, 6));
      if (++st.stack_hist[key] == 1) {
        std::vector<uint32_t> v{g.idx, lr};
        v.insert(v.end(), g.chain, g.chain + std::min(g.chain_n, 6));
        st.stack_keys[key] = std::move(v);
      }
    }
  }
}

void FlushRepeats() {
  State& st = S();
  const uint64_t now = mach_absolute_time();
  for (auto& g : st.gthreads)
    if (g.same) {
      Emit(kTSampleRepeat, g.idx, g.same, 0, 0, now);
      g.same = 0;
    }
}

std::string GThreadName(uint32_t idx) {
  for (auto& g : S().gthreads)
    if (g.idx == idx) return g.name;
  return "?";
}

// [stack-sample] (FABLE2_STACK_SAMPLE / LOADING_TRACE, sampler:3): top stacks of the last second.
void LogStackHist(double t_s) {
  State& st = S();
  if (st.stack_hist.empty()) return;
  std::vector<std::pair<uint64_t, uint32_t>> top(st.stack_hist.begin(), st.stack_hist.end());
  std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
  uint64_t n = 0;
  for (auto& e : top) n += e.second;
  std::string line;
  for (size_t i = 0; i < top.size() && i < 8; ++i) {
    const auto& k = st.stack_keys[top[i].first];
    std::string name = GThreadName(k[0]);
    for (char& c : name) if (c == ' ') c = '_';
    line += " | " + std::to_string(top[i].second) + " " + name + " " + Hex32(k[1]);
    for (size_t j = 2; j < k.size(); ++j) line += " <" + Hex32(k[j]);
  }
  REXSYS_INFO("[stack-sample] t={:.3f} n={}{}", t_s, n, line);
  st.stack_hist.clear();
  st.stack_keys.clear();
}

void LogLrTop() {
  State& st = S();
  static const char* names[3] = {"3D Engine", "GameThread", "Audio Worker"};
  for (int k = 0; k < 3; ++k) {
    std::vector<std::pair<uint32_t, uint32_t>> top(st.lr_hist[k].begin(), st.lr_hist[k].end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    uint64_t n = 0;
    for (auto& e : top) n += e.second;
    std::string line;
    char b[48];
    for (size_t i = 0; i < top.size() && i < 12; ++i) {
      std::snprintf(b, sizeof b, " %08X%s=%.1f%%", top[i].first, LegacyIsWait(k, top[i].first) ? "*" : "",
                    n ? 100.0 * top[i].second / n : 0.0);
      line += b;
    }
    if (n) REXSYS_INFO("[gpu-time-trace] guest lr top ({}, n={}, *=wait):{}", names[k], n, line);
    st.lr_hist[k].clear();
  }
}

// ---------------------------------------------------------------- dump writer

std::string TypesJson() {
  State& st = S();
  std::lock_guard<std::mutex> lock(st.type_mu);
  std::string out = "[";
  bool first = true;
  for (size_t i = 1; i < st.types.size(); ++i) {
    const auto& t = st.types[i];
    if (t.name.empty()) continue;
    if (!first) out += ',';
    first = false;
    out += "{\"id\":" + std::to_string(i) + ",\"name\":" + Q(t.name) + ",\"cat\":" + Q(t.cat) + ",\"a\":" + Q(t.a) +
           ",\"b\":" + Q(t.b) + ",\"v\":" + Q(t.v) + "}";
  }
  return out + "]";
}

std::string ClockJson(const ClockPair& p) {
  return "{\"mach\":" + std::to_string(p.mach) + ",\"monotonic_us\":" + std::to_string(p.monotonic_us) +
         ",\"uptime_raw_us\":" + std::to_string(p.uptime_raw_us) + ",\"realtime_us\":" + std::to_string(p.realtime_us) + "}";
}

std::string LevelsJson() {
  std::string out = "{";
  for (int c = 0; c < kCatCount; ++c) {
    if (c) out += ',';
    out += Q(kCatNames[c]) + ":" + std::to_string(Level(Cat(c)));
  }
  return out + "}";
}

std::string HooksJson() {
  State& st = S();
  std::lock_guard<std::mutex> lock(st.hook_mu);
  std::string fired = "[", never = "[";
  for (size_t i = 0; i < st.hooks.size(); ++i) {
    if (g_hook_fired[i].load(std::memory_order_relaxed)) {
      if (fired.size() > 1) fired += ',';
      fired += "{\"hook\":" + Q(st.hooks[i]) + ",\"first_s\":" +
               std::to_string(TicksToMs(st.hook_first_t[i] - st.start.mach) / 1000.0) + "}";
    } else {
      if (never.size() > 1) never += ',';
      never += Q(st.hooks[i]);
    }
  }
  return "{\"fired\":" + fired + "],\"never_fired\":" + never + "]}";
}

std::string StringsJson() {
  State& st = S();
  std::lock_guard<std::mutex> lock(st.str_mu);
  std::string out = "[";
  for (size_t i = 0; i < st.strings.size(); ++i) {
    if (i) out += ',';
    out += Q(st.strings[i]);
  }
  return out + "]";
}

std::string GuestThreadsJson() {
  std::string out = "[";
  bool first = true;
  for (auto& g : S().gthreads) {
    if (!first) out += ',';
    first = false;
    out += "{\"idx\":" + std::to_string(g.idx) + ",\"guest_tid\":" + std::to_string(g.guest_tid) + ",\"name\":" + Q(g.name) +
           ",\"stack_lo\":" + std::to_string(g.lo) + ",\"stack_hi\":" + std::to_string(g.hi) +
           ",\"ring\":" + (g.ring ? std::to_string(g.ring->index) : std::string("null")) + "}";
  }
  return out + "]";
}

struct RingCopy {
  uint8_t index;
  uint64_t first;
  std::vector<Event> ev;
};

RingCopy CopyRing(const Ring* r) {
  RingCopy c{r->index, 0, {}};
  const uint64_t cap = r->mask + 1;
  const uint64_t before = r->head.load(std::memory_order_acquire);
  const uint64_t start = before > cap ? before - cap : 0;
  c.ev.resize(before - start);
  for (uint64_t i = start; i < before; ++i) c.ev[i - start] = r->buf[i & r->mask];
  const uint64_t after = r->head.load(std::memory_order_acquire);
  const uint64_t valid = after > cap ? after - cap : 0;
  uint64_t drop = valid > start ? valid - start : 0;
  if (drop > c.ev.size()) drop = c.ev.size();
  c.ev.erase(c.ev.begin(), c.ev.begin() + long(drop));
  c.first = start + drop;
  return c;
}

std::string ThreadsJson(const std::vector<RingCopy>& copies, const std::vector<ThreadRow>& host) {
  State& st = S();
  std::string out = "[";
  const int n = st.ring_count.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    Ring* r = st.rings[i];
    if (!r) continue;
    std::string name = r->name;
    for (auto& h : host)
      if (h.tid == r->host_tid && !h.name.empty()) name = h.name;
    bool alive = false;
    for (auto& h : host) alive |= h.tid == r->host_tid;
    if (i) out += ',';
    out += "{\"index\":" + std::to_string(i) + ",\"tid\":" + std::to_string(r->host_tid) + ",\"name\":" + Q(name) +
           ",\"capacity\":" + std::to_string(r->mask + 1) + ",\"head\":" + std::to_string(r->head.load()) +
           ",\"alive\":" + (alive ? "true" : "false") + ",\"mpsc\":" + (i == kLogRing ? "true" : "false") + "}";
  }
  (void)copies;
  return out + "]";
}

std::string HostThreadsJson(const std::vector<ThreadRow>& host) {
  std::string out = "[";
  for (size_t i = 0; i < host.size(); ++i) {
    if (i) out += ',';
    out += "{\"tid\":" + std::to_string(host[i].tid) + ",\"name\":" + Q(host[i].name) + ",\"cpu_ms\":" +
           std::to_string(host[i].cpu_ns / 1000000) + "}";
  }
  return out + "]";
}

const char* StateName(uint8_t s) { return s == 2 ? "WAIT" : s == 1 ? "SPIN" : "WORK"; }

struct Snap {
  uint32_t lr, sp;
  uint8_t state;
  bool torn;
  std::vector<uint32_t> chain;
};

std::string SessionJson();

std::string DigestJsonl();

void FlushDigest(bool force) {
  State& st = S();
  const uint64_t dirty = st.digest_dirty.load(std::memory_order_relaxed);
  if (!force && dirty == st.digest_flushed) return;
  st.digest_flushed = dirty;
  WriteFileAtomic(st.cfg.dir / "log-digest.jsonl", DigestJsonl());
}

uint64_t DirBytes(const fs::path& p) {
  uint64_t n = 0;
  std::error_code ec;
  for (auto& e : fs::recursive_directory_iterator(p, ec))
    if (e.is_regular_file(ec)) n += e.file_size(ec);
  return n;
}

std::string WriteDump(const std::string& reason, const std::string& detail, bool forced) {
  State& st = S();
  const uint64_t now = mach_absolute_time();
  const bool user = reason == "user" || reason == "exit" || reason.rfind("user", 0) == 0;
  if (!forced) {
    struct Limit { const char* r; double cooldown_s; int max; };
    static const Limit limits[] = {{"frame_gap", 10, 20}, {"hang", 0, 20},   {"first_av", 0, 1},
                                   {"av_storm", 60, 3},   {"gpu_timeout", 30, 5}, {"shader_miss", 60, 3},
                                   {"crash_diag", 30, 3}, {"pipeline_fail", 60, 3}};
    double cooldown = 10;
    int max = 20;
    for (auto& l : limits)
      if (reason == l.r) cooldown = l.cooldown_s, max = l.max;
    auto it = st.last_dump_t.find(reason);
    if ((it != st.last_dump_t.end() && TicksToMs(now - it->second) < cooldown * 1000.0) || st.per_reason[reason] >= max ||
        (!user && (st.dumps >= st.cfg.max_dumps || st.dump_bytes >= st.cfg.max_dump_bytes))) {
      ++st.suppressed[reason];
      return "";
    }
  }
  st.last_dump_t[reason] = now;
  ++st.per_reason[reason];
  const int seq = ++st.dumps;
  char folder_name[96];
  std::snprintf(folder_name, sizeof folder_name, "%03d-%s", seq, reason.c_str());
  const fs::path dir = st.cfg.dir / "dumps" / folder_name;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return "";
  const ClockPair cp = TakeClockPair();
  FlushRepeats();
  // 1. All-thread guest stacks, three snapshots 5 ms apart (no suspension).
  RefreshGuestThreads();
  std::vector<std::vector<Snap>> snaps(3);
  for (int s = 0; s < 3; ++s) {
    if (s) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (auto& g : st.gthreads) {
      Snap sn{};
      if (g.ref->is_running()) {
        sn.lr = uint32_t(*reinterpret_cast<volatile uint64_t*>(&g.ctx->lr));
        sn.sp = *reinterpret_cast<volatile uint32_t*>(&g.ctx->r1.u32);
        sn.state = g.ring && g.ring->wait_since.load() ? 2 : IsSpinLr(sn.lr) ? 1 : 0;
        uint32_t chain[24];
        const int n = WalkOther(g, sn.sp, chain, 24, &sn.torn);
        sn.chain.assign(chain, chain + n);
      }
      snaps[s].push_back(std::move(sn));
    }
  }
  // 2. Rings.
  const std::vector<ThreadRow> host = TaskThreads();
  std::vector<RingCopy> copies;
  const int nrings = st.ring_count.load(std::memory_order_acquire);
  uint64_t total_events = 0;
  for (int i = 0; i < nrings; ++i)
    if (st.rings[i]) {
      copies.push_back(CopyRing(st.rings[i]));
      total_events += st.rings[i]->head.load(std::memory_order_relaxed);
    }
  const std::string tags = TagsJson();
  std::string header = "{\"format\":\"f2t\",\"version\":1,\"reason\":" + Q(reason) + ",\"detail\":" + Q(detail) +
                       ",\"dump\":" + Q(folder_name) + ",\"timebase\":{\"numer\":" + std::to_string(Timebase().numer) +
                       ",\"denom\":" + std::to_string(Timebase().denom) + "},\"clock\":" + ClockJson(cp) +
                       ",\"session_start\":" + ClockJson(st.start) + ",\"session_origin_monotonic_us\":" +
                       std::to_string(EnvInt("FABLE2_SESSION_ORIGIN_US", 0)) + ",\"frame\":" +
                       std::to_string(g_frame.load()) + ",\"levels\":" + LevelsJson() + ",\"types\":" + TypesJson() +
                       ",\"threads\":" + ThreadsJson(copies, host) + ",\"guest_threads\":" + GuestThreadsJson() +
                       ",\"host_threads\":" + HostThreadsJson(host) + ",\"tags\":" + tags + ",\"strings\":" +
                       StringsJson() + ",\"hooks\":" + HooksJson() + "}";
  {
    FILE* f = std::fopen((dir / "ring.f2t").c_str(), "wb");
    if (f) {
      const uint32_t len = uint32_t(header.size());
      std::fwrite("F2T1", 1, 4, f);
      std::fwrite(&len, 4, 1, f);
      std::fwrite(header.data(), 1, header.size(), f);
      for (auto& c : copies) {
        const uint32_t idx = c.index, count = uint32_t(c.ev.size());
        std::fwrite("RING", 1, 4, f);
        std::fwrite(&idx, 4, 1, f);
        std::fwrite(&c.first, 8, 1, f);
        std::fwrite(&count, 4, 1, f);
        if (count) std::fwrite(c.ev.data(), sizeof(Event), count, f);
      }
      std::fclose(f);
    }
  }
  // 3. stacks.txt (raw addresses; f2diag symbolises from generated/default/fable_2_funcs.*.h).
  std::string stacks = "# all-thread guest stacks, 3 snapshots 5 ms apart, no suspension (" + reason + ")\n"
                       "# state: WAIT = inside a hooked kernel wait, SPIN = known poll loop, WORK = other\n";
  std::string stacks_json = "[";
  for (size_t gi = 0; gi < st.gthreads.size(); ++gi) {
    const GThread& g = st.gthreads[gi];
    // Common prefix (outermost frames agree across snapshots).
    std::vector<uint32_t> common = snaps[0][gi].chain;
    for (int s = 1; s < 3; ++s) {
      const auto& c = snaps[s][gi].chain;
      size_t k = 0;
      while (k < common.size() && k < c.size() && common[common.size() - 1 - k] == c[c.size() - 1 - k]) ++k;
      common.erase(common.begin(), common.end() - long(k));
    }
    stacks += "\n[" + g.name + "] guest_tid=" + std::to_string(g.guest_tid) + " stack=" + Hex32(g.lo) + "-" + Hex32(g.hi) + "\n";
    std::string sj;
    for (int s = 0; s < 3; ++s) {
      const Snap& sn = snaps[s][gi];
      stacks += "  #" + std::to_string(s) + " " + StateName(sn.state) + " lr=" + Hex32(sn.lr) + " sp=" + Hex32(sn.sp) +
                (sn.torn ? " torn" : "") + " :";
      for (uint32_t a : sn.chain) stacks += " <" + Hex32(a);
      stacks += "\n";
      if (s) sj += ',';
      sj += "{\"state\":" + Q(StateName(sn.state)) + ",\"lr\":" + std::to_string(sn.lr) + ",\"sp\":" + std::to_string(sn.sp) +
            ",\"torn\":" + (sn.torn ? "true" : "false") + ",\"chain\":[";
      for (size_t k = 0; k < sn.chain.size(); ++k) sj += (k ? "," : "") + std::to_string(sn.chain[k]);
      sj += "]}";
    }
    std::string cj = "[";
    for (size_t k = 0; k < common.size(); ++k) cj += (k ? "," : "") + std::to_string(common[k]);
    cj += "]";
    if (gi) stacks_json += ',';
    stacks_json += "{\"thread\":" + Q(g.name) + ",\"guest_tid\":" + std::to_string(g.guest_tid) + ",\"common\":" + cj +
                   ",\"snapshots\":[" + sj + "]}";
  }
  stacks_json += "]";
  WriteFileAtomic(dir / "stacks.txt", stacks);
  // 3b. guest.txt: registers per thread (best effort: the recompiled code may hold values in host
  // registers, so ctx words can be stale between calls), 0x200 stack bytes of SPIN threads, and guest
  // word peeks (built-in frame-throttle words + FABLE2_TRACE_PEEK="addr[:len],..."). Trace-only.
  {
    std::string gt = "# guest registers (snapshot after the stacks), SPIN-thread stack bytes, peeks; big-endian words\n";
    auto hexdump = [&](uint32_t addr, uint32_t len) {
      std::string out;
      if (!st.membase || !len || len > 0x10000) return out;
      std::vector<uint8_t> buf(len);
      if (!SafeCopy(st.membase + addr, buf.data(), len)) return std::string("  (unreadable)\n");
      for (uint32_t o = 0; o < len; o += 16) {
        out += "  " + Hex32(addr + o) + ":";
        for (uint32_t w = o; w < o + 16 && w + 4 <= len; w += 4) {
          char b[12];
          std::snprintf(b, sizeof b, " %02X%02X%02X%02X", buf[w], buf[w + 1], buf[w + 2], buf[w + 3]);
          out += b;
        }
        out += "\n";
      }
      return out;
    };
    for (size_t gi = 0; gi < st.gthreads.size(); ++gi) {
      const GThread& g = st.gthreads[gi];
      if (!g.ref->is_running() || !g.ctx) continue;
      gt += "\n[" + g.name + "] " + StateName(snaps[0][gi].state) + "\n ";
      const volatile PPCContext* c = g.ctx;
      const volatile PPCRegister* r = &c->r0;
      for (int i = 0; i < 32; ++i) {
        char b[24];
        std::snprintf(b, sizeof b, " r%d=%08X", i, uint32_t(r[i].u64));
        gt += b;
        if (i % 8 == 7) gt += "\n ";
      }
      char b[48];
      std::snprintf(b, sizeof b, " lr=%08X ctr=%08X\n", uint32_t(c->lr), uint32_t(c->ctr.u64));
      gt += b;
      if (snaps[0][gi].state == 1) gt += hexdump(snaps[0][gi].sp, 0x200);
    }
    std::vector<std::pair<uint32_t, uint32_t>> peeks = {{0x83496F40, 16}, {0x83491BD0, 48}};
    if (const char* p = std::getenv("FABLE2_TRACE_PEEK")) {
      std::string s = p;
      size_t i = 0;
      while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        const std::string item = s.substr(i, j - i);
        const size_t colon = item.find(':');
        const uint32_t a = uint32_t(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16));
        const uint32_t n = colon == std::string::npos ? 16 : uint32_t(std::strtoul(item.c_str() + colon + 1, nullptr, 0));
        if (a) peeks.push_back({a, n});
        i = j + 1;
      }
    }
    gt += "\n# peeks (0x83496F44 present count, 0x83496F48 stats index, 0x83491BD0 GPU frame-callback stats x2: +16 count)\n";
    for (auto& [a, n] : peeks) gt += hexdump(a, n);
    WriteFileAtomic(dir / "guest.txt", gt);
  }
  // 4. summary.json.
  std::string frames = "[";
  const uint32_t pos = st.swap_pos.load(std::memory_order_relaxed);
  for (uint32_t i = 0, k = 0; i < 120 && i < pos; ++i) {
    const uint64_t dt = st.swap_intervals[(pos - 1 - i) & 255];
    if (k++) frames += ',';
    char b[24];
    std::snprintf(b, sizeof b, "%.2f", TicksToMs(dt));
    frames += b;
  }
  frames += "]";
  const uint64_t last_swap = st.last_swap_t.load();
  const double since_swap = last_swap ? TicksToMs(cp.mach - last_swap) : -1;
  std::string suppressed = "{";
  for (auto& [k, v] : st.suppressed) suppressed += (suppressed.size() > 1 ? "," : "") + Q(k) + ":" + std::to_string(v);
  suppressed += "}";
  const double up_s = TicksToMs(cp.mach - st.start.mach) / 1000.0;
  const uint64_t self_cpu = SelfCpuNs();
  std::string summary =
      "{\"reason\":" + Q(reason) + ",\"detail\":" + Q(detail) + ",\"dump\":" + Q(folder_name) + ",\"t_s\":" +
      std::to_string(up_s) + ",\"frame\":" + std::to_string(g_frame.load()) + ",\"ms_since_last_swap\":" +
      std::to_string(since_swap) + ",\"in_level_load\":" + (st.in_level_load.load() ? "true" : "false") +
      ",\"tags\":" + tags + ",\"last_frame_intervals_ms\":" + frames + ",\"stacks\":" + stacks_json +
      ",\"hooks\":" + HooksJson() + ",\"recorder\":{\"events_total\":" + std::to_string(total_events) +
      ",\"events_per_s\":" + std::to_string(up_s > 0 ? total_events / up_s : 0) + ",\"recorder_thread_cpu_ms\":" +
      std::to_string(self_cpu / 1000000) + ",\"recorder_thread_cpu_pct_of_core\":" +
      std::to_string(up_s > 0 ? 100.0 * (self_cpu / 1e9) / up_s : 0) + ",\"sampler_hz\":" +
      std::to_string(st.cfg.sampler_hz) + ",\"guest_threads_sampled\":" + std::to_string(st.gthreads.size()) +
      ",\"rings\":" + std::to_string(nrings) + "},\"av_lines\":" + std::to_string(st.av_count.load()) +
      ",\"first_av\":" + Q(st.first_av_line) + ",\"suppressed\":" + suppressed + ",\"levels\":" + LevelsJson() + "}";
  WriteFileAtomic(dir / "summary.json", summary);
  FlushDigest(true);
  WriteFileAtomic(st.cfg.dir / "session.json", SessionJson());
  const uint64_t bytes = DirBytes(dir);
  st.dump_bytes += bytes;
  REXSYS_INFO("[trace] dump {} written ({:.1f} MB, {} threads, {}): {}", folder_name, bytes / 1048576.0,
              st.gthreads.size(), detail.empty() ? reason : detail.substr(0, 160), dir.string());
  return dir.string();
}

std::string DigestJsonl() {
  State& st = S();
  struct Row { const DigestEntry* e; };
  std::vector<const DigestEntry*> rows;
  for (int i = 0; i < kDigestSize; ++i)
    if (st.digest[i].state.load(std::memory_order_acquire) == 2) rows.push_back(&st.digest[i]);
  std::sort(rows.begin(), rows.end(), [](auto* a, auto* b) { return a->first_t < b->first_t; });
  std::string out;
  for (auto* e : rows) {
    std::string tags = "{";
    for (int i = 0; i < kTagCount; ++i) {
      if (i) tags += ',';
      tags += Q(kTagNames[i]) + ":" + (e->tag_str[i] >= 0 ? Q(StringAt(e->tag_str[i])) : std::to_string(e->tags[i]));
    }
    tags += "}";
    out += "{\"level\":" + Q(spdlog::level::to_string_view(spdlog::level::level_enum(e->level)).data()) +
           ",\"count\":" + std::to_string(e->count.load()) + ",\"template\":" + Q(e->templ) + ",\"first\":{\"t_s\":" +
           std::to_string(TicksToMs(e->first_t - st.start.mach) / 1000.0) + ",\"frame\":" + std::to_string(e->first_frame) +
           ",\"tid\":" + std::to_string(e->tid) + ",\"logger\":" + Q(e->logger) + ",\"line\":" + Q(e->line) +
           ",\"tags\":" + tags + "}}\n";
  }
  return out;
}

std::string Sha256File(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return "";
  CC_SHA256_CTX c;
  CC_SHA256_Init(&c);
  std::vector<char> buf(1 << 20);
  while (f) {
    f.read(buf.data(), std::streamsize(buf.size()));
    if (f.gcount() > 0) CC_SHA256_Update(&c, buf.data(), CC_LONG(f.gcount()));
  }
  unsigned char md[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(md, &c);
  std::string out;
  char b[3];
  for (unsigned char x : md) std::snprintf(b, sizeof b, "%02x", x), out += b;
  return out;
}

std::string Sysctl(const char* name) {
  char buf[256] = {};
  size_t n = sizeof buf - 1;
  if (sysctlbyname(name, buf, &n, nullptr, 0) != 0) return "";
  return buf;
}
int SysctlInt(const char* name) {
  int v = 0;
  size_t n = sizeof v;
  return sysctlbyname(name, &v, &n, nullptr, 0) == 0 ? v : -1;
}

std::string SessionJson() {
  State& st = S();
  std::string env = "{";
  for (char** e = environ; e && *e; ++e) {
    std::string_view kv(*e);
    if (!(kv.rfind("FABLE2_", 0) == 0 || kv.rfind("REX_", 0) == 0 || kv.rfind("MTL_", 0) == 0)) continue;
    const size_t eq = kv.find('=');
    if (eq == std::string_view::npos) continue;
    env += (env.size() > 1 ? "," : "") + Q(kv.substr(0, eq)) + ":" + Q(kv.substr(eq + 1));
  }
  env += "}";
  std::string cvars = "{";
  for (const auto& name : rex::cvar::ListModifiedFlags())
    cvars += (cvars.size() > 1 ? "," : "") + Q(name) + ":" + Q(rex::cvar::GetFlagByName(name));
  cvars += "}";
  std::string dumps = "{";
  for (auto& [k, v] : st.per_reason) dumps += (dumps.size() > 1 ? "," : "") + Q(k) + ":" + std::to_string(v);
  dumps += "}";
  char exe[1024] = {};
  uint32_t exe_len = sizeof exe;
  _NSGetExecutablePath(exe, &exe_len);
  return "{\"format\":\"f2-trace-session\",\"version\":1,\"pid\":" + std::to_string(getpid()) +
         ",\"git_describe\":" + Q(FABLE2_GIT_DESCRIBE) + ",\"executable\":" + Q(exe) + ",\"executable_sha256\":" +
         Q(st.binary_sha) + ",\"trace_spec\":" + Q(st.cfg.spec) + ",\"aliases\":" + Q(st.cfg.aliases) +
         ",\"expanded\":" + Q(st.cfg.expanded) + ",\"levels\":" + LevelsJson() + ",\"gap_ms\":" +
         std::to_string(st.cfg.gap_ms) + ",\"hang_ms\":" + std::to_string(st.cfg.hang_ms) + ",\"sampler_hz\":" +
         std::to_string(st.cfg.sampler_hz) + ",\"timebase\":{\"numer\":" + std::to_string(Timebase().numer) +
         ",\"denom\":" + std::to_string(Timebase().denom) + "},\"start\":" + ClockJson(st.start) + ",\"now\":" +
         ClockJson(TakeClockPair()) + ",\"session_origin_monotonic_us\":" +
         std::to_string(EnvInt("FABLE2_SESSION_ORIGIN_US", 0)) + ",\"env\":" + env + ",\"cvars_modified\":" + cvars +
         ",\"mac\":{\"model\":" + Q(Sysctl("hw.model")) + ",\"cpu\":" + Q(Sysctl("machdep.cpu.brand_string")) +
         ",\"p_cores\":" + std::to_string(SysctlInt("hw.perflevel0.physicalcpu")) + ",\"e_cores\":" +
         std::to_string(SysctlInt("hw.perflevel1.physicalcpu")) + ",\"mem_gb\":" +
         std::to_string(double([] { uint64_t m = 0; size_t n = sizeof m; sysctlbyname("hw.memsize", &m, &n, nullptr, 0); return m; }()) / 1073741824.0) +
         "},\"frame\":" + std::to_string(g_frame.load()) + ",\"tags\":" + TagsJson() + ",\"hooks\":" + HooksJson() +
         ",\"dumps\":" + dumps + ",\"dumps_written\":" + std::to_string(st.dumps) + ",\"av_lines\":" +
         std::to_string(st.av_count.load()) + "}\n";
}

// Tags pushed from outside (tools/fable2_dev_session.py watcher after a save): trace/tags-in.json,
// lines of "key=value". Polled once a second.
void PollTagsIn() {
  State& st = S();
  static int64_t last_mtime = 0;
  struct stat sb{};
  const fs::path p = st.cfg.dir / "tags-in.txt";
  if (stat(p.c_str(), &sb) != 0) return;
  const int64_t m = int64_t(sb.st_mtimespec.tv_sec) * 1000000000 + sb.st_mtimespec.tv_nsec;
  if (m == last_mtime) return;
  last_mtime = m;
  std::ifstream f(p);
  std::string line;
  while (std::getline(f, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    for (int i = 0; i < kTagCount; ++i)
      if (k == kTagNames[i]) SetTag(Tag(i), 1, v.c_str());
  }
}

// Region tag from the scan requested at a level-load call (the GameThread's stack). Only the nearest
// hit on the stack itself or one pointer away counts (depth <= 1). Measured 2026-10-08: a save boot
// names its level there (*(sp+0x314) = Caves\BowerLake\ThagsCave); an area transition (Cullis Gate)
// does not, so the tag then reads "unknown" until the next save gives level_name (session watcher).
// The loading screen's thread (3D Engine, loadingscreendata.txt) holds the whole region table, so it
// cannot tell the destination either.
void LevelScan() {
  State& st = S();
  std::vector<ScanRequest> due;
  {
    std::lock_guard<std::mutex> lock(g_scan_mu);
    if (g_scans.empty()) return;
    const uint64_t now = mach_absolute_time();
    for (auto it = g_scans.begin(); it != g_scans.end();) {
      if (it->due <= now) { due.push_back(*it); it = g_scans.erase(it); }
      else ++it;
    }
  }
  if (due.empty()) return;
  RefreshGuestThreads();
  for (auto& rq : due) {
    uint32_t hi = 0;
    std::string thread = "?";
    for (auto& g : st.gthreads)
      if (g.ctx == rq.ctx && g.range_ok && rq.sp >= g.lo && rq.sp < g.hi) hi = g.hi, thread = g.name;
    if (!hi) continue;
    // For the loading-screen thread, use its current sp (it keeps running) when still inside its stack.
    uint32_t sp = rq.sp;
    if (rq.purpose == 1) {
      const uint32_t cur = *reinterpret_cast<const volatile uint32_t*>(&static_cast<const PPCContext*>(rq.ctx)->r1.u32);
      for (auto& g : st.gthreads)
        if (g.ctx == rq.ctx && cur >= g.lo && cur < g.hi) sp = cur;
    }
    const uint64_t t0 = mach_absolute_time();
    const auto hits = ScanLevelNames(st.membase, sp, hi);
    std::string seen;
    for (auto& [n, w] : hits) seen += " " + w + "=" + n;
    const int32_t cur = st.tag_str[kTagRegion].load();
    const std::string cur_name = cur >= 0 ? StringAt(cur) : "";
    std::string pick;
    for (auto& [n, w] : hits)
      if (w[0] <= '1') { pick = n; break; }
    (void)cur_name;
    REXSYS_INFO("[trace] region scan ({}, load #{}, '{}', {:.1f} ms on the recorder thread): '{}' (depth:where=name:{})",
                rq.purpose ? "loading-screen thread" : "level-load call", rq.load, thread,
                TicksToMs(mach_absolute_time() - t0), pick, seen.empty() ? " none" : seen);
    SetTag(kTagRegion, rq.load, pick.empty() ? "unknown" : pick.c_str());
  }
}

void HandleTriggers() {
  State& st = S();
  if (!st.trig_flag.exchange(false, std::memory_order_acq_rel)) return;
  LevelScan();
  const uint32_t bits = st.sink_bits.exchange(0, std::memory_order_acq_rel);
  if (bits & kBitFirstAv) WriteDump("first_av", st.first_av_line, false);
  if (bits & kBitGpuTimeout) WriteDump("gpu_timeout", st.sink_detail[0], false);
  if (bits & kBitShaderMiss) WriteDump("shader_miss", st.sink_detail[1], false);
  if (bits & kBitCrashDiag) WriteDump("crash_diag", st.sink_detail[2], false);
  if (bits & kBitPipelineFail) WriteDump("pipeline_fail", st.sink_detail[3], false);
  std::vector<Pending> work;
  {
    std::lock_guard<std::mutex> lock(st.trig_mu);
    work.swap(st.pending);
  }
  for (auto& p : work) {
    const bool forced = p.sync || p.reason == "user" || p.reason.rfind("user", 0) == 0;
    std::string path = WriteDump(p.reason, p.detail, forced);
    if (p.sync) {
      std::lock_guard<std::mutex> lock(st.trig_mu);
      st.sync_results[p.id] = path;
      st.done_cv.notify_all();
    }
  }
}

void StreamFlush() {
  State& st = S();
  if (!st.stream) return;
  const int n = st.ring_count.load(std::memory_order_acquire);
  const std::vector<ThreadRow> host = TaskThreads();
  std::string header = "{\"types\":" + TypesJson() + ",\"strings\":" + StringsJson() + ",\"threads\":" +
                       ThreadsJson({}, host) + ",\"guest_threads\":" + GuestThreadsJson() + ",\"host_threads\":" +
                       HostThreadsJson(host) + "}";
  std::vector<RingCopy> blocks;
  for (int i = 0; i < n; ++i) {
    Ring* r = st.rings[i];
    if (!r) continue;
    const uint64_t head = r->head.load(std::memory_order_acquire);
    if (head == st.stream_pos[i]) continue;
    RingCopy c = CopyRing(r);
    // keep only records not streamed yet
    const uint64_t from = std::max(st.stream_pos[i], c.first);
    if (from > st.stream_pos[i]) st.stream_overruns += from - st.stream_pos[i];
    const uint64_t skip = from - c.first;
    if (skip >= c.ev.size()) { st.stream_pos[i] = head; continue; }
    c.ev.erase(c.ev.begin(), c.ev.begin() + long(skip));
    c.first = from;
    st.stream_pos[i] = c.first + c.ev.size();
    blocks.push_back(std::move(c));
  }
  if (blocks.empty()) return;
  const uint32_t len = uint32_t(header.size());
  std::fwrite("F2TC", 1, 4, st.stream);
  std::fwrite(&len, 4, 1, st.stream);
  std::fwrite(header.data(), 1, header.size(), st.stream);
  for (auto& c : blocks) {
    const uint32_t idx = c.index, count = uint32_t(c.ev.size());
    std::fwrite("RING", 1, 4, st.stream);
    std::fwrite(&idx, 4, 1, st.stream);
    std::fwrite(&c.first, 8, 1, st.stream);
    std::fwrite(&count, 4, 1, st.stream);
    std::fwrite(c.ev.data(), sizeof(Event), count, st.stream);
  }
  std::fflush(st.stream);
}

void LoadLevelNames() {
  State& st = S();
  fs::path root;
  if (const char* r = std::getenv("FABLE2_GAME_DATA_ROOT"); r && *r) root = r;
  else {
    char exe[1024] = {};
    uint32_t n = sizeof exe;
      if (_NSGetExecutablePath(exe, &n) == 0) root = fs::path(exe).parent_path();
  }
  std::ifstream f(root / "data/miscellaneous/levelconnectiongraph.txt");
  std::string line;
  while (std::getline(f, line)) {
    if (line.size() < 3 || line[0] != '"') continue;
    const size_t end = line.find('"', 1);
    if (end != std::string::npos) st.level_names.push_back(line.substr(1, end - 1));
  }
}

void RecorderMain() {
  State& st = S();
  pthread_setname_np("FlightRecorder");
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  st.recorder_port = mach_thread_self();
  RegisterThisThread();  // ring index 1 = sampler ring (registered first after the log ring)
  st.recorder_running.store(true);
  const uint64_t tpm = TicksPerMs();
  const int hz = st.cfg.sampler_hz;
  const uint64_t tick_us = hz > 0 ? uint64_t(1000000 / hz) : 20000;
  uint64_t last_detect = 0, last_second = 0, last_refresh = 0, last_ten = 0;
  uint64_t hang_level = 0;  // 0 none, 1 at hang_ms, 2 at 10 s, 3 at 30 s
  uint64_t gap_fired_for = 0;
  std::unordered_map<uint64_t, uint64_t> prev_cpu;
  bool heartbeat_logged = false, sha_done = false, selftest_av_done = !st.cfg.selftest_av;
  const bool hist = Level(kSampler) >= 3;
  uint64_t last_hist = 0, last_lr_top = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(st.trig_mu);
      st.trig_cv.wait_for(lock, std::chrono::microseconds(tick_us),
                          [&] { return st.trig_flag.load(std::memory_order_acquire); });
    }
    const uint64_t now = mach_absolute_time();
    if (now - last_refresh >= 1000 * tpm) {
      last_refresh = now;
      RefreshGuestThreads();
    }
    if (hz > 0) SampleTick(hist);
    HandleTriggers();
    if (now - last_detect >= 20 * tpm) {
      last_detect = now;
      LevelScan();  // delayed region scans
      // Frame-throttle skew watch (trace-only, 2026-10-08 boot hang). Present (82B69488) does
      // ++*(0x83496F44) and queues the GPU callback 82B89A78, which bumps stats[idx].count
      // (0x83491BD0 + 24*idx + 16, idx at 0x83496F48). Present then spins while
      // count - presents > 1, and nothing in that loop changes either value, so a skew of 3 or more
      // hangs the presenting thread. In normal play count <= presents. Log every new maximum of
      // count - presents >= 1 with the presenter threads' stacks.
      if (st.membase) {
        static int32_t skew_max = 0;
        uint32_t w[2] = {}, cnt = 0;
        if (SafeCopy(st.membase + 0x83496F44u, w, sizeof w)) {
          const uint32_t presents = __builtin_bswap32(w[0]), idx = __builtin_bswap32(w[1]);
          if (idx <= 1 && SafeCopy(st.membase + 0x83491BD0u + 24 * idx + 16, &cnt, 4)) {
            const int32_t skew = int32_t(__builtin_bswap32(cnt) - presents);
            if (skew >= 1 && skew > skew_max) {
              skew_max = skew;
              std::string who;
              for (auto& g : st.gthreads) {
                if (g.legacy_slot != 0 && g.legacy_slot != 1) continue;  // 3D Engine, GameThread
                if (!g.ref->is_running()) continue;
                const uint32_t lr = uint32_t(*reinterpret_cast<volatile uint64_t*>(&g.ctx->lr));
                const uint32_t sp = *reinterpret_cast<volatile uint32_t*>(&g.ctx->r1.u32);
                uint32_t chain[8];
                bool torn = false;
                const int n = WalkOther(g, sp, chain, 8, &torn);
                who += " | " + g.name + " " + Hex32(lr);
                for (int k = 0; k < n; ++k) who += " <" + Hex32(chain[k]);
              }
              REXSYS_WARN("[trace] frame-throttle skew {} (callbacks {} > presents {}, idx {}) at frame {}{}", skew,
                          __builtin_bswap32(cnt), presents, idx, g_frame.load(), who);
            }
          }
        }
      }
      // Fresh clock: sampling and dumps above take time, and a swap may have landed meanwhile.
      const uint64_t now = mach_absolute_time();
      const uint64_t swap = st.last_swap_t.load(std::memory_order_relaxed);
      const uint64_t paint = st.last_paint_t.load(std::memory_order_relaxed);
      const bool suppress = !st.background_test && st.tag_val[kTagFocus].load(std::memory_order_relaxed) != 1;
      if (swap && swap < now && !suppress) {
        const uint64_t since = now - swap;
        if (since >= uint64_t(st.cfg.gap_ms) * tpm && gap_fired_for != swap && !st.in_level_load.load()) {
          gap_fired_for = swap;
          char d[96];
          std::snprintf(d, sizeof d, "no guest swap for %.0f ms (frame %u)", TicksToMs(since), g_frame.load());
          WriteDump("frame_gap", d, false);
        }
        // The presenter repaints the last frame while the guest is stuck, so paint must not mask a
        // swap stall (2026-10-08 boot hang: 47 s without a swap, no hang dump). Paint age goes in the detail.
        const uint64_t since_paint = paint && paint < now ? now - paint : paint ? 0 : since;
        const uint64_t stall = since;
        static const uint64_t steps[3] = {0, 10000, 30000};
        uint64_t want = 0;
        if (stall >= uint64_t(st.cfg.hang_ms) * tpm) want = 1;
        if (stall >= steps[1] * tpm) want = 2;
        if (stall >= steps[2] * tpm) want = 3;
        if (want > hang_level) {
          hang_level = want;
          char d[96];
          std::snprintf(d, sizeof d, "no guest swap for %.0f ms (last paint %.0f ms ago)", TicksToMs(stall),
                        TicksToMs(since_paint));
          WriteDump("hang", d, true);
        } else if (want == 0) {
          hang_level = 0;
        }
      }
      // AV storm: > 100 AV lines in one second.
      static uint64_t av_prev = 0, av_t = 0;
      if (now - av_t >= 1000 * tpm) {
        const uint64_t c = st.av_count.load(std::memory_order_relaxed);
        if (av_t && c - av_prev > 100) {
          char d[96];
          std::snprintf(d, sizeof d, "%llu guest AV lines in the last second", (unsigned long long)(c - av_prev));
          WriteDump("av_storm", d, false);
        }
        av_prev = c;
        av_t = now;
      }
      if (!selftest_av_done) {
        const uint64_t first = st.first_swap_t.load();
        if (first && TicksToMs(now - first) >= st.cfg.selftest_av_at * 1000.0) {
          selftest_av_done = true;
          SetTag(kTagSelfTest, 2, "av");
          REXSYS_ERROR("[trace-selftest] Unhandled guest access violation: read of guest 0xDEADBEEF (host "
                       "0x0000000000000000) on thread 0x0 (synthetic, FABLE2_TRACE_SELFTEST=av)");
        }
      }
    }
    if (hist && now - last_hist >= 1000 * tpm) {
      if (last_hist) LogStackHist(TicksToMs(now - st.start.mach) / 1000.0);
      last_hist = now;
    }
    if (hist && On(kGpu, 3) && now - last_lr_top >= 10000 * tpm) {
      if (last_lr_top) LogLrTop();
      last_lr_top = now;
    }
    if (now - last_second >= 1000 * tpm) {
      last_second = now;
      FlushRepeats();
      FlushDigest(false);
      PollTagsIn();
      StreamFlush();
      if (On(kCpu)) {
        const auto rows = TaskThreads();
        for (auto& r : rows) {
          uint64_t& p = prev_cpu[r.tid];
          if (p && r.cpu_ns > p) Emit(kTCpu, uint32_t(r.tid), uint32_t(r.tid >> 32), r.cpu_ns - p, 0, now);
          p = r.cpu_ns;
        }
      }
      if (!sha_done && TicksToMs(now - st.start.mach) > 5000) {
        sha_done = true;
        char exe[1024] = {};
        uint32_t n = sizeof exe;
              if (_NSGetExecutablePath(exe, &n) == 0) st.binary_sha = Sha256File(exe);
      }
      const uint64_t first = st.first_swap_t.load();
      if (!heartbeat_logged && first && TicksToMs(now - first) > 30000) {
        heartbeat_logged = true;
        std::string fired, never;
        {
          std::lock_guard<std::mutex> lock(st.hook_mu);
          for (size_t i = 0; i < st.hooks.size(); ++i)
            (g_hook_fired[i].load() ? fired : never) += " " + st.hooks[i];
        }
        REXSYS_INFO("[trace] heartbeat 30 s after the first frame: fired:{}; never fired:{}", fired,
                    never.empty() ? " (none)" : never);
        WriteFileAtomic(st.cfg.dir / "session.json", SessionJson());
      }
    }
    if (now - last_ten >= 10000 * tpm) {
      last_ten = now;
      WriteFileAtomic(st.cfg.dir / "session.json", SessionJson());
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

void Init() {
  State& st = S();
  static bool once = false;
  if (once) return;
  once = true;
  for (auto& f : g_hook_fired) f.store(0);
  for (int i = 0; i < kTagCount; ++i) {
    st.tag_val[i].store(0);
    st.tag_str[i].store(-1);
  }
  st.tag_val[kTagFocus].store(1);  // a normal launch starts focused
  st.dummy.buf = st.dummy_buf;
  st.dummy.mask = 15;
  st.dummy.index = 255;
  uint64_t levels = 0;
  ParseSpec(st.cfg, levels);
  g_levels.store(st.cfg.active ? levels : 0, std::memory_order_relaxed);
  st.start = TakeClockPair();
  // Built-in types.
  auto def = [&](Type id, const char* name, Cat cat, const char* a, const char* b, const char* v) {
    st.types[id] = TypeInfo{name, kCatNames[cat], a, b, v, uint8_t(cat)};
    st.type_by_name[name] = id;
  };
  def(kTStack, "STACK", kSampler, "", "", "");
  def(kTText, "TEXT", kLog, "", "", "");
  def(kTSwap, "swap", kFrame, "frame", "", "interval_ticks");
  def(kTGap, "gap", kFrame, "frame", "in_level_load", "gap_ticks");
  def(kTSample, "sample", kSampler, "lr", "thread|state<<16", "sp");
  def(kTSampleRepeat, "sample_repeat", kSampler, "thread", "count", "");
  def(kTWait, "wait", kWait, "caller", "result", "dur_ticks");
  def(kTDelay, "delay", kWait, "caller", "requested_ms", "dur_ticks");
  def(kTPollWait, "poll_wait", kWait, "obj", "poll_fn", "dur_ticks");
  def(kTSetEvent, "set_event", kSignal, "r3", "", "");
  def(kTLevelLoad, "level_load", kLoad, "r3", "n", "dur_ticks");
  def(kTWorker0, "worker0", kWorker0, "caller", "bootloader|game<<1", "dur_ticks");
  def(kTSaveNotice, "save_notice", kSave, "requested_ms", "sleep_ms", "");
  def(kTContentClose, "content_close", kSave, "result", "", "dur_ticks");
  def(kTOpen, "open", kIo, "handle", "status", "");
  def(kTLog, "log", kLog, "level", "tid", "pos");
  def(kTTag, "tag", kTag, "tag", "string_id", "value");
  def(kTMark, "mark", kMark, "", "", "");
  def(kTTrigger, "trigger", kMark, "", "", "");
  def(kTCpu, "cpu", kCpu, "tid_lo", "tid_hi", "cpu_ns");
  def(kTThreadExit, "thread_exit", kSampler, "guest_tid", "", "");
  def(kTPaint, "paint", kFrame, "", "", "");
  def(kTRead, "read", kIo, "handle", "bytes", "dur_ticks");
  def(kTLongWait, "long_wait", kWait, "caller", "result", "dur_ticks");
  def(kTLevelArg, "level_arg", kLoad, "reg", "", "");
  if (!st.cfg.active) {
    REXSYS_INFO("[trace] flight recorder off (FABLE2_TRACE=off)");
    return;
  }
  // trace/ next to the main log (same rule as the Metal frame CSV).
  fs::path log = rex::cvar::GetFlagByName("log_file");
  fs::path folder = log.empty() ? fs::current_path() / "logs" : log.parent_path();
  if (folder.empty()) folder = fs::current_path();
  const char* dir_env = std::getenv("FABLE2_TRACE_DIR");  // override (tests)
  st.cfg.dir = dir_env && *dir_env ? fs::path(dir_env) : folder / "trace";
  std::error_code ec;
  // A shared log folder (title-mode launches) keeps one previous generation.
  if (fs::exists(st.cfg.dir / "session.json", ec)) {
    fs::remove_all(st.cfg.dir.string() + "-prev", ec);
    fs::rename(st.cfg.dir, st.cfg.dir.string() + "-prev", ec);
  }
  fs::create_directories(st.cfg.dir / "dumps", ec);
  if (ec) {
    REXSYS_WARN("[trace] cannot create {}; flight recorder off", st.cfg.dir.string());
    st.cfg.active = false;
    g_levels.store(0);
    return;
  }
  st.background_test = [] { const char* v = std::getenv("FABLE2_TEST_BACKGROUND"); return v && *v == '1'; }();
  st.digest = new DigestEntry[kDigestSize];
  {
    std::lock_guard<std::mutex> lock(st.reg_mu);
    g_log_ring = NewRing(kLogRing, 16384, "log");
    st.rings[kLogRing] = g_log_ring;
    st.ring_count.store(1, std::memory_order_release);
  }
  bool stream = false;
  for (int c = 0; c < kCatCount; ++c) stream |= Level(Cat(c)) >= 2;
  if (stream) st.stream = std::fopen((st.cfg.dir / "stream.f2t").c_str(), "wb");
  if (st.stream) {
    const std::string hdr = "{\"format\":\"f2t-stream\",\"version\":1,\"timebase\":{\"numer\":" +
                            std::to_string(Timebase().numer) + ",\"denom\":" + std::to_string(Timebase().denom) +
                            "},\"session_start\":" + ClockJson(st.start) + "}";
    const uint32_t len = uint32_t(hdr.size());
    std::fwrite("F2TS", 1, 4, st.stream);
    std::fwrite(&len, 4, 1, st.stream);
    std::fwrite(hdr.data(), 1, hdr.size(), st.stream);
  }
  LoadLevelNames();
  WriteFileAtomic(st.cfg.dir / "session.json", SessionJson());
  std::string sel = st.cfg.selftest_gap || st.cfg.selftest_av ? " SELFTEST:" : "";
  if (st.cfg.selftest_gap) sel += " gap@" + std::to_string(int(st.cfg.selftest_gap_at)) + "s";
  if (st.cfg.selftest_av) sel += " av@" + std::to_string(int(st.cfg.selftest_av_at)) + "s";
  REXSYS_INFO("[trace] flight recorder on: {} (FABLE2_TRACE='{}'{}{}; gap {} ms, hang {} ms, sampler {} Hz){}",
              st.cfg.dir.string(), st.cfg.spec, st.cfg.aliases.empty() ? "" : "; aliases: ", st.cfg.aliases,
              st.cfg.gap_ms, st.cfg.hang_ms, st.cfg.sampler_hz, sel);
  std::thread(RecorderMain).detach();
  // Wait until the recorder registered the sampler ring (index 1) so thread rings start at 2.
  for (int i = 0; i < 200 && !st.recorder_running.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void InstallLogSink() {
  if (!S().cfg.active) return;
  static bool once = false;
  if (once) return;
  once = true;
  rex::AddSink(std::make_shared<FrSink>());
}

void OnPostSetup() {}

// Region: the level-load call's arguments carry no string. The recorder thread scans the loading
// thread's stack (sp .. sp+64 KB, frames that stay put while the call runs) and follows guest data
// pointers two levels (128 bytes at level 1, 64 at level 2) for printable ASCII or UTF-16BE runs that
// equal a level name of levelconnectiongraph.txt. All reads are fault-safe; at most ~24k reads per
// level load, on the recorder thread (utility QoS), never on the game thread.
namespace {
struct LevelHit { std::string name; std::string where; int depth = 0; };
void ScanBuffer(const uint8_t* p, size_t n, const std::unordered_map<std::string, std::string>& lower,
                const std::string& where, std::vector<LevelHit>& hits, int depth) {
  auto check = [&](std::string run) {
    if (run.size() < 4) return;
    for (char& c : run) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto it = lower.find(run);
    if (it == lower.end()) return;
    for (auto& h : hits)
      if (h.name == it->second) return;
    hits.push_back({it->second, where, depth});
  };
  std::string a, w;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = p[i];
    if (std::isalnum(c) || c == '_' || c == '\\') a.push_back(char(c));
    else { check(a); a.clear(); }
  }
  check(a);
  for (size_t i = 0; i + 1 < n; i += 2) {  // UTF-16BE
    const uint8_t hi = p[i], c = p[i + 1];
    if (hi == 0 && (std::isalnum(c) || c == '_' || c == '\\')) w.push_back(char(c));
    else { check(w); w.clear(); }
  }
  check(w);
}
bool DataPtr(uint32_t v) { return (v >= 0x40000000u && v < 0x70000000u) || (v >= 0xA0000000u && v < 0xE0000000u); }
}  // namespace

std::vector<std::pair<std::string, std::string>> ScanLevelNames(const uint8_t* membase, uint32_t sp, uint32_t hi) {
  State& st = S();
  std::vector<std::pair<std::string, std::string>> result;
  if (st.level_names.empty() || !membase) return result;
  static const std::unordered_map<std::string, std::string> lower = [&] {
    std::unordered_map<std::string, std::string> m;
    for (const auto& n : st.level_names) {
      std::string l = n;
      for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
      m[l] = n;
    }
    return m;
  }();
  std::vector<LevelHit> hits;
  int reads = 0;
  constexpr int kMaxReads = 24000;
  const uint32_t end = std::min<uint64_t>(uint64_t(sp) + 0x10000, hi ? hi : uint64_t(sp) + 0x10000);
  std::vector<uint8_t> stack(end > sp ? end - sp : 0);
  for (uint32_t off = 0; off < stack.size(); off += 4096) {
    const uint32_t n = std::min<uint32_t>(4096, uint32_t(stack.size()) - off);
    if (!SafeCopy(membase + sp + off, stack.data() + off, n)) { stack.resize(off); break; }
    ++reads;
  }
  char where[64];
  ScanBuffer(stack.data(), stack.size(), lower, "stack", hits, 0);
  std::vector<std::pair<uint32_t, uint32_t>> level1;  // (pointer, stack offset)
  for (size_t k = 0; k + 4 <= stack.size(); k += 4) {
    uint32_t v;
    std::memcpy(&v, stack.data() + k, 4);
    v = __builtin_bswap32(v);
    if (DataPtr(v)) level1.push_back({v, uint32_t(k)});
  }
  uint8_t obj[128];
  std::vector<std::pair<uint32_t, uint32_t>> level2;
  for (auto [ptr, off] : level1) {
    if (reads >= kMaxReads) break;
    ++reads;
    if (!SafeCopy(membase + ptr, obj, sizeof obj)) continue;
    std::snprintf(where, sizeof where, "*(sp+0x%X)", off);
    ScanBuffer(obj, sizeof obj, lower, where, hits, 1);
    for (int k = 0; k < 32; ++k) {
      uint32_t v;
      std::memcpy(&v, obj + 4 * k, 4);
      v = __builtin_bswap32(v);
      if (DataPtr(v) && v != ptr) level2.push_back({v, (off << 8) | uint32_t(4 * k)});
    }
  }
  for (auto [ptr, tag] : level2) {
    if (reads >= kMaxReads) break;
    ++reads;
    if (!SafeCopy(membase + ptr, obj, 64)) continue;
    std::snprintf(where, sizeof where, "*(*(sp+0x%X)+0x%X)", tag >> 8, tag & 0xFF);
    ScanBuffer(obj, 64, lower, where, hits, 2);
  }
  for (auto& h : hits) result.push_back({h.name, h.where + (h.depth ? "" : "")});
  for (size_t i = 0; i < hits.size(); ++i) result[i].second = std::to_string(hits[i].depth) + ":" + hits[i].where;
  return result;
}

bool KnownLevelName(const char* s) {
  const State& st = S();
  if (!s || !*s) return false;
  if (st.level_names.empty()) return true;
  for (const auto& n : st.level_names)
    if (strcasecmp(n.c_str(), s) == 0) return true;
  return false;
}

void OnCloseRequested(const char* why) {
  State& st = S();
  if (!st.cfg.active || st.exit_dumped) return;
  st.exit_dumped = true;
  DumpSync("exit", why, 3000);
}

}  // namespace fable2::fr

// ---------------------------------------------------------------- C API for the Metal plugin

namespace {
using namespace fable2::fr;

int ApiLevel(const char* cat) {
  if (!cat) return 0;
  for (int c = 0; c < kCatCount; ++c)
    if (std::strcmp(cat, kCatNames[c]) == 0) return Level(Cat(c));
  return 0;
}
void ApiEmit(uint16_t type, uint32_t a, uint32_t b, uint64_t v, uint64_t t) {
  if (type && Active()) Emit(type, a, b, v, 0, t);
}
void ApiTrigger(const char* reason, const char* detail) { Trigger(reason, detail); }
int ApiDumpSync(const char* reason, const char* detail, char* out, size_t out_size, uint32_t timeout_ms) {
  State& st = S();
  if (reason && std::strcmp(reason, "exit") == 0) {  // one exit dump per process (close + window close)
    if (st.exit_dumped) return 0;
    st.exit_dumped = true;
  }
  const std::string p = DumpSync(reason, detail, timeout_ms ? timeout_ms : 10000);
  if (out && out_size) std::snprintf(out, out_size, "%s", p.c_str());
  return p.empty() ? 0 : 1;
}
void ApiMark(const char* text) {
  if (!Active() || !text) return;
  EmitText(kTMark, 0, 0, 0, text, std::strlen(text));
  REXSYS_INFO("[trace] mark: {}", text);
}
void ApiTag(const char* kv) {
  if (!Active() || !kv) return;
  const char* eq = std::strchr(kv, '=');
  if (!eq) return;
  const std::string k(kv, eq), v(eq + 1);
  for (int i = 0; i < kTagCount; ++i)
    if (k == kTagNames[i]) {
      char* end = nullptr;
      const long long n = std::strtoll(v.c_str(), &end, 10);
      if (!v.empty() && end && !*end) SetTag(Tag(i), n);
      else SetTag(Tag(i), 1, v.c_str());
    }
}
size_t ApiStateJson(char* out, size_t out_size) {
  State& st = S();
  double fps = 0;
  {
    const uint32_t pos = st.swap_pos.load();
    double sum = 0;
    int n = 0;
    for (uint32_t i = 0; i < 60 && i < pos; ++i) {
      const uint64_t dt = st.swap_intervals[(pos - 1 - i) & 255];
      if (dt) sum += TicksToMs(dt), ++n;
    }
    if (n && sum > 0) fps = 1000.0 * n / sum;
  }
  const uint64_t last = st.last_swap_t.load();
  const std::string s = "{\"active\":" + std::string(st.cfg.active ? "true" : "false") + ",\"frame\":" +
                        std::to_string(g_frame.load()) + ",\"fps\":" + std::to_string(fps) + ",\"ms_since_swap\":" +
                        std::to_string(last ? TicksToMs(mach_absolute_time() - last) : -1) + ",\"last_gap_ms\":" +
                        std::to_string(TicksToMs(st.last_gap_ticks.load())) + ",\"in_level_load\":" +
                        (st.in_level_load.load() ? "true" : "false") + ",\"level_loads\":" +
                        std::to_string(st.level_loads.load()) + ",\"load_end_frame\":" +
                        std::to_string(st.load_end_frame.load()) + ",\"loading_clear_frame\":" +
                        std::to_string(st.loading_clear_frame.load()) + ",\"dumps\":" + std::to_string(st.dumps) +
                        ",\"av_lines\":" + std::to_string(st.av_count.load()) + ",\"tags\":" + TagsJson() +
                        ",\"trace_dir\":" + Q(st.cfg.dir.string()) + "}";
  if (out && out_size) std::snprintf(out, out_size, "%s", s.c_str());
  return s.size();
}
int ApiHookId(const char* name) { return HookId(name); }
void ApiFired(int id) { Fired(id); }
void ApiSamplerCounts(int slot, uint32_t* total, uint32_t* wait) {
  State& st = S();
  if (slot < 0 || slot > 2) return;
  if (total) *total = st.legacy_total[slot].exchange(0, std::memory_order_relaxed);
  if (wait) *wait = st.legacy_wait[slot].exchange(0, std::memory_order_relaxed);
}
const char* ApiTraceDir() {
  static const std::string d = S().cfg.dir.string();
  return d.c_str();
}
uint32_t ApiFrameId() { return g_frame.load(std::memory_order_relaxed); }
uint32_t ApiFrameSwap() { return FrameSwap(); }
void ApiPaint() { Paint(); }
uint16_t ApiTypeId(const char* name, const char* cat, const char* a, const char* b, const char* v) {
  return RegisterType(name, cat, a, b, v);
}
}  // namespace

extern "C" __attribute__((visibility("default"), used)) const Fable2FrApi* fable2_fr_api(void) {
  static const Fable2FrApi api = {
      FABLE2_FR_API_VERSION, sizeof(Fable2FrApi), ApiLevel, ApiTypeId, ApiEmit, ApiFrameSwap, ApiPaint,
      ApiTrigger, ApiDumpSync, ApiMark, ApiTag, ApiStateJson, ApiHookId, ApiFired, ApiSamplerCounts,
      ApiTraceDir, ApiFrameId};
  return &api;
}
