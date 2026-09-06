#pragma once

#import <Metal/Metal.h>

#include <atomic>
#include <cstdint>
#include <memory>

class Profiler
{
public:

	void BeginMeasure(id<MTLCommandBuffer> commandBuffer);
	void BeginMeasure(
		id<MTLCommandBuffer> firstCommandBuffer,
		id<MTLCommandBuffer> lastCommandBuffer);

	double GetTimeMS() const;

private:

	static const uint32_t FrameCountToAverage = 30;
	std::atomic<double> _lastFrames[FrameCountToAverage] = {};
	std::atomic<uint32_t> _lastFramesIndex = 0;
};

class FrameStatistics
{
public:

	FrameStatistics();
	FrameStatistics(const FrameStatistics&) = delete;
	FrameStatistics& operator=(const FrameStatistics&) = delete;
	~FrameStatistics();

	void Initialize();
	void BeginFrame(bool softwareRasterized);
	void BeginMeasure(id<MTLRenderCommandEncoder> encoder);
	void BeginMeasure(id<MTLComputeCommandEncoder> encoder);
	void FinishMeasure(id<MTLRenderCommandEncoder> encoder);
	void FinishMeasure(id<MTLComputeCommandEncoder> encoder);
	void FinishFrame(id<MTLCommandBuffer> commandBuffer);
	void Set(uint64_t pipelineTriangles, uint64_t renderedTriangles);

	bool HasPipelineTriangles(bool softwareRasterized) const;
	bool HasRenderedTriangles(bool softwareRasterized) const;
	bool HasPSInvocations() const;
	bool HasCSInvocations() const;
	bool HasVSInvocations() const;

	uint64_t GetPipelineTriangles() const { return _pipelineTriangles.load(); }
	uint64_t GetRenderedTriangles() const { return _renderedTriangles.load(); }
	uint64_t GetPSInvocations() const { return _PSInvocations.load(); }
	uint64_t GetCSInvocations() const { return _CSInvocations.load(); }
	uint64_t GetVSInvocations() const { return _VSInvocations.load(); }

private:

	struct Resources;
	std::unique_ptr<Resources> _resources;

	std::atomic<uint64_t> _pipelineTriangles = 0;
	std::atomic<uint64_t> _renderedTriangles = 0;
	std::atomic<uint64_t> _PSInvocations = 0;
	std::atomic<uint64_t> _CSInvocations = 0;
	std::atomic<uint64_t> _VSInvocations = 0;
};
