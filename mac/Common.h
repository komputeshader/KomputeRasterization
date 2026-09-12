#pragma once

#include "CPUGPUCommon.h"

#include <simd/simd.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

inline constexpr simd_float4 SkyColor =
{
	136.0f / 255.0f,
	198.0f / 255.0f,
	252.0f / 255.0f,
	1.0f
};

struct Float2
{
	float x = 0.0f;
	float y = 0.0f;
};

struct Float3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

inline simd_float3 ToSIMD(const Float3& value)
{
	return { value.x, value.y, value.z };
}

inline Float3 FromSIMD(simd_float3 value)
{
	return { value.x, value.y, value.z };
}

struct VertexPosition
{
	Float3 position;
};

struct VertexNormal
{
	// | 2 bits - unused | 10 bits - x | 10 bits - y | 10 bits - z |
	uint32_t packedNormal = 0;
};

struct VertexColor
{
	// [0] : |16 bits - r component | 16 bits - g component |
	// [1] : |16 bits - b component | 16 bits - a component |
	uint32_t packedColor[2] = {};
};

struct VertexUV
{
	// | 16 bits - u | 16 bits - v |
	uint32_t packedUV = 0;
};

struct AABB
{
	Float3 center = {};
	float pad0 = 0.0f;
	Float3 extents = {};
	float pad1 = 0.0f;

	float GetDiagonalLength() const
	{
		const simd_float3 e = ToSIMD(extents);
		return 2.0f * simd_length(e);
	}
};

struct MeshMeta
{
	AABB aabb;

	uint32_t indexCountPerInstance = 0;
	uint32_t instanceCount = 0;
	uint32_t startIndexLocation = 0;
	int32_t baseVertexLocation = 0;
	uint32_t startInstanceLocation = 0;

	Float3 coneApex = {};
	Float3 coneAxis = {};
	float coneCutoff = 0.0f;
};

struct Frustum
{
	simd_float4 l = {};
	simd_float4 r = {};
	simd_float4 b = {};
	simd_float4 t = {};
	simd_float4 n = {};
	simd_float4 f = {};

	simd_float4 cornersWS[8] = {};
};

struct Instance
{
	simd_float4x4 worldTransform = matrix_identity_float4x4;
	uint32_t meshID = 0;
	Float3 color = {};
};

struct Prefab
{
	uint32_t meshesOffset = 0;
	uint32_t meshesCount = 0;
	AABB aabb;
};

struct DrawIndexedArguments
{
	uint32_t indexCountPerInstance = 0;
	uint32_t instanceCount = 0;
	uint32_t startIndexLocation = 0;
	int32_t baseVertexLocation = 0;
	uint32_t startInstanceLocation = 0;
};

struct IndirectCommand
{
	uint32_t startInstanceLocation = 0;
	DrawIndexedArguments args;
};

struct DispatchArguments
{
	uint32_t x = 0;
	uint32_t y = 1;
	uint32_t z = 1;
};

// dispatch.x is also the hardware indirect command range length.
struct CullingCommandArguments
{
	uint32_t location = 0;
	DispatchArguments dispatch;
};

struct BigTriangleDepth
{
	float tileOffset = 0.0f;
	Float3 p0WS;
	Float3 p1WS;
	Float3 p2WS;
};

struct BigTriangleOpaque
{
	float tileOffset = 0.0f;
	Float3 p0WS;
	Float3 p1WS;
	Float3 p2WS;
	uint32_t packedNormal0 = 0;
	uint32_t packedNormal1 = 0;
	uint32_t packedNormal2 = 0;
	uint32_t packedColor0[2] = {};
	uint32_t packedColor1[2] = {};
	uint32_t packedColor2[2] = {};
	uint32_t packedUV0 = 0;
	uint32_t packedUV1 = 0;
	uint32_t packedUV2 = 0;
};

struct CullingCB
{
	uint32_t totalInstancesCount = 0;
	uint32_t totalMeshesCount = 0;
	uint32_t maxSceneInstancesCount = 0;
	uint32_t maxSceneMeshesMetaCount = 0;
	uint32_t cascadesCount = 0;
	uint32_t frustumsCount = 0;
	uint32_t frustumCullingEnabled = 0;
	uint32_t cameraHiZCullingEnabled = 0;
	uint32_t shadowsHiZCullingEnabled = 0;
	uint32_t clusterBackfaceCullingEnabled = 0;
	uint32_t hasCameraHistory = 0;
	uint32_t hasShadowHistory = 0;
	simd_float2 depthResolution = {};
	simd_float2 shadowMapResolution = {};
	simd_float4 cameraPosition = {};
	simd_float4 lightDirection = {};
	simd_float4x4 prevFrameCameraVP = matrix_identity_float4x4;
	simd_float4x4 prevFrameCascadeVP[MAX_CASCADES_COUNT] = {};
	Frustum camera;
	Frustum cascade[MAX_CASCADES_COUNT];
};

struct DepthSceneCB
{
	simd_float4x4 vp = matrix_identity_float4x4;
	simd_float2 outputResolution = {};
	simd_float2 inverseOutputResolution = {};
	float bigTriangleThreshold = 4096.0f;
	float bigTriangleTileSize = 128.0f;
	uint32_t scanlineRasterization = 1;
	uint32_t totalTriangles = 0;
	uint32_t perTriangleHiZCullingEnabled = 0;
	uint32_t frustumIndex = 0;
	uint32_t maxSceneMeshes = 0;
	uint32_t maxSceneInstances = 0;
	uint32_t hasHiZHistory = 0;
	float cameraNear = 0.0f;
	uint32_t nearPlaneClippingEnabled = 0;
	uint32_t padding = 0;
};

struct SceneCB
{
	simd_float4x4 vp = matrix_identity_float4x4;
	simd_float4x4 cascadeVP[MAX_CASCADES_COUNT] = {};
	simd_float4 sunDirection = {};
	simd_float4 cascadeBias[MAX_CASCADES_COUNT / 4] = {};
	simd_float4 cascadeSplits[MAX_CASCADES_COUNT / 4] = {};
	simd_float2 outputResolution = {};
	simd_float2 inverseOutputResolution = {};
	simd_float2 shadowMapResolution = {};
	float bigTriangleThreshold = 4096.0f;
	float bigTriangleTileSize = 128.0f;
	uint32_t showCascades = 0;
	uint32_t showMeshlets = 0;
	uint32_t cascadesCount = 4;
	uint32_t scanlineRasterization = 1;
	float shadowsDistance = 5000.0f;
	uint32_t totalTriangles = 0;
	uint32_t perTriangleHiZCullingEnabled = 0;
	uint32_t hasHiZHistory = 0;
	uint32_t showOverdraw = 0;
	float cameraNear = 0.0f;
};

enum class ScenesIndices : uint32_t
{
	Buddha,
	Plant,
	ScenesCount
};

static_assert(sizeof(Float3) == 12);
static_assert(sizeof(VertexPosition) == 12);
static_assert(sizeof(AABB) == 32);
static_assert(sizeof(MeshMeta) == 80);
static_assert(sizeof(Instance) == 80);
static_assert(sizeof(DispatchArguments) == 12);
static_assert(sizeof(CullingCommandArguments) == 16);
static_assert(offsetof(CullingCommandArguments, dispatch) == 4);
static_assert(sizeof(BigTriangleDepth) == 40);
static_assert(sizeof(BigTriangleOpaque) == 88);
static_assert(sizeof(DepthSceneCB) == 128);
static_assert(sizeof(SceneCB) == 736);
