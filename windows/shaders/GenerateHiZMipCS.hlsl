#include "Common.hlsli"

cbuffer Constants : register(b0)
{
	uint2 NPOT;
	uint2 OutputResolution;
};

Texture2D Input : register(t0);

RWTexture2D<float> Output : register(u0);

[numthreads(HIZ_THREADS_X, HIZ_THREADS_Y, HIZ_THREADS_Z)]
void main(
	uint3 groupID : SV_GroupID,
	uint3 dispatchThreadID : SV_DispatchThreadID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (any(dispatchThreadID.xy >= OutputResolution.xy))
	{
		return;
	}

	uint2 xyInput = 2 * dispatchThreadID.xy;

	// TODO: account for non-reversed Z depth buffers
	float result = 1.0;
	result = min(result, Input[xyInput + uint2(0, 0)].r);
	result = min(result, Input[xyInput + uint2(1, 0)].r);
	result = min(result, Input[xyInput + uint2(0, 1)].r);
	result = min(result, Input[xyInput + uint2(1, 1)].r);

	bool borderXNPOT = (dispatchThreadID.x == OutputResolution.x - 1) && NPOT.x;
	bool borderYNPOT = (dispatchThreadID.y == OutputResolution.y - 1) && NPOT.y;

	if (borderXNPOT)
	{
		result = min(result, Input[xyInput + uint2(2, 0)].r);
		result = min(result, Input[xyInput + uint2(2, 1)].r);
	}

	if (borderYNPOT)
	{
		result = min(result, Input[xyInput + uint2(0, 2)].r);
		result = min(result, Input[xyInput + uint2(1, 2)].r);
	}

	if (borderXNPOT && borderYNPOT)
	{
		result = min(result, Input[xyInput + uint2(2, 2)].r);
	}

	Output[dispatchThreadID.xy] = result;
}