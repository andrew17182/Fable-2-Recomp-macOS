#include <algorithm>
#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_trace.h"
#include "rex/graphics/metal/metal_vsync_lock.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/ui/metal/metal_presenter.h"
#include "rex/ui/metal/metal_flash_detect.h"

#include <cstdio>
#include <cstdlib>
#include <time.h>
#include <cstring>
#include <string>
#include <utility>

#include "third_party/metal-cpp/Metal/Metal.hpp"

#include "rex/compat/autorelease_pool_mac.h"
#include "rex/cvar.h"
#include "rex/logging.h"
#include "rex/platform.h"
#include "rex/graphics/shaders/bytecode/metal/apply_gamma_pwl_cs.h"
#include "rex/graphics/shaders/bytecode/metal/apply_gamma_table_cs.h"
#include "rex/ui/metal/metal_provider.h"
#include "rex/ui/shaders/bytecode/metal/guest_output_bilinear_dither_ps.h"
#include "rex/ui/shaders/bytecode/metal/guest_output_bilinear_ps.h"
#include "rex/ui/shaders/bytecode/metal/guest_output_triangle_strip_rect_vs.h"

#if XE_PLATFORM_IOS
#import <UIKit/UIKit.h>
#include "rex/ui/ios/game/surface_ios.h"
#else
#import <Cocoa/Cocoa.h>
#include "rex/ui/surface_mac.h"
#endif

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>
#if !XE_PLATFORM_IOS && __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#define XE_METALFX_AVAILABLE 1
#else
#define XE_METALFX_AVAILABLE 0
#endif

DEFINE_bool(metal_presenter_force_10bpc, true,
            "Force RGB10A2 guest output for presenter (reduces gamma conversion cost).", "Metal");
DEFINE_bool(metal_presenter_use_metalfx, false,
            "Use MetalFX spatial scaling when upscaling guest output.", "Metal");
DEFINE_int32(metal_presenter_metalfx_color_processing, 0,
             "MetalFX color processing mode: 0=perceptual, 1=linear.", "Metal");
DEFINE_int32(metal_presenter_metalfx_scale_x, 0,
             "MetalFX scale factor X (1=1x, 2=2x, etc). 0 = Fit Window.", "Metal");
DEFINE_int32(metal_presenter_metalfx_scale_y, 0,
             "MetalFX scale factor Y (1=1x, 2=2x, etc). 0 = Fit Window.", "Metal");
DEFINE_bool(metal_presenter_use_backing_scale, false,
            "Use macOS backing scale (Retina) for CAMetalLayer drawable size. "
            "If false, drawable size equals logical window size (lower GPU cost, "
            "less sharp).",
            "Metal");
DEFINE_bool(metal_presenter_debug_markers, false,
            "Add Metal debug markers for presenter passes and conversions.", "Metal");
DEFINE_bool(metal_allow_tearing, false,
            "Allow CAMetalLayer presentations without display synchronization. "
            "Disable for consistent presentation pacing; enabling may cause "
            "tearing and redundant/skipped presentations.", "Metal");

namespace rex::graphics::metal::gpu_time_trace {
bool Enabled();  // metal_command_processor.cc (FABLE2_GPU_TIME_TRACE=1)
}  // namespace rex::graphics::metal::gpu_time_trace

namespace {
// Test sessions only (FABLE2_TEST_CONTROL_DIR set): FABLE2_TEST_DRAWABLE=WxH renders the
// presenter into a drawable of that pixel size instead of the window's, so a background test
// window can measure and capture (`capture screen`) the 4K fullscreen path without covering the
// screen. Core Animation scales the drawable down into the small window for display.
// docs/resolution-scale.md section 22.
bool TestDrawableOverride(uint32_t& width, uint32_t& height) {
  static const std::pair<uint32_t, uint32_t> size = [] {
    const char* control = std::getenv("FABLE2_TEST_CONTROL_DIR");
    const char* env = std::getenv("FABLE2_TEST_DRAWABLE");
    unsigned w = 0, h = 0;
    if (!control || !*control || !env || std::sscanf(env, "%ux%u", &w, &h) != 2 || w < 64 ||
        h < 64 || w > 8192 || h > 8192) {
      if (env && *env) {
        REXLOG_WARN("[metalfx] FABLE2_TEST_DRAWABLE='{}' ignored (test sessions only, WxH)", env);
      }
      return std::pair<uint32_t, uint32_t>(0, 0);
    }
    REXLOG_INFO("[metalfx] test drawable override {}x{} (FABLE2_TEST_DRAWABLE)", w, h);
    return std::pair<uint32_t, uint32_t>(w, h);
  }();
  if (!size.first) return false;
  width = size.first;
  height = size.second;
  return true;
}

// Filter used for the last presented guest output: 0 none yet, 1 bilinear, 2 MetalFX spatial.
// One relaxed store per present; read by the F9 FPS counter (fable2_present_filter below).
std::atomic<int> g_present_filter{0};
}  // namespace

