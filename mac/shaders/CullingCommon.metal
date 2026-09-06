#ifndef CULLING_COMMON_METAL
#define CULLING_COMMON_METAL

#include "TypesAndConstants.metal"

static inline AABB TransformAABB(AABB box, float4x4 matrix)
{
	const float3 center = (matrix * float4(float3(box.center), 1.0f)).xyz;
	const float3 e = float3(box.extents);
	float3 extents;

	extents.x = dot(abs(float3(matrix[0].x, matrix[1].x, matrix[2].x)), e);
	extents.y = dot(abs(float3(matrix[0].y, matrix[1].y, matrix[2].y)), e);
	extents.z = dot(abs(float3(matrix[0].z, matrix[1].z, matrix[2].z)), e);

	AABB result;
	result.center = packed_float3(center);
	result.extents = packed_float3(extents);
	result.pad0 = result.pad1 = 0.0f;

	return result;
}

static inline bool AABBVsPlane(AABB box, float4 plane)
{
	const float radius = dot(float3(box.extents), abs(plane.xyz));
	const float signedDistance = dot(plane.xyz, float3(box.center)) + plane.w;
	return radius + signedDistance >= 0.0f;
}

static inline bool FrustumVsAABB(Frustum frustum, AABB box)
{
	const float3 boxMin = float3(box.center) - float3(box.extents);
	const float3 boxMax = float3(box.center) + float3(box.extents);

	bool allXMin = true;
	bool allXMax = true;
	bool allYMin = true;
	bool allYMax = true;
	bool allZMin = true;
	bool allZMax = true;
	for (uint corner = 0; corner < 8; corner++)
	{
		const float3 p = frustum.corners[corner].xyz;
		allXMin &= p.x < boxMin.x;
		allXMax &= p.x > boxMax.x;
		allYMin &= p.y < boxMin.y;
		allYMax &= p.y > boxMax.y;
		allZMin &= p.z < boxMin.z;
		allZMax &= p.z > boxMax.z;
	}

	if (allXMin || allXMax || allYMin || allYMax || allZMin || allZMax)
	{
		return false;
	}

	return AABBVsPlane(box, frustum.left) && AABBVsPlane(box, frustum.right) &&
		AABBVsPlane(box, frustum.bottom) && AABBVsPlane(box, frustum.top) &&
		AABBVsPlane(box, frustum.near) && AABBVsPlane(box, frustum.far);
}

static inline float SampleDepth(texture2d<float> hierarchy, float2 uv, float mip)
{
	const uint level = uint(mip);
	const uint2 size = uint2(hierarchy.get_width(level), hierarchy.get_height(level));
	const float2 position = uv * float2(size) - 0.5f;
	const int2 p0 = int2(floor(position));
	const float2 fraction = fract(position);
	float depth = all(p0 >= 0) && all(p0 < int2(size))
		? hierarchy.read(uint2(p0), level).r
		: 0.0f;

	if (fraction.x > 0.0f)
	{
		const int2 p1 = p0 + int2(1, 0);
		depth = min(depth, all(p1 >= 0) && all(p1 < int2(size))
			? hierarchy.read(uint2(p1), level).r
			: 0.0f);
	}

	if (fraction.y > 0.0f)
	{
		const int2 p1 = p0 + int2(0, 1);
		depth = min(depth, all(p1 >= 0) && all(p1 < int2(size))
			? hierarchy.read(uint2(p1), level).r
			: 0.0f);
	}

	if (all(fraction > 0.0f))
	{
		const int2 p1 = p0 + int2(1, 1);
		depth = min(depth, all(p1 >= 0) && all(p1 < int2(size))
			? hierarchy.read(uint2(p1), level).r
			: 0.0f);
	}

	return depth;
}

