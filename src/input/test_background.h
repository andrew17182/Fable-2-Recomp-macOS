// fable_2 - background test mode (FABLE2_TEST_BACKGROUND=1).
//
// Automated test sessions (tools/fable2_test_control.py, and
// tools/fable2_dev_session.py --test-control) set FABLE2_TEST_BACKGROUND=1 so
// the game never takes the user's keyboard/mouse:
//   - SDL is told not to activate the app or its window (hints below, applied
//     from a static initializer before SDL_Init). The Metal test-control
//     endpoint (backends/metal/port/rex/ui/metal/metal_test_control.mm) also
//     orders the window behind the frontmost app and holds an App Nap
//     exemption while the session runs.
//   - Test-control `press` no longer posts SDL key events (which the window
//     driver ignores without focus). It posts kTestKeyEventType user events;
//     WindowKeyboardMouse (window_keyboard_mouse.h) receives them through an
//     SDL event watch and holds them in a separate synthetic key set that is
//     merged into the guest pad and F5/F6/F7/5/6/7 handling regardless of focus.
//     Real host keyboard/mouse input stays focus-gated.
// Normal play (variable unset) is unchanged.
//
// Physical input switch (FABLE2_PHYSICAL_INPUT=0). Set by the host
// tools for every test-control / agent session, so the maintainer's controller, keyboard and
// mouse can only ever reach the player's own (play/playtest) instance:
//   - SDL never opens a physical controller: its HIDAPI, MFi (GameController) and IOKit
//     joystick backends are disabled before SDL_Init (hints below). The test channel's SDL
//     virtual pad still works (SDL's virtual driver is separate).
//   - The GameController-framework Share watcher is not installed and SDL pad events from any
//     device other than the test virtual pad are ignored (controller_hotkeys.h).
//   - Real keys, mouse buttons and mouse look never reach the guest pad or the F5-F9/5/6/7
//     hotkeys (window_keyboard_mouse.h treats the window as unfocused); the SDK MnK driver is
//     not added (fable_2_app.h). Synthetic test keys (kTestKeyEventType) and the replay-pad
//     channel are unaffected.
// Background controller input (FABLE2_PAD_BACKGROUND, default on; =0 = focus only): with physical
// input on, controllers keep reaching the game while its window is not focused. SDL gets
// SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 before SDL_Init (it drops pad events in the background
// otherwise), GameController monitors background events, the SDK SDL pad driver stays active without
// focus (controller_hotkeys.h BackFilterDriver), and the controller hotkeys act in the background too.
// Keyboard and mouse stay focus-only. Never in a test-only instance (FABLE2_PHYSICAL_INPUT=0).
// Window-system actions are not game input and keep working: the close button, Cmd+Q, Dock
// Quit, move/resize/minimise (AppKit handles them before any of the paths above).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <initializer_list>

#include <SDL3/SDL_hints.h>

namespace fable2::testbg {

// Shared with metal_test_control.mm (keep in sync). Far above the event
// numbers SDL_RegisterEvents hands out (sequential from SDL_EVENT_USER).
// user.code = SDL scancode | (down ? kTestKeyDownBit : 0).
constexpr uint32_t kTestKeyEventType = 0xF2B5;
constexpr int32_t kTestKeyDownBit = 0x10000;

// Optional diagnostic hook for each synthetic key-down (main.cpp sets it when FABLE2_F6_TRACE=1).
inline void (*g_test_key_trace)(uint16_t key) = nullptr;

inline bool enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TEST_BACKGROUND");
    return v && v[0] == '1';
  }();
  return on;
}

// Synthetic test keys exist only for an app-owned test-control session.
inline bool synthetic_keys_enabled() {
  static const bool on = [] {
    const char* dir = std::getenv("FABLE2_TEST_CONTROL_DIR");
    return enabled() && dir && *dir;
  }();
  return on;
}

// False when FABLE2_PHYSICAL_INPUT=0 (test-only instance: no physical controller/keyboard/mouse).
inline bool physical_input() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_PHYSICAL_INPUT");
    return !(v && v[0] == '0');
  }();
  return on;
}

// Controllers work while the window is unfocused (see above). False in test-only instances.
inline bool pad_background() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_PAD_BACKGROUND");
    return physical_input() && !(v && v[0] == '0');
  }();
  return on;
}

// SDL reads hints from the environment, so this must run before SDL_Init.
inline bool ApplyLaunchHints() {
#ifndef _WIN32
  if (pad_background()) {
    setenv(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", 1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  }
  if (!physical_input()) {
    // No physical joystick backends at all: nothing is opened, nothing is polled, and the
    // controller's lights/rumble are never touched by this instance.
    for (const char* hint : {SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_MFI, SDL_HINT_JOYSTICK_IOKIT}) {
      setenv(hint, "0", 1);
      SDL_SetHint(hint, "0");
    }
  }
#endif
  if (!enabled()) return false;
#ifdef _WIN32
  return false;
#else
  // Set both ways: SDL may snapshot the environment before this runs.
  setenv(SDL_HINT_MAC_BACKGROUND_APP, "1", 1);           // no activation on launch
  setenv(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0", 1);   // SDL_ShowWindow does not activate
  setenv(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0", 1);  // SDL_RaiseWindow does not activate
  SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
  SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
  SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
  return true;
#endif
}

}  // namespace fable2::testbg
