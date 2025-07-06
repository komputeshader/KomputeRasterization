#include "ComputeRasterization.h"

#include "DXSample.h"
#include "DXSampleHelper.h"
#include "Scene.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct DepthComputeCB
{
	XMFLOAT4X4 VP;
	XMFLOAT2 screenRes;
	float pad[46];
};
static_assert((sizeof(DepthComputeCB) % 256) == 0, "Constant Buffer size must be 256-byte aligned");

struct SceneComputeCB
{
	XMFLOAT4X4 VP;
	XMFLOAT4X4 cascadeVP[Shadows::MaxCascadeCount];
	XMFLOAT3 sunDirection;
	UINT cascadeCount;
	XMFLOAT2 screenRes;
	UINT pad0[2];
	float cascadeBias[Shadows::MaxCascadeCount];
	float cascadeSplits[Shadows::MaxCascadeCount];
	UINT pad1[24];
};
static_assert((sizeof(SceneComputeCB) % 256) == 0, "Constant Buffer size must be 256-byte aligned");

struct PackedCommand
{
	UINT InstanceCount;
	UINT StartIndexLocation;
	INT BaseVertexLocation;
	UINT StartInstanceLocation;
};

struct IndirectCommand
{
	UINT tTriangleIndex;
	UINT instanceID;
	UINT baseVertexLocation;
	D3D12_DISPATCH_ARGUMENTS arguments;
};

