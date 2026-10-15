/**
 * Fable II recomp: native Xenos -> MSL translator. See metal_msl_translator.h
 * and docs/native-translator.md.
 *
 * Every emitted sequence mirrors the corresponding DxbcShaderTranslator code
 * (src/graphics/pipeline/shader/dxbc_translator*.cpp in the private SDK tree):
 * same operations, same order, same intermediate rounding. Where the DXBC
 * path lowers to a specific AIR intrinsic through DXIL and Metal Shader
 * Converter, the MSL here uses the variant that lowers to the same intrinsic
 * (fast:: for exp2/log2/rsqrt/sqrt/fract/floor/trunc/min/max/saturate, plain
 * fdiv for rcp/div, fma() for DXBC mad, no contraction elsewhere).
 */

#include "rex/graphics/metal/metal_msl_translator.h"

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <algorithm>
#include <array>
#include <functional>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "rex/math.h"

namespace rex {
namespace graphics {
namespace metal {

using namespace ucode;

namespace {

// Exact float literal.
std::string F(float value) {
  if (!std::isfinite(value)) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return fmt::format("as_type<float>(0x{:08X}u)", bits);
  }
  std::string s = fmt::format("{:.9g}", value);
  if (s.find_first_of(".en") == std::string::npos) {
    s += ".0";
  }
  return "(" + s + "f)";
}

std::string U(uint32_t value) { return fmt::format("{}u", value); }

const char kComp[] = {'x', 'y', 'z', 'w'};

std::string Mask(uint32_t mask) {
  std::string s;
  for (uint32_t i = 0; i < 4; ++i) {
    if (mask & (1u << i)) {
      s += kComp[i];
    }
  }
  return s;
}

// bool4 literal selecting the components in mask.
std::string BoolMask(uint32_t mask) {
  return fmt::format("bool4({}, {}, {}, {})", (mask & 1) ? "true" : "false",
                     (mask & 2) ? "true" : "false",
                     (mask & 4) ? "true" : "false",
                     (mask & 8) ? "true" : "false");
}

// Top-level argument buffer slots (MetalCommandProcessor::GraphicsRootABSlot).
constexpr uint32_t kSlotSRVSpace1 = 1;  // 2D array view heap
constexpr uint32_t kSlotSRVSpace2 = 2;  // 3D view heap
constexpr uint32_t kSlotSRVSpace3 = 3;  // cube view heap
constexpr uint32_t kSlotSampler = 9;
constexpr uint32_t kSlotCBVSystem = 10;
constexpr uint32_t kSlotCBVBoolLoop = 12;
constexpr uint32_t kSlotCBVPixelFloat = 21;
constexpr uint32_t kSlotCBVPixelFetch = 22;
constexpr uint32_t kSlotCBVPixelDescriptorIndices = 23;
constexpr uint32_t kSlotSRVSpace0 = 0;  // shared memory (system view table)
constexpr uint32_t kSlotUAVSpace0 = 5;  // shared memory as UAV
constexpr uint32_t kSlotCBVVertexFloat = 11;
constexpr uint32_t kSlotCBVVertexFetch = 13;
constexpr uint32_t kSlotCBVVertexDescriptorIndices = 14;
constexpr uint32_t kSlotCount = 32;

}  // namespace

std::string MslShaderTranslator::Operand::Vec() const {
  std::string s = base + "." + std::string(swizzle, 4);
  if (abs) {
    s = "abs(" + s + ")";
  }
  if (neg) {
    s = "(-" + s + ")";
  }
  return s;
}

std::string MslShaderTranslator::Operand::Comp(uint32_t i) const {
  std::string s = base + "." + swizzle[i & 3];
  if (abs) {
    s = "abs(" + s + ")";
  }
  if (neg) {
    s = "(-" + s + ")";
  }
  return s;
}

MslShaderTranslator::MslShaderTranslator(const MslTranslatorConfig& config)
    : config_(config) {}

MslShaderTranslator::~MslShaderTranslator() = default;

void MslShaderTranslator::Reset() {
  ShaderTranslator::Reset();
  fail_reason_.clear();
  body_.clear();
  indent_ = 1;
  temp_counter_ = 0;
  uses_position_ = false;
  uses_front_face_ = false;
  uses_point_coord_ = false;
  uses_sample_mask_out_ = false;
  writes_depth_out_ = false;
  depth_out_less_equal_ = false;
  cf_exec_bool_constant_ = kBoolNone;
  cf_exec_bool_constant_condition_ = false;
  cf_exec_predicated_ = false;
  cf_exec_predicate_condition_ = false;
  cf_instruction_predicate_if_open_ = false;
  cf_instruction_predicate_condition_ = false;
  cf_exec_predicate_written_ = false;
  jump_skip_targets_.clear();
  pending_flag_targets_.clear();
  loops_.clear();
  loop_counter_ = 0;
  declared_flag_targets_.clear();
  flag_guard_open_ = false;
  current_cf_index_ = 0;
  open_blocks_ = 0;
}

uint32_t MslShaderTranslator::GetModificationRegisterCount() const {
  Modification m = modification();
  return is_vertex_shader() ? m.vertex.dynamic_addressable_register_count
                            : m.pixel.dynamic_addressable_register_count;
}

void MslShaderTranslator::Fail(const std::string& reason) {
  if (fail_reason_.empty()) {
    fail_reason_ = reason;
  }
}

template <typename... Args>
void MslShaderTranslator::Line(const char* format, const Args&... args) {
  body_.append(size_t(indent_) * 2, ' ');
  body_ += fmt::format(fmt::runtime(format), args...);
  body_ += '\n';
}

void MslShaderTranslator::Open(const std::string& header) {
  Line("{} {{", header);
  ++indent_;
  ++open_blocks_;
}

void MslShaderTranslator::Close() {
  if (!open_blocks_) {
    Fail("internal: unbalanced block");
    return;
  }
  --indent_;
  --open_blocks_;
  Line("}}");
}

std::string MslShaderTranslator::NewTemp(const char* prefix) {
  return fmt::format("{}{}", prefix, temp_counter_++);
}

std::string MslShaderTranslator::SysU(size_t offset) const {
  return fmt::format("xe_sys[{}]", offset / sizeof(uint32_t));
}

std::string MslShaderTranslator::SysF(size_t offset) const {
  return fmt::format("as_type<float>(xe_sys[{}])", offset / sizeof(uint32_t));
}

std::string MslShaderTranslator::FetchWord(uint32_t fetch_constant,
                                           uint32_t word) {
  return fmt::format("xe_fetch[{}]", fetch_constant * 6 + word);
}

std::string MslShaderTranslator::BoolConstant(uint32_t index) const {
  return fmt::format("((xe_bool_loop[{}] & {}) != 0u)", index >> 5,
                     U(uint32_t(1) << (index & 31)));
}

MslTranslationResult MslShaderTranslator::Translate(
    DxbcShader& shader, uint64_t modification,
    const std::vector<MslBakedDword>& baked) {
  MslTranslationResult result = Translate(shader, modification);
  if (!result.ok || baked.empty()) {
    return result;
  }
  // Every constant-buffer read in the emitted code is `<buffer>[<literal>]`.
  static const char* kNames[] = {"xe_sys[", "xe_bool_loop[", "xe_fetch["};
  std::unordered_map<uint64_t, const MslBakedDword*> lookup;
  for (const MslBakedDword& b : baked) {
    if (b.buffer <= MslBakedDword::kFetch && b.mask) {
      lookup[(uint64_t(b.buffer) << 32) | b.index] = &b;
    }
  }
  std::unordered_set<const MslBakedDword*> used;
  std::string out;
  out.reserve(result.source.size() + baked.size() * 32);
  const std::string& src = result.source;
  size_t pos = 0;
  while (pos < src.size()) {
    size_t best = std::string::npos;
    uint32_t best_buffer = 0;
    for (uint32_t k = 0; k < 3; ++k) {
      size_t f = src.find(kNames[k], pos);
      if (f < best) {
        best = f;
        best_buffer = k;
      }
    }
    if (best == std::string::npos) {
      out.append(src, pos, std::string::npos);
      break;
    }
    out.append(src, pos, best - pos);
    size_t num_start = best + std::strlen(kNames[best_buffer]);
    size_t num_end = num_start;
    while (num_end < src.size() && src[num_end] >= '0' && src[num_end] <= '9') {
      ++num_end;
    }
    // Must be preceded by a non-identifier character (not e.g. "my_xe_sys[").
    bool word_start = best == 0 || !(std::isalnum((unsigned char)src[best - 1]) ||
                                     src[best - 1] == '_');
    if (!word_start || num_end == num_start || num_end >= src.size() ||
        src[num_end] != ']') {
      out.append(src, best, num_start - best);
      pos = num_start;
      continue;
    }
    uint32_t index = uint32_t(std::stoul(src.substr(num_start, num_end - num_start)));
    std::string access = src.substr(best, num_end + 1 - best);
    auto it = lookup.find((uint64_t(best_buffer) << 32) | index);
    if (it != lookup.end()) {
      used.insert(it->second);
    }
    if (it == lookup.end()) {
      out += access;
    } else if (it->second->mask == 0xFFFFFFFFu) {
      out += fmt::format("{}u", it->second->value);
    } else {
      out += fmt::format("(({} & {}u) | {}u)", access, ~it->second->mask,
                         it->second->value & it->second->mask);
    }
    pos = num_end + 1;
  }
  result.source = std::move(out);
  for (const MslBakedDword& b : baked) {
    if (used.count(&b)) {
      result.baked_used.push_back(b);
    }
  }
  return result;
}

MslTranslationResult MslShaderTranslator::Translate(DxbcShader& shader,
                                                    uint64_t modification) {
  MslTranslationResult result;
  if (shader.type() != xenos::ShaderType::kPixel &&
      shader.type() != xenos::ShaderType::kVertex) {
    result.reason = "unsupported shader type";
    return result;
  }
  if (!shader.is_ucode_analyzed()) {
    result.reason = "ucode not analyzed";
    return result;
  }
  // A private translation object: does not touch the shader's translations.
  struct LocalTranslation : public Shader::Translation {
    LocalTranslation(Shader& s, uint64_t m) : Translation(s, m) {}
  } translation(shader, modification);
  dxbc_shader_ = &shader;
  jump_flags_mode_ = false;
  TranslateAnalyzedShader(translation);
  if (failed() && fail_reason_ == "crossing forward jumps") {
    jump_flags_mode_ = true;
    TranslateAnalyzedShader(translation);
  }
  jump_flags_mode_ = false;
  dxbc_shader_ = nullptr;
  if (failed()) {
    result.reason = fail_reason_;
    return result;
  }
  const std::vector<uint8_t>& bytes = translation.translated_binary();
  result.source.assign(bytes.begin(), bytes.end());
  result.entry_point =
      shader.type() == xenos::ShaderType::kVertex
          ? (Modification(modification)
                         .vertex.host_vertex_shader_type ==
                     Shader::HostVertexShaderType::kQuadDomainPatchIndexed
                 ? (config_.domain_cpu_pattern ? "xe_native_ds_cpu"
                                               : "xe_native_ds")
                 : (config_.points_mesh ? "xe_native_ms_points" :
                    config_.expand_points
                        ? "xe_native_vs_points"
                        : (config_.expand_rects ? "xe_native_vs_rects"
                                                : "xe_native_vs")))
          : "xe_native_ps";
  result.ok = !result.source.empty();
  if (!result.ok) {
    result.reason = "empty output";
  }
  return result;
}

// ---------------------------------------------------------------------------
// Prologue
// ---------------------------------------------------------------------------

void MslShaderTranslator::StartTranslation() {
  const Shader& shader = current_shader();
  Modification m = modification();
  if (shader.uses_register_dynamic_addressing()) {
    Fail("dynamic register addressing");
  }
  if (shader.memexport_eM_written() && !is_vertex_shader()) {
    Fail("memexport in a pixel shader");
  }
  if (is_vertex_shader()) {
    StartVertexShader();
    return;
  }
  // Float24 depth (docs/no-converter.md): converted in the epilogue like
  // CompletePixelShader_DSV_DepthTo24Bit, from the position's depth. The DXBC
  // declares SV_Position with sample interpolation for this; the converter's
  // output nevertheless converts the pixel-centre depth (measured with 4x MSAA
  // in tools/shaderlab/xlate_diff.mm: only edge pixels hold per-sample
  // depths), and so does this code, bit for bit.
  if (DsvWritingFloat24() && !shader.writes_depth()) {
    uses_position_ = true;
  }
  if (DsvApplyingPolygonOffset()) {
    Fail("shader polygon offset");
  }

  // Guest registers: interpolators or zero.
  uint32_t interpolator_mask = m.pixel.interpolator_mask;
  uint32_t param_gen_interpolator =
      (m.pixel.param_gen_enable &&
       m.pixel.param_gen_interpolator < register_count())
          ? m.pixel.param_gen_interpolator
          : UINT32_MAX;
  for (uint32_t i = 0; i < register_count(); ++i) {
    if (i < xenos::kMaxInterpolators && (interpolator_mask & (1u << i)) &&
        i != param_gen_interpolator) {
      Line("float4 r{} = xe_in.v{};", i,
           rex::bit_count(interpolator_mask & ((1u << i) - 1)));
    } else {
      Line("float4 r{} = float4(0.0f);", i);
    }
  }
  if (param_gen_interpolator != UINT32_MAX) {
    const uint32_t rt_scale_x =
        m.pixel.render_target_native_scale ? 1u
                                           : config_.draw_resolution_scale_x;
    const uint32_t rt_scale_y =
        m.pixel.render_target_native_scale ? 1u
                                           : config_.draw_resolution_scale_y;
    uses_position_ = true;
    std::string pg = fmt::format("r{}", param_gen_interpolator);
    Line("{}.xy = fast::floor(xe_in.pos.xy);", pg);
    if (rt_scale_x > 1) {
      Line("{}.x = {}.x * {};", pg, pg, F(1.0f / rt_scale_x));
    }
    if (rt_scale_y > 1) {
      Line("{}.y = {}.y * {};", pg, pg, F(1.0f / rt_scale_y));
    }
    if (m.pixel.param_gen_point) {
      // Point: always front-facing, the point flag in the sign of Y, and the
      // sprite coordinates (saturated) in ZW.
      Line("{}.x = abs({}.x);", pg, pg);
      Line("{}.y = -abs({}.y);", pg, pg);
      Line("{}.zw = fast::saturate(xe_in.point_coord.xy);", pg);
      uses_point_coord_ = true;
    } else {
      Line("{}.xy = abs({}.xy);", pg, pg);
      Open(fmt::format(
          "if ((xe_sys[{}] & {}) != 0u)",
          offsetof(SystemConstants, flags) / 4,
          U(DxbcShaderTranslator::kSysFlag_PrimitivePolygonal)));
      uses_front_face_ = true;
      Line("{}.x = xe_in.front_face ? {}.x : -{}.x;", pg, pg, pg);
      Close();
      Line("{}.z = as_type<float>(extract_bits(xe_sys[{}], {}, 1) << 31);", pg,
           offsetof(SystemConstants, flags) / 4,
           uint32_t(DxbcShaderTranslator::kSysFlag_PrimitiveLine_Shift));
      Line("{}.w = 0.0f;", pg);
    }
  }

  // Internal state (zero-initialized like the DXBC system temps).
  Line("float ps = 0.0f;");
  Line("bool p0 = false;");
  Line("int a0 = 0;");
  Line("int xe_aL = 0;");
  Line("float4 xe_grad_h_lod = float4(0.0f);");
  Line("float4 xe_grad_v = float4(0.0f);");
  uint32_t writes_color = shader.writes_color_targets();
  for (uint32_t i = 0; i < 4; ++i) {
    if (writes_color & (1u << i)) {
      Line("float4 xe_oc{} = float4(0.0f);", i);
    }
  }
  if (shader.writes_depth()) {
    Line("float xe_odepth = 0.0f;");
  }
  // The guest program; `break` ends it (exec end).
  Open("do");
}

// ---------------------------------------------------------------------------
// Control flow
// ---------------------------------------------------------------------------

void MslShaderTranslator::UpdateExecConditionals(
    ParsedExecInstruction::Type type, uint32_t bool_constant_index,
    bool condition) {
  bool merge = false;
  if (type == ParsedExecInstruction::Type::kConditional) {
    merge = cf_exec_bool_constant_ == bool_constant_index &&
            cf_exec_bool_constant_condition_ == condition;
  } else if (type == ParsedExecInstruction::Type::kPredicated) {
    merge = !cf_exec_predicate_written_ && cf_exec_predicated_ &&
            cf_exec_predicate_condition_ == condition;
  } else {
    merge = cf_exec_bool_constant_ == kBoolNone && !cf_exec_predicated_;
  }
  if (merge) {
    return;
  }
  CloseExecConditionals();
  if (type == ParsedExecInstruction::Type::kConditional) {
    Open(fmt::format("if ({}{})", condition ? "" : "!",
                     BoolConstant(bool_constant_index)));
    cf_exec_bool_constant_ = bool_constant_index;
    cf_exec_bool_constant_condition_ = condition;
  } else if (type == ParsedExecInstruction::Type::kPredicated) {
    Open(fmt::format("if ({}p0)", condition ? "" : "!"));
    cf_exec_predicated_ = true;
    cf_exec_predicate_condition_ = condition;
  }
}

void MslShaderTranslator::CloseExecConditionals() {
  CloseInstructionPredication();
  if (cf_exec_bool_constant_ != kBoolNone || cf_exec_predicated_) {
    Close();
    cf_exec_bool_constant_ = kBoolNone;
    cf_exec_predicated_ = false;
  }
  cf_exec_predicate_written_ = false;
}

void MslShaderTranslator::UpdateInstructionPredication(bool predicated,
                                                       bool condition) {
  if (!predicated) {
    CloseInstructionPredication();
    return;
  }
  if (cf_instruction_predicate_if_open_) {
    if (cf_instruction_predicate_condition_ == condition) {
      return;
    }
    CloseInstructionPredication();
  }
  if (!cf_exec_predicate_written_ && cf_exec_predicated_ &&
      cf_exec_predicate_condition_ == condition) {
    return;
  }
  Open(fmt::format("if ({}p0)", condition ? "" : "!"));
  cf_instruction_predicate_if_open_ = true;
  cf_instruction_predicate_condition_ = condition;
}

void MslShaderTranslator::CloseInstructionPredication() {
  if (cf_instruction_predicate_if_open_) {
    Close();
    cf_instruction_predicate_if_open_ = false;
  }
}

std::string MslShaderTranslator::FlagGuardExpr() const {
  std::string e;
  for (uint32_t t : pending_flag_targets_) {
    e += (e.empty() ? "" : " || ") + fmt::format("xe_j{}", t);
  }
  return e;
}

void MslShaderTranslator::OpenFlagGuard() {
  if (!pending_flag_targets_.empty()) {
    Open(fmt::format("if (!({}))", FlagGuardExpr()));
    flag_guard_open_ = true;
  }
}

void MslShaderTranslator::CloseFlagGuard() {
  CloseExecConditionals();
  if (flag_guard_open_) {
    Close();
    flag_guard_open_ = false;
  }
}

void MslShaderTranslator::ProcessLabel(uint32_t cf_index) {
  if (cf_index == 0) {
    return;
  }
  CloseExecConditionals();
  if (jump_flags_mode_) {
    CloseFlagGuard();
    pending_flag_targets_.erase(
        std::remove(pending_flag_targets_.begin(), pending_flag_targets_.end(),
                    cf_index),
        pending_flag_targets_.end());
    OpenFlagGuard();
    return;
  }
  // Close the forward-jump blocks that end here. A label that is the target
  // of a block which is not innermost means the jumps cross (retried in flag
  // mode).
  while (!jump_skip_targets_.empty() &&
         jump_skip_targets_.back() == cf_index) {
    Close();
    jump_skip_targets_.pop_back();
  }
  for (uint32_t target : jump_skip_targets_) {
    if (target == cf_index) {
      Fail("crossing forward jumps");
    }
  }
}

void MslShaderTranslator::ProcessExecInstructionBegin(
    const ParsedExecInstruction& instr) {
  UpdateExecConditionals(instr.type, instr.bool_constant_index,
                         instr.condition);
}

void MslShaderTranslator::ProcessExecInstructionEnd(
    const ParsedExecInstruction& instr) {
  if (instr.is_end) {
    if (!loops_.empty()) {
      Fail("exec end inside a loop");
      return;
    }
    CloseInstructionPredication();
    Line("break;");
  }
}

void MslShaderTranslator::ProcessLoopStartInstruction(
    const ParsedLoopStartInstruction& instr) {
  // Mirrors the DXBC translator: count in bits 0:7 of the loop constant
  // (bool/loop buffer dwords 8+), initial aL in 8:15 unless repeating, aL
  // step (signed) in 16:23; a zero count skips the loop. Structured as a
  // while loop over the body, which must be the CF range up to its endloop.
  CloseExecConditionals();
  // Flag mode: pending jumps must end after the loop. The loop runs under
  // the current guard and gets its own flag scope (labels inside the loop
  // never close the outer guard).
  std::vector<uint32_t> outer_flags;
  bool outer_guard_open = false;
  if (jump_flags_mode_) {
    for (uint32_t t : pending_flag_targets_) {
      if (t < instr.loop_skip_address) {
        Fail("forward jump into a loop");
        return;
      }
    }
    CloseFlagGuard();
    OpenFlagGuard();
    outer_flags = pending_flag_targets_;
    outer_guard_open = flag_guard_open_;
    pending_flag_targets_.clear();
    flag_guard_open_ = false;
  }
  // A loop nested inside forward-jump ranges is fine when those ranges end
  // after it (structured nesting).
  for (uint32_t t : jump_skip_targets_) {
    if (t < instr.loop_skip_address) {
      Fail("forward jump into a loop");
      return;
    }
  }
  if (instr.loop_skip_address <= current_cf_index_) {
    Fail("loop skip address");
    return;
  }
  LoopFrame f;
  f.id = uint32_t(loops_.size() + loop_counter_++);
  f.constant = instr.loop_constant_index;
  f.skip_address = instr.loop_skip_address;
  f.body_address = current_cf_index_ + 1;
  f.jump_depth = uint32_t(jump_skip_targets_.size());
  f.outer_flags = std::move(outer_flags);
  f.outer_guard_open = outer_guard_open;
  std::string lc = fmt::format("xe_bool_loop[{}]", 8 + instr.loop_constant_index);
  Line("uint xe_lc{} = {} & 255u;", f.id, lc);
  Line("int xe_aL_saved{} = xe_aL;", f.id);
  Open(fmt::format("if (xe_lc{} != 0u)", f.id));
  if (!instr.is_repeat) {
    Line("xe_aL = int(extract_bits({}, 8, 8));", lc);
  }
  Open("while (true)");
  loops_.push_back(f);
}

void MslShaderTranslator::ProcessLoopEndInstruction(
    const ParsedLoopEndInstruction& instr) {
  CloseExecConditionals();
  if (loops_.empty() || loops_.back().constant != instr.loop_constant_index ||
      loops_.back().body_address != instr.loop_body_address ||
      loops_.back().skip_address != current_cf_index_ + 1) {
    Fail("unstructured loop");
    return;
  }
  if (jump_flags_mode_ ? !pending_flag_targets_.empty()
                       : jump_skip_targets_.size() != loops_.back().jump_depth) {
    Fail("forward jump out of a loop");
    return;
  }
  if (jump_flags_mode_ && flag_guard_open_) {
    Close();
    flag_guard_open_ = false;
  }
  LoopFrame f = loops_.back();
  loops_.pop_back();
  Line("xe_lc{} = xe_lc{} - 1u;", f.id, f.id);
  if (instr.is_predicated_break) {
    Line("if (xe_lc{} == 0u || {}p0) {{ break; }}", f.id,
         instr.predicate_condition ? "" : "!");
  } else {
    Line("if (xe_lc{} == 0u) {{ break; }}", f.id);
  }
  Line("xe_aL = xe_aL + extract_bits(as_type<int>(xe_bool_loop[{}]), 16, 8);",
       8 + instr.loop_constant_index);
  Close();  // while
  Close();  // if count
  Line("xe_aL = xe_aL_saved{};", f.id);
  if (jump_flags_mode_) {
    pending_flag_targets_ = f.outer_flags;
    flag_guard_open_ = f.outer_guard_open;
  }
}

void MslShaderTranslator::ProcessCallInstruction(const ParsedCallInstruction&) {
  Fail("call");
}
void MslShaderTranslator::ProcessReturnInstruction(
    const ParsedReturnInstruction&) {
  Fail("return");
}

void MslShaderTranslator::ProcessJumpInstruction(
    const ParsedJumpInstruction& instr) {
  // Only forward jumps; the skipped range becomes `if (!taken) { ... }` (or a
  // flag guard in flag mode).
  if (instr.target_address <= current_cf_index_) {
    Fail("backward jump");
    return;
  }
  CloseExecConditionals();
  std::string taken;
  switch (instr.type) {
    case ParsedJumpInstruction::Type::kUnconditional:
      taken = "true";
      break;
    case ParsedJumpInstruction::Type::kConditional:
      taken = fmt::format("{}{}", instr.condition ? "" : "!",
                          BoolConstant(instr.bool_constant_index));
      break;
    case ParsedJumpInstruction::Type::kPredicated:
      taken = fmt::format("{}p0", instr.condition ? "" : "!");
      break;
  }
  if (jump_flags_mode_) {
    // Evaluated where the jump is (inside the current guard).
    const uint32_t t = instr.target_address;
    if (std::find(declared_flag_targets_.begin(), declared_flag_targets_.end(),
                  t) == declared_flag_targets_.end()) {
      declared_flag_targets_.push_back(t);
    }
    Line("if ({}) {{ xe_j{} = true; }}", taken, t);
    CloseFlagGuard();
    if (std::find(pending_flag_targets_.begin(), pending_flag_targets_.end(),
                  t) == pending_flag_targets_.end()) {
      pending_flag_targets_.push_back(t);
    }
    OpenFlagGuard();
    return;
  }
  if (!jump_skip_targets_.empty() &&
      instr.target_address > jump_skip_targets_.back()) {
    Fail("crossing forward jumps");
  }
  jump_skip_targets_.push_back(instr.target_address);
  Open(fmt::format("if (!({}))", taken));
}

void MslShaderTranslator::ProcessAllocInstruction(
    const ParsedAllocInstruction& instr, uint8_t export_eM) {
  // DxbcShaderTranslator::ProcessAllocInstruction.
  bool start_memexport = instr.type == ucode::AllocType::kMemory &&
                         current_shader().memexport_eM_written();
  if (export_eM || start_memexport) {
    CloseExecConditionals();
  }
  if (export_eM) {
    ExportToMemory(export_eM);
    Line("xe_me_written = 0u;");
    for (uint32_t i = 0; i < 5; ++i) {
      if (export_eM & (1u << i)) {
        Line("xe_eM{} = float4(0.0f);", i);
      }
    }
  }
  if (start_memexport) {
    Line("xe_eA = float4(0.0f);");
  }
}

void MslShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  // Mirrors DxbcShaderTranslator::ProcessVertexFetchInstruction.
  if (!is_vertex_shader()) {
    Fail("vertex fetch in a pixel shader");
    return;
  }
  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);
  uint32_t used = instr.result.GetUsedResultComponents();
  uint32_t needed_words =
      xenos::GetVertexFormatNeededWords(instr.attributes.data_format, used);
  if (!needed_words && instr.is_mini_fetch) {
    StoreResult(instr.result, "float4(0.0f)");
    return;
  }
  const uint32_t fc = instr.operands[1].storage_index;
  std::string fc0 = fmt::format("xe_fetch[{}]", fc * 2);
  std::string fc1 = fmt::format("xe_fetch[{}]", fc * 2 + 1);
  if (!instr.is_mini_fetch) {
    if (instr.attributes.stride) {
      Operand index = LoadOperand(instr.operands[0]);
      std::string idx;
      if (instr.attributes.is_index_rounded) {
        idx = fmt::format("fast::floor({} + 0.5f)", index.Comp(0));
      } else {
        idx = fmt::format("fast::floor({})", index.Comp(0));
      }
      Line("xe_vfetch_address = int({}) * {} + int({} & {});", idx,
           int32_t(instr.attributes.stride * sizeof(uint32_t)), fc0,
           U(~uint32_t(3)));
    } else {
      Line("xe_vfetch_address = int({} & {});", fc0, U(~uint32_t(3)));
    }
  }
  if (!needed_words) {
    StoreResult(instr.result, "float4(0.0f)");
    return;
  }
  // Load the needed words (bounds-checked against the buffer size, as the
  // converter's code does).
  std::string w = NewTemp("vw");
  Line("uint4 {} = uint4(0u);", w);
  uint32_t first_word;
  rex::bit_scan_forward(needed_words, &first_word);
  int32_t base_words = instr.attributes.offset;
  Open(fmt::format("if ((xe_sys[{}] & {}) == 0u)",
                   offsetof(SystemConstants, flags) / 4,
                   U(DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV)));
  for (uint32_t i = 0; i < 4; ++i) {
    if (needed_words & (1u << i)) {
      Line("{}.{} = xe_shared_load(xe_tl.srv0, uint(xe_vfetch_address + {}));",
           w, kComp[i], (base_words + int32_t(i)) * 4);
    }
  }
  Close();
  Open("else");
  for (uint32_t i = 0; i < 4; ++i) {
    if (needed_words & (1u << i)) {
      Line("{}.{} = xe_shared_load(xe_tl.uav0, uint(xe_vfetch_address + {}));",
           w, kComp[i], (base_words + int32_t(i)) * 4);
    }
  }
  Close();
  // Endian swap.
  std::string e = NewTemp("ve");
  std::string wm = Mask(needed_words);
  Line("uint {} = {} & 3u;", e, fc1);
  Open(fmt::format("if ({} == {}u || {} == {}u)", e,
                   uint32_t(xenos::Endian128::k8in16), e,
                   uint32_t(xenos::Endian128::k8in32)));
  Line("{}.{} = (({}.{} & 0x00FF00FFu) * 256u) + (({}.{} >> 8u) & 0x00FF00FFu);",
       w, wm, w, wm, w, wm);
  Close();
  Open(fmt::format("if ({} == {}u || {} == {}u)", e,
                   uint32_t(xenos::Endian128::k8in32), e,
                   uint32_t(xenos::Endian128::k16in32)));
  Line("{}.{} = ({}.{} >> 16u) | ({}.{} << 16u);", w, wm, w, wm, w, wm);
  Close();
  // Unpack.
  uint32_t format_count = uint32_t(
      xenos::GetVertexFormatComponentCount(instr.attributes.data_format));
  uint32_t used_format = used & ((1u << format_count) - 1);
  std::string r = NewTemp("vr");
  Line("float4 {} = float4(0.0f);", r);
  uint32_t widths[4] = {}, offsets[4] = {};
  const char* word_of = "xxxx";
  using VF = xenos::VertexFormat;
  switch (instr.attributes.data_format) {
    case VF::k_8_8_8_8:
      widths[0] = widths[1] = widths[2] = widths[3] = 8;
      offsets[1] = 8; offsets[2] = 16; offsets[3] = 24;
      break;
    case VF::k_2_10_10_10:
      widths[0] = widths[1] = widths[2] = 10; widths[3] = 2;
      offsets[1] = 10; offsets[2] = 20; offsets[3] = 30;
      break;
    case VF::k_10_11_11:
      widths[0] = widths[1] = 11; widths[2] = 10;
      offsets[1] = 11; offsets[2] = 22;
      break;
    case VF::k_11_11_10:
      widths[0] = 10; widths[1] = widths[2] = 11;
      offsets[1] = 10; offsets[2] = 21;
      break;
    case VF::k_16_16:
      widths[0] = widths[1] = 16;
      offsets[1] = 16;
      break;
    case VF::k_16_16_16_16:
      widths[0] = widths[1] = widths[2] = widths[3] = 16;
      offsets[1] = offsets[3] = 16;
      word_of = "xxyy";
      break;
    default:
      break;
  }
  if (widths[0]) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(used_format & (1u << i))) continue;
      if (instr.attributes.is_signed) {
        Line("{}.{} = float(extract_bits(as_type<int>({}.{}), {}, {}));", r,
             kComp[i], w, word_of[i], offsets[i], widths[i]);
      } else {
        Line("{}.{} = float(extract_bits({}.{}, {}, {}));", r, kComp[i], w,
             word_of[i], offsets[i], widths[i]);
      }
    }
    if (!instr.attributes.is_integer) {
      if (instr.attributes.is_signed) {
        if (instr.attributes.signed_rf_mode ==
            xenos::SignedRepeatingFractionMode::kZeroClampMinusOne) {
          for (uint32_t i = 0; i < 4; ++i) {
            if ((used_format & (1u << i)) && widths[i] > 2) {
              Line("{}.{} = {}.{} * {};", r, kComp[i], r, kComp[i],
                   F(1.0f / float((uint32_t(1) << (widths[i] - 1)) - 1)));
            }
          }
          for (uint32_t i = 0; i < 4; ++i) {
            if (used_format & (1u << i)) {
              Line("{}.{} = fast::max({}.{}, -1.0f);", r, kComp[i], r,
                   kComp[i]);
            }
          }
        } else {
          for (uint32_t i = 0; i < 4; ++i) {
            if (!(used_format & (1u << i))) continue;
            float zero = 1.0f / float((uint32_t(1) << widths[i]) - 1);
            Line("{}.{} = fma({}.{}, {}, {});", r, kComp[i], r, kComp[i],
                 F(2.0f * zero), F(zero));
          }
        }
      } else {
        for (uint32_t i = 0; i < 4; ++i) {
          if ((used_format & (1u << i)) && widths[i] > 1) {
            Line("{}.{} = {}.{} * {};", r, kComp[i], r, kComp[i],
                 F(1.0f / float((uint32_t(1) << widths[i]) - 1)));
          }
        }
      }
    }
  } else {
    switch (instr.attributes.data_format) {
      case VF::k_16_16_FLOAT:
      case VF::k_16_16_16_16_FLOAT: {
        const char* wo = "xxyy";
        for (uint32_t i = 0; i < 4; ++i) {
          if (!(used_format & (1u << i))) continue;
          Line("{}.{} = float(as_type<half>(ushort(extract_bits({}.{}, {}, 16))));",
               r, kComp[i], w, wo[i], (i & 1) ? 16 : 0);
        }
      } break;
      case VF::k_32:
      case VF::k_32_32:
      case VF::k_32_32_32_32:
        for (uint32_t i = 0; i < 4; ++i) {
          if (!(used_format & (1u << i))) continue;
          if (instr.attributes.is_signed) {
            Line("{}.{} = float(as_type<int>({}.{}));", r, kComp[i], w,
                 kComp[i]);
          } else {
            Line("{}.{} = float({}.{});", r, kComp[i], w, kComp[i]);
          }
          if (!instr.attributes.is_integer) {
            if (instr.attributes.is_signed) {
              if (instr.attributes.signed_rf_mode ==
                  xenos::SignedRepeatingFractionMode::kZeroClampMinusOne) {
                Line("{}.{} = {}.{} * {};", r, kComp[i], r, kComp[i],
                     F(1.0f / 2147483647.0f));
              } else {
                Line("{}.{} = fma({}.{}, {}, {});", r, kComp[i], r, kComp[i],
                     F(1.0f / 2147483647.5f), F(0.5f / 2147483647.5f));
              }
            } else {
              Line("{}.{} = {}.{} * {};", r, kComp[i], r, kComp[i],
                   F(1.0f / 4294967295.0f));
            }
          }
        }
        break;
      case VF::k_32_FLOAT:
      case VF::k_32_32_FLOAT:
      case VF::k_32_32_32_32_FLOAT:
      case VF::k_32_32_32_FLOAT:
        for (uint32_t i = 0; i < 4; ++i) {
          if (used_format & (1u << i)) {
            Line("{}.{} = as_type<float>({}.{});", r, kComp[i], w, kComp[i]);
          }
        }
        break;
      default:
        Fail("unknown vertex format");
        return;
    }
  }
  if (instr.attributes.exp_adjust) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (used_format & (1u << i)) {
        Line("{}.{} = {}.{} * {};", r, kComp[i], r, kComp[i],
             F(std::ldexp(1.0f, instr.attributes.exp_adjust)));
      }
    }
  }
  StoreResult(instr.result, r);
}

