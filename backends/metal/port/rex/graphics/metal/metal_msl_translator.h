/**
 * Fable II recomp: native Xenos -> Metal Shading Language translator
 * (FABLE2_NATIVE_XLATE, docs/native-translator.md).
 *
 * Emits structured MSL directly from the Xenos ucode (no DXBC/DXIL/Metal
 * Shader Converter): typed locals for the guest registers, structured if
 * blocks for the control flow (no program-counter loop), and direct texture
 * sampling. The output binds exactly like the converter's library (top-level
 * argument buffer at buffer(2), user(texcoordN) inputs, color(N) outputs), so
 * it can replace the converter's function per shader. Semantics mirror the
 * DXBC translator (dxbc_translator*.cpp, the reference and the fallback)
 * operation for operation, including the Xenos 0 * x = 0 multiplication rule.
 *
 * Anything not supported yet makes Translate() return false with a reason;
 * the caller keeps the converter's function for that shader.
 */

#ifndef REX_GRAPHICS_METAL_METAL_MSL_TRANSLATOR_H_
#define REX_GRAPHICS_METAL_METAL_MSL_TRANSLATOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "rex/graphics/pipeline/shader/dxbc.h"
#include "rex/graphics/pipeline/shader/dxbc_translator.h"
#include "rex/graphics/pipeline/shader/translator.h"

namespace rex {
namespace graphics {
namespace metal {

struct MslTranslatorConfig {
  uint32_t draw_resolution_scale_x = 1;
  uint32_t draw_resolution_scale_y = 1;
  bool gamma_render_target_as_unorm8 = false;
  bool msaa_2x_supported = true;
  // Values of the DXBC translator's cvars at translation time.
  bool draw_resolution_scaled_texture_offsets = true;
  bool use_fuzzy_alpha_epsilon = false;
  // FABLE2_EFFECTS_FOOTPRINT (default on; docs/bloom-banding-20261014.md):
  // with offsets in guest texels (draw_resolution_scaled_texture_offsets
  // false, the original effect size) and a resolution-scaled colour texture,
  // a pixel-shader 2D fetch with a nonzero texel offset covers the whole
  // guest texel of its tap along each offset axis instead of one host texel
  // (bilinear: one sample snapped to the host-texel corner at 2x, two at 3x;
  // point on clamp-to-edge: the exact average of the guest texel it falls in,
  // via an in-shader bilinear sampler; other point fetches: the average of
  // the scale host texels around the tap). Blur and bloom taps are one
  // guest texel apart; with one host texel each the kernel has holes, which
  // shows as a grating on thin bright features. Depth and single-channel
  // depth-like formats (k_24_8, k_24_8_FLOAT, k_16, k_16_FLOAT, k_32_FLOAT;
  // shadow-map PCF) keep one host texel.
  bool effects_footprint = false;
  // FABLE2_NATIVE_POINTS (docs/native-translator.md, "Geometry and
  // tessellation emulation"): the vertex shader of a point-list draw expands
  // each guest point to a 4-vertex triangle-strip instance itself, with the
  // geometry shader's math (dxbc_geometry_shader.cc, kPointList). Entry
  // xe_native_vs_points; the point index is the instance, the corner the
  // vertex; XePointDraw at buffer(3) names the guest index buffer, if any.
  bool expand_points = false;
  // FABLE2_NATIVE_RECTS: the same for rectangle lists (entry
  // xe_native_vs_rects): each instance runs the guest vertex shader for the
  // rectangle's three vertices and builds the strip with the 4th vertex
  // mirrored across the longest edge (dxbc_geometry_shader.cc,
  // kRectangleList).
  bool expand_rects = false;
  // FABLE2_NATIVE_POINTS=2: point sprites with the guest vertex shader run
  // once per point: a mesh function alone, 64 points per threadgroup, each
  // thread emitting its sprite's 4 vertices and 2 triangles (entry
  // xe_native_ms_points).
  bool points_mesh = false;
  // Exact tessellation (FABLE2_NATIVE_TESS=3 path, docs/native-translator.md):
  // the quad domain shader as a plain vertex function fed with the D3D11
  // reference tessellator's points (entry xe_native_ds_cpu): vertex id =
  // (patch << 20) | point in the uv arena at buffer(4); patch += instance.
  bool domain_cpu_pattern = false;
  // FABLE2_NATIVE_TESS_SNAP_UV: the tessellator entry (xe_native_ds) snaps the
  // domain location to the 1/65536 grid. Every D3D11 domain point is on it
  // (the reference tessellator works in 16.16 fixed point), so this is a
  // no-op where Metal's tessellator is exact, and it makes the positions of
  // the same patch identical in every pipeline whatever the GPU's own
  // tessellator does in the last bits.
  bool snap_domain_uv = false;
  bool point_coordinates = false;
};

// A dword of a constant buffer whose `mask` bits are known to equal `value`
// for every draw that binds the shader (guarded by the caller, as for the
// hot-shader specialization's variants).
struct MslBakedDword {
  enum Buffer : uint32_t { kSystem = 0, kBoolLoop = 1, kFetch = 2 };
  uint32_t buffer;
  uint32_t index;
  uint32_t value;
  uint32_t mask;
};

struct MslTranslationResult {
  bool ok = false;
  std::string source;       // MSL source when ok.
  std::string entry_point;  // Fragment function name when ok.
  std::string reason;       // Why not, when !ok.
  // With baked dwords: the ones the code actually reads (each once, in the
  // order given), so a guard needs to check only these.
  std::vector<MslBakedDword> baked_used;
};

class MslShaderTranslator : public ShaderTranslator {
 public:
  explicit MslShaderTranslator(const MslTranslatorConfig& config);
  ~MslShaderTranslator() override;

