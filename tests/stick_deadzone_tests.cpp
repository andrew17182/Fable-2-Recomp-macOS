// Stick deadzone remap (src/input/stick_deadzone.h), checked against a model of the game's own
// stick processing (guest sub_821895B0, single-precision like the guest).
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "stick_deadzone.h"

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
  std::abort(); } } while (false)

using namespace fable2::stickdz;

namespace {
struct Out { int x, y; };
Out Run(const Config& c, int x, int y) {
  int16_t ox = int16_t(x), oy = int16_t(y);
  Apply(c, ox, oy);
  return {ox, oy};
}
// The game: v = raw * (1/32767); |v|^2 < 0.09 -> 0; else (|v| - 0.3) * (1/0.7) along v, clamped per axis.
struct Game { float x, y, mag; };
Game GameSees(Out o) {
  const float k = 3.051851e-05f;
  const float vx = float(o.x) * k, vy = float(o.y) * k;
  if (vx * vx + vy * vy < 0.09f) return {0, 0, 0};
  const float len = std::sqrt(vx * vx + vy * vy);
  const float s = (len - 0.3f) * 1.4285715f / len;
  const float gx = std::clamp(vx * s, -1.0f, 1.0f), gy = std::clamp(vy * s, -1.0f, 1.0f);
  return {gx, gy, std::hypot(gx, gy)};
}
}  // namespace

int main() {
  // Parsing: game default unless a number is given; clamp to 0..50.
  CHECK(!Parse(nullptr, nullptr).enabled);
  CHECK(!Parse("", nullptr).enabled);
  CHECK(!Parse("game", nullptr).enabled);
  CHECK(Parse("5", nullptr).enabled);
  CHECK(std::abs(Parse("5", nullptr).deadzone - 0.05) < 1e-9);
  CHECK(std::abs(Parse("80", nullptr).deadzone - 0.5) < 1e-9);
  CHECK(Parse("-3", nullptr).deadzone == 0.0);
  CHECK(Parse("5", nullptr).anti);
  CHECK(!Parse("5", "0").anti);
  CHECK(Parse("5", "1").anti);

  // Game default: unchanged, and the game's 30% swallows a 25% push.
  const Config off = Parse("game", nullptr);
  CHECK(Run(off, 1234, -567).x == 1234 && Run(off, 1234, -567).y == -567);
  CHECK(GameSees(Run(off, 8192, 0)).mag == 0);
  // The measured game thresholds (camera: 9700 no, 9900 yes; diagonal 6900 no, 7000 yes).
  CHECK(GameSees({9700, 0}).mag == 0 && GameSees({9900, 0}).mag > 0);
  CHECK(GameSees({6900, 6900}).mag == 0 && GameSees({7000, 7000}).mag > 0);

  for (const char* dz : {"0", "5", "15", "50"}) {
    const Config c = Parse(dz, nullptr);
    const double d = c.deadzone;
    // At rest and inside our deadzone: exactly zero (no drift).
    CHECK(Run(c, 0, 0).x == 0 && Run(c, 0, 0).y == 0);
    if (d > 0.01) {
      const int r = int(d * 32767 * 0.9);
      CHECK(Run(c, r, 0).x == 0 && Run(c, -r / 2, r / 2).y == 0);
    }
    // Just past our deadzone, in every direction: past the game's deadzone (the game sees a non-zero,
    // tiny value), direction kept.
    for (int deg = 0; deg < 360; deg += 3) {
      const double a = deg * M_PI / 180;
      const double r = (d + 0.002) * 32767;
      const Out o = Run(c, int(std::lround(r * std::cos(a))), int(std::lround(r * std::sin(a))));
      const Game g = GameSees(o);
      CHECK(g.mag > 0);
      CHECK(g.mag < 0.01);
      CHECK(std::abs(std::remainder(std::atan2(o.y, o.x) - a, 2 * M_PI)) < 0.01);
    }
    // Near-linear, monotonic response from our deadzone to full: the game sees ~ (r - d) / (1 - d).
    double prev = -1;
    for (int v = int(d * 32767) + 40; v <= 32767; v += 97) {
      const Game g = GameSees(Run(c, v, 0));
      const double want = (v / 32767.0 - d) / (1 - d);
      CHECK(std::abs(g.mag - want) < 0.003);
      CHECK(g.mag >= prev);
      prev = g.mag;
    }
    // Diagonals too.
    const Game half = GameSees(Run(c, int(0.5 / M_SQRT2 * 32767), int(0.5 / M_SQRT2 * 32767)));
    CHECK(std::abs(half.mag - (0.5 - d) / (1 - d)) < 0.003);
    // Full deflection unchanged, including square-cornered pads.
    CHECK(Run(c, 32767, 0).x == 32767 && Run(c, 32767, 0).y == 0);
    CHECK(Run(c, 0, -32768).x == 0 && Run(c, 0, -32768).y == -32768);
    CHECK(Run(c, 32767, 32767).x == 32767 && Run(c, 32767, 32767).y == 32767);
    CHECK(Run(c, 23170, 23170).x >= 23168);
  }

  // Anti-deadzone off: our deadzone only, values past it unchanged.
  const Config plain = Parse("35", "0");
  CHECK(Run(plain, 11000, 0).x == 0);
  CHECK(Run(plain, 12000, 0).x == 12000);
  std::puts("stick_deadzone_tests: ok");
  return 0;
}
