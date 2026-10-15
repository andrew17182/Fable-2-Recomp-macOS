// fable2_ui_text_dump.h - crash-safe dump of the per-frame UI text list.
//
// Hooks UIText_FrameRender (once-per-frame UI text pass -> UIText_FrameRenderIter(0x83314A50,-1)).
// For a bounded number of frames it reads the list descriptor, walks the item
// array (trying several interpretations), and scans each candidate object for
// printable ASCII so we can locate the "Press A to start" bytes and the string
// offset inside a text item.
//
// All guest reads are guarded with VirtualQuery (Windows) so uncommitted arena
// pages are skipped instead of faulting.
//
//   FABLE2_UI_TEXT_DUMP=1           enable
//   FABLE2_UI_TEXT_DUMP_FRAMES=N    default 12
//   FABLE2_UI_TEXT_DUMP_INTERVAL=I  dump every Ith frame (default 20)
// Log: fable2_ui_text.log in the CWD (exe dir).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/ppc/context.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fable2::uitext {

inline bool page_readable(const void* p) {
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi{};
  if (::VirtualQuery(const_cast<void*>(p), &mbi, sizeof(mbi)) == 0) return false;
  return mbi.State == MEM_COMMIT &&
         (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
#else
  (void)p;
  return true;
#endif
}

// Read n bytes at guest addr into dst; returns false if any page is not readable.
inline bool safe_read(const uint8_t* base, uint32_t a, void* dst, size_t n) {
  if (a < 0x1000u) return false;
  uint8_t* d = static_cast<uint8_t*>(dst);
  size_t off = 0;
  while (off < n) {
    uintptr_t host = reinterpret_cast<uintptr_t>(base + a + off);
    size_t page_off = host & 0xFFFUL;
    size_t chunk = 0x1000u - page_off;
    if (chunk > n - off) chunk = n - off;
    if (!page_readable(base + a + off)) return false;
    std::memcpy(d + off, base + a + off, chunk);
    off += chunk;
  }
  return true;
}

inline bool safe_read32(const uint8_t* base, uint32_t a, uint32_t* out) {
  uint32_t v = 0;
  if (!safe_read(base, a, &v, 4)) return false;
  *out = __builtin_bswap32(v);
  return true;
}

inline bool plausible(uint32_t a) { return a >= 0x80000000u && a < 0x84000000u; }

inline void scan_strings(FILE* f, const char* tag, uint32_t base_addr,
                         const uint8_t* p, size_t n, int min_len = 4) {
  size_t i = 0;
  while (i < n) {
    if (p[i] >= 0x20 && p[i] < 0x7F) {
      size_t j = i;
      while (j < n && p[j] >= 0x20 && p[j] < 0x7F) ++j;
      if (j - i >= static_cast<size_t>(min_len)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "      %s 0x%08X +0x%03zx: \"%.*s\"\n",
                      tag, base_addr, i, static_cast<int>(j - i),
                      reinterpret_cast<const char*>(p + i));
        std::fputs(buf, f);
      }
      i = j;
    } else {
      ++i;
    }
  }
}

inline FILE*& logf() {
  static FILE* f = std::fopen("fable2_ui_text.log", "w");
  return f;
}

inline bool& armed() {
  static bool a = [] {
    const char* v = std::getenv("FABLE2_UI_TEXT_DUMP");
    return v && v[0] == '1';
  }();
  return a;
}
inline long env_long(const char* n, long d) {
  const char* v = std::getenv(n);
  return v ? std::atol(v) : d;
}
inline double env_dbl(const char* n, double d) {
  const char* v = std::getenv(n);
  return v ? std::atof(v) : d;
}

