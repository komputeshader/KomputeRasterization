#pragma once

#import <Metal/Metal.h>

#include "CPUGPUCommon.h"

#include <memory>

class Culler;
class FrameStatistics;
class Scene;
class Shadows;

class SoftwareRasterization
{
public:

	SoftwareRasterization();
	SoftwareRasterization(const SoftwareRasterization&) = delete;
	SoftwareRasterization& operator=(const SoftwareRasterization&) = delete;
	~SoftwareRasterization();

	void Initialize(uint32_t width, uint32_t height);
	void Resize(uint32_t width, uint32_t height);
	void GUINewFrame();
	void DrawDepths(
		id<MTLCommandBuffer> commandBuffer,
		const Scene& scene,
		const Culler& culler,
		const Shadows& shadows,
		id<MTLTexture> previousCameraHiZ,
		bool hasCameraHistory,
		FrameStatistics& statistics);
	void DrawOpaque(
		id<MTLCommandBuffer> commandBuffer,
		const Scene& scene,
		const Culler& culler,
		const Shadows& shadows,
		id<MTLTexture> previousCameraHiZ,
		bool hasCameraHistory,
		FrameStatistics& statistics);
	id<MTLTexture> GetRenderTarget() const;
	id<MTLBuffer> GetDepthBuffer() const;

private:

	struct Resources;
	std::unique_ptr<Resources> _resources;

	void _createBigTrianglesBuffers();
	void _drawDepth(
		id<MTLComputeCommandEncoder> encoder,
		const Scene& scene,
		const Culler& culler,
		const Shadows& shadows,
		id<MTLTexture> previousCameraHiZ,
		bool hasCameraHistory,
		uint32_t frustum);
	void _drawDepthBigTriangles(
		id<MTLComputeCommandEncoder> encoder,
		const Scene& scene,
		const Shadows& shadows,
		uint32_t frustum);

	uint32_t _width = 0;
	uint32_t _height = 0;
	uint32_t _maxBigTrianglesDepth[MAX_FRUSTUMS_COUNT] = {};
	uint32_t _maxBigTrianglesOpaque = 0;
	uint32_t _frameIndex = 1;

	int _bigTriangleThreshold = 4096;
	int _bigTriangleTileSize = 128;

	bool _useTopLeftRule = true;
	bool _scanlineRasterization = true;
};

