#pragma once

#include "Timer.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shadows.h"

class ForwardRenderer;

class HardwareRasterization
{
public:

	HardwareRasterization() = default;
	HardwareRasterization(const HardwareRasterization&) = delete;
	HardwareRasterization& operator=(const HardwareRasterization&) = delete;
	~HardwareRasterization() = default;

	void Resize(
		ForwardRenderer* renderer,
		int width,
		int height);
	void DrawDepths();
	void DrawOpaque(ID3D12Resource* renderTarget);
	void Update();

private:

	void _loadAssets();
	void _createDepthBufferResources();
	void _createOverdrawResources();

	void _createHWRRS();
	void _createOverdrawRS();
	void _createDepthPassPSO();
	void _createOpaquePassPSO();
	void _createOverdrawPassPSO();
	void _createOverdrawDisplayRS();
	void _createOverdrawDisplayPSO();
	void _createMDIStuff();

	void _beginFrame();
	void _drawDepth();
	void _drawShadows();
	void _drawOpaque(ID3D12Resource* renderTarget);
	void _drawOverdraw(ID3D12Resource* renderTarget);
	void _endFrame();

	CD3DX12_VIEWPORT _viewport;
	CD3DX12_RECT _scissorRect;

	Microsoft::WRL::ComPtr<ID3D12Resource> _depthBuffer;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _HWRRS;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _overdrawRS;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _overdrawDisplayRS;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _opaquePSO;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _depthPSO;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _overdrawPSO;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _overdrawDisplayPSO;
	Microsoft::WRL::ComPtr<ID3D12Resource> _overdrawBuffer;
	DXGI_FORMAT _depthFormat = DXGI_FORMAT_D32_FLOAT;

	Microsoft::WRL::ComPtr<ID3D12Resource> _depthSceneCB;
	unsigned char* _depthSceneCBData;
	unsigned int _depthSceneCBFrameSize = 0;
	Microsoft::WRL::ComPtr<ID3D12Resource> _sceneCB;
	unsigned char* _sceneCBData;

	// MDI stuff
	Microsoft::WRL::ComPtr<ID3D12CommandSignature> _commandSignature;
	Microsoft::WRL::ComPtr<ID3D12CommandSignature> _overdrawCommandSignature;

	int _width;
	int _height;
	ForwardRenderer* _renderer;
};
