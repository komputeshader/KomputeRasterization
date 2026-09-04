#include "Scene.h"
#include "DX.h"
#include "DescriptorManager.h"
#include "CPUGPUCommon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <thread>

#include "meshoptimizer/src/meshoptimizer.h"
#include "rapidobj/include/rapidobj/rapidobj.hpp"

Scene* Scene::CurrentScene;
Scene Scene::PlantScene;
Scene Scene::BuddhaScene;
size_t Scene::MaxSceneFacesCount = 0;
size_t Scene::MaxSceneInstancesCount = 0;
size_t Scene::MaxSceneMeshesMetaCount = 0;

using namespace DirectX;

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
		std::vector<unsigned int> indices;
		std::vector<MeshMeta> meshes;
		XMFLOAT3 min = {};
		XMFLOAT3 max = {};
		size_t facesCount = 0;
	};

	static_assert(sizeof(rapidobj::Index) == sizeof(unsigned int) * 3);
	static_assert(sizeof(VertexPosition) == sizeof(XMFLOAT3));

	unsigned int PackNormal(const XMFLOAT3& normal)
	{
		return
			(meshopt_quantizeUnorm(normal.x * 0.5f + 0.5f, 10) << 20) |
			(meshopt_quantizeUnorm(normal.y * 0.5f + 0.5f, 10) << 10) |
			meshopt_quantizeUnorm(normal.z * 0.5f + 0.5f, 10);
	}

	VertexColor MakeDefaultColor()
	{
		VertexColor result = {};
		result.packedColor[0] =
			(static_cast<unsigned int>(meshopt_quantizeHalf(0.8f)) << 16) |
			static_cast<unsigned int>(meshopt_quantizeHalf(0.8f));
		result.packedColor[1] =
			(static_cast<unsigned int>(meshopt_quantizeHalf(0.8f)) << 16) |
			static_cast<unsigned int>(meshopt_quantizeHalf(1.0f));
		return result;
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

		std::vector<unsigned int> indices(indexCount);
		std::vector<XMFLOAT3> positions;
		std::vector<VertexNormal> packedNormals;
		std::vector<VertexUV> packedTexcoords;

		{
			const XMMATRIX rotation = XMMatrixRotationY(rotationYRadians);

			std::vector<unsigned int> remap(indexCount);
			const size_t vertexCount = meshopt_generateVertexRemap(
				remap.data(),
				nullptr,
				indexCount,
				shape.mesh.indices.data(),
				indexCount,
				sizeof(rapidobj::Index));

			std::vector<rapidobj::Index> uniqueAttributes(vertexCount);
			meshopt_remapIndexBuffer(indices.data(), nullptr, indexCount, remap.data());
			meshopt_remapVertexBuffer(
				uniqueAttributes.data(),
				shape.mesh.indices.data(),
				indexCount,
				sizeof(rapidobj::Index),
				remap.data());

			positions.resize(vertexCount);
			packedNormals.resize(vertexCount);
			packedTexcoords.resize(vertexCount);

			XMVECTOR shapeMin = g_XMFltMax.v;
			XMVECTOR shapeMax = -g_XMFltMax.v;
			for (size_t vertex = 0; vertex < vertexCount; ++vertex)
			{
				const rapidobj::Index& source = uniqueAttributes[vertex];

				ASSERT(source.position_index >= 0)
				const size_t positionOffset = static_cast<size_t>(source.position_index) * 3;
				XMFLOAT3 position =
				{
					attributes.positions[positionOffset + 0] * scale,
					attributes.positions[positionOffset + 1] * scale,
					attributes.positions[positionOffset + 2] * scale
				};
				XMStoreFloat3(
					&position,
					XMVector3TransformCoord(XMLoadFloat3(&position), rotation));
				positions[vertex] = position;
				shapeMin = XMVectorMin(shapeMin, XMLoadFloat3(&position));
				shapeMax = XMVectorMax(shapeMax, XMLoadFloat3(&position));

				XMFLOAT3 normal = {};
				if (source.normal_index >= 0)
				{
					const size_t normalOffset = static_cast<size_t>(source.normal_index) * 3;
					normal =
					{
						attributes.normals[normalOffset + 0],
						attributes.normals[normalOffset + 1],
						attributes.normals[normalOffset + 2]
					};
					XMStoreFloat3(
						&normal,
						XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&normal), rotation)));
				}
				packedNormals[vertex].packedNormal = PackNormal(normal);

				XMFLOAT2 texcoord = {};
				if (source.texcoord_index >= 0)
				{
					const size_t texcoordOffset = static_cast<size_t>(source.texcoord_index) * 2;
					texcoord =
					{
						attributes.texcoords[texcoordOffset + 0],
						attributes.texcoords[texcoordOffset + 1]
					};
				}
				packedTexcoords[vertex].packedUV =
					(static_cast<unsigned int>(meshopt_quantizeHalf(texcoord.x)) << 16) |
					static_cast<unsigned int>(meshopt_quantizeHalf(texcoord.y));
			}

			XMStoreFloat3(&result.min, shapeMin);
			XMStoreFloat3(&result.max, shapeMax);
		}

		const size_t vertexCount = positions.size();
		meshopt_optimizeVertexCache(
			indices.data(),
			indices.data(),
			indexCount,
			vertexCount);

		const size_t maxMeshlets = meshopt_buildMeshletsBound(
			indexCount,
			MeshletMaxVertices,
			MeshletMaxTriangles);
		std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
		std::vector<unsigned int> meshletVertices(indexCount);
		std::vector<unsigned char> meshletTriangles(indexCount);

		const size_t meshletCount = meshopt_buildMeshlets(
			meshlets.data(),
			meshletVertices.data(),
			meshletTriangles.data(),
			indices.data(),
			indexCount,
			reinterpret_cast<const float*>(positions.data()),
			vertexCount,
			sizeof(XMFLOAT3),
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
				sizeof(XMFLOAT3));

			MeshMeta mesh = {};
			memcpy(&mesh.AABB.center, bounds.center, sizeof(mesh.AABB.center));
			mesh.AABB.extents = { bounds.radius, bounds.radius, bounds.radius };
			mesh.indexCountPerInstance = meshlet.triangle_count * 3;
			mesh.instanceCount = 1;
			mesh.startIndexLocation = static_cast<unsigned int>(outputIndexOffset);
			mesh.baseVertexLocation = 0;
			mesh.startInstanceLocation = 0;
			memcpy(&mesh.coneApex, bounds.cone_apex, sizeof(mesh.coneApex));
			memcpy(&mesh.coneAxis, bounds.cone_axis, sizeof(mesh.coneAxis));
			mesh.coneCutoff = bounds.cone_cutoff;
			result.meshes.push_back(mesh);

			const size_t meshletIndexCount = static_cast<size_t>(meshlet.triangle_count) * 3;
			for (size_t index = 0; index < meshletIndexCount; ++index)
			{
				result.indices[outputIndexOffset + index] =
					meshletVertices[meshlet.vertex_offset + meshletTriangles[meshlet.triangle_offset + index]];
			}
			outputIndexOffset += meshletIndexCount;
		}
		ASSERT(outputIndexOffset == indexCount)

		std::vector<unsigned int> vertexFetchRemap(vertexCount);
		const size_t finalVertexCount = meshopt_optimizeVertexFetchRemap(
			vertexFetchRemap.data(),
			result.indices.data(),
			result.indices.size(),
			vertexCount);
		ASSERT(finalVertexCount == vertexCount)

		result.positions.resize(finalVertexCount);
		result.normals.resize(finalVertexCount);
		result.texcoords.resize(finalVertexCount);
		meshopt_remapIndexBuffer(
			result.indices.data(),
			result.indices.data(),
			result.indices.size(),
			vertexFetchRemap.data());
		meshopt_remapVertexBuffer(
			result.positions.data(),
			positions.data(),
			vertexCount,
			sizeof(VertexPosition),
			vertexFetchRemap.data());
		meshopt_remapVertexBuffer(
			result.normals.data(),
			packedNormals.data(),
			vertexCount,
			sizeof(VertexNormal),
			vertexFetchRemap.data());
		meshopt_remapVertexBuffer(
			result.texcoords.data(),
			packedTexcoords.data(),
			vertexCount,
			sizeof(VertexUV),
			vertexFetchRemap.data());
		result.colors.assign(finalVertexCount, MakeDefaultColor());

		return result;
	}
}

