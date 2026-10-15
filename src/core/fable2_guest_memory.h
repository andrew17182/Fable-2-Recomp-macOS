// fable2_guest_memory.h - guest memory-ordering barriers and indirect-call
// crash diagnostics for the recompiled guest code (fable_2_recomp).
//
// Hooked in like fable2_func_trace.h: CMakeLists.txt appends this header to
// the fable_2_recomp precompile-header list AFTER the generated pch, so the
// generated macros are already defined when this file is processed.
//
// 1. Guest barriers (FABLE2_GUEST_LWSYNC / FABLE2_GUEST_SYNC)
//    ReXGlue 0.10.0 codegen emits PowerPC `sync` / `lwsync` as nothing
//    ("x86 has strong ordering"). Apple Silicon is weakly ordered like PPC,
//    so guest code that publishes data with `lwsync; stw` (spin-lock release,
//    pool free lists, lock-free queues) can have its stores observed out of
//    order by other host threads. tools/apply_recomp_patches.py inserts these
//    macros after every `// lwsync` / `// sync` line of the generated code.
//    On by default; FABLE2_GUEST_BARRIERS=0 (env, read once at startup)
//    turns them back into no-ops (switches: fable2_guest_barriers_flag.h).
//
// 2. Indirect-call diagnostics (FABLE2_CRASH_DIAG=1, off by default)
//    REX_CALL_INDIRECT_FUNC is redefined so that its existing cold path (the
//    target is not in the function table) first logs the target, the return
//    address (identifies the call site), the host thread name and the guest
//    registers/object words that usually supply the target (r3 = this,
//    r30/r31 = saved object registers). The call itself is unchanged: an
//    invalid target still ends in rex::runtime::InvalidFunctionTrap.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

#include "fable2_guest_barriers_flag.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace fable2::guestmem {

inline std::atomic<int> g_diag_lines{0};

// Copies guest memory without faulting on unmapped pages. Returns false if
// any byte could not be read.
inline bool SafeRead(const uint8_t* base, uint32_t guest, void* out, size_t len) {
#if defined(__APPLE__)
  mach_vm_size_t got = 0;
  kern_return_t kr = mach_vm_read_overwrite(
      mach_task_self(), (mach_vm_address_t)(uintptr_t)(base + guest), len,
      (mach_vm_address_t)(uintptr_t)out, &got);
  return kr == KERN_SUCCESS && got == len;
#else
  (void)base; (void)guest; (void)out; (void)len;
  return false;
#endif
}

inline uint32_t SafeU32(const uint8_t* base, uint32_t guest, bool* ok) {
  uint32_t raw = 0;
  *ok = guest != 0 && SafeRead(base, guest, &raw, 4);
  return *ok ? __builtin_bswap32(raw) : 0;
}

inline void DumpObject(const uint8_t* base, const char* label, uint32_t addr, char* buf, size_t cap) {
  size_t n = std::strlen(buf);
  if (n >= cap) return;
  uint8_t raw[32];
  if (addr == 0 || !SafeRead(base, addr, raw, sizeof(raw))) {
    std::snprintf(buf + n, cap - n, " %s=%08X[unreadable]", label, addr);
    return;
  }
  n += std::snprintf(buf + n, cap - n, " %s=%08X[", label, addr);
  for (int i = 0; i < 8 && n < cap; ++i) {
    uint32_t w;
    std::memcpy(&w, raw + i * 4, 4);
    n += std::snprintf(buf + n, cap - n, i ? " %08X" : "%08X", __builtin_bswap32(w));
  }
  if (n < cap) {
    // vtable (word 0) -> first 4 slots
    uint32_t vt;
    std::memcpy(&vt, raw, 4);
    vt = __builtin_bswap32(vt);
    bool ok = false;
    n += std::snprintf(buf + n, cap - n, "] vt[");
    for (int i = 0; i < 8 && n < cap; ++i) {
      uint32_t s = SafeU32(base, vt + i * 4, &ok);
      n += ok ? std::snprintf(buf + n, cap - n, i ? " %08X" : "%08X", s)
              : std::snprintf(buf + n, cap - n, i ? " ?" : "?");
    }
    if (n < cap) std::snprintf(buf + n, cap - n, "]");
  }
}

