// fable2_guest_speed.h - how fast the guest clock runs relative to real time (turbo).
//
// 1.0 normally; the turbo scale while turbo is on. Set by fable2::turbo::SetActive.
// Use this instead of rex::chrono::Clock::guest_time_scalar(): on the prebuilt runtime the turbo wait fix
// (fable2_turbo.h) makes the SDK scalar hold the host-time-per-guest-time factor (1/scale).
#pragma once
#include <atomic>

namespace fable2::guestspeed {
inline std::atomic<double> g_speed{1.0};
inline double Get() { return g_speed.load(std::memory_order_relaxed); }
}  // namespace fable2::guestspeed
