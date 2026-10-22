#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace fable2::f5lua {
inline int requests = 0;
inline void request_run(int) { ++requests; }
}
#include "keyboard_gamepad.h"
#include "fable2_config.h"
#include "recomp_hotkeys.h"
#include <rex/ui/sdl_virtual_key.h>

// Link stubs for headers pulled in by keyboard_gamepad.h (turbo, flight recorder).
namespace fable2::fr { void SetTag(Tag, int64_t, const char*) {} }
namespace fable2::config { const Values& Get() { static Values values; return values; } }

REXCVAR_DEFINE_STRING(keyboard_gamepad_map, "", "Input", "Test map");
REXCVAR_DEFINE_BOOL(mouse_look, true, "Input", "Test mouse");
REXCVAR_DEFINE_INT32(mouse_look_scale, 256, "Input", "Test sensitivity");
REXCVAR_DEFINE_STRING(mouse_unlock_key, "", "Input", "Test unlock");

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
  std::abort(); } } while (false)

// A real SDK Window event dispatcher with no desktop dependency. Cursor
// operations assert UI-thread affinity; input polling also runs off-thread.
class Context : public rex::ui::WindowedAppContext {
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};
class Window : public rex::ui::Window {
 public:
  explicit Window(Context& context) : rex::ui::Window(context, "Input test", 1280, 720) {}
  ~Window() override { EnterDestructor(); }
  bool relative = false;
  bool relative_supported = true;
  bool SetRelativeMouseMode(bool enable) override {
    CHECK(app_context().IsInUIThread());
    relative = enable && relative_supported;
    return relative;
  }
  void Focused(bool focused) {
    WindowDestructionReceiver receiver(this);
    OnFocusUpdate(focused, receiver);
  }
  void Key(rex::ui::VirtualKey key, bool down, bool repeat = false,
           bool shift_still_held = false) {
    rex::ui::KeyEvent event(this, key, 1, repeat, shift_still_held, false, false, false);
    WindowDestructionReceiver receiver(this);
    if (down) OnKeyDown(event, receiver); else OnKeyUp(event, receiver);
  }
  void Motion(float dx, float dy) {
    rex::ui::MouseEvent event(this, rex::ui::MouseEvent::Button::kNone, 0, 0, 0, 0, dx, dy);
    WindowDestructionReceiver receiver(this);
    OnMouseMove(event, receiver);
  }
  void Button(rex::ui::MouseEvent::Button button, bool down) {
    rex::ui::MouseEvent event(this, button, 0, 0);
    WindowDestructionReceiver receiver(this);
    if (down) OnMouseDown(event, receiver); else OnMouseUp(event, receiver);
  }
 protected:
  bool OpenImpl() override { return true; }
  void RequestCloseImpl() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};

