/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PIPELINE_CACHE_H_
#define XENIA_GPU_METAL_METAL_PIPELINE_CACHE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rex/hash.h"
#include "rex/string/buffer.h"
#include "rex/graphics/metal/metal_msl_translator.h"
#include "rex/graphics/pipeline/shader/dxbc_translator.h"
#include "rex/graphics/metal/dxbc_to_dxil_converter.h"
#include "rex/graphics/metal/metal_geometry_shader.h"
#include "rex/graphics/metal/metal_pipeline_failure_backoff.h"
#include "rex/graphics/metal/metal_readiness.h"
#include "rex/graphics/metal/metal_shader.h"
#include "rex/graphics/metal/metal_shader_converter.h"
#include "rex/graphics/metal/metal_stage_compile_cache.h"
#include "rex/graphics/primitive_processor.h"
#include "rex/graphics/register_file.h"
#include "rex/graphics/registers.h"
#include "rex/graphics/shader_storage.h"
// clang-format off
// Must come after Metal.hpp (included transitively via metal_shader.h)
#include "third_party/metal-shader-converter/include/metal_irconverter_runtime.h"
// clang-format on
#include "rex/ui/metal/metal_api.h"

namespace rex {
namespace graphics {
namespace metal {

// Attachment format snapshot used to decouple pipeline creation from render
// target cache / render pass descriptor state.
struct PipelineAttachmentFormats {
  MTL::PixelFormat color_formats[4];
  MTL::PixelFormat depth_format;
  MTL::PixelFormat stencil_format;
  uint32_t sample_count;
};

// Fixed-function rendering state derived from registers that is common to all
// pipeline paths (standard, geometry emulation, tessellation emulation).
// Computed once per draw and passed into each GetOrCreate* method so the
// duplicated register reads are eliminated.
struct PipelineRenderingKey {
  uint32_t normalized_color_mask;
  uint32_t alpha_to_mask_enable;
  uint32_t blendcontrol[4];
};

struct MetalPipelineRuntimeStats {
  uint64_t dxil_convert_requests = 0;
  uint64_t dxil_cache_hits = 0;
  uint64_t dxil_cache_misses = 0;
  uint64_t dxil_convert_failures = 0;
  uint64_t dxil_convert_dxbc_bytes = 0;
  uint64_t dxil_convert_dxil_bytes = 0;
  uint64_t dxil_convert_ms_total = 0;
  uint64_t dxil_convert_ms_max = 0;
  uint64_t library_requests = 0;
  uint64_t library_failures = 0;
  uint64_t library_bytes = 0;
  uint64_t library_ms_total = 0;
  uint64_t library_ms_max = 0;
  uint64_t render_pipeline_requests = 0;
  uint64_t render_pipeline_failures = 0;
  uint64_t render_pipeline_ms_total = 0;
  uint64_t render_pipeline_ms_max = 0;
};

// Derive the shared PipelineRenderingKey from the current register state and
// the pixel translation (which provides writes_color_targets).
PipelineRenderingKey ResolvePipelineRenderingKey(
    const RegisterFile& regs,
    const MetalShader::MetalTranslation* pixel_translation,
    bool use_fallback_pixel_shader);

class MetalPipelineCache {
 public:
  static constexpr size_t kLayoutUIDEmpty = 0;

  MetalPipelineCache(MTL::Device* device, const RegisterFile& register_file);
  ~MetalPipelineCache();

  // Non-copyable.
  MetalPipelineCache(const MetalPipelineCache&) = delete;
  MetalPipelineCache& operator=(const MetalPipelineCache&) = delete;

  // Initialize shader translation components.  Must be called after
  // construction; returns false on failure.
  bool InitializeShaderTranslation(bool gamma_render_target_as_unorm8,
                                   bool msaa_2x_supported,
                                   uint32_t draw_resolution_scale_x,
                                   uint32_t draw_resolution_scale_y);

  // Shader storage (disk cache) lifecycle.
  void InitializeShaderStorage(const std::filesystem::path& cache_root,
                               uint32_t title_id, bool blocking);
  void ShutdownShaderStorage();
  void EndSubmission();

  // Load or retrieve a cached shader from the ucode hash.
  Shader* LoadShader(xenos::ShaderType shader_type,
                     const uint32_t* host_address, uint32_t dword_count);
  void SetupShaderBindingLayoutUserUIDs(MetalShader& shader);

  // Per-draw shader modification selection (mirrors D3D12 PipelineCache).
  DxbcShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  // [ssmask] FABLE2_EFFECTS_FOOTPRINT_VARIANT: a Metal-only pixel-shader
  // modification bit (dword 1 bit 28, unused by the DXBC translator): the
  // native translation includes the effects footprint code.
  static constexpr uint64_t kPixelModificationFootprint = uint64_t(1) << 60;
  DxbcShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control) const;

