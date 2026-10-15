// See metal_ns_specialize.h and docs/native-shaders.md ("Hot-shader
// specialization").

#include "rex/graphics/metal/metal_ns_specialize.h"
#include "rex/graphics/metal/metal_readiness.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <tuple>
#include <vector>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <condition_variable>
#include <thread>
#include <pthread.h>
#include <sys/qos.h>

#include "fmt/format.h"
#include "rex/graphics/pipeline/shader/dxbc_translator.h"
#include "rex/logging.h"
#include "xxhash.h"

namespace rex {
namespace graphics {
namespace metal {
namespace ns_spec {

namespace {

std::vector<uint64_t> ParseHashList(const char* v) {
  std::vector<uint64_t> list;
  while (v && *v) {
    char* end = nullptr;
    uint64_t h = std::strtoull(v, &end, 16);
    if (end == v) break;
    list.push_back(h);
    if (*end != ',') break;
    v = end + 1;
  }
  return list;
}

const char* kKindName[kKindCount] = {"sys", "bl", "fc"};

}  // namespace

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_NS_SPECIALIZE");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool LearnMode() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_NS_SPECIALIZE_LEARN");
    return v && v[0] == '1';
  }();
  return on;
}

bool VerifyMode() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_NS_SPECIALIZE_VERIFY");
    return v && v[0] == '1';
  }();
  return on;
}

bool BakeFloats() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_NS_SPECIALIZE_FLOATS");
    return !(v && v[0] == '0');
  }();
  return on;
}

uint32_t MinDraws() {
  static const uint32_t n = [] {
    const char* v = std::getenv("FABLE2_NS_SPECIALIZE_MIN_DRAWS");
    return v && v[0] ? uint32_t(std::strtoul(v, nullptr, 10)) : 16u;
  }();
  return n;
}

bool Candidate(uint64_t ucode_hash) {
  struct Lists {
    bool all = false;
    std::vector<uint64_t> list;
    std::vector<uint64_t> skip;
  };
  static const Lists lists = [] {
    Lists l;
    // Built-in: the hottest guest pixel shaders at Bower Lake 3x and Old Tomb
    // (docs/engine-options-20261011.md §9.1, docs/native-shaders.md).
    l.list = {
        UINT64_C(0xAF1E16EE57E4BB06), UINT64_C(0x0F99810F072BF0FC),
        UINT64_C(0xF1EB2497108F159D), UINT64_C(0x577CB5267A8C92C3),
        UINT64_C(0x7CD57B81550F19E3), UINT64_C(0x1B4B5AE12EF270CA),
        UINT64_C(0x4BE4344C27016E04), UINT64_C(0x44B567635EB10620),
        UINT64_C(0x04F23292B2F44077), UINT64_C(0x686DE15FC84D8B73),
        UINT64_C(0x0542FFDF2B118509), UINT64_C(0x7928BAA7A6691A5D),
        UINT64_C(0x06547412E4B72251), UINT64_C(0x648B965C200A5113),
        UINT64_C(0x356E4E7B9E2B36E6), UINT64_C(0xCD2CE6D440B77BCE),
        UINT64_C(0x28E202861D3E2411), UINT64_C(0xF2F103F16764043B),
    };
    if (const char* v = std::getenv("FABLE2_NS_SPECIALIZE_LIST")) {
      if (std::strcmp(v, "all") == 0) {
        l.all = true;
      } else {
        l.list = ParseHashList(v);
      }
    }
    // Built-in skip list: shaders whose specialized variant measured not
    // faster than their normal pipeline (per-shader A/B).
    // 4BE4344C27016E04: its own pass (2 draws/frame, Bower Lake) +5-6% at
    // 1x/2x/3x with the variant.
    l.skip = {UINT64_C(0x4BE4344C27016E04)};
    if (const char* v = std::getenv("FABLE2_NS_SPECIALIZE_SKIP")) {
      l.skip = ParseHashList(v);
    }
    return l;
  }();
  if (std::find(lists.skip.begin(), lists.skip.end(), ucode_hash) !=
      lists.skip.end()) {
    return false;
  }
  return lists.all || std::find(lists.list.begin(), lists.list.end(),
                                ucode_hash) != lists.list.end();
}

PlainKey ComputePlainKey(const std::vector<uint8_t>& metallib) {
  PlainKey key;
  if (metallib.empty()) return key;
  XXH128_hash_t h = XXH3_128bits(metallib.data(), metallib.size());
  key.low = h.low64;
  key.high = h.high64;
  key.size = metallib.size();
  return key;
}

Variant::~Variant() {
  if (function) function->release();
  if (library) library->release();
}

std::vector<GuardEntry> VariantEntries(const Variant& v) {
  std::vector<GuardEntry> out;
  for (uint32_t k = 0; k < kKindCount; ++k) {
    for (size_t i = 0; i < v.index[k].size(); ++i) {
      out.push_back({k, v.index[k][i], v.value[k][i], v.mask[k][i]});
    }
  }
  return out;
}

void SetVariantEntries(Variant& v, std::vector<GuardEntry> entries) {
  std::sort(entries.begin(), entries.end(),
            [](const GuardEntry& a, const GuardEntry& b) {
              return a.kind != b.kind ? a.kind < b.kind : a.index < b.index;
            });
  for (uint32_t k = 0; k < kKindCount; ++k) {
    v.index[k].clear();
    v.value[k].clear();
    v.mask[k].clear();
  }
  for (const GuardEntry& e : entries) {
    if (e.kind >= kKindCount) continue;
    v.index[e.kind].push_back(e.index);
    v.value[e.kind].push_back(e.value & e.mask);
    v.mask[e.kind].push_back(e.mask);
  }
  v.guard_count = uint32_t(entries.size());
  v.values_hash = HashEntries(entries);
}

uint64_t HashEntries(const std::vector<GuardEntry>& entries) {
  std::vector<GuardEntry> sorted = entries;
  std::sort(sorted.begin(), sorted.end(),
            [](const GuardEntry& a, const GuardEntry& b) {
              return a.kind != b.kind ? a.kind < b.kind : a.index < b.index;
            });
  std::vector<uint32_t> words;
  for (const GuardEntry& e : sorted) {
    words.push_back(e.kind);
    words.push_back(e.index);
    words.push_back(e.value & e.mask);
    words.push_back(e.mask);
  }
  return XXH3_64bits(words.data(), words.size() * sizeof(uint32_t)) |
         1;  // never 0
}


std::vector<GuardEntry> LearnRecord::Candidates() const {
  std::vector<GuardEntry> out;
  for (uint32_t k = 0; k < kKindCount; ++k) {
    if (draws[k] < MinDraws()) continue;
    for (size_t i = 0; i < stats[k].size(); ++i) {
      const DwordStat& d = stats[k][i];
      if (d.seen != draws[k]) continue;  // not seen in every draw
      const uint32_t invariant = ~d.varying;
      if (invariant) {
        out.push_back({k, uint32_t(i), d.first & invariant, invariant});
      }
    }
  }
  return out;
}

uint64_t LearnHash(const LearnRecord& record) {
  std::vector<GuardEntry> c = record.Candidates();
  uint64_t h = HashEntries(c);
  // Bake version 3: bit-field, mask, shift and truncate uses (masked guards).
  uint64_t extra[3] = {h, 3, uint64_t(BakeFloats()) | 0x4E5332ull << 8};
  return XXH3_64bits(extra, sizeof(extra)) | 1;
}

// ---------------------------------------------------------------------------
// Learn data
// ---------------------------------------------------------------------------

