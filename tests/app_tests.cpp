// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/headless.h"
#include "app/options.h"
#include "app/scripttool.h"
#include "engine/engine.h"
#include "mcp/server.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using gnubase::engine::Json;
using namespace gnubase::app;
using Catch::Matchers::ContainsSubstring;

namespace
{
	std::string message(int id, const std::string& method, const Json& params = Json::object())
	{
		return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
	}

	std::string callTool(int id, const std::string& tool, const Json& arguments = Json::object())
	{
		return message(id, "tools/call", {{"name", tool}, {"arguments", arguments}});
	}

	// The MCP session of the acceptance test: render half a second of a sine on the device input through the
	// project and fetch the first output samples.
	std::string renderSession()
	{
		return message(1, "initialize", {{"protocolVersion", "2025-06-18"}}) + callTool(2, "project_get") +
			callTool(
				3, "audio_setDevice", {{"blockSize", 128}, {"captureSeconds", 1}, {"input", {{"processor", "sine"}}}}) +
			callTool(4, "audio_render", {{"frames", 24000}}) +
			callTool(5, "audio_capture", {{"channel", 0}, {"start", 0}, {"frames", 1000}});
	}

	// Responses by request id.
	std::vector<Json> responses(const std::string& output)
	{
		std::vector<Json> result(16);
		std::istringstream lines(output);
		for (std::string line; std::getline(lines, line);)
		{
			const Json response = Json::parse(line);
			result.at(response.at("id").get<size_t>()) = response;
		}
		return result;
	}

	// A project with one audio track, which plays the device input.
	std::string saveProject(const gnubase::test::TempDir& dir)
	{
		gnubase::engine::Engine engine;
		gnubase::engine::Client client(engine);
		REQUIRE(client.call("track.add", {{"type", "audio"}}).ok());
		const std::string path = (dir.path() / "one track.dawproject").u8string();
		REQUIRE(client.call("project.save", {{"path", path}}).ok());
		return path;
	}

	void checkRendered(const std::vector<Json>& results)
	{
		CHECK(results[2].at("result").at("structuredContent").at("tracks").size() == 1);
		CHECK(results[4].at("result").at("isError") == false);
		const Json samples = results[5].at("result").at("structuredContent").at("samples");
		REQUIRE(samples.size() == 1000);
		const auto loudest = std::max_element(samples.begin(), samples.end(),
			[](const Json& a, const Json& b) { return std::abs(a.get<double>()) < std::abs(b.get<double>()); });
		CHECK(std::abs(loudest->get<double>()) > 0.1); // the sine came through
	}
}

TEST_CASE("the command line selects headless mode, the MCP transport, the audio device and the project", "[unit]")
{
	const Options defaults = parseOptions({});
	CHECK_FALSE(defaults.headless);
	CHECK(defaults.transport == Transport::Stdio);
	CHECK(defaults.audio == "dummy");

	const Options options = parseOptions({"--headless", "--mcp-http=8123", "--project", "song.dawproject", "--audio",
		"dummy", "--sample-rate", "44100", "--block-size", "512"});
	CHECK(options.headless);
	CHECK(options.transport == Transport::Http);
	CHECK(options.port == 8123);
	CHECK(options.project == "song.dawproject");
	CHECK(options.sampleRate == 44100.0);
	CHECK(options.blockSize == 512);
	CHECK(parseOptions({"--headless", "--mcp-http"}).port == 0); // a free port
	CHECK(parseOptions({"--help"}).help);
	CHECK(parseOptions({"--version"}).version);

	CHECK_THROWS_AS(parseOptions({"--nothing"}), UsageError);
	CHECK_THROWS_AS(parseOptions({"--project"}), UsageError);
	CHECK_THROWS_AS(parseOptions({"--sample-rate", "fast"}), UsageError);
	CHECK_THROWS_AS(parseOptions({"--block-size", "12x"}), UsageError);
	CHECK_THROWS_AS(parseOptions({"--mcp-http=port"}), UsageError);
	CHECK_THROWS_AS(parseOptions({"--audio", "nothing"}), UsageError); // no backend of this build
	CHECK(parseOptions({"--headless", "--script", "check.lua"}).script == "check.lua");
	CHECK_THROWS_AS(parseOptions({"--script"}), UsageError);
	CHECK_THROWS_AS(parseOptions({"--headless", "--script", "a.lua", "--mcp-http"}), UsageError); // one or the other
	CHECK_THAT(usage(), ContainsSubstring("--headless"));
}