  enum class PipelineKind : uint32_t {
    kStandard = 0,
    kGeometry = 1,
    kTessellation = 2,
  };

  REXPACKEDSTRUCT(MetalPipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;
    uint64_t auxiliary_hash;
    uint32_t auxiliary0;
    uint32_t auxiliary1;
    uint32_t auxiliary2;
    uint32_t auxiliary3;
    uint32_t kind;
    uint32_t sample_count;
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t color_formats[4];
    uint32_t normalized_color_mask;
    uint32_t alpha_to_mask_enable;
    uint32_t blendcontrol[4];

    static constexpr uint32_t kVersion = 0x20260530;
  });

  REXPACKEDSTRUCT(MetalPipelineStoredDescription, {
    uint64_t description_hash;
    MetalPipelineDescription description;
  });

  // Handle for a standard render pipeline.  The state pointer is set
  // atomically so background compilation threads can publish results that
  // the render thread picks up via acquire loads.
  struct PipelineHandle {
    std::atomic<MTL::RenderPipelineState*> state{nullptr};
    // A null state can mean either pending or failed. Publish completion even
    // on failure so a draw waiting for compilation cannot wait indefinitely.
    std::atomic<bool> creation_complete{false};
    MTL::RenderPipelineState* WaitForCreation() const {
      if (auto* ready = state.load(std::memory_order_acquire)) return ready;
      creation_complete.wait(false, std::memory_order_acquire);
      return state.load(std::memory_order_acquire);
    }
    uint64_t description_hash = 0;
    MetalPipelineDescription description = {};
    // Data needed by background thread to create the pipeline.
    // Cleared after creation.
    MetalShader::MetalTranslation* pending_vertex_translation{nullptr};
    MetalShader::MetalTranslation* pending_pixel_translation{nullptr};
    PipelineAttachmentFormats pending_formats{};
    uint32_t pending_normalized_color_mask{0};
    uint32_t pending_blendcontrol[4]{};
    bool pending_alpha_to_mask{false};
    bool storage_write_pending = false;
    uint8_t priority{0};
    // Command-processor thread only. Preserve cached draws during startup.
    bool startup_warmup = false;
    // FABLE2_NATIVE_SHADERS (docs/native-shaders.md): the translations kept
    // for the background rebuild with the optimized pixel shader (the
    // pending_* pointers are cleared after the first creation), whether
    // `state` already uses the optimized pixel function, and the plain
    // pipeline it replaced (kept alive: in-flight command buffers may still
    // reference it; released with the cache).
    MetalShader::MetalTranslation* ns_vertex_translation{nullptr};
    MetalShader::MetalTranslation* ns_pixel_translation{nullptr};
    std::atomic<bool> ns_optimized{false};
    std::atomic<bool> ns_swap_queued{false};
    // A swap found nothing ready yet (native-shaders workers): counted when a later swap delivers.
    std::atomic<bool> ns_swap_deferred{false};
    MTL::RenderPipelineState* ns_plain_state{nullptr};
    // FABLE2_NATIVE_XLATE: `state` uses the native vertex function; pipelines
    // replaced by a second swap (kept alive like ns_plain_state).
    std::atomic<bool> ns_vs_native{false};
    std::vector<MTL::RenderPipelineState*> ns_retired_states;
    // FABLE2_NS_SPECIALIZE (metal_ns_specialize.h): the same pipeline with the
    // specialized pixel function, built in the background on the first draw
    // whose constants match the variant's guard. 0 none, 1 queued, 2 done.
    std::atomic<MTL::RenderPipelineState*> ns_spec_state{nullptr};
    std::atomic<uint8_t> ns_spec_status{0};
  };

  // Pipeline state creation -- all accept pre-resolved attachment formats and
  // the shared rendering key derived by ResolvePipelineRenderingKey().
  PipelineHandle* GetOrCreatePipelineState(
      MetalShader::MetalTranslation* vertex_translation,
      MetalShader::MetalTranslation* pixel_translation,
      const PipelineAttachmentFormats& attachment_formats,
      const PipelineRenderingKey& rendering_key);

  MTL::RenderPipelineState* WaitForPipeline(PipelineHandle* handle);

  // Returns true while background pipeline creation threads are busy.
  bool IsCreatingPipelines();

  // [ready] Background shader work still pending (readiness bg_busy; GPU Commands thread).
  readiness::Background ReadinessBackground();
  // [shader-prep] counters for the FABLE2_SHADER_PREP status file (JSON members, no braces).
  std::string ShaderPrepCountsJson();
  // [bank-prep] FABLE2_SHADER_PREP_BANK (docs/shader-precompile-20261015.md): headless prep only. Loads every
  // shader of the player's disc shader bank, creates the translations the draw path most likely asks for
  // (modifications predicted from each shader alone) and translates + MSL-compiles them on background threads,
  // so the native translations and macOS's Metal cache are warm before the first play. Command-processor
  // thread; returns the number of translations queued.
  size_t StartBankPrecompile(const std::filesystem::path& bank_path);

