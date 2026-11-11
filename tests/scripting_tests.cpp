// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "scripting/runtime.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
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

TEST_CASE("an empty Lua table is an empty list where a command takes a list", "[unit]")
{
	Engine engine;
	const Json list = {
		{"type", "array"}, {"items", {{"type", "object"}, {"properties", {{"tags", {{"type", "array"}}}}}}}};
	Json seen;
	engine.registerCommand({"fake.lists", 1, "Takes lists.",
							   {{"type", "object"}, {"properties", {{"items", list}, {"other", {{"type", "object"}}}}}},
							   Json::object(), true},
		[&seen](CommandContext& context)
		{
			seen = context.parameters();
			return Json::object();
		});
	std::ostringstream output;
	Runtime lua(engine, Trust::Trusted, output);
	run(lua, "gnubase.fake.lists{items = {{tags = {}}, {}}, other = {}}");
	CHECK(seen == Json{{"items", {{{"tags", Json::array()}}, Json::object()}}, {"other", Json::object()}});
	run(lua, "gnubase.fake.lists{items = {}}");
	CHECK(seen == Json{{"items", Json::array()}});
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

	// gnubase.off unsubscribes a handler, also from inside one, while the others still get the event.
	run(lua, R"(
		local seen = {}
		local first = function(event) seen[#seen + 1] = "first" end
		local once
		once = function(event)
			seen[#seen + 1] = "once"
			gnubase.off("fake.changed", once)
		end
		local last = function(event) seen[#seen + 1] = "last" end
		gnubase.on("fake.changed", first)
		gnubase.on("fake.changed", once)
		gnubase.on("fake.changed", last)
		gnubase.fake.emit{value = 1}
		assert(table.concat(seen, ",") == "first,once,last", table.concat(seen, ","))
		gnubase.off("fake.changed", first)
		gnubase.off("fake.changed", first) -- once more does nothing
		gnubase.off("other.changed", last) -- nor for an event it isn't registered for
		gnubase.fake.emit{value = 2}
		assert(table.concat(seen, ",") == "first,once,last,last", table.concat(seen, ","))
		gnubase.events()
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

TEST_CASE("a time limit asks whether a late script keeps running, and ends with the run", "[unit]")
{
	Engine engine;
	std::ostringstream output;
	Runtime lua(engine, Trust::Trusted, output);
	int asked = 0;
	lua.setTimeLimit(std::chrono::milliseconds(20), [&asked] { return ++asked < 3; });
	CHECK_THAT(lua.run("while true do end", "loop.lua").value_or(""), ContainsSubstring("ran longer than"));
	CHECK(asked == 3);
	CHECK(!lua.running());

	// Without a limit, code that runs later isn't stopped, also in a coroutine that the limited run made.
	run(lua, "late = coroutine.wrap(function() local t = os.clock() + 0.1 while os.clock() < t do end end)");
	lua.setTimeLimit(std::chrono::milliseconds(0));
	run(lua, "local t = os.clock() + 0.1 while os.clock() < t do end late()");
	CHECK(asked == 3);
}

TEST_CASE("a late script that was stopped can't catch the error and go on", "[unit]")
{
	Engine engine;
	std::ostringstream output;
	Runtime lua(engine, Trust::Untrusted, output);
	int asked = 0;
	lua.setTimeLimit(std::chrono::milliseconds(20),
		[&asked]
		{
			++asked;
			return false;
		});
	// The error comes inside the pcall, which catches it: it comes again right after, without asking.
	CHECK_THAT(lua.run("while true do pcall(function() while true do end end) end", "catches.lua").value_or(""),
		ContainsSubstring("ran longer than"));
	CHECK(asked == 1);
	// Also when the pcall loop runs in a coroutine other than the one that was late.
	asked = 0;
	CHECK_THAT(lua.run(R"(coroutine.wrap(function()
		local late = coroutine.create(function() while true do end end)
		while true do pcall(function() coroutine.resume(late) while true do end end) end
	end)())",
					  "resumes.lua")
				   .value_or(""),
		ContainsSubstring("ran longer than"));
	CHECK(asked == 1);
	run(lua, "local caught = pcall(function() end) assert(caught)"); // the next run starts afresh
	CHECK(asked == 1);
}

TEST_CASE("code runs in an environment of its own that reads the globals it doesn't have", "[unit]")
{
	Engine engine;
	std::ostringstream output;
	Runtime lua(engine, Trust::Trusted, output);
	run(lua, "shared = 1");
	const int environment = lua.newEnvironment();
	const auto inside = [&lua, environment](const std::string& code)
	{
		const std::optional<std::string> error = lua.run(code, "inside", environment);
		INFO(error.value_or(""));
		CHECK(!error.has_value());
	};
	inside("shared = 2; own = 3; assert(gnubase.engine.version())");
	inside("assert(shared == 2 and own == 3 and _G.shared == 1)"); // kept from run to run
	run(lua, "assert(shared == 1 and own == nil)");
	CHECK(lua.newEnvironment() != environment);
}

TEST_CASE("Lua calls functions that C++ adds next to the commands, and takes events when asked", "[unit]")
{
	Engine engine;
	registerTestCommands(engine);
	std::ostringstream output;
	Runtime lua(engine, Trust::Trusted, output);
	lua.addFunction(
		"fake.twice", [](const Json& parameters) { return Json{{"value", 2 * parameters.at("value").get<int>()}}; });
	lua.addFunction("fake.picture",
		[](const Json& /*parameters*/) { return Json::binary(std::vector<uint8_t>{0x89, 'P', 'N', 'G', 0}); });
	lua.addFunction(
		"fake.broken", [](const Json& /*parameters*/) -> Json { throw std::invalid_argument("no such element"); });
	run(lua, R"(
		assert(gnubase.fake.twice{value = 21}.value == 42)
		assert(gnubase.fake.echo{value = 1}.value == 1) -- the area's commands stay
		assert(gnubase.fake.picture() == "\x89PNG\0") -- binary results, such as screenshots, are strings
		local ok, message = pcall(gnubase.fake.broken)
		assert(not ok and message:find("no such element", 1, true), message)
	)");
	CHECK_THROWS_AS(lua.addFunction("fake.echo", [](const Json& parameters) { return parameters; }), std::logic_error);

	// Code that doesn't run, such as the UI's handlers, gets the events that arrived since when asked.
	run(lua, R"(
		seen = {}
		gnubase.on("fake.changed", function(event) seen[#seen + 1] = event.data.value end)
	)");
	REQUIRE(gnubase::engine::Client(engine).call("fake.emit", {{"value", 7}}).ok());
	REQUIRE(gnubase::engine::Client(engine).call("fake.emit", {{"value", 8}}).ok());
	bool handled = false;
	CHECK_FALSE(lua.dispatchEvents(&handled).has_value());
	CHECK(handled); // so the UI draws what the handlers changed
	run(lua, "assert(#seen == 2 and seen[1] == 7 and seen[2] == 8)");
	CHECK_FALSE(lua.dispatchEvents(&handled).has_value());
	CHECK(!handled); // nothing arrived
	run(lua, "gnubase.on('fake.changed', function() error('broken handler') end)");
	REQUIRE(gnubase::engine::Client(engine).call("fake.emit", {{"value", 9}}).ok());
	CHECK_THAT(lua.dispatchEvents(&handled).value_or(""), ContainsSubstring("broken handler"));
	CHECK(handled);
}

TEST_CASE("a run inside a run leaves the test cases to the outermost one", "[unit]")
{
	// As when a Lua UI test runs a user script, which the UI runs in the same runtime (#209).
	Engine engine;
	std::ostringstream output;
	Runtime lua(engine, Trust::Trusted, output);
	lua.addFunction("fake.run",
		[&lua](const Json& parameters)
		{
			const std::optional<std::string> error = lua.run(parameters.at("code").get<std::string>(), "inner");
			return Json{{"error", error.value_or("")}};
		});
	run(lua, R"lua(
		local test = gnubase.test
		test.case("runs code inside a test case", function()
			test.equal(gnubase.fake.run{code = "inner = 1"}.error, "")
			test.equal(inner, 1)
			test.ok(gnubase.fake.run{code = "error('inside')"}.error:find("inside", 1, true))
		end)
		assert(gnubase.fake.run{code = "outer = 1"}.error == "")
	)lua");
	CHECK_THAT(output.str(), ContainsSubstring("1..1\nok 1 - runs code inside a test case"));
}