void ComputeRasterization::Resize(
	ID3D12Device * device,
	ID3D12Resource * depthVertexBuffer,
	ID3D12Resource * vertexBuffer,
	ID3D12Resource * indexBuffer,
	ID3D12Resource * instanceBuffer,
	Scene * scene,
	Shadows * shadows,
	std::wstring const & assetsPath,
	DXGI_FORMAT backBufferFormat,
	UINT frameCount,
	UINT width,
	UINT height)
{
	_width = width;
	_height = height;
	_frameCount = frameCount;

	// root signature
	D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
	featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
	{
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

	// depth RS + PSO
	{
		CD3DX12_ROOT_PARAMETER1 computeRootParameters[6];
		computeRootParameters[0].InitAsConstantBufferView(0);
		CD3DX12_DESCRIPTOR_RANGE1 ranges[4];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			DepthVerticesSRV, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
		computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			IndicesSRV + InstancesSRV, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
		computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);
		ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			DepthUAV, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
		computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);
		ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			DepthUAV, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
		computeRootParameters[4].InitAsDescriptorTable(1, &ranges[3]);
		// TODO: temporary
		computeRootParameters[5].InitAsConstants(4, 1);

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
		computeRootSignatureDesc.Init_1_1(_countof(computeRootParameters), computeRootParameters);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc, featureData.HighestVersion, &signature, &error));
		ThrowIfFailed(
			device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&_depthRS)));
		NAME_D3D12_OBJECT(_depthRS);

		Microsoft::WRL::ComPtr<ID3DBlob> computeShader{
			Utils::CompileShader((assetsPath + L"NaiveDepth.hlsl").c_str(), nullptr, "CSMain", "cs_5_0") };

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = _depthRS.Get();
		psoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());

		ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_depthPSO)));
		NAME_D3D12_OBJECT(_depthPSO);
	}

	// depth big triangle RS + PSO
	{
		CD3DX12_ROOT_PARAMETER1 computeRootParameters[5];
		computeRootParameters[0].InitAsConstantBufferView(0);
		CD3DX12_DESCRIPTOR_RANGE1 ranges[3];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			DepthVerticesSRV, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
		computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			IndicesSRV + InstancesSRV, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
		computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);
		ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			DepthUAV, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
		computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);
		// TODO: temporary
		computeRootParameters[4].InitAsConstants(3, 1);

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
		computeRootSignatureDesc.Init_1_1(_countof(computeRootParameters), computeRootParameters);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc, featureData.HighestVersion, &signature, &error));
		ThrowIfFailed(
			device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&_depthBigTriangleRS)));
		NAME_D3D12_OBJECT(_depthBigTriangleRS);

		Microsoft::WRL::ComPtr<ID3DBlob> computeShader{
			Utils::CompileShader((assetsPath + L"NaiveDepthBigTriangle.hlsl").c_str(), nullptr, "CSMain", "cs_5_0") };

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = _depthBigTriangleRS.Get();
		psoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());

		ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_depthBigTrianglePSO)));
		NAME_D3D12_OBJECT(_depthBigTrianglePSO);
	}

	// opaque RS + PSO
	{
		CD3DX12_ROOT_PARAMETER1 computeRootParameters[6];
		computeRootParameters[0].InitAsConstantBufferView(0);
		CD3DX12_DESCRIPTOR_RANGE1 ranges[4];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			VerticesSRV, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
		computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			IndicesSRV + InstancesSRV + DepthSRV + ShadowMapSRV, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
		computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);
		ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			RenderTargetUAV, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
		computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);
		ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			BigTrianglesUAV, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
		computeRootParameters[4].InitAsDescriptorTable(1, &ranges[3]);
		// TODO: temporary
		computeRootParameters[5].InitAsConstants(4, 1);

		D3D12_STATIC_SAMPLER_DESC pointClampSampler{};
		pointClampSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		pointClampSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		pointClampSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		pointClampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
		computeRootSignatureDesc.Init_1_1(_countof(computeRootParameters), computeRootParameters, 1, &pointClampSampler);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc, featureData.HighestVersion, &signature, &error));
		ThrowIfFailed(
			device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&_opaqueRS)));
		NAME_D3D12_OBJECT(_opaqueRS);

		D3D_SHADER_MACRO macros[] = { "OPAQUE", "1", NULL, NULL };
		Microsoft::WRL::ComPtr<ID3DBlob> computeShader{
			Utils::CompileShader((assetsPath + L"NaiveOpaque.hlsl").c_str(), macros, "CSMain", "cs_5_0") };

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = _opaqueRS.Get();
		psoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());

		ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_opaquePSO)));
		NAME_D3D12_OBJECT(_opaquePSO);
	}

	// opaque big triangle RS + PSO
	{
		CD3DX12_ROOT_PARAMETER1 computeRootParameters[5];
		computeRootParameters[0].InitAsConstantBufferView(0);
		CD3DX12_DESCRIPTOR_RANGE1 ranges[3];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			VerticesSRV, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
		computeRootParameters[1].InitAsDescriptorTable(1, &ranges[0]);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			IndicesSRV + InstancesSRV + DepthSRV + ShadowMapSRV, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
		computeRootParameters[2].InitAsDescriptorTable(1, &ranges[1]);
		ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
			RenderTargetUAV, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
		computeRootParameters[3].InitAsDescriptorTable(1, &ranges[2]);
		// TODO: temporary
		computeRootParameters[4].InitAsConstants(3, 1);

		D3D12_STATIC_SAMPLER_DESC pointClampSampler{};
		pointClampSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		pointClampSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		pointClampSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		pointClampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
		computeRootSignatureDesc.Init_1_1(_countof(computeRootParameters), computeRootParameters, 1, &pointClampSampler);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc, featureData.HighestVersion, &signature, &error));
		ThrowIfFailed(
			device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&_opaqueBigTriangleRS)));
		NAME_D3D12_OBJECT(_opaqueBigTriangleRS);

		D3D_SHADER_MACRO macros[] = { "OPAQUE", "1", NULL, NULL };
		Microsoft::WRL::ComPtr<ID3DBlob> computeShader{
			Utils::CompileShader((assetsPath + L"NaiveOpaqueBigTriangle.hlsl").c_str(), macros, "CSMain", "cs_5_0") };

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = _opaqueBigTriangleRS.Get();
		psoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());

		ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_opaqueBigTrianglePSO)));
		NAME_D3D12_OBJECT(_opaqueBigTrianglePSO);
	}

	// view heap
	D3D12_DESCRIPTOR_HEAP_DESC CBVHeapDesc = {};
	CBVHeapDesc.NumDescriptors = SRVUAVDescriptorsCount;
	CBVHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CBVHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	ThrowIfFailed(device->CreateDescriptorHeap(&CBVHeapDesc, IID_PPV_ARGS(&_CBVSRVUAVGPUHeap)));
	NAME_D3D12_OBJECT(_CBVSRVUAVGPUHeap);

	CBVHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(device->CreateDescriptorHeap(&CBVHeapDesc, IID_PPV_ARGS(&_CBVSRVUAVCPUHeap)));
	NAME_D3D12_OBJECT(_CBVSRVUAVCPUHeap);

	_CBVSRVUAVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// render target
	D3D12_RESOURCE_DESC rtDesc{};
	rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rtDesc.Alignment = 0;
	rtDesc.Width = _width;
	rtDesc.Height = _height;
	rtDesc.DepthOrArraySize = 1;
	rtDesc.MipLevels = 1;
	rtDesc.Format = backBufferFormat;
	rtDesc.SampleDesc.Count = 1;
	rtDesc.SampleDesc.Quality = 0;
	rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	{
		auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		ThrowIfFailed(device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&rtDesc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nullptr,
			IID_PPV_ARGS(&_renderTarget)
		));
		NAME_D3D12_OBJECT(_renderTarget);

		// render target UAV
		D3D12_UNORDERED_ACCESS_VIEW_DESC rtUAV{};
		rtUAV.Format = backBufferFormat;
		rtUAV.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		rtUAV.Texture2D.MipSlice = 0;
		rtUAV.Texture2D.PlaneSlice = 0;

		device->CreateUnorderedAccessView(
			_renderTarget.Get(),
			nullptr,
			&rtUAV,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
				RenderTargetUAVOffset,
				_CBVSRVUAVDescriptorSize));

		device->CreateUnorderedAccessView(
			_renderTarget.Get(),
			nullptr,
			&rtUAV,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVCPUHeap->GetCPUDescriptorHandleForHeapStart(),
				RenderTargetUAVOffset,
				_CBVSRVUAVDescriptorSize));
	}

	// z buffer
	D3D12_RESOURCE_DESC depthStencilDesc{};
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = _width;
	depthStencilDesc.Height = _height;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = _depthFormat;
	depthStencilDesc.SampleDesc.Count = 1;
	depthStencilDesc.SampleDesc.Quality = 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	{
		auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		ThrowIfFailed(device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&depthStencilDesc,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(&_depthBuffer)
		));
		NAME_D3D12_OBJECT(_depthBuffer);

		// depth UAV
		D3D12_UNORDERED_ACCESS_VIEW_DESC depthUAV{};
		depthUAV.Format = _depthFormat;
		depthUAV.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		depthUAV.Texture2D.MipSlice = 0;
		depthUAV.Texture2D.PlaneSlice = 0;

		device->CreateUnorderedAccessView(
			_depthBuffer.Get(),
			nullptr,
			&depthUAV,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
				DepthUAVOffset,
				_CBVSRVUAVDescriptorSize));

		device->CreateUnorderedAccessView(
			_depthBuffer.Get(),
			nullptr,
			&depthUAV,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVCPUHeap->GetCPUDescriptorHandleForHeapStart(),
				DepthUAVOffset,
				_CBVSRVUAVDescriptorSize));

		// depth SRV
		D3D12_SHADER_RESOURCE_VIEW_DESC depthSRV{};
		depthSRV.Format = _depthFormat;
		depthSRV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		depthSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		depthSRV.Texture2D.MostDetailedMip = 0;
		depthSRV.Texture2D.MipLevels = 1;
		depthSRV.Texture2D.PlaneSlice = 0;
		depthSRV.Texture2D.ResourceMinLODClamp = 0.0f;

		CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
			_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
			DepthSRVOffset,
			_CBVSRVUAVDescriptorSize);

		device->CreateShaderResourceView(
			_depthBuffer.Get(),
			&depthSRV,
			handle);
	}

	// depth CBV
	{
		// CB size is required to be 256-byte aligned.
		_depthCBFrameSize = sizeof(DepthComputeCB) * (1 + Shadows::CascadeCount);
		UINT constantBufferSize = _depthCBFrameSize * _frameCount;
		auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
		ThrowIfFailed(device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&_depthCB)));

		// Map and initialize the constant buffer. We don't unmap this until the
		// app closes. Keeping things mapped for the lifetime of the resource is okay.
		CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
		ThrowIfFailed(_depthCB->Map(0, &readRange, reinterpret_cast<void **>(&_depthCBData)));
	}

	//opaque CBV
	{
		// CB size is required to be 256-byte aligned.
		UINT constantBufferSize = sizeof(SceneComputeCB) * frameCount;
		auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
		ThrowIfFailed(device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&_opaqueCB)));

		// Map and initialize the constant buffer. We don't unmap this until the
		// app closes. Keeping things mapped for the lifetime of the resource is okay.
		CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
		ThrowIfFailed(_opaqueCB->Map(0, &readRange, reinterpret_cast<void **>(&_opaqueCBData)));
	}

	// depth vertices + vertices SRV
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Buffer.FirstElement = 0;
		SRVDesc.Buffer.NumElements = scene->mutualDepthVertices.size();
		SRVDesc.Buffer.StructureByteStride = sizeof(decltype(scene->mutualDepthVertices)::value_type);
		SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
			_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
			DepthVerticesSRVOffset,
			_CBVSRVUAVDescriptorSize);
		device->CreateShaderResourceView(depthVertexBuffer, &SRVDesc, handle);
	
		SRVDesc.Buffer.NumElements = scene->mutualVertices.size();
		SRVDesc.Buffer.StructureByteStride = sizeof(decltype(scene->mutualVertices)::value_type);
		handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
			VerticesSRVOffset,
			_CBVSRVUAVDescriptorSize);
		device->CreateShaderResourceView(vertexBuffer, &SRVDesc, handle);
	}

	// indices SRV
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Buffer.FirstElement = 0;
		SRVDesc.Buffer.NumElements = scene->mutualIndices.size();
		SRVDesc.Buffer.StructureByteStride = sizeof(decltype(scene->mutualIndices)::value_type);
		SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
			_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
			IndicesSRVOffset,
			_CBVSRVUAVDescriptorSize);
		device->CreateShaderResourceView(indexBuffer, &SRVDesc, handle);
	}

	// instances SRV
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Buffer.FirstElement = 0;
		SRVDesc.Buffer.NumElements = scene->mutualInstances.size();
		SRVDesc.Buffer.StructureByteStride = sizeof(decltype(scene->mutualInstances)::value_type);
		SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
		CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
			_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
			InstancesSRVOffset,
			_CBVSRVUAVDescriptorSize);
		device->CreateShaderResourceView(instanceBuffer, &SRVDesc, handle);
	}

	// shadows SRV
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		SRVDesc.Texture2DArray.MostDetailedMip = 0;
		SRVDesc.Texture2DArray.MipLevels = -1;
		SRVDesc.Texture2DArray.ArraySize = Shadows::CascadeCount;
		SRVDesc.Texture2DArray.FirstArraySlice = 0;
		CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
			_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
			ShadowMapSRVOffset,
			_CBVSRVUAVDescriptorSize);
		device->CreateShaderResourceView(
			shadows->GetShadowMapResourceUA(),
			&SRVDesc,
			handle);
	}

	// shadows UAV
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
		UAVDesc.Format = _depthFormat;
		UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
		UAVDesc.Texture2DArray.MipSlice = 0;
		UAVDesc.Texture2DArray.ArraySize = 1;
		UAVDesc.Texture2DArray.PlaneSlice = 0;
		for (UINT cascade = 0; cascade < Shadows::CascadeCount; cascade++)
		{
			UAVDesc.Texture2DArray.FirstArraySlice = cascade;

			device->CreateUnorderedAccessView(
				shadows->GetShadowMapResourceUA(),
				nullptr,
				&UAVDesc,
				CD3DX12_CPU_DESCRIPTOR_HANDLE(
					_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
					CascadeUAVOffset + cascade,
					_CBVSRVUAVDescriptorSize));

			device->CreateUnorderedAccessView(
				shadows->GetShadowMapResourceUA(),
				nullptr,
				&UAVDesc,
				CD3DX12_CPU_DESCRIPTOR_HANDLE(
					_CBVSRVUAVCPUHeap->GetCPUDescriptorHandleForHeapStart(),
					CascadeUAVOffset + cascade,
					_CBVSRVUAVDescriptorSize));
		}
	}

	// Create the command signature used for indirect drawing.
	D3D12_INDIRECT_ARGUMENT_DESC argumentDescs[2] = {};
	argumentDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
	argumentDescs[0].Constant.RootParameterIndex = 4;
	argumentDescs[0].Constant.DestOffsetIn32BitValues = 0;
	argumentDescs[0].Constant.Num32BitValuesToSet = 3;
	argumentDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

	D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc{};
	commandSignatureDesc.pArgumentDescs = argumentDescs;
	commandSignatureDesc.NumArgumentDescs = _countof(argumentDescs);
	commandSignatureDesc.ByteStride = sizeof(IndirectCommand);

	ThrowIfFailed(device->CreateCommandSignature(&commandSignatureDesc, _depthBigTriangleRS.Get(), IID_PPV_ARGS(&_depthCommandSignature)));
	NAME_D3D12_OBJECT(_depthCommandSignature);
	ThrowIfFailed(device->CreateCommandSignature(&commandSignatureDesc, _opaqueBigTriangleRS.Get(), IID_PPV_ARGS(&_opaqueCommandSignature)));
	NAME_D3D12_OBJECT(_opaqueCommandSignature);

	// Create the command buffers and UAVs to store the results of the compute work.
	// Create the unordered access views (UAVs) that store the results of the compute work.
	_commandSizePerFrame = scene->totalFaces * sizeof(IndirectCommand);
	_commandBufferCounterOffset = Utils::AlignForUavCounter(_commandSizePerFrame);

	// Allocate a buffer large enough to hold all of the indirect commands
	// for a single frame as well as a UAV counter.
	CD3DX12_RESOURCE_DESC commandBufferDesc{
		CD3DX12_RESOURCE_DESC::Buffer(_commandBufferCounterOffset + sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) };

	{
		auto prop{ CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT) };
		ThrowIfFailed(device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&commandBufferDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&_bigTrianglesCommands)));
		NAME_D3D12_OBJECT(_bigTrianglesCommands);
	}

	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
		UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
		UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		UAVDesc.Buffer.FirstElement = 0;
		UAVDesc.Buffer.NumElements = scene->totalFaces;
		UAVDesc.Buffer.StructureByteStride = sizeof(IndirectCommand);
		UAVDesc.Buffer.CounterOffsetInBytes = _commandBufferCounterOffset;
		UAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

		CD3DX12_CPU_DESCRIPTOR_HANDLE processedCommandsHandle(
			_CBVSRVUAVGPUHeap->GetCPUDescriptorHandleForHeapStart(),
			BigTrianglesUAVOffset,
			_CBVSRVUAVDescriptorSize);

		device->CreateUnorderedAccessView(
			_bigTrianglesCommands.Get(),
			_bigTrianglesCommands.Get(),
			&UAVDesc,
			processedCommandsHandle);
	}

	// Allocate a buffer that can be used to reset the UAV counters and initialize
	// it to 0.
	{
		auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT));
		ThrowIfFailed(device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&_bigTrianglesCommandsCounterReset)));
		NAME_D3D12_OBJECT(_bigTrianglesCommandsCounterReset);
	}

	UINT8 * pMappedCounterReset{ nullptr };
	CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
	ThrowIfFailed(_bigTrianglesCommandsCounterReset->Map(0, &readRange, reinterpret_cast<void**>(&pMappedCounterReset)));
	ZeroMemory(pMappedCounterReset, sizeof(UINT));
	_bigTrianglesCommandsCounterReset->Unmap(0, nullptr);
}

