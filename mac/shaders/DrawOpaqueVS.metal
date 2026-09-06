#include "Common.metal"

vertex VSOutput DrawOpaqueVS(
	VSInput input [[stage_in]],
	device const Instance* instances [[buffer(5)]],
	constant SceneCB& constants [[buffer(7)]],
	uint instanceID [[instance_id]])
{
	VSOutput result;

	const Instance instance = instances[instanceID];

	result.positionWS = (instance.worldTransform * float4(input.position, 1.0f)).xyz;
	result.position = constants.vp * float4(result.positionWS, 1.0f);
	result.linearDepth = result.position.w;
	result.normal = UnpackNormal(input.normal);
	result.color = constants.showMeshlets ? float4(float3(instance.color), 1.0f) : UnpackColor(input.color);
	result.uv = UnpackTexcoords(input.uv);

	return result;
}
