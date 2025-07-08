#include "HardwareRasterization.h"
#include "DescriptorManager.h"
#include "ForwardRenderer.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct SceneCB
{
	XMFLOAT4X4 VP;
	XMFLOAT4X4 cascadeVP[MAX_CASCADES_COUNT];
	XMFLOAT4 sunDirection;
	float cascadeBias[MAX_CASCADES_COUNT];
	float cascadeSplits[MAX_CASCADES_COUNT];
	int showCascades;
	int showMeshlets;
	int cascadesCount;
	float shadowsDistance;
	float pad[24];
};
static_assert(
	(sizeof(SceneCB) % 256) == 0,
	"Constant Buffer size must be 256-byte aligned");

void HardwareRasterization::Resize(
	ForwardRenderer* renderer,
	int width,
	int height)
{
	_renderer = renderer;

	_width = width;
	_height = height;

	_viewport = CD3DX12_VIEWPORT(
		0.0f,
		0.0f,
		static_cast<float>(_width),
		static_cast<float>(_height));
	_scissorRect = CD3DX12_RECT(
		0,
		0,
		static_cast<LONG>(_width),
		static_cast<LONG>(_height));

	_createDepthBufferResources();
	_loadAssets();
}

void HardwareRasterization::_createDepthBufferResources()
{
	D3D12_RESOURCE_DESC depthStencilDesc = {};
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = _width;
	depthStencilDesc.Height = _height;
	depthStencilDesc.DepthOrArraySize = 1;
	// 0 for maximum number of mips
	depthStencilDesc.MipLevels = 0;
	depthStencilDesc.Format = _depthFormat;
	depthStencilDesc.SampleDesc.Count = 1;
	depthStencilDesc.SampleDesc.Quality = 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optimizedClear = {};
	optimizedClear.Format = _depthFormat;
	optimizedClear.DepthStencil.Depth =
		Scene::CurrentScene->camera.ReverseZ() ? 0.0 : 1.0f;
	optimizedClear.DepthStencil.Stencil = 0;
	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	SUCCESS(DX::Device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&optimizedClear,
		IID_PPV_ARGS(&_depthBuffer)));
	NAME_D3D12_OBJECT(_depthBuffer);

	DX::Device->CreateDepthStencilView(
		_depthBuffer.Get(),
		nullptr,
		Descriptors::DS.GetCPUHandle(HWRDepthDSV));
}

void HardwareRasterization::_loadAssets()
{
	_createHWRRS();
	_createDepthPassPSO();
	_createOpaquePassPSO();
	_createMDIStuff();

	// depth pass + cascades
	_depthSceneCBFrameSize = sizeof(DepthSceneCB) * MAX_FRUSTUMS_COUNT;
	Utils::CreateCBResources(
		_depthSceneCBFrameSize * DX::FramesCount,
		reinterpret_cast<void**>(&_depthSceneCBData),
		_depthSceneCB);

	Utils::CreateCBResources(
		sizeof(SceneCB) * DX::FramesCount,
		reinterpret_cast<void**>(&_sceneCBData),
		_sceneCB);
}

void HardwareRasterization::_createMDIStuff()
{
	D3D12_INDIRECT_ARGUMENT_DESC argumentDescs[2] = {};
	argumentDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
	argumentDescs[0].Constant.RootParameterIndex = 1;
	argumentDescs[0].Constant.DestOffsetIn32BitValues = 0;
	argumentDescs[0].Constant.Num32BitValuesToSet = 1;
	argumentDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

	D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc = {};
	commandSignatureDesc.pArgumentDescs = argumentDescs;
	commandSignatureDesc.NumArgumentDescs = _countof(argumentDescs);
	commandSignatureDesc.ByteStride = sizeof(IndirectCommand);

	SUCCESS(DX::Device->CreateCommandSignature(
		&commandSignatureDesc,
		_HWRRS.Get(),
		IID_PPV_ARGS(&_commandSignature)));
	NAME_D3D12_OBJECT(_commandSignature);
}

