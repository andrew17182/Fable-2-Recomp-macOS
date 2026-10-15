// fable2_text_probe.h - runtime UI-widget tree probe.
//
// The per-frame UI text pass (sub_82BFD850 -> UIText_FrameRender -> UIText_FrameRenderIter
// over the global list at 0x83314A50) calls vtable slot 2 on each list
// item; for Fable 2's title/menu UI that method is sub_822A2948 on a
// container holding up to 6 widget pointers (object +0..+20).
//
// This strong override of sub_822A2948 dumps, once per unique object, a
// 256-byte view of each widget plus one level of followed heap pointers,
// scanning for printable ASCII so the "Press A to start" string storage
// can be located. Then it forwards to the original function.
//
//   FABLE2_TEXT_PROBE=1              enable
//   FABLE2_TEXT_PROBE_DELAY=<sec>    start N s after the first call (0 = now)
//   FABLE2_TEXT_PROBE_DUR=<sec>      window duration (default 8)
//
// Log: fable2_text_probe.log in the CWD (exe dir).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include <rex/ppc/context.h>

#include "fable2_deadbeef.h"
#include "fable2_session_telemetry.h"
#include "fable2_ui_render_probe.h"

// On-screen text evidence for the game-state classifier (defined in
// fable2_state_probe.h): ticks a per-item counter and, throttled, extracts
// the strings the item renders. Cheap enough to call for every UI text item.
namespace fable2::stateprobe {
void sample_text_item(const uint8_t* base, PPCContext& ctx);
}

namespace fable2::textprobe {

inline bool enabled() {
#ifdef _WIN32
  char v[8] = {};
  size_t n = 0;
  return ::getenv_s(&n, v, sizeof(v), "FABLE2_TEXT_PROBE") == 0 && v[0] == '1';
#else
  // Cached (perf audit 2026-10-11 candidate 14); FABLE2_PROBE_GETENV_CACHE=0 reads it every call.
  static const bool cache = [] {
    const char* c = std::getenv("FABLE2_PROBE_GETENV_CACHE");
    return !(c && c[0] == '0');
  }();
  static const bool cached = [] {
    const char* v = std::getenv("FABLE2_TEXT_PROBE");
    return v != nullptr && v[0] == '1';
  }();
  if (cache) return cached;
  const char* v = std::getenv("FABLE2_TEXT_PROBE");  // getenv-ok: FABLE2_PROBE_GETENV_CACHE=0 only
  return v != nullptr && v[0] == '1';
#endif
}

inline double delay_seconds() {
  const char* v = std::getenv("FABLE2_TEXT_PROBE_DELAY");
  return v ? std::atof(v) : 0.0;
}

inline double duration_seconds() {
  const char* v = std::getenv("FABLE2_TEXT_PROBE_DUR");
  return v ? std::atof(v) : 8.0;
}

inline FILE*& logf() {
  static FILE* f = [] {
#ifdef _WIN32
    FILE* out = nullptr;
    if (::fopen_s(&out, "fable2_text_probe.log", "w") != 0) out = nullptr;
    return out;
#else
    return std::fopen("fable2_text_probe.log", "w");
#endif
  }();
  return f;
}

// Guest memory read via the recompiled image base. REX_PHYS_HOST_OFFSET:
// MMIO (>=0xE0000000) sits behind a 0x1000 gap on Win32 host mappings.
inline void guest_read(const uint8_t* base, uint32_t addr, void* dst,
                       size_t n) {
  uint8_t* out = static_cast<uint8_t*>(dst);
  const uint64_t off =
      (addr >= 0xE0000000u) ? (static_cast<uint64_t>(addr) + 0x1000u)
                            : static_cast<uint64_t>(addr);
  std::memcpy(out, base + off, n);
}

inline bool in_image(uint32_t a) {
  // Image 0x82000000 + 0x1620000 (see fable_2_pch.h REX_IMAGE_SIZE).
  return a >= 0x82000000u && a < 0x82000000u + 0x1620000u;
}

inline bool valid_guest_addr(uint32_t a) {
  // Low guest heap (Xbox 360 user space, below the image) or the image
  // itself / just past it. Page-ish aligned to skip junk.
  return ((a >= 0x1000u && a < 0x82000000u) ||
          (a >= 0x82000000u && a < 0x84000000u)) && (a & 3) == 0;
}

inline bool looks_like_heap_ptr(uint32_t a) {
  return valid_guest_addr(a) && !in_image(a);
}

// Prints every C-string (>= 4 printable chars) found in p[0..n).
inline void scan_strings(FILE* f, const char* label, uint32_t base_addr,
                         const uint8_t* p, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (p[i] >= 0x20 && p[i] < 0x7F) {
      size_t j = i;
      while (j < n && p[j] >= 0x20 && p[j] < 0x7F) ++j;
      if (j - i >= 4) {
        char buf[192];
        std::snprintf(
            buf, sizeof(buf), "      [%s 0x%08X] \"%.*s\"\n", label,
            static_cast<uint32_t>(base_addr + i),
            static_cast<int>(std::min<size_t>(j - i, 96)),
            reinterpret_cast<const char*>(p + i));
        std::fputs(buf, f);
      }
      i = j;
    } else {
      ++i;
    }
  }
}

