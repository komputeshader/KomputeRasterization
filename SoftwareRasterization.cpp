#include "SoftwareRasterization.h"
#include "DescriptorManager.h"
#include "ForwardRenderer.h"
#include "imgui.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct SWRDepthSceneCB
{
	XMFLOAT4X4 VP;
	XMFLOAT2 outputRes;
	XMFLOAT2 invOutputRes;
	float bigTriangleThreshold;
	float bigTriangleTileSize;
	int useTopLeftRule;
	int scanlineRasterization;
	unsigned int totalTriangles;
	int pad[39];
};
static_assert(
	(sizeof(SWRDepthSceneCB) % 256) == 0,
	"Constant Buffer size must be 256-byte aligned");

struct SWRSceneCB
{
	XMFLOAT4X4 VP;
	XMFLOAT4X4 cascadeVP[MAX_CASCADES_COUNT];
	XMFLOAT4 sunDirection;
	float cascadeBias[MAX_CASCADES_COUNT];
	float cascadeSplits[MAX_CASCADES_COUNT];
	XMFLOAT2 outputRes;
	XMFLOAT2 invOutputRes;
	float bigTriangleThreshold;
	float bigTriangleTileSize;
	int showCascades;
	int showMeshlets;
	int useTopLeftRule;
	int cascadesCount;
	int scanlineRasterization;
	float shadowsDistance;
	unsigned int totalTriangles;
	int pad1[15];
};
static_assert(
	(sizeof(SWRSceneCB) % 256) == 0,
	"Constant Buffer size must be 256-byte aligned");

struct BigTriangleDepth
{
	float tileOffset;
	float p0WSX;
	float p0WSY;
	float p0WSZ;
	float p1WSX;
	float p1WSY;
	float p1WSZ;
	float p2WSX;
	float p2WSY;
	float p2WSZ;
};

struct BigTriangleOpaque
{
	float tileOffset;
	float p0WSX;
	float p0WSY;
	float p0WSZ;
	float p1WSX;
	float p1WSY;
	float p1WSZ;
	float p2WSX;
	float p2WSY;
	float p2WSZ;
	unsigned int packedNormal0;
	unsigned int packedNormal1;
	unsigned int packedNormal2;
	unsigned int packedColor0X;
	unsigned int packedColor0Y;
	unsigned int packedColor1X;
	unsigned int packedColor1Y;
	unsigned int packedColor2X;
	unsigned int packedColor2Y;
	unsigned int packedUV0;
	unsigned int packedUV1;
	unsigned int packedUV2;
};

void SoftwareRasterization::Resize(
	ForwardRenderer* renderer,
	int width,
	int height)
{
	_renderer = renderer;

	_width = width;
	_height = height;

	_createTriangleDepthPSO();
	_createBigTriangleDepthPSO();
	_createTriangleOpaquePSO();
	_createBigTriangleOpaquePSO();
	_createRenderTargetResources();
	_createDepthBufferResources();

	// depth CBV
	_depthSceneCBFrameSize = sizeof(SWRDepthSceneCB) * MAX_FRUSTUMS_COUNT;
	Utils::CreateCBResources(
		_depthSceneCBFrameSize * DX::FramesCount,
		reinterpret_cast<void**>(&_depthSceneCBData),
		_depthSceneCB);

	//opaque CBV
	Utils::CreateCBResources(
		sizeof(SWRSceneCB) * DX::FramesCount,
		reinterpret_cast<void**>(&_sceneCBData),
		_sceneCB);

	_createBigTrianglesBuffers();
	_createMDIResources();
	_createStatsResources();
	_createResetBuffer();

#ifdef USE_WORK_GRAPHS
	_createDepthWGResources();
	_createOpaqueWGResources();
#endif
}

void SoftwareRasterization::_createBigTrianglesBuffers()
{
	// TODO:
	//
	// theoretically, we can't have more than width * height
	// triangles actually rendered, since there are only width * height
	// pixels out there
	//
	// in practice, it can be forced with the visibility buffer algorithm,
	// for example
	//
	// current upper bound is unlikely to be reached in practice,
	// but is still just a heuristic, and needs to be fixed
	//
	// anyway, Scene::MaxSceneFacesCount is way too conservative upper bound,
	// and in practice we'll always have smth like O(WH)
	int bigTrianglesPerFrame[MAX_FRUSTUMS_COUNT] =
	{
		Settings::BackBufferWidth * Settings::BackBufferHeight
	};
	for (int cascade = 1; cascade <= MAX_CASCADES_COUNT; cascade++)
	{
		bigTrianglesPerFrame[cascade] = Settings::ShadowMapRes * Settings::ShadowMapRes / 5;
	}
	//int bigTrianglesPerFrame = Scene::MaxSceneFacesCount * sizeof(BigTriangle);

	static D3D12_DISPATCH_ARGUMENTS dispatch;
	dispatch.ThreadGroupCountX = 0;
	dispatch.ThreadGroupCountY = 1;
	dispatch.ThreadGroupCountZ = 1;

	// resources and views for the big triangles going to the depth buffers

	for (int depthBufferIdx = 0; depthBufferIdx < MAX_FRUSTUMS_COUNT; depthBufferIdx++)
	{
		Utils::CreateDefaultHeapBuffer(
			COMMAND_LIST.Get(),
			&dispatch,
			sizeof(D3D12_DISPATCH_ARGUMENTS),
			_bigTrianglesDepthCounters[depthBufferIdx],
			_bigTrianglesDepthCountersUpload[depthBufferIdx],
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
			true);
		NAME_D3D12_OBJECT_INDEXED(_bigTrianglesDepthCounters, depthBufferIdx);
		NAME_D3D12_OBJECT_INDEXED(_bigTrianglesDepthCountersUpload, depthBufferIdx);

		CD3DX12_RESOURCE_DESC desc =
			CD3DX12_RESOURCE_DESC::Buffer(
				bigTrianglesPerFrame[depthBufferIdx] * sizeof(BigTriangleDepth),
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		SUCCESS(DX::Device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(&_bigTrianglesDepth[depthBufferIdx])));
		NAME_D3D12_OBJECT_INDEXED(_bigTrianglesDepth, depthBufferIdx);

		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
		UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
		UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		UAVDesc.Buffer.FirstElement = 0;
		UAVDesc.Buffer.NumElements = bigTrianglesPerFrame[depthBufferIdx];
		UAVDesc.Buffer.StructureByteStride = sizeof(BigTriangleDepth);
		UAVDesc.Buffer.CounterOffsetInBytes = 0;
		UAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
		DX::Device->CreateUnorderedAccessView(
			_bigTrianglesDepth[depthBufferIdx].Get(),
			nullptr,
			&UAVDesc,
			Descriptors::SV.GetCPUHandle(BigTrianglesDepthUAV + depthBufferIdx));

		UAVDesc.Buffer.NumElements = 1;
		UAVDesc.Buffer.StructureByteStride = sizeof(unsigned int);
		DX::Device->CreateUnorderedAccessView(
			_bigTrianglesDepthCounters[depthBufferIdx].Get(),
			nullptr,
			&UAVDesc,
			Descriptors::SV.GetCPUHandle(BigTrianglesDepthCounterUAV + depthBufferIdx));

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Buffer.FirstElement = 0;
		SRVDesc.Buffer.NumElements = bigTrianglesPerFrame[depthBufferIdx] * BIG_TRIANGLE_DEPTH_FIELDS;
		// uint view for parallel reads in the BigTriangle*CS.hlsl
		SRVDesc.Buffer.StructureByteStride = sizeof(unsigned int);
		SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		DX::Device->CreateShaderResourceView(
			_bigTrianglesDepth[depthBufferIdx].Get(),
			&SRVDesc,
			Descriptors::SV.GetCPUHandle(BigTrianglesDepthSRV + depthBufferIdx));
	}

	// resources and views for the big triangles going to the opaque buffer

	Utils::CreateDefaultHeapBuffer(
		COMMAND_LIST.Get(),
		&dispatch,
		sizeof(D3D12_DISPATCH_ARGUMENTS),
		_bigTrianglesOpaqueCounter,
		_bigTrianglesOpaqueCounterUpload,
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
		true);
	NAME_D3D12_OBJECT(_bigTrianglesOpaqueCounter);
	NAME_D3D12_OBJECT(_bigTrianglesOpaqueCounterUpload);

	CD3DX12_RESOURCE_DESC desc =
		CD3DX12_RESOURCE_DESC::Buffer(
			bigTrianglesPerFrame[0] * sizeof(BigTriangleOpaque),
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	SUCCESS(DX::Device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		nullptr,
		IID_PPV_ARGS(&_bigTrianglesOpaque)));
	NAME_D3D12_OBJECT(_bigTrianglesOpaque);

	D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
	UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
	UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	UAVDesc.Buffer.FirstElement = 0;
	UAVDesc.Buffer.NumElements = bigTrianglesPerFrame[0];
	UAVDesc.Buffer.StructureByteStride = sizeof(BigTriangleOpaque);
	UAVDesc.Buffer.CounterOffsetInBytes = 0;
	UAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	DX::Device->CreateUnorderedAccessView(
		_bigTrianglesOpaque.Get(),
		nullptr,
		&UAVDesc,
		Descriptors::SV.GetCPUHandle(BigTrianglesOpaqueUAV));

	UAVDesc.Buffer.NumElements = 1;
	UAVDesc.Buffer.StructureByteStride = sizeof(unsigned int);
	DX::Device->CreateUnorderedAccessView(
		_bigTrianglesOpaqueCounter.Get(),
		nullptr,
		&UAVDesc,
		Descriptors::SV.GetCPUHandle(BigTrianglesOpaqueCounterUAV));

	D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	SRVDesc.Buffer.FirstElement = 0;
	SRVDesc.Buffer.NumElements = bigTrianglesPerFrame[0] * BIG_TRIANGLE_OPAQUE_FIELDS;
	// uint view for parallel reads in the BigTriangle*CS.hlsl
	SRVDesc.Buffer.StructureByteStride = sizeof(unsigned int);
	SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	DX::Device->CreateShaderResourceView(
		_bigTrianglesOpaque.Get(),
		&SRVDesc,
		Descriptors::SV.GetCPUHandle(BigTrianglesOpaqueSRV));
}

