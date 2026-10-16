// Character Editor mod (opt-in, off by default): host side. docs/character-editor.md.
//
// The editor itself is game Lua (src/lua/character_editor.lua) that edits the hero through the game's
// own stat API. The host does two things, and only when FABLE2_CHARACTER_EDITOR=1 (launcher: General ->
// Mods -> Character Editor):
//   1. F6 entry: the F6 key runs scripts/recomp/F6_character_editor.lua instead of the default F6.lua.
//      That file marks the mod as on and runs F6.lua, which then lists "Character Editor".
//      (A custom f6_lua_path is left alone.)
//   2. Backup: when the editor opens it RunScript()s the no-op character_editor_backup.lua. The
//      RunScript probe (src/core/fable2_f5_lua.h) calls OnBackupRequest() on the guest main thread,
//      synchronously inside that call: it copies the save folders to
//        $FABLE2_CHARACTER_EDITOR_BACKUP_DIR/<timestamp>/  (default <state dir>/character-editor-backups)
//      and writes the answer to data/scripts/recomp/character_editor_status.lua (fixed 256 bytes, like
//      the in-game message slot) as `return "<count>", "ok"|"<error>"`. The editor refuses to change
//      anything unless the count changed and the result is "ok". The count is a string because the
//      game's Lua numbers are single-precision floats (two counts near the Unix time compare equal);
//      it starts at the Unix time, so it differs from whatever an earlier run left there.
// With the mod off: F6 runs F6.lua as before, the probe never matches, nothing is written.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/logging/macros.h>

#include "fable2_state_path.h"

namespace fable2::character_editor {

inline constexpr const char* kDefaultF6 = "scripts/recomp/F6.lua";
inline constexpr const char* kModF6 = "scripts/recomp/F6_character_editor.lua";
inline constexpr const char* kBackupAck = "character_editor_backup.lua";
inline constexpr const char* kStatusFile = "scripts/recomp/character_editor_status.lua";
inline constexpr size_t kStatusSize = 256;
inline constexpr size_t kKeepBackups = 20;

inline bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_CHARACTER_EDITOR");  // getenv-ok: once
    const bool enabled = v && v[0] == '1';
    if (enabled) REXSYS_INFO("[character-editor] mod on (F6 -> Character Editor)");
    return enabled;
  }();
  return on;
}

inline std::filesystem::path& DataRoot() {
  static std::filesystem::path root;  // <game_data_root>/data
  return root;
}

// `game_data_root` is the folder that contains data/.
inline void SetGameDataRoot(const std::filesystem::path& game_data_root) {
  DataRoot() = game_data_root / "data";
}

// The F6 script path to run: the mod's entry point instead of the default menu when the mod is on.
inline std::string F6Path(const std::string& configured) {
  if (Enabled() && configured == kDefaultF6) return kModF6;
  return configured;
}

inline bool IsBackupAck(const char* path) {
  return Enabled() && std::string(path).find(kBackupAck) != std::string::npos;
}

// Answer counter, seeded from the clock so it differs from any count a previous run left in the status
// file (the editor proceeds only when the count changed).
inline std::atomic<uint32_t> g_answers{static_cast<uint32_t>(std::time(nullptr))};

inline std::filesystem::path BackupRoot() {
  if (const char* v = std::getenv("FABLE2_CHARACTER_EDITOR_BACKUP_DIR")) {  // getenv-ok: per editor open
    if (v[0]) return std::filesystem::path(v);
  }
  return fable2::StateDirectory() / "character-editor-backups";
}

// Save folders to back up: every title folder (4D5307F1) under <state>/saves, i.e. the hero slots and
// their headers. Returns false when there is nothing to back up.
inline std::vector<std::filesystem::path> SaveFolders(const std::filesystem::path& saves) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(saves, ec), end; !ec && it != end; it.increment(ec)) {
    if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
    if (it->is_directory(ec) && it->path().filename() == "4D5307F1") {
      out.push_back(it->path());
      it.disable_recursion_pending();
    }
  }
  return out;
}

inline std::string Timestamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d_%H-%M-%S", &tm);
  return buf;
}

// Copies the save folders. Returns "ok" or an error.
inline std::string Backup(std::filesystem::path* made) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path saves = fable2::StateDirectory() / "saves";
  const auto folders = SaveFolders(saves);
  if (folders.empty()) return "no save folder under " + saves.string();
  const fs::path root = BackupRoot();
  fs::path dest = root / Timestamp();
  for (int n = 2; fs::exists(dest, ec); ++n) dest = root / (Timestamp() + "-" + std::to_string(n));
  for (const auto& folder : folders) {
    const fs::path target = dest / fs::relative(folder, saves, ec);
    fs::create_directories(target, ec);
    if (ec) return "cannot create " + target.string() + ": " + ec.message();
    fs::copy(folder, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return "copy failed: " + ec.message();
  }
  *made = dest;
  // Keep the newest kKeepBackups (names sort by time).
  std::vector<fs::path> all;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    if (e.is_directory(ec)) all.push_back(e.path());
  }
  std::sort(all.begin(), all.end());
  for (size_t i = 0; i + kKeepBackups < all.size(); ++i) fs::remove_all(all[i], ec);
  return "ok";
}

inline bool WriteStatus(uint32_t count, const std::string& result) {
  std::string r = result.substr(0, 150);
  for (auto& c : r) {
    if (c == '"' || c == '\\' || c == '\n' || c == '\r') c = '\'';
  }
  std::string body = "-- written by the host (src/core/fable2_character_editor.h): backup count, result\nreturn \"" +
                     std::to_string(count) + "\", \"" + r + "\"\n";
  if (body.size() + 2 > kStatusSize) return false;
  body += std::string(kStatusSize - body.size() - 1, '-');
  body += '\n';
  // In place, no temp file (a new file under data/ would be picked up by the dir.manifest self-heal).
  std::ofstream out(DataRoot() / kStatusFile, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(body.data(), std::streamsize(body.size()));
  out.flush();
  return bool(out);
}

// Guest main thread, inside the editor's RunScript(character_editor_backup.lua) call.
inline void OnBackupRequest() {
  if (DataRoot().empty()) {
    REXSYS_INFO("[character-editor] backup requested before the data root was set; refusing");
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::filesystem::path made;
  std::string result;
  try {
    result = Backup(&made);
  } catch (const std::exception& e) {
    result = std::string("backup failed: ") + e.what();
  }
  const uint32_t n = g_answers.fetch_add(1) + 1;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!WriteStatus(n, result)) {
    REXSYS_INFO("[character-editor] could not write the status file; the editor will refuse");
    return;
  }
  if (result == "ok")
    REXSYS_INFO("[character-editor] save backed up to {} ({:.0f} ms, answer #{})", made.string(), ms, n);
  else
    REXSYS_INFO("[character-editor] backup FAILED ({}); the editor will refuse edits", result);
}

}  // namespace fable2::character_editor
