/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHADER_H_
#define XENIA_GPU_METAL_METAL_SHADER_H_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rex/graphics/metal/metal_ns_specialize.h"
#include "rex/graphics/pipeline/shader/dxbc.h"
#include "rex/ui/metal/metal_api.h"

namespace rex {
namespace graphics {
namespace metal {

class DxbcToDxilConverter;
class MetalShaderConverter;
struct MetalStageCompileResult;

class MetalShader : public DxbcShader {
 public:
  class MetalTranslation : public DxbcTranslation {
   public:
    MetalTranslation(MetalShader& shader, uint64_t modification)
        : DxbcTranslation(shader, modification) {}
    ~MetalTranslation();

    // Install a stage-compile result as Metal library/function state.
    bool InstallMetal(MTL::Device* device,
                      const MetalStageCompileResult& result,
                      uint64_t* out_new_library_ms = nullptr,
                      bool* out_new_library_created = nullptr);

    void SetDxilData(std::vector<uint8_t> dxil_data);
    std::vector<uint8_t> GetDxilDataCopy() const;

    // Get the Metal library (contains compiled shader)
    MTL::Library* metal_library() const { return metal_library_; }

    // Get the Metal function (shader entry point)
    MTL::Function* metal_function() const { return metal_function_; }

    // Check if translation succeeded
    bool is_valid() const { return metal_function_ != nullptr; }

    // FABLE2_NATIVE_SHADERS (docs/native-shaders.md): the AIR-optimized entry
    // point, once available (published by a background optimizer thread, or
    // at install when the optimized library came from the disk cache). Null
    // until then; pipelines fall back to metal_function().
    MTL::Function* optimized_function() const {
      return optimized_function_.load(std::memory_order_acquire);
    }
    // The library that holds optimized_function(): the optimized library, or
    // the installed one when that was already optimized (cache hit at
    // install). Only valid once optimized_function() is non-null.
    MTL::Library* optimized_library() const {
      if (!optimized_function()) {
        return nullptr;
      }
      return optimized_library_ ? optimized_library_ : metal_library_;
    }
    MTL::Function* best_function() const {
      MTL::Function* f = optimized_function();
      return f ? f : metal_function_;
    }
    // Creates a library from optimized bytes and publishes its function.
    // Returns false (nothing published) when Metal rejects it.
    bool InstallOptimizedMetal(MTL::Device* device,
                               const std::vector<uint8_t>& metallib);
    // Marks the installed library as the optimized one (cache hit at install).
    void MarkInstalledAsOptimized();
    // FABLE2_NATIVE_XLATE (docs/native-translator.md): publishes a library
    // compiled from the native Xenos -> MSL translation as the optimized
    // function (same ABI as the converter's; its own entry point name).
    // Takes ownership of library and function. False if one is published.
    bool InstallNativeMetal(MTL::Library* library, MTL::Function* function,
                            const std::string& function_name);
    // FABLE2_NATIVE_FIRST (docs/no-converter.md): installs the native
    // translation as the shader's only library, before (and instead of) any
    // converter library. It is also published as the optimized function, so
    // pipelines take the native path from their first build. Takes ownership
    // of library and function. False (nothing installed) if a library is
    // already installed.
    bool InstallNativePrimary(MTL::Library* library, MTL::Function* function,
                              const std::string& function_name);
    // Whether the installed library is the native translation (no converter
    // library exists for this shader).
    bool is_native_primary() const {
      return native_primary_.load(std::memory_order_acquire);
    }
    bool is_native_xlate() const {
      return native_xlate_.load(std::memory_order_acquire);
    }
    // Entry point name inside optimized_library() (native or converter).
    const std::string& optimized_function_name() const {
      return is_native_xlate() ? native_function_name_ : function_name_;
    }
    // Draw count (lookups), used to optimize the hottest shaders first.
    void NoteUse() { use_count_.fetch_add(1, std::memory_order_relaxed); }
    uint32_t use_count() const {
      return use_count_.load(std::memory_order_relaxed);
    }
    // 0 none, 1 queued for background optimization, 2 done (ready or failed).
    std::atomic<uint8_t>& native_shader_state() { return native_shader_state_; }

