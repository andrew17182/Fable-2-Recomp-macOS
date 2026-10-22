#include "rex/graphics/metal/metal_vsync_lock.h"

#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/logging.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace rex::graphics::metal::vsync_lock {
namespace {

double EnvMs(const char* name, double fallback) {
  const char* v = std::getenv(name);  // getenv-ok: once, from a static initializer
  if (!v || !*v) return fallback;
  char* end = nullptr;
  const double d = std::strtod(v, &end);
  return end != v && std::isfinite(d) ? d : fallback;
}

struct Config {
  bool enabled = false;
  double c0 = 0.0;      // initial paint -> photon estimate beyond one refresh (s)
  double margin = 0.0;  // extra lead (s)
  double offset = 0.0;
  double gain = 0.0;
  bool log_frames = false;
};

const Config& Cfg() {
  static const Config cfg = [] {
    Config c;
    const char* v = std::getenv("FABLE2_VSYNC_PHASE_LOCK");  // getenv-ok: once (static)
    // Default on (2026-10-22); 0/off/false turns it off.
    c.enabled = !v || !*v || (std::strcmp(v, "0") != 0 && std::strcmp(v, "off") != 0 && std::strcmp(v, "false") != 0);
    c.c0 = EnvMs("FABLE2_VSYNC_LOCK_LATCH_MS", 3.0) / 1000.0;  // beyond one refresh
    c.margin = EnvMs("FABLE2_VSYNC_LOCK_MARGIN_MS", 1.5) / 1000.0;
    c.offset = EnvMs("FABLE2_VSYNC_LOCK_OFFSET_MS", 0.0) / 1000.0;  // experiments: extra phase lead
    c.gain = std::clamp(EnvMs("FABLE2_VSYNC_LOCK_GAIN", 0.2), 0.01, 1.0);
    c.log_frames = EnvMs("FABLE2_VSYNC_LOCK_TRACE", 0.0) != 0.0;
    REXLOG_INFO("[vsync-lock] {} (FABLE2_VSYNC_PHASE_LOCK={}): latch {:.1f} ms, margin {:.1f} ms, gain {:.2f}",
                c.enabled ? "on" : "off", v ? v : "unset", c.c0 * 1e3, c.margin * 1e3, c.gain);
    return c;
  }();
  return cfg;
}

struct PendingFrame {
  uint64_t frame = 0;
  double ready = 0, vblank = 0, acquire = 0, wait = 0, done = 0;
};

struct State {
  std::mutex mutex;
  std::atomic<double> last_vblank{0.0};
  std::atomic<int64_t> correction_ns{0};
  std::atomic<bool> suspended{false};
  std::atomic<double> refresh_hz{60.0};
  std::atomic<double> grid_vsync{0.0}, lead_atomic{0.0};

  // Guarded by mutex.
  std::array<PendingFrame, 32> pending{};
  size_t pending_next = 0;
  double last_ready = 0;
  std::deque<double> ready_intervals;  // guest output ready -> ready (s)
  std::deque<double> works;            // vblank -> guest output ready (s), trusted frames
  std::deque<double> wakes;            // phase of blocked paints' drawable acquisition after a vsync (s)
  std::deque<uint8_t> blocked;         // per paint: nextDrawable blocked
  double latch = 0;                    // immediate mode: ready -> photon beyond the grid (s)
  double lead = 0;                     // target: vblank this long before the cliff grid point (s)
  double drain_until_vblank = 0;
  int consecutive_late = 0, consecutive_early = 0;
  bool just_drained = false;
  bool locked = false;
  // Window stats for the periodic log line.
  double window_start = 0;
  uint64_t n_frames = 0, n_late = 0, n_spike_late = 0, n_drains = 0, n_inactive = 0, n_untrusted = 0, n_done = 0;
  double sum_abs_err = 0, sum_ready_to_photon = 0, corr_total = 0;
  uint64_t n_err = 0;
  bool ui_mode = false;
  bool ext_mode = false;     // OnExternalSchedule seen: cliff = ext_deadline phase
  double ext_deadline = 0;   // latest deadline (s, absolute)
  uint64_t n_ext_slips = 0;
  double cliff = 0;
  double w_bound = 0;
};

State& Get() {
  static State* s = new State;  // outlives late Metal callbacks at exit
  return *s;
}

double Wrap(double x, double period) {  // into [-period/2, period/2)
  double r = std::fmod(x, period);
  if (r < -period / 2) r += period;
  if (r >= period / 2) r -= period;
  return r;
}

PendingFrame* Find(State& s, uint64_t frame) {
  for (auto& p : s.pending) {
    if (p.frame == frame) return &p;
  }
  return nullptr;
}

}  // namespace

