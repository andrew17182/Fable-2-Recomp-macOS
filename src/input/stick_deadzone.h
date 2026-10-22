// fable_2 - analog stick deadzone remap (controller sticks only; the keyboard pad and mouse look
// produce exact values and are not touched).
//
// The game's own deadzone (gameplay pad update 0x82189058 -> guest sub_821895B0, once per stick):
// v = raw / 32767; if |v|^2 < 0.09 the stick is zero, else it becomes (|v| - 0.3) / 0.7 along v,
// clamped to [-1, 1] per axis. So both sticks have a RADIAL 30% deadzone with a scaled ramp
// (constants read from guest memory, 2026-10-22). Measured with the test-control `stick` command:
// camera turns at rx 9900 (30.2%) but not 9700 (29.6%); a 6900,6900 diagonal (29.8%) does nothing,
// 7000,7000 (30.2%) turns. (Another subsystem, guest sub_830EB6D0, gates sticks with the stock XInput
// square 7849 / 8689; it is not the hero/camera path and is left alone.)
//
// Settings (read once):
//   FABLE2_STICK_DEADZONE_LEFT / FABLE2_STICK_DEADZONE_RIGHT  game (default) | 0..50 (percent)
//     game: the stick is passed unchanged (original behaviour).
//     N:    our own radial deadzone of N% (drift suppression); see FABLE2_STICK_ANTI_DEADZONE.
//   FABLE2_STICK_ANTI_DEADZONE  1 (default) | 0. Only acts on a stick with a deadzone set.
//     1: past our deadzone the stick is rescaled from [N%, 100%] onto [30%, 100%], so the smallest
//        movement lands just past the game's deadzone and the game's own ramp then gives a near-linear
//        0..1 from our deadzone to the edge.
//     0: our radial deadzone only (values past it unchanged); the game's 30% still applies on top.
// Full deflection (radius >= 100%) always passes unchanged. Unit test: tests/stick_deadzone_tests.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace fable2::stickdz {

// The game's radial deadzone (guest sub_821895B0): 30% of full deflection, both sticks.
constexpr double kGameDeadzone = 0.3;
// Smallest radius we hand the game: two raw units past its deadzone, so its float test
// (|v|^2 < 0.09) never rounds a remapped value back to zero.
constexpr double kGameFloor = kGameDeadzone + 2.0 / 32767.0;
constexpr int kMaxDeadzonePercent = 50;

struct Config {
  bool enabled = false;  // false = game default (stick passed unchanged)
  double deadzone = 0;   // 0..0.5 of full deflection, radial
  bool anti = true;      // rescale onto the game's live range
};

// Parses a FABLE2_STICK_DEADZONE_* value: null/empty/"game" or anything non-numeric -> disabled;
// a number -> percent clamped to 0..50.
inline Config Parse(const char* deadzone, const char* anti) {
  Config c;
  c.anti = !(anti && anti[0] == '0');
  if (!deadzone || !*deadzone) return c;
  char* end = nullptr;
  const double v = std::strtod(deadzone, &end);
  if (end == deadzone || !std::isfinite(v)) return c;
  c.enabled = true;
  c.deadzone = std::clamp(v, 0.0, double(kMaxDeadzonePercent)) / 100.0;
  return c;
}

inline int16_t ToRaw(double v) {
  return int16_t(std::clamp(std::lround(v * 32767.0), -32768L, 32767L));
}

// Remaps one stick in place (raw XInput units).
inline void Apply(const Config& c, int16_t& x, int16_t& y) {
  if (!c.enabled) return;
  const double fx = std::max(-1.0, x / 32767.0), fy = std::max(-1.0, y / 32767.0);
  const double m = std::hypot(fx, fy);
  if (m <= c.deadzone || m == 0) { x = 0; y = 0; return; }
  if (m >= 1.0) return;  // full deflection unchanged (also square-cornered pads)
  if (!c.anti) return;   // our deadzone only
  const double t = (m - c.deadzone) / (1.0 - c.deadzone);
  const double out = kGameFloor + (1.0 - kGameFloor) * t;
  x = ToRaw(fx / m * out);
  y = ToRaw(fy / m * out);
}

inline const Config& Left() {
  static const Config c = Parse(std::getenv("FABLE2_STICK_DEADZONE_LEFT"), std::getenv("FABLE2_STICK_ANTI_DEADZONE"));
  return c;
}
inline const Config& Right() {
  static const Config c = Parse(std::getenv("FABLE2_STICK_DEADZONE_RIGHT"), std::getenv("FABLE2_STICK_ANTI_DEADZONE"));
  return c;
}

inline std::string Describe(const Config& c) {
  if (!c.enabled) return "game default";
  return std::to_string(int(std::lround(c.deadzone * 100))) + "%" + (c.anti ? " + anti-deadzone" : " (no anti-deadzone)");
}

}  // namespace fable2::stickdz