namespace {

struct KeyHash {
  size_t operator()(const PlainKey& k) const {
    return size_t(k.low ^ (k.high * 0x9E3779B97F4A7C15ull) ^ k.size);
  }
};

struct ShaderId {
  uint64_t ucode;
  uint64_t modification;
  bool operator==(const ShaderId& o) const {
    return ucode == o.ucode && modification == o.modification;
  }
};
struct ShaderIdHash {
  size_t operator()(const ShaderId& s) const {
    return size_t(s.ucode ^ (s.modification * 0x9E3779B97F4A7C15ull));
  }
};

using RecordMap = std::unordered_map<PlainKey, LearnRecord, KeyHash>;

std::mutex g_learn_mutex;
// Loaded at storage init plus, with auto-learn, records that converged in this
// session (g_learn_mutex).
RecordMap g_records;
// Plain libraries per shader (ucode + modification), for scale-independent
// lookups (g_learn_mutex).
std::unordered_map<ShaderId, std::vector<PlainKey>, ShaderIdHash> g_siblings;
// Auto-learn: last snapshot of the records still sampling (saved, but not
// used for variants) (g_learn_mutex).
RecordMap g_partial;
bool g_dirty = false;
std::filesystem::path g_learn_path;
bool g_storage_ready = false;

// Learn mode, command-processor thread.
struct LiveStats {
  LearnRecord record;
};
std::unordered_map<const void*, LiveStats*> g_live;
std::vector<std::unique_ptr<LiveStats>> g_live_storage;
uint64_t g_notes = 0;
std::chrono::steady_clock::time_point g_last_save{};

// Auto-learn state, owned here (command-processor thread only).
std::vector<std::unique_ptr<AutoLearn>> g_auto;
std::chrono::steady_clock::time_point g_last_snapshot{};
bool g_snapshot_due = false;

// Background writer for auto-learn saves (the command-processor thread only
// snapshots).
// Detached; lives until the process exits.
std::condition_variable g_saver_cond;
bool g_saver_request = false;

void MergeStat(DwordStat& into, const DwordStat& from) {
  if (!from.seen) return;
  if (!into.seen) {
    into = from;
    return;
  }
  into.varying |= from.varying | (from.first ^ into.first);
  into.seen += from.seen;
}

void MergeRecord(LearnRecord& into, const LearnRecord& from) {
  for (uint32_t k = 0; k < kKindCount; ++k) {
    if (into.stats[k].size() < from.stats[k].size()) {
      into.stats[k].resize(from.stats[k].size());
    }
    for (size_t i = 0; i < from.stats[k].size(); ++i) {
      MergeStat(into.stats[k][i], from.stats[k][i]);
    }
    into.draws[k] += from.draws[k];
  }
  into.flags |= from.flags;
  into.demotions = std::max(into.demotions, from.demotions);
}

// System constant dwords whose values depend on the resolution scale (not on
// the guest): never transferred from another scale's record.
bool ScaleDependentSystemDword(uint32_t index) {
  using SC = DxbcShaderTranslator::SystemConstants;
  struct Range {
    size_t begin, end;
  };
  static const Range ranges[] = {
      // ndc_scale .. point_screen_diameter_to_ndc_radius.
      {offsetof(SC, ndc_scale), offsetof(SC, texture_swizzled_signs)},
      {offsetof(SC, textures_resolution_scaled),
       offsetof(SC, textures_resolution_scaled) + 4},
      {offsetof(SC, edram_32bpp_tile_pitch_dwords_scaled),
       offsetof(SC, color_exp_bias)},
      {offsetof(SC, edram_rt_base_dwords_scaled),
       offsetof(SC, edram_rt_format_flags)},
  };
  const size_t byte = size_t(index) * 4;
  for (const Range& r : ranges) {
    if (byte >= r.begin && byte < r.end) return true;
  }
  return false;
}

void AddRecordLocked(const LearnRecord& r) {
  auto [it, inserted] = g_records.emplace(r.key, r);
  if (!inserted) {
    MergeRecord(it->second, r);
  } else {
    g_siblings[{r.ucode_hash, r.modification}].push_back(r.key);
  }
}

// Text format, one shader per block:
//   ps <ucode> <modification> <low> <high> <size> <draws sys> <draws bl>
//      <draws fc> [<flags> <demotions>]
//   k <kind> <dword count>
//   <index> <first value> <bits that varied> <draws seen>   (only dwords seen)
// Values and hashes are hex. Records without flags (learn sessions before
// auto-learn) count as converged.
bool LoadFile(const std::filesystem::path& path, RecordMap& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  if (!std::getline(in, line) || line.rfind("NS2LEARN 2", 0) != 0) {
    return false;
  }
  LearnRecord* rec = nullptr;
  uint32_t kind = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream s(line);
    std::string tag;
    s >> tag;
    if (tag == "ps") {
      LearnRecord r;
      s >> std::hex >> r.ucode_hash >> r.modification >> r.key.low >>
          r.key.high >> std::dec >> r.key.size >> r.draws[0] >> r.draws[1] >>
          r.draws[2];
      if (!s || !r.key.valid()) {
        rec = nullptr;
        continue;
      }
      uint32_t flags = 0, demotions = 0;
      if (s >> flags >> demotions) {
        r.flags = flags;
        r.demotions = demotions;
      } else {
        r.flags = LearnRecord::kConverged;
      }
      // The file is an append-only journal (FABLE2_NS_LEARN_APPEND): every
      // record is a complete snapshot, so a later record replaces an earlier
      // one of the same key. (Files written as one snapshot hold each key once.)
      LearnRecord& slot = out[r.key];
      slot = r;
      rec = &slot;
    } else if (tag == "k" && rec) {
      uint32_t count = 0;
      s >> kind >> count;
      if (kind >= kKindCount || count > 4096) {
        rec = nullptr;
        continue;
      }
      if (rec->stats[kind].size() < count) rec->stats[kind].resize(count);
    } else if (rec && kind < kKindCount) {
      uint32_t index = uint32_t(std::strtoul(tag.c_str(), nullptr, 10));
      if (index >= rec->stats[kind].size()) continue;
      DwordStat stat;
      s >> std::hex >> stat.first >> stat.varying >> std::dec >> stat.seen;
      if (!s) continue;
      MergeStat(rec->stats[kind][index], stat);
    }
  }
  return true;
}

void WriteRecord(std::string& out, const LearnRecord& r);
std::string RecordText(const LearnRecord& r) {
  std::string out;
  WriteRecord(out, r);
  return out;
}

void WriteRecord(std::string& out, const LearnRecord& r) {
  out += fmt::format("ps {:016X} {:X} {:016X} {:016X} {} {} {} {} {} {}\n",
                     r.ucode_hash, r.modification, r.key.low, r.key.high,
                     r.key.size, r.draws[0], r.draws[1], r.draws[2], r.flags,
                     r.demotions);
  for (uint32_t k = 0; k < kKindCount; ++k) {
    if (r.stats[k].empty()) continue;
    out += fmt::format("k {} {}\n", k, r.stats[k].size());
    for (size_t i = 0; i < r.stats[k].size(); ++i) {
      const DwordStat& d = r.stats[k][i];
      if (!d.seen) continue;
      out += fmt::format("{} {:X} {:X} {}\n", i, d.first, d.varying, d.seen);
    }
  }
}

