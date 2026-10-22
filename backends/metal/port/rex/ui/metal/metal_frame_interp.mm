// metal_frame_interp.mm - MetalFX frame interpolation prototype (see metal_frame_interp.h).
// Manual reference counting (like metal_presenter.mm).

#include "rex/ui/metal/metal_frame_interp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include <unistd.h>

#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_vsync_lock.h"
#include "rex/logging.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
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

const char* EnvValue() {
  static const char* v = std::getenv("FABLE2_FRAME_INTERP");  // getenv-ok: once
  return v ? v : "";
}

// ---------------------------------------------------------------- scene inputs (guest GPU thread)

struct Candidate {
  float m[16];
  float vport[6];
  uint32_t count;
};
constexpr uint32_t kCandidates = 32;
Candidate g_candidates[kCandidates];
uint32_t g_candidate_count = 0;
uint64_t g_scene_encodes = 0;

struct Slot {
  id<MTLTexture> depth = nil;   // R32Float, guest output size
  id<MTLTexture> motion = nil;  // RG16Float, pixels to the previous frame
  uint64_t frame_id = 0;        // 0 = not committed
  float vp[16] = {};
  float prev_vp[16] = {};  // the matrix the motion was computed against (diagnostic dump)
  float vport[6] = {};
  bool reset = true;
  float near_plane = 0.1f, far_plane = 1000.0f, fov_deg = 60.0f, aspect = 16.0f / 9.0f;
  bool reversed = false;
};
constexpr int kSlots = 4;
Slot g_slots[kSlots];
std::mutex g_slot_mutex;  // frame_id / params (textures are only replaced at a size change)
int g_pending = -1;       // slot written this frame, not committed yet
int g_next_slot = 0;
bool g_have_prev_vp = false;
float g_prev_vp[16];
bool g_have_prev2_vp = false;  // eval mode: two frames back
float g_prev2_vp[16];

// FABLE2_FRAME_INTERP_EVAL=1 (with FABLE2_FRAME_INTERP_DUMP): quality check against ground truth. Each frame N is
// interpolated from N-2 and N (motion over two frames) and dumped with the real N-1 it should reproduce; the
// generated frames are not presented.
// What the interpolator is given besides the two colour frames (FABLE2_FRAME_INTERP_INPUTS):
//   color  (default) zero motion and a constant depth: MetalFX estimates the motion itself. No game data.
//   depth  zero motion, the game's scene depth.
//   camera camera-reprojection motion vectors and the game's scene depth.
enum class Inputs { kColor, kDepth, kCamera };
Inputs ParseInputs(const char* v, Inputs fallback) {
  if (!v) return fallback;
  if (std::strcmp(v, "color") == 0) return Inputs::kColor;
  if (std::strcmp(v, "depth") == 0) return Inputs::kDepth;
  if (std::strcmp(v, "camera") == 0) return Inputs::kCamera;
  return fallback;
}
const char* InputsName(Inputs i) {
  return i == Inputs::kColor ? "color" : i == Inputs::kDepth ? "depth" : "camera";
}
int EvalLevel();
Inputs ModeA() {
  static const Inputs v = ParseInputs(std::getenv("FABLE2_FRAME_INTERP_INPUTS"), Inputs::kColor);  // getenv-ok: once
  return v;
}
// EVAL=2: a second interpolator with these inputs (FABLE2_FRAME_INTERP_EVAL_B, default camera), dumped beside A.
bool HaveArmB() { return EvalLevel() >= 2; }
Inputs ModeB() {
  static const Inputs v = ParseInputs(std::getenv("FABLE2_FRAME_INTERP_EVAL_B"), Inputs::kCamera);  // getenv-ok: once
  return v;
}
bool SceneMotionWanted() { return ModeA() == Inputs::kCamera || (HaveArmB() && ModeB() == Inputs::kCamera); }

int EvalLevel() {
  static const int v = [] {
    const char* e = std::getenv("FABLE2_FRAME_INTERP_EVAL");  // getenv-ok: once
    return e ? std::atoi(e) : 0;
  }();
  return v;
}
bool EvalMode() { return EvalLevel() > 0; }
std::atomic<uint32_t> g_out_w{0}, g_out_h{0};
id<MTLComputePipelineState> g_prep_ms = nil, g_prep_2d = nil;
bool g_pipelines_failed = false;

const char* kPrepSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Params {
  float4x4 reproj;   // current NDC (homogeneous) -> previous clip
  float4 vport;      // xscale, xoffset, yscale, yoffset (guest pixels)
  float2 zso;        // zscale, zoffset
  float2 scene;      // scene size in guest pixels (viewport extent)
  float2 content;    // scene size in depth texture pixels
  float2 outsz;      // output (guest output) size
  uint zero_motion;
  float depth_scale;  // guest window z per host depth unit
};
static float2 motion_of(float d, float2 uv, constant Params& p) {
  if (p.zero_motion != 0) return float2(0.0);
  float2 win = uv * p.scene;
  float3 ndc = float3((win.x - p.vport.y) / p.vport.x, (win.y - p.vport.w) / p.vport.z,
                      (d - p.zso.y) / p.zso.x);
  float4 prev = p.reproj * float4(ndc, 1.0);
  if (abs(prev.w) < 1e-6) return float2(0.0);
  float2 pndc = prev.xy / prev.w;
  float2 pwin = float2(pndc.x * p.vport.x + p.vport.y, pndc.y * p.vport.z + p.vport.w);
  float2 puv = pwin / p.scene;
  return (puv - uv) * p.outsz;
}
kernel void prep_ms(depth2d_ms<float> src [[texture(0)]],
                    texture2d<float, access::write> dout [[texture(1)]],
                    texture2d<float, access::write> mout [[texture(2)]],
                    constant Params& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(p.outsz.x) || gid.y >= uint(p.outsz.y)) return;
  float2 uv = (float2(gid) + 0.5) / p.outsz;
  uint2 sc = min(uint2(uv * p.content), uint2(p.content) - 1);
  float d = src.read(sc, 0) * p.depth_scale;
  dout.write(float4(d, 0, 0, 0), gid);
  mout.write(float4(motion_of(d, uv, p), 0, 0), gid);
}
kernel void prep_2d(depth2d<float, access::read> src [[texture(0)]],
                    texture2d<float, access::write> dout [[texture(1)]],
                    texture2d<float, access::write> mout [[texture(2)]],
                    constant Params& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(p.outsz.x) || gid.y >= uint(p.outsz.y)) return;
  float2 uv = (float2(gid) + 0.5) / p.outsz;
  uint2 sc = min(uint2(uv * p.content), uint2(p.content) - 1);
  float d = src.read(sc) * p.depth_scale;
  dout.write(float4(d, 0, 0, 0), gid);
  mout.write(float4(motion_of(d, uv, p), 0, 0), gid);
}
)MSL";