void HardwareRasterization::Update()
{
	Camera& camera = Scene::CurrentScene->camera;

	DepthSceneCB depthSceneCB = {};
	depthSceneCB.VP = camera.GetVP();
	memcpy(
		_depthSceneCBData + DX::FrameIndex * _depthSceneCBFrameSize,
		&depthSceneCB,
		sizeof(DepthSceneCB));

	SceneCB sceneCB = {};
	sceneCB.VP = camera.GetVP();
	sceneCB.showCascades = Shadows::Sun.ShowCascades() ? 1 : 0;
	sceneCB.showMeshlets = Settings::ShowMeshlets ? 1 : 0;
	sceneCB.cascadesCount = Settings::CascadesCount;
	sceneCB.shadowsDistance = Shadows::Sun.GetShadowDistance();
	XMStoreFloat4(
		&sceneCB.sunDirection,
		XMVector3Normalize(XMLoadFloat3(&Scene::CurrentScene->lightDirection)));

	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		memcpy(
			_depthSceneCBData + DX::FrameIndex * _depthSceneCBFrameSize +
			(1 + cascade) * sizeof(DepthSceneCB),
			&Shadows::Sun.GetCascadeVP(cascade),
			sizeof(XMFLOAT4X4));
		sceneCB.cascadeVP[cascade] = Shadows::Sun.GetCascadeVP(cascade);
		sceneCB.cascadeBias[cascade] = Shadows::Sun.GetCascadeBias(cascade);
		sceneCB.cascadeSplits[cascade] = Shadows::Sun.GetCascadeSplit(cascade);
	}

	memcpy(
		_sceneCBData + DX::FrameIndex * sizeof(SceneCB),
		&sceneCB,
		sizeof(SceneCB));
}

void HardwareRasterization::Draw(ID3D12Resource* renderTarget)
{
	_beginFrame();
	_drawDepth();
	_drawShadows();
	_drawOpaque(renderTarget);
	_endFrame();
}

void HardwareRasterization::_beginFrame()
{
	COMMAND_LIST->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	COMMAND_LIST->IASetIndexBuffer(&Scene::CurrentScene->indicesGPU.GetIBView());
}

void HardwareRasterization::_drawDepth()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"Draw Depth");

	COMMAND_LIST->SetGraphicsRootSignature(_HWRRS.Get());
	COMMAND_LIST->SetPipelineState(_depthPSO.Get());
	COMMAND_LIST->SetGraphicsRootConstantBufferView(
		0,
		_depthSceneCB->GetGPUVirtualAddress() + DX::FrameIndex * _depthSceneCBFrameSize);
	COMMAND_LIST->SetGraphicsRootDescriptorTable(
		2,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	COMMAND_LIST->IASetVertexBuffers(0, 1, &Scene::CurrentScene->positionsGPU.GetVBView());
	COMMAND_LIST->RSSetViewports(1, &_viewport);
	COMMAND_LIST->RSSetScissorRects(1, &_scissorRect);
	auto DSVHandle = Descriptors::DS.GetCPUHandle(HWRDepthDSV);
	COMMAND_LIST->OMSetRenderTargets(0, nullptr, FALSE, &DSVHandle);
	COMMAND_LIST->ClearDepthStencilView(
		DSVHandle,
		D3D12_CLEAR_FLAG_DEPTH,
		Scene::CurrentScene->camera.ReverseZ() ? 0.0f : 1.0f,
		0,
		0,
		nullptr);

	if (Settings::CullingEnabled)
	{
		COMMAND_LIST->ExecuteIndirect(
			_commandSignature.Get(),
			Scene::CurrentScene->meshesMetaCPU.size(),
			_renderer->GetCulledCommands(DX::FrameIndex, 0),
			0,
			_renderer->GetCulledCommandsCounter(DX::FrameIndex, 0),
			0);
	}
	else
	{
		for (const auto& prefab : Scene::CurrentScene->prefabs)
		{
			for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
			{
				const auto& currentMesh = Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];
				UINT commandData[] =
				{
					currentMesh.startInstanceLocation
				};
				COMMAND_LIST->SetGraphicsRoot32BitConstants(1, _countof(commandData), commandData, 0);
				COMMAND_LIST->DrawIndexedInstanced(
					currentMesh.indexCountPerInstance,
					currentMesh.instanceCount,
					currentMesh.startIndexLocation,
					currentMesh.baseVertexLocation,
					0);
			}
		}
	}

	_renderer->PreparePrevFrameDepth(_depthBuffer.Get());

	PIXEndEvent(COMMAND_LIST.Get());
}

