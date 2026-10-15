// fable2_ui_render_probe.h - UI-render identification probe (timeline + ablation).
//
// Goal: find which guest function(s) do the ACTUAL on-screen rendering of the
// UI text ("Press <a_img> to start" on the title screen, blinking ~35-40 s)
// so the rendering can be recreated in C++ with total control.
//
// Two mechanisms, both active only inside a time window measured from the
// FIRST hook call (guest start):
//
//   1. TIMELINE: one "t=<ms> <name>" line per call of every hooked UI-text
//      pipeline function, written to fable2_ui_render_probe.log. The blink
//      state is visible directly: when the prompt is off, the per-element
//      draw calls drop out of the per-frame block.
//
//   2. ABLATION (suppression): functions named in FABLE2_UIR_SUPPRESS have
//      their ORIGINAL BODY SKIPPED while the window is open. If the prompt
//      text disappears from the screen (verified by screenshot), that
//      function is on the actual render path; the lowest function in the
//      chain that still makes it disappear is the true render.
//
// Env vars:
//   FABLE2_UIR=1                     master enable (default off)
//   FABLE2_UIR_DELAY=<sec>           window start (default 34, from first hook call)
//   FABLE2_UIR_DUR=<sec>             window length (default 8)
//   FABLE2_UIR_SUPPRESS=a,b,c        comma list of symbol names to suppress
//                                    (skip original body) during the window.
//                                    Each name may carry a time slice
//                                    "name@start-end" (seconds from the first
//                                    hook call) so several functions can be
//                                    ablated in sequence within one run, e.g.
//                                    UIText_FrameRender@34-39,ProcessAndProcessAndProcess1637_82B4EEE0@39-44
//   FABLE2_UIR_SKIP=a,b              comma list of names excluded from the
//                                    timeline log (high-frequency noise)
//
// Log: fable2_ui_render_probe.log in the CWD (exe dir). The header line
// "t0=<unix_ms>" gives the absolute time of the first hook call, so the
// timeline can be aligned with screenshot file names (ms since process
// start; the launch script writes its own t0 marker next to the captures).
//
// This header also owns strong overrides for pipeline functions no other
// probe owns: sub_82BFD850, UITextItem_Render (0x82BFD9E8),
// UIText_RenderWithFont (0x82C000F8), ProcessAndProcessAndProcess2382_82C09B50, ProcessAndProcessAndProcess2069_82C0A230,
// ProcessAndProcessAndProcess1637_82B4EEE0 (element draw vtable [13]).
//
// Wiring: each existing probe hook calls fable2::uir::hook("Name", ctx,
// base) just before its __imp__ forward; hook() returns true when the
// original must be skipped. It also drives the input probe (see
// fable2_ui_input_probe.h, FABLE2_UIR_IN=1) which captures register inputs
// and hunts the prompt string. When neither probe is enabled, hook() is a
// single static-bool check.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

// Runtime toggles for the guest function-call tracer (fable2_func_trace.h):
// used to auto-trace the whole guest call tree inside the probe window
// (FABLE2_UIR_FUNC_TRACE=1) without pre-enabling FABLE2_FUNC_TRACE.
#include "fable2_func_trace.h"
#include "fable2_ui_input_probe.h"

namespace fable2::uir {

// Auto-trace the full guest call tree (RLE log fable2_func_trace.log) for
// the probe window only.
inline bool func_trace_auto() {
#ifdef _WIN32
  static const bool on = [] {
    char v[8] = {};
    size_t n = 0;
    return ::getenv_s(&n, v, sizeof(v), "FABLE2_UIR_FUNC_TRACE") == 0 && v[0] == '1';
  }();
  return on;
#else
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_UIR_FUNC_TRACE");
    return v != nullptr && v[0] == '1';
  }();
  return on;
#endif
}

inline bool active() {
#ifdef _WIN32
  static const bool on = [] {
    char v[8] = {};
    size_t n = 0;
    return ::getenv_s(&n, v, sizeof(v), "FABLE2_UIR") == 0 && v[0] == '1';
  }();
  return on;
#else
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_UIR");
    return v != nullptr && v[0] == '1';
  }();
  return on;
#endif
}

