RWTexture2D<uint> FragmentOverdraw : register(u0);
RWTexture2D<float4> RenderTarget : register(u1);

#include "Overdraw.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	FragmentOverdraw.GetDimensions(width, height);
	uint2 pixel = dispatchThreadID.xy;
	if (pixel.x >= width || pixel.y >= height)
	{
		return;
	}

	RenderTarget[pixel] = float4(OverdrawColor(FragmentOverdraw[pixel]), 1.0);
}
