// Hand-tuned override: trace-only wrappers for the pale-root investigation (docs/pale-root-20261014.md).
// The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// Off unless FABLE2_PALE_ROOT_TRACE=1. Every wrapper runs the original (__imp__) unchanged; when off
// it is one cached-bool (or one relaxed atomic) test per call. Nothing here changes game behaviour.
//
// - Mesh walker sub_8221BCC8(r3 = mesh instance, r4 = render context, r5 = material set): it asks
//   sub_8222CDD8(r3, [r3+52], lod) for a sub-mesh list ([list+4] .. [list+8], 64-byte records:
//   +4 material index, +12 primitive count, +16 start) and applies material [set+4] + index * 180
//   (docs/hle-census-20261013.md §3). Every record whose primitive count is in
//   FABLE2_PALE_ROOT_COUNTS (default 457: the Bower Lake root's strip) is logged once per distinct
//   material content, with the material's texture references (slot i at +28 + 20 i points at a
//   texture handle; mat[i + 1] is the slot's texture reference object).
// - FABLE2_PALE_ROOT_WATCH=1: on the first such material, a host thread polls its texture slots and
//   its texture references' fields for 60 s and logs every change with all guest threads' lr chains.
// - Per-material streaming update sub_82A94648(mat, &size, ...) for the watched material, and the
//   per-model size cache sub_82B197C8(model, f1 = distance, ..., r5 = view): the cause of the pale
//   state (the size computed from the shadow view's distance is never recomputed while the
//   first-drawn instance's distance stays the same).
#include "fable_2_pch.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/logging/macros.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#include <thread>

extern "C" void __imp__sub_8221BCC8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8222CDD8(PPCContext& __restrict ctx, uint8_t* base);

namespace {

bool On() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_PALE_ROOT_TRACE");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  return on;
}

const std::vector<uint32_t>& Counts() {
  static const std::vector<uint32_t> counts = [] {
    std::vector<uint32_t> out;
    const char* v = std::getenv("FABLE2_PALE_ROOT_COUNTS");  // getenv-ok: once
    std::string list = v ? v : "457";
    size_t pos = 0;
    while (pos < list.size()) {
      size_t comma = list.find(',', pos);
      if (comma == std::string::npos) comma = list.size();
      out.push_back(uint32_t(std::strtoul(list.substr(pos, comma - pos).c_str(), nullptr, 10)));
      pos = comma + 1;
    }
    return out;
  }();
  return counts;
}

thread_local uint32_t t_last_list = 0;
thread_local uint32_t t_last_lod = 0;
std::atomic<uint32_t> g_watch_slot{0};  // material slot 0 address once the watch starts

// Watcher (FABLE2_PALE_ROOT_WATCH=1 with the trace): on the first matching material, a host
// thread polls the material's first two texture slots and the fields of its first two texture
// references for 60 s and logs every change with the time and each guest thread's lr and a short
// back chain (the writer is usually still in or just above the function that stored).
uint32_t Be32(const uint8_t* mb, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, mb + a, 4);
  return __builtin_bswap32(v);
}

std::string GuestStacks(const uint8_t* mb) {
  std::string out;
  auto* ks = rex::system::kernel_state();
  if (!ks) return out;
  for (auto& t : ks->object_table()->GetObjectsByType<rex::system::XThread>()) {
    if (!t || !t->is_guest_thread() || !t->is_running() || !t->thread_state()) continue;
    PPCContext* c = t->thread_state()->context();
    const uint32_t lr = uint32_t(*reinterpret_cast<volatile uint64_t*>(&c->lr));
    uint32_t sp = *reinterpret_cast<volatile uint32_t*>(&c->r1.u32);
    out += fmt::format("\n    {:08X} {} lr {:08X}", t->thread_id(),
                       t->thread() ? t->thread()->name() : std::string("?"), lr);
    // Stack range from the guest PCR (stack base +0x70 = high, stack end +0x74 = low), as the
    // flight recorder does; never read outside it (guard pages fault).
    if (!t->pcr_ptr()) continue;
    const uint32_t hi = Be32(mb, t->pcr_ptr() + 0x70), lo = Be32(mb, t->pcr_ptr() + 0x74);
    if (!(lo < hi && hi - lo <= (16u << 20))) continue;
    for (int i = 0; i < 10; ++i) {
      if (sp < lo || sp + 4 > hi || (sp & 3)) break;
      const uint32_t back = Be32(mb, sp);
      if (back <= sp || back + 4 > hi || back - 8 < lo) break;
      out += fmt::format(" <{:08X}", Be32(mb, back - 8));
      sp = back;
    }
  }
  return out;
}