struct PrepParams {
  float reproj[16];  // column-major
  float vport[4];
  float zso[2];
  float scene[2];
  float content[2];
  float outsz[2];
  uint32_t zero_motion;
  float depth_scale;
  uint32_t pad[2];
};

bool EnsurePipelines(id<MTLDevice> device) {
  if (g_prep_ms && g_prep_2d) return true;
  if (g_pipelines_failed) return false;
  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kPrepSource]
                                                options:nil
                                                  error:&error];
  if (!library) {
    REXLOG_ERROR("[frame-interp] prep shader compile failed: {}",
                 error ? error.localizedDescription.UTF8String : "?");
    g_pipelines_failed = true;
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    id<MTLFunction> fn = [library newFunctionWithName:i ? @"prep_2d" : @"prep_ms"];
    id<MTLComputePipelineState> ps = fn ? [device newComputePipelineStateWithFunction:fn error:&error] : nil;
    [fn release];
    if (!ps) {
      REXLOG_ERROR("[frame-interp] prep pipeline failed");
      g_pipelines_failed = true;
      [library release];
      return false;
    }
    (i ? g_prep_2d : g_prep_ms) = ps;
  }
  [library release];
  return true;
}

// 4x4 helpers, row-major m[r*4+c].
void Mul(const double* a, const double* b, double* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = s;
    }
}
bool Invert(const double* m, double* inv) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int piv = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
    if (std::fabs(a[piv][c]) < 1e-12) return false;
    if (piv != c)
      for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[piv][k]);
    const double d = a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = a[r][c];
        for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) inv[r * 4 + c] = a[r][c + 4];
  return true;
}

// Camera parameters from a combined view-projection (rows: clip x, y, z, w), assuming an orthonormal view.
void CameraFromVp(const float* m, const float* vport, Slot& s) {
  auto len3 = [](const float* r) { return std::sqrt(double(r[0]) * r[0] + double(r[1]) * r[1] + double(r[2]) * r[2]); };
  const double lw = len3(m + 12);
  if (lw < 1e-9) return;
  const double l0 = len3(m) / lw, l1 = len3(m + 4) / lw;
  if (l1 > 1e-9) s.fov_deg = float(2.0 * std::atan(1.0 / l1) * 180.0 / M_PI);
  if (l0 > 1e-9) s.aspect = float(l1 / l0);
  const double A = (double(m[8]) * m[12] + double(m[9]) * m[13] + double(m[10]) * m[14]) / (lw * lw);
  const double B = m[11] / lw - A * m[15] / lw;
  const double zs = vport[4], zo = vport[5];
  auto vz_at = [&](double window_z) {
    const double ndc = (window_z - zo) / (zs != 0 ? zs : 1);
    const double den = ndc - A;
    return std::fabs(den) < 1e-12 ? 1e7 : B / den;
  };
  double v0 = vz_at(0.0), v1 = vz_at(1.0);
  if (!(v0 > 0) || v0 > 1e7) v0 = 1e7;
  if (!(v1 > 0) || v1 > 1e7) v1 = 1e7;
  s.near_plane = float(std::min(v0, v1));
  s.far_plane = float(std::max(v0, v1));
  s.reversed = v1 < v0;  // window z 1 is nearer
}

// ---------------------------------------------------------------- presenter state (UI thread)

#if FI_AVAILABLE
id<MTLFXFrameInterpolator> g_fi = nil;
id<MTLFXFrameInterpolator> g_fi0 = nil;  // EVAL=2: a second interpolator fed zero motion (paired A/B)
id<MTLTexture> g_out0 = nil, g_zero_motion = nil, g_flat_depth = nil;
#endif
id<MTLTexture> g_color[3] = {nil, nil, nil};  // indexed by guest frame id % 3
uint64_t g_color_frame[3] = {};
id<MTLTexture> g_out = nil;
uint32_t g_fi_w = 0, g_fi_h = 0;
bool g_have_prev_color = false;
uint64_t g_last_frame = 0;
double g_last_ready = 0, g_interval_ema = 0;
bool g_creation_failed = false;

