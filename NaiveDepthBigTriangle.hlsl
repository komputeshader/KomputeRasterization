#include "TypesAndConstants.hlsli"
#include "Rasterization.hlsli"

static const uint ThreadsX = 16;
static const uint ThreadsY = 16;
static const uint ThreadsZ = 1;

cbuffer ConstantBuffer : register(b0)
{
	float4x4 VP;
	float2 ScreenRes;
};

cbuffer ConstantBuffer : register(b1)
{
	uint TriangleIndex;
	uint InstanceID;
	uint BaseVertexLocation;
}

struct DepthVertex
{
	float3 position;
};

typedef DepthVertex Vertex;

StructuredBuffer<Vertex> Vertices : register(t0);
StructuredBuffer<uint> Indices : register(t1);
StructuredBuffer<InstanceData> Instances : register(t2);

groupshared float2 MinP;
groupshared float2 MaxP;
groupshared float3 V0CS;
groupshared float3 V1CS;
groupshared float3 V2CS;
groupshared float InvW0;
groupshared float InvW1;
groupshared float InvW2;
groupshared float InvArea;
groupshared float Area0;
groupshared float Area1;
groupshared float Area2;
groupshared float2 dxdy0;
groupshared float2 dxdy1;
groupshared float2 dxdy2;

RWTexture2D<uint> Depth : register(u0);

#include "Common.hlsli"

[numthreads(ThreadsX, ThreadsY, ThreadsZ)]
void CSMain(
	uint3 groupID : SV_GroupID,
	uint3 dispatchThreadID : SV_DispatchThreadID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0)
	{
		// no checks for this triangle, since it had passed them already

		uint v0Idx = Indices[TriangleIndex + 0];
		uint v1Idx = Indices[TriangleIndex + 1];
		uint v2Idx = Indices[TriangleIndex + 2];

		Vertex v0 = Vertices[BaseVertexLocation + v0Idx];
		Vertex v1 = Vertices[BaseVertexLocation + v1Idx];
		Vertex v2 = Vertices[BaseVertexLocation + v2Idx];

		// instancing
		InstanceData instanceData = Instances[InstanceID];

		// MS -> WS -> VS -> CS
		v0.position = mul(instanceData.WorldTransform, float4(v0.position, 1.0)).xyz;
		v1.position = mul(instanceData.WorldTransform, float4(v1.position, 1.0)).xyz;
		v2.position = mul(instanceData.WorldTransform, float4(v2.position, 1.0)).xyz;

		float4 v0CS = mul(VP, float4(v0.position, 1.0));
		float4 v1CS = mul(VP, float4(v1.position, 1.0));
		float4 v2CS = mul(VP, float4(v2.position, 1.0));

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

		float3 minP = min(min(v0CS.xyz, v1CS.xyz), v2CS.xyz);
		float3 maxP = max(max(v0CS.xyz, v1CS.xyz), v2CS.xyz);

		// 'clipping'
		minP.xy = clamp(minP.xy, float2(0.0, 0.0), ScreenRes - float2(1.0, 1.0));
		maxP.xy = clamp(maxP.xy, float2(0.0, 0.0), ScreenRes - float2(1.0, 1.0));

		// are in terms of pixel centers now
		MinP = floor(minP.xy) + float2(0.5, 0.5);
		MaxP = floor(maxP.xy) + float2(0.5, 0.5);

		V0CS = v0CS.xyz;
		V1CS = v1CS.xyz;
		V2CS = v2CS.xyz;
		InvW0 = invW0;
		InvW1 = invW1;
		InvW2 = invW2;
		InvArea = 1.0 / area;
		EdgeFunction(V1CS.xy, V2CS.xy, MinP, Area0, dxdy0);
		EdgeFunction(V2CS.xy, V0CS.xy, MinP, Area1, dxdy1);
		EdgeFunction(V0CS.xy, V1CS.xy, MinP, Area2, dxdy2);
	}

	GroupMemoryBarrierWithGroupSync();

	float2 dimensions = MaxP - MinP;

	if (dispatchThreadID.x >= dimensions.x || dispatchThreadID.y >= dimensions.y)
	{
		return;
	}

	float x = MinP.x + dispatchThreadID.x;
	float y = MinP.y + dispatchThreadID.y;

	// E(x + a, y + b) = E(x, y) - a * dy + b * dx
	float area0 = Area0 - dispatchThreadID.x * dxdy0.y + dispatchThreadID.y * dxdy0.x;
	float area1 = Area1 - dispatchThreadID.x * dxdy1.y + dispatchThreadID.y * dxdy1.x;
	float area2 = Area2 - dispatchThreadID.x * dxdy2.y + dispatchThreadID.y * dxdy2.x;
	// edge tests, "frustum culling" for 3 lines in 2D
	[branch]
	if (area0 >= 0.0 && area1 >= 0.0 && area2 >= 0.0)
	{
		// convert to barycentric weights
		area0 *= InvArea;
		area1 *= InvArea;
		area2 = 1.0 - area0 - area1;

		float depth = area0 * V0CS.z * InvW0 + area1 * V1CS.z * InvW1 + area2 * V2CS.z * InvW2;

		InterlockedMax(Depth[uint2(x, y)], asuint(depth));
	}
}