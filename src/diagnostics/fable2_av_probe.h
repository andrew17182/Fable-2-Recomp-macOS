#pragma once
// fable2_av_probe.h — app-side VEH that logs the guest state at every
// access-violation (AV) raised inside the emulated process.
//
// Background: the Rexglue SDK logs "Unhandled guest access violation: read of
// guest 0x0" but only dumps the guest registers for fully-unhandled
// exceptions. When the guest's own SEH/C++ handlers swallow the AV the game
// keeps running in a broken state (tight AV loop), and we get no register
// dump. This probe registers a FIRST-priority vectored exception handler and,
// for each AV, identifies the live PPCContext by scanning the host GPRs
// (recompiled guest functions take `PPCContext& ctx` which codegen moves to a
// callee-saved register after the first call) and logs:
//   av fg=0x... lr=0x... r1=0x... r3=0x... r4=0x... r5=0x... rip=+0x... tid=0x...
//
//   fg   = guest fault address (host fault addr - 0x200000000 membase)
//   lr   = guest link register -> return site in the CALLER of the faulting
//          guest function (maps to generated/default/fable_2_recomp.*.cpp)
//   rip  = host instruction pointer, module-relative (ASLR-stable; maps via
//          the exe PDB to the recompiled function `sub_<guestaddr>` that
//          actually faulted)
//
// CRITICAL: this handler runs DURING exception dispatch. It must NEVER raise
// a nested AV, so every memory read is guarded with VirtualQuery (checked for
// MEM_COMMIT) instead of __try/__except. A register may hold the guest
// `base` (0x200000000) or other unmapped guest addresses; reading through
// them would fault. We reject any candidate in the guest-memory region before
// touching it, and VirtualQuery-guard the rest.
//
// Self-installing: `static Registrar g_av_probe_registrar` (below) is
// constructed at process start, before any guest code runs. Purely
// observational: always returns EXCEPTION_CONTINUE_SEARCH.
//
// Log file: <exe dir>\fable2_av_probe.log. Rate-limited: first 10 events per
// (fault, rip) key, then every 2048th; global cap 20000 lines.

#ifndef FABLE2_AV_PROBE_H
#define FABLE2_AV_PROBE_H

#include <Windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fable2_av_probe {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Guest memory layout (see generated/default/fable_2_pch.h, REX_* macros):
// recompiled code reads/writes through `base + guest_addr`, with the host
// membase at 0x200000000 (guest 0x0 -> host 0x200000000) and a 4 GiB region.
constexpr u64 kMemBase = 0x200000000ull;
constexpr u64 kMemSize = 0x100000000ull;

// Guest code region (recompiled functions sub_<addr>; observed 0x82170038 up
// to ~0x833Bxxxx). Used to validate a candidate PPCContext's lr.
constexpr u64 kGuestCodeMin = 0x82000000ull;
constexpr u64 kGuestCodeMax = 0x83500000ull;
// Guest main-thread stack region (observed r1 = 0x83A0A278).
constexpr u64 kGuestStackMin = 0x83800000ull;
constexpr u64 kGuestStackMax = 0x83C00000ull;

// PPCContext layout (thirdparty/rexglue-sdk-src/include/rex/ppc/context.h):
//   struct alignas(0x40) PPCContext {
//     r3@0x00 r0@0x08 r1@0x10 r2@0x18 r4@0x20 r5@0x28 ... r31@0xF8
//     lr@0x100 ctr@0x108 xer@0x110 ...
//   }
inline constexpr u64 kR1Offset = 0x10;
inline constexpr u64 kLROffset = 0x100;
inline constexpr u64 kR3Offset = 0x00;
inline constexpr u64 kR4Offset = 0x20;
inline constexpr u64 kR5Offset = 0x28;
inline constexpr u64 kCtxSize = 0x110;  // bytes of the struct we read

// True if `addr` lies inside the guest-memory host region. A real host
// PPCContext& is never here, so we reject such candidates before reading.
inline bool in_guest_mem(u64 addr) {
  return addr >= kMemBase && addr < kMemBase + kMemSize;
}

// VirtualQuery-guarded 64-bit read. Never raises an AV: if the page is not
// committed (or the range spans regions) we return false without touching it.
inline bool safe_read64(void* p, u64& out) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
  if (mbi.State != MEM_COMMIT) return false;
  const u64 start = reinterpret_cast<u64>(mbi.BaseAddress);
  const u64 end = start + mbi.RegionSize;
  const u64 addr = reinterpret_cast<u64>(p);
  if (addr + 8 > end) return false;
  out = *reinterpret_cast<u64*>(p);
  return true;
}