void MslShaderTranslator::ExportToMemory(uint8_t export_eM) {
  // Mirrors DxbcShaderTranslator::ExportToMemory (dxbc_translator_memexport).
  if (!export_eM) {
    return;
  }
  auto each = [&](const std::function<void(uint32_t)>& f) {
    for (uint32_t i = 0; i < 5; ++i) {
      if (export_eM & (1u << i)) f(i);
    }
  };
  Open("if (xe_me_en != 0u)");
  Line("uint4 xe_ma = as_type<uint4>(xe_eA);");
  Open("if ((xe_ma.x >> 30u) == 1u && (xe_ma.y >> 23u) == 0x96u && "
       "(xe_ma.z >> 23u) == 0x96u && (xe_ma.w >> 23u) == 0x96u)");
  // Working copies.
  each([&](uint32_t i) { Line("float4 xe_mf{} = xe_eM{};", i, i); });
  Open("if (extract_bits(as_type<int>(xe_ma.z), 19, 1) != 0)");
  each([&](uint32_t i) { Line("xe_mf{}.xz = xe_mf{}.zx;", i, i); });
  Close();
  each([&](uint32_t i) { Line("uint4 xe_mu{} = as_type<uint4>(xe_mf{});", i, i); });
  Line("uint xe_mfmt = extract_bits(xe_ma.z, 8, 6);");
  Line("bool xe_msigned = extract_bits(xe_ma.z, 16, 1) != 0u;");
  Line("bool xe_minteger = extract_bits(xe_ma.z, 17, 1) != 0u;");
  Line("uint xe_msize = 0xFFFFFFFFu;");
  auto flush_nan = [&](const std::string& m) {
    each([&](uint32_t i) {
      Line("xe_mf{}.{} = select(xe_mf{}.{}, 0.0f, xe_mf{}.{} != xe_mf{}.{});", i, m, i,
           m, i, m, i, m);
    });
  };
  auto pack = [&](std::array<uint32_t, 4> widths, uint32_t size_log2) {
    uint32_t comps = 0;
    std::array<uint32_t, 4> offsets = {};
    for (uint32_t i = 0; i < 4; ++i) {
      if (widths[i]) comps |= 1u << i;
      if (i) offsets[i] = offsets[i - 1] + widths[i - 1];
    }
    std::string m = Mask(comps);
    flush_nan(m);
    Open("if (xe_msigned)");
    Open("if (xe_minteger)");
    each([&](uint32_t e) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (!widths[i]) continue;
        float mx = float((uint32_t(1) << (widths[i] - 1)) - 1);
        Line("xe_mf{}.{} = fast::min({}, fast::max({}, xe_mf{}.{}));", e, kComp[i],
             F(mx), F(-1.0f - mx), e, kComp[i]);
      }
    });
    Close();
    Open("else");
    each([&](uint32_t e) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (!widths[i]) continue;
        Line("xe_mf{}.{} = fast::min(1.0f, fast::max(-1.0f, xe_mf{}.{}));", e,
             kComp[i], e, kComp[i]);
        if (widths[i] > 2) {
          Line("xe_mf{}.{} = xe_mf{}.{} * {};", e, kComp[i], e, kComp[i],
               F(float((uint32_t(1) << (widths[i] - 1)) - 1)));
        }
      }
    });
    Close();
    each([&](uint32_t e) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (!widths[i]) continue;
        Line("xe_mu{}.{} = as_type<uint>(int(xe_mf{}.{} + as_type<float>((as_type<uint>("
             "xe_mf{}.{}) & 0x80000000u) | 0x3F000000u)));",
             e, kComp[i], e, kComp[i], e, kComp[i]);
      }
    });
    Close();
    Open("else");
    Open("if (xe_minteger)");
    each([&](uint32_t e) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (!widths[i]) continue;
        Line("xe_mf{}.{} = fast::min({}, fast::max(0.0f, xe_mf{}.{}));", e, kComp[i],
             F(float((uint32_t(1) << widths[i]) - 1)), e, kComp[i]);
      }
    });
    Close();
    Open("else");
    each([&](uint32_t e) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (!widths[i]) continue;
        Line("xe_mf{}.{} = fast::saturate(xe_mf{}.{});", e, kComp[i], e, kComp[i]);
        if (widths[i] > 1) {
          Line("xe_mf{}.{} = xe_mf{}.{} * {};", e, kComp[i], e, kComp[i],
               F(float((uint32_t(1) << widths[i]) - 1)));
        }
      }
    });
    Close();
    each([&](uint32_t e) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (!widths[i]) continue;
        Line("xe_mu{}.{} = uint(xe_mf{}.{} + 0.5f);", e, kComp[i], e, kComp[i]);
      }
    });
    Close();
    each([&](uint32_t e) {
      for (uint32_t i = 1; i < 4; ++i) {
        if (!widths[i]) continue;
        Line("xe_mu{}.x = insert_bits(xe_mu{}.x, xe_mu{}.{}, {}, {});", e, e, e,
             kComp[i], offsets[i], widths[i]);
      }
    });
    Line("xe_msize = {}u;", size_log2);
  };
  using CF = xenos::ColorFormat;
  auto case_of = [&](std::initializer_list<CF> fs) {
    std::string c;
    for (CF f : fs) {
      c += (c.empty() ? "" : " || ") + fmt::format("xe_mfmt == {}u", uint32_t(f));
    }
    return c;
  };
  bool first = true;
  auto branch = [&](std::initializer_list<CF> fs) {
    Open(fmt::format("{}if ({})", first ? "" : "else ", case_of(fs)));
    first = false;
  };
  branch({CF::k_8, CF::k_8_A, CF::k_8_B});
  pack({8, 0, 0, 0}, 0);
  Close();
  branch({CF::k_1_5_5_5});
  pack({5, 5, 5, 1}, 1);
  Close();
  branch({CF::k_5_6_5});
  pack({5, 6, 5, 0}, 1);
  Close();
  branch({CF::k_6_5_5});
  pack({5, 5, 6, 0}, 1);
  Close();
  branch({CF::k_8_8_8_8, CF::k_8_8_8_8_A, CF::k_8_8_8_8_AS_16_16_16_16});
  pack({8, 8, 8, 8}, 2);
  Close();
  branch({CF::k_2_10_10_10, CF::k_2_10_10_10_AS_16_16_16_16});
  pack({10, 10, 10, 2}, 2);
  Close();
  branch({CF::k_8_8});
  pack({8, 8, 0, 0}, 1);
  Close();
  branch({CF::k_4_4_4_4});
  pack({4, 4, 4, 4}, 1);
  Close();
  branch({CF::k_10_11_11, CF::k_10_11_11_AS_16_16_16_16});
  pack({11, 11, 10, 0}, 2);
  Close();
  branch({CF::k_11_11_10, CF::k_11_11_10_AS_16_16_16_16});
  pack({10, 11, 11, 0}, 2);
  Close();
  branch({CF::k_16});
  pack({16, 0, 0, 0}, 1);
  Close();
  branch({CF::k_16_16});
  pack({16, 16, 0, 0}, 2);
  Close();
  branch({CF::k_16_16_16_16});
  {
    flush_nan("xyzw");
    Open("if (xe_msigned)");
    Open("if (xe_minteger)");
    each([&](uint32_t e) {
      Line("xe_mf{} = fast::min(float4({}), fast::max(float4({}), xe_mf{}));", e,
           F(float(INT16_MAX)), F(float(INT16_MIN)), e);
    });
    Close();
    Open("else");
    each([&](uint32_t e) {
      Line("xe_mf{} = fast::min(float4(1.0f), fast::max(float4(-1.0f), xe_mf{}));", e, e);
      Line("xe_mf{} = xe_mf{} * {};", e, e, F(float(INT16_MAX)));
    });
    Close();
    each([&](uint32_t e) {
      Line("xe_mu{} = as_type<uint4>(int4(xe_mf{} + as_type<float4>((as_type<uint4>("
           "xe_mf{}) & 0x80000000u) | 0x3F000000u)));",
           e, e, e);
    });
    Close();
    Open("else");
    Open("if (xe_minteger)");
    each([&](uint32_t e) {
      Line("xe_mf{} = fast::min(float4({}), fast::max(float4(0.0f), xe_mf{}));", e,
           F(float(UINT16_MAX)), e);
    });
    Close();
    Open("else");
    each([&](uint32_t e) {
      Line("xe_mf{} = fast::saturate(xe_mf{});", e, e);
      Line("xe_mf{} = xe_mf{} * {};", e, e, F(float(UINT16_MAX)));
    });
    Close();
    each([&](uint32_t e) { Line("xe_mu{} = uint4(xe_mf{} + 0.5f);", e, e); });
    Close();
    each([&](uint32_t e) {
      Line("xe_mu{}.xy = insert_bits(xe_mu{}.xz, xe_mu{}.yw, 16, 16);", e, e, e);
    });
    Line("xe_msize = 3u;");
  }
  Close();
  auto f16_ext = [&](const std::string& m) {
    each([&](uint32_t e) {
      Line("{{ float4 o = xe_mf{};", e);
      Line("  uint4 h = xe_f32_to_f16(xe_mf{});", e);
      Line("  bool4 ovf = (h & 0x7C00u) == 0x7C00u;");
      Line("  o = fast::max(fast::min(o, float4(131008.0f)), float4(-131008.0f)) * 0.5f;");
      Line("  uint4 hx = xe_f32_to_f16(o) + 0x0400u;");
      Line("  xe_mu{}.{} = select(h, hx, ovf).{}; }}", e, m, m);
    });
  };
  branch({CF::k_16_FLOAT});
  f16_ext("x");
  Line("xe_msize = 1u;");
  Close();
  branch({CF::k_16_16_FLOAT});
  f16_ext("xy");
  each([&](uint32_t e) {
    Line("xe_mu{}.x = insert_bits(xe_mu{}.x, xe_mu{}.y, 16, 16);", e, e, e);
  });
  Line("xe_msize = 2u;");
  Close();
  branch({CF::k_16_16_16_16_FLOAT});
  f16_ext("xyzw");
  each([&](uint32_t e) {
    Line("xe_mu{}.xy = insert_bits(xe_mu{}.xz, xe_mu{}.yw, 16, 16);", e, e, e);
  });
  Line("xe_msize = 3u;");
  Close();
  branch({CF::k_32_FLOAT});
  Line("xe_msize = 2u;");
  Close();
  branch({CF::k_32_32_FLOAT});
  Line("xe_msize = 3u;");
  Close();
  branch({CF::k_32_32_32_32_FLOAT});
  Line("xe_msize = 4u;");
  Close();
  // Endian swap.
  Line("uint xe_mend = extract_bits(xe_ma.z, 0, 3);");
  Open(fmt::format("if (xe_mend == {}u)", uint32_t(xenos::Endian128::k8in64)));
  each([&](uint32_t e) { Line("xe_mu{} = xe_mu{}.yxwz;", e, e); });
  Line("xe_mend = {}u;", uint32_t(xenos::Endian128::k8in32));
  Close();
  Open(fmt::format("if (xe_mend == {}u)", uint32_t(xenos::Endian128::k8in128)));
  each([&](uint32_t e) { Line("xe_mu{} = xe_mu{}.wzyx;", e, e); });
  Line("xe_mend = {}u;", uint32_t(xenos::Endian128::k8in32));
  Close();
  Open(fmt::format("if (xe_mend == {}u || xe_mend == {}u)",
                   uint32_t(xenos::Endian128::k8in16),
                   uint32_t(xenos::Endian128::k8in32)));
  each([&](uint32_t e) {
    Line("xe_mu{} = ((xe_mu{} & 0x00FF00FFu) * 256u) + ((xe_mu{} >> 8u) & 0x00FF00FFu);",
         e, e, e);
  });
  Close();
  Open(fmt::format("if (xe_mend == {}u || xe_mend == {}u)",
                   uint32_t(xenos::Endian128::k8in32),
                   uint32_t(xenos::Endian128::k16in32)));
  each([&](uint32_t e) {
    Line("xe_mu{} = (xe_mu{} >> 16u) | (xe_mu{} << 16u);", e, e, e);
  });
  Close();
  // Addresses.
  Line("int xe_mbase = int(extract_bits(xe_ma.y, 0, 23));");
  Line("int xe_mcount = int(extract_bits(xe_ma.w, 0, 23));");
  Open("if (xe_mbase < xe_mcount && xe_msize <= 4u)");
  Line("uint xe_mbytes = xe_ma.x << 2u;");
  each([&](uint32_t e) {
    if (e == 0) {
      Line("bool xe_mst0 = (xe_me_written & 1u) != 0u;");
    } else {
      Line("bool xe_mst{} = (xe_mbase + {} < xe_mcount) && ((xe_me_written & {}u) != 0u);",
           e, e, 1u << e);
    }
    Line("uint xe_maddr{} = xe_mbytes + (uint(xe_mbase + {}) << xe_msize);", e, e);
    Open(fmt::format("if (xe_mst{})", e));
    Open("if (xe_msize <= 1u)");
    Line("uint w = 8u << xe_msize;");
    Line("uint sh = (xe_maddr{} & 3u) << 3u;", e);
    Line("xe_shared_and(xe_tl.uav0, xe_maddr{} & ~3u, ~(((w == 32u) ? 0xFFFFFFFFu : "
         "((1u << w) - 1u)) << sh));", e);
    Line("xe_shared_or(xe_tl.uav0, xe_maddr{} & ~3u, (xe_mu{}.x & ((1u << w) - 1u)) << sh);",
         e, e);
    Close();
    Open("else");
    Line("uint n = 1u << (xe_msize - 2u);");
    Line("for (uint k = 0u; k < n; ++k) {{ xe_shared_store(xe_tl.uav0, xe_maddr{} + k * "
         "4u, xe_mu{}[k]); }}",
         e, e);
    Close();
    Close();
  });
  Close();
  Close();
  Close();
}

