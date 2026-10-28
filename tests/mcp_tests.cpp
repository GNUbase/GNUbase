// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "mcp/server.h"
#include "mcp/transport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <httplib.h>

#include <algorithm>
#include <sstream>
#include <string>

using gnubase::engine::Engine;
using gnubase::engine::Json;
using namespace gnubase::mcp;
using Catch::Matchers::ContainsSubstring;

namespace
{
	Json request(int id, const std::string& method, Json params = Json::object())
	{
		return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
	}

	// The response to a request, which must be there.
	Json respond(Server& server, const Json& message)
	{
		const std::optional<Json> response = server.handle(message);
		REQUIRE(response.has_value());
		return response.value_or(Json());
	}

	const Json* findTool(const Json& tools, const std::string& name)
	{
		const auto tool =
			std::find_if(tools.begin(), tools.end(), [&name](const Json& t) { return t.at("name") == name; });
		return tool != tools.end() ? &*tool : nullptr;
	}

	// test.echo returns its text; test.fail always fails.
	void registerTestCommands(Engine& engine)
	{
		engine.registerCommand(
			{"test.echo", 1, "Returns its text.",
				Json::parse(R"({"type": "object", "properties": {"text": {"type": "string"}},
									   "required": ["text"], "additionalProperties": false})"),
				Json::parse(R"({"type": "object", "properties": {"text": {"type": "string"}}})"), false},
			[](gnubase::engine::CommandContext& context) { return Json{{"text", context.parameters().at("text")}}; });
		engine.registerCommand({"test.fail", 1, "Always fails."},
			[](gnubase::engine::CommandContext&) -> Json { throw gnubase::engine::CommandError("not today"); });
	}
}

TEST_CASE("initialize negotiates the protocol version and describes the server", "[unit]")
{
	Engine engine;
	Server server(engine);
	const Json params = {{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()},
		{"clientInfo", {{"name", "test"}, {"version", "1"}}}};
	const Json response = respond(server, request(1, "initialize", params));
	CHECK(response.at("jsonrpc") == "2.0");
	CHECK(response.at("id") == 1);
	const Json& result = response.at("result");
	CHECK(result.at("protocolVersion") == "2025-06-18");
	CHECK(result.at("capabilities").contains("tools"));
	CHECK(result.at("capabilities").contains("resources"));
	CHECK(result.at("serverInfo").at("name") == "gnubase");
	CHECK(result.at("serverInfo").at("version") == GNUBASE_VERSION);

	// A version the server doesn't know gets the one it speaks.
	const Json older = respond(server, request(2, "initialize", {{"protocolVersion", "1999-01-01"}}));
	CHECK(older.at("result").at("protocolVersion") == "2025-06-18");

	CHECK(respond(server, request(3, "ping")).at("result") == Json::object());
	CHECK_FALSE(server.handle(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).has_value());
}

TEST_CASE("every engine command is a tool with its schemas", "[unit]")
{
	Engine engine;
	registerTestCommands(engine);
	Server server(engine);
	const Json tools = respond(server, request(1, "tools/list")).at("result").at("tools");
	CHECK(tools.size() == engine.commands().size());

	const Json* echo = findTool(tools, "test_echo"); // MCP clients accept letters, digits, '_' and '-' only
	REQUIRE(echo != nullptr);
	CHECK(echo->at("description") == "Returns its text.");
	CHECK(echo->at("inputSchema").at("required") == Json::array({"text"}));
	CHECK(echo->at("outputSchema").at("properties").contains("text"));
	CHECK(echo->at("annotations").at("readOnlyHint") == false);

	const Json* version = findTool(tools, "engine_version");
	REQUIRE(version != nullptr);
	CHECK(version->at("annotations").at("readOnlyHint") == true);
}

TEST_CASE("tools/call runs the command and returns its result", "[unit]")
{
	Engine engine;
	registerTestCommands(engine);
	Server server(engine);

	const Json echoed =
		respond(server, request(1, "tools/call", {{"name", "test_echo"}, {"arguments", {{"text", "hello"}}}}))
			.at("result");
	CHECK(echoed.at("isError") == false);
	CHECK(echoed.at("structuredContent").at("text") == "hello");
	CHECK(Json::parse(echoed.at("content").at(0).at("text").get<std::string>()).at("text") == "hello");

	// Failures of the command are tool results, so the agent sees why.
	const Json failed = respond(server, request(2, "tools/call", {{"name", "test_fail"}})).at("result");
	CHECK(failed.at("isError") == true);
	CHECK_THAT(failed.at("content").at(0).at("text").get<std::string>(), ContainsSubstring("not today"));
	const Json invalid =
		respond(server, request(3, "tools/call", {{"name", "test_echo"}, {"arguments", {{"text", 5}}}})).at("result");
	CHECK(invalid.at("isError") == true);
	CHECK_THAT(invalid.at("content").at(0).at("text").get<std::string>(), ContainsSubstring("parameters.text"));

	// An unknown tool is a protocol error.
	const Json unknown = respond(server, request(4, "tools/call", {{"name", "test_nothing"}}));
	CHECK(unknown.at("error").at("code") == -32602);
}

