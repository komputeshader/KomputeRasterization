#include "DX.h"

#ifdef USE_WORK_GRAPHS
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 616; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = u8".\\D3D12\\"; }
#endif

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace DX
{

ComPtr<ID3D12Device> Device;
ComPtr<IDXGIFactory4> Factory;
ComPtr<IDXGIAdapter3> Adapter;
ComPtr<ID3D12GraphicsCommandList> CommandList[FramesCount];
ComPtr<ID3D12GraphicsCommandList> ComputeCommandList[FramesCount];
ComPtr<ID3D12CommandAllocator> CommandAllocators[FramesCount];
ComPtr<ID3D12CommandAllocator> ComputeCommandAllocators[FramesCount];
ComPtr<ID3D12CommandQueue> CommandQueue;
ComPtr<ID3D12CommandQueue> ComputeCommandQueue;

HANDLE FrameFenceEvents[DX::FramesCount];
Microsoft::WRL::ComPtr<ID3D12Fence> FrameFences[DX::FramesCount];
size_t FrameFenceValues[DX::FramesCount];
size_t FrameFenceValue;

HANDLE ComputeFenceEvents[DX::FramesCount];
Microsoft::WRL::ComPtr<ID3D12Fence> ComputeFences[DX::FramesCount];
size_t ComputeFenceValues[DX::FramesCount];
size_t ComputeFenceValue;

HANDLE UploadFenceEvent;
Microsoft::WRL::ComPtr<ID3D12Fence> UploadFence;
size_t UploadFenceValue;

DXGI_ADAPTER_DESC1 AdapterDesc;
D3D12_FEATURE_DATA_ROOT_SIGNATURE RSFeatureData;

int FrameNumber;
int FrameIndex;
int LastFrameIndex;

void GetHardwareAdapter(
	IDXGIFactory1* pFactory,
	IDXGIAdapter1** ppAdapter,
	bool requestHighPerformanceAdapter)
{
	*ppAdapter = nullptr;

	ComPtr<IDXGIAdapter1> adapter;

	ComPtr<IDXGIFactory6> factory6;
	if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&factory6))))
	{
		for (
			unsigned int adapterIndex = 0;
			SUCCEEDED(factory6->EnumAdapterByGpuPreference(
				adapterIndex,
				requestHighPerformanceAdapter == true
				? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
				: DXGI_GPU_PREFERENCE_UNSPECIFIED,
				IID_PPV_ARGS(&adapter)));
			++adapterIndex)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			{
				// Don't select the Basic Render Driver adapter.
				// If you want a software adapter, pass in "/warp" on the command line.
				continue;
			}

			// Check to see whether the adapter supports Direct3D 12, but don't create the
			// actual device yet.
			if (SUCCEEDED(D3D12CreateDevice(
				adapter.Get(),
				D3D_FEATURE_LEVEL_11_0,
				_uuidof(ID3D12Device),
				nullptr)))
			{
				break;
			}
		}
	}

	if (adapter.Get() == nullptr)
	{
		for (
			unsigned int adapterIndex = 0;
			SUCCEEDED(pFactory->EnumAdapters1(adapterIndex, &adapter));
			++adapterIndex)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			{
				// Don't select the Basic Render Driver adapter.
				// If you want a software adapter, pass in "/warp" on the command line.
				continue;
			}

			// Check to see whether the adapter supports Direct3D 12, but don't create the
			// actual device yet.
			if (SUCCEEDED(D3D12CreateDevice(
				adapter.Get(),
				D3D_FEATURE_LEVEL_11_0,
				_uuidof(ID3D12Device),
				nullptr)))
			{
				break;
			}
		}
	}

	*ppAdapter = adapter.Detach();
}

