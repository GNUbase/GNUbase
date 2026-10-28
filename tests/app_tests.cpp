// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/headless.h"
#include "app/options.h"
#include "engine/engine.h"
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
		std::vector<Json> result(8);
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
	CHECK_THAT(usage(), ContainsSubstring("--headless"));
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

	CHECK(runApp("--no-such-option", in, out) == g_exitUsage);
	CHECK(runApp("--headless --project \"" + (dir.path() / "missing.dawproject").u8string() + "\"", in, out) ==
		g_exitSetup);
}
#endif
