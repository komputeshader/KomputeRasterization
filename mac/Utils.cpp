#include "Utils.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace
{
	simd_float4 Row(simd_float4x4 matrix, int row)
	{
		return
		{
			matrix.columns[0][row], matrix.columns[1][row], matrix.columns[2][row], matrix.columns[3][row]
		};
	}
}

namespace Utils
{

simd_float4 NormalizePlane(simd_float4 plane)
{
	const float length = simd_length(plane.xyz);
	return length > 0.0f ? plane / length : plane;
}

Frustum GetFrustum(simd_float4x4 viewProjection)
{
	Frustum result;

	const simd_float4 r0 = Row(viewProjection, 0);
	const simd_float4 r1 = Row(viewProjection, 1);
	const simd_float4 r2 = Row(viewProjection, 2);
	const simd_float4 r3 = Row(viewProjection, 3);
	result.l = NormalizePlane(r3 + r0);
	result.r = NormalizePlane(r3 - r0);
	result.b = NormalizePlane(r3 + r1);
	result.t = NormalizePlane(r3 - r1);
	result.n = NormalizePlane(r3 - r2);
	result.f = NormalizePlane(r2);

	const simd_float4x4 inverse = simd_inverse(viewProjection);
	uint32_t corner = 0;
	for (float z : { 1.0f, 0.0f })
	{
		for (float y : { 1.0f, -1.0f })
		{
			for (float x : { -1.0f, 1.0f })
			{
				simd_float4 world = simd_mul(inverse, simd_make_float4(x, y, z, 1.0f));
				result.cornersWS[corner++] = world / world.w;
			}
		}
	}

	return result;
}

AABB TransformAABB(
	const AABB& box,
	simd_float4x4 transform)
{
	const simd_float3 center = ToSIMD(box.center);
	const simd_float3 extents = ToSIMD(box.extents);
	const simd_float4 transformed = simd_mul(transform, simd_make_float4(center, 1.0f));
	simd_float3 outputExtents = {};

	for (int row = 0; row < 3; row++)
	{
		for (int column = 0; column < 3; column++)
		{
			outputExtents[row] += std::abs(transform.columns[column][row]) * extents[column];
		}
	}

	AABB result;

	result.center = FromSIMD(transformed.xyz);
	result.extents = FromSIMD(outputExtents);

	return result;
}

AABB MergeAABBs(const AABB& a, const AABB& b)
{
	const simd_float3 aMin = ToSIMD(a.center) - ToSIMD(a.extents);
	const simd_float3 aMax = ToSIMD(a.center) + ToSIMD(a.extents);

	const simd_float3 bMin = ToSIMD(b.center) - ToSIMD(b.extents);
	const simd_float3 bMax = ToSIMD(b.center) + ToSIMD(b.extents);

	const simd_float3 minimum = simd_min(aMin, bMin);
	const simd_float3 maximum = simd_max(aMax, bMax);

	AABB result;

	result.center = FromSIMD((minimum + maximum) * 0.5f);
	result.extents = FromSIMD((maximum - minimum) * 0.5f);

	return result;
}

simd_float4x4 LookAtLH(
	simd_float3 eye,
	simd_float3 target,
	simd_float3 up)
{
	const simd_float3 look = simd_normalize(target - eye);
	const simd_float3 right = simd_normalize(simd_cross(up, look));
	const simd_float3 correctedUp = simd_cross(look, right);

	return simd_matrix_from_rows(
		simd_make_float4(right, -simd_dot(right, eye)),
		simd_make_float4(correctedUp, -simd_dot(correctedUp, eye)),
		simd_make_float4(look, -simd_dot(look, eye)),
		simd_make_float4(0.0f, 0.0f, 0.0f, 1.0f));
}

simd_float4x4 PerspectiveFovLHReverseZ(
	float fovY,
	float aspect,
	float nearZ,
	float farZ)
{
	const float y = 1.0f / std::tan(fovY * 0.5f);
	const float x = y / aspect;
	const float a = nearZ / (nearZ - farZ);
	const float b = nearZ * farZ / (farZ - nearZ);

	return (simd_float4x4){
		simd_make_float4(x, 0.0f, 0.0f, 0.0f),
		simd_make_float4(0.0f, y, 0.0f, 0.0f),
		simd_make_float4(0.0f, 0.0f, a, 1.0f),
		simd_make_float4(0.0f, 0.0f, b, 0.0f)
	};
}

simd_float4x4 OrthographicOffCenterLHReverseZ(
	float left,
	float right,
	float bottom,
	float top,
	float nearZ,
	float farZ)
{
	const float inverseWidth = 1.0f / (right - left);
	const float inverseHeight = 1.0f / (top - bottom);
	const float inverseDepth = 1.0f / (farZ - nearZ);

	return (simd_float4x4){
		simd_make_float4(2.0f * inverseWidth, 0.0f, 0.0f, 0.0f),
		simd_make_float4(0.0f, 2.0f * inverseHeight, 0.0f, 0.0f),
		simd_make_float4(0.0f, 0.0f, -inverseDepth, 0.0f),
		simd_make_float4(
			-(right + left) * inverseWidth,
			-(top + bottom) * inverseHeight,
			farZ * inverseDepth,
			1.0f)
	};
}

std::filesystem::path FindAssetsPath()
{
	const std::filesystem::path sourceRoot = std::filesystem::path(MAC_SOURCE_DIR).parent_path();
	const std::filesystem::path candidates[] =
	{
		sourceRoot / "assets",
		std::filesystem::current_path() / "assets",
		std::filesystem::current_path() / "../assets",
		std::filesystem::current_path() / "../../assets"
	};

	for (const auto& candidate : candidates)
	{
		if (std::filesystem::exists(candidate))
		{
			return std::filesystem::canonical(candidate);
		}
	}

	return sourceRoot / "assets";
}

std::string ReadTextFile(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	ASSERT(stream, "Cannot open %s", path.string().c_str())

	std::ostringstream contents;
	contents << stream.rdbuf();

	return contents.str();
}

void Log()
{
}

void Log(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
}
}