  // `shader` must be analyzed and must already have a successful DXBC
  // translation (the texture and sampler binding lists, and thus the
  // descriptor-index buffer layout, come from it). Pixel shaders only for now.
  MslTranslationResult Translate(DxbcShader& shader, uint64_t modification);
  // Same, with the given constant-buffer bits replaced by literals (the
  // Metal compiler then folds the decode that depends on them).
  MslTranslationResult Translate(DxbcShader& shader, uint64_t modification,
                                 const std::vector<MslBakedDword>& baked);

  // Version of the emitted code; part of any cache key.
  static constexpr uint32_t kVersion = 1;

 protected:
  void Reset() override;
  uint32_t GetModificationRegisterCount() const override;
  void StartTranslation() override;
  std::vector<uint8_t> CompleteTranslation() override;

  void ProcessLabel(uint32_t cf_index) override;
  void ProcessExecInstructionBegin(const ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const ParsedExecInstruction& instr) override;
  void ProcessLoopStartInstruction(
      const ParsedLoopStartInstruction& instr) override;
  void ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) override;
  void ProcessCallInstruction(const ParsedCallInstruction& instr) override;
  void ProcessReturnInstruction(const ParsedReturnInstruction& instr) override;
  void ProcessJumpInstruction(const ParsedJumpInstruction& instr) override;
  void ProcessAllocInstruction(const ParsedAllocInstruction& instr,
                               uint8_t export_eM) override;
  void ProcessVertexFetchInstruction(
      const ParsedVertexFetchInstruction& instr) override;
  void ProcessTextureFetchInstruction(
      const ParsedTextureFetchInstruction& instr) override;
  void ProcessAluInstruction(
      const ParsedAluInstruction& instr,
      uint8_t memexport_eM_potentially_written_before) override;

 private:
  using Modification = DxbcShaderTranslator::Modification;
  using SystemConstants = DxbcShaderTranslator::SystemConstants;

  // An operand as MSL text: base float4 expression, swizzle, modifiers.
  struct Operand {
    std::string base;
    char swizzle[4] = {'x', 'y', 'z', 'w'};
    bool abs = false;
    bool neg = false;
    std::string Vec() const;
    std::string Comp(uint32_t i) const;
  };

  void Fail(const std::string& reason);
  bool failed() const { return !fail_reason_.empty(); }
  Modification modification() const {
    return Modification(current_translation().modification());
  }

  // Code emission.
  template <typename... Args>
  void Line(const char* format, const Args&... args);
  void Open(const std::string& header);
  void Close();
  std::string NewTemp(const char* prefix);

