// Turbo: run the whole guest faster by scaling the SDK guest clock.
//
// Every guest time source goes through rex::chrono::Clock: the PPC timebase
// (mftb -> QueryGuestTickCount), the GPU vblank timer, system time and timed
// waits. Setting the guest time scalar therefore speeds up simulation,
// animation, AI and timers together. Audio follows through the SDK output
// stage (rex::audio::SetPlaybackRate, patches/sdk/audio-playback-rate.patch):
// the host consumes guest audio at the turbo rate and time-stretches
// (AUNewTimePitch) or resamples (AUVarispeed) it into real time, per
// [patches] turbo_audio / FABLE2_TURBO_AUDIO; "normal" keeps the old 1x audio.
//
// Rendering: vblanks arrive scale x faster, so the frame-dispatch hook
// (fable2_hook_fps_60 at 0x82B9C8E8) waits 2 x scale vblanks per frame while
// turbo is active, keeping ~30 real frames per second. Turbo overrides fps_60.
//
// UX: off at startup; F7 or number-row 7 toggles it live (src/input/window_keyboard_mouse.h,
// also via the background test channel key "f7"). [patches] turbo_scale is the
// speed used while on (default 2.0; 1.1..4.0 in 0.1 steps - the SDK clock ratio
// is quantized to tenths; 1.0 disables the hotkey). FABLE2_TURBO_SPEED=<x>
// (Dev Launcher / Player "Turbo speed") overrides the scale; FABLE2_TURBO_SCALE=<x>
// overrides it AND starts turbo on (automated tests).
//
// A toggle only changes the guest/host tick ratio; the accumulated guest tick
// count is kept, so the guest clock does not jump (src/core/fable2_guest_clock.h).
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <rex/chrono/clock.h>
#if __has_include(<rex/audio/downmix.h>)
#include <rex/audio/downmix.h>
#endif
#include "fable2_flight_recorder.h"
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include "fable2_config.h"
#include "fable2_guest_speed.h"

// SDK state (librexruntime, src/core/clock.cpp): the scalar ScaleGuestDuration* and the
// XSystemClock<->WinSystemClock casts read.
namespace rex::chrono {
extern double guest_time_scalar_;
}  // namespace rex::chrono

