// HLE census, command-processor side (trace only; FABLE2_HLE_CENSUS=1, docs/hle-census-20261013.md).
//
// The executable wraps every guest function that builds a PM4 draw header
// (src/core/hotfunc/gpu/HleCensus.cpp) and records each header's guest physical address with a
// "site" id (builder function, caller lr). The SDK command processor calls
// OnDrawPacketForCensus for every DRAW_INDX / DRAW_INDX_2 it parses and
// OnIndirectBufferForCensus for every indirect buffer (sdk-metal-hle-census.patch). This file
// looks the headers up, attributes the draws the backend issues to sites, and logs one
// "[hle-census]" line per frame:
//   pk    draw packets parsed           skip  dropped by the bin predicate
//   col / dep / copy / oth   issued draws by EDRAM mode (colour+depth, depth only, resolve, other)
//   unm   draws whose header no wrapped builder wrote (precompiled / other paths)
//   uniq  distinct header addresses     maxx  most executions of one header (tiling replays)
//   ib / ibu  indirect buffers executed / distinct
//   sites id:n/u  issued draws / distinct headers per site (-1 = unmatched, -2 = predicate-skipped)
// Off: Enabled() is one cached bool; the SDK hooks are not called at all.
#pragma once

#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "rex/logging.h"

namespace rex::graphics::metal::hle_census {

inline bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_HLE_CENSUS");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  return on;
}

struct State {
  using LookupFn = int (*)(uint32_t);
  using SiteFn = int (*)(int, uint32_t*, uint32_t*, uint64_t*, uint64_t*);
  using UnscannedFn = uint64_t (*)();
  LookupFn lookup = nullptr;
  SiteFn site = nullptr;
  UnscannedFn unscanned = nullptr;
  bool resolved = false;

  int current_site = -1;  // site of the draw packet being executed
  // Per frame.
  uint32_t packets = 0, skipped = 0, col = 0, dep = 0, copy = 0, other = 0, unmatched = 0;
  uint32_t ibs = 0;
  struct Exec {
    int site;
    uint32_t n;
  };
  std::unordered_map<uint32_t, Exec> execs;  // header phys -> (site, executions)
  std::unordered_set<uint32_t> ib_ptrs;
  std::map<int, uint32_t> per_site;
  std::unordered_set<int> announced;
  uint64_t frames = 0;
  uint64_t collapse_skipped_ibs = 0;
};

inline State& S() {
  static State* s = new State();
  return *s;
}

inline void Resolve() {
  State& s = S();
  if (s.resolved) return;
  s.resolved = true;
  s.lookup = reinterpret_cast<State::LookupFn>(dlsym(RTLD_DEFAULT, "fable2_hle_census_lookup"));
  s.site = reinterpret_cast<State::SiteFn>(dlsym(RTLD_DEFAULT, "fable2_hle_census_site"));
  s.unscanned =
      reinterpret_cast<State::UnscannedFn>(dlsym(RTLD_DEFAULT, "fable2_hle_census_unscanned"));
  REXLOG_INFO("[hle-census] on: lookup {} (executable wrappers {})", s.lookup ? "found" : "MISSING",
              s.site ? "found" : "MISSING");
}

inline void OnDrawPacket(uint32_t phys, bool skipped) {
  State& s = S();
  Resolve();
  ++s.packets;
  if (skipped) {
    ++s.skipped;
    s.current_site = -2;
    return;
  }
  const int site = s.lookup ? s.lookup(phys) : -1;
  State::Exec& ex = s.execs.emplace(phys, State::Exec{site, 0}).first->second;
  ++ex.n;
  s.current_site = site;
  if (site >= 0 && s.site && s.announced.insert(site).second) {
    uint32_t b = 0, lr = 0;
    uint64_t c = 0, d = 0;
    if (s.site(site, &b, &lr, &c, &d)) {
      REXLOG_INFO("[hle-census] site {} builder {:08X} lr {:08X}", site, b, lr);
    }
  }
}

inline void OnIndirectBuffer(uint32_t ptr) {
  State& s = S();
  ++s.ibs;
  s.ib_ptrs.insert(ptr);
}