void SoftwareRasterization::_createStatsResources()
{
	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
		StatsCount * sizeof(unsigned int),
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	SUCCESS(DX::Device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&_trianglesStats)));
	NAME_D3D12_OBJECT(_trianglesStats);

	D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
	UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
	UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	UAVDesc.Buffer.FirstElement = 0;
	UAVDesc.Buffer.NumElements = StatsCount;
	UAVDesc.Buffer.StructureByteStride = sizeof(unsigned int);
	UAVDesc.Buffer.CounterOffsetInBytes = 0;
	UAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	DX::Device->CreateUnorderedAccessView(
		_trianglesStats.Get(),
		nullptr,
		&UAVDesc,
		Descriptors::SV.GetCPUHandle(SWRStatsUAV));

	prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
	desc = CD3DX12_RESOURCE_DESC::Buffer(StatsCount * sizeof(unsigned int));
	for (int frame = 0; frame < DX::FramesCount; frame++)
	{
		SUCCESS(DX::Device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&_trianglesStatsReadback[frame])));
		NAME_D3D12_OBJECT(_trianglesStatsReadback[frame]);
	}
}

void SoftwareRasterization::_createMDIResources()
{
	D3D12_INDIRECT_ARGUMENT_DESC argumentDescs[1] = {};
	argumentDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

	D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc = {};
	commandSignatureDesc.pArgumentDescs = argumentDescs;
	commandSignatureDesc.NumArgumentDescs = _countof(argumentDescs);
	commandSignatureDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);

	SUCCESS(DX::Device->CreateCommandSignature(
		&commandSignatureDesc,
		nullptr,
		IID_PPV_ARGS(&_dispatchCS)));
	NAME_D3D12_OBJECT(_dispatchCS);
}

#ifdef USE_WORK_GRAPHS
void SoftwareRasterization::_createDepthWGResources()
{
	Utils::CompileDXILLibraryFromFile(
		L"DepthWG.hlsl",
		L"lib_6_8",
		nullptr,
		0,
		_depthWGLibrary.GetAddressOf());

	CD3DX12_STATE_OBJECT_DESC SO(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);

	CD3DX12_DXIL_LIBRARY_SUBOBJECT* lib = SO.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
	CD3DX12_SHADER_BYTECODE libraryCode(_depthWGLibrary.Get());
	lib->SetDXILLibrary(&libraryCode);

	{
		CD3DX12_ROOT_PARAMETER1 computeRootParameters[9] = {};
		computeRootParameters[0].InitAsConstantBufferView(0);
		CD3DX12_DESCRIPTOR_RANGE1 ranges[8] = {};

		ranges[0].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			0);
		computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);

		ranges[1].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			10);
		computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);

		ranges[2].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			20);
		computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);

		ranges[3].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			21);
		computeRootParameters[4].InitAsDescriptorTable(1, &ranges[3]);

		ranges[4].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			22);
		computeRootParameters[5].InitAsDescriptorTable(1, &ranges[4]);

		ranges[5].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			1,
			0);
		computeRootParameters[6].InitAsDescriptorTable(1, &ranges[5]);

		ranges[6].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			1,
			1);
		computeRootParameters[7].InitAsDescriptorTable(1, &ranges[6]);

		ranges[7].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			1,
			2);
		computeRootParameters[8].InitAsDescriptorTable(1, &ranges[7]);

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
		computeRootSignatureDesc.Init_1_1(
			_countof(computeRootParameters),
			computeRootParameters);

		Utils::CreateRS(computeRootSignatureDesc, _depthWGRS);
		NAME_D3D12_OBJECT(_depthWGRS);
	}

	CD3DX12_WORK_GRAPH_SUBOBJECT* WGSubObj = SO.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>();
	WGSubObj->IncludeAllAvailableNodes();
	LPCWSTR workGraphName = L"WGRasterizer";
	WGSubObj->SetProgramName(workGraphName);
	CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* RSSubObj = SO.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
	RSSubObj->SetRootSignature(_depthWGRS.Get());

	ID3D12Device14* device = reinterpret_cast<ID3D12Device14*>(DX::Device.Get());
	SUCCESS(device->CreateStateObject(SO, IID_PPV_ARGS(&_depthWGStateObj)));

	ComPtr<ID3D12StateObjectProperties1> WGStateObjProps;
	_depthWGStateObj->QueryInterface(IID_PPV_ARGS(&WGStateObjProps));
	_depthWG = WGStateObjProps->GetProgramIdentifier(workGraphName);

	ComPtr<ID3D12WorkGraphProperties> WGProps;
	_depthWGStateObj->QueryInterface(IID_PPV_ARGS(&WGProps));
	D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memoryReqs = {};
	WGProps->GetWorkGraphMemoryRequirements(
		WGProps->GetWorkGraphIndex(workGraphName),
		&memoryReqs);

	D3D12_GPU_VIRTUAL_ADDRESS_RANGE WGBackMemRange = {};
	WGBackMemRange.SizeInBytes = memoryReqs.MaxSizeInBytes;

	CD3DX12_HEAP_PROPERTIES prop(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
		WGBackMemRange.SizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	SUCCESS(device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&_depthWGBackMem)));

	WGBackMemRange.StartAddress = _depthWGBackMem->GetGPUVirtualAddress();

	_depthProgramDesc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
	_depthProgramDesc.WorkGraph.ProgramIdentifier = _depthWG;
	_depthProgramDesc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
	_depthProgramDesc.WorkGraph.BackingMemory = WGBackMemRange;
}

