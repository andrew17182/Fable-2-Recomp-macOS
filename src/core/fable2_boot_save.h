// fable2_boot_save.h - opt-in "boot straight into the save" (test-loop speedup).
//
// FABLE2_BOOT_SAVE=1 makes the front end do, without pad input, what the
// A / Down / A menu presses do:
//   1. Title ("Press A") screen: once the prompt is interactive, the title
//      screen's input method (vtable slot 3, sub_826C61B8) is called with
//      action 36 (accept) from pad 0. That records the active user
//      (0x83496934) and switches the front end to the main menu.
//   2. Main menu: the menu's message handler (vtable slot 4,
//      ProcessAndProcessAndProcess2446_826C81A0) is called with the same
//      event the UI posts for the Continue item: {String "Load Game", id 2, 0}.
//      The handler enumerates the user's saves; with exactly one hero save it
//      calls sub_8237A2D8 itself and the game loads that save (no slot cards).
//      With several saves the same menu object deals the slot cards in place
//      (state 0 -> 5; fe+188 is unchanged), or boot-save loads
//      FABLE2_BOOT_SAVE_HERO directly (below).
// Both calls are made from the front-end UI update (82379C10, r3 = front end
// = (*0x83496928)->8->80, current screen at +188) after its original body
// returns, mirroring how 82379C10 itself pops an event into a stack String and
// calls the screen's handler. Each step fires at most once per process.
// Default off: with the variable unset nothing here changes behaviour.
//
// FABLE2_BOOT_SAVE_MENU_DELAY_MS (default 0): optional wait between the main
// menu screen appearing and the first Continue event. With 0 the event is sent
// on the first front-end tick where the main menu is the current screen. The
// handler starts the load synchronously (sub_8237A2D8, counted by
// fable2_save_trace.h); if that did not happen and the main menu is still
// current, the event is re-sent every kMenuRetryMs, up to kMenuMaxTries times,
// so a menu that is not yet accepting input cannot strand the boot.
// The retry is only for an event the menu ignored: the menu's state (+24) is
// read before each send, and a state change (with several saves the handler
// moves 0 -> 5 and the slot-card screen follows ~0.5 s later) counts as
// accepted, so the event is never sent again (2026-10-14: re-sending it every
// 100 ms restarted the save enumeration 50 times, ~21 s of the menu cycling
// before the slot cards appeared; docs/bug-patterns.md).
//
// FABLE2_BOOT_SAVE_HERO=HeroNNN (set by the player app to the newest save):
// with several saves, once the handler has enumerated them (entries at
// menu+72: String* of the content name, HeroNNN; slot records at
// *(menu+136), 36 bytes each, valid byte at +24), the entry whose name matches is
// loaded directly: the same sub_8237A2D8(fe, &entries[i], record_i + 12)
// call the handler makes itself for a single save. If no entry matches or its
// record is not valid, the slot-card screen appears as before.
// FABLE2_BOOT_SAVE_TRACE=1: observation only (logs menu messages and title
// input), independent of FABLE2_BOOT_SAVE.
//
// Single-TU (included from main.cpp after fable2_skip_intro.h and fable2_save_trace.h).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <rex/logging/macros.h>

namespace fable2::bootsave {

inline bool env_flag(const char* name) {
  const char* v = std::getenv(name);
  return v && v[0] == '1';
}

inline bool enabled() {
  static const bool on = env_flag("FABLE2_BOOT_SAVE");
  return on;
}

inline bool trace() {
  static const bool on = env_flag("FABLE2_BOOT_SAVE_TRACE");
  return on;
}

inline int menu_delay_ms() {
  static const int ms = [] {
    const char* v = std::getenv("FABLE2_BOOT_SAVE_MENU_DELAY_MS");
    return v && *v ? std::atoi(v) : 0;
  }();
  return ms;
}

inline const std::string& wanted_hero() {
  static const std::string hero = [] {
    const char* v = std::getenv("FABLE2_BOOT_SAVE_HERO");
    return std::string(v ? v : "");
  }();
  return hero;
}

constexpr uint32_t kTitleVtbl = 0x820CD110;     // "Press A" screen
constexpr uint32_t kMainMenuVtbl = 0x820CD524;  // New Game / Continue / ...
constexpr uint32_t kLoadGameStr = 0x820CD384;   // "Load Game" (Continue item event name)
constexpr uint32_t kConstructString = 0x8222CF18;  // ConstructRefCounted(String*, const char*, -1)
constexpr uint32_t kDestructString = 0x82214F08;   // DestructRefCounted(String*)
constexpr uint32_t kStartFromSave = 0x8237A2D8;    // sub_8237A2D8(fe, &entries[i], record_i + 12)

inline bool ok_addr(uint32_t a) { return a >= 0x40000000u && a < 0xF0000000u; }
inline bool ok_img(uint32_t a) { return a >= 0x82000000u && a < 0x84000000u; }

inline std::string cstr(uint8_t* base, uint32_t addr, int n = 64) {
  std::string out;
  if (!(ok_addr(addr) || ok_img(addr))) return "<bad>";
  for (int i = 0; i < n; ++i) {
    const uint8_t c = REX_LOAD_U8(addr + i);
    if (!c) break;
    out.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '.');
  }
  return out;
}

