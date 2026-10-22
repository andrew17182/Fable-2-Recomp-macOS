/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_RENDER_TARGET_CACHE_H_
#define XENIA_GPU_METAL_METAL_RENDER_TARGET_CACHE_H_

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>

#include "rex/graphics/util/draw.h"
#include "rex/graphics/register_file.h"
#include "rex/graphics/pipeline/render_target/cache.h"
#include "rex/graphics/trace_writer.h"
#include "rex/graphics/xenos.h"
#include "rex/memory.h"

#include "third_party/metal-cpp/Metal/Metal.hpp"

struct IRDescriptorTableEntry;

namespace rex {
namespace graphics {
namespace metal {

class MetalCommandProcessor;
class MetalHeapPool;

class MetalRenderTargetCache final : public graphics::RenderTargetCache {
 public:
  // Metal-specific render target - defined inside cache class to access
  // protected RenderTarget
  class MetalRenderTarget final : public RenderTarget {
   public:
    ~MetalRenderTarget() override;

    MTL::Texture* texture() const { return texture_; }
    MTL::Texture* msaa_texture() const { return msaa_texture_; }
    MTL::Texture* draw_texture() const {
      return draw_texture_ ? draw_texture_ : texture_;
    }
    MTL::Texture* transfer_texture() const {
      return transfer_texture_ ? transfer_texture_ : texture_;
    }
    MTL::Texture* msaa_draw_texture() const {
      return msaa_draw_texture_ ? msaa_draw_texture_ : msaa_texture_;
    }
    MTL::Texture* msaa_transfer_texture() const {
      return msaa_transfer_texture_ ? msaa_transfer_texture_ : msaa_texture_;
    }
    MTL::Texture* stencil_view() const { return stencil_view_; }
    void SetStencilView(MTL::Texture* view) { stencil_view_ = view; }

    void SetTemporarySortIndex(uint32_t index) {
      temporary_sort_index_ = index;
    }
    uint32_t temporary_sort_index() const { return temporary_sort_index_; }

    void SetTexture(MTL::Texture* texture) {
      if (texture_ != texture) {
        if (stencil_view_) {
          stencil_view_->release();
          stencil_view_ = nullptr;
        }
        texture_ = texture;
      }
    }
    void SetMsaaTexture(MTL::Texture* texture) { msaa_texture_ = texture; }
    void SetDrawTexture(MTL::Texture* texture) { draw_texture_ = texture; }
    void SetTransferTexture(MTL::Texture* texture) {
      transfer_texture_ = texture;
    }
    void SetMsaaDrawTexture(MTL::Texture* texture) {
      msaa_draw_texture_ = texture;
    }
    void SetMsaaTransferTexture(MTL::Texture* texture) {
      msaa_transfer_texture_ = texture;
    }
    bool needs_initial_clear() const { return needs_initial_clear_; }
    void SetNeedsInitialClear(bool needs_initial_clear) {
      needs_initial_clear_ = needs_initial_clear;
    }

    // [rt-shrink] FABLE2_RT_SHRINK (docs/resolution-scale.md section 21):
    // the host texture covers only the rows the game has addressed so far.
    // full_height: the EDRAM-height texture height the key would get without
    // the shrink. rows_needed: rows addressed so far (ownership claims, draw
    // scissors, render-pass partners), <= full_height. initial_clear_done: the
    // first bind cleared the whole texture (rows added later get the same
    // clear value).
    uint32_t full_height() const { return full_height_; }
    void set_full_height(uint32_t h) { full_height_ = h; }
    uint32_t rows_needed() const { return rows_needed_; }
    void set_rows_needed(uint32_t r) { rows_needed_ = r; }
    bool rts_pending() const { return rts_pending_; }
    void set_rts_pending(bool p) { rts_pending_ = p; }
    bool initial_clear_done() const { return initial_clear_done_; }
    void set_initial_clear_done(bool d) { initial_clear_done_ = d; }
    uint32_t grow_count() const { return grow_count_; }
    void note_grow() { ++grow_count_; }
    // Releases the texture and its views (before installing a taller one).
    void ReleaseTexturesForReplace() {
      if (stencil_view_) {
        stencil_view_->release();
        stencil_view_ = nullptr;
      }
      if (draw_texture_ && draw_texture_ != texture_) {
        draw_texture_->release();
      }
      draw_texture_ = nullptr;
      if (transfer_texture_ && transfer_texture_ != texture_) {
        transfer_texture_->release();
      }
      transfer_texture_ = nullptr;
      if (texture_) {
        texture_->release();
        texture_ = nullptr;
      }
    }

    // Public constructor for creating render targets
    MetalRenderTarget(RenderTargetKey key) : RenderTarget(key) {}

   private:
    MTL::Texture* texture_ = nullptr;
    MTL::Texture* msaa_texture_ = nullptr;  // If MSAA is enabled
    MTL::Texture* draw_texture_ = nullptr;
    MTL::Texture* transfer_texture_ = nullptr;
    MTL::Texture* msaa_draw_texture_ = nullptr;
    MTL::Texture* msaa_transfer_texture_ = nullptr;
    MTL::Texture* stencil_view_ = nullptr;
    uint32_t temporary_sort_index_ = UINT32_MAX;
    bool needs_initial_clear_ = true;
    uint32_t full_height_ = 0;
    uint32_t rows_needed_ = 0;
    uint32_t grow_count_ = 0;
    bool rts_pending_ = false;
    bool initial_clear_done_ = false;
  };

 public:
  MetalRenderTargetCache(const RegisterFile& register_file,
                         const rex::memory::Memory& memory, TraceWriter* trace_writer,
                         uint32_t draw_resolution_scale_x,
                         uint32_t draw_resolution_scale_y,
                         MetalCommandProcessor& command_processor);
  ~MetalRenderTargetCache() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  // [xfer-prewarm] FABLE2_XFER_PREWARM (docs/frame-pacing-20261012.md): build the
  // ownership-transfer pipelines of earlier sessions (keys in
  // <title root>/<title>.xfer-keys.txt) on background threads, before the draws
  // need them. Called once the shader storage folder is known.
  // [tile-resolve prewarm] builds the tile resolve pipelines this cache has
  // used before (<title>.tres-keys.txt) plus built-in defaults on the tile
  // pipeline thread at startup (FABLE2_TILE_RESOLVE_PREWARM=0: off).
  void StartTileResolvePrewarm(const std::filesystem::path& title_root,
                               uint32_t title_id);
  void StartTransferPrewarm(const std::filesystem::path& title_root,
                            uint32_t title_id);
  // [shader-prep] FABLE2_SHADER_PREP: prewarm work still queued or building.
  uint32_t XferPrewarmPending();
  uint32_t TileResolvePrewarmPending();

  // RenderTargetCache implementation
  Path GetPath() const override;

  // Fixed-point render targets (k_16_16 / k_16_16_16_16) are backed by *_SNORM
  // formats in the host render targets path, which are -1...1 rather than the
  // Xbox 360's -32...32 range. When this is true, resolve/copy must compensate
  // to match the guest packing expectations.
  bool IsFixedRG16TruncatedToMinus1To1() const {
    return !REXCVAR_GET(snorm16_render_target_full_range);
  }
  bool IsFixedRGBA16TruncatedToMinus1To1() const {
    return !REXCVAR_GET(snorm16_render_target_full_range);
  }

  // Whether 2x MSAA is supported on this device.
  bool msaa_2x_supported() const { return msaa_2x_supported_; }

