// native_inflate_fast.h - host-native equivalent of the guest's zlib inflate_fast (ProcessStructFields_82B59D28).
//
// The game's loader decompresses its data with its own copy of zlib 1.2.3: inflate = 82B53A40, inflate_table =
// 82B597E8, inflate_fast = 82B59D28 (the hot one: about 265 ms of the VFS finaliser in an area change's level-load
// call and most of the 3D Engine's async world-entry request; docs/load-fixes-20261016.md part G).
//
// This is a transcription of the recompiled guest code (generated/default/fable_2_recomp.196.cpp), not of the zlib
// source: every register is a 64-bit value updated by the same operation as the recompiler's (slw/srw with the
// 0x20 rule, 64-bit add/subf, 32-bit compares), every guest load and store happens at the same address in the same
// order with the same value, and on return r4-r11, r12, lr and r16-r31 hold exactly what the original leaves
// there. The differences are only how memory is reached:
//   - the input stream and literal stores use a host pointer per region (checked once at entry: the region does
//     not wrap and lies on one side of the 0xE0000000 physical step), instead of a volatile access with a per-byte
//     physical-offset select;
//   - each match copy is one call of CopyForward, which gives the result of the guest's forward byte loop (with
//     any overlap) through memcpy / 8-byte chunks / memset when the ranges allow it, or the byte loop itself.
// States the fast path does not cover (avail_in < 6, avail_out < 258, start < avail_out, regions that wrap or
// straddle 0xE0000000) are left to the recompiled function by the caller (Eligible() == false).
//
// Guest layout (zlib 1.2.3, 32-bit big-endian):
//   z_stream: next_in +0, avail_in +4, next_out +12, avail_out +16, msg +24, state +28
//   inflate_state: mode +0, wsize +40, whave +44, write +48, window +52, hold +56, bits +60, lencode +76,
//                  distcode +80, lenbits +84, distbits +88; code = {u8 op, u8 bits, u16 val} as one BE word.
#pragma once

#include <cstdint>
#include <cstring>

namespace fable2::inflatefast {

// Same as REX_PHYS_HOST_OFFSET in generated/default/fable_2_pch.h on macOS arm64 (checked by the test).
inline uint32_t PhysOff(uint32_t a) { return a >= 0xE0000000u ? 0x1000u : 0u; }
inline uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + PhysOff(a); }

inline uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &v, 4);
}
inline uint64_t Ld64(uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, Host(base, a), 8);
  return __builtin_bswap64(v);
}
inline void St64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(Host(base, a), &v, 8);
}

// True when [a, a + n) does not wrap and lies on one side of 0xE0000000 (one host offset for all of it).
inline bool OneRegion(uint32_t a, uint64_t n) {
  const uint64_t e = uint64_t(a) + n;
  if (e > 0x100000000ull) return false;
  return n == 0 || PhysOff(a) == PhysOff(uint32_t(e - 1));
}

// Guest: for i in 0..n-1 (n >= 1): mem[dst + i] = mem[src + i], one byte at a time, ascending, u32 addresses.
// Same result for any overlap. Returns the last byte written (the guest's register value after the loop).
inline uint8_t CopyForward(uint8_t* base, uint32_t dst, uint32_t src, uint32_t n) {
  if (OneRegion(dst, n) && OneRegion(src, n)) {
    uint8_t* d = Host(base, dst);
    const uint8_t* s = Host(base, src);
    if (s + n <= d || d + n <= s) {
      std::memcpy(d, s, n);  // disjoint
    } else if (s > d || d - s >= 8) {
      // Source ahead of the destination, or at least 8 behind: an 8-byte chunk never reads a byte that an earlier
      // write of the same chunk changes, so chunks give the byte loop's result.
      uint32_t i = 0;
      for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, s + i, 8);
        std::memcpy(d + i, &v, 8);
      }
      for (; i < n; ++i) d[i] = s[i];
    } else if (d - s == 1) {
      std::memset(d, s[0], n);  // run of one byte
    } else {
      for (uint32_t i = 0; i < n; ++i) d[i] = s[i];  // short period (2..7): the byte loop
    }
    return d[n - 1];
  }
  uint8_t v = 0;
  for (uint32_t i = 0; i < n; ++i) {
    v = *Host(base, src + i);
    *Host(base, dst + i) = v;
  }
  return v;
}

