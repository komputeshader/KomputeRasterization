#pragma once

#include "DX.h"

// these serve as indices into the descriptor heap
// NOTE: root signatures are dependent on that ordering
enum CBVSRVUAVIndices
{
	MeshesMetaSRV,
	InstancesSRV = MeshesMetaSRV + ScenesCount,
	CullingCountersSRV = InstancesSRV + ScenesCount,
	CullingCountersUAV,
	GUIFontTextureSRV,
	HWRShadowMapSRV,
	VertexPositionsSRV,
	VertexNormalsSRV = VertexPositionsSRV + ScenesCount,
	VertexColorsSRV = VertexNormalsSRV + ScenesCount,
	VertexTexcoordsSRV = VertexColorsSRV + ScenesCount,
	IndicesSRV = VertexTexcoordsSRV + ScenesCount,
	SWRDepthSRV = IndicesSRV + ScenesCount,
	SWRDepthUAV,
	PrevFrameDepthSRV,
	PrevFrameDepthMipsSRV,
	PrevFrameDepthMipsUAV = PrevFrameDepthMipsSRV + Settings::BackBufferMipsCount,
	SWRRenderTargetUAV = PrevFrameDepthMipsUAV + Settings::BackBufferMipsCount,
	SWRShadowMapSRV,
	SWRShadowMapUAV,
	PrevFrameShadowMapSRV = SWRShadowMapUAV + MAX_CASCADES_COUNT,
	PrevFrameShadowMapMipsSRV = PrevFrameShadowMapSRV + MAX_CASCADES_COUNT,
	PrevFrameShadowMapMipsUAV = PrevFrameShadowMapMipsSRV + MAX_CASCADES_COUNT * Settings::ShadowMapMipsCount,
	BigTrianglesSRV = PrevFrameShadowMapMipsUAV + MAX_CASCADES_COUNT * Settings::ShadowMapMipsCount,
	BigTrianglesUAV = BigTrianglesSRV + MAX_FRUSTUMS_COUNT,
	SWRStatsUAV = BigTrianglesUAV + MAX_FRUSTUMS_COUNT,

	SingleDescriptorsCount,

	// descriptors for frame resources
	VisibleInstancesSRV = SingleDescriptorsCount,
	VisibleInstancesUAV = VisibleInstancesSRV + MAX_FRUSTUMS_COUNT,
	CulledCommandsUAV,
	CulledCommandsSRV = CulledCommandsUAV + MAX_FRUSTUMS_COUNT,

	PerFrameDescriptorsCount = CulledCommandsSRV + MAX_FRUSTUMS_COUNT - VisibleInstancesSRV,
	CBVUAVSRVCount = SingleDescriptorsCount + PerFrameDescriptorsCount * DX::FramesCount
};

enum RTVIndices
{
	ForwardRendererRTV,
	RTVCount = ForwardRendererRTV + DX::FramesCount
};

enum DSVIndices
{
	HWRDepthDSV,
	CascadeDSV,
	DSVCount = CascadeDSV + MAX_CASCADES_COUNT
};

class Descriptors
{
public:

	static Descriptors DS;
	static Descriptors RT;
	// SV stands for Shader Visible
	static Descriptors SV;
	static Descriptors NonSV;

	void Initialize(
		D3D12_DESCRIPTOR_HEAP_TYPE type,
		D3D12_DESCRIPTOR_HEAP_FLAGS flags,
		unsigned int capacity);

	Descriptors() = default;
	Descriptors(const Descriptors&) = delete;
	Descriptors& operator=(const Descriptors&) = delete;

	CD3DX12_CPU_DESCRIPTOR_HANDLE GetCPUHandle(unsigned int index);
	CD3DX12_GPU_DESCRIPTOR_HANDLE GetGPUHandle(unsigned int index);

	ID3D12DescriptorHeap* GetHeap() { return _heap.Get(); }

private:

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;
	unsigned int _descriptorSize = 0;
	unsigned int _capacity = 0;
};