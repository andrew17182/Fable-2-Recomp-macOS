// fable_2 - headless shader prep (FABLE2_SHADER_PREP): never show a window or a Dock icon.
//
// The prep run (docs/offline-shader-seed.md section 2) used to hide its window only at shader storage init
// (MetalGraphicsSystem::InitializeShaderStorage -> HideAppForBackgroundWork), after the SDK had already shown it,
// so each run flashed the window for ~0.4 s and showed a Dock icon (2026-10-19, CGWindowList check). Now:
//   - ApplyEarly() (static initializer, before SDL_Init): SDL's background-app hint (SDL does not make the app a
//     regular, Dock-icon app), and the accessory activation policy once the window exists;
//   - Watch (SDL event watch, armed before the SDK opens the window): SDL_EVENT_WINDOW_SHOWN is sent from inside
//     SDL_ShowWindow, right after Cocoa orders the window in and before AppKit draws it, so the window is made
//     fully transparent, click-through and ordered out there. It is never drawn on screen. The prep never runs the
//     game, so nothing needs the window visible.
// Normal play and test sessions (variable unset) are unchanged.
#pragma once

#include <cstdlib>

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#endif

namespace fable2::shader_prep_hide {

inline bool requested() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_SHADER_PREP");  // getenv-ok: once, at start
    return v && *v && !(v[0] == '0' && v[1] == 0);
  }();
  return on;
}

#ifdef __APPLE__
inline void SetAccessoryPolicy() {
  using GetApp = void* (*)(void*, SEL);
  using SetPolicy = bool (*)(void*, SEL, long);
  Class app_class = objc_getClass("NSApplication");
  if (!app_class) return;
  void* app = reinterpret_cast<GetApp>(objc_msgSend)(app_class, sel_registerName("sharedApplication"));
  if (app) reinterpret_cast<SetPolicy>(objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 1L);
}

inline void HideWindow(SDL_Window* window) {
  void* nswindow = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                          SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
  if (!nswindow) return;
  using SetDouble = void (*)(void*, SEL, double);
  using SetBool = void (*)(void*, SEL, bool);
  using SetId = void (*)(void*, SEL, void*);
  reinterpret_cast<SetDouble>(objc_msgSend)(nswindow, sel_registerName("setAlphaValue:"), 0.0);
  reinterpret_cast<SetBool>(objc_msgSend)(nswindow, sel_registerName("setIgnoresMouseEvents:"), true);
  reinterpret_cast<SetId>(objc_msgSend)(nswindow, sel_registerName("orderOut:"), nullptr);
}

inline bool SDLCALL Watch(void*, SDL_Event* event) {
  if (event->type == SDL_EVENT_WINDOW_SHOWN && SDL_IsMainThread()) {
    SetAccessoryPolicy();  // NSApp exists by now (SDL created it with the window)
    if (SDL_Window* window = SDL_GetWindowFromID(event->window.windowID)) HideWindow(window);
  }
  return true;  // observe only
}
#endif

// Static initializer (main.cpp), before SDL_Init.
inline bool ApplyEarly() {
#ifdef __APPLE__
  if (!requested()) return false;
  // SDL then keeps the app a background app at SDL_Init (no Dock icon, no activation). NSApp itself is not
  // touched here: SDL creates it (its own NSApplication subclass) in SDL_Init.
  setenv(SDL_HINT_MAC_BACKGROUND_APP, "1", 1);
  SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
  return true;
#else
  return false;
#endif
}

// Before the SDK creates and shows the window (Fable2App::SetupPresentation).
inline void Arm() {
#ifdef __APPLE__
  if (!requested()) return;
  SDL_AddEventWatch(Watch, nullptr);
#endif
}

}  // namespace fable2::shader_prep_hide