void MslShaderTranslator::StartVertexShader() {
  Modification m = modification();
  // Domain shaders: the quad domain with patch indexing only, the only kind
  // Fable II draws (docs/native-translator.md, "Geometry and tessellation
  // emulation"). The entry supplies xe_patch_index and xe_uv.
  const bool domain_quad = m.vertex.host_vertex_shader_type ==
                           Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
  if (m.vertex.host_vertex_shader_type !=
          Shader::HostVertexShaderType::kVertex &&
      !domain_quad) {
    Fail("host vertex shader type (domain / emulated primitive)");
  }
  if (m.vertex.output_point_size && !config_.expand_points &&
      !config_.points_mesh) {
    Fail("point size output");
  }
  // Memory export is allowed: the mesh function runs the body once per
  // point (threads past the draw's point count skip it), as the emulation's
  // object stage does.
  if (config_.points_mesh && m.vertex.user_clip_plane_count) {
    Fail("mesh point expansion with clip planes");
  }
  if ((config_.expand_points || config_.expand_rects || config_.points_mesh) &&
      domain_quad) {
    Fail("point/rectangle expansion of a domain shader");
  }
  if (config_.expand_rects &&
      (m.vertex.output_point_size || current_shader().memexport_eM_written())) {
    // The body runs three times per vertex: no side effects allowed.
    Fail("rectangle expansion with point size or memory export");
  }
  if (m.vertex.vertex_kill_and || m.vertex.user_clip_plane_cull) {
    Fail("cull distances");
  }
  for (uint32_t i = 0; i < register_count(); ++i) {
    Line("float4 r{} = float4(0.0f);", i);
  }
  uint32_t count = rex::bit_count(m.vertex.interpolator_mask);
  for (uint32_t i = 0; i < count; ++i) {
    Line("float4 xe_o{} = float4(0.0f);", i);
  }
  Line("float4 xe_pos = float4(0.0f);");
  Line("float4 xe_pse = float4(-1.0f, 0.0f, 0.0f, 0.0f);");
  Line("int xe_vfetch_address = 0;");
  if (uint8_t eM = current_shader().memexport_eM_written()) {
    // Memory export allowed: the shared memory is bound as a UAV.
    Line("uint xe_me_en = ((xe_sys[{}] & {}) != 0u) ? 0xFFFFFFFFu : 0u;",
         offsetof(SystemConstants, flags) / 4,
         U(DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV));
    Line("uint xe_me_written = 0u;");
    Line("float4 xe_eA = float4(0.0f);");
    for (uint32_t i = 0; i < 5; ++i) {
      if (eM & (1u << i)) {
        Line("float4 xe_eM{} = float4(0.0f);", i);
      }
    }
  }
  if (domain_quad) {
    // DxbcShaderTranslator::StartDomainShader, kQuadDomainPatchIndexed:
    // r0.x = patch index (from the hull shader), r0.yz = domain location,
    // r1.x = 0 (identity UV swizzle).
    if (register_count() >= 1) {
      Line("r0.x = xe_patch_index;");
      Line("r0.y = xe_uv.x;");
      Line("r0.z = xe_uv.y;");
    }
    if (register_count() >= 2) {
      Line("r1.x = 0.0f;");
    }
  } else if (register_count() >= 1) {
    // DxbcShaderTranslator::StartVertexShader_LoadVertexIndex.
    Line("uint xe_vi = xe_vertex_id - xe_base_vertex;");
    Line("uint xe_idx = xe_vi & ((xe_vi != {}) ? 0xFFFFFFFFu : 0u);",
         SysU(offsetof(SystemConstants, line_loop_closing_index)));
    std::string endian = SysU(offsetof(SystemConstants, vertex_index_endian));
    Open(fmt::format("if ({} == {}u || {} == {}u)", endian,
                     uint32_t(xenos::Endian::k8in16), endian,
                     uint32_t(xenos::Endian::k8in32)));
    Line("xe_idx = ((xe_idx & 0x00FF00FFu) * 256u) + ((xe_idx >> 8u) & "
         "0x00FF00FFu);");
    Close();
    Open(fmt::format("if ({} == {}u || {} == {}u)", endian,
                     uint32_t(xenos::Endian::k8in32), endian,
                     uint32_t(xenos::Endian::k16in32)));
    Line("xe_idx = (xe_idx >> 16u) | (xe_idx << 16u);");
    Close();
    Line("xe_idx = (xe_idx + {}) & {};",
         SysU(offsetof(SystemConstants, vertex_index_offset)),
         U(xenos::kVertexIndexMask));
    Line("xe_idx = min(max(xe_idx, {}), {});",
         SysU(offsetof(SystemConstants, vertex_index_min)),
         SysU(offsetof(SystemConstants, vertex_index_max)));
    Line("r0.x = float(xe_idx);");
  }
  Line("float ps = 0.0f;");
  Line("bool p0 = false;");
  Line("int a0 = 0;");
  Line("int xe_aL = 0;");
  Line("float4 xe_grad_h_lod = float4(0.0f);");
  Line("float4 xe_grad_v = float4(0.0f);");
  Open("do");
}

void MslShaderTranslator::CompleteVertexShader() {
  // DxbcShaderTranslator::CompleteVertexOrDomainShader.
  Modification m = modification();
  const size_t flags = offsetof(SystemConstants, flags) / 4;
  Open(fmt::format("if ((xe_sys[{}] & {}) == 0u)", flags,
                   U(DxbcShaderTranslator::kSysFlag_WNotReciprocal)));
  Line("xe_pos.w = 1.0f / xe_pos.w;");
  Close();
  Open(fmt::format("if ((xe_sys[{}] & {}) != 0u)", flags,
                   U(DxbcShaderTranslator::kSysFlag_XYDividedByW)));
  Line("xe_pos.xy = xe_pos.xy * xe_pos.ww;");
  Close();
  Open(fmt::format("if ((xe_sys[{}] & {}) != 0u)", flags,
                   U(DxbcShaderTranslator::kSysFlag_ZDividedByW)));
  Line("xe_pos.z = xe_pos.z * xe_pos.w;");
  Close();
  for (uint32_t i = 0; i < m.vertex.user_clip_plane_count; ++i) {
    size_t o = offsetof(SystemConstants, user_clip_planes) + sizeof(float) * 4 * i;
    Line("xe_out.clip[{}] = dot(xe_pos, float4({}, {}, {}, {}));", i, SysF(o),
         SysF(o + 4), SysF(o + 8), SysF(o + 12));
  }
  size_t sc = offsetof(SystemConstants, ndc_scale);
  size_t of = offsetof(SystemConstants, ndc_offset);
  Line("xe_pos.xyz = xe_pos.xyz * float3({}, {}, {});", SysF(sc), SysF(sc + 4),
       SysF(sc + 8));
  Line("xe_pos.xyz = fma(float3({}, {}, {}), xe_pos.www, xe_pos.xyz);",
       SysF(of), SysF(of + 4), SysF(of + 8));
  if (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b100) {
    Line("if ((as_type<uint>(xe_pse.z) & 0x7FFFFFFFu) != 0u) {{ xe_pos.w = "
         "as_type<float>(0x7FC00000u); }}");
  }
  // As the converter's vertex code does: +-Infinity or NaN in XYZ becomes the
  // canonical NaN (W is passed through).
  Line("xe_out.pos = float4(select(xe_pos.xyz, float3(as_type<float>(0x7FC00000u)), "
       "(as_type<uint3>(xe_pos.xyz) & 0x7F800000u) == 0x7F800000u), xe_pos.w);");
  uint32_t count = rex::bit_count(m.vertex.interpolator_mask);
  for (uint32_t i = 0; i < count; ++i) {
    Line("xe_out.o{} = xe_o{};", i, i);
  }
  if (config_.points_mesh) {
    // As below, but the corners are made in the mesh function: hand over the
    // radius and the kill flag (dxbc_geometry_shader.cc, kPointList).
    size_t cd = offsetof(SystemConstants, point_constant_diameter);
    size_t nr = offsetof(SystemConstants, point_screen_diameter_to_ndc_radius);
    Line("xe_pm_kill = any(xe_out.pos != xe_out.pos);");
    Line("float2 xe_psz = float2({}, {});", SysF(cd), SysF(cd + 4));
    if (m.vertex.output_point_size) {
      Line("if (xe_pse.x >= 0.0f) {{ xe_psz = xe_pse.xx; }}");
    }
    Line("xe_pm_kill = xe_pm_kill || !(0.0f < xe_psz.x) || !(0.0f < xe_psz.y);");
    Line("xe_pm_rad = xe_psz * float2({}, {});", SysF(nr), SysF(nr + 4));
    Line("xe_pm_rad = xe_pm_rad * xe_out.pos.ww;");
  }
  if (config_.expand_points) {
    // dxbc_geometry_shader.cc, kPointList, in the same operation order: the
    // whole sprite is dropped for a NaN position or a size that is not > 0;
    // radius = (diameter * screen-to-NDC) * W; corners TL, TR, BL, BR.
    size_t cd = offsetof(SystemConstants, point_constant_diameter);
    size_t nr = offsetof(SystemConstants, point_screen_diameter_to_ndc_radius);
    Line("float4 xe_pp = xe_out.pos;");
    Line("bool xe_pkill = any(xe_pp != xe_pp);");
    Line("float2 xe_psz = float2({}, {});", SysF(cd), SysF(cd + 4));
    if (m.vertex.output_point_size) {
      Line("if (xe_pse.x >= 0.0f) {{ xe_psz = xe_pse.xx; }}");
    }
    Line("xe_pkill = xe_pkill || !(0.0f < xe_psz.x) || !(0.0f < xe_psz.y);");
    Line("float2 xe_prad = xe_psz * float2({}, {});", SysF(nr), SysF(nr + 4));
    Line("xe_prad = xe_prad * xe_pp.ww;");
    Line("xe_out.pos.x = xe_pp.x + (((xe_corner & 1u) != 0u) ? xe_prad.x : "
         "-xe_prad.x);");
    Line("xe_out.pos.y = xe_pp.y + (((xe_corner >> 1u) != 0u) ? -xe_prad.y : "
         "xe_prad.y);");
    if (config_.point_coordinates) {
      Line("xe_out.point_coord = float2(float(xe_corner & 1u), "
           "float(xe_corner >> 1u));");
    }
    // A degenerate sprite: nothing is rasterized, as nothing is emitted.
    Line("if (xe_pkill) {{ xe_out.pos = float4(0.0f); }}");
  }
}