bool Enabled() { return Cfg().enabled; }

bool TraceEnabled() { return Cfg().log_frames && frame_log::Enabled(); }

double NowSeconds() {
  static const double scale = [] {
    mach_timebase_info_data_t s;
    mach_timebase_info(&s);
    return double(s.numer) / double(s.denom) / 1e9;
  }();
  return double(mach_absolute_time()) * scale;
}

void NoteVblank(double host_seconds) { Get().last_vblank.store(host_seconds, std::memory_order_release); }

double LastVblank() { return Get().last_vblank.load(std::memory_order_acquire); }

int64_t TakeCorrectionNs() { return Get().correction_ns.exchange(0, std::memory_order_acq_rel); }

void SetSuspended(bool suspended) { Get().suspended.store(suspended, std::memory_order_relaxed); }

void SetDisplayRefreshHz(double hz) {
  if (!(hz >= 20.0 && hz <= 500.0)) hz = 60.0;
  const double old = Get().refresh_hz.exchange(hz, std::memory_order_relaxed);
  if (Enabled() && std::fabs(old - hz) > 0.01) REXLOG_INFO("[vsync-lock] display refresh {:.2f} Hz", hz);
}

void OnGuestReady(uint64_t frame, double ready, double vblank) {
  if (!Enabled() || !frame) return;
  auto& s = Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.last_ready > 0) {
    s.ready_intervals.push_back(ready - s.last_ready);
    if (s.ready_intervals.size() > 31) s.ready_intervals.pop_front();
  }
  s.last_ready = ready;
  s.pending[s.pending_next] = PendingFrame{frame, ready, vblank, 0, 0, 0};
  s.pending_next = (s.pending_next + 1) % s.pending.size();
}

void OnGuestDone(uint64_t frame, double done) {
  if (!Enabled() || !frame) return;
  auto& s = Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (PendingFrame* f = Find(s, frame)) f->done = done;
}

void OnPaint(uint64_t frame, bool new_frame, double acquire, double wait) {
  if (!Enabled()) return;
  auto& s = Get();
  const double period = 1.0 / s.refresh_hz.load(std::memory_order_relaxed);
  const double grid = s.grid_vsync.load(std::memory_order_acquire);
  if (Cfg().log_frames && frame_log::Enabled()) {
    frame_log::RecordUptime(new_frame ? "vlock_paint_new" : "vlock_paint", frame, acquire, uint64_t(wait * 1e6));
  }
  std::lock_guard<std::mutex> lock(s.mutex);
  s.ext_mode = false;  // the UI thread presents again (frame generation turned off)
  const bool was_blocked = wait > 0.001;
  s.blocked.push_back(was_blocked ? 1 : 0);
  if (s.blocked.size() > 120) s.blocked.pop_front();
  if (was_blocked && grid > 0) {
    // A blocked nextDrawable returns when the display releases a drawable, just after a vsync: the UI-thread
    // paint loop samples the newest guest output at this phase.
    s.wakes.push_back(Wrap(acquire - grid, period));
    if (s.wakes.size() > 120) s.wakes.pop_front();
  }
  if (new_frame && frame) {
    if (PendingFrame* f = Find(s, frame)) {
      if (f->acquire == 0) {
        f->acquire = acquire;
        f->wait = wait;
      }
    }
  }
}

void OnExternalSchedule(uint64_t frame, double handled, double deadline, bool slipped) {
  if (!Enabled() || !frame) return;
  auto& s = Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.ext_mode = true;
  if (deadline > 0) s.ext_deadline = deadline;
  if (slipped) ++s.n_ext_slips;
  if (PendingFrame* f = Find(s, frame)) {
    if (f->acquire == 0) f->acquire = handled;
  }
}

