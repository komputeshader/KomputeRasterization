#pragma once

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

namespace Context
{

extern id<MTLDevice> Device;
extern id<MTLCommandQueue> CommandQueue;
extern id<MTLCommandQueue> ComputeCommandQueue;
extern id<MTLSharedEvent> SharedEvent;

bool Initialize(MTKView* view);

id<MTLCommandBuffer> CreateCommandBuffer(
	id<MTLCommandQueue> queue,
	NSString* label,
	uint64_t frameNumber);
id<MTLFunction> GetFunction(const char* name);
id<MTLComputePipelineState> CreateComputePipeline(const char* name);
id<MTLBuffer> CreateBuffer(
	const void* bytes,
	NSUInteger length,
	MTLResourceOptions options,
	NSString* label);
id<MTLTexture> CreateTexture2D(
	MTLPixelFormat format,
	NSUInteger width,
	NSUInteger height,
	NSUInteger mipLevels,
	NSUInteger arrayLength,
	MTLTextureUsage usage,
	NSString* label);

}

