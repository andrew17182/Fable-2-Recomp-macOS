// fable_2 - ReXGlue Recompiled Project
//
// Recomp user configuration (fable2_config.toml), separate from the ReXGlue
// SDK cvar config (fable_2.toml). Loaded once at startup from next to the
// exe; created with defaults if missing.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fable2::config {

// Default host keyboard -> guest gamepad map (see src/input/keyboard_gamepad.h).
// It lives here, in the config layer, so the mapping is editable in
// fable2_config.toml instead of being baked into the binary; the
// keyboard_gamepad_map cvar itself defaults to empty and is seeded from
// here at startup (see Fable2App::OnPostInitLogging).
inline constexpr std::string_view kDefaultKeyboardGamepadMap =
    "E:A,X:B,Z:X,C:Y,"
    "W:StickUp,S:StickDown,A:StickLeft,D:StickRight,"
    "Escape:Pause,M:Select,Q:LT,Tab:RT,"
    "Up:Up,Down:Down,Left:Left,Right:Right";

// Parsed values from fable2_config.toml. To add a setting:
//   1. Add a member here with its built-in default.
//   2. Read it in Load() below (ReadInt/ReadBool/ReadString helpers).
//   3. Document the key, with the same default, in config/fable2_config.toml.
//      That file is the only template (embedded at build time); the build
//      runs tests/config_defaults_tests.cpp, which fails if they disagree.
// Missing keys keep their defaults; wrong types log a warning and keep the
// default, so the file can grow without breaking the game.
struct Values {
  // [general]
  int32_t config_version = 1;
  // [input]
  std::string keyboard_gamepad_map{std::string{kDefaultKeyboardGamepadMap}};
  // Keep in sync with the cvar defaults in main.cpp (mouse_look,
  // mouse_look_scale); the cvar keeps its compiled default as a fallback so
  // an empty/missing config can never wedge input.
  bool mouse_look = false;
  int32_t mouse_look_scale = 256;  // range 1..4096 (cvar constraint)
  // [patches] - toggles for the recomp-level (mid-asm hook) patches. The
  // hook bodies consult these at runtime (src/core/fable2_hooks.cpp), so a patch
  // can be A/B'd with no rebuild. Guest-image data patches live in
  // fable2_patches.toml instead (see src/core/fable2_patches.h).
  // Unlock the Guild-chest items that were obtainable from the (now-dead)
  // Fable 2 website. Forces both the registration gates and the grant-method
  // result in GuildChest_GetWebsiteItem_8256E368 (hooks
  // fable2_hook_website_g1/g1b/grantnew in src/core/fable2_hooks.cpp).
  // Frame-rate mode. Default (key missing) is "auto": choose 60 or 30 from the
  // GPU at startup (src/core/fable2_fps_auto.h). An explicit
  // fps_60 = true/false clears fps_60_auto and uses fps_60 (true = single-vblank
  // pacing, 60 FPS; false = 30 FPS).
  bool fps_60 = false;
  bool fps_60_auto = true;
  double turbo_scale = 2.0; // F7 turbo speed (1.0 disables); see fable2_turbo.h.
  bool unlock_website = true;
  // Unlock the Collectors Edition chest content. Forces both the
  // registration gates and the grant-method result in
  // GuildChest_GetCEContent_824B3528 (hooks fable2_hook_ce_g1/g1b/grantavail
  // in src/core/fable2_hooks.cpp).
  bool unlock_ce = true;
  // Force a CPU readback of the render-to-texture resolve that (re)generates
  // the hero/dog face+skin texture, so the character does not render black on
  // a split-memory host (see plans/hero-dog-texture-readback.md). Approach + the
  // guest base 0x12704000 come from just-harry's Unofficial Xenia femtofork for
  // Fable II. This seeds the SDK cvar readback_resolve_force_addresses in
  // Fable2App; readback then happens only for that resolve, not every frame.
  bool hero_dog_texture_readback = true;
  // [graphics] - Metal renderer (docs/resolution-scale.md). Exported to the
  // plugin by src/core/fable2_graphics_config.h; FABLE2_RES_SCALE and
  // FABLE2_UPSCALING (Dev Launcher) override them.
  // Internal resolution multiplier per axis: 1 = native 1280x720, 2, 3.
  int32_t resolution_scale = 1;
  // resolution_scale = "auto": pick 1/2/3 from the GPU at startup
  // (src/core/fable2_res_auto.h); resolution_scale is then ignored.
  // Default "auto" (2026-10-14), also when the key is missing.
  bool resolution_scale_auto = true;
  // Upscaling of the game image to the window ("off" = bilinear, the faithful
  // path; "metalfx-spatial" = MetalFX spatial scaler, look-changing). Applies
  // only when the window is larger than the internal resolution. The old
  // boolean key `metalfx` is still read when `upscaling` is absent.
  enum class Upscaling : int32_t { kOff = 0, kMetalFxSpatial = 1 };
  Upscaling upscaling = Upscaling::kOff;
  // At internal resolution 2x/3x: keep bloom, blur and glow effects at their
  // original (native 1x) size. FABLE2_EFFECTS_NATIVE (Dev Launcher) overrides it.
  bool effects_native_scale = true;
  // Start fullscreen (SDK `fullscreen` cvar; macOS Spaces fullscreen) instead
  // of windowed. FABLE2_FULLSCREEN (Dev Launcher) overrides it, and so does a
  // `fullscreen =` line in fable_2.toml (see fable2_graphics_config.h).
  bool fullscreen = false;
  // On-screen FPS counter (bottom-left; src/core/fable2_fps_counter.h). F9
  // toggles it live; FABLE2_FPS_COUNTER (Dev Launcher) overrides the start state.
  bool fps_counter = false;
  // Metal Shader Converter target, which keys the shader cache: "portable"
  // (the plugin's default pin apple7:15.0, so the cache survives macOS updates
  // and works on any Apple Silicon Mac), "native" (this Mac's GPU family and
  // exact macOS version, as before 2026-10-12) or "<family>:<macOS>".
  // Exported as FABLE2_MSC_MIN_TARGET; the environment (Dev Launcher) wins.
  std::string shader_cache_target = "portable";
  // [perf] - hot-function override tuning.
  // hotfunc_yield_every: NtYieldExecution batching factor for the hotfunc
  // overrides (see src/core/hotfunc/hotfunc_yield.h). Every Nth call does
  // the real SwitchToThread; the others take a full memory fence. 1 =
  // original (yield every call), 0 = never yield. Larger = fewer context
  // switches, less frequent CPU rotation to other guest threads.
  int32_t hotfunc_yield_every = 8;

