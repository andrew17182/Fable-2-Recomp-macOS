// Turbo durations in microseconds (patches/sdk/turbo-fine-delay.patch, FABLE2_TURBO_FINE_DELAY).
//
// Under turbo the SDK scaled guest durations in whole milliseconds, so a 1 ms Delay/timeout/timer
// period at 2x truncated to 0 (a yield, a poll, a one-shot timer) and guest sleep loops spun.
// Checks: the scaling helper (0 = poll, infinite unscaled, at least 1 us, 1x unscaled), the
// microsecond waits really block for a sub-millisecond timeout (plain, alertable, multiple), and a
// sub-millisecond timer period stays periodic. Skips itself on SDK headers without the patch.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include <rex/chrono/clock.h>
#include <rex/thread.h>

#ifndef REX_THREAD_WAIT_MICROS
int main() {
  std::puts("turbo fine delay: SDK without turbo-fine-delay.patch, skipped");
  return 0;
}
#else
using namespace std::chrono;
using rex::chrono::Clock;
using rex::thread::WaitResult;

template <typename F>
static microseconds Timed(F&& f) {
  const auto t0 = steady_clock::now();
  f();
  return duration_cast<microseconds>(steady_clock::now() - t0);
}

int main() {
  if (const char* v = std::getenv("FABLE2_TURBO_FINE_DELAY"); v && v[0] == '0') {
    std::puts("turbo fine delay: switched off in the environment, skipped");
    return 0;
  }
  // 1x: the millisecond path stays in charge.
  Clock::set_guest_time_scalar(1.0);
  assert(!Clock::fine_guest_durations());

  Clock::set_guest_time_scalar(2.0);
  assert(Clock::fine_guest_durations());
  assert(Clock::ScaleGuestDurationMillisToMicros(0) == 0);  // poll stays a poll
  assert(Clock::ScaleGuestDurationMillisToMicros(1) == 500);
  assert(Clock::ScaleGuestDurationMillisToMicros(3) == 1500);
  constexpr uint32_t kInfinite = std::numeric_limits<uint32_t>::max();
  assert(Clock::ScaleGuestDurationMillisToMicros(kInfinite) == uint64_t(kInfinite) * 1000);
  assert(Clock::ScaleGuestDurationMillis(1) == 0);  // the old truncation this replaces
  Clock::set_guest_time_scalar(4.0);
  assert(Clock::ScaleGuestDurationMillisToMicros(1) == 250);

  auto event = rex::thread::Event::CreateManualResetEvent(false);
  auto other = rex::thread::Event::CreateManualResetEvent(false);
  // A 500 us timeout blocks for it (the old path returned at once), and 0 polls.
  for (bool alertable : {false, true}) {
    WaitResult r{};
    auto t = Timed([&] { r = rex::thread::WaitMicros(event.get(), alertable, microseconds(500)); });
    assert(r == WaitResult::kTimeout);
    assert(t >= microseconds(500) && t < milliseconds(500));
    t = Timed([&] { r = rex::thread::WaitMicros(event.get(), alertable, microseconds(0)); });
    assert(r == WaitResult::kTimeout && t < milliseconds(500));

    rex::thread::WaitHandle* handles[] = {event.get(), other.get()};
    std::pair<WaitResult, size_t> m{};
    t = Timed([&] {
      m = rex::thread::WaitMultipleMicros(handles, 2, false, alertable, microseconds(500));
    });
    assert(m.first == WaitResult::kTimeout);
    assert(t >= microseconds(500) && t < milliseconds(500));
  }
  other->Set();
  {
    rex::thread::WaitHandle* handles[] = {event.get(), other.get()};
    auto m = rex::thread::WaitMultipleMicros(handles, 2, false, false, microseconds(500));
    assert(m.first == WaitResult::kSuccess && m.second == 1);
    assert(rex::thread::WaitMicros(other.get(), false, microseconds(500)) == WaitResult::kSuccess);
  }

  // A 500 us period stays periodic (in milliseconds it was 0: a one-shot timer).
  auto timer = rex::thread::Timer::CreateSynchronizationTimer();
  assert(timer->SetRepeatingAfter(rex::chrono::hundrednanoseconds(0), microseconds(500)));
  int fired = 0;
  const auto end = steady_clock::now() + milliseconds(50);
  while (steady_clock::now() < end) {
    if (rex::thread::Wait(timer.get(), false, milliseconds(5)) == WaitResult::kSuccess) ++fired;
  }
  timer->Cancel();
  assert(fired >= 3);  // a one-shot fires once; loose for a loaded build machine

  Clock::set_guest_time_scalar(1.0);
  std::printf("turbo fine delay: ok (timer fired %d times in 50 ms at a 500 us period)\n", fired);
  return 0;
}
#endif
