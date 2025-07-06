#pragma once

#include "Types.h"

class Mesh
{
public:

	Mesh(
		std::vector<MeshInfo> && submeshesInfo,
		AABB boundingVolume
	);
	Mesh(Mesh const &) = delete;
	Mesh & operator=(Mesh const &) = delete;
	Mesh(Mesh &&) = default;
	Mesh & operator=(Mesh &&) = default;
	~Mesh() = default;

	void Draw(ID3D12GraphicsCommandList * commandList) const;

	std::vector<MeshInfo> const & GetSubmeshes() const { return _submeshInfo; }
	AABB const & GetAABB() const { return _AABB; }

private:

	std::vector<MeshInfo> _submeshInfo;
	AABB _AABB;

};