void ComputeRasterization::BeginPass(ID3D12GraphicsCommandList * commandList, UINT frameIndex, Scene * scene, Shadows * shadows)
{
	Camera const & camera = scene->camera;

	DepthComputeCB depthData{};
	depthData.VP = camera.GetViewProjectionMatrixF();
	depthData.screenRes = { static_cast<float>(_width), static_cast<float>(_height) };
	memcpy(_depthCBData + frameIndex * _depthCBFrameSize, &depthData, sizeof(DepthComputeCB));

	for (UINT cascade = 0; cascade < Shadows::CascadeCount; cascade++)
	{
		depthData.VP = shadows->GetViewProjectionMatrixF(cascade);
		depthData.screenRes = { static_cast<float>(Shadows::ShadowMapRes), static_cast<float>(Shadows::ShadowMapRes) };
		memcpy(
			_depthCBData + frameIndex * _depthCBFrameSize + (1 + cascade) * sizeof(DepthComputeCB),
			&depthData,
			sizeof(DepthComputeCB));
	}

	SceneComputeCB sceneData{};
	sceneData.VP = camera.GetViewProjectionMatrixF();
	XMStoreFloat3(&sceneData.sunDirection, XMVector3Normalize(XMLoadFloat3(&shadows->GetLightDirection())));
	sceneData.screenRes = { static_cast<float>(_width), static_cast<float>(_height) };
	sceneData.cascadeCount = Shadows::CascadeCount;
	for (UINT cascade = 0; cascade < Shadows::CascadeCount; cascade++)
	{
		sceneData.cascadeVP[cascade] = shadows->GetViewProjectionMatrixF(cascade);
		sceneData.cascadeBias[cascade] = shadows->GetCascadeBias(cascade);
		sceneData.cascadeSplits[cascade] = (camera.GetFarZ() - camera.GetNearZ()) * shadows->GetCascadeSplit(cascade);
	}

	memcpy(_opaqueCBData + frameIndex * sizeof(SceneComputeCB), &sceneData, sizeof(SceneComputeCB));
}

