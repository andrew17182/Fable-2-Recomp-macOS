// Hand-tuned override: opt-in game-logic fix for the texture streamer's per-model size cache.
// The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// FABLE2_FIX_LOD_SIZE_CACHE=1 (default off: it changes the original game's behaviour; launcher
// "Fix game texture-detail cache bug (pale textures)", docs/pale-root-20261014.md section 5).
//
// sub_82B197C8(model, f1 = this instance's distance, f2 = scale, r5 = view, ...) keeps on the
// model: +32 the previous frame's minimum distance, +36 this frame's running minimum, +40 the
// distance of the last size computation, +44 the frame. On the first call of a frame it compares
// THIS call's distance with +40 and reuses the cached per-material sizes when the ratio is within
// a tolerance of 1; otherwise it recomputes them from min(previous minimum, this distance). When the
// shadow (light) view draws a shared model first, at a constant ~405 away, and also made the first
// computation, the cache never recomputes and nearby instances keep the distant-LOD atlas (the
// pale root at Bower Lake, ~25% of loads, until the camera moves).
//
// The fix is one operand: the ratio test uses the same minimum distance (f30) the computation uses.
// LodSizeCacheFixed below is the recompiler's literal output for sub_82B197C8
// (generated/default/fable_2_recomp.112.cpp) with only that line changed. Off: __imp__ (original).
#include "fable_2_pch.h"

#include <cstdlib>

#include <rex/logging/macros.h>

extern "C" void __imp__sub_82B197C8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __savegprlr_24(PPCContext& ctx, uint8_t* base);
extern "C" void __restgprlr_24(PPCContext& ctx, uint8_t* base);
extern "C" void __savefpr_26(PPCContext& ctx, uint8_t* base);
extern "C" void __restfpr_26(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82A94648(PPCContext& __restrict ctx, uint8_t* base);
// Trace hook (src/core/hotfunc/gpu/PaleRootTrace.cpp, FABLE2_PALE_ROOT_TRACE=1).
bool fable2_pale_root_trace_on();
void fable2_pale_root_size_trace(PPCContext& __restrict ctx, uint8_t* base,
                                 void (*impl)(PPCContext&, uint8_t*));

namespace {

bool FixOn() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_FIX_LOD_SIZE_CACHE");  // getenv-ok: once
    const bool enabled = v && v[0] == '1';
    REXLOG_INFO("[lod-size-cache] game texture-detail cache fix {}", enabled ? "ON (changes original behaviour)" : "off (original)");
    return enabled;
  }();
  return on;
}

