#pragma once

#include <cstdint>
#include <functional>

namespace rex::ui::metal {
struct MetalStartupWaitResult {
  uint64_t iterations = 0;
  uint64_t events = 0;
  bool overlay_shown = false;
};

// Main thread only. Services native events without executing deferred SDK
// callbacks, which could re-enter module launch before warm-up completes.
MetalStartupWaitResult WaitForMetalStartup(const std::function<bool()>& complete);

// [shader-prep] Main thread only: hide the app's windows and Dock icon (a headless FABLE2_SHADER_PREP run).
void HideAppForBackgroundWork();
}  // namespace rex::ui::metal
