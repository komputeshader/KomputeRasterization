#include "TypesAndConstants.metal"

#ifdef SHADOWS
vertex float4 DrawShadowVS(
#else
vertex float4 DrawDepthVS(
#endif
	DepthVSInput input [[stage_in]],
	device const Instance* instances [[buffer(5)]],
	constant float4x4& vp [[buffer(7)]],
	uint instanceID [[instance_id]])
{
	const Instance instance = instances[instanceID];

	const float3 positionWS = (instance.worldTransform * float4(input.position, 1.0f)).xyz;

	return vp * float4(positionWS, 1.0f);
}