int main() {
  using rex::X_RESULT;
  using K = rex::ui::VirtualKey;
  using B = rex::input::X_INPUT_GAMEPAD_BUTTON;
  using MB = rex::ui::MouseEvent::Button;
  Context context;
  Window window(context);
  CHECK(window.Open());
  int debug_requests = 0, settings_requests = 0, achievement_requests = 0, console_requests = 0;
  rex::ui::RegisterBind("bind_console", "Backtick", "Test console", [&] { ++console_requests; });
  rex::ui::RegisterBind("bind_debug_overlay", "F3", "Test debug", [&] { ++debug_requests; });
  rex::ui::RegisterBind("bind_settings", "F4", "Test settings", [&] { ++settings_requests; });
  rex::ui::RegisterBind("bind_achievements", "F7", "Test achievements", [&] { ++achievement_requests; });
  // fable_2.toml from an older build saved its defaults (console Backtick, debug overlay 3, achievements 1,
  // unlock 4); bind_settings stays untouched (compiled-in default).
  const auto toml = std::filesystem::temp_directory_path() / "fable2_input_tests_hotkeys.toml";
  {
    std::FILE* f = std::fopen(toml.c_str(), "w");
    CHECK(f);
    std::fputs("bind_console = \"Backtick\"\nbind_debug_overlay = \"3\"\nbind_achievements = \"1\"\n"
               "mouse_unlock_key = \"4\"\n", f);
    std::fclose(f);
  }
  rex::cvar::LoadConfig(toml);
  std::filesystem::remove(toml);
  CHECK(rex::cvar::GetFlagSource("bind_achievements") == rex::cvar::Source::kConfig);
  fable2::ApplyRecompHotkeyDefaults();
  CHECK(rex::cvar::GetFlagByName("bind_debug_overlay") == "None");
  CHECK(rex::cvar::GetFlagByName("bind_settings") == "None");
  CHECK(rex::cvar::GetFlagByName("bind_console") == "None");
  CHECK(rex::cvar::GetFlagByName("bind_achievements") == "8");
  CHECK(REXCVAR_GET(mouse_unlock_key).empty());
  // No SDK bind on the speed keys (` 1-4); 8 opens achievements.
  for (auto key : {K::kOem3, K::k1, K::k2, K::k3, K::k4}) {
    rex::ui::KeyEvent free_event(&window, key, 1, false, false, false, false, false);
    CHECK(!rex::ui::ProcessKeyEvent(free_event));
  }
  rex::ui::KeyEvent achievement_event(&window, K::k8, 1, false, false, false, false, false);
  CHECK(rex::ui::ProcessKeyEvent(achievement_event) && achievement_requests == 1);
  CHECK(debug_requests == 0 && settings_requests == 0 && console_requests == 0);
  // F9 is the achievements key's F-key twin.
  fable2::hotkey_hooks::OpenAchievements();
  CHECK(achievement_requests == 2);
  rex::ui::KeyEvent old_event(&window, K::kF3, 1, false, false, false, false, false);
  CHECK(!rex::ui::ProcessKeyEvent(old_event));
  CHECK(rex::cvar::SetFlagByName("bind_debug_overlay", "F8"));
  fable2::ApplyRecompHotkeyDefaults();
  rex::ui::KeyEvent custom_event(&window, K::kF8, 1, false, false, false, false, false);
  CHECK(rex::ui::ProcessKeyEvent(custom_event) && debug_requests == 1);
  for (auto name : {"bind_console", "bind_debug_overlay", "bind_settings", "bind_achievements"})
    rex::ui::UnregisterBind(name);
  // A saved old default (fable_2.toml) moves to the new default on the first refresh.
  REXCVAR_SET(keyboard_gamepad_map, std::string(fable2::config::kOldDefaultKeyboardGamepadMap));
  {
    fable2::KeyboardGamepadDriver driver(nullptr, 0);
    driver.OnWindowAvailable(&window);
    std::vector<rex::input::DeviceInfo> devices;
    driver.EnumerateDevices(devices);
    CHECK(REXCVAR_GET(keyboard_gamepad_map) == fable2::config::kDefaultKeyboardGamepadMap);
    CHECK(devices.size() == 1 && devices[0].synthetic);
    auto poll = [&] {
      rex::input::X_INPUT_STATE state{};
      CHECK(driver.GetDeviceState(devices[0].id, &state) == X_ERROR_SUCCESS);
      return state;
    };
    CHECK(poll().gamepad.buttons == 0);
    window.Focused(true);
    CHECK(window.relative);
    for (const auto& binding : {std::pair{K::kZ, B::X_INPUT_GAMEPAD_X},
                                std::pair{K::kX, B::X_INPUT_GAMEPAD_B},
                                std::pair{K::kC, B::X_INPUT_GAMEPAD_Y},
                                std::pair{K::kI, B::X_INPUT_GAMEPAD_DPAD_UP},
                                std::pair{K::kK, B::X_INPUT_GAMEPAD_DPAD_DOWN},
                                std::pair{K::kJ, B::X_INPUT_GAMEPAD_DPAD_LEFT},
                                std::pair{K::kL, B::X_INPUT_GAMEPAD_DPAD_RIGHT}}) {
      window.Key(binding.first, true);
      CHECK(poll().gamepad.buttons == binding.second);
      window.Key(binding.first, false);
    }
    for (auto key : {K::k8, K::kF12}) {  // the number row and F1-F11 are hotkeys
      window.Key(key, true);
      CHECK(poll().gamepad.buttons == 0);
      window.Key(key, false);
    }
    CHECK(fable2::f5lua::requests == 0);
    window.Key(K::kE, true);
    window.Key(K::kW, true);
    window.Key(K::kD, true);
    window.Key(K::kQ, true);
    window.Key(K::kTab, true);
    auto state = poll();
    CHECK(state.gamepad.buttons & B::X_INPUT_GAMEPAD_A);
    CHECK(state.gamepad.thumb_ly == 32767 && state.gamepad.thumb_lx == 32767);
    CHECK(state.gamepad.left_trigger == 255 && state.gamepad.right_trigger == 255);
    window.Key(K::kS, true);
    window.Key(K::kA, true);
    CHECK(poll().gamepad.thumb_ly == 0 && poll().gamepad.thumb_lx == 0);
    window.Key(K::kE, false);
    CHECK(poll().gamepad.buttons == 0);
    window.Motion(0.25f, -0.5f);
    window.Motion(0.75f, -0.5f);
    state = poll();
    CHECK(state.gamepad.thumb_rx == 256 && state.gamepad.thumb_ry == 256);
    CHECK(poll().gamepad.thumb_rx == 0 && poll().gamepad.thumb_ry == 0);
    // Arrow keys: right stick, full deflection, no buttons; the mouse adds to them.
    window.Key(K::kUp, true);
    window.Key(K::kLeft, true);
    state = poll();
    CHECK(state.gamepad.buttons == 0 && state.gamepad.thumb_lx == 0 && state.gamepad.thumb_ly == 0);
    CHECK(state.gamepad.thumb_rx == -32767 && state.gamepad.thumb_ry == 32767);
    window.Motion(1.0f, 1.0f);
    state = poll();
    CHECK(state.gamepad.thumb_rx == -32767 + 256 && state.gamepad.thumb_ry == 32767 - 256);
    window.Motion(-1e8f, -1e8f);
    state = poll();
    CHECK(state.gamepad.thumb_rx == -32767 && state.gamepad.thumb_ry == 32767);
    window.Key(K::kRight, true);
    window.Key(K::kDown, true);
    state = poll();
    CHECK(state.gamepad.thumb_rx == 0 && state.gamepad.thumb_ry == 0);
    for (auto key : {K::kUp, K::kLeft, K::kRight, K::kDown}) window.Key(key, false);
    CHECK(poll().gamepad.thumb_rx == 0);
    window.Motion(1e8f, 1e8f);
    state = poll();
    CHECK(state.gamepad.thumb_rx == 32767 && state.gamepad.thumb_ry == -32767);
    // No mouse-unlock key by default: 8 (achievements) keeps the mouse captured.
    window.Key(K::k8, true);
    CHECK(window.relative);
    window.Key(K::k8, false);
    poll();
    // A bound settings key (the launcher passes it as mouse_unlock_key too) toggles the release.
    REXCVAR_SET(mouse_unlock_key, std::string("F12"));
    window.Key(K::kF12, true);
    CHECK(!window.relative);
    window.Key(K::kF12, true, true);
    CHECK(!window.relative);
    CHECK(!(poll().gamepad.buttons & B::X_INPUT_GAMEPAD_DPAD_RIGHT));
    window.Motion(50, 50);
    CHECK(poll().gamepad.thumb_rx == 0);
    window.Key(K::kF12, false);
    window.Key(K::kF12, true);
    CHECK(window.relative);
    window.Key(K::kF12, false);
    REXCVAR_SET(mouse_unlock_key, std::string());
    window.Key(K::k5, true);
    window.Key(K::k5, true, true);
    CHECK(fable2::f5lua::requests == 1);
    window.Key(K::k5, false);
    window.Key(K::k5, true);
    CHECK(fable2::f5lua::requests == 2);
    window.Motion(20, 20);
    window.Focused(false);
    CHECK(!window.relative);
    state = poll();
    CHECK(state.gamepad.buttons == 0 && state.gamepad.thumb_lx == 0);
    CHECK(state.gamepad.left_trigger == 0 && state.gamepad.thumb_rx == 0);
    window.Key(K::kE, true);
    window.Key(K::k5, true);
    CHECK(fable2::f5lua::requests == 2);
    window.Focused(true);
    CHECK(poll().gamepad.buttons == 0 && poll().gamepad.thumb_ly == 0);
    REXCVAR_SET(keyboard_gamepad_map, "Space:B,Shift:LT,Control:RT,LMB:X,invalid:A");
    window.Key(K::kSpace, true);
    const auto shift = rex::ui::TranslateSDLScancode(SDL_SCANCODE_LSHIFT);
    window.Key(shift, true);
    window.Key(rex::ui::TranslateSDLScancode(SDL_SCANCODE_RCTRL), true);
    window.Button(MB::kLeft, true);
    state = poll();
    CHECK(state.gamepad.buttons == (B::X_INPUT_GAMEPAD_B | B::X_INPUT_GAMEPAD_X));
    CHECK(state.gamepad.left_trigger == 255 && state.gamepad.right_trigger == 255);
    window.Key(shift, false, false, true);
    CHECK(poll().gamepad.left_trigger == 255);
    window.Key(shift, false);
    CHECK(poll().gamepad.left_trigger == 0);
    window.Key(K::kControl, false);
    window.Button(MB::kLeft, false);
    CHECK(poll().gamepad.right_trigger == 0);
    CHECK(poll().gamepad.buttons == B::X_INPUT_GAMEPAD_B);
    REXCVAR_SET(mouse_look, false);
    poll();
    CHECK(!window.relative);
    window.Motion(30, 30);
    CHECK(poll().gamepad.thumb_rx == 0);
    REXCVAR_SET(mouse_look, true);
    // UI work queued by the guest thread must release the cursor on teardown.
    std::thread guest([&] { poll(); });
    guest.join();
    context.ExecutePendingFunctionsFromUIThread();
    CHECK(window.relative);
  }
  CHECK(!window.relative);

  using Input = fable2::input_detail::WindowKeyboardMouse;
  Input::Options options{true, false, uint16_t(K::kF12)};
  int overlay_lua_requests = 0, last_action = 0;
  auto input = std::make_shared<Input>([&] { return options; },
                                       [&](int action) { ++overlay_lua_requests; last_action = action; });
  input->Attach(&window);
  CHECK(window.relative);
  window.Key(K::kE, true);
  options.overlay_captures_input = true;
  input->RequestRefresh();
  CHECK(!window.relative && !input->Consume().active);
  window.Key(K::kW, true);
  window.Key(K::k5, true);
  window.Key(K::k5, false);
  window.Key(K::k6, true);
  window.Key(K::k6, false);
  CHECK(overlay_lua_requests == 0);
  window.Key(K::kOem3, true);  // ` (slow) is a typed character too
  window.Key(K::kOem3, false);
  window.Key(K::k2, true);
  window.Key(K::k2, false);
  CHECK(overlay_lua_requests == 0);
  // F keys act while an overlay has the keyboard (never text): F7 opens/closes the script menu.
  window.Key(K::kF7, true);
  window.Key(K::kF7, false);
  CHECK(overlay_lua_requests == 1 && last_action == 6);
  options.overlay_captures_input = false;
  input->RequestRefresh();
  auto snapshot = input->Consume();
  CHECK(window.relative && snapshot.active);
  CHECK(!snapshot.Down(uint16_t(K::kE)) && !snapshot.Down(uint16_t(K::kW)));
  // Hotkey codes (dispatched in keyboard_gamepad.h): the number row, and the F keys one to the right.
  for (const auto& [key, action] : {std::pair{K::kOem3, 20}, std::pair{K::k1, 21}, std::pair{K::k2, 22},
                                    std::pair{K::k3, 23}, std::pair{K::k4, 24}, std::pair{K::k5, 5},
                                    std::pair{K::k6, 6}, std::pair{K::k7, 7}, std::pair{K::k9, 9},
                                    std::pair{K::k0, 10}, std::pair{K::kF1, 20}, std::pair{K::kF2, 21},
                                    std::pair{K::kF3, 22}, std::pair{K::kF4, 23}, std::pair{K::kF5, 24},
                                    std::pair{K::kF6, 5}, std::pair{K::kF7, 6}, std::pair{K::kF8, 7},
                                    std::pair{K::kF9, 8}, std::pair{K::kF10, 9}, std::pair{K::kF11, 10}}) {
    last_action = 0;
    window.Key(key, true);
    window.Key(key, false);
    CHECK(last_action == action);
  }
  last_action = 0;
  window.Key(K::k8, true);  // 8 is the SDK achievements bind, not a recomp hotkey
  window.Key(K::k8, false);
  CHECK(last_action == 0);
  // Closing the settings UI with its mouse close button also clears the settings-key latch.
  window.Key(K::kF12, true);
  window.Key(K::kF12, false);
  options.overlay_captures_input = true;
  input->RequestRefresh();
  options.overlay_captures_input = false;
  input->RequestRefresh();
  CHECK(window.relative);
  double total = 0;
  std::atomic<bool> done{false};
  std::thread consumer([&] {
    while (!done) total += input->Consume().dx;
    total += input->Consume().dx;
  });
  for (int i = 0; i < 1000; ++i) window.Motion(0.25f, 0);
  done = true;
  consumer.join();
  CHECK(total == 250);
  std::thread detach([&] { input->RequestRefresh(); input->RequestDetach(); });
  detach.join();
  input.reset();
  context.ExecutePendingFunctionsFromUIThread();
  CHECK(!window.relative);
  CHECK(!fable2::config::Values{}.mouse_look);
  auto free_cursor = std::make_shared<Input>([] { return Input::Options{}; }, [](int) {});
  free_cursor->Attach(&window);
  window.Focused(true);
  CHECK(!window.relative);
  window.Motion(12, 8);
  auto free_snapshot = free_cursor->Consume();
  CHECK(free_snapshot.active && free_snapshot.dx == 0 && free_snapshot.dy == 0);
  window.Focused(false);
  window.Focused(true);
  CHECK(!window.relative);
  free_cursor->RequestDetach();
  context.ExecutePendingFunctionsFromUIThread();
  std::puts("Keyboard/mouse regression checks passed.");
}
