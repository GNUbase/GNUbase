// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/log.h"
#include "platform/rt/realtime.h"
#include "platform/rt/snapshotpublisher.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <memory>
#include <string_view>
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

TEST_CASE("logging from real-time code passes the RT checker", "[unit]")
{
	const gnubase::test::TempDir dir;
	const gnubase::logging::Session session(dir.path() / "test.log");
	{
		const RecordViolations violations;
		{
			const ScopedRealtimeContext realtime;
			gnubase::logging::info("block {} took {} ms in {} ({})", 42, 0.75, "Delay", "true");
		}
		CHECK(g_violations == 0);
	}
	gnubase::logging::flush();
	CHECK_THAT(gnubase::test::readText(dir.path() / "test.log"),
		Catch::Matchers::ContainsSubstring("block 42 took 0.75 ms in Delay (true)"));
}

TEST_CASE("SnapshotPublisher readers never allocate or free", "[unit]")
{
	SnapshotPublisher<std::vector<int>> publisher(std::make_unique<std::vector<int>>(64, 1));
	const RecordViolations record;
	publisher.publish(std::make_unique<std::vector<int>>(64, 2));
	int seen = 0;
	{
		const ScopedRealtimeContext realtime;
		seen = publisher.acquire().back();
		publisher.release();
	}
	publisher.publish(std::make_unique<std::vector<int>>(64, 3)); // frees the others outside real-time code
	CHECK(seen == 2);
	CHECK(g_violations == 0);
}
