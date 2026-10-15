// Hand-tuned override: trace-only wrappers on the XDK D3D draw-packet builders (HLE census).
// The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// Off unless FABLE2_HLE_CENSUS=1 (docs/hle-census-20261013.md). Every wrapper runs the original
// (__imp__) unchanged; when off it is one cached-bool test per call.
//
// The functions below are every guest function that builds a PM4 DRAW_INDX (type-3 opcode 0x22)
// or DRAW_INDX_2 (0x36) header (grep of the generated code for the header constants). They all
// take the D3D device in r3 and append packets at the device's command-buffer write pointer
// [device+48] (the address of the last dword written; stwu pre-increments). When on, each
// outermost call scans the packets it appended, and records the guest physical address of every
// draw header with a "site" id = (builder, caller lr). The segment-kick helper 821E8D20 can run
// inside a builder when the segment fills; its wrapper scans what was written so far first, so
// every draw is registered before the command processor can see it.
//
// The Metal command processor (metal_hle_census.h) looks each executed draw header up by physical
// address through fable2_hle_census_lookup (dlsym), so it can attribute every draw it runs to a
// call site, and count draws that no wrapped builder wrote.
#include "fable_2_pch.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/logging/macros.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#define F2_CENSUS_BUILDERS(X)                             \
  X(sub_82207C30, 0x82207C30u)                            \
  X(sub_8221E0F0, 0x8221E0F0u)                            \
  X(ProcessAndProcessAndProcess576_82217EE8, 0x82217EE8u) \
  X(sub_82B9C068, 0x82B9C068u)                            \
  X(sub_8221C9C8, 0x8221C9C8u)                            \
  X(ProcessAndProcessAndProcess876_822060A0, 0x822060A0u) \
  X(ProcessAndProcessAndProcess841_8221C518, 0x8221C518u) \
  X(sub_82BA5CE8, 0x82BA5CE8u)                            \
  X(sub_82B988C8, 0x82B988C8u)                            \
  X(sub_82B928A8, 0x82B928A8u)                            \
  X(sub_82BA5448, 0x82BA5448u)                            \
  X(ProcessAndProcessAndProcess820_821EF988, 0x821EF988u) \
  X(ProcessAndProcessAndProcess806_822069C0, 0x822069C0u) \
  X(sub_82B98770, 0x82B98770u)                            \
  X(sub_82BA29B8, 0x82BA29B8u)

#define F2_CENSUS_DECL(name, addr) \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base);
F2_CENSUS_BUILDERS(F2_CENSUS_DECL)
extern "C" void __imp__ProcessAndProcessAndProcess940_821E8D20(PPCContext& __restrict ctx, uint8_t* base);

namespace {

bool On() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_HLE_CENSUS");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  return on;
}

// Direct-mapped table: guest physical dword address -> (phys << 32) | (site + 1). 4M entries
// cover 16 MB of command memory without collisions (segments live in a few MB).
constexpr uint32_t kMapBits = 22;
constexpr uint32_t kMapMask = (1u << kMapBits) - 1;
constexpr uint32_t kMaxSites = 4096;

struct Census {
  std::unique_ptr<std::atomic<uint64_t>[]> map;
  std::mutex mutex;
  std::unordered_map<uint64_t, uint32_t> site_ids;  // (builder << 32) | lr -> id
  uint32_t site_builder[kMaxSites] = {};
  uint32_t site_lr[kMaxSites] = {};
  std::atomic<uint32_t> site_count{0};
  std::atomic<uint64_t> calls[kMaxSites] = {};
  std::atomic<uint64_t> draws[kMaxSites] = {};
  std::atomic<uint64_t> unscanned{0};  // outermost calls whose write window could not be parsed
  Census() : map(new std::atomic<uint64_t>[size_t(1) << kMapBits]) {
    for (size_t i = 0; i < (size_t(1) << kMapBits); ++i) map[i].store(0, std::memory_order_relaxed);
  }
};

Census& C() {
  static Census* c = new Census();
  return *c;
}

uint32_t SiteId(uint32_t builder, uint32_t lr) {
  const uint64_t key = (uint64_t(builder) << 32) | lr;
  thread_local uint64_t last_key = ~0ull;
  thread_local uint32_t last_id = 0;
  if (key == last_key) return last_id;
  Census& c = C();
  std::lock_guard<std::mutex> lock(c.mutex);
  auto it = c.site_ids.find(key);
  uint32_t id;
  if (it != c.site_ids.end()) {
    id = it->second;
  } else {
    id = c.site_count.load(std::memory_order_relaxed);
    if (id >= kMaxSites) return kMaxSites - 1;
    c.site_builder[id] = builder;
    c.site_lr[id] = lr;
    c.site_ids.emplace(key, id);
    c.site_count.store(id + 1, std::memory_order_release);
  }
  last_key = key;
  last_id = id;
  return id;
}

struct Active {
  int depth = 0;
  uint32_t device = 0;
  uint32_t last = 0;  // [device+48] when the window opened (last dword written)
  uint32_t site = 0;
};
thread_local Active t_active;