namespace fable2::turbo {

inline const char* EnvScale() {
  const char* env = std::getenv("FABLE2_TURBO_SCALE");
  return env && *env ? env : nullptr;
}

inline double ConfiguredScale() {
  static const double scale = [] {
    double value = fable2::config::Get().turbo_scale;
    // FABLE2_TURBO_SPEED (launcher setting) then FABLE2_TURBO_SCALE (tests) override the TOML.
    for (const char* name : {"FABLE2_TURBO_SPEED", "FABLE2_TURBO_SCALE"}) {
      const char* env = std::getenv(name);
      if (!env || !*env) continue;
      char* end = nullptr;
      const double parsed = std::strtod(env, &end);
      if (end && !*end) value = parsed;
      else REXSYS_WARN("[turbo] ignoring invalid {}='{}'", name, env);
    }
    if (!(value >= 1.0 && value <= 4.0)) {
      REXSYS_WARN("[turbo] scale {} outside 1.0..4.0; turbo disabled", value);
      value = 1.0;
    }
    return std::round(value * 10.0) / 10.0;
  }();
  return scale;
}

// Audio while turbo is on: [patches] turbo_audio, FABLE2_TURBO_AUDIO overrides.
inline fable2::config::Values::TurboAudio AudioMode() {
  static const auto mode = [] {
    auto value = fable2::config::Get().turbo_audio;
    if (const char* env = std::getenv("FABLE2_TURBO_AUDIO"); env && *env) {
      if (const auto parsed = fable2::config::ParseTurboAudio(env)) value = *parsed;
      else REXSYS_WARN("[turbo] ignoring FABLE2_TURBO_AUDIO='{}' (natural|high-pitch|normal)", env);
    }
    return value;
  }();
  return mode;
}

// Tell the SDK output stage how fast to consume guest audio (rate 1 = original path).
inline void ApplyAudioRate(bool on, double scale) {
#if defined(REX_AUDIO_PLAYBACK_RATE)
  using TA = fable2::config::Values::TurboAudio;
  const TA mode = AudioMode();
  const auto sdk_mode = mode == TA::kNatural     ? rex::audio::PlaybackRateMode::kNaturalPitch
                        : mode == TA::kHighPitch ? rex::audio::PlaybackRateMode::kHighPitch
                                                 : rex::audio::PlaybackRateMode::kNormal;
  rex::audio::SetPlaybackRate(on ? float(scale) : 1.0f, sdk_mode);
#else
  (void)on;
  (void)scale;
#endif
}

inline bool AudioFollows() {
#if defined(REX_AUDIO_PLAYBACK_RATE)
  return AudioMode() != fable2::config::Values::TurboAudio::kNormal;
#else
  return false;
#endif
}

inline std::atomic<bool>& ActiveFlag() {
  static std::atomic<bool> active{false};
  return active;
}

inline bool Active() { return ActiveFlag().load(std::memory_order_relaxed); }

// Vblanks to wait per frame while turbo is active (original pacing is 2).
inline uint64_t FrameInterval() {
  return uint64_t(std::max(1L, std::lround(2.0 * ConfiguredScale())));
}

// Optional on-screen notice, installed by the app (src/core/fable2_turbo_notice.h).
inline void (*g_notice)(const std::string& text) = nullptr;

// Turbo wait fix for the prebuilt runtime (default on; FABLE2_TURBO_WAIT_FIX=0 restores the SDK
// behaviour). The source-built runtime has the real fix (patches/sdk/clock-duration-scaling.patch,
// docs/sdk-from-source.md B2); SdkDividesDurations() detects it and this workaround stands down,
// since both together would invert the scalar twice (Sleep(100) -> 200 ms at 2x).
// set_guest_time_scalar(s) speeds the guest tick ratio up by s, but the prebuilt SDK uses the
// same scalar the other way round for durations: Clock::ScaleGuestDurationMillis /
// ScaleGuestDurationFileTime multiply a guest duration by it (clock.cpp:188-223), and so does
// the XSystemClock->WinSystemClock cast (chrono.h). Those convert guest time to host time, so
// they need 1/s: at 2x a guest Sleep(100) took 200 ms instead of 50, and 500 ms timeouts 1000.
// The callers live inside librexruntime (XThread::Delay = KeDelayExecutionThread,
// XObject::Wait/WaitMultiple/SignalAndWait timeouts, XTimer due times and periods, xam_net
// select), where an executable override of the functions would not bind. Nothing else reads
// guest_time_scalar_ (the tick ratio is computed once in set_guest_time_scalar), so after it the
// scalar is overwritten with 1/s; the guest clock ratio is unchanged. Code that needs the
// guest speed reads fable2::guestspeed::Get().
inline bool WaitFix() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TURBO_WAIT_FIX");
    return !(v && *v == '0');
  }();
  return on;
}

// Runtime capability probe, valid right after set_guest_time_scalar(s) with s > 1: a fixed runtime
// converts a guest duration to host time by dividing by s (1000 -> 500 ms at 2x), the prebuilt one
// multiplies (2000). -1 = not probed yet (turbo never on), 0 = prebuilt behaviour, 1 = fixed.
inline std::atomic<int>& SdkDividesFlag() {
  static std::atomic<int> state{-1};
  return state;
}

inline bool ProbeSdkDividesDurations() {
  const uint32_t host_ms = rex::chrono::Clock::ScaleGuestDurationMillis(1000);
  const bool divides = host_ms < 1000;
  SdkDividesFlag().store(divides ? 1 : 0, std::memory_order_relaxed);
  return divides;
}

inline std::mutex& ToggleMutex() {
  static std::mutex m;
  return m;
}

