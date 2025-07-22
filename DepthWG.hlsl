#include "TypesAndConstants.hlsli"

StructuredBuffer<uint3> DispatchArgument : register(t0);

struct RasterizationDispatch
{
	uint3 dispatchGrid : SV_DispatchGrid;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void RasterizationDispatchNode(
	[MaxRecords(1)] NodeOutput<RasterizationDispatch> TriangleRasterizationNode)
{
	GroupNodeOutputRecords<RasterizationDispatch> output =
		TriangleRasterizationNode.GetGroupNodeOutputRecords(1);

	output[0].dispatchGrid = uint3(
		DispatchArgument[0].x,
		SWR_WG_THREAD_GROUPS_Y,
		1);

	output.OutputComplete();
}

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

StructuredBuffer<VertexPosition> Positions : register(t10);

StructuredBuffer<IndirectCommand> Commands : register(t20);
StructuredBuffer<uint> Indices : register(t21);
StructuredBuffer<Instance> Instances : register(t22);

RWTexture2D<uint> Depth : register(u0);
AppendStructuredBuffer<BigTriangleDepth> BigTriangles : register(u1);
RWStructuredBuffer<uint> Statistics : register(u2);

groupshared IndirectCommand Command;
groupshared uint2 StatisticsSM;

#include "Common.hlsli"
#include "Rasterization.hlsli"

[Shader("node")]
[NodeLaunch("broadcasting")]
// TODO: fix it somehow?
[NodeMaxDispatchGrid(10000, 1, 1)]
[numthreads(SWR_WG_TRIANGLE_THREADS_X, SWR_WG_TRIANGLE_THREADS_Y, SWR_WG_TRIANGLE_THREADS_Z)]
void TriangleRasterizationNode(
	DispatchNodeInputRecord<RasterizationDispatch> input,
	uint3 groupID : SV_GroupID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0)
	{
		Command = Commands[groupID.x];
		StatisticsSM = uint2(0, 0);
	}

	Barrier(GROUP_SHARED_MEMORY, GROUP_SCOPE | GROUP_SYNC);

