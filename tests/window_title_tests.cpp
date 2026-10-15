// Build-time check of the window-title options suffix (src/core/fable2_window_title.h).
#include "fable2_window_title.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Expect(const fable2::window_title::Options& o, const std::string& want) {
  const std::string got = fable2::window_title::Suffix(o);
  if (got != want) {
    std::fprintf(stderr, "window_title_tests: got '%s', want '%s'\n", got.c_str(), want.c_str());
    ++g_failures;
  }
}

}  // namespace

int main() {
  using fable2::window_title::Options;
  Options o;
  o.scale = 2; o.scale_auto = true; o.fps = 60; o.fps_auto = true; o.metalfx = true;
  Expect(o, " \xC2\xB7 2\xC3\x97 (Auto) \xC2\xB7 60 FPS (Auto) \xC2\xB7 MetalFX");

  Options plain;  // explicit 1x, explicit 30 FPS, nothing else
  Expect(plain, " \xC2\xB7 1\xC3\x97 \xC2\xB7 30 FPS");

  Options test;
  test.fps = 0; test.scale = 3; test.turbo = true; test.tag = "benchmark 3u";
  Expect(test, " \xC2\xB7 3\xC3\x97 \xC2\xB7 uncapped \xC2\xB7 turbo \xC2\xB7 benchmark 3u");

  Options rule2x;  // both auto, the 2x rule picked 2x uncapped (M3 Pro)
  rule2x.scale = 2; rule2x.scale_auto = true; rule2x.fps = 0; rule2x.fps_auto = true;
  Expect(rule2x, " \xC2\xB7 2\xC3\x97 (Auto) \xC2\xB7 uncapped (Auto)");

  Options tagged;
  tagged.fps = 60; tagged.tag = "  test agent-2\n ";
  Expect(tagged, " \xC2\xB7 1\xC3\x97 \xC2\xB7 60 FPS \xC2\xB7 test agent-2");

  Options blank;
  blank.tag = "   ";
  Expect(blank, " \xC2\xB7 1\xC3\x97 \xC2\xB7 30 FPS");

  // Long tags are cut at 48 bytes on a UTF-8 boundary.
  const std::string long_tag = std::string(47, 'a') + "\xC3\x97" + "tail";
  const std::string cleaned = fable2::window_title::CleanTag(long_tag);
  if (cleaned != std::string(47, 'a')) {
    std::fprintf(stderr, "window_title_tests: CleanTag cut '%s'\n", cleaned.c_str());
    ++g_failures;
  }

  if (g_failures) return 1;
  std::puts("window_title_tests: ok");
  return 0;
}