inline double now_s() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void dump(const uint8_t* base) {
  if (!armed()) return;
  static const long budget = env_long("FABLE2_UI_TEXT_DUMP_FRAMES", 400);
  static const long interval = env_long("FABLE2_UI_TEXT_DUMP_INTERVAL", 1);
  // Gate to the title-text window (relative to the first UI text pass call).
  static const double start_s = env_dbl("FABLE2_UI_TEXT_DUMP_START", 3.0);
  static const double end_s = env_dbl("FABLE2_UI_TEXT_DUMP_END", 28.0);
  static const double t0 = now_s();
  static std::atomic<long> emitted{0};
  static std::atomic<long> frame{0};
  long n = frame.fetch_add(1);
  if (n % interval != 0) return;
  if (emitted.load() >= budget) return;
  double el = now_s() - t0;
  if (el < start_s || el > end_s) return;

  FILE* f = logf();
  if (!f) return;

  const uint32_t list = 0x83334AA0u;  // from UIText_FrameRender: lis -31949; addi 19104
  uint32_t P = 0, S = 0, A = 0, A264 = 0, A268 = 0;
  if (!safe_read32(base, list + 4, &P)) { std::fputs("  (list+4 unreadable)\n", f); return; }
  if (!safe_read32(base, list + 20, &S)) S = 0;
  if (!safe_read32(base, P, &A)) { std::fputs("  (P unreadable)\n", f); return; }
  if (plausible(A)) {
    safe_read32(base, A + 264, &A264);
    safe_read32(base, A + 268, &A268);
  }
  const uint32_t cnt = (S && plausible(A264)) ? A268 / S : 0;

  char hdr[320];
  std::snprintf(hdr, sizeof(hdr),
                "\n==== dump #%ld list=0x%08X P=0x%08X A=0x%08X stride=%u "
                "A264(B)=0x%08X A268=%u count=%u ====\n",
                emitted.load(), list, P, A, S, A264, A268, cnt);
  std::fputs(hdr, f);

  auto dump_obj = [&](uint32_t item, const char* how, size_t sz) {
    if (!plausible(item)) return;
    static std::atomic<long> guard{0};
    if (guard.load() > 4000) return;  // cap total scans
    uint8_t buf[256];
    if (!safe_read(base, item, buf, sz)) return;
    guard.fetch_add(1);
    uint32_t vtable = __builtin_bswap32(*(const uint32_t*)buf);
    char l[160];
    std::snprintf(l, sizeof(l), "  -- [0x%08X] (%s) vtable=0x%08X\n", item, how, vtable);
    std::fputs(l, f);
    scan_strings(f, "obj", item, buf, sz);
    // follow pointer-ish fields one level (heap only)
    for (size_t k = 0; k + 4 <= sz; k += 4) {
      uint32_t fld = __builtin_bswap32(*(const uint32_t*)(buf + k));
      if (fld >= 0x80000000u && fld < 0x82000000u) {
        uint8_t sub[128];
        if (safe_read(base, fld, sub, sizeof(sub)))
          scan_strings(f, "ptr", fld, sub, sizeof(sub), 5);
      }
    }
  };

  // container
  if (plausible(A)) dump_obj(A, "container", 256);
  // interpretation 1: pointer array at B
  if (plausible(A264))
    for (uint32_t i = 0; i < cnt && i < 48; ++i) {
      uint32_t ptr = 0;
      if (safe_read32(base, A264 + i * 4, &ptr) && plausible(ptr))
        dump_obj(ptr, "ptrarr", 256);
    }
  // interpretation 2: inline items at B + i*stride
  if (plausible(A264) && S)
    for (uint32_t i = 0; i < cnt && i < 48; ++i) {
      uint32_t item = A264 + i * S;
      if (plausible(item)) dump_obj(item, "inline", 256);
    }
  // interpretation 3: literal from disasm (B + A + 272)
  {
    uint32_t item = A264 + A + 272;
    if (plausible(item)) dump_obj(item, "literal", 256);
  }

  emitted.fetch_add(1);
  std::fflush(f);
}

}  // namespace fable2::uitext

extern "C" void UIText_FrameRender(PPCContext& ctx, uint8_t* base) {
  fable2::uitext::dump(base);
  __imp__UIText_FrameRender(ctx, base);
}
