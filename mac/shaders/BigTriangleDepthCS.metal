#include "Common.metal"
#include "Rasterization.metal"

#ifdef SHADOWS
kernel void BigTriangleShadowCS(
#else
kernel void BigTriangleDepthCS(
#endif
	constant DepthSceneCB& constants [[buffer(7)]],
	device const uint* bigTriangles [[buffer(11)]],
	device atomic_uint* depthBuffer [[buffer(6)]],
	uint3 groupID [[threadgroup_position_in_grid]],
	uint3 groupThreadID [[thread_position_in_threadgroup]],
	uint groupIndex [[thread_index_in_threadgroup]])
{
#ifdef SHADOWS
	depthBuffer += (constants.frustumIndex - 1) *
		uint(constants.outputResolution.x) * uint(constants.outputResolution.y);
#endif

	threadgroup uint Triangle[BIG_TRIANGLE_DEPTH_FIELDS];

	threadgroup float2 MinP;
	threadgroup float2 MaxP;
	threadgroup float3 ClipZ;
	threadgroup float3 ClipW;
	threadgroup float Area0;
	threadgroup float Area1;
	threadgroup float Area2;
	threadgroup float2 Dxdy0;
	threadgroup float2 Dxdy1;
	threadgroup float2 Dxdy2;

	if (groupIndex < BIG_TRIANGLE_DEPTH_FIELDS)
	{
		Triangle[groupIndex] = bigTriangles[groupID.x * BIG_TRIANGLE_DEPTH_FIELDS + groupIndex];
	}

	// synchronize all threads after the cooperative load, including across SIMD groups
	threadgroup_barrier(mem_flags::mem_threadgroup);

	if (groupIndex == 0)
	{
		// no tests for this triangle, since it had passed them already

		// WS -> VS -> CS
		float4 p0CS = (constants.vp * float4(as_type<float3>(uint3(Triangle[P0_WS_FLOAT3 + 0], Triangle[P0_WS_FLOAT3 + 1], Triangle[P0_WS_FLOAT3 + 2])), 1.0f));
		float4 p1CS = (constants.vp * float4(as_type<float3>(uint3(Triangle[P1_WS_FLOAT3 + 0], Triangle[P1_WS_FLOAT3 + 1], Triangle[P1_WS_FLOAT3 + 2])), 1.0f));
		float4 p2CS = (constants.vp * float4(as_type<float3>(uint3(Triangle[P2_WS_FLOAT3 + 0], Triangle[P2_WS_FLOAT3 + 1], Triangle[P2_WS_FLOAT3 + 2])), 1.0f));

		uint tileOffsetData = Triangle[TILE_OFFSET_FLOAT];
		bool firstQuadHalf = (tileOffsetData & 0x80000000) == 0;

		bool p0Behind = false;
		bool p1Behind = false;
		bool p2Behind = false;

		if (constants.nearPlaneClippingEnabled)
		{
			p0Behind = p0CS.z > constants.cameraNear;
			p1Behind = p1CS.z > constants.cameraNear;
			p2Behind = p2CS.z > constants.cameraNear;

			if (p0Behind || p1Behind || p2Behind)
			{
				//        p2                             p2
				//        /\                             /\
				//       /  \            =====>         /  \
				//      /    \                         /    \
				// ----x------x------- near plane ----x------x-----
				//    /________\                     p0      p1
				//   p0        p1
				if (p0Behind && p1Behind)
				{
					p0CS = EdgeNearPlaneIntersection(p2CS.xyz, p0CS.xyz, constants.cameraNear);
					p1CS = EdgeNearPlaneIntersection(p2CS.xyz, p1CS.xyz, constants.cameraNear);
				}
				//        p0                             p0
				//        /\                             /\
				//       /  \            =====>         /  \
				//      /    \                         /    \
				// ----x------x------- near plane ----x------x-----
				//    /________\                     p2      p1
				//   p2        p1
				else if (p1Behind && p2Behind)
				{
					p1CS = EdgeNearPlaneIntersection(p0CS.xyz, p1CS.xyz, constants.cameraNear);
					p2CS = EdgeNearPlaneIntersection(p0CS.xyz, p2CS.xyz, constants.cameraNear);
				}
				//        p1                             p1
				//        /\                             /\
				//       /  \            =====>         /  \
				//      /    \                         /    \
				// ----x------x------- near plane ----x------x-----
				//    /________\                     p0      p2
				//   p0        p2
				else if (p2Behind && p0Behind)
				{
					p2CS = EdgeNearPlaneIntersection(p1CS.xyz, p2CS.xyz, constants.cameraNear);
					p0CS = EdgeNearPlaneIntersection(p1CS.xyz, p0CS.xyz, constants.cameraNear);
				}
				//  p1________p2                p1________p2
				//    \      /        =====>      \⟍     /
				//     \    /                      \ ⟍  /
				// -----x--x------- near plane -----x--x----
				//       \/                        p3  p0
				//       p0
				else if (p0Behind)
				{
					if (firstQuadHalf)
					{
						p0CS = EdgeNearPlaneIntersection(p2CS.xyz, p0CS.xyz, constants.cameraNear);
					}
					else
					{
						p2CS = EdgeNearPlaneIntersection(p2CS.xyz, p0CS.xyz, constants.cameraNear);
						p0CS = EdgeNearPlaneIntersection(p1CS.xyz, p0CS.xyz, constants.cameraNear);
					}
				}
				//  p2________p0                p2________p0
				//    \      /        =====>      \⟍     /
				//     \    /                      \ ⟍  /
				// -----x--x------- near plane -----x--x----
				//       \/                        p3  p1
				//       p1
				else if (p1Behind)
				{
					if (firstQuadHalf)
					{
						p1CS = EdgeNearPlaneIntersection(p0CS.xyz, p1CS.xyz, constants.cameraNear);
					}
					else
					{
						p0CS = EdgeNearPlaneIntersection(p0CS.xyz, p1CS.xyz, constants.cameraNear);
						p1CS = EdgeNearPlaneIntersection(p2CS.xyz, p1CS.xyz, constants.cameraNear);
					}
				}
				//  p0________p1                p0________p1
				//    \      /        =====>      \⟍     /
				//     \    /                      \ ⟍  /
				// -----x--x------- near plane -----x--x----
				//       \/                        p3  p2
				//       p2
				else if (p2Behind)
				{
					if (firstQuadHalf)
					{
						p2CS = EdgeNearPlaneIntersection(p1CS.xyz, p2CS.xyz, constants.cameraNear);
					}
					else
					{
						p1CS = EdgeNearPlaneIntersection(p1CS.xyz, p2CS.xyz, constants.cameraNear);
						p2CS = EdgeNearPlaneIntersection(p0CS.xyz, p2CS.xyz, constants.cameraNear);
					}
				}
			}
		}

		float invW0 = 1.0f / p0CS.w;
		float invW1 = 1.0f / p1CS.w;
		float invW2 = 1.0f / p2CS.w;

		float2 p0SS, p1SS, p2SS;
		GetSSPositions(
			p0CS.xy, p1CS.xy, p2CS.xy, invW0, invW1, invW2, constants.outputResolution,
			p0SS, p1SS, p2SS);

		float z0NDC = p0CS.z * invW0;
		float z1NDC = p1CS.z * invW1;
		float z2NDC = p2CS.z * invW2;

		float3 minP = min(min(float3(p0SS.xy, z0NDC), float3(p1SS.xy, z1NDC)), float3(p2SS.xy, z2NDC));
		float3 maxP = max(max(float3(p0SS.xy, z0NDC), float3(p1SS.xy, z1NDC)), float3(p2SS.xy, z2NDC));

		ClampToScreenBounds(minP, maxP, constants.outputResolution);
		minP.xy = SnapMinBoundToPixelCenter(minP.xy);
		float2 dimensions = maxP.xy - minP.xy;
		float2 tileCount = ceil(dimensions / constants.bigTriangleTileSize);
		float tileOffset = as_type<float>(tileOffsetData & 0x7FFFFFFF);
		float yTileOffset = floor(tileOffset / tileCount.x);
		float xTileOffset = tileOffset - yTileOffset * tileCount.x;
		MinP = minP.xy + float2(xTileOffset, yTileOffset) * constants.bigTriangleTileSize;
		MaxP = min(maxP.xy, MinP + float2(constants.bigTriangleTileSize) - float2(1.0f, 1.0f));

		// https://userpages.cs.umbc.edu/olano/papers/2dh-tri/
		ClipZ = float3(p0CS.z, p1CS.z, p2CS.z);
		ClipW = float3(p0CS.w, p1CS.w, p2CS.w);
		float area0Temporary;
		float2 dxdy0Temporary;
		float area1Temporary;
		float2 dxdy1Temporary;
		float area2Temporary;
		float2 dxdy2Temporary;

		EdgeFunctionHomogeneous(
			p1CS, p2CS, float2(0.0f, 0.0f),
			area0Temporary, dxdy0Temporary);
		EdgeFunctionHomogeneous(
			p2CS, p0CS, float2(0.0f, 0.0f),
			area1Temporary, dxdy1Temporary);
		EdgeFunctionHomogeneous(
			p0CS, p1CS, float2(0.0f, 0.0f),
			area2Temporary, dxdy2Temporary);
		Area0 = area0Temporary;
		Dxdy0 = dxdy0Temporary;
		Area1 = area1Temporary;
		Dxdy1 = dxdy1Temporary;
		Area2 = area2Temporary;
		Dxdy2 = dxdy2Temporary;
	}

	threadgroup_barrier(mem_flags::mem_threadgroup);

	for (
		float y = MinP.y + groupThreadID.y;
		y <= MaxP.y;
		y += SWR_BIG_TRIANGLE_THREADS_Y)
	{
		for (
			float x = MinP.x + groupThreadID.x;
			x <= MaxP.x;
			x += SWR_BIG_TRIANGLE_THREADS_X)
		{
			float2 sampleNDC = (float2(x, y) * constants.inverseOutputResolution - float2(0.5f, 0.5f)) * float2(2.0f, -2.0f);

			// E(x + a, y + b) = E(x, y) - a * dy + b * dx
			float area0 = Area0 - sampleNDC.x * Dxdy0.y + sampleNDC.y * Dxdy0.x;
			float area1 = Area1 - sampleNDC.x * Dxdy1.y + sampleNDC.y * Dxdy1.x;
			float area2 = Area2 - sampleNDC.x * Dxdy2.y + sampleNDC.y * Dxdy2.x;

			// edge tests, "frustum culling" for 3 lines in 2D
			bool insideTriangle = true;
			insideTriangle = insideTriangle && (EdgeIsTopLeft(Dxdy0) ? (area0 >= 0.0f) : (area0 > 0.0f));
			insideTriangle = insideTriangle && (EdgeIsTopLeft(Dxdy1) ? (area1 >= 0.0f) : (area1 > 0.0f));
			insideTriangle = insideTriangle && (EdgeIsTopLeft(Dxdy2) ? (area2 >= 0.0f) : (area2 > 0.0f));
			if (insideTriangle)
			{
				float3 weights = float3(area0, area1, area2);
				float weightedW = dot(weights, ClipW);

				float depth = dot(weights, ClipZ) / weightedW;

				atomic_fetch_max_explicit(&depthBuffer[uint(y) * uint(constants.outputResolution.x) + uint(x)], as_type<uint>(depth), memory_order_relaxed);
			}
		}
	}
}
