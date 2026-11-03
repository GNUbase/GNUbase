// SPDX-License-Identifier: GPL-3.0-or-later
// The clock that the engine's timed work goes by (engine/clock.h), moved forward the way tests of due auto saves move
// it (#486).
#include "engine/clock.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>

using gnubase::engine::Clock;
using namespace std::chrono_literals;

TEST_CASE("the clock ticks once for every advance past the period, before advance returns", "[unit]")
{
	Clock clock;
	std::atomic<int> ticks{0};
	// A period far longer than a slow test machine takes between two steps.
	clock.start(1h, [&ticks] { ++ticks; });

	clock.advance(50min);
	CHECK(ticks == 0);
	clock.advance(10min);
	CHECK(ticks == 1);
	clock.advance(50min);
	CHECK(ticks == 1); // the next period started with the tick
	clock.advance(5h);
	CHECK(ticks == 2); // one tick however many periods passed, as after a long sleep of the computer

	clock.stop();
	clock.advance(5h); // nothing waits anymore
	CHECK(ticks == 2);
}

TEST_CASE("the clock runs with steady_clock, ahead by what advance moved it", "[unit]")
{
	Clock clock; // not started: advance only moves the time
	const auto before = clock.now();
	clock.advance(15min);
	CHECK(clock.now() - before >= 15min);
	CHECK(clock.now() - std::chrono::steady_clock::now() >= 15min - 1s);
}