  // Whether gamma render targets use UNORM16 storage (separate from sRGB).
  // When true, gamma correction is done in shaders rather than via sRGB format.
  bool gamma_render_target_as_unorm16() const {
    return gamma_render_target_as_unorm16_;
  }

  bool IsGammaFormatHostStorageSeparate() const override;

  // Check if the render target key uses a 64bpp format.
  bool IsKey64bpp(RenderTargetKey key) const;

  void ClearCache() override;
  void BeginFrame() override;

  bool Update(bool is_rasterization_done,
              reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask,
              const Shader& vertex_shader) override;

  // Metal-specific methods
  MTL::RenderPassDescriptor* GetRenderPassDescriptor(
      uint32_t expected_sample_count = 1,
      bool fallback_depth_attachment_required = false);
  bool IsRenderPassDescriptorCompatible(
      MTL::RenderPassDescriptor* pass_descriptor,
      uint32_t expected_sample_count = 1,
      bool fallback_depth_attachment_required = false) const;
  // Also true while a [deferred-clears] resolve clear waits to be drawn
  // first inside the next draw pass.
  bool HasPendingDrawPassTransfers() const {
    return pending_draw_pass_transfer_mask_ != 0 ||
           pending_draw_pass_clear_mask_ != 0;
  }

  // [raster-state] FABLE2_TRANSFER_QUAD_STATE_RESET (default on).
  static bool TransferQuadStateResetEnabled();
  static void ResetDrawPassRasterizerState(MTL::RenderCommandEncoder* encoder);
  // Ownership-transfer quads (not [deferred-clears] clears) wait to be drawn
  // in the next draw pass.
  bool HasPendingDrawPassTransferQuads() const {
    return pending_draw_pass_transfer_mask_ != 0;
  }

  enum DrawPassTransferEncoderMutation : uint32_t {
    kDrawPassTransferEncoderMutationNone = 0,
    kDrawPassTransferEncoderMutationPipeline = 1u << 0,
    kDrawPassTransferEncoderMutationDepthStencil = 1u << 1,
    kDrawPassTransferEncoderMutationStencilReference = 1u << 2,
    kDrawPassTransferEncoderMutationViewport = 1u << 3,
    kDrawPassTransferEncoderMutationScissor = 1u << 4,
    kDrawPassTransferEncoderMutationVertexSlot0 = 1u << 5,
    kDrawPassTransferEncoderMutationVertexSlot1 = 1u << 6,
    kDrawPassTransferEncoderMutationFragmentSlot0 = 1u << 7,
    kDrawPassTransferEncoderMutationFragmentSlot1 = 1u << 8,
    kDrawPassTransferEncoderMutationFragmentTextures = 1u << 9,
    // Cull mode, winding, fill mode, depth bias and depth clip mode were reset
    // to the encoder defaults ([deferred-clears] clear quad or [raster-state]
    // transfer quads in a draw pass).
    kDrawPassTransferEncoderMutationRasterizer = 1u << 10,
  };
  using DrawPassTransferEncoderMutationMask = uint32_t;
  bool EncodePendingDrawPassTransfers(
      MTL::RenderCommandEncoder* encoder,
      MTL::RenderPassDescriptor* pass_descriptor,
      DrawPassTransferEncoderMutationMask* mutations_out = nullptr);
  // requeue_clears: the draw that bound the targets was not encoded, so
  // [deferred-clears] draw-pass clears go back to the deferred list instead of
  // being encoded as separate passes.
  bool FlushPendingDrawPassTransfers(bool requeue_clears = false);

  struct TelemetryStats {
    struct ResolveDirectHostTelemetry {
      uint64_t direct_host_attempt = 0;
      uint64_t direct_host_success = 0;
      uint64_t direct_host_reject_gamma = 0;
      uint64_t direct_host_reject_exp_bias = 0;
      uint64_t direct_host_reject_format_mismatch = 0;
      uint64_t direct_host_reject_sample_select = 0;
      uint64_t direct_host_reject_depth_no_fast = 0;
    };

    ResolveDirectHostTelemetry resolve_direct_host = {};
  };
  TelemetryStats GetAndResetTelemetryStats();

  bool IsRenderPassDescriptorDirty() const {
    return render_pass_descriptor_dirty_;
  }

  // Get current render targets for capture
  MTL::Texture* GetColorTarget(uint32_t index) const;
  MTL::Texture* GetDepthTarget() const;
  MTL::Texture* GetDummyColorTarget() const;
  // Get current render targets for pipeline attachment formats.
  MTL::Texture* GetColorTargetForDraw(uint32_t index) const;
  MTL::Texture* GetDepthTargetForDraw() const;
  MTL::Texture* GetDummyColorTargetForDraw() const;
  double GetDepthTargetClearDepth() const;
  // [shadows-native] diagnostic pass / resolve / texture graph for two frames
  // starting at frame FABLE2_SHADOW_TRACE=<N> (trace only, no behaviour change).
  static bool ShadowTraceActive();
  std::string DescribeCurrentTargetsForTrace() const;

  // [alias-clear] FABLE2_ALIAS_CLEAR_NATIVE (default on; docs/alias-clear-20261013.md).
  // The XDK clear draws a constant rectangle into the EDRAM range of a
  // surface aliased at another MSAA level (usually 4x, a quarter of the
  // pixels). Every sample of the covered range gets the same 32-bit word, so
  // any MSAA variant of the same format holds the same bytes. TryAliasClear
  // performs that clear as a resolve-style EDRAM clear (deferred, drawn in
  // the next pass that binds it) on the existing variant of the surface, so
  // the aliased host target, its transfers and its pass are not needed.
  // Returns false without any side effect when the clear cannot be done
  // exactly that way (the caller then draws it as before).
  static bool AliasClearNativeEnabled();
  static bool AliasClearTraceEnabled();
  // [main-depth] FABLE2_MAIN_DEPTH_NATIVE (default on; needs alias clears on;
  // docs/main-depth-20261014.md). Two more alias-clear kinds, both exact:
  // - a nonzero depth word through a D24S8 alias (z on the unorm24 grid, e.g.
  //   0.75) with no D24S8 variant: written as that 32-bit word into an
  //   existing 8888 / 10:10:10:2 unorm variant of the surface, whose host
  //   storage holds any word exactly and which transfers read back bit-exact;
  // - a stencil-only clear: the tiles move to the existing same-format
  //   variant with the transfers a draw would do, then only its stencil is
  //   replaced (deferred like every alias clear).
  static bool MainDepthNativeEnabled();
  // Deferred/resolve clear value flag (bit 63, depth targets only): replace
  // the stencil byte (bits 0:7) and keep the depth.
  static constexpr uint64_t kClearValueStencilOnly = uint64_t(1) << 63;
  struct AliasClearRequest {
    xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;
    uint32_t pitch_tiles_at_32bpp = 0;
    // Covered rectangle in pixels of the draw's surface (at its MSAA level).
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool depth = false;
    uint32_t depth_base = 0;
    xenos::DepthRenderTargetFormat depth_format =
        xenos::DepthRenderTargetFormat::kD24S8;
    // Guest RB_DEPTH_CLEAR layout: depth << 8 | stencil.
    uint32_t depth_value = 0;
    // [main-depth] Only the stencil byte is replaced (depth_value = the
    // stencil reference); the depth stays.
    bool stencil_only = false;
    bool color = false;
    uint32_t color_base = 0;
    // Storage format (xenos::GetStorageColorFormat), 32bpp only.
    xenos::ColorRenderTargetFormat color_format =
        xenos::ColorRenderTargetFormat::k_8_8_8_8;
    // Guest RB_COLOR_CLEAR layout for that format.
    uint32_t color_value = 0;
  };
  bool TryAliasClear(const AliasClearRequest& request);
  // [main-depth] the depth-word kind of TryAliasClear (no D24S8 variant):
  // the rows go to existing word-exact targets of the same pitch.
  bool TryDepthWordAliasClear(const AliasClearRequest& request);
  // One surface of an alias clear: the resolve-clear ownership change and
  // the (deferred) clear of base/pitch/msaa/format over width x height.
  bool CommitAliasClearSurface(uint32_t base, uint32_t pitch,
                               xenos::MsaaSamples msaa, bool is_depth,
                               uint32_t format, uint32_t value, uint32_t width,
                               uint32_t height,
                               const AliasClearRequest& request);
  // [main-depth] the stencil-only kind of TryAliasClear.
  bool TryStencilOnlyAliasClear(const AliasClearRequest& request);
  // [main-depth] diagnostic (FABLE2_MAIN_DEPTH_DUMP=<frame> with
  // FABLE2_MAIN_DEPTH_DUMP_DIR=<dir>): the guest words of the whole EDRAM as
  // every owner holds them now (the resolve dump path, flushed deferred
  // clears first), written to <dir>/f<frame>-<tag>.bin after the GPU ran the
  // current command buffer. Layout: the EDRAM buffer (tile-major, 80x16
  // samples per tile times the resolution scale squared, uint32 each).
  static uint64_t MainDepthDumpFrame();
  void MainDepthDumpEdram(const char* tag);

