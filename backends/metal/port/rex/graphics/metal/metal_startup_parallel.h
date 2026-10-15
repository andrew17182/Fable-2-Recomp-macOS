#pragma once
// [startup-par] Startup pipeline work spread over the CPU cores (docs/native-shaders.md, "First launch").
// FABLE2_STARTUP_PARALLEL (default on, =0 serial as before): the backend's own compute pipelines built in
// SetupContext (texture loads, resolves, host depth stores, EDRAM dumps) and the cached shader translations
// installed before the startup pipeline warm-up are created on GCD worker threads. Each item writes its own
// result slot; the pipelines are the same objects the serial loops made. On a Mac whose Metal system shader
// cache is empty (first launch, new OS, new resolution scale) each create is a driver compile of 10-500 ms, so
// the serial loops cost 3-8 s of "Metal startup: preparation"; warm, the whole set takes a few milliseconds.
#include <dispatch/dispatch.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>

#include "rex/logging.h"
#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace rex::graphics::metal::startup {

inline bool ParallelEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_STARTUP_PARALLEL");
    const bool e = !(v && v[0] == '0');
    REXLOG_INFO("[startup-par] {} (FABLE2_STARTUP_PARALLEL)", e ? "on" : "off");
    return e;
  }();
  return on;
}

// Runs fn(i) for i in [0, n): on the GCD worker pool (the caller takes part) when enabled, else in order on
// the calling thread. Each item runs inside its own autorelease pool.
template <typename F>
void ParallelFor(size_t n, F&& fn) {
  if (n == 0) return;
  auto run_one = [&fn](size_t i) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    fn(i);
    pool->release();
  };
  if (n == 1 || !ParallelEnabled()) {
    for (size_t i = 0; i < n; ++i) run_one(i);
    return;
  }
  using RunOne = decltype(run_one);
  dispatch_apply_f(n, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &run_one,
                   [](void* context, size_t i) { (*static_cast<RunOne*>(context))(i); });
}

inline double MsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace rex::graphics::metal::startup
