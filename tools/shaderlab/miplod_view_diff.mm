// [mip-lod] Does a mip-chain draw read level N + 1 on this GPU?
// (docs/mip-lod-20261013.md, "Exactness of the view limit").
//
// Renders the guest's mip-chain draw shape (a quad covering the level-(N + 1)
// target, UV 0..1, computed LOD with the fetch constant's -1 bias, trilinear,
// derivatives as the native translator emits them: gradient2d(dfdx(uv) * 2^bias))
// from a texture whose level N + 1 holds poison, and compares:
//   A: full view, poison 1;  B: full view, poison 2;  C: view of levels 0..N.
// A == B means the hardware gives level N + 1 zero weight; A == C means the
// limited view changes no pixel. Scales 1..3, levels 1..3, sub-pixel quad
// offsets and both triangle orders. MLV_BIAS=-0.9 is the negative control.
//
// Build: clang++ -std=c++20 -fobjc-arc -O2 miplod_view_diff.mm -framework Metal -framework Foundation -o miplod_view_diff

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <vector>

static const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;
#pragma METAL fp math_mode(safe)
#pragma METAL fp contract(off)
struct V { float4 pos [[position]]; float4 t0 [[user(texcoord0)]]; };
struct In { float4 pos; float4 uv; };
vertex V vs(uint vid [[vertex_id]], const device In* v [[buffer(0)]],
            constant float4& ndc [[buffer(1)]]) {
  V o;
  float4 p = v[vid].pos;
  p.xy = p.xy * ndc.xy + ndc.zw;
  o.pos = p;
  o.t0 = v[vid].uv;
  return o;
}
struct P { float bias; float2 offs; float pad; };
fragment float4 ps(V i [[stage_in]], texture2d_array<float> t [[texture(0)]],
                   sampler s [[sampler(0)]], constant P& p [[buffer(0)]]) {
  float4 c = i.t0;
  c.xy = c.xy + p.offs;
  float lod = fast::exp2(p.bias);
  float4 gh = float4(0.0f), gv = float4(0.0f);
  gh.xy = (dfdx(c) * lod).xy;
  gv.xy = (dfdy(c) * lod).xy;
  return t.sample(s, c.xy, 0u, gradient2d(gh.xy, gv.xy));
}
)";

