#include "rex/graphics/metal/metal_hitch.h"
#include "rex/graphics/metal/metal_converter_census.h"
#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_startup_parallel.h"
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_pipeline_cache.h"
#include "rex/graphics/metal/metal_air_optimizer.h"
#include "rex/graphics/metal/metal_ns_specialize.h"
#include "rex/graphics/metal/metal_msl_translator.h"
#include "rex/graphics/metal/metal_shader_bank.h"

#include <dispatch/dispatch.h>
#include <sys/sysctl.h>
#include <pthread/qos.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/metal-cpp/Foundation/NSProcessInfo.hpp"
#include "third_party/metal-cpp/Foundation/NSURL.hpp"

#include "fmt/format.h"
#include "rex/assert.h"
#include "rex/cvar.h"
#include "rex/filesystem.h"
#include "rex/logging.h"
#include "rex/math.h"
#include "rex/thread.h"
#include "rex/hash.h"
#include "rex/graphics/util/draw.h"
#include "rex/graphics/flags.h"
#include "rex/graphics/metal/metal_shader_cache.h"
#include "rex/graphics/registers.h"
#include "rex/graphics/xenos.h"

#include "rex/graphics/metal/metal_shader_converter.h"
using BYTE = uint8_t;
#include "rex/graphics/metal/d3d12_5_1_bytecode/adaptive_quad_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/adaptive_triangle_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/continuous_quad_1cp_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/continuous_quad_4cp_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/continuous_triangle_1cp_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/continuous_triangle_3cp_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/discrete_quad_1cp_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/discrete_quad_4cp_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/discrete_triangle_1cp_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/discrete_triangle_3cp_hs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/tessellation_adaptive_vs.h"
#include "rex/graphics/metal/d3d12_5_1_bytecode/tessellation_indexed_vs.h"
// Metal IR Converter Runtime - defines IRDescriptorTableEntry and bind points.
#ifndef IR_RUNTIME_METALCPP
#define IR_RUNTIME_METALCPP
#endif
#include "third_party/metal-shader-converter/include/metal_irconverter_runtime.h"

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

DECLARE_bool(async_shader_compilation);
DECLARE_bool(depth_float24_convert_in_pixel_shader);
DECLARE_bool(depth_float24_round);

DEFINE_int32(metal_pipeline_creation_threads, -1,
             "Number of threads for background pipeline compilation. "
             "-1 = auto (75% of cores), 0 = disabled (synchronous).",
             "Metal");

DEFINE_int32(metal_pipeline_binary_archive_save_interval, 30,
             "Seconds between background saves of the Metal pipeline binary "
             "archive while new pipelines are being added. It is also saved "
             "when the startup warm-up ends. 0 disables saving it while "
             "running (it is then only written by a renderer shutdown, which "
             "the app normally skips). Requires "
             "metal_pipeline_binary_archive.",
             "Metal");

DEFINE_bool(metal_geometry_pipeline_failure_backoff, true,
            "Delay repeated failed geometry-pipeline creation attempts by "
            "1 to 30 seconds. Disable to compare with the original behavior.",
            "Metal");

// Read by the native MSL translator (FABLE2_NATIVE_XLATE) to mirror the DXBC
// translator, which defines them.
REXCVAR_DECLARE(bool, use_fuzzy_alpha_epsilon);
REXCVAR_DECLARE(bool, draw_resolution_scaled_texture_offsets);

namespace rex {
namespace graphics {
namespace metal {

namespace {

void AtomicMax(std::atomic<uint64_t>& target, uint64_t value) {
  uint64_t current = target.load(std::memory_order_relaxed);
  while (current < value && !target.compare_exchange_weak(
                                current, value, std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
  }
}

// CR-05 (FABLE2_ARCHIVE_SAFE_SAVE; docs/cr-fixes-20261007.md). A Metal binary
// archive save that fails ("cannot pack ... expecting 'fragment' stage in
// pipeline no. N") used to be retried for the rest of the session with the
// same poisoned in-memory archive, so nothing new was persisted.
//   1 (default): after a failed save, start a new segment (the failed
//      segment's unsaved pipelines compile again next launch).
//   2: also never add a pipeline without a fragment function. Measured
//      over-broad: the 22 fragment-less pipelines of a normal warm-up
//      serialize fine, and skipping them adds 22 misses to the next launch
//      after a cold cache, so this is not the default.
//   0: old behaviour.
int ArchiveSafeSaveMode() {
  static const int mode = [] {
    const char* v = std::getenv("FABLE2_ARCHIVE_SAFE_SAVE");
    const int m = !v ? 1 : (v[0] == '0' ? 0 : (v[0] == '2' ? 2 : 1));
    REXLOG_INFO("[archive-safe-save] mode {} (FABLE2_ARCHIVE_SAFE_SAVE={}): {}", m, v ? v : "unset",
                m == 0 ? "off"
                : m == 1 ? "a failed archive save starts a new segment"
                         : "new segment after a failed save; fragment-less pipelines are not added");
    return m;
  }();
  return mode;
}
bool ArchiveSafeSaveEnabled() { return ArchiveSafeSaveMode() != 0; }

// Trace-only test hook (docs/bug-patterns.md #16).
// FABLE2_ARCHIVE_TEST_FAIL_SAVE=1: every save of the first archive object
// that is saved fails (simulates a poisoned archive).
bool ArchiveTestFailSave() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_ARCHIVE_TEST_FAIL_SAVE");
    return v && v[0] == '1';
  }();
  return on;
}
std::atomic<void*> g_archive_test_poisoned{nullptr};

std::atomic<uint64_t> g_archive_skip_no_fragment{0};

// FABLE2_NATIVE_GEOM_PERSIST (default on, docs/bug-patterns.md row 102,
// docs/no-converter.md): native point/rectangle/tessellation pipelines
// (built on the draw path in no-converter / native-first builds) are recorded
// in the pipeline list, so the next launch prewarms them in the background
// (PrewarmNativeGeometry), and are looked up in / added to the binary archive.
// =0: the old behaviour (draw-path build every launch, OS cache only).
bool NativeGeomPersistEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_NATIVE_GEOM_PERSIST");
    const bool enabled = !(v && v[0] == '0');
    REXLOG_INFO("[native-geom-persist] {} (FABLE2_NATIVE_GEOM_PERSIST={})",
                enabled ? "on: native point/rectangle/tessellation pipelines "
                          "recorded for the next launch's prewarm and archived"
                        : "off",
                v ? v : "unset");
    return enabled;
  }();
  return on;
}

// FABLE2_NS2_ARCHIVE (default on, docs/native-shaders.md "Specialized pipeline
// archive"): pipelines with a specialized pixel shader are saved to the
// ".ns2" archive set a few seconds after the last one was added, and every
// dirty archive set is saved when the window closes (the app hard-exits
// without the renderer shutdown). =0: only the periodic save, as before.
bool Ns2ArchiveEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_NS2_ARCHIVE");
    const bool enabled = !(v && v[0] == '0');
    REXLOG_INFO("[ns2-archive] {} (FABLE2_NS2_ARCHIVE={})",
                enabled ? "on: settle save of specialized pipelines, flush "
                          "at window close"
                        : "off: periodic archive saves only",
                v ? v : "unset");
    return enabled;
  }();
  return on;
}
int64_t Ns2SettleNs() {
  static const int64_t ns = [] {
    const char* v = std::getenv("FABLE2_NS2_ARCHIVE_SETTLE_MS");
    long ms = v && v[0] ? std::strtol(v, nullptr, 10) : 10000;
    if (ms < 0) ms = 0;
    return int64_t(ms) * 1000000;
  }();
  return ns;
}
int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
uint64_t ThreadCpuUs() {
  return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1000;
}
// The cache whose archives the window-close flush saves (one per process).
std::atomic<MetalPipelineCache*> g_archive_flush_cache{nullptr};

// ".../X.seg000007.binarchive" or ".seg000007.full.binarchive" ->
// ".../X.seg000008.binarchive" (never ".full": a fresh segment after a failed
// compacting save must not supersede the older segments).
std::filesystem::path NextArchiveSegmentPath(const std::filesystem::path& path) {
  std::string name = path.filename().string();
  const size_t seg = name.rfind(".seg");
  if (seg == std::string::npos) {
    return path.string() + ".next";
  }
  size_t digits = seg + 4;
  size_t end = digits;
  while (end < name.size() && std::isdigit(static_cast<unsigned char>(name[end]))) {
    ++end;
  }
  if (end == digits) {
    return path.string() + ".next";
  }
  const uint32_t index = uint32_t(std::stoul(name.substr(digits, end - digits)));
  std::string next = name.substr(0, seg) +
                     fmt::format(".seg{:06}.binarchive", index + 1);
  return path.parent_path() / next;
}

uint64_t ElapsedMs(std::chrono::steady_clock::time_point start,
                   std::chrono::steady_clock::time_point end) {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
          .count());
}

void LogMetalErrorDetails(const char* label, NS::Error* error) {
  if (!error) {
    return;
  }
  const char* desc = error->localizedDescription()
                         ? error->localizedDescription()->utf8String()
                         : nullptr;
  const char* failure = error->localizedFailureReason()
                            ? error->localizedFailureReason()->utf8String()
                            : nullptr;
  const char* recovery =
      error->localizedRecoverySuggestion()
          ? error->localizedRecoverySuggestion()->utf8String()
          : nullptr;
  const char* domain =
      error->domain() ? error->domain()->utf8String() : nullptr;
  int64_t code = error->code();
  REXLOG_ERROR("{}: domain={} code={} desc='{}' failure='{}' recovery='{}'", label,
         domain ? domain : "<null>", code, desc ? desc : "<null>",
         failure ? failure : "<null>", recovery ? recovery : "<null>");
  NS::Dictionary* user_info = error->userInfo();
  if (user_info) {
    auto* info_desc = user_info->description();
    REXLOG_ERROR("{}: userInfo={}", label,
           info_desc ? info_desc->utf8String() : "<null>");
  }
}

// FABLE2_NATIVE_TESS_DUMP=<dir> (diagnostic): the converter's tessellation
// emulation stages (vertex + stage-in, hull, domain metallibs) and a JSON of
// each pipeline's config, for tools/shaderlab/tess_diff.mm. Game-derived:
// keep the folder under ignored out/.
const char* NativeTessDumpDir() {
  static const char* dir = [] {
    const char* v = std::getenv("FABLE2_NATIVE_TESS_DUMP");
    return (v && v[0]) ? v : nullptr;
  }();
  return dir;
}

void NativeTessDumpFile(const std::string& name, const std::vector<uint8_t>& bytes) {
  const char* dir = NativeTessDumpDir();
  if (!dir || bytes.empty()) {
    return;
  }
  std::filesystem::path path = std::filesystem::path(dir) / name;
  if (std::filesystem::exists(path)) {
    return;
  }
  if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
  }
}

class ScopedAutoreleasePool {
 public:
  ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}
  ~ScopedAutoreleasePool() {
    if (pool_) {
      pool_->drain();
    }
  }

 private:
  NS::AutoreleasePool* pool_ = nullptr;
};

void EnsureDepthFormatForDepthWritingFragment(const char* pipeline_name,
                                              bool fragment_writes_depth,
                                              MTL::PixelFormat* depth_format) {
  if (!fragment_writes_depth || !depth_format ||
      *depth_format != MTL::PixelFormatInvalid) {
    return;
  }
  *depth_format = MTL::PixelFormatDepth32Float;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_WARN(
        "{}: fragment writes depth without a bound depth attachment; "
        "using Depth32Float pipeline fallback",
        pipeline_name);
  }
}

bool ShaderUsesVertexFetch(const Shader& shader) {
  if (!shader.vertex_bindings().empty()) {
    return true;
  }
  const Shader::ConstantRegisterMap& constant_map =
      shader.constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map.vertex_fetch_bitmap); ++i) {
    if (constant_map.vertex_fetch_bitmap[i] != 0) {
      return true;
    }
  }
  return false;
}

MTL::CompareFunction ToMetalCompareFunction(xenos::CompareFunction compare) {
  static const MTL::CompareFunction kCompareMap[8] = {
      MTL::CompareFunctionNever,         // 0
      MTL::CompareFunctionLess,          // 1
      MTL::CompareFunctionEqual,         // 2
      MTL::CompareFunctionLessEqual,     // 3
      MTL::CompareFunctionGreater,       // 4
      MTL::CompareFunctionNotEqual,      // 5
      MTL::CompareFunctionGreaterEqual,  // 6
      MTL::CompareFunctionAlways,        // 7
  };
  return kCompareMap[uint32_t(compare) & 0x7];
}

MTL::StencilOperation ToMetalStencilOperation(xenos::StencilOp op) {
  static const MTL::StencilOperation kStencilOpMap[8] = {
      MTL::StencilOperationKeep,            // 0
      MTL::StencilOperationZero,            // 1
      MTL::StencilOperationReplace,         // 2
      MTL::StencilOperationIncrementClamp,  // 3
      MTL::StencilOperationDecrementClamp,  // 4
      MTL::StencilOperationInvert,          // 5
      MTL::StencilOperationIncrementWrap,   // 6
      MTL::StencilOperationDecrementWrap,   // 7
  };
  return kStencilOpMap[uint32_t(op) & 0x7];
}

MTL::ColorWriteMask ToMetalColorWriteMask(uint32_t write_mask) {
  MTL::ColorWriteMask mtl_mask = MTL::ColorWriteMaskNone;
  if (write_mask & 0x1) {
    mtl_mask |= MTL::ColorWriteMaskRed;
  }
  if (write_mask & 0x2) {
    mtl_mask |= MTL::ColorWriteMaskGreen;
  }
  if (write_mask & 0x4) {
    mtl_mask |= MTL::ColorWriteMaskBlue;
  }
  if (write_mask & 0x8) {
    mtl_mask |= MTL::ColorWriteMaskAlpha;
  }
  return mtl_mask;
}

MTL::BlendOperation ToMetalBlendOperation(xenos::BlendOp blend_op) {
  static const MTL::BlendOperation kBlendOpMap[8] = {
      MTL::BlendOperationAdd,              // 0
      MTL::BlendOperationSubtract,         // 1
      MTL::BlendOperationMin,              // 2
      MTL::BlendOperationMax,              // 3
      MTL::BlendOperationReverseSubtract,  // 4
      MTL::BlendOperationAdd,              // 5
      MTL::BlendOperationAdd,              // 6
      MTL::BlendOperationAdd,              // 7
  };
  return kBlendOpMap[uint32_t(blend_op) & 0x7];
}

MTL::BlendFactor ToMetalBlendFactorRgb(xenos::BlendFactor blend_factor) {
  static const MTL::BlendFactor kBlendFactorMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,  // ?
      /*  3 */ MTL::BlendFactorZero,  // ?
      /*  4 */ MTL::BlendFactorSourceColor,
      /*  5 */ MTL::BlendFactorOneMinusSourceColor,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationColor,
      /*  9 */ MTL::BlendFactorOneMinusDestinationColor,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendColor,  // CONSTANT_COLOR
      /* 13 */ MTL::BlendFactorOneMinusBlendColor,
      /* 14 */ MTL::BlendFactorBlendAlpha,  // CONSTANT_ALPHA
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorMap[uint32_t(blend_factor) & 0x1F];
}

MTL::BlendFactor ToMetalBlendFactorAlpha(xenos::BlendFactor blend_factor) {
  static const MTL::BlendFactor kBlendFactorAlphaMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,  // ?
      /*  3 */ MTL::BlendFactorZero,  // ?
      /*  4 */ MTL::BlendFactorSourceAlpha,
      /*  5 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationAlpha,
      /*  9 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendAlpha,
      /* 13 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 14 */ MTL::BlendFactorBlendAlpha,
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorAlphaMap[uint32_t(blend_factor) & 0x1F];
}

void ApplyBlendStateToDescriptor(
    MTL::RenderPipelineColorAttachmentDescriptorArray* color_attachments,
    uint32_t normalized_color_mask, const uint32_t blendcontrol[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = color_attachments->object(i);
    if (color_attachment->pixelFormat() == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    uint32_t rt_write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    reg::RB_BLENDCONTROL bc = {};
    bc.value = blendcontrol[i];
    MTL::BlendFactor src_rgb = ToMetalBlendFactorRgb(bc.color_srcblend);
    MTL::BlendFactor dst_rgb = ToMetalBlendFactorRgb(bc.color_destblend);
    MTL::BlendOperation op_rgb = ToMetalBlendOperation(bc.color_comb_fcn);
    MTL::BlendFactor src_alpha = ToMetalBlendFactorAlpha(bc.alpha_srcblend);
    MTL::BlendFactor dst_alpha = ToMetalBlendFactorAlpha(bc.alpha_destblend);
    MTL::BlendOperation op_alpha = ToMetalBlendOperation(bc.alpha_comb_fcn);

    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    color_attachment->setBlendingEnabled(blending_enabled);
    if (blending_enabled) {
      color_attachment->setSourceRGBBlendFactor(src_rgb);
      color_attachment->setDestinationRGBBlendFactor(dst_rgb);
      color_attachment->setRgbBlendOperation(op_rgb);
      color_attachment->setSourceAlphaBlendFactor(src_alpha);
      color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
      color_attachment->setAlphaBlendOperation(op_alpha);
    }
  }
}

IRFormat MapVertexFormatToIRFormat(
    const ParsedVertexFetchInstruction::Attributes& attrs) {
  using xenos::VertexFormat;
  switch (attrs.data_format) {
    case VertexFormat::k_8_8_8_8:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR8G8B8A8Sint : IRFormatR8G8B8A8Uint;
      }
      return attrs.is_signed ? IRFormatR8G8B8A8Snorm : IRFormatR8G8B8A8Unorm;
    case VertexFormat::k_2_10_10_10:
      if (attrs.is_integer) {
        return IRFormatR10G10B10A2Uint;
      }
      return IRFormatR10G10B10A2Unorm;
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
      return IRFormatR11G11B10Float;
    case VertexFormat::k_16_16:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR16G16Sint : IRFormatR16G16Uint;
      }
      return attrs.is_signed ? IRFormatR16G16Snorm : IRFormatR16G16Unorm;
    case VertexFormat::k_16_16_16_16:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR16G16B16A16Sint
                               : IRFormatR16G16B16A16Uint;
      }
      return attrs.is_signed ? IRFormatR16G16B16A16Snorm
                             : IRFormatR16G16B16A16Unorm;
    case VertexFormat::k_16_16_FLOAT:
      return IRFormatR16G16Float;
    case VertexFormat::k_16_16_16_16_FLOAT:
      return IRFormatR16G16B16A16Float;
    case VertexFormat::k_32:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR32Sint : IRFormatR32Uint;
      }
      return IRFormatR32Float;
    case VertexFormat::k_32_32:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR32G32Sint : IRFormatR32G32Uint;
      }
      return IRFormatR32G32Float;
    case VertexFormat::k_32_FLOAT:
      return IRFormatR32Float;
    case VertexFormat::k_32_32_FLOAT:
      return IRFormatR32G32Float;
    case VertexFormat::k_32_32_32_FLOAT:
      return IRFormatR32G32B32Float;
    case VertexFormat::k_32_32_32_32:
      if (attrs.is_integer) {
        return attrs.is_signed ? IRFormatR32G32B32A32Sint
                               : IRFormatR32G32B32A32Uint;
      }
      return IRFormatR32G32B32A32Float;
    case VertexFormat::k_32_32_32_32_FLOAT:
      return IRFormatR32G32B32A32Float;
    default:
      return IRFormatUnknown;
  }
}

IRInputTopology GetGeometryStageInputTopology(GeometryShaderKey key) {
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      return IRInputTopologyPoint;
    case PipelineGeometryShader::kRectangleList:
      return IRInputTopologyTriangle;
    case PipelineGeometryShader::kQuadList:
    default:
      return IRInputTopologyUndefined;
  }
}

struct StageInInputAttribute {
  uint32_t input_slot = 0;
  uint32_t offset = 0;
  IRFormat format = IRFormatUnknown;
};

struct StageInInputLayout {
  IRVersionedInputLayoutDescriptor descriptor = {};
  std::vector<std::string> semantic_names_storage;
};

StageInInputLayout BuildStageInInputLayout(
    const Shader& vertex_shader,
    const MetalShaderReflectionInfo& vertex_reflection,
    const char* log_prefix) {
  std::vector<StageInInputAttribute> attribute_map;
  attribute_map.reserve(32);

  uint32_t attr_index = 0;
  for (const auto& binding : vertex_shader.vertex_bindings()) {
    for (const auto& attr : binding.attributes) {
      if (attr_index >= 31) {
        break;
      }
      StageInInputAttribute mapped = {};
      mapped.input_slot = static_cast<uint32_t>(binding.binding_index);
      mapped.offset =
          static_cast<uint32_t>(attr.fetch_instr.attributes.offset * 4);
      mapped.format = MapVertexFormatToIRFormat(attr.fetch_instr.attributes);
      attribute_map.push_back(mapped);
      ++attr_index;
    }
    if (attr_index >= 31) {
      break;
    }
  }

  StageInInputLayout input_layout = {};
  input_layout.descriptor.version = IRInputLayoutDescriptorVersion_1;
  input_layout.descriptor.desc_1_0.numElements = 0;
  if (vertex_reflection.vertex_inputs.empty()) {
    return input_layout;
  }

  input_layout.semantic_names_storage.reserve(
      vertex_reflection.vertex_inputs.size());
  uint32_t element_count = 0;
  for (const auto& input : vertex_reflection.vertex_inputs) {
    if (element_count >= 31) {
      break;
    }
    if (input.attribute_index >= attribute_map.size()) {
      REXLOG_WARN("{}: vertex input {} out of range (max {})", log_prefix,
             input.attribute_index, attribute_map.size());
      continue;
    }
    const StageInInputAttribute& mapped = attribute_map[input.attribute_index];
    if (mapped.format == IRFormatUnknown) {
      REXLOG_WARN("{}: unknown IRFormat for vertex input {}", log_prefix,
             input.attribute_index);
      continue;
    }

    std::string semantic_base = input.name;
    uint32_t semantic_index = 0;
    if (!semantic_base.empty()) {
      size_t digit_pos = semantic_base.size();
      while (digit_pos > 0 && std::isdigit(static_cast<unsigned char>(
                                  semantic_base[digit_pos - 1]))) {
        --digit_pos;
      }
      if (digit_pos < semantic_base.size()) {
        semantic_index = static_cast<uint32_t>(
            std::strtoul(semantic_base.c_str() + digit_pos, nullptr, 10));
        semantic_base.resize(digit_pos);
      }
    }
    if (semantic_base.empty()) {
      semantic_base = "TEXCOORD";
    }

    input_layout.semantic_names_storage.push_back(std::move(semantic_base));
    input_layout.descriptor.desc_1_0.semanticNames[element_count] =
        input_layout.semantic_names_storage.back().c_str();
    IRInputElementDescriptor1& element =
        input_layout.descriptor.desc_1_0.inputElementDescs[element_count];
    element.semanticIndex = semantic_index;
    element.format = mapped.format;
    element.inputSlot = mapped.input_slot;
    element.alignedByteOffset = mapped.offset;
    element.instanceDataStepRate = 0;
    element.inputSlotClass = IRInputClassificationPerVertexData;
    ++element_count;
  }
  input_layout.descriptor.desc_1_0.numElements = element_count;
  return input_layout;
}

constexpr uint32_t kMetalPipelineStorageMagic = 0x4C544D58;  // 'XMTL'
constexpr uint32_t kGeneratedStageGeometryVertex = 0x100;
constexpr uint32_t kGeneratedStageGeometryShader = 0x101;
constexpr uint32_t kGeneratedStageTessellationVertex = 0x200;
constexpr uint32_t kGeneratedStageTessellationHull = 0x201;
constexpr uint32_t kGeneratedStageTessellationDomain = 0x202;

}  // namespace

class MetalPipelineCache::GeneratedStageCache {
 public:
  struct GeometryVertexStageState {
    MTL::Library* library = nullptr;
    MTL::Library* stage_in_library = nullptr;
    std::string function_name;
    uint32_t vertex_output_size_in_bytes = 0;
  };

  struct GeometryShaderStageState {
    MTL::Library* library = nullptr;
    std::string function_name;
    uint32_t max_input_primitives_per_mesh_threadgroup = 0;
    std::vector<MetalShaderFunctionConstant> function_constants;
  };

  struct TessellationVertexStageState {
    MTL::Library* library = nullptr;
    MTL::Library* stage_in_library = nullptr;
    std::string function_name;
    uint32_t vertex_output_size_in_bytes = 0;
  };

  struct TessellationHullStageState {
    MTL::Library* library = nullptr;
    std::string function_name;
    MetalShaderReflectionInfo reflection;
  };

  struct TessellationDomainStageState {
    MTL::Library* library = nullptr;
    std::string function_name;
    MetalShaderReflectionInfo reflection;
  };

  GeneratedStageCache(MetalPipelineCache& owner, MTL::Device* device,
                      DxbcToDxilConverter& dxbc_to_dxil_converter,
                      MetalShaderConverter& metal_shader_converter)
      : owner_(owner),
        device_(device),
        dxbc_to_dxil_converter_(dxbc_to_dxil_converter),
        metal_shader_converter_(metal_shader_converter) {}

  ~GeneratedStageCache() {
    for (auto& pair : geometry_vertex_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
      if (pair.second.stage_in_library) {
        pair.second.stage_in_library->release();
      }
    }
    for (auto& pair : geometry_shader_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
    }
    for (auto& pair : tessellation_vertex_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
      if (pair.second.stage_in_library) {
        pair.second.stage_in_library->release();
      }
    }
    for (auto& pair : tessellation_hull_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
    }
    for (auto& pair : tessellation_domain_stage_cache_) {
      if (pair.second.library) {
        pair.second.library->release();
      }
    }
  }

  std::shared_ptr<const MetalStageCompileResult> CompileStage(
      MetalShaderStage stage, std::shared_ptr<const std::vector<uint8_t>> dxil,
      bool enable_geometry_emulation, int input_topology,
      const IRVersionedInputLayoutDescriptor* input_layout = nullptr,
      uint64_t stage_in_layout_key = 0,
      std::function<const IRVersionedInputLayoutDescriptor*(
          const MetalShaderReflectionInfo&)>
          stage_in_layout_builder = nullptr) {
    MetalStageCompileRequest request;
    request.stage = stage;
    request.dxil_data = std::move(dxil);
    request.enable_geometry_emulation = enable_geometry_emulation;
    request.input_topology = input_topology;
    request.input_layout = input_layout;
    request.stage_in_layout_key = stage_in_layout_key;
    request.stage_in_layout_builder = std::move(stage_in_layout_builder);
    request.requested_outputs = kMetalStageCompileOutputMetallib;
    if (input_layout || request.stage_in_layout_builder) {
      request.requested_outputs |= kMetalStageCompileOutputStageInMetallib;
    }
    return metal_shader_converter_.CompileStage(request);
  }

  GeometryVertexStageState* GetGeometryVertexStage(
      MetalShader::MetalTranslation* vertex_translation,
      GeometryShaderKey geometry_shader_key) {
    auto vertex_it = geometry_vertex_stage_cache_.find(vertex_translation);
    if (vertex_it != geometry_vertex_stage_cache_.end()) {
      return &vertex_it->second;
    }

    converter_census::Scope census_scope(
        converter_census::kGeomVS,
        vertex_translation->shader().ucode_data_hash());
    if (!owner_.EnsureDxilTranslationReady(vertex_translation,
                                           "geometry vertex")) {
      REXLOG_ERROR("Geometry VS: failed to create a valid DXIL translation");
      return nullptr;
    }
    std::vector<uint8_t> dxil_data = vertex_translation->GetDxilDataCopy();

    IRInputTopology input_topology =
        GetGeometryStageInputTopology(geometry_shader_key);
    auto dxil = std::make_shared<std::vector<uint8_t>>(std::move(dxil_data));
    const uint64_t stage_in_key_data[] = {
        vertex_translation->shader().ucode_data_hash(),
        vertex_translation->modification(), uint64_t(geometry_shader_key.key)};
    uint64_t stage_in_layout_key =
        XXH3_64bits(stage_in_key_data, sizeof(stage_in_key_data));
    auto input_layout = std::make_shared<StageInInputLayout>();
    std::shared_ptr<const MetalStageCompileResult> vertex_result = CompileStage(
        MetalShaderStage::kVertex, dxil, true, static_cast<int>(input_topology),
        nullptr, stage_in_layout_key,
        [input_layout,
         vertex_translation](const MetalShaderReflectionInfo& reflection)
            -> const IRVersionedInputLayoutDescriptor* {
          *input_layout = BuildStageInInputLayout(vertex_translation->shader(),
                                                  reflection, "Geometry VS");
          return &input_layout->descriptor;
        });
    if (!vertex_result || !vertex_result->success) {
      REXLOG_ERROR("Geometry VS: DXIL to Metal conversion failed: {}",
             vertex_result ? vertex_result->error_message : "unknown error");
      return nullptr;
    }
    const MetalShaderReflectionInfo& vertex_reflection =
        vertex_result->reflection;
    if (vertex_result->stage_in_metallib.empty()) {
      REXLOG_ERROR(
          "Geometry VS: Failed to synthesize stage-in function "
          "(vertex_inputs={}, output_size={})",
          vertex_reflection.vertex_input_count,
          vertex_reflection.vertex_output_size_in_bytes);
      return nullptr;
    }

    if (NativeTessDumpDir()) {
      const std::string base = fmt::format(
          "geomvs_{:016X}_{:X}_k{:08X}", vertex_translation->shader().ucode_data_hash(),
          vertex_translation->modification(), geometry_shader_key.key);
      NativeTessDumpFile(base + ".metallib", vertex_result->metallib_data);
      NativeTessDumpFile(base + ".stagein.metallib",
                         vertex_result->stage_in_metallib);
    }
    MTL::Library* vertex_library =
        owner_.NewLibraryFromBytes(vertex_result->metallib_data, "Geometry VS");
    if (!vertex_library) {
      return nullptr;
    }
    MTL::Library* stage_in_library = owner_.NewLibraryFromBytes(
        vertex_result->stage_in_metallib, "Geometry VS stage-in");
    if (!stage_in_library) {
      vertex_library->release();
      return nullptr;
    }

    GeometryVertexStageState state;
    state.library = vertex_library;
    state.stage_in_library = stage_in_library;
    state.function_name = vertex_result->function_name;
    state.vertex_output_size_in_bytes =
        vertex_reflection.vertex_output_size_in_bytes;
    if (state.vertex_output_size_in_bytes == 0) {
      REXLOG_ERROR(
          "Geometry VS: reflection returned zero output size "
          "(vertex_inputs={})",
          vertex_reflection.vertex_input_count);
    }

    auto [inserted_it, inserted] = geometry_vertex_stage_cache_.emplace(
        vertex_translation, std::move(state));
    return &inserted_it->second;
  }