// [learn-append] The learn file is an append-only journal (default on;
// FABLE2_NS_LEARN_APPEND=0 = one snapshot rewritten in place, as before). The
// launcher publishes a session's grown cache to the live cache only when every
// changed file is an extension of the live one (tools/fable2_dev_session.py
// publish_cache); a rewrite blocked the whole publish from the second session
// on. Each save appends the records whose text changed since the last save. A
// record is a complete snapshot, so the loader keeps the last one per key.
// When the file grows past max(1 MB, 4x its compacted size), the compacted
// records go into a NEW file of the next generation
// (<title>.ns2-learn.<gen>.txt; generation 0 is <title>.ns2-learn.txt) and the
// older generations of this cache are deleted. Startup reads the highest
// generation.
bool LearnAppendMode() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_NS_LEARN_APPEND");
    return !(v && v[0] == '0');
  }();
  return on;
}

constexpr char kLearnHeader[] = "NS2LEARN 2\n";
// Testing: FABLE2_NS_LEARN_COMPACT_KB=<n> compacts once the file passes n KB
// (instead of max(1 MB, 4x the compacted size)).
uint64_t LearnCompactTestBytes() {
  static const uint64_t n = [] {
    const char* v = std::getenv("FABLE2_NS_LEARN_COMPACT_KB");
    return v && v[0] ? uint64_t(std::strtoull(v, nullptr, 10)) * 1024 : uint64_t(0);
  }();
  return n;
}

// File state (g_file_mutex; taken after g_learn_mutex is released).
std::mutex g_file_mutex;
std::unordered_map<PlainKey, std::string, KeyHash> g_written;
std::filesystem::path g_title_root;
uint32_t g_title_id = 0;
uint32_t g_gen = 0;
uint64_t g_file_bytes = 0;
uint64_t g_compact_bytes = 0;

std::filesystem::path LearnGenPath(uint32_t gen) {
  return g_title_root /
         (gen ? fmt::format("{:08X}.ns2-learn.{}.txt", g_title_id, gen)
              : fmt::format("{:08X}.ns2-learn.txt", g_title_id));
}

// Generation of a learn file name of this title, or -1.
int64_t LearnGenOf(const std::string& name) {
  const std::string base = fmt::format("{:08X}.ns2-learn.", g_title_id);
  if (name.rfind(base, 0) != 0) return -1;
  const std::string rest = name.substr(base.size());
  if (rest == "txt") return 0;
  const size_t dot = rest.find('.');
  if (dot == std::string::npos || rest.substr(dot) != ".txt" || dot == 0) {
    return -1;
  }
  for (size_t i = 0; i < dot; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(rest[i]))) return -1;
  }
  return int64_t(std::strtoull(rest.substr(0, dot).c_str(), nullptr, 10));
}

std::string RecordText(const LearnRecord& r);

std::vector<const std::pair<const PlainKey, std::string>*> SortedWritten() {
  std::vector<const std::pair<const PlainKey, std::string>*> v;
  v.reserve(g_written.size());
  for (const auto& e : g_written) v.push_back(&e);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) {
    return std::tie(a->first.low, a->first.high, a->first.size) <
           std::tie(b->first.low, b->first.high, b->first.size);
  });
  return v;
}

bool WriteWhole(const std::filesystem::path& path, const std::string& out) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(out.data(), std::streamsize(out.size()));
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}

void LogLearnSave(size_t shaders, uint64_t bytes, const char* how) {
  static std::atomic<uint32_t> saves{0};
  const uint32_t n = saves.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 2 || (n & (n - 1)) == 0) {
    REXLOG_INFO("[ns-spec] learn file saved: {} pixel shaders, {} KB ({})",
                shaders, bytes / 1024, how);
  }
}

// Writes `records` (the complete current state) to the learn file. In append
// mode the file is this cache's current generation (g_file_mutex), not `path`.
void WriteFile(const std::filesystem::path& path, const RecordMap& records) {
  if (!LearnAppendMode()) {
    std::string out = kLearnHeader;
    for (const auto& [key, r] : records) out += RecordText(r);
    if (WriteWhole(path, out)) LogLearnSave(records.size(), out.size(), "rewrite");
    return;
  }
  std::lock_guard<std::mutex> lock(g_file_mutex);
  std::vector<std::pair<PlainKey, std::string>> changed;
  for (const auto& [key, r] : records) {
    std::string text = RecordText(r);
    auto it = g_written.find(key);
    if (it != g_written.end() && it->second == text) continue;
    changed.emplace_back(key, std::move(text));
  }
  if (changed.empty()) return;
  std::sort(changed.begin(), changed.end(), [](const auto& a, const auto& b) {
    return std::tie(a.first.low, a.first.high, a.first.size) <
           std::tie(b.first.low, b.first.high, b.first.size);
  });
  std::string append = g_file_bytes ? std::string() : std::string(kLearnHeader);
  for (auto& [key, text] : changed) {
    append += text;
    g_written[key] = std::move(text);
  }
  const uint64_t limit =
      LearnCompactTestBytes()
          ? LearnCompactTestBytes()
          : std::max(uint64_t(1) << 20, 4 * g_compact_bytes);
  if (g_file_bytes + append.size() > limit) {
    std::string out = kLearnHeader;
    for (const auto* e : SortedWritten()) out += e->second;
    const std::filesystem::path next = LearnGenPath(g_gen + 1);
    if (WriteWhole(next, out)) {
      std::error_code ec;
      std::filesystem::remove(LearnGenPath(g_gen), ec);
      ++g_gen;  // later saves go to the new generation (LearnGenPath(g_gen))
      g_file_bytes = g_compact_bytes = out.size();
      REXLOG_INFO("[ns-spec] learn file compacted into {} ({} KB)",
                  next.filename().string(), out.size() / 1024);
      LogLearnSave(g_written.size(), out.size(), "compacted");
    }
    return;
  }
  const std::filesystem::path current = LearnGenPath(g_gen);
  std::ofstream f(current, std::ios::binary | std::ios::app);
  f.write(append.data(), std::streamsize(append.size()));
  if (!f) return;
  g_file_bytes += append.size();
  LogLearnSave(g_written.size(), g_file_bytes, "appended");
}

// Auto-learn: what goes into the file (the records plus the partial ones).
RecordMap AutoSnapshotLocked() {
  RecordMap merged = g_records;
  for (const auto& [key, r] : g_partial) {
    auto [it, inserted] = merged.emplace(key, r);
    if (!inserted) {
      // Keep the stored flags/demotions; add this session's draws.
      const uint32_t flags = it->second.flags;
      MergeRecord(it->second, r);
      it->second.flags = flags;
    }
  }
  return merged;
}

void SaverThread() {
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  std::unique_lock<std::mutex> lock(g_learn_mutex);
  for (;;) {
    g_saver_cond.wait(lock, [] { return g_saver_request; });
    g_saver_request = false;
    if (!g_dirty) continue;
    g_dirty = false;
    RecordMap merged = AutoSnapshotLocked();
    std::filesystem::path path = g_learn_path;
    lock.unlock();
    WriteFile(path, merged);
    lock.lock();
  }
}

void RequestSaveLocked() {
  g_dirty = true;
  g_saver_request = true;
  g_saver_cond.notify_one();
}

}  // namespace

bool AutoLearnEnabled() {
  static const bool on = [] {
    if (LearnMode() || !Enabled()) return false;
    const char* v = std::getenv("FABLE2_NS_SPECIALIZE_AUTOLEARN");
    return !(v && v[0] == '0');
  }();
  return on;
}

