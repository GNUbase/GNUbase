// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "engine/schema.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	constexpr std::chrono::seconds g_timeout{10};

	Json valueParameters()
	{
		return Json::parse(R"({
			"type": "object",
			"properties": {"value": {"type": "integer"}},
			"required": ["value"],
			"additionalProperties": false
		})");
	}

	// test.setValue stores a value and announces it with a test.changed event.
	void registerSetValue(Engine& engine, int& stored)
	{
		engine.registerCommand({"test.setValue", 1, "Stores a value.", valueParameters(), Json::object(), false},
			[&stored](CommandContext& context)
			{
				stored = context.parameters().at("value").get<int>();
				context.emit("test.changed", {{"value", stored}});
				return Json::object();
			});
	}

	// The message as a T; fails the test if there is none or it is something else.
	template <typename T> T expect(const std::optional<Message>& message)
	{
		const T* value = message ? std::get_if<T>(&*message) : nullptr;
		REQUIRE(value != nullptr);
		return value != nullptr ? *value : T{};
	}
}

TEST_CASE("engine reports the project version", "[unit]")
{
	REQUIRE(std::string(version()) == GNUBASE_VERSION);
	Engine engine;
	Client client(engine);
	const Result result = client.call("engine.version");
	REQUIRE(result.ok());
	CHECK(result.value.at("version").get<std::string>() == GNUBASE_VERSION);
}

TEST_CASE("commands run on the engine's command thread", "[unit]")
{
	Engine engine;
	std::thread::id ranOn;
	engine.registerCommand({"test.where", 1, "Notes the thread.", Json::object(), Json::object(), true},
		[&ranOn](CommandContext&)
		{
			ranOn = std::this_thread::get_id();
			return Json::object();
		});
	Client client(engine);
	const RequestId id = client.send("test.where");
	const auto result = expect<Result>(client.wait(g_timeout));
	CHECK(result.request == id);
	CHECK(result.ok());
	CHECK(ranOn != std::this_thread::get_id());
}

TEST_CASE("every client receives events, the sender gets the result after them", "[unit]")
{
	Engine engine;
	int stored = 0;
	registerSetValue(engine, stored);
	Client sender(engine);
	Client listener(engine);
	const RequestId id = sender.send("test.setValue", {{"value", 5}});

	const auto event = expect<Event>(sender.wait(g_timeout));
	CHECK(event.name == "test.changed");
	CHECK(event.data.at("value") == 5);
	CHECK(event.request == id);
	CHECK(expect<Result>(sender.wait(g_timeout)).request == id);
	CHECK(stored == 5);

	CHECK(expect<Event>(listener.wait(g_timeout)).request == id);
	CHECK_FALSE(listener.poll().has_value()); // results go to the sender only
}

TEST_CASE("failed commands report why and change nothing", "[unit]")
{
	Engine engine;
	int stored = 0;
	registerSetValue(engine, stored);
	engine.registerCommand({"test.fail", 1, "Always fails.", Json::object(), Json::object(), false},
		[](CommandContext& context) -> Json
		{
			context.emit("test.changed");
			throw CommandError("not today");
		});
	engine.registerCommand({"test.badResult", 1, "Breaks its result schema.", Json::object(),
							   Json{{"type", "object"}, {"required", {"value"}}}, true},
		[](CommandContext&) { return Json::object(); });
	Client client(engine);

	CHECK_THAT(client.call("test.nothing").error, ContainsSubstring("unknown command test.nothing"));
	CHECK_THAT(client.call("test.setValue", {{"value", "five"}}).error, ContainsSubstring("parameters.value must be"));
	CHECK_THAT(client.call("test.setValue").error, ContainsSubstring("parameters.value is missing"));
	CHECK_THAT(client.call("test.setValue", {{"value", 1}, {"extra", true}}).error,
		ContainsSubstring("parameters.extra is not allowed"));
	CHECK_THAT(client.call("test.fail").error, ContainsSubstring("not today"));
	CHECK_THAT(client.call("test.badResult").error, ContainsSubstring("result.value is missing"));
	CHECK_FALSE(client.poll().has_value()); // a failed command emits no events
	CHECK(stored == 0);
}