  GeometryShaderStageState* GetGeometryShaderStage(
      GeometryShaderKey geometry_shader_key) {
    auto geom_it = geometry_shader_stage_cache_.find(geometry_shader_key);
    if (geom_it != geometry_shader_stage_cache_.end()) {
      return &geom_it->second;
    }

    converter_census::Scope census_scope(converter_census::kGeomGS,
                                         geometry_shader_key.key);
    const std::vector<uint32_t>& dxbc_dwords =
        GetGeometryShader(geometry_shader_key);
    std::vector<uint8_t> dxbc_bytes(dxbc_dwords.size() * sizeof(uint32_t));
    std::memcpy(dxbc_bytes.data(), dxbc_dwords.data(), dxbc_bytes.size());

    std::vector<uint8_t> dxil_data;
    std::string dxil_error;
    if (!owner_.ConvertDxbcToDxil(dxbc_bytes, dxil_data, &dxil_error)) {
      REXLOG_ERROR("Geometry GS: DXBC to DXIL conversion failed: {}", dxil_error);
      return nullptr;
    }

    IRInputTopology input_topology =
        GetGeometryStageInputTopology(geometry_shader_key);
    std::shared_ptr<const MetalStageCompileResult> geometry_result =
        CompileStage(
            MetalShaderStage::kGeometry,
            std::make_shared<std::vector<uint8_t>>(std::move(dxil_data)), true,
            static_cast<int>(input_topology));
    if (!geometry_result || !geometry_result->success) {
      REXLOG_ERROR(
          "Geometry GS: DXIL to Metal conversion failed: {}",
          geometry_result ? geometry_result->error_message : "unknown error");
      return nullptr;
    }
    const MetalShaderReflectionInfo& geometry_reflection =
        geometry_result->reflection;
    if (!geometry_result->has_mesh_stage &&
        !geometry_result->has_geometry_stage) {
      REXLOG_ERROR(
          "Geometry GS: MSC did not emit mesh or geometry stage (mesh={}, "
          "geometry={})",
          geometry_result->has_mesh_stage, geometry_result->has_geometry_stage);
      return nullptr;
    }
    if (!geometry_result->has_mesh_stage) {
      static bool mesh_missing_logged = false;
      if (!mesh_missing_logged) {
        mesh_missing_logged = true;
        REXLOG_WARN(
            "Geometry GS: MSC did not emit mesh stage; using geometry stage "
            "library");
      }
    }

    NativeTessDumpFile(fmt::format("gs_k{:08X}.metallib", geometry_shader_key.key),
                       geometry_result->metallib_data);
    MTL::Library* geometry_library = owner_.NewLibraryFromBytes(
        geometry_result->metallib_data, "Geometry GS");
    if (!geometry_library) {
      return nullptr;
    }

    GeometryShaderStageState state;
    state.library = geometry_library;
    state.function_name = geometry_result->function_name;
    state.max_input_primitives_per_mesh_threadgroup =
        geometry_reflection.gs_max_input_primitives_per_mesh_threadgroup;
    state.function_constants = geometry_reflection.function_constants;
    if (state.max_input_primitives_per_mesh_threadgroup == 0) {
      REXLOG_ERROR("Geometry GS: reflection returned zero max input primitives");
    }

    auto [inserted_it, inserted] = geometry_shader_stage_cache_.emplace(
        geometry_shader_key, std::move(state));
    return &inserted_it->second;
  }

  TessellationVertexStageState* GetTessellationVertexStage(
      MetalShader::MetalTranslation* domain_translation,
      xenos::TessellationMode tessellation_mode) {
    struct VertexStageKey {
      const void* shader;
      uint32_t tessellation_mode;
    } vertex_key = {domain_translation, uint32_t(tessellation_mode)};
    uint64_t vertex_key_hash = XXH3_64bits(&vertex_key, sizeof(vertex_key));
    auto vertex_it = tessellation_vertex_stage_cache_.find(vertex_key_hash);
    if (vertex_it != tessellation_vertex_stage_cache_.end()) {
      return &vertex_it->second;
    }

    const uint64_t stable_vertex_key[] = {
        domain_translation->shader().ucode_data_hash(),
        domain_translation->modification(), uint64_t(tessellation_mode)};
    uint64_t stage_in_layout_key =
        XXH3_64bits(stable_vertex_key, sizeof(stable_vertex_key));

    converter_census::Scope census_scope(
        converter_census::kTessVS,
        domain_translation->shader().ucode_data_hash());
    const uint8_t* vs_bytes = nullptr;
    size_t vs_size = 0;
    if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
      vs_bytes = ::tessellation_adaptive_vs;
      vs_size = sizeof(::tessellation_adaptive_vs);
    } else {
      vs_bytes = ::tessellation_indexed_vs;
      vs_size = sizeof(::tessellation_indexed_vs);
    }
    std::vector<uint8_t> dxbc_bytes(vs_bytes, vs_bytes + vs_size);
    std::vector<uint8_t> dxil_data;
    std::string dxil_error;
    if (!owner_.ConvertDxbcToDxil(dxbc_bytes, dxil_data, &dxil_error)) {
      REXLOG_ERROR("Tessellation VS: DXBC to DXIL conversion failed: {}", dxil_error);
      return nullptr;
    }

    auto dxil = std::make_shared<std::vector<uint8_t>>(std::move(dxil_data));
    auto input_layout = std::make_shared<StageInInputLayout>();
    std::shared_ptr<const MetalStageCompileResult> vertex_result = CompileStage(
        MetalShaderStage::kVertex, dxil, true,
        static_cast<int>(IRInputTopologyUndefined), nullptr,
        stage_in_layout_key,
        [input_layout,
         domain_translation](const MetalShaderReflectionInfo& reflection)
            -> const IRVersionedInputLayoutDescriptor* {
          *input_layout = BuildStageInInputLayout(
              domain_translation->shader(), reflection, "Tessellation VS");
          return &input_layout->descriptor;
        });
    if (!vertex_result || !vertex_result->success) {
      REXLOG_ERROR("Tessellation VS: DXIL to Metal conversion failed: {}",
             vertex_result ? vertex_result->error_message : "unknown error");
      return nullptr;
    }
    const MetalShaderReflectionInfo& vertex_reflection =
        vertex_result->reflection;
    if (vertex_result->stage_in_metallib.empty()) {
      REXLOG_ERROR("Tessellation VS: Failed to synthesize stage-in function");
      return nullptr;
    }
    if (NativeTessDumpDir()) {
      const std::string base = fmt::format(
          "tessvs_{:016X}_{:X}_m{}", domain_translation->shader().ucode_data_hash(),
          domain_translation->modification(), uint32_t(tessellation_mode));
      NativeTessDumpFile(base + ".metallib", vertex_result->metallib_data);
      NativeTessDumpFile(base + ".stagein.metallib",
                         vertex_result->stage_in_metallib);
    }

    MTL::Library* vertex_library = owner_.NewLibraryFromBytes(
        vertex_result->metallib_data, "Tessellation VS");
    if (!vertex_library) {
      return nullptr;
    }
    MTL::Library* stage_in_library = owner_.NewLibraryFromBytes(
        vertex_result->stage_in_metallib, "Tessellation VS stage-in");
    if (!stage_in_library) {
      vertex_library->release();
      return nullptr;
    }

    TessellationVertexStageState state;
    state.library = vertex_library;
    state.stage_in_library = stage_in_library;
    state.function_name = vertex_result->function_name;
    state.vertex_output_size_in_bytes =
        vertex_reflection.vertex_output_size_in_bytes;
    if (state.vertex_output_size_in_bytes == 0) {
      REXLOG_ERROR("Tessellation VS: reflection returned zero output size");
    }

    auto [inserted_it, inserted] = tessellation_vertex_stage_cache_.emplace(
        vertex_key_hash, std::move(state));
    return &inserted_it->second;
  }

  TessellationHullStageState* GetTessellationHullStage(
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      xenos::TessellationMode tessellation_mode) {
    struct HullStageKey {
      uint32_t host_vs_type;
      uint32_t tessellation_mode;
    } hull_key = {uint32_t(primitive_processing_result.host_vertex_shader_type),
                  uint32_t(tessellation_mode)};
    uint64_t hull_key_hash = XXH3_64bits(&hull_key, sizeof(hull_key));
    auto hull_it = tessellation_hull_stage_cache_.find(hull_key_hash);
    converter_census::Scope census_scope(converter_census::kTessHS,
                                         hull_key_hash);
    if (hull_it != tessellation_hull_stage_cache_.end()) {
      return &hull_it->second;
    }

    const uint8_t* hs_bytes = nullptr;
    size_t hs_size = 0;
    switch (tessellation_mode) {
      case xenos::TessellationMode::kDiscrete:
        switch (primitive_processing_result.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            hs_bytes = ::discrete_triangle_3cp_hs;
            hs_size = sizeof(::discrete_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            hs_bytes = ::discrete_triangle_1cp_hs;
            hs_size = sizeof(::discrete_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            hs_bytes = ::discrete_quad_4cp_hs;
            hs_size = sizeof(::discrete_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            hs_bytes = ::discrete_quad_1cp_hs;
            hs_size = sizeof(::discrete_quad_1cp_hs);
            break;
          default:
            REXLOG_ERROR(
                "Tessellation HS: unsupported host vertex shader type {}",
                uint32_t(primitive_processing_result.host_vertex_shader_type));
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kContinuous:
        switch (primitive_processing_result.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            hs_bytes = ::continuous_triangle_3cp_hs;
            hs_size = sizeof(::continuous_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            hs_bytes = ::continuous_triangle_1cp_hs;
            hs_size = sizeof(::continuous_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            hs_bytes = ::continuous_quad_4cp_hs;
            hs_size = sizeof(::continuous_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            hs_bytes = ::continuous_quad_1cp_hs;
            hs_size = sizeof(::continuous_quad_1cp_hs);
            break;
          default:
            REXLOG_ERROR(
                "Tessellation HS: unsupported host vertex shader type {}",
                uint32_t(primitive_processing_result.host_vertex_shader_type));
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kAdaptive:
        switch (primitive_processing_result.host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            hs_bytes = ::adaptive_triangle_hs;
            hs_size = sizeof(::adaptive_triangle_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            hs_bytes = ::adaptive_quad_hs;
            hs_size = sizeof(::adaptive_quad_hs);
            break;
          default:
            REXLOG_ERROR(
                "Tessellation HS: unsupported host vertex shader type {}",
                uint32_t(primitive_processing_result.host_vertex_shader_type));
            return nullptr;
        }
        break;
      default:
        REXLOG_ERROR("Tessellation HS: unsupported tessellation mode {}",
               uint32_t(tessellation_mode));
        return nullptr;
    }

    std::vector<uint8_t> dxbc_bytes(hs_bytes, hs_bytes + hs_size);
    std::vector<uint8_t> dxil_data;
    std::string dxil_error;
    if (!owner_.ConvertDxbcToDxil(dxbc_bytes, dxil_data, &dxil_error)) {
      REXLOG_ERROR("Tessellation HS: DXBC to DXIL conversion failed: {}", dxil_error);
      return nullptr;
    }

    std::shared_ptr<const MetalStageCompileResult> hull_result = CompileStage(
        MetalShaderStage::kHull,
        std::make_shared<std::vector<uint8_t>>(std::move(dxil_data)), true,
        static_cast<int>(IRInputTopologyUndefined));
    if (!hull_result || !hull_result->success) {
      REXLOG_ERROR("Tessellation HS: DXIL to Metal conversion failed: {}",
             hull_result ? hull_result->error_message : "unknown error");
      return nullptr;
    }

    NativeTessDumpFile(
        fmt::format("hs_t{}_m{}.metallib",
                    uint32_t(primitive_processing_result.host_vertex_shader_type),
                    uint32_t(tessellation_mode)),
        hull_result->metallib_data);
    MTL::Library* hull_library = owner_.NewLibraryFromBytes(
        hull_result->metallib_data, "Tessellation HS");
    if (!hull_library) {
      return nullptr;
    }

    TessellationHullStageState state;
    state.library = hull_library;
    state.function_name = hull_result->function_name;
    state.reflection = hull_result->reflection;
    if (!state.reflection.has_hull_info) {
      REXLOG_ERROR("Tessellation HS: reflection missing hull info");
    }

    auto [inserted_it, inserted] =
        tessellation_hull_stage_cache_.emplace(hull_key_hash, std::move(state));
    return &inserted_it->second;
  }

  TessellationDomainStageState* GetTessellationDomainStage(
      MetalShader::MetalTranslation* domain_translation) {
    uint64_t domain_key =
        XXH3_64bits(&domain_translation, sizeof(domain_translation));
    auto domain_it = tessellation_domain_stage_cache_.find(domain_key);
    if (domain_it != tessellation_domain_stage_cache_.end()) {
      return &domain_it->second;
    }

    converter_census::Scope census_scope(
        converter_census::kTessDS,
        domain_translation->shader().ucode_data_hash());
    if (!owner_.EnsureDxilTranslationReady(domain_translation,
                                           "tessellation domain")) {
      REXLOG_ERROR("Tessellation DS: failed to create a valid DXIL translation");
      return nullptr;
    }
    std::vector<uint8_t> dxil_data = domain_translation->GetDxilDataCopy();

    std::shared_ptr<const MetalStageCompileResult> domain_result = CompileStage(
        MetalShaderStage::kDomain,
        std::make_shared<std::vector<uint8_t>>(std::move(dxil_data)), true,
        static_cast<int>(IRInputTopologyUndefined));
    if (!domain_result || !domain_result->success) {
      REXLOG_ERROR("Tessellation DS: DXIL to Metal conversion failed: {}",
             domain_result ? domain_result->error_message : "unknown error");
      return nullptr;
    }

    NativeTessDumpFile(
        fmt::format("ds_{:016X}_{:X}.metallib",
                    domain_translation->shader().ucode_data_hash(),
                    domain_translation->modification()),
        domain_result->metallib_data);
    MTL::Library* domain_library = owner_.NewLibraryFromBytes(
        domain_result->metallib_data, "Tessellation DS");
    if (!domain_library) {
      return nullptr;
    }

    TessellationDomainStageState state;
    state.library = domain_library;
    state.function_name = domain_result->function_name;
    state.reflection = domain_result->reflection;
    if (!state.reflection.has_domain_info) {
      REXLOG_ERROR("Tessellation DS: reflection missing domain info");
    }

    auto [inserted_it, inserted] =
        tessellation_domain_stage_cache_.emplace(domain_key, std::move(state));
    return &inserted_it->second;
  }

 private:
  MetalPipelineCache& owner_;
  MTL::Device* device_ = nullptr;
  DxbcToDxilConverter& dxbc_to_dxil_converter_;
  MetalShaderConverter& metal_shader_converter_;

  std::unordered_map<MetalShader::MetalTranslation*, GeometryVertexStageState>
      geometry_vertex_stage_cache_;
  std::unordered_map<GeometryShaderKey, GeometryShaderStageState,
                     GeometryShaderKey::Hasher>
      geometry_shader_stage_cache_;
  std::unordered_map<uint64_t, TessellationVertexStageState>
      tessellation_vertex_stage_cache_;
  std::unordered_map<uint64_t, TessellationHullStageState>
      tessellation_hull_stage_cache_;
  std::unordered_map<uint64_t, TessellationDomainStageState>
      tessellation_domain_stage_cache_;
};

// ---------------------------------------------------------------------------
// ResolvePipelineRenderingKey (shared fixed-function key derivation)
// ---------------------------------------------------------------------------

PipelineRenderingKey ResolvePipelineRenderingKey(
    const RegisterFile& regs,
    const MetalShader::MetalTranslation* pixel_translation,
    bool use_fallback_pixel_shader) {
  PipelineRenderingKey key = {};

  uint32_t pixel_shader_writes_color_targets =
      (use_fallback_pixel_shader || !pixel_translation)
          ? 0
          : pixel_translation->shader().writes_color_targets();
  key.normalized_color_mask = pixel_shader_writes_color_targets
                                  ? draw_util::GetNormalizedColorMask(
                                        regs, pixel_shader_writes_color_targets)
                                  : 0;
  // Xenos alpha-to-mask is implemented by the translated pixel shader via
  // OMask / sample-mask output. Enabling Metal fixed-function alpha-to-coverage
  // would apply a second coverage mask on top of the shader result.
  key.alpha_to_mask_enable = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key.blendcontrol[i] = regs.Get<reg::RB_BLENDCONTROL>(
                                  reg::RB_BLENDCONTROL::rt_register_indices[i])
                              .value;
  }
  return key;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MetalPipelineCache::MetalPipelineCache(MTL::Device* device,
                                       const RegisterFile& register_file)
    : device_(device), register_file_(register_file) {}

MetalPipelineCache::~MetalPipelineCache() {
  readiness::ClearBackgroundProbe(this);
  {
    MetalPipelineCache* expected = this;
    g_archive_flush_cache.compare_exchange_strong(expected, nullptr);
  }
  // Shut down the native-shaders optimizer first: a swap may wait on a
  // pipeline the creation threads are still building.
  {
    std::lock_guard<std::mutex> lock(ns_mutex_);
    ns_shutdown_ = true;
  }
  ns_cond_.notify_all();
  for (auto& thread : ns_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  ns_threads_.clear();
  native_geom_prewarm_stop_.store(true, std::memory_order_relaxed);
  if (native_geom_prewarm_thread_.joinable()) {
    native_geom_prewarm_thread_.join();
  }
  if (bank_prep_thread_.joinable()) {
    bank_prep_thread_.join();  // [bank-prep] stops at the next item (native_geom_prewarm_stop_)
  }

  // Shut down async pipeline creation threads.
  {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    creation_threads_shutdown_ = true;
  }
  creation_request_cond_.notify_all();
  for (auto& thread : creation_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  creation_threads_.clear();
  for (auto& thread : warmup_helper_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  warmup_helper_threads_.clear();

  // Release MSC pipeline caches.
  for (auto& pair : pipeline_cache_) {
    if (pair.second) {
      auto* ps = pair.second->state.load(std::memory_order_relaxed);
      if (ps) {
        ps->release();
      }
      for (MTL::RenderPipelineState* retired : pair.second->ns_retired_states) {
        retired->release();
      }
      if (pair.second->ns_plain_state) {
        pair.second->ns_plain_state->release();
      }
      if (auto* sp = pair.second->ns_spec_state.exchange(nullptr)) {
        sp->release();
      }
    }
  }
  pipeline_cache_.clear();

  bindless_sampler_layout_map_.clear();
  bindless_sampler_layouts_.clear();
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();

  for (auto& pair : geometry_pipeline_cache_) {
    if (pair.second.pipeline) {
      pair.second.pipeline->release();
    }
    if (pair.second.ns_emu) {
      if (auto* p = pair.second.ns_emu->optimized.exchange(nullptr)) {
        p->release();
      }
      if (auto* p = pair.second.ns_emu->spec.exchange(nullptr)) {
        p->release();
      }
    }
  }
  geometry_pipeline_cache_.clear();

  for (auto& pair : tessellation_pipeline_cache_) {
    if (pair.second.pipeline) {
      pair.second.pipeline->release();
    }
    if (pair.second.ns_emu) {
      if (auto* p = pair.second.ns_emu->optimized.exchange(nullptr)) {
        p->release();
      }
      if (auto* p = pair.second.ns_emu->spec.exchange(nullptr)) {
        p->release();
      }
    }
  }
  tessellation_pipeline_cache_.clear();

  generated_stages_.reset();

  if (depth_only_pixel_function_) {
    depth_only_pixel_function_->release();
    depth_only_pixel_function_ = nullptr;
  }
  if (depth_only_pixel_library_) {
    depth_only_pixel_library_->release();
    depth_only_pixel_library_ = nullptr;
  }
  depth_only_pixel_function_name_.clear();

  ShutdownShaderStorage();

  shader_cache_.clear();
  shader_translator_.reset();
  dxbc_to_dxil_converter_.reset();
  metal_shader_converter_.reset();
}

// ---------------------------------------------------------------------------
// Shader translation initialization
// ---------------------------------------------------------------------------

bool MetalPipelineCache::InitializeShaderTranslation(
    bool gamma_render_target_as_unorm8, bool msaa_2x_supported,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y) {
  bool edram_rov_used = false;
  generated_stages_.reset();
  native_xlate_config_.gamma_render_target_as_unorm8 =
      gamma_render_target_as_unorm8;
  native_xlate_config_.msaa_2x_supported = msaa_2x_supported;
  native_xlate_config_.draw_resolution_scale_x = draw_resolution_scale_x;
  native_xlate_config_.draw_resolution_scale_y = draw_resolution_scale_y;

  REXLOG_INFO(
      "DxbcShaderTranslator init: gamma_as_unorm8={}, msaa_2x={}, scale={}x{}",
      gamma_render_target_as_unorm8, msaa_2x_supported, draw_resolution_scale_x,
      draw_resolution_scale_y);

  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      ui::GraphicsProvider::GpuVendorID::kApple, true, edram_rov_used,
      gamma_render_target_as_unorm8, msaa_2x_supported, draw_resolution_scale_x,
      draw_resolution_scale_y,
      false);  // force_emit_source_map

  // FABLE2_NO_CONVERTER builds (docs/no-converter.md): the stand-ins report
  // "unavailable" here. That is not an error at startup; a request that
  // actually needs the converter fails loudly instead (census refusal).
  dxbc_to_dxil_converter_ = std::make_unique<DxbcToDxilConverter>();
  if (!dxbc_to_dxil_converter_->Initialize() && ConverterBuiltIn()) {
    REXLOG_ERROR("Failed to initialize DXBC to DXIL converter");
    return false;
  }

  metal_shader_converter_ = std::make_unique<MetalShaderConverter>();
  if (!metal_shader_converter_->Initialize() && ConverterBuiltIn()) {
    REXLOG_ERROR("Failed to initialize Metal Shader Converter");
    return false;
  }
  if (!ConverterBuiltIn()) {
    REXLOG_INFO("[no-converter] shader converter not built: native shaders "
                "only (native first {})",
                NativeFirstMode() ? "on" : "OFF: guest shaders will fail");
  }
  generated_stages_ = std::make_unique<GeneratedStageCache>(
      *this, device_, *dxbc_to_dxil_converter_, *metal_shader_converter_);

  // Configure MSC minimum targets.
  if (device_) {
    IRGPUFamily min_family = IRGPUFamilyMetal3;
    if (device_->supportsFamily(MTL::GPUFamilyApple10)) {
      min_family = IRGPUFamilyApple10;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple9)) {
      min_family = IRGPUFamilyApple9;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple8)) {
      min_family = IRGPUFamilyApple8;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple7)) {
      min_family = IRGPUFamilyApple7;
    } else if (device_->supportsFamily(MTL::GPUFamilyApple6)) {
      min_family = IRGPUFamilyApple6;
    } else if (device_->supportsFamily(MTL::GPUFamilyMac2) ||
               device_->supportsFamily(MTL::GPUFamilyMetal4) ||
               device_->supportsFamily(MTL::GPUFamilyMetal3)) {
      min_family = IRGPUFamilyMetal3;
    }

    NS::OperatingSystemVersion os_version =
        NS::ProcessInfo::processInfo()->operatingSystemVersion();
    std::ostringstream version_stream;
    version_stream << os_version.majorVersion << "." << os_version.minorVersion
                   << "." << os_version.patchVersion;
    std::string min_version = version_stream.str();
    const IRGPUFamily native_family = min_family;
    // The converter target is pinned by default (FABLE2_MSC_MIN_TARGET,
    // default apple7:15.0; "native" = this Mac's GPU family and exact OS
    // version, the behaviour before 2026-10-12). The converter output, and so
    // every stage-compile and optimized/specialized-shader cache key, then no
    // longer depends on the Mac or its macOS version: a shader cache survives
    // OS updates and hits on any Apple Silicon Mac (docs/native-shaders.md,
    // "Converter target"; docs/portable-build.md). The driver still compiles
    // each library for the real GPU. The app exports [graphics]
    // shader_cache_target to this variable (src/core/fable2_graphics_config.h).
    static constexpr const char* kDefaultPin = "apple7:15.0";
    const char* pin_env = std::getenv("FABLE2_MSC_MIN_TARGET");  // getenv-ok: per pipeline-compiler setup, not per draw
    // "portable" (the Dev Launcher's and the config's name) = the default.
    const bool pin_from_env = pin_env && pin_env[0] &&
                              std::strcmp(pin_env, "portable") != 0;
    const char* pin = pin_from_env ? pin_env : kDefaultPin;
    if (std::strcmp(pin, "native") == 0) {
      REXLOG_INFO(
          "Metal shader converter: minimum target native, family {} macOS {} "
          "(FABLE2_MSC_MIN_TARGET=native; the shader cache is tied to this "
          "Mac and macOS version)",
          static_cast<int>(native_family), version_stream.str());
    } else {
      static const std::pair<const char*, IRGPUFamily> kFamilies[] = {
          {"apple6", IRGPUFamilyApple6}, {"apple7", IRGPUFamilyApple7},
          {"apple8", IRGPUFamilyApple8}, {"apple9", IRGPUFamilyApple9},
          {"apple10", IRGPUFamilyApple10}, {"metal3", IRGPUFamilyMetal3}};
      auto parse = [&](const std::string& value) {
        const size_t colon = value.find(':');
        const std::string family = value.substr(0, colon);
        const std::string version =
            colon == std::string::npos ? std::string() : value.substr(colon + 1);
        for (const auto& [name, id] : kFamilies) {
          if (family == name && !version.empty() &&
              version.find_first_not_of("0123456789.") == std::string::npos) {
            min_family = id;
            min_version = version;
            return true;
          }
        }
        return false;
      };
      std::string value(pin);
      if (!parse(value)) {
        REXLOG_WARN(
            "Metal shader converter: ignoring FABLE2_MSC_MIN_TARGET='{}' "
            "(expected e.g. apple7:15.0 or native); using the default {}",
            value, kDefaultPin);
        value = kDefaultPin;
        parse(value);
      }
      REXLOG_INFO(
          "Metal shader converter: minimum target pinned to {} macOS {} "
          "({}; native would be family {} macOS {})",
          value.substr(0, value.find(':')), min_version,
          pin_from_env && value == pin ? "FABLE2_MSC_MIN_TARGET" : "default",
          static_cast<int>(native_family), version_stream.str());
    }
    metal_shader_converter_->SetMinimumTarget(min_family, IROperatingSystem_macOS,
                                              min_version);
  }

  // Spawn async pipeline creation threads if enabled.
  int32_t thread_count_config = REXCVAR_GET(metal_pipeline_creation_threads);
  uint32_t logical_cores = std::thread::hardware_concurrency();
  uint32_t thread_count = 0;
  if (thread_count_config < 0) {
    thread_count = std::max(1u, logical_cores * 3 / 4);
  } else {
    thread_count =
        std::min(static_cast<uint32_t>(thread_count_config), logical_cores);
  }
  if (thread_count > 0 && REXCVAR_GET(async_shader_compilation)) {
    creation_threads_.reserve(thread_count);
    for (uint32_t i = 0; i < thread_count; ++i) {
      creation_threads_.emplace_back(&MetalPipelineCache::CreationThread, this,
                                     i);
    }
    REXLOG_INFO("MetalPipelineCache: spawned {} async pipeline creation threads",
           thread_count);
  }

  // FABLE2_NATIVE_SHADERS: tool lookup in the background (warm-up's cache
  // lookups need the toolchain hash) and the low-priority optimizer pool.
  if (NativeShadersMode() && ns_threads_.empty()) {
    StartAirOptimizerLookup();
    const uint32_t ns_count = std::min(4u, std::max(1u, logical_cores / 2));
    for (uint32_t i = 0; i < ns_count; ++i) {
      ns_threads_.emplace_back(&MetalPipelineCache::NativeShaderThread, this);
    }
    REXLOG_INFO(
        "[native-shaders] on: {} background optimizer threads (utility QoS)",
        ns_count);
  }

  return true;
}

// ---------------------------------------------------------------------------
// Shader storage lifecycle
// ---------------------------------------------------------------------------

std::filesystem::path MetalPipelineCache::ShaderStorageTitleRootFor(
    const std::filesystem::path& cache_root, uint32_t title_id) const {
  return GetShaderStorageLocalRoot(cache_root) / "metal" /
         GetShaderStorageDeviceTag() / GetShaderStorageAbiTag() /
         fmt::format("{:08X}", title_id);
}

void MetalPipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  InitializeShaderStorageInternal(cache_root, title_id, blocking);
}

bool MetalPipelineCache::InitializeShaderStorageInternal(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  ShutdownShaderStorage();
  background_warmup_pending_ = false;

  if (!device_) {
    REXLOG_WARN("Metal shader storage init skipped (no device)");
    return false;
  }

  shader_storage_local_root_ = GetShaderStorageLocalRoot(cache_root) / "metal" /
                               GetShaderStorageDeviceTag() /
                               GetShaderStorageAbiTag();
  shader_storage_title_root_ =
      shader_storage_local_root_ / fmt::format("{:08X}", title_id);

  std::error_code ec;
  std::filesystem::create_directories(shader_storage_title_root_, ec);
  if (ec) {
    REXLOG_WARN("Metal shader storage: Failed to create {}: {}",
           shader_storage_title_root_.string(), ec.message());
    return false;
  }

  // FABLE2_NS_SPECIALIZE: learn data lives beside the artifact store.
  if (NativeShadersMode()) {
    ns_spec::InitializeStorage(shader_storage_title_root_, title_id);
  }

  artifact_store_path_ = shader_storage_title_root_ /
                         fmt::format("{:08X}.metal.artifacts", title_id);
  if (REXCVAR_GET(metal_shader_disk_cache) && g_metal_artifact_store) {
    g_metal_artifact_store->Initialize(artifact_store_path_);
  }

  ShaderStorageWriter<MetalPipelineStoredDescription>::PipelineStorageConfig
      pipeline_config;
  pipeline_config.file_suffix = ".metal.xpso";
  pipeline_config.api_magic = kMetalPipelineStorageMagic;
  pipeline_config.version =
      std::max(MetalPipelineDescription::kVersion,
               DxbcShaderTranslator::Modification::kVersion);

  uint32_t storage_index = storage_writer_.storage_index() + 1;
  std::vector<MetalPipelineStoredDescription> stored_descriptions;
  if (!storage_writer_.InitializeShaderStorage(
          cache_root, title_id, pipeline_config,
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
              uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            MetalShader* shader = LoadShader(
                type, ucode_dwords, ucode_dword_count, ucode_data_hash);
            // Cached shaders reach prewarming before IssueDraw, which normally
            // performs this analysis. Translation requires it in both paths.
            if (!shader->is_ucode_analyzed()) {
              shader->AnalyzeUcode(ucode_disasm_buffer_);
            }
            if (shader->ucode_storage_index() == storage_index) {
              return true;
            }
            shader->set_ucode_storage_index(storage_index);
            return true;
          },
          [&](const std::set<std::pair<uint64_t, uint64_t>>
                  & translations_needed) {
            REXLOG_INFO(
                "Metal shader storage: indexed {} cached shader "
                "translations",
                translations_needed.size());
          },
          stored_descriptions)) {
    return false;
  }
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;
  shader_storage_title_id_ = title_id;
  {
    std::lock_guard<std::mutex> lock(stored_pipeline_mutex_);
    stored_pipeline_hashes_.clear();
    for (const MetalPipelineStoredDescription& desc : stored_descriptions) {
      stored_pipeline_hashes_.insert(desc.description_hash);
    }
  }

  // FABLE2_NATIVE_SHADERS: pipelines with an optimized pixel shader use their
  // own archive (".ns1"); plain ones share the switch-off archive, so a launch
  // whose optimized shaders are not ready yet still warms up from it.
  const char* path_suffixes[kArchiveSetCount] = {
      "rtv-bindless", "rtv-bindless.ns1", "rtv-bindless.ns2"};
  const size_t archive_set_count =
      NativeShadersMode() ? (ns_spec::Enabled() ? 3 : 2) : 1;
  // Binary archives hold GPU code compiled by the OS Metal compiler, so they
  // are keyed by OS version as well (Metal also rejects or misses stale
  // entries; a miss only falls back to a normal compile).
  NS::OperatingSystemVersion archive_os_version =
      NS::ProcessInfo::processInfo()->operatingSystemVersion();
  std::filesystem::path archive_prefixes[kArchiveSetCount];
  for (size_t i = 0; i < archive_set_count; ++i) {
    std::string storage_suffix = fmt::format(
        "{}.{}.os{}.{}.{}", path_suffixes[i], GetShaderStorageAbiTag(),
        archive_os_version.majorVersion, archive_os_version.minorVersion,
        archive_os_version.patchVersion);
    // Segment files are <prefix>.segNNNNNN[.full].binarchive.
    archive_prefixes[i] =
        shader_storage_title_root_ /
        fmt::format("{:08X}.{}.metal", title_id, storage_suffix);
  }
  {
    // Remove temporary files left by a save interrupted by process exit.
    std::error_code tmp_ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             shader_storage_title_root_, tmp_ec)) {
      const std::string name = entry.path().filename().string();
      if (name.find(".binarchive.tmp-") != std::string::npos) {
        std::error_code remove_ec;
        std::filesystem::remove(entry.path(), remove_ec);
      }
    }
  }
  warmup_start_ = std::chrono::steady_clock::now();
  pipeline_binary_archive_last_save_ = warmup_start_;

  if (REXCVAR_GET(metal_pipeline_binary_archive)) {
    for (size_t i = 0; i < archive_set_count; ++i) {
      InitializePipelineBinaryArchive(archive_prefixes[i], i);
    }
  }

  bool prewarm_binary_archive = false;
  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    prewarm_binary_archive = archive_sets_[0].archive != nullptr ||
                             !archive_sets_[0].segments.empty();
  }
  if (prewarm_binary_archive && !stored_descriptions.empty()) {
    PrewarmStoredPipelines(stored_descriptions, blocking);
    background_warmup_pending_ = !blocking;
  }

  if (!background_warmup_pending_) frame_log::Warmup(false);
  if (readiness::Enabled()) {
    readiness::SetBackgroundProbe(
        [](void* owner) { return static_cast<MetalPipelineCache*>(owner)->ReadinessBackground(); }, this);
  }
  if (REXCVAR_GET(metal_pipeline_binary_archive)) {
    Ns2ArchiveEnabled();  // logs the switch state once
    g_archive_flush_cache.store(this, std::memory_order_release);
  }
  return true;
}

void MetalPipelineCache::ShutdownShaderStorage() {
  {
    MetalPipelineCache* expected = this;
    g_archive_flush_cache.compare_exchange_strong(expected, nullptr);
  }
  ns_spec::LearnSave();
  if (NativeShadersMode() && ns_spec::Enabled()) {
    ns_spec::LogCounters(true);
  }
  JoinPipelineBinaryArchiveSave();
  if (REXCVAR_GET(metal_pipeline_binary_archive_save_interval) > 0) {
    SerializePipelineBinaryArchive();
  }
  {
    std::lock_guard<std::mutex> write_lock(
        pipeline_binary_archive_write_mutex_);
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    for (ArchiveSet& set : archive_sets_) {
      if (set.archive) {
        set.archive->release();
        set.archive = nullptr;
      }
      for (MTL::BinaryArchive* segment : set.segments) {
        segment->release();
      }
      set.segments.clear();
      set.dirty = false;
      set.compacting = false;
    }
  }
  storage_writer_.ShutdownShaderStorage();
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;
  shader_storage_title_id_ = 0;
  {
    std::lock_guard<std::mutex> lock(stored_pipeline_mutex_);
    stored_pipeline_hashes_.clear();
  }

  if (g_metal_artifact_store) {
    g_metal_artifact_store->Shutdown();
  }
}

void MetalPipelineCache::EndSubmission() {
  if (background_warmup_pending_ && !IsCreatingPipelines()) {
    background_warmup_pending_ = false;
    frame_log::Warmup(false);
    REXLOG_INFO(
        "Metal shader storage: background warm-up complete in {:.2f}s; "
        "binary archive hits {} (lookup {} ms total), misses {}; compiled {} "
        "pipelines ({} ms summed over threads), archive adds {} ({} ms)",
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      warmup_start_)
            .count(),
        archive_lookup_hits_.load(), archive_lookup_ms_total_.load(),
        archive_lookup_misses_.load(), pipeline_compile_count_.load(),
        pipeline_compile_ms_total_.load(), archive_add_count_.load(),
        archive_add_ms_total_.load());
    MaybeSavePipelineBinaryArchiveAsync(true);
  } else if (!background_warmup_pending_) {
    MaybeSavePipelineBinaryArchiveAsync(false);
    // FABLE2_NS2_ARCHIVE: specialized pipelines are built on their first
    // matching draw, seconds to minutes into play, so a short session would
    // end before the next periodic save. Save the ".ns2" set once no new one
    // has been added for the settle time.
    int64_t last_add = ns2_last_add_ns_.load(std::memory_order_relaxed);
    if (last_add && Ns2ArchiveEnabled() &&
        SteadyNs() - last_add >= Ns2SettleNs()) {
      bool dirty;
      {
        std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
        dirty = archive_sets_[2].archive && archive_sets_[2].dirty;
      }
      if (!dirty || MaybeSavePipelineBinaryArchiveAsync(true, 2)) {
        if (dirty) {
          REXLOG_INFO("[ns2-archive] settle save: {} specialized pipelines "
                      "so far ({} from the archive, {} compiled)",
                      ns2_archive_hits_.load() + ns2_archive_compiled_.load(),
                      ns2_archive_hits_.load(), ns2_archive_compiled_.load());
        }
        ns2_last_add_ns_.compare_exchange_strong(last_add, 0);
      }
    }
  }
  const bool flush_shaders = shader_storage_file_flush_needed_.exchange(false);
  const bool flush_pipelines =
      pipeline_storage_file_flush_needed_.exchange(false);
  if (flush_shaders || flush_pipelines) {
    storage_writer_.RequestFlush(flush_shaders, flush_pipelines);
  }
  if (!creation_threads_.empty()) {
    creation_request_cond_.notify_one();
  }
}

bool MetalPipelineCache::ConvertDxbcToDxil(
    const std::vector<uint8_t>& dxbc_data, std::vector<uint8_t>& dxil_data,
    std::string* error_message) {
  dxil_convert_requests_.fetch_add(1, std::memory_order_relaxed);
  dxil_convert_dxbc_bytes_.fetch_add(dxbc_data.size(),
                                     std::memory_order_relaxed);
  MetalDxilBytecodeKey dxil_key = {};
  if (!dxbc_data.empty() && dxbc_to_dxil_converter_) {
    XXH128_hash_t dxbc_hash = XXH3_128bits(dxbc_data.data(), dxbc_data.size());
    dxil_key.dxbc_size = dxbc_data.size();
    dxil_key.dxbc_hash_low = dxbc_hash.low64;
    dxil_key.dxbc_hash_high = dxbc_hash.high64;
    dxil_key.converter_options_hash = dxbc_to_dxil_converter_->GetOptionsHash();
    dxil_key.converter_version = DxbcToDxilConverter::kCacheVersion;
    if (REXCVAR_GET(metal_shader_disk_cache) && g_metal_artifact_store &&
        g_metal_artifact_store->LoadDxilBytecode(dxil_key, &dxil_data)) {
      dxil_cache_hits_.fetch_add(1, std::memory_order_relaxed);
      converter_census::Dxil(true);
      dxil_convert_dxil_bytes_.fetch_add(dxil_data.size(),
                                         std::memory_order_relaxed);
      return true;
    }
  }
  dxil_cache_misses_.fetch_add(1, std::memory_order_relaxed);
  converter_census::Dxil(false);

  auto start = std::chrono::steady_clock::now();
  bool ok = dxbc_to_dxil_converter_ && dxbc_to_dxil_converter_->Convert(
                                           dxbc_data, dxil_data, error_message);
  auto end = std::chrono::steady_clock::now();
  uint64_t ms = ElapsedMs(start, end);
  dxil_convert_ms_total_.fetch_add(ms, std::memory_order_relaxed);
  AtomicMax(dxil_convert_ms_max_, ms);
  if (ok) {
    dxil_convert_dxil_bytes_.fetch_add(dxil_data.size(),
                                       std::memory_order_relaxed);
    if (REXCVAR_GET(metal_shader_disk_cache) && g_metal_artifact_store &&
        !dxbc_data.empty()) {
      g_metal_artifact_store->StoreDxilBytecode(dxil_key, dxil_data);
    }
  } else {
    dxil_convert_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  return ok;
}

MTL::Library* MetalPipelineCache::NewLibraryFromBytes(
    const std::vector<uint8_t>& bytes, const char* label) {
  ScopedAutoreleasePool autorelease_pool;
  if (!device_ || bytes.empty()) {
    RecordLibraryCreation(bytes.size(), false, 0);
    return nullptr;
  }
  NS::Error* error = nullptr;
  dispatch_data_t data = dispatch_data_create(
      bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  auto start = std::chrono::steady_clock::now();
  MTL::Library* library = device_->newLibrary(data, &error);
  auto end = std::chrono::steady_clock::now();
  dispatch_release(data);
  RecordLibraryCreation(bytes.size(), library != nullptr,
                        ElapsedMs(start, end));
  if (library) {
    converter_census::Library(converter_census::CurrentPurpose());
  }
  if (!library) {
    REXLOG_ERROR(
        "{}: Failed to create Metal library: {}",
        label ? label : "Metal library",
        error ? error->localizedDescription()->utf8String() : "unknown error");
  }
  return library;
}

void MetalPipelineCache::RecordLibraryCreation(size_t byte_count, bool success,
                                               uint64_t ms) {
  library_requests_.fetch_add(1, std::memory_order_relaxed);
  library_bytes_.fetch_add(byte_count, std::memory_order_relaxed);
  if (!success) {
    library_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  library_ms_total_.fetch_add(ms, std::memory_order_relaxed);
  AtomicMax(library_ms_max_, ms);
}

void MetalPipelineCache::RecordRenderPipelineCreation(bool success,
                                                      uint64_t ms) {
  render_pipeline_requests_.fetch_add(1, std::memory_order_relaxed);
  if (!success) {
    render_pipeline_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  render_pipeline_ms_total_.fetch_add(ms, std::memory_order_relaxed);
  AtomicMax(render_pipeline_ms_max_, ms);
}

std::string MetalPipelineCache::GetShaderStorageDeviceTag() const {
  std::string tag = "unknown";
  if (device_ && device_->name()) {
    tag = device_->name()->utf8String();
  }

  for (char& ch : tag) {
    if (!std::isalnum(static_cast<unsigned char>(ch))) {
      ch = '_';
    }
  }
  return tag;
}

std::string MetalPipelineCache::GetShaderStorageAbiTag() const {
  return fmt::format("msc{:08X}-ps{:08X}-sh{:08X}",
                     DxbcShaderTranslator::Modification::kVersion,
                     MetalPipelineDescription::kVersion,
                     MetalArtifactStore::kStorageVersion);
}

// ---------------------------------------------------------------------------
// Pipeline binary archive
// ---------------------------------------------------------------------------

bool MetalPipelineCache::InitializePipelineBinaryArchive(
    const std::filesystem::path& archive_prefix, size_t set_index) {
  if (!device_) {
    return false;
  }
  ArchiveSafeSaveEnabled();  // logs the switch state once
  ScopedAutoreleasePool autorelease_pool;
  std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
  ArchiveSet& set = archive_sets_[set_index];
  if (set.archive) {
    set.archive->release();
    set.archive = nullptr;
  }
  for (MTL::BinaryArchive* segment : set.segments) {
    segment->release();
  }
  set.segments.clear();

  // Find the segments of this archive key.
  constexpr size_t kMaxArchiveSegments = 8;
  const std::string prefix = archive_prefix.filename().string() + ".seg";
  const std::string suffix = ".binarchive";
  struct Segment {
    uint32_t index;
    bool full;
    std::filesystem::path path;
  };
  std::vector<Segment> segments;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(
           archive_prefix.parent_path(), ec)) {
    const std::string name = entry.path().filename().string();
    if (name.size() <= prefix.size() + suffix.size() ||
        name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) !=
            0) {
      continue;
    }
    std::string middle = name.substr(
        prefix.size(), name.size() - prefix.size() - suffix.size());
    bool full = false;
    if (middle.size() > 5 &&
        middle.compare(middle.size() - 5, 5, ".full") == 0) {
      full = true;
      middle.resize(middle.size() - 5);
    }
    if (middle.empty() || middle.size() > 9 ||
        middle.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    segments.push_back({uint32_t(std::stoul(middle)), full, entry.path()});
  }
  std::sort(segments.begin(), segments.end(),
            [](const Segment& a, const Segment& b) { return a.index < b.index; });
  uint32_t next_index = segments.empty() ? 1 : segments.back().index + 1;
  // A full segment supersedes every older one; remove those.
  size_t first = 0;
  for (size_t i = segments.size(); i-- > 0;) {
    if (segments[i].full) {
      first = i;
      break;
    }
  }
  for (size_t i = 0; i < first; ++i) {
    std::error_code remove_ec;
    std::filesystem::remove(segments[i].path, remove_ec);
  }

  uint64_t loaded_bytes = 0;
  for (size_t i = first; i < segments.size(); ++i) {
    MTL::BinaryArchiveDescriptor* desc =
        MTL::BinaryArchiveDescriptor::alloc()->init();
    NS::String* path_string = NS::String::string(
        segments[i].path.string().c_str(), NS::UTF8StringEncoding);
    desc->setUrl(NS::URL::fileURLWithPath(path_string));
    NS::Error* error = nullptr;
    MTL::BinaryArchive* segment = device_->newBinaryArchive(desc, &error);
    desc->release();
    if (!segment) {
      // Unreadable (corrupt or from another OS/GPU): drop it; its pipelines
      // are compiled normally and saved again.
      REXLOG_WARN("Metal binary archive: removing unreadable {}: {}",
                  segments[i].path.string(),
                  error ? error->localizedDescription()->utf8String()
                        : "unknown error");
      std::error_code remove_ec;
      std::filesystem::remove(segments[i].path, remove_ec);
      continue;
    }
    std::error_code size_ec;
    uint64_t size = std::filesystem::file_size(segments[i].path, size_ec);
    loaded_bytes += size_ec ? 0 : size;
    set.segments.push_back(segment);
  }

  set.compacting = set.segments.size() >= kMaxArchiveSegments;
  MTL::BinaryArchiveDescriptor* desc =
      MTL::BinaryArchiveDescriptor::alloc()->init();
  NS::Error* error = nullptr;
  set.archive = device_->newBinaryArchive(desc, &error);
  desc->release();
  if (!set.archive) {
    REXLOG_WARN("Metal binary archive init failed: {}",
                error ? error->localizedDescription()->utf8String()
                      : "unknown error");
  }
  std::filesystem::path session_path = archive_prefix;
  session_path += fmt::format(".seg{:06}{}.binarchive", next_index,
                              set.compacting ? ".full" : "");
  set.path = session_path;
  set.dirty = false;
  REXLOG_INFO(
      "Metal binary archive: loaded {} segment(s) ({} bytes); new pipelines "
      "go to {}{}",
      set.segments.size(), loaded_bytes, session_path.filename().string(),
      set.compacting ? " (compacting)" : "");
  return set.archive != nullptr || !set.segments.empty();
}

void MetalPipelineCache::SerializePipelineBinaryArchive() {
  for (size_t i = 0; i < kArchiveSetCount; ++i) {
    SerializeArchiveSet(i);
  }
}

void MetalPipelineCache::SerializeArchiveSet(size_t set_index) {
  ScopedAutoreleasePool autorelease_pool;
  // Adds and saves are serialized; draws keep using the archive for lookups.
  std::lock_guard<std::mutex> write_lock(pipeline_binary_archive_write_mutex_);
  ArchiveSet& set = archive_sets_[set_index];
  MTL::BinaryArchive* archive = nullptr;
  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    if (!set.archive || !set.dirty) {
      return;
    }
    archive = set.archive;
    archive->retain();
    path = set.path;
    set.dirty = false;
  }
  // Write a temporary file and rename it over the archive, so an exit during
  // the save leaves the previous archive intact. The renamed-over file may
  // still be mapped by this process; rename keeps that inode valid.
  std::filesystem::path temporary = path;
  temporary += fmt::format(".tmp-{}", getpid());
  auto start = std::chrono::steady_clock::now();
  NS::String* path_string =
      NS::String::string(temporary.string().c_str(), NS::UTF8StringEncoding);
  NS::URL* url = NS::URL::fileURLWithPath(path_string);
  NS::Error* error = nullptr;
  bool ok;
  if (ArchiveTestFailSave()) {
    void* expected = nullptr;
    g_archive_test_poisoned.compare_exchange_strong(expected, archive);
  }
  if (ArchiveTestFailSave() && g_archive_test_poisoned.load() == archive) {
    ok = false;  // simulated poisoned archive (trace-only test)
    REXLOG_INFO("[archive-safe-save] test: simulated save failure of the poisoned archive");
  } else {
    ok = archive->serializeToURL(url, &error);
  }
  std::error_code ec;
  if (ok) {
    std::filesystem::rename(temporary, path, ec);
    ok = !ec;
  }
  if (!ok) {
    REXLOG_WARN("Metal binary archive save to {} failed: {}", path.string(),
                ec ? ec.message()
                   : (error ? error->localizedDescription()->utf8String()
                            : "unknown error"));
    std::error_code remove_ec;
    std::filesystem::remove(temporary, remove_ec);
    if (ArchiveSafeSaveEnabled() && device_) {
      // CR-05: the in-memory archive holds something it cannot serialize, so
      // every retry would fail the same way. Start a fresh segment; the file
      // of the failed one (if an earlier save of it succeeded) stays valid.
      // Adds also hold write_lock, so none can land in the old archive now.
      MTL::BinaryArchiveDescriptor* fresh_desc =
          MTL::BinaryArchiveDescriptor::alloc()->init();
      NS::Error* fresh_error = nullptr;
      MTL::BinaryArchive* fresh =
          device_->newBinaryArchive(fresh_desc, &fresh_error);
      fresh_desc->release();
      std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
      if (fresh && set.archive == archive) {
        const std::filesystem::path next = NextArchiveSegmentPath(path);
        set.archive->release();
        set.archive = fresh;
        set.path = next;
        set.compacting = false;
        set.dirty = false;
        REXLOG_WARN(
            "[archive-safe-save] save failed; new pipelines now go to {} "
            "(the failed segment's unsaved pipelines compile again next "
            "launch)",
            next.filename().string());
      } else {
        if (fresh) {
          fresh->release();
        }
        set.dirty = true;
      }
    } else {
      std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
      set.dirty = true;
    }
  } else {
    std::error_code size_ec;
    uint64_t size = std::filesystem::file_size(path, size_ec);
    REXLOG_INFO("Metal binary archive: saved {} bytes in {} ms",
                size_ec ? 0 : size,
                ElapsedMs(start, std::chrono::steady_clock::now()));
    if (ArchiveSafeSaveEnabled() && device_) {
      // CR-05 root cause (docs/native-shaders.md, milestone 2): serializing
      // the same in-memory archive a second time, after more adds, fails
      // with "expecting 'fragment' stage in pipeline no. N" (every failure
      // logged so far was a second save; no session ever saved one archive
      // twice successfully). So a saved archive is closed: later pipelines go
      // to a fresh archive saved as the next segment. Adds hold write_lock,
      // so none can land in the old archive between the save and the swap.
      MTL::BinaryArchiveDescriptor* fresh_desc =
          MTL::BinaryArchiveDescriptor::alloc()->init();
      NS::Error* fresh_error = nullptr;
      MTL::BinaryArchive* fresh =
          device_->newBinaryArchive(fresh_desc, &fresh_error);
      fresh_desc->release();
      std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
      if (fresh && set.archive == archive) {
        set.archive->release();
        set.archive = fresh;
        // Keep `compacting`: after a ".full" save, pipelines found in the
        // older segments (deleted next launch) must still be recorded.
        set.path = NextArchiveSegmentPath(path);
      } else if (fresh) {
        fresh->release();
      }
    }
  }
  archive->release();
}

bool MetalPipelineCache::MaybeSavePipelineBinaryArchiveAsync(bool force,
                                                             int only_set) {
  int32_t interval = REXCVAR_GET(metal_pipeline_binary_archive_save_interval);
  if (interval <= 0 || pipeline_binary_archive_saving_.load()) {
    return false;
  }
  auto now = std::chrono::steady_clock::now();
  if (!force &&
      now - pipeline_binary_archive_last_save_ < std::chrono::seconds(interval)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    bool any_dirty = false;
    for (size_t i = 0; i < kArchiveSetCount; ++i) {
      if (only_set < 0 || size_t(only_set) == i) {
        any_dirty |= archive_sets_[i].archive && archive_sets_[i].dirty;
      }
    }
    if (!any_dirty) {
      return false;
    }
  }
  // A single-set save does not move the periodic schedule of the others.
  if (only_set < 0) {
    pipeline_binary_archive_last_save_ = now;
  }
  if (pipeline_binary_archive_save_thread_.joinable()) {
    pipeline_binary_archive_save_thread_.join();
  }
  pipeline_binary_archive_saving_.store(true);
  pipeline_binary_archive_save_thread_ = std::thread([this, only_set]() {
    if (only_set < 0) {
      SerializePipelineBinaryArchive();
    } else {
      SerializeArchiveSet(size_t(only_set));
    }
    pipeline_binary_archive_saving_.store(false);
  });
  return true;
}

void MetalPipelineCache::FlushPipelineBinaryArchiveForQuit() {
  auto start = std::chrono::steady_clock::now();
  size_t dirty_sets = 0;
  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    for (const ArchiveSet& set : archive_sets_) {
      dirty_sets += (set.archive && set.dirty) ? 1 : 0;
    }
  }
  if (!dirty_sets) {
    REXLOG_INFO("[ns2-archive] window close: archives already saved ({} "
                "specialized pipelines this session: {} from the archive, {} "
                "compiled)",
                ns2_archive_hits_.load() + ns2_archive_compiled_.load(),
                ns2_archive_hits_.load(), ns2_archive_compiled_.load());
    return;
  }
  // On a helper thread, waited for at most FABLE2_NS2_ARCHIVE_QUIT_MS
  // (default 500) so quitting never feels slow. A background save in progress
  // holds the write lock; SerializeArchiveSet waits for it and then saves only
  // what is still dirty. Past the limit the app exits anyway: a save cut off
  // leaves only a ".tmp-<pid>" file (removed at the next launch), never a
  // partial segment, because segments are written to a temporary and renamed.
  static const int64_t limit_ms = [] {
    const char* v = std::getenv("FABLE2_NS2_ARCHIVE_QUIT_MS");
    long ms = v && v[0] ? std::strtol(v, nullptr, 10) : 500;
    return int64_t(ms < 0 ? 0 : ms);
  }();
  struct State {
    std::mutex mutex;
    std::condition_variable cond;
    bool done = false;
  };
  auto state = std::make_shared<State>();
  std::thread([this, state]() {
    SerializePipelineBinaryArchive();
    std::lock_guard<std::mutex> lock(state->mutex);
    state->done = true;
    state->cond.notify_all();
  }).detach();
  bool done;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    done = state->cond.wait_for(lock, std::chrono::milliseconds(limit_ms),
                                [&] { return state->done; });
  }
  REXLOG_INFO("[ns2-archive] window close: {} {} dirty archive set(s) in {} ms "
              "(limit {} ms; {} specialized pipelines this session: {} from "
              "the archive, {} compiled)",
              done ? "saved" : "gave up waiting for", dirty_sets,
              ElapsedMs(start, std::chrono::steady_clock::now()), limit_ms,
              ns2_archive_hits_.load() + ns2_archive_compiled_.load(),
              ns2_archive_hits_.load(), ns2_archive_compiled_.load());
}

void MetalPipelineCache::LogNs2Pipeline(uint64_t n, bool emu) {
  ns2_last_pipeline_ns_.store(SteadyNs(), std::memory_order_relaxed);
  if (n > 64) {
    return;
  }
  REXLOG_INFO(
      "[ns2-archive] specialized pipeline #{}{} at {:.2f}s after storage "
      "init: archive hits {}, compiled {} ({} ms), build CPU {:.1f} ms",
      n, emu ? " (emulation, no archive)" : "",
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    warmup_start_)
          .count(),
      ns2_archive_hits_.load(), ns2_archive_compiled_.load(),
      ns2_compile_ms_.load(), ns2_build_cpu_us_.load() / 1000.0);
}