TEST_CASE("queries without required parameters are readable resources", "[unit]")
{
	Engine engine;
	registerTestCommands(engine);
	Server server(engine);
	const Json resources = respond(server, request(1, "resources/list")).at("result").at("resources");
	const auto version = std::find_if(resources.begin(), resources.end(),
		[](const Json& resource) { return resource.at("uri") == "gnubase://engine.version"; });
	REQUIRE(version != resources.end());
	CHECK(version->at("mimeType") == "application/json");
	CHECK(std::none_of(resources.begin(), resources.end(),
		[](const Json& resource) { return resource.at("uri") == "gnubase://test.echo"; })); // not a query

	const Json read = respond(server, request(2, "resources/read", {{"uri", "gnubase://engine.version"}}));
	const Json& content = read.at("result").at("contents").at(0);
	CHECK(content.at("uri") == "gnubase://engine.version");
	CHECK(Json::parse(content.at("text").get<std::string>()).at("version") == GNUBASE_VERSION);

	CHECK(respond(server, request(3, "resources/read", {{"uri", "gnubase://test.echo"}})).at("error").at("code") ==
		-32002);
	CHECK(respond(server, request(4, "resources/templates/list")).at("result").at("resourceTemplates").empty());
}

TEST_CASE("malformed messages get JSON-RPC errors", "[unit]")
{
	Engine engine;
	Server server(engine);
	const Json parseError = Json::parse(server.handle(std::string_view("{not json")).value_or("null"));
	CHECK(parseError.at("error").at("code") == -32700);
	CHECK(parseError.at("id").is_null());

	CHECK(respond(server, request(1, "nothing/here")).at("error").at("code") == -32601);
	CHECK(respond(server, Json{{"id", 2}, {"method", "ping"}}).at("error").at("code") == -32600); // no "jsonrpc"
	CHECK(respond(server, Json::array({request(3, "ping")})).at("error").at("code") == -32600);   // no batches
	CHECK(respond(server, request(4, "tools/call", {{"arguments", Json::object()}})).at("error").at("code") == -32602);
	CHECK_FALSE(server.handle(Json{{"jsonrpc", "2.0"}, {"id", 5}, {"result", Json::object()}}).has_value());
}

TEST_CASE("the stdio transport answers each request on its own line", "[unit]")
{
	Engine engine;
	Server server(engine);
	std::istringstream in(request(1, "initialize", {{"protocolVersion", "2025-06-18"}}).dump() + "\n" +
		Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump() + "\n\n" +
		request(2, "tools/list").dump() + "\n");
	std::ostringstream out;
	serveStdio(server, in, out);

	std::istringstream lines(out.str());
	std::string first;
	std::string second;
	std::string rest;
	REQUIRE(std::getline(lines, first));
	REQUIRE(std::getline(lines, second));
	CHECK_FALSE(std::getline(lines, rest));
	CHECK(Json::parse(first).at("id") == 1);
	CHECK(Json::parse(second).at("result").contains("tools"));
}

TEST_CASE("only loopback addresses count as local", "[unit]")
{
	CHECK(isLoopback("127.0.0.1"));
	CHECK(isLoopback("127.12.0.9"));
	CHECK(isLoopback("::1"));
	CHECK(isLoopback("::ffff:127.0.0.1"));
	CHECK_FALSE(isLoopback("192.168.1.10"));
	CHECK_FALSE(isLoopback("::ffff:10.0.0.1"));
	CHECK_FALSE(isLoopback("1270.0.0.1"));
	CHECK_FALSE(isLoopback(""));

	const std::string token = makeToken();
	CHECK(token.size() == 64);
	CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
	CHECK(token != makeToken());
}

TEST_CASE("the HTTP transport serves MCP on localhost and needs the access token", "[integration]")
{
	Engine engine;
	Server server(engine);
	const std::string token = makeToken();
	const HttpServer http(server, token);
	REQUIRE(http.port() > 0);
	CHECK(http.url() == "http://127.0.0.1:" + std::to_string(http.port()) + "/mcp");

	httplib::Client client("127.0.0.1", http.port());
	const std::string body = request(1, "ping").dump();
	const httplib::Headers authorized = {{"Authorization", "Bearer " + token}};

	const auto ok = client.Post("/mcp", authorized, body, "application/json");
	REQUIRE(ok);
	CHECK(ok->status == 200);
	CHECK(Json::parse(ok->body).at("id") == 1);

	const auto noToken = client.Post("/mcp", body, "application/json");
	REQUIRE(noToken);
	CHECK(noToken->status == 401);
	const auto wrongToken = client.Post("/mcp", {{"Authorization", "Bearer nope"}}, body, "application/json");
	REQUIRE(wrongToken);
	CHECK(wrongToken->status == 401);

	// Web pages must not reach the server through the browser (DNS rebinding).
	httplib::Headers foreign = authorized;
	foreign.emplace("Origin", "http://example.com");
	const auto rejected = client.Post("/mcp", foreign, body, "application/json");
	REQUIRE(rejected);
	CHECK(rejected->status == 403);
	httplib::Headers local = authorized;
	local.emplace("Origin", "http://localhost:3000");
	const auto accepted = client.Post("/mcp", local, body, "application/json");
	REQUIRE(accepted);
	CHECK(accepted->status == 200);

	const std::string notification = Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump();
	const auto noted = client.Post("/mcp", authorized, notification, "application/json");
	REQUIRE(noted);
	CHECK(noted->status == 202);
	const auto stream = client.Get("/mcp", authorized);
	REQUIRE(stream);
	CHECK(stream->status == 405); // no server-initiated stream yet
}