void SoftwareRasterization::_createOpaqueWGResources()
{
	DxcDefine defines[] = { { L"OPAQUE", L"1" } };

	Utils::CompileDXILLibraryFromFile(
		L"OpaqueWG.hlsl",
		L"lib_6_8",
		defines,
		_countof(defines),
		_opaqueWGLibrary.GetAddressOf());

	CD3DX12_STATE_OBJECT_DESC SO(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);

	CD3DX12_DXIL_LIBRARY_SUBOBJECT* lib = SO.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
	CD3DX12_SHADER_BYTECODE libraryCode(_opaqueWGLibrary.Get());
	lib->SetDXILLibrary(&libraryCode);

	{
		CD3DX12_ROOT_PARAMETER1 computeRootParameters[14] = {};
		computeRootParameters[0].InitAsConstantBufferView(0);
		CD3DX12_DESCRIPTOR_RANGE1 ranges[13] = {};

		ranges[0].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			0);
		computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);

		ranges[1].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			10);
		computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);

		ranges[2].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			11);
		computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);

		ranges[3].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			12);
		computeRootParameters[4].InitAsDescriptorTable(1, &ranges[3]);

		ranges[4].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			13);
		computeRootParameters[5].InitAsDescriptorTable(1, &ranges[4]);

		ranges[5].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			20);
		computeRootParameters[6].InitAsDescriptorTable(1, &ranges[5]);

		ranges[6].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			21);
		computeRootParameters[7].InitAsDescriptorTable(1, &ranges[6]);

		ranges[7].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			22);
		computeRootParameters[8].InitAsDescriptorTable(1, &ranges[7]);

		ranges[8].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			23);
		computeRootParameters[9].InitAsDescriptorTable(1, &ranges[8]);

		ranges[9].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			1,
			24);
		computeRootParameters[10].InitAsDescriptorTable(1, &ranges[9]);

		ranges[10].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			1,
			0);
		computeRootParameters[11].InitAsDescriptorTable(1, &ranges[10]);

		ranges[11].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			1,
			1);
		computeRootParameters[12].InitAsDescriptorTable(1, &ranges[11]);

		ranges[12].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			1,
			2);
		computeRootParameters[13].InitAsDescriptorTable(1, &ranges[12]);

		D3D12_STATIC_SAMPLER_DESC pointClampSampler = {};
		pointClampSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		pointClampSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		pointClampSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		pointClampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		pointClampSampler.MipLODBias = 0;
		pointClampSampler.MaxAnisotropy = 0;
		pointClampSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		pointClampSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		pointClampSampler.MinLOD = 0.0f;
		pointClampSampler.MaxLOD = D3D12_FLOAT32_MAX;
		pointClampSampler.ShaderRegister = 0;
		pointClampSampler.RegisterSpace = 0;
		pointClampSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
		computeRootSignatureDesc.Init_1_1(
			_countof(computeRootParameters),
			computeRootParameters,
			1,
			&pointClampSampler);

		Utils::CreateRS(computeRootSignatureDesc, _opaqueWGRS);
		NAME_D3D12_OBJECT(_opaqueWGRS);
	}

	CD3DX12_WORK_GRAPH_SUBOBJECT* WGSubObj = SO.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>();
	WGSubObj->IncludeAllAvailableNodes();
	LPCWSTR workGraphName = L"WGRasterizer";
	WGSubObj->SetProgramName(workGraphName);
	CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* RSSubObj = SO.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
	RSSubObj->SetRootSignature(_opaqueWGRS.Get());

	ID3D12Device14* device = reinterpret_cast<ID3D12Device14*>(DX::Device.Get());
	SUCCESS(device->CreateStateObject(SO, IID_PPV_ARGS(&_opaqueWGStateObj)));

	ComPtr<ID3D12StateObjectProperties1> WGStateObjProps;
	_opaqueWGStateObj->QueryInterface(IID_PPV_ARGS(&WGStateObjProps));
	_opaqueWG = WGStateObjProps->GetProgramIdentifier(workGraphName);

	ComPtr<ID3D12WorkGraphProperties> WGProps;
	_opaqueWGStateObj->QueryInterface(IID_PPV_ARGS(&WGProps));
	D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memoryReqs = {};
	WGProps->GetWorkGraphMemoryRequirements(
		WGProps->GetWorkGraphIndex(workGraphName),
		&memoryReqs);

	D3D12_GPU_VIRTUAL_ADDRESS_RANGE WGBackMemRange = {};
	WGBackMemRange.SizeInBytes = memoryReqs.MaxSizeInBytes;

	CD3DX12_HEAP_PROPERTIES prop(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
		WGBackMemRange.SizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	SUCCESS(device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&_opaqueWGBackMem)));

	WGBackMemRange.StartAddress = _opaqueWGBackMem->GetGPUVirtualAddress();

	_opaqueProgramDesc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
	_opaqueProgramDesc.WorkGraph.ProgramIdentifier = _opaqueWG;
	_opaqueProgramDesc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
	_opaqueProgramDesc.WorkGraph.BackingMemory = WGBackMemRange;
}
#endif

void SoftwareRasterization::Update()
{
	const Camera& camera = Scene::CurrentScene->camera;

	SWRDepthSceneCB depthData = {};
	depthData.VP = camera.GetVP();
	depthData.outputRes =
	{
		static_cast<float>(_width),
		static_cast<float>(_height)
	};
	depthData.invOutputRes =
	{
		1.0f / depthData.outputRes.x,
		1.0f / depthData.outputRes.y
	};
	depthData.bigTriangleThreshold = static_cast<float>(_bigTriangleThreshold);
	depthData.bigTriangleTileSize = static_cast<float>(_bigTriangleTileSize);
	depthData.useTopLeftRule = _useTopLeftRule ? 1 : 0;
	depthData.scanlineRasterization = _scanlineRasterization ? 1 : 0;
	depthData.totalTriangles = static_cast<unsigned>(Scene::CurrentScene->indicesCPU.size() / 3);
	memcpy(
		_depthSceneCBData + DX::FrameIndex * _depthSceneCBFrameSize,
		&depthData,
		sizeof(SWRDepthSceneCB));

	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		depthData.VP = Shadows::Sun.GetCascadeVP(cascade);
		depthData.outputRes =
		{
			static_cast<float>(Settings::ShadowMapRes),
			static_cast<float>(Settings::ShadowMapRes)
		};
		depthData.invOutputRes =
		{
			1.0f / depthData.outputRes.x,
			1.0f / depthData.outputRes.y
		};
		memcpy(
			_depthSceneCBData +
			DX::FrameIndex * _depthSceneCBFrameSize +
			(1 + cascade) * sizeof(SWRDepthSceneCB),
			&depthData,
			sizeof(SWRDepthSceneCB));
	}

	SWRSceneCB sceneData = {};
	sceneData.VP = camera.GetVP();
	XMStoreFloat4(
		&sceneData.sunDirection,
		XMVector3Normalize(XMLoadFloat3(&Scene::CurrentScene->lightDirection)));
	sceneData.outputRes =
	{
		static_cast<float>(_width),
		static_cast<float>(_height)
	};
	sceneData.invOutputRes =
	{
		1.0f / sceneData.outputRes.x,
		1.0f / sceneData.outputRes.y
	};
	sceneData.cascadesCount = Settings::CascadesCount;
	sceneData.bigTriangleThreshold = static_cast<float>(_bigTriangleThreshold);
	sceneData.bigTriangleTileSize = static_cast<float>(_bigTriangleTileSize);
	sceneData.showCascades = Shadows::Sun.ShowCascades() ? 1 : 0;
	sceneData.showMeshlets = Settings::ShowMeshlets ? 1 : 0;
	sceneData.useTopLeftRule = _useTopLeftRule ? 1 : 0;
	sceneData.scanlineRasterization = _scanlineRasterization ? 1 : 0;
	sceneData.shadowsDistance = Shadows::Sun.GetShadowDistance();
	sceneData.totalTriangles = static_cast<unsigned>(Scene::CurrentScene->indicesCPU.size() / 3);
	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		sceneData.cascadeVP[cascade] = Shadows::Sun.GetCascadeVP(cascade);
		sceneData.cascadeBias[cascade] = Shadows::Sun.GetCascadeBias(cascade);
		sceneData.cascadeSplits[cascade] = Shadows::Sun.GetCascadeSplit(cascade);
	}

	memcpy(
		_sceneCBData + DX::FrameIndex * sizeof(SWRSceneCB),
		&sceneData,
		sizeof(SWRSceneCB));
}

void SoftwareRasterization::Draw()
{
	if (Settings::SWRWGEnabled)
	{
#ifdef USE_WORK_GRAPHS
		_beginFrame();
		_drawDepthWG();
		_drawShadowsWG();
		_drawDepthBigTriangles();
		_drawShadowsBigTriangles();
		_finishDepthsRendering();
		_drawOpaqueWG();
		_endFrame();
#endif
	}
	else
	{
		_beginFrame();
		_drawDepth();
		_drawShadows();
		_drawDepthBigTriangles();
		_drawShadowsBigTriangles();
		_finishDepthsRendering();
		_drawOpaque();
		_endFrame();
	}
}

