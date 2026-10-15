#pragma once

// [graphics] settings of fable2_config.toml that the Metal plugin or the SDK
// window reads (docs/resolution-scale.md). Priority: command line / SDK config
// > the environment (Dev Launcher) > fable2_config.toml > built-in default,
// except fullscreen, where the environment also beats the SDK config file
// (see SeedFullscreen).

#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include "fable2_config.h"
#include "fable2_res_auto.h"

namespace fable2::graphics_config {

// Internal resolution. The Metal plugin reads FABLE2_RES_SCALE when its
// command processor starts, so the config value is exported to it unless the
// environment already sets it. "auto" (FABLE2_RES_SCALE=auto or
// resolution_scale = "auto") is resolved here to 1, 2 or 3 from the GPU
// (src/core/fable2_res_auto.h) and exported as that number, so the plugin and
// everything else only ever see a number. Call before the graphics system is
// set up.
inline void ApplyResolutionScale(const fable2::config::Values& cfg) {
  if (const char* env = std::getenv("FABLE2_RES_SCALE"); env && *env) {
    if (strcasecmp(env, "auto") != 0) {
      REXSYS_INFO("[fable2-config] resolution_scale: FABLE2_RES_SCALE={} (environment wins)", env);
      return;
    }
    REXSYS_INFO("[fable2-config] resolution_scale: FABLE2_RES_SCALE=auto (environment wins)");
  } else if (cfg.resolution_scale_auto) {
    REXSYS_INFO("[fable2-config] resolution_scale=auto (fable2_config.toml)");
  } else {
    REXSYS_INFO("[fable2-config] resolution_scale={} (fable2_config.toml)", cfg.resolution_scale);
    if (cfg.resolution_scale > 1) {
      setenv("FABLE2_RES_SCALE", std::to_string(cfg.resolution_scale).c_str(), 1);
    }
    return;
  }
  const int scale = fable2::res_auto::Resolve();
  setenv("FABLE2_RES_SCALE", std::to_string(scale).c_str(), 1);
}

// Shader cache target (docs/native-shaders.md, "Converter target"). The Metal
// plugin pins Metal Shader Converter to apple7:15.0 unless
// FABLE2_MSC_MIN_TARGET says otherwise, so [graphics] shader_cache_target is
// exported only when it is not "portable". Call before the graphics system is
// set up.
inline void ApplyShaderCacheTarget(const fable2::config::Values& cfg) {
  if (const char* env = std::getenv("FABLE2_MSC_MIN_TARGET"); env && *env) {
    REXSYS_INFO("[fable2-config] shader_cache_target: FABLE2_MSC_MIN_TARGET={} (environment wins)",
                env);
    return;
  }
  const std::string& target = cfg.shader_cache_target;
  if (target.empty() || target == "portable") {
    REXSYS_INFO("[fable2-config] shader_cache_target=portable (fable2_config.toml)");
    return;
  }
  REXSYS_INFO("[fable2-config] shader_cache_target={} (fable2_config.toml)", target);
  setenv("FABLE2_MSC_MIN_TARGET", target.c_str(), 1);
}

// Upscaling (metal_presenter_use_metalfx, a Metal-plugin cvar): "off" keeps the
// bilinear presenter pass (the faithful path); "metalfx-spatial" runs the MetalFX
// spatial scaler from the internal resolution to the window's aspect-fit size
// whenever that is larger (docs/resolution-scale.md section 22). Call after the
// plugin has registered its flags. Precedence: an explicit cvar (command line,
// fable_2.toml, REX_*) > FABLE2_UPSCALING=off|metalfx-spatial > the legacy
// FABLE2_METALFX=1|0 > [graphics] upscaling (or the legacy `metalfx` key).
// The upscaling mode from FABLE2_UPSCALING > FABLE2_METALFX > the config (an
// explicit metal_presenter_use_metalfx cvar is checked by SeedMetalFx). `warn`
// logs invalid values (SeedMetalFx); the window title calls it quietly.
inline fable2::config::Values::Upscaling ResolveUpscaling(const fable2::config::Values& cfg,
                                                          const char** source, bool warn) {
  using Upscaling = fable2::config::Values::Upscaling;
  Upscaling mode = cfg.upscaling;
  *source = "fable2_config.toml";
  if (const char* env = std::getenv("FABLE2_UPSCALING"); env && *env) {
    if (const auto parsed = fable2::config::ParseUpscaling(env)) {
      mode = *parsed;
      *source = "FABLE2_UPSCALING";
    } else if (warn) {
      REXSYS_WARN("[fable2-config] ignoring FABLE2_UPSCALING='{}' (expected off or metalfx-spatial)",
                  env);
    }
  } else if (const char* legacy = std::getenv("FABLE2_METALFX"); legacy && *legacy) {
    if (std::strcmp(legacy, "1") == 0 || std::strcmp(legacy, "0") == 0) {
      mode = legacy[0] == '1' ? Upscaling::kMetalFxSpatial : Upscaling::kOff;
      *source = "FABLE2_METALFX";
    } else if (warn) {
      REXSYS_WARN("[fable2-config] ignoring FABLE2_METALFX='{}' (expected 1 or 0)", legacy);
    }
  }
  return mode;
}

inline void SeedMetalFx(bool metal, const fable2::config::Values& cfg) {
  if (!metal) return;
  using Upscaling = fable2::config::Values::Upscaling;
  const char* source = "";
  const Upscaling mode = ResolveUpscaling(cfg, &source, /*warn=*/true);
  const bool on = mode == Upscaling::kMetalFxSpatial;
  constexpr const char* kCvar = "metal_presenter_use_metalfx";
  if (rex::cvar::GetFlagSource(kCvar) != rex::cvar::Source::kDefault) {
    REXSYS_INFO("[fable2-config] upscaling: {} set explicitly; keeping it", kCvar);
    return;
  }
  if (!on) {
    // Off is the cvar default: leave it untouched so the off path is exactly today's.
    REXSYS_INFO("[fable2-config] upscaling=off (source: {})", source);
    return;
  }
  if (!rex::cvar::SetFlagByName(kCvar, "true")) {
    REXSYS_WARN("[fable2-config] graphics plugin rejected {}", kCvar);
    return;
  }
  REXSYS_INFO("[fable2-config] upscaling={} (source: {})", fable2::config::UpscalingName(mode),
              source);
}

// Effect size at internal resolution 2x/3x (docs/resolution-scale.md section 12):
// the SDK shader translator divides texture-fetch texel offsets by the
// resolution scale (draw_resolution_scaled_texture_offsets, default true), so
// every blur/bloom/glow kernel built from fetch offsets shrinks with the
// resolution. "Native" keeps them in guest texels, the original size.
// FABLE2_EFFECTS_NATIVE=1|0 overrides the config; an explicit cvar keeps
// priority over both. Call after the graphics plugin registered its flags.
inline void SeedEffectsNative(bool metal, const fable2::config::Values& cfg) {
  if (!metal) return;
  bool on = cfg.effects_native_scale;
  const char* source = "fable2_config.toml";
  if (const char* env = std::getenv("FABLE2_EFFECTS_NATIVE"); env && *env) {
    if (std::strcmp(env, "1") == 0 || std::strcmp(env, "0") == 0) {
      on = env[0] == '1';
      source = "FABLE2_EFFECTS_NATIVE";
    } else {
      REXSYS_WARN("[fable2-config] ignoring FABLE2_EFFECTS_NATIVE='{}' (expected 1 or 0)", env);
    }
  }
  constexpr const char* kCvar = "draw_resolution_scaled_texture_offsets";
  if (rex::cvar::GetFlagSource(kCvar) != rex::cvar::Source::kDefault) {
    REXSYS_INFO("[fable2-config] effects_native_scale: {} set explicitly; keeping it", kCvar);
    return;
  }
  if (!rex::cvar::SetFlagByName(kCvar, on ? "false" : "true")) {
    REXSYS_WARN("[fable2-config] graphics plugin rejected {}", kCvar);
    return;
  }
  REXSYS_INFO("[fable2-config] effects_native_scale={} (source: {}): {}={}", on, source, kCvar,
              on ? "false" : "true");
}

// Display mode: the SDK `fullscreen` cvar (read when the window is created;
// the SDK default is true). Precedence, highest first:
//   1. --fullscreen=... on the command line;
//   2. FABLE2_FULLSCREEN=1|0 (the Dev Launcher always passes it), which also
//      beats a `fullscreen =` line in fable_2.toml and REX_FULLSCREEN;
//   3. `fullscreen =` in fable_2.toml (the SDK writes it there when its
//      settings screen saves) or REX_FULLSCREEN;
//   4. [graphics] fullscreen in fable2_config.toml (default false).
// Fullscreen is SDL's desktop fullscreen, which on macOS is a native Spaces
// fullscreen (SDL_VIDEO_MAC_FULLSCREEN_SPACES defaults to on).
inline void SeedFullscreen(const fable2::config::Values& cfg) {
  constexpr const char* kCvar = "fullscreen";
  const auto source = rex::cvar::GetFlagSource(kCvar);
  if (source == rex::cvar::Source::kCommandLine) {
    REXSYS_INFO("[fable2-config] fullscreen: set on the command line ({}); keeping it",
                rex::cvar::GetFlagByName(kCvar));
    return;
  }
  bool on = cfg.fullscreen;
  const char* from = "fable2_config.toml";
  if (const char* env = std::getenv("FABLE2_FULLSCREEN"); env && *env) {
    if (std::strcmp(env, "1") == 0 || std::strcmp(env, "0") == 0) {
      on = env[0] == '1';
      from = "FABLE2_FULLSCREEN";
    } else {
      REXSYS_WARN("[fable2-config] ignoring FABLE2_FULLSCREEN='{}' (expected 1 or 0)", env);
    }
  }
  if (std::strcmp(from, "FABLE2_FULLSCREEN") != 0 && source != rex::cvar::Source::kDefault) {
    REXSYS_INFO("[fable2-config] fullscreen: set by fable_2.toml or REX_FULLSCREEN ({}); keeping it",
                rex::cvar::GetFlagByName(kCvar));
    return;
  }
  if (!rex::cvar::SetFlagByName(kCvar, on ? "true" : "false")) {
    REXSYS_WARN("[fable2-config] SDK rejected {}", kCvar);
    return;
  }
  REXSYS_INFO("[fable2-config] fullscreen={} (source: {})", on, from);
}

}  // namespace fable2::graphics_config