// ---------------------------------------------------------------------------
// Operands and results
// ---------------------------------------------------------------------------

MslShaderTranslator::Operand MslShaderTranslator::LoadOperand(
    const InstructionOperand& operand) {
  Operand result;
  if (operand.storage_addressing_mode !=
          InstructionStorageAddressingMode::kAbsolute &&
      operand.storage_source != InstructionStorageSource::kConstantFloat) {
    Fail("relative register addressing");
    result.base = "float4(0.0f)";
    return result;
  }
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      if (operand.storage_index >= register_count()) {
        Fail("register out of range");
      }
      result.base = fmt::format("r{}", operand.storage_index);
      break;
    case InstructionStorageSource::kConstantFloat: {
      if (operand.storage_addressing_mode !=
          InstructionStorageAddressingMode::kAbsolute) {
        // Dynamically indexed: the buffer holds all 256 constants
        // (ConstantRegisterMap::float_dynamic_addressing), as in DXBC.
        if (!current_shader().constant_register_map().float_dynamic_addressing) {
          Fail("relative constant without dynamic addressing");
        }
        result.base = fmt::format(
            "xe_float[{} + {}]",
            operand.storage_addressing_mode ==
                    InstructionStorageAddressingMode::kLoopRelative
                ? "xe_aL"
                : "a0",
            operand.storage_index);
        break;
      }
      uint32_t packed = current_shader()
                            .constant_register_map()
                            .GetPackedFloatConstantIndex(operand.storage_index);
      if (packed == UINT32_MAX) {
        Fail("unmapped float constant");
        result.base = "float4(0.0f)";
        return result;
      }
      result.base = fmt::format("xe_float[{}]", packed);
    } break;
    default:
      Fail("unsupported operand source");
      result.base = "float4(0.0f)";
      return result;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    SwizzleSource c = operand.GetComponent(i);
    if (c > SwizzleSource::kW) {
      Fail("constant swizzle in operand");
      c = SwizzleSource::kX;
    }
    result.swizzle[i] = kComp[uint32_t(c)];
  }
  result.abs = operand.is_absolute_value;
  result.neg = operand.is_negated;
  return result;
}

// value4 is a float4 expression holding the result before the result swizzle.
void MslShaderTranslator::StoreResult(const InstructionResult& result,
                                      const std::string& value4) {
  uint32_t used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }
  std::string dest;
  bool is_clamped = result.is_clamped;
  bool scalar_dest = false;
  switch (result.storage_target) {
    case InstructionStorageTarget::kNone:
      return;
    case InstructionStorageTarget::kRegister:
      if (result.storage_addressing_mode !=
          InstructionStorageAddressingMode::kAbsolute) {
        Fail("relative result addressing");
        return;
      }
      if (result.storage_index >= register_count()) {
        Fail("register out of range");
        return;
      }
      dest = fmt::format("r{}", result.storage_index);
      break;
    case InstructionStorageTarget::kColor:
      dest = fmt::format("xe_oc{}", result.storage_index);
      break;
    case InstructionStorageTarget::kInterpolator: {
      uint32_t mask = modification().vertex.interpolator_mask;
      uint32_t bit = 1u << result.storage_index;
      if (!(mask & bit)) {
        return;  // not needed by the pixel shader
      }
      dest = fmt::format("xe_o{}", rex::bit_count(mask & (bit - 1)));
    } break;
    case InstructionStorageTarget::kPosition:
      dest = "xe_pos";
      break;
    case InstructionStorageTarget::kExportAddress:
      if (!current_shader().memexport_eM_written()) {
        return;
      }
      dest = "xe_eA";
      break;
    case InstructionStorageTarget::kExportData:
      dest = fmt::format("xe_eM{}", result.storage_index);
      Line("xe_me_written |= {};", U(1u << result.storage_index));
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      dest = "xe_pse";
      vertex_pse_written_ = true;
      break;
    case InstructionStorageTarget::kDepth:
      dest = "xe_odepth";
      scalar_dest = true;
      is_clamped = true;
      break;
    default:
      Fail("unsupported result target");
      return;
  }
  uint32_t constant_mask = 0, constant_1_mask = 0;
  std::string swizzle;
  std::string written;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(used_write_mask & (1u << i))) {
      continue;
    }
    SwizzleSource c = result.components[i];
    if (c >= SwizzleSource::kX && c <= SwizzleSource::kW) {
      swizzle += kComp[uint32_t(c)];
      written += kComp[i];
    } else {
      constant_mask |= 1u << i;
      if (c == SwizzleSource::k1) {
        constant_1_mask |= 1u << i;
      }
    }
  }
  if (scalar_dest) {
    // Depth: X only.
    if (constant_mask & 1) {
      Line("xe_odepth = {};", F(float(constant_1_mask & 1)));
    } else {
      std::string v = fmt::format("({}).{}", value4, swizzle.substr(0, 1));
      Line("xe_odepth = {};", is_clamped ? "fast::saturate(" + v + ")" : v);
    }
    return;
  }
  if (!written.empty()) {
    std::string v = fmt::format("({}).{}", value4, swizzle);
    if (is_clamped) {
      v = "fast::saturate(" + v + ")";
    }
    Line("{}.{} = {};", dest, written, v);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (constant_mask & (1u << i)) {
      Line("{}.{} = {};", dest, kComp[i], F(float((constant_1_mask >> i) & 1)));
    }
  }
  if (result.storage_target ==
          InstructionStorageTarget::kPointSizeEdgeFlagKillVertex &&
      (used_write_mask & 1)) {
    Line("xe_pse.x = as_type<float>(min(max(as_type<int>({}), "
         "as_type<int>(xe_pse.x)), as_type<int>({})));",
         SysU(offsetof(SystemConstants, point_vertex_diameter_min)),
         SysU(offsetof(SystemConstants, point_vertex_diameter_max)));
  }
}

// ---------------------------------------------------------------------------
// ALU
// ---------------------------------------------------------------------------

void MslShaderTranslator::KillPixel(const std::string& condition) {
  Line("if ({}) {{ discard_fragment(); }}", condition);
}

void MslShaderTranslator::EmitVectorOp(const ParsedAluInstruction& instr,
                                       std::string& result,
                                       bool& predicate_written) {
  predicate_written = false;
  result.clear();
  uint32_t used = instr.vector_and_constant_result.GetUsedResultComponents();
  if (!used && !GetAluVectorOpcodeInfo(instr.vector_opcode).changed_state) {
    return;
  }
  uint32_t operand_count = instr.vector_opcode == AluVectorOpcode::kCube
                               ? 1
                               : instr.vector_operand_count;
  Operand op[3];
  for (uint32_t i = 0; i < operand_count; ++i) {
    op[i] = LoadOperand(instr.vector_operands[i]);
  }
  std::string v = NewTemp("v");
  Line("float4 {} = float4(0.0f);", v);
  result = v;
  auto identical = [&]() {
    return instr.vector_operands[0].GetIdenticalComponents(
        instr.vector_operands[1]);
  };
  // Xenos multiplication: +-0 or denormal * anything = +0, on the components
  // in `different` (the DXBC translator's min(|a|, |b|) == 0 select).
  auto mul0 = [&](const std::string& dst, uint32_t mask, uint32_t different,
                  const std::string& a, const std::string& b) {
    std::string m = Mask(mask);
    Line("{}.{} = ({} * {}).{};", dst, m, a, b, m);
    if (different & mask) {
      std::string dm = Mask(different & mask);
      Line("{}.{} = select({}.{}, float4(0.0f).{}, (fast::min(abs({}), "
           "abs({})) == 0.0f).{});",
           dst, dm, dst, dm, dm, a, b, dm);
    }
  };
  switch (instr.vector_opcode) {
    case AluVectorOpcode::kAdd:
      Line("{}.{} = ({} + {}).{};", v, Mask(used), op[0].Vec(), op[1].Vec(),
           Mask(used));
      break;
    case AluVectorOpcode::kMul:
    case AluVectorOpcode::kMad: {
      uint32_t different = used & ~identical();
      if (used) {
        mul0(v, used, different, op[0].Vec(), op[1].Vec());
        if (instr.vector_opcode == AluVectorOpcode::kMad) {
          Line("{}.{} = ({} + {}).{};", v, Mask(used), v, op[2].Vec(),
               Mask(used));
        }
      }
    } break;
    case AluVectorOpcode::kMax:
    case AluVectorOpcode::kMin: {
      uint32_t same = identical() & used;
      uint32_t different = used & ~same;
      if (different) {
        std::string m = Mask(different);
        Line("{}.{} = select({}, {}, {} {} {}).{};", v, m, op[1].Vec(),
             op[0].Vec(), op[0].Vec(),
             instr.vector_opcode == AluVectorOpcode::kMin ? "<" : ">=",
             op[1].Vec(), m);
      }
      if (same) {
        Line("{}.{} = {}.{};", v, Mask(same), op[0].Vec(), Mask(same));
      }
    } break;
    case AluVectorOpcode::kSeq:
    case AluVectorOpcode::kSgt:
    case AluVectorOpcode::kSge:
    case AluVectorOpcode::kSne: {
      const char* cmp = "==";
      std::string a = op[0].Vec(), b = op[1].Vec();
      if (instr.vector_opcode == AluVectorOpcode::kSgt) {
        cmp = "<";
        std::swap(a, b);
      } else if (instr.vector_opcode == AluVectorOpcode::kSge) {
        cmp = ">=";
      } else if (instr.vector_opcode == AluVectorOpcode::kSne) {
        cmp = "!=";
      }
      if (used) {
        Line("{}.{} = select(float4(0.0f), float4(1.0f), {} {} {}).{};", v,
             Mask(used), a, cmp, b, Mask(used));
      }
    } break;
    case AluVectorOpcode::kFrc:
      Line("{}.{} = xe_frc({}).{};", v, Mask(used), op[0].Vec(),
           Mask(used));
      break;
    case AluVectorOpcode::kTrunc:
      Line("{}.{} = fast::trunc({}).{};", v, Mask(used), op[0].Vec(),
           Mask(used));
      break;
    case AluVectorOpcode::kFloor:
      Line("{}.{} = fast::floor({}).{};", v, Mask(used), op[0].Vec(),
           Mask(used));
      break;
    case AluVectorOpcode::kCndEq:
    case AluVectorOpcode::kCndGe:
    case AluVectorOpcode::kCndGt: {
      std::string cond;
      if (instr.vector_opcode == AluVectorOpcode::kCndEq) {
        cond = op[0].Vec() + " == 0.0f";
      } else if (instr.vector_opcode == AluVectorOpcode::kCndGe) {
        cond = op[0].Vec() + " >= 0.0f";
      } else {
        cond = "0.0f < " + op[0].Vec();
      }
      Line("{}.{} = select({}, {}, {}).{};", v, Mask(used), op[2].Vec(),
           op[1].Vec(), cond, Mask(used));
    } break;
    case AluVectorOpcode::kDp4:
    case AluVectorOpcode::kDp3:
    case AluVectorOpcode::kDp2Add: {
      uint32_t count = instr.vector_opcode == AluVectorOpcode::kDp2Add ? 2
                       : instr.vector_opcode == AluVectorOpcode::kDp3  ? 3
                                                                       : 4;
      uint32_t different = ((1u << count) - 1) & ~identical();
      for (uint32_t i = 0; i < count; ++i) {
        std::string a = op[0].Comp(i), b = op[1].Comp(i);
        std::string d = i ? v + ".y" : v + ".x";
        Line("{} = {} * {};", d, a, b);
        if (different & (1u << i)) {
          Line("{} = (fast::min(abs({}), abs({})) == 0.0f) ? 0.0f : {};", d, a,
               b, d);
        }
        if (i) {
          Line("{}.x = {}.x + {}.y;", v, v, v);
        }
      }
      if (count == 2) {
        Line("{}.x = {}.x + {};", v, v, op[2].Comp(0));
      }
      Line("{} = {}.xxxx;", v, v);
    } break;
    case AluVectorOpcode::kCube: {
      // op[0] is .z_xy: Z in 0, X in 2, Y in 3.
      std::string cx = op[0].Comp(2), cy = op[0].Comp(3), cz = op[0].Comp(0);
      std::string n = NewTemp("cn");
      Line("bool {};", n);
      Open(fmt::format("if (abs({}) >= abs({}) && abs({}) >= abs({}))", cz, cx,
                       cz, cy));
      Line("{} = {} < 0.0f;", n, cz);
      Line("{}.x = -{};", v, cy);
      Line("{}.y = {} ? -{} : {};", v, n, cx, cx);
      Line("{}.z = {} * {};", v, F(2.0f), cz);
      Line("{}.w = {} ? {} : {};", v, n, F(5.0f), F(4.0f));
      Close();
      Open("else");
      Open(fmt::format("if (abs({}) >= abs({}))", cy, cx));
      Line("{} = {} < 0.0f;", n, cy);
      Line("{}.x = {} ? -{} : {};", v, n, cz, cz);
      Line("{}.y = {};", v, cx);
      Line("{}.z = {} * {};", v, F(2.0f), cy);
      Line("{}.w = {} ? {} : {};", v, n, F(3.0f), F(2.0f));
      Close();
      Open("else");
      Line("{} = {} < 0.0f;", n, cx);
      Line("{}.x = -{};", v, cy);
      Line("{}.y = {} ? {} : -{};", v, n, cz, cz);
      Line("{}.z = {} * {};", v, F(2.0f), cx);
      Line("{}.w = {} ? {} : {};", v, n, F(1.0f), F(0.0f));
      Close();
      Close();
    } break;
    case AluVectorOpcode::kMax4: {
      // SM4 max over the distinct source components, in index order.
      uint32_t remaining = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        remaining |= 1u << (uint32_t(op[0].swizzle[i] == 'x'   ? 0
                                     : op[0].swizzle[i] == 'y' ? 1
                                     : op[0].swizzle[i] == 'z' ? 2
                                                               : 3));
      }
      // The DXBC code selects from the swizzled operand by source component.
      Operand raw = op[0];
      std::string first;
      std::vector<std::string> comps;
      for (uint32_t i = 0; i < 4; ++i) {
        if (remaining & (1u << i)) {
          std::string s = raw.base + "." + kComp[i];
          if (raw.abs) s = "abs(" + s + ")";
          if (raw.neg) s = "(-" + s + ")";
          comps.push_back(s);
        }
      }
      Line("{}.x = {};", v, comps[0]);
      for (size_t i = 1; i < comps.size(); ++i) {
        Line("{}.x = fast::max({}.x, {});", v, v, comps[i]);
      }
      Line("{} = {}.xxxx;", v, v);
    } break;
    case AluVectorOpcode::kSetpEqPush:
    case AluVectorOpcode::kSetpNePush:
    case AluVectorOpcode::kSetpGtPush:
    case AluVectorOpcode::kSetpGePush: {
      predicate_written = true;
      std::string a0x = op[0].Comp(0), a0w = op[0].Comp(3);
      std::string b1x = op[1].Comp(0), b1w = op[1].Comp(3);
      auto cmp = [&](const std::string& s) {
        switch (instr.vector_opcode) {
          case AluVectorOpcode::kSetpEqPush:
            return s + " == 0.0f";
          case AluVectorOpcode::kSetpNePush:
            return s + " != 0.0f";
          case AluVectorOpcode::kSetpGtPush:
            return "0.0f < " + s;
          default:
            return s + " >= 0.0f";
        }
      };
      Line("p0 = ({} == 0.0f) && ({});", a0w, cmp(b1w));
      if (used) {
        Line("{}.x = ((({} == 0.0f) && ({})) ? {} : {}) + {};", v, a0x,
             cmp(b1x), F(-1.0f), a0x, F(1.0f));
        Line("{} = {}.xxxx;", v, v);
      }
    } break;
    case AluVectorOpcode::kKillEq:
    case AluVectorOpcode::kKillGt:
    case AluVectorOpcode::kKillGe:
    case AluVectorOpcode::kKillNe: {
      std::string cond;
      switch (instr.vector_opcode) {
        case AluVectorOpcode::kKillEq:
          cond = fmt::format("any({} == {})", op[0].Vec(), op[1].Vec());
          break;
        case AluVectorOpcode::kKillGt:
          cond = fmt::format("any({} < {})", op[1].Vec(), op[0].Vec());
          break;
        case AluVectorOpcode::kKillGe:
          cond = fmt::format("any({} >= {})", op[0].Vec(), op[1].Vec());
          break;
        default:
          cond = fmt::format("any({} != {})", op[0].Vec(), op[1].Vec());
          break;
      }
      std::string k = NewTemp("k");
      Line("bool {} = {};", k, cond);
      KillPixel(k);
      if (used) {
        Line("{} = float4({} ? 1.0f : 0.0f);", v, k);
      }
    } break;
    case AluVectorOpcode::kDst: {
      if (used & 0b0001) {
        Line("{}.x = 1.0f;", v);
      }
      if (used & 0b0010) {
        Line("{}.y = {} * {};", v, op[0].Comp(1), op[1].Comp(1));
        if (!(identical() & 0b0010)) {
          Line("{}.y = (fast::min(abs({}), abs({})) == 0.0f) ? 0.0f : {}.y;",
               v, op[0].Comp(1), op[1].Comp(1), v);
        }
      }
      if (used & 0b0100) {
        Line("{}.z = {};", v, op[0].Comp(2));
      }
      if (used & 0b1000) {
        Line("{}.w = {};", v, op[1].Comp(2));
      }
    } break;
    case AluVectorOpcode::kMaxA: {
      Line("a0 = int(fast::min(fast::max(fast::floor({} + 0.5f), -256.0f), "
           "255.0f));",
           op[0].Comp(3));
      uint32_t same = identical() & used;
      uint32_t different = used & ~same;
      if (different) {
        std::string m = Mask(different);
        Line("{}.{} = select({}, {}, {} >= {}).{};", v, m, op[1].Vec(),
             op[0].Vec(), op[0].Vec(), op[1].Vec(), m);
      }
      if (same) {
        Line("{}.{} = {}.{};", v, Mask(same), op[0].Vec(), Mask(same));
      }
    } break;
    default:
      Fail("unknown vector op");
  }
}

