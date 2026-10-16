// Hand-tuned override: native zlib 1.2.3 inflate_fast of the game's loader (ProcessStructFields_82B59D28).
// The "// Hand-tuned override" first line makes tools/make_hotfunc_overrides.py skip this file.
//
// The game decompresses its streamed data with its own zlib 1.2.3; inflate_fast is most of that CPU (level-load
// call: the VFS finaliser; the world-entry request: the 3D Engine). docs/load-fixes-20261016.md part G.
//
// FABLE2_NATIVE_INFLATE (launcher "Native decompression", Performance > Startup and CPU):
//   unset / 1 : native (default). native_inflate_fast.h transcribes the recompiled function: same guest writes in
//               the same order, same r4-r11/r12/lr/r16-r31 on return. Calls whose state the fast path does not
//               cover (Eligible() false) run the recompiled function.
//   0         : the recompiled function (__imp__ProcessStructFields_82B59D28).
//   verify    : every eligible call runs the native function, records what it wrote and its registers, puts guest
//               memory back, runs the recompiled function for the game and compares (save area, z_stream, state
//               header, the whole output buffer, registers). Logs [inflate] verify totals. Slow; validation only.
// FABLE2_INFLATE_STATS=1: per thread, a [inflate] line every 0.25 s while it inflates (calls, bytes out, ms).
#include "fable_2_pch.h"

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/logging/macros.h>

#include "native_inflate_fast.h"

extern "C" void __imp__ProcessStructFields_82B59D28(PPCContext& __restrict ctx, uint8_t* base);

namespace {

namespace inf = fable2::inflatefast;

enum class Mode { kOriginal, kNative, kVerify };

Mode GetMode() {
  static const Mode mode = [] {
    const char* v = std::getenv("FABLE2_NATIVE_INFLATE");  // getenv-ok: once
    Mode m = Mode::kNative;
    if (v && v[0] == '0') m = Mode::kOriginal;
    else if (v && (v[0] == 'v' || v[0] == 'V')) m = Mode::kVerify;
    REXLOG_INFO("[inflate] zlib inflate_fast: {}",
                m == Mode::kNative ? "native (FABLE2_NATIVE_INFLATE=0 restores the recompiled function)"
                : m == Mode::kOriginal ? "recompiled (FABLE2_NATIVE_INFLATE=0)"
                                       : "VERIFY (native compared with the recompiled function on every call)");
    return m;
  }();
  return mode;
}

bool StatsOn() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_INFLATE_STATS");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  return on;
}

struct ThreadStats {
  uint64_t calls = 0, native = 0, out_bytes = 0, ns = 0;
  std::chrono::steady_clock::time_point last_log{};
  bool started = false;
};
thread_local ThreadStats t_stats;

struct VerifyTotals {
  std::atomic<uint64_t> calls{0}, mismatches{0}, ineligible{0}, bytes{0};
};
VerifyTotals g_verify;

void LogStats(bool force) {
  ThreadStats& s = t_stats;
  const auto now = std::chrono::steady_clock::now();
  if (!s.started) {
    s.started = true;
    s.last_log = now;
  }
  if (!force && now - s.last_log < std::chrono::milliseconds(250)) return;
  if (s.calls) {
    char name[64] = {};
    pthread_getname_np(pthread_self(), name, sizeof name);
    REXLOG_INFO("[inflate] {}: {} calls ({} native), {} bytes out, {:.2f} ms", name, s.calls, s.native, s.out_bytes,
                s.ns / 1e6);
  }
  s = ThreadStats{};
  s.started = true;
  s.last_log = now;
}

// Guest ranges the function writes: the stack save area, the z_stream, the state header, the output buffer.
struct Snapshot {
  struct Range {
    uint32_t addr, len;
    std::vector<uint8_t> bytes;
  };
  std::vector<Range> ranges;
  void Add(uint8_t* base, uint32_t addr, uint32_t len) {
    Range r{addr, len, std::vector<uint8_t>(len)};
    if (inf::OneRegion(addr, len)) {
      if (len) std::memcpy(r.bytes.data(), inf::Host(base, addr), len);
    } else {
      for (uint32_t i = 0; i < len; ++i) r.bytes[i] = *inf::Host(base, addr + i);
    }
    ranges.push_back(std::move(r));
  }
  void Restore(uint8_t* base) const {
    for (const Range& r : ranges) {
      if (inf::OneRegion(r.addr, r.len)) {
        if (r.len) std::memcpy(inf::Host(base, r.addr), r.bytes.data(), r.len);
      } else {
        for (uint32_t i = 0; i < r.len; ++i) *inf::Host(base, r.addr + i) = r.bytes[i];
      }
    }
  }
};