void StartWatch(uint32_t mat) {
  static std::atomic<bool> started{false};
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_PALE_ROOT_WATCH");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  if (!on || started.exchange(true)) return;
  g_watch_slot.store(mat + 28, std::memory_order_relaxed);
  auto* mem = rex::system::kernel_state()->memory();
  const uint8_t* mb = mem->virtual_membase();
  std::thread([mb, mat] {
    struct W { uint32_t addr; const char* what; uint32_t last; };
    const uint32_t m1 = Be32(mb, mat + 4), m3 = Be32(mb, mat + 12);
    std::vector<W> w = {{mat + 28, "mat.slot0", 0}, {mat + 68, "mat.slot1", 0},
                        {m1 + 8, "tex1.+8", 0},
                        {m1 + 24, "tex1.+24", 0},   {m1 + 36, "tex1.+36", 0},
                        {m3 + 8, "tex3.+8", 0},
                        {m1 + 84, "tex1.+84", 0},   {m1 + 88, "tex1.+88", 0},
                        {m1 + 92, "tex1.+92", 0},   {m1 + 128, "tex1.+128", 0},
                        {m1 + 132, "tex1.+132", 0}, {m1 + 160, "tex1.+160", 0},
                        {m1 + 148, "tex1.+148", 0}, {m1 + 68, "tex1.+68", 0},
                        {m3 + 24, "tex3.+24", 0},   {m3 + 36, "tex3.+36", 0}};
    for (auto& x : w) x.last = Be32(mb, x.addr);
    std::string init;
    for (auto& x : w) init += fmt::format(" {}@{:08X}={:08X}", x.what, x.addr, x.last);
    REXLOG_INFO("[pale-root] watch mat 0x{:08X} tex1 0x{:08X} tex3 0x{:08X}:{}", mat, m1, m3, init);
    const auto t0 = std::chrono::steady_clock::now();
    int changes = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) {
      for (auto& x : w) {
        const uint32_t v = Be32(mb, x.addr);
        if (v == x.last) continue;
        if (++changes > 300) break;
        const std::string stacks = GuestStacks(mb);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        REXLOG_INFO("[pale-root] change {} @{:08X} {:08X} -> {:08X} at {:.3f} ms{}", x.what, x.addr, x.last, v,
                    ms, stacks);
        x.last = v;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    REXLOG_INFO("[pale-root] watch end");
  }).detach();
}

}  // namespace

extern "C" void sub_8222CDD8(PPCContext& __restrict ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_8222CDD8(ctx, base);
    return;
  }
  const uint32_t lod = ctx.r5.u32;
  __imp__sub_8222CDD8(ctx, base);
  t_last_list = ctx.r3.u32;
  t_last_lod = lod;
}

