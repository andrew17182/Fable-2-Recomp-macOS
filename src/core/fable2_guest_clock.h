// fable2_guest_clock.h - guest clock without cumulative rounding drift (macOS).
//
// The pinned SDK's UpdateGuestClock (src/core/clock.cpp) converts every host
// tick delta with integer `delta * first / second` and drops the remainder on
// each call. Guest mftb (REX_QUERY_TIMEBASE -> Clock::QueryGuestTickCount) is
// polled at a very high rate, so on Apple Silicon (24 MHz host ticks, ratio
// 25/12) the scaled guest clock ran ~0.96x real time and starved Bink movie
// audio. The launchers used --clock_no_scaling=true as a workaround, which
// also disables set_guest_time_scalar (turbo, src/core/fable2_turbo.h).
//
// The source fix is patches/sdk/clock-fractional-ticks.patch, but the runtime
// actually loaded is the prebuilt librexruntime.dylib (ReXGlue 0.10.0
// release), which that patch cannot change. This header therefore defines a
// strong Clock::QueryGuestTickCount in the executable. Every recompiled mftb
// site (REX_QUERY_TIMEBASE, generated/default/fable_2_pch.h) and all app code
// bind to it instead of the dylib export (verified with nm -m: defined in
// __TEXT of fable_2; tick_mutex_/last_*_tick_count_/guest_tick_ratio_ imported
// from librexruntime). It advances the SDK's own exported clock state under
// tick_mutex_, so kernel system time, uptime and timers inside the dylib stay
// on the same guest timeline.
//
// Callers inside librexruntime/librexgpu-metal (kernel time queries, GPU vblank
// thread) still run the SDK's truncating path. Measured 2026-10-05: with only a
// per-call remainder carried here, the clock dipped to ~0.988x during loading
// because of those calls. So the count here is computed against an anchor
// (fable2::clockmath::AnchoredClock): every call returns the exact
// anchor + (host - anchor_host) * ratio, which re-absorbs the SDK-path losses.
//
// A ratio change (turbo on/off, set_guest_time_scalar) re-anchors at the
// current count, so the guest clock never jumps.
//
// FABLE2_CLOCK_SDK_ROUNDING=1 forwards to the original SDK implementation
// (for A/B measurements). --clock_no_scaling=true keeps working as before.
//
// Source-built runtime (2026-10-09, docs/sdk-from-source.md B6): the SDK carries
// the fractional ticks itself (patches/sdk/clock-fractional-ticks.patch) and
// defines REX_CLOCK_EXACT_GUEST_TICKS in clock.h. Then this override is compiled
// out and every caller, the recompiled mftb sites included, binds to the runtime's
// Clock::QueryGuestTickCount. It stays the fallback for the prebuilt runtime
// (FABLE2_SDK_FROM_SOURCE=0).
//
// Single-TU (included from main.cpp only). macOS only: Windows host ticks are
// 10 MHz QPC, where the 1x ratio is 5/1 and nothing is lost.
#pragma once

#include <cstdlib>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging/macros.h>

#if defined(__APPLE__) && defined(REX_CLOCK_EXACT_GUEST_TICKS)

namespace fable2::guestclock {
// Called once after runtime setup: say which clock path is live.
inline void LogMode() {
  if (REXCVAR_GET(clock_no_scaling))
    REXSYS_INFO("[clock] clock_no_scaling=true: guest clock follows host time; turbo unavailable");
  else
    REXSYS_INFO("[clock] SDK guest clock carries fractional ticks (runtime fix; ratio {}/{}){}",
                rex::chrono::Clock::guest_tick_ratio().first,
                rex::chrono::Clock::guest_tick_ratio().second,
                std::getenv("FABLE2_CLOCK_SDK_ROUNDING") ? "; FABLE2_CLOCK_SDK_ROUNDING ignored" : "");
}
}  // namespace fable2::guestclock

#elif defined(__APPLE__)

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <utility>

#include <dlfcn.h>

#include "fable2_clock_math.h"

// SDK clock state, exported by librexruntime.dylib (src/core/clock.cpp).
namespace rex::chrono {
extern std::mutex tick_mutex_;
extern uint64_t last_guest_tick_count_;
extern uint64_t last_host_tick_count_;
extern std::pair<uint64_t, uint64_t> guest_tick_ratio_;
}  // namespace rex::chrono

namespace fable2::guestclock {

using QueryFn = uint64_t (*)();

inline QueryFn SdkQuery() {
  static const QueryFn fn = reinterpret_cast<QueryFn>(
      dlsym(RTLD_NEXT, "_ZN3rex6chrono5Clock19QueryGuestTickCountEv"));
  return fn;
}

inline bool UseSdkRounding() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_CLOCK_SDK_ROUNDING");
    return v && v[0] == '1' && SdkQuery() != nullptr;
  }();
  return on;
}

// Guarded by rex::chrono::tick_mutex_.
inline fable2::clockmath::AnchoredClock g_clock;

// Called once after runtime setup: say which clock path is live.
inline void LogMode() {
  if (REXCVAR_GET(clock_no_scaling))
    REXSYS_INFO("[clock] clock_no_scaling=true: guest clock follows host time; turbo unavailable");
  else if (UseSdkRounding())
    REXSYS_INFO("[clock] FABLE2_CLOCK_SDK_ROUNDING=1: using SDK guest clock (drops fractional ticks)");
  else
    REXSYS_INFO("[clock] exact anchored guest clock (executable override; ratio {}/{})",
                rex::chrono::Clock::guest_tick_ratio().first,
                rex::chrono::Clock::guest_tick_ratio().second);
}

}  // namespace fable2::guestclock

uint64_t rex::chrono::Clock::QueryGuestTickCount() {
  namespace gc = fable2::guestclock;
  if (gc::UseSdkRounding()) return gc::SdkQuery()();
  const uint64_t host_tick_count = Clock::QueryHostTickCount();
  if (REXCVAR_GET(clock_no_scaling)) {
    // Same as the SDK: absolute host ticks converted on the fly.
    uint64_t remainder = 0;
    return fable2::clockmath::Convert(host_tick_count, guest_tick_ratio_, remainder);
  }
  std::unique_lock<std::mutex> lock(tick_mutex_, std::defer_lock);
  if (!lock.try_lock()) {
    // Another thread is updating; use its result (SDK behaviour).
    lock.lock();
    return last_guest_tick_count_;
  }
  return fable2::guestclock::g_clock.Update(host_tick_count, guest_tick_ratio_,
                                            last_host_tick_count_, last_guest_tick_count_);
}

#endif  // __APPLE__ && !REX_CLOCK_EXACT_GUEST_TICKS
