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
	"Constant Buffer size must be 256-byte aligned.");

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
	if (DX::WaveOpsSupported)
	{
		_createOverdrawResources();
	}
	_loadAssets();
}

void HardwareRasterization::_createDepthBufferResources()
{
	auto depthStencilDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		_depthFormat,
		_width,
		_height,
		1,
		0,
		1,
		0,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	auto optimizedClear = CD3DX12_CLEAR_VALUE(
		_depthFormat,
		Scene::CurrentScene->camera.ReverseZ() ? 0.0f : 1.0f,
		0);
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

void HardwareRasterization::_createOverdrawResources()
{
	const unsigned int quadWidth = (_width + 1) / 2;
	const unsigned int quadHeight = (_height + 1) / 2;
	auto overdrawDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32_UINT,
		quadWidth,
		quadHeight,
		1,
		1,
		1,
		0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	SUCCESS(DX::Device->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&overdrawDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&_overdrawBuffer)));
	NAME_D3D12_OBJECT(_overdrawBuffer);

	auto overdrawUAV = CD3DX12_UNORDERED_ACCESS_VIEW_DESC::Tex2D(
		DXGI_FORMAT_R32_UINT);
	DX::Device->CreateUnorderedAccessView(
		_overdrawBuffer.Get(),
		nullptr,
		&overdrawUAV,
		Descriptors::SV.GetCPUHandle(HWROverdrawUAV));
	DX::Device->CreateUnorderedAccessView(
		_overdrawBuffer.Get(),
		nullptr,
		&overdrawUAV,
		Descriptors::NonSV.GetCPUHandle(HWROverdrawUAV));

	auto overdrawSRV = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(
		DXGI_FORMAT_R32_UINT,
		1);
	DX::Device->CreateShaderResourceView(
		_overdrawBuffer.Get(),
		&overdrawSRV,
		Descriptors::SV.GetCPUHandle(HWROverdrawSRV));
}

