#pragma once

#import <MetalKit/MetalKit.h>

#include "Culler.h"
#include "HardwareRasterization.h"
#include "Profiler.h"
#include "Scene.h"
#include "Shadows.h"
#include "SoftwareRasterization.h"
#include "Timer.h"

#include <memory>

class ForwardRenderer
{
public:

	ForwardRenderer();
	~ForwardRenderer();

	bool Initialize(MTKView* view);
	void DrawableSizeChanged(CGSize size);
	void Draw(MTKView* view);

	void KeyboardInput();
	void KeyPressed(unsigned char key);
	void SetKey(unsigned char key, bool pressed);
	void MouseDelta(float x, float y);

private:

	struct Resources;
	std::unique_ptr<Resources> _resources;

	void _loadScene(ScenesIndices kind);
	void Update();
	void _newFrameGUI();
	void _encodeCameraHistory(
		id<MTLCommandBuffer> commandBuffer,
		bool softwareRasterized);
	void _generateHiZ(
		id<MTLCommandBuffer> commandBuffer,
		bool softwareRasterized,
		bool perTriangleHiZRasterizationCullingEnabled);
	id<MTLRenderCommandEncoder> _beginComposite(
		id<MTLCommandBuffer> commandBuffer,
		MTLRenderPassDescriptor* pass);

	MTKView* _view = nil;
	Scene* _scene = nullptr;
	Scene _plantScene;
	Scene _buddhaScene;
	Shadows _shadows;
	Culler _culler;
	HardwareRasterization _HWR;
	SoftwareRasterization _SWR;
	Profiler _profiler;
	FrameStatistics _stats;
	Timer _timer;

	bool _keys[256] = {};
	bool _hasCameraHistory = false;
	uint64_t _frameNumber = 0;
	uint64_t _eventValue = 0;
	uint64_t _lastComputeValue = 0;
};