void HardwareRasterization::_drawShadows()
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"Draw Shadows");

	CD3DX12_RESOURCE_BARRIER barriers[1] = {};
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		Shadows::Sun.GetShadowMapHWR(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_DEPTH_WRITE);
	COMMAND_LIST->ResourceBarrier(_countof(barriers), barriers);

	COMMAND_LIST->SetGraphicsRootSignature(_HWRRS.Get());
	COMMAND_LIST->SetPipelineState(Shadows::Sun.GetPSO());
	COMMAND_LIST->RSSetViewports(1, &Shadows::Sun.GetViewport());
	COMMAND_LIST->RSSetScissorRects(1, &Shadows::Sun.GetScissorRect());

	for (int cascade = 1; cascade <= Settings::CascadesCount; cascade++)
	{
		COMMAND_LIST->SetGraphicsRootConstantBufferView(
			0,
			_depthSceneCB->GetGPUVirtualAddress() +
			DX::FrameIndex * _depthSceneCBFrameSize +
			cascade * sizeof(DepthSceneCB));
		COMMAND_LIST->SetGraphicsRootDescriptorTable(
			2,
			Settings::CullingEnabled
			? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + cascade + DX::FrameIndex * PerFrameDescriptorsCount)
			: Scene::CurrentScene->instancesGPU.GetSRV());
		auto shadowMapDSVHandle = Descriptors::DS.GetCPUHandle(CascadeDSV + cascade - 1);
		COMMAND_LIST->OMSetRenderTargets(
			0,
			nullptr,
			FALSE,
			&shadowMapDSVHandle);
		COMMAND_LIST->ClearDepthStencilView(shadowMapDSVHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

		if (Settings::CullingEnabled)
		{
			COMMAND_LIST->ExecuteIndirect(
				_commandSignature.Get(),
				Scene::CurrentScene->meshesMetaCPU.size(),
				_renderer->GetCulledCommands(DX::FrameIndex, cascade),
				0,
				_renderer->GetCulledCommandsCounter(DX::FrameIndex, cascade),
				0);
		}
		else
		{
			for (const auto& prefab : Scene::CurrentScene->prefabs)
			{
				for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
				{
					const auto& currentMesh = Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];
					UINT commandData[] =
					{
						currentMesh.startInstanceLocation
					};
					COMMAND_LIST->SetGraphicsRoot32BitConstants(1, _countof(commandData), commandData, 0);
					COMMAND_LIST->DrawIndexedInstanced(
						currentMesh.indexCountPerInstance,
						currentMesh.instanceCount,
						currentMesh.startIndexLocation,
						currentMesh.baseVertexLocation,
						0);
				}
			}
		}
	}

	if (Settings::ShadowsHiZCullingEnabled)
	{
		Shadows::Sun.PreparePrevFrameShadowMap();
	}
	else
	{
		barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
			Shadows::Sun.GetShadowMapHWR(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		COMMAND_LIST->ResourceBarrier(1, barriers);
	}

	PIXEndEvent(COMMAND_LIST.Get());
}

void HardwareRasterization::_drawOpaque(ID3D12Resource* renderTarget)
{
	PIXBeginEvent(COMMAND_LIST.Get(), 0, L"Draw Opaque");

	CD3DX12_RESOURCE_BARRIER barriers[] =
	{
		CD3DX12_RESOURCE_BARRIER::Transition(
			renderTarget,
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_RENDER_TARGET)
	};
	COMMAND_LIST->ResourceBarrier(_countof(barriers), barriers);

	COMMAND_LIST->SetGraphicsRootSignature(_HWRRS.Get());
	COMMAND_LIST->SetPipelineState(_opaquePSO.Get());
	COMMAND_LIST->RSSetViewports(1, &_viewport);
	COMMAND_LIST->RSSetScissorRects(1, &_scissorRect);
	D3D12_VERTEX_BUFFER_VIEW VBVs[] =
	{
		Scene::CurrentScene->positionsGPU.GetVBView(),
		Scene::CurrentScene->normalsGPU.GetVBView(),
		Scene::CurrentScene->colorsGPU.GetVBView(),
		Scene::CurrentScene->texcoordsGPU.GetVBView()
	};
	COMMAND_LIST->IASetVertexBuffers(0, _countof(VBVs), VBVs);
	COMMAND_LIST->SetGraphicsRootConstantBufferView(
		0, _sceneCB->GetGPUVirtualAddress() + DX::FrameIndex * sizeof(SceneCB));
	COMMAND_LIST->SetGraphicsRootDescriptorTable(
		2,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	COMMAND_LIST->SetGraphicsRootDescriptorTable(3, Descriptors::SV.GetGPUHandle(HWRShadowMapSRV));
	auto DSVHandle = Descriptors::DS.GetCPUHandle(HWRDepthDSV);
	auto RTVHandle = Descriptors::RT.GetCPUHandle(ForwardRendererRTV + DX::FrameIndex);
	COMMAND_LIST->OMSetRenderTargets(1, &RTVHandle, FALSE, &DSVHandle);
	COMMAND_LIST->ClearRenderTargetView(RTVHandle, SkyColor, 0, nullptr);

	if (Settings::CullingEnabled)
	{
		COMMAND_LIST->ExecuteIndirect(
			_commandSignature.Get(),
			Scene::CurrentScene->meshesMetaCPU.size(),
			_renderer->GetCulledCommands(DX::FrameIndex, 0),
			0,
			_renderer->GetCulledCommandsCounter(DX::FrameIndex, 0),
			0);
	}
	else
	{
		for (const auto& prefab : Scene::CurrentScene->prefabs)
		{
			for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
			{
				const auto& currentMesh = Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];
				UINT commandData[] =
				{
					currentMesh.startInstanceLocation
				};
				COMMAND_LIST->SetGraphicsRoot32BitConstants(1, _countof(commandData), commandData, 0);
				COMMAND_LIST->DrawIndexedInstanced(
					currentMesh.indexCountPerInstance,
					currentMesh.instanceCount,
					currentMesh.startIndexLocation,
					currentMesh.baseVertexLocation,
					0);
			}
		}
	}

	PIXEndEvent(COMMAND_LIST.Get());
}