  // Restore EDRAM contents from snapshot (for trace playback), matching
  // D3D12RenderTargetCache::RestoreEdramSnapshot.
  void RestoreEdramSnapshot(const void* snapshot);

  MTL::Buffer* GetEdramBuffer() const { return edram_buffer_; }

  // [rt-shrink] memory report (FABLE2_MEM_REPORT).
  struct MemReportInfo {
    uint64_t rt_bytes = 0, rt_count = 0;
    uint64_t rt_full_bytes = 0;  // what full EDRAM-height textures would take
    uint64_t heap_bytes = 0, heap_used_bytes = 0, heap_count = 0;
    uint64_t edram_bytes = 0;
    uint64_t other_bytes = 0;  // dummy / transfer helper textures
    uint64_t rt_needed_bytes = 0;  // [rt-shrink] at the rows needed so far
    uint64_t grows = 0, grow_bytes = 0, misses = 0, unclipped_draws = 0;
    uint64_t unowned_claims = 0;
    std::string top;           // largest render targets
  };
  void GetMemReportInfo(MemReportInfo& out) const;
  bool WriteEdramUintPow2BindlessDescriptor(
      IRDescriptorTableEntry* entry, uint32_t element_size_bytes_pow2) const;
  uint64_t GetBindlessResourcesSerial() const {
    return bindless_resources_serial_;
  }
  void CollectBindlessResources(
      std::vector<MTL::Resource*>& resources_out) const;

  struct ResolvePlan {
    draw_util::ResolveInfo resolve_info = {};
    bool valid = false;
    bool noop = false;
    bool needs_copy_export = false;
    bool needs_resolve_clear = false;
    bool needs_render_encoder_end = false;
    uint32_t written_address = 0;
    uint32_t written_length = 0;
    // [effects-native] the source is native-scale effect targets: resolved
    // as at 1x into shared memory (resolve_info is unscaled).
    bool native = false;
  };

  // Resolve (copy) render targets to shared memory
  bool PrepareResolvePlan(rex::memory::Memory& memory, ResolvePlan& plan_out);
  bool Resolve(rex::memory::Memory& memory, uint32_t& written_address,
               uint32_t& written_length,
               MTL::CommandBuffer* command_buffer = nullptr,
               const ResolvePlan* prepared_resolve_plan = nullptr);

  // [tile-resolve] (docs/tile-resolve-20261013.md, FABLE2_TILE_RESOLVE)
  // IssueCopy asks before ending the draw pass for a resolve: true keeps the
  // render encoder open, so the colour copy can run as a tile function inside
  // it. Resolve ends the pass itself (after the tile dispatch, or before any
  // other GPU work), exactly where it ends today otherwise.
  static bool TileResolveEnabled();
  // [tile-resolve extent] Draw passes open with a render area only as tall
  // as their draws need (FABLE2_TILE_RESOLVE_EXTENT=0 disables; only with
  // tile resolves on).
  static bool TileResolveExtentEnabled();
  // [tile-resolve keep] FABLE2_TILE_RESOLVE_CLEAR=0: the resolve's clear is
  // not drawn in the tile resolve's pass. FABLE2_TILE_RESOLVE_KEEP_OPEN=1:
  // the pass also stays open for the next draws (default off).
  static bool TileResolveClearInPassEnabled();
  static bool TileResolveKeepOpenEnabled();
  // [ssmask] FABLE2_CLEAR_LOAD_ACTION (default on): a deferred clear drawn at
  // the start of a new draw pass that covers its whole attachment becomes
  // that attachment's clear load action (no load, no clear quad). Applied to
  // the descriptor right before the encoder is made; returns the attachments
  // changed (bit 0 depth+stencil, bit 1+i colour i) for RestoreClearLoadActions.
  static bool ClearLoadActionEnabled();
  uint32_t ApplyClearLoadActions(MTL::RenderPassDescriptor* pass);
  static void RestoreClearLoadActions(MTL::RenderPassDescriptor* pass,
                                      uint32_t changed);
  // The clear value of a whole-target clear as a load action (the same
  // conversion the transfer pass uses); false when it must be drawn.
  bool ClearValueAsLoadAction(const RenderTargetKey& key, uint64_t clear_value,
                              MTL::ClearColor* color, double* depth,
                              uint32_t* stencil) const;
  bool TileResolveDrawClearInPass(RenderTarget* rt, bool depth,
                                  uint64_t clear_value,
                                  const Transfer::Rectangle& rect,
                                  Transfer::Rectangle& remainder,
                                  bool& has_remainder);
  bool TileResolveOffer(const ResolvePlan& plan);
  // Ends the kept-open pass if it is still the one offered (idempotent).
  void TileResolveEnd(const char* detail);

 protected:
  // Virtual methods from RenderTargetCache
  uint32_t GetMaxRenderTargetWidth() const override;
  uint32_t GetMaxRenderTargetHeight() const override;

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;
  // [rt-shrink] SDK hook (sdk-metal-rt-shrink.patch).
  void OnOwnershipClaimed(RenderTargetKey dest,
                          uint32_t end_tiles_base_relative) override;

  bool IsHostDepthEncodingDifferent(
      xenos::DepthRenderTargetFormat format) const override;
  // [native-scale] render targets kept at 1x while the draw resolution is
  // scaled (docs/resolution-scale.md section 16):
  // - shadow maps (FABLE2_SHADOWS_NATIVE): depth-only passes on a depth
  //   target (EDRAM base + pitch) learned as the source of a square depth
  //   resolve of at least 512x512 and never bound with colour;
  // - effects (FABLE2_EFFECTS_NATIVE_TARGETS, experimental): colour-only
  //   1xMSAA passes at most half as wide as the widest depth-tested pass.
  uint32_t NativeScaleRenderTargets(uint32_t pitch_tiles_at_32bpp,
                                    xenos::MsaaSamples msaa_samples,
                                    uint32_t used_rts_bits,
                                    const uint32_t* edram_bases) override;
  void RequestPixelShaderInterlockBarrier() override;

