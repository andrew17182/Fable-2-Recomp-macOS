// On-screen FPS counter (bottom-left corner), toggled with F9.
//
// Headline: the game's own frame rate and average frame time over the last
// ~0.5 s, e.g. "52 FPS · 19.2 ms". Guest frames come from the flight recorder's
// swap counter (fable2::fr::g_frame, counted on every guest swap). "display N"
// (the presented rate; the presenter repaints every UI tick while any overlay,
// this one included, is shown, so it tends toward the display refresh) is always
// shown. Digits are tabular, so the width changes only with the digit count.
// Extra fields: "2× res" for a higher internal resolution, "turbo 2.5x"
// while turbo is on, and the upscaling filter in use for presenting
// ("Bilinear" or "MetalFX"; from the Metal presenter's fable2_present_filter
// export, MetalFX only when it is actually applied). The text uses the game's
// UI font (src/core/fable2_turbo_notice.h) and falls back to the ImGui font.
//
// Presented frames are counted in the overlay's draw callback, which the
// presenter runs once per presented frame (Metal: PaintAndPresentImpl, just
// before presentDrawable).
//
// Off by default. Start state: [graphics] fps_counter in fable2_config.toml,
// overridden by FABLE2_FPS_COUNTER=1|0 (Dev Launcher "Show FPS counter").
// F9 toggles it live (real key, or the background test channel). The dialog
// exists only while the counter is shown. Cost while shown: a few ImGui quads
// per frame; the text is formatted into a fixed buffer twice a second.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>

#include <imgui.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "fable2_config.h"
#include "fable2_flight_recorder.h"
#include "fable2_turbo.h"
#include "fable2_turbo_notice.h"

namespace fable2::fps_counter {

class CounterDialog;
inline CounterDialog* g_dialog = nullptr;
inline std::atomic<bool> g_enabled{false};

class CounterDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit CounterDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {
    if (const char* env = std::getenv("FABLE2_RES_SCALE"); env && env[0] >= '2' && env[0] <= '9') {
      res_scale_ = env[0] - '0';
    }
    start_ = Clock::now();
    guest_start_ = fable2::fr::g_frame.load(std::memory_order_relaxed);
    std::snprintf(text_, sizeof(text_), "-- FPS");
  }

  void RequestClose() { closing_ = true; }

 protected:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kWindow = std::chrono::milliseconds(500);

  void OnDraw(ImGuiIO& io) override {
    if (closing_) {
      Close();
      return;
    }
    ++frames_;
    const auto now = Clock::now();
    if (now - start_ >= kWindow) Update(now);
    Draw(io);
  }

  void OnClose() override {
    if (g_dialog == this) g_dialog = nullptr;
  }

 private:
  void Update(Clock::time_point now) {
    const double seconds = std::chrono::duration<double>(now - start_).count();
    const uint32_t guest_now = fable2::fr::g_frame.load(std::memory_order_relaxed);
    const uint32_t guest_frames = guest_now - guest_start_;
    const double display_fps = frames_ / seconds;
    const double fps = guest_frames / seconds;
    start_ = now;
    frames_ = 0;
    guest_start_ = guest_now;

    if (!separator_) ChooseSymbols();
    const char* sep = separator_;
    int n = guest_frames ? std::snprintf(text_, sizeof(text_), "%.0f FPS%s%.1f ms", fps, sep,
                                         seconds * 1000.0 / guest_frames)
                         : std::snprintf(text_, sizeof(text_), "0 FPS");
    auto append = [&](const char* fmt, auto... args) {
      if (n > 0 && size_t(n) < sizeof(text_)) n += std::snprintf(text_ + n, sizeof(text_) - n, fmt, args...);
    };
    // Presented rate always shown, so the overlay keeps its layout.
    append("%sdisplay %.0f", sep, display_fps);
    if (res_scale_ > 1) append("%s%d%s res", sep, res_scale_, times_);
    if (fable2::turbo::Active()) append("%sturbo %gx", sep, fable2::turbo::ConfiguredScale());
    switch (PresentFilter()) {
      case 1: append("%sBilinear", sep); break;
      case 2: append("%sMetalFX", sep); break;
      default: break;
    }
  }

