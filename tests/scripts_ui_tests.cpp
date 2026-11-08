// SPDX-License-Identifier: GPL-3.0-or-later
// User scripts (#209, docs/features/scripts.md) in the app's UI: the Edit menu's Scripts submenu, and the script
// console opening scripts, their folder and the Lua API reference with the OS. tests/lua/ui/scripts.lua checks running
// them, their undo step and the console's prompt.
#include "ui/automation.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

TEST_CASE("the Edit menu runs the user scripts, and the script console opens them with the OS", "[ui]")
{
	test::UiApp app;
	app.appUi().showProject();
	std::vector<std::filesystem::path> opened;
	app.appUi().openExternallyWith([&opened](const std::filesystem::path& path) { opened.push_back(path); });
	const std::filesystem::path folder = app.engine().settingsDirectory() / "scripts";
	REQUIRE(std::filesystem::is_regular_file(folder / "list-tracks.lua")); // an example

	// Edit > Scripts lists them, then Script Console.
	const Json items = app.ui().dom(std::nullopt, "#scripts-menu menuitem", 0).at("elements");
	REQUIRE(items.size() >= 3);
	CHECK(items.front().at("attributes").at("command") == "script.add-audio-tracks");
	CHECK(items.back().at("attributes").at("command") == "window.scriptConsole");
	CHECK(
		app.ui().dom(std::nullopt, "#scripts-menu menuitem[command=\"script.list-tracks\"]", 0).at("elements").size() ==
		1);

	app.keys().run("window.scriptConsole", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.shell().find(app.appUi().scriptConsoleWindow()) != nullptr; }));
	const ui::WindowId window = app.appUi().scriptConsoleWindow();
	REQUIRE(
		app.waitUntil([&app, window] { return !app.ui().dom(window, "#script-1 .edit", 0).at("elements").empty(); }));

	// Edit opens the script, Open Folder its folder, Lua API Reference the reference written for this GNUbase.
	app.ui().click(window, {"#script-1 .edit", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(opened.size() == 1);
	CHECK(opened[0] == folder / "add-audio-tracks.lua");
	app.ui().click(window, {"#script-folder", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(opened.size() == 2);
	CHECK(opened[1] == folder);
	app.ui().click(window, {"#script-reference", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(opened.size() == 3);
	CHECK(opened[2] == folder / "lua-api.md");
	std::ifstream file(opened[2]);
	const std::string reference{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	CHECK_THAT(reference, ContainsSubstring("gnubase.history.join"));
}

TEST_CASE(
	"a script run from Edit > Scripts that writes a script and finds the scripts again keeps its undo step", "[ui]")
{
	test::UiApp app;
	app.appUi().showProject();
	const std::filesystem::path folder = app.engine().settingsDirectory() / "scripts";
	const std::filesystem::path written = folder / "written.lua";
	{
		// Finding the scripts again replaces every script command and the menu's items, the running ones too.
		std::ofstream writer(folder / "writer.lua");
		writer << R"(gnubase.track.add{type = "audio"}
local file = assert(io.open([==[)"
			   << written.generic_u8string() << R"(]==], "w"))
file:write('gnubase.track.add{type = "midi"}')
file:close()
gnubase.ui.scripts()
)";
	}
	const std::optional<std::string> error = app.lua().run("gnubase.ui.scripts()", "check");
	INFO(error.value_or(""));
	REQUIRE(!error.has_value());
	app.shell().step(std::chrono::milliseconds(0)); // Edit > Scripts gets the new script

	const auto click = [&app](const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	};
	click("#menu-bar button[menu=edit-menu]");
	app.ui().move(std::nullopt, {"#scripts-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click("#scripts-menu menuitem[command=\"script.writer\"]");
	const auto tracks = [&app] { return app.call("project.get").at("tracks").size(); };
	REQUIRE(app.waitUntil([&tracks] { return tracks() == 1; }));
	CHECK(app.call("history.get").at("steps").back().at("action") == "writer");
	REQUIRE(app.waitUntil(
		[&app]
		{
			return !app.ui()
						.dom(std::nullopt, "#scripts-menu menuitem[command=\"script.written\"]", 0)
						.at("elements")
						.empty();
		}));
	app.keys().run("script.written", Json::object(), app.main(), nullptr);
	CHECK(tracks() == 2);
}
