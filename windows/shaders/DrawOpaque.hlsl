#include "TypesAndConstants.hlsli"

cbuffer ConstantBuffer : register(b0)
{
	float4x4 VP;
	float4x4 CascadeVP[MaxCascadeCount];
	float3 SunDirection;
	uint CascadeCount;
	float4 CascadeBias[MaxCascadeCount / 4];
	float4 CascadeSplits[MaxCascadeCount / 4];
};

struct VSInput
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float3 color : COLOR;
	float2 uv : TEXCOORD0;
};

struct VSOutput
{
	float4 positionCS : SV_POSITION;
	float3 positionWS : POSITIONWS;
	float linearDepth : LDEPTH;
	float3 normal : NORMAL;
	float4 color : COLOR;
	float2 uv : TEXCOORD0;
};

StructuredBuffer<InstanceData> Instances : register(t0);

VSOutput VSMain(VSInput input, uint instanceID : SV_InstanceID)
{
	VSOutput result;

	result.positionWS = mul(Instances[instanceID].WorldTransform, float4(input.position, 1.0)).xyz;
	result.positionCS = mul(VP, float4(result.positionWS, 1.0));
	result.linearDepth = result.positionCS.w;
	result.normal = input.normal;
	result.color = float4(input.color, 1.0);
	result.uv = input.uv;

	return result;
}

Texture2DArray ShadowMap : register(t0);

SamplerState PointClampSampler : register(s0);

#include "Common.hlsli"

[earlydepthstencil]
float4 PSMain(VSOutput input) : SV_TARGET
{
	float NdotL = saturate(dot(SunDirection, normalize(input.normal)));
	float shadow = GetShadow(input.linearDepth, input.positionWS);
	float3 ambient = 0.2 * float3(136.0, 198.0, 252.0) / 256.0;

	return float4(input.color.rgb * (NdotL * shadow + ambient), 1.0);
}
