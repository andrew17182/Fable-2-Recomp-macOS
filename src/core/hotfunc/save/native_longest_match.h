// native_longest_match.h - host-native equivalent of the guest's zlib longest_match (sub_82B52630).
//
// The save compressor is the game's own zlib 1.2.3 deflate (deflate_state layout below, strategy 2/3 dispatch
// in the callers sub_82B52D40 / sub_82B531B8). About 80% of an autosave's GameThread CPU is this one function
// (docs/area-load-profile-20261015.md, cost #2). The recompiled copy loads one guest byte per volatile access;
// this version computes the same answer with 8-byte compares. docs/load-fixes-20261016.md has the proof.
//
// Exact semantics of the guest code (fable_2_recomp.121.cpp, DEFINE_REX_FUNC(sub_82B52630)), all 32-bit:
//   s = r3 (deflate_state*), cur = r4 (cur_match)
//   chain  = max_chain_length (+124); if prev_length (+120) >= good_match (+140) (unsigned): chain >>= 2
//   nice   = nice_match (+144); if nice > lookahead (+116) (unsigned): nice = lookahead
//   limit  = strstart (+108) > w_size (+44) - 262 (unsigned) ? strstart - w_size + 262 : 0
//   scan   = window (+56) + strstart; best = prev_length; end1 = scan[best-1]; end = scan[best]
//   loop:
//     m = window + cur
//     if m[best] == end && m[best-1] == end1 && m[0] == scan[0] && m[1] == scan[1]:
//       len = first index i in [3, 258] with m[i] != scan[i], else 258   (index 2 is never compared)
//       if (int)len > (int)best: match_start (+112) = cur; best = len
//                                 if (int)len >= (int)nice: stop; end1 = scan[best-1]; end = scan[best]
//     cur = prev (+64) [cur & w_mask (+52)] (u16); stop unless cur > limit (unsigned) and --chain != 0
//   return best <= lookahead (unsigned) ? best : lookahead
// The guest reads exactly scan[0..258] and m[0..258] per full compare; so does this code (32 x 8 bytes from 3).
// The only guest-memory write is match_start, same value, same condition (the stack save area is written by the
// caller of NativeLongestMatch, see the override).
#pragma once

#include <cstdint>
#include <cstring>

namespace fable2::savecompress {

// Same as REX_PHYS_HOST_OFFSET in generated/default/fable_2_pch.h on macOS arm64 (checked by the test).
inline uint32_t PhysHostOffset(uint32_t addr) { return addr >= 0xE0000000u ? 0x1000u : 0u; }

inline const uint8_t* GuestPtr(const uint8_t* base, uint32_t addr) { return base + addr + PhysHostOffset(addr); }

inline uint32_t LoadBE32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, addr), 4);
  return __builtin_bswap32(v);
}
inline uint16_t LoadBE16(const uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, GuestPtr(base, addr), 2);
  return __builtin_bswap16(v);
}
inline void StoreBE32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(const_cast<uint8_t*>(GuestPtr(base, addr)), &v, 4);
}

// deflate_state field offsets (zlib 1.2.3, 32-bit big-endian guest).
enum : uint32_t {
  kWSize = 44, kWMask = 52, kWindow = 56, kPrev = 64, kStrStart = 108, kMatchStart = 112, kLookahead = 116,
  kPrevLength = 120, kMaxChain = 124, kGoodMatch = 140, kNiceMatch = 144,
};

struct LongestMatchResult {
  uint32_t ret = 0;          // r3 on return
  bool stored = false;       // match_start (+112) written
  uint32_t match_start = 0;  // the value written (last write wins, as in the guest)
  bool fast = false;         // host-contiguous fast path used
};

namespace detail {

// Generic accessor: any guest address, per-byte physical offset (used when a range crosses 0xE0000000 or wraps).
struct GuestBytes {
  const uint8_t* base;
  uint8_t at(uint32_t addr) const { return *GuestPtr(base, addr); }
};

// Length of the common run of scan/m from index 3, in [3, 258] (258 when all of 3..258 match).
inline uint32_t MatchLenFast(const uint8_t* scan, const uint8_t* m) {
  for (uint32_t i = 3; i < 259; i += 8) {
    uint64_t a, b;
    std::memcpy(&a, scan + i, 8);
    std::memcpy(&b, m + i, 8);
    const uint64_t x = a ^ b;
    if (x) return i + (uint32_t(__builtin_ctzll(x)) >> 3);  // little-endian host: lowest byte = first
  }
  return 258;
}

}  // namespace detail

