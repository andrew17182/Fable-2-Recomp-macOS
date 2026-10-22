// metal_frame_interp.h - MetalFX frame interpolation prototype (FABLE2_FRAME_INTERP=1, default off).
//
// Prototype only (docs/frame-interp-prototype.md): presents one MetalFX-generated frame between two real
// frames (30 rendered -> 60 shown). Fable II has no motion vectors, so they are derived per pixel from the
// scene depth and the camera alone (camera-only reprojection): moving objects ghost.
//
// Guest GPU thread (command processor):
//   NoteDraw      per draw into the full-screen scene depth target: tallies the c0..c3 matrix (the most common
//                 one in Fable II's main pass is the world view-projection of world-space geometry).
//   EncodeSceneInputs  at the game's full-screen depth resolve (after the z prepass): copies the depth and
//                 computes motion = where this pixel was in the previous frame (current -> previous clip space).
//   CommitFrame   at the swap, before the presenter refresh: tags those inputs with the guest output frame id.
// UI thread (presenter):
//   Interpolator::Process  per new guest frame: keeps the previous colour, runs MTLFXFrameInterpolator and
//                 returns the generated frame to present half a frame before the real one.
#pragma once

#include <cstdint>

namespace MTL {
class CommandBuffer;
class Device;
class Texture;
}  // namespace MTL

namespace rex::ui::metal::frame_interp {

// FABLE2_FRAME_INTERP: 1 = interpolate; stats = measure display latency only (baseline arm); else off.
bool Enabled();
bool StatsOnly();
inline bool Active() { return Enabled() || StatsOnly(); }
// The guest-side scene inputs (depth copy, camera matrix) are needed: FABLE2_FRAME_INTERP_INPUTS=depth|camera (the
// default, color, gives MetalFX only the two colour frames).
bool SceneInputsWanted();

// Guest GPU thread. c0_rows: VS float constants c0..c3 (16 floats, rows of the clip transform), vport: PA_CL_VPORT
// x/y/z scale and offset (xs, xo, ys, yo, zs, zo).
void NoteDraw(const float* c0_rows, const float* vport);
// depth: the bound scene depth target (2D or 2D multisample, Depth32Float[_Stencil8]); scale_x/y: draw resolution
// scale (host pixels per guest pixel); depth_to_window: guest window z per host depth unit (2 for float24).
void EncodeSceneInputs(MTL::Device* device, MTL::CommandBuffer* command_buffer, MTL::Texture* depth,
                       uint32_t scale_x, uint32_t scale_y, float depth_to_window);
void CommitFrame(uint64_t frame_id);
// Called at every swap (resets the per-frame draw tally).
void OnSwap();

// UI thread (presenter). Objective-C objects as void*: device id<MTLDevice>, queue id<MTLCommandQueue>, color the
// guest output id<MTLTexture> (RGBA8), wait_event id<MTLSharedEvent> (may be null) + wait_value: the guest output's
// copy submission. Call once per new guest frame. Returns the generated texture to show first (null = show only the
// real frame) and the delay before each present (half the measured real frame interval).
struct PresentPlan {
  void* generated = nullptr;  // id<MTLTexture>
  double half_interval_s = 0.0;
};
PresentPlan Process(void* device, void* queue, void* color, uint64_t frame_id, void* wait_event,
                    uint64_t wait_value);
// Presented-time bookkeeping for the pacing / latency stats ([frame-interp] log lines). ready_s: CACurrentMediaTime
// when the real frame reached the presenter.
// log_row: also write the FABLE2_FRAME_INTERP_PRESENT_LOG row (the present thread writes its own).
void OnPresented(bool generated, double presented_s, double ready_s, bool log_row = true);
double Now();
// Time the UI thread spent in nextDrawable: which 0 = first drawable of a paint, 1 = the real frame's after a
// generated one.
void NoteDrawableWait(int which, double seconds);

// Async presentation (FABLE2_FRAME_INTERP=1). AcquireTarget: a free offscreen BGRA8 target of the drawable size to
// paint into (id<MTLTexture>, null when all are queued). SubmitPresent (after committing the paint): the present
// thread waits for a drawable, copies the target into it and presents it no sooner than min_duration after the
// previous one. frame_id: guest frame for the frame log's presented time (0 = none).
void* AcquireTarget(void* device, uint32_t width, uint32_t height);
// Unless FABLE2_FRAME_INTERP_SCHED=legacy, presents go on the display's vsync grid (presentAtTime): the generated
// frame on vsync N, the real one half a real interval later; lock_frame (vsync lock frame id of the real frame)
// feeds the vsync phase lock from this thread.
void SubmitPresent(void* layer, void* queue, void* target, double min_duration, bool generated, double ready_s,
                   uint64_t frame_id, uint64_t lock_frame);
// A new guest output is ready: wakes WaitPresentTick so the UI thread handles it at once.
void NotifyNewGuestFrame();
void StopPresentThread();
// Blocks until the present thread presents again (or the timeout): paces repaints of an unchanged frame.
void WaitPresentTick(double timeout_s);

}  // namespace rex::ui::metal::frame_interp