    // FABLE2_NS_SPECIALIZE (metal_ns_specialize.h): hash of the plain
    // (converter) library, set before install; and the specialized variant
    // once published (owned by the translation, never replaced).
    void SetPlainKey(const ns_spec::PlainKey& key) { plain_key_ = key; }
    const ns_spec::PlainKey& plain_key() const { return plain_key_; }
    const ns_spec::Variant* spec_variant() const {
      return spec_variant_.load(std::memory_order_acquire);
    }
    void PublishSpecVariant(std::unique_ptr<ns_spec::Variant> variant) {
      spec_variant_owner_ = std::move(variant);
      spec_variant_.store(spec_variant_owner_.get(), std::memory_order_release);
    }
    // 0 none, 1 queued, 2 done (published, nothing to do, or failed).
    std::atomic<uint8_t>& spec_state() { return spec_state_; }
    // Auto-learn (FABLE2_NS_SPECIALIZE_AUTOLEARN): set by the background
    // specialization when no converged learn data exists (sampling wanted);
    // the sampling state itself is command-processor thread only.
    std::atomic<uint8_t>& spec_needs_learn() { return spec_needs_learn_; }
    ns_spec::AutoLearn* spec_auto() const { return spec_auto_; }
    void set_spec_auto(ns_spec::AutoLearn* a) { spec_auto_ = a; }

    // Thread-safe translation claiming for async pipeline compilation.
    // Returns true if this thread successfully claimed translation rights.
    // Returns false if another thread already claimed it.
    bool TryClaimTranslation() {
      return !translation_claimed_.test_and_set(std::memory_order_acq_rel);
    }

    // Get intermediate data for debugging
    const std::vector<uint8_t>& dxil_data() const { return dxil_data_; }
    const std::vector<uint8_t>& metallib_data() const { return metallib_data_; }
    const std::string& function_name() const { return function_name_; }

   private:
    std::atomic_flag translation_claimed_ = ATOMIC_FLAG_INIT;
    mutable std::mutex metal_translation_mutex_;
    MTL::Library* metal_library_ = nullptr;
    MTL::Function* metal_function_ = nullptr;
    MTL::Library* optimized_library_ = nullptr;
    std::atomic<MTL::Function*> optimized_function_{nullptr};
    std::atomic<uint32_t> use_count_{0};
    std::atomic<uint8_t> native_shader_state_{0};
    ns_spec::PlainKey plain_key_;
    std::unique_ptr<ns_spec::Variant> spec_variant_owner_;
    std::atomic<const ns_spec::Variant*> spec_variant_{nullptr};
    std::atomic<uint8_t> spec_state_{0};
    std::atomic<uint8_t> spec_needs_learn_{0};
    ns_spec::AutoLearn* spec_auto_ = nullptr;
    std::vector<uint8_t> dxil_data_;
    std::vector<uint8_t> metallib_data_;
    std::string function_name_;
    std::string native_function_name_;
    std::atomic<bool> native_xlate_{false};
    std::atomic<bool> native_primary_{false};
  };

  MetalShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
              const uint32_t* ucode_dwords, size_t ucode_dword_count,
              std::endian ucode_source_endian = std::endian::big);

  struct DrawConstantMetadata {
    DxbcShader::FetchConstantDwordMask shader_fetch_constant_dword_mask = {};
    uint32_t descriptor_indices_word_count = 1;
    uint32_t active_cbv_mask = 0;
  };

  const DrawConstantMetadata& GetDrawConstantMetadata() const;

  // For owning subsystem like the pipeline cache, accessors for unique
  // identifiers (used instead of hashes to make sure collisions can't happen)
  // of binding layouts used by the shader, for invalidation if a shader with an
  // incompatible layout was bound.
  size_t GetTextureBindingLayoutUserUID() const {
    return texture_binding_layout_user_uid_;
  }
  size_t GetSamplerBindingLayoutUserUID() const {
    return sampler_binding_layout_user_uid_;
  }
  // Modifications of the same shader can be translated on different threads.
  // The "set" function must only be called if "enter" returned true - these are
  // set up only once.
  bool EnterBindingLayoutUserUIDSetup() {
    return !binding_layout_user_uids_set_up_.test_and_set();
  }
  void SetTextureBindingLayoutUserUID(size_t uid) {
    texture_binding_layout_user_uid_ = uid;
  }
  void SetSamplerBindingLayoutUserUID(size_t uid) {
    sampler_binding_layout_user_uid_ = uid;
  }

  // [ssmask] FABLE2_EFFECTS_FOOTPRINT_VARIANT: the fetch constants of the 2D
  // texture fetches with a nonzero texel offset along x (bits 0-31) and
  // along y (bits 32-63), from the analyzed ucode (computed once). These are
  // the only fetches the native translator's effects footprint can touch.
  uint64_t OffsetFetchConstantMasks() const;

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;

 private:
  std::atomic_flag binding_layout_user_uids_set_up_ = ATOMIC_FLAG_INIT;
  size_t texture_binding_layout_user_uid_ = 0;
  size_t sampler_binding_layout_user_uid_ = 0;
  mutable std::once_flag offset_fetch_masks_once_;
  mutable uint64_t offset_fetch_masks_ = 0;
  mutable std::once_flag draw_constant_metadata_once_;
  mutable DrawConstantMetadata draw_constant_metadata_ = {};
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_SHADER_H_
