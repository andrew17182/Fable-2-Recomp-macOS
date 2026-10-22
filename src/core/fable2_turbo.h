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
// UX: off at startup; number-row 7 or F8 toggles it live (src/input/window_keyboard_mouse.h,
// also via the background test channel key "7"). [patches] turbo_scale is the
// speed used while on (default 2.0; 1.1..4.0 in 0.1 steps - the SDK clock ratio
// is quantized to tenths; 1.0 disables the hotkey). FABLE2_TURBO_SPEED=<x>
// (Dev Launcher / Player "Turbo speed") overrides the scale; FABLE2_TURBO_SCALE=<x>
// overrides it AND starts turbo on (automated tests).
//
// A toggle only changes the guest/host tick ratio; the accumulated guest tick
// count is kept, so the guest clock does not jump (src/core/fable2_guest_clock.h).
//
// Slow mode (docs/turbo.md "Slow mode"): the same clock scalar below 1. The controller action
// FABLE2_PAD_SLOW toggles it; the ` key sets it (speed keys below). [patches]
// slow_scale is the speed while on (default 0.5; 0.25..0.9 in 0.05 steps, exact since
// patches/sdk/clock-scalar-precision.patch; 1.0 disables slow mode). FABLE2_SLOW_SPEED=<x>
// (launcher "Slow speed") overrides it, FABLE2_SLOW_SCALE=<x> overrides it AND starts slow
// mode on (tests). Turbo and slow mode are one speed setting: turning one on turns the
// other off. Audio uses the turbo audio stage at the slow rate. Frames: vblanks arrive
// slower, so at the 60 or 30 cap the frame-dispatch hook presents immediately and paces
// frames by real time (SlowPaceFrame) instead; uncapped runs as it does at 1x.
//
// Speed keys (docs/turbo.md "Speed keys"; window_keyboard_mouse.h): `/F1 = slow (slow_scale), 1/F2 = normal,
// 2/F3, 3/F4, 4/F5 = turbo at exactly 2x, 3x, 4x (the Turbo speed slider is ignored). They set a speed
// rather than toggle: one already in effect does nothing. State model: one mode (off, turbo, slow) plus
// the turbo speed, which is the slider's (turbo_scale) unless a speed key chose 2/3/4. 7/F8 still toggles:
// on = turbo at the slider speed, off from any turbo speed (so 7 after 2 returns to 1x). With turbo or slow
// disabled in settings (speed 1.0, the Original preset) the keys for it only show a notice.
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
#ifdef __APPLE__
#include <sys/event.h>
#endif
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

inline const char* EnvSlowScale() {
  const char* env = std::getenv("FABLE2_SLOW_SCALE");
  return env && *env ? env : nullptr;
}

// Slow-mode speed: 0.25..0.9 in 0.05 steps; 1.0 = slow mode disabled.
inline double ParseSlowScale(double value) {
  const double rounded = std::round(value * 20.0) / 20.0;
  if (rounded == 1.0) return 1.0;
  if (!(rounded >= 0.25 && rounded <= 0.9)) return -1.0;  // invalid
  return rounded;
}

