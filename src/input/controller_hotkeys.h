// fable_2 - controller hotkeys: bindable turbo / slow / quick save / screenshot.
//
// Four actions, each bound to one controller button (or none):
//   FABLE2_PAD_TURBO       default view   - same toggle as F7 (fable2::turbo::Toggle)
//   FABLE2_PAD_SLOW        default none   - same toggle as F8 (fable2::turbo::ToggleSlow)
//   FABLE2_PAD_QUICKSAVE   default share  - same native quick save as F5
//                                           (fable2::quicksave::request, src/core/fable2_quick_save.h)
//   FABLE2_PAD_SCREENSHOT  default none   - saves the game's rendered frame as a PNG:
//                                           <dir>/Fable II YYYY-MM-DD HH-MM-SS.png. <dir> is
//                                           FABLE2_SCREENSHOT_DIR, else the `screenshot_dir` cvar,
//                                           else <game_data_root>/Screenshots.
// Buttons: none | view (SDL BACK, the 360 Back) | share (SDL MISC1) | ls | rs (stick clicks).
// The launcher sets these (tools/macos/Shared/ControlsView.swift, Controls > Game controllers).
// One button acts for one action: an explicitly set action takes its button first, and a
// default that lands on a taken button is dropped (FABLE2_PAD_SCREENSHOT=view alone moves View
// from turbo to screenshot). Two explicit actions on one button: the first in the list wins.
// (Before 2026-10-21 View defaulted to screenshot and turbo was unbound.)
//
// - A bound button no longer reaches the game: BackFilterDriver wraps the SDK's SDL
//   gamepad driver and clears its XInput bit (view -> BACK, ls -> LEFT_THUMB,
//   rs -> RIGHT_THUMB; share has no XInput bit) from its state and keystrokes.
//   An unbound button reaches the game as usual (unbound View = Back again).
//   FABLE2_VIEW_BUTTON_TO_GAME=1 lets Back through even while View is bound (the action still fires).
//
// Buttons are read straight from SDL gamepad events through an SDL event watch
// (UI thread), so they work for any connected controller and act once per press
// (down edge, 300 ms bounce guard). Pad events and the actions also arrive while the app is
// in the background (FABLE2_PAD_BACKGROUND, default on, test_background.h; =0: focus only), and a
// test-only instance (FABLE2_PHYSICAL_INPUT=0) takes only the test
// channel's virtual pad, the same rules as the keyboard F7. Each open is logged once
// with the controller name and whether it reports MISC1 (Share).
//
// Share on macOS: SDL reads Xbox pads through Apple's GameController framework
// (SDL "MFi" backend, GUID signature 'm') and maps "Button Share" to MISC1, but the
// user's Bluetooth Xbox Series pad never produced a MISC1 event. So Share is also
// read straight from GameController (controller_share_macos.mm: gesture state +
// pressedChangedHandler on the Share element) while Share is bound. Either source
// triggers Share's action; a second report of the same press within 500 ms is logged
// and ignored. With Share unbound the watcher is not installed (macOS keeps its own
// Share gesture). Diagnostics: each pad's SDL backend/mapping is logged once; every
// MISC1 down/up from SDL, the first 5 button-downs of a session, and every GameController Share
// down/up (via the element handler or the 30 Hz poll) are logged;
// FABLE2_PAD_DEBUG=1 logs every SDL gamepad and joystick button down/up.
//
// Test channel: metal_test_control.mm `press <session> share|view|ls|rs` pushes
// kTestPadEventType user events. The watch then drives a virtual SDL gamepad
// (attached on the first such press), so the press travels the real path: SDL
// gamepad event -> this watch, and SDK SDL driver -> BackFilterDriver -> guest.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "fable2_turbo.h"
#include "test_background.h"

#ifdef __APPLE__
#include "controller_share_macos.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

// Defined in main.cpp.
REXCVAR_DECLARE(std::string, screenshot_dir);

// Native quick save (src/core/fable2_quick_save.h, included in main.cpp).
namespace fable2::quicksave {
inline void request();
}