namespace rex {
namespace ui {
namespace metal {

MetalPresenter::MetalPresenter(MetalProvider* provider, HostGpuLossCallback host_gpu_loss_callback)
    : Presenter(host_gpu_loss_callback), provider_(provider) {
  device_ = provider_->GetDevice();
  guest_output_textures_.fill(nil);
  guest_output_submissions_.fill(0);
  guest_output_waited_submission_ = 0;
}

MetalPresenter::~MetalPresenter() { Shutdown(); }

bool MetalPresenter::Initialize() {
  // Use the shared MetalProvider command queue so presenter work is serialized
  // after GPU backend rendering and resolves.
  command_queue_ = (__bridge id<MTLCommandQueue>)provider_->GetCommandQueue();
  if (!command_queue_) {
    REXLOG_ERROR("Metal presenter failed to get command queue");
    return false;
  }

  id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device_;
  if (!mtl_device) {
    REXLOG_ERROR("Metal presenter failed to get Metal device");
    return false;
  }
  shared_event_ = [mtl_device newSharedEvent];
  if (!shared_event_) {
    REXLOG_ERROR("Metal presenter requires MTLSharedEvent for D3D12 parity");
    return false;
  }

  REXLOG_INFO("Metal presenter initialized with shared command queue");
  return true;
}

void MetalPresenter::Shutdown() {
  uint64_t last_submission = guest_output_submission_counter_.load(std::memory_order_acquire);
  if (shared_event_ && last_submission) {
    [(id<MTLSharedEvent>)shared_event_ waitUntilSignaledValue:last_submission timeoutMS:UINT64_MAX];
  }
  ReleaseCachedPresenterTextureViews();
  if (command_queue_) {
    command_queue_ = nullptr;
  }
  if (shared_event_) {
    [shared_event_ release];
    shared_event_ = nullptr;
  }
  if (copy_texture_convert_pipeline_2d_) {
    [copy_texture_convert_pipeline_2d_ release];
    copy_texture_convert_pipeline_2d_ = nullptr;
  }
  if (copy_texture_convert_pipeline_2d_array_) {
    [copy_texture_convert_pipeline_2d_array_ release];
    copy_texture_convert_pipeline_2d_array_ = nullptr;
  }
  if (apply_gamma_table_pipeline_) {
    [apply_gamma_table_pipeline_ release];
    apply_gamma_table_pipeline_ = nullptr;
  }
  if (apply_gamma_pwl_pipeline_) {
    [apply_gamma_pwl_pipeline_ release];
    apply_gamma_pwl_pipeline_ = nullptr;
  }
  if (gamma_output_texture_) {
    [gamma_output_texture_ release];
    gamma_output_texture_ = nullptr;
  }
  gamma_output_width_ = 0;
  gamma_output_height_ = 0;
  if (gamma_ramp_table_texture_) {
    [gamma_ramp_table_texture_ release];
    gamma_ramp_table_texture_ = nullptr;
  }
  if (gamma_ramp_pwl_texture_) {
    [gamma_ramp_pwl_texture_ release];
    gamma_ramp_pwl_texture_ = nullptr;
  }
  if (gamma_ramp_buffer_) {
    [gamma_ramp_buffer_ release];
    gamma_ramp_buffer_ = nullptr;
  }
  gamma_ramp_buffer_size_ = 0;
  gamma_ramp_table_valid_ = false;
  gamma_ramp_pwl_valid_ = false;
  if (guest_output_pipeline_bilinear_) {
    [guest_output_pipeline_bilinear_ release];
    guest_output_pipeline_bilinear_ = nullptr;
  }
  if (guest_output_pipeline_bilinear_dither_) {
    [guest_output_pipeline_bilinear_dither_ release];
    guest_output_pipeline_bilinear_dither_ = nullptr;
  }
  if (guest_output_sampler_) {
    [guest_output_sampler_ release];
    guest_output_sampler_ = nullptr;
  }
  guest_output_pipeline_format_ = 0;
  if (metalfx_scaler_) {
    [metalfx_scaler_ release];
    metalfx_scaler_ = nullptr;
  }
  if (metalfx_output_texture_) {
    [metalfx_output_texture_ release];
    metalfx_output_texture_ = nullptr;
  }
  for (id& guest_output_texture : guest_output_textures_) {
    if (guest_output_texture) {
      [guest_output_texture release];
      guest_output_texture = nil;
    }
  }
  guest_output_submissions_.fill(0);
  guest_output_waited_submission_ = 0;
  last_guest_output_mailbox_index_.store(UINT32_MAX, std::memory_order_relaxed);
  guest_output_submission_counter_.store(0, std::memory_order_relaxed);
  metalfx_input_width_ = 0;
  metalfx_input_height_ = 0;
  metalfx_output_width_ = 0;
  metalfx_output_height_ = 0;
  metalfx_color_format_ = 0;
  metalfx_color_processing_mode_ = 0;
  surface_scale_ = 1.0f;
  surface_width_in_points_ = 0;
  surface_height_in_points_ = 0;
  metal_layer_ = nullptr;
  REXLOG_DEBUG("Metal presenter shut down");
}

void MetalPresenter::ReleaseCachedPresenterTextureView(
    PresenterTextureViewCacheEntry& entry) {
  if (entry.view) {
    entry.view->release();
  }
  entry = {};
}

void MetalPresenter::ReleaseCachedPresenterTextureViews() {
  ReleaseCachedPresenterTextureView(linear_presenter_view_);
  ReleaseCachedPresenterTextureView(array_presenter_view_);
  ReleaseCachedPresenterTextureView(swizzle_presenter_view_);
}

MTL::Texture* MetalPresenter::GetCachedPresenterPixelFormatView(
    PresenterTextureViewCacheEntry& entry, MTL::Texture* parent,
    MTL::PixelFormat pixel_format) {
  if (!parent) {
    return nullptr;
  }
  if (entry.view && entry.parent == parent && !entry.full_descriptor &&
      entry.pixel_format == pixel_format) {
    return entry.view;
  }
  ReleaseCachedPresenterTextureView(entry);
  MTL::Texture* view = parent->newTextureView(pixel_format);
  if (!view) {
    return nullptr;
  }
  entry.parent = parent;
  entry.view = view;
  entry.full_descriptor = false;
  entry.pixel_format = pixel_format;
  return view;
}

MTL::Texture* MetalPresenter::GetCachedPresenterTextureView(
    PresenterTextureViewCacheEntry& entry, MTL::Texture* parent,
    MTL::PixelFormat pixel_format, MTL::TextureType texture_type,
    NS::Range level_range, NS::Range slice_range,
    MTL::TextureSwizzleChannels swizzle) {
  if (!parent) {
    return nullptr;
  }
  if (entry.view && entry.parent == parent && entry.full_descriptor &&
      entry.pixel_format == pixel_format && entry.texture_type == texture_type &&
      entry.level_location == level_range.location &&
      entry.level_length == level_range.length &&
      entry.slice_location == slice_range.location &&
      entry.slice_length == slice_range.length &&
      entry.swizzle.red == swizzle.red && entry.swizzle.green == swizzle.green &&
      entry.swizzle.blue == swizzle.blue &&
      entry.swizzle.alpha == swizzle.alpha) {
    return entry.view;
  }
  ReleaseCachedPresenterTextureView(entry);
  MTL::Texture* view =
      parent->newTextureView(pixel_format, texture_type, level_range,
                             slice_range, swizzle);
  if (!view) {
    return nullptr;
  }
  entry.parent = parent;
  entry.view = view;
  entry.full_descriptor = true;
  entry.pixel_format = pixel_format;
  entry.texture_type = texture_type;
  entry.level_location = level_range.location;
  entry.level_length = level_range.length;
  entry.slice_location = slice_range.location;
  entry.slice_length = slice_range.length;
  entry.swizzle = swizzle;
  return view;
}

Surface::TypeFlags MetalPresenter::GetSupportedSurfaceTypes() const {
#if XE_PLATFORM_IOS
  return Surface::kTypeFlag_iOSUIView;
#else
  return Surface::kTypeFlag_CAMetalLayer;
#endif
}

bool MetalPresenter::CaptureGuestOutput(RawImage& image_out) {
  return CaptureGuestOutput(image_out, nullptr);
}

bool MetalPresenter::CaptureGuestOutput(RawImage& image_out, CaptureTiming* timing) {
  uint32_t guest_output_mailbox_index;
  // Keep the acquired mailbox exclusively owned through readback and metadata.
  std::unique_lock<std::mutex> guest_output_consumer_lock(
      ConsumeGuestOutput(guest_output_mailbox_index, nullptr, nullptr));
  if (guest_output_mailbox_index == UINT32_MAX) {
    return false;
  }

  id<MTLTexture> guest_output_texture = guest_output_textures_[guest_output_mailbox_index];
  if (!guest_output_texture) {
    REXLOG_ERROR("Metal CaptureGuestOutput: Guest output texture is null at mailbox index {}",
           guest_output_mailbox_index);
    return false;
  }

  if (timing) {
    timing->frame_submitted_monotonic_us = guest_output_submitted_us_[guest_output_mailbox_index];
    timing->frame_id = guest_output_frame_ids_[guest_output_mailbox_index];
    timing->submission_id = guest_output_submissions_[guest_output_mailbox_index];
  }
  uint32_t width = static_cast<uint32_t>(guest_output_texture.width);
  uint32_t height = static_cast<uint32_t>(guest_output_texture.height);
  size_t stride = width * 4;  // 4 bytes per pixel

  REXLOG_DEBUG("Metal CaptureGuestOutput: Reading real texture data {}x{} from mailbox index {}", width,
         height, guest_output_mailbox_index);

  image_out.width = width;
  image_out.height = height;
  image_out.stride = stride;
  image_out.data.resize(height * stride);

  MTLResourceOptions options = MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
  id<MTLBuffer> readback_buffer =
      [(__bridge id<MTLDevice>)device_ newBufferWithLength:height * stride options:options];

  if (!readback_buffer) {
    REXLOG_ERROR("Metal CaptureGuestOutput: Failed to create readback buffer");
    return false;
  }

  id<MTLCommandBuffer> copy_command_buffer = [command_queue_ commandBuffer];
  if (!copy_command_buffer) {
    REXLOG_ERROR("Metal CaptureGuestOutput: Failed to create command buffer for texture readback");
    [readback_buffer release];
    return false;
  }

  id<MTLBlitCommandEncoder> blit_encoder = [copy_command_buffer blitCommandEncoder];
  if (!blit_encoder) {
    REXLOG_ERROR("Metal CaptureGuestOutput: Failed to create blit encoder for texture readback");
    [readback_buffer release];
    return false;
  }

  [blit_encoder copyFromTexture:guest_output_texture
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(width, height, 1)
                       toBuffer:readback_buffer
              destinationOffset:0
         destinationBytesPerRow:stride
       destinationBytesPerImage:height * stride];

  [blit_encoder endEncoding];

  [copy_command_buffer commit];
  [copy_command_buffer waitUntilCompleted];

  void* buffer_contents = [readback_buffer contents];
  if (!buffer_contents) {
    REXLOG_ERROR("Metal CaptureGuestOutput: Failed to get readback buffer contents");
    [readback_buffer release];
    return false;
  }

  std::memcpy(image_out.data.data(), buffer_contents, height * stride);

  [readback_buffer release];

  ConvertGuestOutputReadbackToRGBA8(image_out, guest_output_texture.pixelFormat);
  REXLOG_DEBUG("Metal CaptureGuestOutput: Successfully read real texture data from "
         "Metal (forced alpha=255)");
  return true;
}

namespace {
// Shared by CaptureGuestOutput and CaptureGuestOutputAtFrames.
void ConvertGuestOutputReadbackToRGBA8Impl(RawImage& image_out, MTLPixelFormat pixel_format) {
  const uint32_t width = image_out.width, height = image_out.height;
  // `stbi_write_png` expects RGBA8. Convert packed 10bpc and BGRA8 to RGBA8
  // and force alpha to 255 (matches D3D12/Vulkan behavior).
  uint8_t* pixel_data = image_out.data.data();
  size_t pixel_count = width * height;
  bool is_rgb10a2 = pixel_format == MTLPixelFormatRGB10A2Unorm
#ifdef MTLPixelFormatRGB10A2Unorm_sRGB
                    || pixel_format == MTLPixelFormatRGB10A2Unorm_sRGB
#endif
      ;
  bool is_bgr10a2 = false;
#ifdef MTLPixelFormatBGR10A2Unorm
  if (pixel_format == MTLPixelFormatBGR10A2Unorm) {
    is_bgr10a2 = true;
  }
#endif
#ifdef MTLPixelFormatBGR10A2Unorm_sRGB
  if (pixel_format == MTLPixelFormatBGR10A2Unorm_sRGB) {
    is_bgr10a2 = true;
  }
#endif
  if (is_rgb10a2 || is_bgr10a2) {
    auto to_8bpc = [](uint32_t value) -> uint8_t {
      return static_cast<uint8_t>((value * 255u + 511u) / 1023u);
    };
    std::vector<uint8_t> rgba8(pixel_count * 4);
    const uint32_t* packed = reinterpret_cast<const uint32_t*>(pixel_data);
    for (size_t i = 0; i < pixel_count; ++i) {
      uint32_t value = packed[i];
      uint32_t r = value & 0x3FFu;
      uint32_t g = (value >> 10) & 0x3FFu;
      uint32_t b = (value >> 20) & 0x3FFu;
      if (is_bgr10a2) {
        std::swap(r, b);
      }
      size_t out_offset = i * 4;
      rgba8[out_offset + 0] = to_8bpc(r);
      rgba8[out_offset + 1] = to_8bpc(g);
      rgba8[out_offset + 2] = to_8bpc(b);
      rgba8[out_offset + 3] = 255;
    }
    image_out.data.swap(rgba8);
  } else {
    bool needs_bgra_to_rgba =
        pixel_format == MTLPixelFormatBGRA8Unorm || pixel_format == MTLPixelFormatBGRA8Unorm_sRGB;
    for (size_t i = 0; i < pixel_count; ++i) {
      uint8_t* pixel = pixel_data + i * 4;
      if (needs_bgra_to_rgba) {
        std::swap(pixel[0], pixel[2]);
      }
      pixel[3] = 255;
    }
  }

}
}  // namespace

void MetalPresenter::ConvertGuestOutputReadbackToRGBA8(RawImage& image_out, uint32_t pixel_format) {
  ConvertGuestOutputReadbackToRGBA8Impl(image_out, static_cast<MTLPixelFormat>(pixel_format));
}

struct MetalPresenter::FrameCaptureRequest {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<uint64_t> targets;  // ascending
  size_t next = 0;                // GPU thread: the next target to read back
  int outstanding = 0;            // readbacks encoded, not completed yet
  bool failed = false;
  std::vector<FrameCapture> results;
  bool Done() const { return next == targets.size() && outstanding == 0; }
};

int MetalPresenter::CaptureGuestOutputAtFrames(const std::vector<uint64_t>& target_frames, uint32_t timeout_ms,
                                               std::vector<FrameCapture>& captures_out,
                                               uint64_t* current_frame_out) {
  if (target_frames.empty()) return -1;
  auto request = std::make_shared<FrameCaptureRequest>();
  request->targets = target_frames;
  std::sort(request->targets.begin(), request->targets.end());
  request->results.resize(request->targets.size());
  {
    std::lock_guard<std::mutex> lock(frame_capture_mutex_);
    const uint64_t seen = last_refreshed_frame_.load(std::memory_order_acquire);
    if (current_frame_out) *current_frame_out = seen;
    // One request at a time; never a later frame than asked for.
    if (frame_capture_ || (seen && seen >= request->targets.front())) return -1;
    frame_capture_ = request;
    frame_capture_pending_.store(true, std::memory_order_release);
  }
  std::unique_lock<std::mutex> wait_lock(request->mutex);
  const bool finished = request->cv.wait_for(wait_lock, std::chrono::milliseconds(timeout_ms),
                                             [&] { return request->Done(); });
  {
    std::lock_guard<std::mutex> lock(frame_capture_mutex_);
    if (frame_capture_ == request) {
      frame_capture_.reset();
      frame_capture_pending_.store(false, std::memory_order_release);
    }
  }
  if (current_frame_out) *current_frame_out = last_refreshed_frame_.load(std::memory_order_acquire);
  // Whatever completed (in-flight readbacks keep the request alive through their handler's reference).
  captures_out.clear();
  for (size_t i = 0; i < request->next && i < request->results.size(); ++i) {
    FrameCapture& c = request->results[i];
    if (!c.ok) continue;
    ConvertGuestOutputReadbackToRGBA8(c.image, c.pixel_format);
    captures_out.push_back(std::move(c));
  }
  wait_lock.unlock();
  if (!finished) return 0;
  return captures_out.size() == request->targets.size() ? 1 : -1;
}

struct MetalPresenter::ScreenCaptureRequest {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false, ok = false;
  RawImage image;
  uint64_t frame_id = 0;
};

bool MetalPresenter::CaptureScreen(RawImage& image_out, uint32_t timeout_ms, uint64_t* frame_id_out) {
  auto request = std::make_shared<ScreenCaptureRequest>();
  {
    std::lock_guard<std::mutex> lock(screen_capture_mutex_);
    if (screen_capture_) return false;  // one at a time
    screen_capture_ = request;
  }
  screen_capture_pending_.store(true, std::memory_order_release);
  bool ok = false;
  {
    std::unique_lock<std::mutex> lock(request->mutex);
    request->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return request->done; });
    ok = request->done && request->ok;
    if (ok) {
      image_out = std::move(request->image);
      if (frame_id_out) *frame_id_out = request->frame_id;
    }
  }
  {
    std::lock_guard<std::mutex> lock(screen_capture_mutex_);
    screen_capture_.reset();
  }
  screen_capture_pending_.store(false, std::memory_order_release);
  return ok;
}

// Present queue depth (docs/macos-metal.md "Present queue", 2026-10-22). With display sync on, the game renders
// faster than the display takes frames, so CAMetalLayer's queue stays full: with the default 3 drawables every
// frame waits behind two older ones (55-61 ms swap->photon at 60 FPS). 2 drawables keep at most one frame
// waiting (41-45 ms) with the same pacing. FABLE2_PRESENT_DRAWABLES: unset/2 = 2 (fix, default on);
// 3 or 0 = 3 (old behaviour; 0 is the launcher's "off"). Read once (static).
namespace {
NSUInteger PresentDrawableCount() {
  static const NSUInteger count = [] {
    const char* v = std::getenv("FABLE2_PRESENT_DRAWABLES");  // getenv-ok: cached in a static
    const bool old = v && (std::strcmp(v, "3") == 0 || std::strcmp(v, "0") == 0);
    const NSUInteger n = old ? 3 : 2;
    REXLOG_INFO("[present] maximumDrawableCount = {} (FABLE2_PRESENT_DRAWABLES={})", uint32_t(n),
                v ? v : "unset");
    return n;
  }();
  return count;
}
}  // namespace

// Vsync phase lock frame id of a guest output: the frame recorder's id, or without the recorder the guest output
// submission number (distinct per refreshed output).
uint64_t MetalPresenter::VsyncLockFrameId(uint32_t mailbox_index) const {
  const uint64_t id = guest_output_frame_ids_[mailbox_index];
  return id ? id : (guest_output_submissions_[mailbox_index] | (uint64_t(1) << 63));
}

Presenter::PaintResult MetalPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  if (!metal_layer_) {
    REXLOG_WARN("Metal PaintAndPresentImpl called without connected surface");
    return PaintResult::kNotPresented;
  }

