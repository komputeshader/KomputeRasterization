#include "Scene.h"

#include "Context.h"
#include "Settings.h"
#include "Utils.h"

#include "meshoptimizer.h"
#include <rapidobj/rapidobj.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <thread>

struct Scene::Resources
{
	id<MTLBuffer> positions = nil;
	id<MTLBuffer> normals = nil;
	id<MTLBuffer> colors = nil;
	id<MTLBuffer> texcoords = nil;
	id<MTLBuffer> indices = nil;
	id<MTLBuffer> indicesSOA = nil;
	id<MTLBuffer> meshes = nil;
	id<MTLBuffer> instances = nil;
};

size_t Scene::MaxSceneFacesCount = 0;
size_t Scene::MaxSceneInstancesCount = 0;
size_t Scene::MaxSceneMeshesMetaCount = 0;

namespace
{
	constexpr size_t MaxOBJProcessingThreads = 8;
	constexpr size_t MeshletMaxVertices = 128;
	constexpr size_t MeshletMaxTriangles = MESHLET_SIZE;
	constexpr float MeshletConeWeight = 0.0f;

	struct ProcessedOBJShape
	{
		std::vector<VertexPosition> positions;
		std::vector<VertexNormal> normals;
		std::vector<VertexColor> colors;
		std::vector<VertexUV> texcoords;
		std::vector<uint32_t> indices;
		std::vector<MeshMeta> meshes;
		simd_float3 min = simd_make_float3(INFINITY);
		simd_float3 max = simd_make_float3(-INFINITY);
		size_t facesCount = 0;
	};

	uint32_t PackNormal(simd_float3 normal)
	{
		return (meshopt_quantizeUnorm(normal.x * 0.5f + 0.5f, 10) << 20) |
				(meshopt_quantizeUnorm(normal.y * 0.5f + 0.5f, 10) << 10) |
				meshopt_quantizeUnorm(normal.z * 0.5f + 0.5f, 10);
	}

	VertexColor MakeDefaultColor()
	{
		VertexColor result;
		result.packedColor[0] =
			(static_cast<uint32_t>(meshopt_quantizeHalf(0.8f)) << 16) |
			static_cast<uint32_t>(meshopt_quantizeHalf(0.8f));
		result.packedColor[1] =
			(static_cast<uint32_t>(meshopt_quantizeHalf(0.8f)) << 16) |
			static_cast<uint32_t>(meshopt_quantizeHalf(1.0f));
		return result;
	}

	simd_float4x4 RotationY(float angle)
	{
		return simd_matrix4x4(simd_quaternion(angle, simd_make_float3(0.0f, 1.0f, 0.0f)));
	}