inline double SlowScale() {
  static const double scale = [] {
    double value = fable2::config::Get().slow_scale;
    // FABLE2_SLOW_SPEED (launcher setting) then FABLE2_SLOW_SCALE (tests) override the TOML.
    for (const char* name : {"FABLE2_SLOW_SPEED", "FABLE2_SLOW_SCALE"}) {
      const char* env = std::getenv(name);
      if (!env || !*env) continue;
      char* end = nullptr;
      const double parsed = std::strtod(env, &end);
      if (end && !*end) value = parsed;
      else REXSYS_WARN("[turbo] ignoring invalid {}='{}'", name, env);
    }
    const double scale = ParseSlowScale(value);
    if (scale < 0) {
      REXSYS_WARN("[turbo] slow speed {} outside 0.25..0.9; slow mode disabled", value);
      return 1.0;
    }
    return scale;
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

// Guest speed mode: off (1x), turbo (ConfiguredScale) or slow (SlowScale). One at a time.
enum class Mode : int { kOff = 0, kTurbo = 1, kSlow = 2 };

inline std::atomic<int>& ModeFlag() {
  static std::atomic<int> mode{0};
  return mode;
}

inline Mode CurrentMode() { return Mode(ModeFlag().load(std::memory_order_relaxed)); }
// Turbo on.
inline bool Active() { return CurrentMode() == Mode::kTurbo; }
// Slow mode on.
inline bool SlowActive() { return CurrentMode() == Mode::kSlow; }

// Turbo speed chosen by a speed key (2.0, 3.0, 4.0); 0 = the slider's ConfiguredScale(). Written under
// ToggleMutex() by SetMode.
inline std::atomic<double>& DirectTurboSpeed() {
  static std::atomic<double> speed{0.0};
  return speed;
}

// Speed while turbo is on.
inline double TurboSpeed() {
  const double direct = DirectTurboSpeed().load(std::memory_order_relaxed);
  return direct > 1.0 ? direct : ConfiguredScale();
}

// Vblanks to wait per frame while turbo is active (original pacing is 2).
inline uint64_t FrameInterval() {
  return uint64_t(std::max(1L, std::lround(2.0 * TurboSpeed())));
}

// Slow mode at a frame cap: real-time frame pacing in the frame-dispatch hook (fable2_hook_fps_60),
// which then asks for an immediate present. Vblanks follow the guest clock, so at 0.5x a
// one-vblank (60) cap would give 30 real FPS; this holds `fps` real frames per second instead.
// A frame later than one period re-anchors (no catch-up burst). Guest render thread only.
inline void PaceSleepUs(int64_t us) {
  if (us <= 0) return;
#ifdef __APPLE__
  thread_local int kq = kqueue();
  if (kq >= 0) {
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS | NOTE_CRITICAL, us, nullptr);
    struct kevent out;
    if (kevent(kq, &ev, 1, &out, 1, nullptr) >= 0) return;
  }
#endif
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}

inline std::atomic<uint32_t>& SlowPaceEpoch() {  // bumped on each mode change: re-anchor
  static std::atomic<uint32_t> epoch{0};
  return epoch;
}

inline void SlowPaceFrame(double fps) {
  using SteadyClock = std::chrono::steady_clock;
  thread_local SteadyClock::time_point next{};
  thread_local uint32_t epoch = ~0u;
  const auto period = std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(1.0 / fps));
  const uint32_t now_epoch = SlowPaceEpoch().load(std::memory_order_relaxed);
  auto now = SteadyClock::now();
  if (epoch != now_epoch || now > next + period) {
    epoch = now_epoch;
    next = now + period;
    return;
  }
  if (next > now) {
    const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(next - now).count();
    // kqueue timer with NOTE_CRITICAL (fable2_spin_wait.h PreciseSleepUs): ~10-50 us late, no spin.
    PaceSleepUs(us);
  }
  next += period;
}

// Optional on-screen notice, installed by the app (src/core/fable2_turbo_notice.h).
inline void (*g_notice)(const std::string& text) = nullptr;
// Called after every mode change (src/core/fable2_frame_gen.h pauses frame generation while turbo is on).
inline void (*g_on_mode_change)() = nullptr;

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

inline bool ProbeSdkDividesDurations(double speed) {
  const uint32_t host_ms = rex::chrono::Clock::ScaleGuestDurationMillis(1000);
  // Fixed: 1000 / speed (500 at 2x, 2000 at 0.5x). Prebuilt: 1000 x speed.
  const bool divides = speed > 1.0 ? host_ms < 1000 : host_ms > 1000;
  SdkDividesFlag().store(divides ? 1 : 0, std::memory_order_relaxed);
  return divides;
}

inline std::mutex& ToggleMutex() {
  static std::mutex m;
  return m;
}

inline double ModeSpeed(Mode mode) {
  return mode == Mode::kTurbo ? TurboSpeed() : mode == Mode::kSlow ? SlowScale() : 1.0;
}