void MslShaderTranslator::EmitScalarOp(const ParsedAluInstruction& instr,
                                       bool& predicate_written) {
  predicate_written = false;
  if (instr.scalar_opcode == AluScalarOpcode::kRetainPrev) {
    return;
  }
  Operand ops[2];
  for (uint32_t i = 0; i < instr.scalar_operand_count; ++i) {
    ops[i] = LoadOperand(instr.scalar_operands[i]);
  }
  std::string a = ops[0].Comp(0), b = ops[0].Comp(1), c = ops[1].Comp(0);
  auto zero_rule = [&](const std::string& x, const std::string& y) {
    Line("ps = (fast::min(abs({}), abs({})) == 0.0f) ? 0.0f : ps;", x, y);
  };
  switch (instr.scalar_opcode) {
    case AluScalarOpcode::kAdds:
      Line("ps = {} + {};", a, b);
      break;
    case AluScalarOpcode::kAddsPrev:
      Line("ps = {} + ps;", a);
      break;
    case AluScalarOpcode::kMuls:
      Line("ps = {} * {};", a, b);
      if (instr.scalar_operands[0].components[0] !=
          instr.scalar_operands[0].components[1]) {
        zero_rule(a, b);
      }
      break;
    case AluScalarOpcode::kMulsPrev:
    case AluScalarOpcode::kMulsPrev2: {
      if (instr.scalar_opcode == AluScalarOpcode::kMulsPrev2) {
        Open(fmt::format(
            "if (ps != {} && -abs(ps) >= {} && -abs({}) >= {} && 0.0f < {})",
            F(-FLT_MAX), F(-FLT_MAX), b, F(-FLT_MAX), b));
      }
      std::string z = NewTemp("z");
      Line("bool {} = fast::min(abs({}), abs(ps)) == 0.0f;", z, a);
      Line("ps = {} * ps;", a);
      Line("ps = {} ? 0.0f : ps;", z);
      if (instr.scalar_opcode == AluScalarOpcode::kMulsPrev2) {
        Close();
        Open("else");
        Line("ps = {};", F(-FLT_MAX));
        Close();
      }
    } break;
    case AluScalarOpcode::kMaxs:
    case AluScalarOpcode::kMins:
      if (instr.scalar_operands[0].components[0] ==
          instr.scalar_operands[0].components[1]) {
        Line("ps = {};", a);
      } else {
        Line("ps = ({} {} {}) ? {} : {};", a,
             instr.scalar_opcode == AluScalarOpcode::kMins ? "<" : ">=", b, a,
             b);
      }
      break;
    case AluScalarOpcode::kSeqs:
      Line("ps = ({} == 0.0f) ? 1.0f : 0.0f;", a);
      break;
    case AluScalarOpcode::kSgts:
      Line("ps = (0.0f < {}) ? 1.0f : 0.0f;", a);
      break;
    case AluScalarOpcode::kSges:
      Line("ps = ({} >= 0.0f) ? 1.0f : 0.0f;", a);
      break;
    case AluScalarOpcode::kSnes:
      Line("ps = ({} != 0.0f) ? 1.0f : 0.0f;", a);
      break;
    case AluScalarOpcode::kFrcs:
      Line("ps = xe_frc({});", a);
      break;
    case AluScalarOpcode::kTruncs:
      Line("ps = fast::trunc({});", a);
      break;
    case AluScalarOpcode::kFloors:
      Line("ps = fast::floor({});", a);
      break;
    case AluScalarOpcode::kExp:
      Line("ps = fast::exp2({});", a);
      break;
    case AluScalarOpcode::kLogc:
      Line("ps = fast::log2({});", a);
      Line("ps = (ps == {}) ? {} : ps;", F(-INFINITY), F(-FLT_MAX));
      break;
    case AluScalarOpcode::kLog:
      Line("ps = fast::log2({});", a);
      break;
    case AluScalarOpcode::kRcpc:
    case AluScalarOpcode::kRsqc:
      if (instr.scalar_opcode == AluScalarOpcode::kRsqc) {
        Line("ps = fast::rsqrt({});", a);
      } else {
        Line("ps = 1.0f / {};", a);
      }
      // +-Infinity -> +-FLT_MAX by adding -1 to the bits.
      Line("ps = as_type<float>(as_type<uint>(ps) + ((abs(ps) == {}) ? "
           "0xFFFFFFFFu : 0u));",
           F(INFINITY));
      break;
    case AluScalarOpcode::kRcpf:
    case AluScalarOpcode::kRsqf:
      if (instr.scalar_opcode == AluScalarOpcode::kRsqf) {
        Line("ps = fast::rsqrt({});", a);
      } else {
        Line("ps = 1.0f / {};", a);
      }
      Line("ps = as_type<float>(as_type<uint>(ps) & (((abs(ps) != {}) ? "
           "0xFFFFFFFFu : 0u) | 0x80000000u));",
           F(INFINITY));
      break;
    case AluScalarOpcode::kRcp:
      Line("ps = 1.0f / {};", a);
      break;
    case AluScalarOpcode::kRsq:
      Line("ps = fast::rsqrt({});", a);
      break;
    case AluScalarOpcode::kMaxAs:
    case AluScalarOpcode::kMaxAsf:
      if (instr.scalar_opcode == AluScalarOpcode::kMaxAsf) {
        Line("a0 = int(fast::min(fast::max(fast::floor({}), 0.0f), 255.0f));",
             a);
      } else {
        Line("a0 = int(fast::min(fast::max(fast::floor({} + 0.5f), 0.0f), "
             "255.0f));",
             a);
      }
      if (instr.scalar_operands[0].components[0] ==
          instr.scalar_operands[0].components[1]) {
        Line("ps = {};", a);
      } else {
        Line("ps = ({} >= {}) ? {} : {};", a, b, a, b);
      }
      break;
    case AluScalarOpcode::kSubs:
      Line("ps = {} + (-{});", a, b);
      break;
    case AluScalarOpcode::kSubsPrev:
      Line("ps = {} + (-ps);", a);
      break;
    case AluScalarOpcode::kSetpEq:
      predicate_written = true;
      Line("p0 = {} == 0.0f;", a);
      Line("ps = p0 ? 0.0f : 1.0f;");
      break;
    case AluScalarOpcode::kSetpNe:
      predicate_written = true;
      Line("p0 = {} != 0.0f;", a);
      Line("ps = p0 ? 0.0f : 1.0f;");
      break;
    case AluScalarOpcode::kSetpGt:
      predicate_written = true;
      Line("p0 = 0.0f < {};", a);
      Line("ps = p0 ? 0.0f : 1.0f;");
      break;
    case AluScalarOpcode::kSetpGe:
      predicate_written = true;
      Line("p0 = {} >= 0.0f;", a);
      Line("ps = p0 ? 0.0f : 1.0f;");
      break;
    case AluScalarOpcode::kSetpInv:
      predicate_written = true;
      Line("ps = ({} == 0.0f) ? 1.0f : {};", a, a);
      Line("p0 = {} == 1.0f;", a);
      Line("ps = p0 ? 0.0f : ps;");
      break;
    case AluScalarOpcode::kSetpPop:
      predicate_written = true;
      Line("ps = {} + (-1.0f);", a);
      Line("p0 = 0.0f >= ps;");
      Line("ps = p0 ? 0.0f : ps;");
      break;
    case AluScalarOpcode::kSetpClr:
      predicate_written = true;
      Line("ps = {};", F(FLT_MAX));
      Line("p0 = false;");
      break;
    case AluScalarOpcode::kSetpRstr:
      predicate_written = true;
      Line("p0 = {} == 0.0f;", a);
      Line("ps = p0 ? 0.0f : {};", a);
      break;
    case AluScalarOpcode::kKillsEq:
    case AluScalarOpcode::kKillsGt:
    case AluScalarOpcode::kKillsGe:
    case AluScalarOpcode::kKillsNe:
    case AluScalarOpcode::kKillsOne: {
      std::string cond;
      switch (instr.scalar_opcode) {
        case AluScalarOpcode::kKillsEq:
          cond = a + " == 0.0f";
          break;
        case AluScalarOpcode::kKillsGt:
          cond = "0.0f < " + a;
          break;
        case AluScalarOpcode::kKillsGe:
          cond = a + " >= 0.0f";
          break;
        case AluScalarOpcode::kKillsNe:
          cond = a + " != 0.0f";
          break;
        default:
          cond = a + " == 1.0f";
          break;
      }
      std::string k = NewTemp("k");
      Line("bool {} = {};", k, cond);
      KillPixel(k);
      Line("ps = {} ? 1.0f : 0.0f;", k);
    } break;
    case AluScalarOpcode::kSqrt:
      Line("ps = fast::sqrt({});", a);
      break;
    case AluScalarOpcode::kMulsc0:
    case AluScalarOpcode::kMulsc1:
      Line("ps = {} * {};", a, c);
      if (!(instr.scalar_operands[0].GetIdenticalComponents(
                instr.scalar_operands[1]) &
            0b0001)) {
        zero_rule(a, c);
      }
      break;
    case AluScalarOpcode::kAddsc0:
    case AluScalarOpcode::kAddsc1:
      Line("ps = {} + {};", a, c);
      break;
    case AluScalarOpcode::kSubsc0:
    case AluScalarOpcode::kSubsc1:
      Line("ps = {} + (-{});", a, c);
      break;
    case AluScalarOpcode::kSin:
      Line("ps = fast::sin({});", a);
      break;
    case AluScalarOpcode::kCos:
      Line("ps = fast::cos({});", a);
      break;
    default:
      Fail("unknown scalar op");
  }
}

void MslShaderTranslator::ProcessAluInstruction(
    const ParsedAluInstruction& instr, uint8_t) {
  if (instr.IsNop()) {
    return;
  }
  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);
  std::string vector_result;
  bool pw_vector = false, pw_scalar = false;
  EmitVectorOp(instr, vector_result, pw_vector);
  EmitScalarOp(instr, pw_scalar);
  if (!vector_result.empty()) {
    StoreResult(instr.vector_and_constant_result, vector_result);
  } else {
    // Constant-only writes.
    StoreResult(instr.vector_and_constant_result, "float4(0.0f)");
  }
  StoreResult(instr.scalar_result, "float4(ps)");
  if (pw_vector || pw_scalar) {
    cf_exec_predicate_written_ = true;
    CloseInstructionPredication();
  }
}

// ---------------------------------------------------------------------------
// Texture fetch
// ---------------------------------------------------------------------------

uint32_t MslShaderTranslator::FindTextureBinding(
    uint32_t fetch_constant, xenos::FetchOpDimension dimension,
    bool is_signed) {
  if (dimension == xenos::FetchOpDimension::k1D) {
    dimension = xenos::FetchOpDimension::k2D;
  }
  const auto& bindings = dxbc_shader_->GetTextureBindingsAfterTranslation();
  for (const auto& b : bindings) {
    if (b.fetch_constant == fetch_constant && b.dimension == dimension &&
        b.is_signed == is_signed) {
      return b.bindless_descriptor_index;
    }
  }
  Fail("texture binding not in the DXBC translation");
  return 0;
}

uint32_t MslShaderTranslator::FindSamplerBinding(
    uint32_t fetch_constant, xenos::TextureFilter mag_filter,
    xenos::TextureFilter min_filter, xenos::TextureFilter mip_filter,
    xenos::AnisoFilter aniso_filter) {
  if (aniso_filter != xenos::AnisoFilter::kDisabled &&
      aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    mag_filter = xenos::TextureFilter::kLinear;
    min_filter = xenos::TextureFilter::kLinear;
    mip_filter = xenos::TextureFilter::kLinear;
    aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  }
  const auto& bindings = dxbc_shader_->GetSamplerBindingsAfterTranslation();
  for (const auto& b : bindings) {
    if (b.fetch_constant == fetch_constant && b.mag_filter == mag_filter &&
        b.min_filter == min_filter && b.mip_filter == mip_filter &&
        b.aniso_filter == aniso_filter) {
      return b.bindless_descriptor_index;
    }
  }
  Fail("sampler binding not in the DXBC translation");
  return 0;
}

void MslShaderTranslator::PWLGammaToLinear(const std::string& value) {
  // DxbcShaderTranslator::PWLGammaToLinear, not pre-saturated.
  std::string s = NewTemp("gs"), o = NewTemp("go"), t = NewTemp("gt");
  Line("float {}, {};", s, o);
  Open(fmt::format("if ({} >= {})", value, F(96.0f / 255.0f)));
  Line("bool {} = {} >= {};", t, value, F(192.0f / 255.0f));
  Line("{} = {} ? {} : {};", s, t, F(8.0f / 1024.0f), F(4.0f / 1024.0f));
  Line("{} = {} ? {} : {};", o, t, F(-1024.0f), F(-256.0f));
  Close();
  Open("else");
  Line("bool {} = {} >= {};", t, value, F(64.0f / 255.0f));
  Line("{} = {} ? {} : {};", s, t, F(2.0f / 1024.0f), F(1.0f / 1024.0f));
  Line("{} = {} ? {} : {};", o, t, F(-64.0f), F(0.0f));
  Close();
  Line("{} = fast::saturate({});", value, value);
  Line("{} = {} * {};", value, value, F(255.0f * 1024.0f));
  Line("{} = fma({}, {}, {});", value, value, s, o);
  Line("{} = fast::trunc({} * {});", s, value, s);
  Line("{} = {} + {};", value, value, s);
  Line("{} = {} * {};", value, value, F(1.0f / 1023.0f));
}

void MslShaderTranslator::PreSaturatedLinearToPWLGamma(
    const std::string& value) {
  std::string s = NewTemp("gs"), o = NewTemp("go"), t = NewTemp("gt");
  Line("float {}, {};", s, o);
  Open(fmt::format("if ({} >= {})", value, F(128.0f / 1023.0f)));
  Line("bool {} = {} >= {};", t, value, F(512.0f / 1023.0f));
  Line("{} = {} ? {} : {};", s, t, F(1023.0f / 8.0f), F(1023.0f / 4.0f));
  Line("{} = {} ? {} : {};", o, t, F(128.0f / 255.0f), F(64.0f / 255.0f));
  Close();
  Open("else");
  Line("bool {} = {} >= {};", t, value, F(64.0f / 1023.0f));
  Line("{} = {} ? {} : {};", s, t, F(1023.0f / 2.0f), F(1023.0f));
  Line("{} = {} ? {} : {};", o, t, F(32.0f / 255.0f), F(0.0f));
  Close();
  Line("{} = fast::trunc({} * {});", value, value, s);
  Line("{} = fma({}, {}, {});", value, value, F(1.0f / 255.0f), o);
}

void MslShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);

  switch (instr.opcode) {
    case FetchOpcode::kSetTextureLod:
      Line("xe_grad_h_lod.w = {};", LoadOperand(instr.operands[0]).Comp(0));
      return;
    case FetchOpcode::kSetTextureGradientsHorz:
      Line("xe_grad_h_lod.xyz = {}.xyz;", LoadOperand(instr.operands[0]).Vec());
      return;
    case FetchOpcode::kSetTextureGradientsVert:
      Line("xe_grad_v.xyz = {}.xyz;", LoadOperand(instr.operands[0]).Vec());
      return;
    default:
      break;
  }

  uint32_t used_result_components = instr.result.GetUsedResultComponents();
  uint32_t nonzero = instr.GetNonZeroResultComponents();
  if (instr.opcode == FetchOpcode::kGetTextureWeights) {
    nonzero &= ~uint32_t(0b1000);
  }
  if (!nonzero) {
    StoreResult(instr.result, "float4(0.0f)");
    return;
  }
  std::string res = NewTemp("t");
  Line("float4 {} = float4(0.0f);", res);

  if (instr.opcode == FetchOpcode::kGetTextureGradients) {
    Operand g = LoadOperand(instr.operands[0]);
    if (nonzero & 0b0101) {
      // .xxyy derivative X of operand x (into x) and y (into z).
      std::string m = Mask(nonzero & 0b0101);
      Line("{}.{} = dfdx({}.xxyy).{};", res, m, g.Vec(), m);
    }
    if (nonzero & 0b1010) {
      std::string m = Mask(nonzero & 0b1010);
      Line("{}.{} = dfdy({}.xxxy).{};", res, m, g.Vec(), m);
    }
    StoreResult(instr.result, res);
    return;
  }
  if (instr.opcode != FetchOpcode::kTextureFetch &&
      instr.opcode != FetchOpcode::kGetTextureWeights) {
    Fail("unsupported fetch opcode");
    return;
  }
  if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
    Fail("3D / stacked texture fetch");
    return;
  }

  uint32_t tf = instr.operands[1].storage_index;
  bool use_computed_lod =
      instr.attributes.use_computed_lod &&
      (is_pixel_shader() || instr.attributes.use_register_gradients);

  // Offsets (with the rounding epsilon), as in the DXBC translator.
  float offsets[3] = {};
  constexpr float rounding_offset = 1.5f / 1024.0f;
  bool weights = instr.opcode == FetchOpcode::kGetTextureWeights;
  switch (instr.dimension) {
    case xenos::FetchOpDimension::k1D:
      offsets[0] = instr.attributes.offset_x + rounding_offset;
      if (weights) offsets[0] -= 0.5f;
      break;
    case xenos::FetchOpDimension::k2D:
      offsets[0] = instr.attributes.offset_x + rounding_offset;
      offsets[1] = instr.attributes.offset_y + rounding_offset;
      if (weights) {
        offsets[0] -= 0.5f;
        offsets[1] -= 0.5f;
      }
      break;
    case xenos::FetchOpDimension::kCube:
      offsets[0] = instr.attributes.offset_x + rounding_offset;
      offsets[1] = instr.attributes.offset_y + rounding_offset;
      if (weights) {
        offsets[0] -= 0.5f;
        offsets[1] -= 0.5f;
      } else {
        offsets[2] = instr.attributes.offset_z;
      }
      break;
    default:
      break;
  }
  uint32_t offsets_not_zero = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    if (offsets[i]) offsets_not_zero |= 1u << i;
  }
  std::string offsets_src = fmt::format("float4({}, {}, {}, 0.0f)",
                                        F(offsets[0]), F(offsets[1]),
                                        F(offsets[2]));

  // Size.
  uint32_t size_needed = 0;
  if (weights) {
    if (!instr.attributes.unnormalized_coordinates) {
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
          size_needed |= 0b0001;
          break;
        default:
          size_needed |= nonzero & 0b0011;
          break;
      }
    }
  } else {
    size_needed |= offsets_not_zero;
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
        size_needed |= 0b0001;
        break;
      case xenos::FetchOpDimension::k2D:
        if (instr.attributes.unnormalized_coordinates) size_needed |= 0b0011;
        break;
      case xenos::FetchOpDimension::kCube:
        if (instr.attributes.unnormalized_coordinates) size_needed |= 0b0011;
        size_needed &= 0b0011;
        break;
      default:
        break;
    }
  }
  std::string size = NewTemp("sz");
  std::string w1d = NewTemp("w1d");
  if (size_needed) {
    Line("float4 {} = float4(0.0f);", size);
    if (instr.dimension == xenos::FetchOpDimension::k1D) {
      Line("uint {} = extract_bits({}, 0, 24);", w1d, FetchWord(tf, 2));
      Line("{}.x = float({} + 1u);", size, w1d);
    } else {
      std::string m = Mask(size_needed);
      Line("{}.{} = float4(uint4(extract_bits({}, 0, 13), extract_bits({}, "
           "13, 13), 0u, 0u) + 1u).{};",
           size, m, FetchWord(tf, 2), FetchWord(tf, 2), m);
    }
  }
  uint32_t revert_scale_axes =
      config_.draw_resolution_scaled_texture_offsets
          ? uint32_t(config_.draw_resolution_scale_x > 1) |
                (uint32_t(config_.draw_resolution_scale_y > 1) << 1)
          : 0;

  if (weights) {
    Operand coord = LoadOperand(instr.operands[0]);
    std::string m = Mask(nonzero);
    uint32_t offsets_needed = offsets_not_zero & nonzero;
    std::string c = NewTemp("wc");
    Line("float4 {} = {};", c, coord.Vec());
    if (instr.attributes.unnormalized_coordinates) {
      if (offsets_needed) {
        Line("{}.{} = ({} + {}).{};", c, m, coord.Vec(), offsets_src, m);
      }
    } else {
      if (offsets_needed) {
        Line("{}.{} = fma({}, {}, {}).{};", c, m, coord.Vec(), size,
             offsets_src, m);
      } else {
        Line("{}.{} = ({} * {}).{};", c, m, coord.Vec(), size, m);
      }
    }
    Line("{}.{} = xe_frc({}).{};", res, m, c, m);
  } else {
    // Signs.
    std::string signs = NewTemp("sg");
    size_t signs_offset = offsetof(SystemConstants, texture_swizzled_signs) +
                          sizeof(uint32_t) * (tf >> 2);
    uint32_t signs_shift = (tf & 3) * 8;
    Line("uint4 {} = uint4(0u);", signs);
    for (uint32_t i = 0; i < 4; ++i) {
      if (nonzero & (1u << i)) {
        Line("{}.{} = extract_bits({}, {}, 2);", signs, kComp[i],
             SysU(signs_offset), signs_shift + 2 * i);
      }
    }
    // Coordinates.
    std::string coord = NewTemp("tc");
    Line("float4 {} = float4(0.0f);", coord);
    Operand co = LoadOperand(instr.operands[0]);
    uint32_t normalized = instr.dimension == xenos::FetchOpDimension::k1D
                              ? 0b0001
                              : 0b0011;
    uint32_t with_offsets = normalized & offsets_not_zero;
    uint32_t with_scaled = with_offsets & revert_scale_axes;
    uint32_t with_unscaled = with_offsets & ~with_scaled;
    uint32_t without_offsets = normalized & ~with_offsets;
    std::string scaled_cond = fmt::format(
        "(({} & {}) != 0u)",
        SysU(offsetof(SystemConstants, textures_resolution_scaled)),
        U(uint32_t(1) << tf));
    if (instr.attributes.unnormalized_coordinates) {
      if (with_offsets) {
        if (with_scaled) {
          std::string m = Mask(with_scaled);
          Open(fmt::format("if {}", scaled_cond));
          Line("{}.{} = ({} + float4({}, {}, 0.0f, 0.0f)).{};", coord, m,
               co.Vec(), F(offsets[0] / config_.draw_resolution_scale_x),
               F(offsets[1] / config_.draw_resolution_scale_y), m);
          Close();
          Open("else");
          Line("{}.{} = ({} + {}).{};", coord, m, co.Vec(), offsets_src, m);
          Close();
        }
        if (with_unscaled) {
          std::string m = Mask(with_unscaled);
          Line("{}.{} = ({} + {}).{};", coord, m, co.Vec(), offsets_src, m);
        }
        if (without_offsets) {
          std::string m = Mask(without_offsets);
          Line("{}.{} = {}.{};", coord, m, co.Vec(), m);
        }
        std::string m = Mask(normalized & 0b011);
        Line("{}.{} = ({} / {}).{};", coord, m, coord, size, m);
      } else {
        std::string m = Mask(normalized);
        Line("{}.{} = ({} / {}).{};", coord, m, co.Vec(), size, m);
      }
    } else {
      if (with_offsets) {
        std::string m = Mask(with_offsets);
        Line("{}.{} = ({} / {}).{};", coord, m, offsets_src, size, m);
        if (with_scaled) {
          std::string ms = Mask(with_scaled);
          Open(fmt::format("if {}", scaled_cond));
          Line("{}.{} = fma({}, float4({}, {}, 1.0f, 1.0f), {}).{};", coord,
               ms, coord, F(1.0f / config_.draw_resolution_scale_x),
               F(1.0f / config_.draw_resolution_scale_y), co.Vec(), ms);
          Close();
          Open("else");
          Line("{}.{} = ({} + {}).{};", coord, ms, co.Vec(), coord, ms);
          Close();
        }
        if (with_unscaled) {
          std::string mu = Mask(with_unscaled);
          Line("{}.{} = ({} + {}).{};", coord, mu, co.Vec(), coord, mu);
        }
      }
      if (without_offsets & 0b011) {
        std::string m = Mask(without_offsets & 0b011);
        Line("{}.{} = {}.{};", coord, m, co.Vec(), m);
      }
    }
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D: {
        Open(fmt::format("if (extract_bits({}, 9, 2) == {}u)",
                         FetchWord(tf, 5),
                         uint32_t(xenos::DataDimension::k1D)));
        Open(fmt::format("if ({} >= {}u)", w1d,
                         xenos::kTexture2DCubeMaxWidthHeight));
        Line("{}.w = float({} + 1u);", coord, w1d);
        Line("{}.y = {}.x * {}.w;", coord, coord, coord);
        Line("{}.z = {}.y / {};", coord, coord,
             F(float(xenos::kTexture2DCubeMaxWidthHeight)));
        Line("{}.w = fast::floor({}.z);", coord, coord);
        Line("{}.x = xe_frc({}.z);", coord, coord);
        Line("{}.z = float({} + 1u);", coord, w1d);
        Line("{}.z = {}.z / {};", coord, coord,
             F(float(xenos::kTexture2DCubeMaxWidthHeight)));
        Line("{}.z = fast::ceil({}.z);", coord, coord);
        Line("{}.y = {}.w / {}.z;", coord, coord, coord);
        Line("{}.z = 0.0f;", coord);
        Close();
        Open("else");
        Line("{}.yz = float2(0.0f);", coord);
        Close();
        Close();
        Open("else");
        Line("{}.yz = float2(0.0f);", coord);
        Close();
      } break;
      case xenos::FetchOpDimension::k2D:
        Line("{}.z = 0.0f;", coord);
        break;
      case xenos::FetchOpDimension::kCube: {
        Line("{}.xy = fma({}.xy, float2({}), float2({}));", coord, coord,
             F(2.0f), F(-3.0f));
        std::string face = NewTemp("face");
        if (offsets[2]) {
          Line("uint {} = uint({} + {});", face, co.Comp(2), F(offsets[2]));
        } else {
          Line("uint {} = uint({});", face, co.Comp(2));
        }
        Line("{} = min({}, 5u);", face, face);
        std::string axis = NewTemp("ax"), negf = NewTemp("ng");
        Line("uint {} = extract_bits({}, 1, 2);", axis, face);
        Line("bool {} = extract_bits({}, 0, 1) != 0u;", negf, face);
        Open(fmt::format("if ({} == 0u)", axis));
        Line("{}.y = -{}.y;", coord, coord);
        Line("{}.z = {} ? {}.x : -{}.x;", coord, negf, coord, coord);
        Line("{}.x = {} ? {} : {};", coord, negf, F(-1.0f), F(1.0f));
        Close();
        Open(fmt::format("else if ({} == 1u)", axis));
        Line("{}.z = {} ? -{}.y : {}.y;", coord, negf, coord, coord);
        Line("{}.y = {} ? {} : {};", coord, negf, F(-1.0f), F(1.0f));
        Close();
        Open("else");
        Line("{}.x = {} ? -{}.x : {}.x;", coord, negf, coord, coord);
        Line("{}.y = -{}.y;", coord, coord);
        Line("{}.z = {} ? {} : {};", coord, negf, F(-1.0f), F(1.0f));
        Close();
      } break;
      default:
        break;
    }

    // LOD / gradients.
    std::string lod = NewTemp("lod");
    std::string gh = NewTemp("gh"), gv = NewTemp("gv");
    bool have_grad = false;
    bool have_lod = false;
    uint32_t grad_count = 0;
    if (instr.attributes.mip_filter != xenos::TextureFilter::kBaseMap) {
      have_lod = true;
      Line("float {} = float(extract_bits(as_type<int>({}), 12, 10));", lod,
           FetchWord(tf, 4));
      if (instr.attributes.use_register_lod) {
        Line("{} = fma({}, {}, xe_grad_h_lod.w);", lod, lod, F(1.0f / 32.0f));
        if (instr.attributes.lod_bias) {
          Line("{} = {} + {};", lod, lod, F(instr.attributes.lod_bias));
        }
      } else {
        if (instr.attributes.lod_bias) {
          Line("{} = fma({}, {}, {});", lod, lod, F(1.0f / 32.0f),
               F(instr.attributes.lod_bias));
        } else {
          Line("{} = {} * {};", lod, lod, F(1.0f / 32.0f));
        }
      }
      if (use_computed_lod) {
        have_grad = true;
        grad_count =
            instr.dimension == xenos::FetchOpDimension::kCube ? 3 : 2;
        std::string gm = Mask((1u << grad_count) - 1);
        Line("{} = fast::exp2({});", lod, lod);
        Line("float4 {} = float4(0.0f), {} = float4(0.0f);", gh, gv);
        if (instr.attributes.use_register_gradients &&
            instr.dimension != xenos::FetchOpDimension::k1D) {
          Line("{}.{} = (xe_grad_h_lod * {}).{};", gh, gm, lod, gm);
          Line("{}.{} = (xe_grad_v * {}).{};", gv, gm, lod, gm);
          if (instr.attributes.unnormalized_coordinates &&
              instr.dimension != xenos::FetchOpDimension::kCube) {
            Line("{}.{} = ({} / {}).{};", gh, gm, gh, size, gm);
            Line("{}.{} = ({} / {}).{};", gv, gm, gv, size, gm);
          }
        } else {
          Line("{}.{} = (dfdx({}) * {}).{};", gh, gm, coord, lod, gm);
          Line("{}.{} = (dfdy({}) * {}).{};", gv, gm, coord, lod, gm);
        }
      }
    }
    std::string lod_src = have_lod ? lod : "0.0f";

    uint32_t smp = FindSamplerBinding(
        tf, instr.attributes.mag_filter, instr.attributes.min_filter,
        instr.attributes.mip_filter,
        use_computed_lod ? instr.attributes.aniso_filter
                         : xenos::AnisoFilter::kDisabled);
    std::string smp_v = NewTemp("smp");
    Line("sampler {} = xe_tl.smp[xe_desc[{}]].s;", smp_v, smp);

    std::string is_signed = NewTemp("isg");
    Line("bool4 {} = {} == uint4({}u);", is_signed, signs,
         uint32_t(xenos::TextureSign::kSigned));
    uint32_t first;
    rex::bit_scan_forward(nonzero, &first);
    std::string all_signed = fmt::format("{}.{}", is_signed, kComp[first]);
    std::string any_signed = all_signed;
    if (nonzero != (1u << first)) {
      std::string comps;
      for (uint32_t i = 0; i < 4; ++i) {
        if (nonzero & (1u << i)) comps += kComp[i];
      }
      if (comps.size() >= 2) {
        all_signed = fmt::format("all({}.{})", is_signed, comps);
        any_signed = fmt::format("any({}.{})", is_signed, comps);
        if (comps.size() == 2 || comps.size() == 3 || comps.size() == 4) {
          // all()/any() accept bool2/3/4.
        }
      }
    }
    uint32_t tex_u = FindTextureBinding(tf, instr.dimension, false);
    uint32_t tex_s = FindTextureBinding(tf, instr.dimension, true);
    std::string m = Mask(nonzero);
    auto sample_at = [&](uint32_t desc_index, const std::string& xy,
                         const std::string& smp_name) -> std::string {
      if (instr.dimension == xenos::FetchOpDimension::kCube) {
        std::string t =
            fmt::format("xe_tl.tcube[xe_desc[{}]].t", desc_index);
        if (have_grad) {
          return fmt::format("{}.sample({}, {}.xyz, gradientcube({}.xyz, {}.xyz))",
                             t, smp_v, coord, gh, gv);
        }
        return fmt::format("{}.sample({}, {}.xyz, level({}))", t, smp_v, coord,
                           lod_src);
      }
      std::string t = fmt::format("xe_tl.t2d[xe_desc[{}]].t", desc_index);
      if (have_grad) {
        return fmt::format(
            "{}.sample({}, {}, 0u, gradient2d({}.xy, {}.xy))", t, smp_name,
            xy, gh, gv);
      }
      return fmt::format("{}.sample({}, {}, 0u, level({}))", t, smp_name, xy,
                         lod_src);
    };
    // Guest-texel footprint of offset fetches at scaled resolution
    // (MslTranslatorConfig::effects_footprint). Taps placed one guest texel
    // apart on a texture with `scale` (2 or 3) host texels per guest texel
    // must each cover their guest texel, or the kernel has holes between the
    // taps. Along every axis with a nonzero texel offset:
    // - bilinear fetches (the game's sampler): at 2x the sample moves to the
    //   host-texel corner on the kernel-centre side of the tap (floor for
    //   positive offsets, ceil for negative), which the filter turns into the
    //   2-texel box; taps already on a corner (2:1 downsampling) stay put and
    //   a symmetric kernel stays centred. At 3x two samples at +-2/3 host
    //   texel around the tap, together the 3-texel box;
    // - point fetches on a clamp-to-edge texture: the guest texel the point
    //   sample falls in, averaged exactly by a bilinear sampler at its centre
    //   (one sample at 2x, two at +-2/3 at 3x): the 1x texel value of the
    //   box-downsampled image;
    // - other point fetches keep one host texel.
    // At 2x this costs no extra sample: only the coordinate and the sampler
    // change.
    uint32_t fp_axes = 0;
    if (config_.effects_footprint &&
        !config_.draw_resolution_scaled_texture_offsets && is_pixel_shader() &&
        instr.dimension == xenos::FetchOpDimension::k2D) {
      const uint32_t sx = config_.draw_resolution_scale_x;
      const uint32_t sy = config_.draw_resolution_scale_y;
      if (instr.attributes.offset_x != 0.0f && (sx == 2 || sx == 3)) {
        fp_axes |= 0b01;
      }
      if (instr.attributes.offset_y != 0.0f && (sy == 2 || sy == 3)) {
        fp_axes |= 0b10;
      }
      // The footprint step uses the guest size of the axis.
      fp_axes &= size_needed;
    }
    const uint32_t fp_scale[2] = {config_.draw_resolution_scale_x,
                                  config_.draw_resolution_scale_y};
    const float fp_offset[2] = {instr.attributes.offset_x,
                                instr.attributes.offset_y};
    std::vector<std::string> fp_xy;
    std::string fp_base, fp_smp, fp_any;
    if (fp_axes) {
      // Colour data only: depth formats (shadow maps the game compares
      // itself) and the single-channel formats it stores depth in keep the
      // point sample of the texel, as a 1x depth texel is.
      std::string fmt_v = NewTemp("fpf");
      Line("uint {} = extract_bits({}, 0, 6);", fmt_v, FetchWord(tf, 1));
      std::string on = NewTemp("fpon");
      Line("bool {} = {} && {} != {}u && {} != {}u && {} != {}u && {} != {}u "
           "&& {} != {}u;",
           on, scaled_cond, fmt_v, uint32_t(xenos::TextureFormat::k_24_8),
           fmt_v, uint32_t(xenos::TextureFormat::k_24_8_FLOAT), fmt_v,
           uint32_t(xenos::TextureFormat::k_16), fmt_v,
           uint32_t(xenos::TextureFormat::k_16_FLOAT), fmt_v,
           uint32_t(xenos::TextureFormat::k_32_FLOAT));
      // Linear when both the magnification and the minification filter are
      // (the instruction's own, or the fetch constant's).
      auto filter = [&](xenos::TextureFilter f, uint32_t shift) {
        return f == xenos::TextureFilter::kUseFetchConst
                   ? fmt::format("extract_bits({}, {}, 2)", FetchWord(tf, 3),
                                 shift)
                   : fmt::format("{}u", uint32_t(f));
      };
      std::string lin = NewTemp("fplin");
      Line("bool {} = {} && {} == {}u && {} == {}u;", lin, on,
           filter(instr.attributes.mag_filter, 19),
           uint32_t(xenos::TextureFilter::kLinear),
           filter(instr.attributes.min_filter, 21),
           uint32_t(xenos::TextureFilter::kLinear));
      // Point on clamp-to-edge in both directions (fetch constant clamp_x,
      // clamp_y), which the in-shader bilinear sampler reproduces.
      std::string cell = NewTemp("fpcell");
      Line("bool {} = {} && !{} && extract_bits({}, 10, 3) == {}u && "
           "extract_bits({}, 13, 3) == {}u;",
           cell, on, lin, FetchWord(tf, 0),
           uint32_t(xenos::ClampMode::kClampToEdge), FetchWord(tf, 0),
           uint32_t(xenos::ClampMode::kClampToEdge));
      // Host texels per normalized unit along each footprint axis.
      std::string fph = NewTemp("fph");
      Line("float2 {} = float2({}, {});", fph,
           (fp_axes & 1) ? fmt::format("{}.x * {}", size, F(float(fp_scale[0])))
                         : std::string("1.0f"),
           (fp_axes & 2) ? fmt::format("{}.y * {}", size, F(float(fp_scale[1])))
                         : std::string("1.0f"));
      fp_base = NewTemp("fpb");
      Line("float2 {} = {}.xy * {};", fp_base, coord, fph);
      Open(fmt::format("if ({})", lin));
      for (uint32_t a = 0; a < 2; ++a) {
        if (!(fp_axes & (1u << a)) || fp_scale[a] != 2) continue;
        const char* c = a ? "y" : "x";
        if (fp_offset[a] > 0.0f) {
          Line("{}.{} = fast::floor({}.{} + {});", fp_base, c, fp_base, c,
               F(1.0f / 64.0f));
        } else {
          Line("{}.{} = fast::ceil({}.{} - {});", fp_base, c, fp_base, c,
               F(1.0f / 64.0f));
        }
      }
      Close();
      Open(fmt::format("else if ({})", cell));
      for (uint32_t a = 0; a < 2; ++a) {
        if (!(fp_axes & (1u << a))) continue;
        const char* c = a ? "y" : "x";
        const float n = float(fp_scale[a]);
        Line("{}.{} = (fast::floor({}.{} * {}) + 0.5f) * {};", fp_base, c,
             fp_base, c, F(1.0f / n), F(n));
      }
      Close();
      // Untouched draws keep the exact original coordinate.
      Line("{} = ({} || {}) ? {} / {} : {}.xy;", fp_base, lin, cell, fp_base,
           fph, coord);
      std::string lin_smp = NewTemp("fpls");
      Line("constexpr sampler {}(coord::normalized, address::clamp_to_edge, "
           "filter::linear, mip_filter::nearest);",
           lin_smp);
      fp_smp = NewTemp("fps");
      Line("sampler {} = {} ? {} : {};", fp_smp, cell, lin_smp, smp_v);
      fp_any = fmt::format("({} || {})", lin, cell);
      // Sample offsets in host texels: one sample at 2x, +-2/3 at 3x.
      std::vector<float> d[2];
      for (uint32_t a = 0; a < 2; ++a) {
        if ((fp_axes & (1u << a)) && fp_scale[a] == 3) {
          d[a] = {-2.0f / 3.0f, 2.0f / 3.0f};
        } else {
          d[a] = {0.0f};
        }
      }
      for (float dy : d[1]) {
        for (float dx : d[0]) {
          if (dx == 0.0f && dy == 0.0f) {
            fp_xy.push_back(fp_base);
          } else {
            fp_xy.push_back(fmt::format("({} + float2({}, {}) / {})", fp_base,
                                        F(dx), F(dy), fph));
          }
        }
      }
    }
    auto sample_avg = [&](uint32_t desc_index,
                          const std::vector<std::string>* xys,
                          const std::string& smp_name) -> std::string {
      if (!xys) {
        return sample_at(desc_index, coord + ".xy", smp_name);
      }
      if (xys->size() == 1) {
        return sample_at(desc_index, xys->front(), smp_name);
      }
      std::string sum;
      for (const std::string& xy : *xys) {
        if (!sum.empty()) sum += " + ";
        sum += sample_at(desc_index, xy, smp_name);
      }
      return fmt::format("(({}) * {})", sum, F(1.0f / float(xys->size())));
    };
    auto emit_samples = [&](const std::vector<std::string>* xys,
                            const std::string& smp_name) {
      Open(fmt::format("if (!({}))", all_signed));
      Line("{}.{} = {}.{};", res, m, sample_avg(tex_u, xys, smp_name), m);
      Close();
      Open(fmt::format("if ({})", any_signed));
      std::string st = NewTemp("st");
      Line("float4 {} = {};", st, sample_avg(tex_s, xys, smp_name));
      Line("{}.{} = select({}, {}, {}).{};", res, m, res, st, is_signed, m);
      Close();
    };
    if (!fp_axes) {
      emit_samples(nullptr, smp_v);
    } else if (fp_xy.size() == 1) {
      // 2x: the moved coordinate (or the original one) and its sampler.
      emit_samples(&fp_xy, fp_smp);
    } else {
      Open(fmt::format("if ({})", fp_any));
      emit_samples(&fp_xy, fp_smp);
      Close();
      Open("else");
      emit_samples(nullptr, smp_v);
      Close();
    }

    // Bias / gamma by sign.
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(nonzero & (1u << i))) continue;
      std::string comp = fmt::format("{}.{}", res, kComp[i]);
      Open(fmt::format("if ({}.{} == {}u)", signs, kComp[i],
                       uint32_t(xenos::TextureSign::kUnsignedBiased)));
      Line("{} = fma({}, {}, {});", comp, comp, F(2.0f), F(-1.0f));
      Close();
      Open(fmt::format("else if ({}.{} == {}u)", signs, kComp[i],
                       uint32_t(xenos::TextureSign::kGamma)));
      PWLGammaToLinear(comp);
      Close();
    }
  }

  if (instr.opcode == FetchOpcode::kTextureFetch) {
    std::string e = NewTemp("ea");
    Line("float {} = as_type<float>(extract_bits(as_type<int>({}), 13, 6) * "
         "{} + as_type<int>(1.0f));",
         e, FetchWord(tf, 3), int32_t(1) << 23);
    std::string m = Mask(nonzero);
    Line("{}.{} = ({} * {}).{};", res, m, res, e, m);
  }
  StoreResult(instr.result, res);
  (void)used_result_components;
}

