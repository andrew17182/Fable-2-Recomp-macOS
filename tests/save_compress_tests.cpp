// Build-time check of the native save-compressor search (src/core/hotfunc/save/, FABLE2_NATIVE_SAVE_COMPRESS,
// docs/load-fixes-20261016.md): the native longest_match must match the recompiled sub_82B52630 on every call.
//
// "Original" below is the recompiler's literal output for sub_82B52630 (generated/default/fable_2_recomp.121.cpp)
// with its __savegprlr_23 / __restgprlr_23 helpers. Both run on the same fake guest memory (a 4 GB reservation,
// like the game's) with the same registers; after each call the test compares r3, r1, r2, r12, r13-r31, lr, the
// whole deflate_state (incl. match_start), the 80-byte stack save area and the window/prev checksums.
// Volatile r4-r11 are not compared (dead under the PPC ABI; the two guest callers read r3 only).
//
// Inputs: a zlib 1.2.3 deflate_slow / deflate_fast driver (fill_window sliding, hash chains) over random,
// low-entropy, run, period and text-like buffers at levels 1-9, window bits 9-15 and memLevel 1-9, in normal,
// physical (>= 0xE0000000) and boundary-straddling layouts; plus a direct fuzz of raw states (any field values,
// generic path). Optional: save_compress_tests <dir>... replays captured real save inputs
// (FABLE2_SAVE_COMPRESS_CAPTURE, NNNN-in.bin + params.txt) at the captured settings. --bench times both.
#include "fable_2_pch.h"

#include <sys/mman.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <random>
#include <string>
#include <vector>

#include "native_longest_match.h"

namespace sc = fable2::savecompress;

extern "C" void __savegprlr_23(PPCContext& ctx, uint8_t* base) {
	REX_STORE_U64(ctx.r1.u32 + -80, ctx.r23.u64);
	REX_STORE_U64(ctx.r1.u32 + -72, ctx.r24.u64);
	REX_STORE_U64(ctx.r1.u32 + -64, ctx.r25.u64);
	REX_STORE_U64(ctx.r1.u32 + -56, ctx.r26.u64);
	REX_STORE_U64(ctx.r1.u32 + -48, ctx.r27.u64);
	REX_STORE_U64(ctx.r1.u32 + -40, ctx.r28.u64);
	REX_STORE_U64(ctx.r1.u32 + -32, ctx.r29.u64);
	REX_STORE_U64(ctx.r1.u32 + -24, ctx.r30.u64);
	REX_STORE_U64(ctx.r1.u32 + -16, ctx.r31.u64);
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
}

extern "C" void __restgprlr_23(PPCContext& ctx, uint8_t* base) {
	ctx.r23.u64 = REX_LOAD_U64(ctx.r1.u32 + -80);
	ctx.r24.u64 = REX_LOAD_U64(ctx.r1.u32 + -72);
	ctx.r25.u64 = REX_LOAD_U64(ctx.r1.u32 + -64);
	ctx.r26.u64 = REX_LOAD_U64(ctx.r1.u32 + -56);
	ctx.r27.u64 = REX_LOAD_U64(ctx.r1.u32 + -48);
	ctx.r28.u64 = REX_LOAD_U64(ctx.r1.u32 + -40);
	ctx.r29.u64 = REX_LOAD_U64(ctx.r1.u32 + -32);
	ctx.r30.u64 = REX_LOAD_U64(ctx.r1.u32 + -24);
	ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
	ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);
	ctx.lr = ctx.r12.u64;
}

