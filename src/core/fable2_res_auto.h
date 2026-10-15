// Internal resolution "auto": pick 1x, 2x or 3x once at startup from the GPU.
//
// [graphics] resolution_scale = "auto" in fable2_config.toml, or
// FABLE2_RES_SCALE=auto (Dev Launcher "Auto"), resolves to a number before the
// Metal plugin starts (fable2_graphics_config.h ApplyResolutionScale), so the
// plugin, the FPS counter and every log see a plain 1/2/3. Not a dynamic
// resolution controller: the choice is fixed for the session.
//
// Rule (docs/resolution-scale.md section 24, "Auto"; the 2x rule: section 25):
//
//   effective cores = gpu-core-count x generation factor x (0.75 on Ultra)
//
//   pick the highest scale whose row below is met at the session's frame
//   target (60 FPS for fps_60 = true / auto->60 / uncapped, 30 FPS otherwise)
//   and whose RAM floor is met; else 1x.
//
// The frame target is decided first (fps_60 = "auto": ChooseFrameTarget below,
// estimated at 1x when resolution is "auto"), then the scale at that target.
// When both are "auto", ChooseBoth adds the 2x rule: a Mac these rows keep
// below 2x runs 2x when the frame model puts 2x at <= 22 ms (>= 45 FPS) and it
// has >= 16 GB, capped at 60 when 2x is <= 18.5 ms, else uncapped.
//
// The thresholds are "the estimated GPU p95 frame time fits the frame budget",
// scaled from the M5 Max benchmark (40 cores x 1.10 = 44 effective cores,
// headings8 uncapped GPU p95: 9.9 / 12.8 / 17.5 ms at 1x / 2x / 3x):
//   need(scale) = 44 x p95_M5Max(scale) / budget_ms, rounded up.
// The M3 Pro run (14 cores, 2x p95 30.9 ms, 3x p95 52.2 ms) is predicted at
// 40.2 / 55.0 ms, i.e. the rule is conservative there. To update after a new
// benchmark report: adjust that generation's factor so its measured p95 matches
// the prediction, or the threshold rows when the M5 Max anchor changes.
//
// Pure logic only (no IOKit, no logging) so tests/res_auto_tests.cpp can check
// it; the Dev Launcher (tools/macos/DevLauncher/main.swift, AutoResolution)
// mirrors these tables and tests/res_auto_sync_tests.py keeps them in sync.
//
// Test overrides (runtime, src/core/fable2_res_auto.cpp): FABLE2_RES_AUTO_CORES=<n>
// (fake core count), FABLE2_RES_AUTO_NAME=<name> (fake GPU name, e.g. "Apple M1").
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace fable2::res_auto {

enum class Tier { kBase, kPro, kMax, kUltra };

inline const char* TierName(Tier t) {
  switch (t) {
    case Tier::kBase: return "base";
    case Tier::kPro: return "Pro";
    case Tier::kMax: return "Max";
    case Tier::kUltra: return "Ultra";
  }
  return "?";
}

// --- Tables (keep in sync with AutoResolution in the Dev Launcher) ---------

// Per-core speed relative to M3 in this workload. M3 = 1.00 and M5 = 1.10 are
// measured (M3 Pro vs M5 Max at 3x: 1.02 by mean, 1.05 by p95, rounded down
// to a conservative 1.00 for M3); M1/M2/M4 are estimates until their reports
// land. Unknown or older-than-table generations use the lowest factor, newer
// ones the highest.
struct GenerationFactor {
  int generation;
  double factor;
};
inline constexpr GenerationFactor kGenerationFactors[] = {
    {1, 0.80}, {2, 0.90}, {3, 1.00}, {4, 1.05}, {5, 1.10},
};

// Ultra = two Max dies; GPU work does not scale perfectly across them.
inline constexpr double kUltraEfficiency = 0.75;

// Minimum effective cores and RAM for 2x / 3x at each frame target.
struct ScaleRow {
  int target_fps;      // 60 (also uncapped) or 30
  double need_2x;      // effective cores for 2x
  double need_3x;      // effective cores for 3x
};
inline constexpr ScaleRow kScaleRows[] = {
    // 44 x 12.8 / 16.7 = 33.8 ; 44 x 17.5 / 16.7 = 46.2
    {60, 34.0, 47.0},
    // 44 x 12.8 / 33.3 = 16.9 ; 44 x 17.5 / 33.3 = 23.1
    {30, 17.0, 24.0},
};
// Peak footprint 3.3 / 4.4 / 5.3 GB (M5 Max benchmark), ~9 GB at 3x with
// specialization: keep 2x off 8 GB Macs and 3x off 16-24 GB ones.
inline constexpr double kMinRamGb2x = 16.0;
inline constexpr double kMinRamGb3x = 32.0;

// Core count assumed when the IORegistry has none (smallest bin of each tier).
inline constexpr int kFallbackCores[] = {/*base*/ 7, /*Pro*/ 14, /*Max*/ 24, /*Ultra*/ 48};

// --- Logic -------------------------------------------------------------------

// "Apple M3 Pro" -> 3; 0 when there is no "M<digits>".
inline int GenerationOf(const std::string& name) {
  for (size_t i = 0; i + 1 < name.size(); ++i) {
    if (name[i] == 'M' && (i == 0 || name[i - 1] == ' ') &&
        std::isdigit(static_cast<unsigned char>(name[i + 1]))) {
      int g = 0;
      for (size_t j = i + 1; j < name.size() && std::isdigit(static_cast<unsigned char>(name[j])); ++j)
        g = g * 10 + (name[j] - '0');
      return g;
    }
  }
  return 0;
}

inline Tier TierOf(const std::string& name) {
  if (name.find("Ultra") != std::string::npos) return Tier::kUltra;
  if (name.find("Max") != std::string::npos) return Tier::kMax;
  if (name.find("Pro") != std::string::npos) return Tier::kPro;
  return Tier::kBase;
}

inline double FactorFor(int generation) {
  constexpr size_t n = sizeof(kGenerationFactors) / sizeof(kGenerationFactors[0]);
  if (generation <= kGenerationFactors[0].generation) return kGenerationFactors[0].factor;
  for (const auto& g : kGenerationFactors)
    if (g.generation == generation) return g.factor;
  return kGenerationFactors[n - 1].factor;  // newer than the table
}

inline const ScaleRow& RowFor(int target_fps) {
  return target_fps > 30 ? kScaleRows[0] : kScaleRows[1];
}

struct Decision {
  int scale = 1;
  int generation = 0;
  Tier tier = Tier::kBase;
  int cores = 0;            // used (measured, faked or fallback)
  bool cores_assumed = false;
  double factor = 0;
  double effective = 0;
  int target_fps = 60;
  double need_2x = 0, need_3x = 0;
  std::string reason;       // one line for the log / launcher
};

// name: GPU or chip name ("Apple M5 Max"); cores <= 0 = unknown;
// ram_gb <= 0 = unknown (no RAM gate); target_fps: 60 or 30.
inline Decision Choose(const std::string& name, int cores, double ram_gb, int target_fps) {
  Decision d;
  d.generation = GenerationOf(name);
  d.tier = TierOf(name);
  d.target_fps = target_fps > 30 ? 60 : 30;
  d.cores = cores;
  if (d.cores <= 0) {
    d.cores = kFallbackCores[static_cast<int>(d.tier)];
    d.cores_assumed = true;
  }
  d.factor = FactorFor(d.generation);
  d.effective = d.cores * d.factor * (d.tier == Tier::kUltra ? kUltraEfficiency : 1.0);
  const ScaleRow& row = RowFor(d.target_fps);
  d.need_2x = row.need_2x;
  d.need_3x = row.need_3x;
  const bool ram_known = ram_gb > 0;
  char buf[256];
  const char* why;
  if (d.effective >= row.need_3x && (!ram_known || ram_gb >= kMinRamGb3x)) {
    d.scale = 3;
    why = "meets the 3x row";
  } else if (d.effective >= row.need_2x && (!ram_known || ram_gb >= kMinRamGb2x)) {
    d.scale = 2;
    why = d.effective >= row.need_3x ? "meets the 3x row but RAM is below the 3x floor"
                                     : "meets the 2x row";
  } else {
    d.scale = 1;
    why = d.effective >= row.need_2x ? "meets the 2x row but RAM is below the 2x floor"
                                     : "below the 2x row";
  }
  std::snprintf(buf, sizeof(buf),
                "%d cores%s x %.2f (M%d)%s = %.1f effective at %d FPS target "
                "(2x needs %.0f, 3x %.0f): %s",
                d.cores, d.cores_assumed ? " (assumed)" : "", d.factor, d.generation,
                d.tier == Tier::kUltra ? " x 0.75 (Ultra)" : "", d.effective, d.target_fps,
                row.need_2x, row.need_3x, why);
  d.reason = buf;
  return d;
}

// --- Frame target ("fps_60 = auto") ------------------------------------------
//
// Same effective cores as above. fps-auto picks 60 FPS when the estimated
// uncapped mean GPU frame time at the scale that will run is <= kFpsAutoMaxMeanMs,
// at 2x uncapped when it is <= kUncapped2xMaxMeanMs (the 2x rule), else 30 FPS
// (docs/resolution-scale.md section 25):
//
//   mean_ms(scale) = fixed_ms(scale) + work(scale) / effective cores
//
// fixed/work are a two-point fit per scale through the uncapped headings8 GPU
// means (Bower Lake, GPU time trace) of the M5 Max (44 effective: 4.88 / 6.68 /
// 11.07 ms at 1x / 2x / 3x, macos 6eaf54b, 2026-10-14) and the M3 Pro (14
// effective: 8.9 / 19.4 / 37.1 ms, kit report at 51757bd, used as is: no credit
// for the later footprint-variant saving, so the fit is a little pessimistic
// for slower Macs). "fixed" is the part that does not shrink with more cores;
// at 3x it is negative because the M3 Pro loses more there than its core count
// says (memory bandwidth), which the fit folds into "work".
// Keep in sync with AutoFrameRate in the Dev Launcher (tests/res_auto_sync_tests.py).
struct FrameModelRow {
  int scale;
  double fixed_ms;
  double work;  // ms x effective cores
};
inline constexpr FrameModelRow kFrameModel[] = {
    {1, 3.0, 82.6},
    {2, 0.75, 261.0},
    {3, -1.1, 534.5},
};
// 18.5 ms = 54 FPS uncapped average: enough that the 60 cap holds a near-60
// (M3 Pro, older build: 15.7 ms uncapped -> 58.5 FPS at the cap, 1% low 48).
// Equivalent effective-core needs: 1x 5.3, 2x 14.7, 3x 27.3.
inline constexpr double kFpsAutoMaxMeanMs = 18.5;
// The 2x rule: at 2x, an estimated mean above kFpsAutoMaxMeanMs but <= this
// runs uncapped (vsync cvar off, as the launcher's "Uncapped") instead of at a
// 30 cap. 22 ms = 45 FPS; the M3 Pro (19.4 ms measured, 50.7 FPS) plays well
// there. Equivalent effective cores: 12.3. FABLE2_AUTO_2X=0 turns the rule off.
inline constexpr double kUncapped2xMaxMeanMs = 22.0;
// Base-tier rule (2026-10-14): base chips (no Pro/Max/Ultra in the name, also
// unknown GPUs) get at most this frame target from Auto, whatever the estimate,
// until a base Mac is measured (the refit estimates 56-95 FPS at 1x for them,
// extrapolated from 14 down to 5.6-11 effective cores). An explicit 60 still works.
inline constexpr int kBaseTierMaxAutoFps = 30;
// The old rule (before 2026-10-13): 60 FPS from this many raw GPU cores.
// FABLE2_FPS_AUTO_MIN_CORES=<n> switches back to it (n = 24 restores it exactly).
inline constexpr int kLegacyMinGpuCores = 24;

inline double EstimatedMeanMs(double effective, int scale) {
  const FrameModelRow* row = &kFrameModel[0];
  for (const auto& r : kFrameModel)
    if (r.scale == scale) row = &r;
  if (scale > kFrameModel[2].scale) row = &kFrameModel[2];
  return row->fixed_ms + row->work / (effective > 0.1 ? effective : 0.1);
}

struct FrameDecision {
  int target_fps = 30;
  int generation = 0;
  Tier tier = Tier::kBase;
  int cores = 0;
  bool cores_assumed = false;
  double factor = 0;
  double effective = 0;
  int scale = 1;           // scale the estimate is for
  double estimate_ms = 0;  // estimated uncapped mean GPU frame time
  std::string reason;
};

// scale: the resolution scale that will run (explicit 1/2/3, or 1 when
// resolution is Auto, since Auto runs after this and never goes below 1x).
// target_fps: 60, 30, or 0 = uncapped (the 2x rule's band, only at 2x and only
// with rule_2x).
inline FrameDecision ChooseFrameTarget(const std::string& name, int cores, int scale,
                                       bool rule_2x = true) {
  FrameDecision d;
  d.generation = GenerationOf(name);
  d.tier = TierOf(name);
  d.cores = cores;
  if (d.cores <= 0) {
    d.cores = kFallbackCores[static_cast<int>(d.tier)];
    d.cores_assumed = true;
  }
  d.factor = FactorFor(d.generation);
  d.effective = d.cores * d.factor * (d.tier == Tier::kUltra ? kUltraEfficiency : 1.0);
  d.scale = scale < 1 ? 1 : (scale > 3 ? 3 : scale);
  d.estimate_ms = EstimatedMeanMs(d.effective, d.scale);
  d.target_fps = d.estimate_ms <= kFpsAutoMaxMeanMs ? 60 : 30;
  const bool base_capped = d.tier == Tier::kBase && d.target_fps > kBaseTierMaxAutoFps;
  if (base_capped) d.target_fps = kBaseTierMaxAutoFps;
  if (d.target_fps == 30 && d.tier != Tier::kBase && rule_2x && d.scale == 2 && d.estimate_ms <= kUncapped2xMaxMeanMs)
    d.target_fps = 0;
  char band[32] = "";
  if (rule_2x && d.scale == 2)
    std::snprintf(band, sizeof(band), ", uncapped <= %.1f", kUncapped2xMaxMeanMs);
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "%d cores%s x %.2f (M%d)%s = %.1f effective; est. %.1f ms mean at %dx "
                "(60 needs <= %.1f%s) -> %s",
                d.cores, d.cores_assumed ? " (assumed)" : "", d.factor, d.generation,
                d.tier == Tier::kUltra ? " x 0.75 (Ultra)" : "", d.effective, d.estimate_ms,
                d.scale, kFpsAutoMaxMeanMs, band,
                d.target_fps == 60 ? "60 FPS" : d.target_fps == 30 ? "30 FPS" : "uncapped");
  d.reason = buf;
  if (base_capped) d.reason += " (base-tier rule: Auto keeps base chips at 30 until one is measured)";
  return d;
}

