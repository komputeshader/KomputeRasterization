#include "ForwardRenderer.h"

#include "Context.h"
#include "Settings.h"
#include "Utils.h"

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include <mach/mach.h>

struct ForwardRenderer::Resources
{
	id<MTLTexture> previousCameraHiZ = nil;
	id<MTLComputePipelineState> copyHardwareDepth = nil;
	id<MTLComputePipelineState> copySoftwareDepth = nil;
	id<MTLComputePipelineState> downsample = nil;
	id<MTLRenderPipelineState> composite = nil;
	dispatch_semaphore_t frameSemaphore = dispatch_semaphore_create(2);
};

namespace
{
	NSUInteger MipCount(NSUInteger size)
	{
		NSUInteger count = 1;
		while (size > 1)
		{
			size >>= 1;
			++count;
		}

		return count;
	}

	uint64_t CurrentCPUMemoryUsage()
	{
		mach_task_basic_info_data_t information;
		mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
		const kern_return_t result = task_info(
			mach_task_self(),
			MACH_TASK_BASIC_INFO,
			reinterpret_cast<task_info_t>(&information),
			&count);

		return result == KERN_SUCCESS ? information.resident_size : 0;
	}
}

ForwardRenderer::ForwardRenderer() :
	_resources(std::make_unique<Resources>())
{
}

ForwardRenderer::~ForwardRenderer()
{
	if (ImGui::GetCurrentContext())
	{
		ImGui_ImplMetal_Shutdown();
		ImGui_ImplOSX_Shutdown();
		ImGui::DestroyContext();
	}
}

bool ForwardRenderer::Initialize(MTKView* view)
{
	_view = view;
	if (!Context::Initialize(view))
	{
		return false;
	}

	view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
	view.depthStencilPixelFormat = MTLPixelFormatInvalid;
	view.framebufferOnly = YES;
	view.clearColor = MTLClearColorMake(SkyColor.x, SkyColor.y, SkyColor.z, 1.0);
	Settings::AssetsPath = Utils::FindAssetsPath().string();

	_resources->copyHardwareDepth = Context::CreateComputePipeline("CopyDepth");
	_resources->copySoftwareDepth = Context::CreateComputePipeline("CopyFloat");
	_resources->downsample = Context::CreateComputePipeline("GenerateHiZMip");

	MTLRenderPipelineDescriptor* compositeDescriptor = [MTLRenderPipelineDescriptor new];
	compositeDescriptor.label = @"Software rasterizer composite";
	compositeDescriptor.vertexFunction = Context::GetFunction("FullscreenVS");
	compositeDescriptor.fragmentFunction = Context::GetFunction("CompositePS");
	compositeDescriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat;
	NSError* error = nil;
	_resources->composite = [Context::Device
		newRenderPipelineStateWithDescriptor:compositeDescriptor
		error:&error];
	ASSERT(_resources->composite, "%s", error.localizedDescription.UTF8String)
	_stats.Initialize();

	_plantScene.Load(ScenesIndices::Plant);
	_buddhaScene.Load(ScenesIndices::Buddha);
	_loadScene(ScenesIndices::Buddha);
	_culler.Initialize();
	_shadows.Initialize();
	_HWR.Initialize(Settings::BackBufferWidth, Settings::BackBufferHeight);
	_SWR.Initialize(Settings::BackBufferWidth, Settings::BackBufferHeight);
	DrawableSizeChanged(view.drawableSize);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplOSX_Init(view);
	ImGui_ImplMetal_Init(Context::Device);
	_timer.Reset();
	return true;
}

void ForwardRenderer::_loadScene(ScenesIndices kind)
{
	_scene = kind == ScenesIndices::Buddha ? &_buddhaScene : &_plantScene;
	_hasCameraHistory = false;
}

void ForwardRenderer::DrawableSizeChanged(CGSize size)
{
	const uint32_t width = std::max<uint32_t>(1, static_cast<uint32_t>(size.width));
	const uint32_t height = std::max<uint32_t>(1, static_cast<uint32_t>(size.height));
	Settings::RenderWidth = width;
	Settings::RenderHeight = height;
	_scene->camera.SetProjection(
		_scene->FOV * static_cast<float>(M_PI) / 180.0f,
		static_cast<float>(width) / height,
		_scene->nearZ,
		_scene->farZ);
	_HWR.Resize(width, height);
	_SWR.Resize(width, height);
	_resources->previousCameraHiZ = Context::CreateTexture2D(
		MTLPixelFormatR32Float,
		width,
		height,
		MipCount(std::max(width, height)),
		1,
		MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
		@"Previous camera Hi-Z");
	_hasCameraHistory = false;
}

