// Host->guest tick conversion with a carried remainder (src/core/fable2_clock_math.h).
// Apple Silicon: host 24 MHz, guest timebase 50 MHz.
#include "fable2_clock_math.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

using fable2::clockmath::AnchoredClock;
using fable2::clockmath::Convert;
using fable2::clockmath::SdkTickRatio;
using Ratio = std::pair<uint64_t, uint64_t>;

constexpr uint64_t kHost = 24'000'000, kGuest = 50'000'000;

// Many small host deltas (like high-rate mftb polling). Returns guest/host-time ratio.
static double Run(Ratio ratio, double scalar, uint64_t steps, bool carry, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<uint64_t> delta(0, 40);
  uint64_t host = 0, guest = 0, rem = 0;
  for (uint64_t i = 0; i < steps; ++i) {
    const uint64_t d = delta(rng);
    host += d;
    if (carry) guest += Convert(d, ratio, rem);
    else guest += d * ratio.first / ratio.second;  // SDK 0.10.0 behaviour
  }
  if (carry) {  // exact: equals one big conversion of the total
    const unsigned __int128 want = (unsigned __int128)host * ratio.first / ratio.second;
    assert(guest == (uint64_t)want);
  }
  const double real_s = double(host) / kHost, guest_s = double(guest) / kGuest;
  return guest_s / real_s / scalar;
}

int main() {
  // Ratios the SDK computes for 1x/1.5x/2x/3x and every 0.1 step 1.0..4.0.
  assert((SdkTickRatio(kGuest, kHost, 1.0) == Ratio{25, 12}));
  assert((SdkTickRatio(kGuest, kHost, 2.0) == Ratio{25, 6}));
  assert((SdkTickRatio(kGuest, kHost, 3.0) == Ratio{25, 4}));
  assert((SdkTickRatio(kGuest, kHost, 1.5) == Ratio{25, 8}));
  for (int n = 10; n <= 40; ++n) {
    const Ratio r = SdkTickRatio(kGuest, kHost, n / 10.0);
    const double want = double(kGuest) / kHost * n / 10.0;
    assert(std::abs(double(r.first) / r.second - want) < 1e-12);
  }

  // Exact rate over 50M small deltas (~1e9 host ticks, ~42 s) at each speed.
  for (double scalar : {1.0, 1.5, 2.0, 3.0}) {
    const Ratio r = SdkTickRatio(kGuest, kHost, scalar);
    const double fixed = Run(r, scalar, 50'000'000, true, 7);
    const double sdk = Run(r, scalar, 50'000'000, false, 7);
    std::printf("scale %.1f ratio %llu/%llu: carried %.6f, SDK rounding %.6f\n", scalar,
                (unsigned long long)r.first, (unsigned long long)r.second, fixed, sdk);
    assert(std::abs(fixed - 1.0) < 1e-6);
    assert(sdk < 0.999);  // documents the original drift
  }

  // No overflow on huge deltas (q * first path).
  for (uint64_t d : {uint64_t(1) << 40, uint64_t(1) << 60, ~uint64_t(0) / 7}) {
    for (Ratio r : {Ratio{25, 12}, Ratio{25, 6}, Ratio{25, 4}}) {
      uint64_t rem = 5 % r.second;
      const unsigned __int128 num = (unsigned __int128)d * r.first + rem;
      const uint64_t got = Convert(d, r, rem);
      assert(got == (uint64_t)(num / r.second) && rem == (uint64_t)(num % r.second));
    }
  }

  // Continuity across ratio changes (turbo toggles): monotonic, no jump, and
  // each segment runs at its own exact rate. Remainder resets on change.
  {
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<uint64_t> delta(1, 40);
    uint64_t guest = 0, rem = 0;
    Ratio cur{0, 0};
    for (double scalar : {1.0, 2.0, 1.0, 3.0, 1.5, 1.0}) {
      const Ratio r = SdkTickRatio(kGuest, kHost, scalar);
      if (r != cur) { cur = r; rem = 0; }
      uint64_t seg_host = 0;
      const uint64_t seg_guest0 = guest;
      for (int i = 0; i < 2'000'000; ++i) {
        const uint64_t d = delta(rng), before = guest;
        guest += Convert(d, cur, rem);
        seg_host += d;
        assert(guest >= before);
        // A single step never advances more than its own delta's worth (+1 tick).
        assert(guest - before <= d * cur.first / cur.second + 1);
      }
      const double rate =
          (double(guest - seg_guest0) / kGuest) / (double(seg_host) / kHost) / scalar;
      // Only the dropped sub-tick remainder (< 1 tick) at the switch differs.
      assert(std::abs(rate - 1.0) < 1e-6);
    }
  }
  // Anchored clock sharing state with the lossy SDK path (librexruntime callers
  // such as kernel time queries): exact after every anchored update, monotonic,
  // continuous across ratio changes.
  {
    std::mt19937_64 rng(23);
    std::uniform_int_distribution<uint64_t> delta(0, 40);
    std::uniform_int_distribution<int> who(0, 3);
    AnchoredClock clock;
    uint64_t host = 1'000'000, last_host = host, last_guest = 0, prev = 0;
    for (double scalar : {1.0, 2.0, 1.0, 3.0, 1.5, 1.0}) {
      const Ratio r = SdkTickRatio(kGuest, kHost, scalar);
      // First anchored update after the change sets the anchor; no jump.
      host += delta(rng);
      const uint64_t seg_guest0 = clock.Update(host, r, last_host, last_guest);
      assert(seg_guest0 >= prev && seg_guest0 - prev <= 40 * 25 / 4 + 1);
      const uint64_t anchor_guest = clock.anchor_guest, anchor_host = clock.anchor_host;
      for (int i = 0; i < 2'000'000; ++i) {
        host += delta(rng);
        uint64_t now;
        if (who(rng) == 0) {  // SDK UpdateGuestClock: truncating delta conversion
          const uint64_t d = host > last_host ? host - last_host : 0;
          last_host = host;
          last_guest += d * r.first / r.second;
          now = last_guest;
        } else {
          now = clock.Update(host, r, last_host, last_guest);
          const unsigned __int128 exact =
              anchor_guest + (unsigned __int128)(host - anchor_host) * r.first / r.second;
          assert(now == (uint64_t)exact);
        }
        assert(now >= prev);
        prev = now;
      }
      const uint64_t g = clock.Update(host, r, last_host, last_guest);
      const double rate = (double(g - anchor_guest) / kGuest) /
                          (double(host - anchor_host) / kHost) / scalar;
      assert(std::abs(rate - 1.0) < 1e-6);
      prev = g;
    }
    // A thread whose host sample predates the anchor gets the current count.
    const uint64_t before = last_guest;
    assert(clock.Update(clock.anchor_host - 1, clock.anchor_ratio, last_host, last_guest) == before);
  }
  std::puts("clock_math_tests: OK");
  return 0;
}
