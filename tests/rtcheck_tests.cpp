// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/rt/realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <vector>

// gnubase-tests links the RT checker (gnubase-rtcheck), which reports allocations made in real-time code.
using namespace gnubase::rt;

namespace
{
	std::atomic<int> g_violations{0};

	// Optimizers may remove allocations whose memory is never used; publishing the pointer keeps them.
	void* volatile g_keepAlive = nullptr;

	void countViolation(const char* /*what*/) noexcept
	{
		++g_violations;
	}

	// Counts violations instead of aborting, for the lifetime of the object.
	class RecordViolations
	{
	public:
		RecordViolations() : m_previous(setViolationHandler(&countViolation)) { g_violations = 0; }
		~RecordViolations() { setViolationHandler(m_previous); }
		RecordViolations(const RecordViolations&) = delete;
		RecordViolations& operator=(const RecordViolations&) = delete;

	private:
		ViolationHandler m_previous;
	};
}

TEST_CASE("the RT checker reports an allocation in real-time code", "[unit]")
{
	const RecordViolations record;
	std::vector<int> values;
	{
		const ScopedRealtimeContext realtime;
		values.reserve(16);
		g_keepAlive = values.data();
	}
	CHECK(g_violations == 1);
}

TEST_CASE("the RT checker reports a deallocation in real-time code", "[unit]")
{
	const RecordViolations record;
	auto value = std::make_unique<int>(42);
	g_keepAlive = value.get();
	{
		const ScopedRealtimeContext realtime;
		value.reset();
	}
	CHECK(g_violations == 1);
}

TEST_CASE("allocations outside real-time code are fine", "[unit]")
{
	const RecordViolations record;
	const auto value = std::make_unique<int>(42);
	CHECK(*value == 42);
	CHECK(g_violations == 0);
}

TEST_CASE("assertNotRealtime reports only inside real-time code", "[unit]")
{
	const RecordViolations record;
	assertNotRealtime("blocking call");
	CHECK(g_violations == 0);
	{
		const ScopedRealtimeContext realtime;
		assertNotRealtime("blocking call");
	}
	CHECK(g_violations == 1);
}

TEST_CASE("real-time contexts nest", "[unit]")
{
	const RecordViolations record;
	std::vector<int> values;
	{
		const ScopedRealtimeContext outer;
		{
			const ScopedRealtimeContext inner;
		}
		CHECK(isRealtimeContext());
		values.reserve(16);
		g_keepAlive = values.data();
	}
	CHECK_FALSE(isRealtimeContext());
	CHECK(g_violations == 1);
}
