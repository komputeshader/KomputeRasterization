#include "CullingCommon.hlsli"

StructuredBuffer<MeshMeta> MeshesMeta : register(t0);
StructuredBuffer<uint> InstanceCounters : register(t1);

RWStructuredBuffer<IndirectCommand> Commands : register(u0);
RWStructuredBuffer<uint3> CommandsCounters : register(u1);

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

	MeshMeta meshMeta = MeshesMeta[dispatchThreadID.x];

	IndirectCommand result;
	result.startInstanceLocation = meshMeta.startInstanceLocation;
	result.args.indexCountPerInstance = meshMeta.indexCountPerInstance;
	result.args.startIndexLocation = meshMeta.startIndexLocation;
	result.args.baseVertexLocation = meshMeta.baseVertexLocation;
	result.args.startInstanceLocation = 0;

	[unroll(MAX_FRUSTUMS_COUNT)]
	for (uint frustum = 0; frustum < CascadesCount + 1; frustum++)
	{
		uint instanceCount = InstanceCounters[frustum * MaxSceneMeshesMetaCount + dispatchThreadID.x];
		if (instanceCount > 0)
		{
			uint writeIndex;
			InterlockedAdd(CommandsCounters[frustum].x, 1, writeIndex);
			result.args.instanceCount = instanceCount;
			Commands[frustum * MaxSceneMeshesMetaCount + writeIndex] = result;
		}
	}
}