void SoftwareRasterization::_beginFrame()
{
	CD3DX12_RESOURCE_BARRIER* barriers =
		(CD3DX12_RESOURCE_BARRIER*)alloca((5 + 2 * Settings::FrustumsCount) * sizeof(CD3DX12_RESOURCE_BARRIER));
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		_trianglesStats.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_DEST);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		_bigTrianglesOpaqueCounter.Get(),
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
		D3D12_RESOURCE_STATE_COPY_DEST);
	for (int frustum = 0; frustum < Settings::FrustumsCount; frustum++)
	{
		barriers[1 + frustum] = CD3DX12_RESOURCE_BARRIER::Transition(
			_bigTrianglesDepthCounters[frustum].Get(),
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
			D3D12_RESOURCE_STATE_COPY_DEST);
	}
	COMMAND_LIST->ResourceBarrier(1 + Settings::FrustumsCount, barriers);

	_clearStatistics();
	for (int frustum = 0; frustum < Settings::FrustumsCount; frustum++)
	{
		_clearBigTrianglesDepthCounter(frustum);
	}
	_clearBigTrianglesOpaqueCounter();

	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		_depthBuffer.Get(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		Shadows::Sun.GetShadowMapSWR(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barriers[2] = CD3DX12_RESOURCE_BARRIER::Transition(
		_trianglesStats.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barriers[3] = CD3DX12_RESOURCE_BARRIER::Transition(
		_bigTrianglesOpaque.Get(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barriers[4] = CD3DX12_RESOURCE_BARRIER::Transition(
		_bigTrianglesOpaqueCounter.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	for (int frustum = 0; frustum < Settings::FrustumsCount; frustum++)
	{
		barriers[5 + 2 * frustum] = CD3DX12_RESOURCE_BARRIER::Transition(
			_bigTrianglesDepth[frustum].Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		barriers[5 + 2 * frustum + 1] = CD3DX12_RESOURCE_BARRIER::Transition(
			_bigTrianglesDepthCounters[frustum].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	COMMAND_LIST->ResourceBarrier(5 + 2 * Settings::FrustumsCount, barriers);

	unsigned int clearValue[] = { 0, 0, 0, 0 };
	COMMAND_LIST->ClearUnorderedAccessViewUint(
		Descriptors::SV.GetGPUHandle(SWRDepthUAV),
		Descriptors::NonSV.GetCPUHandle(SWRDepthUAV),
		_depthBuffer.Get(),
		clearValue,
		0,
		nullptr);

	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		COMMAND_LIST->ClearUnorderedAccessViewUint(
			Descriptors::SV.GetGPUHandle(SWRShadowMapUAV + cascade),
			Descriptors::NonSV.GetCPUHandle(SWRShadowMapUAV + cascade),
			Shadows::Sun.GetShadowMapSWR(),
			clearValue,
			0,
			nullptr);
	}

	COMMAND_LIST->ClearUnorderedAccessViewFloat(
		Descriptors::SV.GetGPUHandle(SWRRenderTargetUAV),
		Descriptors::NonSV.GetCPUHandle(SWRRenderTargetUAV),
		_renderTarget.Get(),
		SkyColor,
		0,
		nullptr);
}

void SoftwareRasterization::_drawDepth()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"SWR Depth");

	COMMAND_LIST->SetComputeRootSignature(_triangleDepthRS.Get());
	COMMAND_LIST->SetPipelineState(_triangleDepthPSO.Get());
	COMMAND_LIST->SetComputeRootConstantBufferView(
		0, _depthSceneCB->GetGPUVirtualAddress() + DX::FrameIndex * _depthSceneCBFrameSize);
	COMMAND_LIST->SetComputeRootDescriptorTable(
		1, Scene::CurrentScene->positionsGPU.GetSRV());
#ifdef GPU_SOA_BUFFERS
	COMMAND_LIST->SetComputeRootDescriptorTable(
		2, Scene::CurrentScene->indicesSOAGPU.GetSRV());
#else
	COMMAND_LIST->SetComputeRootDescriptorTable(
		2, Scene::CurrentScene->indicesGPU.GetSRV());
#endif
	COMMAND_LIST->SetComputeRootDescriptorTable(
		3,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		4, Descriptors::SV.GetGPUHandle(PrevFrameDepthSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		5, Descriptors::SV.GetGPUHandle(CulledCommandsSRV + DX::FrameIndex * PerFrameDescriptorsCount));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		6, Descriptors::SV.GetGPUHandle(SWRDepthUAV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		7, Descriptors::SV.GetGPUHandle(BigTrianglesDepthUAV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		8, Descriptors::SV.GetGPUHandle(BigTrianglesDepthCounterUAV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		9, Descriptors::SV.GetGPUHandle(SWRStatsUAV));

	if (Settings::CullingEnabled)
	{
		COMMAND_LIST->ExecuteIndirect(
			_dispatchCS.Get(),
			1,
			_renderer->GetCulledCommandsCounter(DX::FrameIndex, 0),
			0,
			nullptr,
			0);
	}
	else
	{
		for (const auto& prefab : Scene::CurrentScene->prefabs)
		{
			for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
			{
				const auto& currentMesh = Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];

				_drawIndexedInstanced(
					currentMesh.indexCountPerInstance,
					currentMesh.instanceCount,
					currentMesh.startIndexLocation,
					currentMesh.baseVertexLocation,
					currentMesh.startInstanceLocation);
			}
		}
	}

	//CD3DX12_RESOURCE_BARRIER barriers[2] = {};
	//barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
	//	_bigTriangles[0].Get(),
	//	D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	//	D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
	//	D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
	//	D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
	//barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
	//	_bigTrianglesCounters[0].Get(),
	//	D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	//	D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
	//	D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
	//	D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
	//COMMAND_LIST->ResourceBarrier(_countof(barriers), barriers);

	PIXEndEvent(COMMAND_LIST.Get());
}

void SoftwareRasterization::_drawShadows()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"SWR Shadows");

	COMMAND_LIST->SetComputeRootSignature(_triangleDepthRS.Get());
	COMMAND_LIST->SetPipelineState(_triangleDepthPSO.Get());
	CD3DX12_RESOURCE_BARRIER barriers[2] = {};
	for (int cascade = 1; cascade <= Settings::CascadesCount; cascade++)
	{
		COMMAND_LIST->SetComputeRootConstantBufferView(
			0, _depthSceneCB->GetGPUVirtualAddress() + DX::FrameIndex * _depthSceneCBFrameSize + cascade * sizeof(SWRDepthSceneCB));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			1, Scene::CurrentScene->positionsGPU.GetSRV());
#ifdef GPU_SOA_BUFFERS
		COMMAND_LIST->SetComputeRootDescriptorTable(
			2, Scene::CurrentScene->indicesSOAGPU.GetSRV());
#else
		COMMAND_LIST->SetComputeRootDescriptorTable(
			2, Scene::CurrentScene->indicesGPU.GetSRV());
#endif
		COMMAND_LIST->SetComputeRootDescriptorTable(
			3,
			Settings::CullingEnabled
			? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + cascade + DX::FrameIndex * PerFrameDescriptorsCount)
			: Scene::CurrentScene->instancesGPU.GetSRV());
		COMMAND_LIST->SetComputeRootDescriptorTable(
			4, Descriptors::SV.GetGPUHandle(PrevFrameShadowMapSRV + cascade - 1));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			5, Descriptors::SV.GetGPUHandle(CulledCommandsSRV + cascade + DX::FrameIndex * PerFrameDescriptorsCount));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			6, Descriptors::SV.GetGPUHandle(SWRShadowMapUAV + cascade - 1));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			7, Descriptors::SV.GetGPUHandle(BigTrianglesDepthUAV + cascade));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			8, Descriptors::SV.GetGPUHandle(BigTrianglesDepthCounterUAV + cascade));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			9, Descriptors::SV.GetGPUHandle(SWRStatsUAV));

		if (Settings::CullingEnabled)
		{
			COMMAND_LIST->ExecuteIndirect(
				_dispatchCS.Get(),
				1,
				_renderer->GetCulledCommandsCounter(DX::FrameIndex, cascade),
				0,
				nullptr,
				0);
		}
		else
		{
			for (const auto& prefab : Scene::CurrentScene->prefabs)
			{
				for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
				{
					const auto& currentMesh =
						Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];

					_drawIndexedInstanced(
						currentMesh.indexCountPerInstance,
						currentMesh.instanceCount,
						currentMesh.startIndexLocation,
						currentMesh.baseVertexLocation,
						currentMesh.startInstanceLocation);
				}
			}
		}

		//barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		//	_bigTriangles[cascade].Get(),
		//	D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		//	D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		//	D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		//	D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
		//barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		//	_bigTrianglesCounters[cascade].Get(),
		//	D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		//	D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
		//	D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		//	D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
		//COMMAND_LIST->ResourceBarrier(_countof(barriers), barriers);
	}

	PIXEndEvent(COMMAND_LIST.Get());
}

