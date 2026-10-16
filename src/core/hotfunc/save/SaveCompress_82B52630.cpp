// Hand-tuned override: native longest-match search of the save compressor (zlib 1.2.3 longest_match,
// sub_82B52630). The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// FABLE2_NATIVE_SAVE_COMPRESS (launcher "Native save compressor", Performance > Startup and CPU):
//   unset / 1 : native (default). Same return value, same match_start write, same stack save-area bytes and the
//               same r12/lr/r23-r31 on return as the recompiled copy; only volatile r4-r11 differ (dead under
//               the PPC ABI; the two callers read r3 only). docs/load-fixes-20261016.md has the byte-identical proof.
//   0         : the recompiled function (__imp__sub_82B52630).
//   verify    : runs the recompiled function for the game AND the native one beside it, compares the return value
//               and match_start on every call, logs [save-compress] verify totals after each compress call and the
//               first mismatches in full. Slow (both run); for validation only.
// FABLE2_SAVE_COMPRESS_CAPTURE=1 (or a folder): the compress wrapper sub_82B49CB8 (r5/r6 input, r7/r8 output,
// one independent zlib stream per call) writes each call's input and output to
// <folder or $FABLE2_TEST_CONTROL_DIR/save-compress>/NNNN-in.bin / NNNN-out.bin plus params.txt (the
// deflate_state settings), so arms can be compared byte for byte and tests/save_compress_tests.cpp can replay
// real save inputs. Captured files are game data: never commit them.
#include "fable_2_pch.h"

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include <rex/logging/macros.h>

#include "native_longest_match.h"

extern "C" void __imp__sub_82B52630(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82B49CB8(PPCContext& __restrict ctx, uint8_t* base);

namespace {

namespace sc = fable2::savecompress;

enum class Mode { kOriginal, kNative, kVerify };

Mode GetMode() {
  static const Mode mode = [] {
    const char* v = std::getenv("FABLE2_NATIVE_SAVE_COMPRESS");  // getenv-ok: once
    Mode m = Mode::kNative;
    if (v && v[0] == '0') m = Mode::kOriginal;
    else if (v && (v[0] == 'v' || v[0] == 'V')) m = Mode::kVerify;
    REXLOG_INFO("[save-compress] longest-match search: {}",
                m == Mode::kNative ? "native (FABLE2_NATIVE_SAVE_COMPRESS=0 restores the recompiled path)"
                : m == Mode::kOriginal ? "recompiled (FABLE2_NATIVE_SAVE_COMPRESS=0)"
                                       : "VERIFY (recompiled for the game, native compared on every call)");
    return m;
  }();
  return mode;
}

std::string CaptureDir() {
  static const std::string dir = [] {
    const char* v = std::getenv("FABLE2_SAVE_COMPRESS_CAPTURE");  // getenv-ok: once
    if (!v || !v[0] || v[0] == '0') return std::string();
    std::string d;
    if (v[0] == '1' && !v[1]) {
      const char* tc = std::getenv("FABLE2_TEST_CONTROL_DIR");  // getenv-ok: once
      if (!tc || !tc[0]) {
        REXLOG_WARN("[save-compress] FABLE2_SAVE_COMPRESS_CAPTURE=1 needs FABLE2_TEST_CONTROL_DIR; capture off");
        return std::string();
      }
      d = std::string(tc) + "/save-compress";
    } else {
      d = v;
    }
    mkdir(d.c_str(), 0755);
    REXLOG_INFO("[save-compress] capturing compress inputs/outputs to {}", d);
    return d;
  }();
  return dir;
}

struct VerifyStats {
  std::atomic<uint64_t> calls{0}, mismatches{0}, fast{0}, stored{0};
};
VerifyStats g_verify;
std::atomic<uint64_t> g_native_calls{0};

// Guest prologue/epilogue side effects of the original (bl __savegprlr_23 / b __restgprlr_23): r12 = lr and
// r23..r31 + r12 are stored below r1; on return r23..r31 are reloaded (unchanged) and lr = r12 = (u32)lr.
inline void GuestSaveArea(PPCContext& ctx, uint8_t* base) {
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
  ctx.r12.u64 = ctx.r12.u32;  // __restgprlr_23: lwz r12,-8(r1); mtlr r12
  ctx.lr = ctx.r12.u64;
}

void Verify(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32;
  const uint32_t cur = ctx.r4.u32;
  const uint32_t ms_before = sc::LoadBE32(base, s + sc::kMatchStart);
  const sc::LongestMatchResult n = sc::NativeLongestMatch(base, s, cur, /*write=*/false);
  __imp__sub_82B52630(ctx, base);
  const uint32_t ms_after = sc::LoadBE32(base, s + sc::kMatchStart);
  const uint32_t expect_ms = n.stored ? n.match_start : ms_before;
  const uint64_t call = g_verify.calls.fetch_add(1) + 1;
  if (n.fast) g_verify.fast.fetch_add(1);
  if (n.stored) g_verify.stored.fetch_add(1);
  if (ctx.r3.u64 != uint64_t(n.ret) || ms_after != expect_ms) {
    const uint64_t k = g_verify.mismatches.fetch_add(1);
    if (k < 8) {
      REXLOG_WARN("[save-compress] VERIFY MISMATCH call {}: s={:08X} cur={:X} original r3={:X} match_start={:X} | "
                  "native ret={:X} stored={} match_start={:X} fast={}",
                  call, s, cur, ctx.r3.u64, ms_after, n.ret, n.stored, expect_ms, n.fast);
    }
  }
}

void WriteFile(const std::string& path, const uint8_t* p, size_t n) {
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    if (n) std::fwrite(p, 1, n, f);
    std::fclose(f);
  }
}

}  // namespace