TEST_CASE("every command describes itself", "[unit]")
{
	Engine engine;
	int stored = 0;
	registerSetValue(engine, stored);
	Client client(engine);
	const Result result = client.call("engine.commands");
	REQUIRE(result.ok());
	const Json& commands = result.value.at("commands");
	const auto setValue = std::find_if(
		commands.begin(), commands.end(), [](const Json& command) { return command.at("name") == "test.setValue"; });
	REQUIRE(setValue != commands.end());
	CHECK(setValue->at("version") == 1);
	CHECK(setValue->at("description") == "Stores a value.");
	CHECK(setValue->at("parameters") == valueParameters());
	CHECK(setValue->at("query") == false);

	std::vector<std::string> names;
	for (const CommandInfo& info : engine.commands())
	{
		names.push_back(info.name);
		CHECK_FALSE(info.description.empty());
	}
	CHECK(std::is_sorted(names.begin(), names.end()));
	for (const char* name : {"engine.commands", "engine.version", "test.setValue"})
		CHECK(std::find(names.begin(), names.end(), name) != names.end());
	CHECK_THROWS_AS(registerSetValue(engine, stored), std::logic_error); // names are unique
}

TEST_CASE("commands run one at a time in the order they were sent", "[unit]")
{
	Engine engine;
	std::vector<int> seen;
	engine.registerCommand({"test.append", 1, "Appends a value.", valueParameters(), Json::object(), false},
		[&seen](CommandContext& context)
		{
			seen.push_back(context.parameters().at("value").get<int>());
			return Json::object();
		});
	Client client(engine);
	std::vector<RequestId> ids;
	ids.reserve(100);
	for (int i = 0; i < 100; ++i)
		ids.push_back(client.send("test.append", {{"value", i}}));

	REQUIRE(client.call("engine.version").ok()); // waits for its own result; the others stay queued
	for (const RequestId id : ids)
		CHECK(expect<Result>(client.poll()).request == id);
	std::vector<int> expected(100);
	std::iota(expected.begin(), expected.end(), 0);
	CHECK(seen == expected);
}

TEST_CASE("values are checked against their JSON schema", "[unit]")
{
	const Json schema = Json::parse(R"({
		"type": "object",
		"properties": {
			"name": {"type": "string"},
			"gain": {"type": "number"},
			"mode": {"enum": ["mono", "stereo"]},
			"tags": {"type": "array", "items": {"type": "string"}},
			"color": {"type": ["string", "null"]}
		},
		"required": ["name"],
		"additionalProperties": false
	})");
	CHECK(
		validate(Json::parse(R"({"name": "Bass", "gain": 0.5, "mode": "mono", "tags": ["a"], "color": null})"), schema)
			.empty());
	CHECK(validate(Json::parse(R"({"name": "Bass", "gain": 1})"), schema).empty()); // integers are numbers
	CHECK(validate(Json::parse("{}"), schema) == "parameters.name is missing");
	CHECK(validate(Json::parse(R"({"name": 1})"), schema) == "parameters.name must be string");
	CHECK(validate(Json::parse(R"({"name": "a", "color": 2})"), schema) == "parameters.color must be string or null");
	CHECK(validate(Json::parse(R"({"name": "a", "mode": "quad"})"), schema) ==
		R"(parameters.mode must be one of ["mono","stereo"])");
	CHECK(validate(Json::parse(R"({"name": "a", "tags": ["x", 2]})"), schema) == "parameters.tags[1] must be string");
	CHECK(validate(Json::parse(R"({"name": "a", "level": 3})"), schema) == "parameters.level is not allowed");
	CHECK(validate(Json::parse("[]"), schema, "result") == "result must be object");
}