inline const char* ModeName(Mode mode) {
  return mode == Mode::kTurbo ? "turbo" : mode == Mode::kSlow ? "slow" : "off";
}

// Switches the guest speed mode (turbo, slow or off). Turning turbo on turns slow mode off and
// the other way round. A mode whose speed is 1.0 (disabled) is refused and the current mode kept.
// direct_turbo: turbo speed from a speed key (0 = the slider's); speed_key: speed-key notice wording.
inline void SetMode(Mode want, double direct_turbo = 0.0, bool speed_key = false) {
  std::lock_guard lock(ToggleMutex());
  const Mode was = CurrentMode();
  const double was_direct = DirectTurboSpeed().load(std::memory_order_relaxed);
  const double was_speed = ModeSpeed(was);
  if (want == Mode::kTurbo) DirectTurboSpeed().store(direct_turbo, std::memory_order_relaxed);
  if (want != Mode::kOff && ModeSpeed(want) == 1.0) {
    DirectTurboSpeed().store(was_direct, std::memory_order_relaxed);
    if (want == Mode::kTurbo)
      REXSYS_INFO("[turbo] turbo_scale is 1.0; turbo disabled (set [patches] turbo_scale)");
    else
      REXSYS_INFO("[turbo] slow_scale is 1.0; slow mode disabled (set [patches] slow_scale)");
    return;
  }
  if (rex::cvar::GetFlagSource("clock_no_scaling") == rex::cvar::Source::kCommandLine &&
      REXCVAR_GET(clock_no_scaling) && want != Mode::kOff) {
    REXSYS_WARN("[turbo] --clock_no_scaling=true blocks turbo and slow mode");
    DirectTurboSpeed().store(was_direct, std::memory_order_relaxed);
    return;
  }
  const double speed = ModeSpeed(want);
  // Guest ticks either side of the ratio change (continuity evidence in logs).
  const uint64_t before = rex::chrono::Clock::QueryGuestTickCount();
  rex::chrono::Clock::set_guest_time_scalar(speed);
  const bool applied = rex::chrono::Clock::guest_time_scalar() == speed;
  // Probe before any overwrite: at scalar s the fixed runtime returns 1000/s.
  if (applied && speed != 1.0) ProbeSdkDividesDurations(speed);
  const bool sdk_fixed = SdkDividesFlag().load(std::memory_order_relaxed) == 1;
  if (applied && WaitFix() && !sdk_fixed) rex::chrono::guest_time_scalar_ = 1.0 / speed;
  const Mode now = applied ? want : Mode::kOff;
  if (applied) fable2::guestspeed::g_speed.store(speed, std::memory_order_relaxed);
  const uint64_t after = rex::chrono::Clock::QueryGuestTickCount();
  ModeFlag().store(int(now), std::memory_order_relaxed);
  SlowPaceEpoch().fetch_add(1, std::memory_order_relaxed);
  if (applied) ApplyAudioRate(speed != 1.0, speed);
  fable2::fr::SetTag(fable2::fr::kTagTurbo, now == Mode::kTurbo ? 1 : 0);
  if (g_on_mode_change) g_on_mode_change();
  REXSYS_INFO(
      "[turbo] {} -> {} (speed {:.2f}x, guest speed {:.2f}, duration scalar {:.2f}{}, frame interval {}, "
      "audio {}, guest ticks {} -> {})",
      ModeName(was), ModeName(now), speed, fable2::guestspeed::Get(), rex::chrono::Clock::guest_time_scalar(),
      sdk_fixed ? " (SDK divides durations; turbo wait fix not needed)"
      : WaitFix() ? " (turbo wait fix; FABLE2_TURBO_WAIT_FIX=0 disables)"
                  : " (SDK, waits run long)",
      now == Mode::kTurbo  ? std::to_string(FrameInterval()) + " vblanks"
      : now == Mode::kSlow ? std::string("real-time pacing at the cap")
                           : std::string("2 vblanks"),
      AudioFollows() ? fable2::config::TurboAudioName(AudioMode()) : "normal (1x)", before, after);
  if (!g_notice) return;
  if (speed_key) {  // the speed now: "Speed 2x", "Slow 0.5x", "Normal speed"
    char text[48];
    if (now == Mode::kOff) std::snprintf(text, sizeof(text), "Normal speed");
    else std::snprintf(text, sizeof(text), "%s %gx", now == Mode::kTurbo ? "Speed" : "Slow", speed);
    g_notice(text);
    return;
  }
  // One notice per change: what is on now, else what went off.
  const Mode shown = now != Mode::kOff ? now : was;
  if (shown == Mode::kOff) return;
  char text[48];
  std::snprintf(text, sizeof(text), "%s %gx %s", shown == Mode::kTurbo ? "Turbo" : "Slow",
                now == shown ? speed : was_speed, now == shown ? "on" : "off");
  g_notice(text);
}

