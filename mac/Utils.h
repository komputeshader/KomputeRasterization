#pragma once

#include "Common.h"

#include <filesystem>

namespace Utils
{

simd_float4 NormalizePlane(simd_float4 plane);
Frustum GetFrustum(simd_float4x4 viewProjection);

AABB TransformAABB(
	const AABB& box,
	simd_float4x4 transform);
AABB MergeAABBs(const AABB& a, const AABB& b);

simd_float4x4 LookAtLH(
	simd_float3 eye,
	simd_float3 target,
	simd_float3 up);
simd_float4x4 PerspectiveFovLHReverseZ(
	float fovY,
	float aspect,
	float nearZ,
	float farZ);
simd_float4x4 OrthographicOffCenterLHReverseZ(
	float left,
	float right,
	float bottom,
	float top,
	float nearZ,
	float farZ);

std::filesystem::path FindAssetsPath();
std::string ReadTextFile(const std::filesystem::path& path);

void Log();
void Log(const char* format, ...);

}

#define ASSERT(isFalse, ...) \
	if (!(bool)(isFalse)) { \
		Utils::Log("\nAssertion " #isFalse " failed in file %s, line %d\n", __FILE__, __LINE__); \
		Utils::Log(__VA_ARGS__); \
		Utils::Log("\n"); \
		__builtin_trap(); \
	}

