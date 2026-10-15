// fable2_hotfuncs.h - hot-call counters to find the per-glyph draw path.
//
// Hooks a set of candidate text-render functions and counts calls over time.
// A background writer flushes the counts to fable2_hotfuncs.log every 2s so
// the log is intact even if the process is SIGKILL'd.
//
//   FABLE2_HOTFUNCS=1   enable (default: off)
//
// The function that fires ~16x per visible text element per frame (for
// "Press A to start") is the per-glyph draw vcall.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/ppc/context.h>

#include "fable2_ui_render_probe.h"

namespace fable2::hotfuncs {

enum Idx {
  kUIText_FrameRender = 0,
  kUIText_FrameRenderIter,
  kUIText_RenderCurrent,
  kUIText_RenderCurrentObject,
  kUIText_RenderSegment,
  kUIFont_EmitGlyphQuad,
  kUIFont_RenderGlyph,
  kUIFont_LookupGlyph,
  kSub_82A6EF40,    // glyph-record vtable [0]
  kSub_82A8D2D0,    // glyph-record vtable [3]
  kSub_82CA38C8,    // glyph-record vtable [1] / obj vtable [19]
  kSub_82C13928,    // obj vtable [4]
  kSub_82C13A70,    // obj vtable [5]
  kSub_82C12D90,    // obj vtable [13]
  kSub_82C12F78,    // obj vtable [9]
  kSub_82C12EA0,    // obj vtable [14]
  kSub_82CDE918,    // big vertex writer
  kSub_82CD1500,    // big vertex writer
  kSub_82CC3978,    // big vertex writer
  kSub_82C12CF8,    // obj vtable [0]
  kSub_82C12D00,    // obj vtable [1]
  kSub_82C133B0,    // obj vtable [2]
  kSub_82C13018,    // obj vtable [15]
  kSub_82C14B70,    // obj vtable [17]
  kSub_82C09018,    // per-element state
  kCount
};

inline const char* name(int i) {
  static const char* n[] = {
      "UIText_FrameRender",
      "UIText_FrameRenderIter",
      "UIText_RenderCurrent",
      "UIText_RenderCurrentObject",
      "UIText_RenderSegment",
      "UIFont_EmitGlyphQuad",
      "UIFont_RenderGlyph",
      "UIFont_LookupGlyph",
      "ProcessAndProcessAndProcess563_82A6EF40 (recvt[0])",
      "sub_82A8D2D0 (recvt[3])",
      "sub_82CA38C8 (recvt[1])",
      "sub_82C13928 (objvt[4])",
      "sub_82C13A70 (objvt[5])",
      "ProcessAndProcessAndProcess1335_82C12D90 (objvt[13])",
      "ProcessAndProcessAndProcess1693_82C12F78 (objvt[9])",
      "sub_82C12EA0 (objvt[14])",
      "sub_82CDE918 (vert?)",
      "sub_82CD1500 (vert?)",
      "sub_82CC3978 (vert?)",
      "sub_82C12CF8 (objvt[0])",
      "sub_82C12D00 (objvt[1])",
      "ProcessAndProcessAndProcess2301_82C133B0 (objvt[2])",
      "sub_82C13018 (objvt[15])",
      "ProcessAndProcessAndProcess2393_82C14B70 (objvt[17])",
      "sub_82C09018 (state)",
  };
  return (i >= 0 && i < kCount) ? n[i] : "?";
}

inline std::atomic<uint64_t>& ctr(int i) {
  static std::atomic<uint64_t> arr[kCount] = {};
  return arr[i];
}

inline void tick(int i) { ctr(i).fetch_add(1, std::memory_order_relaxed); }

inline bool enabled() {
  static const bool on = [] {
#ifdef _WIN32
    char v[8] = {};
    size_t sz = 0;
    return ::getenv_s(&sz, v, sizeof(v), "FABLE2_HOTFUNCS") == 0 && v[0] == '1';
#else
    const char* v = std::getenv("FABLE2_HOTFUNCS");
    return v != nullptr && v[0] == '1';
#endif
  }();
  return on;
}

inline void start_writer() {
  static std::atomic<bool> started{false};
  bool expected = false;
  if (!started.compare_exchange_strong(expected, true)) return;
  std::thread([] {
    FILE* f = std::fopen("fable2_hotfuncs.log", "w");
    if (!f) return;
    std::setvbuf(f, nullptr, _IONBF, 0);
    // Write for up to 180s (2s cadence). The process is usually killed by
    // then; the unbuffered log survives SIGKILL.
    for (int s = 0; s < 90; ++s) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      std::fprintf(f, "=== t=%ds ===\n", s * 2);
      for (int i = 0; i < kCount; ++i)
        std::fprintf(f, "%-26s %llu\n", name(i),
                     (unsigned long long)ctr(i).load(std::memory_order_relaxed));
    }
    std::fclose(f);
  }).detach();
}

}  // namespace fable2::hotfuncs

