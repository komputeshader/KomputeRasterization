// Implicit assumptions:
// - we are dealing with triangles
// - indices represent a triangle list

#include "Common.metal"
#include "Rasterization.metal"

inline void ShadeOpaquePixel(
	uint2 pixel,
	float weight0,
	float weight1,
	float z0NDC,
	float z1NDC,
	float z2NDC,
	float invW0,
	float invW1,
	float invW2,
	float3 p0WS,
	float3 p1WS,
	float3 p2WS,
	float3 n0,
	float3 n1,
	float3 n2,
	float3 c0,
	float3 c1,
	float3 c2,
	constant SceneCB& constants,
	device const uint* depth,
	device const uint* shadowMap,
	texture2d<float, access::write> output)
{
	if (depth[pixel.y * uint(constants.outputResolution.x) + pixel.x] !=
		GetDepthBits(weight0, weight1, z0NDC, z1NDC, z2NDC))
	{
		return;
	}

	const float weight2 = 1.0f - weight0 - weight1;
	const float viewDepth = 1.0f /
		(weight0 * invW0 + weight1 * invW1 + weight2 * invW2);
	const float3 weights = float3(weight0, weight1, weight2) *
		float3(invW0, invW1, invW2) * viewDepth;

	output.write(
		float4(
			ShadePixel(
				n0 * weights.x + n1 * weights.y + n2 * weights.z,
				c0 * weights.x + c1 * weights.y + c2 * weights.z,
				p0WS * weights.x + p1WS * weights.y + p2WS * weights.z,
				viewDepth,
				constants,
				shadowMap),
			1.0f),
		pixel);
}