  if (!command_queue_) {
    REXLOG_WARN("Metal PaintAndPresentImpl called without command queue");
    return PaintResult::kNotPresented;
  }

  XE_SCOPED_AUTORELEASE_POOL("MetalPresenter::PaintAndPresentImpl");

  if (surface_width_in_points_ && surface_height_in_points_) {
    CGFloat drawable_width = CGFloat(surface_width_in_points_) * surface_scale_;
    CGFloat drawable_height = CGFloat(surface_height_in_points_) * surface_scale_;
    if (drawable_width < 1.0) {
      drawable_width = 1.0;
    }
    if (drawable_height < 1.0) {
      drawable_height = 1.0;
    }
    uint32_t test_width = 0, test_height = 0;
    if (TestDrawableOverride(test_width, test_height)) {
      drawable_width = CGFloat(test_width);
      drawable_height = CGFloat(test_height);
    }
    CGSize drawable_size = CGSizeMake(drawable_width, drawable_height);
    if (!CGSizeEqualToSize(metal_layer_.drawableSize, drawable_size)) {
      metal_layer_.drawableSize = drawable_size;
    }
    if (metal_layer_.contentsScale != layer_contents_scale_) {
      metal_layer_.contentsScale = layer_contents_scale_;
    }
  }

  // Screen capture: drawables must be readable (framebufferOnly = NO) while a request is pending.
  const bool screen_capture = screen_capture_pending_.load(std::memory_order_acquire);
  if (screen_capture && metal_layer_.framebufferOnly) {
    metal_layer_.framebufferOnly = NO;
    screen_capture_restore_fbo_ = true;
  } else if (!screen_capture && screen_capture_restore_fbo_) {
    metal_layer_.framebufferOnly = YES;
    screen_capture_restore_fbo_ = false;
  }
  // Vsync phase lock (metal_vsync_lock.h): drawable acquisition time and the nextDrawable wait of each paint.
  const bool vsync_lock_on = graphics::metal::vsync_lock::Enabled();
  const double vsync_lock_paint_start = vsync_lock_on ? graphics::metal::vsync_lock::NowSeconds() : 0.0;
  // nextDrawable blocks while every drawable is queued for the display; the wait is logged as drawable_wait
  // (frame CSV + recorder) so the UI-thread cost of the smaller queue is visible.
  const bool log_drawable_wait = graphics::metal::frame_log::Enabled();
  const auto drawable_wait_t0 =
      log_drawable_wait ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  id<CAMetalDrawable> drawable = [metal_layer_ nextDrawable];
  const double vsync_lock_drawable_wait =
      vsync_lock_on ? graphics::metal::vsync_lock::NowSeconds() - vsync_lock_paint_start : 0.0;
  if (log_drawable_wait) {
    graphics::metal::frame_log::Record(
        "drawable_wait", graphics::metal::frame_log::CurrentFrame(),
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                       drawable_wait_t0)
                     .count()));
  }
  if (!drawable) {
    REXLOG_WARN("Metal PaintAndPresentImpl failed to get drawable");
    return PaintResult::kNotPresented;
  }
  graphics::metal::trace::Paint();
  id<MTLTexture> drawable_texture = drawable.texture;
  if (!drawable_texture) {
    REXLOG_WARN("Metal PaintAndPresentImpl failed to get drawable texture");
    return PaintResult::kNotPresented;
  }

  id<MTLCommandBuffer> command_buffer = [command_queue_ commandBuffer];
  if (REXCVAR_GET(metal_presenter_debug_markers) && command_buffer) {
    command_buffer.label = @"XeniaPresenter";
  }

  // Draw the guest output to the drawable (bilinear/dither only for now).
  uint64_t measured_frame_id = 0;
  uint32_t guest_output_mailbox_index = UINT32_MAX;
  GuestOutputProperties guest_output_properties;
  GuestOutputPaintConfig guest_output_paint_config;
  id<MTLTexture> guest_output_texture = nil;
  {
    std::unique_lock<std::mutex> guest_output_consumer_lock(ConsumeGuestOutput(
        guest_output_mailbox_index, &guest_output_properties, &guest_output_paint_config));
    if (guest_output_mailbox_index != UINT32_MAX) {
      measured_frame_id = guest_output_frame_ids_[guest_output_mailbox_index];
      uint64_t await_submission = guest_output_submissions_[guest_output_mailbox_index];
      // The presenter uses one serialized Metal queue. Once a wait for
      // submission N has been encoded in this queue, later presenter command
      // buffers don't need to re-check/re-encode waits for <= N.
      if (await_submission > guest_output_waited_submission_ && shared_event_) {
        id<MTLSharedEvent> shared_event = (id<MTLSharedEvent>)shared_event_;
        uint64_t completed_submission = [shared_event signaledValue];
        if (await_submission > completed_submission) {
          [command_buffer encodeWaitForEvent:shared_event value:await_submission];
        }
        guest_output_waited_submission_ = await_submission;
      }
      guest_output_texture = guest_output_textures_[guest_output_mailbox_index];
    }
  }

  uint32_t drawable_width = static_cast<uint32_t>(drawable_texture.width);
  uint32_t drawable_height = static_cast<uint32_t>(drawable_texture.height);

  struct GuestOutputPaintConstants {
    float rect_offset[2];
    float rect_size[2];
    int32_t output_offset[2];
    float output_size_inv[2];
  };
  struct GuestOutputPaintConstantsFragment {
    int32_t output_offset[2];
    float output_size_inv[2];
  };

  GuestOutputPaintConstants constants = {};
  GuestOutputPaintConstantsFragment fragment_constants = {};
  bool draw_guest_output = false;
  bool use_dither = false;
  id<MTLTexture> paint_texture = nil;

  if (guest_output_texture) {
    GuestOutputPaintFlow guest_output_flow =
        GetGuestOutputPaintFlow(guest_output_properties, drawable_width, drawable_height,
                                drawable_width, drawable_height, guest_output_paint_config);
    if (guest_output_flow.effect_count &&
        EnsureGuestOutputPaintResources(uint32_t(drawable_texture.pixelFormat))) {
      size_t effect_index = guest_output_flow.effect_count - 1;
      GuestOutputPaintEffect effect = guest_output_flow.effects[effect_index];
      use_dither = effect == GuestOutputPaintEffect::kBilinearDither;
      if (effect != GuestOutputPaintEffect::kBilinear &&
          effect != GuestOutputPaintEffect::kBilinearDither) {
        static bool logged_unsupported_effect = false;
        if (!logged_unsupported_effect) {
          logged_unsupported_effect = true;
          REXLOG_WARN("Metal presenter: guest output effect {} not supported; "
                 "falling back to bilinear",
                 static_cast<int>(effect));
        }
        use_dither = false;
        effect_index = guest_output_flow.effect_count - 1;
      }

      int32_t output_x = 0;
      int32_t output_y = 0;
      guest_output_flow.GetEffectOutputOffset(effect_index, output_x, output_y);
      const std::pair<uint32_t, uint32_t>& output_size =
          guest_output_flow.effect_output_sizes[effect_index];

      float x_to_ndc = 2.0f / float(drawable_width);
      float y_to_ndc = 2.0f / float(drawable_height);
      constants.rect_offset[0] = -1.0f + float(output_x) * x_to_ndc;
      constants.rect_offset[1] = 1.0f - float(output_y) * y_to_ndc;
      constants.rect_size[0] = float(output_size.first) * x_to_ndc;
      constants.rect_size[1] = -float(output_size.second) * y_to_ndc;

      BilinearConstants bilinear_constants;
      bilinear_constants.Initialize(guest_output_flow, effect_index);
      constants.output_offset[0] = bilinear_constants.output_offset[0];
      constants.output_offset[1] = bilinear_constants.output_offset[1];
      constants.output_size_inv[0] = bilinear_constants.output_size_inv[0];
      constants.output_size_inv[1] = bilinear_constants.output_size_inv[1];
      fragment_constants.output_offset[0] = bilinear_constants.output_offset[0];
      fragment_constants.output_offset[1] = bilinear_constants.output_offset[1];
      fragment_constants.output_size_inv[0] = bilinear_constants.output_size_inv[0];
      fragment_constants.output_size_inv[1] = bilinear_constants.output_size_inv[1];

      paint_texture = guest_output_texture;
      bool metalfx_applied = false;
#if XE_METALFX_AVAILABLE
      if (REXCVAR_GET(metal_presenter_use_metalfx)) {
        if (@available(macOS 13.0, *)) {
          id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device_;
          if (mtl_device && [MTLFXSpatialScalerDescriptor supportsDevice:mtl_device]) {
            uint32_t input_width = guest_output_properties.frontbuffer_width;
            uint32_t input_height = guest_output_properties.frontbuffer_height;

            // Allow manual override of MetalFX output size
            uint32_t target_width = output_size.first;
            uint32_t target_height = output_size.second;

            if (REXCVAR_GET(metal_presenter_metalfx_scale_x) > 0) {
              target_width = input_width * REXCVAR_GET(metal_presenter_metalfx_scale_x);
            }
            if (REXCVAR_GET(metal_presenter_metalfx_scale_y) > 0) {
              target_height = input_height * REXCVAR_GET(metal_presenter_metalfx_scale_y);
            }

            // MetalFX spatial only upscales: both axes must be at least the input and one larger
            // (the aspect-fit rect keeps 16:9, so in practice both grow together).
            const bool upscale = target_width >= input_width && target_height >= input_height &&
                                 (target_width > input_width || target_height > input_height);
            if (!upscale) {
              // Logged once per size change: MetalFX only upscales.
              static uint64_t logged_sizes = 0;
              const uint64_t sizes = (uint64_t(input_width) << 48) |
                                     (uint64_t(input_height) << 32) |
                                     (uint64_t(target_width) << 16) | target_height;
              if (sizes != logged_sizes) {
                logged_sizes = sizes;
                REXLOG_INFO("[metalfx] not applied: output {}x{} is not larger than the "
                            "internal {}x{}; bilinear",
                            target_width, target_height, input_width, input_height);
              }
            }
            if (upscale) {
              MTLPixelFormat color_format = guest_output_texture.pixelFormat;
              int color_processing_mode = REXCVAR_GET(metal_presenter_metalfx_color_processing);
              if (color_processing_mode < 0) {
                color_processing_mode = 0;
              } else if (color_processing_mode > 1) {
                color_processing_mode = 1;
              }
              bool recreate = !metalfx_scaler_ || !metalfx_output_texture_ ||
                              metalfx_input_width_ != input_width ||
                              metalfx_input_height_ != input_height ||
                              metalfx_output_width_ != target_width ||
                              metalfx_output_height_ != target_height ||
                              metalfx_color_format_ != uint32_t(color_format) ||
                              metalfx_color_processing_mode_ != uint32_t(color_processing_mode);
              if (recreate) {
                if (metalfx_scaler_) {
                  [metalfx_scaler_ release];
                  metalfx_scaler_ = nullptr;
                }
                if (metalfx_output_texture_) {
                  [metalfx_output_texture_ release];
                  metalfx_output_texture_ = nullptr;
                }
                metalfx_input_width_ = input_width;
                metalfx_input_height_ = input_height;
                metalfx_output_width_ = target_width;
                metalfx_output_height_ = target_height;
                metalfx_color_format_ = uint32_t(color_format);
                metalfx_color_processing_mode_ = uint32_t(color_processing_mode);
                MTLFXSpatialScalerDescriptor* desc = [MTLFXSpatialScalerDescriptor new];
                desc.inputWidth = input_width;
                desc.inputHeight = input_height;
                desc.outputWidth = target_width;
                desc.outputHeight = target_height;
                desc.colorTextureFormat = color_format;
                desc.outputTextureFormat = color_format;
                desc.colorProcessingMode = color_processing_mode == 1
                                               ? MTLFXSpatialScalerColorProcessingModeLinear
                                               : MTLFXSpatialScalerColorProcessingModePerceptual;
                id<MTLFXSpatialScaler> scaler = [desc newSpatialScalerWithDevice:mtl_device];
                [desc release];
                REXLOG_INFO("[metalfx] spatial scaler {}x{} -> {}x{} ({})", input_width,
                            input_height, target_width, target_height,
                            scaler ? "created" : "creation failed; bilinear");
                if (scaler) {
                  metalfx_scaler_ = scaler;
                  MTLTextureDescriptor* output_desc =
                      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:color_format
                                                                         width:target_width
                                                                        height:target_height
                                                                     mipmapped:NO];
                  output_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
                                      MTLTextureUsageShaderWrite;
                  output_desc.storageMode = MTLStorageModePrivate;
                  metalfx_output_texture_ = [mtl_device newTextureWithDescriptor:output_desc];
                }
              }
              if (metalfx_scaler_ && metalfx_output_texture_) {
                if (REXCVAR_GET(metal_presenter_debug_markers)) {
                  [command_buffer pushDebugGroup:@"MetalFX"];
                }
                id<MTLFXSpatialScaler> scaler = (id<MTLFXSpatialScaler>)metalfx_scaler_;
                scaler.colorTexture = guest_output_texture;
                scaler.outputTexture = metalfx_output_texture_;
                scaler.inputContentWidth = input_width;
                scaler.inputContentHeight = input_height;
                [scaler encodeToCommandBuffer:command_buffer];
                paint_texture = metalfx_output_texture_;
                metalfx_applied = true;
                if (REXCVAR_GET(metal_presenter_debug_markers)) {
                  [command_buffer popDebugGroup];
                }
              }
            }
          }
        }
      }
#endif
      if (g_present_filter.load(std::memory_order_relaxed) != (metalfx_applied ? 2 : 1)) {
        g_present_filter.store(metalfx_applied ? 2 : 1, std::memory_order_relaxed);
      }
      draw_guest_output = true;
    }
  }

  MTLRenderPassDescriptor* render_pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
  render_pass_desc.colorAttachments[0].texture = drawable_texture;
  render_pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
  render_pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
  render_pass_desc.colorAttachments[0].clearColor =
      MTLClearColorMake(0.0, 0.0, 0.0, 1.0);  // Black background

  id<MTLRenderCommandEncoder> render_encoder =
      [command_buffer renderCommandEncoderWithDescriptor:render_pass_desc];
  if (!render_encoder) {
    REXLOG_WARN("Metal PaintAndPresentImpl failed to create render encoder");
    return PaintResult::kNotPresented;
  }
  if (REXCVAR_GET(metal_presenter_debug_markers)) {
    render_encoder.label = @"XeniaPresenterRender";
  }

  if (draw_guest_output && paint_texture) {
    if (REXCVAR_GET(metal_presenter_debug_markers)) {
      [render_encoder pushDebugGroup:@"GuestOutput"];
    }
    MTLViewport viewport;
    viewport.originX = 0.0;
    viewport.originY = 0.0;
    viewport.width = double(drawable_width);
    viewport.height = double(drawable_height);
    viewport.znear = 0.0;
    viewport.zfar = 1.0;
    [render_encoder setViewport:viewport];

    MTLScissorRect scissor;
    scissor.x = 0;
    scissor.y = 0;
    scissor.width = drawable_width;
    scissor.height = drawable_height;
    [render_encoder setScissorRect:scissor];

    [render_encoder setRenderPipelineState:use_dither ? guest_output_pipeline_bilinear_dither_
                                                      : guest_output_pipeline_bilinear_];
    [render_encoder setVertexBytes:&constants length:sizeof(constants) atIndex:0];
    [render_encoder setFragmentBytes:&fragment_constants
                              length:sizeof(fragment_constants)
                             atIndex:0];
    [render_encoder setFragmentTexture:paint_texture atIndex:0];
    if (guest_output_sampler_) {
      [render_encoder setFragmentSamplerState:guest_output_sampler_ atIndex:0];
    }
    [render_encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    if (REXCVAR_GET(metal_presenter_debug_markers)) {
      [render_encoder popDebugGroup];
    }
  }

  // Execute UI drawers if requested
  if (execute_ui_drawers) {
    if (REXCVAR_GET(metal_presenter_debug_markers)) {
      [render_encoder pushDebugGroup:@"UI"];
    }

    MetalUIDrawContext metal_ui_draw_context(*this, static_cast<uint32_t>(drawable_texture.width),
                                             static_cast<uint32_t>(drawable_texture.height),
                                             command_buffer, render_encoder);
    ExecuteUIDrawersFromUIThread(metal_ui_draw_context);

    if (REXCVAR_GET(metal_presenter_debug_markers)) {
      [render_encoder popDebugGroup];
    }
  }

  [render_encoder endEncoding];

  // Screen capture (after the ImGui overlay): blit the drawable into a shared buffer; the
  // completion handler converts BGRA to RGBA and wakes CaptureScreen.
  if (screen_capture && !drawable_texture.framebufferOnly) {
    std::shared_ptr<ScreenCaptureRequest> request;
    {
      std::lock_guard<std::mutex> lock(screen_capture_mutex_);
      request = screen_capture_;
    }
    bool claimed = false;
    if (request) {
      std::lock_guard<std::mutex> lock(request->mutex);
      claimed = !request->done && request->frame_id == 0;
      if (claimed) request->frame_id = graphics::metal::frame_log::CurrentFrame() ? graphics::metal::frame_log::CurrentFrame() : 1;
    }
    const NSUInteger w = drawable_texture.width, h = drawable_texture.height;
    id<MTLBuffer> buffer = claimed ? [(__bridge id<MTLDevice>)device_ newBufferWithLength:w * h * 4
                                                                                    options:MTLResourceStorageModeShared]
                                   : nil;
    if (buffer) {
      id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
      [blit copyFromTexture:drawable_texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(w, h, 1) toBuffer:buffer destinationOffset:0
     destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
      [blit endEncoding];
      const bool bgra = drawable_texture.pixelFormat == MTLPixelFormatBGRA8Unorm ||
                        drawable_texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->ok = done.status == MTLCommandBufferStatusCompleted;
        if (request->ok) {
          request->image.width = uint32_t(w);
          request->image.height = uint32_t(h);
          request->image.stride = w * 4;
          request->image.data.resize(w * h * 4);
          std::memcpy(request->image.data.data(), [buffer contents], w * h * 4);
          for (size_t i = 0; i < size_t(w) * h; ++i) {
            uint8_t* px = request->image.data.data() + i * 4;
            if (bgra) std::swap(px[0], px[2]);
            px[3] = 255;
          }
        }
        [buffer release];
        request->done = true;
        request->cv.notify_all();
      }];
    } else if (request && claimed) {
      std::lock_guard<std::mutex> lock(request->mutex);
      request->done = true;
      request->cv.notify_all();
    }
  }

  if (vsync_lock_on) {
    // Frame ids as in RefreshGuestOutputImpl (VsyncLockFrameId). A repaint of the same output (UI) is not a new
    // frame, but its drawable wait still tells the lock where the UI paint loop wakes.
    static std::atomic<uint64_t> vsync_lock_last_frame{0};
    const bool has_frame = draw_guest_output && guest_output_mailbox_index != UINT32_MAX;
    const uint64_t frame = has_frame ? VsyncLockFrameId(guest_output_mailbox_index) : 0;
    const bool new_frame = frame && frame != vsync_lock_last_frame.exchange(frame, std::memory_order_relaxed);
    graphics::metal::vsync_lock::OnPaint(frame, new_frame, vsync_lock_paint_start + vsync_lock_drawable_wait,
                                         vsync_lock_drawable_wait);
    if (new_frame) {
      [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
        graphics::metal::vsync_lock::OnPresented(frame, shown.presentedTime);
      }];
      if (graphics::metal::vsync_lock::TraceEnabled()) {
        [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
          const double start = done.GPUStartTime, end = done.GPUEndTime;
          if (end > 0 && end >= start) {
            graphics::metal::frame_log::RecordUptime("vlock_gpu_end", frame, end, uint64_t((end - start) * 1e6));
          }
        }];
      }
    }
  }
  if (draw_guest_output && measured_frame_id && graphics::metal::frame_log::Enabled()) {
    const uint64_t frame_id = measured_frame_id;
    graphics::metal::frame_log::Record("present_submit", frame_id);
    [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
      graphics::metal::frame_log::Presented(frame_id, shown.presentedTime);
    }];
  }
  // Trace-only: the presenter command buffer's GPU time (MetalFX + the output pass + ImGui), as
  // gpu_present rows in the frame CSV (`f2diag gpu` present_ms). Same queue as the guest work.
  if (graphics::metal::gpu_time_trace::Enabled()) {
    const uint64_t frame = measured_frame_id;  // the guest frame shown (UI thread has no frame tag)
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
      const double start = done.GPUStartTime, end = done.GPUEndTime;
      if (start > 0 && end >= start) {
        graphics::metal::frame_log::RecordUptime("gpu_present", frame, start,
                                                 uint64_t((end - start) * 1e6));
      }
    }];
  }
  [command_buffer presentDrawable:drawable];
  [command_buffer commit];

  return PaintResult::kPresented;
}