void MetalPipelineCache::JoinPipelineBinaryArchiveSave() {
  if (pipeline_binary_archive_save_thread_.joinable()) {
    pipeline_binary_archive_save_thread_.join();
  }
  pipeline_binary_archive_saving_.store(false);
}

void MetalPipelineCache::PrewarmStoredPipelines(
    const std::vector<MetalPipelineStoredDescription>& descriptions,
    bool blocking) {
  if (descriptions.empty()) {
    return;
  }
  size_t queued = 0;
  size_t skipped = 0;
  std::vector<NativeGeomPrewarmJob> native_geom_jobs;
  // [startup-par] Install the cached translations the stored pipelines use on
  // the GCD worker pool first (metal_startup_parallel.h). DXBC/DXIL steps take
  // shader_translation_mutex_, stage compiles go through the concurrent stage
  // compile cache and each library install locks its translation; with an
  // empty Metal system cache each install is a driver compile (3-8 s serial).
  // The loop below then finds them ready, exactly as after a serial install.
  if (startup::ParallelEnabled()) {
    const auto par_start = std::chrono::steady_clock::now();
    std::vector<MetalShader::MetalTranslation*> ready;
    std::unordered_set<MetalShader::MetalTranslation*> seen;
    auto note = [&](uint64_t hash, uint64_t modification) {
      auto it = shader_cache_.find(hash);
      if (it == shader_cache_.end()) return;
      auto* t = static_cast<MetalShader::MetalTranslation*>(
          it->second->GetOrCreateTranslation(modification));
      if (t && !t->is_valid() && seen.insert(t).second) ready.push_back(t);
    };
    for (const MetalPipelineStoredDescription& stored : descriptions) {
      const MetalPipelineDescription& desc = stored.description;
      if (desc.kind == uint32_t(PipelineKind::kStandard)) {
        note(desc.vertex_shader_hash, desc.vertex_shader_modification);
      }
      if (desc.pixel_shader_hash) {
        note(desc.pixel_shader_hash, desc.pixel_shader_modification);
      }
    }
    std::atomic<uint32_t> failed{0};
    startup::ParallelFor(ready.size(), [&](size_t i) {
      if (!EnsureMetalTranslationReady(ready[i])) {
        failed.fetch_add(1, std::memory_order_relaxed);
      }
    });
    REXLOG_INFO(
        "[startup-par] cached shader translations installed: {} in {:.0f} ms "
        "({} failed)",
        ready.size(), startup::MsSince(par_start), failed.load());
  }
  for (const MetalPipelineStoredDescription& stored : descriptions) {
    const MetalPipelineDescription& desc = stored.description;
    auto vs_it = shader_cache_.find(desc.vertex_shader_hash);
    if (vs_it == shader_cache_.end()) {
      ++skipped;
      continue;
    }
    auto* vertex_translation = static_cast<MetalShader::MetalTranslation*>(
        vs_it->second->GetOrCreateTranslation(desc.vertex_shader_modification));
    // Geometry and tessellation use generated Metal stages, as in IssueDraw.
    // Compiling their guest DXBC as an ordinary vertex entry point is invalid
    // (notably for terrain domain shaders) and skips otherwise valid records.
    if (!vertex_translation ||
        !(desc.kind == uint32_t(PipelineKind::kStandard)
              ? EnsureMetalTranslationReady(vertex_translation)
              : EnsureDxbcTranslationReady(vertex_translation, "vertex"))) {
      ++skipped;
      continue;
    }

    MetalShader::MetalTranslation* pixel_translation = nullptr;
    if (desc.pixel_shader_hash) {
      auto ps_it = shader_cache_.find(desc.pixel_shader_hash);
      if (ps_it == shader_cache_.end()) {
        ++skipped;
        continue;
      }
      pixel_translation = static_cast<MetalShader::MetalTranslation*>(
          ps_it->second->GetOrCreateTranslation(
              desc.pixel_shader_modification));
      if (!pixel_translation ||
          !EnsureMetalTranslationReady(pixel_translation)) {
        ++skipped;
        continue;
      }
    }

    PipelineAttachmentFormats formats = {};
    formats.sample_count = desc.sample_count;
    formats.depth_format = static_cast<MTL::PixelFormat>(desc.depth_format);
    formats.stencil_format = static_cast<MTL::PixelFormat>(desc.stencil_format);
    for (uint32_t i = 0; i < 4; ++i) {
      formats.color_formats[i] =
          static_cast<MTL::PixelFormat>(desc.color_formats[i]);
    }
    PipelineRenderingKey rendering = {};
    rendering.normalized_color_mask = desc.normalized_color_mask;
    rendering.alpha_to_mask_enable = desc.alpha_to_mask_enable;
    std::memcpy(rendering.blendcontrol, desc.blendcontrol,
                sizeof(rendering.blendcontrol));

    if (desc.kind == uint32_t(PipelineKind::kStandard)) {
      PipelineHandle* handle = GetOrCreatePipelineState(
          vertex_translation, pixel_translation, formats, rendering);
      if (handle) {
        handle->startup_warmup = true;
        ++queued;
      }
    } else if (desc.kind == uint32_t(PipelineKind::kGeometry)) {
      GeometryShaderKey geometry_key;
      geometry_key.key = uint32_t(desc.auxiliary_hash);
      // FABLE2_NATIVE_FIRST: draws of this kind take the native expansion
      // (the same checks as the draw path), so prewarm that pipeline in the
      // background instead of the converter's emulation pipeline.
      const bool points =
          geometry_key.type == PipelineGeometryShader::kPointList;
      const bool rects =
          geometry_key.type == PipelineGeometryShader::kRectangleList;
      if (NativeFirstMode() &&
          ((points && NativePointsMode()) || (rects && NativeRectsMode())) &&
          !geometry_key.user_clip_plane_cull &&
          !geometry_key.has_vertex_kill_and &&
          (!vertex_translation->shader().memexport_eM_written() ||
           (points && NativePointsMemexportMode()))) {
        NativeGeomPrewarmJob job;
        job.vertex = vertex_translation;
        job.pixel = pixel_translation;
        job.geometry_key = geometry_key;
        job.formats = formats;
        job.rendering = rendering;
        native_geom_jobs.push_back(job);
        ++queued;
      } else if (GetOrCreateGeometryPipelineState(vertex_translation,
                                           pixel_translation, geometry_key,
                                           formats, rendering)) {
        ++queued;
      }
    } else if (desc.kind == uint32_t(PipelineKind::kTessellation)) {
      PrimitiveProcessor::ProcessingResult primitive = {};
      primitive.host_vertex_shader_type =
          Shader::HostVertexShaderType(desc.auxiliary0);
      primitive.tessellation_mode = xenos::TessellationMode(desc.auxiliary1);
      primitive.host_primitive_type = xenos::PrimitiveType(desc.auxiliary2);
      // Blending on any written render target (as on the draw path): such
      // draws take the tessellator only with FABLE2_NATIVE_TESS_BLENDED.
      bool blended = false;
      for (uint32_t rt = 0; rt < 4; ++rt) {
        if (((rendering.normalized_color_mask >> (rt * 4)) & 0xF) &&
            (rendering.blendcontrol[rt] & 0x1FFF1FFFu) != 0x00010001u) {
          blended = true;
        }
      }
      // auxiliary3 = 1: recorded by a reference-pattern (CPU) native draw
      // (FABLE2_NATIVE_GEOM_PERSIST); that draw already chose the native path.
      const bool cpu_pattern = (desc.auxiliary3 & 1) != 0;
      if (NativeFirstMode() && NativeTessMode() &&
          (primitive.tessellation_mode == xenos::TessellationMode::kAdaptive ||
           primitive.tessellation_mode ==
               xenos::TessellationMode::kContinuous) &&
          primitive.host_vertex_shader_type ==
              Shader::HostVertexShaderType::kQuadDomainPatchIndexed &&
          (cpu_pattern || !blended || NativeTessBlendedMode() ||
           NativeTessMode() == 2) &&
          !vertex_translation->shader().memexport_eM_written()) {
        NativeGeomPrewarmJob job;
        job.tess = true;
        job.cpu_pattern = cpu_pattern;
        job.vertex = vertex_translation;
        job.pixel = pixel_translation;
        job.primitive = primitive;
        job.formats = formats;
        job.rendering = rendering;
        native_geom_jobs.push_back(job);
        ++queued;
      } else if (GetOrCreateTessellationPipelineState(vertex_translation,
                                               pixel_translation, primitive,
                                               formats, rendering)) {
        ++queued;
      }
    } else {
      ++skipped;
    }
  }
  if (!native_geom_jobs.empty()) {
    PrewarmNativeGeometry(std::move(native_geom_jobs));
  }
  REXLOG_INFO("Metal shader storage: prepared {} warm pipelines; waiting {}", queued, blocking);
  // [startup-par] The launcher pins the creation threads to 2 (steady play).
  // On a first launch every warm-up pipeline is a driver compile (~27 s summed
  // for ~420 pipelines), so 2 threads finish well after gameplay starts and
  // draws then wait for them (pso_wait). Extra threads help with the startup
  // queue only and exit once it is empty; on a warm launch it drains from the
  // binary archive in ~0.2 s. FABLE2_WARMUP_THREADS=<total> (0 = no extra).
  if (queued && !creation_threads_.empty() &&
      warmup_helper_threads_.empty()) {
    // One per performance core (base M1: 4, M3 Pro: 5), at most 6: the
    // compiles run in Metal's compiler service, these threads mostly wait,
    // and the efficiency cores stay free for the game's own threads.
    int perf_cores = 0;
    size_t perf_cores_size = sizeof(perf_cores);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &perf_cores,
                     &perf_cores_size, nullptr, 0) != 0 ||
        perf_cores <= 0) {
      perf_cores = int(std::max(2u, std::thread::hardware_concurrency() / 2));
    }
    uint32_t total = uint32_t(std::clamp(perf_cores, 2, 6));
    if (const char* v = std::getenv("FABLE2_WARMUP_THREADS"); v && *v) {
      total = uint32_t(std::max(0, std::atoi(v)));
    }
    const size_t extra = total > creation_threads_.size()
                             ? total - creation_threads_.size()
                             : 0;
    for (size_t i = 0; i < extra; ++i) {
      warmup_helper_threads_.emplace_back(&MetalPipelineCache::CreationThread,
                                          this, kWarmupHelperIndex + i);
    }
    if (extra) {
      REXLOG_INFO("[startup-par] {} extra pipeline creation threads for the "
                  "startup warm-up ({} in all; FABLE2_WARMUP_THREADS)",
                  extra, creation_threads_.size() + extra);
    }
  }
  if (blocking && !creation_threads_.empty()) {
    while (IsCreatingPipelines()) {
      creation_request_cond_.notify_one();
      std::this_thread::yield();
    }
  }
  REXLOG_INFO("Metal shader storage: queued {} warm pipelines, skipped {}", queued,
         skipped);
}