 private:
  static uint32_t GetMetalEdramDumpFormat(RenderTargetKey key);
  MTL::Library* GetOrCreateEdramLoadLibrary(bool msaa);
  MTL::RenderPipelineState* GetOrCreateEdramLoadPipeline(
      MTL::PixelFormat dest_format, uint32_t sample_count);
  bool InitializeEdramBufferViews();
  void ReleaseEdramBufferViews();
  MTL::Texture* GetEdramUintPow2BufferView(
      uint32_t element_size_bytes_pow2) const;

  MetalCommandProcessor& command_processor_;
  TraceWriter* trace_writer_;

  // Metal device reference
  MTL::Device* device_ = nullptr;
  bool gamma_render_target_as_unorm16_ = false;

  std::unique_ptr<MetalHeapPool> render_target_heap_pool_;

  // EDRAM buffer (10MB embedded DRAM)
  MTL::Buffer* edram_buffer_ = nullptr;
  MTL::Texture* edram_r32_uint_buffer_view_ = nullptr;
  MTL::Texture* edram_r32g32_uint_buffer_view_ = nullptr;
  MTL::Texture* edram_r32g32b32a32_uint_buffer_view_ = nullptr;

  // EDRAM render pipelines for drawing cached render-target contents.
  std::unordered_map<uint64_t, MTL::RenderPipelineState*> edram_load_pipelines_;
  MTL::Library* edram_load_library_ = nullptr;
  MTL::Library* edram_load_library_msaa_ = nullptr;

  // EDRAM dump compute shaders for host render target → EDRAM copies.
  static constexpr size_t kEdramDumpBppCount = 2;     // 32, 64
  static constexpr size_t kEdramDumpSourceCount = 2;  // float, uint
  static constexpr size_t kEdramDumpMsaaCount = 3;    // 1x, 2x, 4x
  MTL::ComputePipelineState*
      edram_dump_color_pipelines_[kEdramDumpBppCount][kEdramDumpSourceCount]
                                 [kEdramDumpMsaaCount] = {};
  MTL::ComputePipelineState* edram_dump_depth_pipelines_[kEdramDumpMsaaCount] =
      {};

  // Resolve compute shaders (Metal XeSL -> MSL metallib).
  static constexpr size_t kResolveScaledCount = 2;    // false, true
  static constexpr size_t kResolveFullDestCount = 5;  // 8, 16, 32, 64, 128
  static constexpr size_t kResolveFastBppCount = 2;   // 32, 64
  static constexpr size_t kResolveFastMsaaCount = 2;  // 1/2x, 4x
  MTL::ComputePipelineState*
      resolve_full_pipelines_[kResolveScaledCount][kResolveFullDestCount] = {};
  MTL::ComputePipelineState*
      resolve_fast_pipelines_[kResolveScaledCount][kResolveFastBppCount]
                             [kResolveFastMsaaCount] = {};

  // Direct host resolve compute shaders (host RT -> shared/scaled resolve
  // memory) for fast and full color copies plus depth copies.
  static constexpr size_t kDirectHostResolveBppCount = 2;     // 32, 64
  static constexpr size_t kDirectHostResolveMsaaCount = 3;    // 1x, 2x, 4x
  static constexpr size_t kDirectHostResolveScaledCount = 2;  // false, true
  static constexpr size_t kDirectHostResolveSourceCount = 2;  // float, uint
  static constexpr size_t kDirectHostResolveFullDestCount =
      5;  // 8, 16, 32, 64, 128
  MTL::ComputePipelineState* direct_host_resolve_pipelines_
      [kDirectHostResolveBppCount][kDirectHostResolveMsaaCount]
      [kDirectHostResolveScaledCount][kDirectHostResolveSourceCount] = {};
  MTL::ComputePipelineState* direct_host_color_full_resolve_pipelines_
      [kDirectHostResolveMsaaCount][kDirectHostResolveScaledCount]
      [kDirectHostResolveSourceCount][kDirectHostResolveFullDestCount] = {};
  MTL::ComputePipelineState*
      direct_host_depth_resolve_pipelines_[kDirectHostResolveMsaaCount]
                                          [kDirectHostResolveScaledCount] = {};

  // [tile-resolve] state of a kept-open draw pass (TileResolveOffer).
  bool tile_resolve_open_ = false;
  uint64_t tile_resolve_gen_ = 0;
  struct TileResolvePipelines;
  TileResolvePipelines* tile_resolve_pipelines_ = nullptr;  // owned
  void TileResolveShutdown();
  bool TileResolvePrecheck(const ResolvePlan& plan);
  // Encodes the copy as a tile function in the kept-open pass, or returns
  // false (with no GPU work done) when this resolve is not eligible.
  struct TileResolveRequest {
    const draw_util::ResolveInfo* resolve_info;
    const void* push_constants;
    size_t push_constants_size;
    const void* host_constants;
    size_t host_constants_size;
    MTL::Texture* source_texture;
    const char* pipeline_label;  // the compute variant's label ("..._cs")
    uint32_t pixels_per_thread;
    bool scaled;
    MTL::Buffer* dest_buffer;
    size_t dest_offset;
    MTL::Texture* fused_views[2];
    const void* rtt_params;
    size_t rtt_params_size;
    bool fused;
    const void* fused_textures[2];
    // Host rows of the source texture the resolve reads end before this one
    // (UINT32_MAX if unknown, e.g. EDRAM wrapping).
    uint32_t source_rows_end;
  };
  bool TryTileResolveEncode(const TileResolveRequest& request);

  // Host depth store compute shaders (1x/2x/4x MSAA).
  MTL::ComputePipelineState* host_depth_store_pipelines_[3] = {};

  // TransferMode list mirrors D3D12RenderTargetCache::TransferMode so logs and
  // structure stay in sync, even if many modes are not implemented yet.
  enum class TransferMode {
    kColorToColor,
    kColorToDepth,
    kDepthToColor,
    kDepthToDepth,
    kColorToStencilBit,
    kDepthToStencilBit,
    kColorAndHostDepthToDepth,
    kDepthAndHostDepthToDepth,
  };

  struct TransferShaderKey {
    TransferMode mode;
    xenos::MsaaSamples source_msaa_samples;
    xenos::MsaaSamples dest_msaa_samples;
    xenos::MsaaSamples host_depth_source_msaa_samples;
    uint32_t source_resource_format;
    uint32_t dest_resource_format;
    uint32_t dest_sample_id_from_sample;
    uint32_t host_depth_source_is_copy;

    // Members above are declared in comparison order, so defaulted member-wise
    // == and <=> reproduce the previous hand-written equality and ordering
    // exactly. (!=, <, >, <=, >= are synthesized from these.)
    bool operator==(const TransferShaderKey& other) const = default;
    auto operator<=>(const TransferShaderKey& other) const = default;

    struct Hasher {
      size_t operator()(const TransferShaderKey& key) const {
        size_t h = size_t(key.mode);
        h ^= (size_t(key.source_msaa_samples) << 4);
        h ^= (size_t(key.dest_msaa_samples) << 8);
        h ^= (size_t(key.host_depth_source_msaa_samples) << 12);
        h ^= (size_t(key.source_resource_format) << 16);
        h ^= (size_t(key.dest_resource_format) << 24);
        h ^= (size_t(key.dest_sample_id_from_sample) << 28);
        h ^= (size_t(key.host_depth_source_is_copy) << 29);
        return h ^ (h >> 16);
      }
    };
  };