// A register value is a live PPCContext iff:
//   - it is 0x40-aligned (PPCContext is alignas(0x40)),
//   - it is NOT in the guest-memory region,
//   - the struct is committed host memory,
//   - its r1 is a plausible guest stack pointer, and
//   - its lr is in the guest code range.
inline bool is_guest_ctx(u64 p) {
  if (p < 0x40ull || (p & 0x3Full) != 0) return false;
  if (in_guest_mem(p)) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == 0)
    return false;
  if (mbi.State != MEM_COMMIT) return false;
  const u64 start = reinterpret_cast<u64>(mbi.BaseAddress);
  if (p + kCtxSize > start + mbi.RegionSize) return false;
  u64 r1 = 0, lr = 0;
  r1 = *reinterpret_cast<u64*>(p + kR1Offset);
  if (r1 < kGuestStackMin || r1 > kGuestStackMax) return false;
  lr = *reinterpret_cast<u64*>(p + kLROffset);
  return lr >= kGuestCodeMin && lr <= kGuestCodeMax;
}

struct ProbeState {
  FILE* log = nullptr;
  u64 module_base = 0;
  // Rate-limit table (small, fixed; guest AVs concentrate on few sites).
  u64 keys[256];
  u32 counts[256];
  u32 total = 0;
  bool locked = false;
  // Diagnostic: raw exception-code histogram, logged (rate-limited) regardless
  // of guest-ctx matching, so we can tell whether the VEH is being invoked at
  // all and for which codes. Keyed by exception code; capped.
  u32 seen_total = 0;       // total exceptions the VEH saw
  u32 seen_av = 0;          // total AVs the VEH saw
  u32 diag_logged = 0;      // how many diagnostic lines we've emitted
};

inline ProbeState& state() {
  static ProbeState s;
  return s;
}

inline void log_line(const char* fmt, ...) {
  ProbeState& s = state();
  if (!s.log) return;
  va_list ap;
  va_start(ap, fmt);
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fputs(buf, s.log);
  fputc('\n', s.log);
  fflush(s.log);
}

// (fault, rip) -> slot; logs first 10, then every 2048th.
inline bool should_log(u64 fault, u64 rip) {
  ProbeState& s = state();
  if (s.total >= 20000) return false;
  u64 key = (fault ^ (rip >> 4)) & 0xFFFFFFFFull;
  u64 h = (fault * 0x9E3779B97F4A7C15ull) ^ (rip >> 4);
  u32 slot = static_cast<u32>(h % (sizeof(s.keys) / sizeof(s.keys[0])));
  if (s.keys[slot] == 0) {
    s.keys[slot] = key;
    s.counts[slot] = 1;
    s.total++;
    return true;
  }
  if (s.keys[slot] != key) {
    // Slot busy with a different key: reuse only if the old one is cold.
    if (s.counts[slot] > 10) return false;
    s.keys[slot] = key;
    s.counts[slot] = 1;
    s.total++;
    return true;
  }
  s.counts[slot]++;
  s.total++;
  return s.counts[slot] <= 10 || (s.counts[slot] & 0x7FF) == 0;
}

