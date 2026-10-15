// fable2_clock_math.h - exact host->guest tick conversion with a carried remainder.
//
// Pure arithmetic shared by the executable's guest clock (fable2_guest_clock.h)
// and tests/clock_math_tests.cpp. Same math as patches/sdk/clock-fractional-ticks.patch.
#pragma once

#include <cstdint>
#include <numeric>
#include <utility>

namespace fable2::clockmath {

// guest = (host_delta * first + remainder) / second; the new remainder is kept.
// Split so host_delta * first cannot overflow:
//   host_delta = q * second + r  =>  guest = q * first + (r * first + remainder) / second.
// Requires second * (first + 1) < 2^64 (true for every reduced tick ratio:
// on Apple Silicon the ratios are 25/12 at 1x, 25/6 at 2x, 25/4 at 3x).
constexpr uint64_t Convert(uint64_t host_delta, std::pair<uint64_t, uint64_t> ratio,
                           uint64_t& remainder) {
  const uint64_t q = host_delta / ratio.second;
  const uint64_t r = host_delta % ratio.second;
  const uint64_t partial = r * ratio.first + remainder;
  remainder = partial % ratio.second;
  return q * ratio.first + partial / ratio.second;
}

// Guest clock that is exact against an anchor instead of summing per-call
// conversions. The SDK keeps (last_host, last_guest) and adds a truncated
// conversion of each host delta; callers inside librexruntime still do that
// and lose a fraction of a tick per call. Update() recomputes the guest count
// from the anchor (set where the ratio last changed), so those losses are
// absorbed on the next call here and the clock stays exact. The result never
// goes backwards (max with last_guest). On a ratio change the anchor moves to
// the current (last_host, last_guest), so the count continues without a jump.
struct AnchoredClock {
  uint64_t anchor_host = 0;
  uint64_t anchor_guest = 0;
  std::pair<uint64_t, uint64_t> anchor_ratio{0, 0};

  // Caller holds the SDK tick mutex. Updates the shared last_host/last_guest.
  uint64_t Update(uint64_t host, std::pair<uint64_t, uint64_t> ratio, uint64_t& last_host,
                  uint64_t& last_guest) {
    if (anchor_ratio != ratio) {
      anchor_ratio = ratio;
      anchor_host = host < last_host ? host : last_host;
      anchor_guest = last_guest;
    }
    // A host sample taken before the anchor (thread raced the re-anchor):
    // the shared count is already later than it, so just return that.
    if (host < anchor_host) return last_guest;
    uint64_t remainder = 0;
    const uint64_t exact = anchor_guest + Convert(host - anchor_host, ratio, remainder);
    if (host > last_host) last_host = host;
    if (exact > last_guest) last_guest = exact;
    return last_guest;
  }
};

// Mirror of SDK RecomputeGuestTickScalar (src/core/clock.cpp): the reduced
// guest/host tick ratio for a guest time scalar. The SDK quantizes the scalar
// to tenths with a truncating cast, so callers should pass n / 10.0.
inline std::pair<uint64_t, uint64_t> SdkTickRatio(uint64_t guest_freq, uint64_t host_freq,
                                                  double scalar) {
  std::pair<uint64_t, uint64_t> frac{guest_freq, host_freq};
  if (scalar > 1.0) {
    frac.first *= static_cast<uint64_t>(scalar * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / scalar);
  }
  const uint64_t g = std::gcd(frac.first, frac.second);
  return {frac.first / g, frac.second / g};
}

}  // namespace fable2::clockmath