  using TransferColorAttachmentFormats =
      std::array<MTL::PixelFormat, xenos::kMaxColorRenderTargets>;

  struct TransferAttachmentFormats {
    TransferColorAttachmentFormats color_attachment_formats = {};
    MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid;
    MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid;
  };

  struct TransferPipelineKey {
    TransferShaderKey shader_key;
    uint32_t color_attachment_index = 0;
    uint32_t native_stencil_output = 0;
    TransferColorAttachmentFormats color_attachment_formats = {};
    MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid;
    MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid;

    bool operator==(const TransferPipelineKey& other) const = default;

    struct Hasher {
      size_t operator()(const TransferPipelineKey& key) const {
        auto combine = [](size_t seed, size_t value) {
          return seed ^ (value + 0x9E3779B9 + (seed << 6) + (seed >> 2));
        };
        size_t h = TransferShaderKey::Hasher()(key.shader_key);
        h = combine(h, key.color_attachment_index);
        h = combine(h, key.native_stencil_output);
        h = combine(h, size_t(key.depth_attachment_format));
        h = combine(h, size_t(key.stencil_attachment_format));
        for (MTL::PixelFormat color_format : key.color_attachment_formats) {
          h = combine(h, size_t(color_format));
        }
        return h;
      }
    };
  };

  struct TransferClearPipelineKey {
    uint32_t color_attachment_index = 0;
    uint32_t sample_count = 1;
    uint32_t dest_is_uint = 0;
    uint32_t is_depth = 0;
    TransferColorAttachmentFormats color_attachment_formats = {};
    MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid;
    MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid;

    bool operator==(const TransferClearPipelineKey& other) const = default;

    struct Hasher {
      size_t operator()(const TransferClearPipelineKey& key) const {
        auto combine = [](size_t seed, size_t value) {
          return seed ^ (value + 0x9E3779B9 + (seed << 6) + (seed >> 2));
        };
        size_t h = key.color_attachment_index;
        h = combine(h, key.sample_count);
        h = combine(h, key.dest_is_uint);
        h = combine(h, key.is_depth);
        h = combine(h, size_t(key.depth_attachment_format));
        h = combine(h, size_t(key.stencil_attachment_format));
        for (MTL::PixelFormat color_format : key.color_attachment_formats) {
          h = combine(h, size_t(color_format));
        }
        return h;
      }
    };
  };

  struct TransferRectanglePlan {
    uint32_t transfer_index = 0;
    std::array<Transfer::Rectangle, Transfer::kMaxRectanglesWithCutout>
        rectangles = {};
    uint32_t rectangle_count = 0;
  };

  struct TransferInvocation {
    Transfer transfer;
    TransferShaderKey shader_key;
    const TransferRectanglePlan* rectangle_plan = nullptr;
    TransferInvocation(const Transfer& transfer,
                       const TransferShaderKey& shader_key,
                       const TransferRectanglePlan* rectangle_plan = nullptr)
        : transfer(transfer),
          shader_key(shader_key),
          rectangle_plan(rectangle_plan) {}
    bool operator<(const TransferInvocation& other) const {
      if (shader_key != other.shader_key) {
        return shader_key < other.shader_key;
      }
      assert_not_null(transfer.source);
      assert_not_null(other.transfer.source);
      uint32_t source_index =
          static_cast<const MetalRenderTarget*>(transfer.source)
              ->temporary_sort_index();
      uint32_t other_source_index =
          static_cast<const MetalRenderTarget*>(other.transfer.source)
              ->temporary_sort_index();
      if (source_index != other_source_index) {
        return source_index < other_source_index;
      }
      return transfer.start_tiles < other.transfer.start_tiles;
    }
    bool CanBeMergedIntoOneDraw(const TransferInvocation& other) const {
      return shader_key == other.shader_key &&
             transfer.AreSourcesSame(other.transfer);
    }
  };

  struct AttachmentPlanAttachment {
    MetalRenderTarget* render_target = nullptr;
    MTL::Texture* texture = nullptr;
    bool bound = false;
    bool needs_initial_clear = false;
    bool load_action_safe = false;
  };
  struct AttachmentPlan {
    AttachmentPlanAttachment depth = {};
    std::array<AttachmentPlanAttachment, xenos::kMaxColorRenderTargets> colors =
        {};
    bool has_any_color_target = false;
    uint32_t coverage_width = 0;
    uint32_t coverage_height = 0;
    uint32_t coverage_samples = 1;
  };