uint32_t DemotePercent() {
  static const uint32_t n = [] {
    const char* v = std::getenv("FABLE2_NS_SPECIALIZE_DEMOTE_PCT");
    return v && v[0] ? uint32_t(std::strtoul(v, nullptr, 10)) : 5u;
  }();
  return n;
}

uint32_t AutoLearnSampleEvery() {
  static const uint32_t n = [] {
    const char* v = std::getenv("FABLE2_NS_SPECIALIZE_AUTOLEARN_SAMPLE");
    uint32_t x = v && v[0] ? uint32_t(std::strtoul(v, nullptr, 10)) : 4u;
    return std::max(x, 1u);
  }();
  return n;
}

namespace {
uint32_t AutoLearnMinSamples() {
  static const uint32_t n = [] {
    const char* v = std::getenv("FABLE2_NS_SPECIALIZE_AUTOLEARN_MIN_SAMPLES");
    uint32_t x = v && v[0] ? uint32_t(std::strtoul(v, nullptr, 10)) : 64u;
    return std::max(x, MinDraws());
  }();
  return n;
}
constexpr uint32_t kAutoQuietSamples = 48;
constexpr auto kAutoQuietTime = std::chrono::seconds(3);
constexpr auto kAutoMinSpan = std::chrono::seconds(5);
constexpr auto kAutoSnapshotInterval = std::chrono::seconds(15);
constexpr uint32_t kDemoteWindow = 1024;
}  // namespace

void InitializeStorage(const std::filesystem::path& title_root,
                       uint32_t title_id) {
  std::lock_guard<std::mutex> lock(g_learn_mutex);
  if (g_storage_ready) return;
  g_storage_ready = true;
  g_title_root = title_root;
  g_title_id = title_id;
  g_learn_path = LearnGenPath(0);
  if (!Enabled() && !LearnMode()) return;
  if (LearnAppendMode()) {
    // Highest generation wins; older ones in this cache are superseded.
    int64_t best = -1;
    std::vector<std::pair<int64_t, std::filesystem::path>> found;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(title_root, ec)) {
      const int64_t gen = LearnGenOf(entry.path().filename().string());
      if (gen < 0 || !entry.is_regular_file(ec)) continue;
      found.emplace_back(gen, entry.path());
      best = std::max(best, gen);
    }
    if (best > 0) {
      for (const auto& [gen, path] : found) {
        if (gen < best) std::filesystem::remove(path, ec);
      }
    }
    std::lock_guard<std::mutex> file_lock(g_file_mutex);
    g_gen = best > 0 ? uint32_t(best) : 0;
    g_learn_path = LearnGenPath(g_gen);
    g_file_bytes = std::filesystem::exists(g_learn_path, ec)
                       ? uint64_t(std::filesystem::file_size(g_learn_path, ec))
                       : 0;
    if (ec) g_file_bytes = 0;
  }
  RecordMap loaded;
  if (LoadFile(g_learn_path, loaded)) {
    if (LearnAppendMode()) {
      // What is on disk now: unchanged records are not appended again.
      std::lock_guard<std::mutex> file_lock(g_file_mutex);
      uint64_t compact = sizeof(kLearnHeader) - 1;
      for (const auto& [key, r] : loaded) {
        std::string text = RecordText(r);
        compact += text.size();
        g_written.emplace(key, std::move(text));
      }
      g_compact_bytes = compact;
    }
    size_t converged = 0;
    for (const auto& [key, r] : loaded) {
      AddRecordLocked(r);
      converged += (r.flags & LearnRecord::kConverged) ? 1 : 0;
    }
    REXLOG_INFO("[ns-spec] learn data: {} pixel shaders ({} converged) from {}",
                loaded.size(), converged, g_learn_path.filename().string());
  } else if (LearnMode()) {
    REXLOG_INFO("[ns-spec] learn mode: new learn file {}",
                g_learn_path.filename().string());
  }
  if (AutoLearnEnabled()) {
    REXLOG_INFO("[ns-spec] auto-learn on: 1 in {} draws of candidate shaders "
                "without a variant, until converged",
                AutoLearnSampleEvery());
    std::thread(SaverThread).detach();
  }
}

namespace {
bool GetLearnRecordLocked(const PlainKey& key, uint64_t ucode_hash,
                          uint64_t modification, LearnRecord* out,
                          bool* blacklisted) {
  *blacklisted = false;
  auto exact_it = g_records.find(key);
  const LearnRecord* exact =
      exact_it != g_records.end() ? &exact_it->second : nullptr;
  if (exact && exact->demotions >= LearnRecord::kMaxDemotions) {
    *blacklisted = true;
    return false;
  }
  bool ready = exact && (exact->flags & LearnRecord::kConverged);
  auto sib_it = g_siblings.find({ucode_hash, modification});
  std::vector<const LearnRecord*> others;
  if (sib_it != g_siblings.end()) {
    for (const PlainKey& k : sib_it->second) {
      if (k == key) continue;
      auto it = g_records.find(k);
      if (it == g_records.end() ||
          !(it->second.flags & LearnRecord::kConverged)) {
        continue;
      }
      others.push_back(&it->second);
      ready = true;
    }
  }
  if (!ready) return false;
  LearnRecord r;
  r.key = key;
  r.ucode_hash = ucode_hash;
  r.modification = modification;
  if (exact) {
    r = *exact;
  }
  for (const LearnRecord* o : others) {
    MergeRecord(r, *o);
  }
  r.flags |= LearnRecord::kConverged;
  if (!others.empty()) {
    // Scale-dependent system dwords: this library's own statistics only.
    std::vector<DwordStat>& sys = r.stats[kSystem];
    for (size_t i = 0; i < sys.size(); ++i) {
      if (!ScaleDependentSystemDword(uint32_t(i))) continue;
      DwordStat d;
      d.seen = r.draws[kSystem];
      d.varying = 0xFFFFFFFFu;
      if (exact && i < exact->stats[kSystem].size()) {
        const DwordStat& e = exact->stats[kSystem][i];
        if (e.seen && e.seen == exact->draws[kSystem]) {
          d.first = e.first;
          d.varying = e.varying;
        }
      }
      sys[i] = d;
    }
  }
  r.demotions = exact ? exact->demotions : 0;
  *out = std::move(r);
  return true;
}
}  // namespace

bool GetLearnRecord(const PlainKey& key, uint64_t ucode_hash,
                    uint64_t modification, LearnRecord* out,
                    bool* blacklisted) {
  std::lock_guard<std::mutex> lock(g_learn_mutex);
  return GetLearnRecordLocked(key, ucode_hash, modification, out,
                              blacklisted);
}

bool RecordDemotion(const PlainKey& key, const Variant& v) {
  std::lock_guard<std::mutex> lock(g_learn_mutex);
  LearnRecord r;
  bool blacklisted = false;
  if (!GetLearnRecordLocked(key, v.ucode_hash, v.modification, &r,
                            &blacklisted)) {
    return false;
  }
  for (uint32_t k = 0; k < kKindCount; ++k) {
    for (size_t i = 0; i < v.index[k].size() && i < v.miss_bits[k].size();
         ++i) {
      const uint32_t idx = v.index[k][i];
      if (idx < r.stats[k].size()) {
        r.stats[k][idx].varying |= v.miss_bits[k][i];
      }
    }
  }
  r.flags = LearnRecord::kConverged;
  ++r.demotions;
  // Replace (not merge): `r` already contains this library's record.
  auto it = g_records.find(key);
  if (it != g_records.end()) {
    it->second = std::move(r);
  } else {
    AddRecordLocked(r);
  }
  RequestSaveLocked();
  return true;
}

