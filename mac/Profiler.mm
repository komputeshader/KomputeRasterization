#include "Profiler.h"

#include "Context.h"

struct FrameStatistics::Resources
{
	static constexpr uint32_t MaxSamples = 16;
	id<MTLCounterSampleBuffer> samples[2] = {};
	id<MTLBuffer> results[2] = {};
	uint32_t frameIndex = 1;
	uint32_t sampleIndex = 0;
	bool softwareRasterized = false;
	bool supported = false;
	bool pipelineTrianglesSupported = false;
	bool renderedTrianglesSupported = false;
	bool PSInvocationsSupported = false;
	bool CSInvocationsSupported = false;
	bool VSInvocationsSupported = false;
};

namespace
{
	uint64_t AccumulateDifference(uint64_t total, uint64_t first, uint64_t second)
	{
		return total != MTLCounterErrorValue && first != MTLCounterErrorValue &&
			second != MTLCounterErrorValue && second >= first
			? total + (second - first)
			: MTLCounterErrorValue;
	}
}

void Profiler::BeginMeasure(id<MTLCommandBuffer> commandBuffer)
{
	BeginMeasure(commandBuffer, commandBuffer);
}

void Profiler::BeginMeasure(
	id<MTLCommandBuffer> firstCommandBuffer,
	id<MTLCommandBuffer> lastCommandBuffer)
{
	Profiler* profiler = this;
	[lastCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed)
	{
		double milliseconds = 0.0;
		if (firstCommandBuffer == completed)
		{
			milliseconds = (completed.GPUEndTime - completed.GPUStartTime) * 1000.0;
		}
		else
		{
			milliseconds =
				(firstCommandBuffer.GPUEndTime - firstCommandBuffer.GPUStartTime) * 1000.0 +
				(completed.GPUEndTime - completed.GPUStartTime) * 1000.0;
		}

		const uint32_t index = profiler->_lastFramesIndex.fetch_add(1) % FrameCountToAverage;
		profiler->_lastFrames[index].store(milliseconds);
	}];
}

double Profiler::GetTimeMS() const
{
	double average = 0.0;
	for (uint32_t index = 0; index < FrameCountToAverage; index++)
	{
		average += _lastFrames[index].load();
	}

	return average / FrameCountToAverage;
}

void FrameStatistics::Set(uint64_t pipelineTriangles, uint64_t renderedTriangles)
{
	_pipelineTriangles.store(pipelineTriangles);
	_renderedTriangles.store(renderedTriangles);
}

FrameStatistics::FrameStatistics() :
	_resources(std::make_unique<Resources>())
{
}

FrameStatistics::~FrameStatistics() = default;

void FrameStatistics::Initialize()
{
	if (![Context::Device supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary] ||
		![Context::Device supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary])
	{
		return;
	}

	id<MTLCounterSet> statistics = nil;
	for (id<MTLCounterSet> counterSet in Context::Device.counterSets)
	{
		if ([counterSet.name isEqualToString:MTLCommonCounterSetStatistic])
		{
			statistics = counterSet;
			break;
		}
	}

	if (!statistics)
	{
		return;
	}

	MTLCounterSampleBufferDescriptor* descriptor = [MTLCounterSampleBufferDescriptor new];
	descriptor.counterSet = statistics;
	descriptor.storageMode = MTLStorageModePrivate;
	descriptor.sampleCount = Resources::MaxSamples;
	for (uint32_t frameIndex = 0; frameIndex < 2; frameIndex++)
	{
		NSError* error = nil;
		_resources->samples[frameIndex] = [Context::Device
			newCounterSampleBufferWithDescriptor:descriptor
			error:&error];
		if (!_resources->samples[frameIndex])
		{
			return;
		}

		_resources->results[frameIndex] = Context::CreateBuffer(
			nullptr,
			Resources::MaxSamples * sizeof(MTLCounterResultStatistic),
			MTLResourceStorageModeShared,
			@"Pipeline statistics");
	}

	_resources->supported = true;
	for (id<MTLCounter> counter in statistics.counters)
	{
		_resources->pipelineTrianglesSupported |= [counter.name isEqualToString:MTLCommonCounterClipperInvocations];
		_resources->renderedTrianglesSupported |= [counter.name isEqualToString:MTLCommonCounterClipperPrimitivesOut];
		_resources->PSInvocationsSupported |= [counter.name isEqualToString:MTLCommonCounterFragmentInvocations];
		_resources->CSInvocationsSupported |= [counter.name isEqualToString:MTLCommonCounterComputeKernelInvocations];
		_resources->VSInvocationsSupported |= [counter.name isEqualToString:MTLCommonCounterVertexInvocations];
	}
}