  // System constant access by byte offset.
  std::string SysU(size_t offset) const;
  std::string SysF(size_t offset) const;
  std::string FetchWord(uint32_t fetch_constant, uint32_t word);
  std::string BoolConstant(uint32_t index) const;

  Operand LoadOperand(const InstructionOperand& operand);
  void StoreResult(const InstructionResult& result, const std::string& value4);

  // Control flow (mirrors DxbcShaderTranslator).
  void UpdateExecConditionals(ParsedExecInstruction::Type type,
                              uint32_t bool_constant_index, bool condition);
  void CloseExecConditionals();
  void UpdateInstructionPredication(bool predicated, bool condition);
  void CloseInstructionPredication();

  void EmitVectorOp(const ParsedAluInstruction& instr, std::string& result,
                    bool& predicate_written);
  void EmitScalarOp(const ParsedAluInstruction& instr, bool& predicate_written);
  void KillPixel(const std::string& condition);

  void PWLGammaToLinear(const std::string& value);
  void PreSaturatedLinearToPWLGamma(const std::string& value);

  void CompletePixelShader();
  void StartVertexShader();
  void ExportToMemory(uint8_t export_eM);
  void CompleteVertexShader();
  bool vertex_pse_written_ = false;
  void EmitAlphaToMaskSample(bool initialize, uint32_t sample_index,
                             float threshold_base, const std::string& offset,
                             float offset_scale, const std::string& coverage);
  bool IsForceEarlyDepthStencil() const;
  bool DsvWritingFloat24() const;
  bool DsvApplyingPolygonOffset() const;

  uint32_t FindTextureBinding(uint32_t fetch_constant,
                              xenos::FetchOpDimension dimension,
                              bool is_signed);
  uint32_t FindSamplerBinding(uint32_t fetch_constant,
                              xenos::TextureFilter mag_filter,
                              xenos::TextureFilter min_filter,
                              xenos::TextureFilter mip_filter,
                              xenos::AnisoFilter aniso_filter);

  MslTranslatorConfig config_;
  DxbcShader* dxbc_shader_ = nullptr;

  std::string fail_reason_;
  std::string body_;
  uint32_t indent_ = 1;
  uint32_t temp_counter_ = 0;

  // Inputs and outputs used.
  bool uses_position_ = false;
  bool uses_front_face_ = false;
  bool uses_point_coord_ = false;
  bool uses_sample_mask_out_ = false;
  bool writes_depth_out_ = false;
  bool depth_out_less_equal_ = false;

  // Exec / instruction predication state (as in the DXBC translator).
  static constexpr uint32_t kBoolNone = UINT32_MAX;
  uint32_t cf_exec_bool_constant_ = kBoolNone;
  bool cf_exec_bool_constant_condition_ = false;
  bool cf_exec_predicated_ = false;
  bool cf_exec_predicate_condition_ = false;
  bool cf_instruction_predicate_if_open_ = false;
  bool cf_instruction_predicate_condition_ = false;
  bool cf_exec_predicate_written_ = false;
  // Forward jumps: open `if (!taken)` blocks, by target label (innermost last).
  std::vector<uint32_t> jump_skip_targets_;
  // Crossing forward jumps (retry mode): one flag per jump target, and every
  // exec between a jump and its label runs under `if (!(flags))`.
  struct LoopFrame {
    uint32_t id;
    uint32_t constant;
    uint32_t skip_address;
    uint32_t body_address;
    uint32_t jump_depth;
    std::vector<uint32_t> outer_flags;
    bool outer_guard_open;
  };
  std::vector<LoopFrame> loops_;
  uint32_t loop_counter_ = 0;
  bool jump_flags_mode_ = false;
  std::vector<uint32_t> pending_flag_targets_;
  std::vector<uint32_t> declared_flag_targets_;
  bool flag_guard_open_ = false;
  uint32_t current_cf_index_ = 0;
  std::string FlagGuardExpr() const;
  void OpenFlagGuard();
  void CloseFlagGuard();
  void ProcessControlFlowInstructionBegin(uint32_t cf_index) override {
    current_cf_index_ = cf_index;
  }
  uint32_t open_blocks_ = 0;
};

}  // namespace metal
}  // namespace graphics
}  // namespace rex

#endif  // REX_GRAPHICS_METAL_METAL_MSL_TRANSLATOR_H_
