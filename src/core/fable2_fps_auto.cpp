// See fable2_fps_auto.h.
#include "fable2_fps_auto.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "fable2_config.h"
#include "fable2_res_auto.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#endif

namespace fable2::fps {
namespace {

// Strict positive integer parse; false on empty/garbage.
bool ParsePositiveInt(const char* text, int& out) {
  if (!text || !*text) return false;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (!end || *end || value <= 0 || value > 100000) return false;
  out = static_cast<int>(value);
  return true;
}

}  // namespace

GpuInfo QueryGpu() {
  GpuInfo info;
#ifdef __APPLE__
  // AGXAccelerator* subclasses IOAccelerator; MACH_PORT_NULL = default main port.
  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOAccelerator"), &it) !=
      KERN_SUCCESS) {
    info.why = "IOServiceGetMatchingServices(IOAccelerator) failed";
  } else {
    bool any = false;
    for (io_object_t entry = IOIteratorNext(it); entry != IO_OBJECT_NULL;
         entry = IOIteratorNext(it)) {
      any = true;
      if (info.name.empty()) {
        if (CFTypeRef model = IORegistryEntryCreateCFProperty(entry, CFSTR("model"),
                                                              kCFAllocatorDefault, 0)) {
          char buf[256];
          if (CFGetTypeID(model) == CFStringGetTypeID() &&
              CFStringGetCString(static_cast<CFStringRef>(model), buf, sizeof(buf),
                                 kCFStringEncodingUTF8)) {
            info.name = buf;
          }
          CFRelease(model);
        }
      }
      if (info.cores < 0) {
        if (CFTypeRef count = IORegistryEntryCreateCFProperty(entry, CFSTR("gpu-core-count"),
                                                              kCFAllocatorDefault, 0)) {
          int n = 0;
          if (CFGetTypeID(count) == CFNumberGetTypeID() &&
              CFNumberGetValue(static_cast<CFNumberRef>(count), kCFNumberIntType, &n) && n > 0) {
            info.cores = n;
          }
          CFRelease(count);
        }
      }
      IOObjectRelease(entry);
    }
    IOObjectRelease(it);
    if (!any) info.why = "no IOAccelerator in the IORegistry";
    else if (info.cores < 0) info.why = "IOAccelerator has no gpu-core-count property";
  }
  if (info.name.empty()) {
    char buf[256] = {};
    size_t len = sizeof(buf) - 1;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) info.name = buf;
  }
#else
  info.why = "GPU core count is only read on macOS";
#endif
  return info;
}

namespace {

// The resolution scale that will run, for the estimate: an explicit 1/2/3
// (FABLE2_RES_SCALE, else [graphics] resolution_scale), or 1 when resolution is
// "auto" (it is decided after the frame target and never goes below 1x).
int PlannedScale(const char** source) {
  if (const char* env = std::getenv("FABLE2_RES_SCALE"); env && *env) {
    if (strcasecmp(env, "auto") == 0) {
      *source = "resolution auto (decided after this)";
      return 1;
    }
    int v = 0;
    if (ParsePositiveInt(env, v)) {
      *source = "FABLE2_RES_SCALE";
      return v > 3 ? 3 : v;
    }
  }
  const fable2::config::Values& cfg = fable2::config::Get();
  if (cfg.resolution_scale_auto) {
    *source = "resolution auto (decided after this)";
    return 1;
  }
  *source = "resolution_scale";
  return cfg.resolution_scale < 1 ? 1 : (cfg.resolution_scale > 3 ? 3 : cfg.resolution_scale);
}

bool g_picked_by_auto = false;
bool g_auto_uncapped = false;
int g_auto_scale = 0;

// FABLE2_AUTO_2X=0 turns the 2x rule off (fps/resolution auto as before it).
bool Rule2x() {
  const char* env = std::getenv("FABLE2_AUTO_2X");
  return !(env && std::strcmp(env, "0") == 0);
}

double RamGb() {
#ifdef __APPLE__
  uint64_t mem = 0;
  size_t len = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) return mem / 1073741824.0;
#endif
  return 0;
}

// Uncapped = the vsync cvar off (guest vblank timer at 1 kHz), exactly what
// REX_VSYNC=0 / the launcher's "Uncapped" does. Presentation stays display-synced
// (CAMetalLayer displaySyncEnabled, metal_allow_tearing=false): no tearing.
// The cvar lives in the GPU plugin, which registers it after this runs when
// resolution is "auto" (OnPostInitLogging); the SDK reads REX_VSYNC when it
// registers a late cvar, so set that instead (an explicit --vsync wins).
void ApplyUncapped() {
  g_auto_uncapped = true;
  if (rex::cvar::SetFlagByName("vsync", "false")) {
    REXSYS_INFO("[fps-auto] uncapped: vsync=false");
  } else {
    setenv("REX_VSYNC", "0", 1);
    REXSYS_INFO("[fps-auto] uncapped: REX_VSYNC=0 (vsync cvar not registered yet)");
  }
}