bool FrameStatistics::HasPipelineTriangles(bool softwareRasterized) const
{
	return (softwareRasterized || (_resources->supported && _resources->pipelineTrianglesSupported)) &&
		_pipelineTriangles.load() != MTLCounterErrorValue;
}

bool FrameStatistics::HasRenderedTriangles(bool softwareRasterized) const
{
	return (softwareRasterized || (_resources->supported && _resources->renderedTrianglesSupported)) &&
		_renderedTriangles.load() != MTLCounterErrorValue;
}

bool FrameStatistics::HasPSInvocations() const
{
	return _resources->supported && _resources->PSInvocationsSupported &&
		_PSInvocations.load() != MTLCounterErrorValue;
}

bool FrameStatistics::HasCSInvocations() const
{
	return _resources->supported && _resources->CSInvocationsSupported &&
		_CSInvocations.load() != MTLCounterErrorValue;
}

bool FrameStatistics::HasVSInvocations() const
{
	return _resources->supported && _resources->VSInvocationsSupported &&
		_VSInvocations.load() != MTLCounterErrorValue;
}

void FrameStatistics::BeginFrame(bool softwareRasterized)
{
	_resources->frameIndex = (_resources->frameIndex + 1) % 2;
	_resources->sampleIndex = 0;
	_resources->softwareRasterized = softwareRasterized;
}

void FrameStatistics::BeginMeasure(id<MTLRenderCommandEncoder> encoder)
{
	if (_resources->supported && _resources->sampleIndex < Resources::MaxSamples)
	{
		[encoder sampleCountersInBuffer:_resources->samples[_resources->frameIndex]
			atSampleIndex:_resources->sampleIndex++
			withBarrier:NO];
	}
}

void FrameStatistics::BeginMeasure(id<MTLComputeCommandEncoder> encoder)
{
	if (_resources->supported && _resources->sampleIndex < Resources::MaxSamples)
	{
		[encoder sampleCountersInBuffer:_resources->samples[_resources->frameIndex]
			atSampleIndex:_resources->sampleIndex++
			withBarrier:NO];
	}
}

void FrameStatistics::FinishMeasure(id<MTLRenderCommandEncoder> encoder)
{
	BeginMeasure(encoder);
}

void FrameStatistics::FinishMeasure(id<MTLComputeCommandEncoder> encoder)
{
	BeginMeasure(encoder);
}

void FrameStatistics::FinishFrame(id<MTLCommandBuffer> commandBuffer)
{
	if (!_resources->supported || _resources->sampleIndex == 0)
	{
		return;
	}

	const uint32_t frameIndex = _resources->frameIndex;
	const uint32_t sampleCount = _resources->sampleIndex;
	id<MTLBlitCommandEncoder> encoder = [commandBuffer blitCommandEncoder];
	[encoder resolveCounters:_resources->samples[frameIndex]
		inRange:NSMakeRange(0, sampleCount)
		destinationBuffer:_resources->results[frameIndex]
		destinationOffset:0];
	[encoder endEncoding];

	id<MTLBuffer> results = _resources->results[frameIndex];
	FrameStatistics* statistics = this;
	const bool softwareRasterized = _resources->softwareRasterized;
	[commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>)
	{
		const MTLCounterResultStatistic* values =
			static_cast<const MTLCounterResultStatistic*>(results.contents);
		uint64_t pipelineTriangles = 0;
		uint64_t renderedTriangles = 0;
		uint64_t PSInvocations = 0;
		uint64_t CSInvocations = 0;
		uint64_t VSInvocations = 0;
		for (uint32_t sample = 0; sample + 1 < sampleCount; sample += 2)
		{
			pipelineTriangles = AccumulateDifference(
				pipelineTriangles,
				values[sample].clipperInvocations,
				values[sample + 1].clipperInvocations);
			renderedTriangles = AccumulateDifference(
				renderedTriangles,
				values[sample].clipperPrimitivesOut,
				values[sample + 1].clipperPrimitivesOut);
			PSInvocations = AccumulateDifference(
				PSInvocations,
				values[sample].fragmentInvocations,
				values[sample + 1].fragmentInvocations);
			CSInvocations = AccumulateDifference(
				CSInvocations,
				values[sample].computeKernelInvocations,
				values[sample + 1].computeKernelInvocations);
			VSInvocations = AccumulateDifference(
				VSInvocations,
				values[sample].vertexInvocations,
				values[sample + 1].vertexInvocations);
		}

		if (!softwareRasterized)
		{
			statistics->Set(pipelineTriangles, renderedTriangles);
		}

		statistics->_PSInvocations.store(PSInvocations);
		statistics->_CSInvocations.store(CSInvocations);
		statistics->_VSInvocations.store(VSInvocations);
	}];
}
