// fable2_fast_content_io.h - CR-01 (docs/cr-fixes-20261007.md, the 2026-10-07 code review).
//
// The SDK completes every overlapped XamContentCreate*/XamEnumerate call on its
// kernel dispatch thread after a fixed rex::thread::Sleep(100 ms)
// (kernel_state.cpp kDeferredOverlappedDelayMillis), one queued call after the
// other. Fable II mounts its save container with XamContentCreateInternal
// (xam ordinal 0x271), which it resolves at runtime through
// XexGetProcedureAddress and calls indirectly, so every save load and quick
// save waits ~100 ms per content call (three of them around Continue).
//
// FABLE2_FAST_CONTENT_IO (on unless set to 0):
// an overlapped call runs the SDK's own synchronous path (overlapped = 0) on
// the calling thread, then completes the guest XOVERLAPPED exactly as the
// SDK's deferred path would (result, extended error, length, event set,
// completion-routine APC) via KernelState::CompleteOverlappedImmediateEx, and
// returns ERROR_IO_PENDING as before. The work and its completion happen in
// call order, so content calls stay ordered. Only the sleep is gone.
//
// Hooks:
//   - __imp__XexGetProcedureAddress: after the real lookup of xam ordinal
//     0x271, the dispatch slot of the returned thunk is pointed at
//     FastXamContentCreateInternal (indirect calls dispatch through it).
//   - __imp__XamEnumerate (static import, direct calls).
//   Not hooked: XamShowDeviceSelectorUI (also ~100 ms, headless in the SDK).
//   Completing it at once was measured slower: the following menu-transition
//   wait (8235DB34 <826C8898) grew from ~22 to ~364 ms, title->load start went
//   0.13 -> 0.37 s (docs/cr-fixes-20261007.md), so the SDK path stays.
//   - __imp__XamContentCreateEx: fable2_skip_intro.h calls RunCreateEx().
// FABLE2_SKIP_INTRO_TRACE=1 or FABLE2_LOADING_TRACE=1 log each fast call.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <dlfcn.h>

#include <rex/logging/macros.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

namespace fable2::fastcontent {

using GuestFn = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kErrorIoPending = 0x3E5;
constexpr uint32_t kXamContentCreateInternalOrdinal = 0x271;

inline bool enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_FAST_CONTENT_IO");
    const bool r = !(v && v[0] == '0');
    REXSYS_INFO("[fast-content] {} (FABLE2_FAST_CONTENT_IO={})",
                r ? "on: overlapped XamContentCreate*/XamEnumerate complete without the SDK's 100 ms delay"
                  : "off",
                v ? v : "unset");
    return r;
  }();
  return on;
}

inline bool trace() {
  static const bool on = [] {
    const char* a = std::getenv("FABLE2_SKIP_INTRO_TRACE");
    const char* b = std::getenv("FABLE2_LOADING_TRACE");
    return (a && a[0] == '1') || (b && b[0] == '1');
  }();
  return on;
}

inline GuestFn next(const char* sym) {
  GuestFn f = reinterpret_cast<GuestFn>(dlsym(RTLD_NEXT, sym));
  if (!f) REXSYS_WARN("[fast-content] dlsym(RTLD_NEXT, {}) failed", sym);
  return f;
}

inline uint32_t HresultFromWin32(uint32_t x) {
  return static_cast<int32_t>(x) <= 0 ? x : ((x & 0xFFFF) | (7u << 16) | 0x80000000u);
}

inline double NowMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Runs `real` through the SDK's synchronous path: `set_args(ctx, base,
// overlapped, out_ptr)` writes the overlapped argument and the out-value
// pointer whose final value becomes the overlapped length (the caller's own
// pointer, or the overlapped length field when the caller passed none). Then
// the guest XOVERLAPPED is completed as the SDK's deferred path would and the
// call returns ERROR_IO_PENDING. All other guest registers are restored.
template <typename SetArgs>
inline void RunOverlapped(PPCContext& ctx, uint8_t* base, GuestFn real, const char* who,
                          uint32_t overlapped, uint32_t caller_out_ptr, SetArgs set_args) {
  const PPCContext saved = ctx;
  const double t0 = NowMs();
  const uint32_t out_ptr = caller_out_ptr ? caller_out_ptr : overlapped + 4;
  set_args(ctx, base, 0u, out_ptr);
  real(ctx, base);
  const uint32_t result = ctx.r3.u32;
  const uint32_t length = REX_LOAD_U32(out_ptr);
  ctx = saved;
  set_args(ctx, base, overlapped, caller_out_ptr);  // restore the guest arguments (stack slot for Ex)
  rex::system::kernel_state()->CompleteOverlappedImmediateEx(overlapped, result, HresultFromWin32(result),
                                                             length);
  ctx.r3.u64 = kErrorIoPending;
  static std::atomic<uint32_t> calls{0};
  const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (trace() || n <= 3)
    REXSYS_INFO("[fast-content] {} #{} overlapped={:08X} result={:#x} length={} completed in {:.2f} ms", who, n,
                overlapped, result, length, NowMs() - t0);
}

