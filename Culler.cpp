#include "Culler.h"
#include "Utils.h"
#include "Types.h"
#include "Scene.h"
#include "DX.h"
#include "DescriptorManager.h"
#include "Shadows.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct CullingCB
{
	unsigned int maxSceneInstancesCount;
	unsigned int maxSceneMeshesMetaCount;
	unsigned int totalInstancesCount;
	unsigned int totalMeshesCount;
	unsigned int cascadesCount;
	unsigned int frustumCullingEnabled;
	unsigned int cameraHiZCullingEnabled;
	unsigned int shadowsHiZCullingEnabled;
	unsigned int clusterBackfaceCullingEnabled;
	unsigned int pad0[3];
	XMFLOAT2 depthResolution;
	XMFLOAT2 shadowMapResolution;
	XMFLOAT4 cameraPosition;
	XMFLOAT4 lightDirection;
	XMFLOAT4 cascadeCameraPosition[MAX_CASCADES_COUNT];
	Frustum camera;
	Frustum cascade[MAX_CASCADES_COUNT];
	XMFLOAT4X4 prevFrameCameraVP;
	XMFLOAT4X4 prevFrameCascadeVP[MAX_CASCADES_COUNT];
};
static_assert(
	(sizeof(CullingCB) % 256) == 0,
	"Constant Buffer size must be 256-byte aligned");

Culler::Culler()
{
	_createClearPSO();
	_createCullingPSO();
	_createGenerateCommandsPSO();
	_createCullingCounters();

	Utils::CreateCBResources(
		sizeof(CullingCB) * DX::FramesCount,
		reinterpret_cast<void**>(&_cullingCBData),
		_cullingCB);

	// Allocate a buffer that can be used to reset the UAV counters and
	// initialize it to 0.
	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT));
	SUCCESS(DX::Device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&_culledCommandsCounterReset)));
	NAME_D3D12_OBJECT(_culledCommandsCounterReset);

	UINT8* pMappedCounterReset = nullptr;
	// We do not intend to read from this resource on the CPU.
	CD3DX12_RANGE readRange(0, 0);
	SUCCESS(_culledCommandsCounterReset->Map(
		0,
		&readRange,
		reinterpret_cast<void**>(&pMappedCounterReset)));
	ZeroMemory(pMappedCounterReset, sizeof(UINT));
	_culledCommandsCounterReset->Unmap(0, nullptr);
}

void Culler::Update()
{
	Camera& camera = Scene::CurrentScene->camera;
	CullingCB cullingData = {};
	cullingData.maxSceneInstancesCount = Scene::MaxSceneInstancesCount;
	cullingData.maxSceneMeshesMetaCount = Scene::MaxSceneMeshesMetaCount;
	cullingData.totalInstancesCount = Scene::CurrentScene->instancesCPU.size();
	cullingData.totalMeshesCount = Scene::CurrentScene->meshesMetaCPU.size();
	cullingData.cascadesCount = Settings::CascadesCount;
	cullingData.frustumCullingEnabled = Settings::FrustumCullingEnabled ? 1 : 0;
	cullingData.cameraHiZCullingEnabled = Settings::CameraHiZCullingEnabled ? 1 : 0;
	cullingData.shadowsHiZCullingEnabled = Settings::ShadowsHiZCullingEnabled ? 1 : 0;
	cullingData.clusterBackfaceCullingEnabled = Settings::ClusterBackfaceCullingEnabled ? 1 : 0;
	cullingData.depthResolution =
	{
		static_cast<float>(Settings::BackBufferWidth),
		static_cast<float>(Settings::BackBufferHeight)
	};
	cullingData.shadowMapResolution =
	{
		static_cast<float>(Settings::ShadowMapRes),
		static_cast<float>(Settings::ShadowMapRes)
	};
	const XMFLOAT3& cameraPosition = camera.GetPosition();
	memcpy(&cullingData.cameraPosition, &cameraPosition, sizeof(cameraPosition));
	XMStoreFloat4(
		&cullingData.lightDirection,
		XMVector3Normalize(XMLoadFloat3(&Scene::CurrentScene->lightDirection)));
	cullingData.camera = camera.GetFrustum();
	cullingData.prevFrameCameraVP = camera.GetPrevFrameVP();

	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		cullingData.cascadeCameraPosition[cascade] = Shadows::Sun.GetCascadeCameraPosition(cascade);
		cullingData.cascade[cascade] = Shadows::Sun.GetCascadeFrustum(cascade);
		cullingData.prevFrameCascadeVP[cascade] = Shadows::Sun.GetPrevFrameCascadeVP(cascade);
	}

	memcpy(
		_cullingCBData + DX::FrameIndex * sizeof(CullingCB),
		&cullingData,
		sizeof(CullingCB));
}

