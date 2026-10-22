// metal_frame_interp.h - frame generation: MetalFX frame interpolation, 30 rendered -> 60 shown
// (docs/macos-metal.md "Frame generation"; launcher Settings > Display "Frame generation").
//
// Off unless the game turns it on (fable2_frame_interp_enable from src/core/fable2_frame_gen.h: at startup with
// FABLE2_FRAME_INTERP=1, and live with F10, at the 30 FPS cap). Needs macOS 26 and
// MTLFXFrameInterpolatorDescriptor supportsDevice.
// Colour only: MetalFX gets the previous and the current guest output, zero motion and a flat depth, and estimates
// the motion itself. No game data and no guest-side hooks.
//
// UI thread (presenter), once per new guest frame:
//   Process        copies the guest output into a 2-frame ring and interpolates (previous, current) into the
//                  generated frame, on the presenter queue after the guest output's copy.
//   AcquireTarget / SubmitPresent  the presenter paints the generated frame, then the real one, into offscreen
//                  targets; the present thread copies each into a drawable and presents it at a display time on
//                  the vsync grid (presentDrawable:atTime:): generated on vsync N, real on N + k/2 (k = refreshes
//                  per real frame). It reports each real frame to the vsync phase lock (external mode).
//   WaitPresentTick  repaints of an unchanged frame wait for the next present or new guest frame.
#pragma once

#include <cstdint>

namespace rex::ui::metal::frame_interp {

// On: the game enabled it and the device supports it. Cheap (one atomic load); false until the game decides.
bool Enabled();
double Now();

// UI thread, once per new guest frame. device id<MTLDevice>, queue id<MTLCommandQueue>, color the guest output
// id<MTLTexture>, wait_event id<MTLSharedEvent> (may be null) + wait_value: the guest output's copy submission.
// ready_s: Now() when the frame reached the presenter. Returns the generated texture to show first (null: show only
// the real frame, e.g. the first frame or after a gap).
void* Process(void* device, void* queue, void* color, uint64_t frame_id, void* wait_event, uint64_t wait_value,
              double ready_s);

// A free offscreen BGRA8 target of the drawable size to paint into (id<MTLTexture>; null when all are queued).
void* AcquireTarget(void* device, uint32_t width, uint32_t height);
// After committing the paint into `target`: queue it for the present thread. ready_s: when the real frame reached
// the presenter (0 for the generated frame), frame_id: frame log id (0 = none), lock_frame: vsync lock frame id of
// the real frame (both presents of a pair carry it; 0 = none).
void SubmitPresent(void* layer, void* queue, void* target, bool generated, double ready_s, uint64_t frame_id,
                   uint64_t lock_frame);
// A new guest output is ready: wakes WaitPresentTick so the UI thread handles it at once.
void NotifyNewGuestFrame();
// Blocks until the present thread presents again, a new guest frame arrives, or the timeout.
void WaitPresentTick(double timeout_s);
void StopPresentThread();
// Waits (up to timeout_s) until the present thread has presented every queued frame: turning frame generation off
// hands presenting back to the UI thread without two threads presenting at once.
void Drain(double timeout_s);

}  // namespace rex::ui::metal::frame_interp
