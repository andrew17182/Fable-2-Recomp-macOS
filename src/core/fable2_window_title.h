// Window-title suffix: the effective options, set once at startup
// (src/core/fable_2_app.h WindowTitle), e.g.
//   "Fable II Recomp 0.1.0 (1bba6e2) · Metal · 2× (Auto) · 60 FPS (Auto) · MetalFX"
//   "Fable II [AGENT TEST <id>] 0.1.0 (1bba6e2) · Metal · 1× · 60 FPS · test agent-2"
// Only the scale, the frame target and notable non-default extras (MetalFX,
// turbo at start) are listed, never the set of fixes. FABLE2_WINDOW_TAG (set by
// the test, benchmark and warm-up launchers, never by normal play) is appended
// last so agent windows are easy to tell from the player's.
//
// Pure (no runtime state) so tests/window_title_tests.cpp can check it.
#pragma once

#include <string>

namespace fable2::window_title {

struct Options {
  int scale = 1;           // resolution scale actually running
  bool scale_auto = false; // resolution "auto" picked it
  int fps = 30;            // 60, 30, or 0 = uncapped (vsync off)
  bool fps_auto = false;   // fps_60 = "auto" picked it (uncapped too: the 2x rule)
  bool metalfx = false;    // MetalFX spatial upscaling on
  bool turbo = false;      // turbo active at startup (FABLE2_TURBO_SCALE)
  std::string tag;         // FABLE2_WINDOW_TAG, may be empty
};

// Keeps a tag printable and short: control characters dropped, at most 48 bytes
// (cut at a UTF-8 boundary), surrounding spaces trimmed.
inline std::string CleanTag(const std::string& raw) {
  std::string out;
  for (unsigned char c : raw)
    if (c >= 0x20 && c != 0x7F) out.push_back(static_cast<char>(c));
  if (out.size() > 48) {
    size_t n = 48;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
    out.resize(n);
  }
  const size_t b = out.find_first_not_of(' ');
  if (b == std::string::npos) return "";
  return out.substr(b, out.find_last_not_of(' ') - b + 1);
}

// " · 2× (Auto) · 60 FPS (Auto) · MetalFX · test agent-2"
inline std::string Suffix(const Options& o) {
  static const char* kSep = " \xC2\xB7 ";  // " · "
  std::string s = kSep + std::to_string(o.scale < 1 ? 1 : o.scale) + "\xC3\x97";  // "×"
  if (o.scale_auto) s += " (Auto)";
  s += kSep;
  if (o.fps <= 0) {
    s += "uncapped";
    if (o.fps_auto) s += " (Auto)";
  } else {
    s += std::to_string(o.fps) + " FPS";
    if (o.fps_auto) s += " (Auto)";
  }
  if (o.metalfx) s += std::string(kSep) + "MetalFX";
  if (o.turbo) s += std::string(kSep) + "turbo";
  const std::string tag = CleanTag(o.tag);
  if (!tag.empty()) s += kSep + tag;
  return s;
}

}  // namespace fable2::window_title
