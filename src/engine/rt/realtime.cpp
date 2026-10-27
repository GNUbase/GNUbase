// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/rt/realtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace gnubase::rt
{
	namespace
	{
		thread_local int g_realtimeDepth = 0;

		void abortOnViolation(const char* what) noexcept
		{
			std::fprintf(stderr, "GNUbase real-time violation: %s\n", what);
			std::abort();
		}

		std::atomic<ViolationHandler> g_violationHandler{&abortOnViolation};
	}

	ViolationHandler setViolationHandler(ViolationHandler handler) noexcept
	{
		return g_violationHandler.exchange(handler);
	}

	bool isRealtimeContext() noexcept
	{
		return g_realtimeDepth > 0;
	}

	void reportViolation(const char* what) noexcept
	{
		// The handler runs outside the real-time context, so it may allocate without reporting itself again.
		const int depth = g_realtimeDepth;
		g_realtimeDepth = 0;
		g_violationHandler.load()(what);
		g_realtimeDepth = depth;
	}

	ScopedRealtimeContext::ScopedRealtimeContext() noexcept
	{
		++g_realtimeDepth;
	}

	ScopedRealtimeContext::~ScopedRealtimeContext()
	{
		--g_realtimeDepth;
	}
}
