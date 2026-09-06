#include "Timer.h"

Timer::Timer()
{
	Reset();
}

void Timer::Reset()
{
	_base = _previous = _current = Clock::now();
	_stop = {};
	_paused = Clock::duration::zero();
	_stopped = false;
}

void Timer::Start()
{
	const Clock::time_point start = Clock::now();

	if (_stopped)
	{
		_paused += start - _stop;
		_previous = start;
		_stop = {};
		_stopped = false;
	}
}

void Timer::Stop()
{
	if (!_stopped)
	{
		_stop = Clock::now();
		_stopped = true;
	}
}

void Timer::Tick()
{
	if (_stopped)
	{
		_deltaTime = 0.0;
		return;
	}

	_current = Clock::now();
	_deltaTime = std::chrono::duration<double>(_current - _previous).count();

	_previous = _current;

	if (_deltaTime < 0.0)
	{
		_deltaTime = 0.0;
	}
}

float Timer::TotalTime() const
{
	const Clock::time_point end = _stopped ? _stop : _current;

	return static_cast<float>(std::chrono::duration<double>(end - _paused - _base).count());
}
