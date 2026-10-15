// fable2_dir_manifest_heal.h
//
// Self-heals <game_data_root>/data/dir.manifest at startup.
//
// Why: the game's VFS indexes ALL of data/ from data/dir.manifest; a file that
// is not listed there is invisible to the guest, so content loads fail with
// null pointers (observed: deterministic "read of guest 0x0" AV at startup).
// The manifest is easy to lose or truncate:
//   - content migrated with an mtime-respecting copy (xcopy /D, robocopy, ...)
//     skips a newer build-created stub, or
//   - the build (tools/ensure_recomp_manifest.cmake) creates a stub containing
//     only the staged scripts\recomp\*.lua entries before the content exists.
// Fixing this by hand works but is fragile, so the exe repairs it itself.
//
// The manifest stores paths relative to the data/ root with backslashes and in
// the game's canonical (mostly lowercase) casing; the emulated FS matches
// case-insensitively (the shipped content ships e.g. as Shaders\Shaders.sbk
// while the manifest lists shaders\shaders.sbk), so we:
//   - compare existing entries case-insensitively,
//   - append missing entries in lowercase (the canonical casing),
//   - never rewrite or drop existing lines.
//
// Idempotent: a second run appends nothing. Entries are compared in the
// canonical form (lowercase, backslash separators); before 2026-10-07 the scan
// compared forward-slash paths against backslash entries, so every file in a
// subfolder looked missing and was re-appended on every launch (the manifest
// grew to 7.7 MB: 462 unique lines x ~500). Duplicate lines are therefore
// removed on startup (first occurrence kept, original order preserved);
// otherwise lines are only ever APPENDED, so hand-added lines (e.g. by
// tools/stage_content.cmd's merge or ensure_recomp_manifest) survive.
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/logging/macros.h>  // REXSYS_* logging

namespace fable2::manifestheal {

// Ensures every file under <game_data_root>/data/ has an entry in
// <game_data_root>/data/dir.manifest. Returns the number of entries appended
// (0 when the manifest was already complete). Never fails hard: problems are
// logged and the startup continues with whatever manifest exists.
inline int EnsureComplete(const std::filesystem::path& game_data_root) {
  const std::filesystem::path manifest = game_data_root / "data" / "dir.manifest";
  std::error_code ec;
  const std::filesystem::path data_root = manifest.parent_path();
  if (!std::filesystem::is_directory(data_root, ec)) {
    REXSYS_WARN("[manifest] no {} directory; skipping dir.manifest self-heal",
                data_root.string());
    return 0;
  }

  // Canonical comparison key: lowercase, backslash separators.
  const auto canonical = [](std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  };

  // 1. Existing entries (compared canonically); drop duplicate lines.
  std::vector<std::string> existing;
  std::vector<std::string> unique_lines;
  size_t duplicates = 0;
  {
    std::ifstream in(manifest, std::ios::binary);
    std::string line;
    std::unordered_set<std::string> seen;
    if (in) {
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::string key = canonical(line);
        if (!seen.insert(key).second) {
          ++duplicates;
          continue;
        }
        unique_lines.push_back(line);
        existing.push_back(std::move(key));
      }
    }
  }
  std::sort(existing.begin(), existing.end());
  if (duplicates > 0) {
    // Rewrite with the unique lines in their original order (CRLF). Written to a
    // sibling temp file and renamed over the manifest so a crash cannot leave a
    // truncated index.
    const std::filesystem::path tmp = manifest.string() + ".dedupe-tmp";
    bool ok = false;
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (out) {
        for (const auto& l : unique_lines) out << l << "\r\n";
        out.flush();
        ok = bool(out);
      }
    }
    if (ok) std::filesystem::rename(tmp, manifest, ec);
    if (!ok || ec) {
      std::filesystem::remove(tmp, ec);
      REXSYS_WARN("[manifest] could not rewrite {} without its {} duplicate line(s)",
                  manifest.string(), duplicates);
    } else {
      REXSYS_INFO("[manifest] removed {} duplicate line(s) from {} ({} unique entries kept)",
                  duplicates, manifest.string(), unique_lines.size());
    }
  }

  // 2. Everything under data/ (relative, backslash form, lowercased).
  std::vector<std::string> missing;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(
           data_root,
           std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string rel =
        std::filesystem::relative(entry.path(), data_root, ec).generic_string();
    // The index itself and our own dedupe temp file are never entries.
    if (rel.empty() || rel == "dir.manifest" || rel == "dir.manifest.dedupe-tmp") continue;
    rel = canonical(std::move(rel));
    if (std::binary_search(existing.begin(), existing.end(), rel)) continue;
    missing.push_back(std::move(rel));
  }
  if (missing.empty()) {
    REXSYS_INFO("[manifest] complete: {} entries, 0 appended ({})", existing.size(),
                manifest.string());
    return 0;
  }

  std::sort(missing.begin(), missing.end());

  // 3. Append (CRLF), starting a fresh line if the file did not end with one.
  std::ofstream out(manifest, std::ios::binary | std::ios::app);
  if (!out) {
    REXSYS_ERROR("[manifest] cannot open {} for append; guest VFS index will "
                 "be incomplete ({} unlisted file(s))",
                 manifest.string(), missing.size());
    return 0;
  }
  {
    std::ifstream in(manifest, std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    if (!all.empty() && all.back() != '\n') out << "\r\n";
  }
  for (const auto& m : missing) {
    out << m << "\r\n";  // already canonical: lowercase, backslashes
  }
  out.flush();
  REXSYS_INFO("[manifest] appended {} missing entr{} to {}", missing.size(),
              missing.size() == 1 ? "y" : "ies", manifest.string());
  return static_cast<int>(missing.size());
}

}  // namespace fable2::manifestheal