void MetalPipelineCache::PrewarmNativeGeometry(
    std::vector<NativeGeomPrewarmJob> jobs) {
  if (native_geom_prewarm_thread_.joinable()) {
    native_geom_prewarm_thread_.join();  // a second storage init
  }
  // Created here, once, so the workers only read it.
  DepthOnlyPixelFunction();
  native_geom_prewarm_pending_.store(uint32_t(jobs.size()),
                                     std::memory_order_release);
  native_geom_prewarm_thread_ = std::thread([this, jobs = std::move(jobs)]() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    const auto start = std::chrono::steady_clock::now();
    // First one job per native function (vertex/domain translation and
    // expansion kind), so no two workers compile the same function; then
    // the rest, which only build pipelines.
    std::vector<size_t> first, rest;
    std::unordered_set<uint64_t> seen;
    for (size_t i = 0; i < jobs.size(); ++i) {
      const NativeGeomPrewarmJob& j = jobs[i];
      const uint64_t k[] = {uint64_t(reinterpret_cast<uintptr_t>(j.vertex)),
                            j.tess ? (0xFFFFFFFFull ^ uint64_t(j.cpu_pattern))
                                   : (uint64_t(j.geometry_key.type) |
                                      (uint64_t(j.geometry_key
                                                    .has_point_coordinates)
                                       << 8))};
      (seen.insert(XXH3_64bits(k, sizeof(k))).second ? first : rest)
          .push_back(i);
    }
    std::atomic<uint32_t> built{0}, failed{0};
    auto run = [&](size_t i) {
      if (!native_geom_prewarm_stop_.load(std::memory_order_relaxed)) {
        const NativeGeomPrewarmJob& j = jobs[i];
        MTL::RenderPipelineState* pipeline =
            j.tess ? GetOrCreateNativeTessPipeline(j.vertex, j.pixel,
                                                   j.primitive, j.formats,
                                                   j.rendering, j.cpu_pattern)
                   : GetOrCreateNativePointsPipeline(j.vertex, j.pixel,
                                                     j.geometry_key, j.formats,
                                                     j.rendering);
        (pipeline ? built : failed).fetch_add(1, std::memory_order_relaxed);
      }
      native_geom_prewarm_pending_.fetch_sub(1, std::memory_order_acq_rel);
    };
    startup::ParallelFor(first.size(), [&](size_t n) { run(first[n]); });
    startup::ParallelFor(rest.size(), [&](size_t n) { run(rest[n]); });
    REXLOG_INFO(
        "[native-prewarm] {} native tessellation / point / rectangle "
        "pipelines prepared in the background ({} failed: those draws take "
        "emulation) in {:.0f} ms; binary archive hits {}, misses {}",
        built.load(), failed.load(), startup::MsSince(start),
        native_geom_archive_hits_.load(), native_geom_archive_misses_.load());
  });
}

// ---------------------------------------------------------------------------
// [bank-prep] Shader bank precompile (headless prep; docs/shader-precompile-20261015.md)
// ---------------------------------------------------------------------------

namespace {
uint32_t BankPrepThreadCount() {
  if (const char* v = std::getenv("FABLE2_SHADER_PREP_BANK_THREADS"); v && *v) {  // getenv-ok: once (prep)
    return uint32_t(std::clamp(std::atoi(v), 1, 64));
  }
  // All cores but one, at most 11 (the MSL compiles run in Metal's compiler service processes; this leaves a
  // core for the prep's own threads). M5 Max and M3 Pro: 11; base M1: 7.
  const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
  return std::clamp(cores - 1, 2u, 11u);
}
}  // namespace

size_t MetalPipelineCache::StartBankPrecompile(const std::filesystem::path& bank_path) {
  const auto start = std::chrono::steady_clock::now();
  std::vector<bank::BankShader> shaders;
  std::string error;
  if (!bank::ReadBank(bank_path, &shaders, &error)) {
    REXLOG_WARN("[bank-prep] shader bank not read: {}", error);
    return 0;
  }
  // FABLE2_SHADER_PREP_BANK_VS: which vertex shaders. Default: those without vertex fetches (D3D patches the
  // vertex-fetch instructions of the others at bind time, so their runtime microcode is not the bank's); "all";
  // 0 = none.
  const char* vs_env = std::getenv("FABLE2_SHADER_PREP_BANK_VS");  // getenv-ok: once (prep)
  const int vs_mode = (vs_env && vs_env[0] == '0') ? 0 : (vs_env && std::strcmp(vs_env, "all") == 0) ? 2 : 1;
  using Mod = DxbcShaderTranslator::Modification;
  using DepthStencilMode = Mod::DepthStencilMode;
  const DepthStencilMode float24_mode =
      REXCVAR_GET(depth_float24_convert_in_pixel_shader)
          ? (REXCVAR_GET(depth_float24_round) ? DepthStencilMode::kFloat24Rounding
                                              : DepthStencilMode::kFloat24Truncating)
          : DepthStencilMode::kNoModifiers;
  // A job installs a translation (as the draw path does), or compiles one of the native expansion functions
  // of a vertex shader (rectangle / point / domain; GetOrCreateNativePointsPipeline, GetOrCreateNativeTess-
  // Pipeline) and discards it: only macOS's Metal cache keeps it.
  enum class JobKind : uint8_t { kInstall, kRects, kPoints, kPointsMesh, kDomain };
  struct Job {
    MetalShader::MetalTranslation* t;
    JobKind kind;
    bool flag;  // point coordinates / domain CPU pattern
    // Order of work (lower first), so a time budget keeps the most common variants: 0 pixel shaders without
    // MSAA, 1 with MSAA and the vertex shaders, 2 float24 depth, 3 footprint variants.
    uint8_t priority;
  };
  uint8_t priority = 0;
  std::vector<Job> work;
  std::unordered_set<MetalShader::MetalTranslation*> seen;
  auto add = [&](MetalShader* shader, uint64_t modification) {
    auto* t = static_cast<MetalShader::MetalTranslation*>(shader->GetOrCreateTranslation(modification));
    if (t && !t->is_valid() && seen.insert(t).second) work.push_back({t, JobKind::kInstall, false, priority});
    return t;
  };
  struct Loaded {
    MetalShader* shader;
    const bank::BankShader* bank;
  };
  std::vector<Loaded> pixel, vertex;
  std::unordered_set<uint64_t> loaded;
  size_t vertex_skipped = 0;
  for (const bank::BankShader& b : shaders) {
    if (!b.pixel && !vs_mode) continue;
    MetalShader* shader = LoadShader(b.pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex,
                                     b.ucode.data(), uint32_t(b.ucode.size()),
                                     XXH3_64bits(b.ucode.data(), b.ucode.size() * sizeof(uint32_t)));
    if (!shader || !loaded.insert(shader->ucode_data_hash()).second) continue;
    // Not written to the shader list (cache/shaders/shareable/*.xsh): that list stays "what the game used on
    // this Mac" (QueueStoredShader skips a shader marked as stored in this storage generation).
    if (storage_writer_.is_active()) shader->try_set_ucode_storage_index(storage_writer_.storage_index());
    if (!shader->is_ucode_analyzed()) shader->AnalyzeUcode(ucode_disasm_buffer_);
    if (!b.pixel && vs_mode == 1 && !shader->vertex_bindings().empty()) {
      ++vertex_skipped;
      continue;
    }
    (b.pixel ? pixel : vertex).push_back({shader, &b});
  }
  // Pixel shaders: GetCurrentPixelShaderModification with the SQ_PROGRAM_CNTL / SQ_CONTEXT_MISC /
  // SQ_INTERPOLATOR_CNTL values D3D sets with the shader (from its container): interpolators, param gen and
  // centroids are then exact. What stays draw state: MSAA or not (centroids), and the depth mode (early-Z hint
  // unless alpha test / alpha to coverage; float24 conversion when depth is on with a D24FS8 buffer). The
  // vertex shader is assumed to write every interpolator the pixel shader reads (512 of 514 remembered
  // pipelines on the development Mac).
  // FABLE2_EFFECTS_FOOTPRINT_VARIANT: at a scaled resolution, a shader with offset texture fetches gets a
  // second translation for draws that sample a resolution-scaled resolve (FootprintVariantNeeded).
  const uint32_t scale_x = native_xlate_config_.draw_resolution_scale_x;
  const uint32_t scale_y = native_xlate_config_.draw_resolution_scale_y;
  const bool footprint_variants = EffectsFootprintMode() && EffectsFootprintVariantMode() &&
                                  !REXCVAR_GET(draw_resolution_scaled_texture_offsets) &&
                                  (scale_x > 1 || scale_y > 1);
  std::set<uint32_t> input_masks;
  for (const Loaded& l : pixel) {
    MetalShader* shader = l.shader;
    reg::SQ_PROGRAM_CNTL program_cntl;
    program_cntl.value = l.bank->sq_program_cntl;
    reg::SQ_CONTEXT_MISC context_misc;
    context_misc.value = l.bank->sq_context_misc;
    reg::SQ_INTERPOLATOR_CNTL interpolator_cntl;
    interpolator_cntl.value = l.bank->sq_interpolator_cntl;
    uint32_t param_gen_pos = UINT32_MAX;
    // The register count also covers temporaries; the low half of the container's interpolator word is the
    // interpolators the shader reads (what the paired vertex shader writes).
    const uint32_t mask = shader->GetInterpolatorInputMask(program_cntl, context_misc, param_gen_pos) &
                          (l.bank->sq_interpolator_cntl & 0xFFFFu);
    input_masks.insert(mask);
    const uint32_t msaa_centroids =
        mask & ~xenos::GetInterpolatorSamplingPattern(xenos::MsaaSamples::k2X, context_misc.sc_sample_cntl,
                                                      interpolator_cntl.sampling_pattern);
    const uint32_t centroids[2] = {0, msaa_centroids};
    DepthStencilMode modes[3] = {DepthStencilMode::kNoModifiers, DepthStencilMode::kNoModifiers,
                                 DepthStencilMode::kNoModifiers};
    int n_modes = 1;
    if (shader->implicit_early_z_write_allowed()) modes[n_modes++] = DepthStencilMode::kEarlyHint;
    if (float24_mode != DepthStencilMode::kNoModifiers) modes[n_modes++] = float24_mode;
    bool footprint = false;
    if (footprint_variants) {
      const uint64_t masks = shader->OffsetFetchConstantMasks();
      footprint = (((scale_x == 2 || scale_x == 3) ? uint32_t(masks) : 0u) |
                   ((scale_y == 2 || scale_y == 3) ? uint32_t(masks >> 32) : 0u)) != 0;
    }
    for (int c = 0; c < (msaa_centroids ? 2 : 1); ++c) {
      for (int m = 0; m < n_modes; ++m) {
        Mod mod(shader_translator_->GetDefaultPixelShaderModification(
            shader->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
        mod.pixel.interpolator_mask = mask;
        mod.pixel.interpolators_centroid = centroids[c];
        if (param_gen_pos < xenos::kMaxInterpolators) {
          mod.pixel.param_gen_enable = 1;
          mod.pixel.param_gen_interpolator = param_gen_pos;
        } else {
          mod.pixel.param_gen_enable = 0;
          mod.pixel.param_gen_interpolator = 0;
        }
        mod.pixel.param_gen_point = 0;
        mod.pixel.depth_stencil_mode = modes[m];
        mod.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kOne;
        mod.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;
        priority = modes[m] == float24_mode && float24_mode != DepthStencilMode::kNoModifiers ? 2 : uint8_t(c);
        add(shader, mod.value);
        // Draws that sample a scaled resolve are post-processing passes without MSAA.
        priority = 3;
        if (footprint && c == 0) add(shader, mod.value | kPixelModificationFootprint);
      }
    }
  }
  // Vertex shaders (plain vertex type): the interpolators any bank pixel shader reads; no user clip planes, no
  // point size (point lists take the native expansion).
  for (const Loaded& l : vertex) {
    MetalShader* shader = l.shader;
    reg::SQ_PROGRAM_CNTL program_cntl;
    program_cntl.value = l.bank->sq_program_cntl;
    std::set<uint32_t> masks;
    for (uint32_t m : input_masks) masks.insert(shader->writes_interpolators() & m);
    masks.insert(0);  // no pixel shader (depth only)
    for (uint32_t mask : masks) {
      Mod mod(shader_translator_->GetDefaultVertexShaderModification(
          shader->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg),
          Shader::HostVertexShaderType::kVertex));
      mod.vertex.interpolator_mask = mask;
      mod.vertex.user_clip_plane_count = 0;
      mod.vertex.user_clip_plane_cull = 0;
      mod.vertex.vertex_kill_and = uint32_t((shader->writes_point_size_edge_flag_kill_vertex() & 0b100) != 0);
      mod.vertex.output_point_size = 0;
      priority = 1;
      MetalShader::MetalTranslation* t = add(shader, mod.value);
      if (!t || !NativeFirstMode()) continue;
      if (NativeRectsMode()) work.push_back({t, JobKind::kRects, false, 1});
      if (NativePointsMode()) {
        for (bool coordinates : {false, true}) {
          work.push_back({t, NativePointsMode() == 2 ? JobKind::kPointsMesh : JobKind::kPoints, coordinates, 1});
        }
      }
      if (NativeTessMode()) {
        // Terrain domain shaders: also with one user clip plane (water reflections).
        mod.vertex.host_vertex_shader_type = Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
        for (uint32_t clip_planes : {0u, 1u}) {
          mod.vertex.user_clip_plane_count = clip_planes;
          auto* domain = static_cast<MetalShader::MetalTranslation*>(shader->GetOrCreateTranslation(mod.value));
          if (!domain) continue;
          for (bool cpu_pattern : {false, true}) work.push_back({domain, JobKind::kDomain, cpu_pattern, 1});
        }
      }
    }
  }
  std::stable_sort(work.begin(), work.end(), [](const Job& a, const Job& b) { return a.priority < b.priority; });
  // FABLE2_SHADER_PREP_BANK_BUDGET_S (default 120, 0 = none): no new work starts after this many seconds, so
  // a slower Mac's install stays bounded; what is left is compiled on the draw path as before.
  double budget_s = 120.0;
  if (const char* v = std::getenv("FABLE2_SHADER_PREP_BANK_BUDGET_S"); v && *v) {  // getenv-ok: once (prep)
    budget_s = std::max(0.0, std::atof(v));
  }
  bank_prep_shaders_.store(uint32_t(pixel.size() + vertex.size()), std::memory_order_relaxed);
  bank_prep_total_.store(uint32_t(work.size()), std::memory_order_relaxed);
  bank_prep_pending_.store(uint32_t(work.size()), std::memory_order_release);
  const uint32_t threads = BankPrepThreadCount();
  REXLOG_INFO("[bank-prep] {}: {} shaders ({} pixel, {} vertex; {} vertex shaders with vertex fetches left to "
              "the draw path, FABLE2_SHADER_PREP_BANK_VS={}), {} translations queued on {} threads (read in "
              "{:.0f} ms)",
              bank_path.filename().string(), shaders.size(), pixel.size(), vertex.size(), vertex_skipped,
              vs_mode == 0 ? "0" : vs_mode == 2 ? "all" : "fetchless", work.size(), threads,
              startup::MsSince(start));
  if (work.empty()) return 0;
  if (bank_prep_thread_.joinable()) bank_prep_thread_.join();
  bank_prep_thread_ = std::thread([this, work = std::move(work), threads, budget_s]() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<size_t> next{0};
    auto worker = [&]() {
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
      for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < work.size();) {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        const Job& job = work[i];
        if (budget_s > 0 && startup::MsSince(t0) > budget_s * 1000.0) {
          bank_prep_skipped_.fetch_add(1, std::memory_order_relaxed);
          bank_prep_pending_.fetch_sub(1, std::memory_order_acq_rel);
          continue;
        }
        bool ok = !native_geom_prewarm_stop_.load(std::memory_order_relaxed);
        if (ok && job.kind == JobKind::kInstall) {
          if (NativeFirstMode() && !ConverterBuiltIn()) {
            // No-converter build: native only. A shader the native translator cannot take is left alone here
            // (the draw path reports it, [converter-use] refused, if the game ever uses it).
            const int native = job.t->is_valid() ? 1 : TryNativeFirst(job.t);
            ok = native > 0 || (native < 0 && job.t->is_valid());
          } else {
            ok = EnsureMetalTranslationReady(job.t);
          }
        } else if (ok) {
          MTL::Library* library = nullptr;
          MTL::Function* function = nullptr;
          std::string entry;
          ok = EnsureDxbcTranslationReady(job.t, "bank prep") &&
               CompileNativeXlate(job.t, {}, &library, &function, &entry,
                                  job.kind == JobKind::kPoints, /*point_coordinates=*/
                                  (job.kind == JobKind::kPoints || job.kind == JobKind::kPointsMesh) && job.flag,
                                  job.kind == JobKind::kRects, job.kind == JobKind::kPointsMesh,
                                  job.kind == JobKind::kDomain && job.flag);
          if (function) function->release();
          if (library) library->release();
        }
        (ok ? bank_prep_ok_ : bank_prep_failed_).fetch_add(1, std::memory_order_relaxed);
        pool->release();
        bank_prep_pending_.fetch_sub(1, std::memory_order_acq_rel);
      }
    };
    std::vector<std::thread> pool;
    for (uint32_t i = 1; i < threads; ++i) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
    bank_prep_ms_.store(uint64_t(startup::MsSince(t0)), std::memory_order_relaxed);
    REXLOG_INFO("[bank-prep] {} translations ready, {} failed, {} left to the draw path (time budget {:.0f} s), in "
                "{:.0f} ms ({} threads); native translate {} ms, MSL compile {} ms summed",
                bank_prep_ok_.load(), bank_prep_failed_.load(), bank_prep_skipped_.load(), budget_s,
                startup::MsSince(t0), threads,
                native_xlate_translate_us_.load(std::memory_order_relaxed) / 1000,
                native_xlate_compile_us_.load(std::memory_order_relaxed) / 1000);
  });
  return bank_prep_total_.load();
}

// ---------------------------------------------------------------------------
// LoadShader
// ---------------------------------------------------------------------------

Shader* MetalPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                       const uint32_t* host_address,
                                       uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  return LoadShader(shader_type, host_address, dword_count, hash);
}

MetalShader* MetalPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                            const uint32_t* host_address,
                                            uint32_t dword_count,
                                            uint64_t hash) {
  auto it = shader_cache_.find(hash);
  if (it != shader_cache_.end()) {
    return it->second.get();
  }

  auto shader = std::make_unique<MetalShader>(shader_type, hash, host_address,
                                              dword_count);

  MetalShader* result = shader.get();
  shader_cache_[hash] = std::move(shader);

  REXLOG_DEBUG("Loaded {} shader ({} dwords, hash {:016X})",
         shader_type == xenos::ShaderType::kVertex ? "vertex" : "pixel",
         dword_count, hash);

  return result;
}

void MetalPipelineCache::SetupShaderBindingLayoutUserUIDs(MetalShader& shader) {
  if (!shader.EnterBindingLayoutUserUIDSetup()) {
    return;
  }

  const std::vector<MetalShader::TextureBinding>& texture_bindings =
      shader.GetTextureBindingsAfterTranslation();
  const size_t texture_binding_count = texture_bindings.size();
  const std::vector<MetalShader::SamplerBinding>& sampler_bindings =
      shader.GetSamplerBindingsAfterTranslation();
  const size_t sampler_binding_count = sampler_bindings.size();

  const size_t texture_binding_layout_bytes =
      texture_binding_count * sizeof(*texture_bindings.data());
  uint64_t texture_binding_layout_hash = 0;
  if (texture_binding_count) {
    texture_binding_layout_hash =
        XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
  }

  uint64_t bindless_sampler_layout_hash = 0;
  if (sampler_binding_count) {
    XXH3_state_t hash_state;
    XXH3_64bits_reset(&hash_state);
    for (size_t i = 0; i < sampler_binding_count; ++i) {
      XXH3_64bits_update(&hash_state,
                         &sampler_bindings[i].bindless_descriptor_index,
                         sizeof(sampler_bindings[i].bindless_descriptor_index));
    }
    bindless_sampler_layout_hash = XXH3_64bits_digest(&hash_state);
  }

  size_t texture_binding_layout_uid = kLayoutUIDEmpty;
  size_t sampler_binding_layout_uid = kLayoutUIDEmpty;
  if (texture_binding_count || sampler_binding_count) {
    std::lock_guard<std::mutex> layouts_lock(layouts_mutex_);
    if (texture_binding_count) {
      auto found_range =
          texture_binding_layout_map_.equal_range(texture_binding_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length == texture_binding_count &&
            !std::memcmp(
                texture_binding_layouts_.data() + it->second.vector_span_offset,
                texture_bindings.data(), texture_binding_layout_bytes)) {
          texture_binding_layout_uid = it->second.uid;
          break;
        }
      }
      if (texture_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(
            kLayoutUIDEmpty == 0,
            "Layout UID is size + 1 because 0 is the empty layout UID");
        texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
        LayoutUID new_uid;
        new_uid.uid = texture_binding_layout_uid;
        new_uid.vector_span_offset = texture_binding_layouts_.size();
        new_uid.vector_span_length = texture_binding_count;
        texture_binding_layouts_.resize(new_uid.vector_span_offset +
                                        texture_binding_count);
        std::memcpy(
            texture_binding_layouts_.data() + new_uid.vector_span_offset,
            texture_bindings.data(), texture_binding_layout_bytes);
        texture_binding_layout_map_.emplace(texture_binding_layout_hash,
                                            new_uid);
      }
    }

    if (sampler_binding_count) {
      auto found_range = bindless_sampler_layout_map_.equal_range(
          bindless_sampler_layout_hash);
      for (auto it = found_range.first; it != found_range.second; ++it) {
        if (it->second.vector_span_length != sampler_binding_count) {
          continue;
        }
        sampler_binding_layout_uid = it->second.uid;
        const uint32_t* vector_bindless_sampler_layout =
            bindless_sampler_layouts_.data() + it->second.vector_span_offset;
        for (size_t i = 0; i < sampler_binding_count; ++i) {
          if (vector_bindless_sampler_layout[i] !=
              sampler_bindings[i].bindless_descriptor_index) {
            sampler_binding_layout_uid = kLayoutUIDEmpty;
            break;
          }
        }
        if (sampler_binding_layout_uid != kLayoutUIDEmpty) {
          break;
        }
      }
      if (sampler_binding_layout_uid == kLayoutUIDEmpty) {
        static_assert(
            kLayoutUIDEmpty == 0,
            "Layout UID is size + 1 because 0 is the empty layout UID");
        LayoutUID new_uid;
        new_uid.uid = bindless_sampler_layout_map_.size() + 1;
        sampler_binding_layout_uid = new_uid.uid;
        new_uid.vector_span_offset = bindless_sampler_layouts_.size();
        new_uid.vector_span_length = sampler_binding_count;
        bindless_sampler_layouts_.resize(new_uid.vector_span_offset +
                                         sampler_binding_count);
        uint32_t* vector_bindless_sampler_layout =
            bindless_sampler_layouts_.data() + new_uid.vector_span_offset;
        for (size_t i = 0; i < sampler_binding_count; ++i) {
          vector_bindless_sampler_layout[i] =
              sampler_bindings[i].bindless_descriptor_index;
        }
        bindless_sampler_layout_map_.emplace(bindless_sampler_layout_hash,
                                             new_uid);
      }
    }
  }

  shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);
  shader.SetSamplerBindingLayoutUserUID(sampler_binding_layout_uid);
}

// ---------------------------------------------------------------------------
// EnsureDepthOnlyPixelShader
// ---------------------------------------------------------------------------

MTL::Function* MetalPipelineCache::DepthOnlyPixelFunction() {
  if (!depth_only_pixel_function_ && EnsureDepthOnlyPixelShader()) {
    depth_only_pixel_function_ = depth_only_pixel_library_->newFunction(
        NS::String::string(depth_only_pixel_function_name_.c_str(),
                           NS::UTF8StringEncoding));
  }
  return depth_only_pixel_function_;
}

bool MetalPipelineCache::EnsureDepthOnlyPixelShader() {
  if (depth_only_pixel_library_) {
    return true;
  }
  // FABLE2_NATIVE_FIRST (docs/no-converter.md): the depth-only shader the
  // DXBC translator generates for render-target (non-ROV) output is empty:
  // no inputs, no outputs, no discard. The same empty fragment in MSL.
  if (NativeFirstMode()) {
    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    options->setLanguageVersion(MTL::LanguageVersion3_2);
    options->setMathMode(MTL::MathModeSafe);
    NS::Error* error = nullptr;
    MTL::Library* library = device_->newLibrary(
        NS::String::string("#include <metal_stdlib>\n"
                           "fragment void xe_native_depth_only_ps() {}\n",
                           NS::UTF8StringEncoding),
        options, &error);
    options->release();
    if (library) {
      depth_only_pixel_library_ = library;
      depth_only_pixel_function_name_ = "xe_native_depth_only_ps";
      REXLOG_INFO("[native-first] depth-only pixel shader: native (empty)");
      return true;
    }
    REXLOG_WARN("[native-first] depth-only pixel shader: MSL compile failed "
                "({}); converter",
                error ? error->localizedDescription()->utf8String() : "?");
  }
  if (!shader_translator_ || !dxbc_to_dxil_converter_ ||
      !metal_shader_converter_) {
    REXLOG_ERROR("Depth-only PS: shader translation not initialized");
    return false;
  }

  converter_census::Scope census_scope(converter_census::kDepthOnlyPS, 0);
  std::vector<uint8_t> dxbc_data =
      shader_translator_->CreateDepthOnlyPixelShader();
  if (dxbc_data.empty()) {
    REXLOG_ERROR("Depth-only PS: failed to create DXBC");
    return false;
  }

  std::vector<uint8_t> dxil_data;
  std::string dxil_error;
  if (!ConvertDxbcToDxil(dxbc_data, dxil_data, &dxil_error)) {
    REXLOG_ERROR("Depth-only PS: DXBC to DXIL conversion failed: {}", dxil_error);
    return false;
  }

  MetalShaderConversionResult result;
  if (!metal_shader_converter_->ConvertWithStage(MetalShaderStage::kFragment,
                                                 dxil_data, result)) {
    REXLOG_ERROR("Depth-only PS: DXIL to Metal conversion failed: {}",
           result.error_message);
    return false;
  }

  depth_only_pixel_library_ =
      NewLibraryFromBytes(result.metallib_data, "Depth-only PS");
  if (!depth_only_pixel_library_) {
    return false;
  }
  depth_only_pixel_function_name_ = result.function_name;
  if (depth_only_pixel_function_name_.empty()) {
    REXLOG_ERROR("Depth-only PS: missing function name");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Shader modification selection
// ---------------------------------------------------------------------------

DxbcShaderTranslator::Modification
MetalPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask) const {
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);

  modification.vertex.output_point_size =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                   xenos::PrimitiveType::kPointList);

  return modification;
}

