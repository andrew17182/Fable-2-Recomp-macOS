// [mip-lod] Shader analysis for the per-draw mip-level proof
// (metal_miplod_proof.h, docs/mip-lod-20261013.md).

#include "rex/graphics/metal/metal_miplod_proof.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "rex/graphics/pipeline/shader/translator.h"

namespace rex {
namespace graphics {
namespace metal {
namespace miplod {

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_MIPLOD_PROOF");  // getenv-ok: once
    return !(v && v[0] == '0');
  }();
  return on;
}

bool TraceEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_MIPLOD_TRACE");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  return on;
}

namespace {

using ucode::AluVectorOpcode;
using ucode::FetchOpcode;

bool IsFloat32Format(xenos::VertexFormat f, uint32_t& count) {
  switch (f) {
    case xenos::VertexFormat::k_32_FLOAT:
      count = 1;
      return true;
    case xenos::VertexFormat::k_32_32_FLOAT:
      count = 2;
      return true;
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      count = 3;
      return true;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      count = 4;
      return true;
    default:
      return false;
  }
}

// Follows straight-line code and records, per register component, where its
// value comes from. Anything it does not understand makes the value unknown;
// control flow it does not understand fails the whole shader.
class Analyzer : public ShaderTranslator {
 public:
  bool Run(Shader& shader) {
    struct LocalTranslation : public Shader::Translation {
      LocalTranslation(Shader& s) : Translation(s, 0) {}
    } translation(shader);
    TranslateAnalyzedShader(translation);
    return !failed_;
  }
  bool failed_ = false;
  const char* reason_ = "";
  Src regs_[64][4];
  Src pos_[4];
  Src interp_[16][4];
  std::vector<PsFetch> fetches_;
  uint32_t unprovable_ = 0;

 protected:
  void Reset() override {
    ShaderTranslator::Reset();
    failed_ = false;
    reason_ = "";
    for (auto& r : regs_) {
      for (auto& c : r) c = Src();
    }
    for (auto& c : pos_) c = Src();
    for (auto& r : interp_) {
      for (auto& c : r) c = Src();
    }
    fetches_.clear();
    unprovable_ = 0;
    full_index_ok_ = false;
    full_stride_ = 0;
  }
  void StartTranslation() override {
    if (is_vertex_shader()) {
      regs_[0][0].kind = Src::kVertexIndex;
    } else {
      for (uint32_t k = 0; k < 16; ++k) {
        for (uint32_t c = 0; c < 4; ++c) {
          regs_[k][c].kind = Src::kInterp;
          regs_[k][c].index = uint8_t(k);
          regs_[k][c].comp = uint8_t(c);
        }
      }
    }
  }
  void Fail(const char* why) {
    if (!failed_) reason_ = why;
    failed_ = true;
  }
  void ProcessExecInstructionBegin(const ParsedExecInstruction& instr) override {
    if (instr.type != ParsedExecInstruction::Type::kUnconditional) {
      Fail("conditional exec");
    }
  }
  void ProcessLoopStartInstruction(const ParsedLoopStartInstruction&) override {
    Fail("loop");
  }
  void ProcessLoopEndInstruction(const ParsedLoopEndInstruction&) override {
    Fail("loop");
  }
  void ProcessCallInstruction(const ParsedCallInstruction&) override {
    Fail("call");
  }
  void ProcessReturnInstruction(const ParsedReturnInstruction&) override {
    Fail("return");
  }
  void ProcessJumpInstruction(const ParsedJumpInstruction&) override {
    Fail("jump");
  }

  // Values written to a result (constant 0/1 components resolved here).
  void Store(const InstructionResult& result, const Src values[4]) {
    if (result.storage_addressing_mode !=
        InstructionStorageAddressingMode::kAbsolute) {
      Fail("relative destination");
      return;
    }
    Src* dest = nullptr;
    switch (result.storage_target) {
      case InstructionStorageTarget::kRegister:
        if (result.storage_index >= 64) {
          Fail("register index");
          return;
        }
        dest = regs_[result.storage_index];
        break;
      case InstructionStorageTarget::kPosition:
        dest = pos_;
        break;
      case InstructionStorageTarget::kInterpolator:
        if (result.storage_index >= 16) return;
        dest = interp_[result.storage_index];
        break;
      default:
        return;
    }
    const uint32_t mask = result.original_write_mask &
                          ((1u << GetInstructionStorageTargetUsedComponentCount(
                                result.storage_target)) -
                           1);
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(mask & (1u << i))) continue;
      Src v;
      switch (result.components[i]) {
        case SwizzleSource::k0:
          v.kind = Src::kConst0;
          break;
        case SwizzleSource::k1:
          v.kind = Src::kConst1;
          break;
        default:
          v = result.is_clamped ? Src() : values[uint32_t(result.components[i])];
          break;
      }
      dest[i] = v;
    }
  }
  void StoreUnknown(const InstructionResult& result) {
    Src unknown[4];
    Store(result, unknown);
  }
  // The four components of a register operand (unknown otherwise).
  void Load(const InstructionOperand& op, Src out[4]) const {
    for (uint32_t i = 0; i < 4; ++i) out[i] = Src();
    if (op.storage_source != InstructionStorageSource::kRegister ||
        op.storage_addressing_mode !=
            InstructionStorageAddressingMode::kAbsolute ||
        op.is_negated || op.is_absolute_value || op.storage_index >= 64) {
      return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      SwizzleSource s = op.GetComponent(i);
      if (s == SwizzleSource::k0) {
        out[i].kind = Src::kConst0;
      } else if (s == SwizzleSource::k1) {
        out[i].kind = Src::kConst1;
      } else {
        out[i] = regs_[op.storage_index][uint32_t(s)];
      }
    }
  }

