#include "Rasterization.metal"

#ifdef SHADOWS
kernel void BigTriangleShadowCS(
#else
kernel void BigTriangleDepthCS(
#endif
	constant DepthSceneCB& constants [[buffer(7)]],
	device const BigTriangleDepth* bigTriangles [[buffer(11)]],
	device atomic_uint* depth [[buffer(6)]],
	uint3 groupID [[threadgroup_position_in_grid]],
	uint3 groupThreadID [[thread_position_in_threadgroup]],
	uint groupIndex [[thread_index_in_threadgroup]])
{
#ifdef SHADOWS
	depth += (constants.frustumIndex - 1) *
		uint(constants.outputResolution.x) * uint(constants.outputResolution.y);
#endif

	threadgroup float2 minP;
	threadgroup float2 maxP;
	threadgroup float2 p0SS;
	threadgroup float2 p1SS;
	threadgroup float2 p2SS;
	threadgroup float z0NDC;
	threadgroup float z1NDC;
	threadgroup float z2NDC;
	threadgroup float invArea;
	threadgroup float area0;
	threadgroup float area1;
	threadgroup float area2;
	threadgroup float2 dxdy0;
	threadgroup float2 dxdy1;
	threadgroup float2 dxdy2;

	if (groupIndex == 0)
	{
		const BigTriangleDepth triangle = bigTriangles[groupID.x];

		const float4 p0CS = constants.vp * float4(float3(triangle.p0WS), 1.0f);
		const float4 p1CS = constants.vp * float4(float3(triangle.p1WS), 1.0f);
		const float4 p2CS = constants.vp * float4(float3(triangle.p2WS), 1.0f);

		const float invW0 = 1.0f / p0CS.w;
		const float invW1 = 1.0f / p1CS.w;
		const float invW2 = 1.0f / p2CS.w;

		float2 p0SSTemporary;
		float2 p1SSTemporary;
		float2 p2SSTemporary;

		GetSSPositions(
			p0CS.xy, p1CS.xy, p2CS.xy, invW0, invW1, invW2,
			constants.outputResolution,
			p0SSTemporary, p1SSTemporary, p2SSTemporary);

		p0SS = p0SSTemporary;
		p1SS = p1SSTemporary;
		p2SS = p2SSTemporary;

		const float area = Area(p0SS, p1SS, p2SS);

		z0NDC = p0CS.z * invW0;
		z1NDC = p1CS.z * invW1;
		z2NDC = p2CS.z * invW2;

		minP = min(p0SS, min(p1SS, p2SS));
		maxP = max(p0SS, max(p1SS, p2SS));

		minP = clamp(minP, float2(0.0f), constants.outputResolution);
		maxP = clamp(maxP, float2(0.0f), constants.outputResolution);
		minP = ceil(minP - 0.5f) + 0.5f;
		const float2 dimensions = maxP - minP;
		const float2 tileCount = ceil(dimensions / constants.bigTriangleTileSize);
		const float yTileOffset = floor(triangle.tileOffset / tileCount.x);
		const float xTileOffset = triangle.tileOffset - yTileOffset * tileCount.x;
		minP += float2(xTileOffset, yTileOffset) * constants.bigTriangleTileSize;
		maxP = min(maxP, minP + constants.bigTriangleTileSize);

		invArea = 1.0f / area;

		float area0Temporary;
		float area1Temporary;
		float area2Temporary;
		float2 dxdy0Temporary;
		float2 dxdy1Temporary;
		float2 dxdy2Temporary;

		EdgeFunction(
			p1SS, p2SS, minP,
			area0Temporary, dxdy0Temporary);
		EdgeFunction(
			p2SS, p0SS, minP,
			area1Temporary, dxdy1Temporary);
		EdgeFunction(
			p0SS, p1SS, minP,
			area2Temporary, dxdy2Temporary);
		area0 = area0Temporary;
		area1 = area1Temporary;
		area2 = area2Temporary;

		dxdy0 = dxdy0Temporary;
		dxdy1 = dxdy1Temporary;
		dxdy2 = dxdy2Temporary;
	}

	threadgroup_barrier(mem_flags::mem_threadgroup);

	uint yTiles = 0;
	for (
		float y = minP.y + groupThreadID.y;
		y <= maxP.y;
		y += SWR_BIG_TRIANGLE_THREADS_Y, yTiles++)
	{
		const uint yOffset = groupThreadID.y + yTiles * SWR_BIG_TRIANGLE_THREADS_Y;

		uint xTiles = 0;
		for (
			float x = minP.x + groupThreadID.x;
			x <= maxP.x;
			x += SWR_BIG_TRIANGLE_THREADS_X, xTiles++)
		{
			const uint xOffset = groupThreadID.x + xTiles * SWR_BIG_TRIANGLE_THREADS_X;

			const float currentArea0 = area0 - xOffset * dxdy0.y + yOffset * dxdy0.x;
			const float currentArea1 = area1 - xOffset * dxdy1.y + yOffset * dxdy1.x;
			const float currentArea2 = area2 - xOffset * dxdy2.y + yOffset * dxdy2.x;

			if (IsInsideTriangle(
					currentArea0,
					currentArea1,
					currentArea2,
					p0SS,
					p1SS,
					p2SS))
			{
				const float weight0 = currentArea0 * invArea;
				const float weight1 = currentArea1 * invArea;

				WriteDepth(
					depth,
					uint2(x, y),
					uint2(constants.outputResolution),
					GetDepthBits(weight0, weight1, z0NDC, z1NDC, z2NDC));
			}
		}
	}
}
