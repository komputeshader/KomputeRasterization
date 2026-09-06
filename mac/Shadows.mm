#include "Shadows.h"

#include "Context.h"
#include "Scene.h"
#include "Settings.h"
#include "Utils.h"

#include "imgui.h"

struct Shadows::Resources
{
	id<MTLTexture> hardwareMap = nil;
	id<MTLBuffer> softwareMap = nil;
	id<MTLTexture> previousHiZ = nil;
	id<MTLComputePipelineState> copyHardware = nil;
	id<MTLComputePipelineState> copySoftware = nil;
	id<MTLComputePipelineState> downsample = nil;
};

namespace
{
	NSUInteger MipCount(NSUInteger size)
	{
		NSUInteger count = 1;
		while (size > 1)
		{
			size >>= 1;
			++count;
		}

		return count;
	}

	MTLSize Dispatch2D(
		id<MTLComputePipelineState> pipeline,
		NSUInteger width,
		NSUInteger height,
		NSUInteger depth)
	{
		const NSUInteger x = std::min<NSUInteger>(8, pipeline.maxTotalThreadsPerThreadgroup);
		const NSUInteger y = std::max<NSUInteger>(1, std::min<NSUInteger>(8, pipeline.maxTotalThreadsPerThreadgroup / x));

		return MTLSizeMake((width + x - 1) / x, (height + y - 1) / y, depth);
	}
}

void Shadows::_computeNearAndFar(
	float& nearPlane,
	float& farPlane,
	simd_float3 orthographicMinimum,
	simd_float3 orthographicMaximum,
	const simd_float3* scenePoints)
{
	static const int indices[] =
	{
		0, 1, 2, 1, 2, 3,
		4, 5, 6, 5, 6, 7,
		0, 2, 4, 2, 4, 6,
		1, 3, 5, 3, 5, 7,
		0, 1, 4, 1, 4, 5,
		2, 3, 6, 3, 6, 7
	};

	nearPlane = INFINITY;
	farPlane = -INFINITY;
	for (int triangle = 0; triangle < 12; ++triangle)
	{
		simd_float3 vertices[16] =
		{
			scenePoints[indices[triangle * 3]],
			scenePoints[indices[triangle * 3 + 1]],
			scenePoints[indices[triangle * 3 + 2]]
		};

		int vertexCount = 3;
		for (int plane = 0; plane < 4 && vertexCount; ++plane)
		{
			const int component = plane / 2;
			const float edge = (plane & 1)
				? orthographicMaximum[component]
				: orthographicMinimum[component];
			simd_float3 clipped[16];
			int clippedCount = 0;
			for (int vertex = 0; vertex < vertexCount; ++vertex)
			{
				const simd_float3 first = vertices[vertex];
				const simd_float3 second = vertices[(vertex + 1) % vertexCount];
				const bool firstInside = (plane & 1)
					? first[component] < edge
					: first[component] > edge;
				const bool secondInside = (plane & 1)
					? second[component] < edge
					: second[component] > edge;
				if (firstInside)
				{
					clipped[clippedCount++] = first;
				}

				if (firstInside != secondInside)
				{
					const float distance =
						(edge - first[component]) /
						(second[component] - first[component]);
					clipped[clippedCount++] = first + (second - first) * distance;
				}
			}

			vertexCount = clippedCount;
			for (int vertex = 0; vertex < vertexCount; ++vertex)
			{
				vertices[vertex] = clipped[vertex];
			}
		}

		for (int vertex = 0; vertex < vertexCount; ++vertex)
		{
			nearPlane = std::min(nearPlane, vertices[vertex].z);
			farPlane = std::max(farPlane, vertices[vertex].z);
		}
	}
}

Shadows::Shadows() :
	_resources(std::make_unique<Resources>())
{
}

Shadows::~Shadows() = default;

void Shadows::Initialize()
{
	const MTLTextureUsage depthUsage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	_resources->hardwareMap = Context::CreateTexture2D(
		MTLPixelFormatDepth32Float,
		Settings::ShadowMapRes,
		Settings::ShadowMapRes,
		1,
		MAX_CASCADES_COUNT,
		depthUsage,
		@"Hardware shadow map");
	_resources->softwareMap = Context::CreateBuffer(
		nullptr,
		static_cast<NSUInteger>(MAX_CASCADES_COUNT) *
			Settings::ShadowMapRes * Settings::ShadowMapRes * sizeof(uint32_t),
		MTLResourceStorageModePrivate,
		@"Software shadow map");
	_resources->previousHiZ = Context::CreateTexture2D(
		MTLPixelFormatR32Float,
		Settings::ShadowMapRes,
		Settings::ShadowMapRes,
		MipCount(Settings::ShadowMapRes),
		MAX_CASCADES_COUNT,
		MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
		@"Previous shadow Hi-Z");

	_resources->copyHardware = Context::CreateComputePipeline("CopyDepthArray");
	_resources->copySoftware = Context::CreateComputePipeline("CopyFloatArray");
	_resources->downsample = Context::CreateComputePipeline("GenerateHiZMipArray");
}

