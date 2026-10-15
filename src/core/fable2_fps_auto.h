// Frame-rate mode: fps_60 = true / false / "auto".
//
// [patches] fps_60 in fable2_config.toml (or FABLE2_FPS_60=1/0/auto) picks the
// frame-dispatch interval used by fable2_hook_fps_60 (src/core/fable2_hooks.cpp):
// 60 FPS (one vblank per frame) or 30 FPS (two, the console's rate). "auto"
// decides once at startup, before resolution "auto", from the GPU (IORegistry
// IOAccelerator "model" + "gpu-core-count"; MTLDevice does not expose the count):
// effective cores as in resolution auto, then 60 FPS when the estimated uncapped
// mean GPU frame time at the scale that will run (explicit 1/2/3, or 1x when
// resolution is "auto") is <= 18.5 ms, else 30 FPS. The 2x rule (FABLE2_AUTO_2X,
// default on): at 2x, <= 22 ms runs uncapped (the vsync cvar is set to false, as
// the launcher's "Uncapped" does with REX_VSYNC=0); with resolution "auto" too,
// a Mac the p95 rows keep at 1x moves to 2x (60 or uncapped) when 2x is
// estimated at <= 22 ms and RAM >= 16 GB (ChooseBoth). Rule, model and table:
// src/core/fable2_res_auto.h ChooseFrameTarget / ChooseBoth,
// docs/resolution-scale.md section 25.
//
// Test overrides: FABLE2_FPS_AUTO_CORES=<n> (or FABLE2_RES_AUTO_CORES) fakes the
// core count, FABLE2_RES_AUTO_NAME=<name> the GPU name. FABLE2_FPS_AUTO_MIN_CORES=<n>
// switches back to the old rule (60 FPS from n raw GPU cores; 24 = the old
// default). Turbo still overrides all modes.
#pragma once

#include <string>

namespace fable2::fps {

// GPU identity from the IORegistry (also used by resolution "auto",
// src/core/fable2_res_auto.cpp). Queried fresh on every call.
struct GpuInfo {
  int cores = -1;      // -1 = unknown
  std::string name;    // accelerator "model" (e.g. "Apple M5 Max") or CPU brand
  std::string why;     // why cores is unknown
};
GpuInfo QueryGpu();

// True when the frame-dispatch hook should run at 60 FPS. Resolved once (first
// call logs one line: "[fps] ..." or "[fps-auto] ..."); thread-safe.
bool Use60();

// True when fps_60 = "auto" made the choice (for the window title).
bool PickedByAuto();

// True when fps_60 = "auto" chose uncapped (the 2x rule); the vsync cvar is
// then false. Resolved with Use60().
bool AutoUncapped();

// The scale fps-auto chose together with resolution "auto" (ChooseBoth), or 0
// when resolution is not "auto", fps_60 is not "auto" or the legacy rule ran.
// res_auto::Resolve() uses it so both halves agree. Resolved with Use60().
int AutoScale();

}  // namespace fable2::fps
