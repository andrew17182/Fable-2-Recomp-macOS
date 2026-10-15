// Fable II recomp: offline driver for the native Xenos -> MSL translator
// (docs/native-translator.md). Diagnostic only; not part of the game.
//
//   fable2-msl-xlate <shader.ucode.bin.frag> <modification hex> <out.metal>
//       [--scale N] [--gamma-unorm8 0|1]
//
// Runs the DXBC translator first (bindings), then the MSL translator. Exit
// code 0 = translated, 2 = fallback (reason on stdout), 1 = error.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "rex/graphics/metal/metal_msl_translator.h"
#include "rex/graphics/pipeline/shader/dxbc.h"
#include "rex/graphics/pipeline/shader/dxbc_translator.h"
#include "rex/string/buffer.h"

using namespace rex::graphics;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <ucode.bin> <mod hex> <out.metal> [--scale N] [--gamma-unorm8 0|1] [--points 1|2|3|4] [--rects 1|2] (even: with point coordinates; 3|4: mesh) [--ds-cpu 1] [--snap-uv 1]\n", argv[0]);
    return 1;
  }
  uint32_t scale = 1;
  bool gamma_unorm8 = true;
  uint32_t points = 0, rects = 0, ds_cpu = 0, snap_uv = 0;
  for (int i = 4; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--scale")) scale = uint32_t(std::atoi(argv[i + 1]));
    if (!std::strcmp(argv[i], "--gamma-unorm8")) gamma_unorm8 = std::atoi(argv[i + 1]) != 0;
    if (!std::strcmp(argv[i], "--points")) points = uint32_t(std::atoi(argv[i + 1]));
    if (!std::strcmp(argv[i], "--rects")) rects = uint32_t(std::atoi(argv[i + 1]));
    if (!std::strcmp(argv[i], "--ds-cpu")) ds_cpu = uint32_t(std::atoi(argv[i + 1]));
    if (!std::strcmp(argv[i], "--snap-uv")) snap_uv = uint32_t(std::atoi(argv[i + 1]));
  }
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
  if (bytes.empty() || bytes.size() % 4) {
    std::fprintf(stderr, "bad ucode file\n");
    return 1;
  }
  std::vector<uint32_t> dwords(bytes.size() / 4);
  std::memcpy(dwords.data(), bytes.data(), bytes.size());
  bool is_pixel = std::string(argv[1]).find(".frag") != std::string::npos;
  uint64_t modification = std::strtoull(argv[2], nullptr, 16);
  DxbcShader shader(is_pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, 0,
                    dwords.data(), dwords.size(), std::endian::little);
  rex::string::StringBuffer disasm;
  shader.AnalyzeUcode(disasm);
  DxbcShaderTranslator dxbc(rex::ui::GraphicsProvider::GpuVendorID::kApple, true, false,
                            gamma_unorm8, true, scale, scale, false);
  Shader::Translation* t = shader.GetOrCreateTranslation(modification);
  if (!dxbc.TranslateAnalyzedShader(*t)) {
    std::printf("dxbc translation failed\n");
    return 1;
  }
  rex::graphics::metal::MslTranslatorConfig config;
  config.draw_resolution_scale_x = scale;
  config.draw_resolution_scale_y = scale;
  config.gamma_render_target_as_unorm8 = gamma_unorm8;
  // --points 1|2: per-corner expansion; 3|4: mesh function (VS once per point);
  // even values: with point coordinates.
  config.expand_points = points == 1 || points == 2;
  config.points_mesh = points == 3 || points == 4;
  config.expand_rects = rects != 0;
  config.domain_cpu_pattern = ds_cpu != 0;
  config.snap_domain_uv = snap_uv != 0;
  config.point_coordinates = points == 2 || points == 4 || rects == 2;
  rex::graphics::metal::MslShaderTranslator msl(config);
  auto result = msl.Translate(shader, modification);
  if (!result.ok) {
    std::printf("fallback: %s\n", result.reason.c_str());
    return 2;
  }
  std::ofstream(argv[3]) << result.source;
  // Sidecar for the differential test (tools/shaderlab/xlate_diff.mm).
  {
    std::ofstream j(std::string(argv[3]) + ".json");
    Shader::Translation* tr = shader.GetTranslation(modification);
    (void)tr;
    DxbcShaderTranslator::Modification m(modification);
    j << "{\"vertex\": " << (is_pixel ? 0 : 1)
      << ", \"clip_planes\": " << (is_pixel ? 0u : uint32_t(m.vertex.user_clip_plane_count))
      << ", \"interpolator_mask\": " << m.pixel.interpolator_mask
      << ", \"colors\": " << shader.writes_color_targets()
      << ", \"depth\": " << ((shader.writes_depth() || result.source.find("[[depth(") != std::string::npos) ? 1 : 0)
      << ", \"float_count\": " << shader.constant_register_map().float_count
      << ", \"textures\": [";
    bool first = true;
    for (const auto& b : shader.GetTextureBindingsAfterTranslation()) {
      j << (first ? "" : ", ") << "[" << b.bindless_descriptor_index << ", "
        << uint32_t(b.dimension) << ", " << b.fetch_constant << ", "
        << (b.is_signed ? 1 : 0) << "]";
      first = false;
    }
    j << "], \"float_map\": [";
    {
      bool f2 = true;
      for (uint32_t c = 0; c < 256; ++c) {
        uint32_t packed = shader.constant_register_map().GetPackedFloatConstantIndex(c);
        if (packed == UINT32_MAX) continue;
        j << (f2 ? "" : ", ") << "[" << c << ", " << packed << "]";
        f2 = false;
      }
    }
    j << "], \"samplers\": [";
    first = true;
    for (const auto& b : shader.GetSamplerBindingsAfterTranslation()) {
      j << (first ? "" : ", ") << "[" << b.bindless_descriptor_index << ", "
        << b.fetch_constant << ", " << uint32_t(b.mag_filter) << ", "
        << uint32_t(b.min_filter) << ", " << uint32_t(b.mip_filter) << "]";
      first = false;
    }
    using SC = DxbcShaderTranslator::SystemConstants;
    j << "], \"sys\": {\"size\": " << sizeof(SC)
      << ", \"flags\": " << offsetof(SC, flags)
      << ", \"signs\": " << offsetof(SC, texture_swizzled_signs)
      << ", \"res_scaled\": " << offsetof(SC, textures_resolution_scaled)
      << ", \"sample_count_log2\": " << offsetof(SC, sample_count_log2)
      << ", \"alpha_ref\": " << offsetof(SC, alpha_test_reference)
      << ", \"alpha_to_mask\": " << offsetof(SC, alpha_to_mask)
      << ", \"color_exp_bias\": " << offsetof(SC, color_exp_bias)
      << ", \"blend_constant\": " << offsetof(SC, edram_blend_constant)
      << ", \"vertex_index_endian\": " << offsetof(SC, vertex_index_endian)
      << ", \"vertex_index_offset\": " << offsetof(SC, vertex_index_offset)
      << ", \"vertex_index_min\": " << offsetof(SC, vertex_index_min)
      << ", \"vertex_index_max\": " << offsetof(SC, vertex_index_max)
      << ", \"line_loop_closing_index\": " << offsetof(SC, line_loop_closing_index)
      << ", \"user_clip_planes\": " << offsetof(SC, user_clip_planes)
      << ", \"ndc_scale\": " << offsetof(SC, ndc_scale)
      << ", \"ndc_offset\": " << offsetof(SC, ndc_offset)
      << ", \"point_constant_diameter\": " << offsetof(SC, point_constant_diameter)
      << ", \"point_ndc_radius\": " << offsetof(SC, point_screen_diameter_to_ndc_radius)
      << ", \"point_diameter_min\": " << offsetof(SC, point_vertex_diameter_min)
      << ", \"point_diameter_max\": " << offsetof(SC, point_vertex_diameter_max)
      << ", \"tess_factor_min\": " << offsetof(SC, tessellation_factor_range_min)
      << ", \"tess_factor_max\": " << offsetof(SC, tessellation_factor_range_max)
      << "}, \"domain\": "
      << ((!is_pixel && m.vertex.host_vertex_shader_type !=
                            Shader::HostVertexShaderType::kVertex)
              ? 1
              : 0)
      << "}\n";
  }
  std::printf("ok\n");
  return 0;
}
extern "C" void fable2_ring_writeback_stored() {}
