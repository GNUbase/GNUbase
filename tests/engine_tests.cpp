// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("engine reports the project version", "[unit]")
{
	REQUIRE(std::string(gnubase::engine::version()) == GNUBASE_VERSION);
}
