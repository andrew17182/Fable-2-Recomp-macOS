// [tile-resolve] Colour resolves as tile functions inside the draw pass that
// rendered the source (docs/tile-resolve-20261013.md).
//
// Today a resolve ends the draw pass and runs a compute encoder that reads the
// render target back from memory. Here, when the source is colour attachment 0
// of the still-open pass, the same XeniOS resolve code runs as a tile function
// (dispatchThreadsPerTile) after the pass's last draw, reading the attachment
// from tile memory. It writes the same destination (shared memory or the
// scaled-resolve buffer) and the same fused textures. The pass then ends where
// it ends today, so every later pass boundary, clear and transfer is
// unchanged; only the compute encoder and its attachment read go away.
//
// Switches: FABLE2_TILE_RESOLVE=0 (default on), FABLE2_TILE_RESOLVE_VERIFY=1
// (also run the compute resolve and compare bytes), FABLE2_TILE_RESOLVE_VERIFY_BREAK=1
// (negative control), FABLE2_TILE_RESOLVE_AREA_RATIO=<x> (default 1.5),
// FABLE2_TILE_RESOLVE_TRACE=1 (log fallback reasons), FABLE2_TILE_RESOLVE_MSAA=0
// (MSAA sources stay on compute). Follow-ups: FABLE2_TILE_RESOLVE_EXTENT=0,
// FABLE2_TILE_RESOLVE_DRAWN_GATE=0, FABLE2_TILE_RESOLVE_CLEAR=0,
// FABLE2_TILE_RESOLVE_PREWARM=0 (kill switches), FABLE2_TILE_RESOLVE_KEEP_OPEN=1.

#include "rex/graphics/metal/metal_render_target_cache.h"
#include "rex/graphics/metal/metal_miplod_proof.h"

#include <dispatch/dispatch.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_pass_trace.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/graphics/metal/metal_texture_cache.h"
#include "fmt/format.h"
#include "rex/logging.h"

#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_1xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_1xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_2xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_2xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_4xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_32bpp_4xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_1xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_1xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_2xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_2xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_4xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_64bpp_4xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_1xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_1xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_2xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_2xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_4xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_8bpp_4xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_1xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_1xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_2xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_2xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_4xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_16bpp_4xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_1xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_1xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_2xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_2xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_4xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_32bpp_4xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_1xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_1xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_2xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_2xmsaa_scaled_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_4xmsaa_rtt_tile.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_host_color_full_64bpp_4xmsaa_scaled_rtt_tile.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {

struct TileLibrary {
  const char* name;
  const uint8_t* data;
  size_t size;
};
#define XE_TILE_LIB(id) {#id, id##_metallib, sizeof(id##_metallib)}
const TileLibrary kTileLibraries[] = {
    XE_TILE_LIB(resolve_host_color_32bpp_1xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_32bpp_1xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_32bpp_2xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_32bpp_2xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_32bpp_4xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_32bpp_4xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_64bpp_1xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_64bpp_1xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_64bpp_2xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_64bpp_2xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_64bpp_4xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_64bpp_4xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_8bpp_1xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_8bpp_1xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_8bpp_2xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_8bpp_2xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_8bpp_4xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_8bpp_4xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_16bpp_1xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_16bpp_1xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_16bpp_2xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_16bpp_2xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_16bpp_4xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_16bpp_4xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_32bpp_1xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_32bpp_1xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_32bpp_2xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_32bpp_2xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_32bpp_4xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_32bpp_4xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_64bpp_1xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_64bpp_1xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_64bpp_2xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_64bpp_2xmsaa_scaled_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_64bpp_4xmsaa_rtt_tile),
    XE_TILE_LIB(resolve_host_color_full_64bpp_4xmsaa_scaled_rtt_tile),
};
#undef XE_TILE_LIB

const TileLibrary* FindTileLibrary(const std::string& name) {
  for (const TileLibrary& lib : kTileLibraries) {
    if (name == lib.name) return &lib;
  }
  return nullptr;
}

bool EnvOn(const char* name) {
  const char* v = std::getenv(name);  // getenv-ok: read once by callers
  return v && v[0] == '1';
}

bool VerifyOn() {
  static const bool on = EnvOn("FABLE2_TILE_RESOLVE_VERIFY");
  return on;
}
bool VerifyBreakOn() {
  static const bool on = EnvOn("FABLE2_TILE_RESOLVE_VERIFY_BREAK");
  return on;
}
bool TraceOn() {
  static const bool on = EnvOn("FABLE2_TILE_RESOLVE_TRACE");
  return on;
}
// FABLE2_TILE_RESOLVE_MSAA=0: 2x/4x MSAA sources stay on the compute path.
bool MsaaOn() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TILE_RESOLVE_MSAA");  // getenv-ok: once
    return !(v && v[0] == '0');
  }();
  return on;
}
double AreaRatio() {
  static const double ratio = [] {
    const char* v = std::getenv("FABLE2_TILE_RESOLVE_AREA_RATIO");  // getenv-ok: once
    double r = v ? std::atof(v) : 1.5;
    return r > 0.0 ? r : 1.5;
  }();
  return ratio;
}