void ComputeRasterization::Draw(ID3D12GraphicsCommandList * commandList, UINT frameIndex, Scene * scene, Shadows * shadows)
{
	BeginPass(commandList, frameIndex, scene, shadows);

	ID3D12DescriptorHeap* ppHeaps[] = { _CBVSRVUAVGPUHeap.Get() };
	commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

	_clearMDICounter(commandList);

	{
		CD3DX12_RESOURCE_BARRIER barriers[] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(_depthBuffer.Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(shadows->GetShadowMapResourceUA(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			CD3DX12_RESOURCE_BARRIER::Transition(_bigTrianglesCommands.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		commandList->ResourceBarrier(_countof(barriers), barriers);
	}

	PIXBeginEvent(commandList, 0, L"Depth pass");

	{
		UINT clearValue[] = { Utils::AsUINT(0.0f), Utils::AsUINT(0.0f), Utils::AsUINT(0.0f), Utils::AsUINT(0.0f) };
		commandList->ClearUnorderedAccessViewUint(
			CD3DX12_GPU_DESCRIPTOR_HANDLE(_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(), DepthUAVOffset, _CBVSRVUAVDescriptorSize),
			CD3DX12_CPU_DESCRIPTOR_HANDLE(_CBVSRVUAVCPUHeap->GetCPUDescriptorHandleForHeapStart(), DepthUAVOffset, _CBVSRVUAVDescriptorSize),
			_depthBuffer.Get(),
			clearValue,
			0,
			nullptr);
	}

	commandList->SetComputeRootSignature(_depthRS.Get());
	commandList->SetPipelineState(_depthPSO.Get());

	commandList->SetComputeRootConstantBufferView(0, _depthCB->GetGPUVirtualAddress() + frameIndex * _depthCBFrameSize);
	commandList->SetComputeRootDescriptorTable(1,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			DepthVerticesSRVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(2,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			IndicesSRVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(3,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			DepthUAVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(4,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			BigTrianglesUAVOffset,
			_CBVSRVUAVDescriptorSize));

	for (auto const & mesh : scene->meshes)
	{
		for (auto const & submesh : mesh.GetSubmeshes())
		{
			// TODO: temporary
			UINT commandData[] = {
				submesh.indexCountPerInstance,
				submesh.startIndexLocation,
				submesh.baseVertexLocation,
				submesh.startInstanceLocation
			};
			commandList->SetComputeRoot32BitConstants(5, _countof(commandData), commandData, 0);

			assert(submesh.indexCountPerInstance % 3 == 0);
			UINT trianglesCount = submesh.indexCountPerInstance / 3;
			commandList->Dispatch(Utils::DispatchSize(GroupThreadsX, trianglesCount), submesh.instanceCount, 1);
		}
	}

	{
		CD3DX12_RESOURCE_BARRIER barriers[] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(_bigTrianglesCommands.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
		};
		commandList->ResourceBarrier(_countof(barriers), barriers);
	}

	commandList->SetComputeRootSignature(_depthBigTriangleRS.Get());
	commandList->SetPipelineState(_depthBigTrianglePSO.Get());

	commandList->SetComputeRootConstantBufferView(0, _depthCB->GetGPUVirtualAddress() + frameIndex * _depthCBFrameSize);
	commandList->SetComputeRootDescriptorTable(1,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			DepthVerticesSRVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(2,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			IndicesSRVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(3,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			DepthUAVOffset,
			_CBVSRVUAVDescriptorSize));

	commandList->ExecuteIndirect(
		_depthCommandSignature.Get(),
		scene->totalFaces,
		_bigTrianglesCommands.Get(),
		0,
		_bigTrianglesCommands.Get(),
		_commandBufferCounterOffset);

	PIXEndEvent(commandList);

	PIXBeginEvent(commandList, 0, L"Shadows pass");

	{
		UINT clearValue[] = { Utils::AsUINT(0.0f), Utils::AsUINT(0.0f), Utils::AsUINT(0.0f), Utils::AsUINT(0.0f) };
		for (UINT cascade = 0; cascade < Shadows::CascadeCount; cascade++)
		{
			commandList->ClearUnorderedAccessViewUint(
				CD3DX12_GPU_DESCRIPTOR_HANDLE(_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
					CascadeUAVOffset + cascade,
					_CBVSRVUAVDescriptorSize),
				CD3DX12_CPU_DESCRIPTOR_HANDLE(_CBVSRVUAVCPUHeap->GetCPUDescriptorHandleForHeapStart(),
					CascadeUAVOffset + cascade,
					_CBVSRVUAVDescriptorSize),
				shadows->GetShadowMapResourceUA(),
				clearValue,
				0,
				nullptr);
		}
	}

	for (UINT cascade = 0; cascade < Shadows::CascadeCount; cascade++)
	{
		{
			CD3DX12_RESOURCE_BARRIER barriers[] =
			{
				CD3DX12_RESOURCE_BARRIER::Transition(_bigTrianglesCommands.Get(),
				D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST),
			};
			commandList->ResourceBarrier(_countof(barriers), barriers);
		}

		_clearMDICounter(commandList);

		{
			CD3DX12_RESOURCE_BARRIER barriers[] =
			{
				CD3DX12_RESOURCE_BARRIER::Transition(_bigTrianglesCommands.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			};
			commandList->ResourceBarrier(_countof(barriers), barriers);
		}

		commandList->SetComputeRootSignature(_depthRS.Get());
		commandList->SetPipelineState(_depthPSO.Get());

		commandList->SetComputeRootConstantBufferView(0,
			_depthCB->GetGPUVirtualAddress() + frameIndex * _depthCBFrameSize + (1 + cascade) * sizeof(DepthComputeCB));
		commandList->SetComputeRootDescriptorTable(1,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
				DepthVerticesSRVOffset,
				_CBVSRVUAVDescriptorSize));
		commandList->SetComputeRootDescriptorTable(2,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
				IndicesSRVOffset,
				_CBVSRVUAVDescriptorSize));
		commandList->SetComputeRootDescriptorTable(3,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
				CascadeUAVOffset + cascade,
				_CBVSRVUAVDescriptorSize));
		commandList->SetComputeRootDescriptorTable(4,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
				BigTrianglesUAVOffset,
				_CBVSRVUAVDescriptorSize));

		for (auto const & mesh : scene->meshes)
		{
			for (auto const & submesh : mesh.GetSubmeshes())
			{
				// TODO: temporary
				UINT commandData[] = {
					submesh.indexCountPerInstance,
					submesh.startIndexLocation,
					submesh.baseVertexLocation,
					submesh.startInstanceLocation
				};
				commandList->SetComputeRoot32BitConstants(5, _countof(commandData), commandData, 0);

				assert(submesh.indexCountPerInstance % 3 == 0);
				UINT trianglesCount = submesh.indexCountPerInstance / 3;
				commandList->Dispatch(Utils::DispatchSize(GroupThreadsX, trianglesCount), submesh.instanceCount, 1);
			}
		}

		{
			CD3DX12_RESOURCE_BARRIER barriers[] =
			{
				CD3DX12_RESOURCE_BARRIER::Transition(_bigTrianglesCommands.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
			};
			commandList->ResourceBarrier(_countof(barriers), barriers);
		}

		commandList->SetComputeRootSignature(_depthBigTriangleRS.Get());
		commandList->SetPipelineState(_depthBigTrianglePSO.Get());

		commandList->SetComputeRootConstantBufferView(0,
			_depthCB->GetGPUVirtualAddress() + frameIndex * _depthCBFrameSize + (1 + cascade) * sizeof(DepthComputeCB));
		commandList->SetComputeRootDescriptorTable(1,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
				DepthVerticesSRVOffset,
				_CBVSRVUAVDescriptorSize));
		commandList->SetComputeRootDescriptorTable(2,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
				IndicesSRVOffset,
				_CBVSRVUAVDescriptorSize));
		commandList->SetComputeRootDescriptorTable(3,
			CD3DX12_GPU_DESCRIPTOR_HANDLE(
				_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
				CascadeUAVOffset + cascade,
				_CBVSRVUAVDescriptorSize));

		commandList->ExecuteIndirect(
			_depthCommandSignature.Get(),
			scene->totalFaces,
			_bigTrianglesCommands.Get(),
			0,
			_bigTrianglesCommands.Get(),
			_commandBufferCounterOffset);
	}

	PIXEndEvent(commandList);

	{
		CD3DX12_RESOURCE_BARRIER barriers[] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(_depthBuffer.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(shadows->GetShadowMapResourceUA(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
		};
		commandList->ResourceBarrier(_countof(barriers), barriers);
	}

	PIXBeginEvent(commandList, 0, L"Opaque pass");

	{
		commandList->ClearUnorderedAccessViewFloat(
			CD3DX12_GPU_DESCRIPTOR_HANDLE(_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
				RenderTargetUAVOffset,
				_CBVSRVUAVDescriptorSize),
			CD3DX12_CPU_DESCRIPTOR_HANDLE(_CBVSRVUAVCPUHeap->GetCPUDescriptorHandleForHeapStart(),
				RenderTargetUAVOffset,
				_CBVSRVUAVDescriptorSize),
			_renderTarget.Get(),
			SkyColor,
			0,
			nullptr);
	}
	
	{
		CD3DX12_RESOURCE_BARRIER barriers[] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(_bigTrianglesCommands.Get(),
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST),
		};
		commandList->ResourceBarrier(_countof(barriers), barriers);
	}

	_clearMDICounter(commandList);

	{
		CD3DX12_RESOURCE_BARRIER barriers[] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(_bigTrianglesCommands.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		commandList->ResourceBarrier(_countof(barriers), barriers);
	}

	commandList->SetComputeRootSignature(_opaqueRS.Get());
	commandList->SetPipelineState(_opaquePSO.Get());

	commandList->SetComputeRootConstantBufferView(0, _opaqueCB->GetGPUVirtualAddress() + frameIndex * sizeof(SceneComputeCB));
	commandList->SetComputeRootDescriptorTable(1,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			VerticesSRVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(2,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			IndicesSRVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(3,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			RenderTargetUAVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(4,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			BigTrianglesUAVOffset,
			_CBVSRVUAVDescriptorSize));

	for (auto const & mesh : scene->meshes)
	{
		for (auto const & submesh : mesh.GetSubmeshes())
		{
			// TODO: temporary
			UINT commandData[] = {
				submesh.indexCountPerInstance,
				submesh.startIndexLocation,
				submesh.baseVertexLocation,
				submesh.startInstanceLocation
			};
			commandList->SetComputeRoot32BitConstants(5, _countof(commandData), commandData, 0);

			assert(submesh.indexCountPerInstance % 3 == 0);
			UINT trianglesCount = submesh.indexCountPerInstance / 3;
			commandList->Dispatch(Utils::DispatchSize(GroupThreadsX, trianglesCount), submesh.instanceCount, 1);
		}
	}

	{
		CD3DX12_RESOURCE_BARRIER barriers[] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(_bigTrianglesCommands.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
		};
		commandList->ResourceBarrier(_countof(barriers), barriers);
	}

	commandList->SetComputeRootSignature(_opaqueBigTriangleRS.Get());
	commandList->SetPipelineState(_opaqueBigTrianglePSO.Get());

	commandList->SetComputeRootConstantBufferView(0, _opaqueCB->GetGPUVirtualAddress() + frameIndex * sizeof(SceneComputeCB));
	commandList->SetComputeRootDescriptorTable(1,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			VerticesSRVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(2,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			IndicesSRVOffset,
			_CBVSRVUAVDescriptorSize));
	commandList->SetComputeRootDescriptorTable(3,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(
			_CBVSRVUAVGPUHeap->GetGPUDescriptorHandleForHeapStart(),
			RenderTargetUAVOffset,
			_CBVSRVUAVDescriptorSize));

	commandList->ExecuteIndirect(
		_opaqueCommandSignature.Get(),
		scene->totalFaces,
		_bigTrianglesCommands.Get(),
		0,
		_bigTrianglesCommands.Get(),
		_commandBufferCounterOffset);

	PIXEndEvent(commandList);

	ThrowIfFailed(commandList->Close());
}

void ComputeRasterization::EndPass(ID3D12GraphicsCommandList * commandList)
{

}

void ComputeRasterization::_clearMDICounter(ID3D12GraphicsCommandList * commandList)
{
	commandList->CopyBufferRegion(
		_bigTrianglesCommands.Get(),
		_commandBufferCounterOffset,
		_bigTrianglesCommandsCounterReset.Get(), 0, sizeof(UINT));
}