// fable_2 - ReXGlue Recompiled Project

#include "generated/default/fable_2_init.h"

// Number-row 5 -> run external Lua file. Must be included before fable_2_app.h (which
// pulls in keyboard_gamepad.h, which calls fable2::f5lua::poll_f5() per frame).
#include "fable2_f5_lua.h"

// Before fable_2_app.h, which logs the clock mode (fable2::guestclock::LogMode).
#include "fable2_guest_clock.h"  // macOS: guest clock without fractional-tick drift (SDK clock fix)
#include "fable_2_app.h"
#include "fps_meter.h"
#include "fable2_thread_trace.h"
#ifdef FABLE2_CONTINUATION_NATIVE_TRACE  // save-state work only (opt-in build define)
#include "core/continuation/native_indirect_trace.h"
#endif
#include "fable2_guest_lock_trace.h"
#ifdef FABLE2_CONTINUATION_LIVE_WORKER  // save-state work only (opt-in build define)
#include "core/continuation/live_worker.h"
#endif
#include "fable2_asset_null_trace.h"
#include "fable2_zpd_wait.h"  // occlusion results: sleep, not spin, in the culling wait (FABLE2_OCCLUSION_REAL)
#include "fable2_text_probe.h"
#include "fable2_glyph_probe.h"
#include "fable2_hotfuncs.h"
#include "fable2_heap_scan.h"
#include "fable2_text_append.h"
#include "fable2_state_probe.h"
#include "fable2_skip_intro.h"  // opt-in: FABLE2_SKIP_INTRO_MOVIES=1 hides boot logo .bik files
#include "core/fable2_loading_pacing.h"
#include "fable2_save_trace.h"  // opt-in: FABLE2_SAVE_TRACE=1 logs the front-end load-save state
#include "fable2_boot_save.h"   // opt-in: FABLE2_BOOT_SAVE=1 auto-continues into the single save
#include "fable2_quick_save.h"  // F5 / number-row 5: native quick save (pause-menu "Save and Continue")
#ifdef _WIN32
#include "fable2_av_probe.h"  // self-installs a guest-PC AV logger (VEH)
#endif
// #include "fable2_ui_render_probe.h"
#include "keyboard_gamepad.h"
#include "test_background.h"
#include "core/fable2_shader_prep_hide.h"

#include <rex/cvar.h>

// FABLE2_TEST_BACKGROUND=1 (set by the test tools): SDL launch hints so the
// game opens without activating. Must run before SDL_Init, hence a static
// initializer. See src/input/test_background.h.
[[maybe_unused]] static const bool g_fable2_test_background_hints =
    fable2::testbg::ApplyLaunchHints();
// FABLE2_F6_TRACE=1: timestamp synthetic test keys next to the F6 menu trace (src/core/fable2_f5_lua.h).
[[maybe_unused]] static const bool g_fable2_f6_trace_keys = [] {
  if (fable2::f5lua::trace_on()) {
    fable2::testbg::g_test_key_trace = [](uint16_t key) {
      fable2::f5lua::logline("[f6t %.1f] test key 0x%02X\n", fable2::f5lua::trace_ms(), key);
    };
  }
  return true;
}();
// FABLE2_SHADER_PREP (headless shader prep): no Dock icon; see src/core/fable2_shader_prep_hide.h.
[[maybe_unused]] static const bool g_fable2_shader_prep_hidden =
    fable2::shader_prep_hide::ApplyEarly();

// Host keyboard -> guest gamepad map (see src/input/keyboard_gamepad.h).
// Format: "Key:Button,Key:Button,...".
// The default layout is NOT baked in here: it lives in the config layer
// (fable2::config::kDefaultKeyboardGamepadMap) and is editable in
// fable2_config.toml under [input]. Fable2App::OnPostInitLogging seeds this
// cvar from the config at startup (only when no higher-priority source set
// it). Override at runtime with --keyboard_gamepad_map "..." or in the
// console; empty string disables the keyboard gamepad.
REXCVAR_DEFINE_STRING(
    keyboard_gamepad_map,
    "",
    "Input",
    "Map host keyboard keys to guest gamepad input "
    "(Key:Button,...; targets: A/B/X/Y, LB/RB, LT/RT, Up/Down/Left/Right, "
    "Pause, Select, L3/R3, StickUp/StickDown/StickLeft/StickRight, "
    "RStickUp/RStickDown/RStickLeft/RStickRight). "
    "Default comes from fable2_config.toml [input] keyboard_gamepad_map.");

// Mouse -> right stick (camera look). See src/input/keyboard_gamepad.h.
// The defaults below are a fallback only: Fable2App::OnPostInitLogging
// seeds these cvars from fable2_config.toml [input] at startup (when no
// higher-priority source set them). Keep the values in sync with the
// defaults in fable2::config::Values (src/core/fable2_config.h).
REXCVAR_DEFINE_BOOL(mouse_look, false, "Input",
                    "Map mouse movement to the guest right stick (camera "
                    "look): sweep to look, stop to stop. Default comes from "
                    "fable2_config.toml [input] mouse_look.");
REXCVAR_DEFINE_INT32(mouse_look_scale, 256, "Input",
                     "Mouse-look sensitivity: right-stick units per pixel of "
                     "mouse movement (larger = more sensitive). Default comes "
                     "from fable2_config.toml [input] mouse_look_scale.")
    .range(1, 4096);

// Key that toggles the settings menu (bind_settings; no key by default). While the menu is open the mouse
// lock is released (free cursor); closing it re-locks. This key is excluded from the gamepad map so it
// doesn't double as a button. Empty = no key: focus loss (switching apps) or any open overlay (console,
// F6 menu) still frees the mouse. Untouched, it follows bind_settings (src/input/recomp_hotkeys.h).
REXCVAR_DEFINE_STRING(
    mouse_unlock_key,
    "",
    "Input",
    "Key that toggles the settings menu; while the menu is open the mouse lock is "
    "released (free cursor) and re-engaged when it closes. Empty = no key (switching "
    "apps or opening an overlay still frees the mouse). Excluded from the gamepad map.");

// Controller screenshot hotkey (src/input/controller_hotkeys.h). Empty =
// <game_data_root>/Screenshots; FABLE2_SCREENSHOT_DIR overrides both.
REXCVAR_DEFINE_STRING(
    screenshot_dir,
    "",
    "Input",
    "Folder for controller screenshots (PNG). Empty = Screenshots/ "
    "in the game data root. The FABLE2_SCREENSHOT_DIR environment variable wins.");

REX_DEFINE_APP(fable_2, Fable2App::Create)
