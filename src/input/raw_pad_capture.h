// fable_2 - controller sticks before the deadzone remap, for the input recorder (docs/reference.md
// "Input recording and replay", schema 3).
//
// One guest pad poll (the XamInputGetState wrapper, src/diagnostics/fable2_state_probe.h) runs the SDK's
// InputSystem::GetState on the calling thread, which asks every device of the user, including
// BackFilterDriver (controller_hotkeys.h) for SDL controllers and the test pad. BackFilterDriver reports
// each controller's sticks before and after its deadzone remap (stick_deadzone.h) here, merged across
// controllers the way the SDK merges devices (per axis, the larger remapped magnitude; the raw value
// comes from the same controller). The recorder
// (fable2::session::ObservePad) then compares each stick the game read with the merged remapped value:
// equal = the stick came from a controller, so it records the raw value and replay remaps it again with
// the replaying session's settings. Otherwise (keyboard pad, mouse look, no controller) the stick is
// recorded as the game read it and replayed unchanged.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace fable2::rawpad {

struct Poll {
  bool any = false;                     // a controller reported this poll
  int16_t raw[4] = {}, remapped[4] = {};  // lx, ly, rx, ry
};

inline Poll& Current() {
  static thread_local Poll poll;
  return poll;
}

// Start of one guest pad poll.
inline void Begin() { Current() = Poll{}; }

inline int16_t Larger(int16_t a, int16_t b) { return std::abs(int(b)) > std::abs(int(a)) ? b : a; }

// One controller's sticks (lx, ly, rx, ry) before and after the remap.
inline void Observe(const int16_t raw[4], const int16_t remapped[4]) {
  Poll& p = Current();
  for (int i = 0; i < 4; ++i) {
    // The raw value follows the controller whose remapped value wins the merge (what the game reads).
    if (p.any && Larger(p.remapped[i], remapped[i]) == p.remapped[i]) continue;
    p.raw[i] = raw[i];
    p.remapped[i] = remapped[i];
  }
  p.any = true;
}

// The recorder's rule: `sticks` (lx, ly, rx, ry) is what the game read this poll. Each stick that equals the
// controllers' merged remapped value is replaced by their raw value. Returns which sticks were (bit 0 left,
// bit 1 right); the others came from the keyboard pad or mouse look (or no controller reported).
inline int ToRaw(int16_t sticks[4]) {
  const Poll& p = Current();
  if (!p.any) return 0;
  int mask = 0;
  for (int s = 0; s < 2; ++s) {
    if (sticks[2 * s] != p.remapped[2 * s] || sticks[2 * s + 1] != p.remapped[2 * s + 1]) continue;
    sticks[2 * s] = p.raw[2 * s];
    sticks[2 * s + 1] = p.raw[2 * s + 1];
    mask |= 1 << s;
  }
  return mask;
}

}  // namespace fable2::rawpad
