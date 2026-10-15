/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_
#define XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_

#include <dispatch/dispatch.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rex/platform.h"
#include "rex/string/buffer.h"
#include "rex/graphics/command_processor.h"
#include "rex/graphics/util/draw.h"
#include "rex/graphics/pipeline/shader/dxbc_translator.h"
#include "rex/graphics/metal/dxbc_to_dxil_converter.h"
#include "rex/graphics/metal/metal_geometry_shader.h"
#include "rex/graphics/metal/metal_pass_trace.h"
#include "rex/graphics/metal/metal_pipeline_cache.h"
#include "rex/graphics/metal/metal_primitive_processor.h"
#include "rex/graphics/metal/metal_ptr_set.h"
#include "rex/graphics/metal/metal_render_target_cache.h"
#include "rex/graphics/metal/metal_shader.h"
#include "rex/graphics/metal/metal_shader_converter.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/graphics/metal/metal_texture_cache.h"
#include "rex/graphics/metal/metal_upload_buffer_pool.h"
#include "rex/graphics/metal/metal_zpd_visibility_pool.h"
// clang-format off
// Must come after metal_texture_cache.h which includes Metal.hpp
#include "third_party/metal-shader-converter/include/metal_irconverter_runtime.h"
// clang-format on
#include "rex/ui/metal/metal_api.h"
#include "rex/ui/metal/metal_provider.h"

namespace MTL {
class BlitCommandEncoder;
class ComputeCommandEncoder;
class Fence;
class Heap;
class ResidencySet;
class SharedEvent;
class Event;
}  // namespace MTL

namespace rex {
namespace graphics {
namespace metal {

class MetalGraphicsSystem;

class MetalCommandProcessor final : public CommandProcessor {
 protected:

 public:
  explicit MetalCommandProcessor(MetalGraphicsSystem* graphics_system,
                                 system::KernelState* kernel_state);
  ~MetalCommandProcessor();

  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length);
  void RestoreEdramSnapshot(const void* snapshot);
  bool DrainForCheckpoint();
  bool PauseForCheckpoint();
  void ResumeFromCheckpoint();
  // [ring-reinit] FABLE2_RING_REINIT_FIX (docs/boot-hang-20261008.md "Fix"): called from the
  // guest thread around VdInitializeRingBuffer so the re-init cannot replay old ring packets.
  // ParkRingForReinit waits (bounded) until the worker has consumed everything up to the
  // current write index, then parks it with the 0xBAADF00D write-index sentinel. Returns true
  // when parked; false on timeout (nothing changed). FinishRingReinit sets write = read = 0.
  // Used only when the SDK's own synchronisation (gpu_ring_reinit_sync, phase B1) is off.
  bool ParkRingForReinit(uint32_t timeout_us, uint32_t* old_write, uint32_t* read_at_start,
                         uint32_t* waited_us);
  void FinishRingReinit();
  void ClearCaches() override;
  void InvalidateGpuMemory() override;
  void ClearReadbackBuffers();

  ui::metal::MetalProvider& GetMetalProvider() const;

  // Get the Metal device and command queue
  MTL::Device* GetMetalDevice() const { return device_; }
  MTL::CommandQueue* GetMetalCommandQueue() const { return command_queue_; }
  MTL::CommandBuffer* GetCurrentCommandBuffer() const {
    return current_command_buffer_;
  }

  // Submission coordination helpers — callers use these to query or obtain
  // command buffers for transfer/upload work without reaching into internal
  // command-processor state.
  bool HasActiveSubmission() const {
    return current_command_buffer_ != nullptr;
  }
  // Returns true when upload/transfer work can join the current submission's
  // command buffer. This is the case when a command buffer exists but no render
  // encoder is open.
  bool CanJoinActiveSubmissionForTransfer() const {
    return current_command_buffer_ != nullptr &&
           current_render_encoder_ == nullptr;
  }
  enum class TransferRequestSource : uint32_t {
    kUnknown,
    kSharedMemoryUpload,
    kGuestIndexCopy,
    kRenderTargetTransfer,
    kCount,
  };
  static constexpr size_t kTransferRequestSourceCount =
      static_cast<size_t>(TransferRequestSource::kCount);
  enum class SharedMemoryRequestReason : uint32_t {
    kUnknown,
    kVertexFetch,
    kMemexportStream,
    kGuestIndex,
    kShaderPrimitiveIndex,
    kIndexCopySource,
    kTextureBase,
    kTextureMips,
    kTextureBaseAndMips,
    kResolveCopyDest,
    kDrawMaterialization,
    kCount,
  };
  static constexpr size_t kSharedMemoryRequestReasonCount =
      static_cast<size_t>(SharedMemoryRequestReason::kCount);
  enum class SharedMemoryRequestOutcome : uint32_t {
    kAlreadyResident,
    kUploadBeforeRenderEncoder,
    kUploadInsideRenderEncoder,
    kRequestFailed,
    kNoSharedMemory,
    kTextureDeferredUploadFlush,
    kCount,
  };
  static constexpr size_t kSharedMemoryRequestOutcomeCount =
      static_cast<size_t>(SharedMemoryRequestOutcome::kCount);
  enum class SharedMemoryUploadRoute : uint32_t {
    kStagedBlit,
    kDirectWrite,
    kCount,
  };
  static constexpr size_t kSharedMemoryUploadRouteCount =
      static_cast<size_t>(SharedMemoryUploadRoute::kCount);
  enum class SharedMemoryDirectWriteRejectReason : uint32_t {
    kMainGpuAccessInFlight,
    kStandaloneAccessInFlight,
    kNoSharedBufferContents,
    kMixedRangeSplit,
    kCount,
  };
  static constexpr size_t kSharedMemoryDirectWriteRejectReasonCount =
      static_cast<size_t>(SharedMemoryDirectWriteRejectReason::kCount);
  enum class TextureUploadSourceRoute : uint32_t {
    kCpuGuestMemory,
    kResidentSharedMemory,
    kScaledResolve,
    kCount,
  };
  static constexpr size_t kTextureUploadSourceRouteCount =
      static_cast<size_t>(TextureUploadSourceRoute::kCount);
  enum class TextureUploadSourceFallbackReason : uint32_t {
    kMixedValidity,
    kScaledResolve,
    kSourceAlreadyResident,
    kCpuSourceLoadFailed,
    kUnknown,
    kCount,
  };
  static constexpr size_t kTextureUploadSourceFallbackReasonCount =
      static_cast<size_t>(TextureUploadSourceFallbackReason::kCount);
  enum class SharedMemoryUploadEncoderEndReason : uint32_t {
    kUnknown,
    kRenderBegin,
    kTransferRequest,
    kTextureCompute,
    kTextureBlit,
    kTextureDeferredBlit,
    kScaledResolveBlit,
    kCommandBufferEnd,
    kSwap,
    kUploadFailure,
    kShutdown,
    kMaterializationDrain,
    kCount,
  };
  static constexpr size_t kSharedMemoryUploadEncoderEndReasonCount =
      static_cast<size_t>(SharedMemoryUploadEncoderEndReason::kCount);
  // Returns a command buffer suitable for transfer (blit/compute) work.
  // If a render encoder is active it is ended first; if no command buffer
  // exists one is created. This is an encoder-lifetime break, not necessarily
  // a command-buffer submission break. Returns nullptr on failure.
  MTL::CommandBuffer* RequestTransferCommandBuffer(
      TransferRequestSource source = TransferRequestSource::kUnknown);
  void RecordSharedMemoryUploadRoute(SharedMemoryUploadRoute route,
                                     uint64_t bytes);
  void RecordSharedMemoryDirectWriteEligibility(uint64_t direct_bytes,
                                                uint64_t staged_bytes);
  void RecordSharedMemoryDirectWriteReject(
      SharedMemoryDirectWriteRejectReason reason, uint64_t bytes);
  void RecordTextureUploadSourceRoute(TextureUploadSourceRoute route,
                                      uint64_t bytes);
  void RecordTextureUploadSourceFallback(
      TextureUploadSourceFallbackReason reason);
  void RecordSharedMemoryUploadEncoderCopy();
  uint64_t SharedMemoryUploadEncoderCopies() const {
    return backend_telemetry_.shared_memory_upload_encoder_copies;
  }
  bool RequestSharedMemoryRange(SharedMemoryRequestReason reason,
                                uint32_t start, uint32_t length);
  bool RequestSharedMemoryRangeBeforeDrawPass(SharedMemoryRequestReason reason,
                                              uint32_t start, uint32_t length);
  bool RequestSharedMemoryRanges(SharedMemoryRequestReason reason,
                                 const SharedMemory::Range* ranges,
                                 uint32_t range_count);
  void RecordSharedMemoryRequestOutcome(SharedMemoryRequestOutcome outcome);
  MTL::BlitCommandEncoder* GetSharedMemoryUploadBlitEncoder();
  void EndSharedMemoryUploadBlitEncoder(
      SharedMemoryUploadEncoderEndReason reason =
          SharedMemoryUploadEncoderEndReason::kUnknown);

  // Standalone (detached) transfer command-buffer helpers.
  // These create command buffers that are independent of the active submission
  // and are used by caches for upload/transfer work that cannot join the
  // current command buffer.  The returned CB is retained; ownership transfers
  // back via CommitStandaloneAsync or CommitStandaloneAndWait.
  MTL::CommandBuffer* CreateStandaloneTransferCommandBuffer(const char* label);
  // Commit a standalone command buffer asynchronously (fire-and-forget).
  // The CB is released via a completion handler.
  void CommitStandaloneAsync(MTL::CommandBuffer* cmd);
  // Commit a standalone command buffer synchronously and wait for completion.
  // The CB is released before returning.
  void CommitStandaloneAndWait(MTL::CommandBuffer* cmd);