void SoftwareRasterization::_drawDepthBigTriangles()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"SWR Depth Big Triangles");

	CD3DX12_RESOURCE_BARRIER barriers[2] = {};
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		_bigTrianglesDepth[0].Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE/*,
		D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_BARRIER_FLAG_END_ONLY*/);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		_bigTrianglesDepthCounters[0].Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT/*,
		D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_BARRIER_FLAG_END_ONLY*/);
	COMMAND_LIST->ResourceBarrier(_countof(barriers), barriers);

	COMMAND_LIST->SetComputeRootSignature(_bigTriangleDepthRS.Get());
	COMMAND_LIST->SetPipelineState(_bigTriangleDepthPSO.Get());
	COMMAND_LIST->SetComputeRootConstantBufferView(
		0, _depthSceneCB->GetGPUVirtualAddress() + DX::FrameIndex * _depthSceneCBFrameSize);
	COMMAND_LIST->SetComputeRootDescriptorTable(
		1, Descriptors::SV.GetGPUHandle(BigTrianglesDepthSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		2,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		3, Descriptors::SV.GetGPUHandle(SWRDepthUAV));

	COMMAND_LIST->ExecuteIndirect(
		_dispatchCS.Get(),
		1,
		_bigTrianglesDepthCounters[0].Get(),
		0,
		nullptr,
		0);

	//barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
	//	_bigTriangles[0].Get(),
	//	D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
	//	D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	//	D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
	//	D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
	//COMMAND_LIST->ResourceBarrier(1, barriers);

	PIXEndEvent(COMMAND_LIST.Get());
}

void SoftwareRasterization::_drawShadowsBigTriangles()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"SWR Shadows Big Triangles");

	COMMAND_LIST->SetComputeRootSignature(_bigTriangleDepthRS.Get());
	COMMAND_LIST->SetPipelineState(_bigTriangleDepthPSO.Get());
	CD3DX12_RESOURCE_BARRIER barriers[2] = {};
	for (int cascade = 1; cascade <= Settings::CascadesCount; cascade++)
	{
		barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
			_bigTrianglesDepth[cascade].Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE/*,
			D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			D3D12_RESOURCE_BARRIER_FLAG_END_ONLY*/);
		barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
			_bigTrianglesDepthCounters[cascade].Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT/*,
			D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			D3D12_RESOURCE_BARRIER_FLAG_END_ONLY*/);
		COMMAND_LIST->ResourceBarrier(_countof(barriers), barriers);

		COMMAND_LIST->SetComputeRootConstantBufferView(
			0, _depthSceneCB->GetGPUVirtualAddress() + DX::FrameIndex * _depthSceneCBFrameSize + cascade * sizeof(SWRDepthSceneCB));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			1, Descriptors::SV.GetGPUHandle(BigTrianglesDepthSRV + cascade));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			2,
			Settings::CullingEnabled
			? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + cascade + DX::FrameIndex * PerFrameDescriptorsCount)
			: Scene::CurrentScene->instancesGPU.GetSRV());
		COMMAND_LIST->SetComputeRootDescriptorTable(
			3, Descriptors::SV.GetGPUHandle(SWRShadowMapUAV + cascade - 1));

		COMMAND_LIST->ExecuteIndirect(
			_dispatchCS.Get(),
			1,
			_bigTrianglesDepthCounters[cascade].Get(),
			0,
			nullptr,
			0);
	}

	PIXEndEvent(COMMAND_LIST.Get());
}

void SoftwareRasterization::_finishDepthsRendering()
{
	_renderer->PreparePrevFrameDepth(_depthBuffer.Get());

	if (Settings::ShadowsHiZCullingEnabled)
	{
		Shadows::Sun.PreparePrevFrameShadowMap();
	}
	else
	{
		CD3DX12_RESOURCE_BARRIER barriers[1] = {};
		barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
			Shadows::Sun.GetShadowMapSWR(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		COMMAND_LIST->ResourceBarrier(1, barriers);
	}
}

void SoftwareRasterization::_drawOpaque()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"SWR Opaque");

	COMMAND_LIST->SetComputeRootSignature(_triangleOpaqueRS.Get());
	COMMAND_LIST->SetPipelineState(_triangleOpaquePSO.Get());
	COMMAND_LIST->SetComputeRootConstantBufferView(
		0, _sceneCB->GetGPUVirtualAddress() + DX::FrameIndex * sizeof(SWRSceneCB));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		1, Scene::CurrentScene->positionsGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		2, Scene::CurrentScene->normalsGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		3, Scene::CurrentScene->colorsGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		4, Scene::CurrentScene->texcoordsGPU.GetSRV());
#ifdef GPU_SOA_BUFFERS
	COMMAND_LIST->SetComputeRootDescriptorTable(
		5, Scene::CurrentScene->indicesSOAGPU.GetSRV());
#else
	COMMAND_LIST->SetComputeRootDescriptorTable(
		5, Scene::CurrentScene->indicesGPU.GetSRV());
#endif
	COMMAND_LIST->SetComputeRootDescriptorTable(
		6,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	// misleading naming, actually, at this point in time, it is
	// current frame depth with Hi-Z mipchain
	COMMAND_LIST->SetComputeRootDescriptorTable(
		7, Descriptors::SV.GetGPUHandle(PrevFrameDepthSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		8, Descriptors::SV.GetGPUHandle(SWRShadowMapSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		9, Descriptors::SV.GetGPUHandle(CulledCommandsSRV + DX::FrameIndex * PerFrameDescriptorsCount));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		10, Descriptors::SV.GetGPUHandle(SWRRenderTargetUAV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		11, Descriptors::SV.GetGPUHandle(BigTrianglesOpaqueUAV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		12, Descriptors::SV.GetGPUHandle(BigTrianglesOpaqueCounterUAV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		13, Descriptors::SV.GetGPUHandle(SWRStatsUAV));

	if (Settings::CullingEnabled)
	{
		COMMAND_LIST->ExecuteIndirect(
			_dispatchCS.Get(),
			1,
			_renderer->GetCulledCommandsCounter(DX::FrameIndex, 0),
			0,
			nullptr,
			0);
	}
	else
	{
		for (const auto& prefab : Scene::CurrentScene->prefabs)
		{
			for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
			{
				const auto& currentMesh = Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];

				_drawIndexedInstanced(
					currentMesh.indexCountPerInstance,
					currentMesh.instanceCount,
					currentMesh.startIndexLocation,
					currentMesh.baseVertexLocation,
					currentMesh.startInstanceLocation);
			}
		}
	}

	CD3DX12_RESOURCE_BARRIER barriers[2] = {};
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		_bigTrianglesOpaque.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		_bigTrianglesOpaqueCounter.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	COMMAND_LIST->ResourceBarrier(2, barriers);

	COMMAND_LIST->SetComputeRootSignature(_bigTriangleOpaqueRS.Get());
	COMMAND_LIST->SetPipelineState(_bigTriangleOpaquePSO.Get());
	COMMAND_LIST->SetComputeRootConstantBufferView(
		0, _sceneCB->GetGPUVirtualAddress() + DX::FrameIndex * sizeof(SWRSceneCB));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		1, Descriptors::SV.GetGPUHandle(BigTrianglesOpaqueSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		2,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		3, Descriptors::SV.GetGPUHandle(SWRDepthSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		4, Descriptors::SV.GetGPUHandle(SWRShadowMapSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		5, Descriptors::SV.GetGPUHandle(SWRRenderTargetUAV));

	COMMAND_LIST->ExecuteIndirect(
		_dispatchCS.Get(),
		1,
		_bigTrianglesOpaqueCounter.Get(),
		0,
		nullptr,
		0);

	PIXEndEvent(COMMAND_LIST.Get());
}

#ifdef USE_WORK_GRAPHS
void SoftwareRasterization::_drawDepthWG()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"SWR Depth WG");

	int frustumIndex = 0;

	COMMAND_LIST->SetComputeRootSignature(_depthWGRS.Get());

	COMMAND_LIST->SetComputeRootConstantBufferView(
		0, _depthSceneCB->GetGPUVirtualAddress() + frustumIndex +  DX::FrameIndex * _depthSceneCBFrameSize);
	COMMAND_LIST->SetComputeRootDescriptorTable(
		1, Descriptors::SV.GetGPUHandle(CulledCommandsCountersSRV + frustumIndex + DX::FrameIndex * PerFrameDescriptorsCount));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		2, Scene::CurrentScene->positionsGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		3, Descriptors::SV.GetGPUHandle(CulledCommandsSRV + frustumIndex + DX::FrameIndex * PerFrameDescriptorsCount));
#ifdef GPU_SOA_BUFFERS
	COMMAND_LIST->SetComputeRootDescriptorTable(
		4, Scene::CurrentScene->indicesSOAGPU.GetSRV());
#else
	COMMAND_LIST->SetComputeRootDescriptorTable(
		4, Scene::CurrentScene->indicesGPU.GetSRV());
#endif
	COMMAND_LIST->SetComputeRootDescriptorTable(
		5,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + frustumIndex + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		6, Descriptors::SV.GetGPUHandle(SWRDepthUAV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		7, Descriptors::SV.GetGPUHandle(BigTrianglesDepthUAV + frustumIndex));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		8, Descriptors::SV.GetGPUHandle(SWRStatsUAV));

	ID3D12GraphicsCommandList10* commandList = (ID3D12GraphicsCommandList10*)COMMAND_LIST.Get();

	commandList->SetProgram(&_depthProgramDesc);

	D3D12_DISPATCH_GRAPH_DESC dispatchDesc = {};
	dispatchDesc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
	dispatchDesc.NodeCPUInput.EntrypointIndex = 0;
	dispatchDesc.NodeCPUInput.NumRecords = 1;
	dispatchDesc.NodeCPUInput.pRecords = nullptr;
	dispatchDesc.NodeCPUInput.RecordStrideInBytes = 0;
	commandList->DispatchGraph(&dispatchDesc);

	//CD3DX12_RESOURCE_BARRIER barriers[2] = {};
	//barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
	//	_bigTriangles[0].Get(),
	//	D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	//	D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
	//	D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
	//	D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
	//barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
	//	_bigTrianglesCounters[0].Get(),
	//	D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	//	D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
	//	D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
	//	D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
	//COMMAND_LIST->ResourceBarrier(_countof(barriers), barriers);

	PIXEndEvent(COMMAND_LIST.Get());
}

void SoftwareRasterization::_drawShadowsWG()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"SWR Shadows WG");

	COMMAND_LIST->SetComputeRootSignature(_depthWGRS.Get());

	CD3DX12_RESOURCE_BARRIER barriers[2] = {};
	for (int cascade = 1; cascade <= Settings::CascadesCount; cascade++)
	{
		COMMAND_LIST->SetComputeRootConstantBufferView(
			0, _depthSceneCB->GetGPUVirtualAddress() + DX::FrameIndex * _depthSceneCBFrameSize + cascade * sizeof(SWRDepthSceneCB));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			1, Descriptors::SV.GetGPUHandle(CulledCommandsCountersSRV + cascade + DX::FrameIndex * PerFrameDescriptorsCount));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			2, Scene::CurrentScene->positionsGPU.GetSRV());
		COMMAND_LIST->SetComputeRootDescriptorTable(
			3, Descriptors::SV.GetGPUHandle(CulledCommandsSRV + cascade + DX::FrameIndex * PerFrameDescriptorsCount));
