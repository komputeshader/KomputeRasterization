#include "TypesAndConstants.hlsli"

#define BIG_TRIANGLES

cbuffer SceneCB : register(b0)
{
	float4x4 VP;
	float4x4 CascadeVP[MAX_CASCADES_COUNT];
	float4 SunDirection;
	float4 CascadeBias[MAX_CASCADES_COUNT / 4];
	float4 CascadeSplits[MAX_CASCADES_COUNT / 4];
	float2 OutputRes;
	float2 InvOutputRes;
	float BigTriangleThreshold;
	float BigTriangleTileSize;
	int ShowCascades;
	int ShowMeshlets;
	int UseTopLeftRule;
	int CascadesCount;
	int ScanlineRasterization;
	float ShadowsDistance;
	uint TotalTriangles;
};

SamplerState PointClampSampler : register(s0);

StructuredBuffer<uint> BigTriangles : register(t0);
StructuredBuffer<Instance> Instances : register(t1);
Texture2D Depth : register(t2);
Texture2DArray ShadowMap : register(t3);

RWTexture2D<float4> RenderTarget : register(u0);

groupshared uint Triangle[BIG_TRIANGLE_OPAQUE_FIELDS];

groupshared float2 MinP;
groupshared float2 MaxP;
groupshared float2 P0SS;
groupshared float2 P1SS;
groupshared float2 P2SS;
groupshared float3 P0WS;
groupshared float3 P1WS;
groupshared float3 P2WS;
groupshared float3 N0;
groupshared float3 N1;
groupshared float3 N2;
groupshared float4 C0;
groupshared float4 C1;
groupshared float4 C2;
groupshared float2 UV0;
groupshared float2 UV1;
groupshared float2 UV2;
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
	if (groupIndex < BIG_TRIANGLE_OPAQUE_FIELDS)
	{
		Triangle[groupIndex] = BigTriangles[groupID.x * BIG_TRIANGLE_OPAQUE_FIELDS + groupIndex];
	}

	GroupMemoryBarrier();

	if (groupIndex == 0)
	{
		// no tests for this triangle, since it had passed them already

		P0WS = asfloat(uint3(Triangle[P0_WS_FLOAT3 + 0], Triangle[P0_WS_FLOAT3 + 1], Triangle[P0_WS_FLOAT3 + 2]));
		P1WS = asfloat(uint3(Triangle[P1_WS_FLOAT3 + 0], Triangle[P1_WS_FLOAT3 + 1], Triangle[P1_WS_FLOAT3 + 2]));
		P2WS = asfloat(uint3(Triangle[P2_WS_FLOAT3 + 0], Triangle[P2_WS_FLOAT3 + 1], Triangle[P2_WS_FLOAT3 + 2]));

		// WS -> VS -> CS
		float4 p0CS = mul(VP, float4(P0WS, 1.0));
		float4 p1CS = mul(VP, float4(P1WS, 1.0));
		float4 p2CS = mul(VP, float4(P2WS, 1.0));

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
		float yTileOffset = floor(asfloat(Triangle[TILE_OFFSET_FLOAT]) / tileCount.x);
		float xTileOffset = asfloat(Triangle[TILE_OFFSET_FLOAT]) - yTileOffset * tileCount.x;
		MinP = minP.xy + float2(xTileOffset, yTileOffset) * BigTriangleTileSize;
		MaxP = min(maxP.xy, MinP + BigTriangleTileSize.xx);

		N0 = UnpackNormal(Triangle[N0_PACKED_UINT]);
		N1 = UnpackNormal(Triangle[N1_PACKED_UINT]);
		N2 = UnpackNormal(Triangle[N2_PACKED_UINT]);

		C0 = UnpackColor(uint2(Triangle[C0_PACKED_UINT2 + 0], Triangle[C0_PACKED_UINT2 + 1]));
		C1 = UnpackColor(uint2(Triangle[C1_PACKED_UINT2 + 0], Triangle[C1_PACKED_UINT2 + 1]));
		C2 = UnpackColor(uint2(Triangle[C2_PACKED_UINT2 + 0], Triangle[C2_PACKED_UINT2 + 1]));
		//if (ShowMeshlets)
		//{
		//	C0 = float4(instance.color, 1.0);
		//	C1 = float4(instance.color, 1.0);
		//	C2 = float4(instance.color, 1.0);
		//}

		UV0 = UnpackTexcoords(Triangle[UV0_PACKED_UINT]);
		UV1 = UnpackTexcoords(Triangle[UV1_PACKED_UINT]);
		UV2 = UnpackTexcoords(Triangle[UV2_PACKED_UINT]);

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
				// early z test
				[branch]
				if (Depth[uint2(x, y)].r == depth)
				{
					// for perspective-correct interpolation
					float denom = 1.0 / (weight0 * InvW0 + weight1 * InvW1 + weight2 * InvW2);

					float3 N = denom * (weight0 * N0 * InvW0 + weight1 * N1 * InvW1 + weight2 * N2 * InvW2);
					N = normalize(N);

					float3 color = denom * (weight0 * C0.rgb * InvW0 + weight1 * C1.rgb * InvW1 + weight2 * C2.rgb * InvW2);

					float3 positionWS = denom * (weight0 * P0WS * InvW0 + weight1 * P1WS * InvW1 + weight2 * P2WS * InvW2);

					float NdotL = saturate(dot(SunDirection.xyz, N));
					float viewDepth = denom;
					float shadow = GetShadow(viewDepth, positionWS);
					float3 ambient = 0.2 * SkyColor;

					float3 result = color * (NdotL * shadow + ambient);
					if (ShowCascades)
					{
						result = GetCascadeColor(viewDepth, positionWS);
						result *= (NdotL * shadow + ambient);
					}

					RenderTarget[uint2(x, y)] = float4(result, 1.0);
				}
			}
		}
	}
}