std::vector<PlainKey> SiblingKeys(uint64_t ucode_hash, uint64_t modification) {
  std::lock_guard<std::mutex> lock(g_learn_mutex);
  auto it = g_siblings.find({ucode_hash, modification});
  return it != g_siblings.end() ? it->second : std::vector<PlainKey>();
}

bool HasConvergedRecord(const PlainKey& key) {
  std::lock_guard<std::mutex> lock(g_learn_mutex);
  auto it = g_records.find(key);
  return it != g_records.end() && (it->second.flags & LearnRecord::kConverged);
}

uint32_t Demotions(const PlainKey& key) {
  std::lock_guard<std::mutex> lock(g_learn_mutex);
  auto it = g_records.find(key);
  return it != g_records.end() ? it->second.demotions : 0;
}

void LearnNote(const void* owner, const PlainKey& key, uint64_t ucode_hash,
               uint64_t modification, const DrawPayloads& p) {
  if (!key.valid()) return;
  LiveStats*& live = g_live[owner];
  if (!live) {
    std::lock_guard<std::mutex> lock(g_learn_mutex);
    g_live_storage.push_back(std::make_unique<LiveStats>());
    live = g_live_storage.back().get();
    live->record.key = key;
    live->record.ucode_hash = ucode_hash;
    live->record.modification = modification;
    live->record.flags = LearnRecord::kConverged;
  }
  LearnRecord& r = live->record;
  for (uint32_t k = 0; k < kKindCount; ++k) {
    if (!p.data[k]) continue;
    if (r.stats[k].size() < p.dwords[k]) r.stats[k].resize(p.dwords[k]);
    const uint32_t n = p.dwords[k];
    DwordStat* s = r.stats[k].data();
    const uint32_t* d = p.data[k];
    for (uint32_t i = 0; i < n; ++i) s[i].Note(d[i]);
    ++r.draws[k];
  }
  if ((++g_notes & 4095) == 0) {
    auto now = std::chrono::steady_clock::now();
    if (g_last_save == std::chrono::steady_clock::time_point{}) {
      g_last_save = now;
    } else if (now - g_last_save > std::chrono::seconds(10)) {
      g_last_save = now;
      LearnSave();
    }
  }
}

void LearnSave() {
  if (AutoLearnEnabled()) {
    // Shutdown: write what the background writer has not written yet.
    RecordMap merged;
    std::filesystem::path path;
    {
      std::lock_guard<std::mutex> lock(g_learn_mutex);
      if (g_learn_path.empty() || !g_dirty) return;
      g_dirty = false;
      merged = AutoSnapshotLocked();
      path = g_learn_path;
    }
    WriteFile(path, merged);
    return;
  }
  if (!LearnMode()) return;
  RecordMap merged;
  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(g_learn_mutex);
    if (g_learn_path.empty()) return;
    path = g_learn_path;
    merged = g_records;
    for (const auto& live : g_live_storage) {
      const LearnRecord& r = live->record;
      auto [it, inserted] = merged.emplace(r.key, r);
      if (!inserted) MergeRecord(it->second, r);
    }
  }
  WriteFile(path, merged);
}

// ---------------------------------------------------------------------------
// Auto-learn
// ---------------------------------------------------------------------------

AutoLearn* NewAutoLearn(const PlainKey& key, uint64_t ucode_hash,
                        uint64_t modification) {
  g_auto.push_back(std::make_unique<AutoLearn>());
  AutoLearn* a = g_auto.back().get();
  a->record.key = key;
  a->record.ucode_hash = ucode_hash;
  a->record.modification = modification;
  GetCounters().auto_shaders.fetch_add(1, std::memory_order_relaxed);
  return a;
}

namespace {
// Command-processor thread: copies the records still sampling for the file.
void AutoSnapshot() {
  std::lock_guard<std::mutex> lock(g_learn_mutex);
  g_partial.clear();
  for (const auto& a : g_auto) {
    if (a->state == AutoLearn::kDone || !a->samples) continue;
    LearnRecord r = a->record;
    r.flags = 0;
    for (uint32_t k = 0; k < kKindCount; ++k) {
      for (size_t i = 0;
           i < a->pending_varying[k].size() && i < r.stats[k].size(); ++i) {
        r.stats[k][i].varying |= a->pending_varying[k][i];
      }
    }
    g_partial[r.key] = std::move(r);
  }
  RequestSaveLocked();
}
}  // namespace

bool AutoLearnSample(AutoLearn& a, const DrawPayloads& p) {
  const auto start = std::chrono::steady_clock::now();
  LearnRecord& r = a.record;
  uint32_t changed = 0;
  for (uint32_t k = 0; k < kKindCount; ++k) {
    if (!p.data[k]) continue;
    const uint32_t n = p.dwords[k];
    if (r.stats[k].size() < n) {
      r.stats[k].resize(n);
      changed = 1;
    }
    DwordStat* s = r.stats[k].data();
    const uint32_t* d = p.data[k];
    if (!r.draws[k]) {
      for (uint32_t i = 0; i < n; ++i) s[i].Note(d[i]);
      changed = 1;
    } else {
      for (uint32_t i = 0; i < n; ++i) {
        DwordStat& st = s[i];
        if (!st.seen) {
          // Bound later than the rest of the buffer: never counts as
          // invariant (seen != draws).
          st.first = d[i];
          ++st.seen;
          continue;
        }
        const uint32_t diff = (d[i] ^ st.first) & ~st.varying;
        changed |= diff;
        st.varying |= diff;
        ++st.seen;
      }
    }
    ++r.draws[k];
  }
  ++a.samples;
  if (a.samples == 1) a.first_sample = start;
  // [ready] a shader is still being learned (bg_busy until no such sample for 1 s).
  readiness::NoteUnconvergedSample(
      std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count());
  if (changed) {
    a.last_change = a.samples;
    a.last_change_time = start;
  }
  Counters& c = GetCounters();
  if ((c.auto_samples.fetch_add(1, std::memory_order_relaxed) & 255) == 255) {
    LogCounters(false);
  }
  bool converged = false;
  if (a.samples >= AutoLearnMinSamples() &&
      a.samples - a.last_change >= kAutoQuietSamples &&
      start - a.last_change_time >= kAutoQuietTime &&
      start - a.first_sample >= kAutoMinSpan) {
    converged = true;
    LearnRecord done = r;
    for (uint32_t k = 0; k < kKindCount; ++k) {
      for (size_t i = 0;
           i < a.pending_varying[k].size() && i < done.stats[k].size(); ++i) {
        done.stats[k][i].varying |= a.pending_varying[k][i];
      }
    }
    done.flags = LearnRecord::kConverged;
    {
      std::lock_guard<std::mutex> lock(g_learn_mutex);
      auto it = g_records.find(done.key);
      done.demotions =
          (it != g_records.end() ? it->second.demotions : 0) + (a.demoted ? 1 : 0);
      AddRecordLocked(done);
      g_partial.erase(done.key);
      RequestSaveLocked();
    }
    REXLOG_INFO(
        "[ns-spec] auto-learn: PS {:016X}/{:X} converged after {} samples "
        "({:.1f} s){}",
        r.ucode_hash, r.modification, a.samples,
        std::chrono::duration<double>(start - a.first_sample).count(),
        a.demoted ? ", after a demotion: the next session regenerates"
                  : ", specializing");
    c.auto_converged.fetch_add(1, std::memory_order_relaxed);
    const bool respecialize = !a.demoted;
    a.state = AutoLearn::kDone;
    a.record = LearnRecord();  // free the statistics
    a.record.key = done.key;
    a.record.ucode_hash = done.ucode_hash;
    a.record.modification = done.modification;
    c.auto_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count()),
                        std::memory_order_relaxed);
    return respecialize;
  }
  if (g_last_snapshot == std::chrono::steady_clock::time_point{}) {
    g_last_snapshot = start;
  } else if (start - g_last_snapshot >= kAutoSnapshotInterval) {
    g_last_snapshot = start;
    AutoSnapshot();
  }
  c.auto_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count()),
                      std::memory_order_relaxed);
  return converged;
}

