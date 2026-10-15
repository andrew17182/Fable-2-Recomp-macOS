// fable2_quick_save.h - F5 / number-row 5 quick save (native).
//
// Pressing F5 saves the current game exactly like the pause menu's
// "Save > Save and Continue" item, without opening the menu.
//
// How the game saves (traced 2026-10-03 with FABLE2_SKIP_INTRO_TRACE=1 file-open stacks):
//   - Save manager: mgr = *(*(*(0x83496920) + 12) + 148).
//   - "Save and Continue" (menu handler sub_82413988) calls
//     sub_824E8990(mgr, autosave=0, 0, 0, 1). That writes mainsave.bin, herosave.bin,
//     chaptersave.bin and texturemorphs.bin and closes them within ~1 s.
//   - The Lua AutoSaveGame() reaches the same writer through sub_824E8D28(mgr, 0, ..)
//     -> sub_824E8990(mgr, autosave=1, 0, 0, 1) from inside the running Lua call.
//     Called from an F5 script that never completed: once it crashed (guest read
//     of 0x8) and once it hung on a loading screen with mainsave.bin half written.
//     It is not used.
// The hook runs the menu's call at the start of the world update sub_82275F00. That
// update runs on the game thread, and the game handles its own deferred saves there
// (mgr+2088). sub_824E8990 checks whether saving is allowed (sub_824EBF30 and the
// world flags) and does nothing when it is not, for example on the title screen.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/logging/macros.h>

extern "C" void sub_824E8990(PPCContext&, uint8_t*);

namespace fable2::quicksave {

constexpr uint32_t kGameGlobals = 0x83496920;

inline std::atomic<bool> g_pending{false};
inline std::atomic<uint32_t> g_count{0};

inline void request() { g_pending.store(true); }

inline bool ok_addr(uint32_t a) { return a >= 0x40000000u && a < 0xF0000000u; }

inline double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Called on the game thread at the start of the world update.
inline void poll(PPCContext& ctx, uint8_t* base) {
  if (!g_pending.exchange(false)) return;
  static double last = -100.0;
  const double t = now_s();
  if (t - last < 3.0) {
    REXSYS_INFO("[quick-save] F5 ignored: previous quick save {:.1f} s ago", t - last);
    return;
  }
  const uint32_t g = REX_LOAD_U32(kGameGlobals);
  const uint32_t g12 = ok_addr(g) ? REX_LOAD_U32(g + 12) : 0;
  const uint32_t mgr = ok_addr(g12) ? REX_LOAD_U32(g12 + 148) : 0;
  if (!ok_addr(mgr)) {
    REXSYS_INFO("[quick-save] F5 ignored: no save manager (g={:08X} g12={:08X})", g, g12);
    return;
  }
  last = t;
  const uint32_t n = g_count.fetch_add(1) + 1;
  REXSYS_INFO("[quick-save] #{} saving like Save and Continue (mgr={:08X})", n, mgr);
  const PPCContext saved = ctx;
  ctx.r3.u64 = mgr;
  ctx.r4.u64 = 0;  // not an autosave: the full menu save
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = 1;
  sub_824E8990(ctx, base);
  ctx = saved;
  REXSYS_INFO("[quick-save] #{} save call returned", n);
}

}  // namespace fable2::quicksave

// World update (game thread). The game processes its own deferred saves here.
extern "C" void __imp__sub_82275F00(PPCContext&, uint8_t*);
extern "C" void sub_82275F00(PPCContext& ctx, uint8_t* base) {
  fable2::quicksave::poll(ctx, base);
  __imp__sub_82275F00(ctx, base);
}
