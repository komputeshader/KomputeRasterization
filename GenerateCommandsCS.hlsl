#include "CullingCommon.hlsli"

StructuredBuffer<MeshMeta> MeshesMeta : register(t0);
StructuredBuffer<uint> InstanceCounters : register(t1);

// TODO: remove hardcode and reduce atomics use per group
AppendStructuredBuffer<IndirectCommand> CameraCommands : register(u0);
AppendStructuredBuffer<IndirectCommand> Cascade0Commands : register(u1);
AppendStructuredBuffer<IndirectCommand> Cascade1Commands : register(u2);
AppendStructuredBuffer<IndirectCommand> Cascade2Commands : register(u3);
AppendStructuredBuffer<IndirectCommand> Cascade3Commands : register(u4);
AppendStructuredBuffer<IndirectCommand> Cascade4Commands : register(u5);
AppendStructuredBuffer<IndirectCommand> Cascade5Commands : register(u6);
AppendStructuredBuffer<IndirectCommand> Cascade6Commands : register(u7);
AppendStructuredBuffer<IndirectCommand> Cascade7Commands : register(u8);

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

	uint cameraCount = InstanceCounters[0 * MaxSceneMeshesMetaCount + dispatchThreadID.x];
	if (cameraCount > 0)
	{
		result.args.instanceCount = cameraCount;
		CameraCommands.Append(result);
	}

	uint cascade0Count = InstanceCounters[1 * MaxSceneMeshesMetaCount + dispatchThreadID.x];
	if (cascade0Count > 0)
	{
		result.args.instanceCount = cascade0Count;
		Cascade0Commands.Append(result);
	}

	uint cascade1Count = InstanceCounters[2 * MaxSceneMeshesMetaCount + dispatchThreadID.x];
	if (cascade1Count > 0)
	{
		result.args.instanceCount = cascade1Count;
		Cascade1Commands.Append(result);
	}

	uint cascade2Count = InstanceCounters[3 * MaxSceneMeshesMetaCount + dispatchThreadID.x];
	if (cascade2Count > 0)
	{
		result.args.instanceCount = cascade2Count;
		Cascade2Commands.Append(result);
	}

	uint cascade3Count = InstanceCounters[4 * MaxSceneMeshesMetaCount + dispatchThreadID.x];
	if (cascade3Count > 0)
	{
		result.args.instanceCount = cascade3Count;
		Cascade3Commands.Append(result);
	}

	uint cascade4Count = InstanceCounters[5 * MaxSceneMeshesMetaCount + dispatchThreadID.x];
	if (cascade4Count > 0)
	{
		result.args.instanceCount = cascade4Count;
		Cascade4Commands.Append(result);
	}

	uint cascade5Count = InstanceCounters[6 * MaxSceneMeshesMetaCount + dispatchThreadID.x];
	if (cascade5Count > 0)
	{
		result.args.instanceCount = cascade5Count;
		Cascade5Commands.Append(result);
	}

	uint cascade6Count = InstanceCounters[7 * MaxSceneMeshesMetaCount + dispatchThreadID.x];
	if (cascade6Count > 0)
	{
		result.args.instanceCount = cascade6Count;
		Cascade6Commands.Append(result);
	}

	uint cascade7Count = InstanceCounters[8 * MaxSceneMeshesMetaCount + dispatchThreadID.x];
	if (cascade7Count > 0)
	{
		result.args.instanceCount = cascade7Count;
		Cascade7Commands.Append(result);
	}
}