namespace fable2::padkeys {

// Shared with metal_test_control.mm (keep in sync). Next to the test-key event
// type 0xF2B5 (test_background.h). user.code = SDL_GamepadButton | flags.
constexpr uint32_t kTestPadEventType = 0xF2B6;
constexpr int32_t kTestPadDownBit = 0x10000;
constexpr int32_t kTestPadPrepareBit = 0x20000;  // attach the virtual pad only
constexpr const char* kVirtualPadName = "Fable II test pad (virtual)";

inline bool view_to_game() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_VIEW_BUTTON_TO_GAME");
    return v && v[0] == '1';
  }();
  return on;
}

inline bool pad_debug() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_PAD_DEBUG");
    return v && v[0] == '1';
  }();
  return on;
}

// ---------------------------------------------------------------------------
// Bindings (FABLE2_PAD_TURBO / FABLE2_PAD_SLOW / FABLE2_PAD_QUICKSAVE / FABLE2_PAD_SCREENSHOT).
enum class Action : uint8_t { kNone, kTurbo, kQuickSave, kScreenshot, kSlow };
enum Button : int { kView, kShare, kLeftStick, kRightStick, kButtonCount };

inline const char* ActionName(Action a) {
  switch (a) {
    case Action::kTurbo: return "turbo";
    case Action::kSlow: return "slow";
    case Action::kQuickSave: return "quick save";
    case Action::kScreenshot: return "screenshot";
    default: return "nothing";
  }
}
inline const char* ButtonName(int b) {
  static constexpr const char* kNames[kButtonCount] = {"View", "Share", "Left stick click",
                                                       "Right stick click"};
  return b >= 0 && b < kButtonCount ? kNames[b] : "?";
}
// SDL button -> Button, or -1.
inline int ButtonFromSdl(int sdl_button) {
  switch (sdl_button) {
    case SDL_GAMEPAD_BUTTON_BACK: return kView;
    case SDL_GAMEPAD_BUTTON_MISC1: return kShare;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return kLeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return kRightStick;
    default: return -1;
  }
}

struct Bindings {
  Action by_button[kButtonCount] = {};
  std::string summary;  // "turbo=View, slow=none, quick save=Share, screenshot=none"
  std::vector<std::string> warnings;
  std::vector<std::string> notes;  // defaults that gave way to a set action
  Action at(int b) const { return b >= 0 && b < kButtonCount ? by_button[b] : Action::kNone; }
  int button_of(Action a) const {
    for (int b = 0; b < kButtonCount; ++b)
      if (by_button[b] == a) return b;
    return -1;
  }
};

// "none"=-1, a Button, or -2 (invalid).
inline int ParseButton(std::string v) {
  for (auto& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (v == "none" || v == "off" || v.empty()) return -1;
  if (v == "view" || v == "back") return kView;
  if (v == "share" || v == "misc1") return kShare;
  if (v == "ls" || v == "l3" || v == "leftstick") return kLeftStick;
  if (v == "rs" || v == "r3" || v == "rightstick") return kRightStick;
  return -2;
}

inline Bindings ResolveBindings() {
  struct Slot {
    Action action;
    const char* env;
    int fallback;
  };
  const Slot slots[] = {{Action::kTurbo, "FABLE2_PAD_TURBO", kView},
                        {Action::kSlow, "FABLE2_PAD_SLOW", -1},
                        {Action::kQuickSave, "FABLE2_PAD_QUICKSAVE", kShare},
                        {Action::kScreenshot, "FABLE2_PAD_SCREENSHOT", -1}};
  constexpr int kSlots = int(sizeof(slots) / sizeof(slots[0]));
  Bindings out;
  int chosen[kSlots];
  bool explicit_set[kSlots];
  for (int i = 0; i < kSlots; ++i) {
    const char* env = std::getenv(slots[i].env);
    explicit_set[i] = env && *env;
    chosen[i] = slots[i].fallback;
    if (!explicit_set[i]) continue;
    const int parsed = ParseButton(env);
    if (parsed == -2) {
      out.warnings.push_back(std::format("ignoring {}='{}' (none|view|share|ls|rs)", slots[i].env, env));
      explicit_set[i] = false;
    } else {
      chosen[i] = parsed;
    }
  }
  // Explicit settings first, then defaults on buttons still free.
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < kSlots; ++i) {
      if (explicit_set[i] != (pass == 0) || chosen[i] < 0) continue;
      Action& slot = out.by_button[chosen[i]];
      if (slot != Action::kNone) {
        (pass == 0 ? out.warnings : out.notes)
            .push_back(std::format("{} on {} dropped: {} has it", ActionName(slots[i].action),
                                   ButtonName(chosen[i]), ActionName(slot)));
        continue;
      }
      slot = slots[i].action;
    }
  }
  for (int i = 0; i < kSlots; ++i) {
    const int b = out.button_of(slots[i].action);
    out.summary += std::format("{}{}={}", i ? ", " : "", ActionName(slots[i].action),
                               b < 0 ? "none" : ButtonName(b));
  }
  return out;
}