extern "C" void sub_8221BCC8(PPCContext& __restrict ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_8221BCC8(ctx, base);
    return;
  }
  const uint32_t inst = ctx.r3.u32;
  const uint32_t set = ctx.r5.u32;
  t_last_list = 0;
  __imp__sub_8221BCC8(ctx, base);
  const uint32_t list = t_last_list;
  if (!list || !set) return;
  const uint32_t begin = REX_LOAD_U32(list + 4);
  const uint32_t end = REX_LOAD_U32(list + 8);
  if (end <= begin || end - begin > 64 * 4096) return;
  const uint32_t mats = REX_LOAD_U32(set + 4);
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  for (uint32_t rec = begin; rec + 64 <= end; rec += 64) {
    const uint32_t count = REX_LOAD_U32(rec + 12);  // primitive count (+16 is the start)
    bool match = false;
    for (uint32_t c : Counts()) match |= c == count;
    if (!match) continue;
    const uint32_t index = REX_LOAD_U32(rec + 4);
    const uint32_t mat = mats + index * 180;
    StartWatch(mat);
    // The material's apply function (sub_8221BE38 inline: effect object, vtable +44).
    uint32_t apply = 0;
    {
      const uint32_t r24 = REX_LOAD_U32(inst + 52);
      const int32_t ei = int32_t(REX_LOAD_U32(r24 + 60));
      const uint32_t eff = ei < 0 ? REX_LOAD_U32(0x834A5518u)
                                  : REX_LOAD_U32(REX_LOAD_U32(REX_LOAD_U32(r24 + 4) + 88) + uint32_t(ei) * 4);
      if (eff) apply = REX_LOAD_U32(REX_LOAD_U32(eff) + 44);
    }
    std::string recs, matd;
    for (uint32_t i = 0; i < 16; ++i) recs += fmt::format(" {:08X}", REX_LOAD_U32(rec + i * 4));
    for (uint32_t i = 0; i < 45; ++i) matd += fmt::format(" {:08X}", REX_LOAD_U32(mat + i * 4));
    // Texture references: every material dword that points into guest memory, its first 12
    // dwords, and for each pointer among those the dword at +0x20 (a D3D texture header's fetch
    // constant dword 1 = base address, if it is one).
    std::string refs;
    auto is_ptr = [](uint32_t v) { return v >= 0x40000000u && v < 0x50000000u && !(v & 3); };
    for (uint32_t i = 0; i < 45; ++i) {
      const uint32_t p = REX_LOAD_U32(mat + i * 4);
      if (!is_ptr(p)) continue;
      refs += fmt::format("\n  m[{}]=0x{:08X}:", i, p);
      for (uint32_t j = 0; j < 12; ++j) {
        const uint32_t q = REX_LOAD_U32(p + j * 4);
        refs += fmt::format(" {:08X}", q);
        if ((q >= 0x40000000u && q < 0x50000000u && !(q & 3)) ||
            (q >= 0xA0000000u && !(q & 3))) {
          refs += fmt::format("(+20={:08X})", REX_LOAD_U32(q + 0x20));
        }
      }
    }
    const std::string key = fmt::format("{:08X}/{:08X}{}", set, rec, refs);
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (seen.size() > 2000 || !seen.insert(key).second) continue;
    }
    REXLOG_INFO(
        "[pale-root] walk inst 0x{:08X} set 0x{:08X} mats 0x{:08X} lod {} list 0x{:08X} rec "
        "0x{:08X} count {} index {} mat 0x{:08X} apply 0x{:08X}\n  rec:{}\n  mat:{}{}",
        inst, set, mats, t_last_lod, list, rec, count, index, mat, apply, recs, matd, refs);
  }
}