  void ProcessVertexFetchInstruction(
      const ParsedVertexFetchInstruction& instr) override {
    if (instr.is_predicated) {
      StoreUnknown(instr.result);
      if (!instr.is_mini_fetch) full_index_ok_ = false;
      return;
    }
    if (!instr.is_mini_fetch) {
      Src index[4];
      Load(instr.operands[0], index);
      full_index_ok_ = index[0].kind == Src::kVertexIndex;
      full_stride_ = instr.attributes.stride;
    }
    uint32_t count = 0;
    Src values[4];
    if (full_index_ok_ && full_stride_ &&
        IsFloat32Format(instr.attributes.data_format, count) &&
        instr.attributes.exp_adjust > -64 && instr.attributes.exp_adjust < 64) {
      for (uint32_t i = 0; i < count; ++i) {
        values[i].kind = Src::kVfetch;
        values[i].index = uint8_t(instr.operands[1].storage_index);
        values[i].comp = uint8_t(i);
        values[i].exp_adjust = int8_t(instr.attributes.exp_adjust);
        values[i].offset = instr.attributes.offset;
        values[i].stride = full_stride_;
      }
    }
    Store(instr.result, values);
  }

  void ProcessTextureFetchInstruction(
      const ParsedTextureFetchInstruction& instr) override {
    if (is_pixel_shader() && instr.opcode == FetchOpcode::kTextureFetch) {
      PsFetch f;
      f.fetch_constant = instr.operands[1].storage_index;
      f.lod_bias = instr.attributes.lod_bias;
      Src coord[4];
      Load(instr.operands[0], coord);
      f.coord[0] = coord[0];
      f.coord[1] = coord[1];
      f.provable =
          !instr.is_predicated &&
          instr.dimension == xenos::FetchOpDimension::k2D &&
          instr.attributes.use_computed_lod &&
          !instr.attributes.use_register_lod &&
          !instr.attributes.use_register_gradients &&
          !instr.attributes.unnormalized_coordinates &&
          coord[0].kind == Src::kInterp && coord[1].kind == Src::kInterp;
      if (!f.provable) unprovable_ |= 1u << (f.fetch_constant & 31);
      fetches_.push_back(f);
    } else if (instr.operand_count >= 2 &&
               instr.operands[1].storage_source ==
                   InstructionStorageSource::kTextureFetchConstant) {
      // Any other use of a texture (getWeights, getCompTexLOD, ...).
      unprovable_ |= 1u << (instr.operands[1].storage_index & 31);
    }
    StoreUnknown(instr.result);
  }

  void ProcessAluInstruction(const ParsedAluInstruction& instr,
                             uint8_t) override {
    Src vec[4];
    if (!instr.is_predicated && instr.vector_opcode == AluVectorOpcode::kMax &&
        instr.vector_operand_count >= 2 &&
        instr.vector_operands[0].GetIdenticalComponents(
            instr.vector_operands[1]) == 0b1111) {
      // max(a, a) = a: the ucode's move.
      Load(instr.vector_operands[0], vec);
    }
    // Both results take their sources before either is written.
    if (instr.vector_and_constant_result.storage_target !=
        InstructionStorageTarget::kNone) {
      Store(instr.vector_and_constant_result, vec);
    }
    if (instr.scalar_result.storage_target != InstructionStorageTarget::kNone) {
      StoreUnknown(instr.scalar_result);
    }
  }

 private:
  bool full_index_ok_ = false;
  uint32_t full_stride_ = 0;
};

std::unordered_map<const Shader*, std::unique_ptr<VsInfo>> vs_cache;
std::unordered_map<const Shader*, std::unique_ptr<PsInfo>> ps_cache;

}  // namespace

const VsInfo* AnalyzeVertexShader(Shader& shader) {
  auto it = vs_cache.find(&shader);
  if (it != vs_cache.end()) return it->second.get();
  if (!shader.is_ucode_analyzed()) return nullptr;
  auto info = std::make_unique<VsInfo>();
  Analyzer a;
  if (shader.type() != xenos::ShaderType::kVertex) {
    info->reason = "not a vertex shader";
  } else if (!a.Run(shader)) {
    info->reason = a.reason_;
  } else {
    info->ok = true;
    std::memcpy(info->pos, a.pos_, sizeof(info->pos));
    std::memcpy(info->interp, a.interp_, sizeof(info->interp));
  }
  const VsInfo* result = info.get();
  vs_cache.emplace(&shader, std::move(info));
  return result;
}

const PsInfo* AnalyzePixelShader(Shader& shader) {
  auto it = ps_cache.find(&shader);
  if (it != ps_cache.end()) return it->second.get();
  if (!shader.is_ucode_analyzed()) return nullptr;
  auto info = std::make_unique<PsInfo>();
  Analyzer a;
  if (shader.type() != xenos::ShaderType::kPixel) {
    info->reason = "not a pixel shader";
  } else if (!a.Run(shader)) {
    info->reason = a.reason_;
  } else {
    info->ok = true;
    info->fetches = a.fetches_;
    info->unprovable_fetch_constants = a.unprovable_;
  }
  const PsInfo* result = info.get();
  ps_cache.emplace(&shader, std::move(info));
  return result;
}

}  // namespace miplod
}  // namespace metal
}  // namespace graphics
}  // namespace rex