// Cold path only: the target was not found in the function table.
__attribute__((noinline, cold)) inline void OnUnresolvedIndirect(PPCContext& ctx, uint8_t* base,
                                                                 uint32_t target) {
  const bool out_of_range = (uint32_t)(target - REX_CODE_BASE) >= REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE;
  // In-range misses can be legitimate lazily-resolved entries; log a few only.
  int line = g_diag_lines.fetch_add(1, std::memory_order_relaxed);
  if (!out_of_range && line >= 20) return;
  char thread_name[64] = {};
  pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  char buf[1400];
  std::snprintf(buf, sizeof(buf),
                "[crash-diag] unresolved indirect call target=%08X %s ret_lr=%08X thread='%s' tid=%llu "
                "r3=%08X r4=%08X r5=%08X r10=%08X r11=%08X r29=%08X r30=%08X r31=%08X r1=%08X",
                target, out_of_range ? "OUT-OF-RANGE" : "in-range-miss", (uint32_t)ctx.lr, thread_name,
                (unsigned long long)tid, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r10.u32, ctx.r11.u32,
                ctx.r29.u32, ctx.r30.u32, ctx.r31.u32, ctx.r1.u32);
  std::fprintf(stderr, "%s\n", buf);
  REXCPU_ERROR("{}", buf);
  if (!out_of_range) return;
  buf[0] = 0;
  std::snprintf(buf, sizeof(buf), "[crash-diag] objects:");
  DumpObject(base, "r3", ctx.r3.u32, buf, sizeof(buf));
  DumpObject(base, "r30", ctx.r30.u32, buf, sizeof(buf));
  DumpObject(base, "r31", ctx.r31.u32, buf, sizeof(buf));
  std::fprintf(stderr, "%s\n", buf);
  REXCPU_ERROR("{}", buf);
  std::fflush(stderr);
}

}  // namespace fable2::guestmem

#define FABLE2_GUEST_LWSYNC()                                    \
  do {                                                           \
    if (::fable2::guestmem::g_barriers)                          \
      std::atomic_thread_fence(std::memory_order_acq_rel);       \
  } while (0)

#define FABLE2_GUEST_SYNC()                                      \
  do {                                                           \
    if (::fable2::guestmem::g_barriers)                          \
      std::atomic_thread_fence(std::memory_order_seq_cst);       \
  } while (0)

// Source-built SDK codegen (phase B3, patches/sdk/codegen-memory-barriers.patch) emits these
// itself when the manifest sets memory_barriers = true; route them through the runtime switch.
// The post-pass (tools/apply_recomp_patches.py) then finds every barrier already fenced.
#undef REX_GUEST_SYNC
#undef REX_GUEST_LWSYNC
#define REX_GUEST_SYNC() FABLE2_GUEST_SYNC()
#define REX_GUEST_LWSYNC() FABLE2_GUEST_LWSYNC()

// Stage 0b (perf pass 2026-10-11, patches/sdk/codegen-fctiw-rounding.patch): the source-built
// codegen emits REX_FCTIW / REX_FCTID for fctiw / fctid. Route them through the runtime switch
// FABLE2_FCTIW_ROUNDING (default on: round per the guest rounding mode; 0: the old SIMDe
// round-half-away-from-zero expressions). The prebuilt codegen still emits the old inline form.
#if defined(REX_FCTIW) && defined(REX_FCTID)
#undef REX_FCTIW
#undef REX_FCTID
#define REX_FCTIW(x)                                                              \
  (::fable2::guestmem::g_fctiw_rounding ? ::rex::ppc::FctiwCurrentMode(x)          \
                                        : ::rex::ppc::FctiwSimde(x))
#define REX_FCTID(x)                                                              \
  (::fable2::guestmem::g_fctiw_rounding ? ::rex::ppc::FctidCurrentMode(x)          \
                                        : ::rex::ppc::FctidSimde(x))
#endif

// Same as the generated definition, plus the diagnostic call on the cold path.
#undef REX_CALL_INDIRECT_FUNC
#define REX_CALL_INDIRECT_FUNC(x)                                                          \
  do {                                                                                     \
    REX_PROFILE_INDIRECT_DISPATCH();                                                       \
    uint32_t rex_indirect_target_ = (uint32_t)(x);                                         \
    PPCFunc* rex_indirect_func_;                                                           \
    if ((uint32_t)(rex_indirect_target_ - REX_CODE_BASE) <                                 \
        REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) [[likely]] {                               \
      rex_indirect_func_ = REX_LOOKUP_FUNC(base, rex_indirect_target_);                    \
    } else {                                                                               \
      rex_indirect_func_ = nullptr;                                                        \
    }                                                                                      \
    if (!rex_indirect_func_) [[unlikely]] {                                                \
      ctx.last_indirect_target = rex_indirect_target_;                                     \
      if (::fable2::guestmem::g_crash_diag)                                                \
        ::fable2::guestmem::OnUnresolvedIndirect(ctx, base, rex_indirect_target_);         \
      rex_indirect_func_ = rex::runtime::ResolveIndirectFunction(rex_indirect_target_);    \
    }                                                                                      \
    rex_indirect_func_(ctx, base);                                                         \
  } while (0)