inline void RasterizeOpaque(
	float2 p0SS,
	float2 p1SS,
	float2 p2SS,
	float4 p0CS,
	float4 p1CS,
	float4 p2CS,
	float3 p0WS,
	float3 p1WS,
	float3 p2WS,
	float3 n0,
	float3 n1,
	float3 n2,
	float3 c0,
	float3 c1,
	float3 c2,
	float area,
	float2 minP,
	float2 maxP,
	constant SceneCB& constants,
	device const uint* depth,
	device const uint* shadowMap,
	texture2d<float, access::write> output)
{
	const float invW0 = 1.0f / p0CS.w;
	const float invW1 = 1.0f / p1CS.w;
	const float invW2 = 1.0f / p2CS.w;

	const float z0NDC = p0CS.z * invW0;
	const float z1NDC = p1CS.z * invW1;
	const float z2NDC = p2CS.z * invW2;

	const float invArea = 1.0f / area;

	float2 dxdy0;
	float area0;
	EdgeFunction(p1SS, p2SS, minP, area0, dxdy0);
	float2 dxdy1;
	float area1;
	EdgeFunction(p2SS, p0SS, minP, area1, dxdy1);
	float2 dxdy2;
	float area2;
	EdgeFunction(p0SS, p1SS, minP, area2, dxdy2);

	if (constants.scanlineRasterization)
	{
		for (float y = minP.y; y <= maxP.y; y += 1.0f)
		{
			const float t0 = EdgeScanlineIntersection(p1SS, p2SS, y);
			const float t1 = EdgeScanlineIntersection(p2SS, p0SS, y);
			const float t2 = EdgeScanlineIntersection(p0SS, p1SS, y);

			const bool t0Test = 0.0f <= t0 && t0 <= 1.0f;
			const bool t1Test = 0.0f <= t1 && t1 <= 1.0f;
			const bool t2Test = 0.0f <= t2 && t2 <= 1.0f;

			if ((!t0Test && !t1Test) || (!t1Test && !t2Test) || (!t2Test && !t0Test))
			{
				continue;
			}

			const float x0 = mix(p1SS.x, p2SS.x, t0);
			const float x1 = mix(p2SS.x, p0SS.x, t1);
			const float x2 = mix(p0SS.x, p1SS.x, t2);

			const float candidate0 = t0Test ? x0 : mix(x1, x2, 0.5f);
			const float candidate1 = t1Test ? x1 : mix(x2, x0, 0.5f);
			const float candidate2 = t2Test ? x2 : mix(x0, x1, 0.5f);

			float xMin = min(candidate0, min(candidate1, candidate2));
			float xMax = max(candidate0, max(candidate1, candidate2));

			xMin = ceil(xMin - 0.5f) + 0.5f;

			if (constants.useTopLeftRule)
			{
				xMax += fract(xMax) == 0.5f ? -1.0f : 0.0f;
			}

			float area0Temporary = area0 - dxdy0.y * (xMin - minP.x);
			float area1Temporary = area1 - dxdy1.y * (xMin - minP.x);

			for (float x = xMin; x <= xMax; x += 1.0f)
			{
				ShadeOpaquePixel(
					uint2(x, y),
					area0Temporary * invArea,
					area1Temporary * invArea,
					z0NDC,
					z1NDC,
					z2NDC,
					invW0,
					invW1,
					invW2,
					p0WS,
					p1WS,
					p2WS,
					n0,
					n1,
					n2,
					c0,
					c1,
					c2,
					constants,
					depth,
					shadowMap,
					output);

				area0Temporary -= dxdy0.y;
				area1Temporary -= dxdy1.y;
			}

			area0 += dxdy0.x;
			area1 += dxdy1.x;
		}
	}
	else
	{
		for (float y = minP.y; y <= maxP.y; y += 1.0f)
		{
			float area0Temporary = area0;
			float area1Temporary = area1;
			float area2Temporary = area2;

			for (float x = minP.x; x <= maxP.x; x += 1.0f)
			{
				if (IsInsideTriangle(
						area0Temporary,
						area1Temporary,
						area2Temporary,
						p0SS,
						p1SS,
						p2SS,
						constants.useTopLeftRule != 0))
				{
					ShadeOpaquePixel(
						uint2(x, y),
						area0Temporary * invArea,
						area1Temporary * invArea,
						z0NDC,
						z1NDC,
						z2NDC,
						invW0,
						invW1,
						invW2,
						p0WS,
						p1WS,
						p2WS,
						n0,
						n1,
						n2,
						c0,
						c1,
						c2,
						constants,
						depth,
						shadowMap,
						output);
				}

				area0Temporary -= dxdy0.y;
				area1Temporary -= dxdy1.y;
				area2Temporary -= dxdy2.y;
			}

			area0 += dxdy0.x;
			area1 += dxdy1.x;
			area2 += dxdy2.x;
		}
	}
}

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
	device const uint* depth [[buffer(6)]],
	device const uint* shadowMap [[buffer(8)]],
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
			indices,
			constants.totalTriangles,
			command.args.startIndexLocation + triangleIndex * 3,
			i0,
			i1,
			i2);

		i0 += command.args.baseVertexLocation;
		i1 += command.args.baseVertexLocation;
		i2 += command.args.baseVertexLocation;

		const float3 p0 = float3(positions[i0].position);
		const float3 p1 = float3(positions[i1].position);
		const float3 p2 = float3(positions[i2].position);

		const float3 n0 = UnpackNormal(normals[i0].packedNormal);
		const float3 n1 = UnpackNormal(normals[i1].packedNormal);
		const float3 n2 = UnpackNormal(normals[i2].packedNormal);

		const float3 baseColor0 = UnpackColor(colors[i0].packedColor).rgb;
		const float3 baseColor1 = UnpackColor(colors[i1].packedColor).rgb;
		const float3 baseColor2 = UnpackColor(colors[i2].packedColor).rgb;

		for (uint instanceID = 0; instanceID < command.args.instanceCount; instanceID++)
		{
			atomic_fetch_add_explicit(&statisticsSM[0], 1, memory_order_relaxed);

			const Instance instance = instances[command.startInstanceLocation + instanceID];

			float3 p0WS, p1WS, p2WS;
			float4 p0CS, p1CS, p2CS;
			float2 p0SS, p1SS, p2SS;
			float area;
			float2 minP, maxP;

			if (!SetupTriangle(
					p0,
					p1,
					p2,
					instance,
					constants.vp,
					constants.outputResolution,
					p0WS,
					p1WS,
					p2WS,
					p0CS,
					p1CS,
					p2CS,
					p0SS,
					p1SS,
					p2SS,
					area,
					minP,
					maxP))
			{
				continue;
			}

			const float z0NDC = p0CS.z * (1.0f / p0CS.w);
			const float z1NDC = p1CS.z * (1.0f / p1CS.w);
			const float z2NDC = p2CS.z * (1.0f / p2CS.w);

			if (constants.perTriangleHiZCullingEnabled && constants.hasHiZHistory &&
				!TriangleVsHiZ(
					minP,
					maxP,
					max(z0NDC, max(z1NDC, z2NDC)),
					constants.inverseOutputResolution,
					previousDepth))
			{
				continue;
			}

			atomic_fetch_add_explicit(&statisticsSM[1], 1, memory_order_relaxed);

			const float2 dimensions = maxP - minP;

			if (dimensions.x * dimensions.y >= constants.bigTriangleThreshold)
			{
				BigTriangleOpaque result;
				result.p0WS = packed_float3(p0WS);
				result.p1WS = packed_float3(p1WS);
				result.p2WS = packed_float3(p2WS);

				result.packedNormal0 = normals[i0].packedNormal;
				result.packedNormal1 = normals[i1].packedNormal;
				result.packedNormal2 = normals[i2].packedNormal;

				result.packedColor0 = colors[i0].packedColor;
				result.packedColor1 = colors[i1].packedColor;
				result.packedColor2 = colors[i2].packedColor;

				result.packedUV0 = 0;
				result.packedUV1 = 0;
				result.packedUV2 = 0;

				const float2 tilesCount = ceil(dimensions / constants.bigTriangleTileSize);
				const uint totalTiles = uint(tilesCount.x * tilesCount.y);
				bool enqueued = true;

				for (uint offset = 0; offset < totalTiles; offset++)
				{
					result.tileOffset = float(offset);

					enqueued &= EnqueueBigTriangle(
						result,
						bigTriangles,
						arguments,
						constants.maxBigTriangles);
				}

				if (enqueued)
				{
					continue;
				}
			}

			const float3 meshletColor = float3(instance.color);

			RasterizeOpaque(
				p0SS,
				p1SS,
				p2SS,
				p0CS,
				p1CS,
				p2CS,
				p0WS,
				p1WS,
				p2WS,
				n0,
				n1,
				n2,
				constants.showMeshlets ? meshletColor : baseColor0,
				constants.showMeshlets ? meshletColor : baseColor1,
				constants.showMeshlets ? meshletColor : baseColor2,
				area,
				minP,
				maxP,
				constants,
				depth,
				shadowMap,
				output);
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
