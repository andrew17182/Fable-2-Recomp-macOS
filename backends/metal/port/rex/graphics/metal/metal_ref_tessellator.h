// The D3D11 reference tessellator (third_party/d3d11-tessellator, MIT) for the
// exact native tessellation path (FABLE2_NATIVE_TESS, docs/native-translator.md):
// the domain points and the triangle list, in the reference order, of a quad
// patch with fractional-even partitioning and clockwise output, as the hull
// shaders declare.
#ifndef REX_GRAPHICS_METAL_METAL_REF_TESSELLATOR_H_
#define REX_GRAPHICS_METAL_METAL_REF_TESSELLATOR_H_

#include <cstdint>
#include <vector>

namespace rex::graphics::metal::ref_tess {

// A domain point (the uv arena of the native domain entry xe_native_ds_cpu).
struct Vertex {
  float u, v;
};

// Edge factors in D3D order (Ueq0, Veq0, Ueq1, Veq1), then inside U, V.
void TessellateQuadFractionalEven(const float factors[6],
                                  std::vector<Vertex>& vertices_out,
                                  std::vector<uint32_t>& indices_out);

}  // namespace rex::graphics::metal::ref_tess

#endif  // REX_GRAPHICS_METAL_METAL_REF_TESSELLATOR_H_
