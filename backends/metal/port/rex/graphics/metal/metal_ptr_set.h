// [draw-prep-lite] FlatPtrSet: an insert/count/clear-only pointer set that keeps
// its storage across clear() (the per-pass and per-command-buffer sampled
// texture sets are refilled thousands of times per frame; std::unordered_set
// allocated and freed a node per insert). Open addressing, linear probing,
// power-of-two capacity, load factor <= 1/2. No erase, no iteration.
// With FABLE2_DRAW_PREP_LITE=0 it forwards to std::unordered_set as before.
#ifndef REX_GRAPHICS_METAL_METAL_PTR_SET_H_
#define REX_GRAPHICS_METAL_METAL_PTR_SET_H_

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rex::graphics::metal {

bool FlatPtrSetEnabled();  // FABLE2_DRAW_PREP_LITE, read once
bool FlatPtrSetLite2Enabled();  // FABLE2_DRAW_PREP_LITE2, read once

template <typename T>
class FlatPtrSet {
 public:
  FlatPtrSet() : flat_(FlatPtrSetEnabled()) {}
  // [draw-prep-lite2] a set behind a different switch.
  explicit FlatPtrSet(bool flat) : flat_(flat) {}
  // Returns {unused, inserted} like std::unordered_set::insert (.second only).
  std::pair<bool, bool> insert(T p) {
    if (!flat_) return {true, legacy_.insert(p).second};
    if (!p) {  // null is the empty marker; track it separately
      bool inserted = !has_null_;
      has_null_ = true;
      if (inserted) ++size_;
      return {true, inserted};
    }
    if ((size_ + 1) * 2 > slots_.size()) Grow();
    size_t mask = slots_.size() - 1;
    for (size_t i = Hash(p) & mask;; i = (i + 1) & mask) {
      if (slots_[i] == p) return {true, false};
      if (!slots_[i]) {
        slots_[i] = p;
        ++size_;
        return {true, true};
      }
    }
  }
  size_t count(T p) const {
    if (!flat_) return legacy_.count(p);
    if (!p) return has_null_ ? 1 : 0;
    if (slots_.empty()) return 0;
    size_t mask = slots_.size() - 1;
    for (size_t i = Hash(p) & mask;; i = (i + 1) & mask) {
      if (slots_[i] == p) return 1;
      if (!slots_[i]) return 0;
    }
  }
  bool empty() const { return flat_ ? size_ == 0 : legacy_.empty(); }
  size_t size() const { return flat_ ? size_ : legacy_.size(); }
  void clear() {
    if (!flat_) {
      legacy_.clear();
      return;
    }
    if (size_) std::fill(slots_.begin(), slots_.end(), T(nullptr));
    size_ = 0;
    has_null_ = false;
  }

 private:
  static size_t Hash(T p) {
    uint64_t v = uint64_t(reinterpret_cast<uintptr_t>(p));
    v ^= v >> 29;
    v *= UINT64_C(0xBF58476D1CE4E5B9);
    return size_t(v ^ (v >> 32));
  }
  void Grow() {
    std::vector<T> old = std::move(slots_);
    slots_.assign(old.empty() ? 64 : old.size() * 2, T(nullptr));
    size_t mask = slots_.size() - 1;
    for (T p : old) {
      if (!p) continue;
      size_t i = Hash(p) & mask;
      while (slots_[i]) i = (i + 1) & mask;
      slots_[i] = p;
    }
  }
  bool flat_;
  std::vector<T> slots_;
  size_t size_ = 0;
  bool has_null_ = false;
  std::unordered_set<T> legacy_;
};

}  // namespace rex::graphics::metal

#endif  // REX_GRAPHICS_METAL_METAL_PTR_SET_H_
