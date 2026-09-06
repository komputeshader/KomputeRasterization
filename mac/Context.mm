#include "Context.h"

#include "Utils.h"

#import <Foundation/Foundation.h>

#include <cstring>

id<MTLDevice> Context::Device = nil;
id<MTLCommandQueue> Context::CommandQueue = nil;
id<MTLCommandQueue> Context::ComputeCommandQueue = nil;
id<MTLSharedEvent> Context::SharedEvent = nil;

namespace
{

	id<MTLLibrary> Library = nil;

}

bool Context::Initialize(MTKView* view)
{
	if (Device)
	{
		return true;
	}

	Device = view.device ?: MTLCreateSystemDefaultDevice();
	if (!Device)
	{
		Utils::Log("Metal is unavailable on this Mac.\n");
		return false;
	}

	if (view)
	{
		view.device = Device;
	}

	CommandQueue = [Device newCommandQueue];
	CommandQueue.label = @"Graphics queue";
	ComputeCommandQueue = [Device newCommandQueue];
	ComputeCommandQueue.label = @"Async compute queue";
	SharedEvent = [Device newSharedEvent];
	SharedEvent.label = @"Graphics/compute synchronization";

	Library = [Device newDefaultLibrary];
	if (!Library)
	{
		Utils::Log("Cannot load the default Metal library.\n");
		return false;
	}

	Utils::Log("Metal device: %s\n", Device.name.UTF8String);
	return true;
}

id<MTLFunction> Context::GetFunction(const char* name)
{
	NSString* functionName = [NSString stringWithUTF8String:name];
	id<MTLFunction> function = [Library newFunctionWithName:functionName];
	ASSERT(function, "Missing Metal function: %s", name)

	return function;
}

id<MTLComputePipelineState> Context::CreateComputePipeline(const char* name)
{
	id<MTLFunction> function = GetFunction(name);

	NSError* error = nil;
	id<MTLComputePipelineState> pipeline = [Device
		newComputePipelineStateWithFunction:function
		error:&error];
	ASSERT(
		pipeline,
		"Cannot create compute pipeline %s: %s",
		name,
		error.localizedDescription.UTF8String ?: "unknown error")

	return pipeline;
}

id<MTLBuffer> Context::CreateBuffer(
	const void* bytes,
	NSUInteger length,
	MTLResourceOptions options,
	NSString* label)
{
	const NSUInteger safeLength = std::max<NSUInteger>(length, 16);
	id<MTLBuffer> result = [Device newBufferWithLength:safeLength options:options];
	ASSERT(result, "Metal buffer allocation failed")

	if (bytes && length)
	{
		std::memcpy(result.contents, bytes, length);
	}

	result.label = label;
	return result;
}

id<MTLTexture> Context::CreateTexture2D(
	MTLPixelFormat format,
	NSUInteger width,
	NSUInteger height,
	NSUInteger mipLevels,
	NSUInteger arrayLength,
	MTLTextureUsage usage,
	NSString* label)
{
	MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
	descriptor.textureType = arrayLength > 1 ? MTLTextureType2DArray : MTLTextureType2D;
	descriptor.pixelFormat = format;
	descriptor.width = std::max<NSUInteger>(width, 1);
	descriptor.height = std::max<NSUInteger>(height, 1);
	descriptor.mipmapLevelCount = std::max<NSUInteger>(mipLevels, 1);
	descriptor.arrayLength = std::max<NSUInteger>(arrayLength, 1);
	descriptor.storageMode = MTLStorageModePrivate;
	descriptor.usage = usage;

	id<MTLTexture> result = [Device newTextureWithDescriptor:descriptor];
	ASSERT(result, "Metal texture allocation failed")

	result.label = label;
	return result;
}
