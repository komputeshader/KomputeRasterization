#include "TypesAndConstants.metal"

struct FullscreenOutput
{
	float4 position [[position]];
	float2 uv;
};

vertex FullscreenOutput FullscreenVS(uint vertexID [[vertex_id]])
{
	constexpr float2 positions[] =
	{
		float2(-1, -1),
		float2(3, -1),
		float2(-1, 3)
	};

	FullscreenOutput result;
	result.position = float4(positions[vertexID], 0.0f, 1.0f);
	result.uv = positions[vertexID] * float2(0.5f, -0.5f) + 0.5f;

	return result;
}

fragment float4 CompositePS(
	FullscreenOutput input [[stage_in]],
	texture2d<float> source [[texture(0)]])
{
	constexpr sampler pointSampler(coord::normalized, address::clamp_to_edge, filter::nearest);

	return source.sample(pointSampler, input.uv);
}