Presenter::SurfacePaintConnectResult
MetalPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(Surface& new_surface,
                                                                uint32_t new_surface_width,
                                                                uint32_t new_surface_height,
                                                                bool was_paintable,
                                                                bool& is_vsync_implicit_out) {
  Surface::TypeIndex surface_type = new_surface.GetType();
  if (!(GetSupportedSurfaceTypes() & (Surface::TypeFlags(1) << surface_type))) {
    REXLOG_ERROR("Metal surface type not supported: {}", static_cast<int>(surface_type));
    return SurfacePaintConnectResult::kFailure;
  }

  // Obtain a CAMetalLayer from the platform surface.
  CAMetalLayer* metal_layer = nullptr;
  CGFloat surface_scale = 1.0;

#if XE_PLATFORM_IOS
  if (surface_type != Surface::kTypeIndex_iOSUIView) {
    REXLOG_ERROR("Metal presenter requires iOSUIView surface, got: {}", static_cast<int>(surface_type));
    return SurfacePaintConnectResult::kFailure;
  }
  iOSUIViewSurface& ios_surface = static_cast<iOSUIViewSurface&>(new_surface);
  metal_layer = ios_surface.GetOrCreateMetalLayer();
  // iOS intentionally presents to a fixed 720p-class drawable and lets the
  // UIView frame handle screen placement, Fit/Stretch/Zoom, and device scaling.
  surface_scale = 1.0;
#else
  if (surface_type != Surface::kTypeIndex_CAMetalLayer) {
    REXLOG_ERROR("Metal presenter requires MacNSView surface, got: {}", static_cast<int>(surface_type));
    return SurfacePaintConnectResult::kFailure;
  }
  // ReXGlue's SDL surface already supplies a Metal layer and physical pixels.
  auto& surface = static_cast<CAMetalLayerSurface&>(new_surface);
  metal_layer = (__bridge CAMetalLayer*)surface.layer();
  surface_scale = 1.0;
#endif

  if (!metal_layer) {
    REXLOG_ERROR("Metal presenter failed to get CAMetalLayer from surface");
    return SurfacePaintConnectResult::kFailure;
  }

  metal_layer.device = (__bridge id<MTLDevice>)device_;
  metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  if (surface_scale <= 0.0) {
    surface_scale = 1.0;
  }
#if !XE_PLATFORM_IOS
  if (!REXCVAR_GET(metal_presenter_use_backing_scale)) {
    surface_scale = 1.0;
  }
#endif
  surface_scale_ = static_cast<float>(surface_scale);
  surface_width_in_points_ = new_surface_width;
  surface_height_in_points_ = new_surface_height;
  // contentsScale follows the window's backing scale (Retina 2, else 1). On macOS the SDL surface
  // already reports pixels (SDL_GetWindowSizeInPixels), so the drawable is the full pixel size
  // either way; this only stops the presenter overwriting the scale SDL's Metal view sets.
  // FABLE2_RETINA=0 restores the old contentsScale = 1. The SDK reconnects on every pixel-size
  // change (resize, move to a display with another backing scale), so this is re-read then.
  CGFloat backing_scale = 1.0;
  bool retina = true;
#if !XE_PLATFORM_IOS
  if (const char* env = std::getenv("FABLE2_RETINA")) {  // getenv-ok: per surface connect (resize)
    retina = !(env[0] == '0' && env[1] == '\0');
  }
  {
    id delegate = metal_layer.delegate;
    NSWindow* ns_window = [delegate isKindOfClass:[NSView class]] ? [(NSView*)delegate window] : nil;
    if (ns_window) {
      backing_scale = ns_window.backingScaleFactor;
      if (graphics::metal::vsync_lock::Enabled()) {
        // Display refresh for the vsync phase lock: now, and whenever the window changes screen or the screen
        // configuration changes (refresh rate). Observers are added once; the game has one window.
        auto update_refresh = [ns_window] {
          NSScreen* screen = ns_window.screen ? ns_window.screen : [NSScreen mainScreen];
          graphics::metal::vsync_lock::SetDisplayRefreshHz(screen ? double(screen.maximumFramesPerSecond) : 60.0);
        };
        update_refresh();
        static bool vsync_lock_observing = false;
        if (!vsync_lock_observing) {
          vsync_lock_observing = true;
          NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
          [center addObserverForName:NSWindowDidChangeScreenNotification
                              object:ns_window
                               queue:[NSOperationQueue mainQueue]
                          usingBlock:^(NSNotification*) { update_refresh(); }];
          [center addObserverForName:NSApplicationDidChangeScreenParametersNotification
                              object:nil
                               queue:[NSOperationQueue mainQueue]
                          usingBlock:^(NSNotification*) { update_refresh(); }];
        }
      }
    } else if (metal_layer.bounds.size.width > 0.0) {
      backing_scale = CGFloat(new_surface_width) / metal_layer.bounds.size.width;
    }
    if (!(backing_scale > 0.0)) {
      backing_scale = 1.0;
    }
  }
#endif
  layer_contents_scale_ = retina ? static_cast<float>(backing_scale) : 1.0f;
  metal_layer.contentsScale = layer_contents_scale_;
  metal_layer.drawableSize =
      CGSizeMake(new_surface_width * surface_scale, new_surface_height * surface_scale);
  if (uint32_t test_width = 0, test_height = 0; TestDrawableOverride(test_width, test_height)) {
    metal_layer.drawableSize = CGSizeMake(test_width, test_height);
  }
  metal_layer.minificationFilter = kCAFilterNearest;
  metal_layer.magnificationFilter = kCAFilterNearest;

#if XE_PLATFORM_IOS
  is_vsync_implicit_out = true;
#else
  const bool tearing_allowed = REXCVAR_GET(metal_allow_tearing);
  metal_layer.displaySyncEnabled = tearing_allowed ? NO : YES;
  is_vsync_implicit_out = !tearing_allowed;
#endif
  metal_layer.maximumDrawableCount = PresentDrawableCount();

  metal_layer_ = metal_layer;

  REXLOG_INFO("Metal surface connected successfully: {}x{} px = {}x{} points (backing scale={}, "
              "contentsScale={}, extra drawable scale={}, drawable={}x{}) [retina] {} (FABLE2_RETINA={})",
              new_surface_width, new_surface_height,
              uint32_t(new_surface_width / backing_scale + 0.5),
              uint32_t(new_surface_height / backing_scale + 0.5), double(backing_scale),
              double(metal_layer.contentsScale), double(surface_scale),
              uint32_t(metal_layer.drawableSize.width), uint32_t(metal_layer.drawableSize.height),
              retina ? "on" : "off", std::getenv("FABLE2_RETINA") ? std::getenv("FABLE2_RETINA") : "unset");  // getenv-ok: per surface connect (resize)
  return SurfacePaintConnectResult::kSuccess;
}

void MetalPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  if (metal_layer_) {
    metal_layer_ = nullptr;
  }
  REXLOG_INFO("Metal surface disconnected successfully");
}

bool MetalPresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  // Validate mailbox index
  if (mailbox_index >= kGuestOutputMailboxSize) {
    REXLOG_ERROR("Metal RefreshGuestOutputImpl: Invalid mailbox index {}", mailbox_index);
    is_8bpc_out_ref = false;
    return false;
  }

  id<MTLTexture> guest_output_texture = guest_output_textures_[mailbox_index];
  MTLPixelFormat guest_output_format =
      REXCVAR_GET(metal_presenter_force_10bpc) ? MTLPixelFormatRGB10A2Unorm : MTLPixelFormatRGBA8Unorm;

  if (!guest_output_texture || guest_output_texture.width != frontbuffer_width ||
      guest_output_texture.height != frontbuffer_height ||
      guest_output_texture.pixelFormat != guest_output_format) {
    id<MTLTexture> previous_texture = guest_output_texture;
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:guest_output_format
                                                           width:frontbuffer_width
                                                          height:frontbuffer_height
                                                       mipmapped:NO];

    descriptor.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModePrivate;

    guest_output_texture = [(__bridge id<MTLDevice>)device_ newTextureWithDescriptor:descriptor];
    if (!guest_output_texture) {
      REXLOG_ERROR("Metal RefreshGuestOutputImpl: Failed to create guest output texture");
      is_8bpc_out_ref = false;
      return false;
    }

    if (previous_texture && previous_texture != guest_output_texture) {
      [previous_texture release];
    }

    guest_output_textures_[mailbox_index] = guest_output_texture;
  }

  MetalGuestOutputRefreshContext context(is_8bpc_out_ref, guest_output_texture);
  bool success = refresher(context);

  if (!success) {
    REXLOG_WARN("Metal RefreshGuestOutputImpl: Refresher callback failed");
    return false;
  }

  guest_output_textures_[mailbox_index] = guest_output_texture;
  last_guest_output_mailbox_index_.store(mailbox_index, std::memory_order_relaxed);
  guest_output_submissions_[mailbox_index] = context.submission_id();
  guest_output_frame_ids_[mailbox_index] = graphics::metal::frame_log::CurrentFrame();
  // CPU submission time; GPU completion and physical display time are distinct.
  timespec submitted{};
  clock_gettime(CLOCK_MONOTONIC, &submitted);
  guest_output_submitted_us_[mailbox_index] = int64_t(submitted.tv_sec) * 1000000 + submitted.tv_nsec / 1000;
  graphics::metal::frame_log::Record("guest_ready", guest_output_frame_ids_[mailbox_index]);
  if (graphics::metal::vsync_lock::Enabled()) {
    const uint64_t frame = VsyncLockFrameId(mailbox_index);
    graphics::metal::vsync_lock::OnGuestReady(frame, graphics::metal::vsync_lock::NowSeconds(),
                                              graphics::metal::vsync_lock::LastVblank());
    // The guest GPU work of this output finishes well after its CPU submission (about 10 ms measured): the lock
    // times that completion, so the presenter's pass never waits on it.
    const uint64_t submission = guest_output_submissions_[mailbox_index];
    if (shared_event_ && submission) {
      static MTLSharedEventListener* listener = [] {
        dispatch_queue_t queue = dispatch_queue_create("fable2.vsync-lock", DISPATCH_QUEUE_SERIAL);
        return [[MTLSharedEventListener alloc] initWithDispatchQueue:queue];
      }();
      [(id<MTLSharedEvent>)shared_event_ notifyListener:listener
                                                atValue:submission
                                                  block:^(id<MTLSharedEvent>, uint64_t) {
                                                    graphics::metal::vsync_lock::OnGuestDone(
                                                        frame, graphics::metal::vsync_lock::NowSeconds());
                                                  }];
    }
  }
  last_refreshed_frame_.store(guest_output_frame_ids_[mailbox_index], std::memory_order_release);
  if (frame_capture_pending_.load(std::memory_order_acquire)) {
    MaybeReadBackGuestOutputForFrameCapture(mailbox_index);
  }
  // Flash kit (FABLE2_FLASH_DETECT, metal_flash_detect.h): per-frame reduced readback while its window is open.
  if (flash::Enabled()) {
    flash::Detector::Get().OnRefresh((__bridge id<MTLDevice>)device_, (id<MTLCommandQueue>)command_queue_,
                                     guest_output_texture, guest_output_frame_ids_[mailbox_index]);
  }
  return true;
}

// Flash kit window (src/core/fable2_benchmark.h, via dlsym): 1 = open, 2 = close and write the results.
extern "C" __attribute__((visibility("default"), used)) int fable2_flash_control(int op) {
  return rex::ui::metal::flash::Detector::Get().Control(op);
}

// Upscaling filter of the last presented frame (src/core/fable2_fps_counter.h, via dlsym):
// 0 = nothing presented yet, 1 = bilinear, 2 = MetalFX spatial.
extern "C" __attribute__((visibility("default"), used)) int fable2_present_filter() {
  return g_present_filter.load(std::memory_order_relaxed);
}

// Test control `capture at <frame>`: on the GPU thread, right after the target frame's guest output copy was
// committed to command_queue_, encode its readback on the same queue (ordered after the copy). One small blit
// per requested frame; nothing happens while no request is pending.
void MetalPresenter::MaybeReadBackGuestOutputForFrameCapture(uint32_t mailbox_index) {
  std::shared_ptr<FrameCaptureRequest> request;
  {
    std::lock_guard<std::mutex> lock(frame_capture_mutex_);
    request = frame_capture_;
  }
  if (!request) return;
  const uint64_t frame = guest_output_frame_ids_[mailbox_index];
  size_t slot = 0;
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    if (request->next >= request->targets.size() || frame < request->targets[request->next]) return;
    // The first refresh at or after the target (frame_id says which; normally equal). A refresh satisfies
    // one target; later targets it already passed are read back from the following refreshes.
    slot = request->next++;
    ++request->outstanding;
  }
  auto finish = [request](size_t i, bool ok) {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->results[i].ok = ok;
    --request->outstanding;
    if (request->Done()) request->cv.notify_all();
  };
  id<MTLTexture> texture = guest_output_textures_[mailbox_index];
  if (!texture) return finish(slot, false);
  const uint32_t width = static_cast<uint32_t>(texture.width), height = static_cast<uint32_t>(texture.height);
  const size_t stride = size_t(width) * 4;
  id<MTLBuffer> buffer = [(__bridge id<MTLDevice>)device_ newBufferWithLength:stride * height
                                                                       options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> command_buffer = buffer ? [command_queue_ commandBuffer] : nil;
  id<MTLBlitCommandEncoder> blit = command_buffer ? [command_buffer blitCommandEncoder] : nil;
  if (!blit) {
    if (buffer) [buffer release];
    return finish(slot, false);
  }
  [blit copyFromTexture:texture
             sourceSlice:0
             sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(width, height, 1)
                toBuffer:buffer
       destinationOffset:0
  destinationBytesPerRow:stride
destinationBytesPerImage:stride * height];
  [blit endEncoding];
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    FrameCapture& c = request->results[slot];
    c.requested_frame = request->targets[slot];
    c.timing.frame_id = frame;
    c.timing.submission_id = guest_output_submissions_[mailbox_index];
    c.timing.frame_submitted_monotonic_us = guest_output_submitted_us_[mailbox_index];
    c.pixel_format = static_cast<uint32_t>(texture.pixelFormat);
  }
  [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    const bool ok = completed.status == MTLCommandBufferStatusCompleted;
    if (ok) {
      std::lock_guard<std::mutex> lock(request->mutex);
      RawImage& image = request->results[slot].image;
      image.width = width;
      image.height = height;
      image.stride = stride;
      image.data.resize(stride * height);
      std::memcpy(image.data.data(), [buffer contents], stride * height);
    }
    [buffer release];
    finish(slot, ok);
  }];
  [command_buffer commit];
}