  // The Metal presenter's export (backends/metal/port/rex/ui/metal/metal_presenter.mm):
  // 0 = unknown, 1 = bilinear, 2 = MetalFX. Looked up until found.
  static int PresentFilter() {
    using Fn = int (*)();
    static Fn fn = nullptr;
    if (!fn) fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "fable2_present_filter"));
    return fn ? fn() : 0;
  }

  void ChooseSymbols() {
    namespace tn = fable2::turbo_notice;
    tn::LoadGameFont();
    const bool game_font = tn::Font().ok;
    // The game font has Latin-1; fall back to ASCII if a glyph is missing.
    const bool dot = !game_font || tn::Font().font.Find(0xB7);
    const bool cross = !game_font || tn::Font().font.Find(0xD7);
    separator_ = dot ? " \xC2\xB7 " : "  |  ";
    times_ = cross ? "\xC3\x97" : "x";
  }

  void Draw(ImGuiIO& io) {
    namespace tn = fable2::turbo_notice;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    tn::LoadGameFont();
    const bool game_font = tn::Font().ok;
    // About two thirds of the game's 32 px UI text at 720p, scaled with the window.
    const float text_h = std::max(12.0f, io.DisplaySize.y * (21.0f / 720.0f));
    float text_w, cell_h, scale = 1.0f;
    if (game_font) {
      scale = text_h / float(tn::Font().font.pixel_size);
      text_w = tn::GameTextWidth(text_, scale, /*tabular=*/true);
      cell_h = tn::Font().font.pixel_size * scale;
    } else {
      const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(text_h, 1e9f, 0.0f, text_);
      text_w = sz.x;
      cell_h = sz.y;
    }
    // Bottom-left, clear of the game HUD (health and tutorial text are top-left,
    // prompts are centred and right) and of the turbo notice (top-right).
    const float margin = io.DisplaySize.y * 0.03f;
    const ImVec2 pos(margin, io.DisplaySize.y - margin - cell_h);
    const float pad = text_h * 0.35f;
    dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.5f),
                      ImVec2(pos.x + text_w + pad, pos.y + cell_h + pad * 0.5f),
                      tn::Rgba(8, 6, 4, 0.45f), pad);
    const ImU32 shadow = tn::Rgba(0, 0, 0, 0.6f);
    const ImU32 cream = tn::Rgba(240, 230, 204, 1.0f);
    const float o = std::max(1.0f, text_h * 0.06f);
    if (game_font) {
      tn::DrawGameText(dl, ImVec2(pos.x + o, pos.y + o), text_, scale, shadow, /*tabular=*/true);
      tn::DrawGameText(dl, pos, text_, scale, cream, /*tabular=*/true);
    } else {
      dl->AddText(ImGui::GetFont(), text_h, ImVec2(pos.x + o, pos.y + o), shadow, text_);
      dl->AddText(ImGui::GetFont(), text_h, pos, cream, text_);
    }
  }

  char text_[128] = {};
  const char* separator_ = nullptr;
  const char* times_ = "x";
  int res_scale_ = 1;
  uint32_t frames_ = 0;
  uint32_t guest_start_ = 0;
  Clock::time_point start_{};
  bool closing_ = false;
};

// UI thread only (dialogs are created and closed there).
inline void SetVisible(bool on) {
  g_enabled.store(on, std::memory_order_relaxed);
  if (on) {
    if (g_dialog) return;
    auto* rt = rex::Runtime::instance();
    auto* drawer = rt ? rt->imgui_drawer() : nullptr;
    if (!drawer) {
      REXSYS_WARN("[fps-counter] no ImGui drawer; counter unavailable");
      return;
    }
    g_dialog = new CounterDialog(drawer);  // the dialog retains itself
  } else if (g_dialog) {
    g_dialog->RequestClose();  // closes from its own draw callback
    g_dialog = nullptr;
  }
}

inline void Toggle() {
  const bool on = !g_enabled.load(std::memory_order_relaxed);
  SetVisible(on);
  REXSYS_INFO("[fps-counter] {} (F9)", on ? "shown" : "hidden");
}

// Start state from the config and FABLE2_FPS_COUNTER; logs it once.
inline bool ConfiguredOn(const fable2::config::Values& cfg) {
  bool on = cfg.fps_counter;
  const char* source = "fable2_config.toml";
  if (const char* env = std::getenv("FABLE2_FPS_COUNTER"); env && *env) {
    if (std::strcmp(env, "1") == 0 || std::strcmp(env, "0") == 0) {
      on = env[0] == '1';
      source = "FABLE2_FPS_COUNTER";
    } else {
      REXSYS_WARN("[fps-counter] ignoring FABLE2_FPS_COUNTER='{}' (expected 1 or 0)", env);
    }
  }
  REXSYS_INFO("[fps-counter] {} at start (source: {}); F9 toggles", on ? "shown" : "hidden", source);
  return on;
}

}  // namespace fable2::fps_counter
