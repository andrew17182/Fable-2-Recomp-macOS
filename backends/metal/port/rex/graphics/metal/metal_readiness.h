#pragma once
// [ready] Readiness signals for test harnesses (docs/readiness.md): instead of waiting a fixed time after
// load or after a camera turn, tools ask the game whether it is ready to measure.
//
// On only when a harness is attached: FABLE2_TEST_CONTROL_DIR (test-control sessions) or FABLE2_BENCHMARK
// (benchmark mode) is set, or FABLE2_READINESS=1; FABLE2_READINESS=0 turns it off. Off, the per-draw hook
// is one increment and the per-swap hook returns at once.
//
// Evaluated on the GPU Commands thread at every guest swap (before the frame id advances):
//   world    the world is drawing: >= FABLE2_READY_WORLD_DRAWS (500) depth-target draws per frame for 10
//            frames in a row (the loading screen draws ~90-250; Bower Lake ~2700, Old Tomb ~1300 median).
//            Cleared after 30 frames in a row below the threshold (an area-transition loading screen).
//   steady   frame pacing is stable: the median of the last 15 swap intervals is within 5% of the 15 before,
//            no interval of the last 15 is over 2x that median, and no frame of the last 15 spent >= 1 ms
//            (FABLE2_READY_PSO_MS) building pipelines / translating shaders on the draw path (hitch pso_* / xlate).
//   bg_busy  background shader work still pending: native-shaders optimizer / native translator / ns-spec
//            queues and busy workers, async pipeline creation, the startup pipeline warm-up, and ns-spec
//            auto-learn still sampling a shader that has not converged (a sample in the last 1 s).
// Each is published as a flight-recorder tag (world, steady, bg_busy) on change, so the test-control `state`
// reply, dumps and the in-game benchmark (src/core/fable2_benchmark.h) see them; `state` also carries a
// detailed `ready` object (StateJson).
#include <cstdint>
#include <string>

namespace rex::graphics::metal::readiness {

bool Enabled();

// Depth-target draws this frame (GPU Commands thread; MetalRenderTargetCache draw path).
extern uint32_t g_frame_draws;
inline void NoteDraw() { ++g_frame_draws; }

// Background work snapshot (from the pipeline cache's probe).
struct Background {
  uint32_t ns_pending = 0;         // optimizer / native-translator queue + busy workers + swap queues
  uint32_t ns_spec_pending = 0;    // ns-spec specialize / build queues
  uint32_t pipelines_pending = 0;  // async pipeline creation queue + busy creation threads
  bool warmup_pending = false;     // startup pipeline warm-up not finished
};
using BackgroundProbe = Background (*)(void* owner);
void SetBackgroundProbe(BackgroundProbe probe, void* owner);
void ClearBackgroundProbe(void* owner);

// ns-spec auto-learn: steady-clock ns of the last sample of a shader that had not converged yet.
void NoteUnconvergedSample(int64_t steady_ns);

// GPU Commands thread, at every guest swap, before hitch::OnSwap resets its per-frame counters.
// pso_us = time in the pso_* + xlate hitch scopes this frame (a frame with >= FABLE2_READY_PSO_MS (1 ms) of it
// counts as pipeline work on the draw path).
void OnSwap(uint64_t frame, double pso_us);

// Detailed JSON object for the test-control `state` reply ("{...}").
std::string StateJson();

}  // namespace rex::graphics::metal::readiness
