// Build-time check of the native zlib inflate_fast (src/core/hotfunc/load/native_inflate_fast.h,
// FABLE2_NATIVE_INFLATE, docs/load-fixes-20261016.md part G): the game's own zlib 1.2.3 inflate must behave
// identically with the native inflate_fast and with the recompiled one.
//
// The guest functions (inflate 82B53A40, inflate_table 82B597E8, inflate_fast 82B59D28, adler32, crc32,
// updatewindow, the SDK memcpy and their register save/restore helpers) are copied out of generated/default at
// build time by tools/extract_guest_funcs.py into the build folder (game-derived; never committed). The guest
// inflate runs twice in lockstep on two fake guest memories (4 GB reservations, like the game's): memory A with
// the recompiled inflate_fast, memory B with the override's native path (Eligible() ? RunCtx : recompiled). After
// every inflate() call the test compares: return value, every register after every inflate_fast call (r1-r31,
// lr), the z_stream, the whole inflate_state (9520 bytes incl. tables), the window, the output buffer, the input
// buffer and 8 KB of stack. Valid streams must also decompress to the original data (checks the driver).
//
// Streams: host zlib deflate of random, text-like, run, short-period, zero, far-copy and mixed buffers at levels
// 0-9, strategies default / filtered / huffman-only / RLE / fixed, window bits 9-15, raw / zlib / gzip wrappers,
// fed in random chunk sizes; plus corrupted copies (bit flips) for the error paths. Three layouts: normal heap,
// physical (>= 0xE0000000, +0x1000 host offset) and buffers straddling 0xE0000000 (the native path's fallbacks).
// --full: many more cases. --bench: times both inflate_fast versions on the same streams.
#include "fable_2_pch.h"

#include <sys/mman.h>
#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "native_inflate_fast.h"

#include "inflate_guest_funcs.inc"

namespace inf = fable2::inflatefast;

namespace {

uint8_t* g_mem[2] = {nullptr, nullptr};  // A: recompiled inflate_fast, B: native
bool g_bench = false;
int g_bench_native = 0;
double g_fast_ns = 0;

struct CallRec {
  uint64_t r[32];
  uint64_t lr;
  bool native;
};
std::vector<CallRec> g_rec[2];
uint64_t g_fast_calls = 0, g_native_calls = 0, g_ineligible = 0;

int MemIndex(uint8_t* base) { return base == g_mem[1] ? 1 : 0; }

}  // namespace

