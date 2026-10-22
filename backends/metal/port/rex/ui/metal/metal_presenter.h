/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#ifndef XENIA_UI_METAL_METAL_PRESENTER_H_
#define XENIA_UI_METAL_METAL_PRESENTER_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "rex/ui/metal/metal_provider.h"
#include "rex/ui/presenter.h"
#include "rex/ui/surface.h"

#ifdef __OBJC__
@class CAMetalLayer;
@protocol MTLCommandQueue;
@protocol MTLTexture;
#else
typedef struct objc_object CAMetalLayer;
typedef struct objc_object* id;
#endif

namespace rex {
namespace ui {
namespace metal {

class MetalProvider;

class MetalGuestOutputRefreshContext final
    : public Presenter::GuestOutputRefreshContext {
 public:
  MetalGuestOutputRefreshContext(bool& is_8bpc_out_ref, id resource)
      : Presenter::GuestOutputRefreshContext(is_8bpc_out_ref),
        resource_(resource) {}

  // The guest output Metal texture that the refresher should write to.
  // Initial state is undefined, refresher must write all pixels.
  id resource_uav_capable() const { return resource_; }
  void SetSubmissionId(uint64_t submission_id) {
    submission_id_ = submission_id;
  }
  uint64_t submission_id() const { return submission_id_; }

 private:
  id resource_;
  uint64_t submission_id_ = 0;
};

class MetalUIDrawContext final : public UIDrawContext {
 public:
  MetalUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                     uint32_t render_target_height,
                     id command_buffer,  // id<MTLCommandBuffer>
                     id render_encoder)  // id<MTLRenderCommandEncoder>
      : UIDrawContext(presenter, render_target_width, render_target_height),
        command_buffer_(command_buffer),
        render_encoder_(render_encoder) {}

  id command_buffer() const { return command_buffer_; }  // id<MTLCommandBuffer>
  id render_encoder() const {
    return render_encoder_;
  }  // id<MTLRenderCommandEncoder>

 private:
  id command_buffer_;  // id<MTLCommandBuffer>
  id render_encoder_;  // id<MTLRenderCommandEncoder>
};

class MetalPresenter : public Presenter {
 public:
  MetalPresenter(MetalProvider* provider,
                 HostGpuLossCallback host_gpu_loss_callback);
  ~MetalPresenter() override;

  bool Initialize();
  void Shutdown();

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  struct CaptureTiming {
    int64_t frame_submitted_monotonic_us = 0;
    uint64_t frame_id = 0;
    uint64_t submission_id = 0;
  };
  bool CaptureGuestOutput(RawImage& image_out) override;
  bool CaptureGuestOutput(RawImage& image_out, CaptureTiming* timing);
  // Test control `capture screen`: the presented drawable after the ImGui overlay (turbo notice,
  // F3 overlay, menus), RGBA8 at window pixel size. Only while a request is pending does the
  // layer switch to framebufferOnly = NO (restored right after), so it costs nothing otherwise.
  // Call from a non-UI thread; waits for the next paint.
  bool CaptureScreen(RawImage& image_out, uint32_t timeout_ms, uint64_t* frame_id_out);
  // Test control `capture at <frame> ...` (frame-matched parity, docs/readiness.md): the guest output of
  // exactly these guest frames, read back on the GPU thread as each frame's output is refreshed, so
  // arms of an A/B capture the same guest moments. Returns 1 when all were captured, 0 on timeout,
  // -1 when the first frame had already passed, another request is pending or a readback failed
  // (captures_out holds what was captured; *current_frame_out = the last frame seen). Call from a
  // non-UI, non-GPU thread.
  struct FrameCapture {
    RawImage image;
    CaptureTiming timing;
    uint64_t requested_frame = 0;
    uint32_t pixel_format = 0;
    bool ok = false;
  };
  int CaptureGuestOutputAtFrames(const std::vector<uint64_t>& target_frames, uint32_t timeout_ms,
                                 std::vector<FrameCapture>& captures_out, uint64_t* current_frame_out);

  bool CopyTextureToGuestOutput(MTL::Texture* source_texture, id dest_texture,
                                uint32_t source_width, uint32_t source_height,
                                bool force_swap_rb, bool use_pwl_gamma_ramp,
                                uint64_t* submission_out = nullptr);

  bool UpdateGammaRamp(const void* table_data, size_t table_bytes,
                       const void* pwl_data, size_t pwl_bytes);

 protected:
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width,
      uint32_t new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(
      uint32_t mailbox_index, uint32_t frontbuffer_width,
      uint32_t frontbuffer_height,
      std::function<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out_ref) override;

 private:
  // Metal's blit encoder copyFromTexture:toTexture requires identical pixel
  // formats. The swap surface may be 10-bit or BGRA while the guest output is
  // RGBA8 for PNG capture, so a shader conversion path is required.
  bool EnsureCopyTextureConvertPipelines();
  // RGBA8 with alpha 255 from a guest output readback (RGBA8, BGRA8 or 10-bit packed).
  static void ConvertGuestOutputReadbackToRGBA8(RawImage& image_out, uint32_t pixel_format);
  void MaybeReadBackGuestOutputForFrameCapture(uint32_t mailbox_index);
  bool EnsureApplyGammaPipelines();
  bool EnsureGuestOutputPaintResources(uint32_t pixel_format);

