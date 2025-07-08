#pragma once

#include "DX.h"

class FrameStatistics
{
public:

	FrameStatistics();

	FrameStatistics(const FrameStatistics&) = delete;
	FrameStatistics& operator=(const FrameStatistics&) = delete;

	void BeginMeasure(ID3D12GraphicsCommandList* commandList);
	void FinishMeasure(ID3D12GraphicsCommandList* commandList);

	const D3D12_QUERY_DATA_PIPELINE_STATISTICS& GetStats() const
	{
		return _currentFrameStats;
	}

private:

	Microsoft::WRL::ComPtr<ID3D12QueryHeap> _queryHeap;
	Microsoft::WRL::ComPtr<ID3D12Resource> _queryResult;
	D3D12_QUERY_DATA_PIPELINE_STATISTICS _currentFrameStats;
};

class Profiler
{
public:

	Profiler();

	Profiler(const Profiler&) = delete;
	Profiler& operator=(const Profiler&) = delete;

	void BeginMeasure(ID3D12GraphicsCommandList* commandList);
	void FinishMeasure(ID3D12GraphicsCommandList* commandList);

	float GetTimeMS(ID3D12CommandQueue* queue);

private:

	// per separate command list
	static const unsigned int MaxQueries = 2;
	static const unsigned int FrameCountToAverage = 30;

	struct Profile
	{
		ID3D12GraphicsCommandList* commandList;
		unsigned int queryIndex;
	};

	Profile _profiles[MaxQueries];
	unsigned int _profilesCount = 0;
	Microsoft::WRL::ComPtr<ID3D12QueryHeap> _queryHeap;
	Microsoft::WRL::ComPtr<ID3D12Resource> _queryResult[DX::FramesCount];
	size_t _time[2 * MaxQueries];
	float _lastFrames[FrameCountToAverage];
	unsigned int _lastFramesIndex = 0;
};