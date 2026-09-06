#pragma once

#include "Settings.h"
#include "CPUGPUCommon.h"

class Culler
{
public:

	Culler();
	void Update();
	void Cull(
		ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* visibleInstances,
		const Microsoft::WRL::ComPtr<ID3D12Resource> (&culledCommands)[MAX_FRUSTUMS_COUNT],
		const Microsoft::WRL::ComPtr<ID3D12Resource> (&culledCommandsCounters)[MAX_FRUSTUMS_COUNT]);

private:

	void _createClearPSO();
	void _createCullingPSO();
	void _createGenerateCommandsPSO();
	void _createCullingCounters();

	Microsoft::WRL::ComPtr<ID3D12RootSignature> _clearRS;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _clearPSO;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _cullingRS;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _cullingPSO;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _generateHWRCommandsRS;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _generateHWRCommandsPSO;
	Microsoft::WRL::ComPtr<ID3D12Resource> _cullingCounters;
	Microsoft::WRL::ComPtr<ID3D12Resource> _culledCommandsCounterReset;

	Microsoft::WRL::ComPtr<ID3D12Resource> _cullingCB;
	unsigned char* _cullingCBData;
};