// Stats (logged every kStatEvery real frames).
constexpr uint32_t kStatEvery = 150;
uint32_t g_stat_frames = 0, g_stat_generated = 0, g_stat_no_inputs = 0;
std::atomic<uint64_t> g_gpu_us_sum{0}, g_gpu_us_max{0}, g_gpu_n{0};
std::mutex g_present_mutex;
double g_last_presented = 0;
double g_gap_sum[2] = {}, g_gap_sq[2] = {};  // gap before a generated / real present
uint32_t g_gap_n[2] = {};
double g_wait_sum[2] = {}, g_wait_max[2] = {};
uint32_t g_wait_n[2] = {};
double g_lat_sum = 0, g_lat_max = 0;
uint32_t g_lat_n = 0;

// Frame dump (FABLE2_FRAME_INTERP_DUMP=<dir>; write a count into <dir>/trigger to dump that many frames).
const char* DumpDir() {
  static const char* v = std::getenv("FABLE2_FRAME_INTERP_DUMP");  // getenv-ok: once
  return v;
}
uint32_t g_dump_remaining = 0;
uint32_t g_dropped = 0;  // generated/real frames dropped by the present queue (cumulative)

bool WritePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  bool ok = false;
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, rgba.data(), rgba.size(), nullptr);
  CGImageRef image = (space && provider) ? CGImageCreate(w, h, 8, 32, size_t(w) * 4, space,
                                                         CGBitmapInfo(kCGImageAlphaNoneSkipLast), provider,
                                                         nullptr, false, kCGRenderingIntentDefault)
                                         : nullptr;
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.data()),
                                                         CFIndex(path.size()), false);
  CGImageDestinationRef dest =
      (image && url) ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr) : nullptr;
  if (dest) {
    CGImageDestinationAddImage(dest, image, nullptr);
    ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
  }
  if (url) CFRelease(url);
  if (image) CGImageRelease(image);
  if (provider) CGDataProviderRelease(provider);
  if (space) CGColorSpaceRelease(space);
  return ok;
}

id<MTLBuffer> BlitToBuffer(id<MTLDevice> device, id<MTLBlitCommandEncoder> blit, id<MTLTexture> t, uint32_t bpp) {
  const NSUInteger w = t.width, h = t.height;
  id<MTLBuffer> b = [device newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared];
  if (!b) return nil;
  [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0
 destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
  return b;
}

bool GridSchedule();
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
      "[frame-interp] real={} generated={} dropped={} no_inputs={} real_interval={:.2f} ms | fi gpu mean {:.3f} ms max {:.3f} "
      "ms (n={}) | gap before generated {:.2f}+-{:.2f} ms (n={}), before real {:.2f}+-{:.2f} ms (n={}) | "
      "real ready->on screen {:.2f} ms mean, {:.2f} max (n={}) | nextDrawable wait first {:.2f} ms mean {:.2f} max, "
      "second {:.2f} mean {:.2f} max | sched {} lead {:.2f} ms slips {} late {} pull-ins {}",
      g_stat_frames, g_stat_generated, g_dropped, g_stat_no_inputs, g_interval_ema * 1000.0, n ? sum / 1000.0 / n : 0.0,
      mx / 1000.0, n, mean(g_gap_sum[0], g_gap_n[0]), sd(g_gap_sum[0], g_gap_sq[0], g_gap_n[0]), g_gap_n[0],
      mean(g_gap_sum[1], g_gap_n[1]), sd(g_gap_sum[1], g_gap_sq[1], g_gap_n[1]), g_gap_n[1],
      mean(g_lat_sum, g_lat_n), g_lat_max * 1000.0, g_lat_n, mean(g_wait_sum[0], g_wait_n[0]),
      g_wait_max[0] * 1000.0, mean(g_wait_sum[1], g_wait_n[1]), g_wait_max[1] * 1000.0,
      GridSchedule() ? "grid" : "legacy", g_sched_lead.load() * 1000.0, g_sched_slips.exchange(0),
      g_sched_late.exchange(0), g_sched_pullins.exchange(0));
  g_wait_sum[0] = g_wait_sum[1] = g_wait_max[0] = g_wait_max[1] = 0;
  g_wait_n[0] = g_wait_n[1] = 0;
  g_stat_frames = g_stat_generated = g_stat_no_inputs = 0;
  g_gap_sum[0] = g_gap_sum[1] = g_gap_sq[0] = g_gap_sq[1] = 0;
  g_gap_n[0] = g_gap_n[1] = 0;
  g_lat_sum = g_lat_max = 0;
  g_lat_n = 0;
}

// ---------------------------------------------------------------- async presentation (present thread)

constexpr int kTargets = 6;
struct Target {
  id<MTLTexture> tex = nil;
  bool busy = false;
};
Target g_targets[kTargets];
struct Job {
  int idx;
  double min_duration;
  bool generated;
  double ready;
  uint64_t frame_id;
  uint64_t lock_frame;
  double submitted;  // CACurrentMediaTime at SubmitPresent
  CAMetalLayer* layer;
  id<MTLCommandQueue> queue;
};
std::mutex g_job_mutex;  // g_targets, g_jobs, g_stop
std::condition_variable g_job_cv;
std::deque<Job> g_jobs;
std::thread g_thread;
bool g_stop = false;
std::condition_variable g_tick_cv;
uint64_t g_ticks = 0;

void FreeTarget(int idx) {
  std::lock_guard<std::mutex> lock(g_job_mutex);
  g_targets[idx].busy = false;
}

