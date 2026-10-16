// Hand-tuned override: trace-only wrappers for the area-load holds (FABLE2_LOAD_HOLD_TRACE=1, default off).
// The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// docs/load-fixes-20261016.md part C. Both wrappers call the original (__imp__) unchanged and only read guest
// memory, so the game behaves the same with the trace on or off.
//
//   sub_822C1FB8(ls)  GameThread, once per game tick from ProcessGameFrame (8227709C .. 822770B8): the
//                     loading-screen controller. Logged whenever one of its fields changes: +0 (progress float),
//                     +4 (duration float), +12..15 (id), +33/+34/+44 (flags). Its teardown branch calls
//                     sub_8235DA58 (render wait 2, return 822C20AC).
//   sub_8235DA58(h)   "release render object" (h+4 = object): writes the float at 0x820994C0 to obj+172, sets
//                     obj+176 = 1 and waits (infinite) on the event at obj+180, which the 3D Engine's per-frame
//                     update sub_8235ABA0 signals once it has stepped obj+172 down to 0. Logged: caller, the constant, the
//                     wait time; a watcher thread samples obj+160..+199 every 0.5 ms during the wait and logs each
//                     change (what the 3D Engine does to the object before it signals).
// Lines: "[load-hold] ...". Timestamps are steady-clock ms since the first traced call.
#include "fable_2_pch.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <rex/logging/macros.h>

#include "fable2_guest_stack.h"

