#include "HardwareRasterization.h"

#include "Common.h"
#include "Context.h"
#include "Culler.h"
#include "DescriptorManager.h"
#include "Profiler.h"
#include "Scene.h"
#include "Settings.h"
#include "Shadows.h"
#include "Utils.h"

struct HardwareRasterization::Resources
{
	id<MTLRenderPipelineState> depthPipeline = nil;
	id<MTLRenderPipelineState> shadowPipeline = nil;
	id<MTLRenderPipelineState> opaquePipeline = nil;
	id<MTLRenderPipelineState> overdrawPipeline = nil;
	id<MTLRenderPipelineState> overdrawDisplayPipeline = nil;
	id<MTLBuffer> overdrawBuffer = nil;
	id<MTLDepthStencilState> depthWriteState = nil;
	id<MTLDepthStencilState> depthEqualState = nil;
	id<MTLTexture> depthTexture = nil;
	id<MTLArgumentEncoder> shadowArgumentEncoder = nil;
	id<MTLBuffer> shadowArguments = nil;
};

namespace
{
	MTLVertexDescriptor* VertexDescriptor(bool depthOnly)
	{
		MTLVertexDescriptor* descriptor = [MTLVertexDescriptor vertexDescriptor];
		descriptor.attributes[0].format = MTLVertexFormatFloat3;
		descriptor.attributes[0].offset = 0;
		descriptor.attributes[0].bufferIndex = Bindings::Positions;
		descriptor.layouts[Bindings::Positions].stride = sizeof(VertexPosition);
		descriptor.layouts[Bindings::Positions].stepFunction = MTLVertexStepFunctionPerVertex;
		if (!depthOnly)
		{
			descriptor.attributes[1].format = MTLVertexFormatUInt;
			descriptor.attributes[1].bufferIndex = Bindings::Normals;
			descriptor.layouts[Bindings::Normals].stride = sizeof(VertexNormal);
			descriptor.attributes[2].format = MTLVertexFormatUInt2;
			descriptor.attributes[2].bufferIndex = Bindings::Colors;
			descriptor.layouts[Bindings::Colors].stride = sizeof(VertexColor);
			descriptor.attributes[3].format = MTLVertexFormatUInt;
			descriptor.attributes[3].bufferIndex = Bindings::Texcoords;
			descriptor.layouts[Bindings::Texcoords].stride = sizeof(VertexUV);
		}

		return descriptor;
	}

	id<MTLRenderPipelineState> MakePipeline(bool depthOnly, bool shadows = false)
	{
		MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
		descriptor.label = shadows ? @"Hardware shadow" : depthOnly ? @"Hardware depth" : @"Hardware opaque";
		descriptor.vertexFunction = Context::GetFunction(shadows
			? "DrawShadowVS"
			: depthOnly ? "DrawDepthVS" : "DrawOpaqueVS");
		descriptor.fragmentFunction = depthOnly ? nil : Context::GetFunction("DrawOpaquePSICB");
		descriptor.vertexDescriptor = VertexDescriptor(depthOnly);
		descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
		if (!depthOnly)
		{
			descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		}

		descriptor.supportIndirectCommandBuffers = YES;
		NSError* error = nil;
		id<MTLRenderPipelineState> pipeline = [Context::Device
			newRenderPipelineStateWithDescriptor:descriptor
			error:&error];
		ASSERT(pipeline, "%s", error.localizedDescription.UTF8String)

		return pipeline;
	}

	id<MTLRenderPipelineState> MakeOverdrawPipeline(bool display)
	{
		MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
		descriptor.label = display ? @"Quad overshading display" : @"Quad overshading count";
		descriptor.vertexFunction = Context::GetFunction(display ? "FullscreenVS" : "DrawDepthVS");
		descriptor.fragmentFunction = Context::GetFunction(display ? "DrawOverdrawDisplayPS" : "DrawOverdrawPS");
		if (display)
		{
			descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		}
		else
		{
			descriptor.vertexDescriptor = VertexDescriptor(true);
			descriptor.supportIndirectCommandBuffers = YES;
		}

		NSError* error = nil;
		id<MTLRenderPipelineState> pipeline = [Context::Device
			newRenderPipelineStateWithDescriptor:descriptor
			error:&error];
		ASSERT(pipeline, "%s", error.localizedDescription.UTF8String)

		return pipeline;
	}

