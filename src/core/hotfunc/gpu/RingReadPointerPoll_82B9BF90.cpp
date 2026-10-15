// Hand-tuned override: ProcessAndProcessAndProcess523_82B9BF90 (D3D read-pointer poll).
// The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// One iteration of the guest's "wait until the command processor has consumed the ring past
// a position" loop (caller 82242628 loops while the condition holds and this returns 1).
// The original runs unchanged (__imp__ entry); before it, the call from 82242628 may block on
// command-processor progress instead of spinning (FABLE2_RING_WAIT, src/core/fable2_spin_wait.h).
// Other callers (fence/idle waits on other conditions) never block here.
//
// Guest layout read here (from the original's disassembly): r3 = wait record; [r3+0] = D3D
// device; [device+10896] = pointer to the read-pointer writeback; [device+10941] bit 1 = the
// abort flag the original checks first (no blocking when set).
#include "fable_2_pch.h"

#include "fable2_spin_wait.h"

extern "C" void __imp__ProcessAndProcessAndProcess523_82B9BF90(PPCContext& __restrict ctx, uint8_t* base);

extern "C" void ProcessAndProcessAndProcess523_82B9BF90(PPCContext& __restrict ctx, uint8_t* base) {
  if (uint32_t(ctx.lr) == 0x822426E8u) {
    fable2::spinwait::PromoteCurrentThreadQosOnce("3D Engine");
    const uint32_t device = REX_LOAD_U32(ctx.r3.u32 + 0);
    if (device && !(REX_LOAD_U8(device + 10941) & 0x2)) {
      const uint32_t writeback = REX_LOAD_U32(device + 10896);
      if (writeback) {
        fable2::spinwait::RingPollWait([&]() -> uint32_t { return REX_LOAD_U32(writeback); });
      }
    }
  }
  __imp__ProcessAndProcessAndProcess523_82B9BF90(ctx, base);
}