extern "C" void __imp__sub_822C1FB8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8235DA58(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8245F260(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8245E3F8(PPCContext& __restrict ctx, uint8_t* base);

namespace {

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_LOAD_HOLD_TRACE");  // getenv-ok: once
    const bool enabled = v && v[0] == '1';
    if (enabled) REXLOG_INFO("[load-hold] trace on (FABLE2_LOAD_HOLD_TRACE=1)");
    return enabled;
  }();
  return on;
}

double NowMs() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

float LoadF32(uint8_t* base, uint32_t addr) {
  const uint32_t u = REX_LOAD_U32(addr);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

struct LsFields {
  uint32_t progress_bits = 0, duration_bits = 0, id = 0;
  uint8_t f33 = 0, f34 = 0, f44 = 0;
  bool operator==(const LsFields& o) const {
    return progress_bits == o.progress_bits && duration_bits == o.duration_bits && id == o.id && f33 == o.f33 &&
           f34 == o.f34 && f44 == o.f44;
  }
};

LsFields ReadLs(uint8_t* base, uint32_t ls) {
  LsFields f;
  f.progress_bits = REX_LOAD_U32(ls + 0);
  f.duration_bits = REX_LOAD_U32(ls + 4);
  f.id = REX_LOAD_U32(ls + 12);
  f.f33 = REX_LOAD_U8(ls + 33);
  f.f34 = REX_LOAD_U8(ls + 34);
  f.f44 = REX_LOAD_U8(ls + 44);
  return f;
}

float AsF(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

std::atomic<uint64_t> g_tick{0};

}  // namespace

extern "C" void sub_822C1FB8(PPCContext& __restrict ctx, uint8_t* base) {
  if (!TraceOn()) {
    __imp__sub_822C1FB8(ctx, base);
    return;
  }
  static LsFields last{};
  static uint32_t last_ls = 0;
  static uint64_t last_change_tick = 0;
  const uint32_t ls = ctx.r3.u32;
  const uint64_t tick = g_tick.fetch_add(1) + 1;
  const LsFields before = ReadLs(base, ls);
  if (ls != last_ls || !(before == last)) {
    REXLOG_INFO("[load-hold] t={:.1f} tick {} (+{} since last change) ls={:08X} progress={:.4f} duration={:.4f} "
                "id={:08X} f33={} f34={} f44={}",
                NowMs(), tick, tick - last_change_tick, ls, AsF(before.progress_bits), AsF(before.duration_bits),
                before.id, before.f33, before.f34, before.f44);
    last = before;
    last_ls = ls;
    last_change_tick = tick;
  }
  __imp__sub_822C1FB8(ctx, base);
  const LsFields after = ReadLs(base, ls);
  if (!(after == last)) {
    REXLOG_INFO("[load-hold] t={:.1f} tick {} after call: progress={:.4f} duration={:.4f} id={:08X} f33={} f34={} "
                "f44={}",
                NowMs(), tick, AsF(after.progress_bits), AsF(after.duration_bits), after.id, after.f33, after.f34,
                after.f44);
    last = after;
    last_change_tick = tick;
  }
}

extern "C" void sub_8235DA58(PPCContext& __restrict ctx, uint8_t* base) {
  if (!TraceOn()) {
    __imp__sub_8235DA58(ctx, base);
    return;
  }
  const uint32_t holder = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t obj = REX_LOAD_U32(holder + 4);
  // lis r11,-32246; lfs f0,-27456(r11) = 0x820A0000 - 0x6B40 = 0x820994C0 (not 0x82099440).
  const uint32_t const_bits = REX_LOAD_U32(0x820994C0u);
  auto f64 = [&](uint32_t a) {
    const uint64_t u = REX_LOAD_U64(a);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
  };
  // sub_8235ABA0 (3D Engine, per frame while obj+176 is set): obj+172 -= [0x8209FFD8] / [0x83319510], clamped at
  // [0x820994B4]; at or below [0x82099AE0] it frees the object's resources, clears +176 and signals obj+180.
  const double step_num = f64(0x8209FFD8u), step_den = f64(0x83319510u);
  const uint32_t lr2 = REX_LOAD_U32(ctx.r1.u32 + 0) ? REX_LOAD_U32(REX_LOAD_U32(ctx.r1.u32 + 0) - 8) : 0;
  const double t0 = NowMs();
  REXLOG_INFO("[load-hold] t={:.1f} render-object release enter: caller {:08X} <{:08X} holder={:08X} obj={:08X} "
              "start alpha [820994C0]={} step [8209FFD8]/[83319510]={}/{}={} clamp [820994B4]={} end [82099AE0]={} "
              "obj+172={} obj+176={} tick {}",
              t0, lr, lr2, holder, obj, AsF(const_bits), step_num, step_den, step_num / step_den,
              AsF(REX_LOAD_U32(0x820994B4u)), AsF(REX_LOAD_U32(0x82099AE0u)), obj ? LoadF32(base, obj + 172) : 0.0f,
              obj ? REX_LOAD_U8(obj + 176) : 0, g_tick.load());
  std::atomic<bool> done{false};
  std::thread watcher;
  if (obj) {
    watcher = std::thread([&, obj] {
      uint8_t prev[40];
      for (int i = 0; i < 40; ++i) prev[i] = REX_LOAD_U8(obj + 160 + i);
      int lines = 0;
      while (!done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        uint8_t cur[40];
        for (int i = 0; i < 40; ++i) cur[i] = REX_LOAD_U8(obj + 160 + i);
        if (std::memcmp(cur, prev, 40) != 0 && lines < 200) {
          std::string diff;
          for (int i = 0; i < 40; i += 4) {
            if (std::memcmp(cur + i, prev + i, 4) == 0) continue;
            uint32_t a, b;
            std::memcpy(&a, prev + i, 4);
            std::memcpy(&b, cur + i, 4);
            a = __builtin_bswap32(a);
            b = __builtin_bswap32(b);
            diff += fmt::format(" +{}: {:08X}->{:08X} ({}->{})", 160 + i, a, b, AsF(a), AsF(b));
          }
          REXLOG_INFO("[load-hold] t={:.1f} obj {:08X} changed during wait:{}", NowMs(), obj, diff);
          ++lines;
          std::memcpy(prev, cur, 40);
        }
      }
    });
  }
  __imp__sub_8235DA58(ctx, base);
  done.store(true, std::memory_order_release);
  if (watcher.joinable()) watcher.join();
  const double t1 = NowMs();
  REXLOG_INFO("[load-hold] t={:.1f} render-object release exit: caller {:08X} wait+teardown {:.1f} ms tick {}", t1,
              lr, t1 - t0, g_tick.load());
}

// The two writers of ls+34 = 1 ("loading finished, tear the loading screen down"): ls = [[0x834968A4]+64].
namespace {
uint32_t LoadingScreen(uint8_t* base) {
  const uint32_t g = REX_LOAD_U32(0x834968A4u);
  return g ? REX_LOAD_U32(g + 64) : 0;
}
void TraceSetter(const char* name, PPCContext& ctx, uint8_t* base, void (*impl)(PPCContext&, uint8_t*)) {
  if (!TraceOn()) {
    impl(ctx, base);
    return;
  }
  static std::atomic<uint64_t> calls{0};
  const uint64_t n = calls.fetch_add(1) + 1;
  const uint32_t ls = LoadingScreen(base);
  const uint8_t before = ls ? REX_LOAD_U8(ls + 34) : 0;
  const uint32_t self = ctx.r3.u32;
  std::string stack = fmt::format("{:08X}", uint32_t(ctx.lr));
  uint32_t frames[16];
  const int nf = fable2::gstack::OwnStack(base, ctx.r1.u32, frames, 16);
  for (int i = 0; i < nf; ++i) stack += fmt::format(" <{:08X}", frames[i]);
  impl(ctx, base);
  const uint8_t after = ls ? REX_LOAD_U8(ls + 34) : 0;
  if (before != after || n <= 2) {
    REXLOG_INFO("[load-hold] t={:.1f} tick {} {} call {} this={:08X} ls={:08X} f34 {}->{} stack {}", NowMs(),
                g_tick.load(), name, n, self, ls, before, after, stack);
  }
}
}  // namespace

extern "C" void sub_8245F260(PPCContext& __restrict ctx, uint8_t* base) {
  TraceSetter("sub_8245F260", ctx, base, __imp__sub_8245F260);
}
extern "C" void sub_8245E3F8(PPCContext& __restrict ctx, uint8_t* base) {
  TraceSetter("sub_8245E3F8", ctx, base, __imp__sub_8245E3F8);
}

// The area-change state machine (sub_8245D088, this+44 = state 1..9; GameThread, once per tick while a change is
// in progress). State 7 waits until sub_8236BCD8(obj) is 0, i.e. double obj+152 > double obj+176 and byte
// obj+600 == 0, and a request counter ([[[[[S+12]+88]+4]+8]+4]+220)+4 is 0; state 8 waits for flags of
// [S+12]+140 (+52/+53/+221/+241/+242); state 9 hands over to the new mode (sub_8245E3F8 sets ls+34).
// S = [0x83496920]. Logged: every state change (with ticks and ms spent in the old state) and, in state 7, the
// values sub_8236BCD8 compares whenever they change.
extern "C" void __imp__sub_8245D088(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8236BCD8(PPCContext& __restrict ctx, uint8_t* base);

namespace {
std::atomic<uint32_t> g_area_state{0};
}

extern "C" void sub_8245D088(PPCContext& __restrict ctx, uint8_t* base) {
  if (!TraceOn()) {
    __imp__sub_8245D088(ctx, base);
    return;
  }
  static uint32_t last_state = 0, calls_in_state = 0;
  static double since = 0;
  const uint32_t self = ctx.r3.u32;
  const uint32_t before = REX_LOAD_U32(self + 44);
  g_area_state.store(before);
  __imp__sub_8245D088(ctx, base);
  const uint32_t after = REX_LOAD_U32(self + 44);
  ++calls_in_state;
  if (after != before || before != last_state) {
    const double now = NowMs();
    REXLOG_INFO("[load-hold] t={:.1f} tick {} area state {} -> {} ({} calls, {:.1f} ms in state {})", now,
                g_tick.load(), before, after, calls_in_state, now - since, before);
    last_state = after;
    calls_in_state = 0;
    since = now;
  }
}

extern "C" void sub_8236BCD8(PPCContext& __restrict ctx, uint8_t* base) {
  if (!TraceOn() || uint32_t(ctx.lr) != 0x8245DBB4u) {
    __imp__sub_8236BCD8(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  auto f64 = [&](uint32_t a) {
    const uint64_t u = REX_LOAD_U64(a);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
  };
  const double a = f64(obj + 152), b = f64(obj + 176);
  const uint8_t busy = REX_LOAD_U8(obj + 600);
  __imp__sub_8236BCD8(ctx, base);
  static int lines = 0;
  if (lines < 600) {
    ++lines;
    // obj+600 mirrors the global byte 0x83496C3B, which ProcessAndProcessAndProcess1414_821815B0 sets when the
    // game mode [0x83496C74] is not 0/5, or sub_82181270() is true, or an entry of the list at 0x8349F498
    // (begin +4, end +8) is not finished (+3692 == 0; +3404 async handle, +3712 handle complete).
    const uint32_t mode = REX_LOAD_U32(0x83496C74u);
    const uint32_t lb = REX_LOAD_U32(0x8349F498u + 4), le = REX_LOAD_U32(0x8349F498u + 8);
    std::string pending;
    int n_pending = 0, n_total = 0;
    for (uint32_t p = lb; p < le && n_total < 256; p += 4, ++n_total) {
      const uint32_t o = REX_LOAD_U32(p);
      if (!o || REX_LOAD_U8(o + 3692)) continue;
      ++n_pending;
      if (n_pending <= 4) {
        const uint32_t h = REX_LOAD_U32(o + 3404);
        // sub_82AAA818 polls the handle: a state machine on h+4 (0..7).
        // sub_82A4F520 (per tick): finished when every entity of the set at +3412 reports ready, or the set is
        // empty, or now - t0 (+3696) > the limit at +3704 (about 45 s in the traces: never reached).
        pending += fmt::format(" [{:08X} vt={:08X} h={:08X} hstate={} hdone={} t0={:.3f} limit={:.3f} elapsed={:.3f}]",
                               o, REX_LOAD_U32(o), h, h ? REX_LOAD_U32(h + 4) : 0, REX_LOAD_U8(o + 3712),
                               f64(o + 3696), f64(o + 3704), a - f64(o + 3696));
      }
    }
    REXLOG_INFO("[load-hold] t={:.1f} tick {} state-7 check obj={:08X} +152={:.4f} +176={:.4f} +600={} -> {} | "
                "global busy={} mode={} list {} entries, {} unfinished:{}",
                NowMs(), g_tick.load(), obj, a, b, busy, ctx.r3.u32, REX_LOAD_U8(0x83496C3Bu), mode, n_total,
                n_pending, pending);
  }
}

// The unfinished entry above: sub_82A4F520(entry, f1 = now). Logged when it finishes (+3692 0 -> 1): elapsed since
// t0 against the timeout, i.e. whether the entities reported ready or the timeout ended the wait.
extern "C" void __imp__sub_82A4F520(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82A4F520(PPCContext& __restrict ctx, uint8_t* base) {
  if (!TraceOn()) {
    __imp__sub_82A4F520(ctx, base);
    return;
  }
  const uint32_t o = ctx.r3.u32;
  const double now = ctx.f1.f64;
  const uint8_t before = REX_LOAD_U8(o + 3692);
  __imp__sub_82A4F520(ctx, base);
  if (!before && REX_LOAD_U8(o + 3692)) {
    auto f64 = [&](uint32_t a) {
      const uint64_t u = REX_LOAD_U64(a);
      double d;
      std::memcpy(&d, &u, 8);
      return d;
    };
    const double t0 = f64(o + 3696), timeout = f64(o + 3704);
    REXLOG_INFO("[load-hold] t={:.1f} tick {} entry {:08X} finished: elapsed {:.3f} s, timeout {:.3f} s -> {}", NowMs(),
                g_tick.load(), o, now - t0, timeout,
                now - t0 > timeout ? "TIMEOUT ended it" : "entities ready (before the timeout)");
  }
}
