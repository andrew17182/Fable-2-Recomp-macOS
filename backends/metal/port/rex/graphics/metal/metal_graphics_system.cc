#include "rex/ui/metal/metal_test_control.h"
#include "rex/graphics/metal/metal_frame_log.h"
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_graphics_system.h"

#include "rex/logging.h"
#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/metal/metal_pipeline_cache.h"
#include "rex/graphics/metal/metal_render_target_cache.h"
#include "rex/graphics/metal/metal_texture_cache.h"
#include "rex/logging/api.h"
#include "rex/graphics/metal/metal_readiness.h"
#include <unistd.h>
#include "rex/ui/metal/metal_provider.h"
#include "rex/kernel.h"
#include "rex/ui/metal/metal_startup_wait.h"
#include "rex/ui/windowed_app_context.h"
#include "rex/graphics/flags.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

REXCVAR_DECLARE(bool, store_shaders);
REXCVAR_DECLARE(bool, gpu_ring_reinit_sync);  // SDK command_processor.cpp (phase B1)
DEFINE_bool(metal_background_pipeline_warmup, true,
            "Compile cached Metal pipelines while opening videos play. "
            "Disable to finish all compilation on the loading screen.", "Metal");

namespace rex {
namespace graphics {
namespace metal {

MetalGraphicsSystem::MetalGraphicsSystem() {}

MetalGraphicsSystem::~MetalGraphicsSystem() {}

bool MetalGraphicsSystem::IsAvailable() {
  return rex::ui::metal::MetalProvider::IsMetalAPIAvailable();
}

std::string MetalGraphicsSystem::name() const { return "Metal"; }

void MetalGraphicsSystem::CreateProvider(bool with_presentation) {
  provider_ = rex::ui::metal::MetalProvider::Create();
}

void MetalGraphicsSystem::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  frame_log::Initialize();
  frame_log::Warmup(REXCVAR_GET(store_shaders));
  if (ShaderPrepRequested() && app_context_ && app_context_->IsInUIThread()) {
    ui::metal::HideAppForBackgroundWork();  // [shader-prep] no window, no Dock icon
  }
  if (!blocking || !app_context_ || !app_context_->IsInUIThread() ||
      command_processor_->is_paused() || !REXCVAR_GET(store_shaders)) {
    GraphicsSystem::InitializeShaderStorage(cache_root, title_id, blocking);
    return;
  }

