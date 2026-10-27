// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace
{
	// Runs gnubase-crashtest, which opens a log session in the folder and crashes the way it is told.
	int crash(const std::filesystem::path& dir, const std::string& how)
	{
		const std::string command = "\"" + std::string(GNUBASE_CRASHTEST) + "\" \"" + dir.string() + "\" " + how;
#ifdef _WIN32
		// cmd.exe removes the outer quotes.
		return std::system(("\"" + command + "\"").c_str()); // NOLINT(bugprone-command-processor): fixed helper command
#else
		return std::system(command.c_str()); // NOLINT(bugprone-command-processor): fixed helper command
#endif
	}
}

TEST_CASE("a crash is recorded in the log", "[integration]")
{
	const std::string how = GENERATE(as<std::string>{}, "segv", "abort");
	CAPTURE(how);
	const gnubase::test::TempDir dir;
	CHECK(crash(dir.path(), how) != 0);
	const std::string log = gnubase::test::readText(dir.path() / "crash.log");
	CHECK_THAT(log, ContainsSubstring("about to crash"));
	CHECK_THAT(log, ContainsSubstring("Crash: "));
#ifdef _WIN32
	CHECK(!gnubase::test::readText(dir.path() / "crash.dmp").empty());
#endif
}