// FABLE2_TILE_RESOLVE_DRAWN_GATE=0: the original gate (render area against
// the resolve rectangle). Default: render area against the area drawn.
bool DrawnGate() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TILE_RESOLVE_DRAWN_GATE");  // getenv-ok: once
    return !(v && v[0] == '0');
  }();
  return on;
}

void TraceFallback(const char* reason, const std::string& detail) {
  if (!TraceOn()) return;
  static std::map<std::string, uint64_t> counts;
  static uint64_t total = 0;
  counts[std::string(reason) + " " + detail]++;
  if (++total % 2048 == 0) {
    for (const auto& e : counts) {
      REXLOG_INFO("[tile-resolve] fallback {:8} {}", e.second, e.first);
    }
    counts.clear();
  }
}

// Verify state: snapshots taken after the tile resolve (B), compared with the
// same regions after the reference compute resolve (A).
struct VerifyRegion {
  MTL::Buffer* buffer = nullptr;   // destination buffer region, or
  MTL::Texture* texture = nullptr;  // a fused texture level
  size_t offset = 0, length = 0;
  uint32_t level = 0, width = 0, height = 0, texel_bytes = 0;
};
struct VerifyPending {
  bool active = false;
  std::vector<VerifyRegion> regions;
  MTL::Buffer* snapshot_b = nullptr;
  size_t total = 0;
};
VerifyPending g_verify;
std::atomic<uint64_t> g_verify_checked{0}, g_verify_mismatched{0},
    g_verify_bytes{0}, g_verify_bad_bytes{0};

void EncodeSnapshot(MTL::CommandBuffer* cmd, const VerifyPending& v,
                    MTL::Buffer* into, const char* site) {
  MTL::BlitCommandEncoder* blit = pass_trace::Blit(cmd, site);
  if (!blit) return;
  size_t at = 0;
  for (const VerifyRegion& r : v.regions) {
    if (r.buffer) {
      blit->copyFromBuffer(r.buffer, r.offset, into, at, r.length);
    } else {
      blit->copyFromTexture(r.texture, 0, r.level, MTL::Origin::Make(0, 0, 0),
                            MTL::Size::Make(r.width, r.height, 1), into, at,
                            size_t(r.width) * r.texel_bytes, r.length);
    }
    at += (r.length + 255) & ~size_t(255);
  }
  blit->endEncoding();
}

}  // namespace

struct MetalRenderTargetCache::TileResolvePipelines {
  struct Entry {
    MTL::RenderPipelineState* pso = nullptr;
    bool done = false;
    bool used = false;       // requested by a resolve
    bool prewarmed = false;  // queued by the startup prewarm
  };
  struct Job {
    std::string key;
    const TileLibrary* lib;
    MTL::PixelFormat formats[8];
    uint32_t samples;
  };
  MTL::Device* device = nullptr;
  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<std::string, Entry> entries;
  std::deque<Job> queue;
  bool stop = false;
  std::thread worker;
  // Worker thread only.
  std::unordered_map<std::string, MTL::Function*> functions;

