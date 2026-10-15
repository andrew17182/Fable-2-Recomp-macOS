// FABLE2_NO_CONVERTER builds (docs/no-converter.md): stand-ins for the
// DXBC -> DXIL (dxilconv) and DXIL -> Metal (Metal Shader Converter) wrappers,
// compiled instead of dxbc_to_dxil_converter.cc and metal_shader_converter.cc.
// Neither library is linked or shipped. Every request fails, and the census
// logs the first refusal per purpose at error level with the shader hash, so
// a draw path that still needs the converter is loud, never silent.

#include "rex/graphics/metal/dxbc_to_dxil_converter.h"
#include "rex/graphics/metal/metal_converter_census.h"
#include "rex/graphics/metal/metal_shader_converter.h"
#include "rex/graphics/metal/metal_stage_compile_cache.h"
#include "rex/logging.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {
constexpr const char* kNoConverter =
    "this build has no shader converter (FABLE2_NO_CONVERTER)";
}

DxbcToDxilConverter::DxbcToDxilConverter() = default;
DxbcToDxilConverter::~DxbcToDxilConverter() = default;

bool DxbcToDxilConverter::Initialize() {
  REXLOG_INFO("[no-converter] dxilconv not built: DXBC -> DXIL unavailable");
  is_available_ = false;
  return false;
}

bool DxbcToDxilConverter::Convert(const std::vector<uint8_t>&,
                                  std::vector<uint8_t>& dxil_data_out,
                                  std::string* error_message) {
  dxil_data_out.clear();
  converter_census::Refused();
  if (error_message) {
    *error_message = kNoConverter;
  }
  return false;
}

uint64_t DxbcToDxilConverter::GetOptionsHash() const { return 0; }

MetalShaderConverter::MetalShaderConverter()
    : stage_compile_cache_(std::make_unique<MetalStageCompileCache>()) {}

MetalShaderConverter::~MetalShaderConverter() = default;

bool MetalShaderConverter::Initialize() {
  REXLOG_INFO(
      "[no-converter] Metal Shader Converter not built: DXIL -> Metal "
      "unavailable");
  is_available_ = false;
  return false;
}

bool MetalShaderConverter::Convert(xenos::ShaderType,
                                   const std::vector<uint8_t>&,
                                   MetalShaderConversionResult& result) {
  converter_census::Refused();
  result.success = false;
  result.error_message = kNoConverter;
  return false;
}

bool MetalShaderConverter::ConvertWithStage(
    MetalShaderStage, const std::vector<uint8_t>&,
    MetalShaderConversionResult& result) {
  converter_census::Refused();
  result.success = false;
  result.error_message = kNoConverter;
  return false;
}

bool MetalShaderConverter::ConvertWithStageEx(
    MetalShaderStage, const std::vector<uint8_t>&,
    MetalShaderConversionResult& result, MetalShaderReflectionInfo*,
    const IRVersionedInputLayoutDescriptor*, std::vector<uint8_t>*, bool, int) {
  converter_census::Refused();
  result.success = false;
  result.error_message = kNoConverter;
  return false;
}

std::shared_ptr<const MetalStageCompileResult>
MetalShaderConverter::CompileStage(const MetalStageCompileRequest&) {
  converter_census::Refused();
  auto result = std::make_shared<MetalStageCompileResult>();
  result->success = false;
  result->error_message = kNoConverter;
  return result;
}

MetalStageCompileResult MetalShaderConverter::CompileStageUncached(
    const MetalStageCompileRequest&) {
  MetalStageCompileResult result;
  result.success = false;
  result.error_message = kNoConverter;
  return result;
}

void MetalShaderConverter::SetMinimumTarget(uint32_t, uint32_t,
                                            const std::string&) {}

void MetalShaderConverter::PopulateDefaultRequestOptions(
    MetalStageCompileRequest&) const {}

void* MetalShaderConverter::CreateXbox360RootSignature(bool) {
  return nullptr;
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
