// Flight recorder checks (built and run by the macOS build, see CMakeLists.txt):
//  1. the shared guest back-chain walker decides exactly like the old InBootLoader loop;
//  2. FABLE2_TRACE parsing with the old switches as aliases;
//  3. ring / stack / text records, a synchronous dump (ring.f2t, summary.json, stacks.txt);
//  4. the log sink: a guest-AV line produces a first_av dump and a log-digest row;
//  5. the per-event emit cost (reported; fails only if absurd).
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/logging/macros.h>

#include "fable2_flight_recorder.h"
#include "fable2_guest_stack.h"

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

// The pre-telemetry InBootLoader loop, verbatim apart from the load macro.
static bool OldInBootLoader(const uint8_t* base, uint32_t sp) {
  auto load = [base](uint32_t a) { return fable2::gstack::LoadOwn(base, a); };
  for (int i = 0; i < 24 && sp >= 0x40000000u && sp < 0xF0000000u && (sp & 3) == 0; ++i) {
    const uint32_t prev = load(sp);
    if (prev <= sp || prev - sp > 0x100000u) return false;
    const uint32_t lr = load(prev - 8);
    if (lr >= 0x822F33B8u && lr < 0x822F3638u) return true;
    if (lr < 0x82000000u || lr >= 0x84000000u) return false;
    sp = prev;
  }
  return false;
}
static bool NewInBootLoader(const uint8_t* base, uint32_t sp) {
  bool found = false;
  fable2::gstack::WalkBackChain(
      sp, 24, fable2::gstack::Range{},
      [base](uint32_t a, uint32_t* v) {
        *v = fable2::gstack::LoadOwn(base, a);
        return true;
      },
      [&found](uint32_t lr) {
        if (lr >= 0x822F33B8u && lr < 0x822F3638u) {
          found = true;
          return false;
        }
        return true;
      });
  return found;
}

static void TestWalker() {
  // A small fake guest window: addresses 0x70000000..0x70010000 map to buf.
  constexpr uint32_t kLo = 0x70000000u, kSize = 0x10000u;
  std::vector<uint8_t> mem(kSize + 64, 0);
  const uint8_t* base = mem.data() - kLo;  // base + addr lands in mem for addr in the window
  auto store = [&](uint32_t a, uint32_t v) {
    const uint32_t be = __builtin_bswap32(v);
    std::memcpy(mem.data() + (a - kLo), &be, 4);
  };
  std::mt19937 rng(1234);
  int agree = 0, boot_true = 0;
  for (int trial = 0; trial < 20000; ++trial) {
    std::fill(mem.begin(), mem.end(), 0);
    uint32_t sp = kLo + 0x100 + (rng() % 64) * 16;
    const int frames = 1 + int(rng() % 30);
    uint32_t cur = sp;
    for (int f = 0; f < frames && cur + 0x400 < kLo + kSize; ++f) {
      uint32_t prev = cur + 16 + (rng() % 48) * 16;
      const int kind = int(rng() % 100);
      if (kind == 0) prev = cur;                // bad chain (no growth)
      if (kind == 1) prev = cur + 0x200000u;    // bad chain (too far)
      store(cur, prev);
      uint32_t lr = 0x82100000u + (rng() % 0x100000u) * 4;
      if (kind < 6) lr = 0x822F33B8u + (rng() % 0xA0u) * 4;  // boot-loader range
      if (kind >= 6 && kind < 9) lr = 0x12345678u;           // not code
      if (prev + 8 < kLo + kSize && prev >= 8) store(prev - 8, lr);
      if (prev >= kLo + kSize - 0x400) break;
      cur = prev;
    }
    const bool a = OldInBootLoader(base, sp), b = NewInBootLoader(base, sp);
    agree += a == b;
    boot_true += a;
    CHECK(a == b);
    if (a != b) break;
  }
  std::printf("walker: %d/20000 random chains agree with the old InBootLoader (%d true)\n", agree, boot_true);
  CHECK(boot_true > 100);
}

