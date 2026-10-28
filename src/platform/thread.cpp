// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/thread.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace gnubase::platform
{
	bool setRealtimePriority() noexcept
	{
#ifdef _WIN32
		return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != FALSE;
#else
		// Below the maximum, so watchdogs and the audio device's own threads can still preempt it.
		sched_param parameters{};
		parameters.sched_priority = sched_get_priority_max(SCHED_FIFO) - 10;
		if (parameters.sched_priority < sched_get_priority_min(SCHED_FIFO))
			parameters.sched_priority = sched_get_priority_min(SCHED_FIFO);
		return pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters) == 0;
#endif
	}
}
