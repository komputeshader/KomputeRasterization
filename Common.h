#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers.
#endif

#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#include "d3dx12.h"
#include <pix3.h>

#include <string>
#include <memory>
#include <wrl.h>
#include <shellapi.h>

#include "CPUGPUCommon.h"

static const float SkyColor[] =
{
	136.0f / 255.0f,
	198.0f / 255.0f,
	252.0f / 255.0f,
	1.0f
};

static const DirectX::XMFLOAT4X4 Identity4x4 =
{
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f
};

// https://developer.nvidia.com/content/understanding-structured-buffer-performance

struct VertexPosition
{
	DirectX::XMFLOAT3 position;
};

struct VertexNormal
{
	// | 2 bits - unused | 10 bits - x | 10 bits - y | 10 bits - z |
	unsigned int packedNormal;
};

struct VertexColor
{
	// [0] : |16 bits - r component | 16 bits - g component |
	// [1] : |16 bits - b component | 16 bits - a component |
	unsigned int packedColor[2];
};

struct VertexUV
{
	// | 16 bits - u | 16 bits - v |
	unsigned int packedUV;
};

class AABB
{
public:

	DirectX::XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
	float pad0;
	DirectX::XMFLOAT3 extents = { 0.0f, 0.0f, 0.0f };
	float pad1;

	float GetDiagonalLength() const
	{
		return 2.0f * sqrtf(
			extents.x * extents.x +
			extents.y * extents.y +
			extents.z * extents.z);
	}
};

struct MeshMeta
{
	AABB AABB;

	unsigned int indexCountPerInstance;
	unsigned int instanceCount;
	unsigned int startIndexLocation;
	int baseVertexLocation;
	unsigned int startInstanceLocation;

	DirectX::XMFLOAT3 coneApex;
	DirectX::XMFLOAT3 coneAxis;
	float coneCutoff;
};

struct Frustum
{
	DirectX::XMFLOAT4 l;
	DirectX::XMFLOAT4 r;
	DirectX::XMFLOAT4 b;
	DirectX::XMFLOAT4 t;
	DirectX::XMFLOAT4 n;
	DirectX::XMFLOAT4 f;

	DirectX::XMFLOAT4 cornersWS[8];
};

struct Instance
{
	DirectX::XMFLOAT4X4 worldTransform;
	unsigned int meshID;
	DirectX::XMFLOAT3 color;
};

struct Prefab
{
	unsigned int meshesOffset = 0;
	unsigned int meshesCount = 0;
	AABB AABB;
};

struct IndirectCommand
{
	unsigned int startInstanceLocation;
	D3D12_DRAW_INDEXED_ARGUMENTS arguments;
};

struct DepthSceneCB
{
	DirectX::XMFLOAT4X4 VP;
	float pad[48];
};
static_assert(
	(sizeof(DepthSceneCB) % 256) == 0,
	"Constant Buffer size must be 256-byte aligned");

enum ScenesIndices
{
	Buddha,
	Plant,
	ScenesCount
};