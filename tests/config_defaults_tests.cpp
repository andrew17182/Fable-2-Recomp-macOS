// Single-source check for the default user config.
//
// config/fable2_config.toml is the only copy of the default config: it is
// embedded into the game at build time and written out on a first launch. The
// built-in defaults (fable2::config::Values) are what a missing key falls back
// to. This test fails (and with it the normal macOS build) when:
//   - the embedded bytes differ from the repo file (stale generated header);
//   - a first launch does not write the file byte-for-byte;
//   - any value in the file differs from the built-in default;
//   - the file has a key Load() ignores, or lacks one of the expected keys.

#include "fable2_config.h"
#include "fable2_config_template.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>

#include <toml++/toml.hpp>

#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "config_defaults_tests: failed at line %d: %s\n",  \
                   __LINE__, #x);                                             \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

namespace {

namespace fs = std::filesystem;
using fable2::config::Values;

std::string ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteBytes(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

// Every key the shipped file must contain. A new setting goes here, in
// Values, in Load() and in config/fable2_config.toml.
const std::set<std::string> kExpectedKeys = {
    "general.config_version",
    "input.keyboard_gamepad_map",
    "input.mouse_look",
    "input.mouse_look_scale",
    "patches.fps_60",
    "patches.turbo_scale",
    "patches.slow_scale",
    "patches.turbo_audio",
    "patches.unlock_website",
    "patches.unlock_ce",
    "patches.hero_dog_texture_readback",
    "graphics.resolution_scale",
    "graphics.upscaling",
    "graphics.effects_native_scale",
    "graphics.fullscreen",
    "graphics.fps_counter",
    "graphics.shader_cache_target",
    "perf.hotfunc_yield_every",
};

// Replace one value with a different one of a type Load() accepts.
void Mutate(toml::node& node) {
  if (auto* b = node.as_boolean()) *b = !b->get();
  else if (auto* i = node.as_integer()) *i = i->get() + 1;
  else if (auto* f = node.as_floating_point()) *f = f->get() + 0.5;
  else if (auto* s = node.as_string()) s->get() += ",F9:L3";
}

}  // namespace

int main() {
  unsetenv("FABLE2_UNLOCK_EXTRAS");
  const fs::path source = FABLE2_DEFAULT_CONFIG_PATH;
  const fs::path work = FABLE2_TEST_WORK_DIR;
  fs::remove_all(work);
  fs::create_directories(work);

  // 1. The embedded template is exactly the repo file.
  const std::string repo = ReadBytes(source);
  CHECK(!repo.empty());
  CHECK(fable2::config::DefaultTemplate() == repo);
  CHECK(fable2::config::embedded::kDefaultConfig[repo.size()] == 0);

  // 2. First launch: a missing file is created byte-for-byte, then loaded.
  const fs::path first = work / "fable2_config.toml";
  CHECK(fable2::config::Load(first));
  CHECK(ReadBytes(first) == repo);

  // 3. Every shipped value equals the built-in default for a missing key.
  CHECK(fable2::config::Get() == Values{});
  const fs::path empty = work / "empty.toml";
  WriteBytes(empty, "");
  CHECK(fable2::config::Load(empty));
  CHECK(fable2::config::Get() == Values{});

  // 4. The file has exactly the expected keys, and Load() reads each one
  //    (changing it in the file changes the loaded values).
  const toml::table shipped = toml::parse(repo);
  std::set<std::string> found;
  for (const auto& [section, table] : shipped) {
    CHECK(table.is_table());
    for (const auto& [key, value] : *table.as_table()) {
      const std::string name = std::string(section.str()) + "." + std::string(key.str());
      found.insert(name);
      toml::table changed = shipped;
      toml::node& node = *changed[section.str()][key.str()].node();
      if (name == "patches.fps_60") {
        CHECK(node.value_exact<std::string>() == std::string("auto"));
        changed.at_path("patches").as_table()->insert_or_assign("fps_60", false);
      } else if (name == "patches.turbo_audio") {
        CHECK(node.value_exact<std::string>() == std::string("natural"));
        changed.at_path("patches").as_table()->insert_or_assign("turbo_audio", "normal");
      } else if (name == "graphics.upscaling") {
        CHECK(node.value_exact<std::string>() == std::string("off"));
        changed.at_path("graphics").as_table()->insert_or_assign("upscaling", "metalfx-spatial");
      } else if (name == "graphics.shader_cache_target") {
        CHECK(node.value_exact<std::string>() == std::string("portable"));
        changed.at_path("graphics").as_table()->insert_or_assign("shader_cache_target", "native");
      } else {
        Mutate(node);
      }
      std::ostringstream text;
      text << changed;
      const fs::path mutated = work / "mutated.toml";
      WriteBytes(mutated, text.str());
      CHECK(fable2::config::Load(mutated));
      if (fable2::config::Get() == Values{}) {
        std::fprintf(stderr, "config_defaults_tests: Load() ignores %s\n", name.c_str());
        return 1;
      }
    }
  }
  for (const auto& key : kExpectedKeys) {
    if (!found.contains(key))
      std::fprintf(stderr, "config_defaults_tests: missing from file: %s\n", key.c_str());
  }
  for (const auto& key : found) {
    if (!kExpectedKeys.contains(key))
      std::fprintf(stderr, "config_defaults_tests: unexpected key in file: %s\n", key.c_str());
  }
  CHECK(found == kExpectedKeys);

  // 5. The pre-2026-10-12 boolean key `metalfx` still selects MetalFX spatial
  //    when `upscaling` is missing, and `upscaling` wins when both exist.
  const fs::path legacy = work / "legacy.toml";
  WriteBytes(legacy, "[graphics]\nmetalfx = true\n");
  CHECK(fable2::config::Load(legacy));
  CHECK(fable2::config::Get().upscaling == Values::Upscaling::kMetalFxSpatial);
  WriteBytes(legacy, "[graphics]\nmetalfx = true\nupscaling = \"off\"\n");
  CHECK(fable2::config::Load(legacy));
  CHECK(fable2::config::Get().upscaling == Values::Upscaling::kOff);

  // 6. resolution_scale = "auto" (src/core/fable2_res_auto.h); numbers leave it off.
  const fs::path res = work / "res.toml";
  WriteBytes(res, "[graphics]\nresolution_scale = \"auto\"\n");
  CHECK(fable2::config::Load(res));
  CHECK(fable2::config::Get().resolution_scale_auto);
  CHECK(fable2::config::Get().resolution_scale == 1);
  WriteBytes(res, "[graphics]\nresolution_scale = 2\n");
  CHECK(fable2::config::Load(res));
  CHECK(!fable2::config::Get().resolution_scale_auto);
  CHECK(fable2::config::Get().resolution_scale == 2);

  fs::remove_all(work);
  std::puts("config/fable2_config.toml matches the built-in defaults and is embedded byte-for-byte");
  return 0;
}