void ForwardRenderer::SetKey(unsigned char key, bool pressed)
{
	_keys[key] = pressed;
}

void ForwardRenderer::MouseDelta(float x, float y)
{
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)
	{
		return;
	}

	constexpr float sensitivity = 0.003f;
	_scene->camera.RotateY(x * sensitivity);
	_scene->camera.RotateX(-y * sensitivity);
}

// used for camera movement
void ForwardRenderer::KeyboardInput()
{
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard)
	{
		return;
	}

	float cameraSpeed = 200.0f;

	if (_keys[0x10])
	{
		cameraSpeed = 1000.0f;
	}

	float dt = static_cast<float>(_timer.DeltaTime()) * cameraSpeed;

	Camera& camera = _scene->camera;

	if (_keys[0x25] || _keys['A'])
	{
		camera.Strafe(-dt);
	}

	if (_keys[0x27] || _keys['D'])
	{
		camera.Strafe(dt);
	}

	if (_keys[0x26] || _keys['W'])
	{
		camera.Walk(dt);
	}

	if (_keys[0x28] || _keys['S'])
	{
		camera.Walk(-dt);
	}

	if (_keys[0x20] || _keys['E'])
	{
		camera.MoveVertical(dt);
	}

	if (_keys['Q'])
	{
		camera.MoveVertical(-dt);
	}
}

void ForwardRenderer::KeyPressed(unsigned char key)
{
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard)
	{
		return;
	}

	// F is 0x46
	if (key == 0x46)
	{
		Settings::FreezeCulling = !Settings::FreezeCulling;
	}
}

void ForwardRenderer::Update()
{
	_timer.Tick();

	KeyboardInput();

	_scene->camera.UpdateViewMatrix();
	_shadows.Update(*_scene);
}

