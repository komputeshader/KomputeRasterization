#pragma once

#include "DXSample.h"
#include "Timer.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shadows.h"
#include "Culler.h"
#include "HardwareRasterization.h"
#include "SoftwareRasterization.h"
#include "Scene.h"

class ForwardRenderer : public DXSample
{
public:

	ForwardRenderer(unsigned int width, unsigned int height, std::wstring name);

	virtual void Initialize();
	virtual void Update();
	virtual void Draw();
	virtual void Resize(unsigned int width, unsigned int height, bool minimized);
	virtual void Destroy();

	void KeyboardInput();
	virtual void MouseMove(unsigned int x, unsigned int y);
	virtual void RMBPressed(unsigned int x, unsigned int y);
	virtual void KeyPressed(unsigned char key);

	void PreparePrevFrameDepth(ID3D12Resource* depth);
	ID3D12Resource* GetCulledCommands(int frame, int frustum)
	{
		assert(frame >= 0);
		assert(frame < DX::FramesCount);
		assert(frustum >= 0);
		assert(frustum < Settings::FrustumsCount);
		return _culledCommands[frame][frustum].Get();
	}
	ID3D12Resource* GetCulledCommandsCounter(int frame, int frustum)
	{
		assert(frame >= 0);
		assert(frame < DX::FramesCount);
		assert(frustum >= 0);
		assert(frustum < Settings::FrustumsCount);
		return _culledCommandsCounters[frame][frustum].Get();
	}

private:

	void _createDescriptorHeaps();
	void _createFrameResources();
	void _createSwapChain();
	void _createVisibleInstancesBuffer();
	void _createDepthBufferResources();
	void _createCulledCommandsBuffers();

	void _initGUI();
	void _newFrameGUI();
	void _drawGUI();
	void _destroyGUI();

	void _beginFrameRendering();
	void _finishFrameRendering();
	void _rasterizerSwitch();
	void _softwareRasterization();

	Microsoft::WRL::ComPtr<IDXGISwapChain3> _swapChain;
	Microsoft::WRL::ComPtr<ID3D12Resource> _renderTargets[DX::FramesCount];
	Microsoft::WRL::ComPtr<ID3D12Resource> _visibleInstances[DX::FramesCount];
	Microsoft::WRL::ComPtr<ID3D12Resource> _prevFrameDepthBuffer;
	// per frame granularity for async compute and graphics work
	Microsoft::WRL::ComPtr<ID3D12Resource> _culledCommands[DX::FramesCount][MAX_FRUSTUMS_COUNT];
	// first 4 bytes used as a counter
	// all 12 bytes are used as a dispatch indirect command
	// [0] - counter / group count X
	// [1] - group count Y
	// [2] - group count Z
	Microsoft::WRL::ComPtr<ID3D12Resource> _culledCommandsCounters[DX::FramesCount][MAX_FRUSTUMS_COUNT];
	Microsoft::WRL::ComPtr<ID3D12Resource> _culledCommandsCountersUpload[DX::FramesCount][MAX_FRUSTUMS_COUNT];

	std::unique_ptr<Culler> _culler;
	std::unique_ptr<HardwareRasterization> _HWR;
	std::unique_ptr<SoftwareRasterization> _SWR;
	std::unique_ptr<FrameStatistics> _stats;
	std::unique_ptr<Profiler> _profiler;
	DXGI_QUERY_VIDEO_MEMORY_INFO _GPUMemoryInfo;
	DXGI_QUERY_VIDEO_MEMORY_INFO _CPUMemoryInfo;
	Timer _timer;
	POINT _lastMousePos;

	bool _switchToSWR = false;
	bool _switchFromSWR = false;
};