// fable_2 - ReXGlue Recompiled Project
//
// fable2_config.toml: the recomp's own user config (separate from the ReXGlue
// SDK cvar config fable_2.toml). Look for it next to the exe, create it with
// defaults if missing, parse it, and expose the values via Get().
//
// Growth model: add a member to Values (with its default), read it in Load()
// with one of the Read* helpers, and document the key with the same default in
// config/fable2_config.toml (embedded at build time; it is the only template).
// tests/config_defaults_tests.cpp fails the build if the two disagree.
// Missing keys -> built-in default. Wrong type -> warning + default. Unknown
// sections -> warning. A broken file -> error + dialog + defaults (the game
// still runs), so a hand edit can never wedge the launch.

#include "fable2_config.h"
#include "fable2_config_template.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>

#include <rex/logging/macros.h>
#include <rex/system.h>

#include <toml++/toml.hpp>

namespace fable2::config {
namespace {

Values g_values;

// The default file is config/fable2_config.toml itself, embedded byte-for-byte
// at build time (CMakeLists.txt generates fable2_config_template.h). It is the
// only copy: a first launch writes exactly these bytes.
std::string_view TemplateText() {
  return {reinterpret_cast<const char*>(embedded::kDefaultConfig),
          embedded::kDefaultConfigSize};
}

std::string_view TypeName(toml::node_type t) {
  return toml::impl::node_type_friendly_names[static_cast<std::size_t>(t)];
}

// Read a typed key from a section. Missing key -> default (silent); the key
// exists but is the wrong kind -> warning + default.
template <typename T>
T Read(const toml::table& section, std::string_view section_name,
       std::string_view key, std::string_view type_name, T default_value) {
  const toml::path key_path{key};
  const auto view = section[key_path];
  if (!view) return default_value;
  if (auto v = view.value_exact<T>()) return static_cast<T>(*v);
  REXSYS_WARN(
      "[fable2-config] [{}] {}: expected {}, got {}; using built-in default",
      section_name, key, type_name, TypeName(view.type()));
  return default_value;
}

// FABLE2_UNLOCK_EXTRAS=1|0 (Dev Launcher "Unlock Collector's Edition + website
// gear") sets BOTH unlock_ce and unlock_website, overriding [patches]; unset
// leaves the TOML in charge. Logs the effective values and their source once.
void ApplyUnlockOverride(Values& values, const char* toml_source) {
  const char* env = std::getenv("FABLE2_UNLOCK_EXTRAS");
  const char* source = toml_source;
  if (env && *env) {
    if (std::strcmp(env, "1") == 0 || std::strcmp(env, "0") == 0) {
      values.unlock_ce = values.unlock_website = env[0] == '1';
      source = "FABLE2_UNLOCK_EXTRAS";
    } else {
      REXSYS_WARN("[fable2-config] ignoring FABLE2_UNLOCK_EXTRAS='{}' (expected 1 or 0)", env);
    }
  }
  REXSYS_INFO("[fable2-config] unlock_ce={} unlock_website={} (source: {})", values.unlock_ce,
              values.unlock_website, source);
}

}  // namespace

bool Load(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      REXSYS_ERROR("[fable2-config] could not create {}", path.string());
      return false;
    }
    const std::string_view text = TemplateText();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REXSYS_INFO("[fable2-config] {} not found; created with defaults",
                path.string());
  }

  // toml++ (v3, exceptions on) returns the table directly and throws a
  // toml::parse_error on a broken file.
  toml::table root;
  try {
    root = toml::parse_file(path.native());
  } catch (const toml::parse_error& err) {
    REXSYS_ERROR("[fable2-config] {} failed to parse at line {}, column {}: {}",
                 path.string(), err.source().begin.line, err.source().begin.column,
                 err.description());
    const std::string msg = std::format(
        "fable2_config.toml could not be parsed\n\n"
        "{}\n\n"
        "File: {}\n"
        "The game will continue with built-in defaults. Fix the file (or "
        "delete it to recreate the defaults) and start again.",
        err.description(), path.string());
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    ApplyUnlockOverride(g_values, "built-in defaults");
    return false;
  }

  // Warn about unknown top-level sections (usually a typo or a file written
  // for a newer build).
  static constexpr std::array<std::string_view, 5> kKnownSections = {
      "general", "input", "patches", "graphics", "perf"};
  for (const auto& [key, value] : root) {
    const bool known =
        std::ranges::find(kKnownSections, key.str()) != kKnownSections.end();
    if (!known && value.is_table())
      REXSYS_WARN("[fable2-config] unknown section [{}] ignored", key.str());
  }

  Values values;
  const toml::path general_path{"general"};
  const auto general = root[general_path];
  if (general.is_table()) {
    values.config_version = static_cast<int32_t>(
        Read<int64_t>(*general.as_table(), "general", "config_version",
                      "integer", 1));
  }
  const toml::path input_path{"input"};
  const auto input = root[input_path];
  if (input.is_table()) {
    const toml::table& input_table = *input.as_table();
    values.keyboard_gamepad_map = Read<std::string>(
        input_table, "input", "keyboard_gamepad_map", "string",
        std::string{kDefaultKeyboardGamepadMap});
    // Defaults come from Values itself (single source of truth).
    values.mouse_look =
        Read<bool>(input_table, "input", "mouse_look", "boolean",
                   values.mouse_look);
    values.mouse_look_scale = static_cast<int32_t>(Read<int64_t>(
        input_table, "input", "mouse_look_scale", "integer",
        values.mouse_look_scale));
  }
  const toml::path patches_path{"patches"};
  const auto patches = root[patches_path];
  if (patches.is_table()) {
    const toml::table& patches_table = *patches.as_table();
    // fps_60 is a boolean or the string "auto" ("auto", "Auto" or "AUTO").
    if (const auto fps = patches_table[toml::path{"fps_60"}]) {
      if (auto b = fps.value_exact<bool>()) {
        values.fps_60 = *b;
        values.fps_60_auto = false;
      } else if (auto str = fps.value_exact<std::string>();
                 str && (*str == "auto" || *str == "Auto" || *str == "AUTO")) {
        values.fps_60_auto = true;
      } else {
        REXSYS_WARN("[fable2-config] [patches] fps_60: expected true, false or \"auto\"; "
                    "using built-in default");
      }
    }
    if (const auto turbo = patches_table[toml::path{"turbo_scale"}]) {
      if (auto v = turbo.value<double>()) values.turbo_scale = *v;
      else REXSYS_WARN("[fable2-config] [patches] turbo_scale: expected a number; using {}",
                       values.turbo_scale);
    }
    if (const auto slow = patches_table[toml::path{"slow_scale"}]) {
      if (auto v = slow.value<double>()) values.slow_scale = *v;
      else REXSYS_WARN("[fable2-config] [patches] slow_scale: expected a number; using {}",
                       values.slow_scale);
    }
    if (const auto audio = patches_table[toml::path{"turbo_audio"}]) {
      const auto str = audio.value_exact<std::string>();
      const auto mode = str ? ParseTurboAudio(*str) : std::nullopt;
      if (mode) values.turbo_audio = *mode;
      else REXSYS_WARN("[fable2-config] [patches] turbo_audio: expected \"natural\", "
                       "\"high-pitch\" or \"normal\"; using built-in default");
    }
    values.unlock_website = Read<bool>(patches_table, "patches",
                                       "unlock_website", "boolean",
                                       values.unlock_website);
    values.unlock_ce = Read<bool>(patches_table, "patches", "unlock_ce",
                                  "boolean", values.unlock_ce);
    values.hero_dog_texture_readback =
        Read<bool>(patches_table, "patches", "hero_dog_texture_readback", "boolean",
                   values.hero_dog_texture_readback);
  }
  const toml::path graphics_path{"graphics"};
  const auto graphics = root[graphics_path];
  if (graphics.is_table()) {
    const toml::table& graphics_table = *graphics.as_table();
    // resolution_scale is 1..3 or the string "auto" (src/core/fable2_res_auto.h).
    const auto scale_node = graphics_table[toml::path{"resolution_scale"}];
    if (auto str = scale_node.value_exact<std::string>();
        str && (*str == "auto" || *str == "Auto" || *str == "AUTO")) {
      values.resolution_scale_auto = true;
    } else if (scale_node) {
      values.resolution_scale_auto = false;
      const int64_t scale = Read<int64_t>(graphics_table, "graphics", "resolution_scale",
                                          "integer or \"auto\"", values.resolution_scale);
      if (scale < 1 || scale > 3) {
        REXSYS_WARN("[fable2-config] [graphics] resolution_scale={} out of range 1..3; clamped",
                    scale);
      }
      values.resolution_scale = static_cast<int32_t>(std::clamp<int64_t>(scale, 1, 3));
    }
    // upscaling = "off" | "metalfx-spatial"; the old boolean `metalfx` key
    // (before 2026-10-12) is honoured when `upscaling` is absent.
    if (const auto up = graphics_table[toml::path{"upscaling"}]) {
      bool ok = false;
      if (auto str = up.value_exact<std::string>()) {
        if (const auto mode = ParseUpscaling(*str)) {
          values.upscaling = *mode;
          ok = true;
        }
      }
      if (!ok) {
        REXSYS_WARN("[fable2-config] [graphics] upscaling: expected \"off\" or "
                    "\"metalfx-spatial\"; using built-in default");
      }
    } else if (graphics_table[toml::path{"metalfx"}]) {
      values.upscaling = Read<bool>(graphics_table, "graphics", "metalfx", "boolean", false)
                             ? Values::Upscaling::kMetalFxSpatial
                             : Values::Upscaling::kOff;
    }
    values.effects_native_scale =
        Read<bool>(graphics_table, "graphics", "effects_native_scale", "boolean",
                   values.effects_native_scale);
    values.fullscreen =
        Read<bool>(graphics_table, "graphics", "fullscreen", "boolean", values.fullscreen);
    values.fps_counter =
        Read<bool>(graphics_table, "graphics", "fps_counter", "boolean", values.fps_counter);
    values.shader_cache_target =
        Read<std::string>(graphics_table, "graphics", "shader_cache_target", "string",
                          values.shader_cache_target);
  }
  const toml::path perf_path{"perf"};
  const auto perf = root[perf_path];
  if (perf.is_table()) {
    const toml::table& perf_table = *perf.as_table();
    values.hotfunc_yield_every = static_cast<int32_t>(Read<int64_t>(
        perf_table, "perf", "hotfunc_yield_every", "integer",
        values.hotfunc_yield_every));
  }

  ApplyUnlockOverride(values, "[patches] in fable2_config.toml");
  g_values = values;
  REXSYS_INFO("[fable2-config] loaded {} (config_version={})", path.string(),
              values.config_version);
  return true;
}

const Values& Get() { return g_values; }

std::string_view DefaultTemplate() { return TemplateText(); }

}  // namespace fable2::config