	void BindGeometry(
		id<MTLRenderCommandEncoder> encoder,
		const Scene& scene,
		const Culler& culler,
		bool depthOnly)
	{
		[encoder setVertexBuffer:scene.GetPositionsBuffer() offset:0 atIndex:Bindings::Positions];
		if (!depthOnly)
		{
			[encoder setVertexBuffer:scene.GetNormalsBuffer() offset:0 atIndex:Bindings::Normals];
			[encoder setVertexBuffer:scene.GetColorsBuffer() offset:0 atIndex:Bindings::Colors];
			[encoder setVertexBuffer:scene.GetTexcoordsBuffer() offset:0 atIndex:Bindings::Texcoords];
		}

		id<MTLBuffer> instances = Settings::CullingEnabled
			? culler.GetVisibleInstances()
			: scene.GetInstancesBuffer();
		[encoder setVertexBuffer:instances offset:0 atIndex:Bindings::Instances];
		[encoder useResource:scene.GetIndicesBuffer() usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
		[encoder useResource:instances usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
	}

	void Draw(
		id<MTLRenderCommandEncoder> encoder,
		const Scene& scene,
		const Culler& culler,
		uint32_t frustum)
	{
		if (Settings::CullingEnabled)
		{
			id<MTLIndirectCommandBuffer> commands = culler.GetCommands(frustum);
			[encoder useResource:commands usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
			[encoder executeCommandsInBuffer:commands
				indirectBuffer:culler.GetCommandRanges()
				indirectBufferOffset:frustum * sizeof(MTLIndirectCommandBufferExecutionRange)];
		}
		else
		{
			for (const MeshMeta& mesh : scene.GetMeshesMetaCPU())
			{
				[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
					indexCount:mesh.indexCountPerInstance
					indexType:MTLIndexTypeUInt32
					indexBuffer:scene.GetIndicesBuffer()
					indexBufferOffset:mesh.startIndexLocation * sizeof(uint32_t)
					instanceCount:mesh.instanceCount
					baseVertex:mesh.baseVertexLocation
					baseInstance:mesh.startInstanceLocation];
			}
		}
	}

	void ConfigureRasterizer(id<MTLRenderCommandEncoder> encoder)
	{
		[encoder setFrontFacingWinding:MTLWindingClockwise];
		[encoder setCullMode:MTLCullModeBack];
	}
}

HardwareRasterization::HardwareRasterization() :
	_resources(std::make_unique<Resources>())
{
}

HardwareRasterization::~HardwareRasterization() = default;

void HardwareRasterization::Initialize(uint32_t width, uint32_t height)
{
	_resources->depthPipeline = MakePipeline(true);
	_resources->shadowPipeline = MakePipeline(true, true);
	_resources->opaquePipeline = MakePipeline(false);
	_resources->overdrawPipeline = MakeOverdrawPipeline(false);
	_resources->overdrawDisplayPipeline = MakeOverdrawPipeline(true);

	id<MTLFunction> opaqueFragment = Context::GetFunction("DrawOpaquePSICB");
	_resources->shadowArgumentEncoder = [opaqueFragment newArgumentEncoderWithBufferIndex:8];
	_resources->shadowArguments = Context::CreateBuffer(
		nullptr, _resources->shadowArgumentEncoder.encodedLength, MTLResourceStorageModeShared, @"Hardware shadow arguments");

	MTLDepthStencilDescriptor* depthWrite = [MTLDepthStencilDescriptor new];
	depthWrite.depthCompareFunction = MTLCompareFunctionGreater;
	depthWrite.depthWriteEnabled = YES;
	_resources->depthWriteState = [Context::Device newDepthStencilStateWithDescriptor:depthWrite];

	MTLDepthStencilDescriptor* depthEqual = [MTLDepthStencilDescriptor new];
	depthEqual.depthCompareFunction = MTLCompareFunctionEqual;
	depthEqual.depthWriteEnabled = NO;
	_resources->depthEqualState = [Context::Device newDepthStencilStateWithDescriptor:depthEqual];

	Resize(width, height);
}

void HardwareRasterization::Resize(uint32_t width, uint32_t height)
{
	_width = std::max(width, 1u);
	_height = std::max(height, 1u);
	_resources->depthTexture = Context::CreateTexture2D(
		MTLPixelFormatDepth32Float, _width, _height, 1, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, @"Hardware camera depth");
	const NSUInteger quadWidth = (_width + 1) / 2;
	const NSUInteger quadHeight = (_height + 1) / 2;
	_resources->overdrawBuffer = Context::CreateBuffer(
		nullptr, quadWidth * quadHeight * sizeof(uint32_t), MTLResourceStorageModePrivate, @"Hardware quad overshading");
}

void HardwareRasterization::DrawDepths(
	id<MTLCommandBuffer> commandBuffer,
	const Scene& scene,
	const Culler& culler,
	const Shadows& shadows,
	FrameStatistics& statistics)
{
	MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.depthAttachment.texture = _resources->depthTexture;
	pass.depthAttachment.loadAction = MTLLoadActionClear;
	pass.depthAttachment.storeAction = MTLStoreActionStore;
	pass.depthAttachment.clearDepth = 0.0;

	id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
	encoder.label = @"Hardware camera depth";
	[encoder setRenderPipelineState:_resources->depthPipeline];
	[encoder setDepthStencilState:_resources->depthWriteState];
	ConfigureRasterizer(encoder);
	BindGeometry(encoder, scene, culler, true);
	statistics.BeginMeasure(encoder);

	const simd_float4x4 vp = scene.camera.GetVP();
	[encoder setVertexBytes:&vp length:sizeof(vp) atIndex:Bindings::Constants];
	Draw(encoder, scene, culler, 0);
	[encoder endEncoding];

	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.depthAttachment.texture = shadows.GetShadowMapHWR();
		pass.depthAttachment.slice = cascade;
		pass.depthAttachment.loadAction = MTLLoadActionClear;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		pass.depthAttachment.clearDepth = 0.0;

		encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
		encoder.label = @"Hardware shadow depth";
		[encoder setRenderPipelineState:_resources->shadowPipeline];
		[encoder setDepthStencilState:_resources->depthWriteState];
		ConfigureRasterizer(encoder);
		BindGeometry(encoder, scene, culler, true);

		const simd_float4x4 cascadeVP = shadows.GetCascadeVP(cascade);
		[encoder setVertexBytes:&cascadeVP length:sizeof(cascadeVP) atIndex:Bindings::Constants];
		Draw(encoder, scene, culler, cascade + 1);
		if (cascade == Settings::CascadesCount - 1)
		{
			statistics.FinishMeasure(encoder);
		}

		[encoder endEncoding];
	}
}

id<MTLRenderCommandEncoder> HardwareRasterization::DrawOpaque(
	id<MTLCommandBuffer> commandBuffer,
	MTLRenderPassDescriptor* pass,
	const Scene& scene,
	const Culler& culler,
	const Shadows& shadows,
	FrameStatistics& statistics)
{
	if (Settings::ShowOverdraw)
	{
		return _drawOverdraw(commandBuffer, pass, scene, culler, statistics);
	}

	pass.depthAttachment.texture = _resources->depthTexture;
	pass.depthAttachment.loadAction = MTLLoadActionLoad;
	pass.depthAttachment.storeAction = MTLStoreActionStore;

	id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
	encoder.label = @"Hardware opaque";
	[encoder setRenderPipelineState:_resources->opaquePipeline];
	[encoder setDepthStencilState:_resources->depthEqualState];
	ConfigureRasterizer(encoder);
	BindGeometry(encoder, scene, culler, false);
	statistics.BeginMeasure(encoder);

	SceneCB constants;
	constants.vp = scene.camera.GetVP();
	constants.sunDirection = simd_make_float4(
		simd_normalize(ToSIMD(scene.lightDirection)), 0.0f);
	constants.showCascades = shadows.ShowCascades();
	constants.showMeshlets = Settings::ShowMeshlets;
	constants.cascadesCount = Settings::CascadesCount;
	constants.shadowsDistance = shadows.GetShadowDistance();
	for (int cascade = 0; cascade < Settings::CascadesCount; cascade++)
	{
		constants.cascadeVP[cascade] = shadows.GetCascadeVP(cascade);
		reinterpret_cast<float*>(&constants.cascadeBias)[cascade] = shadows.GetCascadeBias(cascade);
		reinterpret_cast<float*>(&constants.cascadeSplits)[cascade] = shadows.GetCascadeSplit(cascade);
	}

	[encoder setVertexBytes:&constants length:sizeof(constants) atIndex:Bindings::Constants];
	[encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:Bindings::Constants];
	[_resources->shadowArgumentEncoder setArgumentBuffer:_resources->shadowArguments offset:0];
	[_resources->shadowArgumentEncoder setTexture:shadows.GetShadowMapHWR() atIndex:0];
	[encoder setFragmentBuffer:_resources->shadowArguments offset:0 atIndex:8];
	[encoder useResource:shadows.GetShadowMapHWR()
		usage:MTLResourceUsageRead
		stages:MTLRenderStageFragment];

	Draw(encoder, scene, culler, 0);
	statistics.FinishMeasure(encoder);

	return encoder;
}

id<MTLRenderCommandEncoder> HardwareRasterization::_drawOverdraw(
	id<MTLCommandBuffer> commandBuffer,
	MTLRenderPassDescriptor* pass,
	const Scene& scene,
	const Culler& culler,
	FrameStatistics& statistics)
{
	id<MTLBlitCommandEncoder> clear = [commandBuffer blitCommandEncoder];
	clear.label = @"Clear quad overshading";
	[clear fillBuffer:_resources->overdrawBuffer
		range:NSMakeRange(0, _resources->overdrawBuffer.length)
		value:0];
	[clear endEncoding];

	// Match the Windows reference view: keep geometry culling, bypass pixel depth.
	MTLRenderPassDescriptor* countPass = [MTLRenderPassDescriptor renderPassDescriptor];
	countPass.renderTargetWidth = _width;
	countPass.renderTargetHeight = _height;
	countPass.defaultRasterSampleCount = 1;
	id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:countPass];
	encoder.label = @"Hardware quad overshading";
	[encoder setRenderPipelineState:_resources->overdrawPipeline];
	ConfigureRasterizer(encoder);
	BindGeometry(encoder, scene, culler, true);
	const simd_float4x4 vp = scene.camera.GetVP();
	[encoder setVertexBytes:&vp length:sizeof(vp) atIndex:Bindings::Constants];
	const uint32_t quadWidth = (_width + 1) / 2;
	[encoder setFragmentBytes:&quadWidth length:sizeof(quadWidth) atIndex:Bindings::Constants];
	[encoder setFragmentBuffer:_resources->overdrawBuffer offset:0 atIndex:Bindings::Counters];
	statistics.BeginMeasure(encoder);
	Draw(encoder, scene, culler, 0);
	statistics.FinishMeasure(encoder);
	[encoder endEncoding];

	// Tracked resources synchronize the clear, count, and display encoders.
	pass.depthAttachment.texture = nil;
	encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
	encoder.label = @"Quad overshading display";
	[encoder setRenderPipelineState:_resources->overdrawDisplayPipeline];
	[encoder setFragmentBytes:&quadWidth length:sizeof(quadWidth) atIndex:Bindings::Constants];
	[encoder setFragmentBuffer:_resources->overdrawBuffer offset:0 atIndex:Bindings::Counters];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

	return encoder;
}

id<MTLTexture> HardwareRasterization::GetDepthTexture() const
{
	return _resources->depthTexture;
}