// The guest inflate calls this for inflate_fast.
extern "C" void ProcessStructFields_82B59D28(PPCContext& __restrict ctx, uint8_t* base) {
  const int m = MemIndex(base);
  bool native = false;
  const auto t0 = std::chrono::steady_clock::now();
  const bool want_native = g_bench ? g_bench_native != 0 : m == 1;
  if (want_native && inf::Eligible(base, ctx.r3.u32, ctx.r4.u32)) {
    inf::RunCtx(ctx, base);
    native = true;
  } else {
    Original_82B59D28(ctx, base);
  }
  g_fast_ns += double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  if (g_bench) return;
  ++g_fast_calls;
  if (m == 1) {
    if (native) ++g_native_calls;
    else ++g_ineligible;
  }
  CallRec rec{};
  const PPCRegister* regs[32] = {&ctx.r0,  &ctx.r1,  &ctx.r2,  &ctx.r3,  &ctx.r4,  &ctx.r5,  &ctx.r6,  &ctx.r7,
                                 &ctx.r8,  &ctx.r9,  &ctx.r10, &ctx.r11, &ctx.r12, &ctx.r13, &ctx.r14, &ctx.r15,
                                 &ctx.r16, &ctx.r17, &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22, &ctx.r23,
                                 &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
  for (int i = 0; i < 32; ++i) rec.r[i] = regs[i]->u64;
  rec.lr = ctx.lr;
  rec.native = native;
  g_rec[m].push_back(rec);
}

namespace {

std::mt19937_64 g_rng(0x1F1A7Eu);
uint64_t g_cases = 0, g_steps = 0, g_fail = 0, g_bytes = 0;
const char* g_case = "";

uint8_t* H(int m, uint32_t a) { return g_mem[m] + a + inf::PhysOff(a); }
void W32(int m, uint32_t a, uint32_t v) { inf::St32(g_mem[m], a, v); }
uint32_t R32(int m, uint32_t a) { return inf::Ld32(g_mem[m], a); }
void PutBytes(int m, uint32_t a, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) *H(m, a + uint32_t(i)) = p[i];
}
bool SameBytes(uint32_t a, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if (*H(0, a + i) != *H(1, a + i)) return false;
  return true;
}

constexpr uint32_t kStateSize = 9520;  // zlib 1.2.3 inflate_state, 32-bit (codes[ENOUGH = 2048] at +1328)

struct Layout {
  const char* name;
  uint32_t strm, state, window, in, out, stack;
};
const Layout kLayouts[] = {
    {"normal", 0x40001000u, 0x40002000u, 0x40010000u, 0x41000000u, 0x48000000u, 0x70010000u},
    {"physical", 0xE0001000u, 0xE0002000u, 0xE0010000u, 0xE1000000u, 0xE8000000u, 0xF0010000u},
    // One buffer crosses 0xE0000000: the native path must fall back (whole call, or per copy for the window).
    {"straddle-in", 0x40001000u, 0x40002000u, 0x40010000u, 0xDFFE0000u, 0x48000000u, 0x70010000u},
    {"straddle-out", 0x40001000u, 0x40002000u, 0x40010000u, 0x41000000u, 0xDFFC0000u, 0x70010000u},
    {"straddle-window", 0x40001000u, 0x40002000u, 0xDFFFC000u, 0x41000000u, 0x48000000u, 0x70010000u},
};

std::vector<uint8_t> MakeData(int kind, size_t n) {
  std::vector<uint8_t> v(n);
  std::uniform_int_distribution<int> byte(0, 255);
  switch (kind) {
    case 0:  // random
      for (auto& b : v) b = uint8_t(byte(g_rng));
      break;
    case 1: {  // text-like
      static const char* words[] = {"the ", "hero ", "of ", "Bower", "stone ", "lake ", "dog ", "and ", "a ",
                                    "Albion\n", "gold ", "quest ", "renown ", "chicken ", "Lucien ", "Theresa "};
      size_t i = 0;
      while (i < n) {
        const char* w = words[g_rng() % 16];
        for (const char* c = w; *c && i < n; ++c) v[i++] = uint8_t(*c);
      }
      break;
    }
    case 2: {  // runs
      size_t i = 0;
      while (i < n) {
        const uint8_t b = uint8_t(byte(g_rng));
        size_t len = 1 + g_rng() % 400;
        while (len-- && i < n) v[i++] = b;
      }
      break;
    }
    case 3: {  // short periods (overlapping copies with distance 1..9)
      size_t i = 0;
      while (i < n) {
        const int p = 1 + int(g_rng() % 9);
        uint8_t pat[9];
        for (int k = 0; k < p; ++k) pat[k] = uint8_t(byte(g_rng));
        size_t len = 3 + g_rng() % 600;
        for (size_t k = 0; k < len && i < n; ++k) v[i++] = pat[k % p];
      }
      break;
    }
    case 4:  // zeros
      break;
    case 5: {  // far copies: blocks repeated up to 32 KB back
      for (size_t i = 0; i < n; ++i) {
        if (i > 1000 && g_rng() % 64 == 0) {
          const size_t d = 1 + g_rng() % std::min<size_t>(i, 32768);
          size_t len = 3 + g_rng() % 300;
          for (; len && i < n; --len, ++i) v[i] = v[i - d];
          --i;
        } else {
          v[i] = uint8_t(byte(g_rng) & 0x3F);
        }
      }
      break;
    }
    default: {  // mixed
      size_t i = 0;
      while (i < n) {
        const auto part = MakeData(int(g_rng() % 6), std::min<size_t>(n - i, 1 + g_rng() % 20000));
        std::memcpy(&v[i], part.data(), part.size());
        i += part.size();
      }
      break;
    }
  }
  return v;
}

// wrap: 0 raw, 1 zlib, 2 gzip.
std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, int level, int wbits, int wrap, int mem_level,
                              int strategy) {
  z_stream z{};
  const int wb = wrap == 0 ? -wbits : wrap == 1 ? wbits : wbits + 16;
  if (deflateInit2(&z, level, Z_DEFLATED, wb, mem_level, strategy) != Z_OK) return {};
  std::vector<uint8_t> out(deflateBound(&z, uLong(data.size())) + 64);
  z.next_in = const_cast<Bytef*>(data.data());
  z.avail_in = uInt(data.size());
  z.next_out = out.data();
  z.avail_out = uInt(out.size());
  // Random sync / full flushes split the stream into blocks of every kind (stored blocks too).
  size_t fed = 0;
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    const size_t step = std::min<size_t>(data.size() - fed, 1 + g_rng() % 70000);
    z.next_in = const_cast<Bytef*>(data.data()) + fed;
    z.avail_in = uInt(step);
    fed += step;
    const int flush = fed >= data.size() ? Z_FINISH : (g_rng() % 4 == 0 ? Z_SYNC_FLUSH : Z_NO_FLUSH);
    rc = deflate(&z, flush);
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) break;
  }
  out.resize(z.total_out);
  deflateEnd(&z);
  return out;
}

