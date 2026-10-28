// SPDX-License-Identifier: GPL-3.0-or-later
// gnubase.test, the Lua test framework (#206): test cases, fixtures, failure reports and golden files. Its assertions
// are tested in Lua, in tests/lua/framework.lua.
#include "engine/engine.h"
#include "scripting/runtime.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using gnubase::engine::CommandContext;
using gnubase::engine::Engine;
using gnubase::engine::Json;
using gnubase::scripting::Runtime;
using gnubase::scripting::Trust;

namespace
{
	// A trusted runtime that names its scripts check.lua.
	struct Lua
	{
		Engine engine;
		std::ostringstream output;
		Runtime runtime{engine, Trust::Trusted, output};

		// Runs a script; returns its error, or an empty string.
		std::string run(const std::string& code) { return runtime.run(code, "check.lua").value_or(""); }
	};

	// A Lua string literal with a path.
	std::string quoted(const std::filesystem::path& path)
	{
		return "[==[" + path.u8string() + "]==]";
	}
}

TEST_CASE("Lua test cases run when the script ends and report in TAP format", "[unit]")
{
	Lua lua;
	CHECK(lua.run(R"(local test = gnubase.test
test.case("adds", function() test.equal(1 + 1, 2) end)
test.case("knows its version", function() test.ok(gnubase.engine.version().version) end)
print("registered"))") == "");
	CHECK(lua.output.str() == "registered\n1..2\nok 1 - adds\nok 2 - knows its version\n");

	// A script without test cases runs as before, and every script starts without test cases.
	lua.output.str("");
	CHECK(lua.run("print('no tests')") == "");
	CHECK(lua.run("gnubase.test.case('again', function() end)") == "");
	CHECK(lua.output.str() == "no tests\n1..1\nok 1 - again\n");

	// A script that fails before its end runs none of its test cases.
	lua.output.str("");
	CHECK_THAT(lua.run("gnubase.test.case('dropped', function() end) error('stop')"), ContainsSubstring("stop"));
	CHECK(lua.run("gnubase.test.case('kept', function() end)") == "");
	CHECK(lua.output.str() == "1..1\nok 1 - kept\n");
}

TEST_CASE("a failing Lua test case fails the script, and the report names the file and line", "[unit]")
{
	Lua lua;
	const std::string error = lua.run(R"(local test = gnubase.test
test.case("passes", function() end)
test.case("compares", function()
	test.equal(1 + 1, 3)
end)
test.case("adds a track", function()
	gnubase.track.add{type = "nonsense"}
end)
test.case("asserts", function() assert(false, "plain assert") end)
test.case("calls a helper", function()
	local function helper() error("deep down") end
	helper()
end))");
	CHECK(error == "4 of 5 test cases failed: compares, adds a track, asserts, calls a helper");
	const std::string output = lua.output.str();
	CHECK_THAT(
		output, ContainsSubstring("1..5\nok 1 - passes\nnot ok 2 - compares\n# check.lua:4: expected 3, got 2\n"));
	// Errors without a position, like those of commands and assert, get the line in the test case.
	CHECK_THAT(output, ContainsSubstring("not ok 3 - adds a track\n# check.lua:7: "));
	CHECK_THAT(output, ContainsSubstring("parameters.type"));
	CHECK_THAT(output, ContainsSubstring("not ok 4 - asserts\n# check.lua:9: plain assert\n"));
	CHECK_THAT(output, ContainsSubstring("not ok 5 - calls a helper\n# check.lua:11: deep down\n"));

	// Two test cases with the same name are a mistake in the script.
	CHECK_THAT(lua.run("gnubase.test.case('twice', print)\ngnubase.test.case('twice', print)"),
		ContainsSubstring("check.lua:2: there already is a test case named \"twice\""));
}

TEST_CASE("Lua fixtures, before and after hooks and cleanups run around every test case", "[unit]")
{
	Lua lua;
	const std::string error = lua.run(R"(local test = gnubase.test
log = {}
local function note(text) return function() log[#log + 1] = text end end
test.before(note("before"))
test.after(note("after"))
local count = 0
local counter = test.fixture(function()
	count = count + 1
	log[#log + 1] = "setup " .. count
	return count, note("teardown " .. count)
end)
test.case("uses a fixture twice", function()
	test.equal(counter(), counter()) -- set up once per test case
	test.cleanup(note("cleanup"))
end)
test.case("fails after setting up", function()
	counter()
	error("broken")
end)
test.case("doesn't use the fixture", function() end))");
	CHECK(error == "1 of 3 test cases failed: fails after setting up");
	CHECK(lua.run("print(table.concat(log, ', '))") == "");
	CHECK_THAT(lua.output.str(),
		ContainsSubstring("before, setup 1, cleanup, teardown 1, after, "
						  "before, setup 2, teardown 2, after, before, after\n"));

	// A failing before hook skips the test case; after hooks still run.
	Lua hooks;
	CHECK(hooks.run(R"(local test = gnubase.test
ran, cleaned = false, false
test.before(function() error("no setup") end)
test.after(function() cleaned = true end)
test.case("never runs", function() ran = true end))") == "1 of 1 test cases failed: never runs");
	CHECK(hooks.run("assert(not ran and cleaned)") == "");
	CHECK_THAT(hooks.output.str(), ContainsSubstring("not ok 1 - never runs\n# check.lua:3: no setup\n"));
}

TEST_CASE("Lua tests compare audio with golden files", "[unit]")
{
	const gnubase::test::TempDir golden;
	const gnubase::test::TempDir output;
	Lua lua;
	REQUIRE(lua.run("dirs = {goldenDir = " + quoted(golden.path()) + ", outputDir = " + quoted(output.path()) +
				", update = false}") == "");

	// Update mode writes the golden file, in the format of the C++ helpers.
	CHECK(lua.run(R"(gnubase.test.case("writes", function()
	local options = {goldenDir = dirs.goldenDir, outputDir = dirs.outputDir, update = true}
	gnubase.test.golden("ramp", {samples = {0, 0.25, 0.5, 0.75}, channels = 2, sampleRate = 44100}, 0, options)
end))") == "");
	const gnubase::test::Audio written = gnubase::test::readWav(golden.path() / "ramp.wav");
	CHECK(written.samples == std::vector<float>{0.0f, 0.25f, 0.5f, 0.75f});
	CHECK(written.channels == 2);
	CHECK(written.sampleRate == 44100);

	// Within the tolerance it passes. A difference fails and leaves the actual audio and the difference.
	CHECK(lua.run(R"(gnubase.test.case("matches", function()
	gnubase.test.golden("ramp", {samples = {0, 0.25, 0.5, 0.7501}, channels = 2, sampleRate = 44100}, 0.001, dirs)
end))") == "");
	CHECK(lua.run(R"(gnubase.test.case("differs", function()
	gnubase.test.golden("ramp", {samples = {0, 0.25, 1, 0.75}, channels = 2, sampleRate = 44100}, 0.001, dirs)
end))") == "1 of 1 test cases failed: differs");
	CHECK_THAT(lua.output.str(),
		ContainsSubstring("# check.lua:2: ramp: 1 of 4 samples differ by more than 0.001. "
						  "First: sample 2 (frame 1, channel 0, "));
	CHECK_THAT(lua.output.str(), ContainsSubstring("GNUBASE_UPDATE_GOLDEN=1"));
	CHECK(gnubase::test::readWav(output.path() / "ramp.actual.wav").samples ==
		std::vector<float>{0.0f, 0.25f, 1.0f, 0.75f});
	CHECK(
		gnubase::test::readWav(output.path() / "ramp.diff.wav").samples == std::vector<float>{0.0f, 0.0f, 0.5f, 0.0f});

	// Another format, or no golden file at all, fails too.
	lua.output.str("");
	CHECK(lua.run(R"(gnubase.test.case("mono", function()
	gnubase.test.golden("ramp", {samples = {0, 0.25, 0.5, 0.75}}, 0.001, dirs)
end))") == "1 of 1 test cases failed: mono");
	CHECK_THAT(lua.output.str(), ContainsSubstring("the golden file has 2 channels at 44100 Hz"));
	CHECK(lua.run(R"(gnubase.test.case("missing", function()
	gnubase.test.golden("missing", {samples = {0}}, 0, dirs)
end))") == "1 of 1 test cases failed: missing");
	CHECK_THAT(lua.output.str(), ContainsSubstring("missing.wav doesn't exist"));
	CHECK(gnubase::test::readWav(output.path() / "missing.actual.wav").samples == std::vector<float>{0.0f});

	// Lua reads what the C++ helpers write.
	gnubase::test::writeWav(golden.path() / "cpp.wav", {{0.5f, -0.5f}, 1, 48000});
	CHECK(lua.run(R"(local audio = gnubase.test.readWav(dirs.goldenDir .. "/cpp.wav")
assert(audio.channels == 1 and audio.sampleRate == 48000)
assert(#audio.samples == 2 and audio.samples[1] == 0.5 and audio.samples[2] == -0.5))") == "");
}

TEST_CASE("untrusted Lua scripts run test cases, but golden files need a trusted script", "[unit]")
{
	Engine engine;
	std::ostringstream output;
	Runtime lua(engine, Trust::Untrusted, output);
	const std::string code = "gnubase.test.case('golden', function() gnubase.test.golden('x', {samples = {0}}, 0) end)";
	CHECK(lua.run(code, "agent.lua").value_or("") == "1 of 1 test cases failed: golden");
	CHECK_THAT(output.str(), ContainsSubstring("# agent.lua:1: golden files need a trusted script"));
}

TEST_CASE("no command area may be called test, which is the name of the Lua test framework", "[unit]")
{
	Engine engine;
	const Json anything = Json{{"type", "object"}};
	engine.registerCommand({"test.echo", 1, "Returns its parameters.", anything, anything, true},
		[](CommandContext& context) { return context.parameters(); });
	std::ostringstream output;
	CHECK_THROWS_WITH(Runtime(engine, Trust::Trusted, output), ContainsSubstring("gnubase.test"));
}