// ---------------------------------------------------------------------------
// Epilogue
// ---------------------------------------------------------------------------

bool MslShaderTranslator::IsForceEarlyDepthStencil() const {
  return modification().pixel.depth_stencil_mode ==
             Modification::DepthStencilMode::kEarlyHint &&
         current_shader().implicit_early_z_write_allowed();
}

bool MslShaderTranslator::DsvWritingFloat24() const {
  auto mode = modification().pixel.depth_stencil_mode;
  using M = Modification::DepthStencilMode;
  return mode == M::kFloat24Truncating ||
         mode == M::kFloat24TruncatingPolygonOffset ||
         mode == M::kFloat24Rounding ||
         mode == M::kFloat24RoundingPolygonOffset;
}

bool MslShaderTranslator::DsvApplyingPolygonOffset() const {
  auto mode = modification().pixel.depth_stencil_mode;
  using M = Modification::DepthStencilMode;
  return mode == M::kPolygonOffset ||
         mode == M::kFloat24TruncatingPolygonOffset ||
         mode == M::kFloat24RoundingPolygonOffset;
}

void MslShaderTranslator::EmitAlphaToMaskSample(bool initialize,
                                                uint32_t sample_index,
                                                float threshold_base,
                                                const std::string& offset,
                                                float offset_scale,
                                                const std::string& coverage) {
  std::string t = NewTemp("a2m");
  Line("uint {} = (xe_oc0.w >= fma({}, {}, {})) ? 0xFFFFFFFFu : 0u;", t, offset,
       F(-offset_scale), F(threshold_base));
  if (initialize) {
    Line("{} = {} & {};", coverage, t, U(uint32_t(1) << sample_index));
  } else {
    Line("{} = {} | ({} & {});", coverage, coverage, t,
         U(uint32_t(1) << sample_index));
  }
}

void MslShaderTranslator::CompletePixelShader() {
  const Shader& shader = current_shader();
  Modification m = modification();
  const size_t flags_dword = offsetof(SystemConstants, flags) / 4;
  if (shader.writes_color_target(0) && !IsForceEarlyDepthStencil()) {
    // Alpha test.
    Line("uint xe_at_mask = extract_bits(xe_sys[{}], {}, 3);", flags_dword,
         uint32_t(DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift));
    Open(fmt::format("if (xe_at_mask != {}u)",
                     uint32_t(xenos::CompareFunction::kAlways)));
    std::string ref = SysF(offsetof(SystemConstants, alpha_test_reference));
    Open(fmt::format("if (xe_at_mask == {}u)",
                     uint32_t(xenos::CompareFunction::kNotEqual)));
    if (config_.use_fuzzy_alpha_epsilon) {
      Line("xe_at_mask = (abs(xe_oc0.w + (-{})) < {}) ? 0u : 0xFFFFFFFFu;", ref,
           F(1e-3f));
    } else {
      Line("xe_at_mask = (xe_oc0.w != {}) ? 0xFFFFFFFFu : 0u;", ref);
    }
    Close();
    Open("else");
    if (config_.use_fuzzy_alpha_epsilon) {
      Line("xe_at_mask &= ((xe_oc0.w + (-{})) < {}) ? 0xFFFFFFFFu : ~1u;",
           F(1e-3f), ref);
      Line("xe_at_mask &= (abs(xe_oc0.w + (-{})) < {}) ? 0xFFFFFFFFu : ~2u;",
           ref, F(1e-3f));
      Line("xe_at_mask &= ({} < (xe_oc0.w + {})) ? 0xFFFFFFFFu : ~4u;", ref,
           F(1e-3f));
    } else {
      Line("xe_at_mask &= (xe_oc0.w < {}) ? 0xFFFFFFFFu : ~1u;", ref);
      Line("xe_at_mask &= (xe_oc0.w == {}) ? 0xFFFFFFFFu : ~2u;", ref);
      Line("xe_at_mask &= ({} < xe_oc0.w) ? 0xFFFFFFFFu : ~4u;", ref);
    }
    Close();
    Line("if (xe_at_mask == 0u) {{ discard_fragment(); }}");
    Close();

    // Alpha to coverage (non-ROV).
    uses_sample_mask_out_ = true;
    Line("xe_out.mask = 0xFFFFFFFFu;");
    std::string a2m_const = SysU(offsetof(SystemConstants, alpha_to_mask));
    Open(fmt::format("if ({} != 0u)", a2m_const));
    uses_position_ = true;
    Line("uint2 xe_a2m_p = uint2(xe_in.pos.xy);");
    Line("uint xe_a2m_i = insert_bits(xe_a2m_p.y & 1u, xe_a2m_p.x, 1u, 1u) << 1;");
    Line("float xe_a2m_o = float(extract_bits({}, xe_a2m_i, 2));", a2m_const);
    Line("uint xe_a2m_cov = 0u;");
    size_t sc = offsetof(SystemConstants, sample_count_log2);
    Open(fmt::format("if ({} != 0u)", SysU(sc + 4)));
    Open(fmt::format("if ({} != 0u)", SysU(sc)));
    EmitAlphaToMaskSample(true, 0, 0.75f, "xe_a2m_o", 1.0f / 16.0f,
                          "xe_a2m_cov");
    EmitAlphaToMaskSample(false, 1, 0.25f, "xe_a2m_o", 1.0f / 16.0f,
                          "xe_a2m_cov");
    EmitAlphaToMaskSample(false, 2, 0.5f, "xe_a2m_o", 1.0f / 16.0f,
                          "xe_a2m_cov");
    EmitAlphaToMaskSample(false, 3, 1.0f, "xe_a2m_o", 1.0f / 16.0f,
                          "xe_a2m_cov");
    Close();
    Open("else");
    EmitAlphaToMaskSample(true, config_.msaa_2x_supported ? 1 : 0, 0.5f,
                          "xe_a2m_o", 1.0f / 8.0f, "xe_a2m_cov");
    EmitAlphaToMaskSample(false, config_.msaa_2x_supported ? 0 : 3, 1.0f,
                          "xe_a2m_o", 1.0f / 8.0f, "xe_a2m_cov");
    Close();
    Close();
    Open("else");
    EmitAlphaToMaskSample(true, 0, 1.0f, "xe_a2m_o", 1.0f / 4.0f,
                          "xe_a2m_cov");
    Close();
    Line("if (xe_a2m_cov == 0u) {{ discard_fragment(); }}");
    Line("xe_out.mask = xe_a2m_cov;");
    Close();
  }

  // Color outputs.
  uint32_t writes_color = shader.writes_color_targets();
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(writes_color & (1u << i))) continue;
    std::string c = fmt::format("xe_oc{}", i);
    Line("{} = {} * {};", c, c,
         SysF(offsetof(SystemConstants, color_exp_bias) + sizeof(float) * i));
    if (config_.gamma_render_target_as_unorm8) {
      Open(fmt::format(
          "if ((xe_sys[{}] & {}) != 0u)", flags_dword,
          U(uint32_t(DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma)
            << i)));
      Line("{}.xyz = fast::saturate({}.xyz);", c, c);
      for (uint32_t j = 0; j < 3; ++j) {
        PreSaturatedLinearToPWLGamma(fmt::format("{}.{}", c, kComp[j]));
      }
      Close();
    }
    if (i == 0) {
      xenos::BlendFactor rgb = m.pixel.rt0_blend_rgb_factor_for_premult;
      xenos::BlendFactor alpha = m.pixel.rt0_blend_a_factor_for_premult;
      std::string bc = SysF(offsetof(SystemConstants, edram_blend_constant));
      std::string bc4 = fmt::format(
          "float4({}, {}, {}, {})", bc,
          SysF(offsetof(SystemConstants, edram_blend_constant) + 4),
          SysF(offsetof(SystemConstants, edram_blend_constant) + 8),
          SysF(offsetof(SystemConstants, edram_blend_constant) + 12));
      std::string bca =
          SysF(offsetof(SystemConstants, edram_blend_constant) + 12);
      if (rgb != xenos::BlendFactor::kOne) {
        switch (rgb) {
          case xenos::BlendFactor::kZero:
            Line("{}.xyz = float3(0.0f);", c);
            break;
          case xenos::BlendFactor::kSrcColor:
            Line("{}.xyz = {}.xyz * {}.xyz;", c, c, c);
            break;
          case xenos::BlendFactor::kOneMinusSrcColor:
            Line("{}.xyz = {}.xyz * (1.0f + (-{}.xyz));", c, c, c);
            break;
          case xenos::BlendFactor::kSrcAlpha:
            Line("{}.xyz = {}.xyz * {}.w;", c, c, c);
            break;
          case xenos::BlendFactor::kOneMinusSrcAlpha:
            Line("{}.xyz = {}.xyz * (1.0f + (-{}.w));", c, c, c);
            break;
          case xenos::BlendFactor::kConstantColor:
            Line("{}.xyz = {}.xyz * {}.xyz;", c, c, bc4);
            break;
          case xenos::BlendFactor::kOneMinusConstantColor:
            Line("{}.xyz = {}.xyz * (1.0f + (-{}.xyz));", c, c, bc4);
            break;
          case xenos::BlendFactor::kConstantAlpha:
            Line("{}.xyz = {}.xyz * {};", c, c, bca);
            break;
          case xenos::BlendFactor::kOneMinusConstantAlpha:
            Line("{}.xyz = {}.xyz * (1.0f + (-{}));", c, c, bca);
            break;
          default:
            break;
        }
      }
      if (alpha != xenos::BlendFactor::kOne) {
        switch (alpha) {
          case xenos::BlendFactor::kZero:
            Line("{}.w = 0.0f;", c);
            break;
          case xenos::BlendFactor::kSrcColor:
          case xenos::BlendFactor::kSrcAlpha:
            Line("{}.w = {}.w * {}.w;", c, c, c);
            break;
          case xenos::BlendFactor::kOneMinusSrcColor:
          case xenos::BlendFactor::kOneMinusSrcAlpha:
            Line("{}.w = {}.w * (1.0f + (-{}.w));", c, c, c);
            break;
          case xenos::BlendFactor::kConstantColor:
          case xenos::BlendFactor::kConstantAlpha:
            Line("{}.w = {}.w * {};", c, c, bca);
            break;
          case xenos::BlendFactor::kOneMinusConstantColor:
          case xenos::BlendFactor::kOneMinusConstantAlpha:
            Line("{}.w = {}.w * (1.0f + (-{}));", c, c, bca);
            break;
          default:
            break;
        }
      }
    }
    Line("xe_out.c{} = {};", i, c);
  }

  if (DsvWritingFloat24()) {
    // CompletePixelShader_DSV_DepthTo24Bit (non-ROV), operation for operation.
    using M = Modification::DepthStencilMode;
    const bool truncating =
        modification().pixel.depth_stencil_mode == M::kFloat24Truncating;
    writes_depth_out_ = true;
    // SV_DepthLessEqual for truncation without a shader depth write.
    depth_out_less_equal_ = truncating && !shader.writes_depth();
    if (shader.writes_depth()) {
      // Already saturated by the oDepth store.
      Line("float xe_d24 = xe_odepth;");
    } else {
      // Host 0...0.5 back to guest 0...1, saturated (mul_sat).
      Line("float xe_d24 = fast::saturate(xe_in.pos.z * 2.0f);");
    }
    if (truncating) {
      // Drop the low mantissa bits (3 for normal 20e4, up to 23 for the
      // smallest denormal); below 2^-34, zero.
      Line("uint xe_d24b = as_type<uint>(xe_d24);");
      Open("if (xe_d24b >= 0x2E800000u)");
      Line("int xe_d24s = max(116 - int(extract_bits(xe_d24b, 23u, 8u)), 3);");
      Line("xe_d24b = insert_bits(xe_d24b, 0u, 0u, uint(xe_d24s) & 31u);");
      Line("xe_out.depth = as_type<float>(xe_d24b) * 0.5f;");
      Close();
      Open("else");
      Line("xe_out.depth = 0.0f;");
      Close();
    } else {
      // PreClampedDepthTo20e4 (round to nearest even, no remap), then
      // Depth20e4To32 (remap to 0...0.5).
      Line("uint xe_f32 = as_type<uint>(xe_d24);");
      Line("uint xe_f24;");
      Open("if (xe_f32 < 0x38800000u)");
      Line("uint xe_sh = min(uint(113 - int(xe_f32 >> 23u)), 24u);");
      Line("xe_f24 = ((xe_f32 & 0x7FFFFFu) | 0x800000u) >> xe_sh;");
      Close();
      Open("else");
      Line("xe_f24 = xe_f32 + 0xC8000000u;");
      Close();
      Line("xe_f24 = xe_f24 + 3u + extract_bits(xe_f24, 3u, 1u);");
      Line("xe_f24 = extract_bits(xe_f24, 3u, 24u);");
      Line("int xe_e24 = int(extract_bits(xe_f24, 20u, 4u));");
      Line("uint xe_m24 = extract_bits(xe_f24, 0u, 20u);");
      Open("if (xe_e24 == 0)");
      Open("if (xe_m24 != 0u)");
      Line("xe_e24 = int(clz(xe_m24)) + (20 - 31);");
      Line("xe_m24 = xe_m24 << (uint(xe_e24) & 31u);");
      Line("xe_e24 = 1 - xe_e24;");
      Close();
      Open("else");
      Line("xe_e24 = -111;");
      Close();
      Close();
      Line("uint xe_d24r = uint(xe_e24) * 0x800000u + (111u << 23u);");
      Line("xe_out.depth = as_type<float>(insert_bits(xe_d24r, xe_m24, 3u, 20u));");
    }
  } else if (shader.writes_depth()) {
    // Not converting to float24.
    writes_depth_out_ = true;
    Open(fmt::format("if ((xe_sys[{}] & {}) != 0u)", flags_dword,
                     U(DxbcShaderTranslator::kSysFlag_DepthFloat24)));
    Line("xe_odepth = xe_odepth * 0.5f;");
    Close();
    Line("xe_out.depth = xe_odepth;");
  }
}

