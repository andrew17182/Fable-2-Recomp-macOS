// Metal plugin side of the flight recorder (telemetry phase 1). The recorder lives in the
// executable (src/diagnostics/fable2_flight_recorder.*); this resolves its C API once with
// dlsym(RTLD_DEFAULT, "fable2_fr_api") and forwards frame events. Every call is a no-op when the
// executable has no recorder or FABLE2_TRACE=off.
#pragma once
#include <cstdint>

#include "fable2_trace_api.h"

namespace rex::graphics::metal::trace {
const Fable2FrApi* Api();  // nullptr when unavailable
// Guest swap: returns the recorder's frame id (0 without a recorder).
uint32_t Swap();
// Presenter paint on the UI thread (hang detector).
void Paint();
// Frame-CSV bridge: every frame_log row also becomes a ring event (a = frame, v = duration_us).
void Csv(const char* name, uint64_t frame, uint64_t mach_ticks, uint64_t duration_us);
uint64_t NowTicks();
uint64_t UptimeSecondsToTicks(double seconds);
}  // namespace rex::graphics::metal::trace
