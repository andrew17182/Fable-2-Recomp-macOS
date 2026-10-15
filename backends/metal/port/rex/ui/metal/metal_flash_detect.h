// metal_flash_detect.h - transient-white-pixel detector for the flash kit (docs/flash-kit.md).
//
// FABLE2_FLASH_DETECT=<folder> (diagnostic, default off; set by the flash kit's FableIIBenchmark per arm).
// Every guest output refresh, a small compute kernel on the presenter queue (ordered after the guest output
// copy, like `capture at <frame>`) reduces the guest output to a grid kGridW cells wide (height by aspect): per cell
// the mean RGB and the whitest pixel (max over the cell of min(R, G, B)), 8 bit each. The CPU keeps the last three
// grids and scores the middle frame N once N+1 arrives. Two detectors, each "white in N only":
//   flash: the cell's mean is white (every channel >= kMinWhite) and bright (luma >= kMinLuma), and its luma
//          exceeds the brightest luma of the 3x3 cells around it in BOTH N-1 and N+1 by >= kMinDelta. Catches
//          patches a few pixels wide and up.
//   crack: the same test on the whitest pixel of the cell (per-cell peak). Catches hairlines and single pixels
//          (a 1-pixel crack showing the sky through the terrain), which the cell mean dilutes.
// The frame score of each is its largest 8-connected cluster of transient cells: the breadcrumb trail's star
// sparkles and leaf gaps are white for one frame too but make clusters of 1-3 cells (M5 Max, Bower Lake,
// 2026-10-14), so a flash / crack frame needs a cluster of at least kClusterCells. Only the ground region is
// scored: the top kSkyTop of the screen (sky through leaves and fence tops, lamps, the quest text, the health
// icon: the M5 floor's sources) and the d-pad HUD box at the bottom left are skipped, as is a border of kBorder
// (content entering the screen during a turn is new in N). The floor (scene content that legitimately flickers)
// is measured by the arms without the suspect setting; the kit compares arms, not absolute counts.
// No Metal captures; one 480x270x4 readback per frame.
//
// Scoring runs only inside the window the built-in benchmark opens (src/core/fable2_benchmark.h calls the
// exported fable2_flash_control(1) at the sweep start and fable2_flash_control(2) at its end): op 2 waits for the
// in-flight frames, then writes <folder>/flash-frames.csv (one row per scored frame), <folder>/flash.json
// (summary + worst frames) and <folder>/flash-<rank>-f<frame>.png for the worst moments (previous | frame | next |
// transient cells: red = flash, orange = crack only, over the darkened frame; each panel at grid size, 2x).
//
// FABLE2_FLASH_INDUCE=1 (detector check only): while the window is open, every 40th frame gets a white shape
// written into the guest output before the reduction (so it is also on screen), cycling through a 0.4 % and a
// 1.6 % of the width square, a 1-pixel and a 2-pixel line 6 % of the width long, and a 3 % square, at a
// pseudo-random spot in the lower two thirds. flash.json lists the induced frames and what was detected.
#pragma once

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "rex/logging.h"

namespace rex::ui::metal::flash {

constexpr uint32_t kGridW = 480;
constexpr int kMinLuma = 166;    // 0.65 (gamma-encoded, 0..255)
constexpr int kMinDelta = 64;    // 0.25
constexpr int kMinWhite = 140;   // 0.55, every channel
constexpr float kBorder = 0.04f;
constexpr float kSkyTop = 0.30f;                          // skipped top fraction of the height
constexpr float kHudLeft = 0.16f, kHudBottom = 0.70f;     // skipped d-pad box: x < 16 %, y > 70 %
constexpr uint32_t kClusterCells = 6;  // a frame whose largest transient cluster has this many cells counts
constexpr size_t kExamples = 6;

inline const char* Folder() {
  static const char* dir = [] {
    const char* v = std::getenv("FABLE2_FLASH_DETECT");
    return v && v[0] ? v : nullptr;
  }();
  return dir;
}
inline bool Enabled() { return Folder() != nullptr; }
inline bool Induce() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_FLASH_INDUCE");
    return v && v[0] == '1';
  }();
  return on;
}

