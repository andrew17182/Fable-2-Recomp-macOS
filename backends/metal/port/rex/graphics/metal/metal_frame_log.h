#pragma once
#include <cstdint>

namespace rex::graphics::metal::frame_log {
// Initialized once before launching the guest. Callbacks never perform disk I/O.
void Initialize();
bool Enabled();
uint64_t NowUs();
uint64_t BeginFrame();
uint64_t CurrentFrame();
void SetSynthetic(bool value);
void Warmup(bool active);
void Record(const char* event, uint64_t frame, uint64_t duration_us = 0);
// Metal's presentedTime uses the Core Animation / mach absolute uptime clock.
void Presented(uint64_t frame, double uptime_seconds);
// Trace-only (FABLE2_GPU_TIME_TRACE): an event stamped at a mach-uptime instant (e.g. MTLCommandBuffer GPUStartTime).
void RecordUptime(const char* event, uint64_t frame, double uptime_seconds, uint64_t duration_us);
// Flush acknowledgement (readiness, docs/readiness.md): the newest guest_swap frame id and the CSV clock
// (time_us) up to which rows are on disk (0 before the first flush). RequestFlush wakes the writer and
// waits up to timeout_ms for a flush that started after the call; true when it happened.
uint64_t FlushedFrame();
uint64_t FlushedUs();
bool RequestFlush(int timeout_ms);
}  // namespace rex::graphics::metal::frame_log
