// Implicit assumptions:
// - we are dealing with triangles
// - indices represent a triangle list

#include "Common.metal"
#include "Rasterization.metal"

kernel void TriangleOpaqueCS(
	device const VertexPosition* positions [[buffer(0)]],
	device const VertexNormal* normals [[buffer(1)]],
	device const VertexColor* colors [[buffer(2)]],
	device const VertexUV* texcoords [[buffer(3)]],
	device const uint* indices [[buffer(4)]],
	device const Instance* instances [[buffer(5)]],
	constant SceneCB& constants [[buffer(7)]],
	device const IndirectCommand* commands [[buffer(9)]],
	device atomic_uint* statistics [[buffer(10)]],
	device BigTriangleOpaque* bigTriangles [[buffer(11)]],
	device DispatchArguments& arguments [[buffer(12)]],
	device const uint* depthBuffer [[buffer(6)]],
	device const uint* shadowMap [[buffer(8)]],
	device atomic_uint* fragmentOverdraw [[buffer(13)]],
	texture2d<float, access::write> output [[texture(2)]],
	texture2d<float> previousDepth [[texture(3)]],
	uint3 groupID [[threadgroup_position_in_grid]],
	uint3 groupThreadID [[thread_position_in_threadgroup]],
	uint groupIndex [[thread_index_in_threadgroup]])
{
	threadgroup IndirectCommand command;
	threadgroup atomic_uint statisticsSM[2];

	if (groupIndex == 0)
	{
		command = commands[groupID.x];
		atomic_store_explicit(&statisticsSM[0], 0, memory_order_relaxed);
		atomic_store_explicit(&statisticsSM[1], 0, memory_order_relaxed);
	}

	threadgroup_barrier(mem_flags::mem_threadgroup);

	for (uint meshletChunkIndex = 0; meshletChunkIndex < TRIANGLES_PER_THREAD; meshletChunkIndex++)
	{
		const uint triangleIndex =
			groupThreadID.x + meshletChunkIndex * SWR_TRIANGLE_THREADS_X;

		if (triangleIndex * 3 >= command.args.indexCountPerInstance)
		{
			continue;
		}

		uint i0, i1, i2;
		GetTriangleIndices(
			indices, constants.totalTriangles, command.args.startIndexLocation + triangleIndex * 3,
			i0, i1, i2);

		float3 p0, p1, p2;
		GetTriangleVertexPositions(
			positions, i0, i1, i2, command.args.baseVertexLocation,
			p0, p1, p2);

		VertexNormal n0P, n1P, n2P;
		GetPackedVertexNormals(
			normals, i0, i1, i2, command.args.baseVertexLocation,
			n0P, n1P, n2P);
		VertexColor c0P, c1P, c2P;
		GetPackedVertexColors(
			colors, i0, i1, i2, command.args.baseVertexLocation,
			c0P, c1P, c2P);

		for (uint instanceID = 0; instanceID < command.args.instanceCount; instanceID++)
		{
			// one more triangle attempted to be rendered
			atomic_fetch_add_explicit(&statisticsSM[0], 1u, memory_order_relaxed);

			float3 p0WS, p1WS, p2WS;
			float4 p0CS, p1CS, p2CS;
			Instance instance = instances[command.startInstanceLocation + instanceID];
			GetCSPositions(
				instance, p0, p1, p2, constants.vp,
				p0WS, p1WS, p2WS, p0CS, p1CS, p2CS);

			// https://userpages.cs.umbc.edu/olano/papers/2dh-tri/ (section 5.2)
			// backface culling before clipping and division by w
			// reverse the cross product because screen y points down
			// NOTE: not actually faster (or even lil bit slower) than standard backface culling
			if (dot(p0CS.xyw, cross(p2CS.xyw, p1CS.xyw)) <= 0.0f)
			{
				continue;
			}

			// near plane clipping handling adds to register pressure and processing costs,
			// and could be avoided for most triangles by tagging meshlets, as crossing
			// the near plane, at the culling stage
			// however, that's an optimization for the concrete renderer architecture,
			// and isn't the general rasterizer optimization
			bool p0Behind = p0CS.z > constants.cameraNear;
			bool p1Behind = p1CS.z > constants.cameraNear;
			bool p2Behind = p2CS.z > constants.cameraNear;
			float4 p3Helper = float4(0.0f, 0.0f, 0.0f, 0.0f);
			bool quadrilateral = false;

			if (p0Behind || p1Behind || p2Behind)
			{
				if (p0Behind && p1Behind && p2Behind)
				{
					continue;
				}

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
					p3Helper = EdgeNearPlaneIntersection(p1CS.xyz, p0CS.xyz, constants.cameraNear);
					p0CS = EdgeNearPlaneIntersection(p2CS.xyz, p0CS.xyz, constants.cameraNear);
					quadrilateral = true;
				}

				//  p2________p0                p2________p0
				//    \      /        =====>      \⟍     /
				//     \    /                      \ ⟍  /
				// -----x--x------- near plane -----x--x----
				//       \/                        p3  p1
				//       p1
				else if (p1Behind)
				{
					p3Helper = EdgeNearPlaneIntersection(p2CS.xyz, p1CS.xyz, constants.cameraNear);
					p1CS = EdgeNearPlaneIntersection(p0CS.xyz, p1CS.xyz, constants.cameraNear);
					quadrilateral = true;
				}

				//  p0________p1                p0________p1
				//    \      /        =====>      \⟍     /
				//     \    /                      \ ⟍  /
				// -----x--x------- near plane -----x--x----
				//       \/                        p3  p2
				//       p2
				else if (p2Behind)
				{
					p3Helper = EdgeNearPlaneIntersection(p0CS.xyz, p2CS.xyz, constants.cameraNear);
					p2CS = EdgeNearPlaneIntersection(p1CS.xyz, p2CS.xyz, constants.cameraNear);
					quadrilateral = true;
				}
			}

			// 1 / z for each vertex (z in VS)
			float invW0 = 1.0f / p0CS.w;
			float invW1 = 1.0f / p1CS.w;
			float invW2 = 1.0f / p2CS.w;

			float2 p0SS, p1SS, p2SS;
			GetSSPositions(p0CS.xy, p1CS.xy, p2CS.xy, invW0, invW1, invW2, constants.outputResolution, p0SS, p1SS, p2SS);

			float z0NDC = p0CS.z * invW0;
			float z1NDC = p1CS.z * invW1;
			float z2NDC = p2CS.z * invW2;

			float3 minP = min(min(float3(p0SS.xy, z0NDC), float3(p1SS.xy, z1NDC)), float3(p2SS.xy, z2NDC));
			float3 maxP = max(max(float3(p0SS.xy, z0NDC), float3(p1SS.xy, z1NDC)), float3(p2SS.xy, z2NDC));

			// frustum culling
			if (minP.x >= constants.outputResolution.x || maxP.x < 0.0f || maxP.y < 0.0f || minP.y >= constants.outputResolution.y)
			{
				continue;
			}

			ClampToScreenBounds(minP, maxP, constants.outputResolution);

			// small triangles between pixel centers
			// https://frostbite-wp-prd.s3.amazonaws.com/wp-content/uploads/2016/03/29204330/GDC_2016_Compute.pdf
			if (any(round(minP.xy) == round(maxP.xy)))
			{
				continue;
			}

			minP.xy = SnapMinBoundToPixelCenter(minP.xy);

			float2 dimensions = maxP.xy - minP.xy;

			// Hi-Z
			if (constants.perTriangleHiZCullingEnabled && constants.hasHiZHistory)
			{
				const float maximumDepth = maxP.z;
				const bool visible = TriangleVsHiZ(
					minP.xy, maxP.xy, maximumDepth, constants.inverseOutputResolution,
					previousDepth);

				if (!visible)
				{
					continue;
				}
			}

			// one more triangle was rendered
			// not precise, though, since it still could miss any pixel centers
			atomic_fetch_add_explicit(&statisticsSM[1], 1u, memory_order_relaxed);
			if (dimensions.x * dimensions.y >= constants.bigTriangleThreshold || quadrilateral)
			{
				BigTriangleOpaque result;
				result.p0WSX = p0WS.x;
				result.p0WSY = p0WS.y;
				result.p0WSZ = p0WS.z;
				result.p1WSX = p1WS.x;
				result.p1WSY = p1WS.y;
				result.p1WSZ = p1WS.z;
				result.p2WSX = p2WS.x;
				result.p2WSY = p2WS.y;
				result.p2WSZ = p2WS.z;
				result.packedNormal0 = n0P.packedNormal;
				result.packedNormal1 = n1P.packedNormal;
				result.packedNormal2 = n2P.packedNormal;
				result.packedColor0X = c0P.packedColor.x;
				result.packedColor0Y = c0P.packedColor.y;
				result.packedColor1X = c1P.packedColor.x;
				result.packedColor1Y = c1P.packedColor.y;
				result.packedColor2X = c2P.packedColor.x;
				result.packedColor2Y = c2P.packedColor.y;
				// TODO: add this
				result.packedUV0 = 0;
				result.packedUV1 = 0;
				result.packedUV2 = 0;

				float2 tilesCount = ceil(dimensions / constants.bigTriangleTileSize);
				uint firstHalfTiles = uint(tilesCount.x * tilesCount.y);
				uint secondHalfTiles = 0;

				if (quadrilateral)
				{
					// screen-space coordinate of the fourth clipped vertex
					p3Helper.xy = (p3Helper.xy / p3Helper.w * float2(0.5f, -0.5f) + float2(0.5f, 0.5f)) * constants.outputResolution;

					if (p0Behind)
					{
						minP.xy = min(p3Helper.xy, min(p0SS, p1SS));
						maxP.xy = max(p3Helper.xy, max(p0SS, p1SS));
					}
					else if (p1Behind)
					{
						minP.xy = min(p3Helper.xy, min(p1SS, p2SS));
						maxP.xy = max(p3Helper.xy, max(p1SS, p2SS));
					}
					else
					{
						minP.xy = min(p3Helper.xy, min(p0SS, p2SS));
						maxP.xy = max(p3Helper.xy, max(p0SS, p2SS));
					}

					ClampToScreenBounds(minP, maxP, constants.outputResolution);
					minP.xy = SnapMinBoundToPixelCenter(minP.xy);
					dimensions = maxP.xy - minP.xy;

					tilesCount = ceil(dimensions / constants.bigTriangleTileSize);
					secondHalfTiles = uint(tilesCount.x * tilesCount.y);
				}

				uint totalTiles = firstHalfTiles + secondHalfTiles;
				if (totalTiles > 0)
				{
					uint writeIndex = atomic_fetch_add_explicit(&arguments.x, totalTiles, memory_order_relaxed);

					// seemingly vastly inefficient way to write out that data,
					// but the more reasonable/parallel approach isn't faster, and is in fact slower
					// see the same code in the "experimental" branch
					for (uint offset = 0; offset < firstHalfTiles; offset++)
					{
						result.tileOffset = float(offset);
						bigTriangles[writeIndex + offset] = result;
					}

					for (uint secondOffset = 0; secondOffset < secondHalfTiles; secondOffset++)
					{
						result.tileOffset = as_type<float>(as_type<uint>(float(secondOffset)) | 0x80000000);
						bigTriangles[writeIndex + firstHalfTiles + secondOffset] = result;
					}
				}

				continue;
			}

			float3 n0 = UnpackNormal(n0P);
			float3 n1 = UnpackNormal(n1P);
			float3 n2 = UnpackNormal(n2P);
			float4 c0 = UnpackColor(c0P);
			float4 c1 = UnpackColor(c1P);
			float4 c2 = UnpackColor(c2P);

			float area = Area(p0SS.xy, p1SS.xy, p2SS.xy);

			// skip zero-area triangles produced by clipping or screen-space rounding before dividing by area
			if (area == 0.0f)
			{
				continue;
			}

			float invArea = 1.0f / area;

			// https://www.cs.drexel.edu/~david/Classes/Papers/comp175-06-pineda.pdf
			float2 dxdy0;
			float area0;
			EdgeFunction(p1SS.xy, p2SS.xy, minP.xy, area0, dxdy0);
			float2 dxdy1;
			float area1;
			EdgeFunction(p2SS.xy, p0SS.xy, minP.xy, area1, dxdy1);
			float2 dxdy2;
			float area2;
			EdgeFunction(p0SS.xy, p1SS.xy, minP.xy, area2, dxdy2);

			if (constants.scanlineRasterization)
			{
				for (float y = minP.y; y <= maxP.y; y += 1.0f)
				{
					float t0 = EdgeScanlineIntersection(p1SS.xy, p2SS.xy, y);
					float t1 = EdgeScanlineIntersection(p2SS.xy, p0SS.xy, y);
					float t2 = EdgeScanlineIntersection(p0SS.xy, p1SS.xy, y);

					bool t0Test = (0.0f <= t0 && t0 <= 1.0f);
					bool t1Test = (0.0f <= t1 && t1 <= 1.0f);
					bool t2Test = (0.0f <= t2 && t2 <= 1.0f);

					// no intersection with a scanline
					if ((!t0Test && !t1Test) || (!t1Test && !t2Test) || (!t2Test && !t0Test))
					{
						continue;
					}

					float x0 = mix(p1SS.x, p2SS.x, t0);
					float x1 = mix(p2SS.x, p0SS.x, t1);
					float x2 = mix(p0SS.x, p1SS.x, t2);

					// filtering out redundant intersection
					float candidate0 = t0Test ? x0 : mix(x1, x2, 0.5f);
					float candidate1 = t1Test ? x1 : mix(x2, x0, 0.5f);
					float candidate2 = t2Test ? x2 : mix(x0, x1, 0.5f);

					float xMin = min(candidate0, min(candidate1, candidate2));
					float xMax = max(candidate0, max(candidate1, candidate2));

					ClampScanline(minP.x, maxP.x, xMin, xMax);

					float area0tmp = area0 - dxdy0.y * (xMin - minP.x);
					float area1tmp = area1 - dxdy1.y * (xMin - minP.x);
					float area2tmp = area2 - dxdy2.y * (xMin - minP.x);

					for (float x = xMin; x <= xMax; x += 1.0f)
					{
						// convert to barycentric weights
						float weight0 = area0tmp * invArea;
						float weight1 = area1tmp * invArea;
						float weight2 = area2tmp * invArea;

						float depth = weight0 * z0NDC + weight1 * z1NDC + weight2 * z2NDC;

						uint2 pixelCoord = uint2(x, y);
						if (constants.showOverdraw)
						{
							atomic_fetch_add_explicit(&fragmentOverdraw[pixelCoord.y * uint(constants.outputResolution.x) + pixelCoord.x], 1u, memory_order_relaxed);
						}
						// early z test
						else if (as_type<float>(depthBuffer[pixelCoord.y * uint(constants.outputResolution.x) + pixelCoord.x]) == depth)
						{
							// for perspective-correct interpolation
							float denom = 1.0f / (weight0 * invW0 + weight1 * invW1 + weight2 * invW2);

							float3 N = denom * (weight0 * n0 * invW0 + weight1 * n1 * invW1 + weight2 * n2 * invW2);
							N = normalize(N);

							float3 color = denom * (weight0 * c0.rgb * invW0 + weight1 * c1.rgb * invW1 + weight2 * c2.rgb * invW2);
							if (constants.showMeshlets)
							{
								color = float3(instance.color);
							}

							float3 positionWS = denom * (weight0 * p0WS * invW0 + weight1 * p1WS * invW1 + weight2 * p2WS * invW2);

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

						// E(x + a, y + b) = E(x, y) - a * dy + b * dx
						area0tmp -= dxdy0.y;
						area1tmp -= dxdy1.y;
						area2tmp -= dxdy2.y;
					}

					area0 += dxdy0.x;
					area1 += dxdy1.x;
					area2 += dxdy2.x;
				}
			}
			else
			{
				//  --->----
				// |
				//  --->----
				// |
				//  --->----
				// etc.
				for (float y = minP.y; y <= maxP.y; y += 1.0f)
				{
					float area0tmp = area0;
					float area1tmp = area1;
					float area2tmp = area2;
					for (float x = minP.x; x <= maxP.x; x += 1.0f)
					{
						// edge tests, "frustum culling" for 3 lines in 2D
						bool insideTriangle = true;
						insideTriangle = insideTriangle && (EdgeIsTopLeft(p1SS.xy, p2SS.xy) ? (area0tmp >= 0.0f) : (area0tmp > 0.0f));
						insideTriangle = insideTriangle && (EdgeIsTopLeft(p2SS.xy, p0SS.xy) ? (area1tmp >= 0.0f) : (area1tmp > 0.0f));
						insideTriangle = insideTriangle && (EdgeIsTopLeft(p0SS.xy, p1SS.xy) ? (area2tmp >= 0.0f) : (area2tmp > 0.0f));
						if (insideTriangle)
						{
							// convert to barycentric weights
							float weight0 = area0tmp * invArea;
							float weight1 = area1tmp * invArea;
							float weight2 = area2tmp * invArea;

							float depth = weight0 * z0NDC + weight1 * z1NDC + weight2 * z2NDC;

							uint2 pixelCoord = uint2(x, y);
							if (constants.showOverdraw)
							{
								atomic_fetch_add_explicit(&fragmentOverdraw[pixelCoord.y * uint(constants.outputResolution.x) + pixelCoord.x], 1u, memory_order_relaxed);
							}
							// early z test
							else if (as_type<float>(depthBuffer[pixelCoord.y * uint(constants.outputResolution.x) + pixelCoord.x]) == depth)
							{
								// for perspective-correct interpolation
								float denom = 1.0f / (weight0 * invW0 + weight1 * invW1 + weight2 * invW2);

								float3 N = denom * (weight0 * n0 * invW0 + weight1 * n1 * invW1 + weight2 * n2 * invW2);
								N = normalize(N);

								float3 color = denom * (weight0 * c0.rgb * invW0 + weight1 * c1.rgb * invW1 + weight2 * c2.rgb * invW2);
								if (constants.showMeshlets)
								{
									color = float3(instance.color);
								}

								float3 positionWS = denom * (weight0 * p0WS * invW0 + weight1 * p1WS * invW1 + weight2 * p2WS * invW2);

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

						// E(x + a, y + b) = E(x, y) - a * dy + b * dx
						area0tmp -= dxdy0.y;
						area1tmp -= dxdy1.y;
						area2tmp -= dxdy2.y;
					}

					area0 += dxdy0.x;
					area1 += dxdy1.x;
					area2 += dxdy2.x;
				}
			}
		}
	}

	threadgroup_barrier(mem_flags::mem_threadgroup);

	if (groupIndex == 0)
	{
		atomic_fetch_add_explicit(
			&statistics[0],
			atomic_load_explicit(&statisticsSM[0], memory_order_relaxed),
			memory_order_relaxed);
		atomic_fetch_add_explicit(
			&statistics[1],
			atomic_load_explicit(&statisticsSM[1], memory_order_relaxed),
			memory_order_relaxed);
	}

	(void)texcoords;
}