void HardwareRasterization::_loadAssets()
{
	_createHWRRS();
	_createDepthPassPSO();
	_createOpaquePassPSO();
	if (DX::WaveOpsSupported)
	{
		_createOverdrawRS();
		_createOverdrawPassPSO();
		_createOverdrawDisplayRS();
		_createOverdrawDisplayPSO();
	}
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

	if (DX::WaveOpsSupported)
	{
		SUCCESS(DX::Device->CreateCommandSignature(
			&commandSignatureDesc,
			_overdrawRS.Get(),
			IID_PPV_ARGS(&_overdrawCommandSignature)));
		NAME_D3D12_OBJECT(_overdrawCommandSignature);
	}
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

void HardwareRasterization::DrawDepths()
{
	_beginFrame();
	_drawDepth();
	_drawShadows();
}

void HardwareRasterization::DrawOpaque(ID3D12Resource* renderTarget)
{
	_beginFrame();
	if (Settings::ShowOverdraw && DX::WaveOpsSupported)
	{
		_drawOverdraw(renderTarget);
	}
	else
	{
		_drawOpaque(renderTarget);
	}
	_endFrame();
}

void HardwareRasterization::_beginFrame()
{
	COMMAND_LIST->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	COMMAND_LIST->IASetIndexBuffer(&Scene::CurrentScene->indicesGPU.GetIBView());
}

void HardwareRasterization::_drawDepth()
{
	PIXScopedEvent(COMMAND_LIST.Get(), 0, L"Draw Depth");

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
			static_cast<unsigned int>(Scene::CurrentScene->meshesMetaCPU.size()),
			_renderer->GetCulledCommands(DX::FrameIndex),
			_renderer->GetCulledCommandsOffset(0),
			_renderer->GetCulledCommandsCounters(DX::FrameIndex),
			_renderer->GetCulledCommandsCountersOffset(0));
	}
	else
	{
		for (const auto& prefab : Scene::CurrentScene->prefabs)
		{
			for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
			{
				const auto& currentMesh = Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];
				unsigned int commandData[] =
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

	if (Settings::CameraHiZCullingEnabled)
	{
		_renderer->PreparePrevFrameDepth(_depthBuffer.Get());
	}
}

void HardwareRasterization::_drawShadows()
{
	PIXScopedEvent(COMMAND_LIST.Get(), 0, L"Draw Shadows");

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
				static_cast<unsigned int>(Scene::CurrentScene->meshesMetaCPU.size()),
				_renderer->GetCulledCommands(DX::FrameIndex),
				_renderer->GetCulledCommandsOffset(cascade),
				_renderer->GetCulledCommandsCounters(DX::FrameIndex),
				_renderer->GetCulledCommandsCountersOffset(cascade));
		}
		else
		{
			for (const auto& prefab : Scene::CurrentScene->prefabs)
			{
				for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
				{
					const auto& currentMesh = Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];
					unsigned int commandData[] =
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
}

void HardwareRasterization::_drawOpaque(ID3D12Resource* renderTarget)
{
	PIXScopedEvent(COMMAND_LIST.Get(), 0, L"Draw Opaque");

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
			static_cast<unsigned int>(Scene::CurrentScene->meshesMetaCPU.size()),
			_renderer->GetCulledCommands(DX::FrameIndex),
			_renderer->GetCulledCommandsOffset(0),
			_renderer->GetCulledCommandsCounters(DX::FrameIndex),
			_renderer->GetCulledCommandsCountersOffset(0));
	}
	else
	{
		for (const auto& prefab : Scene::CurrentScene->prefabs)
		{
			for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
			{
				const auto& currentMesh = Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];
				unsigned int commandData[] =
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

void HardwareRasterization::_drawOverdraw(ID3D12Resource* renderTarget)
{
	PIXScopedEvent(COMMAND_LIST.Get(), 0, L"Draw Quad Overshading");

	unsigned int clearValue[] = { 0, 0, 0, 0 };
	COMMAND_LIST->ClearUnorderedAccessViewUint(
		Descriptors::SV.GetGPUHandle(HWROverdrawUAV),
		Descriptors::NonSV.GetCPUHandle(HWROverdrawUAV),
		_overdrawBuffer.Get(),
		clearValue,
		0,
		nullptr);
	auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(_overdrawBuffer.Get());
	COMMAND_LIST->ResourceBarrier(1, &uavBarrier);

	COMMAND_LIST->SetGraphicsRootSignature(_overdrawRS.Get());
	COMMAND_LIST->SetPipelineState(_overdrawPSO.Get());
	COMMAND_LIST->RSSetViewports(1, &_viewport);
	COMMAND_LIST->RSSetScissorRects(1, &_scissorRect);
	COMMAND_LIST->IASetVertexBuffers(0, 1, &Scene::CurrentScene->positionsGPU.GetVBView());
	COMMAND_LIST->SetGraphicsRootConstantBufferView(
		0, _sceneCB->GetGPUVirtualAddress() + DX::FrameIndex * sizeof(SceneCB));
	COMMAND_LIST->SetGraphicsRootDescriptorTable(
		2,
		Settings::CullingEnabled
		? Descriptors::SV.GetGPUHandle(VisibleInstancesSRV + DX::FrameIndex * PerFrameDescriptorsCount)
		: Scene::CurrentScene->instancesGPU.GetSRV());
	COMMAND_LIST->SetGraphicsRootDescriptorTable(
		3,
		Descriptors::SV.GetGPUHandle(HWROverdrawUAV));
	COMMAND_LIST->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

	if (Settings::CullingEnabled)
	{
		COMMAND_LIST->ExecuteIndirect(
			_overdrawCommandSignature.Get(),
			static_cast<unsigned int>(Scene::CurrentScene->meshesMetaCPU.size()),
			_renderer->GetCulledCommands(DX::FrameIndex),
			_renderer->GetCulledCommandsOffset(0),
			_renderer->GetCulledCommandsCounters(DX::FrameIndex),
			_renderer->GetCulledCommandsCountersOffset(0));
	}
	else
	{
		for (const auto& prefab : Scene::CurrentScene->prefabs)
		{
			for (unsigned int mesh = 0; mesh < prefab.meshesCount; mesh++)
			{
				const auto& currentMesh = Scene::CurrentScene->meshesMetaCPU[prefab.meshesOffset + mesh];
				unsigned int commandData[] =
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

	CD3DX12_RESOURCE_BARRIER barriers[] =
	{
		CD3DX12_RESOURCE_BARRIER::Transition(
			_overdrawBuffer.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(
			renderTarget,
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_RENDER_TARGET)
	};
	COMMAND_LIST->ResourceBarrier(_countof(barriers), barriers);

	COMMAND_LIST->SetGraphicsRootSignature(_overdrawDisplayRS.Get());
	COMMAND_LIST->SetPipelineState(_overdrawDisplayPSO.Get());
	COMMAND_LIST->SetGraphicsRootDescriptorTable(
		0,
		Descriptors::SV.GetGPUHandle(HWROverdrawSRV));
	auto RTVHandle = Descriptors::RT.GetCPUHandle(ForwardRendererRTV + DX::FrameIndex);
	COMMAND_LIST->OMSetRenderTargets(1, &RTVHandle, FALSE, nullptr);
	float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	COMMAND_LIST->ClearRenderTargetView(RTVHandle, clearColor, 0, nullptr);
	COMMAND_LIST->DrawInstanced(3, 1, 0, 0);

	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		_overdrawBuffer.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	COMMAND_LIST->ResourceBarrier(1, &barrier);
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

	auto pointClampSampler = Utils::PointClampSampler(
		0,
		D3D12_SHADER_VISIBILITY_PIXEL);

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

void HardwareRasterization::_createOverdrawRS()
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
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	rootParameters[3].InitAsDescriptorTable(
		1,
		&ranges[1],
		D3D12_SHADER_VISIBILITY_PIXEL);

	D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init_1_1(
		_countof(rootParameters),
		rootParameters,
		0,
		nullptr,
		rootSignatureFlags);

	Utils::CreateRS(rootSignatureDesc, _overdrawRS);
	NAME_D3D12_OBJECT(_overdrawRS);
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

	ComPtr<ID3DBlob> vertexShader = Utils::CompileShader(
		L"shaders\\DrawDepthVS.hlsl",
		nullptr,
		"main",
		"vs_5_0");

	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	SUCCESS(DX::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_depthPSO)));
	NAME_D3D12_OBJECT(_depthPSO);
}

void HardwareRasterization::_createOpaquePassPSO()
{
	ComPtr<ID3DBlob> vertexShader = Utils::CompileShader(
		L"shaders\\DrawOpaqueVS.hlsl",
		nullptr,
		"main",
		"vs_5_0");

	const D3D_SHADER_MACRO defines[] = { { "OPAQUE", "1" }, { nullptr, nullptr } };
	ComPtr<ID3DBlob> pixelShader = Utils::CompileShader(
		L"shaders\\DrawOpaquePS.hlsl",
		defines,
		"main",
		"ps_5_0");

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
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
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

void HardwareRasterization::_createOverdrawPassPSO()
{
	ComPtr<ID3DBlob> vertexShader;
	Utils::CompileDXILFromFile(
		L"shaders\\DrawOverdrawVS.hlsl",
		L"main",
		L"vs_6_0",
		nullptr,
		0,
		vertexShader.GetAddressOf());
	ComPtr<ID3DBlob> pixelShader;
	Utils::CompileDXILFromFile(
		L"shaders\\DrawOverdrawPS.hlsl",
		L"main",
		L"ps_6_0",
		nullptr,
		0,
		pixelShader.GetAddressOf());

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
	psoDesc.pRootSignature = _overdrawRS.Get();
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	// The reference mode bypasses fine depth so every launched shading quad contributes.
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.SampleDesc.Count = 1;
	SUCCESS(DX::Device->CreateGraphicsPipelineState(
		&psoDesc,
		IID_PPV_ARGS(&_overdrawPSO)));
	NAME_D3D12_OBJECT(_overdrawPSO);
}

void HardwareRasterization::_createOverdrawDisplayRS()
{
	CD3DX12_ROOT_PARAMETER1 rootParameters[1] = {};
	CD3DX12_DESCRIPTOR_RANGE1 range = {};
	range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	rootParameters[0].InitAsDescriptorTable(
		1,
		&range,
		D3D12_SHADER_VISIBILITY_PIXEL);

	D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init_1_1(
		_countof(rootParameters),
		rootParameters,
		0,
		nullptr,
		rootSignatureFlags);

	Utils::CreateRS(rootSignatureDesc, _overdrawDisplayRS);
	NAME_D3D12_OBJECT(_overdrawDisplayRS);
}

void HardwareRasterization::_createOverdrawDisplayPSO()
{
	ComPtr<ID3DBlob> vertexShader = Utils::CompileShader(
		L"shaders\\DrawOverdrawDisplayVS.hlsl",
		nullptr,
		"main",
		"vs_5_0");
	ComPtr<ID3DBlob> pixelShader = Utils::CompileShader(
		L"shaders\\DrawOverdrawDisplayPS.hlsl",
		nullptr,
		"main",
		"ps_5_0");

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = _overdrawDisplayRS.Get();
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = Settings::BackBufferFormat;
	psoDesc.SampleDesc.Count = 1;
	SUCCESS(DX::Device->CreateGraphicsPipelineState(
		&psoDesc,
		IID_PPV_ARGS(&_overdrawDisplayPSO)));
	NAME_D3D12_OBJECT(_overdrawDisplayPSO);
}