// Parse the packets written in (from, to] and register every draw header.
void Scan(uint8_t* base, uint32_t from, uint32_t to, uint32_t site) {
  Census& c = C();
  if (to <= from || to - from > (1u << 20)) {
    if (to != from) c.unscanned.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto* mem = rex::system::kernel_state()->memory();
  const uint32_t phys_from = mem->GetPhysicalAddress(from + 4);
  if (phys_from == UINT32_MAX) {
    c.unscanned.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint32_t p = from + 4;
  uint64_t n = 0;
  while (p <= to) {
    const uint32_t h = REX_LOAD_U32(p);
    const uint32_t type = h >> 30;
    uint32_t len;  // dwords including the header
    if (type == 0) {
      len = ((h >> 16) & 0x3FFF) + 2;
    } else if (type == 1) {
      len = 3;
    } else if (type == 2) {
      len = 1;
    } else {
      const uint32_t opcode = (h >> 8) & 0x7F;
      len = ((h >> 16) & 0x3FFF) + 2;
      if (opcode == 0x22 || opcode == 0x36) {
        const uint32_t phys = phys_from + (p - (from + 4));
        c.map[(phys >> 2) & kMapMask].store((uint64_t(phys) << 32) | (site + 1),
                                            std::memory_order_release);
        ++n;
      }
    }
    p += len * 4;
  }
  if (n) c.draws[site].fetch_add(n, std::memory_order_relaxed);
}

template <void (*Imp)(PPCContext&, uint8_t*)>
inline void Wrap(uint32_t builder, PPCContext& __restrict ctx, uint8_t* base) {
  if (!On()) {
    Imp(ctx, base);
    return;
  }
  Active& a = t_active;
  if (a.depth > 0) {  // nested builder: the outermost window covers its packets
    ++a.depth;
    Imp(ctx, base);
    --a.depth;
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t site = SiteId(builder, uint32_t(ctx.lr));
  C().calls[site].fetch_add(1, std::memory_order_relaxed);
  a.depth = 1;
  a.device = device;
  a.last = REX_LOAD_U32(device + 48);
  a.site = site;
  Imp(ctx, base);
  uint32_t to = REX_LOAD_U32(device + 48);
  // The "begin" UP builders (82217EE8, 822060A0) write the draw packet but leave [device+48]
  // for the matching "end" call; the pending end of the packet is at [device+13444].
  if (builder == 0x82217EE8u || builder == 0x822060A0u) {
    const uint32_t pending = REX_LOAD_U32(device + 13444);
    if (pending > to && pending - a.last < (1u << 20)) to = pending;
  }
  Scan(base, a.last, to, site);
  a.depth = 0;
}

}  // namespace

#define F2_CENSUS_WRAP(name, addr)                                       \
  extern "C" void name(PPCContext& __restrict ctx, uint8_t* base) {     \
    Wrap<__imp__##name>(addr, ctx, base);                                \
  }
F2_CENSUS_BUILDERS(F2_CENSUS_WRAP)

// Segment kick (r3 = device; returns the new write pointer, also stored at [device+48]).
extern "C" void ProcessAndProcessAndProcess940_821E8D20(PPCContext& __restrict ctx, uint8_t* base) {
  if (!On() || t_active.depth == 0 || ctx.r3.u32 != t_active.device) {
    __imp__ProcessAndProcessAndProcess940_821E8D20(ctx, base);
    return;
  }
  Active& a = t_active;
  const uint32_t device = a.device;
  Scan(base, a.last, REX_LOAD_U32(device + 48), a.site);
  __imp__ProcessAndProcessAndProcess940_821E8D20(ctx, base);
  a.last = REX_LOAD_U32(device + 48);
}

// --- API for the Metal plugin (dlsym RTLD_DEFAULT) -------------------------------------------
// Site id (>= 0) of the draw header at a guest physical address, or -1 if no wrapped builder
// wrote it.
extern "C" __attribute__((visibility("default"), used)) int fable2_hle_census_lookup(uint32_t phys) {
  if (!On()) return -1;
  const uint64_t e = C().map[(phys >> 2) & kMapMask].load(std::memory_order_acquire);
  if (!e || uint32_t(e >> 32) != phys) return -1;
  return int(uint32_t(e) - 1);
}

// Site description: builder function address, caller lr, guest-side calls and draw headers.
extern "C" __attribute__((visibility("default"), used)) int fable2_hle_census_site(
    int id, uint32_t* builder, uint32_t* lr, uint64_t* calls, uint64_t* draws) {
  Census& c = C();
  if (id < 0 || uint32_t(id) >= c.site_count.load(std::memory_order_acquire)) return 0;
  *builder = c.site_builder[id];
  *lr = c.site_lr[id];
  *calls = c.calls[id].load(std::memory_order_relaxed);
  *draws = c.draws[id].load(std::memory_order_relaxed);
  return 1;
}

extern "C" __attribute__((visibility("default"), used)) uint64_t fable2_hle_census_unscanned() {
  return On() ? C().unscanned.load(std::memory_order_relaxed) : 0;
}
