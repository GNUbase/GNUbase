// SPDX-License-Identifier: GPL-3.0-or-later
// User scripts (#209, docs/features/scripts.md) in the app's UI: the Edit menu's Scripts submenu, and the script
// console opening scripts, their folder and the Lua API reference with the OS. tests/lua/ui/scripts.lua checks running
// them, their undo step and the console's prompt.
#include "ui/automation.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