void Scene::LoadBuddha()
{
	CurrentScene = this;

	XMVECTOR sceneMin = g_XMFltMax.v;
	XMVECTOR sceneMax = -g_XMFltMax.v;
	XMStoreFloat3(&sceneAABB.center, (sceneMin + sceneMax) * 0.5f);
	XMStoreFloat3(&sceneAABB.extents, (sceneMax - sceneMin) * 0.5f);

	camera.SetProjection(
		XMConvertToRadians(FOV),
		Settings::BackBufferAspectRatio,
		nearZ,
		farZ);

	camera.LookAt(
		XMVectorSet(-30.0f, 100.0f, -30.0f, 0.0f),
		XMVectorSet(100.0f, 0.0f, 100.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
	);

	lightDirection = { -1.0f, 1.0f, -1.0f };

	_loadObj("Buddha//buddha.obj", 50.0f, 100.0f, 10, 10);

	_createVBResources(Buddha);
	_createIBResources(Buddha);
	_createMeshMetaResources(Buddha);
	_createInstancesBufferResources(Buddha);

	MaxSceneFacesCount = std::max(MaxSceneFacesCount, totalFacesCount);
	MaxSceneInstancesCount = std::max(MaxSceneInstancesCount, instancesCPU.size());
	MaxSceneMeshesMetaCount = std::max(MaxSceneMeshesMetaCount, meshesMetaCPU.size());
}

void Scene::LoadPlant()
{
	CurrentScene = this;

	XMVECTOR sceneMin = g_XMFltMax.v;
	XMVECTOR sceneMax = -g_XMFltMax.v;
	XMStoreFloat3(&sceneAABB.center, (sceneMin + sceneMax) * 0.5f);
	XMStoreFloat3(&sceneAABB.extents, (sceneMax - sceneMin) * 0.5f);

	camera.SetProjection(
		XMConvertToRadians(FOV),
		Settings::BackBufferAspectRatio,
		nearZ,
		farZ);

	camera.LookAt(
		XMVectorSet(-1000.0f, 500.0f, 600.0f, 0.0f),
		XMVectorSet(-999.0f, 500.0f, 600.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
	);

	lightDirection = { 1.0f, 1.0f, 1.0f };

	_loadObj("powerplant//powerplant.obj", 0.0f, 0.01f, 3, 1, XM_PIDIV2);

	_createVBResources(Plant);
	_createIBResources(Plant);
	_createMeshMetaResources(Plant);
	_createInstancesBufferResources(Plant);

	MaxSceneFacesCount = std::max(MaxSceneFacesCount, totalFacesCount);
	MaxSceneInstancesCount = std::max(MaxSceneInstancesCount, instancesCPU.size());
	MaxSceneMeshesMetaCount = std::max(MaxSceneMeshesMetaCount, meshesMetaCPU.size());
}

void Scene::_loadObj(
	const std::string& OBJPath,
	float translation,
	float scale,
	unsigned int instancesCountX,
	unsigned int instancesCountZ,
	float rotationYRadians)
{
	const auto loadStart = std::chrono::steady_clock::now();
	XMVECTOR objectMin = g_XMFltMax.v;
	XMVECTOR objectMax = -g_XMFltMax.v;
	std::vector<MeshMeta> meshesMeta;
	size_t facesCount = 0;
	size_t loadedVertexCount = 0;
	size_t processingThreadCount = 0;

	{
		rapidobj::Result OBJResult = rapidobj::ParseFile(
			OBJPath,
			rapidobj::MaterialLibrary::Ignore());
		if (OBJResult.error)
		{
			PrintToOutput(
				"Error loading %s: %s (line %zu: %s)\n",
				OBJPath.c_str(),
				OBJResult.error.code.message().c_str(),
				OBJResult.error.line_num,
				OBJResult.error.line.c_str());
			ASSERT(false, "OBJ parsing failed.")
			return;
		}

		if (!rapidobj::Triangulate(OBJResult))
		{
			PrintToOutput(
				"Error triangulating %s: %s\n",
				OBJPath.c_str(),
				OBJResult.error.code.message().c_str());
			ASSERT(false, "OBJ triangulation failed.")
			return;
		}

		std::vector<size_t> shapeJobs;
		shapeJobs.reserve(OBJResult.shapes.size());
		for (size_t shape = 0; shape < OBJResult.shapes.size(); ++shape)
		{
			if (!OBJResult.shapes[shape].mesh.indices.empty())
			{
				shapeJobs.push_back(shape);
			}
		}

		if (shapeJobs.empty())
		{
			PrintToOutput("Error loading %s: OBJ contains no triangle meshes\n", OBJPath.c_str());
			ASSERT(false, "OBJ contains no triangle meshes.")
			return;
		}

		std::sort(
			shapeJobs.begin(),
			shapeJobs.end(),
			[&OBJResult](size_t lhs, size_t rhs)
			{
				return OBJResult.shapes[lhs].mesh.indices.size() >
					OBJResult.shapes[rhs].mesh.indices.size();
			});

		std::vector<ProcessedOBJShape> processedShapes(OBJResult.shapes.size());
		const size_t hardwareThreads = std::max<size_t>(1, std::thread::hardware_concurrency());
		processingThreadCount = std::min({ shapeJobs.size(), hardwareThreads, MaxOBJProcessingThreads });
		std::atomic_size_t nextJob = 0;

		auto processShapes = [&]()
		{
			for (;;)
			{
				const size_t job = nextJob.fetch_add(1, std::memory_order_relaxed);
				if (job >= shapeJobs.size())
				{
					break;
				}

				const size_t shapeIndex = shapeJobs[job];
				processedShapes[shapeIndex] = ProcessOBJShape(
					OBJResult.shapes[shapeIndex],
					OBJResult.attributes,
					scale,
					rotationYRadians);
			}
		};

		if (processingThreadCount == 1)
		{
			processShapes();
		}
		else
		{
			std::vector<std::future<void>> workers;
			workers.reserve(processingThreadCount);
			for (size_t worker = 0; worker < processingThreadCount; ++worker)
			{
				workers.push_back(std::async(std::launch::async, processShapes));
			}
			for (std::future<void>& worker : workers)
			{
				worker.get();
			}
		}

		size_t newVertexCount = 0;
		size_t newIndexCount = 0;
		size_t newMeshCount = 0;
		for (const ProcessedOBJShape& shape : processedShapes)
		{
			newVertexCount += shape.positions.size();
			newIndexCount += shape.indices.size();
			newMeshCount += shape.meshes.size();
		}
		loadedVertexCount = newVertexCount;

		positionsCPU.reserve(positionsCPU.size() + newVertexCount);
		normalsCPU.reserve(normalsCPU.size() + newVertexCount);
		colorsCPU.reserve(colorsCPU.size() + newVertexCount);
		texcoordsCPU.reserve(texcoordsCPU.size() + newVertexCount);
		indicesCPU.reserve(indicesCPU.size() + newIndexCount);
		meshesMeta.reserve(newMeshCount);

		for (ProcessedOBJShape& shape : processedShapes)
		{
			if (shape.positions.empty())
			{
				continue;
			}

			ASSERT(positionsCPU.size() <= static_cast<size_t>(std::numeric_limits<int>::max()))
			ASSERT(indicesCPU.size() <= static_cast<size_t>(std::numeric_limits<unsigned int>::max()))
			const int baseVertexLocation = static_cast<int>(positionsCPU.size());
			const unsigned int startIndexLocation = static_cast<unsigned int>(indicesCPU.size());

			for (MeshMeta& mesh : shape.meshes)
			{
				mesh.startIndexLocation += startIndexLocation;
				mesh.baseVertexLocation = baseVertexLocation;
				meshesMeta.push_back(mesh);
			}

			positionsCPU.insert(positionsCPU.end(), shape.positions.begin(), shape.positions.end());
			normalsCPU.insert(normalsCPU.end(), shape.normals.begin(), shape.normals.end());
			colorsCPU.insert(colorsCPU.end(), shape.colors.begin(), shape.colors.end());
			texcoordsCPU.insert(texcoordsCPU.end(), shape.texcoords.begin(), shape.texcoords.end());
			indicesCPU.insert(indicesCPU.end(), shape.indices.begin(), shape.indices.end());

			objectMin = XMVectorMin(objectMin, XMLoadFloat3(&shape.min));
			objectMax = XMVectorMax(objectMax, XMLoadFloat3(&shape.max));
			facesCount += shape.facesCount;
		}
	}

	const double loadSeconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - loadStart).count();
	PrintToOutput(
		"Parsed and processed %s: %zu triangles, %zu vertices, %zu meshlets on %zu thread(s) in %.3f s\n",
		OBJPath.c_str(),
		facesCount,
		loadedVertexCount,
		meshesMeta.size(),
		processingThreadCount,
		loadSeconds);

#ifdef GPU_SOA_BUFFERS
	indicesSOACPU.resize(indicesCPU.size());
	unsigned int totalTrianglesCount = static_cast<unsigned int>(indicesCPU.size() / 3);
	for (unsigned int triangle = 0; triangle < totalTrianglesCount; triangle++)
	{
		indicesSOACPU[0 * totalTrianglesCount + triangle] = indicesCPU[triangle * 3 + 0];
		indicesSOACPU[1 * totalTrianglesCount + triangle] = indicesCPU[triangle * 3 + 1];
		indicesSOACPU[2 * totalTrianglesCount + triangle] = indicesCPU[triangle * 3 + 2];
	}
#endif

	AABB objectBoundingVolume;
	XMStoreFloat3(&objectBoundingVolume.center, (objectMin + objectMax) * 0.5f);
	XMStoreFloat3(&objectBoundingVolume.extents, (objectMax - objectMin) * 0.5f);

	Prefab newPrefab;
	newPrefab.meshesOffset = static_cast<unsigned int>(meshesMetaCPU.size());
	newPrefab.meshesCount = static_cast<unsigned int>(meshesMeta.size());
	prefabs.push_back(newPrefab);

	meshesMetaCPU.insert(meshesMetaCPU.end(), meshesMeta.begin(), meshesMeta.end());

	// generate instances

	const unsigned int totalMeshInstances = instancesCountX * instancesCountZ;

	totalFacesCount += facesCount * totalMeshInstances;

	unsigned int newInstancesOffset = static_cast<unsigned int>(instancesCPU.size());
	instancesCPU.resize(instancesCPU.size() + newPrefab.meshesCount * instancesCountX * instancesCountZ);
	for (unsigned int mesh = 0; mesh < newPrefab.meshesCount; mesh++)
	{
		unsigned int meshIndex = newPrefab.meshesOffset + mesh;
		auto& currentMesh = meshesMetaCPU[meshIndex];
		currentMesh.instanceCount = totalMeshInstances;
		currentMesh.startInstanceLocation = newInstancesOffset + mesh * totalMeshInstances;
		for (unsigned int instanceZ = 0; instanceZ < instancesCountZ; instanceZ++)
		{
			for (unsigned int instanceX = 0; instanceX < instancesCountX; instanceX++)
			{
				XMMATRIX transform = XMMatrixTranslation(
					(translation + objectBoundingVolume.extents.x * 2.0f) *
					instanceX,
					0.0f,
					(translation + objectBoundingVolume.extents.z * 2.0f) *
					instanceZ);

				Instance& instance = instancesCPU[currentMesh.startInstanceLocation + instanceZ * instancesCountX + instanceX];
				XMStoreFloat4x4(&instance.worldTransform, transform);
				instance.meshID = meshIndex;
				instance.color =
				{
					static_cast<float>(meshIndex & 1),
					static_cast<float>(meshIndex & 3) / 4,
					static_cast<float>(meshIndex & 7) / 8
				};

				sceneAABB = Utils::MergeAABBs(sceneAABB, Utils::TransformAABB(objectBoundingVolume, transform));
			}
		}
	}
}

void Scene::_createVBResources(ScenesIndices sceneIndex)
{
	positionsGPU.Initialize(
		COMMAND_LIST.Get(),
		positionsCPU.data(),
		positionsCPU.size(),
		sizeof(decltype(positionsCPU)::value_type),
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		VertexPositionsSRV + sceneIndex,
		L"VertexPositions");

	normalsGPU.Initialize(
		COMMAND_LIST.Get(),
		normalsCPU.data(),
		normalsCPU.size(),
		sizeof(decltype(normalsCPU)::value_type),
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		VertexNormalsSRV + sceneIndex,
		L"VertexNormals");

	colorsGPU.Initialize(
		COMMAND_LIST.Get(),
		colorsCPU.data(),
		colorsCPU.size(),
		sizeof(decltype(colorsCPU)::value_type),
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		VertexColorsSRV + sceneIndex,
		L"VertexColors");

	texcoordsGPU.Initialize(
		COMMAND_LIST.Get(),
		texcoordsCPU.data(),
		texcoordsCPU.size(),
		sizeof(decltype(texcoordsCPU)::value_type),
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		VertexTexcoordsSRV + sceneIndex,
		L"VertexTexcoords");
}

void Scene::_createIBResources(ScenesIndices sceneIndex)
{
	indicesGPU.Initialize(
		COMMAND_LIST.Get(),
		indicesCPU.data(),
		indicesCPU.size(),
		sizeof(decltype(indicesCPU)::value_type),
		D3D12_RESOURCE_STATE_INDEX_BUFFER,
		IndicesSRV + sceneIndex,
		L"Indices");

#ifdef GPU_SOA_BUFFERS
	indicesSOAGPU.Initialize(
		COMMAND_LIST.Get(),
		indicesSOACPU.data(),
		indicesSOACPU.size(),
		sizeof(decltype(indicesSOACPU)::value_type),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		IndicesSOASRV + sceneIndex,
		L"IndicesSOA");
#endif
}

void Scene::_createMeshMetaResources(ScenesIndices sceneIndex)
{
	meshesMetaGPU.Initialize(
		COMMAND_LIST.Get(),
		meshesMetaCPU.data(),
		meshesMetaCPU.size(),
		sizeof(decltype(meshesMetaCPU)::value_type),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		MeshesMetaSRV + sceneIndex,
		L"MeshesMeta");
}

void Scene::_createInstancesBufferResources(ScenesIndices sceneIndex)
{
	instancesGPU.Initialize(
		COMMAND_LIST.Get(),
		instancesCPU.data(),
		instancesCPU.size(),
		sizeof(decltype(instancesCPU)::value_type),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		InstancesSRV + sceneIndex,
		L"Instances");
}
