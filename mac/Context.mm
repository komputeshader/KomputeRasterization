#include "Context.h"

#include "Settings.h"
#include "Utils.h"

#import <Foundation/Foundation.h>

#include <cstring>
#include <sys/sysctl.h>
#include <unistd.h>

id<MTLDevice> Context::Device = nil;
id<MTLCommandQueue> Context::CommandQueue = nil;
id<MTLCommandQueue> Context::ComputeCommandQueue = nil;
id<MTLSharedEvent> Context::SharedEvent = nil;

namespace
{

	id<MTLLibrary> Library = nil;
	bool EncoderExecutionStatusEnabled = false;

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

	// capture GPU progress in Release too when launched with a debugger
	int query[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
	kinfo_proc process = {};
	size_t processSize = sizeof(process);
	EncoderExecutionStatusEnabled =
		sysctl(query, 4, &process, &processSize, nullptr, 0) == 0 &&
		(process.kp_proc.p_flag & P_TRACED) != 0;

	Library = [Device newDefaultLibrary];
	if (!Library)
	{
		Utils::Log("Cannot load the default Metal library.\n");
		return false;
	}

	Utils::Log("Metal device: %s\n", Device.name.UTF8String);
	return true;
}

id<MTLCommandBuffer> Context::CreateCommandBuffer(
	id<MTLCommandQueue> queue,
	NSString* label,
	uint64_t frameNumber)
{
	MTLCommandBufferDescriptor* descriptor = [MTLCommandBufferDescriptor new];
	descriptor.errorOptions = EncoderExecutionStatusEnabled
		? MTLCommandBufferErrorOptionEncoderExecutionStatus
		: MTLCommandBufferErrorOptionNone;
	id<MTLCommandBuffer> commandBuffer = [queue commandBufferWithDescriptor:descriptor];
	commandBuffer.label = label;

	// snapshot settings for this submission
	// completion runs on a driver thread
	const bool softwareRasterization = Settings::SWREnabled;
	const bool culling = Settings::CullingEnabled;
	const bool perTriangleHiZ = Settings::PerTriangleHiZRasterizationCullingEnabled;
	const uint32_t width = Settings::RenderWidth;
	const uint32_t height = Settings::RenderHeight;
	const int cascades = Settings::CascadesCount;
	[commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed)
	{
		if (completed.status != MTLCommandBufferStatusError)
		{
			return;
		}

		Utils::Log("Metal command buffer failed: %s, error=%ld, frame=%llu, SWR=%d\n",
			completed.label.UTF8String, static_cast<long>(completed.error.code),
			static_cast<unsigned long long>(frameNumber), softwareRasterization);
		Utils::Log("Metal settings: resolution=%ux%u, culling=%d, per-triangle Hi-Z=%d, cascades=%d\n",
			width, height, culling, perTriangleHiZ, cascades);
		Utils::Log("%s\n", completed.error.description.UTF8String);
		NSArray<id<MTLCommandBufferEncoderInfo>>* encoders =
			completed.error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
		for (id<MTLCommandBufferEncoderInfo> encoder in encoders)
		{
			if (encoder.errorState == MTLCommandEncoderErrorStateCompleted)
			{
				continue;
			}

			Utils::Log("Metal encoder: %s, state=%ld\n",
				encoder.label.UTF8String, static_cast<long>(encoder.errorState));
			for (NSString* signpost in encoder.debugSignposts)
			{
				Utils::Log("Metal signpost: %s\n", signpost.UTF8String);
			}
		}
	}];

	return commandBuffer;
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