struct Regs {
  uint64_t r4, r5, r6, r7, r8, r9, r10, r11, r12, lr;
  uint64_t r16_31[16];
};

// Entry state the fast path needs (else the caller runs the recompiled function).
inline bool Eligible(uint8_t* base, uint32_t strm, uint32_t start) {
  if (!OneRegion(strm, 56)) return false;
  const uint32_t next_in = Ld32(base, strm + 0), avail_in = Ld32(base, strm + 4);
  const uint32_t next_out = Ld32(base, strm + 12), avail_out = Ld32(base, strm + 16);
  if (avail_in < 6 || avail_out < 258 || start < avail_out) return false;
  // Input reads stay in [next_in, next_in + avail_in] (at most 7 bytes per symbol, the loop stops once fewer than
  // 6 are left); literal stores in [next_out, next_out + avail_out - 257]. Margins added.
  if (!OneRegion(next_in, uint64_t(avail_in) + 16)) return false;
  if (!OneRegion(next_out, uint64_t(avail_out) + 16)) return false;
  return true;
}

// Runs the function for ctx.r3 = strm, ctx.r4 = start (Eligible() must be true). Writes guest memory exactly as the
// original and returns the registers the original leaves (the caller copies them into ctx).
inline void Run(uint8_t* base, uint64_t r1, uint64_t r3, uint64_t r4, uint64_t lr_in, const uint64_t* r16_31,
                Regs& out) {
  // mflr r12; bl __savegprlr_16
  const uint64_t r12_entry = lr_in;
  for (int i = 0; i < 16; ++i) St64(base, uint32_t(r1) - 136 + 8 * i, r16_31[i]);
  St32(base, uint32_t(r1) - 8, uint32_t(r12_entry));

  uint64_t r5, r6, r7, r8, r9, r10, r11;
  uint64_t r17, r18, r19, r20, r21, r22, r23, r24, r25, r26, r27, r28, r29, r30, r31;
  auto slw = [](uint64_t x, uint64_t n) -> uint64_t {
    return (n & 0x20) ? 0 : uint64_t(uint32_t(uint32_t(x) << (n & 0x3F)));
  };
  auto srw = [](uint64_t x, uint64_t n) -> uint64_t { return (n & 0x20) ? 0 : uint64_t(uint32_t(x) >> (n & 0x3F)); };
  auto ld = [&](uint64_t a) -> uint64_t { return Ld32(base, uint32_t(a)); };

  r30 = ld(r3 + 28);
  r29 = 1;
  r11 = ld(r3 + 0);
  r17 = 3;
  r10 = ld(r3 + 12);
  r31 = r11 + uint64_t(-1);
  r11 = ld(r3 + 16);
  r9 = ld(r3 + 4);
  r8 = r10 + uint64_t(-1);
  r10 = ld(r30 + 84);
  r7 = r11 - r4;
  r5 = ld(r30 + 88);
  r9 = r9 + r31;
  r6 = slw(r29, r10);
  r25 = ld(r30 + 40);
  r11 = r11 + r8;
  r20 = ld(r30 + 44);
  r5 = slw(r29, r5);
  r28 = ld(r30 + 48);
  r21 = r9 + uint64_t(-5);
  r19 = ld(r30 + 52);
  r10 = ld(r30 + 56);
  r24 = r7 + r8;
  r9 = ld(r30 + 60);
  r18 = r11 + uint64_t(-257);
  r27 = ld(r30 + 76);
  r23 = r6 + uint64_t(-1);
  r26 = ld(r30 + 80);
  r22 = r5 + uint64_t(-1);
  (void)r17;

  // Input bytes and literal stores: one host offset each (Eligible() checked the regions).
  const uint32_t next_in = uint32_t(r31 + 1), next_out = uint32_t(r8 + 1);
  uint8_t* const in_host = base + PhysOff(next_in);    // + guest address
  uint8_t* const out_host = base + PhysOff(next_out);  // + guest address
  auto in8 = [&](uint64_t a) -> uint64_t { return in_host[uint32_t(a)]; };
  auto tab = [&](uint64_t a) -> uint64_t { return Ld32(base, uint32_t(a)); };
  auto any8 = [&](uint64_t a) -> uint64_t { return *Host(base, uint32_t(a)); };

  uint64_t r4v = r4;  // r4 is reused as a scratch register below
#define R4 r4v
loc_DA4:
  if (uint32_t(r9) >= 15u) goto loc_DD4;
  r11 = r31 + 1;
  r7 = in8(r31 + 1);
  r31 = r11 + 1;
  r7 = slw(r7, r9);
  r6 = in8(r11 + 1);
  r11 = r9 + 8;
  r10 = r7 + r10;
  r9 = slw(r6, r11);
  r10 = r9 + r10;
  r9 = r11 + 8;
loc_DD4:
  r11 = r23 & r10;
  r7 = uint32_t(uint32_t(r11) << 2);
  r11 = tab(r7 + r27);
  r6 = (uint32_t(r11) >> 16) & 0xFF;
  r7 = (uint32_t(r11) >> 24) & 0xFF;
  r10 = srw(r10, r6);
  r9 = r9 - r6;
  if (uint32_t(r7) == 0) goto loc_literal;
loc_DF8:
  r6 = uint32_t(r7) & 0x10;
  if (uint32_t(r6) != 0) goto loc_E48;
  r6 = uint32_t(r7) & 0x40;
  if (uint32_t(r6) != 0) goto loc_A14C;
  r6 = uint32_t(r11) & 0xFFFF;
  r11 = slw(r29, r7);
  r11 = r11 + uint64_t(-1);
  r11 = r11 & r10;
  r7 = r11 + r6;
  r6 = uint32_t(uint32_t(r7) << 2);
  r11 = tab(r6 + r27);
  r5 = (uint32_t(r11) >> 16) & 0xFF;
  r7 = (uint32_t(r11) >> 24) & 0xFF;
  r10 = srw(r10, r5);
  r9 = r9 - r5;
  if (uint32_t(r7) != 0) goto loc_DF8;
  goto loc_literal;
loc_E48:
  r7 = uint32_t(r7) & 0xF;
  r6 = uint32_t(r11) & 0xFFFF;
  if (uint32_t(r7) == 0) goto loc_E8C;
  if (uint32_t(r9) >= uint32_t(r7)) goto loc_E74;
  r31 = r31 + 1;
  r11 = in8(r31);
  r11 = slw(r11, r9);
  r10 = r11 + r10;
  r9 = r9 + 8;
loc_E74:
  r11 = slw(r29, r7);
  r11 = r11 + uint64_t(-1);
  r9 = r9 - r7;
  r11 = r11 & r10;
  r10 = srw(r10, r7);
  r6 = r11 + r6;
loc_E8C:
  if (uint32_t(r9) >= 15u) goto loc_EBC;
  r11 = r31 + 1;
  r7 = in8(r31 + 1);
  r31 = r11 + 1;
  r7 = slw(r7, r9);
  r5 = in8(r11 + 1);
  r11 = r9 + 8;
  r10 = r7 + r10;
  r9 = slw(r5, r11);
  r10 = r9 + r10;
  r9 = r11 + 8;
loc_EBC:
  r11 = r22 & r10;
  r7 = uint32_t(uint32_t(r11) << 2);
  r11 = tab(r7 + r26);
  r5 = (uint32_t(r11) >> 16) & 0xFF;
  R4 = (uint32_t(r11) >> 24) & 0x10;
  r10 = srw(r10, r5);
  r9 = r9 - r5;
  r7 = (uint32_t(r11) >> 24) & 0xFF;
  if (uint32_t(R4) != 0) goto loc_F28;
loc_EE4:
  r5 = uint32_t(r7) & 0x40;
  if (uint32_t(r5) != 0) goto loc_A140;
  r5 = uint32_t(r11) & 0xFFFF;
  r11 = slw(r29, r7);
  r11 = r11 + uint64_t(-1);
  r11 = r11 & r10;
  r7 = r11 + r5;
  r5 = uint32_t(uint32_t(r7) << 2);
  r11 = tab(r5 + r26);
  R4 = (uint32_t(r11) >> 16) & 0xFF;
  r5 = (uint32_t(r11) >> 24) & 0x10;
  r10 = srw(r10, R4);
  r9 = r9 - R4;
  r7 = (uint32_t(r11) >> 24) & 0xFF;
  if (uint32_t(r5) == 0) goto loc_EE4;
loc_F28:
  r7 = uint32_t(r7) & 0xF;
  r5 = uint32_t(r11) & 0xFFFF;
  if (uint32_t(r9) >= uint32_t(r7)) goto loc_F68;
  r31 = r31 + 1;
  r11 = in8(r31);
  r11 = slw(r11, r9);
  r9 = r9 + 8;
  r10 = r11 + r10;
  if (uint32_t(r9) >= uint32_t(r7)) goto loc_F68;
  r31 = r31 + 1;
  r11 = in8(r31);
  r11 = slw(r11, r9);
  r10 = r11 + r10;
  r9 = r9 + 8;
loc_F68:
  r11 = slw(r29, r7);
  r11 = r11 + uint64_t(-1);
  r9 = r9 - r7;
  r11 = r11 & r10;
  r10 = srw(r10, r7);
  R4 = r11 + r5;  // dist
  r11 = r8 - r24;  // out - beg
  if (uint32_t(R4) <= uint32_t(r11)) goto loc_A0B8;
  r7 = R4 - r11;  // distance back in the window
  if (uint32_t(r7) > uint32_t(r20)) goto loc_A134;
  r5 = r19 + uint64_t(-1);
  r11 = r5 - r7;
  if (uint32_t(r28) != 0) goto loc_FD4;
  r11 = r11 + r25;
  if (uint32_t(r7) >= uint32_t(r6)) goto loc_A064;
  r6 = r6 - r7;
  {  // loc_FB8: r7 bytes from r11+1 to r8+1
    const uint32_t n = uint32_t(r7);
    if (n == 0) goto loc_bad_count;
    r5 = CopyForward(base, uint32_t(r8 + 1), uint32_t(r11 + 1), n);
    r11 += n;
    r8 += n;
    r7 -= n;
  }
  goto loc_A060;
loc_FD4:
  r11 = r11 + r28;
  if (uint32_t(r28) >= uint32_t(r7)) goto loc_A03C;
  r7 = r7 - r28;
  r11 = r11 + r25;
  if (uint32_t(r7) >= uint32_t(r6)) goto loc_A064;
  r6 = r6 - r7;
  {  // loc_FF4 (byte register r16 is restored by __restgprlr_16)
    const uint32_t n = uint32_t(r7);
    if (n == 0) goto loc_bad_count;
    CopyForward(base, uint32_t(r8 + 1), uint32_t(r11 + 1), n);
    r11 += n;
    r8 += n;
    r7 -= n;
  }
  r11 = r5;
  if (uint32_t(r28) >= uint32_t(r6)) goto loc_A064;
  r7 = r28;
  r6 = r6 - r28;
  {  // loc_A020
    const uint32_t n = uint32_t(r7);
    if (n == 0) goto loc_bad_count;
    r5 = CopyForward(base, uint32_t(r8 + 1), uint32_t(r11 + 1), n);
    r11 += n;
    r8 += n;
    r7 -= n;
  }
  goto loc_A060;
loc_A03C:
  if (uint32_t(r7) >= uint32_t(r6)) goto loc_A064;
  r6 = r6 - r7;
  {  // loc_A048
    const uint32_t n = uint32_t(r7);
    if (n == 0) goto loc_bad_count;
    r5 = CopyForward(base, uint32_t(r8 + 1), uint32_t(r11 + 1), n);
    r11 += n;
    r8 += n;
    r7 -= n;
  }
loc_A060:
  r11 = r8 - R4;
loc_A064:
  if (uint32_t(r6) <= 2u) goto loc_A0F8;
  r7 = r6 + uint64_t(-3);
  r7 = uint32_t(uint32_t(r7) / 3u);
  r7 = r7 + 1;
  {  // loc_A078: r7 iterations of 3 bytes (r7 >= 1 here)
    const uint32_t k = uint32_t(r7);
    const uint32_t n = 3u * k;  // k <= 0x55555556; n fits unless k is the maximum (len >= 2^32 - 2)
    if (uint64_t(k) * 3u > 0xFFFFFFFFull) goto loc_bad_count;
    r5 = CopyForward(base, uint32_t(r8 + 1), uint32_t(r11 + 1), n);
    R4 = *Host(base, uint32_t(r8 + n - 1));  // second byte of the last triple (as written)
    r11 += n;
    r8 += n;
    r7 -= k;
    r6 -= uint64_t(n);
  }
  goto loc_A0F8;
loc_A0B8:
  r11 = r8 - R4;
  if (uint32_t(r6) < 3u) goto loc_bad_count;  // the guest's do-while would wrap r6 and copy ~4 GB
  {  // loc_A0BC: do { 3 bytes; r6 -= 3 } while (r6 > 2)
    const uint32_t k = (uint32_t(r6) - 3u) / 3u + 1u;
    const uint32_t n = 3u * k;
    CopyForward(base, uint32_t(r8 + 1), uint32_t(r11 + 1), n);
    r7 = *Host(base, uint32_t(r8 + n - 2));
    r5 = *Host(base, uint32_t(r8 + n - 1));
    R4 = *Host(base, uint32_t(r8 + n));
    r11 += n;
    r8 += n;
    r6 -= uint64_t(n);
  }
loc_A0F8:
  if (uint32_t(r6) == 0) goto loc_A120;
  r7 = any8(r11 + 1);
  r8 = r8 + 1;
  *Host(base, uint32_t(r8)) = uint8_t(r7);
  if (uint32_t(r6) <= 1u) goto loc_A120;
  r11 = any8(r11 + 2);
  r8 = r8 + 1;
  *Host(base, uint32_t(r8)) = uint8_t(r11);
  goto loc_A120;
loc_literal:  // loc_A118 reached from a literal: r8 < end (or the first symbol), inside the output region
  r8 = r8 + 1;
  out_host[uint32_t(r8)] = uint8_t(r11);
loc_A120:
  if (uint32_t(r31) >= uint32_t(r21)) goto loc_A178;
  if (uint32_t(r8) < uint32_t(r18)) goto loc_DA4;
  goto loc_A178;
loc_A134:
  r11 = uint64_t(int64_t(-2112880640));
  r6 = r11 + uint64_t(-25328);
  goto loc_A16C;
loc_A140:
  r11 = uint64_t(int64_t(-2112880640));
  r6 = r11 + uint64_t(-25352);
  goto loc_A16C;
loc_A14C:
  r11 = uint32_t(r7) & 0x20;
  if (uint32_t(r11) == 0) goto loc_A164;
  r11 = 11;
  St32(base, uint32_t(r30 + 0), uint32_t(r11));
  goto loc_A178;
loc_A164:
  r11 = uint64_t(int64_t(-2112880640));
  r6 = r11 + uint64_t(-25380);
loc_A16C:
  r7 = 27;
  St32(base, uint32_t(r3 + 24), uint32_t(r6));
  St32(base, uint32_t(r30 + 0), uint32_t(r7));
loc_A178:
  r11 = uint32_t(r9) & 0xFFFFFFF8u;
  r6 = (uint32_t(r9) >> 3) & 0x1FFFFFFFu;
  r7 = r9 - r11;
  r11 = r31 - r6;
  r9 = slw(r29, r7);
  R4 = r11 + 1;
  r11 = r21 - r11;
  St32(base, uint32_t(r3 + 0), uint32_t(R4));
  r5 = r9 + uint64_t(-1);
  r11 = r11 + 5;
  r9 = r8 + 1;
  St32(base, uint32_t(r3 + 4), uint32_t(r11));
  r11 = r18 - r8;
  r10 = r5 & r10;
  St32(base, uint32_t(r3 + 12), uint32_t(r9));
  r11 = r11 + 257;
  St32(base, uint32_t(r3 + 16), uint32_t(r11));
  St32(base, uint32_t(r30 + 56), uint32_t(r10));
  St32(base, uint32_t(r30 + 60), uint32_t(r7));
  // b __restgprlr_16
  for (int i = 0; i < 16; ++i) out.r16_31[i] = Ld64(base, uint32_t(r1) - 136 + 8 * i);
  out.r12 = Ld32(base, uint32_t(r1) - 8);
  out.lr = out.r12;
  out.r4 = R4;
  out.r5 = r5;
  out.r6 = r6;
  out.r7 = r7;
  out.r8 = r8;
  out.r9 = r9;
  out.r10 = r10;
  out.r11 = r11;
  return;
loc_bad_count:
  // A copy count the guest would run for ~2^32 bytes (impossible with zlib's own tables): not reproduced here.
  // Unreachable for states built by inflate(); the caller only gets here with a corrupt state and traps.
  __builtin_trap();
#undef R4
}

