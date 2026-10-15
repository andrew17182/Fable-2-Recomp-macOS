// See metal_ref_tessellator.h. The vendored reference is compiled inside a
// namespace: its own fmin/fmax and min/max macros would clash with libc++.
#include "rex/graphics/metal/metal_ref_tessellator.h"

#include <math.h>
#include <string.h>

namespace d3d11_ref_tessellator {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../../../../third_party/d3d11-tessellator/tessellator.cpp"
#pragma clang diagnostic pop
}  // namespace d3d11_ref_tessellator
#undef min
#undef max

namespace rex::graphics::metal::ref_tess {

void TessellateQuadFractionalEven(const float factors[6],
                                  std::vector<Vertex>& vertices_out,
                                  std::vector<uint32_t>& indices_out) {
  // CHWTessellator holds buffers for the maximum tessellation; keep one per
  // thread (the render thread in practice).
  static thread_local d3d11_ref_tessellator::CHWTessellator tessellator;
  static thread_local bool initialized = false;
  if (!initialized) {
    tessellator.Init(
        d3d11_ref_tessellator::D3D11_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN,
        d3d11_ref_tessellator::D3D11_TESSELLATOR_OUTPUT_TRIANGLE_CW);
    initialized = true;
  }
  tessellator.TessellateQuadDomain(factors[0], factors[1], factors[2],
                                   factors[3], factors[4], factors[5]);
  const int point_count = tessellator.GetPointCount();
  const int index_count = tessellator.GetIndexCount();
  const d3d11_ref_tessellator::DOMAIN_POINT* points = tessellator.GetPoints();
  const int* indices = tessellator.GetIndices();
  vertices_out.resize(size_t(point_count));
  for (int i = 0; i < point_count; ++i) {
    vertices_out[size_t(i)] = {points[i].u, points[i].v};
  }
  indices_out.resize(size_t(index_count));
  for (int i = 0; i < index_count; ++i) {
    indices_out[size_t(i)] = uint32_t(indices[i]);
  }
}

}  // namespace rex::graphics::metal::ref_tess
