// Stress test for patches/sdk/thread-exit-signal-safe.patch (2026-10-14 Flash Kit exit crash).
//
// KernelState::TerminateTitle wakes guest threads, they self-exit, and it then sends each one a
// user-callback signal (SIGUSR2 on macOS). On the unpatched runtime the signal handler touched a
// thread_local at entry; a signal landing while the thread was inside pthread_exit's TSD cleanup
// (malloc zone lock held) re-instantiated the dyld TLV block -> malloc -> "recursively lock an
// os_unfair_lock" abort. This test reproduces that shape directly: many rex::thread::Thread
// workers exit (half return, half call Thread::Exit) while hammer threads queue user callbacks
// (each one a SIGUSR2 while the thread has not published kFinished) for their whole exit. The
// patched runtime must survive every round.
//
// Usage: fable_2_thread_exit_signal_tests [rounds]   (default 2000, ~6 s; exit 0 = pass). 2026-10-14, M5 Max:
// unpatched runtime crashed 4/5 runs at 2000 rounds and 10/10 at 5000 (SIGSEGV in _tlv_get_addr <
// rex::thread::signal_handler < _sigtramp < _pthread_terminate); patched 10/10 pass at 5000.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rex/thread.h>

namespace {

// Per-thread state with destructors so pthread_exit's TSD cleanup does real work (frees),
// which widens the window the original crash needed.
struct Heavy {
  std::vector<std::string> v;
  Heavy() {
    for (int i = 0; i < 16; ++i) v.emplace_back(std::string(64 + i * 8, 'x'));
  }
};
thread_local std::unique_ptr<Heavy> tls_heavy;

}  // namespace

int main(int argc, char** argv) {
#ifndef REX_THREAD_EXIT_SIGNAL_SAFE
  (void)argc;
  (void)argv;
  std::puts("thread_exit_signal_tests: SDK without thread-exit-signal-safe.patch; skipped");
  return 0;
#else
  const int rounds = argc > 1 ? std::atoi(argv[1]) : 2000;
  constexpr int kThreads = 24;
  auto start = std::chrono::steady_clock::now();
  uint64_t signals = 0;
  for (int round = 0; round < rounds; ++round) {
    std::vector<std::unique_ptr<rex::thread::Thread>> threads;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    for (int i = 0; i < kThreads; ++i) {
      rex::thread::Thread::CreationParameters params;
      params.stack_size = 256 * 1024;
      const bool explicit_exit = (i & 1) != 0;
      auto t = rex::thread::Thread::Create(params, [&ready, &go, explicit_exit] {
        tls_heavy = std::make_unique<Heavy>();
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        if (explicit_exit) rex::thread::Thread::Exit(0);  // the XThread::Exit -> Terminate path
      });
      if (!t) {
        std::fprintf(stderr, "Thread::Create failed\n");
        return 2;
      }
      threads.push_back(std::move(t));
    }
    while (ready.load() < kThreads) std::this_thread::yield();
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> sent{0};
    std::vector<std::thread> hammers;
    for (int h = 0; h < 3; ++h) {
      hammers.emplace_back([&, h] {
        while (!stop.load(std::memory_order_acquire)) {
          for (size_t i = h; i < threads.size(); i += 3) {
            threads[i]->QueueUserCallback([] {});
            sent.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads) rex::thread::Wait(t.get(), false);
    // Keep signalling a little after the last exit was published.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    stop.store(true, std::memory_order_release);
    for (auto& h : hammers) h.join();
    signals += sent.load();
    threads.clear();
  }
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("thread_exit_signal: %d rounds x %d threads, %llu callbacks queued, %.1f s: PASS\n", rounds,
              kThreads, static_cast<unsigned long long>(signals), s);
  return 0;
#endif
}
