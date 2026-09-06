#include <metal_stdlib>
using namespace metal;

// https://blog.selfshadow.com/publications/overdraw-in-overdrive/
fragment void DrawOverdrawPS(
	float4 position [[position]],
	ushort lane [[thread_index_in_quadgroup]],
	constant uint& quadWidth [[buffer(7)]],
	device atomic_uint* quadOverdraw [[buffer(8)]])
{
	// Helper lanes participate in quad operations but cannot commit buffer writes.
	// Elect a covered lane, which need not be lane zero for a small triangle.
	const bool live = !quad_is_helper_thread();
	const uint liveLanes = static_cast<quad_vote::vote_t>(quad_ballot(live));
	if (live && lane == ctz(liveLanes))
	{
		const uint2 quadID = uint2(position.xy) / 2;
		atomic_fetch_add_explicit(&quadOverdraw[quadID.y * quadWidth + quadID.x], 1u, memory_order_relaxed);
	}
}

float3 OverdrawColor(uint count)
{
	if (count == 0)
	{
		return 0.0f;
	}

	const float level = log2(float(count));
	if (level < 1.0f)
	{
		return mix(float3(0.0f, 0.1f, 0.8f), float3(0.0f, 0.8f, 1.0f), level);
	}
	if (level < 2.0f)
	{
		return mix(float3(0.0f, 0.8f, 1.0f), float3(0.0f, 1.0f, 0.0f), level - 1.0f);
	}
	if (level < 3.0f)
	{
		return mix(float3(0.0f, 1.0f, 0.0f), float3(1.0f, 1.0f, 0.0f), level - 2.0f);
	}
	if (level < 4.0f)
	{
		return mix(float3(1.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), level - 3.0f);
	}
	if (level < 5.0f)
	{
		return mix(float3(1.0f, 0.0f, 0.0f), float3(1.0f), level - 4.0f);
	}

	return 1.0f;
}

fragment float4 DrawOverdrawDisplayPS(
	float4 position [[position]],
	constant uint& quadWidth [[buffer(7)]],
	device const uint* quadOverdraw [[buffer(8)]])
{
	const uint2 quadID = uint2(position.xy) / 2;
	const uint count = quadOverdraw[quadID.y * quadWidth + quadID.x];
	return float4(OverdrawColor(count), 1.0f);
}
