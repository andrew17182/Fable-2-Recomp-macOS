// xlate_diff: differential exactness test of a native Xenos->MSL pixel shader
// against the Metal Shader Converter library of the same shader
// (docs/native-translator.md, "Parity"). Diagnostic only.
//
// Both functions use the same ABI (top-level argument buffer at buffer(2),
// user(texcoordN) inputs, color(N) outputs), so one harness drives both with
// identical synthetic inputs: random interpolants per grid cell, random float /
// bool / fetch / system constants (fields that select code paths are chosen
// from their valid values), random textures and samplers in an
// IRDescriptorTableEntry heap. Outputs are compared bit for bit.
//
// Build: clang++ -std=c++20 -fobjc-arc -O2 xlate_diff.mm -framework Metal
//        -framework Foundation -o xlate_diff
// Run:   xlate_diff <msc.metallib> <native.metal> <native.metal.json> [seeds=8]
// Exit:  0 identical, 3 differences, 1 error.
// XD_ZWIDE=1: random depth over 0...0.5 with tiny values (float24 depth tests).
// XD_MSAA=<n> (2 or 4): render with n samples and compare the depth of every
// sample (float24 depth conversion runs per sample; colours are not compared
// in this mode).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr uint32_t kW = 64, kH = 64, kCells = 8;
constexpr uint32_t kHeapSlots = 1024;

struct Entry {
  uint64_t gpuVA, textureViewID, metadata;
};

NSDictionary* LoadJson(const char* path) {
  NSData* d = [NSData dataWithContentsOfFile:@(path)];
  return d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
}

const char* kVertexSource = R"(
#include <metal_stdlib>
using namespace metal;
struct V {
  float4 pos [[position]];
  float4 t0 [[user(texcoord0)]]; float4 t1 [[user(texcoord1)]]; float4 t2 [[user(texcoord2)]];
  float4 t3 [[user(texcoord3)]]; float4 t4 [[user(texcoord4)]]; float4 t5 [[user(texcoord5)]];
  float4 t6 [[user(texcoord6)]]; float4 t7 [[user(texcoord7)]]; float4 t8 [[user(texcoord8)]];
  float4 t9 [[user(texcoord9)]]; float4 t10 [[user(texcoord10)]]; float4 t11 [[user(texcoord11)]];
  float4 t12 [[user(texcoord12)]]; float4 t13 [[user(texcoord13)]]; float4 t14 [[user(texcoord14)]];
  float4 t15 [[user(texcoord15)]];
  float2 sprite [[user(xespritetexcoord0)]];
};
vertex V xd_vs(uint vid [[vertex_id]], const device float4* data [[buffer(0)]]) {
  // 6 vertices per cell, 17 float4 per vertex (position + 16 interpolators).
  const device float4* p = data + vid * 17;
  V o; o.pos = p[0];
  o.t0 = p[1]; o.t1 = p[2]; o.t2 = p[3]; o.t3 = p[4]; o.t4 = p[5]; o.t5 = p[6]; o.t6 = p[7];
  o.t7 = p[8]; o.t8 = p[9]; o.t9 = p[10]; o.t10 = p[11]; o.t11 = p[12]; o.t12 = p[13];
  o.t13 = p[14]; o.t14 = p[15]; o.t15 = p[16]; o.sprite = p[16].zw;
  return o;
}
)";

