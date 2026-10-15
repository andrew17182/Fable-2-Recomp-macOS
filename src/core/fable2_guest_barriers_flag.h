// fable2_guest_barriers_flag.h - runtime switches shared by the recompiled
// guest code (src/core/fable2_guest_memory.h, via the fable_2_recomp PCH) and
// the app (startup log in fable_2_app.h). No SDK dependencies, so both
// targets see the same inline variables.
//   FABLE2_GUEST_BARRIERS  guest sync/lwsync fences; ON unless set to 0
//   FABLE2_CRASH_DIAG      indirect-call crash diagnostics; OFF unless set to 1
//   FABLE2_FAST_MSR        CR-08 lock-free guest mfmsr / atomic mtmsrd sections; ON unless set to 0
//   FABLE2_FCTIW_ROUNDING  fctiw/fctid round per the guest rounding mode (nearest-even by
//                          default, as on Xenon); 0 restores the old round-half-away path
// All are read once during static initialisation.
#pragma once

#include <cstdlib>

namespace fable2::guestmem {

inline bool EnvFlag(const char* name, bool fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

inline const bool g_barriers = EnvFlag("FABLE2_GUEST_BARRIERS", true);
inline const bool g_crash_diag = EnvFlag("FABLE2_CRASH_DIAG", false);
inline const bool g_fast_msr = EnvFlag("FABLE2_FAST_MSR", true);
inline const bool g_fctiw_rounding = EnvFlag("FABLE2_FCTIW_ROUNDING", true);

}  // namespace fable2::guestmem