// Per-material streaming update (sub_82A94648(mat, size, ...)) for the watched material: count calls
// and log the result and the first texture reference's fields on the first calls and every 64th.
extern "C" void __imp__sub_82A94648(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82A94648(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t slot = g_watch_slot.load(std::memory_order_relaxed);
  const uint32_t mat = ctx.r3.u32;
  if (!slot || mat + 28 != slot) {
    __imp__sub_82A94648(ctx, base);
    return;
  }
  const uint32_t size_ptr = ctx.r4.u32;
  const uint32_t w = REX_LOAD_U32(size_ptr), h = REX_LOAD_U32(size_ptr + 4);
  const uint32_t r5 = ctx.r5.u32, r6 = ctx.r6.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82A94648(ctx, base);
  static std::atomic<uint32_t> calls{0};
  const uint32_t n = calls.fetch_add(1);
  if (n < 8 || (n & 63) == 0) {
    const uint32_t t = REX_LOAD_U32(mat + 4);
    REXLOG_INFO(
        "[pale-root] stream-update #{} mat {:08X} (lr {:08X} size {}x{} r5 {:08X} r6 {:08X}) -> {} slot0 {:08X}; "
        "tex1 {:08X}: +24 {} +84 {:08X} +88 {} +92 {:08X} +100 {:08X} +128 {:08X} +132 {} +148 {} +152 {} +160 {} +68 {:08X}",
        n, mat, lr, w, h, r5, r6, ctx.r3.u32 & 0xFF, REX_LOAD_U32(slot), t, REX_LOAD_U32(t + 24),
        REX_LOAD_U32(t + 84), int32_t(REX_LOAD_U32(t + 88)), REX_LOAD_U32(t + 92), REX_LOAD_U32(t + 100),
        REX_LOAD_U32(t + 128), int32_t(REX_LOAD_U32(t + 132)), REX_LOAD_U32(t + 148), REX_LOAD_U32(t + 152),
        REX_LOAD_U32(t + 160), REX_LOAD_U32(t + 68));
  }
}

// Per-model texture-size update (sub_82B197C8(model, f1 = distance, f2 = scale, r5 = view, ...)).
// Every model keeps its first 6 calls as raw values (cheap: no formatting on the render thread):
// frame, view, distance, the distance cache before (+32 previous frame's minimum, +36 this frame's,
// +40 distance of the last size computation) and after (+40), and its first material's cached size
// (+168/+172). When the watch starts on a material, the owning model's history is logged; later calls
// for it are logged on the first 12 and every 256th.
namespace {
struct SizeCall {
  uint32_t frame, view, lr, w, h;
  float dist, c32, c36, c40, c40_after;
};
struct ModelHistory {
  SizeCall calls[6];
  uint32_t n = 0;
  bool dumped = false;
};
std::mutex g_hist_mutex;
std::unordered_map<uint32_t, ModelHistory>* g_hist = new std::unordered_map<uint32_t, ModelHistory>();
std::string FormatCall(const SizeCall& c) {
  auto m = [](float v) { return v > 1e30f ? std::string("MAX") : fmt::format("{:.3f}", v); };
  return fmt::format("frame {} view {:08X} lr {:08X} dist {:.3f} | before +32 {} +36 {} +40 {} | after +40 {} size {}x{}",
                     c.frame, c.view, c.lr, c.dist, m(c.c32), m(c.c36), m(c.c40), m(c.c40_after), c.w, c.h);
}
}  // namespace
// Called by the sub_82B197C8 override in LodSizeCacheFix.cpp (impl = original or fixed copy).
bool fable2_pale_root_trace_on() { return On(); }
void fable2_pale_root_size_trace(PPCContext& __restrict ctx, uint8_t* base,
                                 void (*impl)(PPCContext&, uint8_t*)) {
  const uint32_t model = ctx.r3.u32;
  auto f = [&](uint32_t a) {
    const uint32_t v = REX_LOAD_U32(a);
    float x;
    std::memcpy(&x, &v, 4);
    return x;
  };
  SizeCall c{};
  c.dist = float(ctx.f1.f64);
  c.view = ctx.r5.u32;
  c.lr = uint32_t(ctx.lr);
  c.c32 = f(model + 32);
  c.c36 = f(model + 36);
  c.c40 = f(model + 40);
  c.frame = REX_LOAD_U32(REX_LOAD_U32(0x8349F7E4u) + 1300);
  impl(ctx, base);
  const uint32_t mb = REX_LOAD_U32(model + 20), me = REX_LOAD_U32(model + 24);
  c.c40_after = f(model + 40);
  c.w = mb ? REX_LOAD_U32(mb + 168) : 0;
  c.h = mb ? REX_LOAD_U32(mb + 172) : 0;
  const uint32_t slot = g_watch_slot.load(std::memory_order_relaxed);
  const bool watched = slot && slot - 28 >= mb && slot - 28 < me;
  std::lock_guard<std::mutex> lock(g_hist_mutex);
  ModelHistory& h = (*g_hist)[model];
  if (h.n < 6) h.calls[h.n] = c;
  ++h.n;
  static uint32_t n = 0;
  if (watched) {
    if (!h.dumped) {
      h.dumped = true;
      for (uint32_t i = 0; i < std::min<uint32_t>(h.n, 6); ++i)
        REXLOG_INFO("[pale-root] size-history model {:08X} #{}: {}", model, i, FormatCall(h.calls[i]));
    }
    if (n < 12 || (n & 255) == 0) REXLOG_INFO("[pale-root] size-update model {:08X} #{}: {}", model, n, FormatCall(c));
    ++n;
  }
}