#define FABLE2_HOT_HOOK(SYM, IDX)                                              \
  extern "C" void SYM(PPCContext& ctx, uint8_t* base) {                        \
    if (fable2::hotfuncs::enabled()) {                                         \
      fable2::hotfuncs::tick(fable2::hotfuncs::IDX);                           \
      fable2::hotfuncs::start_writer();                                        \
    }                                                                          \
    if (fable2::uir::hook(#SYM, ctx, base)) return;                             \
    __imp__##SYM(ctx, base);                                                    \
  }

// Note: UIText_FrameRender is owned by fable2_heap_scan.h; not hooked here.
FABLE2_HOT_HOOK(UIText_FrameRenderIter, kUIText_FrameRenderIter)
FABLE2_HOT_HOOK(UIText_RenderCurrent, kUIText_RenderCurrent)
FABLE2_HOT_HOOK(UIText_RenderCurrentObject, kUIText_RenderCurrentObject)
// UIText_RenderSegment / UIFont_EmitGlyphQuad / UIFont_LookupGlyph are
// owned by fable2_text_append.h (probe + append).
FABLE2_HOT_HOOK(UIFont_RenderGlyph, kUIFont_RenderGlyph)
FABLE2_HOT_HOOK(ProcessAndProcessAndProcess563_82A6EF40, kSub_82A6EF40)
FABLE2_HOT_HOOK(sub_82A8D2D0, kSub_82A8D2D0)
FABLE2_HOT_HOOK(sub_82CA38C8, kSub_82CA38C8)
FABLE2_HOT_HOOK(ProcessAndProcessAndProcess2206_82C13928, kSub_82C13928)
FABLE2_HOT_HOOK(sub_82C13A70, kSub_82C13A70)
FABLE2_HOT_HOOK(ProcessAndProcessAndProcess1335_82C12D90, kSub_82C12D90)
FABLE2_HOT_HOOK(ProcessAndProcessAndProcess1693_82C12F78, kSub_82C12F78)
FABLE2_HOT_HOOK(ProcessAndProcessAndProcess974_82C12EA0, kSub_82C12EA0)
FABLE2_HOT_HOOK(sub_82CDE918, kSub_82CDE918)
FABLE2_HOT_HOOK(sub_82CD1500, kSub_82CD1500)
FABLE2_HOT_HOOK(sub_82CC3978, kSub_82CC3978)
FABLE2_HOT_HOOK(sub_82C12CF8, kSub_82C12CF8)
FABLE2_HOT_HOOK(sub_82C12D00, kSub_82C12D00)
FABLE2_HOT_HOOK(ProcessAndProcessAndProcess2301_82C133B0, kSub_82C133B0)
FABLE2_HOT_HOOK(sub_82C13018, kSub_82C13018)
FABLE2_HOT_HOOK(ProcessAndProcessAndProcess2393_82C14B70, kSub_82C14B70)
// sub_82C09018 is owned by fable2_text_append.h (probe + append).