inline double now_s() {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  return duration<double>(steady_clock::now() - t0).count();
}

constexpr int kMenuRetryMs = 100;
constexpr int kMenuMaxTries = 50;

struct State {
  bool title_done = false;
  bool menu_done = false;
  double menu_seen_at = -1.0;
  double last_send_at = -1.0;
  int tries = 0;
  int starts_at_first_send = 0;
  bool direct_tried = false;
};

inline State& state() {
  static State s;
  return s;
}

// Title screen ready: state 9, prompt-interactive flag (+72) set, not busy (+57).
inline bool title_ready(uint8_t* base, uint32_t scr) {
  return REX_LOAD_U32(scr + 24) == 9 && REX_LOAD_U8(scr + 72) != 0 && REX_LOAD_U8(scr + 57) == 0;
}

// Several saves, FABLE2_BOOT_SAVE_HERO set: load that entry the way the handler loads a single save.
// sp = the guest frame step() made for the event (still owned by this call).
inline void load_direct(PPCContext& ctx, uint8_t* base, uint32_t fe, uint32_t scr, uint32_t entries, uint32_t sp) {
  const uint32_t begin = REX_LOAD_U32(scr + 72), records = REX_LOAD_U32(scr + 136);
  if (!ok_addr(begin) || !ok_addr(records) || entries > 64) {
    REXSYS_INFO("[boot-save] direct load of {}: no save list (entries {:08X}, records {:08X}); slot cards instead",
                wanted_hero(), begin, records);
    return;
  }
  int found = -1;
  std::string names;
  for (uint32_t i = 0; i < entries; ++i) {
    const uint32_t entry = REX_LOAD_U32(begin + i * 4);
    // Each entry is the game's ref-counted String {chars*, length, capacity, refs} holding the content
    // file name ("Hero003"; probed 2026-10-14).
    const uint32_t chars = ok_addr(entry) ? REX_LOAD_U32(entry) : 0;
    const std::string name = ok_addr(chars) ? cstr(base, chars, 40) : "<bad>";
    const bool valid = REX_LOAD_U8(records + i * 36 + 24) != 0;
    names += (i ? ", " : "") + name + (valid ? "" : " (no record)");
    if (found < 0 && valid && name == wanted_hero()) found = static_cast<int>(i);
  }
  REXSYS_INFO("[boot-save] saves listed by the menu: {}", names);
  if (found < 0) {
    REXSYS_INFO("[boot-save] {} not in the list (or no valid record); slot cards instead", wanted_hero());
    return;
  }
  const int starts_before = fable2::savetrace::start_from_save_calls().load();
  ctx.r1.u32 = sp;
  ctx.r3.u32 = fe;
  ctx.r4.u32 = begin + static_cast<uint32_t>(found) * 4;
  ctx.r5.u32 = records + static_cast<uint32_t>(found) * 36 + 12;
  REX_CALL_INDIRECT_FUNC(kStartFromSave);
  REXSYS_INFO("[boot-save] t={:.2f}s direct load of {} (entry {} of {}): load started={}", now_s(), wanted_hero(),
              found, entries, fable2::savetrace::start_from_save_calls().load() != starts_before);
}

