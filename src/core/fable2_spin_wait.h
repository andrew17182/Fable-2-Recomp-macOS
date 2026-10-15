// fable2_spin_wait.h - block instead of spin in two guest wait loops (docs/weak-mac-perf-20261008.md).
//
// Both loops are pure polling: the guest re-checks a condition that only time or another
// thread can change, so each kept a whole P-core busy (GameThread ~79%, 3D Engine ~72% of
// their samples were in these loops). On a 5-6 P-core Mac that competes with GPU Commands
// and the Metal driver threads.
//
// 1. GameThread tick wait (FABLE2_GAME_TICK_SLEEP, default on, =0 off). In
//    ProcessGameFrame_82276C30 the frame loop runs game work when the tick computed from the
//    guest timebase advances (r30 = r29 - r28 > 0), and otherwise calls
//    YieldAndCheckThreshold and loops. The next tick time is known exactly from the same
//    registers, so the thread sleeps until kGameTickMarginUs before it (capped at
//    kGameTickMaxSleepUs) and the original yield/re-check loop does the rest. The guest
//    computation is unchanged; only how often it is repeated changes.
// 2. 3D Engine ring wait (FABLE2_RING_WAIT, default on, =0 off). The D3D loop 82242628
//    polls 82B9BF90 until the command processor's read-pointer writeback passes a position.
//    After kRingSpinUs without progress the poll blocks on the Metal plugin's progress
//    event (signalled from MetalCommandProcessor::OnPrimaryBufferEnd, immediately before
//    the SDK stores the writeback), with a kRingWaitTimeoutUs safety timeout, then spins up
//    to kRingPostWakeSpinUs for the store. Only the call from 82242628 (lr 822426E8) waits.
//
// Perf pass 2026-10-11 (the 2026-10-11 performance audit, stage 1), each with its own switch:
// - FABLE2_RING_WAIT_NOSPIN (default on): the plugin signals progress after the writeback
//   store, so the ring wait re-reads once after waking instead of yield-spinning up to 200 us
//   (half of all wakes in the audit found no store and spun the whole 200 us; a timeout spun
//   too). Those yields were most of the ~590k swtch_pri syscalls/s at 60 FPS.
// - FABLE2_GAME_TICK_FINE (default on): a second, shorter precise sleep inside the 1 ms
//   margin before the tick, so the guest's own yield loop runs ~kGameTickFineMarginUs instead
//   of ~1 ms per tick.
// - FABLE2_GUEST_THREAD_QOS (default on): the 3D Engine and GameThread ask for the
//   user-interactive QoS class (GPU Commands already has it) so a P/E-core Mac keeps them on
//   P-cores. They ran at the default class (priority 31).
#pragma once

#include <dlfcn.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/event.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <thread>

#include <rex/chrono/clock.h>

#include "fable2_guest_speed.h"
#include <rex/logging/macros.h>

namespace fable2::spinwait {

using Clock = std::chrono::steady_clock;
inline int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

// macOS gives nanosleep/mach_wait_until/condition-variable timeouts a timer leeway of about
// 50% of the duration on this class of thread (measured: a 5 ms sleep_for returns 2.5 ms
// late, a 20 ms one 10 ms late). A one-shot kqueue timer with NOTE_CRITICAL asks for no
// coalescing (measured p50 overshoot 7-23 us, rare outliers ~1-2 ms).
inline void PreciseSleepUs(int64_t us) {
  if (us <= 0) return;
  thread_local int kq = kqueue();
  if (kq < 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
    return;
  }
  struct kevent ev;
  EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS | NOTE_CRITICAL, us, nullptr);
  struct kevent out;
  if (kevent(kq, &ev, 1, &out, 1, nullptr) < 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
  }
}

inline bool EnvOn(const char* name) {
  const char* v = std::getenv(name);
  return !(v && v[0] == '0');
}
// User-interactive QoS for the calling guest thread, once per thread (FABLE2_GUEST_THREAD_QOS).
inline void PromoteCurrentThreadQosOnce(const char* what) {
  thread_local bool done = false;
  if (done) return;
  done = true;
  static const bool on = EnvOn("FABLE2_GUEST_THREAD_QOS");
  if (!on) return;
  const int r = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  REXSYS_INFO("[thread-qos] {}: user-interactive QoS {}", what, r == 0 ? "set" : "failed");
}

inline bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_GPU_TIME_TRACE");
    return v && v[0] == '1';
  }();
  return on;
}

// ---------------------------------------------------------------------------
// 1. GameThread tick wait
// ---------------------------------------------------------------------------
constexpr double kGameTickMarginUs = 1000.0;  // wake this early, then the guest loop polls
constexpr double kGameTickMinSleepUs = 2000.0;  // shorter waits just poll as before
constexpr double kGameTickMaxSleepUs = 8000.0;  // re-check other loop exits at least this often
// Second stage (FABLE2_GAME_TICK_FINE): inside the margin, sleep again until this long before
// the tick when at least kGameTickFineMinUs remain; the guest's yield loop does the rest.
constexpr double kGameTickFineMarginUs = 250.0;
constexpr double kGameTickFineMinUs = 400.0;

