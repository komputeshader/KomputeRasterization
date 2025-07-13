#pragma once

#include "Settings.h"
#include "Utils.h"

namespace DX
{

const int FramesCount = 2;
extern int FrameNumber;
extern int FrameIndex;
extern int LastFrameIndex;
extern DXGI_ADAPTER_DESC1 AdapterDesc;

extern Microsoft::WRL::ComPtr<ID3D12Device> Device;
extern Microsoft::WRL::ComPtr<IDXGIFactory4> Factory;
extern Microsoft::WRL::ComPtr<IDXGIAdapter3> Adapter;
extern Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> CommandList[FramesCount];
extern Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> ComputeCommandList[FramesCount];
extern Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CommandAllocators[FramesCount];
extern Microsoft::WRL::ComPtr<ID3D12CommandAllocator> ComputeCommandAllocators[FramesCount];
extern Microsoft::WRL::ComPtr<ID3D12CommandQueue> CommandQueue;
extern Microsoft::WRL::ComPtr<ID3D12CommandQueue> ComputeCommandQueue;

// synchronization objects
extern HANDLE FrameFenceEvents[DX::FramesCount];
extern Microsoft::WRL::ComPtr<ID3D12Fence> FrameFences[DX::FramesCount];
extern size_t FrameFenceValues[DX::FramesCount];
extern size_t FrameFenceValue;

extern HANDLE ComputeFenceEvents[DX::FramesCount];
extern Microsoft::WRL::ComPtr<ID3D12Fence> ComputeFences[DX::FramesCount];
extern size_t ComputeFenceValues[DX::FramesCount];
extern size_t ComputeFenceValue;

extern HANDLE UploadFenceEvent;
extern Microsoft::WRL::ComPtr<ID3D12Fence> UploadFence;
extern size_t UploadFenceValue;

void WaitForFence(ID3D12Fence* fence, size_t fenceValue, HANDLE fenceEvent);

extern D3D12_FEATURE_DATA_ROOT_SIGNATURE RSFeatureData;

void GetHardwareAdapter(
	IDXGIFactory1* pFactory,
	IDXGIAdapter1** ppAdapter,
	bool requestHighPerformanceAdapter = true);

void CreateDevice();
void CreateCommandAllocators();
void CreateCommandQueues();
void CreateCommandLists();
void CloseCommandLists();
void CreateSyncObjects();

}

#define COMMAND_LIST (DX::CommandList[DX::FrameIndex])
#define COMPUTE_COMMAND_LIST (DX::ComputeCommandList[DX::FrameIndex])

