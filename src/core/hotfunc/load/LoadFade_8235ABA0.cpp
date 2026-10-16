// Hand-tuned override: opt-in shorter render-object release fades (FABLE2_LOAD_FADE_FRAMES, default off).
// The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// docs/load-fixes-20261016.md, sections C and D. When the GameThread releases a render object (sub_8235DA58: after
// the save notice, before the arrival, once at boot), it sets obj+172 = 1.0 and obj+176 = 1 and waits on obj+180.
// The 3D Engine's per-frame update of the object, sub_8235ABA0 (this function), then subtracts 1/15 per render
// frame from obj+172. At <= 0.0001 it frees the object's resources and signals the event. That is 15 render frames,
// about 0.5 s at the loading screen's 30 Hz, and the screen stays static meanwhile. No other code reads obj+172.
//
// FABLE2_LOAD_FADE_FRAMES=N (0..14): while a release is pending (+176 == 1, +177 == 0), the fade value is capped
// at N/15 before the original update runs. The same teardown therefore runs after N frames (0 = on the first frame)
// instead of 15. Unset, 15 or anything else: the original 15 frames, and this is a plain call of the original.
// This is a pacing change (the save notice and the loading screen leave sooner); the launcher's
// "Skip cosmetic waits" (FABLE2_FAST_PACING=1, fable2_fast_pacing.h) makes 1 the default; an explicit value wins.
#include "fable_2_pch.h"

#include <cstdlib>
#include <cstring>

#include <rex/logging/macros.h>

#include "fable2_fast_pacing.h"

extern "C" void __imp__sub_8235ABA0(PPCContext& __restrict ctx, uint8_t* base);

namespace {

int FadeFrames() {
  static const int frames = [] {
    const char* v = std::getenv("FABLE2_LOAD_FADE_FRAMES");  // getenv-ok: once
    int n = fable2::pacing::FastPacing() ? 1 : 15;
    if (v && v[0]) {
      n = 15;
      char* end = nullptr;
      const long x = std::strtol(v, &end, 10);
      if (end && *end == 0 && x >= 0 && x <= 15) n = int(x);
    }
    if (n < 15) {
      REXLOG_INFO("[load-fade] render-object release fades: {} frame(s) instead of 15 (FABLE2_LOAD_FADE_FRAMES; "
                  "pacing change, unset or 15 = original{})", n,
                  v && v[0] ? "" : "; from FABLE2_FAST_PACING");
    }
    return n;
  }();
  return frames;
}

}  // namespace

extern "C" void sub_8235ABA0(PPCContext& __restrict ctx, uint8_t* base) {
  const int frames = FadeFrames();
  if (frames < 15) {
    const uint32_t obj = ctx.r3.u32;
    if (REX_LOAD_U8(obj + 177) == 0 && REX_LOAD_U8(obj + 176) != 0) {
      // Cap = N steps of the guest's own step ([0x8209FFD8] / [0x83319510] = 1/15), so the original reaches its
      // 0.0001 threshold after N updates.
      uint64_t nu, du;
      nu = REX_LOAD_U64(0x8209FFD8u);
      du = REX_LOAD_U64(0x83319510u);
      double num, den;
      std::memcpy(&num, &nu, 8);
      std::memcpy(&den, &du, 8);
      const float cap = float(double(frames) * (num / den));
      const uint32_t cur_bits = REX_LOAD_U32(obj + 172);
      float cur;
      std::memcpy(&cur, &cur_bits, 4);
      if (cur > cap) {
        uint32_t cap_bits;
        std::memcpy(&cap_bits, &cap, 4);
        REX_STORE_U32(obj + 172, cap_bits);
      }
    }
  }
  __imp__sub_8235ABA0(ctx, base);
}
