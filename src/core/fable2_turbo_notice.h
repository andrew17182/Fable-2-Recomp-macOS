// "Turbo 2x on/off" notice, shown on each turbo toggle (docs/turbo.md).
//
// Shown by fable2::turbo::SetActive through turbo::g_notice. Toggles come from
// key events on the UI thread (real 7/F8 and the background test channel), which
// is where ImGui dialogs must be created. The dialog exists only while the
// notice is visible (or an in-game request is pending), so the overlay stack is
// idle otherwise.
//
// FABLE2_TURBO_NOTICE selects the style (Dev Launcher "Turbo notice"):
//   overlay (default)  Fable-style ImGui overlay: the game's own UI font
//                      (data/language/en-uk/fonts/gui_large.bft, decoded at
//                      runtime by src/core/fable2_bft_font.h), cream text with a
//                      soft shadow on a feathered dark band, thin gold rule,
//                      ~200 ms fades. Falls back to the ImGui font if the .bft
//                      cannot be loaded.
//   game (experimental) the game's own info box via src/core/fable2_game_message.h;
//                      falls back to the overlay when the game declines (not in
//                      normal gameplay) or does not answer.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <imgui.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "fable2_bft_font.h"
#include "fable2_game_message.h"
#include "fable2_turbo.h"

namespace fable2::turbo_notice {

enum class Mode { kOverlay, kGame };

inline Mode ConfiguredMode() {
  static const Mode mode = [] {
    const char* env = std::getenv("FABLE2_TURBO_NOTICE");
    Mode m = Mode::kOverlay;
    if (env && *env) {
      if (std::strcmp(env, "game") == 0) m = Mode::kGame;
      else if (std::strcmp(env, "overlay") != 0)
        REXSYS_WARN("[turbo-notice] ignoring FABLE2_TURBO_NOTICE='{}' (overlay|game)", env);
    }
    REXSYS_INFO("[turbo-notice] style: {}", m == Mode::kGame ? "in-game message (experimental)"
                                                            : "overlay (Fable style)");
    return m;
  }();
  return mode;
}

// Set by Install(): where the game data lives and how to create textures.
inline std::filesystem::path g_game_data_root;
inline std::function<rex::ui::ImmediateDrawer*()> g_get_immediate_drawer;

// The game UI font, loaded on first use (UI thread).
struct GameFont {
  bool tried = false;
  bool ok = false;
  fable2::bftfont::Font font;
  std::unique_ptr<rex::ui::ImmediateTexture> texture;
};
inline GameFont& Font() {
  static GameFont f;
  return f;
}

inline void LoadGameFont() {
  auto& gf = Font();
  if (gf.tried) return;
  gf.tried = true;
  auto* drawer = g_get_immediate_drawer ? g_get_immediate_drawer() : nullptr;
  if (!drawer || g_game_data_root.empty()) {
    REXSYS_WARN("[turbo-notice] no immediate drawer / data root; using the ImGui font");
    return;
  }
  const auto path = g_game_data_root / "data/language/en-uk/fonts/gui_large.bft";
  std::string error;
  if (!fable2::bftfont::Load(path, gf.font, error)) {
    REXSYS_WARN("[turbo-notice] game font {} unusable ({}); using the ImGui font", path.string(),
                error);
    return;
  }
  gf.texture = drawer->CreateTexture(gf.font.tex_width, gf.font.tex_height,
                                     rex::ui::ImmediateTextureFilter::kLinear, false,
                                     gf.font.rgba.data());
  gf.font.rgba.clear();
  gf.font.rgba.shrink_to_fit();
  gf.ok = gf.texture != nullptr;
  REXSYS_INFO("[turbo-notice] game font {}: {} px, {} glyphs, {}x{} atlas", gf.ok ? "loaded" : "texture failed",
              gf.font.pixel_size, gf.font.glyphs.size(), gf.font.tex_width, gf.font.tex_height);
}

// Widest advance among the digits 0-9 (unscaled), for tabular digits.
inline float DigitAdvance() {
  const auto& f = Font().font;
  float w = 0;
  for (uint32_t c = '0'; c <= '9'; ++c)
    if (const auto* g = f.Find(c)) w = std::max(w, float(g->advance));
  return w;
}

// Measures / draws `text` with the game font at `scale` (1 = native 32 px).
// `tabular`: every digit takes the widest digit's advance (centred in it), so
// changing numbers keep the text width; only a change in digit count moves it.
inline float GameTextWidth(const char* text, float scale, bool tabular = false) {
  const auto& f = Font().font;
  const float digit = tabular ? DigitAdvance() : 0.0f;
  float w = 0;
  for (const char* s = text; *s;) {
    const uint32_t c = fable2::bftfont::NextCodepoint(s);
    if (tabular && c >= '0' && c <= '9') w += digit * scale;
    else if (const auto* g = f.Find(c)) w += g->advance * scale;
  }
  return w;
}

inline void DrawGameText(ImDrawList* dl, ImVec2 pos, const char* text, float scale, ImU32 color,
                         bool tabular = false) {
  auto& gf = Font();
  const ImTextureID tex = reinterpret_cast<ImTextureID>(gf.texture.get());
  const float cell = float(gf.font.pixel_size);
  const float digit = tabular ? DigitAdvance() : 0.0f;
  float x = pos.x;
  for (const char* s = text; *s;) {
    const uint32_t c = fable2::bftfont::NextCodepoint(s);
    const auto* g = gf.font.Find(c);
    if (!g) continue;
    const bool fixed = tabular && c >= '0' && c <= '9';
    const float shift = fixed ? (digit - g->advance) * 0.5f * scale : 0.0f;
    if (g->width > 0) {
      const ImVec2 a(x + shift + g->x_offset * scale, pos.y);
      const ImVec2 b(a.x + (g->u1 - g->u0) * gf.font.tex_width * scale, pos.y + cell * scale);
      dl->AddImage(tex, a, b, ImVec2(g->u0, g->v0), ImVec2(g->u1, g->v1), color);
    }
    x += (fixed ? digit : g->advance) * scale;
  }
}

inline ImU32 Rgba(int r, int g, int b, float a) {
  return IM_COL32(r, g, b, int(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}

class NoticeDialog;
inline NoticeDialog* g_dialog = nullptr;

class NoticeDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit NoticeDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

  void ShowOverlay(std::string text) {
    const auto now = Clock::now();
    // Re-trigger while visible: keep it fully shown instead of re-fading in.
    const bool visible = overlay_ && now < start_ + kTotal;
    text_ = std::move(text);
    overlay_ = true;
    game_pending_ = false;
    start_ = visible ? now - kFade : now;
  }

  void ShowGame(std::string text, const fable2::gamemsg::Ticket& ticket) {
    text_ = std::move(text);
    ticket_ = ticket;
    game_pending_ = true;
    overlay_ = false;
  }

 protected:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kFade = std::chrono::milliseconds(200);
  static constexpr auto kTotal = std::chrono::milliseconds(2000);

  void OnDraw(ImGuiIO& io) override {
    if (game_pending_) {
      const auto status = fable2::gamemsg::Poll(ticket_);
      if (status == fable2::gamemsg::Status::kPending) return;
      game_pending_ = false;
      REXSYS_INFO("[turbo-notice] in-game message '{}': {}{}", text_,
                  fable2::gamemsg::StatusName(status),
                  status == fable2::gamemsg::Status::kShown ? "" : " -> overlay");
      if (status == fable2::gamemsg::Status::kShown) {
        Close();
        return;
      }
      ShowOverlay(text_);
    }
    const auto now = Clock::now();
    if (!overlay_ || now >= start_ + kTotal) {
      Close();
      return;
    }
    const float t = std::chrono::duration<float>(now - start_).count();
    const float fade = std::chrono::duration<float>(kFade).count();
    const float total = std::chrono::duration<float>(kTotal).count();
    float alpha = std::min(1.0f, std::min(t / fade, (total - t) / fade));
    alpha = alpha * alpha * (3.0f - 2.0f * alpha);  // smoothstep
    DrawFableStyle(io, alpha);
  }

  void DrawFableStyle(ImGuiIO& io, float alpha) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl || alpha <= 0.0f) return;
    LoadGameFont();
    const bool game_font = Font().ok;
    // Glyph cell at the game's own size: gui_large (32 px) on its 720p frame,
    // scaled with the window, so it matches the game's text.
    const float text_h = std::max(18.0f, io.DisplaySize.y * (32.0f / 720.0f));
    float text_w, cell_h;
    float scale = 1.0f;
    if (game_font) {
      scale = text_h / float(Font().font.pixel_size);
      text_w = GameTextWidth(text_.c_str(), scale);
      cell_h = Font().font.pixel_size * scale;
    } else {
      const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(text_h, 1e9f, 0.0f, text_.c_str());
      text_w = sz.x;
      cell_h = sz.y;
    }
    const float margin_x = io.DisplaySize.x * 0.045f;
    // Below the game's own top banner (tutorial/info text uses the top ~12%).
    const float top = io.DisplaySize.y * 0.16f;
    const ImVec2 pos(io.DisplaySize.x - margin_x - text_w, top);

    // Soft band: horizontal feather on both ends, vertical feather via strips.
    const float pad_x = text_h * 3.0f, pad_y = text_h * 0.5f;
    const float bx0 = pos.x - pad_x, bx1 = std::min(io.DisplaySize.x, pos.x + text_w + pad_x);
    const float by0 = pos.y - pad_y, by1 = pos.y + cell_h + pad_y;
    const float feather_x = pad_x * 0.9f;
    const int strips = 16;
    for (int i = 0; i < strips; ++i) {
      const float y0 = by0 + (by1 - by0) * i / strips, y1 = by0 + (by1 - by0) * (i + 1) / strips;
      const float c = (i + 0.5f) / strips;                 // 0..1 down the band
      const float v = 1.0f - std::abs(c * 2.0f - 1.0f);    // 0 at edges, 1 centre
      const float a = 0.62f * alpha * std::min(1.0f, v * 2.2f);
      const ImU32 dark = Rgba(8, 6, 4, a), clear = Rgba(8, 6, 4, 0);
      dl->AddRectFilledMultiColor(ImVec2(bx0, y0), ImVec2(bx0 + feather_x, y1), clear, dark, dark, clear);
      dl->AddRectFilled(ImVec2(bx0 + feather_x, y0), ImVec2(bx1 - feather_x, y1), dark);
      dl->AddRectFilledMultiColor(ImVec2(bx1 - feather_x, y0), ImVec2(bx1, y1), dark, clear, clear, dark);
    }

    // Thin gold rule under the text, fading out at both ends.
    const float rule_y = pos.y + cell_h + pad_y * 0.15f;
    const float rx0 = pos.x - text_h * 1.2f, rx1 = pos.x + text_w + text_h * 1.2f;
    const float rmid = (rx0 + rx1) * 0.5f;
    const ImU32 gold = Rgba(201, 164, 92, 0.85f * alpha), gold0 = Rgba(201, 164, 92, 0);
    dl->AddRectFilledMultiColor(ImVec2(rx0, rule_y), ImVec2(rmid, rule_y + 1.5f), gold0, gold, gold, gold0);
    dl->AddRectFilledMultiColor(ImVec2(rmid, rule_y), ImVec2(rx1, rule_y + 1.5f), gold, gold0, gold0, gold);

    // Text: soft shadow (a few offset passes), then cream.
    const ImU32 shadow = Rgba(0, 0, 0, 0.35f * alpha);
    const ImU32 cream = Rgba(240, 230, 204, alpha);
    const float o = std::max(1.0f, text_h * 0.06f);
    const ImVec2 offs[] = {{o, o}, {o * 2, o * 2}, {0, o * 1.5f}, {o * 1.5f, 0}};
    const float cell_top = pos.y;
    for (const auto& off : offs) {
      if (game_font) {
        DrawGameText(dl, ImVec2(pos.x + off.x, cell_top + off.y), text_.c_str(), scale, shadow);
      } else {
        dl->AddText(ImGui::GetFont(), text_h, ImVec2(pos.x + off.x, pos.y + off.y), shadow,
                    text_.c_str());
      }
    }
    if (game_font) {
      DrawGameText(dl, ImVec2(pos.x, cell_top), text_.c_str(), scale, cream);
    } else {
      dl->AddText(ImGui::GetFont(), text_h, pos, cream, text_.c_str());
    }
  }