  std::unordered_map<TransferPipelineKey, MTL::RenderPipelineState*,
                     TransferPipelineKey::Hasher>
      transfer_pipelines_;
  std::vector<TransferInvocation> transfer_invocations_;
  MTL::Library* transfer_library_ = nullptr;
  // [xfer-prewarm] compiled by a prewarm thread at startup (transfer clears).
  std::mutex transfer_library_mutex_;
  std::unordered_map<TransferClearPipelineKey, MTL::RenderPipelineState*,
                     TransferClearPipelineKey::Hasher>
      transfer_clear_pipelines_;
  std::array<RenderTarget*, 1 + xenos::kMaxColorRenderTargets>
      pending_draw_pass_render_targets_ = {};
  std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
      pending_draw_pass_transfers_;
  uint32_t pending_draw_pass_transfer_mask_ = 0;
  uint32_t pending_draw_pass_full_overwrite_mask_ = 0;
  uint32_t pending_draw_pass_load_dontcare_mask_ = 0;
  // [tile-edram] K1: the pending depth transfers read the destination's own
  // previous host depth (host_depth_source == destination). Its rt_depth_store
  // copy into the EDRAM buffer was already encoded at queue time, before the
  // draw pass opened, so the transfer quads can run inside the draw pass
  // (host_depth_source_is_copy). A standalone flush redoes the store.
  bool pending_draw_pass_self_host_depth_stored_ = false;
  // [deferred-clears] resolve clears not encoded yet (FABLE2_DEFERRED_CLEARS,
  // default on). Ownership already changed at resolve time; only the GPU work
  // waits. At most one entry per render target. A clear is drawn first inside
  // the next draw pass that binds its target (pending_draw_pass_clear_*), or
  // flushed as today's separate clear pass before anything else reads or
  // writes that target (resolve, transfer, host depth store, snapshot).
  struct DeferredClear {
    MetalRenderTarget* render_target = nullptr;
    uint64_t clear_value = 0;
    Transfer::Rectangle rectangle = {};
  };
  std::vector<DeferredClear> deferred_clears_;
  std::array<RenderTarget*, 1 + xenos::kMaxColorRenderTargets>
      pending_draw_pass_clear_targets_ = {};
  std::array<uint64_t, 1 + xenos::kMaxColorRenderTargets>
      pending_draw_pass_clear_values_ = {};
  std::array<Transfer::Rectangle, 1 + xenos::kMaxColorRenderTargets>
      pending_draw_pass_clear_rects_ = {};
  uint32_t pending_draw_pass_clear_mask_ = 0;
  // [ssm-merge] FABLE2_SSM_MERGE (default off, =1 on; docs/ssm-merge-20261015.md).
  // A stencil-only alias clear (main-depth P8a) changes ownership at once but
  // waits with its GPU work: the transfers into the variant (with the
  // variant's carried depth-word clear, still in deferred_clears_) and then
  // the stencil clear. The next Update that binds the variant as depth with
  // no transfers of its own draws all of it inside that draw pass: carried
  // clear, transfer quads, then the stencil clear (the post-transfer slot).
  // Every other cache call first does today's standalone work
  // (FlushQueuedStencilAlias).
  struct QueuedStencilAlias {
    MetalRenderTarget* render_target = nullptr;
    std::vector<Transfer> transfers;
    uint64_t stencil_value = 0;
    Transfer::Rectangle rectangle = {};
    // The carried clear's rectangle (valid when has_carried).
    bool has_carried = false;
    Transfer::Rectangle carried_rectangle = {};
  };
  QueuedStencilAlias queued_stencil_alias_;
  // Slot 0 of the pending draw pass: a stencil-only clear drawn after the
  // transfer quads (the opposite order of pending_draw_pass_clear_*).
  bool pending_draw_pass_post_clear_ = false;
  DeferredClear pending_draw_pass_post_clear_value_;
  static bool SsmMergeEnabled();
  // [ssm-merge] FABLE2_SSM_STENCIL_FOLD (default on): a pending whole-target
  // stencil-only clear of a 1x depth source whose tiles all move to other
  // targets in this update is never drawn. Those transfers read the stencil
  // from a constant texture holding the cleared byte, and the clear is
  // dropped: the target owns none of its tiles any more, so nothing can read
  // its stencil there (host depth reads only depth, which the clear keeps).
  static bool SsmStencilFoldEnabled();
  void FoldDeadStencilClears(RenderTarget* const* accumulated_targets,
                             const std::vector<Transfer>* update_transfers);
  MTL::Texture* GetConstantStencilTexture(uint32_t width, uint32_t height,
                                          uint8_t value);
  std::unordered_map<const RenderTarget*, uint8_t> stencil_const_sources_;
  std::map<uint64_t, MTL::Texture*> stencil_const_textures_;
  // [clear-const] FABLE2_CLEAR_CONST_SOURCE (default on;
  // docs/ssm-merge-20261015.md): a transfer whose source colour target has a
  // pending whole-target clear (the composite's resolve clear, flushed as a
  // pass of its own at the next frame's first transfer before) reads a
  // texture holding exactly that clear (made once with the same clear load
  // action the in-pass clear uses) instead. The clear stays deferred, so the
  // composite pass that binds the target again starts with it as its load
  // action.
  static bool ClearConstSourceEnabled();
  bool TryClearConstSource(RenderTarget* source,
                           RenderTarget* const* accumulated_targets);
  MTL::Texture* GetConstantClearColorTexture(MetalRenderTarget* rt,
                                             uint64_t clear_value);
  std::unordered_map<const RenderTarget*, MTL::Texture*> clear_const_sources_;
  struct ClearConstTexture {
    MTL::PixelFormat format;
    uint32_t width, height;
    uint64_t value;
    MTL::Texture* texture;
  };
  std::vector<ClearConstTexture> clear_const_textures_;
  bool FlushQueuedStencilAlias();
  bool StencilAliasCoversTarget(const QueuedStencilAlias& q) const;
  enum class DeferredClearFlushReason {
    kResolve,     // a copy resolve reads the EDRAM
    kTransfer,    // an ownership transfer reads or writes the target
    kReclear,     // another resolve clear of the same target
    kPreflight,   // bound, but the clear cannot be drawn in this pass
    kGuard,       // bound, but the draw was not encoded
    kSwap,        // FABLE2_DEFERRED_CLEARS_SWAP_FLUSH=1 (conservative arm)
    kOther,       // snapshot restore, explicit flush
  };
  static bool DeferredClearsEnabled();
  static bool DeferredClearsSwapFlush();
  bool EncodeResolveClearPass(RenderTarget* render_target, uint64_t clear_value,
                              const Transfer::Rectangle& rectangle,
                              MTL::CommandBuffer* command_buffer = nullptr);
  bool FlushDeferredClearsFor(const RenderTarget* render_target,
                              DeferredClearFlushReason reason);
  bool FlushDeferredClears(DeferredClearFlushReason reason);
  bool FlushPendingDrawPassClears(DeferredClearFlushReason reason);
  bool CanDrawDeferredClearInPass(uint32_t slot, MetalRenderTarget* rt,
                                  const TransferAttachmentFormats& formats);
  void NoteDeferredClearFlush(DeferredClearFlushReason reason,
                              const DeferredClear& clear);
  static uint32_t DeferredClearTouchedKb(const MetalRenderTarget* rt,
                                         const Transfer::Rectangle& rect,
                                         uint32_t scale_x, uint32_t scale_y,
                                         bool* full_target_out = nullptr);
  mutable TelemetryStats telemetry_;
  MTL::DepthStencilState* transfer_depth_state_ = nullptr;
  MTL::DepthStencilState* transfer_depth_state_none_ = nullptr;
  MTL::DepthStencilState* transfer_depth_clear_state_ = nullptr;
  MTL::DepthStencilState* transfer_stencil_clear_state_ = nullptr;
  MTL::DepthStencilState* transfer_stencil_output_state_ = nullptr;
  MTL::DepthStencilState* transfer_stencil_bit_states_[8] = {};
  MTL::Buffer* transfer_dummy_buffer_ = nullptr;
  MTL::Texture* transfer_dummy_color_float_[3] = {};
  MTL::Texture* transfer_dummy_color_uint_[3] = {};
  MTL::Texture* transfer_dummy_depth_[3] = {};
  MTL::Texture* transfer_dummy_stencil_[3] = {};
  bool msaa_2x_supported_ = true;
  uint64_t bindless_resources_serial_ = 1;

  // Current render targets - updated by base class Update() call

  MetalRenderTarget* current_color_targets_[4] = {};
  MetalRenderTarget* current_depth_target_ = nullptr;

  // Track the last REAL (non-dummy) render targets for capture
  MetalRenderTarget* last_real_color_targets_[4] = {};
  MetalRenderTarget* last_real_depth_target_ = nullptr;

  // Track all created render targets so we can find them
  std::unordered_map<uint32_t, MetalRenderTarget*> render_target_map_;

  // Render pass descriptor cache
  MTL::RenderPassDescriptor* cached_render_pass_descriptor_ = nullptr;
  bool render_pass_descriptor_dirty_ = true;
  uint32_t cached_render_pass_descriptor_sample_count_ = 0;
  bool cached_render_pass_descriptor_fallback_depth_required_ = false;

  // Transient dummy color target used for passes with no bound color render
  // targets, so the render pass/pipeline still has a matching color output.
  // Cached by shape/format and recreated only when the required shape changes.
  // (D3D12 needs no analog.)
  std::unique_ptr<MetalRenderTarget> dummy_color_target_owner_;
  uint64_t dummy_color_target_shape_key_ = 0;
  mutable MetalRenderTarget* dummy_color_target_ = nullptr;

  // Debug helper to log a small region of the current color RT0.
  // Helper methods
  MTL::Texture* CreateColorTexture(uint32_t width, uint32_t height,
                                   xenos::ColorRenderTargetFormat format,
                                   uint32_t samples,
                                   bool transient_render_target_only = false,
                                   bool allow_unpooled_fallback = true);
  MTL::Texture* CreateDepthTexture(uint32_t width, uint32_t height,
                                   xenos::DepthRenderTargetFormat format,
                                   uint32_t samples);
  MTL::Texture* CreateTransientDepthTexture(uint32_t width, uint32_t height,
                                            uint32_t samples);
  MTL::Texture* GetStencilTextureView(MetalRenderTarget* render_target);

