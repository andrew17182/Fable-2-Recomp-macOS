// metal_frame_interp.mm - frame generation (see metal_frame_interp.h, docs/macos-metal.md "Frame generation").
// Manual reference counting (like metal_presenter.mm).

#include "rex/ui/metal/metal_frame_interp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <pthread.h>

#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_vsync_lock.h"
#include "rex/logging.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#if __has_include(<MetalFX/MTLFXFrameInterpolator.h>)
#import <MetalFX/MetalFX.h>
#define FI_AVAILABLE 1
#else
#define FI_AVAILABLE 0
#endif

namespace rex::ui::metal::frame_interp {

namespace {

std::atomic<bool> g_on{false};
std::atomic<bool> g_reset_schedule{false};  // set on each enable: the present thread starts a fresh cadence

// ---------------------------------------------------------------- interpolation (UI thread)

#if FI_AVAILABLE
id g_fi = nil;  // id<MTLFXFrameInterpolator> (macOS 26)
#endif
id<MTLTexture> g_color[2] = {nil, nil};  // indexed by guest frame id % 2
uint64_t g_color_frame[2] = {};
id<MTLTexture> g_out = nil, g_zero_motion = nil, g_flat_depth = nil;
uint32_t g_fi_w = 0, g_fi_h = 0;
double g_last_ready = 0, g_interval_ema = 0;

// Stats ([frame-interp] line every kStatEvery real frames).
constexpr uint32_t kStatEvery = 300;
uint32_t g_stat_frames = 0, g_stat_generated = 0;
std::atomic<uint64_t> g_gpu_us_sum{0}, g_gpu_us_max{0}, g_gpu_n{0};
std::atomic<uint32_t> g_dropped{0};  // generated frames dropped before presentation (queue full, slip, catch-up)
std::mutex g_present_mutex;          // the stats below (presented handlers, present thread)
double g_last_presented = 0;
double g_gap_sum[2] = {}, g_gap_sq[2] = {};  // gap before a generated / real present
uint32_t g_gap_n[2] = {};
double g_wait_sum = 0, g_wait_max = 0;  // present thread nextDrawable waits
uint32_t g_wait_n = 0;
double g_lat_sum = 0, g_lat_max = 0;  // real frame ready -> on screen
uint32_t g_lat_n = 0;

extern std::atomic<double> g_sched_lead;
extern std::atomic<uint32_t> g_sched_slips, g_sched_late, g_sched_pullins;

void MaybeLogStats() {
  if (++g_stat_frames < kStatEvery) return;
  const uint64_t n = g_gpu_n.exchange(0), sum = g_gpu_us_sum.exchange(0), mx = g_gpu_us_max.exchange(0);
  std::lock_guard<std::mutex> lock(g_present_mutex);
  auto mean = [](double s, uint32_t k) { return k ? s / k * 1000.0 : 0.0; };
  auto sd = [](double s, double q, uint32_t k) {
    if (!k) return 0.0;
    const double m = s / k;
    return std::sqrt(std::max(0.0, q / k - m * m)) * 1000.0;
  };
  REXLOG_INFO(
      "[frame-interp] real {} generated {} dropped {} | real interval {:.2f} ms | gpu mean {:.3f} ms max {:.3f} ms | "
      "gap before generated {:.2f}+-{:.2f} ms, before real {:.2f}+-{:.2f} ms | ready->on screen {:.2f} ms mean "
      "{:.2f} max | nextDrawable {:.2f} ms mean {:.2f} max | lead {:.2f} ms slips {} late {} pull-ins {}",
      g_stat_frames, g_stat_generated, g_dropped.exchange(0), g_interval_ema * 1000.0, n ? sum / 1000.0 / n : 0.0,
      mx / 1000.0, mean(g_gap_sum[0], g_gap_n[0]), sd(g_gap_sum[0], g_gap_sq[0], g_gap_n[0]),
      mean(g_gap_sum[1], g_gap_n[1]), sd(g_gap_sum[1], g_gap_sq[1], g_gap_n[1]), mean(g_lat_sum, g_lat_n),
      g_lat_max * 1000.0, mean(g_wait_sum, g_wait_n), g_wait_max * 1000.0, g_sched_lead.load() * 1000.0,
      g_sched_slips.exchange(0), g_sched_late.exchange(0), g_sched_pullins.exchange(0));
  g_stat_frames = g_stat_generated = 0;
  g_gap_sum[0] = g_gap_sum[1] = g_gap_sq[0] = g_gap_sq[1] = 0;
  g_gap_n[0] = g_gap_n[1] = 0;
  g_wait_sum = g_wait_max = 0;
  g_wait_n = 0;
  g_lat_sum = g_lat_max = 0;
  g_lat_n = 0;
}

void OnPresented(bool generated, double presented_s, double ready_s) {
  if (presented_s <= 0) return;
  std::lock_guard<std::mutex> lock(g_present_mutex);
  if (g_last_presented > 0 && presented_s > g_last_presented) {
    const double gap = presented_s - g_last_presented;
    if (gap < 0.2) {
      const int k = generated ? 0 : 1;
      g_gap_sum[k] += gap;
      g_gap_sq[k] += gap * gap;
      ++g_gap_n[k];
    }
  }
  g_last_presented = std::max(g_last_presented, presented_s);
  if (!generated && ready_s > 0 && presented_s > ready_s) {
    const double lat = presented_s - ready_s;
    g_lat_sum += lat;
    g_lat_max = std::max(g_lat_max, lat);
    ++g_lat_n;
  }
}

void NoteDrawableWait(double seconds) {
  std::lock_guard<std::mutex> lock(g_present_mutex);
  g_wait_sum += seconds;
  g_wait_max = std::max(g_wait_max, seconds);
  ++g_wait_n;
}

// Present log (dev: FABLE2_FRAME_INTERP_PRESENT_LOG=<file>): one CSV row per present with its presentedTime.
FILE* PresentLog() {
  static FILE* f = [] {
    const char* e = std::getenv("FABLE2_FRAME_INTERP_PRESENT_LOG");  // getenv-ok: once
    FILE* file = e && *e ? std::fopen(e, "w") : nullptr;
    if (file) std::fprintf(file, "kind,frame,taken,target,presented,ready,arrived\n");
    return file;
  }();
  return f;
}
std::mutex g_log_mutex;
void LogPresent(const char* kind, uint64_t frame, double taken, double target, double presented, double ready,
                double arrived) {
  FILE* f = PresentLog();
  if (!f) return;
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::fprintf(f, "%s,%llu,%.6f,%.6f,%.6f,%.6f,%.6f\n", kind, (unsigned long long)frame, taken, target, presented,
               ready, arrived);
  static int n = 0;
  if (++n % 60 == 0) std::fflush(f);
}

// ---------------------------------------------------------------- presentation (present thread)

constexpr int kTargets = 6;
struct Target {
  id<MTLTexture> tex = nil;
  bool busy = false;
};
Target g_targets[kTargets];
struct Job {
  int idx;
  bool generated;
  double ready;
  uint64_t frame_id;
  uint64_t lock_frame;
  double submitted;  // CACurrentMediaTime at SubmitPresent (the pair's arrival)
  CAMetalLayer* layer;
  id<MTLCommandQueue> queue;
};
std::mutex g_job_mutex;  // g_targets, g_jobs, g_stop, g_ticks
std::condition_variable g_job_cv;
std::deque<Job> g_jobs;
std::thread g_thread;
bool g_stop = false;
bool g_busy = false;  // the present thread is handling a job
std::condition_variable g_tick_cv;
uint64_t g_ticks = 0;

void FreeTarget(int idx) {
  std::lock_guard<std::mutex> lock(g_job_mutex);
  g_targets[idx].busy = false;
}

// ---- Grid scheduling. Every present gets an explicit display time on the display's vsync grid (presentAtTime): per
// real frame the generated frame goes on vsync N and the real one on N + k/2 (k = refreshes per real frame: 2 for
// 30 -> 60 on a 60 Hz display). N is the next planned slot (the previous pair's N + k), or, when the frame came too
// late for it, the first vsync at least `lead` after the drawable was taken (a slip: the previous real frame stays
// up longer). WindowServer needs a present a little over two refreshes before its display time, and with 3
// drawables at 60 presents/s a drawable comes free about three refreshes before the vsync it will be shown on; so a
// pair that arrives early waits for a drawable, and a backlog (after a hitch elsewhere) never drains by itself.
// When pairs keep arriving a refresh earlier than their slot needs, one generated frame is skipped and the real
// frame takes its slot (catch-up: one refresh less latency from then on). The vsync lock (OnExternalSchedule) moves
// the guest vblank so pairs arrive just before the slot deadline (target - lead).
// Present thread only:
double g_grid = 0;          // a known vsync (latest presentedTime), mach seconds
double g_slot_vsync = 0;    // planned vsync of the next real frame's first present
double g_pair_target = 0;   // vsync of the current pair's first present
int g_pair_half = 1;        // refreshes from the pair's first present to its real frame
bool g_pair_open = false;   // a generated frame was scheduled (or skipped); its real frame follows
int g_slip_run = 0;         // consecutive pairs too late for their slot
int g_behind_run = 0;       // consecutive pairs that arrived a refresh earlier than their slot needed
int g_catchup_need = 3;     // behind_run that triggers a catch-up; doubles when a catch-up is followed by a slip
int g_pairs_since_catchup = 1 << 20;
std::atomic<double> g_grid_atomic{0.0};
std::atomic<double> g_sched_lead{0.0};  // commit -> display lead needed (learned), seconds
std::atomic<uint32_t> g_sched_slips{0}, g_sched_late{0}, g_sched_pullins{0};
std::atomic<uint64_t> g_presents_ok{0};
double g_lead_floor = 0.0;  // lowest lead: two refreshes (or a lower FABLE2_FRAME_INTERP_LEAD_MS)

double Period() {
  double period = 0, unused = 0;
  graphics::metal::vsync_lock::DisplayGrid(&unused, &period);
  return period > 0 ? period : 1.0 / 60.0;
}

double SchedLead(double period) {
  double lead = g_sched_lead.load(std::memory_order_relaxed);
  if (lead > 0) return lead;
  // Dev tuning: FABLE2_FRAME_INTERP_LEAD_MS = initial lead.
  static const double ms = [] {
    const char* e = std::getenv("FABLE2_FRAME_INTERP_LEAD_MS");  // getenv-ok: once
    return e ? std::strtod(e, nullptr) : 0.0;
  }();
  // Measured at 60 Hz: a 32 ms lead was a refresh late, 33.3+ on time.
  lead = ms > 0 ? ms / 1000.0 : 2 * period + 0.0015;
  g_lead_floor = std::min(2 * period, lead);
  g_sched_lead.store(lead, std::memory_order_relaxed);
  return lead;
}

double CeilGrid(double t, double period) { return g_grid + std::ceil((t - g_grid) / period - 1e-6) * period; }

int RefreshesPerFrame(double period) {
  const double interval = g_interval_ema > 0 ? g_interval_ema : 2 * period;
  return std::max(1, int(std::lround(interval / period)));
}

// Catch-up check for a generated job, before taking a drawable: true = skip it (its real frame takes its slot).
bool SkipForCatchUp(const Job& job) {
  if (g_grid <= 0 || g_slot_vsync <= 0 || !job.generated) return false;
  const double period = Period();
  const double ideal = CeilGrid(job.submitted + SchedLead(period), period);
  if (g_slot_vsync - ideal < 0.5 * period) {
    g_behind_run = 0;
    return false;
  }
  if (++g_behind_run < g_catchup_need) return false;
  g_behind_run = 0;
  g_pairs_since_catchup = 0;
  g_sched_pullins.fetch_add(1, std::memory_order_relaxed);
  g_pair_open = true;
  g_pair_target = g_slot_vsync;
  g_pair_half = 0;
  g_slot_vsync += (RefreshesPerFrame(period) - 1) * period;
  return true;
}

// Display time for a job (0 = present as soon as possible, -1 = drop it). taken = when its drawable was taken.
double ScheduleJob(const Job& job, double taken, double* deadline, bool* slipped) {
  *deadline = 0;
  *slipped = false;
  if (g_grid <= 0) return 0.0;
  const double period = Period();
  const int k = RefreshesPerFrame(period);
  if (!job.generated && g_pair_open) {
    // The real frame of the pair: half a real interval after its generated frame (or in its slot after a skip).
    g_pair_open = false;
    const double target = g_pair_target + g_pair_half * period;
    return std::max(target, CeilGrid(taken + SchedLead(period), period));
  }
  const double lead = SchedLead(period);
  const double feasible = CeilGrid(taken + lead, period);
  double target = feasible;
  if (g_slot_vsync > 0) {
    const int half = std::max(1, k / 2);
    if (feasible <= g_slot_vsync + 0.5 * period) {
      target = g_slot_vsync;
      g_slip_run = 0;
    } else if (job.generated && g_slip_run++ == 0 && feasible <= g_slot_vsync + (half + 0.5) * period) {
      // Too late for its slot but in time for the real frame's: drop the generated frame and keep the cadence (the
      // previous real frame stays up one refresh longer; nothing after it moves). A second slip in a row means the
      // pairs now arrive a refresh later than the cadence (the vsync lock aligns arrivals only modulo one refresh),
      // so the next one re-anchors the cadence instead.
      *slipped = true;
      *deadline = g_slot_vsync - lead;
      g_sched_slips.fetch_add(1, std::memory_order_relaxed);
      g_pair_open = true;
      g_pair_target = g_slot_vsync;
      g_pair_half = half;
      g_slot_vsync += k * period;
      return -1.0;
    } else {
      *slipped = true;
      g_behind_run = 0;
      g_slip_run = 0;
      g_sched_slips.fetch_add(1, std::memory_order_relaxed);
      // A slip soon after a catch-up: the earlier slot was not sustainable (drawables), back off.
      if (g_pairs_since_catchup < 30) g_catchup_need = std::min(g_catchup_need * 2, 960);
    }
  }
  if (++g_pairs_since_catchup == 600) g_catchup_need = 3;
  *deadline = target - lead;
  g_slot_vsync = target + k * period;
  if (job.generated) {
    g_pair_open = true;
    g_pair_target = target;
    g_pair_half = std::max(1, k / 2);
  } else {
    g_pair_open = false;
  }
  return target;
}

void OnScheduledPresented(double target, double taken, double presented) {
  if (presented <= 0) return;
  const double prev = g_grid_atomic.load(std::memory_order_relaxed);
  if (presented > prev) g_grid_atomic.store(presented, std::memory_order_relaxed);
  if (target <= 0) return;
  const double period = Period();
  const double lead = g_sched_lead.load(std::memory_order_relaxed);
  if (presented > target + 0.5 * period) {
    g_sched_late.fetch_add(1, std::memory_order_relaxed);
    g_presents_ok.store(0, std::memory_order_relaxed);
    // Late with a lead near the current one: the compositor needs more. A late present with a longer lead is a
    // hitch elsewhere (every present queued behind it is late too) or a session where WindowServer takes a refresh
    // longer; raising the lead to chase those starves the 3 drawables (measured: stuck at 30 FPS), so it is left.
    if (target - taken < lead + 0.002) {
      g_sched_lead.store(std::min(2 * period + 0.006, lead + 0.001), std::memory_order_relaxed);
    }
  } else if (g_presents_ok.fetch_add(1, std::memory_order_relaxed) + 1 >= 600) {
    // 10 s on time: try a little less lead (not below two refreshes, or a lower FABLE2_FRAME_INTERP_LEAD_MS).
    g_presents_ok.store(0, std::memory_order_relaxed);
    g_sched_lead.store(std::max(g_lead_floor > 0 ? g_lead_floor : 2 * period, lead - 0.00025),
                       std::memory_order_relaxed);
  }
}

void PresentThread() {
  pthread_setname_np("FrameInterpPresent");
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(g_job_mutex);
      g_job_cv.wait(lock, [] { return g_stop || !g_jobs.empty(); });
      if (g_stop) return;
      job = g_jobs.front();
      g_jobs.pop_front();
      g_busy = true;
    }
    struct Idle {
      ~Idle() {
        {
          std::lock_guard<std::mutex> lock(g_job_mutex);
          g_busy = false;
        }
        g_tick_cv.notify_all();
      }
    } idle;
    @autoreleasepool {
      g_grid = g_grid_atomic.load(std::memory_order_relaxed);
      if (g_reset_schedule.exchange(false)) {
        // Turned on again (F10): plan a new cadence from this pair; the learned lead stays.
        g_slot_vsync = g_pair_target = 0;
        g_pair_open = false;
        g_slip_run = g_behind_run = 0;
        g_catchup_need = 3;
        g_pairs_since_catchup = 1 << 20;
      }
      if (SkipForCatchUp(job)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        FreeTarget(job.idx);
        continue;
      }
      const double t0 = CACurrentMediaTime();
      id<CAMetalDrawable> drawable = [job.layer nextDrawable];
      const double taken = CACurrentMediaTime();
      NoteDrawableWait(taken - t0);
      id<MTLTexture> src = g_targets[job.idx].tex;
      if (!drawable || drawable.texture.width != src.width || drawable.texture.height != src.height ||
          drawable.texture.framebufferOnly) {
        if (!job.generated) g_pair_open = false;
        FreeTarget(job.idx);
        continue;
      }
      double deadline = 0;
      bool slipped = false;
      const double target = ScheduleJob(job, taken, &deadline, &slipped);
      if (target != 0 && deadline > 0 && job.lock_frame) {
        // The lock times the pair's arrival (the UI thread's submit of its first present), against a deadline
        // kSafety early: arrivals jitter by a millisecond or two (a late one costs a generated frame).
        constexpr double kSafety = 0.002;
        graphics::metal::vsync_lock::OnExternalSchedule(job.lock_frame, job.submitted, deadline - kSafety, slipped);
      }
      if (target < 0) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        FreeTarget(job.idx);  // generated frame dropped (slip); the drawable goes back unpresented
        continue;
      }
      id<MTLCommandBuffer> cb = [job.queue commandBuffer];
      cb.label = @"FrameInterpPresent";
      id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
      [blit copyFromTexture:src toTexture:drawable.texture];
      [blit endEncoding];
      const bool generated = job.generated;
      const double ready = job.ready;
      const double submitted = job.submitted;
      const uint64_t frame_id = job.frame_id;
      const uint64_t lock_frame = job.lock_frame;
      [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
        const double presented = shown.presentedTime;
        OnScheduledPresented(target, taken, presented);
        OnPresented(generated, presented, ready);
        LogPresent(generated ? "gen" : "real", lock_frame, taken, target, presented, ready, submitted);
        if (!generated && lock_frame) graphics::metal::vsync_lock::OnPresented(lock_frame, presented);
        if (frame_id && graphics::metal::frame_log::Enabled()) {
          graphics::metal::frame_log::Presented(frame_id, presented);
        }
      }];
      const int idx = job.idx;
      [cb addCompletedHandler:^(id<MTLCommandBuffer>) { FreeTarget(idx); }];
      if (target > 0) {
        // A tenth of a refresh before the vsync, so rounding can only land on it.
        [cb presentDrawable:drawable atTime:target - 0.1 * Period()];
      } else {
        [cb presentDrawable:drawable];
      }
      [cb commit];
      {
        std::lock_guard<std::mutex> lock(g_job_mutex);
        ++g_ticks;
      }
      g_tick_cv.notify_all();
    }
  }
}