// kind: 0 colour+depth, 1 depth only, 2 copy (resolve), 3 other.
inline void OnIssueDraw(int kind) {
  State& s = S();
  switch (kind) {
    case 0: ++s.col; break;
    case 1: ++s.dep; break;
    case 2: ++s.copy; break;
    default: ++s.other; break;
  }
  if (s.current_site == -1) ++s.unmatched;
  ++s.per_site[s.current_site];
  s.current_site = -1;
}

inline int CurrentSite() { return S().current_site; }

// FABLE2_TILING_COLLAPSE (docs/hle-census-20261013.md section 10). "skip2" is an UNSAFE
// diagnostic: indirect buffers executed while the bin select names only the second tile's bins are
// skipped, so the bottom rows of the main pass are left stale; it bounds the possible saving.
enum class CollapseMode { kOff, kSkipSecondTile };
inline CollapseMode Collapse() {
  static const CollapseMode m = [] {
    const char* v = std::getenv("FABLE2_TILING_COLLAPSE");  // getenv-ok: once
    if (v && std::string(v) == "skip2") return CollapseMode::kSkipSecondTile;
    return CollapseMode::kOff;
  }();
  return m;
}

// FABLE2_TILING_TRACE=<frame> (with FABLE2_HLE_CENSUS=1): the packet stream of frames
// [frame, frame + FABLE2_TILING_TRACE_FRAMES) as "[tiling-trace]" lines: indirect buffers,
// type-3 packets (opcode, predicate, first data dwords) and, from the command processor, each
// draw / resolve with the tiling-relevant registers. Diagnostic for the tiling collapse.
inline uint64_t TilingTraceFirst() {
  static const uint64_t f = [] {
    const char* v = std::getenv("FABLE2_TILING_TRACE");  // getenv-ok: once
    return v ? std::strtoull(v, nullptr, 10) : 0ull;
  }();
  return f;
}
inline bool TilingTraceActive(uint64_t frame) {
  static const uint64_t n = [] {
    const char* v = std::getenv("FABLE2_TILING_TRACE_FRAMES");  // getenv-ok: once
    const uint64_t k = v ? std::strtoull(v, nullptr, 10) : 0ull;
    return k ? k : 2ull;
  }();
  const uint64_t f = TilingTraceFirst();
  return f && frame >= f && frame < f + n;
}

inline void EndFrame(uint64_t frame) {
  State& s = S();
  uint32_t maxx = 0, replayed = 0;
  std::map<int, uint32_t> uniq_site;
  for (const auto& e : s.execs) {
    if (e.second.n > maxx) maxx = e.second.n;
    if (e.second.n > 1) ++replayed;
    ++uniq_site[e.second.site];
  }
  std::string sites;
  for (const auto& e : s.per_site) {  // site:issued draws/distinct headers
    sites += " " + std::to_string(e.first) + ":" + std::to_string(e.second) + "/" +
             std::to_string(uniq_site[e.first]);
  }
  REXLOG_INFO(
      "[hle-census] f{} pk={} skip={} col={} dep={} copy={} oth={} unm={} uniq={} maxx={} rep={} "
      "ib={} ibu={} unscanned={} sites{}",
      frame, s.packets, s.skipped, s.col, s.dep, s.copy, s.other, s.unmatched, s.execs.size(), maxx,
      replayed, s.ibs, s.ib_ptrs.size(), s.unscanned ? s.unscanned() : 0, sites);
  if (++s.frames % 600 == 0 && s.site) {
    for (int id = 0;; ++id) {
      uint32_t b = 0, lr = 0;
      uint64_t c = 0, d = 0;
      if (!s.site(id, &b, &lr, &c, &d)) break;
      REXLOG_INFO("[hle-census] guest site {} builder {:08X} lr {:08X} calls {} draw headers {}",
                  id, b, lr, c, d);
    }
  }
  s.packets = s.skipped = s.col = s.dep = s.copy = s.other = s.unmatched = s.ibs = 0;
  s.execs.clear();
  s.ib_ptrs.clear();
  s.per_site.clear();
}

}  // namespace rex::graphics::metal::hle_census
