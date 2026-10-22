#pragma once
// Vsync phase lock (FABLE2_VSYNC_PHASE_LOCK, docs/macos-metal.md "Vsync phase lock").
//
// The guest vblank interrupt comes from a host timer (60 Hz guest video mode) that is not aligned with the
// display. Each launch the guest frame therefore reaches the presenter at a random phase relative to vsync: frames
// wait in CAMetalLayer's queue (2-3 refreshes swap->photon), and near the bad phase swap jitter flips frames
// between two vsyncs (judder, dropped frames). With the lock on, MetalGraphicsSystem replaces the SDK's vblank
// worker with one that keeps the same guest-tick schedule but shifts its phase, a fraction of a millisecond per
// frame, so the guest frame is painted a learned lead before a display vsync (presentedTime feedback):
//   lead = max(vblank -> paint, last ~4 s) + paint -> latch (learned, only grows on a miss) + margin.
// A frame presented a refresh later than targeted means a miss; a run of them means the queue holds an extra
// frame, which rate-locked production never drains, so one vblank is delayed by a refresh (drain).
// The lock is inactive (plain timer, same phase as the last lock) without presentedTime feedback (occluded or no
// window), when the guest clock is scaled (turbo, slow mode), uncapped (vsync off), or when the frame interval is
// not a whole number of display refreshes.
#include <cstdint>

namespace rex::graphics::metal::vsync_lock {
// FABLE2_VSYNC_PHASE_LOCK=1 (default off). Read once.
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
}  // namespace rex::graphics::metal::vsync_lock
