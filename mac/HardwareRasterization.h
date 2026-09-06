#pragma once

#import <Metal/Metal.h>

#include <memory>

class Culler;
class FrameStatistics;
class Scene;
class Shadows;

class HardwareRasterization
{
public:

	HardwareRasterization();
	HardwareRasterization(const HardwareRasterization&) = delete;
	HardwareRasterization& operator=(const HardwareRasterization&) = delete;
	~HardwareRasterization();

	void Initialize(uint32_t width, uint32_t height);
	void Resize(uint32_t width, uint32_t height);
	void DrawDepths(
		id<MTLCommandBuffer> commandBuffer,
		const Scene& scene,
		const Culler& culler,
		const Shadows& shadows,
		FrameStatistics& statistics);
	id<MTLRenderCommandEncoder> DrawOpaque(
		id<MTLCommandBuffer> commandBuffer,
		MTLRenderPassDescriptor* renderPass,
		const Scene& scene,
		const Culler& culler,
		const Shadows& shadows,
		FrameStatistics& statistics);

	id<MTLTexture> GetDepthTexture() const;

private:

	struct Resources;
	std::unique_ptr<Resources> _resources;

	uint32_t _width = 0;
	uint32_t _height = 0;
};

