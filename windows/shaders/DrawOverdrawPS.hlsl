// https://blog.selfshadow.com/publications/overdraw-in-overdrive/

RWTexture2D<uint> QuadOverdraw : register(u0);

void main(float4 positionCS : SV_POSITION)
{
	float2 quadPosition = QuadReadLaneAt(positionCS.xy, 0);
	uint laneIndex = WaveGetLaneIndex();
	uint laneBit = laneIndex & 31;
	uint firstQuadBit = laneBit & ~3;
	uint4 activeLanes = WaveActiveBallot(true);
	uint activeQuadLanes =
		activeLanes[laneIndex / 32] & (0xFu << firstQuadBit);

	if (laneBit == firstbitlow(activeQuadLanes))
	{
		InterlockedAdd(QuadOverdraw[uint2(quadPosition) / 2], 1);
	}
}