void Verify(PPCContext& ctx, uint8_t* base) {
  const uint32_t strm = ctx.r3.u32;
  const uint32_t state = inf::Ld32(base, strm + 28);
  const uint32_t next_out = inf::Ld32(base, strm + 12), avail_out = inf::Ld32(base, strm + 16);
  Snapshot before;
  before.Add(base, ctx.r1.u32 - 136, 136);
  before.Add(base, strm, 56);
  before.Add(base, state, 92);
  before.Add(base, next_out, avail_out);
  PPCContext nctx = ctx;
  inf::RunCtx(nctx, base);
  inf::Regs n{};
  n.r4 = nctx.r4.u64, n.r5 = nctx.r5.u64, n.r6 = nctx.r6.u64, n.r7 = nctx.r7.u64, n.r8 = nctx.r8.u64;
  n.r9 = nctx.r9.u64, n.r10 = nctx.r10.u64, n.r11 = nctx.r11.u64, n.r12 = nctx.r12.u64, n.lr = nctx.lr;
  const uint64_t* ngp[16] = {&nctx.r16.u64, &nctx.r17.u64, &nctx.r18.u64, &nctx.r19.u64, &nctx.r20.u64,
                             &nctx.r21.u64, &nctx.r22.u64, &nctx.r23.u64, &nctx.r24.u64, &nctx.r25.u64,
                             &nctx.r26.u64, &nctx.r27.u64, &nctx.r28.u64, &nctx.r29.u64, &nctx.r30.u64,
                             &nctx.r31.u64};
  for (int i = 0; i < 16; ++i) n.r16_31[i] = *ngp[i];
  Snapshot after_native;
  for (const auto& r : before.ranges) after_native.Add(base, r.addr, r.len);
  before.Restore(base);
  __imp__ProcessStructFields_82B59D28(ctx, base);
  inf::Regs o{};
  o.r4 = ctx.r4.u64, o.r5 = ctx.r5.u64, o.r6 = ctx.r6.u64, o.r7 = ctx.r7.u64, o.r8 = ctx.r8.u64;
  o.r9 = ctx.r9.u64, o.r10 = ctx.r10.u64, o.r11 = ctx.r11.u64, o.r12 = ctx.r12.u64, o.lr = ctx.lr;
  const uint64_t* gp[16] = {&ctx.r16.u64, &ctx.r17.u64, &ctx.r18.u64, &ctx.r19.u64, &ctx.r20.u64, &ctx.r21.u64,
                            &ctx.r22.u64, &ctx.r23.u64, &ctx.r24.u64, &ctx.r25.u64, &ctx.r26.u64, &ctx.r27.u64,
                            &ctx.r28.u64, &ctx.r29.u64, &ctx.r30.u64, &ctx.r31.u64};
  bool same = o.r4 == n.r4 && o.r5 == n.r5 && o.r6 == n.r6 && o.r7 == n.r7 && o.r8 == n.r8 && o.r9 == n.r9 &&
              o.r10 == n.r10 && o.r11 == n.r11 && o.r12 == n.r12 && o.lr == n.lr;
  for (int i = 0; i < 16; ++i) same = same && *gp[i] == n.r16_31[i];
  const char* what = same ? nullptr : "registers";
  for (size_t k = 0; k < before.ranges.size() && !what; ++k) {
    const auto& r = after_native.ranges[k];
    for (uint32_t i = 0; i < r.len; ++i) {
      if (*inf::Host(base, r.addr + i) != r.bytes[i]) {
        what = k == 0 ? "stack save area" : k == 1 ? "z_stream" : k == 2 ? "inflate state" : "output";
        break;
      }
    }
  }
  const uint64_t call = g_verify.calls.fetch_add(1) + 1;
  g_verify.bytes.fetch_add(uint64_t(avail_out) - inf::Ld32(base, strm + 16));
  if (what) {
    const uint64_t k = g_verify.mismatches.fetch_add(1);
    if (k < 8) {
      REXLOG_WARN("[inflate] VERIFY MISMATCH call {}: {} differs (strm {:08X} state {:08X} out {:08X}+{}) "
                  "orig r4..r11 {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X} | native {:X} {:X} {:X} {:X} {:X} {:X} {:X} {:X}",
                  call, what, strm, state, next_out, avail_out, o.r4, o.r5, o.r6, o.r7, o.r8, o.r9, o.r10, o.r11, n.r4,
                  n.r5, n.r6, n.r7, n.r8, n.r9, n.r10, n.r11);
    }
  }
  if ((call & 1023) == 0 || what) {
    REXLOG_INFO("[inflate] verify totals: {} calls, {} mismatches, {} not eligible (recompiled), {} bytes compared",
                call, g_verify.mismatches.load(), g_verify.ineligible.load(), g_verify.bytes.load());
  }
}

}  // namespace

extern "C" void ProcessStructFields_82B59D28(PPCContext& __restrict ctx, uint8_t* base) {
  REX_FUNC_PROLOGUE();
  const Mode mode = GetMode();
  const bool stats = StatsOn();
  std::chrono::steady_clock::time_point t0;
  uint32_t avail_out_before = 0, strm = 0;
  if (stats) {
    t0 = std::chrono::steady_clock::now();
    strm = ctx.r3.u32;
    avail_out_before = inf::Ld32(base, strm + 16);
  }
  bool native = false;
  if (mode == Mode::kOriginal) {
    __imp__ProcessStructFields_82B59D28(ctx, base);
  } else if (!inf::Eligible(base, ctx.r3.u32, ctx.r4.u32)) {
    if (mode == Mode::kVerify) g_verify.ineligible.fetch_add(1);
    __imp__ProcessStructFields_82B59D28(ctx, base);
  } else if (mode == Mode::kVerify) {
    Verify(ctx, base);
  } else {
    inf::RunCtx(ctx, base);
    native = true;
  }
  if (stats) {
    ThreadStats& s = t_stats;
    ++s.calls;
    s.native += native;
    s.out_bytes += uint64_t(avail_out_before) - inf::Ld32(base, strm + 16);
    s.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    LogStats(false);
  }
}
