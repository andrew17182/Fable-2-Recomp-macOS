/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_GRAPHICS_SYSTEM_H
#define XENIA_GPU_METAL_GRAPHICS_SYSTEM_H

#include <memory>

#include "rex/graphics/command_processor.h"
#include "rex/graphics/graphics_system.h"

namespace rex {
namespace graphics {
namespace metal {
class MetalGraphicsSystem : public GraphicsSystem {
 public:
  MetalGraphicsSystem();
  ~MetalGraphicsSystem();

  static bool IsAvailable();

  std::string name() const override;

  void CreateProvider(bool with_presentation) override;

  void InitializeShaderStorage(const std::filesystem::path& cache_root,
                               uint32_t title_id, bool blocking) override;

  // FABLE2_VSYNC_PHASE_LOCK (metal_vsync_lock.h): replaces the SDK's guest vblank worker with a phase-locked one.
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                         system::KernelState* kernel_state) override;

 protected:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;

 private:
  // [shader-prep] FABLE2_SHADER_PREP: headless shader preparation, then exit.
  static bool ShaderPrepRequested();
  [[noreturn]] void RunShaderPrep();
  void StartPhaseLockedVblankWorker();
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_GRAPHICS_SYSTEM_H