// Turbo on/off (off leaves slow mode alone).
inline void SetActive(bool on) {
  if (on) SetMode(Mode::kTurbo);
  else if (Active()) SetMode(Mode::kOff);
}

inline void SetSlow(bool on) {
  if (on) SetMode(Mode::kSlow);
  else if (SlowActive()) SetMode(Mode::kOff);
}

inline void ToggleSlow() { SetSlow(!SlowActive()); }

inline void Toggle() { SetActive(!Active()); }

// Speed keys: set the guest speed directly (see the header comment). speed 1.0 = normal; above 1 = turbo
// at exactly that speed; below 1 = slow mode at slow_scale. A speed already in effect does nothing.
inline void SetSpeed(double speed) {
  if (speed == 1.0) {
    if (CurrentMode() != Mode::kOff) SetMode(Mode::kOff, 0.0, true);
    return;
  }
  const bool turbo = speed > 1.0;
  if ((turbo ? ConfiguredScale() : SlowScale()) == 1.0) {  // disabled in settings (Original preset)
    REXSYS_INFO("[turbo] speed key {} ignored: {} is off in settings", turbo ? std::to_string(int(speed)) + "x" : "slow",
                turbo ? "turbo" : "slow mode");
    if (g_notice) g_notice(turbo ? "Turbo is off in settings" : "Slow is off in settings");
    return;
  }
  if (turbo ? Active() && TurboSpeed() == speed : SlowActive()) return;
  SetMode(turbo ? Mode::kTurbo : Mode::kSlow, turbo ? speed : 0.0, true);
}

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
                  "mode {}, speed {:.2f}",
                  (double(g1 - g0) / freq) / real, real,
                  (double(g1 - gstart) / freq) / total, total, ModeName(CurrentMode()),
                  fable2::guestspeed::Get());
      g0 = g1;
      t0 = t1;
    }
  }).detach();
}

// Called once after runtime setup. Turbo and slow mode start off unless FABLE2_TURBO_SCALE /
// FABLE2_SLOW_SCALE is set (tests; turbo wins if both are); 7/F8 toggles turbo, the speed keys set a speed.
inline void Init() {
  const double scale = ConfiguredScale();
  const double slow = SlowScale();
  const char* speed_env = std::getenv("FABLE2_TURBO_SPEED");
  const char* slow_env = std::getenv("FABLE2_SLOW_SPEED");
  REXSYS_INFO("[turbo] ready: 7/F8 toggles {:.1f}x (turbo_scale{}), `/F1 sets slow {:.2f}x (slow_scale{}), "
              "1-4/F2-F5 set 1x-4x, audio {}",
              scale, EnvScale() ? ", FABLE2_TURBO_SCALE" : speed_env && *speed_env ? ", FABLE2_TURBO_SPEED" : "",
              slow, EnvSlowScale() ? ", FABLE2_SLOW_SCALE" : slow_env && *slow_env ? ", FABLE2_SLOW_SPEED" : "",
              AudioFollows() ? fable2::config::TurboAudioName(AudioMode()) : "normal (1x)");
  if (EnvScale() && scale > 1.0) SetActive(true);
  else if (EnvSlowScale() && slow < 1.0) SetSlow(true);
  StartClockReport();
}

}  // namespace fable2::turbo