// --- recompiler output for sub_82B52630, verbatim body ---
static void Original_82B52630(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr6{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// bl 0x82ca2bd4
	ctx.lr = 0x82B52638;
	__savegprlr_23(ctx, base);
	// mr r7,r3
	ctx.r7.u64 = ctx.r3.u64;
	// lwz r8,44(r7)
	ctx.r8.u64 = REX_LOAD_U32(ctx.r7.u32 + 44);
	// lwz r11,108(r7)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r7.u32 + 108);
	// lwz r30,56(r7)
	ctx.r30.u64 = REX_LOAD_U32(ctx.r7.u32 + 56);
	// addi r5,r8,-262
	ctx.r5.s64 = ctx.r8.s64 + -262;
	// lwz r9,120(r7)
	ctx.r9.u64 = REX_LOAD_U32(ctx.r7.u32 + 120);
	// lwz r25,124(r7)
	ctx.r25.u64 = REX_LOAD_U32(ctx.r7.u32 + 124);
	// add r10,r30,r11
	ctx.r10.u64 = ctx.r30.u64 + ctx.r11.u64;
	// lwz r27,144(r7)
	ctx.r27.u64 = REX_LOAD_U32(ctx.r7.u32 + 144);
	// mr r6,r9
	ctx.r6.u64 = ctx.r9.u64;
	// cmplw cr6,r11,r5
	cr6.compare<uint32_t>(ctx.r11.u32, ctx.r5.u32, xer);
	// ble cr6,0x82b52674
	if (!cr6.gt) goto loc_82B52674;
	// subf r11,r8,r11
	ctx.r11.u64 = ctx.r11.u64 - ctx.r8.u64;
	// addi r26,r11,262
	ctx.r26.s64 = ctx.r11.s64 + 262;
	// b 0x82b52678
	goto loc_82B52678;
loc_82B52674:
	// li r26,0
	ctx.r26.s64 = 0;
loc_82B52678:
	// add r11,r6,r10
	ctx.r11.u64 = ctx.r6.u64 + ctx.r10.u64;
	// lwz r8,140(r7)
	ctx.r8.u64 = REX_LOAD_U32(ctx.r7.u32 + 140);
	// lwz r29,64(r7)
	ctx.r29.u64 = REX_LOAD_U32(ctx.r7.u32 + 64);
	// addi r5,r10,258
	ctx.r5.s64 = ctx.r10.s64 + 258;
	// cmplw cr6,r9,r8
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r8.u32, xer);
	// lwz r28,52(r7)
	ctx.r28.u64 = REX_LOAD_U32(ctx.r7.u32 + 52);
	// lbzx r8,r6,r10
	ctx.r8.u64 = REX_LOAD_U8(ctx.r6.u32 + ctx.r10.u32);
	// lbz r31,-1(r11)
	ctx.r31.u64 = REX_LOAD_U8(ctx.r11.u32 + -1);
	// blt cr6,0x82b526a0
	if (cr6.lt) goto loc_82B526A0;
	// rlwinm r25,r25,30,2,31
	ctx.r25.u64 = __builtin_rotateleft64(ctx.r25.u32 | (ctx.r25.u64 << 32), 30) & 0x3FFFFFFF;
loc_82B526A0:
	// lwz r3,116(r7)
	ctx.r3.u64 = REX_LOAD_U32(ctx.r7.u32 + 116);
	// cmplw cr6,r27,r3
	cr6.compare<uint32_t>(ctx.r27.u32, ctx.r3.u32, xer);
	// ble cr6,0x82b526b0
	if (!cr6.gt) goto loc_82B526B0;
	// mr r27,r3
	ctx.r27.u64 = ctx.r3.u64;
loc_82B526B0:
	// add r11,r30,r4
	ctx.r11.u64 = ctx.r30.u64 + ctx.r4.u64;
	// clrlwi r24,r8,24
	ctx.r24.u64 = ctx.r8.u32 & 0xFF;
	// add r9,r11,r6
	ctx.r9.u64 = ctx.r11.u64 + ctx.r6.u64;
	// lbzx r23,r11,r6
	ctx.r23.u64 = REX_LOAD_U8(ctx.r11.u32 + ctx.r6.u32);
	// cmplw cr6,r23,r24
	cr6.compare<uint32_t>(ctx.r23.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527fc
	if (!cr6.eq) goto loc_82B527FC;
	// lbz r9,-1(r9)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r9.u32 + -1);
	// clrlwi r24,r31,24
	ctx.r24.u64 = ctx.r31.u32 & 0xFF;
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527fc
	if (!cr6.eq) goto loc_82B527FC;
	// lbz r9,0(r11)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
	// lbz r24,0(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 0);
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527fc
	if (!cr6.eq) goto loc_82B527FC;
	// addi r9,r11,1
	ctx.r9.s64 = ctx.r11.s64 + 1;
	// lbz r24,1(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 1);
	// lbz r11,1(r11)
	ctx.r11.u64 = REX_LOAD_U8(ctx.r11.u32 + 1);
	// cmplw cr6,r11,r24
	cr6.compare<uint32_t>(ctx.r11.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527fc
	if (!cr6.eq) goto loc_82B527FC;
	// addi r11,r10,2
	ctx.r11.s64 = ctx.r10.s64 + 2;
	// addi r10,r9,1
	ctx.r10.s64 = ctx.r9.s64 + 1;
loc_82B52704:
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// addi r10,r10,1
	ctx.r10.s64 = ctx.r10.s64 + 1;
	// lbz r9,0(r11)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
	// lbz r24,0(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 0);
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527cc
	if (!cr6.eq) goto loc_82B527CC;
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// addi r10,r10,1
	ctx.r10.s64 = ctx.r10.s64 + 1;
	// lbz r9,0(r11)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
	// lbz r24,0(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 0);
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527cc
	if (!cr6.eq) goto loc_82B527CC;
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// addi r10,r10,1
	ctx.r10.s64 = ctx.r10.s64 + 1;
	// lbz r9,0(r11)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
	// lbz r24,0(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 0);
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527cc
	if (!cr6.eq) goto loc_82B527CC;
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// addi r10,r10,1
	ctx.r10.s64 = ctx.r10.s64 + 1;
	// lbz r9,0(r11)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
	// lbz r24,0(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 0);
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527cc
	if (!cr6.eq) goto loc_82B527CC;
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// addi r10,r10,1
	ctx.r10.s64 = ctx.r10.s64 + 1;
	// lbz r9,0(r11)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
	// lbz r24,0(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 0);
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527cc
	if (!cr6.eq) goto loc_82B527CC;
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// addi r10,r10,1
	ctx.r10.s64 = ctx.r10.s64 + 1;
	// lbz r9,0(r11)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
	// lbz r24,0(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 0);
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527cc
	if (!cr6.eq) goto loc_82B527CC;
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// addi r10,r10,1
	ctx.r10.s64 = ctx.r10.s64 + 1;
	// lbz r9,0(r11)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
	// lbz r24,0(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 0);
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527cc
	if (!cr6.eq) goto loc_82B527CC;
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// addi r10,r10,1
	ctx.r10.s64 = ctx.r10.s64 + 1;
	// lbz r9,0(r11)
	ctx.r9.u64 = REX_LOAD_U8(ctx.r11.u32 + 0);
	// lbz r24,0(r10)
	ctx.r24.u64 = REX_LOAD_U8(ctx.r10.u32 + 0);
	// cmplw cr6,r9,r24
	cr6.compare<uint32_t>(ctx.r9.u32, ctx.r24.u32, xer);
	// bne cr6,0x82b527cc
	if (!cr6.eq) goto loc_82B527CC;
	// cmplw cr6,r11,r5
	cr6.compare<uint32_t>(ctx.r11.u32, ctx.r5.u32, xer);
	// blt cr6,0x82b52704
	if (cr6.lt) goto loc_82B52704;
loc_82B527CC:
	// subf r11,r5,r11
	ctx.r11.u64 = ctx.r11.u64 - ctx.r5.u64;
	// addi r10,r5,-258
	ctx.r10.s64 = ctx.r5.s64 + -258;
	// addi r11,r11,258
	ctx.r11.s64 = ctx.r11.s64 + 258;
	// cmpw cr6,r11,r6
	cr6.compare<int32_t>(ctx.r11.s32, ctx.r6.s32, xer);
	// ble cr6,0x82b527fc
	if (!cr6.gt) goto loc_82B527FC;
	// stw r4,112(r7)
	REX_STORE_U32(ctx.r7.u32 + 112, ctx.r4.u32);
	// mr r6,r11
	ctx.r6.u64 = ctx.r11.u64;
	// cmpw cr6,r11,r27
	cr6.compare<int32_t>(ctx.r11.s32, ctx.r27.s32, xer);
	// bge cr6,0x82b52818
	if (!cr6.lt) goto loc_82B52818;
	// add r9,r11,r10
	ctx.r9.u64 = ctx.r11.u64 + ctx.r10.u64;
	// lbzx r8,r11,r10
	ctx.r8.u64 = REX_LOAD_U8(ctx.r11.u32 + ctx.r10.u32);
	// lbz r31,-1(r9)
	ctx.r31.u64 = REX_LOAD_U8(ctx.r9.u32 + -1);
loc_82B527FC:
	// and r11,r28,r4
	ctx.r11.u64 = ctx.r28.u64 & ctx.r4.u64;
	// rlwinm r9,r11,1,0,30
	ctx.r9.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 1) & 0xFFFFFFFE;
	// lhzx r4,r9,r29
	ctx.r4.u64 = REX_LOAD_U16(ctx.r9.u32 + ctx.r29.u32);
	// cmplw cr6,r4,r26
	cr6.compare<uint32_t>(ctx.r4.u32, ctx.r26.u32, xer);
	// ble cr6,0x82b52818
	if (!cr6.gt) goto loc_82B52818;
	// addic. r25,r25,-1
	xer.ca = ctx.r25.u32 > 0;
	ctx.r25.s64 = ctx.r25.s64 + -1;
	cr0.compare<int32_t>(ctx.r25.s32, 0, xer);
	// bne 0x82b526b0
	if (!cr0.eq) goto loc_82B526B0;
loc_82B52818:
	// cmplw cr6,r6,r3
	cr6.compare<uint32_t>(ctx.r6.u32, ctx.r3.u32, xer);
	// bgt cr6,0x82b52824
	if (cr6.gt) goto loc_82B52824;
	// mr r3,r6
	ctx.r3.u64 = ctx.r6.u64;
loc_82B52824:
	// b 0x82ca2c24
	__restgprlr_23(ctx, base);
	return;
}


// --- native side: the override's exact code path (save area + NativeLongestMatch) ---
static void Native_82B52630(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;
  REX_STORE_U64(sp + -80, ctx.r23.u64);
  REX_STORE_U64(sp + -72, ctx.r24.u64);
  REX_STORE_U64(sp + -64, ctx.r25.u64);
  REX_STORE_U64(sp + -56, ctx.r26.u64);
  REX_STORE_U64(sp + -48, ctx.r27.u64);
  REX_STORE_U64(sp + -40, ctx.r28.u64);
  REX_STORE_U64(sp + -32, ctx.r29.u64);
  REX_STORE_U64(sp + -24, ctx.r30.u64);
  REX_STORE_U64(sp + -16, ctx.r31.u64);
  REX_STORE_U32(sp + -8, ctx.r12.u32);
  ctx.r12.u64 = ctx.r12.u32;
  ctx.lr = ctx.r12.u64;
  const sc::LongestMatchResult r = sc::NativeLongestMatch(base, ctx.r3.u32, ctx.r4.u32, true);
  ctx.r3.u64 = r.ret;
}

namespace {

uint8_t* g_base = nullptr;
std::mt19937_64 g_rng(0xF4B1E2u);
uint64_t g_calls = 0, g_fail = 0, g_fast = 0;
const char* g_case = "";

uint8_t* H(uint32_t a) { return g_base + a + sc::PhysHostOffset(a); }
void W32(uint32_t a, uint32_t v) { sc::StoreBE32(g_base, a, v); }
uint32_t R32(uint32_t a) { return sc::LoadBE32(g_base, a); }

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}
// Byte range [a, a + n) through guest addressing (handles the 0xE0000000 step and wrap).
std::vector<uint8_t> Grab(uint32_t a, uint32_t n) {
  std::vector<uint8_t> v(n);
  for (uint32_t i = 0; i < n; ++i) v[i] = *H(a + i);
  return v;
}
void Put(uint32_t a, const std::vector<uint8_t>& v) {
  for (uint32_t i = 0; i < v.size(); ++i) *H(a + i) = v[i];
}

constexpr uint32_t kStack = 0x70010000u;
constexpr uint32_t kStateSize = 148;

// One call, both implementations, everything compared. Returns the agreed r3; match_start is left as the
// original wrote it.
uint32_t CheckedCall(uint32_t s, uint32_t cur, uint32_t ext_lo, uint32_t ext_n) {
  PPCContext a{};
  std::uniform_int_distribution<uint64_t> d;
  // Arbitrary register contents, incl. upper halves (the guest is 64-bit).
  for (PPCRegister* r : {&a.r0, &a.r2, &a.r5, &a.r6, &a.r7, &a.r8, &a.r9, &a.r10, &a.r11, &a.r12, &a.r13,
                         &a.r14, &a.r15, &a.r16, &a.r17, &a.r18, &a.r19, &a.r20, &a.r21, &a.r22, &a.r23,
                         &a.r24, &a.r25, &a.r26, &a.r27, &a.r28, &a.r29, &a.r30, &a.r31}) r->u64 = d(g_rng);
  a.r1.u64 = kStack;
  a.r3.u64 = s;
  a.r4.u64 = (g_rng() & 1) ? cur : (uint64_t(cur) | (d(g_rng) << 32));
  a.lr = 0x82B52E80u;
  PPCContext b = a;
  const auto state0 = Grab(s, kStateSize);
  const auto stack0 = Grab(kStack - 80, 80);
  const uint64_t ext0 = ext_n ? Fnv(Grab(ext_lo, ext_n).data(), ext_n) : 0;

  Original_82B52630(a, g_base);
  const auto state_a = Grab(s, kStateSize);
  const auto stack_a = Grab(kStack - 80, 80);
  Put(s, state0);
  Put(kStack - 80, stack0);
  Native_82B52630(b, g_base);
  const auto state_b = Grab(s, kStateSize);
  const auto stack_b = Grab(kStack - 80, 80);
  const uint64_t ext1 = ext_n ? Fnv(Grab(ext_lo, ext_n).data(), ext_n) : 0;

  ++g_calls;
  bool ok = a.r3.u64 == b.r3.u64 && a.r1.u64 == b.r1.u64 && a.r2.u64 == b.r2.u64 && a.r12.u64 == b.r12.u64 &&
            a.lr == b.lr && state_a == state_b && stack_a == stack_b && ext0 == ext1;
  const PPCRegister* ra[] = {&a.r13, &a.r14, &a.r15, &a.r16, &a.r17, &a.r18, &a.r19, &a.r20, &a.r21, &a.r22,
                             &a.r23, &a.r24, &a.r25, &a.r26, &a.r27, &a.r28, &a.r29, &a.r30, &a.r31};
  const PPCRegister* rb[] = {&b.r13, &b.r14, &b.r15, &b.r16, &b.r17, &b.r18, &b.r19, &b.r20, &b.r21, &b.r22,
                             &b.r23, &b.r24, &b.r25, &b.r26, &b.r27, &b.r28, &b.r29, &b.r30, &b.r31};
  for (size_t i = 0; i < sizeof ra / sizeof ra[0]; ++i) ok = ok && ra[i]->u64 == rb[i]->u64;
  if (!ok && g_fail++ < 10) {
    std::fprintf(stderr,
                 "MISMATCH [%s] call %llu s=%08X cur=%X: original r3=%llX ms=%08X | native r3=%llX ms=%08X "
                 "(state %s, stack %s, ext %s, lr %llX/%llX r12 %llX/%llX)\n",
                 g_case, (unsigned long long)g_calls, s, cur, (unsigned long long)a.r3.u64,
                 sc::LoadBE32(state_a.data() - s, s + sc::kMatchStart), (unsigned long long)b.r3.u64,
                 sc::LoadBE32(state_b.data() - s, s + sc::kMatchStart), state_a == state_b ? "same" : "DIFF",
                 stack_a == stack_b ? "same" : "DIFF", ext0 == ext1 ? "same" : "DIFF", (unsigned long long)a.lr,
                 (unsigned long long)b.lr, (unsigned long long)a.r12.u64, (unsigned long long)b.r12.u64);
  }
  return a.r3.u32;
}

// ---------------------------------------------------------------------------------------------------------
// zlib 1.2.3 deflate_slow / deflate_fast matching (no output stage) over guest-layout state.
struct Layout {
  uint32_t state, window, prev;
};

struct Params {
  int level, w_bits, mem_level, strategy;  // strategy 0 default, 1 filtered
};

struct Cfg {
  uint32_t good, lazy, nice, chain;
  bool slow;
};
const Cfg kTable[10] = {{0, 0, 0, 0, false},     {4, 4, 8, 4, false},     {4, 5, 16, 8, false},
                        {4, 6, 32, 32, false},   {4, 4, 16, 16, true},    {8, 16, 32, 32, true},
                        {8, 16, 128, 128, true}, {8, 32, 128, 256, true}, {32, 128, 258, 1024, true},
                        {32, 258, 258, 4096, true}};

void RunDeflate(const std::vector<uint8_t>& input, const Params& p, const Layout& L, const char* name) {
  g_case = name;
  const uint32_t w_bits = p.w_bits < 9 ? 9 : p.w_bits;
  const uint32_t w_size = 1u << w_bits, w_mask = w_size - 1;
  const uint32_t hash_bits = p.mem_level + 7, hash_size = 1u << hash_bits, hash_mask = hash_size - 1;
  const uint32_t hash_shift = (hash_bits + 2) / 3;
  const uint32_t window_size = 2 * w_size;
  const uint32_t MIN_LOOKAHEAD = 262, MAX_DIST = w_size - MIN_LOOKAHEAD, TOO_FAR = 4096;
  const Cfg c = kTable[p.level];
  std::vector<uint16_t> head(hash_size, 0);
  // Guest state fields read by the search.
  W32(L.state + sc::kWSize, w_size);
  W32(L.state + 48, w_bits);
  W32(L.state + sc::kWMask, w_mask);
  W32(L.state + sc::kWindow, L.window);
  W32(L.state + 60, window_size);
  W32(L.state + sc::kPrev, L.prev);
  W32(L.state + sc::kMaxChain, c.chain);
  W32(L.state + 128, c.lazy);
  W32(L.state + 132, p.level);
  W32(L.state + 136, p.strategy);
  W32(L.state + sc::kGoodMatch, c.good);
  W32(L.state + sc::kNiceMatch, c.nice);
  W32(L.state + sc::kMatchStart, 0);
  // zlib 1.2.3 does not clear the window; start from junk so reads past lookahead see stale data.
  for (uint32_t i = 0; i < window_size + 300; ++i) *H(L.window + i) = uint8_t(g_rng());
  for (uint32_t i = 0; i < w_size; ++i) {
    *H(L.prev + 2 * i) = 0;
    *H(L.prev + 2 * i + 1) = 0;
  }
  auto prev_at = [&](uint32_t i) -> uint16_t { return sc::LoadBE16(g_base, L.prev + 2 * i); };
  auto prev_set = [&](uint32_t i, uint16_t v) {
    *H(L.prev + 2 * i) = uint8_t(v >> 8);
    *H(L.prev + 2 * i + 1) = uint8_t(v);
  };
  auto win = [&](uint32_t i) -> uint8_t { return *H(L.window + i); };

  size_t in_pos = 0;
  uint32_t strstart = 0, lookahead = 0, ins_h = 0, match_length = 2, prev_length = 2, match_start = 0;
  bool match_available = false;
  auto update_hash = [&](uint32_t c) { ins_h = ((ins_h << hash_shift) ^ c) & hash_mask; };
  auto insert = [&](uint32_t str) -> uint32_t {
    update_hash(win(str + 2));
    const uint32_t mh = head[ins_h];
    prev_set(str & w_mask, uint16_t(mh));
    head[ins_h] = uint16_t(str);
    return mh;
  };
  auto fill_window = [&]() {
    do {
      uint32_t more = window_size - lookahead - strstart;
      if (strstart >= w_size + MAX_DIST) {
        for (uint32_t i = 0; i < w_size; ++i) *H(L.window + i) = win(w_size + i);
        match_start -= w_size;
        strstart -= w_size;
        for (auto& h : head) h = h >= w_size ? uint16_t(h - w_size) : 0;
        for (uint32_t i = 0; i < w_size; ++i) {
          const uint16_t m = prev_at(i);
          prev_set(i, m >= w_size ? uint16_t(m - w_size) : 0);
        }
        more += w_size;
      }
      if (in_pos >= input.size()) return;
      const size_t n = std::min<size_t>(more, input.size() - in_pos);
      for (size_t i = 0; i < n; ++i) *H(L.window + strstart + lookahead + uint32_t(i)) = input[in_pos + i];
      in_pos += n;
      lookahead += uint32_t(n);
      if (lookahead >= 3) {
        ins_h = win(strstart);
        update_hash(win(strstart + 1));
      }
    } while (lookahead < MIN_LOOKAHEAD && in_pos < input.size());
  };
  auto search = [&](uint32_t hash_head) -> uint32_t {
    W32(L.state + sc::kStrStart, strstart);
    W32(L.state + sc::kLookahead, lookahead);
    W32(L.state + sc::kPrevLength, prev_length);
    W32(L.state + sc::kMatchStart, match_start);
    const uint32_t ext_lo = L.window, ext_n = window_size + 300;
    // Checksum the window only now and then (it is large); the state + stack are compared on every call.
    const bool full = (g_calls % 997) == 0;
    const uint32_t r = CheckedCall(L.state, hash_head, ext_lo, full ? ext_n : 0);
    match_start = R32(L.state + sc::kMatchStart);
    return r;
  };

  for (;;) {
    if (lookahead < MIN_LOOKAHEAD) {
      fill_window();
      if (lookahead == 0) break;  // Z_FINISH
    }
    uint32_t hash_head = 0;
    if (lookahead >= 3) hash_head = insert(strstart);
    if (c.slow) {
      prev_length = match_length;
      match_length = 2;
      if (hash_head != 0 && prev_length < c.lazy && strstart - hash_head <= MAX_DIST) {
        match_length = search(hash_head);
        if (match_length <= 5 && (p.strategy == 1 || (match_length == 3 && strstart - match_start > TOO_FAR)))
          match_length = 2;
      }
      if (prev_length >= 3 && match_length <= prev_length) {
        const uint32_t max_insert = strstart + lookahead - 3;
        lookahead -= prev_length - 1;
        prev_length -= 2;
        do {
          if (++strstart <= max_insert) insert(strstart);
        } while (--prev_length != 0);
        match_available = false;
        match_length = 2;
        strstart++;
      } else {
        match_available = true;
        strstart++;
        lookahead--;
      }
    } else {
      // deflate_fast: prev_length stays MIN_MATCH-1.
      prev_length = 2;
      if (hash_head != 0 && strstart - hash_head <= MAX_DIST) match_length = search(hash_head);
      if (match_length >= 3) {
        lookahead -= match_length;
        if (match_length <= c.lazy && lookahead >= 3) {
          match_length--;
          do {
            strstart++;
            insert(strstart);
          } while (--match_length != 0);
          strstart++;
        } else {
          strstart += match_length;
          match_length = 0;
          ins_h = win(strstart);
          update_hash(win(strstart + 1));
        }
      } else {
        lookahead--;
        strstart++;
      }
    }
  }
  (void)match_available;
}

// ---------------------------------------------------------------------------------------------------------
std::vector<uint8_t> MakeInput(int kind, size_t n) {
  std::vector<uint8_t> v(n);
  switch (kind) {
    case 0: for (auto& b : v) b = uint8_t(g_rng()); break;                       // incompressible
    case 1: for (auto& b : v) b = uint8_t(g_rng() % 2); break;                   // 2 symbols
    case 2: for (auto& b : v) b = uint8_t(g_rng() % 4 + 'a'); break;             // 4 symbols
    case 3: std::fill(v.begin(), v.end(), 0); break;                             // all zero (max matches)
    case 4: { size_t per = 1 + g_rng() % 300; for (size_t i = 0; i < n; ++i) v[i] = uint8_t(i % per); } break;
    case 5: {                                                                    // runs
      size_t i = 0;
      while (i < n) { size_t r = 1 + g_rng() % 600; uint8_t b = uint8_t(g_rng() % 3); for (; r && i < n; --r) v[i++] = b; }
    } break;
    case 6: {                                                                    // text-like with repeats
      const char* words[] = {"the ", "hero ", "Bower ", "Lake ", "gold ", "renown ", "quest ", "\x00\x01", "dog "};
      size_t i = 0;
      while (i < n) { const char* w = words[g_rng() % 9]; for (size_t k = 0; w[k] && i < n; ++k) v[i++] = uint8_t(w[k]); if (g_rng() % 7 == 0 && i < n) v[i++] = uint8_t(g_rng()); }
    } break;
    case 7: {                                                                    // copies of earlier blocks at far distances
      for (size_t i = 0; i < n; ++i) v[i] = (i > 40000 && g_rng() % 4) ? v[i - 32000 - g_rng() % 700] : uint8_t(g_rng() % 16);
    } break;
    default: {                                                                   // near-258 matches
      for (size_t i = 0; i < n; ++i) v[i] = (i >= 258 && (i % 258) != (g_rng() % 260)) ? v[i - 258] : uint8_t(g_rng());
    } break;
  }
  return v;
}

const Layout kLayouts[] = {
    {0x40000000u, 0x40010000u, 0x40040000u},  // normal heap
    {0xE0100000u, 0xE0110000u, 0xE0140000u},  // physical range (+0x1000 host offset)
    {0x40000000u, 0xDFFF8000u, 0xDFFFC000u},  // window/prev straddle 0xE0000000: generic path
};

void Fuzz(int iters, bool wild) {
  g_case = wild ? "fuzz-wild" : "fuzz";
  std::uniform_int_distribution<uint32_t> d;
  const uint32_t s = 0x41000000u;
  for (int it = 0; it < iters; ++it) {
    // A window anywhere (also near the top, so addresses wrap), random fields, short chains.
    // Tame: 64 window slots (bounded memory), fast path mostly. Wild: anywhere, incl. the top of the address
    // space (wrap) and the 0xE0000000 step, any field values (generic path).
    const uint32_t window = !wild ? 0x42000000u + (d(g_rng) % 64) * 0x40000u
                            : (g_rng() % 3 == 0) ? (0xFFFFF000u + d(g_rng) % 0x1000)
                            : (g_rng() % 2) ? (0xDFFF0000u + d(g_rng) % 0x20000u) : d(g_rng);
    const uint32_t prev = 0x43000000u + d(g_rng) % 0x1000000u;
    const uint32_t w_size = (wild && g_rng() % 4 == 0) ? d(g_rng) % 600 : (1u << (9 + g_rng() % 7));
    const uint32_t wmask = (wild && g_rng() % 6 == 0) ? d(g_rng) : w_size - 1;
    const uint32_t strstart = (wild && g_rng() % 6 == 0) ? d(g_rng) : d(g_rng) % (2 * w_size + 1);
    const uint32_t lookahead = (g_rng() % 3 == 0) ? d(g_rng) % 300 : d(g_rng) % 70000;
    const uint32_t prev_length = (g_rng() % 4 == 0) ? d(g_rng) % 400 : (wild && g_rng() % 30 == 0 ? d(g_rng) : d(g_rng) % 259);
    const uint32_t chain = 1 + d(g_rng) % 64;
    const uint32_t nice = (g_rng() % 3 == 0) ? d(g_rng) : d(g_rng) % 300;
    const uint32_t good = d(g_rng) % 300;
    const uint32_t cur = (wild && g_rng() % 8 == 0) ? d(g_rng) : d(g_rng) % 0x10000u;
    W32(s + sc::kWSize, w_size);
    W32(s + sc::kWMask, wmask);
    W32(s + sc::kWindow, window);
    W32(s + sc::kPrev, prev);
    W32(s + sc::kStrStart, strstart);
    W32(s + sc::kLookahead, lookahead);
    W32(s + sc::kPrevLength, prev_length);
    W32(s + sc::kMaxChain, chain);
    W32(s + sc::kGoodMatch, good);
    W32(s + sc::kNiceMatch, nice);
    W32(s + sc::kMatchStart, d(g_rng));
    // Matchy bytes around scan and the first few candidates: a small alphabet so compares run long.
    const uint8_t alpha = uint8_t(1 + g_rng() % 3);
    for (uint32_t i = 0; i < 600; ++i) *H(window + strstart - 300 + i) = uint8_t(g_rng() % alpha);
    for (uint32_t k = 0; k < 8; ++k) {
      const uint32_t m = window + ((k == 0) ? cur : uint32_t(g_rng() % 0x10000u));
      for (uint32_t i = 0; i < 300; ++i) *H(m + i) = (g_rng() % 50) ? *H(window + strstart + i) : uint8_t(g_rng());
    }
    for (uint32_t k = 0; k < 64; ++k) {
      const uint32_t idx = (g_rng() % 2) ? (cur & wmask) : d(g_rng) % 0x10000u;
      const uint16_t v = uint16_t(g_rng());
      const uint32_t slot = idx & wmask & 0xFFFFu;  // bounded writes (wild masks then read zeros elsewhere)
      *H(prev + 2u * slot) = uint8_t(v >> 8);
      *H(prev + 2u * slot + 1) = uint8_t(v);
    }
    CheckedCall(s, cur, 0, 0);
  }
  // chain = 0 (wraps to 2^32 - 1 in both) with a chain that ends at once.
  W32(s + sc::kMaxChain, 0);
  W32(s + sc::kPrev, 0x44000000u);
  W32(s + sc::kWMask, 0xFF);
  for (uint32_t i = 0; i < 512; ++i) *H(0x44000000u + i) = 0;
  CheckedCall(s, 5, 0, 0);
}

void Replay(const std::string& dir) {
  int level = 6, w_bits = 15, hash_bits = 15, strategy = 0;
  if (FILE* f = std::fopen((dir + "/params.txt").c_str(), "r")) {
    char line[512] = {};
    if (std::fgets(line, sizeof line, f)) {
      unsigned st, ws, wb, hb, lv, sg;
      if (std::sscanf(line, "state %x w_size %u w_bits %u hash_bits %u level %u strategy %u", &st, &ws, &wb, &hb,
                      &lv, &sg) == 6) {
        level = int(lv), w_bits = int(wb), hash_bits = int(hb), strategy = int(sg);
      }
    }
    std::fclose(f);
  }
  std::printf("replay %s: level %d w_bits %d hash_bits %d strategy %d\n", dir.c_str(), level, w_bits, hash_bits, strategy);
  DIR* dp = opendir(dir.c_str());
  if (!dp) { std::fprintf(stderr, "cannot open %s\n", dir.c_str()); ++g_fail; return; }
  std::vector<std::string> files;
  while (dirent* e = readdir(dp)) {
    std::string n = e->d_name;
    if (n.size() > 7 && n.compare(n.size() - 7, 7, "-in.bin") == 0) files.push_back(n);
  }
  closedir(dp);
  std::sort(files.begin(), files.end());
  for (const auto& n : files) {
    std::vector<uint8_t> v;
    if (FILE* f = std::fopen((dir + "/" + n).c_str(), "rb")) {
      uint8_t buf[65536];
      size_t k;
      while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + k);
      std::fclose(f);
    }
    const uint64_t before = g_calls;
    for (const Layout& L : kLayouts) RunDeflate(v, {level, w_bits, hash_bits - 7, strategy}, L, n.c_str());
    std::printf("  %s: %zu bytes, %llu calls checked\n", n.c_str(), v.size(), (unsigned long long)(g_calls - before));
  }
}

}  // namespace

