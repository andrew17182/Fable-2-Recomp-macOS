// fable_2 - controller hotkeys: Share = quick save, View = screenshot.
//
// - Share (SDL_GAMEPAD_BUTTON_MISC1 on Xbox Series controllers) makes the same
//   native quick save as F5 (fable2::quicksave::request, src/core/fable2_quick_save.h:
//   game thread, pause-menu "Save and Continue", 3 s cooldown).
// - View (SDL_GAMEPAD_BUTTON_BACK, the 360 "Back") saves the game's rendered frame
//   as a PNG: <dir>/Fable II YYYY-MM-DD HH-MM-SS.png. <dir> is FABLE2_SCREENSHOT_DIR,
//   else the `screenshot_dir` cvar, else <game_data_root>/Screenshots.
// - View no longer reaches the game as Back: BackFilterDriver wraps the SDK's SDL
//   gamepad driver and clears X_INPUT_GAMEPAD_BACK from its state and keystrokes.
//   FABLE2_VIEW_BUTTON_TO_GAME=1 lets Back through again (the screenshot still fires).
//
// Buttons are read straight from SDL gamepad events through an SDL event watch
// (UI thread), so they work for any connected controller and act once per press
// (down edge). Each open is logged once with the controller name and whether it
// reports MISC1 (Share).
//
// Share on macOS: SDL reads Xbox pads through Apple's GameController framework
// (SDL "MFi" backend, GUID signature 'm') and maps "Button Share" to MISC1, but the
// user's Bluetooth Xbox Series pad never produced a MISC1 event. So Share is also
// read straight from GameController (controller_share_macos.mm: gesture state +
// pressedChangedHandler on the Share element). Either source triggers the quick
// save; a second report of the same press within 500 ms is logged and ignored.
// Diagnostics: each pad's SDL backend/mapping is logged once; every MISC1 down/up
// from SDL and every GameController Share down/up is logged; FABLE2_PAD_DEBUG=1
// logs every SDL gamepad and joystick button down/up.
//
// Test channel: metal_test_control.mm `press <session> share|view` pushes
// kTestPadEventType user events. The watch then drives a virtual SDL gamepad
// (attached on the first such press), so the press travels the real path: SDL
// gamepad event -> this watch, and SDK SDL driver -> BackFilterDriver -> guest.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
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

namespace detail {
inline void OnSdkDevicesChanged();
}

// ---------------------------------------------------------------------------
// Back filter around the SDK SDL driver.
class BackFilterDriver final : public rex::input::InputDriver {
 public:
  explicit BackFilterDriver(std::unique_ptr<rex::input::InputDriver> inner)
      : rex::input::InputDriver(nullptr, 0), inner_(std::move(inner)) {
    // InputSystem::SetActiveCallback reaches only this wrapper; pass it on.
    inner_->set_is_active_callback([this] { return is_active(); });
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
    const bool back = (buttons & rex::input::X_INPUT_GAMEPAD_BACK) != 0;
    if (back && !view_to_game()) {
      out_state->gamepad.buttons.set(
          uint16_t(buttons & ~uint16_t(rex::input::X_INPUT_GAMEPAD_BACK)));
    }
    // One line per press, so a log shows what the guest got.
    std::lock_guard lock(mutex_);
    const bool was = back_down_.count(uint64_t(id)) != 0;
    if (back && !was) {
      back_down_.insert(uint64_t(id));
      REXLOG_INFO("[pad-hotkeys] guest poll: Back held on SDL pad device {} -> {}", uint64_t(id),
                  view_to_game() ? "passed to the game" : "removed (not sent to the game)");
    } else if (!back && was) {
      back_down_.erase(uint64_t(id));
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
    // Drop Back keystrokes (down, repeat and up); return the next one instead.
    for (int i = 0; i < 64; ++i) {
      const auto result = inner_->GetDeviceKeystroke(id, flags, out);
      if (result != X_ERROR_SUCCESS || !out || view_to_game()) return result;
      if (uint16_t(out->virtual_key) != uint16_t(rex::ui::VirtualKey::kXInputPadBack))
        return result;
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
  std::set<uint64_t> back_down_;  // SDL pad device ids with Back held
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
  std::chrono::steady_clock::time_point last_shot{};
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
      "Share = quick save, View = screenshot{}",
      name ? name : "?", uint32_t(id), SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_MISC1) ? "yes" : "no",
      SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_BACK) ? "yes" : "no",
      view_to_game() ? " (Back also passed to the game)" : " (Back not passed to the game)");
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

// Share from either source (SDL MISC1 or GameController). One quick save per
// press: a report within 500 ms of the last accepted one is the same press.
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
  REXLOG_INFO("[pad-hotkeys] Share pressed ({}) -> quick save", source);
  fable2::quicksave::request();
}

inline void OnButtonDown(SDL_JoystickID which, int button) {
  if (button == SDL_GAMEPAD_BUTTON_MISC1) {
    ShareDown(std::format("SDL MISC1, pad {}", uint32_t(which)));
  } else if (button == SDL_GAMEPAD_BUTTON_BACK) {
    auto& s = state();
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last_shot < std::chrono::milliseconds(300)) return;  // bounce guard
    s.last_shot = now;
    REXLOG_INFO("[pad-hotkeys] View pressed on pad {} -> screenshot", uint32_t(which));
    // Not inside the SDL event watch: run the GPU readback as its own UI task.
    if (!s.context || !s.context->CallInUIThreadDeferred([] { TakeScreenshot(); }))
      REXLOG_WARN("[pad-hotkeys] screenshot skipped: UI thread unavailable");
  }
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
      if (button == SDL_GAMEPAD_BUTTON_MISC1 || pad_debug()) {
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
  REXLOG_INFO("[pad-hotkeys] installed: Share = quick save, View = screenshot -> {} ({} pad(s) now)",
              detail::ScreenshotDir().string(), count);
#ifdef __APPLE__
  // Share straight from GameController (main thread = this UI thread). Not on a test-only
  // instance (FABLE2_PHYSICAL_INPUT=0): GameController sees controllers whatever SDL does.
  if (!fable2::testbg::physical_input()) {
    REXLOG_INFO("[pad-hotkeys] physical input off: GameController Share watcher not installed");
    return;
  }
  gcshare::Install(
      [](const char* line) { REXLOG_INFO("{}", line); },
      [](const char* name, bool down, bool app_active) {
        REXLOG_INFO("[pad-hotkeys] GameController Share {} on \"{}\" (app active={})",
                    down ? "down" : "up", name ? name : "?", app_active);
        if (!down) return;
        // Same focus rule as SDL pad events: no saves from a background window.
        if (!app_active) {
          REXLOG_INFO("[pad-hotkeys] GameController Share ignored: app not active");
          return;
        }
        detail::ShareDown(std::format("GameController, \"{}\"", name ? name : "?"));
      });
#endif
}

}  // namespace fable2::padkeys