void Shadows::Update(const Scene& scene)
{
	const Camera& camera = scene.camera;
	const float denominator = 1.0f / Settings::CascadesCount;
	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		_cascadeSplitsNormalized[cascade] = (cascade + 1) * denominator;
	}

	for (int cascade = 0; cascade < MAX_CASCADES_COUNT; cascade++)
	{
		_prevFrameCascadeVP[cascade] = _cascadeVP[cascade];
	}

	const AABB& sceneAABB = scene.GetSceneAABB();
	const Frustum& cameraFrustum = camera.GetFrustum();
	const float frustumLookDistance = camera.GetFarZ() - camera.GetNearZ();
	const float shadowDistance = std::min(
		{ _shadowDistance, frustumLookDistance, sceneAABB.GetDiagonalLength() });
	const float shadowDistanceNormalized = shadowDistance / frustumLookDistance;

	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		_cascadeBias[cascade] = _bias;
		const float previousSplit = cascade == 0
			? 0.0f
			: _cascadeSplitsNormalized[cascade - 1];
		const float nextSplit = _cascadeSplitsNormalized[cascade];
		_cascadeSplits[cascade] = nextSplit * shadowDistance;

		simd_float3 splitCorners[8];
		simd_float3 splitCenter = {};
		for (int corner = 0; corner < 4; corner++)
		{
			const simd_float3 nearCorner = cameraFrustum.cornersWS[corner].xyz;
			const simd_float3 cornerRay =
				(cameraFrustum.cornersWS[corner + 4].xyz - nearCorner) *
				shadowDistanceNormalized;
			splitCorners[corner] = nearCorner + cornerRay * previousSplit;
			splitCorners[corner + 4] = nearCorner + cornerRay * nextSplit;
			splitCenter += splitCorners[corner] * 0.125f;
			splitCenter += splitCorners[corner + 4] * 0.125f;
		}

		simd_float3 up = camera.GetRight();
		const simd_float3 look = simd_normalize(-ToSIMD(scene.lightDirection));
		const simd_float3 right = simd_normalize(simd_cross(up, look));
		up = simd_cross(look, right);
		simd_float3 position = splitCenter;
		simd_float4x4 view = Utils::LookAtLH(position, position + look, up);

		const simd_float3 sceneCenter = ToSIMD(sceneAABB.center);
		const simd_float3 sceneExtents = ToSIMD(sceneAABB.extents);
		simd_float3 sceneCorners[8];
		for (int corner = 0; corner < 8; corner++)
		{
			const simd_float3 sign = simd_make_float3(
				(corner & 4) ? -1.0f : 1.0f,
				(corner & 2) ? -1.0f : 1.0f,
				(corner & 1) ? -1.0f : 1.0f);
			sceneCorners[corner] = simd_mul(
				view,
				simd_make_float4(sceneCenter + sceneExtents * sign, 1.0f)).xyz;
		}

		simd_float3 cascadeMinimum = simd_make_float3(INFINITY);
		simd_float3 cascadeMaximum = simd_make_float3(-INFINITY);
		for (int corner = 0; corner < 8; corner++)
		{
			const simd_float3 lightSpace = simd_mul(
				view,
				simd_make_float4(splitCorners[corner], 1.0f)).xyz;
			cascadeMinimum = simd_min(cascadeMinimum, lightSpace);
			cascadeMaximum = simd_max(cascadeMaximum, lightSpace);
		}

		float cascadeNear;
		float cascadeFar;
		_computeNearAndFar(
			cascadeNear,
			cascadeFar,
			cascadeMinimum,
			cascadeMaximum,
			sceneCorners);
		if (cascadeNear < 0.0f)
		{
			const float offset = -cascadeNear + 1.0f;
			position -= look * offset;
			view = Utils::LookAtLH(position, position + look, up);
			cascadeFar += offset;
			cascadeNear = 1.0f;
		}

		_cascadeCameraPosition[cascade] = simd_make_float4(position, 1.0f);
		const simd_float4x4 projection = Utils::OrthographicOffCenterLHReverseZ(
			cascadeMinimum.x,
			cascadeMaximum.x,
			cascadeMinimum.y,
			cascadeMaximum.y,
			cascadeNear,
			cascadeFar);
		_cascadeVP[cascade] = simd_mul(projection, view);
		_cascadeFrustums[cascade] = Utils::GetFrustum(_cascadeVP[cascade]);

		Frustum& frustum = _cascadeFrustums[cascade];
		frustum.cornersWS[0] = simd_make_float4(
			position + look + up * cascadeMaximum.y + right * cascadeMinimum.x, 1.0f);
		frustum.cornersWS[1] = simd_make_float4(
			position + look + up * cascadeMaximum.y + right * cascadeMaximum.x, 1.0f);
		frustum.cornersWS[2] = simd_make_float4(
			position + look + up * cascadeMinimum.y + right * cascadeMaximum.x, 1.0f);
		frustum.cornersWS[3] = simd_make_float4(
			position + look + up * cascadeMinimum.y + right * cascadeMinimum.x, 1.0f);
		frustum.cornersWS[4] = simd_make_float4(
			position + look * cascadeFar + up * cascadeMaximum.y + right * cascadeMinimum.x, 1.0f);
		frustum.cornersWS[5] = simd_make_float4(
			position + look * cascadeFar + up * cascadeMaximum.y + right * cascadeMaximum.x, 1.0f);
		frustum.cornersWS[6] = simd_make_float4(
			position + look * cascadeFar + up * cascadeMinimum.y + right * cascadeMaximum.x, 1.0f);
		frustum.cornersWS[7] = simd_make_float4(
			position + look * cascadeFar + up * cascadeMinimum.y + right * cascadeMinimum.x, 1.0f);
	}
}

