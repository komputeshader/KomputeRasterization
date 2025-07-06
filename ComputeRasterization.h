#pragma once

#include "Common.h"
#include "Utils.h"
#include "Shadows.h"

class Camera;
struct Scene;

class ComputeRasterization
{
public:

	ComputeRasterization() = default;
	ComputeRasterization(ComputeRasterization const &) = delete;
	ComputeRasterization & operator=(ComputeRasterization const &) = delete;
	~ComputeRasterization() = default;

	void Resize(
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
		UINT height);

	void BeginPass(ID3D12GraphicsCommandList * commandList, UINT frameIndex, Scene * scene, Shadows * shadows);
	void Draw(ID3D12GraphicsCommandList * commandList, UINT frameIndex, Scene * scene, Shadows * shadows);
	void EndPass(ID3D12GraphicsCommandList * commandList);

	ID3D12Resource * GetRenderTarget() const { return _renderTarget.Get(); }

private:

	// should match it's duplicate in Rasterization.hlsli
	static const UINT GroupThreadsX = 256;

	enum SRVUAVDescriptors
	{
		DepthVerticesSRV = 1,
		VerticesSRV = 1,
		IndicesSRV = 1,
		InstancesSRV = 1,
		DepthSRV = 1,
		DepthUAV = 1,
		RenderTargetUAV = 1,
		ShadowMapSRV = 1,
		BigTrianglesUAV = 1,
	};

	enum SRVUAVHeapOffsets
	{
		DepthVerticesSRVOffset = 0,
		VerticesSRVOffset = DepthVerticesSRVOffset + 1,
		IndicesSRVOffset = VerticesSRVOffset + 1,
		InstancesSRVOffset = IndicesSRVOffset + 1,
		DepthSRVOffset = InstancesSRVOffset + 1,
		ShadowMapSRVOffset = DepthSRVOffset + 1,
		SRVDescriptorCount = ShadowMapSRVOffset + 1,

		BigTrianglesUAVOffset = ShadowMapSRVOffset + 1,
		RenderTargetUAVOffset = BigTrianglesUAVOffset + 1,
		DepthUAVOffset = RenderTargetUAVOffset + 1,
		CascadeUAVOffset = DepthUAVOffset + 1,
		UAVDescriptorCount = CascadeUAVOffset - SRVDescriptorCount + Shadows::CascadeCount,

		SRVUAVDescriptorsCount = SRVDescriptorCount + UAVDescriptorCount
	};

	void _clearMDICounter(ID3D12GraphicsCommandList * commandList);

	Microsoft::WRL::ComPtr<ID3D12Resource> _renderTarget;
	Microsoft::WRL::ComPtr<ID3D12Resource> _depthBuffer;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _depthPSO;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _depthBigTrianglePSO;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _opaquePSO;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _opaqueBigTrianglePSO;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _depthRS;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _depthBigTriangleRS;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _opaqueRS;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _opaqueBigTriangleRS;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _CBVSRVUAVGPUHeap;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _CBVSRVUAVCPUHeap;
	UINT _CBVSRVUAVDescriptorSize = 0;
	// in order to use InterlockedMax
	DXGI_FORMAT _depthFormat = DXGI_FORMAT_R32_UINT;
	Microsoft::WRL::ComPtr<ID3D12Resource> _depthCB;
	UINT8 * _depthCBData;
	UINT _depthCBFrameSize = 0;
	Microsoft::WRL::ComPtr<ID3D12Resource> _opaqueCB;
	UINT8 * _opaqueCBData;

	UINT _width = 0;
	UINT _height = 0;
	UINT _frameCount = 0;

	// MDI stuff
	Microsoft::WRL::ComPtr<ID3D12CommandSignature> _depthCommandSignature;
	Microsoft::WRL::ComPtr<ID3D12CommandSignature> _opaqueCommandSignature;
	Microsoft::WRL::ComPtr<ID3D12Resource> _bigTrianglesCommands;
	Microsoft::WRL::ComPtr<ID3D12Resource> _bigTrianglesCommandsCounterReset;
	UINT _commandSizePerFrame = 0;
	UINT _commandBufferCounterOffset = 0;
	UINT _totalTrianglesCount = 0;

};