#ifdef GPU_SOA_BUFFERS
		COMMAND_LIST->SetComputeRootDescriptorTable(
			4, Scene::CurrentScene->indicesSOAGPU.GetSRV());
#else
		COMMAND_LIST->SetComputeRootDescriptorTable(
			4, Scene::CurrentScene->indicesGPU.GetSRV());
#endif
		COMMAND_LIST->SetComputeRootDescriptorTable(
			5,
			Settings::CullingEnabled
			? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + cascade + DX::FrameIndex * PerFrameDescriptorsCount)
			: Scene::CurrentScene->instancesGPU.GetSRV());
		COMMAND_LIST->SetComputeRootDescriptorTable(
			6, Descriptors::SV.GetGPUHandle(SWRShadowMapUAV + cascade - 1));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			7, Descriptors::SV.GetGPUHandle(BigTrianglesDepthUAV + cascade));
		COMMAND_LIST->SetComputeRootDescriptorTable(
			8, Descriptors::SV.GetGPUHandle(SWRStatsUAV));

		ID3D12GraphicsCommandList10* commandList = (ID3D12GraphicsCommandList10*)COMMAND_LIST.Get();

		commandList->SetProgram(&_depthProgramDesc);

		D3D12_DISPATCH_GRAPH_DESC dispatchDesc = {};
		dispatchDesc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
		dispatchDesc.NodeCPUInput.EntrypointIndex = 0;
		dispatchDesc.NodeCPUInput.NumRecords = 1;
		dispatchDesc.NodeCPUInput.pRecords = nullptr;
		dispatchDesc.NodeCPUInput.RecordStrideInBytes = 0;
		commandList->DispatchGraph(&dispatchDesc);

		//barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		//	_bigTriangles[cascade].Get(),
		//	D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		//	D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		//	D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		//	D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
		//barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		//	_bigTrianglesCounters[cascade].Get(),
		//	D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		//	D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
		//	D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		//	D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
		//COMMAND_LIST->ResourceBarrier(_countof(barriers), barriers);
	}

	PIXEndEvent(COMMAND_LIST.Get());
}

void SoftwareRasterization::_drawOpaqueWG()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"SWR Opaque WG");

	COMMAND_LIST->SetComputeRootSignature(_opaqueWGRS.Get());

	COMMAND_LIST->SetComputeRootConstantBufferView(
		0, _sceneCB->GetGPUVirtualAddress() + DX::FrameIndex * sizeof(SWRSceneCB));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		1, Descriptors::SV.GetGPUHandle(CulledCommandsCountersSRV + DX::FrameIndex * PerFrameDescriptorsCount));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		2, Scene::CurrentScene->positionsGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		3, Scene::CurrentScene->normalsGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		4, Scene::CurrentScene->colorsGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		5, Scene::CurrentScene->texcoordsGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		6, Descriptors::SV.GetGPUHandle(CulledCommandsSRV + DX::FrameIndex * PerFrameDescriptorsCount));
#ifdef GPU_SOA_BUFFERS
	COMMAND_LIST->SetComputeRootDescriptorTable(
		7, Scene::CurrentScene->indicesSOAGPU.GetSRV());
#else
	COMMAND_LIST->SetComputeRootDescriptorTable(
		7, Scene::CurrentScene->indicesGPU.GetSRV());