LONG WINAPI handler(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;

  CONTEXT* c = ep->ContextRecord;
  if (!c) return EXCEPTION_CONTINUE_SEARCH;

  ProbeState& s = state();
  if (!s.log) return EXCEPTION_CONTINUE_SEARCH;
  if (s.locked) return EXCEPTION_CONTINUE_SEARCH;  // reentrancy guard
  s.locked = true;

  // Diagnostic: is the VEH being invoked at all? Log a bounded histogram of
  // (code, read/write) so we can see whether guest AVs reach us. Independent
  // of the rate-limited guest-ctx logging below.
  if (s.diag_logged < 4096) {
    const u64 code = ep->ExceptionRecord->ExceptionCode;
    const int access = ep->ExceptionRecord->ExceptionInformation[1];
    if (code == EXCEPTION_ACCESS_VIOLATION) s.seen_av++;
    s.seen_total++;
    // Log the first ~32 events and then every 4096th, with a hard cap.
    if (s.seen_total <= 32 || (s.seen_total % 4096) == 0) {
      log_line("diag av_seen=%u total_seen=%u code=0x%08llX access=%d rip=+0x%llX "
               "tid=0x%llX", s.seen_av, s.seen_total, (unsigned long long)code,
               access, (unsigned long long)(c->Rip - s.module_base),
               (unsigned long long)GetCurrentThreadId());
      s.diag_logged++;
    }
  }

  const u64 fault = ep->ExceptionRecord->ExceptionInformation[0];
  const int access = ep->ExceptionRecord->ExceptionInformation[1];
  const u64 rip = c->Rip;

  // Rate-limit BEFORE the (relatively expensive) register scan: for
  // storm-dominated keys we skip the scan entirely.
  if (should_log(fault, rip)) {
    const u64 rip_rel = rip - s.module_base;
    const u64 fg = (fault >= kMemBase && fault < kMemBase + kMemSize)
                       ? fault - kMemBase
                       : fault;
    const char* kind =
        access == 0 ? "read" : (access == 1 ? "write" : "cf");

    // Scan all GPRs for the live PPCContext. Callee-saved first: codegen
    // moves the `ctx` parameter into one of these in any function that makes
    // calls. Every candidate is VirtualQuery-guarded (see is_guest_ctx).
    const u64 regs[16] = {
        c->Rbx,  c->Rsi,  c->Rdi,  c->Rbp,  c->R12, c->R13, c->R14, c->R15,
        c->Rcx,  c->Rdx,  c->Rax,  c->R8,   c->R9,  c->R10, c->R11, c->Rsp};
    u64 ctx = 0;
    for (u64 r : regs) {
      if (is_guest_ctx(r)) {
        ctx = r;
        break;
      }
    }

    if (ctx) {
      u64 r1 = 0, lr = 0, r3 = 0, r4 = 0, r5 = 0;
      safe_read64(reinterpret_cast<void*>(ctx + kR1Offset), r1);
      safe_read64(reinterpret_cast<void*>(ctx + kLROffset), lr);
      safe_read64(reinterpret_cast<void*>(ctx + kR3Offset), r3);
      safe_read64(reinterpret_cast<void*>(ctx + kR4Offset), r4);
      safe_read64(reinterpret_cast<void*>(ctx + kR5Offset), r5);
      log_line("av fg=0x%08llX %s lr=0x%08llX r1=0x%08llX r3=0x%08llX "
               "r4=0x%08llX r5=0x%08llX rip=+0x%llX tid=0x%llX",
               (unsigned long long)fg, kind, (unsigned long long)lr,
               (unsigned long long)r1, (unsigned long long)r3,
               (unsigned long long)r4, (unsigned long long)r5,
               (unsigned long long)rip_rel,
               (unsigned long long)GetCurrentThreadId());
    } else {
      // No recognizable guest context — fault in host/runtime code (or a
      // context with unusual state). Log raw regs so the probe is
      // self-diagnosing.
      log_line("av(no-ctx) fg=0x%08llX %s rip=+0x%llX tid=0x%llX rax=0x%llX "
               "rcx=0x%llX rdx=0x%llX rbx=0x%llX r12=0x%llX r13=0x%llX "
               "r14=0x%llX r15=0x%llX rbp=0x%llX rsp=0x%llX",
               (unsigned long long)fg, kind, (unsigned long long)rip_rel,
               (unsigned long long)GetCurrentThreadId(),
               (unsigned long long)c->Rax, (unsigned long long)c->Rcx,
               (unsigned long long)c->Rdx, (unsigned long long)c->Rbx,
               (unsigned long long)c->R12, (unsigned long long)c->R13,
               (unsigned long long)c->R14, (unsigned long long)c->R15,
               (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
    }
  }

  s.locked = false;
  return EXCEPTION_CONTINUE_SEARCH;
}

struct Registrar {
  Registrar() {
    state().module_base = reinterpret_cast<u64>(GetModuleHandleA(nullptr));
    char dir[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, dir, MAX_PATH)) {
      char* cut = nullptr;
      for (char* p = dir + strlen(dir) - 1; p >= dir; --p) {
        if (*p == '\\' || *p == '/') {
          cut = p;
          break;
        }
      }
      if (cut) *(cut + 1) = 0;
      char path[MAX_PATH + 32];
      snprintf(path, sizeof(path), "%sfable2_av_probe.log", dir);
      state().log = fopen(path, "a");
    }
    // FIRST priority so we run before guest SEH can swallow the AV.
    AddVectoredExceptionHandler(1, handler);
    log_line("av probe installed base=0x%llX",
             (unsigned long long)state().module_base);
  }
  ~Registrar() {
    if (state().log) fclose(state().log);
  }
};

// One instance per including TU (header is #pragma once and only included
// from main.cpp), constructed at process start before guest code runs.
static Registrar g_av_probe_registrar;

}  // namespace fable2_av_probe

#endif  // FABLE2_AV_PROBE_H