std::vector<uint8_t> MslShaderTranslator::CompleteTranslation() {
  // Close the guest program.
  CloseExecConditionals();
  if (jump_flags_mode_) {
    CloseFlagGuard();
  }
  while (!jump_skip_targets_.empty()) {
    Close();
    jump_skip_targets_.pop_back();
  }
  if (!loops_.empty()) {
    Fail("unterminated loop");
  }
  --indent_;
  --open_blocks_;
  Line("}} while (false);");
  if (open_blocks_) {
    Fail("internal: unclosed blocks");
  }
  if (is_vertex_shader()) {
    if (uint8_t eM =
            current_shader().memexport_eM_potentially_written_before_end()) {
      ExportToMemory(eM);
    }
    CompleteVertexShader();
  } else {
    CompletePixelShader();
  }
  if (failed()) {
    return {};
  }

  const Shader& shader = current_shader();
  Modification m = modification();
  std::string s;
  s += "// Fable II native Xenos->MSL translation (metal_msl_translator.cc)\n";
  s += "#pragma METAL fp math_mode(safe)\n";
  s += "#pragma METAL fp contract(off)\n";
  s += "#include <metal_stdlib>\nusing namespace metal;\n";
  // DXBC frc as the converter's code computes it: x - floor(x) (NaN for
  // +-Infinity). MSL fract() compiled from source returns 0 there.
  s += "template <typename T> static inline T xe_frc(T x) { return x - "
       "fast::floor(x); }\n";
  s += "struct XeTex2DA { uint64_t va; texture2d_array<float> t; uint64_t meta; };\n";
  s += "struct XeTex3D { uint64_t va; texture3d<float> t; uint64_t meta; };\n";
  s += "struct XeTexCube { uint64_t va; texturecube<float> t; uint64_t meta; };\n";
  s += "struct XeSmp { sampler s; uint64_t a; uint64_t b; };\n";
  // Raw buffer descriptor (shared memory): address, view, size in bytes.
  s += "struct XeBuf { device uint* va; uint64_t view; uint64_t size; };\n";
  s += "struct XeTop;\n";
  s += "struct XeTop {\n";
  for (uint32_t i = 0; i < kSlotCount; ++i) {
    switch (i) {
      case kSlotSRVSpace1:
        s += "  constant XeTex2DA* t2d;\n";
        break;
      case kSlotSRVSpace2:
        s += "  constant XeTex3D* t3d;\n";
        break;
      case kSlotSRVSpace3:
        s += "  constant XeTexCube* tcube;\n";
        break;
      case kSlotSampler:
        s += "  constant XeSmp* smp;\n";
        break;
      case kSlotCBVSystem:
        s += "  constant uint* sys;\n";
        break;
      case kSlotCBVBoolLoop:
        s += "  constant uint* bool_loop;\n";
        break;
      case kSlotCBVPixelFloat:
        s += "  constant float4* ps_float;\n";
        break;
      case kSlotCBVPixelFetch:
        s += "  constant uint* ps_fetch;\n";
        break;
      case kSlotCBVPixelDescriptorIndices:
        s += "  constant uint* ps_desc;\n";
        break;
      case kSlotSRVSpace0:
        s += "  constant XeBuf* srv0;\n";
        break;
      case kSlotUAVSpace0:
        s += "  constant XeBuf* uav0;\n";
        break;
      case kSlotCBVVertexFloat:
        s += "  constant float4* vs_float;\n";
        break;
      case kSlotCBVVertexFetch:
        s += "  constant uint* vs_fetch;\n";
        break;
      case kSlotCBVVertexDescriptorIndices:
        s += "  constant uint* vs_desc;\n";
        break;
      default:
        s += fmt::format("  uint64_t unused{};\n", i);
        break;
    }
  }
  s += "};\n";
  // Bounds-checked dword load from the shared memory (out of range reads 0,
  // like the converter's code).
  s += "static inline uint xe_shared_load(constant XeBuf* b, uint byte_address) "
       "{ uint i = byte_address >> 2u; return (i < (uint(b[0].size) >> 2u)) ? "
       "b[0].va[i] : 0u; }\n";
  // DXBC f32tof16 as the converter's code computes it: round toward zero
  // (MSL's half() conversion rounds to nearest even).
  s += "static inline uint4 xe_f32_to_f16(float4 x) { uint4 f = as_type<uint4>(x); "
       "uint4 sg = (f >> 16u) & 0x8000u; uint4 a = f & 0x7FFFFFFFu; uint4 e = a >> 23u; "
       "uint4 m = (a & 0x7FFFFFu) | 0x800000u; "
       "uint4 nrm = ((e - 112u) << 10u) | ((a >> 13u) & 0x3FFu); "
       "uint4 den = m >> min(126u - e, 31u); "
       "uint4 r = select(select(nrm, den, e < 113u), 0u, e < 103u); "
       "r = select(r, uint4(0x7BFFu), e >= 143u && a < 0x7F800000u); "
       "r = select(r, uint4(0x7C00u), a == 0x7F800000u); "
       "r = select(r, 0x7C00u | 0x200u | ((a >> 13u) & 0x3FFu), a > 0x7F800000u); "
       "return sg | r; }\n";
  // Memory export (bounds-checked like the loads; out of range is dropped).
  s += "static inline void xe_shared_store(constant XeBuf* b, uint byte_address, "
       "uint v) { uint i = byte_address >> 2u; if (i < (uint(b[0].size) >> 2u)) "
       "b[0].va[i] = v; }\n";
  s += "static inline void xe_shared_and(constant XeBuf* b, uint byte_address, uint "
       "v) { uint i = byte_address >> 2u; if (i < (uint(b[0].size) >> 2u)) "
       "atomic_fetch_and_explicit((device atomic_uint*)(b[0].va + i), v, "
       "memory_order_relaxed); }\n";
  s += "static inline void xe_shared_or(constant XeBuf* b, uint byte_address, uint "
       "v) { uint i = byte_address >> 2u; if (i < (uint(b[0].size) >> 2u)) "
       "atomic_fetch_or_explicit((device atomic_uint*)(b[0].va + i), v, "
       "memory_order_relaxed); }\n";
  if (is_vertex_shader()) {
    uint32_t count = rex::bit_count(m.vertex.interpolator_mask);
    s += "struct XeVOut {\n";
    for (uint32_t i = 0; i < count; ++i) {
      s += fmt::format("  float4 o{} [[user(texcoord{})]];\n", i, i);
    }
    s += "  float4 pos [[position, invariant]];\n";
    if ((config_.expand_points || config_.expand_rects) &&
        config_.point_coordinates) {
      s += "  float2 point_coord [[user(xespritetexcoord0)]];\n";
    }
    uint32_t clips = m.vertex.user_clip_plane_count;
    if (clips) {
      s += fmt::format("  float clip [[clip_distance]] [{}];\n", clips);
    }
    s += "};\n";
    const bool domain_quad =
        m.vertex.host_vertex_shader_type ==
        Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
    if (domain_quad) {
      // Post-tessellation vertex function (FABLE2_NATIVE_TESS). The patch
      // index is the adaptive hull shader's control-point phase:
      // (primitive + vertex_index_offset) & 0xFFFFFF, clamped to the index
      // min/max, as a float.
      // XeTessDraw at buffer(3): continuous/discrete draws (indexed = 1)
      // take the patch index from the guest index buffer (or the patch when
      // auto-indexed) through tessellation_indexed_vs: endian swap, offset,
      // clamp. Adaptive draws (indexed = 0) use the primitive, as
      // adaptive_quad_hs's control-point phase does.
      s += "struct XeTessDraw { device const uchar* index; uint format; uint "
           "indexed; };\n";
      if (config_.domain_cpu_pattern) {
        // Vertex id = (patch << 20) | domain point in the uv arena at
        // buffer(4); the patch adds to the instance (runs of patches drawn
        // instanced use patch 0 and the instance).
        s += "vertex XeVOut xe_native_ds_cpu(uint xe_vid [[vertex_id]], uint "
             "xe_inst [[instance_id]], constant XeTop& xe_tl [[buffer(2)]], "
             "constant XeTessDraw& xe_td [[buffer(3)]], device const float2* "
             "xe_tv [[buffer(4)]]) {\n";
        s += "  float2 xe_uv = xe_tv[xe_vid & 0xFFFFFu];\n";
        s += "  uint xe_patch_id = xe_inst + (xe_vid >> 20u);\n";
      } else {
        if (config_.snap_domain_uv) {
          s += "[[patch(quad)]] vertex XeVOut xe_native_ds(uint xe_patch_id "
               "[[patch_id]], float2 xe_uv_hw [[position_in_patch]], constant "
               "XeTop& xe_tl [[buffer(2)]], constant XeTessDraw& xe_td "
               "[[buffer(3)]]) {\n";
          s += "  float2 xe_uv = rint(xe_uv_hw * 65536.0f) * "
               "0.0000152587890625f;\n";
        } else {
          s += "[[patch(quad)]] vertex XeVOut xe_native_ds(uint xe_patch_id "
               "[[patch_id]], float2 xe_uv [[position_in_patch]], constant "
               "XeTop& xe_tl [[buffer(2)]], constant XeTessDraw& xe_td "
               "[[buffer(3)]]) {\n";
        }
      }
      s += "  uint xe_pi = xe_patch_id;\n";
      s += "  if (xe_td.format == 1u) { xe_pi = uint(((device const "
           "ushort*)xe_td.index)[xe_patch_id]); }\n";
      s += "  else if (xe_td.format == 2u) { xe_pi = ((device const "
           "uint*)xe_td.index)[xe_patch_id]; }\n";
      {
        std::string endian = fmt::format(
            "xe_tl.sys[{}]", offsetof(SystemConstants, vertex_index_endian) / 4);
        s += fmt::format(
            "  if (xe_td.indexed != 0u && ({} == {}u || {} == {}u)) {{ xe_pi = "
            "((xe_pi & 0x00FF00FFu) * 256u) + ((xe_pi >> 8u) & 0x00FF00FFu); "
            "}}\n",
            endian, uint32_t(xenos::Endian::k8in16), endian,
            uint32_t(xenos::Endian::k8in32));
        s += fmt::format(
            "  if (xe_td.indexed != 0u && ({} == {}u || {} == {}u)) {{ xe_pi = "
            "(xe_pi >> 16u) | (xe_pi << 16u); }}\n",
            endian, uint32_t(xenos::Endian::k8in32), endian,
            uint32_t(xenos::Endian::k16in32));
      }
      s += fmt::format(
          "  xe_pi = (xe_pi + xe_tl.sys[{}]) & 0x00FFFFFFu;\n",
          offsetof(SystemConstants, vertex_index_offset) / 4);
      s += fmt::format(
          "  xe_pi = min(max(xe_pi, xe_tl.sys[{}]), xe_tl.sys[{}]);\n",
          offsetof(SystemConstants, vertex_index_min) / 4,
          offsetof(SystemConstants, vertex_index_max) / 4);
      s += "  float xe_patch_index = float(xe_pi);\n";
    } else if (config_.expand_points) {
      s += "struct XePointDraw { device const uchar* index; uint format; uint "
           "pad; };\n";
      s += "vertex XeVOut xe_native_vs_points(uint xe_corner [[vertex_id]], "
           "uint xe_point [[instance_id]], constant XeTop& xe_tl [[buffer(2)]], "
           "constant XePointDraw& xe_pd [[buffer(3)]]) {\n";
      // The guest vertex index: the point, or the index buffer's entry for it
      // (the prologue swaps and offsets it as for any draw).
      s += "  uint xe_vertex_id = xe_point;\n  uint xe_base_vertex = 0u;\n";
      s += "  if (xe_pd.format == 1u) { xe_vertex_id = uint(((device const "
           "ushort*)xe_pd.index)[xe_point]); }\n";
      s += "  else if (xe_pd.format == 2u) { xe_vertex_id = ((device const "
           "uint*)xe_pd.index)[xe_point]; }\n";
    } else if (config_.expand_rects) {
      s += "static XeVOut xe_vs_body(uint xe_vertex_id, uint xe_base_vertex, "
           "constant XeTop& xe_tl) {\n";
    } else if (config_.points_mesh) {
      s += "static XeVOut xe_vs_body(uint xe_vertex_id, uint xe_base_vertex, "
           "constant XeTop& xe_tl, thread float2& xe_pm_rad, thread bool& "
           "xe_pm_kill) {\n";
    } else {
      s += "vertex XeVOut xe_native_vs(uint xe_vertex_id [[vertex_id]], uint "
           "xe_base_vertex [[base_vertex]], constant XeTop& xe_tl [[buffer(2)]]) {\n";
    }
    s += "  constant uint* xe_sys = xe_tl.sys;\n";
    s += "  constant uint* xe_bool_loop = xe_tl.bool_loop;\n";
    s += "  constant float4* xe_float = xe_tl.vs_float;\n";
    s += "  constant uint* xe_fetch = xe_tl.vs_fetch;\n";
    s += "  constant uint* xe_desc = xe_tl.vs_desc;\n";
    s += "  (void)xe_sys; (void)xe_bool_loop; (void)xe_float; (void)xe_fetch; "
         "(void)xe_desc;\n";
    for (uint32_t t : declared_flag_targets_) {
      s += fmt::format("  bool xe_j{} = false;\n", t);
    }
    s += "  XeVOut xe_out;\n";
    s += body_;
    s += "  return xe_out;\n}\n";
    if (config_.points_mesh && !domain_quad) {
      // A mesh function alone (no object stage): 64 points per threadgroup,
      // each thread runs the guest VS once for its point and emits the
      // sprite's corners TL, TR, BL, BR (the geometry shader's strip) as
      // triangles (0, 1, 2), (2, 1, 3).
      s += "struct XePointDraw { device const uchar* index; uint format; uint "
           "count; };\n";
      s += "struct XePmVert {\n";
      for (uint32_t i = 0; i < count; ++i) {
        s += fmt::format("  float4 o{} [[user(texcoord{})]];\n", i, i);
      }
      s += "  float4 pos [[position, invariant]];\n";
      if (config_.point_coordinates) {
        s += "  float2 point_coord [[user(xespritetexcoord0)]];\n";
      }
      s += "};\n";
      s += "using XePmMesh = metal::mesh<XePmVert, void, 256, 128, "
           "metal::topology::triangle>;\n";
      s += "[[mesh]] void xe_native_ms_points(XePmMesh xe_m, uint xe_t "
           "[[thread_index_in_threadgroup]], uint xe_g "
           "[[threadgroup_position_in_grid]], constant XeTop& xe_tl "
           "[[buffer(2)]], constant XePointDraw& xe_pd [[buffer(3)]]) {\n";
      s += "  if (xe_t == 0u) { xe_m.set_primitive_count(128u); }\n";
      s += "  uint xe_point = xe_g * 64u + xe_t;\n";
      s += "  XeVOut xe_v;\n  float2 xe_rad = float2(0.0f);\n  bool xe_kill = "
           "true;\n";
      s += "  if (xe_point < xe_pd.count) {\n";
      s += "    uint xe_id = xe_point;\n";
      s += "    if (xe_pd.format == 1u) { xe_id = uint(((device const "
           "ushort*)xe_pd.index)[xe_point]); }\n";
      s += "    else if (xe_pd.format == 2u) { xe_id = ((device const "
           "uint*)xe_pd.index)[xe_point]; }\n";
      s += "    xe_v = xe_vs_body(xe_id, 0u, xe_tl, xe_rad, xe_kill);\n  }\n";
      s += "  for (uint c = 0u; c < 4u; ++c) {\n";
      s += "    XePmVert xe_o;\n";
      for (uint32_t i = 0; i < count; ++i) {
        s += fmt::format("    xe_o.o{} = xe_v.o{};\n", i, i);
      }
      s += "    float4 xe_pp = xe_v.pos;\n";
      s += "    xe_o.pos = xe_pp;\n";
      s += "    xe_o.pos.x = xe_pp.x + (((c & 1u) != 0u) ? xe_rad.x : -xe_rad.x);\n";
      s += "    xe_o.pos.y = xe_pp.y + (((c >> 1u) != 0u) ? -xe_rad.y : "
           "xe_rad.y);\n";
      if (config_.point_coordinates) {
        s += "    xe_o.point_coord = float2(float(c & 1u), float(c >> 1u));\n";
      }
      s += "    if (xe_kill) { xe_o.pos = float4(0.0f); }\n";
      s += "    xe_m.set_vertex(xe_t * 4u + c, xe_o);\n  }\n";
      s += "  uint b = xe_t * 4u, i0 = xe_t * 6u;\n";
      s += "  xe_m.set_index(i0 + 0u, b + 0u); xe_m.set_index(i0 + 1u, b + 1u); "
           "xe_m.set_index(i0 + 2u, b + 2u);\n";
      s += "  xe_m.set_index(i0 + 3u, b + 2u); xe_m.set_index(i0 + 4u, b + 1u); "
           "xe_m.set_index(i0 + 5u, b + 3u);\n}\n";
    }
    if (config_.expand_rects && !domain_quad) {
      // dxbc_geometry_shader.cc, kRectangleList, in the same operation order.
      s += "struct XePointDraw { device const uchar* index; uint format; uint "
           "pad; };\n";
      s += "vertex XeVOut xe_native_vs_rects(uint xe_corner [[vertex_id]], uint "
           "xe_rect [[instance_id]], constant XeTop& xe_tl [[buffer(2)]], "
           "constant XePointDraw& xe_pd [[buffer(3)]]) {\n";
      s += "  XeVOut xe_v[3];\n";
      s += "  for (uint k = 0u; k < 3u; ++k) {\n";
      s += "    uint xe_i = xe_rect * 3u + k;\n    uint xe_id = xe_i;\n";
      s += "    if (xe_pd.format == 1u) { xe_id = uint(((device const "
           "ushort*)xe_pd.index)[xe_i]); }\n";
      s += "    else if (xe_pd.format == 2u) { xe_id = ((device const "
           "uint*)xe_pd.index)[xe_i]; }\n";
      s += "    xe_v[k] = xe_vs_body(xe_id, 0u, xe_tl);\n  }\n";
      s += "  bool xe_kill = any(xe_v[0].pos != xe_v[0].pos) || any(xe_v[1].pos "
           "!= xe_v[1].pos) || any(xe_v[2].pos != xe_v[2].pos);\n";
      // Squared edge lengths (dp2) of 12, 20, 01.
      s += "  float2 xe_e12 = xe_v[2].pos.xy + (-xe_v[1].pos.xy);\n";
      s += "  float xe_l12 = xe_e12.x * xe_e12.x + xe_e12.y * xe_e12.y;\n";
      s += "  float2 xe_e20 = xe_v[0].pos.xy + (-xe_v[2].pos.xy);\n";
      s += "  float xe_l20 = xe_e20.x * xe_e20.x + xe_e20.y * xe_e20.y;\n";
      s += "  float2 xe_e01 = xe_v[1].pos.xy + (-xe_v[0].pos.xy);\n";
      s += "  float xe_l01 = xe_e01.x * xe_e01.x + xe_e01.y * xe_e01.y;\n";
      s += "  uint3 xe_o;\n";
      s += "  if ((xe_l20 < xe_l12) && (xe_l01 < xe_l12)) { xe_o = uint3(0u, 1u, "
           "2u); }\n";
      s += "  else { xe_o = (xe_l01 < xe_l20) ? uint3(1u, 2u, 0u) : uint3(2u, 0u, "
           "1u); }\n";
      s += "  XeVOut xe_r;\n";
      s += "  if (xe_corner < 3u) { xe_r = xe_v[xe_o[xe_corner]]; }\n";
      s += "  else {\n";
      s += "    XeVOut xa = xe_v[xe_o.x], xb = xe_v[xe_o.y], xc = xe_v[xe_o.z];\n";
      for (uint32_t i = 0; i < count; ++i) {
        s += fmt::format("    xe_r.o{} = (-xa.o{} + xb.o{}) + xc.o{};\n", i, i,
                         i, i);
      }
      s += "    xe_r.pos = (-xa.pos + xb.pos) + xc.pos;\n";
      for (uint32_t i = 0; i < clips; ++i) {
        s += fmt::format(
            "    xe_r.clip[{}] = (-xa.clip[{}] + xb.clip[{}]) + xc.clip[{}];\n",
            i, i, i, i);
      }
      s += "  }\n";
      if (config_.point_coordinates) {
        s += "  xe_r.point_coord = float2(0.0f);\n";
      }
      s += "  if (xe_kill) { xe_r.pos = float4(0.0f); }\n";
      s += "  return xe_r;\n}\n";
    }
    return std::vector<uint8_t>(s.begin(), s.end());
  }
  // Inputs: packed interpolators (TEXCOORDn), position, front face.
  s += "struct XeIn {\n";
  uint32_t interpolator_mask = m.pixel.interpolator_mask;
  uint32_t packed = 0;
  for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
    if (!(interpolator_mask & (1u << i))) continue;
    bool centroid = (m.pixel.interpolators_centroid >> i) & 1;
    s += fmt::format("  float4 v{} [[user(texcoord{}){}]];\n", packed, packed,
                     centroid ? ", centroid_perspective" : "");
    ++packed;
  }
  if (uses_point_coord_) {
    s += "  float2 point_coord [[user(xespritetexcoord0)]];\n";
  }
  if (uses_position_) {
    s += "  float4 pos [[position]];\n";
  }
  s += "};\n";
  s += "struct XeOut {\n";
  bool any_out = false;
  for (uint32_t i = 0; i < 4; ++i) {
    if (shader.writes_color_target(i)) {
      s += fmt::format("  float4 c{} [[color({})]];\n", i, i);
      any_out = true;
    }
  }
  if (writes_depth_out_) {
    s += depth_out_less_equal_ ? "  float depth [[depth(less)]];\n"
                               : "  float depth [[depth(any)]];\n";
    any_out = true;
  }
  if (uses_sample_mask_out_) {
    s += "  uint mask [[sample_mask]];\n";
    any_out = true;
  }
  s += "};\n";
  if (IsForceEarlyDepthStencil()) {
    s += "[[early_fragment_tests]] ";
  }
  s += fmt::format("fragment {} xe_native_ps(XeIn xe_in [[stage_in]]",
                   any_out ? "XeOut" : "void");
  if (uses_front_face_) {
    s += ", bool xe_front_face [[front_facing]]";
  }
  s += ", constant XeTop& xe_tl [[buffer(2)]]) {\n";
  s += "  constant uint* xe_sys = xe_tl.sys;\n";
  s += "  constant uint* xe_bool_loop = xe_tl.bool_loop;\n";
  s += "  constant float4* xe_float = xe_tl.ps_float;\n";
  s += "  constant uint* xe_fetch = xe_tl.ps_fetch;\n";
  s += "  constant uint* xe_desc = xe_tl.ps_desc;\n";
  s += "  (void)xe_sys; (void)xe_bool_loop; (void)xe_float; (void)xe_fetch; "
       "(void)xe_desc;\n";
  for (uint32_t t : declared_flag_targets_) {
    s += fmt::format("  bool xe_j{} = false;\n", t);
  }
  if (any_out) {
    s += "  XeOut xe_out;\n";
  }
  // front_face is referenced as xe_in.front_face in the body.
  std::string body = body_;
  for (size_t pos = 0;
       (pos = body.find("xe_in.front_face", pos)) != std::string::npos;) {
    body.replace(pos, 16, "xe_front_face");
  }
  s += body;
  if (any_out) {
    s += "  return xe_out;\n";
  }
  s += "}\n";
  (void)shader;
  return std::vector<uint8_t>(s.begin(), s.end());
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