extern "C" void sub_82B52630(PPCContext& __restrict ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const Mode mode = GetMode();
  if (mode == Mode::kOriginal) {
    __imp__sub_82B52630(ctx, base);
    return;
  }
  if (mode == Mode::kVerify) {
    Verify(ctx, base);
    return;
  }
  GuestSaveArea(ctx, base);
  const sc::LongestMatchResult r = sc::NativeLongestMatch(base, ctx.r3.u32, ctx.r4.u32, /*write=*/true);
  ctx.r3.u64 = r.ret;
  g_native_calls.fetch_add(1, std::memory_order_relaxed);
}

// The save compressor's per-buffer entry (r3 = result {consumed, produced}, r4 = compressor, r5/r6 = input,
// r7/r8 = output). Timing + verify totals + optional capture; the call itself is always the original.
extern "C" void sub_82B49CB8(PPCContext& __restrict ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  static std::atomic<uint32_t> index{0};
  static std::mutex mu;
  const uint32_t result = ctx.r3.u32, compressor = ctx.r4.u32;
  const uint32_t in = ctx.r5.u32, in_len = ctx.r6.u32, out = ctx.r7.u32;
  const std::string dir = CaptureDir();
  const uint64_t native_before = g_native_calls.load(std::memory_order_relaxed);
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_82B49CB8(ctx, base);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const uint32_t consumed = REX_LOAD_U32(result + 0), produced = REX_LOAD_U32(result + 4);
  const uint32_t n = index.fetch_add(1);
  const Mode mode = GetMode();
  REXLOG_INFO("[save-compress] #{} {} -> {} bytes (consumed {}) in {:.2f} ms, search {}{}", n, in_len, produced,
              consumed, ms,
              mode == Mode::kNative ? "native" : mode == Mode::kOriginal ? "recompiled" : "verify",
              mode == Mode::kNative ? fmt::format(" ({} native calls)", g_native_calls.load() - native_before) : "");
  if (mode == Mode::kVerify) {
    REXLOG_INFO("[save-compress] verify totals: {} calls, {} mismatches, {} fast path, {} with a better match",
                g_verify.calls.load(), g_verify.mismatches.load(), g_verify.fast.load(), g_verify.stored.load());
  }
  if (!dir.empty()) {
    std::lock_guard<std::mutex> lock(mu);
    char name[32];
    std::snprintf(name, sizeof name, "/%04u", n);
    WriteFile(dir + name + "-in.bin", sc::GuestPtr(base, in), consumed <= in_len ? consumed : in_len);
    WriteFile(dir + name + "-out.bin", sc::GuestPtr(base, out), produced);
    if (n == 0) {
      // deflate_state of the z_stream at compressor+8 (z_stream.state = +28).
      const uint32_t s = REX_LOAD_U32(compressor + 8 + 28);
      if (FILE* f = std::fopen((dir + "/params.txt").c_str(), "w")) {
        std::fprintf(f, "state %08X w_size %u w_bits %u hash_bits %u level %u strategy %u good %u lazy %u nice %u "
                        "chain %u\n",
                     s, REX_LOAD_U32(s + 44), REX_LOAD_U32(s + 48), REX_LOAD_U32(s + 80), REX_LOAD_U32(s + 132),
                     REX_LOAD_U32(s + 136), REX_LOAD_U32(s + 140), REX_LOAD_U32(s + 128), REX_LOAD_U32(s + 144),
                     REX_LOAD_U32(s + 124));
        std::fclose(f);
      }
    }
  }
}