	//[unroll(WG_TRIANGLES_PER_THREAD)]
	//for (uint meshletChunkIndex = 0; meshletChunkIndex < WG_TRIANGLES_PER_THREAD; meshletChunkIndex++)
	//{
		[branch]
		if ((groupThreadID.x + groupID.y * SWR_WG_TRIANGLE_THREADS_X) * 3 < Command.args.indexCountPerInstance)
		{
			uint i0, i1, i2;
			GetTriangleIndices(
				Command.args.startIndexLocation + (groupThreadID.x + groupID.y * SWR_WG_TRIANGLE_THREADS_X) * 3,
				i0, i1, i2);

			float3 p0, p1, p2;
			GetTriangleVertexPositions(i0, i1, i2, Command.args.baseVertexLocation, p0, p1, p2);

			for (uint instanceID = 0; instanceID < Command.args.instanceCount; instanceID++)
			{
				// one more triangle attempted to be rendered
				InterlockedAdd(StatisticsSM[0], 1);

				float3 p0WS, p1WS, p2WS;
				float4 p0CS, p1CS, p2CS;
				Instance instance = Instances[Command.startInstanceLocation + instanceID];
				GetCSPositions(instance, p0, p1, p2, p0WS, p1WS, p2WS, p0CS, p1CS, p2CS);

				// crude "clipping" of polygons behind the camera
				// w in CS is a view space z
				// TODO: implement proper near plane clipping
				[branch]
				if (p0CS.w <= 0.0 || p1CS.w <= 0.0 || p2CS.w <= 0.0)
				{
					continue;
				}

				// 1 / z for each vertex (z in VS)
				float invW0 = 1.0 / p0CS.w;
				float invW1 = 1.0 / p1CS.w;
				float invW2 = 1.0 / p2CS.w;

				float2 p0SS, p1SS, p2SS;
				GetSSPositions(p0CS.xy, p1CS.xy, p2CS.xy, invW0, invW1, invW2, p0SS, p1SS, p2SS);

				float area = Area(p0SS.xy, p1SS.xy, p2SS.xy);

				// backface if negative
				[branch]
				if (area <= 0.0)
				{
					continue;
				}

				float z0NDC = p0CS.z * invW0;
				float z1NDC = p1CS.z * invW1;
				float z2NDC = p2CS.z * invW2;

				float3 minP = min(min(float3(p0SS.xy, z0NDC), float3(p1SS.xy, z1NDC)), float3(p2SS.xy, z2NDC));
				float3 maxP = max(max(float3(p0SS.xy, z0NDC), float3(p1SS.xy, z1NDC)), float3(p2SS.xy, z2NDC));

				// frustum culling
				[branch]
				if (minP.x >= OutputRes.x || maxP.x < 0.0 || maxP.y < 0.0 || minP.y >= OutputRes.y)
				{
					continue;
				}

				ClampToScreenBounds(minP.xy, maxP.xy);

				// small triangles between pixel centers
				// https://frostbite-wp-prd.s3.amazonaws.com/wp-content/uploads/2016/03/29204330/GDC_2016_Compute.pdf
				[branch]
				if (any(round(minP.xy) == round(maxP.xy)))
				{
					continue;
				}

				minP.xy = SnapMinBoundToPixelCenter(minP.xy);

				float2 dimensions = maxP.xy - minP.xy;

				// Hi-Z
				//float mipLevel = ceil(log2(0.5 * max(dimensions.x, dimensions.y)));
				//float tileDepth = HiZ.SampleLevel(
				//	DepthSampler,
				//	(minP.xy + maxP.xy) * 0.5 * InvOutputRes,
				//	mipLevel).r;
				//[branch]
				//if (tileDepth > maxP.z)
				//{
				//	continue;
				//}

				// one more triangle was rendered
				// not precise, though, since it still could miss any pixel centers
				InterlockedAdd(StatisticsSM[1], 1);

				// TODO: thin triangles area vs box area
				// TODO: thread local

				[branch]
				if (dimensions.x * dimensions.y >= BigTriangleThreshold)
				{
					BigTriangleDepth result;
					result.p0WSX = p0WS.x;
					result.p0WSY = p0WS.y;
					result.p0WSZ = p0WS.z;
					result.p1WSX = p1WS.x;
					result.p1WSY = p1WS.y;
					result.p1WSZ = p1WS.z;
					result.p2WSX = p2WS.x;
					result.p2WSY = p2WS.y;
					result.p2WSZ = p2WS.z;

					float2 tilesCount = ceil(dimensions / BigTriangleTileSize);
					float totalTiles = tilesCount.x * tilesCount.y;
					for (float offset = 0.0; offset < totalTiles; offset += 1.0)
					{
						result.tileOffset = offset;

						BigTriangles.Append(result);
					}

					continue;
				}

				float invArea = 1.0 / area;

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

				if (ScanlineRasterization)
				{
					for (float y = minP.y; y <= maxP.y; y += 1.0)
					{
						float t0 = EdgeScanlineIntersection(p1SS.xy, p2SS.xy, y);
						float t1 = EdgeScanlineIntersection(p2SS.xy, p0SS.xy, y);
						float t2 = EdgeScanlineIntersection(p0SS.xy, p1SS.xy, y);

						bool t0Test = (0.0 <= t0 && t0 <= 1.0);
						bool t1Test = (0.0 <= t1 && t1 <= 1.0);
						bool t2Test = (0.0 <= t2 && t2 <= 1.0);

						// no intersection with a scanline
						if ((!t0Test && !t1Test) || (!t1Test && !t2Test) || (!t2Test && !t0Test))
						{
							continue;
						}

						float x0 = lerp(p1SS.x, p2SS.x, t0);
						float x1 = lerp(p2SS.x, p0SS.x, t1);
						float x2 = lerp(p0SS.x, p1SS.x, t2);

						// filtering out redundant intersection
						float candidate0 = t0Test ? x0 : lerp(x1, x2, 0.5);
						float candidate1 = t1Test ? x1 : lerp(x2, x0, 0.5);
						float candidate2 = t2Test ? x2 : lerp(x0, x1, 0.5);

						float xMin = min(candidate0, min(candidate1, candidate2));
						float xMax = max(candidate0, max(candidate1, candidate2));

						// snap min x bound to pixel center
						xMin = ceil(xMin - 0.5) + 0.5;

						// top-left rule
						if (UseTopLeftRule)
						{
							xMax += ((frac(xMax) == 0.5) ? -1.0 : 0.0);
						}

						float area0tmp = area0 - dxdy0.y * (xMin - minP.x);
						float area1tmp = area1 - dxdy1.y * (xMin - minP.x);

						for (float x = xMin; x <= xMax; x += 1.0)
						{
							// convert to barycentric weights
							float weight0 = area0tmp * invArea;
							float weight1 = area1tmp * invArea;
							float weight2 = 1.0 - weight0 - weight1;

							precise float depth = weight0 * z0NDC + weight1 * z1NDC + weight2 * z2NDC;

							// TODO: account for non-reversed Z
							InterlockedMax(Depth[uint2(x, y)], asuint(depth));

							// E(x + a, y + b) = E(x, y) - a * dy + b * dx
							area0tmp -= dxdy0.y;
							area1tmp -= dxdy1.y;
						}

						area0 += dxdy0.x;
						area1 += dxdy1.x;
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
					for (float y = minP.y; y <= maxP.y; y += 1.0)
					{
						float area0tmp = area0;
						float area1tmp = area1;
						float area2tmp = area2;
						for (float x = minP.x; x <= maxP.x; x += 1.0)
						{
							// edge tests, "frustum culling" for 3 lines in 2D
							bool insideTriangle = true;
							if (UseTopLeftRule)
							{
								insideTriangle = insideTriangle && (EdgeIsTopLeft(p1SS.xy, p2SS.xy) ? (area0tmp >= 0.0) : (area0tmp > 0.0));
								insideTriangle = insideTriangle && (EdgeIsTopLeft(p2SS.xy, p0SS.xy) ? (area1tmp >= 0.0) : (area1tmp > 0.0));
								insideTriangle = insideTriangle && (EdgeIsTopLeft(p0SS.xy, p1SS.xy) ? (area2tmp >= 0.0) : (area2tmp > 0.0));
							}
							else
							{
								insideTriangle = area0tmp >= 0.0 && area1tmp >= 0.0 && area2tmp >= 0.0;
							}

							[branch]
							if (insideTriangle)
							{
								// convert to barycentric weights
								float weight0 = area0tmp * invArea;
								float weight1 = area1tmp * invArea;
								float weight2 = 1.0 - weight0 - weight1;

								precise float depth = weight0 * z0NDC + weight1 * z1NDC + weight2 * z2NDC;

								// TODO: account for non-reversed Z
								InterlockedMax(Depth[uint2(x, y)], asuint(depth));
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
	//}

	// WaveActiveSum() + WaveIsFirstLane() approach, instead of grouphared atomics, was slower
	Barrier(GROUP_SHARED_MEMORY, GROUP_SCOPE | GROUP_SYNC);

	if (groupIndex == 0)
	{
		InterlockedAdd(Statistics[0], StatisticsSM[0]);
		InterlockedAdd(Statistics[1], StatisticsSM[1]);
	}
}