  // Prepare cached translations before guest execution. Compilation can then
  // overlap the opening videos; native events remain responsive during setup.
  std::atomic<bool> complete{false};
  std::exception_ptr error;
  auto start = std::chrono::steady_clock::now();
  command_processor_->CallInThread([this, cache_root, title_id, &complete, &error] {
    try {
      command_processor_->InitializeShaderStorage(
          cache_root, title_id, !REXCVAR_GET(metal_background_pipeline_warmup));
    } catch (...) {
      error = std::current_exception();
    }
    complete.store(true, std::memory_order_release);
  });
  auto result = ui::metal::WaitForMetalStartup(
      [&complete] { return complete.load(std::memory_order_acquire); });
  REXLOG_INFO("Metal startup: preparation completed in {:.2f}s; {} UI iterations, "
              "{} native events, loading screen {}",
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
              result.iterations, result.events, result.overlay_shown);
  if (error) {
    std::rethrow_exception(error);
  }
  ui::metal::StartMetalTestControl(app_context_, presenter());
  if (ShaderPrepRequested()) {
    RunShaderPrep();  // never returns
  }
}

// [shader-prep] FABLE2_SHADER_PREP=1|<status.json> (docs/offline-shader-seed.md): a headless shader preparation
// run. The game's normal storage init above has loaded the remembered shader and pipeline lists and queued the
// startup warm-up (translation, pipeline creation into the binary archives, transfer / fused-resolve / tile
// prewarms, and the native-shader pool: optimized and native-translated shaders). The guest is never resumed:
// this waits on the UI thread until every one of those queues is empty, saves the archives and shader storage,
// writes the status file and exits. Under the app's bundle id this also fills macOS's Metal cache for that app.
bool MetalGraphicsSystem::ShaderPrepRequested() {
  const char* v = std::getenv("FABLE2_SHADER_PREP");  // getenv-ok: once, at start
  return v && *v && std::string(v) != "0";
}

void MetalGraphicsSystem::RunShaderPrep() {
  const char* v = std::getenv("FABLE2_SHADER_PREP");  // getenv-ok: once
  const std::string target = v ? v : "";
  const std::filesystem::path status_path =
      target.size() > 5 && target.compare(target.size() - 5, 5, ".json") == 0 ? std::filesystem::path(target)
                                                                              : std::filesystem::path();
  double timeout_s = 1800.0;  // guard only
  if (const char* t = std::getenv("FABLE2_SHADER_PREP_TIMEOUT_S"); t && *t) timeout_s = std::atof(t);
  auto* cp = static_cast<MetalCommandProcessor*>(command_processor_.get());
  MetalPipelineCache* cache = cp ? cp->pipeline_cache() : nullptr;
  const auto start = std::chrono::steady_clock::now();
  auto seconds = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };
  struct Pending {
    readiness::Background bg;
    uint32_t xfer = 0, tile = 0, rtt = 0;
    uint32_t total() const {
      return bg.pipelines_pending + bg.ns_pending + bg.ns_spec_pending + xfer + tile + rtt;
    }
  };
  auto sample = [&] {
    Pending p;
    if (cache) p.bg = cache->ReadinessBackground();
    if (cp && cp->render_target_cache()) {
      p.xfer = cp->render_target_cache()->XferPrewarmPending();
      p.tile = cp->render_target_cache()->TileResolvePrewarmPending();
    }
    if (cp && cp->texture_cache()) p.rtt = cp->texture_cache()->RttPrewarmPending();
    return p;
  };
  auto write_status = [&](const char* phase, const Pending& p, bool done, bool ok) {
    if (status_path.empty()) return;
    std::string json = fmt::format(
        "{{\"phase\":\"{}\",\"done\":{},\"ok\":{},\"seconds\":{:.2f},\"pid\":{},"
        "\"pending\":{{\"pipelines\":{},\"native_shaders\":{},\"ns_spec\":{},\"xfer\":{},\"tile\":{},"
        "\"rtt\":{}}},{}}}\n",
        phase, done, ok, seconds(), int(getpid()), p.bg.pipelines_pending, p.bg.ns_pending, p.bg.ns_spec_pending,
        p.xfer, p.tile, p.rtt, cache ? cache->ShaderPrepCountsJson() : std::string("\"no_cache\":true"));
    std::filesystem::path tmp = status_path;
    tmp += ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
      std::fwrite(json.data(), 1, json.size(), f);
      std::fclose(f);
      std::error_code ec;
      std::filesystem::rename(tmp, status_path, ec);
    }
  };
  REXLOG_INFO("[shader-prep] headless shader preparation: the guest is not started; waiting for the warm-up "
              "queues (status {})", status_path.empty() ? std::string("log only") : status_path.string());
  // Idle = every queue empty. Work hands over between queues (a created pipeline queues its pixel shader for
  // optimization, an optimized shader queues swaps), so idle must hold across a short settle window before it
  // counts; the timeout is a guard.
  constexpr double kSettleS = 0.5;
  double idle_since = -1.0, last_write = -1.0;
  bool timed_out = false;
  Pending last;
  ui::metal::WaitForMetalStartup([&] {
    last = sample();
    const double now = seconds();
    if (now - last_write >= 0.25) {
      write_status("warm-up", last, false, true);
      last_write = now;
    }
    if (now > timeout_s) {
      timed_out = true;
      return true;
    }
    if (last.total() == 0) {
      if (idle_since < 0) idle_since = now;
      return now - idle_since >= kSettleS;
    }
    idle_since = -1.0;
    return false;
  });
  REXLOG_INFO("[shader-prep] queues {} after {:.2f}s; saving the binary archives and shader storage",
              timed_out ? "NOT drained (timeout)" : "drained", seconds());
  write_status("saving", last, false, !timed_out);
  if (cache && cp) {
    std::atomic<bool> saved{false};
    cp->CallInThread([cache, &saved] {
      cache->ShutdownShaderStorage();
      saved.store(true, std::memory_order_release);
    });
    ui::metal::WaitForMetalStartup([&saved] { return saved.load(std::memory_order_acquire); });
  }
  const std::string counts = cache ? cache->ShaderPrepCountsJson() : std::string();
  REXLOG_INFO("[shader-prep] done in {:.2f}s (ok {}): {}", seconds(), !timed_out, counts);
  write_status("done", last, true, !timed_out);
  // The same exit as a window close (SDK ReXApp::OnClosing): the marker line tells tools it was clean.
  REXLOG_INFO("Title terminated; hard-exiting process.");
  rex::FlushLogging();
  std::_Exit(timed_out ? 3 : 0);
}