  uint64_t GetCurrentSubmission() const;
  uint64_t GetCompletedSubmission() const;
  uint64_t GetLatestSubmissionStarted() const { return submission_current_; }
  // Newest main command buffer that has been committed (the open one, if
  // any, is submission_current_).
  uint64_t GetLastCommittedSubmission() const {
    return current_command_buffer_ && submission_current_
               ? submission_current_ - 1
               : submission_current_;
  }
  // [upload-precommit] true when a staged shared-memory upload may go to a
  // pre-commit command buffer instead of ending the active render pass
  // (FABLE2_UPLOAD_PRECOMMIT; see metal_shared_memory.h).
  bool SharedMemoryPrecommitAllowed() const;
  // [texture-precommit] whether a draw encoded in the open main command
  // buffer samples this (root) texture.
  bool IsTextureSampledByCurrentCommandBuffer(const MTL::Texture* texture) const;
  // [tex-rename] (docs/tex-rename-20261013.md) a render pass is open and
  // texture loads may take the pre-commit route.
  bool TexRenamePassOpen() const {
    return current_render_encoder_ != nullptr && TexturePrecommitEnabled();
  }
  // A prepared, not yet encoded draw samples this (root) texture.
  // 0: none; 1: one of them also plans a load of the texture; 2: they only
  // sample it (prepared before it went out of date).
  int TexRenameQueuedDrawsSample(const MTL::Texture* root,
                                 const void* texture) const;
  // A texture got a new backing: descriptor indices must be rebuilt.
  void TexRenameInvalidateDescriptors() {
    cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
  }
  // [upload-precommit] trace: cause of the render-pass end that the next
  // staged upload makes (pass-trace end reason detail + tr_n_brk_* rows).
  void NoteSharedMemoryUploadBreakCause(MetalSharedMemory::PageAccess cause,
                                        bool guard_blocked);
  // [upload-precommit] A blit encoder in a fresh command buffer that is
  // committed (EndSharedMemoryPrecommit) before the current one. It first
  // waits until main command buffer `wait_submission` (the newest one that
  // read the pages) has finished on the GPU (WAR). The active render encoder
  // then waits on a fence the blit updates, and every later encoder and
  // command buffer waits on the pre-commit event (RAW).
  MTL::BlitCommandEncoder* BeginSharedMemoryPrecommit(uint64_t wait_submission);
  void EndSharedMemoryPrecommit(uint32_t pages);
  // [texture-precommit] FABLE2_TEXTURE_PRECOMMIT (default on; =0 kill
  // switch): texture loads of a draw flush go to the pre-commit command
  // buffer instead of ending the open render pass when every load is
  // eligible (MetalTextureCache::ClassifyMaterializationPlan).
  static bool TexturePrecommitEnabled();
  // Encoders the texture cache opens in the pre-commit command buffer:
  // PrecommitEncoderBegin right after creation, PrecommitEncoderEnd right
  // before endEncoding (fence chaining).
  void PrecommitEncoderBegin(MTL::BlitCommandEncoder* encoder);
  void PrecommitEncoderBegin(MTL::ComputeCommandEncoder* encoder);
  void PrecommitEncoderEnd(MTL::BlitCommandEncoder* encoder);
  void PrecommitEncoderEnd(MTL::ComputeCommandEncoder* encoder);
  // The texture cache abandons the pre-commit route mid-flush (a load needs
  // GPU work in the current command buffer first): ends the render pass.
  // why: pass-trace end detail ("texture:fallback_*").
  void AbortTexturePrecommit(const char* why);
  // [tex-rename] after a flush's texture loads / before resolves and the
  // command buffer end.
  void TexRenameAfterLoads(
      const MetalTextureCache::TextureMaterializationPlan* const* plans,
      size_t count);
  void TexRenameSettleOrphansNow();
  MTL::CommandBuffer* TexRenameBeginSettle(const char* why);
  void SetShmemPrecommitInTexturePrecommit(bool on);
  bool SharedMemoryRangeHasPendingGpuWrite(uint32_t start,
                                           uint32_t length) const;
  MTL::CommandBuffer* EnsureCommandBuffer();
  void EndRenderEncoder();
  // [tile-resolve] The open render pass a resolve may run its tile function
  // in (docs/tile-resolve-20261013.md), and ending it for a resolve.
  MTL::RenderCommandEncoder* TileResolveRenderEncoder() const {
    return current_render_encoder_;
  }
  MTL::RenderPassDescriptor* TileResolvePassDescriptor() const {
    return current_render_pass_descriptor_;
  }
  uint64_t TileResolveRenderEncoderGen() const { return render_encoder_gen_; }
  // [tile-resolve extent] The open pass's render area height (0 = the full
  // attachment height) and the bounding box of what its draws could write
  // (union of the draw scissors; full when transfers were drawn in it).
  uint32_t TileResolvePassExtentHeight() const { return pass_extent_h_; }
  // [tile-resolve keep] After a tile resolve the pass stays open until the
  // resolve's clear (drawn in it when possible) is done, or with
  // FABLE2_TILE_RESOLVE_KEEP_OPEN=1 until something else ends it. Later draws
  // in it that read what the tile resolve wrote end it first.
  bool TileResolveKeptOpen() const {
    return current_render_encoder_ && tres_kept_gen_ == render_encoder_gen_;
  }
  void TileResolveKeep(MTL::Texture* const* written, uint32_t count);
  // [ssmask] FABLE2_EFFECTS_FOOTPRINT_VARIANT: whether some 2D offset fetch
  // of this pixel shader can take the effects footprint in the current draw
  // (texel offset along a 2x/3x axis, resolution-scaled texture, format not
  // depth-like): then its translation needs the footprint code.
  bool FootprintVariantNeeded(const Shader& shader);
  // The offset-fetch constants with a non-depth format of the last pixel
  // shader checked, and whether it got the footprint variant (checked
  // against the textures' actual scaling in UpdateSystemConstantValues).
  uint32_t fp_variant_check_mask_ = 0;
  bool fp_variant_chosen_ = false;
  void TileResolveEndKept(const char* detail) {
    if (TileResolveKeptOpen()) EndRenderEncoderForResolve(detail);
  }
  // A texture (or a view of it) sampled by a draw encoded in the open pass.
  bool TileResolvePassSampled(MTL::Texture* texture);
  uint64_t TileResolvePassDrawnArea() const {
    return pass_drawn_any_ ? uint64_t(pass_drawn_x1_ - pass_drawn_x0_) *
                                 uint64_t(pass_drawn_y1_ - pass_drawn_y0_)
                           : 0;
  }
  void EndRenderEncoderForResolve(const char* detail);
  void NoteDrawPassTransferInheritedRasterizerState();
  void InvalidateRenderEncoderStateAfterDrawPassTransfers(
      MetalRenderTargetCache::DrawPassTransferEncoderMutationMask mutations);
  void ResetRenderEncoderResourceUsage();
  void UseRenderEncoderResource(MTL::Resource* resource,
                                MTL::ResourceUsage usage);
  void UseRenderEncoderResource(MTL::Resource* resource,
                                MTL::ResourceUsage usage,
                                MTL::RenderStages stages);
  bool AddResidencySetHeap(MTL::Heap* heap);
  void EnsureCommandBufferAutoreleasePool();
  void DrainCommandBufferAutoreleasePool();

  // Force issue a swap to push render target to presenter (for trace dumps)
  void ForceIssueSwap();
  bool HasSeenSwap() const { return saw_swap_; }
  void SetSwapDestSwap(uint32_t dest_base, bool swap);
  bool ConsumeSwapDestSwap(uint32_t dest_base, bool* swap_out);

  MetalSharedMemory* shared_memory() const { return shared_memory_.get(); }
  MetalRenderTargetCache* render_target_cache() const {
    return render_target_cache_.get();
  }
  MetalTextureCache* texture_cache() const { return texture_cache_.get(); }
  MetalPipelineCache* pipeline_cache() const { return pipeline_cache_.get(); }

  // D3D12-style shared-memory hazard tracking for GPU writes produced by
  // resolves and memexport draws, expressed with Metal fences/barriers.
  struct SharedMemoryRange {
    uint32_t start = 0;
    uint32_t length = 0;
  };

  struct SharedMemoryReadDependency {
    bool needs_fence_wait = false;
  };

  void MarkSharedMemoryComputeWritePending(uint32_t address, uint32_t length,
                                           MTL::ComputeCommandEncoder* encoder);
  void MarkSharedMemoryRenderWritePending(uint32_t address, uint32_t length,
                                          MTL::RenderStages stages);
  bool PrepareSharedMemoryComputeReadDependency(
      const SharedMemoryRange* ranges, uint32_t range_count,
      bool consumer_can_join_current_submission,
      SharedMemoryReadDependency* dependency_out);
  bool EncodeSharedMemoryComputeReadDependency(
      MTL::ComputeCommandEncoder* encoder,
      const SharedMemoryReadDependency& dependency,
      const SharedMemoryRange* ranges, uint32_t range_count);

  // Persistent bindless descriptor heap allocators.
  uint32_t AllocateViewBindlessIndex();
  void ReleaseViewBindlessIndex(uint32_t index);
  void RetireViewBindlessIndex(uint32_t index);
  uint32_t GetViewBindlessHeapAvailableCount() const;
  uint32_t AllocateSamplerBindlessIndex();
  void ReleaseSamplerBindlessIndex(uint32_t index);
  IRDescriptorTableEntry* GetViewBindlessHeapEntry(uint32_t index);
  IRDescriptorTableEntry* GetSamplerBindlessHeapEntry(uint32_t index);

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) override;

  // Flush pending GPU work before entering wait state.
  // This ensures Metal command buffers are submitted and completed before
  // the autorelease pool is drained, preventing hangs from deferred
  // deallocation.
  void PrepareForWait() override;
  void ReturnFromWait() override;  // [gpu-time-trace] idle-wait timing only