  void Run() {
    pthread_setname_np("Tile Resolve PSO");
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return stop || !queue.empty(); });
        if (stop) break;
        job = queue.front();
        queue.pop_front();
      }
      NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
      const auto t0 = std::chrono::steady_clock::now();
      MTL::Function* fn = nullptr;
      auto it = functions.find(job.lib->name);
      if (it != functions.end()) {
        fn = it->second;
      } else {
        NS::Error* error = nullptr;
        dispatch_data_t data = dispatch_data_create(
            job.lib->data, job.lib->size, nullptr,
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        MTL::Library* library = device->newLibrary(data, &error);
        dispatch_release(data);
        if (library) {
          fn = library->newFunction(
              NS::String::string("entry_xe", NS::UTF8StringEncoding));
          library->release();
        }
        if (!fn) {
          REXLOG_WARN("[tile-resolve] no tile function in {}: {}",
                      job.lib->name,
                      error ? error->localizedDescription()->utf8String()
                            : "unknown");
        }
        functions[job.lib->name] = fn;
      }
      MTL::RenderPipelineState* pso = nullptr;
      if (fn) {
        MTL::TileRenderPipelineDescriptor* desc =
            MTL::TileRenderPipelineDescriptor::alloc()->init();
        desc->setTileFunction(fn);
        desc->setLabel(
            NS::String::string(job.lib->name, NS::UTF8StringEncoding));
        for (uint32_t i = 0; i < 8; ++i) {
          desc->colorAttachments()->object(i)->setPixelFormat(job.formats[i]);
        }
        desc->setRasterSampleCount(job.samples);
        desc->setThreadgroupSizeMatchesTileSize(false);
        NS::Error* error = nullptr;
        pso = device->newRenderPipelineState(desc, MTL::PipelineOptionNone,
                                             nullptr, &error);
        desc->release();
        if (!pso) {
          REXLOG_WARN("[tile-resolve] tile pipeline {} failed: {}",
                      job.lib->name,
                      error ? error->localizedDescription()->utf8String()
                            : "unknown");
        }
      }
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
      static uint32_t logged = 0;
      if (logged < 24) {
        ++logged;
        REXLOG_INFO("[tile-resolve] built {} in {:.1f} ms (background): {}",
                    job.key, ms, pso ? "ok" : "failed");
      }
      pool->release();
      std::lock_guard<std::mutex> lock(mutex);
      Entry& e = entries[job.key];
      e.pso = pso;
      e.done = true;
      if (e.prewarmed && ++prewarm_built == prewarm_queued) {
        REXLOG_INFO(
            "[tile-resolve] prewarm: {} tile pipelines built {:.2f} s after "
            "start, {} already requested by resolves",
            prewarm_queued,
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          prewarm_start)
                .count(),
            prewarm_used);
      }
    }
  }

  // Prewarm/persistence: keys first requested this run are appended to
  // keys_path (once storage is known; earlier ones wait in early_keys).
  std::filesystem::path keys_path;
  std::vector<std::string> early_keys;
  uint32_t prewarm_queued = 0, prewarm_built = 0;
  std::chrono::steady_clock::time_point prewarm_start;

  static std::string KeyFor(const TileLibrary* lib,
                            const MTL::PixelFormat formats[8],
                            uint32_t samples) {
    std::string key = lib->name;
    for (uint32_t i = 0; i < 8; ++i) {
      key += '|' + std::to_string(uint32_t(formats[i]));
    }
    key += '|' + std::to_string(samples);
    return key;
  }

  // mutex held.
  void QueueLocked(const std::string& key, const TileLibrary* lib,
                   const MTL::PixelFormat formats[8], uint32_t samples) {
    entries.emplace(key, Entry());
    Job job;
    job.key = key;
    job.lib = lib;
    std::memcpy(job.formats, formats, sizeof(job.formats));
    job.samples = samples;
    queue.push_back(job);
    if (!worker.joinable()) {
      worker = std::thread([this] { Run(); });
    }
    cv.notify_one();
  }

  // mutex held.
  void RecordLocked(const std::string& key) {
    if (keys_path.empty()) {
      early_keys.push_back(key);
      return;
    }
    std::ofstream out(keys_path, std::ios::app);
    if (out) out << key << '\n';
  }

  // Render thread. Null until built in the background (or if it failed).
  MTL::RenderPipelineState* Get(const TileLibrary* lib,
                                const MTL::PixelFormat formats[8],
                                uint32_t samples) {
    const std::string key = KeyFor(lib, formats, samples);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = entries.find(key);
    if (it != entries.end()) {
      if (!it->second.used) {
        it->second.used = true;
        if (it->second.prewarmed) {
          ++prewarm_used;
        }
      }
      return it->second.done ? it->second.pso : nullptr;
    }
    QueueLocked(key, lib, formats, samples);
    entries[key].used = true;
    RecordLocked(key);
    return nullptr;
  }
  uint32_t prewarm_used = 0;

  ~TileResolvePipelines() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
    for (auto& e : entries) {
      if (e.second.pso) e.second.pso->release();
    }
    for (auto& f : functions) {
      if (f.second) f.second->release();
    }
  }
};

bool MetalRenderTargetCache::TileResolveEnabled() {
  // Default on since 2026-10-13 (docs/tile-resolve-20261013.md, follow-up);
  // FABLE2_TILE_RESOLVE=0 is the kill switch.
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TILE_RESOLVE");  // getenv-ok: once
    const bool enabled = !(v && v[0] == '0');
    if (enabled) {
      REXLOG_INFO(
          "[tile-resolve] on (FABLE2_TILE_RESOLVE=0 turns it off): colour "
          "copies run as tile functions in the draw pass{}{}, area ratio "
          "{:.2f} ({} gate)",
          VerifyOn() ? ", VERIFY against the compute resolve" : "",
          VerifyBreakOn() ? ", VERIFY_BREAK (negative control)" : "",
          AreaRatio(), DrawnGate() ? "drawn" : "area");
    }
    return enabled;
  }();
  return on;
}

namespace {
// Pipelines seen in Bower Lake / Old Tomb / Bowerstone (1x-3x); a new Mac's
// first launch builds these before the first resolve asks for them.
// Format: <library>|<8 colour pixel formats>|<sample count>.
constexpr const char* kTileResolveDefaultKeys[] = {
#include "metal_tile_resolve_keys.inc"
    nullptr,
};
}  // namespace