// Runs after the original front-end UI update. ctx is restored by the caller.
inline void step(PPCContext& ctx, uint8_t* base, uint32_t fe) {
  State& s = state();
  if (s.menu_done || !ok_addr(fe)) return;
  const uint32_t scr = REX_LOAD_U32(fe + 188);
  if (!ok_addr(scr)) return;
  const uint32_t vt = REX_LOAD_U32(scr);

  if (!s.title_done && vt == kTitleVtbl) {
    if (!title_ready(base, scr)) return;
    s.title_done = true;
    REXSYS_INFO("[boot-save] t={:.2f}s title ready (screen {:08X}); sending accept for pad 0", now_s(), scr);
    ctx.r3.u32 = scr;
    ctx.r4.s64 = 36;  // accept
    ctx.r5.s64 = 0;   // pad / user index 0
    REX_CALL_INDIRECT_FUNC(REX_LOAD_U32(vt + 12));
    REXSYS_INFO("[boot-save] title accept done: state={} active user={}", REX_LOAD_U32(scr + 24),
                REX_LOAD_U32(0x83496934u));
    return;
  }

  if (vt != kMainMenuVtbl) {
    // Another screen replaced the menu after our event. Stop here.
    if (s.tries > 0) {
      s.menu_done = true;
      REXSYS_INFO("[boot-save] t={:.2f}s main menu left without a direct load (screen vtbl {:08X}); stopping",
                  now_s(), vt);
    }
    return;
  }
  const double t = now_s();
  if (s.menu_seen_at < 0) {
    s.menu_seen_at = t;
    REXSYS_INFO("[boot-save] t={:.2f}s main menu up (screen {:08X}, state={} b57={} b72={})", t, scr,
                REX_LOAD_U32(scr + 24), REX_LOAD_U8(scr + 57), REX_LOAD_U8(scr + 72));
  }
  if ((t - s.menu_seen_at) * 1000.0 < menu_delay_ms()) return;
  if (s.tries > 0 && (t - s.last_send_at) * 1000.0 < kMenuRetryMs) return;
  if (s.tries > 0 && fable2::savetrace::start_from_save_calls().load() != s.starts_at_first_send) {
    s.menu_done = true;  // a load started later than the event itself; never send twice
    REXSYS_INFO("[boot-save] t={:.2f}s load started after Continue; done", t);
    return;
  }
  if (s.tries >= kMenuMaxTries) {
    s.menu_done = true;
    REXSYS_INFO("[boot-save] t={:.2f}s Continue did not start a load after {} tries; giving up", t, s.tries);
    return;
  }
  ++s.tries;
  s.last_send_at = t;
  const int starts_before = fable2::savetrace::start_from_save_calls().load();
  const uint32_t state_before = REX_LOAD_U32(scr + 24);
  if (s.tries == 1) s.starts_at_first_send = starts_before;
  REXSYS_INFO("[boot-save] t={:.2f}s sending Continue ('{}') to main menu {:08X} (try {}, {:.0f} ms after menu up)",
              t, cstr(base, kLoadGameStr), scr, s.tries, (t - s.menu_seen_at) * 1000.0);

  // Event on a fresh guest frame: msg = {String name @+96, id @+100, arg @+104}.
  const uint32_t sp = (ctx.r1.u32 - 256) & ~0xFu;
  REX_STORE_U32(sp, ctx.r1.u32);  // back chain
  ctx.r1.u32 = sp;
  const uint32_t msg = sp + 96;
  ctx.r3.u32 = msg;
  ctx.r4.u32 = kLoadGameStr;
  ctx.r5.s64 = -1;
  REX_CALL_INDIRECT_FUNC(kConstructString);
  REX_STORE_U32(msg + 4, 2u);
  REX_STORE_U32(msg + 8, 0u);
  ctx.r1.u32 = sp;
  ctx.r3.u32 = scr;
  ctx.r4.u32 = msg;
  REX_CALL_INDIRECT_FUNC(REX_LOAD_U32(vt + 16));
  ctx.r1.u32 = sp;
  ctx.r3.u32 = msg;
  REX_CALL_INDIRECT_FUNC(kDestructString);
  const bool started = fable2::savetrace::start_from_save_calls().load() != starts_before;
  const uint32_t now_scr = REX_LOAD_U32(fe + 188);
  const bool left_menu = !ok_addr(now_scr) || REX_LOAD_U32(now_scr) != kMainMenuVtbl;
  const uint32_t state_after = REX_LOAD_U32(scr + 24);
  const bool accepted = state_after != state_before;
  const uint32_t entries = (REX_LOAD_U32(scr + 76) - REX_LOAD_U32(scr + 72)) / 4;
  REXSYS_INFO("[boot-save] t={:.2f}s Continue event handled: load started={} (entries={}, menu state {} -> {}, "
              "screen changed={})", now_s(), started, entries, state_before, state_after, left_menu);
  if (started || left_menu || accepted) s.menu_done = true;  // never re-send an accepted event
  if (!started && !left_menu && entries > 1 && !wanted_hero().empty() && !s.direct_tried) {
    s.direct_tried = true;
    load_direct(ctx, base, fe, scr, entries, sp);
  }
}

}  // namespace fable2::bootsave