bool ResolveAuto() {
  int cores = -1;
  const char* cores_note = "";
  for (const char* var : {"FABLE2_FPS_AUTO_CORES", "FABLE2_RES_AUTO_CORES"}) {
    const char* env = std::getenv(var);
    if (!env || !*env || cores > 0) continue;
    if (ParsePositiveInt(env, cores)) cores_note = var;
    else REXSYS_WARN("[fps-auto] ignoring invalid {}='{}'", var, env);
  }
  std::string name;
  if (const char* env = std::getenv("FABLE2_RES_AUTO_NAME"); env && *env) name = env;
  GpuInfo gpu;
  if (cores <= 0 || name.empty()) {
    gpu = QueryGpu();
    if (cores <= 0) cores = gpu.cores;
    if (name.empty()) name = gpu.name;
  }
  const std::string fake = *cores_note ? std::string(" (cores faked by ") + cores_note + ")" : "";

  // Old rule (before 2026-10-13): 60 FPS from N raw GPU cores.
  if (const char* env = std::getenv("FABLE2_FPS_AUTO_MIN_CORES"); env && *env) {
    int threshold = 0;
    if (ParsePositiveInt(env, threshold)) {
      if (cores > 0) {
        const bool use60 = cores >= threshold;
        REXSYS_INFO("[fps-auto] legacy rule (FABLE2_FPS_AUTO_MIN_CORES={}): gpu-core-count={}{} -> {} FPS",
                    threshold, cores, fake, use60 ? 60 : 30);
        return use60;
      }
      const bool big = name.find("Max") != std::string::npos ||
                       name.find("Ultra") != std::string::npos;
      REXSYS_WARN("[fps-auto] legacy rule: gpu-core-count unavailable ({}); device '{}' -> {} FPS "
                  "(60 for Max/Ultra names, else 30)",
                  gpu.why, name.empty() ? "unknown" : name, big ? 60 : 30);
      return big;
    }
    REXSYS_WARN("[fps-auto] ignoring invalid FABLE2_FPS_AUTO_MIN_CORES='{}'", env);
  }

  const char* scale_source = "";
  const int scale = PlannedScale(&scale_source);
  const bool rule_2x = Rule2x();
  const char* rule_note = rule_2x ? "" : " (2x rule off: FABLE2_AUTO_2X=0)";
  const auto d = fable2::res_auto::ChooseFrameTarget(name, cores, scale, rule_2x);
  if (d.cores_assumed) {
    REXSYS_WARN("[fps-auto] gpu-core-count unavailable ({}); assuming {} for a {} GPU",
                gpu.why.empty() ? "unknown" : gpu.why, d.cores,
                fable2::res_auto::TierName(d.tier));
  }
  int target = d.target_fps;
  if (std::strncmp(scale_source, "resolution auto", 15) == 0) {
    // Both auto: frame target and scale together (ChooseBoth, the 2x rule).
    const auto b = fable2::res_auto::ChooseBoth(name, cores, RamGb(), rule_2x);
    g_auto_scale = b.scale;
    target = b.target_fps;
    REXSYS_INFO("[fps-auto] '{}' {}: {} (scale from {}; picks {}x){}{}", name.empty() ? "unknown" : name,
                fable2::res_auto::TierName(d.tier), b.reason, scale_source, b.scale, rule_note, fake);
  } else {
    REXSYS_INFO("[fps-auto] '{}' {}: {} (scale from {}){}{}", name.empty() ? "unknown" : name,
                fable2::res_auto::TierName(d.tier), d.reason, scale_source, rule_note, fake);
  }
  if (target == 0) ApplyUncapped();
  return target == 60;
}

bool Resolve() {
  const fable2::config::Values& cfg = fable2::config::Get();
  bool auto_mode = cfg.fps_60_auto;
  bool use60 = cfg.fps_60;
  const char* source = "fable2_config.toml";
  if (const char* env = std::getenv("FABLE2_FPS_60"); env && *env) {
    if (std::strcmp(env, "1") == 0 || std::strcmp(env, "0") == 0) {
      auto_mode = false;
      use60 = env[0] == '1';
      source = "FABLE2_FPS_60";
    } else if (strcasecmp(env, "auto") == 0) {
      auto_mode = true;
      source = "FABLE2_FPS_60";
    } else {
      REXSYS_WARN("[fps] ignoring FABLE2_FPS_60='{}' (expected 1, 0 or auto)", env);
    }
  }
  if (!auto_mode) {
    REXSYS_INFO("[fps] fps_60={} -> {} FPS (source: {})", use60, use60 ? 60 : 30, source);
    return use60;
  }
  REXSYS_INFO("[fps] fps_60=auto (source: {})", source);
  g_picked_by_auto = true;
  return ResolveAuto();
}

}  // namespace

bool Use60() {
  static const bool value = Resolve();
  return value;
}

bool PickedByAuto() {
  Use60();
  return g_picked_by_auto;
}

bool AutoUncapped() {
  Use60();
  return g_auto_uncapped;
}

int AutoScale() {
  Use60();
  return g_auto_scale;
}

}  // namespace fable2::fps
