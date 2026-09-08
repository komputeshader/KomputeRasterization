#include "SoftwareRasterization.h"

#include "Common.h"
#include "Context.h"
#include "Culler.h"
#include "DescriptorManager.h"
#include "Profiler.h"
#include "Scene.h"
#include "Settings.h"
#include "Shadows.h"

#include "imgui.h"

struct SoftwareRasterization::Resources
{
	id<MTLTexture> renderTarget = nil;
	id<MTLBuffer> depth = nil;
	id<MTLBuffer> bigTrianglesDepth[MAX_FRUSTUMS_COUNT] = {};
	id<MTLBuffer> bigTrianglesDepthCounters[MAX_FRUSTUMS_COUNT] = {};
	id<MTLBuffer> bigTrianglesOpaque = nil;
	id<MTLBuffer> bigTrianglesOpaqueCounter = nil;
	id<MTLBuffer> statistics[2] = {};
	id<MTLComputePipelineState> clearDepth = nil;
	id<MTLComputePipelineState> clearShadow = nil;
	id<MTLComputePipelineState> clearColor = nil;
	id<MTLComputePipelineState> clearStatistics = nil;
	id<MTLComputePipelineState> resetDispatch = nil;
	id<MTLComputePipelineState> triangleDepth = nil;
	id<MTLComputePipelineState> bigTriangleDepth = nil;
	id<MTLComputePipelineState> triangleShadow = nil;
	id<MTLComputePipelineState> bigTriangleShadow = nil;
	id<MTLComputePipelineState> triangleOpaque = nil;
	id<MTLComputePipelineState> bigTriangleOpaque = nil;
};

namespace
{
	void Dispatch2D(
		id<MTLComputeCommandEncoder> encoder,
		id<MTLComputePipelineState> pipeline,
		uint32_t width,
		uint32_t height,
		uint32_t depth = 1)
	{
		[encoder setComputePipelineState:pipeline];
		const MTLSize threads = MTLSizeMake(8, 8, 1);
		const MTLSize groups = MTLSizeMake(
			(width + threads.width - 1) / threads.width,
			(height + threads.height - 1) / threads.height,
			depth);
		[encoder dispatchThreadgroups:groups threadsPerThreadgroup:threads];
	}
}

SoftwareRasterization::SoftwareRasterization() :
	_resources(std::make_unique<Resources>())
{
}

SoftwareRasterization::~SoftwareRasterization() = default;

void SoftwareRasterization::Initialize(uint32_t width, uint32_t height)
{
	_resources->clearDepth = Context::CreateComputePipeline("ClearUIntBuffer");
	_resources->clearShadow = Context::CreateComputePipeline("ClearUIntBuffer");
	_resources->clearColor = Context::CreateComputePipeline("ClearColorTexture");
	_resources->clearStatistics = Context::CreateComputePipeline("ClearStatistics");
	_resources->resetDispatch = Context::CreateComputePipeline("ResetDispatchArguments");
	_resources->triangleDepth = Context::CreateComputePipeline("TriangleDepthCS");
	_resources->bigTriangleDepth = Context::CreateComputePipeline("BigTriangleDepthCS");
	_resources->triangleShadow = Context::CreateComputePipeline("TriangleShadowCS");
	_resources->bigTriangleShadow = Context::CreateComputePipeline("BigTriangleShadowCS");
	_resources->triangleOpaque = Context::CreateComputePipeline("TriangleOpaqueCS");
	_resources->bigTriangleOpaque = Context::CreateComputePipeline("BigTriangleOpaqueCS");

	for (uint32_t frameIndex = 0; frameIndex < 2; frameIndex++)
	{
		_resources->statistics[frameIndex] = Context::CreateBuffer(
			nullptr, sizeof(uint32_t) * 2, MTLResourceStorageModeShared, @"Software rasterizer statistics");
	}

	Resize(width, height);
}