void NoteMiss(const Variant& v, const DrawPayloads& p) {
  for (uint32_t k = 0; k < kKindCount; ++k) {
    const size_t n = v.index[k].size();
    if (!n) continue;
    std::vector<uint32_t>& bits = v.miss_bits[k];
    if (bits.size() < n) bits.resize(n);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t idx = v.index[k][i];
      if (!p.data[k] || idx >= p.dwords[k]) {
        continue;  // not bound: not a constant value difference
      }
      bits[i] |= (p.data[k][idx] ^ v.value[k][i]) & v.mask[k][i];
    }
  }
}

bool CheckDemotion(const Variant& v, bool missed) {
  ++v.window_checks;
  v.window_misses += missed ? 1 : 0;
  if (v.window_checks < kDemoteWindow) return false;
  const uint32_t pct = DemotePercent();
  const bool demote =
      pct && uint64_t(v.window_misses) * 100 > uint64_t(v.window_checks) * pct;
  if (demote) {
    v.demoted.store(true, std::memory_order_relaxed);
    const uint64_t n =
        GetCounters().demotions.fetch_add(1, std::memory_order_relaxed) + 1;
    REXLOG_WARN(
        "[ns-spec] PS {:016X}/{:X}: variant demoted ({} of the last {} guard "
        "checks missed; #{}); the normal pipeline draws for the rest of the "
        "session",
        v.ucode_hash, v.modification, v.window_misses, v.window_checks, n);
  }
  v.window_checks = 0;
  v.window_misses = 0;
  return demote;
}

AutoLearn* AutoLearnAfterDemotion(AutoLearn* existing, const Variant& v,
                                  const PlainKey& key) {
  AutoLearn* a = existing ? existing
                          : NewAutoLearn(key, v.ucode_hash, v.modification);
  a->record = LearnRecord();
  a->record.key = key;
  a->record.ucode_hash = v.ucode_hash;
  a->record.modification = v.modification;
  a->state = AutoLearn::kSamplingAfterDemotion;
  a->samples = 0;
  a->last_change = 0;
  a->counter = 0;
  a->demoted = true;
  for (uint32_t k = 0; k < kKindCount; ++k) {
    a->pending_varying[k].clear();
    for (size_t i = 0; i < v.index[k].size(); ++i) {
      const uint32_t idx = v.index[k][i];
      const uint32_t bits =
          i < v.miss_bits[k].size() ? v.miss_bits[k][i] : 0u;
      if (!bits) continue;
      if (a->pending_varying[k].size() <= idx) {
        a->pending_varying[k].resize(idx + 1);
      }
      a->pending_varying[k][idx] |= bits;
    }
  }
  return a;
}

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------

namespace {

struct PtrInfo {
  enum Base : uint8_t { kNone, kTopLevel, kCb } base = kNone;
  uint32_t kind = 0;
  int64_t offset = 0;
};

struct Def {
  enum Op : uint8_t { kBitcast, kGep, kLoad } op;
  std::string name;
  std::string src;
  std::string type;  // gep element type / load type
  int64_t index = 0;  // gep constant index
  size_t line = 0;
};

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Last %name token in a fragment such as "i8 addrspace(2)* %x".
std::string LastValue(const std::string& s) {
  size_t p = s.rfind('%');
  if (p == std::string::npos) return {};
  size_t e = p + 1;
  while (e < s.size() && (std::isalnum(uint8_t(s[e])) || s[e] == '.' ||
                          s[e] == '_' || s[e] == '-' || s[e] == '$')) {
    ++e;
  }
  return s.substr(p, e - p);
}

// Splits on top-level ", " (not inside parentheses or angle brackets).
std::vector<std::string> SplitOperands(const std::string& s) {
  std::vector<std::string> parts;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '(' || c == '<' || c == '[' || c == '{') ++depth;
    if (c == ')' || c == '>' || c == ']' || c == '}') --depth;
    if (c == ',' && depth == 0) {
      parts.push_back(Trim(s.substr(start, i - start)));
      start = i + 1;
    }
  }
  parts.push_back(Trim(s.substr(start)));
  return parts;
}

int ElementSize(const std::string& t) {
  if (t == "i8") return 1;
  if (t == "i16" || t == "half") return 2;
  if (t == "i32" || t == "float") return 4;
  if (t == "i64" || t == "double") return 8;
  return 0;
}

bool IsPointerType(const std::string& t) {
  return (!t.empty() && t.back() == '*') || t.rfind("ptr", 0) == 0;
}

}  // namespace

