// Frame generation (docs/macos-metal.md "Frame generation"): MetalFX frame interpolation in the Metal presenter
// shows one generated frame between every two real ones, 30 rendered -> 60 shown.
//
// Start state: FABLE2_FRAME_INTERP=1 (default 0; launcher Settings > Display > Frame rate "Frame generation", a
// Custom preset setting). 0 or F10 (keyboard_gamepad.h) and the controller action FABLE2_PAD_FRAMEGEN
// (controller_hotkeys.h, default none) toggle it live; the toggle never changes the launcher setting.
// Rules, decided once at startup (Configure, after the frame-rate decision):
// - It needs the 30 FPS cap (fps_60 off, vsync on). At 60 FPS or uncapped it stays off; the frame rate is not
//   changed. The start state logs why, and F10 shows a notice ("Frame generation needs 30 FPS") and does nothing.
// - It needs macOS 26 and a GPU that MetalFX frame interpolation supports (MTLFXFrameInterpolatorDescriptor
//   supportsDevice), and the Metal renderer. Same handling.
// - Turbo pauses it (maintainer, 2026-10-22): while turbo is on the presenter shows only real frames, and it resumes
//   when turbo turns off. No notice of its own (turbo's notice says enough); the F9 overlay shows "framegen paused".
//   Slow mode keeps it running (about 30 real frames per second at the 30 cap, src/core/fable2_turbo.h).
// The presenter side is backends/metal/port/rex/ui/metal/metal_frame_interp.{h,mm}, driven through its export
// fable2_frame_interp_enable (dlsym): 1 = on, 0 = off, 2 = query support.
#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dlfcn.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "fable2_fps_auto.h"
#include "fable2_turbo.h"

namespace fable2::frame_gen {

// Why frame generation can't run (kOk: it can).
enum class Block { kUnknown, kOk, kNeeds30, kNeedsMetal, kNeedsMacOS26, kUnsupported };

inline std::atomic<Block> g_block{Block::kUnknown};
inline std::atomic<bool> g_on{false};      // wanted (start state, F10)
inline std::atomic<bool> g_paused{false};  // wanted but paused while turbo is on

using EnableFn = int (*)(int);
inline EnableFn Backend() {
  static const EnableFn fn = reinterpret_cast<EnableFn>(dlsym(RTLD_DEFAULT, "fable2_frame_interp_enable"));
  return fn;
}

// Short reason for the notice and the log ("needs 30 FPS").
inline const char* BlockText(Block b) {
  switch (b) {
    case Block::kNeeds30: return "needs 30 FPS";
    case Block::kNeedsMetal: return "needs the Metal renderer";
    case Block::kNeedsMacOS26: return "needs macOS 26";
    case Block::kUnsupported: return "is not supported on this Mac";
    default: return "is not ready";
  }
}

// Whether FABLE2_FRAME_INTERP asks for frame generation at start (1 / 0; anything else is ignored with a warning).
inline bool Requested() {
  const char* v = std::getenv("FABLE2_FRAME_INTERP");  // getenv-ok: once, at startup
  if (!v || !*v || std::strcmp(v, "0") == 0) return false;
  if (std::strcmp(v, "1") == 0) return true;
  REXSYS_WARN("[frame-gen] ignoring FABLE2_FRAME_INTERP='{}' (expected 1 or 0)", v);
  return false;
}

// Is frame generation on (wanted; it may be paused by turbo)? Paused()? (F9 overlay)
inline bool Active() { return g_on.load(std::memory_order_relaxed); }
inline bool Paused() { return Active() && g_paused.load(std::memory_order_relaxed); }

// Tells the presenter: on when wanted and turbo is off. Called on every change (F10, turbo mode change).
inline void Apply() {
  if (g_block.load() != Block::kOk || !Backend()) return;
  const bool pause = fable2::turbo::Active();
  const bool was_paused = g_paused.exchange(pause);
  Backend()(Active() && !pause ? 1 : 0);
  if (Active() && pause != was_paused) REXSYS_INFO("[frame-gen] {} (turbo {})", pause ? "paused" : "resumed", pause ? "on" : "off");
}

// Once, after fable2::fps::Use60() (OnPostSetup). Returns whether frame generation is on.
inline bool Configure() {
  Block block = Block::kOk;
  const bool uncapped = rex::cvar::GetFlagByName("vsync") == "false";
  if (uncapped || fable2::fps::Use60()) {
    block = Block::kNeeds30;
  } else if (!Backend()) {
    block = Block::kNeedsMetal;
  } else {
    const int support = Backend()(2);
    block = support == 1 ? Block::kOk : support == -1 ? Block::kNeedsMacOS26 : Block::kUnsupported;
  }
  g_block.store(block);
  fable2::turbo::g_on_mode_change = &Apply;
  const bool want = Requested();
  if (block != Block::kOk) {
    REXSYS_INFO("[frame-gen] {}: {} ({}; 0/F10 shows this)", want ? "off" : "off, unavailable", BlockText(block),
                block == Block::kNeeds30 ? (uncapped ? "running uncapped" : "running at 60 FPS") : "checked at start");
    return false;
  }
  if (!want) {
    REXSYS_INFO("[frame-gen] off (available: 0/F10 turns it on)");
    return false;
  }
  g_on.store(true);
  Apply();
  REXSYS_INFO("[frame-gen] on: MetalFX frame interpolation, 30 -> 60{}", Paused() ? " (paused: turbo is on)" : "");
  return true;
}

// F10 / the controller action (UI thread): flip it, or say why it can't run. Runtime only.
inline void Toggle() {
  const Block block = g_block.load();
  std::string text;
  if (block != Block::kOk) {
    text = std::string("Frame generation ") + BlockText(block);
  } else {
    g_on.store(!Active());
    Apply();
    text = std::string("Frame generation ") + (Active() ? "on" : "off");
  }
  REXSYS_INFO("[frame-gen] toggle: {}", text);
  if (fable2::turbo::g_notice) fable2::turbo::g_notice(text);
}

}  // namespace fable2::frame_gen
