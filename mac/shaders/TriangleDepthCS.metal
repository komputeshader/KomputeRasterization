// Implicit assumptions:
// - we are dealing with triangles
// - indices represent a triangle list

#include "Rasterization.metal"

#ifdef SHADOWS
kernel void TriangleShadowCS(
#else
kernel void TriangleDepthCS(
#endif
	device const VertexPosition* positions [[buffer(0)]],
	device const uint* indices [[buffer(4)]],
	device const Instance* instances [[buffer(5)]],
	constant DepthSceneCB& constants [[buffer(7)]],
	device const IndirectCommand* commands [[buffer(9)]],
	device atomic_uint* statistics [[buffer(10)]],
	device BigTriangleDepth* bigTriangles [[buffer(11)]],
	device DispatchArguments& arguments [[buffer(12)]],
	device atomic_uint* depth [[buffer(6)]],
#ifdef SHADOWS
	texture2d_array<float> previousShadows [[texture(3)]],
#else
	texture2d<float> previousDepth [[texture(2)]],
#endif
	uint3 groupID [[threadgroup_position_in_grid]],
	uint3 groupThreadID [[thread_position_in_threadgroup]],
	uint groupIndex [[thread_index_in_threadgroup]])
{
#ifdef SHADOWS
	depth += (constants.frustumIndex - 1) *
		uint(constants.outputResolution.x) * uint(constants.outputResolution.y);
#endif

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

		const float3 p0 = float3(positions[command.args.baseVertexLocation + i0].position);
		const float3 p1 = float3(positions[command.args.baseVertexLocation + i1].position);
		const float3 p2 = float3(positions[command.args.baseVertexLocation + i2].position);

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

			if (constants.perTriangleHiZCullingEnabled && constants.hasHiZHistory)
			{
				const float maximumDepth = max(z0NDC, max(z1NDC, z2NDC));
				const bool visible = TriangleVsHiZ(
					minP,
					maxP,
					maximumDepth,
					constants.inverseOutputResolution,
#ifdef SHADOWS
					previousShadows,
					constants.frustumIndex - 1);
#else
					previousDepth);
#endif

				if (!visible)
				{
					continue;
				}
			}

			atomic_fetch_add_explicit(&statisticsSM[1], 1, memory_order_relaxed);

			const float2 dimensions = maxP - minP;

			if (dimensions.x * dimensions.y >= constants.bigTriangleThreshold)
			{
				BigTriangleDepth result;
				result.p0WS = packed_float3(p0WS);
				result.p1WS = packed_float3(p1WS);
				result.p2WS = packed_float3(p2WS);

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

			RasterizeDepth(
				p0SS,
				p1SS,
				p2SS,
				p0CS,
				p1CS,
				p2CS,
				area,
				minP,
				maxP,
				constants.scanlineRasterization != 0,
				uint2(constants.outputResolution),
				depth);
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
}