// The override's whole native path on a recompiler context: Run() for ctx.r3 = strm, ctx.r4 = start, then the
// registers the original leaves. Shared by the override and tests/inflate_fast_tests.cpp.
template <class Ctx>
inline void RunCtx(Ctx& ctx, uint8_t* base) {
  const uint64_t gpr[16] = {ctx.r16.u64, ctx.r17.u64, ctx.r18.u64, ctx.r19.u64, ctx.r20.u64, ctx.r21.u64,
                            ctx.r22.u64, ctx.r23.u64, ctx.r24.u64, ctx.r25.u64, ctx.r26.u64, ctx.r27.u64,
                            ctx.r28.u64, ctx.r29.u64, ctx.r30.u64, ctx.r31.u64};
  Regs r;
  Run(base, ctx.r1.u64, ctx.r3.u64, ctx.r4.u64, ctx.lr, gpr, r);
  ctx.r4.u64 = r.r4;
  ctx.r5.u64 = r.r5;
  ctx.r6.u64 = r.r6;
  ctx.r7.u64 = r.r7;
  ctx.r8.u64 = r.r8;
  ctx.r9.u64 = r.r9;
  ctx.r10.u64 = r.r10;
  ctx.r11.u64 = r.r11;
  ctx.r12.u64 = r.r12;
  ctx.lr = r.lr;
  auto* g = &r.r16_31[0];
  ctx.r16.u64 = g[0], ctx.r17.u64 = g[1], ctx.r18.u64 = g[2], ctx.r19.u64 = g[3], ctx.r20.u64 = g[4];
  ctx.r21.u64 = g[5], ctx.r22.u64 = g[6], ctx.r23.u64 = g[7], ctx.r24.u64 = g[8], ctx.r25.u64 = g[9];
  ctx.r26.u64 = g[10], ctx.r27.u64 = g[11], ctx.r28.u64 = g[12], ctx.r29.u64 = g[13], ctx.r30.u64 = g[14];
  ctx.r31.u64 = g[15];
}

}  // namespace fable2::inflatefast
