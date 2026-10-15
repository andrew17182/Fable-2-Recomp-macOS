// Hand-tuned override: trace-only wrappers for the D3D frame throttle (2026-10-08 boot hang).
// The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// Off unless FABLE2_THROTTLE_TRACE=1; then the first N calls of each function are logged.
// Every wrapper runs the original (__imp__) unchanged.
//   82B69488  Present: ++presents (0x83496F44), InsertCallback(82B89A78), then spins while
//             callbacks - presents > 1.
//   82B89A78  GPU frame callback: ++stats[idx].count (0x83491BD0 + 24*idx + 16).
//   82B9B8D8  D3D GPU interrupt handler (r3 = source, r4 = device). Source 1 calls the callback
//             stored in the scratch writeback [device+10900] +16 (fn) / +20 (arg).
// See docs/boot-hang-20261008.md.
#include "fable_2_pch.h"

#include <pthread.h>

#include <atomic>
#include <cstdlib>

#include <rex/logging/macros.h>

extern "C" void __imp__ProcessAndProcessAndProcess1265_82B69488(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__ProcessAndProcessAndProcess1267_82B89A78(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__ProcessAndProcessAndProcess894_82B9B8D8(PPCContext& __restrict ctx, uint8_t* base);

namespace {

bool On() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_THROTTLE_TRACE");
    return v && v[0] == '1';
  }();
  return on;
}

constexpr int kMax = 80;

const char* ThreadName(char (&buf)[64]) {
  buf[0] = 0;
  pthread_getname_np(pthread_self(), buf, sizeof buf);
  return buf;
}

uint32_t Presents(uint8_t* base) { return REX_LOAD_U32(0x83496F44u); }
uint32_t Callbacks(uint8_t* base) {
  const uint32_t idx = REX_LOAD_U32(0x83496F48u);
  return idx <= 1 ? REX_LOAD_U32(0x83491BD0u + 24 * idx + 16) : 0xFFFFFFFFu;
}

}  // namespace

extern "C" void ProcessAndProcessAndProcess1265_82B69488(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> n{0};
  if (On()) {
    const int i = n.fetch_add(1);
    if (i < kMax) {
      char t[64];
      REXSYS_INFO("[throttle-trace] present #{} enter thread '{}' lr={:08X} presents={} callbacks={}", i,
                  ThreadName(t), uint32_t(ctx.lr), Presents(base), Callbacks(base));
    }
  }
  __imp__ProcessAndProcessAndProcess1265_82B69488(ctx, base);
}

extern "C" void ProcessAndProcessAndProcess1267_82B89A78(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> n{0};
  if (On()) {
    const int i = n.fetch_add(1);
    if (i < kMax) {
      char t[64];
      REXSYS_INFO("[throttle-trace] frame callback #{} thread '{}' lr={:08X} r3={:08X} presents={} callbacks(before)={}",
                  i, ThreadName(t), uint32_t(ctx.lr), ctx.r3.u32, Presents(base), Callbacks(base));
    }
  }
  __imp__ProcessAndProcessAndProcess1267_82B89A78(ctx, base);
}

extern "C" void ProcessAndProcessAndProcess894_82B9B8D8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> n{0};
  if (On() && ctx.r3.u32 == 1) {  // source 1 = command-stream interrupt (vblank = 0 is noise)
    const int i = n.fetch_add(1);
    if (i < kMax) {
      char t[64];
      const uint32_t device = ctx.r4.u32;
      const uint32_t s = device ? REX_LOAD_U32(device + 10900) : 0;
      const uint32_t cpu = REX_LOAD_U8(ctx.r13.u32 + 268);
      const uint32_t rp_wb = device ? REX_LOAD_U32(device + 10896) : 0;  // ring read-pointer writeback
      REXSYS_INFO("[throttle-trace] interrupt #{} source 1 thread '{}' cpu={} scratch={:08X} pending={:08X} fn={:08X} "
                  "arg={:08X} rptr={:08X} presents={} callbacks={}",
                  i, ThreadName(t), cpu, s, s ? REX_LOAD_U32(s) : 0, s ? REX_LOAD_U32(s + 16) : 0,
                  s ? REX_LOAD_U32(s + 20) : 0, rp_wb ? REX_LOAD_U32(rp_wb) : 0, Presents(base), Callbacks(base));
    }
  }
  __imp__ProcessAndProcessAndProcess894_82B9B8D8(ctx, base);
}

// 821F5F20 = D3D InsertCallback(device r3, cmd ptr r4, flags r5, fn r6, arg r7): writes the scratch
// fn/arg + PM4_INTERRUPT packets. Logs inserts of the frame callback 82B89A78 with the caller.
extern "C" void __imp__ProcessAndProcessAndProcess952_821F5F20(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void ProcessAndProcessAndProcess952_821F5F20(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> n{0};
  if (On() && ctx.r6.u32 == 0x82B89A78u) {
    const int i = n.fetch_add(1);
    if (i < kMax) {
      char t[64];
      // Caller's return address two frames up: Present saved its lr at its frame; the back chain
      // word at r1 points to the caller frame, whose lr save slot is at -8 (savegprlr convention).
      const uint32_t sp = ctx.r1.u32;
      const uint32_t up = REX_LOAD_U32(sp);
      REXSYS_INFO("[throttle-trace] insert frame callback #{} thread '{}' lr={:08X} up-lr={:08X} flags={:08X} "
                  "cmd={:08X} presents={} callbacks={}",
                  i, ThreadName(t), uint32_t(ctx.lr), up ? REX_LOAD_U32(up - 8) : 0, ctx.r5.u32, ctx.r4.u32, Presents(base),
                  Callbacks(base));
    }
  }
  __imp__ProcessAndProcessAndProcess952_821F5F20(ctx, base);
}

// 82B9C7F8(device, 0) runs right before every swap: from Present (lr 82B694C8 area) and from the
// D3D keep-alive thread 82B9F598 (lr 82B9F690), which re-swaps the last frame when its 30 ms wait
// times out and device[10942] bit 1 is set.
extern "C" void __imp__ProcessAndProcessAndProcess1258_82B9C7F8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void ProcessAndProcessAndProcess1258_82B9C7F8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> n{0};
  if (On()) {
    const int i = n.fetch_add(1);
    if (i < kMax) {
      char t[64];
      REXSYS_INFO("[throttle-trace] pre-swap #{} thread '{}' lr={:08X} presents={} callbacks={}", i, ThreadName(t),
                  uint32_t(ctx.lr), Presents(base), Callbacks(base));
    }
  }
  __imp__ProcessAndProcessAndProcess1258_82B9C7F8(ctx, base);
}

// The ring-buffer kernel imports (VdInitializeRingBuffer / VdEnableRingBufferRPtrWriteBack) are
// overridden in RingReinitFix.cpp, which also logs them under FABLE2_THROTTLE_TRACE=1.
