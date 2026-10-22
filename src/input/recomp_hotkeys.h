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
  // Number row first (2026-10-22, most players are on MacBooks where F keys need fn): 1 achievements,
  // 3 debug overlay, 4 settings; 5-9 and 0 are the recomp's own hotkeys (window_keyboard_mouse.h).
  for (const auto& binding : {Binding{"bind_debug_overlay", "3"},
                             Binding{"bind_settings", "4"},
                             Binding{"bind_achievements", "1"}}) {
    if (rex::cvar::GetFlagSource(binding.name) == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName(binding.name, binding.key);
  }
  // fable_2.toml is auto-generated with every cvar, so older builds persisted
  // their default bind_achievements ("7", later "8") as a config value. 7 is turbo and 8 slow
  // motion now: move a saved old default to 1 (command-line/env/runtime values stay).
  if (rex::cvar::GetFlagSource("bind_achievements") == rex::cvar::Source::kConfig) {
    const std::string old = rex::cvar::GetFlagByName("bind_achievements");
    if (old == "7" || old == "8") {
      rex::cvar::SetFlagByName("bind_achievements", "1");
      REXSYS_INFO("[hotkeys] bind_achievements \"{}\" from fable_2.toml moved to \"1\" ({} = {})", old, old,
                  old == "7" ? "turbo" : "slow motion");
    }
  }
  // Test-only instance (FABLE2_PHYSICAL_INPUT=0): no SDK overlay opens from a physical key.
  if (!fable2::testbg::physical_input()) {
    for (const char* name : {"bind_debug_overlay", "bind_console", "bind_settings", "bind_achievements"})
      rex::cvar::SetFlagByName(name, "None");
    REXSYS_INFO("[hotkeys] physical input off: SDK overlay keys unbound");
    return;
  }
  REXSYS_INFO("[hotkeys] bind_achievements = \"{}\"", rex::cvar::GetFlagByName("bind_achievements"));
}

}  // namespace fable2
