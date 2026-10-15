// fable2_glyph_probe.h - runtime dump of the UI text-item + inner object.
//
// Hooks UIText_RenderElement (0x82C0A6A0, r3 = text item, r4 = current).
// Dumps the item struct, its inner object (*(item+16)), and the object's
// vtable so the glyph layout / font / atlas can be located.
//
//   FABLE2_GLYPH_PROBE=1        enable
//   FABLE2_GLYPH_PROBE_EVERY=N  dump every Nth call (default 60)
//
// Log: fable2_glyph_probe.log in the CWD (exe dir).

#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/ppc/context.h>

#include "fable2_font_probe.h"

namespace fable2::glyphprobe {

// Cache the env read: the hook fires on the render thread for every text
// element every frame, so the env vars are read exactly once.
inline bool enabled() {
  static const bool on = [] {
#ifdef _WIN32
    char v[8] = {};
    size_t n = 0;
    return ::getenv_s(&n, v, sizeof(v), "FABLE2_GLYPH_PROBE") == 0 && v[0] == '1';
#else
    const char* v = std::getenv("FABLE2_GLYPH_PROBE");
    return v != nullptr && v[0] == '1';
#endif
  }();
  return on;
}

inline int every() {
  static const int step = [] {
#ifdef _WIN32
    char v[16] = {};
    size_t n = 0;
    ::getenv_s(&n, v, sizeof(v), "FABLE2_GLYPH_PROBE_EVERY");
    return v[0] ? std::atoi(v) : 200;
#else
    const char* v = std::getenv("FABLE2_GLYPH_PROBE_EVERY");
    return v ? std::atoi(v) : 200;
#endif
  }();
  return step;
}

inline FILE*& logf() {
  static FILE* f = [] {
    FILE* out = std::fopen("fable2_glyph_probe.log", "w");
    if (out) std::setvbuf(out, nullptr, _IONBF, 0);
    return out;
  }();
  return f;
}

inline void log_line(const char* fmt, ...) {
  FILE* f = logf();
  if (!f) return;
  va_list args;
  va_start(args, fmt);
  std::vfprintf(f, fmt, args);
  va_end(args);
}

inline uint64_t host_addr(const uint8_t* base, uint32_t a) {
  return (uint64_t)(base + (a >= 0xE0000000u ? a + 0x1000u : a));
}

inline bool wr(uint64_t h, size_t n) {
#ifdef _WIN32
  // Validate the FULL range (every page), not just the first.
  uint64_t end = h + n;
  uint64_t cur = h;
  while (cur < end) {
    MEMORY_BASIC_INFORMATION m = {};
    if (!VirtualQuery((void*)cur, &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT) return false;
    if ((m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) ==
        0) return false;
    const uint64_t page_end = (uint64_t)m.BaseAddress + m.RegionSize;
    if (page_end <= cur) break;  // safety: no forward progress
    cur = page_end < end ? page_end : end;
  }
  return true;
#else
  (void)h;
  (void)n;
  return true;
#endif
}

inline void rd(const uint8_t* base, uint32_t a, void* d, size_t n) {
  std::memcpy(d, base + (a >= 0xE0000000u ? a + 0x1000u : a), n);
}

// Guest u32 at a (big-endian load, like the guest's lwz).
inline uint32_t load_be(const uint8_t* base, uint32_t a) {
  uint32_t w = 0;
  rd(base, a, &w, 4);
  return __builtin_bswap32(w);
}

// Big-endian store (like the guest's stw) at guest address a.
inline void store_be(uint8_t* base, uint32_t a, uint32_t v) {
  uint32_t w = __builtin_bswap32(v);
  std::memcpy(base + (a >= 0xE0000000u ? a + 0x1000u : a), &w, 4);
}

// Shift the per-glyph x_offsets in the glyph run at draw_obj+0xE4, moving the
// on-screen text block. The run is a list of {record_ptr, x_offset} pairs; the
// x_offsets are the 2nd word of each pair and live in the ~0x20000-0x3F000
// range. Applied once per run (tracked) so it does not accumulate.
inline void shift_run_x(uint8_t* base, uint32_t draw, uint32_t shift) {
  if (shift == 0 || draw < 0x3F000000u || draw >= 0x84000000u) return;
  uint32_t run = draw + 0xE4;
  static uint32_t runs[128];
  static int count = 0;
  for (int i = 0; i < count; ++i) if (runs[i] == run) return;
  if (!wr(host_addr(base, run), 16 * 8)) return;
  bool any = false;
  for (int i = 0; i < 16; ++i) {
    uint32_t off = run + i * 8 + 4;  // x_offset word of pair i
    uint32_t w = load_be(base, off);
    if (w >= 0x20000 && w < 0x40000) {  // looks like a run x_offset
      store_be(base, off, w + shift);
      any = true;
    }
  }
  if (any && count < 128) runs[count++] = run;
}

// Shift the per-glyph VERTEX x (the first float in each record's vertex data,
// record+0x2C) to move the on-screen text. The vertex data is built by the
// state builder (sub_82C09018) and read by the per-element draw (0x82B4EEE0),
// so this must run in ProcessAndProcessAndProcess1384_82C09870 (after the build, before the draw).
inline void shift_vertex_x(uint8_t* base, uint32_t el, uint32_t shift) {
  if (shift == 0 || el < 0x3F000000u || el >= 0x84000000u) return;
  if (!wr(host_addr(base, el + 16), 4)) return;
  uint32_t draw = load_be(base, (uint32_t)(el + 16));
  if (draw < 0x3F000000u || draw >= 0x84000000u) return;
  uint32_t run = draw + 0xE4;
  if (!wr(host_addr(base, run), 16 * 8)) return;
  for (int i = 0; i < 16; ++i) {
    uint32_t rec = load_be(base, (uint32_t)(run + i * 8));
    if (rec < 0x3F000000u || rec >= 0x84000000u) continue;
    if (!wr(host_addr(base, rec + 0x2C), 8)) continue;
    uint32_t vtx = load_be(base, (uint32_t)(rec + 0x2C));
    if (vtx < 0x3F000000u || vtx >= 0x84000000u) continue;
    if (!wr(host_addr(base, vtx), 4)) continue;
    uint32_t xf = load_be(base, vtx);
    // The prompt's per-glyph x floats are ~0x46xxxxxx (tens of thousands).
    // Shift the first float (x) of every vertex in the 84-byte record.
    if (xf > 0x42000000u && xf < 0x49000000u)
      store_be(base, vtx, xf + shift);
  }
}

// Dump a run of u32 words as guest (big-endian) values, hex lines.
inline void dump_words(const char* name, const uint8_t* base, uint32_t a, int nwords) {
  if (nwords > 64) nwords = 64;
  for (int row = 0; row < nwords; row += 8) {
    char line[160] = {};
    int off = std::snprintf(line, sizeof(line), "%s +0x%02X:", name, row * 4);
    for (int c = row; c < row + 8 && c < nwords; ++c) {
      uint32_t w = 0;
      bool ok = wr(host_addr(base, a + c * 4), 4);
      if (ok) w = load_be(base, a + c * 4);
      off += std::snprintf(line + off, sizeof(line) - off, ok ? " %08X" : " XXXXXXXX", w);
    }
    log_line("%s\n", line);
  }
}

// Decode up to nchars UTF-16BE chars at a as printable ASCII (or '?').
// Returns the number of chars decoded (0 if not valid UTF-16BE text).
inline int dump_str(const char* name, const uint8_t* base, uint32_t a, int nchars) {
  if (!wr(host_addr(base, a), (size_t)nchars * 2)) return 0;
  char s[128] = {};
  int n = 0;
  for (int i = 0; i < nchars && n < 60; ++i) {
    uint8_t b[2] = {};
    rd(base, a + i * 2, b, 2);
    if (b[0] == 0 && b[1] == 0) break;
    if (b[0] != 0) return 0;  // not ASCII UTF-16BE
    s[n++] = (b[1] >= 0x20 && b[1] < 0x7F) ? (char)b[1] : '?';
  }
  if (n < 2) return 0;
  s[n] = 0;
  log_line("%s 0x%08X: \"%s\"\n", name, a, s);
  return n;
}

// Dump 48 words (192 bytes) at a as guest values, 4 bytes at a time,
// each guarded by wr. Words that fail wr are shown as "XXXXXXXX".
inline void dump_block(const char* name, const uint8_t* base, uint32_t a) {
  for (int row = 0; row < 48; row += 8) {
    char line[160] = {};
    int off = std::snprintf(line, sizeof(line), "%s +0x%02X:", name, row * 4);
    for (int c = row; c < row + 8; ++c) {
      uint32_t w = 0;
      bool ok = wr(host_addr(base, a + c * 4), 4);
      if (ok) w = load_be(base, a + c * 4);
      off += std::snprintf(line + off, sizeof(line) - off, ok ? " %08X" : " XXXXXXXX",
                           w);
    }
    log_line("%s\n", line);
  }
}

inline void dump_item(const uint8_t* base, uint32_t item, uint32_t cur) {
  log_line("--- item r3=0x%08X r4=0x%08X ---\n", item, cur);
  dump_block("item", base, item);
  uint32_t obj = 0;
  if (wr(host_addr(base, item + 16), 4)) obj = load_be(base, item + 16);
  log_line("item+16 (obj) = 0x%08X\n", obj);
  if (obj == 0 || obj >= 0xE0000000u || !wr(host_addr(base, obj), 256)) return;
  dump_words("obj", base, obj, 64);  // obj[0..256]
  // The vtable is obj[0]; dump 24 entries (function pointers, guest BE).
  uint32_t vt = load_be(base, obj);
  log_line("obj+0 (vtable) = 0x%08X\n", vt);
  if (vt >= 0x82000000u && vt < 0x84000000u && wr(host_addr(base, vt), 96)) {
    char line[320] = {};
    int off = std::snprintf(line, sizeof(line), "vtable:");
    for (int i = 0; i < 24; ++i) {
      uint32_t e = load_be(base, (uint32_t)(vt + i * 4));
      off += std::snprintf(line + off, sizeof(line) - off, " %08X", e);
    }
    log_line("%s\n", line);
  }
  // Dump the obj's candidate font/pointer fields as 32-word blocks to find
  // the font object + glyph UV table.
  static const int font_offs[] = {0x10, 0x30, 0x40, 0x44, 0x48};
  for (int fo : font_offs) {
    uint32_t p = load_be(base, (uint32_t)(obj + fo));
    if (p < 0x3F000000u || p >= 0x84000000u) continue;
    if (!wr(host_addr(base, p), 128)) continue;
    char lbl[16];
    std::snprintf(lbl, sizeof(lbl), "obj+0x%02X->", fo);
    log_line("%s 0x%08X\n", lbl, p);
    dump_words(lbl, base, p, 32);
  }
  // Scan every word of the object for a pointer to UTF-16BE text and decode.
  int found = 0;
  for (int off = 0; off + 4 <= 256 && found < 8; off += 4) {
    uint32_t p = load_be(base, (uint32_t)(obj + off));
    if (p < 0x3F000000u || p >= 0x84000000u) continue;
    if (!wr(host_addr(base, p), 48)) continue;
    uint8_t b[4] = {};
    rd(base, p, b, 4);
    // UTF-16BE chars: high byte 0, low byte printable (allow space/CR/LF).
    auto okc = [](uint8_t hi, uint8_t lo) {
      return hi == 0 && (lo >= 0x20 && lo < 0x7F);
    };
    if (okc(b[0], b[1]) && okc(b[2], b[3]))
      found += dump_str("obj+str", base, p, 40) ? 1 : 0;
  }
}

}  // namespace fable2::glyphprobe

// Fast per-call capture of the transient per-glyph run (obj+0xE4). Runs on
// every UIText_RenderElement call but is cheap (guarded reads) and dedups
// by (obj, first record pointer).
namespace fable2::glyphprobe {
inline void dump_glyphrun(const uint8_t* base, uint32_t item, uint32_t cur) {
  if (item == 0 || item >= 0xE0000000u) return;
  if (!wr(host_addr(base, item + 16), 4)) return;
  const uint32_t obj = load_be(base, item + 16);
  if (obj == 0 || obj >= 0xE0000000u) return;
  if (!wr(host_addr(base, obj + 0xE4), 4)) return;
  const uint32_t gr = load_be(base, (uint32_t)(obj + 0xE4));
  if (gr == 0 || gr < 0x3F000000u || gr >= 0x84000000u) return;
  static uint64_t seen[32] = {};
  static int nseen = 0;
  const uint64_t key = ((uint64_t)obj << 32) | gr;
  for (int i = 0; i < nseen; ++i) if (seen[i] == key) return;
  if (nseen < 32) seen[nseen++] = key;
  log_line("glyphrun item=0x%08X obj=0x%08X rec0=0x%08X\n", item, obj, gr);
  dump_words("run  ", base, (uint32_t)(obj + 0xE4), 32);
  dump_words("grec0", base, gr, 24);
  // The record's +0x2C points to a 0x54-stride region that likely holds the
  // per-glyph vertex/UV data. Dump the first two entries.
  if (wr(host_addr(base, gr), 0x30)) {
    const uint32_t v0 = load_be(base, (uint32_t)(gr + 0x2C));
    log_line("vtx0 = 0x%08X (rec0+0x2C)\n", v0);
    if (v0 >= 0x3F000000u && v0 < 0x84000000u && wr(host_addr(base, v0), 0x60)) {
      dump_words("vtx0", base, v0, 18);   // 0x48 bytes
      if (wr(host_addr(base, v0 + 0x54), 0x60)) dump_words("vtx1", base, (uint32_t)(v0 + 0x54), 18);
    }
  }
}
}  // namespace fable2::glyphprobe

extern "C" void UIText_RenderElement(PPCContext& ctx, uint8_t* base) {
  // Shift the glyph run's x_offsets (moves the on-screen text block). Applied
  // once per run (tracked), so it does not accumulate across frames.
  {
    static uint32_t shift = 0xFFFFFFFFu;
    if (shift == 0xFFFFFFFFu) {
      const char* es = std::getenv("FABLE2_TA_SHIFT");
      shift = es ? (uint32_t)std::atoi(es) : 0;
    }
    if (shift > 0) {
      uint32_t el = (uint32_t)ctx.r3.u64;
      if (el >= 0x3F000000u && el < 0x84000000u) {
        uint32_t draw = fable2::glyphprobe::load_be(base, (uint32_t)(el + 16));
        fable2::glyphprobe::shift_run_x(base, draw, shift);
      }
    }
  }
  static std::atomic<int> g_count{0};
  if (fable2::glyphprobe::enabled()) {
    // Fast path every call: catch the transient per-glyph run while live.
    fable2::glyphprobe::dump_glyphrun(base, (uint32_t)ctx.r3.u64,
                                      (uint32_t)ctx.r4.u64);
    const int step = fable2::glyphprobe::every() > 0 ? fable2::glyphprobe::every() : 60;
    if (g_count.fetch_add(1) % step == 0)
      fable2::glyphprobe::dump_item(base, (uint32_t)ctx.r3.u64,
                                    (uint32_t)ctx.r4.u64);
  }
  // Probe: dump the vertex-writer object (element+0x24) and the draw object's
  // +0xE0 pointer to find the on-screen transform matrix / position fields.
  if (fable2::glyphprobe::enabled()) {
    static std::atomic<int> mw{0};
    if (mw.fetch_add(1) % 300 == 0) {
      uint32_t el = (uint32_t)ctx.r3.u64;
      if (el >= 0x3F000000u && el < 0x84000000u) {
        uint32_t vw = fable2::glyphprobe::load_be(base, (uint32_t)(el + 36));
        if (vw >= 0x3F000000u && vw < 0x84000000u) {
          fable2::glyphprobe::dump_words("vw   ", base, vw, 12);
          // The two live pointers at vw+0x0C and vw+0x10 (candidate transform).
          for (int o = 0x0C; o <= 0x10; ++o) {
            uint32_t p = fable2::glyphprobe::load_be(base, (uint32_t)(vw + o));
            if (p >= 0x3F000000u && p < 0x84000000u) {
              char l[16] = {};
              std::snprintf(l, sizeof(l), "vw+%X->", o);
              fable2::glyphprobe::dump_words(l, base, p, 16);
            }
          }
          // First glyph node in the entry list (vw+0x28) and the glyph-node ptr.
          uint32_t gnode = fable2::glyphprobe::load_be(base, (uint32_t)(vw + 0x28));
          if (gnode >= 0x3F000000u && gnode < 0x84000000u) {
            fable2::glyphprobe::log_line("[gnode] 0x%08X\n", gnode);
            fable2::glyphprobe::dump_words("gn   ", base, gnode, 16);
          }
          // The transform object's pointer fields (candidate transform matrix).
          uint32_t tr = fable2::glyphprobe::load_be(base, (uint32_t)(vw + 0x0C));
          if (tr >= 0x3F000000u && tr < 0x84000000u) {
            for (int o : {0x10, 0x18, 0x1C, 0x34, 0x38}) {
              uint32_t p = fable2::glyphprobe::load_be(base, (uint32_t)(tr + o));
              if (p >= 0x3F000000u && p < 0x84000000u) {
                char l[16] = {};
                std::snprintf(l, sizeof(l), "tr+%X->", o);
                fable2::glyphprobe::dump_words(l, base, p, 16);
              }
            }
          }
        }
      }
    }
  }
  // Throttled font/glyph-table scan (no-op unless FABLE2_FONT_PROBE=1).
  fable2::fontprobe::maybe_scan(base);
  if (fable2::uir::hook("UIText_RenderElement", ctx, base)) return;
  __imp__UIText_RenderElement(ctx, base);
}

// Hook: ProcessAndProcessAndProcess1384_82C09870 (r3 = element). This is a query (element.vtable[13]) that
// runs AFTER the vertex-emitting calls, so it is NOT the draw. The vertex shift
// is applied in the sub_82C09018 hook instead (right after the state builder).
extern "C" void __imp__ProcessAndProcessAndProcess1384_82C09870(PPCContext& ctx, uint8_t* base);
extern "C" void ProcessAndProcessAndProcess1384_82C09870(PPCContext& ctx, uint8_t* base) {
  if (fable2::uir::hook("ProcessAndProcessAndProcess1384_82C09870", ctx, base)) return;
  __imp__ProcessAndProcessAndProcess1384_82C09870(ctx, base);
}