void ForwardRenderer::_newFrameGUI()
{
	const float guiSpacing = ImGui::GetFontSize() * 0.625f;
	int location = Settings::StatsGUILocation;
	ImGuiWindowFlags window_flags =
		ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav;
	if (location >= 0)
	{
		const float PAD = guiSpacing;
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImVec2 work_pos = viewport->WorkPos;
		ImVec2 work_size = viewport->WorkSize;
		ImVec2 window_pos, window_pos_pivot;
		window_pos.x = (location & 1)
			? (work_pos.x + work_size.x - PAD)
			: (work_pos.x + PAD);
		window_pos.y = (location & 2)
			? (work_pos.y + work_size.y - PAD)
			: (work_pos.y + PAD);
		window_pos_pivot.x = (location & 1) ? 1.0f : 0.0f;
		window_pos_pivot.y = (location & 2) ? 1.0f : 0.0f;
		ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
		window_flags |= ImGuiWindowFlags_NoMove;
	}

	ImGui::SetNextWindowBgAlpha(Settings::GUITransparency);
	if (ImGui::Begin("Scene Information", nullptr, window_flags))
	{
		ImGui::Text("%s", Context::Device.name.UTF8String);
		ImGui::Dummy(ImVec2(0.0f, guiSpacing));

		int scene = _scene->GetKind() == ScenesIndices::Buddha ? 0 : 1;
		if (ImGui::Combo("Scene", &scene, "Buddha\0Plant\0"))
		{
			_loadScene(scene == 0 ? ScenesIndices::Buddha : ScenesIndices::Plant);
		}

		ImGui::Dummy(ImVec2(0.0f, guiSpacing));
		ImGui::Text(
			"Total triangles in the scene: %.3f Mil",
			_scene->GetTotalFacesCount() / 1'000'000.0f);
		if (_stats.HasPipelineTriangles(Settings::SWREnabled))
		{
			ImGui::Text(
				"Triangles On Pipeline: %.3f Mil",
				_stats.GetPipelineTriangles() / 1'000'000.0f);
		}

		if (_stats.HasRenderedTriangles(Settings::SWREnabled))
		{
			ImGui::Text(
				"Triangles Rendered: %.3f Mil",
				_stats.GetRenderedTriangles() / 1'000'000.0f);
		}

		const bool hasPSInvocations = _stats.HasPSInvocations();
		const bool hasCSInvocations = _stats.HasCSInvocations();
		if (hasPSInvocations || hasCSInvocations)
		{
			ImGui::Dummy(ImVec2(0.0f, guiSpacing));
			if (hasPSInvocations)
			{
				ImGui::Text(
					"PS Invocations: %.3f Mil",
					_stats.GetPSInvocations() / 1'000'000.0f);
			}

			if (hasCSInvocations)
			{
				ImGui::Text(
					"CS Invocations: %.3f Mil",
					_stats.GetCSInvocations() / 1'000'000.0f);
			}
		}

		if (!Settings::SWREnabled && _stats.HasPipelineTriangles(false) && _stats.HasVSInvocations())
		{
			const double pipelineTriangles = _stats.GetPipelineTriangles();
			if (pipelineTriangles > 0.0)
			{
				ImGui::Dummy(ImVec2(0.0f, guiSpacing));
				ImGui::Text(
					"Average Vertex Cache Miss Rate\n"
					"(0.5 is ideal, 3.0 is terrible): %.3f",
					_stats.GetVSInvocations() / pipelineTriangles);
			}
		}

		ImGui::Dummy(ImVec2(0.0f, guiSpacing));
		ImGui::Text(
			"Current Metal GPU Memory Usage: %.1f GB / %.1f GB",
			Context::Device.currentAllocatedSize / 1'000'000'000.0,
			Context::Device.recommendedMaxWorkingSetSize / 1'000'000'000.0);
		ImGui::Text(
			"Current Metal CPU Memory Usage: %.1f GB / %.1f GB",
			CurrentCPUMemoryUsage() / 1'000'000'000.0,
			NSProcessInfo.processInfo.physicalMemory / 1'000'000'000.0);

		ImGui::Dummy(ImVec2(0.0f, guiSpacing));
		ImGui::Text("Frame Time:");
		ImGui::SameLine();
		const float frameTime = static_cast<float>(_profiler.GetTimeMS());
		ImVec4 frameColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
		if (frameTime > 33.3f)
		{
			frameColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		}
		else if (frameTime > 16.6f)
		{
			frameColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
		}

		ImGui::TextColored(frameColor, "%.1f ms", frameTime);

		ImGui::Dummy(ImVec2(0.0f, guiSpacing));
		ImGui::Checkbox("Software Rasterization", &Settings::SWREnabled);
		if (!Settings::SWREnabled)
		{
			ImGui::Checkbox("Async Compute", &Settings::AsyncComputeEnabled);
			ImGui::Checkbox("Show Quad Overshading", &Settings::ShowOverdraw);
		}

		ImGui::Checkbox("Frustum Culling", &Settings::FrustumCullingEnabled);
		ImGui::Checkbox("Cluster Backface Culling", &Settings::ClusterBackfaceCullingEnabled);
		ImGui::Checkbox("Camera Hi-Z Culling", &Settings::CameraHiZCullingEnabled);
		ImGui::Checkbox("Shadows Hi-Z Culling", &Settings::ShadowsHiZCullingEnabled);
		if (Settings::SWREnabled)
		{
			ImGui::Checkbox(
				"Per-triangle Hi-Z Rasterization Culling",
				&Settings::PerTriangleHiZRasterizationCullingEnabled);
		}

		Settings::CullingEnabled =
			Settings::FrustumCullingEnabled ||
			Settings::CameraHiZCullingEnabled ||
			Settings::ShadowsHiZCullingEnabled ||
			Settings::ClusterBackfaceCullingEnabled;
	}

	ImGui::End();
}

void ForwardRenderer::_encodeCameraHistory(
	id<MTLCommandBuffer> commandBuffer,
	bool softwareRasterized)
{
	id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
	encoder.label = @"Camera Hi-Z";
	id<MTLComputePipelineState> copy = softwareRasterized
		? _resources->copySoftwareDepth
		: _resources->copyHardwareDepth;
	[encoder setComputePipelineState:copy];
	if (softwareRasterized)
	{
		[encoder setBuffer:_SWR.GetDepthBuffer() offset:0 atIndex:0];
	}
	else
	{
		[encoder setTexture:_HWR.GetDepthTexture() atIndex:0];
	}

	[encoder setTexture:_resources->previousCameraHiZ atIndex:1];
	const MTLSize threads = MTLSizeMake(8, 8, 1);
	[encoder dispatchThreads:MTLSizeMake(Settings::RenderWidth, Settings::RenderHeight, 1)
		threadsPerThreadgroup:threads];
	[encoder memoryBarrierWithScope:MTLBarrierScopeTextures];
	[encoder setComputePipelineState:_resources->downsample];
	for (uint32_t mip = 1; mip < _resources->previousCameraHiZ.mipmapLevelCount; mip++)
	{
		const uint32_t levels[2] = { mip - 1, mip };
		[encoder setBytes:levels length:sizeof(levels) atIndex:0];
		[encoder dispatchThreads:MTLSizeMake(
			std::max<uint32_t>(1, Settings::RenderWidth >> mip),
			std::max<uint32_t>(1, Settings::RenderHeight >> mip),
			1)
			threadsPerThreadgroup:threads];
		[encoder memoryBarrierWithScope:MTLBarrierScopeTextures];
	}

	[encoder endEncoding];
	_hasCameraHistory = true;
}

void ForwardRenderer::_generateHiZ(
	id<MTLCommandBuffer> commandBuffer,
	bool softwareRasterized,
	bool perTriangleHiZRasterizationCullingEnabled)
{
	if (Settings::CameraHiZCullingEnabled ||
		perTriangleHiZRasterizationCullingEnabled)
	{
		_encodeCameraHistory(commandBuffer, softwareRasterized);
	}

	if (Settings::ShadowsHiZCullingEnabled ||
		perTriangleHiZRasterizationCullingEnabled)
	{
		_shadows.EncodeHistory(commandBuffer, softwareRasterized);
	}
}

id<MTLRenderCommandEncoder> ForwardRenderer::_beginComposite(
	id<MTLCommandBuffer> commandBuffer,
	MTLRenderPassDescriptor* pass)
{
	id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
	encoder.label = @"Software rasterizer composite";
	[encoder setRenderPipelineState:_resources->composite];
	[encoder setFragmentTexture:_SWR.GetRenderTarget() atIndex:0];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	return encoder;
}

void ForwardRenderer::Draw(MTKView* view)
{
	dispatch_semaphore_wait(_resources->frameSemaphore, DISPATCH_TIME_FOREVER);
	@autoreleasepool
	{
		id<CAMetalDrawable> drawable = view.currentDrawable;
		MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
		if (!drawable || !pass)
		{
			dispatch_semaphore_signal(_resources->frameSemaphore);
			return;
		}

		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = view.clearColor;
		MTLRenderPassDescriptor* guiPass = [MTLRenderPassDescriptor renderPassDescriptor];
		guiPass.colorAttachments[0].texture = drawable.texture;
		guiPass.colorAttachments[0].loadAction = MTLLoadActionLoad;
		guiPass.colorAttachments[0].storeAction = MTLStoreActionStore;

		ImGui_ImplMetal_NewFrame(guiPass);
		ImGui_ImplOSX_NewFrame(view);
		ImGui::NewFrame();
		_newFrameGUI();
		_shadows.GUINewFrame(*_scene);
		if (Settings::SWREnabled)
		{
			_SWR.GUINewFrame();
		}

		ImGui::Render();
		Update();
		_stats.BeginFrame(Settings::SWREnabled);

		const bool culledCommandsRequired = Settings::CullingEnabled || Settings::SWREnabled;
		uint64_t computeReady = _lastComputeValue;
		if (culledCommandsRequired)
		{
			id<MTLCommandBuffer> culling = [Context::ComputeCommandQueue commandBuffer];
			culling.label = @"GPU culling";
			_culler.Cull(
				culling,
				*_scene,
				_shadows,
				_resources->previousCameraHiZ,
				_hasCameraHistory);
			computeReady = ++_eventValue;
			[culling encodeSignalEvent:Context::SharedEvent value:computeReady];
			[culling commit];
		}

		if (computeReady != 0)
		{
			id<MTLCommandBuffer> waitForCompute = [Context::CommandQueue commandBuffer];
			[waitForCompute encodeWaitForEvent:Context::SharedEvent value:computeReady];
			[waitForCompute commit];
		}

		id<MTLCommandBuffer> graphics = nil;

		if (Settings::SWREnabled)
		{
			const bool perTriangleHiZRasterizationCullingEnabled =
				Settings::PerTriangleHiZRasterizationCullingEnabled;
			graphics = [Context::CommandQueue commandBuffer];
			graphics.label = @"Software frame";
			_SWR.DrawDepths(graphics, *_scene, _culler, _shadows, _resources->previousCameraHiZ, _hasCameraHistory, _stats);
			_generateHiZ(
				graphics,
				true,
				perTriangleHiZRasterizationCullingEnabled);
			_SWR.DrawOpaque(graphics, *_scene, _culler, _shadows, _resources->previousCameraHiZ, _hasCameraHistory, _stats);
			id<MTLRenderCommandEncoder> encoder = _beginComposite(graphics, pass);
			[encoder endEncoding];
			_profiler.BeginMeasure(graphics);
		}
		else if (Settings::AsyncComputeEnabled)
		{
			id<MTLCommandBuffer> depths = [Context::CommandQueue commandBuffer];
			depths.label = @"Hardware depths";
			_HWR.DrawDepths(depths, *_scene, _culler, _shadows, _stats);
			const uint64_t depthsReady = ++_eventValue;
			[depths encodeSignalEvent:Context::SharedEvent value:depthsReady];
			[depths addCompletedHandler:^(id<MTLCommandBuffer> completed)
			{
				if (completed.status == MTLCommandBufferStatusError)
				{
					Utils::Log("Metal depth command buffer failed: %s\n",
						completed.error.localizedDescription.UTF8String);
				}
			}];
			[depths commit];

			if (Settings::CameraHiZCullingEnabled ||
				Settings::ShadowsHiZCullingEnabled)
			{
				id<MTLCommandBuffer> history = [Context::ComputeCommandQueue commandBuffer];
				history.label = @"Async depth history";
				[history encodeWaitForEvent:Context::SharedEvent value:depthsReady];
				_generateHiZ(history, false, false);
				_lastComputeValue = ++_eventValue;
				[history encodeSignalEvent:Context::SharedEvent value:_lastComputeValue];
				[history addCompletedHandler:^(id<MTLCommandBuffer> completed)
				{
					if (completed.status == MTLCommandBufferStatusError)
					{
						Utils::Log("Metal history command buffer failed: %s\n",
							completed.error.localizedDescription.UTF8String);
					}
				}];
				[history commit];
			}

			graphics = [Context::CommandQueue commandBuffer];
			graphics.label = @"Hardware opaque frame";
			id<MTLRenderCommandEncoder> encoder =
				_HWR.DrawOpaque(graphics, pass, *_scene, _culler, _shadows, _stats);
			[encoder endEncoding];
			_profiler.BeginMeasure(depths, graphics);
		}
		else
		{
			graphics = [Context::CommandQueue commandBuffer];
			graphics.label = @"Hardware frame";

			_HWR.DrawDepths(graphics, *_scene, _culler, _shadows, _stats);
			_generateHiZ(graphics, false, false);
			id<MTLRenderCommandEncoder> encoder =
				_HWR.DrawOpaque(graphics, pass, *_scene, _culler, _shadows, _stats);
			[encoder endEncoding];
			_profiler.BeginMeasure(graphics);
		}

		_stats.FinishFrame(graphics);
		id<MTLRenderCommandEncoder> guiEncoder =
			[graphics renderCommandEncoderWithDescriptor:guiPass];
		guiEncoder.label = @"GUI";
		ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), graphics, guiEncoder);
		[guiEncoder endEncoding];

		[graphics presentDrawable:drawable];
		dispatch_semaphore_t semaphore = _resources->frameSemaphore;
		[graphics addCompletedHandler:^(id<MTLCommandBuffer> completed)
		{
			if (completed.status == MTLCommandBufferStatusError)
			{
				Utils::Log("Metal command buffer failed: %s\n", completed.error.localizedDescription.UTF8String);
			}

			dispatch_semaphore_signal(semaphore);
		}];
		[graphics commit];
	}
}
