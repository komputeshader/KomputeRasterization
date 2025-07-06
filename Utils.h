#pragma once

#include "Types.h"
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

inline void SetNameIndexed(ID3D12Object* pObject, LPCWSTR name, UINT index)
{
	WCHAR fullName[50];
	if (swprintf_s(fullName, L"%s[%u]", name, index) > 0)
	{
		pObject->SetName(fullName);
	}
}
#else
inline void SetName(ID3D12Object*, LPCWSTR)
{
}
inline void SetNameIndexed(ID3D12Object*, LPCWSTR, UINT)
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
		__debugbreak(); \
	}

#define SUCCESS(hr, ...) \
	if (FAILED(hr)) { \
		PrintToOutput("\nHRESULT hr = 0x%08X failed in file %s, line %d\n", static_cast<UINT>(hr), __FILE__, __LINE__); \
		PrintToOutput(__VA_ARGS__); \
		__debugbreak(); \
	}

namespace Utils
{

extern Microsoft::WRL::ComPtr<ID3D12RootSignature> HiZRS;
extern Microsoft::WRL::ComPtr<ID3D12PipelineState> HiZPSO;
extern D3D12_STATIC_SAMPLER_DESC HiZSamplerDesc;

inline void GetAssetsPath(_Out_writes_(pathSize) WCHAR* path, UINT pathSize)
{
	ASSERT(path);

	DWORD size = GetModuleFileName(nullptr, path, pathSize);
	ASSERT(size);
	ASSERT(size != pathSize);

	WCHAR* lastSlash = wcsrchr(path, L'\\');
	if (lastSlash)
	{
		*(lastSlash + 1) = L'\0';
	}
}

class ShaderHelper
{
public:

	ShaderHelper(LPCWSTR filename)
	{
		using namespace Microsoft::WRL;

#if WINVER >= _WIN32_WINNT_WIN8
		CREATEFILE2_EXTENDED_PARAMETERS extendedParams = {};
		extendedParams.dwSize = sizeof(CREATEFILE2_EXTENDED_PARAMETERS);
		extendedParams.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
		extendedParams.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
		extendedParams.dwSecurityQosFlags = SECURITY_ANONYMOUS;
		extendedParams.lpSecurityAttributes = nullptr;
		extendedParams.hTemplateFile = nullptr;

		Wrappers::FileHandle file(CreateFile2(filename, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &extendedParams));
#else
		Wrappers::FileHandle file(CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
#endif
		ASSERT(file.Get() != INVALID_HANDLE_VALUE);

		FILE_STANDARD_INFO fileInfo = {};
		ASSERT(GetFileInformationByHandleEx(file.Get(), FileStandardInfo, &fileInfo, sizeof(fileInfo)));
		ASSERT(fileInfo.EndOfFile.HighPart == 0);

		_data = reinterpret_cast<byte*>(malloc(fileInfo.EndOfFile.LowPart));
		_size = fileInfo.EndOfFile.LowPart;

		ASSERT(ReadFile(file.Get(), _data, fileInfo.EndOfFile.LowPart, nullptr, nullptr));
	}

	~ShaderHelper()
	{
		free(_data);
	}

	byte* GetData() { return _data; }
	UINT GetSize() { return _size; }

private:

	byte* _data;
	UINT _size;
};

void InitializeResources();

inline UINT DispatchSize(UINT groupSize, UINT elementsCount)
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

void CreateDefaultHeapBuffer(
	ID3D12GraphicsCommandList* commandList,
	const void* data,
	UINT64 bufferSize,
	Microsoft::WRL::ComPtr<ID3D12Resource>& defaultBuffer,
	Microsoft::WRL::ComPtr<ID3D12Resource>& uploadBuffer,
	D3D12_RESOURCE_STATES endState,
	bool unorderedAccess = false);

void CreateCBResources(
	// CB size is required to be 256-byte aligned.
	UINT64 bufferSize,
	void** data,
	Microsoft::WRL::ComPtr<ID3D12Resource>& uploadBuffer);

void CreateRS(
	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc,
	Microsoft::WRL::ComPtr<ID3D12RootSignature>& rootSignature);

UINT MipsCount(UINT width, UINT height);

void GenerateHiZ(
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* resource,
	UINT startSRV,
	UINT startUAV,
	UINT inputWidth,
	UINT inputHeight,
	UINT arraySlice = 0,
	UINT arraySize = 1);

inline std::wstring GetAssetFullPath(LPCWSTR assetName)
{
	return Settings::Demo.AssetsPath + assetName;
}

inline UINT AlignForUavCounter(UINT bufferSize)
{
	UINT alignment = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT;
	return (bufferSize + (alignment - 1)) & ~(alignment - 1);
}

inline UINT AsUINT(float f)
{
	return *reinterpret_cast<UINT*>(&f);
}

class GPUBuffer
{
public:

	void Initialize(
		ID3D12GraphicsCommandList* commandList,
		const void* data,
		UINT elementsCount,
		UINT strideInBytes,
		D3D12_RESOURCE_STATES endState,
		UINT SRVIndex,
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