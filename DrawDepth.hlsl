#include "TypesAndConstants.hlsli"

cbuffer ConstantBuffer : register(b0)
{
	float4x4 VP;
};

struct VSInput
{
	float3 position : POSITION;
};

struct VSOutput
{
	float4 positionCS : SV_POSITION;
};

StructuredBuffer<InstanceData> Instances : register(t0);

VSOutput VSMain(VSInput input, uint instanceID : SV_InstanceID)
{
	VSOutput result;

	float4 positionWS = mul(Instances[instanceID].WorldTransform, float4(input.position, 1.0));
	result.positionCS = mul(VP, positionWS);

	return result;
}
