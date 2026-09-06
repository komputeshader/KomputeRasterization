#include "TypesAndConstants.hlsli"

cbuffer SceneCB : register(b0)
{
	float4x4 VP;
	float4x4 CascadeVP[MAX_CASCADES_COUNT];
	float4 SunDirection;
	float4 CascadeBias[MAX_CASCADES_COUNT / 4];
	float4 CascadeSplits[MAX_CASCADES_COUNT / 4];
	int ShowCascades;
	int ShowMeshlets;
	int CascadesCount;
	float ShadowsDistance;
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

Texture2DArray ShadowMap : register(t1);

SamplerState PointClampSampler : register(s0);

#include "Common.hlsli"

[earlydepthstencil]
float4 main(VSOutput input) : SV_TARGET
{
	float NdotL = saturate(dot(SunDirection.xyz, normalize(input.normal)));
	float shadow = GetShadow(input.linearDepth, input.positionWS);
	float3 ambient = 0.2 * SkyColor;

	float3 result = input.color.rgb * (NdotL * shadow + ambient);
	if (ShowCascades)
	{
		result = GetCascadeColor(input.linearDepth, input.positionWS);
		result *= (NdotL * shadow + ambient);
	}

	return float4(result, 1.0);
}