  MetalStageCompileCacheStats GetAndResetStageCompileStats();
  MetalPipelineRuntimeStats GetAndResetRuntimeStats();

  // FABLE2_NS_EMU_PIPELINES (docs/native-shaders.md, "Emulation pipelines"):
  // a geometry- or tessellation-emulation pipeline created with a plain pixel
  // shader is rebuilt in the background with the optimized pixel library once
  // that is ready, and published here. The plain pipeline stays alive (in-flight
  // command buffers may reference it); both are released with the cache.
  struct NsEmuSwap {
    std::atomic<MTL::RenderPipelineState*> optimized{nullptr};
    std::atomic<bool> queued{false};
    // FABLE2_NS_SPECIALIZE: the rebuild with the specialized pixel library
    // (0 none, 1 queued, 2 done).
    std::atomic<MTL::RenderPipelineState*> spec{nullptr};
    std::atomic<uint8_t> spec_status{0};
    MetalShader::MetalTranslation* pixel_translation = nullptr;
    // Registered for the optimized-library rebuild (created plain).
    bool swappable = false;
    bool tessellation = false;
    // Rebuilds the same pipeline with another fragment library (same entry point).
    std::function<MTL::RenderPipelineState*(MTL::Library*)> build;
  };

  struct GeometryPipelineState {
    MTL::RenderPipelineState* pipeline = nullptr;
    uint32_t gs_vertex_size_in_bytes = 0;
    uint32_t gs_max_input_primitives_per_mesh_threadgroup = 0;
    std::shared_ptr<NsEmuSwap> ns_emu;
    // The pipeline to bind: the optimized rebuild once published.
    MTL::RenderPipelineState* current_pipeline() const {
      if (ns_emu) {
        if (auto* p = ns_emu->optimized.load(std::memory_order_acquire)) {
          return p;
        }
      }
      return pipeline;
    }
  };

  struct TessellationPipelineState {
    MTL::RenderPipelineState* pipeline = nullptr;
    IRRuntimeTessellationPipelineConfig config = {};
    IRRuntimePrimitiveType primitive = IRRuntimePrimitiveTypeTriangle;
    std::shared_ptr<NsEmuSwap> ns_emu;
    MTL::RenderPipelineState* current_pipeline() const {
      if (ns_emu) {
        if (auto* p = ns_emu->optimized.load(std::memory_order_acquire)) {
          return p;
        }
      }
      return pipeline;
    }
  };

  GeometryPipelineState* GetOrCreateGeometryPipelineState(
      MetalShader::MetalTranslation* vertex_translation,
      MetalShader::MetalTranslation* pixel_translation,
      GeometryShaderKey geometry_shader_key,
      const PipelineAttachmentFormats& attachment_formats,
      const PipelineRenderingKey& rendering_key);

  TessellationPipelineState* GetOrCreateTessellationPipelineState(
      MetalShader::MetalTranslation* domain_translation,
      MetalShader::MetalTranslation* pixel_translation,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      const PipelineAttachmentFormats& attachment_formats,
      const PipelineRenderingKey& rendering_key);

  // FABLE2_NATIVE_TESS (default 1 = exact only since 2026-10-13, =0 emulation):
  // a plain render pipeline with Metal's hardware tessellator (fractional-even
  // quads, half factors per patch) and the native domain shader as the
  // post-tessellation vertex function. Null when the draw is not adaptive quad tessellation or the
  // domain shader has no native translation; the caller then uses emulation.
  // cpu_pattern: a plain triangle pipeline whose vertex function is the
  // domain shader fed with the D3D11 reference tessellator's points
  // (xe_native_ds_cpu) instead of Metal's tessellator.
  MTL::RenderPipelineState* GetOrCreateNativeTessPipeline(
      MetalShader::MetalTranslation* domain_translation,
      MetalShader::MetalTranslation* pixel_translation,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      const PipelineAttachmentFormats& attachment_formats,
      const PipelineRenderingKey& rendering_key, bool cpu_pattern = false);

  // FABLE2_NATIVE_POINTS / FABLE2_NATIVE_RECTS (default on since 2026-10-13,
  // =0 emulation; points default to the mesh path, =1 this one): a standard
  // pipeline whose native vertex shader expands each guest point (rectangle)
  // to a triangle-strip instance with the point-list (rectangle-list)
  // geometry shader's math. Null when the key is not a plain point or
  // rectangle list (cull distances, kill-AND), the mode is off, or there is
  // no native translation.
  MTL::RenderPipelineState* GetOrCreateNativePointsPipeline(
      MetalShader::MetalTranslation* vertex_translation,
      MetalShader::MetalTranslation* pixel_translation,
      GeometryShaderKey geometry_shader_key,
      const PipelineAttachmentFormats& attachment_formats,
      const PipelineRenderingKey& rendering_key);