inline const Bindings& bindings() {
  static const Bindings b = ResolveBindings();
  return b;
}

// XInput bits / keystroke keys the guest must not see (bound buttons; Back stays with
// FABLE2_VIEW_BUTTON_TO_GAME=1).
inline uint16_t RemovedGuestButtons() {
  static const uint16_t mask = [] {
    const auto& b = bindings();
    uint16_t m = 0;
    if (b.at(kView) != Action::kNone && !view_to_game()) m |= rex::input::X_INPUT_GAMEPAD_BACK;
    if (b.at(kLeftStick) != Action::kNone) m |= rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
    if (b.at(kRightStick) != Action::kNone) m |= rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
    return m;
  }();
  return mask;
}
// Bound XInput bits (removed or, for Back with FABLE2_VIEW_BUTTON_TO_GAME=1, passed): logged per press.
inline uint16_t BoundGuestButtons() {
  const auto& b = bindings();
  uint16_t m = 0;
  if (b.at(kView) != Action::kNone) m |= rex::input::X_INPUT_GAMEPAD_BACK;
  if (b.at(kLeftStick) != Action::kNone) m |= rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
  if (b.at(kRightStick) != Action::kNone) m |= rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
  return m;
}
inline bool RemovedKeystroke(uint16_t vk) {
  const uint16_t m = RemovedGuestButtons();
  using VK = rex::ui::VirtualKey;
  return ((m & rex::input::X_INPUT_GAMEPAD_BACK) && vk == uint16_t(VK::kXInputPadBack)) ||
         ((m & rex::input::X_INPUT_GAMEPAD_LEFT_THUMB) && vk == uint16_t(VK::kXInputPadLThumbPress)) ||
         ((m & rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB) && vk == uint16_t(VK::kXInputPadRThumbPress));
}

namespace detail {
inline void OnSdkDevicesChanged();
}