void HardwareRasterization::_endFrame()
{

}

void HardwareRasterization::_createHWRRS()
{
	CD3DX12_ROOT_PARAMETER1 rootParameters[4] = {};
	rootParameters[0].InitAsConstantBufferView(0);
	rootParameters[1].InitAsConstants(1, 1);
	CD3DX12_DESCRIPTOR_RANGE1 ranges[2] = {};
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	rootParameters[2].InitAsDescriptorTable(
		1,
		&ranges[0],
		D3D12_SHADER_VISIBILITY_VERTEX);
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
	rootParameters[3].InitAsDescriptorTable(
		1,
		&ranges[1],
		D3D12_SHADER_VISIBILITY_PIXEL);

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
	pointClampSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init_1_1(
		_countof(rootParameters),
		rootParameters,
		1,
		&pointClampSampler,
		rootSignatureFlags);

	Utils::CreateRS(rootSignatureDesc, _HWRRS);
	NAME_D3D12_OBJECT(_HWRRS);
}

void HardwareRasterization::_createDepthPassPSO()
{
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
	{
		{
			"POSITION",
			0,
			DXGI_FORMAT_R32G32B32_FLOAT,
			0,
			0,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
			0
		}
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = _HWRRS.Get();
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthFunc = Scene::CurrentScene->camera.ReverseZ()
		? D3D12_COMPARISON_FUNC_GREATER
		: D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DSVFormat = _depthFormat;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.SampleDesc.Count = 1;

	Utils::ShaderHelper vertexShader(Utils::GetAssetFullPath(L"DrawDepthVS.cso").c_str());

	psoDesc.VS = { vertexShader.GetData(), vertexShader.GetSize() };
	SUCCESS(DX::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_depthPSO)));
	NAME_D3D12_OBJECT(_depthPSO);
}

void HardwareRasterization::_createOpaquePassPSO()
{
	Utils::ShaderHelper vertexShader(Utils::GetAssetFullPath(L"DrawOpaqueVS.cso").c_str());
	Utils::ShaderHelper pixelShader(Utils::GetAssetFullPath(L"DrawOpaquePS.cso").c_str());

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
	{
		{
			"POSITION",
			0,
			DXGI_FORMAT_R32G32B32_FLOAT,
			0,
			0,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
			0
		},
		{
			"NORMAL",
			0,
			DXGI_FORMAT_R32_UINT,
			1,
			0,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
			0
		},
		{
			"COLOR",
			0,
			DXGI_FORMAT_R32G32_UINT,
			2,
			0,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
			0
		},
		{
			"TEXCOORD",
			0,
			DXGI_FORMAT_R32_UINT,
			3,
			0,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
			0
		}
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = _HWRRS.Get();
	psoDesc.VS = { vertexShader.GetData(), vertexShader.GetSize() };
	psoDesc.PS = { pixelShader.GetData(), pixelShader.GetSize() };
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	// early z pass was made
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DSVFormat = _depthFormat;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = Settings::BackBufferFormat;
	psoDesc.SampleDesc.Count = 1;
	SUCCESS(DX::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_opaquePSO)));
	NAME_D3D12_OBJECT(_opaquePSO);
}