	ProcessedOBJShape ProcessOBJShape(
		const rapidobj::Shape& shape,
		const rapidobj::Attributes& attributes,
		float scale,
		float rotationYRadians)
	{
		ProcessedOBJShape result;
		const size_t indexCount = shape.mesh.indices.size();
		if (indexCount == 0)
		{
			return result;
		}

		ASSERT(indexCount % 3 == 0)
		result.facesCount = indexCount / 3;
		std::vector<uint32_t> indices(indexCount);
		std::vector<Float3> positions;
		std::vector<VertexNormal> packedNormals;
		std::vector<VertexUV> packedTexcoords;

		std::vector<uint32_t> remap(indexCount);
		const size_t vertexCount = meshopt_generateVertexRemap(
			remap.data(), nullptr, indexCount, shape.mesh.indices.data(), indexCount, sizeof(rapidobj::Index));
		std::vector<rapidobj::Index> uniqueAttributes(vertexCount);
		meshopt_remapIndexBuffer(indices.data(), nullptr, indexCount, remap.data());
		meshopt_remapVertexBuffer(
			uniqueAttributes.data(), shape.mesh.indices.data(), indexCount, sizeof(rapidobj::Index), remap.data());

		positions.resize(vertexCount);
		packedNormals.resize(vertexCount);
		packedTexcoords.resize(vertexCount);
		const simd_float4x4 rotation = RotationY(rotationYRadians);
		for (size_t vertex = 0; vertex < vertexCount; ++vertex)
		{
			const rapidobj::Index& source = uniqueAttributes[vertex];
			ASSERT(source.position_index >= 0)
			const size_t positionOffset = static_cast<size_t>(source.position_index) * 3;
			simd_float3 position =
			{
				attributes.positions[positionOffset] * scale,
				attributes.positions[positionOffset + 1] * scale,
				attributes.positions[positionOffset + 2] * scale
			};

			position = simd_mul(rotation, simd_make_float4(position, 1.0f)).xyz;
			positions[vertex] = FromSIMD(position);
			result.min = simd_min(result.min, position);
			result.max = simd_max(result.max, position);

			simd_float3 normal = {};
			if (source.normal_index >= 0)
			{
				const size_t normalOffset = static_cast<size_t>(source.normal_index) * 3;
				normal =
				{
					attributes.normals[normalOffset],
					attributes.normals[normalOffset + 1],
					attributes.normals[normalOffset + 2]
				};

				normal = simd_normalize(simd_mul(rotation, simd_make_float4(normal, 0.0f)).xyz);
			}

			packedNormals[vertex].packedNormal = PackNormal(normal);

			Float2 texcoord;
			if (source.texcoord_index >= 0)
			{
				const size_t uvOffset = static_cast<size_t>(source.texcoord_index) * 2;
				texcoord = { attributes.texcoords[uvOffset], attributes.texcoords[uvOffset + 1] };
			}

			packedTexcoords[vertex].packedUV =
				(static_cast<uint32_t>(meshopt_quantizeHalf(texcoord.x)) << 16) |
				static_cast<uint32_t>(meshopt_quantizeHalf(texcoord.y));
		}

		meshopt_optimizeVertexCache(indices.data(), indices.data(), indexCount, vertexCount);
		const size_t maxMeshlets = meshopt_buildMeshletsBound(
			indexCount, MeshletMaxVertices, MeshletMaxTriangles);
		std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
		std::vector<uint32_t> meshletVertices(indexCount);
		std::vector<uint8_t> meshletTriangles(indexCount);
		const size_t meshletCount = meshopt_buildMeshlets(
			meshlets.data(),
			meshletVertices.data(),
			meshletTriangles.data(),
			indices.data(),
			indexCount,
			reinterpret_cast<const float*>(positions.data()),
			vertexCount,
			sizeof(Float3),
			MeshletMaxVertices,
			MeshletMaxTriangles,
			MeshletConeWeight);
		ASSERT(meshletCount > 0)
		meshlets.resize(meshletCount);

		result.indices.resize(indexCount);
		result.meshes.reserve(meshletCount);
		size_t outputIndexOffset = 0;
		for (const meshopt_Meshlet& meshlet : meshlets)
		{
			meshopt_optimizeMeshlet(
				meshletVertices.data() + meshlet.vertex_offset,
				meshletTriangles.data() + meshlet.triangle_offset,
				meshlet.triangle_count,
				meshlet.vertex_count);
			const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
				meshletVertices.data() + meshlet.vertex_offset,
				meshletTriangles.data() + meshlet.triangle_offset,
				meshlet.triangle_count,
				reinterpret_cast<const float*>(positions.data()),
				vertexCount,
				sizeof(Float3));

			MeshMeta mesh;
			std::memcpy(&mesh.aabb.center, bounds.center, sizeof(Float3));
			mesh.aabb.extents = { bounds.radius, bounds.radius, bounds.radius };
			mesh.indexCountPerInstance = meshlet.triangle_count * 3;
			mesh.instanceCount = 1;
			mesh.startIndexLocation = static_cast<uint32_t>(outputIndexOffset);
			std::memcpy(&mesh.coneApex, bounds.cone_apex, sizeof(Float3));
			std::memcpy(&mesh.coneAxis, bounds.cone_axis, sizeof(Float3));
			mesh.coneCutoff = bounds.cone_cutoff;
			result.meshes.push_back(mesh);

			const size_t meshletIndexCount = static_cast<size_t>(meshlet.triangle_count) * 3;
			for (size_t index = 0; index < meshletIndexCount; ++index)
			{
				result.indices[outputIndexOffset + index] =
					meshletVertices[meshlet.vertex_offset +
						meshletTriangles[meshlet.triangle_offset + index]];
			}

			outputIndexOffset += meshletIndexCount;
		}

