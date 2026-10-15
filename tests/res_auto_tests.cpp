// Build-time check of resolution "auto" (src/core/fable2_res_auto.h): the scale it
// picks for known Apple Silicon GPUs. Core counts are the shipping bins.
#include "fable2_res_auto.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Expect(const char* name, int cores, double ram_gb, int fps, int want) {
  const auto d = fable2::res_auto::Choose(name, cores, ram_gb, fps);
  if (d.scale != want) {
    std::fprintf(stderr, "res_auto_tests: %s %d cores %.0f GB @%d FPS -> %dx, want %dx (%s)\n", name,
                 cores, ram_gb, fps, d.scale, want, d.reason.c_str());
    ++g_failures;
  }
}

// fps_60 = "auto" at the scale that will run (1 = native or resolution auto).
void ExpectFps(const char* name, int cores, int scale, int want) {
  const auto d = fable2::res_auto::ChooseFrameTarget(name, cores, scale);
  if (d.target_fps != want) {
    std::fprintf(stderr, "res_auto_tests: fps %s %d cores at %dx -> %d FPS, want %d (%s)\n", name,
                 cores, scale, d.target_fps, want, d.reason.c_str());
    ++g_failures;
  }
}

// Both auto (ChooseBoth): frame target at 1x, the scale at that target, then the 2x rule.
// want_fps 0 = uncapped.
void ExpectBoth(const char* name, int cores, double ram_gb, int want_fps, int want_scale) {
  const auto b = fable2::res_auto::ChooseBoth(name, cores, ram_gb);
  if (b.target_fps != want_fps || b.scale != want_scale) {
    std::fprintf(stderr, "res_auto_tests: both auto %s %d cores %.0f GB -> %d FPS %dx, want %d FPS %dx (%s)\n",
                 name, cores, ram_gb, b.target_fps, b.scale, want_fps, want_scale, b.reason.c_str());
    ++g_failures;
  }
}

}  // namespace

