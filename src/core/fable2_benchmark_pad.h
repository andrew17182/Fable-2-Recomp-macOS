// fable2_benchmark_pad.h - pad override for the built-in benchmark (src/core/fable2_benchmark.h).
//
// While `active` is set, the guest pad of user 0 is replaced at the same guest boundary the
// test-control replay-pad channel uses (fable2::session::ObservePad, the XamInputGetState
// wrapper), so the camera moves exactly as in tools/f2diag sweeps. Inert unless FABLE2_BENCHMARK
// is set: nothing ever sets `active` otherwise.
#pragma once

#include <atomic>
#include <cstdint>

namespace fable2::bench {
struct PadOverride {
  std::atomic<bool> active{false};
  std::atomic<int32_t> buttons{0}, rx{0}, ry{0}, lx{0}, ly{0};  // lx/ly: the flash plan walks
};
inline PadOverride g_pad;
}  // namespace fable2::bench