// Timed single-implementation driver (bench): same zlib driver, one implementation, wall time of the searches.
namespace {
double TimedDeflate(const std::vector<uint8_t>& input, bool native, uint64_t* calls_out, int level) {
  const Layout L = kLayouts[0];
  const uint32_t w_bits = 15, w_size = 1u << w_bits, w_mask = w_size - 1, hash_bits = 15;
  const uint32_t hash_size = 1u << hash_bits, hash_mask = hash_size - 1, hash_shift = (hash_bits + 2) / 3;
  const uint32_t window_size = 2 * w_size, MIN_LOOKAHEAD = 262, MAX_DIST = w_size - MIN_LOOKAHEAD;
  const Cfg c = kTable[level];
  std::vector<uint16_t> head(hash_size, 0);
  W32(L.state + sc::kWSize, w_size);
  W32(L.state + sc::kWMask, w_mask);
  W32(L.state + sc::kWindow, L.window);
  W32(L.state + sc::kPrev, L.prev);
  W32(L.state + sc::kMaxChain, c.chain);
  W32(L.state + sc::kGoodMatch, c.good);
  W32(L.state + sc::kNiceMatch, c.nice);
  for (uint32_t i = 0; i < w_size; ++i) { *H(L.prev + 2 * i) = 0; *H(L.prev + 2 * i + 1) = 0; }
  auto prev_at = [&](uint32_t i) -> uint16_t { return sc::LoadBE16(g_base, L.prev + 2 * i); };
  auto prev_set = [&](uint32_t i, uint16_t v) { *H(L.prev + 2 * i) = uint8_t(v >> 8); *H(L.prev + 2 * i + 1) = uint8_t(v); };
  auto win = [&](uint32_t i) -> uint8_t { return *H(L.window + i); };
  size_t in_pos = 0;
  uint32_t strstart = 0, lookahead = 0, ins_h = 0, match_length = 2, prev_length = 2, match_start = 0;
  auto update_hash = [&](uint32_t ch) { ins_h = ((ins_h << hash_shift) ^ ch) & hash_mask; };
  auto insert = [&](uint32_t str) -> uint32_t {
    update_hash(win(str + 2));
    const uint32_t mh = head[ins_h];
    prev_set(str & w_mask, uint16_t(mh));
    head[ins_h] = uint16_t(str);
    return mh;
  };
  double secs = 0;
  uint64_t calls = 0;
  PPCContext ctx{};
  for (;;) {
    if (lookahead < MIN_LOOKAHEAD) {
      do {
        uint32_t more = window_size - lookahead - strstart;
        if (strstart >= w_size + MAX_DIST) {
          for (uint32_t i = 0; i < w_size; ++i) *H(L.window + i) = win(w_size + i);
          match_start -= w_size;
          strstart -= w_size;
          for (auto& h : head) h = h >= w_size ? uint16_t(h - w_size) : 0;
          for (uint32_t i = 0; i < w_size; ++i) { const uint16_t m = prev_at(i); prev_set(i, m >= w_size ? uint16_t(m - w_size) : 0); }
          more += w_size;
        }
        if (in_pos >= input.size()) break;
        const size_t n = std::min<size_t>(more, input.size() - in_pos);
        std::memcpy(H(L.window + strstart + lookahead), input.data() + in_pos, n);
        in_pos += n;
        lookahead += uint32_t(n);
        if (lookahead >= 3) { ins_h = win(strstart); update_hash(win(strstart + 1)); }
      } while (lookahead < MIN_LOOKAHEAD && in_pos < input.size());
      if (lookahead == 0) break;
    }
    uint32_t hash_head = 0;
    if (lookahead >= 3) hash_head = insert(strstart);
    prev_length = match_length;
    match_length = 2;
    if (hash_head != 0 && prev_length < c.lazy && strstart - hash_head <= MAX_DIST) {
      W32(L.state + sc::kStrStart, strstart);
      W32(L.state + sc::kLookahead, lookahead);
      W32(L.state + sc::kPrevLength, prev_length);
      ctx.r1.u64 = kStack;
      ctx.r3.u64 = L.state;
      ctx.r4.u64 = hash_head;
      ctx.lr = 0x82B52E80u;
      const auto t0 = std::chrono::steady_clock::now();
      if (native) Native_82B52630(ctx, g_base);
      else Original_82B52630(ctx, g_base);
      secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      ++calls;
      match_length = ctx.r3.u32;
      match_start = R32(L.state + sc::kMatchStart);
      if (match_length <= 5 && match_length == 3 && strstart - match_start > 4096) match_length = 2;
    }
    if (prev_length >= 3 && match_length <= prev_length) {
      const uint32_t max_insert = strstart + lookahead - 3;
      lookahead -= prev_length - 1;
      prev_length -= 2;
      do { if (++strstart <= max_insert) insert(strstart); } while (--prev_length != 0);
      match_length = 2;
      strstart++;
    } else {
      strstart++;
      lookahead--;
    }
  }
  *calls_out = calls;
  return secs;
}
}  // namespace