  bool operator==(const Values&) const = default;
};

// [graphics] upscaling names: "off" and "metalfx-spatial" ("metalfx" is
// accepted as a short alias). Shared by the TOML loader and FABLE2_UPSCALING.
inline std::optional<Values::Upscaling> ParseUpscaling(std::string_view name) {
  if (name == "off") return Values::Upscaling::kOff;
  if (name == "metalfx-spatial" || name == "metalfx") return Values::Upscaling::kMetalFxSpatial;
  return std::nullopt;
}
inline const char* UpscalingName(Values::Upscaling mode) {
  return mode == Values::Upscaling::kMetalFxSpatial ? "metalfx-spatial" : "off";
}

// Load the config from `path`.
//   - File missing  -> writes the default template, then parses it (returns
//                      true).
//   - Parse failure -> logs the error, shows a dialog, falls back to the
//                      in-memory defaults (returns false).
// Unknown top-level sections log a warning; everything is otherwise ignored.
// Safe to call only after the SDK logging is initialized (OnPostInitLogging).
bool Load(const std::filesystem::path& path);

// Parsed values (in-memory defaults until Load() succeeds).
const Values& Get();

// The default template written when the file is missing: the exact bytes of
// config/fable2_config.toml, embedded at build time.
std::string_view DefaultTemplate();

}  // namespace fable2::config