void CreateDevice()
{
	FrameNumber = 0;
	FrameIndex = 0;
	LastFrameIndex = 0;

	unsigned int dxgiFactoryFlags = 0;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	//ComPtr<IDXGIFactory4> factory;
	SUCCESS(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&Factory)));

	if (Settings::UseWarpDevice)
	{
		ComPtr<IDXGIAdapter> warpAdapter;
		SUCCESS(Factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

		SUCCESS(D3D12CreateDevice(
			warpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&Device)));

		SUCCESS(warpAdapter.As(&Adapter));
	}
	else
	{
		ComPtr<IDXGIAdapter1> hardwareAdapter;
		GetHardwareAdapter(Factory.Get(), &hardwareAdapter);

		SUCCESS(D3D12CreateDevice(
			hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&Device)));

		SUCCESS(hardwareAdapter.As(&Adapter));
	}
	NAME_D3D12_OBJECT(Device);

	Adapter->GetDesc1(&AdapterDesc);

	RSFeatureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

	if (FAILED(Device->CheckFeatureSupport(
		D3D12_FEATURE_ROOT_SIGNATURE,
		&RSFeatureData,
		sizeof(RSFeatureData))))
	{
		RSFeatureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

#ifdef USE_WORK_GRAPHS
	D3D12_FEATURE_DATA_D3D12_OPTIONS21 Options;
	SUCCESS(Device->CheckFeatureSupport(
		D3D12_FEATURE_D3D12_OPTIONS21,
		&Options,
		sizeof(Options)));
	ASSERT(Options.WorkGraphsTier != D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED, "Device does not report support for work graphs.")
#endif
}

void CreateCommandAllocators()
{
	for (int frame = 0; frame < FramesCount; frame++)
	{
		SUCCESS(Device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&CommandAllocators[frame])));
		SUCCESS(Device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_COMPUTE,
			IID_PPV_ARGS(&ComputeCommandAllocators[frame])));
	}
}

void CreateCommandLists()
{
	for (int frame = 0; frame < FramesCount; frame++)
	{
		SUCCESS(Device->CreateCommandList(
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			CommandAllocators[frame].Get(),
			nullptr,
			IID_PPV_ARGS(&CommandList[frame])));
		NAME_D3D12_OBJECT_INDEXED(CommandList, frame);

		SUCCESS(Device->CreateCommandList(
			0,
			D3D12_COMMAND_LIST_TYPE_COMPUTE,
			ComputeCommandAllocators[frame].Get(),
			nullptr,
			IID_PPV_ARGS(&ComputeCommandList[frame])));
		NAME_D3D12_OBJECT_INDEXED(ComputeCommandList, frame);
	}
}

void CloseCommandLists()
{
	for (int frame = 0; frame < FramesCount; frame++)
	{
		SUCCESS(CommandList[frame]->Close());
		SUCCESS(ComputeCommandList[frame]->Close());
	}
}

void CreateCommandQueues()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	SUCCESS(Device->CreateCommandQueue(
		&queueDesc,
		IID_PPV_ARGS(&CommandQueue)));
	NAME_D3D12_OBJECT(CommandQueue);

	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;

	SUCCESS(Device->CreateCommandQueue(
		&queueDesc,
		IID_PPV_ARGS(&ComputeCommandQueue)));
	NAME_D3D12_OBJECT(ComputeCommandQueue);
}

void CreateSyncObjects()
{
	FrameFenceValue = 0;
	for (int frame = 0; frame < DX::FramesCount; frame++)
	{
		FrameFenceValues[frame] = FrameFenceValue;
		SUCCESS(Device->CreateFence(
			FrameFenceValues[frame],
			D3D12_FENCE_FLAG_NONE,
			IID_PPV_ARGS(&FrameFences[frame])));
		NAME_D3D12_OBJECT_INDEXED(FrameFences, frame);
		FrameFenceEvents[frame] = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		ComputeFenceValues[frame] = 0;
		SUCCESS(Device->CreateFence(
			ComputeFenceValues[frame],
			D3D12_FENCE_FLAG_NONE,
			IID_PPV_ARGS(&ComputeFences[frame])));
		NAME_D3D12_OBJECT_INDEXED(ComputeFences, frame);
		ComputeFenceEvents[frame] = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	}

	UploadFenceValue = 0;
	SUCCESS(Device->CreateFence(
		0,
		D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(&UploadFence)));
	NAME_D3D12_OBJECT(UploadFence);
	UploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	ComputeFenceValue = 1;
}

void WaitForFence(ID3D12Fence* fence, size_t fenceValue, HANDLE fenceEvent)
{
	if (fence->GetCompletedValue() < fenceValue)
	{
		SUCCESS(fence->SetEventOnCompletion(fenceValue, fenceEvent));
		WaitForSingleObject(fenceEvent, INFINITE);
	}
}

}