  // Ensure the shared DXBC translator has produced DXBC bytecode.
  bool EnsureDxbcTranslationReady(MetalShader::MetalTranslation* translation,
                                  const char* stage_name);
  bool EnsureDxilTranslationReady(MetalShader::MetalTranslation* translation,
                                  const char* stage_name);
  // FABLE2_NATIVE_SHADERS: cache key of the optimized form of a plain
  // pixel-shader metallib; disk-cache lookup (no tools run); and the
  // optimization itself (runs metal-opt + metallib, background threads only).
  bool LoadOptimizedPixelShader(const std::vector<uint8_t>& plain,
                                std::vector<uint8_t>* optimized);
  bool OptimizeGuestPixelShader(uint64_t ucode_hash,
                                const std::vector<uint8_t>& plain,
                                std::vector<uint8_t>* optimized);
  // FABLE2_NATIVE_FIRST: 1 installed natively, -1 another thread installed
  // the shader meanwhile, 0 not translatable (converter path).
  int TryNativeFirst(MetalShader::MetalTranslation* translation);
  // Translates (no compile) with `baked` and returns the baked dwords the
  // code reads.
  bool NativeXlateBakedUse(MetalShader::MetalTranslation* translation,
                           const std::vector<MslBakedDword>& baked,
                           std::vector<MslBakedDword>* used);
  bool EnsureMetalTranslationReady(MetalShader::MetalTranslation* translation);

  // FABLE2_NS_SPECIALIZE (metal_ns_specialize.h): the specialized pipeline of
  // a standard / emulation pipeline is built in the background on request
  // (the first draw whose constants match the guard); no-op once requested.
  void RequestSpecPipeline(PipelineHandle* handle);
  void RequestSpecEmuPipeline(const std::shared_ptr<NsEmuSwap>& swap);
  // Auto-learn converged for a translation without a variant: specialize it
  // again in the background (command-processor thread).
  void RequestRespecialization(MetalShader::MetalTranslation* translation);

  // Ensure the depth-only pixel shader is compiled and ready.
  bool EnsureDepthOnlyPixelShader();
  // The depth-only fragment function (created once, owned by the cache).
  MTL::Function* DepthOnlyPixelFunction();

  // Ucode disassembly scratch buffer (shared with command processor).
  rex::string::StringBuffer& ucode_disasm_buffer() { return ucode_disasm_buffer_; }

  // Serialize pipeline binary archive to disk.
  void SerializePipelineBinaryArchive();

  // FABLE2_NS2_ARCHIVE: save every dirty archive set synchronously (window
  // close / quit: the app hard-exits without the renderer shutdown, so
  // pipelines added since the last periodic save would be lost).
  void FlushPipelineBinaryArchiveForQuit();

 private:
  bool InitializeShaderStorageInternal(const std::filesystem::path& cache_root,
                                       uint32_t title_id, bool blocking);
  std::string GetShaderStorageDeviceTag() const;
  std::string GetShaderStorageAbiTag() const;
  bool InitializePipelineBinaryArchive(
      const std::filesystem::path& archive_path, size_t set_index);
  void PrewarmStoredPipelines(
      const std::vector<MetalPipelineStoredDescription>& descriptions,
      bool blocking);
  MetalShader* LoadShader(xenos::ShaderType shader_type,
                          const uint32_t* host_address, uint32_t dword_count,
                          uint64_t data_hash);
  MetalPipelineDescription BuildStandardPipelineDescription(
      MetalShader::MetalTranslation* vertex_translation,
      MetalShader::MetalTranslation* pixel_translation,
      const PipelineAttachmentFormats& attachment_formats,
      const PipelineRenderingKey& rendering_key) const;
  MetalPipelineDescription BuildPipelineDescription(
      PipelineKind kind, MetalShader::MetalTranslation* vertex_translation,
      MetalShader::MetalTranslation* pixel_translation,
      const PipelineAttachmentFormats& attachment_formats,
      const PipelineRenderingKey& rendering_key) const;
  MetalPipelineDescription BuildGeometryPipelineDescription(
      MetalShader::MetalTranslation* vertex_translation,
      MetalShader::MetalTranslation* pixel_translation,
      GeometryShaderKey geometry_shader_key,
      const PipelineAttachmentFormats& attachment_formats,
      const PipelineRenderingKey& rendering_key) const;
  MetalPipelineDescription BuildTessellationPipelineDescription(
      MetalShader::MetalTranslation* domain_translation,
      MetalShader::MetalTranslation* pixel_translation,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      xenos::TessellationMode tessellation_mode,
      const PipelineAttachmentFormats& attachment_formats,
      const PipelineRenderingKey& rendering_key) const;
  void QueueStoredShader(MetalShader& shader);
  void QueueStoredPipeline(const MetalPipelineDescription& description,
                           uint64_t description_hash);
  bool EnsureDxbcTranslationReadyLocked(
      MetalShader::MetalTranslation* translation, const char* stage_name);
  bool ConvertDxbcToDxil(const std::vector<uint8_t>& dxbc_data,
                         std::vector<uint8_t>& dxil_data,
                         std::string* error_message);
  MTL::Library* NewLibraryFromBytes(const std::vector<uint8_t>& bytes,
                                    const char* label);
  void RecordLibraryCreation(size_t byte_count, bool success, uint64_t ms);
  void RecordRenderPipelineCreation(bool success, uint64_t ms);