void MetalRenderTargetCache::StartTileResolvePrewarm(
    const std::filesystem::path& title_root, uint32_t title_id) {
  if (!TileResolveEnabled()) return;
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_TILE_RESOLVE_PREWARM");  // getenv-ok: once
    return !(v && v[0] == '0');
  }();
  if (!tile_resolve_pipelines_) {
    tile_resolve_pipelines_ = new TileResolvePipelines();
    tile_resolve_pipelines_->device = device_;
  }
  TileResolvePipelines& p = *tile_resolve_pipelines_;
  const std::filesystem::path path =
      title_root / fmt::format("{:08X}.tres-keys.txt", title_id);
  std::vector<std::string> keys;
  {
    std::ifstream in(path);
    std::string line;
    while (in && std::getline(in, line)) keys.push_back(line);
  }
  const bool scaled = resolve_scale_x_ > 1 || resolve_scale_y_ > 1;
  for (const char* k : kTileResolveDefaultKeys) {
    if (k) keys.push_back(k);
  }
  std::lock_guard<std::mutex> lock(p.mutex);
  {
    std::error_code ec;
    std::filesystem::create_directories(title_root, ec);
  }
  p.keys_path = path;
  for (const std::string& k : p.early_keys) {
    std::ofstream out(path, std::ios::app);
    if (out) out << k << '\n';
  }
  p.early_keys.clear();
  if (!enabled) return;
  p.prewarm_start = std::chrono::steady_clock::now();
  for (const std::string& key : keys) {
    // <library>|f0|...|f7|samples
    std::vector<std::string> parts;
    std::stringstream ss(key);
    std::string part;
    while (std::getline(ss, part, '|')) parts.push_back(part);
    if (parts.size() != 10) continue;
    const TileLibrary* lib = FindTileLibrary(parts[0]);
    if (!lib) continue;
    // Unscaled launches never use the scaled variants.
    if (!scaled && std::strstr(lib->name, "_scaled_")) continue;
    MTL::PixelFormat formats[8];
    for (uint32_t i = 0; i < 8; ++i) {
      formats[i] = MTL::PixelFormat(std::strtoul(parts[1 + i].c_str(), nullptr, 10));
    }
    const uint32_t samples =
        uint32_t(std::strtoul(parts[9].c_str(), nullptr, 10));
    if (samples != 1 && samples != 2 && samples != 4) continue;
    const std::string canon = TileResolvePipelines::KeyFor(lib, formats, samples);
    if (p.entries.count(canon)) continue;
    p.QueueLocked(canon, lib, formats, samples);
    p.entries[canon].prewarmed = true;
    ++p.prewarm_queued;
  }
  REXLOG_INFO("[tile-resolve] prewarm: building {} tile pipelines in the "
              "background (FABLE2_TILE_RESOLVE_PREWARM)",
              p.prewarm_queued);
}

uint32_t MetalRenderTargetCache::TileResolvePrewarmPending() {
  // [shader-prep] tile pipelines queued by the startup prewarm and not built yet.
  TileResolvePipelines* p = tile_resolve_pipelines_;
  if (!p) return 0;
  std::lock_guard<std::mutex> lock(p->mutex);
  return p->prewarm_queued > p->prewarm_built ? p->prewarm_queued - p->prewarm_built : 0;
}

