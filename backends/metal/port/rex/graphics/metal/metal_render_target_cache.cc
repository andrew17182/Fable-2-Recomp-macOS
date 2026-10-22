/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_hitch.h"
#include "rex/graphics/metal/metal_xfer_keys_default.h"
#include "rex/graphics/metal/metal_startup_parallel.h"
#include "rex/graphics/metal/metal_render_target_cache.h"
#include "rex/graphics/metal/metal_pass_trace.h"
#include "rex/graphics/metal/metal_encoder_trim.h"
#include "rex/graphics/metal/metal_pass_merge.h"
#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_readiness.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <pthread.h>
#include <pthread/qos.h>
#include <cstring>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "rex/assert.h"
#include <rex/bit.h>
#include "rex/logging.h"
#include "rex/math.h"
#include "rex/graphics/util/draw.h"
#include "rex/graphics/flags.h"
#include "rex/graphics/metal/metal_heap_pool.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/graphics/metal/metal_texture_cache.h"
#include "rex/graphics/shaders/bytecode/metal/host_depth_store_1xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/host_depth_store_2xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/host_depth_store_4xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_32bpp_4xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_64bpp_4xmsaa_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_128bpp_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_128bpp_scaled_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_16bpp_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_16bpp_scaled_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_32bpp_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_32bpp_scaled_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_64bpp_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_64bpp_scaled_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_8bpp_cs.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_full_8bpp_scaled_cs.h"

#include "third_party/metal-shader-converter/include/metal_irconverter_runtime.h"
#include "rex/graphics/metal/metal_command_processor.h"
#include "rex/graphics/pipeline/texture/info.h"
#include "rex/graphics/pipeline/texture/util.h"
#include "rex/graphics/xenos.h"

DEFINE_bool(
    metal_allow_gamma_unorm16, false,
    "Allow gamma_render_target_as_unorm16 on Metal despite known issues",
    "Metal");
DEFINE_bool(metal_transfer_fast_divmod, true,
            "Use fast exact div/mod in Metal transfer shaders", "Metal");
DEFINE_bool(metal_transfer_native_stencil_output, true,
            "Use Metal fragment stencil output for transfer stencil writes",
            "Metal");
DEFINE_bool(
    metal_transfer_msaa_sample_id, true,
    "Use sample_id in Metal transfer shaders for MSAA (sample-rate shading)",
    "Metal");
DEFINE_bool(metal_transfer_in_draw_pass, true,
            "Encode eligible color ownership transfers in the guest draw pass",
            "Metal");
DEFINE_bool(metal_direct_host_resolve, true,
            "Resolve eligible fast color/depth copies directly from Metal host "
            "render targets to shared/scaled resolve memory",
            "Metal");
DEFINE_bool(metal_use_heaps, true,
            "Use MTLHeap-backed texture allocations in Metal to reduce "
            "allocation overhead and fragmentation.",
            "Metal");
DEFINE_int32(metal_heap_min_bytes, 33554432,
             "Minimum heap size (bytes) for Metal heap allocations.", "Metal");

namespace rex {
namespace graphics {
namespace metal {

namespace {

class ScopedAutoreleasePool {
 public:
  ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}
  ~ScopedAutoreleasePool() {
    if (pool_) {
      pool_->release();
    }
  }

  ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
  ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

 private:
  NS::AutoreleasePool* pool_;
};

uint32_t EstimateRenderTargetBytesPerPixel(bool is_64bpp) {
  return is_64bpp ? 8u : 4u;
}

// [hitch] Diagnostic FABLE2_MSL_SALT=<text>: prepend a comment to every MSL source compiled at run
// time here, so Metal's system shader cache misses (what a first launch after a shader-source change,
// or on a new Mac / OS update, sees). Unset: empty.
const std::string& MslSalt() {
  static const std::string salt = [] {
    const char* v = std::getenv("FABLE2_MSL_SALT");
    return (v && v[0]) ? std::string("// salt ") + v + "\n" : std::string();
  }();
  return salt;
}

MTL::ComputePipelineState* CreateComputePipelineFromEmbeddedLibrary(
    MTL::Device* device, const void* metallib_data, size_t metallib_size,
    const char* debug_name) {
  if (!device || !metallib_data || !metallib_size) {
    return nullptr;
  }
  hitch::Scope hitch_scope(hitch::kPsoRt);

  NS::Error* error = nullptr;

  dispatch_data_t data = dispatch_data_create(
      metallib_data, metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* lib = device->newLibrary(data, &error);
  dispatch_release(data);
  if (!lib) {
    REXLOG_ERROR("Metal: failed to create {} library: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  // XeSL compute entrypoint name used in the embedded metallibs.
  NS::String* fn_name = NS::String::string("entry_xe", NS::UTF8StringEncoding);
  MTL::Function* fn = lib->newFunction(fn_name);
  if (!fn) {
    REXLOG_ERROR("Metal: {} missing entry_xe", debug_name);
    lib->release();
    return nullptr;
  }

  // Label the pipeline so each resolve variant is distinguishable in the
  // Xcode GPU trace; the entrypoint is always "entry_xe", so without a label
  // every variant looks identical there.
  MTL::ComputePipelineDescriptor* pipeline_desc =
      MTL::ComputePipelineDescriptor::alloc()->init();
  pipeline_desc->setComputeFunction(fn);
  if (debug_name) {
    pipeline_desc->setLabel(
        NS::String::string(debug_name, NS::UTF8StringEncoding));
  }
  MTL::ComputePipelineState* pipeline = device->newComputePipelineState(
      pipeline_desc, MTL::PipelineOptionNone,
      static_cast<MTL::AutoreleasedComputePipelineReflection*>(nullptr),
      &error);
  pipeline_desc->release();
  fn->release();
  lib->release();

  if (!pipeline) {
    REXLOG_ERROR("Metal: failed to create {} pipeline: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  return pipeline;
}

bool IsResolveDirectHostRTFastCandidate(
    draw_util::ResolveCopyShaderIndex shader) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
      return true;
    default:
      return false;
  }
}

size_t DirectHostResolveFullDestIndex(
    draw_util::ResolveCopyShaderIndex shader) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFull8bpp:
      return 0;
    case draw_util::ResolveCopyShaderIndex::kFull16bpp:
      return 1;
    case draw_util::ResolveCopyShaderIndex::kFull32bpp:
      return 2;
    case draw_util::ResolveCopyShaderIndex::kFull64bpp:
      return 3;
    case draw_util::ResolveCopyShaderIndex::kFull128bpp:
      return 4;
    default:
      return 5;
  }
}

size_t ResolveFastBppIndex(draw_util::ResolveCopyShaderIndex shader) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
      return 0;
    case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
      return 1;
    default:
      break;
  }
  assert_unhandled_case(shader);
  return 0;
}

size_t ResolveFastMsaaIndex(draw_util::ResolveCopyShaderIndex shader) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
      return 0;
    case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
      return 1;
    default:
      break;
  }
  assert_unhandled_case(shader);
  return 0;
}

bool IsResolveDirectHostRTFullColorCandidate(
    draw_util::ResolveCopyShaderIndex shader) {
  return DirectHostResolveFullDestIndex(shader) < 5;
}

bool IsResolveDirectHostRTFullColorSourcePackable(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

bool IsResolveDirectHostRTCandidate(draw_util::ResolveCopyShaderIndex shader) {
  return IsResolveDirectHostRTFastCandidate(shader) ||
         IsResolveDirectHostRTFullColorCandidate(shader);
}

uint32_t DirectHostResolveFullDestBppLog2(
    draw_util::ResolveCopyShaderIndex shader) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFull8bpp:
      return 0;
    case draw_util::ResolveCopyShaderIndex::kFull16bpp:
      return 1;
    case draw_util::ResolveCopyShaderIndex::kFull32bpp:
      return 2;
    case draw_util::ResolveCopyShaderIndex::kFull64bpp:
      return 3;
    case draw_util::ResolveCopyShaderIndex::kFull128bpp:
      return 4;
    default:
      break;
  }
  assert_unhandled_case(shader);
  return 2;
}

uint32_t DirectHostResolvePixelsPerThread(
    draw_util::ResolveCopyShaderIndex shader, bool source_is_64bpp,
    xenos::MsaaSamples msaa_samples) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFull8bpp:
      return msaa_samples >= xenos::MsaaSamples::k4X ? 4 : 8;
    case draw_util::ResolveCopyShaderIndex::kFull128bpp:
      return 2;
    case draw_util::ResolveCopyShaderIndex::kFull16bpp:
    case draw_util::ResolveCopyShaderIndex::kFull32bpp:
    case draw_util::ResolveCopyShaderIndex::kFull64bpp:
      return 4;
    default:
      return source_is_64bpp ? 4 : 8;
  }
}

const char* ResolveCopyEncoderLabel(bool direct_host_rt_candidate) {
  return direct_host_rt_candidate ? "XeniaResolveCopyDirectCandidate"
                                  : "XeniaResolveCopyFallback";
}

const char* ResolveDumpEncoderLabel(bool direct_host_rt_candidate) {
  return direct_host_rt_candidate ? "XeniaEDRAMDumpResolveDirectCandidate"
                                  : "XeniaEDRAMDumpResolveFallback";
}

struct AttachmentLoadStoreActions {
  MTL::LoadAction load = MTL::LoadActionLoad;
  MTL::StoreAction store = MTL::StoreActionStore;
};

AttachmentLoadStoreActions GetRealAttachmentLoadStoreActions(
    bool needs_initial_clear, bool previous_contents_needed = true) {
  if (needs_initial_clear) {
    return {MTL::LoadActionClear, MTL::StoreActionStore};
  }
  return {
      previous_contents_needed ? MTL::LoadActionLoad : MTL::LoadActionDontCare,
      MTL::StoreActionStore};
}

AttachmentLoadStoreActions GetTransientAttachmentLoadStoreActions() {
  return {MTL::LoadActionDontCare, MTL::StoreActionDontCare};
}

// [rt-store-audit] FABLE2_RT_STORE_DIAG=stencil|depth|both: DIAGNOSTIC ONLY,
// UNSAFE (discards EDRAM contents the game may read later). Upper bound of
// what DontCare stores could save; bit 0 stencil, bit 1 depth.
uint32_t RtStoreDiag() {
  static const uint32_t mode = [] {
    const char* v = std::getenv("FABLE2_RT_STORE_DIAG");
    uint32_t m = 0;
    if (v && !std::strcmp(v, "stencil")) m = 1;
    if (v && !std::strcmp(v, "depth")) m = 2;
    if (v && !std::strcmp(v, "both")) m = 3;
    if (m) {
      REXLOG_WARN("[rt-store-audit] FABLE2_RT_STORE_DIAG={}: diagnostic "
                  "DontCare stores (unsafe)", v);
    }
    return m;
  }();
  return mode;
}

void SetAttachmentLoadStoreActions(
    MTL::RenderPassAttachmentDescriptor* attachment,
    AttachmentLoadStoreActions actions) {
  attachment->setLoadAction(actions.load);
  attachment->setStoreAction(actions.store);
}

void SetEncoderLabel(MTL::CommandEncoder* encoder, const char* label) {
  if (!encoder || !label || !EncoderDebugLabelsEnabled()) {
    return;
  }
  encoder->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
}

void EndSharedMemoryUploadBlitEncoderForCommandBuffer(
    MetalCommandProcessor& command_processor,
    MTL::CommandBuffer* command_buffer) {
  if (command_buffer &&
      command_buffer == command_processor.GetCurrentCommandBuffer()) {
    command_processor.EndSharedMemoryUploadBlitEncoder();
  }
}

void PushEncoderDebugGroup(MTL::CommandEncoder* encoder,
                           const std::string& label) {
  if (!encoder || label.empty() || !EncoderDebugLabelsEnabled()) {
    return;
  }
  encoder->pushDebugGroup(
      NS::String::string(label.c_str(), NS::UTF8StringEncoding));
}

// Packing formats for transferring host RT contents to the EDRAM buffer.
// Keep numeric values in sync with Metal dump shaders in
// InitializeEdramComputeShaders.
enum class MetalEdramDumpFormat : uint32_t {
  kColorRGBA8 = 0,
  kColorRGB10A2Unorm = 1,
  kColorRGB10A2Float = 2,
  kColorRG16Snorm = 3,
  kColorRG16Float = 4,
  kColorR32Float = 5,
  kColorRGBA16Snorm = 6,
  kColorRGBA16Float = 7,
  kColorRGBA16Unorm = 8,
  kColorRG32Float = 9,
  kDepthD24S8 = 16,
  kDepthD24FS8 = 17,
};

constexpr uint32_t kMetalEdramDumpFlagHasStencil = 1u << 0;
constexpr uint32_t kMetalEdramDumpFlagDepthRound = 1u << 1;
constexpr uint32_t kMetalEdramDumpFlagGammaAsLinear = 1u << 2;

size_t MsaaSamplesToIndex(xenos::MsaaSamples samples) {
  switch (samples) {
    case xenos::MsaaSamples::k1X:
      return 0;
    case xenos::MsaaSamples::k2X:
      return 1;
    case xenos::MsaaSamples::k4X:
      return 2;
    default:
      return 0;
  }
}

uint32_t MsaaSamplesToCount(xenos::MsaaSamples samples) {
  switch (samples) {
    case xenos::MsaaSamples::k1X:
      return 1;
    case xenos::MsaaSamples::k2X:
      return 2;
    case xenos::MsaaSamples::k4X:
      return 4;
    default:
      return 1;
  }
}

struct TransferAddressConstants {
  uint32_t dest_pitch;
  uint32_t source_pitch;
  int32_t source_to_dest;
};

struct TransferShaderConstants {
  TransferAddressConstants address;
  TransferAddressConstants host_depth_address;
  uint32_t source_format;
  uint32_t dest_format;
  uint32_t source_is_depth;
  uint32_t dest_is_depth;
  uint32_t source_is_uint;
  uint32_t dest_is_uint;
  uint32_t source_is_64bpp;
  uint32_t dest_is_64bpp;
  uint32_t source_msaa_samples;
  uint32_t dest_msaa_samples;
  uint32_t host_depth_source_msaa_samples;
  uint32_t host_depth_source_is_copy;
  uint32_t depth_round;
  uint32_t msaa_2x_supported;
  uint32_t tile_width_samples;
  uint32_t tile_height_samples;
  uint32_t dest_tile_width_pixels;
  uint32_t dest_tile_height_pixels;
  float dest_tile_width_pixels_inv;
  float dest_tile_height_pixels_inv;
  float source_pitch_tiles_inv;
  float host_depth_source_pitch_tiles_inv;
  float dest_pixel_to_ndc_x;
  float dest_pixel_to_ndc_y;
  uint32_t dest_sample_id;
  uint32_t stencil_mask;
  uint32_t stencil_clear;
  // [effects-native] mixed-scale transfers (native 1x effect targets).
  uint32_t source_tile_width_samples;
  uint32_t source_tile_height_samples;
  uint32_t source_scale_x;
  uint32_t source_scale_y;
  uint32_t dest_scale_x;
  uint32_t dest_scale_y;
};

struct TransferRectInstance {
  float origin_x;
  float origin_y;
  float size_x;
  float size_y;
};

struct TransferClearColorFloatConstants {
  float color[4];
};

struct TransferClearColorUintConstants {
  uint32_t color[4];
};

struct TransferClearDepthConstants {
  float depth;
  float padding[3];
};

enum class TransferOutput {
  kColor,
  kDepth,
  kStencilBit,
};

struct TransferModeInfo {
  TransferOutput output;
  bool source_is_color;
  bool uses_host_depth;
};

constexpr TransferModeInfo kTransferModeInfos[] = {
    {TransferOutput::kColor, true, false},        // kColorToColor
    {TransferOutput::kDepth, true, false},        // kColorToDepth
    {TransferOutput::kColor, false, false},       // kDepthToColor
    {TransferOutput::kDepth, false, false},       // kDepthToDepth
    {TransferOutput::kStencilBit, true, false},   // kColorToStencilBit
    {TransferOutput::kStencilBit, false, false},  // kDepthToStencilBit
    {TransferOutput::kDepth, true, true},         // kColorAndHostDepthToDepth
    {TransferOutput::kDepth, false, true},        // kDepthAndHostDepthToDepth
};

}  // namespace

bool MetalRenderTargetCache::IsKey64bpp(RenderTargetKey key) const {
  // For host texture storage and byte estimates, gamma-as-unorm16 uses
  // RGBA16Unorm. Guest/EDRAM addressing is still 32bpp, so transfer rectangle
  // and shader tile math must use RenderTargetKey::Is64bpp instead.
  return key.Is64bpp() ||
         (!key.is_depth &&
          key.GetColorFormat() ==
              xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
          gamma_render_target_as_unorm16_);
}

uint32_t MetalRenderTargetCache::GetMetalEdramDumpFormat(RenderTargetKey key) {
  if (key.is_depth) {
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24FS8:
        return static_cast<uint32_t>(MetalEdramDumpFormat::kDepthD24FS8);
      case xenos::DepthRenderTargetFormat::kD24S8:
      default:
        return static_cast<uint32_t>(MetalEdramDumpFormat::kDepthD24S8);
    }
  }
  switch (key.GetColorFormat()) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGBA8);
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGB10A2Unorm);
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGB10A2Float);
    case xenos::ColorRenderTargetFormat::k_16_16:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRG16Snorm);
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRG16Float);
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorR32Float);
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGBA16Snorm);
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGBA16Float);
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRG32Float);
    default:
      return static_cast<uint32_t>(MetalEdramDumpFormat::kColorRGBA8);
  }
}

// MetalRenderTarget implementation
MetalRenderTargetCache::MetalRenderTarget::~MetalRenderTarget() {
  if (stencil_view_) {
    stencil_view_->release();
    stencil_view_ = nullptr;
  }
  if (draw_texture_ && draw_texture_ != texture_) {
    draw_texture_->release();
    draw_texture_ = nullptr;
  }
  if (transfer_texture_ && transfer_texture_ != texture_) {
    transfer_texture_->release();
    transfer_texture_ = nullptr;
  }
  if (msaa_draw_texture_ && msaa_draw_texture_ != msaa_texture_) {
    msaa_draw_texture_->release();
    msaa_draw_texture_ = nullptr;
  }
  if (msaa_transfer_texture_ && msaa_transfer_texture_ != msaa_texture_) {
    msaa_transfer_texture_->release();
    msaa_transfer_texture_ = nullptr;
  }
  if (texture_) {
    texture_->release();
    texture_ = nullptr;
  }
  if (msaa_texture_) {
    msaa_texture_->release();
    msaa_texture_ = nullptr;
  }
}

// MetalRenderTargetCache implementation
MetalRenderTargetCache::MetalRenderTargetCache(
    const RegisterFile& register_file, const rex::memory::Memory& memory,
    TraceWriter* trace_writer, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y, MetalCommandProcessor& command_processor)
    : RenderTargetCache(register_file, memory, trace_writer,
                        draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {
  resolve_scale_x_ = draw_resolution_scale_x;
  resolve_scale_y_ = draw_resolution_scale_y;
}

MetalRenderTargetCache::~MetalRenderTargetCache() { Shutdown(true); }

MetalRenderTargetCache::TelemetryStats
MetalRenderTargetCache::GetAndResetTelemetryStats() {
  TelemetryStats stats = telemetry_;
  telemetry_ = TelemetryStats();
  return stats;
}

RenderTargetCache::Path MetalRenderTargetCache::GetPath() const {
  return Path::kHostRenderTargets;
}

bool MetalRenderTargetCache::InitializeEdramBufferViews() {
  ReleaseEdramBufferViews();
  if (!edram_buffer_) {
    return false;
  }

  struct ViewInit {
    uint32_t element_size_bytes_pow2;
    MTL::PixelFormat format;
    MTL::Texture** texture_out;
    const char* label;
  };
  const ViewInit kViews[] = {
      {2, MTL::PixelFormatR32Uint, &edram_r32_uint_buffer_view_,
       "XeniaEDRAMR32UintView"},
      {3, MTL::PixelFormatRG32Uint, &edram_r32g32_uint_buffer_view_,
       "XeniaEDRAMR32G32UintView"},
      {4, MTL::PixelFormatRGBA32Uint, &edram_r32g32b32a32_uint_buffer_view_,
       "XeniaEDRAMR32G32B32A32UintView"},
  };

  for (const ViewInit& view_init : kViews) {
    const NS::UInteger bytes_per_element = NS::UInteger(1u)
                                           << view_init.element_size_bytes_pow2;
    const NS::UInteger width = edram_buffer_->length() / bytes_per_element;
    if (!width) {
      REXLOG_ERROR("MetalRenderTargetCache: invalid EDRAM bindless width for {}",
             view_init.label);
      ReleaseEdramBufferViews();
      return false;
    }

    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
    desc->setTextureType(MTL::TextureTypeTextureBuffer);
    desc->setPixelFormat(view_init.format);
    desc->setWidth(width);
    desc->setHeight(1);
    desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite |
                   MTL::TextureUsagePixelFormatView);
    desc->setResourceOptions(edram_buffer_->resourceOptions());
    desc->setStorageMode(edram_buffer_->storageMode());

    MTL::Texture* texture =
        edram_buffer_->newTexture(desc, 0, edram_buffer_->length());
    desc->release();
    if (!texture) {
      REXLOG_ERROR("MetalRenderTargetCache: failed to create EDRAM bindless view {}",
             view_init.label);
      ReleaseEdramBufferViews();
      return false;
    }
    texture->setLabel(
        NS::String::string(view_init.label, NS::UTF8StringEncoding));
    *view_init.texture_out = texture;
  }

  return true;
}

void MetalRenderTargetCache::ReleaseEdramBufferViews() {
  if (edram_r32_uint_buffer_view_) {
    edram_r32_uint_buffer_view_->release();
    edram_r32_uint_buffer_view_ = nullptr;
  }
  if (edram_r32g32_uint_buffer_view_) {
    edram_r32g32_uint_buffer_view_->release();
    edram_r32g32_uint_buffer_view_ = nullptr;
  }
  if (edram_r32g32b32a32_uint_buffer_view_) {
    edram_r32g32b32a32_uint_buffer_view_->release();
    edram_r32g32b32a32_uint_buffer_view_ = nullptr;
  }
}

MTL::Texture* MetalRenderTargetCache::GetEdramUintPow2BufferView(
    uint32_t element_size_bytes_pow2) const {
  switch (element_size_bytes_pow2) {
    case 2:
      return edram_r32_uint_buffer_view_;
    case 3:
      return edram_r32g32_uint_buffer_view_;
    case 4:
      return edram_r32g32b32a32_uint_buffer_view_;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return nullptr;
  }
}

bool MetalRenderTargetCache::WriteEdramUintPow2BindlessDescriptor(
    IRDescriptorTableEntry* entry, uint32_t element_size_bytes_pow2) const {
  if (!entry || !edram_buffer_) {
    return false;
  }
  MTL::Texture* texture_view =
      GetEdramUintPow2BufferView(element_size_bytes_pow2);
  if (!texture_view) {
    return false;
  }
  IRBufferView buffer_view = {};
  const uint64_t bytes_per_element = uint64_t(1u) << element_size_bytes_pow2;
  buffer_view.buffer = edram_buffer_;
  buffer_view.bufferOffset = 0;
  buffer_view.bufferSize = edram_buffer_->length();
  buffer_view.textureBufferView = texture_view;
  buffer_view.textureViewOffsetInElements = uint32_t(
      (uint64_t(edram_buffer_->gpuAddress()) % 16u) / bytes_per_element);
  buffer_view.typedBuffer = true;
  IRDescriptorTableSetBufferView(entry, &buffer_view);
  return true;
}

void MetalRenderTargetCache::CollectBindlessResources(
    std::vector<MTL::Resource*>& resources_out) const {
  if (edram_buffer_) {
    resources_out.push_back(edram_buffer_);
  }
  if (edram_r32_uint_buffer_view_) {
    resources_out.push_back(edram_r32_uint_buffer_view_);
  }
  if (edram_r32g32_uint_buffer_view_) {
    resources_out.push_back(edram_r32g32_uint_buffer_view_);
  }
  if (edram_r32g32b32a32_uint_buffer_view_) {
    resources_out.push_back(edram_r32g32b32a32_uint_buffer_view_);
  }
}

bool MetalRenderTargetCache::Initialize() {
  device_ = command_processor_.GetMetalDevice();
  if (!device_) {
    REXLOG_ERROR("MetalRenderTargetCache: No Metal device available");
    return false;
  }

  // 2x msaa and unorm16 support virtually guarunteed as minimum OS target /
  // Metal version currently is MacOS 15 / Metal 3
  msaa_2x_supported_ = device_->supportsTextureSampleCount(2);

  gamma_render_target_as_unorm16_ = REXCVAR_GET(gamma_render_target_as_unorm16) &&
                                    REXCVAR_GET(metal_allow_gamma_unorm16);
  if (REXCVAR_GET(gamma_render_target_as_unorm16) &&
      !REXCVAR_GET(metal_allow_gamma_unorm16)) {
    REXLOG_WARN(
        "Metal: gamma_render_target_as_unorm16 disabled due to known issues; "
        "set --metal_allow_gamma_unorm16=true to force");
  }

  // [rt-shrink] growing render targets release their old textures; inside
  // power-of-two heaps (32 MB minimum, never returned) that left ~200 MB of
  // holes at 3x. Render targets are created rarely, so with the shrink they
  // are separate allocations (FABLE2_RT_SHRINK_HEAPS=1 keeps the heap pool).
  const bool rt_heaps =
      RtShrinkMode() != 1 || [] {
        const char* v = std::getenv("FABLE2_RT_SHRINK_HEAPS");  // getenv-ok: once, at cache setup
        return v && v[0] == '1';
      }();
  if (REXCVAR_GET(metal_use_heaps) && rt_heaps) {
    size_t min_heap_bytes = std::max<int32_t>(0, REXCVAR_GET(metal_heap_min_bytes));
    render_target_heap_pool_ = std::make_unique<MetalHeapPool>(
        device_, MTL::StorageModePrivate, min_heap_bytes, "XeniaRT");
    render_target_heap_pool_->SetHeapCreatedCallback([this](MTL::Heap* heap) {
      command_processor_.AddResidencySetHeap(heap);
    });
  }

  // Create the EDRAM buffer.
  //
  // The guest has 10 MiB of EDRAM for samples, but with host resolution
  // scaling enabled the compute path addresses a scaled EDRAM layout (the
  // shaders multiply the tile dimensions by resolution_scale_x/y). The buffer
  // therefore must be scaled by the same factor to avoid out-of-bounds writes.
  const uint32_t scale_x = std::max<uint32_t>(1u, draw_resolution_scale_x());
  const uint32_t scale_y = std::max<uint32_t>(1u, draw_resolution_scale_y());
  const size_t edram_dwords = size_t(xenos::kEdramTileCount) *
                              size_t(xenos::kEdramTileWidthSamples) *
                              size_t(xenos::kEdramTileHeightSamples) *
                              size_t(scale_x) * size_t(scale_y);
  const size_t edram_size_bytes = edram_dwords * sizeof(uint32_t);
  const bool edram_cpu_visible = false;
  const MTL::ResourceOptions edram_storage_mode =
      edram_cpu_visible ? MTL::ResourceStorageModeShared
                        : MTL::ResourceStorageModePrivate;
  edram_buffer_ = device_->newBuffer(edram_size_bytes, edram_storage_mode);
  if (!edram_buffer_) {
    REXLOG_ERROR("MetalRenderTargetCache: Failed to create EDRAM buffer");
    return false;
  }
  edram_buffer_->setLabel(
      NS::String::string("EDRAM Buffer", NS::UTF8StringEncoding));
  if (edram_cpu_visible) {
    void* edram_contents = edram_buffer_->contents();
    if (edram_contents) {
      std::memset(edram_contents, 0, edram_size_bytes);
    }
  } else {
    ScopedAutoreleasePool autorelease_pool;
    MTL::CommandBuffer* cmd =
        command_processor_.CreateStandaloneTransferCommandBuffer(
            "XeniaCB reason=edram-init");
    if (cmd) {
      MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
      if (blit) {
        blit->fillBuffer(
            edram_buffer_,
            NS::Range::Make(0, static_cast<NS::UInteger>(edram_size_bytes)), 0);
        blit->endEncoding();
        command_processor_.CommitStandaloneAsync(cmd);
      } else {
        cmd->release();
      }
    }
  }
  if (!InitializeEdramBufferViews()) {
    REXLOG_ERROR("MetalRenderTargetCache: Failed to create EDRAM bindless views");
    return false;
  }
  ++bindless_resources_serial_;
  if (!bindless_resources_serial_) {
    bindless_resources_serial_ = 1;
  }
  // Initialize EDRAM compute shaders
  if (!InitializeEdramComputeShaders()) {
    REXLOG_ERROR(
        "MetalRenderTargetCache: Failed to initialize EDRAM compute shaders");
    return false;
  }

  // Initialize base class
  InitializeCommon();

  return true;
}

void MetalRenderTargetCache::Shutdown(bool from_destructor) {
  StopTransferPrewarm();
  if (!from_destructor) {
    ClearCache();
  }
  for (auto& e : stencil_const_textures_) {
    if (e.second) e.second->release();
  }
  stencil_const_textures_.clear();
  stencil_const_sources_.clear();
  for (auto& e : clear_const_textures_) {
    if (e.texture) e.texture->release();
  }
  clear_const_textures_.clear();
  clear_const_sources_.clear();

  // Clean up dummy target
  dummy_color_target_owner_.reset();
  dummy_color_target_ = nullptr;
  dummy_color_target_shape_key_ = 0;
  if (cached_render_pass_descriptor_) {
    cached_render_pass_descriptor_->release();
    cached_render_pass_descriptor_ = nullptr;
  }

  for (auto& it : transfer_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_pipelines_.clear();
  for (auto& it : edram_load_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  edram_load_pipelines_.clear();
  for (auto& it : transfer_clear_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_clear_pipelines_.clear();
  if (transfer_library_) {
    transfer_library_->release();
    transfer_library_ = nullptr;
  }
  if (edram_load_library_) {
    edram_load_library_->release();
    edram_load_library_ = nullptr;
  }
  if (edram_load_library_msaa_) {
    edram_load_library_msaa_->release();
    edram_load_library_msaa_ = nullptr;
  }
  if (transfer_depth_state_) {
    transfer_depth_state_->release();
    transfer_depth_state_ = nullptr;
  }
  if (transfer_depth_state_none_) {
    transfer_depth_state_none_->release();
    transfer_depth_state_none_ = nullptr;
  }
  if (transfer_depth_clear_state_) {
    transfer_depth_clear_state_->release();
    transfer_depth_clear_state_ = nullptr;
  }
  if (transfer_stencil_clear_state_) {
    transfer_stencil_clear_state_->release();
    transfer_stencil_clear_state_ = nullptr;
  }
  if (transfer_stencil_output_state_) {
    transfer_stencil_output_state_->release();
    transfer_stencil_output_state_ = nullptr;
  }
  for (auto& state : transfer_stencil_bit_states_) {
    if (state) {
      state->release();
      state = nullptr;
    }
  }
  if (transfer_dummy_buffer_) {
    transfer_dummy_buffer_->release();
    transfer_dummy_buffer_ = nullptr;
  }
  for (size_t i = 0; i < rex::countof(transfer_dummy_color_float_); ++i) {
    if (transfer_dummy_color_float_[i]) {
      transfer_dummy_color_float_[i]->release();
      transfer_dummy_color_float_[i] = nullptr;
    }
    if (transfer_dummy_color_uint_[i]) {
      transfer_dummy_color_uint_[i]->release();
      transfer_dummy_color_uint_[i] = nullptr;
    }
    if (transfer_dummy_depth_[i]) {
      transfer_dummy_depth_[i]->release();
      transfer_dummy_depth_[i] = nullptr;
    }
    if (transfer_dummy_stencil_[i]) {
      transfer_dummy_stencil_[i]->release();
      transfer_dummy_stencil_[i] = nullptr;
    }
  }

  // Clean up EDRAM compute shaders
  ShutdownEdramComputeShaders();
  ReleaseEdramBufferViews();

  if (edram_buffer_) {
    edram_buffer_->release();
    edram_buffer_ = nullptr;
  }

  // Destroy all render targets
  DestroyAllRenderTargets(!from_destructor);
  render_target_map_.clear();

  if (render_target_heap_pool_) {
    render_target_heap_pool_->Shutdown();
    render_target_heap_pool_.reset();
  }

  // Shutdown base class
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool MetalRenderTargetCache::InitializeEdramComputeShaders() {
  // Initialize the resolve / EDRAM compute pipelines used by the Metal backend.
  const bool draw_resolution_scaled = IsDrawResolutionScaled();
  for (auto& by_bpp : edram_dump_color_pipelines_) {
    for (auto& by_source : by_bpp) {
      for (auto*& pipeline : by_source) {
        pipeline = nullptr;
      }
    }
  }
  for (auto*& pipeline : edram_dump_depth_pipelines_) {
    pipeline = nullptr;
  }
  for (auto& by_scaled : resolve_full_pipelines_) {
    for (auto*& pipeline : by_scaled) {
      pipeline = nullptr;
    }
  }
  for (auto& by_scaled : resolve_fast_pipelines_) {
    for (auto& by_bpp : by_scaled) {
      for (auto*& pipeline : by_bpp) {
        pipeline = nullptr;
      }
    }
  }
  ResetDirectHostResolvePipelines(false);
  for (size_t i = 0; i < rex::countof(host_depth_store_pipelines_); ++i) {
    host_depth_store_pipelines_[i] = nullptr;
  }

  struct ResolveFullPipelineConfig {
    const void* metallib_data;
    size_t metallib_size;
    bool scaled;
    draw_util::ResolveCopyShaderIndex copy_shader;
    const char* debug_name;
  };
  struct ResolveFastPipelineConfig {
    const void* metallib_data;
    size_t metallib_size;
    bool scaled;
    draw_util::ResolveCopyShaderIndex copy_shader;
    const char* debug_name;
  };
#define XE_RESOLVE_FULL_CONFIG(id, scaled, copy_shader) \
  {id##_metallib, sizeof(id##_metallib), scaled,        \
   draw_util::ResolveCopyShaderIndex::copy_shader, #id}
#define XE_RESOLVE_FAST_CONFIG(id, scaled, copy_shader)     \
  {                                                         \
    id##_metallib, sizeof(id##_metallib), scaled,           \
        draw_util::ResolveCopyShaderIndex::copy_shader, #id \
  }
  static constexpr ResolveFullPipelineConfig kResolveFullPipelineConfigs[] = {
      XE_RESOLVE_FULL_CONFIG(resolve_full_8bpp_cs, false, kFull8bpp),
      XE_RESOLVE_FULL_CONFIG(resolve_full_16bpp_cs, false, kFull16bpp),
      XE_RESOLVE_FULL_CONFIG(resolve_full_32bpp_cs, false, kFull32bpp),
      XE_RESOLVE_FULL_CONFIG(resolve_full_64bpp_cs, false, kFull64bpp),
      XE_RESOLVE_FULL_CONFIG(resolve_full_128bpp_cs, false, kFull128bpp),
      XE_RESOLVE_FULL_CONFIG(resolve_full_8bpp_scaled_cs, true, kFull8bpp),
      XE_RESOLVE_FULL_CONFIG(resolve_full_16bpp_scaled_cs, true, kFull16bpp),
      XE_RESOLVE_FULL_CONFIG(resolve_full_32bpp_scaled_cs, true, kFull32bpp),
      XE_RESOLVE_FULL_CONFIG(resolve_full_64bpp_scaled_cs, true, kFull64bpp),
      XE_RESOLVE_FULL_CONFIG(resolve_full_128bpp_scaled_cs, true, kFull128bpp),
  };
  static constexpr ResolveFastPipelineConfig kResolveFastPipelineConfigs[] = {
      XE_RESOLVE_FAST_CONFIG(resolve_fast_32bpp_1x2xmsaa_cs, false,
                             kFast32bpp1x2xMSAA),
      XE_RESOLVE_FAST_CONFIG(resolve_fast_32bpp_4xmsaa_cs, false,
                             kFast32bpp4xMSAA),
      XE_RESOLVE_FAST_CONFIG(resolve_fast_64bpp_1x2xmsaa_cs, false,
                             kFast64bpp1x2xMSAA),
      XE_RESOLVE_FAST_CONFIG(resolve_fast_64bpp_4xmsaa_cs, false,
                             kFast64bpp4xMSAA),
      XE_RESOLVE_FAST_CONFIG(resolve_fast_32bpp_1x2xmsaa_scaled_cs, true,
                             kFast32bpp1x2xMSAA),
      XE_RESOLVE_FAST_CONFIG(resolve_fast_32bpp_4xmsaa_scaled_cs, true,
                             kFast32bpp4xMSAA),
      XE_RESOLVE_FAST_CONFIG(resolve_fast_64bpp_1x2xmsaa_scaled_cs, true,
                             kFast64bpp1x2xMSAA),
      XE_RESOLVE_FAST_CONFIG(resolve_fast_64bpp_4xmsaa_scaled_cs, true,
                             kFast64bpp4xMSAA),
  };
#undef XE_RESOLVE_FAST_CONFIG
#undef XE_RESOLVE_FULL_CONFIG

  // [startup-par] Resolve, host depth store (and below, EDRAM dump) pipelines
  // are created in parallel (metal_startup_parallel.h); each writes its slot.
  struct EmbeddedComputeJob {
    MTL::ComputePipelineState** slot;
    const void* data;
    size_t size;
    const char* name;
  };
  std::vector<EmbeddedComputeJob> compute_jobs;
  for (const ResolveFullPipelineConfig& cfg : kResolveFullPipelineConfigs) {
    if (cfg.scaled && !draw_resolution_scaled) {
      continue;
    }
    size_t scaled_index = cfg.scaled ? 1u : 0u;
    compute_jobs.push_back(
        {&resolve_full_pipelines_[scaled_index]
                                 [DirectHostResolveFullDestIndex(
                                     cfg.copy_shader)],
         cfg.metallib_data, cfg.metallib_size, cfg.debug_name});
  }
  for (const ResolveFastPipelineConfig& cfg : kResolveFastPipelineConfigs) {
    if (cfg.scaled && !draw_resolution_scaled) {
      continue;
    }
    size_t scaled_index = cfg.scaled ? 1u : 0u;
    compute_jobs.push_back(
        {&resolve_fast_pipelines_[scaled_index][ResolveFastBppIndex(
             cfg.copy_shader)][ResolveFastMsaaIndex(cfg.copy_shader)],
         cfg.metallib_data, cfg.metallib_size, cfg.debug_name});
  }
  const size_t resolve_job_count = compute_jobs.size();
  compute_jobs.push_back(
      {&host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)],
       host_depth_store_1xmsaa_cs_metallib,
       sizeof(host_depth_store_1xmsaa_cs_metallib), "host_depth_store_1xmsaa"});
  compute_jobs.push_back(
      {&host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)],
       host_depth_store_2xmsaa_cs_metallib,
       sizeof(host_depth_store_2xmsaa_cs_metallib), "host_depth_store_2xmsaa"});
  compute_jobs.push_back(
      {&host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)],
       host_depth_store_4xmsaa_cs_metallib,
       sizeof(host_depth_store_4xmsaa_cs_metallib), "host_depth_store_4xmsaa"});
  const auto par_start = std::chrono::steady_clock::now();
  startup::ParallelFor(compute_jobs.size(), [&](size_t i) {
    const EmbeddedComputeJob& job = compute_jobs[i];
    *job.slot = CreateComputePipelineFromEmbeddedLibrary(device_, job.data,
                                                         job.size, job.name);
  });
  for (size_t i = 0; i < resolve_job_count; ++i) {
    if (!*compute_jobs[i].slot) {
      REXLOG_ERROR("Metal: failed to initialize resolve compute pipelines");
      return false;
    }
  }
  REXLOG_INFO("[startup-par] resolve + host depth store pipelines: {} in {:.0f} ms",
              compute_jobs.size(), startup::MsSince(par_start));

  InitializeDirectHostResolvePipelines(draw_resolution_scaled);

  for (size_t i = 0; i < rex::countof(host_depth_store_pipelines_); ++i) {
    if (!host_depth_store_pipelines_[i]) {
      REXLOG_ERROR("Metal: failed to initialize host depth store pipelines");
      return false;
    }
  }

  // EDRAM dump compute shaders -- parameterized MSL template compiled for
  // float, uint ownership-transfer, and depth sources.
  // with different #defines. Each variant differs in MSAA sample count,
  // bits-per-pixel, and whether it dumps color or depth data.
  //
  // Shared preamble: constants, utilities, and ALL pack functions so every
  // variant can reference them through #if guards in the kernel template.
  static const char kEdramDumpPreamble[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramDumpConstants {
  uint dispatch_first_tile;
  uint source_base_tiles;
  uint dest_pitch_tiles;
  uint source_pitch_tiles;
  uint2 resolution_scale;
  uint tile_size_x;
  uint tile_size_y;
  float tile_size_inv_x;
  float tile_size_inv_y;
  float source_pitch_tiles_inv;
  uint format;
  uint flags;
  uint padding;
};

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  if (w == 0u) {
    q = 0u;
    r = 0u;
    return;
  }
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

constant uint kDumpFormatColorRGBA8 = 0;
constant uint kDumpFormatColorRGB10A2Unorm = 1;
constant uint kDumpFormatColorRGB10A2Float = 2;
constant uint kDumpFormatColorRG16Snorm = 3;
constant uint kDumpFormatColorRG16Float = 4;
constant uint kDumpFormatColorR32Float = 5;
constant uint kDumpFormatColorRGBA16Snorm = 6;
constant uint kDumpFormatColorRGBA16Float = 7;
constant uint kDumpFormatColorRGBA16Unorm = 8;
constant uint kDumpFormatColorRG32Float = 9;
constant uint kDumpFormatDepthD24S8 = 16;
constant uint kDumpFormatDepthD24FS8 = 17;
constant uint kDumpFlagHasStencil = 1;   // bit 0
constant uint kDumpFlagDepthRound = 2;   // bit 1
constant uint kDumpFlagGammaAsLinear = 4; // bit 2

// --- Color 32bpp helpers (guarded so depth-only compiles skip them) ---
#if DUMP_IS_DEPTH == 0 && DUMP_BPP == 32

// PWL gamma encode: linear -> gamma (for gamma RTs stored as linear RGBA16Unorm)
inline float XeLinearToPWLGamma(float value) {
  float clamped = clamp(value, 0.0f, 1.0f);
  float scale, offset;
  if (clamped >= (128.0f / 1023.0f)) {
    if (clamped >= (512.0f / 1023.0f)) { scale = 1023.0f / 8.0f; offset = 128.0f / 255.0f; }
    else { scale = 1023.0f / 4.0f; offset = 64.0f / 255.0f; }
  } else {
    if (clamped >= (64.0f / 1023.0f)) { scale = 1023.0f / 2.0f; offset = 32.0f / 255.0f; }
    else { scale = 1023.0f; offset = 0.0f; }
  }
  return trunc(clamped * scale) * (1.0f / 255.0f) + offset;
}
inline float3 XeLinearToPWLGamma3(float3 v) {
  return float3(XeLinearToPWLGamma(v.r), XeLinearToPWLGamma(v.g), XeLinearToPWLGamma(v.b));
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

inline uint XePackSnorm16(float value) {
  float clamped = clamp(value, -1.0f, 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint(packed) & 0xFFFFu;
}

uint XePreClampedFloat32To7e3(float value) {
  uint f32 = as_type<uint>(value);
  uint biased_f32;
  if (f32 < 0x3E800000u) {
    uint f32_exp = f32 >> 23u;
    uint shift = 125u - f32_exp;
    shift = min(shift, 24u);
    uint mantissa = (f32 & 0x7FFFFFu) | 0x800000u;
    biased_f32 = mantissa >> shift;
  } else {
    biased_f32 = f32 + 0xC2000000u;
  }
  uint round_bit = (biased_f32 >> 16u) & 1u;
  uint f10 = biased_f32 + 0x7FFFu + round_bit;
  return (f10 >> 16u) & 0x3FFu;
}

uint XeUnclampedFloat32To7e3(float value) {
  float clamped = min(max(value, 0.0f), 31.875f);
  return XePreClampedFloat32To7e3(clamped);
}

uint XePackColor32bpp(uint format, float4 color) {
  switch (format) {
    case kDumpFormatColorRGBA8: {
      uint r = XePackUnorm(color.r, 255.0f);
      uint g = XePackUnorm(color.g, 255.0f);
      uint b = XePackUnorm(color.b, 255.0f);
      uint a = XePackUnorm(color.a, 255.0f);
      return r | (g << 8u) | (b << 16u) | (a << 24u);
    }
    case kDumpFormatColorRGB10A2Unorm: {
      uint r = XePackUnorm(color.r, 1023.0f);
      uint g = XePackUnorm(color.g, 1023.0f);
      uint b = XePackUnorm(color.b, 1023.0f);
      uint a = XePackUnorm(color.a, 3.0f);
      return r | (g << 10u) | (b << 20u) | (a << 30u);
    }
    case kDumpFormatColorRGB10A2Float: {
      uint r = XeUnclampedFloat32To7e3(color.r);
      uint g = XeUnclampedFloat32To7e3(color.g);
      uint b = XeUnclampedFloat32To7e3(color.b);
      uint a = XePackUnorm(color.a, 3.0f);
      return (r & 0x3FFu) | ((g & 0x3FFu) << 10u) |
             ((b & 0x3FFu) << 20u) | ((a & 0x3u) << 30u);
    }
    case kDumpFormatColorRG16Snorm: {
      uint r = XePackSnorm16(color.r);
      uint g = XePackSnorm16(color.g);
      return r | (g << 16u);
    }
    case kDumpFormatColorRG16Float:
      return as_type<uint>(half2(color.rg));
    case kDumpFormatColorR32Float:
      return as_type<uint>(color.r);
    default: {
      uint r = XePackUnorm(color.r, 255.0f);
      uint g = XePackUnorm(color.g, 255.0f);
      uint b = XePackUnorm(color.b, 255.0f);
      uint a = XePackUnorm(color.a, 255.0f);
      return r | (g << 8u) | (b << 16u) | (a << 24u);
    }
  }
}

uint XePackColor32bppUint(uint format, uint4 color) {
  switch (format) {
    case kDumpFormatColorRG16Snorm:
    case kDumpFormatColorRG16Float:
      return (color.r & 0xFFFFu) | ((color.g & 0xFFFFu) << 16u);
    case kDumpFormatColorR32Float:
      return color.r;
    default:
      return color.r;
  }
}

#endif  // DUMP_IS_DEPTH == 0 && DUMP_BPP == 32

// --- Color 64bpp helpers ---
#if DUMP_IS_DEPTH == 0 && DUMP_BPP == 64

inline uint XePackSnorm16(float value) {
  float clamped = clamp(value, -1.0f, 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint(packed) & 0xFFFFu;
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

uint2 XePackColor64bpp(uint format, float4 color) {
  switch (format) {
    case kDumpFormatColorRGBA16Snorm: {
      uint r = XePackSnorm16(color.r);
      uint g = XePackSnorm16(color.g);
      uint b = XePackSnorm16(color.b);
      uint a = XePackSnorm16(color.a);
      uint rg = r | (g << 16u);
      uint ba = b | (a << 16u);
      return uint2(rg, ba);
    }
    case kDumpFormatColorRGBA16Float: {
      uint rg = as_type<uint>(half2(color.rg));
      uint ba = as_type<uint>(half2(color.ba));
      return uint2(rg, ba);
    }
    case kDumpFormatColorRGBA16Unorm: {
      uint r = XePackUnorm(color.r, 65535.0f);
      uint g = XePackUnorm(color.g, 65535.0f);
      uint b = XePackUnorm(color.b, 65535.0f);
      uint a = XePackUnorm(color.a, 65535.0f);
      uint rg = r | (g << 16u);
      uint ba = b | (a << 16u);
      return uint2(rg, ba);
    }
    case kDumpFormatColorRG32Float: {
      uint r = as_type<uint>(color.r);
      uint g = as_type<uint>(color.g);
      return uint2(r, g);
    }
    default: {
      uint rg = as_type<uint>(half2(color.rg));
      uint ba = as_type<uint>(half2(color.ba));
      return uint2(rg, ba);
    }
  }
}

uint2 XePackColor64bppUint(uint format, uint4 color) {
  switch (format) {
    case kDumpFormatColorRGBA16Snorm:
    case kDumpFormatColorRGBA16Float: {
      uint rg = (color.r & 0xFFFFu) | ((color.g & 0xFFFFu) << 16u);
      uint ba = (color.b & 0xFFFFu) | ((color.a & 0xFFFFu) << 16u);
      return uint2(rg, ba);
    }
    case kDumpFormatColorRG32Float:
      return uint2(color.r, color.g);
    default:
      return uint2(color.r, color.g);
  }
}

#endif  // DUMP_IS_DEPTH == 0 && DUMP_BPP == 64

// --- Depth 32bpp helpers ---
#if DUMP_IS_DEPTH == 1

inline uint XeRoundToNearestEven(float value) {
  float floor_value = floor(value);
  float frac = value - floor_value;
  uint result = uint(floor_value);
  if (frac > 0.5f || (frac == 0.5f && (result & 1u))) {
    result += 1u;
  }
  return result;
}

uint XeFloat32To20e4(float value, bool round_to_nearest_even) {
  uint f32 = as_type<uint>(value);
  f32 = min((f32 <= 0x7FFFFFFFu) ? f32 : 0u, 0x3FFFFFF8u);
  uint denormalized =
      ((f32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32 >> 23u), 24u);
  uint f24 = (f32 < 0x38800000u) ? denormalized : (f32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24 += 3u + ((f24 >> 3u) & 1u);
  }
  return (f24 >> 3u) & 0xFFFFFFu;
}

#endif  // DUMP_IS_DEPTH == 1
)METAL";

  // Parameterized kernel template. Uses #if on DUMP_MSAA_SAMPLES, DUMP_BPP,
  // and DUMP_IS_DEPTH to select texture type, sample access, EDRAM element
  // type, pack function, and the depth-specific tile-half swizzle.
  static const char kEdramDumpKernelTemplate[] = R"METAL(

kernel void DUMP_KERNEL_NAME(
#if DUMP_IS_DEPTH == 1
  #if DUMP_MSAA_SAMPLES == 1
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<uint, access::read> stencil [[texture(1)]],
  #else
    texture2d_ms<float, access::read> source [[texture(0)]],
    texture2d_ms<uint, access::read> stencil [[texture(1)]],
  #endif
    device uint* edram [[buffer(0)]],
#else  // color
  #if DUMP_SOURCE_IS_UINT == 1
    #if DUMP_MSAA_SAMPLES == 1
    texture2d<uint, access::read> source [[texture(0)]],
    #else
    texture2d_ms<uint, access::read> source [[texture(0)]],
    #endif
  #else
    #if DUMP_MSAA_SAMPLES == 1
    texture2d<float, access::read> source [[texture(0)]],
    #else
    texture2d_ms<float, access::read> source [[texture(0)]],
    #endif
  #endif
  #if DUMP_BPP == 64
    device uint2* edram [[buffer(0)]],
  #else
    device uint* edram [[buffer(0)]],
  #endif
#endif
    constant EdramDumpConstants& constants [[buffer(1)]],
    uint3 tid [[thread_position_in_grid]]) {
  const uint kEdramTileCount = 2048u;

  uint2 tile_size = uint2(constants.tile_size_x, constants.tile_size_y);

  uint tile_coord_x = 0u;
  uint tile_coord_y = 0u;
  uint sample_in_tile_x = 0u;
  uint sample_in_tile_y = 0u;
  XeFastDivMod(tid.x, tile_size.x, constants.tile_size_inv_x, tile_coord_x,
               sample_in_tile_x);
  XeFastDivMod(tid.y, tile_size.y, constants.tile_size_inv_y, tile_coord_y,
               sample_in_tile_y);
  uint2 tile_coord = uint2(tile_coord_x, tile_coord_y);
  uint2 sample_in_tile = uint2(sample_in_tile_x, sample_in_tile_y);

#if DUMP_IS_DEPTH == 1
  // Depth tiles use a half-width swizzle for EDRAM sample indexing.
  uint2 edram_sample_in_tile = sample_in_tile;
  uint tile_width_half = tile_size.x >> 1u;
  edram_sample_in_tile.x =
      (edram_sample_in_tile.x < tile_width_half)
          ? (edram_sample_in_tile.x + tile_width_half)
          : (edram_sample_in_tile.x - tile_width_half);
#endif

  uint rect_tile_index = tile_coord.y * constants.dest_pitch_tiles + tile_coord.x;

  uint nonwrapped_tile = constants.dispatch_first_tile + rect_tile_index;
  uint wrapped_tile = nonwrapped_tile & (kEdramTileCount - 1u);

  uint tile_samples = tile_size.x * tile_size.y;
#if DUMP_IS_DEPTH == 1
  uint sample_index =
      edram_sample_in_tile.y * tile_size.x + edram_sample_in_tile.x;
#else
  uint sample_index = sample_in_tile.y * tile_size.x + sample_in_tile.x;
#endif
  uint edram_index = wrapped_tile * tile_samples + sample_index;

  uint source_linear_tile = nonwrapped_tile - constants.source_base_tiles;
  uint source_tile_y = 0u;
  uint source_tile_x = 0u;
  XeFastDivMod(source_linear_tile, constants.source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_y, source_tile_x);

#if DUMP_MSAA_SAMPLES == 1
  uint2 source_coord = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                             source_tile_y * tile_size.y + sample_in_tile.y);
#else
  uint2 source_sample = uint2(source_tile_x * tile_size.x + sample_in_tile.x,
                              source_tile_y * tile_size.y + sample_in_tile.y);
  #if DUMP_MSAA_SAMPLES == 2
  uint sample_id = (source_sample.y & 1u) ? DUMP_2X_SAMPLE_MAP_1
                                           : DUMP_2X_SAMPLE_MAP_0;
  uint2 pixel_coord = uint2(source_sample.x, source_sample.y >> 1);
  #else  // 4x
  uint sample_x = source_sample.x & 1u;
  uint sample_y = source_sample.y & 1u;
  uint sample_id = sample_x | (sample_y << 1u);
  uint2 pixel_coord = uint2(source_sample.x >> 1, source_sample.y >> 1);
  #endif
#endif

  // --- Read and pack ---
#if DUMP_IS_DEPTH == 1
  #if DUMP_MSAA_SAMPLES == 1
  float depth = source.read(source_coord).r;
  #else
  float depth = source.read(pixel_coord, sample_id).r;
  #endif

  uint depth24;
  if (constants.format == kDumpFormatDepthD24FS8) {
    bool round_depth = (constants.flags & kDumpFlagDepthRound) != 0u;
    depth24 = XeFloat32To20e4(depth * 2.0f, round_depth);
  } else {
    float depth_f = clamp(depth, 0.0f, 1.0f) * 16777215.0f;
    depth24 = XeRoundToNearestEven(depth_f);
  }

  uint stencil_value = 0u;
  if ((constants.flags & kDumpFlagHasStencil) != 0u) {
  #if DUMP_MSAA_SAMPLES == 1
    stencil_value = stencil.read(source_coord).x & 0xFFu;
  #else
    stencil_value = stencil.read(pixel_coord, sample_id).x & 0xFFu;
  #endif
  }

  edram[edram_index] = (depth24 << 8u) | stencil_value;

#elif DUMP_BPP == 32
  #if DUMP_SOURCE_IS_UINT == 1
    #if DUMP_MSAA_SAMPLES == 1
    uint4 color = source.read(source_coord);
    #else
    uint4 color = source.read(pixel_coord, sample_id);
    #endif

  edram[edram_index] = XePackColor32bppUint(constants.format, color);
  #else
    #if DUMP_MSAA_SAMPLES == 1
    float4 color = source.read(source_coord);
    #else
    float4 color = source.read(pixel_coord, sample_id);
    #endif

    // If source is a linear RGBA16Unorm gamma RT, convert to PWL gamma encoding
    if (constants.flags & kDumpFlagGammaAsLinear) {
      color.rgb = XeLinearToPWLGamma3(color.rgb);
    }

    edram[edram_index] = XePackColor32bpp(constants.format, color);
  #endif

#else  // color 64bpp
  #if DUMP_SOURCE_IS_UINT == 1
    #if DUMP_MSAA_SAMPLES == 1
    uint4 color = source.read(source_coord);
    #else
    uint4 color = source.read(pixel_coord, sample_id);
    #endif

  edram[edram_index] = XePackColor64bppUint(constants.format, color);
  #else
    #if DUMP_MSAA_SAMPLES == 1
    float4 color = source.read(source_coord);
    #else
    float4 color = source.read(pixel_coord, sample_id);
    #endif

    edram[edram_index] = XePackColor64bpp(constants.format, color);
  #endif
#endif
}
)METAL";

  // Configuration table for the EDRAM dump shader variants.
  struct EdramDumpConfig {
    const char* kernel_name;
    uint32_t msaa_samples;
    uint32_t bpp;
    uint32_t is_depth;
    uint32_t source_is_uint;
    MTL::ComputePipelineState** pipeline;
  };
  const EdramDumpConfig kEdramDumpConfigs[] = {
      {"edram_dump_color_32bpp_1xmsaa", 1, 32, 0, 0,
       &edram_dump_color_pipelines_[0][0][0]},
      {"edram_dump_color_32bpp_2xmsaa", 2, 32, 0, 0,
       &edram_dump_color_pipelines_[0][0][1]},
      {"edram_dump_color_32bpp_4xmsaa", 4, 32, 0, 0,
       &edram_dump_color_pipelines_[0][0][2]},
      {"edram_dump_color_uint_32bpp_1xmsaa", 1, 32, 0, 1,
       &edram_dump_color_pipelines_[0][1][0]},
      {"edram_dump_color_uint_32bpp_2xmsaa", 2, 32, 0, 1,
       &edram_dump_color_pipelines_[0][1][1]},
      {"edram_dump_color_uint_32bpp_4xmsaa", 4, 32, 0, 1,
       &edram_dump_color_pipelines_[0][1][2]},
      {"edram_dump_depth_32bpp_1xmsaa", 1, 32, 1, 0,
       &edram_dump_depth_pipelines_[0]},
      {"edram_dump_depth_32bpp_2xmsaa", 2, 32, 1, 0,
       &edram_dump_depth_pipelines_[1]},
      {"edram_dump_depth_32bpp_4xmsaa", 4, 32, 1, 0,
       &edram_dump_depth_pipelines_[2]},
      {"edram_dump_color_64bpp_1xmsaa", 1, 64, 0, 0,
       &edram_dump_color_pipelines_[1][0][0]},
      {"edram_dump_color_64bpp_2xmsaa", 2, 64, 0, 0,
       &edram_dump_color_pipelines_[1][0][1]},
      {"edram_dump_color_64bpp_4xmsaa", 4, 64, 0, 0,
       &edram_dump_color_pipelines_[1][0][2]},
      {"edram_dump_color_uint_64bpp_1xmsaa", 1, 64, 0, 1,
       &edram_dump_color_pipelines_[1][1][0]},
      {"edram_dump_color_uint_64bpp_2xmsaa", 2, 64, 0, 1,
       &edram_dump_color_pipelines_[1][1][1]},
      {"edram_dump_color_uint_64bpp_4xmsaa", 4, 64, 0, 1,
       &edram_dump_color_pipelines_[1][1][2]},
  };

  auto append_dump_define = [](std::string& src, const char* name,
                               uint32_t value) {
    src.append("#define ");
    src.append(name);
    src.push_back(' ');
    src.append(std::to_string(value));
    src.push_back('\n');
  };
  auto append_dump_define_str = [](std::string& src, const char* name,
                                   const char* value) {
    src.append("#define ");
    src.append(name);
    src.push_back(' ');
    src.append(value);
    src.push_back('\n');
  };

  const auto dump_start = std::chrono::steady_clock::now();
  const size_t dump_count = sizeof(kEdramDumpConfigs) / sizeof(kEdramDumpConfigs[0]);
  startup::ParallelFor(dump_count, [&](size_t dump_index) {
    const auto& cfg = kEdramDumpConfigs[dump_index];
    NS::Error* error = nullptr;
    std::string dump_source;
    dump_source.reserve(8192);

    // Prepend per-variant #defines before the shared preamble.
    append_dump_define(dump_source, "DUMP_MSAA_SAMPLES", cfg.msaa_samples);
    append_dump_define(dump_source, "DUMP_BPP", cfg.bpp);
    append_dump_define(dump_source, "DUMP_IS_DEPTH", cfg.is_depth);
    append_dump_define(dump_source, "DUMP_SOURCE_IS_UINT", cfg.source_is_uint);
    append_dump_define_str(dump_source, "DUMP_KERNEL_NAME", cfg.kernel_name);
    if (cfg.msaa_samples == 2) {
      // 2x MSAA sample remapping: guest sample 0/1 -> host sample indices.
      uint32_t map_0 =
          draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_supported_);
      uint32_t map_1 =
          draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_supported_);
      append_dump_define(dump_source, "DUMP_2X_SAMPLE_MAP_0", map_0);
      append_dump_define(dump_source, "DUMP_2X_SAMPLE_MAP_1", map_1);
    }

    dump_source.insert(0, MslSalt());
    dump_source.append(kEdramDumpPreamble);
    dump_source.append(kEdramDumpKernelTemplate);

    NS::String* ns_source =
        NS::String::string(dump_source.c_str(), NS::UTF8StringEncoding);
    MTL::Library* lib = device_->newLibrary(ns_source, nullptr, &error);
    if (!lib) {
      REXLOG_WARN("Metal: failed to compile {} shader: {}", cfg.kernel_name,
             error ? error->localizedDescription()->utf8String() : "unknown");
      return;
    }
    NS::String* fn_name =
        NS::String::string(cfg.kernel_name, NS::UTF8StringEncoding);
    MTL::Function* fn = lib->newFunction(fn_name);
    if (!fn) {
      REXLOG_WARN("Metal: {} missing entrypoint", cfg.kernel_name);
      lib->release();
      return;
    }
    *cfg.pipeline = device_->newComputePipelineState(fn, &error);
    fn->release();
    lib->release();
    if (!*cfg.pipeline) {
      REXLOG_WARN("Metal: failed to create {} pipeline: {}", cfg.kernel_name,
             error ? error->localizedDescription()->utf8String() : "unknown");
    }
  });
  REXLOG_INFO("[startup-par] EDRAM dump pipelines: {} in {:.0f} ms", dump_count,
              startup::MsSince(dump_start));

  return true;
}

void MetalRenderTargetCache::ShutdownEdramComputeShaders() {
  for (auto& by_bpp : edram_dump_color_pipelines_) {
    for (auto& by_source : by_bpp) {
      for (auto*& pipeline : by_source) {
        if (pipeline) {
          pipeline->release();
          pipeline = nullptr;
        }
      }
    }
  }
  for (auto*& pipeline : edram_dump_depth_pipelines_) {
    if (pipeline) {
      pipeline->release();
      pipeline = nullptr;
    }
  }
  for (auto& by_scaled : resolve_full_pipelines_) {
    for (auto*& pipeline : by_scaled) {
      if (pipeline) {
        pipeline->release();
        pipeline = nullptr;
      }
    }
  }
  for (auto& by_scaled : resolve_fast_pipelines_) {
    for (auto& by_bpp : by_scaled) {
      for (auto*& pipeline : by_bpp) {
        if (pipeline) {
          pipeline->release();
          pipeline = nullptr;
        }
      }
    }
  }
  ResetDirectHostResolvePipelines(true);
  TileResolveShutdown();
  for (size_t i = 0; i < rex::countof(host_depth_store_pipelines_); ++i) {
    if (host_depth_store_pipelines_[i]) {
      host_depth_store_pipelines_[i]->release();
      host_depth_store_pipelines_[i] = nullptr;
    }
  }
}

void MetalRenderTargetCache::MarkRenderPassDescriptorDirty() {
  render_pass_descriptor_dirty_ = true;
}

void MetalRenderTargetCache::ClearCache() {
  ClearPendingDrawPassTransfers();
  queued_stencil_alias_ = QueuedStencilAlias();
  // [deferred-clears] the targets are destroyed with their contents.
  deferred_clears_.clear();

  // Clear current bindings
  for (uint32_t i = 0; i < 4; ++i) {
    current_color_targets_[i] = nullptr;
  }
  current_depth_target_ = nullptr;
  MarkRenderPassDescriptorDirty();

  dummy_color_target_owner_.reset();
  dummy_color_target_ = nullptr;
  dummy_color_target_shape_key_ = 0;
  render_target_map_.clear();

  // Call base implementation
  RenderTargetCache::ClearCache();
}

void MetalRenderTargetCache::BeginFrame() {
  (void)FlushPendingDrawPassTransfers(/*requeue_clears=*/true);
  (void)FlushQueuedStencilAlias();
  if (DeferredClearsSwapFlush()) {
    (void)FlushDeferredClears(DeferredClearFlushReason::kSwap);
  }

  // Call base implementation
  RenderTargetCache::BeginFrame();
}

MTL::ComputePipelineState* MetalRenderTargetCache::GetResolvePipeline(
    draw_util::ResolveCopyShaderIndex copy_shader, bool scaled) const {
  size_t scaled_index = scaled ? 1u : 0u;
  if (IsResolveDirectHostRTFastCandidate(copy_shader)) {
    return resolve_fast_pipelines_[scaled_index][ResolveFastBppIndex(
        copy_shader)][ResolveFastMsaaIndex(copy_shader)];
  }
  size_t full_dest_index = DirectHostResolveFullDestIndex(copy_shader);
  if (full_dest_index < kResolveFullDestCount) {
    return resolve_full_pipelines_[scaled_index][full_dest_index];
  }
  return nullptr;
}

bool MetalRenderTargetCache::Update(
    bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, const Shader& vertex_shader) {
  // Pending draw-pass transfers are ownership-visible already. If control
  // reaches another RT update before the command processor encoded them,
  // preserve correctness by falling back to the standalone transfer path first.
  // [deferred-clears] that draw was not encoded: its clears stay deferred.
  if (!FlushPendingDrawPassTransfers(/*requeue_clears=*/true)) {
    return false;
  }

  // Use the base class logic to update the current render target setup.
  rt_shrink_claim_context_ = "draw";
  if (!RenderTargetCache::Update(is_rasterization_done,
                                 normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    REXLOG_ERROR("MetalRenderTargetCache::Update - Base class Update failed");
    return false;
  }

  RenderTarget* const* accumulated_targets =
      last_update_accumulated_render_targets();
  // [rt-shrink] the base update claimed this draw's EDRAM ranges; size the
  // pass's textures before any transfer or draw addresses them.
  RtShrinkNoteDraw(accumulated_targets);
  if (!RtShrinkApplyGrowth()) {
    REXLOG_ERROR("[rt-shrink] growth failed before a draw");
  }
  {
    // [rt-store-audit] does the game use stencil at all?
    UploadTraceCounters& u = UploadTrace();
    if (accumulated_targets[0]) {
      ++u.draw_ds;
      readiness::NoteDraw();  // [ready] world-drawing signal
      if (normalized_depth_control.stencil_enable) ++u.draw_stencil;
    }
    if (EffectsNativeTraceEnabled()) {
      NoteEffectsNativeDraw(
          accumulated_targets,
          normalized_depth_control.z_enable ||
              normalized_depth_control.stencil_enable);
    }
  }

  bool targets_changed = false;

  MetalRenderTarget* new_depth_target =
      accumulated_targets[0]
          ? static_cast<MetalRenderTarget*>(accumulated_targets[0])
          : nullptr;
  if (new_depth_target != current_depth_target_) {
    targets_changed = true;
    current_depth_target_ = new_depth_target;
  }

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MetalRenderTarget* new_color_target =
        accumulated_targets[i + 1]
            ? static_cast<MetalRenderTarget*>(accumulated_targets[i + 1])
            : nullptr;
    if (new_color_target != current_color_targets_[i]) {
      targets_changed = true;
      current_color_targets_[i] = new_color_target;
    }
  }

  // Queue or perform the ownership transfers collected by the base update.
  const std::vector<Transfer>* update_transfers = last_update_transfers();
  // [native-scale] before the K1 / draw-pass routing looks at host depth.
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    DropMixedScaleHostDepth(
        accumulated_targets[i],
        const_cast<std::vector<Transfer>&>(update_transfers[i]));
  }
  // [ssm-merge] a queued stencil-only alias clear joins this draw pass when
  // the pass binds its variant as depth and has no transfers of its own into
  // it or out of it; otherwise its standalone work is encoded first (no GPU
  // work has been encoded since it was queued, and the base update only
  // recorded transfers).
  bool ssm_inject = false;
  if (queued_stencil_alias_.render_target) {
    MetalRenderTarget* qrt = queued_stencil_alias_.render_target;
    bool ok = REXCVAR_GET(metal_transfer_in_draw_pass) &&
              accumulated_targets[0] == qrt && update_transfers[0].empty();
    for (uint32_t i = 0; ok && i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      for (const Transfer& t : update_transfers[i]) {
        if (t.source == qrt || t.host_depth_source == qrt) ok = false;
        for (const Transfer& q : queued_stencil_alias_.transfers) {
          if (t.source == q.source || accumulated_targets[i] == q.source) {
            ok = false;
          }
        }
      }
    }
    if (ok) {
      bool self_host_depth = false;
      queue_reject_line_ = 0;
      ok = CanQueueDrawPassTransfers(0, accumulated_targets,
                                     queued_stencil_alias_.transfers,
                                     &self_host_depth);
    }
    if (ok) {
      const_cast<std::vector<Transfer>&>(update_transfers[0]) =
          queued_stencil_alias_.transfers;
      ssm_inject = true;
    } else if (!FlushQueuedStencilAlias()) {
      return false;
    }
  }
  stencil_const_sources_.clear();
  clear_const_sources_.clear();
  if (SsmStencilFoldEnabled()) {
    FoldDeadStencilClears(accumulated_targets, update_transfers);
  }
  if (ShadowTraceActive()) {
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (!accumulated_targets[i]) continue;
      for (const Transfer& t : update_transfers[i]) {
        REXLOG_INFO("[shadow-trace] f{} xfer [{}] -> [{}] hostdepth={}",
                    frame_log::CurrentFrame(),
                    t.source ? t.source->key().GetDebugName() : "none",
                    accumulated_targets[i]->key().GetDebugName(),
                    t.host_depth_source ? 1 : 0);
      }
    }
  }

  // [deferred-clears] before any transfer of this update is encoded: flush
  // deferred clears of every render target a transfer reads or writes. A
  // bound target with a deferred clear and no transfers gets its clear drawn
  // first inside this draw pass; otherwise it is flushed now.
  // [pass-merge] FABLE2_PASS_MERGE_CLEAR_DST: a destination's own clear is
  // kept for its draw pass, where it is drawn before the transfer quads, the
  // same order as the separate clear pass followed by the transfers. Not when
  // a transfer reads the destination's own host depth (K1 stores it before
  // the pass opens, so it must already hold the clear), and not when the
  // transfers end up outside the draw pass (flushed below).
  uint32_t clear_dst_keep_mask = 0;
  if (!deferred_clears_.empty()) {
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (update_transfers[i].empty()) continue;
      bool keep_dst = PassMergeClearDstEnabled() &&
                      REXCVAR_GET(metal_transfer_in_draw_pass);
      for (const Transfer& t : update_transfers[i]) {
        if (t.source == accumulated_targets[i] ||
            (t.host_depth_source == accumulated_targets[i] &&
             !(ssm_inject && i == 0))) {
          // [ssm-merge] the queued carried clear never overlaps a transfer
          // rectangle (checked when queued), so K1's self host-depth store
          // over those rectangles does not need it first.
          keep_dst = false;
        }
      }
      if (ssm_inject && i == 0 &&
          !(PassMergeClearDstEnabled() &&
            REXCVAR_GET(metal_transfer_in_draw_pass))) {
        keep_dst = false;
      }
      if (keep_dst) {
        for (const DeferredClear& c : deferred_clears_) {
          if (c.render_target == accumulated_targets[i]) {
            clear_dst_keep_mask |= uint32_t(1) << i;
            break;
          }
        }
      } else {
        FlushDeferredClearsFor(accumulated_targets[i],
                               DeferredClearFlushReason::kTransfer);
      }
      for (const Transfer& t : update_transfers[i]) {
        // [clear-const] or read the pending clear from a constant texture.
        if (!TryClearConstSource(t.source, accumulated_targets)) {
          FlushDeferredClearsFor(t.source, DeferredClearFlushReason::kTransfer);
        }
        // [ssm-merge] the self host-depth source is the destination: its
        // carried clear stays for the pass (disjoint from the transfers).
        if (!(ssm_inject && i == 0 &&
              t.host_depth_source == accumulated_targets[0])) {
          FlushDeferredClearsFor(t.host_depth_source,
                                 DeferredClearFlushReason::kTransfer);
        }
      }
    }
  }
  if (!deferred_clears_.empty()) {
    TransferAttachmentFormats clear_formats;
    bool have_formats = GetCurrentTransferAttachmentFormats(clear_formats);
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      auto* rt = static_cast<MetalRenderTarget*>(accumulated_targets[i]);
      if (!rt) continue;
      for (size_t c = 0; c < deferred_clears_.size(); ++c) {
        if (deferred_clears_[c].render_target != rt) continue;
        if (have_formats &&
            !(pending_draw_pass_clear_mask_ & (uint32_t(1) << i)) &&
            CanDrawDeferredClearInPass(i, rt, clear_formats)) {
          pending_draw_pass_clear_targets_[i] = rt;
          pending_draw_pass_clear_values_[i] = deferred_clears_[c].clear_value;
          pending_draw_pass_clear_rects_[i] = deferred_clears_[c].rectangle;
          pending_draw_pass_clear_mask_ |= uint32_t(1) << i;
          deferred_clears_.erase(deferred_clears_.begin() + c);
        } else {
          FlushDeferredClearsFor(rt, DeferredClearFlushReason::kPreflight);
        }
        break;
      }
    }
  }
  if (REXCVAR_GET(metal_transfer_in_draw_pass)) {
    std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
        fallback_transfers;
    bool fallback_transfer_work = false;
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      const std::vector<Transfer>& transfers = update_transfers[i];
      if (transfers.empty()) {
        continue;
      }
      queue_reject_line_ = 0;
      bool self_host_depth = false;
      bool queued = CanQueueDrawPassTransfers(i, accumulated_targets, transfers,
                                              &self_host_depth);
      if (queued && self_host_depth) {
        // [tile-edram] K1: store the destination's host depth now, before the
        // draw pass that will receive the transfer quads opens. Same compute
        // and same data as the standalone path, which runs it right before
        // its own transfer pass.
        std::vector<TransferRectanglePlan> self_rectangles;
        MTL::CommandBuffer* self_cmd = nullptr;
        if (BuildTransferRectanglePlans(accumulated_targets[i]->key(),
                                        transfers, nullptr, true,
                                        self_rectangles)) {
          self_cmd = command_processor_.RequestTransferCommandBuffer(
              MetalCommandProcessor::TransferRequestSource::
                  kRenderTargetTransfer);
        }
        if (self_cmd &&
            EncodeSelfHostDepthStore(self_cmd, accumulated_targets[i],
                                     transfers, self_rectangles)) {
          pending_draw_pass_self_host_depth_stored_ = true;
          ++UploadTrace().tile_selfdepth;
        } else {
          queued = QueueReject(__LINE__);
        }
      }
      NoteDrawPassTransferRoute(queued);
      if (queued) {
        pending_draw_pass_render_targets_[i] = accumulated_targets[i];
        pending_draw_pass_transfers_[i] = transfers;
        pending_draw_pass_transfer_mask_ |= uint32_t(1) << i;
        pending_draw_pass_load_dontcare_mask_ = 0;
        if (PendingDrawPassTransfersFullyOverwriteTarget(
                i, accumulated_targets[i], transfers)) {
          pending_draw_pass_full_overwrite_mask_ |= uint32_t(1) << i;
          MarkRenderPassDescriptorDirty();
        }
        auto* dest_metal_rt =
            static_cast<MetalRenderTarget*>(accumulated_targets[i]);
        if (dest_metal_rt->needs_initial_clear()) {
          dest_metal_rt->SetNeedsInitialClear(false);
          MarkRenderPassDescriptorDirty();
        }
      } else {
        if (pending_draw_pass_clear_mask_ & (uint32_t(1) << i)) {
          // [pass-merge] the destination's clear must precede its
          // transfers, which now take a separate pass.
          FlushDeferredClearsFor(accumulated_targets[i],
                                 DeferredClearFlushReason::kTransfer);
        }
        fallback_transfers[i] = transfers;
        fallback_transfer_work = true;
      }
    }
    if (clear_dst_keep_mask) {
      uint32_t merged = clear_dst_keep_mask & pending_draw_pass_clear_mask_ &
                        pending_draw_pass_transfer_mask_;
      UploadTrace().dc_dst_merged += uint32_t(__builtin_popcount(merged));
    }
    if (fallback_transfer_work) {
      PerformTransfersAndResolveClears(
          1 + xenos::kMaxColorRenderTargets, accumulated_targets,
          fallback_transfers.data(), nullptr, nullptr, nullptr);
    }
    if (ssm_inject) {
      // [ssm-merge] the stencil-only clear after the transfers.
      QueuedStencilAlias q = std::move(queued_stencil_alias_);
      queued_stencil_alias_ = QueuedStencilAlias();
      DeferredClear pc;
      pc.render_target = q.render_target;
      pc.clear_value = q.stencil_value;
      pc.rectangle = q.rectangle;
      if (pending_draw_pass_transfer_mask_ & 1u) {
        pending_draw_pass_post_clear_ = true;
        pending_draw_pass_post_clear_value_ = pc;
        // Carried clear (drawn first in the pass) + transfer quads cover the
        // whole target: no load.
        const bool carried_in_pass =
            !q.has_carried || ((pending_draw_pass_clear_mask_ & 1u) &&
                               pending_draw_pass_clear_targets_[0] ==
                                   q.render_target);
        static uint32_t logged_c = 0;
        if (AliasClearTraceEnabled() && logged_c++ < 4) {
          REXLOG_INFO("[ssm-merge] has_carried {} carried_in_pass {} "
                      "clear_mask {:X}",
                      q.has_carried, carried_in_pass,
                      pending_draw_pass_clear_mask_);
        }
        if (carried_in_pass && StencilAliasCoversTarget(q)) {
          pending_draw_pass_full_overwrite_mask_ |= 1u;
          MarkRenderPassDescriptorDirty();
          ++UploadTrace().ssm_dontcare;
        }
      } else {
        // The transfers took a pass of their own above (after the carried
        // clear): the stencil clear is an ordinary deferred clear now.
        ++UploadTrace().ssm_flushed;
        deferred_clears_.push_back(pc);
        TransferAttachmentFormats clear_formats;
        if (!(pending_draw_pass_clear_mask_ & 1u) &&
            GetCurrentTransferAttachmentFormats(clear_formats) &&
            CanDrawDeferredClearInPass(0, q.render_target, clear_formats)) {
          pending_draw_pass_clear_targets_[0] = q.render_target;
          pending_draw_pass_clear_values_[0] = pc.clear_value;
          pending_draw_pass_clear_rects_[0] = pc.rectangle;
          pending_draw_pass_clear_mask_ |= 1u;
          deferred_clears_.pop_back();
        } else {
          FlushDeferredClearsFor(q.render_target,
                                 DeferredClearFlushReason::kPreflight);
        }
      }
    }
    if (HasPendingDrawPassTransfers()) {
      TransferAttachmentFormats attachment_formats;
      if (!GetCurrentTransferAttachmentFormats(attachment_formats) ||
          !PreflightPendingDrawPassTransfers(attachment_formats)) {
        if (!FlushPendingDrawPassTransfers()) {
          return false;
        }
      }
    }
  } else {
    PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                     accumulated_targets, update_transfers,
                                     nullptr, nullptr, nullptr);
  }

  // Only mark render pass descriptor as dirty if targets actually changed
  if (targets_changed) {
    MarkRenderPassDescriptorDirty();
  }
  // [ssm-merge] the constant stencil applies only to this update's transfers.
  if (!HasPendingDrawPassTransfers()) {
    stencil_const_sources_.clear();
    clear_const_sources_.clear();
  }

  return true;
}

void MetalRenderTargetCache::ClearPendingDrawPassTransfers() {
  bool load_dontcare_descriptor_used =
      pending_draw_pass_load_dontcare_mask_ != 0;
  pending_draw_pass_clear_targets_.fill(nullptr);
  pending_draw_pass_clear_mask_ = 0;
  for (auto& transfers : pending_draw_pass_transfers_) {
    transfers.clear();
  }
  pending_draw_pass_render_targets_.fill(nullptr);
  pending_draw_pass_transfer_mask_ = 0;
  pending_draw_pass_full_overwrite_mask_ = 0;
  pending_draw_pass_load_dontcare_mask_ = 0;
  pending_draw_pass_self_host_depth_stored_ = false;
  pending_draw_pass_post_clear_ = false;
  pending_draw_pass_post_clear_value_ = DeferredClear();
  stencil_const_sources_.clear();
  clear_const_sources_.clear();
  if (load_dontcare_descriptor_used) {
    MarkRenderPassDescriptorDirty();
  }
}

bool MetalRenderTargetCache::CanQueueDrawPassTransfers(
    uint32_t render_target_index, RenderTarget* const* render_targets,
    const std::vector<Transfer>& transfers, bool* self_host_depth_out) const {
  if (self_host_depth_out) {
    *self_host_depth_out = false;
  }
  if (!render_targets || transfers.empty() ||
      render_target_index > xenos::kMaxColorRenderTargets) {
    return QueueReject(__LINE__);
  }
  auto* dest_metal_rt =
      static_cast<MetalRenderTarget*>(render_targets[render_target_index]);
  if (!dest_metal_rt) {
    return QueueReject(__LINE__);
  }
  RenderTargetKey dest_key = dest_metal_rt->key();
  if (dest_key.is_depth != (render_target_index == 0)) {
    return QueueReject(__LINE__);
  }

  bool dest_is_uint = false;
  MTL::Texture* dest_draw_texture = dest_metal_rt->draw_texture();
  if (dest_key.is_depth) {
    MTL::PixelFormat depth_format =
        GetDepthPixelFormat(dest_key.GetDepthFormat());
    if (!dest_draw_texture ||
        dest_draw_texture->pixelFormat() != depth_format) {
      return QueueReject(__LINE__);
    }
  } else {
    MTL::PixelFormat transfer_format = GetColorOwnershipTransferPixelFormat(
        dest_key.GetColorFormat(), &dest_is_uint);
    MTL::PixelFormat draw_format =
        GetColorDrawPixelFormat(dest_key.GetColorFormat());
    if (dest_is_uint || draw_format != transfer_format || !dest_draw_texture ||
        dest_draw_texture != dest_metal_rt->transfer_texture() ||
        dest_draw_texture->pixelFormat() != transfer_format) {
      return QueueReject(__LINE__);
    }
  }

  auto is_active_draw_pass_texture = [&](const MetalRenderTarget* rt,
                                         MTL::Texture* texture) -> bool {
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      auto* active_rt = static_cast<MetalRenderTarget*>(render_targets[i]);
      if (!active_rt) {
        continue;
      }
      MTL::Texture* active_draw_texture = active_rt->draw_texture();
      if (rt == active_rt || texture == active_draw_texture ||
          (rt && rt->draw_texture() == active_draw_texture)) {
        return true;
      }
    }
    return QueueReject(__LINE__);
  };

  for (const Transfer& transfer : transfers) {
    if (!transfer.source) {
      return QueueReject(__LINE__);
    }
    auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
    RenderTargetKey source_key = source_rt->key();
    if (transfer.host_depth_source) {
      if (!dest_key.is_depth) {
        return QueueReject(__LINE__);
      }
      auto* host_depth_rt =
          static_cast<MetalRenderTarget*>(transfer.host_depth_source);
      if (!host_depth_rt) {
        return QueueReject(__LINE__);
      }
      if (host_depth_rt == dest_metal_rt) {
        // [tile-edram] K1: the caller encodes the destination's host depth
        // store before the draw pass opens; the in-pass transfer then reads
        // the EDRAM buffer copy, as the standalone path does.
        if (!self_host_depth_out || !edram_buffer_ ||
            dest_metal_rt->texture() != dest_draw_texture ||
            !TileEdramSelfDepthEnabled()) {
          return QueueReject(__LINE__);
        }
        *self_host_depth_out = true;
      } else {
      RenderTargetKey host_depth_key = host_depth_rt->key();
      if (!host_depth_key.is_depth) {
        return QueueReject(__LINE__);
      }
      MTL::Texture* host_depth_texture = host_depth_rt->texture();
      if (!host_depth_texture || host_depth_texture == dest_draw_texture ||
          host_depth_rt->draw_texture() == dest_draw_texture) {
        return QueueReject(__LINE__);
      }
      MTL::PixelFormat host_depth_format =
          GetDepthPixelFormat(host_depth_key.GetDepthFormat());
      if (host_depth_texture->pixelFormat() != host_depth_format) {
        return QueueReject(__LINE__);
      }
      if (is_active_draw_pass_texture(host_depth_rt, host_depth_texture)) {
        return QueueReject(__LINE__);
      }
      }
    }
    if (source_rt == dest_metal_rt) {
      return QueueReject(__LINE__);
    }

    MTL::Texture* source_texture = source_key.is_depth
                                       ? source_rt->texture()
                                       : source_rt->transfer_texture();
    if (!source_texture || source_texture == dest_draw_texture ||
        source_rt->draw_texture() == dest_draw_texture) {
      return QueueReject(__LINE__);
    }
    if (source_key.is_depth) {
      MTL::PixelFormat source_depth_format =
          GetDepthPixelFormat(source_key.GetDepthFormat());
      if (source_texture->pixelFormat() != source_depth_format) {
        return QueueReject(__LINE__);
      }
    } else {
      MTL::PixelFormat source_transfer_format =
          GetColorOwnershipTransferPixelFormat(source_key.GetColorFormat(),
                                               nullptr);
      if (source_texture->pixelFormat() != source_transfer_format) {
        return QueueReject(__LINE__);
      }
    }

    if (is_active_draw_pass_texture(source_rt, source_texture)) {
      return QueueReject(__LINE__);
    }
  }

  std::vector<TransferRectanglePlan> transfer_rectangles;
  if (!BuildTransferRectanglePlans(dest_key, transfers, nullptr, true,
                                   transfer_rectangles)) {
    return QueueReject(__LINE__);
  }

  return true;
}

bool MetalRenderTargetCache::BuildTransferRectanglePlans(
    RenderTargetKey dest_key, const std::vector<Transfer>& transfers,
    const Transfer::Rectangle* cutout, bool require_all_rectangles,
    std::vector<TransferRectanglePlan>& transfer_rectangles_out) const {
  transfer_rectangles_out.clear();
  transfer_rectangles_out.reserve(transfers.size());
  for (uint32_t transfer_index = 0; transfer_index < transfers.size();
       ++transfer_index) {
    const Transfer& transfer = transfers[transfer_index];
    TransferRectanglePlan plan;
    plan.transfer_index = transfer_index;
    plan.rectangle_count = transfer.GetRectangles(
        dest_key.base_tiles, dest_key.GetPitchTiles(), dest_key.msaa_samples,
        dest_key.Is64bpp(), plan.rectangles.data(), cutout);
    if (!plan.rectangle_count) {
      if (require_all_rectangles) {
        transfer_rectangles_out.clear();
        return false;
      }
      continue;
    }
    transfer_rectangles_out.push_back(plan);
  }
  return true;
}

bool MetalRenderTargetCache::PendingDrawPassTransfersFullyOverwriteTarget(
    uint32_t render_target_index, RenderTarget* render_target,
    const std::vector<Transfer>& transfers) const {
  if (!render_target || transfers.empty() ||
      render_target_index > xenos::kMaxColorRenderTargets) {
    return false;
  }

  auto* dest_metal_rt = static_cast<MetalRenderTarget*>(render_target);
  RenderTargetKey dest_key = dest_metal_rt->key();
  if (dest_key.is_depth != (render_target_index == 0)) {
    return false;
  }

  MTL::Texture* dest_texture = dest_metal_rt->draw_texture();
  if (!dest_texture) {
    return false;
  }
  uint32_t dest_width = uint32_t(dest_texture->width());
  uint32_t dest_height = uint32_t(dest_texture->height());
  if (!dest_width || !dest_height) {
    return false;
  }

  auto is_full_target_rectangle = [&](const Transfer::Rectangle& rect) -> bool {
    uint32_t scaled_x = rect.x_pixels * RenderTargetScaleX(dest_key);
    uint32_t scaled_y = rect.y_pixels * RenderTargetScaleY(dest_key);
    uint32_t scaled_width = rect.width_pixels * RenderTargetScaleX(dest_key);
    uint32_t scaled_height = rect.height_pixels * RenderTargetScaleY(dest_key);
    return !scaled_x && !scaled_y && scaled_width == dest_width &&
           scaled_height == dest_height;
  };

  std::vector<TransferRectanglePlan> transfer_rectangles;
  if (!BuildTransferRectanglePlans(dest_key, transfers, nullptr, true,
                                   transfer_rectangles) ||
      transfer_rectangles.size() != transfers.size()) {
    return false;
  }
  for (const TransferRectanglePlan& transfer_plan : transfer_rectangles) {
    if (transfer_plan.rectangle_count != 1 ||
        !is_full_target_rectangle(transfer_plan.rectangles[0])) {
      return false;
    }
  }
  return true;
}

bool MetalRenderTargetCache::GetActiveTransferAttachmentFormats(
    MTL::RenderPassDescriptor* pass_descriptor,
    TransferAttachmentFormats& attachment_formats_out) const {
  attachment_formats_out.color_attachment_formats.fill(MTL::PixelFormatInvalid);
  attachment_formats_out.depth_attachment_format = MTL::PixelFormatInvalid;
  attachment_formats_out.stencil_attachment_format = MTL::PixelFormatInvalid;
  if (!pass_descriptor) {
    return false;
  }

  auto* color_attachments = pass_descriptor->colorAttachments();
  if (color_attachments) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      auto* color_attachment = color_attachments->object(i);
      MTL::Texture* texture =
          color_attachment ? color_attachment->texture() : nullptr;
      if (texture) {
        attachment_formats_out.color_attachment_formats[i] =
            texture->pixelFormat();
      }
    }
  }

  if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
    if (MTL::Texture* texture = depth_attachment->texture()) {
      attachment_formats_out.depth_attachment_format = texture->pixelFormat();
    }
  }
  if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
    if (MTL::Texture* texture = stencil_attachment->texture()) {
      attachment_formats_out.stencil_attachment_format = texture->pixelFormat();
    }
  }
  return true;
}

bool MetalRenderTargetCache::GetCurrentTransferAttachmentFormats(
    TransferAttachmentFormats& attachment_formats_out) const {
  attachment_formats_out.color_attachment_formats.fill(MTL::PixelFormatInvalid);
  attachment_formats_out.depth_attachment_format = MTL::PixelFormatInvalid;
  attachment_formats_out.stencil_attachment_format = MTL::PixelFormatInvalid;

  bool has_color_attachment = false;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MTL::Texture* texture = current_color_targets_[i]
                                ? current_color_targets_[i]->draw_texture()
                                : nullptr;
    if (!texture) {
      continue;
    }
    attachment_formats_out.color_attachment_formats[i] = texture->pixelFormat();
    has_color_attachment = true;
  }
  if (!has_color_attachment) {
    attachment_formats_out.color_attachment_formats[0] =
        GetColorDrawPixelFormat(xenos::ColorRenderTargetFormat::k_8_8_8_8);
  }

  MTL::Texture* depth_texture =
      current_depth_target_ ? current_depth_target_->draw_texture() : nullptr;
  if (depth_texture) {
    MTL::PixelFormat depth_pixel_format = depth_texture->pixelFormat();
    attachment_formats_out.depth_attachment_format = depth_pixel_format;
    if (depth_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatX32_Stencil8) {
      attachment_formats_out.stencil_attachment_format = depth_pixel_format;
    }
  }
  return true;
}

bool MetalRenderTargetCache::PreflightPendingDrawPassTransfers(
    const TransferAttachmentFormats& attachment_formats) {
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }

  // Shared RenderTargetCache state uses slot 0 for depth and slots 1..4 for
  // color, so preflight depth transfers against the active depth attachment.
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_draw_pass_transfer_mask_ & (uint32_t(1) << i))) {
      continue;
    }

    auto* dest_metal_rt =
        static_cast<MetalRenderTarget*>(pending_draw_pass_render_targets_[i]);
    if (!dest_metal_rt || pending_draw_pass_transfers_[i].empty()) {
      return false;
    }

    RenderTargetKey dest_key = dest_metal_rt->key();
    std::vector<TransferRectanglePlan> pending_rectangles;
    if (!BuildTransferRectanglePlans(dest_key, pending_draw_pass_transfers_[i],
                                     nullptr, true, pending_rectangles)) {
      return false;
    }
    bool dest_is_uint = false;
    uint32_t color_attachment_index = 0;
    MTL::PixelFormat dest_format = MTL::PixelFormatInvalid;
    if (dest_key.is_depth) {
      if (i != 0) {
        return false;
      }
      dest_format = GetDepthPixelFormat(dest_key.GetDepthFormat());
      MTL::Texture* depth_texture = dest_metal_rt->draw_texture();
      if (!depth_texture || depth_texture->pixelFormat() != dest_format ||
          attachment_formats.depth_attachment_format != dest_format ||
          !GetTransferDepthStencilState(true)) {
        return false;
      }
      if (dest_format == MTL::PixelFormatDepth32Float_Stencil8 ||
          dest_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
        if (attachment_formats.stencil_attachment_format != dest_format ||
            !GetTransferStencilClearState()) {
          return false;
        }
        bool native_stencil_output_ready =
            REXCVAR_GET(metal_transfer_native_stencil_output) &&
            GetTransferStencilOutputState();
        if (!native_stencil_output_ready) {
          for (uint32_t bit = 0; bit < 8; ++bit) {
            if (!GetTransferStencilBitState(bit)) {
              return false;
            }
          }
        }
      }
      uint32_t sample_count = MsaaSamplesToCount(dest_key.msaa_samples);
      if (!GetOrCreateTransferClearPipeline(
              dest_format, false, true, sample_count, 0,
              &attachment_formats.color_attachment_formats,
              attachment_formats.depth_attachment_format,
              attachment_formats.stencil_attachment_format)) {
        return false;
      }
    } else {
      if (i == 0) {
        return false;
      }
      color_attachment_index = i - 1;
      MTL::Texture* attachment_texture = dest_metal_rt->draw_texture();
      if (!attachment_texture) {
        return false;
      }
      dest_format = GetColorOwnershipTransferPixelFormat(
          dest_key.GetColorFormat(), &dest_is_uint);
      if (dest_is_uint ||
          attachment_formats.color_attachment_formats[color_attachment_index] !=
              dest_format ||
          dest_metal_rt->transfer_texture() != attachment_texture ||
          !GetTransferNoDepthStencilState()) {
        return false;
      }
    }

    auto is_active_draw_pass_texture = [&](const MetalRenderTarget* rt,
                                           MTL::Texture* texture) -> bool {
      for (uint32_t active_index = 0;
           active_index <= xenos::kMaxColorRenderTargets; ++active_index) {
        auto* active_rt = static_cast<MetalRenderTarget*>(
            pending_draw_pass_render_targets_[active_index]);
        if (!active_rt) {
          continue;
        }
        MTL::Texture* active_draw_texture = active_rt->draw_texture();
        if (rt == active_rt || texture == active_draw_texture ||
            (rt && rt->draw_texture() == active_draw_texture)) {
          return true;
        }
      }
      return false;
    };

    for (const Transfer& transfer : pending_draw_pass_transfers_[i]) {
      auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
      if (!source_rt) {
        return false;
      }
      RenderTargetKey source_key = source_rt->key();
      MetalRenderTarget* host_depth_rt = nullptr;
      RenderTargetKey host_depth_key;
      bool host_depth_is_copy = false;
      if (transfer.host_depth_source) {
        host_depth_rt =
            static_cast<MetalRenderTarget*>(transfer.host_depth_source);
        if (!dest_key.is_depth || !host_depth_rt) {
          return false;
        }
        host_depth_key = host_depth_rt->key();
      }
      if (host_depth_rt == dest_metal_rt && host_depth_rt) {
        // [tile-edram] K1: host depth already copied to the EDRAM buffer.
        if (!pending_draw_pass_self_host_depth_stored_ || !edram_buffer_) {
          return false;
        }
        host_depth_is_copy = true;
      } else if (host_depth_rt) {
        if (!host_depth_key.is_depth) {
          return false;
        }
        MTL::Texture* host_depth_texture = host_depth_rt->texture();
        if (!host_depth_texture ||
            host_depth_texture != host_depth_rt->draw_texture() ||
            host_depth_texture == dest_metal_rt->draw_texture() ||
            host_depth_texture->pixelFormat() !=
                GetDepthPixelFormat(host_depth_key.GetDepthFormat()) ||
            is_active_draw_pass_texture(host_depth_rt, host_depth_texture)) {
          return false;
        }
      }
      if (source_key.is_depth && !GetStencilTextureView(source_rt)) {
        return false;
      }
      bool dest_sample_id_from_sample_default =
          dest_key.msaa_samples != xenos::MsaaSamples::k1X &&
          REXCVAR_GET(metal_transfer_msaa_sample_id);
      TransferShaderKey shader_key = GetTransferShaderKey(
          source_key, dest_key, host_depth_rt ? &host_depth_key : nullptr,
          host_depth_is_copy, false, dest_sample_id_from_sample_default);
      if (!GetOrCreateTransferPipelines(
              shader_key, dest_format, false, false, color_attachment_index,
              &attachment_formats.color_attachment_formats,
              attachment_formats.depth_attachment_format,
              attachment_formats.stencil_attachment_format)) {
        return false;
      }
      if (dest_key.is_depth) {
        TransferShaderKey stencil_shader_key =
            GetTransferShaderKey(source_key, dest_key, nullptr, false, true,
                                 dest_sample_id_from_sample_default);
        bool native_stencil_output_ready =
            REXCVAR_GET(metal_transfer_native_stencil_output) &&
            GetTransferStencilOutputState() &&
            GetOrCreateTransferPipelines(
                stencil_shader_key, dest_format, false, true,
                color_attachment_index,
                &attachment_formats.color_attachment_formats,
                attachment_formats.depth_attachment_format,
                attachment_formats.stencil_attachment_format);
        if (!native_stencil_output_ready &&
            !GetOrCreateTransferPipelines(
                stencil_shader_key, dest_format, false, false,
                color_attachment_index,
                &attachment_formats.color_attachment_formats,
                attachment_formats.depth_attachment_format,
                attachment_formats.stencil_attachment_format)) {
          return false;
        }
      }
    }
  }

  return true;
}

bool MetalRenderTargetCache::PreflightPendingDrawPassTransfers(
    MTL::RenderPassDescriptor* pass_descriptor) {
  TransferAttachmentFormats attachment_formats;
  if (!GetActiveTransferAttachmentFormats(pass_descriptor,
                                          attachment_formats)) {
    return false;
  }
  return PreflightPendingDrawPassTransfers(attachment_formats);
}

bool MetalRenderTargetCache::BuildCurrentAttachmentPlan(
    uint32_t expected_sample_count, AttachmentPlan& plan_out) {
  plan_out = AttachmentPlan();
  plan_out.coverage_samples = std::max(1u, expected_sample_count);

  pending_draw_pass_load_dontcare_mask_ = 0;
  if (HasPendingDrawPassTransfers()) {
    TransferAttachmentFormats attachment_formats;
    if (GetCurrentTransferAttachmentFormats(attachment_formats) &&
        PreflightPendingDrawPassTransfers(attachment_formats)) {
      for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
        if ((pending_draw_pass_transfer_mask_ & (uint32_t(1) << i)) &&
            (pending_draw_pass_full_overwrite_mask_ & (uint32_t(1) << i))) {
          pending_draw_pass_load_dontcare_mask_ |= uint32_t(1) << i;
        }
      }
    }
  }
  auto update_coverage = [&](MTL::Texture* texture) {
    if (!texture || plan_out.coverage_width) {
      return;
    }
    plan_out.coverage_width = static_cast<uint32_t>(texture->width());
    plan_out.coverage_height = static_cast<uint32_t>(texture->height());
    if (texture->sampleCount() > 0) {
      plan_out.coverage_samples =
          std::max<uint32_t>(plan_out.coverage_samples,
                             static_cast<uint32_t>(texture->sampleCount()));
    }
  };
  auto fill_attachment = [&](AttachmentPlanAttachment& attachment,
                             MetalRenderTarget* render_target,
                             uint32_t pending_index) {
    if (!render_target || !render_target->texture()) {
      return;
    }
    attachment.render_target = render_target;
    attachment.texture = render_target->draw_texture();
    attachment.bound = attachment.texture != nullptr;
    attachment.needs_initial_clear = render_target->needs_initial_clear();
    const uint32_t pending_bit = uint32_t(1) << pending_index;
    attachment.load_action_safe =
        (pending_draw_pass_load_dontcare_mask_ & pending_bit) != 0;
    if (attachment.bound) {
      update_coverage(attachment.texture);
    }
  };

  fill_attachment(plan_out.depth, current_depth_target_, 0);
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    fill_attachment(plan_out.colors[i], current_color_targets_[i], i + 1);
    if (plan_out.colors[i].bound) {
      plan_out.has_any_color_target = true;
    }
  }
  return true;
}

bool MetalRenderTargetCache::EncodePendingDrawPassTransfers(
    MTL::RenderCommandEncoder* encoder,
    MTL::RenderPassDescriptor* pass_descriptor,
    DrawPassTransferEncoderMutationMask* mutations_out) {
  if (mutations_out) {
    *mutations_out = kDrawPassTransferEncoderMutationNone;
  }
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  if (!encoder) {
    return false;
  }

  // [deferred-clears] resolve clears first: they happened before this
  // update's transfers (which never touch the same target, see Update).
  if (pending_draw_pass_clear_mask_) {
    TransferAttachmentFormats active_formats;
    if (!GetActiveTransferAttachmentFormats(pass_descriptor, active_formats)) {
      return false;
    }
    for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
      if (!(pending_draw_pass_clear_mask_ & (uint32_t(1) << i))) continue;
      auto* rt =
          static_cast<MetalRenderTarget*>(pending_draw_pass_clear_targets_[i]);
      if (!rt || !CanDrawDeferredClearInPass(i, rt, active_formats)) {
        return false;
      }
      // The attachment must be exactly the texture the separate pass clears.
      if (i == 0) {
        auto* da = pass_descriptor->depthAttachment();
        if (!da || da->texture() != rt->texture()) return false;
      } else {
        auto* ca = pass_descriptor->colorAttachments()->object(i - 1);
        if (!ca || ca->texture() != rt->transfer_texture()) return false;
      }
      // Encoder defaults for the state a separate clear pass would start
      // with and that the clear does not set itself.
      ResetDrawPassRasterizerState(encoder);
      if (mutations_out) {
        *mutations_out |= kDrawPassTransferEncoderMutationRasterizer;
      }
      std::array<RenderTarget*, 1 + xenos::kMaxColorRenderTargets> targets =
          {};
      targets[i] = rt;
      std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
          no_transfers;
      std::array<uint64_t, 1 + xenos::kMaxColorRenderTargets> values = {};
      values[i] = pending_draw_pass_clear_values_[i];
      DrawPassTransferEncoderMutationMask clear_mutations =
          kDrawPassTransferEncoderMutationNone;
      bool ok = PerformTransfersAndResolveClears(
          1 + xenos::kMaxColorRenderTargets, targets.data(),
          no_transfers.data(), values.data(), &pending_draw_pass_clear_rects_[i],
          nullptr, encoder, pass_descriptor, &clear_mutations);
      if (mutations_out) {
        *mutations_out |= clear_mutations;
      }
      if (!ok) {
        return false;
      }
      UploadTraceCounters& u = UploadTrace();
      ++u.dc_merged;
      bool full_target = false;
      u.dc_saved_kb += DeferredClearTouchedKb(
          rt, pending_draw_pass_clear_rects_[i], RenderTargetScaleX(rt->key()),
          RenderTargetScaleY(rt->key()), &full_target);
      if (full_target) {
        ++u.dc_fullrect;
      }
      pending_draw_pass_clear_targets_[i] = nullptr;
      pending_draw_pass_clear_mask_ &= ~(uint32_t(1) << i);
    }
  }
  if (!pending_draw_pass_transfer_mask_) {
    ClearPendingDrawPassTransfers();
    return true;
  }

  if (!PreflightPendingDrawPassTransfers(pass_descriptor)) {
    return false;
  }

  // [raster-state] the transfer quads would otherwise run with the cull mode,
  // winding, fill mode, depth bias and depth clip of the last draw in a
  // reused encoder; a separate transfer pass starts at the encoder defaults.
  if (TransferQuadStateResetEnabled()) {
    ResetDrawPassRasterizerState(encoder);
    if (mutations_out) {
      *mutations_out |= kDrawPassTransferEncoderMutationRasterizer;
    }
  }

  DrawPassTransferEncoderMutationMask transfer_mutations =
      kDrawPassTransferEncoderMutationNone;
  bool success = PerformTransfersAndResolveClears(
      1 + xenos::kMaxColorRenderTargets,
      pending_draw_pass_render_targets_.data(),
      pending_draw_pass_transfers_.data(), nullptr, nullptr, nullptr, encoder,
      pass_descriptor, &transfer_mutations);
  if (mutations_out) {
    *mutations_out |= transfer_mutations;
  }
  if (success && pending_draw_pass_post_clear_) {
    // [ssm-merge] the stencil-only clear after the transfer quads, in the
    // same pass (a separate clear pass after the transfer pass before).
    const DeferredClear& pc = pending_draw_pass_post_clear_value_;
    TransferAttachmentFormats active_formats;
    auto* da = pass_descriptor->depthAttachment();
    if (!GetActiveTransferAttachmentFormats(pass_descriptor, active_formats) ||
        !CanDrawDeferredClearInPass(0, pc.render_target, active_formats) ||
        !da || da->texture() != pc.render_target->texture()) {
      return false;
    }
    ResetDrawPassRasterizerState(encoder);
    if (mutations_out) {
      *mutations_out |= kDrawPassTransferEncoderMutationRasterizer;
    }
    std::array<RenderTarget*, 1 + xenos::kMaxColorRenderTargets> targets = {};
    targets[0] = pc.render_target;
    std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
        no_transfers;
    std::array<uint64_t, 1 + xenos::kMaxColorRenderTargets> values = {};
    values[0] = pc.clear_value;
    DrawPassTransferEncoderMutationMask clear_mutations =
        kDrawPassTransferEncoderMutationNone;
    success = PerformTransfersAndResolveClears(
        1 + xenos::kMaxColorRenderTargets, targets.data(), no_transfers.data(),
        values.data(), &pc.rectangle, nullptr, encoder, pass_descriptor,
        &clear_mutations);
    if (mutations_out) {
      *mutations_out |= clear_mutations;
    }
    if (success) {
      ++UploadTrace().ssm_merged;
    }
  }
  if (success) {
    ClearPendingDrawPassTransfers();
  }
  return success;
}

bool MetalRenderTargetCache::FlushPendingDrawPassTransfers(
    bool requeue_clears) {
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  if (requeue_clears) {
    // [deferred-clears] the draw that bound these targets was not encoded,
    // so nothing has read them: the clears go back to the deferred list.
    // [pass-merge] except a clear of a target that also has transfers
    // pending here: those are encoded next, and the clear comes first.
    for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
      if (!(pending_draw_pass_clear_mask_ & (uint32_t(1) << i))) continue;
      if (pending_draw_pass_clear_targets_[i] &&
          (pending_draw_pass_transfer_mask_ & (uint32_t(1) << i))) {
        DeferredClear clear;
        clear.render_target = static_cast<MetalRenderTarget*>(
            pending_draw_pass_clear_targets_[i]);
        clear.clear_value = pending_draw_pass_clear_values_[i];
        clear.rectangle = pending_draw_pass_clear_rects_[i];
        pending_draw_pass_clear_targets_[i] = nullptr;
        NoteDeferredClearFlush(DeferredClearFlushReason::kGuard, clear);
        if (!EncodeResolveClearPass(clear.render_target, clear.clear_value,
                                    clear.rectangle)) {
          return false;
        }
        continue;
      }
      if (pending_draw_pass_clear_targets_[i]) {
        DeferredClear clear;
        clear.render_target = static_cast<MetalRenderTarget*>(
            pending_draw_pass_clear_targets_[i]);
        clear.clear_value = pending_draw_pass_clear_values_[i];
        clear.rectangle = pending_draw_pass_clear_rects_[i];
        deferred_clears_.push_back(clear);
        ++UploadTrace().dc_requeue;
      }
      pending_draw_pass_clear_targets_[i] = nullptr;
    }
    pending_draw_pass_clear_mask_ = 0;
  } else if (!FlushPendingDrawPassClears(DeferredClearFlushReason::kGuard)) {
    return false;
  }
  if (!pending_draw_pass_transfer_mask_) {
    ClearPendingDrawPassTransfers();
    return true;
  }
  bool success = PerformTransfersAndResolveClears(
      1 + xenos::kMaxColorRenderTargets,
      pending_draw_pass_render_targets_.data(),
      pending_draw_pass_transfers_.data(), nullptr, nullptr, nullptr, nullptr,
      nullptr, nullptr);
  if (!success) {
    return false;
  }
  if (pending_draw_pass_post_clear_) {
    // [ssm-merge] the stencil clear still follows the transfers: deferred
    // again when the draw was not encoded (nothing read the target), else
    // its own pass now.
    DeferredClear pc = pending_draw_pass_post_clear_value_;
    pending_draw_pass_post_clear_ = false;
    ++UploadTrace().ssm_flushed;
    if (requeue_clears) {
      deferred_clears_.push_back(pc);
    } else if (!EncodeResolveClearPass(pc.render_target, pc.clear_value,
                                       pc.rectangle)) {
      return false;
    }
  }
  ClearPendingDrawPassTransfers();
  return true;
}

MetalRenderTargetCache::TransferShaderKey
MetalRenderTargetCache::GetTransferShaderKey(
    RenderTargetKey source_key, RenderTargetKey dest_key,
    const RenderTargetKey* host_depth_source_key,
    bool host_depth_source_is_copy, bool stencil_bit,
    bool dest_sample_id_from_sample_default) const {
  TransferShaderKey shader_key = {};
  shader_key.source_msaa_samples = source_key.msaa_samples;
  shader_key.dest_msaa_samples = dest_key.msaa_samples;
  shader_key.source_resource_format = source_key.resource_format;
  shader_key.dest_resource_format = dest_key.resource_format;
  shader_key.host_depth_source_msaa_samples = xenos::MsaaSamples::k1X;
  shader_key.host_depth_source_is_copy = 0;

  if (stencil_bit) {
    shader_key.mode = source_key.is_depth ? TransferMode::kDepthToStencilBit
                                          : TransferMode::kColorToStencilBit;
  } else if (dest_key.is_depth) {
    if (host_depth_source_key) {
      shader_key.mode = source_key.is_depth
                            ? TransferMode::kDepthAndHostDepthToDepth
                            : TransferMode::kColorAndHostDepthToDepth;
      shader_key.host_depth_source_is_copy = host_depth_source_is_copy ? 1 : 0;
      shader_key.host_depth_source_msaa_samples =
          host_depth_source_is_copy ? xenos::MsaaSamples::k1X
                                    : host_depth_source_key->msaa_samples;
    } else {
      shader_key.mode = source_key.is_depth ? TransferMode::kDepthToDepth
                                            : TransferMode::kColorToDepth;
    }
  } else {
    shader_key.mode = source_key.is_depth ? TransferMode::kDepthToColor
                                          : TransferMode::kColorToColor;
  }

  const TransferModeInfo& mode_info =
      kTransferModeInfos[size_t(shader_key.mode)];
  bool transfer_use_sample_id = dest_sample_id_from_sample_default;
  if (transfer_use_sample_id) {
    bool source_is_multisample =
        source_key.msaa_samples != xenos::MsaaSamples::k1X;
    bool host_depth_is_multisample =
        mode_info.uses_host_depth &&
        shader_key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X &&
        !shader_key.host_depth_source_is_copy;
    if (!source_is_multisample && !host_depth_is_multisample) {
      transfer_use_sample_id = false;
    }
  }
  shader_key.dest_sample_id_from_sample = transfer_use_sample_id ? 1u : 0u;
  return shader_key;
}

void MetalRenderTargetCache::GetMemReportInfo(MemReportInfo& out) const {
  struct Row {
    uint64_t bytes;
    std::string text;
  };
  std::vector<Row> rows;
  for (const auto& [k, rt] : render_target_map_) {
    if (!rt) continue;
    uint64_t bytes = 0;
    if (rt->texture()) bytes += uint64_t(rt->texture()->allocatedSize());
    if (rt->msaa_texture()) {
      bytes += uint64_t(rt->msaa_texture()->allocatedSize());
    }
    out.rt_bytes += bytes;
    ++out.rt_count;
    MTL::Texture* t = rt->texture();
    if (t) {
      const RenderTargetKey key = rt->key();
      uint32_t full_h = RtShrinkFullHeight(key);
      uint64_t full = t->height()
                          ? bytes * uint64_t(full_h) / uint64_t(t->height())
                          : bytes;
      out.rt_full_bytes += full;
      const uint32_t tile_h = RtShrinkTileRowHeight(key);
      const uint32_t needed_h = std::min(
          full_h, (rt->rows_needed() + tile_h - 1) / tile_h * tile_h);
      out.rt_needed_bytes +=
          t->height() ? bytes * uint64_t(std::max(needed_h, tile_h)) /
                            uint64_t(t->height())
                      : bytes;
      rows.push_back(
          {bytes, fmt::format("{} {}x{}x{} need {} full {} grows {} {:.1f}MB",
                              key.GetDebugName(), uint32_t(t->width()),
                              uint32_t(t->height()), uint32_t(t->sampleCount()),
                              rt->rows_needed(), full_h, rt->grow_count(),
                              double(bytes) / (1024.0 * 1024.0))});
    }
  }
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
  out.grows = rt_shrink_grows_;
  out.grow_bytes = rt_shrink_grow_bytes_;
  out.misses = rt_shrink_misses_;
  out.unclipped_draws = rt_shrink_unclipped_draws_;
  out.unowned_claims = rt_shrink_unowned_claims_;
  for (size_t i = 0; i < rows.size() && i < 16; ++i) {
    if (i) out.top += " | ";
    out.top += rows[i].text;
  }
  if (render_target_heap_pool_) {
    out.heap_bytes = render_target_heap_pool_->total_heap_bytes();
    out.heap_used_bytes = render_target_heap_pool_->used_heap_bytes();
    out.heap_count = render_target_heap_pool_->heap_count();
  }
  if (edram_buffer_) out.edram_bytes = uint64_t(edram_buffer_->allocatedSize());
  auto add_tex = [&](MTL::Texture* t) {
    if (t && t->storageMode() != MTL::StorageModeMemoryless) {
      out.other_bytes += uint64_t(t->allocatedSize());
    }
  };
  if (dummy_color_target_owner_) add_tex(dummy_color_target_owner_->texture());
  for (int i = 0; i < 3; ++i) {
    add_tex(transfer_dummy_color_float_[i]);
    add_tex(transfer_dummy_color_uint_[i]);
    add_tex(transfer_dummy_depth_[i]);
    add_tex(transfer_dummy_stencil_[i]);
  }
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetWidth() const {
  // Metal maximum texture dimension
  return 16384;
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetHeight() const {
  // Metal maximum texture dimension
  return 16384;
}

bool MetalRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

void MetalRenderTargetCache::RequestPixelShaderInterlockBarrier() {
  command_processor_.EndRenderEncoder();
  PixelShaderInterlockFullEdramBarrierPlaced();
}

RenderTargetCache::RenderTarget* MetalRenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  // Calculate dimensions
  uint32_t width = key.GetWidth();

  // Apply resolution scaling (native-scale effect targets stay at 1x).
  width *= RenderTargetScaleX(key);
  const uint32_t full_height = RtShrinkFullHeight(key);
  // [rt-shrink] start with one tile row; ownership claims and draw scissors
  // grow it before anything addresses more rows (RtShrinkApplyGrowth).
  uint32_t height = full_height;
  if (RtShrinkMode() == 1) {
    height = std::min(full_height, RtShrinkTileRowHeight(key));
  }

  // Create Metal render target
  auto* render_target = new MetalRenderTarget(key);
  render_target->set_full_height(full_height);

  // Create the texture based on format
  MTL::Texture* texture = nullptr;
  uint32_t samples = 1 << uint32_t(key.msaa_samples);

  if (key.is_depth) {
    texture = CreateDepthTexture(width, height, key.GetDepthFormat(), samples);
  } else {
    texture = CreateColorTexture(width, height, key.GetColorFormat(), samples);
  }

  if (!texture) {
    delete render_target;
    return nullptr;
  }

  render_target->SetTexture(texture);
  SetUpRenderTargetViews(render_target);

  // NOTE: Unlike the previous implementation, we do NOT load EDRAM data here.
  // This matches D3D12's approach where:
  // 1. CreateRenderTarget creates an empty texture
  // 2. Data transfer happens via ownership transfers in
  // PerformTransfersAndResolveClears
  // 3. The EDRAM buffer is only used as scratch space for resolves
  //
  // The ownership transfer system (called from Update()) handles copying data
  // between render target textures when EDRAM regions are aliased between
  // different RT configurations.

  // Store in our map for later retrieval
  render_target_map_[key.key] = render_target;

  return render_target;
}

void MetalRenderTargetCache::SetUpRenderTargetViews(
    MetalRenderTarget* render_target) {
  const RenderTargetKey key = render_target->key();
  MTL::Texture* texture = render_target->texture();
  if (key.is_depth || !texture) {
    return;
  }
  MTL::PixelFormat resource_format =
      GetColorResourcePixelFormat(key.GetColorFormat());
  MTL::PixelFormat draw_format = GetColorDrawPixelFormat(key.GetColorFormat());
  MTL::PixelFormat transfer_format =
      GetColorOwnershipTransferPixelFormat(key.GetColorFormat(), nullptr);
  if (draw_format != resource_format) {
    MTL::Texture* draw_view = texture->newTextureView(draw_format);
    if (!draw_view) {
      REXLOG_ERROR("Failed to create texture view for render target");
    }
    render_target->SetDrawTexture(draw_view);
  }
  if (transfer_format != resource_format) {
    MTL::Texture* transfer_view = texture->newTextureView(transfer_format);
    if (!transfer_view) {
      REXLOG_ERROR("Failed to create texture view for render target");
    }
    render_target->SetTransferTexture(transfer_view);
  }
  if (render_target->msaa_texture()) {
    if (draw_format != render_target->msaa_texture()->pixelFormat()) {
      MTL::Texture* msaa_draw_view =
          render_target->msaa_texture()->newTextureView(draw_format);
      if (!msaa_draw_view) {
        REXLOG_ERROR("Failed to create texture view for render target");
      }
      render_target->SetMsaaDrawTexture(msaa_draw_view);
    }
    if (transfer_format != render_target->msaa_texture()->pixelFormat()) {
      MTL::Texture* msaa_transfer_view =
          render_target->msaa_texture()->newTextureView(transfer_format);
      if (!msaa_transfer_view) {
        REXLOG_ERROR("Failed to create texture view for render target");
      }
      render_target->SetMsaaTransferTexture(msaa_transfer_view);
    }
  }
}

// ---------------------------------------------------------------------------
// [rt-shrink] Lazily sized host render targets (FABLE2_RT_SHRINK,
// docs/resolution-scale.md section 21).
//
// A host render target used to span the whole EDRAM height times the
// resolution scale (e.g. 960x16368 for a 320-wide effect target at 3x) from
// creation, although the game only ever addresses the rows of its viewport.
// Now the texture covers only the rows addressed so far and grows (new
// texture, blit of the old rows, the initial clear value in the new rows when
// the first bind had cleared the texture) before anything addresses more.
// Every texel access derives from one of:
// - draws: the pixels are inside the scissor, which is clamped to the render
//   area (RtShrinkNoteDraw grows every attachment of the pass to the scissor
//   bottom and to a common height, so the render area and the initial clear
//   cover the same rows as before);
// - EDRAM ownership claims (draws, resolve clears, tile conversion; the SDK
//   hook OnOwnershipClaimed): transfers into and out of a target, resolves,
//   dumps, resolve clears and host-depth copies all address rows within ranges
//   the target has claimed.
// Growth is applied right after those points (Update, Resolve, tile
// conversion) and always before the GPU work that addresses the rows.
// FABLE2_RT_SHRINK=0 restores the EDRAM-height textures; =census keeps them
// and only tracks the rows (memory report).
// ---------------------------------------------------------------------------
uint32_t MetalRenderTargetCache::RtShrinkMode() {
  static const uint32_t mode = [] {
    const char* v = std::getenv("FABLE2_RT_SHRINK");
    uint32_t m = 1;
    if (v && v[0] == '0') m = 0;
    if (v && std::strcmp(v, "census") == 0) m = 2;
    REXLOG_INFO("[rt-shrink] {} (FABLE2_RT_SHRINK)",
                m == 1 ? "on" : (m == 2 ? "census" : "off"));
    return m;
  }();
  return mode;
}

bool MetalRenderTargetCache::RtShrinkVerify() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_RT_SHRINK_VERIFY");
    bool e = v && v[0] == '1';
    if (e) REXLOG_WARN("[rt-shrink] FABLE2_RT_SHRINK_VERIFY=1 (diagnostic)");
    return e;
  }();
  return on;
}

// FABLE2_RT_SHRINK_TRACE: read once. It used to be a getenv per draw and per
// ownership claim (~5% of the GPU Commands thread at 1x, docs/archive/perf-gpucmd-20261012.md).
// FABLE2_GETENV_PER_CALL=1 (A/B diagnostic only) reads it on every call again.
bool MetalRenderTargetCache::RtShrinkTrace() {
  static const bool on = std::getenv("FABLE2_RT_SHRINK_TRACE") != nullptr;
  static const bool per_call = [] {
    const char* v = std::getenv("FABLE2_GETENV_PER_CALL");
    return v && v[0] == '1';
  }();
  if (per_call) {
    return std::getenv("FABLE2_RT_SHRINK_TRACE") != nullptr;  // getenv-ok: A/B diagnostic only
  }
  return on;
}

uint32_t MetalRenderTargetCache::RtShrinkFullHeight(RenderTargetKey key) const {
  return GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) *
         RenderTargetScaleY(key);
}

uint32_t MetalRenderTargetCache::RtShrinkTileRowHeight(
    RenderTargetKey key) const {
  return (xenos::kEdramTileHeightSamples >>
          uint32_t(key.msaa_samples >= xenos::MsaaSamples::k2X)) *
         RenderTargetScaleY(key);
}

void MetalRenderTargetCache::OnOwnershipClaimed(
    RenderTargetKey dest, uint32_t end_tiles_base_relative) {
  if (!RtShrinkMode()) {
    return;
  }
  // Negative control for the verify mode (UNSAFE, image wrong): ignore
  // claims, so only draws size the textures.
  static const bool diag_noclaim = [] {
    const char* v = std::getenv("FABLE2_RT_SHRINK_DIAG");
    bool on = v && std::strcmp(v, "noclaim") == 0;
    if (on) REXLOG_WARN("[rt-shrink] DIAG noclaim: ownership claims ignored");
    return on;
  }();
  if (diag_noclaim) {
    return;
  }
  auto it = render_target_map_.find(dest.key);
  if (it == render_target_map_.end() || !it->second) {
    return;
  }
  // Tile t (relative to the base) is in tile row t / pitch. The divisor is the
  // pitch at 32bpp also for 64bpp targets: a conservative bound.
  const uint32_t pitch = std::max<uint32_t>(1u, dest.pitch_tiles_at_32bpp);
  const uint32_t tile_rows = (end_tiles_base_relative + pitch - 1) / pitch;
  const uint32_t rows = tile_rows * RtShrinkTileRowHeight(dest);
  if (RtShrinkTrace() &&
      std::min(rows, it->second->full_height()) > it->second->rows_needed()) {
    REXLOG_INFO("[rt-shrink] f{} claim {} end {}t -> {} rows (full {}) by {}",
                frame_log::CurrentFrame(), dest.GetDebugName(),
                end_tiles_base_relative, rows, it->second->full_height(),
                rt_shrink_claim_context_);
  }
  MetalRenderTarget* rt = it->second;
  if (RtShrinkVerify() && !rt->needs_initial_clear() &&
      std::min(rows, rt->full_height()) > rt->rows_needed()) {
    // A target that was already drawn claims rows past every earlier claim.
    // Tiles there without any owner get no transfer, so they keep what the
    // texture had: pixels an earlier draw wrote outside its own claim (the
    // draw extent estimate) before the shrink, the initial clear value or
    // nothing with it. Count them: only these could differ.
    const uint32_t tile_h = RtShrinkTileRowHeight(dest);
    const uint32_t prev_tiles = rt->rows_needed() / tile_h * pitch;
    uint32_t unowned = 0;
    for (uint32_t t = prev_tiles; t < end_tiles_base_relative; ++t) {
      if (GetTileOwner((dest.base_tiles + t) & (xenos::kEdramTileCount - 1))
              .IsEmpty()) {
        ++unowned;
      }
    }
    if (unowned) {
      ++rt_shrink_unowned_claims_;
      static uint32_t logs = 0;
      if (logs < 32) {
        ++logs;
        REXLOG_INFO("[rt-shrink] f{} claim of {} unowned tiles past rows {} "
                    "by used {} ({})",
                    frame_log::CurrentFrame(), unowned, rt->rows_needed(),
                    dest.GetDebugName(), rt_shrink_claim_context_);
      }
    }
  }
  RtShrinkNoteRows(rt, rows);
}

void MetalRenderTargetCache::RtShrinkNoteRows(MetalRenderTarget* rt,
                                              uint32_t rows) {
  if (!rt) {
    return;
  }
  rows = std::min(rows, rt->full_height());
  if (rows > rt->rows_needed()) {
    rt->set_rows_needed(rows);
  }
  if (RtShrinkMode() == 1 && rt->texture() &&
      rows > uint32_t(rt->texture()->height()) && !rt->rts_pending()) {
    rt->set_rts_pending(true);
    rt_shrink_pending_.push_back(rt);
  }
}

void MetalRenderTargetCache::RtShrinkNoteDraw(
    RenderTarget* const* accumulated_targets) {
  const uint32_t mode = RtShrinkMode();
  if (!mode) {
    return;
  }
  // The draw's own extent is its ownership claim (the SDK's draw extent
  // estimate: viewport and scissor, or the vertex positions for unclipped
  // draws), already in rows_needed. The guest scissor is usually the
  // 8192x8192 Direct3D 9 default, so it bounds nothing.
  // All attachments of a pass share the MSAA mode and the scale (native-scale
  // passes are all-or-none), so they share the tile row height. They get one
  // common height (clamped to each target's own full height), so the render
  // area and a first-bind clear cover each texture exactly as before.
  uint32_t need = 0, max_height = 0, tile_h = 0;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    auto* rt = static_cast<MetalRenderTarget*>(accumulated_targets[i]);
    if (!rt) continue;
    tile_h = std::max(tile_h, RtShrinkTileRowHeight(rt->key()));
    need = std::max(need, rt->rows_needed());
    if (mode == 1 && rt->texture()) {
      max_height = std::max(max_height, uint32_t(rt->texture()->height()));
    }
  }
  if (!tile_h) {
    return;
  }
  uint32_t height = std::max(need, max_height);
  height = (height + tile_h - 1) / tile_h * tile_h;
  if (mode == 1 && height > max_height && max_height > tile_h) {
    // Growing targets that already have rows: leave some headroom so a
    // slowly growing extent does not copy every frame.
    const uint32_t slack = max_height + max_height / 8;
    height = std::max(height, (slack + tile_h - 1) / tile_h * tile_h);
  }
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    auto* rt = static_cast<MetalRenderTarget*>(accumulated_targets[i]);
    if (!rt) continue;
    if (RtShrinkTrace() &&
        std::min(height, rt->full_height()) > rt->rows_needed()) {
      REXLOG_INFO("[rt-shrink] f{} pass {} need {} height {} -> {}",
                  frame_log::CurrentFrame(), rt->key().GetDebugName(), need,
                  max_height, height);
    }
    RtShrinkNoteRows(rt, height);
  }
}

bool MetalRenderTargetCache::RtShrinkApplyGrowth() {
  if (rt_shrink_pending_.empty()) {
    return true;
  }
  bool ok = true;
  std::vector<MetalRenderTarget*> pending;
  pending.swap(rt_shrink_pending_);
  for (MetalRenderTarget* rt : pending) {
    rt->set_rts_pending(false);
    MTL::Texture* texture = rt->texture();
    if (!texture) continue;
    const uint32_t old_height = uint32_t(texture->height());
    if (rt->rows_needed() <= old_height) continue;
    const uint32_t tile_h = RtShrinkTileRowHeight(rt->key());
    uint32_t height = (rt->rows_needed() + tile_h - 1) / tile_h * tile_h;
    if (!rt->needs_initial_clear() && old_height > tile_h) {
      const uint32_t slack = old_height + old_height / 8;
      height = std::max(height, (slack + tile_h - 1) / tile_h * tile_h);
    }
    height = std::min(height, rt->full_height());
    if (!RtShrinkGrow(rt, height)) {
      ok = false;
    }
  }
  return ok;
}

bool MetalRenderTargetCache::RtShrinkGrow(MetalRenderTarget* rt,
                                          uint32_t new_height,
                                          bool discard_contents) {
  hitch::Scope hitch_scope(hitch::kRtGrow);
  MTL::Texture* old_texture = rt->texture();
  const RenderTargetKey key = rt->key();
  const uint32_t width = uint32_t(old_texture->width());
  const uint32_t old_height = uint32_t(old_texture->height());
  if (new_height <= old_height) {
    return true;
  }
  const uint32_t samples = 1u << uint32_t(key.msaa_samples);
  MTL::Texture* texture =
      key.is_depth
          ? CreateDepthTexture(width, new_height, key.GetDepthFormat(), samples)
          : CreateColorTexture(width, new_height, key.GetColorFormat(),
                               samples);
  if (!texture) {
    REXLOG_ERROR("[rt-shrink] failed to grow {} to {}x{}",
                 key.GetDebugName(), width, new_height);
    return false;
  }
  // A target that was never bound or written holds nothing yet: its first
  // bind clears the whole (new) texture. Otherwise keep its rows.
  const bool keep_contents = !rt->needs_initial_clear() && !discard_contents;
  if (keep_contents) {
    MTL::CommandBuffer* cmd = command_processor_.RequestTransferCommandBuffer(
        MetalCommandProcessor::TransferRequestSource::kRenderTargetTransfer);
    if (!cmd) {
      texture->release();
      REXLOG_ERROR("[rt-shrink] no command buffer to grow {}",
                   key.GetDebugName());
      return false;
    }
    if (rt->initial_clear_done()) {
      // Rows the first bind cleared before the shrink: same clear value.
      MTL::RenderPassDescriptor* desc =
          MTL::RenderPassDescriptor::renderPassDescriptor();
      if (key.is_depth) {
        auto* depth = desc->depthAttachment();
        depth->setTexture(texture);
        depth->setLoadAction(MTL::LoadActionClear);
        depth->setStoreAction(MTL::StoreActionStore);
        depth->setClearDepth(key.GetDepthFormat() ==
                                     xenos::DepthRenderTargetFormat::kD24FS8
                                 ? 0.0
                                 : 1.0);
        MTL::PixelFormat f = texture->pixelFormat();
        if (f == MTL::PixelFormatDepth32Float_Stencil8 ||
            f == MTL::PixelFormatDepth24Unorm_Stencil8 ||
            f == MTL::PixelFormatX32_Stencil8) {
          auto* stencil = desc->stencilAttachment();
          stencil->setTexture(texture);
          stencil->setLoadAction(MTL::LoadActionClear);
          stencil->setStoreAction(MTL::StoreActionStore);
          stencil->setClearStencil(0);
        }
      } else {
        auto* color = desc->colorAttachments()->object(0);
        color->setTexture(texture);
        color->setLoadAction(MTL::LoadActionClear);
        color->setStoreAction(MTL::StoreActionStore);
        color->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 0.0));
      }
      MTL::RenderCommandEncoder* clear_encoder =
          cmd->renderCommandEncoder(desc);
      if (clear_encoder) {
        clear_encoder->endEncoding();
      }
    }
    MTL::BlitCommandEncoder* blit = pass_trace::Blit(cmd, "rt_shrink_grow");
    if (!blit) {
      texture->release();
      REXLOG_ERROR("[rt-shrink] no blit encoder to grow {}",
                   key.GetDebugName());
      return false;
    }
    blit->copyFromTexture(old_texture, 0, 0, MTL::Origin::Make(0, 0, 0),
                          MTL::Size::Make(width, old_height, 1), texture, 0, 0,
                          MTL::Origin::Make(0, 0, 0));
    blit->endEncoding();
    UploadTraceCounters& u = UploadTrace();
    ++u.rts_grow;
    u.rts_grow_kb += uint32_t(
        (uint64_t(old_texture->allocatedSize()) + 1023) / 1024);
    rt_shrink_grow_bytes_ += uint64_t(old_texture->allocatedSize());
  }
  ++rt_shrink_grows_;
  rt->note_grow();
  static uint32_t grow_logs = 0;
  if (grow_logs < 64 || RtShrinkVerify()) {
    ++grow_logs;
    REXLOG_INFO("[rt-shrink] f{} grow {} {}x{} -> {}x{} (full {}){}",
                frame_log::CurrentFrame(), key.GetDebugName(), width,
                old_height, width, new_height, rt->full_height(),
                keep_contents ? (rt->initial_clear_done() ? " copy+clear"
                                                          : " copy")
                              : "");
  }
  // The command buffer keeps the old texture alive until the copy has run.
  rt->ReleaseTexturesForReplace();
  rt->SetTexture(texture);
  SetUpRenderTargetViews(rt);
  if (dummy_color_target_owner_ && current_depth_target_ == rt) {
    // The dummy colour target follows the depth texture's size.
    dummy_color_target_shape_key_ = 0;
  }
  MarkRenderPassDescriptorDirty();
  return true;
}

void MetalRenderTargetCache::RtShrinkCheckTiles(const MetalRenderTarget* rt,
                                                uint32_t start_tiles,
                                                uint32_t end_tiles,
                                                const char* what) {
  if (!rt || end_tiles <= start_tiles || !RtShrinkVerify()) {
    return;
  }
  const RenderTargetKey key = rt->key();
  // Relative to the base, with EDRAM addressing wrapping.
  uint32_t rel_start =
      (start_tiles - key.base_tiles) & (xenos::kEdramTileCount - 1);
  uint32_t rel_end = rel_start + (end_tiles - start_tiles);
  const uint32_t pitch = std::max<uint32_t>(1u, key.pitch_tiles_at_32bpp);
  RtShrinkCheckRows(rt,
                    (rel_end + pitch - 1) / pitch * RtShrinkTileRowHeight(key),
                    what);
}

void MetalRenderTargetCache::RtShrinkCheckTransfer(const MetalRenderTarget* rt,
                                                   const Transfer& transfer,
                                                   const char* what) {
  if (!rt || !RtShrinkVerify()) {
    return;
  }
  const RenderTargetKey key = rt->key();
  Transfer::Rectangle rects[Transfer::kMaxRectanglesWithCutout];
  uint32_t n = transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(),
                                      key.msaa_samples, key.Is64bpp(), rects);
  uint32_t bottom = 0;
  for (uint32_t i = 0; i < n; ++i) {
    bottom = std::max(bottom, rects[i].y_pixels + rects[i].height_pixels);
  }
  RtShrinkCheckRows(rt, bottom * RenderTargetScaleY(key), what);
}

void MetalRenderTargetCache::RtShrinkCheckDraw(uint32_t scissor_bottom_rows,
                                               uint32_t viewport_bottom_rows,
                                               bool clip_disabled) {
  if (!RtShrinkVerify()) {
    return;
  }
  // With clipping on, the host rasterizes inside the viewport and the
  // scissor; unclipped draws are bounded by the vertex positions (the claim),
  // which only the GPU knows: count them.
  if (clip_disabled) {
    ++rt_shrink_unclipped_draws_;
    return;
  }
  const uint32_t rows = std::min(scissor_bottom_rows, viewport_bottom_rows);
  RtShrinkCheckRows(current_depth_target_, rows, "draw");
  for (MetalRenderTarget* rt : current_color_targets_) {
    RtShrinkCheckRows(rt, rows, "draw");
  }
}

void MetalRenderTargetCache::RtShrinkCheckRows(const MetalRenderTarget* rt,
                                               uint32_t rows,
                                               const char* what) {
  if (!rt || !rt->texture() || RtShrinkMode() != 1) {
    return;
  }
  rows = std::min(rows, rt->full_height());
  if (rows <= uint32_t(rt->texture()->height())) {
    return;
  }
  ++rt_shrink_misses_;
  static uint32_t logs = 0;
  if (logs < 64) {
    ++logs;
    REXLOG_WARN("[rt-shrink] MISS f{} {}: {} addresses rows {} > texture {} "
                "(full {})",
                frame_log::CurrentFrame(), what, rt->key().GetDebugName(), rows,
                uint32_t(rt->texture()->height()), rt->full_height());
  }
}

bool MetalRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  // Metal uses different depth encoding than Xbox 360
  // D24S8 on Xbox 360 vs D32Float_S8 on Metal
  return format == xenos::DepthRenderTargetFormat::kD24S8 ||
         format == xenos::DepthRenderTargetFormat::kD24FS8;
}

void MetalRenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  (void)FlushQueuedStencilAlias();
  (void)FlushDeferredClears(DeferredClearFlushReason::kOther);
  if (!snapshot) {
    return;
  }

  if (IsDrawResolutionScaled()) {
    return;
  }

  if (GetPath() == Path::kPixelShaderInterlock) {
    if (!edram_buffer_) {
      return;
    }

    constexpr size_t kSnapshotSize = xenos::kEdramSizeBytes;
    if (void* edram_contents = edram_buffer_->contents()) {
      std::memcpy(edram_contents, snapshot, kSnapshotSize);
      return;
    }

    MTL::ResourceOptions staging_options =
        MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined;
    MTL::Buffer* staging = device_->newBuffer(kSnapshotSize, staging_options);
    if (!staging) {
      return;
    }
    void* staging_contents = staging->contents();
    if (!staging_contents) {
      staging->release();
      return;
    }
    std::memcpy(staging_contents, snapshot, kSnapshotSize);

    ScopedAutoreleasePool autorelease_pool;
    MTL::CommandBuffer* cmd =
        command_processor_.CreateStandaloneTransferCommandBuffer(
            "XeniaCB reason=edram-snapshot-restore");
    if (!cmd) {
      staging->release();
      return;
    }

    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    if (!blit) {
      cmd->release();
      staging->release();
      return;
    }

    blit->copyFromBuffer(staging, 0, edram_buffer_, 0, kSnapshotSize);
    blit->endEncoding();
    command_processor_.CommitStandaloneAndWait(cmd);
    staging->release();
    return;
  }

  RenderTarget* full_edram_rt =
      PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
          xenos::ColorRenderTargetFormat::k_32_FLOAT);
  if (!full_edram_rt) {
    return;
  }

  MetalRenderTarget* metal_rt = static_cast<MetalRenderTarget*>(full_edram_rt);
  // [rt-shrink] the upload below rewrites the whole texture from a standalone
  // command buffer: grow it to the full height without a copy first.
  if (RtShrinkMode() == 1 && metal_rt->texture() &&
      uint32_t(metal_rt->texture()->height()) < metal_rt->full_height()) {
    metal_rt->set_rows_needed(metal_rt->full_height());
    RtShrinkGrow(metal_rt, metal_rt->full_height(), true);
  }
  MTL::Texture* texture = metal_rt->texture();
  if (!texture) {
    return;
  }

  constexpr uint32_t kPitchTilesAt32bpp = 16;
  constexpr uint32_t kWidth =
      kPitchTilesAt32bpp * xenos::kEdramTileWidthSamples;
  constexpr uint32_t kTileRows = xenos::kEdramTileCount / kPitchTilesAt32bpp;
  constexpr uint32_t kHeight = kTileRows * xenos::kEdramTileHeightSamples;

  size_t staging_size = size_t(kWidth) * size_t(kHeight) * sizeof(uint32_t);
  MTL::Buffer* staging =
      device_->newBuffer(staging_size, MTL::ResourceStorageModeShared);
  if (!staging) {
    return;
  }

  auto* dst_base = static_cast<uint8_t*>(staging->contents());
  const uint8_t* src = static_cast<const uint8_t*>(snapshot);
  uint32_t bytes_per_row = kWidth * sizeof(uint32_t);

  for (uint32_t y_tile = 0; y_tile < kTileRows; ++y_tile) {
    for (uint32_t x_tile = 0; x_tile < kPitchTilesAt32bpp; ++x_tile) {
      uint32_t tile_index = y_tile * kPitchTilesAt32bpp + x_tile;
      const uint8_t* tile_src =
          src + tile_index * xenos::kEdramTileWidthSamples *
                    xenos::kEdramTileHeightSamples * sizeof(uint32_t);

      for (uint32_t sample_row = 0; sample_row < xenos::kEdramTileHeightSamples;
           ++sample_row) {
        uint32_t dst_y = y_tile * xenos::kEdramTileHeightSamples + sample_row;
        uint32_t dst_x = x_tile * xenos::kEdramTileWidthSamples;

        uint8_t* dst_row =
            dst_base + dst_y * bytes_per_row + dst_x * sizeof(uint32_t);
        const uint8_t* src_row = tile_src + sample_row *
                                                xenos::kEdramTileWidthSamples *
                                                sizeof(uint32_t);

        std::memcpy(dst_row, src_row,
                    xenos::kEdramTileWidthSamples * sizeof(uint32_t));
      }
    }
  }

  ScopedAutoreleasePool autorelease_pool;
  MTL::CommandBuffer* cmd =
      command_processor_.CreateStandaloneTransferCommandBuffer(
          "XeniaCB reason=rt-texture-upload");
  if (!cmd) {
    staging->release();
    return;
  }

  MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
  if (!blit) {
    cmd->release();
    staging->release();
    return;
  }

  blit->copyFromBuffer(staging, 0, bytes_per_row, 0,
                       MTL::Size::Make(kWidth, kHeight, 1), texture, 0, 0,
                       MTL::Origin::Make(0, 0, 0));
  blit->endEncoding();
  command_processor_.CommitStandaloneAndWait(cmd);
  staging->release();
  if (metal_rt->needs_initial_clear()) {
    metal_rt->SetNeedsInitialClear(false);
    MarkRenderPassDescriptorDirty();
  }

  // Seed edram_buffer_ with the restored full-EDRAM render target contents
  // so subsequent DumpRenderTargets and resolve passes see the same initial
  // EDRAM state as D3D12/Vulkan.
  DumpRenderTargets(0, kPitchTilesAt32bpp, kTileRows, kPitchTilesAt32bpp,
                    nullptr, "XeniaEDRAMDumpRestoreUpload");
}

MTL::Texture* MetalRenderTargetCache::CreateColorTexture(
    uint32_t width, uint32_t height, xenos::ColorRenderTargetFormat format,
    uint32_t samples, bool transient_render_target_only,
    bool allow_unpooled_fallback) {
  MTL::PixelFormat resource_format = GetColorResourcePixelFormat(format);
  MTL::PixelFormat draw_format = GetColorDrawPixelFormat(format);
  MTL::PixelFormat transfer_format =
      GetColorOwnershipTransferPixelFormat(format, nullptr);
  bool needs_pixel_format_view =
      draw_format != resource_format || transfer_format != resource_format;

  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);  // Default height if not specified
  desc->setPixelFormat(resource_format);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  MTL::TextureUsage usage = MTL::TextureUsageRenderTarget;
  if (!transient_render_target_only) {
    usage |= MTL::TextureUsageShaderRead;
  }
  if (needs_pixel_format_view) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);

  MTL::Texture* texture = nullptr;
  bool can_use_memoryless = transient_render_target_only &&
                            !needs_pixel_format_view &&
                            device_->supportsFamily(MTL::GPUFamilyApple1);
  if (can_use_memoryless) {
    // Dummy fallback color targets are transient (load/store don't care) and
    // never sampled - memoryless is optimal on Apple TBDR GPUs.
    desc->setStorageMode(MTL::StorageModeMemoryless);
    texture = device_->newTexture(desc);
  }
  if (!texture) {
    desc->setStorageMode(MTL::StorageModePrivate);
    if (render_target_heap_pool_ && !can_use_memoryless) {
      texture = render_target_heap_pool_->CreateTexture(desc);
    }
    if (!texture && (!render_target_heap_pool_ || allow_unpooled_fallback)) {
      texture = device_->newTexture(desc);
    }
  }
  desc->release();
  // Initial clear is handled on first bind via load actions; avoid
  // synchronous clears here to keep the host RT path fast.
  return texture;
}

MTL::Texture* MetalRenderTargetCache::CreateDepthTexture(
    uint32_t width, uint32_t height, xenos::DepthRenderTargetFormat format,
    uint32_t samples) {
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);  // Default height if not specified
  MTL::PixelFormat pixel_format = GetDepthPixelFormat(format);
  desc->setPixelFormat(pixel_format);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  MTL::TextureUsage usage =
      MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
  if (pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
      pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);
  desc->setStorageMode(MTL::StorageModePrivate);

  MTL::Texture* texture = nullptr;
  if (render_target_heap_pool_) {
    texture = render_target_heap_pool_->CreateTexture(desc);
  }
  if (!texture) {
    texture = device_->newTexture(desc);
  }
  desc->release();
  // Initial clear is handled on first bind via load actions; avoid
  // synchronous clears here to keep the host RT path fast.
  return texture;
}

MTL::Texture* MetalRenderTargetCache::CreateTransientDepthTexture(
    uint32_t width, uint32_t height, uint32_t samples) {
  if (!device_) {
    return nullptr;
  }

  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(std::max(1u, width));
  desc->setHeight(std::max(1u, height));
  desc->setPixelFormat(MTL::PixelFormatDepth32Float);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(std::max(1u, samples));
  desc->setUsage(MTL::TextureUsageRenderTarget);

  MTL::Texture* texture = nullptr;
  bool can_use_memoryless = device_->supportsFamily(MTL::GPUFamilyApple1);
  if (can_use_memoryless) {
    desc->setStorageMode(MTL::StorageModeMemoryless);
    texture = device_->newTexture(desc);
  }
  if (!texture) {
    desc->setStorageMode(MTL::StorageModePrivate);
    if (render_target_heap_pool_ && !can_use_memoryless) {
      texture = render_target_heap_pool_->CreateTexture(desc);
    }
    if (!texture) {
      texture = device_->newTexture(desc);
    }
  }

  desc->release();
  return texture;
}

MTL::PixelFormat MetalRenderTargetCache::GetColorResourcePixelFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return MTL::PixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      // Updated to suport unorm16 for gamma render targets.
      return gamma_render_target_as_unorm16_ ? MTL::PixelFormatRGBA16Unorm
                                             : MTL::PixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return MTL::PixelFormatRGB10A2Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      // Match D3D12 behavior: store as RGBA16F and pack to float10 on dump.
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return MTL::PixelFormatRG16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return MTL::PixelFormatRGBA16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    default:
      REXLOG_ERROR("MetalRenderTargetCache: Unsupported color format {}",
             static_cast<uint32_t>(format));
      return MTL::PixelFormatRGBA8Unorm;
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetColorDrawPixelFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return MTL::PixelFormatRG16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return MTL::PixelFormatRGBA16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    default:
      return GetColorResourcePixelFormat(format);
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetColorOwnershipTransferPixelFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Uint;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Uint;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Uint;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Uint;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      // Ownership transfers must use a linear resource view to avoid
      // implicit sRGB conversion for gamma render targets.
      return GetColorResourcePixelFormat(format);
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetDepthPixelFormat(
    xenos::DepthRenderTargetFormat format) const {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
    case xenos::DepthRenderTargetFormat::kD24FS8:
      // Metal doesn't have D24S8, use D32Float_S8
      return MTL::PixelFormatDepth32Float_Stencil8;
    default:
      REXLOG_ERROR("MetalRenderTargetCache: Unsupported depth format {}",
             static_cast<uint32_t>(format));
      return MTL::PixelFormatDepth32Float_Stencil8;
  }
}

MTL::Texture* MetalRenderTargetCache::GetStencilTextureView(
    MetalRenderTarget* render_target) {
  if (!render_target) {
    return nullptr;
  }
  if (render_target->stencil_view()) {
    return render_target->stencil_view();
  }
  RenderTargetKey key = render_target->key();
  if (!key.is_depth) {
    return nullptr;
  }
  MTL::Texture* depth_texture = render_target->texture();
  if (!depth_texture) {
    return nullptr;
  }
  MTL::Texture* view =
      depth_texture->newTextureView(MTL::PixelFormatX32_Stencil8);
  if (view) {
    render_target->SetStencilView(view);
  }
  return view;
}

MTL::RenderPassDescriptor* MetalRenderTargetCache::GetRenderPassDescriptor(
    uint32_t expected_sample_count, bool fallback_depth_attachment_required) {
  if (!render_pass_descriptor_dirty_ && cached_render_pass_descriptor_ &&
      cached_render_pass_descriptor_sample_count_ == expected_sample_count &&
      cached_render_pass_descriptor_fallback_depth_required_ ==
          fallback_depth_attachment_required) {
    return cached_render_pass_descriptor_;
  }
  if (cached_render_pass_descriptor_sample_count_ != expected_sample_count ||
      cached_render_pass_descriptor_fallback_depth_required_ !=
          fallback_depth_attachment_required) {
    MarkRenderPassDescriptorDirty();
  }

  // Release old descriptor
  if (cached_render_pass_descriptor_) {
    cached_render_pass_descriptor_->release();
    cached_render_pass_descriptor_ = nullptr;
  }

  // Create new descriptor
  cached_render_pass_descriptor_ =
      MTL::RenderPassDescriptor::renderPassDescriptor();
  if (!cached_render_pass_descriptor_) {
    REXLOG_ERROR("MetalRenderTargetCache: Failed to create render pass descriptor");
    return nullptr;
  }
  cached_render_pass_descriptor_->retain();
  cached_render_pass_descriptor_sample_count_ = expected_sample_count;
  cached_render_pass_descriptor_fallback_depth_required_ =
      fallback_depth_attachment_required;
  render_pass_descriptor_dirty_ = false;

  AttachmentPlan attachment_plan;
  if (!BuildCurrentAttachmentPlan(expected_sample_count, attachment_plan)) {
    return nullptr;
  }
  bool has_any_color_target = attachment_plan.has_any_color_target;
  uint32_t coverage_width = attachment_plan.coverage_width;
  uint32_t coverage_height = attachment_plan.coverage_height;
  uint32_t coverage_samples = attachment_plan.coverage_samples;

  // Bind the actual render targets retrieved from base class in Update()

  // Bind depth target if present
  const AttachmentPlanAttachment& depth_plan = attachment_plan.depth;
  if (depth_plan.bound && depth_plan.render_target && depth_plan.texture) {
    auto* depth_attachment = cached_render_pass_descriptor_->depthAttachment();
    depth_attachment->setTexture(depth_plan.texture);

    // Clear on first bind to avoid synchronous clears at creation.
    bool depth_needs_clear = depth_plan.needs_initial_clear;
    bool depth_load_dontcare = depth_plan.load_action_safe;
    AttachmentLoadStoreActions depth_load_store =
        GetRealAttachmentLoadStoreActions(depth_needs_clear,
                                          !depth_load_dontcare);
    AttachmentLoadStoreActions depth_only_load_store = depth_load_store;
    if (RtStoreDiag() & 2) {
      depth_only_load_store.store = MTL::StoreActionDontCare;
    }
    SetAttachmentLoadStoreActions(depth_attachment, depth_only_load_store);
    if (depth_needs_clear) {
      depth_attachment->setClearDepth(GetDepthTargetClearDepth());
      depth_plan.render_target->SetNeedsInitialClear(false);
      depth_plan.render_target->set_initial_clear_done(true);
      MarkRenderPassDescriptorDirty();
    }

    // If the depth texture includes stencil, bind the same texture to the
    // stencil attachment too (Metal requires explicit stencil attachment
    // binding to match pipeline state).
    MTL::PixelFormat depth_pixel_format = depth_plan.texture->pixelFormat();
    if (depth_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatX32_Stencil8) {
      auto* stencil_attachment =
          cached_render_pass_descriptor_->stencilAttachment();
      stencil_attachment->setTexture(depth_plan.texture);
      AttachmentLoadStoreActions stencil_load_store = depth_load_store;
      if (!depth_needs_clear && depth_load_dontcare) {
        stencil_load_store = {MTL::LoadActionClear, MTL::StoreActionStore};
      }
      if (RtStoreDiag() & 1) {
        stencil_load_store.store = MTL::StoreActionDontCare;
      }
      SetAttachmentLoadStoreActions(stencil_attachment, stencil_load_store);
      if (depth_needs_clear || (!depth_needs_clear && depth_load_dontcare)) {
        stencil_attachment->setClearStencil(0);
      }
    }

    // Track this as a real render target for capture
    last_real_depth_target_ = depth_plan.render_target;
  }

  // Bind color targets
  for (uint32_t i = 0; i < 4; ++i) {
    const AttachmentPlanAttachment& color_plan = attachment_plan.colors[i];
    if (color_plan.bound && color_plan.render_target && color_plan.texture) {
      auto* color_attachment =
          cached_render_pass_descriptor_->colorAttachments()->object(i);
      color_attachment->setTexture(color_plan.texture);

      // Clear on first bind to avoid synchronous clears at creation.
      bool color_needs_clear = color_plan.needs_initial_clear;
      bool color_load_dontcare = color_plan.load_action_safe;
      AttachmentLoadStoreActions color_load_store =
          GetRealAttachmentLoadStoreActions(color_needs_clear,
                                            !color_load_dontcare);
      SetAttachmentLoadStoreActions(color_attachment, color_load_store);
      if (color_needs_clear) {
        color_attachment->setClearColor(
            MTL::ClearColor::Make(0.0, 0.0, 0.0, 0.0));
        color_plan.render_target->SetNeedsInitialClear(false);
        color_plan.render_target->set_initial_clear_done(true);
        MarkRenderPassDescriptorDirty();
      }

      has_any_color_target = true;

      // Track this as a real render target for capture
      last_real_color_targets_[i] = color_plan.render_target;
    }
  }

  // If no color render targets are bound, attach a dummy color target so Metal
  // has at least one color attachment. This mirrors the D3D12/Vulkan behavior
  // where an RTV is always bound when drawing, and also keeps pipeline state
  // validation happy for depth-only passes.
  if (!has_any_color_target) {
    xenos::ColorRenderTargetFormat fmt =
        xenos::ColorRenderTargetFormat::k_8_8_8_8;
    uint32_t samples = std::max(1u, expected_sample_count);

    uint32_t width = 1280;
    uint32_t height = 720;
    if (current_depth_target_ && current_depth_target_->texture()) {
      width = static_cast<uint32_t>(current_depth_target_->texture()->width());
      height =
          static_cast<uint32_t>(current_depth_target_->texture()->height());
      if (current_depth_target_->texture()->sampleCount() > 0) {
        samples = std::max<uint32_t>(
            samples, static_cast<uint32_t>(
                         current_depth_target_->texture()->sampleCount()));
      }
    } else if (last_real_color_targets_[0] &&
               last_real_color_targets_[0]->texture()) {
      width = static_cast<uint32_t>(
          last_real_color_targets_[0]->texture()->width());
      height = static_cast<uint32_t>(
          last_real_color_targets_[0]->texture()->height());
    } else if (last_real_depth_target_ && last_real_depth_target_->texture()) {
      width =
          static_cast<uint32_t>(last_real_depth_target_->texture()->width());
      height =
          static_cast<uint32_t>(last_real_depth_target_->texture()->height());
      if (last_real_depth_target_->texture()->sampleCount() > 0) {
        samples = std::max<uint32_t>(
            samples, static_cast<uint32_t>(
                         last_real_depth_target_->texture()->sampleCount()));
      }
    }

    uint32_t dummy_sample_count =
        samples >= 4u ? 4u : (samples == 2u ? 2u : 1u);
    // No color render targets are bound: attach a single transient dummy color
    // target so the render pass/pipeline has a matching color output. Cache it
    // by shape/format and recreate it only when the required shape changes.
    uint64_t dummy_key = uint64_t(width & 0xFFFFu) |
                         (uint64_t(height & 0xFFFFu) << 16) |
                         (uint64_t(dummy_sample_count & 0xFFu) << 32) |
                         (uint64_t(uint32_t(fmt) & 0xFFFFu) << 40);
    if (!dummy_color_target_owner_ || !dummy_color_target_owner_->texture() ||
        dummy_color_target_shape_key_ != dummy_key) {
      // Release the previous dummy first so its heap space is freed before the
      // new allocation.
      dummy_color_target_owner_.reset();
      dummy_color_target_ = nullptr;
      RenderTargetKey dummy_rt_key;
      dummy_rt_key.key = 0;
      dummy_rt_key.is_depth = 0;
      dummy_rt_key.resource_format = uint32_t(fmt);
      dummy_rt_key.msaa_samples =
          dummy_sample_count >= 4u   ? xenos::MsaaSamples::k4X
          : dummy_sample_count == 2u ? xenos::MsaaSamples::k2X
                                     : xenos::MsaaSamples::k1X;
      auto dummy = std::make_unique<MetalRenderTarget>(dummy_rt_key);
      MTL::Texture* tex =
          CreateColorTexture(width, height, fmt, dummy_sample_count,
                             /*transient_render_target_only=*/true,
                             /*allow_unpooled_fallback=*/false);
      if (!tex) {
        static bool logged_unpooled_fallback = false;
        if (render_target_heap_pool_ && !logged_unpooled_fallback) {
          REXLOG_WARN(
              "Metal RT dummy target: heap allocation failed for {}x{} {}x; "
              "falling back to unpooled texture",
              width, height, dummy_sample_count);
          logged_unpooled_fallback = true;
        }
        tex = CreateColorTexture(width, height, fmt, dummy_sample_count,
                                 /*transient_render_target_only=*/true,
                                 /*allow_unpooled_fallback=*/true);
      }
      dummy->SetTexture(tex);
      if (tex) {
        MTL::PixelFormat resource_format = GetColorResourcePixelFormat(fmt);
        MTL::PixelFormat draw_format = GetColorDrawPixelFormat(fmt);
        MTL::PixelFormat transfer_format =
            GetColorOwnershipTransferPixelFormat(fmt, nullptr);
        if (draw_format != resource_format) {
          MTL::Texture* draw_view = tex->newTextureView(draw_format);
          if (!draw_view) {
            REXLOG_ERROR("Failed to create texture view for render target");
          }
          dummy->SetDrawTexture(draw_view);
        }
        if (transfer_format != resource_format) {
          MTL::Texture* transfer_view = tex->newTextureView(transfer_format);
          if (!transfer_view) {
            REXLOG_ERROR("Failed to create texture view for render target");
          }
          dummy->SetTransferTexture(transfer_view);
        }
      }
      dummy_color_target_owner_ = std::move(dummy);
      dummy_color_target_shape_key_ = dummy_key;
    }
    dummy_color_target_ = dummy_color_target_owner_.get();

    auto* color_attachment =
        cached_render_pass_descriptor_->colorAttachments()->object(0);
    color_attachment->setTexture(dummy_color_target_->draw_texture());
    SetAttachmentLoadStoreActions(color_attachment,
                                  GetTransientAttachmentLoadStoreActions());

    if (!coverage_width && dummy_color_target_->draw_texture()) {
      coverage_width =
          static_cast<uint32_t>(dummy_color_target_->draw_texture()->width());
      coverage_height =
          static_cast<uint32_t>(dummy_color_target_->draw_texture()->height());
      if (dummy_color_target_->draw_texture()->sampleCount() > 0) {
        coverage_samples = std::max<uint32_t>(
            coverage_samples,
            static_cast<uint32_t>(
                dummy_color_target_->draw_texture()->sampleCount()));
      }
    }
  }

  if (fallback_depth_attachment_required && !current_depth_target_) {
    uint32_t width = coverage_width ? coverage_width : 1280;
    uint32_t height = coverage_height ? coverage_height : 720;
    uint32_t samples = std::max(1u, coverage_samples);
    MTL::Texture* fallback_depth_texture =
        CreateTransientDepthTexture(width, height, samples);
    if (!fallback_depth_texture) {
      REXLOG_ERROR(
          "MetalRenderTargetCache: Failed to create transient depth "
          "attachment");
      cached_render_pass_descriptor_->release();
      cached_render_pass_descriptor_ = nullptr;
      MarkRenderPassDescriptorDirty();
      return nullptr;
    }

    auto* depth_attachment = cached_render_pass_descriptor_->depthAttachment();
    depth_attachment->setTexture(fallback_depth_texture);
    SetAttachmentLoadStoreActions(depth_attachment,
                                  GetTransientAttachmentLoadStoreActions());
    fallback_depth_texture->release();
  }

  return cached_render_pass_descriptor_;
}

bool MetalRenderTargetCache::IsRenderPassDescriptorCompatible(
    MTL::RenderPassDescriptor* pass_descriptor, uint32_t expected_sample_count,
    bool fallback_depth_attachment_required) const {
  if (pass_descriptor && pass_descriptor == cached_render_pass_descriptor_ &&
      !render_pass_descriptor_dirty_ &&
      cached_render_pass_descriptor_sample_count_ == expected_sample_count &&
      cached_render_pass_descriptor_fallback_depth_required_ ==
          fallback_depth_attachment_required) {
    return true;
  }
  return IsRenderPassDescriptorCompatibleSlow(
      pass_descriptor, expected_sample_count,
      fallback_depth_attachment_required);
}

bool MetalRenderTargetCache::IsRenderPassDescriptorCompatibleSlow(
    MTL::RenderPassDescriptor* pass_descriptor, uint32_t expected_sample_count,
    bool fallback_depth_attachment_required) const {
  (void)expected_sample_count;
  if (!pass_descriptor) {
    return false;
  }

  MTL::Texture* expected_depth =
      current_depth_target_ ? current_depth_target_->draw_texture() : nullptr;
  auto* depth_attachment = pass_descriptor->depthAttachment();
  auto* stencil_attachment = pass_descriptor->stencilAttachment();
  if (expected_depth) {
    if (!depth_attachment || depth_attachment->texture() != expected_depth) {
      return false;
    }
    MTL::PixelFormat depth_pixel_format = expected_depth->pixelFormat();
    bool expects_stencil =
        depth_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatX32_Stencil8;
    if (expects_stencil) {
      if (!stencil_attachment ||
          stencil_attachment->texture() != expected_depth) {
        return false;
      }
    } else if (stencil_attachment && stencil_attachment->texture()) {
      return false;
    }
  } else {
    MTL::Texture* depth_texture =
        depth_attachment ? depth_attachment->texture() : nullptr;
    MTL::Texture* stencil_texture =
        stencil_attachment ? stencil_attachment->texture() : nullptr;
    if (fallback_depth_attachment_required) {
      if (!depth_texture ||
          depth_texture->pixelFormat() != MTL::PixelFormatDepth32Float ||
          stencil_texture) {
        return false;
      }
    } else if (depth_texture || stencil_texture) {
      return false;
    }
  }

  auto* color_attachments = pass_descriptor->colorAttachments();
  bool has_current_color_target = false;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (current_color_targets_[i] &&
        current_color_targets_[i]->draw_texture()) {
      has_current_color_target = true;
      break;
    }
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* color_attachment = color_attachments->object(i);
    MTL::Texture* expected_color =
        current_color_targets_[i] ? current_color_targets_[i]->draw_texture()
                                  : nullptr;
    if (expected_color) {
      if (!color_attachment || color_attachment->texture() != expected_color) {
        return false;
      }
      continue;
    }
    if (color_attachment && color_attachment->texture()) {
      if (!has_current_color_target && i == 0) {
        continue;
      }
      return false;
    }
  }

  if (has_current_color_target) {
    return true;
  }

  MTL::Texture* expected_dummy =
      dummy_color_target_ ? dummy_color_target_->draw_texture() : nullptr;
  auto* color_attachment_0 = color_attachments->object(0);
  if (expected_dummy && color_attachment_0 &&
      color_attachment_0->texture() == expected_dummy) {
    return true;
  }
  return false;
}

MTL::Texture* MetalRenderTargetCache::GetColorTarget(uint32_t index) const {
  if (index >= 4 || !current_color_targets_[index]) {
    return nullptr;
  }
  return current_color_targets_[index]->texture();
}

MTL::Texture* MetalRenderTargetCache::GetDepthTarget() const {
  if (!current_depth_target_) {
    return nullptr;
  }
  return current_depth_target_->texture();
}

MTL::Texture* MetalRenderTargetCache::GetDummyColorTarget() const {
  if (dummy_color_target_ && dummy_color_target_->texture()) {
    return dummy_color_target_->texture();
  }
  return nullptr;
}

MTL::Texture* MetalRenderTargetCache::GetColorTargetForDraw(
    uint32_t index) const {
  if (index >= 4 || !current_color_targets_[index]) {
    return nullptr;
  }
  return current_color_targets_[index]->draw_texture();
}

MTL::Texture* MetalRenderTargetCache::GetDepthTargetForDraw() const {
  if (!current_depth_target_) {
    return nullptr;
  }
  return current_depth_target_->draw_texture();
}

MTL::Texture* MetalRenderTargetCache::GetDummyColorTargetForDraw() const {
  if (dummy_color_target_ && dummy_color_target_->draw_texture()) {
    return dummy_color_target_->draw_texture();
  }
  return nullptr;
}

bool MetalRenderTargetCache::IsDepthTargetFloat24() const {
  return current_depth_target_ && current_depth_target_->key().GetDepthFormat() ==
                                      xenos::DepthRenderTargetFormat::kD24FS8;
}

double MetalRenderTargetCache::GetDepthTargetClearDepth() const {
  if (!current_depth_target_) {
    return 1.0;
  }
  return current_depth_target_->key().GetDepthFormat() ==
                 xenos::DepthRenderTargetFormat::kD24FS8
             ? 0.0
             : 1.0;
}

void MetalRenderTargetCache::DumpRenderTargets(
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, MTL::CommandBuffer* command_buffer,
    const char* encoder_label) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  XELOGGPU(
      "MetalRenderTargetCache::DumpRenderTargets: base={} row_length_used={} "
      "rows={} pitch={}",
      dump_base, dump_row_length_used, dump_rows, dump_pitch);

  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, rectangles);

  XELOGGPU("MetalRenderTargetCache::DumpRenderTargets: {} rectangles to dump",
           rectangles.size());
  if (rectangles.empty()) {
    REXLOG_WARN(
        "MetalRenderTargetCache::DumpRenderTargets: no rectangles for base={} "
        "row_length_used={} rows={} pitch={}",
        dump_base, dump_row_length_used, dump_rows, dump_pitch);
    return;
  }

  if (!edram_buffer_) {
    REXLOG_WARN(
        "MetalRenderTargetCache::DumpRenderTargets: EDRAM buffer not "
        "initialized, skipping GPU dump");
    return;
  }

  struct EdramDumpConstants {
    uint32_t dispatch_first_tile;
    uint32_t source_base_tiles;
    uint32_t dest_pitch_tiles;
    uint32_t source_pitch_tiles;
    uint32_t resolution_scale_x;
    uint32_t resolution_scale_y;
    uint32_t tile_size_x;
    uint32_t tile_size_y;
    float tile_size_inv_x;
    float tile_size_inv_y;
    float source_pitch_tiles_inv;
    uint32_t format;
    uint32_t flags;
    uint32_t padding;
  };

  ScopedAutoreleasePool autorelease_pool;
  bool standalone = false;
  MTL::CommandBuffer* cmd = command_buffer;
  if (!cmd) {
    cmd = command_processor_.CreateStandaloneTransferCommandBuffer(
        "XeniaCB reason=rt-dump");
    if (!cmd) {
      REXLOG_ERROR("MetalRenderTargetCache::DumpRenderTargets: no command buffer");
      return;
    }
    standalone = true;
  }

  EndSharedMemoryUploadBlitEncoderForCommandBuffer(command_processor_, cmd);
  MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
  if (!encoder) {
    REXLOG_ERROR("MetalRenderTargetCache::DumpRenderTargets: no compute encoder");
    if (standalone) {
      cmd->release();
    }
    return;
  }
  SetEncoderLabel(encoder,
                  encoder_label ? encoder_label : "XeniaEDRAMDumpEncoder");
  PushEncoderDebugGroup(
      encoder,
      fmt::format("{} base={} rows={} pitch={}",
                  encoder_label ? encoder_label : "XeniaEDRAMDumpEncoder",
                  dump_base, dump_rows, dump_pitch));

  encoder->setBuffer(edram_buffer_, 0, 0);
  encoder->useResource(edram_buffer_, MTL::ResourceUsageWrite);

  uint32_t scale_x = resolve_scale_x_;
  uint32_t scale_y = resolve_scale_y_;

  for (const ResolveCopyDumpRectangle& rect : rectangles) {
    auto* rt = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!rt) {
      continue;
    }

    RenderTargetKey key = rt->key();
    MTL::Texture* tex = nullptr;
    bool dump_source_is_uint = false;
    if (key.is_depth) {
      tex = rt->texture();
      if (!tex) {
        continue;
      }
      MTL::PixelFormat expected_format =
          GetDepthPixelFormat(key.GetDepthFormat());
      assert_true(tex->pixelFormat() == expected_format,
                  "Dump depth must bind resource pixel format");
    } else {
      bool ownership_transfer_is_uint = false;
      MTL::PixelFormat ownership_transfer_format =
          GetColorOwnershipTransferPixelFormat(key.GetColorFormat(),
                                               &ownership_transfer_is_uint);
      dump_source_is_uint = ownership_transfer_is_uint;
      if (dump_source_is_uint) {
        tex =
            (key.msaa_samples != xenos::MsaaSamples::k1X && rt->msaa_texture())
                ? rt->msaa_transfer_texture()
                : rt->transfer_texture();
      } else {
        tex = rt->texture();
      }
      if (!tex) {
        continue;
      }
      MTL::PixelFormat expected_format =
          dump_source_is_uint
              ? ownership_transfer_format
              : GetColorResourcePixelFormat(key.GetColorFormat());
      assert_true(tex->pixelFormat() == expected_format,
                  dump_source_is_uint
                      ? "Dump color uint must bind ownership-transfer format"
                      : "Dump color must bind resource pixel format");
    }

    uint32_t dump_format = GetMetalEdramDumpFormat(key);
    uint32_t dump_flags = 0;
    MTL::Texture* stencil_tex = nullptr;
    if (key.is_depth) {
      if (!REXCVAR_GET(depth_float24_convert_in_pixel_shader) &&
          REXCVAR_GET(depth_float24_round)) {
        dump_flags |= kMetalEdramDumpFlagDepthRound;
      }
      stencil_tex = GetStencilTextureView(rt);
      if (stencil_tex) {
        dump_flags |= kMetalEdramDumpFlagHasStencil;
      }
    }

    // Choose the appropriate dump pipeline based on:
    // - 32bpp vs 64bpp (key.Is64bpp())
    // - color vs depth (key.is_depth)
    // - MSAA sample count (key.msaa_samples)
    // This mirrors D3D12's dump pipeline selection: use key.Is64bpp() directly,
    // NOT IsKey64bpp() which includes gamma-as-unorm16. The EDRAM buffer is
    // always 32bpp for gamma formats; only the host texture storage is 64bpp.
    MTL::ComputePipelineState* dump_pipeline = nullptr;
    bool is_64bpp = key.Is64bpp();

    // If this is a gamma RT stored as linear RGBA16Unorm, we need to encode
    // to PWL gamma when dumping. Set the flag for the dump shader.
    if (!key.is_depth &&
        key.GetColorFormat() ==
            xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
        gamma_render_target_as_unorm16_) {
      dump_flags |= kMetalEdramDumpFlagGammaAsLinear;
    }

    size_t msaa_index = MsaaSamplesToIndex(key.msaa_samples);
    if (msaa_index != SIZE_MAX) {
      if (!key.is_depth) {
        dump_pipeline =
            edram_dump_color_pipelines_[is_64bpp ? 1u : 0u]
                                       [dump_source_is_uint ? 1u : 0u]
                                       [msaa_index];
      } else {
        dump_pipeline = edram_dump_depth_pipelines_[msaa_index];
      }
    }

    if (!dump_pipeline) {
      XELOGGPU(
          "MetalRenderTargetCache::DumpRenderTargets: no dump pipeline for "
          "key=0x{:08X} (is_depth={}, is_64bpp={}, msaa={})",
          key.key, key.is_depth ? 1 : 0, is_64bpp ? 1 : 0,
          static_cast<uint32_t>(key.msaa_samples));
      continue;
    }

    XELOGGPU(
        "MetalRenderTargetCache::DumpRenderTargets: dump RT key=0x{:08X} "
        "(is_depth={}, is_64bpp={}, source_uint={}, msaa={}) tex={}x{} "
        "pipeline={:p}",
        key.key, key.is_depth ? 1 : 0, is_64bpp ? 1 : 0,
        dump_source_is_uint ? 1 : 0, static_cast<uint32_t>(key.msaa_samples),
        tex->width(), tex->height(), static_cast<void*>(dump_pipeline));

    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rect.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    if (!dispatch_count) {
      continue;
    }

    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];

      EdramDumpConstants constants;
      constants.dispatch_first_tile = dump_base + dispatch.offset;
      constants.source_base_tiles = key.base_tiles;
      constants.dest_pitch_tiles = dump_pitch;
      constants.source_pitch_tiles = key.GetPitchTiles();
      constants.resolution_scale_x = scale_x;
      constants.resolution_scale_y = scale_y;
      uint32_t tile_size_x = (is_64bpp ? 40u : 80u) * scale_x;
      uint32_t tile_size_y = 16u * scale_y;
      constants.tile_size_x = tile_size_x;
      constants.tile_size_y = tile_size_y;
      constants.tile_size_inv_x =
          tile_size_x ? (1.0f / float(tile_size_x)) : 0.0f;
      constants.tile_size_inv_y =
          tile_size_y ? (1.0f / float(tile_size_y)) : 0.0f;
      constants.source_pitch_tiles_inv =
          constants.source_pitch_tiles
              ? (1.0f / float(constants.source_pitch_tiles))
              : 0.0f;
      constants.format = dump_format;
      constants.flags = dump_flags;
      constants.padding = 0;

      encoder->setComputePipelineState(dump_pipeline);
      encoder->setTexture(tex, 0);
      if (stencil_tex) {
        encoder->setTexture(stencil_tex, 1);
      }
      encoder->useResource(tex, MTL::ResourceUsageRead);
      if (stencil_tex) {
        encoder->useResource(stencil_tex, MTL::ResourceUsageRead);
      }
      encoder->setBytes(&constants, sizeof(constants), 1);

      // Thread group dispatch:
      // - 40x16 threads per group (same as D3D12/Vulkan)
      // - For 32bpp: two groups per tile along X (80 samples / 40 threads)
      // - For 64bpp: one group per tile along X (40 samples / 40 threads)
      uint32_t groups_x = dispatch.width_tiles * scale_x;
      if (!is_64bpp) {
        groups_x <<= 1;  // Double for 32bpp
      }
      uint32_t groups_y = dispatch.height_tiles * scale_y;

      MTL::Size threads_per_group = MTL::Size::Make(40, 16, 1);
      MTL::Size threadgroups = MTL::Size::Make(groups_x, groups_y, 1);
      encoder->dispatchThreadgroups(threadgroups, threads_per_group);
    }
  }

  if (EncoderDebugLabelsEnabled()) {
    encoder->popDebugGroup();
  }
  encoder->endEncoding();
  if (standalone) {
    command_processor_.CommitStandaloneAndWait(cmd);
  }
}

MTL::Library* MetalRenderTargetCache::GetOrCreateEdramLoadLibrary(bool msaa) {
  MTL::Library*& library =
      msaa ? edram_load_library_msaa_ : edram_load_library_;
  if (library) {
    return library;
  }

  static const char kEdramLoadShaderSource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramLoadConstants {
  uint base_tiles;
  uint pitch_tiles;
  uint format;
  uint format_is_64bpp;
  uint msaa_samples;
  uint sample_id;
  uint resolution_scale_x;
  uint resolution_scale_y;
};

struct VSOut {
  float4 position [[position]];
};

vertex VSOut edram_load_vs(uint vid [[vertex_id]]) {
  float2 pt = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(pt * 2.0f - 1.0f, 0.0f, 1.0f);
  return out;
}

constant uint kXenosMsaaSamples1X = 0u;
constant uint kXenosMsaaSamples2X = 1u;
constant uint kXenosMsaaSamples4X = 2u;
constant uint kEdramTileCount = 2048u;

uint XeEdramOffsetInts(uint2 pixel_index, uint base_tiles, bool wrap,
                       uint pitch_tiles, uint msaa_samples, bool is_depth,
                       uint format_ints_log2, uint pixel_sample_index,
                       uint2 resolution_scale) {
  uint msaa_samples_x_log2 = (msaa_samples >= kXenosMsaaSamples4X) ? 1u : 0u;
  uint msaa_samples_y_log2 = (msaa_samples >= kXenosMsaaSamples2X) ? 1u : 0u;
  uint2 rt_sample_index =
      pixel_index << uint2(msaa_samples_x_log2, msaa_samples_y_log2);
  rt_sample_index +=
      (uint2(pixel_sample_index) >> uint2(1u, 0u)) & 1u;
  uint2 tile_size_at_32bpp = uint2(80u, 16u) * resolution_scale;
  uint2 tile_size_samples =
      tile_size_at_32bpp >> uint2(format_ints_log2, 0u);
  uint2 tile_offset_xy = rt_sample_index / tile_size_samples;
  base_tiles += tile_offset_xy.y * pitch_tiles + tile_offset_xy.x;
  rt_sample_index -= tile_offset_xy * tile_size_samples;
  if (is_depth) {
    uint tile_width_half = tile_size_samples.x >> 1u;
    rt_sample_index.x =
        uint(int(rt_sample_index.x) +
             ((rt_sample_index.x >= tile_width_half)
                  ? -int(tile_width_half)
                  : int(tile_width_half)));
  }
  uint address =
      base_tiles * (tile_size_at_32bpp.x * tile_size_at_32bpp.y) +
      ((rt_sample_index.y * tile_size_samples.x + rt_sample_index.x) <<
       format_ints_log2);
  if (wrap) {
    address %= tile_size_at_32bpp.x * tile_size_at_32bpp.y * kEdramTileCount;
  }
  return address;
}

float XeFloat7e3To32(uint f10) {
  f10 &= 0x3FFu;
  if (f10 == 0u) {
    return 0.0f;
  }
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint mantissa_lzcnt = clz(mantissa) - 24u;
    exponent = uint(int(1) - int(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7Fu;
  }
  uint f32 = ((exponent + 124u) << 23u) | (mantissa << 16u);
  return as_type<float>(f32);
}

float4 XeUnpackR8G8B8A8UNorm(uint packed) {
  float4 value = float4(packed & 0xFFu, (packed >> 8u) & 0xFFu,
                        (packed >> 16u) & 0xFFu, packed >> 24u);
  return value * (1.0f / 255.0f);
}

float4 XeUnpackR10G10B10A2UNorm(uint packed) {
  float4 value = float4(packed & 0x3FFu, (packed >> 10u) & 0x3FFu,
                        (packed >> 20u) & 0x3FFu, (packed >> 30u) & 0x3u);
  return value * float4(1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 1023.0f,
                        1.0f / 3.0f);
}

float4 XeUnpackR10G10B10A2Float(uint packed) {
  float r = XeFloat7e3To32(packed & 0x3FFu);
  float g = XeFloat7e3To32((packed >> 10u) & 0x3FFu);
  float b = XeFloat7e3To32((packed >> 20u) & 0x3FFu);
  float a = float((packed >> 30u) & 0x3u) * (1.0f / 3.0f);
  return float4(r, g, b, a);
}

float2 XeUnpackR16G16Edram(uint packed) {
  int r = int(packed << 16u) >> 16u;
  int g = int(packed) >> 16u;
  float2 value = float2(float(r), float(g)) * (32.0f / 32767.0f);
  return max(value, float2(-1.0f));
}

float4 XeUnpackR16G16B16A16Edram(uint2 packed) {
  int r = int(packed.x << 16u) >> 16u;
  int g = int(packed.x) >> 16u;
  int b = int(packed.y << 16u) >> 16u;
  int a = int(packed.y) >> 16u;
  float4 value = float4(float(r), float(g), float(b), float(a)) *
                 (32.0f / 32767.0f);
  return max(value, float4(-1.0f));
}

float2 XeUnpackHalf2(uint packed) {
  return float2(as_type<half2>(packed));
}

float4 XeUnpackColor32bpp(uint format, uint packed) {
  switch (format) {
    case 0u:  // kXenosColorRenderTargetFormat_8_8_8_8
    case 1u:  // kXenosColorRenderTargetFormat_8_8_8_8_GAMMA
      return XeUnpackR8G8B8A8UNorm(packed);
    case 2u:  // kXenosColorRenderTargetFormat_2_10_10_10
    case 10u: // kXenosColorRenderTargetFormat_2_10_10_10_AS_10_10_10_10
      return XeUnpackR10G10B10A2UNorm(packed);
    case 3u:  // kXenosColorRenderTargetFormat_2_10_10_10_FLOAT
    case 12u: // kXenosColorRenderTargetFormat_2_10_10_10_FLOAT_AS_16_16_16_16
      return XeUnpackR10G10B10A2Float(packed);
    case 4u: {  // kXenosColorRenderTargetFormat_16_16
      float2 rg = XeUnpackR16G16Edram(packed);
      return float4(rg, 0.0f, 1.0f);
    }
    case 6u: {  // kXenosColorRenderTargetFormat_16_16_FLOAT
      float2 rg = XeUnpackHalf2(packed);
      return float4(rg, 0.0f, 1.0f);
    }
    case 14u:  // kXenosColorRenderTargetFormat_32_FLOAT
      return float4(as_type<float>(packed), 0.0f, 0.0f, 1.0f);
    default:
      return float4(0.0f);
  }
}

float4 XeUnpackColor64bpp(uint format, uint2 packed) {
  switch (format) {
    case 5u:  // kXenosColorRenderTargetFormat_16_16_16_16
      return XeUnpackR16G16B16A16Edram(packed);
    case 7u: {  // kXenosColorRenderTargetFormat_16_16_16_16_FLOAT
      float2 rg = XeUnpackHalf2(packed.x);
      float2 ba = XeUnpackHalf2(packed.y);
      return float4(rg, ba);
    }
    case 15u:  // kXenosColorRenderTargetFormat_32_32_FLOAT
      return float4(as_type<float>(packed.x), as_type<float>(packed.y),
                    0.0f, 0.0f);
    default:
      return float4(0.0f);
  }
}

struct EdramLoadOut {
  float4 color [[color(0)]];
#if XE_EDRAM_LOAD_MSAA
  uint sample_mask [[sample_mask]];
#endif
};

fragment EdramLoadOut edram_load_ps(
    VSOut in [[stage_in]],
    constant EdramLoadConstants& constants [[buffer(0)]],
    device const uint* edram [[buffer(1)]]) {
  uint2 pixel = uint2(in.position.xy);
  uint format_ints_log2 = constants.format_is_64bpp;
  uint address = XeEdramOffsetInts(
      pixel, constants.base_tiles, true, constants.pitch_tiles,
      constants.msaa_samples, false, format_ints_log2, constants.sample_id,
      uint2(constants.resolution_scale_x, constants.resolution_scale_y));
  float4 color;
  if (constants.format_is_64bpp != 0u) {
    uint2 packed = uint2(edram[address], edram[address + 1u]);
    color = XeUnpackColor64bpp(constants.format, packed);
  } else {
    color = XeUnpackColor32bpp(constants.format, edram[address]);
  }
  EdramLoadOut out;
  out.color = color;
#if XE_EDRAM_LOAD_MSAA
  out.sample_mask = 1u << (constants.sample_id & 0x1Fu);
#endif
  return out;
}
)METAL";

  std::string source;
  source.reserve(sizeof(kEdramLoadShaderSource) + 32);
  source.append(msaa ? "#define XE_EDRAM_LOAD_MSAA 1\n"
                     : "#define XE_EDRAM_LOAD_MSAA 0\n");
  source.append(kEdramLoadShaderSource);
  source.insert(0, MslSalt());

  NS::Error* error = nullptr;
  auto source_str = NS::String::string(source.c_str(), NS::UTF8StringEncoding);
  library = device_->newLibrary(source_str, nullptr, &error);
  if (!library) {
    REXLOG_ERROR("Metal: failed to compile edram load shader: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
  }
  return library;
}

MTL::RenderPipelineState* MetalRenderTargetCache::GetOrCreateEdramLoadPipeline(
    MTL::PixelFormat dest_format, uint32_t sample_count) {
  uint64_t key = uint64_t(dest_format) | (uint64_t(sample_count) << 32);
  auto it = edram_load_pipelines_.find(key);
  if (it != edram_load_pipelines_.end()) {
    return it->second;
  }
  hitch::Scope hitch_scope(hitch::kPsoRt);

  bool msaa = sample_count > 1;
  MTL::Library* lib = GetOrCreateEdramLoadLibrary(msaa);
  if (!lib) {
    return nullptr;
  }

  NS::String* vs_name =
      NS::String::string("edram_load_vs", NS::UTF8StringEncoding);
  NS::String* ps_name =
      NS::String::string("edram_load_ps", NS::UTF8StringEncoding);
  MTL::Function* vs = lib->newFunction(vs_name);
  MTL::Function* ps = lib->newFunction(ps_name);
  if (!vs || !ps) {
    if (vs) {
      vs->release();
    }
    if (ps) {
      ps->release();
    }
    REXLOG_ERROR("Metal: edram load missing shader entrypoints");
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs);
  desc->setFragmentFunction(ps);
  desc->colorAttachments()->object(0)->setPixelFormat(dest_format);
  desc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
  desc->setSampleCount(sample_count);

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);
  desc->release();
  vs->release();
  ps->release();

  if (!pipeline) {
    REXLOG_ERROR("Metal: failed to create edram load pipeline: {}",
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  edram_load_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool MetalRenderTargetCache::PrepareResolvePlan(rex::memory::Memory& memory,
                                                ResolvePlan& plan_out) {
  plan_out = ResolvePlan();
  const RegisterFile& regs = register_file();

  // Fixed16 formats may be truncated to -1..1 when backed by SNORM.
  bool fixed_rg16_trunc = IsFixedRG16TruncatedToMinus1To1();
  bool fixed_rgba16_trunc = IsFixedRGBA16TruncatedToMinus1To1();

  if (!trace_writer_) {
    REXLOG_ERROR("MetalRenderTargetCache::Resolve: trace_writer_ is null");
    return false;
  }

  if (!draw_util::GetResolveInfo(regs, memory, *trace_writer_,
                                 draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), fixed_rg16_trunc,
                                 fixed_rgba16_trunc, plan_out.resolve_info)) {
    REXLOG_ERROR("MetalRenderTargetCache::Resolve: GetResolveInfo failed");
    return false;
  }
  plan_out.valid = true;

  const draw_util::ResolveInfo& resolve_info = plan_out.resolve_info;
  plan_out.noop =
      !resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8;
  if (plan_out.noop) {
    return true;
  }
  plan_out.needs_copy_export = resolve_info.copy_dest_extent_length != 0;
  plan_out.needs_resolve_clear =
      resolve_info.IsClearingDepth() || resolve_info.IsClearingColor();
  // TODO (xenios-jp): Add a queued resolve-clear system for clear-only resolves
  // that can be materialized before the next observer. Full clears could become
  // descriptor-time loadActionClear in the next render pass, avoiding an
  // immediate render encoder end on Apple TBDR GPUs. This needs a real
  // ownership transaction because PrepareHostRenderTargetsResolveClear mutates
  // tile ownership, and every later draw, transfer, export, readback, or
  // ownership query must observe the cleared contents.
  plan_out.needs_render_encoder_end =
      plan_out.needs_copy_export || plan_out.needs_resolve_clear;
  if (plan_out.needs_copy_export && IsDrawResolutionScaled() &&
      ShadowsNativeEnabled() && resolve_info.IsCopyingDepth()) {
    LearnShadowTarget(resolve_info);
  }
  if (plan_out.needs_copy_export && IsDrawResolutionScaled() &&
      (EffectsNativeEnabled() || ShadowsNativeEnabled() ||
       TexSpaceNativeEnabled())) {
    // [native-scale] resolves of native-scale targets are unscaled.
    bool unified = false;
    if (UnifyResolveSourceScale(resolve_info, unified)) {
      plan_out.native = true;
      if (!draw_util::GetResolveInfo(regs, memory, *trace_writer_, 1, 1,
                                     fixed_rg16_trunc, fixed_rgba16_trunc,
                                     plan_out.resolve_info)) {
        REXLOG_ERROR(
            "MetalRenderTargetCache::Resolve: GetResolveInfo (native) failed");
        return false;
      }
    }
    UploadTraceCounters& u = UploadTrace();
    if (plan_out.native) ++u.eff_rsv_native;
    if (unified) ++u.eff_rsv_unified;
  }
  if (ShadowTraceActive() &&
      (plan_out.needs_copy_export || plan_out.needs_resolve_clear)) {
    uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
    resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used,
                                      dump_rows, dump_pitch);
    std::vector<ResolveCopyDumpRectangle> rects;
    GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                   dump_pitch, rects);
    std::string owners;
    for (const ResolveCopyDumpRectangle& rect : rects) {
      owners += fmt::format(" [{}]", rect.render_target
                                         ? rect.render_target->key().GetDebugName()
                                         : std::string("none"));
    }
    REXLOG_INFO(
        "[shadow-trace] f{} resolve depth={} copy={} clear_d={} clear_c={} "
        "{}x{} dest=0x{:08X} extent=0x{:08X}+{} dest_fmt={} pitch={} h={} "
        "sample_sel={} owners:{}",
        frame_log::CurrentFrame(), resolve_info.IsCopyingDepth() ? 1 : 0,
        plan_out.needs_copy_export ? 1 : 0,
        resolve_info.IsClearingDepth() ? 1 : 0,
        resolve_info.IsClearingColor() ? 1 : 0,
        resolve_info.coordinate_info.width_div_8 * 8,
        resolve_info.height_div_8 * 8, resolve_info.copy_dest_base,
        resolve_info.copy_dest_extent_start,
        resolve_info.copy_dest_extent_length,
        uint32_t(resolve_info.copy_dest_info.copy_dest_format),
        resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32,
        resolve_info.copy_dest_coordinate_info.height_aligned_div_32 * 32,
        uint32_t(resolve_info.copy_dest_coordinate_info.copy_sample_select),
        owners);
  }
  if (plan_out.needs_copy_export) {
    plan_out.written_address = resolve_info.copy_dest_extent_start;
    plan_out.written_length = resolve_info.copy_dest_extent_length;
    UploadTraceCounters& u = UploadTrace();
    ++u.rsv_copy;
    if (IsDrawResolutionScaled()) ++u.rsv_scaled;
    if (EffectsNativeTraceEnabled()) {
      uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
      resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used,
                                        dump_rows, dump_pitch);
      std::vector<ResolveCopyDumpRectangle> rects;
      GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used,
                                     dump_rows, dump_pitch, rects);
      uint32_t native = 0, other = 0;
      for (const ResolveCopyDumpRectangle& rect : rects) {
        if (rect.render_target &&
            IsEffectsNativeEligible(rect.render_target->key())) {
          ++native;
        } else {
          ++other;
        }
      }
      if (native && !other) ++u.eff_rsv;
      if (native && other) ++u.eff_rsv_mixed;
    }
  }
  return true;
}

bool MetalRenderTargetCache::UnifyResolveSourceScale(
    const draw_util::ResolveInfo& resolve_info, bool& unified_out) {
  unified_out = false;
  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows,
                                    dump_pitch);
  std::vector<ResolveCopyDumpRectangle> rects;
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, rects);
  auto rect_tiles = [&](const ResolveCopyDumpRectangle& rect) -> uint64_t {
    if (rect.rows == 1) {
      return rect.row_last_end - rect.row_first_start;
    }
    return uint64_t(dump_row_length_used - rect.row_first_start) +
           uint64_t(rect.rows - 2) * dump_row_length_used + rect.row_last_end;
  };
  uint64_t native_tiles = 0, full_tiles = 0;
  bool full_not_convertible = false;
  for (const ResolveCopyDumpRectangle& rect : rects) {
    if (!rect.render_target) continue;
    RenderTargetKey key = rect.render_target->key();
    if (key.native_scale) {
      native_tiles += rect_tiles(rect);
    } else {
      full_tiles += rect_tiles(rect);
      // A native target takes colour 1xMSAA keys (effects) or depth keys of
      // a learned shadow-map target.
      const bool convertible =
          key.is_depth
              ? std::find(shadow_targets_.begin(), shadow_targets_.end(),
                          ShadowTargetId(key.base_tiles,
                                         key.pitch_tiles_at_32bpp)) !=
                    shadow_targets_.end()
              : key.msaa_samples == xenos::MsaaSamples::k1X;
      if (!convertible) {
        full_not_convertible = true;
      }
    }
  }
  if (!native_tiles) {
    return false;
  }
  if (!full_tiles) {
    return true;
  }
  // Mixed: move the minority's tiles to the majority's scale.
  const bool to_native = native_tiles >= full_tiles && !full_not_convertible;
  static uint32_t mixed_logs = 0;
  if (mixed_logs < 16) {
    ++mixed_logs;
    REXLOG_INFO(
        "[native-scale] mixed resolve 0x{:08X}: {} native / {} full-scale "
        "tiles -> {}",
        resolve_info.copy_dest_base, native_tiles, full_tiles,
        to_native ? "native" : "full scale");
  }
  for (const ResolveCopyDumpRectangle& rect : rects) {
    if (!rect.render_target) continue;
    RenderTargetKey key = rect.render_target->key();
    if (bool(key.native_scale) == to_native) continue;
    key.native_scale = to_native ? 1 : 0;
    RenderTarget* dest = GetOrCreateRenderTargetForTileConversion(key);
    if (!dest) {
      return false;
    }
    const uint32_t local_start =
        rect.row_first * dump_pitch + rect.row_first_start;
    const uint32_t local_end =
        (rect.row_first + rect.rows - 1) * dump_pitch + rect.row_last_end;
    if (local_end <= local_start) continue;
    const uint32_t global_start =
        (dump_base + local_start) & (xenos::kEdramTileCount - 1);
    const uint32_t start_rel =
        (global_start - key.base_tiles) & (xenos::kEdramTileCount - 1);
    const uint32_t length =
        std::min(local_end - local_start, xenos::kEdramTileCount - start_rel);
    RenderTarget* targets[1 + xenos::kMaxColorRenderTargets] = {};
    std::vector<Transfer> transfers[1 + xenos::kMaxColorRenderTargets];
    const uint32_t slot = key.is_depth ? 0 : 1;
    targets[slot] = dest;
    rt_shrink_claim_context_ = "tile conversion";
    ChangeOwnershipForTileConversion(key, start_rel, length,
                                     &transfers[slot]);
    if (!RtShrinkApplyGrowth()) {
      REXLOG_ERROR("[rt-shrink] growth failed before a tile conversion");
    }
    if (!transfers[slot].empty()) {
      PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                       targets, transfers, nullptr, nullptr,
                                       nullptr);
    }
  }
  unified_out = true;
  return to_native;
}

bool MetalRenderTargetCache::EffectsNativeEnabled() {
  static const bool enabled = [] {
    // Experimental, opt-in: the effect size itself comes from
    // draw_resolution_scaled_texture_offsets (FABLE2_EFFECTS_NATIVE in the
    // executable); these targets only save GPU time (section 12).
    const char* v = std::getenv("FABLE2_EFFECTS_NATIVE_TARGETS");
    const bool on = v && v[0] == '1';
    REXLOG_INFO(
        "[native-scale] effect render targets at native 1x resolution "
        "when the draw resolution is scaled: {} "
        "(FABLE2_EFFECTS_NATIVE_TARGETS={}, experimental)",
        on ? "on" : "off", v ? v : "unset");
    return on;
  }();
  return enabled;
}

bool MetalRenderTargetCache::ShadowsNativeEnabled() {
  static const bool enabled = [] {
    // Shadow maps at the console's own resolution when the draw resolution
    // is scaled (section 16). =0 keeps them scaled (today's 2x/3x image).
    const char* v = std::getenv("FABLE2_SHADOWS_NATIVE");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[native-scale] shadow maps at native 1x resolution when the draw "
        "resolution is scaled: {} (FABLE2_SHADOWS_NATIVE={})",
        on ? "on" : "off", v ? v : "unset");
    return on;
  }();
  return enabled;
}

bool MetalRenderTargetCache::TexSpaceNativeEnabled() {
  static const bool enabled = [] {
    // Fable II texture-space passes at the console's 1x when the draw
    // resolution is scaled (docs/resolution-scale.md section 23). Scaled, the
    // dog's per-frame lighting texture is rasterized at 2x/3x and its seam
    // dilation (fixed texel-offset fetches) no longer fills the gutters
    // between UV islands, so the fur samples the clear colour there: a dark
    // seam down the dog's chest. =0 keeps them scaled (old 2x/3x image).
    const char* v = std::getenv("FABLE2_TEXSPACE_NATIVE");
    const bool on = !(v && v[0] == '0');
    REXLOG_INFO(
        "[native-scale] texture-space passes (morph, dog lighting) at native "
        "1x resolution when the draw resolution is scaled: {} "
        "(FABLE2_TEXSPACE_NATIVE={})",
        on ? "on" : "off", v ? v : "unset");
    return on;
  }();
  return enabled;
}

bool MetalRenderTargetCache::IsTexSpacePixelShader(uint64_t ucode_hash) {
  switch (ucode_hash) {
    case UINT64_C(0xE2120FEAFA4CF565):  // morph (resolved to 0x12704000)
    case UINT64_C(0x44B567635EB10620):  // dog lighting -> 0x13401000
    case UINT64_C(0x39B39197B46ECA72):  // dog lighting, 2nd variant
    case UINT64_C(0x1B52AA35A311938D):  // seam dilation -> 0x133E1000/F1000
      return true;
    default:
      return false;
  }
}

void MetalRenderTargetCache::LearnShadowTarget(
    const draw_util::ResolveInfo& resolve_info) {
  const uint32_t width = resolve_info.coordinate_info.width_div_8 * 8;
  const uint32_t height = resolve_info.height_div_8 * 8;
  if (width != height || width < 512) {
    return;
  }
  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows,
                                    dump_pitch);
  std::vector<ResolveCopyDumpRectangle> rects;
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, rects);
  for (const ResolveCopyDumpRectangle& rect : rects) {
    if (!rect.render_target) continue;
    const RenderTargetKey key = rect.render_target->key();
    if (!key.is_depth) continue;
    const uint32_t id = ShadowTargetId(key.base_tiles, key.pitch_tiles_at_32bpp);
    if (std::find(shadow_targets_.begin(), shadow_targets_.end(), id) !=
            shadow_targets_.end() ||
        std::find(shadow_rejected_targets_.begin(),
                  shadow_rejected_targets_.end(),
                  id) != shadow_rejected_targets_.end()) {
      continue;
    }
    shadow_targets_.push_back(id);
    REXLOG_INFO(
        "[native-scale] shadow map: depth target @ {}t <{}t> (depth resolve "
        "{}x{} to 0x{:08X}) drawn at native 1x from now on",
        key.base_tiles, key.pitch_tiles_at_32bpp, width, height,
        resolve_info.copy_dest_base);
  }
}

uint32_t MetalRenderTargetCache::NativeScaleRenderTargets(
    uint32_t pitch_tiles_at_32bpp, xenos::MsaaSamples msaa_samples,
    uint32_t used_rts_bits, const uint32_t* edram_bases) {
  if (!IsDrawResolutionScaled()) {
    return 0;
  }
  const bool depth_used = (used_rts_bits & 1) != 0;
  const bool color_used = (used_rts_bits & ~uint32_t(1)) != 0;
  if (depth_used) {
    // Learn the scene width from depth-tested passes.
    if (color_used && pitch_tiles_at_32bpp > effects_scene_pitch_tiles_) {
      effects_scene_pitch_tiles_ = pitch_tiles_at_32bpp;
      if (EffectsNativeEnabled()) {
        REXLOG_INFO(
            "[native-scale] scene pitch {} tiles: colour-only 1xMSAA passes "
            "up to {} tiles stay at native resolution",
            pitch_tiles_at_32bpp, pitch_tiles_at_32bpp / 2);
      }
    }
    if (!ShadowsNativeEnabled()) {
      return 0;
    }
    const uint32_t id = ShadowTargetId(edram_bases[0], pitch_tiles_at_32bpp);
    auto it = std::find(shadow_targets_.begin(), shadow_targets_.end(), id);
    if (color_used) {
      // A depth target that is ever drawn together with colour is scene
      // depth, not a shadow map: never native.
      if (std::find(shadow_rejected_targets_.begin(),
                    shadow_rejected_targets_.end(),
                    id) == shadow_rejected_targets_.end()) {
        shadow_rejected_targets_.push_back(id);
        if (it != shadow_targets_.end()) {
          shadow_targets_.erase(it);
          REXLOG_INFO(
              "[native-scale] depth target @ {}t <{}t> drawn with colour: no "
              "longer a shadow map",
              edram_bases[0], pitch_tiles_at_32bpp);
        }
      }
      return 0;
    }
    if (it == shadow_targets_.end()) {
      return 0;
    }
    ++UploadTrace().sh_draw;
    return 1;
  }
  // Texture-space draws: one 1xMSAA colour target, no depth. Other draws on
  // the same target (clear quads) stay scaled; the ownership transfer
  // between the two keys carries their result.
  if (texspace_draw_hint_ && msaa_samples == xenos::MsaaSamples::k1X &&
      used_rts_bits == 0b10) {
    ++UploadTrace().ts_draw;
    return used_rts_bits;
  }
  if (EffectsNativeEnabled() && msaa_samples == xenos::MsaaSamples::k1X &&
      effects_scene_pitch_tiles_ &&
      pitch_tiles_at_32bpp * 2 <= effects_scene_pitch_tiles_) {
    return used_rts_bits;
  }
  return 0;
}

bool MetalRenderTargetCache::IsCurrentPassNativeScale() const {
  if (current_depth_target_) {
    return current_depth_target_->key().native_scale != 0;
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (current_color_targets_[i]) {
      return current_color_targets_[i]->key().native_scale != 0;
    }
  }
  return false;
}

void MetalRenderTargetCache::DropMixedScaleHostDepth(
    RenderTarget* dest, std::vector<Transfer>& transfers) {
  if (!dest || !IsDrawResolutionScaled()) {
    return;
  }
  const RenderTargetKey dest_key = dest->key();
  for (Transfer& t : transfers) {
    if (!t.host_depth_source) continue;
    const RenderTargetKey hd_key = t.host_depth_source->key();
    if (hd_key.native_scale || dest_key.native_scale) {
      if (ShadowTraceActive()) {
        REXLOG_INFO(
            "[shadow-trace] f{} hostdepth-drop dest [{}] src [{}] hostdepth "
            "[{}] rect {}t+{}t",
            frame_log::CurrentFrame(), dest_key.GetDebugName(),
            t.source ? t.source->key().GetDebugName() : std::string("none"),
            hd_key.GetDebugName(), t.start_tiles, t.end_tiles - t.start_tiles);
      }
      t.host_depth_source = nullptr;
      ++UploadTrace().sh_hostdepth_drop;
    }
  }
}

bool MetalRenderTargetCache::ShadowTraceActive() {
  static const uint64_t first = [] {
    const char* v = std::getenv("FABLE2_SHADOW_TRACE");
    return v ? std::strtoull(v, nullptr, 10) : 0ull;
  }();
  if (!first) {
    return false;
  }
  // FABLE2_SHADOW_TRACE_FRAMES=<n>: trace window length (default 2 frames).
  static const uint64_t count = [] {
    const char* v = std::getenv("FABLE2_SHADOW_TRACE_FRAMES");
    const uint64_t n = v ? std::strtoull(v, nullptr, 10) : 0ull;
    return n ? n : 2ull;
  }();
  const uint64_t f = frame_log::CurrentFrame();
  return f >= first && f < first + count;
}

std::string MetalRenderTargetCache::DescribeCurrentTargetsForTrace() const {
  std::string out = fmt::format(
      "d=[{}]", current_depth_target_
                    ? current_depth_target_->key().GetDebugName()
                    : std::string("-"));
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_color_targets_[i]) {
      out += fmt::format(" c{}=[{}]", i,
                         current_color_targets_[i]->key().GetDebugName());
    }
  }
  return out;
}

bool MetalRenderTargetCache::EffectsNativeTraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_EFFECTS_NATIVE_TRACE");
    return v && v[0] == '1';
  }();
  return enabled;
}

bool MetalRenderTargetCache::IsEffectsNativeEligible(
    RenderTargetKey key) const {
  if (key.native_scale) {
    return true;
  }
  return !key.is_depth && key.msaa_samples == xenos::MsaaSamples::k1X &&
         effects_scene_pitch_tiles_ &&
         key.pitch_tiles_at_32bpp * 2 <= effects_scene_pitch_tiles_;
}

void MetalRenderTargetCache::NoteEffectsNativeDraw(
    RenderTarget* const* targets, bool depth_used) {
  UploadTraceCounters& u = UploadTrace();
  bool any_color = false, all_eligible = true, any_eligible = false;
  for (uint32_t i = 1; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (!targets[i]) continue;
    any_color = true;
    if (IsEffectsNativeEligible(targets[i]->key())) {
      any_eligible = true;
    } else {
      all_eligible = false;
    }
  }
  if (!any_color) return;
  if (depth_used) {
    if (any_eligible) ++u.eff_draw_depth;
  } else if (all_eligible) {
    ++u.eff_draw;
  }
}

bool MetalRenderTargetCache::TileEdramSelfDepthEnabled() {
  static const bool on = [] {
    auto off = [](const char* name) {
      const char* v = std::getenv(name);
      return v && v[0] == '0';
    };
    bool enabled = !off("FABLE2_TILE_EDRAM") &&
                   !off("FABLE2_TILE_EDRAM_SELFDEPTH");
    REXLOG_INFO("[tile-edram] self-source depth transfers in the draw pass: {}",
                enabled ? "on" : "off");
    return enabled;
  }();
  return on;
}

bool MetalRenderTargetCache::DeferredClearsEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_DEFERRED_CLEARS");
    bool enabled = !(v && v[0] == '0');
    REXLOG_INFO(
        "[deferred-clears] resolve clears drawn in the next draw pass: {}",
        enabled ? "on" : "off");
    return enabled;
  }();
  return on;
}

bool MetalRenderTargetCache::TransferQuadStateResetEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TRANSFER_QUAD_STATE_RESET");
    bool enabled = !(v && v[0] == '0');
    REXLOG_INFO(
        "[raster-state] draw-pass transfer quads reset the rasterizer state: {}",
        enabled ? "on" : "off");
    return enabled;
  }();
  return on;
}

void MetalRenderTargetCache::ResetDrawPassRasterizerState(
    MTL::RenderCommandEncoder* encoder) {
  // Metal's defaults for a new render command encoder.
  encoder->setCullMode(MTL::CullModeNone);
  encoder->setFrontFacingWinding(MTL::WindingClockwise);
  encoder->setTriangleFillMode(MTL::TriangleFillModeFill);
  encoder->setDepthBias(0.0f, 0.0f, 0.0f);
  encoder->setDepthClipMode(MTL::DepthClipModeClip);
}

bool MetalRenderTargetCache::AliasClearNativeEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_ALIAS_CLEAR_NATIVE");
    bool enabled = !(v && v[0] == '0');
    REXLOG_INFO(
        "[alias-clear] XDK alias-MSAA clear quads done as EDRAM clears of the "
        "surface's existing variant, no-op draws skipped "
        "(FABLE2_ALIAS_CLEAR_NATIVE): {}",
        enabled ? "on" : "off");
    return enabled;
  }();
  return on;
}

bool MetalRenderTargetCache::AliasClearTraceEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_ALIAS_CLEAR_TRACE");
    return v && v[0] == '1';
  }();
  return on;
}

bool MetalRenderTargetCache::MainDepthNativeEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_MAIN_DEPTH_NATIVE");
    bool enabled = !(v && v[0] == '0') && AliasClearNativeEnabled();
    REXLOG_INFO(
        "[main-depth] depth-word alias clears into a unorm colour variant and "
        "stencil-only alias clears on the same-format variant "
        "(FABLE2_MAIN_DEPTH_NATIVE): {}",
        enabled ? "on" : "off");
    return enabled;
  }();
  return on;
}

bool MetalRenderTargetCache::TryStencilOnlyAliasClear(
    const AliasClearRequest& req) {
  UploadTraceCounters& u = UploadTrace();
  if (GetPath() != Path::kHostRenderTargets || !req.depth || req.color ||
      !req.stencil_only || req.msaa_samples > xenos::MsaaSamples::k4X ||
      !req.pitch_tiles_at_32bpp) {
    ++u.ac_fb_state;
    return false;
  }
  auto x_log2 = [](xenos::MsaaSamples m) {
    return uint32_t(m >= xenos::MsaaSamples::k4X);
  };
  auto y_log2 = [](xenos::MsaaSamples m) {
    return uint32_t(m >= xenos::MsaaSamples::k2X);
  };
  const uint32_t sx1 = req.x1 << x_log2(req.msaa_samples);
  const uint32_t sy1 = req.y1 << y_log2(req.msaa_samples);
  const uint32_t pitch = req.pitch_tiles_at_32bpp;
  if (req.x0 || req.y0 || !sx1 || !sy1 ||
      sx1 > pitch * xenos::kEdramTileWidthSamples) {
    ++u.ac_fb_rect;
    return false;
  }
  // Whole tile rows only: every tile of the range is fully covered, so the
  // ownership change below covers exactly the cleared samples.
  if (sx1 != pitch * xenos::kEdramTileWidthSamples ||
      (sy1 % xenos::kEdramTileHeightSamples)) {
    ++u.ac_fb_rect;
    return false;
  }
  const uint32_t length_tiles = (sy1 / xenos::kEdramTileHeightSamples) * pitch;
  if (!length_tiles || length_tiles > xenos::kEdramTileCount) {
    ++u.ac_fb_rect;
    return false;
  }
  // The existing same-format variant at another MSAA level (unique).
  RenderTargetKey want;
  want.base_tiles = req.depth_base;
  want.pitch_tiles_at_32bpp = pitch;
  want.is_depth = 1;
  want.resource_format = uint32_t(req.depth_format);
  RenderTargetKey key;
  MetalRenderTarget* rt = nullptr;
  uint32_t n = 0;
  for (uint32_t m = 0; m <= uint32_t(xenos::MsaaSamples::k4X); ++m) {
    if (m == uint32_t(req.msaa_samples)) continue;
    RenderTargetKey k = want;
    k.msaa_samples = xenos::MsaaSamples(m);
    auto it = render_target_map_.find(k.key);
    if (it != render_target_map_.end() && it->second) {
      key = k;
      rt = static_cast<MetalRenderTarget*>(it->second);
      ++n;
    }
  }
  if (n != 1 || !rt || rt->needs_initial_clear()) {
    ++u.ac_fb_owner;
    return false;
  }
  // [rt-shrink] no growth of the variant for this (see TryDepthWordAliasClear).
  if (RtShrinkMode() &&
      (length_tiles / pitch) * RtShrinkTileRowHeight(key) > rt->rows_needed()) {
    ++u.ac_fb_owner;
    return false;
  }
  const uint32_t mx = x_log2(key.msaa_samples), my = y_log2(key.msaa_samples);
  if ((sx1 & ((1u << mx) - 1)) || (sy1 & ((1u << my) - 1))) {
    ++u.ac_fb_rect;
    return false;
  }
  Transfer::Rectangle rect;
  rect.x_pixels = 0;
  rect.y_pixels = 0;
  rect.width_pixels = sx1 >> mx;
  rect.height_pixels = sy1 >> my;

  // Committed. Same order as a draw binding the variant: its own queued
  // clear first, then the tiles move to it with the transfers the draw
  // would do (no clear cutout: the depth is kept), then the stencil clear.
  // The variant's own queued clear (the depth word just cleared into it,
  // whose tiles it owns, so no transfer below touches them) is drawn in the
  // same pass as the transfers when there are any, else first on its own.
  size_t carried_index = SIZE_MAX;
  {
    bool in_draw_pass_slot = false;
    for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
      if ((pending_draw_pass_clear_mask_ & (uint32_t(1) << i)) &&
          pending_draw_pass_clear_targets_[i] == rt) {
        in_draw_pass_slot = true;
      }
    }
    uint32_t queued = 0;
    for (size_t c = 0; c < deferred_clears_.size(); ++c) {
      if (deferred_clears_[c].render_target == rt) {
        ++queued;
        carried_index = c;
      }
    }
    if (in_draw_pass_slot || queued != 1 ||
        (deferred_clears_[carried_index].clear_value &
         kClearValueStencilOnly)) {
      carried_index = SIZE_MAX;
      FlushDeferredClearsFor(rt, DeferredClearFlushReason::kReclear);
    }
  }
  RenderTarget* targets[1 + xenos::kMaxColorRenderTargets] = {};
  std::vector<Transfer> transfers[1 + xenos::kMaxColorRenderTargets];
  targets[0] = rt;
  rt_shrink_claim_context_ = "stencil-only alias clear";
  ChangeOwnershipForTileConversion(key, 0, length_tiles, &transfers[0]);
  if (!RtShrinkApplyGrowth()) {
    REXLOG_ERROR("[rt-shrink] growth failed before a stencil-only alias clear");
  }
  for (const Transfer& t : transfers[0]) {
    FlushDeferredClearsFor(t.source, DeferredClearFlushReason::kTransfer);
    // A self host-depth source is stored over the transfer rectangles only,
    // which the carried clear (tiles the variant already owns) never covers.
    if (!(carried_index != SIZE_MAX && t.host_depth_source == rt)) {
      FlushDeferredClearsFor(t.host_depth_source,
                             DeferredClearFlushReason::kTransfer);
    }
  }
  const size_t transfer_count = transfers[0].size();
  const uint64_t value = kClearValueStencilOnly | (req.depth_value & 0xFF);
  // [ssm-merge] queue the transfers and the stencil clear for the next draw
  // pass that binds the variant (FlushQueuedStencilAlias does the standalone
  // work below if anything else comes first).
  bool queued = false;
  if (transfer_count && SsmMergeEnabled() && DeferredClearsEnabled() &&
      REXCVAR_GET(metal_transfer_in_draw_pass)) {
    QueuedStencilAlias q;
    q.render_target = rt;
    q.transfers = transfers[0];
    q.stencil_value = value;
    q.rectangle = rect;
    bool ok = true;
    if (carried_index != SIZE_MAX) {
      for (const DeferredClear& c : deferred_clears_) {
        if (c.render_target == rt) {
          if (q.has_carried) ok = false;  // never more than one
          q.has_carried = true;
          q.carried_rectangle = c.rectangle;
        }
      }
      ok = ok && q.has_carried;
    } else {
      for (const DeferredClear& c : deferred_clears_) {
        if (c.render_target == rt) ok = false;
      }
    }
    // The carried clear must not overlap any transfer rectangle: the draw
    // pass draws it before the transfer quads, and K1 stores the variant's
    // own host depth over the transfer rectangles before the pass opens.
    std::vector<TransferRectanglePlan> plans;
    if (ok && !BuildTransferRectanglePlans(key, q.transfers, nullptr, true,
                                           plans)) {
      ok = false;
    }
    if (ok && q.has_carried) {
      const Transfer::Rectangle& c = q.carried_rectangle;
      for (const TransferRectanglePlan& plan : plans) {
        for (uint32_t r = 0; r < plan.rectangle_count; ++r) {
          const Transfer::Rectangle& t = plan.rectangles[r];
          const bool x_apart = t.x_pixels >= c.x_pixels + c.width_pixels ||
                               c.x_pixels >= t.x_pixels + t.width_pixels;
          const bool y_apart = t.y_pixels >= c.y_pixels + c.height_pixels ||
                               c.y_pixels >= t.y_pixels + t.height_pixels;
          if (!x_apart && !y_apart) ok = false;
        }
      }
    }
    if (ok) {
      queued_stencil_alias_ = std::move(q);
      queued = true;
      u.md_stencil_xfer += uint32_t(transfer_count);
      ++u.ac_deferred;
    }
  }
  if (queued) {
    // Nothing is encoded now.
  } else if (transfer_count) {
    if (carried_index != SIZE_MAX) {
      // Flushing the transfer sources above encodes only other targets'
      // clears; the carried entry is still the variant's single one.
      DeferredClear carried;
      bool found = false;
      for (size_t c = 0; c < deferred_clears_.size(); ++c) {
        if (deferred_clears_[c].render_target == rt) {
          carried = deferred_clears_[c];
          deferred_clears_.erase(deferred_clears_.begin() + c);
          found = true;
          break;
        }
      }
      uint64_t values[1 + xenos::kMaxColorRenderTargets] = {};
      values[0] = carried.clear_value;
      PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                       targets, transfers,
                                       found ? values : nullptr,
                                       found ? &carried.rectangle : nullptr,
                                       nullptr);
      if (found) {
        ++u.md_carried;
      }
    } else {
      PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                       targets, transfers, nullptr, nullptr,
                                       nullptr);
    }
    u.md_stencil_xfer += uint32_t(transfer_count);
  } else if (carried_index != SIZE_MAX) {
    FlushDeferredClearsFor(rt, DeferredClearFlushReason::kReclear);
  }
  if (queued) {
    // The stencil clear is part of the queued work.
  } else if (DeferredClearsEnabled()) {
    DeferredClear clear;
    clear.render_target = rt;
    clear.clear_value = value;
    clear.rectangle = rect;
    deferred_clears_.push_back(clear);
    ++u.ac_deferred;
  } else {
    EncodeResolveClearPass(rt, value, rect);
    ++u.ac_immediate;
  }
  ++u.ac_clear;
  ++u.ac_surfaces;
  ++u.md_stencil;
  if (AliasClearTraceEnabled() && ShadowTraceActive()) {
    REXLOG_INFO(
        "[alias-clear] f{} stencil-only [{}] rect 0,0 {}x{} stencil {:02X} "
        "transfers {} (draw {}x MSAA, {}x{})",
        frame_log::CurrentFrame(), key.GetDebugName(), rect.width_pixels,
        rect.height_pixels, req.depth_value & 0xFF, transfer_count,
        1u << uint32_t(req.msaa_samples), req.x1, req.y1);
  }
  return true;
}

bool MetalRenderTargetCache::SsmMergeEnabled() {
  static const bool on = [] {
    // Default off (docs/ssm-merge-20261015.md): exact, but on the M5 Max the
    // removed pass overlapped texture uploads for free, so 2x GPU time rose
    // about 0.1 ms; opt in with =1 (meant for bandwidth-bound Macs).
    const char* v = std::getenv("FABLE2_SSM_MERGE");  // getenv-ok: once
    const bool enabled = v && v[0] == '1';
    REXLOG_INFO("[ssm-merge] stencil-only alias clear transfers inside the "
                "next draw pass {} (FABLE2_SSM_MERGE={})",
                enabled ? "on" : "off", v ? v : "unset");
    return enabled;
  }();
  return on;
}

bool MetalRenderTargetCache::SsmStencilFoldEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_SSM_STENCIL_FOLD");  // getenv-ok: once
    const bool enabled = !(v && v[0] == '0');
    REXLOG_INFO("[ssm-merge] dead stencil-only clears folded into the "
                "transfers that move their tiles away {} "
                "(FABLE2_SSM_STENCIL_FOLD={})",
                enabled ? "on" : "off", v ? v : "unset");
    return enabled;
  }();
  return on;
}

MTL::Texture* MetalRenderTargetCache::GetConstantStencilTexture(
    uint32_t width, uint32_t height, uint8_t value) {
  const uint64_t k = (uint64_t(width) << 40) | (uint64_t(height) << 8) | value;
  auto it = stencil_const_textures_.find(k);
  if (it != stencil_const_textures_.end()) return it->second;
  MTL::TextureDescriptor* d = MTL::TextureDescriptor::texture2DDescriptor(
      MTL::PixelFormatR8Uint, width, height, false);
  d->setUsage(MTL::TextureUsageShaderRead);
  d->setStorageMode(MTL::StorageModeShared);
  MTL::Texture* t = device_->newTexture(d);
  if (t) {
    std::vector<uint8_t> bytes(size_t(width) * height, value);
    t->replaceRegion(MTL::Region(0, 0, width, height), 0, bytes.data(),
                     width);
    t->setLabel(NS::String::string("ssm constant stencil",
                                   NS::UTF8StringEncoding));
  }
  stencil_const_textures_[k] = t;
  return t;
}

void MetalRenderTargetCache::FoldDeadStencilClears(
    RenderTarget* const* accumulated_targets,
    const std::vector<Transfer>* update_transfers) {
  if (!DeferredClearsEnabled() || deferred_clears_.empty()) return;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    for (const Transfer& t : update_transfers[i]) {
      auto* src = static_cast<MetalRenderTarget*>(t.source);
      if (!src || stencil_const_sources_.count(src)) continue;
      const RenderTargetKey key = src->key();
      MTL::Texture* tex = src->texture();
      if (!key.is_depth || key.msaa_samples != xenos::MsaaSamples::k1X ||
          !tex || tex->sampleCount() > 1) {
        continue;
      }
      bool bound = false;
      for (uint32_t j = 0; j < 1 + xenos::kMaxColorRenderTargets; ++j) {
        if (accumulated_targets[j] == src) bound = true;
      }
      if (bound) continue;
      size_t index = SIZE_MAX;
      uint32_t count = 0;
      for (size_t c = 0; c < deferred_clears_.size(); ++c) {
        if (deferred_clears_[c].render_target == src) {
          index = c;
          ++count;
        }
      }
      if (count != 1) continue;
      const DeferredClear& dc = deferred_clears_[index];
      if (!(dc.clear_value & kClearValueStencilOnly)) continue;
      const Transfer::Rectangle& r = dc.rectangle;
      const uint32_t sx = RenderTargetScaleX(key), sy = RenderTargetScaleY(key);
      if (r.x_pixels || r.y_pixels || r.width_pixels * sx < tex->width() ||
          r.height_pixels * sy < tex->height()) {
        continue;
      }
      // Every tile of the cleared rows now belongs to another target.
      const uint32_t pitch = key.GetPitchTiles();
      const uint32_t rows =
          (r.height_pixels + xenos::kEdramTileHeightSamples - 1) /
          xenos::kEdramTileHeightSamples;
      const uint32_t length = rows * pitch;
      if (!pitch || !length || length > xenos::kEdramTileCount) continue;
      bool owned = false;
      for (uint32_t tile = 0; tile < length && !owned; ++tile) {
        if (GetTileOwner((key.base_tiles + tile) &
                         (xenos::kEdramTileCount - 1)) == key) {
          owned = true;
        }
      }
      if (owned) continue;
      stencil_const_sources_[src] = uint8_t(dc.clear_value & 0xFF);
      deferred_clears_.erase(deferred_clears_.begin() + index);
      ++UploadTrace().ssm_folded;
    }
  }
}

bool MetalRenderTargetCache::ClearConstSourceEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_CLEAR_CONST_SOURCE");  // getenv-ok: once
    const bool enabled = !(v && v[0] == '0');
    REXLOG_INFO("[clear-const] transfers read a pending whole-target clear "
                "from a constant texture {} (FABLE2_CLEAR_CONST_SOURCE={})",
                enabled ? "on" : "off", v ? v : "unset");
    return enabled;
  }();
  return on;
}

MTL::Texture* MetalRenderTargetCache::GetConstantClearColorTexture(
    MetalRenderTarget* rt, uint64_t clear_value) {
  MTL::Texture* src = rt ? rt->transfer_texture() : nullptr;
  if (!src || src != rt->draw_texture() || src->sampleCount() > 1) {
    return nullptr;
  }
  const MTL::PixelFormat fmt = src->pixelFormat();
  const uint32_t w = uint32_t(src->width()), h = uint32_t(src->height());
  for (const ClearConstTexture& c : clear_const_textures_) {
    if (c.format == fmt && c.width == w && c.height == h &&
        c.value == clear_value) {
      return c.texture;
    }
  }
  // A few shapes only (one per frame size in practice); never evicted, since
  // command buffers in flight may still read them.
  if (clear_const_textures_.size() >= 4) return nullptr;
  MTL::ClearColor color = MTL::ClearColor(0.0, 0.0, 0.0, 0.0);
  double depth = 1.0;
  uint32_t stencil = 0;
  if (!ClearValueAsLoadAction(rt->key(), clear_value, &color, &depth,
                              &stencil)) {
    return nullptr;
  }
  MTL::TextureDescriptor* d =
      MTL::TextureDescriptor::texture2DDescriptor(fmt, w, h, false);
  d->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
  d->setStorageMode(MTL::StorageModePrivate);
  MTL::Texture* t = device_->newTexture(d);
  if (!t) return nullptr;
  t->setLabel(NS::String::string("clear-const source", NS::UTF8StringEncoding));
  MTL::CommandBuffer* cmd = command_processor_.RequestTransferCommandBuffer(
      MetalCommandProcessor::TransferRequestSource::kRenderTargetTransfer);
  if (!cmd) {
    t->release();
    return nullptr;
  }
  // The same clear load action the in-pass whole-target clear uses
  // (ClearValueAsLoadAction: formats exact both ways).
  MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::alloc()->init();
  auto* a0 = rpd->colorAttachments()->object(0);
  a0->setTexture(t);
  a0->setLoadAction(MTL::LoadActionClear);
  a0->setStoreAction(MTL::StoreActionStore);
  a0->setClearColor(color);
  MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(rpd);
  rpd->release();
  if (!enc) {
    t->release();
    return nullptr;
  }
  enc->setLabel(NS::String::string("clear-const fill", NS::UTF8StringEncoding));
  enc->endEncoding();
  clear_const_textures_.push_back({fmt, w, h, clear_value, t});
  REXLOG_INFO("[clear-const] constant source texture {}x{} format {} value "
              "{:016X}",
              w, h, uint32_t(fmt), clear_value);
  return t;
}

bool MetalRenderTargetCache::TryClearConstSource(
    RenderTarget* source, RenderTarget* const* accumulated_targets) {
  auto* src = static_cast<MetalRenderTarget*>(source);
  if (!src || !ClearConstSourceEnabled() || !DeferredClearsEnabled() ||
      !ClearLoadActionEnabled()) {
    return false;
  }
  if (clear_const_sources_.count(src)) return true;
  const RenderTargetKey key = src->key();
  if (key.is_depth || key.msaa_samples != xenos::MsaaSamples::k1X) {
    return false;
  }
  // Formats whose clear is exact as a load action (ApplyClearLoadActions).
  switch (key.GetColorFormat()) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      break;
    default:
      return false;
  }
  for (uint32_t j = 0; j < 1 + xenos::kMaxColorRenderTargets; ++j) {
    if (accumulated_targets[j] == src) return false;
    if ((pending_draw_pass_clear_mask_ & (uint32_t(1) << j)) &&
        pending_draw_pass_clear_targets_[j] == src) {
      return false;
    }
  }
  size_t index = SIZE_MAX;
  uint32_t count = 0;
  for (size_t c = 0; c < deferred_clears_.size(); ++c) {
    if (deferred_clears_[c].render_target == src) {
      index = c;
      ++count;
    }
  }
  if (count != 1) return false;
  const DeferredClear& dc = deferred_clears_[index];
  bool full_target = false;
  DeferredClearTouchedKb(src, dc.rectangle, RenderTargetScaleX(key),
                         RenderTargetScaleY(key), &full_target);
  if (!full_target) return false;
  MTL::Texture* t = GetConstantClearColorTexture(src, dc.clear_value);
  if (!t) return false;
  clear_const_sources_[src] = t;
  ++UploadTrace().clr_const;
  return true;
}

bool MetalRenderTargetCache::FlushQueuedStencilAlias() {
  if (!queued_stencil_alias_.render_target) return true;
  QueuedStencilAlias q = std::move(queued_stencil_alias_);
  queued_stencil_alias_ = QueuedStencilAlias();
  ++UploadTrace().ssm_flushed;
  // Exactly what TryStencilOnlyAliasClear does without the merge: the
  // transfers with the carried clear in one pass, then the stencil clear
  // deferred.
  RenderTarget* targets[1 + xenos::kMaxColorRenderTargets] = {};
  std::vector<Transfer> transfers[1 + xenos::kMaxColorRenderTargets];
  targets[0] = q.render_target;
  transfers[0] = q.transfers;
  bool ok = true;
  DeferredClear carried;
  bool found = false;
  if (q.has_carried) {
    for (size_t c = 0; c < deferred_clears_.size(); ++c) {
      if (deferred_clears_[c].render_target == q.render_target) {
        carried = deferred_clears_[c];
        deferred_clears_.erase(deferred_clears_.begin() + c);
        found = true;
        break;
      }
    }
  }
  if (found) {
    uint64_t values[1 + xenos::kMaxColorRenderTargets] = {};
    values[0] = carried.clear_value;
    ok = PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                          targets, transfers, values,
                                          &carried.rectangle, nullptr);
    ++UploadTrace().md_carried;
  } else {
    ok = PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                          targets, transfers, nullptr,
                                          nullptr, nullptr);
  }
  DeferredClear clear;
  clear.render_target = q.render_target;
  clear.clear_value = q.stencil_value;
  clear.rectangle = q.rectangle;
  deferred_clears_.push_back(clear);
  return ok;
}

bool MetalRenderTargetCache::StencilAliasCoversTarget(
    const QueuedStencilAlias& q) const {
  // Full-width row bands (carried clear + transfer rectangles) that cover
  // every row of the variant's texture: the depth/stencil load can be
  // DontCare (every sample is written by in-pass quads before any draw).
  MetalRenderTarget* rt = q.render_target;
  MTL::Texture* tex = rt ? rt->draw_texture() : nullptr;
  if (!tex || tex->sampleCount() > 1) return false;
  const RenderTargetKey key = rt->key();
  const uint32_t sx = RenderTargetScaleX(key), sy = RenderTargetScaleY(key);
  const uint32_t width = uint32_t(tex->width());
  const uint32_t height = uint32_t(tex->height());
  std::vector<std::pair<uint32_t, uint32_t>> bands;
  auto add = [&](const Transfer::Rectangle& r) {
    if (r.x_pixels || r.width_pixels * sx != width) {
      static uint32_t logged_w = 0;
      if (AliasClearTraceEnabled() && logged_w++ < 4) {
        REXLOG_INFO("[ssm-merge] cover check: band x {} w {} (x{}) vs width {}",
                    r.x_pixels, r.width_pixels, sx, width);
      }
      return false;
    }
    bands.emplace_back(r.y_pixels * sy, (r.y_pixels + r.height_pixels) * sy);
    return true;
  };
  if (q.has_carried && !add(q.carried_rectangle)) return false;
  std::vector<TransferRectanglePlan> plans;
  if (!BuildTransferRectanglePlans(key, q.transfers, nullptr, true, plans)) {
    return false;
  }
  for (const TransferRectanglePlan& plan : plans) {
    for (uint32_t r = 0; r < plan.rectangle_count; ++r) {
      if (!add(plan.rectangles[r])) return false;
    }
  }
  std::sort(bands.begin(), bands.end());
  uint32_t covered = 0;
  bool gap = false;
  for (const auto& b : bands) {
    if (b.first > covered) gap = true;
    covered = std::max(covered, b.second);
  }
  static uint32_t logged = 0;
  if (AliasClearTraceEnabled() && logged < 4) {
    ++logged;
    std::string bs;
    for (const auto& b : bands) bs += fmt::format(" [{},{})", b.first, b.second);
    REXLOG_INFO("[ssm-merge] cover check {}x{} bands{} -> {}", width, height,
                bs, (!gap && covered >= height) ? "full" : "partial");
  }
  return !gap && covered >= height;
}

uint64_t MetalRenderTargetCache::MainDepthDumpFrame() {
  static const uint64_t frame = [] {
    const char* v = std::getenv("FABLE2_MAIN_DEPTH_DUMP");
    const char* d = std::getenv("FABLE2_MAIN_DEPTH_DUMP_DIR");
    return (v && d && d[0]) ? std::strtoull(v, nullptr, 10) : 0ull;
  }();
  return frame;
}

void MetalRenderTargetCache::MainDepthDumpEdram(const char* tag) {
  (void)FlushQueuedStencilAlias();
  const char* dir = std::getenv("FABLE2_MAIN_DEPTH_DUMP_DIR");
  if (!dir || !dir[0] || !edram_buffer_ ||
      GetPath() != Path::kHostRenderTargets) {
    return;
  }
  FlushDeferredClears(DeferredClearFlushReason::kOther);
  MTL::CommandBuffer* cmd = command_processor_.RequestTransferCommandBuffer();
  if (!cmd) {
    return;
  }
  // The whole EDRAM: 128 rows of 16 tiles.
  DumpRenderTargets(0, 16, 128, 16, cmd, "XeniaMainDepthDump");
  const NS::UInteger length = edram_buffer_->length();
  MTL::Buffer* readback =
      device_->newBuffer(length, MTL::ResourceStorageModeShared);
  if (!readback) {
    return;
  }
  MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
  if (!blit) {
    readback->release();
    return;
  }
  blit->copyFromBuffer(edram_buffer_, 0, readback, 0, length);
  blit->endEncoding();
  const std::string path = fmt::format("{}/f{}-{}.bin", dir,
                                       frame_log::CurrentFrame(), tag);
  cmd->addCompletedHandler([readback, path, length](MTL::CommandBuffer*) {
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(readback->contents(), 1, size_t(length), f);
      std::fclose(f);
    }
    readback->release();
  });
  REXLOG_INFO("[main-depth] EDRAM dump {} queued ({} bytes)", path, length);
}

bool MetalRenderTargetCache::CommitAliasClearSurface(
    uint32_t base, uint32_t pitch, xenos::MsaaSamples msaa, bool is_depth,
    uint32_t format, uint32_t value, uint32_t width, uint32_t height,
    const AliasClearRequest& req) {
  UploadTraceCounters& u = UploadTrace();
  draw_util::ResolveInfo ri;
  ri.rb_copy_control.value = 0;
  ri.coordinate_info.width_div_8 = width >> 3;
  ri.coordinate_info.draw_resolution_scale_x = draw_resolution_scale_x();
  ri.coordinate_info.draw_resolution_scale_y = draw_resolution_scale_y();
  ri.height_div_8 = height >> 3;
  ri.depth_original_base = 0;
  ri.color_original_base = 0;
  ri.rb_depth_clear = 0;
  ri.rb_color_clear = 0;
  ri.rb_color_clear_lo = 0;
  draw_util::ResolveEdramInfo edram;
  edram.pitch_tiles = pitch;
  edram.msaa_samples = msaa;
  edram.is_depth = is_depth ? 1 : 0;
  edram.base_tiles = base;
  edram.format = format;
  edram.format_is_64bpp = 0;
  if (is_depth) {
    ri.rb_copy_control.depth_clear_enable = 1;
    ri.depth_edram_info = edram;
    ri.depth_original_base = base;
    ri.rb_depth_clear = value;
  } else {
    ri.rb_copy_control.color_clear_enable = 1;
    ri.color_edram_info = edram;
    ri.color_original_base = base;
    ri.rb_color_clear = value;
  }
  Transfer::Rectangle rect;
  RenderTarget* targets[2] = {};
  std::vector<Transfer> transfers[2];
  rt_shrink_claim_context_ = "alias clear";
  if (!PrepareHostRenderTargetsResolveClear(ri, rect, targets[0], transfers[0],
                                            targets[1], transfers[1])) {
    // Nothing changed for this surface; a clear done for the other one
    // stays valid, the draw writes the same values again.
    ++u.ac_fb_state;
    return false;
  }
  ++u.ac_surfaces;
  if (!RtShrinkApplyGrowth()) {
    REXLOG_ERROR("[rt-shrink] growth failed before an alias clear");
  }
  uint64_t values[2] = {is_depth ? value : 0, is_depth ? 0 : value};
  // Same order rules as the resolve clear in Resolve.
  for (uint32_t c = 0; c < 2; ++c) {
    if (!targets[c]) continue;
    FlushDeferredClearsFor(targets[c], DeferredClearFlushReason::kReclear);
    for (const Transfer& t : transfers[c]) {
      FlushDeferredClearsFor(t.source, DeferredClearFlushReason::kTransfer);
      FlushDeferredClearsFor(t.host_depth_source,
                             DeferredClearFlushReason::kTransfer);
    }
  }
  if (DeferredClearsEnabled() && transfers[0].empty() &&
      transfers[1].empty()) {
    for (uint32_t c = 0; c < 2; ++c) {
      auto* rt = static_cast<MetalRenderTarget*>(targets[c]);
      if (!rt) continue;
      if (rt->needs_initial_clear()) {
        rt->SetNeedsInitialClear(false);
        MarkRenderPassDescriptorDirty();
      }
      DeferredClear clear;
      clear.render_target = rt;
      clear.clear_value = values[c];
      clear.rectangle = rect;
      deferred_clears_.push_back(clear);
      ++u.ac_deferred;
    }
  } else {
    PerformTransfersAndResolveClears(2, targets, transfers, values, &rect,
                                     nullptr);
    ++u.ac_immediate;
  }
  if (AliasClearTraceEnabled() && ShadowTraceActive()) {
    for (uint32_t c = 0; c < 2; ++c) {
      if (!targets[c]) continue;
      REXLOG_INFO(
          "[alias-clear] f{} clear [{}] rect {},{} {}x{} value {:08X} "
          "transfers {} (draw {}x MSAA, {}x{})",
          frame_log::CurrentFrame(), targets[c]->key().GetDebugName(),
          rect.x_pixels, rect.y_pixels, rect.width_pixels,
          rect.height_pixels, uint32_t(values[c]), transfers[c].size(),
          1u << uint32_t(req.msaa_samples), req.x1, req.y1);
    }
  }
  return true;
}

bool MetalRenderTargetCache::TryDepthWordAliasClear(
    const AliasClearRequest& req) {
  UploadTraceCounters& u = UploadTrace();
  auto x_log2 = [](xenos::MsaaSamples m) {
    return uint32_t(m >= xenos::MsaaSamples::k4X);
  };
  auto y_log2 = [](xenos::MsaaSamples m) {
    return uint32_t(m >= xenos::MsaaSamples::k2X);
  };
  const uint32_t pitch = req.pitch_tiles_at_32bpp;
  const uint32_t sx1 = req.x1 << x_log2(req.msaa_samples);
  const uint32_t sy1 = req.y1 << y_log2(req.msaa_samples);
  // Whole tile rows at the surface origin: every tile of the range is fully
  // covered, and each piece below is whole rows of its own target.
  if (req.x0 || req.y0 || !pitch ||
      sx1 != pitch * xenos::kEdramTileWidthSamples || !sy1 ||
      (sy1 % xenos::kEdramTileHeightSamples)) {
    ++u.ac_fb_rect;
    return false;
  }
  const uint32_t rows = sy1 / xenos::kEdramTileHeightSamples;
  const uint32_t length_tiles = rows * pitch;
  if (length_tiles > xenos::kEdramTileCount) {
    ++u.ac_fb_rect;
    return false;
  }
  // Existing targets of this pitch that hold any 32-bit word exactly: D24S8
  // (the word's depth is UNorm24To32(word >> 8), which TryAliasClearDraw
  // checked equals the z the draw writes; stencil is the low byte) and
  // 8888 / 10:10:10:2 unorm colour (RGBA8Unorm / RGB10A2Unorm; transfers
  // pack them back into the same word). All 32bpp keys share one tile
  // layout, so a target whose base lies in the range on a tile-row boundary
  // can hold the rows from its base on. Each row band goes to the target
  // with the nearest base at or before it (no texture growth past what that
  // target addresses anyway); per base: the current owner of that tile,
  // then a D24S8 target, then a colour one, fewest samples first; a tie
  // between two different targets (8888 and 10:10:10:2 at one MSAA level)
  // leaves that base out.
  const uint32_t b = req.depth_base;
  struct Pick {
    RenderTargetKey key;
    int rank = 99;
    bool tie = false;
  };
  std::vector<Pick> by_row(rows);
  for (const auto& entry : render_target_map_) {
    if (!entry.second) continue;
    RenderTargetKey k;
    k.key = entry.first;
    if (k.pitch_tiles_at_32bpp != pitch || k.native_scale ||
        k.msaa_samples > xenos::MsaaSamples::k4X) {
      continue;
    }
    bool word_exact;
    if (k.is_depth) {
      word_exact = k.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24S8;
    } else {
      word_exact =
          k.GetColorFormat() == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
          k.GetColorFormat() == xenos::ColorRenderTargetFormat::k_2_10_10_10;
    }
    if (!word_exact) continue;
    if (k.is_depth && k.base_tiles == b && k.msaa_samples == req.msaa_samples) {
      continue;  // the aliased target itself
    }
    const uint32_t rel = (k.base_tiles - b) & (xenos::kEdramTileCount - 1);
    if (rel >= length_tiles || (rel % pitch)) continue;
    const uint32_t row = rel / pitch;
    const uint32_t mx = x_log2(k.msaa_samples), my = y_log2(k.msaa_samples);
    if ((sx1 & ((1u << mx) - 1)) || (xenos::kEdramTileHeightSamples >> my) & 7) {
      continue;
    }
    // Lower is better: the owner of the base tile, then D24S8, then colour;
    // within a kind the fewest samples (cheapest to clear and transfer).
    int rank = (k.is_depth ? 10 : 20) + int(k.msaa_samples);
    if (GetTileOwner(k.base_tiles) == k) {
      rank = 0;
    }
    Pick& pick = by_row[row];
    if (rank < pick.rank) {
      pick.key = k;
      pick.rank = rank;
      pick.tie = false;
    } else if (rank == pick.rank && pick.key != k) {
      pick.tie = true;
    }
  }
  std::vector<uint32_t> starts;
  for (uint32_t r = 0; r < rows; ++r) {
    if (by_row[r].rank != 99 && !by_row[r].tie) {
      starts.push_back(r);
    }
  }
  if (starts.empty() || starts[0] != 0) {
    ++u.ac_fb_owner;
    return false;
  }
  // [rt-shrink] never grow a target for this: a band past the rows its
  // target already addresses keeps the draw (a taller long-lived target
  // would load and store more every frame).
  if (RtShrinkMode()) {
    for (size_t i = 0; i < starts.size(); ++i) {
      const uint32_t r1 = i + 1 < starts.size() ? starts[i + 1] : rows;
      const RenderTargetKey& k = by_row[starts[i]].key;
      auto it = render_target_map_.find(k.key);
      const uint32_t need = (r1 - starts[i]) * RtShrinkTileRowHeight(k);
      if (it == render_target_map_.end() || !it->second ||
          need > static_cast<MetalRenderTarget*>(it->second)->rows_needed()) {
        ++u.ac_fb_owner;
        return false;
      }
    }
  }
  // Committed (no side effects before this point).
  for (size_t i = 0; i < starts.size(); ++i) {
    const uint32_t r0 = starts[i];
    const uint32_t r1 = i + 1 < starts.size() ? starts[i + 1] : rows;
    const RenderTargetKey& k = by_row[r0].key;
    const uint32_t mx = x_log2(k.msaa_samples), my = y_log2(k.msaa_samples);
    const uint32_t width = sx1 >> mx;
    const uint32_t height = ((r1 - r0) * xenos::kEdramTileHeightSamples) >> my;
    if (!CommitAliasClearSurface(k.base_tiles, pitch, k.msaa_samples,
                                 k.is_depth != 0, k.resource_format,
                                 req.depth_value, width, height, req)) {
      return false;
    }
    ++u.md_word;
  }
  ++u.ac_clear;
  return true;
}

bool MetalRenderTargetCache::TryAliasClear(const AliasClearRequest& req) {
  UploadTraceCounters& u = UploadTrace();
  // [ssm-merge] an earlier stencil-only clear's queued work comes first.
  if (!FlushQueuedStencilAlias()) return false;
  if (req.stencil_only) {
    return TryStencilOnlyAliasClear(req);
  }
  if (GetPath() != Path::kHostRenderTargets ||
      req.msaa_samples > xenos::MsaaSamples::k4X || !req.pitch_tiles_at_32bpp ||
      (!req.depth && !req.color)) {
    ++u.ac_fb_state;
    return false;
  }
  auto x_log2 = [](xenos::MsaaSamples m) {
    return uint32_t(m >= xenos::MsaaSamples::k4X);
  };
  auto y_log2 = [](xenos::MsaaSamples m) {
    return uint32_t(m >= xenos::MsaaSamples::k2X);
  };
  // The covered rectangle in EDRAM samples. Only rectangles at the surface
  // origin (every XDK alias clear seen); within the pitch, no wrap past the
  // 2048-tile addressing period.
  const uint32_t sx1 = req.x1 << x_log2(req.msaa_samples);
  const uint32_t sy1 = req.y1 << y_log2(req.msaa_samples);
  const uint32_t pitch = req.pitch_tiles_at_32bpp;
  if (req.x0 || req.y0 || !sx1 || !sy1 ||
      sx1 > pitch * xenos::kEdramTileWidthSamples) {
    ++u.ac_fb_rect;
    return false;
  }
  const uint32_t tile_rows = (sy1 + xenos::kEdramTileHeightSamples - 1) /
                             xenos::kEdramTileHeightSamples;
  const uint32_t length_tiles =
      (tile_rows - 1) * pitch +
      (sx1 + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples;
  if (length_tiles > xenos::kEdramTileCount) {
    ++u.ac_fb_rect;
    return false;
  }
  if (req.depth && req.color) {
    // Both ranges [base, base + length) modulo the addressing period must be
    // disjoint (the draw path would drop one of the two otherwise).
    uint32_t a = req.depth_base, b = req.color_base;
    uint32_t d = (b - a) & (xenos::kEdramTileCount - 1);
    uint32_t e = (a - b) & (xenos::kEdramTileCount - 1);
    if (d < length_tiles || e < length_tiles) {
      ++u.ac_fb_rect;
      return false;
    }
  }

  // Pick the existing MSAA variant of each surface: the owner of its first
  // tile if that is such a variant, else the only other variant created so
  // far. The clear must not create the aliased target.
  struct Plan {
    bool used = false;
    bool is_depth = false;
    uint32_t base = 0;
    uint32_t format = 0;
    uint32_t value = 0;
    xenos::MsaaSamples msaa = xenos::MsaaSamples::k1X;
    uint32_t width = 0, height = 0;
  } plans[2];
  plans[0].used = req.depth;
  plans[0].is_depth = true;
  plans[0].base = req.depth_base;
  plans[0].format = uint32_t(req.depth_format);
  plans[0].value = req.depth_value;
  plans[1].used = req.color;
  plans[1].base = req.color_base;
  plans[1].format = uint32_t(req.color_format);
  plans[1].value = req.color_value;
  for (Plan& p : plans) {
    if (!p.used) continue;
    RenderTargetKey want;
    want.base_tiles = p.base;
    want.pitch_tiles_at_32bpp = pitch;
    want.is_depth = p.is_depth ? 1 : 0;
    want.resource_format = p.format;
    bool found = false;
    RenderTargetKey owner = GetTileOwner(p.base);
    const bool owner_same_surface =
        !owner.IsEmpty() && owner.base_tiles == want.base_tiles &&
        owner.pitch_tiles_at_32bpp == want.pitch_tiles_at_32bpp &&
        owner.msaa_samples != req.msaa_samples;
    // An all-zero word is zero in every 32bpp format, colour or depth (depth
    // 0, stencil 0): the XDK clears colour surfaces through a depth alias
    // this way, and today's transfer reads the zeros back as the same bits.
    const bool owner_any_format_ok =
        p.value == 0 &&
        (owner.is_depth ||
         !xenos::IsColorRenderTargetFormat64bpp(owner.GetColorFormat()));
    if (owner_same_surface && owner.is_depth == want.is_depth &&
        owner.resource_format == want.resource_format) {
      p.msaa = owner.msaa_samples;
      found = true;
    } else if (owner_same_surface && owner_any_format_ok) {
      p.msaa = owner.msaa_samples;
      p.is_depth = owner.is_depth != 0;
      p.format = owner.resource_format;
      found = true;
      ++u.ac_cross_format;
    } else {
      uint32_t n = 0;
      for (uint32_t m = 0; m <= uint32_t(xenos::MsaaSamples::k4X); ++m) {
        if (m == uint32_t(req.msaa_samples)) continue;
        RenderTargetKey k = want;
        k.msaa_samples = xenos::MsaaSamples(m);
        auto it = render_target_map_.find(k.key);
        if (it != render_target_map_.end() && it->second) {
          p.msaa = xenos::MsaaSamples(m);
          ++n;
        }
      }
      found = n == 1;
      if (!found && !n && p.is_depth && p.value && !req.color &&
          req.depth_format == xenos::DepthRenderTargetFormat::kD24S8 &&
          MainDepthNativeEnabled()) {
        // [main-depth] a nonzero depth word with no D24S8 variant.
        return TryDepthWordAliasClear(req);
      }
    }
    if (!found) {
      ++u.ac_fb_owner;
      return false;
    }
    const uint32_t mx = x_log2(p.msaa), my = y_log2(p.msaa);
    if ((sx1 & ((1u << mx) - 1)) || (sy1 & ((1u << my) - 1))) {
      ++u.ac_fb_rect;
      return false;
    }
    p.width = sx1 >> mx;
    p.height = sy1 >> my;
    // Resolve clears address whole 8x8 pixel blocks.
    if ((p.width & 7) || (p.height & 7)) {
      ++u.ac_fb_rect;
      return false;
    }
  }

  // Committed: the same ownership change and clear a resolve clear of that
  // rectangle does (transfers only for partly covered tiles of other owners).
  for (Plan& p : plans) {
    if (!p.used) continue;
    if (!CommitAliasClearSurface(p.base, pitch, p.msaa, p.is_depth, p.format,
                                 p.value, p.width, p.height, req)) {
      return false;
    }
  }
  ++u.ac_clear;
  return true;
}

bool MetalRenderTargetCache::DeferredClearsSwapFlush() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_DEFERRED_CLEARS_SWAP_FLUSH");
    bool enabled = v && v[0] == '1';
    if (enabled) {
      REXLOG_INFO("[deferred-clears] flush every deferred clear at frame start");
    }
    return enabled;
  }();
  return on;
}

uint32_t MetalRenderTargetCache::DeferredClearTouchedKb(
    const MetalRenderTarget* rt, const Transfer::Rectangle& rect,
    uint32_t scale_x, uint32_t scale_y, bool* full_target_out) {
  if (full_target_out) *full_target_out = false;
  if (!rt) return 0;
  MTL::Texture* tex = rt->key().is_depth ? rt->texture() : rt->transfer_texture();
  if (!tex) return 0;
  uint32_t w = uint32_t(tex->width()), h = uint32_t(tex->height());
  uint32_t x = rect.x_pixels * scale_x, y = rect.y_pixels * scale_y;
  if (x >= w || y >= h) return 0;
  uint32_t rw = std::min(rect.width_pixels * scale_x, w - x);
  uint32_t rh = std::min(rect.height_pixels * scale_y, h - y);
  bool full = !x && !y && rw == w && rh == h;
  if (full_target_out) *full_target_out = full;
  // Same model as tr_n_xfp_clear_kb: a partial clear pass loads and stores the
  // rectangle; a whole-target one DontCare-loads.
  uint32_t bytes_per_sample = 4;
  switch (tex->pixelFormat()) {
    case MTL::PixelFormatDepth32Float_Stencil8:
      bytes_per_sample = 5;
      break;
    case MTL::PixelFormatRGBA16Float:
    case MTL::PixelFormatRGBA16Uint:
    case MTL::PixelFormatRGBA16Unorm:
    case MTL::PixelFormatRGBA16Snorm:
    case MTL::PixelFormatRG32Float:
    case MTL::PixelFormatRG32Uint:
      bytes_per_sample = 8;
      break;
    default:
      break;
  }
  uint64_t samples = std::max<uint64_t>(1, tex->sampleCount());
  return uint32_t(uint64_t(rw) * rh * samples * bytes_per_sample *
                  (full ? 1 : 2) / 1024);
}

bool MetalRenderTargetCache::EncodeResolveClearPass(
    RenderTarget* render_target, uint64_t clear_value,
    const Transfer::Rectangle& rectangle, MTL::CommandBuffer* command_buffer) {
  RenderTarget* targets[1] = {render_target};
  std::vector<Transfer> no_transfers[1];
  uint64_t values[1] = {clear_value};
  return PerformTransfersAndResolveClears(1, targets, no_transfers, values,
                                          &rectangle, command_buffer);
}

void MetalRenderTargetCache::NoteDeferredClearFlush(
    DeferredClearFlushReason reason, const DeferredClear& clear) {
  (void)clear;
  UploadTraceCounters& u = UploadTrace();
  switch (reason) {
    case DeferredClearFlushReason::kResolve: ++u.dc_fl_resolve; break;
    case DeferredClearFlushReason::kTransfer: ++u.dc_fl_xfer; break;
    case DeferredClearFlushReason::kReclear: ++u.dc_fl_reclear; break;
    case DeferredClearFlushReason::kPreflight: ++u.dc_fl_preflight; break;
    case DeferredClearFlushReason::kGuard: ++u.dc_fl_guard; break;
    case DeferredClearFlushReason::kSwap: ++u.dc_fl_swap; break;
    default: ++u.dc_fl_other; break;
  }
}

bool MetalRenderTargetCache::FlushPendingDrawPassClears(
    DeferredClearFlushReason reason) {
  bool ok = true;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_draw_pass_clear_mask_ & (uint32_t(1) << i))) continue;
    DeferredClear clear;
    clear.render_target =
        static_cast<MetalRenderTarget*>(pending_draw_pass_clear_targets_[i]);
    clear.clear_value = pending_draw_pass_clear_values_[i];
    clear.rectangle = pending_draw_pass_clear_rects_[i];
    pending_draw_pass_clear_targets_[i] = nullptr;
    pending_draw_pass_clear_mask_ &= ~(uint32_t(1) << i);
    if (!clear.render_target) continue;
    NoteDeferredClearFlush(reason, clear);
    ok = EncodeResolveClearPass(clear.render_target, clear.clear_value,
                                clear.rectangle) && ok;
  }
  return ok;
}

bool MetalRenderTargetCache::FlushDeferredClearsFor(
    const RenderTarget* render_target, DeferredClearFlushReason reason) {
  if (!render_target) return true;
  bool ok = true;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    if ((pending_draw_pass_clear_mask_ & (uint32_t(1) << i)) &&
        pending_draw_pass_clear_targets_[i] == render_target) {
      DeferredClear clear;
      clear.render_target =
          static_cast<MetalRenderTarget*>(pending_draw_pass_clear_targets_[i]);
      clear.clear_value = pending_draw_pass_clear_values_[i];
      clear.rectangle = pending_draw_pass_clear_rects_[i];
      pending_draw_pass_clear_targets_[i] = nullptr;
      pending_draw_pass_clear_mask_ &= ~(uint32_t(1) << i);
      NoteDeferredClearFlush(reason, clear);
      ok = EncodeResolveClearPass(clear.render_target, clear.clear_value,
                                  clear.rectangle) && ok;
    }
  }
  for (size_t c = 0; c < deferred_clears_.size();) {
    if (deferred_clears_[c].render_target != render_target) {
      ++c;
      continue;
    }
    DeferredClear clear = deferred_clears_[c];
    deferred_clears_.erase(deferred_clears_.begin() + c);
    NoteDeferredClearFlush(reason, clear);
    ok = EncodeResolveClearPass(clear.render_target, clear.clear_value,
                                clear.rectangle) && ok;
  }
  return ok;
}

bool MetalRenderTargetCache::FlushDeferredClears(
    DeferredClearFlushReason reason) {
  bool ok = FlushPendingDrawPassClears(reason);
  if (deferred_clears_.empty()) return ok;
  std::vector<DeferredClear> clears;
  clears.swap(deferred_clears_);
  for (const DeferredClear& clear : clears) {
    NoteDeferredClearFlush(reason, clear);
    ok = EncodeResolveClearPass(clear.render_target, clear.clear_value,
                                clear.rectangle) && ok;
  }
  return ok;
}

bool MetalRenderTargetCache::ClearLoadActionEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_CLEAR_LOAD_ACTION");  // getenv-ok: once
    const bool enabled = !(v && v[0] == '0');
    REXLOG_INFO("[ssmask] whole-target in-pass clears as load actions {} "
                "(FABLE2_CLEAR_LOAD_ACTION={})",
                enabled ? "on" : "off", v ? v : "unset");
    return enabled;
  }();
  return on;
}

uint32_t MetalRenderTargetCache::ApplyClearLoadActions(
    MTL::RenderPassDescriptor* pass) {
  if (!pass || !pending_draw_pass_clear_mask_ || !ClearLoadActionEnabled() ||
      !REXCVAR_GET(metal_transfer_in_draw_pass)) {
    return 0;
  }
  uint32_t changed = 0;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    const uint32_t bit = uint32_t(1) << i;
    if (!(pending_draw_pass_clear_mask_ & bit)) continue;
    auto* rt =
        static_cast<MetalRenderTarget*>(pending_draw_pass_clear_targets_[i]);
    if (!rt) continue;
    const uint64_t value = pending_draw_pass_clear_values_[i];
    const RenderTargetKey key = rt->key();
    // A stencil-only clear keeps the depth: never a load action. The flag
    // exists for depth targets only: a 32bpp colour clear value is repeated
    // in both halves, so bit 63 is just the colour word's top bit there
    // ([clear-const]: such colour clears were drawn as quads before;
    // FABLE2_CLEAR_CONST_SOURCE=0 keeps that).
    if ((value & kClearValueStencilOnly) &&
        (key.is_depth || !ClearConstSourceEnabled())) {
      continue;
    }
    // Formats whose clear value converts exactly both ways (the clear quad's
    // float constants and the load action's clear value are the same
    // numbers; unorm and fp16 hold them exactly).
    if (key.is_depth) {
      if (i != 0) continue;
    } else {
      if (i == 0) continue;
      switch (key.GetColorFormat()) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
          break;
        default:
          continue;
      }
    }
    bool full_target = false;
    DeferredClearTouchedKb(rt, pending_draw_pass_clear_rects_[i],
                           RenderTargetScaleX(key), RenderTargetScaleY(key),
                           &full_target);
    if (!full_target) continue;
    MTL::ClearColor color = MTL::ClearColor(0.0, 0.0, 0.0, 0.0);
    double depth = 1.0;
    uint32_t stencil = 0;
    if (!ClearValueAsLoadAction(key, value, &color, &depth, &stencil)) continue;
    if (i == 0) {
      MTL::RenderPassDepthAttachmentDescriptor* da = pass->depthAttachment();
      if (!da || da->texture() != rt->texture() ||
          da->loadAction() != MTL::LoadActionLoad) {
        continue;
      }
      MTL::RenderPassStencilAttachmentDescriptor* sa = pass->stencilAttachment();
      const MTL::PixelFormat fmt = rt->texture()->pixelFormat();
      const bool has_stencil = fmt == MTL::PixelFormatDepth32Float_Stencil8 ||
                               fmt == MTL::PixelFormatDepth24Unorm_Stencil8;
      if (has_stencil && (!sa || sa->texture() != rt->texture() ||
                          sa->loadAction() != MTL::LoadActionLoad)) {
        continue;
      }
      da->setLoadAction(MTL::LoadActionClear);
      da->setClearDepth(depth);
      if (has_stencil) {
        sa->setLoadAction(MTL::LoadActionClear);
        sa->setClearStencil(stencil);
      }
    } else {
      MTL::RenderPassColorAttachmentDescriptor* ca =
          pass->colorAttachments()->object(i - 1);
      if (!ca || ca->texture() != rt->transfer_texture() ||
          ca->texture() != rt->draw_texture() ||
          ca->loadAction() != MTL::LoadActionLoad) {
        continue;
      }
      ca->setLoadAction(MTL::LoadActionClear);
      ca->setClearColor(color);
    }
    changed |= bit;
    UploadTraceCounters& u = UploadTrace();
    ++u.ssm_clear_la;
    ++u.dc_merged;
    ++u.dc_fullrect;
    pending_draw_pass_clear_targets_[i] = nullptr;
    pending_draw_pass_clear_mask_ &= ~bit;
  }
  if (changed && !HasPendingDrawPassTransfers()) {
    ClearPendingDrawPassTransfers();
  }
  return changed;
}

void MetalRenderTargetCache::RestoreClearLoadActions(
    MTL::RenderPassDescriptor* pass, uint32_t changed) {
  // The descriptor is the cache's: later passes with it load as before.
  if (!pass || !changed) return;
  if (changed & 1) {
    if (auto* da = pass->depthAttachment()) da->setLoadAction(MTL::LoadActionLoad);
    if (auto* sa = pass->stencilAttachment()) {
      if (sa->texture()) sa->setLoadAction(MTL::LoadActionLoad);
    }
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (changed & (uint32_t(1) << (i + 1))) {
      pass->colorAttachments()->object(i)->setLoadAction(MTL::LoadActionLoad);
    }
  }
}

bool MetalRenderTargetCache::CanDrawDeferredClearInPass(
    uint32_t slot, MetalRenderTarget* rt,
    const TransferAttachmentFormats& formats) {
  if (!rt || !REXCVAR_GET(metal_transfer_in_draw_pass)) return false;
  RenderTargetKey key = rt->key();
  uint32_t samples = MsaaSamplesToCount(key.msaa_samples);
  if (slot == 0) {
    if (!key.is_depth) return false;
    MTL::PixelFormat fmt = GetDepthPixelFormat(key.GetDepthFormat());
    MTL::Texture* tex = rt->texture();
    if (!tex || tex != rt->draw_texture() || tex->pixelFormat() != fmt ||
        uint32_t(std::max<NS::UInteger>(1, tex->sampleCount())) != samples ||
        formats.depth_attachment_format != fmt) {
      return false;
    }
    if ((fmt == MTL::PixelFormatDepth32Float_Stencil8 ||
         fmt == MTL::PixelFormatDepth24Unorm_Stencil8) &&
        formats.stencil_attachment_format != fmt) {
      return false;
    }
    return GetTransferDepthClearState() &&
           GetOrCreateTransferClearPipeline(
               fmt, false, true, samples, 0, &formats.color_attachment_formats,
               formats.depth_attachment_format,
               formats.stencil_attachment_format);
  }
  // The clear fragment shader writes color(0) only.
  if (slot != 1 || key.is_depth) return false;
  bool is_uint = false;
  MTL::PixelFormat fmt =
      GetColorOwnershipTransferPixelFormat(key.GetColorFormat(), &is_uint);
  MTL::Texture* tex = rt->transfer_texture();
  if (is_uint || !tex || tex != rt->draw_texture() ||
      tex->pixelFormat() != fmt ||
      uint32_t(std::max<NS::UInteger>(1, tex->sampleCount())) != samples ||
      formats.color_attachment_formats[0] != fmt) {
    return false;
  }
  return GetTransferNoDepthStencilState() &&
         GetOrCreateTransferClearPipeline(
             fmt, false, false, samples, 0, &formats.color_attachment_formats,
             formats.depth_attachment_format,
             formats.stencil_attachment_format);
}

bool MetalRenderTargetCache::XferTraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("FABLE2_XFER_TRACE");
    return v && v[0] == '1';
  }();
  return enabled;
}

void MetalRenderTargetCache::NoteDrawPassTransferRoute(bool queued) {
  UploadTraceCounters& u = UploadTrace();
  if (queued) {
    ++u.xfer_q;
    return;
  }
  ++u.xfer_fb;
  if (XferTraceEnabled()) {
    ++xfer_reject_lines_[queue_reject_line_];
  }
}

void MetalRenderTargetCache::NoteOwnershipTransfers(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers) {
  UploadTraceCounters& u = UploadTrace();
  const bool histogram = XferTraceEnabled();
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt || render_target_transfers[i].empty()) {
      continue;
    }
    ++u.xfer_rt;
    const RenderTargetKey dest_key = dest_rt->key();
    for (const Transfer& transfer : render_target_transfers[i]) {
      ++u.xfer_n;
      if (transfer.host_depth_source) ++u.xfer_hostdepth;
      if (!transfer.source) continue;
      const RenderTargetKey source_key = transfer.source->key();
      if (source_key.is_depth != dest_key.is_depth) {
        ++u.xfer_cd;
      } else if (source_key.msaa_samples != dest_key.msaa_samples) {
        ++u.xfer_msaa;
      } else if (source_key.resource_format != dest_key.resource_format) {
        ++u.xfer_fmt;
      } else {
        ++u.xfer_same;
      }
      if (histogram) {
        ++xfer_pairs_[(uint64_t(source_key.key) << 32) | dest_key.key];
      }
      if (EffectsNativeTraceEnabled()) {
        const bool source_native = IsEffectsNativeEligible(source_key);
        const bool dest_native = IsEffectsNativeEligible(dest_key);
        if (source_native && dest_native) {
          ++u.eff_xfer_native;
        } else if (source_native != dest_native) {
          ++u.eff_xfer_mixed;
        }
      }
    }
  }
  if (!histogram) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - xfer_last_log_ < std::chrono::seconds(10) || xfer_pairs_.empty()) {
    return;
  }
  xfer_last_log_ = now;
  std::vector<std::pair<uint32_t, uint64_t>> sorted;
  for (const auto& it : xfer_pairs_) sorted.emplace_back(it.second, it.first);
  std::sort(sorted.rbegin(), sorted.rend());
  std::string line;
  for (size_t k = 0; k < sorted.size() && k < 24; ++k) {
    RenderTargetKey src, dst;
    src.key = uint32_t(sorted[k].second >> 32);
    dst.key = uint32_t(sorted[k].second);
    line += fmt::format(" | {} x [{}] -> [{}]", sorted[k].first,
                        src.GetDebugName(), dst.GetDebugName());
  }
  std::string rejects;
  for (const auto& it : xfer_reject_lines_) {
    rejects += fmt::format(" line{}={}", it.first, it.second);
  }
  REXLOG_INFO("[xfer-trace] last 10 s, separate-pass rejects:{}; top pairs:{}",
              rejects, line);
  xfer_pairs_.clear();
  xfer_reject_lines_.clear();
}

bool MetalRenderTargetCache::Resolve(rex::memory::Memory& memory, uint32_t& written_address,
                                     uint32_t& written_length,
                                     MTL::CommandBuffer* command_buffer,
                                     const ResolvePlan* prepared_resolve_plan) {
  written_address = 0;
  written_length = 0;
  // [ssm-merge] (TileResolvePrecheck refuses while work is queued, so no
  // tile-resolve pass is open here when this encodes.)
  (void)FlushQueuedStencilAlias();
  ResolvePlan resolve_plan_storage;
  if (!prepared_resolve_plan) {
    if (!PrepareResolvePlan(memory, resolve_plan_storage)) {
      return false;
    }
    prepared_resolve_plan = &resolve_plan_storage;
  }
  const ResolvePlan& resolve_plan = *prepared_resolve_plan;
  const draw_util::ResolveInfo& resolve_info = resolve_plan.resolve_info;

  // Nothing to do.
  if (resolve_plan.noop) {
    return true;
  }
  // [tile-resolve] the draw pass was kept open (IssueCopy): end it now unless
  // nothing before the copy needs a boundary (growth, clears, verify reads).
  if (tile_resolve_open_ && !TileResolvePrecheck(resolve_plan)) {
    TileResolveEnd("resolve");
  }
  // [rt-shrink] claims made since the last growth point (none expected).
  if (!RtShrinkApplyGrowth()) {
    REXLOG_ERROR("[rt-shrink] growth failed before a resolve");
  }

  bool is_depth = resolve_info.IsCopyingDepth();

  // [effects-native] a resolve of native-scale effect targets is done exactly
  // as at 1x: unscaled copy constants (from PrepareResolvePlan), pipelines,
  // EDRAM dump layout and destination (shared memory).
  struct ResolveScaleScope {
    MetalRenderTargetCache& cache;
    ResolveScaleScope(MetalRenderTargetCache& c, bool native) : cache(c) {
      cache.resolve_scale_x_ = native ? 1u : c.draw_resolution_scale_x();
      cache.resolve_scale_y_ = native ? 1u : c.draw_resolution_scale_y();
    }
    ~ResolveScaleScope() {
      cache.resolve_scale_x_ = cache.draw_resolution_scale_x();
      cache.resolve_scale_y_ = cache.draw_resolution_scale_y();
    }
  } resolve_scale_scope(*this, resolve_plan.native);
  bool draw_resolution_scaled = IsResolveScaled();

  const auto& coord = resolve_info.coordinate_info;
  uint32_t resolve_width = coord.width_div_8 * 8;
  uint32_t resolve_height = resolve_info.height_div_8 * 8;

  // Try GPU compute resolve first (RT -> EDRAM -> shared memory), matching
  // D3D12/Vulkan behavior for the supported cases.
  if (edram_buffer_) {
    // Copy dispatch -- only when there is an actual copy extent.
    bool copy_succeeded = !resolve_plan.needs_copy_export;
    if (resolve_plan.needs_copy_export) {
      // [deferred-clears] the copy reads the render targets that own its
      // EDRAM tiles (the same GetResolveCopyRectanglesToDump list the dump and
      // direct-host paths read): encode their deferred clears first.
      if (!deferred_clears_.empty() || pending_draw_pass_clear_mask_) {
        uint32_t span_base, span_row_length, span_rows, span_pitch;
        resolve_info.GetCopyEdramTileSpan(span_base, span_row_length,
                                          span_rows, span_pitch);
        std::vector<ResolveCopyDumpRectangle> owner_rects;
        GetResolveCopyRectanglesToDump(span_base, span_row_length, span_rows,
                                       span_pitch, owner_rects);
        for (const ResolveCopyDumpRectangle& rect : owner_rects) {
          if (!FlushDeferredClearsFor(rect.render_target,
                                      DeferredClearFlushReason::kResolve)) {
            return false;
          }
        }
      }
      if (RtShrinkVerify()) {
        // [rt-shrink] the dump / direct resolve reads these owners' rows.
        uint32_t span_base, span_row_length, span_rows, span_pitch;
        resolve_info.GetCopyEdramTileSpan(span_base, span_row_length,
                                          span_rows, span_pitch);
        std::vector<ResolveCopyDumpRectangle> owner_rects;
        GetResolveCopyRectanglesToDump(span_base, span_row_length, span_rows,
                                       span_pitch, owner_rects);
        for (const ResolveCopyDumpRectangle& rect : owner_rects) {
          const uint32_t first =
              span_base + rect.row_first * span_pitch + rect.row_first_start;
          const uint32_t last =
              span_base + (rect.row_first + rect.rows - 1) * span_pitch +
              rect.row_last_end;
          RtShrinkCheckTiles(static_cast<MetalRenderTarget*>(rect.render_target),
                             first, last, "resolve source");
        }
      }
      draw_util::ResolveCopyShaderConstants copy_constants;
      uint32_t group_count_x = 0, group_count_y = 0;
      draw_util::ResolveCopyShaderIndex copy_shader =
          resolve_info.GetCopyShader(resolve_scale_x_, resolve_scale_y_,
                                     copy_constants,
                                     group_count_x, group_count_y);
      bool direct_host_resolve_enabled = REXCVAR_GET(metal_direct_host_resolve);
      bool direct_host_rt_candidate =
          direct_host_resolve_enabled &&
          IsResolveDirectHostRTCandidate(copy_shader);
      const draw_util::ResolveCopyShaderInfo& copy_shader_info =
          draw_util::resolve_copy_shader_info[size_t(copy_shader)];

      // Match D3D12/Vulkan: dump host RT ownership into EDRAM, then resolve
      // from EDRAM to shared memory. Resolve-time blend fallback is not correct
      // because blending state is per-draw, not per-resolve.
      uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
      resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used,
                                        dump_rows, dump_pitch);
      if (direct_host_resolve_enabled &&
          TryDirectHostResolveCopy(resolve_info, copy_constants, copy_shader,
                                   dump_base, dump_row_length_used, dump_rows,
                                   dump_pitch, command_buffer, written_address,
                                   written_length)) {
        copy_succeeded = true;
      } else {
        TileResolveEnd("resolve");
        DumpRenderTargets(dump_base, dump_row_length_used, dump_rows,
                          dump_pitch, command_buffer,
                          ResolveDumpEncoderLabel(direct_host_rt_candidate));

        uint32_t dest_base = resolve_info.copy_dest_base;
        uint32_t dest_local_start =
            resolve_info.copy_dest_extent_start - dest_base;
        uint32_t dest_local_end =
            dest_local_start + resolve_info.copy_dest_extent_length;

        command_processor_.SetSwapDestSwap(
            dest_base, resolve_info.copy_dest_info.copy_dest_swap);

        // For now, only apply the 8888 restriction to color resolves; depth
        // resolves may use different destination formats.
        uint32_t bytes_per_pixel = 4;

        // Select the appropriate Metal pipeline for this shader.
        MTL::ComputePipelineState* pipeline =
            GetResolvePipeline(copy_shader, draw_resolution_scaled);
        if (draw_resolution_scaled && !pipeline) {
          static uint32_t missing_scaled_pipeline_log_count = 0;
          if (missing_scaled_pipeline_log_count < 8) {
            ++missing_scaled_pipeline_log_count;
            REXLOG_WARN(
                "MetalResolve: scaled resolve pipeline missing for shader {}",
                int(copy_shader));
          }
        }

        if (pipeline && group_count_x && group_count_y) {
          ResolveDestinationBuffer destination = {};
          if (!PrepareResolveDestinationBuffer(
                  resolve_info, draw_resolution_scaled, destination)) {
            REXLOG_ERROR(
                "MetalRenderTargetCache::Resolve: failed to prepare resolve "
                "destination for 0x{:08X} len {}",
                resolve_info.copy_dest_extent_start,
                resolve_info.copy_dest_extent_length);
            return false;
          }

          {
            ScopedAutoreleasePool autorelease_pool;
            bool standalone = false;
            MTL::CommandBuffer* cmd = command_buffer;
            if (!cmd) {
              cmd = command_processor_.CreateStandaloneTransferCommandBuffer(
                  "XeniaCB reason=resolve-compute");
              if (!cmd) {
                REXLOG_ERROR(
                    "MetalRenderTargetCache::Resolve: failed to get command "
                    "buffer for GPU path");
              }
              standalone = (cmd != nullptr);
            }
            if (cmd) {
              EndSharedMemoryUploadBlitEncoderForCommandBuffer(
                  command_processor_, cmd);
              MTL::ComputeCommandEncoder* encoder =
                  pass_trace::Compute(cmd, "rt_resolve");
              if (!encoder) {
                REXLOG_ERROR(
                    "MetalRenderTargetCache::Resolve: failed to get compute "
                    "encoder for GPU path");
                if (standalone) {
                  cmd->release();
                }
              } else {
                SetEncoderLabel(
                    encoder, ResolveCopyEncoderLabel(direct_host_rt_candidate));
                PushEncoderDebugGroup(
                    encoder,
                    fmt::format(
                        "{} shader=\"{}\" direct_host_rt_candidate={} "
                        "source=edram_buffer dest={}",
                        ResolveCopyEncoderLabel(direct_host_rt_candidate),
                        copy_shader_info.debug_name,
                        direct_host_rt_candidate ? 1 : 0,
                        draw_resolution_scaled ? "scaled_resolve_memory"
                                               : "shared_memory"));
                encoder->setComputePipelineState(pipeline);

                // Buffer 0: push constants
                if (draw_resolution_scaled) {
                  encoder->setBytes(&copy_constants.dest_relative,
                                    sizeof(copy_constants.dest_relative), 0);
                } else {
                  encoder->setBytes(&copy_constants, sizeof(copy_constants), 0);
                }

                // Buffer 1: destination memory (shared or scaled resolve).
                encoder->setBuffer(destination.buffer, destination.offset, 1);

                // Buffer 2: EDRAM source buffer.
                encoder->setBuffer(edram_buffer_, 0, 2);
                encoder->useResource(destination.buffer,
                                     MTL::ResourceUsageWrite);
                encoder->useResource(edram_buffer_, MTL::ResourceUsageRead);

                encoder->dispatchThreadgroups(
                    MTL::Size::Make(group_count_x, group_count_y, 1),
                    MTL::Size::Make(8, 8, 1));

                if (!draw_resolution_scaled) {
                  command_processor_.MarkSharedMemoryComputeWritePending(
                      resolve_info.copy_dest_extent_start,
                      resolve_info.copy_dest_extent_length, encoder);
                  if (auto* shared_memory =
                          command_processor_.shared_memory()) {
                    shared_memory->MarkGpuAccess(
                        resolve_info.copy_dest_extent_start,
                        resolve_info.copy_dest_extent_length,
                        command_processor_.GetCurrentSubmission());
                  }
                }

                if (EncoderDebugLabelsEnabled()) {
                  encoder->popDebugGroup();
                }
                encoder->endEncoding();
                if (standalone) {
                  command_processor_.CommitStandaloneAndWait(cmd);
                }

                written_address = resolve_plan.written_address;
                written_length = resolve_plan.written_length;

                // Mark the range as resolved in the texture cache so that any
                // textures overlapping this range will be reloaded from the
                // updated shared memory. This matches D3D12/Vulkan behavior.
                if (auto* tex_cache = command_processor_.texture_cache()) {
                  if (draw_resolution_scaled || !IsDrawResolutionScaled()) {
                    static_cast<MetalTextureCache*>(tex_cache)
                        ->MarkRangeAsResolvedRtt(resolve_info, written_address,
                                                 written_length);
                  } else {
                    // [effects-native] unscaled data from a native target.
                    static_cast<MetalTextureCache*>(tex_cache)
                        ->MarkRangeAsResolvedUnscaledRtt(written_address,
                                                         written_length);
                  }
                }

                copy_succeeded = true;
              }
            }
          }
        }
      }

      if (!copy_succeeded) {
        REXLOG_ERROR(
            "MetalRenderTargetCache::Resolve: no valid GPU resolve shader / "
            "pipeline for this configuration");
      }
    }  // if (needs_copy_export)

    // [tile-resolve] the copy is encoded; the clear runs after the pass.
    TileResolveEnd("resolve");
    // Clearing -- runs independently of whether the copy succeeded, matching
    // D3D12/Vulkan behavior.
    bool clear_depth =
        resolve_plan.needs_resolve_clear && resolve_info.IsClearingDepth();
    bool clear_color =
        resolve_plan.needs_resolve_clear && resolve_info.IsClearingColor();
    bool clear_succeeded = !(clear_depth || clear_color);
    if (clear_depth || clear_color) {
      clear_succeeded = true;
      Transfer::Rectangle clear_rectangle;
      RenderTarget* clear_targets[2] = {};
      std::vector<Transfer> clear_transfers[2];
      rt_shrink_claim_context_ = "resolve clear";
      if (PrepareHostRenderTargetsResolveClear(
              resolve_info, clear_rectangle, clear_targets[0],
              clear_transfers[0], clear_targets[1], clear_transfers[1])) {
        // [rt-shrink] the clear claimed its range: size the targets first.
        if (!rt_shrink_pending_.empty()) {
          command_processor_.TileResolveEndKept("resolve:tile-grow");
        }
        if (!RtShrinkApplyGrowth()) {
          REXLOG_ERROR("[rt-shrink] growth failed before a resolve clear");
        }
        uint64_t clear_values[2];
        clear_values[0] = resolve_info.rb_depth_clear;
        clear_values[1] = resolve_info.rb_color_clear |
                          (uint64_t(resolve_info.rb_color_clear_lo) << 32);
        // [deferred-clears] DIAGNOSTICS (FABLE2_DEFERRED_CLEARS_DIAG):
        // skip: UNSAFE (wrong image), drops clear-only resolve clears to bound
        // the gain of deferring them. dup<N>: encodes each clear-only resolve
        // clear pass N extra times (same image, clears are idempotent) to
        // measure what these passes cost on this GPU (ms per GB slope).
        static const char* diag_env =
            std::getenv("FABLE2_DEFERRED_CLEARS_DIAG");
        static const bool diag_skip = [] {
          bool on = diag_env && std::strcmp(diag_env, "skip") == 0;
          if (on) {
            REXLOG_WARN(
                "[deferred-clears] DIAG skip: resolve clears are DROPPED "
                "(image wrong, measurement only)");
          }
          return on;
        }();
        static const uint32_t diag_dup = [] {
          uint32_t n = 0;
          if (diag_env && std::strncmp(diag_env, "dup", 3) == 0) {
            n = uint32_t(std::max(0, std::atoi(diag_env + 3)));
            REXLOG_WARN("[deferred-clears] DIAG dup: each clear pass x{}",
                        n + 1);
          }
          return n;
        }();
        bool defer = DeferredClearsEnabled() && !diag_dup;
        if (command_processor_.TileResolveKeptOpen()) {
          // [tile-resolve keep] anything below that encodes GPU work in
          // another pass needs the tile resolve's pass ended first.
          bool gpu_work = !defer || diag_skip || !clear_transfers[0].empty() ||
                          !clear_transfers[1].empty();
          for (const DeferredClear& dc : deferred_clears_) {
            for (uint32_t c = 0; c < 2; ++c) {
              if (dc.render_target == clear_targets[c]) gpu_work = true;
              for (const Transfer& t : clear_transfers[c]) {
                if (dc.render_target == t.source ||
                    dc.render_target == t.host_depth_source) {
                  gpu_work = true;
                }
              }
            }
          }
          if (gpu_work) {
            command_processor_.TileResolveEndKept("resolve:tile");
          }
        }
        if (defer || !deferred_clears_.empty()) {
          // [deferred-clears] an older deferred clear of the same target, or
          // of a source of this clear's transfers, must land first.
          for (uint32_t c = 0; c < 2; ++c) {
            if (!clear_targets[c]) continue;
            FlushDeferredClearsFor(clear_targets[c],
                                   DeferredClearFlushReason::kReclear);
            for (const Transfer& t : clear_transfers[c]) {
              FlushDeferredClearsFor(t.source,
                                     DeferredClearFlushReason::kTransfer);
              FlushDeferredClearsFor(t.host_depth_source,
                                     DeferredClearFlushReason::kTransfer);
            }
          }
        }
        if (defer && !diag_skip && clear_transfers[0].empty() &&
            clear_transfers[1].empty()) {
          // [deferred-clears] ownership already changed; queue the GPU work.
          // Same order as PerformTransfersAndResolveClears: depth, colour.
          for (uint32_t c = 0; c < 2; ++c) {
            auto* rt = static_cast<MetalRenderTarget*>(clear_targets[c]);
            if (!rt) continue;
            if (rt->needs_initial_clear()) {
              rt->SetNeedsInitialClear(false);
              MarkRenderPassDescriptorDirty();
            }
            // [tile-resolve keep] copy and clear in the same pass: the clear
            // quad follows the tile function in each tile, as the separate
            // clear pass followed the copy.
            Transfer::Rectangle remainder = {};
            bool has_remainder = false;
            if (command_processor_.TileResolveKeptOpen() &&
                TileResolveClearInPassEnabled() &&
                TileResolveDrawClearInPass(rt, c == 0, clear_values[c],
                                           clear_rectangle, remainder,
                                           has_remainder)) {
              ++UploadTrace().tres_clear_inpass;
              if (!has_remainder) continue;
            }
            DeferredClear clear;
            clear.render_target = rt;
            clear.clear_value = clear_values[c];
            clear.rectangle = has_remainder ? remainder : clear_rectangle;
            deferred_clears_.push_back(clear);
            ++UploadTrace().dc_deferred;
          }
        } else if (diag_skip && clear_transfers[0].empty() &&
            clear_transfers[1].empty()) {
          for (RenderTarget* rt : clear_targets) {
            if (rt) {
              static_cast<MetalRenderTarget*>(rt)->SetNeedsInitialClear(false);
            }
          }
          MarkRenderPassDescriptorDirty();
        } else {
          PerformTransfersAndResolveClears(2, clear_targets, clear_transfers,
                                           clear_values, &clear_rectangle,
                                           command_buffer);
          if (diag_dup && clear_transfers[0].empty() &&
              clear_transfers[1].empty()) {
            for (uint32_t d = 0; d < diag_dup; ++d) {
              PerformTransfersAndResolveClears(2, clear_targets,
                                               clear_transfers, clear_values,
                                               &clear_rectangle,
                                               command_buffer);
            }
          }
        }
      }
    }

    return copy_succeeded && clear_succeeded;
  }

  REXLOG_ERROR(
      "MetalRenderTargetCache::Resolve: no valid GPU resolve shader / pipeline "
      "for this configuration");
  return false;
}

bool MetalRenderTargetCache::EncodeSelfHostDepthStore(
    MTL::CommandBuffer* cmd, RenderTarget* dest_rt,
    const std::vector<Transfer>& transfers,
    const std::vector<TransferRectanglePlan>& transfer_rectangles) {
  if (!cmd || !dest_rt || !edram_buffer_) {
    return false;
  }
  RenderTargetKey dest_key = dest_rt->key();
  if (!dest_key.is_depth) {
    return false;
  }
  auto* dest_metal_rt = static_cast<MetalRenderTarget*>(dest_rt);
  MTL::Texture* depth_texture = dest_metal_rt->texture();
  if (!depth_texture) {
    return false;
  }
  size_t pipeline_index = size_t(dest_key.msaa_samples);
  if (pipeline_index >= rex::countof(host_depth_store_pipelines_) ||
      !host_depth_store_pipelines_[pipeline_index]) {
    REXLOG_ERROR(
        "MetalRenderTargetCache: missing host depth store pipeline for "
        "msaa={}",
        uint32_t(dest_key.msaa_samples));
    return false;
  }
  HostDepthStoreRenderTargetConstant render_target_constant =
      GetHostDepthStoreRenderTargetConstant(dest_key.pitch_tiles_at_32bpp,
                                            msaa_2x_supported_);
  MTL::ComputeCommandEncoder* depth_store_encoder = nullptr;
  for (const TransferRectanglePlan& transfer_plan : transfer_rectangles) {
    if (transfer_plan.transfer_index >= transfers.size()) {
      continue;
    }
    const Transfer& transfer = transfers[transfer_plan.transfer_index];
    if (transfer.host_depth_source != dest_rt || !transfer_plan.rectangle_count) {
      continue;
    }
    if (!depth_store_encoder) {
      EndSharedMemoryUploadBlitEncoderForCommandBuffer(command_processor_, cmd);
      depth_store_encoder = pass_trace::Compute(cmd, "rt_depth_store");
      if (!depth_store_encoder) {
        REXLOG_ERROR(
            "MetalRenderTargetCache: failed to create host depth store "
            "encoder");
        return false;
      }
      SetEncoderLabel(depth_store_encoder, "XeniaHostDepthStoreEncoder");
      depth_store_encoder->setComputePipelineState(
          host_depth_store_pipelines_[pipeline_index]);
      depth_store_encoder->setBuffer(edram_buffer_, 0, 1);
      depth_store_encoder->setTexture(depth_texture, 0);
      depth_store_encoder->useResource(edram_buffer_, MTL::ResourceUsageWrite);
      depth_store_encoder->useResource(depth_texture, MTL::ResourceUsageRead);
    }
    for (uint32_t rect_index = 0; rect_index < transfer_plan.rectangle_count;
         ++rect_index) {
      uint32_t group_count_x = 0;
      uint32_t group_count_y = 0;
      HostDepthStoreRectangleConstant rectangle_constant;
      GetHostDepthStoreRectangleInfo(transfer_plan.rectangles[rect_index],
                                     dest_key.msaa_samples, rectangle_constant,
                                     group_count_x, group_count_y);
      if (!group_count_x || !group_count_y) {
        continue;
      }
      HostDepthStoreConstants constants = {};
      constants.rectangle = rectangle_constant;
      constants.render_target = render_target_constant;
      depth_store_encoder->setBytes(&constants, sizeof(constants), 0);
      depth_store_encoder->dispatchThreadgroups(
          MTL::Size::Make(group_count_x, group_count_y, 1),
          MTL::Size::Make(8, 8, 1));
    }
  }
  if (depth_store_encoder) {
    depth_store_encoder->endEncoding();
  }
  return true;
}

bool MetalRenderTargetCache::ClearValueAsLoadAction(
    const RenderTargetKey& key, uint64_t clear_value, MTL::ClearColor* color,
    double* depth, uint32_t* stencil) const {
  // Shared by the transfer pass's whole-target clears and the in-pass ones
  // (FABLE2_CLEAR_LOAD_ACTION), so both convert the guest value the same way.
  bool ok = false;
  if (key.is_depth) {
    uint32_t depth_guest_clear_value =
        (uint32_t(clear_value) >> 8) & 0xFFFFFF;
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8:
        *depth = xenos::UNorm24To32(depth_guest_clear_value);
        ok = true;
        break;
      case xenos::DepthRenderTargetFormat::kD24FS8:
        *depth =
            xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
        ok = true;
        break;
    }
    *stencil = uint32_t(clear_value) & 0xFF;
  } else {
    TransferClearColorFloatConstants float_constants = {};
    bool clear_via_drawing = false;
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
        for (uint32_t j = 0; j < 4; ++j) {
          float_constants.color[j] =
              ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
        for (uint32_t j = 0; j < 4; ++j) {
          float_constants.color[j] =
              ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
        }
        if (gamma_render_target_as_unorm16_) {
          for (uint32_t j = 0; j < 3; ++j) {
            float_constants.color[j] =
                xenos::PWLGammaToLinear(float_constants.color[j]);
          }
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
        for (uint32_t j = 0; j < 3; ++j) {
          float_constants.color[j] =
              ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
        }
        float_constants.color[3] =
            ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::
          k_2_10_10_10_FLOAT_AS_16_16_16_16: {
        for (uint32_t j = 0; j < 3; ++j) {
          float_constants.color[j] =
              xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
        }
        float_constants.color[3] =
            ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
        for (uint32_t j = 0; j < 2; ++j) {
          float_constants.color[j] =
              float((clear_value >> (j * 16)) & 0xFFFF);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
        for (uint32_t j = 0; j < 4; ++j) {
          float_constants.color[j] =
              float((clear_value >> (j * 16)) & 0xFFFF);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
        float_constants.color[0] = float(uint32_t(clear_value));
        if (uint64_t(float_constants.color[0]) != uint32_t(clear_value)) {
          clear_via_drawing = true;
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
        float_constants.color[0] = float(uint32_t(clear_value));
        float_constants.color[1] = float(uint32_t(clear_value >> 32));
        if (uint64_t(float_constants.color[0]) != uint32_t(clear_value) ||
            uint64_t(float_constants.color[1]) !=
                uint32_t(clear_value >> 32)) {
          clear_via_drawing = true;
        }
      } break;
    }

    bool clear_is_uint = false;
    GetColorOwnershipTransferPixelFormat(key.GetColorFormat(),
                                         &clear_is_uint);
    if (!clear_is_uint && !clear_via_drawing) {
      *color = MTL::ClearColor(
          float_constants.color[0], float_constants.color[1],
          float_constants.color[2], float_constants.color[3]);
      ok = true;
    }
  }
  return ok;
}

bool MetalRenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle,
    MTL::CommandBuffer* command_buffer,
    MTL::RenderCommandEncoder* active_render_encoder,
    MTL::RenderPassDescriptor* active_render_pass_descriptor,
    DrawPassTransferEncoderMutationMask* mutations_out) {
  if (mutations_out) {
    *mutations_out = kDrawPassTransferEncoderMutationNone;
  }
  if (!render_targets || !render_target_transfers) {
    return false;
  }
  // [native-scale] every caller passes its own mutable transfer lists.
  for (uint32_t i = 0; i < render_target_count; ++i) {
    DropMixedScaleHostDepth(
        render_targets[i],
        const_cast<std::vector<Transfer>&>(render_target_transfers[i]));
  }

  bool resolve_clear_needed =
      render_target_resolve_clear_values && resolve_clear_rectangle;
  bool use_active_render_encoder = active_render_encoder != nullptr;
  // A resolve clear inside an active draw pass comes only from
  // [deferred-clears] (EncodePendingDrawPassTransfers, no transfers).
  if (use_active_render_encoder && !active_render_pass_descriptor) {
    return false;
  }
  auto mark_active_encoder_mutation =
      [&](DrawPassTransferEncoderMutationMask mutations) {
        if (use_active_render_encoder && mutations_out) {
          *mutations_out |= mutations;
        }
      };
  TransferAttachmentFormats active_attachment_formats;
  if (use_active_render_encoder &&
      !GetActiveTransferAttachmentFormats(active_render_pass_descriptor,
                                          active_attachment_formats)) {
    return false;
  }
  // [edram-transfer] classify the ownership transfers (tr_n_xfer_*).
  NoteOwnershipTransfers(render_target_count, render_targets,
                         render_target_transfers);
  if (RtShrinkVerify()) {
    // [rt-shrink] every row these transfers and clears address must exist.
    for (uint32_t i = 0; i < render_target_count; ++i) {
      auto* dest = static_cast<MetalRenderTarget*>(render_targets[i]);
      if (!dest) continue;
      for (const Transfer& t : render_target_transfers[i]) {
        RtShrinkCheckTransfer(dest, t, "transfer dest");
        RtShrinkCheckTransfer(static_cast<MetalRenderTarget*>(t.source), t,
                              "transfer source");
        RtShrinkCheckTransfer(
            static_cast<MetalRenderTarget*>(t.host_depth_source), t,
            "transfer host depth");
      }
      if (resolve_clear_needed) {
        RtShrinkCheckRows(dest,
                          (resolve_clear_rectangle->y_pixels +
                           resolve_clear_rectangle->height_pixels) *
                              RenderTargetScaleY(dest->key()),
                          "resolve clear");
      }
    }
  }
  bool any_work = false;
  bool host_depth_store_needed = false;
  std::array<std::vector<TransferRectanglePlan>,
             1 + xenos::kMaxColorRenderTargets>
      local_transfer_rectangle_plans;
  std::array<const std::vector<TransferRectanglePlan>*,
             1 + xenos::kMaxColorRenderTargets>
      transfer_rectangle_plans = {};
  if (render_target_count > transfer_rectangle_plans.size()) {
    return false;
  }
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    if (resolve_clear_needed) {
      any_work = true;
    }
    const std::vector<Transfer>& transfers = render_target_transfers[i];
    if (transfers.empty()) {
      continue;
    }
    RenderTargetKey dest_key = dest_rt->key();
    std::vector<TransferRectanglePlan>& local_transfer_rectangles =
        local_transfer_rectangle_plans[i];
    if (!BuildTransferRectanglePlans(dest_key, transfers,
                                     resolve_clear_rectangle, false,
                                     local_transfer_rectangles)) {
      continue;
    }
    transfer_rectangle_plans[i] = &local_transfer_rectangles;
    if (local_transfer_rectangles.empty()) {
      continue;
    }
    for (const TransferRectanglePlan& transfer_plan :
         local_transfer_rectangles) {
      const Transfer& transfer = transfers[transfer_plan.transfer_index];
      if (dest_key.is_depth && transfer.host_depth_source == dest_rt) {
        host_depth_store_needed = true;
      }
    }
    any_work = true;
  }
  if (!any_work) {
    return true;
  }
  if (use_active_render_encoder && host_depth_store_needed &&
      !pending_draw_pass_self_host_depth_stored_) {
    return false;
  }

  MTL::CommandBuffer* cmd = command_buffer;
  if (use_active_render_encoder) {
    cmd = command_processor_.GetCurrentCommandBuffer();
  } else if (!cmd) {
    // RequestTransferCommandBuffer ends any active render encoder and ensures
    // transfer work has a command buffer. It does not require a standalone
    // command-buffer submission if the current one can be reused.
    cmd = command_processor_.RequestTransferCommandBuffer(
        MetalCommandProcessor::TransferRequestSource::kRenderTargetTransfer);
  } else {
    // An externally-provided command buffer still requires the render
    // encoder to be ended before transfer work can proceed.
    command_processor_.EndRenderEncoder();
  }
  if (!cmd) {
    REXLOG_ERROR(
        "MetalRenderTargetCache::PerformTransfersAndResolveClears: no command "
        "buffer");
    return false;
  }

  uint32_t scale_x = draw_resolution_scale_x();
  uint32_t scale_y = draw_resolution_scale_y();
  uint32_t tile_width_samples =
      xenos::kEdramTileWidthSamples * draw_resolution_scale_x();
  uint32_t tile_height_samples =
      xenos::kEdramTileHeightSamples * draw_resolution_scale_y();
  uint32_t depth_round = (!REXCVAR_GET(depth_float24_convert_in_pixel_shader) &&
                          REXCVAR_GET(depth_float24_round))
                             ? 1u
                             : 0u;

  // Host depth store pass (dest depth where host depth source == dest).
  // Skipped inside the draw pass: [tile-edram] K1 encoded it at queue time.
  if (host_depth_store_needed && !use_active_render_encoder) {
    for (uint32_t i = 0; i < render_target_count; ++i) {
      RenderTarget* dest_rt = render_targets[i];
      if (!dest_rt || !dest_rt->key().is_depth || !transfer_rectangle_plans[i]) {
        continue;
      }
      EncodeSelfHostDepthStore(cmd, dest_rt, render_target_transfers[i],
                               *transfer_rectangle_plans[i]);
      break;
    }
  }

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& transfers = render_target_transfers[i];
    const std::vector<TransferRectanglePlan>* target_transfer_rectangles =
        transfer_rectangle_plans[i];
    if ((!target_transfer_rectangles || target_transfer_rectangles->empty()) &&
        !resolve_clear_needed) {
      continue;
    }

    auto* dest_metal_rt = static_cast<MetalRenderTarget*>(dest_rt);
    if (dest_metal_rt->needs_initial_clear()) {
      dest_metal_rt->SetNeedsInitialClear(false);
      MarkRenderPassDescriptorDirty();
    }
    RenderTargetKey dest_key = dest_metal_rt->key();
    bool dest_is_depth = dest_key.is_depth;
    uint32_t active_color_attachment_index = 0;

    bool dest_is_uint = false;
    MTL::PixelFormat dest_pixel_format =
        dest_is_depth ? GetDepthPixelFormat(dest_key.GetDepthFormat())
                      : GetColorOwnershipTransferPixelFormat(
                            dest_key.GetColorFormat(), &dest_is_uint);

    MTL::Texture* dest_texture = dest_is_depth
                                     ? dest_metal_rt->texture()
                                     : dest_metal_rt->transfer_texture();
    if (!dest_texture) {
      REXLOG_WARN(
          "MetalRenderTargetCache::PerformTransfersAndResolveClears: "
          "Destination RT {} has no texture",
          i);
      continue;
    }
    if (dest_is_depth) {
      assert_true(dest_texture->pixelFormat() == dest_pixel_format,
                  "Transfer depth must use resource pixel format");
    } else {
      assert_true(dest_texture->pixelFormat() == dest_pixel_format,
                  "Transfer color must use ownership pixel format");
    }
    if (use_active_render_encoder) {
      if (dest_is_depth) {
        if (i != 0 || active_attachment_formats.depth_attachment_format !=
                          dest_pixel_format) {
          return false;
        }
        auto* depth_attachment =
            active_render_pass_descriptor->depthAttachment();
        MTL::Texture* depth_texture =
            depth_attachment ? depth_attachment->texture() : nullptr;
        if (depth_texture != dest_metal_rt->draw_texture()) {
          return false;
        }
        if (dest_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
            dest_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
          auto* stencil_attachment =
              active_render_pass_descriptor->stencilAttachment();
          MTL::Texture* stencil_texture =
              stencil_attachment ? stencil_attachment->texture() : nullptr;
          if (stencil_texture != depth_texture ||
              active_attachment_formats.stencil_attachment_format !=
                  dest_pixel_format) {
            return false;
          }
        }
      } else {
        if (i == 0 || i > xenos::kMaxColorRenderTargets) {
          return false;
        }
        active_color_attachment_index = i - 1;
        if (active_attachment_formats
                .color_attachment_formats[active_color_attachment_index] !=
            dest_pixel_format) {
          return false;
        }
      }
    }

    uint32_t dest_sample_count = MsaaSamplesToCount(dest_key.msaa_samples);
    bool transfer_use_sample_id_default =
        dest_sample_count > 1 && REXCVAR_GET(metal_transfer_msaa_sample_id);
    uint32_t dest_width = uint32_t(dest_texture->width());
    uint32_t dest_height = uint32_t(dest_texture->height());

    auto get_scaled_rect = [&](const Transfer::Rectangle& rect,
                               uint32_t& scaled_x, uint32_t& scaled_y,
                               uint32_t& scaled_width,
                               uint32_t& scaled_height) -> bool {
      // [effects-native] in the destination's own scale.
      uint32_t rect_x = rect.x_pixels * RenderTargetScaleX(dest_key);
      uint32_t rect_y = rect.y_pixels * RenderTargetScaleY(dest_key);
      uint32_t rect_width = rect.width_pixels * RenderTargetScaleX(dest_key);
      uint32_t rect_height = rect.height_pixels * RenderTargetScaleY(dest_key);
      if (rect_x >= dest_width || rect_y >= dest_height) {
        return false;
      }
      rect_width = std::min(rect_width, dest_width - rect_x);
      rect_height = std::min(rect_height, dest_height - rect_y);
      if (!rect_width || !rect_height) {
        return false;
      }
      scaled_x = rect_x;
      scaled_y = rect_y;
      scaled_width = rect_width;
      scaled_height = rect_height;
      return true;
    };

    auto set_rect_viewport = [&](MTL::RenderCommandEncoder* encoder,
                                 const Transfer::Rectangle& rect) -> bool {
      uint32_t scaled_x = 0;
      uint32_t scaled_y = 0;
      uint32_t scaled_width = 0;
      uint32_t scaled_height = 0;
      if (!get_scaled_rect(rect, scaled_x, scaled_y, scaled_width,
                           scaled_height)) {
        return false;
      }
      MTL::Viewport vp;
      vp.originX = double(scaled_x);
      vp.originY = double(scaled_y);
      vp.width = double(scaled_width);
      vp.height = double(scaled_height);
      vp.znear = 0.0;
      vp.zfar = 1.0;
      encoder->setViewport(vp);
      mark_active_encoder_mutation(kDrawPassTransferEncoderMutationViewport);
      MTL::ScissorRect scissor;
      scissor.x = scaled_x;
      scissor.y = scaled_y;
      scissor.width = scaled_width;
      scissor.height = scaled_height;
      encoder->setScissorRect(scissor);
      mark_active_encoder_mutation(kDrawPassTransferEncoderMutationScissor);
      return true;
    };

    std::vector<uint32_t> all_transfer_plan_indices;
    if (target_transfer_rectangles) {
      all_transfer_plan_indices.reserve(target_transfer_rectangles->size());
      for (uint32_t transfer_plan_index = 0;
           transfer_plan_index < target_transfer_rectangles->size();
           ++transfer_plan_index) {
        all_transfer_plan_indices.push_back(transfer_plan_index);
      }
    }
    std::vector<uint32_t> filtered_transfer_plan_indices;
    bool used_blit = false;
    MTL::BlitCommandEncoder* blit_encoder = nullptr;
    auto ensure_blit_encoder = [&]() -> MTL::BlitCommandEncoder* {
      if (!blit_encoder) {
        EndSharedMemoryUploadBlitEncoderForCommandBuffer(command_processor_,
                                                         cmd);
        blit_encoder = pass_trace::Blit(cmd, "rt_transfer_blit");
        if (blit_encoder) {
          SetEncoderLabel(blit_encoder, "XeniaRTTransferBlitEncoder");
        }
      }
      return blit_encoder;
    };

    // Fast path: when source/dest share compatible EDRAM layout and format,
    // use a blit instead of shader-based transfers.
    if (!use_active_render_encoder && target_transfer_rectangles &&
        !target_transfer_rectangles->empty()) {
      auto try_blit_transfer = [&](uint32_t transfer_plan_index) -> bool {
        const TransferRectanglePlan& transfer_plan =
            (*target_transfer_rectangles)[transfer_plan_index];
        const Transfer& transfer = transfers[transfer_plan.transfer_index];
        auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
        if (!source_rt || transfer.host_depth_source) {
          return false;
        }

        RenderTargetKey source_key = source_rt->key();
        if (dest_is_depth != source_key.is_depth) {
          return false;
        }
        if (source_key.resource_format != dest_key.resource_format ||
            source_key.msaa_samples != dest_key.msaa_samples ||
            source_key.pitch_tiles_at_32bpp != dest_key.pitch_tiles_at_32bpp) {
          return false;
        }

        bool base_tiles_match = source_key.base_tiles == dest_key.base_tiles;
        // Different-base color transfers need shader transfer address
        // semantics.
        if (!base_tiles_match) {
          return false;
        }

        MTL::Texture* source_texture = dest_is_depth
                                           ? source_rt->texture()
                                           : source_rt->transfer_texture();
        if (!source_texture) {
          return false;
        }
        if (!dest_is_depth) {
          MTL::PixelFormat expected_format =
              GetColorOwnershipTransferPixelFormat(source_key.GetColorFormat(),
                                                   nullptr);
          assert_true(source_texture->pixelFormat() == expected_format,
                      "Transfer source must use ownership pixel format");
        }
        if (source_texture->pixelFormat() != dest_texture->pixelFormat() ||
            source_texture->sampleCount() != dest_texture->sampleCount() ||
            source_texture->width() != dest_width ||
            source_texture->height() != dest_height) {
          return false;
        }

        if (!transfer_plan.rectangle_count) {
          return false;
        }

        MTL::BlitCommandEncoder* blit = ensure_blit_encoder();
        if (!blit) {
          return false;
        }

        for (uint32_t rect_index = 0;
             rect_index < transfer_plan.rectangle_count; ++rect_index) {
          uint32_t scaled_x = 0;
          uint32_t scaled_y = 0;
          uint32_t scaled_width = 0;
          uint32_t scaled_height = 0;
          if (!get_scaled_rect(transfer_plan.rectangles[rect_index], scaled_x,
                               scaled_y, scaled_width, scaled_height)) {
            continue;
          }
          MTL::Origin origin = MTL::Origin::Make(scaled_x, scaled_y, 0);
          MTL::Size size = MTL::Size::Make(scaled_width, scaled_height, 1);
          blit->copyFromTexture(source_texture, 0, 0, origin, size,
                                dest_texture, 0, 0, origin);
        }

        used_blit = true;
        return true;
      };

      for (uint32_t transfer_plan_index : all_transfer_plan_indices) {
        if (!try_blit_transfer(transfer_plan_index)) {
          filtered_transfer_plan_indices.push_back(transfer_plan_index);
        }
      }
    }

    if (blit_encoder) {
      blit_encoder->endEncoding();
    }

    const std::vector<uint32_t>& transfer_plan_indices_for_shaders =
        used_blit ? filtered_transfer_plan_indices : all_transfer_plan_indices;

    auto is_full_target_rectangle =
        [&](const Transfer::Rectangle& rect) -> bool {
      uint32_t scaled_x = 0;
      uint32_t scaled_y = 0;
      uint32_t scaled_width = 0;
      uint32_t scaled_height = 0;
      if (!get_scaled_rect(rect, scaled_x, scaled_y, scaled_width,
                           scaled_height)) {
        return false;
      }
      return !scaled_x && !scaled_y && scaled_width == dest_width &&
             scaled_height == dest_height;
    };

    auto transfers_fully_overwrite_target = [&]() -> bool {
      if (transfer_plan_indices_for_shaders.empty() ||
          !target_transfer_rectangles) {
        return false;
      }
      for (uint32_t transfer_plan_index : transfer_plan_indices_for_shaders) {
        const TransferRectanglePlan& transfer_plan =
            (*target_transfer_rectangles)[transfer_plan_index];
        if (transfer_plan.rectangle_count != 1 ||
            !is_full_target_rectangle(transfer_plan.rectangles[0])) {
          return false;
        }
      }
      return true;
    };

    // [main-depth] a stencil-only clear keeps the depth: never a load action
    // or DontCare load for it.
    const bool resolve_clear_stencil_only =
        resolve_clear_needed && dest_is_depth &&
        (render_target_resolve_clear_values[i] & kClearValueStencilOnly);
    bool resolve_clear_fully_overwrites_target = false;
    if (resolve_clear_needed && resolve_clear_rectangle &&
        !resolve_clear_stencil_only) {
      resolve_clear_fully_overwrites_target =
          is_full_target_rectangle(*resolve_clear_rectangle);
    }

    bool resolve_clear_via_load_action = false;
    MTL::ClearColor resolve_clear_color = MTL::ClearColor(0.0, 0.0, 0.0, 0.0);
    double resolve_clear_depth = 1.0;
    uint32_t resolve_clear_stencil = 0;
    if (resolve_clear_needed && resolve_clear_fully_overwrites_target) {
      resolve_clear_via_load_action = ClearValueAsLoadAction(
          dest_key, render_target_resolve_clear_values[i], &resolve_clear_color,
          &resolve_clear_depth, &resolve_clear_stencil);
    }
    if (resolve_clear_via_load_action &&
        transfer_plan_indices_for_shaders.empty()) {
      resolve_clear_via_load_action = false;
    }

    // Depth transfers that fully overwrite the destination still need a clean
    // stencil surface before the per-bit stencil draws run. A load-action
    // clear is cheaper than a separate clear draw in that case.
    bool transfer_stencil_clear_via_load_action =
        dest_is_depth && !transfer_plan_indices_for_shaders.empty() &&
        transfers_fully_overwrite_target() && !resolve_clear_via_load_action;
    if (transfer_stencil_clear_via_load_action) {
      resolve_clear_depth = 0.0;
      resolve_clear_stencil = 0;
    }

    // Prefer DontCare on transfer-pass loads only when destination contents are
    // provably fully overwritten by this pass.
    bool transfer_pass_load_dontcare = false;
    if (resolve_clear_fully_overwrites_target &&
        !resolve_clear_via_load_action) {
      transfer_pass_load_dontcare = true;
    }
    if (!transfer_pass_load_dontcare && !resolve_clear_needed &&
        !transfer_stencil_clear_via_load_action) {
      transfer_pass_load_dontcare = transfers_fully_overwrite_target();
    }
    MTL::LoadAction transfer_load_action = MTL::LoadActionLoad;
    if (resolve_clear_via_load_action ||
        transfer_stencil_clear_via_load_action) {
      transfer_load_action = MTL::LoadActionClear;
    } else if (transfer_pass_load_dontcare) {
      transfer_load_action = MTL::LoadActionDontCare;
    }

    // [tile-edram] separate-pass kind and touched-bytes estimate (tr_n_xfp_*).
    const char* transfer_pass_site = "rt_transfer";
    uint64_t transfer_pass_touched_pixels = 0;
    bool transfer_pass_clear_only = transfer_plan_indices_for_shaders.empty();
    bool transfer_pass_self_depth = false;
    if (transfer_pass_clear_only) {
      transfer_pass_site = "rt_transfer_clear";
      uint32_t sx = 0, sy = 0, sw = 0, sh = 0;
      if (resolve_clear_rectangle &&
          get_scaled_rect(*resolve_clear_rectangle, sx, sy, sw, sh)) {
        transfer_pass_touched_pixels = uint64_t(sw) * sh;
      }
    } else if (target_transfer_rectangles) {
      for (uint32_t transfer_plan_index : transfer_plan_indices_for_shaders) {
        const TransferRectanglePlan& plan =
            (*target_transfer_rectangles)[transfer_plan_index];
        if (dest_is_depth &&
            transfers[plan.transfer_index].host_depth_source == dest_rt) {
          transfer_pass_site = "rt_transfer_selfdepth";
          transfer_pass_self_depth = true;
        }
        for (uint32_t r = 0; r < plan.rectangle_count; ++r) {
          uint32_t sx = 0, sy = 0, sw = 0, sh = 0;
          if (get_scaled_rect(plan.rectangles[r], sx, sy, sw, sh)) {
            transfer_pass_touched_pixels += uint64_t(sw) * sh;
          }
        }
      }
    }
    MTL::RenderCommandEncoder* transfer_encoder = nullptr;
    auto ensure_transfer_encoder = [&]() -> MTL::RenderCommandEncoder* {
      if (transfer_encoder) {
        return transfer_encoder;
      }
      if (use_active_render_encoder) {
        transfer_encoder = active_render_encoder;
        return transfer_encoder;
      }
      MTL::RenderPassDescriptor* rp =
          MTL::RenderPassDescriptor::renderPassDescriptor();
      if (dest_is_depth) {
        auto* da = rp->depthAttachment();
        da->setTexture(dest_texture);
        da->setLoadAction(transfer_load_action);
        da->setStoreAction(MTL::StoreActionStore);
        if (transfer_load_action == MTL::LoadActionClear) {
          da->setClearDepth(resolve_clear_depth);
        }
        if (dest_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
            dest_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
          auto* sa = rp->stencilAttachment();
          sa->setTexture(dest_texture);
          sa->setLoadAction(transfer_load_action);
          sa->setStoreAction(MTL::StoreActionStore);
          if (transfer_load_action == MTL::LoadActionClear) {
            sa->setClearStencil(resolve_clear_stencil);
          }
        }
      } else {
        auto* ca = rp->colorAttachments()->object(0);
        ca->setTexture(dest_texture);
        ca->setLoadAction(transfer_load_action);
        ca->setStoreAction(MTL::StoreActionStore);
        if (transfer_load_action == MTL::LoadActionClear) {
          ca->setClearColor(resolve_clear_color);
        }
      }
      EndSharedMemoryUploadBlitEncoderForCommandBuffer(command_processor_, cmd);
      {
        UploadTraceCounters& u = UploadTrace();
        uint32_t bytes_per_sample = 4;
        switch (dest_pixel_format) {
          case MTL::PixelFormatDepth32Float_Stencil8:
            bytes_per_sample = 5;
            break;
          case MTL::PixelFormatRGBA16Float:
          case MTL::PixelFormatRGBA16Uint:
          case MTL::PixelFormatRGBA16Unorm:
          case MTL::PixelFormatRGBA16Snorm:
          case MTL::PixelFormatRG32Float:
          case MTL::PixelFormatRG32Uint:
            bytes_per_sample = 8;
            break;
          default:
            break;
        }
        uint64_t kb = transfer_pass_touched_pixels * dest_sample_count *
                      bytes_per_sample *
                      (transfer_load_action == MTL::LoadActionLoad ? 2 : 1) /
                      1024;
        u.xfp_kb += uint32_t(kb);
        if (transfer_pass_clear_only) {
          ++u.xfp_clear;
          u.xfp_clear_kb += uint32_t(kb);
        } else if (transfer_pass_self_depth) {
          ++u.xfp_self;
        } else {
          ++u.xfp_other;
        }
      }
      const bool pass_traced =
          bool(pass_trace::BeginRender(cmd, rp, transfer_pass_site));
      transfer_encoder = cmd->renderCommandEncoder(rp);
      if (pass_traced) pass_trace::ClearRender(rp);
      if (transfer_encoder) {
        SetEncoderLabel(transfer_encoder, "XeniaTransferEncoder");
      }
      return transfer_encoder;
    };

    if (!transfer_plan_indices_for_shaders.empty() &&
        target_transfer_rectangles) {
      bool need_stencil_bit_draws = dest_is_depth;
      bool stencil_clear_needed =
          need_stencil_bit_draws && !transfer_stencil_clear_via_load_action;

      transfer_invocations_.clear();
      transfer_invocations_.reserve(transfer_plan_indices_for_shaders.size() *
                                    (need_stencil_bit_draws ? 2 : 1));

      for (uint32_t transfer_plan_index : transfer_plan_indices_for_shaders) {
        const TransferRectanglePlan& transfer_plan =
            (*target_transfer_rectangles)[transfer_plan_index];
        const Transfer& transfer = transfers[transfer_plan.transfer_index];
        if (transfer.source) {
          auto* source = static_cast<MetalRenderTarget*>(transfer.source);
          source->SetTemporarySortIndex(UINT32_MAX);
        }
        if (transfer.host_depth_source) {
          auto* host_depth =
              static_cast<MetalRenderTarget*>(transfer.host_depth_source);
          host_depth->SetTemporarySortIndex(UINT32_MAX);
        }
      }

      uint32_t rt_sort_index = 0;
      auto ensure_sort_index = [&](MetalRenderTarget* rt) {
        if (rt && rt->temporary_sort_index() == UINT32_MAX) {
          rt->SetTemporarySortIndex(rt_sort_index++);
        }
      };

      for (uint32_t pass = 0; pass <= uint32_t(need_stencil_bit_draws);
           ++pass) {
        for (uint32_t transfer_plan_index : transfer_plan_indices_for_shaders) {
          const TransferRectanglePlan& transfer_plan =
              (*target_transfer_rectangles)[transfer_plan_index];
          const Transfer& transfer = transfers[transfer_plan.transfer_index];
          if (!transfer.source) {
            continue;
          }
          auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
          auto* host_depth_rt =
              pass
                  ? nullptr
                  : static_cast<MetalRenderTarget*>(transfer.host_depth_source);
          ensure_sort_index(source_rt);
          ensure_sort_index(host_depth_rt);

          RenderTargetKey source_key = source_rt->key();
          bool host_depth_is_copy = host_depth_rt == dest_metal_rt;
          RenderTargetKey host_depth_key;
          if (host_depth_rt) {
            host_depth_key = host_depth_rt->key();
          }
          TransferShaderKey shader_key = GetTransferShaderKey(
              source_key, dest_key, host_depth_rt ? &host_depth_key : nullptr,
              host_depth_is_copy, pass != 0, transfer_use_sample_id_default);

          transfer_invocations_.emplace_back(transfer, shader_key,
                                             &transfer_plan);
          if (pass) {
            transfer_invocations_.back().transfer.host_depth_source = nullptr;
          }
        }
      }

      std::sort(transfer_invocations_.begin(), transfer_invocations_.end());

      if (stencil_clear_needed) {
        MTL::RenderPipelineState* clear_pipeline =
            GetOrCreateTransferClearPipeline(
                dest_pixel_format, false, true, dest_sample_count, 0,
                use_active_render_encoder
                    ? &active_attachment_formats.color_attachment_formats
                    : nullptr,
                use_active_render_encoder
                    ? active_attachment_formats.depth_attachment_format
                    : MTL::PixelFormatInvalid,
                use_active_render_encoder
                    ? active_attachment_formats.stencil_attachment_format
                    : MTL::PixelFormatInvalid);
        MTL::DepthStencilState* stencil_clear_state =
            GetTransferStencilClearState();
        if (clear_pipeline && stencil_clear_state) {
          MTL::RenderCommandEncoder* encoder = ensure_transfer_encoder();
          if (encoder) {
            TransferClearDepthConstants constants = {};
            constants.depth = 0.0f;
            encoder->setRenderPipelineState(clear_pipeline);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationPipeline);
            encoder->setDepthStencilState(stencil_clear_state);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationDepthStencil);
            encoder->setStencilReferenceValue(0);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationStencilReference);
            encoder->setFragmentBytes(&constants, sizeof(constants), 0);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationFragmentSlot0);
            for (uint32_t transfer_plan_index :
                 transfer_plan_indices_for_shaders) {
              const TransferRectanglePlan& transfer_plan =
                  (*target_transfer_rectangles)[transfer_plan_index];
              for (uint32_t rect_index = 0;
                   rect_index < transfer_plan.rectangle_count; ++rect_index) {
                if (!set_rect_viewport(encoder,
                                       transfer_plan.rectangles[rect_index])) {
                  continue;
                }
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                        NS::UInteger(0), NS::UInteger(3));
              }
            }
          }
        }
      }
      MTL::RenderCommandEncoder* encoder = ensure_transfer_encoder();
      if (encoder) {
        bool transfer_viewport_full_set = false;
        MTL::ScissorRect last_transfer_scissor = {};
        bool last_transfer_scissor_valid = false;
        MTL::RenderPipelineState* last_transfer_pipeline = nullptr;
        MTL::DepthStencilState* last_transfer_depth_state = nullptr;
        MTL::Buffer* last_transfer_fragment_buffer_1 = nullptr;
        std::array<MTL::Texture*, 3> last_transfer_fragment_textures = {
            nullptr, nullptr, nullptr};
        bool last_transfer_stencil_reference_valid = false;
        uint32_t last_transfer_stencil_reference = 0;
        bool transfer_constants_valid = false;
        TransferShaderConstants last_transfer_constants = {};
        bool last_transfer_vertex_bytes_1_valid = false;
        TransferRectInstance last_transfer_vertex_bytes_1 = {};
        auto bind_transfer_pipeline = [&](MTL::RenderPipelineState* pipeline) {
          if (last_transfer_pipeline != pipeline) {
            encoder->setRenderPipelineState(pipeline);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationPipeline);
            last_transfer_pipeline = pipeline;
          }
        };
        auto bind_transfer_depth_state = [&](MTL::DepthStencilState* state) {
          if (last_transfer_depth_state != state) {
            encoder->setDepthStencilState(state);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationDepthStencil);
            last_transfer_depth_state = state;
          }
        };
        auto bind_transfer_fragment_texture = [&](uint32_t index,
                                                  MTL::Texture* texture) {
          if (index >= last_transfer_fragment_textures.size()) {
            return;
          }
          if (last_transfer_fragment_textures[index] != texture) {
            encoder->setFragmentTexture(texture, index);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationFragmentTextures);
            last_transfer_fragment_textures[index] = texture;
          }
        };
        auto bind_transfer_fragment_buffer_1 = [&](MTL::Buffer* buffer) {
          if (last_transfer_fragment_buffer_1 != buffer) {
            encoder->setFragmentBuffer(buffer, 0, 1);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationFragmentSlot1);
            last_transfer_fragment_buffer_1 = buffer;
          }
        };
        auto bind_transfer_stencil_reference = [&](uint32_t reference) {
          if (!last_transfer_stencil_reference_valid ||
              last_transfer_stencil_reference != reference) {
            encoder->setStencilReferenceValue(reference);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationStencilReference);
            last_transfer_stencil_reference = reference;
            last_transfer_stencil_reference_valid = true;
          }
        };
        auto bind_transfer_constants =
            [&](const TransferShaderConstants& constants) {
              if (!transfer_constants_valid ||
                  std::memcmp(&last_transfer_constants, &constants,
                              sizeof(constants)) != 0) {
                encoder->setVertexBytes(&constants, sizeof(constants), 0);
                encoder->setFragmentBytes(&constants, sizeof(constants), 0);
                mark_active_encoder_mutation(
                    kDrawPassTransferEncoderMutationVertexSlot0 |
                    kDrawPassTransferEncoderMutationFragmentSlot0);
                last_transfer_constants = constants;
                transfer_constants_valid = true;
              }
            };
        auto bind_transfer_scissor = [&](const MTL::ScissorRect& scissor) {
          if (!last_transfer_scissor_valid ||
              last_transfer_scissor.x != scissor.x ||
              last_transfer_scissor.y != scissor.y ||
              last_transfer_scissor.width != scissor.width ||
              last_transfer_scissor.height != scissor.height) {
            encoder->setScissorRect(scissor);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationScissor);
            last_transfer_scissor = scissor;
            last_transfer_scissor_valid = true;
          }
        };
        auto bind_transfer_vertex_bytes_1 =
            [&](const TransferRectInstance& rect_instance) {
              if (!last_transfer_vertex_bytes_1_valid ||
                  std::memcmp(&last_transfer_vertex_bytes_1, &rect_instance,
                              sizeof(rect_instance)) != 0) {
                encoder->setVertexBytes(&rect_instance, sizeof(rect_instance),
                                        1);
                mark_active_encoder_mutation(
                    kDrawPassTransferEncoderMutationVertexSlot1);
                last_transfer_vertex_bytes_1 = rect_instance;
                last_transfer_vertex_bytes_1_valid = true;
              }
            };
        auto bind_transfer_vertex_bytes_1_span =
            [&](const TransferRectInstance* rect_instances,
                uint32_t rect_instance_count) {
              if (!rect_instances || !rect_instance_count) {
                return;
              }
              encoder->setVertexBytes(
                  rect_instances,
                  size_t(rect_instance_count) * sizeof(TransferRectInstance),
                  1);
              mark_active_encoder_mutation(
                  kDrawPassTransferEncoderMutationVertexSlot1);
              last_transfer_vertex_bytes_1_valid = false;
            };
        auto set_full_transfer_viewport_scissor = [&]() {
          if (!transfer_viewport_full_set) {
            MTL::Viewport vp;
            vp.originX = 0.0;
            vp.originY = 0.0;
            vp.width = double(dest_width);
            vp.height = double(dest_height);
            vp.znear = 0.0;
            vp.zfar = 1.0;
            encoder->setViewport(vp);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationViewport);
            transfer_viewport_full_set = true;
          }
          MTL::ScissorRect scissor;
          scissor.x = 0;
          scissor.y = 0;
          scissor.width = dest_width;
          scissor.height = dest_height;
          bind_transfer_scissor(scissor);
        };

        std::vector<Transfer::Rectangle> merged_transfer_rectangles;
        for (size_t invocation_index = 0;
             invocation_index < transfer_invocations_.size();) {
          const auto& invocation = transfer_invocations_[invocation_index];
          size_t merged_invocation_end = invocation_index + 1;
          while (merged_invocation_end < transfer_invocations_.size() &&
                 invocation.CanBeMergedIntoOneDraw(
                     transfer_invocations_[merged_invocation_end])) {
            ++merged_invocation_end;
          }

          merged_transfer_rectangles.clear();
          merged_transfer_rectangles.reserve(
              (merged_invocation_end - invocation_index) *
              Transfer::kMaxRectanglesWithCutout);
          for (size_t merged_index = invocation_index;
               merged_index < merged_invocation_end; ++merged_index) {
            const TransferRectanglePlan* rectangle_plan =
                transfer_invocations_[merged_index].rectangle_plan;
            if (!rectangle_plan) {
              continue;
            }
            for (uint32_t rect_index = 0;
                 rect_index < rectangle_plan->rectangle_count; ++rect_index) {
              merged_transfer_rectangles.push_back(
                  rectangle_plan->rectangles[rect_index]);
            }
          }
          invocation_index = merged_invocation_end;
          if (merged_transfer_rectangles.empty()) {
            continue;
          }
          uint64_t merged_transfer_pixels = 0;
          for (const Transfer::Rectangle& rect : merged_transfer_rectangles) {
            uint32_t scaled_x = 0;
            uint32_t scaled_y = 0;
            uint32_t scaled_width = 0;
            uint32_t scaled_height = 0;
            if (get_scaled_rect(rect, scaled_x, scaled_y, scaled_width,
                                scaled_height)) {
              merged_transfer_pixels +=
                  uint64_t(scaled_width) * uint64_t(scaled_height);
            }
          }

          const Transfer& transfer = invocation.transfer;
          const TransferShaderKey& shader_key = invocation.shader_key;
          const TransferModeInfo& mode_info =
              kTransferModeInfos[size_t(shader_key.mode)];
          bool is_stencil_bit = mode_info.output == TransferOutput::kStencilBit;
          bool needs_source_depth =
              !mode_info.source_is_color &&
              mode_info.output != TransferOutput::kStencilBit;
          bool needs_source_stencil =
              !mode_info.source_is_color &&
              (mode_info.output == TransferOutput::kColor ||
               mode_info.output == TransferOutput::kStencilBit);

          auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
          if (!source_rt) {
            continue;
          }

          RenderTargetKey source_key = source_rt->key();
          bool source_is_uint = false;
          MTL::PixelFormat source_transfer_format = MTL::PixelFormatInvalid;
          if (mode_info.source_is_color) {
            source_transfer_format = GetColorOwnershipTransferPixelFormat(
                source_key.GetColorFormat(), &source_is_uint);
          }

          if (is_stencil_bit) {
            // Depth/stencil state set per-bit below.
          } else if (dest_is_depth) {
            bind_transfer_depth_state(GetTransferDepthStencilState(true));
          } else {
            MTL::DepthStencilState* no_depth_state =
                GetTransferNoDepthStencilState();
            if (!no_depth_state) {
              continue;
            }
            bind_transfer_depth_state(no_depth_state);
          }

          // Bind source textures.
          if (mode_info.source_is_color) {
            MTL::Texture* source_texture = source_rt->transfer_texture();
            {
              // [clear-const] the source's pending clear, as a texture.
              auto cc = clear_const_sources_.find(source_rt);
              if (cc != clear_const_sources_.end() && cc->second) {
                source_texture = cc->second;
              }
            }
            if (!source_texture) {
              continue;
            }
            assert_true(source_texture->pixelFormat() == source_transfer_format,
                        "Transfer source must use ownership pixel format");
            bind_transfer_fragment_texture(0, source_texture);
          } else {
            if (needs_source_depth) {
              MTL::Texture* depth_texture = source_rt->texture();
              if (!depth_texture) {
                continue;
              }
              bind_transfer_fragment_texture(0, depth_texture);
            }
            if (needs_source_stencil) {
              MTL::Texture* stencil_texture = nullptr;
              auto sc = stencil_const_sources_.find(source_rt);
              if (sc != stencil_const_sources_.end() && source_rt->texture()) {
                // [ssm-merge] the folded stencil-only clear's byte.
                stencil_texture = GetConstantStencilTexture(
                    uint32_t(source_rt->texture()->width()),
                    uint32_t(source_rt->texture()->height()), sc->second);
              } else {
                stencil_texture = GetStencilTextureView(source_rt);
              }
              if (!stencil_texture) {
                continue;
              }
              bind_transfer_fragment_texture(1, stencil_texture);
            }
          }

          // Bind host depth source if needed.
          if (mode_info.uses_host_depth) {
            uint32_t host_depth_index = mode_info.source_is_color ? 1 : 2;
            if (shader_key.host_depth_source_is_copy) {
              if (edram_buffer_) {
                bind_transfer_fragment_buffer_1(edram_buffer_);
              } else {
                MTL::Buffer* dummy = GetTransferDummyBuffer();
                if (dummy) {
                  bind_transfer_fragment_buffer_1(dummy);
                }
              }
              MTL::Texture* dummy_host_depth = GetTransferDummyDepthTexture(1);
              if (!dummy_host_depth) {
                continue;
              }
              bind_transfer_fragment_texture(host_depth_index,
                                             dummy_host_depth);
            } else {
              MTL::Buffer* dummy = GetTransferDummyBuffer();
              if (!dummy) {
                continue;
              }
              bind_transfer_fragment_buffer_1(dummy);
              auto* host_depth_rt =
                  static_cast<MetalRenderTarget*>(transfer.host_depth_source);
              MTL::Texture* host_depth_texture =
                  host_depth_rt ? host_depth_rt->texture() : nullptr;
              if (!host_depth_texture) {
                continue;
              }
              bind_transfer_fragment_texture(host_depth_index,
                                             host_depth_texture);
            }
          }

          TransferShaderConstants constants = {};
          constants.address.dest_pitch = dest_key.GetPitchTiles();
          constants.address.source_pitch = source_key.GetPitchTiles();
          constants.address.source_to_dest =
              int32_t(dest_key.base_tiles) - int32_t(source_key.base_tiles);
          constants.host_depth_address = {};
          if (mode_info.uses_host_depth &&
              !shader_key.host_depth_source_is_copy) {
            auto* host_depth_rt =
                static_cast<MetalRenderTarget*>(transfer.host_depth_source);
            if (host_depth_rt) {
              RenderTargetKey host_depth_key = host_depth_rt->key();
              constants.host_depth_address.dest_pitch =
                  dest_key.GetPitchTiles();
              constants.host_depth_address.source_pitch =
                  host_depth_key.GetPitchTiles();
              constants.host_depth_address.source_to_dest =
                  int32_t(dest_key.base_tiles) -
                  int32_t(host_depth_key.base_tiles);
            }
          }
          constants.source_format = source_key.resource_format;
          constants.dest_format = dest_key.resource_format;
          constants.source_is_depth = source_key.is_depth ? 1 : 0;
          constants.dest_is_depth = dest_key.is_depth ? 1 : 0;
          constants.source_is_uint = source_is_uint ? 1 : 0;
          constants.dest_is_uint = dest_is_uint ? 1 : 0;
          // Don't treat gamma-as-unorm16 as 64bpp for guest coordinate math.
          // It's 64bpp in storage, but represents a single pixel, not two
          // packed 32bpp halves.
          constants.source_is_64bpp = source_key.Is64bpp() ? 1 : 0;
          constants.dest_is_64bpp = dest_key.Is64bpp() ? 1 : 0;
          constants.source_msaa_samples =
              MsaaSamplesToCount(source_key.msaa_samples);
          constants.dest_msaa_samples =
              MsaaSamplesToCount(dest_key.msaa_samples);
          constants.host_depth_source_msaa_samples =
              MsaaSamplesToCount(shader_key.host_depth_source_msaa_samples);
          constants.host_depth_source_is_copy =
              shader_key.host_depth_source_is_copy ? 1 : 0;
          constants.depth_round = depth_round;
          constants.msaa_2x_supported = msaa_2x_supported_ ? 1 : 0;
          // [effects-native] tile sizes in the destination's scale (dest
          // pixels, host depth) and in the source's scale (source pixels).
          const uint32_t dest_tile_width_samples =
              xenos::kEdramTileWidthSamples * RenderTargetScaleX(dest_key);
          const uint32_t dest_tile_height_samples =
              xenos::kEdramTileHeightSamples * RenderTargetScaleY(dest_key);
          constants.tile_width_samples = dest_tile_width_samples;
          constants.tile_height_samples = dest_tile_height_samples;
          constants.source_tile_width_samples =
              xenos::kEdramTileWidthSamples * RenderTargetScaleX(source_key);
          constants.source_tile_height_samples =
              xenos::kEdramTileHeightSamples * RenderTargetScaleY(source_key);
          constants.source_scale_x = RenderTargetScaleX(source_key);
          constants.source_scale_y = RenderTargetScaleY(source_key);
          constants.dest_scale_x = RenderTargetScaleX(dest_key);
          constants.dest_scale_y = RenderTargetScaleY(dest_key);
          if (constants.source_scale_x != constants.dest_scale_x ||
              constants.source_scale_y != constants.dest_scale_y) {
            ++UploadTrace().eff_xfer_scaled;
          }
          uint32_t dest_tile_width_pixels =
              dest_tile_width_samples >>
              ((constants.dest_is_64bpp != 0u) +
               (constants.dest_msaa_samples >= 4u ? 1u : 0u));
          uint32_t dest_tile_height_pixels =
              dest_tile_height_samples >>
              (constants.dest_msaa_samples >= 2u ? 1u : 0u);
          constants.dest_tile_width_pixels = dest_tile_width_pixels;
          constants.dest_tile_height_pixels = dest_tile_height_pixels;
          constants.dest_tile_width_pixels_inv =
              dest_tile_width_pixels ? (1.0f / float(dest_tile_width_pixels))
                                     : 0.0f;
          constants.dest_tile_height_pixels_inv =
              dest_tile_height_pixels ? (1.0f / float(dest_tile_height_pixels))
                                      : 0.0f;
          uint32_t source_pitch_tiles = source_key.GetPitchTiles();
          constants.source_pitch_tiles_inv =
              source_pitch_tiles ? (1.0f / float(source_pitch_tiles)) : 0.0f;
          constants.host_depth_source_pitch_tiles_inv = 0.0f;
          if (mode_info.uses_host_depth &&
              !shader_key.host_depth_source_is_copy) {
            auto* host_depth_rt =
                static_cast<MetalRenderTarget*>(transfer.host_depth_source);
            if (host_depth_rt) {
              uint32_t host_pitch_tiles = host_depth_rt->key().GetPitchTiles();
              constants.host_depth_source_pitch_tiles_inv =
                  host_pitch_tiles ? (1.0f / float(host_pitch_tiles)) : 0.0f;
            }
          }
          constants.dest_pixel_to_ndc_x =
              dest_width ? (2.0f / float(dest_width)) : 0.0f;
          constants.dest_pixel_to_ndc_y =
              dest_height ? (2.0f / float(dest_height)) : 0.0f;
          constants.dest_sample_id = 0;

          const uint32_t rectangle_count =
              uint32_t(merged_transfer_rectangles.size());

          std::vector<TransferRectInstance> rect_instance_fallback;
          rect_instance_fallback.reserve(rectangle_count);
          for (uint32_t rect_index = 0; rect_index < rectangle_count;
               ++rect_index) {
            uint32_t scaled_x = 0;
            uint32_t scaled_y = 0;
            uint32_t scaled_width = 0;
            uint32_t scaled_height = 0;
            if (!get_scaled_rect(merged_transfer_rectangles[rect_index],
                                 scaled_x, scaled_y, scaled_width,
                                 scaled_height) ||
                !scaled_width || !scaled_height) {
              continue;
            }
            TransferRectInstance rect_instance = {};
            rect_instance.origin_x = float(scaled_x);
            rect_instance.origin_y = float(scaled_y);
            rect_instance.size_x = float(scaled_width);
            rect_instance.size_y = float(scaled_height);
            rect_instance_fallback.push_back(rect_instance);
          }

          MTL::DepthStencilState* native_stencil_output_state =
              is_stencil_bit && REXCVAR_GET(metal_transfer_native_stencil_output)
                  ? GetTransferStencilOutputState()
                  : nullptr;
          bool use_native_stencil_output =
              native_stencil_output_state != nullptr;
          MTL::RenderPipelineState* pipeline = GetOrCreateTransferPipelines(
              shader_key, dest_pixel_format, dest_is_uint,
              use_native_stencil_output, active_color_attachment_index,
              use_active_render_encoder
                  ? &active_attachment_formats.color_attachment_formats
                  : nullptr,
              use_active_render_encoder
                  ? active_attachment_formats.depth_attachment_format
                  : MTL::PixelFormatInvalid,
              use_active_render_encoder
                  ? active_attachment_formats.stencil_attachment_format
                  : MTL::PixelFormatInvalid);
          if (!pipeline && use_native_stencil_output) {
            use_native_stencil_output = false;
            native_stencil_output_state = nullptr;
            pipeline = GetOrCreateTransferPipelines(
                shader_key, dest_pixel_format, dest_is_uint, false,
                active_color_attachment_index,
                use_active_render_encoder
                    ? &active_attachment_formats.color_attachment_formats
                    : nullptr,
                use_active_render_encoder
                    ? active_attachment_formats.depth_attachment_format
                    : MTL::PixelFormatInvalid,
                use_active_render_encoder
                    ? active_attachment_formats.stencil_attachment_format
                    : MTL::PixelFormatInvalid);
          }
          if (!pipeline) {
            continue;
          }
          bind_transfer_pipeline(pipeline);

          bool use_sample_id_for_invocation =
              shader_key.dest_sample_id_from_sample != 0;
          auto draw_transfer_samples = [&](auto&& draw_fn) {
            if (use_sample_id_for_invocation || dest_sample_count <= 1) {
              draw_fn(0);
              return;
            }
            for (uint32_t sample_id = 0; sample_id < dest_sample_count;
                 ++sample_id) {
              draw_fn(sample_id);
            }
          };

          auto draw_transfer = [&](uint32_t sample_id) {
            constants.dest_sample_id = sample_id;
            bind_transfer_constants(constants);
            if (rect_instance_fallback.empty()) {
              return;
            }
            set_full_transfer_viewport_scissor();
            constexpr uint32_t kTransferRectInlineBatchMax = 240;
            const TransferRectInstance* rect_instances =
                rect_instance_fallback.data();
            uint32_t rect_instances_remaining =
                uint32_t(rect_instance_fallback.size());
            while (rect_instances_remaining) {
              uint32_t batch_count = std::min(rect_instances_remaining,
                                              kTransferRectInlineBatchMax);
              if (batch_count == 1) {
                bind_transfer_vertex_bytes_1(*rect_instances);
              } else {
                bind_transfer_vertex_bytes_1_span(rect_instances, batch_count);
              }
              encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip,
                                      NS::UInteger(0), NS::UInteger(4),
                                      NS::UInteger(batch_count));
              rect_instances += batch_count;
              rect_instances_remaining -= batch_count;
            }
          };

          if (is_stencil_bit) {
            if (use_native_stencil_output) {
              if (!native_stencil_output_state) {
                use_native_stencil_output = false;
              } else {
                constants.stencil_mask = 0xFFu;
                constants.stencil_clear = 0;
                bind_transfer_depth_state(native_stencil_output_state);
                bind_transfer_stencil_reference(0);
                draw_transfer_samples(draw_transfer);
              }
            }
            if (!use_native_stencil_output) {
              for (uint32_t bit = 0; bit < 8; ++bit) {
                MTL::DepthStencilState* stencil_state =
                    GetTransferStencilBitState(bit);
                if (!stencil_state) {
                  continue;
                }
                constants.stencil_mask = uint32_t(1) << bit;
                constants.stencil_clear = 0;
                bind_transfer_depth_state(stencil_state);
                bind_transfer_stencil_reference(uint32_t(1) << bit);
                draw_transfer_samples(draw_transfer);
              }
            }
          } else {
            constants.stencil_mask = 0;
            constants.stencil_clear = 0;
            draw_transfer_samples(draw_transfer);
          }
        }
      }
    }

    if (resolve_clear_needed && !resolve_clear_via_load_action) {
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_is_depth) {
        uint32_t depth_guest_clear_value =
            (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        float depth_host_clear_value = 0.0f;
        switch (dest_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            depth_host_clear_value =
                xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            depth_host_clear_value =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        MTL::RenderPipelineState* clear_pipeline =
            GetOrCreateTransferClearPipeline(
                dest_pixel_format, false, true, dest_sample_count, 0,
                use_active_render_encoder
                    ? &active_attachment_formats.color_attachment_formats
                    : nullptr,
                use_active_render_encoder
                    ? active_attachment_formats.depth_attachment_format
                    : MTL::PixelFormatInvalid,
                use_active_render_encoder
                    ? active_attachment_formats.stencil_attachment_format
                    : MTL::PixelFormatInvalid);
        MTL::DepthStencilState* clear_state =
            resolve_clear_stencil_only ? GetTransferStencilClearState()
                                       : GetTransferDepthClearState();
        if (clear_pipeline && clear_state) {
          MTL::RenderCommandEncoder* clear_encoder = ensure_transfer_encoder();
          if (clear_encoder) {
            TransferClearDepthConstants constants = {};
            constants.depth = depth_host_clear_value;
            clear_encoder->setRenderPipelineState(clear_pipeline);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationPipeline);
            clear_encoder->setDepthStencilState(clear_state);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationDepthStencil);
            clear_encoder->setStencilReferenceValue(uint32_t(clear_value) &
                                                    0xFF);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationStencilReference);
            clear_encoder->setFragmentBytes(&constants, sizeof(constants), 0);
            mark_active_encoder_mutation(
                kDrawPassTransferEncoderMutationFragmentSlot0);
            if (set_rect_viewport(clear_encoder, *resolve_clear_rectangle)) {
              clear_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                            NS::UInteger(0), NS::UInteger(3));
            }
          }
        }
      } else {
        TransferClearColorFloatConstants float_constants = {};
        TransferClearColorUintConstants uint_constants = {};
        bool clear_via_drawing = false;
        switch (dest_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            if (gamma_render_target_as_unorm16_) {
              for (uint32_t j = 0; j < 3; ++j) {
                float_constants.color[j] =
                    xenos::PWLGammaToLinear(float_constants.color[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::
              k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            for (uint32_t j = 0; j < 2; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            float_constants.color[1] = float(uint32_t(clear_value >> 32));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value) ||
                uint64_t(float_constants.color[1]) !=
                    uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }

        bool clear_is_uint = false;
        MTL::PixelFormat clear_format = GetColorOwnershipTransferPixelFormat(
            dest_key.GetColorFormat(), &clear_is_uint);
        MTL::Texture* clear_texture = dest_metal_rt->transfer_texture();
        bool clear_use_uint = clear_is_uint;

        if (clear_use_uint) {
          switch (dest_key.GetColorFormat()) {
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value) & 0xFFFF;
              uint_constants.color[1] = (uint32_t(clear_value) >> 16) & 0xFFFF;
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            case xenos::ColorRenderTargetFormat::k_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value) & 0xFFFF;
              uint_constants.color[1] = (uint32_t(clear_value) >> 16) & 0xFFFF;
              uint_constants.color[2] = (uint32_t(clear_value >> 32)) & 0xFFFF;
              uint_constants.color[3] =
                  (uint32_t(clear_value >> 32) >> 16) & 0xFFFF;
              break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value);
              uint_constants.color[1] = 0;
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value);
              uint_constants.color[1] = uint32_t(clear_value >> 32);
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            default:
              break;
          }
        }

        if (clear_via_drawing && clear_use_uint) {
          uint_constants.color[0] = uint32_t(clear_value);
          uint_constants.color[1] = uint32_t(clear_value >> 32);
          uint_constants.color[2] = 0;
          uint_constants.color[3] = 0;
        }

        if (clear_texture) {
          MTL::RenderPipelineState* clear_pipeline =
              GetOrCreateTransferClearPipeline(
                  clear_format, clear_use_uint, false, dest_sample_count,
                  use_active_render_encoder ? active_color_attachment_index
                                            : 0,
                  use_active_render_encoder
                      ? &active_attachment_formats.color_attachment_formats
                      : nullptr,
                  use_active_render_encoder
                      ? active_attachment_formats.depth_attachment_format
                      : MTL::PixelFormatInvalid,
                  use_active_render_encoder
                      ? active_attachment_formats.stencil_attachment_format
                      : MTL::PixelFormatInvalid);
          if (clear_pipeline) {
            MTL::RenderCommandEncoder* clear_encoder =
                ensure_transfer_encoder();
            if (clear_encoder) {
              MTL::DepthStencilState* no_depth_state =
                  GetTransferNoDepthStencilState();
              if (!no_depth_state) {
                continue;
              }
              clear_encoder->setRenderPipelineState(clear_pipeline);
              mark_active_encoder_mutation(
                  kDrawPassTransferEncoderMutationPipeline);
              clear_encoder->setDepthStencilState(no_depth_state);
              mark_active_encoder_mutation(
                  kDrawPassTransferEncoderMutationDepthStencil);
              if (clear_use_uint) {
                clear_encoder->setFragmentBytes(&uint_constants,
                                                sizeof(uint_constants), 0);
              } else {
                clear_encoder->setFragmentBytes(&float_constants,
                                                sizeof(float_constants), 0);
              }
              mark_active_encoder_mutation(
                  kDrawPassTransferEncoderMutationFragmentSlot0);
              if (set_rect_viewport(clear_encoder, *resolve_clear_rectangle)) {
                clear_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                              NS::UInteger(0), NS::UInteger(3));
              }
            }
          }
        }
      }
    }

    if (transfer_encoder && !use_active_render_encoder) {
      transfer_encoder->endEncoding();
    }
  }
  return true;
}

MTL::RenderPipelineState* MetalRenderTargetCache::GetOrCreateTransferPipelines(
    const TransferShaderKey& key, MTL::PixelFormat dest_format,
    bool dest_is_uint, bool native_stencil_output,
    uint32_t color_attachment_index,
    const TransferColorAttachmentFormats* color_attachment_formats,
    MTL::PixelFormat depth_attachment_format,
    MTL::PixelFormat stencil_attachment_format) {
  const TransferModeInfo& mode_info = kTransferModeInfos[size_t(key.mode)];
  TransferOutput output = mode_info.output;
  bool source_is_color = mode_info.source_is_color;
  bool has_host_depth = mode_info.uses_host_depth;
  native_stencil_output =
      native_stencil_output && output == TransferOutput::kStencilBit;

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);

  bool source_is_uint = false;
  if (source_is_color) {
    GetColorOwnershipTransferPixelFormat(source_color_format, &source_is_uint);
  }
  bool source_is_64bpp = false;
  if (source_is_color) {
    source_is_64bpp =
        xenos::IsColorRenderTargetFormat64bpp(source_color_format);
  }
  bool dest_is_depth = output != TransferOutput::kColor;
  TransferPipelineKey pipeline_key = {};
  pipeline_key.shader_key = key;
  pipeline_key.shader_key.host_depth_source_is_copy = 0;
  pipeline_key.native_stencil_output = native_stencil_output ? 1u : 0u;
  if (output == TransferOutput::kColor) {
    if (color_attachment_index >= xenos::kMaxColorRenderTargets) {
      return nullptr;
    }
    pipeline_key.color_attachment_index = color_attachment_index;
    if (color_attachment_formats) {
      pipeline_key.color_attachment_formats = *color_attachment_formats;
    } else {
      pipeline_key.color_attachment_formats.fill(MTL::PixelFormatInvalid);
      pipeline_key.color_attachment_formats[color_attachment_index] =
          dest_format;
    }
    if (pipeline_key.color_attachment_formats[color_attachment_index] !=
        dest_format) {
      return nullptr;
    }
    pipeline_key.depth_attachment_format = depth_attachment_format;
    pipeline_key.stencil_attachment_format = stencil_attachment_format;
  } else {
    pipeline_key.color_attachment_index = 0;
    if (color_attachment_formats) {
      pipeline_key.color_attachment_formats = *color_attachment_formats;
    } else {
      pipeline_key.color_attachment_formats.fill(MTL::PixelFormatInvalid);
    }
    pipeline_key.depth_attachment_format =
        depth_attachment_format != MTL::PixelFormatInvalid
            ? depth_attachment_format
            : dest_format;
    if (pipeline_key.depth_attachment_format != dest_format) {
      return nullptr;
    }
    if (dest_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        dest_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
      pipeline_key.stencil_attachment_format =
          stencil_attachment_format != MTL::PixelFormatInvalid
              ? stencil_attachment_format
              : dest_format;
      if (pipeline_key.stencil_attachment_format != dest_format) {
        return nullptr;
      }
    } else {
      pipeline_key.stencil_attachment_format = stencil_attachment_format;
    }
  }

  auto it = transfer_pipelines_.find(pipeline_key);
  if (it != transfer_pipelines_.end()) {
    return it->second;
  }
  hitch::Scope hitch_scope(hitch::kPsoXfer);
  // [xfer-prewarm] Built in the background at startup from the keys of earlier
  // sessions; otherwise built here (and its key recorded).
  MTL::RenderPipelineState* pipeline =
      TakePrewarmedTransferPipeline(pipeline_key, dest_is_uint);
  if (!pipeline) {
    const auto build_start = std::chrono::steady_clock::now();
    pipeline = BuildTransferPipeline(pipeline_key, dest_is_uint);
    if (!pipeline) {
      return nullptr;
    }
    // Which keys still cost a draw-path build (first 48 per session).
    static uint32_t logged = 0;
    if (logged < 48) {
      ++logged;
      std::string line = XferKeyLine(pipeline_key, dest_is_uint);
      if (!line.empty() && line.back() == '\n') line.pop_back();
      REXLOG_INFO("[xfer-prewarm] draw-path build {:.1f} ms: {}",
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - build_start)
                      .count(),
                  line);
    }
    NoteTransferPipelineKey(pipeline_key, dest_is_uint);
  }
  transfer_pipelines_.emplace(pipeline_key, pipeline);
  return pipeline;
}

// Thread-safe: reads only the key and settings fixed at initialization
// (FABLE2_XFER_PREWARM builds on background threads).
MTL::RenderPipelineState* MetalRenderTargetCache::BuildTransferPipeline(
    const TransferPipelineKey& pipeline_key, bool dest_is_uint) const {
  const TransferShaderKey& key = pipeline_key.shader_key;
  const TransferModeInfo& mode_info = kTransferModeInfos[size_t(key.mode)];
  TransferOutput output = mode_info.output;
  bool source_is_color = mode_info.source_is_color;
  bool has_host_depth = mode_info.uses_host_depth;
  const bool native_stencil_output = pipeline_key.native_stencil_output != 0;
  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  bool source_is_uint = false;
  if (source_is_color) {
    GetColorOwnershipTransferPixelFormat(source_color_format, &source_is_uint);
  }
  bool source_is_64bpp = false;
  if (source_is_color) {
    source_is_64bpp =
        xenos::IsColorRenderTargetFormat64bpp(source_color_format);
  }
  bool dest_is_depth = output != TransferOutput::kColor;

  bool dest_is_64bpp = false;
  bool dest_is_gamma_unorm16 = false;
  if (!dest_is_depth) {
    dest_is_64bpp = xenos::IsColorRenderTargetFormat64bpp(dest_color_format);
    dest_is_gamma_unorm16 =
        dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
        gamma_render_target_as_unorm16_ && !dest_is_uint;
  }
  bool source_needs_depth =
      !source_is_color && output != TransferOutput::kStencilBit;
  bool source_needs_stencil =
      !source_is_color && (output == TransferOutput::kColor ||
                           output == TransferOutput::kStencilBit);

  uint32_t dest_component_count = 1;
  if (output == TransferOutput::kColor) {
    dest_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
  }

  bool source_is_multisample =
      key.source_msaa_samples != xenos::MsaaSamples::k1X;
  bool dest_is_multisample = key.dest_msaa_samples != xenos::MsaaSamples::k1X;
  bool host_depth_is_multisample =
      key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X;
  uint32_t host_depth_texture_index = source_is_color ? 1 : 2;

  auto append_define = [](std::string& source, const char* name,
                          uint32_t value) {
    source.append("#define ");
    source.append(name);
    source.push_back(' ');
    source.append(std::to_string(value));
    source.push_back('\n');
  };

  std::string source;
  source.reserve(16384);
  append_define(source, "XE_TRANSFER_SOURCE_IS_COLOR", source_is_color ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_IS_DEPTH", source_is_color ? 0 : 1);
  append_define(source, "XE_TRANSFER_SOURCE_NEEDS_DEPTH",
                source_needs_depth ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_NEEDS_STENCIL",
                source_needs_stencil ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_IS_UINT", source_is_uint ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_IS_64BPP", source_is_64bpp ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_IS_MULTISAMPLE",
                source_is_multisample ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_IS_UINT", dest_is_uint ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_IS_DEPTH", dest_is_depth ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_IS_64BPP", dest_is_64bpp ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_IS_GAMMA_UNORM16",
                dest_is_gamma_unorm16 ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_COMPONENTS", dest_component_count);
  append_define(source, "XE_TRANSFER_DEST_IS_MULTISAMPLE",
                dest_is_multisample ? 1 : 0);
  append_define(source, "XE_TRANSFER_DEST_SAMPLE_ID_FROM_SAMPLE",
                key.dest_sample_id_from_sample ? 1 : 0);
  append_define(source, "XE_TRANSFER_COLOR_ATTACHMENT_INDEX",
                pipeline_key.color_attachment_index);
  append_define(source, "XE_TRANSFER_HAS_HOST_DEPTH", has_host_depth ? 1 : 0);
  append_define(source, "XE_TRANSFER_HOST_DEPTH_IS_MULTISAMPLE",
                host_depth_is_multisample ? 1 : 0);
  append_define(source, "XE_TRANSFER_SOURCE_FORMAT",
                key.source_resource_format);
  append_define(source, "XE_TRANSFER_DEST_FORMAT", key.dest_resource_format);
  append_define(source, "XE_TRANSFER_SOURCE_MSAA_SAMPLES",
                MsaaSamplesToCount(key.source_msaa_samples));
  append_define(source, "XE_TRANSFER_DEST_MSAA_SAMPLES",
                MsaaSamplesToCount(key.dest_msaa_samples));
  append_define(source, "XE_TRANSFER_HOST_DEPTH_MSAA_SAMPLES",
                MsaaSamplesToCount(key.host_depth_source_msaa_samples));
  append_define(source, "XE_TRANSFER_SOURCE_TEXTURE_INDEX", 0);
  append_define(source, "XE_TRANSFER_STENCIL_TEXTURE_INDEX", 1);
  append_define(source, "XE_TRANSFER_HOST_DEPTH_TEXTURE_INDEX",
                host_depth_texture_index);
  append_define(source, "XE_TRANSFER_OUTPUT_COLOR",
                output == TransferOutput::kColor ? 1 : 0);
  append_define(source, "XE_TRANSFER_OUTPUT_DEPTH",
                output == TransferOutput::kDepth ? 1 : 0);
  append_define(source, "XE_TRANSFER_OUTPUT_STENCIL_BIT",
                output == TransferOutput::kStencilBit ? 1 : 0);
  append_define(source, "XE_TRANSFER_NATIVE_STENCIL_OUTPUT",
                native_stencil_output ? 1 : 0);
  append_define(source, "XE_TRANSFER_FAST_DIVMOD",
                REXCVAR_GET(metal_transfer_fast_divmod) ? 1 : 0);
  append_define(source, "XE_FMT_8_8_8_8",
                uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8));
  append_define(source, "XE_FMT_8_8_8_8_GAMMA",
                uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA));
  append_define(source, "XE_FMT_2_10_10_10",
                uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10));
  append_define(
      source, "XE_FMT_2_10_10_10_AS_10_10_10_10",
      uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10));
  append_define(source, "XE_FMT_2_10_10_10_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT));
  append_define(
      source, "XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16",
      uint32_t(
          xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16));
  append_define(source, "XE_FMT_16_16",
                uint32_t(xenos::ColorRenderTargetFormat::k_16_16));
  append_define(source, "XE_FMT_16_16_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_16_16_FLOAT));
  append_define(source, "XE_FMT_16_16_16_16",
                uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16));
  append_define(source, "XE_FMT_16_16_16_16_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT));
  append_define(source, "XE_FMT_32_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_32_FLOAT));
  append_define(source, "XE_FMT_32_32_FLOAT",
                uint32_t(xenos::ColorRenderTargetFormat::k_32_32_FLOAT));
  append_define(source, "XE_FMT_D24S8",
                uint32_t(xenos::DepthRenderTargetFormat::kD24S8));
  append_define(source, "XE_FMT_D24FS8",
                uint32_t(xenos::DepthRenderTargetFormat::kD24FS8));
  append_define(source, "XE_GAMMA_RT_AS_UNORM16",
                gamma_render_target_as_unorm16_ ? 1 : 0);

  static const char kTransferShaderSource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct TransferAddressConstants {
  uint dest_pitch;
  uint source_pitch;
  int source_to_dest;
};

struct TransferShaderConstants {
  TransferAddressConstants address;
  TransferAddressConstants host_depth_address;
  uint source_format;
  uint dest_format;
  uint source_is_depth;
  uint dest_is_depth;
  uint source_is_uint;
  uint dest_is_uint;
  uint source_is_64bpp;
  uint dest_is_64bpp;
  uint source_msaa_samples;
  uint dest_msaa_samples;
  uint host_depth_source_msaa_samples;
  uint host_depth_source_is_copy;
  uint depth_round;
  uint msaa_2x_supported;
  uint tile_width_samples;
  uint tile_height_samples;
  uint dest_tile_width_pixels;
  uint dest_tile_height_pixels;
  float dest_tile_width_pixels_inv;
  float dest_tile_height_pixels_inv;
  float source_pitch_tiles_inv;
  float host_depth_source_pitch_tiles_inv;
  float dest_pixel_to_ndc_x;
  float dest_pixel_to_ndc_y;
  uint dest_sample_id;
  uint stencil_mask;
  uint stencil_clear;
  uint source_tile_width_samples;
  uint source_tile_height_samples;
  uint source_scale_x;
  uint source_scale_y;
  uint dest_scale_x;
  uint dest_scale_y;
};

// [effects-native] a destination pixel within its tile, in the source's
// resolution scale: point sample when the source is larger, replicate when it
// is smaller (native 1x effect targets vs scaled targets).
inline uint XeTransferScalePixel(uint pixel, uint source_scale,
                                 uint dest_scale) {
  if (source_scale == 0u || source_scale == dest_scale) {
    return pixel;
  }
  if (source_scale > dest_scale) {
    return pixel * (source_scale / dest_scale) +
           ((source_scale / dest_scale) >> 1u);
  }
  return pixel / (dest_scale / source_scale);
}

struct TransferRectInstance {
  float2 origin;
  float2 size;
};

constant uint kEdramTileCount = 2048u;

inline uint XeBitFieldMask(uint count) {
  if (count >= 32u) {
    return 0xFFFFFFFFu;
  }
  return (1u << count) - 1u;
}

inline uint XeBitFieldInsert(uint base, uint insert, uint offset, uint count) {
  uint mask = XeBitFieldMask(count) << offset;
  return (base & ~mask) | ((insert << offset) & mask);
}

inline uint XeBitFieldExtract(uint value, uint offset, uint count) {
  return (value >> offset) & XeBitFieldMask(count);
}

inline uint XeRoundToNearestEven(float value) {
  float floor_value = floor(value);
  float frac = value - floor_value;
  uint result = uint(floor_value);
  if (frac > 0.5f || (frac == 0.5f && (result & 1u))) {
    result += 1u;
  }
  return result;
}

inline uint XePackUnorm(float value, float scale) {
  return uint(clamp(value, 0.0f, 1.0f) * scale + 0.5f);
}

inline float XeSaturateNoNaN(float value) {
  float clamped = clamp(value, 0.0f, 1.0f);
  return (clamped == clamped) ? clamped : 0.0f;
}

inline float XeTruncToFloat(float value) {
  return trunc(value);
}

inline float XePWLGammaToLinear(float value) {
  float clamped = XeSaturateNoNaN(value);
  float scale;
  float offset;
  if (clamped >= (96.0f / 255.0f)) {
    if (clamped >= (192.0f / 255.0f)) {
      scale = 8.0f / 1024.0f;
      offset = -1024.0f;
    } else {
      scale = 4.0f / 1024.0f;
      offset = -256.0f;
    }
  } else {
    if (clamped >= (64.0f / 255.0f)) {
      scale = 2.0f / 1024.0f;
      offset = -64.0f;
    } else {
      scale = 1.0f / 1024.0f;
      offset = 0.0f;
    }
  }
  float linear = clamped * (255.0f * 1024.0f) * scale + offset;
  linear += XeTruncToFloat(linear * scale);
  return linear * (1.0f / 1023.0f);
}

inline void XeFastDivMod(uint x, uint w, float inv_w, thread uint& q,
                         thread uint& r) {
  q = uint(float(x) * inv_w);
  r = x - q * w;
  if (r >= w) {
    r -= w;
    q += 1u;
  } else if (r > x) {
    r += w;
    q -= 1u;
  }
}

inline float XeLinearToPWLGamma(float value) {
  float clamped = XeSaturateNoNaN(value);
  float scale;
  float offset;
  if (clamped >= (128.0f / 1023.0f)) {
    if (clamped >= (512.0f / 1023.0f)) {
      scale = 1023.0f / 8.0f;
      offset = 128.0f / 255.0f;
    } else {
      scale = 1023.0f / 4.0f;
      offset = 64.0f / 255.0f;
    }
  } else {
    if (clamped >= (64.0f / 1023.0f)) {
      scale = 1023.0f / 2.0f;
      offset = 32.0f / 255.0f;
    } else {
      scale = 1023.0f;
      offset = 0.0f;
    }
  }
  return XeTruncToFloat(clamped * scale) * (1.0f / 255.0f) + offset;
}

inline float3 XePWLGammaToLinear3(float3 v) {
  return float3(XePWLGammaToLinear(v.r), XePWLGammaToLinear(v.g),
                XePWLGammaToLinear(v.b));
}

inline float3 XeLinearToPWLGamma3(float3 v) {
  return float3(XeLinearToPWLGamma(v.r), XeLinearToPWLGamma(v.g),
                XeLinearToPWLGamma(v.b));
}

inline float XePWLGammaByteToLinearMidpoint(uint byte_value) {
  byte_value &= 0xFFu;
  float recip = 1.0f / 1023.0f;
  float offset = 0.5f / 1023.0f;
  if (byte_value >= 64u) {
    recip = 1.0f / 511.5f;
    offset = -31.5f / 511.5f;
  }
  if (byte_value >= 96u) {
    recip = 1.0f / 255.75f;
    offset = -63.5f / 255.75f;
  }
  if (byte_value >= 192u) {
    recip = 1.0f / 127.875f;
    offset = -127.5f / 127.875f;
  }
  return float(byte_value) * recip + offset;
}

inline float4 XePWLGammaPackedRGBA8ToLinearMidpoint(uint packed) {
  return float4(
      XePWLGammaByteToLinearMidpoint(packed),
      XePWLGammaByteToLinearMidpoint(packed >> 8u),
      XePWLGammaByteToLinearMidpoint(packed >> 16u),
      float((packed >> 24u) & 0xFFu) * (1.0f / 255.0f));
}

uint XePreClampedFloat32To7e3(float value) {
  uint f32 = as_type<uint>(value);
  uint biased_f32;
  if (f32 < 0x3E800000u) {
    uint f32_exp = f32 >> 23u;
    uint shift = 125u - f32_exp;
    shift = min(shift, 24u);
    uint mantissa = (f32 & 0x7FFFFFu) | 0x800000u;
    biased_f32 = mantissa >> shift;
  } else {
    biased_f32 = f32 + 0xC2000000u;
  }
  uint round_bit = (biased_f32 >> 16u) & 1u;
  uint f10 = biased_f32 + 0x7FFFu + round_bit;
  return (f10 >> 16u) & 0x3FFu;
}

uint XeUnclampedFloat32To7e3(float value) {
  float clamped = min(max(value, 0.0f), 31.875f);
  return XePreClampedFloat32To7e3(clamped);
}

float XeFloat7e3To32(uint f10) {
  f10 &= 0x3FFu;
  if (f10 == 0u) {
    return 0.0f;
  }
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint mantissa_lzcnt = clz(mantissa) - 24u;
    exponent = uint(int(1) - int(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7Fu;
  }
  uint f32 = ((exponent + 124u) << 23u) | (mantissa << 16u);
  return as_type<float>(f32);
}

uint XeFloat32To20e4(float value, bool round_to_nearest_even) {
  uint f32 = as_type<uint>(value);
  f32 = min((f32 <= 0x7FFFFFFFu) ? f32 : 0u, 0x3FFFFFF8u);
  uint denormalized =
      ((f32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32 >> 23u), 24u);
  uint f24 = (f32 < 0x38800000u) ? denormalized : (f32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24 += 3u + ((f24 >> 3u) & 1u);
  }
  return (f24 >> 3u) & 0xFFFFFFu;
}

float XeFloat20e4To32(uint f24, bool remap_to_0_to_0_5) {
  if (f24 == 0u) {
    return 0.0f;
  }
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20u;
  if (exponent == 0u) {
    uint msb = 31u - clz(mantissa);
    uint mantissa_lzcnt = 20u - msb;
    exponent = 1u - mantissa_lzcnt;
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFFu;
  }
  uint bias = remap_to_0_to_0_5 ? 111u : 112u;
  uint f32 = ((exponent + bias) << 23u) | (mantissa << 3u);
  return as_type<float>(f32);
}

float XeUnorm24To32(uint n24) {
  return float(n24 + (n24 >> 23u)) * (1.0f / 16777216.0f);
}

uint XePackColorRGBA8(float4 color) {
  uint r = XePackUnorm(color.r, 255.0f);
  uint g = XePackUnorm(color.g, 255.0f);
  uint b = XePackUnorm(color.b, 255.0f);
  uint a = XePackUnorm(color.a, 255.0f);
  return r | (g << 8u) | (b << 16u) | (a << 24u);
}

uint XePackColorRGB10A2(float4 color) {
  uint r = XePackUnorm(color.r, 1023.0f);
  uint g = XePackUnorm(color.g, 1023.0f);
  uint b = XePackUnorm(color.b, 1023.0f);
  uint a = XePackUnorm(color.a, 3.0f);
  return r | (g << 10u) | (b << 20u) | (a << 30u);
}

uint XePackColorRGB10A2Float(float4 color) {
  uint r = XeUnclampedFloat32To7e3(color.r);
  uint g = XeUnclampedFloat32To7e3(color.g);
  uint b = XeUnclampedFloat32To7e3(color.b);
  uint a = XePackUnorm(color.a, 3.0f);
  return (r & 0x3FFu) | ((g & 0x3FFu) << 10u) |
         ((b & 0x3FFu) << 20u) | ((a & 0x3u) << 30u);
}

struct VSOut {
  float4 position [[position]];
};

vertex VSOut transfer_vs(uint vid [[vertex_id]]) {
  float2 pt = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(pt * 2.0f - 1.0f, 0.0f, 1.0f);
  return out;
}

vertex VSOut transfer_rect_vs(uint vid [[vertex_id]],
                              uint iid [[instance_id]],
                              constant TransferShaderConstants& constants
                                  [[buffer(0)]],
                              constant TransferRectInstance* instances
                                  [[buffer(1)]]) {
  float2 quad = float2(float(vid & 1), float(vid >> 1));
  TransferRectInstance inst = instances[iid];
  float2 pos_pixel = inst.origin + quad * inst.size;
  float2 ndc;
  ndc.x = pos_pixel.x * constants.dest_pixel_to_ndc_x - 1.0f;
  ndc.y = 1.0f - pos_pixel.y * constants.dest_pixel_to_ndc_y;
  VSOut out;
  out.position = float4(ndc, 0.0f, 1.0f);
  return out;
}

#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
    #if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d_ms<uint, access::read> xe_transfer_source \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #else
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d<uint, access::read> xe_transfer_source \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #endif
  #else
    #if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d_ms<float, access::read> xe_transfer_source \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #else
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d<float, access::read> xe_transfer_source \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #endif
  #endif
#else
  #if XE_TRANSFER_SOURCE_NEEDS_DEPTH && XE_TRANSFER_SOURCE_NEEDS_STENCIL
    #if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d_ms<float, access::read> xe_transfer_source_depth \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]], \
            texture2d_ms<uint, access::read> xe_transfer_source_stencil \
              [[texture(XE_TRANSFER_STENCIL_TEXTURE_INDEX)]]
    #else
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d<float, access::read> xe_transfer_source_depth \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]], \
            texture2d<uint, access::read> xe_transfer_source_stencil \
              [[texture(XE_TRANSFER_STENCIL_TEXTURE_INDEX)]]
    #endif
  #elif XE_TRANSFER_SOURCE_NEEDS_DEPTH
    #if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d_ms<float, access::read> xe_transfer_source_depth \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #else
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d<float, access::read> xe_transfer_source_depth \
              [[texture(XE_TRANSFER_SOURCE_TEXTURE_INDEX)]]
    #endif
  #elif XE_TRANSFER_SOURCE_NEEDS_STENCIL
    #if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d_ms<uint, access::read> xe_transfer_source_stencil \
              [[texture(XE_TRANSFER_STENCIL_TEXTURE_INDEX)]]
    #else
      #define XE_TRANSFER_SOURCE_PARAMS \
          , texture2d<uint, access::read> xe_transfer_source_stencil \
              [[texture(XE_TRANSFER_STENCIL_TEXTURE_INDEX)]]
    #endif
  #else
    #define XE_TRANSFER_SOURCE_PARAMS
  #endif
#endif

#if XE_TRANSFER_HAS_HOST_DEPTH
  #define XE_TRANSFER_HOST_DEPTH_BUFFER_PARAM \
      , device const uint* xe_transfer_host_depth_buffer [[buffer(1)]]
#else
  #define XE_TRANSFER_HOST_DEPTH_BUFFER_PARAM
#endif

#if XE_TRANSFER_HAS_HOST_DEPTH
  #if XE_TRANSFER_HOST_DEPTH_IS_MULTISAMPLE
    #define XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM \
        , texture2d_ms<float, access::read> xe_transfer_host_depth \
            [[texture(XE_TRANSFER_HOST_DEPTH_TEXTURE_INDEX)]]
  #else
    #define XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM \
        , texture2d<float, access::read> xe_transfer_host_depth \
            [[texture(XE_TRANSFER_HOST_DEPTH_TEXTURE_INDEX)]]
  #endif
#else
  #define XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM
#endif

#if XE_TRANSFER_DEST_IS_MULTISAMPLE && XE_TRANSFER_DEST_SAMPLE_ID_FROM_SAMPLE
  #define XE_TRANSFER_SAMPLE_ID_PARAM , uint xe_sample_id [[sample_id]]
#else
  #define XE_TRANSFER_SAMPLE_ID_PARAM
#endif

#if XE_TRANSFER_OUTPUT_COLOR
  #if XE_TRANSFER_DEST_IS_UINT
    #if XE_TRANSFER_DEST_COMPONENTS == 1
      typedef uint XeTransferColorOutType;
    #elif XE_TRANSFER_DEST_COMPONENTS == 2
      typedef uint2 XeTransferColorOutType;
    #else
      typedef uint4 XeTransferColorOutType;
    #endif
  #else
    #if XE_TRANSFER_DEST_COMPONENTS == 1
      typedef float XeTransferColorOutType;
    #elif XE_TRANSFER_DEST_COMPONENTS == 2
      typedef float2 XeTransferColorOutType;
    #else
      typedef float4 XeTransferColorOutType;
    #endif
  #endif

struct TransferColorOut {
  XeTransferColorOutType color [[color(XE_TRANSFER_COLOR_ATTACHMENT_INDEX)]];
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  uint sample_mask [[sample_mask]];
#endif
};

fragment TransferColorOut transfer_ps(
    VSOut in [[stage_in]],
    constant TransferShaderConstants& constants [[buffer(0)]]
    XE_TRANSFER_HOST_DEPTH_BUFFER_PARAM
    XE_TRANSFER_SOURCE_PARAMS
    XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM
    XE_TRANSFER_SAMPLE_ID_PARAM) {
  uint2 dest_pixel = uint2(in.position.xy);
  uint dest_sample_id = 0u;
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  #if XE_TRANSFER_DEST_SAMPLE_ID_FROM_SAMPLE
    dest_sample_id = xe_sample_id;
  #else
    dest_sample_id = constants.dest_sample_id;
  #endif
#endif

  uint tile_width_samples = constants.tile_width_samples;
  uint tile_height_samples = constants.tile_height_samples;
  uint dest_tile_width_pixels = constants.dest_tile_width_pixels;
  uint dest_tile_height_pixels = constants.dest_tile_height_pixels;

  uint dest_tile_pixel_x = 0u;
  uint dest_tile_pixel_y = 0u;
  uint dest_tile_index = 0u;
  uint dest_tile_index_x = 0u;
  uint dest_tile_index_y = 0u;
#if XE_TRANSFER_FAST_DIVMOD
  XeFastDivMod(dest_pixel.x, dest_tile_width_pixels,
               constants.dest_tile_width_pixels_inv, dest_tile_index_x,
               dest_tile_pixel_x);
  XeFastDivMod(dest_pixel.y, dest_tile_height_pixels,
               constants.dest_tile_height_pixels_inv, dest_tile_index_y,
               dest_tile_pixel_y);
#else
  dest_tile_index_x = dest_pixel.x / dest_tile_width_pixels;
  dest_tile_pixel_x = dest_pixel.x % dest_tile_width_pixels;
  dest_tile_index_y = dest_pixel.y / dest_tile_height_pixels;
  dest_tile_pixel_y = dest_pixel.y % dest_tile_height_pixels;
#endif

  dest_tile_index =
      dest_tile_index_x +
      dest_tile_index_y * constants.address.dest_pitch;

  uint source_sample_id = dest_sample_id;
  // [effects-native] mixed-scale transfers map through the source's scale.
  uint source_tile_width_samples = constants.source_tile_width_samples != 0u
      ? constants.source_tile_width_samples : tile_width_samples;
  uint source_tile_height_samples = constants.source_tile_height_samples != 0u
      ? constants.source_tile_height_samples : tile_height_samples;
  uint xfer_tile_pixel_x = XeTransferScalePixel(
      dest_tile_pixel_x, constants.source_scale_x, constants.dest_scale_x);
  uint xfer_tile_pixel_y = XeTransferScalePixel(
      dest_tile_pixel_y, constants.source_scale_y, constants.dest_scale_y);
  uint source_tile_pixel_x = xfer_tile_pixel_x;
  uint source_tile_pixel_y = xfer_tile_pixel_y;
  uint source_color_half = 0u;
  bool source_color_half_valid = false;

  bool source_is_64bpp = XE_TRANSFER_SOURCE_IS_64BPP != 0u;
  bool dest_is_64bpp = XE_TRANSFER_DEST_IS_64BPP != 0u;
  uint source_msaa = XE_TRANSFER_SOURCE_MSAA_SAMPLES;
  uint dest_msaa = XE_TRANSFER_DEST_MSAA_SAMPLES;
  bool msaa_2x_supported = constants.msaa_2x_supported != 0u;

  if (!source_is_64bpp && dest_is_64bpp) {
    if (source_msaa >= 4u) {
      if (dest_msaa >= 4u) {
        source_sample_id = dest_sample_id & 2u;
        source_tile_pixel_x =
            XeBitFieldInsert(dest_sample_id, xfer_tile_pixel_x, 1u, 31u);
      } else if (dest_msaa == 2u) {
        if (msaa_2x_supported) {
          source_sample_id = (dest_sample_id ^ 1u) << 1u;
        } else {
          source_sample_id = dest_sample_id & 2u;
        }
      } else {
        source_sample_id =
            XeBitFieldInsert(0u, xfer_tile_pixel_y, 1u, 1u);
        source_tile_pixel_y = xfer_tile_pixel_y >> 1u;
      }
    } else {
      if (dest_msaa >= 4u) {
        source_tile_pixel_x = XeBitFieldInsert(
            xfer_tile_pixel_x << 2u, dest_sample_id, 1u, 1u);
      } else {
        source_tile_pixel_x = xfer_tile_pixel_x << 1u;
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    if (dest_msaa >= 4u) {
      if (source_msaa >= 4u) {
        source_sample_id =
            XeBitFieldInsert(dest_sample_id, xfer_tile_pixel_x, 0u, 1u);
        source_tile_pixel_x = xfer_tile_pixel_x >> 1u;
      }
      source_color_half = dest_sample_id & 1u;
      source_color_half_valid = true;
    } else {
      if (source_msaa >= 4u) {
        source_sample_id = XeBitFieldExtract(xfer_tile_pixel_x, 1u, 1u);
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            source_sample_id = XeBitFieldInsert(
                source_sample_id, dest_sample_id ^ 1u, 1u, 1u);
          } else {
            source_sample_id = XeBitFieldInsert(
                dest_sample_id, source_sample_id, 0u, 1u);
          }
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, xfer_tile_pixel_y, 1u, 1u);
          source_tile_pixel_y = xfer_tile_pixel_y >> 1u;
        }
        source_tile_pixel_x = xfer_tile_pixel_x >> 2u;
      } else {
        source_tile_pixel_x = xfer_tile_pixel_x >> 1u;
      }
      source_color_half = xfer_tile_pixel_x & 1u;
      source_color_half_valid = true;
    }
  } else {
    if (source_msaa != dest_msaa) {
      if (source_msaa >= 4u) {
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            source_sample_id = XeBitFieldInsert(
                xfer_tile_pixel_x, dest_sample_id ^ 1u, 1u, 31u);
          } else {
            source_sample_id = XeBitFieldInsert(
                dest_sample_id, xfer_tile_pixel_x, 0u, 1u);
          }
          source_tile_pixel_x = xfer_tile_pixel_x >> 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              xfer_tile_pixel_x & 1u, xfer_tile_pixel_y, 1u, 1u);
          source_tile_pixel_x = xfer_tile_pixel_x >> 1u;
          source_tile_pixel_y = xfer_tile_pixel_y >> 1u;
        }
      } else if (dest_msaa >= 4u) {
        source_tile_pixel_x = XeBitFieldInsert(
            dest_sample_id, xfer_tile_pixel_x, 1u, 31u);
      }
    }
  }

  if (source_msaa < 4u && source_msaa != dest_msaa) {
    if (dest_msaa >= 4u) {
      if (source_msaa == 2u) {
        source_sample_id = dest_sample_id >> 1u;
        if (msaa_2x_supported) {
          source_sample_id ^= 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, source_sample_id, 1u, 1u);
        }
      } else {
        source_tile_pixel_y = XeBitFieldInsert(
            dest_sample_id >> 1u, xfer_tile_pixel_y, 1u, 31u);
      }
    } else {
      if (source_msaa == 2u) {
        source_sample_id = xfer_tile_pixel_y & 1u;
        if (msaa_2x_supported) {
          source_sample_id ^= 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, source_sample_id, 1u, 1u);
        }
        source_tile_pixel_y = xfer_tile_pixel_y >> 1u;
      } else {
        if (msaa_2x_supported) {
          source_tile_pixel_y = XeBitFieldInsert(
              dest_sample_id ^ 1u, xfer_tile_pixel_y, 1u, 31u);
        } else {
          source_tile_pixel_y = XeBitFieldInsert(
              dest_sample_id >> 1u, xfer_tile_pixel_y, 1u, 31u);
        }
      }
    }
  }

  uint source_pixel_width_dwords_log2 =
      (source_msaa >= 4u ? 1u : 0u) + (source_is_64bpp ? 1u : 0u);

  if ((XE_TRANSFER_SOURCE_IS_DEPTH != 0u) != (XE_TRANSFER_DEST_IS_DEPTH != 0u)) {
    uint source_32bpp_tile_half_pixels =
        source_tile_width_samples >> (1u + source_pixel_width_dwords_log2);
    if (source_tile_pixel_x < source_32bpp_tile_half_pixels) {
      source_tile_pixel_x += source_32bpp_tile_half_pixels;
    } else {
      source_tile_pixel_x -= source_32bpp_tile_half_pixels;
    }
  }

  uint source_pixel_x = 0u;
  uint source_pixel_y = 0u;
  uint source_tile_index =
      uint(int(dest_tile_index) + constants.address.source_to_dest) &
      (kEdramTileCount - 1u);
  uint source_pitch_tiles = constants.address.source_pitch;
  uint source_tile_index_y = 0u;
  uint source_tile_index_x = 0u;
  XeFastDivMod(source_tile_index, source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_index_y,
               source_tile_index_x);
  source_pixel_x =
      source_tile_index_x *
          (source_tile_width_samples >> source_pixel_width_dwords_log2) +
      source_tile_pixel_x;
  source_pixel_y =
      source_tile_index_y *
          (source_tile_height_samples >> (source_msaa >= 2u ? 1u : 0u)) +
      source_tile_pixel_y;

  bool load_two = !source_is_64bpp && dest_is_64bpp;
  uint source_pixel_x1 = source_pixel_x;
  uint source_sample_id1 = source_sample_id;
  if (load_two) {
    if (source_msaa >= 4u) {
      source_sample_id1 = source_sample_id | 1u;
    } else {
      source_pixel_x1 = source_pixel_x | 1u;
    }
  }

#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
  uint4 source_color0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y),
                              source_sample_id);
#else
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y));
#endif
  uint4 source_color1 = source_color0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1);
#else
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y));
#endif
  }
  #else
  float4 source_color0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y),
                              source_sample_id);
#else
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y));
#endif
  float4 source_color1 = source_color0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1);
#else
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y));
#endif
  }
#if !XE_TRANSFER_SOURCE_IS_MULTISAMPLE
  // [effects-native] scaled -> native-scale colour (same MSAA and pixel size):
  // box-filter the block of source pixels instead of point sampling it, so
  // thin features keep their coverage (as when the game renders at 1x).
  if (!load_two && source_msaa == 1u && dest_msaa == 1u &&
      source_is_64bpp == dest_is_64bpp &&
      constants.dest_scale_x != 0u && constants.dest_scale_y != 0u &&
      constants.source_scale_x > constants.dest_scale_x &&
      constants.source_scale_y >= constants.dest_scale_y) {
    uint box_x = constants.source_scale_x / constants.dest_scale_x;
    uint box_y = max(constants.source_scale_y / constants.dest_scale_y, 1u);
    uint x0 = source_pixel_x - min(source_pixel_x, box_x >> 1u);
    uint y0 = source_pixel_y - min(source_pixel_y, box_y >> 1u);
    float4 sum = float4(0.0f);
    for (uint by = 0u; by < box_y; ++by) {
      for (uint bx = 0u; bx < box_x; ++bx) {
        sum += xe_transfer_source.read(uint2(x0 + bx, y0 + by));
      }
    }
    source_color0 = sum / float(box_x * box_y);
    source_color1 = source_color0;
  }
#endif
  #endif
#else
#if XE_TRANSFER_SOURCE_NEEDS_DEPTH
  float source_depth0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source_depth.read(uint2(source_pixel_x, source_pixel_y),
                                    source_sample_id).r;
#else
      xe_transfer_source_depth.read(uint2(source_pixel_x, source_pixel_y)).r;
#endif
  float source_depth1 = source_depth0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_depth1 = xe_transfer_source_depth.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1).r;
#else
    source_depth1 = xe_transfer_source_depth.read(
        uint2(source_pixel_x1, source_pixel_y)).r;
#endif
  }
#endif
#if XE_TRANSFER_SOURCE_NEEDS_STENCIL
  uint source_stencil0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source_stencil.read(uint2(source_pixel_x, source_pixel_y),
                                      source_sample_id).r;
#else
      xe_transfer_source_stencil.read(uint2(source_pixel_x, source_pixel_y)).r;
#endif
  uint source_stencil1 = source_stencil0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_stencil1 = xe_transfer_source_stencil.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1).r;
#else
    source_stencil1 = xe_transfer_source_stencil.read(
        uint2(source_pixel_x1, source_pixel_y)).r;
#endif
  }
#else
  uint source_stencil0 = 0u;
  uint source_stencil1 = 0u;
#endif
#endif

#if XE_TRANSFER_SOURCE_IS_COLOR
  if (source_is_64bpp && !dest_is_64bpp && source_color_half_valid) {
    uint source_component_count = 0u;
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_32_FLOAT:
        source_component_count = 1u;
        break;
      case XE_FMT_16_16:
      case XE_FMT_16_16_FLOAT:
      case XE_FMT_32_32_FLOAT:
        source_component_count = 2u;
        break;
      default:
        source_component_count = 4u;
        break;
    }
    if (source_component_count == 2u) {
  #if XE_TRANSFER_SOURCE_IS_UINT
      source_color0[0] = source_color_half != 0u ? source_color0[1]
                                                 : source_color0[0];
  #else
      source_color0[0] = source_color_half != 0u ? source_color0[1]
                                                 : source_color0[0];
  #endif
    } else if (source_color_half != 0u) {
  #if XE_TRANSFER_SOURCE_IS_UINT
      source_color0[0] = source_color0[2];
      source_color0[1] = source_color0[3];
  #else
      source_color0[0] = source_color0[2];
      source_color0[1] = source_color0[3];
  #endif
    }
  }
#endif

#if XE_TRANSFER_DEST_IS_UINT
  uint4 out_color = uint4(0u);
#else
  float4 out_color = float4(0.0f);
#endif

  if (dest_is_64bpp) {
    uint2 packed64 = uint2(0u);
#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_16_16:
      case XE_FMT_16_16_FLOAT:
        packed64.x = source_color0[0] | (source_color0[1] << 16u);
        packed64.y = source_color1[0] | (source_color1[1] << 16u);
        break;
      case XE_FMT_16_16_16_16:
      case XE_FMT_16_16_16_16_FLOAT:
        packed64.x = source_color0[0] | (source_color0[1] << 16u);
        packed64.y = source_color0[2] | (source_color0[3] << 16u);
        break;
      case XE_FMT_32_FLOAT:
        packed64.x = source_color0[0];
        packed64.y = source_color1[0];
        break;
      case XE_FMT_32_32_FLOAT:
        packed64.x = source_color0[0];
        packed64.y = source_color0[1];
        break;
      default:
        packed64.x = source_color0[0];
        packed64.y = source_color1[0];
        break;
    }
  #else
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_8_8_8_8_GAMMA: {
#if XE_GAMMA_RT_AS_UNORM16
        float4 gamma_color0 = source_color0;
        float4 gamma_color1 = source_color1;
        gamma_color0.rgb = XeLinearToPWLGamma3(gamma_color0.rgb);
        gamma_color1.rgb = XeLinearToPWLGamma3(gamma_color1.rgb);
        packed64.x = XePackColorRGBA8(gamma_color0);
        packed64.y = XePackColorRGBA8(gamma_color1);
#else
        packed64.x = XePackColorRGBA8(source_color0);
        packed64.y = XePackColorRGBA8(source_color1);
#endif
      } break;
      case XE_FMT_8_8_8_8:
        packed64.x = XePackColorRGBA8(source_color0);
        packed64.y = XePackColorRGBA8(source_color1);
        break;
      case XE_FMT_2_10_10_10:
      case XE_FMT_2_10_10_10_AS_10_10_10_10:
        packed64.x = XePackColorRGB10A2(source_color0);
        packed64.y = XePackColorRGB10A2(source_color1);
        break;
      case XE_FMT_2_10_10_10_FLOAT:
      case XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16:
        packed64.x = XePackColorRGB10A2Float(source_color0);
        packed64.y = XePackColorRGB10A2Float(source_color1);
        break;
      case XE_FMT_32_FLOAT:
        packed64.x = as_type<uint>(source_color0[0]);
        packed64.y = as_type<uint>(source_color1[0]);
        break;
      case XE_FMT_32_32_FLOAT:
        packed64.x = as_type<uint>(source_color0[0]);
        packed64.y = as_type<uint>(source_color0[1]);
        break;
      default:
        packed64.x = as_type<uint>(source_color0[0]);
        packed64.y = as_type<uint>(source_color1[0]);
        break;
    }
  #endif
#else
    uint depth24_0 = 0u;
    uint depth24_1 = 0u;
    if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_D24FS8) {
      bool round_depth = constants.depth_round != 0u;
      depth24_0 = XeFloat32To20e4(source_depth0 * 2.0f, round_depth);
      depth24_1 = XeFloat32To20e4(source_depth1 * 2.0f, round_depth);
    } else {
      depth24_0 = XeRoundToNearestEven(
          clamp(source_depth0, 0.0f, 1.0f) * 16777215.0f);
      depth24_1 = XeRoundToNearestEven(
          clamp(source_depth1, 0.0f, 1.0f) * 16777215.0f);
    }
    packed64.x = (depth24_0 << 8u) | (source_stencil0 & 0xFFu);
    packed64.y = (depth24_1 << 8u) | (source_stencil1 & 0xFFu);
#endif

    if (XE_TRANSFER_DEST_FORMAT == XE_FMT_32_32_FLOAT) {
#if XE_TRANSFER_DEST_IS_UINT
      out_color = uint4(packed64.x, packed64.y, 0u, 0u);
#else
      out_color = float4(as_type<float>(packed64.x),
                         as_type<float>(packed64.y), 0.0f, 0.0f);
#endif
    } else {
      uint4 components = uint4(packed64.x & 0xFFFFu, packed64.x >> 16u,
                               packed64.y & 0xFFFFu, packed64.y >> 16u);
#if XE_TRANSFER_DEST_IS_UINT
      out_color = components;
#else
      out_color = float4(components);
#endif
    }
  } else {
    bool wrote_direct = false;
    uint packed32 = 0u;
#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_16_16:
      case XE_FMT_16_16_FLOAT:
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_16_16 ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_16_16_FLOAT) {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(source_color0[0], source_color0[1], 0u, 0u);
#else
          out_color = float4(float(source_color0[0]),
                             float(source_color0[1]), 0.0f, 0.0f);
#endif
          wrote_direct = true;
        } else {
          packed32 = source_color0[0] | (source_color0[1] << 16u);
        }
        break;
      case XE_FMT_16_16_16_16:
      case XE_FMT_16_16_16_16_FLOAT:
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_16_16 ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_16_16_FLOAT) {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(source_color0[0], source_color0[1], 0u, 0u);
#else
          out_color = float4(float(source_color0[0]),
                             float(source_color0[1]), 0.0f, 0.0f);
#endif
          wrote_direct = true;
        } else {
          packed32 = source_color0[0] | (source_color0[1] << 16u);
        }
        break;
      case XE_FMT_32_FLOAT:
      case XE_FMT_32_32_FLOAT:
        packed32 = source_color0[0];
        break;
      default:
        packed32 = source_color0[0];
        break;
    }
  #else
    switch (XE_TRANSFER_SOURCE_FORMAT) {
      case XE_FMT_8_8_8_8:
      case XE_FMT_8_8_8_8_GAMMA: {
        float4 color = source_color0;
#if XE_GAMMA_RT_AS_UNORM16
        if ((XE_TRANSFER_SOURCE_FORMAT == XE_FMT_8_8_8_8_GAMMA ||
             XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8_GAMMA) &&
            (XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8 ||
             XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8_GAMMA) &&
            XE_TRANSFER_DEST_FORMAT != XE_TRANSFER_SOURCE_FORMAT) {
          if (XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8) {
            color.rgb = XeLinearToPWLGamma3(color.rgb);
          } else {
            color.rgb = XePWLGammaToLinear3(color.rgb);
          }
        }
#endif
#if !XE_TRANSFER_DEST_IS_UINT
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8 ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_8_8_8_8_GAMMA) {
          out_color = color;
          wrote_direct = true;
        } else
#endif
        {
#if XE_GAMMA_RT_AS_UNORM16
          if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_8_8_8_8_GAMMA) {
            color.rgb = XeLinearToPWLGamma3(color.rgb);
          }
#endif
          uint packed_component_offset = 0u;
          if (XE_TRANSFER_DEST_IS_DEPTH != 0u) {
            packed_component_offset = 1u;
          }
          packed32 =
              XePackUnorm(color[packed_component_offset], 255.0f);
          if (XE_TRANSFER_DEST_IS_DEPTH == 0u) {
            packed32 |= XePackUnorm(color[packed_component_offset + 1],
                                    255.0f) << 8u;
            packed32 |= XePackUnorm(color[packed_component_offset + 2],
                                    255.0f) << 16u;
            packed32 |= XePackUnorm(color[packed_component_offset + 3],
                                    255.0f) << 24u;
          }
        }
      } break;
      case XE_FMT_2_10_10_10:
      case XE_FMT_2_10_10_10_AS_10_10_10_10:
#if !XE_TRANSFER_DEST_IS_UINT
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_2_10_10_10 ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_2_10_10_10_AS_10_10_10_10) {
          out_color = source_color0;
          wrote_direct = true;
        } else
#endif
        {
          packed32 = XePackUnorm(source_color0[0], 1023.0f);
          if (XE_TRANSFER_DEST_IS_DEPTH == 0u) {
            packed32 |= XePackUnorm(source_color0[1], 1023.0f) << 10u;
            packed32 |= XePackUnorm(source_color0[2], 1023.0f) << 20u;
            packed32 |= XePackUnorm(source_color0[3], 3.0f) << 30u;
          }
        }
        break;
      case XE_FMT_2_10_10_10_FLOAT:
      case XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16:
#if !XE_TRANSFER_DEST_IS_UINT
        if (XE_TRANSFER_DEST_FORMAT == XE_FMT_2_10_10_10_FLOAT ||
            XE_TRANSFER_DEST_FORMAT == XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16) {
          out_color = source_color0;
          wrote_direct = true;
        } else
#endif
        {
          packed32 = XeUnclampedFloat32To7e3(source_color0[0]);
          if (XE_TRANSFER_DEST_IS_DEPTH == 0u) {
            packed32 |= XeUnclampedFloat32To7e3(source_color0[1]) << 10u;
            packed32 |= XeUnclampedFloat32To7e3(source_color0[2]) << 20u;
            packed32 |= XePackUnorm(source_color0[3], 3.0f) << 30u;
          }
        }
        break;
      case XE_FMT_32_FLOAT:
      case XE_FMT_32_32_FLOAT:
        packed32 = as_type<uint>(source_color0[0]);
        break;
      default:
        packed32 = as_type<uint>(source_color0[0]);
        break;
    }
  #endif
#else
    if (XE_TRANSFER_DEST_IS_DEPTH != 0u &&
        XE_TRANSFER_DEST_FORMAT == XE_TRANSFER_SOURCE_FORMAT) {
      TransferColorOut out;
      out.color = XeTransferColorOutType(source_depth0);
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
      out.sample_mask = 1u << dest_sample_id;
#endif
      return out;
    }
    if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_D24FS8) {
      bool round_depth = constants.depth_round != 0u;
      packed32 = XeFloat32To20e4(source_depth0 * 2.0f, round_depth);
    } else {
      packed32 = XeRoundToNearestEven(
          clamp(source_depth0, 0.0f, 1.0f) * 16777215.0f);
    }
    if (XE_TRANSFER_DEST_IS_DEPTH == 0u) {
      packed32 = (packed32 << 8u) | (source_stencil0 & 0xFFu);
    }
#endif

    if (!wrote_direct) {
      switch (XE_TRANSFER_DEST_FORMAT) {
        case XE_FMT_8_8_8_8: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
          out_color = float4(
              float((packed32 >> 0u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 8u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 16u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 24u) & 0xFFu) * (1.0f / 255.0f));
#endif
        } break;
        case XE_FMT_8_8_8_8_GAMMA: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
#if XE_TRANSFER_DEST_IS_GAMMA_UNORM16
          out_color = XePWLGammaPackedRGBA8ToLinearMidpoint(packed32);
#else
          float4 color = float4(
              float((packed32 >> 0u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 8u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 16u) & 0xFFu) * (1.0f / 255.0f),
              float((packed32 >> 24u) & 0xFFu) * (1.0f / 255.0f));
#if XE_GAMMA_RT_AS_UNORM16
          color.rgb = XePWLGammaToLinear3(color.rgb);
#endif
          out_color = color;
#endif
#endif
        } break;
        case XE_FMT_2_10_10_10:
        case XE_FMT_2_10_10_10_AS_10_10_10_10: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
          out_color = float4(
              float((packed32 >> 0u) & 0x3FFu) * (1.0f / 1023.0f),
              float((packed32 >> 10u) & 0x3FFu) * (1.0f / 1023.0f),
              float((packed32 >> 20u) & 0x3FFu) * (1.0f / 1023.0f),
              float((packed32 >> 30u) & 0x3u) * (1.0f / 3.0f));
#endif
        } break;
        case XE_FMT_2_10_10_10_FLOAT:
        case XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
          out_color = float4(
              XeFloat7e3To32((packed32 >> 0u) & 0x3FFu),
              XeFloat7e3To32((packed32 >> 10u) & 0x3FFu),
              XeFloat7e3To32((packed32 >> 20u) & 0x3FFu),
              float((packed32 >> 30u) & 0x3u) * (1.0f / 3.0f));
#endif
        } break;
        case XE_FMT_16_16:
        case XE_FMT_16_16_FLOAT: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32 & 0xFFFFu, packed32 >> 16u, 0u, 0u);
#else
          out_color = float4(float(packed32 & 0xFFFFu),
                             float(packed32 >> 16u), 0.0f, 0.0f);
#endif
        } break;
        case XE_FMT_32_FLOAT: {
#if XE_TRANSFER_DEST_IS_UINT
          out_color = uint4(packed32, 0u, 0u, 0u);
#else
          out_color = float4(as_type<float>(packed32), 0.0f, 0.0f, 0.0f);
#endif
        } break;
        default:
          break;
      }
    }
  }

  TransferColorOut out;
#if XE_TRANSFER_DEST_COMPONENTS == 1
  out.color = out_color.x;
#elif XE_TRANSFER_DEST_COMPONENTS == 2
  out.color = out_color.xy;
#else
  out.color = out_color;
#endif
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  out.sample_mask = 1u << dest_sample_id;
#endif
  return out;
}
#elif XE_TRANSFER_OUTPUT_DEPTH || XE_TRANSFER_OUTPUT_STENCIL_BIT
struct TransferDepthOut {
#if XE_TRANSFER_OUTPUT_STENCIL_BIT && XE_TRANSFER_NATIVE_STENCIL_OUTPUT
  uint stencil [[stencil]];
#else
  float depth [[depth(any)]];
#endif
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  uint sample_mask [[sample_mask]];
#endif
};

fragment TransferDepthOut transfer_ps(
    VSOut in [[stage_in]],
    constant TransferShaderConstants& constants [[buffer(0)]]
    XE_TRANSFER_HOST_DEPTH_BUFFER_PARAM
    XE_TRANSFER_SOURCE_PARAMS
    XE_TRANSFER_HOST_DEPTH_TEXTURE_PARAM
    XE_TRANSFER_SAMPLE_ID_PARAM) {
  uint2 dest_pixel = uint2(in.position.xy);
  uint dest_sample_id = 0u;
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  #if XE_TRANSFER_DEST_SAMPLE_ID_FROM_SAMPLE
    dest_sample_id = xe_sample_id;
  #else
    dest_sample_id = constants.dest_sample_id;
  #endif
#endif

  uint tile_width_samples = constants.tile_width_samples;
  uint tile_height_samples = constants.tile_height_samples;
  uint dest_tile_width_pixels = constants.dest_tile_width_pixels;
  uint dest_tile_height_pixels = constants.dest_tile_height_pixels;

  uint dest_tile_pixel_x = 0u;
  uint dest_tile_pixel_y = 0u;
  uint dest_tile_index = 0u;
  uint dest_tile_index_x = 0u;
  uint dest_tile_index_y = 0u;
#if XE_TRANSFER_FAST_DIVMOD
  XeFastDivMod(dest_pixel.x, dest_tile_width_pixels,
               constants.dest_tile_width_pixels_inv, dest_tile_index_x,
               dest_tile_pixel_x);
  XeFastDivMod(dest_pixel.y, dest_tile_height_pixels,
               constants.dest_tile_height_pixels_inv, dest_tile_index_y,
               dest_tile_pixel_y);
#else
  dest_tile_index_x = dest_pixel.x / dest_tile_width_pixels;
  dest_tile_pixel_x = dest_pixel.x % dest_tile_width_pixels;
  dest_tile_index_y = dest_pixel.y / dest_tile_height_pixels;
  dest_tile_pixel_y = dest_pixel.y % dest_tile_height_pixels;
#endif

  dest_tile_index =
      dest_tile_index_x +
      dest_tile_index_y * constants.address.dest_pitch;

  uint source_sample_id = dest_sample_id;
  // [effects-native] mixed-scale transfers map through the source's scale.
  uint source_tile_width_samples = constants.source_tile_width_samples != 0u
      ? constants.source_tile_width_samples : tile_width_samples;
  uint source_tile_height_samples = constants.source_tile_height_samples != 0u
      ? constants.source_tile_height_samples : tile_height_samples;
  uint xfer_tile_pixel_x = XeTransferScalePixel(
      dest_tile_pixel_x, constants.source_scale_x, constants.dest_scale_x);
  uint xfer_tile_pixel_y = XeTransferScalePixel(
      dest_tile_pixel_y, constants.source_scale_y, constants.dest_scale_y);
  uint source_tile_pixel_x = xfer_tile_pixel_x;
  uint source_tile_pixel_y = xfer_tile_pixel_y;

  bool source_is_64bpp = XE_TRANSFER_SOURCE_IS_64BPP != 0u;
  bool dest_is_64bpp = XE_TRANSFER_DEST_IS_64BPP != 0u;
  uint source_msaa = XE_TRANSFER_SOURCE_MSAA_SAMPLES;
  uint dest_msaa = XE_TRANSFER_DEST_MSAA_SAMPLES;
  bool msaa_2x_supported = constants.msaa_2x_supported != 0u;

  if (!source_is_64bpp && dest_is_64bpp) {
    if (source_msaa >= 4u) {
      if (dest_msaa >= 4u) {
        source_sample_id = dest_sample_id & 2u;
        source_tile_pixel_x =
            XeBitFieldInsert(dest_sample_id, xfer_tile_pixel_x, 1u, 31u);
      } else if (dest_msaa == 2u) {
        if (msaa_2x_supported) {
          source_sample_id = (dest_sample_id ^ 1u) << 1u;
        } else {
          source_sample_id = dest_sample_id & 2u;
        }
      } else {
        source_sample_id =
            XeBitFieldInsert(0u, xfer_tile_pixel_y, 1u, 1u);
        source_tile_pixel_y = xfer_tile_pixel_y >> 1u;
      }
    } else {
      if (dest_msaa >= 4u) {
        source_tile_pixel_x = XeBitFieldInsert(
            xfer_tile_pixel_x << 2u, dest_sample_id, 1u, 1u);
      } else {
        source_tile_pixel_x = xfer_tile_pixel_x << 1u;
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    if (dest_msaa >= 4u) {
      if (source_msaa >= 4u) {
        source_sample_id =
            XeBitFieldInsert(dest_sample_id, xfer_tile_pixel_x, 0u, 1u);
        source_tile_pixel_x = xfer_tile_pixel_x >> 1u;
      }
    } else {
      if (source_msaa >= 4u) {
        source_sample_id = XeBitFieldExtract(xfer_tile_pixel_x, 1u, 1u);
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            source_sample_id = XeBitFieldInsert(
                source_sample_id, dest_sample_id ^ 1u, 1u, 1u);
          } else {
            source_sample_id = XeBitFieldInsert(
                dest_sample_id, source_sample_id, 0u, 1u);
          }
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, xfer_tile_pixel_y, 1u, 1u);
          source_tile_pixel_y = xfer_tile_pixel_y >> 1u;
        }
        source_tile_pixel_x = xfer_tile_pixel_x >> 2u;
      } else {
        source_tile_pixel_x = xfer_tile_pixel_x >> 1u;
      }
    }
  } else {
    if (source_msaa != dest_msaa) {
      if (source_msaa >= 4u) {
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            source_sample_id = XeBitFieldInsert(
                xfer_tile_pixel_x, dest_sample_id ^ 1u, 1u, 31u);
          } else {
            source_sample_id = XeBitFieldInsert(
                dest_sample_id, xfer_tile_pixel_x, 0u, 1u);
          }
          source_tile_pixel_x = xfer_tile_pixel_x >> 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              xfer_tile_pixel_x & 1u, xfer_tile_pixel_y, 1u, 1u);
          source_tile_pixel_x = xfer_tile_pixel_x >> 1u;
          source_tile_pixel_y = xfer_tile_pixel_y >> 1u;
        }
      } else if (dest_msaa >= 4u) {
        source_tile_pixel_x = XeBitFieldInsert(
            dest_sample_id, xfer_tile_pixel_x, 1u, 31u);
      }
    }
  }

  if (source_msaa < 4u && source_msaa != dest_msaa) {
    if (dest_msaa >= 4u) {
      if (source_msaa == 2u) {
        source_sample_id = dest_sample_id >> 1u;
        if (msaa_2x_supported) {
          source_sample_id ^= 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, source_sample_id, 1u, 1u);
        }
      } else {
        source_tile_pixel_y = XeBitFieldInsert(
            dest_sample_id >> 1u, xfer_tile_pixel_y, 1u, 31u);
      }
    } else {
      if (source_msaa == 2u) {
        source_sample_id = xfer_tile_pixel_y & 1u;
        if (msaa_2x_supported) {
          source_sample_id ^= 1u;
        } else {
          source_sample_id = XeBitFieldInsert(
              source_sample_id, source_sample_id, 1u, 1u);
        }
        source_tile_pixel_y = xfer_tile_pixel_y >> 1u;
      } else {
        if (msaa_2x_supported) {
          source_tile_pixel_y = XeBitFieldInsert(
              dest_sample_id ^ 1u, xfer_tile_pixel_y, 1u, 31u);
        } else {
          source_tile_pixel_y = XeBitFieldInsert(
              dest_sample_id >> 1u, xfer_tile_pixel_y, 1u, 31u);
        }
      }
    }
  }

  uint source_pixel_width_dwords_log2 =
      (source_msaa >= 4u ? 1u : 0u) + (source_is_64bpp ? 1u : 0u);

  if ((XE_TRANSFER_SOURCE_IS_DEPTH != 0u) != (XE_TRANSFER_DEST_IS_DEPTH != 0u)) {
    uint source_32bpp_tile_half_pixels =
        source_tile_width_samples >> (1u + source_pixel_width_dwords_log2);
    if (source_tile_pixel_x < source_32bpp_tile_half_pixels) {
      source_tile_pixel_x += source_32bpp_tile_half_pixels;
    } else {
      source_tile_pixel_x -= source_32bpp_tile_half_pixels;
    }
  }

  uint source_pixel_x = 0u;
  uint source_pixel_y = 0u;
  uint source_tile_index =
      uint(int(dest_tile_index) + constants.address.source_to_dest) &
      (kEdramTileCount - 1u);
  uint source_pitch_tiles = constants.address.source_pitch;
  uint source_tile_index_y = 0u;
  uint source_tile_index_x = 0u;
  XeFastDivMod(source_tile_index, source_pitch_tiles,
               constants.source_pitch_tiles_inv, source_tile_index_y,
               source_tile_index_x);
  source_pixel_x =
      source_tile_index_x *
          (source_tile_width_samples >> source_pixel_width_dwords_log2) +
      source_tile_pixel_x;
  source_pixel_y =
      source_tile_index_y *
          (source_tile_height_samples >> (source_msaa >= 2u ? 1u : 0u)) +
      source_tile_pixel_y;

  bool load_two = !source_is_64bpp && dest_is_64bpp;
  uint source_pixel_x1 = source_pixel_x;
  uint source_sample_id1 = source_sample_id;
  if (load_two) {
    if (source_msaa >= 4u) {
      source_sample_id1 = source_sample_id | 1u;
    } else {
      source_pixel_x1 = source_pixel_x | 1u;
    }
  }

#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
  uint4 source_color0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y),
                              source_sample_id);
#else
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y));
#endif
  uint4 source_color1 = source_color0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1);
#else
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y));
#endif
  }
  #else
  float4 source_color0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y),
                              source_sample_id);
#else
      xe_transfer_source.read(uint2(source_pixel_x, source_pixel_y));
#endif
  float4 source_color1 = source_color0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1);
#else
    source_color1 = xe_transfer_source.read(
        uint2(source_pixel_x1, source_pixel_y));
#endif
  }
#if !XE_TRANSFER_SOURCE_IS_MULTISAMPLE
  // [effects-native] scaled -> native-scale colour (same MSAA and pixel size):
  // box-filter the block of source pixels instead of point sampling it, so
  // thin features keep their coverage (as when the game renders at 1x).
  if (!load_two && source_msaa == 1u && dest_msaa == 1u &&
      source_is_64bpp == dest_is_64bpp &&
      constants.dest_scale_x != 0u && constants.dest_scale_y != 0u &&
      constants.source_scale_x > constants.dest_scale_x &&
      constants.source_scale_y >= constants.dest_scale_y) {
    uint box_x = constants.source_scale_x / constants.dest_scale_x;
    uint box_y = max(constants.source_scale_y / constants.dest_scale_y, 1u);
    uint x0 = source_pixel_x - min(source_pixel_x, box_x >> 1u);
    uint y0 = source_pixel_y - min(source_pixel_y, box_y >> 1u);
    float4 sum = float4(0.0f);
    for (uint by = 0u; by < box_y; ++by) {
      for (uint bx = 0u; bx < box_x; ++bx) {
        sum += xe_transfer_source.read(uint2(x0 + bx, y0 + by));
      }
    }
    source_color0 = sum / float(box_x * box_y);
    source_color1 = source_color0;
  }
#endif
  #endif
#else
#if XE_TRANSFER_SOURCE_NEEDS_DEPTH
  float source_depth0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source_depth.read(uint2(source_pixel_x, source_pixel_y),
                                    source_sample_id).r;
#else
      xe_transfer_source_depth.read(uint2(source_pixel_x, source_pixel_y)).r;
#endif
  float source_depth1 = source_depth0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_depth1 = xe_transfer_source_depth.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1).r;
#else
    source_depth1 = xe_transfer_source_depth.read(
        uint2(source_pixel_x1, source_pixel_y)).r;
#endif
  }
#endif
#if XE_TRANSFER_SOURCE_NEEDS_STENCIL
  uint source_stencil0 =
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
      xe_transfer_source_stencil.read(uint2(source_pixel_x, source_pixel_y),
                                      source_sample_id).r;
#else
      xe_transfer_source_stencil.read(uint2(source_pixel_x, source_pixel_y)).r;
#endif
  uint source_stencil1 = source_stencil0;
  if (load_two) {
#if XE_TRANSFER_SOURCE_IS_MULTISAMPLE
    source_stencil1 = xe_transfer_source_stencil.read(
        uint2(source_pixel_x1, source_pixel_y), source_sample_id1).r;
#else
    source_stencil1 = xe_transfer_source_stencil.read(
        uint2(source_pixel_x1, source_pixel_y)).r;
#endif
  }
#else
  uint source_stencil0 = 0u;
  uint source_stencil1 = 0u;
#endif
#endif

  uint packed = 0u;
#if !XE_TRANSFER_OUTPUT_STENCIL_BIT
  bool packed_only_depth = false;
#endif
#if XE_TRANSFER_SOURCE_IS_COLOR
  #if XE_TRANSFER_SOURCE_IS_UINT
  switch (XE_TRANSFER_SOURCE_FORMAT) {
    case XE_FMT_16_16:
    case XE_FMT_16_16_FLOAT:
    case XE_FMT_16_16_16_16:
    case XE_FMT_16_16_16_16_FLOAT:
      packed = source_color0[0] | (source_color0[1] << 16u);
      break;
    case XE_FMT_32_FLOAT:
    case XE_FMT_32_32_FLOAT:
      packed = source_color0[0];
      break;
    default:
      packed = source_color0[0];
      break;
  }
  #else
  switch (XE_TRANSFER_SOURCE_FORMAT) {
    case XE_FMT_8_8_8_8:
    case XE_FMT_8_8_8_8_GAMMA: {
      float4 color = source_color0;
      if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_8_8_8_8_GAMMA) {
#if XE_GAMMA_RT_AS_UNORM16 || XE_TRANSFER_OUTPUT_STENCIL_BIT
        color.rgb = XeLinearToPWLGamma3(color.rgb);
#endif
      }
      // Match D3D12: color -> depth transfers derive guest depth from bits
      // 8:31 of the packed 32bpp color word rather than treating depth as a
      // standalone component.
      packed = XePackColorRGBA8(color);
    } break;
    case XE_FMT_2_10_10_10:
    case XE_FMT_2_10_10_10_AS_10_10_10_10: {
      packed = XePackColorRGB10A2(source_color0);
    } break;
    case XE_FMT_2_10_10_10_FLOAT:
    case XE_FMT_2_10_10_10_FLOAT_AS_16_16_16_16: {
      packed = XePackColorRGB10A2Float(source_color0);
    } break;
    case XE_FMT_32_FLOAT:
    case XE_FMT_32_32_FLOAT:
      packed = as_type<uint>(source_color0[0]);
      break;
    default:
      packed = as_type<uint>(source_color0[0]);
      break;
  }
  #endif
#else
#if XE_TRANSFER_OUTPUT_STENCIL_BIT
  packed = source_stencil0;
#else
  if (XE_TRANSFER_SOURCE_FORMAT == XE_FMT_D24FS8) {
    bool round_depth = constants.depth_round != 0u;
    packed = XeFloat32To20e4(source_depth0 * 2.0f, round_depth);
  } else {
    packed = XeRoundToNearestEven(
        clamp(source_depth0, 0.0f, 1.0f) * 16777215.0f);
  }
  if (XE_TRANSFER_DEST_IS_DEPTH != 0u) {
#if !XE_TRANSFER_OUTPUT_STENCIL_BIT
    packed_only_depth = true;
#endif
  } else {
    packed = (packed << 8u) | (source_stencil0 & 0xFFu);
  }
#endif
#endif

#if XE_TRANSFER_OUTPUT_STENCIL_BIT
#if XE_TRANSFER_NATIVE_STENCIL_OUTPUT
  TransferDepthOut out;
  out.stencil = packed & 0xFFu;
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  out.sample_mask = 1u << dest_sample_id;
#endif
  return out;
#else
  if (constants.stencil_clear == 0u) {
    if ((packed & constants.stencil_mask) == 0u) {
      discard_fragment();
    }
  }
  TransferDepthOut out;
  out.depth = 0.0f;
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  out.sample_mask = 1u << dest_sample_id;
#endif
  return out;
#endif
#else
  uint guest_depth24 = packed;
  if (!packed_only_depth) {
    guest_depth24 = packed >> 8u;
  }

  float host_depth32 = 0.0f;
  bool has_host_depth = false;

#if XE_TRANSFER_HAS_HOST_DEPTH
  if (constants.host_depth_source_is_copy == 0u) {
    uint host_tile_pixel_x = dest_tile_pixel_x;
    uint host_tile_pixel_y = dest_tile_pixel_y;
    uint host_sample_id = dest_sample_id;
    uint host_msaa = XE_TRANSFER_HOST_DEPTH_MSAA_SAMPLES;

    if (host_msaa != dest_msaa) {
      if (host_msaa >= 4u) {
        if (dest_msaa == 2u) {
          if (msaa_2x_supported) {
            host_sample_id = XeBitFieldInsert(
                dest_tile_pixel_x, dest_sample_id ^ 1u, 1u, 31u);
          } else {
            host_sample_id = XeBitFieldInsert(
                dest_sample_id, dest_tile_pixel_x, 0u, 1u);
          }
          host_tile_pixel_x = dest_tile_pixel_x >> 1u;
        } else {
          host_sample_id = XeBitFieldInsert(
              dest_tile_pixel_x & 1u, dest_tile_pixel_y, 1u, 1u);
          host_tile_pixel_x = dest_tile_pixel_x >> 1u;
          host_tile_pixel_y = dest_tile_pixel_y >> 1u;
        }
      } else if (dest_msaa >= 4u) {
        host_tile_pixel_x = XeBitFieldInsert(
            dest_sample_id, dest_tile_pixel_x, 1u, 31u);
      }

      if (host_msaa < 4u) {
        if (dest_msaa >= 4u) {
          if (host_msaa == 2u) {
            host_sample_id = dest_sample_id >> 1u;
            if (msaa_2x_supported) {
              host_sample_id ^= 1u;
            } else {
              host_sample_id = XeBitFieldInsert(
                  host_sample_id, host_sample_id, 1u, 1u);
            }
          } else {
            host_tile_pixel_y = XeBitFieldInsert(
                dest_sample_id >> 1u, dest_tile_pixel_y, 1u, 31u);
          }
        } else {
          if (host_msaa == 2u) {
            host_sample_id = dest_tile_pixel_y & 1u;
            if (msaa_2x_supported) {
              host_sample_id ^= 1u;
            } else {
              host_sample_id = XeBitFieldInsert(
                  host_sample_id, host_sample_id, 1u, 1u);
            }
            host_tile_pixel_y = dest_tile_pixel_y >> 1u;
          } else {
            if (msaa_2x_supported) {
              host_tile_pixel_y = XeBitFieldInsert(
                  dest_sample_id ^ 1u, dest_tile_pixel_y, 1u, 31u);
            } else {
              host_tile_pixel_y = XeBitFieldInsert(
                  dest_sample_id >> 1u, dest_tile_pixel_y, 1u, 31u);
            }
          }
        }
      }
    }

    uint host_pixel_x = 0u;
    uint host_pixel_y = 0u;
    uint host_tile_index =
        uint(int(dest_tile_index) +
             constants.host_depth_address.source_to_dest) &
        (kEdramTileCount - 1u);
    uint host_pitch_tiles = constants.host_depth_address.source_pitch;
    uint host_tile_index_y = 0u;
    uint host_tile_index_x = 0u;
    XeFastDivMod(host_tile_index, host_pitch_tiles,
                 constants.host_depth_source_pitch_tiles_inv,
                 host_tile_index_y, host_tile_index_x);
    host_pixel_x =
        host_tile_index_x *
            (tile_width_samples >> (host_msaa >= 4u ? 1u : 0u)) +
        host_tile_pixel_x;
    host_pixel_y =
        host_tile_index_y *
            (tile_height_samples >> (host_msaa >= 2u ? 1u : 0u)) +
        host_tile_pixel_y;

#if XE_TRANSFER_HOST_DEPTH_IS_MULTISAMPLE
    host_depth32 = xe_transfer_host_depth.read(
        uint2(host_pixel_x, host_pixel_y), host_sample_id).r;
#else
    host_depth32 =
        xe_transfer_host_depth.read(uint2(host_pixel_x, host_pixel_y)).r;
#endif
  } else {
    uint dest_tile_sample_x = dest_tile_pixel_x;
    uint dest_tile_sample_y = dest_tile_pixel_y;
    if (dest_msaa >= 2u) {
      if (dest_msaa >= 4u) {
        dest_tile_sample_x = XeBitFieldInsert(
            dest_sample_id, dest_tile_pixel_x, 1u, 31u);
      }
      uint vert_sample = 0u;
      if (dest_msaa == 2u && msaa_2x_supported) {
        vert_sample = dest_sample_id ^ 1u;
      } else {
        vert_sample = dest_sample_id >> 1u;
      }
      dest_tile_sample_y = XeBitFieldInsert(
          vert_sample, dest_tile_pixel_y, 1u, 31u);
    }
    uint host_depth_offset =
        (tile_width_samples * tile_height_samples) * dest_tile_index +
        tile_width_samples * dest_tile_sample_y + dest_tile_sample_x;
    host_depth32 =
        as_type<float>(xe_transfer_host_depth_buffer[host_depth_offset]);
  }
  has_host_depth = true;
#endif

  float fragment_depth = 0.0f;
  if (XE_TRANSFER_DEST_FORMAT == XE_FMT_D24FS8) {
    float guest_depth32 = XeFloat20e4To32(guest_depth24, true);
    fragment_depth = guest_depth32;
  } else {
    fragment_depth = XeUnorm24To32(guest_depth24);
  }

  if (has_host_depth) {
    uint host_depth24 = 0u;
    if (XE_TRANSFER_DEST_FORMAT == XE_FMT_D24FS8) {
      bool round_depth = constants.depth_round != 0u;
      host_depth24 = XeFloat32To20e4(host_depth32 * 2.0f, round_depth);
    } else {
      host_depth24 = XeRoundToNearestEven(
          clamp(host_depth32, 0.0f, 1.0f) * 16777215.0f);
    }
    if (host_depth24 == guest_depth24) {
      fragment_depth = host_depth32;
    }
  }

  TransferDepthOut out;
  out.depth = fragment_depth;
#if XE_TRANSFER_DEST_IS_MULTISAMPLE
  out.sample_mask = 1u << dest_sample_id;
#endif
  return out;
#endif
}
#endif
)METAL";

  source.append(kTransferShaderSource);
  source.insert(0, MslSalt());

  NS::Error* error = nullptr;
  auto src_str = NS::String::string(source.c_str(), NS::UTF8StringEncoding);
  MTL::CompileOptions* compile_options = MTL::CompileOptions::alloc()->init();
  compile_options->setFastMathEnabled(true);
  compile_options->setLanguageVersion(MTL::LanguageVersion2_4);
  MTL::Library* lib = device_->newLibrary(src_str, compile_options, &error);
  compile_options->release();
  if (!lib) {
    REXLOG_ERROR(
        "GetOrCreateTransferPipelines: failed to compile transfer MSL "
        "(mode={}): "
        "{}",
        int(key.mode),
        error && error->localizedDescription()
            ? error->localizedDescription()->utf8String()
            : "unknown error");
    return nullptr;
  }

  auto vs_name = NS::String::string("transfer_rect_vs", NS::UTF8StringEncoding);
  auto ps_name = NS::String::string("transfer_ps", NS::UTF8StringEncoding);
  MTL::Function* vs = lib->newFunction(vs_name);
  MTL::Function* ps = lib->newFunction(ps_name);
  if (!vs || !ps) {
    REXLOG_ERROR("GetOrCreateTransferPipelines: failed to get transfer shader entry");
    if (vs) {
      vs->release();
    }
    if (ps) {
      ps->release();
    }
    lib->release();
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs);
  desc->setFragmentFunction(ps);

  if (output == TransferOutput::kColor) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      auto* color_attachment = desc->colorAttachments()->object(i);
      color_attachment->setPixelFormat(
          pipeline_key.color_attachment_formats[i]);
      color_attachment->setWriteMask(i == pipeline_key.color_attachment_index
                                         ? MTL::ColorWriteMaskAll
                                         : MTL::ColorWriteMaskNone);
    }
    desc->setDepthAttachmentPixelFormat(pipeline_key.depth_attachment_format);
    desc->setStencilAttachmentPixelFormat(
        pipeline_key.stencil_attachment_format);
  } else {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      auto* color_attachment = desc->colorAttachments()->object(i);
      color_attachment->setPixelFormat(
          pipeline_key.color_attachment_formats[i]);
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
    }
    desc->setDepthAttachmentPixelFormat(pipeline_key.depth_attachment_format);
    desc->setStencilAttachmentPixelFormat(
        pipeline_key.stencil_attachment_format);
  }

  uint32_t sample_count = 1;
  if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
    sample_count = 2;
  } else if (key.dest_msaa_samples == xenos::MsaaSamples::k4X) {
    sample_count = 4;
  }
  desc->setSampleCount(sample_count);

  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);

  desc->release();
  vs->release();
  ps->release();
  lib->release();

  if (!pipeline) {
    REXLOG_ERROR(
        "GetOrCreateTransferPipelines: failed to create pipeline (mode={}): {}",
        int(key.mode),
        error && error->localizedDescription()
            ? error->localizedDescription()->utf8String()
            : "unknown error");
    return nullptr;
  }

  return pipeline;
}

// ---------------------------------------------------------------------------
// [xfer-prewarm] Ownership-transfer pipelines built before the draws need them.
//
// Each transfer key compiles its own MSL source and pipeline (~15 ms with a
// cold Metal system cache, ~0.1 ms warm). The first launch after a transfer
// shader change, a new Mac or an OS update compiled ~85 of them on the draw
// path: ~1.3 s of load frames and 100-130 ms gameplay hitches
// (docs/frame-pacing-20261012.md). The keys used are recorded in an
// append-only text file in the shader storage folder (the launcher only
// publishes appended or new cache files); the next launch builds them on two
// background threads right after shader storage init. A draw that needs a
// key takes the prebuilt pipeline, waits for one being built, or builds it
// itself if no thread has started it. FABLE2_XFER_PREWARM=0: off (keys are
// not recorded either).
// ---------------------------------------------------------------------------
namespace {
bool XferPrewarmEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_XFER_PREWARM");
    return !(v && v[0] == '0');
  }();
  return on;
}
constexpr char kXferKeysHeader[] = "XFERKEYS 1";
}  // namespace

struct MetalRenderTargetCache::XferPrewarm {
  enum State : uint8_t { kPending, kBuilding, kBuilt, kClaimed };
  struct Entry {
    TransferPipelineKey key;
    bool dest_is_uint = false;
    State state = kPending;
    MTL::RenderPipelineState* pipeline = nullptr;
  };
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<Entry> entries;
  std::unordered_map<TransferPipelineKey, size_t, TransferPipelineKey::Hasher>
      index;
  size_t next = 0;
  bool stop = false;
  std::vector<std::thread> threads;
  std::filesystem::path path;
  // Stats (mutex).
  uint32_t loaded = 0, built = 0, failed = 0, taken = 0, waited = 0,
           claimed = 0, recorded = 0;
  double build_ms = 0.0, wait_ms = 0.0;
  std::chrono::steady_clock::time_point start;
  bool done_logged = false;
};


void MetalRenderTargetCache::StartTransferPrewarm(
    const std::filesystem::path& title_root, uint32_t title_id) {
  if (!XferPrewarmEnabled() || xfer_prewarm_ || !device_) return;
  auto state = std::make_shared<XferPrewarm>();
  state->start = std::chrono::steady_clock::now();
  state->path = title_root / fmt::format("{:08X}.xfer-keys.txt", title_id);
  {
    // Started before shader storage init creates the folder (early keys are
    // appended right below).
    std::error_code dir_ec;
    std::filesystem::create_directories(title_root, dir_ec);
  }
  uint32_t from_default = 0;
  // The cache's own keys first (this Mac's order of first use), then the
  // built-in defaults (FABLE2_XFER_PREWARM_DEFAULTS=0: cache keys only).
  auto parse = [&](std::istream& in, bool defaults) {
    std::string line;
    {
      constexpr size_t kColors = xenos::kMaxColorRenderTargets;
      while (std::getline(in, line)) {
        std::vector<uint64_t> v;
        const char* p = line.c_str();
        char* end = nullptr;
        for (;;) {
          const unsigned long long x = std::strtoull(p, &end, 10);
          if (end == p) break;
          v.push_back(x);
          p = end;
        }
        if (v.size() != 9 + kColors + 3) continue;  // torn or foreign line
        TransferPipelineKey k = {};
        k.shader_key.mode = TransferMode(v[0]);
        if (size_t(k.shader_key.mode) >=
            sizeof(kTransferModeInfos) / sizeof(kTransferModeInfos[0])) {
          continue;
        }
        k.shader_key.source_msaa_samples = xenos::MsaaSamples(v[1]);
        k.shader_key.dest_msaa_samples = xenos::MsaaSamples(v[2]);
        k.shader_key.host_depth_source_msaa_samples = xenos::MsaaSamples(v[3]);
        k.shader_key.source_resource_format = uint32_t(v[4]);
        k.shader_key.dest_resource_format = uint32_t(v[5]);
        k.shader_key.dest_sample_id_from_sample = uint32_t(v[6]);
        k.shader_key.host_depth_source_is_copy = 0;
        k.color_attachment_index = uint32_t(v[7]);
        k.native_stencil_output = uint32_t(v[8]);
        if (k.color_attachment_index >= kColors) continue;
        for (size_t i = 0; i < kColors; ++i) {
          k.color_attachment_formats[i] = MTL::PixelFormat(v[9 + i]);
        }
        k.depth_attachment_format = MTL::PixelFormat(v[9 + kColors]);
        k.stencil_attachment_format = MTL::PixelFormat(v[10 + kColors]);
        const bool dest_is_uint = v[11 + kColors] != 0;
        if (state->index.count(k)) continue;
        state->index.emplace(k, state->entries.size());
        state->entries.push_back({k, dest_is_uint});
        from_default += defaults ? 1 : 0;
      }
    }
  };
  {
    std::ifstream in(state->path);
    std::string header;
    if (in && std::getline(in, header) && header == kXferKeysHeader) {
      parse(in, false);
    }
  }
  const char* use_defaults = std::getenv("FABLE2_XFER_PREWARM_DEFAULTS");  // getenv-ok: once, transfer-pipeline prewarm at startup
  if (!(use_defaults && use_defaults[0] == '0')) {
    std::istringstream in(kXferKeysDefault);
    parse(in, true);
  }
  state->loaded = uint32_t(state->entries.size());
  xfer_prewarm_ = state;
  // Keys built on the draw path before storage init (boot): record them now.
  for (const auto& [key, dest_is_uint] : xfer_early_keys_) {
    NoteTransferPipelineKey(key, dest_is_uint);
  }
  xfer_early_keys_.clear();
  if (state->entries.empty()) {
    REXLOG_INFO("[xfer-prewarm] no recorded transfer pipelines yet ({})",
                state->path.filename().string());
    return;
  }
  const size_t thread_count = std::min<size_t>(2, state->entries.size());
  for (size_t t = 0; t < thread_count; ++t) {
    state->threads.emplace_back([this, state, t]() {
      pthread_setname_np("Xfer Prewarm");
      pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
      if (t == 0) {
        // The shared transfer-clear library (pso_xclr on its first use).
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        GetOrCreateTransferLibrary();
        pool->release();
      }
      std::unique_lock<std::mutex> lock(state->mutex);
      for (;;) {
        while (state->next < state->entries.size() &&
               state->entries[state->next].state != XferPrewarm::kPending) {
          ++state->next;
        }
        if (state->stop || state->next >= state->entries.size()) break;
        XferPrewarm::Entry& e = state->entries[state->next++];
        e.state = XferPrewarm::kBuilding;
        const TransferPipelineKey key = e.key;
        const bool dest_is_uint = e.dest_is_uint;
        lock.unlock();
        const auto t0 = std::chrono::steady_clock::now();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        MTL::RenderPipelineState* pipeline =
            BuildTransferPipeline(key, dest_is_uint);
        pool->release();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        lock.lock();
        // `e` is stable: entries never grows after the threads start.
        e.pipeline = pipeline;
        e.state = XferPrewarm::kBuilt;
        state->build_ms += ms;
        ++(pipeline ? state->built : state->failed);
        state->cv.notify_all();
      }
      bool all_done = true;
      for (const auto& e : state->entries) {
        all_done &= e.state != XferPrewarm::kPending &&
                    e.state != XferPrewarm::kBuilding;
      }
      if (all_done && !state->done_logged && !state->stop) {
        state->done_logged = true;
        REXLOG_INFO(
            "[xfer-prewarm] {} recorded transfer pipelines: built {} in "
            "background ({:.0f} ms summed, done {:.2f} s after storage init), "
            "failed {}, already taken by draws {}",
            state->loaded, state->built, state->build_ms,
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          state->start)
                .count(),
            state->failed, state->claimed);
      }
    });
  }
  REXLOG_INFO("[xfer-prewarm] building {} transfer pipelines ({} from the "
              "built-in list) on {} background threads",
              state->entries.size(), from_default, thread_count);
}

uint32_t MetalRenderTargetCache::XferPrewarmPending() {
  // [shader-prep] transfer pipelines still queued or building.
  XferPrewarm* state = xfer_prewarm_.get();
  if (!state) return 0;
  std::lock_guard<std::mutex> lock(state->mutex);
  uint32_t pending = 0;
  for (const XferPrewarm::Entry& e : state->entries) {
    pending += e.state == XferPrewarm::kPending || e.state == XferPrewarm::kBuilding;
  }
  return pending;
}

MTL::RenderPipelineState* MetalRenderTargetCache::TakePrewarmedTransferPipeline(
    const TransferPipelineKey& pipeline_key, bool dest_is_uint) {
  XferPrewarm* state = xfer_prewarm_.get();
  if (!state) return nullptr;
  std::unique_lock<std::mutex> lock(state->mutex);
  auto it = state->index.find(pipeline_key);
  if (it == state->index.end()) return nullptr;
  XferPrewarm::Entry& e = state->entries[it->second];
  if (e.state == XferPrewarm::kPending) {
    // No thread has started it: build it here (the caller), no waiting.
    e.state = XferPrewarm::kClaimed;
    ++state->claimed;
    return nullptr;
  }
  if (e.state == XferPrewarm::kBuilding) {
    const auto t0 = std::chrono::steady_clock::now();
    state->cv.wait(lock, [&] { return e.state != XferPrewarm::kBuilding; });
    ++state->waited;
    state->wait_ms += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
  }
  if (e.state != XferPrewarm::kBuilt) return nullptr;
  MTL::RenderPipelineState* pipeline = e.pipeline;
  e.pipeline = nullptr;
  e.state = XferPrewarm::kClaimed;
  if (pipeline && e.dest_is_uint != dest_is_uint) {
    pipeline->release();  // built for another output type: build it exactly
    pipeline = nullptr;
  }
  if (pipeline) ++state->taken;
  return pipeline;
}

std::string MetalRenderTargetCache::XferKeyLine(const TransferPipelineKey& k,
                                                bool dest_is_uint) {
  std::string line = fmt::format(
      "{} {} {} {} {} {} {} {} {}", uint32_t(k.shader_key.mode),
      uint32_t(k.shader_key.source_msaa_samples),
      uint32_t(k.shader_key.dest_msaa_samples),
      uint32_t(k.shader_key.host_depth_source_msaa_samples),
      k.shader_key.source_resource_format, k.shader_key.dest_resource_format,
      k.shader_key.dest_sample_id_from_sample, k.color_attachment_index,
      k.native_stencil_output);
  for (MTL::PixelFormat f : k.color_attachment_formats) {
    line += fmt::format(" {}", uint32_t(f));
  }
  line += fmt::format(" {} {} {}\n", uint32_t(k.depth_attachment_format),
                      uint32_t(k.stencil_attachment_format),
                      dest_is_uint ? 1 : 0);
  return line;
}

void MetalRenderTargetCache::NoteTransferPipelineKey(
    const TransferPipelineKey& pipeline_key, bool dest_is_uint) {
  if (!XferPrewarmEnabled()) return;
  XferPrewarm* state = xfer_prewarm_.get();
  if (!state) {
    // Before shader storage init: recorded when the prewarm starts.
    xfer_early_keys_.emplace_back(pipeline_key, dest_is_uint);
    return;
  }
  if (state->path.empty()) return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->index.count(pipeline_key)) return;  // already in the file
    ++state->recorded;
  }
  {
    std::string line = XferKeyLine(pipeline_key, dest_is_uint);
    if (!line.empty() && line.back() == '\n') line.pop_back();
    REXLOG_INFO("[xfer-prewarm] new key (not in the prewarm list), recorded: {}",
                line);
  }
  // Append-only (the launcher publishes extensions of live cache files only).
  // Only on a draw-path build, which already cost a compile.
  std::error_code ec;
  const bool fresh = !std::filesystem::exists(state->path, ec) ||
                     std::filesystem::file_size(state->path, ec) == 0;
  if (FILE* f = std::fopen(state->path.c_str(), "ab")) {
    std::string text;
    if (fresh) {
      text = std::string(kXferKeysHeader) + "\n";
    }
    text += XferKeyLine(pipeline_key, dest_is_uint);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }
}

void MetalRenderTargetCache::StopTransferPrewarm() {
  std::shared_ptr<XferPrewarm> state = std::move(xfer_prewarm_);
  if (!state) return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->stop = true;
  }
  for (auto& t : state->threads) {
    if (t.joinable()) t.join();
  }
  REXLOG_INFO("[xfer-prewarm] at shutdown: recorded {}, built {}, taken {}, "
              "waited {} ({:.1f} ms), built by draws {}, new keys {}",
              state->loaded, state->built, state->taken, state->waited,
              state->wait_ms, state->claimed, state->recorded);
  for (auto& e : state->entries) {
    if (e.pipeline) e.pipeline->release();
    e.pipeline = nullptr;
  }
}

MTL::Library* MetalRenderTargetCache::GetOrCreateTransferLibrary() {
  std::lock_guard<std::mutex> lock(transfer_library_mutex_);
  if (transfer_library_) {
    return transfer_library_;
  }
  static const char kTransferLibrarySource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
  float4 position [[position]];
};

struct TransferClearColorFloatConstants {
  float4 color;
};

struct TransferClearColorUintConstants {
  uint4 color;
};

struct TransferClearDepthConstants {
  float4 depth;
};

vertex VSOut transfer_clear_vs(uint vid [[vertex_id]]) {
  float2 pt = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(pt * 2.0f - 1.0f, 0.0f, 1.0f);
  return out;
}

fragment float4 transfer_clear_color_float_ps(
    VSOut in [[stage_in]],
    constant TransferClearColorFloatConstants& constants [[buffer(0)]]) {
  return constants.color;
}

fragment uint4 transfer_clear_color_uint_ps(
    VSOut in [[stage_in]],
    constant TransferClearColorUintConstants& constants [[buffer(0)]]) {
  return constants.color;
}

struct TransferDepthOut {
  float depth [[depth(any)]];
};

fragment TransferDepthOut transfer_clear_depth_ps(
    VSOut in [[stage_in]],
    constant TransferClearDepthConstants& constants [[buffer(0)]]) {
  TransferDepthOut out;
  out.depth = constants.depth.x;
  return out;
}
)METAL";

  NS::Error* error = nullptr;
  const std::string salted_source = MslSalt() + kTransferLibrarySource;
  auto source_str =
      NS::String::string(salted_source.c_str(), NS::UTF8StringEncoding);
  transfer_library_ = device_->newLibrary(source_str, nullptr, &error);
  if (!transfer_library_) {
    REXLOG_ERROR("GetOrCreateTransferLibrary: failed to compile transfer library: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
  }
  return transfer_library_;
}

MTL::RenderPipelineState*
MetalRenderTargetCache::GetOrCreateTransferClearPipeline(
    MTL::PixelFormat dest_format, bool dest_is_uint, bool is_depth,
    uint32_t sample_count, uint32_t color_attachment_index,
    const TransferColorAttachmentFormats* color_attachment_formats,
    MTL::PixelFormat depth_attachment_format,
    MTL::PixelFormat stencil_attachment_format) {
  TransferClearPipelineKey key = {};
  key.sample_count = sample_count ? sample_count : 1;
  key.dest_is_uint = dest_is_uint ? 1u : 0u;
  key.is_depth = is_depth ? 1u : 0u;
  key.color_attachment_formats.fill(MTL::PixelFormatInvalid);
  if (color_attachment_formats) {
    key.color_attachment_formats = *color_attachment_formats;
  }
  if (is_depth) {
    key.depth_attachment_format =
        depth_attachment_format != MTL::PixelFormatInvalid
            ? depth_attachment_format
            : dest_format;
    if (key.depth_attachment_format != dest_format) {
      return nullptr;
    }
    if (dest_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        dest_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
      key.stencil_attachment_format =
          stencil_attachment_format != MTL::PixelFormatInvalid
              ? stencil_attachment_format
              : dest_format;
      if (key.stencil_attachment_format != dest_format) {
        return nullptr;
      }
    } else {
      key.stencil_attachment_format = stencil_attachment_format;
    }
  } else {
    if (color_attachment_index >= xenos::kMaxColorRenderTargets) {
      return nullptr;
    }
    key.color_attachment_index = color_attachment_index;
    if (!color_attachment_formats) {
      key.color_attachment_formats[color_attachment_index] = dest_format;
    }
    if (key.color_attachment_formats[color_attachment_index] != dest_format) {
      return nullptr;
    }
    key.depth_attachment_format = depth_attachment_format;
    key.stencil_attachment_format = stencil_attachment_format;
  }
  auto it = transfer_clear_pipelines_.find(key);
  if (it != transfer_clear_pipelines_.end()) {
    return it->second;
  }
  hitch::Scope hitch_scope(hitch::kPsoXclr);

  MTL::Library* lib = GetOrCreateTransferLibrary();
  if (!lib) {
    return nullptr;
  }

  auto vs_name =
      NS::String::string("transfer_clear_vs", NS::UTF8StringEncoding);
  const char* ps_name_cstr = nullptr;
  if (is_depth) {
    ps_name_cstr = "transfer_clear_depth_ps";
  } else {
    ps_name_cstr = dest_is_uint ? "transfer_clear_color_uint_ps"
                                : "transfer_clear_color_float_ps";
  }
  auto ps_name = NS::String::string(ps_name_cstr, NS::UTF8StringEncoding);

  MTL::Function* vs = lib->newFunction(vs_name);
  MTL::Function* ps = lib->newFunction(ps_name);
  if (!vs || !ps) {
    REXLOG_ERROR(
        "GetOrCreateTransferClearPipeline: missing transfer clear functions");
    if (vs) {
      vs->release();
    }
    if (ps) {
      ps->release();
    }
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs);
  desc->setFragmentFunction(ps);
  desc->setSampleCount(key.sample_count);

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    color_attachment->setPixelFormat(key.color_attachment_formats[i]);
    color_attachment->setWriteMask(!is_depth && i == key.color_attachment_index
                                       ? MTL::ColorWriteMaskAll
                                       : MTL::ColorWriteMaskNone);
  }
  desc->setDepthAttachmentPixelFormat(key.depth_attachment_format);
  desc->setStencilAttachmentPixelFormat(key.stencil_attachment_format);

  NS::Error* error = nullptr;
  auto create_start = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);
  // [deferred-clears] runtime pipeline cost (not in the binary archive).
  REXLOG_INFO(
      "[rt-clear-pipeline] created in {} us (depth {}, samples {}, colour "
      "index {}, colour0 fmt {}, depth fmt {})",
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - create_start)
          .count(),
      key.is_depth, key.sample_count, key.color_attachment_index,
      uint32_t(key.color_attachment_formats[0]),
      uint32_t(key.depth_attachment_format));

  desc->release();
  vs->release();
  ps->release();

  if (!pipeline) {
    REXLOG_ERROR("GetOrCreateTransferClearPipeline: failed to create pipeline: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
    return nullptr;
  }

  transfer_clear_pipelines_.emplace(key, pipeline);
  return pipeline;
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyTexture(
    MTL::PixelFormat format, uint32_t sample_count) {
  if (!device_) {
    return nullptr;
  }
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(1);
  desc->setHeight(1);
  desc->setPixelFormat(format);
  desc->setTextureType(sample_count > 1 ? MTL::TextureType2DMultisample
                                        : MTL::TextureType2D);
  desc->setSampleCount(sample_count ? sample_count : 1);
  MTL::TextureUsage usage = MTL::TextureUsageShaderRead;
  if (format == MTL::PixelFormatDepth32Float_Stencil8 ||
      format == MTL::PixelFormatDepth24Unorm_Stencil8) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);
  desc->setStorageMode(MTL::StorageModePrivate);
  MTL::Texture* tex = nullptr;
  if (render_target_heap_pool_) {
    tex = render_target_heap_pool_->CreateTexture(desc);
  }
  if (!tex) {
    tex = device_->newTexture(desc);
  }
  desc->release();
  return tex;
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyColorFloatTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_color_float_[index]) {
    transfer_dummy_color_float_[index] =
        GetTransferDummyTexture(MTL::PixelFormatRGBA8Unorm, sample_count);
  }
  return transfer_dummy_color_float_[index];
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyColorUintTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_color_uint_[index]) {
    transfer_dummy_color_uint_[index] =
        GetTransferDummyTexture(MTL::PixelFormatRGBA8Uint, sample_count);
  }
  return transfer_dummy_color_uint_[index];
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyDepthTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_depth_[index]) {
    transfer_dummy_depth_[index] = GetTransferDummyTexture(
        MTL::PixelFormatDepth32Float_Stencil8, sample_count);
  }
  return transfer_dummy_depth_[index];
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyStencilTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_stencil_[index]) {
    MTL::Texture* depth_tex = GetTransferDummyDepthTexture(sample_count);
    if (!depth_tex) {
      return nullptr;
    }
    transfer_dummy_stencil_[index] =
        depth_tex->newTextureView(MTL::PixelFormatX32_Stencil8);
  }
  return transfer_dummy_stencil_[index];
}

MTL::Buffer* MetalRenderTargetCache::GetTransferDummyBuffer() {
  if (!transfer_dummy_buffer_ && device_) {
    transfer_dummy_buffer_ =
        device_->newBuffer(sizeof(uint32_t), MTL::ResourceStorageModeShared);
    if (transfer_dummy_buffer_) {
    }
    if (transfer_dummy_buffer_) {
      std::memset(transfer_dummy_buffer_->contents(), 0, sizeof(uint32_t));
    }
  }
  return transfer_dummy_buffer_;
}

MTL::DepthStencilState* MetalRenderTargetCache::BuildTransferDepthStencilState(
    MTL::CompareFunction depth_compare, bool depth_write, bool stencil_enable,
    uint32_t stencil_write_mask) {
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(depth_compare);
  desc->setDepthWriteEnabled(depth_write);
  if (stencil_enable) {
    MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
    stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
    stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
    stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
    stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
    stencil->setReadMask(0xFF);
    stencil->setWriteMask(stencil_write_mask);
    desc->setFrontFaceStencil(stencil);
    desc->setBackFaceStencil(stencil);
    stencil->release();
  }
  MTL::DepthStencilState* state = device_->newDepthStencilState(desc);
  desc->release();
  return state;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferDepthStencilState(
    bool depth_write) {
  if (!transfer_depth_state_) {
    transfer_depth_state_ = BuildTransferDepthStencilState(
        REXCVAR_GET(depth_transfer_not_equal_test) ? MTL::CompareFunctionNotEqual
                                               : MTL::CompareFunctionAlways,
        depth_write, /*stencil_enable=*/false, 0);
  }
  return transfer_depth_state_;
}

MTL::DepthStencilState*
MetalRenderTargetCache::GetTransferNoDepthStencilState() {
  if (!transfer_depth_state_none_) {
    transfer_depth_state_none_ = BuildTransferDepthStencilState(
        MTL::CompareFunctionAlways, /*depth_write=*/false,
        /*stencil_enable=*/false, 0);
  }
  return transfer_depth_state_none_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferDepthClearState() {
  if (!transfer_depth_clear_state_) {
    transfer_depth_clear_state_ = BuildTransferDepthStencilState(
        MTL::CompareFunctionAlways, /*depth_write=*/true,
        /*stencil_enable=*/true, 0xFF);
  }
  return transfer_depth_clear_state_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferStencilClearState() {
  if (!transfer_stencil_clear_state_) {
    transfer_stencil_clear_state_ = BuildTransferDepthStencilState(
        MTL::CompareFunctionAlways, /*depth_write=*/false,
        /*stencil_enable=*/true, 0xFF);
  }
  return transfer_stencil_clear_state_;
}

MTL::DepthStencilState*
MetalRenderTargetCache::GetTransferStencilOutputState() {
  if (!transfer_stencil_output_state_) {
    transfer_stencil_output_state_ = BuildTransferDepthStencilState(
        MTL::CompareFunctionAlways, /*depth_write=*/false,
        /*stencil_enable=*/true, 0xFF);
  }
  return transfer_stencil_output_state_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferStencilBitState(
    uint32_t bit) {
  if (bit >= 8) {
    return nullptr;
  }
  if (!transfer_stencil_bit_states_[bit]) {
    transfer_stencil_bit_states_[bit] = BuildTransferDepthStencilState(
        MTL::CompareFunctionAlways, /*depth_write=*/false,
        /*stencil_enable=*/true, uint32_t(1) << bit);
  }
  return transfer_stencil_bit_states_[bit];
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
