#include "CullingCommon.hlsli"

RWStructuredBuffer<uint> InstanceCounters : register(u0);

[numthreads(CULLING_THREADS_X, CULLING_THREADS_Y, CULLING_THREADS_Z)]
void main(
	uint3 groupID : SV_GroupID,
	uint3 dispatchThreadID : SV_DispatchThreadID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (dispatchThreadID.x >= TotalMeshesCount)
	{
		return;
	}

	[unroll(MAX_FRUSTUMS_COUNT)]
	for (uint frustum = 0; frustum < MAX_FRUSTUMS_COUNT; frustum++)
	{
		InstanceCounters[dispatchThreadID.x + frustum * MaxSceneMeshesMetaCount] = 0;
	}
}