// ---- Grid scheduling (default; FABLE2_FRAME_INTERP_SCHED=legacy = the first prototype's min-duration pacing).
// Every present gets an explicit display time on the display's vsync grid (presentAtTime): per real frame the
// generated frame goes on vsync N and the real one on N + k/2 (k = refreshes per real frame: 2 for 30 -> 60 on a
// 60 Hz display). N is the next planned slot (the previous pair's N + k), or, when the frame came too late for it,
// the first vsync at least `lead` after the drawable was taken (a slip: the previous real frame stays up longer).
// The compositor needs a present a little over two refreshes before its display time, and with 3 drawables at
// 60 presents/s a drawable comes free about three refreshes before the vsync it will be shown on; so a pair that
// arrives early waits for a drawable, and a backlog (after a hitch elsewhere) never drains by itself. When pairs
// keep arriving a refresh earlier than their slot needs, one generated frame is skipped and the real frame takes
// its slot (catch-up: one refresh less latency from then on). The vsync lock (OnExternalSchedule) moves the guest
// vblank so pairs arrive just before the slot deadline (target - lead).
bool GridSchedule() {
  static const bool v = [] {
    const char* e = std::getenv("FABLE2_FRAME_INTERP_SCHED");  // getenv-ok: once
    return !(e && std::strcmp(e, "legacy") == 0);
  }();
  return v;
}
// Present thread only.
double g_grid = 0;            // a known vsync (latest presentedTime), mach seconds
double g_slot_vsync = 0;      // planned vsync of the next real frame's first present
double g_pair_target = 0;     // vsync of the current pair's first present
int g_pair_half = 1;          // refreshes from the pair's first present to its real frame
bool g_pair_open = false;     // a generated frame was scheduled (or skipped); its real frame follows
int g_slip_run = 0;           // consecutive pairs too late for their slot
int g_behind_run = 0;         // consecutive pairs that arrived a refresh earlier than their slot needed
int g_catchup_need = 3;       // behind_run that triggers a catch-up; doubles when a catch-up is followed by a slip
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

double CeilGrid(double t, double period) {
  return g_grid + std::ceil((t - g_grid) / period - 1e-6) * period;
}

int RefreshesPerFrame(double period) {
  const double interval = g_interval_ema > 0 ? g_interval_ema : 2 * period;
  return std::max(1, int(std::lround(interval / period)));
}

// Present log (FABLE2_FRAME_INTERP_PRESENT_LOG=<file>): one CSV row per present with its presentedTime.
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
                double arrived = 0) {
  FILE* f = PresentLog();
  if (!f) return;
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::fprintf(f, "%s,%llu,%.6f,%.6f,%.6f,%.6f,%.6f\n", kind, (unsigned long long)frame, taken, target, presented,
               ready, arrived);
  static int n = 0;
  if (++n % 60 == 0) std::fflush(f);
}

// Catch-up check for a generated job, before taking a drawable: true = skip it (its real frame takes its slot).
bool SkipForCatchUp(const Job& job) {
  if (!GridSchedule() || g_grid <= 0 || g_slot_vsync <= 0 || !job.generated) return false;
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

// Display time for a job (0 = present as soon as possible). taken = when its drawable was taken.
double ScheduleJob(const Job& job, double taken, double* deadline, bool* slipped) {
  *deadline = 0;
  *slipped = false;
  if (!GridSchedule() || g_grid <= 0) return 0.0;
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
      // Too late for its slot but in time for the real frame's: drop the generated frame and keep the cadence
      // (the previous real frame stays up one refresh longer; nothing after it moves). A second slip in a row
      // means the pairs now arrive a refresh later than the cadence (the vsync lock aligns arrivals only modulo
      // one refresh), so the next one re-anchors the cadence instead.
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
    }
    @autoreleasepool {
      g_grid = g_grid_atomic.load(std::memory_order_relaxed);
      if (SkipForCatchUp(job)) {
        FreeTarget(job.idx);
        continue;
      }
      const double t0 = CACurrentMediaTime();
      id<CAMetalDrawable> drawable = [job.layer nextDrawable];
      const double taken = CACurrentMediaTime();
      NoteDrawableWait(job.generated ? 0 : 1, taken - t0);
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
        FreeTarget(job.idx);  // generated frame dropped (slip); the drawable goes back unpresented
        continue;
      }
      id<MTLCommandBuffer> cb = [job.queue commandBuffer];
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
        OnPresented(generated, presented, ready, false);
        LogPresent(generated ? "gen" : "real", lock_frame, taken, target, presented, ready, submitted);
        if (!generated && lock_frame) graphics::metal::vsync_lock::OnPresented(lock_frame, presented);
        if (frame_id && graphics::metal::frame_log::Enabled()) {
          graphics::metal::frame_log::Presented(frame_id, presented);
        }
      }];
      const int idx = job.idx;
      [cb addCompletedHandler:^(id<MTLCommandBuffer>) { FreeTarget(idx); }];
      if (target > 0) {
        // A little before the vsync so rounding can only land on it (FABLE2_FRAME_INTERP_AT_OFFSET: fraction of a
        // refresh before the target, experiments).
        static const double at_offset = [] {
          const char* e = std::getenv("FABLE2_FRAME_INTERP_AT_OFFSET");  // getenv-ok: once
          return e ? std::strtod(e, nullptr) : 0.1;
        }();
        [cb presentDrawable:drawable atTime:target - at_offset * Period()];
      } else if (job.min_duration > 0.0) {
        [cb presentDrawable:drawable afterMinimumDuration:job.min_duration];
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

}  // namespace

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
        ++g_dropped;
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

