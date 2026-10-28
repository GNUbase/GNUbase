// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "scripting/runtime.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <sstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using gnubase::engine::CommandContext;
using gnubase::engine::Engine;
using gnubase::engine::Json;
using gnubase::scripting::Runtime;
using gnubase::scripting::Trust;

namespace
{
	// fake.echo returns its parameters; fake.emit announces fake.changed with its parameters.
	void registerTestCommands(Engine& engine)
	{
		const Json anything = Json{{"type", "object"}};
		engine.registerCommand({"fake.echo", 1, "Returns its parameters.", anything, anything, true},
			[](CommandContext& context) { return context.parameters(); });
		engine.registerCommand({"fake.emit", 1, "Announces fake.changed.", anything, Json::object(), false},
			[](CommandContext& context)
			{
				context.emit("fake.changed", context.parameters());
				return Json::object();
			});
	}

	// Runs code and fails the test with Lua's message if it raises an error.
	void run(Runtime& lua, const std::string& code)
	{
		const std::optional<std::string> error = lua.run(code, "test");
		INFO(error.value_or(""));
		CHECK(!error.has_value());
	}
}

TEST_CASE("Lua calls engine commands through functions generated from the registry", "[unit]")
{
	Engine engine;
	std::ostringstream output;
	Runtime lua(engine, Trust::Trusted, output);
	run(lua, "print(gnubase.engine.version().version)");
	CHECK(output.str() == std::string(GNUBASE_VERSION) + "\n");

	// Every command has its function, and the registry's documentation is one call away.
	run(lua, R"(
		local count = 0
		for _, command in ipairs(gnubase.engine.commands().commands) do
			local area, name = command.name:match("^(%w+)%.(%w+)$")
			assert(type(gnubase[area][name]) == "function", command.name)
			count = count + 1
		end
		assert(count > 10)
	)");
}

TEST_CASE("a failed command raises a Lua error with the reason", "[unit]")
{
	Engine engine;
	std::ostringstream output;
	Runtime lua(engine, Trust::Trusted, output);
	const std::optional<std::string> error = lua.run("gnubase.track.add{type = 'nonsense'}", "add.lua");
	REQUIRE(error.has_value());
	CHECK_THAT(error.value_or(""), ContainsSubstring("parameters.type"));
	CHECK_THAT(error.value_or(""), ContainsSubstring("add.lua:1"));

	run(lua, R"(
		local ok, message = pcall(gnubase.track.add, {type = "nonsense"})
		assert(not ok and message:find("parameters.type", 1, true), message)
	)");
	CHECK_THAT(lua.run("error('stop here')", "stop.lua").value_or(""), ContainsSubstring("stop.lua:1: stop here"));
	CHECK_THAT(lua.run("this is not Lua", "syntax.lua").value_or(""), ContainsSubstring("syntax.lua:1"));
}

TEST_CASE("values travel between Lua tables and JSON", "[unit]")
{
	Engine engine;
	registerTestCommands(engine);
	std::ostringstream output;
	Runtime lua(engine, Trust::Trusted, output);
	run(lua, R"(
		local result = gnubase.fake.echo{
			integer = 42, number = 0.25, text = "hello", yes = true, no = false,
			list = {1, 2, 3}, nested = {inner = {deeper = "x"}}, empty = {}
		}
		assert(math.type(result.integer) == "integer" and result.integer == 42)
		assert(math.type(result.number) == "float" and result.number == 0.25)
		assert(result.text == "hello" and result.yes == true and result.no == false)
		assert(#result.list == 3 and result.list[3] == 3)
		assert(result.nested.inner.deeper == "x")
		assert(next(result.empty) == nil)
	)");

	const std::optional<std::string> cycle = lua.run("local t = {}; t.self = t; gnubase.fake.echo(t)", "cycle.lua");
	CHECK_THAT(cycle.value_or(""), ContainsSubstring("too deep"));
	CHECK_THAT(
		lua.run("gnubase.fake.echo{f = print}", "function.lua").value_or(""), ContainsSubstring("can't be sent"));
}

TEST_CASE("Lua handles engine events and waits for them", "[unit]")
{
	Engine engine;
	registerTestCommands(engine);
	std::ostringstream output;
	Runtime lua(engine, Trust::Trusted, output);
	run(lua, R"(
		local seen = {}
		gnubase.on("fake.changed", function(event) seen[#seen + 1] = event.data.value end)
		gnubase.fake.emit{value = 1}
		gnubase.fake.emit{value = 2}
		assert(#seen == 2 and seen[1] == 1 and seen[2] == 2)

		-- The same events wait to be taken.
		local event = gnubase.waitFor("fake.changed", 0)
		assert(event.name == "fake.changed" and event.data.value == 1)
		assert(#gnubase.events() == 1) -- the second one
		assert(gnubase.waitFor("fake.changed", 0.01) == nil)
	)");

	// Coroutines can call commands as well.
	run(lua, R"(
		local version = coroutine.wrap(function() return gnubase.engine.version().version end)
		assert(version() == gnubase.engine.version().version)
	)");
}

TEST_CASE("untrusted scripts have no access to files, processes or bytecode", "[unit]")
{
	Engine engine;
	std::ostringstream output;
	Runtime lua(engine, Trust::Untrusted, output);
	run(lua, R"(
		assert(io == nil and os == nil and debug == nil and package == nil and require == nil)
		assert(dofile == nil and loadfile == nil)
		assert(load("return 1 + 1")() == 2)
		assert(load(string.dump(function() end)) == nil) -- no binary chunks
		assert(gnubase.engine.version().version)
	)");

	Runtime trusted(engine, Trust::Trusted, output);
	run(trusted, "assert(io and os and os.exit == nil)"); // os.exit would skip the engine's shutdown
}

TEST_CASE("a time limit stops scripts that run too long", "[unit]")
{
	Engine engine;
	std::ostringstream output;
	Runtime lua(engine, Trust::Untrusted, output);
	lua.setTimeLimit(std::chrono::milliseconds(100));
	CHECK_THAT(lua.run("while true do end", "loop.lua").value_or(""), ContainsSubstring("ran longer than"));
	CHECK_THAT(lua.run("coroutine.wrap(function() while true do end end)()", "coroutine.lua").value_or(""),
		ContainsSubstring("ran longer than"));
	run(lua, "assert(gnubase.engine.version())"); // each run gets the full time again
}