DxbcShaderTranslator::Modification
MetalPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask &
      ~xenos::GetInterpolatorSamplingPattern(
          regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
          regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
          regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                 xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
  if (REXCVAR_GET(depth_float24_convert_in_pixel_shader) &&
      normalized_depth_control.z_enable &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
          xenos::DepthRenderTargetFormat::kD24FS8) {
    modification.pixel.depth_stencil_mode =
        REXCVAR_GET(depth_float24_round) ? DepthStencilMode::kFloat24Rounding
                                   : DepthStencilMode::kFloat24Truncating;
  } else if (shader.implicit_early_z_write_allowed() &&
             (!shader.writes_color_target(0) ||
              !draw_util::DoesCoverageDependOnAlpha(
                  regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }

  // Check if MIN/MAX blend is used with non-trivial source factors.
  // Fixed-function blend ignores factors for MIN/MAX, but Xbox 360 applies
  // them. If the destination factor is ONE, we can pre-multiply the shader
  // output by the source factor to emulate this. Only RT0 is supported.
  modification.pixel.rt0_blend_rgb_factor_for_premult =
      xenos::BlendFactor::kOne;
  modification.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;

  if (shader.writes_color_target(0)) {
    auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[0]);

    // Pre-multiply by kSrcAlpha for MIN/MAX blend ops when dstFactor is ONE.
    if ((blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.color_destblend == xenos::BlendFactor::kOne) {
      modification.pixel.rt0_blend_rgb_factor_for_premult =
          xenos::BlendFactor::kSrcAlpha;
    }

    if ((blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
         blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
        blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
        blend_control.alpha_destblend == xenos::BlendFactor::kOne) {
      modification.pixel.rt0_blend_a_factor_for_premult =
          xenos::BlendFactor::kSrcAlpha;
    }
  }

  return modification;
}

// ---------------------------------------------------------------------------
// GetOrCreatePipelineState (standard render pipeline)
// ---------------------------------------------------------------------------

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildPipelineDescription(
    PipelineKind kind, MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) const {
  MetalPipelineDescription desc = {};
  desc.kind = uint32_t(kind);
  desc.vertex_shader_hash = vertex_translation->shader().ucode_data_hash();
  desc.vertex_shader_modification = vertex_translation->modification();
  if (pixel_translation) {
    desc.pixel_shader_hash = pixel_translation->shader().ucode_data_hash();
    desc.pixel_shader_modification = pixel_translation->modification();
  }
  desc.sample_count = attachment_formats.sample_count;
  desc.depth_format = uint32_t(attachment_formats.depth_format);
  desc.stencil_format = uint32_t(attachment_formats.stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    desc.color_formats[i] = uint32_t(attachment_formats.color_formats[i]);
  }
  desc.normalized_color_mask = rendering_key.normalized_color_mask;
  desc.alpha_to_mask_enable = rendering_key.alpha_to_mask_enable;
  std::memcpy(desc.blendcontrol, rendering_key.blendcontrol,
              sizeof(desc.blendcontrol));
  return desc;
}

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildStandardPipelineDescription(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) const {
  return BuildPipelineDescription(PipelineKind::kStandard, vertex_translation,
                                  pixel_translation, attachment_formats,
                                  rendering_key);
}

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildGeometryPipelineDescription(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    GeometryShaderKey geometry_shader_key,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) const {
  MetalPipelineDescription desc = BuildPipelineDescription(
      PipelineKind::kGeometry, vertex_translation, pixel_translation,
      attachment_formats, rendering_key);
  desc.auxiliary_hash = geometry_shader_key.key;
  return desc;
}

MetalPipelineCache::MetalPipelineDescription
MetalPipelineCache::BuildTessellationPipelineDescription(
    MetalShader::MetalTranslation* domain_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    xenos::TessellationMode tessellation_mode,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) const {
  MetalPipelineDescription desc = BuildPipelineDescription(
      PipelineKind::kTessellation, domain_translation, pixel_translation,
      attachment_formats, rendering_key);
  desc.auxiliary0 =
      uint32_t(primitive_processing_result.host_vertex_shader_type);
  desc.auxiliary1 = uint32_t(tessellation_mode);
  desc.auxiliary2 = uint32_t(primitive_processing_result.host_primitive_type);
  return desc;
}

void MetalPipelineCache::QueueStoredShader(MetalShader& shader) {
  if (!storage_writer_.is_active()) {
    return;
  }
  if (shader.try_set_ucode_storage_index(storage_writer_.storage_index())) {
    shader_storage_file_flush_needed_ = true;
    storage_writer_.QueueShaderWrite(&shader);
  }
}

void MetalPipelineCache::QueueStoredPipeline(
    const MetalPipelineDescription& description, uint64_t description_hash) {
  if (!storage_writer_.is_active()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(stored_pipeline_mutex_);
    if (!stored_pipeline_hashes_.insert(description_hash).second) {
      return;
    }
  }
  MetalPipelineStoredDescription stored = {};
  stored.description_hash = description_hash;
  std::memcpy(&stored.description, &description, sizeof(description));
  pipeline_storage_file_flush_needed_ = true;
  storage_writer_.QueuePipelineWrite(stored);
}

bool MetalPipelineCache::EnsureDxbcTranslationReadyLocked(
    MetalShader::MetalTranslation* translation, const char* stage_name) {
  auto dxbc_translation =
      static_cast<const DxbcShader::DxbcTranslation*>(translation);
  if (translation->is_translated() && dxbc_translation->is_valid() &&
      !translation->translated_binary().empty()) {
    return true;
  }

  if (!shader_translator_->TranslateAnalyzedShader(*translation)) {
    REXLOG_ERROR("Failed to translate {} shader {:016X} to DXBC",
           stage_name ? stage_name : "unknown",
           translation->shader().ucode_data_hash());
    return false;
  }
  QueueStoredShader(static_cast<MetalShader&>(translation->shader()));
  return true;
}

bool MetalPipelineCache::EnsureDxilTranslationReady(
    MetalShader::MetalTranslation* translation, const char* stage_name) {
  if (!translation) {
    return true;
  }
  if (!translation->GetDxilDataCopy().empty()) {
    return true;
  }
  std::lock_guard<std::mutex> lock(shader_translation_mutex_);
  if (!translation->GetDxilDataCopy().empty()) {
    return true;
  }
  if (!EnsureDxbcTranslationReadyLocked(translation, stage_name)) {
    return false;
  }
  std::vector<uint8_t> dxil_data;
  std::string dxil_error;
  if (!ConvertDxbcToDxil(translation->translated_binary(), dxil_data,
                         &dxil_error)) {
    REXLOG_ERROR("{} shader {:016X}: DXBC to DXIL conversion failed: {}",
           stage_name ? stage_name : "guest",
           translation->shader().ucode_data_hash(), dxil_error);
    return false;
  }
  translation->SetDxilData(std::move(dxil_data));
  return true;
}

namespace {
// Cache key of the optimized library: the plain metallib's hash (it already
// encodes the DXIL and the resolution scale baked into the translation) plus
// the toolchain hash. Stored as a DXIL-bytecode record with its own options
// hash, so it can never be mistaken for a DXIL entry.
MetalDxilBytecodeKey NativeShaderCacheKey(const std::vector<uint8_t>& plain) {
  MetalDxilBytecodeKey key = {};
  XXH128_hash_t hash = XXH3_128bits(plain.data(), plain.size());
  key.dxbc_size = plain.size();
  key.dxbc_hash_low = hash.low64;
  key.dxbc_hash_high = hash.high64;
  key.converter_options_hash = AirOptimizerToolchainHash();
  key.converter_version = 0x4E530001;  // "NS" v1: metal-opt -O2 + metallib
  return key;
}
}  // namespace

bool MetalPipelineCache::LoadOptimizedPixelShader(
    const std::vector<uint8_t>& plain, std::vector<uint8_t>* optimized) {
  if (plain.empty() || !REXCVAR_GET(metal_shader_disk_cache) ||
      !g_metal_artifact_store || !AirOptimizerCacheReadable()) {
    return false;
  }
  return g_metal_artifact_store->LoadDxilBytecode(NativeShaderCacheKey(plain),
                                                  optimized) &&
         !optimized->empty();
}

bool MetalPipelineCache::OptimizeGuestPixelShader(
    uint64_t ucode_hash, const std::vector<uint8_t>& plain,
    std::vector<uint8_t>* optimized) {
  if (plain.empty() || !AirOptimizerAvailable()) {
    return false;
  }
  std::string error;
  auto start = std::chrono::steady_clock::now();
  bool ok = OptimizeMetallibAir(plain, *optimized, &error);
  native_shaders_opt_ms_total_.fetch_add(
      ElapsedMs(start, std::chrono::steady_clock::now()),
      std::memory_order_relaxed);
  if (!ok) {
    const uint64_t n =
        native_shaders_fallbacks_.fetch_add(1, std::memory_order_relaxed) + 1;
    REXLOG_WARN(
        "[native-shaders] PS {:016X}: AIR optimization failed ({}); plain "
        "library stays (fallback #{})",
        ucode_hash, error, n);
    return false;
  }
  if (REXCVAR_GET(metal_shader_disk_cache) && g_metal_artifact_store) {
    g_metal_artifact_store->StoreDxilBytecode(NativeShaderCacheKey(plain),
                                              *optimized);
  }
  return true;
}

// ---------------------------------------------------------------------------
// FABLE2_NATIVE_SHADERS background optimizer (docs/native-shaders.md, M2)
// ---------------------------------------------------------------------------

void MetalPipelineCache::QueueNativeShaderOptimization(
    MetalShader::MetalTranslation* translation) {
  uint8_t expected = 0;
  if (!NativeXlateMode() && AirOptimizerLookupDone() &&
      !AirOptimizerAvailable()) {
    return;  // no Metal Toolchain: logged once by the lookup
  }
  if (translation && !NativeXlateMode() &&
      NativeShaderKeepPlain(translation->shader().ucode_data_hash())) {
    if (translation->native_shader_state().exchange(2) != 2) {
      REXLOG_INFO("[native-shaders] PS {:016X}: kept plain (FABLE2_NS_KEEP_PLAIN)",
                  translation->shader().ucode_data_hash());
    }
    return;
  }
  if (!translation || ns_threads_.empty() ||
      !translation->native_shader_state().compare_exchange_strong(expected,
                                                                  1)) {
    return;
  }
  native_shaders_queued_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(ns_mutex_);
    if (ns_start_ == std::chrono::steady_clock::time_point{}) {
      ns_start_ = std::chrono::steady_clock::now();
    }
    ns_idle_logged_ = false;
    ns_optimize_queue_.push_back(translation);
  }
  ns_cond_.notify_one();
}

// FABLE2_NATIVE_XLATE: whether `library` is a native translation (entry point
// xe_native_ps) rather than a converter library (its own entry point).
static bool NativeXlateEntryIn(MTL::Library* library,
                               MetalShader::MetalTranslation* translation) {
  if (!library || !translation || !translation->is_native_xlate() ||
      library == translation->metal_library()) {
    return false;
  }
  if (library == translation->optimized_library()) {
    return true;
  }
  MTL::Function* f = library->newFunction(
      NS::String::string("xe_native_ps", NS::UTF8StringEncoding));
  if (f) {
    f->release();
    return true;
  }
  return false;
}

bool MetalPipelineCache::CompileNativeXlate(
    MetalShader::MetalTranslation* translation,
    const std::vector<MslBakedDword>& baked, MTL::Library** library_out,
    MTL::Function** function_out, std::string* entry_out, bool expand_points,
    bool point_coordinates, bool expand_rects, bool points_mesh,
    bool domain_cpu_pattern) {
  *library_out = nullptr;
  *function_out = nullptr;
  if (!translation) {
    return false;
  }
  const bool vertex = translation->shader().type() == xenos::ShaderType::kVertex;
  if (vertex && !NativeXlateVertexMode()) {
    return false;
  }
  const uint64_t hash = translation->shader().ucode_data_hash();
  auto t0 = std::chrono::steady_clock::now();
  MslTranslatorConfig config;
  config.draw_resolution_scale_x = native_xlate_config_.draw_resolution_scale_x;
  config.draw_resolution_scale_y = native_xlate_config_.draw_resolution_scale_y;
  config.gamma_render_target_as_unorm8 =
      native_xlate_config_.gamma_render_target_as_unorm8;
  config.msaa_2x_supported = native_xlate_config_.msaa_2x_supported;
  config.draw_resolution_scaled_texture_offsets =
      REXCVAR_GET(draw_resolution_scaled_texture_offsets);
  config.use_fuzzy_alpha_epsilon = REXCVAR_GET(use_fuzzy_alpha_epsilon);
  config.effects_footprint =
      EffectsFootprintMode() &&
      (!EffectsFootprintVariantMode() ||
       (translation->modification() & kPixelModificationFootprint));
  config.expand_points = expand_points;
  config.expand_rects = expand_rects;
  config.points_mesh = points_mesh;
  config.domain_cpu_pattern = domain_cpu_pattern;
  config.snap_domain_uv = NativeTessSnapUvMode();
  config.point_coordinates = point_coordinates;
  MslShaderTranslator translator(config);
  MslTranslationResult result = translator.Translate(
      static_cast<MetalShader&>(translation->shader()),
      translation->modification(), baked);
  auto t1 = std::chrono::steady_clock::now();
  native_xlate_translate_us_.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
                   .count()),
      std::memory_order_relaxed);
  if (!result.ok) {
    const uint64_t n =
        native_xlate_fallback_.fetch_add(1, std::memory_order_relaxed) + 1;
    REXLOG_INFO("[native-xlate] {} {:016X}/{:X}: fallback ({}) #{}",
                vertex ? "VS" : "PS", hash,
                translation->modification(), result.reason, n);
    return false;
  }
  if (const char* dir = NativeXlateDumpDir()) {
    std::filesystem::path path =
        std::filesystem::path(dir) /
        fmt::format("{}_{:016X}_{:X}{}{}.metal", vertex ? "vs" : "ps", hash,
                    translation->modification(),
                    baked.empty() ? "" : "_spec",
                    domain_cpu_pattern ? "_dscpu"
                    : points_mesh ? "_ptsm"
                    : expand_points ? (point_coordinates ? "_ptsc" : "_pts")
                                    : (expand_rects ? "_rect" : ""));
    if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
      std::fwrite(result.source.data(), 1, result.source.size(), f);
      std::fclose(f);
    }
  }
  MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
  options->setLanguageVersion(MTL::LanguageVersion3_2);
  options->setMathMode(MTL::MathModeSafe);
  // The converter marks the vertex position invariant; keep it so here too.
  options->setPreserveInvariance(vertex);
  NS::Error* error = nullptr;
  // Diagnostic FABLE2_MSL_SALT=<text> (as in metal_render_target_cache.cc): a
  // comment in front of the source, so Metal's system shader cache misses.
  static const std::string msl_salt = [] {
    const char* v = std::getenv("FABLE2_MSL_SALT");
    return (v && v[0]) ? std::string("// salt ") + v + "\n" : std::string();
  }();
  if (!msl_salt.empty()) {
    result.source.insert(0, msl_salt);
  }
  MTL::Library* library = device_->newLibrary(
      NS::String::string(result.source.c_str(), NS::UTF8StringEncoding),
      options, &error);
  options->release();
  MTL::Function* function =
      library ? library->newFunction(NS::String::string(
                    result.entry_point.c_str(), NS::UTF8StringEncoding))
              : nullptr;
  auto t2 = std::chrono::steady_clock::now();
  const uint64_t compile_us = uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count());
  native_xlate_compile_us_.fetch_add(compile_us, std::memory_order_relaxed);
  uint64_t prev_max = native_xlate_compile_max_us_.load(std::memory_order_relaxed);
  while (compile_us > prev_max &&
         !native_xlate_compile_max_us_.compare_exchange_weak(prev_max,
                                                             compile_us)) {
  }
  if (!function) {
    const uint64_t n =
        native_xlate_compile_failed_.fetch_add(1, std::memory_order_relaxed) + 1;
    REXLOG_WARN("[native-xlate] {} {:016X}/{:X}: MSL compile failed #{}: {}",
                vertex ? "VS" : "PS", hash, translation->modification(), n,
                error ? error->localizedDescription()->utf8String()
                      : "no function");
    if (library) {
      library->release();
    }
    return false;
  }
  if (config.snap_domain_uv && result.entry_point == "xe_native_ds") {
    static std::atomic<uint32_t> snapped{0};
    const uint32_t n = snapped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & (n - 1)) == 0) {
      REXLOG_INFO("[tess-trace] snap-uv: native domain shader VS {:016X} compiled with the uv snapped to the "
                  "1/65536 grid (#{})",
                  hash, n);
    }
  }
  *library_out = library;
  *function_out = function;
  *entry_out = result.entry_point;
  return true;
}

bool MetalPipelineCache::NativeXlateBakedUse(
    MetalShader::MetalTranslation* translation,
    const std::vector<MslBakedDword>& baked,
    std::vector<MslBakedDword>* used) {
  MslTranslatorConfig config;
  config.draw_resolution_scale_x = native_xlate_config_.draw_resolution_scale_x;
  config.draw_resolution_scale_y = native_xlate_config_.draw_resolution_scale_y;
  config.gamma_render_target_as_unorm8 =
      native_xlate_config_.gamma_render_target_as_unorm8;
  config.msaa_2x_supported = native_xlate_config_.msaa_2x_supported;
  config.draw_resolution_scaled_texture_offsets =
      REXCVAR_GET(draw_resolution_scaled_texture_offsets);
  config.use_fuzzy_alpha_epsilon = REXCVAR_GET(use_fuzzy_alpha_epsilon);
  config.effects_footprint =
      EffectsFootprintMode() &&
      (!EffectsFootprintVariantMode() ||
       (translation->modification() & kPixelModificationFootprint));
  MslShaderTranslator translator(config);
  MslTranslationResult result = translator.Translate(
      static_cast<MetalShader&>(translation->shader()),
      translation->modification(), baked);
  if (!result.ok) {
    return false;
  }
  *used = std::move(result.baked_used);
  return true;
}

bool MetalPipelineCache::TryNativeXlate(
    MetalShader::MetalTranslation* translation) {
  MTL::Library* library = nullptr;
  MTL::Function* function = nullptr;
  std::string entry;
  if (!CompileNativeXlate(translation, {}, &library, &function, &entry)) {
    return false;
  }
  if (!translation->InstallNativeMetal(library, function, entry)) {
    function->release();
    library->release();
    return false;
  }
  const bool is_vertex =
      translation->shader().type() == xenos::ShaderType::kVertex;
  (is_vertex ? native_xlate_ok_vs_ : native_xlate_ok_ps_)
      .fetch_add(1, std::memory_order_relaxed);
  const uint64_t n = native_xlate_ok_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || (n & (n - 1)) == 0) {
    REXLOG_INFO(
        "[native-xlate] {} native shaders ({} pixel, {} vertex; {} fallbacks, "
        "{} compile failures); translate {} ms, MSL compile {} ms total, max "
        "{} ms",
        n, native_xlate_ok_ps_.load(std::memory_order_relaxed),
        native_xlate_ok_vs_.load(std::memory_order_relaxed),
        native_xlate_fallback_.load(std::memory_order_relaxed),
        native_xlate_compile_failed_.load(std::memory_order_relaxed),
        native_xlate_translate_us_.load(std::memory_order_relaxed) / 1000,
        native_xlate_compile_us_.load(std::memory_order_relaxed) / 1000,
        native_xlate_compile_max_us_.load(std::memory_order_relaxed) / 1000);
  }
  return true;
}

void MetalPipelineCache::RegisterNativeShaderHandle(PipelineHandle* handle) {
  std::lock_guard<std::mutex> lock(ns_mutex_);
  MetalShader::MetalTranslation* translation = handle->ns_pixel_translation;
  if (translation && !translation->optimized_function() &&
      translation->native_shader_state().load() != 2) {
    ns_handles_[translation].push_back(handle);
  }
  // FABLE2_NATIVE_XLATE: also rebuilt when the native vertex shader is ready.
  MetalShader::MetalTranslation* vertex = handle->ns_vertex_translation;
  if (NativeXlateMode() && vertex && !vertex->is_native_xlate() &&
      vertex->native_shader_state().load() != 2) {
    ns_handles_[vertex].push_back(handle);
  }
}

void MetalPipelineCache::LogNativeShaderProgress(const char* what) {
  const uint64_t plain =
      native_shaders_plain_pipelines_.load(std::memory_order_relaxed);
  const uint64_t swapped = native_shaders_swapped_.load(std::memory_order_relaxed);
  REXLOG_INFO(
      "[native-shaders] {} at {:.2f}s after storage init: queued {}, "
      "optimized-ready {} (+{} from cache at install), plain pipelines {}, "
      "swapped {}, plain-in-use {}, swap failed {} (deferred for the other "
      "shader {}, swapped later {}), fallbacks {}, {} ms "
      "optimizing; emulation pipelines plain {}, swapped {}, failed {}",
      what,
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    warmup_start_)
          .count(),
      native_shaders_queued_.load(std::memory_order_relaxed),
      native_shaders_ready_.load(std::memory_order_relaxed),
      native_shaders_cache_hits_.load(std::memory_order_relaxed), plain,
      swapped, plain > swapped ? plain - swapped : 0,
      native_shaders_swap_failed_.load(std::memory_order_relaxed),
      native_shaders_swap_nothing_yet_.load(std::memory_order_relaxed),
      native_shaders_swap_deferred_done_.load(std::memory_order_relaxed),
      native_shaders_fallbacks_.load(std::memory_order_relaxed),
      native_shaders_opt_ms_total_.load(std::memory_order_relaxed),
      ns_emu_plain_.load(std::memory_order_relaxed),
      ns_emu_swapped_.load(std::memory_order_relaxed),
      ns_emu_swap_failed_.load(std::memory_order_relaxed));
}

void MetalPipelineCache::NativeShaderSwap(PipelineHandle* handle) {
  // Never replace a pipeline that is still being created; a plain pipeline
  // stays bound until the optimized one is fully built. With
  // FABLE2_NATIVE_XLATE a pipeline can be swapped twice (pixel shader ready,
  // then vertex shader ready, or the other way round).
  handle->WaitForCreation();
  // exchange, not store: when a ready event found this swap already queued, it published its shader before
  // its own exchange, and this acquire makes that shader visible to the checks below.
  handle->ns_swap_queued.exchange(false, std::memory_order_acq_rel);
  if (!handle->state.load(std::memory_order_acquire)) {
    return;
  }
  const bool had_ps = handle->ns_optimized.load(std::memory_order_acquire);
  const bool had_vs = handle->ns_vs_native.load(std::memory_order_acquire);
  const bool want_vs = handle->ns_vertex_translation &&
                       handle->ns_vertex_translation->is_native_xlate();
  if (had_ps && (had_vs || !want_vs)) {
    return;
  }
  // Only build when something is ready to improve this pipeline. A swap is queued when either shader
  // becomes ready; the pipeline's creation may already have picked that shader up (it waits for creation
  // above), and the other shader may still be pending. Example (cold cache, FABLE2_NATIVE_XLATE): the
  // native VS is ready before the handle's async creation runs, so creation builds native VS + plain PS
  // and the VS-ready swap has nothing to add while the PS is still optimizing. That used to build a
  // redundant pipeline and log "optimized pipeline not created" (soak 2026-10-13, open question 2);
  // the PS's own ready event swaps it later. Real failures (a pipeline that should improve but doesn't)
  // still log below.
  MetalShader::MetalTranslation* ps_translation = handle->ns_pixel_translation;
  const bool ps_ready = ps_translation && ps_translation->optimized_function() != nullptr;
  if (!(ps_ready && !had_ps) && !(want_vs && !had_vs)) {
    native_shaders_swap_nothing_yet_.fetch_add(1, std::memory_order_relaxed);
    handle->ns_swap_deferred.store(true, std::memory_order_relaxed);
    return;
  }
  bool optimized = false;
  bool vertex_native = false;
  MTL::RenderPipelineState* pipeline = CreatePipelineFromHandle(
      handle, handle->ns_vertex_translation, handle->ns_pixel_translation,
      &optimized, nullptr, &vertex_native);
  const bool improves = pipeline && ((optimized && !had_ps) ||
                                     (vertex_native && !had_vs));
  if (!improves) {
    if (pipeline) {
      pipeline->release();
    }
    if (!pipeline || !had_ps) {
      const uint64_t n =
          native_shaders_swap_failed_.fetch_add(1, std::memory_order_relaxed) +
          1;
      if (n <= 4) {
        MetalShader::MetalTranslation* ps = handle->ns_pixel_translation;
        MetalShader::MetalTranslation* vs = handle->ns_vertex_translation;
        REXLOG_WARN(
            "[native-shaders] PS {:016X}: optimized pipeline not created; the "
            "plain pipeline stays (#{}; pipeline {}, optimized {}, vs native "
            "{}, had ps {} vs {}, want vs {}, ps opt fn {} state {}, vs {:016X} "
            "native {} state {})",
            ps->shader().ucode_data_hash(), n, pipeline ? "built" : "null",
            optimized, vertex_native, had_ps, had_vs, want_vs,
            ps->optimized_function() != nullptr,
            int(ps->native_shader_state().load()),
            vs ? vs->shader().ucode_data_hash() : 0,
            vs && vs->is_native_xlate(),
            vs ? int(vs->native_shader_state().load()) : -1);
      }
    }
    return;
  }
  // The draw path loads `state` once per draw, so the next lookup binds the
  // new pipeline; the replaced one stays alive for command buffers that
  // already reference it.
  MTL::RenderPipelineState* old =
      handle->state.exchange(pipeline, std::memory_order_acq_rel);
  if (!handle->ns_plain_state) {
    handle->ns_plain_state = old;
  } else {
    handle->ns_retired_states.push_back(old);
  }
  if (optimized) {
    handle->ns_optimized.store(true, std::memory_order_release);
  }
  if (vertex_native) {
    handle->ns_vs_native.store(true, std::memory_order_release);
  }
  if (optimized && handle->ns_swap_deferred.exchange(false, std::memory_order_relaxed)) {
    native_shaders_swap_deferred_done_.fetch_add(1, std::memory_order_relaxed);
  }
  const uint64_t n =
      native_shaders_swapped_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || (n & (n - 1)) == 0) {
    LogNativeShaderProgress("swapped");
  }
}

MTL::Library* MetalPipelineCache::NsEmuFragmentLibrary(
    MetalShader::MetalTranslation* pixel_translation, bool* out_optimized) {
  *out_optimized = false;
  if (NativeShadersEmuPipelines()) {
    if (MTL::Library* optimized = pixel_translation->optimized_library()) {
      *out_optimized = true;
      return optimized;
    }
  }
  return pixel_translation->metal_library();
}

void MetalPipelineCache::RegisterNsEmuSwap(
    const std::shared_ptr<NsEmuSwap>& swap) {
  MetalShader::MetalTranslation* translation = swap->pixel_translation;
  ns_emu_plain_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(ns_mutex_);
    if (translation->optimized_function()) {
      // Published between the library choice and now: rebuild right away.
      if (!swap->queued.exchange(true)) {
        ns_emu_swap_queue_.push_back(swap);
      }
    } else if (translation->native_shader_state().load() == 2) {
      return;  // optimization failed or unavailable: stays plain
    } else {
      ns_emu_handles_[translation].push_back(swap);
    }
  }
  ns_cond_.notify_one();
  // A plain library whose optimization was never queued (installed while no
  // background pool existed) gets queued here; a no-op when already queued.
  QueueNativeShaderOptimization(translation);
}

void MetalPipelineCache::NsEmuRebuild(const std::shared_ptr<NsEmuSwap>& swap) {
  if (swap->optimized.load(std::memory_order_acquire)) {
    return;
  }
  MTL::Library* library = swap->pixel_translation->optimized_library();
  MTL::RenderPipelineState* pipeline = library ? swap->build(library) : nullptr;
  if (!pipeline) {
    const uint64_t n =
        ns_emu_swap_failed_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4) {
      REXLOG_WARN(
          "[native-shaders] PS {:016X}: optimized {} emulation pipeline not "
          "created; the plain pipeline stays (#{})",
          swap->pixel_translation->shader().ucode_data_hash(),
          swap->tessellation ? "tessellation" : "geometry", n);
    }
    return;
  }
  // The draw path loads the published pipeline once per draw
  // (current_pipeline()); the plain one stays alive until the cache goes.
  swap->optimized.store(pipeline, std::memory_order_release);
  const uint64_t n = ns_emu_swapped_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || (n & (n - 1)) == 0) {
    LogNativeShaderProgress("emulation pipelines swapped");
  }
}

// ---------------------------------------------------------------------------
// FABLE2_NS_SPECIALIZE: hot-shader specialization (metal_ns_specialize.h,
// docs/native-shaders.md "Hot-shader specialization")
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t kNs2LibraryVersion = 0x4E530002;  // "NS" v2: specialized
constexpr uint32_t kNs2IndexVersion = 0x4E530003;    // its guard list

// Same parts as the NS1 key (plain library + toolchain), so the records of a
// pinned converter target can go into the portable seed. The library record
// also carries the hash of the baked values.
MetalDxilBytecodeKey Ns2Key(const ns_spec::PlainKey& plain, uint32_t version,
                            uint64_t values_hash) {
  MetalDxilBytecodeKey key = {};
  key.dxbc_size = plain.size;
  key.dxbc_hash_low = plain.low;
  key.dxbc_hash_high = plain.high;
  key.converter_options_hash = AirOptimizerToolchainHash();
  key.converter_version = version;
  key.reserved = uint32_t(values_hash) ^ uint32_t(values_hash >> 32);
  return key;
}
}  // namespace

void MetalPipelineCache::QueueSpecialization(
    MetalShader::MetalTranslation* translation) {
  if (!translation || ns_threads_.empty() ||
      !translation->plain_key().valid()) {
    return;
  }
  if (!translation->is_native_primary() && AirOptimizerLookupDone() &&
      !AirOptimizerCacheReadable()) {
    return;
  }
  uint8_t expected = 0;
  if (!translation->spec_state().compare_exchange_strong(expected, 1)) {
    return;
  }
  ns_spec_queued_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(ns_mutex_);
    ns_idle_logged_ = false;
    ns_spec_queue_.push_back(translation);
  }
  ns_cond_.notify_one();
}

void MetalPipelineCache::RequestRespecialization(
    MetalShader::MetalTranslation* translation) {
  uint8_t expected = 2;
  if (!translation || ns_threads_.empty() ||
      !translation->spec_state().compare_exchange_strong(expected, 1)) {
    return;
  }
  ns_spec_queued_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(ns_mutex_);
    ns_idle_logged_ = false;
    ns_spec_queue_.push_back(translation);
  }
  ns_cond_.notify_one();
}

void MetalPipelineCache::RequestSpecPipeline(PipelineHandle* handle) {
  uint8_t expected = 0;
  if (!handle || ns_threads_.empty() ||
      !handle->ns_spec_status.compare_exchange_strong(expected, 1)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(ns_mutex_);
    ns_idle_logged_ = false;
    ns_spec_build_queue_.push_back(handle);
  }
  ns_cond_.notify_one();
}

void MetalPipelineCache::RequestSpecEmuPipeline(
    const std::shared_ptr<NsEmuSwap>& swap) {
  uint8_t expected = 0;
  if (!swap || ns_threads_.empty() ||
      !swap->spec_status.compare_exchange_strong(expected, 1)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(ns_mutex_);
    ns_idle_logged_ = false;
    ns_spec_emu_queue_.push_back(swap);
  }
  ns_cond_.notify_one();
}

void MetalPipelineCache::LogSpecProgress(const char* what) {
  REXLOG_INFO(
      "[ns-spec] {} at {:.2f}s after storage init: queued {}, from cache {}, "
      "generated {} ({} ms), nothing to specialize {}, failed {}; "
      "specialized pipelines {}, failed {}",
      what,
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    warmup_start_)
          .count(),
      ns_spec_queued_.load(std::memory_order_relaxed),
      ns_spec_from_cache_.load(std::memory_order_relaxed),
      ns_spec_generated_.load(std::memory_order_relaxed),
      ns_spec_ms_.load(std::memory_order_relaxed),
      ns_spec_none_.load(std::memory_order_relaxed),
      ns_spec_failed_.load(std::memory_order_relaxed),
      ns_spec_pipelines_.load(std::memory_order_relaxed),
      ns_spec_pipeline_failed_.load(std::memory_order_relaxed));
}

