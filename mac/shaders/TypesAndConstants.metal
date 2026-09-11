#ifndef TYPES_AND_CONSTANTS_METAL
#define TYPES_AND_CONSTANTS_METAL

#include <metal_stdlib>

using namespace metal;

#include "../CPUGPUCommon.h"

constant float4 SkyColor = float4(136.0f, 198.0f, 252.0f, 255.0f) / 255.0f;

constant float FloatMax = 3.402823466e+38f;

struct VertexPosition
{
	packed_float3 position;
};

struct VertexNormal
{
	// | 2 bits - unused | 10 bits - x | 10 bits - y | 10 bits - z |
	uint packedNormal;
};

struct VertexColor
{
	// .x : |16 bits - r component | 16 bits - g component |
	// .y : |16 bits - b component | 16 bits - a component |
	uint2 packedColor;
};

struct VertexUV
{
	// | 16 bits - u | 16 bits - v |
	uint packedUV;
};

struct AABB
{
	packed_float3 center;
	float pad0;
	packed_float3 extents;
	float pad1;
};

struct MeshMeta
{
	AABB aabb;

	uint indexCountPerInstance;
	uint instanceCount;
	uint startIndexLocation;
	int baseVertexLocation;
	uint startInstanceLocation;

	packed_float3 coneApex;
	packed_float3 coneAxis;
	float coneCutoff;
};

struct Instance
{
	float4x4 worldTransform;
	uint meshID;
	packed_float3 color;
};

struct DrawIndexedArguments
{
	uint indexCountPerInstance;
	uint instanceCount;
	uint startIndexLocation;
	int baseVertexLocation;
	uint startInstanceLocation;
};

struct IndirectCommand
{
	uint startInstanceLocation;
	DrawIndexedArguments args;
};

struct Frustum
{
	float4 left;
	float4 right;
	float4 bottom;
	float4 top;
	float4 near;
	float4 far;

	float4 corners[8];
};

struct CullingCB
{
	uint totalInstancesCount;
	uint totalMeshesCount;
	uint maxSceneInstancesCount;
	uint maxSceneMeshesMetaCount;
	uint cascadesCount;
	uint frustumsCount;
	uint frustumCullingEnabled;
	uint cameraHiZCullingEnabled;
	uint shadowsHiZCullingEnabled;
	uint clusterBackfaceCullingEnabled;
	uint hasCameraHistory;
	uint hasShadowHistory;
	float2 depthResolution;
	float2 shadowMapResolution;
	float4 cameraPosition;
	float4 lightDirection;
	float4x4 prevFrameCameraVP;
	float4x4 prevFrameCascadeVP[MAX_CASCADES_COUNT];
	Frustum camera;
	Frustum cascade[MAX_CASCADES_COUNT];
};

struct DepthSceneCB
{
	float4x4 vp;
	float2 outputResolution;
	float2 inverseOutputResolution;
	float bigTriangleThreshold;
	float bigTriangleTileSize;
	uint scanlineRasterization;
	uint totalTriangles;
	uint perTriangleHiZCullingEnabled;
	uint frustumIndex;
	uint maxSceneMeshes;
	uint maxSceneInstances;
	uint hasHiZHistory;

	float cameraNear;
	uint nearPlaneClippingEnabled;
	uint padding;
};

struct SceneCB
{
	float4x4 vp;
	float4x4 cascadeVP[MAX_CASCADES_COUNT];
	float4 sunDirection;
	float4 cascadeBias[MAX_CASCADES_COUNT / 4];
	float4 cascadeSplits[MAX_CASCADES_COUNT / 4];
	float2 outputResolution;
	float2 inverseOutputResolution;
	float2 shadowMapResolution;
	float bigTriangleThreshold;
	float bigTriangleTileSize;
	uint showCascades;
	uint showMeshlets;
	uint cascadesCount;
	uint scanlineRasterization;
	float shadowsDistance;
	uint totalTriangles;
	uint perTriangleHiZCullingEnabled;
	uint hasHiZHistory;

	uint showOverdraw;
	float cameraNear;
};

struct BigTriangleDepth
{
	float tileOffset;
	float p0WSX;
	float p0WSY;
	float p0WSZ;
	float p1WSX;
	float p1WSY;
	float p1WSZ;
	float p2WSX;
	float p2WSY;
	float p2WSZ;
};

struct BigTriangleOpaque
{
	float tileOffset;
	float p0WSX;
	float p0WSY;
	float p0WSZ;
	float p1WSX;
	float p1WSY;
	float p1WSZ;
	float p2WSX;
	float p2WSY;
	float p2WSZ;
	uint packedNormal0;
	uint packedNormal1;
	uint packedNormal2;
	uint packedColor0X;
	uint packedColor0Y;
	uint packedColor1X;
	uint packedColor1Y;
	uint packedColor2X;
	uint packedColor2Y;
	uint packedUV0;
	uint packedUV1;
	uint packedUV2;
};

static_assert(sizeof(BigTriangleDepth) == 40, "BigTriangleDepth must match the CPU layout");
static_assert(sizeof(BigTriangleOpaque) == 88, "BigTriangleOpaque must match the CPU layout");
static_assert(sizeof(DepthSceneCB) == 128, "DepthSceneCB must match the CPU layout");
static_assert(sizeof(SceneCB) == 736, "SceneCB must match the CPU layout");

struct DispatchArguments
{
	atomic_uint x;
	uint y;
	uint z;
};

static_assert(sizeof(DispatchArguments) == 12, "DispatchArguments must match the CPU layout");

struct ICBExecutionRange
{
	uint location;
	atomic_uint length;
};

struct DepthVSInput
{
	float3 position [[attribute(0)]];
};

struct VSInput
{
	float3 position [[attribute(0)]];
	uint normal [[attribute(1)]];
	uint2 color [[attribute(2)]];
	uint uv [[attribute(3)]];
};

struct VSOutput
{
	float4 position [[position]];
	float3 positionWS;
	float linearDepth;
	float3 normal;
	float4 color;
	float2 uv;
};

#endif // TYPES_AND_CONSTANTS_METAL