int main() {
  using fable2::res_auto::GenerationOf;
  using fable2::res_auto::Tier;
  using fable2::res_auto::TierOf;

  // Name parsing.
  if (GenerationOf("Apple M1") != 1 || GenerationOf("Apple M3 Pro") != 3 ||
      GenerationOf("Apple M10 Max") != 10 || GenerationOf("AMD Radeon Pro 5500M") != 0 ||
      GenerationOf("") != 0) {
    std::fprintf(stderr, "res_auto_tests: GenerationOf\n");
    ++g_failures;
  }
  if (TierOf("Apple M2") != Tier::kBase || TierOf("Apple M3 Pro") != Tier::kPro ||
      TierOf("Apple M5 Max") != Tier::kMax || TierOf("Apple M2 Ultra") != Tier::kUltra) {
    std::fprintf(stderr, "res_auto_tests: TierOf\n");
    ++g_failures;
  }

  // 60 FPS target (fps_60 = true / auto->60 / uncapped). Plenty of RAM.
  Expect("Apple M1", 7, 8, 60, 1);
  Expect("Apple M1", 8, 16, 60, 1);
  Expect("Apple M1 Pro", 16, 32, 60, 1);
  Expect("Apple M1 Max", 24, 32, 60, 1);
  Expect("Apple M1 Max", 32, 64, 60, 1);       // 25.6 effective
  Expect("Apple M1 Ultra", 64, 128, 60, 2);    // 38.4
  Expect("Apple M2", 10, 24, 60, 1);
  Expect("Apple M2 Pro", 19, 32, 60, 1);
  Expect("Apple M2 Max", 30, 32, 60, 1);       // 27.0
  Expect("Apple M2 Max", 38, 64, 60, 2);       // 34.2
  Expect("Apple M2 Ultra", 60, 64, 60, 2);     // 40.5
  Expect("Apple M2 Ultra", 76, 192, 60, 3);    // 51.3
  Expect("Apple M3", 10, 24, 60, 1);
  Expect("Apple M3 Pro", 14, 36, 60, 1);       // measured: 2x 27 ms GPU, 37 FPS
  Expect("Apple M3 Pro", 18, 36, 60, 1);
  Expect("Apple M3 Max", 30, 36, 60, 1);
  Expect("Apple M3 Max", 40, 48, 60, 2);
  Expect("Apple M3 Ultra", 60, 96, 60, 2);     // 45.0
  Expect("Apple M3 Ultra", 80, 256, 60, 3);    // 60.0
  Expect("Apple M4", 10, 16, 60, 1);
  Expect("Apple M4 Pro", 20, 48, 60, 1);
  Expect("Apple M4 Max", 32, 36, 60, 1);       // 33.6, just under 34
  Expect("Apple M4 Max", 40, 64, 60, 2);       // 42.0
  Expect("Apple M5", 10, 16, 60, 1);
  Expect("Apple M5 Max", 40, 64, 60, 2);       // 44.0; measured 3x p95 17.5 ms > 16.7
  Expect("Apple M7 Max", 40, 64, 60, 2);       // newer than the table: highest factor

  // 30 FPS target (fps_60 = false, or fps auto picked 30).
  Expect("Apple M1", 8, 16, 30, 1);
  Expect("Apple M1 Pro", 16, 32, 30, 1);       // 12.8
  Expect("Apple M3 Pro", 14, 36, 30, 1);       // 14 < 17 (measured 2x p95 30.9 ms: thin)
  Expect("Apple M3 Pro", 18, 36, 30, 2);
  Expect("Apple M2 Pro", 19, 32, 30, 2);       // 17.1
  Expect("Apple M4 Pro", 20, 24, 30, 2);       // 21.0
  Expect("Apple M1 Max", 32, 64, 30, 3);       // 25.6
  Expect("Apple M3 Max", 30, 36, 30, 3);
  Expect("Apple M5 Max", 40, 64, 30, 3);

  // RAM floors: 2x needs 16 GB, 3x 32 GB.
  Expect("Apple M5 Max", 40, 24, 30, 2);
  Expect("Apple M4 Pro", 20, 8, 30, 1);
  Expect("Apple M5 Max", 40, 0, 30, 3);        // RAM unknown: no gate

  // Core count unknown: smallest bin of the tier.
  Expect("Apple M5 Max", 0, 64, 60, 1);        // 24 x 1.10 = 26.4
  Expect("Apple M2 Ultra", -1, 192, 60, 1);    // 48 x 0.90 x 0.75 = 32.4
  Expect("Apple M2 Ultra", -1, 192, 30, 3);
  Expect("", -1, 0, 60, 1);

  const auto d = fable2::res_auto::Choose("Apple M5 Max", 40, 64, 60);
  if (d.reason.find("44.0 effective") == std::string::npos || d.need_2x != 34.0) {
    std::fprintf(stderr, "res_auto_tests: reason '%s'\n", d.reason.c_str());
    ++g_failures;
  }

  // --- Frame target (fps_60 = "auto"), docs/resolution-scale.md section 25 ---
  // Model recalibrated 2026-10-14 (M5 Max 4.88 / 6.68 / 11.07 ms, M3 Pro 8.9 / 19.4 / 37.1 ms).
  // 1x (native, or resolution auto): every chip is estimated under 18.5 ms now, but the
  // base-tier rule keeps base chips (and unknown GPUs) at 30 until a base Mac is measured.
  ExpectFps("Apple M1", 7, 1, 30);             // 5.6 eff, est. 17.8 ms; base-tier rule
  ExpectFps("Apple M1", 8, 1, 30);             // 15.9 ms; base-tier rule
  ExpectFps("Apple M1 Pro", 14, 1, 60);        // 10.4 ms
  ExpectFps("Apple M1 Max", 24, 1, 60);
  ExpectFps("Apple M1 Ultra", 48, 1, 60);
  ExpectFps("Apple M2", 8, 1, 30);             // 14.5 ms; base-tier rule
  ExpectFps("Apple M2 Pro", 16, 1, 60);
  ExpectFps("Apple M2 Max", 30, 1, 60);
  ExpectFps("Apple M2 Ultra", 60, 1, 60);
  ExpectFps("Apple M3", 8, 1, 30);             // 13.3 ms; base-tier rule
  ExpectFps("Apple M3 Pro", 14, 1, 60);        // measured 8.9 ms, 110 FPS uncapped
  ExpectFps("Apple M3 Max", 30, 1, 60);
  ExpectFps("Apple M4", 10, 1, 30);
  ExpectFps("Apple M4 Pro", 20, 1, 60);
  ExpectFps("Apple M5", 10, 1, 30);
  ExpectFps("Apple M5 Max", 40, 1, 60);        // measured 4.88 ms
  ExpectFps("Apple M7", 10, 1, 30);            // newer than the table: M5 factor

  // Explicit 2x / 3x set by hand: estimated at that scale. 0 = uncapped (the 2x rule).
  ExpectFps("Apple M3 Pro", 14, 2, 0);         // measured 19.4 ms, 50.7 FPS uncapped (was 30)
  ExpectFps("Apple M3 Pro", 14, 3, 30);        // measured 37.1 ms, 26.5 FPS
  ExpectFps("Apple M3 Pro", 18, 2, 60);        // 15.3 ms
  ExpectFps("Apple M2 Pro", 16, 2, 0);         // 18.9 ms
  ExpectFps("Apple M2 Pro", 19, 2, 60);        // 16.0 ms
  ExpectFps("Apple M1 Pro", 16, 2, 0);         // 21.1 ms (47 FPS)
  ExpectFps("Apple M1 Pro", 14, 2, 30);        // 24.1 ms (41 FPS): below the 45 FPS band
  ExpectFps("Apple M1", 8, 2, 30);             // 41.5 ms
  ExpectFps("Apple M2", 10, 2, 30);            // 29.8 ms
  ExpectFps("Apple M5", 10, 2, 30);            // 24.5 ms
  ExpectFps("Apple M4 Pro", 20, 2, 60);        // 13.2 ms
  ExpectFps("Apple M1 Max", 24, 2, 60);        // 14.3 ms
  ExpectFps("Apple M1 Max", 32, 3, 30);        // 19.8 ms (no uncapped band at 3x)
  ExpectFps("Apple M1 Ultra", 48, 3, 60);      // 17.5 ms
  ExpectFps("Apple M4 Max", 32, 3, 60);        // 14.8 ms
  ExpectFps("Apple M5 Max", 40, 2, 60);        // measured 6.68 ms
  ExpectFps("Apple M5 Max", 40, 3, 60);        // measured 11.07 ms, 87-90 FPS uncapped
  ExpectFps("Apple M1", 8, 9, 30);             // out-of-range scale clamps to 3x
  {
    // FABLE2_AUTO_2X=0: no uncapped band, the 30 cap as before.
    const auto f = fable2::res_auto::ChooseFrameTarget("Apple M3 Pro", 14, 2, false);
    if (f.target_fps != 30) {
      std::fprintf(stderr, "res_auto_tests: 2x rule off: %s\n", f.reason.c_str());
      ++g_failures;
    }
  }

  {
    const auto f = fable2::res_auto::ChooseFrameTarget("Apple M1", 8, 1);
    if (f.reason.find("base-tier rule") == std::string::npos || std::fabs(f.estimate_ms - 15.9) > 0.1) {
      std::fprintf(stderr, "res_auto_tests: base-tier reason '%s'\n", f.reason.c_str());
      ++g_failures;
    }
  }

  // Core count unknown: smallest bin of the tier.
  ExpectFps("Apple M5", -1, 1, 30);            // 7 x 1.10
  ExpectFps("Apple M1 Pro", -1, 1, 60);        // 14 x 0.80
  ExpectFps("Apple M5 Max", 0, 1, 60);
  ExpectFps("", -1, 1, 30);                    // unknown GPU counts as base

  // Model anchors: the fit reproduces the two measured Macs (+-0.2 ms).
  {
    using fable2::res_auto::EstimatedMeanMs;
    const double m5[] = {4.88, 6.68, 11.07}, m3[] = {8.9, 19.4, 37.1};
    for (int s = 1; s <= 3; ++s) {
      if (std::fabs(EstimatedMeanMs(44.0, s) - m5[s - 1]) > 0.2 ||
          std::fabs(EstimatedMeanMs(14.0, s) - m3[s - 1]) > 0.2) {
        std::fprintf(stderr, "res_auto_tests: model anchor %dx: %.2f / %.2f\n", s,
                     EstimatedMeanMs(44.0, s), EstimatedMeanMs(14.0, s));
        ++g_failures;
      }
    }
  }

  // Both auto (ChooseBoth: frame target at 1x, p95 rows, then the 2x rule). FPS 0 = uncapped.
  ExpectBoth("Apple M1", 7, 8, 30, 1);         // 2x 47.4 ms; 8 GB anyway
  ExpectBoth("Apple M1", 8, 16, 30, 1);        // 2x 41.5 ms (24 FPS): stays 1x
  ExpectBoth("Apple M2", 10, 16, 30, 1);       // 2x 29.8 ms (34 FPS): stays 1x
  ExpectBoth("Apple M1 Pro", 14, 16, 60, 1);   // 2x 24.1 ms (41 FPS): stays 1x
  ExpectBoth("Apple M1 Pro", 16, 32, 0, 2);    // 2x 21.1 ms: 2x uncapped
  ExpectBoth("Apple M2 Pro", 16, 16, 0, 2);    // 2x 18.9 ms: 2x uncapped
  ExpectBoth("Apple M2 Pro", 19, 32, 60, 2);   // 2x 16.0 ms: 2x at 60
  ExpectBoth("Apple M3 Pro", 14, 36, 0, 2);    // the maintainer's Mac: 2x uncapped (~50 FPS)
  ExpectBoth("Apple M3 Pro", 14, 8, 60, 1);    // RAM below 16 GB: no 2x
  ExpectBoth("Apple M3 Pro", 18, 36, 60, 2);
  ExpectBoth("Apple M4 Pro", 20, 48, 60, 2);
  ExpectBoth("Apple M5", 10, 16, 30, 1);       // 2x 24.5 ms
  ExpectBoth("Apple M1 Max", 32, 64, 60, 2);
  ExpectBoth("Apple M3 Max", 40, 48, 60, 2);   // p95 row
  ExpectBoth("Apple M5 Max", 40, 64, 60, 2);   // p95 row (3x row needs 47)
  ExpectBoth("Apple M3 Ultra", 80, 256, 60, 3);
  {
    const auto b = fable2::res_auto::ChooseBoth("Apple M3 Pro", 14, 36, false);
    if (b.scale != 1 || b.target_fps != 60 || b.by_rule_2x) {
      std::fprintf(stderr, "res_auto_tests: 2x rule off, both auto: %dx %d\n", b.scale, b.target_fps);
      ++g_failures;
    }
  }
  // Consistency sweep: whatever both autos pick, fps-auto at that scale picks the
  // same frame target (the 2x rule uses fps-auto's own 2x decision; the p95 rows
  // are stricter than fps-auto's mean rule), and 2x/3x never run above 22 ms.
  for (int cores = 4; cores <= 96; ++cores) {
    for (const char* name : {"Apple M1", "Apple M3 Pro", "Apple M5 Max", "Apple M2 Ultra"}) {
      for (double ram : {0.0, 8.0, 16.0, 64.0}) {
        const auto b = fable2::res_auto::ChooseBoth(name, cores, ram);
        const auto at = fable2::res_auto::ChooseFrameTarget(name, cores, b.scale);
        if (b.target_fps != 30 && at.target_fps != b.target_fps) {
          std::fprintf(stderr, "res_auto_tests: inconsistent %s %d cores %.0f GB: %d FPS at %dx but %s\n",
                       name, cores, ram, b.target_fps, b.scale, at.reason.c_str());
          ++g_failures;
        }
        if (b.scale >= 2 && b.target_fps != 30 &&
            b.estimate_ms > fable2::res_auto::kUncapped2xMaxMeanMs) {
          std::fprintf(stderr, "res_auto_tests: %s %d cores at %dx est. %.1f ms\n", name, cores,
                       b.scale, b.estimate_ms);
          ++g_failures;
        }
      }
    }
  }
  {
    const auto f = fable2::res_auto::ChooseFrameTarget("Apple M3 Pro", 14, 1);
    if (f.reason.find("14.0 effective") == std::string::npos ||
        f.reason.find("8.9 ms") == std::string::npos) {
      std::fprintf(stderr, "res_auto_tests: fps reason '%s'\n", f.reason.c_str());
      ++g_failures;
    }
    const auto b = fable2::res_auto::ChooseBoth("Apple M3 Pro", 14, 36);
    if (b.reason.find("2x rule: est. 19.4 ms") == std::string::npos ||
        b.reason.find("2x uncapped") == std::string::npos) {
      std::fprintf(stderr, "res_auto_tests: both reason '%s'\n", b.reason.c_str());
      ++g_failures;
    }
  }

  if (g_failures) return 1;
  std::puts("res_auto_tests: ok");
  return 0;
}
