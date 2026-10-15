// Hot-shader specialization (FABLE2_NS_SPECIALIZE, docs/native-shaders.md,
// "Hot-shader specialization").
//
// A guest pixel shader reads the system, bool/loop and fetch constants at run
// time and decodes them per pixel (texture formats, signs, exp-adjust, branch
// flags). For the hot shaders most of those dwords never change. This module:
//
// - learns, per pixel-shader translation, which of the constant dwords bound
//   for the pixel stage are invariant (FABLE2_NS_SPECIALIZE_LEARN=1, written to
//   a learn file in the shader cache; nothing game-derived goes to the repo);
// - bakes those dwords into the shader's AIR as literals (only loads at
//   constant offsets of the system, bool/loop and pixel fetch buffers, found by
//   their top-level argument-buffer slot), then runs `metal-opt -O2` again;
// - keeps the list of dwords actually baked as the guard. A draw binds the
//   specialized pipeline only when every guarded dword of the bound constant
//   bytes equals the baked value; otherwise the normal pipeline. So a
//   specialized pipeline only ever runs with the inputs it was built for.

#ifndef REX_GRAPHICS_METAL_METAL_NS_SPECIALIZE_H_
#define REX_GRAPHICS_METAL_METAL_NS_SPECIALIZE_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rex/ui/metal/metal_api.h"

namespace rex {
namespace graphics {
namespace metal {
namespace ns_spec {

// Constant buffers a pixel shader can read that are specialized.
enum Kind : uint32_t { kSystem = 0, kBoolLoop = 1, kFetch = 2, kKindCount = 3 };

// Byte offsets of those buffers' pointers in the top-level argument buffer
// (GraphicsRootABSlot x 8: CBVSystem 10, CBVBoolLoop 12, CBVPixelFetch 22).
constexpr uint32_t kTopLevelOffsetSystem = 10 * 8;
constexpr uint32_t kTopLevelOffsetBoolLoop = 12 * 8;
constexpr uint32_t kTopLevelOffsetPixelFetch = 22 * 8;

// --- Switches (env only; read once) ---------------------------------------
// FABLE2_NS_SPECIALIZE: default on; =0 off. Needs FABLE2_NATIVE_SHADERS.
bool Enabled();
// FABLE2_NS_SPECIALIZE_LEARN=1: record constant statistics (learn file).
bool LearnMode();
// FABLE2_NS_SPECIALIZE_VERIFY=1: on every guard hit, re-check the guarded
// dwords against the guest registers / system constants (misbind counter).
bool VerifyMode();
// FABLE2_NS_SPECIALIZE_FLOATS (default 1): bake float-typed loads too.
bool BakeFloats();
// FABLE2_NS_SPECIALIZE_MIN_DRAWS (default 16).
uint32_t MinDraws();
// FABLE2_NS_SPECIALIZE_AUTOLEARN (default on; =0 off): low-rate learning
// during normal play. Off in a learn session (FABLE2_NS_SPECIALIZE_LEARN=1),
// which records everything.
bool AutoLearnEnabled();
// FABLE2_NS_SPECIALIZE_DEMOTE_PCT (default 5; 0 = never): a variant whose
// guard misses more than this share of a window's checks is demoted.
uint32_t DemotePercent();
// FABLE2_NS_SPECIALIZE_LIST=<hash>[,...] | all: which pixel shaders
// (ucode hashes) are specialized. Unset = the built-in list of hot shaders.
// FABLE2_NS_SPECIALIZE_SKIP=<hash>[,...]: never (per-shader A/B); unset = the
// built-in list of shaders measured not faster.
bool Candidate(uint64_t ucode_hash);

struct PlainKey {
  uint64_t low = 0;
  uint64_t high = 0;
  uint64_t size = 0;
  bool valid() const { return size != 0; }
  bool operator==(const PlainKey& o) const {
    return low == o.low && high == o.high && size == o.size;
  }
};
PlainKey ComputePlainKey(const std::vector<uint8_t>& metallib);

struct GuardEntry {
  uint32_t kind;
  uint32_t index;  // dword index in the buffer
  uint32_t value;  // already masked
  uint32_t mask = 0xFFFFFFFFu;  // the bits that were baked / are compared
};

// The constant bytes bound for the pixel stage of one draw (null when that
// buffer is not active for the pixel stage).
struct DrawPayloads {
  const uint32_t* data[kKindCount] = {};
  uint32_t dwords[kKindCount] = {};
};

// A specialized variant of one pixel-shader translation.
struct Variant {
  ~Variant();
  // Guarded dwords, per kind, in index order.
  std::vector<uint32_t> index[kKindCount];
  std::vector<uint32_t> value[kKindCount];
  std::vector<uint32_t> mask[kKindCount];
  uint64_t values_hash = 0;
  uint32_t guard_count = 0;
  MTL::Library* library = nullptr;
  MTL::Function* function = nullptr;
  uint64_t ucode_hash = 0;
  uint64_t modification = 0;
  // FABLE2_NATIVE_XLATE: library/function come from the native MSL
  // translation with the guarded bits baked (else the converter's AIR).
  bool native = false;
  // Draw counters (command-processor thread; relaxed).
  mutable std::atomic<uint64_t> hits{0};
  mutable std::atomic<uint64_t> misses{0};
  // Miss tracking (command-processor thread only): the current window, the
  // guarded bits that differed on misses (per kind, parallel to `mask`), and
  // whether the variant was demoted (then it is never bound again this
  // session).
  mutable uint32_t window_checks = 0;
  mutable uint32_t window_misses = 0;
  mutable std::atomic<bool> demoted{false};
  mutable std::vector<uint32_t> miss_bits[kKindCount];

