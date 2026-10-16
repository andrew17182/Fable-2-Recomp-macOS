// fable2_fast_pacing.h - FABLE2_FAST_PACING=1: the "Skip cosmetic waits" umbrella (docs/load-fixes-20261016.md,
// section F; launcher General -> Game, default off). It changes the game's original pacing, so it is opt-in.
//
// It only changes the default of each sub-switch below. An explicit value of a sub-switch's own variable always wins
// (FABLE2_FAST_PACING=1 FABLE2_LOAD_FADE_FRAMES=15 keeps the original fades). Every item is a fixed timer or frame
// count that nothing else depends on; readiness and streaming waits are never touched.
//   FABLE2_LOAD_FADE_FRAMES  15 -> 1   render-object release fades (save notice, loading screen, boot), ~0.47 s each
//   FABLE2_SAVE_NOTICE_MS   500 -> 0   "Saving content" minimum display after the save is written (game: 3100)
#pragma once
#include <cstdlib>

namespace fable2::pacing {
inline bool FastPacing() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_FAST_PACING");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  return on;
}
}  // namespace fable2::pacing
