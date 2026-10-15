#pragma once
// [encoder-trim] Per-encoder CPU trims on the GPU Commands thread (docs/resolution-scale.md §19).
// FABLE2_ENCODER_TRIM (default on, =0 restores the previous behaviour):
// - debug labels and debug groups on per-frame encoders and command buffers are skipped. Each one
//   created an NSString (some with an fmt::format first) on every encoder; nothing reads them
//   outside a GPU capture. FABLE2_ENCODER_LABELS=1 or MTL_CAPTURE_ENABLED=1 keeps them with the trim on.
// - render passes declare their attachment heaps from the render-target cache's record of the
//   textures it bound, instead of walking all 8 colour attachment descriptors.
// None of this changes what the GPU executes.
#include <cstdlib>

#include "rex/logging.h"

namespace rex::graphics::metal {

inline bool EncoderTrimEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_ENCODER_TRIM");
    const bool e = !(v && v[0] == '0');
    REXLOG_INFO("[encoder-trim] {} (FABLE2_ENCODER_TRIM)", e ? "on" : "off");
    return e;
  }();
  return on;
}

inline bool EncoderDebugLabelsEnabled() {
  static const bool on = [] {
    const char* l = std::getenv("FABLE2_ENCODER_LABELS");
    const char* c = std::getenv("MTL_CAPTURE_ENABLED");
    return !EncoderTrimEnabled() || (l && l[0] == '1') || (c && c[0] == '1');
  }();
  return on;
}

}  // namespace rex::graphics::metal