#if FI_AVAILABLE
// Creates the interpolator and its textures for a w x h guest output; false (and frame generation off) on failure.
API_AVAILABLE(macos(26.0))
bool EnsureInterpolator(id<MTLDevice> device, MTLPixelFormat format, uint32_t w, uint32_t h) {
  if (g_fi && g_fi_w == w && g_fi_h == h) return true;
  [g_fi release];
  g_fi = nil;
  for (auto& t : g_color) {
    [t release];
    t = nil;
  }
  for (id<MTLTexture>* t : {&g_out, &g_zero_motion, &g_flat_depth}) {
    [*t release];
    *t = nil;
  }
  g_color_frame[0] = g_color_frame[1] = 0;
  MTLFXFrameInterpolatorDescriptor* desc = [MTLFXFrameInterpolatorDescriptor new];
  desc.colorTextureFormat = format;
  desc.outputTextureFormat = format;
  desc.depthTextureFormat = MTLPixelFormatR32Float;
  desc.motionTextureFormat = MTLPixelFormatRG16Float;
  desc.inputWidth = w;
  desc.inputHeight = h;
  desc.outputWidth = w;
  desc.outputHeight = h;
  id<MTLFXFrameInterpolator> fi = [desc newFrameInterpolatorWithDevice:device];
  [desc release];
  if (!fi) {
    REXLOG_WARN("[frame-interp] MTLFXFrameInterpolator {}x{} creation failed; frame generation off", w, h);
    return false;
  }
  g_fi = fi;
  g_fi_w = w;
  g_fi_h = h;
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                width:w
                                                                               height:h
                                                                            mipmapped:NO];
  td.storageMode = MTLStorageModePrivate;
  td.usage = fi.colorTextureUsage | MTLTextureUsageShaderRead;
  for (auto& t : g_color) t = [device newTextureWithDescriptor:td];
  td.usage = fi.outputTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
  g_out = [device newTextureWithDescriptor:td];
  // Colour-only inputs: zero motion and a flat depth (MetalFX estimates the motion from the two colour frames).
  MTLTextureDescriptor* zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                                                                                width:w
                                                                               height:h
                                                                            mipmapped:NO];
  zd.storageMode = MTLStorageModeShared;
  zd.usage = MTLTextureUsageShaderRead;
  g_zero_motion = [device newTextureWithDescriptor:zd];
  std::vector<uint8_t> zeros(size_t(w) * h * 4, 0);
  [g_zero_motion replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:zeros.data() bytesPerRow:w * 4];
  zd.pixelFormat = MTLPixelFormatR32Float;
  g_flat_depth = [device newTextureWithDescriptor:zd];
  std::vector<float> flat(size_t(w) * h, 0.01f);  // reversed z: a plane about 10 units away
  [g_flat_depth replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:flat.data() bytesPerRow:w * 4];
  if (!g_color[0] || !g_color[1] || !g_out || !g_zero_motion || !g_flat_depth) {
    REXLOG_WARN("[frame-interp] texture allocation failed; frame generation off");
    return false;
  }
  REXLOG_INFO("[frame-interp] MTLFXFrameInterpolator {}x{} format {} created (colour inputs)", w, h, uint32_t(format));
  return true;
}
#endif

}  // namespace

