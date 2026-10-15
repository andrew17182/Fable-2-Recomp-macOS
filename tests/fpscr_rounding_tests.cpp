// Build-time check of the guest FPSCR rounding-mode mapping (patches/sdk/fpscr-rounding-per-arch.patch).
// Every PowerPC FPSCR[RN] mode must survive mtfsf -> host FPCR/MXCSR -> mffs, and the host must
// really round in that direction. On the prebuilt runtime (unfixed headers) it reports and passes.
#include <rex/ppc/context.h>

#include <cstdio>

int main() {
#ifndef REX_FPSCR_HOST_TO_GUEST_PER_ARCH
  std::puts("fpscr_rounding_tests: SDK headers without the per-arch rounding table; skipped");
  return 0;
#else
  PPCFPSCRRegister fpscr{};
  fpscr.InitHost();
  const uint32_t saved = fpscr.getcsr();
  int failures = 0;
  for (uint32_t mode = 0; mode < 4; ++mode) {
    fpscr.storeFromGuest(mode);
    const uint32_t back = fpscr.loadFromHost();
    if (back != mode) {
      std::fprintf(stderr, "fpscr_rounding_tests: RN %u read back as %u\n", mode, back);
      ++failures;
    }
  }
  // Direction check: 1/3 rounded toward +inf must exceed 1/3 rounded toward -inf, and
  // -1/3 toward zero must equal -1/3 toward +inf.
  volatile double one = 1.0, three = 3.0;
  fpscr.storeFromGuest(rex::ppc::kRoundUp);
  const double up = one / three, neg_up = -one / three;
  fpscr.storeFromGuest(rex::ppc::kRoundDown);
  const double down = one / three;
  fpscr.storeFromGuest(rex::ppc::kRoundTowardZero);
  const double neg_zero = -one / three;
  fpscr.storeFromGuest(rex::ppc::kRoundNearest);
  if (!(up > down)) {
    std::fprintf(stderr, "fpscr_rounding_tests: RN 2 (+inf) does not round above RN 3 (-inf)\n");
    ++failures;
  }
  if (neg_zero != neg_up) {
    std::fprintf(stderr, "fpscr_rounding_tests: RN 1 (zero) differs from +inf for a negative value\n");
    ++failures;
  }
  fpscr.restoreGuestBits(saved);
  if (failures == 0) std::puts("fpscr_rounding_tests: all 4 rounding modes round-trip and round correctly");
  return failures == 0 ? 0 : 1;
#endif
}