void SubmitPresent(void* layer_v, void* queue_v, void* target, double min_duration, bool generated, double ready_s,
                   uint64_t frame_id, uint64_t lock_frame) {
  CAMetalLayer* layer = (CAMetalLayer*)layer_v;
  if (layer.framebufferOnly) {
    layer.framebufferOnly = NO;  // the present thread copies into the drawable
  }
  // FABLE2_FRAME_INTERP_DRAWABLES=2|3: CAMetalLayer maximumDrawableCount while interpolating. Default 2: two
  // presents per real frame keep every drawable queued, so a third only adds a refresh of latency (measured 30 -> 60
  // on a 60 Hz display: ready -> on screen 79 ms with 2, 94 ms with 3; pacing identical).
  static const NSUInteger drawables = [] {
    const char* v = std::getenv("FABLE2_FRAME_INTERP_DRAWABLES");  // getenv-ok: once
    // Grid scheduling needs 3: each present is committed two refreshes ahead of its display time, so with 2 the
    // present thread can only hand over 20 presents/s (measured 2026-10-22). Legacy pacing defaulted to 2.
    const long fallback = GridSchedule() ? 3 : 2;
    const long n = v ? std::strtol(v, nullptr, 10) : fallback;
    return NSUInteger(n == 2 || n == 3 ? n : fallback);
  }();
  if (drawables && layer.maximumDrawableCount != drawables) {
    layer.maximumDrawableCount = drawables;
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
    ++g_dropped;
  }
  g_jobs.push_back(
      Job{idx, min_duration, generated, ready_s, frame_id, lock_frame, CACurrentMediaTime(), layer,
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
  // Legacy pacing keeps the first prototype's behaviour exactly: the UI thread only wakes on presents, so a new guest
  // frame waits up to a refresh before it is interpolated (a per-launch phase effect, docs/frame-interp-prototype.md).
  if (!GridSchedule()) return;
  {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    ++g_ticks;
  }
  g_tick_cv.notify_all();
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

bool Enabled() {
  static const bool v = std::strcmp(EnvValue(), "1") == 0 || std::strcmp(EnvValue(), "on") == 0;
  return v;
}
bool StatsOnly() {
  static const bool v = std::strcmp(EnvValue(), "stats") == 0;
  return v;
}
double Now() { return CACurrentMediaTime(); }
bool SceneInputsWanted() {
  static const bool v = Enabled() && (ModeA() != Inputs::kColor || (HaveArmB() && ModeB() != Inputs::kColor));
  return v;
}

void NoteDraw(const float* c0_rows, const float* vport) {
  for (uint32_t i = 0; i < g_candidate_count; ++i) {
    if (std::memcmp(g_candidates[i].m, c0_rows, sizeof(float) * 16) == 0) {
      ++g_candidates[i].count;
      return;
    }
  }
  // Full: replace the least used candidate (space-saving count), so the frequent matrix always survives.
  Candidate* c = nullptr;
  if (g_candidate_count < kCandidates) {
    c = &g_candidates[g_candidate_count++];
    c->count = 0;
  } else {
    c = &g_candidates[0];
    for (uint32_t i = 1; i < kCandidates; ++i)
      if (g_candidates[i].count < c->count) c = &g_candidates[i];
  }
  std::memcpy(c->m, c0_rows, sizeof(c->m));
  std::memcpy(c->vport, vport, sizeof(c->vport));
  ++c->count;
}

void OnSwap() { g_candidate_count = 0; }

void EncodeSceneInputs(MTL::Device* device_cpp, MTL::CommandBuffer* cb_cpp, MTL::Texture* depth_cpp,
                       uint32_t scale_x, uint32_t scale_y, float depth_to_window) {
  if (!SceneInputsWanted()) return;
  id<MTLDevice> device = (id<MTLDevice>)device_cpp;
  id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)cb_cpp;
  id<MTLTexture> depth = (id<MTLTexture>)depth_cpp;
  const uint32_t ow = g_out_w.load(std::memory_order_relaxed), oh = g_out_h.load(std::memory_order_relaxed);
  if (!device || !cb || !depth || !ow || !oh || !g_candidate_count || !EnsurePipelines(device)) return;
  const Candidate* best = &g_candidates[0];
  for (uint32_t i = 1; i < g_candidate_count; ++i)
    if (g_candidates[i].count > best->count) best = &g_candidates[i];

  const int si = g_pending >= 0 ? g_pending : g_next_slot;
  Slot& s = g_slots[si];
  if (!s.depth || s.depth.width != ow || s.depth.height != oh) {
    [s.depth release];
    [s.motion release];
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                                 width:ow
                                                                                height:oh
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    d.storageMode = MTLStorageModePrivate;
    s.depth = [device newTextureWithDescriptor:d];
    d.pixelFormat = MTLPixelFormatRG16Float;
    s.motion = [device newTextureWithDescriptor:d];
  }
  if (!s.depth || !s.motion) return;

  PrepParams p = {};
  double cur[16], prev[16], inv[16], m[16];
  for (int i = 0; i < 16; ++i) {
    cur[i] = best->m[i];
    prev[i] = EvalMode() ? (g_have_prev2_vp ? g_prev2_vp[i] : best->m[i])
                         : (g_have_prev_vp ? g_prev_vp[i] : best->m[i]);
  }
  bool reset = EvalMode() ? !g_have_prev2_vp : !g_have_prev_vp;
  if (Invert(cur, inv)) {
    Mul(prev, inv, m);
  } else {
    reset = true;
    for (int i = 0; i < 16; ++i) m[i] = (i % 5) == 0;
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) p.reproj[c * 4 + r] = float(m[r * 4 + c]);
  const float* vp = best->vport;
  p.vport[0] = vp[0];
  p.vport[1] = vp[1];
  p.vport[2] = vp[2];
  p.vport[3] = vp[3];
  p.zso[0] = vp[4] != 0 ? vp[4] : 1.0f;
  p.zso[1] = vp[5];
  p.scene[0] = std::fabs(vp[0]) * 2.0f;
  p.scene[1] = std::fabs(vp[2]) * 2.0f;
  p.content[0] = p.scene[0] * float(scale_x ? scale_x : 1);
  p.content[1] = p.scene[1] * float(scale_y ? scale_y : 1);
  p.outsz[0] = float(ow);
  p.outsz[1] = float(oh);
  p.zero_motion = SceneMotionWanted() ? 0 : 1;
  p.depth_scale = depth_to_window > 0.0f ? depth_to_window : 1.0f;

  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  if (!enc) return;
  enc.label = @"FrameInterpPrep";
  [enc setComputePipelineState:depth.sampleCount > 1 ? g_prep_ms : g_prep_2d];
  [enc setTexture:depth atIndex:0];
  [enc setTexture:s.depth atIndex:1];
  [enc setTexture:s.motion atIndex:2];
  [enc setBytes:&p length:sizeof(p) atIndex:0];
  [enc dispatchThreads:MTLSizeMake(ow, oh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
  [enc endEncoding];

  std::lock_guard<std::mutex> lock(g_slot_mutex);
  s.frame_id = 0;
  std::memcpy(s.vp, best->m, sizeof(s.vp));
  for (int i = 0; i < 16; ++i) s.prev_vp[i] = float(prev[i]);
  std::memcpy(s.vport, best->vport, sizeof(s.vport));
  s.reset = reset;
  CameraFromVp(best->m, best->vport, s);
  g_pending = si;
  if ((g_scene_encodes++ % 300) == 0) {
    REXLOG_INFO("[frame-interp] scene inputs: depth {}x{} {} samples, scene {}x{} guest px, out {}x{}, fov {:.1f} "
                "aspect {:.3f} near {:.3f} far {:.1f} reversed {} (matrix used by {} of {} candidates' draws; rows {} {} {} {} | {} {} {} {})",
                depth.width, depth.height, depth.sampleCount, p.scene[0], p.scene[1], ow, oh, s.fov_deg, s.aspect,
                s.near_plane, s.far_plane, s.reversed, best->count,
                [] {
                  uint32_t t = 0;
                  for (uint32_t i = 0; i < g_candidate_count; ++i) t += g_candidates[i].count;
                  return t;
                }(),
                best->m[0], best->m[1], best->m[2], best->m[3], best->m[12], best->m[13], best->m[14], best->m[15]);
  }
}

void CommitFrame(uint64_t frame_id) {
  std::lock_guard<std::mutex> lock(g_slot_mutex);
  if (g_pending < 0) {
    g_have_prev_vp = false;
    g_have_prev2_vp = false;
    return;
  }
  Slot& s = g_slots[g_pending];
  s.frame_id = frame_id;
  std::memcpy(g_prev2_vp, g_prev_vp, sizeof(g_prev2_vp));
  g_have_prev2_vp = g_have_prev_vp;
  std::memcpy(g_prev_vp, s.vp, sizeof(g_prev_vp));
  g_have_prev_vp = true;
  g_next_slot = (g_pending + 1) % kSlots;
  g_pending = -1;
}

PresentPlan Process(void* device_v, void* queue_v, void* color_v, uint64_t frame_id, void* wait_event_v,
                    uint64_t wait_value) {
  PresentPlan plan;
  id<MTLDevice> device = (id<MTLDevice>)device_v;
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>)queue_v;
  id<MTLTexture> color = (id<MTLTexture>)color_v;
  const double now = CACurrentMediaTime();
  if (g_last_ready > 0) {
    const double interval = now - g_last_ready;
    if (interval > 1.0 / 250.0 && interval < 0.2) {
      g_interval_ema = g_interval_ema > 0 ? g_interval_ema * 0.9 + interval * 0.1 : interval;
    }
  }
  g_last_ready = now;
  if (!color) return plan;
  g_out_w.store(uint32_t(color.width), std::memory_order_relaxed);
  g_out_h.store(uint32_t(color.height), std::memory_order_relaxed);
  // Minimum on-screen time of each present: under half the real interval, so a present is never pushed a whole
  // refresh late by jitter (30 -> 60 on a 60 Hz display: one refresh each; on 120 Hz: two).
  plan.half_interval_s = g_interval_ema * 0.5 * 0.75;
  MaybeLogStats();
  if (!Enabled() || g_creation_failed) return plan;
#if FI_AVAILABLE
  const uint32_t w = uint32_t(color.width), h = uint32_t(color.height);
  if (!g_fi || g_fi_w != w || g_fi_h != h) {
    [g_fi release];
    g_fi = nil;
    for (auto& t : g_color) {
      [t release];
      t = nil;
    }
    [g_out release];
    g_out = nil;
    g_have_prev_color = false;
    if (![MTLFXFrameInterpolatorDescriptor supportsDevice:device]) {
      REXLOG_WARN("[frame-interp] MTLFXFrameInterpolator not supported on this device; off");
      g_creation_failed = true;
      return plan;
    }
    MTLFXFrameInterpolatorDescriptor* desc = [MTLFXFrameInterpolatorDescriptor new];
    desc.colorTextureFormat = color.pixelFormat;
    desc.outputTextureFormat = color.pixelFormat;
    desc.depthTextureFormat = MTLPixelFormatR32Float;
    desc.motionTextureFormat = MTLPixelFormatRG16Float;
    desc.inputWidth = w;
    desc.inputHeight = h;
    desc.outputWidth = w;
    desc.outputHeight = h;
    g_fi = [desc newFrameInterpolatorWithDevice:device];
    if (HaveArmB()) {
      [g_fi0 release];
      g_fi0 = [desc newFrameInterpolatorWithDevice:device];
    }
    [desc release];
    REXLOG_INFO("[frame-interp] MTLFXFrameInterpolator {}x{} format {} inputs {}{}{} ({})", w, h,
                uint32_t(color.pixelFormat), InputsName(ModeA()), HaveArmB() ? ", eval arm B " : "",
                HaveArmB() ? InputsName(ModeB()) : "",
                g_fi ? "created" : "creation failed; off");
    if (!g_fi) {
      g_creation_failed = true;
      return plan;
    }
    g_fi_w = w;
    g_fi_h = h;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:color.pixelFormat
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = g_fi.colorTextureUsage | MTLTextureUsageShaderRead;
    for (int i = 0; i < 3; ++i) {
      g_color[i] = [device newTextureWithDescriptor:td];
      g_color_frame[i] = 0;
    }
    td.usage = g_fi.outputTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    g_out = [device newTextureWithDescriptor:td];
    if (g_fi0) {
      [g_out0 release];
      g_out0 = [device newTextureWithDescriptor:td];
    }
    {
      MTLTextureDescriptor* zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];
      zd.storageMode = MTLStorageModeShared;
      zd.usage = MTLTextureUsageShaderRead;
      [g_zero_motion release];
      g_zero_motion = [device newTextureWithDescriptor:zd];
      std::vector<uint8_t> zeros(size_t(w) * h * 4, 0);
      [g_zero_motion replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:zeros.data()
                       bytesPerRow:w * 4];
      zd.pixelFormat = MTLPixelFormatR32Float;
      [g_flat_depth release];
      g_flat_depth = [device newTextureWithDescriptor:zd];
      std::vector<float> flat(size_t(w) * h, 0.01f);  // reversed z: a plane about 10 units away
      [g_flat_depth replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:flat.data()
                       bytesPerRow:w * 4];
    }
  }

  Slot inputs;
  bool have_inputs = false;
  {
    std::lock_guard<std::mutex> lock(g_slot_mutex);
    for (const Slot& s : g_slots) {
      if (s.frame_id == frame_id && s.depth) {
        inputs.depth = s.depth;  // not retained: slot textures live until a size change
        inputs.motion = s.motion;
        inputs.reset = s.reset;
        std::memcpy(inputs.vp, s.vp, sizeof(inputs.vp));
        std::memcpy(inputs.prev_vp, s.prev_vp, sizeof(inputs.prev_vp));
        std::memcpy(inputs.vport, s.vport, sizeof(inputs.vport));
        inputs.near_plane = s.near_plane;
        inputs.far_plane = s.far_plane;
        inputs.fov_deg = s.fov_deg;
        inputs.aspect = s.aspect;
        inputs.reversed = s.reversed;
        have_inputs = true;
        break;
      }
    }
  }
  const bool eval = EvalMode();
  const int cur = int(frame_id % 3), prev = int((frame_id + 2) % 3), prev2 = int((frame_id + 1) % 3);
  const bool consecutive = eval ? (g_color_frame[prev2] == frame_id - 2 && g_color_frame[prev] == frame_id - 1)
                                : (g_have_prev_color && g_color_frame[prev] == frame_id - 1);
  g_last_frame = frame_id;

  id<MTLCommandBuffer> cb = [queue commandBuffer];
  cb.label = @"FrameInterp";
  if (wait_event_v) {
    [cb encodeWaitForEvent:(id<MTLSharedEvent>)wait_event_v value:wait_value];
  }
  g_color_frame[cur] = frame_id;
  id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
  [blit copyFromTexture:color toTexture:g_color[cur]];
  [blit endEncoding];
  bool generated = false;
  const bool need_scene = ModeA() != Inputs::kColor || (HaveArmB() && ModeB() != Inputs::kColor);
  if (consecutive && (have_inputs || !need_scene) && g_interval_ema > 0) {
    for (id<MTLFXFrameInterpolator> fi : {g_fi0, g_fi}) {
    if (!fi) continue;
    const bool zero = fi == g_fi0;  // arm B
    const Inputs mode = zero ? ModeB() : ModeA();
    fi.colorTexture = g_color[cur];
    fi.prevColorTexture = g_color[eval ? prev2 : prev];
    fi.depthTexture = mode == Inputs::kColor ? g_flat_depth : inputs.depth;
    fi.motionTexture = mode == Inputs::kCamera ? inputs.motion : g_zero_motion;
    fi.outputTexture = zero ? g_out0 : g_out;
    fi.motionVectorScaleX = 1.0f;
    fi.motionVectorScaleY = 1.0f;
    fi.deltaTime = float(g_interval_ema * (eval ? 2.0 : 1.0));
    if (!have_inputs) {
      inputs.aspect = float(w) / float(h);
      inputs.far_plane = 5000.0f;
      inputs.reversed = true;  // the flat depth (0.01) is a plane about 10 units away in reversed z
      inputs.reset = false;
    }
    fi.nearPlane = inputs.near_plane;
    fi.farPlane = std::min(inputs.far_plane, 100000.0f);
    fi.fieldOfView = inputs.fov_deg;
    fi.aspectRatio = inputs.aspect;
    fi.jitterOffsetX = 0.0f;
    fi.jitterOffsetY = 0.0f;
    fi.depthReversed = inputs.reversed;
    fi.shouldResetHistory = inputs.reset;
    fi.uiTexture = nil;
    [fi encodeToCommandBuffer:cb];
    }
    generated = true;
    ++g_stat_generated;
  } else if (!have_inputs && need_scene) {
    ++g_stat_no_inputs;
  }
  g_have_prev_color = true;

  // Frame dump for the evaluation (real frame, generated frame, motion and depth as PNGs).
  if (const char* dir = DumpDir()) {
    const std::string trigger = std::string(dir) + "/trigger";
    if (!g_dump_remaining && access(trigger.c_str(), F_OK) == 0) {
      g_dump_remaining = 8;
      if (FILE* f = std::fopen(trigger.c_str(), "r")) {
        unsigned n = 0;
        if (std::fscanf(f, "%u", &n) == 1 && n) g_dump_remaining = n;
        std::fclose(f);
      }
      unlink(trigger.c_str());
    }
    if (g_dump_remaining) {
      --g_dump_remaining;
      id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
      id<MTLBuffer> real = BlitToBuffer(device, b, g_color[cur], 4);
      id<MTLBuffer> gen = generated ? BlitToBuffer(device, b, g_out, 4) : nil;
      id<MTLBuffer> mv = have_inputs ? BlitToBuffer(device, b, inputs.motion, 4) : nil;
      id<MTLBuffer> dp = have_inputs ? BlitToBuffer(device, b, inputs.depth, 4) : nil;
      id<MTLBuffer> truth = eval && consecutive ? BlitToBuffer(device, b, g_color[prev], 4) : nil;
      id<MTLBuffer> before = eval && consecutive ? BlitToBuffer(device, b, g_color[prev2], 4) : nil;
      id<MTLBuffer> gen0 = generated && g_fi0 ? BlitToBuffer(device, b, g_out0, 4) : nil;
      [b endEncoding];
      const std::string base = std::string(dir) + "/" + std::to_string(frame_id);
      if (have_inputs) {
        if (FILE* f = std::fopen((base + "_cam.txt").c_str(), "w")) {
          std::fprintf(f, "vp");
          for (float v : inputs.vp) std::fprintf(f, " %.9g", v);
          std::fprintf(f, "\nprev_vp");
          for (float v : inputs.prev_vp) std::fprintf(f, " %.9g", v);
          std::fprintf(f, "\nvport");
          for (float v : inputs.vport) std::fprintf(f, " %.9g", v);
          std::fprintf(f, "\n");
          std::fclose(f);
        }
      }
      const bool rgb10 = color.pixelFormat == MTLPixelFormatRGB10A2Unorm;
      [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        const size_t n = size_t(w) * h;
        std::vector<uint8_t> px(n * 4);
        auto put = [&](id<MTLBuffer> buf, const char* suffix) {
          if (!buf) return;
          if (rgb10) {
            const uint32_t* src = static_cast<const uint32_t*>(buf.contents);
            for (size_t i = 0; i < n; ++i) {
              const uint32_t v = src[i];
              px[i * 4 + 0] = uint8_t((v & 0x3FF) >> 2);
              px[i * 4 + 1] = uint8_t(((v >> 10) & 0x3FF) >> 2);
              px[i * 4 + 2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
            }
          } else {
            std::memcpy(px.data(), buf.contents, n * 4);
          }
          for (size_t i = 0; i < n; ++i) px[i * 4 + 3] = 255;
          WritePng(base + suffix, px, w, h);
        };
        put(gen, "_a_gen.png");
        put(real, "_b_real.png");
        put(truth, "_e_truth.png");
        put(before, "_f_prev2.png");
        put(gen0, "_g_gen_b.png");
        if (mv) {
          if (FILE* f = std::fopen((base + "_motion.bin").c_str(), "wb")) {
            std::fwrite(mv.contents, 1, n * 4, f);
            std::fclose(f);
          }
          const __fp16* m = static_cast<const __fp16*>(mv.contents);
          for (size_t i = 0; i < n; ++i) {
            const float x = m[i * 2], y = m[i * 2 + 1];
            px[i * 4 + 0] = uint8_t(std::clamp(128.0f + x * 4.0f, 0.0f, 255.0f));
            px[i * 4 + 1] = uint8_t(std::clamp(128.0f + y * 4.0f, 0.0f, 255.0f));
            px[i * 4 + 2] = 128;
            px[i * 4 + 3] = 255;
          }
          WritePng(base + "_c_motion.png", px, w, h);
        }
        if (dp) {
          if (FILE* f = std::fopen((base + "_depth.bin").c_str(), "wb")) {
            std::fwrite(dp.contents, 1, n * 4, f);
            std::fclose(f);
          }
          const float* d = static_cast<const float*>(dp.contents);
          for (size_t i = 0; i < n; ++i) {
            const float v = d[i] > 0 ? std::clamp(1.0f + std::log10(d[i]) / 4.0f, 0.0f, 1.0f) : 0.0f;
            px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = uint8_t(v * 255.0f);
            px[i * 4 + 3] = 255;
          }
          WritePng(base + "_d_depth.png", px, w, h);
        }
        [real release];
        [gen release];
        [mv release];
        [dp release];
        [truth release];
        [before release];
        [gen0 release];
      }];
    }
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
  if (generated && !eval) plan.generated = (void*)g_out;
#endif
  return plan;
}

void NoteDrawableWait(int which, double seconds) {
  std::lock_guard<std::mutex> lock(g_present_mutex);
  which = which ? 1 : 0;
  g_wait_sum[which] += seconds;
  g_wait_max[which] = std::max(g_wait_max[which], seconds);
  ++g_wait_n[which];
}

void OnPresented(bool generated, double presented_s, double ready_s, bool log_row) {
  if (log_row) LogPresent(generated ? "gen" : "real", 0, 0.0, 0.0, presented_s, ready_s);
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

}  // namespace rex::ui::metal::frame_interp
