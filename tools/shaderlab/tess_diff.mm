// tess_diff: differential test of FABLE2_NATIVE_TESS (Metal hardware
// tessellation + the native domain shader) against the Metal Shader
// Converter's tessellation emulation of the same draw
// (docs/native-translator.md, "Geometry and tessellation emulation").
// Diagnostic only.
//
// Both sides draw the same adaptive quad patches (factors in a synthetic
// guest index buffer) with the same synthetic guest state (random shared
// memory, constants, textures) through the same ABI:
// - A: the converter's emulation pipeline, from the stages the game dumps
//   with FABLE2_NATIVE_TESS_DUMP=<dir> (tessvs_*, hs_*, ds_*, tess_*.json);
// - B: a tessellation pipeline with the native domain shader
//   (fable2-msl-xlate output for the same modification) and per-patch half
//   factors computed as the game does.
// A generated fragment shader writes every output, interpolated, to RGBA32F
// targets (several passes), plus depth. Reported: pixels covered by either
// side, pixels covered by only one, bit-identical pixels, and the largest
// difference where both cover.
//
// Build: clang++ -std=c++20 -fobjc-arc -O2 -I<msc include> tess_diff.mm
//        -framework Metal -framework Foundation -o tess_diff
// Run:   tess_diff <dumpdir> <ds hash> <mod hex> <native.metal> <native.json> [seeds] [patches]
// Env:   TD_FACTORS=range:LO:HI (effective factors, default range:1:16) | uniform:F
//        TD_EDGE_MAP=3012 (control point per Metal edge), TD_WINDING=cw (default ccw), TD_CULL=back|front
//        TD_HALF_EXACT=1, TD_PATCH_UNIFORM=1, TD_CP=a,b,c,d, TD_INSIDE_SWAP=1, TD_FCONST=g:x,y,z,w;..., TD_DEBUG=1
//        TD_W=<width> (default 256)
// Exit:  0 identical, 3 differences, 1 error.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define IR_PRIVATE_IMPLEMENTATION
#include "metal_irconverter_runtime.h"
// The D3D11 reference tessellator (backends/metal/third_party/d3d11-tessellator, MIT), in a namespace: its
// own fmin/fmax/min/max would clash with the C++ library.
#include <math.h>
#include <string.h>
namespace d3d11_ref_tessellator {
#include "../../backends/metal/third_party/d3d11-tessellator/tessellator.cpp"
}
#undef min
#undef max
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kHeapSlots = 1024;
constexpr uint32_t kSharedBytes = 1u << 20;
struct Entry { uint64_t gpuVA, textureViewID, metadata; };
struct BufEntry { uint64_t va, view, size; };

float RandomFloat(std::mt19937& rng) {
  std::uniform_real_distribution<float> u(-2.0f, 2.0f);
  uint32_t k = rng() % 100;
  if (k < 80) return u(rng);
  if (k < 88) return 0.0f;
  if (k < 92) return 1.0f;
  return u(rng) * 16.0f;
}

