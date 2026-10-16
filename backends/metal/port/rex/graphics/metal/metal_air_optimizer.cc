// See metal_air_optimizer.h and docs/native-shaders.md.

#include "rex/graphics/metal/metal_air_optimizer.h"

#include <fcntl.h>
#include <pthread/qos.h>
#include <pthread/spawn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "xxhash.h"
#include "rex/logging.h"

extern char** environ;

namespace rex {
namespace graphics {
namespace metal {

namespace {

struct Tools {
  bool available = false;
  // No tools, but FABLE2_NS_CACHE_TOOLCHAIN names the toolchain a bundled
  // cache was optimized with (portable build, docs/portable-build.md): cached
  // optimized libraries are used, nothing new is optimized.
  bool cache_only = false;
  std::string metal_opt;
  std::string metallib;
  uint64_t toolchain_hash = 0;
};

// One per-user scratch folder ($TMPDIR is per-user on macOS). Files are named
// <pid>-<n>.*; the folder exists only while optimizations run (created on the
// first in-flight job, removed when the last one ends). Files of a process
// that exited mid-job are removed at the next startup.
std::filesystem::path TempDir() {
  static const std::filesystem::path dir = [] {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
      base = "/tmp";
    }
    return base / "fable2-native-shaders";
  }();
  return dir;
}

std::mutex g_temp_mutex;
uint32_t g_temp_in_flight = 0;

bool PidAlive(long pid) {
  return pid > 0 && (kill(pid_t(pid), 0) == 0 || errno == EPERM);
}

// Removes leftovers of earlier processes: stale <pid>-* files in the shared
// folder and the per-pid folders milestone 1 used (fable2-native-shaders-<pid>).
void CleanStaleTemp() {
  std::error_code ec;
  const std::filesystem::path dir = TempDir();
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    long pid = std::strtol(name.c_str(), nullptr, 10);
    if (pid != long(getpid()) && !PidAlive(pid)) {
      std::error_code remove_ec;
      std::filesystem::remove(entry.path(), remove_ec);
    }
  }
  std::error_code remove_ec;
  std::filesystem::remove(dir, remove_ec);  // only when empty
  const std::string old_prefix = "fable2-native-shaders-";
  for (const auto& entry :
       std::filesystem::directory_iterator(dir.parent_path(), ec)) {
    const std::string name = entry.path().filename().string();
    if (name.compare(0, old_prefix.size(), old_prefix) != 0) {
      continue;
    }
    long pid = std::strtol(name.c_str() + old_prefix.size(), nullptr, 10);
    if (pid != long(getpid()) && !PidAlive(pid)) {
      std::error_code rm_ec;
      std::filesystem::remove_all(entry.path(), rm_ec);
    }
  }
}

std::atomic<bool> g_lookup_done{false};

// Runs a tool and returns its stdout (empty on failure). posix_spawn, not
// popen: forking the game process (large address space) is slow.
std::string Capture(const std::vector<std::string>& args) {
  int fds[2];
  if (pipe(fds) != 0) {
    return {};
  }
  std::vector<char*> argv;
  for (const std::string& a : args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addclose(&actions, fds[1]);
  pid_t pid = 0;
  int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  std::string out;
  if (rc == 0) {
    char buffer[1024];
    ssize_t n;
    while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) {
      out.append(buffer, size_t(n));
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      out.clear();
    }
  }
  close(fds[0]);
  return out;
}

// True when a developer directory exists, so /usr/bin/xcrun can answer
// without prompting. FABLE2_NS_ASSUME_NO_XCODE=1 simulates a Mac without
// Xcode or the command-line tools (testing, docs/portable-build.md).
bool HaveDeveloperTools() {
  const char* assume = std::getenv("FABLE2_NS_ASSUME_NO_XCODE");  // getenv-ok: toolchain discovery (compile worker)
  if (assume && assume[0] == '1') {
    return false;
  }
  const char* developer_dir = std::getenv("DEVELOPER_DIR");  // getenv-ok: toolchain discovery (compile worker)
  if (developer_dir && developer_dir[0] && access(developer_dir, F_OK) == 0) {
    return true;
  }
  // xcode-select's choice (a symlink), else its defaults.
  char target[4096];
  const ssize_t n =
      readlink("/var/db/xcode_select_link", target, sizeof(target) - 1);
  if (n > 0) {
    target[n] = 0;
    if (access(target, F_OK) == 0) {
      return true;
    }
  }
  return access("/Applications/Xcode.app/Contents/Developer", F_OK) == 0 ||
         access("/Library/Developer/CommandLineTools/usr/bin", F_OK) == 0;
}

std::string FirstLine(const std::string& text) {
  std::string line = text.substr(0, text.find('\n'));
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
    line.pop_back();
  }
  return line;
}

const Tools& GetTools() {
  static Tools tools;
  static std::once_flag once;
  std::call_once(once, [] {
    struct Done {
      ~Done() { g_lookup_done.store(true, std::memory_order_release); }
    } done;
    auto start = std::chrono::steady_clock::now();
    CleanStaleTemp();
    // FABLE2_NS_TOOL_DIR=<dir>: take metal-opt/metallib from <dir> instead of
    // xcrun (testing: a missing dir simulates a missing Metal Toolchain).
    const char* tool_dir = std::getenv("FABLE2_NS_TOOL_DIR");  // getenv-ok: toolchain discovery (compile worker)
    const char* why = "Metal Toolchain";
    if (tool_dir && tool_dir[0]) {
      tools.metal_opt = (std::filesystem::path(tool_dir) / "metal-opt").string();
      tools.metallib = (std::filesystem::path(tool_dir) / "metallib").string();
    } else if (!HaveDeveloperTools()) {
      // Without Xcode or the command-line tools, /usr/bin/xcrun is a stub
      // that opens Apple's "install developer tools" dialog: don't run it.
      why = "no Xcode or command-line tools installed";
    } else {
      tools.metal_opt =
          FirstLine(Capture({"/usr/bin/xcrun", "--find", "metal-opt"}));
      tools.metallib =
          FirstLine(Capture({"/usr/bin/xcrun", "--find", "metallib"}));
    }
    if (tools.metal_opt.empty() || tools.metallib.empty() ||
        access(tools.metal_opt.c_str(), X_OK) != 0 ||
        access(tools.metallib.c_str(), X_OK) != 0) {
      const char* seeded = std::getenv("FABLE2_NS_CACHE_TOOLCHAIN");  // getenv-ok: toolchain discovery (compile worker)
      char* end = nullptr;
      const uint64_t hash =
          seeded && seeded[0] ? std::strtoull(seeded, &end, 16) : 0;
      if (hash && end && *end == 0) {
        tools.toolchain_hash = hash;
        tools.cache_only = true;
        REXLOG_WARN(
            "[native-shaders] metal-opt/metallib not found ({}); optimized "
            "pixel shaders come only from the cache (toolchain {:016X}), "
            "others stay plain",
            why, hash);
        return;
      }
      REXLOG_WARN(
          "[native-shaders] metal-opt/metallib not found ({}); "
          "optimized shader path unavailable, plain shaders are used",
          why);
      return;
    }
    std::string version = Capture({tools.metal_opt, "--version"});
    tools.toolchain_hash =
        XXH3_64bits(version.data(), version.size()) ^ UINT64_C(0x4E53314F32);
    tools.available = true;
    REXLOG_INFO("[native-shaders] AIR optimizer: {} (toolchain {:016X}, "
                "lookup {} ms, scratch {})",
                tools.metal_opt, tools.toolchain_hash,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count(),
                TempDir().string());
  });
  return tools;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    return false;
  }
  out.clear();
  uint8_t buffer[65536];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
    out.insert(out.end(), buffer, buffer + n);
  }
  std::fclose(f);
  return !out.empty();
}

