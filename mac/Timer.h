#pragma once

#include <chrono>

class Timer
{
public:

	Timer();

	float TotalTime() const;

	float DeltaTime() const { return static_cast<float>(_deltaTime); }

	void Reset();
	void Start();
	void Stop();
	void Tick();

private:

	using Clock = std::chrono::steady_clock;

	Clock::time_point _base;
	Clock::time_point _previous;
	Clock::time_point _current;
	Clock::time_point _stop;
	Clock::duration _paused = Clock::duration::zero();

	double _deltaTime = -1.0;
	bool _stopped = false;
};

