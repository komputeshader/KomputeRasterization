#include "CullingCommon.metal"

kernel void ClearCounters(
	device uint* counters [[buffer(0)]],
	constant uint& count [[buffer(1)]],
	device CullingCommandArguments* commandCounters [[buffer(2)]],
	constant uint& frustumsCount [[buffer(3)]],
	constant uint& maxMeshes [[buffer(4)]],
	uint index [[thread_position_in_grid]])
{
	if (index < count)
	{
		counters[index] = 0;
	}

	if (index < frustumsCount)
	{
		commandCounters[index].location = index * maxMeshes;
		commandCounters[index].dispatch.x = 0;
		commandCounters[index].dispatch.y = SWR_THREAD_GROUPS_Y;
		commandCounters[index].dispatch.z = 1;
	}
}

kernel void CullInstances(
	device const MeshMeta* meshes [[buffer(0)]],
	device const Instance* instances [[buffer(1)]],
	device Instance* visibleInstances [[buffer(2)]],
	device atomic_uint* instanceCounters [[buffer(3)]],
	constant CullingCB& constants [[buffer(4)]],
	texture2d<float> previousDepth [[texture(0)]],
	texture2d_array<float> previousShadows [[texture(1)]],
	uint index [[thread_position_in_grid]])
{
	if (index >= constants.totalInstancesCount)
	{
		return;
	}

	const Instance instance = instances[index];
	MeshMeta mesh = meshes[instance.meshID];
	mesh.aabb = TransformAABB(mesh.aabb, instance.worldTransform);
	mesh.coneApex = packed_float3((instance.worldTransform * float4(float3(mesh.coneApex), 1.0f)).xyz);
	mesh.coneAxis = packed_float3((instance.worldTransform * float4(float3(mesh.coneAxis), 0.0f)).xyz);

	const bool cameraBackface = dot(
		normalize(float3(mesh.coneApex) - constants.cameraPosition.xyz),
		float3(mesh.coneAxis)) >= mesh.coneCutoff;
	bool cameraVisible = !constants.clusterBackfaceCullingEnabled || !cameraBackface;
	cameraVisible &= !constants.frustumCullingEnabled || FrustumVsAABB(constants.camera, mesh.aabb);

	if (cameraVisible && constants.cameraHiZCullingEnabled && constants.hasCameraHistory)
	{
		cameraVisible = AABBVsHiZ(
			mesh.aabb, constants.prevFrameCameraVP, constants.depthResolution, previousDepth);
	}

	if (cameraVisible)
	{
		const uint counterIndex = instance.meshID;
		const uint offset = atomic_fetch_add_explicit(
			&instanceCounters[counterIndex], 1, memory_order_relaxed);
		visibleInstances[mesh.startInstanceLocation + offset] = instance;
	}

	const bool shadowBackface = dot(-constants.lightDirection.xyz, float3(mesh.coneAxis)) >= mesh.coneCutoff;

	for (uint cascade = 0; cascade < constants.cascadesCount; cascade++)
	{
		bool visible = !constants.clusterBackfaceCullingEnabled || !shadowBackface;
		visible &= !constants.frustumCullingEnabled || FrustumVsAABB(constants.cascade[cascade], mesh.aabb);
		if (visible && constants.shadowsHiZCullingEnabled && constants.hasShadowHistory)
		{
			visible = AABBVsHiZArray(
				mesh.aabb, constants.prevFrameCascadeVP[cascade], constants.shadowMapResolution, previousShadows, cascade);
		}

		if (visible)
		{
			const uint counterIndex = (cascade + 1) * constants.maxSceneMeshesMetaCount + instance.meshID;
			const uint offset = atomic_fetch_add_explicit(
				&instanceCounters[counterIndex], 1, memory_order_relaxed);
			const uint output = (cascade + 1) * constants.maxSceneInstancesCount +
				mesh.startInstanceLocation + offset;

			visibleInstances[output] = instance;
		}
	}
}