inline bool GameTickFineEnabled() {
  static const bool on = [] {
    const bool e = EnvOn("FABLE2_GAME_TICK_FINE");
    REXSYS_INFO("[game-tick-sleep] fine stage {} (FABLE2_GAME_TICK_FINE)", e ? "on" : "off");
    return e;
  }();
  return on;
}

inline bool GameTickSleepEnabled() {
  static const bool on = [] {
    const bool e = EnvOn("FABLE2_GAME_TICK_SLEEP");
    REXSYS_INFO("[game-tick-sleep] {} (FABLE2_GAME_TICK_SLEEP={})",
                e ? "on: GameThread sleeps until shortly before its next tick instead of yield-spinning"
                  : "off",
                std::getenv("FABLE2_GAME_TICK_SLEEP") ? std::getenv("FABLE2_GAME_TICK_SLEEP") : "unset");
    return e;
  }();
  return on;
}

struct GameTickStats {
  uint64_t sleeps = 0, late = 0, fine_sleeps = 0, fine_late = 0;
  double slept_us = 0, max_over_us = 0;
  int64_t last_log_us = 0;
  int logged_samples = 0;
};

// tick_value = f6 (fractional tick count), last_tick = r28, rate = f25 (ticks per unit of
// (timebase - base) / divisor), divisor = f13: all as they are when the loop chose the
// yield branch.
inline void GameTickWait(double tick_value, int32_t last_tick, double rate, double divisor) {
  if (!GameTickSleepEnabled()) return;
  if (!(rate > 0) || !(divisor > 0) || !std::isfinite(tick_value)) return;
  const double ticks_needed = double(last_tick) + 1.0 - tick_value;
  if (!(ticks_needed > 0)) return;
  const double guest_ticks = ticks_needed / rate * divisor;  // guest timebase ticks
  const double freq = double(rex::chrono::Clock::guest_tick_frequency());
  double scalar = fable2::guestspeed::Get();  // guest speed (turbo scale), not the SDK duration scalar
  if (!(freq > 0)) return;
  if (!(scalar > 0)) scalar = 1.0;
  const double host_us = guest_ticks / freq / scalar * 1e6;
  thread_local GameTickStats st;
  if (st.logged_samples < 3) {
    ++st.logged_samples;
    REXSYS_INFO("[game-tick-sleep] sample: next tick in {:.0f} us (tick {:.3f} -> {}, rate {}, divisor {}, freq {}, scalar {})",
                host_us, tick_value, last_tick + 1, rate, divisor, freq, scalar);
  }
  if (host_us > 1e6) return;
  if (!(host_us >= kGameTickMinSleepUs)) {
    if (!(host_us >= kGameTickFineMinUs) || !GameTickFineEnabled()) return;
    const double target = host_us - kGameTickFineMarginUs;
    const int64_t t0 = NowUs();
    PreciseSleepUs(int64_t(target));
    const double actual = double(NowUs() - t0);
    ++st.fine_sleeps;
    st.slept_us += actual;
    const double over = actual - target;
    if (over > st.max_over_us) st.max_over_us = over;
    if (over > kGameTickFineMarginUs) ++st.fine_late;
  } else {
    const double target = std::min(host_us - kGameTickMarginUs, kGameTickMaxSleepUs);
    const int64_t t0 = NowUs();
    PreciseSleepUs(int64_t(target));
    const double actual = double(NowUs() - t0);
    ++st.sleeps;
    st.slept_us += actual;
    const double over = actual - target;
    if (over > st.max_over_us) st.max_over_us = over;
    if (over > kGameTickMarginUs) ++st.late;  // woke after the tick: the tick ran late by over-margin
  }
  if (TraceOn()) {
    const int64_t now = NowUs();
    if (!st.last_log_us) st.last_log_us = now;
    if (now - st.last_log_us > 10000000) {
      REXSYS_INFO("[game-tick-sleep] 10 s: {} sleeps, {:.0f} ms slept, {} woke after the tick, max overshoot {:.0f} us; "
                  "fine stage {} sleeps, {} woke after the tick",
                  st.sleeps, st.slept_us / 1000.0, st.late, st.max_over_us, st.fine_sleeps, st.fine_late);
      st = GameTickStats{0, 0, 0, 0, 0, 0, now, st.logged_samples};
    }
  }
}

// ---------------------------------------------------------------------------
// 2. 3D Engine ring wait
// ---------------------------------------------------------------------------
constexpr int64_t kRingSpinUs = 30;
constexpr uint32_t kRingWaitTimeoutUs = 2000;
constexpr int64_t kRingPostWakeSpinUs = 200;

using RingSeqFn = uint64_t (*)();
using RingWaitFn = bool (*)(uint64_t, uint32_t);