		std::vector<uint32_t> vertexFetchRemap(vertexCount);
		const size_t finalVertexCount = meshopt_optimizeVertexFetchRemap(
			vertexFetchRemap.data(), result.indices.data(), result.indices.size(), vertexCount);
		result.positions.resize(finalVertexCount);
		result.normals.resize(finalVertexCount);
		result.texcoords.resize(finalVertexCount);
		meshopt_remapIndexBuffer(
			result.indices.data(), result.indices.data(), result.indices.size(), vertexFetchRemap.data());
		meshopt_remapVertexBuffer(
			result.positions.data(), positions.data(), vertexCount, sizeof(VertexPosition), vertexFetchRemap.data());
		meshopt_remapVertexBuffer(
			result.normals.data(), packedNormals.data(), vertexCount, sizeof(VertexNormal), vertexFetchRemap.data());
		meshopt_remapVertexBuffer(
			result.texcoords.data(), packedTexcoords.data(), vertexCount, sizeof(VertexUV), vertexFetchRemap.data());
		result.colors.assign(finalVertexCount, MakeDefaultColor());

		return result;
	}
}

Scene::Scene() :
	_resources(std::make_unique<Resources>())
{
}

Scene::~Scene() = default;

void Scene::_clear()
{
	_resources = std::make_unique<Resources>();
	_positions.clear();
	_normals.clear();
	_colors.clear();
	_texcoords.clear();
	_indices.clear();
	_indicesSOA.clear();
	_meshes.clear();
	_instances.clear();
	_prefabs.clear();
	_meshCount = 0;
	_instanceCount = 0;
	_totalFacesCount = 0;
	_trianglesCount = 0;
	_sceneAABB = {};
	_hasSceneBounds = false;
}

void Scene::Load(ScenesIndices kind)
{
	_clear();
	_kind = kind;
	if (kind == ScenesIndices::Buddha)
	{
		LoadBuddha();
	}
	else
	{
		LoadPlant();
	}

	_upload();
}

void Scene::LoadBuddha()
{
	camera.SetProjection(
		FOV * static_cast<float>(M_PI) / 180.0f,
		Settings::BackBufferAspectRatio,
		nearZ,
		farZ);
	camera.LookAt(
		simd_make_float3(-30.0f, 100.0f, -30.0f),
		simd_make_float3(100.0f, 0.0f, 100.0f),
		simd_make_float3(0.0f, 1.0f, 0.0f));

	lightDirection = { -1.0f, 1.0f, -1.0f };
	_loadObj(std::filesystem::path(Settings::AssetsPath) / "buddha" / "buddha.obj",
		50.0f,
		100.0f,
		10,
		10,
		0.0f);
}

void Scene::LoadPlant()
{
	camera.SetProjection(
		FOV * static_cast<float>(M_PI) / 180.0f,
		Settings::BackBufferAspectRatio,
		nearZ,
		farZ);
	camera.LookAt(
		simd_make_float3(-1000.0f, 500.0f, 600.0f),
		simd_make_float3(-999.0f, 500.0f, 600.0f),
		simd_make_float3(0.0f, 1.0f, 0.0f));

	lightDirection = { 1.0f, 1.0f, 1.0f };
	_loadObj(std::filesystem::path(Settings::AssetsPath) / "powerplant" / "powerplant.obj",
		0.0f,
		0.01f,
		3,
		1,
		static_cast<float>(M_PI_2));
}