inline void SetActive(bool on) {
  std::lock_guard lock(ToggleMutex());
  const double scale = ConfiguredScale();
  if (scale <= 1.0) {
    if (on) REXSYS_INFO("[turbo] turbo_scale is 1.0; turbo disabled (set [patches] turbo_scale)");
    on = false;
  }
  if (rex::cvar::GetFlagSource("clock_no_scaling") == rex::cvar::Source::kCommandLine &&
      REXCVAR_GET(clock_no_scaling) && on) {
    REXSYS_WARN("[turbo] --clock_no_scaling=true blocks turbo");
    on = false;
  }
  // Guest ticks either side of the ratio change (continuity evidence in logs).
  const uint64_t before = rex::chrono::Clock::QueryGuestTickCount();
  rex::chrono::Clock::set_guest_time_scalar(on ? scale : 1.0);
  const bool applied = rex::chrono::Clock::guest_time_scalar() == (on ? scale : 1.0);
  // Probe before any overwrite: at scalar s the fixed runtime returns 1000/s.
  if (applied && on) ProbeSdkDividesDurations();
  const bool sdk_fixed = SdkDividesFlag().load(std::memory_order_relaxed) == 1;
  if (applied && WaitFix() && !sdk_fixed) rex::chrono::guest_time_scalar_ = on ? 1.0 / scale : 1.0;
  if (applied) fable2::guestspeed::g_speed.store(on ? scale : 1.0, std::memory_order_relaxed);
  const uint64_t after = rex::chrono::Clock::QueryGuestTickCount();
  ActiveFlag().store(on, std::memory_order_relaxed);
  if (applied) ApplyAudioRate(on, scale);
  fable2::fr::SetTag(fable2::fr::kTagTurbo, on ? 1 : 0);
  REXSYS_INFO(
      "[turbo] {} (scale {:.1f}x, guest speed {:.2f}, duration scalar {:.2f}{}, frame interval {} vblanks, "
      "audio {}, guest ticks {} -> {})",
      on ? "ON" : "off", scale, fable2::guestspeed::Get(), rex::chrono::Clock::guest_time_scalar(),
      sdk_fixed ? " (SDK divides durations; turbo wait fix not needed)"
      : WaitFix() ? " (turbo wait fix; FABLE2_TURBO_WAIT_FIX=0 disables)"
                  : " (SDK, waits run long)",
      on ? FrameInterval() : 2,
      AudioFollows() ? fable2::config::TurboAudioName(AudioMode()) : "normal (1x)", before, after);
  if (g_notice && scale > 1.0) {
    char text[48];
    std::snprintf(text, sizeof(text), "Turbo %gx %s", scale, on ? "on" : "off");
    g_notice(text);
  }
}

inline void Toggle() { SetActive(!Active()); }

// FABLE2_CLOCK_REPORT=<seconds>: log the guest-clock rate against real time
// (validation of the clock fix and turbo ratios).
inline void StartClockReport() {
  const char* env = std::getenv("FABLE2_CLOCK_REPORT");
  if (!env || !*env) return;
  const double seconds = std::max(0.5, std::atof(env));
  std::thread([seconds] {
    using SteadyClock = std::chrono::steady_clock;
    const double freq = double(rex::chrono::Clock::guest_tick_frequency());
    uint64_t g0 = rex::chrono::Clock::QueryGuestTickCount();
    auto t0 = SteadyClock::now();
    const uint64_t gstart = g0;
    const auto tstart = t0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
      const uint64_t g1 = rex::chrono::Clock::QueryGuestTickCount();
      const auto t1 = SteadyClock::now();
      const double real = std::chrono::duration<double>(t1 - t0).count();
      const double total = std::chrono::duration<double>(t1 - tstart).count();
      REXSYS_INFO("[clock-report] guest/real {:.4f} over {:.2f}s (cumulative {:.4f} over {:.1f}s), "
                  "turbo {}, speed {:.2f}",
                  (double(g1 - g0) / freq) / real, real,
                  (double(g1 - gstart) / freq) / total, total, Active() ? "on" : "off",
                  fable2::guestspeed::Get());
      g0 = g1;
      t0 = t1;
    }
  }).detach();
}

// Called once after runtime setup. Turbo starts off unless FABLE2_TURBO_SCALE
// is set (tests); F7/7 toggles it.
inline void Init() {
  const double scale = ConfiguredScale();
  const char* speed_env = std::getenv("FABLE2_TURBO_SPEED");
  REXSYS_INFO("[turbo] ready: F7/7 toggles {:.1f}x (turbo_scale{}), turbo audio {}", scale,
              EnvScale() ? ", FABLE2_TURBO_SCALE" : speed_env && *speed_env ? ", FABLE2_TURBO_SPEED" : "",
              AudioFollows() ? fable2::config::TurboAudioName(AudioMode()) : "normal (1x)");
  if (EnvScale() && scale > 1.0) SetActive(true);
  StartClockReport();
}

}  // namespace fable2::turbo