TEST_CASE("MCP clients run Lua scripts in a sandbox with script_run", "[unit]")
{
	gnubase::engine::Engine engine;
	gnubase::mcp::Server server(engine);
	addScriptTool(server, engine);
	const auto call = [&server](const std::string& code)
	{
		const Json request = Json::parse(callTool(1, "script_run", {{"code", code}, {"name", "agent.lua"}}));
		return server.handle(request).value_or(Json()).at("result");
	};

	const Json tools = server.handle(Json::parse(message(1, "tools/list"))).value_or(Json()).at("result").at("tools");
	CHECK(std::any_of(tools.begin(), tools.end(), [](const Json& tool) { return tool.at("name") == "script_run"; }));

	const Json printed = call("print('version', gnubase.engine.version().version)");
	CHECK(printed.at("isError") == false);
	CHECK(printed.at("structuredContent").at("output") == std::string("version\t") + GNUBASE_VERSION + "\n");

	const Json failed = call("print('before'); error('nope')");
	CHECK(failed.at("isError") == true);
	const std::string text = failed.at("content").at(0).at("text").get<std::string>();
	CHECK_THAT(text, ContainsSubstring("agent.lua:1: nope"));
	CHECK_THAT(text, ContainsSubstring("before")); // what it printed until then

	CHECK(call("io.open('secret.txt')").at("isError") == true); // no files
}

TEST_CASE("MCP clients run Lua test cases with script_run", "[unit]")
{
	gnubase::engine::Engine engine;
	gnubase::mcp::Server server(engine);
	addScriptTool(server, engine);
	const auto call = [&server](const std::string& code)
	{
		const Json request = Json::parse(callTool(1, "script_run", {{"code", code}, {"name", "agent.lua"}}));
		return server.handle(request).value_or(Json()).at("result");
	};

	const Json passed =
		call("gnubase.test.case('has a version', function() gnubase.test.ok(gnubase.engine.version().version) end)");
	CHECK(passed.at("isError") == false);
	CHECK(passed.at("structuredContent").at("output") == "1..1\nok 1 - has a version\n");

	const Json failed = call("local test = gnubase.test\ntest.case('adds', function()\n\ttest.equal(1 + 1, 3)\nend)");
	CHECK(failed.at("isError") == true);
	const std::string text = failed.at("content").at(0).at("text").get<std::string>();
	CHECK_THAT(text, ContainsSubstring("not ok 1 - adds\n# agent.lua:3: expected 3, got 2\n"));
	CHECK_THAT(text, ContainsSubstring("1 of 1 test cases failed: adds"));
}

TEST_CASE("headless GNUbase loads a project and renders it for an MCP client on stdio", "[integration]")
{
	const gnubase::test::TempDir dir;
	Options options;
	options.headless = true;
	options.project = saveProject(dir);
	std::istringstream in(renderSession());
	std::ostringstream out;
	std::ostringstream err;
	CHECK(runHeadless(options, in, out, err) == g_exitOk);
	CHECK(err.str().empty());
	checkRendered(responses(out.str()));
}

TEST_CASE("headless GNUbase reports a project it cannot load", "[integration]")
{
	Options options;
	options.headless = true;
	options.project = "no such file.dawproject";
	std::istringstream in;
	std::ostringstream out;
	std::ostringstream err;
	CHECK(runHeadless(options, in, out, err) == g_exitSetup);
	CHECK_THAT(err.str(), ContainsSubstring("no such file.dawproject"));
	CHECK(out.str().empty()); // stdout belongs to MCP
}

namespace
{
	// The acceptance script for Lua: build a project, render it and check the level.
	constexpr const char* g_renderScript = R"(
gnubase.project.new()
gnubase.track.add{type = "audio"} -- plays the device input
gnubase.audio.setDevice{blockSize = 128, captureSeconds = 1, input = {processor = "sine"}}
gnubase.audio.render{frames = 24000}
local peak = 0
for _, sample in ipairs(gnubase.audio.capture{channel = 0, start = 0, frames = 24000}.samples) do
	peak = math.max(peak, math.abs(sample))
end
print(string.format("peak %.2f", peak))
assert(peak > 0.4, "too quiet")
)";

	std::filesystem::path writeScript(
		const gnubase::test::TempDir& dir, const std::string& name, const std::string& code)
	{
		const std::filesystem::path file = dir.path() / name;
		std::ofstream(file) << code;
		return file;
	}
}

TEST_CASE("headless GNUbase runs a Lua script that renders a project and checks its level", "[integration]")
{
	const gnubase::test::TempDir dir;
	Options options;
	options.headless = true;
	options.script = writeScript(dir, "render.lua", g_renderScript).u8string();
	std::istringstream in;
	std::ostringstream out;
	std::ostringstream err;
	CHECK(runHeadless(options, in, out, err) == g_exitOk);
	CHECK(out.str() == "peak 0.50\n"); // the sine generator's default level
	CHECK(err.str().empty());

	options.script = writeScript(dir, "fail.lua", "local level = 0\nassert(level > 1, 'too quiet')\n").u8string();
	CHECK(runHeadless(options, in, out, err) == g_exitScript);
	CHECK_THAT(err.str(), ContainsSubstring("fail.lua:2: too quiet"));
}