// zlib 1.2.3's static tables live in the game image, which the test does not load: seed them at the addresses the
// guest code uses (inflate_table: lbase 820F9FE0, lext 820FA020, dbase 820FA060, dext 820FA0A0; inflate: lenfix
// 820F92F8, distfix 820F9AF8, order 820F9B78). lenfix/distfix are made the way zlib's makefixed() made inffixed.h:
// the guest inflate_table over the fixed code lengths. Valid streams then decompress to the original data, which
// checks the seeding. (gzip's crc table is not seeded: gzip streams end in "incorrect data check", same in A/B.)
void SeedConstants(int m) {
  static const uint16_t lbase[31] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0,  0};
  static const uint16_t lext[31] = {16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 18, 18,  18, 18,
                                    19, 19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 16, 201, 196};
  static const uint16_t dbase[32] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,   33,
                                     49,   65,   97,   129,  193,  257,   385,   513,   769, 1025, 1537,
                                     2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 0,   0};
  static const uint16_t dext[32] = {16, 16, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22,
                                    23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 64, 64};
  static const uint16_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  auto put16 = [&](uint32_t a, const uint16_t* v, int n) {
    for (int i = 0; i < n; ++i) {
      *H(m, a + 2 * i) = uint8_t(v[i] >> 8);
      *H(m, a + 2 * i + 1) = uint8_t(v[i]);
    }
  };
  put16(0x820F9FE0u, lbase, 31);
  put16(0x820FA020u, lext, 31);
  put16(0x820FA060u, dbase, 32);
  put16(0x820FA0A0u, dext, 32);
  put16(0x820F9B78u, order, 19);
  // Fixed tables via the guest inflate_table(type, lens, codes, &next, &bits, work): scratch at 0x50000000.
  const uint32_t lens = 0x50000000u, work = 0x50001000u, next = 0x50002000u, bits = 0x50002010u;
  uint16_t fl[320];
  int i = 0;
  for (; i < 144; ++i) fl[i] = 8;
  for (; i < 256; ++i) fl[i] = 9;
  for (; i < 280; ++i) fl[i] = 7;
  for (; i < 288; ++i) fl[i] = 8;
  put16(lens, fl, 288);
  for (int pass = 0; pass < 2; ++pass) {
    const uint32_t dst = pass == 0 ? 0x820F92F8u : 0x820F9AF8u;
    if (pass == 1) {
      for (i = 0; i < 32; ++i) fl[i] = 5;
      put16(lens, fl, 32);
    }
    W32(m, next, dst);
    W32(m, bits, pass == 0 ? 9 : 5);
    PPCContext c{};
    c.r1.u64 = 0x70010000u;
    c.r3.u64 = pass == 0 ? 1 : 2;  // LENS, DISTS
    c.r4.u64 = lens;
    c.r5.u64 = pass == 0 ? 288 : 32;
    c.r6.u64 = next;
    c.r7.u64 = bits;
    c.r8.u64 = work;
    c.lr = 0x82B54000u;
    ProcessHistogram_82B597E8(c, g_mem[m]);
    if (c.r3.u32 != 0) std::fprintf(stderr, "SeedConstants: inflate_table(%d) returned %d\n", pass, int(c.r3.u32));
  }
}

