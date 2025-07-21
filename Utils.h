#pragma once

#include "Settings.h"

inline void PrintToOutput(void)
{

}

inline void PrintToOutput(const char* format, ...)
{
	char buffer[512];
	va_list arg;
	va_start(arg, format);
	vsprintf_s(buffer, sizeof(buffer), format, arg);
	va_end(arg);
	OutputDebugStringA(buffer);
}

inline void PrintToOutput(const wchar_t* format, ...)
{
	wchar_t buffer[512];
	va_list arg;
	va_start(arg, format);
	vswprintf(buffer, sizeof(buffer), format, arg);
	va_end(arg);
	OutputDebugString(buffer);
}

// Assign a name to the object to aid with debugging.
#if defined(_DEBUG) || defined(DBG)
inline void SetName(ID3D12Object* pObject, LPCWSTR name)
{
	pObject->SetName(name);
}

inline void SetNameIndexed(ID3D12Object* pObject, LPCWSTR name, unsigned int index)
{
	wchar_t fullName[50];
	if (swprintf_s(fullName, L"%s[%u]", name, index) > 0)
	{
		pObject->SetName(fullName);
	}
}
#else
inline void SetName(ID3D12Object*, LPCWSTR)
{
}
inline void SetNameIndexed(ID3D12Object*, LPCWSTR, unsigned int)
{
}
#endif

// Naming helper for ComPtr<T>.
// Assigns the name of the variable as the name of the object.
// The indexed variant will include the index in the name of the object.
#define NAME_D3D12_OBJECT(x) SetName((x).Get(), L#x)
#define NAME_D3D12_OBJECT_INDEXED(x, n) SetNameIndexed((x)[n].Get(), L#x, n)

#define ASSERT(isFalse, ...) \
	if (!(bool)(isFalse)) { \
		PrintToOutput("\nAssertion " #isFalse " failed in file %s, line %d\n", __FILE__, __LINE__); \
		PrintToOutput(__VA_ARGS__); \
		PrintToOutput("\n"); \
		__debugbreak(); \
	}

#define SUCCESS(hr, ...) \
	if (FAILED(hr)) { \
		PrintToOutput("\nHRESULT hr = 0x%08X failed in file %s, line %d\n", static_cast<unsigned int>(hr), __FILE__, __LINE__); \
		PrintToOutput(__VA_ARGS__); \
		PrintToOutput("\n"); \
		__debugbreak(); \
	}

namespace Utils
{

extern Microsoft::WRL::ComPtr<ID3D12RootSignature> HiZRS;
extern Microsoft::WRL::ComPtr<ID3D12PipelineState> HiZPSO;
extern D3D12_STATIC_SAMPLER_DESC HiZSamplerDesc;

inline void GetAssetsPath(_Out_writes_(pathSize) wchar_t* path, unsigned int pathSize)
{
	ASSERT(path)

	DWORD size = GetModuleFileName(nullptr, path, pathSize);
	ASSERT(size)
	ASSERT(size != pathSize)

	wchar_t* lastSlash = wcsrchr(path, L'\\');
	if (lastSlash)
	{
		*(lastSlash + 1) = L'\0';
	}
}

void InitializeResources();

inline unsigned int DispatchSize(unsigned int groupSize, unsigned int elementsCount)
{
	assert(groupSize != 0 && "DispatchSize : groupSize cannot be 0");

	return (elementsCount + groupSize - 1) / groupSize;
}

AABB MergeAABBs(const AABB& a, const AABB& b);

AABB TransformAABB(
	const AABB& a,
	DirectX::FXMMATRIX m,
	bool ignoreCenter = false);

void GetFrustumPlanes(DirectX::FXMMATRIX m, Frustum& f);

Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
	const std::wstring& filename,
	const D3D_SHADER_MACRO* defines,
	const std::string& entrypoint,
	const std::string& target);

#ifdef USE_WORK_GRAPHS
void CompileDXILLibraryFromFile(
	const std::wstring& filename,
	const std::wstring& target,
	DxcDefine* defines,
	unsigned int definesCount,
	ID3DBlob** ppCode);
#endif

void CreateDefaultHeapBuffer(
	ID3D12GraphicsCommandList* commandList,
	const void* data,
	size_t bufferSize,
	Microsoft::WRL::ComPtr<ID3D12Resource>& defaultBuffer,
	Microsoft::WRL::ComPtr<ID3D12Resource>& uploadBuffer,
	D3D12_RESOURCE_STATES endState,
	bool unorderedAccess = false);

void CreateCBResources(
	// CB size is required to be 256-byte aligned.
	size_t bufferSize,
	void** data,
	Microsoft::WRL::ComPtr<ID3D12Resource>& uploadBuffer);

void CreateRS(
	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc,
	Microsoft::WRL::ComPtr<ID3D12RootSignature>& rootSignature);

unsigned int MipsCount(unsigned int width, unsigned int height);

void GenerateHiZ(
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* resource,
	unsigned int startSRV,
	unsigned int startUAV,
	unsigned int inputWidth,
	unsigned int inputHeight,
	unsigned int arraySlice = 0,
	unsigned int arraySize = 1);

inline std::wstring GetAssetFullPath(LPCWSTR assetName)
{
	return Settings::Demo.AssetsPath + assetName;
}

inline unsigned int AlignForUavCounter(unsigned int bufferSize)
{
	unsigned int alignment = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT;
	return (bufferSize + (alignment - 1)) & ~(alignment - 1);
}

inline unsigned int AsUINT(float f)
{
	return *reinterpret_cast<unsigned int*>(&f);
}

class GPUBuffer
{
public:

	void Initialize(
		ID3D12GraphicsCommandList* commandList,
		const void* data,
		size_t elementsCount,
		unsigned int strideInBytes,
		D3D12_RESOURCE_STATES endState,
		unsigned int SRVIndex,
		LPCWSTR name);

	GPUBuffer() = default;
	GPUBuffer(const GPUBuffer&) = delete;
	GPUBuffer& operator=(const GPUBuffer&) = delete;

	ID3D12Resource* Get()
	{
		return _buffer.Get();
	}

	D3D12_VERTEX_BUFFER_VIEW& GetVBView()
	{
		assert(_isVB);
		return _VBView;
	}

	D3D12_INDEX_BUFFER_VIEW& GetIBView()
	{
		assert(_isIB);
		return _IBView;
	}

	CD3DX12_GPU_DESCRIPTOR_HANDLE GetSRV()
	{
		return _SRV;
	}

private:

	Microsoft::WRL::ComPtr<ID3D12Resource> _buffer;
	Microsoft::WRL::ComPtr<ID3D12Resource> _bufferUpload;
	D3D12_VERTEX_BUFFER_VIEW _VBView;
	D3D12_INDEX_BUFFER_VIEW _IBView;
	CD3DX12_GPU_DESCRIPTOR_HANDLE _SRV;
	bool _isVB = false;
	bool _isIB = false;
};

};