// Computes the guest function's result and its one guest write. `write` = false leaves guest memory untouched
// (verify mode compares against the original first).
inline LongestMatchResult NativeLongestMatch(uint8_t* base, uint32_t s, uint32_t cur, bool write) {
  LongestMatchResult r;
  const uint32_t w_size = LoadBE32(base, s + kWSize);
  const uint32_t strstart = LoadBE32(base, s + kStrStart);
  const uint32_t window = LoadBE32(base, s + kWindow);
  const uint32_t prev_length = LoadBE32(base, s + kPrevLength);
  uint32_t chain = LoadBE32(base, s + kMaxChain);
  uint32_t nice = LoadBE32(base, s + kNiceMatch);
  const uint32_t good = LoadBE32(base, s + kGoodMatch);
  const uint32_t prev = LoadBE32(base, s + kPrev);
  const uint32_t wmask = LoadBE32(base, s + kWMask);
  const uint32_t lookahead = LoadBE32(base, s + kLookahead);

  const uint32_t limit = strstart > w_size - 262u ? strstart - w_size + 262u : 0u;
  if (!(prev_length < good)) chain >>= 2;
  if (nice > lookahead) nice = lookahead;
  const uint32_t scan = window + strstart;
  uint32_t best = prev_length;

  auto set_match = [&](uint32_t v) {
    r.stored = true;
    r.match_start = v;
    if (write) StoreBE32(base, s + kMatchStart, v);
  };

  // Fast path: every byte this call can read is host-contiguous and no guest address wraps. After the first
  // iteration cur is a u16 (prev[] entry); the compares read m[0..max(best, 258)] and scan[0..258], and
  // scan_end/scan_end1 at scan[best-1..best]. So with cur <= 0xFFFF and 1 <= prev_length <= 258 every read lies
  // in [window, window + max(0xFFFF, strstart) + 259) and prev[] reads in [prev, prev + 2 * (wmask + 1)).
  const uint64_t span_end = uint64_t(window) + (strstart > 0xFFFFu ? strstart : 0xFFFFu) + 259u;
  const uint64_t prev_end = uint64_t(prev) + 2ull * (uint64_t(wmask) + 1u);
  const bool fast = cur <= 0xFFFFu && prev_length >= 1u && prev_length <= 258u && wmask <= 0xFFFFu &&
                    span_end <= 0xFFFFFFFFull && prev_end <= 0xFFFFFFFFull &&
                    PhysHostOffset(window) == PhysHostOffset(uint32_t(span_end)) &&
                    PhysHostOffset(prev) == PhysHostOffset(uint32_t(prev_end));
  r.fast = fast;
  if (fast) {
    const uint8_t* W = GuestPtr(base, window);
    const uint8_t* P = GuestPtr(base, prev);
    const uint8_t* sp = W + strstart;
    // scan[best-1..best] and scan[0..1] as 16-bit words (one load per candidate instead of two bytes each;
    // the same bytes are compared, so the same candidates pass).
    auto ld16 = [](const uint8_t* p) {
      uint16_t v;
      std::memcpy(&v, p, 2);
      return v;
    };
    uint16_t scan_end = ld16(sp + best - 1u);
    const uint16_t scan_start = ld16(sp);
    for (;;) {
      const uint8_t* m = W + cur;
      if (ld16(m + best - 1u) == scan_end && ld16(m) == scan_start) {
        const uint32_t len = detail::MatchLenFast(sp, m);
        if (int32_t(len) > int32_t(best)) {
          set_match(cur);
          best = len;
          if (int32_t(len) >= int32_t(nice)) break;
          scan_end = ld16(sp + best - 1u);
        }
      }
      uint16_t next;
      std::memcpy(&next, P + 2u * (cur & wmask), 2);
      cur = __builtin_bswap16(next);
      if (cur <= limit || --chain == 0) break;
    }
  } else {
    // Literal structure of the guest loop with 32-bit address arithmetic (any state, any address).
    const detail::GuestBytes g{base};
    const uint32_t strend = scan + 258u;  // r5
    uint8_t end1 = g.at(scan + best - 1u);
    uint8_t end = g.at(scan + best);
    for (;;) {
      const uint32_t m = window + cur;
      if (g.at(m + best) == end && g.at(m + best - 1u) == end1 && g.at(m) == g.at(scan) &&
          g.at(m + 1u) == g.at(scan + 1u)) {
        uint32_t a = scan + 2u, b = m + 2u;  // r11, r10
        bool mismatch = false;
        do {
          for (int k = 0; k < 8; ++k) {
            ++a;
            ++b;
            if (g.at(a) != g.at(b)) {
              mismatch = true;
              break;
            }
          }
        } while (!mismatch && a < strend);
        const uint32_t len = a - strend + 258u;
        if (int32_t(len) > int32_t(best)) {
          set_match(cur);
          best = len;
          if (int32_t(len) >= int32_t(nice)) break;
          end1 = g.at(scan + best - 1u);
          end = g.at(scan + best);
        }
      }
      cur = LoadBE16(base, prev + 2u * (cur & wmask));
      if (!(cur > limit)) break;
      if (--chain == 0) break;
    }
  }
  r.ret = best <= lookahead ? best : lookahead;
  return r;
}

}  // namespace fable2::savecompress