void Scene::_loadObj(
	const std::filesystem::path& path,
	float translation,
	float scale,
	uint32_t instancesCountX,
	uint32_t instancesCountZ,
	float rotationYRadians)
{
	const auto startTime = std::chrono::steady_clock::now();
	rapidobj::Result parsed = rapidobj::ParseFile(path, rapidobj::MaterialLibrary::Ignore());
	ASSERT(!parsed.error, "OBJ parsing failed.")
	ASSERT(rapidobj::Triangulate(parsed), "OBJ triangulation failed.")

	std::vector<size_t> jobs;
	for (size_t shape = 0; shape < parsed.shapes.size(); ++shape)
	{
		if (!parsed.shapes[shape].mesh.indices.empty())
		{
			jobs.push_back(shape);
		}
	}

	ASSERT(!jobs.empty(), "OBJ contains no triangle meshes.")
	std::sort(jobs.begin(), jobs.end(), [&](size_t a, size_t b)
	{
		return parsed.shapes[a].mesh.indices.size() > parsed.shapes[b].mesh.indices.size();
	});

	std::vector<ProcessedOBJShape> processed(parsed.shapes.size());
	const size_t threadCount = std::min(
		{ jobs.size(), std::max<size_t>(1, std::thread::hardware_concurrency()), MaxOBJProcessingThreads });
	std::atomic_size_t nextJob = 0;
	auto worker = [&]()
	{
		for (;;)
		{
			const size_t job = nextJob.fetch_add(1);
			if (job >= jobs.size())
			{
				break;
			}

			const size_t shape = jobs[job];
			processed[shape] = ProcessOBJShape(
				parsed.shapes[shape], parsed.attributes, scale, rotationYRadians);
		}
	};

	std::vector<std::future<void>> workers;
	for (size_t thread = 1; thread < threadCount; ++thread)
	{
		workers.emplace_back(std::async(std::launch::async, worker));
	}

	worker();
	for (auto& future : workers)
	{
		future.get();
	}

	size_t vertexCount = 0;
	size_t indexCount = 0;
	size_t meshCount = 0;
	size_t faceCount = 0;
	simd_float3 objectMin = simd_make_float3(INFINITY);
	simd_float3 objectMax = simd_make_float3(-INFINITY);
	for (const auto& shape : processed)
	{
		vertexCount += shape.positions.size();
		indexCount += shape.indices.size();
		meshCount += shape.meshes.size();
		faceCount += shape.facesCount;
		if (!shape.positions.empty())
		{
			objectMin = simd_min(objectMin, shape.min);
			objectMax = simd_max(objectMax, shape.max);
		}
	}

	_positions.reserve(vertexCount);
	_normals.reserve(vertexCount);
	_colors.reserve(vertexCount);
	_texcoords.reserve(vertexCount);
	_indices.reserve(indexCount);
	_meshes.reserve(meshCount);

	for (ProcessedOBJShape& shape : processed)
	{
		if (shape.positions.empty())
		{
			continue;
		}

		ASSERT(_positions.size() <= static_cast<size_t>(INT32_MAX))
		ASSERT(_indices.size() <= static_cast<size_t>(UINT32_MAX))
		const int32_t baseVertex = static_cast<int32_t>(_positions.size());
		const uint32_t firstIndex = static_cast<uint32_t>(_indices.size());
		for (MeshMeta& mesh : shape.meshes)
		{
			mesh.startIndexLocation += firstIndex;
			mesh.baseVertexLocation = baseVertex;
			_meshes.push_back(mesh);
		}

		_positions.insert(_positions.end(), shape.positions.begin(), shape.positions.end());
		_normals.insert(_normals.end(), shape.normals.begin(), shape.normals.end());
		_colors.insert(_colors.end(), shape.colors.begin(), shape.colors.end());
		_texcoords.insert(_texcoords.end(), shape.texcoords.begin(), shape.texcoords.end());
		_indices.insert(_indices.end(), shape.indices.begin(), shape.indices.end());
	}

	AABB objectBounds;
	objectBounds.center = FromSIMD((objectMin + objectMax) * 0.5f);
	objectBounds.extents = FromSIMD((objectMax - objectMin) * 0.5f);
	Prefab prefab;
	prefab.meshesOffset = 0;
	prefab.meshesCount = static_cast<uint32_t>(_meshes.size());
	prefab.aabb = objectBounds;
	_prefabs.push_back(prefab);

	const uint32_t objectInstanceCount = instancesCountX * instancesCountZ;
	_instances.resize(_meshes.size() * objectInstanceCount);
	for (uint32_t meshIndex = 0; meshIndex < _meshes.size(); meshIndex++)
	{
		MeshMeta& mesh = _meshes[meshIndex];
		mesh.instanceCount = objectInstanceCount;
		mesh.startInstanceLocation = meshIndex * objectInstanceCount;
		for (uint32_t z = 0; z < instancesCountZ; z++)
		{
			for (uint32_t x = 0; x < instancesCountX; x++)
			{
				Instance& instance = _instances[mesh.startInstanceLocation + z * instancesCountX + x];
				instance.worldTransform = matrix_identity_float4x4;
				instance.worldTransform.columns[3].xyz =
				{
					(translation + objectBounds.extents.x * 2.0f) * x,
					0.0f,
					(translation + objectBounds.extents.z * 2.0f) * z
				};

				instance.meshID = meshIndex;
				instance.color =
				{
					static_cast<float>(meshIndex & 1),
					static_cast<float>(meshIndex & 3) / 4.0f,
					static_cast<float>(meshIndex & 7) / 8.0f
				};

				const AABB transformed = Utils::TransformAABB(objectBounds, instance.worldTransform);
				_sceneAABB = _hasSceneBounds ? Utils::MergeAABBs(_sceneAABB, transformed) : transformed;
				_hasSceneBounds = true;
			}
		}
	}

	_meshCount = _meshes.size();
	_instanceCount = _instances.size();
	_totalFacesCount = static_cast<uint64_t>(faceCount) * objectInstanceCount;
	MaxSceneFacesCount = std::max(MaxSceneFacesCount, static_cast<size_t>(_totalFacesCount));
	MaxSceneInstancesCount = std::max(MaxSceneInstancesCount, _instances.size());
	MaxSceneMeshesMetaCount = std::max(MaxSceneMeshesMetaCount, _meshes.size());
	_indicesSOA.resize(_indices.size());
	const uint32_t totalTrianglesCount = static_cast<uint32_t>(_indices.size() / 3);
	_trianglesCount = totalTrianglesCount;
	for (uint32_t triangle = 0; triangle < totalTrianglesCount; triangle++)
	{
		_indicesSOA[triangle] = _indices[triangle * 3];
		_indicesSOA[totalTrianglesCount + triangle] = _indices[triangle * 3 + 1];
		_indicesSOA[totalTrianglesCount * 2 + triangle] = _indices[triangle * 3 + 2];
	}

	const double seconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - startTime)
		.count();
	Utils::Log(
		"Loaded %s: %zu source triangles, %zu vertices, %zu meshlets, %zu instances in %.2f s\n",
		path.string().c_str(),
		faceCount,
		_positions.size(),
		_meshes.size(),
		_instances.size(),
		seconds);
}

