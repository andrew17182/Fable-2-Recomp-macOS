// Stick deadzone remap (src/input/stick_deadzone.h), checked against a model of the game's own
// stick processing (guest sub_821895B0, single-precision like the guest).
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "raw_pad_capture.h"
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
Out RunLeft(const Config& c, const WalkConfig& w, int x, int y) {
  int16_t ox = int16_t(x), oy = int16_t(y);
  ApplyLeft(c, w, ox, oy);
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
  CHECK(!Parse("original", nullptr).enabled);
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
  // Walk deadzone parsing: game/original off, percent clamped to 0..25.
  CHECK(!ParseWalk(nullptr).enabled && !ParseWalk("").enabled);
  CHECK(!ParseWalk("game").enabled && !ParseWalk("original").enabled);
  CHECK(ParseWalk("0").enabled && ParseWalk("0").band == 0.0);
  CHECK(std::abs(ParseWalk("5").band - 0.05) < 1e-9);
  CHECK(std::abs(ParseWalk("60").band - 0.25) < 1e-9);
  // Inverse ramp: the raw radius that gives a post-ramp value.
  CHECK(std::abs(RawForPost(0) - 0.3) < 1e-12 && std::abs(RawForPost(1) - 1.0) < 1e-12);
  CHECK(std::abs(RawForPost(kWalkPost) - 0.475) < 1e-12);
  CHECK(GameSees({int(kTurnTopRaw * 32767), 0}).mag < kWalkPost);
  CHECK(GameSees({int(std::lround(kWalkRaw * 32767)), 0}).mag > kWalkPost);
  // Walk deadzone off: identical to Apply.
  {
    const Config c = Parse("6", nullptr);
    const WalkConfig w = ParseWalk("game");
    for (int v = 0; v <= 32767; v += 211) CHECK(RunLeft(c, w, v, -v / 3).x == Run(c, v, -v / 3).x);
  }
  // Walk deadzone on: [d, d+B] turns only (0 < post < 0.25), [d+B, 1] walks (0.25 < post, linear to 1).
  for (const char* dz : {"game", "0", "6", "20", "50"}) {
    for (const char* band : {"0", "5", "12", "25"}) {
      const Config c = Parse(dz, nullptr);
      const WalkConfig w = ParseWalk(band);
      const double d = c.enabled ? c.deadzone : 0.3, b = w.band;
      CHECK(RunLeft(c, w, 0, 0).x == 0 && RunLeft(c, w, 0, 0).y == 0);
      if (c.enabled && d > 0.01) CHECK(RunLeft(c, w, int(d * 0.9 * 32767), 0).x == 0);
      if (!c.enabled) CHECK(RunLeft(c, w, 8000, 0).x == 8000);  // under the game's 30%: unchanged
      double prev = -1;
      for (int v = int(d * 32767) + 7; v < 32767; v += 53) {
        const double m = v / 32767.0;
        for (int deg : {0, 45, 90, 200, 300}) {
          const double a = deg * M_PI / 180;
          const Out o = RunLeft(c, w, int(std::lround(v * std::cos(a))), int(std::lround(v * std::sin(a))));
          const Game g = GameSees(o);
          const double mm = std::hypot(std::lround(v * std::cos(a)), std::lround(v * std::sin(a))) / 32767.0;
          if (mm >= 1.0 || mm <= d) continue;
          CHECK(g.mag > 0);
          if (mm < d + b - 0.0005) CHECK(g.mag < kWalkPost);
          if (mm > d + b + 0.0005) {
            CHECK(g.mag > kWalkPost);
            const double want = kWalkPost + (1 - kWalkPost) * (mm - d - b) / (1 - d - b);
            CHECK(std::abs(g.mag - want) < 0.004);
          }
          if (v >= 300)  // tiny inputs: integer rounding of the input itself dominates the angle
            CHECK(std::abs(std::remainder(std::atan2(o.y, o.x) - std::atan2(std::sin(a), std::cos(a)), 2 * M_PI)) < 0.01);
        }
        const double mag = GameSees(RunLeft(c, w, v, 0)).mag;
        CHECK(mag >= prev);
        prev = mag;
        (void)m;
      }
      // Full deflection unchanged.
      CHECK(RunLeft(c, w, 32767, 0).x == 32767 && RunLeft(c, w, 0, -32768).y == -32768);
    }
  }
  // Example from the design: deadzone 6%, band 5% -> walking starts at ~11%.
  {
    const Config c = Parse("6", nullptr);
    const WalkConfig w = ParseWalk("5");
    CHECK(GameSees(RunLeft(c, w, int(0.105 * 32767), 0)).mag < kWalkPost);
    CHECK(GameSees(RunLeft(c, w, int(0.112 * 32767), 0)).mag > kWalkPost);
  }
  // Input recorder (raw_pad_capture.h): controller sticks recorded raw, the keyboard pad / mouse look as read.
  {
    namespace rp = fable2::rawpad;
    rp::Begin();
    int16_t none[4] = {100, 0, 0, 0};
    CHECK(rp::ToRaw(none) == 0 && none[0] == 100);  // no controller this poll
    // A 6% deadzone controller: raw 20% left is remapped; the game reads the remapped value.
    const Config c = Parse("6", nullptr);
    int16_t lx = 6554, ly = 0, rx = 1000, ry = 0;
    const int16_t raw[4] = {lx, ly, rx, ry};
    Apply(c, lx, ly);
    Apply(c, rx, ry);  // 3% right: inside the deadzone -> 0
    const int16_t remapped[4] = {lx, ly, rx, ry};
    CHECK(lx != 6554 && rx == 0);
    rp::Observe(raw, remapped);
    int16_t game[4] = {lx, ly, rx, ry};
    CHECK(rp::ToRaw(game) == 3 && game[0] == 6554 && game[2] == 1000);
    // The keyboard pad pushed the left stick past the controller's value: recorded as read, not remapped.
    int16_t keyboard[4] = {32767, 0, rx, ry};
    CHECK(rp::ToRaw(keyboard) == 2 && keyboard[0] == 32767 && keyboard[2] == 1000);
    // Two controllers merge like the SDK (larger remapped magnitude per axis, the first on a tie); the raw
    // value comes from the controller that won.
    const int16_t raw2[4] = {-9000, 0, 1000, 0}, remapped2[4] = {-9000, 0, 0, 0};
    rp::Observe(raw2, remapped2);
    int16_t merged[4] = {lx, 0, 0, 0};
    CHECK(lx > 9000 && rp::ToRaw(merged) == 3 && merged[0] == 6554);
    rp::Begin();
    CHECK(!rp::Current().any);
  }
  // Replay remap (ApplySession): this session's settings, read once from the environment.
  {
    setenv("FABLE2_STICK_DEADZONE_LEFT", "6", 1);
    setenv("FABLE2_WALK_DEADZONE", "5", 1);
    unsetenv("FABLE2_STICK_DEADZONE_RIGHT");
    int16_t lx = int16_t(0.20 * 32767), ly = 0, rx = 5000, ry = 0;
    int16_t ex = lx, ey = ly;
    ApplyLeft(Parse("6", nullptr), ParseWalk("5"), ex, ey);
    ApplySession(lx, ly, rx, ry, 1);
    CHECK(lx == ex && ly == ey && rx == 5000);           // right stick: game default, unchanged
    CHECK(GameSees({lx, ly}).mag > kWalkPost);           // 20% walks with deadzone 6% + walk 5%
    CHECK(GameSees({int(0.20 * 32767), 0}).mag == 0);    // the same raw value under Original: nothing
    int16_t kx = 3000, ky = 0, z = 0, z2 = 0;
    ApplySession(kx, ky, z, z2, 0);
    CHECK(kx == 3000);  // not a controller stick: unchanged
  }
  std::puts("stick_deadzone_tests: ok");
  return 0;
}