// 3. CR-08 fast MSR sections (FABLE2_FAST_MSR, on unless set to 0;
//    docs/cr-fixes-20261007.md). The generated code brackets every guest
//    interrupts-off section (`mtmsrd r13,1` ... `mtmsrd rX,1`) with the SDK's
//    global recursive mutex, and every `mfmsr` locks it just to read a counter.
//    tools/apply_recomp_patches.py marks sections that hold only a
//    lwarx/stwcx. pair plus register instructions as FABLE2_MSR_ENTER_ATOMIC
//    (in Fable II that is all 8,406 of them: atomic add / CAS). Such a section
//    is atomic by itself (the stwcx. is a host CAS), so with the switch on it
//    takes no mutex; any other section still does. The interrupt-disable depth
//    is tracked per host thread (one guest thread = one host thread), which is
//    what mfmsr reports: under the global mutex the old counter could only ever
//    show the reading thread's own depth. With the switch off every macro is
//    the generated original. Fences emitted around mfmsr/mtmsrd are unchanged.
namespace fable2::guestmem {
inline thread_local uint32_t tl_msr_depth = 0;
inline thread_local uint64_t tl_msr_locked = 0;  // bit d: section at depth d took the mutex
inline void FastMsrEnter(bool lock) {
  const uint32_t d = tl_msr_depth++;
  if (d >= 64) lock = true;  // deep nesting: always lock (LEAVE unlocks for d >= 64)
  if (lock) {
    rex::thread::global_critical_region::mutex().lock();
    if (d < 64) tl_msr_locked |= (uint64_t(1) << d);
  } else {
    tl_msr_locked &= ~(uint64_t(1) << d);
  }
}
inline void FastMsrLeave() {
  if (!tl_msr_depth) return;  // unbalanced leave: nothing this thread holds
  const uint32_t d = --tl_msr_depth;
  if (d >= 64 || (tl_msr_locked >> d) & 1) rex::thread::global_critical_region::mutex().unlock();
}
}  // namespace fable2::guestmem

#undef REX_CHECK_GLOBAL_LOCK
#define REX_CHECK_GLOBAL_LOCK()                                          \
  ([&]() -> u64 {                                                        \
    if (::fable2::guestmem::g_fast_msr)                                  \
      return ::fable2::guestmem::tl_msr_depth ? 0 : 0x8000;              \
    auto lock_ = rex::thread::global_critical_region::AcquireDirect();   \
    return ppc_global_lock_count_().load() ? 0 : 0x8000;                 \
  }())

#undef REX_ENTER_GLOBAL_LOCK
#define REX_ENTER_GLOBAL_LOCK()                                          \
  do {                                                                   \
    if (::fable2::guestmem::g_fast_msr) {                                \
      ::fable2::guestmem::FastMsrEnter(true);                            \
    } else {                                                             \
      rex::thread::global_critical_region::mutex().lock();               \
      ppc_global_lock_count_().fetch_add(1);                             \
    }                                                                    \
  } while (0)

#define FABLE2_MSR_ENTER_ATOMIC()                                        \
  do {                                                                   \
    if (::fable2::guestmem::g_fast_msr) {                                \
      ::fable2::guestmem::FastMsrEnter(false);                           \
    } else {                                                             \
      rex::thread::global_critical_region::mutex().lock();               \
      ppc_global_lock_count_().fetch_add(1);                             \
    }                                                                    \
  } while (0)

#undef REX_LEAVE_GLOBAL_LOCK
#define REX_LEAVE_GLOBAL_LOCK()                                                             \
  do {                                                                                      \
    if (::fable2::guestmem::g_fast_msr) {                                                   \
      ::fable2::guestmem::FastMsrLeave();                                                   \
    } else {                                                                                \
      auto old_count_ = ppc_global_lock_count_().fetch_sub(1);                              \
      assert(old_count_ >= 1 && "LeaveGlobalLock called without matching EnterGlobalLock"); \
      rex::thread::global_critical_region::mutex().unlock();                                \
    }                                                                                       \
  } while (0)
