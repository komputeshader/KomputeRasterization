#pragma once

#include "DX.h"
#include "Utils.h"
#include "Shadows.h"

class ForwardRenderer;

class SoftwareRasterization
{
public:

	SoftwareRasterization() = default;
	SoftwareRasterization(const SoftwareRasterization&) = delete;
	SoftwareRasterization& operator=(const SoftwareRasterization&) = delete;
	~SoftwareRasterization() = default;

	void Resize(
		ForwardRenderer* renderer,
		int width,
		int height);
	void GUINewFrame();
	void Update();
	void Draw();

	ID3D12Resource* GetRenderTarget() const { return _renderTarget.Get(); }

	int GetPipelineTrianglesCount() const
	{
		return _statsResult[PipelineTriangles];
	}

	int GetRenderedTrianglesCount() const
	{
		return _statsResult[RenderedTriangles];
	}

private:

	void _createRenderTargetResources();
	void _createDepthBufferResources();
	void _createMDIResources();
	void _createResetBuffer();
	void _createStatsResources();
#ifdef USE_WORK_GRAPHS
	void _createDepthWGResources();
	void _createOpaqueWGResources();
#endif

	void _clearBigTrianglesDepthCounter(int frustum);
	void _clearBigTrianglesOpaqueCounter();
	void _clearStatistics();

	void _beginFrame();
	void _drawDepth();
	void _drawDepthBigTriangles();
	void _drawShadows();
	void _drawShadowsBigTriangles();
	void _finishDepthsRendering();
	void _drawOpaque();
	void _endFrame();
#ifdef USE_WORK_GRAPHS
	void _drawDepthWG();
	void _drawShadowsWG();
	void _drawOpaqueWG();
#endif

	void _drawIndexedInstanced(
		unsigned int indexCountPerInstance,
		unsigned int instanceCount,
		unsigned int startIndexLocation,
		int baseVertexLocation,
		unsigned int startInstanceLocation);

	void _createTriangleDepthPSO();
	void _createBigTriangleDepthPSO();
	void _createTriangleOpaquePSO();
	void _createBigTriangleOpaquePSO();
	void _createBigTrianglesBuffers();

	// need these two for UAV writes
	Microsoft::WRL::ComPtr<ID3D12Resource> _renderTarget;
	Microsoft::WRL::ComPtr<ID3D12Resource> _depthBuffer;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> _triangleDepthRS;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _triangleDepthPSO;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _bigTriangleDepthRS;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _bigTriangleDepthPSO;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _triangleOpaqueRS;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _triangleOpaquePSO;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _bigTriangleOpaqueRS;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _bigTriangleOpaquePSO;
	Microsoft::WRL::ComPtr<ID3D12Resource> _bigTrianglesDepth[MAX_FRUSTUMS_COUNT];
	Microsoft::WRL::ComPtr<ID3D12Resource> _bigTrianglesOpaque;
	Microsoft::WRL::ComPtr<ID3D12Resource> _depthSceneCB;
	unsigned char* _depthSceneCBData;
	int _depthSceneCBFrameSize = 0;
	Microsoft::WRL::ComPtr<ID3D12Resource> _sceneCB;
	unsigned char* _sceneCBData;

	int _width = 0;
	int _height = 0;
	ForwardRenderer* _renderer;

	// MDI stuff
	Microsoft::WRL::ComPtr<ID3D12CommandSignature> _dispatchCS;
	// first 4 bytes used as a counter
	// all 12 bytes are used as a dispatch indirect command
	// [0] - counter / group count X
	// [1] - group count Y
	// [2] - group count Z
	Microsoft::WRL::ComPtr<ID3D12Resource> _bigTrianglesDepthCounters[MAX_FRUSTUMS_COUNT];
	Microsoft::WRL::ComPtr<ID3D12Resource> _bigTrianglesDepthCountersUpload[MAX_FRUSTUMS_COUNT];
	Microsoft::WRL::ComPtr<ID3D12Resource> _bigTrianglesOpaqueCounter;
	Microsoft::WRL::ComPtr<ID3D12Resource> _bigTrianglesOpaqueCounterUpload;
	Microsoft::WRL::ComPtr<ID3D12Resource> _counterReset;

#ifdef USE_WORK_GRAPHS
	Microsoft::WRL::ComPtr<ID3DBlob> _depthWGLibrary;
	Microsoft::WRL::ComPtr<ID3D12StateObject> _depthWGStateObj;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _depthWGRS;
	D3D12_PROGRAM_IDENTIFIER _depthWG;
	D3D12_SET_PROGRAM_DESC _depthProgramDesc;
	Microsoft::WRL::ComPtr<ID3D12Resource> _depthWGBackMem;

	Microsoft::WRL::ComPtr<ID3DBlob> _opaqueWGLibrary;
	Microsoft::WRL::ComPtr<ID3D12StateObject> _opaqueWGStateObj;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _opaqueWGRS;
	D3D12_PROGRAM_IDENTIFIER _opaqueWG;
	D3D12_SET_PROGRAM_DESC _opaqueProgramDesc;
	Microsoft::WRL::ComPtr<ID3D12Resource> _opaqueWGBackMem;
#endif

	// statistics resources
	enum StatsIndices
	{
		PipelineTriangles,
		RenderedTriangles,
		StatsCount
	};
	Microsoft::WRL::ComPtr<ID3D12Resource> _trianglesStats;
	Microsoft::WRL::ComPtr<ID3D12Resource> _trianglesStatsReadback[DX::FramesCount];
	int _statsResult[StatsCount];

	// how much screen space area should triangle's AABB occupy to be considered "big"
	int _bigTriangleThreshold = 4096;
	int _bigTriangleTileSize = 128;

	bool _useTopLeftRule = true;
	bool _scanlineRasterization = true;
};