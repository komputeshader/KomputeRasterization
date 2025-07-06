#include "Mesh.h"

Mesh::Mesh(std::vector<MeshInfo> && submeshInfo, AABB boundingVolume)
{
	_submeshInfo = submeshInfo;
	_AABB = boundingVolume;
}

void Mesh::Draw(ID3D12GraphicsCommandList * commandList) const
{
	for (auto & submesh : _submeshInfo)
	{
		commandList->DrawIndexedInstanced(
			submesh.indexCountPerInstance,
			submesh.instanceCount,
			submesh.startIndexLocation,
			submesh.baseVertexLocation,
			submesh.startInstanceLocation);
	}
}