void InitStream(int m, const Layout& L, int wbits, int wrap) {
  for (uint32_t i = 0; i < 56; ++i) *H(m, L.strm + i) = 0;
  for (uint32_t i = 0; i < kStateSize; ++i) *H(m, L.state + i) = uint8_t(0xA5 ^ (i * 7));  // junk, like malloc
  W32(m, L.strm + 28, L.state);
  W32(m, L.strm + 48, 1);  // adler
  // inflateInit2_ + inflateReset (1.2.3), window preset so updatewindow never calls zalloc.
  W32(m, L.state + 8, uint32_t(wrap));
  W32(m, L.state + 36, uint32_t(wbits));
  W32(m, L.state + 52, L.window);
  W32(m, L.state + 0, 0);       // mode HEAD
  W32(m, L.state + 4, 0);       // last
  W32(m, L.state + 12, 0);      // havedict
  W32(m, L.state + 20, 32768);  // dmax
  W32(m, L.state + 28, 0);      // total
  W32(m, L.state + 32, 0);      // head
  W32(m, L.state + 40, 0);      // wsize
  W32(m, L.state + 44, 0);      // whave
  W32(m, L.state + 48, 0);      // write
  W32(m, L.state + 56, 0);      // hold
  W32(m, L.state + 60, 0);      // bits
  W32(m, L.state + 76, L.state + 1328);
  W32(m, L.state + 80, L.state + 1328);
  W32(m, L.state + 108, L.state + 1328);
}

// Registers for one inflate() call: random everywhere (upper halves too), the same for both memories.
PPCContext MakeCtx(const Layout& L, uint32_t flush) {
  PPCContext c{};
  std::uniform_int_distribution<uint64_t> d;
  for (PPCRegister* r : {&c.r0, &c.r2, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10, &c.r11, &c.r12, &c.r13, &c.r14,
                         &c.r15, &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21, &c.r22, &c.r23, &c.r24, &c.r25,
                         &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31})
    r->u64 = d(g_rng);
  c.r1.u64 = L.stack;
  c.r3.u64 = L.strm;
  c.r4.u64 = flush;
  c.lr = 0x82B49DA0u;
  return c;
}

bool Compare(const Layout& L, size_t comp_n, uint32_t out_hi, const PPCContext& a, const PPCContext& b, int step) {
  const char* what = nullptr;
  if (g_rec[0].size() != g_rec[1].size()) what = "inflate_fast call count";
  for (size_t i = 0; !what && i < g_rec[0].size(); ++i) {
    if (std::memcmp(g_rec[0][i].r, g_rec[1][i].r, sizeof g_rec[0][i].r) != 0 || g_rec[0][i].lr != g_rec[1][i].lr)
      what = "inflate_fast registers";
  }
  if (!what && std::memcmp(&a.r0, &b.r0, sizeof(PPCRegister) * 32) != 0) what = "inflate registers";
  if (!what && a.lr != b.lr) what = "inflate lr";
  if (!what && !SameBytes(L.strm, 56)) what = "z_stream";
  if (!what && !SameBytes(L.state, kStateSize)) what = "inflate_state";
  if (!what && !SameBytes(L.window, 32768 + 16)) what = "window";
  if (!what && !SameBytes(L.out, out_hi + 300)) what = "output";
  if (!what && !SameBytes(L.in, uint32_t(comp_n))) what = "input";
  if (!what && !SameBytes(L.stack - 8192, 8192)) what = "stack";
  if (what && g_fail++ < 10) {
    std::fprintf(stderr, "MISMATCH [%s/%s] step %d: %s (calls A %zu B %zu)\n", g_case, L.name, step, what,
                 g_rec[0].size(), g_rec[1].size());
  }
  return what == nullptr;
}

