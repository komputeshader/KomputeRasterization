#pragma once

#import <Metal/Metal.h>

#include "Camera.h"
#include "Settings.h"

#include <filesystem>
#include <memory>

class Scene
{
public:

	static size_t MaxSceneFacesCount;
	static size_t MaxSceneInstancesCount;
	static size_t MaxSceneMeshesMetaCount;

	Scene();
	Scene(const Scene&) = delete;
	Scene& operator=(const Scene&) = delete;
	~Scene();

	void Load(ScenesIndices kind);
	void LoadBuddha();
	void LoadPlant();

	ScenesIndices GetKind() const { return _kind; }

	id<MTLBuffer> GetPositionsBuffer() const;
	id<MTLBuffer> GetNormalsBuffer() const;
	id<MTLBuffer> GetColorsBuffer() const;
	id<MTLBuffer> GetTexcoordsBuffer() const;
	id<MTLBuffer> GetIndicesBuffer() const;
	id<MTLBuffer> GetIndicesSOABuffer() const;
	id<MTLBuffer> GetMeshesBuffer() const;
	id<MTLBuffer> GetInstancesBuffer() const;

	uint32_t GetMeshCount() const { return static_cast<uint32_t>(_meshCount); }
	uint32_t GetInstanceCount() const { return static_cast<uint32_t>(_instanceCount); }
	uint64_t GetTotalFacesCount() const { return _totalFacesCount; }
	uint32_t GetTrianglesCount() const { return _trianglesCount; }
	const AABB& GetSceneAABB() const { return _sceneAABB; }
	const std::vector<MeshMeta>& GetMeshesMetaCPU() const { return _meshes; }

	Camera camera;
	float FOV = 90.0f;
	float nearZ = Settings::CameraNearZ;
	float farZ = Settings::CameraFarZ;
	bool FOVChanged = false;

	Float3 lightDirection = { -1.0f, 1.0f, -1.0f };

private:

	struct Resources;
	std::unique_ptr<Resources> _resources;

	void _clear();
	void _loadObj(
		const std::filesystem::path& path,
		float translation,
		float scale,
		uint32_t instancesCountX,
		uint32_t instancesCountZ,
		float rotationYRadians);
	void _upload();

	ScenesIndices _kind = ScenesIndices::Buddha;
	std::vector<VertexPosition> _positions;
	std::vector<VertexNormal> _normals;
	std::vector<VertexColor> _colors;
	std::vector<VertexUV> _texcoords;
	std::vector<uint32_t> _indices;
	std::vector<uint32_t> _indicesSOA;
	std::vector<MeshMeta> _meshes;
	std::vector<Instance> _instances;
	std::vector<Prefab> _prefabs;
	size_t _meshCount = 0;
	size_t _instanceCount = 0;
	uint64_t _totalFacesCount = 0;
	uint32_t _trianglesCount = 0;

	AABB _sceneAABB;
	bool _hasSceneBounds = false;
};