void MetalPipelineCache::NsSpecialize(
    MetalShader::MetalTranslation* translation) {
  struct Done {
    MetalShader::MetalTranslation* t;
    ~Done() { t->spec_state().store(2, std::memory_order_release); }
  } done{translation};
  // FABLE2_NATIVE_FIRST: no converter library to bake; the guard entries
  // come from the native translation itself (no Metal Toolchain needed).
  const bool native_primary = translation->is_native_primary();
  if (!native_primary && !AirOptimizerCacheReadable()) {
    return;
  }
  // FABLE2_NATIVE_XLATE: the variant must match the library the shader ends
  // up with, so wait (background thread) until the native translation of this
  // shader has been decided.
  if (NativeXlateMode()) {
    for (int i = 0; i < 5000 && translation->native_shader_state().load(
                                    std::memory_order_acquire) != 2;
         ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  const uint64_t ucode = translation->shader().ucode_data_hash();
  const uint64_t modification = translation->modification();
  const ns_spec::PlainKey& plain = translation->plain_key();
  translation->spec_needs_learn().store(0, std::memory_order_relaxed);
  ns_spec::LearnRecord learned;
  bool blacklisted = false;
  const ns_spec::LearnRecord* record =
      ns_spec::GetLearnRecord(plain, ucode, modification, &learned,
                              &blacklisted)
          ? &learned
          : nullptr;
  if (blacklisted) {
    ns_spec_none_.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("[ns-spec] PS {:016X}/{:X}: demoted {} times, not specialized "
                "(delete the learn file to retry)",
                ucode, modification, ns_spec::LearnRecord::kMaxDemotions);
    return;
  }
  // FABLE2_NATIVE_FIRST: a new key whose record so far comes only from the
  // converter-keyed siblings (other resolution scales). Their render-target
  // texture sizes in the fetch constants differ from this scale's, so a
  // variant baked from them misses its guard and is demoted (measured: Bower
  // 2x, ~1000 misses, 1 demotion). Learn this key's own statistics first
  // (auto-learn, as for any unlearned shader); a variant stored for this key
  // by an earlier session is still used.
  if (native_primary && record && ns_spec::AutoLearnEnabled() &&
      !ns_spec::HasConvergedRecord(plain)) {
    record = nullptr;
  }
  const uint64_t learn_hash = record ? ns_spec::LearnHash(*record) : 0;
  // Without tools a variant can't be regenerated: a stored one (e.g. from
  // the portable seed) is used even when the learn data has changed since,
  // unless it was demoted; the guard keeps it exact either way.
  const bool can_generate = native_primary || AirOptimizerAvailable();
  MetalArtifactStore* store =
      REXCVAR_GET(metal_shader_disk_cache) ? g_metal_artifact_store.get()
                                           : nullptr;

  std::vector<ns_spec::GuardEntry> entries;
  std::vector<uint8_t> library_bytes;
  bool from_cache = false;
  if (store) {
    std::vector<uint8_t> index_bytes;
    uint64_t stored_learn_hash = 0;
    if (store->LoadDxilBytecode(Ns2Key(plain, kNs2IndexVersion, 0),
                                &index_bytes) &&
        ns_spec::ParseIndex(index_bytes, &stored_learn_hash, &entries) &&
        (!record || stored_learn_hash == learn_hash ||
         (!can_generate && !ns_spec::Demotions(plain)))) {
      if (entries.empty()) {
        ns_spec_none_.fetch_add(1, std::memory_order_relaxed);
        return;  // learned before: nothing to specialize
      }
      from_cache = native_primary ||
                   (store->LoadDxilBytecode(
                        Ns2Key(plain, kNs2LibraryVersion,
                               ns_spec::HashEntries(entries)),
                        &library_bytes) &&
                    !library_bytes.empty());
    }
  }
  uint64_t generate_ms = 0;
  if (!from_cache) {
    entries.clear();
    if (!record || !can_generate) {
      // Not learned (or no tools): stays on the normal pipeline. Auto-learn
      // samples it and asks again once the statistics converge.
      if (!record && can_generate && ns_spec::AutoLearnEnabled()) {
        translation->spec_needs_learn().store(1, std::memory_order_release);
      }
      return;
    }
    std::vector<ns_spec::GuardEntry> candidates = record->Candidates();
    std::string error;
    bool ok = false;
    if (!candidates.empty() && native_primary) {
      // The candidates the native code reads, with their learned masks.
      std::vector<MslBakedDword> baked;
      baked.reserve(candidates.size());
      for (const ns_spec::GuardEntry& e : candidates) {
        baked.push_back({e.kind, e.index, e.value, e.mask});
      }
      std::vector<MslBakedDword> used;
      // The dwords the native source reads include some the compiled code
      // never uses (dead after optimization); guarding those only adds
      // misses. Where converter-keyed variants of this shader were built
      // before (NS2 index records of the sibling keys), keep only the dwords
      // and bits they baked: the optimizer had proven those live.
      std::unordered_map<uint64_t, uint32_t> live;
      if (store) {
        for (const ns_spec::PlainKey& k :
             ns_spec::SiblingKeys(ucode, modification)) {
          if (k == plain) continue;
          std::vector<uint8_t> index_bytes;
          std::vector<ns_spec::GuardEntry> sibling_entries;
          uint64_t unused_hash = 0;
          if (store->LoadDxilBytecode(Ns2Key(k, kNs2IndexVersion, 0),
                                      &index_bytes) &&
              ns_spec::ParseIndex(index_bytes, &unused_hash,
                                  &sibling_entries)) {
            for (const ns_spec::GuardEntry& e : sibling_entries) {
              live[(uint64_t(e.kind) << 32) | e.index] |= e.mask;
            }
          }
        }
      }
      if (!live.empty()) {
        std::vector<MslBakedDword> narrowed;
        for (MslBakedDword b : baked) {
          auto it = live.find((uint64_t(b.buffer) << 32) | b.index);
          if (it == live.end() || !(b.mask & it->second)) continue;
          b.mask &= it->second;
          b.value &= b.mask;
          narrowed.push_back(b);
        }
        baked = std::move(narrowed);
      }
      if (baked.empty()) {
        error = "nothing to bake (no live candidate dword)";
      } else if (!NativeXlateBakedUse(translation, baked, &used)) {
        error = "native translation failed";
      } else if (used.empty()) {
        error = "nothing to bake (no candidate dword is read)";
      } else {
        for (const MslBakedDword& b : used) {
          ns_spec::GuardEntry e;
          e.kind = b.buffer;
          e.index = b.index;
          e.value = b.value;
          e.mask = b.mask;
          entries.push_back(e);
        }
        ok = true;
      }
    } else if (!candidates.empty()) {
      auto start = std::chrono::steady_clock::now();
      ok = SpecializeMetallibAir(
          translation->metallib_data(),
          [&](std::string& ll, std::string* e) {
            return ns_spec::BakeAir(ll, candidates, ns_spec::BakeFloats(),
                                    &entries, e);
          },
          library_bytes, &error);
      generate_ms = ElapsedMs(start, std::chrono::steady_clock::now());
      ns_spec_ms_.fetch_add(generate_ms, std::memory_order_relaxed);
    } else {
      error = "nothing to bake (no invariant dwords)";
    }
    if (!ok) {
      if (error.rfind("nothing to bake", 0) == 0) {
        entries.clear();
        if (store) {
          store->StoreDxilBytecode(Ns2Key(plain, kNs2IndexVersion, 0),
                                   ns_spec::SerializeIndex(learn_hash, {}));
        }
        ns_spec_none_.fetch_add(1, std::memory_order_relaxed);
        REXLOG_INFO("[ns-spec] PS {:016X}/{:X}: {}", ucode, modification,
                    error);
      } else {
        ns_spec_failed_.fetch_add(1, std::memory_order_relaxed);
        REXLOG_WARN("[ns-spec] PS {:016X}/{:X}: specialization failed ({}); "
                    "the normal pipeline stays",
                    ucode, modification, error);
      }
      return;
    }
    if (store) {
      if (!native_primary) {
        store->StoreDxilBytecode(Ns2Key(plain, kNs2LibraryVersion,
                                        ns_spec::HashEntries(entries)),
                                 library_bytes);
      }
      store->StoreDxilBytecode(Ns2Key(plain, kNs2IndexVersion, 0),
                               ns_spec::SerializeIndex(learn_hash, entries));
    }
  }

  MTL::Library* library = nullptr;
  MTL::Function* function = nullptr;
  bool native_variant = false;
  if (translation->is_native_xlate()) {
    // The same guarded bits, baked into the native MSL translation.
    std::vector<MslBakedDword> baked;
    baked.reserve(entries.size());
    for (const ns_spec::GuardEntry& e : entries) {
      baked.push_back({e.kind, e.index, e.value, e.mask});
    }
    std::string entry;
    native_variant =
        CompileNativeXlate(translation, baked, &library, &function, &entry);
  }
  if (!native_variant && native_primary) {
    ns_spec_failed_.fetch_add(1, std::memory_order_relaxed);
    REXLOG_WARN("[ns-spec] PS {:016X}/{:X}: native specialized variant not "
                "compiled; the normal pipeline stays",
                ucode, modification);
    return;
  }
  if (!native_variant) {
    dispatch_data_t data =
        dispatch_data_create(library_bytes.data(), library_bytes.size(),
                             nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NS::Error* error = nullptr;
    library = device_->newLibrary(data, &error);
    dispatch_release(data);
    function = library ? library->newFunction(NS::String::string(
                             translation->function_name().c_str(),
                             NS::UTF8StringEncoding))
                       : nullptr;
  }
  if (!function) {
    if (library) library->release();
    ns_spec_failed_.fetch_add(1, std::memory_order_relaxed);
    REXLOG_WARN("[ns-spec] PS {:016X}/{:X}: specialized library rejected; "
                "the normal pipeline stays",
                ucode, modification);
    return;
  }
  static const bool shader_labels = [] {
    const char* v = std::getenv("FABLE2_SHADER_LABELS");
    return v && v[0] == '1';
  }();
  if (shader_labels) {
    char label[64];
    std::snprintf(label, sizeof(label), "ps:%016llX/%llX/spec",
                  (unsigned long long)ucode, (unsigned long long)modification);
    library->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
  }
  auto variant = std::make_unique<ns_spec::Variant>();
  ns_spec::SetVariantEntries(*variant, entries);
  variant->library = library;
  variant->function = function;
  variant->native = native_variant;
  variant->ucode_hash = ucode;
  variant->modification = modification;
  uint32_t per_kind[ns_spec::kKindCount] = {};
  for (const auto& e : entries) ++per_kind[e.kind];
  ns_spec::RegisterVariant(variant.get());
  translation->PublishSpecVariant(std::move(variant));
  (from_cache ? ns_spec_from_cache_ : ns_spec_generated_)
      .fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO(
      "[ns-spec] PS {:016X}/{:X}: specialized{}, {} guarded dwords (system {}, "
      "bool/loop {}, fetch {}), {}",
      ucode, modification, native_variant ? " (native MSL)" : "",
      entries.size(), per_kind[0], per_kind[1], per_kind[2],
      from_cache ? std::string("from cache")
                 : fmt::format("generated in {} ms", generate_ms));
}

void MetalPipelineCache::NsSpecBuild(PipelineHandle* handle) {
  handle->WaitForCreation();
  const ns_spec::Variant* variant =
      handle->ns_pixel_translation
          ? handle->ns_pixel_translation->spec_variant()
          : nullptr;
  MTL::RenderPipelineState* pipeline = nullptr;
  if (variant && handle->state.load(std::memory_order_acquire)) {
    bool optimized = false;
    const uint64_t cpu_start = ThreadCpuUs();
    pipeline = CreatePipelineFromHandle(handle, handle->ns_vertex_translation,
                                        handle->ns_pixel_translation,
                                        &optimized, variant->function);
    ns2_build_cpu_us_.fetch_add(ThreadCpuUs() - cpu_start,
                                std::memory_order_relaxed);
  }
  if (!pipeline) {
    const uint64_t n =
        ns_spec_pipeline_failed_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4) {
      REXLOG_WARN("[ns-spec] specialized pipeline not created; the normal "
                  "pipeline stays (#{})",
                  n);
    }
  } else {
    handle->ns_spec_state.store(pipeline, std::memory_order_release);
    const uint64_t n =
        ns_spec_pipelines_.fetch_add(1, std::memory_order_relaxed) + 1;
    LogNs2Pipeline(n, false);
    if (n == 1 || (n & (n - 1)) == 0) {
      LogSpecProgress("specialized pipelines");
    }
  }
  handle->ns_spec_status.store(2, std::memory_order_release);
}

void MetalPipelineCache::NsSpecBuildEmu(const std::shared_ptr<NsEmuSwap>& swap) {
  const ns_spec::Variant* variant =
      swap->pixel_translation ? swap->pixel_translation->spec_variant()
                              : nullptr;
  const uint64_t cpu_start = ThreadCpuUs();
  MTL::RenderPipelineState* pipeline =
      variant && swap->build ? swap->build(variant->library) : nullptr;
  ns2_build_cpu_us_.fetch_add(ThreadCpuUs() - cpu_start,
                              std::memory_order_relaxed);
  if (!pipeline) {
    const uint64_t n =
        ns_spec_pipeline_failed_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4) {
      REXLOG_WARN("[ns-spec] specialized {} emulation pipeline not created; "
                  "the normal pipeline stays (#{})",
                  swap->tessellation ? "tessellation" : "geometry", n);
    }
  } else {
    swap->spec.store(pipeline, std::memory_order_release);
    const uint64_t n =
        ns_spec_pipelines_.fetch_add(1, std::memory_order_relaxed) + 1;
    LogNs2Pipeline(n, true);
    if (n == 1 || (n & (n - 1)) == 0) {
      LogSpecProgress("specialized pipelines");
    }
  }
  swap->spec_status.store(2, std::memory_order_release);
}

void MetalPipelineCache::NativeShaderThread() {
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
  for (;;) {
    PipelineHandle* swap = nullptr;
    std::shared_ptr<NsEmuSwap> emu_swap;
    MetalShader::MetalTranslation* translation = nullptr;
    PipelineHandle* spec_build = nullptr;
    std::shared_ptr<NsEmuSwap> spec_emu_build;
    MetalShader::MetalTranslation* spec_translation = nullptr;
    {
      std::unique_lock<std::mutex> lock(ns_mutex_);
      ns_cond_.wait(lock, [this] {
        return ns_shutdown_ || !ns_swap_queue_.empty() ||
               !ns_emu_swap_queue_.empty() || !ns_optimize_queue_.empty() ||
               !ns_spec_queue_.empty() || !ns_spec_build_queue_.empty() ||
               !ns_spec_emu_queue_.empty();
      });
      if (ns_shutdown_) {
        break;
      }
      // FABLE2_NS_SPECIALIZE work comes after the optimized-library swaps:
      // first specialized pipeline builds (a draw is waiting for them), then
      // optimization, then specialization (cache loads or generation).
      if (ns_swap_queue_.empty() && ns_emu_swap_queue_.empty() &&
          (!ns_spec_build_queue_.empty() || !ns_spec_emu_queue_.empty())) {
        if (!ns_spec_build_queue_.empty()) {
          spec_build = ns_spec_build_queue_.front();
          ns_spec_build_queue_.erase(ns_spec_build_queue_.begin());
        } else {
          spec_emu_build = std::move(ns_spec_emu_queue_.front());
          ns_spec_emu_queue_.erase(ns_spec_emu_queue_.begin());
        }
      } else if (ns_swap_queue_.empty() && ns_emu_swap_queue_.empty() &&
                 ns_optimize_queue_.empty()) {
        // Hottest first, as for optimization.
        size_t best = 0;
        uint32_t best_uses = ns_spec_queue_[0]->use_count();
        for (size_t i = 1; i < ns_spec_queue_.size(); ++i) {
          uint32_t uses = ns_spec_queue_[i]->use_count();
          if (uses > best_uses) {
            best = i;
            best_uses = uses;
          }
        }
        spec_translation = ns_spec_queue_[best];
        ns_spec_queue_.erase(ns_spec_queue_.begin() + best);
      } else if (!ns_emu_swap_queue_.empty() && ns_swap_queue_.empty()) {
        emu_swap = std::move(ns_emu_swap_queue_.front());
        ns_emu_swap_queue_.erase(ns_emu_swap_queue_.begin());
      } else if (!ns_swap_queue_.empty()) {
        // Swaps first: they deliver an already-optimized shader.
        swap = ns_swap_queue_.front();
        ns_swap_queue_.erase(ns_swap_queue_.begin());
      } else {
        // Hottest shader first (draw lookups so far); ties keep queue order,
        // which is warm-up order.
        size_t best = 0;
        uint32_t best_uses = ns_optimize_queue_[0]->use_count();
        for (size_t i = 1; i < ns_optimize_queue_.size(); ++i) {
          uint32_t uses = ns_optimize_queue_[i]->use_count();
          if (uses > best_uses) {
            best = i;
            best_uses = uses;
          }
        }
        translation = ns_optimize_queue_[best];
        ns_optimize_queue_.erase(ns_optimize_queue_.begin() + best);
      }
      ++ns_busy_;
    }
    pool->drain();
    pool = NS::AutoreleasePool::alloc()->init();

    if (spec_build) {
      NsSpecBuild(spec_build);
    } else if (spec_emu_build) {
      NsSpecBuildEmu(spec_emu_build);
    } else if (spec_translation) {
      NsSpecialize(spec_translation);
    } else if (emu_swap) {
      NsEmuRebuild(emu_swap);
    } else if (swap) {
      NativeShaderSwap(swap);
    } else {
      std::vector<uint8_t> optimized;
      bool ok = NativeXlateMode() && TryNativeXlate(translation);
      // Cache-only (portable build without Xcode): load, never optimize.
      if (!ok && AirOptimizerCacheReadable() &&
          translation->shader().type() == xenos::ShaderType::kPixel &&
          !NativeShaderKeepPlain(translation->shader().ucode_data_hash())) {
        const std::vector<uint8_t>& plain = translation->metallib_data();
        ok = LoadOptimizedPixelShader(plain, &optimized) ||
             OptimizeGuestPixelShader(translation->shader().ucode_data_hash(),
                                      plain, &optimized);
        if (ok && !translation->InstallOptimizedMetal(device_, optimized)) {
          ok = false;
          const uint64_t n = native_shaders_fallbacks_.fetch_add(
                                 1, std::memory_order_relaxed) +
                             1;
          REXLOG_WARN(
              "[native-shaders] PS {:016X}: optimized library rejected; "
              "plain library stays (fallback #{})",
              translation->shader().ucode_data_hash(), n);
        }
      }
      uint64_t ready = 0;
      if (ok) {
        ready = native_shaders_ready_.fetch_add(1, std::memory_order_relaxed) + 1;
      }
      {
        std::lock_guard<std::mutex> lock(ns_mutex_);
        translation->native_shader_state().store(2);
        auto it = ns_handles_.find(translation);
        if (it != ns_handles_.end()) {
          if (ok) {
            for (PipelineHandle* handle : it->second) {
              const bool wanted =
                  translation->shader().type() == xenos::ShaderType::kVertex
                      ? !handle->ns_vs_native.load(std::memory_order_acquire)
                      : !handle->ns_optimized.load(std::memory_order_acquire);
              if (wanted && !handle->ns_swap_queued.exchange(true)) {
                ns_swap_queue_.push_back(handle);
              }
            }
          }
          ns_handles_.erase(it);
        }
        auto emu_it = ns_emu_handles_.find(translation);
        if (emu_it != ns_emu_handles_.end()) {
          if (ok) {
            for (auto& emu : emu_it->second) {
              if (!emu->queued.exchange(true)) {
                ns_emu_swap_queue_.push_back(emu);
              }
            }
          }
          ns_emu_handles_.erase(emu_it);
        }
      }
      ns_cond_.notify_all();
      if (ready == 1 || (ready && (ready & (ready - 1)) == 0)) {
        LogNativeShaderProgress("optimized");
      }
    }

    bool idle = false;
    {
      std::lock_guard<std::mutex> lock(ns_mutex_);
      --ns_busy_;
      ++ns_items_since_idle_log_;
      // Log the first drain, then only drains that did real work (shaders
      // first met in play arrive one at a time).
      if (!ns_busy_ && ns_swap_queue_.empty() && ns_emu_swap_queue_.empty() &&
          ns_optimize_queue_.empty() && ns_spec_queue_.empty() &&
          ns_spec_build_queue_.empty() && ns_spec_emu_queue_.empty() &&
          !ns_idle_logged_ &&
          (!ns_first_idle_logged_ || ns_items_since_idle_log_ >= 16)) {
        ns_idle_logged_ = true;
        ns_first_idle_logged_ = true;
        ns_items_since_idle_log_ = 0;
        idle = true;
      }
    }
    if (idle) {
      LogNativeShaderProgress("background work done");
      if (ns_spec_queued_.load(std::memory_order_relaxed)) {
        LogSpecProgress("background work done");
      }
    }
  }
  pool->drain();
}

namespace {
// FABLE2_NATIVE_FIRST: the key of a shader's specialization learn data and
// NS2 records when no converter library exists. The DXBC translation's hash
// (it encodes the ucode, the modification and the resolution scale, like the
// converter library did), tagged so it never equals a converter-library key.
// Learn data of the converter keys still applies through the sibling lookup
// (same ucode hash and modification).
ns_spec::PlainKey NativeFirstPlainKey(
    MetalShader::MetalTranslation* translation) {
  const std::vector<uint8_t>& dxbc = translation->translated_binary();
  ns_spec::PlainKey key = ns_spec::ComputePlainKey(dxbc);
  if (key.valid()) {
    key.low ^= UINT64_C(0x4E46495253544E46);   // "NFIRSTNF"
    key.high ^= UINT64_C(0x6E61746976656631);  // "native f1"
    key.size |= UINT64_C(1) << 62;
  }
  return key;
}
}  // namespace

int MetalPipelineCache::TryNativeFirst(
    MetalShader::MetalTranslation* translation) {
  const bool vertex =
      translation->shader().type() == xenos::ShaderType::kVertex;
  if (vertex ? !NativeXlateVertexMode()
             : translation->shader().type() != xenos::ShaderType::kPixel) {
    return 0;
  }
  // The texture / sampler binding lists (descriptor-index layout) come from
  // the DXBC translation (the SDK's translator; no converter involved).
  if (!EnsureDxbcTranslationReady(translation,
                                  vertex ? "native vertex" : "native pixel")) {
    return 0;
  }
  MTL::Library* library = nullptr;
  MTL::Function* function = nullptr;
  std::string entry;
  if (!CompileNativeXlate(translation, {}, &library, &function, &entry)) {
    return 0;
  }
  if (!translation->InstallNativePrimary(library, function, entry)) {
    // Another thread installed this shader first.
    function->release();
    library->release();
    return -1;
  }
  (vertex ? native_xlate_ok_vs_ : native_xlate_ok_ps_)
      .fetch_add(1, std::memory_order_relaxed);
  const uint64_t n =
      native_xlate_ok_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || (n & (n - 1)) == 0) {
    REXLOG_INFO(
        "[native-first] {} native shaders installed ({} pixel, {} vertex; {} "
        "translator fallbacks, {} compile failures); translate {} ms, MSL "
        "compile {} ms total, max {} ms",
        n, native_xlate_ok_ps_.load(std::memory_order_relaxed),
        native_xlate_ok_vs_.load(std::memory_order_relaxed),
        native_xlate_fallback_.load(std::memory_order_relaxed),
        native_xlate_compile_failed_.load(std::memory_order_relaxed),
        native_xlate_translate_us_.load(std::memory_order_relaxed) / 1000,
        native_xlate_compile_us_.load(std::memory_order_relaxed) / 1000,
        native_xlate_compile_max_us_.load(std::memory_order_relaxed) / 1000);
  }
  // Nothing to optimize or swap later; pipelines registered for a swap
  // before the install get their native function at creation.
  translation->native_shader_state().store(2, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(ns_mutex_);
    ns_handles_.erase(translation);
    ns_emu_handles_.erase(translation);
  }
  if (!vertex && NativeShadersMode() &&
      (ns_spec::Enabled() || ns_spec::LearnMode())) {
    translation->SetPlainKey(NativeFirstPlainKey(translation));
    if (ns_spec::Enabled() &&
        ns_spec::Candidate(translation->shader().ucode_data_hash())) {
      QueueSpecialization(translation);
    }
  }
  return 1;
}

bool MetalPipelineCache::EnsureMetalTranslationReady(
    MetalShader::MetalTranslation* translation) {
  if (!translation) {
    return true;
  }
  if (translation->is_valid()) {
    return true;
  }
  hitch::Scope hitch_scope(hitch::kXlate);
  // [xlate-trace] one line per translation that took >= 10 ms (which shader, which modification, which thread),
  // so first-play hitches can be attributed (docs/shader-precompile-20261015.md). FABLE2_XLATE_TRACE=0 off.
  struct XlateTrace {
    MetalShader::MetalTranslation* t;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~XlateTrace() {
      static const bool on = [] {
        const char* v = std::getenv("FABLE2_XLATE_TRACE");
        return !(v && v[0] == '0');
      }();
      const double ms = startup::MsSince(start);
      if (on && ms >= 10.0) {
        REXLOG_INFO("[xlate-trace] {} {:016X}/{:X} {:.1f} ms{}",
                    t->shader().type() == xenos::ShaderType::kVertex ? "VS" : "PS",
                    t->shader().ucode_data_hash(), t->modification(), ms, t->is_valid() ? "" : " (failed)");
      }
    }
  } xlate_trace{translation};
  converter_census::Scope census_scope(
      translation->shader().type() == xenos::ShaderType::kVertex
          ? converter_census::kGuestVS
          : converter_census::kGuestPS,
      translation->shader().ucode_data_hash());
  // FABLE2_NATIVE_FIRST (docs/no-converter.md): the native translation is
  // the shader's library from the start; DXIL and the converter never run
  // for it.
  if (NativeFirstMode()) {
    const int native = TryNativeFirst(translation);
    if (native > 0) {
      return true;
    }
    if (native < 0) {
      return translation->is_valid();
    }
    if (!ConverterBuiltIn()) {
      converter_census::Refused();
      return false;
    }
  }
  if (!EnsureDxilTranslationReady(translation, "guest")) {
    return false;
  }

  MetalShaderStage stage;
  switch (translation->shader().type()) {
    case xenos::ShaderType::kVertex:
      stage = MetalShaderStage::kVertex;
      break;
    case xenos::ShaderType::kPixel:
      stage = MetalShaderStage::kFragment;
      break;
    default:
      REXLOG_ERROR("Unsupported Metal shader type {}",
             uint32_t(translation->shader().type()));
      return false;
  }

  MetalStageCompileRequest request;
  request.stage = stage;
  request.dxil_data =
      std::make_shared<std::vector<uint8_t>>(translation->GetDxilDataCopy());
  request.requested_outputs = kMetalStageCompileOutputMetallib;
  std::shared_ptr<const MetalStageCompileResult> result =
      metal_shader_converter_->CompileStage(request);
  if (!result || !result->success) {
    REXLOG_ERROR("Failed to translate shader {:016X} to Metal: {}",
           translation->shader().ucode_data_hash(),
           result ? result->error_message : "unknown error");
    return false;
  }
  uint64_t library_ms = 0;
  bool library_created = false;
  // FABLE2_NATIVE_SHADERS (docs/native-shaders.md): a guest pixel shader whose
  // optimized library is already in the disk cache installs it directly
  // (warm launches: optimized pipelines come straight from the ".ns1"
  // archive). Otherwise the plain library is installed now, exactly as with
  // the switch off, and the shader is optimized in the background; its
  // pipelines are swapped once that is done. Nothing here runs the tools.
  bool queue_optimization = false;
  bool installed = false;
  // FABLE2_NS_SPECIALIZE: key of the plain library (learn data and NS2
  // records), whatever library ends up installed.
  if (NativeShadersMode() && stage == MetalShaderStage::kFragment &&
      (ns_spec::Enabled() || ns_spec::LearnMode())) {
    translation->SetPlainKey(ns_spec::ComputePlainKey(result->metallib_data));
  }
  if (NativeXlateMode() && (stage == MetalShaderStage::kFragment ||
                            (stage == MetalShaderStage::kVertex &&
                             NativeXlateVertexMode()))) {
    // FABLE2_NATIVE_XLATE: the background pool translates to MSL first and
    // falls back to the cached / -O2 library per shader (vertex shaders:
    // the converter's library stays).
    queue_optimization = true;
  } else if (NativeShadersMode() && stage == MetalShaderStage::kFragment &&
      !NativeShaderKeepPlain(translation->shader().ucode_data_hash())) {
    // Never wait for the tool lookup here: if it has not finished, install
    // plain; the background pool then finds the cached library and swaps.
    std::vector<uint8_t> optimized_bytes;
    if (AirOptimizerLookupDone() &&
        LoadOptimizedPixelShader(result->metallib_data, &optimized_bytes)) {
      auto optimized = std::make_shared<MetalStageCompileResult>(*result);
      optimized->metallib_data = std::move(optimized_bytes);
      installed = translation->InstallMetal(device_, *optimized, &library_ms,
                                            &library_created);
      if (installed && library_created) {
        translation->MarkInstalledAsOptimized();
        native_shaders_cache_hits_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t n =
            native_shaders_optimized_.fetch_add(1, std::memory_order_relaxed) +
            1;
        if (n == 1 || (n & (n - 1)) == 0) {
          REXLOG_INFO(
              "[native-shaders] optimized pixel shaders installed from cache: "
              "{}",
              n);
        }
        result = optimized;
      } else if (!installed) {
        const uint64_t n = native_shaders_fallbacks_.fetch_add(
                               1, std::memory_order_relaxed) +
                           1;
        REXLOG_WARN(
            "[native-shaders] PS {:016X}: cached optimized library rejected; "
            "plain library (fallback #{})",
            translation->shader().ucode_data_hash(), n);
      }
    } else {
      queue_optimization = true;
    }
  }
  if (!installed) {
    installed = translation->InstallMetal(device_, *result, &library_ms,
                                          &library_created);
  }
  if (installed && queue_optimization) {
    QueueNativeShaderOptimization(translation);
  }
  if (installed && NativeShadersMode() && ns_spec::Enabled() &&
      stage == MetalShaderStage::kFragment &&
      ns_spec::Candidate(translation->shader().ucode_data_hash())) {
    QueueSpecialization(translation);
  }
  if (library_created) {
    converter_census::Library(converter_census::CurrentPurpose());
    RecordLibraryCreation(result->metallib_data.size(),
                          translation->metal_library() != nullptr, library_ms);
  }
  if (!installed) {
    return false;
  }

  return true;
}

bool MetalPipelineCache::EnsureDxbcTranslationReady(
    MetalShader::MetalTranslation* translation, const char* stage_name) {
  if (!translation) {
    return true;
  }
  hitch::Scope hitch_scope(hitch::kXlate);
  std::lock_guard<std::mutex> lock(shader_translation_mutex_);
  return EnsureDxbcTranslationReadyLocked(translation, stage_name);
}

