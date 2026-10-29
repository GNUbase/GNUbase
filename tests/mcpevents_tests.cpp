// SPDX-License-Identifier: GPL-3.0-or-later
// Engine events for MCP clients (#273): every session has a queue of the events since it started, and events_take
// takes them.
#include "engine/engine.h"
#include "mcp/server.h"
#include "mcp/transport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

using gnubase::engine::Client;
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

	Json toolCall(const std::string& tool, const Json& arguments = Json::object())
	{
		return request(1, "tools/call", {{"name", tool}, {"arguments", arguments}});
	}

	// The result of a tool call in a session.
	Json call(Server& server, std::string& session, const std::string& tool, const Json& arguments = Json::object())
	{
		const std::optional<Json> response = server.handle(toolCall(tool, arguments), session);
		REQUIRE(response.has_value());
		return response.value_or(Json()).at("result");
	}

	// The events events_take returned, which must have succeeded.
	Json events(const Json& result)
	{
		REQUIRE(result.at("isError") == false);
		return result.at("structuredContent").at("events");
	}

	size_t count(const Json& events, const std::string& name)
	{
		return static_cast<size_t>(std::count_if(
			events.begin(), events.end(), [&name](const Json& event) { return event.at("name") == name; }));
	}

	double secondsSince(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	}
}

TEST_CASE("initialize starts a session that receives every engine event from then on", "[unit]")
{
	Engine engine;
	Server server(engine);
	std::string session;
	const std::optional<Json> initialized =
		server.handle(request(1, "initialize", {{"protocolVersion", g_protocolVersion}}), session);
	REQUIRE(initialized.has_value());
	REQUIRE(!session.empty());
	CHECK(server.hasSession(session));

	const Json tools = server.handle(request(2, "tools/list")).value_or(Json()).at("result").at("tools");
	const auto tool =
		std::find_if(tools.begin(), tools.end(), [](const Json& t) { return t.at("name") == "events_take"; });
	REQUIRE(tool != tools.end());
	CHECK(tool->at("annotations").at("readOnlyHint") == true); // takes events, changes nothing in GNUbase

	// The session's own command: its events are there when the result is.
	const Json added = call(server, session, "track_add", {{"type", "audio"}});
	REQUIRE(added.at("isError") == false);
	const Json taken = call(server, session, "events_take");
	CHECK(taken.at("structuredContent").at("dropped") == 0);
	const Json first = events(taken);
	REQUIRE(count(first, "track.added") == 1);
	const Json event = *std::find_if(
		first.begin(), first.end(), [](const Json& candidate) { return candidate.at("name") == "track.added"; });
	CHECK(event.at("data").at("track").at("id") == added.at("structuredContent").at("track").at("id"));
	CHECK(event.at("request").get<uint64_t>() > 0);
	CHECK(events(call(server, session, "events_take")).empty()); // taken

	// Commands of other clients, here a Lua script or the UI.
	Client other(engine);
	REQUIRE(other.call("track.add", {{"type", "instrument"}}).ok());
	REQUIRE(other.call("track.add", {{"type", "midi"}}).ok());
	const Json added2 = events(call(server, session, "events_take", {{"names", {"track.added"}}}));
	CHECK(added2.size() == 2);
	CHECK(count(added2, "track.added") == 2);
	const Json rest = events(call(server, session, "events_take")); // the other events stayed
	CHECK(!rest.empty());
	CHECK(count(rest, "track.added") == 0);

	// A request without the session doesn't take its events.
	const Json stateless = server.handle(toolCall("events_take")).value_or(Json()).at("result");
	CHECK(stateless.at("isError") == true);
	CHECK_THAT(stateless.at("content").at(0).at("text").get<std::string>(), ContainsSubstring("initialize"));
	std::string none;
	REQUIRE(server.handle(request(3, "ping"), none).has_value());
	CHECK(none.empty()); // only initialize starts a session
}

TEST_CASE("events_take waits up to the given seconds for the first event", "[unit]")
{
	Engine engine;
	Server server(engine);
	std::string session = server.startSession();
	Client other(engine);

	// Catch2 assertions stay on this thread.
	auto waiting = std::async(std::launch::async, [&server, session]() mutable
		{ return server.handle(toolCall("events_take", {{"names", {"track.added"}}, {"seconds", 10}}), session); });
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	const auto sent = std::chrono::steady_clock::now();
	REQUIRE(other.call("track.add", {{"type", "audio"}}).ok());
	const std::optional<Json> response = waiting.get();
	CHECK(secondsSince(sent) < 5.0); // as soon as the event arrived, not after 10 s
	REQUIRE(response.has_value());
	CHECK(count(events(response.value_or(Json()).at("result")), "track.added") == 1);

	const auto start = std::chrono::steady_clock::now();
	CHECK(events(call(server, session, "events_take", {{"names", {"nothing.happened"}}, {"seconds", 0.2}})).empty());
	CHECK(secondsSince(start) >= 0.2);

	const Json tooLong = call(server, session, "events_take", {{"seconds", 61}});
	CHECK(tooLong.at("isError") == true);
	CHECK_THAT(tooLong.at("content").at(0).at("text").get<std::string>(), ContainsSubstring("60"));
	CHECK(call(server, session, "events_take", {{"seconds", -1}}).at("isError") == true);
}

