#pragma once
// Safe probe: dump the CURRENT TEXT object and the default font, using CORRECT
// guest offsets. Hook the per-frame UI pass (weak UIText_FrameRender) and call the
// original body (__imp__UIText_FrameRender) so rendering is unaffected.
//
// Font manager base = 0x83330000 (lis r11,-31949).
//   text-object pointer slot = 0x83330000 + 19756 (0x4D2C) = 0x83334D2C
//   font[2] (GUI_Normal)     = 0x83330000 + 18952 (0x4A08) = 0x83334A08
//   UI text list             = 0x83330000 + 19104 (0x4AA0) = 0x83334AA0
//
// Env: FABLE2_TEXTOBJ_DUMP=1
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {

static FILE* g_td_log = nullptr;
static double g_td_t0 = -1.0;
static double g_td_last = -100.0;
static int g_td_follows = 0;

static double td_now() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count() / 1000000.0;
}

static uint32_t td_rd(uint32_t a) {
  uint32_t v = 0;
  __try {
    v = __builtin_bswap32(*(volatile uint32_t*)(0x100000000ull + a));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0xCCCCCCCC;
  }
  return v;
}

static void td_dump_block(uint32_t a, int nwords, const char* label) {
  if (!g_td_log) return;
  char line[4096];
  int s = 0;
  s += std::snprintf(line + s, sizeof(line) - s, "  %s @0x%08X:", label, a);
  for (int k = 0; k < nwords; ++k)
    s += std::snprintf(line + s, sizeof(line) - s, " %08X", td_rd(a + k * 4));
  line[s] = 0;
  std::fputs(line, g_td_log);
  std::fputc('\n', g_td_log);
  std::fflush(g_td_log);
}

static void td_follow(uint32_t p, const char* label) {
  if (!g_td_log) return;
  if (g_td_follows > 60) return;
  if (p < 0x00200000 || p >= 0x80000000) {
    std::fprintf(g_td_log, "  %s -> 0x%08X (not heap)\n", label, p);
    std::fflush(g_td_log);
    return;
  }
  ++g_td_follows;
  uint8_t* host = (uint8_t*)(0x100000000ull + p);
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(host, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
    std::fprintf(g_td_log, "  %s -> 0x%08X (not committed)\n", label, p);
    std::fflush(g_td_log);
    return;
  }
  DWORD pr = mbi.Protect & 0xFF;
  if (pr != PAGE_READWRITE && pr != PAGE_WRITECOPY && pr != PAGE_READONLY &&
      pr != PAGE_EXECUTE_READ && pr != PAGE_EXECUTE_READWRITE) {
    std::fprintf(g_td_log, "  %s -> 0x%08X (protect=%d)\n", label, p, pr);
    std::fflush(g_td_log);
    return;
  }
#endif
  uint8_t buf[64];
  for (int i = 0; i < 64; ++i) buf[i] = host[i];
  char asc[80] = {0}, u16[80] = {0};
  int an = 0, un = 0;
  for (int i = 0; i < 60; ++i) asc[an++] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? buf[i] : '.';
  for (int i = 0; i < 58; i += 2) {
    uint16_t c = (uint16_t)((buf[i] << 8) | buf[i + 1]);  // UTF-16BE
    u16[un++] = (c >= 0x20 && c < 0x7F) ? c : '.';
  }
  std::fprintf(g_td_log, "  %s -> 0x%08X asc=\"%s\" u16=\"%s\"\n", label, p, asc, u16);
  std::fflush(g_td_log);
}

static void textobj_dump(PPCContext& ctx) {
  double now = td_now();
  if (g_td_t0 < 0) g_td_t0 = now;
  double el = now - g_td_t0;
  if (el < 2.0 || el > 32.0) return;
  if (now - g_td_last < 0.4) return;  // ~2.5x/sec
  g_td_last = now;
  if (!g_td_log) return;
  g_td_follows = 0;
  std::fprintf(g_td_log, "\n=== dump el=%.2f ===\n", el);
  // Pointer slots in the font manager.
  std::fprintf(g_td_log, "  slot text_obj_ptr @0x83334D2C = 0x%08X\n", td_rd(0x83334D2C));
  std::fprintf(g_td_log, "  slot font[2] @0x83334A08 = 0x%08X\n", td_rd(0x83334A08));
  std::fflush(g_td_log);
  // Follow the text-object pointer and dump the object + its heap pointers.
  uint32_t tobj = td_rd(0x83334D2C);
  if (tobj >= 0x00200000 && tobj < 0x80000000) {
    td_dump_block(tobj, 40, "textobj");
    for (int off = 0; off < 160 && g_td_follows < 60; off += 4) {
      uint32_t v = td_rd(tobj + off);
      if (v >= 0x00200000 && v < 0x80000000) {
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "T+%d", off);
        td_follow(v, lbl);
      }
    }
  }
  // Follow the default font.
  uint32_t f2 = td_rd(0x83334A08);
  if (f2 >= 0x00200000 && f2 < 0x80000000) {
    td_dump_block(f2, 24, "font2");
    for (int off = 0; off < 96 && g_td_follows < 60; off += 4) {
      uint32_t v = td_rd(f2 + off);
      if (v >= 0x00200000 && v < 0x80000000) {
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "F+%d", off);
        td_follow(v, lbl);
      }
    }
  }
  std::fflush(g_td_log);
}

static void textobj_hook(PPCContext& ctx, uint8_t* base) {
  static int state = -1;
  if (state < 0) {
    const char* e = std::getenv("FABLE2_TEXTOBJ_DUMP");
    state = (e && e[0] == '1') ? 1 : 0;
    if (state) {
      g_td_log = std::fopen("fable2_textobj_dump.log", "w");
      if (g_td_log) std::fflush(g_td_log);
    }
  }
  if (!state) return;
  textobj_dump(ctx);
  (void)base;
}

// Original recompiled body (strong).
extern "C" void __imp__UIText_FrameRender(PPCContext& ctx, uint8_t* base);

// Strong override of the weak wrapper.
void UIText_FrameRender(PPCContext& ctx, uint8_t* base) {
  textobj_hook(ctx, base);
  __imp__UIText_FrameRender(ctx, base);
}

}  // extern "C"