void Shadows::GUINewFrame(Scene& scene)
{
	int location = Settings::ShadowsGUILocation;
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
	if (ImGui::Begin("Shadow Settings", nullptr, window_flags))
	{
		ImGui::SliderInt(
			"Cascades Count",
			&Settings::CascadesCount,
			1,
			MAX_CASCADES_COUNT,
			"%i",
			ImGuiSliderFlags_AlwaysClamp);
		Settings::FrustumsCount = Settings::CameraCount + Settings::CascadesCount;
		ImGui::InputFloat3("Direction To Light", &scene.lightDirection.x);
		ImGui::SliderFloat(
			"Shadow Distance",
			&_shadowDistance,
			ShadowMinDistance,
			Settings::CameraFarZ,
			"%.3f",
			ImGuiSliderFlags_AlwaysClamp);
		ImGui::Checkbox("Show Cascades", &_showCascades);
		ImGui::Checkbox("Show Meshlets", &Settings::ShowMeshlets);
		if (Settings::CullingEnabled)
		{
			ImGui::Checkbox("Freeze Culling", &Settings::FreezeCulling);
		}
	}

	ImGui::End();
}

void Shadows::EncodeHistory(id<MTLCommandBuffer> commandBuffer, bool softwareRasterized)
{
	id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
	encoder.label = @"Shadow Hi-Z";
	id<MTLComputePipelineState> copy = softwareRasterized
		? _resources->copySoftware
		: _resources->copyHardware;
	[encoder setComputePipelineState:copy];
	if (softwareRasterized)
	{
		[encoder setBuffer:_resources->softwareMap offset:0 atIndex:0];
	}
	else
	{
		[encoder setTexture:_resources->hardwareMap atIndex:0];
	}

	[encoder setTexture:_resources->previousHiZ atIndex:1];
	MTLSize threads = MTLSizeMake(8, 8, 1);
	[encoder dispatchThreadgroups:Dispatch2D(
			copy,
			Settings::ShadowMapRes,
			Settings::ShadowMapRes,
			Settings::CascadesCount)
			threadsPerThreadgroup:threads];
	[encoder memoryBarrierWithScope:MTLBarrierScopeTextures];

	[encoder setComputePipelineState:_resources->downsample];
	for (uint32_t mip = 1; mip < _resources->previousHiZ.mipmapLevelCount; mip++)
	{
		struct
		{
			uint32_t sourceMip;
			uint32_t destinationMip;
		} levels = { mip - 1, mip };

		[encoder setBytes:&levels length:sizeof(levels) atIndex:0];
		const NSUInteger width = std::max<NSUInteger>(1, Settings::ShadowMapRes >> mip);
		const NSUInteger height = std::max<NSUInteger>(1, Settings::ShadowMapRes >> mip);
		[encoder dispatchThreadgroups:Dispatch2D(
				_resources->downsample,
				width,
				height,
				Settings::CascadesCount)
				threadsPerThreadgroup:threads];
		[encoder memoryBarrierWithScope:MTLBarrierScopeTextures];
	}

	[encoder endEncoding];
	_hasHistory = true;
}

id<MTLTexture> Shadows::GetShadowMapHWR() const
{
	return _resources->hardwareMap;
}

id<MTLBuffer> Shadows::GetShadowMapSWR() const
{
	return _resources->softwareMap;
}

id<MTLTexture> Shadows::GetPrevFrameShadowMapMips() const
{
	return _resources->previousHiZ;
}