inline double window_delay_seconds() {
  const char* v = std::getenv("FABLE2_UIR_DELAY");
  return v ? std::atof(v) : 34.0;
}
inline double window_duration_seconds() {
  const char* v = std::getenv("FABLE2_UIR_DUR");
  return v ? std::atof(v) : 8.0;
}

// Parses "a,b,c" into a vector of trimmed tokens.
inline std::vector<std::string> parse_list(const char* env) {
  std::vector<std::string> out;
  const char* v = std::getenv(env);
  if (!v) return out;
  std::string s(v);
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == ',') {
      std::string tok = s.substr(start, i - start);
      while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
        tok.erase(tok.begin());
      while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
        tok.pop_back();
      if (!tok.empty()) out.push_back(tok);
      start = i + 1;
    }
  }
  return out;
}

struct SuppressRule {
  std::string name;
  double start_s = 0.0;  // from first hook call
  double end_s = 0.0;    // 0 = for the whole probe window
};

// "name" or "name@start-end" (seconds from the first hook call).
inline SuppressRule parse_rule(const std::string& tok) {
  SuppressRule r;
  auto at = tok.find('@');
  if (at == std::string::npos) {
    r.name = tok;
  } else {
    r.name = tok.substr(0, at);
    std::string span = tok.substr(at + 1);
    auto dash = span.find('-');
    if (dash != std::string::npos) {
      r.start_s = std::atof(span.substr(0, dash).c_str());
      r.end_s = std::atof(span.substr(dash + 1).c_str());
    }
  }
  return r;
}

inline FILE* logf() {
  static FILE* f = [] {
#ifdef _WIN32
    FILE* out = nullptr;
    if (::fopen_s(&out, "fable2_ui_render_probe.log", "w") != 0) out = nullptr;
    return out;
#else
    return std::fopen("fable2_ui_render_probe.log", "w");
#endif
  }();
  return f;
}

struct State {
  int64_t t0_us = 0;          // first hook call (steady clock)
  int64_t start_us = 0;       // t0 + delay
  int64_t end_us = 0;         // start + dur
  bool window_logged_open = false;
  bool func_trace_on = false;
  std::vector<SuppressRule> suppress;
  std::vector<std::string> skip;
};

inline State& st() {
  static State* s = new State;  // leaked on purpose (steady address)
  return *s;
}

inline int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Initializes the state machine on the very first hook call.
inline void init_once() {
  static std::atomic<bool> done{false};
  bool expected = false;
  if (!done.compare_exchange_strong(expected, true)) return;
  State& s = st();
  s.t0_us = now_us();
  s.start_us = s.t0_us + (int64_t)(window_delay_seconds() * 1e6);
  s.end_us = s.start_us + (int64_t)(window_duration_seconds() * 1e6);
  for (const std::string& tok : parse_list("FABLE2_UIR_SUPPRESS"))
    s.suppress.push_back(parse_rule(tok));
  s.skip = parse_list("FABLE2_UIR_SKIP");
  if (FILE* f = logf()) {
    for (const SuppressRule& x : s.suppress)
      std::fprintf(f, "# SUPPRESS %s (%.1f-%.1fs)\n", x.name.c_str(), x.start_s,
                   x.end_s);
    const int64_t epoch_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    std::fprintf(f, "# fable2_ui_render_probe\n");
    std::fprintf(f, "# t0=%lld (unix ms of first hook call)\n",
                 (long long)epoch_ms);
    std::fprintf(f, "# window = [%.1fs, %.1fs] from first hook call\n",
                 window_delay_seconds(), window_delay_seconds() + window_duration_seconds());
    for (const std::string& x : s.skip) std::fprintf(f, "# SKIP %s\n", x.c_str());
    std::fflush(f);
  }
}

inline bool in_window(int64_t now) {
  State& s = st();
  return now >= s.start_us && now < s.end_us;
}

