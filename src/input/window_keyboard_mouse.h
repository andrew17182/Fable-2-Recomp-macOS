#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>

#include <SDL3/SDL.h>

#include "fable2_session_journal.h"
#include "test_background.h"
#include <rex/logging.h>
#include <rex/ui/sdl_virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace fable2::input_detail {

// SDL-backed Window events already use the SDK's portable virtual keys.
// All window operations stay on the UI thread. Guest threads only consume a
// locked snapshot, and queued UI work owns the listener until it is detached.
class WindowKeyboardMouse final
    : public rex::ui::WindowListener,
      public rex::ui::WindowInputListener,
      public std::enable_shared_from_this<WindowKeyboardMouse> {
 public:
  struct Options {
    // Mouse capture is opt-in; focusing the app alone must not trap the cursor.
    bool mouse_look = false;
    bool overlay_captures_input = false;
    uint16_t unlock_key = 0;
  };
  struct Snapshot {
    std::array<bool, 256> keys{};
    // Synthetic keys from the background test channel (test_background.h).
    // Not focus-gated; only the app-owned test-control endpoint can set them.
    std::array<bool, 256> test_keys{};
    bool active = false;
    double dx = 0, dy = 0;
    bool Down(uint16_t key) const {
      using K = rex::ui::VirtualKey;
      if (key == uint16_t(K::kShift))
        return keys[key] || keys[uint16_t(K::kLShift)] || keys[uint16_t(K::kRShift)];
      if (key == uint16_t(K::kControl))
        return keys[key] || keys[uint16_t(K::kLControl)] || keys[uint16_t(K::kRControl)];
      if (key == uint16_t(K::kMenu))
        return keys[key] || keys[uint16_t(K::kLMenu)] || keys[uint16_t(K::kRMenu)];
      return key < keys.size() && keys[key];
    }
    bool TestDown(uint16_t key) const { return key < test_keys.size() && test_keys[key]; }
  };

  WindowKeyboardMouse(std::function<Options()> options,
                      std::function<void(int)> request_lua)
      : options_(std::move(options)), request_lua_(std::move(request_lua)) {}

  // Called by InputSystem::AttachWindow on the UI thread.
  void Attach(rex::ui::Window* window) {
    if (window_ == window) return;
    Detach();
    window_ = window;
    if (!window_) return;
    context_ = &window_->app_context();
    window_->AddListener(this);
    // Observe releases even when an overlay consumes the event. Never mark
    // events handled: SDK hotkeys and text entry must continue to work.
    window_->AddInputListener(this, std::numeric_limits<size_t>::max());
    // Test-only instance (FABLE2_PHYSICAL_INPUT=0): behave as never focused, so no real key,
    // mouse button, mouse look or hotkey reaches the game; synthetic test keys still do.
    focused_ = fable2::testbg::physical_input() && window_->HasFocus();
    if (!fable2::testbg::physical_input()) REXLOG_INFO("Physical input off (FABLE2_PHYSICAL_INPUT=0): test-control input only");
    if (fable2::testbg::synthetic_keys_enabled() && !test_watch_) {
      test_watch_ = SDL_AddEventWatch(TestKeyWatch, this);
      if (test_watch_) REXLOG_INFO("Background test mode: synthetic test keys bypass the focus gate");
    }
    Refresh();
  }

  void RequestDetach() {
    if (context_) {
      context_->CallInUIThread([self = shared_from_this()] { self->Detach(); });
    }
  }

  void RequestRefresh() {
    if (!context_ || refresh_pending_.exchange(true)) return;
    if (!context_->CallInUIThread([self = shared_from_this()] {
          self->refresh_pending_ = false;
          self->Refresh();
        })) {
      refresh_pending_ = false;
    }
  }

  Snapshot Consume() {
    std::lock_guard lock(mutex_);
    auto result = snapshot_;
    result.test_keys = test_keys_;
    snapshot_.dx = snapshot_.dy = 0;
    return result;
  }

  void OnGotFocus(rex::ui::UISetupEvent&) override {
    focused_ = fable2::testbg::physical_input();
    Clear();
    Refresh();
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    focused_ = false;
    Clear();
    Refresh();
  }
  void OnClosing(rex::ui::UIEvent&) override {
    fable2::session::Get().Finish("quit");  // input recording: complete file on a normal quit
    Detach();
  }

  void OnKeyDown(rex::ui::KeyEvent& event) override {
    fable2::session::Get().Key(uint16_t(event.virtual_key()), true, event.prev_state());
    Refresh();
    if (!focused_) return;
    const auto key = uint16_t(event.virtual_key());
    const bool edge = SetKey(key, true) && !event.prev_state();
    if (!edge) return;
    // Number keys only while no overlay has the keyboard (a digit typed into a menu or console must not
    // trigger anything); F keys always (never typed text).
    int action = options_().overlay_captures_input ? 0 : NumberRowAction(key);
    if (!action) action = FKeyAction(key);
    if (action) request_lua_(action);
    if (key && key == options_().unlock_key) {
      manual_release_ = !manual_release_;
      Refresh();
    }
  }
  void OnKeyUp(rex::ui::KeyEvent& event) override {
    fable2::session::Get().Key(uint16_t(event.virtual_key()), false);
    // SDL maps both left/right modifiers to one virtual key. Releasing one
    // side must not release the binding while the other side remains held.
    using K = rex::ui::VirtualKey;
    const auto key = event.virtual_key();
    const bool down = (key == K::kShift && event.is_shift_pressed()) ||
                      (key == K::kControl && event.is_ctrl_pressed()) ||
                      (key == K::kMenu && event.is_alt_pressed());
    SetKey(uint16_t(key), focused_ && down);
  }
  void OnMouseDown(rex::ui::MouseEvent& event) override {
    Refresh();
    if (focused_) SetKey(MouseKey(event.button()), true);
  }
  void OnMouseUp(rex::ui::MouseEvent& event) override {
    SetKey(MouseKey(event.button()), false);
  }
  void OnMouseMove(rex::ui::MouseEvent& event) override {
    Refresh();
    if (!relative_) return;
    std::lock_guard lock(mutex_);
    snapshot_.dx += event.dx();
    snapshot_.dy += event.dy();
  }

 private:
  // Background test channel: metal_test_control.mm pushes kTestKeyEventType
  // user events (from the UI thread); SDL calls watches at push time. The SDK
  // event loop ignores the event afterwards. Real SDL key events never reach
  // this path, so host input stays focus-gated.
  static bool SDLCALL TestKeyWatch(void* userdata, SDL_Event* event) {
    if (event->type != fable2::testbg::kTestKeyEventType) return true;
    auto* self = static_cast<WindowKeyboardMouse*>(userdata);
    const int32_t code = event->user.code;
    const bool down = (code & fable2::testbg::kTestKeyDownBit) != 0;
    const auto scancode = static_cast<SDL_Scancode>(code & 0xFFFF);
    const auto key = uint16_t(rex::ui::TranslateSDLScancode(scancode));
    if (!key || key >= 256) return true;
    fable2::session::Get().Key(key, down);
    if (down && fable2::testbg::g_test_key_trace) fable2::testbg::g_test_key_trace(key);
    bool edge = false;
    {
      std::lock_guard lock(self->mutex_);
      edge = down && !self->test_keys_[key];
      self->test_keys_[key] = down;
    }
    if (!edge) return true;
    int action = self->options_().overlay_captures_input ? 0 : NumberRowAction(key);
    if (!action) action = FKeyAction(key);
    if (action) self->request_lua_(action);
    return true;
  }

  // Hotkeys (request_lua_ codes, dispatched in keyboard_gamepad.h). Number row first (MacBook F keys need
  // fn); the F keys mirror it one to the right, starting with slow on F1:
  //   ` F1 slow (20), 1 F2 normal (21), 2 F3 2x (22), 3 F4 3x (23), 4 F5 4x (24), 5 F6 quick save (5),
  //   6 F7 script menu (6), 7 F8 turbo (7), 8 F9 achievements (8: 8 itself is the SDK bind_achievements),
  //   9 F10 FPS counter (9), 0 F11 frame generation (10).
  static int NumberRowAction(uint16_t key) {
    using K = rex::ui::VirtualKey;
    switch (K(key)) {
      case K::kOem3: return 20;
      case K::k1: return 21;
      case K::k2: return 22;
      case K::k3: return 23;
      case K::k4: return 24;
      case K::k5: return 5;
      case K::k6: return 6;
      case K::k7: return 7;
      case K::k9: return 9;
      case K::k0: return 10;
      default: return 0;
    }
  }
  static int FKeyAction(uint16_t key) {
    using K = rex::ui::VirtualKey;
    switch (K(key)) {
      case K::kF1: return 20;
      case K::kF2: return 21;
      case K::kF3: return 22;
      case K::kF4: return 23;
      case K::kF5: return 24;
      case K::kF6: return 5;
      case K::kF7: return 6;
      case K::kF8: return 7;
      case K::kF9: return 8;
      case K::kF10: return 9;
      case K::kF11: return 10;
      default: return 0;
    }
  }

  static uint16_t MouseKey(rex::ui::MouseEvent::Button button) {
    using B = rex::ui::MouseEvent::Button;
    using K = rex::ui::VirtualKey;
    switch (button) {
      case B::kLeft: return uint16_t(K::kLButton);
      case B::kRight: return uint16_t(K::kRButton);
      case B::kMiddle: return uint16_t(K::kMButton);
      case B::kX1: return uint16_t(K::kXButton1);
      case B::kX2: return uint16_t(K::kXButton2);
      default: return 0;
    }
  }
  bool SetKey(uint16_t key, bool down) {
    std::lock_guard lock(mutex_);
    if (!key || key >= snapshot_.keys.size()) return false;
    const bool edge = down && !snapshot_.keys[key];
    snapshot_.keys[key] = down;
    return edge;
  }
  void Clear() {
    std::lock_guard lock(mutex_);
    snapshot_ = {};
  }
  void Refresh() {
    if (!window_) return;
    const auto options = options_();
    // Closing an overlay with its close button must release the settings-key latch too.
    if (overlay_was_open_ && !options.overlay_captures_input)
      manual_release_ = false;
    overlay_was_open_ = options.overlay_captures_input;
    const bool active = focused_ && !options.overlay_captures_input;
    const bool looking = active && options.mouse_look && !manual_release_;
    if (looking != relative_) {
      relative_ = window_->SetRelativeMouseMode(looking);
      window_->SetCursorVisibility(relative_
          ? rex::ui::Window::CursorVisibility::kHidden
          : rex::ui::Window::CursorVisibility::kVisible);
      std::lock_guard lock(mutex_);
      snapshot_.dx = snapshot_.dy = 0;
    }
    std::lock_guard lock(mutex_);
    if (snapshot_.active != active) snapshot_ = {};
    snapshot_.active = active;
  }
  void Detach() {
    if (test_watch_) {
      SDL_RemoveEventWatch(TestKeyWatch, this);
      test_watch_ = false;
      std::lock_guard lock(mutex_);
      test_keys_ = {};
    }
    if (window_) {
      if (relative_) window_->SetRelativeMouseMode(false);
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      window_->RemoveInputListener(this);
      window_->RemoveListener(this);
      window_ = nullptr;
    }
    focused_ = relative_ = manual_release_ = overlay_was_open_ = false;
    Clear();
  }

  std::function<Options()> options_;
  std::function<void(int)> request_lua_;
  rex::ui::Window* window_ = nullptr;  // UI thread only
  rex::ui::WindowedAppContext* context_ = nullptr;
  bool focused_ = false, relative_ = false;
  bool manual_release_ = false, overlay_was_open_ = false;
  bool test_watch_ = false;  // UI thread only
  std::array<bool, 256> test_keys_{};  // guarded by mutex_
  std::atomic<bool> refresh_pending_{false};
  std::mutex mutex_;
  Snapshot snapshot_;
};

}  // namespace fable2::input_detail