void Scene::_upload()
{
	auto upload = [&](const void* bytes, size_t count, size_t stride, NSString* name)
	{
		return Context::CreateBuffer(bytes, count * stride, MTLResourceStorageModeShared, name);
	};

	_resources->positions = upload(_positions.data(), _positions.size(), sizeof(VertexPosition), @"Positions");
	_resources->normals = upload(_normals.data(), _normals.size(), sizeof(VertexNormal), @"Normals");
	_resources->colors = upload(_colors.data(), _colors.size(), sizeof(VertexColor), @"Colors");
	_resources->texcoords = upload(_texcoords.data(), _texcoords.size(), sizeof(VertexUV), @"Texcoords");
	_resources->indices = upload(_indices.data(), _indices.size(), sizeof(uint32_t), @"Indices");
	_resources->indicesSOA = upload(_indicesSOA.data(), _indicesSOA.size(), sizeof(uint32_t), @"SOA indices");
	_resources->meshes = upload(_meshes.data(), _meshes.size(), sizeof(MeshMeta), @"Mesh metadata");
	_resources->instances = upload(_instances.data(), _instances.size(), sizeof(Instance), @"Instances");

	std::vector<VertexPosition>().swap(_positions);
	std::vector<VertexNormal>().swap(_normals);
	std::vector<VertexColor>().swap(_colors);
	std::vector<VertexUV>().swap(_texcoords);
	std::vector<uint32_t>().swap(_indices);
	std::vector<uint32_t>().swap(_indicesSOA);
	std::vector<Instance>().swap(_instances);
}

id<MTLBuffer> Scene::GetPositionsBuffer() const
{
	return _resources->positions;
}

id<MTLBuffer> Scene::GetNormalsBuffer() const
{
	return _resources->normals;
}

id<MTLBuffer> Scene::GetColorsBuffer() const
{
	return _resources->colors;
}

id<MTLBuffer> Scene::GetTexcoordsBuffer() const
{
	return _resources->texcoords;
}

id<MTLBuffer> Scene::GetIndicesBuffer() const
{
	return _resources->indices;
}

id<MTLBuffer> Scene::GetIndicesSOABuffer() const
{
	return _resources->indicesSOA;
}

id<MTLBuffer> Scene::GetMeshesBuffer() const
{
	return _resources->meshes;
}

id<MTLBuffer> Scene::GetInstancesBuffer() const
{
	return _resources->instances;
}