bool MetalRenderTargetCache::TileResolveDrawClearInPass(
    RenderTarget* rt, bool depth, uint64_t clear_value,
    const Transfer::Rectangle& rect, Transfer::Rectangle& remainder,
    bool& has_remainder) {
  has_remainder = false;
  if (!rt || HasPendingDrawPassTransfers() || !rect.width_pixels ||
      !rect.height_pixels) {
    return false;
  }
  // A shorter render area: the rows of the clear inside it are drawn here,
  // the rest (rows this pass never touched) stay a deferred clear.
  Transfer::Rectangle in_pass = rect;
  if (const uint32_t extent_h =
          command_processor_.TileResolvePassExtentHeight()) {
    const uint32_t scale_y = std::max<uint32_t>(1, RenderTargetScaleY(rt->key()));
    const uint32_t ext_rows = extent_h / scale_y;
    if (rect.y_pixels >= ext_rows) return false;
    const uint32_t end = rect.y_pixels + rect.height_pixels;
    if (end > ext_rows) {
      in_pass.height_pixels = ext_rows - rect.y_pixels;
      remainder = rect;
      remainder.y_pixels = ext_rows;
      remainder.height_pixels = end - ext_rows;
      has_remainder = true;
    }
  }
  MTL::RenderCommandEncoder* encoder =
      command_processor_.TileResolveRenderEncoder();
  MTL::RenderPassDescriptor* pass =
      command_processor_.TileResolvePassDescriptor();
  if (!encoder || !pass) return false;
  auto* mrt = static_cast<MetalRenderTarget*>(rt);
  uint32_t index = UINT32_MAX;
  if (depth) {
    MTL::RenderPassDepthAttachmentDescriptor* da = pass->depthAttachment();
    if (da && da->texture() && da->texture() == mrt->texture()) index = 0;
  } else {
    for (uint32_t j = 0; j < xenos::kMaxColorRenderTargets; ++j) {
      MTL::RenderPassColorAttachmentDescriptor* ca =
          pass->colorAttachments()->object(j);
      if (ca && ca->texture() && ca->texture() == mrt->transfer_texture()) {
        index = j + 1;
        break;
      }
    }
  }
  if (index == UINT32_MAX) {
    has_remainder = false;
    return false;
  }
  pending_draw_pass_clear_targets_[index] = rt;
  pending_draw_pass_clear_values_[index] = clear_value;
  pending_draw_pass_clear_rects_[index] = in_pass;
  pending_draw_pass_clear_mask_ = uint32_t(1) << index;
  DrawPassTransferEncoderMutationMask mutations =
      kDrawPassTransferEncoderMutationNone;
  const bool ok = EncodePendingDrawPassTransfers(encoder, pass, &mutations);
  command_processor_.InvalidateRenderEncoderStateAfterDrawPassTransfers(
      mutations);
  if (!ok) {
    // Not drawn (or not completely): the caller defers it as before (a
    // clear is idempotent).
    pending_draw_pass_clear_targets_[index] = nullptr;
    pending_draw_pass_clear_mask_ = 0;
    has_remainder = false;
    return false;
  }
  return true;
}

bool MetalRenderTargetCache::TileResolveKeepOpenEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TILE_RESOLVE_KEEP_OPEN");  // getenv-ok: once
    return TileResolveEnabled() && v && v[0] == '1';
  }();
  return on;
}

bool MetalRenderTargetCache::TileResolveClearInPassEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TILE_RESOLVE_CLEAR");  // getenv-ok: once
    return TileResolveEnabled() && !(v && v[0] == '0');
  }();
  return on;
}

bool MetalRenderTargetCache::TileResolveExtentEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TILE_RESOLVE_EXTENT");  // getenv-ok: once
    return TileResolveEnabled() && !(v && v[0] == '0');
  }();
  return on;
}

void MetalRenderTargetCache::TileResolveShutdown() {
  delete tile_resolve_pipelines_;
  tile_resolve_pipelines_ = nullptr;
}

bool MetalRenderTargetCache::TileResolveOffer(const ResolvePlan& plan) {
  tile_resolve_open_ = false;
  if (!TileResolveEnabled() || !plan.needs_copy_export ||
      plan.resolve_info.IsCopyingDepth() ||
      GetPath() != Path::kHostRenderTargets) {
    return false;
  }
  tile_resolve_open_ = true;
  tile_resolve_gen_ = command_processor_.TileResolveRenderEncoderGen();
  ++UploadTrace().tres_offer;
  return true;
}

void MetalRenderTargetCache::TileResolveEnd(const char* detail) {
  if (!tile_resolve_open_) return;
  tile_resolve_open_ = false;
  // Only the pass that was offered; if it already ended, anything open now
  // belongs to the resolve's own work and ends as it does today.
  if (command_processor_.TileResolveRenderEncoder() &&
      command_processor_.TileResolveRenderEncoderGen() == tile_resolve_gen_) {
    command_processor_.EndRenderEncoderForResolve(detail);
  }
}

bool MetalRenderTargetCache::TileResolvePrecheck(const ResolvePlan& plan) {
  // Everything Resolve does before the copy must be GPU-free: no render-target
  // growth, no deferred clear to flush, no rt-shrink verify readback.
  // Deferred clears only matter for the copy's own source targets (Resolve
  // flushes exactly those, FlushDeferredClearsFor).
  bool source_clear_pending = false;
  if (!deferred_clears_.empty() || pending_draw_pass_clear_mask_) {
    uint32_t span_base, span_row_length, span_rows, span_pitch;
    plan.resolve_info.GetCopyEdramTileSpan(span_base, span_row_length,
                                           span_rows, span_pitch);
    std::vector<ResolveCopyDumpRectangle> owner_rects;
    GetResolveCopyRectanglesToDump(span_base, span_row_length, span_rows,
                                   span_pitch, owner_rects);
    for (const ResolveCopyDumpRectangle& rect : owner_rects) {
      for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
        if ((pending_draw_pass_clear_mask_ & (uint32_t(1) << i)) &&
            pending_draw_pass_clear_targets_[i] == rect.render_target) {
          source_clear_pending = true;
        }
      }
      for (const DeferredClear& clear : deferred_clears_) {
        if (clear.render_target == rect.render_target) {
          source_clear_pending = true;
        }
      }
    }
  }
  if (!rt_shrink_pending_.empty() || RtShrinkVerify() ||
      source_clear_pending || queued_stencil_alias_.render_target) {
    ++UploadTrace().tres_fb_precheck;
    TraceFallback("precheck",
                  fmt::format("grow{} clears{} drawpass{} src_clear{} w{}h{}",
                              rt_shrink_pending_.size(),
                              deferred_clears_.size(),
                              pending_draw_pass_clear_mask_,
                              source_clear_pending ? 1 : 0,
                              uint32_t(plan.resolve_info.coordinate_info
                                           .width_div_8) << 3,
                              plan.resolve_info.height_div_8 << 3));
    return false;
  }
  return true;
}