bool BakeAir(std::string& ll, const std::vector<GuardEntry>& candidates,
             bool bake_floats, std::vector<GuardEntry>* baked,
             std::string* error) {
  baked->clear();
  std::map<std::pair<uint32_t, uint32_t>, GuardEntry> want;
  for (const GuardEntry& e : candidates) want[{e.kind, e.index}] = e;

  std::vector<std::string> lines;
  {
    size_t start = 0;
    while (start <= ll.size()) {
      size_t end = ll.find('\n', start);
      if (end == std::string::npos) end = ll.size();
      lines.push_back(ll.substr(start, end - start));
      start = end + 1;
    }
  }

  std::unordered_map<std::string, PtrInfo> info;
  std::vector<Def> defs;
  const std::string tlab = "%struct.top_level_global_ab addrspace(2)*";
  int functions = 0;
  for (size_t li = 0; li < lines.size(); ++li) {
    const std::string& line = lines[li];
    if (line.rfind("define ", 0) == 0) {
      ++functions;
      size_t p = line.find(tlab);
      if (p != std::string::npos) {
        // Skip parameter attributes up to the value name.
        size_t q = line.find('%', p + tlab.size());
        if (q != std::string::npos) {
          std::string name = LastValue(line.substr(0, line.find_first_of(",)", q)));
          if (!name.empty()) info[name] = {PtrInfo::kTopLevel, 0, 0};
        }
      }
      continue;
    }
    size_t eq = line.find(" = ");
    if (eq == std::string::npos) continue;
    std::string lhs = Trim(line.substr(0, eq));
    if (lhs.empty() || lhs[0] != '%') continue;
    std::string rhs = line.substr(eq + 3);
    if (rhs.rfind("bitcast ", 0) == 0) {
      size_t to = rhs.rfind(" to ");
      if (to == std::string::npos) continue;
      std::string src = LastValue(rhs.substr(0, to));
      if (src.empty()) continue;
      defs.push_back({Def::kBitcast, lhs, src, "", 0, li});
    } else if (rhs.rfind("getelementptr ", 0) == 0) {
      std::string body = rhs.substr(14);
      if (body.rfind("inbounds ", 0) == 0) body = body.substr(9);
      std::vector<std::string> ops = SplitOperands(body);
      if (ops.size() != 3) continue;  // element type, base, one index
      if (ops[2].rfind("i64 ", 0) != 0) continue;
      char* end = nullptr;
      std::string idx = ops[2].substr(4);
      long long c = std::strtoll(idx.c_str(), &end, 10);
      if (end == idx.c_str() || *end) continue;
      std::string src = LastValue(ops[1]);
      if (src.empty()) continue;
      defs.push_back({Def::kGep, lhs, src, ops[0], c, li});
    } else if (rhs.rfind("load ", 0) == 0) {
      std::vector<std::string> ops = SplitOperands(rhs.substr(5));
      if (ops.size() < 2) continue;
      std::string src = LastValue(ops[1]);
      if (src.empty() || ops[0].rfind("volatile", 0) == 0) continue;
      defs.push_back({Def::kLoad, lhs, src, ops[0], 0, li});
    }
  }
  if (functions != 1) {
    if (error) *error = fmt::format("{} functions", functions);
    return false;
  }
  if (info.empty()) {
    if (error) *error = "no top-level argument buffer parameter";
    return false;
  }

  // Resolve pointer chains to a fixed point (SSA: each name is defined once;
  // textual order need not follow dominance).
  bool changed = true;
  while (changed) {
    changed = false;
    for (const Def& d : defs) {
      if (info.count(d.name)) continue;
      auto it = info.find(d.src);
      if (it == info.end()) continue;
      const PtrInfo src = it->second;
      PtrInfo out;
      switch (d.op) {
        case Def::kBitcast:
          out = src;
          break;
        case Def::kGep: {
          int size = ElementSize(d.type);
          if (!size) continue;
          out = src;
          out.offset += d.index * size;
          break;
        }
        case Def::kLoad:
          if (src.base != PtrInfo::kTopLevel || !IsPointerType(d.type)) continue;
          if (src.offset == kTopLevelOffsetSystem) {
            out = {PtrInfo::kCb, kSystem, 0};
          } else if (src.offset == kTopLevelOffsetBoolLoop) {
            out = {PtrInfo::kCb, kBoolLoop, 0};
          } else if (src.offset == kTopLevelOffsetPixelFetch) {
            out = {PtrInfo::kCb, kFetch, 0};
          } else {
            continue;
          }
          break;
      }
      info[d.name] = out;
      changed = true;
    }
  }

  // Uses of every value: line indices (built lazily for the loads baked).
  auto is_ident = [](char c) {
    return std::isalnum(uint8_t(c)) || c == '.' || c == '_' || c == '-' ||
           c == '$';
  };
  auto uses_of = [&](const std::string& name, size_t def_line) {
    std::vector<size_t> out;
    for (size_t li = 0; li < lines.size(); ++li) {
      if (li == def_line) continue;
      const std::string& l = lines[li];
      size_t p = 0;
      while ((p = l.find(name, p)) != std::string::npos) {
        size_t e = p + name.size();
        if (e >= l.size() || !is_ident(l[e])) {
          out.push_back(li);
          break;
        }
        p = e;
      }
    }
    return out;
  };
  // A use the bake understands: "%r = [tail ]call i32
  // @air.extract_bits.{u,s}.i32(i32 %x, i32 A, i32 B)" or "%r = and i32 %x, C"
  // (either operand order). Returns false for anything else.
  struct Use {
    std::string result;
    std::string type = "i32";  // result type
    bool extract = false;
    bool sign = false;
    uint32_t offset = 0, width = 0, and_mask = 0;
    // lshr / ashr by a constant: offset = shift, width = 32 - shift.
    bool shift = false;
    uint32_t Bits() const {
      if (!extract && !shift) return and_mask;
      return width >= 32 ? 0xFFFFFFFFu : (((1u << width) - 1) << offset);
    }
    int64_t Apply(uint32_t v) const {
      if (shift) {
        return sign ? int64_t(int32_t(v) >> offset) : int64_t(v >> offset);
      }
      if (!extract) return int32_t(v & and_mask);
      if (width >= 32) return int32_t(v);
      uint32_t f = (v >> offset) & ((1u << width) - 1);
      if (sign && (f >> (width - 1)) & 1) f |= ~((1u << width) - 1);
      return int32_t(f);
    }
  };
  auto parse_use = [&](const std::string& line, const std::string& name,
                       Use* use) -> bool {
    size_t eq = line.find(" = ");
    if (eq == std::string::npos) return false;
    use->result = Trim(line.substr(0, eq));
    std::string rhs = line.substr(eq + 3);
    const bool lshr = rhs.rfind("lshr i32 ", 0) == 0;
    const bool ashr = rhs.rfind("ashr i32 ", 0) == 0;
    if (lshr || ashr) {
      // "%r = lshr i32 %x, C" (also "exact"); only %x as the shifted value.
      std::string body = rhs.substr(9);
      if (body.rfind("exact ", 0) == 0) body = body.substr(6);
      std::vector<std::string> ops = SplitOperands(body);
      if (ops.size() != 2 || ops[0] != name) return false;
      char* end = nullptr;
      long c = std::strtol(ops[1].c_str(), &end, 10);
      if (end == ops[1].c_str() || *end || c < 0 || c > 31) return false;
      use->shift = true;
      use->sign = ashr;
      use->offset = uint32_t(c);
      use->width = 32 - uint32_t(c);
      return true;
    }
    if (rhs.rfind("trunc i32 ", 0) == 0) {
      // "%r = trunc i32 %x to i8|i16"
      std::string body = rhs.substr(10);
      size_t to = body.find(" to ");
      if (to == std::string::npos || Trim(body.substr(0, to)) != name) {
        return false;
      }
      std::string t = Trim(body.substr(to + 4));
      uint32_t bits = t == "i8" ? 8 : t == "i16" ? 16 : t == "i1" ? 1 : 0;
      if (!bits) return false;
      use->extract = true;
      use->sign = false;
      use->offset = 0;
      use->width = bits;
      use->type = t;
      return true;
    }
    if (rhs.rfind("tail call ", 0) == 0) rhs = rhs.substr(10);
    else if (rhs.rfind("call ", 0) == 0) rhs = rhs.substr(5);
    else if (rhs.rfind("and i32 ", 0) == 0) {
      std::vector<std::string> ops = SplitOperands(rhs.substr(8));
      if (ops.size() != 2) return false;
      const std::string& other = ops[0] == name ? ops[1] : ops[0];
      if ((ops[0] != name && ops[1] != name) || other == name) return false;
      char* end = nullptr;
      long long c = std::strtoll(other.c_str(), &end, 10);
      if (end == other.c_str() || *end) return false;
      use->extract = false;
      use->and_mask = uint32_t(c);
      return true;
    } else {
      return false;
    }
    bool sign;
    if (rhs.rfind("i32 @air.extract_bits.u.i32(", 0) == 0) sign = false;
    else if (rhs.rfind("i32 @air.extract_bits.s.i32(", 0) == 0) sign = true;
    else return false;
    size_t open = rhs.find('(');
    size_t close = rhs.find(')', open);
    if (close == std::string::npos) return false;
    std::vector<std::string> ops =
        SplitOperands(rhs.substr(open + 1, close - open - 1));
    if (ops.size() != 3 || ops[0] != "i32 " + name ||
        ops[1].rfind("i32 ", 0) != 0 || ops[2].rfind("i32 ", 0) != 0) {
      return false;
    }
    char* end = nullptr;
    long a = std::strtol(ops[1].c_str() + 4, &end, 10);
    if (*end) return false;
    long b = std::strtol(ops[2].c_str() + 4, &end, 10);
    if (*end) return false;
    if (a < 0 || b < 1 || a + b > 32) return false;
    use->extract = true;
    use->sign = sign;
    use->offset = uint32_t(a);
    use->width = uint32_t(b);
    return true;
  };

  std::map<std::pair<uint32_t, uint32_t>, uint32_t> done_mask, done_value;
  uint32_t loads_seen = 0;
  for (const Def& d : defs) {
    if (d.op != Def::kLoad) continue;
    auto it = info.find(d.src);
    if (it == info.end() || it->second.base != PtrInfo::kCb) continue;
    const bool is_i32 = d.type == "i32";
    const bool is_float = d.type == "float";
    if (!is_i32 && !(is_float && bake_floats)) continue;
    const int64_t off = it->second.offset;
    if (off < 0 || (off & 3)) continue;
    ++loads_seen;
    const std::pair<uint32_t, uint32_t> k{it->second.kind, uint32_t(off / 4)};
    auto w = want.find(k);
    if (w == want.end()) continue;
    const uint32_t inv = w->second.mask;
    const uint32_t val = w->second.value;
    uint32_t used = 0;
    bool replace_load = false;
    if (is_i32) {
      // Fold the bit-field uses whose bits are all invariant.
      std::vector<size_t> uses = uses_of(d.name, d.line);
      bool all_folded = true;
      for (size_t li : uses) {
        Use u;
        if (!parse_use(lines[li], d.name, &u) || (u.Bits() & ~inv)) {
          all_folded = false;
          continue;
        }
        const size_t indent = lines[li].find_first_not_of(" \t");
        const std::string pre =
            lines[li].substr(0, indent == std::string::npos ? 0 : indent);
        if (u.type == "i32") {
          lines[li] = fmt::format("{}{} = or i32 {}, 0", pre, u.result,
                                  int32_t(u.Apply(val)));
        } else {
          // trunc: the low bits as a constant of the narrow type.
          const uint64_t m = (uint64_t(1) << u.width) - 1;
          lines[li] = fmt::format("{}{} = or {} {}, 0", pre, u.result, u.type,
                                  uint64_t(u.Apply(val)) & m);
        }
        used |= u.Bits();
      }
      // Any other use needs the whole dword: bake the load itself when all
      // of its bits are invariant.
      if (!all_folded && inv == 0xFFFFFFFFu) {
        replace_load = true;
        used = 0xFFFFFFFFu;
      }
    } else if (inv == 0xFFFFFFFFu) {
      replace_load = true;
      used = 0xFFFFFFFFu;
    }
    if (replace_load) {
      std::string& line = lines[d.line];
      const size_t indent = line.find_first_not_of(" \t");
      std::string prefix =
          line.substr(0, indent == std::string::npos ? 0 : indent);
      if (is_i32) {
        line = fmt::format("{}{} = or i32 {}, 0", prefix, d.name, int32_t(val));
      } else {
        line = fmt::format("{}{} = bitcast i32 {} to float", prefix, d.name,
                           int32_t(val));
      }
    }
    if (used) {
      done_mask[k] |= used;
      done_value[k] = val;
    }
  }
  if (done_mask.empty()) {
    if (error) {
      *error = fmt::format("nothing to bake ({} constant loads)", loads_seen);
    }
    return false;
  }
  for (auto& [k, m] : done_mask) {
    baked->push_back({k.first, k.second, done_value[k] & m, m});
  }
  std::string out;
  out.reserve(ll.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size()) out += '\n';
  }
  ll.swap(out);
  return true;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