void SoftwareRasterization::_createBigTrianglesBuffers()
{
	_maxBigTrianglesDepth[0] = _width * _height;
	for (uint32_t frustum = 1; frustum < MAX_FRUSTUMS_COUNT; frustum++)
	{
		_maxBigTrianglesDepth[frustum] = Settings::ShadowMapRes * Settings::ShadowMapRes / 5;
	}

	_maxBigTrianglesOpaque = _width * _height;

	for (uint32_t frustum = 0; frustum < MAX_FRUSTUMS_COUNT; frustum++)
	{
		_resources->bigTrianglesDepth[frustum] = Context::CreateBuffer(
			nullptr,
			static_cast<NSUInteger>(_maxBigTrianglesDepth[frustum]) * sizeof(BigTriangleDepth),
			MTLResourceStorageModePrivate,
			@"Big depth triangles");
		_resources->bigTrianglesDepthCounters[frustum] = Context::CreateBuffer(
			nullptr,
			sizeof(DispatchArguments),
			MTLResourceStorageModePrivate,
			@"Big depth triangle counter");
	}

	_resources->bigTrianglesOpaque = Context::CreateBuffer(
		nullptr,
		static_cast<NSUInteger>(_maxBigTrianglesOpaque) * sizeof(BigTriangleOpaque),
		MTLResourceStorageModePrivate,
		@"Big opaque triangles");
	_resources->bigTrianglesOpaqueCounter = Context::CreateBuffer(
		nullptr,
		sizeof(DispatchArguments),
		MTLResourceStorageModePrivate,
		@"Big opaque triangle counter");
}

void SoftwareRasterization::Resize(uint32_t width, uint32_t height)
{
	_width = std::max(width, 1u);
	_height = std::max(height, 1u);
	_resources->renderTarget = Context::CreateTexture2D(
		MTLPixelFormatRGBA8Unorm, _width, _height, 1, 1, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"Software color");
	_resources->depth = Context::CreateBuffer(
		nullptr,
		static_cast<NSUInteger>(_width) * _height * sizeof(uint32_t),
		MTLResourceStorageModePrivate,
		@"Software camera depth");
	_createBigTrianglesBuffers();
}