void LodSizeCacheFixed(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr1{};
	PPCCRRegister cr2{};
	PPCCRRegister cr3{};
	PPCCRRegister cr4{};
	PPCCRRegister cr5{};
	PPCCRRegister cr6{};
	PPCCRRegister cr7{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// bl 0x82ca2bd8
	ctx.lr = 0x82B197D0;
	__savegprlr_24(ctx, base);
	// addi r12,r1,-72
	ctx.r12.s64 = ctx.r1.s64 + -72;
	// bl 0x82ca7500
	ctx.lr = 0x82B197D8;
	__savefpr_26(ctx, base);
	// stwu r1,-224(r1)
	ea = -224 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lis r9,-31926
	ctx.r9.s64 = -2092302336;
	// fmr f26,f2
	ctx.fpscr.disableFlushMode();
	ctx.f26.f64 = ctx.f2.f64;
	// mr r28,r3
	ctx.r28.u64 = ctx.r3.u64;
	// lis r10,-32246
	ctx.r10.s64 = -2113273856;
	// addi r11,r28,32
	ctx.r11.s64 = ctx.r28.s64 + 32;
	// addi r10,r10,-27456
	ctx.r10.s64 = ctx.r10.s64 + -27456;
	// lwz r9,-2076(r9)
	ctx.r9.u64 = REX_LOAD_U32(ctx.r9.u32 + -2076);
	// mr r25,r8
	ctx.r25.u64 = ctx.r8.u64;
	// lwz r7,44(r28)
	ctx.r7.u64 = REX_LOAD_U32(ctx.r28.u32 + 44);
	// lis r8,-32240
	ctx.r8.s64 = -2112880640;
	// mr r24,r5
	ctx.r24.u64 = ctx.r5.u64;
	// lfs f0,4(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 4);
	ctx.f0.f64 = double(temp.f32);
	// mr r26,r6
	ctx.r26.u64 = ctx.r6.u64;
	// lfs f31,-12(r10)
	temp.u32 = REX_LOAD_U32(ctx.r10.u32 + -12);
	ctx.f31.f64 = double(temp.f32);
	// addi r27,r8,368
	ctx.r27.s64 = ctx.r8.s64 + 368;
	// lwz r9,1300(r9)
	ctx.r9.u64 = REX_LOAD_U32(ctx.r9.u32 + 1300);
	// cmpw cr6,r9,r7
	cr6.compare<int32_t>(ctx.r9.s32, ctx.r7.s32, xer);
	// beq cr6,0x82b19838
	if (cr6.eq) goto loc_82B19838;
	// stw r9,12(r11)
	REX_STORE_U32(ctx.r11.u32 + 12, ctx.r9.u32);
	// stfs f0,0(r11)
	temp.f32 = float(ctx.f0.f64);
	REX_STORE_U32(ctx.r11.u32 + 0, temp.u32);
	// stfs f1,4(r11)
	temp.f32 = float(ctx.f1.f64);
	REX_STORE_U32(ctx.r11.u32 + 4, temp.u32);
	// li r9,1
	ctx.r9.s64 = 1;
	// b 0x82b19860
	goto loc_82B19860;
loc_82B19838:
	// fsubs f13,f0,f1
	ctx.fpscr.disableFlushMode();
	ctx.f13.f64 = double(float(ctx.f0.f64 - ctx.f1.f64));
	// li r9,0
	ctx.r9.s64 = 0;
	// fcmpu cr6,f13,f31
	cr6.compare(ctx.f13.f64, ctx.f31.f64);
	// mfcr r8
	ctx.r8.u64 = cr0.lt ? 0x80000000 : 0;
	ctx.r8.u64 |= cr0.gt ? 0x40000000 : 0;
	ctx.r8.u64 |= cr0.eq ? 0x20000000 : 0;
	ctx.r8.u64 |= cr0.so ? 0x10000000 : 0;
	ctx.r8.u64 |= cr1.lt ? 0x8000000 : 0;
	ctx.r8.u64 |= cr1.gt ? 0x4000000 : 0;
	ctx.r8.u64 |= cr1.eq ? 0x2000000 : 0;
	ctx.r8.u64 |= cr1.so ? 0x1000000 : 0;
	ctx.r8.u64 |= cr2.lt ? 0x800000 : 0;
	ctx.r8.u64 |= cr2.gt ? 0x400000 : 0;
	ctx.r8.u64 |= cr2.eq ? 0x200000 : 0;
	ctx.r8.u64 |= cr2.so ? 0x100000 : 0;
	ctx.r8.u64 |= cr3.lt ? 0x80000 : 0;
	ctx.r8.u64 |= cr3.gt ? 0x40000 : 0;
	ctx.r8.u64 |= cr3.eq ? 0x20000 : 0;
	ctx.r8.u64 |= cr3.so ? 0x10000 : 0;
	ctx.r8.u64 |= cr4.lt ? 0x8000 : 0;
	ctx.r8.u64 |= cr4.gt ? 0x4000 : 0;
	ctx.r8.u64 |= cr4.eq ? 0x2000 : 0;
	ctx.r8.u64 |= cr4.so ? 0x1000 : 0;
	ctx.r8.u64 |= cr5.lt ? 0x800 : 0;
	ctx.r8.u64 |= cr5.gt ? 0x400 : 0;
	ctx.r8.u64 |= cr5.eq ? 0x200 : 0;
	ctx.r8.u64 |= cr5.so ? 0x100 : 0;
	ctx.r8.u64 |= cr6.lt ? 0x80 : 0;
	ctx.r8.u64 |= cr6.gt ? 0x40 : 0;
	ctx.r8.u64 |= cr6.eq ? 0x20 : 0;
	ctx.r8.u64 |= cr6.so ? 0x10 : 0;
	ctx.r8.u64 |= cr7.lt ? 0x8 : 0;
	ctx.r8.u64 |= cr7.gt ? 0x4 : 0;
	ctx.r8.u64 |= cr7.eq ? 0x2 : 0;
	ctx.r8.u64 |= cr7.so ? 0x1 : 0;
	// rlwinm r7,r8,27,29,29
	ctx.r7.u64 = __builtin_rotateleft64(ctx.r8.u32 | (ctx.r8.u64 << 32), 27) & 0x4;
	// rlwinm r6,r8,30,29,29
	ctx.r6.u64 = __builtin_rotateleft64(ctx.r8.u32 | (ctx.r8.u64 << 32), 30) & 0x4;
	// or r5,r7,r6
	ctx.r5.u64 = ctx.r7.u64 | ctx.r6.u64;
	// lfsx f12,r27,r5
	temp.u32 = REX_LOAD_U32(ctx.r27.u32 + ctx.r5.u32);
	ctx.f12.f64 = double(temp.f32);
	// fsel f11,f12,f1,f0
	ctx.f11.f64 = ctx.f12.f64 >= 0.0 ? ctx.f1.f64 : ctx.f0.f64;
	// stfs f11,4(r11)
	temp.f32 = float(ctx.f11.f64);
	REX_STORE_U32(ctx.r11.u32 + 4, temp.u32);
loc_82B19860:
	// clrlwi r9,r9,24
	ctx.r9.u64 = ctx.r9.u32 & 0xFF;
	// cmplwi cr6,r9,0
	cr6.compare<uint32_t>(ctx.r9.u32, 0, xer);
	// bne cr6,0x82b19880
	if (!cr6.eq) goto loc_82B19880;
	// lbz r3,48(r28)
	ctx.r3.u64 = REX_LOAD_U8(ctx.r28.u32 + 48);
	// addi r1,r1,224
	ctx.r1.s64 = ctx.r1.s64 + 224;
	// addi r12,r1,-72
	ctx.r12.s64 = ctx.r1.s64 + -72;
	// bl 0x82ca754c
	ctx.lr = 0x82B1987C;
	__restfpr_26(ctx, base);
	// b 0x82ca2c28
	__restgprlr_24(ctx, base);
	return;
loc_82B19880:
	// lfs f12,4(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 4);
	ctx.f12.f64 = double(temp.f32);
	// lwz r9,24(r28)
	ctx.r9.u64 = REX_LOAD_U32(ctx.r28.u32 + 24);
	// lfs f11,0(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 0);
	ctx.f11.f64 = double(temp.f32);
	// lwz r8,20(r28)
	ctx.r8.u64 = REX_LOAD_U32(ctx.r28.u32 + 20);
	// lfs f10,8(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 8);
	ctx.f10.f64 = double(temp.f32);
	// fsubs f9,f11,f12
	ctx.f9.f64 = double(float(ctx.f11.f64 - ctx.f12.f64));
	// lfs f0,24524(r10)
	temp.u32 = REX_LOAD_U32(ctx.r10.u32 + 24524);
	ctx.f0.f64 = double(temp.f32);
	// subf r6,r8,r9
	ctx.r6.u64 = ctx.r9.u64 - ctx.r8.u64;
	// fsubs f8,f10,f0
	ctx.f8.f64 = double(float(ctx.f10.f64 - ctx.f0.f64));
	// lfs f29,0(r10)
	temp.u32 = REX_LOAD_U32(ctx.r10.u32 + 0);
	ctx.f29.f64 = double(temp.f32);
	// fmr f7,f12
	ctx.f7.f64 = ctx.f12.f64;
	// lfs f13,-392(r10)
	temp.u32 = REX_LOAD_U32(ctx.r10.u32 + -392);
	ctx.f13.f64 = double(temp.f32);
	// li r7,180
	ctx.r7.s64 = 180;
	// li r31,1
	ctx.r31.s64 = 1;
	// divw r30,r6,r7
	ctx.r30.u64 = uint32_t((ctx.r7.s32 && !(ctx.r6.s32 == INT32_MIN && ctx.r7.s32 == -1)) ? ctx.r6.s32 / ctx.r7.s32 : 0);
	// fcmpu cr6,f9,f31
	cr6.compare(ctx.f9.f64, ctx.f31.f64);
	// mfcr r5
	ctx.r5.u64 = cr0.lt ? 0x80000000 : 0;
	ctx.r5.u64 |= cr0.gt ? 0x40000000 : 0;
	ctx.r5.u64 |= cr0.eq ? 0x20000000 : 0;
	ctx.r5.u64 |= cr0.so ? 0x10000000 : 0;
	ctx.r5.u64 |= cr1.lt ? 0x8000000 : 0;
	ctx.r5.u64 |= cr1.gt ? 0x4000000 : 0;
	ctx.r5.u64 |= cr1.eq ? 0x2000000 : 0;
	ctx.r5.u64 |= cr1.so ? 0x1000000 : 0;
	ctx.r5.u64 |= cr2.lt ? 0x800000 : 0;
	ctx.r5.u64 |= cr2.gt ? 0x400000 : 0;
	ctx.r5.u64 |= cr2.eq ? 0x200000 : 0;
	ctx.r5.u64 |= cr2.so ? 0x100000 : 0;
	ctx.r5.u64 |= cr3.lt ? 0x80000 : 0;
	ctx.r5.u64 |= cr3.gt ? 0x40000 : 0;
	ctx.r5.u64 |= cr3.eq ? 0x20000 : 0;
	ctx.r5.u64 |= cr3.so ? 0x10000 : 0;
	ctx.r5.u64 |= cr4.lt ? 0x8000 : 0;
	ctx.r5.u64 |= cr4.gt ? 0x4000 : 0;
	ctx.r5.u64 |= cr4.eq ? 0x2000 : 0;
	ctx.r5.u64 |= cr4.so ? 0x1000 : 0;
	ctx.r5.u64 |= cr5.lt ? 0x800 : 0;
	ctx.r5.u64 |= cr5.gt ? 0x400 : 0;
	ctx.r5.u64 |= cr5.eq ? 0x200 : 0;
	ctx.r5.u64 |= cr5.so ? 0x100 : 0;
	ctx.r5.u64 |= cr6.lt ? 0x80 : 0;
	ctx.r5.u64 |= cr6.gt ? 0x40 : 0;
	ctx.r5.u64 |= cr6.eq ? 0x20 : 0;
	ctx.r5.u64 |= cr6.so ? 0x10 : 0;
	ctx.r5.u64 |= cr7.lt ? 0x8 : 0;
	ctx.r5.u64 |= cr7.gt ? 0x4 : 0;
	ctx.r5.u64 |= cr7.eq ? 0x2 : 0;
	ctx.r5.u64 |= cr7.so ? 0x1 : 0;
	// fcmpu cr6,f8,f31
	cr6.compare(ctx.f8.f64, ctx.f31.f64);
	// mfcr r4
	ctx.r4.u64 = cr0.lt ? 0x80000000 : 0;
	ctx.r4.u64 |= cr0.gt ? 0x40000000 : 0;
	ctx.r4.u64 |= cr0.eq ? 0x20000000 : 0;
	ctx.r4.u64 |= cr0.so ? 0x10000000 : 0;
	ctx.r4.u64 |= cr1.lt ? 0x8000000 : 0;
	ctx.r4.u64 |= cr1.gt ? 0x4000000 : 0;
	ctx.r4.u64 |= cr1.eq ? 0x2000000 : 0;
	ctx.r4.u64 |= cr1.so ? 0x1000000 : 0;
	ctx.r4.u64 |= cr2.lt ? 0x800000 : 0;
	ctx.r4.u64 |= cr2.gt ? 0x400000 : 0;
	ctx.r4.u64 |= cr2.eq ? 0x200000 : 0;
	ctx.r4.u64 |= cr2.so ? 0x100000 : 0;
	ctx.r4.u64 |= cr3.lt ? 0x80000 : 0;
	ctx.r4.u64 |= cr3.gt ? 0x40000 : 0;
	ctx.r4.u64 |= cr3.eq ? 0x20000 : 0;
	ctx.r4.u64 |= cr3.so ? 0x10000 : 0;
	ctx.r4.u64 |= cr4.lt ? 0x8000 : 0;
	ctx.r4.u64 |= cr4.gt ? 0x4000 : 0;
	ctx.r4.u64 |= cr4.eq ? 0x2000 : 0;
	ctx.r4.u64 |= cr4.so ? 0x1000 : 0;
	ctx.r4.u64 |= cr5.lt ? 0x800 : 0;
	ctx.r4.u64 |= cr5.gt ? 0x400 : 0;
	ctx.r4.u64 |= cr5.eq ? 0x200 : 0;
	ctx.r4.u64 |= cr5.so ? 0x100 : 0;
	ctx.r4.u64 |= cr6.lt ? 0x80 : 0;
	ctx.r4.u64 |= cr6.gt ? 0x40 : 0;
	ctx.r4.u64 |= cr6.eq ? 0x20 : 0;
	ctx.r4.u64 |= cr6.so ? 0x10 : 0;
	ctx.r4.u64 |= cr7.lt ? 0x8 : 0;
	ctx.r4.u64 |= cr7.gt ? 0x4 : 0;
	ctx.r4.u64 |= cr7.eq ? 0x2 : 0;
	ctx.r4.u64 |= cr7.so ? 0x1 : 0;
	// rlwinm r11,r4,30,29,29
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r4.u32 | (ctx.r4.u64 << 32), 30) & 0x4;
	// rlwinm r3,r4,27,29,29
	ctx.r3.u64 = __builtin_rotateleft64(ctx.r4.u32 | (ctx.r4.u64 << 32), 27) & 0x4;
	// rlwinm r10,r5,27,29,29
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r5.u32 | (ctx.r5.u64 << 32), 27) & 0x4;
	// or r9,r3,r11
	ctx.r9.u64 = ctx.r3.u64 | ctx.r11.u64;
	// rlwinm r8,r5,30,29,29
	ctx.r8.u64 = __builtin_rotateleft64(ctx.r5.u32 | (ctx.r5.u64 << 32), 30) & 0x4;
	// li r11,1
	ctx.r11.s64 = 1;
	// or r7,r10,r8
	ctx.r7.u64 = ctx.r10.u64 | ctx.r8.u64;
	// lfsx f6,r27,r9
	temp.u32 = REX_LOAD_U32(ctx.r27.u32 + ctx.r9.u32);
	ctx.f6.f64 = double(temp.f32);
	// fsel f5,f6,f10,f0
	ctx.f5.f64 = ctx.f6.f64 >= 0.0 ? ctx.f10.f64 : ctx.f0.f64;
	// lfsx f4,r27,r7
	temp.u32 = REX_LOAD_U32(ctx.r27.u32 + ctx.r7.u32);
	ctx.f4.f64 = double(temp.f32);
	// fsel f30,f4,f12,f11
	ctx.f30.f64 = ctx.f4.f64 >= 0.0 ? ctx.f12.f64 : ctx.f11.f64;
	// fdivs f3,f7,f5   -- PATCHED (FABLE2_FIX_LOD_SIZE_CACHE): fdivs f3,f30,f5
	// The original divides this call's distance (f7 = f12) by the distance of the last size
	// computation; the sizes are computed from f30 = min(previous frame's minimum, this distance).
	// Test the same distance the computation uses.
	ctx.f3.f64 = double(float(ctx.f30.f64 / ctx.f5.f64));
	// fsubs f2,f3,f29
	ctx.f2.f64 = double(float(ctx.f3.f64 - ctx.f29.f64));
	// fabs f1,f2
	ctx.f1.u64 = ctx.f2.u64 & ~0x8000000000000000;
	// fcmpu cr6,f1,f13
	cr6.compare(ctx.f1.f64, ctx.f13.f64);
	// blt cr6,0x82b19910
	if (cr6.lt) goto loc_82B19910;
	// li r11,0
	ctx.r11.s64 = 0;