// --- Both "auto": frame target and scale together ----------------------------
//
// 1. ChooseFrameTarget at 1x, then Choose (the p95 rows) at that target, as before.
// 2. The 2x rule (rule_2x, FABLE2_AUTO_2X != 0): if that is below 2x, RAM is
//    >= kMinRamGb2x (or unknown) and the estimated 2x mean is <= 22 ms, run 2x
//    at the frame target ChooseFrameTarget gives an explicit 2x: 60 FPS when
//    <= 18.5 ms, else uncapped. The result always equals what fps-auto would
//    pick for the scale that runs.
struct BothDecision {
  int scale = 1;
  int target_fps = 60;      // 60, 30, or 0 = uncapped
  bool by_rule_2x = false;  // the 2x rule moved it to 2x
  double estimate_ms = 0;   // estimated uncapped mean at `scale`
  std::string reason;
};

inline BothDecision ChooseBoth(const std::string& name, int cores, double ram_gb,
                               bool rule_2x = true) {
  BothDecision b;
  const FrameDecision f = ChooseFrameTarget(name, cores, 1, rule_2x);
  const Decision r = Choose(name, cores, ram_gb, f.target_fps);
  b.scale = r.scale;
  b.target_fps = f.target_fps;
  b.estimate_ms = EstimatedMeanMs(f.effective, r.scale);
  b.reason = f.reason;
  const bool ram_ok = ram_gb <= 0 || ram_gb >= kMinRamGb2x;
  if (rule_2x && r.scale < 2 && ram_ok) {
    const FrameDecision f2 = ChooseFrameTarget(name, cores, 2, true);
    if (f2.target_fps != 30) {
      b.scale = 2;
      b.target_fps = f2.target_fps;
      b.estimate_ms = f2.estimate_ms;
      b.by_rule_2x = true;
      char buf[160];
      std::snprintf(buf, sizeof(buf), "; 2x rule: est. %.1f ms at 2x (<= %.1f, RAM ok) -> 2x %s",
                    f2.estimate_ms, kUncapped2xMaxMeanMs,
                    b.target_fps == 60 ? "at 60 FPS" : "uncapped");
      b.reason += buf;
    }
  }
  return b;
}

// Runtime (fable2_res_auto.cpp): reads the GPU, RAM and frame target, logs one
// "[res-auto] ..." line and returns 1, 2 or 3. Call once, before the Metal
// plugin starts.
int Resolve();

// The scale Resolve() picked, or 0 when resolution "auto" was not used.
int AutoPicked();

}  // namespace fable2::res_auto