// Dumps 256 bytes at guest `addr` with hex + printable scan.
inline void dump_region(FILE* f, const uint8_t* base, uint32_t addr,
                        const char* label) {
  static std::set<uint32_t> dumped;
  if (!dumped.insert(addr).second) return;

  uint8_t buf[256];
  guest_read(base, addr, buf, sizeof(buf));
  char line[512];
  std::snprintf(line, sizeof(line), "== 0x%08X (%s) ==\n", addr, label);
  std::fputs(line, f);
  for (int off = 0; off < 256; off += 16) {
    char p[19] = {};
    for (int k = 0; k < 16; ++k) {
      p[k] = (buf[off + k] >= 0x20 && buf[off + k] < 0x7F) ? buf[off + k] : '.';
    }
    std::snprintf(line, sizeof(line), "   0x%08X:", addr + off);
    for (int k = 0; k < 16; ++k) {
      char h[8];
      std::snprintf(h, sizeof(h), " %02X", buf[off + k]);
      std::strncat(line, h, sizeof(line) - std::strlen(line) - 1);
    }
    std::strncat(line, " ", sizeof(line) - std::strlen(line) - 1);
    std::strncat(line, p, sizeof(line) - std::strlen(line) - 1);
    std::strncat(line, "\n", sizeof(line) - std::strlen(line) - 1);
    std::fputs(line, f);
  }
  scan_strings(f, label, addr, buf, sizeof(buf));
}

// Walks a widget object: dumps it, then follows heap pointers to `depth`.
inline void dump_widget(FILE* f, const uint8_t* base, uint32_t w,
                        const char* label, int depth) {
  if (!valid_guest_addr(w)) return;
  dump_region(f, base, w, label);
  if (depth <= 0) return;

  uint32_t words[64];
  guest_read(base, w, words, sizeof(words));
  int followed = 0;
  for (int i = 0; i < 64 && followed < 16; ++i) {
    if (looks_like_heap_ptr(words[i])) {
      dump_widget(f, base, words[i], "follow", depth - 1);
      ++followed;
    }
  }
}

inline void flush_note(const char* msg) {
  FILE* f = logf();
  if (!f) return;
  std::fputs(msg, f);
  std::fflush(f);
}

