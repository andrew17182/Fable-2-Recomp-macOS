// One guarded PowerPC back-chain walker for every guest stack walk in the executable.
//
// Replaces three copies of the same loop (fable2::skipintro::guest_stack, the boot-loader test
// fable2::loading::InBootLoader, and the old FABLE2_STACK_SAMPLE sampler). The guards are the ones
// those copies used, so their decisions are unchanged:
//   - sp inside the range (own thread: [0x40000000, 0xF0000000), 4-byte aligned);
//   - back chain prev = [sp] must grow (prev > sp) by at most 1 MB, otherwise the walk stops
//     ("bad chain");
//   - the saved return address [prev - 8] must lie in guest code [0x82000000, 0x84000000);
//     the outermost frame's back chain points at the unmapped stack top, so the first non-code
//     return address ends the walk and is never followed;
//   - at most `max` frames.
// Cross-thread walks (the flight-recorder sampler) pass the thread's own stack range from its
// PCR and a fault-safe loader (mach_vm_read_overwrite): a guest thread frees its stack inside
// XThread::Exit, so a raw load on another thread's stack could fault.
//
// No SDK dependency: the caller supplies the loader.
#pragma once
#include <cstdint>

namespace fable2::gstack {

inline constexpr uint32_t kCodeLo = 0x82000000u, kCodeHi = 0x84000000u;
inline bool IsCode(uint32_t a) { return a >= kCodeLo && a < kCodeHi; }

struct Range {
  uint32_t lo = 0x40000000u, hi = 0xF0000000u;  // generic (own-thread) range
};

enum class Stop : uint8_t { kMax, kRange, kLoad, kBadChain, kNotCode, kVisitor };

// Calls visit(lr) for each caller return address, innermost first. visit returns false to stop.
// load(addr, &value) returns false when addr cannot be read (cross-thread loader only).
template <class Load, class Visit>
inline Stop WalkBackChain(uint32_t sp, int max, Range range, Load&& load, Visit&& visit) {
  for (int i = 0; i < max; ++i) {
    if (!(sp >= range.lo && sp < range.hi && (sp & 3) == 0)) return Stop::kRange;
    uint32_t prev = 0;
    if (!load(sp, &prev)) return Stop::kLoad;
    if (prev <= sp || prev - sp > 0x100000u) return Stop::kBadChain;
    uint32_t lr = 0;
    if (!load(prev - 8, &lr)) return Stop::kLoad;
    if (!IsCode(lr)) return Stop::kNotCode;
    if (!visit(lr)) return Stop::kVisitor;
    sp = prev;
  }
  return Stop::kMax;
}

// Same address translation as the generated code's REX_LOAD_U32 on macOS arm64 (the 0xE0000000+
// physical views sit one host page higher).
inline uint32_t LoadOwn(const uint8_t* base, uint32_t addr) {
  const uint32_t off = addr >= 0xE0000000u ? 0x1000u : 0u;
  return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + addr + off));
}

// Own-thread walk into an array (innermost caller first). Returns the number of addresses.
// `base` is the guest membase (the recompiled functions' `base` argument).
inline int OwnStack(const uint8_t* base, uint32_t sp, uint32_t* out, int max) {
  int n = 0;
  WalkBackChain(sp, max, Range{},
                [base](uint32_t addr, uint32_t* v) {
                  *v = LoadOwn(base, addr);
                  return true;
                },
                [&](uint32_t lr) {
                  out[n++] = lr;
                  return true;
                });
  return n;
}

}  // namespace fable2::gstack
