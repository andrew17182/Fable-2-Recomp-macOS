// Hand-tuned override: ring-buffer re-init without replay (FABLE2_RING_REINIT_FIX, 2026-10-09).
// The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// Bug (docs/boot-hang-20261008.md, bug-patterns row 27): the SDK's
// CommandProcessor::InitializeRingBuffer sets read_ptr_index_ = 0 on the calling guest thread but
// keeps write_ptr_index_ and does not synchronise with the GPU Commands worker. Fable II calls
// VdInitializeRingBuffer a third time (3D Engine) on ring 1FC85000 after GameThread used it up to
// index 0xF, so the worker replays ring[0, oldWPTR): duplicate GPU frame callbacks (skew in ~95%
// of boots) and, rarely, an endless spin in Present's frame throttle (82B6966C).
//
// Fix (default on; FABLE2_RING_REINIT_FIX=0 restores the old behaviour): a strong
// __imp__VdInitializeRingBuffer (takes precedence over librexruntime's export; the real one is
// reached with dlsym(RTLD_NEXT), as in src/core/fable2_skip_intro.h):
//   1. park: wait (bounded, kParkTimeoutUs) until the worker has consumed everything up to the
//      current write index (read == write), then swap the write index for the worker's
//      0xBAADF00D "no commands" sentinel. On timeout nothing is changed (old behaviour).
//   2. forward to the real init (read = 0, new base/size);
//   3. write index = 0 (CP_RB_WPTR = 0), so read == write; the guest then writes its WPTR.
// CommandProcessor lives in librexgpu-metal.dylib with hidden symbols and protected indices, so
// steps 1 and 3 are the plugin's exported fable2_ring_reinit (metal_graphics_system.cc), found with
// dlsym. Without it (Vulkan) the fix is inactive and the import is forwarded unchanged.
// FABLE2_THROTTLE_TRACE=1 also logs both ring imports (a strong __imp__ symbol can be defined only once).
//
// SDK fix (2026-10-09, phase B1 of docs/sdk-from-source.md): CommandProcessor::InitializeRingBuffer
// now does the drain / park / reset / WPTR=0 itself (patches/sdk/ring-reinit-sync.patch, also in
// backends/metal/sdk-metal.patch for the plugin). The plugin export's op 2 reports whether that is
// active (SDK cvar gpu_ring_reinit_sync, default true); then this override only forwards (and
// traces). The steps above run only when the SDK sync is off (--gpu_ring_reinit_sync=false) or the
// plugin predates it. A/B: default = SDK fix; --gpu_ring_reinit_sync=false = this workaround;
// plus FABLE2_RING_REINIT_FIX=0 = no fix.
#include "fable_2_pch.h"

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdlib>

#include <rex/logging/macros.h>
#include <rex/runtime.h>

namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);
using RingReinitFn = int (*)(int, rex::system::IGraphicsSystem*, uint32_t, uint32_t*);

constexpr uint32_t kParkTimeoutUs = 50'000;

bool EnvFlag(const char* name, bool fallback) {
  const char* v = std::getenv(name);
  if (!v || !v[0]) return fallback;
  return v[0] != '0';
}

bool FixOn() {
  static const bool on = EnvFlag("FABLE2_RING_REINIT_FIX", true);
  return on;
}

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_THROTTLE_TRACE");
    return v && v[0] == '1';
  }();
  return on;
}

GuestFn Next(const char* sym) {
  GuestFn f = reinterpret_cast<GuestFn>(dlsym(RTLD_NEXT, sym));
  if (!f) REXSYS_WARN("[ring-reinit] dlsym(RTLD_NEXT, {}) failed", sym);
  return f;
}

const char* ThreadName(char (&buf)[64]) {
  buf[0] = 0;
  pthread_getname_np(pthread_self(), buf, sizeof buf);
  return buf;
}

}  // namespace

extern "C" void __imp__VdInitializeRingBuffer(PPCContext& ctx, uint8_t* base) {
  static const GuestFn real = Next("__imp__VdInitializeRingBuffer");
  static const RingReinitFn reinit =
      reinterpret_cast<RingReinitFn>(dlsym(RTLD_DEFAULT, "fable2_ring_reinit"));
  static std::atomic<int> calls{0};
  const int call = calls.fetch_add(1);
  const uint32_t ptr = ctx.r3.u32;
  const int32_t size_log2 = ctx.r4.s32;
  char t[64];
  if (TraceOn()) {
    REXSYS_INFO("[throttle-trace] VdInitializeRingBuffer ptr={:08X} size_log2={} thread '{}' lr={:08X}", ptr,
                size_log2, ThreadName(t), uint32_t(ctx.lr));
  }

  auto* rt = rex::Runtime::instance();
  rex::system::IGraphicsSystem* gs = rt ? rt->graphics_system() : nullptr;
  if (reinit && gs && reinit(2, gs, 0, nullptr) == 1) {
    // The SDK synchronises the re-init itself; never park twice.
    if (call == 0) {
      REXSYS_INFO("[ring-reinit] SDK synchronises ring re-init (gpu_ring_reinit_sync); executable fix stands down");
    }
    if (real) real(ctx, base);
    if (TraceOn()) {
      REXSYS_INFO("[ring-reinit] init #{} ptr={:08X} size_log2={} thread '{}': forwarded to the SDK sync", call, ptr,
                  size_log2, ThreadName(t));
    }
    return;
  }
  if (!FixOn() || !reinit || !gs) {
    if (call == 0) {
      REXSYS_INFO("[ring-reinit] fix {} (FABLE2_RING_REINIT_FIX={}, plugin export {})",
                  FixOn() ? "inactive" : "off", FixOn() ? 1 : 0, reinit ? "found" : "missing");
    }
    if (real) real(ctx, base);
    return;
  }

  uint32_t info[3] = {};  // old write index, read index at start, waited us
  const auto t0 = std::chrono::steady_clock::now();
  const int parked = reinit(0, gs, kParkTimeoutUs, info);
  if (parked < 0) {
    if (call == 0) REXSYS_INFO("[ring-reinit] fix inactive (not the Metal backend)");
    if (real) real(ctx, base);
    return;
  }
  if (real) real(ctx, base);
  if (parked == 1) reinit(1, gs, 0, nullptr);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (parked == 1) {
    REXSYS_INFO("[ring-reinit] init #{} ptr={:08X} size_log2={} thread '{}': parked (old wptr={:X} read at "
                "start={:X}, drain wait {:.3f} ms), total {:.3f} ms",
                call, ptr, size_log2, ThreadName(t), info[0], info[1], info[2] / 1000.0, ms);
  } else {
    REXSYS_WARN("[ring-reinit] init #{} ptr={:08X} size_log2={} thread '{}': drain timeout after {:.3f} ms "
                "(wptr={:X} read at start={:X}); forwarded unchanged (old behaviour)",
                call, ptr, size_log2, ThreadName(t), info[2] / 1000.0, info[0], info[1]);
  }
}

extern "C" void __imp__VdEnableRingBufferRPtrWriteBack(PPCContext& ctx, uint8_t* base) {
  static const GuestFn real = Next("__imp__VdEnableRingBufferRPtrWriteBack");
  if (TraceOn()) {
    char t[64];
    REXSYS_INFO("[throttle-trace] VdEnableRingBufferRPtrWriteBack ptr={:08X} block_log2={} thread '{}' lr={:08X}",
                ctx.r3.u32, ctx.r4.s32, ThreadName(t), uint32_t(ctx.lr));
  }
  if (real) real(ctx, base);
}
