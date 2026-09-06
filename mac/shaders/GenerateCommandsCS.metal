#include "TypesAndConstants.metal"

struct ICBContainer
{
	command_buffer commandBuffer [[id(0)]];
};

struct GenerateParameters
{
	uint frustum;
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
	const uint count = atomic_load_explicit(
		&counters[parameters.frustum * parameters.maxMeshes + meshIndex],
		memory_order_relaxed);

	if (count > 0)
	{
		const uint commandIndex = atomic_fetch_add_explicit(
			&commandRanges[parameters.frustum].length,
			1,
			memory_order_relaxed);
		render_command command(container->commandBuffer, commandIndex);
		command.draw_indexed_primitives(
			primitive_type::triangle,
			mesh.indexCountPerInstance,
			indices + mesh.startIndexLocation,
			count,
			mesh.baseVertexLocation,
			parameters.frustum * parameters.maxInstances + mesh.startInstanceLocation);

		const uint softwareCommandIndex = atomic_fetch_add_explicit(
			&dispatchArguments[parameters.frustum].x,
			1,
			memory_order_relaxed);

		IndirectCommand result;
		result.startInstanceLocation = parameters.frustum * parameters.maxInstances + mesh.startInstanceLocation;
		result.args.indexCountPerInstance = mesh.indexCountPerInstance;
		result.args.instanceCount = count;
		result.args.startIndexLocation = mesh.startIndexLocation;
		result.args.baseVertexLocation = mesh.baseVertexLocation;
		result.args.startInstanceLocation = 0;

		softwareCommands[parameters.frustum * parameters.maxMeshes + softwareCommandIndex] = result;
	}
}