// ---------------------------------------------------------------------------
// Filter around the SDK SDL driver: bound buttons do not reach the guest.
class BackFilterDriver final : public rex::input::InputDriver {
 public:
  explicit BackFilterDriver(std::unique_ptr<rex::input::InputDriver> inner)
      : rex::input::InputDriver(nullptr, 0), inner_(std::move(inner)) {
    // InputSystem::SetActiveCallback reaches only this wrapper; pass it on. The SDK callback is false
    // without window focus; with background pads on (FABLE2_PAD_BACKGROUND, test_background.h) the pad
    // stays active while the app has no keyboard focus. Focused, the SDK rule (ImGui overlay capturing
    // the mouse) still applies.
    inner_->set_is_active_callback([this] {
      if (fable2::testbg::pad_background() && !SDL_GetKeyboardFocus()) return true;
      return is_active();
    });
  }
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }  // inner already set up
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    const size_t before = out.size();
    inner_->EnumerateDevices(out);
    // The test channel's virtual pad feeds guest user 0 (like the keyboard pad),
    // so it reaches the game even when a real controller holds slot 0.
    for (size_t i = before; i < out.size(); ++i)
      if (out[i].name == kVirtualPadName) out[i].synthetic = true;
    if (out.size() - before != last_count_) {
      last_count_ = out.size() - before;
      detail::OnSdkDevicesChanged();  // log MISC1 support of newly opened pads
    }
  }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id,
                               rex::input::X_INPUT_STATE* out_state) override {
    const auto result = inner_->GetDeviceState(id, out_state);
    if (result != X_ERROR_SUCCESS || !out_state) return result;
    const uint16_t buttons = out_state->gamepad.buttons;
    const uint16_t removed = RemovedGuestButtons();
    if (buttons & removed) out_state->gamepad.buttons.set(uint16_t(buttons & ~removed));
    // One line per press of a bound button, so a log shows what the guest got.
    const uint16_t held = buttons & BoundGuestButtons();
    std::lock_guard lock(mutex_);
    uint16_t& was = held_[uint64_t(id)];
    const uint16_t pressed = uint16_t(held & ~was);
    was = held;
    for (const uint16_t bit : {uint16_t(rex::input::X_INPUT_GAMEPAD_BACK),
                               uint16_t(rex::input::X_INPUT_GAMEPAD_LEFT_THUMB),
                               uint16_t(rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB)}) {
      if (!(pressed & bit)) continue;
      const char* name = bit == rex::input::X_INPUT_GAMEPAD_BACK         ? "Back"
                         : bit == rex::input::X_INPUT_GAMEPAD_LEFT_THUMB ? "LeftThumb"
                                                                         : "RightThumb";
      REXLOG_INFO("[pad-hotkeys] guest poll: {} held on SDL pad device {} -> {}", name, uint64_t(id),
                  (removed & bit) ? "removed (not sent to the game)" : "passed to the game");
    }
    return result;
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                      rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    return inner_->GetDeviceCapabilities(id, flags, out_caps);
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id,
                                   rex::input::X_INPUT_VIBRATION* vibration) override {
    return inner_->SetDeviceVibration(id, vibration);
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                                   rex::input::X_INPUT_KEYSTROKE* out) override {
    // Drop keystrokes (down, repeat and up) of removed buttons; return the next one instead.
    for (int i = 0; i < 64; ++i) {
      const auto result = inner_->GetDeviceKeystroke(id, flags, out);
      if (result != X_ERROR_SUCCESS || !out || !RemovedGuestButtons()) return result;
      if (!RemovedKeystroke(uint16_t(out->virtual_key))) return result;
    }
    return X_ERROR_EMPTY;
  }
  void OnWindowAvailable(rex::ui::Window* window) override {
    inner_->OnWindowAvailable(window);
  }

 private:
  std::unique_ptr<rex::input::InputDriver> inner_;
  size_t last_count_ = 0;
  std::mutex mutex_;
  std::map<uint64_t, uint16_t> held_;  // SDL pad device id -> bound buttons held
};

// ---------------------------------------------------------------------------
// Screenshot.
namespace detail {

inline std::mutex& path_mutex() {
  static std::mutex m;
  return m;
}
inline std::filesystem::path& default_dir() {
  static std::filesystem::path p;
  return p;
}

inline std::filesystem::path ScreenshotDir() {
  if (const char* env = std::getenv("FABLE2_SCREENSHOT_DIR"); env && *env)
    return std::filesystem::path(env);
  const std::string cvar = REXCVAR_GET(screenshot_dir);
  if (!cvar.empty()) return std::filesystem::path(cvar);
  std::lock_guard lock(path_mutex());
  return default_dir();
}

#ifdef __APPLE__
// RawImage is R8 G8 B8 X8 rows of `stride` bytes. Writes <path>.tmp, then renames.
inline bool WritePng(const rex::ui::RawImage& image, const std::filesystem::path& path) {
  const size_t bytes = size_t(image.stride) * image.height;
  if (!image.width || !image.height || image.stride < size_t(image.width) * 4 ||
      image.data.size() < bytes)
    return false;
  bool ok = false;
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, image.data.data(), bytes, nullptr);
  CGImageRef cg = (space && provider)
      ? CGImageCreate(image.width, image.height, 8, 32, image.stride, space,
                      CGBitmapInfo(kCGImageAlphaNoneSkipLast), provider,
                      nullptr, false, kCGRenderingIntentDefault)
      : nullptr;
  auto temporary = path;
  temporary += ".tmp";
  const std::string tmp = temporary.string();
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(tmp.data()), CFIndex(tmp.size()), false);
  CGImageDestinationRef dest =
      (cg && url) ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr) : nullptr;
  if (dest) {
    CGImageDestinationAddImage(dest, cg, nullptr);
    ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
  }
  if (url) CFRelease(url);
  if (cg) CGImageRelease(cg);
  if (provider) CGDataProviderRelease(provider);
  if (space) CGColorSpaceRelease(space);
  std::error_code ec;
  if (ok) {
    std::filesystem::rename(temporary, path, ec);
    ok = !ec;
  }
  if (!ok) std::filesystem::remove(temporary, ec);
  return ok;
}
#endif