std::unique_ptr<CommandProcessor>
MetalGraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<CommandProcessor>(
      new MetalCommandProcessor(this, kernel_state_));
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe

// Local checkpoint ABI keeps the app independent of plugin RTTI/hidden symbols.
#include <rex/stream.h>
extern "C" __attribute__((visibility("default"))) bool Fable2CheckpointGpu(
    int operation, rex::system::IGraphicsSystem* interface, uint8_t* bytes, size_t* length) {
  auto* graphics = dynamic_cast<rex::graphics::metal::MetalGraphicsSystem*>(interface);
  if (!graphics) return false;
  auto* processor=static_cast<rex::graphics::metal::MetalCommandProcessor*>(graphics->command_processor());
  if (operation == 0) return processor->PauseForCheckpoint();
  if (operation == 1) { processor->ResumeFromCheckpoint(); return true; }
  if (!processor->is_paused() || !bytes || !length) return false;
  rex::stream::ByteStream stream(bytes,*length);
  if (operation == 2) {
    if (*length < 128 + sizeof(graphics->register_file()->values)) return false;
    if (!graphics->Save(&stream)) return false;
    stream.Write(graphics->register_file()->values,sizeof(graphics->register_file()->values));
    *length=stream.offset(); return true;
  }
  if (operation == 3) {
    if (*length != 32 + sizeof(graphics->register_file()->values)) return false;
    if (!graphics->Restore(&stream)) return false;
    stream.Read(graphics->register_file()->values,sizeof(graphics->register_file()->values));
    graphics->InvalidateGpuMemory(); return true;
  }
  return false;
}

// [ring-reinit] FABLE2_RING_REINIT_FIX: the executable's __imp__VdInitializeRingBuffer override
// (src/core/hotfunc/gpu/RingReinitFix.cpp) calls op 0 (park; out = {old write, read at start,
// waited us}) before forwarding to the real init and op 1 (write = 0) after it.
// Returns 1 = parked / done, 0 = park timed out (nothing changed), -1 = not a Metal system.
// Op 2 (phase B1, docs/sdk-from-source.md): 1 when the SDK's CommandProcessor::InitializeRingBuffer
// synchronises the re-init itself (cvar gpu_ring_reinit_sync, default true), 0 when that is turned
// off. The executable stands down on 1, so the SDK fix and the workaround are never both active;
// a plugin without op 2 returns -1 and the workaround stays on.
extern "C" __attribute__((visibility("default"))) int fable2_ring_reinit(
    int operation, rex::system::IGraphicsSystem* interface, uint32_t timeout_us, uint32_t* out) {
  auto* graphics = dynamic_cast<rex::graphics::metal::MetalGraphicsSystem*>(interface);
  if (!graphics || !graphics->command_processor()) return -1;
  auto* processor =
      static_cast<rex::graphics::metal::MetalCommandProcessor*>(graphics->command_processor());
  if (operation == 0) {
    uint32_t tmp[3] = {};
    uint32_t* o = out ? out : tmp;
    return processor->ParkRingForReinit(timeout_us, &o[0], &o[1], &o[2]) ? 1 : 0;
  }
  if (operation == 1) {
    processor->FinishRingReinit();
    return 1;
  }
  if (operation == 2) {
    return REXCVAR_GET(gpu_ring_reinit_sync) ? 1 : 0;
  }
  return -1;
}