void OnPresented(uint64_t frame, double presented) {
  if (!Enabled() || !frame) return;
  auto& s = Get();
  const Config& cfg = Cfg();
  std::lock_guard<std::mutex> lock(s.mutex);
  PendingFrame f;
  if (PendingFrame* p = Find(s, frame)) {
    f = *p;
    p->frame = 0;
  }
  if (presented <= 0) return;
  s.grid_vsync.store(presented, std::memory_order_release);
  if (!f.frame || f.vblank <= 0 || f.ready <= 0) return;
  const double period = 1.0 / s.refresh_hz.load(std::memory_order_relaxed);
  if (s.window_start == 0) s.window_start = presented;
  if (s.latch == 0) s.latch = period + cfg.c0;
  ++s.n_frames;

  // Active only when the frame interval is a whole number of refreshes and the worker runs at 1x with vsync.
  bool active = !s.suspended.load(std::memory_order_relaxed);
  double frame_period = 0;
  if (active && s.ready_intervals.size() >= 15) {
    std::vector<double> iv(s.ready_intervals.begin(), s.ready_intervals.end());
    std::nth_element(iv.begin(), iv.begin() + iv.size() / 2, iv.end());
    frame_period = iv[iv.size() / 2];
    const double ratio = frame_period / period;
    active = std::round(ratio) >= 1 && std::fabs(ratio - std::round(ratio)) < 0.06;
  } else {
    active = false;
  }
  if (!active) {
    ++s.n_inactive;
    s.locked = false;
    s.consecutive_late = 0;
  }

  // Work = vblank -> the frame's guest GPU work done (CPU ready if the notification has not arrived; it is
  // never later than the paint that waited on it).
  // External present thread: work runs to the moment that thread took the frame (it decides the slot then).
  const double finished = s.ext_mode && f.acquire > 0 ? f.acquire : f.done > 0 ? f.done : f.ready;
  const double work = finished - f.vblank;
  if (f.done > 0) ++s.n_done;
  const bool trusted = work >= 0 && work < 0.98 * std::max(period, frame_period);
  const double w_max_prev = s.w_bound;  // last frame's work bound
  if (trusted) {
    s.works.push_back(work);
    if (s.works.size() > 240) s.works.pop_front();
  } else {
    ++s.n_untrusted;
  }
  // Work bound: 95th percentile of the last 240 frames. A max would let a few loading frames (15 ms) set the
  // lead for seconds, wrapping typical frames (1 ms) onto the cliff, the bad phase this lock exists to avoid.
  double w_max = 0;
  if (!s.works.empty()) {
    std::vector<double> w(s.works.begin(), s.works.end());
    const size_t i = std::min(w.size() - 1, size_t(double(w.size()) * 0.95));
    std::nth_element(w.begin(), w.begin() + i, w.end());
    w_max = w[i];
  }

  // UI-thread paint loop (a UI drawer keeps the UI thread painting, blocked in nextDrawable): the cliff is the
  // loop's wake phase. Otherwise frames are painted on arrival and the cliff is the latch before a vsync.
  size_t n_blocked = 0;
  for (uint8_t b : s.blocked) n_blocked += b;
  s.ui_mode = !s.ext_mode && s.blocked.size() >= 30 && n_blocked * 2 > s.blocked.size() && s.wakes.size() >= 30;

  if (active && trusted) {
    s.sum_ready_to_photon += presented - finished;
    double err;
    if (s.ext_mode) {
      // Cliff = the present thread's decision deadline, as a phase after the grid vsync.
      s.cliff = Wrap(s.ext_deadline - presented, period);
      if (s.cliff < 0) s.cliff += period;
      s.lead = w_max + cfg.margin + cfg.offset;
      err = Wrap(f.vblank + s.lead - (presented + s.cliff), period);
    } else if (s.ui_mode) {
      std::vector<double> w(s.wakes.begin(), s.wakes.end());
      std::nth_element(w.begin(), w.begin() + w.size() / 50, w.end());
      s.cliff = w[w.size() / 50];  // earliest wakes (2nd percentile) after the grid vsync
      // Ready (vblank + worst work + margin) should land just before a wake.
      s.lead = w_max + cfg.margin + cfg.offset;
      err = Wrap(f.vblank + s.lead - (presented + s.cliff), period);
      // A frame that missed its wake waited for the next one (a repeated frame on screen; no queue builds, the
      // loop always takes the newest output).
      if (s.locked && f.acquire > 0 && f.acquire - finished > 0.6 * period) {
        ++s.n_late;
        if (work > w_max_prev + 0.0005) ++s.n_spike_late;
      }
    } else {
      const double d = presented - f.vblank;  // vblank -> photon
      // Lateness: presented k refreshes after the grid point this vblank targeted (`lead` after it).
      const int k = s.locked && f.vblank > s.drain_until_vblank ? int(std::lround((d - s.lead) / period)) : 0;
      if (k >= 1) {
        if (s.just_drained) {
          // Still late right after a drain: not a queued frame, the latch needs whole refreshes more.
          s.latch += k * period;
          s.just_drained = false;
          s.consecutive_late = 0;
        } else {
          ++s.n_late;
          if (work > w_max_prev + 0.0005) {
            ++s.n_spike_late;
          } else if (s.consecutive_late == 0) {
            s.latch += 0.0005;  // missed with ordinary work: the latch needs more time than estimated
          }
          if (++s.consecutive_late >= 3) {
            // The queue holds an extra frame; rate-locked production never drains it. Delay one vblank by a
            // refresh.
            s.correction_ns.fetch_add(int64_t(period * 1e9), std::memory_order_acq_rel);
            s.corr_total += period;
            ++s.n_drains;
            s.consecutive_late = 0;
            s.just_drained = true;
            s.drain_until_vblank = f.vblank + 4 * frame_period + period;  // frames already in flight
          }
        }
        s.consecutive_early = 0;
      } else if (k <= -1) {
        if (++s.consecutive_early >= 30) {  // shown earlier than targeted: the latch is whole refreshes too long
          s.latch = std::max(0.002, s.latch + k * period);
          s.consecutive_early = 0;
        }
        s.consecutive_late = 0;
      } else if (s.locked && f.vblank > s.drain_until_vblank) {
        s.consecutive_late = s.consecutive_early = 0;
        s.just_drained = false;
      }
      s.lead = w_max + s.latch + cfg.margin + cfg.offset;
      err = Wrap(f.vblank - (presented - s.lead), period);
    }
    s.sum_abs_err += std::fabs(err);
    ++s.n_err;
    const double step_cap = std::fabs(err) > 0.003 ? 0.001 : 0.0005;
    const double corr = std::clamp(-cfg.gain * err, -step_cap, step_cap);
    s.correction_ns.fetch_add(int64_t(corr * 1e9), std::memory_order_acq_rel);
    s.corr_total += corr;
    s.locked = std::fabs(err) < 0.002;
    if (cfg.log_frames && frame_log::Enabled()) {
      frame_log::RecordUptime("vlock_vblank", frame, f.vblank, uint64_t(std::max(0.0, work) * 1e6));
      if (f.done > 0) frame_log::RecordUptime("vlock_done", frame, f.done, 0);
      frame_log::Record("vlock_err", frame, uint64_t(std::llround((err + period) * 1e6)));
    }
  }

  if (presented - s.window_start >= 5.0) {
    REXLOG_INFO(
        "[vsync-lock] {} {} refresh {:.2f} Hz frame {:.2f} ms | frames {} late {} (spikes {}) drains {} slips {} "
        "inactive {} untrusted {} gpu-timed {} | lead {:.2f} ms (work p95 {:.2f}, cliff {:+.2f}, latch {:.2f}) | |err| avg "
        "{:.3f} ms, done->photon avg {:.2f} ms, shift {:+.2f} ms",
        active ? "active" : "inactive", s.ext_mode ? "external" : s.ui_mode ? "ui-loop" : "immediate", 1.0 / period, frame_period * 1e3,
        s.n_frames, s.n_late, s.n_spike_late, s.n_drains, s.n_ext_slips, s.n_inactive, s.n_untrusted, s.n_done, s.lead * 1e3, w_max * 1e3,
        s.cliff * 1e3, s.latch * 1e3, s.n_err ? s.sum_abs_err / s.n_err * 1e3 : 0.0,
        s.n_err ? s.sum_ready_to_photon / s.n_err * 1e3 : 0.0, s.corr_total * 1e3);
    s.window_start = presented;
    s.n_ext_slips = 0;
    s.n_done = s.n_untrusted = s.n_frames = s.n_late = s.n_spike_late = s.n_drains = s.n_inactive = s.n_err = 0;
    s.sum_abs_err = s.sum_ready_to_photon = s.corr_total = 0;
  }
  s.w_bound = w_max;
  s.lead_atomic.store(s.lead, std::memory_order_relaxed);
}

bool DisplayGrid(double* last_vsync, double* period) {
  auto& s = Get();
  const double v = s.grid_vsync.load(std::memory_order_acquire);
  if (last_vsync) *last_vsync = v;
  if (period) *period = 1.0 / s.refresh_hz.load(std::memory_order_relaxed);
  return v > 0;
}

double NextVsyncAfter(double t) {
  double v = 0, period = 0;
  if (!DisplayGrid(&v, &period)) return 0;
  return v + (std::floor((t - v) / period) + 1) * period;
}

double Lead() { return Get().lead_atomic.load(std::memory_order_relaxed); }

}  // namespace rex::graphics::metal::vsync_lock
