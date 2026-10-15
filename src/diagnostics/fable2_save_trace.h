// fable2_save_trace.h - opt-in tracing of the front-end "load save" path.
//
// FABLE2_SAVE_TRACE=1 logs, once per distinct object, the game-state object
// whose Update (vtable slot 6) loads a save (sub_8245F420 -> sub_8245ED58 ->
// sub_824EA150 opens save:\herosave.bin / mainsave.bin), with its vtable and
// first words, plus the guest call chain. Observation only: every hook
// forwards unchanged to the original body. Single-TU (main.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include <rex/logging/macros.h>

namespace fable2::savetrace {

inline bool enabled() {
  static const bool on = [] { const char* v = std::getenv("FABLE2_SAVE_TRACE"); return v && v[0] == '1'; }();
  return on;
}

// Count of front-end "start from save" calls (sub_8237A2D8). Read by
// fable2_boot_save.h to confirm its Continue event actually started the load.
inline std::atomic<int>& start_from_save_calls() {
  static std::atomic<int> n{0};
  return n;
}

inline std::string words(uint8_t* base, uint32_t addr, int n) {
  std::string out;
  char tmp[16];
  if (addr < 0x40000000u || addr >= 0xF0000000u) return "<bad>";
  for (int i = 0; i < n; ++i) {
    std::snprintf(tmp, sizeof tmp, "%08X ", REX_LOAD_U32(addr + 4 * i));
    out += tmp;
  }
  return out;
}

inline bool first_time(uint32_t key) {
  static std::mutex m;
  static std::set<uint32_t> seen;
  std::lock_guard<std::mutex> lock(m);
  return seen.insert(key).second;
}

}  // namespace fable2::savetrace

extern "C" void __imp__sub_8245F420(PPCContext&, uint8_t*);
extern "C" void sub_8245F420(PPCContext& ctx, uint8_t* base) {
  if (fable2::savetrace::enabled() && fable2::savetrace::first_time(ctx.r3.u32)) {
    const uint32_t obj = ctx.r3.u32, sub = REX_LOAD_U32(obj + 20);
    REXSYS_INFO("[save-trace] LoadState.Update obj={:08X} vtbl={:08X} obj[]={} sub={:08X} sub[]={} stack {}",
                obj, REX_LOAD_U32(obj), fable2::savetrace::words(base, obj, 12), sub,
                fable2::savetrace::words(base, sub, 12), fable2::skipintro::guest_stack(ctx, base));
  }
  __imp__sub_8245F420(ctx, base);
}

extern "C" void __imp__sub_824EA150(PPCContext&, uint8_t*);
extern "C" void sub_824EA150(PPCContext& ctx, uint8_t* base) {
  if (fable2::savetrace::enabled() && fable2::savetrace::first_time(0x1000000u ^ ctx.r4.u32)) {
    const uint32_t mgr = ctx.r3.u32;
    REXSYS_INFO("[save-trace] SaveLoad mgr={:08X} mgr[]={} state={:08X} flag={}", mgr,
                fable2::savetrace::words(base, mgr, 24), ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_824EA150(ctx, base);
}

namespace fable2::savetrace {
inline std::string text(uint8_t* base, uint32_t addr, int n = 48) {
  std::string out;
  if (addr < 0x40000000u || addr >= 0xF0000000u) return "<bad>";
  for (int i = 0; i < n; ++i) {
    const uint8_t c = REX_LOAD_U8(addr + i);
    if (!c) break;
    out.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '.');
  }
  return out;
}
}  // namespace fable2::savetrace

// sub_8233AE50 (via thunk sub_8233D6E8: r3 = r3->12) builds the world and, when
// the string at obj+96 equals the one at 0x81FF0C98, allocates the load-save
// state (vtable 0x820B3B7C) whose Update is sub_8245F420.
extern "C" void __imp__sub_8233AE50(PPCContext&, uint8_t*);
extern "C" void sub_8233AE50(PPCContext& ctx, uint8_t* base) {
  if (fable2::savetrace::enabled()) {
    using namespace fable2::savetrace;
    const uint32_t obj = ctx.r3.u32, s = obj + 96, p = REX_LOAD_U32(s);
    REXSYS_INFO("[save-trace] WorldStart obj={:08X} obj[]={} str@96[]={} *str[]={} *str='{}' *(str+4)='{}' magic='{}' magic[]={} stack {}",
                obj, words(base, obj, 32), words(base, s, 4), words(base, p, 8), text(base, p),
                text(base, p + 4), text(base, 0x81FF0C98u), words(base, 0x81FF0C98u, 4),
                fable2::skipintro::guest_stack(ctx, base));
  }
  __imp__sub_8233AE50(ctx, base);
}

// Slot-screen A handler (ProcessAndProcessAndProcess1819_826C72A8, +0x9F4):
// idx = this+60; sub_824EA7A0(saveMgr, &hdr, &this->72[idx], ...) reads the
// slot's chaptersave.bin, then sub_8237A2D8(fe = global->8->80,
// &this->72[idx], this->136 + idx*36 + 12) - candidate "start from save".
extern "C" void __imp__sub_8237A2D8(PPCContext&, uint8_t*);
extern "C" void sub_8237A2D8(PPCContext& ctx, uint8_t* base) {
  fable2::savetrace::start_from_save_calls().fetch_add(1);
  if (fable2::savetrace::enabled()) {
    using namespace fable2::savetrace;
    const uint32_t e = ctx.r4.u32, ep = REX_LOAD_U32(e);
    REXSYS_INFO("[save-trace] StartFromSave fe={:08X} fe[]={} entry={:08X} *entry={:08X} (*entry)[]={} name+8='{}' name+264='{}' rec={:08X} rec[]={} stack {}",
                ctx.r3.u32, words(base, ctx.r3.u32, 16), e, ep, words(base, ep, 16), text(base, ep + 8),
                text(base, ep + 264), ctx.r5.u32, words(base, ctx.r5.u32, 9),
                fable2::skipintro::guest_stack(ctx, base));
  }
  __imp__sub_8237A2D8(ctx, base);
}

// Front-end mode Run (vtable slot 3); returns the next app mode (this+132).
extern "C" void __imp__sub_82355460(PPCContext&, uint8_t*);
extern "C" void sub_82355460(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82355460(ctx, base);
  if (fable2::savetrace::enabled()) {
    using namespace fable2::savetrace;
    const uint32_t next = ctx.r3.u32;
    REXSYS_INFO("[save-trace] FrontEndRun self={:08X} next={:08X} next.vtbl={:08X} next[]={}", self, next,
                next ? REX_LOAD_U32(next) : 0u, next ? words(base, next, 8) : std::string());
  }
}
