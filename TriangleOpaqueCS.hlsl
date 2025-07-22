// Implicit assumptions:
// - we are dealing with triangles
// - indices represent a triangle list

#include "TypesAndConstants.hlsli"

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
SamplerState DepthSampler : register(s1);

StructuredBuffer<VertexPosition> Positions : register(t0);
StructuredBuffer<VertexNormal> Normals : register(t1);
StructuredBuffer<VertexColor> Colors : register(t2);
StructuredBuffer<VertexUV> UVs : register(t3);

StructuredBuffer<uint> Indices : register(t8);
StructuredBuffer<Instance> Instances : register(t9);
Texture2D Depth : register(t10);
Texture2DArray ShadowMap : register(t11);
StructuredBuffer<IndirectCommand> Commands : register(t12);

RWTexture2D<float4> RenderTarget : register(u0);
AppendStructuredBuffer<BigTriangleOpaque> BigTriangles : register(u1);
RWStructuredBuffer<uint> Statistics : register(u2);

groupshared IndirectCommand Command;
groupshared uint2 StatisticsSM;

#include "Common.hlsli"
#include "Rasterization.hlsli"

[numthreads(SWR_TRIANGLE_THREADS_X, SWR_TRIANGLE_THREADS_Y, SWR_TRIANGLE_THREADS_Z)]
void main(
	uint3 groupID : SV_GroupID,
	uint3 dispatchThreadID : SV_DispatchThreadID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0)
	{
		Command = Commands[groupID.x];
		StatisticsSM = uint2(0, 0);
	}

	GroupMemoryBarrierWithGroupSync();

	//[unroll(TRIANGLES_PER_THREAD)]
	//for (uint meshletChunkIndex = 0; meshletChunkIndex < TRIANGLES_PER_THREAD; meshletChunkIndex++)
	//{
		[branch]
		if ((groupThreadID.x + groupID.y * SWR_TRIANGLE_THREADS_X) * 3 < Command.args.indexCountPerInstance)
		{
			uint i0, i1, i2;
			GetTriangleIndices(
				Command.args.startIndexLocation + (groupThreadID.x + groupID.y * SWR_TRIANGLE_THREADS_X) * 3,
				i0, i1, i2);

			float3 p0, p1, p2;
			GetTriangleVertexPositions(i0, i1, i2, Command.args.baseVertexLocation, p0, p1, p2);

			VertexNormal n0P, n1P, n2P;
			GetPackedVertexNormals(i0, i1, i2, Command.args.baseVertexLocation, n0P, n1P, n2P);
			VertexColor c0P, c1P, c2P;
			GetPackedVertexColors(i0, i1, i2, Command.args.baseVertexLocation, c0P, c1P, c2P);

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
				//float tileDepth = Depth.SampleLevel(
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

				[branch]
				if (dimensions.x * dimensions.y >= BigTriangleThreshold)
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

					float2 tilesCount = ceil(dimensions / BigTriangleTileSize);
					float totalTiles = tilesCount.x * tilesCount.y;
					for (float offset = 0.0; offset < totalTiles; offset += 1.0)
					{
						result.tileOffset = offset;

						BigTriangles.Append(result);
					}

					continue;
				}

				float3 n0 = UnpackNormal(n0P);
				float3 n1 = UnpackNormal(n1P);
				float3 n2 = UnpackNormal(n2P);
				float4 c0 = UnpackColor(c0P);
				float4 c1 = UnpackColor(c1P);
				float4 c2 = UnpackColor(c2P);

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

							// early z test
							[branch]
							if (Depth[uint2(x, y)].r == depth)
							{
								// for perspective-correct interpolation
								float denom = 1.0 / (weight0 * invW0 + weight1 * invW1 + weight2 * invW2);

								float3 N = denom * (weight0 * n0 * invW0 + weight1 * n1 * invW1 + weight2 * n2 * invW2);
								N = normalize(N);

								float3 color = denom * (weight0 * c0.rgb * invW0 + weight1 * c1.rgb * invW1 + weight2 * c2.rgb * invW2);
								if (ShowMeshlets)
								{
									color = instance.color;
								}

								float3 positionWS = denom * (weight0 * p0WS * invW0 + weight1 * p1WS * invW1 + weight2 * p2WS * invW2);

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

								uint2 pixelCoord = uint2(x, y);

								// early z test
								[branch]
								if (Depth[pixelCoord].r == depth)
								{
									// for perspective-correct interpolation
									float denom = 1.0 / (weight0 * invW0 + weight1 * invW1 + weight2 * invW2);

									float3 N = denom * (weight0 * n0 * invW0 + weight1 * n1 * invW1 + weight2 * n2 * invW2);
									N = normalize(N);

									float3 color = denom * (weight0 * c0.rgb * invW0 + weight1 * c1.rgb * invW1 + weight2 * c2.rgb * invW2);
									if (ShowMeshlets)
									{
										color = instance.color;
									}

									float3 positionWS = denom * (weight0 * p0WS * invW0 + weight1 * p1WS * invW1 + weight2 * p2WS * invW2);

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

									RenderTarget[pixelCoord] = float4(result, 1.0);
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
	//}

	// WaveActiveSum() + WaveIsFirstLane() approach, instead of grouphared atomics, was slower
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0)
	{
		InterlockedAdd(Statistics[0], StatisticsSM[0]);
		InterlockedAdd(Statistics[1], StatisticsSM[1]);
	}
}