  MTL::Device* device_;
  const RegisterFile& register_file_;

  // MSC shader translation components.
  std::unique_ptr<DxbcShaderTranslator> shader_translator_;
  std::unique_ptr<DxbcToDxilConverter> dxbc_to_dxil_converter_;
  std::unique_ptr<MetalShaderConverter> metal_shader_converter_;
  std::mutex shader_translation_mutex_;

  std::atomic<uint64_t> dxil_convert_requests_{0};
  std::atomic<uint64_t> dxil_cache_hits_{0};
  std::atomic<uint64_t> dxil_cache_misses_{0};
  std::atomic<uint64_t> dxil_convert_failures_{0};
  // FABLE2_NATIVE_SHADERS (docs/native-shaders.md): optimized pixel shaders
  // installed (and how many came from the disk cache), and fallbacks to the
  // plain converter output.
  std::atomic<uint64_t> native_shaders_optimized_{0};
  std::atomic<uint64_t> native_shaders_cache_hits_{0};
  std::atomic<uint64_t> native_shaders_fallbacks_{0};
  std::atomic<uint64_t> native_shaders_opt_ms_total_{0};
  // Milestone 2 (no first-launch delay): shaders installed plain and queued
  // for background optimization, optimized libraries made ready in the
  // background, plain pipelines created / swapped to optimized, failed swaps.
  std::atomic<uint64_t> native_shaders_queued_{0};
  std::atomic<uint64_t> native_shaders_ready_{0};
  std::atomic<uint64_t> native_shaders_plain_pipelines_{0};
  std::atomic<uint64_t> native_shaders_swapped_{0};
  std::atomic<uint64_t> native_shaders_swap_failed_{0};
  // Swaps that found nothing ready to improve yet (the other shader's ready event swaps later).
  std::atomic<uint64_t> native_shaders_swap_nothing_yet_{0};
  std::atomic<uint64_t> native_shaders_swap_deferred_done_{0};
  std::atomic<uint64_t> dxil_convert_dxbc_bytes_{0};
  std::atomic<uint64_t> dxil_convert_dxil_bytes_{0};
  std::atomic<uint64_t> dxil_convert_ms_total_{0};
  std::atomic<uint64_t> dxil_convert_ms_max_{0};
  std::atomic<uint64_t> library_requests_{0};
  std::atomic<uint64_t> library_failures_{0};
  std::atomic<uint64_t> library_bytes_{0};
  std::atomic<uint64_t> library_ms_total_{0};
  std::atomic<uint64_t> library_ms_max_{0};
  std::atomic<uint64_t> render_pipeline_requests_{0};
  std::atomic<uint64_t> render_pipeline_failures_{0};
  std::atomic<uint64_t> render_pipeline_ms_total_{0};
  std::atomic<uint64_t> render_pipeline_ms_max_{0};

  rex::string::StringBuffer ucode_disasm_buffer_;

  // MSC shader cache (keyed by ucode hash).
  std::unordered_map<uint64_t, std::unique_ptr<MetalShader>> shader_cache_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  // Texture binding layouts of different shaders, for obtaining layout UIDs.
  std::vector<MetalShader::TextureBinding> texture_binding_layouts_;
  // Map of texture binding layouts used by shaders, for obtaining UIDs. Keys
  // are XXH3 hashes of layouts, values need manual collision resolution using
  // layout_vector_offset:layout_length of texture_binding_layouts_.
  std::unordered_multimap<uint64_t, LayoutUID,
                          rex::hash::IdentityHasher<uint64_t>>
      texture_binding_layout_map_;
  // Bindless sampler indices of different shaders, for obtaining layout UIDs.
  std::vector<uint32_t> bindless_sampler_layouts_;
  // Keys are XXH3 hashes of used bindless sampler indices.
  std::unordered_multimap<uint64_t, LayoutUID,
                          rex::hash::IdentityHasher<uint64_t>>
      bindless_sampler_layout_map_;

  class GeneratedStageCache;

