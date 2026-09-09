#include "Common.metal"
#include "Rasterization.metal"

kernel void BigTriangleOpaqueCS(
	constant SceneCB& constants [[buffer(7)]],
	device const uint* bigTriangles [[buffer(11)]],
	device const uint* depthBuffer [[buffer(6)]],
	device const uint* shadowMap [[buffer(8)]],
	device atomic_uint* fragmentOverdraw [[buffer(13)]],
	texture2d<float, access::write> output [[texture(2)]],
	uint3 groupID [[threadgroup_position_in_grid]],
	uint3 groupThreadID [[thread_position_in_threadgroup]],
	uint groupIndex [[thread_index_in_threadgroup]])
{
	threadgroup uint Triangle[BIG_TRIANGLE_OPAQUE_FIELDS];

	threadgroup float2 MinP;
	threadgroup float2 MaxP;
	threadgroup float3 P0WS;
	threadgroup float3 P1WS;
	threadgroup float3 P2WS;
	threadgroup float3 N0;
	threadgroup float3 N1;
	threadgroup float3 N2;
	threadgroup float4 C0;
	threadgroup float4 C1;
	threadgroup float4 C2;
	threadgroup float2 UV0;
	threadgroup float2 UV1;
	threadgroup float2 UV2;
	threadgroup float3 ClipZ;
	threadgroup float3 ClipW;
	threadgroup float Area0;
	threadgroup float Area1;
	threadgroup float Area2;
	threadgroup float2 Dxdy0;
	threadgroup float2 Dxdy1;
	threadgroup float2 Dxdy2;

	if (groupIndex < BIG_TRIANGLE_OPAQUE_FIELDS)
	{
		Triangle[groupIndex] = bigTriangles[groupID.x * BIG_TRIANGLE_OPAQUE_FIELDS + groupIndex];
	}

	// synchronize all threads after the cooperative load, including across SIMD groups
	threadgroup_barrier(mem_flags::mem_threadgroup);

	if (groupIndex == 0)
	{
		// no tests for this triangle, since it had passed them already

		P0WS = as_type<float3>(uint3(Triangle[P0_WS_FLOAT3 + 0], Triangle[P0_WS_FLOAT3 + 1], Triangle[P0_WS_FLOAT3 + 2]));
		P1WS = as_type<float3>(uint3(Triangle[P1_WS_FLOAT3 + 0], Triangle[P1_WS_FLOAT3 + 1], Triangle[P1_WS_FLOAT3 + 2]));
		P2WS = as_type<float3>(uint3(Triangle[P2_WS_FLOAT3 + 0], Triangle[P2_WS_FLOAT3 + 1], Triangle[P2_WS_FLOAT3 + 2]));

		N0 = UnpackNormal(Triangle[N0_PACKED_UINT]);
		N1 = UnpackNormal(Triangle[N1_PACKED_UINT]);
		N2 = UnpackNormal(Triangle[N2_PACKED_UINT]);

		C0 = UnpackColor(uint2(Triangle[C0_PACKED_UINT2 + 0], Triangle[C0_PACKED_UINT2 + 1]));
		C1 = UnpackColor(uint2(Triangle[C1_PACKED_UINT2 + 0], Triangle[C1_PACKED_UINT2 + 1]));
		C2 = UnpackColor(uint2(Triangle[C2_PACKED_UINT2 + 0], Triangle[C2_PACKED_UINT2 + 1]));

		UV0 = UnpackTexcoords(Triangle[UV0_PACKED_UINT]);
		UV1 = UnpackTexcoords(Triangle[UV1_PACKED_UINT]);
		UV2 = UnpackTexcoords(Triangle[UV2_PACKED_UINT]);

		// WS -> VS -> CS
		float4 p0CS = (constants.vp * float4(P0WS, 1.0f));
		float4 p1CS = (constants.vp * float4(P1WS, 1.0f));
		float4 p2CS = (constants.vp * float4(P2WS, 1.0f));

		uint tileOffsetData = Triangle[TILE_OFFSET_FLOAT];
		bool firstQuadHalf = (tileOffsetData & 0x80000000) == 0;
		bool p0Behind = p0CS.z > constants.cameraNear;
		bool p1Behind = p1CS.z > constants.cameraNear;
		bool p2Behind = p2CS.z > constants.cameraNear;

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
				float t0, t1;
				p0CS = EdgeNearPlaneIntersection(
					p2CS.xyz, p0CS.xyz, constants.cameraNear,
					t0);
				p1CS = EdgeNearPlaneIntersection(
					p2CS.xyz, p1CS.xyz, constants.cameraNear,
					t1);
				P0WS = mix(P2WS, P0WS, t0);
				P1WS = mix(P2WS, P1WS, t1);
				N0 = mix(N2, N0, t0);
				N1 = mix(N2, N1, t1);
				C0 = mix(C2, C0, t0);
				C1 = mix(C2, C1, t1);
				UV0 = mix(UV2, UV0, t0);
				UV1 = mix(UV2, UV1, t1);
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
				float t1, t2;
				p1CS = EdgeNearPlaneIntersection(
					p0CS.xyz, p1CS.xyz, constants.cameraNear,
					t1);
				p2CS = EdgeNearPlaneIntersection(
					p0CS.xyz, p2CS.xyz, constants.cameraNear,
					t2);
				P1WS = mix(P0WS, P1WS, t1);
				P2WS = mix(P0WS, P2WS, t2);
				N1 = mix(N0, N1, t1);
				N2 = mix(N0, N2, t2);
				C1 = mix(C0, C1, t1);
				C2 = mix(C0, C2, t2);
				UV1 = mix(UV0, UV1, t1);
				UV2 = mix(UV0, UV2, t2);
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
				float t2, t0;
				p2CS = EdgeNearPlaneIntersection(
					p1CS.xyz, p2CS.xyz, constants.cameraNear,
					t2);
				p0CS = EdgeNearPlaneIntersection(
					p1CS.xyz, p0CS.xyz, constants.cameraNear,
					t0);
				P2WS = mix(P1WS, P2WS, t2);
				P0WS = mix(P1WS, P0WS, t0);
				N2 = mix(N1, N2, t2);
				N0 = mix(N1, N0, t0);
				C2 = mix(C1, C2, t2);
				C0 = mix(C1, C0, t0);
				UV2 = mix(UV1, UV2, t2);
				UV0 = mix(UV1, UV0, t0);
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
					float t0;
					p0CS = EdgeNearPlaneIntersection(
						p2CS.xyz, p0CS.xyz, constants.cameraNear,
						t0);
					P0WS = mix(P2WS, P0WS, t0);
					N0 = mix(N2, N0, t0);
					C0 = mix(C2, C0, t0);
					UV0 = mix(UV2, UV0, t0);
				}
				else
				{
					float t2, t0;
					p2CS = EdgeNearPlaneIntersection(
						p2CS.xyz, p0CS.xyz, constants.cameraNear,
						t2);
					p0CS = EdgeNearPlaneIntersection(
						p1CS.xyz, p0CS.xyz, constants.cameraNear,
						t0);
					P2WS = mix(P2WS, P0WS, t2);
					P0WS = mix(P1WS, P0WS, t0);
					N2 = mix(N2, N0, t2);
					N0 = mix(N1, N0, t0);
					C2 = mix(C2, C0, t2);
					C0 = mix(C1, C0, t0);
					UV2 = mix(UV2, UV0, t2);
					UV0 = mix(UV1, UV0, t0);
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
					float t1;
					p1CS = EdgeNearPlaneIntersection(
						p0CS.xyz, p1CS.xyz, constants.cameraNear,
						t1);
					P1WS = mix(P0WS, P1WS, t1);
					N1 = mix(N0, N1, t1);
					C1 = mix(C0, C1, t1);
					UV1 = mix(UV0, UV1, t1);
				}
				else
				{
					float t0, t1;
					p0CS = EdgeNearPlaneIntersection(
						p0CS.xyz, p1CS.xyz, constants.cameraNear,
						t0);
					p1CS = EdgeNearPlaneIntersection(
						p2CS.xyz, p1CS.xyz, constants.cameraNear,
						t1);
					P0WS = mix(P0WS, P1WS, t0);
					P1WS = mix(P2WS, P1WS, t1);
					N0 = mix(N0, N1, t0);
					N1 = mix(N2, N1, t1);
					C0 = mix(C0, C1, t0);
					C1 = mix(C2, C1, t1);
					UV0 = mix(UV0, UV1, t0);
					UV1 = mix(UV2, UV1, t1);
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
					float t2;
					p2CS = EdgeNearPlaneIntersection(
						p1CS.xyz, p2CS.xyz, constants.cameraNear,
						t2);
					P2WS = mix(P1WS, P2WS, t2);
					N2 = mix(N1, N2, t2);
					C2 = mix(C1, C2, t2);
					UV2 = mix(UV1, UV2, t2);
				}
				else
				{
					float t1, t2;
					p1CS = EdgeNearPlaneIntersection(
						p1CS.xyz, p2CS.xyz, constants.cameraNear,
						t1);
					p2CS = EdgeNearPlaneIntersection(
						p0CS.xyz, p2CS.xyz, constants.cameraNear,
						t2);
					P1WS = mix(P1WS, P2WS, t1);
					P2WS = mix(P0WS, P2WS, t2);
					N1 = mix(N1, N2, t1);
					N2 = mix(N0, N2, t2);
					C1 = mix(C1, C2, t1);
					C2 = mix(C0, C2, t2);
					UV1 = mix(UV1, UV2, t1);
					UV2 = mix(UV0, UV2, t2);
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

		//if (constants.showMeshlets)
		//{
		//	C0 = float4(instance.color, 1.0f);
		//	C1 = float4(instance.color, 1.0f);
		//	C2 = float4(instance.color, 1.0f);
		//}

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

				uint2 pixelCoord = uint2(x, y);
				if (constants.showOverdraw)
				{
					atomic_fetch_add_explicit(&fragmentOverdraw[pixelCoord.y * uint(constants.outputResolution.x) + pixelCoord.x], 1u, memory_order_relaxed);
				}
				// early z test
				else if (as_type<float>(depthBuffer[pixelCoord.y * uint(constants.outputResolution.x) + pixelCoord.x]) == depth)
				{
					// homogeneous edge values already include the 1 / w factor
					float invWeightSum = 1.0f / (weights.x + weights.y + weights.z);
					float denom = weightedW * invWeightSum;
					weights *= invWeightSum;

					float3 N = weights.x * N0 + weights.y * N1 + weights.z * N2;
					N = normalize(N);

					float3 color = weights.x * C0.rgb + weights.y * C1.rgb + weights.z * C2.rgb;

					float3 positionWS = weights.x * P0WS + weights.y * P1WS + weights.z * P2WS;

					float NdotL = saturate(dot(constants.sunDirection.xyz, N));
					float viewDepth = denom;
					float shadow = GetShadow(viewDepth, positionWS, constants, shadowMap);
					float3 ambient = 0.2f * SkyColor.rgb;

					float3 result = color * (NdotL * shadow + ambient);
					if (constants.showCascades)
					{
						result = GetCascadeColor(viewDepth, constants);
						result *= (NdotL * shadow + ambient);
					}

					output.write(float4(result, 1.0f), pixelCoord);
				}
			}
		}
	}
}
