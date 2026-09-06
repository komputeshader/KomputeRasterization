#include "Common.metal"

fragment float4 DrawOpaquePS(
	VSOutput input [[stage_in]],
	constant SceneCB& constants [[buffer(7)]],
	texture2d_array<float> shadowMap [[texture(0)]])
{
	const float3 result = ShadePixel(
		input.normal,
		input.color.rgb,
		input.positionWS,
		input.linearDepth,
		constants,
		shadowMap);

	return float4(result, 1.0f);
}

struct FragmentResources
{
	texture2d_array<float> shadowMap [[id(0)]];
};

fragment float4 DrawOpaquePSICB(
	VSOutput input [[stage_in]],
	constant SceneCB& constants [[buffer(7)]],
	constant FragmentResources& resources [[buffer(8)]])
{
	const float3 result = ShadePixel(
		input.normal,
		input.color.rgb,
		input.positionWS,
		input.linearDepth,
		constants,
		resources.shadowMap);

	return float4(result, 1.0f);
}