#ifdef GNUBASE_APP
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace
{
	// Runs the gnubase executable with arguments, stdin from a file and stdout into one; returns its exit code.
	int runApp(const std::string& arguments, const std::filesystem::path& in, const std::filesystem::path& out)
	{
		const std::string command = "\"" + std::string(GNUBASE_APP) + "\" " + arguments + " < \"" + in.u8string() +
			"\" > \"" + out.u8string() + "\"";
#ifdef _WIN32
		// cmd.exe removes the outer quotes.
		return std::system(("\"" + command + "\"").c_str()); // NOLINT(bugprone-command-processor): fixed test command
#else
		const int status = std::system(command.c_str()); // NOLINT(bugprone-command-processor): fixed test command
		return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
	}
}

TEST_CASE("the gnubase executable serves MCP on stdio in headless mode", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string project = saveProject(dir);
	const auto in = dir.path() / "in.jsonl";
	const auto out = dir.path() / "out.jsonl";
	std::ofstream(in) << renderSession();

	CHECK(runApp("--headless --block-size 128 --project \"" + project + "\"", in, out) == g_exitOk);
	checkRendered(responses(gnubase::test::readText(out)));

	const auto script = writeScript(dir, "render.lua", g_renderScript);
	CHECK(runApp("--headless --script \"" + script.u8string() + "\"", in, out) == g_exitOk);
	CHECK_THAT(gnubase::test::readText(out), ContainsSubstring("peak 0.50"));

	CHECK(runApp("--no-such-option", in, out) == g_exitUsage);
	CHECK(runApp("--headless --project \"" + (dir.path() / "missing.dawproject").u8string() + "\"", in, out) ==
		g_exitSetup);
}

TEST_CASE("the gnubase executable reports engine events to MCP clients on stdio", "[integration]")
{
	const gnubase::test::TempDir dir;
	const auto in = dir.path() / "in.jsonl";
	const auto out = dir.path() / "out.jsonl";
	const Json trackAdded = {{"names", {"track.added"}}};
	std::ofstream(in) << message(1, "initialize", {{"protocolVersion", "2025-06-18"}}) +
			callTool(2, "track_add", {{"type", "audio"}}) + callTool(3, "events_take", trackAdded) +
			callTool(4, "script_run", {{"code", "gnubase.track.add{type = 'instrument'}"}}) +
			callTool(5, "events_take", trackAdded) +
			callTool(6, "script_run", {{"code", "for i = 1, 1100 do gnubase.transport.locate{position = i} end"}}) +
			callTool(7, "events_take") + callTool(8, "events_take", {{"seconds", 0.2}});

	CHECK(runApp("--headless", in, out) == g_exitOk);
	const std::vector<Json> results = responses(gnubase::test::readText(out));
	const auto taken = [&results](size_t id) { return results.at(id).at("result").at("structuredContent"); };

	const Json own = taken(3).at("events"); // the session's own command
	REQUIRE(own.size() == 1);
	CHECK(own[0].at("data").at("track") == results[2].at("result").at("structuredContent").at("track"));
	const Json lua = taken(5).at("events"); // a command of another client, the Lua script
	REQUIRE(lua.size() == 1);
	CHECK(lua[0].at("data").at("track").at("type") == "instrument");

	// The queue keeps the newest 1000 events and counts the others.
	const Json full = taken(7);
	REQUIRE(full.at("events").size() == 1000);
	CHECK(full.at("dropped").get<int>() >= 100);
	CHECK(full.at("events").back().at("name") == "transport.changed");
	CHECK(full.at("events").back().at("data").at("position") == 1100);
	CHECK(taken(8).at("events").empty()); // after waiting 0.2 s
	CHECK(taken(8).at("dropped") == 0);
}

TEST_CASE("the gnubase executable runs a Lua test file and exits with code 4 when an assertion fails", "[integration]")
{
	const gnubase::test::TempDir dir;
	const auto in = writeScript(dir, "in.txt", "");
	const auto out = dir.path() / "out.txt";
	const auto script = writeScript(dir, "version.lua", R"(local test = gnubase.test
test.case("has a version", function() test.ok(gnubase.engine.version().version) end)
test.case("is version 0", function()
	test.equal(gnubase.engine.version().version, "0")
end)
)");
	CHECK(runApp("--headless --script \"" + script.u8string() + "\"", in, out) == g_exitScript);
	const std::string output = gnubase::test::readText(out); // Windows writes CRLF to stdout
	CHECK_THAT(output, ContainsSubstring("ok 1 - has a version"));
	CHECK_THAT(output, ContainsSubstring("not ok 2 - is version 0"));
	CHECK_THAT(output, ContainsSubstring("version.lua:4: expected \"0\", got \"" GNUBASE_VERSION "\""));
}
#endif