static std::string ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main() {
  TestWalker();

  const fs::path dir = FABLE2_TRACE_TEST_DIR;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  setenv("FABLE2_TRACE_DIR", (dir / "trace").c_str(), 1);
  setenv("FABLE2_LOADING_TRACE", "1", 1);
  setenv("FABLE2_TRACE", "draw:0,gap=1500,shader:2", 1);
  setenv("FABLE2_TRACE_SAMPLER_HZ", "0", 1);
  rex::InitLogging(nullptr);

  fable2::fr::Init();
  fable2::fr::InstallLogSink();
  const Fable2FrApi* api = fable2_fr_api();
  CHECK(api && api->version == FABLE2_FR_API_VERSION && api->size == sizeof(Fable2FrApi));
  CHECK(api->level("load") == 3 && api->level("wait") == 3 && api->level("io") == 2);
  CHECK(api->level("sampler") == 3 && api->level("draw") == 0 && api->level("shader") == 2);
  CHECK(api->level("frame") == 1 && api->level("tex") == 0);
  CHECK(fs::exists(dir / "trace/session.json"));
  const std::string session = ReadAll(dir / "trace/session.json");
  CHECK(session.find("\"gap_ms\":1500") != std::string::npos);
  CHECK(session.find("FABLE2_LOADING_TRACE=1 -> load:3") != std::string::npos);

  // Records.
  const uint32_t frames[9] = {0x82000010, 0x82000020, 0x82000030, 0x82000040, 0x82000050,
                              0x82000060, 0x82000070, 0x82000080, 0x82000090};
  fable2::fr::EmitStack(fable2::fr::kTWait, 0x82CBD0A8u, 0x102, 12345, 0, frames, 9);
  const char* text = "game:\\data\\levels\\a_rather_long_path_name.bnk";
  fable2::fr::EmitText(fable2::fr::kTOpen, 7, 0, 0, text, std::strlen(text));
  fable2::fr::SetTag(fable2::fr::kTagRegion, 1, "BowerLake");
  for (int i = 0; i < 5; ++i) api->frame_swap();
  const uint16_t dyn = api->type_id("csv_test", "frame", "frame", "", "dur_us");
  CHECK(dyn >= fable2::fr::kTDynamic);
  api->emit(dyn, 1, 2, 3, 0);
  api->mark("test mark");
  char state[2048];
  api->state_json(state, sizeof state);
  CHECK(std::strstr(state, "\"region\":\"BowerLake\"") != nullptr);
  CHECK(std::strstr(state, "\"frame\":5") != nullptr);

  // Cost per event (single thread, ring hot in cache).
  const int kN = 2000000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) fable2::fr::Emit(fable2::fr::kTDelay, uint32_t(i), 0, 0);
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kN;
  std::printf("emit: %.1f ns per event\n", ns);
  CHECK(ns < 500.0);

  // Synchronous user dump.
  char path[1024] = {};
  CHECK(api->dump_sync("user", "unit test", path, sizeof path, 10000) == 1);
  const fs::path dump = path;
  CHECK(fs::exists(dump / "ring.f2t") && fs::exists(dump / "summary.json") && fs::exists(dump / "stacks.txt"));
  const std::string ring = ReadAll(dump / "ring.f2t");
  CHECK(ring.compare(0, 4, "F2T1") == 0);
  CHECK(ring.find("\"csv_test\"") != std::string::npos);
  CHECK(ring.find("BowerLake") != std::string::npos);
  CHECK(ReadAll(dump / "summary.json").find("\"reason\":\"user\"") != std::string::npos);

  // Log sink: first guest AV -> first_av dump + digest row (count 3 after three lines).
  for (int i = 0; i < 3; ++i)
    REXSYS_ERROR("Unhandled guest access violation: read of guest 0x{:08X} (host 0x0000000000000000) on thread 0x{:X}",
                 0x1000u + i, 7 + i);
  REXSYS_WARN("unit test warning {}", 42);
  bool av_dump = false;
  for (int i = 0; i < 300 && !av_dump; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    for (auto& e : fs::directory_iterator(dir / "trace/dumps", ec))
      av_dump |= e.path().filename().string().find("first_av") != std::string::npos;
  }
  CHECK(av_dump);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  const std::string digest = ReadAll(dir / "trace/log-digest.jsonl");
  CHECK(digest.find("Unhandled guest access violation") != std::string::npos);
  CHECK(digest.find("\"count\":3") != std::string::npos);
  CHECK(digest.find("unit test warning #") != std::string::npos);

  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "flight recorder tests passed", failures,
              failures == 1 ? "" : "s");
  std::fflush(stdout);
  std::_Exit(failures ? 1 : 0);  // the recorder thread is detached; skip static destructors
}
