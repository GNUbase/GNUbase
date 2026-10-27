// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/log.h"
#include "platform/rt/realtime.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;

namespace
{
	enum class Color : uint8_t
	{
		Red,
		Green,
	};

	struct Failure
	{
		std::string condition;
		std::string file;
		int line = 0;
		int count = 0;
	};

	Failure g_failure;

	void recordFailure(const char* condition, const char* file, int line)
	{
		g_failure = {condition, file, line, g_failure.count + 1};
	}

	// Records failed assertions instead of logging them, for the lifetime of the object.
	class RecordFailures
	{
	public:
		RecordFailures() : m_previous(logging::setAssertionHandler(&recordFailure)) { g_failure = {}; }
		~RecordFailures() { logging::setAssertionHandler(m_previous); }
		RecordFailures(const RecordFailures&) = delete;
		RecordFailures& operator=(const RecordFailures&) = delete;

	private:
		logging::AssertionHandler m_previous;
	};
}

TEST_CASE("log messages fill placeholders with their arguments", "[unit]")
{
	const test::TempDir dir;
	const auto file = dir.path() / "test.log";
	{
		const logging::Session session(file);
		logging::info("{} + {} = {}", 1, 2.5, "three");
		logging::warning("{} {} {} {}", -7, std::numeric_limits<uint64_t>::max(), true, Color::Green);
		logging::error("{} and {}", std::string("text"), std::string_view("view"));
		logging::debug("{} stays without an argument");
	}
	const std::string log = test::readText(file);
	CHECK_THAT(log, ContainsSubstring("info    1 + 2.5 = three"));
	CHECK_THAT(log, ContainsSubstring("warning -7 18446744073709551615 true 1"));
	CHECK_THAT(log, ContainsSubstring("error   text and view"));
	CHECK_THAT(log, ContainsSubstring("debug   {} stays without an argument"));
}

TEST_CASE("audio threads queue log messages for the writer thread", "[unit]")
{
	const test::TempDir dir;
	const auto file = dir.path() / "test.log";
	const logging::Session session(file);
	std::string plugin = "Delay";
	{
		const rt::ScopedRealtimeContext realtime;
		logging::warning("{} missed {} deadlines", plugin, 3);
	}
	plugin = "overwritten"; // the queued message holds its own copy of the text
	logging::flush();
	CHECK_THAT(test::readText(file), ContainsSubstring("Delay missed 3 deadlines"));
}

TEST_CASE("a full log queue drops messages and reports how many", "[unit]")
{
	const test::TempDir dir;
	const auto file = dir.path() / "test.log";
	const logging::Session session(file);
	constexpr int count = 5000; // far more than the queue holds between two writer passes
	{
		const rt::ScopedRealtimeContext realtime;
		for (int i = 0; i < count; ++i)
			logging::info("message {}", i);
	}
	logging::flush();

	int written = 0;
	int dropped = 0;
	std::istringstream lines(test::readText(file));
	for (std::string line; std::getline(lines, line);)
	{
		if (line.find(" message ") != std::string::npos)
			++written;
		if (const size_t at = line.find("Dropped "); at != std::string::npos)
			dropped += std::stoi(line.substr(at + 8));
	}
	CHECK(dropped > 0);
	CHECK(written + dropped == count);
}

TEST_CASE("a new log session keeps the previous log", "[unit]")
{
	const test::TempDir dir;
	const auto file = dir.path() / "test.log";
	{
		const logging::Session session(file);
		logging::info("first run");
		CHECK_THROWS_AS(logging::Session(dir.path() / "other.log"), std::logic_error); // one per process
	}
	{
		const logging::Session session(file);
		logging::info("second run");
	}
	CHECK_THAT(test::readText(dir.path() / "test.previous.log"), ContainsSubstring("first run"));
	const std::string current = test::readText(file);
	CHECK_THAT(current, ContainsSubstring("second run"));
	CHECK_THAT(current, !ContainsSubstring("first run"));
}

TEST_CASE("failed assertions go to the assertion handler", "[unit]")
{
	const RecordFailures failures;
	const int value = 2;
	GNUBASE_ASSERT(value == 2);
	CHECK(g_failure.count == 0);

	GNUBASE_ASSERT(value == 3);
	CHECK(g_failure.count == 1);
	CHECK(g_failure.condition == "value == 3");
	CHECK_THAT(g_failure.file, ContainsSubstring("log_tests.cpp"));
	CHECK(g_failure.line > 0);

	GNUBASE_ASSERT_REALTIME(); // not on an audio thread
	CHECK(g_failure.count == 2);
	{
		const rt::ScopedRealtimeContext realtime;
		GNUBASE_ASSERT_REALTIME();
	}
	CHECK(g_failure.count == 2);
}
