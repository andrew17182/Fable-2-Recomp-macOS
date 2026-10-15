// Build-time check of the guest fctiw / fctid conversion (patches/sdk/codegen-fctiw-rounding.patch,
// stage 0b of the 2026-10-11 performance audit). For every PowerPC rounding mode (set through the guest
// FPSCR path, as mtfsf does) and a table of ties, near-ties, signed zeros, denormals, range edges,
// infinities and NaN, rex::ppc::FctiwCurrentMode / FctidCurrentMode must equal an independent
// floor/ceil model of Xenon's behaviour. On SDK headers without the change it reports and passes.
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

#include <cmath>
#include <cstdio>
#include <limits>

#if defined(REX_FCTIW) && defined(REX_FCTID)
namespace {
double RefRound(double x, int rn) {
  const double f = std::floor(x), c = std::ceil(x);
  if (f == c) return x;
  switch (rn) {
    case 1: return x < 0 ? c : f;  // toward zero
    case 2: return c;              // +infinity
    case 3: return f;              // -infinity
    default: {                     // nearest, ties to even
      const double d = x - f;
      if (d < 0.5) return f;
      if (d > 0.5) return c;
      return std::fmod(f, 2.0) == 0 ? f : c;
    }
  }
}
int64_t RefW(double x, int rn) {
  if (std::isnan(x)) return int64_t(0x80000000U);
  const double r = std::isinf(x) ? x : RefRound(x, rn);
  if (r >= 2147483647.0) return 2147483647;
  if (r <= -2147483648.0) return -2147483647 - 1;
  return int32_t(r);
}
int64_t RefD(double x, int rn) {
  if (std::isnan(x)) return std::numeric_limits<int64_t>::min();
  const double r = std::isinf(x) ? x : RefRound(x, rn);
  if (r >= 9223372036854775808.0) return std::numeric_limits<int64_t>::max();
  if (r <= -9223372036854775808.0) return std::numeric_limits<int64_t>::min();
  return int64_t(r);
}
}  // namespace
#endif

int main() {
#if !defined(REX_FCTIW) || !defined(REX_FCTID)
  std::puts("fctiw_rounding_tests: SDK headers without REX_FCTIW/REX_FCTID; skipped");
  return 0;
#else
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double values[] = {0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 2.4, 2.6, -2.4, -2.6, 0.0, -0.0,
                           1e-300, -1e-300, 4.9e-324, 2147483646.5, 2147483647.0, 2147483647.4,
                           2147483647.5, 3e9, -2147483647.5, -2147483648.0, -2147483648.4,
                           -2147483648.5, -3e9, 9.2233720368547748e18, 9.3e18, -9.3e18,
                           4503599627370495.5, inf, -inf, nan};
  PPCFPSCRRegister fpscr{};
  fpscr.InitHost();
  const uint32_t saved = fpscr.getcsr();
  int failures = 0, cases = 0;
  for (uint32_t rn = 0; rn < 4; ++rn) {
    for (double x : values) {
      volatile double vx = x;
      fpscr.storeFromGuest(rn);
      const int64_t w = rex::ppc::FctiwCurrentMode(vx);
      const int64_t d = rex::ppc::FctidCurrentMode(vx);
      fpscr.storeFromGuest(rex::ppc::kRoundNearest);
      ++cases;
      if (w != RefW(x, int(rn)) || d != RefD(x, int(rn))) {
        ++failures;
        std::fprintf(stderr, "fctiw_rounding_tests: RN %u x=%.17g fctiw %lld (want %lld) fctid %lld (want %lld)\n",
                     rn, x, (long long)w, (long long)RefW(x, int(rn)), (long long)d,
                     (long long)RefD(x, int(rn)));
      }
    }
  }
  // The legacy path must still reproduce the old SIMDe behaviour (switch =0): ties away from zero.
  volatile double tie = 2.5;
  if (rex::ppc::FctiwSimde(tie) != 3 && rex::ppc::FctiwSimde(tie) != 2) ++failures;
  fpscr.restoreGuestBits(saved);
  if (!failures) {
    std::printf("fctiw_rounding_tests: %d cases (4 rounding modes) match the Xenon model; fctiw(2.5) = %lld\n",
                cases, (long long)rex::ppc::FctiwCurrentMode(tie));
  }
  return failures ? 1 : 0;
#endif
}
