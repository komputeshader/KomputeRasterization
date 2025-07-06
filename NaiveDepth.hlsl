// Implicit assumptions:
// - we are dealing with triangles
// - indices represent a triangle list

#include "TypesAndConstants.hlsli"
#include "Rasterization.hlsli"

static const uint ThreadsX = 256;
static const uint ThreadsY = 1;
static const uint ThreadsZ = 1;

cbuffer ConstantBuffer : register(b0)
{
	float4x4 VP;
	float2 ScreenRes;
};

cbuffer ConstantBuffer : register(b1)
{
	uint IndexCountPerInstance;
	uint StartIndexLocation;
	uint BaseVertexLocation;
	uint StartInstanceLocation;
}

struct DepthVertex
{
	float3 position;
};

typedef DepthVertex Vertex;

StructuredBuffer<Vertex> Vertices : register(t0);
StructuredBuffer<uint> Indices : register(t1);
StructuredBuffer<InstanceData> Instances : register(t2);

RWTexture2D<uint> Depth : register(u0);
AppendStructuredBuffer<IndirectCommand> BigTriangles : register(u1);

#include "Common.hlsli"

[numthreads(ThreadsX, ThreadsY, ThreadsZ)]
void CSMain(
	uint3 groupID : SV_GroupID,
	uint3 dispatchThreadID : SV_DispatchThreadID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex)
{
	[branch]
	if (dispatchThreadID.x * 3 >= IndexCountPerInstance)
	{
		return;
	}

	uint v0Idx = Indices[StartIndexLocation + dispatchThreadID.x * 3 + 0];
	uint v1Idx = Indices[StartIndexLocation + dispatchThreadID.x * 3 + 1];
	uint v2Idx = Indices[StartIndexLocation + dispatchThreadID.x * 3 + 2];

	Vertex v0 = Vertices[BaseVertexLocation + v0Idx];
	Vertex v1 = Vertices[BaseVertexLocation + v1Idx];
	Vertex v2 = Vertices[BaseVertexLocation + v2Idx];

	// instancing
	uint instanceID = groupID.y;
	InstanceData instanceData = Instances[instanceID];

	// MS -> WS -> VS -> CS
	v0.position = mul(instanceData.WorldTransform, float4(v0.position, 1.0)).xyz;
	v1.position = mul(instanceData.WorldTransform, float4(v1.position, 1.0)).xyz;
	v2.position = mul(instanceData.WorldTransform, float4(v2.position, 1.0)).xyz;

	float4 v0CS = mul(VP, float4(v0.position, 1.0));
	float4 v1CS = mul(VP, float4(v1.position, 1.0));
	float4 v2CS = mul(VP, float4(v2.position, 1.0));

	// crude clipping of polygons behind the camera
	[branch]
	if (v0CS.w <= 0.0 || v1CS.w <= 0.0 || v2CS.w <= 0.0)
	{
		return;
	}

	float invW0 = 1.0 / v0CS.w;
	float invW1 = 1.0 / v1CS.w;
	float invW2 = 1.0 / v2CS.w;

	v0CS.xy *= invW0;
	v1CS.xy *= invW1;
	v2CS.xy *= invW2;

	// CS -> DX [0,1]
	v0CS.xy = v0CS.xy * float2(0.5, -0.5) + float2(0.5, 0.5);
	v1CS.xy = v1CS.xy * float2(0.5, -0.5) + float2(0.5, 0.5);
	v2CS.xy = v2CS.xy * float2(0.5, -0.5) + float2(0.5, 0.5);

	v0CS.xy *= ScreenRes;
	v1CS.xy *= ScreenRes;
	v2CS.xy *= ScreenRes;

	float area = Area(v0CS.xy, v1CS.xy, v2CS.xy);

	// backface if negative
	// TODO: degenerate case ?? (area == 0). avoid that to not divide by 0 later
	[branch]
	if (area <= 0)
	{
		return;
	}

	float3 minP = min(min(v0CS.xyz, v1CS.xyz), v2CS.xyz);
	float3 maxP = max(max(v0CS.xyz, v1CS.xyz), v2CS.xyz);

	// frustum culling
	[branch]
	if (minP.x >= ScreenRes.x || maxP.x < 0.0 ||
		maxP.y < 0.0 || minP.y >= ScreenRes.y ||
		minP.z > 1.0 || maxP.z < 0.0)
	{
		return;
	}

	// 'clipping'
	minP.xy = clamp(minP.xy, float2(0.0, 0.0), ScreenRes - float2(1.0, 1.0));
	maxP.xy = clamp(maxP.xy, float2(0.0, 0.0), ScreenRes - float2(1.0, 1.0));

	// are in terms of pixel centers now
	minP.xy = floor(minP.xy) + float2(0.5, 0.5);
	maxP.xy = floor(maxP.xy) + float2(0.5, 0.5);

	float2 dimensions = maxP.xy - minP.xy;

	[branch]
	if (dimensions.x * dimensions.y > 10000)
	{
		IndirectCommand result;
		result.TriangleIndex = StartIndexLocation + dispatchThreadID.x * 3;
		result.InstanceID = instanceID;
		result.BaseVertexLocation = BaseVertexLocation;
		result.ThreadGroupCountX = DispatchSize(16, dimensions.x);
		result.ThreadGroupCountY = DispatchSize(16, dimensions.y);
		result.ThreadGroupCountZ = 1;

		BigTriangles.Append(result);

		return;
	}

	float invArea = 1.0 / area;

	// https://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.157.4621&rep=rep1&type=pdf
	float2 dxdy0;
	float area0;
	EdgeFunction(v1CS.xy, v2CS.xy, minP.xy, area0, dxdy0);
	float2 dxdy1;
	float area1;
	EdgeFunction(v2CS.xy, v0CS.xy, minP.xy, area1, dxdy1);
	float2 dxdy2;
	float area2;
	EdgeFunction(v0CS.xy, v1CS.xy, minP.xy, area2, dxdy2);

	//  --->----
	//          |
	//  ---<----
	// |
	//  --->----
	// etc.
	float t = -1.0;
	for (float y = minP.y; y <= maxP.y; y += 1.0)
	{
		for (float x = minP.x; x <= maxP.x; x += 1.0)
		{
			// edge tests, "frustum culling" for 3 lines in 2D
			[branch]
			if (area0 >= 0.0 && area1 >= 0.0 && area2 >= 0.0)
			{
				// convert to barycentric weights
				float weight0 = area0 * invArea;
				float weight1 = area1 * invArea;
				float weight2 = 1.0 - weight0 - weight1;

				float depth = weight0 * v0CS.z * invW0 + weight1 * v1CS.z * invW1 + weight2 * v2CS.z * invW2;

				InterlockedMax(Depth[uint2(lerp(x, maxP.x - x + minP.x, t * 0.5 + 0.5), y)], asuint(depth));
			}

			area0 -= dxdy0.y;
			area1 -= dxdy1.y;
			area2 -= dxdy2.y;
		}

		t *= -1.0;

		area0 += dxdy0.x;
		area1 += dxdy1.x;
		area2 += dxdy2.x;

		dxdy0.y *= -1.0;
		dxdy1.y *= -1.0;
		dxdy2.y *= -1.0;

		area0 -= dxdy0.y;
		area1 -= dxdy1.y;
		area2 -= dxdy2.y;
	}
}