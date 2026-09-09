#ifndef RASTERIZATION_METAL
#define RASTERIZATION_METAL

#include "CullingCommon.metal"

static inline bool EnqueueBigTriangle(
	BigTriangleDepth triangle,
	device BigTriangleDepth* triangles,
	device DispatchArguments& arguments,
	uint capacity)
{
	uint expected = atomic_load_explicit(&arguments.x, memory_order_relaxed);
	for (;;)
	{
		if (expected >= capacity)
		{
			atomic_fetch_add_explicit(&arguments.overflow, 1, memory_order_relaxed);
			return false;
		}

		if (atomic_compare_exchange_weak_explicit(
				&arguments.x,
				&expected,
				expected + 1,
				memory_order_relaxed,
				memory_order_relaxed))
		{
			triangles[expected] = triangle;
			return true;
		}
	}
}

static inline bool EnqueueBigTriangle(
	BigTriangleOpaque triangle,
	device BigTriangleOpaque* triangles,
	device DispatchArguments& arguments,
	uint capacity)
{
	uint expected = atomic_load_explicit(&arguments.x, memory_order_relaxed);
	for (;;)
	{
		if (expected >= capacity)
		{
			atomic_fetch_add_explicit(&arguments.overflow, 1, memory_order_relaxed);
			return false;
		}

		if (atomic_compare_exchange_weak_explicit(
				&arguments.x,
				&expected,
				expected + 1,
				memory_order_relaxed,
				memory_order_relaxed))
		{
			triangles[expected] = triangle;
			return true;
		}
	}
}

static inline float Area(float2 v0, float2 v1, float2 v2)
{
	const float2 e0 = v1 - v0;
	const float2 e1 = v2 - v0;
	return e0.x * e1.y - e1.x * e0.y;
}

static inline void EdgeFunction(
	float2 v0,
	float2 v1,
	float2 p,
	thread float& area,
	thread float2& dxdy)
{
	const float2 e0 = v1 - v0;
	const float2 e1 = p - v0;
	area = e0.x * e1.y - e1.x * e0.y;
	dxdy = e0;
}

static inline bool EdgeIsTopLeft(float2 v0, float2 v1)
{
	const float2 e = v1 - v0;
	const bool top = e.y == 0.0f && e.x > 0.0f;
	const bool left = e.y < 0.0f;
	return top || left;
}

static inline float EdgeScanlineIntersection(float2 v0, float2 v1, float y)
{
	const float denominator = v1.y - v0.y;
	return denominator == 0.0f ? FloatMax : (y - v0.y) / denominator;
}

static inline void GetTriangleIndices(
	device const uint* indices,
	uint totalTriangles,
	uint startIndexLocation,
	thread uint& i0,
	thread uint& i1,
	thread uint& i2)
{
	startIndexLocation /= INDICES_STRIDE;
	i0 = indices[startIndexLocation];
	i1 = indices[totalTriangles + startIndexLocation];
	i2 = indices[2 * totalTriangles + startIndexLocation];
}

static inline void GetCSPositions(
	Instance instance,
	float3 p0,
	float3 p1,
	float3 p2,
	float4x4 VP,
	thread float3& p0WS,
	thread float3& p1WS,
	thread float3& p2WS,
	thread float4& p0CS,
	thread float4& p1CS,
	thread float4& p2CS)
{
	p0WS = (instance.worldTransform * float4(p0, 1.0f)).xyz;
	p1WS = (instance.worldTransform * float4(p1, 1.0f)).xyz;
	p2WS = (instance.worldTransform * float4(p2, 1.0f)).xyz;

	p0CS = VP * float4(p0WS, 1.0f);
	p1CS = VP * float4(p1WS, 1.0f);
	p2CS = VP * float4(p2WS, 1.0f);
}

static inline void GetSSPositions(
	float2 p0CS,
	float2 p1CS,
	float2 p2CS,
	float invW0,
	float invW1,
	float invW2,
	float2 outputResolution,
	thread float2& p0SS,
	thread float2& p1SS,
	thread float2& p2SS)
{
	p0SS = (p0CS * invW0 * float2(0.5f, -0.5f) + 0.5f) * outputResolution;
	p1SS = (p1CS * invW1 * float2(0.5f, -0.5f) + 0.5f) * outputResolution;
	p2SS = (p2CS * invW2 * float2(0.5f, -0.5f) + 0.5f) * outputResolution;
}