struct Grid {
  uint64_t frame = 0;
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;  // mean RGB + max luma in A
};

struct Example {
  uint64_t frame = 0;
  uint32_t score = 0, flash = 0, crack = 0;  // score = max(flash, crack) largest clusters
  Grid prev, cur, next;
  std::vector<uint8_t> mask;  // 1 = flash cell (mean), 2 = crack cell only (peak)
};

struct Row {
  uint64_t frame;
  uint32_t flash_cells, crack_cells, flash_cluster, crack_cluster;
  float luma;  // mean luma of the frame (0..1)
};

struct Induced {
  uint64_t frame;
  const char* kind;
};

class Detector {
 public:
  static Detector& Get() {
    static Detector d;
    return d;
  }

  // GPU thread, after the guest output of `frame` was committed to `queue`.
  void OnRefresh(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture, uint64_t frame) {
    if (!texture || !queue || !device) return;
    if (!pipeline_ && !failed_) Init(device);
    if (!pipeline_) return;
    if (!active_.load(std::memory_order_acquire)) return;
    const uint32_t w = kGridW;
    const uint32_t h = std::max<uint32_t>(1, uint32_t(std::lround(double(kGridW) * texture.height / texture.width)));
    id<MTLBuffer> buffer = nil;
    {
      std::lock_guard lock(mutex_);
      for (auto it = free_.begin(); it != free_.end(); ++it) {
        if ((*it).length >= size_t(w) * h * 4) { buffer = *it; free_.erase(it); break; }
      }
      if (!buffer && in_flight_ >= 6) { ++skipped_; return; }
      ++in_flight_;
    }
    if (!buffer) buffer = [device newBufferWithLength:size_t(w) * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = buffer ? [queue commandBuffer] : nil;
    id<MTLComputeCommandEncoder> enc = cb ? [cb computeCommandEncoder] : nil;
    if (!enc) {
      std::lock_guard lock(mutex_);
      --in_flight_;
      ++skipped_;
      if (buffer) free_.push_back(buffer);
      cv_.notify_all();
      return;
    }
    if (Induce() && induce_pipeline_ && (++induce_counter_ % 40) == 20) {
      struct Shape { const char* kind; float w, h; uint32_t px; };  // fraction of the width, or px when px > 0
      static const Shape kShapes[5] = {{"square 0.4%", 0.004f, 0.004f, 0}, {"square 1.6%", 0.016f, 0.016f, 0},
                                       {"line 1px", 0.06f, 0, 1}, {"line 2px", 0.06f, 0, 2},
                                       {"square 3%", 0.03f, 0.03f, 0}};
      const Shape& shape = kShapes[(induce_counter_ / 40) % 5];
      const uint32_t sw = std::max<uint32_t>(1, uint32_t(shape.w * float(texture.width)));
      const uint32_t sh = shape.px ? shape.px : std::max<uint32_t>(1, uint32_t(shape.h * float(texture.width)));
      rng_ = rng_ * 1664525u + 1013904223u;
      const uint32_t x = uint32_t((uint64_t(rng_ >> 8) * (texture.width - sw)) >> 24);
      rng_ = rng_ * 1664525u + 1013904223u;
      const uint32_t y = uint32_t(texture.height / 3) +
                         uint32_t((uint64_t(rng_ >> 8) * (texture.height * 2 / 3 - sh)) >> 24);
      const uint32_t rect[4] = {x, y, sw, sh};
      [enc setComputePipelineState:induce_pipeline_];
      [enc setTexture:texture atIndex:0];
      [enc setBytes:rect length:sizeof(rect) atIndex:0];
      [enc dispatchThreads:MTLSizeMake(sw, sh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
      std::lock_guard lock(mutex_);
      induced_.push_back({frame, shape.kind});
    }
    const uint32_t dims[4] = {w, h, 0, 0};
    [enc setComputePipelineState:pipeline_];
    [enc setTexture:texture atIndex:0];
    [enc setBuffer:buffer offset:0 atIndex:0];
    [enc setBytes:dims length:sizeof(dims) atIndex:1];
    [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 8, 1)];
    [enc endEncoding];
    cb.label = @"FlashDetect";
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
      Grid g;
      const bool ok = done.status == MTLCommandBufferStatusCompleted;
      if (ok) {
        g.frame = frame;
        g.w = w;
        g.h = h;
        g.rgba.assign(static_cast<const uint8_t*>(buffer.contents),
                      static_cast<const uint8_t*>(buffer.contents) + size_t(w) * h * 4);
      }
      std::lock_guard lock(mutex_);
      free_.push_back(buffer);
      --in_flight_;
      if (ok) Push(std::move(g)); else ++skipped_;
      cv_.notify_all();
    }];
    [cb commit];
  }