bool WriteFile(const std::filesystem::path& path, const uint8_t* data,
               size_t size) {
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return false;
  }
  bool ok = std::fwrite(data, 1, size, f) == size;
  return std::fclose(f) == 0 && ok;
}

// posix_spawn + wait with a timeout; stdout/stderr go to /dev/null.
bool Run(const std::vector<std::string>& args, std::string* error) {
  std::vector<char*> argv;
  for (const std::string& a : args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  // The tools run at utility QoS so they never compete with the game threads.
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_set_qos_class_np(&attr, QOS_CLASS_UTILITY);
  pid_t pid = 0;
  int rc = posix_spawn(&pid, argv[0], &actions, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    if (error) *error = "spawn failed: " + std::string(std::strerror(rc));
    return false;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  int status = 0;
  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      break;
    }
    if (r < 0) {
      if (error) *error = "waitpid failed";
      return false;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      if (error) *error = "timeout";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (error) *error = args[0] + " exited with status " + std::to_string(status);
    return false;
  }
  return true;
}

}  // namespace

bool ConverterBuiltIn() {
#if FABLE2_NO_CONVERTER
  return false;
#else
  return true;
#endif
}

bool NoConverterForcesNative() {
  // FABLE2_NOCONV_ALLOW_OFF=1 (diagnostic only, no launcher toggle): honour
  // the native-path switches in a no-converter build anyway, to test how the
  // renderer copes with refused draws.
  static const bool force = [] {
    if (ConverterBuiltIn()) {
      return false;
    }
    const char* v = std::getenv("FABLE2_NOCONV_ALLOW_OFF");
    return !(v && v[0] == '1');
  }();
  return force;
}

