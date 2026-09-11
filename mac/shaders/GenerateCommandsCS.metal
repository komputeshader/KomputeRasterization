#include "TypesAndConstants.metal"

struct ICBContainer
{
	command_buffer commandBuffer [[id(0)]];
};

struct GenerateParameters
{
	uint frustumsCount;
	uint meshCount;
	uint maxMeshes;
	uint maxInstances;
};

kernel void GenerateCommandsCS(
	device const MeshMeta* meshes [[buffer(0)]],
	device const atomic_uint* counters [[buffer(1)]],
	device const uint* indices [[buffer(2)]],
	constant GenerateParameters& parameters [[buffer(3)]],
	device ICBContainer* container [[buffer(4)]],
	device ICBExecutionRange* commandRanges [[buffer(5)]],
	device IndirectCommand* softwareCommands [[buffer(6)]],
	device DispatchArguments* dispatchArguments [[buffer(7)]],
	uint meshIndex [[thread_position_in_grid]])
{
	if (meshIndex >= parameters.meshCount)
	{
		return;
	}

	const MeshMeta mesh = meshes[meshIndex];
	IndirectCommand result;
	result.args.indexCountPerInstance = mesh.indexCountPerInstance;
	result.args.startIndexLocation = mesh.startIndexLocation;
	result.args.baseVertexLocation = mesh.baseVertexLocation;
	result.args.startInstanceLocation = 0;

	for (uint frustum = 0; frustum < parameters.frustumsCount; frustum++)
	{
		const uint instanceCount = atomic_load_explicit(
			&counters[frustum * parameters.maxMeshes + meshIndex],
			memory_order_relaxed);

		if (instanceCount > 0)
		{
			const uint writeIndex = atomic_fetch_add_explicit(
				&dispatchArguments[frustum].x,
				1,
				memory_order_relaxed);
			result.startInstanceLocation = frustum * parameters.maxInstances + mesh.startInstanceLocation;
			result.args.instanceCount = instanceCount;
			softwareCommands[frustum * parameters.maxMeshes + writeIndex] = result;

			render_command command(container->commandBuffer, frustum * parameters.maxMeshes + writeIndex);
			command.draw_indexed_primitives(
				primitive_type::triangle,
				mesh.indexCountPerInstance,
				indices + mesh.startIndexLocation,
				instanceCount,
				mesh.baseVertexLocation,
				result.startInstanceLocation);
			atomic_fetch_add_explicit(&commandRanges[frustum].length, 1, memory_order_relaxed);
		}
	}
}