  bool Matches(const DrawPayloads& p) const {
    for (uint32_t k = 0; k < kKindCount; ++k) {
      const size_t n = index[k].size();
      if (!n) continue;
      const uint32_t* d = p.data[k];
      if (!d) return false;
      const uint32_t* idx = index[k].data();
      const uint32_t* val = value[k].data();
      const uint32_t* msk = mask[k].data();
      const uint32_t limit = p.dwords[k];
      for (size_t i = 0; i < n; ++i) {
        if (idx[i] >= limit || (d[idx[i]] & msk[i]) != val[i]) return false;
      }
    }
    return true;
  }
};

std::vector<GuardEntry> VariantEntries(const Variant& v);
void SetVariantEntries(Variant& v, std::vector<GuardEntry> entries);
uint64_t HashEntries(const std::vector<GuardEntry>& entries);

// --- Learn data ------------------------------------------------------------
// Per dword: the first value seen, the bits that ever differed from it, and
// how many draws saw it.
struct DwordStat {
  uint32_t first = 0;
  uint32_t varying = 0;
  uint32_t seen = 0;
  void Note(uint32_t x) {
    if (!seen++) {
      first = x;
    } else {
      varying |= x ^ first;
    }
  }
};

struct LearnRecord {
  // flags: the statistics converged (a learn session, or auto-learn's
  // confidence rule); only converged records make variants.
  static constexpr uint32_t kConverged = 1;
  PlainKey key;
  uint64_t ucode_hash = 0;
  uint64_t modification = 0;
  uint32_t draws[kKindCount] = {};
  uint32_t flags = 0;
  // How often a variant made from this shader's data was demoted (guard
  // misses). At kMaxDemotions the shader is no longer specialized.
  uint32_t demotions = 0;
  static constexpr uint32_t kMaxDemotions = 3;
  std::vector<DwordStat> stats[kKindCount];
  // The dword bits that never changed over the learned draws (per dword: the
  // first value, masked to the invariant bits).
  std::vector<GuardEntry> Candidates() const;
};

// Called at shader-storage init with the title's cache folder: loads the learn
// file (if any) and, in learn mode, remembers where to save it.
void InitializeStorage(const std::filesystem::path& title_root,
                       uint32_t title_id);
// The learn data a variant of this plain library is made from, if any is
// ready: the library's own converged record merged with the converged records
// of the same shader (ucode + modification) at other resolution scales or
// converter targets. Constants do not depend on those, except a few
// scale-dependent system constants, which are taken from the library's own
// record only (never baked when it has none). Returns false when nothing
// converged; `*blacklisted` when the shader was demoted too often.
// Thread-safe.
bool GetLearnRecord(const PlainKey& key, uint64_t ucode_hash,
                    uint64_t modification, LearnRecord* out,
                    bool* blacklisted);
// Demotions recorded for this plain library (0 if none). Thread-safe.
uint32_t Demotions(const PlainKey& key);
// Whether this exact key has converged learn data of its own (not only
// siblings with the same ucode hash and modification).
bool HasConvergedRecord(const PlainKey& key);
// The keys of every learn record of this shader (ucode hash + modification),
// e.g. converter-library keys at other scales.
std::vector<PlainKey> SiblingKeys(uint64_t ucode_hash, uint64_t modification);
// Hash of a record's candidate set plus the settings that shape it, so a
// changed learn file or threshold regenerates the variant.
uint64_t LearnHash(const LearnRecord& record);

// Learn mode, command-processor thread: note one draw of a translation.
// `owner` identifies the translation (its address).
void LearnNote(const void* owner, const PlainKey& key, uint64_t ucode_hash,
               uint64_t modification, const DrawPayloads& p);
// Saves the learn file (merged with what was loaded). Called periodically by
// LearnNote and at shutdown (auto-learn: final synchronous save).
void LearnSave();

// --- Auto-learn (FABLE2_NS_SPECIALIZE_AUTOLEARN) ------------------------------
// Per pixel-shader translation without a variant whose shader is a candidate:
// every Nth draw (FABLE2_NS_SPECIALIZE_AUTOLEARN_SAMPLE, default 4) notes the
// bound constants until the statistics converge: at least
// FABLE2_NS_SPECIALIZE_AUTOLEARN_MIN_SAMPLES (default 64) samples over at
// least 5 s, the last 48 of them and the last 3 s without a new varying bit. The record then
// goes into the learn data and the translation is specialized in the
// background. Once a variant exists its guard is the learning signal: misses
// collect the differing bits, and a demoted variant feeds them back (the next
// session regenerates). Owned by this module; command-processor thread only.
struct AutoLearn {
  enum State : uint8_t { kSampling, kDone, kSamplingAfterDemotion };
  LearnRecord record;
  State state = kSampling;
  uint32_t counter = 0;
  uint32_t samples = 0;
  uint32_t last_change = 0;
  std::chrono::steady_clock::time_point first_sample{};
  std::chrono::steady_clock::time_point last_change_time{};
  // Demotion: guarded bits that missed, applied as varying at convergence.
  std::vector<uint32_t> pending_varying[kKindCount];
  bool demoted = false;
};
AutoLearn* NewAutoLearn(const PlainKey& key, uint64_t ucode_hash,
                        uint64_t modification);
// Cheap per-draw check: true when this draw is to be sampled.
inline bool AutoLearnWantsSample(AutoLearn& a, uint32_t every) {
  if (a.state == AutoLearn::kDone) return false;
  return (a.counter++ % every) == 0;
}
uint32_t AutoLearnSampleEvery();
// Notes one sampled draw. Returns true when the statistics just converged
// and the translation should be (re)specialized now (not after a demotion).
bool AutoLearnSample(AutoLearn& a, const DrawPayloads& p);
// A variant missed: accumulate the guarded bits that differ.
void NoteMiss(const Variant& v, const DrawPayloads& p);
// Called after each check of a variant (command-processor thread). Returns
// true when the variant was demoted by this check.
bool CheckDemotion(const Variant& v, bool missed);
// A variant was demoted: its translation's learn data gets the guarded bits
// that missed as varying (and a demotion), so the next session regenerates
// it without them. False when there is no learn data to update (a seeded
// variant): then learn again with AutoLearnAfterDemotion.
bool RecordDemotion(const PlainKey& key, const Variant& v);
// Starts learning again for a demoted variant's translation (the miss bits
// are applied at convergence; the learn file then makes the next session
// regenerate). `existing` may be null.
AutoLearn* AutoLearnAfterDemotion(AutoLearn* existing, const Variant& v,
                                  const PlainKey& key);

// --- Bake --------------------------------------------------------------------
// Replaces, in disassembled AIR text, what the shader computes from candidate
// dwords with literals: bit-field extracts and masks of invariant bits become
// constants; a load used otherwise is replaced whole when all its bits are
// invariant. `baked` receives the dwords and bits actually used (the guard).
// Returns false (text untouched) when nothing could be baked.
bool BakeAir(std::string& ll, const std::vector<GuardEntry>& candidates,
             bool bake_floats, std::vector<GuardEntry>* baked,
             std::string* error);

// --- Record payloads (artifact store) ------------------------------------------
// Index record: which dwords the variant guards and what learn data made it.
std::vector<uint8_t> SerializeIndex(uint64_t learn_hash,
                                    const std::vector<GuardEntry>& entries);
bool ParseIndex(const std::vector<uint8_t>& bytes, uint64_t* learn_hash,
                std::vector<GuardEntry>* entries);

// --- Draw counters -------------------------------------------------------------
struct Counters {
  std::atomic<uint64_t> checks{0};
  std::atomic<uint64_t> hits{0};
  std::atomic<uint64_t> misses{0};
  std::atomic<uint64_t> hit_no_pipeline{0};
  std::atomic<uint64_t> verified{0};
  std::atomic<uint64_t> misbinds{0};
  std::atomic<uint64_t> demotions{0};
  // Auto-learn: sampled draws, nanoseconds spent noting them, converged
  // shaders.
  std::atomic<uint64_t> auto_samples{0};
  std::atomic<uint64_t> auto_ns{0};
  std::atomic<uint64_t> auto_converged{0};
  std::atomic<uint64_t> auto_shaders{0};
};
Counters& GetCounters();
void RegisterVariant(const Variant* v);
// Logs the counters and per-variant hit rates (rate-limited unless forced).
void LogCounters(bool force);

}  // namespace ns_spec
}  // namespace metal
}  // namespace graphics
}  // namespace rex

#endif  // REX_GRAPHICS_METAL_METAL_NS_SPECIALIZE_H_
