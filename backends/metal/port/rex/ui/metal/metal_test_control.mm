#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include "rex/ui/metal/metal_test_control.h"
#include "rex/ui/windowed_app_context.h"
#include "rex/ui/presenter.h"
#include "rex/ui/metal/metal_presenter.h"
#include "rex/graphics/metal/metal_trace.h"
#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_readiness.h"
#include <time.h>
#include "rex/logging.h"
#include "rex/cvar.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

namespace rex::ui::metal {
namespace {
int64_t MonotonicUs() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}
void Write(const std::filesystem::path& path, const std::string& text) {
  auto temporary = path; temporary += ".tmp";
  { std::ofstream file(temporary); file << text; }
  std::filesystem::rename(temporary, path);
}
SDL_Scancode Key(const std::string& name) {
  static const std::map<std::string, SDL_Scancode> keys = {
    {"e", SDL_SCANCODE_E}, {"x", SDL_SCANCODE_X}, {"z", SDL_SCANCODE_Z},
    {"c", SDL_SCANCODE_C}, {"w", SDL_SCANCODE_W}, {"a", SDL_SCANCODE_A},
    {"s", SDL_SCANCODE_S}, {"d", SDL_SCANCODE_D}, {"q", SDL_SCANCODE_Q},
    {"tab", SDL_SCANCODE_TAB}, {"escape", SDL_SCANCODE_ESCAPE},
    {"return", SDL_SCANCODE_RETURN}, {"up", SDL_SCANCODE_UP},
    {"down", SDL_SCANCODE_DOWN}, {"left", SDL_SCANCODE_LEFT},
    {"right", SDL_SCANCODE_RIGHT}, {"m", SDL_SCANCODE_M},
    {"i", SDL_SCANCODE_I}, {"j", SDL_SCANCODE_J},
    {"k", SDL_SCANCODE_K}, {"l", SDL_SCANCODE_L},
    {"5", SDL_SCANCODE_5}, {"6", SDL_SCANCODE_6},
    {"7", SDL_SCANCODE_7}, {"8", SDL_SCANCODE_8},
    {"f5", SDL_SCANCODE_F5}, {"f6", SDL_SCANCODE_F6}, {"f7", SDL_SCANCODE_F7},
    {"f8", SDL_SCANCODE_F8}, {"f9", SDL_SCANCODE_F9}};
  auto it = keys.find(name); return it == keys.end() ? SDL_SCANCODE_UNKNOWN : it->second;
}
// Background test mode (FABLE2_TEST_BACKGROUND=1, see src/input/test_background.h).
// Keep these in sync with fable2::testbg in that header.
constexpr Uint32 kTestKeyEventType = 0xF2B5;
constexpr Sint32 kTestKeyDownBit = 0x10000;
bool BackgroundMode() {
  static const bool on = [] { const char* v = std::getenv("FABLE2_TEST_BACKGROUND"); return v && v[0] == '1'; }();
  return on;
}
bool SendKey(SDL_Scancode key, bool down) {
  int count = 0; SDL_Window** windows = SDL_GetWindows(&count);
  Uint32 id = count ? SDL_GetWindowID(windows[0]) : 0;
  SDL_free(windows);
  if (!id) return false;
  if (BackgroundMode()) {
    // Synthetic test key: the app's keyboard driver takes it through an SDL
    // event watch, without the focus gate that applies to real keys.
    SDL_Event event{};
    event.type = kTestKeyEventType;
    event.user.windowID = id;
    event.user.code = Sint32(key) | (down ? kTestKeyDownBit : 0);
    return SDL_PushEvent(&event);
  }
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.windowID = id; event.key.scancode = key;
  event.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
  event.key.down = down;
  return SDL_PushEvent(&event);
}
// Controller buttons for the test channel: the app's controller hotkeys
// (src/input/controller_hotkeys.h) press them on a virtual SDL gamepad.
// Keep these in sync with fable2::padkeys in that header.
constexpr Uint32 kTestPadEventType = 0xF2B6;
constexpr Sint32 kTestPadDownBit = 0x10000;
constexpr Sint32 kTestPadPrepareBit = 0x20000;
constexpr Sint32 kTestPadAxisBit = 0x40000;  // code = SDL_GamepadAxis | this, user.data1 = value
int PadAxis(const std::string& name) {
  if (name == "lx") return SDL_GAMEPAD_AXIS_LEFTX;
  if (name == "ly") return SDL_GAMEPAD_AXIS_LEFTY;
  if (name == "rx") return SDL_GAMEPAD_AXIS_RIGHTX;
  if (name == "ry") return SDL_GAMEPAD_AXIS_RIGHTY;
  return -1;
}
bool SendPadAxis(int axis, int value) {
  SDL_Event event{};
  event.type = kTestPadEventType;
  event.user.code = Sint32(axis) | kTestPadAxisBit;
  event.user.data1 = reinterpret_cast<void*>(intptr_t(value));
  return SDL_PushEvent(&event);
}
int PadButton(const std::string& name) {
  if (name == "share") return SDL_GAMEPAD_BUTTON_MISC1;
  if (name == "view") return SDL_GAMEPAD_BUTTON_BACK;
  if (name == "ls") return SDL_GAMEPAD_BUTTON_LEFT_STICK;
  if (name == "rs") return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
  return -1;
}
bool SendPad(int button, Sint32 flags) {
  SDL_Event event{};
  event.type = kTestPadEventType;
  event.user.code = Sint32(button) | flags;
  return SDL_PushEvent(&event);
}
// Input-isolation check (local agent docs): push events exactly as SDL delivers real
// hardware input - a window focus gain then a genuine SDL key down/up, or a gamepad button from a
// device id that is not the test virtual pad. A test-only instance (FABLE2_PHYSICAL_INPUT=0) must
// ignore them; an instance with physical input on reacts (F7 toggles the turbo tag, Share asks
// for a quick save). The OS-level HID path can't be driven without Accessibility permission.
constexpr SDL_JoystickID kFakePhysicalPad = 0x7FFFFFF0;
bool SendPhysicalKey(SDL_Scancode key, bool down) {
  int count = 0; SDL_Window** windows = SDL_GetWindows(&count);
  Uint32 id = count ? SDL_GetWindowID(windows[0]) : 0;
  SDL_free(windows);
  if (!id) return false;
  if (down) {
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    focus.window.windowID = id;
    if (!SDL_PushEvent(&focus)) return false;
  }
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.windowID = id; event.key.scancode = key;
  event.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
  event.key.down = down;
  return SDL_PushEvent(&event);
}
bool SendPhysicalPad(int button, bool down) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
  event.gbutton.which = kFakePhysicalPad;
  event.gbutton.button = Uint8(button);
  event.gbutton.down = down;
  return SDL_PushEvent(&event);
}
// Writes <stem>.ppm + <stem>.capture.json in the control folder; returns the reply ("ok <path>" or an error).
// requested_frame: the `capture at` target (0 for a plain capture).
std::string WriteCapture(const std::filesystem::path& folder, const std::string& stem, const RawImage& image,
                         const MetalPresenter::CaptureTiming& timing, int64_t started, int64_t completed, bool screen,
                         uint64_t requested_frame) {
  if (!(image.width && image.height && image.stride >= image.width * 4 &&
        image.data.size() >= size_t(image.stride) * image.height)) {
    return "error capture image invalid";
  }
  auto image_path = folder / (stem + ".ppm");
  std::ofstream output(image_path, std::ios::binary);
  output << "P6\n" << image.width << " " << image.height << "\n255\n";
  for (uint32_t y = 0; y < image.height; ++y) for (uint32_t x = 0; x < image.width; ++x) {
    output.write(reinterpret_cast<const char*>(image.data.data() + size_t(y) * image.stride + x * 4), 3);
  }
  if (!output) return "error capture write failed";
  output.close();
  const char* origin_value = std::getenv("FABLE2_SESSION_ORIGIN_US");  // getenv-ok: per test capture
  int64_t origin = origin_value ? std::strtoll(origin_value, nullptr, 10) : 0;
  std::ostringstream metadata;
  metadata << "{\"schema\":1,\"clock\":\"CLOCK_MONOTONIC\","
           << "\"source\":\"" << (screen ? "screen" : "guest") << "\","
           << "\"frame_timestamp_semantics\":\"" << (screen ? "drawable after ImGui overlay; frame_id = recorder frame at paint" : "guest_output_submitted; not GPU completion or display time") << "\","
           << "\"frame_submitted_monotonic_us\":" << timing.frame_submitted_monotonic_us
           << ",\"frame_id\":" << timing.frame_id
           << ",\"submission_id\":" << timing.submission_id;
  if (requested_frame) metadata << ",\"requested_frame\":" << requested_frame;
  metadata << ",\"capture_started_monotonic_us\":" << started
           << ",\"capture_completed_monotonic_us\":" << completed
           << ",\"session_origin_monotonic_us\":";
  if (origin) metadata << origin; else metadata << "null";
  metadata << ",\"frame_t_us\":";
  if (origin && timing.frame_submitted_monotonic_us) metadata << timing.frame_submitted_monotonic_us - origin;
  else metadata << "null";
  metadata << ",\"capture_started_t_us\":";
  if (origin) metadata << started - origin; else metadata << "null";
  metadata << ",\"capture_completed_t_us\":";
  if (origin) metadata << completed - origin; else metadata << "null";
  metadata << "}\n";
  Write(folder / (stem + ".capture.json"), metadata.str());
  return "ok " + image_path.string();
}
}  // namespace
void StartMetalTestControl(WindowedAppContext* context, Presenter* presenter) {
  const char* value = std::getenv("FABLE2_TEST_CONTROL_DIR");  // getenv-ok: once, at start
  if (!value || !*value || !context || !presenter) return;
  std::filesystem::path folder(value);
  if (!folder.is_absolute()) { REXLOG_WARN("Metal test control requires an absolute directory"); return; }
  static std::once_flag once;
  std::call_once(once, [context, presenter, folder] {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) { REXLOG_WARN("Metal test control: directory unavailable"); return; }
    chmod(folder.c_str(), 0700);
    Write(folder / "pid.txt", std::to_string(getpid()));
    REXLOG_INFO("Metal test control: {} (app-owned captures/input; no accessibility API)", folder.string());
    if (BackgroundMode()) {
      context->CallInUIThreadDeferred([] {
        // Keep frames advancing while hidden/occluded unless the test asks to
        // measure App Nap (FABLE2_TEST_APP_NAP=1). The token lives for the process.
        const char* nap = std::getenv("FABLE2_TEST_APP_NAP");  // getenv-ok: once, at start
        static id activity = nil;
        if (!(nap && nap[0] == '1') && !activity) {
          activity = [[NSProcessInfo processInfo]
              beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                                reason:@"Fable II background test session"];
#if !__has_feature(objc_arc)
          [activity retain];  // held for the life of the process
#endif
        }
        // The window must not cover the user's frontmost app.
        if (!NSApp.active) {
          for (NSWindow* window in NSApp.windows) {
            if (window.visible) [window orderBack:nil];
          }
        }
        const char* bg_hint = SDL_GetHint(SDL_HINT_MAC_BACKGROUND_APP);
        const char* shown_hint = SDL_GetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN);
        REXLOG_INFO("Metal test control: background mode (app active={}, app nap exemption={}, occlusion visible={}, "
                    "policy={}, hints bg={} shown={})",
                    bool(NSApp.active), activity != nil,
                    bool(NSApp.occlusionState & NSApplicationOcclusionStateVisible), int(NSApp.activationPolicy),
                    bg_hint ? bg_hint : "null", shown_hint ? shown_hint : "null");
      });
    }
    std::thread([context, presenter, folder] {
      while (true) {
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
          if (entry.path().extension() != ".req") continue;
          auto stem = entry.path().stem().string();
          if (stem.find_first_not_of("0123456789abcdef-") != std::string::npos) continue;
          std::string text;
          { std::ifstream request(entry.path()); std::stringstream ss; ss << request.rdbuf(); text = ss.str(); }
          std::istringstream request(text);
          std::string command, argument, rest;
          request >> command >> argument;
          std::getline(request, rest);  // the remainder (hold ms, dump reason, mark text, tag)
          if (const size_t b = rest.find_first_not_of(' '); b != std::string::npos) rest = rest.substr(b); else rest.clear();
          const int duration = std::atoi(rest.c_str());
          auto response = folder / (stem + ".reply");
          std::string result = "error unsupported command";
          const Fable2FrApi* api = graphics::metal::trace::Api();
          if (command == "close") {
            // Flight recorder "exit" dump before quitting (the SDK hard-exits afterwards).
            if (api) { char path[1024] = {}; api->dump_sync("exit", "test-control close", path, sizeof path, 3000); }
            Write(response, "ok closing");
            context->CallInUIThreadDeferred([] { SDL_Event event{}; event.type = SDL_EVENT_QUIT; SDL_PushEvent(&event); });
            std::filesystem::remove(entry.path()); return;
          } else if (command == "focus") {
            bool success = context->CallInUIThreadSynchronous([] {
              [NSApp activateIgnoringOtherApps:YES];
              NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow;
              if (!window && NSApp.windows.count) window = NSApp.windows[0];
              [window makeKeyAndOrderFront:nil];
            });
            result = success ? "ok focused" : "error UI unavailable";
          } else if (command == "stick") {
            // stick lx|ly|rx|ry <value>: hold a virtual-pad stick axis at value (SDL units -32768..32767, ly/ry
            // positive = down as SDL reports it; the game sees ly/ry inverted). It stays until set again.
            const int axis = PadAxis(argument);
            char* end = nullptr;
            const long value = std::strtol(rest.c_str(), &end, 10);
            if (axis < 0 || rest.empty() || *end || value < -32768 || value > 32767) {
              result = "error usage: stick lx|ly|rx|ry <-32768..32767>";
            } else {
              bool sent = false;
              context->CallInUIThreadSynchronous([&] { sent = SendPadAxis(axis, int(value)); });
              result = sent ? "ok stick " + argument + " " + std::to_string(value) : "error pad event delivery failed";
            }
          } else if ((command == "press" || command == "down" || command == "up") && PadButton(argument) >= 0) {
            const int button = PadButton(argument);
            if (command != "press") {
              bool sent = false;
              context->CallInUIThreadSynchronous([&] {
                sent = SendPad(button, kTestPadPrepareBit) && SendPad(button, command == "down" ? kTestPadDownBit : 0);
              });
              result = sent ? "ok " + command + " " + argument : "error pad event delivery failed";
            } else if (duration >= 20 && duration <= 10000) {
              // Non-blocking: the hold runs on its own thread, which writes the reply when the
              // button is released; captures and other commands keep running meanwhile.
              std::filesystem::remove(entry.path());
              std::thread([context, button, duration, argument, response] {
                bool prepared = false, down = false, up = false;
                // The first press attaches the virtual pad; give the game's input
                // driver a moment to open it before the button goes down.
                context->CallInUIThreadSynchronous([&] { prepared = SendPad(button, kTestPadPrepareBit); });
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                context->CallInUIThreadSynchronous([&] { down = SendPad(button, kTestPadDownBit); });
                std::this_thread::sleep_for(std::chrono::milliseconds(duration));
                context->CallInUIThreadSynchronous([&] { up = SendPad(button, 0); });
                Write(response, prepared && down && up ? "ok pressed " + argument : "error pad event delivery failed");
              }).detach();
              continue;
            } else result = "error invalid hold duration (20..10000ms)";
          } else if (command == "down" || command == "up") {
            auto key = Key(argument);
            if (key != SDL_SCANCODE_UNKNOWN) {
              bool sent = false;
              context->CallInUIThreadSynchronous([&] { sent = SendKey(key, command == "down"); });
              result = sent ? "ok " + command + " " + argument : "error key delivery failed";
            } else result = "error invalid key";
          } else if (command == "press") {
            auto key = Key(argument);
            if (key != SDL_SCANCODE_UNKNOWN && duration >= 20 && duration <= 10000) {
              // Non-blocking (see the pad press above).
              std::filesystem::remove(entry.path());
              std::thread([context, key, duration, argument, response] {
                bool down = false, up = false;
                context->CallInUIThreadSynchronous([&] { down = SendKey(key, true); });
                std::this_thread::sleep_for(std::chrono::milliseconds(duration));
                context->CallInUIThreadSynchronous([&] { up = SendKey(key, false); });
                Write(response, down && up ? "ok pressed " + argument : "error key delivery failed");
              }).detach();
              continue;
            } else result = "error invalid key or hold duration (20..10000ms)";
          } else if (command == "capture" && argument == "at") {
            // capture at <frame> [timeout_ms] [count] [step]: the guest output of guest frames <frame>,
            // <frame>+step, ... exactly (frame-matched parity, docs/readiness.md). Non-blocking like press: it waits
            // on its own thread, so `state` and other commands keep working. Reply "ok <ppm> [<ppm> ...]" when all
            // were captured; "error passed <current>" when the first frame is already gone; "error timeout
            // <current>" (or "error partial <current>") with the paths of what was captured.
            std::istringstream at(rest);
            unsigned long long target = 0; int timeout = 30000, count = 1, step = 1;
            at >> target >> timeout >> count >> step;
            if (!target || count < 1 || count > 64 || step < 1) {
              result = "error usage: capture at <frame> [timeout_ms] [count 1..64] [step >= 1]";
            } else {
              std::filesystem::remove(entry.path());
              std::thread([presenter, folder, stem, response, target, timeout, count, step] {
                std::vector<uint64_t> targets;
                for (int i = 0; i < count; ++i) targets.push_back(target + uint64_t(i) * uint64_t(step));
                std::vector<MetalPresenter::FrameCapture> captures;
                uint64_t current = 0;
                const int64_t started = MonotonicUs();
                const int got = static_cast<MetalPresenter*>(presenter)->CaptureGuestOutputAtFrames(
                    targets, uint32_t(std::clamp(timeout, 1, 600000)), captures, &current);
                const int64_t completed = MonotonicUs();
                std::string paths;
                for (size_t i = 0; i < captures.size(); ++i) {
                  const std::string name = count == 1 ? stem : stem + "_" + std::to_string(i);
                  std::string one = WriteCapture(folder, name, captures[i].image, captures[i].timing, started,
                                                 completed, false, captures[i].requested_frame);
                  if (one.rfind("ok ", 0) == 0) paths += " " + one.substr(3);
                }
                std::string reply;
                if (got > 0 && captures.size() == targets.size()) reply = "ok" + paths;
                else if (got == 0) reply = "error timeout " + std::to_string(current) + paths;
                else if (captures.empty() && current >= targets.front()) reply = "error passed " + std::to_string(current);
                else reply = "error partial " + std::to_string(current) + paths;
                Write(response, reply);
              }).detach();
              continue;
            }
          } else if (command == "capture") {
            // Client controls no arbitrary output path: capture stays in this session.
            // `capture` = guest frame only; `capture screen` = presented drawable incl. ImGui overlay.
            const bool screen = argument == "screen";
            RawImage image;
            bool captured = false;
            MetalPresenter::CaptureTiming timing;
            int64_t started = 0, completed = 0;
            if (screen) {
              started = MonotonicUs();
              uint64_t frame = 0;
              captured = static_cast<MetalPresenter*>(presenter)->CaptureScreen(image, 3000, &frame);
              completed = MonotonicUs();
              timing.frame_id = frame;
            } else {
              context->CallInUIThreadSynchronous([&] {
                started = MonotonicUs();
                captured = static_cast<MetalPresenter*>(presenter)->CaptureGuestOutput(image, &timing);
                completed = MonotonicUs();
              });
            }
            if (captured) result = WriteCapture(folder, stem, image, timing, started, completed, screen, 0);
            else result = screen ? "error screen capture unavailable (no paint within 3 s)" : "error no rendered output yet";
          } else if (command == "trace") {
            // trace dump [reason] | trace mark <text> | trace tag <key>=<value>
            if (!api) result = "error flight recorder unavailable (FABLE2_TRACE=off?)";
            else if (argument == "dump") {
              std::string reason = "user";
              std::string extra = rest.substr(0, rest.find(' '));
              for (char& c : extra) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
              if (!extra.empty()) reason = "user-" + extra.substr(0, 40);
              char path[1024] = {};
              result = api->dump_sync(reason.c_str(), "test-control trace dump", path, sizeof path, 15000)
                           ? std::string("ok ") + path : "error dump failed (recorder busy or off)";
            } else if (argument == "mark") {
              api->mark(rest.c_str());
              result = "ok marked";
            } else if (argument == "tag") {
              api->tag(rest.c_str());
              result = "ok tagged";
            } else result = "error usage: trace dump [reason] | trace mark <text> | trace tag <key>=<value>";
          } else if (command == "state") {
            if (!api) result = "error flight recorder unavailable";
            else {
              char json[4096] = {};
              api->state_json(json, sizeof json);
              std::string state(json);
              // [ready] readiness detail (docs/readiness.md) beside the recorder's tags.
              if (const size_t end = state.rfind('}'); end != std::string::npos) {
                state.insert(end, ",\"ready\":" + graphics::metal::readiness::StateJson());
              }
              result = "ok " + state;
            }
          } else if (command == "flush") {
            // flush [timeout_ms]: write the frame CSV now; replies with the flushed frame and CSV time.
            const int timeout = argument.empty() ? 2000 : std::max(1, std::atoi(argument.c_str()));
            const bool done = graphics::metal::frame_log::RequestFlush(timeout);
            result = done ? "ok {\"flushed_frame\":" + std::to_string(graphics::metal::frame_log::FlushedFrame()) +
                                ",\"flushed_us\":" + std::to_string(graphics::metal::frame_log::FlushedUs()) + "}"
                          : std::string("error frame log flush timed out (frame logging off?)");
          } else if (command == "window") {
            // Test sessions only (display options, docs/resolution-scale.md):
            //   window frame | window size <w> <h> | window move <x> <y> | window fullscreen 0|1
            // size/move act on the SDL window like a user drag; fullscreen sets the SDK cvar like
            // the settings screen. The reply is the frame afterwards (points).
            int a = 0, b = 0;
            std::istringstream numbers(rest);
            const bool two = bool(numbers >> a >> b);
            const bool known = argument == "frame" || ((argument == "size" || argument == "move") && two) ||
                               (argument == "fullscreen" && (rest == "0" || rest == "1"));
            if (!known) {
              result = "error usage: window frame | size <w> <h> | move <x> <y> | fullscreen 0|1";
            } else {
              if (argument == "fullscreen") rex::cvar::SetFlagByName("fullscreen", rest == "1" ? "true" : "false");
              bool ok = context->CallInUIThreadSynchronous([&] {
                int count = 0; SDL_Window** windows = SDL_GetWindows(&count);
                SDL_Window* window = count ? windows[0] : nullptr;
                SDL_free(windows);
                if (!window) { result = "error no window"; return; }
                if (argument == "size") SDL_SetWindowSize(window, a, b);
                if (argument == "move") SDL_SetWindowPosition(window, a, b);
                if (argument == "size" || argument == "move") SDL_SyncWindow(window);
                int x = 0, y = 0, w = 0, h = 0;
                SDL_GetWindowPosition(window, &x, &y);
                SDL_GetWindowSize(window, &w, &h);
                const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
                result = "ok x=" + std::to_string(x) + " y=" + std::to_string(y) + " w=" + std::to_string(w) +
                         " h=" + std::to_string(h) + " fullscreen=" + std::to_string(bool(flags & SDL_WINDOW_FULLSCREEN)) +
                         " maximized=" + std::to_string(bool(flags & SDL_WINDOW_MAXIMIZED));
              });
              if (!ok) result = "error UI unavailable";
            }
          } else if (command == "physical") {
            // `physical key <name>` / `physical pad share|view|ls|rs` (see SendPhysicalKey above).
            std::string kind = argument, name = rest;
            bool sent = false;
            if (kind == "key" && Key(name) != SDL_SCANCODE_UNKNOWN) {
              const auto key = Key(name);
              context->CallInUIThreadSynchronous([&] { sent = SendPhysicalKey(key, true); });
              std::this_thread::sleep_for(std::chrono::milliseconds(150));
              context->CallInUIThreadSynchronous([&] { sent = SendPhysicalKey(key, false) && sent; });
              result = sent ? "ok physical key " + name : "error delivery failed";
            } else if (kind == "pad" && PadButton(name) >= 0) {
              const int button = PadButton(name);
              context->CallInUIThreadSynchronous([&] { sent = SendPhysicalPad(button, true); });
              std::this_thread::sleep_for(std::chrono::milliseconds(150));
              context->CallInUIThreadSynchronous([&] { sent = SendPhysicalPad(button, false) && sent; });
              result = sent ? "ok physical pad " + name : "error delivery failed";
            } else if (kind == "close-window" || kind == "quit") {
              // The user's own ways out, through AppKit exactly as they arrive: performClose: is what
              // the red close button sends; terminate: is what Cmd+Q and Dock > Quit end in.
              Write(response, "ok physical " + kind);
              const bool quit = kind == "quit";
              context->CallInUIThreadDeferred([quit] {
                if (quit) { [NSApp terminate:nil]; return; }
                NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow;
                if (!window) for (NSWindow* w in NSApp.windows) if (w.visible) { window = w; break; }
                [window performClose:nil];
              });
              std::filesystem::remove(entry.path()); continue;
            } else result = "error usage: physical key <name> | physical pad share|view|ls|rs | physical close-window|quit";
          } else if (command == "ping") result = "ok ready";
          Write(response, result);
          std::filesystem::remove(entry.path());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }).detach();
  });
}
}  // namespace rex::ui::metal