inline std::filesystem::path NextScreenshotPath(const std::filesystem::path& dir) {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &local);
  auto path = dir / std::format("Fable II {}.png", stamp);
  for (int n = 2; std::filesystem::exists(path) && n < 100; ++n)
    path = dir / std::format("Fable II {} ({}).png", stamp, n);
  return path;
}

// UI thread. Reads the guest frame (same CaptureGuestOutput as the test-control
// `capture`), then encodes and writes the PNG on a worker thread.
inline void TakeScreenshot() {
  static std::atomic<uint32_t> count{0};
  auto* rt = rex::Runtime::instance();
  auto* graphics = rt ? rt->graphics_system() : nullptr;
  auto* presenter = graphics ? graphics->presenter() : nullptr;
  if (!presenter) {
    REXLOG_WARN("[pad-hotkeys] screenshot skipped: no presenter");
    return;
  }
  auto image = std::make_shared<rex::ui::RawImage>();
  if (!presenter->CaptureGuestOutput(*image)) {
    REXLOG_WARN("[pad-hotkeys] screenshot skipped: no rendered frame yet");
    return;
  }
  const uint32_t n = count.fetch_add(1) + 1;
  std::thread([image, n] {
    const auto dir = ScreenshotDir();
    if (dir.empty()) {
      REXLOG_WARN("[pad-hotkeys] screenshot #{} skipped: no screenshot folder", n);
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = NextScreenshotPath(dir);
#ifdef __APPLE__
    const bool ok = WritePng(*image, path);
#else
    const bool ok = false;  // PNG encoding is implemented with ImageIO (macOS) only
#endif
    if (ok)
      REXLOG_INFO("[pad-hotkeys] screenshot #{} {}x{} -> {}", n, image->width, image->height,
                  path.string());
    else
      REXLOG_WARN("[pad-hotkeys] screenshot #{} failed to write {}", n, path.string());
  }).detach();
}

// ---------------------------------------------------------------------------
// SDL side (UI thread only).
struct State {
  rex::ui::WindowedAppContext* context = nullptr;
  std::set<SDL_JoystickID> logged, buttons_logged;
  SDL_JoystickID virtual_id = 0;
  SDL_Joystick* virtual_joystick = nullptr;
  std::string saved_background_hint;
  bool background_hint_forced = false;
  std::chrono::steady_clock::time_point last_press[kButtonCount]{};  // bounce guard
};
inline State& state() {
  static State s;
  return s;
}

// Never opens a controller itself: the SDK's SDL driver opens every pad, and SDL
// then sends gamepad events to every watch. (Opening the user's real pad from
// here at startup coincided with a background test session becoming active.)
// Logged once per pad: name/ids at connect, MISC1 support once the pad is open.
inline void LogButtons(SDL_JoystickID id);

inline void LogConnected(SDL_JoystickID id) {
  auto& s = state();
  if (!s.logged.insert(id).second) return;
  const char* name = SDL_GetGamepadNameForID(id);
  REXLOG_INFO("[pad-hotkeys] controller connected: \"{}\" (id {}, type {}, vid 0x{:04X} pid 0x{:04X})",
              name ? name : "?", uint32_t(id), int(SDL_GetGamepadTypeForID(id)),
              SDL_GetGamepadVendorForID(id), SDL_GetGamepadProductForID(id));
  // MISC1 support needs the open pad: logged when the SDK driver reports it
  // (BackFilterDriver::EnumerateDevices -> OnSdkDevicesChanged) or on a press.
}

inline void LogButtons(SDL_JoystickID id) {
  auto& s = state();
  if (s.buttons_logged.count(id)) return;
  SDL_Gamepad* pad = SDL_GetGamepadFromID(id);  // no open; null until the SDK opens it
  if (!pad) return;
  s.buttons_logged.insert(id);
  LogConnected(id);
  const char* name = SDL_GetGamepadName(pad);
  REXLOG_INFO(
      "[pad-hotkeys] controller \"{}\" (id {}): Share/MISC1 reported: {}; View/Back reported: {}. "
      "{}{}",
      name ? name : "?", uint32_t(id), SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_MISC1) ? "yes" : "no",
      SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_BACK) ? "yes" : "no", bindings().summary,
      bindings().at(kView) == Action::kNone ? " (View reaches the game as Back)"
      : view_to_game()                       ? " (Back also passed to the game)"
                                             : " (Back not passed to the game)");
  // Which SDL backend reads the pad: GUID byte 14 is the driver signature.
  const SDL_GUID guid = SDL_GetGamepadGUIDForID(id);
  char guid_text[64] = {};
  SDL_GUIDToString(guid, guid_text, sizeof(guid_text));
  const uint8_t sig = guid.data[14];
  const char* backend = sig == 'm'   ? "GameController framework (SDL MFi)"
                        : sig == 'h' ? "HIDAPI"
                        : sig == 'v' ? "virtual"
                        : sig == 0   ? "IOKit/other"
                                     : "other";
  char* mapping = SDL_GetGamepadMapping(pad);
  REXLOG_INFO("[pad-hotkeys] controller \"{}\" (id {}): SDL backend {} (guid {}); mapping: {}",
              name ? name : "?", uint32_t(id), backend, guid_text, mapping ? mapping : "(none)");
  SDL_free(mapping);
}

