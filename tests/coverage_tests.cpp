// SPDX-License-Identifier: GPL-3.0-or-later
// Every engine command must be reachable through MCP and Lua, with a schema and documentation (#31). The tools and
// bindings are generated from the command registry, so this fails when a command lacks what generation needs.
#include "app/scripttool.h"
#include "app/uitools.h"
#include "engine/engine.h"
#include "mcp/catalogue.h"
#include "mcp/server.h"
#include "scripting/reference.h"
#include "scripting/runtime.h"
#include "testutils/fakeui.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

namespace
{
	// The generated references go to the build folder, where CI picks them up (docs/api-reference.md). They are read
	// there, in the artifact and on the web, so their links must lead somewhere from anywhere: no relative ones.
	void writeGenerated(const char* name, const std::string& text)
	{
		CAPTURE(name);
		for (std::size_t link = text.find("]("); link != std::string::npos; link = text.find("](", link + 2))
		{
			const std::string target = text.substr(link + 2, text.find(')', link) - link - 2);
			CAPTURE(target);
			CHECK((target.rfind("https://", 0) == 0 || target.rfind('#', 0) == 0));
		}
		const std::filesystem::path folder = GNUBASE_GENERATED_DOCS_DIR;
		std::filesystem::create_directories(folder);
		std::ofstream file(folder / name, std::ios::binary);
		file << text;
		file.close();
		CHECK(file.good());
	}

	// An MCP server with every tool: the engine commands, script_run and the windowed app's UI tools.
	struct AllTools
	{
		gnubase::test::FakeUi ui{{}};
		Engine engine;
		gnubase::mcp::Server server{engine};

		AllTools()
		{
			gnubase::app::addScriptTool(server, engine);
			gnubase::app::addUiTools(server, ui.shell);
		}
	};

	std::string catalogue()
	{
		AllTools all;
		return gnubase::mcp::catalogue(all.server);
	}
}

TEST_CASE("the MCP tool catalogue lists every tool with its arguments, and the resources", "[unit]")
{
	const std::string text = catalogue();
	CHECK(text.find("### track\n\n#### `track_add`\n\nEngine command `track.add`.") != std::string::npos);
	CHECK(text.find("#### `script_run`\n\n") != std::string::npos); // tools that aren't engine commands too
	CHECK(text.find("(Lua API reference: https://gnubase.github.io/GNUbase/lua.html)") != std::string::npos);
	CHECK(text.find("### ui\n\n#### `ui_click`\n\n") != std::string::npos);
	CHECK(text.find("#### `project_get`\n\nEngine command `project.get`. Read-only.") != std::string::npos);
	CHECK(text.find("- `type`: one of \"audio\" | \"instrument\"") != std::string::npos);
	CHECK(text.find("Result:\n\n- `channel`: object or null, required\n- `track`: object, required") !=
		std::string::npos);
	CHECK(text.find("- `gnubase://project.get`: ") != std::string::npos);
	CHECK(text.find("gnubase://track.add") == std::string::npos); // not a query
}

TEST_CASE("the MCP tool catalogue is generated from the MCP server", "[unit]")
{
	AllTools all;
	const std::string text = gnubase::mcp::catalogue(all.server);
	const Json tools = all.server.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})
						   .value_or(Json())
						   .at("result")
						   .at("tools");
	CHECK(tools.size() > 100);
	for (const Json& tool : tools)
	{
		const std::string name = tool.at("name").get<std::string>();
		CAPTURE(name);
		CHECK(text.find("#### `" + name + "`\n") != std::string::npos);
	}
	writeGenerated("mcp-tools.md",
		"# MCP tool catalogue\n\nGenerated from what the MCP server lists, with `script_run`: every tool by area, its "
		"engine command, whether it only reads, its arguments and its result.\n\n" +
			text);
}

TEST_CASE("the Lua API reference is generated from the command registry", "[unit]")
{
	Engine engine;
	const std::string generated = gnubase::scripting::reference(engine);
	CHECK(generated.rfind("# Lua API\n", 0) == 0);
	for (const CommandInfo& info : engine.commands())
	{
		CAPTURE(info.name);
		CHECK(generated.find("#### `gnubase." + info.name + "`") != std::string::npos);
	}
	writeGenerated("lua.md", generated);
}

TEST_CASE("the generated API references stay out of the repository", "[unit]")
{
	// A branch made before they left that keeps its side of the conflict would bring back a stale copy.
	const std::filesystem::path docs = std::filesystem::path(GNUBASE_RESOURCES_DIR).parent_path() / "docs";
	INFO("If a branch from before #919 restores generated references, see docs/api-reference.md for the merge "
		 "resolution.");
	REQUIRE(std::filesystem::exists(docs / "api-reference.md"));
	CHECK_FALSE(std::filesystem::exists(docs / "lua.md"));
	CHECK_FALSE(std::filesystem::exists(docs / "mcp-tools.md"));
	// The catalogue's entries begin with their engine command; the guide in mcp.md has none.
	CHECK(gnubase::test::readText(docs / "mcp.md").find("Engine command `") == std::string::npos);
}