  // Use base class WriteRegister - don't override with empty implementation!
  // The base class stores values in register_file_->values[] which we need.
  void OnPrimaryBufferEnd() override;
  // [zpd-stats] counts occlusion-query packets (begin/end) before the SDK's fake
  // result path; FABLE2_ZPD_STATS=1 logs them (workaround audit, 2026-10-14).
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count) override;
  // [hle-census] trace only (FABLE2_HLE_CENSUS=1, metal_hle_census.h).
  void OnDrawPacketForCensus(const void* header, bool skipped) override;
  bool OnIndirectBufferForCensus(uint32_t ptr, uint32_t count) override;
  void OnType3PacketForCensus(uint32_t packet, const void* header,
                              bool skipped) override;
  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  void MaybeLogMemReport();  // [rt-shrink] FABLE2_MEM_REPORT=<seconds>
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address,
                     uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;
  // Trace-only (FABLE2_DRAW_SKIP_TRACE=1): IssueDraw wraps this and records
  // per-frame counts of skipped/dropped draws by reason into the frame CSV.
  bool IssueDrawImpl(xenos::PrimitiveType primitive_type, uint32_t index_count,
                     IndexBufferInfo* index_buffer_info,
                     bool major_mode_explicit);
  // [alias-clear] recognises an XDK clear rectangle (FABLE2_ALIAS_CLEAR_NATIVE)
  // and performs it as an EDRAM clear; false = draw it as usual.
  bool TryAliasClearDraw(xenos::PrimitiveType primitive_type,
                         uint32_t index_count,
                         const IndexBufferInfo* index_buffer_info,
                         const Shader& vertex_shader, const Shader* pixel_shader,
                         reg::RB_DEPTHCONTROL normalized_depth_control,
                         uint32_t normalized_color_mask, bool memexport_used);
  const char* draw_skip_reason_ = nullptr;
  int census_draw_site_ = -1;  // [hle-census] site of the draw being issued
  bool IssueCopy() override;

  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers) override;
  void WriteRegisterRangeFromRing(rex::memory::RingBuffer* ring, uint32_t base,
                                  uint32_t num_registers) override;

  bool CanFastWriteRegisterRange(uint32_t start_index,
                                 uint32_t num_registers) const;
  bool TryWriteKnownRegisterRangeFromMem(uint32_t start_index, uint32_t* base,
                                         uint32_t num_registers);
  void WriteFastRegisterRangeFromRing(rex::memory::RingBuffer* ring, uint32_t base,
                                      uint32_t num_registers);
  void WriteShaderConstantsFromMem(uint32_t start_index, uint32_t* base,
                                   uint32_t num_registers);
  void WriteBoolLoopConstantsFromMem(uint32_t start_index, uint32_t* base,
                                     uint32_t num_registers);
  void WriteFetchConstantsFromMem(uint32_t start_index, uint32_t* base,
                                  uint32_t num_registers);
  bool FloatConstantRangeNeedsDirty(uint32_t start_index, const uint32_t* base,
                                    uint32_t num_registers,
                                    const uint64_t* constant_map,
                                    uint32_t stage_first_constant) const;
  void DirtyFetchConstantDwords(
      const DxbcShader::FetchConstantDwordMask& dirty_mask);

  static constexpr size_t kStageVertex = 0;
  static constexpr size_t kStagePixel = 1;
  static constexpr size_t kStageCount = 2;  // Vertex + pixel.
  static constexpr size_t kCbvSlotCount = 5;
  enum CbvSlot : size_t {
    kCbvSlotSystem,
    kCbvSlotFloat,
    kCbvSlotBoolLoop,
    kCbvSlotFetch,
    kCbvSlotDescriptorIndices,
  };
  static constexpr size_t kBindlessRootRebuildReasonCount = 4;
  enum BindlessRootRebuildReason : size_t {
    kBindlessRootRebuildFrameOpen,
    kBindlessRootRebuildDescriptorIndicesPointerChange,
    kBindlessRootRebuildOtherCbvPointerChange,
    kBindlessRootRebuildSharedMemoryUavChange,
  };
  static constexpr size_t kBindlessRootSlotsChangedBinCount = 6;
  static constexpr size_t kBindlessRootRebuildDetailCount = 7;
  enum BindlessRootRebuildDetail : size_t {
    kBindlessRootDetailSameBufferOffsetChanged,
    kBindlessRootDetailDifferentBuffer,
    kBindlessRootDetailDescriptorIndicesOnly,
    kBindlessRootDetailOtherCbvOnly,
    kBindlessRootDetailMixedDescriptorAndOther,
    kBindlessRootDetailResourceIdentityChanged,
    kBindlessRootDetailResourceIdentitySame,
  };
  static constexpr size_t kRenderEncoderBufferStageTelemetryCount = 4;
  static constexpr size_t kBindlessRootArgTelemetrySlotCount = 32;

  // Per-draw uniform buffer coordinates passed between IssueDraw sub-methods.
  struct UniformBufferInfo {
    struct Cbv {
      MTL::Buffer* buffer = nullptr;
      NS::UInteger offset = 0;
      uint64_t gpu_address = 0;
      size_t size = 0;
      // Inactive slots are canonicalized to null in the top-level argument
      // table, matching the fixed MSC layout without treating unused CBVs as
      // per-draw state.
      bool active = false;
    };

    std::array<std::array<Cbv, kCbvSlotCount>, kStageCount> cbvs = {};
    std::array<uint32_t, kStageCount> active_cbv_masks = {};
    std::array<DxbcShader::FetchConstantDwordMask, kStageCount>
        fetch_constant_dword_masks = {};
  };

  struct DrawDynamicState {
    MTL::Viewport viewport = {};
    MTL::ScissorRect scissor = {};
    draw_util::ViewportInfo viewport_info = {};
    reg::RB_DEPTHCONTROL depth_control = {};
    reg::RB_STENCILREFMASK stencil_ref_mask_front = {};
    reg::RB_STENCILREFMASK stencil_ref_mask_back = {};
    reg::PA_SU_SC_MODE_CNTL pa_su_sc_mode_cntl = {};
    MTL::CullMode cull_mode = MTL::CullModeNone;
    MTL::Winding front_facing_winding = MTL::WindingCounterClockwise;
    MTL::TriangleFillMode triangle_fill_mode = MTL::TriangleFillModeFill;
    float depth_bias_constant = 0.0f;
    float depth_bias_slope = 0.0f;
    MTL::DepthClipMode depth_clip_mode = MTL::DepthClipModeClip;
    bool primitive_polygonal = false;
    bool rasterization_enabled = true;
    float blend_constants[4] = {};
  };

  // Vertex binding range for stage-in / geometry emulation.
  struct VertexBindingRange {
    uint32_t binding_index = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t stride = 0;
  };

  struct PreparedIndexBuffer {
    MTL::Buffer* buffer = nullptr;
    uint64_t offset = 0;
  };

  enum class PreparedDrawFlushReason : uint32_t {
    kManual,
    kRenderTargetUpdate,
    kRenderTargetKeyMismatch,
    kQueueBudget,
    kQueueReject,
    kPrepareForWait,
    kSwap,
    kIssueCopy,
    kTransferRequest,
    kRenderEncoderEnd,
    kCommandBufferEnd,
    kQuery,
    kCount,
  };
  static constexpr size_t kPreparedDrawFlushReasonCount =
      static_cast<size_t>(PreparedDrawFlushReason::kCount);

  enum class PreparedDrawQueueRejectReason : uint32_t {
    kNone,
    kResidentWithoutActiveQueue,
    kNoSharedMemoryRanges,
    kMemexport,
    kTextureUpload,
    kTextureRequestLoadData,
    kPendingDrawPassTransfers,
    kZPDActive,
    kRenderTargetKeyMismatch,
    kQueueBudget,
    kCount,
  };
  static constexpr size_t kPreparedDrawQueueRejectReasonCount =
      static_cast<size_t>(PreparedDrawQueueRejectReason::kCount);

  struct PreparedDrawRenderTargetKey {
    uint32_t rb_surface_info = 0;
    uint32_t rb_depth_info = 0;
    std::array<uint32_t, xenos::kMaxColorRenderTargets> rb_color_info = {};
    uint32_t normalized_depth_control = 0;
    uint32_t normalized_color_mask = 0;
    bool is_rasterization_done = false;

    bool operator==(const PreparedDrawRenderTargetKey& other) const {
      return rb_surface_info == other.rb_surface_info &&
             rb_depth_info == other.rb_depth_info &&
             rb_color_info == other.rb_color_info &&
             normalized_depth_control == other.normalized_depth_control &&
             normalized_color_mask == other.normalized_color_mask &&
             is_rasterization_done == other.is_rasterization_done;
    }

    bool operator!=(const PreparedDrawRenderTargetKey& other) const {
      return !(*this == other);
    }
  };

  struct RenderResourceRef {
    MTL::Resource* resource = nullptr;
    MTL::ResourceUsage usage = MTL::ResourceUsageRead;
    MTL::RenderStages stages = MTL::RenderStages(0);
  };
  struct RenderResourceSet {
    std::vector<RenderResourceRef> resources;
    uint64_t serial = 0;
    uint64_t source_serial = 0;
  };
  struct EncoderResourceUsageState {
    uint32_t usage_bits = 0;
    uint32_t read_stage_bits = 0;
    uint32_t write_stage_bits = 0;
    uint32_t sample_stage_bits = 0;
  };

  // FABLE2_NATIVE_TESS exact path: a D3D11 reference tessellator pattern
  // (XeTessVert vertices, uint32 triangle list) and runs of patches using it.
  struct TessPattern {
    uint32_t factor_bits[6] = {};
    uint32_t arena_base = 0;          // first point in tess_arena_
    MTL::Buffer* indices = nullptr;   // local indices (instanced runs)
    std::vector<uint32_t> local_indices;  // for merged per-draw index lists
    uint32_t index_count = 0;
  };
  struct TessRun {
    const TessPattern* pattern;
    uint32_t first;
    uint32_t count;
  };
  struct PreparedDraw {
    std::string capture_label;
    uint64_t trace_ps = 0;  // FABLE2_PASS_TRACE_PS
    MTL::RenderPipelineState* pipeline = nullptr;
    MetalPipelineCache::TessellationPipelineState* tessellation_pipeline_state =
        nullptr;
    MetalPipelineCache::GeometryPipelineState* geometry_pipeline_state =
        nullptr;

    PrimitiveProcessor::ProcessingResult primitive_processing_result = {};
    UniformBufferInfo uniforms = {};
    DrawDynamicState dynamic_state = {};
    PreparedIndexBuffer prepared_guest_dma_index_buffer = {};
    IndexBufferInfo index_buffer_info = {};
    bool has_index_buffer_info = false;

    // Copied only with FABLE2_DRAW_PREP_LITE=0; DispatchDraw reads only
    // has_vertex_bindings (whether the vertex shader has any).
    std::vector<Shader::VertexBinding> vertex_bindings;
    bool has_vertex_bindings = false;
    std::array<VertexBindingRange, 32> vertex_ranges = {};
    uint32_t vertex_range_count = 0;
    std::vector<SharedMemory::Range> materialization_ranges;
    bool has_invalid_shared_memory = false;

    std::array<SharedMemoryRange, 96> shared_memory_hazard_ranges = {};
    uint32_t shared_memory_hazard_range_count = 0;
    MTL::RenderStages shared_memory_consumer_stages = MTL::RenderStages(0);

    std::vector<draw_util::MemExportRange> memexport_ranges;
    MTL::RenderStages memexport_write_stages = MTL::RenderStages(0);
    MTL::ResourceUsage shared_memory_usage = MTL::ResourceUsageRead;
    PreparedDrawRenderTargetKey render_target_key = {};
    RenderResourceSet texture_resource_set = {};
    MetalTextureCache::TextureMaterializationPlan texture_materialization_plan =
        {};

    bool use_tessellation_emulation = false;
    bool use_geometry_emulation = false;
    // FABLE2_NATIVE_TESS: hardware tessellation with per-patch half factors.
    bool native_tess = false;
    bool native_tess_cpu = false;
    std::vector<TessRun> tess_runs;
    // Merged: one indexed draw of the whole guest draw (index list uploaded).
    MTL::Buffer* tess_merged_indices = nullptr;
    uint32_t tess_merged_offset = 0;
    uint32_t tess_merged_count = 0;
    bool native_points = false;  // FABLE2_NATIVE_POINTS / _RECTS
    uint32_t native_geometry_vertices = 1;  // guest vertices per instance
    bool native_points_mesh = false;  // FABLE2_NATIVE_POINTS=2
    MTL::Buffer* tess_factor_buffer = nullptr;
    uint32_t tess_factor_offset = 0;
    uint32_t tess_patch_count = 0;
    bool shared_memory_is_uav = false;
    bool memexport_used = false;
    bool uses_vertex_fetch = false;
    bool prepare_uniforms = false;
    bool fallback_depth_attachment_required = false;
    bool texture_upload_needed = false;
    bool may_texture_request_load_data = false;
    bool has_pending_draw_pass_transfers = false;
  };

  bool PrepareGuestDMAIndexBufferForMemexport(
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      PreparedIndexBuffer& prepared_index_buffer_out);

  // Host draw path — prepare per-draw dynamic state and upload constant buffers
  // before entering the Metal render encoder.
  // [mip-lod] the draw's pixel shader modification, for PrepareDrawConstants.
  uint64_t miplod_ps_mod_ = 0;
  bool PrepareDrawConstants(
      const RegisterFile& regs, Shader* vertex_shader, Shader* pixel_shader,
      MetalShader* metal_vertex_shader, MetalShader* metal_pixel_shader,
      bool shared_memory_is_uav, bool is_rasterization_done,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      uint32_t used_texture_mask, uint32_t normalized_color_mask,
      MTL::RenderPassDescriptor* render_pass_descriptor,
      UniformBufferInfo& uniforms_out, DrawDynamicState& dynamic_state_out);

  void ApplyDrawDynamicState(const DrawDynamicState& dynamic_state);

  PreparedDrawRenderTargetKey BuildPreparedDrawRenderTargetKey(
      const RegisterFile& regs, bool is_rasterization_done,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask) const;
  bool SubmitPreparedDraw(std::unique_ptr<PreparedDraw> draw);
  // [prepared-draw-pool] FABLE2_PREPARED_DRAW_POOL (default on, =0 restores the per-draw
  // allocations): reuse PreparedDraw records (their vectors keep their capacity), recycle
  // flushed queue entries and queue storage, and build resource sets in reused storage.
  static bool PreparedDrawPoolEnabled();
  // [prepared-draw-slim] FABLE2_PREPARED_DRAW_SLIM (default on, =0 restores the whole-record
  // reset and the build-locally-then-copy of the fixed arrays): reset a reused record field
  // by field, leaving the count-bounded arrays (hazard ranges, vertex ranges) as they are, and
  // build the arrays, uniforms and dynamic state in the record itself.
  static bool PreparedDrawSlimEnabled();
  // [draw-prep-lite] FABLE2_DRAW_PREP_LITE (default on, =0 off; docs/gpucmd-cpu-20261014.md):
  // per-draw host bookkeeping that recomputed or copied unchanged data (same draws, same
  // encoded state). Dev Launcher toggle.
  static bool DrawPrepLiteEnabled();
  // [draw-prep-lite2] FABLE2_DRAW_PREP_LITE2 (default on, =0 off; docs/gpucmd-cpu-20261014.md
  // round 2): more of the same kind of trims (same draws, same encoded state): the texture
  // load-data check result passed down instead of recomputed, the pending-write list rebuilt
  // in reused storage, constant packing by contiguous runs without double fills or copies,
  // cached immutable texture properties. Dev Launcher toggle.
  static bool DrawPrepLite2Enabled();
  std::unique_ptr<PreparedDraw> AcquirePreparedDraw();
  void ReleasePreparedDraw(std::unique_ptr<PreparedDraw> draw);
  static void ResetPreparedDrawKeepCapacity(PreparedDraw& draw);
  static void ResetPreparedDrawSlim(PreparedDraw& draw);
  void RecyclePreparedDraws(std::vector<std::unique_ptr<PreparedDraw>>& draws);
  RenderResourceSet& AcquireResourceSetScratch(RenderResourceSet& fallback);
  bool EncodePreparedDraw(const PreparedDraw& draw);
  bool FlushPreparedDrawQueue(PreparedDrawFlushReason reason);
  bool PreparedDrawQueueHasActiveZPD() const;
  bool CanQueuePreparedDraw(const PreparedDraw& draw,
                            PreparedDrawQueueRejectReason& reject_reason) const;
  void RecordPreparedDrawQueueReject(
      PreparedDrawQueueRejectReason reject_reason);

  // Host draw path — refresh top-level argument buffers when root CBV state
  // changes and bind them plus the stable descriptor heap buffers to the
  // render encoder when encoder state requires it.
  bool PopulateBindlessTables(bool shared_memory_is_uav,
                              MTL::ResourceUsage shared_memory_usage,
                              bool use_geometry_emulation,
                              bool use_tessellation_emulation,
                              const UniformBufferInfo& uniforms);

  // Host draw path — bind vertex buffers and dispatch the actual draw call
  // (tessellation, geometry emulation, or standard path), then track
  // memexport writes.
  // FABLE2_NATIVE_TESS / FABLE2_EMU_DRAW_STATS: the effective adaptive
  // tessellation factor of each control point (4 per quad patch), read from
  // the guest index buffer on the CPU and clamped like the adaptive host VS.
  bool ReadAdaptiveTessFactors(
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      float factor_min, float factor_max, std::vector<float>& out);
  // The guest tessellation factor range as the system constants carry it.
  void GetTessellationFactorRange(const RegisterFile& regs, float& min_out,
                                  float& max_out);
  std::vector<float> tess_cp_scratch_;
  // FABLE2_NATIVE_TESS_TRACE=1: per-combination path log and counts.
  static bool NativeTessTraceEnabled();
  void NativeTessTrace(uint64_t vs, uint64_t ps, const char* path,
                       uint32_t mode, bool blended, uint32_t patches,
                       float factor_min, float factor_max, bool bad_factor,
                       uint32_t zfunc, uint32_t z_write);
  std::vector<TessRun> native_tess_runs_scratch_;
  std::unordered_map<uint64_t, TessPattern> tess_patterns_;
  MTL::Buffer* tess_arena_ = nullptr;  // float2 domain points of all patterns
  uint32_t tess_arena_used_ = 0;
  static constexpr uint32_t kTessArenaPoints = 1u << 20;
  const TessPattern* GetTessPattern(const float factors[6]);
  // Set by EncodePreparedDraw for DispatchDraw (FABLE2_NATIVE_TESS).
  const PreparedDraw* dispatch_native_tess_ = nullptr;
  bool dispatch_native_points_ = false;
  uint32_t dispatch_native_geometry_vertices_ = 1;
  bool dispatch_native_points_mesh_ = false;
  bool DispatchDraw(
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool use_tessellation_emulation,
      MetalPipelineCache::TessellationPipelineState*
          tessellation_pipeline_state,
      bool use_geometry_emulation,
      MetalPipelineCache::GeometryPipelineState* geometry_pipeline_state,
      bool shared_memory_is_uav, MTL::ResourceUsage shared_memory_usage,
      bool memexport_used, MTL::RenderStages memexport_write_stages,
      bool uses_vertex_fetch, bool shared_memory_resource_registered,
      const PreparedIndexBuffer* prepared_guest_dma_index_buffer,
      bool has_vertex_bindings,
      const VertexBindingRange* vertex_ranges, uint32_t vertex_range_count,
      const IndexBufferInfo* index_buffer_info,
      const std::vector<draw_util::MemExportRange>& memexport_ranges);

 private:
  // Command buffer management
  enum class RenderEncoderEndReason : uint32_t {
    kUnknown,
    kPrepareForWait,
    kSwap,
    kCommandBufferEnd,
    kRequestTransferCommandBuffer,
    kSharedMemoryReadDependency,
    kRenderTargetUpdateDescriptorDirty,
    kPipelineDescriptorIncompatible,
    kTextureUploadBeforeDrawPass,
    kSharedMemoryUploadBeforeDrawPass,
    kResolveNeedsBoundary,
    kBeginRenderEncoderDescriptorChanged,
    kCount,
  };

  static constexpr size_t kRenderEncoderEndReasonCount =
      static_cast<size_t>(RenderEncoderEndReason::kCount);

  enum class RenderResourceSetKind : uint32_t {
    kFixed,
    kTexture,
    kRoot,
    kCount,
  };
  static constexpr size_t kRenderResourceSetKindCount =
      static_cast<size_t>(RenderResourceSetKind::kCount);

  enum class DrawMaterializationSource : uint32_t {
    kVertexFetch,
    kGuestIndex,
    kMemexport,
    kTextureSource,
    kCount,
  };
  static constexpr size_t kDrawMaterializationSourceCount =
      static_cast<size_t>(DrawMaterializationSource::kCount);

  struct BackendTelemetryStats {
    uint64_t swaps = 0;
    uint64_t draw_calls = 0;
    uint64_t pipeline_sets = 0;
    uint64_t pipeline_set_skips = 0;
    uint64_t texture_requests_before_encoder = 0;
    uint64_t texture_requests_after_encoder_begin = 0;

    uint64_t begin_encoder_calls = 0;
    uint64_t begin_encoder_reused_compatible = 0;
    uint64_t begin_encoder_created = 0;
    uint64_t begin_encoder_descriptor_restarts = 0;
    uint64_t begin_encoder_resource_usage_resets = 0;
    uint64_t begin_encoder_descriptor_failures = 0;
    uint64_t begin_encoder_creation_failures = 0;

    uint64_t end_encoder_active = 0;
    uint64_t end_encoder_no_active = 0;
    std::array<uint64_t, kRenderEncoderEndReasonCount> end_reasons = {};
    std::array<uint64_t, kTransferRequestSourceCount>
        transfer_request_sources_total = {};
    std::array<uint64_t, kTransferRequestSourceCount>
        transfer_request_sources_active = {};
    std::array<uint64_t, kTransferRequestSourceCount>
        transfer_request_sources_no_active = {};
    std::array<uint64_t, kTransferRequestSourceCount>
        transfer_request_render_encoder_ends = {};
    std::array<uint64_t, kSharedMemoryRequestReasonCount>
        shared_memory_request_upload_bytes = {};
    std::array<uint64_t, kSharedMemoryRequestReasonCount>
        shared_memory_request_failures = {};
    std::array<uint64_t, kSharedMemoryRequestOutcomeCount>
        shared_memory_request_outcomes = {};
    std::array<uint64_t, kSharedMemoryUploadRouteCount>
        shared_memory_upload_route_counts = {};
    std::array<uint64_t, kSharedMemoryUploadRouteCount>
        shared_memory_upload_route_bytes = {};
    uint64_t shared_memory_direct_write_eligible_bytes = 0;
    uint64_t shared_memory_direct_write_staged_required_bytes = 0;
    std::array<uint64_t, kSharedMemoryDirectWriteRejectReasonCount>
        shared_memory_direct_write_reject_counts = {};
    std::array<uint64_t, kSharedMemoryDirectWriteRejectReasonCount>
        shared_memory_direct_write_reject_bytes = {};
    uint64_t shared_memory_lazy_upload_no_upload_batches = 0;
    uint64_t shared_memory_lazy_upload_direct_only_batches = 0;
    uint64_t shared_memory_lazy_upload_mixed_batches = 0;
    uint64_t shared_memory_lazy_upload_staged_only_batches = 0;
    uint64_t shared_memory_lazy_upload_direct_only_active = 0;
    uint64_t shared_memory_lazy_upload_mixed_active = 0;
    uint64_t shared_memory_lazy_upload_staged_only_active = 0;
    std::array<uint64_t, kTextureUploadSourceRouteCount>
        texture_upload_source_route_counts = {};
    std::array<uint64_t, kTextureUploadSourceRouteCount>
        texture_upload_source_route_bytes = {};
    std::array<uint64_t, kTextureUploadSourceFallbackReasonCount>
        texture_upload_source_fallback_reasons = {};
    uint64_t shared_memory_upload_batches = 0;
    uint64_t shared_memory_upload_batch_input_ranges = 0;
    uint64_t shared_memory_upload_batch_coalesced_ranges = 0;
    uint64_t shared_memory_upload_batch_bytes = 0;
    uint64_t shared_memory_upload_encoder_acquisitions = 0;
    uint64_t shared_memory_upload_encoder_reuses = 0;
    uint64_t shared_memory_upload_encoder_copies = 0;
    std::array<uint64_t, kSharedMemoryUploadEncoderEndReasonCount>
        shared_memory_upload_encoder_end_reasons = {};
    std::array<uint64_t, kDrawMaterializationSourceCount>
        draw_materialization_source_ranges = {};
    std::array<uint64_t, kDrawMaterializationSourceCount>
        draw_materialization_source_bytes = {};
    std::array<uint64_t, kDrawMaterializationSourceCount>
        draw_materialization_source_invalid_ranges = {};
    std::array<uint64_t, kDrawMaterializationSourceCount>
        draw_materialization_source_invalid_bytes = {};
    uint64_t draw_materialization_per_draw_requests = 0;
    uint64_t draw_materialization_per_draw_invalid_requests = 0;
    uint64_t draw_materialization_per_draw_resident_skips = 0;
    uint64_t prepared_draw_queue_appends = 0;
    uint64_t prepared_draw_queue_flushes = 0;
    uint64_t prepared_draw_queue_single_draw_flushes = 0;
    uint64_t prepared_draw_queue_draws_flushed = 0;
    uint64_t prepared_draw_queue_ranges_flushed = 0;
    uint64_t prepared_draw_queue_bytes_flushed = 0;
    uint64_t prepared_draw_queue_invalid_flushes = 0;
    uint64_t prepared_draw_queue_texture_plans_flushed = 0;
    uint64_t prepared_draw_queue_texture_loads_planned = 0;
    uint64_t prepared_draw_queue_texture_loads_executed = 0;
    std::array<uint64_t, kPreparedDrawFlushReasonCount>
        prepared_draw_queue_flush_reasons = {};
    std::array<uint64_t, kPreparedDrawQueueRejectReasonCount>
        prepared_draw_queue_reject_reasons = {};

    std::array<uint64_t, kCbvSlotCount> cbv_uploads = {};
    std::array<uint64_t, kCbvSlotCount> cbv_reuse_hits = {};
    std::array<uint64_t, kStageCount> descriptor_index_uploads = {};
    std::array<uint64_t, kCbvSlotCount> constant_payload_cache_hits = {};
    std::array<uint64_t, kCbvSlotCount> constant_payload_cache_misses = {};
    std::array<uint64_t, kCbvSlotCount> constant_payload_cache_bytes_saved = {};
    std::array<uint64_t, kStageCount> descriptor_index_payload_cache_hits = {};
    std::array<uint64_t, kStageCount> descriptor_index_payload_cache_misses =
        {};
    std::array<uint64_t, kStageCount>
        descriptor_index_payload_cache_bytes_saved = {};
    std::array<uint64_t, kStageCount> bindless_root_allocations = {};
    std::array<uint64_t, kStageCount> bindless_root_reuse_hits = {};
    std::array<uint64_t, kStageCount> bindless_root_arg_noop_updates = {};
    std::array<uint64_t, kStageCount> bindless_root_arg_slots_patched = {};
    std::array<uint64_t, kStageCount> bindless_root_arg_bytes_copied = {};
    std::array<uint64_t, kBindlessRootArgTelemetrySlotCount>
        bindless_root_arg_slot_patches = {};
    std::array<uint64_t, kBindlessRootRebuildReasonCount>
        bindless_root_rebuild_reasons = {};
    std::array<uint64_t, kBindlessRootSlotsChangedBinCount>
        bindless_root_slots_changed = {};
    std::array<uint64_t, kBindlessRootRebuildDetailCount>
        bindless_root_rebuild_details = {};
    std::array<uint64_t, kRenderEncoderBufferStageTelemetryCount>
        render_encoder_buffer_full_binds = {};
    std::array<uint64_t, kRenderEncoderBufferStageTelemetryCount>
        render_encoder_buffer_offset_binds = {};
    std::array<uint64_t, kRenderEncoderBufferStageTelemetryCount>
        render_encoder_buffer_noop_binds = {};
    std::array<uint64_t, kRenderEncoderBufferStageTelemetryCount>
        render_encoder_buffer_null_binds = {};
    std::array<uint64_t, kRenderEncoderBufferStageTelemetryCount>
        render_encoder_buffer_untracked_binds = {};
    uint64_t render_encoder_use_resource_calls = 0;
    uint64_t render_encoder_use_resource_skips = 0;
    uint64_t render_encoder_use_resource_upgrades = 0;
    uint64_t render_encoder_use_resources_batches = 0;
    uint64_t render_encoder_use_resources_requested = 0;
    uint64_t render_encoder_use_resources_skips = 0;
    std::array<uint64_t, kRenderResourceSetKindCount>
        render_resource_set_applies = {};
    std::array<uint64_t, kRenderResourceSetKindCount>
        render_resource_set_skips = {};
    std::array<uint64_t, kRenderResourceSetKindCount>
        render_resource_set_resources = {};
    std::array<uint64_t, kRenderResourceSetKindCount>
        render_resource_registry_serial_skips = {};
    std::array<uint64_t, kRenderResourceSetKindCount>
        render_resource_registry_builds = {};
    std::array<uint64_t, kRenderResourceSetKindCount>
        render_resource_registry_registers = {};
    uint64_t residency_set_allocations_added = 0;
    uint64_t residency_set_allocation_duplicates = 0;
    uint64_t residency_set_commits = 0;
    uint64_t residency_set_resource_refs_covered = 0;
    uint64_t residency_set_resource_refs_fallback = 0;
    uint64_t residency_set_use_resources_covered = 0;
    uint64_t residency_set_use_resources_fallback = 0;
    uint64_t residency_set_use_heaps_covered = 0;
    uint64_t residency_set_use_heaps_fallback = 0;
    uint64_t frame_slot_waits = 0;
    uint64_t frame_slot_wait_submission_count = 0;
    uint64_t frame_slot_wait_submission_last = 0;
  };

  void FlushCommandBufferAndWait(uint64_t timeout_ns, const char* context);
  MTL::RenderPassDescriptor* GetDrawRenderPassDescriptor(
      bool fallback_depth_attachment_required = false);
  bool BeginRenderEncoderForDraw(
      bool fallback_depth_attachment_required = false);
  void EndRenderEncoder(RenderEncoderEndReason reason);
  void EndCommandBuffer();
  bool CanEndSubmissionImmediately();
  void WaitForPendingCompletionHandlers();
  void ProcessCompletedSubmissions();
  void WaitForFrameSlotSubmission(uint64_t awaited_submission);
  void OpenFrameLifetime();
  void InvalidateFrameTransientBindings();
  void CloseFrameLifetime();
  void MaybeDumpBackendTelemetry(const char* reason, bool force = false);
  void ResetBackendTelemetry();
  void InitializeResidencySet();
  void ShutdownResidencySet();
  void RegisterInitialResidencySetResources();
  bool AddResidencySetResource(MTL::Resource* resource);
  bool IsResidencySetResourceCovered(MTL::Resource* resource) const;
  bool IsResidencySetHeapCovered(MTL::Heap* heap) const;
  bool AnySharedMemoryRangeInvalid(const SharedMemory::Range* ranges,
                                   uint32_t range_count) const;
  void RecordSharedMemoryLazyUploadRoute(
      const MetalSharedMemory::UploadRouteInfo& route_info,
      bool render_encoder_active);
  void PrepareSharedMemoryUploadBeforeDrawPass(
      const SharedMemory::Range* ranges, uint32_t range_count);
  bool HasActiveSharedMemoryWritePending() const;

  void UseRenderEncoderAttachmentHeaps(MTL::RenderPassDescriptor* descriptor);
  void AddRenderResourceRef(RenderResourceSet& set, MTL::Resource* resource,
                            MTL::ResourceUsage usage, MTL::RenderStages stages);
  void RestoreRenderResourceSet(RenderResourceSetKind kind,
                                RenderResourceSet& current,
                                const RenderResourceSet& snapshot);
  void PublishRenderResourceSet(RenderResourceSet& current,
                                RenderResourceSet&& next);
  uint64_t GetBindlessFixedResourceSourceSerial(
      MTL::ResourceUsage shared_memory_usage) const;
  uint64_t GetBindlessTextureResourceSourceSerial() const;
  uint64_t GetBindlessRootResourceSourceSerial(
      const UniformBufferInfo& uniforms) const;
  void PublishBindlessFixedResourceSet(MTL::ResourceUsage shared_memory_usage);
  void RestoreBindlessTextureResourceSet(const RenderResourceSet& set);
  void PublishBindlessTextureResourceSet();
  void PublishBindlessRootResourceSet(const UniformBufferInfo& uniforms);
  void ApplyRenderEncoderResourceSets();
  void ApplyRenderEncoderResourceSet(RenderResourceSetKind kind,
                                     const RenderResourceSet& set,
                                     uint64_t& applied_serial);
  EncoderResourceUsageState* FindOrInsertRenderEncoderResourceUsage(
      MTL::Resource* resource, bool& inserted);
  void GrowRenderEncoderResourceUsageTable(size_t min_capacity);
  void UseRenderEncoderResources(const MTL::Resource* const resources[],
                                 uint32_t count, MTL::ResourceUsage usage);
  void UseRenderEncoderResources(const MTL::Resource* const resources[],
                                 uint32_t count, MTL::ResourceUsage usage,
                                 MTL::RenderStages stages);
  void UseRenderEncoderHeap(MTL::Heap* heap);
  uint64_t GetBindlessDescriptorRetirementSubmission() const;
  void FreeViewBindlessIndexNow(uint32_t index);
  void FreeSamplerBindlessIndexNow(uint32_t index);

  struct PendingSharedMemoryWrite {
    uint32_t start = 0;
    uint32_t end = 0;
    uint64_t submission_id = 0;
    MTL::RenderStages producer_stages = MTL::RenderStages(0);
    bool active_render_encoder = false;
    bool fence_updated = false;
  };

  void MarkSharedMemoryWritePending(uint32_t address, uint32_t length,
                                    MTL::RenderStages producer_stages,
                                    bool active_render_encoder,
                                    bool fence_updated);
  bool PendingSharedMemoryWritesOverlapRange(uint32_t start,
                                             uint32_t length) const;
  bool PendingSharedMemoryWriteOverlapsRanges(
      const PendingSharedMemoryWrite& pending, const SharedMemoryRange* ranges,
      uint32_t range_count) const;
  bool PendingSharedMemoryWritesOverlapRanges(const SharedMemoryRange* ranges,
                                              uint32_t range_count) const;
  void UpdateSharedMemoryFenceForActiveRenderEncoder();
  void PruneCompletedSharedMemoryWrites(uint64_t completed_submission);
  void RetireFenceWaitedSharedMemoryWrites(const SharedMemoryRange* ranges,
                                           uint32_t range_count);
  bool EncodeSharedMemoryRenderReadDependencies(
      const SharedMemoryRange* ranges, uint32_t range_count,
      MTL::RenderStages consumer_stages);
  bool EncodeSharedMemoryBlitReadDependency(MTL::BlitCommandEncoder* encoder,
                                            uint32_t start, uint32_t length);

  // Fixed-function depth/stencil state (mirrors Vulkan/D3D12 dynamic state).
  void ApplyDepthStencilState(const DrawDynamicState& dynamic_state);
  void ApplyRasterizerState(const DrawDynamicState& dynamic_state);

  // Constants for the MSC path.
  static constexpr size_t kNullBufferSize = 4096;
  static constexpr size_t kCbvSizeBytes = 4096;

  // Constants for MSC descriptor heap sizes.
  static constexpr size_t kResourceHeapSlotsPerTable = 1025 + 2;
  static constexpr size_t kSamplerHeapSlotsPerTable = 257 + 2;
  static constexpr size_t kTopLevelABSlotsPerTable = 32;
  static_assert(kBindlessRootArgTelemetrySlotCount == kTopLevelABSlotsPerTable);
  static constexpr size_t kTopLevelABBytesPerTable =
      kTopLevelABSlotsPerTable * sizeof(uint64_t);
  // MSC explicit root signatures encode descriptor-table pointers and root
  // resource pointers as 64-bit entries in the top-level argument buffer.
  // Keep these in the same order as MetalShaderConverter's root signature.
  enum TopLevelABSlot : uint32_t {
    kTopLevelABSlotSRVSpace0,
    kTopLevelABSlotSRVSpace1,
    kTopLevelABSlotSRVSpace2,
    kTopLevelABSlotSRVSpace3,
    kTopLevelABSlotSRVSpace10,
    kTopLevelABSlotUAVSpace0,
    kTopLevelABSlotUAVSpace1,
    kTopLevelABSlotUAVSpace2,
    kTopLevelABSlotUAVSpace3,
    kTopLevelABSlotSamplerSpace0,
    kTopLevelABSlotCBVSystem,
    kTopLevelABSlotCBVFloat,
    kTopLevelABSlotCBVBoolLoop,
    kTopLevelABSlotCBVFetch,
    kTopLevelABSlotCBVDescriptorIndices,
  };
  // Descriptor tables and common CBVs are shared by every graphics stage, so
  // one top-level argument table can be rebound across
  // vertex/fragment/object/mesh stages. The first stage-local CBV slots are
  // kept near the shared slots for telemetry continuity.
  enum GraphicsRootABSlot : uint32_t {
    kGraphicsRootABSlotSRVSpace0 = kTopLevelABSlotSRVSpace0,
    kGraphicsRootABSlotSRVSpace1 = kTopLevelABSlotSRVSpace1,
    kGraphicsRootABSlotSRVSpace2 = kTopLevelABSlotSRVSpace2,
    kGraphicsRootABSlotSRVSpace3 = kTopLevelABSlotSRVSpace3,
    kGraphicsRootABSlotSRVSpace10 = kTopLevelABSlotSRVSpace10,
    kGraphicsRootABSlotUAVSpace0 = kTopLevelABSlotUAVSpace0,
    kGraphicsRootABSlotUAVSpace1 = kTopLevelABSlotUAVSpace1,
    kGraphicsRootABSlotUAVSpace2 = kTopLevelABSlotUAVSpace2,
    kGraphicsRootABSlotUAVSpace3 = kTopLevelABSlotUAVSpace3,
    kGraphicsRootABSlotSamplerSpace0 = kTopLevelABSlotSamplerSpace0,
    kGraphicsRootABSlotCBVSystem = kTopLevelABSlotCBVSystem,
    kGraphicsRootABSlotCBVVertexFloat = kTopLevelABSlotCBVFloat,
    kGraphicsRootABSlotCBVBoolLoop = kTopLevelABSlotCBVBoolLoop,
    kGraphicsRootABSlotCBVVertexFetch = kTopLevelABSlotCBVFetch,
    kGraphicsRootABSlotCBVVertexDescriptorIndices =
        kTopLevelABSlotCBVDescriptorIndices,
    kGraphicsRootABSlotCBVHullFloat,
    kGraphicsRootABSlotCBVHullFetch,
    kGraphicsRootABSlotCBVHullDescriptorIndices,
    kGraphicsRootABSlotCBVDomainFloat,
    kGraphicsRootABSlotCBVDomainFetch,
    kGraphicsRootABSlotCBVDomainDescriptorIndices,
    kGraphicsRootABSlotCBVPixelFloat,
    kGraphicsRootABSlotCBVPixelFetch,
    kGraphicsRootABSlotCBVPixelDescriptorIndices,
  };

  // System constants population (mirrors D3D12 implementation)
  void UpdateSystemConstantValues(bool shared_memory_is_uav,
                                  bool primitive_polygonal,
                                  uint32_t line_loop_closing_index,
                                  xenos::Endian index_endian,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);

  // Metal device and command queue (from provider)
  MTL::Device* device_ = nullptr;
  MTL::CommandQueue* command_queue_ = nullptr;
  MTL::ResidencySet* residency_set_ = nullptr;
  bool residency_set_supported_ = false;
  bool residency_set_enabled_ = false;
  bool residency_set_attached_ = false;
  // [draw-prep-lite2] insert/count/clear only; looked up for every resource of
  // every rebuilt render resource set (open addressing instead of node hashing).
  FlatPtrSet<MTL::Resource*> residency_set_resources_{FlatPtrSetLite2Enabled()};
  std::unordered_set<MTL::Heap*> residency_set_heaps_;
  MTL::SharedEvent* wait_shared_event_ = nullptr;
  uint64_t wait_shared_event_value_ = 0;
  // [res-scale] Fable morph readback at draw resolution scale > 1: the
  // scaled resolve is point-sampled back to 1x (XeniOS resolve_downscale)
  // into a CPU-visible buffer, then copied to guest RAM.
  bool MorphReadbackScaled(uint32_t written_address, uint32_t written_length,
                           uint32_t bits_per_pixel,
                           const char* diag_dump_path = nullptr);
  // Diagnostic (FABLE2_DIAG_RESOLVE_DUMP=<dir>, FABLE2_DIAG_RESOLVE_RANGE=
  // <lo>-<hi>): writes each resolve into the range to <dir> as 1x bytes
  // (downscaled at scale > 1). Guest RAM is not written.
  void DiagResolveDump(uint32_t written_address, uint32_t written_length);
  bool WaitForMorphReadbackSubmission(uint32_t written_address,
                                      uint32_t written_length);
  void NoteMorphReadback(const uint8_t* data, uint32_t written_address,
                         uint32_t written_length, bool scaled);
  MTL::ComputePipelineState* morph_downscale_pipeline_ = nullptr;
  bool morph_downscale_pipeline_failed_ = false;
  MTL::Buffer* morph_downscale_buffer_ = nullptr;
  MTL::Fence* shared_memory_fence_ = nullptr;
  SharedMemoryRequestReason current_shared_memory_upload_reason_ =
      SharedMemoryRequestReason::kUnknown;
  // Current command buffer and encoder
  MTL::CommandBuffer* current_command_buffer_ = nullptr;
  MTL::RenderCommandEncoder* current_render_encoder_ = nullptr;
  pass_trace::PassRef current_render_pass_trace_;  // [pass-trace] trace-only
  // [upload-precommit] pass-trace end-reason detail for the next
  // EndRenderEncoder (a static string), and the pending upload break cause.
  const char* render_end_detail_ = nullptr;
  // [upload-precommit] main command buffers signal their submission index on
  // main_cb_event_ when they finish; pre-commit command buffers signal
  // precommit_event_ (values 1, 2, ...) and update precommit_fence_.
  // GPU-only MTLEvents (lower wait latency than shared events); the CPU
  // tracks completed pre-commit values through completion handlers.
  MTL::Event* main_cb_event_ = nullptr;
  MTL::Event* precommit_event_ = nullptr;
  std::atomic<uint64_t> precommit_completed_value_{0};
  MTL::Fence* precommit_fence_ = nullptr;
  uint64_t precommit_event_value_ = 0;
  // Highest pre-commit value the open command buffer has waited for.
  uint64_t precommit_waited_value_ = 0;
  MTL::CommandBuffer* precommit_cb_ = nullptr;
  MTL::BlitCommandEncoder* precommit_blit_ = nullptr;
  void EncodePrecommitWait(MTL::CommandBuffer* cmd);
  // [texture-precommit] root textures (and views) sampled by draws already
  // encoded in the open main command buffer; cleared per command buffer.
  FlatPtrSet<const MTL::Resource*> cb_sampled_textures_;
  void NoteTexturesSampledByCurrentCommandBuffer(const RenderResourceSet& set);
  // [texture-precommit] true when every planned load may be encoded in the
  // pre-commit command buffer; counts tr_n_tex_* trace rows.
  bool ClassifyTextureMaterialization(
      const MetalTextureCache::TextureMaterializationPlan* const* plans,
      size_t count);
  void EncodePrecommitWaitCommitted(MTL::CommandBuffer* cmd);
  void CommitSharedMemoryPrecommit();
  bool EnsurePrecommitCommandBuffer();
  void EndPrecommitBlit();
  void WaitPrecommitInRenderEncoder();
  // Fence updates encoded in the open pre-commit command buffer.
  uint32_t precommit_fence_updates_ = 0;
  // [texture-precommit] a texture-load flush is being encoded into the
  // pre-commit command buffer (render pass left open).
  bool texture_precommit_active_ = false;
  bool TexturePrecommitAllowed() const;
  bool BeginTexturePrecommit();
  void EndTexturePrecommit();
  uint64_t precommit_open_value_ = 0;
  uint64_t precommit_committed_value_ = 0;
  uint64_t render_encoder_gen_ = 1;
  // [tile-resolve extent] docs/tile-resolve-20261013.md (follow-up).
  uint32_t pass_extent_h_ = 0;          // 0: full attachment height
  uint32_t pass_open_need_bottom_ = 0;  // rows the opening draw needs
  bool pass_drawn_any_ = false;
  uint32_t pass_draws_y1_ = 0;  // rows the pass's draws (not transfers) need
  uint64_t tres_kept_gen_ = 0;
  uint64_t tres_kept_counted_gen_ = 0;
  FlatPtrSet<const void*> pass_sampled_textures_;
  std::vector<const void*> pass_sampled_order_;
  FlatPtrSet<const void*> pass_sampled_bases_;
  size_t pass_sampled_based_ = 0;
  FlatPtrSet<const void*> pass_tile_written_;
  uint64_t pass_sampled_source_serial_ = 0;
  void TileResolveNotePassSampled(const RenderResourceSet& set);
  bool TileResolveKeptHazard(const PreparedDraw& draw) const;
  uint32_t pass_drawn_x0_ = 0, pass_drawn_y0_ = 0, pass_drawn_x1_ = 0,
           pass_drawn_y1_ = 0;
  const void* pass_extent_key_ = nullptr;
  struct PassExtentHistory {
    uint16_t rows[16] = {};
    uint8_t next = 0;
  };
  std::unordered_map<const void*, PassExtentHistory> pass_extent_history_;
  void PassExtentNoteDrawn(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
  void PassExtentRecordEnd();
  uint32_t PassExtentChoose(MTL::RenderPassDescriptor* desc,
                            uint32_t& full_width, uint32_t& full_height);
  uint64_t precommit_fence_waited_encoder_gen_ = 0;
  void EncodeMainCommandBufferEnd();
  int pending_upload_break_cause_ = 0;  // UploadBreakCause in the .cc
  void CountUploadBreak(int cause, bool prepare_path);
  MTL::BlitCommandEncoder* shared_memory_upload_blit_encoder_ = nullptr;
  MTL::RenderPassDescriptor* current_render_pass_descriptor_ = nullptr;
  bool current_render_encoder_has_zpd_visibility_ = false;

  // [occlusion-real] default on, FABLE2_OCCLUSION_REAL=0 off (metal_zpd_real.cc, CR-04): real
  // occlusion queries through Metal visibility results. BEGIN zeroes the guest
  // BEGIN record; draws inside the query count samples into visibility-buffer
  // slots (one per draw); END leaves D3D's 0xFFFFFEED sentinel in place until
  // the command buffers holding the query's draws complete, then the completion
  // handler writes the real count (guest polling sees "not ready" until then,
  // as on Xenos).
  struct ZpdReport {
    uint32_t begin_record = 0;
    uint32_t end_record = 0;
    uint64_t seq = 0;
    uint64_t samples = 0;  // normalised to guest scale, summed over segments
    uint32_t pending = 0;  // segments whose command buffer has not completed
    uint32_t guest_draws = 0;
    uint32_t dispatched = 0;
    uint64_t end_ns = 0;
    bool ended = false;
    bool uncertain = false;  // a draw may be missing: never report 0
  };
  struct ZpdSegment {
    uint32_t report = 0;
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint32_t scale = 1;
  };
  bool ZpdRealEnabled() const { return zpd_real_; }
  void ZpdInitialize();
  void ZpdShutdown();
  bool ZpdExecuteEvent(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  void ZpdBeginDrawSpan();
  void ZpdEndDrawSpan(bool dispatched);
  void ZpdNoteIssueDraw(bool dispatched_before_valid, uint32_t dispatched_before);
  void ZpdOnSubmissionCompleted(uint64_t submission, bool error);
  // Called with zpd_mutex_ held.
  void ZpdFinalizeLocked(uint32_t report_id, ZpdReport& report);
  void ZpdWriteGuestResult(uint32_t end_record, uint32_t samples);
  void ZpdMaybeLogStats();
  bool zpd_real_ = false;
  std::unique_ptr<MetalZPDVisibilityPool> zpd_pool_;
  std::mutex zpd_mutex_;
  std::unordered_map<uint32_t, ZpdReport> zpd_reports_;
  std::unordered_map<uint32_t, uint64_t> zpd_record_seq_;
  std::unordered_map<uint64_t, std::vector<ZpdSegment>> zpd_pending_segments_;
  uint32_t zpd_next_report_ = 1;
  uint32_t zpd_active_report_ = 0;  // CP thread
  // Open draw span (CP thread).
  bool zpd_span_open_ = false;
  uint64_t zpd_span_encoder_gen_ = 0;
  struct ZpdStats {
    std::atomic<uint64_t> begins{0}, ends{0}, orphan_ends{0}, overlaps{0},
        segments{0}, results{0}, zero_results{0}, uncertain{0}, stale{0},
        slot_exhausted{0}, latency_ns{0}, latency_max_ns{0}, encoder_changed{0},
        skipped_draws{0}, no_visibility{0};
  } zpd_stats_;
  uint64_t zpd_stats_last_frame_ = 0;
  std::unordered_map<std::string, uint64_t> zpd_skip_reasons_;  // CP thread
  NS::AutoreleasePool* command_buffer_autorelease_pool_ = nullptr;

  // Tracks resources marked via useResource for the current render encoder to
  // avoid redundant driver calls across draws within the same encoder.
  struct EncoderResourceUsageTableEntry {
    MTL::Resource* resource = nullptr;
    EncoderResourceUsageState state = {};
  };
  std::vector<EncoderResourceUsageTableEntry>
      render_encoder_resource_usage_table_;
  size_t render_encoder_resource_usage_count_ = 0;
  std::vector<MTL::Heap*> render_encoder_heap_usage_;
  bool shmem_precommit_in_tex_precommit_ = false;  // [pass-merge]
  BackendTelemetryStats backend_telemetry_;
  uint64_t backend_telemetry_last_dump_swap_ = 0;

  static constexpr size_t kPreparedDrawQueueMaxDraws = 64;
  // [draw-prep-lite] materialization range count / bytes of the queued draws.
  size_t prepared_draw_queue_range_total_ = 0;
  uint64_t prepared_draw_queue_byte_total_ = 0;
  static constexpr size_t kPreparedDrawQueueMaxRanges = 4096;
  static constexpr uint64_t kPreparedDrawQueueMaxBytes = 64ull * 1024 * 1024;
  // [prepared-draw-slim] the queue holds the draw records by pointer: queueing, flushing and
  // recycling a draw moves a pointer, not the ~2.6 KB record.
  using PreparedDrawPtr = std::unique_ptr<PreparedDraw>;
  std::vector<PreparedDrawPtr> prepared_draw_queue_;
  // [prepared-draw-pool] records (their vectors keep capacity) and queue storage reused
  // across draws and flushes.
  std::vector<PreparedDrawPtr> prepared_draw_free_;
  std::vector<std::vector<PreparedDrawPtr>> prepared_draw_queue_storage_spares_;
  RenderResourceSet resource_set_scratch_;
  PreparedDrawRenderTargetKey prepared_draw_queue_render_target_key_ = {};
  bool prepared_draw_queue_render_target_key_valid_ = false;
  // [native-scale] texture-space hint of the previous draw (FABLE2_TEXSPACE_NATIVE).
  bool last_texspace_hint_ = false;
  bool flushing_prepared_draw_queue_ = false;

  // Shared memory for Xbox 360 memory access
  std::unique_ptr<MetalSharedMemory> shared_memory_;
  std::unique_ptr<MetalPrimitiveProcessor> primitive_processor_;
  bool frame_open_ = false;

  bool saw_swap_ = false;
  // [hitch] the idle scope opened in PrepareForWait (closed in ReturnFromWait).
  bool hitch_idle_open_ = false;
  uint32_t last_swap_ptr_ = 0;
  uint32_t last_swap_width_ = 0;
  uint32_t last_swap_height_ = 0;
  std::unordered_map<uint32_t, bool> swap_dest_swaps_by_base_;

  // Pipeline cache (owns shaders, pipelines, shader translation components).
  std::unique_ptr<MetalPipelineCache> pipeline_cache_;

  struct DepthStencilStateKey {
    uint32_t depth_control;
    uint32_t stencil_ref_mask_front;
    uint32_t stencil_ref_mask_back;
    uint32_t polygonal_and_backface;
    bool operator==(const DepthStencilStateKey& other) const {
      return depth_control == other.depth_control &&
             stencil_ref_mask_front == other.stencil_ref_mask_front &&
             stencil_ref_mask_back == other.stencil_ref_mask_back &&
             polygonal_and_backface == other.polygonal_and_backface;
    }
    struct Hasher {
      size_t operator()(const DepthStencilStateKey& key) const {
        size_t h = size_t(key.depth_control);
        h ^= size_t(key.stencil_ref_mask_front) << 1;
        h ^= size_t(key.stencil_ref_mask_back) << 2;
        h ^= size_t(key.polygonal_and_backface) << 3;
        return h;
      }
    };
  };

  std::unordered_map<DepthStencilStateKey, MTL::DepthStencilState*,
                     DepthStencilStateKey::Hasher>
      depth_stencil_state_cache_;

  bool mesh_shader_supported_ = false;

  // Texture cache for guest texture uploads
  std::unique_ptr<MetalTextureCache> texture_cache_;

  // Render target cache for framebuffer management
  std::unique_ptr<MetalRenderTargetCache> render_target_cache_;

  // Null resources for unbound slots
  MTL::Buffer* null_buffer_ = nullptr;
  MTL::Texture* null_texture_ = nullptr;
  MTL::SamplerState* null_sampler_ = nullptr;

  // Persistent bindless descriptor heaps.
  // Canonical texture views and samplers get stable slot indices allocated on
  // demand and freed on destruction. Non-canonical texture views may use
  // submission-lifetime slots from the same heap. The heaps are bound once per
  // encoder.
  // 1M entries (24 MiB). Metal has no API-side descriptor heap cap — the heap
  // is just an MTLBuffer. D3D12 uses 262144 but doesn't need per-swizzle
  // texture views; 4x headroom covers the swizzled-view multiplier and avoids
  // exhausting the heap before the first submission completes.
  static constexpr uint32_t kViewBindlessHeapSize = 1048576;
  static constexpr uint32_t kSamplerBindlessHeapSize = 2048;
  MTL::Buffer* view_bindless_heap_ = nullptr;
  MTL::Buffer* sampler_bindless_heap_ = nullptr;
  // Explicit-layout top-level bindings for shared memory / EDRAM use small
  // dedicated system tables rather than overlapping the bindless texture heap.
  MTL::Buffer* system_view_tables_ = nullptr;
  static constexpr uint32_t kSystemViewTableSRVSharedMemory = 0;
  static constexpr uint32_t kSystemViewTableSRVNull = 1;
  static constexpr uint32_t kSystemViewTableUAVNullStart = 2;
  static constexpr uint32_t kSystemViewTableUAVSharedMemoryStart = 4;
  static constexpr uint32_t kSystemViewTableEntryCount = 6;

  // Simple bump allocator with free list for persistent heap slots.
  uint32_t view_bindless_heap_next_ = 0;
  std::vector<uint32_t> view_bindless_heap_free_;
  bool view_bindless_heap_exhausted_logged_ = false;
  uint32_t sampler_bindless_heap_next_ = 0;
  std::vector<uint32_t> sampler_bindless_heap_free_;
  bool sampler_bindless_heap_exhausted_logged_ = false;
  struct RetiredBindlessDescriptor {
    uint32_t index;
    uint64_t submission_id;
  };
  std::deque<RetiredBindlessDescriptor> retired_view_bindless_indices_;
  std::deque<RetiredBindlessDescriptor> retired_sampler_bindless_indices_;

  struct RetiredMetalBuffer {
    MTL::Buffer* buffer = nullptr;
    uint64_t submission_id = 0;
  };
  std::deque<RetiredMetalBuffer> retired_memexport_index_buffers_;

  MTL::Buffer* tessellator_tables_buffer_ = nullptr;

  // System constants - matches DxbcShaderTranslator::SystemConstants layout
  DxbcShaderTranslator::SystemConstants system_constants_;

  // Fixed-function dynamic state cached per render encoder.
  MTL::RenderPipelineState* current_render_pipeline_state_ = nullptr;
  float ff_blend_factor_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  bool ff_blend_factor_valid_ = false;
  bool rasterizer_state_valid_ = false;
  MTL::CullMode current_cull_mode_ = MTL::CullModeNone;
  MTL::Winding current_front_facing_winding_ = MTL::WindingCounterClockwise;
  MTL::TriangleFillMode current_triangle_fill_mode_ = MTL::TriangleFillModeFill;
  float current_depth_bias_values_[3] = {0.0f, 0.0f, 0.0f};
  MTL::DepthClipMode current_depth_clip_mode_ = MTL::DepthClipModeClip;
  MTL::DepthStencilState* current_depth_stencil_state_ = nullptr;
  bool stencil_reference_valid_ = false;
  uint32_t current_stencil_reference_ = 0;
  bool viewport_dirty_ = true;
  MTL::Viewport cached_viewport_ = {};
  bool scissor_dirty_ = true;
  MTL::ScissorRect cached_scissor_ = {};

  // Constant buffer dirty tracking (D3D12 pattern).
  // Each binding records the pool-allocated buffer, offset, and GPU address
  // for the most recent constant upload.  When up_to_date is true, the draw
  // reuses that upload through the top-level argument buffer instead of
  // gathering and uploading the same CBV again.
  struct ConstantBufferBinding {
    MTL::Buffer* buffer = nullptr;
    NS::UInteger offset = 0;
    uint64_t gpu_address = 0;
    size_t size = 0;
    // Guest frame that originally allocated the pool slice this binding
    // references. The upload pool is frame-lifetime here, matching D3D12 and
    // Vulkan so command-buffer rollover inside one frame doesn't force a
    // re-upload of unchanged constants.
    uint64_t upload_frame = 0;
    bool up_to_date = false;
  };
  static constexpr size_t kConstantPayloadSharedStage = kStageCount;
  static constexpr size_t kConstantPayloadCacheMaxBytes = 64 * 1024 * 1024;
  struct ConstantPayloadCacheEntry {
    size_t cbv_slot = 0;
    size_t stage_index = kConstantPayloadSharedStage;
    size_t size = 0;
    ConstantBufferBinding binding;
    std::vector<uint8_t> bytes;
  };
  using StageRootArgumentEntries =
      std::array<uint64_t, kTopLevelABSlotsPerTable>;
  struct StageRootArgumentAllocation {
    MTL::Buffer* buffer = nullptr;
    NS::UInteger offset = 0;
    uint64_t gpu_address = 0;
    // Same frame-lifetime rationale as ConstantBufferBinding.
    uint64_t upload_frame = 0;
    bool valid = false;
  };
  // Mirror D3DMetal's model: track the current graphics root argument entries,
  // patch the logical slots that changed, and write one fresh frame-lifetime
  // table snapshot for Metal to bind.
  StageRootArgumentEntries BuildGraphicsRootArgumentEntries(
      const UniformBufferInfo& uniforms, bool shared_memory_is_uav,
      bool use_tessellation_emulation) const;
  bool AllocateStageRootArgument(size_t stage_index,
                                 const StageRootArgumentEntries& entries,
                                 StageRootArgumentAllocation& allocation_out);
  bool TryReuseConstantPayloadFromCache(ConstantBufferBinding& binding,
                                        size_t cbv_slot, size_t stage_index,
                                        const uint8_t* payload, size_t size);
  void StoreConstantPayloadInCache(const ConstantBufferBinding& binding,
                                   size_t cbv_slot, size_t stage_index,
                                   const uint8_t* payload, size_t size);
  ConstantBufferBinding cbuffer_binding_system_;
  ConstantBufferBinding cbuffer_binding_float_vertex_;
  ConstantBufferBinding cbuffer_binding_float_pixel_;
  ConstantBufferBinding cbuffer_binding_bool_loop_;
  // Fetch constants are one physical register file, but each shader stage gets
  // its own snapshot so one stage's fetch-register churn doesn't force the
  // other stage's root CBV pointer to move.
  std::array<ConstantBufferBinding, kStageCount> cbuffer_binding_fetch_ = {};
  ConstantBufferBinding cbuffer_binding_descriptor_indices_vertex_;
  ConstantBufferBinding cbuffer_binding_descriptor_indices_pixel_;

  // Per-CBV payload caches.  We always honour the "register write dirties
  // live constant range" semantic from metal_command_processor.cc (matches
  // D3D12/Vulkan).  But after packing the new payload into scratch, if the
  // bytes equal the previous upload we keep the previous pool slice instead
  // of allocating a fresh one.  Mirrors the descriptor-indices
  // current_/scratch_ pattern in PrepareDrawConstants.
  std::vector<uint8_t> current_payload_system_;
  std::vector<uint8_t> current_payload_float_vertex_;
  std::vector<uint8_t> current_payload_float_pixel_;
  std::vector<uint8_t> current_payload_bool_loop_;
  std::array<std::vector<uint8_t>, kStageCount> current_payload_fetch_;
  std::vector<uint8_t> scratch_payload_system_;
  std::vector<uint8_t> scratch_payload_float_vertex_;
  std::vector<uint8_t> scratch_payload_float_pixel_;
  std::vector<uint8_t> scratch_payload_bool_loop_;
  std::array<std::vector<uint8_t>, kStageCount> scratch_payload_fetch_;
  std::unordered_map<uint64_t, std::vector<ConstantPayloadCacheEntry>>
      constant_payload_cache_;
  size_t constant_payload_cache_bytes_ = 0;
  std::array<DxbcShader::FetchConstantDwordMask, kStageCount>
      fetch_constant_dirty_masks_ = {};

  // Float constant usage bitmaps for the current shader pair.
  // Used to gate WriteRegister invalidation: only dirty the float CBV
  // when the written register is in the current shader's bitmap.
  // Matches D3D12's current_float_constant_map_vertex_/pixel_ at
  // d3d12_command_processor.h:829-830.
  uint64_t current_float_constant_map_vertex_[4] = {};
  uint64_t current_float_constant_map_pixel_[4] = {};
  size_t current_texture_layout_uid_vertex_ = 0;
  size_t current_texture_layout_uid_pixel_ = 0;
  size_t current_sampler_layout_uid_vertex_ = 0;
  size_t current_sampler_layout_uid_pixel_ = 0;
  std::vector<MetalTextureCache::TextureSRVKey>
      current_texture_srv_keys_vertex_;
  std::vector<MetalTextureCache::TextureSRVKey> current_texture_srv_keys_pixel_;
  std::vector<MetalTextureCache::SamplerParameters> current_samplers_vertex_;
  std::vector<MetalTextureCache::SamplerParameters> current_samplers_pixel_;
  std::vector<MTL::Texture*> current_texture_bindless_resources_vertex_;
  std::vector<MTL::Texture*> current_texture_bindless_resources_pixel_;
  std::vector<uint32_t> current_descriptor_indices_vertex_;
  std::vector<uint32_t> current_descriptor_indices_pixel_;
  std::vector<uint32_t> scratch_texture_bindless_indices_vertex_;
  std::vector<MTL::Texture*> scratch_texture_bindless_resources_vertex_;
  std::vector<uint32_t> scratch_sampler_bindless_indices_vertex_;
  std::vector<uint32_t> scratch_texture_bindless_indices_pixel_;
  std::vector<MTL::Texture*> scratch_texture_bindless_resources_pixel_;
  std::vector<uint32_t> scratch_sampler_bindless_indices_pixel_;
  std::vector<uint32_t> scratch_descriptor_indices_vertex_;
  std::vector<uint32_t> scratch_descriptor_indices_pixel_;
  std::array<bool, kStageCount> current_bindless_stage_root_valid_ = {};
  std::array<bool, kStageCount>
      current_bindless_stage_root_frame_open_rebuild_pending_ = {};
  std::array<uint64_t, kStageCount> current_bindless_stage_root_serials_ = {};
  RenderResourceSet current_bindless_fixed_resource_set_;
  RenderResourceSet current_bindless_texture_resource_set_;
  RenderResourceSet current_bindless_root_resource_set_;
  uint64_t current_bindless_fixed_resource_source_serial_ = 0;
  uint64_t current_bindless_texture_resource_source_serial_ = 0;
  uint64_t current_bindless_root_resource_source_serial_ = 0;
  uint64_t render_encoder_bindless_fixed_resources_serial_ = 0;
  uint64_t render_encoder_bindless_texture_resources_serial_ = 0;
  uint64_t render_encoder_bindless_root_resources_serial_ = 0;
  std::array<uint64_t, kStageCount>
      render_encoder_bindless_stage_root_bind_serials_ = {};
  bool render_encoder_bindless_table_bind_mesh_path_ = false;
  bool render_encoder_bindless_table_bind_tessellation_ = false;
  std::array<StageRootArgumentAllocation, kStageCount>
      current_bindless_stage_root_arguments_ = {};
  std::array<StageRootArgumentEntries, kStageCount>
      current_bindless_stage_root_entries_ = {};
  std::array<bool, kStageCount> current_bindless_stage_root_entries_valid_ = {};
  std::array<std::array<uint64_t, kCbvSlotCount>, kStageCount>
      current_bindless_cbv_gpu_addresses_ = {};
  std::array<std::array<MTL::Buffer*, kCbvSlotCount>, kStageCount>
      current_bindless_cbv_buffers_ = {};
  std::array<std::array<NS::UInteger, kCbvSlotCount>, kStageCount>
      current_bindless_cbv_offsets_ = {};
  std::array<uint32_t, kStageCount> current_bindless_active_cbv_masks_ = {};
  std::array<DxbcShader::FetchConstantDwordMask, kStageCount>
      current_fetch_constant_dword_masks_ = {};
  bool current_bindless_shared_memory_is_uav_ = false;

  // Pool for frame-lifetime constant and top-level argument allocations
  // (replaces the ring's uniforms_buffer_ for constant data).
  std::unique_ptr<MetalUploadBufferPool> constant_buffer_pool_;
  enum class RenderEncoderBufferStage : uint32_t {
    kVertex,
    kFragment,
    kObject,
    kMesh,
    kCount,
  };
  struct RenderEncoderBufferBinding {
    MTL::Buffer* buffer = nullptr;
    NS::UInteger offset = 0;
    bool valid = false;
  };
  static constexpr size_t kTrackedRenderEncoderBufferBindingCount = 32;
  std::array<std::array<RenderEncoderBufferBinding,
                        kTrackedRenderEncoderBufferBindingCount>,
             size_t(RenderEncoderBufferStage::kCount)>
      render_encoder_buffer_bindings_ = {};
  void ResetRenderEncoderBufferBindings();
  void InvalidateRenderEncoderBufferBinding(RenderEncoderBufferStage stage,
                                            NS::UInteger index);
  void SetRenderEncoderBuffer(RenderEncoderBufferStage stage,
                              MTL::Buffer* buffer, NS::UInteger offset,
                              NS::UInteger index);
  void SetRenderEncoderVertexBuffer(MTL::Buffer* buffer, NS::UInteger offset,
                                    NS::UInteger index);
  void SetRenderEncoderFragmentBuffer(MTL::Buffer* buffer, NS::UInteger offset,
                                      NS::UInteger index);
  void SetRenderEncoderObjectBuffer(MTL::Buffer* buffer, NS::UInteger offset,
                                    NS::UInteger index);
  void SetRenderEncoderMeshBuffer(MTL::Buffer* buffer, NS::UInteger offset,
                                  NS::UInteger index);
  // Track which heap buffer binds have been set on the current encoder.
  bool heap_binds_set_on_encoder_ = false;

  std::atomic<uint64_t> completed_command_buffers_{0};
  std::atomic<uint32_t> pending_completion_handlers_{0};
  std::mutex completion_mutex_;
  std::condition_variable completion_cond_;
  uint64_t submission_current_ = 0;
  uint64_t submission_completed_processed_ = 0;
  static constexpr uint32_t kMaxFramesInFlight = 3;
  // Guest frame index. Transient constant and MSC root argument allocations are
  // tagged with this, not with Metal command-buffer submissions, so they can be
  // reused across mid-frame command-buffer rollover.
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  uint64_t closed_frame_submissions_[kMaxFramesInFlight] = {};

  bool submission_has_draws_ = false;

  std::vector<PendingSharedMemoryWrite> pending_shared_memory_writes_;
  // [draw-prep-lite2] RetireFenceWaitedSharedMemoryWrites builds the retained list here and
  // swaps it in (no 64-entry stack array fill, no copy back).
  std::vector<PendingSharedMemoryWrite> pending_shared_memory_writes_scratch_;
  // [draw-prep-lite2] descriptor-indices payload scratch (was a vector per upload).
  std::vector<uint8_t> descriptor_payload_scratch_;
  MTL::RenderStages active_render_encoder_shared_memory_write_stages_ =
      MTL::RenderStages(0);
  // FABLE2_RENDER_BARRIER_DEDUP (default on): skip a render-encoder memory
  // barrier or shared-memory fence wait that an earlier one in the same encoder
  // already covers (same producer generation, superset of stages).
  uint64_t shared_memory_active_write_gen_ = 0;
  uint64_t shared_memory_fence_update_gen_ = 0;
  uint64_t render_barrier_gen_ = UINT64_MAX;
  NS::UInteger render_barrier_producers_ = 0;
  NS::UInteger render_barrier_consumers_ = 0;
  uint64_t render_fence_wait_gen_ = UINT64_MAX;
  NS::UInteger render_fence_waited_stages_ = 0;
  void ResetRenderDependencyDedup() {
    render_barrier_gen_ = UINT64_MAX;
    render_barrier_producers_ = render_barrier_consumers_ = 0;
    render_fence_wait_gen_ = UINT64_MAX;
    render_fence_waited_stages_ = 0;
  }

  TraceWriter trace_writer_{nullptr};
  uint64_t diagnostic_swap_count_ = 0;

  // Memexport tracking for shared memory invalidation.
  std::vector<draw_util::MemExportRange> memexport_ranges_;

  bool gamma_ramp_256_entry_table_up_to_date_ = false;
  bool gamma_ramp_pwl_up_to_date_ = false;
  struct CheckpointBarrier {
    std::mutex mutex;
    std::condition_variable changed;
    bool ready=false, released=false, succeeded=false;
  };
  std::shared_ptr<CheckpointBarrier> checkpoint_barrier_;

};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_COMMAND_PROCESSOR_H_