  void OnClose() override {
    if (g_dialog == this) g_dialog = nullptr;
  }

 private:
  std::string text_;
  bool overlay_ = false;
  bool game_pending_ = false;
  fable2::gamemsg::Ticket ticket_{};
  Clock::time_point start_{};
};

inline void Show(const std::string& text) {
  auto* rt = rex::Runtime::instance();
  auto* drawer = rt ? rt->imgui_drawer() : nullptr;
  if (!drawer) return;
  if (!g_dialog) g_dialog = new NoticeDialog(drawer);  // dialog retains itself
  if (ConfiguredMode() == Mode::kGame) {
    fable2::gamemsg::Ticket ticket;
    if (fable2::gamemsg::Request(text, 2.0, ticket)) {
      g_dialog->ShowGame(text, ticket);
      return;
    }
    REXSYS_INFO("[turbo-notice] in-game message unavailable (script runner not ready) -> overlay");
  }
  g_dialog->ShowOverlay(text);
}

// `get_immediate_drawer` returns the app's ImmediateDrawer (texture creation for
// the game font); `game_data_root` is the folder that contains data/.
inline void Install(const std::filesystem::path& game_data_root,
                    std::function<rex::ui::ImmediateDrawer*()> get_immediate_drawer) {
  g_game_data_root = game_data_root;
  g_get_immediate_drawer = std::move(get_immediate_drawer);
  fable2::gamemsg::SetGameDataRoot(game_data_root);
  ConfiguredMode();  // log the style once at startup
  fable2::turbo::g_notice = &Show;
}

}  // namespace fable2::turbo_notice