MetalPipelineCache::PipelineHandle*
MetalPipelineCache::GetOrCreatePipelineState(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  if (!vertex_translation) {
    REXLOG_ERROR("No vertex shader translation");
    return nullptr;
  }
  MetalPipelineDescription description = BuildStandardPipelineDescription(
      vertex_translation, pixel_translation, attachment_formats, rendering_key);
  uint64_t key = XXH3_64bits(&description, sizeof(description));

  // Check cache.
  auto it = pipeline_cache_.find(key);
  if (it != pipeline_cache_.end()) {
    if (it->second->ns_pixel_translation) {
      it->second->ns_pixel_translation->NoteUse();
    }
    return it->second.get();
  }

  // Create a new handle.
  auto handle = std::make_unique<PipelineHandle>();
  handle->description_hash = key;
  handle->description = description;
  handle->pending_vertex_translation = vertex_translation;
  handle->pending_pixel_translation = pixel_translation;
  handle->pending_formats = attachment_formats;
  handle->pending_normalized_color_mask = description.normalized_color_mask;
  std::memcpy(handle->pending_blendcontrol, description.blendcontrol,
              sizeof(description.blendcontrol));
  handle->pending_alpha_to_mask = (description.alpha_to_mask_enable != 0);
  handle->storage_write_pending = true;

  PipelineHandle* raw_handle = handle.get();
  pipeline_cache_.emplace(key, std::move(handle));
  if (NativeShadersMode() && pixel_translation) {
    raw_handle->ns_vertex_translation = vertex_translation;
    raw_handle->ns_pixel_translation = pixel_translation;
    pixel_translation->NoteUse();
    RegisterNativeShaderHandle(raw_handle);
  }

  // Async path: enqueue for background compilation.
  if (REXCVAR_GET(async_shader_compilation) && !creation_threads_.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_queue_.push(PipelineCreationRequest{raw_handle});
    }
    creation_request_cond_.notify_one();
    return raw_handle;
  }

  // Synchronous path: create immediately.
  bool optimized = false;
  bool vertex_native = false;
  MTL::RenderPipelineState* pipeline = CreatePipelineFromHandle(
      raw_handle, vertex_translation, pixel_translation, &optimized, nullptr,
      &vertex_native);
  raw_handle->ns_vs_native.store(vertex_native, std::memory_order_release);
  if (pipeline && raw_handle->ns_pixel_translation) {
    if (optimized) {
      raw_handle->ns_optimized.store(true, std::memory_order_release);
    } else {
      native_shaders_plain_pipelines_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  raw_handle->state.store(pipeline, std::memory_order_release);
  raw_handle->pending_vertex_translation = nullptr;
  raw_handle->pending_pixel_translation = nullptr;
  raw_handle->creation_complete.store(true, std::memory_order_release);
  raw_handle->creation_complete.notify_all();

  if (!pipeline) {
    return nullptr;
  }

  if (raw_handle->storage_write_pending) {
    QueueStoredShader(static_cast<MetalShader&>(vertex_translation->shader()));
    if (pixel_translation) {
      QueueStoredShader(static_cast<MetalShader&>(pixel_translation->shader()));
    }
    QueueStoredPipeline(raw_handle->description, raw_handle->description_hash);
    raw_handle->storage_write_pending = false;
  }

  return raw_handle;
}

// ---------------------------------------------------------------------------
// CreatePipelineFromHandle (builds a MTL::RenderPipelineState from a handle)
// ---------------------------------------------------------------------------

MTL::RenderPipelineState* MetalPipelineCache::CreatePipelineFromHandle(
    const PipelineHandle* handle,
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation, bool* out_optimized,
    MTL::Function* fragment_override, bool* out_vertex_native) {
  ScopedAutoreleasePool autorelease_pool;
  if (out_vertex_native) {
    *out_vertex_native = false;
  }
  const PipelineAttachmentFormats& formats = handle->pending_formats;
  if (out_optimized) {
    *out_optimized = false;
  }

  if (!EnsureMetalTranslationReady(vertex_translation) ||
      !EnsureMetalTranslationReady(pixel_translation)) {
    return nullptr;
  }

  // Create pipeline descriptor.
  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();

  // FABLE2_NATIVE_XLATE: the native vertex function once it is ready.
  MTL::Function* vertex_function = vertex_translation->best_function();
  desc->setVertexFunction(vertex_function);
  if (out_vertex_native) {
    *out_vertex_native = vertex_translation->is_native_xlate() &&
                         vertex_function == vertex_translation->optimized_function();
  }

  // FABLE2_NATIVE_SHADERS: the optimized pixel function once it is ready
  // (docs/native-shaders.md); such pipelines use the ".ns1" archive set.
  bool fragment_optimized = false;
  if (fragment_override) {
    // FABLE2_NS_SPECIALIZE: specialized pixel function (".ns2" archive set).
    desc->setFragmentFunction(fragment_override);
  } else if (pixel_translation) {
    MTL::Function* fragment = pixel_translation->best_function();
    if (fragment) {
      desc->setFragmentFunction(fragment);
      fragment_optimized = fragment == pixel_translation->optimized_function();
    }
  }

  // Trace-only (FABLE2_SHADER_LABELS=1): name the pipeline after its guest
  // shaders so GPU captures (gpucapture/gpudebug) can attribute cost to them.
  static const bool shader_labels = [] {
    const char* v = std::getenv("FABLE2_SHADER_LABELS");
    return v && v[0] == '1';
  }();
  if (shader_labels) {
    char label[96];
    std::snprintf(
        label, sizeof(label), "vs:%016llX/%llX ps:%016llX/%llX",
        (unsigned long long)vertex_translation->shader().ucode_data_hash(),
        (unsigned long long)vertex_translation->modification(),
        (unsigned long long)(pixel_translation
                                 ? pixel_translation->shader().ucode_data_hash()
                                 : 0),
        (unsigned long long)(pixel_translation
                                 ? pixel_translation->modification()
                                 : 0));
    desc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
  }

  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(
        formats.color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(formats.depth_format);
  desc->setStencilAttachmentPixelFormat(formats.stencil_format);
  desc->setSampleCount(formats.sample_count);
  desc->setAlphaToCoverageEnabled(false);

  ApplyBlendStateToDescriptor(desc->colorAttachments(),
                              handle->pending_normalized_color_mask,
                              handle->pending_blendcontrol);

  // Configure vertex fetch layout for MSC stage-in.
  const Shader& vertex_shader_ref = vertex_translation->shader();
  const auto& vertex_bindings = vertex_shader_ref.vertex_bindings();
  if (!ShaderUsesVertexFetch(vertex_shader_ref) && !vertex_bindings.empty()) {
    auto map_vertex_format =
        [](const ParsedVertexFetchInstruction::Attributes& attrs)
        -> MTL::VertexFormat {
      using xenos::VertexFormat;
      switch (attrs.data_format) {
        case VertexFormat::k_8_8_8_8:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatChar4
                                   : MTL::VertexFormatUChar4;
          }
          return attrs.is_signed ? MTL::VertexFormatChar4Normalized
                                 : MTL::VertexFormatUChar4Normalized;
        case VertexFormat::k_2_10_10_10:
          return attrs.is_signed ? MTL::VertexFormatInt1010102Normalized
                                 : MTL::VertexFormatUInt1010102Normalized;
        case VertexFormat::k_10_11_11:
        case VertexFormat::k_11_11_10:
          return MTL::VertexFormatFloatRG11B10;
        case VertexFormat::k_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatShort2
                                   : MTL::VertexFormatUShort2;
          }
          return attrs.is_signed ? MTL::VertexFormatShort2Normalized
                                 : MTL::VertexFormatUShort2Normalized;
        case VertexFormat::k_16_16_16_16:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatShort4
                                   : MTL::VertexFormatUShort4;
          }
          return attrs.is_signed ? MTL::VertexFormatShort4Normalized
                                 : MTL::VertexFormatUShort4Normalized;
        case VertexFormat::k_16_16_FLOAT:
          return MTL::VertexFormatHalf2;
        case VertexFormat::k_16_16_16_16_FLOAT:
          return MTL::VertexFormatHalf4;
        case VertexFormat::k_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatInt
                                   : MTL::VertexFormatUInt;
          }
          return MTL::VertexFormatFloat;
        case VertexFormat::k_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatInt2
                                   : MTL::VertexFormatUInt2;
          }
          return MTL::VertexFormatFloat2;
        case VertexFormat::k_32_FLOAT:
          return MTL::VertexFormatFloat;
        case VertexFormat::k_32_32_FLOAT:
          return MTL::VertexFormatFloat2;
        case VertexFormat::k_32_32_32_FLOAT:
          return MTL::VertexFormatFloat3;
        case VertexFormat::k_32_32_32_32:
          if (attrs.is_integer) {
            return attrs.is_signed ? MTL::VertexFormatInt4
                                   : MTL::VertexFormatUInt4;
          }
          return MTL::VertexFormatFloat4;
        case VertexFormat::k_32_32_32_32_FLOAT:
          return MTL::VertexFormatFloat4;
        default:
          return MTL::VertexFormatInvalid;
      }
    };

    MTL::VertexDescriptor* vertex_desc = MTL::VertexDescriptor::alloc()->init();

    uint32_t attr_index = static_cast<uint32_t>(kIRStageInAttributeStartIndex);
    for (const auto& binding : vertex_bindings) {
      uint64_t buffer_index =
          kIRVertexBufferBindPoint + uint64_t(binding.binding_index);
      bool used_any_attribute = false;

      for (const auto& attr : binding.attributes) {
        MTL::VertexFormat fmt = map_vertex_format(attr.fetch_instr.attributes);
        if (fmt == MTL::VertexFormatInvalid) {
          ++attr_index;
          continue;
        }
        auto attr_desc = vertex_desc->attributes()->object(attr_index);
        attr_desc->setFormat(fmt);
        attr_desc->setOffset(
            static_cast<NS::UInteger>(attr.fetch_instr.attributes.offset * 4));
        attr_desc->setBufferIndex(static_cast<NS::UInteger>(buffer_index));
        used_any_attribute = true;
        ++attr_index;
      }

      if (used_any_attribute) {
        auto layout = vertex_desc->layouts()->object(buffer_index);
        layout->setStride(binding.stride_words * 4);
        layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        layout->setStepRate(1);
      }
    }

    desc->setVertexDescriptor(vertex_desc);
    vertex_desc->release();
  }

  MTL::BinaryArchive* archive = nullptr;
  std::vector<MTL::BinaryArchive*> archives;
  bool compacting = false;
  ArchiveSet& archive_set =
      archive_sets_[fragment_override ? 2 : (fragment_optimized ? 1 : 0)];
  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    archives = archive_set.segments;
    if (archive_set.archive) {
      archive = archive_set.archive;
      archives.push_back(archive);
    }
    for (MTL::BinaryArchive* a : archives) {
      a->retain();
    }
    compacting = archive_set.compacting;
  }
  const bool have_segments = archives.size() > (archive ? 1u : 0u);
  // CR-05: some pipelines without a fragment function cannot be serialized
  // ("expecting 'fragment' stage") and poison the archive they are added to.
  // Never add one. Looking one up stays allowed: older segments do hold
  // fragment-less pipelines that serialized fine (22 hit at warm-up).
  const bool archive_skip =
      ArchiveSafeSaveMode() == 2 && desc->fragmentFunction() == nullptr;

  // Create pipeline state. With archive segments from earlier launches, first
  // look the pipeline up without compiling. On a miss, compile normally (in
  // parallel on the creation threads), then record it in this launch's
  // archive for the next launch.
  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline = nullptr;
  auto pipeline_start = std::chrono::steady_clock::now();
  if (!archives.empty()) {
    desc->setBinaryArchives(NS::Array::array(
        reinterpret_cast<const NS::Object* const*>(archives.data()),
        archives.size()));
    if (have_segments) {
      NS::Error* lookup_error = nullptr;
      pipeline = device_->newRenderPipelineState(
          desc, MTL::PipelineOptionFailOnBinaryArchiveMiss, nullptr,
          &lookup_error);
      uint64_t lookup_ms =
          ElapsedMs(pipeline_start, std::chrono::steady_clock::now());
      archive_lookup_ms_total_.fetch_add(lookup_ms, std::memory_order_relaxed);
      (pipeline ? archive_lookup_hits_ : archive_lookup_misses_)
          .fetch_add(1, std::memory_order_relaxed);
    }
  }
  bool compiled = false;
  if (!pipeline) {
    auto compile_start = std::chrono::steady_clock::now();
    pipeline = device_->newRenderPipelineState(desc, &error);
    compiled = true;
    pipeline_compile_count_.fetch_add(1, std::memory_order_relaxed);
    pipeline_compile_ms_total_.fetch_add(
        ElapsedMs(compile_start, std::chrono::steady_clock::now()),
        std::memory_order_relaxed);
  }
  auto pipeline_end = std::chrono::steady_clock::now();
  RecordRenderPipelineCreation(pipeline != nullptr,
                               ElapsedMs(pipeline_start, pipeline_end));
  if (fragment_override && pipeline) {
    if (compiled) {
      ns2_archive_compiled_.fetch_add(1, std::memory_order_relaxed);
      ns2_compile_ms_.fetch_add(ElapsedMs(pipeline_start, pipeline_end),
                                std::memory_order_relaxed);
    } else {
      ns2_archive_hits_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (archive && pipeline && (compiled || compacting) && archive_skip) {
    const uint64_t n =
        g_archive_skip_no_fragment.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & (n - 1)) == 0) {
      REXLOG_INFO(
          "[archive-safe-save] pipeline without a fragment function: not "
          "added to the binary archive (#{})",
          n);
    }
  }
  if (archive && pipeline && (compiled || compacting) && !archive_skip) {
    auto add_start = std::chrono::steady_clock::now();
    NS::Error* archive_error = nullptr;
    bool added;
    {
      std::lock_guard<std::mutex> write_lock(
          pipeline_binary_archive_write_mutex_);
      added = archive->addRenderPipelineFunctions(desc, &archive_error);
    }
    archive_add_count_.fetch_add(1, std::memory_order_relaxed);
    archive_add_ms_total_.fetch_add(
        ElapsedMs(add_start, std::chrono::steady_clock::now()),
        std::memory_order_relaxed);
    if (added) {
      std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
      archive_set.dirty = true;
      if (fragment_override) {
        ns2_last_add_ns_.store(SteadyNs(), std::memory_order_relaxed);
      }
    } else {
      REXLOG_WARN("Metal binary archive: add failed: {}",
                  archive_error
                      ? archive_error->localizedDescription()->utf8String()
                      : "unknown error");
    }
  }
  for (MTL::BinaryArchive* a : archives) {
    a->release();
  }
  desc->release();

  if (!pipeline) {
    if (error) {
      REXLOG_ERROR("Failed to create pipeline state: {}",
             error->localizedDescription()->utf8String());
    } else {
      REXLOG_ERROR("Failed to create pipeline state (unknown error)");
    }
    return nullptr;
  }

  if (out_optimized) {
    *out_optimized = fragment_optimized;
  }
  return pipeline;
}

// ---------------------------------------------------------------------------
// CreationThread (background pipeline compilation)
// ---------------------------------------------------------------------------

MTL::RenderPipelineState* MetalPipelineCache::WaitForPipeline(
    PipelineHandle* handle) {
  hitch::Scope hitch_scope(hitch::kPsoWait);
  if (!handle->creation_complete.load(std::memory_order_acquire)) {
    // A video or a player who skips the logos may need a cached pipeline
    // before warm-up finishes. Move it ahead of speculative work. Rebuild the
    // heap under its lock: mutating a queued priority alone breaks heap order.
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      std::vector<PipelineCreationRequest> pending;
      while (!creation_queue_.empty()) {
        pending.push_back(creation_queue_.top());
        creation_queue_.pop();
      }
      handle->priority = UINT8_MAX;
      for (const auto& request : pending) creation_queue_.push(request);
    }
    creation_request_cond_.notify_all();
  }
  return handle->WaitForCreation();
}

void MetalPipelineCache::CreationThread(size_t thread_index) {
  NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

  while (true) {
    PipelineCreationRequest request;
    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      if (thread_index >= kWarmupHelperIndex &&
          (creation_queue_.empty() || creation_threads_shutdown_)) {
        break;  // startup warm-up helper: done
      }
      while (creation_queue_.empty() && !creation_threads_shutdown_) {
        creation_request_cond_.wait(lock);
      }
      if (creation_threads_shutdown_ && creation_queue_.empty()) {
        break;
      }
      request = creation_queue_.top();
      creation_queue_.pop();
      ++creation_threads_busy_;
    }

    // Drain and recreate autorelease pool per pipeline.
    pool->drain();
    pool = NS::AutoreleasePool::alloc()->init();

    // Create the pipeline.
    PipelineHandle* handle = request.handle;
    bool optimized = false;
    bool vertex_native = false;
    MTL::RenderPipelineState* pipeline = CreatePipelineFromHandle(
        handle, handle->pending_vertex_translation,
        handle->pending_pixel_translation, &optimized, nullptr,
        &vertex_native);
    handle->ns_vs_native.store(vertex_native, std::memory_order_release);
    if (pipeline && handle->ns_pixel_translation) {
      if (optimized) {
        handle->ns_optimized.store(true, std::memory_order_release);
      } else {
        native_shaders_plain_pipelines_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    handle->state.store(pipeline, std::memory_order_release);
    if (pipeline && handle->storage_write_pending) {
      if (handle->pending_vertex_translation) {
        QueueStoredShader(static_cast<MetalShader&>(
            handle->pending_vertex_translation->shader()));
      }
      if (handle->pending_pixel_translation) {
        QueueStoredShader(static_cast<MetalShader&>(
            handle->pending_pixel_translation->shader()));
      }
      QueueStoredPipeline(handle->description, handle->description_hash);
      handle->storage_write_pending = false;
    }

    // Clear pending data.
    handle->pending_vertex_translation = nullptr;
    handle->pending_pixel_translation = nullptr;
    handle->creation_complete.store(true, std::memory_order_release);
    handle->creation_complete.notify_all();

    --creation_threads_busy_;
  }

  pool->drain();
}

// ---------------------------------------------------------------------------
// IsCreatingPipelines
// ---------------------------------------------------------------------------

std::string MetalPipelineCache::ShaderPrepCountsJson() {
  // [shader-prep] FABLE2_SHADER_PREP status (metal_graphics_system.cc).
  size_t stored = 0;
  {
    std::lock_guard<std::mutex> lock(stored_pipeline_mutex_);
    stored = stored_pipeline_hashes_.size();
  }
  return fmt::format(
      "\"stored_pipelines\":{},\"shaders\":{},\"pipelines_compiled\":{},"
      "\"pipeline_compile_ms\":{},\"archive_hits\":{},\"archive_misses\":{},"
      "\"ns_queued\":{},\"ns_ready\":{},\"ns_from_cache\":{},"
      "\"ns_fallbacks\":{},\"ns_optimize_ms\":{},\"native_xlate\":{},"
      "\"native_xlate_fallbacks\":{},\"native_xlate_compile_failed\":{},"
      "\"bank\":{{\"shaders\":{},\"translations\":{},\"ready\":{},\"failed\":{},\"skipped\":{},"
      "\"pending\":{},\"ms\":{}}}",
      stored, shader_cache_.size(), pipeline_compile_count_.load(),
      pipeline_compile_ms_total_.load(), archive_lookup_hits_.load(),
      archive_lookup_misses_.load(), native_shaders_queued_.load(),
      native_shaders_ready_.load(), native_shaders_cache_hits_.load(),
      native_shaders_fallbacks_.load(), native_shaders_opt_ms_total_.load(),
      native_xlate_ok_.load(), native_xlate_fallback_.load(),
      native_xlate_compile_failed_.load(), bank_prep_shaders_.load(), bank_prep_total_.load(),
      bank_prep_ok_.load(), bank_prep_failed_.load(), bank_prep_skipped_.load(), bank_prep_pending_.load(),
      bank_prep_ms_.load());
}

readiness::Background MetalPipelineCache::ReadinessBackground() {
  readiness::Background bg;
  {
    std::lock_guard<std::mutex> lock(ns_mutex_);
    bg.ns_pending = uint32_t(ns_busy_ + ns_optimize_queue_.size() + ns_swap_queue_.size() +
                             ns_emu_swap_queue_.size());
    bg.ns_spec_pending =
        uint32_t(ns_spec_queue_.size() + ns_spec_build_queue_.size() + ns_spec_emu_queue_.size());
  }
  if (!creation_threads_.empty()) {
    std::lock_guard<std::mutex> lock(creation_request_lock_);
    bg.pipelines_pending = uint32_t(creation_queue_.size() + creation_threads_busy_.load());
  }
  // Native tessellation / point / rectangle pipelines prewarmed in the
  // background (FABLE2_NATIVE_FIRST).
  bg.pipelines_pending +=
      native_geom_prewarm_pending_.load(std::memory_order_acquire);
  // [bank-prep] shader bank translations (headless prep only).
  bg.ns_pending += bank_prep_pending_.load(std::memory_order_acquire);
  bg.warmup_pending = background_warmup_pending_;
  return bg;
}

bool MetalPipelineCache::IsCreatingPipelines() {
  if (creation_threads_.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(creation_request_lock_);
  return !creation_queue_.empty() || creation_threads_busy_ != 0;
}

MetalStageCompileCacheStats MetalPipelineCache::GetAndResetStageCompileStats() {
  if (!metal_shader_converter_ ||
      !metal_shader_converter_->stage_compile_cache()) {
    return {};
  }
  return metal_shader_converter_->stage_compile_cache()->GetAndResetStats();
}

MetalPipelineRuntimeStats MetalPipelineCache::GetAndResetRuntimeStats() {
  MetalPipelineRuntimeStats stats;
  stats.dxil_convert_requests =
      dxil_convert_requests_.exchange(0, std::memory_order_relaxed);
  stats.dxil_cache_hits =
      dxil_cache_hits_.exchange(0, std::memory_order_relaxed);
  stats.dxil_cache_misses =
      dxil_cache_misses_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_failures =
      dxil_convert_failures_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_dxbc_bytes =
      dxil_convert_dxbc_bytes_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_dxil_bytes =
      dxil_convert_dxil_bytes_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_ms_total =
      dxil_convert_ms_total_.exchange(0, std::memory_order_relaxed);
  stats.dxil_convert_ms_max =
      dxil_convert_ms_max_.exchange(0, std::memory_order_relaxed);
  stats.library_requests =
      library_requests_.exchange(0, std::memory_order_relaxed);
  stats.library_failures =
      library_failures_.exchange(0, std::memory_order_relaxed);
  stats.library_bytes = library_bytes_.exchange(0, std::memory_order_relaxed);
  stats.library_ms_total =
      library_ms_total_.exchange(0, std::memory_order_relaxed);
  stats.library_ms_max = library_ms_max_.exchange(0, std::memory_order_relaxed);
  stats.render_pipeline_requests =
      render_pipeline_requests_.exchange(0, std::memory_order_relaxed);
  stats.render_pipeline_failures =
      render_pipeline_failures_.exchange(0, std::memory_order_relaxed);
  stats.render_pipeline_ms_total =
      render_pipeline_ms_total_.exchange(0, std::memory_order_relaxed);
  stats.render_pipeline_ms_max =
      render_pipeline_ms_max_.exchange(0, std::memory_order_relaxed);
  return stats;
}

// ---------------------------------------------------------------------------
// GetOrCreateGeometryPipelineState
// ---------------------------------------------------------------------------

MetalPipelineCache::GeometryPipelineState*
MetalPipelineCache::GetOrCreateGeometryPipelineState(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    GeometryShaderKey geometry_shader_key,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  if (!vertex_translation) {
    REXLOG_ERROR("No valid vertex shader translation for geometry pipeline");
    return nullptr;
  }
  bool use_fallback_pixel_shader = (pixel_translation == nullptr);
  bool pixel_optimized = false;
  MTL::Library* pixel_library =
      use_fallback_pixel_shader
          ? nullptr
          : NsEmuFragmentLibrary(pixel_translation, &pixel_optimized);
  const char* pixel_function =
      use_fallback_pixel_shader
          ? nullptr
          : (pixel_optimized
                 ? pixel_translation->optimized_function_name().c_str()
                 : pixel_translation->function_name().c_str());
  if (use_fallback_pixel_shader) {
    if (!EnsureDepthOnlyPixelShader()) {
      REXLOG_ERROR("Geometry pipeline: failed to create depth-only PS");
      return nullptr;
    }
    pixel_library = depth_only_pixel_library_;
    pixel_function = depth_only_pixel_function_name_.c_str();
  } else if (!pixel_library) {
    REXLOG_ERROR("No valid pixel shader translation for geometry pipeline");
    return nullptr;
  }

  uint32_t sample_count = attachment_formats.sample_count;
  MTL::PixelFormat color_formats[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_formats[i] = attachment_formats.color_formats[i];
  }
  MTL::PixelFormat depth_format = attachment_formats.depth_format;
  MTL::PixelFormat stencil_format = attachment_formats.stencil_format;

  struct GeometryPipelineKey {
    const void* vs;
    const void* ps;
    uint32_t geometry_key;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};

  key_data.vs = vertex_translation;
  key_data.ps = use_fallback_pixel_shader
                    ? static_cast<const void*>(pixel_library)
                    : static_cast<const void*>(pixel_translation);
  key_data.geometry_key = geometry_shader_key.key;
  key_data.sample_count = sample_count;
  key_data.depth_format = uint32_t(depth_format);
  key_data.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(color_formats[i]);
  }
  key_data.normalized_color_mask = rendering_key.normalized_color_mask;
  key_data.alpha_to_mask_enable = rendering_key.alpha_to_mask_enable;
  std::memcpy(key_data.blendcontrol, rendering_key.blendcontrol,
              sizeof(key_data.blendcontrol));
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));

  auto it = geometry_pipeline_cache_.find(key);
  if (it != geometry_pipeline_cache_.end()) {
    return &it->second;
  }
  hitch::Scope hitch_scope(hitch::kPsoEmu);

  const bool failure_backoff =
      REXCVAR_GET(metal_geometry_pipeline_failure_backoff);
  if (failure_backoff && !geometry_pipeline_failures_.CanAttempt(
                             key, std::chrono::steady_clock::now())) {
    return nullptr;
  }

  if (!generated_stages_) {
    REXLOG_ERROR("Geometry pipeline: generated stage cache is not initialized");
    return nullptr;
  }

  auto* vertex_stage = generated_stages_->GetGeometryVertexStage(
      vertex_translation, geometry_shader_key);
  if (!vertex_stage || !vertex_stage->library ||
      !vertex_stage->stage_in_library) {
    return nullptr;
  }
  auto* geometry_stage =
      generated_stages_->GetGeometryShaderStage(geometry_shader_key);
  if (!geometry_stage || !geometry_stage->library) {
    return nullptr;
  }

  if (!vertex_stage->vertex_output_size_in_bytes ||
      !geometry_stage->max_input_primitives_per_mesh_threadgroup) {
    REXLOG_ERROR(
        "Geometry pipeline: invalid reflection (vs_output={}, gs_max_input={})",
        vertex_stage->vertex_output_size_in_bytes,
        geometry_stage->max_input_primitives_per_mesh_threadgroup);
    return nullptr;
  }

  if (NativeTessDumpDir()) {
    const std::string json = fmt::format(
        "{{\"vs\": \"{:016X}\", \"mod\": \"{:X}\", \"key\": \"{:08X}\", "
        "\"type\": {}, \"vs_fn\": \"{}\", \"gs_fn\": \"{}\", "
        "\"vertex_size\": {}, \"max_input_prims\": {}}}\n",
        vertex_translation->shader().ucode_data_hash(),
        vertex_translation->modification(), geometry_shader_key.key,
        uint32_t(geometry_shader_key.type), vertex_stage->function_name,
        geometry_stage->function_name, vertex_stage->vertex_output_size_in_bytes,
        geometry_stage->max_input_primitives_per_mesh_threadgroup);
    NativeTessDumpFile(
        fmt::format("geom_{:016X}_{:X}_k{:08X}.json",
                    vertex_translation->shader().ucode_data_hash(),
                    vertex_translation->modification(), geometry_shader_key.key),
        std::vector<uint8_t>(json.begin(), json.end()));
  }

  // Builds the pipeline for a given fragment library. Kept for the
  // FABLE2_NS_EMU_PIPELINES background rebuild with the optimized pixel
  // shader: the stage states live as long as the cache, and the optimizer
  // threads stop before it is destroyed.
  uint32_t blendcontrol[4];
  std::memcpy(blendcontrol, key_data.blendcontrol, sizeof(blendcontrol));
  const uint32_t normalized_color_mask = key_data.normalized_color_mask;
  // The converter's entry point; the native translation's library has its
  // own (FABLE2_NATIVE_XLATE), chosen per library at build time.
  const std::string fragment_function =
      (pixel_function && pixel_translation && !use_fallback_pixel_shader)
          ? pixel_translation->function_name()
          : std::string(pixel_function ? pixel_function : "");
  MetalShader::MetalTranslation* fragment_translation =
      use_fallback_pixel_shader ? nullptr : pixel_translation;
  auto build = [device = device_, vertex_stage, geometry_stage, color_formats,
                depth_format, stencil_format, sample_count,
                normalized_color_mask, blendcontrol, fragment_function,
                fragment_translation](
                   MTL::Library* fragment_library,
                   NS::Error** error) -> MTL::RenderPipelineState* {
    MTL::MeshRenderPipelineDescriptor* desc =
        MTL::MeshRenderPipelineDescriptor::alloc()->init();
    for (uint32_t i = 0; i < 4; ++i) {
      desc->colorAttachments()->object(i)->setPixelFormat(color_formats[i]);
    }
    desc->setDepthAttachmentPixelFormat(depth_format);
    desc->setStencilAttachmentPixelFormat(stencil_format);
    desc->setRasterSampleCount(sample_count);
    desc->setAlphaToCoverageEnabled(false);
    ApplyBlendStateToDescriptor(desc->colorAttachments(),
                                normalized_color_mask, blendcontrol);
    IRGeometryEmulationPipelineDescriptor ir_desc = {};
    ir_desc.stageInLibrary = vertex_stage->stage_in_library;
    ir_desc.vertexLibrary = vertex_stage->library;
    ir_desc.vertexFunctionName = vertex_stage->function_name.c_str();
    ir_desc.geometryLibrary = geometry_stage->library;
    ir_desc.geometryFunctionName = geometry_stage->function_name.c_str();
    ir_desc.fragmentLibrary = fragment_library;
    ir_desc.fragmentFunctionName =
        NativeXlateEntryIn(fragment_library, fragment_translation)
            ? "xe_native_ps"
            : fragment_function.c_str();
    ir_desc.basePipelineDescriptor = desc;
    ir_desc.pipelineConfig.gsVertexSizeInBytes =
        vertex_stage->vertex_output_size_in_bytes;
    ir_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup =
        geometry_stage->max_input_primitives_per_mesh_threadgroup;
    MTL::RenderPipelineState* result =
        IRRuntimeNewGeometryEmulationPipeline(device, &ir_desc, error);
    desc->release();
    return result;
  };

  NS::Error* error = nullptr;
  auto pipeline_start = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline = build(pixel_library, &error);
  auto pipeline_end = std::chrono::steady_clock::now();
  RecordRenderPipelineCreation(pipeline != nullptr,
                               ElapsedMs(pipeline_start, pipeline_end));

  if (!pipeline) {
    const auto retry_delay =
        failure_backoff
            ? geometry_pipeline_failures_.RecordFailure(key, pipeline_end)
            : std::chrono::seconds(0);
    REXLOG_ERROR(
        "Failed to create geometry pipeline state: {}",
        error ? error->localizedDescription()->utf8String() : "unknown error");
    REXLOG_ERROR(
        "Geometry pipeline identity: key={:016X} vs={:016X}/{:016X} "
        "ps={:016X}/{:016X} geometry={:08X} retry_delay_seconds={}",
        key, vertex_translation->shader().ucode_data_hash(),
        vertex_translation->modification(),
        pixel_translation ? pixel_translation->shader().ucode_data_hash() : 0,
        pixel_translation ? pixel_translation->modification() : 0,
        geometry_shader_key.key, retry_delay.count());
    REXLOG_ERROR(
        "Geometry pipeline details: vs_fn='{}' gs_fn='{}' ps_fn='{}' "
        "depth_format={} stencil_format={} samples={}",
        vertex_stage->function_name, geometry_stage->function_name,
        pixel_function ? pixel_function : "<null>", uint32_t(depth_format),
        uint32_t(stencil_format), sample_count);
    LogMetalErrorDetails("Geometry pipeline error", error);
    return nullptr;
  }

  geometry_pipeline_failures_.RecordSuccess(key);

  GeometryPipelineState state;
  state.pipeline = pipeline;
  state.gs_vertex_size_in_bytes = vertex_stage->vertex_output_size_in_bytes;
  state.gs_max_input_primitives_per_mesh_threadgroup =
      geometry_stage->max_input_primitives_per_mesh_threadgroup;
  // FABLE2_NS_SPECIALIZE also keeps the rebuild closure (specialized
  // variant), even when the optimized library was used from the start.
  const bool ns_emu_swappable = !use_fallback_pixel_shader &&
                                NativeShadersEmuPipelines() && !pixel_optimized;
  const bool ns_emu_spec =
      !use_fallback_pixel_shader && pixel_translation && NativeShadersMode() &&
      ns_spec::Enabled() &&
      ns_spec::Candidate(pixel_translation->shader().ucode_data_hash());
  if (ns_emu_swappable || ns_emu_spec) {
    auto swap = std::make_shared<NsEmuSwap>();
    swap->pixel_translation = pixel_translation;
    swap->swappable = ns_emu_swappable;
    swap->tessellation = false;
    swap->build = [build](MTL::Library* library) {
      NS::Error* build_error = nullptr;
      return build(library, &build_error);
    };
    state.ns_emu = swap;
  }

  auto [inserted_it, inserted] =
      geometry_pipeline_cache_.emplace(key, std::move(state));
  if (inserted_it->second.ns_emu && inserted_it->second.ns_emu->swappable) {
    RegisterNsEmuSwap(inserted_it->second.ns_emu);
  }
  MetalPipelineDescription stored_description =
      BuildGeometryPipelineDescription(vertex_translation, pixel_translation,
                                       geometry_shader_key, attachment_formats,
                                       rendering_key);
  QueueStoredShader(static_cast<MetalShader&>(vertex_translation->shader()));
  if (pixel_translation) {
    QueueStoredShader(static_cast<MetalShader&>(pixel_translation->shader()));
  }
  QueueStoredPipeline(
      stored_description,
      XXH3_64bits(&stored_description, sizeof(stored_description)));
  return &inserted_it->second;
}

// ---------------------------------------------------------------------------
// GetOrCreateNativePointsPipeline (FABLE2_NATIVE_POINTS)
// ---------------------------------------------------------------------------

