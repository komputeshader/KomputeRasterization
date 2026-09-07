Texture2D<uint> QuadOverdraw : register(t0);

#include "Overdraw.hlsli"

float4 main(float4 positionCS : SV_POSITION) : SV_TARGET
{
	uint2 quadID = uint2(positionCS.xy) / 2;
	uint count = QuadOverdraw.Load(int3(quadID, 0));

	return float4(OverdrawColor(count), 1.0);
}
