#include "TypesAndConstants.hlsli"

#define BIG_TRIANGLES

cbuffer DepthSceneCB : register(b0)
{
	float4x4 VP;
	float2 OutputRes;
	float2 InvOutputRes;
	float BigTriangleThreshold;
	float BigTriangleTileSize;
	int UseTopLeftRule;
	int ScanlineRasterization;
	uint TotalTriangles;
};

StructuredBuffer<VertexPosition> Positions : register(t0);

StructuredBuffer<Instance> Instances : register(t9);
StructuredBuffer<BigTriangle> BigTriangles : register(t10);

RWTexture2D<uint> Depth : register(u0);

groupshared float2 MinP;
groupshared float2 MaxP;
groupshared float2 P0SS;
groupshared float2 P1SS;
groupshared float2 P2SS;
groupshared float Z0NDC;
groupshared float Z1NDC;
groupshared float Z2NDC;
groupshared float InvW0;
groupshared float InvW1;
groupshared float InvW2;
groupshared float InvArea;
groupshared float Area0;
groupshared float Area1;
groupshared float Area2;
groupshared float2 Dxdy0;
groupshared float2 Dxdy1;
groupshared float2 Dxdy2;

#include "Common.hlsli"
#include "Rasterization.hlsli"

[numthreads(SWR_BIG_TRIANGLE_THREADS_X, SWR_BIG_TRIANGLE_THREADS_Y, SWR_BIG_TRIANGLE_THREADS_Z)]
void main(
	uint3 groupID : SV_GroupID,
	uint3 dispatchThreadID : SV_DispatchThreadID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0)
	{
		BigTriangle t = BigTriangles[groupID.x];

		// no tests for this triangle, since it had passed them already

		// WS -> VS -> CS
		float4 p0CS = mul(VP, float4(t.p0WS, 1.0));
		float4 p1CS = mul(VP, float4(t.p1WS, 1.0));
		float4 p2CS = mul(VP, float4(t.p2WS, 1.0));

		float invW0 = 1.0 / p0CS.w;
		float invW1 = 1.0 / p1CS.w;
		float invW2 = 1.0 / p2CS.w;

		float2 p0SS, p1SS, p2SS;
		GetSSPositions(p0CS.xy, p1CS.xy, p2CS.xy, invW0, invW1, invW2, p0SS, p1SS, p2SS);

		float area = Area(p0SS.xy, p1SS.xy, p2SS.xy);

		float z0NDC = p0CS.z * invW0;
		float z1NDC = p1CS.z * invW1;
		float z2NDC = p2CS.z * invW2;

		float3 minP = min(min(float3(p0SS.xy, z0NDC), float3(p1SS.xy, z1NDC)), float3(p2SS.xy, z2NDC));
		float3 maxP = max(max(float3(p0SS.xy, z0NDC), float3(p1SS.xy, z1NDC)), float3(p2SS.xy, z2NDC));

		ClampToScreenBounds(minP.xy, maxP.xy);
		minP.xy = SnapMinBoundToPixelCenter(minP.xy);
		float2 dimensions = maxP.xy - minP.xy;
		float2 tileCount = ceil(dimensions / BigTriangleTileSize);
		float yTileOffset = floor(t.tileOffset / tileCount.x);
		float xTileOffset = t.tileOffset - yTileOffset * tileCount.x;
		MinP = minP.xy + float2(xTileOffset, yTileOffset) * BigTriangleTileSize;
		MaxP = min(maxP.xy, MinP + BigTriangleTileSize.xx);

		P0SS = p0SS;
		P1SS = p1SS;
		P2SS = p2SS;
		Z0NDC = z0NDC;
		Z1NDC = z1NDC;
		Z2NDC = z2NDC;
		InvW0 = invW0;
		InvW1 = invW1;
		InvW2 = invW2;
		InvArea = 1.0 / area;

		// https://www.cs.drexel.edu/~david/Classes/Papers/comp175-06-pineda.pdf
		EdgeFunction(p1SS.xy, p2SS.xy, MinP, Area0, Dxdy0);
		EdgeFunction(p2SS.xy, p0SS.xy, MinP, Area1, Dxdy1);
		EdgeFunction(p0SS.xy, p1SS.xy, MinP, Area2, Dxdy2);
	}

	GroupMemoryBarrierWithGroupSync();

	uint yTiles = 0;
	for (
		float y = MinP.y + groupThreadID.y;
		y <= MaxP.y;
		y += SWR_BIG_TRIANGLE_THREADS_Y, yTiles++)
	{
		uint yOffset = groupThreadID.y + yTiles * SWR_BIG_TRIANGLE_THREADS_Y;

		uint xTiles = 0;
		for (
			float x = MinP.x + groupThreadID.x;
			x <= MaxP.x;
			x += SWR_BIG_TRIANGLE_THREADS_X, xTiles++)
		{
			uint xOffset = groupThreadID.x + xTiles * SWR_BIG_TRIANGLE_THREADS_X;

			// E(x + a, y + b) = E(x, y) - a * dy + b * dx
			float area0 = Area0 - xOffset * Dxdy0.y + yOffset * Dxdy0.x;
			float area1 = Area1 - xOffset * Dxdy1.y + yOffset * Dxdy1.x;
			float area2 = Area2 - xOffset * Dxdy2.y + yOffset * Dxdy2.x;

			// edge tests, "frustum culling" for 3 lines in 2D
			bool insideTriangle = true;
			if (UseTopLeftRule)
			{
				insideTriangle = insideTriangle && (EdgeIsTopLeft(P1SS.xy, P2SS.xy) ? (area0 >= 0.0) : (area0 > 0.0));
				insideTriangle = insideTriangle && (EdgeIsTopLeft(P2SS.xy, P0SS.xy) ? (area1 >= 0.0) : (area1 > 0.0));
				insideTriangle = insideTriangle && (EdgeIsTopLeft(P0SS.xy, P1SS.xy) ? (area2 >= 0.0) : (area2 > 0.0));
			}
			else
			{
				insideTriangle = area0 >= 0.0 && area1 >= 0.0 && area2 >= 0.0;
			}

			[branch]
			if (insideTriangle)
			{
				// convert to barycentric weights
				float weight0 = area0 * InvArea;
				float weight1 = area1 * InvArea;
				float weight2 = 1.0 - weight0 - weight1;

				precise float depth = weight0 * Z0NDC + weight1 * Z1NDC + weight2 * Z2NDC;

				InterlockedMax(Depth[uint2(x, y)], asuint(depth));
			}
		}
	}
}