// Any thread: the SDK's pad list changed, so its pads are open now.
inline void OnSdkDevicesChanged() {
  auto* ctx = state().context;
  if (!ctx) return;
  ctx->CallInUIThreadDeferred([] {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) LogButtons(ids[i]);
    SDL_free(ids);
  });
}

// Runs a bound action (UI thread, from the SDL watch or the GameController handler).
inline void Dispatch(Action action, const std::string& what) {
  auto& s = state();
  switch (action) {
    case Action::kNone:
      if (pad_debug()) REXLOG_INFO("[pad-hotkeys] {} pressed -> nothing (not bound)", what);
      return;
    case Action::kTurbo:
      REXLOG_INFO("[pad-hotkeys] {} pressed -> turbo", what);
      // Same toggle as F7; its own UI task, outside the SDL event watch.
      if (!s.context || !s.context->CallInUIThreadDeferred([] { fable2::turbo::Toggle(); }))
        REXLOG_WARN("[pad-hotkeys] turbo skipped: UI thread unavailable");
      return;
    case Action::kSlow:
      REXLOG_INFO("[pad-hotkeys] {} pressed -> slow", what);
      // Same toggle as F8.
      if (!s.context || !s.context->CallInUIThreadDeferred([] { fable2::turbo::ToggleSlow(); }))
        REXLOG_WARN("[pad-hotkeys] slow skipped: UI thread unavailable");
      return;
    case Action::kQuickSave:
      REXLOG_INFO("[pad-hotkeys] {} pressed -> quick save", what);
      fable2::quicksave::request();
      return;
    case Action::kScreenshot:
      REXLOG_INFO("[pad-hotkeys] {} pressed -> screenshot", what);
      // Not inside the SDL event watch: run the GPU readback as its own UI task.
      if (!s.context || !s.context->CallInUIThreadDeferred([] { TakeScreenshot(); }))
        REXLOG_WARN("[pad-hotkeys] screenshot skipped: UI thread unavailable");
      return;
  }
}

// Share from either source (SDL MISC1 or GameController). One action per press:
// a report within 500 ms of the last accepted one is the same press.
inline void ShareDown(const std::string& source) {
  static std::atomic<int64_t> last_ms{-1000000};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  const int64_t prev = last_ms.load();
  if (now - prev < 500) {
    REXLOG_INFO("[pad-hotkeys] Share via {}: same press as {} ms ago, ignored", source, now - prev);
    return;
  }
  last_ms.store(now);
  Dispatch(bindings().at(kShare), std::format("Share ({})", source));
}

