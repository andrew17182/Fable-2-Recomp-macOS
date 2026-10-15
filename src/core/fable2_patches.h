// fable2_patches - runtime guest-image patch system (Xenia patch.toml format).
//
// Patches are applied to the decrypted XEX image in the guest arena from
// Fable2App::OnPostLoadXexImage() - after the SDK has loaded default.xex
// into guest memory, before the guest module launches (the SDK documents
// this hook as the place for data patches).
//
// SCOPE: DATA PATCHES ONLY. This build executes *native* recompiled code,
// so guest .text bytes are never executed - a code-region op here only
// rewrites dead bytes (ApplyAll flags these in the log). Code-region Xenia
// patches must be implemented as mid-asm hooks instead: see
// src/core/fable2_hooks.cpp + [[entrypoint.midasm_hook]] in fable_2_manifest.toml
// (e.g. the 60 FPS patch, formerly a (inert) entry in this table).
// Data ops (BSS/.data/.rodata, i.e. addresses outside the code region)
// DO take effect, because the recompiled code reads/writes guest memory.
//
// The patch table is data-driven: Load() reads fable2_patches.toml (same
// format as Xenia's game-patches files) from next to the exe, creating it
// with the built-in defaults if missing. A missing or malformed file never
// stops the game - the built-in defaults are used instead. Each patch has
// an `enabled` toggle (default true).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/image_info.h>

namespace rex::memory {
class Memory;
}

// Defined by the codegen output (generated/default/fable_2_init.cpp).
extern const rex::PPCImageInfo PPCImageConfig;

namespace fable2::patches {

// One write to the guest image. Mirrors the Xenia patch file format:
//   [[patch.be8]] / [[patch.be16]] / [[patch.be32]] / [[patch.be64]]
//       address = 0x8233aeb4
//       value   = 0x60000000
// (in fable2_patches.toml: ops = [{width = "be8", address = 0x..., value = 0x...}])
struct Op {
  enum class Width { kBe8, kBe16, kBe32, kBe64 };
  Width width;
  uint32_t address;  // guest address
  uint64_t value;
};

struct Patch {
  std::string name;
  std::string desc;
  std::string author;
  bool is_enabled;
  std::vector<Op> ops;
};

// Load the patch table from `path` (fable2_patches.toml, next to the exe):
//   - File missing   -> writes the built-in template, then parses it
//                       (returns true).
//   - Parse failure  -> logs the error, shows a dialog, falls back to the
//                       built-in defaults (returns false).
// Safe to call only after the SDK logging is initialized.
bool Load(const std::filesystem::path& path);

// The patch table: the file contents if Load() succeeded (or the built-in
// defaults if it did not / was not called).
const std::vector<Patch>& Patches();

// Applies all enabled patches to the loaded guest image.
//   memory - the runtime guest memory manager (ReXApp::runtime()->memory()).
//   image  - image layout for bounds checks and code/data classification.
// Logs every write (old -> new value) plus a summary. Returns the number of
// ops applied (out-of-range ops are skipped with an error log).
size_t ApplyAll(rex::memory::Memory* memory, const rex::PPCImageInfo& image);

}  // namespace fable2::patches
