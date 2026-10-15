// Runtime side of resolution "auto" (see fable2_res_auto.h for the rule).
#include "fable2_res_auto.h"

#include <cstdlib>
#include <string>

#include <rex/logging/macros.h>

#include "fable2_fps_auto.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace fable2::res_auto {

namespace {
int g_auto_picked = 0;
}  // namespace

int AutoPicked() { return g_auto_picked; }

int Resolve() {
  fable2::fps::GpuInfo gpu;
  int cores = -1;
  const char* fake_note = "";
  if (const char* env = std::getenv("FABLE2_RES_AUTO_CORES"); env && *env) {
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
    if (end && !*end && v > 0 && v < 10000) {
      cores = static_cast<int>(v);
      fake_note = " (cores faked by FABLE2_RES_AUTO_CORES)";
    } else {
      REXSYS_WARN("[res-auto] ignoring invalid FABLE2_RES_AUTO_CORES='{}'", env);
    }
  }
  std::string name;
  if (const char* env = std::getenv("FABLE2_RES_AUTO_NAME"); env && *env) name = env;
  if (cores <= 0 || name.empty()) {
    gpu = fable2::fps::QueryGpu();
    if (cores <= 0) cores = gpu.cores;
    if (name.empty()) name = gpu.name;
  }
  double ram_gb = 0;
#ifdef __APPLE__
  uint64_t mem = 0;
  size_t len = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) ram_gb = mem / 1073741824.0;
#endif
  const bool use60 = fable2::fps::Use60();
  const int target = use60 || fable2::fps::AutoUncapped() ? 60 : 30;
  const Decision d = Choose(name, cores, ram_gb, target);
  if (d.cores_assumed) {
    REXSYS_WARN("[res-auto] gpu-core-count unavailable ({}); assuming {} for a {} GPU",
                gpu.why.empty() ? "unknown" : gpu.why, d.cores, TierName(d.tier));
  }
  // fps_60 = "auto" too: it chose the scale with the frame target (ChooseBoth,
  // the 2x rule); use that so both halves agree.
  int scale = d.scale;
  const char* joint = "";
  if (const int both = fable2::fps::AutoScale(); both > 0 && both != d.scale) {
    scale = both;
    joint = both == 2 ? "; 2x rule (see [fps-auto]) -> 2x" : "; frame-rate auto's pick (see [fps-auto])";
  }
  REXSYS_INFO("[res-auto] '{}' {}, {:.0f} GB RAM -> {}x: {}{}{}", name.empty() ? "unknown" : name,
              TierName(d.tier), ram_gb, scale, d.reason, joint, fake_note);
  g_auto_picked = scale;
  return scale;
}

}  // namespace fable2::res_auto
