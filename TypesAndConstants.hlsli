#ifndef TYPES_AND_CONSTANTS_HLSL
#define TYPES_AND_CONSTANTS_HLSL

#include "CPUGPUCommon.h"

static const float3 SkyColor = float3(136.0, 198.0, 252.0) / 255.0;

static const float FloatMax = 3.402823466e+38;
static const float FloatMin = 1.175494351e-38;

// https://developer.nvidia.com/content/understanding-structured-buffer-performance

struct VertexPosition
{
	float3 position;
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

struct AABB
{
	float3 center;
	float pad0;
	float3 extents;
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

	float3 coneApex;
	float3 coneAxis;
	float coneCutoff;
};

struct Instance
{
	float4x4 worldTransform;
	uint meshID;
	float3 color;
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

struct BigTriangle
{
	float3 p0WS;
	uint packedNormal0;
	float3 p1WS;
	uint packedNormal1;
	float3 p2WS;
	uint packedNormal2;
	uint2 packedColor0;
	uint2 packedColor1;
	uint2 packedColor2;
	uint packedUV0;
	uint packedUV1;
	uint packedUV2;

	float tileOffset;
};

#endif // TYPES_AND_CONSTANTS_HLSL