// Per-frame entry: `root` = container (r3 of sub_822A2948).
inline void on_frame(const uint8_t* base, PPCContext& ctx) {
  FILE* f = logf();
  if (!f) return;

  static std::set<uint32_t> seen;
  static std::set<uint64_t> logged_raw;
  const uint32_t root = ctx.r3.u32;
  if (logged_raw.insert((uint64_t)root | ((uint64_t)ctx.lr << 32)).second) {
    uint32_t w0 = 0, w1 = 0, w2 = 0;
    if (valid_guest_addr(root)) {
      guest_read(base, root, &w0, 4);
      guest_read(base, root + 4, &w1, 4);
      guest_read(base, root + 8, &w2, 4);
    }
    char line[192];
    std::snprintf(line, sizeof(line),
                  "raw r3=0x%08X w0=0x%08X w1=0x%08X w2=0x%08X (lr=0x%08X)\n",
                  root, w0, w1, w2, (uint32_t)ctx.lr);
    std::fputs(line, f);
    std::fflush(f);
  }
  if (!valid_guest_addr(root)) return;
  uint32_t vtable = 0;
  guest_read(base, root, &vtable, 4);
  if (!in_image(vtable)) return;  // not a C++ object; skip
  if (!seen.insert(root).second) return;
  if (seen.size() > 300) return;  // cap the log

  char hdr[128];
  std::snprintf(hdr, sizeof(hdr), "\n######## frame-dump root=0x%08X ########\n",
                root);
  std::fputs(hdr, f);

  dump_widget(f, base, root, "root", 2);
  uint32_t child = 0;
  for (int i = 0; i < 6; ++i) {
    guest_read(base, root + i * 4, &child, 4);
    if (!valid_guest_addr(child)) continue;
    dump_widget(f, base, child, "child", 2);
  }
  // The sub_822A2A40-style children store a nested widget at +80.
  for (int i = 0; i < 6; ++i) {
    guest_read(base, root + i * 4, &child, 4);
    if (!valid_guest_addr(child)) continue;
    uint32_t nested = 0;
    guest_read(base, child + 80, &nested, 4);
    if (valid_guest_addr(nested)) dump_widget(f, base, nested, "nested", 2);
  }
  std::fflush(f);
}

inline void run(const uint8_t* base, PPCContext& ctx, int64_t now_us) {
  if (!enabled()) return;
  static const int64_t t0 = now_us;
  const int64_t start_us =
      t0 + static_cast<int64_t>(delay_seconds() * 1e6);
  const int64_t end_us =
      start_us + static_cast<int64_t>(duration_seconds() * 1e6);
  if (now_us < start_us) return;
  if (now_us < end_us) {
    on_frame(base, ctx);
  } else {
    static bool done = false;
    if (!done) {
      done = true;
      flush_note("== window done ==\n");
      std::fprintf(stderr, "[textprobe] window done\n");
    }
  }
}

}  // namespace fable2::textprobe

extern "C" void ProcessAndProcessAndProcess508_822A2948(PPCContext& ctx, uint8_t* base) {
  const int64_t now_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  fable2::textprobe::run(base, ctx, now_us);
  if (fable2::uir::hook("ProcessAndProcessAndProcess508_822A2948", ctx, base)) return;
  __imp__ProcessAndProcessAndProcess508_822A2948(ctx, base);
}

// Second probe point: vtable-slot-1 thunk of the font-module text item base
// class (vtable 0x8200A118). Derived items are constructed ~84x/s while the
// title text is on screen (per-frame regeneration). Dump each unique object
// (fully constructed by the time slot 1 runs) with its referenced memory.
extern "C" void UITextItem_Dispatch(PPCContext& ctx, uint8_t* base) {
  #if defined(__APPLE__)
  RecordPeriodicUiTask(ctx, base);
  #endif
  // State classifier on-screen-text evidence (fable2_state_probe.h).
  fable2::stateprobe::sample_text_item(base, ctx);
  fable2::session::ObserveText(base, ctx.r3.u32);
  // DEADBEEF canary: append " DEADBEEF" to every UI text string (FABLE2_DEADBEEF=1).
  if (fable2::deadbeef::enabled()) fable2::deadbeef::process(base, ctx);
  if (fable2::textprobe::enabled()) {
    const int64_t now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    fable2::textprobe::run(base, ctx, now_us);
  }
  if (fable2::uir::hook("UITextItem_Dispatch", ctx, base)) return;
  __imp__UITextItem_Dispatch(ctx, base);
}
