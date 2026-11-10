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
	std::vector<bool> edited;
	app.appUi().openExternallyWith(
		[&opened, &edited](const std::filesystem::path& path, bool edit)
		{
			opened.push_back(path);
			edited.push_back(edit);
		});
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
	CHECK(edited == std::vector<bool>{true, false, false}); // the script in the user's editor, which doesn't run it
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

namespace
{
	// Writes a user script and finds the scripts again, as Reload does.
	void writeScript(test::UiApp& app, const std::string& name, const std::string& code)
	{
		std::ofstream(app.engine().settingsDirectory() / "scripts" / (name + ".lua")) << code;
		const std::optional<std::string> error = app.lua().run("gnubase.ui.scripts()", "check");
		INFO(error.value_or(""));
		REQUIRE(!error.has_value());
		app.shell().step(std::chrono::milliseconds(0)); // their keys
	}

	ui::WindowId openConsole(test::UiApp& app)
	{
		app.keys().run("window.scriptConsole", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return app.shell().find(app.appUi().scriptConsoleWindow()) != nullptr; }));
		return app.appUi().scriptConsoleWindow();
	}

	std::string output(test::UiApp& app, ui::WindowId window)
	{
		return app.ui().dom(window, "#script-output", 0).at("elements").at(0).value("text", std::string());
	}
}

TEST_CASE("a user script or console line that runs too long asks whether it goes on, and stops", "[ui]")
{
	test::UiApp app;
	app.appUi().showProject();
	std::vector<std::string> asked;
	app.appUi().keepScriptsRunningWith(std::chrono::milliseconds(20),
		[&asked](const std::string& script)
		{
			asked.push_back(script);
			return asked.size() < 2;
		});
	writeScript(app, "runaway", "gnubase.track.add{type = \"audio\"}\nwhile true do end");
	app.keys().run("script.runaway", Json::object(), app.main(), nullptr);
	CHECK(asked == std::vector<std::string>{"runaway", "runaway"});
	CHECK(app.call("project.get").at("tracks").size() == 1);
	CHECK(app.call("history.get").at("steps").back().at("action") == "runaway");

	// A line typed into the console too, and the console's own Lua runs on after it.
	const ui::WindowId window = openConsole(app);
	REQUIRE(app.waitUntil([&app, window] { return output(app, window).find("ran longer than") != std::string::npos; }));
	asked.clear();
	app.ui().click(window, {"#script-prompt", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.ui().text(window, "while true do end");
	app.ui().key(window, "Enter");
	CHECK(asked == std::vector<std::string>{"Script Console", "Script Console"});
	app.ui().text(window, "print('goes on')");
	app.ui().key(window, "Enter");
	CHECK(app.waitUntil([&app, window] { return output(app, window).find("goes on") != std::string::npos; }));
	CHECK(asked.size() == 2);
}

TEST_CASE("a user script's undo step has what it sent through the UI, and not what the UI sent before", "[ui]")
{
	test::UiApp app;
	app.appUi().showProject();
	ui::EngineBinding& binding = app.appUi().binding();
	const auto addTrack = [](const char* type)
	{ return Json::array({Json{{"command", "track.add"}, {"parameters", {{"type", type}}}}}); };
	// A UI command whose second step waits for the first.
	app.keys().add({"test.twoSteps", "Adds two MIDI tracks in steps.",
		[&binding, &addTrack](ui::Window& /*window*/, Rml::Element* /*zone*/)
		{
			binding.sendStep(0, "test", "Add Track", addTrack("midi"));
			binding.sendStep(0, "test", "Add Track", addTrack("midi"));
		}});
	writeScript(app, "steps", "gnubase.ui.command{command = \"test.twoSteps\"}");

	// The user's steps: one in flight, one waiting, when the script starts.
	binding.sendStep(0, "user", "Add Track", addTrack("audio"));
	binding.sendStep(0, "user", "Add Track", addTrack("audio"));
	app.keys().run("script.steps", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 4; }));
	const Json steps = app.call("history.get").at("steps");
	REQUIRE(steps.size() == 3);
	CHECK(steps[0].at("action") == "Add Track");
	CHECK(steps[1].at("action") == "Add Track");
	CHECK(steps[2].at("action") == "steps");
	app.call("history.undo");
	CHECK(app.call("project.get").at("tracks").size() == 2);
}

TEST_CASE("the script console adds what is printed to its output, and drops what the output dropped", "[ui]")
{
	test::UiApp app;
	app.appUi().showProject();
	const ui::WindowId window = openConsole(app);
	const auto texts = [&app, window]
	{ return app.ui().dom(window, "#script-output", 1).at("elements").at(0).value("children", Json::array()).size(); };
	const auto print = [&app](const std::string& code)
	{
		CHECK(!app.lua().run(code, "print").has_value());
		app.shell().step(std::chrono::milliseconds(0));
	};
	print("print('one')");
	print("print('two')");
	CHECK(texts() == 2); // the first one stays as it is
	CHECK_THAT(output(app, window), ContainsSubstring("one") && ContainsSubstring("two"));

	app.ui().click(window, {"#script-clear", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(texts() == 0);

	// The last 64 KiB.
	for (int i = 0; i < 10; ++i)
		print("for _ = 1, 10 do print(string.rep('" + std::to_string(i) + "', 1023)) end");
	CHECK(texts() <= 8);
	const std::string shown = output(app, window);
	CHECK(shown.find('9') != std::string::npos);
	CHECK(shown.find('0') == std::string::npos);
}