#endif
	COMMAND_LIST->SetComputeRootDescriptorTable(
		8,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	// misleading naming, actually, at this point in time, it is
	// current frame depth with Hi-Z mipchain
	COMMAND_LIST->SetComputeRootDescriptorTable(
		9, Descriptors::SV.GetGPUHandle(PrevFrameDepthSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		10, Descriptors::SV.GetGPUHandle(SWRShadowMapSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		11, Descriptors::SV.GetGPUHandle(SWRRenderTargetUAV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		12, Descriptors::SV.GetGPUHandle(BigTrianglesOpaqueUAV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		13, Descriptors::SV.GetGPUHandle(SWRStatsUAV));

	ID3D12GraphicsCommandList10* commandList = (ID3D12GraphicsCommandList10*)COMMAND_LIST.Get();

	commandList->SetProgram(&_opaqueProgramDesc);

	D3D12_DISPATCH_GRAPH_DESC dispatchDesc = {};
	dispatchDesc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
	dispatchDesc.NodeCPUInput.EntrypointIndex = 0;
	dispatchDesc.NodeCPUInput.NumRecords = 1;
	dispatchDesc.NodeCPUInput.pRecords = nullptr;
	dispatchDesc.NodeCPUInput.RecordStrideInBytes = 0;
	commandList->DispatchGraph(&dispatchDesc);

	CD3DX12_RESOURCE_BARRIER barriers[2] = {};
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		_bigTrianglesOpaque.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		_bigTrianglesOpaqueCounter.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	COMMAND_LIST->ResourceBarrier(2, barriers);

	COMMAND_LIST->SetComputeRootSignature(_bigTriangleOpaqueRS.Get());
	COMMAND_LIST->SetPipelineState(_bigTriangleOpaquePSO.Get());
	COMMAND_LIST->SetComputeRootConstantBufferView(
		0, _sceneCB->GetGPUVirtualAddress() + DX::FrameIndex * sizeof(SWRSceneCB));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		1, Descriptors::SV.GetGPUHandle(BigTrianglesOpaqueSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		2,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	COMMAND_LIST->SetComputeRootDescriptorTable(
		3, Descriptors::SV.GetGPUHandle(SWRDepthSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		4, Descriptors::SV.GetGPUHandle(SWRShadowMapSRV));
	COMMAND_LIST->SetComputeRootDescriptorTable(
		5, Descriptors::SV.GetGPUHandle(SWRRenderTargetUAV));

	COMMAND_LIST->ExecuteIndirect(
		_dispatchCS.Get(),
		1,
		_bigTrianglesOpaqueCounter.Get(),
		0,
		nullptr,
		0);

	PIXEndEvent(COMMAND_LIST.Get());
}
#endif

void SoftwareRasterization::_endFrame()
{
	// collect statistics
	CD3DX12_RESOURCE_BARRIER barriers[1] = {};
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		_trianglesStats.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	COMMAND_LIST->ResourceBarrier(1, barriers);

	COMMAND_LIST->CopyBufferRegion(
		_trianglesStatsReadback[DX::FrameIndex].Get(),
		0,
		_trianglesStats.Get(),
		0,
		StatsCount * sizeof(unsigned int));

	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		_trianglesStats.Get(),
		D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	COMMAND_LIST->ResourceBarrier(1, barriers);

	unsigned int* result = nullptr;
	SUCCESS(_trianglesStatsReadback[DX::FrameIndex]->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&result)));
	memcpy(
		_statsResult,
		result,
		StatsCount * sizeof(unsigned int));
	_trianglesStatsReadback[DX::FrameIndex]->Unmap(0, nullptr);
}

void SoftwareRasterization::GUINewFrame()
{
	int location = Settings::SWRGUILocation;
	ImGuiWindowFlags window_flags =
		ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav;
	if (location >= 0)
	{
		float PAD = 10.0f;
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		// Use work area to avoid menu-bar/task-bar, if any!
		ImVec2 work_pos = viewport->WorkPos;
		ImVec2 work_size = viewport->WorkSize;
		ImVec2 window_pos, window_pos_pivot;
		window_pos.x = (location & 1)
			? (work_pos.x + work_size.x - PAD)
			: (work_pos.x + PAD);
		window_pos.y = (location & 2)
			? (work_pos.y + work_size.y - PAD)
			: (work_pos.y + PAD);
		window_pos_pivot.x = (location & 1) ? 1.0f : 0.0f;
		window_pos_pivot.y = (location & 2) ? 1.0f : 0.0f;
		ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
		window_flags |= ImGuiWindowFlags_NoMove;
	}

	// Transparent background
	ImGui::SetNextWindowBgAlpha(Settings::GUITransparency);

	if (ImGui::Begin("Software Rasterization", nullptr, window_flags))
	{
		ImGui::SliderInt(
			"Big Triangle Threshold",
			&_bigTriangleThreshold,
			1,
			8192,
			"%i",
			ImGuiSliderFlags_AlwaysClamp);

		ImGui::SliderInt(
			"Big Triangle Tile Size",
			&_bigTriangleTileSize,
			64,
			2048,
			"%i",
			ImGuiSliderFlags_AlwaysClamp);

		ImGui::Checkbox("Use top-left rasterization rule", &_useTopLeftRule);
		ImGui::Checkbox("Scanline rasterization", &_scanlineRasterization);
	}

	ImGui::End();
}

void SoftwareRasterization::_drawIndexedInstanced(
	unsigned int indexCountPerInstance,
	unsigned int instanceCount,
	unsigned int startIndexLocation,
	int baseVertexLocation,
	unsigned int startInstanceLocation)
{
	// TODO: restore it
	//unsigned int commandData[] =
	//{
	//	indexCountPerInstance,
	//	startIndexLocation,
	//	baseVertexLocation,
	//	startInstanceLocation
	//};
	//COMMAND_LIST->SetComputeRoot32BitConstants(
	//	RootConstants,
	//	_countof(commandData),
	//	commandData,
	//	0);
	//
	//assert(indexCountPerInstance % 3 == 0);
	//unsigned int trianglesCount = indexCountPerInstance / 3;
	//COMMAND_LIST->Dispatch(
	//	Utils::DispatchSize(SWR_TRIANGLE_THREADS_X, trianglesCount),
	//	instanceCount,
	//	1);
}

void SoftwareRasterization::_clearStatistics()
{
	COMMAND_LIST->CopyBufferRegion(
		_trianglesStats.Get(),
		0,
		_counterReset.Get(),
		0,
		StatsCount * sizeof(unsigned int));
}

void SoftwareRasterization::_clearBigTrianglesDepthCounter(int frustum)
{
	COMMAND_LIST->CopyBufferRegion(
		_bigTrianglesDepthCounters[frustum].Get(),
		0,
		_counterReset.Get(),
		0,
		sizeof(unsigned int));
}

void SoftwareRasterization::_clearBigTrianglesOpaqueCounter()
{
	COMMAND_LIST->CopyBufferRegion(
		_bigTrianglesOpaqueCounter.Get(),
		0,
		_counterReset.Get(),
		0,
		sizeof(unsigned int));
}

void SoftwareRasterization::_createRenderTargetResources()
{
	D3D12_RESOURCE_DESC rtDesc = {};
	rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rtDesc.Alignment = 0;
	rtDesc.Width = _width;
	rtDesc.Height = _height;
	rtDesc.DepthOrArraySize = 1;
	rtDesc.MipLevels = 1;
	rtDesc.Format = Settings::BackBufferFormat;
	rtDesc.SampleDesc.Count = 1;
	rtDesc.SampleDesc.Quality = 0;
	rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	SUCCESS(DX::Device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&rtDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&_renderTarget)));
	NAME_D3D12_OBJECT(_renderTarget);

	// render target UAV
	D3D12_UNORDERED_ACCESS_VIEW_DESC rtUAV = {};
	rtUAV.Format = Settings::BackBufferFormat;
	rtUAV.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	rtUAV.Texture2D.MipSlice = 0;
	rtUAV.Texture2D.PlaneSlice = 0;

	DX::Device->CreateUnorderedAccessView(
		_renderTarget.Get(),
		nullptr,
		&rtUAV,
		Descriptors::SV.GetCPUHandle(SWRRenderTargetUAV));

	DX::Device->CreateUnorderedAccessView(
		_renderTarget.Get(),
		nullptr,
		&rtUAV,
		Descriptors::NonSV.GetCPUHandle(SWRRenderTargetUAV));
}

void SoftwareRasterization::_createDepthBufferResources()
{
	D3D12_RESOURCE_DESC depthStencilDesc = {};
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = _width;
	depthStencilDesc.Height = _height;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	depthStencilDesc.SampleDesc.Count = 1;
	depthStencilDesc.SampleDesc.Quality = 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	SUCCESS(DX::Device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		nullptr,
		IID_PPV_ARGS(&_depthBuffer)));
	NAME_D3D12_OBJECT(_depthBuffer);

	// depth UAV
	D3D12_UNORDERED_ACCESS_VIEW_DESC depthUAV = {};
	depthUAV.Format = DXGI_FORMAT_R32_UINT;
	depthUAV.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	depthUAV.Texture2D.PlaneSlice = 0;
	depthUAV.Texture2D.MipSlice = 0;

	DX::Device->CreateUnorderedAccessView(
		_depthBuffer.Get(),
		nullptr,
		&depthUAV,
		Descriptors::SV.GetCPUHandle(SWRDepthUAV));

	DX::Device->CreateUnorderedAccessView(
		_depthBuffer.Get(),
		nullptr,
		&depthUAV,
		Descriptors::NonSV.GetCPUHandle(SWRDepthUAV));

	// depth SRV
	D3D12_SHADER_RESOURCE_VIEW_DESC depthSRV = {};
	depthSRV.Format = DXGI_FORMAT_R32_FLOAT;
	depthSRV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	depthSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	depthSRV.Texture2D.MostDetailedMip = 0;
	depthSRV.Texture2D.MipLevels = 1;
	depthSRV.Texture2D.PlaneSlice = 0;
	depthSRV.Texture2D.ResourceMinLODClamp = 0.0f;

	DX::Device->CreateShaderResourceView(
		_depthBuffer.Get(),
		&depthSRV,
		Descriptors::SV.GetCPUHandle(SWRDepthSRV));
}

void SoftwareRasterization::_createResetBuffer()
{
	// allocate a buffer that can be used to reset the UAV counters and
	// initialize it to 0
	// is used also for clearing statistics buffer,
	// so the size is bigger than 1
	unsigned int bufferSize = StatsCount * sizeof(unsigned int);
	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto desc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
	SUCCESS(DX::Device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&_counterReset)));
	NAME_D3D12_OBJECT(_counterReset);

	unsigned char* mappedCounterReset = nullptr;
	// We do not intend to read from this resource on the CPU.
	CD3DX12_RANGE readRange(0, 0);
	SUCCESS(_counterReset->Map(
		0,
		&readRange,
		reinterpret_cast<void**>(&mappedCounterReset)));
	ZeroMemory(mappedCounterReset, bufferSize);
	_counterReset->Unmap(0, nullptr);
}