bool Enabled() { return g_on.load(std::memory_order_relaxed); }
double Now() { return CACurrentMediaTime(); }

void* Process(void* device_v, void* queue_v, void* color_v, uint64_t frame_id, void* wait_event_v,
              uint64_t wait_value, double ready_s) {
  id<MTLDevice> device = (id<MTLDevice>)device_v;
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>)queue_v;
  id<MTLTexture> color = (id<MTLTexture>)color_v;
  if (g_last_ready > 0) {
    const double interval = ready_s - g_last_ready;
    if (interval > 1.0 / 250.0 && interval < 0.2) {
      g_interval_ema = g_interval_ema > 0 ? g_interval_ema * 0.9 + interval * 0.1 : interval;
    }
  }
  g_last_ready = ready_s;
  if (!color || !Enabled()) return nullptr;
  MaybeLogStats();
#if FI_AVAILABLE
  if (@available(macOS 26.0, *)) {
    const uint32_t w = uint32_t(color.width), h = uint32_t(color.height);
    if (!EnsureInterpolator(device, color.pixelFormat, w, h)) {
      g_on.store(false, std::memory_order_relaxed);
      return nullptr;
    }
    id<MTLFXFrameInterpolator> fi = (id<MTLFXFrameInterpolator>)g_fi;
    const int cur = int(frame_id % 2), prev = int((frame_id + 1) % 2);
    const bool consecutive = frame_id > 1 && g_color_frame[prev] == frame_id - 1;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    cb.label = @"FrameInterp";
    if (wait_event_v) [cb encodeWaitForEvent:(id<MTLSharedEvent>)wait_event_v value:wait_value];
    g_color_frame[cur] = frame_id;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:color toTexture:g_color[cur]];
    [blit endEncoding];
    const bool generate = consecutive && g_interval_ema > 0;
    if (generate) {
      fi.colorTexture = g_color[cur];
      fi.prevColorTexture = g_color[prev];
      fi.depthTexture = g_flat_depth;
      fi.motionTexture = g_zero_motion;
      fi.outputTexture = g_out;
      fi.motionVectorScaleX = 1.0f;
      fi.motionVectorScaleY = 1.0f;
      fi.deltaTime = float(g_interval_ema);
      fi.nearPlane = 0.1f;
      fi.farPlane = 5000.0f;
      fi.fieldOfView = 60.0f;
      fi.aspectRatio = float(w) / float(h);
      fi.jitterOffsetX = 0.0f;
      fi.jitterOffsetY = 0.0f;
      fi.depthReversed = YES;
      fi.shouldResetHistory = NO;
      fi.uiTexture = nil;
      [fi encodeToCommandBuffer:cb];
      ++g_stat_generated;
    }
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
      const double s = done.GPUStartTime, e = done.GPUEndTime;
      if (s > 0 && e >= s) {
        const uint64_t us = uint64_t((e - s) * 1e6);
        g_gpu_us_sum += us;
        ++g_gpu_n;
        uint64_t m = g_gpu_us_max.load();
        while (us > m && !g_gpu_us_max.compare_exchange_weak(m, us)) {
        }
      }
    }];
    [cb commit];
    return generate ? (void*)g_out : nullptr;
  }