// Index record: magic "NS2K", count, learn hash, values hash, then per entry
// kind, index, value, mask (u32 each).
constexpr uint32_t kIndexMagic = 0x4B32534E;

std::vector<uint8_t> SerializeIndex(uint64_t learn_hash,
                                    const std::vector<GuardEntry>& entries) {
  std::vector<uint8_t> out(24 + entries.size() * 16);
  const uint32_t count = uint32_t(entries.size());
  std::memcpy(&out[0], &kIndexMagic, 4);
  std::memcpy(&out[4], &count, 4);
  std::memcpy(&out[8], &learn_hash, 8);
  const uint64_t vh = HashEntries(entries);
  std::memcpy(&out[16], &vh, 8);
  for (size_t i = 0; i < entries.size(); ++i) {
    const uint32_t words[4] = {entries[i].kind, entries[i].index,
                               entries[i].value & entries[i].mask,
                               entries[i].mask};
    std::memcpy(&out[24 + i * 16], words, 16);
  }
  return out;
}

bool ParseIndex(const std::vector<uint8_t>& bytes, uint64_t* learn_hash,
                std::vector<GuardEntry>* entries) {
  if (bytes.size() < 24) return false;
  uint32_t magic, count;
  std::memcpy(&magic, &bytes[0], 4);
  std::memcpy(&count, &bytes[4], 4);
  if (magic != kIndexMagic || bytes.size() != 24 + size_t(count) * 16) {
    return false;
  }
  std::memcpy(learn_hash, &bytes[8], 8);
  uint64_t vh;
  std::memcpy(&vh, &bytes[16], 8);
  entries->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t words[4];
    std::memcpy(words, &bytes[24 + i * 16], 16);
    GuardEntry& e = (*entries)[i];
    e = {words[0], words[1], words[2], words[3]};
    if (e.kind >= kKindCount || !e.mask) return false;
  }
  return count == 0 || HashEntries(*entries) == vh;
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

namespace {
Counters g_counters;
std::mutex g_variants_mutex;
std::vector<const Variant*> g_variants;
std::chrono::steady_clock::time_point g_last_log{};
}  // namespace

Counters& GetCounters() { return g_counters; }

void RegisterVariant(const Variant* v) {
  std::lock_guard<std::mutex> lock(g_variants_mutex);
  g_variants.push_back(v);
}

void LogCounters(bool force) {
  auto now = std::chrono::steady_clock::now();
  if (!force && g_last_log != std::chrono::steady_clock::time_point{} &&
      now - g_last_log < std::chrono::seconds(10)) {
    return;
  }
  if (g_last_log == std::chrono::steady_clock::time_point{} && !force) {
    g_last_log = now;
    return;
  }
  g_last_log = now;
  const Counters& c = g_counters;
  std::string per;
  {
    std::lock_guard<std::mutex> lock(g_variants_mutex);
    for (const Variant* v : g_variants) {
      const uint64_t h = v->hits.load(std::memory_order_relaxed);
      const uint64_t m = v->misses.load(std::memory_order_relaxed);
      per += fmt::format(" {:08X}/{:X}:{}/{}", uint32_t(v->ucode_hash >> 32),
                         v->modification, h, h + m);
      if (v->demoted.load(std::memory_order_relaxed)) per += "(demoted)";
    }
  }
  REXLOG_INFO(
      "[ns-spec] guard: checks {}, hits {}, misses {}, hit without pipeline "
      "yet {}, verified {}, misbinds {}, demoted {}; variants "
      "(hits/checks):{}",
      c.checks.load(), c.hits.load(), c.misses.load(),
      c.hit_no_pipeline.load(), c.verified.load(), c.misbinds.load(),
      c.demotions.load(), per);
  if (AutoLearnEnabled()) {
    REXLOG_INFO(
        "[ns-spec] auto-learn: {} shaders sampled, {} converged, {} samples, "
        "{:.2f} ms noting them",
        c.auto_shaders.load(), c.auto_converged.load(), c.auto_samples.load(),
        double(c.auto_ns.load()) / 1e6);
  }
}

}  // namespace ns_spec
}  // namespace metal
}  // namespace graphics
}  // namespace rex