// Runs one stream through the guest inflate on both memories in lockstep. expect = the original data for a
// valid stream (checked), or nullptr for a corrupted one.
void RunCase(const std::vector<uint8_t>& comp, const std::vector<uint8_t>* expect, size_t out_cap, int wbits,
             int wrap, const Layout& L) {
  ++g_cases;
  for (int m = 0; m < 2; ++m) {
    InitStream(m, L, wbits, wrap);
    PutBytes(m, L.in, comp.data(), comp.size());
    for (uint32_t i = 0; i < 32768 + 16; ++i) *H(m, L.window + i) = uint8_t(i * 13);
    for (uint32_t i = 0; i < out_cap + 300; ++i) *H(m, L.out + i) = 0xCD;
  }
  size_t in_pos = 0, out_pos = 0;
  int rc = 0;
  for (int step = 0; step < 100000; ++step) {
    // Mostly large chunks (inflate_fast needs >= 6 in and >= 258 out), sometimes tiny ones.
    const uint32_t r = uint32_t(g_rng() % 8);
    size_t ain = r < 5 ? 1 + g_rng() % 65536 : r < 7 ? 1 + g_rng() % 300 : 1 + g_rng() % 8;
    size_t aout = (g_rng() % 8) < 6 ? 258 + g_rng() % 70000 : 1 + g_rng() % 600;
    ain = std::min(ain, comp.size() - in_pos);
    aout = std::min(aout, out_cap - out_pos);
    const uint32_t flush = (g_rng() % 5 == 0) ? 4u : (g_rng() % 4 == 0 ? 2u : 0u);
    const PPCContext c0 = MakeCtx(L, flush);
    PPCContext ctx[2] = {c0, c0};
    for (int m = 0; m < 2; ++m) {
      W32(m, L.strm + 0, L.in + uint32_t(in_pos));
      W32(m, L.strm + 4, uint32_t(ain));
      W32(m, L.strm + 12, L.out + uint32_t(out_pos));
      W32(m, L.strm + 16, uint32_t(aout));
      g_rec[m].clear();
      ProcessStructureValidation_82B53A40(ctx[m], g_mem[m]);
    }
    ++g_steps;
    in_pos = R32(0, L.strm + 0) - L.in;
    out_pos = R32(0, L.strm + 12) - L.out;
    if (!Compare(L, comp.size(), uint32_t(out_pos), ctx[0], ctx[1], step)) return;
    rc = int32_t(ctx[0].r3.u32);
    if (std::getenv("INFLATE_TEST_DEBUG")) {
      const uint32_t msg = R32(0, L.strm + 24);
      std::string ms;
      for (uint32_t k = 0; msg && k < 60 && *H(0, msg + k); ++k) ms += char(*H(0, msg + k));
      std::fprintf(stderr, "  step %d ain %zu aout %zu flush %u -> rc %d in %zu out %zu mode %u msg '%s' fast %zu\n", step,
                   ain, aout, flush, rc, in_pos, out_pos, R32(0, L.state), ms.c_str(), g_rec[0].size());
    }
    if (rc == 1 || rc < 0 && rc != -5) break;     // Z_STREAM_END or an error (Z_BUF_ERROR = no progress)
    if (in_pos >= comp.size() && ain == 0) break;  // input exhausted
    if (out_pos >= out_cap) break;
  }
  g_bytes += out_pos;
  if (expect) {
    // gzip: no crc table (see SeedConstants), so the trailer check fails after all the data: same in A and B.
    bool ok = (rc == 1 || (wrap == 2 && rc == -3)) && out_pos == expect->size();
    for (size_t i = 0; ok && i < out_pos; ++i) ok = *H(0, L.out + uint32_t(i)) == (*expect)[i];
    if (!ok && g_fail++ < 10) {
      std::fprintf(stderr, "DRIVER [%s/%s]: guest inflate returned %d with %zu of %zu bytes or wrong data\n", g_case,
                   L.name, rc, out_pos, expect->size());
    }
  }
}

void RunSet(int n_cases, bool corrupt_too) {
  static const char* kinds[] = {"random", "text", "runs", "periods", "zeros", "far", "mixed"};
  static const int strategies[] = {Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED};
  for (int i = 0; i < n_cases; ++i) {
    const int kind = int(g_rng() % 7);
    const size_t n = (g_rng() % 4 == 0) ? g_rng() % 2000 : 1000 + g_rng() % 400000;
    const auto data = MakeData(kind, n);
    const int level = int(g_rng() % 10), wbits = 9 + int(g_rng() % 7), wrap = int(g_rng() % 3);
    const int strategy = strategies[g_rng() % 5], mem_level = 1 + int(g_rng() % 9);
    const auto comp = Compress(data, level, wbits, wrap, mem_level, strategy);
    if (comp.empty()) continue;
    const Layout& L = kLayouts[g_rng() % 5];
    char name[96];
    std::snprintf(name, sizeof name, "%s n=%zu L%d w%d wrap%d s%d", kinds[kind], n, level, wbits, wrap, strategy);
    g_case = name;
    // The guest window size: wbits for raw streams; zlib/gzip headers carry it (inflate checks it against wbits).
    RunCase(comp, &data, data.size() + 64, wrap == 0 ? wbits : 15, wrap, L);
    if (corrupt_too && comp.size() > 8) {
      auto bad = comp;
      const int flips = 1 + int(g_rng() % 6);
      for (int f = 0; f < flips; ++f) bad[2 + g_rng() % (bad.size() - 2)] ^= uint8_t(1u << (g_rng() % 8));
      char cname[128];
      std::snprintf(cname, sizeof cname, "%s corrupt", name);
      g_case = cname;
      RunCase(bad, nullptr, data.size() + 70000, wrap == 0 ? wbits : 15, wrap, L);
    }
  }
}