loc_82B19910:
	// clrlwi r11,r11,24
	ctx.r11.u64 = ctx.r11.u32 & 0xFF;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(ctx.r11.u32, 0, xer);
	// beq cr6,0x82b1996c
	if (cr6.eq) goto loc_82B1996C;
	// cmpwi cr6,r30,0
	cr6.compare<int32_t>(ctx.r30.s32, 0, xer);
	// ble cr6,0x82b19a24
	if (!cr6.gt) goto loc_82B19A24;
	// li r29,0
	ctx.r29.s64 = 0;
loc_82B19928:
	// lwz r11,20(r28)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r28.u32 + 20);
	// mr r6,r25
	ctx.r6.u64 = ctx.r25.u64;
	// mr r5,r26
	ctx.r5.u64 = ctx.r26.u64;
	// add r3,r29,r11
	ctx.r3.u64 = ctx.r29.u64 + ctx.r11.u64;
	// addi r4,r1,80
	ctx.r4.s64 = ctx.r1.s64 + 80;
	// lwz r11,168(r3)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r3.u32 + 168);
	// lwz r10,172(r3)
	ctx.r10.u64 = REX_LOAD_U32(ctx.r3.u32 + 172);
	// stw r11,80(r1)
	REX_STORE_U32(ctx.r1.u32 + 80, ctx.r11.u32);
	// stw r10,84(r1)
	REX_STORE_U32(ctx.r1.u32 + 84, ctx.r10.u32);
	// bl 0x82a94648
	ctx.lr = 0x82B19950;
	sub_82A94648(ctx, base);
	// clrlwi r9,r3,24
	ctx.r9.u64 = ctx.r3.u32 & 0xFF;
	// clrlwi r8,r31,24
	ctx.r8.u64 = ctx.r31.u32 & 0xFF;
	// addic. r30,r30,-1
	xer.ca = ctx.r30.u32 > 0;
	ctx.r30.s64 = ctx.r30.s64 + -1;
	cr0.compare<int32_t>(ctx.r30.s32, 0, xer);
	// and r31,r9,r8
	ctx.r31.u64 = ctx.r9.u64 & ctx.r8.u64;
	// addi r29,r29,180
	ctx.r29.s64 = ctx.r29.s64 + 180;
	// bne 0x82b19928
	if (!cr0.eq) goto loc_82B19928;
	// b 0x82b19a24
	goto loc_82B19A24;