void SoftwareRasterization::GUINewFrame()
{
	int location = Settings::SWRGUILocation;
	ImGuiWindowFlags window_flags =
		ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav;
	if (location >= 0)
	{
		const float PAD = ImGui::GetFontSize() * 0.625f;
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImVec2 work_pos = viewport->WorkPos;
		ImVec2 work_size = viewport->WorkSize;
		ImVec2 window_pos, window_pos_pivot;
		window_pos.x = (location & 1)
			? (work_pos.x + work_size.x - PAD)
			: (work_pos.x + PAD);
		window_pos.y = (location & 2)
			? (work_pos.y + work_size.y - PAD)
			: (work_pos.y + PAD);
		window_pos_pivot.x = (location & 1) ? 1.0f : 0.0f;
		window_pos_pivot.y = (location & 2) ? 1.0f : 0.0f;
		ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
		window_flags |= ImGuiWindowFlags_NoMove;
	}

	ImGui::SetNextWindowBgAlpha(Settings::GUITransparency);
	if (ImGui::Begin("Software Rasterization", nullptr, window_flags))
	{
		ImGui::SliderInt(
			"Big Triangle Threshold",
			&_bigTriangleThreshold,
			1,
			8192,
			"%i",
			ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderInt(
			"Big Triangle Tile Size",
			&_bigTriangleTileSize,
			64,
			2048,
			"%i",
			ImGuiSliderFlags_AlwaysClamp);
		ImGui::Checkbox("Scanline rasterization", &_scanlineRasterization);
	}

	ImGui::End();
}

void SoftwareRasterization::_drawDepth(
	id<MTLComputeCommandEncoder> encoder,
	const Scene& scene,
	const Culler& culler,
	const Shadows& shadows,
	id<MTLTexture> previousCameraHiZ,
	bool hasCameraHistory,
	uint32_t frustum)
{
	DepthSceneCB constants;
	constants.vp = frustum == 0 ? scene.camera.GetVP() : shadows.GetCascadeVP(frustum - 1);
	const uint32_t width = frustum == 0 ? _width : Settings::ShadowMapRes;
	const uint32_t height = frustum == 0 ? _height : Settings::ShadowMapRes;
	constants.outputResolution = { static_cast<float>(width), static_cast<float>(height) };
	constants.inverseOutputResolution = { 1.0f / width, 1.0f / height };
	constants.bigTriangleThreshold = static_cast<float>(_bigTriangleThreshold);
	constants.bigTriangleTileSize = static_cast<float>(_bigTriangleTileSize);
	constants.scanlineRasterization = _scanlineRasterization;
	constants.totalTriangles = scene.GetTrianglesCount();
	constants.perTriangleHiZCullingEnabled =
		Settings::PerTriangleHiZRasterizationCullingEnabled;
	constants.frustumIndex = frustum;
	constants.maxBigTriangles = _maxBigTrianglesDepth[frustum];
	constants.maxSceneMeshes = culler.GetMaxMeshes();
	constants.maxSceneInstances = culler.GetMaxInstances();
	constants.hasHiZHistory = frustum == 0 ? hasCameraHistory : shadows.HasHistory();

	[encoder setComputePipelineState:frustum == 0
		? _resources->triangleDepth
		: _resources->triangleShadow];
	[encoder setBuffer:scene.GetPositionsBuffer() offset:0 atIndex:Bindings::Positions];
	[encoder setBuffer:scene.GetIndicesSOABuffer() offset:0 atIndex:Bindings::Indices];
	[encoder setBuffer:culler.GetVisibleInstances() offset:0 atIndex:Bindings::Instances];
	[encoder setBytes:&constants length:sizeof(constants) atIndex:Bindings::Constants];
	[encoder setBuffer:culler.GetSoftwareCommands()
		offset:frustum * culler.GetMaxMeshes() * sizeof(IndirectCommand)
		atIndex:9];
	[encoder setBuffer:_resources->statistics[_frameIndex] offset:0 atIndex:Bindings::Statistics];
	[encoder setBuffer:_resources->bigTrianglesDepth[frustum] offset:0 atIndex:Bindings::BigTriangles];
	[encoder setBuffer:_resources->bigTrianglesDepthCounters[frustum] offset:0 atIndex:12];
	[encoder setBuffer:frustum == 0 ? _resources->depth : shadows.GetShadowMapSWR()
		offset:0 atIndex:6];
	[encoder setTexture:previousCameraHiZ atIndex:2];
	[encoder setTexture:shadows.GetPrevFrameShadowMapMips() atIndex:3];
	[encoder dispatchThreadgroupsWithIndirectBuffer:culler.GetSoftwareDispatchArguments()
		indirectBufferOffset:frustum * sizeof(DispatchArguments)
		threadsPerThreadgroup:MTLSizeMake(SWR_TRIANGLE_THREADS_X, 1, 1)];
}

void SoftwareRasterization::_drawDepthBigTriangles(
	id<MTLComputeCommandEncoder> encoder,
	const Scene& scene,
	const Shadows& shadows,
	uint32_t frustum)
{
	DepthSceneCB constants;
	constants.vp = frustum == 0 ? scene.camera.GetVP() : shadows.GetCascadeVP(frustum - 1);
	const uint32_t width = frustum == 0 ? _width : Settings::ShadowMapRes;
	const uint32_t height = frustum == 0 ? _height : Settings::ShadowMapRes;
	constants.outputResolution = { static_cast<float>(width), static_cast<float>(height) };
	constants.inverseOutputResolution = { 1.0f / width, 1.0f / height };
	constants.bigTriangleTileSize = static_cast<float>(_bigTriangleTileSize);
	constants.frustumIndex = frustum;
	constants.maxBigTriangles = _maxBigTrianglesDepth[frustum];

	[encoder setComputePipelineState:frustum == 0
		? _resources->bigTriangleDepth
		: _resources->bigTriangleShadow];
	[encoder setBytes:&constants length:sizeof(constants) atIndex:Bindings::Constants];
	[encoder setBuffer:_resources->bigTrianglesDepth[frustum] offset:0 atIndex:Bindings::BigTriangles];
	[encoder setBuffer:frustum == 0 ? _resources->depth : shadows.GetShadowMapSWR()
		offset:0 atIndex:6];
	[encoder dispatchThreadgroupsWithIndirectBuffer:_resources->bigTrianglesDepthCounters[frustum]
		indirectBufferOffset:0
		threadsPerThreadgroup:MTLSizeMake(
			SWR_BIG_TRIANGLE_THREADS_X,
			SWR_BIG_TRIANGLE_THREADS_Y,
			1)];
}

void SoftwareRasterization::DrawDepths(
	id<MTLCommandBuffer> commandBuffer,
	const Scene& scene,
	const Culler& culler,
	const Shadows& shadows,
	id<MTLTexture> previousCameraHiZ,
	bool hasCameraHistory,
	FrameStatistics& statistics)
{
	_frameIndex = (_frameIndex + 1) % 2;
	id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
	encoder.label = @"Software depths";
	statistics.BeginMeasure(encoder);
	[encoder setComputePipelineState:_resources->clearStatistics];
	[encoder setBuffer:_resources->statistics[_frameIndex] offset:0 atIndex:0];
	[encoder dispatchThreads:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
	[encoder setComputePipelineState:_resources->resetDispatch];
	for (uint32_t frustum = 0; frustum < static_cast<uint32_t>(Settings::FrustumsCount); frustum++)
	{
		[encoder setBuffer:_resources->bigTrianglesDepthCounters[frustum] offset:0 atIndex:0];
		[encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
	}

	[encoder setComputePipelineState:_resources->clearDepth];
	[encoder setBuffer:_resources->depth offset:0 atIndex:0];
	uint32_t clearCount = _width * _height;
	[encoder setBytes:&clearCount length:sizeof(clearCount) atIndex:1];
	[encoder dispatchThreads:MTLSizeMake(clearCount, 1, 1)
		threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
	[encoder setComputePipelineState:_resources->clearShadow];
	[encoder setBuffer:shadows.GetShadowMapSWR() offset:0 atIndex:0];
	clearCount = Settings::ShadowMapRes * Settings::ShadowMapRes * Settings::CascadesCount;
	[encoder setBytes:&clearCount length:sizeof(clearCount) atIndex:1];
	[encoder dispatchThreads:MTLSizeMake(clearCount, 1, 1)
		threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
	[encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];

	_drawDepth(encoder, scene, culler, shadows, previousCameraHiZ, hasCameraHistory, 0);
	for (uint32_t cascade = 0; cascade < static_cast<uint32_t>(Settings::CascadesCount); cascade++)
	{
		_drawDepth(encoder, scene, culler, shadows, previousCameraHiZ, hasCameraHistory, cascade + 1);
	}

	[encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
	_drawDepthBigTriangles(encoder, scene, shadows, 0);
	for (uint32_t cascade = 0; cascade < static_cast<uint32_t>(Settings::CascadesCount); cascade++)
	{
		_drawDepthBigTriangles(encoder, scene, shadows, cascade + 1);
	}

	statistics.FinishMeasure(encoder);
	[encoder endEncoding];
}

void SoftwareRasterization::DrawOpaque(
	id<MTLCommandBuffer> commandBuffer,
	const Scene& scene,
	const Culler& culler,
	const Shadows& shadows,
	id<MTLTexture> previousCameraHiZ,
	bool hasCameraHistory,
	FrameStatistics& statistics)
{
	id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
	encoder.label = @"Software opaque";
	statistics.BeginMeasure(encoder);
	[encoder setTexture:_resources->renderTarget atIndex:0];
	Dispatch2D(encoder, _resources->clearColor, _width, _height);
	[encoder setComputePipelineState:_resources->resetDispatch];
	[encoder setBuffer:_resources->bigTrianglesOpaqueCounter offset:0 atIndex:0];
	[encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
	[encoder memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];

	SceneCB constants;
	constants.vp = scene.camera.GetVP();
	constants.sunDirection = simd_make_float4(simd_normalize(ToSIMD(scene.lightDirection)), 0.0f);
	constants.outputResolution = { static_cast<float>(_width), static_cast<float>(_height) };
	constants.inverseOutputResolution = { 1.0f / _width, 1.0f / _height };
	constants.shadowMapResolution =
	{
		static_cast<float>(Settings::ShadowMapRes),
		static_cast<float>(Settings::ShadowMapRes)
	};

	constants.bigTriangleThreshold = static_cast<float>(_bigTriangleThreshold);
	constants.bigTriangleTileSize = static_cast<float>(_bigTriangleTileSize);
	constants.showCascades = shadows.ShowCascades();
	constants.showMeshlets = Settings::ShowMeshlets;
	constants.cascadesCount = Settings::CascadesCount;
	constants.scanlineRasterization = _scanlineRasterization;
	constants.shadowsDistance = shadows.GetShadowDistance();
	constants.totalTriangles = scene.GetTrianglesCount();
	constants.perTriangleHiZCullingEnabled =
		Settings::PerTriangleHiZRasterizationCullingEnabled;
	constants.maxBigTriangles = _maxBigTrianglesOpaque;
	constants.hasHiZHistory = hasCameraHistory;
	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		constants.cascadeVP[cascade] = shadows.GetCascadeVP(cascade);
		reinterpret_cast<float*>(&constants.cascadeBias)[cascade] = shadows.GetCascadeBias(cascade);
		reinterpret_cast<float*>(&constants.cascadeSplits)[cascade] = shadows.GetCascadeSplit(cascade);
	}

	[encoder setComputePipelineState:_resources->triangleOpaque];
	[encoder setBuffer:scene.GetPositionsBuffer() offset:0 atIndex:Bindings::Positions];
	[encoder setBuffer:scene.GetNormalsBuffer() offset:0 atIndex:Bindings::Normals];
	[encoder setBuffer:scene.GetColorsBuffer() offset:0 atIndex:Bindings::Colors];
	[encoder setBuffer:scene.GetTexcoordsBuffer() offset:0 atIndex:Bindings::Texcoords];
	[encoder setBuffer:scene.GetIndicesSOABuffer() offset:0 atIndex:Bindings::Indices];
	[encoder setBuffer:culler.GetVisibleInstances() offset:0 atIndex:Bindings::Instances];
	[encoder setBytes:&constants length:sizeof(constants) atIndex:Bindings::Constants];
	[encoder setBuffer:culler.GetSoftwareCommands() offset:0 atIndex:9];
	[encoder setBuffer:_resources->statistics[_frameIndex] offset:0 atIndex:Bindings::Statistics];
	[encoder setBuffer:_resources->bigTrianglesOpaque offset:0 atIndex:Bindings::BigTriangles];
	[encoder setBuffer:_resources->bigTrianglesOpaqueCounter offset:0 atIndex:12];
	[encoder setBuffer:_resources->depth offset:0 atIndex:6];
	[encoder setBuffer:shadows.GetShadowMapSWR() offset:0 atIndex:8];
	[encoder setTexture:_resources->renderTarget atIndex:2];
	[encoder setTexture:previousCameraHiZ atIndex:3];
	[encoder dispatchThreadgroupsWithIndirectBuffer:culler.GetSoftwareDispatchArguments()
		indirectBufferOffset:0
		threadsPerThreadgroup:MTLSizeMake(SWR_TRIANGLE_THREADS_X, 1, 1)];
	[encoder memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
	[encoder setComputePipelineState:_resources->bigTriangleOpaque];
	[encoder setBuffer:_resources->bigTrianglesOpaque offset:0 atIndex:Bindings::BigTriangles];
	[encoder dispatchThreadgroupsWithIndirectBuffer:_resources->bigTrianglesOpaqueCounter
		indirectBufferOffset:0
		threadsPerThreadgroup:MTLSizeMake(
			SWR_BIG_TRIANGLE_THREADS_X,
			SWR_BIG_TRIANGLE_THREADS_Y,
			1)];
	statistics.FinishMeasure(encoder);
	[encoder endEncoding];

	id<MTLBuffer> resultBuffer = _resources->statistics[_frameIndex];
	FrameStatistics* result = &statistics;
	[commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>)
	{
		const uint32_t* values = static_cast<const uint32_t*>(resultBuffer.contents);
		result->Set(values[0], values[1]);
	}];
}

id<MTLTexture> SoftwareRasterization::GetRenderTarget() const
{
	return _resources->renderTarget;
}

id<MTLBuffer> SoftwareRasterization::GetDepthBuffer() const
{
	return _resources->depth;
}
