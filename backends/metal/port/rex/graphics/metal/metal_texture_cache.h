/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_
#define XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rex/graphics/pipeline/shader/dxbc.h"
#include "rex/graphics/register_file.h"
#include "rex/graphics/pipeline/texture/cache.h"
#include "rex/graphics/pipeline/texture/info.h"
#include "rex/graphics/util/draw.h"
#include "rex/graphics/xenos.h"
#include "rex/memory.h"

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace rex {
namespace graphics {
namespace metal {

class MetalCommandProcessor;
class MetalSharedMemory;
class MetalHeapPool;

class MetalTextureCache : public TextureCache {
 public:
  MetalTextureCache(MetalCommandProcessor* command_processor,
                    const RegisterFile& register_file,
                    MetalSharedMemory& shared_memory,
                    uint32_t draw_resolution_scale_x,
                    uint32_t draw_resolution_scale_y);
  ~MetalTextureCache();

  bool Initialize();
  void Shutdown();
  void ClearCache() override;
  void CompletedSubmissionUpdated(uint64_t completed_submission_index) override;
  bool TrimViewBindlessPressure(uint32_t needed_slot_count = 1);

  // [rt-shrink] memory report (FABLE2_MEM_REPORT): texture and scaled-resolve
  // buffer allocations (MTLResource allocatedSize).
  struct MemReportInfo {
    uint64_t tex_bytes = 0, tex_count = 0;
    uint64_t scaled_bytes = 0, scaled_count = 0;
    uint64_t retired_bytes = 0, retired_count = 0;
    uint64_t heap_bytes = 0, heap_used_bytes = 0, heap_count = 0;
    uint64_t staging_bytes = 0;  // pooled texture-upload staging buffers
  };
  void GetMemReportInfo(MemReportInfo& out) const;
  static inline std::atomic<int64_t> mem_tex_bytes_{0};
  static inline std::atomic<int64_t> mem_tex_count_{0};

  // Bindless persistent heap index lookups.
  // Returns the persistent view_bindless_heap_ slot for the texture that
  // would be resolved for the given binding parameters.  Falls back to the
  // null texture's persistent slot when no valid texture is found.  If
  // texture_for_encoder_out is provided, also returns the Metal texture/view
  // that must be marked as used by the active render encoder.
  uint32_t GetBindlessSRVIndexForBinding(
      uint32_t fetch_constant, xenos::FetchOpDimension dimension,
      bool is_signed, MTL::Texture** texture_for_encoder_out = nullptr,
      uint32_t view_levels = 0);
  // Returns the persistent sampler_bindless_heap_ slot for the sampler that
  // would be created for the given binding.  Falls back to
  // null_sampler_bindless_index_ when no sampler can be resolved.
  uint32_t GetBindlessSamplerIndexForBinding(
      const DxbcShader::SamplerBinding& binding);

  struct TextureSRVKey {
    TextureKey key;
    uint32_t host_swizzle;
    uint8_t swizzled_signs;
    // [mip-lod] levels of the bound view (0: all).
    uint8_t view_levels = 0;
  };

  // view_levels: per fetch constant (pixel stage, [mip-lod]); null for none.
  bool AreActiveTextureSRVKeysUpToDate(
      const TextureSRVKey* keys,
      const DxbcShader::TextureBinding* host_shader_bindings,
      size_t host_shader_binding_count,
      const uint8_t* view_levels = nullptr) const;
  void WriteActiveTextureSRVKeys(
      TextureSRVKey* keys,
      const DxbcShader::TextureBinding* host_shader_bindings,
      size_t host_shader_binding_count,
      const uint8_t* view_levels = nullptr) const;

  // [mip-lod] (metal_miplod_proof.h) the draw's state the proof needs.
  struct MipLodDrawInputs {
    Shader* vertex_shader = nullptr;
    Shader* pixel_shader = nullptr;
    const std::vector<DxbcShader::SamplerBinding>* pixel_samplers = nullptr;
    uint32_t interpolator_mask = 0;
    uint32_t interpolators_centroid = 0;
    bool param_gen = false;
    uint32_t param_gen_interpolator = 0;
    // Host vertex shader type kVertex, not tessellated.
    bool plain_vertex = false;
    xenos::PrimitiveType primitive = xenos::PrimitiveType::kNone;
    bool primitive_reset = false;
    uint32_t vertex_count = 0;
    // PrimitiveProcessor::ProcessedIndexBufferType.
    uint32_t index_type = 0;
    uint32_t index_address = 0;
    bool index_32bit = false;
    // System constants of the draw (vertex index transform).
    uint32_t index_endian = 0, index_offset = 0, index_min = 0, index_max = 0,
             line_loop_closing_index = 0;
    // Host framebuffer pixels per unit of vertex shader position X / Y
    // (NDC scale and viewport extent; W is proven to be 1).
    double pixels_per_x = 0.0, pixels_per_y = 0.0;
    // GPU-visible bytes of a shared-memory range when no GPU work still
    // pending can change them, else null.
    std::function<const uint8_t*(uint32_t, uint32_t)> read;
  };
  // Bounds the draw's pixel-stage fetches of candidate textures, sets
  // miplod_view_levels_ and notes what the draw samples.
  void MipLodDraw(const MipLodDrawInputs& in);
  // [mip-lod] (metal_miplod_proof.h) per-draw proof bookkeeping.
  // Levels of the view each pixel-stage fetch constant binds this draw
  // (0: the whole texture).
  uint8_t miplod_view_levels_[32] = {};
  // A draw samples `texture` (a Texture*) reading at most level max_read
  // (-1: not bounded). Kept per open render pass (epoch).
  void MipLodNoteSample(const void* texture, int32_t max_read);
  // Marks a texture whose stored mip level a fused resolve writes.
  static void MipLodMarkCandidate(const void* texture);
  static bool MipLodIsCandidate(const void* texture);
  // 1: no draw of the open pass samples the texture; 2: every draw of the
  // open pass that samples it reads only levels below `level`; 0 otherwise.
  int MipLodPassSafeForLevel(const void* texture, uint32_t level) const;
  uint64_t miplod_epoch() const { return miplod_epoch_; }

  MTL::Texture* RequestSwapTexture(uint32_t& width_scaled_out,
                                   uint32_t& height_scaled_out,
                                   xenos::TextureFormat& format_out);

  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;
      xenos::ClampMode clamp_y : 3;
      xenos::ClampMode clamp_z : 3;
      xenos::BorderColor border_color : 2;
      uint32_t mag_linear : 1;
      uint32_t min_linear : 1;
      uint32_t mip_linear : 1;
      xenos::AnisoFilter aniso_filter : 3;
      uint32_t mip_min_level : 4;
      uint32_t mip_base_map : 1;
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    bool operator==(const SamplerParameters& other) const {
      return value == other.value;
    }
    bool operator!=(const SamplerParameters& other) const {
      return value != other.value;
    }
  };

  SamplerParameters GetSamplerParameters(
      const DxbcShader::SamplerBinding& binding) const;
  // [draw-prep-lite2] GetSamplerParameters is a pure function of the six fetch
  // constant dwords, the binding's four requested filters and the
  // anisotropic_override cvar: memoized by all of them (direct-mapped, command
  // processor thread only, like the bindings).
  struct SamplerParamsMemoEntry {
    uint32_t dwords[6] = {};
    uint32_t request = 0;
    int32_t aniso_override = 0;
    SamplerParameters parameters;
    bool valid = false;
  };
  static constexpr uint32_t kSamplerParamsMemoSizeLog2 = 8;
  mutable std::array<SamplerParamsMemoEntry, size_t(1)
                                                 << kSamplerParamsMemoSizeLog2>
      sampler_params_memo_;
  // [draw-prep-lite] GetBindlessSamplerIndexForBinding for parameters the caller already built.
  uint32_t GetBindlessSamplerIndexForParameters(SamplerParameters parameters);
  MTL::SamplerState* GetOrCreateSampler(SamplerParameters parameters);

  // TextureCache virtual method overrides
  void RequestTextures(uint32_t used_texture_mask) override;
  // [load-check-exact] frame count for the verify mode.
  void BeginFrame() override {
    ++exact_verify_frame_;
    // The process hard-exits at close: log the totals every 256 frames.
    if (!(exact_verify_frame_ & 255) && (load_hint_used_ | load_hint_rechecks_ | request_deferred_loads_ |
                                          request_gap_loads_)) {
      LogLoadCheckSummary("frame");
    }
    TextureCache::BeginFrame();
  }
  struct TextureMaterializationPlan;
  void RequestTexturesWithoutLoading(
      uint32_t used_texture_mask,
      const TextureMaterializationPlan* plan = nullptr);
  // [draw-prep-lite2] RequestTextures with the caller's MayRequestTexturesLoadData
  // result for the same mask, computed earlier in the same draw right after taking
  // `state`. If the state changed since (a watch callback, a texture created or
  // destroyed, bindings reset, a scaled-resolve change), the check runs again, as
  // before; otherwise the earlier result is exact. Loads outside the upload batch
  // (which the original asserts never happen) are counted and logged.
  void RequestTexturesWithLoadHint(uint32_t used_texture_mask,
                                   bool may_load_data,
                                   const LoadCheckState& state);
  struct TextureMaterializationPlan {
    std::vector<SharedMemory::Range> source_ranges;
    uint32_t request_count = 0;
    uint32_t planned_load_count = 0;
    uint32_t executed_load_count = 0;

    void Reset() {
      source_ranges.clear();
      texture_loads_.clear();
      renamed_loads_.clear();
      request_count = 0;
      planned_load_count = 0;
      executed_load_count = 0;
    }
    bool NeedsTextureUpload() const { return !texture_loads_.empty(); }
    bool PlansLoadOf(const void* texture) const {
      for (const auto& l : texture_loads_) {
        if (l.texture == texture) return true;
      }
      return false;
    }

   private:
    friend class MetalTextureCache;
    struct TextureLoad {
      Texture* texture = nullptr;
      bool load_base = false;
      bool load_mips = false;
      bool use_cpu_source = false;
      // [tex-rename] the texture's backing was renamed for this load.
      bool renamed = false;
    };
    std::vector<TextureLoad> texture_loads_;
    // [tex-rename] renamed loads, kept after execution for the settle step.
    std::vector<TextureLoad> renamed_loads_;
  };
  // [tex-rename] FABLE2_TEX_RENAME (docs/tex-rename-20261013.md): a full
  // reload of a texture that the open command buffer already sampled goes
  // into a fresh backing texture (new bindless slots) instead of ending the
  // render pass; draws encoded before keep sampling the old backing.
  static bool TexRenameEnabled();
  static bool TexRenameVerify();
  static bool TexRenameNoopEnabled();
  // After a flush's loads: textures renamed for these plans whose load did
  // not write the new backing (failed / kept / skipped) get the old backing's
  // content copied (and, with FABLE2_TEX_RENAME_VERIFY, the split path's load
  // is re-run into the old backing and compared). True when there is such
  // work; the caller ends the render pass first, then calls ...Encode.
  bool TexRenameNeedsSettle(
      const TextureMaterializationPlan* const* plans, size_t count) const;
  void TexRenameSettleEncode(const TextureMaterializationPlan* const* plans,
                             size_t count, MTL::CommandBuffer* cmd);
  // Renamed textures whose draw never flushed (orphans): same copy.
  bool TexRenameHasOrphans() const { return !rename_pending_list_.empty(); }
  bool TexRenameAnyPending() const;
  void TexRenameSettleOrphans(MTL::CommandBuffer* cmd);
  // [texture-precommit] whether each planned load could be encoded in the
  // pre-commit command buffer (see IsLoadPrecommitEligible); adds the loads
  // and eligible loads of the plan to *loads / *eligible. With trace set,
  // also counts the load categories (UploadTraceCounters tex_ld_*).
  // seen: textures already classified in this flush (deduplicated).
  struct TextureFlushClass {
    uint32_t loads = 0;
    uint32_t eligible = 0;
    // Ineligible loads by first blocking reason.
    uint32_t sampled_current = 0;
    // [tex-rename] of those, loads that would be eligible otherwise.
    uint32_t sampled_only = 0;
    // [tex-rename] loads whose texture was renamed for them.
    uint32_t renamed = 0;
    // [tex-rename] planned loads that will not write (not counted in loads).
    uint32_t noop = 0;
    uint32_t resident_touched = 0;
    uint32_t other = 0;
  };
  static bool TexRenameTraceEnabled();
  static void TexRenameTraceNote(const std::string& sig);
  void ClassifyMaterializationPlan(const TextureMaterializationPlan& plan,
                                   bool render_pass_open, bool trace,
                                   std::vector<const void*>& seen,
                                   TextureFlushClass& out);
  uint64_t loaded_texture_data_count() const {
    return loaded_texture_data_count_;
  }
  bool PrepareTextureMaterialization(const RegisterFile& regs,
                                     uint32_t used_texture_mask,
                                     TextureMaterializationPlan& plan);
  bool ExecuteTextureMaterialization(TextureMaterializationPlan& plan);
  void BeginTextureUploadBatch();
  bool EndTextureUploadBatch();
  bool PrepareTextureDataLoadRanges(Texture** textures, uint32_t texture_count,
                                    uint64_t base_outdated_mask,
                                    uint64_t mips_outdated_mask) override;
  bool RequestTextureDataRange(Texture& texture, TextureDataRangeSource source,
                               uint32_t start, uint32_t length) override;

  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  bool EnsureScaledResolveMemoryCommitted(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2 = 0) override;
  bool MakeScaledResolveRangeCurrent(uint32_t start_unscaled,
                                     uint32_t length_unscaled,
                                     uint32_t length_scaled_alignment_log2 = 0);
  bool GetCurrentScaledResolveBuffer(MTL::Buffer*& buffer_out,
                                     size_t& buffer_offset_out,
                                     size_t& buffer_length_out) const;
  // [res-scale texture-precommit] exact record of scaled-resolve memory
  // written in the open command buffer (resolves, direct host resolves and
  // buffer-grow copies). Never collapsed (bug-patterns row 26): a scaled
  // texture load may take the pre-commit route only when its scaled source
  // has no such write, since the pre-commit command buffer runs before the
  // open one. Committed writers finish before the pre-commit command buffer
  // starts (main_cb_event_ wait); standalone ones are waited on the CPU.
  void MarkScaledResolveWrite(uint32_t start_unscaled,
                              uint32_t length_unscaled);
  bool ScaledResolveRangeWrittenInOpenCommandBuffer(
      uint32_t start_unscaled, uint32_t length_unscaled) const;
  // FABLE2_SCALED_TEXTURE_PRECOMMIT=0 keeps scaled-resolve loads out of the
  // texture pre-commit route (default on; needs FABLE2_TEXTURE_PRECOMMIT).
  static bool ScaledTexturePrecommitEnabled();
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(
      xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(
      xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;
  bool FlushPendingUploadEncodersForCommandEncoderBoundary();
  // [upload-precommit] texture upload work that is encoded but not yet in a
  // committed position (deferred encoders/copies, an open upload-batch
  // command buffer). Shared-memory uploads must not be pre-committed ahead of
  // it, since it may read the pages being uploaded.
  // [texture-precommit] loads are encoded into `cmd` (the command
  // processor's pre-commit command buffer) while set; nullptr closes the
  // deferred encoders there and returns to the current command buffer.
  void SetPrecommitTarget(MTL::CommandBuffer* cmd);
  // FABLE2_TEXTURE_PRECOMMIT_VERIFY=1 (trace-only): for pre-commit loads
  // with a shared-memory source, the source bytes the pre-commit compute
  // read are compared with the same range copied in the current command
  // buffer at the load's original position ([texture-precommit-verify]).
  static bool PrecommitVerifyEnabled();
  bool HasPendingPrecommitVerify() const {
    return !precommit_verify_pending_.empty();
  }
  void EncodePrecommitVerifyReferences(MTL::CommandBuffer* cmd);
  bool CanUploadToPrecommit() const {
    return ShouldUploadViaBlit() && !HasPendingUploadWork();
  }
  bool HasPendingUploadWork() const {
    // Parked pre-commit work is not pending for these decisions: it lives in the pre-commit
    // command buffer and is closed before any other pre-commit encoder opens.
    return (!deferred_upload_parked_ && (deferred_upload_compute_encoder_ != nullptr ||
                                         !deferred_upload_copies_.empty())) ||
           upload_batch_command_buffer_ != nullptr;
  }
  // [tex-upload-merge] FABLE2_TEX_UPLOAD_MERGE (default on, =0 closes every batch's
  // encoders at the end of the batch): pre-commit texture flushes share one untile compute
  // encoder and one copy blit encoder per pre-commit command buffer when nothing else is
  // encoded into it in between. Off while FABLE2_TEXTURE_PRECOMMIT_VERIFY is on.
  static bool UploadMergeEnabled();
  // Close parked encoders (before another pre-commit encoder opens or the pre-commit
  // command buffer is committed).
  bool FlushParkedPrecommitUploads();

 private:
  bool EnsureViewBindlessHeadroom(uint32_t target_free_slots) const;
  enum class TextureLoadSourceMode {
    kResidentMemory,
    kCpuGuestMemory,
  };
  // GPU-based texture loading entry point. Returns true on success.
  bool TryGpuLoadTexture(Texture& texture, bool load_base, bool load_mips,
                         TextureLoadSourceMode source_mode =
                             TextureLoadSourceMode::kResidentMemory);
  bool LoadTextureDataFromCpuGuestMemory(Texture& texture, bool load_base,
                                         bool load_mips);
  MTL::StorageMode GetCacheTextureStorageMode() const;
  bool ShouldUploadViaBlit() const;
  void BeginUploadCommandBufferBatch();
  void EndUploadCommandBufferBatch();
  void AbortUploadCommandBufferBatch(bool commit_if_has_work = true);
  void BeginDeferredUploadEncoderBatch();
  bool EndDeferredUploadEncoderBatch();
  bool FlushDeferredUploadEncoderBatch();
  class UploadBatchScope;
  MTL::ComputeCommandEncoder* GetDeferredUploadComputeEncoder(
      MTL::CommandBuffer* command_buffer);
  void QueueDeferredUploadCopy(MTL::Buffer* source_buffer,
                               size_t source_offset_bytes,
                               size_t source_row_pitch,
                               size_t source_bytes_per_image,
                               MTL::Texture* destination_texture,
                               uint32_t destination_slice,
                               uint32_t destination_level, uint32_t width,
                               uint32_t height, uint32_t depth,
                               bool scaled = false);

  // Format / load shader mapping for Metal texture loading.
  bool IsDecompressionNeededForKey(TextureKey key) const;
  LoadShaderIndex GetLoadShaderIndexForKey(TextureKey key) const;
  MTL::PixelFormat GetPixelFormatForKey(TextureKey key) const;

  // Initialize GPU texture_load_* pipelines for Metal.
  bool InitializeLoadPipelines();

  struct Norm16Selection {
    bool unsigned_uses_float = false;
    bool signed_uses_float = false;
  };

  void InitializeNorm16Selection(MTL::Device* device);

  // Metal compute pipelines for texture_load_* shaders (unscaled and
  // resolution-scaled variants), indexed by TextureCache::LoadShaderIndex.
  MTL::ComputePipelineState* load_pipelines_[kLoadShaderCount] = {};
  MTL::ComputePipelineState* load_pipelines_scaled_[kLoadShaderCount] = {};
  // [tex-load-direct] scaled loads writing the host texture directly
  // (backends/metal/shaders/texture_load_dest_texture.metal).
  MTL::ComputePipelineState* load_pipelines_scaled_tex_[kLoadShaderCount] = {};
 public:
  // FABLE2_TEX_LOAD_DIRECT (default on): resolution-scaled texture loads write
  // the texture from the untile compute instead of a staging buffer + blit.
  static bool TexLoadDirectEnabled();

  // [resolve-to-texture] (docs/resolution-scale.md §16). Replaces the plain
  // MarkRangeAsResolved after a scaled copy resolve: classifies the cached
  // scaled textures whose base level the resolve writes (census), and, when
  // enabled, writes the resolve output straight into the matching ones.
  void MarkRangeAsResolvedRtt(const draw_util::ResolveInfo& resolve_info,
                              uint32_t written_address,
                              uint32_t written_length) {
    MarkRangeAsResolvedRttFused(resolve_info, written_address, written_length,
                                nullptr);
  }
  // FABLE2_RTT_DIAG: 0 off, 1 census (trace only), 2 skip (UNSAFE: matching
  // textures are marked up to date without being written; image wrong).
  static uint32_t RttDiagMode();
  // FABLE2_RESOLVE_TO_TEXTURE (master), FABLE2_RTT_KEEP (memexport keep),
  // FABLE2_RTT_FUSED (resolve writes the texture), FABLE2_RTT_VERIFY.
  static bool RttEnabled();
  static bool RttKeepEnabled();
  static bool RttFusedEnabled();
  static bool RttVerify();
  // [rtt-1x] FABLE2_RTT_1X: keep / fused / verify for unscaled (1x)
  // textures, whose loads read shared memory (docs/resolution-scale.md §20).
  static bool Rtt1xEnabled();
  static bool Rtt1xFusedEnabled();   // FABLE2_RTT_1X_FUSED
  static bool Rtt1xNarrowEnabled();  // FABLE2_RTT_1X_KEEP
  // [rtt-1x] an unscaled texture tracked at 1x (uncompressed tiled 2D).
  bool Rtt1xCandidateKey(const TextureKey& key) const;
  // Keep (and mips keep) in effect for the current scale.
  bool RttKeepActive() const;
  bool RttMipsKeepActive() const;
  void RttShutdown();

  // Fused resolve: textures written by the resolve itself (mirrors
  // XeRttParams in backends/metal/shaders/resolve_dest_texture.metal).
  struct RttTargetParams {
    int32_t origin_x, origin_y;
    uint32_t width, height, texel_bytes, endian, transform, level;
  };
  struct RttParams {
    RttTargetParams t[2];
    uint32_t count, dest_bpb_log2, skip_buffer, pad1;
  };
  struct RttFusedPlan {
    RttParams params = {};
    Texture* textures[2] = {};
    MTL::Texture* views[2] = {};
    uint64_t kb = 0;
    // [tex-resolve] guest range of the mip level a target rewrites (0 for
    // base-level targets).
    uint32_t level_start[2] = {}, level_end[2] = {};
    // [tex-resolve] lazy-buffer census: target 0 holds every pixel the
    // resolve writes and its load is invertible (plain or 16 bpb swizzle).
    bool lazy_ok = false;
  };
  // [tex-resolve] FABLE2_LAZY_CENSUS=1 (diagnostic): which scaled-buffer
  // bytes written by fused resolves are read again (texture load, morph
  // readback) before another resolve overwrites them.
  static bool LazyCensusEnabled();
  void LazyCensusWrite(uint32_t start, uint32_t length, bool candidate);
  void LazyCensusRead(uint32_t start, uint32_t length, bool morph);
  // Picks up to two cached scaled textures this resolve can write directly.
  // Census (FABLE2_RTT_DIAG=census): a planned fused write was dropped
  // because a source shader has no fused variant.
  void RttNoteFusedPipelineMissing() {
    if (RttDiagMode() == 1) {
      auto global_lock = AcquireGlobalLock();
      rtt_census_sigs_["fused-reject no-pipeline"]++;
    }
  }
  bool RttPrepareFused(const draw_util::ResolveInfo& resolve_info,
                       RttFusedPlan& plan);
  // [tile-resolve] Draw-binding serials: a texture bound by a draw prepared
  // after the last render-encoder end may be sampled by the open pass, so a
  // tile resolve in that pass must not write it (cross-tile write-after-read).
  // Prepared draws are flushed at every encoder end, so a texture whose last
  // bind is not later than that end is not sampled by the open pass.
  void TileResolveNoteEncoderEnd() {
    tres_encoder_end_serial_ = tres_bind_serial_counter_;
    ++miplod_epoch_;
  }
  // texture: a RttFusedPlan::textures entry.
  bool TileResolveTextureIdleInOpenPass(const void* texture) const {
    const auto* t = static_cast<const MetalTexture*>(
        static_cast<const Texture*>(texture));
    return !t || t->tres_bind_serial_ <= tres_encoder_end_serial_;
  }
  // [mip-lod] FABLE2_MIPLOD_TRACE=1 (diagnostic): remembers, per mipped
  // texture, the last pixel-shader fetch of it (fetch constant dwords and the
  // fetch instruction's LOD attributes) and logs it when a tile resolve into
  // one of its levels falls back on the in-pass hazard.
  static bool MipLodTraceEnabled();
  void MipLodTraceNoteDraw(const Shader& pixel_shader, uint64_t vs_hash,
                           uint32_t prim);
  void MipLodTraceHazard(const void* fused_texture, uint32_t level,
                         const char* what);
  // The fused variant of a scaled host resolve pipeline (null if none).
  MTL::ComputePipelineState* RttFusedPipeline(
      MTL::ComputePipelineState* pipeline);
  // [rtt-prewarm] Builds the fused resolve pipelines on two background
  // threads at startup (FABLE2_RTT_PREWARM=0: off); RttFusedPipeline takes
  // a prebuilt one by name instead of compiling on the draw path.
  void StartRttPrewarm();
  // [shader-prep] fused resolve pipelines the prewarm has not built yet.
  uint32_t RttPrewarmPending();
  // After the fused dispatch, instead of MarkRangeAsResolvedRtt.
  // [effects-native] an unscaled resolve while scaling is on: the scaled
  // textures over the range are invalidated like by any resolve.
  void MarkRangeAsResolvedUnscaledRtt(uint32_t written_address,
                                      uint32_t written_length);
  void MarkRangeAsResolvedRttFused(const draw_util::ResolveInfo& resolve_info,
                                   uint32_t written_address,
                                   uint32_t written_length,
                                   const RttFusedPlan* plan);
 private:
  std::unordered_map<MTL::ComputePipelineState*, MTL::ComputePipelineState*>
      rtt_fused_pipelines_;
  uint64_t tres_bind_serial_counter_ = 0;
  uint64_t tres_encoder_end_serial_ = 0;
  uint64_t miplod_epoch_ = 1;
  struct MipLodTraceEntry {
    uint32_t fetch[6];
    uint64_t ps_hash, vs_hash;
    uint32_t prim;
    uint32_t fetch_constant;
    bool computed_lod, register_lod, register_gradients;
    float instr_lod_bias;
    uint32_t instr_mip_filter;
  };
  std::unordered_map<const void*, MipLodTraceEntry> miplod_trace_;
  struct RttPrewarm;
  std::shared_ptr<RttPrewarm> rtt_prewarm_;
  MTL::ComputePipelineState* texture_upload_repack_pipeline_ = nullptr;
  // Number of LoadTextureDataFromResidentMemoryImpl calls. Used only to derive
  // the per-request load count reported to command-processor telemetry.
  uint64_t loaded_texture_data_count_ = 0;
  uint64_t load_hint_race_loads_ = 0;  // [draw-prep-lite2]
  uint64_t load_hint_used_ = 0, load_hint_rechecks_ = 0;
  uint64_t load_hint_race_loads_reused_ = 0, load_hint_verify_mismatches_ = 0;
  // [load-check-exact] FABLE2_LOAD_CHECK_TRACE=1: set while a request runs after a
  // "no load" check; a texture load in that window is logged with its call path.
  bool load_check_expect_none_ = false;
  uint32_t load_check_trace_logged_ = 0;
  void NoteLoadWhileExpectNone(const Texture& texture, const char* path);
  static bool LoadCheckTraceEnabled();
  // [load-check-exact] FABLE2_LOAD_CHECK_EXACT (default on): a request after a
  // "no load" check loads nothing the check could not see (TextureCache::
  // request_defer_after_epoch_).
  static bool LoadCheckExactEnabled();
  static uint64_t LoadCheckDeferEpoch(uint64_t check_epoch);
  void LogLoadCheckSummary(const char* when);
  // [load-check-exact] FABLE2_LOAD_CHECK_EXACT_VERIFY=1: every deferred texture
  // is tracked until a later request finds it loaded; a later draw that uses it
  // while it is still outdated, with no newer CPU write and no planned load, is
  // a violation (counted and logged).
  static bool LoadCheckExactVerifyEnabled();
  void LoadCheckExactVerifyBegin();
  void LoadCheckExactVerifyAfterRequest(uint32_t used_texture_mask,
                                        const TextureMaterializationPlan* plan);
  void LoadCheckExactForget(const Texture* texture) {
    exact_deferred_.erase(texture);
  }
  struct ExactDeferred {
    uint64_t serial = 0;
    uint64_t frame = 0;
    bool gpu_touched = false;
  };
  std::unordered_map<const Texture*, ExactDeferred> exact_deferred_;
  uint64_t exact_verify_serial_ = 0, exact_verify_epoch_ = 0;
  uint64_t exact_verify_frame_ = 0;
  uint64_t exact_verify_loaded_later_ = 0, exact_verify_planned_ = 0;
  uint64_t exact_verify_redeferred_ = 0, exact_verify_violations_ = 0;
  uint64_t exact_verify_gpu_touched_ = 0, exact_verify_frames_crossed_ = 0;
  uint64_t exact_verify_max_pending_ = 0;

  // Metal-specific Texture implementation

  class MetalTexture : public Texture {
   public:
    MetalTexture(MetalTextureCache& texture_cache, const TextureKey& key,
                 MTL::Texture* metal_texture, bool track_usage = true,
                 bool is_3d_as_2d_wrapper = false);
    ~MetalTexture() override;

    MTL::Texture* metal_texture() const { return metal_texture_; }
    MTL::Texture* GetOrCreateView(uint32_t host_swizzle,
                                  xenos::FetchOpDimension dimension,
                                  bool is_signed, uint32_t view_levels = 0);
    MTL::Texture* GetOrCreate3DAs2DView(uint32_t host_swizzle,
                                        xenos::FetchOpDimension dimension,
                                        bool is_signed);
    uint32_t GetOrCreateBindlessSRVIndexAndView(
        uint32_t host_swizzle, xenos::FetchOpDimension dimension,
        bool is_signed, MTL::Texture** view_out, uint32_t view_levels = 0);

    // [texture-precommit] set after the first successful data load; set when
    // the texture is handed to the presenter (never pre-committed then).
    bool loaded_once() const { return loaded_once_; }
    void set_loaded_once() { loaded_once_ = true; }
    bool presented() const { return presented_; }
    void set_presented() { presented_ = true; }
    // [tex-load-direct] unsigned-integer 2D-array view with the host texel
    // size (R8Uint ... RGBA32Uint) for raw compute writes; null if the texture
    // was not created writable or the view is not supported.
    MTL::Texture* GetOrCreateDirectWriteView(uint32_t texel_bytes);
    // [tex-rename] swaps the backing texture; the old one, its views and
    // bindless slots are handed back (slots retired via the command
    // processor). Returns the old backing (owned by the caller).
    MTL::Texture* RenameBacking(MTL::Texture* new_texture,
                                std::vector<MTL::Texture*>& old_views_out);
    // [tex-rename verify] temporarily exchange the backing and direct-write
    // view with another texture (no slot changes).
    void SwapBackingForVerify(MTL::Texture*& texture,
                              MTL::Texture*& direct_view);
    bool has_3d_as_2d() const {
      return texture_3d_as_2d_ != nullptr || is_3d_as_2d_wrapper_;
    }
    // [tex-rename] renamed and not yet written by its load; rename_src_ is
    // the old backing (retained) until settled.
    bool rename_pending_ = false;
    MTL::Texture* rename_src_ = nullptr;
    // [tex-rename nx-diag] unscaled texture at scale > 1 whose contents
    // still equal shared memory over its exact range (diagnostic only).
    bool nx_tracked_ = false;
    bool nx_valid_ = false;

   private:
    bool loaded_once_ = false;
    bool presented_ = false;
    MTL::Texture* direct_write_view_ = nullptr;
    uint32_t direct_write_view_texel_bytes_ = 0;
    bool direct_write_view_failed_ = false;
    uint64_t GetViewKey(uint32_t host_swizzle,
                        xenos::FetchOpDimension dimension, bool is_signed,
                        MTL::PixelFormat view_format,
                        uint32_t view_levels = 0) const;
    MTL::PixelFormat GetViewPixelFormat(bool is_signed) const;
    MTL::TextureType GetViewType(xenos::FetchOpDimension dimension) const;
    uint32_t GetOrCreateBindlessSRVIndexForResolvedView(uint64_t view_key,
                                                        MTL::Texture* view);

    MetalTextureCache& texture_cache_;
    MTL::Texture* metal_texture_;
    // [draw-prep-lite2] properties of metal_texture_ (set in the constructor and
    // refreshed where tex-rename swaps the backing), read on every view / bindless
    // lookup instead of three Objective-C messages.
    bool mtl_props_cached_ = false;
    NS::UInteger mtl_level_count_ = 0;
    MTL::PixelFormat mtl_pixel_format_ = MTL::PixelFormatInvalid;
    MTL::TextureType mtl_texture_type_ = MTL::TextureType2D;
    void RefreshMtlProps() {
      if (!mtl_props_cached_) return;
      if (!metal_texture_) {
        mtl_props_cached_ = false;
        return;
      }
      mtl_level_count_ = metal_texture_->mipmapLevelCount();
      mtl_pixel_format_ = metal_texture_->pixelFormat();
      mtl_texture_type_ = metal_texture_->textureType();
    }
    NS::UInteger MtlLevelCount() const {
      return mtl_props_cached_ ? mtl_level_count_
                               : metal_texture_->mipmapLevelCount();
    }
    MTL::PixelFormat MtlPixelFormat() const {
      return mtl_props_cached_ ? mtl_pixel_format_
                               : metal_texture_->pixelFormat();
    }
    MTL::TextureType MtlTextureType() const {
      return mtl_props_cached_ ? mtl_texture_type_
                               : metal_texture_->textureType();
    }
    uint32_t bindless_srv_index_ = UINT32_MAX;
    std::unique_ptr<MetalTexture> texture_3d_as_2d_;
    bool is_3d_as_2d_wrapper_ = false;
    uint64_t mem_report_bytes_ = 0;  // [rt-shrink] counted in mem_tex_bytes_
   public:
    // [resolve-to-texture] class of the last resolve that wrote this texture's
    // base level since its last load (RttClass), for the load census.
    uint8_t rtt_class_ = 0;
    // [resolve-to-texture] the base level holds exactly what a load from the
    // scaled-resolve buffer would write at this point in GPU order (set by a
    // load or a fused resolve write; cleared by a resolve or CPU write over
    // its pages; memexport writes leave the scaled buffer, so keep it).
    bool rtt_valid_ = false;
    // A fused write not consumed yet (no keep since): counts written-but-
    // unsampled bytes (tr_n_rtt_waste_kb).
    bool rtt_fused_pending_ = false;
    // [tex-resolve] the same for all stored mip levels (FABLE2_RTT_MIPS):
    // set by a mips load; cleared by a write over the mips range other than
    // the fused resolve that rewrites one whole level.
    bool rtt_mips_valid_ = false;
    // [rtt-1x] registered for resolve-to-texture tracking: a scaled-resolve
    // texture at scale > 1, or (FABLE2_RTT_1X) an uncompressed tiled 2D
    // texture at 1x, whose loads read shared memory.
    bool rtt_tracked_ = false;
    // [tile-resolve] TileResolveBindSerial() when a draw last bound this
    // texture (any view). A tile resolve may write it as a fused target only
    // if no draw of the open render pass samples it.
    uint64_t tres_bind_serial_ = 0;
    // [mip-lod] a fused resolve writes one of its stored mip levels; draws
    // sampling it are bounded (metal_miplod_proof.h).
    bool miplod_candidate_ = false;
    // Open-pass sampling: epoch (MetalTextureCache::miplod_epoch()), highest
    // level read by a bounded draw, and whether an unbounded draw sampled it.
    uint64_t miplod_epoch_ = 0;
    int32_t miplod_max_read_ = -1;
    bool miplod_unbounded_ = false;
   private:
    std::unordered_map<uint64_t, MTL::Texture*> swizzled_view_cache_;
    std::unordered_map<uint64_t, uint32_t> swizzled_view_bindless_srv_indices_;
  };

 private:
  // Metal texture creation helpers
  MTL::Texture* CreateTexture(MTL::TextureDescriptor* descriptor);
  MTL::Texture* CreateTexture2D(uint32_t width, uint32_t height,
                                uint32_t array_length, MTL::PixelFormat format,
                                MTL::TextureSwizzleChannels swizzle,
                                uint32_t mip_levels = 1,
                                bool shader_write = false);
  MTL::Texture* CreateTexture3D(uint32_t width, uint32_t height, uint32_t depth,
                                MTL::PixelFormat format,
                                MTL::TextureSwizzleChannels swizzle,
                                uint32_t mip_levels = 1);
  MTL::Texture* CreateTextureCube(uint32_t width, MTL::PixelFormat format,
                                  MTL::TextureSwizzleChannels swizzle,
                                  uint32_t mip_levels = 1,
                                  uint32_t cube_count = 1);
  struct ScaledResolveBuffer {
    MTL::Buffer* buffer = nullptr;
    uint64_t base_scaled = 0;
    uint64_t length_scaled = 0;
  };
  struct RetiredScaledResolveBuffer {
    MTL::Buffer* buffer = nullptr;
    uint64_t submission_id = 0;
    uint64_t length_scaled = 0;
  };
  struct RetiredSamplerState {
    MTL::SamplerState* sampler = nullptr;
    uint64_t submission_id = 0;
  };

  bool GetScaledResolveRange(uint32_t start_unscaled, uint32_t length_unscaled,
                             uint32_t length_scaled_alignment_log2,
                             uint64_t& start_scaled_out,
                             uint64_t& length_scaled_out) const;
  bool IsScaledResolveRangeResident(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2) const;
  bool EnsureScaledResolveBufferRange(uint64_t start_scaled,
                                      uint64_t length_scaled);
  void MarkScaledResolveWriteScaled(uint64_t start_scaled,
                                    uint64_t length_scaled);
  // Scaled [start, end) ranges written in submission
  // scaled_open_writes_submission_ (see MarkScaledResolveWrite).
  std::vector<std::pair<uint64_t, uint64_t>> scaled_open_writes_;
  uint64_t scaled_open_writes_submission_ = 0;
  void ClearScaledResolveBuffers();
  void ReleaseOrRetireSamplerState(MTL::SamplerState* sampler);

  // Null texture factory methods (following existing CreateTexture pattern)
  MTL::Texture* CreateNullTexture2D();
  MTL::Texture* CreateNullTexture3D();
  MTL::Texture* CreateNullTextureCube();

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  MetalCommandProcessor* command_processor_;

  // Pre-created null textures for invalid bindings (following existing
  // patterns)
  MTL::Texture* null_texture_2d_ = nullptr;
  MTL::Texture* null_texture_3d_ = nullptr;
  MTL::Texture* null_texture_cube_ = nullptr;

  // Persistent bindless heap indices for null textures/samplers.
  // Allocated during Initialize, released during Shutdown.
  uint32_t null_texture_2d_bindless_index_ = UINT32_MAX;
  uint32_t null_texture_3d_bindless_index_ = UINT32_MAX;
  uint32_t null_texture_cube_bindless_index_ = UINT32_MAX;
  uint32_t null_sampler_bindless_index_ = UINT32_MAX;
  MTL::SamplerState* null_sampler_bindless_ = nullptr;

  Norm16Selection r16_selection_;
  Norm16Selection rg16_selection_;
  Norm16Selection rgba16_selection_;

  std::unordered_map<uint32_t, MTL::SamplerState*> sampler_cache_;
  // Maps sampler parameter key -> persistent bindless heap index.
  std::unordered_map<uint32_t, uint32_t> sampler_bindless_indices_;

  class UploadBufferPool;
  struct DeferredUploadCopy {
    MTL::Buffer* source_buffer = nullptr;
    size_t source_offset_bytes = 0;
    size_t source_row_pitch = 0;
    size_t source_bytes_per_image = 0;
    MTL::Texture* destination_texture = nullptr;
    uint32_t destination_slice = 0;
    uint32_t destination_level = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    bool retained = false;  // [tex-upload-merge] destination retained until encoded
    bool scaled = false;    // [tex-blit] destination is a resolution-scaled resolve texture
  };
  // [tex-blit] counts one buffer->texture upload copy (tr_n_tex_blit_*) and
  // returns how many times to encode it: 1 normally; 0 or 1+N under the
  // FABLE2_TEX_BLIT_DIAG=skip|dup<N> diagnostic (scaled destinations only).
  static uint32_t TexBlitEncodeCount(size_t bytes, bool scaled);

  mutable std::mutex upload_buffer_pool_mutex_;
  std::shared_ptr<UploadBufferPool> upload_buffer_pool_;
  MTL::CommandBuffer* upload_batch_command_buffer_ = nullptr;
  bool upload_batch_command_buffer_has_work_ = false;
  std::vector<std::pair<uint32_t, uint32_t>>
      upload_batch_command_buffer_shared_memory_ranges_;
  uint32_t upload_batch_depth_ = 0;
  MTL::CommandBuffer* deferred_upload_command_buffer_ = nullptr;
  MTL::ComputeCommandEncoder* deferred_upload_compute_encoder_ = nullptr;
  std::vector<DeferredUploadCopy> deferred_upload_copies_;
  // [tex-load-direct] FABLE2_TEX_LOAD_DIRECT_VERIFY=1 (diagnostic): each
  // direct load also runs the staging-buffer shader into `ref`; the deferred
  // blit encoder copies the written texture level into `readback`, and the
  // command buffer's completion compares the two byte for byte.
  struct DirectLoadVerify {
    MTL::Buffer* ref = nullptr;       // shared by the levels of one load
    MTL::Buffer* readback = nullptr;  // one per level/slice
    MTL::Texture* texture = nullptr;  // retained
    size_t ref_offset = 0;
    uint32_t level = 0, slice = 0, width = 0, height = 0, texel_bytes = 0,
             pitch = 0, format = 0;
    bool rtt = false;  // [resolve-to-texture] verify of a kept texture
    uint32_t base = 0;
  };
  std::vector<DirectLoadVerify> direct_load_verify_;
  static bool TexLoadDirectVerify();
  uint32_t deferred_upload_batch_depth_ = 0;
  MTL::CommandBuffer* precommit_target_cb_ = nullptr;
  struct PrecommitVerifySample {
    MTL::Buffer* precommit_copy = nullptr;
    // Scaled-resolve source (retained), or nullptr for shared memory; the
    // copied range is [source_offset, source_offset + length) in it.
    MTL::Buffer* source = nullptr;
    uint64_t source_offset = 0;
    uint32_t start = 0;
    uint32_t length = 0;
    uint64_t submission = 0;
    uint32_t reasons = 0;
    uint32_t reference_reasons = 0;
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  std::vector<PrecommitVerifySample> precommit_verify_pending_;
  bool deferred_upload_on_precommit_ = false;
  // [tex-upload-merge] the deferred encoders of a finished batch were left open in the
  // pre-commit command buffer for the next pre-commit texture flush to append to.
  bool deferred_upload_parked_ = false;
  std::unique_ptr<MetalHeapPool> texture_heap_pool_;
  bool supports_bc_texture_compression_ = false;

  std::vector<ScaledResolveBuffer> scaled_resolve_buffers_;
  std::vector<RetiredScaledResolveBuffer> scaled_resolve_retired_buffers_;
  std::vector<RetiredSamplerState> retired_sampler_states_;
  uint64_t scaled_resolve_retired_bytes_ = 0;
  size_t scaled_resolve_current_buffer_index_ = size_t(-1);
  uint64_t scaled_resolve_current_range_start_scaled_ = 0;
  uint64_t scaled_resolve_current_range_length_scaled_ = 0;

  // [resolve-to-texture] all cached scaled-resolve textures.
  std::vector<MetalTexture*> rtt_textures_;
  struct RttCandidate {
    MetalTexture* texture = nullptr;
    uint8_t cls = 0;
    bool full = false;
    uint32_t bx = 0, by = 0;
    uint32_t load_shader = 0;
    // [tex-resolve] cls == kRttClassMipLevel: the stored mip level the
    // resolve rewrites completely, and that level's guest byte range.
    uint32_t level = 0;
    uint32_t level_start = 0, level_end = 0;
  };
  // [tex-resolve] RttCollect class for a resolve that fully rewrites one
  // stored (non-packed) mip level with a plain or 16-bit-conversion load.
  static constexpr uint8_t kRttClassMipLevel = 6;
  static bool RttMipsEnabled();
  static bool Rtt16Enabled();
  void RttCollect(const draw_util::ResolveInfo& resolve_info,
                  uint32_t written_address, uint32_t written_length,
                  std::vector<RttCandidate>& out);
  void RttRegister(MetalTexture* texture);
  void RttUnregister(MetalTexture* texture);
  void RttCensusLoad(MetalTexture& texture, uint64_t scaled_bytes);
  void RttCensusTick();
  void RttTraceLoadResult(Texture& texture, bool load_base, bool load_mips,
                          bool ok);
  // Returns true when the load was handled here (kept or verified).
  bool RttTryKeep(MetalTexture& texture, bool load_base, bool load_mips);
  static void RttGlobalWatchThunk(const global_unique_lock_type& global_lock,
                                  void* context, uint32_t address_first,
                                  uint32_t address_last,
                                  bool invalidated_by_gpu);
  void RttNoteFusedUnused(MetalTexture& texture);
  void RttOnInvalidate(uint32_t address_first, uint32_t address_last,
                       bool invalidated_by_gpu);
  SharedMemory::GlobalWatchHandle rtt_global_watch_ = nullptr;
  bool rtt_in_resolve_ = false;
  // Exact guest byte range of the resolve being marked (watches fire for
  // whole host pages, 16 KB, so neighbours sharing a page get notified too).
  uint32_t rtt_resolve_first_ = 0, rtt_resolve_last_ = 0;
  // [tex-resolve] during a fused resolve: textures whose mip level the
  // dispatch rewrote completely, with that level's guest range; their mips
  // validity survives this resolve's own invalidation.
  struct RttMipExempt {
    MetalTexture* texture = nullptr;
    uint32_t start = 0, end = 0;
  };
  RttMipExempt rtt_mip_exempt_[2];
  std::map<uint32_t, uint32_t> lazy_census_ranges_;  // start -> end
  // TryGpuLoadTexture: compute the reference load and compare the texture's
  // current content with it instead of writing the texture (RTT verify).
  bool rtt_verify_only_ = false;
  // [tex-rename]
  bool TexRenameTry(MetalTexture& texture, bool load_base, bool load_mips,
                    bool use_cpu_source);
  // Load blockers other than "sampled by the open command buffer"
  // (pre-commit eligibility); ignore_invalid: pages not uploaded yet do not
  // count (the flush uploads them before classifying).
  bool LoadOtherwiseEligible(MetalTexture& texture, bool load_base,
                             bool load_mips, bool use_cpu_source,
                             bool ignore_invalid);
  void TexRenameClearPending(MetalTexture& texture, bool load_base,
                             bool load_mips);
  void TexRenameSettleOne(MetalTexture& texture, MTL::CommandBuffer* cmd,
                          MTL::BlitCommandEncoder*& blit);
  void TexRenameRetire(MTL::Texture* texture,
                       std::vector<MTL::Texture*>&& views);
  void TexRenameCompleted(uint64_t completed_submission);
  // A MetalTexture being destroyed leaves the pending list.
  void TexRenameForget(MetalTexture* texture);
  MTL::Texture* TexRenameAcquire(MTL::Texture* like);
  static uint64_t TexRenameBytes(MTL::Texture* t);
  struct RenameRetired {
    MTL::Texture* texture = nullptr;
    std::vector<MTL::Texture*> views;
    uint64_t submission = 0;
    uint64_t bytes = 0;
  };
  struct RenamePoolEntry {
    MTL::Texture* texture = nullptr;
    uint64_t bytes = 0;
    uint64_t freed_submission = 0;
  };
  std::vector<RenameRetired> rename_retired_;
  std::vector<RenamePoolEntry> rename_pool_;
  std::vector<MetalTexture*> rename_pending_list_;
  uint64_t rename_retired_bytes_ = 0, rename_pool_bytes_ = 0;
  uint32_t tex_rename_last_reasons_ = 0;  // trace
  // [tex-rename] FABLE2_TEX_NX_DIAG=1: how many loads of unscaled textures at
  // scale > 1 reload bytes no write has touched since their last load.
  static bool NxDiagAllEnabled();
  // Starts exact-range tracking for an unscaled texture at scale > 1. With
  // FABLE2_TEX_UNCHANGED only textures whose reload was a rename candidate
  // (reloaded while sampled by the open command buffer) are tracked, so the
  // per-invalidation scan stays short.
  void NxTrack(MetalTexture& texture);
  static bool NxKeepEnabled();
  static bool NxVerify();
  std::vector<MetalTexture*> nx_textures_;
  // [tex-rename] the planned load will not write the texture (it is up to
  // date again, or RTT keeps it whole). Should that change before it runs,
  // TryGpuLoadTexture's guard ends the pre-commit route first.
  bool PlannedLoadIsNoop(MetalTexture& texture, bool load_base,
                         bool load_mips);
  std::unordered_map<std::string, uint64_t> rtt_census_sigs_;
  uint64_t rtt_census_last_log_us_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_