std::string ReadText(const std::string& path) {
  NSString* s = [NSString stringWithContentsOfFile:@(path.c_str()) encoding:NSUTF8StringEncoding error:nil];
  return s ? std::string(s.UTF8String) : std::string();
}
}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc < 6) {
      fprintf(stderr, "usage: tess_diff <dumpdir> <ds hash> <mod hex> <native.metal> <native.json> [seeds] [patches]\n");
      return 1;
    }
    std::string dir = argv[1], hash = argv[2], mod = argv[3];
    int seeds = argc > 6 ? atoi(argv[6]) : 4;
    uint32_t patches = argc > 7 ? uint32_t(atoi(argv[7])) : 24;
    const uint32_t kW = getenv("TD_W") ? uint32_t(atoi(getenv("TD_W"))) : 256u, kH = kW;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    NSError* err = nil;
    NSDictionary* cfg = [NSJSONSerialization
        JSONObjectWithData:[NSData dataWithContentsOfFile:@((dir + "/tess_" + hash + "_" + mod + ".json").c_str())]
                   options:0 error:nil];
    NSDictionary* info = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[5])]
                                                         options:0 error:nil];
    if (!cfg || !info) {
      fprintf(stderr, "missing tess json or sidecar\n");
      return 1;
    }
    NSDictionary* sys = info[@"sys"];
    uint32_t mode = [cfg[@"mode"] unsignedIntValue], host_vs_type = [cfg[@"host_vs_type"] unsignedIntValue];
    auto lib = [&](const std::string& name) -> id<MTLLibrary> {
      NSError* e = nil;
      id<MTLLibrary> l = [dev newLibraryWithURL:[NSURL fileURLWithPath:@((dir + "/" + name).c_str())] error:&e];
      if (!l) fprintf(stderr, "%s: %s\n", name.c_str(), e.localizedDescription.UTF8String);
      return l;
    };
    std::string vs_base = "tessvs_" + hash + "_" + mod + "_m" + std::to_string(mode);
    id<MTLLibrary> lib_stage_in = lib(vs_base + ".stagein.metallib");
    id<MTLLibrary> lib_vs = lib(vs_base + ".metallib");
    id<MTLLibrary> lib_hs = lib("hs_t" + std::to_string(host_vs_type) + "_m" + std::to_string(mode) + ".metallib");
    id<MTLLibrary> lib_ds = lib("ds_" + hash + "_" + mod + ".metallib");
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion3_2;
    opts.mathMode = MTLMathModeSafe;
    opts.preserveInvariance = YES;
    std::string nsrc = ReadText(argv[4]);
    id<MTLLibrary> lib_native = [dev newLibraryWithSource:@(nsrc.c_str()) options:opts error:&err];
    if (!lib_stage_in || !lib_vs || !lib_hs || !lib_ds || !lib_native) {
      if (!lib_native) fprintf(stderr, "native: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    // CPU-pattern variant (fable2-msl-xlate --ds-cpu 1): the reference tessellator's points and indices.
    const bool cpu_pattern = nsrc.find("xe_native_ds_cpu") != std::string::npos;
    id<MTLFunction> ds_native = [lib_native newFunctionWithName:cpu_pattern ? @"xe_native_ds_cpu" : @"xe_native_ds"];
    // Probe library: the native domain function as a plain function called from a kernel, to place
    // the patches on screen (NDC scale/offset), as xlate_diff_vs does.
    std::string psrc = nsrc;
    {
      size_t f = psrc.find(cpu_pattern ? "vertex XeVOut xe_native_ds_cpu(" : "[[patch(quad)]] vertex XeVOut xe_native_ds(");
      if (f == std::string::npos || !ds_native) { fprintf(stderr, "not a native quad DS\n"); return 1; }
      size_t e = psrc.find(") {", f);
      psrc.replace(f, e - f, "static XeVOut xe_ds_body(uint xe_patch_id, float2 xe_uv, constant XeTop& xe_tl, "
                             "constant XeTessDraw& xe_td");
      {
        // Snapped variant (--snap-uv 1): the probe passes uv directly, snap it in place.
        size_t b = psrc.find("float2 xe_uv = rint(xe_uv_hw");
        if (b != std::string::npos) psrc.replace(b, strlen("float2 xe_uv = rint(xe_uv_hw"), "xe_uv = rint(xe_uv");
      }
      if (cpu_pattern) {
        // The CPU entry derives uv and the patch from its buffers; the probe passes them directly.
        size_t b = psrc.find("float2 xe_uv = xe_tv[xe_vid & 0xFFFFFu];");
        psrc.erase(b, strlen("float2 xe_uv = xe_tv[xe_vid & 0xFFFFFu];"));
        b = psrc.find("uint xe_patch_id = xe_inst + (xe_vid >> 20u);");
        psrc.erase(b, strlen("uint xe_patch_id = xe_inst + (xe_vid >> 20u);"));
      }
      psrc += "kernel void xe_probe(constant XeTop& tl [[buffer(2)]], device float4* out [[buffer(3)]], "
              "constant XeTessDraw& td [[buffer(4)]], uint id [[thread_position_in_grid]]) { "
              "uint p = id / 25u, k = id % 25u; "
              "out[id] = xe_ds_body(p, float2(float(k % 5u) * 0.25f, float(k / 5u) * 0.25f), tl, td).pos; }\n";
    }
    id<MTLLibrary> lib_probe = [dev newLibraryWithSource:@(psrc.c_str()) options:opts error:&err];
    if (!lib_probe) { fprintf(stderr, "probe lib: %s\n", err.localizedDescription.UTF8String); return 1; }
    uint32_t mask = [info[@"interpolator_mask"] unsignedIntValue];
    uint32_t count = __builtin_popcount(mask);
    uint32_t passes = (count + 3) / 4 + 1;  // last pass: position
    std::vector<id<MTLLibrary>> frag_libs;
    for (uint32_t p = 0; p < passes; ++p) {
      std::string src = "#include <metal_stdlib>\nusing namespace metal;\nstruct I {\n";
      for (uint32_t i = 0; i < count; ++i) {
        src += "  float4 t" + std::to_string(i) + " [[user(texcoord" + std::to_string(i) + ")]];\n";
      }
      src += "  float4 pos [[position]];\n};\nstruct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; "
             "float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };\nfragment O f(I i [[stage_in]]) { O o; ";
      for (uint32_t k = 0; k < 4; ++k) {
        uint32_t idx = p * 4 + k;
        std::string v = (p == passes - 1) ? (k == 0 ? "i.pos" : "float4(0.0f)")
                        : (idx < count ? "i.t" + std::to_string(idx) : "float4(0.0f)");
        src += "o.c" + std::to_string(k) + " = " + v + "; ";
      }
      src += "return o; }\n";
      id<MTLLibrary> l = [dev newLibraryWithSource:@(src.c_str()) options:nil error:&err];
      if (!l) { fprintf(stderr, "fs: %s\n", err.localizedDescription.UTF8String); return 1; }
      frag_libs.push_back(l);
    }
    IRRuntimeTessellationPipelineConfig tcfg = {};
    tcfg.outputPrimitiveType = IRRuntimeTessellatorOutputPrimitive([cfg[@"output_primitive"] unsignedIntValue]);
    tcfg.vsOutputSizeInBytes = [cfg[@"vs_output_size"] unsignedIntValue];
    tcfg.gsMaxInputPrimitivesPerMeshThreadgroup = [cfg[@"gs_max_input_prims"] unsignedIntValue];
    tcfg.hsMaxPatchesPerObjectThreadgroup = [cfg[@"hs_max_patches"] unsignedIntValue];
    tcfg.hsInputControlPointCount = [cfg[@"hs_input_cp"] unsignedIntValue];
    tcfg.hsMaxObjectThreadsPerThreadgroup = [cfg[@"hs_max_object_threads"] unsignedIntValue];
    tcfg.hsMaxTessellationFactor = [cfg[@"hs_max_tess_factor"] floatValue];
    tcfg.gsInstanceCount = 1;
    std::string vs_fn = [cfg[@"vs_fn"] UTF8String], hs_fn = [cfg[@"hs_fn"] UTF8String],
                ds_fn = [cfg[@"ds_fn"] UTF8String], gs_fn = [cfg[@"gs_fn"] UTF8String];
    const bool ccw = !(getenv("TD_WINDING") && !strcmp(getenv("TD_WINDING"), "cw"));
    auto pipeline = [&](int which, uint32_t pass) -> id<MTLRenderPipelineState> {
      NSError* e = nil;
      id<MTLRenderPipelineState> s = nil;
      if (which == 0) {
        MTLMeshRenderPipelineDescriptor* d = [MTLMeshRenderPipelineDescriptor new];
        for (uint32_t i = 0; i < 4; ++i) d.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA32Float;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        IRGeometryTessellationEmulationPipelineDescriptor ir = {};
        ir.stageInLibrary = lib_stage_in;
        ir.vertexLibrary = lib_vs;
        ir.vertexFunctionName = vs_fn.c_str();
        ir.hullLibrary = lib_hs;
        ir.hullFunctionName = hs_fn.c_str();
        ir.domainLibrary = lib_ds;
        ir.domainFunctionName = ds_fn.c_str();
        ir.geometryLibrary = nil;
        ir.geometryFunctionName = gs_fn.c_str();
        ir.fragmentLibrary = frag_libs[pass];
        ir.fragmentFunctionName = "f";
        ir.basePipelineDescriptor = d;
        ir.pipelineConfig = tcfg;
        s = IRRuntimeNewGeometryTessellationEmulationPipeline(dev, &ir, &e);
      } else if (cpu_pattern) {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = ds_native;
        d.fragmentFunction = [frag_libs[pass] newFunctionWithName:@"f"];
        for (uint32_t i = 0; i < 4; ++i) d.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA32Float;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
      } else {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = ds_native;
        d.fragmentFunction = [frag_libs[pass] newFunctionWithName:@"f"];
        for (uint32_t i = 0; i < 4; ++i) d.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA32Float;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        d.tessellationPartitionMode = MTLTessellationPartitionModeFractionalEven;
        d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        d.tessellationOutputWindingOrder = ccw ? MTLWindingCounterClockwise : MTLWindingClockwise;
        d.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
        d.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
        d.maxTessellationFactor = 64;
        s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
      }
      if (!s) fprintf(stderr, "pipeline %d: %s\n", which, e.localizedDescription.UTF8String);
      return s;
    };
    MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionLessEqual;
    dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depth_state = [dev newDepthStencilStateWithDescriptor:dsd];
    id<MTLBuffer> tables = [dev newBufferWithLength:IRRuntimeTessellatorTablesSize() options:MTLResourceStorageModeShared];
    IRRuntimeLoadTessellatorTables(tables);

    // Factors: effective (after +1 and the clamp) per control point.
    float fmin = 1.0f, fmax = 16.0f, lo = 1.0f, hi = 16.0f;
    bool uniform = false;
    if (const char* f = getenv("TD_FACTORS")) {
      if (sscanf(f, "uniform:%f", &lo) == 1) { hi = lo; uniform = true; }
      else sscanf(f, "range:%f:%f", &lo, &hi);
    }
    fmin = std::min(fmin, lo);
    fmax = std::max(fmax, hi);
    uint32_t edge_map[4] = {3, 0, 1, 2};
    if (const char* m = getenv("TD_EDGE_MAP")) {
      for (uint32_t i = 0; i < 4 && m[i]; ++i) edge_map[i] = uint32_t(m[i] - '0') & 3;
    }
    MTLCullMode cull = MTLCullModeNone;
    if (getenv("TD_CULL")) cull = !strcmp(getenv("TD_CULL"), "front") ? MTLCullModeFront : MTLCullModeBack;

    uint64_t covered = 0, only_one = 0, identical = 0, both = 0, depth_diff = 0;
    double max_diff = 0.0, sum_diff = 0.0;
    uint64_t n_diff = 0;
    for (int seed = 0; seed < seeds; ++seed) {
      std::mt19937 rng(4242u + seed * 7919u);
      std::vector<id<MTLResource>> res;
      auto buffer = [&](size_t size) {
        id<MTLBuffer> b = [dev newBufferWithLength:std::max<size_t>(size, 16) options:MTLResourceStorageModeShared];
        memset(b.contents, 0, b.length);
        res.push_back(b);
        return b;
      };
      id<MTLBuffer> null_buf = buffer(4096);
      id<MTLBuffer> shared = buffer(kSharedBytes);
      uint32_t* sh = (uint32_t*)shared.contents;
      for (uint32_t i = 0; i < kSharedBytes / 4; ++i) {
        if (rng() % 4) { float f = RandomFloat(rng); memcpy(&sh[i], &f, 4); }
        else sh[i] = rng();
      }
      id<MTLBuffer> srv_table = buffer(sizeof(BufEntry) * 4);
      ((BufEntry*)srv_table.contents)[0] = {shared.gpuAddress, 0, kSharedBytes};
      id<MTLBuffer> sysb = buffer(1024);
      uint32_t* su = (uint32_t*)sysb.contents;
      uint32_t sys_size = [sys[@"size"] unsignedIntValue];
      for (uint32_t i = 0; i < sys_size / 4; ++i) { float f = RandomFloat(rng); memcpy(&su[i], &f, 4); }
      auto at = [&](NSString* k) { return [sys[k] unsignedIntValue] / 4; };
      su[at(@"flags")] = rng() & 0xEu;  // never the shared-memory-UAV flag (no memexport here)
      su[at(@"vertex_index_endian")] = 0;
      su[at(@"vertex_index_offset")] = rng() % 64;
      su[at(@"vertex_index_min")] = 0;
      su[at(@"vertex_index_max")] = 0xFFFFFFu;
      su[at(@"line_loop_closing_index")] = 0xFFFFFFFFu;
      memcpy(&su[at(@"tess_factor_min")], &fmin, 4);
      memcpy(&su[at(@"tess_factor_max")], &fmax, 4);
      for (uint32_t i = 0; i < 3; ++i) {
        float s = 1.0f, o = 0.0f;
        memcpy(&su[at(@"ndc_scale") + i], &s, 4);
        memcpy(&su[at(@"ndc_offset") + i], &o, 4);
      }
      id<MTLBuffer> fb = buffer(256 * 16);
      for (uint32_t i = 0; i < 256 * 4; ++i) ((float*)fb.contents)[i] = RandomFloat(rng);
      // TD_FCONST="g:x,y,z,w;..." sets guest float constants (float values), e.g. the terrain domain
      // shader's swizzle selector c255 = 0,1,2,3.
      {
        std::string all = getenv("TD_FCONST") ? getenv("TD_FCONST") : "255:0,1,2,3";
        size_t pos = 0;
        while (pos < all.size()) {
          size_t end = all.find(';', pos);
          std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
          pos = end == std::string::npos ? all.size() : end + 1;
          uint32_t g = 0;
          float v[4] = {};
          if (sscanf(item.c_str(), "%u:%f,%f,%f,%f", &g, &v[0], &v[1], &v[2], &v[3]) != 5) continue;
          for (NSArray* m in info[@"float_map"]) {
            if ([m[0] unsignedIntValue] == g) memcpy((float*)fb.contents + [m[1] unsignedIntValue] * 4, v, 16);
          }
        }
      }
      id<MTLBuffer> bl = buffer(40 * 4);
      for (uint32_t i = 0; i < 8; ++i) ((uint32_t*)bl.contents)[i] = rng();
      for (uint32_t i = 8; i < 40; ++i)
        ((uint32_t*)bl.contents)[i] = (rng() % 5) | ((rng() % 8) << 8) | ((uint32_t(int32_t(rng() % 4) - 1) & 0xFF) << 16);
      id<MTLBuffer> fc = buffer(32 * 6 * 4);
      uint32_t* fw = (uint32_t*)fc.contents;
      for (uint32_t i = 0; i < 32 * 6; ++i) fw[i] = rng();
      for (uint32_t v = 0; v < 96; ++v) {
        fw[v * 2] = (rng() % (kSharedBytes / 2)) & ~3u;
        fw[v * 2 + 1] = (fw[v * 2 + 1] & ~3u) | (rng() % 4);
      }
      id<MTLBuffer> di = buffer(kHeapSlots * 4);
      for (uint32_t i = 0; i < kHeapSlots; ++i) ((uint32_t*)di.contents)[i] = i;
      id<MTLBuffer> view_heap = buffer(kHeapSlots * sizeof(Entry));
      id<MTLBuffer> smp_heap = buffer(kHeapSlots * sizeof(Entry));
      for (NSArray* t in info[@"textures"]) {
        uint32_t slot = [t[0] unsignedIntValue], dim = [t[1] unsignedIntValue];
        MTLTextureDescriptor* td = [MTLTextureDescriptor new];
        td.textureType = dim == 3 ? MTLTextureTypeCube : MTLTextureType2DArray;
        td.pixelFormat = MTLPixelFormatRGBA16Float;
        td.width = td.height = 32;
        td.mipmapLevelCount = 6;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        for (uint32_t m = 0; m < 6; ++m) {
          uint32_t s = 32u >> m;
          for (uint32_t f = 0; f < (dim == 3 ? 6u : 1u); ++f) {
            std::vector<uint16_t> px(s * s * 4);
            for (auto& p : px) p = uint16_t(0x3000 + (rng() % 0x0C00));
            [tex replaceRegion:MTLRegionMake2D(0, 0, s, s) mipmapLevel:m slice:f withBytes:px.data()
                   bytesPerRow:s * 8 bytesPerImage:0];
          }
        }
        res.push_back(tex);
        if (slot < kHeapSlots) ((Entry*)view_heap.contents)[slot] = {0, tex.gpuResourceID._impl, 0};
      }
      for (NSArray* s in info[@"samplers"]) {
        uint32_t slot = [s[0] unsignedIntValue];
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.supportArgumentBuffers = YES;
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeRepeat;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        if (slot < kHeapSlots) ((Entry*)smp_heap.contents)[slot].gpuVA = smp.gpuResourceID._impl;
      }
      id<MTLBuffer> top = buffer(32 * 8);
      uint64_t* tl = (uint64_t*)top.contents;
      for (uint32_t i = 0; i < 32; ++i) tl[i] = null_buf.gpuAddress;
      tl[0] = srv_table.gpuAddress;
      tl[5] = srv_table.gpuAddress;
      tl[1] = tl[2] = tl[3] = tl[4] = view_heap.gpuAddress;
      tl[9] = smp_heap.gpuAddress;
      tl[10] = sysb.gpuAddress;
      tl[11] = fb.gpuAddress;
      tl[12] = bl.gpuAddress;
      tl[13] = fc.gpuAddress;
      tl[14] = di.gpuAddress;
      // The emulation's hull/domain stages read their own copies of the vertex slots.
      tl[15] = fb.gpuAddress; tl[16] = fc.gpuAddress; tl[17] = di.gpuAddress;
      tl[18] = fb.gpuAddress; tl[19] = fc.gpuAddress; tl[20] = di.gpuAddress;
      // Guest index buffer: adaptive, raw factors (effective - 1), little endian (endian 0);
      // continuous, one patch index per patch.
      const bool continuous = mode == 1;
      id<MTLBuffer> index = buffer(patches * 16);
      struct { uint64_t index; uint32_t format, indexed; } td = {0, 0, 0};
      if (continuous) {
        td = {index.gpuAddress, 2u, 1u};
        lo = hi = fmax;
        uniform = true;
      }
      std::vector<float> cp(patches * 4);
      std::uniform_real_distribution<float> fd(lo, hi);
      for (uint32_t i = 0; i < patches * 4; ++i) {
        float eff = uniform ? lo : fd(rng);
        // TD_HALF_EXACT=1: effective factors representable in half precision, so only the
        // tessellator itself (edge mapping, point placement) can differ.
        if (getenv("TD_HALF_EXACT")) eff = float(__fp16(eff));
        // TD_CP="a,b,c,d": fixed effective factors per control point for every patch.
        if (const char* f = getenv("TD_CP")) {
          float v[4] = {};
          if (sscanf(f, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) == 4) eff = v[i & 3];
        }
        // TD_PATCH_UNIFORM=1: one factor per patch (all four edges equal).
        if (getenv("TD_PATCH_UNIFORM") && (i & 3)) eff = ((float*)index.contents)[i & ~3u] + 1.0f;
        cp[i] = std::fmin(std::fmax(eff, fmin), fmax);
        if (!continuous) ((float*)index.contents)[i] = eff - 1.0f;
        else if (i < patches) ((uint32_t*)index.contents)[i] = rng() % 4096;
      }
      id<MTLBuffer> factors = buffer(patches * 12);
      __fp16* hf = (__fp16*)factors.contents;
      for (uint32_t p = 0; p < patches; ++p) {
        const float* c = &cp[p * 4];
        for (uint32_t i = 0; i < 4; ++i) hf[p * 6 + i] = __fp16(c[edge_map[i]]);
        hf[p * 6 + 4] = __fp16(std::fmin(c[2], c[0]));
        hf[p * 6 + 5] = __fp16(std::fmin(c[1], c[3]));
        if (getenv("TD_INSIDE_SWAP") && getenv("TD_INSIDE_SWAP")[0] == '1') std::swap(hf[p * 6 + 4], hf[p * 6 + 5]);
      }
      // CPU pattern: the reference tessellator per patch, with the adaptive hull shader's factors
      // (edge i = control point (i + 3) & 3, inside = min of the opposite edges), CW output.
      id<MTLBuffer> cpu_verts = nil, cpu_indices = nil;
      uint32_t cpu_index_count = 0;
      if (cpu_pattern) {
        d3d11_ref_tessellator::CHWTessellator tess;
        tess.Init(d3d11_ref_tessellator::D3D11_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN,
                  d3d11_ref_tessellator::D3D11_TESSELLATOR_OUTPUT_TRIANGLE_CW);
        // As the game: a uv arena, index = (patch << 20) | arena point.
        struct TV { float u, v; };
        std::vector<TV> verts;
        std::vector<uint32_t> idx;
        for (uint32_t p = 0; p < patches; ++p) {
          const float* c = &cp[p * 4];
          float in_u = std::fmin(c[2], c[0]), in_v = std::fmin(c[1], c[3]);
          if (getenv("TD_INSIDE_SWAP") && getenv("TD_INSIDE_SWAP")[0] == '1') std::swap(in_u, in_v);
          tess.TessellateQuadDomain(c[edge_map[0]], c[edge_map[1]], c[edge_map[2]], c[edge_map[3]], in_u, in_v);
          uint32_t base = uint32_t(verts.size());
          const d3d11_ref_tessellator::DOMAIN_POINT* pts = tess.GetPoints();
          for (int i = 0; i < tess.GetPointCount(); ++i) verts.push_back({pts[i].u, pts[i].v});
          const int* ix = tess.GetIndices();
          for (int i = 0; i < tess.GetIndexCount(); ++i) idx.push_back((p << 20) | (base + uint32_t(ix[i])));
        }
        cpu_verts = buffer(verts.size() * sizeof(TV));
        memcpy(cpu_verts.contents, verts.data(), verts.size() * sizeof(TV));
        cpu_indices = buffer(idx.size() * 4);
        memcpy(cpu_indices.contents, idx.data(), idx.size() * 4);
        cpu_index_count = uint32_t(idx.size());
      }
      // Probe: positions of the native domain function on a 5x5 grid per patch -> NDC scale/offset.
      {
        id<MTLBuffer> pos = buffer(patches * 25 * 16);
        MTLComputePipelineDescriptor* cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = [lib_probe newFunctionWithName:@"xe_probe"];
        id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&err];
        if (!cps) { fprintf(stderr, "probe: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:cps];
        [ce setBuffer:top offset:0 atIndex:2];
        [ce setBuffer:pos offset:0 atIndex:3];
        [ce setBytes:&td length:sizeof(td) atIndex:4];
        for (id<MTLResource> r : res) [ce useResource:r usage:MTLResourceUsageRead];
        [ce dispatchThreads:MTLSizeMake(patches * 25, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const float* p = (const float*)pos.contents;
        if (getenv("TD_DEBUG")) {
          for (uint32_t v = 0; v < 50; v += 6) printf("probe v%u: %g %g %g %g\n", v, p[v*4], p[v*4+1], p[v*4+2], p[v*4+3]);
        }
        std::vector<float> ax[3];
        for (uint32_t v = 0; v < patches * 25; ++v) {
          float w = p[v * 4 + 3];
          if (!(w > 0.0f) || !std::isfinite(w)) continue;
          for (uint32_t c = 0; c < 3; ++c) {
            float q = p[v * 4 + c] / w;
            if (std::isfinite(q)) ax[c].push_back(q);
          }
        }
        for (uint32_t c = 0; c < 3; ++c) {
          float sc = 1.0f, of = c == 2 ? 0.5f : 0.0f;
          if (ax[c].size() >= 2) {
            std::sort(ax[c].begin(), ax[c].end());
            float l = ax[c][ax[c].size() / 10], h = ax[c][ax[c].size() * 9 / 10];
            if (h > l) {
              float tl0 = c == 2 ? 0.1f : -0.9f, th = 0.9f;
              sc = (th - tl0) / (h - l);
              of = tl0 - l * sc;
            }
          }
          memcpy(&su[at(@"ndc_scale") + c], &sc, 4);
          memcpy(&su[at(@"ndc_offset") + c], &of, 4);
        }
      }
      std::vector<float> out[2], dep[2];
      for (int which = 0; which < 2; ++which) {
        for (uint32_t p = 0; p < passes; ++p) {
          id<MTLRenderPipelineState> ps = pipeline(which, p);
          if (!ps) return 1;
          MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
          id<MTLTexture> rts[4];
          for (uint32_t i = 0; i < 4; ++i) {
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                           width:kW height:kH mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModeShared;
            rts[i] = [dev newTextureWithDescriptor:td];
            rp.colorAttachments[i].texture = rts[i];
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(-7, -7, -7, -7);
            rp.colorAttachments[i].storeAction = MTLStoreActionStore;
          }
          MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                         width:kW height:kH mipmapped:NO];
          dd.usage = MTLTextureUsageRenderTarget;
          dd.storageMode = MTLStorageModePrivate;
          id<MTLTexture> dt = [dev newTextureWithDescriptor:dd];
          rp.depthAttachment.texture = dt;
          rp.depthAttachment.loadAction = MTLLoadActionClear;
          rp.depthAttachment.clearDepth = 1.0;
          rp.depthAttachment.storeAction = MTLStoreActionStore;
          id<MTLCommandBuffer> cb = [queue commandBuffer];
          id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
          [enc setRenderPipelineState:ps];
          [enc setDepthStencilState:depth_state];
          [enc setCullMode:cull];
          [enc setFrontFacingWinding:MTLWindingClockwise];
          // TD_CLAMP=1: depth clamp for more coverage; clamped depths tie, and ties then depend on the
          // primitive order inside the draw, which differs between the emulation and the tessellator.
          if (getenv("TD_CLAMP")) [enc setDepthClipMode:MTLDepthClipModeClamp];
          if (which == 0) {
            [enc setObjectBuffer:top offset:0 atIndex:kIRArgumentBufferBindPoint];
            [enc setMeshBuffer:top offset:0 atIndex:kIRArgumentBufferBindPoint];
            [enc setObjectBuffer:top offset:0 atIndex:kIRArgumentBufferHullDomainBindPoint];
            [enc setMeshBuffer:top offset:0 atIndex:kIRArgumentBufferHullDomainBindPoint];
            [enc setFragmentBuffer:top offset:0 atIndex:kIRArgumentBufferBindPoint];
            [enc setObjectBuffer:view_heap offset:0 atIndex:kIRDescriptorHeapBindPoint];
            [enc setMeshBuffer:view_heap offset:0 atIndex:kIRDescriptorHeapBindPoint];
            [enc setObjectBuffer:smp_heap offset:0 atIndex:kIRSamplerHeapBindPoint];
            [enc setMeshBuffer:smp_heap offset:0 atIndex:kIRSamplerHeapBindPoint];
            [enc setObjectBuffer:tables offset:0 atIndex:kIRRuntimeTessellatorTablesBindPoint];
            [enc setMeshBuffer:tables offset:0 atIndex:kIRRuntimeTessellatorTablesBindPoint];
            IRRuntimeVertexBuffers vbs = {};
            [enc setObjectBytes:&vbs length:sizeof(vbs) atIndex:kIRVertexBufferBindPoint];
            [enc useResource:tables usage:MTLResourceUsageRead stages:MTLRenderStageObject | MTLRenderStageMesh];
            for (id<MTLResource> r : res)
              [enc useResource:r usage:MTLResourceUsageRead
                        stages:MTLRenderStageObject | MTLRenderStageMesh | MTLRenderStageFragment];
            IRRuntimeDrawIndexedPatchesTessellationEmulation(
                enc, continuous ? IRRuntimePrimitiveType1ControlPointPatchlist
                                : IRRuntimePrimitiveType4ControlPointPatchlist,
                MTLIndexTypeUInt32, index, tcfg, 1, continuous ? patches : patches * 4, 0, 0, 0);
          } else {
            [enc setVertexBuffer:top offset:0 atIndex:2];
            [enc setVertexBytes:&td length:sizeof(td) atIndex:3];
            [enc setVertexBuffer:view_heap offset:0 atIndex:0];
            [enc setVertexBuffer:smp_heap offset:0 atIndex:1];
            for (id<MTLResource> r : res)
              [enc useResource:r usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
            if (cpu_pattern) {
              [enc setVertexBuffer:cpu_verts offset:0 atIndex:4];
              [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:cpu_index_count
                               indexType:MTLIndexTypeUInt32 indexBuffer:cpu_indices indexBufferOffset:0];
            } else {
              [enc setTessellationFactorBuffer:factors offset:0 instanceStride:0];
              [enc drawPatches:4 patchStart:0 patchCount:patches patchIndexBuffer:nil patchIndexBufferOffset:0
                   instanceCount:1 baseInstance:0];
            }
          }
          [enc endEncoding];
          id<MTLBuffer> dread = [dev newBufferWithLength:kW * kH * 4 options:MTLResourceStorageModeShared];
          id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
          [b copyFromTexture:dt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(kW, kH, 1) toBuffer:dread destinationOffset:0
         destinationBytesPerRow:kW * 4 destinationBytesPerImage:kW * kH * 4];
          [b endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
          if (cb.error) { fprintf(stderr, "gpu: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
          for (uint32_t i = 0; i < 4; ++i) {
            size_t o = out[which].size();
            out[which].resize(o + kW * kH * 4);
            [rts[i] getBytes:out[which].data() + o bytesPerRow:kW * 16
                  fromRegion:MTLRegionMake2D(0, 0, kW, kH) mipmapLevel:0];
          }
          size_t o = dep[which].size();
          dep[which].resize(o + kW * kH);
          memcpy(dep[which].data() + o, dread.contents, kW * kH * 4);
        }
      }
      if (getenv("TD_DEBUG")) {
        for (int w = 0; w < 2; ++w) {
          uint64_t nc = 0, nd = 0;
          for (size_t i = 0; i < size_t(kW) * kH * 4; ++i) nc += out[w][i] != -7.0f;
          for (size_t i = 0; i < size_t(kW) * kH; ++i) nd += dep[w][i] != 1.0f;
          printf("side %d: color values written %llu, depth written %llu\n", w, (unsigned long long)nc, (unsigned long long)nd);
        }
      }
      // Per pixel over all passes/targets: coverage from the depth of pass 0, then values.
      const size_t px_count = size_t(kW) * kH;
      uint64_t s_cov = 0, s_one = 0, s_same = 0;
      for (size_t px = 0; px < px_count; ++px) {
        bool ca = dep[0][px] != 1.0f, cbv = dep[1][px] != 1.0f;
        if (!ca && !cbv) continue;
        ++s_cov;
        if (ca != cbv) { ++s_one; continue; }
        ++both;
        bool same = true;
        for (uint32_t p = 0; p < passes; ++p) {
          if (memcmp(&dep[0][p * px_count + px], &dep[1][p * px_count + px], 4)) {
            same = false;
            ++depth_diff;
          }
          for (uint32_t t = 0; t < 4; ++t) {
            for (uint32_t c = 0; c < 4; ++c) {
              size_t i = (size_t(p) * 4 + t) * px_count * 4 + px * 4 + c;
              float a = out[0][i], bb = out[1][i];
              if (memcmp(&a, &bb, 4)) {
                same = false;
                double d = std::fabs(double(a) - double(bb));
                double rel = d / std::max(1e-6, std::max(std::fabs(double(a)), std::fabs(double(bb))));
                if (std::isfinite(rel)) {
                  max_diff = std::max(max_diff, rel);
                  sum_diff += rel;
                  ++n_diff;
                }
              }
            }
          }
        }
        if (same) ++s_same;
      }
      printf("  seed %d: covered %llu, by one side only %llu (%.2f%%), identical %llu (%.2f%% of covered)\n", seed,
             (unsigned long long)s_cov, (unsigned long long)s_one, s_cov ? 100.0 * s_one / s_cov : 0.0,
             (unsigned long long)s_same, s_cov ? 100.0 * s_same / s_cov : 0.0);
      covered += s_cov;
      only_one += s_one;
      identical += s_same;
    }
    printf("RESULT covered %llu one-side %llu (%.3f%%) identical %llu (%.3f%%) both-covered %llu; "
           "relative diff where both cover: mean %.3g max %.3g over %llu values\n",
           (unsigned long long)covered, (unsigned long long)only_one, covered ? 100.0 * only_one / covered : 0.0,
           (unsigned long long)identical, covered ? 100.0 * identical / covered : 0.0, (unsigned long long)both,
           n_diff ? sum_diff / n_diff : 0.0, max_diff, (unsigned long long)n_diff);
    return identical == covered ? 0 : 3;
  }
}