  MTL::PixelFormat GetColorResourcePixelFormat(
      xenos::ColorRenderTargetFormat format) const;
  MTL::PixelFormat GetColorDrawPixelFormat(
      xenos::ColorRenderTargetFormat format) const;
  MTL::PixelFormat GetColorOwnershipTransferPixelFormat(
      xenos::ColorRenderTargetFormat format, bool* is_integer_out) const;
  MTL::PixelFormat GetDepthPixelFormat(
      xenos::DepthRenderTargetFormat format) const;
  TransferShaderKey GetTransferShaderKey(
      RenderTargetKey source_key, RenderTargetKey dest_key,
      const RenderTargetKey* host_depth_source_key,
      bool host_depth_source_is_copy, bool stencil_bit,
      bool dest_sample_id_from_sample_default) const;
  bool GetActiveTransferAttachmentFormats(
      MTL::RenderPassDescriptor* pass_descriptor,
      TransferAttachmentFormats& attachment_formats_out) const;
  bool GetCurrentTransferAttachmentFormats(
      TransferAttachmentFormats& attachment_formats_out) const;
  bool CanQueueDrawPassTransfers(uint32_t render_target_index,
                                 RenderTarget* const* render_targets,
                                 const std::vector<Transfer>& transfers,
                                 bool* self_host_depth_out = nullptr) const;
  // [tile-edram] switches (env, read once). Master FABLE2_TILE_EDRAM=0 turns
  // every kind off; FABLE2_TILE_EDRAM_SELFDEPTH=0 turns K1 off.
  static bool TileEdramSelfDepthEnabled();
  // Encodes the rt_depth_store compute (destination host depth -> EDRAM
  // buffer) for the transfers of one depth destination whose host depth
  // source is the destination itself. Ends the open render encoder (flushing
  // prepared draws) first. Returns false if nothing could be encoded.
  bool EncodeSelfHostDepthStore(MTL::CommandBuffer* cmd, RenderTarget* dest_rt,
                                const std::vector<Transfer>& transfers,
                                const std::vector<TransferRectanglePlan>&
                                    transfer_rectangles);
  bool PendingDrawPassTransfersFullyOverwriteTarget(
      uint32_t render_target_index, RenderTarget* render_target,
      const std::vector<Transfer>& transfers) const;
  bool BuildTransferRectanglePlans(
      RenderTargetKey dest_key, const std::vector<Transfer>& transfers,
      const Transfer::Rectangle* cutout, bool require_all_rectangles,
      std::vector<TransferRectanglePlan>& transfer_rectangles_out) const;
  bool PreflightPendingDrawPassTransfers(
      const TransferAttachmentFormats& attachment_formats);
  bool PreflightPendingDrawPassTransfers(
      MTL::RenderPassDescriptor* pass_descriptor);
  bool BuildCurrentAttachmentPlan(uint32_t expected_sample_count,
                                  AttachmentPlan& plan_out);
  void MarkRenderPassDescriptorDirty();
  bool IsRenderPassDescriptorCompatibleSlow(
      MTL::RenderPassDescriptor* pass_descriptor,
      uint32_t expected_sample_count,
      bool fallback_depth_attachment_required) const;
  void ClearPendingDrawPassTransfers();

  // EDRAM compute shader setup
  bool InitializeEdramComputeShaders();
  void ShutdownEdramComputeShaders();
  void InitializeDirectHostResolvePipelines(bool draw_resolution_scaled);
  void ResetDirectHostResolvePipelines(bool release_existing);

  // Transfer pipeline setup for host RT ownership transfers. The shader keys
  // intentionally mirror D3D12RenderTargetCache transfer modes.
  MTL::RenderPipelineState* BuildTransferPipeline(
      const TransferPipelineKey& pipeline_key, bool dest_is_uint) const;
  MTL::RenderPipelineState* TakePrewarmedTransferPipeline(
      const TransferPipelineKey& pipeline_key, bool dest_is_uint);
  void NoteTransferPipelineKey(const TransferPipelineKey& pipeline_key,
                               bool dest_is_uint);
  void StopTransferPrewarm();
  struct XferPrewarm;
  std::shared_ptr<XferPrewarm> xfer_prewarm_;
  std::vector<std::pair<TransferPipelineKey, bool>> xfer_early_keys_;
  static std::string XferKeyLine(const TransferPipelineKey& k,
                                 bool dest_is_uint);
  MTL::RenderPipelineState* GetOrCreateTransferPipelines(
      const TransferShaderKey& key, MTL::PixelFormat dest_format,
      bool dest_is_uint, bool native_stencil_output,
      uint32_t color_attachment_index = 0,
      const TransferColorAttachmentFormats* color_attachment_formats = nullptr,
      MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid,
      MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid);
  MTL::RenderPipelineState* GetOrCreateTransferClearPipeline(
      MTL::PixelFormat dest_format, bool dest_is_uint, bool is_depth,
      uint32_t sample_count, uint32_t color_attachment_index = 0,
      const TransferColorAttachmentFormats* color_attachment_formats = nullptr,
      MTL::PixelFormat depth_attachment_format = MTL::PixelFormatInvalid,
      MTL::PixelFormat stencil_attachment_format = MTL::PixelFormatInvalid);
  MTL::Library* GetOrCreateTransferLibrary();
  MTL::Texture* GetTransferDummyTexture(MTL::PixelFormat format,
                                        uint32_t sample_count);
  MTL::Texture* GetTransferDummyColorFloatTexture(uint32_t sample_count);
  MTL::Texture* GetTransferDummyColorUintTexture(uint32_t sample_count);
  MTL::Texture* GetTransferDummyDepthTexture(uint32_t sample_count);
  MTL::Texture* GetTransferDummyStencilTexture(uint32_t sample_count);
  MTL::Buffer* GetTransferDummyBuffer();
  MTL::DepthStencilState* GetTransferDepthStencilState(bool depth_write);
  MTL::DepthStencilState* GetTransferNoDepthStencilState();
  MTL::DepthStencilState* GetTransferDepthClearState();
  MTL::DepthStencilState* GetTransferStencilClearState();
  MTL::DepthStencilState* GetTransferStencilOutputState();
  MTL::DepthStencilState* GetTransferStencilBitState(uint32_t bit);
  // Shared builder for the transfer depth/stencil states above. The stencil
  // half (when enabled) is always Always/Keep/Keep/Replace with readMask 0xFF;
  // callers vary only the depth compare/write and the stencil write mask.
  MTL::DepthStencilState* BuildTransferDepthStencilState(
      MTL::CompareFunction depth_compare, bool depth_write, bool stencil_enable,
      uint32_t stencil_write_mask);

  // Sole host-side transfer and resolve-clear execution entry point.
  bool PerformTransfersAndResolveClears(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const uint64_t* render_target_resolve_clear_values = nullptr,
      const Transfer::Rectangle* resolve_clear_rectangle = nullptr,
      MTL::CommandBuffer* command_buffer = nullptr,
      MTL::RenderCommandEncoder* active_render_encoder = nullptr,
      MTL::RenderPassDescriptor* active_render_pass_descriptor = nullptr,
      DrawPassTransferEncoderMutationMask* mutations_out = nullptr);

  // Writes contents of host render targets within rectangles from
  // ResolveInfo::GetCopyEdramTileSpan to edram_buffer_.
  void DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                         uint32_t dump_rows, uint32_t dump_pitch,
                         MTL::CommandBuffer* command_buffer = nullptr,
                         const char* encoder_label = nullptr);

