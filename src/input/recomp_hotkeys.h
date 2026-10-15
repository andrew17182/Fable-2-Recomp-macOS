#pragma once

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <string_view>
#include <initializer_list>

#include "test_background.h"

namespace fable2 {

// SDK overlay binds are registered just before OnCreateDialogs. Change only
// untouched defaults there, preserving explicit user or command-line bindings.
inline void ApplyRecompHotkeyDefaults() {
  struct Binding { std::string_view name, key; };
  for (const auto& binding : {Binding{"bind_debug_overlay", "3"},
                             Binding{"bind_settings", "4"},
                             Binding{"bind_achievements", "8"}}) {  // 7 = turbo
    if (rex::cvar::GetFlagSource(binding.name) == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName(binding.name, binding.key);
  }
  // fable_2.toml is auto-generated with every cvar, so older builds persisted
  // their default bind_achievements = "7" as a config value. 7 is turbo now:
  // move that saved old default to 8 (command-line/env/runtime values stay).
  if (rex::cvar::GetFlagSource("bind_achievements") == rex::cvar::Source::kConfig &&
      rex::cvar::GetFlagByName("bind_achievements") == "7") {
    rex::cvar::SetFlagByName("bind_achievements", "8");
    REXSYS_INFO("[hotkeys] bind_achievements \"7\" from fable_2.toml moved to \"8\" (7 = turbo)");
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
