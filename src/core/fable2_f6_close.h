// F6 closes the F6 menu (src/lua/F6.lua): a short B tap on the open game menu box.
//
// The game's menu box has no close call (GUI.DisplayMenuBox returns no handle), so the menu dismisses it
// the way a player does. The Lua RunScript()s the no-op F6_close_box.lua; the RunScript dispatcher probe
// (src/core/fable2_f5_lua.h) sees the path and calls Request(), and the next guest pad reads of user 0
// (the XamInputGetState wrapper, src/diagnostics/fable2_state_probe.h) carry B. The box posts
// "cancelled" and the Lua unwinds every open page and submenu. The Lua only asks while one of its boxes
// is up, so B never reaches gameplay on its own. No dependencies, so tests/f6_menu_tests.py compiles it.
#pragma once

#include <atomic>
#include <cstdint>

namespace fable2::f6close {

inline constexpr const char* kCloseBoxAck = "F6_close_box.lua";
inline constexpr uint16_t kPadB = 0x2000;  // X_INPUT_GAMEPAD_B
inline constexpr int kTapPolls = 2;        // pad reads with B held (press edge), then released

inline std::atomic<int> g_tap_polls{0};

inline void Request() { g_tap_polls.store(kTapPolls, std::memory_order_relaxed); }

// Called after each successful XamInputGetState of user 0 with the guest XINPUT_STATE (big-endian: u32
// packet number, u16 buttons, ...). The packet number is raised by one at each press/release change
// (and stays raised), so the game sees both as new packets.
inline void InjectTap(uint8_t* state) {
  static uint32_t bias = 0;  // pad-read thread only
  static bool held = false;
  int n = g_tap_polls.load(std::memory_order_relaxed);
  const bool now = n > 0;
  if (now) g_tap_polls.compare_exchange_strong(n, n - 1, std::memory_order_relaxed);
  if (now != held) {
    ++bias;
    held = now;
  }
  if (bias == 0) return;
  uint32_t packet = (uint32_t(state[0]) << 24) | (uint32_t(state[1]) << 16) | (uint32_t(state[2]) << 8) | state[3];
  packet += bias;
  state[0] = uint8_t(packet >> 24);
  state[1] = uint8_t(packet >> 16);
  state[2] = uint8_t(packet >> 8);
  state[3] = uint8_t(packet);
  if (now) {
    const uint16_t buttons = uint16_t((state[4] << 8) | state[5]) | kPadB;
    state[4] = uint8_t(buttons >> 8);
    state[5] = uint8_t(buttons);
  }
}

}  // namespace fable2::f6close
