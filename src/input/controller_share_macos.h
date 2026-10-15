// fable_2 - Share button read straight from Apple's GameController framework
// (macOS only; implementation in controller_share_macos.mm).
//
// SDL reads Xbox pads on macOS through GameController ("MFi" backend) and maps
// "Button Share" to SDL_GAMEPAD_BUTTON_MISC1, but with the user's Xbox Series pad
// over Bluetooth no MISC1 press ever reached the app. This module, for every
// GCController (already connected and on GCControllerDidConnectNotification):
//   - logs the Share element's system-gesture state, sets it so the app receives
//     the press (default Disabled = app only, like SDL; FABLE2_SHARE_GESTURE=receive
//     -> AlwaysReceive, =leave -> untouched), and logs it again ~2 s later (after
//     SDL has opened the pad);
//   - installs a pressedChangedHandler on the Share element only (SDL polls and
//     sets no element handlers; the Xbox/Home button is left alone).
// It never opens a controller. Presses are passed to the ShareFn on the main
// (UI) thread; controller_hotkeys.h turns them into quick saves.
#pragma once

#ifdef __APPLE__
namespace fable2::padkeys::gcshare {

using LogFn = void (*)(const char* line);
// down: pressed (true) / released (false). app_active: [NSApp isActive].
using ShareFn = void (*)(const char* controller_name, bool down, bool app_active);

// Main thread. Safe to call more than once (only the first call installs).
void Install(LogFn log, ShareFn share);

}  // namespace fable2::padkeys::gcshare
#endif