void Bench() {
  // Same streams through the guest inflate on memory A, once per inflate_fast version.
  g_bench = true;
  std::vector<std::vector<uint8_t>> comps;
  std::vector<std::vector<uint8_t>> datas;
  static const int kinds[] = {1, 5, 6, 3, 2, 0};  // text, far, mixed, periods, runs, random
  for (int i = 0; i < 18; ++i) {
    datas.push_back(MakeData(kinds[i % 6], 1000000));
    comps.push_back(Compress(datas.back(), 6, 15, 0, 8, Z_DEFAULT_STRATEGY));
  }
  const Layout& L = kLayouts[0];
  for (int pass = 0; pass < 2; ++pass) {
    for (int native = 0; native < 2; ++native) {
      g_bench_native = native;
      g_fast_ns = 0;
      size_t total = 0;
      for (size_t k = 0; k < comps.size(); ++k) {
        InitStream(0, L, 15, 0);
        PutBytes(0, L.in, comps[k].data(), comps[k].size());
        W32(0, L.strm + 0, L.in);
        W32(0, L.strm + 4, uint32_t(comps[k].size()));
        size_t out_pos = 0;
        for (int guard = 0; guard < 100000; ++guard) {
          W32(0, L.strm + 12, L.out + uint32_t(out_pos));
          W32(0, L.strm + 16, 65536);
          PPCContext c = MakeCtx(L, 0);
          ProcessStructureValidation_82B53A40(c, g_mem[0]);
          out_pos = R32(0, L.strm + 12) - L.out;
          if (int32_t(c.r3.u32) != 0) break;
        }
        if (out_pos != datas[k].size()) std::fprintf(stderr, "bench stream %zu: %zu of %zu bytes\n", k, out_pos, datas[k].size());
        total += out_pos;
      }
      std::fprintf(stderr, "bench pass %d %s: inflate_fast %.1f ms for %.1f MB (%.2f ns/byte)\n", pass,
                  native ? "native    " : "recompiled", g_fast_ns / 1e6, total / 1e6, g_fast_ns / double(total));
    }
  }
  g_bench = false;
}

}  // namespace

int main(int argc, char** argv) {
  bool full = false, bench = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--full")) full = true;
    else if (!std::strcmp(argv[i], "--bench")) bench = true;
  }
  if (inf::PhysOff(0xE0000000u) != REX_PHYS_HOST_OFFSET(0xE0000000u) ||
      inf::PhysOff(0xDFFFFFFFu) != REX_PHYS_HOST_OFFSET(0xDFFFFFFFu)) {
    std::fprintf(stderr, "PhysOff differs from REX_PHYS_HOST_OFFSET\n");
    return 1;
  }
  for (int m = 0; m < 2; ++m) {
    void* p = mmap(nullptr, 0x100000000ull + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE,
                   -1, 0);
    if (p == MAP_FAILED) {
      std::perror("mmap");
      return 1;
    }
    g_mem[m] = static_cast<uint8_t*>(p);
    SeedConstants(m);
  }
  const auto t0 = std::chrono::steady_clock::now();
  if (bench) {
    Bench();
    return 0;
  }
  if (std::getenv("INFLATE_TEST_DEBUG")) {
    const auto data = MakeData(1, 5000);
    for (int wrap = 0; wrap < 3; ++wrap) {
      const auto comp = Compress(data, 6, 15, wrap, 8, Z_DEFAULT_STRATEGY);
      std::fprintf(stderr, "case wrap %d: %zu -> %zu bytes, first %02X %02X %02X\n", wrap, data.size(), comp.size(),
                   comp[0], comp[1], comp[2]);
      g_case = "debug";
      RunCase(comp, &data, data.size() + 64, 15, wrap, kLayouts[0]);
    }
    return 0;
  }
  RunSet(full ? 3000 : 120, true);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("inflate_fast_tests: %llu streams, %llu inflate calls, %llu inflate_fast calls (%llu native, %llu "
              "recompiled fallback), %.1f MB out, %llu mismatches, %.1f s\n",
              (unsigned long long)g_cases, (unsigned long long)g_steps, (unsigned long long)g_fast_calls / 2,
              (unsigned long long)g_native_calls, (unsigned long long)g_ineligible, g_bytes / 1e6,
              (unsigned long long)g_fail, s);
  if (g_native_calls == 0) {
    std::fprintf(stderr, "inflate_fast_tests: the native path never ran\n");
    return 1;
  }
  return g_fail ? 1 : 0;
}
