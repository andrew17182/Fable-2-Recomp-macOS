#pragma once
// Vsync phase lock (FABLE2_VSYNC_PHASE_LOCK, docs/macos-metal.md "Vsync phase lock").
//
// The guest vblank interrupt comes from a host timer (60 Hz guest video mode) that is not aligned with the
// display. Each launch the finished guest frame therefore lands at a random phase before the UI thread's paint loop
// samples it (the loop wakes from a blocked nextDrawable just after each vsync): 0-16 ms of extra wait, and near
// the edge swap jitter flips frames between two paints (dropped frames at 60, judder at 30). With the lock on,
// MetalGraphicsSystem replaces the SDK's vblank worker with one that keeps the same guest-tick schedule but shifts
// its phase (at most 1 ms per frame) so the frame's guest GPU work completes `margin` before a paint wake:
//   lead = p95(vblank -> guest GPU done, last 240 frames) + margin; target = wake phase - lead (mod refresh).
// With an external present thread (OnExternalSchedule, frame interpolation) the cliff is that thread's decision
// deadline before the vsync it targets. Without a UI paint loop (frames painted on arrival) the target is a learned latch before the presentedTime
// grid instead; a run of frames shown a refresh late there means an extra queued frame, which rate-locked
// production never drains, so one vblank is delayed by a refresh (drain).
// The lock is inactive (plain timer, same phase as the last lock) without presentedTime feedback (hidden or no
// window), when the guest clock is scaled (turbo, slow mode), uncapped (vsync off), or when the frame interval is
// not a whole number of display refreshes.
#include <cstdint>

namespace rex::graphics::metal::vsync_lock {
// FABLE2_VSYNC_PHASE_LOCK (default on; =0 turns it off). Read once.
bool Enabled();
// FABLE2_VSYNC_LOCK_TRACE=1: per-frame lock rows in the frame CSV (vlock_*).
bool TraceEnabled();
// Mach uptime in seconds (the clock of CAMetalDrawable.presentedTime).
double NowSeconds();

// Vblank worker side.
void NoteVblank(double host_seconds);
// Host time of the latest guest vblank (0 before the first).
double LastVblank();
// Phase correction to apply to the vblank schedule (ns; negative = earlier). Consumed by the call.
int64_t TakeCorrectionNs();
// Worker state that suspends the lock (guest clock scaled, vsync cvar off).
void SetSuspended(bool suspended);

// Presenter side.
void SetDisplayRefreshHz(double hz);
// A guest output became ready to present (its final pass submitted), with the latest vblank at that moment.
void OnGuestReady(uint64_t frame, double ready_seconds, double vblank_seconds);
// Its guest GPU work completed (shared-event notification).
void OnGuestDone(uint64_t frame, double done_seconds);
// Every paint: drawable acquisition time (after nextDrawable) and the nextDrawable wait; new_frame when it
// shows `frame` for the first time.
void OnPaint(uint64_t frame, bool new_frame, double acquire_seconds, double drawable_wait_seconds);
// Its presentedTime (0 = not presented).
void OnPresented(uint64_t frame, double presented_seconds);

// Display timeline for other presenters (e.g. a frame-interpolation present thread): the latest known display
// vsync (a presentedTime) and the refresh period, both mach-uptime seconds. False before the first presented
// frame. NextVsyncAfter(t) = first grid point after t; the lock's current vblank -> photon target is Lead().
bool DisplayGrid(double* last_vsync_seconds, double* period_seconds);
double NextVsyncAfter(double t_seconds);
double Lead();

// Frames presented by another thread on the grid (frame interpolation present thread, presentAtTime). Per real
// frame, before its OnPresented: `handled` = when that thread took the frame, `deadline` = the latest time it could
// have taken it and still made the vsync it targeted (its cliff), `slipped` = it missed the planned vsync. From the
// first call on, the lock targets this cliff instead of the UI paint loop's wake: vblank + p95(vblank -> handled) +
// margin lands on the deadline phase.
void OnExternalSchedule(uint64_t frame, double handled_seconds, double deadline_seconds, bool slipped);
}  // namespace rex::graphics::metal::vsync_lock