loc_82B1996C:
	// cmpwi cr6,r30,0
	cr6.compare<int32_t>(ctx.r30.s32, 0, xer);
	// ble cr6,0x82b19a20
	if (!cr6.gt) goto loc_82B19A20;
	// lis r11,-32246
	ctx.r11.s64 = -2113273856;
	// li r29,0
	ctx.r29.s64 = 0;
	// lfs f28,-25888(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -25888);
	ctx.f28.f64 = double(temp.f32);
	// fsubs f27,f30,f28
	ctx.f27.f64 = double(float(ctx.f30.f64 - ctx.f28.f64));
loc_82B19984:
	// fcmpu cr6,f27,f31
	ctx.fpscr.disableFlushMode();
	cr6.compare(ctx.f27.f64, ctx.f31.f64);
	// mfcr r11
	ctx.r11.u64 = cr0.lt ? 0x80000000 : 0;
	ctx.r11.u64 |= cr0.gt ? 0x40000000 : 0;
	ctx.r11.u64 |= cr0.eq ? 0x20000000 : 0;
	ctx.r11.u64 |= cr0.so ? 0x10000000 : 0;
	ctx.r11.u64 |= cr1.lt ? 0x8000000 : 0;
	ctx.r11.u64 |= cr1.gt ? 0x4000000 : 0;
	ctx.r11.u64 |= cr1.eq ? 0x2000000 : 0;
	ctx.r11.u64 |= cr1.so ? 0x1000000 : 0;
	ctx.r11.u64 |= cr2.lt ? 0x800000 : 0;
	ctx.r11.u64 |= cr2.gt ? 0x400000 : 0;
	ctx.r11.u64 |= cr2.eq ? 0x200000 : 0;
	ctx.r11.u64 |= cr2.so ? 0x100000 : 0;
	ctx.r11.u64 |= cr3.lt ? 0x80000 : 0;
	ctx.r11.u64 |= cr3.gt ? 0x40000 : 0;
	ctx.r11.u64 |= cr3.eq ? 0x20000 : 0;
	ctx.r11.u64 |= cr3.so ? 0x10000 : 0;
	ctx.r11.u64 |= cr4.lt ? 0x8000 : 0;
	ctx.r11.u64 |= cr4.gt ? 0x4000 : 0;
	ctx.r11.u64 |= cr4.eq ? 0x2000 : 0;
	ctx.r11.u64 |= cr4.so ? 0x1000 : 0;
	ctx.r11.u64 |= cr5.lt ? 0x800 : 0;
	ctx.r11.u64 |= cr5.gt ? 0x400 : 0;
	ctx.r11.u64 |= cr5.eq ? 0x200 : 0;
	ctx.r11.u64 |= cr5.so ? 0x100 : 0;
	ctx.r11.u64 |= cr6.lt ? 0x80 : 0;
	ctx.r11.u64 |= cr6.gt ? 0x40 : 0;
	ctx.r11.u64 |= cr6.eq ? 0x20 : 0;
	ctx.r11.u64 |= cr6.so ? 0x10 : 0;
	ctx.r11.u64 |= cr7.lt ? 0x8 : 0;
	ctx.r11.u64 |= cr7.gt ? 0x4 : 0;
	ctx.r11.u64 |= cr7.eq ? 0x2 : 0;
	ctx.r11.u64 |= cr7.so ? 0x1 : 0;
	// rlwinm r10,r11,27,29,29
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 27) & 0x4;
	// lwz r9,4(r24)
	ctx.r9.u64 = REX_LOAD_U32(ctx.r24.u32 + 4);
	// rlwinm r8,r11,30,29,29
	ctx.r8.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 30) & 0x4;
	// lwz r11,20(r28)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r28.u32 + 20);
	// mr r6,r25
	ctx.r6.u64 = ctx.r25.u64;
	// add r3,r29,r11
	ctx.r3.u64 = ctx.r29.u64 + ctx.r11.u64;
	// or r7,r10,r8
	ctx.r7.u64 = ctx.r10.u64 | ctx.r8.u64;
	// lfs f0,192(r9)
	temp.u32 = REX_LOAD_U32(ctx.r9.u32 + 192);
	ctx.f0.f64 = double(temp.f32);
	// mr r5,r26
	ctx.r5.u64 = ctx.r26.u64;
	// fmuls f13,f0,f26
	ctx.f13.f64 = double(float(ctx.f0.f64 * ctx.f26.f64));
	// addi r4,r1,88
	ctx.r4.s64 = ctx.r1.s64 + 88;
	// ld r11,156(r3)
	ctx.r11.u64 = REX_LOAD_U64(ctx.r3.u32 + 156);
	// lfsx f12,r27,r7
	temp.u32 = REX_LOAD_U32(ctx.r27.u32 + ctx.r7.u32);
	ctx.f12.f64 = double(temp.f32);
	// fsel f11,f12,f30,f28
	ctx.f11.f64 = ctx.f12.f64 >= 0.0 ? ctx.f30.f64 : ctx.f28.f64;
	// std r11,80(r1)
	REX_STORE_U64(ctx.r1.u32 + 80, ctx.r11.u64);
	// lfs f9,84(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 84);
	ctx.f9.f64 = double(temp.f32);
	// lfs f10,80(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 80);
	ctx.f10.f64 = double(temp.f32);
	// fmuls f8,f10,f13
	ctx.f8.f64 = double(float(ctx.f10.f64 * ctx.f13.f64));
	// fdivs f7,f29,f11
	ctx.f7.f64 = double(float(ctx.f29.f64 / ctx.f11.f64));
	// fmuls f6,f9,f13
	ctx.f6.f64 = double(float(ctx.f9.f64 * ctx.f13.f64));
	// fmuls f5,f8,f7
	ctx.f5.f64 = double(float(ctx.f8.f64 * ctx.f7.f64));
	// fmuls f4,f6,f7
	ctx.f4.f64 = double(float(ctx.f6.f64 * ctx.f7.f64));
	// fctiwz f3,f5
	ctx.f3.s64 = std::isnan(ctx.f5.f64) ? int64_t(0x80000000U) : (ctx.f5.f64 >= double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&ctx.f5.f64));
	// stfd f3,80(r1)
	REX_STORE_U64(ctx.r1.u32 + 80, ctx.f3.u64);
	// lwz r10,84(r1)
	ctx.r10.u64 = REX_LOAD_U32(ctx.r1.u32 + 84);
	// fctiwz f2,f4
	ctx.f2.s64 = std::isnan(ctx.f4.f64) ? int64_t(0x80000000U) : (ctx.f4.f64 >= double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&ctx.f4.f64));
	// stfd f2,80(r1)
	REX_STORE_U64(ctx.r1.u32 + 80, ctx.f2.u64);
	// lwz r9,84(r1)
	ctx.r9.u64 = REX_LOAD_U32(ctx.r1.u32 + 84);
	// stw r9,92(r1)
	REX_STORE_U32(ctx.r1.u32 + 92, ctx.r9.u32);
	// stw r10,88(r1)
	REX_STORE_U32(ctx.r1.u32 + 88, ctx.r10.u32);
	// bl 0x82a94648
	ctx.lr = 0x82B19A08;
	sub_82A94648(ctx, base);
	// clrlwi r8,r3,24
	ctx.r8.u64 = ctx.r3.u32 & 0xFF;
	// clrlwi r7,r31,24
	ctx.r7.u64 = ctx.r31.u32 & 0xFF;
	// addic. r30,r30,-1
	xer.ca = ctx.r30.u32 > 0;
	ctx.r30.s64 = ctx.r30.s64 + -1;
	cr0.compare<int32_t>(ctx.r30.s32, 0, xer);
	// and r31,r8,r7
	ctx.r31.u64 = ctx.r8.u64 & ctx.r7.u64;
	// addi r29,r29,180
	ctx.r29.s64 = ctx.r29.s64 + 180;
	// bne 0x82b19984
	if (!cr0.eq) goto loc_82B19984;
loc_82B19A20:
	// stfs f30,40(r28)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(ctx.f30.f64);
	REX_STORE_U32(ctx.r28.u32 + 40, temp.u32);
loc_82B19A24:
	// stb r31,48(r28)
	REX_STORE_U8(ctx.r28.u32 + 48, ctx.r31.u8);
	// mr r3,r31
	ctx.r3.u64 = ctx.r31.u64;
	// addi r1,r1,224
	ctx.r1.s64 = ctx.r1.s64 + 224;
	// addi r12,r1,-72
	ctx.r12.s64 = ctx.r1.s64 + -72;
	// bl 0x82ca754c
	ctx.lr = 0x82B19A38;
	__restfpr_26(ctx, base);
	// b 0x82ca2c28
	__restgprlr_24(ctx, base);
	return;
}


}  // namespace

extern "C" void sub_82B197C8(PPCContext& __restrict ctx, uint8_t* base) {
  void (*impl)(PPCContext&, uint8_t*) = FixOn() ? &LodSizeCacheFixed : &__imp__sub_82B197C8;
  if (fable2_pale_root_trace_on()) {
    fable2_pale_root_size_trace(ctx, base, impl);
    return;
  }
  impl(ctx, base);
}