int main(int argc, char** argv) {
  // 4 GB guest reservation + the physical-range host offset, untouched pages cost nothing.
  void* m = mmap(nullptr, 0x100000000ull + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) { std::perror("mmap"); return 1; }
  g_base = static_cast<uint8_t*>(m);
  bool bench = false, full = false;
  std::vector<std::string> dirs;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bench") == 0) bench = true;
    else if (std::strcmp(argv[i], "--full") == 0) full = true;
    else dirs.push_back(argv[i]);
  }
  const auto t_start = std::chrono::steady_clock::now();
  // The header's physical offset must be the pch's.
  for (uint32_t a : {0u, 0x82000000u, 0xDFFFFFFFu, 0xE0000000u, 0xFFFFFFFFu}) {
    if (sc::PhysHostOffset(a) != REX_PHYS_HOST_OFFSET(a)) { std::fprintf(stderr, "PhysHostOffset differs at %08X\n", a); return 1; }
  }

  // Generated inputs: every kind x layouts x a spread of levels / window bits / memLevels / sizes.
  // Default (build-time, a few seconds): the sizes up to 70000 and a 70000-byte pass per level. --full (the
  // docs' evidence run, minutes): up to 140000 and 200000-byte passes, 10x the fuzz.
  const size_t sizes[] = {0, 1, 2, 3, 4, 257, 258, 259, 262, 263, 1000, 33000, 70000, 140000};
  const size_t n_sizes = full ? sizeof sizes / sizeof sizes[0] : sizeof sizes / sizeof sizes[0] - 1;
  int run = 0;
  for (int kind = 0; kind < 9; ++kind) {
    for (size_t si = 0; si < n_sizes; ++si) {
      const size_t n = sizes[si];
      const auto input = MakeInput(kind, n);
      const int level = 1 + (run % 9);
      const int w_bits = (n > 30000) ? 9 + int(g_rng() % 7) : 15;
      const int mem = 1 + int(g_rng() % 9);
      const int strategy = (run % 11 == 0) ? 1 : 0;
      const Layout& L = kLayouts[run % 3];
      char name[64];
      std::snprintf(name, sizeof name, "kind%d n%zu L%d wb%d m%d s%d", kind, n, level, w_bits, mem, strategy);
      const auto t0 = std::chrono::steady_clock::now();
      RunDeflate(input, {level, w_bits, mem, strategy}, L, name);
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (secs > 1.0) std::printf("  slow case %s: %.1f s\n", name, secs);
      ++run;
    }
  }
  // The game's settings at every level, all three layouts, larger inputs.
  for (int level = 1; level <= 9; ++level) {
    for (int li = 0; li < 3; ++li) {
      const size_t n = full ? 200000 : 70000;
      const auto input = MakeInput(int(g_rng() % 9), n);
      char name[64];
      std::snprintf(name, sizeof name, "level%d layout%d %zu", level, li, n);
      RunDeflate(input, {level, 15, 8, 0}, kLayouts[li], name);
    }
  }
  const uint64_t generated_calls = g_calls;
  Fuzz(full ? 200000 : 20000, false);
  Fuzz(full ? 400 : 100, true);
  std::printf("save_compress_tests: %llu deflate searches + %llu fuzz states", (unsigned long long)generated_calls,
              (unsigned long long)(g_calls - generated_calls));
  for (const auto& d : dirs) {
    std::printf("\n");
    Replay(d);
  }
  std::printf("; %llu mismatches (%.1f s)\n", (unsigned long long)g_fail,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count());
  if (bench) {
    // Each captured buffer as its own stream at the game's level 9 (params.txt), else 4 MB of text-like data.
    std::vector<std::vector<uint8_t>> inputs;
    for (const auto& d : dirs) {
      DIR* dp = opendir(d.c_str());
      std::vector<std::string> names;
      while (dp) {
        dirent* e = readdir(dp);
        if (!e) break;
        std::string n = e->d_name;
        if (n.size() > 7 && n.compare(n.size() - 7, 7, "-in.bin") == 0) names.push_back(n);
      }
      if (dp) closedir(dp);
      std::sort(names.begin(), names.end());
      for (const auto& n : names) {
        std::vector<uint8_t> v;
        if (FILE* f = std::fopen((d + "/" + n).c_str(), "rb")) {
          uint8_t buf[65536];
          size_t k;
          while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + k);
          std::fclose(f);
        }
        inputs.push_back(std::move(v));
      }
    }
    if (inputs.empty()) inputs.push_back(MakeInput(6, 4 << 20));
    for (int level : {9, 6}) {
      double t_orig = 0, t_nat = 0;
      uint64_t c0 = 0, c1 = 0, bytes = 0;
      for (int rep = 0; rep < 3; ++rep) {
        double a = 0, b = 0;
        uint64_t x = 0, y = 0;
        for (const auto& in : inputs) {
          uint64_t k0 = 0, k1 = 0;
          a += TimedDeflate(in, false, &k0, level);
          b += TimedDeflate(in, true, &k1, level);
          x += k0;
          y += k1;
          if (rep == 0) bytes += in.size();
        }
        if (rep == 0 || a < t_orig) t_orig = a;
        if (rep == 0 || b < t_nat) t_nat = b;
        c0 = x;
        c1 = y;
      }
      std::printf("bench level %d (%zu streams, %llu bytes, best of 3): original %.1f ms, native %.1f ms "
                  "(%llu / %llu searches), %.2fx\n",
                  level, inputs.size(), (unsigned long long)bytes, t_orig * 1e3, t_nat * 1e3,
                  (unsigned long long)c0, (unsigned long long)c1, t_nat > 0 ? t_orig / t_nat : 0.0);
    }
  }
  return g_fail ? 1 : 0;
}