  struct PresenterTextureViewCacheEntry {
    MTL::Texture* parent = nullptr;
    MTL::Texture* view = nullptr;
    bool full_descriptor = false;
    MTL::PixelFormat pixel_format = MTL::PixelFormatInvalid;
    MTL::TextureType texture_type = MTL::TextureType2D;
    uint64_t level_location = 0;
    uint64_t level_length = 0;
    uint64_t slice_location = 0;
    uint64_t slice_length = 0;
    MTL::TextureSwizzleChannels swizzle = {};
  };

  MTL::Texture* GetCachedPresenterPixelFormatView(
      PresenterTextureViewCacheEntry& entry, MTL::Texture* parent,
      MTL::PixelFormat pixel_format);
  MTL::Texture* GetCachedPresenterTextureView(
      PresenterTextureViewCacheEntry& entry, MTL::Texture* parent,
      MTL::PixelFormat pixel_format, MTL::TextureType texture_type,
      NS::Range level_range, NS::Range slice_range,
      MTL::TextureSwizzleChannels swizzle);
  void ReleaseCachedPresenterTextureView(PresenterTextureViewCacheEntry& entry);
  void ReleaseCachedPresenterTextureViews();

  MetalProvider* provider_;
  MTL::Device* device_ = nullptr;

  CAMetalLayer* metal_layer_ = nullptr;
  struct ScreenCaptureRequest;
  struct FrameCaptureRequest;
  std::shared_ptr<FrameCaptureRequest> frame_capture_;  // guarded by frame_capture_mutex_
  std::mutex frame_capture_mutex_;
  std::atomic<bool> frame_capture_pending_{false};
  std::atomic<uint64_t> last_refreshed_frame_{0};
  std::shared_ptr<ScreenCaptureRequest> screen_capture_;  // guarded by screen_capture_mutex_
  std::mutex screen_capture_mutex_;
  std::atomic<bool> screen_capture_pending_{false};
  bool screen_capture_restore_fbo_ = false;  // UI thread
  id command_queue_ = nullptr;  // id<MTLCommandQueue>
  id shared_event_ = nullptr;   // id<MTLSharedEvent>

  // Compute pipeline state used to convert/copy from swap formats to the
  // RGBA8 guest output texture.
  id copy_texture_convert_pipeline_2d_ =
      nullptr;  // id<MTLComputePipelineState>
  id copy_texture_convert_pipeline_2d_array_ =
      nullptr;                               // id<MTLComputePipelineState>
  id apply_gamma_table_pipeline_ = nullptr;  // id<MTLComputePipelineState>
  id apply_gamma_pwl_pipeline_ = nullptr;    // id<MTLComputePipelineState>
  id gamma_output_texture_ = nullptr;        // id<MTLTexture>
  uint32_t gamma_output_width_ = 0;
  uint32_t gamma_output_height_ = 0;
  id gamma_ramp_buffer_ = nullptr;         // id<MTLBuffer>
  id gamma_ramp_table_texture_ = nullptr;  // id<MTLTexture>
  id gamma_ramp_pwl_texture_ = nullptr;    // id<MTLTexture>
  uint32_t gamma_ramp_buffer_size_ = 0;
  bool gamma_ramp_table_valid_ = false;
  bool gamma_ramp_pwl_valid_ = false;
  PresenterTextureViewCacheEntry linear_presenter_view_;
  PresenterTextureViewCacheEntry array_presenter_view_;
  PresenterTextureViewCacheEntry swizzle_presenter_view_;
  id guest_output_pipeline_bilinear_ = nullptr;  // id<MTLRenderPipelineState>
  id guest_output_pipeline_bilinear_dither_ =
      nullptr;                         // id<MTLRenderPipelineState>
  id guest_output_sampler_ = nullptr;  // id<MTLSamplerState>
  uint32_t guest_output_pipeline_format_ = 0;

  id metalfx_scaler_ = nullptr;          // id<MTLFXSpatialScaler>
  id metalfx_output_texture_ = nullptr;  // id<MTLTexture>
  uint32_t metalfx_input_width_ = 0;
  uint32_t metalfx_input_height_ = 0;
  uint32_t metalfx_output_width_ = 0;
  uint32_t metalfx_output_height_ = 0;
  uint32_t metalfx_color_format_ = 0;
  uint32_t metalfx_color_processing_mode_ = 0;
  float surface_scale_ = 1.0f;
  // CAMetalLayer contentsScale: the window's backing scale (FABLE2_RETINA, default on) or 1.
  float layer_contents_scale_ = 1.0f;
  // Despite the name these are the surface size from the SDK, which on macOS is already in
  // pixels (SDL_GetWindowSizeInPixels); surface_scale_ stays 1 there.
  uint32_t surface_width_in_points_ = 0;
  uint32_t surface_height_in_points_ = 0;

  // Guest output textures for PNG capture (mailbox system)
  std::array<id, kGuestOutputMailboxSize> guest_output_textures_;
  std::atomic<uint32_t> last_guest_output_mailbox_index_{UINT32_MAX};
  std::array<uint64_t, kGuestOutputMailboxSize> guest_output_submissions_{};
  uint64_t VsyncLockFrameId(uint32_t mailbox_index) const;
  std::array<uint64_t, kGuestOutputMailboxSize> guest_output_frame_ids_{};
  std::array<int64_t, kGuestOutputMailboxSize> guest_output_submitted_us_{};
  uint64_t guest_output_waited_submission_ = 0;
  std::atomic<uint64_t> guest_output_submission_counter_{0};
};

}  // namespace metal
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_METAL_METAL_PRESENTER_H_
