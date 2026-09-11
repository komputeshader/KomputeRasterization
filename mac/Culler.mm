#include "Culler.h"

#include "Context.h"
#include "Scene.h"
#include "Settings.h"
#include "Shadows.h"
#include "Utils.h"

namespace
{
	struct FrameResources
	{
		id<MTLBuffer> counters = nil;
		id<MTLBuffer> visibleInstances = nil;
		id<MTLIndirectCommandBuffer> commands = nil;
		id<MTLBuffer> commandArguments = nil;
		id<MTLBuffer> commandRanges = nil;
		id<MTLBuffer> softwareCommands = nil;
		id<MTLBuffer> softwareDispatchArguments = nil;
	};
}

struct Culler::Resources
{
	id<MTLComputePipelineState> clearPipeline = nil;
	id<MTLComputePipelineState> cullingPipeline = nil;
	id<MTLComputePipelineState> generatePipeline = nil;
	id<MTLArgumentEncoder> argumentEncoder = nil;
	FrameResources frames[2];
};

Culler::Culler() :
	_resources(std::make_unique<Resources>())
{
}

Culler::~Culler() = default;

void Culler::Initialize()
{
	_resources = std::make_unique<Resources>();
	_frameIndex = 1;
	_maxInstances = std::max<uint32_t>(static_cast<uint32_t>(Scene::MaxSceneInstancesCount), 1);
	_maxMeshes = std::max<uint32_t>(static_cast<uint32_t>(Scene::MaxSceneMeshesMetaCount), 1);

	const NSUInteger counterLength = static_cast<NSUInteger>(MAX_FRUSTUMS_COUNT) *
		_maxMeshes * sizeof(uint32_t);
	const NSUInteger visibleLength = static_cast<NSUInteger>(MAX_FRUSTUMS_COUNT) *
		_maxInstances * sizeof(Instance);
	ASSERT(
		visibleLength <= Context::Device.maxBufferLength,
		"Visible-instance buffer exceeds this Metal device's maximum buffer length")

	_resources->clearPipeline = Context::CreateComputePipeline("ClearCounters");
	_resources->cullingPipeline = Context::CreateComputePipeline("CullInstances");

	id<MTLFunction> generateFunction = Context::GetFunction("GenerateCommandsCS");
	NSError* error = nil;
	_resources->generatePipeline = [Context::Device
		newComputePipelineStateWithFunction:generateFunction
		error:&error];
	ASSERT(_resources->generatePipeline, "%s", error.localizedDescription.UTF8String)
	_resources->argumentEncoder = [generateFunction newArgumentEncoderWithBufferIndex:4];

	MTLIndirectCommandBufferDescriptor* descriptor = [MTLIndirectCommandBufferDescriptor new];
	descriptor.commandTypes = MTLIndirectCommandTypeDrawIndexed;
	descriptor.inheritBuffers = YES;
	descriptor.inheritPipelineState = YES;
	descriptor.maxVertexBufferBindCount = 0;
	descriptor.maxFragmentBufferBindCount = 0;

	for (uint32_t frameIndex = 0; frameIndex < 2; frameIndex++)
	{
		FrameResources& resources = _resources->frames[frameIndex];
		resources.counters = Context::CreateBuffer(
			nullptr, counterLength, MTLResourceStorageModePrivate, @"Visible instance counters");
		resources.visibleInstances = Context::CreateBuffer(
			nullptr, visibleLength, MTLResourceStorageModePrivate, @"Visible instances");
		resources.commandRanges = Context::CreateBuffer(
			nullptr,
			MAX_FRUSTUMS_COUNT * sizeof(MTLIndirectCommandBufferExecutionRange),
			MTLResourceStorageModePrivate,
			@"Indirect command ranges");
		resources.softwareCommands = Context::CreateBuffer(
			nullptr,
			static_cast<NSUInteger>(MAX_FRUSTUMS_COUNT) * _maxMeshes * sizeof(IndirectCommand),
			MTLResourceStorageModePrivate,
			@"Software rasterization commands");
		resources.softwareDispatchArguments = Context::CreateBuffer(
			nullptr,
			MAX_FRUSTUMS_COUNT * sizeof(DispatchArguments),
			MTLResourceStorageModePrivate,
			@"Software rasterization dispatch arguments");

		resources.commands = [Context::Device
			newIndirectCommandBufferWithDescriptor:descriptor
			maxCommandCount:static_cast<NSUInteger>(MAX_FRUSTUMS_COUNT) * _maxMeshes
			options:MTLResourceStorageModePrivate];
		resources.commands.label = @"Culled indirect draws";
		resources.commandArguments = Context::CreateBuffer(
			nullptr, _resources->argumentEncoder.encodedLength, MTLResourceStorageModeShared, @"Indirect command argument buffer");
		[_resources->argumentEncoder setArgumentBuffer:resources.commandArguments offset:0];
		[_resources->argumentEncoder setIndirectCommandBuffer:resources.commands atIndex:0];
	}
}

