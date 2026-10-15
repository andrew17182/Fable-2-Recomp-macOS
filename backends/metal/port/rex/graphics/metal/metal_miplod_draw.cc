// [mip-lod] Per-draw bound of the mip levels a fetch can read
// (metal_miplod_proof.h, docs/mip-lod-20261013.md).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>

#include "rex/graphics/metal/metal_miplod_proof.h"
#include "rex/graphics/metal/metal_shared_memory.h"
#include "rex/graphics/metal/metal_texture_cache.h"
#include "rex/graphics/primitive_processor.h"
#include "rex/logging.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {

constexpr double kLodTolerance = 1.0 / 4096.0;
constexpr uint32_t kMaxVertices = 1024;

uint32_t SwapWord(uint32_t v, uint32_t endian) {
  // Endian128 / Endian: 1 = 8in16, 2 = 8in32, 3 = 16in32.
  if (endian == 1 || endian == 2) {
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  }
  if (endian == 2 || endian == 3) {
    v = (v >> 16) | (v << 16);
  }
  return v;
}

}  // namespace

void MetalTextureCache::MipLodDraw(const MipLodDrawInputs& in) {
  std::memset(miplod_view_levels_, 0, sizeof(miplod_view_levels_));
  if (!miplod::Enabled()) return;
  UploadTraceCounters& u = UploadTrace();

  // Vertex-stage samplings are never bounded.
  if (in.vertex_shader) {
    for (const Shader::TextureBinding& b : in.vertex_shader->texture_bindings()) {
      const TextureBinding* tb = GetValidTextureBinding(b.fetch_constant);
      if (!tb) continue;
      MipLodNoteSample(tb->texture, -1);
      MipLodNoteSample(tb->texture_signed, -1);
    }
  }
  if (!in.pixel_shader) return;
  const auto& ps_bindings = in.pixel_shader->texture_bindings();
  uint32_t candidate_fcs = 0;
  for (const Shader::TextureBinding& b : ps_bindings) {
    const TextureBinding* tb = GetValidTextureBinding(b.fetch_constant);
    if (tb && (MipLodIsCandidate(tb->texture) ||
               MipLodIsCandidate(tb->texture_signed))) {
      candidate_fcs |= 1u << (b.fetch_constant & 31);
    }
  }
  if (!candidate_fcs) return;

  const char* why = nullptr;
  const miplod::PsInfo* ps = miplod::AnalyzePixelShader(*in.pixel_shader);
  const miplod::VsInfo* vs =
      in.vertex_shader ? miplod::AnalyzeVertexShader(*in.vertex_shader)
                       : nullptr;
  if (!ps || !ps->ok) {
    why = ps ? ps->reason : "ps not analysed";
  } else if (!vs || !vs->ok) {
    why = vs ? vs->reason : "vs not analysed";
  } else if (!in.plain_vertex) {
    why = "host vertex shader type";
  } else if (in.primitive_reset) {
    why = "primitive reset";
  } else if (in.primitive != xenos::PrimitiveType::kTriangleList &&
             in.primitive != xenos::PrimitiveType::kTriangleStrip &&
             in.primitive != xenos::PrimitiveType::kTriangleFan &&
             in.primitive != xenos::PrimitiveType::kRectangleList) {
    why = "primitive type";
  } else if (in.vertex_count < 3 || in.vertex_count > kMaxVertices) {
    why = "vertex count";
  } else if (in.index_type != PrimitiveProcessor::kNone &&
             in.index_type != PrimitiveProcessor::kGuestDMA) {
    why = "index buffer type";
  } else if (!(in.pixels_per_x > 0.0 || in.pixels_per_x < 0.0) ||
             !(in.pixels_per_y > 0.0 || in.pixels_per_y < 0.0) ||
             !std::isfinite(in.pixels_per_x) ||
             !std::isfinite(in.pixels_per_y)) {
    why = "viewport";
  } else if (vs->pos[0].kind != miplod::Src::kVfetch ||
             vs->pos[1].kind != miplod::Src::kVfetch ||
             vs->pos[3].kind != miplod::Src::kConst1) {
    why = "position not a float fetch with W = 1";
  }

  // Vertex indices as the vertex shader computes them.
  std::vector<uint32_t> indices;
  if (!why) {
    indices.resize(in.vertex_count);
    const uint8_t* index_data = nullptr;
    if (in.index_type == PrimitiveProcessor::kGuestDMA) {
      index_data = in.read(in.index_address,
                           in.vertex_count * (in.index_32bit ? 4u : 2u));
      if (!index_data) why = "index buffer not stable";
    }
    for (uint32_t i = 0; !why && i < in.vertex_count; ++i) {
      uint32_t vi = i;
      if (index_data) {
        if (in.index_32bit) {
          std::memcpy(&vi, index_data + 4 * i, 4);
        } else {
          uint16_t v16;
          std::memcpy(&v16, index_data + 2 * i, 2);
          vi = v16;
        }
      }
      uint32_t idx = vi != in.line_loop_closing_index ? vi : 0u;
      idx = SwapWord(idx, in.index_endian);
      idx = (idx + in.index_offset) & xenos::kVertexIndexMask;
      idx = std::min(std::max(idx, in.index_min), in.index_max);
      indices[i] = idx;
    }
  }

  // A value of the vertex shader for every vertex of the draw.
  const RegisterFile& regs = register_file();
  auto load = [&](const miplod::Src& src, std::vector<double>& out) -> bool {
    out.resize(in.vertex_count);
    if (src.kind == miplod::Src::kConst0 || src.kind == miplod::Src::kConst1) {
      std::fill(out.begin(), out.end(),
                src.kind == miplod::Src::kConst1 ? 1.0 : 0.0);
      return true;
    }
    if (src.kind != miplod::Src::kVfetch) return false;
    const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(src.index);
    const uint32_t base = uint32_t(vf.address) << 2;
    const double scale = std::ldexp(1.0, src.exp_adjust);
    for (uint32_t i = 0; i < in.vertex_count; ++i) {
      const int64_t address = int64_t(base) +
                              int64_t(indices[i]) * src.stride * 4 +
                              (int64_t(src.offset) + src.comp) * 4;
      if (address < 0 || address > int64_t(UINT32_MAX) - 4) return false;
      const uint8_t* p = in.read(uint32_t(address), 4);
      if (!p) return false;
      uint32_t w;
      std::memcpy(&w, p, 4);
      w = SwapWord(w, uint32_t(vf.endian));
      float f;
      std::memcpy(&f, &w, 4);
      if (!std::isfinite(f)) return false;
      out[i] = double(f) * scale;
    }
    return true;
  };

  std::vector<double> px, py;
  if (!why && (!load(vs->pos[0], px) || !load(vs->pos[1], py))) {
    why = "position data";
  }
  if (!why) {
    for (uint32_t i = 0; i < in.vertex_count; ++i) {
      px[i] *= in.pixels_per_x;
      py[i] *= in.pixels_per_y;
    }
  }
  // Triangles of the draw (a rectangle's fourth vertex lies on the plane of
  // its first three).
  auto triangle = [&](uint32_t t, uint32_t& a, uint32_t& b,
                      uint32_t& c) -> bool {
    switch (in.primitive) {
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        if (3 * t + 2 >= in.vertex_count) return false;
        a = 3 * t;
        b = a + 1;
        c = a + 2;
        return true;
      case xenos::PrimitiveType::kTriangleStrip:
        if (t + 2 >= in.vertex_count) return false;
        a = t;
        b = t + 1;
        c = t + 2;
        return true;
      case xenos::PrimitiveType::kTriangleFan:
        if (t + 2 >= in.vertex_count) return false;
        a = 0;
        b = t + 1;
        c = t + 2;
        return true;
      default:
        return false;
    }
  };

  // Highest LOD of the coordinate (u, v) over the draw, for a texture of
  // width x height host texels at level 0, without biases; -inf if constant.
  auto max_lod = [&](const std::vector<double>& cu,
                     const std::vector<double>& cv, double width,
                     double height, double& out) -> bool {
    double rho_max = 0.0;
    uint32_t a, b, c;
    for (uint32_t t = 0; triangle(t, a, b, c); ++t) {
      const double x1 = px[b] - px[a], y1 = py[b] - py[a];
      const double x2 = px[c] - px[a], y2 = py[c] - py[a];
      const double det = x1 * y2 - x2 * y1;
      if (!(det > 0.0 || det < 0.0)) continue;  // not rasterized
      const double u1 = cu[b] - cu[a], u2 = cu[c] - cu[a];
      const double v1 = cv[b] - cv[a], v2 = cv[c] - cv[a];
      const double dudx = (u1 * y2 - u2 * y1) / det;
      const double dudy = (x1 * u2 - x2 * u1) / det;
      const double dvdx = (v1 * y2 - v2 * y1) / det;
      const double dvdy = (x1 * v2 - x2 * v1) / det;
      // L1 norms of the texel-space gradients: not below the Euclidean or
      // max norm a sampler may use.
      const double rx = std::fabs(dudx) * width + std::fabs(dvdx) * height;
      const double ry = std::fabs(dudy) * width + std::fabs(dvdy) * height;
      if (!std::isfinite(rx) || !std::isfinite(ry)) return false;
      rho_max = std::max(rho_max, std::max(rx, ry));
    }
    out = rho_max > 0.0 ? std::log2(rho_max) : -HUGE_VAL;
    return true;
  };

  for (uint32_t fc = 0; fc < 32; ++fc) {
    if (!(candidate_fcs & (1u << fc))) continue;
    const TextureBinding* tb = GetValidTextureBinding(fc);
    const char* fc_why = why;
    int32_t max_read = -1;
    double lod_bound = -HUGE_VAL;
    const auto* texture = tb ? static_cast<const MetalTexture*>(tb->texture)
                             : nullptr;
    if (!fc_why && (!tb || !texture || !texture->metal_texture())) {
      fc_why = "no texture";
    }
    if (!fc_why && (ps->unprovable_fetch_constants & (1u << fc))) {
      fc_why = "a fetch of this constant is not bounded";
    }
    if (!fc_why && (tb->key.dimension != xenos::DataDimension::k2DOrStacked ||
                    tb->key.GetDepthOrArraySize() != 1)) {
      fc_why = "not a 2D texture";
    }
    if (!fc_why) {
      const double width = double(texture->metal_texture()->width());
      const double height = double(texture->metal_texture()->height());
      const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(fc);
      const double fetch_bias = double(int32_t(fetch.lod_bias)) / 32.0;
      bool any = false;
      for (const miplod::PsFetch& f : ps->fetches) {
        if (f.fetch_constant != fc) continue;
        any = true;
        std::vector<double> cu, cv;
        bool ok = true;
        for (uint32_t i = 0; i < 2 && ok; ++i) {
          const uint32_t k = f.coord[i].index;
          if (!(in.interpolator_mask & (1u << k)) ||
              (in.interpolators_centroid & (1u << k)) ||
              (in.param_gen && in.param_gen_interpolator == k)) {
            ok = false;
            fc_why = "interpolant (mask, centroid or parameter generation)";
          }
        }
        if (ok && (!load(vs->interp[f.coord[0].index][f.coord[0].comp], cu) ||
                   !load(vs->interp[f.coord[1].index][f.coord[1].comp], cv))) {
          ok = false;
          fc_why = "coordinate not a float fetch";
        }
        double lod = 0.0;
        if (ok && !max_lod(cu, cv, width, height, lod)) {
          ok = false;
          fc_why = "gradient";
        }
        if (!ok) break;
        lod_bound = std::max(lod_bound, lod + fetch_bias + double(f.lod_bias));
      }
      if (!fc_why && !any) fc_why = "no fetch";
      // Levels read through every sampler of this fetch constant.
      bool any_sampler = false;
      if (!fc_why && in.pixel_samplers) {
        for (const DxbcShader::SamplerBinding& sb : *in.pixel_samplers) {
          if (sb.fetch_constant != fc) continue;
          any_sampler = true;
          const SamplerParameters sp = GetSamplerParameters(sb);
          if (sp.aniso_filter != xenos::AnisoFilter::kDisabled) {
            fc_why = "anisotropic";
            break;
          }
          int32_t read = int32_t(sp.mip_min_level);
          if (!sp.mip_base_map && lod_bound > -HUGE_VAL) {
            read = std::max(
                read, int32_t(std::ceil(lod_bound - kLodTolerance)));
          }
          max_read = std::max(max_read, read);
        }
      }
      if (!fc_why && !any_sampler) fc_why = "no sampler";
      if (fc_why) max_read = -1;
    }
    if (max_read >= 0) {
      max_read = std::min(max_read, int32_t(tb->key.mip_max_level));
      ++u.miplod_bounded;
      if (max_read < int32_t(tb->key.mip_max_level)) {
        miplod_view_levels_[fc] = uint8_t(max_read + 1);
        ++u.miplod_view;
      }
    } else {
      ++u.miplod_unbounded;
    }
    if (tb) {
      MipLodNoteSample(tb->texture, max_read);
      MipLodNoteSample(tb->texture_signed, max_read);
    }
    if (miplod::TraceEnabled()) {
      static std::set<std::string> seen;
      if (seen.size() < 512) {
        std::string line = fmt::format(
            "[mip-lod] draw vs={:016X} ps={:016X} tf{} prim={} n={} idx={} "
            "{}: lod<={:.6f} read<={} levels={} of {}",
            in.vertex_shader ? in.vertex_shader->ucode_data_hash() : 0,
            in.pixel_shader->ucode_data_hash(), fc, uint32_t(in.primitive),
            in.vertex_count, in.index_type, fc_why ? fc_why : "bounded",
            lod_bound, max_read, uint32_t(miplod_view_levels_[fc]),
            tb ? uint32_t(tb->key.mip_max_level) + 1 : 0);
        if (seen.insert(line).second) REXLOG_INFO("{}", line);
      }
    }
  }
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