bool MetalPresenter::UpdateGammaRamp(const void* table_data, size_t table_bytes,
                                     const void* pwl_data, size_t pwl_bytes) {
  if (!table_data || !pwl_data || !table_bytes || !pwl_bytes) {
    REXLOG_WARN("MetalPresenter::UpdateGammaRamp: missing gamma ramp data");
    gamma_ramp_table_valid_ = false;
    gamma_ramp_pwl_valid_ = false;
    return false;
  }
  id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device_;
  if (!mtl_device) {
    REXLOG_WARN("MetalPresenter::UpdateGammaRamp: no Metal device");
    return false;
  }

  size_t total_bytes = table_bytes + pwl_bytes;
  if (!gamma_ramp_buffer_ || gamma_ramp_buffer_size_ < total_bytes) {
    if (gamma_ramp_table_texture_) {
      [gamma_ramp_table_texture_ release];
      gamma_ramp_table_texture_ = nullptr;
    }
    if (gamma_ramp_pwl_texture_) {
      [gamma_ramp_pwl_texture_ release];
      gamma_ramp_pwl_texture_ = nullptr;
    }
    if (gamma_ramp_buffer_) {
      [gamma_ramp_buffer_ release];
      gamma_ramp_buffer_ = nullptr;
    }
    gamma_ramp_buffer_ = [mtl_device newBufferWithLength:total_bytes
                                                 options:MTLResourceStorageModeShared];
    if (!gamma_ramp_buffer_) {
      REXLOG_ERROR("MetalPresenter::UpdateGammaRamp: failed to allocate buffer");
      gamma_ramp_buffer_size_ = 0;
      gamma_ramp_table_texture_ = nullptr;
      gamma_ramp_pwl_texture_ = nullptr;
      gamma_ramp_table_valid_ = false;
      gamma_ramp_pwl_valid_ = false;
      return false;
    }
    gamma_ramp_buffer_size_ = static_cast<uint32_t>(total_bytes);
    gamma_ramp_table_texture_ = nullptr;
    gamma_ramp_pwl_texture_ = nullptr;
  }

  void* contents = [gamma_ramp_buffer_ contents];
  if (!contents) {
    REXLOG_ERROR("MetalPresenter::UpdateGammaRamp: gamma ramp buffer has no contents");
    gamma_ramp_table_valid_ = false;
    gamma_ramp_pwl_valid_ = false;
    return false;
  }
  std::memcpy(contents, table_data, table_bytes);
  std::memcpy(reinterpret_cast<uint8_t*>(contents) + table_bytes, pwl_data, pwl_bytes);

  if (!gamma_ramp_table_texture_) {
    constexpr uint32_t kGammaRampTableWidth = 256;
    constexpr NSUInteger kGammaRampTableBytesPerRow = kGammaRampTableWidth * sizeof(uint32_t);
    MTLTextureDescriptor* table_desc =
        [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm
                                                               width:kGammaRampTableWidth
                                                     resourceOptions:MTLResourceStorageModeShared
                                                               usage:MTLTextureUsageShaderRead];
    gamma_ramp_table_texture_ =
        [gamma_ramp_buffer_ newTextureWithDescriptor:table_desc
                                              offset:0
                                         bytesPerRow:kGammaRampTableBytesPerRow];
    if (!gamma_ramp_table_texture_) {
      REXLOG_ERROR("MetalPresenter::UpdateGammaRamp: failed to create table texture");
    }
  }
  if (!gamma_ramp_pwl_texture_) {
    constexpr uint32_t kGammaRampPwlWidth = 384;
    constexpr NSUInteger kGammaRampPwlBytesPerRow = kGammaRampPwlWidth * sizeof(uint32_t);
    MTLTextureDescriptor* pwl_desc =
        [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRG16Uint
                                                               width:kGammaRampPwlWidth
                                                     resourceOptions:MTLResourceStorageModeShared
                                                               usage:MTLTextureUsageShaderRead];
    gamma_ramp_pwl_texture_ =
        [gamma_ramp_buffer_ newTextureWithDescriptor:pwl_desc
                                              offset:table_bytes
                                         bytesPerRow:kGammaRampPwlBytesPerRow];
    if (!gamma_ramp_pwl_texture_) {
      REXLOG_ERROR("MetalPresenter::UpdateGammaRamp: failed to create PWL texture");
    }
  }

  gamma_ramp_table_valid_ = gamma_ramp_table_texture_ != nullptr;
  gamma_ramp_pwl_valid_ = gamma_ramp_pwl_texture_ != nullptr;
  return gamma_ramp_table_valid_ && gamma_ramp_pwl_valid_;
}

bool MetalPresenter::EnsureCopyTextureConvertPipelines() {
  if (copy_texture_convert_pipeline_2d_ && copy_texture_convert_pipeline_2d_array_) {
    return true;
  }

  id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device_;
  if (!mtl_device) {
    REXLOG_ERROR("MetalPresenter::EnsureCopyTextureConvertPipelines: No Metal device");
    return false;
  }

  static const char kCopyTextureConvertShaderSource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct CopyConstants {
  uint width;
  uint height;
  uint slice;
  uint flags;
};

constant uint kCopyFlagSwapRB = 1u;
constant uint kCopyFlagDecodeSRGB = 2u;
constant uint kCopyFlagEncodeSRGB = 4u;

float3 SrgbToLinear(float3 c) {
  float3 lo = c / 12.92;
  float3 hi = pow((c + 0.055) / 1.055, float3(2.4));
  return select(hi, lo, c <= 0.04045);
}

float3 LinearToSrgb(float3 c) {
  c = clamp(c, 0.0, 1.0);
  float3 lo = c * 12.92;
  float3 hi = 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055;
  return select(hi, lo, c <= 0.0031308);
}

kernel void xe_copy_texture_convert_2d(
    texture2d<float, access::read> src [[texture(0)]],
    texture2d<half, access::write> dst [[texture(1)]],
    constant CopyConstants& c [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= c.width || gid.y >= c.height) {
    return;
  }
  float4 v = src.read(gid);
  if (c.flags & kCopyFlagSwapRB) {
    v = v.bgra;
  }
  if (c.flags & kCopyFlagDecodeSRGB) {
    v.rgb = SrgbToLinear(v.rgb);
  }
  if (c.flags & kCopyFlagEncodeSRGB) {
    v.rgb = LinearToSrgb(v.rgb);
  }
  dst.write(half4(v), gid);
}

kernel void xe_copy_texture_convert_2d_array(
    texture2d_array<float, access::read> src [[texture(0)]],
    texture2d<half, access::write> dst [[texture(1)]],
    constant CopyConstants& c [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= c.width || gid.y >= c.height) {
    return;
  }
  float4 v = src.read(gid, c.slice);
  if (c.flags & kCopyFlagSwapRB) {
    v = v.bgra;
  }
  if (c.flags & kCopyFlagDecodeSRGB) {
    v.rgb = SrgbToLinear(v.rgb);
  }
  if (c.flags & kCopyFlagEncodeSRGB) {
    v.rgb = LinearToSrgb(v.rgb);
  }
  dst.write(half4(v), gid);
}
)METAL";

  NSString* source = [NSString stringWithUTF8String:kCopyTextureConvertShaderSource];
  NSError* error = nil;
  id<MTLLibrary> library = [mtl_device newLibraryWithSource:source options:nil error:&error];
  if (!library) {
    REXLOG_ERROR("MetalPresenter::EnsureCopyTextureConvertPipelines: Failed to compile "
           "library: {}",
           error ? error.localizedDescription.UTF8String : "unknown error");
    return false;
  }

  id<MTLFunction> function_2d = [library newFunctionWithName:@"xe_copy_texture_convert_2d"];
  if (!function_2d) {
    REXLOG_ERROR("MetalPresenter::EnsureCopyTextureConvertPipelines: Missing "
           "xe_copy_texture_convert_2d function");
    [library release];
    return false;
  }
  id<MTLFunction> function_2d_array =
      [library newFunctionWithName:@"xe_copy_texture_convert_2d_array"];
  if (!function_2d_array) {
    REXLOG_ERROR("MetalPresenter::EnsureCopyTextureConvertPipelines: Missing "
           "xe_copy_texture_convert_2d_array function");
    [function_2d release];
    [library release];
    return false;
  }

  id<MTLComputePipelineState> pipeline_2d =
      [mtl_device newComputePipelineStateWithFunction:function_2d error:&error];
  if (!pipeline_2d) {
    REXLOG_ERROR("MetalPresenter::EnsureCopyTextureConvertPipelines: Failed to create "
           "compute pipeline: {}",
           error ? error.localizedDescription.UTF8String : "unknown error");
    [function_2d_array release];
    [function_2d release];
    [library release];
    return false;
  }
  id<MTLComputePipelineState> pipeline_2d_array =
      [mtl_device newComputePipelineStateWithFunction:function_2d_array error:&error];
  if (!pipeline_2d_array) {
    REXLOG_ERROR("MetalPresenter::EnsureCopyTextureConvertPipelines: Failed to create "
           "compute pipeline (2d array): {}",
           error ? error.localizedDescription.UTF8String : "unknown error");
    [pipeline_2d release];
    [function_2d_array release];
    [function_2d release];
    [library release];
    return false;
  }

  [function_2d_array release];
  [function_2d release];
  [library release];

  copy_texture_convert_pipeline_2d_ = pipeline_2d;
  copy_texture_convert_pipeline_2d_array_ = pipeline_2d_array;
  return true;
}

bool MetalPresenter::EnsureApplyGammaPipelines() {
  if (apply_gamma_table_pipeline_ && apply_gamma_pwl_pipeline_) {
    return true;
  }

  id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device_;
  if (!mtl_device) {
    REXLOG_ERROR("MetalPresenter::EnsureApplyGammaPipelines: No Metal device");
    return false;
  }

  auto create_pipeline = [&](const uint8_t* data, size_t size,
                             const char* label) -> id<MTLComputePipelineState> {
    if (!data || !size) {
      REXLOG_ERROR("MetalPresenter::EnsureApplyGammaPipelines: {} data missing", label);
      return nil;
    }
    dispatch_data_t dispatch_data =
        dispatch_data_create(data, size, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
                             DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* error = nil;
    id<MTLLibrary> library = [mtl_device newLibraryWithData:dispatch_data error:&error];
    dispatch_release(dispatch_data);
    if (!library) {
      REXLOG_ERROR("MetalPresenter::EnsureApplyGammaPipelines: failed to create {} "
             "library: {}",
             label, error ? error.localizedDescription.UTF8String : "unknown");
      return nil;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"entry_xe"];
    if (!function) {
      REXLOG_ERROR("MetalPresenter::EnsureApplyGammaPipelines: missing entry_xe for "
             "{}",
             label);
      [library release];
      return nil;
    }
    id<MTLComputePipelineState> pipeline = [mtl_device newComputePipelineStateWithFunction:function
                                                                                     error:&error];
    if (!pipeline) {
      REXLOG_ERROR("MetalPresenter::EnsureApplyGammaPipelines: failed to create {} "
             "pipeline: {}",
             label, error ? error.localizedDescription.UTF8String : "unknown");
    }
    [function release];
    [library release];
    return pipeline;
  };

  if (!apply_gamma_table_pipeline_) {
    apply_gamma_table_pipeline_ =
        create_pipeline(apply_gamma_table_cs_metallib, sizeof(apply_gamma_table_cs_metallib),
                        "apply_gamma_table_cs");
  }
  if (!apply_gamma_pwl_pipeline_) {
    apply_gamma_pwl_pipeline_ = create_pipeline(
        apply_gamma_pwl_cs_metallib, sizeof(apply_gamma_pwl_cs_metallib), "apply_gamma_pwl_cs");
  }
  return apply_gamma_table_pipeline_ && apply_gamma_pwl_pipeline_;
}

namespace {

id<MTLFunction> CreateGuestOutputFunction(id<MTLDevice> device, const uint8_t* data, size_t size,
                                          const char* label) {
  if (!device || !data || !size) {
    return nil;
  }
  dispatch_data_t dispatch_data =
      dispatch_data_create(data, size, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
                           DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithData:dispatch_data error:&error];
  dispatch_release(dispatch_data);
  if (!library) {
    REXLOG_ERROR("Metal presenter: failed to create guest output library {}: {}", label,
           error ? error.localizedDescription.UTF8String : "unknown");
    return nil;
  }
  id<MTLFunction> function = [library newFunctionWithName:@"entry_xe"];
  if (!function) {
    REXLOG_ERROR("Metal presenter: missing guest output entrypoint for {}", label);
  }
  [library release];
  return function;
}

id<MTLRenderPipelineState> CreateGuestOutputPipeline(id<MTLDevice> device,
                                                     id<MTLFunction> vertex_function,
                                                     id<MTLFunction> fragment_function,
                                                     MTLPixelFormat pixel_format,
                                                     const char* label) {
  if (!device || !vertex_function || !fragment_function) {
    return nil;
  }
  MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vertex_function;
  desc.fragmentFunction = fragment_function;
  desc.colorAttachments[0].pixelFormat = pixel_format;

  NSError* error = nil;
  id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:desc
                                                                               error:&error];
  if (!pipeline) {
    REXLOG_ERROR("Metal presenter: failed to create {} pipeline: {}", label,
           error ? error.localizedDescription.UTF8String : "unknown");
  }
  [desc release];
  return pipeline;
}

}  // namespace