#endif
  return nullptr;
}

void* AcquireTarget(void* device_v, uint32_t width, uint32_t height) {
  id<MTLDevice> device = (id<MTLDevice>)device_v;
  if (!device || !width || !height) return nullptr;
  std::lock_guard<std::mutex> lock(g_job_mutex);
  int idx = -1;
  for (int i = 0; i < kTargets; ++i) {
    if (!g_targets[i].busy) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    // Behind: drop the oldest queued generated frame and reuse its target.
    for (auto it = g_jobs.begin(); it != g_jobs.end(); ++it) {
      if (it->generated) {
        idx = it->idx;
        g_jobs.erase(it);
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
    if (idx < 0) return nullptr;
  }
  Target& t = g_targets[idx];
  if (!t.tex || t.tex.width != width || t.tex.height != height) {
    [t.tex release];
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    t.tex = [device newTextureWithDescriptor:d];
    if (!t.tex) return nullptr;
  }
  t.busy = true;
  return (void*)t.tex;
}

void SubmitPresent(void* layer_v, void* queue_v, void* target, bool generated, double ready_s, uint64_t frame_id,
                   uint64_t lock_frame) {
  CAMetalLayer* layer = (CAMetalLayer*)layer_v;
  if (layer.framebufferOnly) layer.framebufferOnly = NO;  // the present thread copies into the drawable
  // 3 drawables: each present is committed two refreshes ahead of its display time, so with 2 the present thread
  // hands over only 20 presents/s (measured 2026-10-22: 30 unique/s, 136 ms latency).
  if (layer.maximumDrawableCount != 3) {
    layer.maximumDrawableCount = 3;
    REXLOG_INFO("[present] maximumDrawableCount = 3 (frame generation)");
  }
  std::lock_guard<std::mutex> lock(g_job_mutex);
  int idx = -1;
  for (int i = 0; i < kTargets; ++i) {
    if ((void*)g_targets[i].tex == target) idx = i;
  }
  if (idx < 0) return;
  if (!g_thread.joinable()) {
    g_stop = false;
    g_thread = std::thread(PresentThread);
  }
  // Keep at most two frames waiting: drop the oldest generated one first.
  while (g_jobs.size() >= 2) {
    auto it = g_jobs.begin();
    while (it != g_jobs.end() && !it->generated) ++it;
    if (it == g_jobs.end()) it = g_jobs.begin();
    g_targets[it->idx].busy = false;
    g_jobs.erase(it);
    g_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  g_jobs.push_back(Job{idx, generated, ready_s, frame_id, lock_frame, CACurrentMediaTime(), layer,
                       (id<MTLCommandQueue>)queue_v});
  g_job_cv.notify_one();
}

void WaitPresentTick(double timeout_s) {
  std::unique_lock<std::mutex> lock(g_job_mutex);
  const uint64_t start = g_ticks;
  g_tick_cv.wait_for(lock, std::chrono::microseconds(int64_t(timeout_s * 1e6)),
                     [start] { return g_ticks != start || g_stop; });
}

void NotifyNewGuestFrame() {
  {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    ++g_ticks;
  }
  g_tick_cv.notify_all();
}

void Drain(double timeout_s) {
  std::unique_lock<std::mutex> lock(g_job_mutex);
  g_tick_cv.wait_for(lock, std::chrono::microseconds(int64_t(timeout_s * 1e6)),
                     [] { return (g_jobs.empty() && !g_busy) || g_stop; });
}

void StopPresentThread() {
  {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    g_stop = true;
    for (const Job& j : g_jobs) g_targets[j.idx].busy = false;
    g_jobs.clear();
  }
  g_job_cv.notify_all();
  g_tick_cv.notify_all();
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace rex::ui::metal::frame_interp

// The game (src/core/fable2_frame_gen.h, via dlsym) turns frame generation on or off once, after its frame-rate
// decision, and from F10 / the controller toggle (UI thread). op: 1 = on, 0 = off, 2 = query support only.
// Returns 1 = on (or supported, for 2), 0 = off, -1 = needs macOS 26, -2 = this GPU does not support MetalFX frame
// interpolation. The interpolator is created on the first new frame after turning on; turning off goes back to the
// UI thread's own presents after the queued ones (metal_presenter.mm).
extern "C" __attribute__((visibility("default"), used)) int fable2_frame_interp_enable(int op) {
  using namespace rex::ui::metal::frame_interp;
  if (op == 0) {
    g_on.store(false, std::memory_order_relaxed);
    return 0;
  }
#if FI_AVAILABLE
  if (@available(macOS 26.0, *)) {
    static const bool supported = [] {
      id<MTLDevice> device = MTLCreateSystemDefaultDevice();
      const bool ok = device && [MTLFXFrameInterpolatorDescriptor supportsDevice:device];
      [device release];
      return ok;
    }();
    if (!supported) return -2;
    if (op == 1 && !g_on.load(std::memory_order_relaxed)) {
      g_reset_schedule.store(true);
      g_on.store(true, std::memory_order_relaxed);
    }
    return 1;
  }
#endif
  return -1;
}