bool MetalRenderTargetCache::TryTileResolveEncode(
    const TileResolveRequest& r) {
  const draw_util::ResolveInfo& ri = *r.resolve_info;
  auto fallback = [&](uint32_t& counter, const char* reason) {
    ++counter;
    TraceFallback(reason, fmt::format("{} w{}h{}", r.pipeline_label,
                                      uint32_t(ri.coordinate_info.width_div_8)
                                          << 3,
                                      ri.height_div_8 << 3));
    return false;
  };
  UploadTraceCounters& u = UploadTrace();
  if (!tile_resolve_open_ ||
      command_processor_.TileResolveRenderEncoderGen() != tile_resolve_gen_ ||
      !command_processor_.TileResolveRenderEncoder()) {
    return fallback(u.tres_fb_pass, "pass-ended");
  }
  MTL::RenderCommandEncoder* encoder =
      command_processor_.TileResolveRenderEncoder();
  MTL::RenderPassDescriptor* pass =
      command_processor_.TileResolvePassDescriptor();
  if (!pass) return fallback(u.tres_fb_pass, "no-descriptor");

  std::string label = r.pipeline_label ? r.pipeline_label : "";
  const std::string suffix = "_cs";
  if (label.size() <= suffix.size() ||
      label.compare(label.size() - suffix.size(), suffix.size(), suffix)) {
    return fallback(u.tres_fb_shape, "label");
  }
  const TileLibrary* lib = FindTileLibrary(
      label.substr(0, label.size() - suffix.size()) + "_rtt_tile");
  if (!lib) return fallback(u.tres_fb_shape, "variant");

  // The source must be colour attachment 0 of the open pass, as the same
  // texture object the compute resolve reads.
  MTL::RenderPassColorAttachmentDescriptor* a0 =
      pass->colorAttachments()->object(0);
  if (!a0 || a0->texture() != r.source_texture) {
    return fallback(u.tres_fb_pass, "not-attachment0");
  }
  if (r.source_texture->sampleCount() > 1 && !MsaaOn()) {
    return fallback(u.tres_fb_shape, "msaa-off");
  }

  // Area gate (stage-0 microbench): the tile dispatch visits every tile of the
  // render area, so tiles no draw touched cost a load and a store that the
  // pass alone would skip. The render area is the pass's (shorter than the
  // texture when the extent shrink opened it so).
  const uint32_t extent_h = command_processor_.TileResolvePassExtentHeight();
  const double tex_w = double(r.source_texture->width());
  const double area_h =
      extent_h ? double(std::min<uint32_t>(
                     extent_h, uint32_t(r.source_texture->height())))
               : double(r.source_texture->height());
  if (extent_h && r.source_rows_end > extent_h) {
    // The resolve reads rows outside the render area.
    return fallback(u.tres_fb_extent, "beyond-extent");
  }
  const double area = tex_w * area_h;
  const double rect = double(uint32_t(ri.coordinate_info.width_div_8) * 8u *
                             resolve_scale_x_) *
                      double(ri.height_div_8 * 8u * resolve_scale_y_);
  // Small targets: the untouched part is cheap whatever the ratio (64K
  // pixels, about 256 KB at 32 bpp; the 32x32 mip-chain levels of an 80-wide
  // EDRAM pitch).
  if (!(rect > 0.0)) return fallback(u.tres_fb_area, "area");
  if (DrawnGate()) {
    // Untouched = render area minus the bounding box of the pass's draws.
    const double drawn = std::min(
        area, double(command_processor_.TileResolvePassDrawnArea()));
    const double untouched = area - drawn;
    if (untouched > (AreaRatio() - 1.0) * rect && untouched > 65536.0) {
      return fallback(u.tres_fb_area, "area-untouched");
    }
  } else if (area > AreaRatio() * rect && area - rect > 65536.0) {
    return fallback(u.tres_fb_area, "area");
  }

  // Hazards inside the pass: a draw of this pass may sample a fused target
  // (the tile writes would race other tiles' reads).
  auto* texture_cache =
      static_cast<MetalTextureCache*>(command_processor_.texture_cache());
  // [mip-lod] a fused target that is one stored mip level of a texture the
  // pass samples is safe when every draw of the pass that samples it is
  // bounded (metal_miplod_proof.h) to lower levels through a view that ends
  // below the written level: no draw can read what the tile stage writes.
  bool miplod_safe[2] = {false, false};
  if (r.fused && texture_cache && miplod::Enabled() && r.rtt_params) {
    const auto* p =
        static_cast<const MetalTextureCache::RttParams*>(r.rtt_params);
    for (uint32_t i = 0; i < 2 && i < p->count; ++i) {
      miplod_safe[i] = r.fused_textures[i] && p->t[i].level &&
                       texture_cache->MipLodPassSafeForLevel(
                           r.fused_textures[i], p->t[i].level) == 2;
    }
    if (p->count == 1) miplod_safe[1] = miplod_safe[0];
  }
  if (r.fused && texture_cache) {
    for (uint32_t i = 0; i < 2; ++i) {
      const void* t = r.fused_textures[i];
      if (t && !miplod_safe[i] &&
          !texture_cache->TileResolveTextureIdleInOpenPass(t)) {
        return fallback(u.tres_fb_hazard, "fused-target-sampled");
      }
    }
  }
  // Also by the draws actually encoded in this pass (a draw may sample a
  // texture through descriptors bound before the pass began, which the bind
  // serials above do not see).
  if (r.fused) {
    for (uint32_t i = 0; i < 2; ++i) {
      MTL::Texture* v = r.fused_views[i];
      if (v && !miplod_safe[i] && command_processor_.TileResolvePassSampled(v)) {
        if (MetalTextureCache::MipLodTraceEnabled() && texture_cache) {
          const auto* p =
              static_cast<const MetalTextureCache::RttParams*>(r.rtt_params);
          for (uint32_t i = 0; i < 2 && p && i < p->count; ++i) {
            texture_cache->MipLodTraceHazard(r.fused_textures[i], p->t[i].level,
                                             "in-pass");
          }
        }
        return fallback(u.tres_fb_hazard, "fused-target-sampled-in-pass");
      }
    }
  }

  const uint32_t tile_w = uint32_t(encoder->tileWidth());
  const uint32_t tile_h = uint32_t(encoder->tileHeight());
  if (!tile_w || !tile_h || tile_w % r.pixels_per_thread) {
    return fallback(u.tres_fb_tile, "tile-width");
  }

  if (!tile_resolve_pipelines_) {
    tile_resolve_pipelines_ = new TileResolvePipelines();
    tile_resolve_pipelines_->device = device_;
  }
  MTL::PixelFormat formats[8];
  for (uint32_t i = 0; i < 8; ++i) {
    MTL::RenderPassColorAttachmentDescriptor* a =
        pass->colorAttachments()->object(i);
    formats[i] = (a && a->texture()) ? a->texture()->pixelFormat()
                                     : MTL::PixelFormatInvalid;
  }
  MTL::RenderPipelineState* pso = tile_resolve_pipelines_->Get(
      lib, formats, uint32_t(std::max<NS::UInteger>(
                         1, r.source_texture->sampleCount())));
  if (!pso) return fallback(u.tres_fb_pso, "pso-not-ready");

  // Encode. The pipeline state of the encoder changes; the pass ends right
  // after (TileResolveEnd), which resets the cached draw state.
  encoder->setRenderPipelineState(pso);
  encoder->setTileBytes(r.push_constants, r.push_constants_size, 0);
  encoder->setTileBuffer(r.dest_buffer, r.dest_offset, 1);
  encoder->setTileBytes(r.host_constants, r.host_constants_size, 2);
  encoder->setTileTexture(r.fused ? r.fused_views[0] : nullptr, 4);
  encoder->setTileTexture(r.fused ? r.fused_views[1] : nullptr, 5);
  encoder->setTileBytes(r.rtt_params, r.rtt_params_size, 6);
  encoder->useResource(r.dest_buffer, MTL::ResourceUsageWrite,
                       MTL::RenderStageTile);
  for (MTL::Texture* v : r.fused_views) {
    if (r.fused && v) {
      encoder->useResource(v, MTL::ResourceUsageWrite, MTL::RenderStageTile);
    }
  }
  encoder->dispatchThreadsPerTile(
      MTL::Size::Make(tile_w / r.pixels_per_thread, tile_h, 1));
  if (!r.scaled) {
    // Shared-memory destination: the pass's fence update at its end covers
    // the tile stage (MarkSharedMemoryRenderWritePending).
    command_processor_.MarkSharedMemoryRenderWritePending(
        ri.copy_dest_extent_start, ri.copy_dest_extent_length,
        MTL::RenderStageTile);
    if (auto* shared_memory = command_processor_.shared_memory()) {
      shared_memory->MarkGpuAccess(ri.copy_dest_extent_start,
                                   ri.copy_dest_extent_length,
                                   command_processor_.GetCurrentSubmission());
    }
  }
  ++u.tres_tile;
  if (miplod_safe[0] || miplod_safe[1]) {
    ++u.tres_miplod;
  }
  if (!VerifyOn()) {
    // Keep the pass for the resolve's clear (Resolve draws it here when it
    // can) and, with KEEP_OPEN, for the next draws; IssueCopy ends it.
    tile_resolve_open_ = false;
    MTL::Texture* written[2] = {r.fused ? r.fused_views[0] : nullptr,
                                r.fused ? r.fused_views[1] : nullptr};
    command_processor_.TileResolveKeep(written, 2);
  } else {
    TileResolveEnd("resolve:tile");
  }
  return true;
}