  bool TryDirectHostResolveCopy(
      const draw_util::ResolveInfo& resolve_info,
      const draw_util::ResolveCopyShaderConstants& copy_constants,
      draw_util::ResolveCopyShaderIndex copy_shader, uint32_t dump_base,
      uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch,
      MTL::CommandBuffer* command_buffer, uint32_t& written_address,
      uint32_t& written_length);
  struct ResolveDestinationBuffer {
    MTL::Buffer* buffer = nullptr;
    size_t offset = 0;
    size_t length = 0;
  };
  bool PrepareResolveDestinationBuffer(
      const draw_util::ResolveInfo& resolve_info, bool draw_resolution_scaled,
      ResolveDestinationBuffer& destination);
  MTL::ComputePipelineState* GetResolvePipeline(
      draw_util::ResolveCopyShaderIndex copy_shader, bool scaled) const;
  MTL::ComputePipelineState* GetDirectHostResolvePipeline(
      bool is_64bpp, xenos::MsaaSamples msaa_samples, bool scaled,
      bool source_is_uint) const;
  MTL::ComputePipelineState* GetDirectHostColorFullResolvePipeline(
      xenos::MsaaSamples msaa_samples, bool scaled, bool source_is_uint,
      draw_util::ResolveCopyShaderIndex copy_shader) const;
  MTL::ComputePipelineState* GetDirectHostDepthResolvePipeline(
      xenos::MsaaSamples msaa_samples, bool scaled) const;

  // [edram-transfer] trace (tr_n_xfer_*; FABLE2_XFER_TRACE=1 adds a 10 s log
  // line with the top source -> destination pairs and the reasons transfers
  // went to a separate pass, as CanQueueDrawPassTransfers source lines).
  static bool XferTraceEnabled();
  bool QueueReject(int line) const {
    queue_reject_line_ = line;
    return false;
  }
  void NoteDrawPassTransferRoute(bool queued);
  void NoteOwnershipTransfers(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers);
  // [effects-native]
 public:
  // Scale of the resolve being performed: draw_resolution_scale, or 1 when
  // its source is a native-scale effect target.
  bool IsResolveScaled() const {
    return resolve_scale_x_ > 1 || resolve_scale_y_ > 1;
  }
  static bool EffectsNativeEnabled();
  static bool ShadowsNativeEnabled();
  static bool TexSpaceNativeEnabled();
  // [native-scale] Fable II texture-space pixel shaders (character morph,
  // dog lighting and its seam dilation): draws with them render at 1x.
  static bool IsTexSpacePixelShader(uint64_t ucode_hash);
  // Set around RenderTargetCache::Update for such a draw.
  void SetTexSpaceDrawHint(bool hint) { texspace_draw_hint_ = hint; }
  // Resolution scale of a render target (1 for native-scale targets).
  uint32_t RenderTargetScaleX(RenderTargetKey key) const {
    return key.native_scale ? 1u : draw_resolution_scale_x();
  }
  uint32_t RenderTargetScaleY(RenderTargetKey key) const {
    return key.native_scale ? 1u : draw_resolution_scale_y();
  }
  // Whether the current draw's render targets are native-scale.
  bool IsCurrentPassNativeScale() const;

  // [rt-shrink] lazily sized host render targets (FABLE2_RT_SHRINK,
  // docs/resolution-scale.md section 21). Mode: 0 off (EDRAM-height
  // textures), 1 on, 2 census (track the rows, keep full textures).
  static uint32_t RtShrinkMode();
  static bool RtShrinkTrace();
  static bool RtShrinkVerify();
  uint32_t RtShrinkFullHeight(RenderTargetKey key) const;
  // Host pixel rows of a tile row of this key.
  uint32_t RtShrinkTileRowHeight(RenderTargetKey key) const;
  void RtShrinkNoteRows(MetalRenderTarget* rt, uint32_t rows);
  // Before encoding a draw: the scissor and the render-pass partners.
  void RtShrinkNoteDraw(RenderTarget* const* accumulated_targets);
  // Grows every render target whose texture is shorter than its needed rows.
  // Only where no encoder of ours is open: ends the render encoder.
  bool RtShrinkApplyGrowth();
  bool RtShrinkGrow(MetalRenderTarget* rt, uint32_t new_height,
                    bool discard_contents = false);
  // Verify (FABLE2_RT_SHRINK_VERIFY=1): logs an access past the texture.
  void RtShrinkCheckRows(const MetalRenderTarget* rt, uint32_t rows,
                         const char* what);
  // Global EDRAM tiles [start, end) (end may exceed the tile count).
  void RtShrinkCheckTiles(const MetalRenderTarget* rt, uint32_t start_tiles,
                          uint32_t end_tiles, const char* what);
  void RtShrinkCheckTransfer(const MetalRenderTarget* rt,
                             const Transfer& transfer, const char* what);
 public:
  // Command processor, before the scissor clamp: host rows of the scissor.
  void RtShrinkCheckDraw(uint32_t scissor_bottom_rows,
                         uint32_t viewport_bottom_rows, bool clip_disabled);
 private:
  void SetUpRenderTargetViews(MetalRenderTarget* render_target);
  std::vector<MetalRenderTarget*> rt_shrink_pending_;
  const char* rt_shrink_claim_context_ = "other";
  uint64_t rt_shrink_grows_ = 0, rt_shrink_grow_bytes_ = 0;
  uint64_t rt_shrink_misses_ = 0;
  uint64_t rt_shrink_unclipped_draws_ = 0;
  uint64_t rt_shrink_unowned_claims_ = 0;

 private:
  // [effects-native] lever-5 classification trace (diagnostic only).
  static bool EffectsNativeTraceEnabled();
  bool IsEffectsNativeEligible(RenderTargetKey key) const;
  void NoteEffectsNativeDraw(RenderTarget* const* targets, bool depth_used);
  uint32_t effects_scene_pitch_tiles_ = 0;
  uint32_t resolve_scale_x_ = 1;
  uint32_t resolve_scale_y_ = 1;
  // [native-scale] shadow-map depth targets: key (base_tiles << 16) | pitch.
  static uint32_t ShadowTargetId(uint32_t base_tiles, uint32_t pitch_tiles) {
    return (base_tiles << 16) | pitch_tiles;
  }
  std::vector<uint32_t> shadow_targets_;
  std::vector<uint32_t> shadow_rejected_targets_;
  void LearnShadowTarget(const draw_util::ResolveInfo& resolve_info);
  bool texspace_draw_hint_ = false;
  // Mixed-scale depth transfers cannot use a host-depth source of another
  // scale (the EDRAM host-depth copy and the shaders address one scale); the
  // transfer then reads the guest-format depth of its source instead.
  void DropMixedScaleHostDepth(RenderTarget* dest,
                               std::vector<Transfer>& transfers);
  // Converts the source tiles of a resolve to one scale (the majority) with
  // ownership transfers; returns whether the resolve is native-scale.
  bool UnifyResolveSourceScale(const draw_util::ResolveInfo& resolve_info,
                               bool& unified_out);
  mutable int queue_reject_line_ = 0;
  std::unordered_map<int, uint32_t> xfer_reject_lines_;
  std::unordered_map<uint64_t, uint32_t> xfer_pairs_;
  std::chrono::steady_clock::time_point xfer_last_log_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_RENDER_TARGET_CACHE_H_
