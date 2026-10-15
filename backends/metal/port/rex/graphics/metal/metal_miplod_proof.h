// [mip-lod] Per-draw proof of the mip levels a texture fetch can read
// (docs/mip-lod-20261013.md).
//
// Xenos clamps a fetch's LOD to the fetch constant's mip range, and the host
// does the same: the sampler's lodMinClamp is the effective mip_min_level and
// the host texture has exactly key.mip_max_level + 1 levels. But the guest's
// mip-chain passes (render level N + 1 by sampling the same texture, then
// resolve into level N + 1) use a fetch constant whose range includes N + 1:
// only the derivative LOD (exactly N for a 2:1 quad) keeps level N + 1 at
// weight zero. So the guest's own clamp cannot exclude N + 1.
//
// This module computes, from guest state only, an upper bound of that LOD:
//  - the shaders are analysed once: the pixel shader's fetch coordinate must
//    be an interpolant unchanged since entry, and the vertex shader must write
//    that interpolant and the position straight from float vertex fetches,
//    with position W the constant 1 (so the attribute is affine on screen);
//  - per draw, the vertices are read from the GPU-visible shared memory, the
//    screen-space gradient of the coordinate is solved per triangle with the
//    host viewport transform, and the LOD bound is log2 of the L1 norm of the
//    texel-space gradient (an upper bound of any norm the hardware may use)
//    plus the fetch constant and instruction LOD biases.
// The levels the draw may read are then at most max(mip_min, ceil(bound)).
// The draw samples a view limited to those levels, so the host cannot read a
// higher level whatever its own rounding. Under the guest's exact arithmetic
// the bound is the LOD itself, so the view changes nothing the guest would
// read (beyond a 2^-12 LOD tolerance, below any hardware's LOD precision).

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "rex/graphics/pipeline/shader/shader.h"
#include "rex/graphics/xenos.h"

namespace rex {
namespace graphics {
namespace metal {
namespace miplod {

// Where a shader value component comes from.
struct Src {
  enum Kind : uint8_t {
    kUnknown = 0,
    kConst0,
    kConst1,
    kVertexIndex,  // VS r0.x at entry
    kVfetch,       // a float vertex fetch component
    kInterp,       // PS interpolant `index`, component `comp`, at entry
  };
  Kind kind = kUnknown;
  uint8_t index = 0;  // vertex fetch constant (kVfetch) or interpolant (kInterp)
  uint8_t comp = 0;   // fetched / interpolant component
  int8_t exp_adjust = 0;
  int32_t offset = 0;   // dwords (kVfetch)
  uint32_t stride = 0;  // dwords (kVfetch)
};

struct VsInfo {
  bool ok = false;
  const char* reason = "";
  Src pos[4];
  Src interp[16][4];
};

struct PsFetch {
  uint32_t fetch_constant = 0;
  bool provable = false;
  Src coord[2];
  float lod_bias = 0.0f;  // instruction LOD bias
};

struct PsInfo {
  bool ok = false;
  const char* reason = "";
  std::vector<PsFetch> fetches;
  // Fetch constants with at least one fetch that cannot be bounded.
  uint32_t unprovable_fetch_constants = 0;
};

// Analysed once per shader (cached by Shader pointer); null if the shader is
// not analysed yet.
const VsInfo* AnalyzeVertexShader(Shader& shader);
const PsInfo* AnalyzePixelShader(Shader& shader);

// FABLE2_MIPLOD_PROOF (default on, =0 kill switch).
bool Enabled();
// FABLE2_MIPLOD_TRACE=1: log proofs and hazard decisions.
bool TraceEnabled();

}  // namespace miplod
}  // namespace metal
}  // namespace graphics
}  // namespace rex