bool MetalPresenter::EnsureGuestOutputPaintResources(uint32_t pixel_format) {
  if (guest_output_pipeline_bilinear_ && guest_output_pipeline_bilinear_dither_ &&
      guest_output_sampler_ && guest_output_pipeline_format_ == pixel_format) {
    return true;
  }

  if (guest_output_pipeline_bilinear_) {
    [guest_output_pipeline_bilinear_ release];
    guest_output_pipeline_bilinear_ = nullptr;
  }
  if (guest_output_pipeline_bilinear_dither_) {
    [guest_output_pipeline_bilinear_dither_ release];
    guest_output_pipeline_bilinear_dither_ = nullptr;
  }
  if (guest_output_sampler_) {
    [guest_output_sampler_ release];
    guest_output_sampler_ = nullptr;
  }
  guest_output_pipeline_format_ = pixel_format;

  id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device_;
  if (!mtl_device) {
    REXLOG_ERROR("Metal presenter: missing Metal device for guest output");
    return false;
  }

  MTLSamplerDescriptor* sampler_desc = [[MTLSamplerDescriptor alloc] init];
  sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
  sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
  sampler_desc.mipFilter = MTLSamplerMipFilterNotMipmapped;
  sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
  guest_output_sampler_ = [mtl_device newSamplerStateWithDescriptor:sampler_desc];
  [sampler_desc release];
  if (!guest_output_sampler_) {
    REXLOG_ERROR("Metal presenter: failed to create guest output sampler");
    return false;
  }

  id<MTLFunction> vs = CreateGuestOutputFunction(
      mtl_device, guest_output_triangle_strip_rect_vs_metallib,
      sizeof(guest_output_triangle_strip_rect_vs_metallib), "guest_output_vs");
  id<MTLFunction> ps_bilinear = CreateGuestOutputFunction(
      mtl_device, guest_output_bilinear_ps_metallib, sizeof(guest_output_bilinear_ps_metallib),
      "guest_output_bilinear_ps");
  id<MTLFunction> ps_bilinear_dither = CreateGuestOutputFunction(
      mtl_device, guest_output_bilinear_dither_ps_metallib,
      sizeof(guest_output_bilinear_dither_ps_metallib), "guest_output_bilinear_dither_ps");

  if (!vs || !ps_bilinear || !ps_bilinear_dither) {
    if (vs) {
      [vs release];
    }
    if (ps_bilinear) {
      [ps_bilinear release];
    }
    if (ps_bilinear_dither) {
      [ps_bilinear_dither release];
    }
    return false;
  }

  MTLPixelFormat mtl_format = static_cast<MTLPixelFormat>(pixel_format);
  guest_output_pipeline_bilinear_ =
      CreateGuestOutputPipeline(mtl_device, vs, ps_bilinear, mtl_format, "guest_output_bilinear");
  guest_output_pipeline_bilinear_dither_ = CreateGuestOutputPipeline(
      mtl_device, vs, ps_bilinear_dither, mtl_format, "guest_output_bilinear_dither");

  [vs release];
  [ps_bilinear release];
  [ps_bilinear_dither release];

  return guest_output_pipeline_bilinear_ && guest_output_pipeline_bilinear_dither_;
}