inline void OnButtonDown(SDL_JoystickID which, int sdl_button) {
  const int button = ButtonFromSdl(sdl_button);
  if (button < 0) return;
  if (button == kShare) {
    ShareDown(std::format("SDL MISC1, pad {}", uint32_t(which)));
    return;
  }
  const Action action = bindings().at(button);
  if (action == Action::kNone) return;  // reaches the game as a normal button
  auto& s = state();
  const auto now = std::chrono::steady_clock::now();
  if (now - s.last_press[button] < std::chrono::milliseconds(300)) return;  // bounce guard
  s.last_press[button] = now;
  Dispatch(action, std::format("{} on pad {}", ButtonName(button), uint32_t(which)));
}

// Virtual pad for the test channel. Buttons 0..15 = SDL_GamepadButton SOUTH..MISC1
// (with a contiguous button_mask SDL's automatic mapping keeps the indices); four
// stick axes and no trigger axes, so it reads as an idle pad.
inline bool EnsureVirtualPad() {
  auto& s = state();
  if (s.virtual_joystick) return true;
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = 4;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_MISC1 + 1;
  desc.button_mask = (1u << (SDL_GAMEPAD_BUTTON_MISC1 + 1)) - 1;
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY) |
                   (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY);
  desc.name = kVirtualPadName;
  s.virtual_id = SDL_AttachVirtualJoystick(&desc);
  if (!s.virtual_id) {
    REXLOG_WARN("[pad-hotkeys] test: virtual gamepad attach failed: {}", SDL_GetError());
    return false;
  }
  s.virtual_joystick = SDL_OpenJoystick(s.virtual_id);
  if (!s.virtual_joystick) {
    REXLOG_WARN("[pad-hotkeys] test: virtual gamepad open failed: {}", SDL_GetError());
    return false;
  }
  REXLOG_INFO("[pad-hotkeys] test: virtual gamepad attached (id {})", uint32_t(s.virtual_id));
  return true;
}

inline void OnTestPadEvent(int32_t code) {
  auto& s = state();
  const int button = code & 0xFFFF;
  const bool down = (code & kTestPadDownBit) != 0;
  if (!EnsureVirtualPad()) {
    // No virtual pad: still exercise the hotkey action itself.
    if (down && !(code & kTestPadPrepareBit)) OnButtonDown(0, button);
    return;
  }
  if (code & kTestPadPrepareBit) return;
  // SDL drops joystick button-downs while the app has no keyboard focus (the
  // background test window never has it). Allow them only for this press.
  if (down && !s.background_hint_forced) {
    const char* old = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
    s.saved_background_hint = old ? old : "";
    s.background_hint_forced = true;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  }
  SDL_SetJoystickVirtualButton(s.virtual_joystick, button, down);
  SDL_UpdateJoysticks();  // emit the gamepad event now (watch runs here)
  if (!down && s.background_hint_forced) {
    s.background_hint_forced = false;
    if (s.saved_background_hint.empty())
      SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
    else
      SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, s.saved_background_hint.c_str());
  }
}

inline bool test_pad_enabled() {
  static const bool on = [] {
    const char* dir = std::getenv("FABLE2_TEST_CONTROL_DIR");
    return dir && *dir;
  }();
  return on;
}