void Culler::Cull(
	ID3D12GraphicsCommandList* commandList,
	ComPtr<ID3D12Resource> visibleInstances,
	ComPtr<ID3D12Resource>* culledCommands,
	ComPtr<ID3D12Resource>* culledCommandsCounters)
{
	PIXBeginEvent(commandList, 0, L"Culling");

	CD3DX12_RESOURCE_BARRIER barriers[2 + 2 * MAX_FRUSTUMS_COUNT] = {};
	for (int frustum = 0; frustum < MAX_FRUSTUMS_COUNT; frustum++)
	{
		barriers[frustum] = CD3DX12_RESOURCE_BARRIER::Transition(
			culledCommandsCounters[frustum].Get(),
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
			D3D12_RESOURCE_STATE_COPY_DEST);
	}
	commandList->ResourceBarrier(MAX_FRUSTUMS_COUNT, barriers);

	// reset the UAV counters for this frame
	for (int frustum = 0; frustum < MAX_FRUSTUMS_COUNT; frustum++)
	{
		commandList->CopyBufferRegion(
			culledCommandsCounters[frustum].Get(),
			0,
			_culledCommandsCounterReset.Get(),
			0,
			sizeof(UINT));
	}

	D3D12_GPU_VIRTUAL_ADDRESS cbAdress = _cullingCB->GetGPUVirtualAddress() + DX::FrameIndex * sizeof(CullingCB);

	for (int frustum = 0; frustum < MAX_FRUSTUMS_COUNT; frustum++)
	{
		barriers[2 * frustum] = CD3DX12_RESOURCE_BARRIER::Transition(
			culledCommandsCounters[frustum].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		barriers[2 * frustum + 1] =
			CD3DX12_RESOURCE_BARRIER::Transition(
				culledCommands[frustum].Get(),
				Settings::SWREnabled
				? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
				: D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	barriers[_countof(barriers) - 2] =
		CD3DX12_RESOURCE_BARRIER::Transition(
			visibleInstances.Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barriers[_countof(barriers) - 1] =
		CD3DX12_RESOURCE_BARRIER::Transition(
			_cullingCounters.Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	commandList->ResourceBarrier(_countof(barriers), barriers);

	// clear
	commandList->SetComputeRootSignature(_clearRS.Get());
	commandList->SetPipelineState(_clearPSO.Get());
	commandList->SetComputeRootConstantBufferView(
		0, cbAdress);
	commandList->SetComputeRootDescriptorTable(
		1, Descriptors::SV.GetGPUHandle(CullingCountersUAV));
	commandList->Dispatch(
		Utils::DispatchSize(CULLING_THREADS_X, Scene::CurrentScene->meshesMetaCPU.size()),
		1,
		1);

	// culling
	commandList->SetComputeRootSignature(_cullingRS.Get());
	commandList->SetPipelineState(_cullingPSO.Get());
	commandList->SetComputeRootConstantBufferView(
		0, cbAdress);
	commandList->SetComputeRootDescriptorTable(
		1, Scene::CurrentScene->meshesMetaGPU.GetSRV());
	commandList->SetComputeRootDescriptorTable(
		2, Scene::CurrentScene->instancesGPU.GetSRV());
	commandList->SetComputeRootDescriptorTable(
		3, Descriptors::SV.GetGPUHandle(PrevFrameDepthSRV));
	commandList->SetComputeRootDescriptorTable(
		4, Descriptors::SV.GetGPUHandle(PrevFrameShadowMapSRV));
	commandList->SetComputeRootDescriptorTable(
		5, Descriptors::SV.GetGPUHandle(VisibleInstancesUAV + DX::FrameIndex * PerFrameDescriptorsCount));
	commandList->SetComputeRootDescriptorTable(
		6, Descriptors::SV.GetGPUHandle(CullingCountersUAV));
	commandList->Dispatch(
		Utils::DispatchSize(CULLING_THREADS_X, Scene::CurrentScene->instancesCPU.size()),
		1,
		1);

	// gererate commands
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		visibleInstances.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		_cullingCounters.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	commandList->ResourceBarrier(2, barriers);

	commandList->SetComputeRootSignature(_generateHWRCommandsRS.Get());
	commandList->SetPipelineState(_generateHWRCommandsPSO.Get());
	commandList->SetComputeRootConstantBufferView(
		0, cbAdress);
	commandList->SetComputeRootDescriptorTable(
		1, Scene::CurrentScene->meshesMetaGPU.GetSRV());
	commandList->SetComputeRootDescriptorTable(
		2, Descriptors::SV.GetGPUHandle(CullingCountersSRV));
	commandList->SetComputeRootDescriptorTable(
		3, Descriptors::SV.GetGPUHandle(CulledCommandsUAV + DX::FrameIndex * PerFrameDescriptorsCount));
	commandList->Dispatch(
		Utils::DispatchSize(CULLING_THREADS_X, Scene::CurrentScene->meshesMetaCPU.size()),
		1,
		1);

	for (int frustum = 0; frustum < MAX_FRUSTUMS_COUNT; frustum++)
	{
		barriers[2 * frustum] = CD3DX12_RESOURCE_BARRIER::Transition(
			culledCommands[frustum].Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			Settings::SWREnabled
			? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
			: D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
		barriers[2 * frustum + 1] =
			CD3DX12_RESOURCE_BARRIER::Transition(
				culledCommandsCounters[frustum].Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	}
	commandList->ResourceBarrier(2 * MAX_FRUSTUMS_COUNT, barriers);

	PIXEndEvent(commandList);
}

void Culler::_createCullingCounters()
{
	// buffers with counters for culling
	UINT64 bufferSize = Scene::MaxSceneMeshesMetaCount * sizeof(UINT) * MAX_FRUSTUMS_COUNT;
	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto desc = CD3DX12_RESOURCE_DESC::Buffer(
		bufferSize,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
	UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
	UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	UAVDesc.Buffer.FirstElement = 0;
	UAVDesc.Buffer.CounterOffsetInBytes = 0;
	UAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	UAVDesc.Buffer.NumElements = Scene::MaxSceneMeshesMetaCount * MAX_FRUSTUMS_COUNT;
	UAVDesc.Buffer.StructureByteStride = sizeof(UINT);

	D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	SRVDesc.Buffer.FirstElement = 0;
	SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	SRVDesc.Buffer.NumElements = Scene::MaxSceneMeshesMetaCount * MAX_FRUSTUMS_COUNT;
	SRVDesc.Buffer.StructureByteStride = sizeof(UINT);

	SUCCESS(DX::Device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		nullptr,
		IID_PPV_ARGS(&_cullingCounters)));
	NAME_D3D12_OBJECT(_cullingCounters);

	DX::Device->CreateUnorderedAccessView(
		_cullingCounters.Get(),
		nullptr,
		&UAVDesc,
		Descriptors::SV.GetCPUHandle(CullingCountersUAV));

	DX::Device->CreateShaderResourceView(
		_cullingCounters.Get(),
		&SRVDesc,
		Descriptors::SV.GetCPUHandle(CullingCountersSRV));
}

void Culler::_createClearPSO()
{
	CD3DX12_ROOT_PARAMETER1 computeRootParameters[2] = {};
	computeRootParameters[0].InitAsConstantBufferView(0);
	CD3DX12_DESCRIPTOR_RANGE1 ranges[1] = {};
	ranges[0].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		0);
	computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
	computeRootSignatureDesc.Init_1_1(_countof(computeRootParameters), computeRootParameters);

	Utils::CreateRS(computeRootSignatureDesc, _clearRS);
	NAME_D3D12_OBJECT(_clearRS);

	Utils::ShaderHelper computeShader(Utils::GetAssetFullPath(L"ClearCS.cso").c_str());

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = _clearRS.Get();
	psoDesc.CS = { computeShader.GetData(), computeShader.GetSize() };

	SUCCESS(DX::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_clearPSO)));
	NAME_D3D12_OBJECT(_clearPSO);
}

void Culler::_createCullingPSO()
{
	CD3DX12_ROOT_PARAMETER1 computeRootParameters[7] = {};
	computeRootParameters[0].InitAsConstantBufferView(0);
	CD3DX12_DESCRIPTOR_RANGE1 ranges[6] = {};
	ranges[0].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		0,
		0,
		D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
	computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);
	ranges[1].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		1);
	computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);
	ranges[2].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		2);
	computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);
	ranges[3].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		MAX_CASCADES_COUNT,
		3);
	computeRootParameters[4].InitAsDescriptorTable(1, &ranges[3]);
	ranges[4].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		0);
	computeRootParameters[5].InitAsDescriptorTable(1, &ranges[4]);
	ranges[5].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		1);
	computeRootParameters[6].InitAsDescriptorTable(1, &ranges[5]);

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
	computeRootSignatureDesc.Init_1_1(
		_countof(computeRootParameters),
		computeRootParameters,
		1,
		&Utils::HiZSamplerDesc);

	Utils::CreateRS(
		computeRootSignatureDesc,
		_cullingRS);
	NAME_D3D12_OBJECT(_cullingRS);

	Utils::ShaderHelper computeShader(Utils::GetAssetFullPath(L"CullingCS.cso").c_str());

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = _cullingRS.Get();
	psoDesc.CS = { computeShader.GetData(), computeShader.GetSize() };

	SUCCESS(DX::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_cullingPSO)));
	NAME_D3D12_OBJECT(_cullingPSO);
}

void Culler::_createGenerateCommandsPSO()
{
	CD3DX12_ROOT_PARAMETER1 computeRootParameters[4] = {};
	computeRootParameters[0].InitAsConstantBufferView(0);
	CD3DX12_DESCRIPTOR_RANGE1 ranges[3] = {};
	ranges[0].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		0,
		0,
		D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
	computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);
	ranges[1].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		1);
	computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);
	ranges[2].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		MAX_FRUSTUMS_COUNT,
		0);
	computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
	computeRootSignatureDesc.Init_1_1(
		_countof(computeRootParameters),
		computeRootParameters);

	Utils::CreateRS(
		computeRootSignatureDesc,
		_generateHWRCommandsRS);
	NAME_D3D12_OBJECT(_generateHWRCommandsRS);

	Utils::ShaderHelper computeShader(Utils::GetAssetFullPath(L"GenerateCommandsCS.cso").c_str());

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = _generateHWRCommandsRS.Get();
	psoDesc.CS = { computeShader.GetData(), computeShader.GetSize() };

	SUCCESS(DX::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_generateHWRCommandsPSO)));
	NAME_D3D12_OBJECT(_generateHWRCommandsPSO);
}