// Called by every hooked pipeline function just before forwarding to the
// __imp__ body. Returns TRUE when the original body must be SKIPPED
// (suppression active for this name). Also appends the timeline entry and
// feeds the input probe.
inline bool hook(const char* name, PPCContext& ctx, uint8_t* base) {
  fable2::uip::scan(name, ctx, base);  // no-op unless FABLE2_UIR_IN=1
  if (!active()) return false;
  if (!active()) return false;
  init_once();
  const int64_t now = now_us();
  if (now < st().start_us) return false;
  if (now >= st().end_us) {
    State& s2 = st();
    if (s2.window_logged_open && s2.func_trace_on) {
      s2.func_trace_on = false;
      Fable2FuncTraceFlush();
      Fable2FuncTraceSetEnabled(false);
    }
    return false;  // window closed
  }
  State& s = st();
  if (!s.window_logged_open) {
    s.window_logged_open = true;
    if (func_trace_auto()) {
      Fable2FuncTraceSetEnabled(true);
      s.func_trace_on = true;
    }
    if (FILE* f = logf()) {
      std::fprintf(f, "=== window open%s ===\n", s.func_trace_on ? " (+func trace)" : "");
      std::fflush(f);
    }
  }
  const double rel_s = (now - s.t0_us) / 1e6;
  bool suppressed = false;
  for (const SuppressRule& x : s.suppress) {
    if (x.name != name) continue;
    if (x.end_s > 0.0 && (rel_s < x.start_s || rel_s >= x.end_s)) continue;
    suppressed = true;
    break;
  }
  for (const std::string& x : s.skip)
    if (x == name) return suppressed;  // not logged, but suppression still applies
  if (FILE* f = logf()) {
    char line[160];
    std::snprintf(line, sizeof(line), "t=%lld %s%s\n", (long long)((now - s.t0_us) / 1000),
                  suppressed ? "[SUPPRESS] " : "", name);
    std::fwrite(line, 1, std::strlen(line), f);
  }
  return suppressed;
}

}  // namespace fable2::uir

// Strong overrides for pipeline functions not owned by any other probe.
// (Pattern: weak sub_XXX forwards to __imp__sub_XXX; we replace the weak
// wrapper with a strong one that does probe work then calls the original.)
extern "C" void __imp__ThunkTo_UIText_FrameRender_82BFD850(PPCContext& ctx, uint8_t* base);
extern "C" void ThunkTo_UIText_FrameRender_82BFD850(PPCContext& ctx, uint8_t* base) {
  if (fable2::uir::hook("ThunkTo_UIText_FrameRender_82BFD850", ctx, base)) return;
  __imp__ThunkTo_UIText_FrameRender_82BFD850(ctx, base);
}

extern "C" void __imp__UITextItem_Render(PPCContext& ctx, uint8_t* base);
extern "C" void UITextItem_Render(PPCContext& ctx, uint8_t* base) {
  if (fable2::uir::hook("UITextItem_Render", ctx, base)) return;
  __imp__UITextItem_Render(ctx, base);
}

extern "C" void __imp__UIText_RenderWithFont(PPCContext& ctx, uint8_t* base);
extern "C" void UIText_RenderWithFont(PPCContext& ctx, uint8_t* base) {
  if (fable2::uir::hook("UIText_RenderWithFont", ctx, base)) return;
  __imp__UIText_RenderWithFont(ctx, base);
}

extern "C" void __imp__ProcessAndProcessAndProcess2382_82C09B50(PPCContext& ctx, uint8_t* base);
extern "C" void ProcessAndProcessAndProcess2382_82C09B50(PPCContext& ctx, uint8_t* base) {
  if (fable2::uir::hook("ProcessAndProcessAndProcess2382_82C09B50", ctx, base)) return;
  __imp__ProcessAndProcessAndProcess2382_82C09B50(ctx, base);
}

extern "C" void __imp__ProcessAndProcessAndProcess2069_82C0A230(PPCContext& ctx, uint8_t* base);
extern "C" void ProcessAndProcessAndProcess2069_82C0A230(PPCContext& ctx, uint8_t* base) {
  if (fable2::uir::hook("ProcessAndProcessAndProcess2069_82C0A230", ctx, base)) return;
  __imp__ProcessAndProcessAndProcess2069_82C0A230(ctx, base);
}

// The per-element DRAW (element vtable [13], invoked via sub_82C09870).
extern "C" void __imp__ProcessAndProcessAndProcess1637_82B4EEE0(PPCContext& ctx, uint8_t* base);
extern "C" void ProcessAndProcessAndProcess1637_82B4EEE0(PPCContext& ctx, uint8_t* base) {
  if (fable2::uir::hook("ProcessAndProcessAndProcess1637_82B4EEE0", ctx, base)) return;
  __imp__ProcessAndProcessAndProcess1637_82B4EEE0(ctx, base);
}