inline bool SDLCALL Watch(void*, SDL_Event* event) {
  switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
      const SDL_JoystickID id = event->gdevice.which;
      if (auto* ctx = state().context) ctx->CallInUIThreadDeferred([id] { LogConnected(id); });
      break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
      const SDL_JoystickID id = event->gdevice.which;
      if (auto* ctx = state().context)
        ctx->CallInUIThreadDeferred([id] {
          state().logged.erase(id);
          state().buttons_logged.erase(id);
          REXLOG_INFO("[pad-hotkeys] controller {} disconnected", uint32_t(id));
        });
      break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      const SDL_JoystickID id = event->gbutton.which;
      // Test-only instance: only the test channel's virtual pad may trigger hotkeys.
      if (!fable2::testbg::physical_input() && id != state().virtual_id) break;
      const int button = event->gbutton.button;
      const bool down = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      // The first few presses of each session are always logged, so a play log shows that pad
      // input arrives at all (and under which SDL button) next to any Share report.
      static std::atomic<int> logged_downs{0};
      const bool early = down && id != state().virtual_id && logged_downs.fetch_add(1) < 5;
      if (button == SDL_GAMEPAD_BUTTON_MISC1 || pad_debug() || early) {
        const char* bname = SDL_GetGamepadStringForButton(SDL_GamepadButton(button));
        REXLOG_INFO("[pad-hotkeys] SDL gamepad button {} ({}) {} on pad {}", bname ? bname : "?",
                    button, down ? "down" : "up", uint32_t(id));
      }
      if (!down) break;
      OnButtonDown(id, button);
      // The sets in State are UI-thread only; log the pad's buttons from there.
      if (auto* ctx = state().context) ctx->CallInUIThreadDeferred([id] { LogButtons(id); });
      break;
    }
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
      if (pad_debug())
        REXLOG_INFO("[pad-hotkeys] SDL joystick {} raw button {} {}", uint32_t(event->jbutton.which),
                    int(event->jbutton.button),
                    event->type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? "down" : "up");
      break;
    default:
      if (event->type == kTestPadEventType && test_pad_enabled()) {
        const int32_t code = event->user.code;
        if (auto* ctx = state().context) ctx->CallInUIThreadDeferred([code] { OnTestPadEvent(code); });
      }
      break;
  }
  return true;
}

}  // namespace detail

// The default folder: <game_data_root>/Screenshots.
inline void SetDefaultScreenshotDir(std::filesystem::path dir) {
  std::lock_guard lock(detail::path_mutex());
  detail::default_dir() = std::move(dir);
}

// UI thread, after the input system has initialized SDL's gamepad subsystem.
inline void Install(rex::ui::WindowedAppContext* context) {
  auto& s = detail::state();
  if (s.context || !context) return;
  s.context = context;
  if (!SDL_AddEventWatch(detail::Watch, nullptr)) {
    REXLOG_WARN("[pad-hotkeys] event watch failed: {}", SDL_GetError());
    return;
  }
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i) detail::LogConnected(ids[i]);
  SDL_free(ids);
  const auto& b = bindings();
  for (const auto& warning : b.warnings) REXLOG_WARN("[pad-hotkeys] {}", warning);
  for (const auto& note : b.notes) REXLOG_INFO("[pad-hotkeys] default {}", note);
  REXLOG_INFO("[pad-hotkeys] installed: {}; screenshots -> {} ({} pad(s) now){}", b.summary,
              detail::ScreenshotDir().string(), count,
              b.at(kView) == Action::kNone ? "; View reaches the game as Back" : "");
#ifdef __APPLE__
  // Share straight from GameController (main thread = this UI thread). Not on a test-only
  // instance (FABLE2_PHYSICAL_INPUT=0): GameController sees controllers whatever SDL does.
  if (!fable2::testbg::physical_input()) {
    REXLOG_INFO("[pad-hotkeys] physical input off: GameController Share watcher not installed");
    return;
  }
  // GameController (SDL's MFi backend and the Share watcher) delivers input in the background only
  // when asked to (FABLE2_PAD_BACKGROUND).
  gcshare::MonitorBackgroundEvents(fable2::testbg::pad_background());
  REXLOG_INFO("[pad-hotkeys] controllers in the background: {}",
              fable2::testbg::pad_background() ? "on" : "off (FABLE2_PAD_BACKGROUND=0)");
  if (b.at(kShare) == Action::kNone) {
    REXLOG_INFO("[pad-hotkeys] Share not bound: GameController Share watcher not installed");
    return;
  }
  gcshare::Install(
      [](const char* line) { REXLOG_INFO("{}", line); },
      [](const char* name, bool down, bool app_active, const char* via) {
        REXLOG_INFO("[pad-hotkeys] GameController Share {} on \"{}\" (via {}, app active={})",
                    down ? "down" : "up", name ? name : "?", via ? via : "?", app_active);
        if (!down) return;
        // Same focus rule as SDL pad events: no actions from a background window, unless background
        // pads are on (FABLE2_PAD_BACKGROUND).
        if (!app_active && !fable2::testbg::pad_background()) {
          REXLOG_INFO("[pad-hotkeys] GameController Share ignored: app not active");
          return;
        }
        detail::ShareDown(std::format("GameController, \"{}\"", name ? name : "?"));
      });
#endif
}

}  // namespace fable2::padkeys