  // MSC pipeline caches (keyed by shader combination).
  std::unordered_map<uint64_t, std::unique_ptr<PipelineHandle>> pipeline_cache_;
  std::unordered_map<uint64_t, GeometryPipelineState> geometry_pipeline_cache_;
  PipelineFailureBackoff geometry_pipeline_failures_;
  std::unordered_map<uint64_t, TessellationPipelineState>
      tessellation_pipeline_cache_;
  std::unique_ptr<GeneratedStageCache> generated_stages_;
  // FABLE2_NATIVE_TESS: pipelines by key, native domain functions by domain
  // translation (null: no native translation). Render thread only.
  // Guards the native tessellation / points / rectangles maps below (draw
  // path and the background prewarm, PrewarmNativeGeometry).
  std::mutex native_geom_mutex_;
  MTL::RenderPipelineState* PublishNativeGeomPipeline(
      std::unordered_map<uint64_t, MTL::RenderPipelineState*>& map,
      uint64_t key, MTL::RenderPipelineState* pipeline);
  // FABLE2_NATIVE_GEOM_PERSIST: native point/rectangle/tessellation pipelines
  // are recorded in the pipeline list (so the next launch's storage init
  // prewarms them) and built through the binary archive.
  MTL::RenderPipelineState* NewNativeGeomPipelineState(
      MTL::RenderPipelineDescriptor* desc, NS::Error** error);
  void StoreNativeGeomDescription(
      const MetalPipelineDescription& description,
      MetalShader::MetalTranslation* vertex_translation,
      MetalShader::MetalTranslation* pixel_translation);
  std::unordered_map<uint64_t, MTL::RenderPipelineState*> native_tess_pipelines_;
  std::unordered_map<const void*, MTL::Function*> native_ds_functions_;
  // FABLE2_NATIVE_POINTS: pipelines by key; expansion functions by
  // (translation, point coordinates).
  std::unordered_map<uint64_t, MTL::RenderPipelineState*> native_points_pipelines_;
  std::unordered_map<uint64_t, MTL::Function*> native_points_functions_;

  MTL::Library* depth_only_pixel_library_ = nullptr;
  std::string depth_only_pixel_function_name_;
  MTL::Function* depth_only_pixel_function_ = nullptr;

  // Shader storage paths and writers.
  ShaderStorageWriter<MetalPipelineStoredDescription> storage_writer_;
  std::atomic<bool> shader_storage_file_flush_needed_{false};
  std::atomic<bool> pipeline_storage_file_flush_needed_{false};
  uint32_t shader_storage_title_id_ = 0;
  std::mutex stored_pipeline_mutex_;
  std::unordered_set<uint64_t> stored_pipeline_hashes_;
  std::filesystem::path shader_storage_local_root_;
  std::filesystem::path shader_storage_title_root_;

 public:
  const std::filesystem::path& shader_storage_title_root() const {
    return shader_storage_title_root_;
  }
  // The folder InitializeShaderStorage uses for this cache root and title
  // (computed without touching storage; [xfer-prewarm] starts before it).
  std::filesystem::path ShaderStorageTitleRootFor(
      const std::filesystem::path& cache_root, uint32_t title_id) const;

 private:
  std::filesystem::path artifact_store_path_;
  // One archive set per pixel-shader flavour: [0] plain pipelines (shared
  // with FABLE2_NATIVE_SHADERS off), [1] pipelines with an optimized pixel
  // shader (".ns1" key, only used when the switch is on).
  struct ArchiveSet {
    std::filesystem::path path;
    MTL::BinaryArchive* archive = nullptr;
    bool dirty = false;
    // Archive segments saved by earlier launches, used read-only for lookups.
    // New pipelines go to `archive` (always created empty) and are saved as a
    // new segment file: re-serializing an archive loaded from disk fails or
    // drops its loaded entries (seen on macOS 27).
    std::vector<MTL::BinaryArchive*> segments;
    // When too many segments exist, this launch's archive also receives the
    // pipelines found in older segments and is saved as a ".full" segment
    // that supersedes them.
    bool compacting = false;
  };
  // [2] pipelines with a specialized pixel shader (".ns2", FABLE2_NS_SPECIALIZE).
  static constexpr size_t kArchiveSetCount = 3;
  ArchiveSet archive_sets_[kArchiveSetCount];
  void SerializeArchiveSet(size_t set_index);
  // Guards the archive pointer and dirty flag (held only briefly).
  std::mutex pipeline_binary_archive_mutex_;
  // Serializes archive mutation (add) and serialization to disk.
  std::mutex pipeline_binary_archive_write_mutex_;
  // Background serialization: the app exits without running the renderer
  // shutdown, so the archive is saved while running (after warm-up and
  // periodically when new pipelines were added).
  std::thread pipeline_binary_archive_save_thread_;
  std::atomic<bool> pipeline_binary_archive_saving_{false};
  std::chrono::steady_clock::time_point pipeline_binary_archive_last_save_{};
  // only_set >= 0: save just that set (the NS2 settle save).
  bool MaybeSavePipelineBinaryArchiveAsync(bool force, int only_set = -1);
  void JoinPipelineBinaryArchiveSave();
  // Warm-up / archive statistics (logged when the background warm-up ends).
  std::chrono::steady_clock::time_point warmup_start_{};
  std::atomic<uint64_t> archive_lookup_hits_{0};
  std::atomic<uint64_t> archive_lookup_misses_{0};
  std::atomic<uint64_t> archive_lookup_ms_total_{0};
  std::atomic<uint64_t> archive_add_count_{0};
  std::atomic<uint64_t> archive_add_ms_total_{0};
  std::atomic<uint64_t> pipeline_compile_count_{0};
  std::atomic<uint64_t> pipeline_compile_ms_total_{0};