void SoftwareRasterization::_createTriangleDepthPSO()
{
	CD3DX12_ROOT_PARAMETER1 computeRootParameters[10] = {};
	computeRootParameters[0].InitAsConstantBufferView(0);
	CD3DX12_DESCRIPTOR_RANGE1 ranges[9] = {};

	ranges[0].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		0);
	computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);

	ranges[1].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		8);
	computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);

	ranges[2].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		9);
	computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);

	ranges[3].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		10);
	computeRootParameters[4].InitAsDescriptorTable(1, &ranges[3]);

	ranges[4].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		12);
	computeRootParameters[5].InitAsDescriptorTable(1, &ranges[4]);

	ranges[5].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		0);
	computeRootParameters[6].InitAsDescriptorTable(1, &ranges[5]);

	ranges[6].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		1);
	computeRootParameters[7].InitAsDescriptorTable(1, &ranges[6]);

	ranges[7].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		2);
	computeRootParameters[8].InitAsDescriptorTable(1, &ranges[7]);

	ranges[8].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		3);
	computeRootParameters[9].InitAsDescriptorTable(1, &ranges[8]);

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
	computeRootSignatureDesc.Init_1_1(
		_countof(computeRootParameters),
		computeRootParameters,
		1,
		&Utils::HiZSamplerDesc);

	Utils::CreateRS(computeRootSignatureDesc, _triangleDepthRS);
	NAME_D3D12_OBJECT(_triangleDepthRS);

	ComPtr<ID3DBlob> computeShader = Utils::CompileShader(
		L"TriangleDepthCS.hlsl",
		nullptr,
		"main",
		"cs_5_0");

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = _triangleDepthRS.Get();
	psoDesc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };

	SUCCESS(DX::Device->CreateComputePipelineState(
		&psoDesc,
		IID_PPV_ARGS(&_triangleDepthPSO)));
	NAME_D3D12_OBJECT(_triangleDepthPSO);
}

void SoftwareRasterization::_createBigTriangleDepthPSO()
{
	CD3DX12_ROOT_PARAMETER1 computeRootParameters[4] = {};
	computeRootParameters[0].InitAsConstantBufferView(0);
	CD3DX12_DESCRIPTOR_RANGE1 ranges[3] = {};

	ranges[0].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		0);
	computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);

	ranges[1].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		1);
	computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);

	ranges[2].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		0);
	computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
	computeRootSignatureDesc.Init_1_1(_countof(computeRootParameters), computeRootParameters);

	Utils::CreateRS(computeRootSignatureDesc, _bigTriangleDepthRS);
	NAME_D3D12_OBJECT(_bigTriangleDepthRS);

	ComPtr<ID3DBlob> computeShader = Utils::CompileShader(
		L"BigTriangleDepthCS.hlsl",
		nullptr,
		"main",
		"cs_5_0");

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = _bigTriangleDepthRS.Get();
	psoDesc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };

	SUCCESS(DX::Device->CreateComputePipelineState(
		&psoDesc,
		IID_PPV_ARGS(&_bigTriangleDepthPSO)));
	NAME_D3D12_OBJECT(_bigTriangleDepthPSO);
}

void SoftwareRasterization::_createTriangleOpaquePSO()
{
	CD3DX12_ROOT_PARAMETER1 computeRootParameters[14] = {};
	computeRootParameters[0].InitAsConstantBufferView(0);
	CD3DX12_DESCRIPTOR_RANGE1 ranges[13] = {};

	ranges[0].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		0);
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
		1,
		3);
	computeRootParameters[4].InitAsDescriptorTable(1, &ranges[3]);

	ranges[4].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		8);
	computeRootParameters[5].InitAsDescriptorTable(1, &ranges[4]);

	ranges[5].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		9);
	computeRootParameters[6].InitAsDescriptorTable(1, &ranges[5]);

	ranges[6].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		10);
	computeRootParameters[7].InitAsDescriptorTable(1, &ranges[6]);

	ranges[7].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		11);
	computeRootParameters[8].InitAsDescriptorTable(1, &ranges[7]);

	ranges[8].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		12);
	computeRootParameters[9].InitAsDescriptorTable(1, &ranges[8]);

	ranges[9].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		0);
	computeRootParameters[10].InitAsDescriptorTable(1, &ranges[9]);

	ranges[10].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		1);
	computeRootParameters[11].InitAsDescriptorTable(1, &ranges[10]);

	ranges[11].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		2);
	computeRootParameters[12].InitAsDescriptorTable(1, &ranges[11]);

	ranges[12].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		3);
	computeRootParameters[13].InitAsDescriptorTable(1, &ranges[12]);

	D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
	D3D12_STATIC_SAMPLER_DESC* pointClampSampler = &samplers[0];
	pointClampSampler->Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	pointClampSampler->AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	pointClampSampler->AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	pointClampSampler->AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	pointClampSampler->MipLODBias = 0;
	pointClampSampler->MaxAnisotropy = 0;
	pointClampSampler->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	pointClampSampler->BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	pointClampSampler->MinLOD = 0.0f;
	pointClampSampler->MaxLOD = D3D12_FLOAT32_MAX;
	pointClampSampler->ShaderRegister = 0;
	pointClampSampler->RegisterSpace = 0;
	pointClampSampler->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC* depthSampler = &samplers[1];
	samplers[1] = samplers[0];
	depthSampler->Filter = Utils::HiZSamplerDesc.Filter;
	depthSampler->AddressU = Utils::HiZSamplerDesc.AddressU;
	depthSampler->AddressV = Utils::HiZSamplerDesc.AddressV;
	depthSampler->AddressW = Utils::HiZSamplerDesc.AddressW;
	depthSampler->BorderColor = Utils::HiZSamplerDesc.BorderColor;
	depthSampler->ShaderRegister = 1;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
	computeRootSignatureDesc.Init_1_1(
		_countof(computeRootParameters),
		computeRootParameters,
		_countof(samplers),
		samplers);

	Utils::CreateRS(computeRootSignatureDesc, _triangleOpaqueRS);
	NAME_D3D12_OBJECT(_triangleOpaqueRS);

	const D3D_SHADER_MACRO defines[] = { { "OPAQUE", "1" }, { nullptr, nullptr } };
	ComPtr<ID3DBlob> computeShader = Utils::CompileShader(
		L"TriangleOpaqueCS.hlsl",
		defines,
		"main",
		"cs_5_0");

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = _triangleOpaqueRS.Get();
	psoDesc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };

	SUCCESS(DX::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_triangleOpaquePSO)));
	NAME_D3D12_OBJECT(_triangleOpaquePSO);
}

void SoftwareRasterization::_createBigTriangleOpaquePSO()
{
	CD3DX12_ROOT_PARAMETER1 computeRootParameters[6] = {};
	computeRootParameters[0].InitAsConstantBufferView(0);
	CD3DX12_DESCRIPTOR_RANGE1 ranges[5] = {};

	ranges[0].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		1,
		0);
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
		1,
		3);
	computeRootParameters[4].InitAsDescriptorTable(1, &ranges[3]);

	ranges[4].Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
		1,
		0);
	computeRootParameters[5].InitAsDescriptorTable(1, &ranges[4]);

	D3D12_STATIC_SAMPLER_DESC pointClampSampler = {};
	pointClampSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	pointClampSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	pointClampSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	pointClampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	pointClampSampler.MipLODBias = 0;
	pointClampSampler.MaxAnisotropy = 0;
	pointClampSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	pointClampSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	pointClampSampler.MinLOD = 0.0f;
	pointClampSampler.MaxLOD = D3D12_FLOAT32_MAX;
	pointClampSampler.ShaderRegister = 0;
	pointClampSampler.RegisterSpace = 0;
	pointClampSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
	computeRootSignatureDesc.Init_1_1(
		_countof(computeRootParameters),
		computeRootParameters,
		1,
		&pointClampSampler);

	Utils::CreateRS(computeRootSignatureDesc, _bigTriangleOpaqueRS);
	NAME_D3D12_OBJECT(_bigTriangleOpaqueRS);

	const D3D_SHADER_MACRO defines[] = { { "OPAQUE", "1" }, { nullptr, nullptr } };
	ComPtr<ID3DBlob> computeShader = Utils::CompileShader(
		L"BigTriangleOpaqueCS.hlsl",
		defines,
		"main",
		"cs_5_0");

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = _bigTriangleOpaqueRS.Get();
	psoDesc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };

	SUCCESS(DX::Device->CreateComputePipelineState(
		&psoDesc,
		IID_PPV_ARGS(&_bigTriangleOpaquePSO)));
	NAME_D3D12_OBJECT(_bigTriangleOpaquePSO);
}