// Front-end UI update (called every front-end tick from sub_82355460).
extern "C" void __imp__ProcessAndProcessAndProcess1614_82379C10(PPCContext&, uint8_t*);
extern "C" void ProcessAndProcessAndProcess1614_82379C10(PPCContext& ctx, uint8_t* base) {
  using namespace fable2::bootsave;
  const uint32_t fe = ctx.r3.u32;
  if (trace()) {
    static std::atomic<int> n{0};
    const int i = n.fetch_add(1);
    if (i == 0 || (i % 300) == 0) {
      const uint32_t scr = REX_LOAD_U32(fe + 188);
      const uint32_t vt = ok_addr(scr) ? REX_LOAD_U32(scr) : 0;
      REXSYS_INFO("[boot-save] trace FEUpdate #{} fe={:08X} scr188={:08X} vtbl={:08X}", i, fe, scr, vt);
    }
  }
  __imp__ProcessAndProcessAndProcess1614_82379C10(ctx, base);
  if (enabled() && !state().menu_done) {
    const PPCContext saved = ctx;
    step(ctx, base, fe);
    ctx = saved;
  }
}

// Main-menu message handler (vtable slot 4): r3 = screen, r4 = msg {String* name, int id, int arg}.
extern "C" void __imp__ProcessAndProcessAndProcess2446_826C81A0(PPCContext&, uint8_t*);
extern "C" void ProcessAndProcessAndProcess2446_826C81A0(PPCContext& ctx, uint8_t* base) {
  using namespace fable2::bootsave;
  if (trace()) {
    const uint32_t self = ctx.r3.u32, msg = ctx.r4.u32;
    const uint32_t name = REX_LOAD_U32(msg), id = REX_LOAD_U32(msg + 4), arg = REX_LOAD_U32(msg + 8);
    const uint32_t chars = ok_addr(name) ? REX_LOAD_U32(name) : 0;
    REXSYS_INFO("[boot-save] trace MenuMsg self={:08X} msg={:08X} name={:08X} '{}' id={} arg={:08X} stack {}",
                self, msg, name, chars ? cstr(base, chars) : std::string(), id, arg,
                fable2::skipintro::guest_stack(ctx, base));
  }
  __imp__ProcessAndProcessAndProcess2446_826C81A0(ctx, base);
}

// Title ("Press A") screen input handler: r3 = title screen, r4 = action (36 = accept),
// r5 = pad/user index. With +72 set (prompt interactive) and action 36 it records the
// active user (0x83496934 = r5) and moves the front end to the main menu (state 10).
extern "C" void __imp__sub_826C61B8(PPCContext&, uint8_t*);
extern "C" void sub_826C61B8(PPCContext& ctx, uint8_t* base) {
  using namespace fable2::bootsave;
  if (trace() && ctx.r4.s32 != 0) {
    const uint32_t self = ctx.r3.u32;
    REXSYS_INFO("[boot-save] trace TitleInput self={:08X} vtbl={:08X} action={} pad={} st24={} b56={} b57={} b72={} stack {}",
                self, REX_LOAD_U32(self), ctx.r4.s32, ctx.r5.s32, REX_LOAD_U32(self + 24),
                REX_LOAD_U8(self + 56), REX_LOAD_U8(self + 57), REX_LOAD_U8(self + 72),
                fable2::skipintro::guest_stack(ctx, base));
  }
  __imp__sub_826C61B8(ctx, base);
}
