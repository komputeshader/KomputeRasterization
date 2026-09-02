#include "Profiler.h"
#include "DX.h"

FrameStatistics::FrameStatistics()
{
	D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
	queryHeapDesc.Count = 2 * DX::FramesCount;
	queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
	SUCCESS(DX::Device->CreateQueryHeap(
		&queryHeapDesc,
		IID_PPV_ARGS(&_queryHeap)));
	NAME_D3D12_OBJECT(_queryHeap);

	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
		2 * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
	for (int frame = 0; frame < DX::FramesCount; frame++)
	{
		SUCCESS(DX::Device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&_queryResult[frame])));
		NAME_D3D12_OBJECT_INDEXED(_queryResult, frame);
		SUCCESS(_queryResult[frame]->Map(
			0,
			nullptr,
			reinterpret_cast<void**>(&_queryResultData[frame])));
	}
}

void FrameStatistics::BeginMeasure(
	ID3D12GraphicsCommandList* commandList,
	unsigned int queryIndex)
{
	if (queryIndex == 0 && _hasResults[DX::FrameIndex])
	{
		ZeroMemory(&_currentFrameStats, sizeof(_currentFrameStats));
		UINT64* current = reinterpret_cast<UINT64*>(&_currentFrameStats);
		for (int query = 0; query < 2; query++)
		{
			UINT64* source = reinterpret_cast<UINT64*>(
				&_queryResultData[DX::FrameIndex][query]);
			for (int field = 0;
				field < sizeof(_currentFrameStats) / sizeof(UINT64);
				field++)
			{
				current[field] += source[field];
			}
		}
	}

	commandList->BeginQuery(
		_queryHeap.Get(),
		D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
		DX::FrameIndex * 2 + queryIndex);
}

void FrameStatistics::FinishMeasure(
	ID3D12GraphicsCommandList* commandList,
	unsigned int queryIndex)
{
	unsigned int heapIndex = DX::FrameIndex * 2 + queryIndex;
	commandList->EndQuery(
		_queryHeap.Get(),
		D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
		heapIndex);

	commandList->ResolveQueryData(
		_queryHeap.Get(),
		D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
		heapIndex,
		1,
		_queryResult[DX::FrameIndex].Get(),
		queryIndex * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
	_hasResults[DX::FrameIndex] = true;
}

Profiler::Profiler()
{
	D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
	// each profile is a start and end timestamps (size_ts)
	queryHeapDesc.Count = 2 * MaxQueries;
	queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	SUCCESS(DX::Device->CreateQueryHeap(
		&queryHeapDesc,
		IID_PPV_ARGS(&_queryHeap)));
	NAME_D3D12_OBJECT(_queryHeap);

	auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(2 * MaxQueries * sizeof(size_t));
	for (int frame = 0; frame < DX::FramesCount; frame++)
	{
		SUCCESS(DX::Device->CreateCommittedResource(
			&prop,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&_queryResult[frame])));
		NAME_D3D12_OBJECT_INDEXED(_queryResult, frame);
		SUCCESS(_queryResult[frame]->Map(
			0,
			nullptr,
			reinterpret_cast<void**>(&_queryResultData[frame])));
	}

	ZeroMemory(
		_lastFrames,
		FrameCountToAverage * sizeof(float));
}

void Profiler::BeginMeasure(ID3D12GraphicsCommandList* commandList)
{
	Profile& profile = _profiles[_profilesCount];
	profile.commandList = commandList;
	profile.queryIndex = _profilesCount * 2;

	commandList->EndQuery(
		_queryHeap.Get(),
		D3D12_QUERY_TYPE_TIMESTAMP,
		profile.queryIndex);

	_profilesCount++;
}

void Profiler::FinishMeasure(ID3D12GraphicsCommandList* commandList)
{
	unsigned int cmdListProfileIndex = 0;
	for (unsigned int i = 0; i < MaxQueries; i++)
	{
		if (_profiles[i].commandList == commandList)
		{
			cmdListProfileIndex = i;
			break;
		}
	}

	unsigned int queryIndex = _profiles[cmdListProfileIndex].queryIndex;
	commandList->EndQuery(
		_queryHeap.Get(),
		D3D12_QUERY_TYPE_TIMESTAMP,
		queryIndex + 1);

	commandList->ResolveQueryData(
		_queryHeap.Get(),
		D3D12_QUERY_TYPE_TIMESTAMP,
		queryIndex,
		2,
		_queryResult[DX::FrameIndex].Get(),
		queryIndex * sizeof(size_t));
	_hasResults[DX::FrameIndex] = true;
}

float Profiler::GetTimeMS(ID3D12CommandQueue* queue)
{
	if (!_hasResults[DX::FrameIndex])
	{
		_profilesCount = 0;
		return 0.0f;
	}

	memcpy(
		_time,
		_queryResultData[DX::FrameIndex],
		2 * MaxQueries * sizeof(size_t));

	size_t GPUFrequency;
	queue->GetTimestampFrequency(&GPUFrequency);
	float frequency = static_cast<float>(GPUFrequency);

	// iterate over command lists to sum up their running times
	// TODO: probably incorrect way to measure frame time due to
	// possible async execution of command lists
	float frameTime = 0.0f;
	for (unsigned int i = 0; i < _profilesCount; i++)
	{
		float delta = static_cast<float>(_time[2 * i + 1] - _time[2 * i]);
		frameTime += (delta / frequency) * 1000.0f;
	}

	_profilesCount = 0;

	_lastFrames[_lastFramesIndex++] = frameTime;
	if (_lastFramesIndex == FrameCountToAverage)
	{
		_lastFramesIndex = 0;
	}
	float avgTime = 0.0f;
	for (unsigned int i = 0; i < FrameCountToAverage; i++)
	{
		avgTime += _lastFrames[i];
	}
	avgTime /= static_cast<float>(FrameCountToAverage);

	return avgTime;
}
