#pragma once

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <string>
#include <string_view>
#include <initializer_list>

#include "test_background.h"

namespace fable2 {

// SDK overlay binds are registered just before OnCreateDialogs. Change only
// untouched defaults there, preserving explicit user or command-line bindings.
inline void ApplyRecompHotkeyDefaults() {
  struct Binding { std::string_view name, key; };
  // Number row (2026-10-22, most players are on MacBooks where F keys need fn): 8 achievements. The console,
  // debug overlay and settings menu have no default key (rebindable in Settings > Controls). The rest of the
  // row is the recomp's own (window_keyboard_mouse.h): ` 1 2 3 4 speed, 5 quick save, 6 F6 menu, 7 turbo,
  // 9 FPS counter, 0 frame generation.
  for (const auto& binding : {Binding{"bind_console", "None"},
                             Binding{"bind_debug_overlay", "None"},
                             Binding{"bind_settings", "None"},
                             Binding{"bind_achievements", "8"}}) {
    if (rex::cvar::GetFlagSource(binding.name) == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName(binding.name, binding.key);
  }
  // fable_2.toml is auto-generated with every cvar, so older builds persisted their defaults as config
  // values. Move a saved old default to the current one (command-line/env/runtime values stay):
  // bind_achievements "7" (turbo), "1", "4" (speed keys now) -> "8"; bind_console "Backtick" (slow
  // speed key now), bind_debug_overlay "3" and bind_settings "4" -> None.
  struct Migration { std::string_view name; std::initializer_list<std::string_view> old; std::string_view key; };
  for (const auto& m : {Migration{"bind_achievements", {"7", "1", "4"}, "8"},
                        Migration{"bind_console", {"Backtick"}, "None"},
                        Migration{"bind_debug_overlay", {"3"}, "None"},
                        Migration{"bind_settings", {"4"}, "None"}}) {
    if (rex::cvar::GetFlagSource(m.name) != rex::cvar::Source::kConfig) continue;
    const std::string old = rex::cvar::GetFlagByName(m.name);
    for (auto o : m.old) {
      if (old != o) continue;
      rex::cvar::SetFlagByName(m.name, m.key);
      REXSYS_INFO("[hotkeys] {} \"{}\" from fable_2.toml moved to \"{}\" (old default)", m.name, old, m.key);
      break;
    }
  }
  // The settings key also frees the mouse while mouse look captures it (mouse_unlock_key, src/main.cpp).
  // Untouched or a saved old default ("4"): follow bind_settings (None = no key; switching apps with
  // Cmd+Tab or opening an overlay such as the F6 menu still frees the mouse). The launcher passes both together.
  {
    const auto src = rex::cvar::GetFlagSource("mouse_unlock_key");
    if (src == rex::cvar::Source::kDefault ||
        (src == rex::cvar::Source::kConfig && rex::cvar::GetFlagByName("mouse_unlock_key") == "4")) {
      const std::string settings = rex::cvar::GetFlagByName("bind_settings");
      rex::cvar::SetFlagByName("mouse_unlock_key", settings == "None" ? "" : settings);
    }
  }
  // Test-only instance (FABLE2_PHYSICAL_INPUT=0): no SDK overlay opens from a physical key.
  if (!fable2::testbg::physical_input()) {
    for (const char* name : {"bind_debug_overlay", "bind_console", "bind_settings", "bind_achievements"})
      rex::cvar::SetFlagByName(name, "None");
    REXSYS_INFO("[hotkeys] physical input off: SDK overlay keys unbound");
    return;
  }
  REXSYS_INFO("[hotkeys] achievements \"{}\", console \"{}\", debug overlay \"{}\", settings \"{}\", "
              "mouse unlock \"{}\"",
              rex::cvar::GetFlagByName("bind_achievements"), rex::cvar::GetFlagByName("bind_console"),
              rex::cvar::GetFlagByName("bind_debug_overlay"),
              rex::cvar::GetFlagByName("bind_settings"), rex::cvar::GetFlagByName("mouse_unlock_key"));
}

}  // namespace fable2