static inline bool SetupTriangle(
	float3 p0,
	float3 p1,
	float3 p2,
	Instance instance,
	float4x4 VP,
	float2 outputResolution,
	thread float3& p0WS,
	thread float3& p1WS,
	thread float3& p2WS,
	thread float4& p0CS,
	thread float4& p1CS,
	thread float4& p2CS,
	thread float2& p0SS,
	thread float2& p1SS,
	thread float2& p2SS,
	thread float& area,
	thread float2& minP,
	thread float2& maxP)
{
	GetCSPositions(instance, p0, p1, p2, VP, p0WS, p1WS, p2WS, p0CS, p1CS, p2CS);

	// https://userpages.cs.umbc.edu/olano/papers/2dh-tri/ (section 5.2)
	// backface culling before division by w
	// reverse the cross product because screen y points down
	if (dot(p0CS.xyw, cross(p2CS.xyw, p1CS.xyw)) <= 0.0f)
	{
		return false;
	}

	if (p0CS.w <= 0.0f || p1CS.w <= 0.0f || p2CS.w <= 0.0f)
	{
		return false;
	}

	const float invW0 = 1.0f / p0CS.w;
	const float invW1 = 1.0f / p1CS.w;
	const float invW2 = 1.0f / p2CS.w;

	GetSSPositions(
		p0CS.xy,
		p1CS.xy,
		p2CS.xy,
		invW0,
		invW1,
		invW2,
		outputResolution,
		p0SS,
		p1SS,
		p2SS);

	area = Area(p0SS, p1SS, p2SS);
	// skip zero-area triangles produced by screen-space rounding before dividing by area
	if (area == 0.0f)
	{
		return false;
	}

	minP = min(p0SS, min(p1SS, p2SS));
	maxP = max(p0SS, max(p1SS, p2SS));

	if (minP.x >= outputResolution.x || maxP.x < 0.0f || maxP.y < 0.0f || minP.y >= outputResolution.y)
	{
		return false;
	}

	minP = clamp(minP, float2(0.0f), outputResolution);
	maxP = clamp(maxP, float2(0.0f), outputResolution);

	if (any(round(minP) == round(maxP)))
	{
		return false;
	}

	minP = ceil(minP - 0.5f) + 0.5f;

	return true;
}

static inline bool IsInsideTriangle(
	float area0,
	float area1,
	float area2,
	float2 p0,
	float2 p1,
	float2 p2)
{
	return (EdgeIsTopLeft(p1, p2) ? area0 >= 0.0f : area0 > 0.0f) &&
		(EdgeIsTopLeft(p2, p0) ? area1 >= 0.0f : area1 > 0.0f) &&
		(EdgeIsTopLeft(p0, p1) ? area2 >= 0.0f : area2 > 0.0f);
}

static inline uint GetDepthBits(float weight0, float weight1, float z0, float z1, float z2)
{
	const float weight2 = 1.0f - weight0 - weight1;
	const float depth = weight0 * z0 + weight1 * z1 + weight2 * z2;

	return as_type<uint>(depth);
}

static inline void WriteDepth(
	device atomic_uint* depthBuffer,
	uint2 pixel,
	uint2 outputResolution,
	uint depth)
{
	const uint index = pixel.y * outputResolution.x + pixel.x;

	atomic_fetch_max_explicit(&depthBuffer[index], depth, memory_order_relaxed);
}

static inline bool TriangleVsHiZ(
	float2 minP,
	float2 maxP,
	float maximumDepth,
	float2 inverseOutputResolution,
	texture2d<float> hierarchy)
{
	const float2 dimensions = maxP - minP;
	const float mipLevel = ceil(log2(0.5f * max(dimensions.x, dimensions.y)));
	const float tileDepth = SampleDepth(
		hierarchy,
		(minP + maxP) * 0.5f * inverseOutputResolution,
		mipLevel);

	return !(tileDepth > maximumDepth);
}

static inline bool TriangleVsHiZ(
	float2 minP,
	float2 maxP,
	float maximumDepth,
	float2 inverseOutputResolution,
	texture2d_array<float> hierarchy,
	uint slice)
{
	const float2 dimensions = maxP - minP;
	const float mipLevel = ceil(log2(0.5f * max(dimensions.x, dimensions.y)));
	const float tileDepth = SampleDepth(
		hierarchy,
		(minP + maxP) * 0.5f * inverseOutputResolution,
		slice,
		mipLevel);

	return !(tileDepth > maximumDepth);
}

static inline void RasterizeDepth(
	float2 p0SS,
	float2 p1SS,
	float2 p2SS,
	float4 p0CS,
	float4 p1CS,
	float4 p2CS,
	float area,
	float2 minP,
	float2 maxP,
	bool scanlineRasterization,
	uint2 outputResolution,
	device atomic_uint* depth)
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

	if (scanlineRasterization)
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

			xMax += fract(xMax) == 0.5f ? -1.0f : 0.0f;

			float area0Temporary = area0 - dxdy0.y * (xMin - minP.x);
			float area1Temporary = area1 - dxdy1.y * (xMin - minP.x);

			for (float x = xMin; x <= xMax; x += 1.0f)
			{
				const float weight0 = area0Temporary * invArea;
				const float weight1 = area1Temporary * invArea;

				WriteDepth(
					depth,
					uint2(x, y),
					outputResolution,
					GetDepthBits(weight0, weight1, z0NDC, z1NDC, z2NDC));

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
						p2SS))
				{
					const float weight0 = area0Temporary * invArea;
					const float weight1 = area1Temporary * invArea;

					WriteDepth(
						depth,
						uint2(x, y),
						outputResolution,
						GetDepthBits(weight0, weight1, z0NDC, z1NDC, z2NDC));
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

#endif // RASTERIZATION_METAL
