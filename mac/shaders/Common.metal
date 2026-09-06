#ifndef COMMON_METAL
#define COMMON_METAL

#include "TypesAndConstants.metal"

inline float3 UnpackNormal(uint packed)
{
	constexpr float scale = 2.0f / 1023.0f;

	return float3((packed >> 20) & 1023, (packed >> 10) & 1023, packed & 1023) * scale - 1.0f;
}

inline float UnpackHalf(uint bits)
{
	return float(as_type<half>(ushort(bits & 0xffff)));
}

inline float4 UnpackColor(uint2 packed)
{
	return float4(
		UnpackHalf(packed.x >> 16), UnpackHalf(packed.x), UnpackHalf(packed.y >> 16), UnpackHalf(packed.y));
}

inline float2 UnpackTexcoords(uint packed)
{
	return float2(UnpackHalf(packed >> 16), UnpackHalf(packed));
}

inline uint SelectCascade(float depth, constant SceneCB& constants)
{
	uint selected = max(1u, constants.cascadesCount) - 1;
	for (int cascade = int(constants.cascadesCount) - 1; cascade >= 0; --cascade)
	{
		if (depth <= constants.cascadeSplits[cascade / 4][cascade % 4])
		{
			selected = cascade;
		}
	}

	return selected;
}

inline float GetShadow(
	float viewDepth,
	float3 positionWS,
	constant SceneCB& constants,
	texture2d_array<float> shadowMap)
{
	if (viewDepth >= constants.shadowsDistance || constants.cascadesCount == 0)
	{
		return 1.0f;
	}

	const uint cascade = SelectCascade(viewDepth, constants);

	float4 lightClip = constants.cascadeVP[cascade] * float4(positionWS, 1.0f);
	lightClip.xyz /= lightClip.w;
	const float2 uv = lightClip.xy * float2(0.5f, -0.5f) + 0.5f;
	constexpr sampler pointSampler(coord::normalized, address::clamp_to_zero, filter::nearest);
	const float stored = shadowMap.sample(pointSampler, uv, cascade).r;
	const float bias = constants.cascadeBias[cascade / 4][cascade % 4];

	return lightClip.z > stored - bias ? 1.0f : 0.0f;
}

inline float GetShadow(
	float viewDepth,
	float3 positionWS,
	constant SceneCB& constants,
	device const uint* shadowMap)
{
	if (viewDepth >= constants.shadowsDistance || constants.cascadesCount == 0)
	{
		return 1.0f;
	}

	const uint cascade = SelectCascade(viewDepth, constants);

	float4 lightClip = constants.cascadeVP[cascade] * float4(positionWS, 1.0f);
	lightClip.xyz /= lightClip.w;
	const float2 uv = lightClip.xy * float2(0.5f, -0.5f) + 0.5f;
	float stored = 0.0f;
	if (all(uv >= 0.0f) && all(uv < 1.0f))
	{
		const uint2 resolution = uint2(constants.shadowMapResolution);
		const uint2 pixel = min(uint2(uv * constants.shadowMapResolution), resolution - 1);
		const uint index = cascade * resolution.x * resolution.y +
			pixel.y * resolution.x + pixel.x;
		stored = as_type<float>(shadowMap[index]);
	}

	const float bias = constants.cascadeBias[cascade / 4][cascade % 4];

	return lightClip.z > stored - bias ? 1.0f : 0.0f;
}

inline float3 ShadePixel(
	float3 normal,
	float3 color,
	float3 positionWS,
	float viewDepth,
	constant SceneCB& constants,
	texture2d_array<float> shadowMap)
{
	const float NdotL = saturate(dot(constants.sunDirection.xyz, normalize(normal)));
	const float shadow = GetShadow(viewDepth, positionWS, constants, shadowMap);
	const float3 ambient = 0.2f * SkyColor.rgb;

	float3 result = color * (NdotL * shadow + ambient);
	if (constants.showCascades)
	{
		constexpr float3 cascadeColors[8] =
		{
			float3(1, 0, 0),
			float3(0, 1, 0),
			float3(0, 0, 1),
			float3(1, 1, 0),
			float3(0.5f, 0, 0),
			float3(0, 0.5f, 0),
			float3(0, 0, 0.5f),
			float3(0.5f, 0.5f, 0)
		};

		result = cascadeColors[SelectCascade(viewDepth, constants)];
		result *= (NdotL * shadow + ambient);
	}

	return result;
}

inline float3 ShadePixel(
	float3 normal,
	float3 color,
	float3 positionWS,
	float viewDepth,
	constant SceneCB& constants,
	device const uint* shadowMap)
{
	const float NdotL = saturate(dot(constants.sunDirection.xyz, normalize(normal)));
	const float shadow = GetShadow(viewDepth, positionWS, constants, shadowMap);
	const float3 ambient = 0.2f * SkyColor.rgb;

	float3 result = color * (NdotL * shadow + ambient);
	if (constants.showCascades)
	{
		constexpr float3 cascadeColors[8] =
		{
			float3(1, 0, 0),
			float3(0, 1, 0),
			float3(0, 0, 1),
			float3(1, 1, 0),
			float3(0.5f, 0, 0),
			float3(0, 0.5f, 0),
			float3(0, 0, 0.5f),
			float3(0.5f, 0.5f, 0)
		};

		result = cascadeColors[SelectCascade(viewDepth, constants)];
		result *= (NdotL * shadow + ambient);
	}

	return result;
}

#endif // COMMON_METAL