void Culler::Cull(
	id<MTLCommandBuffer> commandBuffer,
	const Scene& scene,
	const Shadows& shadows,
	id<MTLTexture> previousCameraHiZ,
	bool hasCameraHistory)
{
	_frameIndex = (_frameIndex + 1) % 2;
	if (Settings::FreezeCulling && Settings::CullingEnabled)
	{
		return;
	}

	FrameResources& resources = _resources->frames[_frameIndex];

	id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
	encoder.label = @"GPU culling";

	const uint32_t counterCount = Settings::FrustumsCount * _maxMeshes;
	[encoder setComputePipelineState:_resources->clearPipeline];
	[encoder setBuffer:resources.counters offset:0 atIndex:0];
	[encoder setBytes:&counterCount length:sizeof(counterCount) atIndex:1];
	[encoder setBuffer:resources.commandRanges offset:0 atIndex:2];
	const uint32_t commandRangeCount = Settings::FrustumsCount;
	[encoder setBytes:&commandRangeCount length:sizeof(commandRangeCount) atIndex:3];
	[encoder setBuffer:resources.softwareDispatchArguments offset:0 atIndex:4];
	[encoder setBytes:&_maxMeshes length:sizeof(_maxMeshes) atIndex:5];
	[encoder dispatchThreads:MTLSizeMake(counterCount, 1, 1)
		threadsPerThreadgroup:MTLSizeMake(CULLING_THREADS_X, 1, 1)];
	[encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];

	CullingCB constants;
	constants.totalInstancesCount = scene.GetInstanceCount();
	constants.totalMeshesCount = scene.GetMeshCount();
	constants.maxSceneInstancesCount = _maxInstances;
	constants.maxSceneMeshesMetaCount = _maxMeshes;
	constants.cascadesCount = Settings::CascadesCount;
	constants.frustumsCount = Settings::FrustumsCount;
	const bool culling = Settings::CullingEnabled;
	constants.frustumCullingEnabled = culling && Settings::FrustumCullingEnabled;
	constants.cameraHiZCullingEnabled = culling && Settings::CameraHiZCullingEnabled;
	constants.shadowsHiZCullingEnabled = culling && Settings::ShadowsHiZCullingEnabled;
	constants.clusterBackfaceCullingEnabled = culling && Settings::ClusterBackfaceCullingEnabled;
	constants.hasCameraHistory = hasCameraHistory;
	constants.hasShadowHistory = shadows.HasHistory();
	constants.depthResolution =
	{
		static_cast<float>(Settings::RenderWidth), static_cast<float>(Settings::RenderHeight)
	};

	constants.shadowMapResolution =
	{
		static_cast<float>(Settings::ShadowMapRes), static_cast<float>(Settings::ShadowMapRes)
	};

	constants.cameraPosition = simd_make_float4(scene.camera.GetPosition(), 1.0f);
	constants.lightDirection = simd_make_float4(
		simd_normalize(ToSIMD(scene.lightDirection)), 0.0f);
	constants.prevFrameCameraVP = scene.camera.GetPrevFrameVP();
	constants.camera = scene.camera.GetFrustum();
	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		constants.prevFrameCascadeVP[cascade] = shadows.GetPrevFrameCascadeVP(cascade);
		constants.cascade[cascade] = shadows.GetCascadeFrustum(cascade);
	}

	[encoder setComputePipelineState:_resources->cullingPipeline];
	[encoder setBuffer:scene.GetMeshesBuffer() offset:0 atIndex:0];
	[encoder setBuffer:scene.GetInstancesBuffer() offset:0 atIndex:1];
	[encoder setBuffer:resources.visibleInstances offset:0 atIndex:2];
	[encoder setBuffer:resources.counters offset:0 atIndex:3];
	[encoder setBytes:&constants length:sizeof(constants) atIndex:4];
	[encoder setTexture:previousCameraHiZ atIndex:0];
	[encoder setTexture:shadows.GetPrevFrameShadowMapMips() atIndex:1];
	[encoder dispatchThreads:MTLSizeMake(scene.GetInstanceCount(), 1, 1)
		threadsPerThreadgroup:MTLSizeMake(CULLING_THREADS_X, 1, 1)];
	[encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];

	[encoder setComputePipelineState:_resources->generatePipeline];
	[encoder setBuffer:scene.GetMeshesBuffer() offset:0 atIndex:0];
	[encoder setBuffer:resources.counters offset:0 atIndex:1];
	[encoder setBuffer:scene.GetIndicesBuffer() offset:0 atIndex:2];
	struct Parameters
	{
		uint32_t frustumsCount;
		uint32_t meshCount;
		uint32_t maxMeshes;
		uint32_t maxInstances;
	} parameters = { static_cast<uint32_t>(Settings::FrustumsCount), scene.GetMeshCount(), _maxMeshes, _maxInstances };

	[encoder setBytes:&parameters length:sizeof(parameters) atIndex:3];
	[encoder setBuffer:resources.commandArguments offset:0 atIndex:4];
	[encoder setBuffer:resources.commandRanges offset:0 atIndex:5];
	[encoder setBuffer:resources.softwareCommands offset:0 atIndex:6];
	[encoder setBuffer:resources.softwareDispatchArguments offset:0 atIndex:7];
	[encoder useResource:scene.GetIndicesBuffer() usage:MTLResourceUsageRead];
	[encoder useResource:resources.commands usage:MTLResourceUsageWrite];
	[encoder dispatchThreads:MTLSizeMake(scene.GetMeshCount(), 1, 1)
		threadsPerThreadgroup:MTLSizeMake(CULLING_THREADS_X, 1, 1)];

	[encoder endEncoding];
}

id<MTLBuffer> Culler::GetVisibleInstances() const
{
	return _resources->frames[_frameIndex].visibleInstances;
}

id<MTLBuffer> Culler::GetInstanceCounters() const
{
	return _resources->frames[_frameIndex].counters;
}

id<MTLIndirectCommandBuffer> Culler::GetCommands() const
{
	return _resources->frames[_frameIndex].commands;
}

id<MTLBuffer> Culler::GetCommandRanges() const
{
	return _resources->frames[_frameIndex].commandRanges;
}

id<MTLBuffer> Culler::GetSoftwareCommands() const
{
	return _resources->frames[_frameIndex].softwareCommands;
}

id<MTLBuffer> Culler::GetSoftwareDispatchArguments() const
{
	return _resources->frames[_frameIndex].softwareDispatchArguments;
}
