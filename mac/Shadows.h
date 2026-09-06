#pragma once

#import <Metal/Metal.h>

#include "Common.h"

#include <memory>

class Scene;

class Shadows
{
public:

	Shadows();
	Shadows(const Shadows&) = delete;
	Shadows& operator=(const Shadows&) = delete;
	~Shadows();

	void Initialize();
	void Update(const Scene& scene);
	void GUINewFrame(Scene& scene);
	void EncodeHistory(id<MTLCommandBuffer> commandBuffer, bool softwareRasterized);

	id<MTLTexture> GetShadowMapHWR() const;
	id<MTLBuffer> GetShadowMapSWR() const;
	id<MTLTexture> GetPrevFrameShadowMapMips() const;

	const simd_float4x4& GetCascadeVP(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeVP[cascade];
	}

	const simd_float4x4& GetPrevFrameCascadeVP(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _prevFrameCascadeVP[cascade];
	}

	float GetCascadeBias(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeBias[cascade];
	}

	float GetCascadeSplitNormalized(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeSplitsNormalized[cascade];
	}

	float GetCascadeSplit(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeSplits[cascade];
	}

	const Frustum& GetCascadeFrustum(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeFrustums[cascade];
	}

	const simd_float4& GetCascadeCameraPosition(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeCameraPosition[cascade];
	}

	bool ShowCascades() const { return _showCascades; }
	float GetShadowDistance() const { return _shadowDistance; }
	bool HasHistory() const { return _hasHistory; }

private:

	static constexpr float ShadowMinDistance = 200.0f;

	void _computeNearAndFar(
		float& nearPlane,
		float& farPlane,
		simd_float3 orthographicMinimum,
		simd_float3 orthographicMaximum,
		const simd_float3* scenePoints);

	struct Resources;
	std::unique_ptr<Resources> _resources;

	simd_float4 _cascadeCameraPosition[MAX_CASCADES_COUNT] = {};
	simd_float4x4 _cascadeVP[MAX_CASCADES_COUNT] = {};
	simd_float4x4 _prevFrameCascadeVP[MAX_CASCADES_COUNT] = {};
	Frustum _cascadeFrustums[MAX_CASCADES_COUNT] = {};
	float _cascadeBias[MAX_CASCADES_COUNT] = {};
	float _cascadeSplitsNormalized[MAX_CASCADES_COUNT] = {};
	float _cascadeSplits[MAX_CASCADES_COUNT] = {};
	float _shadowDistance = 5000.0f;
	float _bias = 0.001f;

	bool _showCascades = false;
	bool _hasHistory = false;
};