  // FABLE2_NATIVE_SHADERS background optimizer pool (low QoS): optimizes
  // queued pixel shaders (hottest first), then rebuilds the standard
  // pipelines that use them and swaps them in (docs/native-shaders.md).
  void NativeShaderThread();
  void QueueNativeShaderOptimization(
      MetalShader::MetalTranslation* translation);
  void RegisterNativeShaderHandle(PipelineHandle* handle);
  void NativeShaderSwap(PipelineHandle* handle);
  // FABLE2_NS_EMU_PIPELINES: choose the fragment library for a new emulation
  // pipeline and, when it is plain, register it for the background rebuild.
  MTL::Library* NsEmuFragmentLibrary(MetalShader::MetalTranslation* pixel_translation,
                                     bool* out_optimized);
  void RegisterNsEmuSwap(const std::shared_ptr<NsEmuSwap>& swap);
  void NsEmuRebuild(const std::shared_ptr<NsEmuSwap>& swap);
  void LogNativeShaderProgress(const char* what);
  // FABLE2_NATIVE_XLATE (docs/native-translator.md): translate the guest pixel
  // shader to MSL, compile it and publish it as the optimized function.
  // False (nothing published) on any unsupported feature or compile error.
  bool TryNativeXlate(MetalShader::MetalTranslation* translation);
  // Translates (with optional baked constants) and compiles; the caller owns
  // the returned library and function. False on any failure (logged).
  bool CompileNativeXlate(MetalShader::MetalTranslation* translation,
                          const std::vector<MslBakedDword>& baked,
                          MTL::Library** library_out,
                          MTL::Function** function_out,
                          std::string* entry_out,
                          bool expand_points = false,
                          bool point_coordinates = false,
                          bool expand_rects = false,
                          bool points_mesh = false,
                          bool domain_cpu_pattern = false);
  struct NativeXlateConfig {
    bool gamma_render_target_as_unorm8 = false;
    bool msaa_2x_supported = true;
    uint32_t draw_resolution_scale_x = 1;
    uint32_t draw_resolution_scale_y = 1;
  } native_xlate_config_;
  std::atomic<uint64_t> native_xlate_ok_{0};
  std::atomic<uint64_t> native_xlate_ok_ps_{0};
  std::atomic<uint64_t> native_xlate_ok_vs_{0};
  std::atomic<uint64_t> native_xlate_fallback_{0};
  std::atomic<uint64_t> native_xlate_compile_failed_{0};
  std::atomic<uint64_t> native_xlate_translate_us_{0};
  std::atomic<uint64_t> native_xlate_compile_us_{0};
  std::atomic<uint64_t> native_xlate_compile_max_us_{0};
  std::vector<std::thread> ns_threads_;
  std::mutex ns_mutex_;
  std::condition_variable ns_cond_;
  std::vector<MetalShader::MetalTranslation*> ns_optimize_queue_;
  std::vector<PipelineHandle*> ns_swap_queue_;
  std::unordered_map<MetalShader::MetalTranslation*,
                     std::vector<PipelineHandle*>>
      ns_handles_;
  std::vector<std::shared_ptr<NsEmuSwap>> ns_emu_swap_queue_;
  std::unordered_map<MetalShader::MetalTranslation*,
                     std::vector<std::shared_ptr<NsEmuSwap>>>
      ns_emu_handles_;
  std::atomic<uint64_t> ns_emu_plain_{0};
  std::atomic<uint64_t> ns_emu_swapped_{0};
  std::atomic<uint64_t> ns_emu_swap_failed_{0};
  size_t ns_busy_ = 0;
  bool ns_shutdown_ = false;
  bool ns_idle_logged_ = true;
  bool ns_first_idle_logged_ = false;
  uint32_t ns_items_since_idle_log_ = 0;
  std::chrono::steady_clock::time_point ns_start_{};