// Verify (FABLE2_TILE_RESOLVE_VERIFY=1): after the tile resolve, snapshot its
// output (B); the caller then runs the reference compute resolve over the same
// destination and calls TileResolveVerifyFinish, which snapshots it (A) and
// compares A with B on completion. The game keeps the compute result.
bool TileResolveVerifyActive() { return VerifyOn(); }

void TileResolveVerifyBegin(MTL::Device* device, MTL::CommandBuffer* cmd,
                            MTL::Buffer* dest_buffer, size_t dest_offset,
                            size_t dest_length, MTL::Texture* const views[2],
                            const uint32_t levels[2], const uint32_t widths[2],
                            const uint32_t heights[2],
                            const uint32_t texel_bytes[2], uint32_t count) {
  g_verify = {};
  VerifyRegion b;
  b.buffer = dest_buffer;
  b.offset = dest_offset;
  b.length = dest_length;
  g_verify.regions.push_back(b);
  for (uint32_t i = 0; i < count && i < 2; ++i) {
    if (!views[i] || !widths[i] || !heights[i] || !texel_bytes[i]) continue;
    VerifyRegion t;
    t.texture = views[i];
    t.level = levels[i];
    t.width = widths[i];
    t.height = heights[i];
    t.texel_bytes = texel_bytes[i];
    t.length = size_t(widths[i]) * texel_bytes[i] * heights[i];
    g_verify.regions.push_back(t);
  }
  size_t total = 0;
  for (const VerifyRegion& r : g_verify.regions) {
    total += (r.length + 255) & ~size_t(255);
  }
  g_verify.total = total;
  g_verify.snapshot_b =
      total ? device->newBuffer(total, MTL::ResourceStorageModeShared)
            : nullptr;
  if (!g_verify.snapshot_b) {
    g_verify = {};
    return;
  }
  EncodeSnapshot(cmd, g_verify, g_verify.snapshot_b, "tres_verify_b");
  g_verify.active = true;
}

