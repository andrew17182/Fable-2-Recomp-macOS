// xlate_diff_vs: differential exactness test of a native Xenos->MSL vertex
// shader against the Metal Shader Converter library of the same shader
// (docs/native-translator.md, "Parity"). Diagnostic only.
//
// Both vertex functions read the same synthetic state (random shared memory
// with vertex data, vertex fetch / float / bool-loop / system constants,
// textures) through the same ABI. Triangles are rasterized with a generated
// fragment shader that writes the outputs (flat) to four RGBA32F targets per
// pass, plus depth; the images of the two are compared bit for bit, so
// positions, clip distances and every interpolator are covered.
//
// Build: clang++ -std=c++20 -fobjc-arc -O2 xlate_diff_vs.mm -framework Metal
//        -framework Foundation -o xlate_diff_vs
// Run:   xlate_diff_vs <msc.metallib> <native.metal> <native.metal.json> [seeds] [seed0]
// Exit:  0 identical, 3 differences, 1 error.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kW = 64, kH = 64, kTris = 48, kHeapSlots = 1024;
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
}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc < 4) {
      fprintf(stderr, "usage: xlate_diff_vs <msc.metallib> <native.metal> <json> [seeds] [seed0]\n");
      return 1;
    }
    int seeds = argc > 4 ? atoi(argv[4]) : 8;
    int seed0 = argc > 5 ? atoi(argv[5]) : 0;
    NSDictionary* info = [NSJSONSerialization
        JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[3])] options:0 error:nil];
    NSDictionary* sys = info[@"sys"];
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    NSError* err = nil;
    id<MTLLibrary> libA = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion3_2;
    opts.mathMode = MTLMathModeSafe;
    opts.preserveInvariance = YES;
    id<MTLLibrary> libB = [dev newLibraryWithSource:[NSString stringWithContentsOfFile:@(argv[2])
                                                                               encoding:NSUTF8StringEncoding
                                                                                  error:nil]
                                             options:opts
                                               error:&err];
    if (!libA || !libB) {
      fprintf(stderr, "library: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    // Probe library: the native vertex function as a plain function called from a kernel.
    std::string psrc = [[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil] UTF8String];
    {
      size_t f = psrc.find("vertex XeVOut xe_native_vs(");
      if (f == std::string::npos) { fprintf(stderr, "not a native VS\n"); return 1; }
      size_t e = psrc.find(") {", f);
      psrc.replace(f, e - f, "static XeVOut xe_vs_body(uint xe_vertex_id, uint xe_base_vertex, constant XeTop& xe_tl");
      psrc += "kernel void xe_probe(constant XeTop& tl [[buffer(2)]], device float4* out [[buffer(3)]], "
              "uint id [[thread_position_in_grid]]) { out[id] = xe_vs_body(id, 0u, tl).pos; }\n";
    }
    id<MTLLibrary> libP = [dev newLibraryWithSource:@(psrc.c_str()) options:opts error:&err];
    if (!libP) { fprintf(stderr, "probe lib: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLFunction> probe_fn = [libP newFunctionWithName:@"xe_probe"];
    id<MTLFunction> vA = [libA newFunctionWithName:@"main"];
    id<MTLFunction> vB = [libB newFunctionWithName:@"xe_native_vs"];
    if (!vA || !vB) {
      fprintf(stderr, "missing function\n");
      return 1;
    }
    uint32_t mask = [info[@"interpolator_mask"] unsignedIntValue];
    uint32_t count = __builtin_popcount(mask);
    uint32_t passes = (count + 3) / 4 + 1;  // last pass: position
    // Fragment shaders, one per pass.
    std::vector<id<MTLFunction>> frags;
    for (uint32_t p = 0; p < passes; ++p) {
      std::string src = "#include <metal_stdlib>\nusing namespace metal;\nstruct I {\n";
      for (uint32_t i = 0; i < count; ++i) {
        src += "  float4 t" + std::to_string(i) + " [[user(texcoord" + std::to_string(i) + "), flat]];\n";
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
      if (!l) {
        fprintf(stderr, "fs: %s\n", err.localizedDescription.UTF8String);
        return 1;
      }
      frags.push_back([l newFunctionWithName:@"f"]);
    }
    auto pipeline = [&](id<MTLFunction> v, id<MTLFunction> f) -> id<MTLRenderPipelineState> {
      MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
      d.vertexFunction = v;
      d.fragmentFunction = f;
      for (uint32_t i = 0; i < 4; ++i) d.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA32Float;
      d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
      NSError* e = nil;
      id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
      if (!s) fprintf(stderr, "pipeline: %s\n", e.localizedDescription.UTF8String);
      return s;
    };
    MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionLessEqual;
    dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dsd];

    uint64_t total = 0, diff = 0, covered = 0;
    int reported = 0;
    for (int seed = seed0; seed < seed0 + seeds; ++seed) {
      std::mt19937 rng(777u + seed * 7919u);
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
        if (rng() % 4) {
          float f = RandomFloat(rng);
          memcpy(&sh[i], &f, 4);
        } else {
          sh[i] = rng();
        }
      }
      id<MTLBuffer> srv_table = buffer(sizeof(BufEntry) * 4);
      ((BufEntry*)srv_table.contents)[0] = {shared.gpuAddress, 0, kSharedBytes};
      id<MTLBuffer> sysb = buffer(1024);
      uint32_t* su = (uint32_t*)sysb.contents;
      uint32_t sys_size = [sys[@"size"] unsignedIntValue];
      for (uint32_t i = 0; i < sys_size / 4; ++i) {
        float f = RandomFloat(rng);
        memcpy(&su[i], &f, 4);
      }
      auto at = [&](NSString* k) { return [sys[k] unsignedIntValue] / 4; };
      // flags: shared memory UAV(0), XY/W(1), Z/W(2), W not reciprocal(3).
      su[at(@"flags")] = rng() & 0xFu;
      if (getenv("XD_FLAGS")) su[at(@"flags")] = uint32_t(strtoul(getenv("XD_FLAGS"), nullptr, 0));
      if (getenv("XD_NOLOOPCLOSE")) su[at(@"line_loop_closing_index")] = 0xFFFFFFFFu;
      su[at(@"vertex_index_endian")] = getenv("XD_ENDIAN") ? uint32_t(atoi(getenv("XD_ENDIAN"))) : 0u;
      su[at(@"vertex_index_offset")] = rng() % 64;
      su[at(@"vertex_index_min")] = 0;
      su[at(@"vertex_index_max")] = 0xFFFFFFu;
      su[at(@"line_loop_closing_index")] = (rng() % 4 == 0 && !getenv("XD_NOLOOPCLOSE")) ? 5u : 0xFFFFFFFFu;
      // NDC scale 1 / offset 0 for the probe below, which then picks a scale and offset that put most
      // vertices on screen (NDC = scale * xyz / w + offset).
      for (uint32_t i = 0; i < 3; ++i) {
        float s = 1.0f, o = 0.0f;
        memcpy(&su[at(@"ndc_scale") + i], &s, 4);
        memcpy(&su[at(@"ndc_offset") + i], &o, 4);
      }
      id<MTLBuffer> fb = buffer(256 * 16);
      for (uint32_t i = 0; i < 256 * 4; ++i) ((float*)fb.contents)[i] = RandomFloat(rng);
      // XD_FCONST="g:x,y,z,w;g:..." sets guest float constants by raw bits (e.g. a memexport stream).
      if (const char* fcs = getenv("XD_FCONST")) {
        std::string all(fcs);
        size_t pos = 0;
        while (pos < all.size()) {
          size_t end = all.find(';', pos);
          std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
          pos = end == std::string::npos ? all.size() : end + 1;
          uint32_t g = 0, v[4] = {};
          if (sscanf(item.c_str(), "%u:%i,%i,%i,%i", &g, (int*)&v[0], (int*)&v[1], (int*)&v[2], (int*)&v[3]) != 5) continue;
          for (NSArray* m in info[@"float_map"]) {
            if ([m[0] unsignedIntValue] == g) memcpy((uint32_t*)fb.contents + [m[1] unsignedIntValue] * 4, v, 16);
          }
        }
      }
      id<MTLBuffer> bl = buffer(40 * 4);
      for (uint32_t i = 0; i < 8; ++i) ((uint32_t*)bl.contents)[i] = rng();
      for (uint32_t i = 8; i < 40; ++i)
        ((uint32_t*)bl.contents)[i] = (rng() % 5) | ((rng() % 8) << 8) | ((uint32_t(int32_t(rng() % 4) - 1) & 0xFF) << 16);
      // Vertex fetch constants (2 dwords each) for 96 slots; texture fetch
      // constants share the buffer (6 dwords each).
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
      id<MTLBuffer> draw_args = buffer(64);
      // Probe: the native shader's positions, computed in a kernel (the MSL is ours, so its vertex
      // function can be turned into a plain function).
      {
        id<MTLBuffer> pos = buffer(kTris * 3 * 16);
        MTLComputePipelineDescriptor* cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = probe_fn;
        id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&err];
        if (!cps) { fprintf(stderr, "probe: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:cps];
        [ce setBuffer:top offset:0 atIndex:2];
        [ce setBuffer:pos offset:0 atIndex:3];
        for (id<MTLResource> r : res) [ce useResource:r usage:MTLResourceUsageRead];
        [ce dispatchThreads:MTLSizeMake(kTris * 3, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const float* p = (const float*)pos.contents;
        std::vector<float> ax[3];
        for (uint32_t v = 0; v < kTris * 3; ++v) {
          float w = p[v * 4 + 3];
          if (!(w > 0.0f) || !std::isfinite(w)) continue;
          for (uint32_t c = 0; c < 3; ++c) {
            float q = p[v * 4 + c] / w;
            if (std::isfinite(q)) ax[c].push_back(q);
          }
        }
        if (getenv("XD_DEBUG")) {
          for (uint32_t v = 0; v < 6; ++v) printf("probe v%u: %g %g %g %g\n", v, p[v*4], p[v*4+1], p[v*4+2], p[v*4+3]);
          printf("valid %zu\n", ax[0].size());
        }
        for (uint32_t c = 0; c < 3; ++c) {
          float sc = 1.0f, of = c == 2 ? 0.5f : 0.0f;
          if (ax[c].size() >= 2) {
            std::sort(ax[c].begin(), ax[c].end());
            float lo = ax[c][ax[c].size() / 10], hi = ax[c][ax[c].size() * 9 / 10];
            if (hi > lo) {
              float target_lo = c == 2 ? 0.1f : -0.9f, target_hi = 0.9f;
              sc = (target_hi - target_lo) / (hi - lo);
              of = target_lo - lo * sc;
            }
          }
          memcpy(&su[at(@"ndc_scale") + c], &sc, 4);
          memcpy(&su[at(@"ndc_offset") + c], &of, 4);
        }
      }
      std::vector<float> out[2];
      std::vector<float> dep[2];
      // Memory export: compare the shared memory after each version's draws.
      std::vector<uint32_t> shared_init(sh, sh + kSharedBytes / 4), shared_after[2];
      for (int which = 0; which < 2; ++which) {
        memcpy(sh, shared_init.data(), kSharedBytes);
        for (uint32_t p = 0; p < passes; ++p) {
          id<MTLRenderPipelineState> ps = pipeline(which ? vB : vA, frags[p]);
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
          [enc setDepthStencilState:ds];
          [enc setVertexBuffer:top offset:0 atIndex:2];
          [enc setVertexBuffer:view_heap offset:0 atIndex:0];
          [enc setVertexBuffer:smp_heap offset:0 atIndex:1];
          [enc setVertexBuffer:draw_args offset:0 atIndex:4];
          [enc setVertexBuffer:null_buf offset:0 atIndex:5];
          for (id<MTLResource> r : res) [enc useResource:r usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageVertex];
          [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:kTris * 3];
          [enc endEncoding];
          id<MTLBuffer> dread = [dev newBufferWithLength:kW * kH * 4 options:MTLResourceStorageModeShared];
          id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
          [b copyFromTexture:dt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(kW, kH, 1) toBuffer:dread destinationOffset:0
         destinationBytesPerRow:kW * 4 destinationBytesPerImage:kW * kH * 4];
          [b endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
          if (cb.error) {
            fprintf(stderr, "gpu: %s\n", cb.error.localizedDescription.UTF8String);
            return 1;
          }
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
        shared_after[which].assign(sh, sh + kSharedBytes / 4);
      }
      uint64_t mem_diff = 0, mem_changed = 0;
      for (size_t i = 0; i < shared_init.size(); ++i) {
        if (shared_after[0][i] != shared_init[i]) ++mem_changed;
        if (shared_after[0][i] != shared_after[1][i]) {
          if (mem_diff < 4) printf("  seed %d shared dword %zu: msc %08X native %08X (was %08X)\n", seed, i,
                                   shared_after[0][i], shared_after[1][i], shared_init[i]);
          ++mem_diff;
        }
      }
      if (mem_changed || mem_diff) printf("  seed %d memexport: %llu dwords written, %llu differ\n", seed,
                                          (unsigned long long)mem_changed, (unsigned long long)mem_diff);
      diff += mem_diff;
      size_t n = out[0].size();
      for (size_t i = 0; i < n; ++i) {
        if (out[0][i] != -7.0f) ++covered;
        if (memcmp(&out[0][i], &out[1][i], 4)) {
          ++diff;
          if (reported < 6 || getenv("XD_ALL")) {
            size_t img = i / (kW * kH * 4), px = (i / 4) % (kW * kH);
            printf("  seed %d pass/rt %zu px (%zu,%zu).%c: msc %.9g native %.9g\n", seed, img, px % kW, px / kW,
                   "xyzw"[i % 4], out[0][i], out[1][i]);
            ++reported;
          }
        }
        ++total;
      }
      for (size_t i = 0; i < dep[0].size(); ++i) {
        if (memcmp(&dep[0][i], &dep[1][i], 4)) {
          ++diff;
          if (reported < 6 || getenv("XD_ALL")) {
            printf("  seed %d depth %zu: msc %.9g native %.9g\n", seed, i, dep[0][i], dep[1][i]);
            ++reported;
          }
        }
      }
    }
    printf("RESULT values %llu covered %llu differ %llu\n", (unsigned long long)total, (unsigned long long)covered,
           (unsigned long long)diff);
    return diff ? 3 : 0;
  }
}
