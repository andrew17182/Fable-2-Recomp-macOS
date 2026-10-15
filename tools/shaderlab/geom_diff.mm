// geom_diff: differential test of FABLE2_NATIVE_POINTS / FABLE2_NATIVE_RECTS (point sprites and
// rectangles expanded by the native vertex shader as triangle-strip instances) against the Metal Shader Converter's geometry emulation of the same
// point-list draw (docs/native-translator.md, "Geometry and tessellation emulation"). Diagnostic only.
//
// A: the converter's emulation pipeline from the stages the game dumps with FABLE2_NATIVE_TESS_DUMP=<dir>
//    (geomvs_*, gs_k*, geom_*.json); B: the native expansion (fable2-msl-xlate --points 1|2 or --rects 1|2).
// Same synthetic state, a generated fragment shader writing every output (and the point coordinate) to
// RGBA32F targets, plus depth; pixel-exact comparison as in tess_diff.
//
// Build: clang++ -std=c++20 -fobjc-arc -O2 -I<msc include> geom_diff.mm -framework Metal -framework Foundation
// Run:   geom_diff <dumpdir> <vs hash> <mod hex> <key hex8> <native.metal> <native.json> [seeds] [points or rectangles]
// Env:   GD_DIAMETER=<guest pixels> (constant diameter, default 6), GD_MIN=<per-vertex minimum, default 2>,
//        GD_W=<width> (default 256),
//        GD_FCONST=g:x,y,z,w;..., GD_DEBUG=1
// Exit:  0 identical, 3 differences, 1 error.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define IR_PRIVATE_IMPLEMENTATION
#include "metal_irconverter_runtime.h"
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
    if (argc < 7) {
      fprintf(stderr, "usage: geom_diff <dumpdir> <vs hash> <mod hex> <key hex8> <native.metal> <native.json> [seeds] [points]\n");
      return 1;
    }
    std::string dir = argv[1], hash = argv[2], mod = argv[3], key = argv[4];
    int seeds = argc > 7 ? atoi(argv[7]) : 4;
    uint32_t points = argc > 8 ? uint32_t(atoi(argv[8])) : 256;
    const uint32_t kW = getenv("GD_W") ? uint32_t(atoi(getenv("GD_W"))) : 256u, kH = kW;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    NSError* err = nil;
    std::string base = hash + "_" + mod + "_k" + key;
    NSDictionary* cfg = [NSJSONSerialization
        JSONObjectWithData:[NSData dataWithContentsOfFile:@((dir + "/geom_" + base + ".json").c_str())] options:0 error:nil];
    NSDictionary* info = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[6])]
                                                         options:0 error:nil];
    if (!cfg || !info) { fprintf(stderr, "missing geom json or sidecar\n"); return 1; }
    NSDictionary* sys = info[@"sys"];
    const uint32_t key_bits = uint32_t(strtoul(key.c_str(), nullptr, 16));
    const uint32_t interp_count = (key_bits >> 2) & 31;
    const bool has_point_coords = (key_bits >> 13) & 1;
    const bool rects = (key_bits & 3) == 2;  // else point list
    const char* entry = rects ? "xe_native_vs_rects" : "xe_native_vs_points";
    auto lib = [&](const std::string& name) -> id<MTLLibrary> {
      NSError* e = nil;
      id<MTLLibrary> l = [dev newLibraryWithURL:[NSURL fileURLWithPath:@((dir + "/" + name).c_str())] error:&e];
      if (!l) fprintf(stderr, "%s: %s\n", name.c_str(), e.localizedDescription.UTF8String);
      return l;
    };
    id<MTLLibrary> lib_stage_in = lib("geomvs_" + base + ".stagein.metallib");
    id<MTLLibrary> lib_vs = lib("geomvs_" + base + ".metallib");
    id<MTLLibrary> lib_gs = lib("gs_k" + key + ".metallib");
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion3_2;
    opts.mathMode = MTLMathModeSafe;
    opts.preserveInvariance = YES;
    std::string nsrc = ReadText(argv[5]);
    id<MTLLibrary> lib_native = [dev newLibraryWithSource:@(nsrc.c_str()) options:opts error:&err];
    if (!lib_stage_in || !lib_vs || !lib_gs || !lib_native) {
      if (!lib_native) fprintf(stderr, "native: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    // Mesh variant (fable2-msl-xlate --points 3|4): the VS runs once per point.
    const bool mesh_variant = nsrc.find("xe_native_ms_points") != std::string::npos;
    id<MTLFunction> vs_native = mesh_variant ? [lib_native newFunctionWithName:@"xe_native_ms_points"]
                                             : [lib_native newFunctionWithName:@(entry)];
    // Probe: the native expansion as a plain function (corner 0 of each point) to place points on screen.
    std::string psrc = nsrc;
    if (mesh_variant) {
      psrc += "kernel void xe_probe(constant XeTop& tl [[buffer(2)]], constant XePointDraw& pd [[buffer(3)]], "
              "device float4* out [[buffer(4)]], uint id [[thread_position_in_grid]]) { float2 r; bool k; "
              "out[id] = xe_vs_body(id, 0u, tl, r, k).pos; }\n";
    } else {
      size_t f = psrc.find(std::string("vertex XeVOut ") + entry + "(");
      if (f == std::string::npos || !vs_native) { fprintf(stderr, "not a native point/rect expansion\n"); return 1; }
      size_t e = psrc.find(") {", f);
      psrc.replace(f, e - f, std::string("static XeVOut xe_pts_body(uint xe_corner, uint ") +
                                 (rects ? "xe_rect" : "xe_point") +
                                 ", constant XeTop& xe_tl, constant XePointDraw& xe_pd");
      psrc += "kernel void xe_probe(constant XeTop& tl [[buffer(2)]], constant XePointDraw& pd [[buffer(3)]], "
              "device float4* out [[buffer(4)]], uint id [[thread_position_in_grid]]) { "
              "out[id] = xe_pts_body(0u, id, tl, pd).pos; }\n";
    }
    id<MTLLibrary> lib_probe = [dev newLibraryWithSource:@(psrc.c_str()) options:opts error:&err];
    if (!lib_probe) { fprintf(stderr, "probe lib: %s\n", err.localizedDescription.UTF8String); return 1; }
    uint32_t passes = (interp_count + 1 + 3) / 4 + 1;  // interpolators + point coord, then position
    std::vector<id<MTLLibrary>> frag_libs;
    for (uint32_t p = 0; p < passes; ++p) {
      std::string src = "#include <metal_stdlib>\nusing namespace metal;\nstruct I {\n";
      for (uint32_t i = 0; i < interp_count; ++i) {
        src += "  float4 t" + std::to_string(i) + " [[user(texcoord" + std::to_string(i) + ")]];\n";
      }
      if (has_point_coords) src += "  float2 pc [[user(xespritetexcoord0)]];\n";
      src += "  float4 pos [[position]];\n};\nstruct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; "
             "float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };\nfragment O f(I i [[stage_in]]) { O o; ";
      for (uint32_t k = 0; k < 4; ++k) {
        uint32_t idx = p * 4 + k;
        std::string v = "float4(0.0f)";
        if (p == passes - 1) { if (k == 0) v = "i.pos"; }
        else if (idx < interp_count) v = "i.t" + std::to_string(idx);
        else if (idx == interp_count && has_point_coords) v = "float4(i.pc, 0.0f, 0.0f)";
        src += "o.c" + std::to_string(k) + " = " + v + "; ";
      }
      src += "return o; }\n";
      id<MTLLibrary> l = [dev newLibraryWithSource:@(src.c_str()) options:nil error:&err];
      if (!l) { fprintf(stderr, "fs: %s\n", err.localizedDescription.UTF8String); return 1; }
      frag_libs.push_back(l);
    }
    IRRuntimeGeometryPipelineConfig gcfg = {};
    gcfg.gsVertexSizeInBytes = [cfg[@"vertex_size"] unsignedIntValue];
    gcfg.gsMaxInputPrimitivesPerMeshThreadgroup = [cfg[@"max_input_prims"] unsignedIntValue];
    std::string vs_fn = [cfg[@"vs_fn"] UTF8String], gs_fn = [cfg[@"gs_fn"] UTF8String];
    auto pipeline = [&](int which, uint32_t pass) -> id<MTLRenderPipelineState> {
      NSError* e = nil;
      id<MTLRenderPipelineState> s = nil;
      if (which == 0) {
        MTLMeshRenderPipelineDescriptor* d = [MTLMeshRenderPipelineDescriptor new];
        for (uint32_t i = 0; i < 4; ++i) d.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA32Float;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        IRGeometryEmulationPipelineDescriptor ir = {};
        ir.stageInLibrary = lib_stage_in;
        ir.vertexLibrary = lib_vs;
        ir.vertexFunctionName = vs_fn.c_str();
        ir.geometryLibrary = lib_gs;
        ir.geometryFunctionName = gs_fn.c_str();
        ir.fragmentLibrary = frag_libs[pass];
        ir.fragmentFunctionName = "f";
        ir.basePipelineDescriptor = d;
        ir.pipelineConfig = gcfg;
        s = IRRuntimeNewGeometryEmulationPipeline(dev, &ir, &e);
      } else if (mesh_variant) {
        MTLMeshRenderPipelineDescriptor* d = [MTLMeshRenderPipelineDescriptor new];
        d.meshFunction = vs_native;
        d.fragmentFunction = [frag_libs[pass] newFunctionWithName:@"f"];
        for (uint32_t i = 0; i < 4; ++i) d.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA32Float;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        s = [dev newRenderPipelineStateWithMeshDescriptor:d options:0 reflection:nil error:&e];
      } else {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vs_native;
        d.fragmentFunction = [frag_libs[pass] newFunctionWithName:@"f"];
        for (uint32_t i = 0; i < 4; ++i) d.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA32Float;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
      }
      if (!s) fprintf(stderr, "pipeline %d: %s\n", which, e.localizedDescription.UTF8String);
      return s;
    };
    MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionLessEqual;
    dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depth_state = [dev newDepthStencilStateWithDescriptor:dsd];
    float diameter = getenv("GD_DIAMETER") ? float(atof(getenv("GD_DIAMETER"))) : 6.0f;

    uint64_t covered = 0, only_one = 0, identical = 0, both = 0;
    double max_diff = 0.0, sum_diff = 0.0;
    uint64_t n_diff = 0;
    for (int seed = 0; seed < seeds; ++seed) {
      std::mt19937 rng(9001u + seed * 7919u);
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
      auto setf = [&](NSString* k, uint32_t i, float v) { memcpy(&su[at(k) + i], &v, 4); };
      su[at(@"flags")] = rng() & 0xEu;
      su[at(@"vertex_index_endian")] = 0;
      su[at(@"vertex_index_offset")] = rng() % 64;
      su[at(@"vertex_index_min")] = 0;
      su[at(@"vertex_index_max")] = 0xFFFFFFu;
      su[at(@"line_loop_closing_index")] = 0xFFFFFFFFu;
      // Point sizes: a constant diameter in guest pixels, its conversion to NDC radius for this target, and
      // the per-vertex clamp; GD_ZERO=1 uses a zero constant size (culled unless the VS sets a size).
      float d = getenv("GD_ZERO") ? 0.0f : diameter;
      setf(@"point_constant_diameter", 0, d);
      setf(@"point_constant_diameter", 1, d * 0.75f);
      setf(@"point_ndc_radius", 0, 1.0f / float(kW));
      setf(@"point_ndc_radius", 1, 1.0f / float(kH));
      setf(@"point_diameter_min", 0, getenv("GD_MIN") ? float(atof(getenv("GD_MIN"))) : 2.0f);
      setf(@"point_diameter_max", 0, 64.0f);
      for (uint32_t i = 0; i < 3; ++i) { setf(@"ndc_scale", i, 1.0f); setf(@"ndc_offset", i, 0.0f); }
      id<MTLBuffer> fb = buffer(256 * 16);
      for (uint32_t i = 0; i < 256 * 4; ++i) ((float*)fb.contents)[i] = RandomFloat(rng);
      if (const char* fcs = getenv("GD_FCONST")) {
        std::string all(fcs);
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
      struct { uint64_t index; uint32_t format, count; } pd = {0, 0, points};
      // Probe: corner 0 of every point -> NDC scale/offset.
      {
        id<MTLBuffer> pos = buffer(points * 16);
        MTLComputePipelineDescriptor* cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = [lib_probe newFunctionWithName:@"xe_probe"];
        id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&err];
        if (!cps) { fprintf(stderr, "probe: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:cps];
        [ce setBuffer:top offset:0 atIndex:2];
        [ce setBytes:&pd length:sizeof(pd) atIndex:3];
        [ce setBuffer:pos offset:0 atIndex:4];
        for (id<MTLResource> r : res) [ce useResource:r usage:MTLResourceUsageRead];
        [ce dispatchThreads:MTLSizeMake(points, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const float* p = (const float*)pos.contents;
        if (getenv("GD_DEBUG")) {
          for (uint32_t v = 0; v < 6; ++v) printf("probe p%u: %g %g %g %g\n", v, p[v*4], p[v*4+1], p[v*4+2], p[v*4+3]);
        }
        std::vector<float> ax[3];
        for (uint32_t v = 0; v < points; ++v) {
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
          setf(@"ndc_scale", c, sc);
          setf(@"ndc_offset", c, of);
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
          [enc setDepthClipMode:MTLDepthClipModeClamp];
          if (which == 0) {
            [enc setObjectBuffer:top offset:0 atIndex:kIRArgumentBufferBindPoint];
            [enc setMeshBuffer:top offset:0 atIndex:kIRArgumentBufferBindPoint];
            [enc setFragmentBuffer:top offset:0 atIndex:kIRArgumentBufferBindPoint];
            [enc setObjectBuffer:view_heap offset:0 atIndex:kIRDescriptorHeapBindPoint];
            [enc setMeshBuffer:view_heap offset:0 atIndex:kIRDescriptorHeapBindPoint];
            [enc setObjectBuffer:smp_heap offset:0 atIndex:kIRSamplerHeapBindPoint];
            [enc setMeshBuffer:smp_heap offset:0 atIndex:kIRSamplerHeapBindPoint];
            IRRuntimeVertexBuffers vbs = {};
            [enc setObjectBytes:&vbs length:sizeof(vbs) atIndex:kIRVertexBufferBindPoint];
            for (id<MTLResource> r : res)
              [enc useResource:r usage:MTLResourceUsageRead
                        stages:MTLRenderStageObject | MTLRenderStageMesh | MTLRenderStageFragment];
            IRRuntimeDrawPrimitivesGeometryEmulation(enc, rects ? IRRuntimePrimitiveTypeTriangle
                                                                : IRRuntimePrimitiveTypePoint,
                                                     gcfg, 1, rects ? points * 3 : points, 0, 0);
          } else if (mesh_variant) {
            [enc setMeshBuffer:top offset:0 atIndex:2];
            [enc setMeshBytes:&pd length:sizeof(pd) atIndex:3];
            for (id<MTLResource> r : res)
              [enc useResource:r usage:MTLResourceUsageRead
                        stages:MTLRenderStageObject | MTLRenderStageMesh | MTLRenderStageFragment];
            [enc drawMeshThreadgroups:MTLSizeMake((points + 63) / 64, 1, 1)
                threadsPerObjectThreadgroup:MTLSizeMake(64, 1, 1)
                  threadsPerMeshThreadgroup:MTLSizeMake(64, 1, 1)];
          } else {
            [enc setVertexBuffer:top offset:0 atIndex:2];
            [enc setVertexBytes:&pd length:sizeof(pd) atIndex:3];
            [enc setVertexBuffer:view_heap offset:0 atIndex:0];
            [enc setVertexBuffer:smp_heap offset:0 atIndex:1];
            for (id<MTLResource> r : res)
              [enc useResource:r usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:points];
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
      const size_t px_count = size_t(kW) * kH;
      uint64_t s_cov = 0, s_one = 0, s_same = 0;
      int reported = 0;
      for (size_t px = 0; px < px_count; ++px) {
        bool ca = dep[0][px] != 1.0f, cbv = dep[1][px] != 1.0f;
        if (!ca && !cbv) continue;
        ++s_cov;
        if (ca != cbv) { ++s_one; continue; }
        ++both;
        bool same = true;
        for (uint32_t p = 0; p < passes; ++p) {
          if (memcmp(&dep[0][p * px_count + px], &dep[1][p * px_count + px], 4)) same = false;
          for (uint32_t t = 0; t < 4; ++t) {
            for (uint32_t c = 0; c < 4; ++c) {
              size_t i = (size_t(p) * 4 + t) * px_count * 4 + px * 4 + c;
              float a = out[0][i], bb = out[1][i];
              if (memcmp(&a, &bb, 4)) {
                same = false;
                if (getenv("GD_DEBUG") && reported < 6) {
                  printf("  px (%zu,%zu) pass %u rt %u .%c: msc %.9g native %.9g\n", px % kW, px / kW, p, t, "xyzw"[c], a, bb);
                  ++reported;
                }
                double dd2 = std::fabs(double(a) - double(bb));
                double rel = dd2 / std::max(1e-6, std::max(std::fabs(double(a)), std::fabs(double(bb))));
                if (std::isfinite(rel)) { max_diff = std::max(max_diff, rel); sum_diff += rel; ++n_diff; }
              }
            }
          }
        }
        if (same) ++s_same;
      }
      printf("  seed %d: covered %llu, by one side only %llu, identical %llu (%.2f%% of covered)\n", seed,
             (unsigned long long)s_cov, (unsigned long long)s_one, (unsigned long long)s_same,
             s_cov ? 100.0 * s_same / s_cov : 0.0);
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
