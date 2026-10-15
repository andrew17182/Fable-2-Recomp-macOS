#pragma once
// [pass-trace] Trace-only per-encoder GPU timing (FABLE2_GPU_TIME_TRACE=1; FABLE2_PASS_TRACE=0
// turns just this part off). Every render/compute/blit encoder gets MTLCounterSampleBuffer
// stage-boundary timestamps; rows go to passes-<stamp>-<pid>.csv beside the frame CSV.
// With tracing off every function is a cheap no-op and encoders are created exactly as before.
#include <cstdint>

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace rex::graphics::metal::pass_trace {

struct PassRef {
  void* cb = nullptr;  // internal command-buffer record
  int32_t index = -1;
  explicit operator bool() const { return cb && index >= 0; }
};

bool Enabled();
void Initialize(MTL::Device* device);

// Render pass: attaches sample-buffer indices to the descriptor, which the caller must
// pass to renderCommandEncoder() and then hand to ClearRender() (descriptors are cached).
PassRef BeginRender(MTL::CommandBuffer* cmd, MTL::RenderPassDescriptor* desc,
                    const char* site);
void ClearRender(MTL::RenderPassDescriptor* desc);
void NoteDraw(PassRef pass);
// FABLE2_PASS_TRACE_PS=1: the pass key also lists its pixel shaders (ucode hash, high 32 bits).
bool ShaderKeysEnabled();
void NoteShader(PassRef pass, uint64_t ps);
void NoteBarrier(PassRef pass);
void NoteFenceWait(PassRef pass);
void EndPass(PassRef pass, const char* end_reason);

// Compute/blit encoders: drop-in replacements for cmd->computeCommandEncoder() /
// cmd->blitCommandEncoder() (same autoreleased ownership).
MTL::ComputeCommandEncoder* Compute(MTL::CommandBuffer* cmd, const char* site);
// Same, with a per-encoder key for the CSV (default: the site). Build the key only when
// Enabled() is true.
MTL::ComputeCommandEncoder* Compute(MTL::CommandBuffer* cmd, const char* site,
                                    const char* key);
MTL::BlitCommandEncoder* Blit(MTL::CommandBuffer* cmd, const char* site);

}  // namespace rex::graphics::metal::pass_trace