  // Async pipeline compilation thread pool.
  void CreationThread(size_t thread_index);
  // Builds the pipeline with the translations' best functions (optimized
  // pixel function when available); *out_optimized says which was used.
  MTL::RenderPipelineState* CreatePipelineFromHandle(
      const PipelineHandle* handle,
      MetalShader::MetalTranslation* vertex_translation,
      MetalShader::MetalTranslation* pixel_translation,
      bool* out_optimized,
      MTL::Function* fragment_override = nullptr,
      bool* out_vertex_native = nullptr);
  // FABLE2_NS_SPECIALIZE background work.
  void QueueSpecialization(MetalShader::MetalTranslation* translation);
  void NsSpecialize(MetalShader::MetalTranslation* translation);
  void NsSpecBuild(PipelineHandle* handle);
  void NsSpecBuildEmu(const std::shared_ptr<NsEmuSwap>& swap);
  void LogSpecProgress(const char* what);
  std::vector<MetalShader::MetalTranslation*> ns_spec_queue_;
  std::vector<PipelineHandle*> ns_spec_build_queue_;
  std::vector<std::shared_ptr<NsEmuSwap>> ns_spec_emu_queue_;
  std::atomic<uint64_t> ns_spec_queued_{0};
  std::atomic<uint64_t> ns_spec_from_cache_{0};
  std::atomic<uint64_t> ns_spec_generated_{0};
  std::atomic<uint64_t> ns_spec_none_{0};
  std::atomic<uint64_t> ns_spec_failed_{0};
  std::atomic<uint64_t> ns_spec_pipelines_{0};
  std::atomic<uint64_t> ns_spec_pipeline_failed_{0};
  std::atomic<uint64_t> ns_spec_ms_{0};
  // FABLE2_NS2_ARCHIVE: specialized (standard) pipelines found in the ".ns2"
  // archive vs compiled, compile ms, thread CPU of the specialized pipeline
  // builds, time of the last add to the ".ns2" archive (steady ns, 0 = none
  // pending) and of the last specialized pipeline built.
  std::atomic<uint64_t> ns2_archive_hits_{0};
  std::atomic<uint64_t> ns2_archive_compiled_{0};
  std::atomic<uint64_t> ns2_compile_ms_{0};
  std::atomic<uint64_t> ns2_build_cpu_us_{0};
  std::atomic<int64_t> ns2_last_add_ns_{0};
  std::atomic<int64_t> ns2_last_pipeline_ns_{0};
  void LogNs2Pipeline(uint64_t n, bool emu);

  std::vector<std::thread> creation_threads_;
  // [startup-par] Extra creation threads for the startup warm-up only; each
  // exits once the creation queue is empty (kWarmupHelperIndex and up).
  static constexpr size_t kWarmupHelperIndex = 1000;
  std::vector<std::thread> warmup_helper_threads_;
  // FABLE2_NATIVE_FIRST: native tessellation / point / rectangle pipelines of
  // the stored pipeline list, built on a background thread at storage init
  // (not on the draw path). Counted in ReadinessBackground().pipelines_pending.
  struct NativeGeomPrewarmJob {
    bool tess = false;
    bool cpu_pattern = false;  // tessellation: the reference-pattern variant
    MetalShader::MetalTranslation* vertex = nullptr;
    MetalShader::MetalTranslation* pixel = nullptr;
    GeometryShaderKey geometry_key;
    PrimitiveProcessor::ProcessingResult primitive = {};
    PipelineAttachmentFormats formats = {};
    PipelineRenderingKey rendering = {};
  };
  void PrewarmNativeGeometry(std::vector<NativeGeomPrewarmJob> jobs);
  std::thread native_geom_prewarm_thread_;
  // [bank-prep] StartBankPrecompile's worker and progress (counted in ReadinessBackground).
  std::thread bank_prep_thread_;
  std::atomic<uint32_t> bank_prep_pending_{0};
  std::atomic<uint32_t> bank_prep_total_{0};
  std::atomic<uint32_t> bank_prep_ok_{0};
  std::atomic<uint32_t> bank_prep_failed_{0};
  std::atomic<uint32_t> bank_prep_skipped_{0};
  std::atomic<uint32_t> bank_prep_shaders_{0};
  std::atomic<uint64_t> bank_prep_ms_{0};
  std::atomic<uint32_t> native_geom_prewarm_pending_{0};
  std::atomic<bool> native_geom_prewarm_stop_{false};
  std::atomic<uint32_t> native_geom_archive_hits_{0};
  std::atomic<uint32_t> native_geom_archive_misses_{0};
  struct PipelineCreationRequest {
    PipelineHandle* handle{nullptr};
  };
  struct PipelineCreationPriorityCompare {
    bool operator()(const PipelineCreationRequest& a,
                    const PipelineCreationRequest& b) const {
      return a.handle->priority < b.handle->priority;
    }
  };
  std::priority_queue<PipelineCreationRequest,
                      std::vector<PipelineCreationRequest>,
                      PipelineCreationPriorityCompare>
      creation_queue_;
  std::mutex creation_request_lock_;
  std::condition_variable creation_request_cond_;
  std::atomic<size_t> creation_threads_busy_{0};
  bool creation_threads_shutdown_{false};
  bool background_warmup_pending_ = false;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_PIPELINE_CACHE_H_