bool MetalPresenter::CopyTextureToGuestOutput(MTL::Texture* source_texture, id dest_texture,
                                              uint32_t source_width, uint32_t source_height,
                                              bool force_swap_rb, bool use_pwl_gamma_ramp,
                                              uint64_t* submission_out) {
  if (!source_texture || !dest_texture) {
    REXLOG_ERROR("MetalPresenter::CopyTextureToGuestOutput: Invalid textures");
    return false;
  }
  if (submission_out) {
    *submission_out = 0;
  }

  id<MTLCommandBuffer> copy_command_buffer = [command_queue_ commandBuffer];
  if (!copy_command_buffer) {
    REXLOG_ERROR("MetalPresenter::CopyTextureToGuestOutput: Failed to create "
           "command buffer");
    return false;
  }
  if (REXCVAR_GET(metal_presenter_debug_markers)) {
    copy_command_buffer.label = @"XeniaGuestOutputCopy";
  }
  auto commit_copy_command_buffer = [&]() {
    uint64_t submission_id =
        guest_output_submission_counter_.fetch_add(1,
                                                   std::memory_order_relaxed) +
        1;
    if (submission_out) {
      *submission_out = submission_id;
    }
    if (shared_event_) {
      [copy_command_buffer encodeSignalEvent:(id<MTLSharedEvent>)shared_event_
                                       value:submission_id];
    }
    [copy_command_buffer commit];
  };

  // Cast dest_texture to proper Metal texture type
  id<MTLTexture> dest_metal_texture = (id<MTLTexture>)dest_texture;

  // Handle size differences by copying the minimum dimensions.
  uint32_t source_clamped_width =
      std::min(source_width, static_cast<uint32_t>(source_texture->width()));
  uint32_t source_clamped_height =
      std::min(source_height, static_cast<uint32_t>(source_texture->height()));
  uint32_t copy_width =
      std::min(source_clamped_width, static_cast<uint32_t>([dest_metal_texture width]));
  uint32_t copy_height =
      std::min(source_clamped_height, static_cast<uint32_t>([dest_metal_texture height]));
  if (!copy_width || !copy_height) {
    REXLOG_WARN("MetalPresenter::CopyTextureToGuestOutput: Empty copy region");
    return false;
  }

  MTLPixelFormat src_format = (MTLPixelFormat)source_texture->pixelFormat();
  MTLPixelFormat dst_format = dest_metal_texture.pixelFormat;
  static MTLPixelFormat last_src_format = MTLPixelFormatInvalid;
  static MTLPixelFormat last_dst_format = MTLPixelFormatInvalid;
  static bool last_swap_rb = false;
  static bool last_used_shader = false;
  auto is_srgb_format = [](MTLPixelFormat fmt) -> bool {
    switch (fmt) {
      case MTLPixelFormatRGBA8Unorm_sRGB:
      case MTLPixelFormatBGRA8Unorm_sRGB:
        return true;
      default:
        break;
    }
#ifdef MTLPixelFormatRGB10A2Unorm_sRGB
    if (fmt == MTLPixelFormatRGB10A2Unorm_sRGB) {
      return true;
    }
#endif
#ifdef MTLPixelFormatBGR10A2Unorm_sRGB
    if (fmt == MTLPixelFormatBGR10A2Unorm_sRGB) {
      return true;
    }
#endif
    return false;
  };
  auto linear_format_for_srgb = [](MTLPixelFormat fmt) -> MTLPixelFormat {
    switch (fmt) {
      case MTLPixelFormatRGBA8Unorm_sRGB:
        return MTLPixelFormatRGBA8Unorm;
      case MTLPixelFormatBGRA8Unorm_sRGB:
        return MTLPixelFormatBGRA8Unorm;
      default:
        break;
    }
#ifdef MTLPixelFormatRGB10A2Unorm_sRGB
    if (fmt == MTLPixelFormatRGB10A2Unorm_sRGB) {
      return MTLPixelFormatRGB10A2Unorm;
    }
#endif
#ifdef MTLPixelFormatBGR10A2Unorm_sRGB
    if (fmt == MTLPixelFormatBGR10A2Unorm_sRGB) {
      return MTLPixelFormatBGR10A2Unorm;
    }
#endif
    return fmt;
  };
  auto is_bgra_format = [](MTLPixelFormat fmt) -> bool {
    switch (fmt) {
      case MTLPixelFormatBGRA8Unorm:
      case MTLPixelFormatBGRA8Unorm_sRGB:
        return true;
      default:
        break;
    }
#ifdef MTLPixelFormatBGR10A2Unorm
    if (fmt == MTLPixelFormatBGR10A2Unorm) {
      return true;
    }
#endif
#ifdef MTLPixelFormatBGR10A2Unorm_sRGB
    if (fmt == MTLPixelFormatBGR10A2Unorm_sRGB) {
      return true;
    }
#endif
    return false;
  };
  auto is_rgb10a2_format = [](MTLPixelFormat fmt) -> bool {
    switch (fmt) {
      case MTLPixelFormatRGB10A2Unorm:
        return true;
      default:
        break;
    }
#ifdef MTLPixelFormatRGB10A2Unorm_sRGB
    if (fmt == MTLPixelFormatRGB10A2Unorm_sRGB) {
      return true;
    }
#endif
#ifdef MTLPixelFormatBGR10A2Unorm
    if (fmt == MTLPixelFormatBGR10A2Unorm) {
      return true;
    }
#endif
#ifdef MTLPixelFormatBGR10A2Unorm_sRGB
    if (fmt == MTLPixelFormatBGR10A2Unorm_sRGB) {
      return true;
    }
#endif
    return false;
  };
  bool decode_srgb = false;
  bool encode_srgb = false;
  MTL::Texture* sample_texture = source_texture;
  bool swizzle_view_used = false;
  if (is_srgb_format(src_format)) {
    MTLPixelFormat linear_format = linear_format_for_srgb(src_format);
    if (linear_format != src_format) {
      MTL::Texture* linear_view = GetCachedPresenterPixelFormatView(
          linear_presenter_view_, source_texture,
          static_cast<MTL::PixelFormat>(linear_format));
      if (linear_view) {
        sample_texture = linear_view;
        src_format = linear_format;
      } else {
        decode_srgb = true;
      }
    }
  }

  if (sample_texture->textureType() == MTL::TextureType2DArray) {
    if (sample_texture->arrayLength() != 1) {
      REXLOG_WARN("MetalPresenter::CopyTextureToGuestOutput: array source has {} "
             "slices; using slice 0",
             sample_texture->arrayLength());
    }
    NS::Range level_range = NS::Range::Make(0, sample_texture->mipmapLevelCount());
    NS::Range slice_range = NS::Range::Make(0, 1);
    MTL::TextureSwizzleChannels swizzle = sample_texture->swizzle();
    MTL::Texture* present_view = GetCachedPresenterTextureView(
        array_presenter_view_, sample_texture, sample_texture->pixelFormat(),
        MTL::TextureType2D, level_range, slice_range, swizzle);
    if (present_view) {
      sample_texture = present_view;
    } else {
      REXLOG_WARN("MetalPresenter::CopyTextureToGuestOutput: failed to create 2D view "
             "for array source");
    }
  }

  bool swap_rb_in_shader = force_swap_rb;
  if (force_swap_rb) {
    NS::Range level_range = NS::Range::Make(0, sample_texture->mipmapLevelCount());
    NS::Range slice_range = NS::Range::Make(0, sample_texture->arrayLength());
    MTL::TextureSwizzleChannels swizzle = {MTL::TextureSwizzleBlue, MTL::TextureSwizzleGreen,
                                           MTL::TextureSwizzleRed, MTL::TextureSwizzleAlpha};
    MTL::Texture* swizzle_view = GetCachedPresenterTextureView(
        swizzle_presenter_view_, sample_texture, sample_texture->pixelFormat(),
        sample_texture->textureType(), level_range, slice_range, swizzle);
    if (swizzle_view) {
      sample_texture = swizzle_view;
      swap_rb_in_shader = false;
      swizzle_view_used = true;
    }
  }

  if (is_srgb_format(dst_format) && !is_srgb_format(src_format)) {
    encode_srgb = true;
  }

  bool apply_gamma = EnsureApplyGammaPipelines() &&
                     (use_pwl_gamma_ramp ? gamma_ramp_pwl_valid_ : gamma_ramp_table_valid_);
  if (apply_gamma) {
    id<MTLTexture> gamma_dest_texture = dest_metal_texture;
    bool needs_gamma_convert = false;
    if (!is_rgb10a2_format(dst_format)) {
      if (!gamma_output_texture_ || gamma_output_width_ != copy_width ||
          gamma_output_height_ != copy_height) {
        if (gamma_output_texture_) {
          [gamma_output_texture_ release];
          gamma_output_texture_ = nullptr;
        }
        MTLTextureDescriptor* gamma_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm
                                                               width:copy_width
                                                              height:copy_height
                                                           mipmapped:NO];
        gamma_desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        gamma_desc.storageMode = MTLStorageModePrivate;
        gamma_output_texture_ =
            [(__bridge id<MTLDevice>)device_ newTextureWithDescriptor:gamma_desc];
        if (!gamma_output_texture_) {
          REXLOG_ERROR("MetalPresenter::CopyTextureToGuestOutput: Failed to create "
                 "gamma output texture {}x{}",
                 copy_width, copy_height);
          return false;
        }
        gamma_output_width_ = copy_width;
        gamma_output_height_ = copy_height;
      }
      gamma_dest_texture = gamma_output_texture_;
      needs_gamma_convert = true;
    }

    id<MTLComputeCommandEncoder> compute_encoder = [copy_command_buffer computeCommandEncoder];
    if (!compute_encoder) {
      REXLOG_ERROR("MetalPresenter::CopyTextureToGuestOutput: Failed to create "
             "compute encoder for gamma");
      return false;
    }
    if (REXCVAR_GET(metal_presenter_debug_markers)) {
      compute_encoder.label = @"ApplyGamma";
    }

    id<MTLComputePipelineState> pipeline =
        use_pwl_gamma_ramp ? apply_gamma_pwl_pipeline_ : apply_gamma_table_pipeline_;
    id<MTLTexture> ramp_texture =
        use_pwl_gamma_ramp ? gamma_ramp_pwl_texture_ : gamma_ramp_table_texture_;
    if (!pipeline || !ramp_texture) {
      REXLOG_ERROR("MetalPresenter::CopyTextureToGuestOutput: missing gamma pipeline");
      return false;
    }

    struct ApplyGammaConstants {
      uint32_t width;
      uint32_t height;
    } constants;
    constants.width = copy_width;
    constants.height = copy_height;

    [compute_encoder setComputePipelineState:pipeline];
    [compute_encoder setTexture:ramp_texture atIndex:0];
    [compute_encoder setTexture:(__bridge id<MTLTexture>)sample_texture atIndex:1];
    [compute_encoder setTexture:gamma_dest_texture atIndex:2];
    [compute_encoder setBytes:&constants length:sizeof(constants) atIndex:0];

    const MTLSize threads_per_threadgroup = MTLSizeMake(16, 8, 1);
    const MTLSize threads_per_grid = MTLSizeMake(copy_width, copy_height, 1);
    [compute_encoder dispatchThreads:threads_per_grid
               threadsPerThreadgroup:threads_per_threadgroup];
    [compute_encoder endEncoding];

    if (last_src_format != src_format || last_dst_format != dst_format || last_swap_rb ||
        !last_used_shader) {
      REXLOG_DEBUG("MetalPresenter::CopyTextureToGuestOutput: present path=gamma "
             "src={} dst={} pwl={}",
             int(src_format), int(dst_format), use_pwl_gamma_ramp ? 1 : 0);
      last_src_format = src_format;
      last_dst_format = dst_format;
      last_swap_rb = false;
      last_used_shader = true;
    }

    bool swap_rb_after_gamma = force_swap_rb && !swizzle_view_used;
    if (needs_gamma_convert || swap_rb_after_gamma) {
      if (!EnsureCopyTextureConvertPipelines()) {
        return false;
      }
      id<MTLComputeCommandEncoder> convert_encoder = [copy_command_buffer computeCommandEncoder];
      if (!convert_encoder) {
        REXLOG_ERROR("MetalPresenter::CopyTextureToGuestOutput: Failed to create "
               "compute encoder for gamma conversion");
        return false;
      }
      if (REXCVAR_GET(metal_presenter_debug_markers)) {
        convert_encoder.label = @"GammaConvert";
      }
      id<MTLComputePipelineState> pipeline =
          (gamma_dest_texture.textureType == MTLTextureType2DArray)
              ? (id<MTLComputePipelineState>)copy_texture_convert_pipeline_2d_array_
              : (id<MTLComputePipelineState>)copy_texture_convert_pipeline_2d_;
      [convert_encoder setComputePipelineState:pipeline];
      [convert_encoder setTexture:gamma_dest_texture atIndex:0];
      [convert_encoder setTexture:dest_metal_texture atIndex:1];

      constexpr uint32_t kCopyFlagSwapRB = 1u;
      struct CopyConstants {
        uint32_t width;
        uint32_t height;
        uint32_t slice;
        uint32_t flags;
      } constants;
      constants.width = copy_width;
      constants.height = copy_height;
      constants.slice = 0;
      constants.flags = swap_rb_after_gamma ? kCopyFlagSwapRB : 0u;
      [convert_encoder setBytes:&constants length:sizeof(constants) atIndex:0];

      const MTLSize threads_per_threadgroup = MTLSizeMake(16, 16, 1);
      const MTLSize threads_per_grid = MTLSizeMake(copy_width, copy_height, 1);
      [convert_encoder dispatchThreads:threads_per_grid
                 threadsPerThreadgroup:threads_per_threadgroup];
      [convert_encoder endEncoding];
    }

    commit_copy_command_buffer();
    return true;
  }

  // Metal blit encoder copies require identical pixel formats. If formats
  // differ (for example RGBA8 vs BGRA8), the result is undefined and may
  // manifest as diagonal splits / corrupted colors in captures.
  bool needs_shader = src_format != dst_format || swap_rb_in_shader || decode_srgb || encode_srgb;
  if (needs_shader) {
    REXLOG_WARN("MetalPresenter::CopyTextureToGuestOutput: {} src={} dst={} - using "
           "shader conversion",
           swap_rb_in_shader ? "forced swap_rb" : "pixel format mismatch", int(src_format),
           int(dst_format));

    if (!EnsureCopyTextureConvertPipelines()) {
      return false;
    }

    const auto src_type = sample_texture->textureType();
    if (src_type != MTL::TextureType::TextureType2D &&
        src_type != MTL::TextureType::TextureType2DArray) {
      REXLOG_ERROR("MetalPresenter::CopyTextureToGuestOutput: Unsupported source "
             "texture type {} for conversion",
             int(src_type));
      return false;
    }

    id<MTLComputeCommandEncoder> compute_encoder = [copy_command_buffer computeCommandEncoder];
    if (!compute_encoder) {
      REXLOG_ERROR("MetalPresenter::CopyTextureToGuestOutput: Failed to create "
             "compute encoder");
      return false;
    }
    if (REXCVAR_GET(metal_presenter_debug_markers)) {
      compute_encoder.label = @"CopyConvert";
    }

    id<MTLComputePipelineState> pipeline =
        (src_type == MTL::TextureType::TextureType2DArray)
            ? (id<MTLComputePipelineState>)copy_texture_convert_pipeline_2d_array_
            : (id<MTLComputePipelineState>)copy_texture_convert_pipeline_2d_;
    [compute_encoder setComputePipelineState:pipeline];
    [compute_encoder setTexture:(__bridge id<MTLTexture>)sample_texture atIndex:0];
    [compute_encoder setTexture:dest_metal_texture atIndex:1];

    constexpr uint32_t kCopyFlagSwapRB = 1u;
    constexpr uint32_t kCopyFlagDecodeSRGB = 1u << 1;
    constexpr uint32_t kCopyFlagEncodeSRGB = 1u << 2;

    struct CopyConstants {
      uint32_t width;
      uint32_t height;
      uint32_t slice;
      uint32_t flags;
    } constants;
    constants.width = copy_width;
    constants.height = copy_height;
    constants.slice = 0;
    bool swap_rb = swap_rb_in_shader || (is_bgra_format(src_format) != is_bgra_format(dst_format));
    if (last_src_format != src_format || last_dst_format != dst_format || last_swap_rb != swap_rb ||
        !last_used_shader) {
      REXLOG_DEBUG("MetalPresenter::CopyTextureToGuestOutput: present path=shader "
             "src={} dst={} swap_rb={} srgb_decode={} srgb_encode={}",
             int(src_format), int(dst_format), swap_rb ? 1 : 0, decode_srgb ? 1 : 0,
             encode_srgb ? 1 : 0);
      last_src_format = src_format;
      last_dst_format = dst_format;
      last_swap_rb = swap_rb;
      last_used_shader = true;
    }
    constants.flags = (swap_rb ? kCopyFlagSwapRB : 0u) | (decode_srgb ? kCopyFlagDecodeSRGB : 0u) |
                      (encode_srgb ? kCopyFlagEncodeSRGB : 0u);
    [compute_encoder setBytes:&constants length:sizeof(constants) atIndex:0];

    const MTLSize threads_per_threadgroup = MTLSizeMake(16, 16, 1);
    const MTLSize threads_per_grid = MTLSizeMake(copy_width, copy_height, 1);
    [compute_encoder dispatchThreads:threads_per_grid
               threadsPerThreadgroup:threads_per_threadgroup];
    [compute_encoder endEncoding];

    commit_copy_command_buffer();
    REXLOG_DEBUG("MetalPresenter::CopyTextureToGuestOutput: Shader copy completed successfully");
    return true;
  }

  if (last_src_format != src_format || last_dst_format != dst_format || last_swap_rb ||
      last_used_shader) {
    REXLOG_DEBUG("MetalPresenter::CopyTextureToGuestOutput: present path=blit "
           "src={} dst={}",
           int(src_format), int(dst_format));
    last_src_format = src_format;
    last_dst_format = dst_format;
    last_swap_rb = false;
    last_used_shader = false;
  }

  id<MTLBlitCommandEncoder> blit_encoder = [copy_command_buffer blitCommandEncoder];
  if (!blit_encoder) {
    REXLOG_ERROR("MetalPresenter::CopyTextureToGuestOutput: Failed to create blit encoder");
    return false;
  }

  REXLOG_DEBUG("MetalPresenter::CopyTextureToGuestOutput: Copying {}x{} (src {}x{}) → "
         "{}x{}",
         copy_width, copy_height, sample_texture->width(), sample_texture->height(),
         [dest_metal_texture width], [dest_metal_texture height]);

  [blit_encoder copyFromTexture:(__bridge id<MTLTexture>)sample_texture
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(copy_width, copy_height, 1)
                      toTexture:dest_metal_texture
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];

  [blit_encoder endEncoding];

  commit_copy_command_buffer();

  REXLOG_DEBUG("MetalPresenter::CopyTextureToGuestOutput: Copy completed successfully");
  return true;
}

}  // namespace metal
}  // namespace ui
}  // namespace xe
