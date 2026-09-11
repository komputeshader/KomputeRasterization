#pragma once

#import <Metal/Metal.h>

#include "Common.h"

#include <memory>

class Scene;
class Shadows;

class Culler
{
public:

	Culler();
	Culler(const Culler&) = delete;
	Culler& operator=(const Culler&) = delete;
	~Culler();

	void Initialize();
	void Cull(
		id<MTLCommandBuffer> commandBuffer,
		const Scene& scene,
		const Shadows& shadows,
		id<MTLTexture> previousCameraHiZ,
		bool hasCameraHistory);

	id<MTLBuffer> GetVisibleInstances() const;
	id<MTLBuffer> GetInstanceCounters() const;
	id<MTLIndirectCommandBuffer> GetCommands() const;
	id<MTLBuffer> GetCommandRanges() const;
	id<MTLBuffer> GetSoftwareCommands() const;
	id<MTLBuffer> GetSoftwareDispatchArguments() const;
	uint32_t GetMaxInstances() const { return _maxInstances; }
	uint32_t GetMaxMeshes() const { return _maxMeshes; }

private:

	struct Resources;
	std::unique_ptr<Resources> _resources;

	uint32_t _maxInstances = 0;
	uint32_t _maxMeshes = 0;
	uint32_t _frameIndex = 1;
};

