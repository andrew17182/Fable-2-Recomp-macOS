// Synthetic GPU sampling check for the packed 1555 upload layout.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    assert(device);
    NSError* error = nil;
    NSString* shader = @"#include <metal_stdlib>\n"
        "using namespace metal; "
        "kernel void readtex(texture2d<float, access::read> t [[texture(0)]], "
        "device float4* out [[buffer(0)]], uint i [[thread_position_in_grid]]) "
        "{ out[i] = t.read(uint2(i, 0)); }";
    id<MTLLibrary> library = [device newLibraryWithSource:shader options:nil error:&error];
    id<MTLComputePipelineState> pipeline = [device
        newComputePipelineStateWithFunction:[library newFunctionWithName:@"readtex"]
        error:&error];
    if (!pipeline) { NSLog(@"%@", error); return 1; }

    // The upload transform puts alpha in bit 15 and B/G/R in bits 0/5/10.
    const uint16_t words[] = {0xfc00, 0x83e0, 0x801f, 0xffff, 0x0000};
    const float expected[5][4] = {
        {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1},
        {1, 1, 1, 1}, {0, 0, 0, 0}};
    auto* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:TEST_PIXEL_FORMAT
        width:5 height:1 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    assert(texture);
    [texture replaceRegion:MTLRegionMake2D(0, 0, 5, 1) mipmapLevel:0
                 withBytes:words bytesPerRow:sizeof(words)];
    id<MTLBuffer> output = [device newBufferWithLength:sizeof(expected)
        options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(5, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(5, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    assert(command.status == MTLCommandBufferStatusCompleted);
    const float* sampled = static_cast<const float*>(output.contents);
    for (int pixel = 0; pixel < 5; ++pixel) {
      for (int channel = 0; channel < 4; ++channel) {
        assert(std::abs(sampled[pixel * 4 + channel] -
                        expected[pixel][channel]) < 0.001f);
      }
    }
  }
}