int g_tame = 0;  // 1: no large or tiny constants (isolates inf/denormal cases)
float RandomValue(std::mt19937& rng) {
  std::uniform_real_distribution<float> u(-2.0f, 2.0f);
  uint32_t k = rng() % 100;
  if (g_tame && k >= 90) return u(rng);
  if (k < 70) return u(rng);
  if (k < 80) return 0.0f;
  if (k < 85) return -0.0f;
  if (k < 90) return 1.0f;
  if (k < 95) return u(rng) * 64.0f;
  return std::ldexp(u(rng), -(int)(rng() % 20));
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc < 4) {
      fprintf(stderr, "usage: xlate_diff <msc.metallib> <native.metal> <json> [seeds]\n");
      return 1;
    }
    int seeds = argc > 4 ? atoi(argv[4]) : 8;
    NSDictionary* info = LoadJson(argv[3]);
    if (!info) {
      fprintf(stderr, "bad json\n");
      return 1;
    }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    NSError* err = nil;
    id<MTLLibrary> libA = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];
    if (!libA) {
      fprintf(stderr, "msc lib: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    NSString* src = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil];
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion3_2;
    opts.mathMode = MTLMathModeSafe;
    id<MTLLibrary> libB = [dev newLibraryWithSource:src options:opts error:&err];
    if (!libB) {
      fprintf(stderr, "native compile: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    id<MTLLibrary> libV = [dev newLibraryWithSource:@(kVertexSource) options:nil error:&err];
    if (!libV) {
      fprintf(stderr, "vs: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    id<MTLFunction> fA = [libA newFunctionWithName:@"main"];
    id<MTLFunction> fB = [libB newFunctionWithName:@"xe_native_ps"];
    id<MTLFunction> vs = [libV newFunctionWithName:@"xd_vs"];
    if (!fA || !fB || !vs) {
      fprintf(stderr, "missing function\n");
      return 1;
    }
    const uint32_t msaa = getenv("XD_MSAA") ? std::max(1, atoi(getenv("XD_MSAA"))) : 1;
    id<MTLComputePipelineState> ms_copy = nil;
    if (msaa > 1) {
      const char* src_ms = R"(
#include <metal_stdlib>
using namespace metal;
kernel void xd_ms_depth(depth2d_ms<float, access::read> d [[texture(0)]], device float* o [[buffer(0)]],
                        constant uint& n [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
  for (uint s = 0; s < n; ++s) o[(g.y * d.get_width() + g.x) * n + s] = d.read(g, s);
}
)";
      id<MTLLibrary> lm = [dev newLibraryWithSource:@(src_ms) options:nil error:&err];
      ms_copy = lm ? [dev newComputePipelineStateWithFunction:[lm newFunctionWithName:@"xd_ms_depth"] error:&err] : nil;
      if (!ms_copy) {
        fprintf(stderr, "ms copy: %s\n", err.localizedDescription.UTF8String);
        return 1;
      }
    }
    uint32_t colors = [info[@"colors"] unsignedIntValue];
    bool depth = [info[@"depth"] boolValue];
    NSDictionary* sys = info[@"sys"];
    uint32_t sys_size = [sys[@"size"] unsignedIntValue];

    auto make_pipeline = [&](id<MTLFunction> f) -> id<MTLRenderPipelineState> {
      MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
      d.vertexFunction = vs;
      d.fragmentFunction = f;
      for (uint32_t i = 0; i < 4; ++i) {
        if (colors & (1u << i))
          d.colorAttachments[i].pixelFormat = msaa > 1 ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA32Float;
      }
      d.rasterSampleCount = msaa;
      if (depth) d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
      NSError* e = nil;
      id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
      if (!p) fprintf(stderr, "pipeline: %s\n", e.localizedDescription.UTF8String);
      return p;
    };
    id<MTLRenderPipelineState> pA = make_pipeline(fA), pB = make_pipeline(fB);
    if (!pA || !pB) return 1;
    MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionAlways;
    dsd.depthWriteEnabled = depth;
    id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dsd];

    uint64_t total_pixels = 0, diff_pixels = 0;
    double max_abs = 0;
    int reported = 0;
    int seed0 = argc > 5 ? atoi(argv[5]) : 0;
    g_tame = argc > 6 ? atoi(argv[6]) : 0;
    for (int seed = seed0; seed < seed0 + seeds; ++seed) {
      std::mt19937 rng(1234u + seed * 7919u);
      std::vector<id<MTLResource>> resources;
      auto buffer = [&](size_t size) {
        id<MTLBuffer> b = [dev newBufferWithLength:std::max<size_t>(size, 16) options:MTLResourceStorageModeShared];
        memset(b.contents, 0, b.length);
        resources.push_back(b);
        return b;
      };
      id<MTLBuffer> null_buf = buffer(4096);
      // System constants.
      id<MTLBuffer> sysb = buffer(std::max(sys_size, 1024u));
      uint32_t* su = (uint32_t*)sysb.contents;
      for (uint32_t i = 0; i < sys_size / 4; ++i) {
        float f = RandomValue(rng);
        memcpy(&su[i], &f, 4);
      }
      auto at = [&](NSString* k) { return [sys[k] unsignedIntValue] / 4; };
      // flags: polygonal(4) / line(5) / alpha func (7..9) / gamma (10..13) / depth float24 (6).
      su[at(@"flags")] = (rng() & (1u << 4)) | (rng() & (1u << 5)) | (rng() & (1u << 6)) |
                         ((rng() % 8) << 7) | (rng() & (0xFu << 10));
      for (uint32_t i = 0; i < 8; ++i) su[at(@"signs") + i] = rng();
      if (const char* fl = getenv("XD_FLAGS")) su[at(@"flags")] = uint32_t(strtoul(fl, nullptr, 0));
      if (const char* sg = getenv("XD_SIGNS")) {
        for (uint32_t i = 0; i < 8; ++i) su[at(@"signs") + i] = uint32_t(atoi(sg)) * 0x55555555u;
      }
      su[at(@"res_scaled")] = rng();
      su[at(@"sample_count_log2")] = rng() % 2;
      su[at(@"sample_count_log2") + 1] = rng() % 2;
      su[at(@"alpha_to_mask")] = (rng() % 3 == 0) ? (0x100u | (rng() & 0xFF)) : 0u;
      for (uint32_t i = 0; i < 4; ++i) {
        float b = std::ldexp(1.0f, int(rng() % 5) - 2);
        memcpy(&su[at(@"color_exp_bias") + i], &b, 4);
      }
      // Float constants (256 float4), bool/loop, fetch, descriptor indices.
      id<MTLBuffer> fb = buffer(256 * 16);
      float* ff = (float*)fb.contents;
      for (uint32_t i = 0; i < 256 * 4; ++i) ff[i] = RandomValue(rng);
      id<MTLBuffer> bl = buffer(40 * 4);
      for (uint32_t i = 0; i < 40; ++i) ((uint32_t*)bl.contents)[i] = rng();
      // Loop constants (dwords 8..39): count 0..4, start aL 0..7, step -1..2 (keeps c[aL+n] in range).
      for (uint32_t i = 8; i < 40; ++i)
        ((uint32_t*)bl.contents)[i] = (rng() % 5) | ((rng() % 8) << 8) | ((uint32_t(int32_t(rng() % 4) - 1) & 0xFF) << 16);
      if (const char* b = getenv("XD_BOOL")) {
        for (uint32_t i = 0; i < 8; ++i) ((uint32_t*)bl.contents)[i] = atoi(b) ? ~0u : 0u;
      }
      id<MTLBuffer> fc = buffer(32 * 6 * 4);
      uint32_t* fw = (uint32_t*)fc.contents;
      for (uint32_t t = 0; t < 32; ++t) {
        uint32_t* w = fw + t * 6;
        w[0] = rng();
        w[1] = rng();
        w[2] = 31u | (31u << 13);  // 32x32
        w[3] = (rng() & ~(0x3Fu << 13)) | ((uint32_t(int32_t(rng() % 5) - 2) & 0x3F) << 13);
        w[4] = (rng() & ~(0x3FFu << 12)) | ((uint32_t(int32_t(rng() % 97) - 48) & 0x3FF) << 12);
        w[5] = (rng() & ~(3u << 9)) | ((rng() % 4 == 0 ? 0u : 1u) << 9);  // 1D or 2D
        if (const char* d = getenv("XD_DIM")) w[5] = (w[5] & ~(3u << 9)) | (uint32_t(atoi(d)) << 9);
        if (getenv("XD_W1D_SMALL")) w[2] = 255u;  // 1D width 256
      }
      id<MTLBuffer> di = buffer(kHeapSlots * 4);
      for (uint32_t i = 0; i < kHeapSlots; ++i) ((uint32_t*)di.contents)[i] = i;
      // Heaps.
      id<MTLBuffer> view_heap = buffer(kHeapSlots * sizeof(Entry));
      id<MTLBuffer> smp_heap = buffer(kHeapSlots * sizeof(Entry));
      Entry* ve = (Entry*)view_heap.contents;
      Entry* se = (Entry*)smp_heap.contents;
      for (NSArray* t in info[@"textures"]) {
        uint32_t slot = [t[0] unsignedIntValue], dim = [t[1] unsignedIntValue];
        bool is_signed = [t[3] boolValue];
        MTLTextureDescriptor* td = [MTLTextureDescriptor new];
        td.textureType = dim == 3 ? MTLTextureTypeCube : MTLTextureType2DArray;
        td.pixelFormat = is_signed ? MTLPixelFormatRGBA8Snorm : MTLPixelFormatRGBA16Float;
        td.width = td.height = 32;
        td.arrayLength = 1;
        td.mipmapLevelCount = 6;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        uint32_t faces = dim == 3 ? 6 : 1;
        for (uint32_t m = 0; m < 6; ++m) {
          uint32_t s = 32u >> m;
          for (uint32_t f = 0; f < faces; ++f) {
            std::vector<uint16_t> px(s * s * 4);
            static const bool tex_const = getenv("XD_TEX_CONST") != nullptr;
            for (auto& p : px) {
              if (tex_const) { p = is_signed ? 0x4040 : 0x3800; continue; }
              p = is_signed ? uint16_t(rng() & 0xFFFF)
                            : uint16_t(0x3000 + (rng() % 0x0C00));  // half in ~[0.125, 1)
            }
            MTLRegion r = MTLRegionMake2D(0, 0, s, s);
            [tex replaceRegion:r mipmapLevel:m slice:f withBytes:px.data()
                   bytesPerRow:s * (is_signed ? 4 : 8) bytesPerImage:0];
          }
        }
        resources.push_back(tex);
        if (slot < kHeapSlots) {
          ve[slot].gpuVA = 0;
          ve[slot].textureViewID = tex.gpuResourceID._impl;
          ve[slot].metadata = 0;
        }
      }
      for (NSArray* s in info[@"samplers"]) {
        uint32_t slot = [s[0] unsignedIntValue];
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.supportArgumentBuffers = YES;
        static const bool point = getenv("XD_POINT") != nullptr;
        sd.minFilter = point ? MTLSamplerMinMagFilterNearest : (rng() & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.magFilter = (rng() & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = (rng() & 1) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeRepeat;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        if (slot < kHeapSlots) se[slot].gpuVA = smp.gpuResourceID._impl;
      }
      // Top-level argument buffer.
      id<MTLBuffer> top = buffer(32 * 8);
      uint64_t* tl = (uint64_t*)top.contents;
      for (uint32_t i = 0; i < 32; ++i) tl[i] = null_buf.gpuAddress;
      tl[0] = view_heap.gpuAddress;
      tl[1] = tl[2] = tl[3] = tl[4] = view_heap.gpuAddress;
      tl[9] = smp_heap.gpuAddress;
      tl[10] = sysb.gpuAddress;
      tl[12] = bl.gpuAddress;
      tl[21] = fb.gpuAddress;
      tl[22] = fc.gpuAddress;
      tl[23] = di.gpuAddress;
      // Geometry: kCells x kCells cells, random interpolants per vertex.
      id<MTLBuffer> vb = buffer(kCells * kCells * 6 * 17 * 16);
      float* vf = (float*)vb.contents;
      size_t v = 0;
      for (uint32_t cy = 0; cy < kCells; ++cy) {
        for (uint32_t cx = 0; cx < kCells; ++cx) {
          float corner[4][16][4];
          for (auto& c : corner)
            for (auto& i : c)
              for (auto& x : i) {
                std::uniform_real_distribution<float> u(-1.5f, 1.5f);
                x = u(rng);
              }
          if (getenv("XD_FLAT")) {  // constant interpolants per cell (uniform control flow)
            for (int c = 1; c < 4; ++c) memcpy(corner[c], corner[0], sizeof(corner[0]));
          }
          const uint32_t idx[6] = {0, 1, 2, 2, 1, 3};
          for (uint32_t k = 0; k < 6; ++k) {
            uint32_t c = idx[k];
            float x = (float(cx + (c & 1)) / kCells) * 2 - 1;
            float y = (float(cy + (c >> 1)) / kCells) * 2 - 1;
            static const bool full = getenv("XD_FULL") != nullptr;
            if (full) {  // one triangle covering the viewport (no interior edges)
              if (cx || cy || k >= 3) { x = y = -5; }
              else { x = k == 1 ? 3.0f : -1.0f; y = k == 2 ? 3.0f : -1.0f; }
            }
            float* o = vf + v * 17 * 4;
            o[0] = x; o[1] = y; o[2] = 0.25f + 0.5f * float(rng() % 1000) / 1000.0f; o[3] = 1;
            static const bool zwide = getenv("XD_ZWIDE") != nullptr;
            if (zwide) {  // depth over the whole host range 0...0.5, small exponents and denormal 20e4 values too
              const float u01 = float(rng() % 100000) / 100000.0f;
              switch (rng() % 4) {
                case 0: o[2] = 0.5f * u01; break;
                case 1: o[2] = std::ldexp(0.5f * u01, -int(rng() % 40)); break;
                case 2: o[2] = (rng() % 2) ? 0.0f : 0.5f; break;
                default: o[2] = 0.5f - std::ldexp(u01, -int(rng() % 24)); break;
              }
            }
            for (uint32_t i = 0; i < 16; ++i)
              for (uint32_t j = 0; j < 4; ++j) o[4 + i * 4 + j] = corner[c][i][j];
            ++v;
          }
        }
      }
      // Render both.
      std::vector<float> out[2][4];
      std::vector<float> dout[2];
      for (int which = 0; which < 2; ++which) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLTexture> rts[4] = {nil, nil, nil, nil};
        for (uint32_t i = 0; i < 4; ++i) {
          if (!(colors & (1u << i))) continue;
          MTLTextureDescriptor* td = [MTLTextureDescriptor
              texture2DDescriptorWithPixelFormat:msaa > 1 ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA32Float
                                           width:kW height:kH mipmapped:NO];
          td.usage = MTLTextureUsageRenderTarget;
          td.storageMode = msaa > 1 ? MTLStorageModePrivate : MTLStorageModeShared;
          if (msaa > 1) {
            td.textureType = MTLTextureType2DMultisample;
            td.sampleCount = msaa;
          }
          rts[i] = [dev newTextureWithDescriptor:td];
          rp.colorAttachments[i].texture = rts[i];
          rp.colorAttachments[i].loadAction = MTLLoadActionClear;
          rp.colorAttachments[i].clearColor = MTLClearColorMake(-7, -7, -7, -7);
          rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        }
        id<MTLTexture> dt = nil;
        if (depth) {
          MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                         width:kW height:kH mipmapped:NO];
          td.usage = MTLTextureUsageRenderTarget | (msaa > 1 ? MTLTextureUsageShaderRead : 0);
          td.storageMode = MTLStorageModePrivate;
          if (msaa > 1) {
            td.textureType = MTLTextureType2DMultisample;
            td.sampleCount = msaa;
          }
          dt = [dev newTextureWithDescriptor:td];
          rp.depthAttachment.texture = dt;
          rp.depthAttachment.loadAction = MTLLoadActionClear;
          rp.depthAttachment.clearDepth = 0.75;
          rp.depthAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:which ? pB : pA];
        [enc setDepthStencilState:ds];
        [enc setVertexBuffer:vb offset:0 atIndex:0];
        [enc setFragmentBuffer:top offset:0 atIndex:2];
        [enc setFragmentBuffer:view_heap offset:0 atIndex:0];
        [enc setFragmentBuffer:smp_heap offset:0 atIndex:1];
        [enc setFragmentBuffer:null_buf offset:0 atIndex:5];
        for (id<MTLResource> r : resources) {
          [enc useResource:r usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
        }
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:v];
        [enc endEncoding];
        id<MTLBuffer> dread = nil;
        if (depth && msaa > 1) {
          dread = [dev newBufferWithLength:kW * kH * msaa * 4 options:MTLResourceStorageModeShared];
          id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
          [ce setComputePipelineState:ms_copy];
          [ce setTexture:dt atIndex:0];
          [ce setBuffer:dread offset:0 atIndex:0];
          [ce setBytes:&msaa length:4 atIndex:1];
          [ce dispatchThreads:MTLSizeMake(kW, kH, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
          [ce endEncoding];
        } else if (depth) {
          dread = [dev newBufferWithLength:kW * kH * 4 options:MTLResourceStorageModeShared];
          id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
          [b copyFromTexture:dt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(kW, kH, 1) toBuffer:dread destinationOffset:0
         destinationBytesPerRow:kW * 4 destinationBytesPerImage:kW * kH * 4];
          [b endEncoding];
        }
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) {
          fprintf(stderr, "gpu error: %s\n", cb.error.localizedDescription.UTF8String);
          return 1;
        }
        for (uint32_t i = 0; i < 4; ++i) {
          if (!rts[i] || msaa > 1) continue;
          out[which][i].resize(kW * kH * 4);
          [rts[i] getBytes:out[which][i].data() bytesPerRow:kW * 16
                fromRegion:MTLRegionMake2D(0, 0, kW, kH) mipmapLevel:0];
        }
        if (dread) {
          dout[which].resize(kW * kH * msaa);
          memcpy(dout[which].data(), dread.contents, kW * kH * msaa * 4);
        }
      }
      if (msaa > 1 && depth && getenv("XD_MSAA_STATS")) {
        // Pixels whose samples hold different depths (per-sample depth).
        for (int w = 0; w < 2; ++w) {
          uint32_t varied = 0;
          for (uint32_t p = 0; p < kW * kH; ++p)
            for (uint32_t sm = 1; sm < msaa; ++sm)
              if (memcmp(&dout[w][p * msaa], &dout[w][p * msaa + sm], 4)) { ++varied; break; }
          printf("  seed %d %s: %u pixels with per-sample depth\n", seed, w ? "native" : "msc", varied);
        }
      }
      for (uint32_t p = 0; p < kW * kH; ++p) {
        bool differ = false;
        for (uint32_t i = 0; i < 4; ++i) {
          if (out[0][i].empty()) continue;
          for (uint32_t c = 0; c < 4; ++c) {
            float a = out[0][i][p * 4 + c], b = out[1][i][p * 4 + c];
            if (memcmp(&a, &b, 4)) {
              differ = true;
              if (std::isfinite(a) && std::isfinite(b)) max_abs = std::max(max_abs, double(std::fabs(a - b)));
              static const bool all = getenv("XD_ALL") != nullptr;
              if (reported < 6 || all) {
                uint32_t ab, bb;
                memcpy(&ab, &a, 4);
                memcpy(&bb, &b, 4);
                printf("  seed %d px (%u,%u) rt%u.%c: msc %.9g (%08X) native %.9g (%08X)\n", seed, p % kW, p / kW, i,
                       "xyzw"[c], a, ab, b, bb);
                ++reported;
              }
            }
          }
        }
        for (uint32_t sm = 0; depth && sm < msaa; ++sm) {
          const uint32_t q = p * msaa + sm;
          if (!memcmp(&dout[0][q], &dout[1][q], 4)) continue;
          differ = true;
          if (reported < 6) {
            printf("  seed %d px (%u,%u) sample %u depth: msc %.9g native %.9g\n", seed, p % kW, p / kW, sm,
                   dout[0][q], dout[1][q]);
            ++reported;
          }
        }
        ++total_pixels;
        diff_pixels += differ;
      }
    }
    printf("RESULT pixels %llu differ %llu (%.4f%%) max_abs %.3g\n", (unsigned long long)total_pixels,
           (unsigned long long)diff_pixels, 100.0 * diff_pixels / std::max<uint64_t>(total_pixels, 1), max_abs);
    return diff_pixels ? 3 : 0;
  }
}
