// SPDX-License-Identifier: GPL-3.0-or-later
// Every engine command must be reachable through MCP and Lua, with a schema and documentation (#31). The tools and
// bindings are generated from the command registry, so this fails when a command lacks what generation needs.
#include "app/scripttool.h"
#include "engine/engine.h"
#include "mcp/catalogue.h"
#include "mcp/server.h"
#include "scripting/reference.h"
#include "scripting/runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

using gnubase::engine::CommandInfo;
using gnubase::engine::Engine;
using gnubase::engine::Json;

TEST_CASE("every engine command is reachable through MCP and Lua, with a schema and documentation", "[unit]")
{
	Engine engine;
	gnubase::mcp::Server server(engine);
	const Json tools = server.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})
						   .value_or(Json())
						   .at("result")
						   .at("tools");
	std::ostringstream output;
	gnubase::scripting::Runtime lua(engine, gnubase::scripting::Trust::Untrusted, output);

	for (const CommandInfo& info : engine.commands())
	{
		CAPTURE(info.name);
		CHECK(info.name.find('.') != std::string::npos);
		CHECK(info.description.size() > 10);
		CHECK(info.parameters.value("type", "") == "object");
		std::string tool = info.name;
		std::replace(tool.begin(), tool.end(), '.', '_');
		CHECK(std::any_of(tools.begin(), tools.end(), [&tool](const Json& t) { return t.at("name") == tool; }));
		const std::optional<std::string> error =
			lua.run("assert(type(gnubase." + info.name + ") == 'function')", info.name);
		CHECK(error.value_or("").empty());
	}
}

TEST_CASE("docs/lua.md is the reference generated from the command registry", "[unit]")
{
	Engine engine;
	const std::string generated = gnubase::scripting::reference(engine);
	const std::string path = std::string(GNUBASE_DOCS_DIR) + "/lua.md";
	if (const char* update = std::getenv("GNUBASE_UPDATE_DOCS"); update != nullptr && std::string(update) == "1")
		std::ofstream(path, std::ios::binary) << generated;

	std::ifstream in(path, std::ios::binary);
	std::string written{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	written.erase(std::remove(written.begin(), written.end(), '\r'), written.end()); // a Windows checkout has CRLF
	INFO("docs/lua.md is out of date: run the tests with GNUBASE_UPDATE_DOCS=1 and commit the file");
	CHECK(written == generated);
}

namespace
{
	constexpr const char* g_catalogueBegin =
		"<!-- Generated from the MCP server's tools/list: run the tests with GNUBASE_UPDATE_DOCS=1 to update it. -->\n";
	constexpr const char* g_catalogueEnd = "<!-- End of the generated tool catalogue -->\n";

	std::string catalogue()
	{
		Engine engine;
		gnubase::mcp::Server server(engine);
		gnubase::app::addScriptTool(server, engine);
		return gnubase::mcp::catalogue(server);
	}
}

TEST_CASE("the MCP tool catalogue lists every tool with its arguments, and the resources", "[unit]")
{
	const std::string text = catalogue();
	CHECK(text.find("### track\n\n#### `track_add`\n\nEngine command `track.add`.") != std::string::npos);
	CHECK(text.find("#### `script_run`\n\n") != std::string::npos); // tools that aren't engine commands too
	CHECK(text.find("#### `project_get`\n\nEngine command `project.get`. Read-only.") != std::string::npos);
	CHECK(text.find("- `type`: one of \"audio\" | \"instrument\"") != std::string::npos);
	CHECK(text.find("Result:\n\n- `channel`: object or null, required\n- `track`: object, required") !=
		std::string::npos);
	CHECK(text.find("- `gnubase://project.get`: ") != std::string::npos);
	CHECK(text.find("gnubase://track.add") == std::string::npos); // not a query
}

TEST_CASE("docs/mcp.md has the tool catalogue generated from the MCP server", "[unit]")
{
	const std::string path = std::string(GNUBASE_DOCS_DIR) + "/mcp.md";
	std::ifstream in(path, std::ios::binary);
	std::string written{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	in.close();
	written.erase(std::remove(written.begin(), written.end(), '\r'), written.end()); // a Windows checkout has CRLF
	const size_t begin = written.find(g_catalogueBegin);
	const size_t end = written.find(g_catalogueEnd);
	REQUIRE(begin != std::string::npos);
	REQUIRE(end != std::string::npos);
	REQUIRE(begin < end);
	const std::string generated = written.substr(0, begin + std::string(g_catalogueBegin).size()) + "\n" + catalogue() +
		"\n" + written.substr(end);
	if (const char* update = std::getenv("GNUBASE_UPDATE_DOCS"); update != nullptr && std::string(update) == "1")
	{
		std::ofstream(path, std::ios::binary) << generated;
		written = generated;
	}
	INFO("docs/mcp.md's tool catalogue is out of date: run the tests with GNUBASE_UPDATE_DOCS=1 and commit the file");
	CHECK(written == generated);
}
