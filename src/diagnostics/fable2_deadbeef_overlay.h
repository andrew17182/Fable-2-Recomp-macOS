// fable2_deadbeef_overlay.h - host-side ImGui overlay that draws "DEADBEEF".
//
// This is a SEPARATE DRAW: an ImGuiDialog registered with the SDK presenter
// that renders "DEADBEEF" as text on top of the guest frame. It proves the
// rendering-pipeline hook + extra-draw mechanism without touching the guest's
// text layout (which overflows the glyph buffer on in-place extension).
//
//   FABLE2_DEADBEEF_OVERLAY=1   enable (default: enabled)
//   FABLE2_DEADBEEF_POS=x,y     screen position (logical px; default: bottom)
//   FABLE2_DEADBEEF_SCALE=f     font scale (default 2.0)
//
// The overlay is positioned at the bottom-center of the window (where the
// "Press <a_img> to start" prompt lives). For the final A->E keycap goal this
// becomes the keyboard "E" keycap drawn in place of the A button.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace fable2::deadbeef_overlay {

inline bool enabled() {
  static const bool on = [] {
#ifdef _WIN32
    char v[8] = {};
    size_t n = 0;
    ::getenv_s(&n, v, sizeof(v), "FABLE2_DEADBEEF_OVERLAY");
    return v[0] == 0 || v[0] == '1';
#else
    const char* v = std::getenv("FABLE2_DEADBEEF_OVERLAY");
    return v == nullptr || v[0] == '1';
#endif
  }();
  return on;
}

inline float scale() {
  static const float s = [] {
#ifdef _WIN32
    char v[16] = {};
    size_t n = 0;
    ::getenv_s(&n, v, sizeof(v), "FABLE2_DEADBEEF_SCALE");
    return v[0] ? (float)std::atof(v) : 2.0f;
#else
    const char* v = std::getenv("FABLE2_DEADBEEF_SCALE");
    return v ? (float)std::atof(v) : 2.0f;
#endif
  }();
  return s;
}

// A persistent ImGui dialog that draws "DEADBEEF" as raw text (no window
// frame) at the bottom-center of the render target.
class DeadbeefDialog : public rex::ui::ImGuiDialog {
 public:
  explicit DeadbeefDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!enabled()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    const float w = io.DisplaySize.x;
    const float h = io.DisplaySize.y;
    // Bottom-center, just below the prompt line.
    const float x = w * 0.5f + 120.0f;
    const float y = h * 0.78f;
    dl->AddText(ImGui::GetFont(), 16.0f * scale(), ImVec2(x, y),
                IM_COL32(255, 255, 0, 255), "DEADBEEF");
  }
};

// Called from ReXApp::OnCreateDialogs to register the overlay.
inline void register_overlay(rex::ui::ImGuiDrawer* drawer) {
  (void)new DeadbeefDialog(drawer);  // dialog retains itself
}

}  // namespace fable2::deadbeef_overlay