inline bool RingWaitNoSpin() {
  static const bool on = [] {
    const bool e = EnvOn("FABLE2_RING_WAIT_NOSPIN");
    REXSYS_INFO("[ring-wait] post-wake yield loop {} (FABLE2_RING_WAIT_NOSPIN)", e ? "off: progress is signalled after the writeback store" : "on");
    return e;
  }();
  return on;
}

inline bool RingWaitEnabled() {
  static const bool on = [] {
    const bool e = EnvOn("FABLE2_RING_WAIT");
    REXSYS_INFO("[ring-wait] {} (FABLE2_RING_WAIT={})",
                e ? "on: 3D Engine blocks on command-processor progress instead of spinning on the read pointer"
                  : "off",
                std::getenv("FABLE2_RING_WAIT") ? std::getenv("FABLE2_RING_WAIT") : "unset");
    return e;
  }();
  return on;
}

struct RingFns {
  RingSeqFn seq = nullptr;
  RingWaitFn wait = nullptr;
};
inline const RingFns* GetRingFns() {
  static std::atomic<const RingFns*> fns{nullptr};
  static std::atomic<int64_t> last_try{0};
  if (const RingFns* f = fns.load(std::memory_order_acquire)) return f;
  const int64_t now = NowUs();
  if (now - last_try.load(std::memory_order_relaxed) < 1000000) return nullptr;
  last_try.store(now, std::memory_order_relaxed);
  auto seq = reinterpret_cast<RingSeqFn>(dlsym(RTLD_DEFAULT, "fable2_ring_progress_seq"));
  auto wait = reinterpret_cast<RingWaitFn>(dlsym(RTLD_DEFAULT, "fable2_ring_progress_wait"));
  if (!seq || !wait) return nullptr;
  auto* f = new RingFns{seq, wait};
  fns.store(f, std::memory_order_release);
  REXSYS_INFO("[ring-wait] Metal plugin progress event found");
  return f;
}

struct RingStats {
  uint32_t last_rptr = 0;
  int64_t last_change_us = 0;
  uint64_t last_seen_seq = 0;
  uint64_t waits = 0, timeouts = 0, post_spin_misses = 0;
  double blocked_us = 0;
  int64_t last_log_us = 0;
};

// read_rptr: returns the current read-pointer writeback value (guest memory).
template <typename ReadRptr>
inline void RingPollWait(ReadRptr read_rptr) {
  if (!RingWaitEnabled()) return;
  const RingFns* fns = GetRingFns();
  if (!fns) return;
  thread_local RingStats st;
  const int64_t now = NowUs();
  uint32_t rptr = read_rptr();
  if (rptr != st.last_rptr || !st.last_change_us) {
    st.last_rptr = rptr;
    st.last_change_us = now;
    return;
  }
  if (now - st.last_change_us < kRingSpinUs) return;
  const uint64_t seq = fns->seq();
  if (seq != st.last_seen_seq) {
    // An event fired since we last looked: its writeback store may still be in flight
    // (the event precedes the store), so give the store a moment instead of blocking.
    st.last_seen_seq = seq;
    if (RingWaitNoSpin()) {
      rptr = read_rptr();  // the event follows the store: nothing more is coming for it
    } else {
      const int64_t t = NowUs();
      while ((rptr = read_rptr()) == st.last_rptr && NowUs() - t < kRingPostWakeSpinUs) {
        std::this_thread::yield();
      }
    }
  } else {
    rptr = read_rptr();
  }
  if (rptr != st.last_rptr) {
    st.last_rptr = rptr;
    st.last_change_us = NowUs();
    return;
  }
  const bool progressed = fns->wait(seq, kRingWaitTimeoutUs);
  st.last_seen_seq = fns->seq();
  ++st.waits;
  if (!progressed) ++st.timeouts;
  // The writeback store follows the event within microseconds.
  const int64_t woke = NowUs();
  if (RingWaitNoSpin()) {
    rptr = read_rptr();
  } else {
    while ((rptr = read_rptr()) == st.last_rptr && NowUs() - woke < kRingPostWakeSpinUs) {
      std::this_thread::yield();
    }
  }
  const int64_t end = NowUs();
  st.blocked_us += double(end - now);
  if (rptr != st.last_rptr) {
    st.last_rptr = rptr;
    st.last_change_us = end;
  } else if (progressed) {
    ++st.post_spin_misses;
  }
  if (TraceOn()) {
    if (!st.last_log_us) st.last_log_us = end;
    if (end - st.last_log_us > 10000000) {
      REXSYS_INFO("[ring-wait] 10 s: {} waits ({} timeouts, {} without a store after the event), {:.0f} ms blocked",
                  st.waits, st.timeouts, st.post_spin_misses, st.blocked_us / 1000.0);
      st.waits = st.timeouts = st.post_spin_misses = 0;
      st.blocked_us = 0;
      st.last_log_us = end;
    }
  }
}

}  // namespace fable2::spinwait