TEST_CASE("a full session queue drops the oldest events and reports how many", "[unit]")
{
	Engine engine;
	Server server(engine, SessionLimits{3, std::chrono::minutes(30)});
	std::string session = server.startSession();
	Client other(engine);
	for (int i = 1; i <= 5; ++i)
		REQUIRE(other.call("transport.locate", {{"position", i * 100}}).ok());

	const Json taken = call(server, session, "events_take");
	const Json kept = events(taken);
	REQUIRE(kept.size() == 3);
	CHECK(taken.at("structuredContent").at("dropped") == 2);
	CHECK(kept.at(0).at("data").at("position") == 300); // the newest stay
	CHECK(kept.at(2).at("data").at("position") == 500);
	CHECK(call(server, session, "events_take").at("structuredContent").at("dropped") == 0); // reported once
}

TEST_CASE("sessions end when the client ends them or when they are idle", "[unit]")
{
	Engine engine;
	Server server(engine);
	const std::string session = server.startSession();
	CHECK(server.hasSession(session));
	CHECK(server.endSession(session));
	CHECK_FALSE(server.hasSession(session));
	CHECK_FALSE(server.endSession(session));
	std::string ended = session;
	const Json result = call(server, ended, "events_take");
	CHECK(result.at("isError") == true);

	Server idle(engine, SessionLimits{1000, std::chrono::seconds(0)});
	const std::string expiring = idle.startSession();
	const std::string lasting = idle.startSession(false);
	CHECK_FALSE(idle.hasSession(expiring)); // no idle time at all
	CHECK(idle.hasSession(lasting));
}

TEST_CASE("over stdio, the whole input is one session", "[unit]")
{
	Engine engine;
	Server server(engine);
	std::istringstream in(request(1, "initialize", {{"protocolVersion", g_protocolVersion}}).dump() + "\n" +
		toolCall("track_add", {{"type", "audio"}}).dump() + "\n" +
		toolCall("events_take", {{"names", {"track.added"}}}).dump() + "\n");
	std::ostringstream out;
	serveStdio(server, in, out);

	std::istringstream lines(out.str());
	std::string line;
	for (int i = 0; i < 3; ++i)
		REQUIRE(std::getline(lines, line));
	CHECK(count(events(Json::parse(line).at("result")), "track.added") == 1);
}

TEST_CASE("over HTTP, initialize starts a session of its own for every client", "[integration]")
{
	Engine engine;
	Server server(engine);
	const std::string token = makeToken();
	const HttpServer http(server, token);
	httplib::Client client("127.0.0.1", http.port());
	const httplib::Headers authorized = {{"Authorization", "Bearer " + token}};

	const auto initialize = [&]
	{
		const auto response = client.Post("/mcp", authorized,
			request(1, "initialize", {{"protocolVersion", g_protocolVersion}}).dump(), "application/json");
		REQUIRE(response);
		CHECK(response->status == 200);
		return response->get_header_value("Mcp-Session-Id");
	};
	const auto post = [&](const std::string& session, const Json& message)
	{
		httplib::Headers headers = authorized;
		if (!session.empty())
			headers.emplace("Mcp-Session-Id", session);
		return client.Post("/mcp", headers, message.dump(), "application/json");
	};
	const auto take = [&](const std::string& session)
	{
		const auto response = post(session, toolCall("events_take"));
		REQUIRE(response);
		REQUIRE(response->status == 200);
		return Json::parse(response->body).at("result");
	};

	const std::string first = initialize();
	const std::string second = initialize();
	REQUIRE(!first.empty());
	REQUIRE(!second.empty());
	CHECK(first != second);

	const auto added = post(first, toolCall("track_add", {{"type", "audio"}}));
	REQUIRE(added);
	CHECK(added->status == 200);
	CHECK(added->get_header_value("Mcp-Session-Id") == first);
	CHECK(count(events(take(first)), "track.added") == 1); // both sessions get every event
	CHECK(count(events(take(second)), "track.added") == 1);
	CHECK(take("").at("isError") == true); // without a session there are no events

	httplib::Headers ending = authorized;
	ending.emplace("Mcp-Session-Id", first);
	const auto ended = client.Delete("/mcp", ending);
	REQUIRE(ended);
	CHECK(ended->status == 204);
	const auto gone = post(first, request(2, "ping"));
	REQUIRE(gone);
	CHECK(gone->status == 404); // the client starts a new session with initialize
	const auto endedAgain = client.Delete("/mcp", ending);
	REQUIRE(endedAgain);
	CHECK(endedAgain->status == 404);
	const auto noSession = client.Delete("/mcp", authorized);
	REQUIRE(noSession);
	CHECK(noSession->status == 400);
	const auto unknown = post("no such session", request(3, "ping"));
	REQUIRE(unknown);
	CHECK(unknown->status == 404);
	const auto stillThere = post(second, request(4, "ping"));
	REQUIRE(stillThere);
	CHECK(stillThere->status == 200);
}