int main() {
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError* err = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new];
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSource) options:opts error:&err];
    if (!lib) {
      fprintf(stderr, "compile: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"ps"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) {
      fprintf(stderr, "pso: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.mipFilter = MTLSamplerMipFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.lodMinClamp = 0.0f;
    sd.lodMaxClamp = FLT_MAX;
    sd.supportArgumentBuffers = YES;
    id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const uint32_t kBase = 256;
    uint64_t cases = 0, weight_n1 = 0, view_diff = 0, pixels = 0;

    for (uint32_t scale = 1; scale <= 3; ++scale) {
      const uint32_t size0 = kBase * scale;
      MTLTextureDescriptor* td = [MTLTextureDescriptor new];
      td.textureType = MTLTextureType2DArray;
      td.pixelFormat = MTLPixelFormatRGBA16Float;
      td.width = size0;
      td.height = size0;
      td.arrayLength = 1;
      td.mipmapLevelCount = 4;
      td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
      td.storageMode = MTLStorageModeShared;
      id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
      auto fill = [&](uint32_t level, int poison) {
        uint32_t w = std::max(size0 >> level, 1u);
        std::vector<uint16_t> data(size_t(w) * w * 4);
        for (auto& h : data) {
          float f = poison ? (poison == 1 ? 1000.0f : -500.0f) : uni(rng);
          __fp16 hf = __fp16(f);
          std::memcpy(&h, &hf, 2);
        }
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, w)
               mipmapLevel:level
                     slice:0
                 withBytes:data.data()
               bytesPerRow:w * 8
             bytesPerImage:0];
      };
      for (uint32_t target = 1; target <= 3; ++target) {
        // Level target - 1 is read; level target is the one being written.
        for (uint32_t l = 0; l < 4; ++l) fill(l, 0);
        const uint32_t tw = (kBase >> target) * scale;
        id<MTLTexture> limited = [tex newTextureViewWithPixelFormat:MTLPixelFormatRGBA16Float
                                                        textureType:MTLTextureType2DArray
                                                             levels:NSMakeRange(0, target)
                                                             slices:NSMakeRange(0, 1)];
        for (int variant = 0; variant < 16; ++variant) {
          // Quad over the target in "VS units" -1..1 with the host's NDC
          // scale/offset: sub-pixel offsets as the half-pixel offset.
          float ox = variant ? (uni(rng) - 0.5f) / float(tw) : 0.0f;
          float oy = variant ? (uni(rng) - 0.5f) / float(tw) : 0.0f;
          float ndc[4] = {1.0f, 1.0f, ox, oy};
          bool flip = variant & 1;
          struct Vtx {
            float pos[4];
            float uv[4];
          } quad[6];
          const float xs[4] = {-1, 1, -1, 1}, ys[4] = {1, 1, -1, -1};
          const float us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
          const int order[2][6] = {{0, 1, 2, 2, 1, 3}, {0, 1, 3, 0, 3, 2}};
          for (int i = 0; i < 6; ++i) {
            int k = order[flip][i];
            quad[i] = {{xs[k], ys[k], 0.5f, 1.0f}, {us[k], vs[k], 0, 0}};
          }
          // The native fetch's rounding offset (1.5/1024 texel) over the
          // guest size of level 0.
          float offs = (1.5f / 1024.0f) / float(kBase);
          // MLV_BIAS (negative control): another bias, e.g. -0.9 (LOD N + 0.1).
          static const float bias = getenv("MLV_BIAS") ? float(atof(getenv("MLV_BIAS"))) : -1.0f;
          float pcb[4] = {bias, offs, offs, 0.0f};
          std::vector<float> out[3];
          for (int run = 0; run < 3; ++run) {
            if (run < 2) fill(target, run + 1);
            MTLTextureDescriptor* rd =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                   width:tw
                                                                  height:tw
                                                               mipmapped:NO];
            rd.usage = MTLTextureUsageRenderTarget;
            rd.storageMode = MTLStorageModeShared;
            id<MTLTexture> rt = [dev newTextureWithDescriptor:rd];
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:pso];
            [e setVertexBytes:quad length:sizeof(quad) atIndex:0];
            [e setVertexBytes:ndc length:sizeof(ndc) atIndex:1];
            [e setFragmentTexture:(run == 2 ? limited : tex) atIndex:0];
            [e setFragmentSamplerState:smp atIndex:0];
            [e setFragmentBytes:pcb length:sizeof(pcb) atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [e endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            out[run].resize(size_t(tw) * tw * 4);
            [rt getBytes:out[run].data()
             bytesPerRow:tw * 16
              fromRegion:MTLRegionMake2D(0, 0, tw, tw)
             mipmapLevel:0];
          }
          ++cases;
          pixels += uint64_t(tw) * tw;
          uint64_t ab = 0, ac = 0;
          for (size_t i = 0; i < out[0].size(); ++i) {
            ab += std::memcmp(&out[0][i], &out[1][i], 4) != 0;
            ac += std::memcmp(&out[0][i], &out[2][i], 4) != 0;
          }
          weight_n1 += ab;
          view_diff += ac;
          if (ab || ac || variant == 0) {
            printf("scale %u write level %u variant %2d: A!=B %llu, A!=C %llu (of %u px)\n",
                   scale, target, variant, (unsigned long long)ab,
                   (unsigned long long)ac, tw * tw);
          }
        }
      }
    }
    printf("cases %llu, pixels %llu: level N+1 weighted in %llu values, limited view differs in %llu\n",
           (unsigned long long)cases, (unsigned long long)pixels,
           (unsigned long long)weight_n1, (unsigned long long)view_diff);
    return (weight_n1 || view_diff) ? 2 : 0;
  }
}