static inline float SampleDepth(texture2d_array<float> hierarchy, float2 uv, uint slice, float mip)
{
	const uint level = uint(mip);
	const uint2 size = uint2(hierarchy.get_width(level), hierarchy.get_height(level));
	const float2 position = uv * float2(size) - 0.5f;
	const int2 p0 = int2(floor(position));
	const float2 fraction = fract(position);
	float depth = all(p0 >= 0) && all(p0 < int2(size))
		? hierarchy.read(uint2(p0), slice, level).r
		: 0.0f;

	if (fraction.x > 0.0f)
	{
		const int2 p1 = p0 + int2(1, 0);
		depth = min(depth, all(p1 >= 0) && all(p1 < int2(size))
			? hierarchy.read(uint2(p1), slice, level).r
			: 0.0f);
	}

	if (fraction.y > 0.0f)
	{
		const int2 p1 = p0 + int2(0, 1);
		depth = min(depth, all(p1 >= 0) && all(p1 < int2(size))
			? hierarchy.read(uint2(p1), slice, level).r
			: 0.0f);
	}

	if (all(fraction > 0.0f))
	{
		const int2 p1 = p0 + int2(1, 1);
		depth = min(depth, all(p1 >= 0) && all(p1 < int2(size))
			? hierarchy.read(uint2(p1), slice, level).r
			: 0.0f);
	}

	return depth;
}

static inline bool AABBVsHiZ(
	AABB box,
	float4x4 vp,
	float2 resolution,
	texture2d<float> hierarchy)
{
	float3 minimum = float3(INFINITY);
	float3 maximum = float3(-INFINITY);
	for (uint corner = 0; corner < 8; corner++)
	{
		const float3 sign = float3(
			(corner & 1) ? 1.0f : -1.0f,
			(corner & 2) ? 1.0f : -1.0f,
			(corner & 4) ? 1.0f : -1.0f);
		float4 clip = vp * float4(float3(box.center) + float3(box.extents) * sign, 1.0f);
		const float3 ndc = clip.xyz / clip.w;

		minimum = min(minimum, ndc);
		maximum = max(maximum, ndc);
	}

	float2 p0 = minimum.xy * float2(0.5f, -0.5f) + 0.5f;
	float2 p1 = maximum.xy * float2(0.5f, -0.5f) + 0.5f;

	const float2 uvMin = min(p0, p1);
	const float2 uvMax = max(p0, p1);

	const float extentPixels = max((uvMax.x - uvMin.x) * resolution.x,
		(uvMax.y - uvMin.y) * resolution.y);
	const float mip = clamp(ceil(log2(max(1.0f, extentPixels * 0.5f))),
		0.0f,
		float(hierarchy.get_num_mip_levels() - 1));
	const float tileDepth = SampleDepth(hierarchy, (uvMin + uvMax) * 0.5f, mip);

	return !(tileDepth > maximum.z);
}

static inline bool AABBVsHiZArray(
	AABB box,
	float4x4 vp,
	float2 resolution,
	texture2d_array<float> hierarchy,
	uint slice)
{
	float3 minimum = float3(INFINITY);
	float3 maximum = float3(-INFINITY);
	for (uint corner = 0; corner < 8; corner++)
	{
		const float3 sign = float3(
			(corner & 1) ? 1.0f : -1.0f,
			(corner & 2) ? 1.0f : -1.0f,
			(corner & 4) ? 1.0f : -1.0f);
		float4 clip = vp * float4(float3(box.center) + float3(box.extents) * sign, 1.0f);
		const float3 ndc = clip.xyz / clip.w;

		minimum = min(minimum, ndc);
		maximum = max(maximum, ndc);
	}

	float2 p0 = minimum.xy * float2(0.5f, -0.5f) + 0.5f;
	float2 p1 = maximum.xy * float2(0.5f, -0.5f) + 0.5f;

	const float2 uvMin = min(p0, p1);
	const float2 uvMax = max(p0, p1);

	const float extentPixels = max((uvMax.x - uvMin.x) * resolution.x,
		(uvMax.y - uvMin.y) * resolution.y);
	const float mip = clamp(ceil(log2(max(1.0f, extentPixels * 0.5f))),
		0.0f,
		float(hierarchy.get_num_mip_levels() - 1));
	const float tileDepth = SampleDepth(hierarchy, (uvMin + uvMax) * 0.5f, slice, mip);

	return !(tileDepth > maximum.z);
}

#endif // CULLING_COMMON_METAL