  // op 1: open the window (clears earlier results). op 2: close it, wait for in-flight frames (1 s guard), write
  // the files. Returns 1 when the detector is on.
  int Control(int op) {
    if (!Enabled()) return 0;
    if (op == 1) {
      std::lock_guard lock(mutex_);
      rows_.clear();
      induced_.clear();
      examples_.clear();
      ring_.clear();
      skipped_ = 0;
      started_ = std::chrono::steady_clock::now();
      active_.store(true, std::memory_order_release);
      REXLOG_INFO("[flash] detector window open ({} cells wide; folder {})", kGridW, Folder());
      return 1;
    }
    if (op == 2) {
      active_.store(false, std::memory_order_release);
      std::unique_lock lock(mutex_);
      cv_.wait_for(lock, std::chrono::seconds(1), [&] { return in_flight_ == 0; });
      Write();
      return 1;
    }
    return 0;
  }

 private:
  void Init(id<MTLDevice> device) {
    static const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;
kernel void flash_reduce(texture2d<float, access::read> src [[texture(0)]], device uchar4* out [[buffer(0)]],
                         constant uint4& dims [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dims.x || gid.y >= dims.y) return;
  uint W = src.get_width(), H = src.get_height();
  uint x0 = gid.x * W / dims.x, x1 = max(x0 + 1, (gid.x + 1) * W / dims.x);
  uint y0 = gid.y * H / dims.y, y1 = max(y0 + 1, (gid.y + 1) * H / dims.y);
  float3 sum = 0.0f;
  float peak = 0.0f;
  for (uint y = y0; y < y1; ++y) {
    for (uint x = x0; x < x1; ++x) {
      float3 c = saturate(src.read(uint2(x, y)).rgb);
      sum += c;
      peak = max(peak, min(c.r, min(c.g, c.b)));
    }
  }
  float3 mean = sum / float((x1 - x0) * (y1 - y0));
  out[gid.y * dims.x + gid.x] = uchar4(uchar3(rint(mean * 255.0f)), uchar(rint(peak * 255.0f)));
}
kernel void flash_induce(texture2d<float, access::write> dst [[texture(0)]], constant uint4& rect [[buffer(0)]],
                         uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= rect.z || gid.y >= rect.w) return;
  dst.write(float4(1.0f), uint2(rect.x + gid.x, rect.y + gid.y));
}
)";
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kSource]
                                                  options:nil
                                                    error:&error];
    id<MTLFunction> fn = library ? [library newFunctionWithName:@"flash_reduce"] : nil;
    pipeline_ = fn ? [device newComputePipelineStateWithFunction:fn error:&error] : nil;
    if (Induce() && library) {
      id<MTLFunction> induce = [library newFunctionWithName:@"flash_induce"];
      induce_pipeline_ = induce ? [device newComputePipelineStateWithFunction:induce error:nil] : nil;
      REXLOG_INFO("[flash] FABLE2_FLASH_INDUCE=1: a white shape every 40th frame ({})",
                  induce_pipeline_ ? "on" : "kernel failed");
    }
    if (!pipeline_) {
      failed_ = true;
      REXLOG_ERROR("[flash] detector kernel failed: {}",
                   error ? [[error localizedDescription] UTF8String] : "unknown");
    } else {
      REXLOG_INFO("[flash] detector on: {} cells wide; transient = white (channels >= {}), luma >= {} and >= {} "
                  "above the 3x3 neighbourhood max of both neighbouring frames (0..255); flash = cell mean, crack = "
                  "whitest pixel of the cell; a frame counts with a cluster of >= {} cells; ground region only (top {:.0f}%, "
                  "d-pad HUD and a {:.0f}% border skipped)",
                  kGridW, kMinWhite, kMinLuma, kMinDelta, kClusterCells, kSkyTop * 100, kBorder * 100);
    }
  }

  static int Luma(const uint8_t* p) { return (p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8; }

  // mutex_ held.
  void Push(Grid g) {
    if (!active_.load(std::memory_order_relaxed)) return;
    if (!ring_.empty() && (g.frame <= ring_.back().frame || g.w != ring_.back().w || g.h != ring_.back().h)) {
      ring_.clear();
    }
    ring_.push_back(std::move(g));
    if (ring_.size() > 3) ring_.pop_front();
    if (ring_.size() == 3) Score(ring_[0], ring_[1], ring_[2]);
  }

  // Largest 8-connected cluster of cells whose mask has any of `bits`.
  static uint32_t LargestCluster(const std::vector<uint8_t>& mask, int w, int h, uint8_t bits) {
    uint32_t best = 0;
    std::vector<uint8_t> seen(mask.size(), 0);
    std::vector<int> stack;
    for (size_t i = 0; i < mask.size(); ++i) {
      if (!(mask[i] & bits) || seen[i]) continue;
      uint32_t size = 0;
      stack.assign(1, int(i));
      seen[i] = 1;
      while (!stack.empty()) {
        const int k = stack.back();
        stack.pop_back();
        ++size;
        const int kx = k % w, ky = k / w;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = kx + dx, ny = ky + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const size_t n = size_t(ny) * w + nx;
            if ((mask[n] & bits) && !seen[n]) { seen[n] = 1; stack.push_back(int(n)); }
          }
        }
      }
      best = std::max(best, size);
    }
    return best;
  }

  void Score(const Grid& a, const Grid& b, const Grid& c) {
    const int w = int(b.w), h = int(b.h);
    std::vector<uint8_t> mask(size_t(w) * h, 0);
    uint32_t flash_cells = 0, crack_cells = 0;
    uint64_t luma_sum = 0;
    const int bx = int(kBorder * w), by = int(kBorder * h), sky = int(kSkyTop * h);
    const int hud_x = int(kHudLeft * w), hud_y = int(kHudBottom * h);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const size_t i = size_t(y) * w + x;
        const uint8_t* p = &b.rgba[i * 4];
        const int l = Luma(p);
        luma_sum += l;
        if (x < bx || y < std::max(by, sky) || x >= w - bx || y >= h - by) continue;
        if (x < hud_x && y > hud_y) continue;
        const bool white = std::min({p[0], p[1], p[2]}) >= kMinWhite;
        const int pk = p[3];  // whitest pixel: min(R, G, B), max over the cell
        if (!white && pk < kMinWhite) continue;
        int na = 0, nc = 0, pa = 0, pc = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          const int yy = std::clamp(y + dy, 0, h - 1);
          for (int dx = -1; dx <= 1; ++dx) {
            const size_t j = size_t(yy) * w + std::clamp(x + dx, 0, w - 1);
            na = std::max(na, Luma(&a.rgba[j * 4]));
            nc = std::max(nc, Luma(&c.rgba[j * 4]));
            pa = std::max<int>(pa, a.rgba[j * 4 + 3]);
            pc = std::max<int>(pc, c.rgba[j * 4 + 3]);
          }
        }
        if (white && l >= kMinLuma && l - std::max(na, nc) >= kMinDelta) {
          ++flash_cells;
          mask[i] |= 1;
        }
        if (pk >= kMinLuma && pk - std::max(pa, pc) >= kMinDelta) {
          ++crack_cells;
          mask[i] |= 2;
        }
      }
    }
    const uint32_t flash = flash_cells ? LargestCluster(mask, w, h, 1) : 0;
    const uint32_t crack = crack_cells ? LargestCluster(mask, w, h, 2) : 0;
    rows_.push_back({b.frame, flash_cells, crack_cells, flash, crack, float(luma_sum) / float(size_t(w) * h) / 255.0f});
    const uint32_t score = std::max(flash, crack);
    if (score < 2) return;
    // Keep the worst kExamples moments, at least 8 frames apart (by the larger of the two clusters).
    for (auto it = examples_.begin(); it != examples_.end(); ++it) {
      if ((it->frame > b.frame ? it->frame - b.frame : b.frame - it->frame) < 8) {
        if (score <= it->score) return;
        examples_.erase(it);
        break;
      }
    }
    if (examples_.size() >= kExamples && score <= examples_.back().score) return;
    examples_.push_back({b.frame, score, flash, crack, a, b, c, std::move(mask)});
    std::sort(examples_.begin(), examples_.end(), [](const Example& x, const Example& y) { return x.score > y.score; });
    if (examples_.size() > kExamples) examples_.pop_back();
  }

  static bool WritePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
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

  // Four panels side by side, each at 2x the grid: previous | flash frame | next | mask over the flash frame.
  static bool WriteExample(const std::string& path, const Example& e) {
    const uint32_t w = e.cur.w, h = e.cur.h, s = 2, pw = w * s, W = pw * 4 + 3 * 4;
    std::vector<uint8_t> img(size_t(W) * h * s * 4, 255);
    auto panel = [&](int index, const Grid& g, bool overlay) {
      for (uint32_t y = 0; y < h * s; ++y) {
        for (uint32_t x = 0; x < pw; ++x) {
          const size_t i = size_t(y / s) * w + x / s;
          uint8_t* o = &img[(size_t(y) * W + index * (pw + 4) + x) * 4];
          const uint8_t* p = &g.rgba[i * 4];
          if (overlay) {
            const uint8_t m = e.mask[i];
            o[0] = m ? 255 : p[0] / 3;
            o[1] = (m & 1) ? 0 : m ? 160 : p[1] / 3;
            o[2] = m ? 0 : p[2] / 3;
          } else {
            o[0] = p[0];
            o[1] = p[1];
            o[2] = p[2];
          }
          o[3] = 255;
        }
      }
    };
    panel(0, e.prev, false);
    panel(1, e.cur, false);
    panel(2, e.next, false);
    panel(3, e.cur, true);
    return WritePng(path, img, W, h * s);
  }

  // mutex_ held.
  void Write() {
    namespace fs = std::filesystem;
    const fs::path dir(Folder());
    std::error_code ec;
    fs::create_directories(dir, ec);
    uint64_t flash_total = 0, crack_total = 0;
    uint32_t flash_frames = 0, crack_frames = 0, worst_flash = 0, worst_crack = 0;
    uint64_t worst_flash_frame = 0, worst_crack_frame = 0;
    if (FILE* f = std::fopen((dir / "flash-frames.csv").c_str(), "w")) {
      std::fprintf(f, "frame,flash_cluster,crack_cluster,flash_cells,crack_cells,luma\n");
      for (const Row& r : rows_) {
        std::fprintf(f, "%llu,%u,%u,%u,%u,%.4f\n", (unsigned long long)r.frame, r.flash_cluster, r.crack_cluster,
                     r.flash_cells, r.crack_cells, r.luma);
        flash_total += r.flash_cells;
        crack_total += r.crack_cells;
        flash_frames += r.flash_cluster >= kClusterCells;
        crack_frames += r.crack_cluster >= kClusterCells;
        if (r.flash_cluster > worst_flash) { worst_flash = r.flash_cluster; worst_flash_frame = r.frame; }
        if (r.crack_cluster > worst_crack) { worst_crack = r.crack_cluster; worst_crack_frame = r.frame; }
      }
      std::fclose(f);
    }
    std::string induced;
    uint32_t induced_scored = 0, induced_flash = 0, induced_crack = 0;
    for (const Induced& d : induced_) {
      for (const Row& r : rows_) {
        if (r.frame != d.frame) continue;
        ++induced_scored;
        induced_flash += r.flash_cluster >= kClusterCells;
        induced_crack += r.crack_cluster >= kClusterCells;
        char item[128];
        std::snprintf(item, sizeof item, "%s\n  {\"frame\": %llu, \"shape\": \"%s\", \"flash\": %u, \"crack\": %u}",
                      induced.empty() ? "" : ",", (unsigned long long)d.frame, d.kind, r.flash_cluster,
                      r.crack_cluster);
        induced += item;
      }
    }
    std::string ex;
    for (size_t k = 0; k < examples_.size(); ++k) {
      const Example& e = examples_[k];
      char name[64];
      std::snprintf(name, sizeof name, "flash-%zu-f%llu.png", k + 1, (unsigned long long)e.frame);
      const bool ok = WriteExample((dir / name).string(), e);
      char line[256];
      std::snprintf(line, sizeof line,
                    "%s\n  {\"rank\": %zu, \"frame\": %llu, \"flash_cluster\": %u, \"crack_cluster\": %u, "
                    "\"png\": \"%s\"}",
                    k ? "," : "", k + 1, (unsigned long long)e.frame, e.flash, e.crack, ok ? name : "");
      ex += line;
    }
    if (FILE* f = std::fopen((dir / "flash.json.tmp").c_str(), "w")) {
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
      std::fprintf(f,
                   "{\n \"schema\": 2,\n \"grid_w\": %u,\n \"min_luma\": %d,\n \"min_delta\": %d,\n"
                   " \"min_white\": %d,\n \"border\": %.2f,\n \"sky_top\": %.2f,\n \"cluster_cells\": %u,\n \"window_s\": %.2f,\n"
                   " \"frames_scored\": %zu,\n \"frames_skipped\": %llu,\n"
                   " \"flash_frames\": %u,\n \"crack_frames\": %u,\n \"flash_cells_total\": %llu,\n"
                   " \"crack_cells_total\": %llu,\n \"worst_flash_cluster\": %u,\n \"worst_flash_frame\": %llu,\n"
                   " \"worst_crack_cluster\": %u,\n \"worst_crack_frame\": %llu,\n"
                   " \"induced\": %zu,\n \"induced_scored\": %u,\n \"induced_flash_detected\": %u,\n"
                   " \"induced_crack_detected\": %u,\n \"induced_frames\": [%s\n ],\n"
                   " \"examples\": [%s\n ]\n}\n",
                   kGridW, kMinLuma, kMinDelta, kMinWhite, double(kBorder), double(kSkyTop), kClusterCells, seconds, rows_.size(),
                   (unsigned long long)skipped_, flash_frames, crack_frames, (unsigned long long)flash_total,
                   (unsigned long long)crack_total, worst_flash, (unsigned long long)worst_flash_frame, worst_crack,
                   (unsigned long long)worst_crack_frame, induced_.size(), induced_scored, induced_flash,
                   induced_crack, induced.c_str(), ex.c_str());
      std::fclose(f);
      fs::rename(dir / "flash.json.tmp", dir / "flash.json", ec);
    }
    REXLOG_INFO("[flash] window closed: {} frames scored, {} skipped; flash frames {} (largest {} cells, frame {}), "
                "crack frames {} (largest {} cells, frame {})",
                rows_.size(), skipped_, flash_frames, worst_flash, worst_flash_frame, crack_frames, worst_crack,
                worst_crack_frame);
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> active_{false};
  id<MTLComputePipelineState> pipeline_ = nil;
  id<MTLComputePipelineState> induce_pipeline_ = nil;
  uint32_t induce_counter_ = 0, rng_ = 12345;
  std::vector<Induced> induced_;
  bool failed_ = false;
  std::vector<id<MTLBuffer>> free_;
  int in_flight_ = 0;
  uint64_t skipped_ = 0;
  std::deque<Grid> ring_;
  std::vector<Row> rows_;
  std::vector<Example> examples_;
  std::chrono::steady_clock::time_point started_{};
};

}  // namespace rex::ui::metal::flash