void TileResolveVerifyFinish(MTL::Device* device, MTL::CommandBuffer* cmd,
                             const char* what) {
  if (!g_verify.active) return;
  MTL::Buffer* a =
      device->newBuffer(g_verify.total, MTL::ResourceStorageModeShared);
  MTL::Buffer* b = g_verify.snapshot_b;
  if (!a) {
    b->release();
    g_verify = {};
    return;
  }
  EncodeSnapshot(cmd, g_verify, a, "tres_verify_a");
  const size_t total = g_verify.total;
  std::string tag = what ? what : "";
  cmd->addCompletedHandler([a, b, total, tag](MTL::CommandBuffer*) {
    const uint8_t* pa = static_cast<const uint8_t*>(a->contents());
    const uint8_t* pb = static_cast<const uint8_t*>(b->contents());
    size_t bad = 0;
    for (size_t i = 0; i < total; ++i) {
      bad += pa[i] != pb[i];
    }
    const uint64_t checked = ++g_verify_checked;
    g_verify_bytes += total;
    g_verify_bad_bytes += bad;
    if (bad) {
      const uint64_t mism = ++g_verify_mismatched;
      if (mism <= 16) {
        REXLOG_WARN("[tile-resolve] verify MISMATCH {}: {} of {} bytes", tag,
                    bad, total);
      }
    }
    if (checked % 2000 == 0) {
      REXLOG_INFO(
          "[tile-resolve] verify: {} resolves checked, {} mismatched ({} of "
          "{} KB differ)",
          checked, g_verify_mismatched.load(), g_verify_bad_bytes.load() >> 10,
          g_verify_bytes.load() >> 10);
    }
    a->release();
    b->release();
  });
  g_verify = {};
}

void TileResolveVerifyReport() {
  if (!VerifyOn()) return;
  REXLOG_INFO(
      "[tile-resolve] verify: {} resolves checked, {} mismatched ({} of {} KB "
      "differ)",
      g_verify_checked.load(), g_verify_mismatched.load(),
      g_verify_bad_bytes.load() >> 10, g_verify_bytes.load() >> 10);
}

bool TileResolveVerifyBreak() { return VerifyBreakOn(); }

}  // namespace metal
}  // namespace graphics
}  // namespace rex