MTL::RenderPipelineState* MetalPipelineCache::GetOrCreateNativePointsPipeline(
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation,
    GeometryShaderKey geometry_shader_key,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  const bool points =
      geometry_shader_key.type == PipelineGeometryShader::kPointList;
  const bool rects =
      geometry_shader_key.type == PipelineGeometryShader::kRectangleList;
  if (!vertex_translation || !(points ? NativePointsMode() : rects && NativeRectsMode()) ||
      geometry_shader_key.user_clip_plane_cull ||
      geometry_shader_key.has_vertex_kill_and) {
    return nullptr;
  }
  const bool point_coordinates = geometry_shader_key.has_point_coordinates;
  const bool mesh = points && NativePointsMode() == 2;
  const uint64_t fn_key_data[] = {
      uint64_t(reinterpret_cast<uintptr_t>(vertex_translation)),
      (point_coordinates ? 1ull : 0ull) | (rects ? 2ull : 0ull) |
          (mesh ? 4ull : 0ull)};
  const uint64_t fn_key = XXH3_64bits(fn_key_data, sizeof(fn_key_data));
  // The maps are shared with the background native-geometry prewarm
  // (PrewarmNativeGeometry): lookups and inserts under native_geom_mutex_,
  // compiles outside it; a lost race releases its own result.
  MTL::Function* vs_function = nullptr;
  bool have_fn = false;
  {
    std::lock_guard<std::mutex> lock(native_geom_mutex_);
    auto fn_it = native_points_functions_.find(fn_key);
    if (fn_it != native_points_functions_.end()) {
      vs_function = fn_it->second;
      have_fn = true;
    }
  }
  if (!have_fn) {
    MTL::Library* library = nullptr;
    std::string entry;
    if (!EnsureDxbcTranslationReady(vertex_translation, "native points") ||
        !CompileNativeXlate(vertex_translation, {}, &library, &vs_function,
                            &entry, points && !mesh, point_coordinates, rects,
                            mesh)) {
      vs_function = nullptr;
      REXLOG_WARN("[native-points] {} expansion of VS {:016X}/{:X}: none; "
                  "emulation", points ? "point" : "rectangle",
                  vertex_translation->shader().ucode_data_hash(),
                  vertex_translation->modification());
    } else {
      REXLOG_INFO("[native-points] {} expansion of VS {:016X}/{:X}: native",
                  points ? "point" : "rectangle",
                  vertex_translation->shader().ucode_data_hash(),
                  vertex_translation->modification());
    }
    std::lock_guard<std::mutex> lock(native_geom_mutex_);
    auto [fn_it, inserted] = native_points_functions_.emplace(fn_key, vs_function);
    if (!inserted) {
      if (vs_function) vs_function->release();
      if (library) library->release();
      vs_function = fn_it->second;
    }
  }
  if (!vs_function) {
    return nullptr;
  }
  MTL::Function* fragment_function = nullptr;
  if (pixel_translation) {
    if (!EnsureMetalTranslationReady(pixel_translation)) {
      return nullptr;
    }
    fragment_function = pixel_translation->best_function();
  } else if (NativeFirstMode()) {
    // No pixel shader: the emulation would bind its (empty) depth-only
    // fragment; the native expansion binds the same empty fragment
    // (EnsureDepthOnlyPixelShader, native under FABLE2_NATIVE_FIRST).
    fragment_function = DepthOnlyPixelFunction();
  }
  // Without FABLE2_NATIVE_FIRST, no-PS draws stay on emulation.
  if (!fragment_function) {
    return nullptr;
  }
  struct Key {
    const void* vs;
    const void* fragment;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};
  key_data.vs = vs_function;
  key_data.fragment = fragment_function;
  key_data.sample_count = attachment_formats.sample_count;
  key_data.depth_format = uint32_t(attachment_formats.depth_format);
  key_data.stencil_format = uint32_t(attachment_formats.stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(attachment_formats.color_formats[i]);
  }
  key_data.normalized_color_mask = rendering_key.normalized_color_mask;
  key_data.alpha_to_mask_enable = rendering_key.alpha_to_mask_enable;
  std::memcpy(key_data.blendcontrol, rendering_key.blendcontrol,
              sizeof(key_data.blendcontrol));
  const uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));
  {
    std::lock_guard<std::mutex> lock(native_geom_mutex_);
    auto it = native_points_pipelines_.find(key);
    if (it != native_points_pipelines_.end()) {
      return it->second;
    }
  }
  hitch::Scope hitch_scope(hitch::kPsoEmu);
  ScopedAutoreleasePool autorelease_pool;
  if (mesh) {
    MTL::MeshRenderPipelineDescriptor* mdesc =
        MTL::MeshRenderPipelineDescriptor::alloc()->init();
    mdesc->setMeshFunction(vs_function);
    mdesc->setFragmentFunction(fragment_function);
    for (uint32_t i = 0; i < 4; ++i) {
      mdesc->colorAttachments()->object(i)->setPixelFormat(
          attachment_formats.color_formats[i]);
    }
    mdesc->setDepthAttachmentPixelFormat(attachment_formats.depth_format);
    mdesc->setStencilAttachmentPixelFormat(attachment_formats.stencil_format);
    mdesc->setRasterSampleCount(attachment_formats.sample_count);
    mdesc->setAlphaToCoverageEnabled(false);
    mdesc->setMaxTotalThreadsPerMeshThreadgroup(64);
    ApplyBlendStateToDescriptor(mdesc->colorAttachments(),
                                rendering_key.normalized_color_mask,
                                rendering_key.blendcontrol);
    NS::Error* error = nullptr;
    auto t0 = std::chrono::steady_clock::now();
    MTL::RenderPipelineState* pipeline = device_->newRenderPipelineState(
        mdesc, MTL::PipelineOptionNone, nullptr, &error);
    auto t1 = std::chrono::steady_clock::now();
    mdesc->release();
    RecordRenderPipelineCreation(pipeline != nullptr, ElapsedMs(t0, t1));
    if (!pipeline) {
      REXLOG_ERROR("[native-points] mesh pipeline creation failed (VS "
                   "{:016X}): {}",
                   vertex_translation->shader().ucode_data_hash(),
                   error ? error->localizedDescription()->utf8String()
                         : "unknown error");
    } else {
      REXLOG_INFO("[native-points] mesh pipeline VS {:016X} PS {:016X}: {} ms",
                  vertex_translation->shader().ucode_data_hash(),
                  pixel_translation
                      ? pixel_translation->shader().ucode_data_hash()
                      : 0,
                  ElapsedMs(t0, t1));
      StoreNativeGeomDescription(
          BuildGeometryPipelineDescription(vertex_translation,
                                           pixel_translation,
                                           geometry_shader_key,
                                           attachment_formats, rendering_key),
          vertex_translation, pixel_translation);
    }
    return PublishNativeGeomPipeline(native_points_pipelines_, key, pipeline);
  }
  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs_function);
  desc->setFragmentFunction(fragment_function);
  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(
        attachment_formats.color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(attachment_formats.depth_format);
  desc->setStencilAttachmentPixelFormat(attachment_formats.stencil_format);
  desc->setSampleCount(attachment_formats.sample_count);
  desc->setAlphaToCoverageEnabled(false);
  ApplyBlendStateToDescriptor(desc->colorAttachments(),
                              rendering_key.normalized_color_mask,
                              rendering_key.blendcontrol);
  desc->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);
  NS::Error* error = nullptr;
  auto t0 = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline = NewNativeGeomPipelineState(desc, &error);
  auto t1 = std::chrono::steady_clock::now();
  desc->release();
  RecordRenderPipelineCreation(pipeline != nullptr, ElapsedMs(t0, t1));
  if (pipeline) {
    StoreNativeGeomDescription(
        BuildGeometryPipelineDescription(vertex_translation, pixel_translation,
                                         geometry_shader_key,
                                         attachment_formats, rendering_key),
        vertex_translation, pixel_translation);
  }
  if (!pipeline) {
    REXLOG_ERROR("[native-points] pipeline creation failed (VS {:016X}): {}",
                 vertex_translation->shader().ucode_data_hash(),
                 error ? error->localizedDescription()->utf8String()
                       : "unknown error");
  } else {
    REXLOG_INFO("[native-points] pipeline VS {:016X} PS {:016X}: {} ms",
                vertex_translation->shader().ucode_data_hash(),
                pixel_translation ? pixel_translation->shader().ucode_data_hash()
                                  : 0,
                ElapsedMs(t0, t1));
  }
  return PublishNativeGeomPipeline(native_points_pipelines_, key, pipeline);
}

// ---------------------------------------------------------------------------
// GetOrCreateNativeTessPipeline (FABLE2_NATIVE_TESS, default 1 = exact only)
// ---------------------------------------------------------------------------

MTL::RenderPipelineState* MetalPipelineCache::GetOrCreateNativeTessPipeline(
    MetalShader::MetalTranslation* domain_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key, bool cpu_pattern) {
  if (!NativeTessMode() || !domain_translation ||
      (primitive_processing_result.tessellation_mode !=
           xenos::TessellationMode::kAdaptive &&
       primitive_processing_result.tessellation_mode !=
           xenos::TessellationMode::kContinuous) ||
      primitive_processing_result.host_vertex_shader_type !=
          Shader::HostVertexShaderType::kQuadDomainPatchIndexed) {
    return nullptr;
  }
  // The native domain function, once per domain translation.
  MTL::Function* ds_function = nullptr;
  // Keyed per translation and entry kind (CPU pattern or tessellator).
  const void* ds_key =
      reinterpret_cast<const char*>(domain_translation) + (cpu_pattern ? 1 : 0);
  bool have_ds = false;
  {
    std::lock_guard<std::mutex> lock(native_geom_mutex_);
    auto ds_it = native_ds_functions_.find(ds_key);
    if (ds_it != native_ds_functions_.end()) {
      ds_function = ds_it->second;
      have_ds = true;
    }
  }
  if (!have_ds) {
    MTL::Library* library = nullptr;
    std::string entry;
    if (EnsureDxbcTranslationReady(domain_translation, "native domain") &&
        CompileNativeXlate(domain_translation, {}, &library, &ds_function,
                           &entry, false, false, false, false, cpu_pattern)) {
      REXLOG_INFO("[native-tess] DS {:016X}/{:X}: native domain shader",
                  domain_translation->shader().ucode_data_hash(),
                  domain_translation->modification());
    } else {
      ds_function = nullptr;
      REXLOG_WARN("[native-tess] DS {:016X}/{:X}: no native domain shader; "
                  "emulation",
                  domain_translation->shader().ucode_data_hash(),
                  domain_translation->modification());
    }
    // The library stays alive with its function (never released).
    std::lock_guard<std::mutex> lock(native_geom_mutex_);
    auto [ds_it, inserted] = native_ds_functions_.emplace(ds_key, ds_function);
    if (!inserted) {
      if (ds_function) ds_function->release();
      if (library) library->release();
      ds_function = ds_it->second;
    }
  }
  if (!ds_function) {
    return nullptr;
  }
  MTL::Function* fragment_function = nullptr;
  if (pixel_translation) {
    if (!EnsureMetalTranslationReady(pixel_translation)) {
      return nullptr;
    }
    fragment_function = pixel_translation->best_function();
    if (!fragment_function) {
      return nullptr;
    }
  } else {
    fragment_function = DepthOnlyPixelFunction();
    if (!fragment_function) {
      return nullptr;
    }
  }
  // The hull shaders declare output_triangle_cw, but Metal's tessellator needs
  // counter-clockwise output for the same facing as the converter's emulation
  // (tools/shaderlab/tess_diff.mm with culling: cw culls the opposite side).
  // Experiment knob: FABLE2_NATIVE_TESS_WINDING=cw.
  static const bool winding_ccw = [] {
    const char* v = std::getenv("FABLE2_NATIVE_TESS_WINDING");
    return !(v && std::strcmp(v, "cw") == 0);
  }();
  struct Key {
    const void* ds;
    const void* fragment;  // the current best function: a later native or
                           // optimized pixel shader gets its own pipeline
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};
  key_data.ds = ds_function;
  key_data.fragment = fragment_function;
  key_data.sample_count = attachment_formats.sample_count;
  key_data.depth_format = uint32_t(attachment_formats.depth_format);
  key_data.stencil_format = uint32_t(attachment_formats.stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(attachment_formats.color_formats[i]);
  }
  key_data.normalized_color_mask = rendering_key.normalized_color_mask;
  key_data.alpha_to_mask_enable = rendering_key.alpha_to_mask_enable;
  std::memcpy(key_data.blendcontrol, rendering_key.blendcontrol,
              sizeof(key_data.blendcontrol));
  const uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));
  {
    std::lock_guard<std::mutex> lock(native_geom_mutex_);
    auto it = native_tess_pipelines_.find(key);
    if (it != native_tess_pipelines_.end()) {
      return it->second;
    }
  }
  hitch::Scope hitch_scope(hitch::kPsoEmu);
  ScopedAutoreleasePool autorelease_pool;
  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(ds_function);
  desc->setFragmentFunction(fragment_function);
  for (uint32_t i = 0; i < 4; ++i) {
    desc->colorAttachments()->object(i)->setPixelFormat(
        attachment_formats.color_formats[i]);
  }
  desc->setDepthAttachmentPixelFormat(attachment_formats.depth_format);
  desc->setStencilAttachmentPixelFormat(attachment_formats.stencil_format);
  desc->setSampleCount(attachment_formats.sample_count);
  desc->setAlphaToCoverageEnabled(false);
  ApplyBlendStateToDescriptor(desc->colorAttachments(),
                              rendering_key.normalized_color_mask,
                              rendering_key.blendcontrol);
  if (cpu_pattern) {
    desc->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);
  } else {
  // The adaptive hull shaders: domain_quad, partitioning_fractional_even;
  // factors are written per patch as halves.
  desc->setTessellationPartitionMode(MTL::TessellationPartitionModeFractionalEven);
  desc->setTessellationFactorStepFunction(
      MTL::TessellationFactorStepFunctionPerPatch);
  desc->setTessellationOutputWindingOrder(winding_ccw
                                              ? MTL::WindingCounterClockwise
                                              : MTL::WindingClockwise);
  desc->setTessellationFactorFormat(MTL::TessellationFactorFormatHalf);
  desc->setTessellationControlPointIndexType(
      MTL::TessellationControlPointIndexTypeNone);
  desc->setTessellationFactorScaleEnabled(false);
  desc->setMaxTessellationFactor(64);
  }
  NS::Error* error = nullptr;
  auto t0 = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline = NewNativeGeomPipelineState(desc, &error);
  auto t1 = std::chrono::steady_clock::now();
  desc->release();
  RecordRenderPipelineCreation(pipeline != nullptr, ElapsedMs(t0, t1));
  if (pipeline) {
    // auxiliary3 = 1: the reference-pattern (CPU) variant, so the prewarm
    // builds the same entry kind (the emulation's records leave it 0).
    MetalPipelineDescription stored = BuildTessellationPipelineDescription(
        domain_translation, pixel_translation, primitive_processing_result,
        primitive_processing_result.tessellation_mode, attachment_formats,
        rendering_key);
    stored.auxiliary3 = cpu_pattern ? 1u : 0u;
    StoreNativeGeomDescription(stored, domain_translation, pixel_translation);
  }
  if (!pipeline) {
    REXLOG_ERROR("[native-tess] pipeline creation failed (DS {:016X}): {}",
                 domain_translation->shader().ucode_data_hash(),
                 error ? error->localizedDescription()->utf8String()
                       : "unknown error");
  } else {
    REXLOG_INFO("[native-tess] pipeline DS {:016X} PS {:016X}: {} ms",
                domain_translation->shader().ucode_data_hash(),
                pixel_translation
                    ? pixel_translation->shader().ucode_data_hash()
                    : 0,
                ElapsedMs(t0, t1));
  }
  return PublishNativeGeomPipeline(native_tess_pipelines_, key, pipeline);
}

void MetalPipelineCache::StoreNativeGeomDescription(
    const MetalPipelineDescription& description,
    MetalShader::MetalTranslation* vertex_translation,
    MetalShader::MetalTranslation* pixel_translation) {
  if (!NativeGeomPersistEnabled() || !vertex_translation) {
    return;
  }
  QueueStoredShader(static_cast<MetalShader&>(vertex_translation->shader()));
  if (pixel_translation) {
    QueueStoredShader(static_cast<MetalShader&>(pixel_translation->shader()));
  }
  QueueStoredPipeline(description,
                      XXH3_64bits(&description, sizeof(description)));
}

MTL::RenderPipelineState* MetalPipelineCache::NewNativeGeomPipelineState(
    MTL::RenderPipelineDescriptor* desc, NS::Error** error) {
  if (!NativeGeomPersistEnabled()) {
    return device_->newRenderPipelineState(desc, error);
  }
  // As CreatePipelineFromHandle: look the pipeline up in earlier launches'
  // archive segments without compiling; on a miss compile and record it in
  // this launch's archive (set 0, plain pipelines).
  MTL::BinaryArchive* archive = nullptr;
  std::vector<MTL::BinaryArchive*> archives;
  ArchiveSet& archive_set = archive_sets_[0];
  bool compacting = false;
  {
    std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
    archives = archive_set.segments;
    if (archive_set.archive) {
      archive = archive_set.archive;
      archives.push_back(archive);
    }
    for (MTL::BinaryArchive* a : archives) {
      a->retain();
    }
    compacting = archive_set.compacting;
  }
  const bool have_segments = archives.size() > (archive ? 1u : 0u);
  MTL::RenderPipelineState* pipeline = nullptr;
  if (!archives.empty()) {
    desc->setBinaryArchives(NS::Array::array(
        reinterpret_cast<const NS::Object* const*>(archives.data()),
        archives.size()));
    if (have_segments) {
      NS::Error* lookup_error = nullptr;
      pipeline = device_->newRenderPipelineState(
          desc, MTL::PipelineOptionFailOnBinaryArchiveMiss, nullptr,
          &lookup_error);
      (pipeline ? native_geom_archive_hits_ : native_geom_archive_misses_)
          .fetch_add(1, std::memory_order_relaxed);
    }
  }
  bool compiled = false;
  if (!pipeline) {
    pipeline = device_->newRenderPipelineState(desc, error);
    compiled = true;
  }
  if (archive && pipeline && (compiled || compacting) &&
      desc->fragmentFunction() != nullptr) {
    NS::Error* archive_error = nullptr;
    bool added;
    {
      std::lock_guard<std::mutex> write_lock(
          pipeline_binary_archive_write_mutex_);
      added = archive->addRenderPipelineFunctions(desc, &archive_error);
    }
    if (added) {
      std::lock_guard<std::mutex> lock(pipeline_binary_archive_mutex_);
      archive_set.dirty = true;
    } else {
      REXLOG_WARN("[native-geom-persist] binary archive add failed: {}",
                  archive_error
                      ? archive_error->localizedDescription()->utf8String()
                      : "unknown error");
    }
  }
  desc->setBinaryArchives(nullptr);
  for (MTL::BinaryArchive* a : archives) {
    a->release();
  }
  return pipeline;
}

MTL::RenderPipelineState* MetalPipelineCache::PublishNativeGeomPipeline(
    std::unordered_map<uint64_t, MTL::RenderPipelineState*>& map, uint64_t key,
    MTL::RenderPipelineState* pipeline) {
  std::lock_guard<std::mutex> lock(native_geom_mutex_);
  auto [it, inserted] = map.emplace(key, pipeline);
  if (!inserted && it->second != pipeline) {
    // Built by the other side (draw path or prewarm) meanwhile.
    if (pipeline) pipeline->release();
    return it->second;
  }
  return pipeline;
}

// ---------------------------------------------------------------------------
// GetOrCreateTessellationPipelineState
// ---------------------------------------------------------------------------

MetalPipelineCache::TessellationPipelineState*
MetalPipelineCache::GetOrCreateTessellationPipelineState(
    MetalShader::MetalTranslation* domain_translation,
    MetalShader::MetalTranslation* pixel_translation,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    const PipelineAttachmentFormats& attachment_formats,
    const PipelineRenderingKey& rendering_key) {
  if (!domain_translation) {
    REXLOG_ERROR("No valid domain shader translation for tessellation pipeline");
    return nullptr;
  }
  bool use_fallback_pixel_shader = (pixel_translation == nullptr);
  bool pixel_optimized = false;
  MTL::Library* pixel_library =
      use_fallback_pixel_shader
          ? nullptr
          : NsEmuFragmentLibrary(pixel_translation, &pixel_optimized);
  const char* pixel_function =
      use_fallback_pixel_shader
          ? nullptr
          : (pixel_optimized
                 ? pixel_translation->optimized_function_name().c_str()
                 : pixel_translation->function_name().c_str());
  if (use_fallback_pixel_shader) {
    if (!EnsureDepthOnlyPixelShader()) {
      REXLOG_ERROR("Tessellation pipeline: failed to create depth-only PS");
      return nullptr;
    }
    pixel_library = depth_only_pixel_library_;
    pixel_function = depth_only_pixel_function_name_.c_str();
  } else if (!pixel_library) {
    REXLOG_ERROR("No valid pixel shader translation for tessellation pipeline");
    return nullptr;
  }

  uint32_t sample_count = attachment_formats.sample_count;
  MTL::PixelFormat color_formats[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_formats[i] = attachment_formats.color_formats[i];
  }
  MTL::PixelFormat depth_format = attachment_formats.depth_format;
  MTL::PixelFormat stencil_format = attachment_formats.stencil_format;

  struct TessellationPipelineKey {
    const void* ds;
    const void* ps;
    uint32_t host_vs_type;
    uint32_t tessellation_mode;
    uint32_t host_prim;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];
  } key_data = {};

  key_data.ds = domain_translation;
  key_data.ps = use_fallback_pixel_shader
                    ? static_cast<const void*>(pixel_library)
                    : static_cast<const void*>(pixel_translation);
  key_data.host_vs_type =
      uint32_t(primitive_processing_result.host_vertex_shader_type);
  key_data.tessellation_mode =
      uint32_t(primitive_processing_result.tessellation_mode);
  key_data.host_prim =
      uint32_t(primitive_processing_result.host_primitive_type);
  key_data.sample_count = sample_count;
  key_data.depth_format = uint32_t(depth_format);
  key_data.stencil_format = uint32_t(stencil_format);
  for (uint32_t i = 0; i < 4; ++i) {
    key_data.color_formats[i] = uint32_t(color_formats[i]);
  }
  key_data.normalized_color_mask = rendering_key.normalized_color_mask;
  key_data.alpha_to_mask_enable = rendering_key.alpha_to_mask_enable;
  std::memcpy(key_data.blendcontrol, rendering_key.blendcontrol,
              sizeof(key_data.blendcontrol));
  uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));

  auto it = tessellation_pipeline_cache_.find(key);
  if (it != tessellation_pipeline_cache_.end()) {
    return &it->second;
  }
  hitch::Scope hitch_scope(hitch::kPsoEmu);

  xenos::TessellationMode tessellation_mode =
      primitive_processing_result.tessellation_mode;

  if (!generated_stages_) {
    REXLOG_ERROR("Tessellation pipeline: generated stage cache is not initialized");
    return nullptr;
  }

  auto* vertex_stage = generated_stages_->GetTessellationVertexStage(
      domain_translation, tessellation_mode);
  if (!vertex_stage || !vertex_stage->library ||
      !vertex_stage->stage_in_library) {
    return nullptr;
  }

  auto* hull_stage = generated_stages_->GetTessellationHullStage(
      primitive_processing_result, tessellation_mode);
  if (!hull_stage || !hull_stage->library) {
    return nullptr;
  }

  auto* domain_stage =
      generated_stages_->GetTessellationDomainStage(domain_translation);
  if (!domain_stage || !domain_stage->library) {
    return nullptr;
  }

  IRRuntimeTessellatorOutputPrimitive output_primitive =
      IRRuntimeTessellatorOutputUndefined;
  switch (hull_stage->reflection.hs_tessellator_output_primitive) {
    case IRRuntimeTessellatorOutputPoint:
      output_primitive = IRRuntimeTessellatorOutputPoint;
      break;
    case IRRuntimeTessellatorOutputLine:
      output_primitive = IRRuntimeTessellatorOutputLine;
      break;
    case IRRuntimeTessellatorOutputTriangleCW:
      output_primitive = IRRuntimeTessellatorOutputTriangleCW;
      break;
    case IRRuntimeTessellatorOutputTriangleCCW:
      output_primitive = IRRuntimeTessellatorOutputTriangleCCW;
      break;
    default:
      REXLOG_ERROR("Tessellation pipeline: unsupported tessellator output {}",
             hull_stage->reflection.hs_tessellator_output_primitive);
      return nullptr;
  }

  IRRuntimePrimitiveType geometry_primitive = IRRuntimePrimitiveTypeTriangle;
  const char* geometry_function = kIRTrianglePassthroughGeometryShader;
  switch (output_primitive) {
    case IRRuntimeTessellatorOutputPoint:
      geometry_primitive = IRRuntimePrimitiveTypePoint;
      geometry_function = kIRPointPassthroughGeometryShader;
      break;
    case IRRuntimeTessellatorOutputLine:
      geometry_primitive = IRRuntimePrimitiveTypeLine;
      geometry_function = kIRLinePassthroughGeometryShader;
      break;
    case IRRuntimeTessellatorOutputTriangleCW:
    case IRRuntimeTessellatorOutputTriangleCCW:
      geometry_primitive = IRRuntimePrimitiveTypeTriangle;
      geometry_function = kIRTrianglePassthroughGeometryShader;
      break;
    default:
      break;
  }

  if (!IRRuntimeValidateTessellationPipeline(
          output_primitive, geometry_primitive,
          hull_stage->reflection.hs_output_control_point_size,
          domain_stage->reflection.ds_input_control_point_size,
          hull_stage->reflection.hs_patch_constants_size,
          domain_stage->reflection.ds_patch_constants_size,
          hull_stage->reflection.hs_output_control_point_count,
          domain_stage->reflection.ds_input_control_point_count)) {
    REXLOG_ERROR("Tessellation pipeline: validation failed for HS/DS pairing");
    return nullptr;
  }

  IRRuntimeTessellationPipelineConfig pipeline_config = {};
  pipeline_config.outputPrimitiveType = output_primitive;
  pipeline_config.vsOutputSizeInBytes =
      vertex_stage->vertex_output_size_in_bytes;
  pipeline_config.gsMaxInputPrimitivesPerMeshThreadgroup =
      domain_stage->reflection.ds_max_input_prims_per_mesh_threadgroup;
  pipeline_config.hsMaxPatchesPerObjectThreadgroup =
      hull_stage->reflection.hs_max_patches_per_object_threadgroup;
  pipeline_config.hsInputControlPointCount =
      hull_stage->reflection.hs_input_control_point_count;
  pipeline_config.hsMaxObjectThreadsPerThreadgroup =
      hull_stage->reflection.hs_max_object_threads_per_patch;
  pipeline_config.hsMaxTessellationFactor =
      hull_stage->reflection.hs_max_tessellation_factor;
  pipeline_config.gsInstanceCount = 1;

  if (!pipeline_config.vsOutputSizeInBytes ||
      !pipeline_config.gsMaxInputPrimitivesPerMeshThreadgroup ||
      !pipeline_config.hsMaxPatchesPerObjectThreadgroup ||
      !pipeline_config.hsInputControlPointCount ||
      !pipeline_config.hsMaxObjectThreadsPerThreadgroup) {
    REXLOG_ERROR(
        "Tessellation pipeline: invalid reflection values (vs_output={}, "
        "gs_max_input={}, hs_patches={}, hs_cp_count={}, hs_threads={})",
        pipeline_config.vsOutputSizeInBytes,
        pipeline_config.gsMaxInputPrimitivesPerMeshThreadgroup,
        pipeline_config.hsMaxPatchesPerObjectThreadgroup,
        pipeline_config.hsInputControlPointCount,
        pipeline_config.hsMaxObjectThreadsPerThreadgroup);
    return nullptr;
  }

  if (NativeTessDumpDir()) {
    const std::string json = fmt::format(
        "{{\"ds\": \"{:016X}\", \"mod\": \"{:X}\", \"host_vs_type\": {}, "
        "\"mode\": {}, \"vs_fn\": \"{}\", \"hs_fn\": \"{}\", \"ds_fn\": \"{}\", "
        "\"gs_fn\": \"{}\", \"output_primitive\": {}, \"vs_output_size\": {}, "
        "\"gs_max_input_prims\": {}, \"hs_max_patches\": {}, "
        "\"hs_input_cp\": {}, \"hs_max_object_threads\": {}, "
        "\"hs_max_tess_factor\": {}}}\n",
        domain_translation->shader().ucode_data_hash(),
        domain_translation->modification(),
        uint32_t(primitive_processing_result.host_vertex_shader_type),
        uint32_t(tessellation_mode), vertex_stage->function_name,
        hull_stage->function_name, domain_stage->function_name,
        geometry_function, uint32_t(pipeline_config.outputPrimitiveType),
        pipeline_config.vsOutputSizeInBytes,
        pipeline_config.gsMaxInputPrimitivesPerMeshThreadgroup,
        pipeline_config.hsMaxPatchesPerObjectThreadgroup,
        pipeline_config.hsInputControlPointCount,
        pipeline_config.hsMaxObjectThreadsPerThreadgroup,
        pipeline_config.hsMaxTessellationFactor);
    NativeTessDumpFile(
        fmt::format("tess_{:016X}_{:X}.json",
                    domain_translation->shader().ucode_data_hash(),
                    domain_translation->modification()),
        std::vector<uint8_t>(json.begin(), json.end()));
  }

  // Builds the pipeline for a given fragment library; kept for the
  // FABLE2_NS_EMU_PIPELINES background rebuild (see the geometry pipeline).
  uint32_t blendcontrol[4];
  std::memcpy(blendcontrol, key_data.blendcontrol, sizeof(blendcontrol));
  const uint32_t normalized_color_mask = key_data.normalized_color_mask;
  // The converter's entry point; the native translation's library has its
  // own (FABLE2_NATIVE_XLATE), chosen per library at build time.
  const std::string fragment_function =
      (pixel_function && pixel_translation && !use_fallback_pixel_shader)
          ? pixel_translation->function_name()
          : std::string(pixel_function ? pixel_function : "");
  MetalShader::MetalTranslation* fragment_translation =
      use_fallback_pixel_shader ? nullptr : pixel_translation;
  auto build = [device = device_, vertex_stage, hull_stage, domain_stage,
                geometry_function, pipeline_config, color_formats,
                depth_format, stencil_format, sample_count,
                normalized_color_mask, blendcontrol, fragment_function,
                fragment_translation](
                   MTL::Library* fragment_library,
                   NS::Error** error) -> MTL::RenderPipelineState* {
    MTL::MeshRenderPipelineDescriptor* desc =
        MTL::MeshRenderPipelineDescriptor::alloc()->init();
    for (uint32_t i = 0; i < 4; ++i) {
      desc->colorAttachments()->object(i)->setPixelFormat(color_formats[i]);
    }
    desc->setDepthAttachmentPixelFormat(depth_format);
    desc->setStencilAttachmentPixelFormat(stencil_format);
    desc->setRasterSampleCount(sample_count);
    desc->setAlphaToCoverageEnabled(false);
    ApplyBlendStateToDescriptor(desc->colorAttachments(),
                                normalized_color_mask, blendcontrol);
    IRGeometryTessellationEmulationPipelineDescriptor ir_desc = {};
    ir_desc.stageInLibrary = vertex_stage->stage_in_library;
    ir_desc.vertexLibrary = vertex_stage->library;
    ir_desc.vertexFunctionName = vertex_stage->function_name.c_str();
    ir_desc.hullLibrary = hull_stage->library;
    ir_desc.hullFunctionName = hull_stage->function_name.c_str();
    ir_desc.domainLibrary = domain_stage->library;
    ir_desc.domainFunctionName = domain_stage->function_name.c_str();
    ir_desc.geometryLibrary = nullptr;
    ir_desc.geometryFunctionName = geometry_function;
    ir_desc.fragmentLibrary = fragment_library;
    ir_desc.fragmentFunctionName =
        NativeXlateEntryIn(fragment_library, fragment_translation)
            ? "xe_native_ps"
            : fragment_function.c_str();
    ir_desc.basePipelineDescriptor = desc;
    ir_desc.pipelineConfig = pipeline_config;
    MTL::RenderPipelineState* result =
        IRRuntimeNewGeometryTessellationEmulationPipeline(device, &ir_desc,
                                                          error);
    desc->release();
    return result;
  };

  NS::Error* error = nullptr;
  auto pipeline_start = std::chrono::steady_clock::now();
  MTL::RenderPipelineState* pipeline = build(pixel_library, &error);
  auto pipeline_end = std::chrono::steady_clock::now();
  RecordRenderPipelineCreation(pipeline != nullptr,
                               ElapsedMs(pipeline_start, pipeline_end));
  if (!pipeline) {
    REXLOG_ERROR(
        "Failed to create tessellation pipeline state: {}",
        error ? error->localizedDescription()->utf8String() : "unknown error");
    return nullptr;
  }

  TessellationPipelineState state;
  state.pipeline = pipeline;
  state.config = pipeline_config;
  state.primitive = geometry_primitive;
  // FABLE2_NS_SPECIALIZE also keeps the rebuild closure (specialized
  // variant), even when the optimized library was used from the start.
  const bool ns_emu_swappable = !use_fallback_pixel_shader &&
                                NativeShadersEmuPipelines() && !pixel_optimized;
  const bool ns_emu_spec =
      !use_fallback_pixel_shader && pixel_translation && NativeShadersMode() &&
      ns_spec::Enabled() &&
      ns_spec::Candidate(pixel_translation->shader().ucode_data_hash());
  if (ns_emu_swappable || ns_emu_spec) {
    auto swap = std::make_shared<NsEmuSwap>();
    swap->pixel_translation = pixel_translation;
    swap->swappable = ns_emu_swappable;
    swap->tessellation = true;
    swap->build = [build](MTL::Library* library) {
      NS::Error* build_error = nullptr;
      return build(library, &build_error);
    };
    state.ns_emu = swap;
  }

  auto [inserted_it, inserted] =
      tessellation_pipeline_cache_.emplace(key, std::move(state));
  if (inserted_it->second.ns_emu && inserted_it->second.ns_emu->swappable) {
    RegisterNsEmuSwap(inserted_it->second.ns_emu);
  }
  MetalPipelineDescription stored_description =
      BuildTessellationPipelineDescription(
          domain_translation, pixel_translation, primitive_processing_result,
          tessellation_mode, attachment_formats, rendering_key);
  QueueStoredShader(static_cast<MetalShader&>(domain_translation->shader()));
  if (pixel_translation) {
    QueueStoredShader(static_cast<MetalShader&>(pixel_translation->shader()));
  }
  QueueStoredPipeline(
      stored_description,
      XXH3_64bits(&stored_description, sizeof(stored_description)));
  return &inserted_it->second;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe

// Window close (src/core/fable_2_app.h OnWindowCloseRequested, via dlsym):
// save the binary archives before the SDK hard-exits (FABLE2_NS2_ARCHIVE).
extern "C" __attribute__((visibility("default"))) void
fable2_pipeline_archive_flush() {
  using rex::graphics::metal::MetalPipelineCache;
  if (!rex::graphics::metal::Ns2ArchiveEnabled()) {
    return;
  }
  MetalPipelineCache* cache =
      rex::graphics::metal::g_archive_flush_cache.load(
          std::memory_order_acquire);
  if (cache) {
    cache->FlushPipelineBinaryArchiveForQuit();
  }
}