namespace {

// docs/no-converter.md "Forced switches": in a build without the converter
// these native paths are the only shader paths, so turning one off would
// refuse every draw it covers (black frames). Returns the value to use: the
// environment value, or nullptr (the default, i.e. on) when the value turns
// the path off in a no-converter build. Logs one line per switch.
const char* NativeSwitchValue(const char* name) {
  const char* v = std::getenv(name);
  if (!v || v[0] != '0' || !NoConverterForcesNative()) {
    return v;
  }
  REXLOG_WARN("[no-converter] {}={} ignored: no converter fallback in this "
              "build",
              name, v);
  return nullptr;
}

}  // namespace

uint32_t NativeShadersMode() {
  static const uint32_t mode = [] {
    const char* v = NativeSwitchValue("FABLE2_NATIVE_SHADERS");
    if (v && v[0]) {
      return v[0] == '0' ? 0u : 1u;
    }
    return kNativeShadersDefault;
  }();
  return mode;
}

bool NativeShadersEmuPipelines() {
  static const bool on = [] {
    if (!NativeShadersMode()) {
      return false;
    }
    const char* v = std::getenv("FABLE2_NS_EMU_PIPELINES");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool NativeXlateMode() {
  static const bool on = [] {
    if (!NativeShadersMode()) {
      return false;
    }
    // Default on since 2026-10-13 (docs/native-translator.md); =0 restores
    // the converter path.
    const char* v = NativeSwitchValue("FABLE2_NATIVE_XLATE");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool NativeFirstMode() {
  static const bool on = [] {
    if (!NativeXlateMode()) {
      if (!ConverterBuiltIn()) {
        REXLOG_ERROR("[no-converter] FABLE2_NATIVE_XLATE / FABLE2_NATIVE_SHADERS "
                     "are off (FABLE2_NOCONV_ALLOW_OFF=1), but this build has "
                     "no converter: every guest shader will fail");
      }
      return false;
    }
    const char* v = NativeSwitchValue("FABLE2_NATIVE_FIRST");
    const bool mode = !(v && v[0] == '0');
    REXLOG_INFO("[native-first] {} (FABLE2_NATIVE_FIRST={}; converter {})",
                mode ? "on: guest shaders are compiled from the native "
                       "translation directly"
                     : "off: converter library first, native swap later",
                v ? v : "unset", ConverterBuiltIn() ? "built in" : "not built");
    return mode;
  }();
  return on;
}

bool NativeXlateVertexMode() {
  static const bool on = [] {
    if (!NativeXlateMode()) {
      return false;
    }
    const char* v = NativeSwitchValue("FABLE2_NATIVE_XLATE_VS");
    return !(v && v[0] == '0');
  }();
  return on;
}

uint32_t NativeTessMode() {
  static const uint32_t mode = [] {
    if (!NativeXlateMode()) {
      return 0u;
    }
    // Default 1 (exact only) since 2026-10-13; =0 restores the emulation.
    const char* v = NativeSwitchValue("FABLE2_NATIVE_TESS");
    if (!v || !v[0]) {
      return 1u;
    }
    return v[0] == '3' ? 3u : (v[0] == '2' ? 2u : (v[0] == '0' ? 0u : 1u));
  }();
  return mode;
}

uint32_t NativePointsMode() {
  static const uint32_t mode = [] {
    if (!NativeXlateVertexMode()) {
      return 0u;
    }
    // Default 2 (mesh function, VS once per point) since 2026-10-13: exact
    // and not slower. =1 per-corner expansion (slower), =0 emulation.
    const char* v = NativeSwitchValue("FABLE2_NATIVE_POINTS");
    if (!v || !v[0]) {
      return 2u;
    }
    return v[0] == '0' ? 0u : (v[0] == '1' ? 1u : 2u);
  }();
  return mode;
}

bool NativePointsMemexportMode() {
  static const bool on = [] {
    if (NativePointsMode() != 2 || !NativeFirstMode()) {
      return false;
    }
    const char* v = NativeSwitchValue("FABLE2_NATIVE_POINTS_MEMEXPORT");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool NativeRectsMode() {
  static const bool on = [] {
    if (!NativeXlateVertexMode()) {
      return false;
    }
    // Default on since 2026-10-13 (bit-identical, neutral); =0 emulates.
    const char* v = NativeSwitchValue("FABLE2_NATIVE_RECTS");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool TerrainAdaptiveMode() {
  static const bool on = [] {
    if (!NativeTessMode()) {
      return false;
    }
    const char* v = std::getenv("FABLE2_TERRAIN_ADAPTIVE");
    return v && v[0] == '1';
  }();
  return on;
}

bool TerrainLayerMatchMode() {
  static const bool on = [] {
    if (!TerrainAdaptiveMode()) {
      return false;
    }
    // Default on (docs/adaptive-terrain-flashes-20261014.md); =0 leaves the
    // layers at the uniform maximum on Metal's tessellator, as before.
    const char* v = std::getenv("FABLE2_TERRAIN_LAYER_MATCH");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool NativeTessBlendedMode() {
  static const bool on = [] {
    if (!NativeTessMode()) {
      return false;
    }
    const char* v = NativeSwitchValue("FABLE2_NATIVE_TESS_BLENDED");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool NativeTessSnapUvMode() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_NATIVE_TESS_SNAP_UV");
    const bool enabled = v && v[0] == '1';
    // Confirms in the log that the switch reached the game (flash kit, docs/flash-kit.md); the translator
    // logs each domain shader compiled with the snap ([tess-trace] snap-uv ... compiled).
    REXLOG_INFO("[tess-trace] snap-uv: {} (FABLE2_NATIVE_TESS_SNAP_UV={})", enabled ? "on" : "off",
                v ? v : "unset");
    return enabled;
  }();
  return on;
}

bool EffectsFootprintMode() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_EFFECTS_FOOTPRINT");
    const bool enabled = !(v && v[0] == '0');
    REXLOG_INFO(
        "[effects-footprint] offset texture fetches cover their guest "
        "texel at scaled resolution: {} (FABLE2_EFFECTS_FOOTPRINT={})",
        enabled ? "on" : "off", v ? v : "unset");
    return enabled;
  }();
  return on;
}

bool EffectsFootprintVariantMode() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_EFFECTS_FOOTPRINT_VARIANT");
    const bool enabled = !(v && v[0] == '0');
    REXLOG_INFO(
        "[effects-footprint] footprint code only in pixel-shader variants of "
        "draws it can apply to: {} (FABLE2_EFFECTS_FOOTPRINT_VARIANT={})",
        enabled ? "on" : "off", v ? v : "unset");
    return enabled;
  }();
  return on;
}

const char* NativeXlateDumpDir() {
  static const char* dir = [] {
    const char* v = std::getenv("FABLE2_NATIVE_XLATE_DUMP");
    return (v && v[0]) ? v : nullptr;
  }();
  return dir;
}

bool NativeShaderKeepPlain(uint64_t ucode_hash) {
  static const std::vector<uint64_t> list = [] {
    // Measured (docs/native-shaders.md): -O2 makes these slower than plain.
    // 0F99810F072BF0FC: full-screen pass at Bower Lake, 3x: +12% pass time
    // with -O2 (1.24 -> 1.39 ms), +34% shader cost in the replay profile.
    std::vector<uint64_t> l = {UINT64_C(0x0F99810F072BF0FC)};
    const char* v = std::getenv("FABLE2_NS_KEEP_PLAIN");
    if (v) {
      l.clear();
      while (*v) {
        char* end = nullptr;
        uint64_t h = std::strtoull(v, &end, 16);
        if (end == v) {
          break;  // "none" or junk: nothing (more) kept plain
        }
        l.push_back(h);
        v = (*end == ',') ? end + 1 : end;
        if (*end != ',') {
          break;
        }
      }
    }
    return l;
  }();
  return std::find(list.begin(), list.end(), ucode_hash) != list.end();
}

bool AirOptimizerAvailable() { return GetTools().available; }

bool AirOptimizerCacheReadable() {
  const Tools& tools = GetTools();
  return tools.available || tools.cache_only;
}

bool AirOptimizerLookupDone() {
  return g_lookup_done.load(std::memory_order_acquire);
}

void StartAirOptimizerLookup() {
  std::thread([] {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    GetTools();
  }).detach();
}

uint64_t AirOptimizerToolchainHash() { return GetTools().toolchain_hash; }

bool OptimizeMetallibAir(const std::vector<uint8_t>& metallib,
                         std::vector<uint8_t>& optimized_out,
                         std::string* error) {
  const Tools& tools = GetTools();
  if (!tools.available) {
    if (error) *error = "toolchain unavailable";
    return false;
  }
  // The LLVM bitcode wrapper (magic 0x0B17C0DE, then version, offset, size).
  static const uint8_t kWrapper[4] = {0xDE, 0xC0, 0x17, 0x0B};
  size_t found = SIZE_MAX;
  for (size_t i = 0; i + 20 <= metallib.size(); ++i) {
    if (std::memcmp(&metallib[i], kWrapper, 4) == 0) {
      if (found != SIZE_MAX) {
        if (error) *error = "more than one module";
        return false;
      }
      found = i;
    }
  }
  if (found == SIZE_MAX) {
    if (error) *error = "no bitcode module";
    return false;
  }
  uint32_t offset = 0, size = 0;
  std::memcpy(&offset, &metallib[found + 8], 4);
  std::memcpy(&size, &metallib[found + 12], 4);
  if (uint64_t(found) + offset + size > metallib.size()) {
    if (error) *error = "bad bitcode wrapper";
    return false;
  }
  static std::atomic<uint64_t> counter{0};
  const std::filesystem::path dir = TempDir();
  const std::string stem =
      std::to_string(getpid()) + "-" +
      std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
  const std::filesystem::path in = dir / (stem + ".bc");
  const std::filesystem::path opt = dir / (stem + ".o2.bc");
  const std::filesystem::path lib = dir / (stem + ".metallib");
  {
    std::lock_guard<std::mutex> lock(g_temp_mutex);
    if (g_temp_in_flight++ == 0) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
    }
  }
  bool ok = WriteFile(in, &metallib[found], size_t(offset) + size);
  if (!ok && error) *error = "cannot write " + in.string();
  ok = ok && Run({tools.metal_opt, "-O2", in.string(), "-o", opt.string()},
                 error);
  ok = ok && Run({tools.metallib, opt.string(), "-o", lib.string()}, error);
  std::vector<uint8_t> result;
  ok = ok && ReadFile(lib, result);
  {
    std::lock_guard<std::mutex> lock(g_temp_mutex);
    std::error_code ec;
    std::filesystem::remove(in, ec);
    std::filesystem::remove(opt, ec);
    std::filesystem::remove(lib, ec);
    if (--g_temp_in_flight == 0) {
      std::filesystem::remove(dir, ec);  // only when empty
    }
  }
  if (!ok) {
    return false;
  }
  optimized_out = std::move(result);
  return true;
}

bool SpecializeMetallibAir(
    const std::vector<uint8_t>& metallib,
    const std::function<bool(std::string& ll, std::string* error)>& transform,
    std::vector<uint8_t>& specialized_out, std::string* error) {
  const Tools& tools = GetTools();
  if (!tools.available) {
    if (error) *error = "toolchain unavailable";
    return false;
  }
  static const uint8_t kWrapper[4] = {0xDE, 0xC0, 0x17, 0x0B};
  size_t found = SIZE_MAX;
  for (size_t i = 0; i + 20 <= metallib.size(); ++i) {
    if (std::memcmp(&metallib[i], kWrapper, 4) == 0) {
      if (found != SIZE_MAX) {
        if (error) *error = "more than one module";
        return false;
      }
      found = i;
    }
  }
  if (found == SIZE_MAX) {
    if (error) *error = "no bitcode module";
    return false;
  }
  uint32_t offset = 0, size = 0;
  std::memcpy(&offset, &metallib[found + 8], 4);
  std::memcpy(&size, &metallib[found + 12], 4);
  if (uint64_t(found) + offset + size > metallib.size()) {
    if (error) *error = "bad bitcode wrapper";
    return false;
  }
  static std::atomic<uint64_t> counter{0};
  const std::filesystem::path dir = TempDir();
  const std::string stem =
      std::to_string(getpid()) + "-s" +
      std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
  const std::filesystem::path in = dir / (stem + ".bc");
  const std::filesystem::path o2 = dir / (stem + ".o2.ll");
  const std::filesystem::path spec = dir / (stem + ".spec.ll");
  const std::filesystem::path spec_bc = dir / (stem + ".spec.bc");
  const std::filesystem::path lib = dir / (stem + ".metallib");
  {
    std::lock_guard<std::mutex> lock(g_temp_mutex);
    if (g_temp_in_flight++ == 0) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
    }
  }
  bool ok = WriteFile(in, &metallib[found], size_t(offset) + size);
  if (!ok && error) *error = "cannot write " + in.string();
  // -O2 first (hoists and merges the constant loads, as for NS1), then bake
  // the invariant loads into literals and run -O2 again.
  ok = ok && Run({tools.metal_opt, "-O2", "-S", in.string(), "-o", o2.string()},
                 error);
  std::vector<uint8_t> text;
  ok = ok && ReadFile(o2, text);
  std::string ll(text.begin(), text.end());
  ok = ok && transform(ll, error);
  ok = ok && WriteFile(spec, reinterpret_cast<const uint8_t*>(ll.data()),
                       ll.size());
  ok = ok && Run({tools.metal_opt, "-O2", spec.string(), "-o", spec_bc.string()},
                 error);
  ok = ok && Run({tools.metallib, spec_bc.string(), "-o", lib.string()}, error);
  std::vector<uint8_t> result;
  ok = ok && ReadFile(lib, result);
  {
    std::lock_guard<std::mutex> lock(g_temp_mutex);
    std::error_code ec;
    for (const auto& p : {in, o2, spec, spec_bc, lib}) {
      std::filesystem::remove(p, ec);
    }
    if (--g_temp_in_flight == 0) {
      std::filesystem::remove(dir, ec);  // only when empty
    }
  }
  if (!ok) {
    return false;
  }
  specialized_out = std::move(result);
  return true;
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