// XamContentCreateInternal(root r3, XCONTENT_AGGREGATE_DATA* r4, flags r5,
// disposition* r6, license_mask* r7, cache_size r8, content_size r9, overlapped r10)
inline GuestFn& RealContentCreateInternal() {
  static GuestFn f = nullptr;
  return f;
}

inline void FastXamContentCreateInternal(PPCContext& ctx, uint8_t* base) {
  GuestFn real = RealContentCreateInternal();
  if (!enabled() || !ctx.r10.u32) {
    real(ctx, base);
    return;
  }
  RunOverlapped(ctx, base, real, "XamContentCreateInternal", ctx.r10.u32, ctx.r6.u32,
                [](PPCContext& c, uint8_t*, uint32_t ovl, uint32_t out) {
                  c.r10.u64 = ovl;
                  c.r6.u64 = out;
                });
}

// XamContentCreateEx(user r3, root r4, XCONTENT_DATA* r5, flags r6, disposition* r7,
// license_mask* r8, cache_size r9, content_size r10, overlapped = 9th arg at r1+0x54)
inline bool RunCreateEx(PPCContext& ctx, uint8_t* base, GuestFn real) {
  if (!enabled()) return false;
  const uint32_t overlapped = REX_LOAD_U32(ctx.r1.u32 + 0x54);
  if (!overlapped) return false;
  RunOverlapped(ctx, base, real, "XamContentCreateEx", overlapped, ctx.r7.u32,
                [](PPCContext& c, uint8_t* base, uint32_t ovl, uint32_t out) {
                  REX_STORE_U32(c.r1.u32 + 0x54, ovl);
                  c.r7.u64 = out;
                });
  return true;
}

}  // namespace fable2::fastcontent

// XamEnumerate(handle r3, flags r4, buffer r5, buffer_length r6, items_returned* r7, overlapped r8)
extern "C" void __imp__XamEnumerate(PPCContext& ctx, uint8_t* base) {
  static const auto real = fable2::fastcontent::next("__imp__XamEnumerate");
  if (!fable2::fastcontent::enabled() || !ctx.r8.u32) {
    real(ctx, base);
    return;
  }
  // The SDK's overlapped path never writes items_returned; its synchronous
  // path writes the item count there. Point it at the overlapped length field
  // (the count becomes the overlapped length, as in the deferred path).
  const uint32_t guest_items = ctx.r7.u32;
  fable2::fastcontent::RunOverlapped(ctx, base, real, "XamEnumerate", ctx.r8.u32, 0,
                                     [guest_items](PPCContext& c, uint8_t*, uint32_t ovl, uint32_t out) {
                                       c.r8.u64 = ovl;
                                       c.r7.u64 = ovl ? guest_items : out;
                                     });
}

// XexGetProcedureAddress(hmodule r3, ordinal r4, out_function_ptr r5)
extern "C" void __imp__XexGetProcedureAddress(PPCContext& ctx, uint8_t* base) {
  static const auto real = fable2::fastcontent::next("__imp__XexGetProcedureAddress");
  static const bool logged = fable2::fastcontent::enabled();  // startup line with the switch state
  (void)logged;
  const uint32_t ordinal = ctx.r4.u32;
  const uint32_t out_ptr = ctx.r5.u32;
  real(ctx, base);
  if (ordinal != fable2::fastcontent::kXamContentCreateInternalOrdinal || ctx.r3.s32 < 0 || !out_ptr ||
      !fable2::fastcontent::enabled())
    return;
  static const auto sdk_impl = fable2::fastcontent::next("__imp__XamContentCreateInternal");
  const uint32_t thunk = REX_LOAD_U32(out_ptr);
  if (!thunk || (uint32_t)(thunk - REX_CODE_BASE) >= REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) {
    REXSYS_WARN("[fast-content] XamContentCreateInternal thunk {:08X} outside the dispatch table; not hooked",
                thunk);
    return;
  }
  PPCFunc* current = REX_LOOKUP_FUNC(base, thunk);
  if (current == &fable2::fastcontent::FastXamContentCreateInternal) return;  // already hooked
  if (!current) {
    REXSYS_WARN("[fast-content] XamContentCreateInternal thunk {:08X} has no dispatch entry; not hooked", thunk);
    return;
  }
  if (sdk_impl && current != sdk_impl)
    REXSYS_INFO("[fast-content] note: thunk {:08X} entry differs from dlsym(__imp__XamContentCreateInternal)",
                thunk);
  // Ordinal 0x271 of a module other than xam would be a different function;
  // only hook when the resolved entry is the SDK's XamContentCreateInternal.
  if (sdk_impl && current != sdk_impl) return;
  fable2::fastcontent::RealContentCreateInternal() = current;
  if (rex::system::kernel_state()->memory()->SetFunction(thunk, &fable2::fastcontent::FastXamContentCreateInternal))
    REXSYS_INFO("[fast-content] hooked XamContentCreateInternal thunk {:08X}", thunk);
  else
    REXSYS_WARN("[fast-content] SetFunction({:08X}) failed; XamContentCreateInternal not hooked", thunk);
}
