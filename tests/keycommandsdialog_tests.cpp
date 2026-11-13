// SPDX-License-Identifier: GPL-3.0-or-later
// The key commands dialog and macros (#161, docs/features/key-commands.md): the user's key commands and macros in the
// user settings, which the key commands follow, their presets and files in the engine, and the dialog that edits them
// in the app's UI, driven with ui::Automation as MCP's UI tools drive it.
#include "uiapp.h"

#include "engine/engine.h"
#include "platform/files.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/keycommands.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;
using ui::Platform;
using Strings = std::vector<std::string>;

namespace
{
	Json run(engine::Client& client, const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(engine::Client& client, const std::string& command, Json parameters)
	{
		const engine::Result result = client.call(command, std::move(parameters));
		REQUIRE(!result.ok());
		return result.error;
	}

	Json setting(engine::Client& client, const std::string& key)
	{
		return run(client, "settings.get", {{"key", key}}).at("value");
	}

	// The entry of commands() for the command with these parameters.
	Json entryOf(const Json& commands, const std::string& command, const Json& parameters = Json::object())
	{
		for (const Json& entry : commands)
			if (entry.at("command") == command && entry.at("parameters") == parameters)
				return entry;
		return nullptr;
	}

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		INFO(selector);
		REQUIRE(found.size() == 1);
		return found[0];
	}

	std::string text(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).value("text", std::string());
	}

	void click(test::UiApp& app, const std::string& selector, const Strings& modifiers = {})
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, modifiers);
		app.shell().step(std::chrono::milliseconds(0));
	}

	// A double click, which RmlUi reports as a dblclick event (Automation::click holds its clock still for it).
	void doubleClick(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 2, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	void type(test::UiApp& app, const std::string& selector, const std::string& text)
	{
		click(app, selector);
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, text);
		app.shell().step(std::chrono::milliseconds(0));
	}

	void press(test::UiApp& app, const std::string& keys)
	{
		app.ui().key(std::nullopt, keys);
		app.shell().step(std::chrono::milliseconds(0));
	}

	// Opens a drop-down list, moves the selection that many options down with the arrow keys, and closes it.
	void choose(test::UiApp& app, const std::string& select, int down)
	{
		click(app, select);
		for (int i = 0; i < std::abs(down); ++i)
			app.ui().key(std::nullopt, down > 0 ? "Down" : "Up");
		click(app, select);
	}

	bool open(test::UiApp& app)
	{
		return !elements(app, "#key-commands").empty();
	}

	void openDialog(test::UiApp& app)
	{
		app.keys().run("window.keyCommands", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return open(app); }));
		app.shell().step(std::chrono::milliseconds(0));
	}

	bool closed(test::UiApp& app)
	{
		return app.waitUntil([&app] { return !open(app); });
	}

	// The command rows that the list shows, by their kc-command attribute.
	Strings shownCommands(test::UiApp& app)
	{
		Strings shown;
		for (const Json& row : elements(app, "#keycommands-list listitem.command"))
			if (row.at("visible") == true)
				shown.push_back(row.at("attributes").at("kc-command").get<std::string>());
		return shown;
	}

	// Searches for a command, or for search, and selects the first row of the command.
	void select(test::UiApp& app, const std::string& command, const std::string& search = {})
	{
		type(app, "#keycommands-search", search.empty() ? command : search);
		for (const Json& row : elements(app, "#keycommands-list listitem.command"))
			if (row.at("attributes").at("kc-command") == command && row.at("visible") == true)
			{
				click(app, "#" + row.at("id").get<std::string>());
				return;
			}
		FAIL("no row for " << command);
	}

	// The keys the dialog shows for the selected command.
	Strings shownKeys(test::UiApp& app)
	{
		Strings keys;
		for (const Json& row : elements(app, "#keycommands-keys listitem"))
			keys.push_back(row.value("text", std::string()));
		return keys;
	}

	// A key as this platform shows it: "Mod+Z" is Ctrl+Z on Windows and Linux and Cmd+Z on macOS, Alt is Opt there.
	std::string shown(const std::string& keys)
	{
		return ui::keyName(ui::parseKeys(keys));
	}

	// Presses keys into the dialog's key field, which assigns them to the selected command.
	void assign(test::UiApp& app, const std::string& keys)
	{
		click(app, "#keycommands-assign");
		press(app, keys);
	}

	void ok(test::UiApp& app)
	{
		click(app, "#keycommands-ok");
		REQUIRE(closed(app));
	}

	// Clicks the confirmation dialog's OK button (confirm.rml), which Remove Key Command, Defaults and the tool
	// modifiers' Assign open before they change anything.
	void confirm(test::UiApp& app)
	{
		REQUIRE(app.waitUntil([&app] { return !elements(app, "#confirm-ok").empty(); }));
		click(app, "#confirm-ok");
	}

	// Clicks the confirmation dialog's Cancel, which leaves everything as it was.
	void cancelConfirm(test::UiApp& app)
	{
		REQUIRE(app.waitUntil([&app] { return !elements(app, "#confirm-cancel").empty(); }));
		click(app, "#confirm-cancel");
		REQUIRE(app.waitUntil([&app] { return elements(app, "#confirm-dialog").empty(); }));
	}
}

TEST_CASE("keys are written for the user settings the same way on every platform", "[unit]")
{
	CHECK(ui::keySetting(ui::parseKeys("Ctrl+Shift+K", Platform::Windows), Platform::Windows) == "Mod+Shift+K");
	CHECK(ui::keySetting(ui::parseKeys("Cmd+Shift+K", Platform::Mac), Platform::Mac) == "Mod+Shift+K");
	CHECK(ui::keySetting(ui::parseKeys("Ctrl+K", Platform::Mac), Platform::Mac) == "Ctrl+K");
	CHECK(ui::keySetting(ui::parseKeys("Shift+Alt+Meta+F5", Platform::Linux), Platform::Linux) == "Alt+Shift+Meta+F5");
	CHECK(ui::keySetting(ui::parseKeys("Alt", Platform::Windows), Platform::Windows) == "Alt");
	for (const Platform platform : {Platform::Windows, Platform::Mac, Platform::Linux})
		for (const char* keys : {"Space", "Mod+Num1", "Mod+Shift+Z", "Mod+Alt+Num6", "Shift+Num/"})
		{
			INFO(keys);
			CHECK(ui::keySetting(ui::parseKeys(keys, platform), platform) == keys);
		}
}

TEST_CASE("the user's key commands replace a command's default keys, and macros run as commands", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding, Platform::Windows);
	keys.add({"test.first", "A command of the UI's own.", {}});
	keys.bind({"F7", "test.first", Json::object(), "project"});

	// All its keys, in the zone of its first default binding.
	keys.customize(Json::array({{{"command", "transport.toggle"}, {"keys", {"Mod+Shift+P", "F8"}}},
					   {{"command", "test.first"}, {"keys", {"F9"}}}}),
		Json::array());
	CHECK(keys.keysFor("transport.toggle") == Strings{"Ctrl+Shift+P", "F8"});
	CHECK(keys.keysFor("test.first") == Strings{"F9"});
	// Others keep theirs; a key taken from another command in the same zone is gone there.
	CHECK(keys.keysFor("transport.stop") == Strings{"Num0"});
	keys.customize(Json::array({{{"command", "history.redo"}, {"parameters", Json::object()}, {"keys", {"Mod+Z"}}}}),
		Json::array());
	CHECK(keys.keysFor("history.undo").empty());
	CHECK(keys.keysFor("history.redo") == Strings{"Ctrl+Z"});
	CHECK(keys.keysFor("transport.toggle") == Strings{"Space"}); // back to its defaults
	// Without keys a command has none; with parameters it is that binding only.
	keys.customize(
		Json::array({{{"command", "transport.toggle"}, {"parameters", {{"setting", "cycle"}}}}}), Json::array());
	CHECK(keys.keysFor("transport.toggle", {{"setting", "cycle"}}).empty());
	CHECK(keys.keysFor("transport.toggle") == Strings{"Space"});

	// What doesn't fit is left out, the rest stays.
	keys.customize(
		Json::array({{{"command", "no.such"}, {"keys", {"F7"}}}, {{"command", "marker.add"}, {"keys", {"Nope"}}},
			{{"command", "transport.stop"}, {"keys", {"F11"}}}}),
		Json::array());
	CHECK(keys.keysFor("marker.add") == Strings{"Insert"});
	CHECK(keys.keysFor("transport.stop") == Strings{"F11"});

	// Macros are commands that keys and menus run, as long as they exist.
	const Json macros = Json::array({{{"name", "Stop and Back"},
		{"commands",
			{{{"command", "transport.stop"}},
				{{"command", "transport.locate"}, {"parameters", {{"target", "projectStart"}}}}}}}});
	keys.customize(Json::array({{{"command", "macro.Stop and Back"}, {"keys", {"Mod+Alt+B"}}}}), macros);
	CHECK(keys.keysFor("macro.Stop and Back") == Strings{"Ctrl+Alt+B"});
	CHECK_NOTHROW(keys.check("macro.Stop and Back", Json::object()));
	CHECK_THROWS_AS(keys.check("macro.Other", Json::object()), std::invalid_argument);
	CHECK_THROWS_AS(keys.add({"macro.Mine", "A UI command can't be a macro.", {}}), std::invalid_argument);
	keys.customize(Json::array(), Json::array());
	CHECK_THROWS_AS(keys.check("macro.Stop and Back", Json::object()), std::invalid_argument);
	CHECK(keys.keysFor("history.undo") == Strings{"Ctrl+Z"});
}

TEST_CASE("the key commands list every command a key can run with its default keys", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding, Platform::Mac);
	keys.add({"test.first", "A command of the UI's own.", {}});
	keys.bind({"F7", "test.first", Json::object(), "project"});
	const Json commands = keys.commands();
	CHECK(entryOf(commands, "test.first") ==
		Json({{"command", "test.first"}, {"parameters", Json::object()}, {"description", "A command of the UI's own."},
			{"zone", "project"}, {"keys", {"F7"}}}));
	CHECK(entryOf(commands, "transport.toggle", {{"setting", "cycle"}}).at("keys") == Json({"Num/"}));
	CHECK(entryOf(commands, "transport.set", {{"left", "cursor"}}).at("keys") == Json({"Mod+Num1"}));
	CHECK(entryOf(commands, "history.undo").at("keys") == Json({"Mod+Z"}));
	CHECK(entryOf(commands, "track.add").is_null());   // needs parameters
	CHECK(entryOf(commands, "project.get").is_null()); // only reads
	// Commands that need one of their parameters, though none is required, aren't listed without (#971).
	CHECK(entryOf(commands, "chordPads.trigger").is_null());
	CHECK(entryOf(commands, "marker.locate").is_null());
	CHECK(entryOf(commands, "marker.setLocators").is_null());
	CHECK_FALSE(entryOf(commands, "transport.play").is_null());
}

TEST_CASE("key commands presets and files are kept by the engine", "[unit]")
{
	// Reference manual p. 1559: presets of the key commands and macros, and files to take them to another computer.
	const test::TempDir dir;
	engine::Engine engine(audio::makeBackends(), dir.path());
	engine::Client client(engine);
	const Json mine = Json::array({{{"command", "transport.stop"}, {"keys", {"F11"}}}});
	const Json macros = Json::array({{{"name", "Stop"}, {"commands", {{{"command", "transport.stop"}}}}}});
	run(client, "settings.set", {{"values", {{"ui.keyCommands", mine}, {"ui.macros", macros}}}});
	CHECK_THAT(reject(client, "settings.set", {{"key", "ui.keyCommands"}, {"value", {{{"keys", {"F1"}}}}}}),
		ContainsSubstring("command"));

	// A preset holds the settings' key commands and macros, or those given.
	run(client, "keyCommands.savePreset", {{"name", "Mine"}});
	run(client, "keyCommands.savePreset",
		{{"name", "Empty"}, {"keyCommands", Json::array()}, {"macros", Json::array()}});
	CHECK(setting(client, "ui.keyCommandPresets") ==
		Json({{"Mine", {{"keyCommands", mine}, {"macros", macros}}},
			{"Empty", {{"keyCommands", Json::array()}, {"macros", Json::array()}}}}));
	CHECK_THAT(reject(client, "keyCommands.savePreset", {{"name", ""}}), ContainsSubstring("name"));
	CHECK_THAT(reject(client, "keyCommands.savePreset", {{"name", "Bad"}, {"keyCommands", {{{"keys", "F1"}}}}}),
		ContainsSubstring("preset"));
	run(client, "keyCommands.removePreset", {{"name", "Empty"}});
	CHECK_THAT(reject(client, "keyCommands.removePreset", {{"name", "Empty"}}), ContainsSubstring("Empty"));
	CHECK(setting(client, "ui.keyCommandPresets").size() == 1);

	// A file of them, read back as a preset.
	const std::string file = (dir.path() / "keys.json").u8string();
	run(client, "keyCommands.export", {{"path", file}});
	run(client, "keyCommands.import", {{"path", file}, {"name", "Imported"}});
	CHECK(setting(client, "ui.keyCommandPresets").at("Imported") == Json({{"keyCommands", mine}, {"macros", macros}}));
	{
		std::ofstream other(dir.path() / "other.json");
		other << R"({"version": 1})";
	}
	CHECK_THAT(reject(client, "keyCommands.import", {{"path", (dir.path() / "other.json").u8string()}, {"name", "X"}}),
		ContainsSubstring("no key commands file"));
	// A format or version of another type is the readable message, not the JSON library's type error.
	{
		std::ofstream format(dir.path() / "format.json");
		format << R"({"format": 1, "version": 1})";
		std::ofstream version(dir.path() / "version.json");
		version << R"({"format": "gnubase.keyCommands", "version": "1"})";
	}
	CHECK_THAT(reject(client, "keyCommands.import", {{"path", (dir.path() / "format.json").u8string()}, {"name", "X"}}),
		ContainsSubstring("no key commands file"));
	CHECK_THAT(
		reject(client, "keyCommands.import", {{"path", (dir.path() / "version.json").u8string()}, {"name", "X"}}),
		ContainsSubstring("another version"));
	CHECK_THAT(reject(client, "keyCommands.import", {{"path", (dir.path() / "none.json").u8string()}, {"name", "X"}}),
		ContainsSubstring("can't read"));
}

TEST_CASE("the user's key commands and macros take effect in the app and the Edit menu", "[ui]")
{
	test::UiApp app({1000, 760});
	app.appUi().showProject();
	// Each menu item shows its keys once, also after the menu was built again for the macros.
	const auto menuKeys = [&app]
	{
		Strings shown;
		for (const Json& found : elements(app, "menuitem[command=\"transport.toggle\"] .menu-keys"))
			shown.push_back(found.value("text", std::string()));
		return shown;
	};
	CHECK(menuKeys() == Strings{"Space"});
	CHECK(elements(app, "menuitem[command] .menu-keys").size() == elements(app, "menuitem[command]").size());
	app.call("settings.set",
		{{"values",
			{{"ui.keyCommands", {{{"command", "transport.toggle"}, {"keys", {"F11"}}}}},
				{"ui.macros", {{{"name", "Play"}, {"commands", {{{"command", "transport.play"}}}}}}}}}});
	REQUIRE(app.waitUntil([&app] { return app.keys().keysFor("transport.toggle") == Strings{"F11"}; }));
	CHECK(app.waitUntil([&menuKeys] { return menuKeys() == Strings{"F11"}; }));
	CHECK(elements(app, "menuitem[command] .menu-keys").size() == elements(app, "menuitem[command]").size());
	press(app, "F11");
	CHECK(app.waitUntil([&app] { return app.call("transport.get").at("playing") == true; }));
	app.call("transport.stop");
	// Edit > Macros runs the macro.
	REQUIRE(elements(app, "#macros-menu menuitem[command=\"macro.Play\"]").size() == 1);
	app.keys().run("macro.Play", Json::object(), app.main(), nullptr);
	CHECK(app.waitUntil([&app] { return app.call("transport.get").at("playing") == true; }));
	app.call("transport.stop");

	// The next start keeps them.
	app.restart();
	CHECK(app.keys().keysFor("transport.toggle") == Strings{"F11"});
	CHECK_NOTHROW(app.keys().check("macro.Play", Json::object()));
}

TEST_CASE("the key commands dialog finds, assigns, removes and resets keys", "[ui]")
{
	test::UiApp app({1000, 760});
	openDialog(app);

	// Searching by name filters the list; the categories are the commands' areas.
	type(app, "#keycommands-search", "transport.st");
	const Strings found = shownCommands(app);
	CHECK(std::find(found.begin(), found.end(), "transport.stop") != found.end());
	CHECK(std::find(found.begin(), found.end(), "history.undo") == found.end());
	CHECK(element(app, "#kc-area-transport").at("visible") == true);
	click(app, "#keycommands-reset-search");
	CHECK(shownCommands(app).empty()); // the categories are collapsed
	click(app, "#keycommands-expand");
	CHECK(shownCommands(app).size() > found.size());

	// A key pressed into the key field is added to the selected command's keys.
	select(app, "transport.stop");
	CHECK(shownKeys(app) == Strings{"Num0"});
	assign(app, "Mod+Shift+F12");
	CHECK(shownKeys(app) == Strings{"Num0", shown("Mod+Shift+F12")});
	// A key another command has moves to this one.
	select(app, "history.redo");
	assign(app, "Mod+Z");
	CHECK_THAT(text(app, "#keycommands-message"), ContainsSubstring("history.undo"));
	ok(app);
	CHECK(app.call("settings.get", {{"key", "ui.keyCommands"}}).at("value") ==
		Json::array({{{"command", "history.redo"}, {"keys", {"Mod+Shift+Z", "Mod+Z"}}}, {{"command", "history.undo"}},
			{{"command", "transport.stop"}, {"keys", {"Num0", "Mod+Shift+F12"}}}}));
	REQUIRE(app.waitUntil([&app] { return app.keys().keysFor("transport.stop").size() == 2; }));
	CHECK(app.keys().keysFor("history.undo").empty());

	// Press Key: the commands with the key pressed.
	openDialog(app);
	click(app, "#keycommands-press-key");
	click(app, "#keycommands-key-search");
	press(app, "Mod+Shift+F12");
	CHECK(shownCommands(app) == Strings{"transport.stop"});
	click(app, "#keycommands-press-key");

	// The filters: customized, assigned and unassigned commands.
	choose(app, "#keycommands-filter", 2); // Customized
	CHECK(shownCommands(app) == Strings{"history.redo", "history.undo", "transport.stop"});
	choose(app, "#keycommands-filter", -1); // Assigned
	Strings assigned = shownCommands(app);
	CHECK(std::find(assigned.begin(), assigned.end(), "history.undo") == assigned.end());
	CHECK(std::find(assigned.begin(), assigned.end(), "transport.stop") != assigned.end());
	choose(app, "#keycommands-filter", 2); // Unassigned
	Strings unassigned = shownCommands(app);
	CHECK(std::find(unassigned.begin(), unassigned.end(), "history.undo") != unassigned.end());
	choose(app, "#keycommands-filter", -3); // All

	// Remove takes away the selected key, but asks first; Cancel keeps it. Reset brings back the defaults.
	select(app, "transport.stop");
	click(app, "#keycommands-keys listitem:last-child");
	click(app, "#keycommands-remove");
	cancelConfirm(app);
	CHECK(shownKeys(app) == Strings{"Num0", shown("Mod+Shift+F12")});
	click(app, "#keycommands-remove");
	confirm(app);
	CHECK(shownKeys(app) == Strings{"Num0"});
	select(app, "history.undo");
	click(app, "#keycommands-reset");
	CHECK(shownKeys(app) == Strings{shown("Mod+Z")});
	ok(app);
	REQUIRE(app.waitUntil([&app] { return app.keys().keysFor("transport.stop") == Strings{"Num0"}; }));
	CHECK(app.keys().keysFor("history.undo") == Strings{shown("Mod+Z")});

	// Clear All removes every key, also of an entry the dialog doesn't list (a command with parameters no default
	// binding has); Defaults brings them back, but asks first, and Cancel drops what changed.
	app.call("settings.set",
		{{"values",
			{{"ui.keyCommands",
				Json::array({{{"command", "transport.locate"}, {"parameters", {{"target", "projectEnd"}}},
					{"keys", {"F13"}}}})}}}});
	REQUIRE(app.waitUntil(
		[&app] { return app.keys().keysFor("transport.locate", {{"target", "projectEnd"}}) == Strings{"F13"}; }));
	openDialog(app);
	click(app, "#keycommands-clear-all");
	select(app, "transport.toggle");
	CHECK(shownKeys(app).empty());
	ok(app);
	// Clear All removed the unlisted entry's keys too.
	REQUIRE(
		app.waitUntil([&app] { return app.keys().keysFor("transport.locate", {{"target", "projectEnd"}}).empty(); }));
	openDialog(app);
	click(app, "#keycommands-defaults");
	cancelConfirm(app);
	select(app, "transport.toggle");
	CHECK(shownKeys(app).empty()); // Cancel left it cleared
	click(app, "#keycommands-defaults");
	confirm(app);
	ok(app);
	CHECK(app.call("settings.get", {{"key", "ui.keyCommands"}}).at("value") == Json::array());
	CHECK(app.waitUntil([&app] { return app.keys().keysFor("history.redo") == Strings{shown("Mod+Shift+Z")}; }));
}

TEST_CASE("the key commands dialog sets up macros", "[ui]")
{
	test::UiApp app({1000, 760});
	app.appUi().showProject();
	openDialog(app);
	click(app, "#keycommands-show-macros");
	click(app, "#keycommands-new-macro");
	type(app, "#keycommands-macro-name", "Restart");
	click(app, "#keycommands-rename-macro");
	select(app, "transport.locate", "transport.locate (target: leftLocator)");
	CHECK(shownKeys(app) == Strings{"Num1"});
	click(app, "#keycommands-add-command");
	select(app, "transport.stop");
	click(app, "#keycommands-add-command");
	select(app, "transport.play");
	click(app, "#keycommands-add-command");
	CHECK(elements(app, "#keycommands-macro-commands listitem").size() == 3);
	// Commands go after the selected one, and move up and down.
	click(app, "#keycommands-macro-commands listitem:first-child");
	click(app, "#keycommands-command-down");
	click(app, "#keycommands-macro-commands listitem:last-child");
	click(app, "#keycommands-remove-command");
	// The macro is a command in the list, and gets keys.
	select(app, "macro.Restart");
	assign(app, "Mod+Alt+R");
	click(app, "#keycommands-duplicate-macro");
	click(app, "#keycommands-delete-macro");
	ok(app);
	const Json macros = app.call("settings.get", {{"key", "ui.macros"}}).at("value");
	REQUIRE(macros.size() == 1);
	CHECK(macros[0].at("name") == "Restart");
	CHECK(macros[0].at("commands") ==
		Json::array({{{"command", "transport.stop"}},
			{{"command", "transport.locate"}, {"parameters", {{"target", "leftLocator"}}}}}));
	REQUIRE(app.waitUntil([&app] { return app.keys().keysFor("macro.Restart") == Strings{shown("Mod+Alt+R")}; }));

	// Run Macro runs what the dialog shows; Edit > Macros has it.
	app.call("transport.play");
	openDialog(app);
	click(app, "#keycommands-show-macros");
	click(app, "#keycommands-macros listitem:first-child");
	click(app, "#keycommands-run-macro");
	CHECK(app.waitUntil([&app] { return app.call("transport.get").at("playing") == false; }));
	click(app, "#keycommands-cancel");
	CHECK(elements(app, "#macros-menu menuitem[command=\"macro.Restart\"]").size() == 1);

	// A deleted macro's keys go with it: a new macro of its name has none.
	openDialog(app);
	click(app, "#keycommands-show-macros");
	click(app, "#keycommands-new-macro");
	select(app, "macro.Macro 1");
	assign(app, "Mod+Alt+M");
	click(app, "#keycommands-delete-macro");
	click(app, "#keycommands-new-macro");
	select(app, "macro.Macro 1");
	CHECK(shownKeys(app).empty());
	assign(app, "Mod+Alt+M");
	ok(app);
	REQUIRE(app.waitUntil([&app] { return app.keys().keysFor("macro.Macro 1") == Strings{shown("Mod+Alt+M")}; }));
	openDialog(app);
	click(app, "#keycommands-show-macros");
	click(app, "#keycommands-macros listitem:last-child");
	click(app, "#keycommands-delete-macro");
	ok(app);
	const Json saved = app.call("settings.get", {{"key", "ui.keyCommands"}}).at("value");
	for (const Json& entry : saved)
		CHECK(entry.at("command") != "macro.Macro 1");
	openDialog(app);
	click(app, "#keycommands-show-macros");
	click(app, "#keycommands-new-macro");
	select(app, "macro.Macro 1");
	CHECK(shownKeys(app).empty());
	click(app, "#keycommands-cancel");
	REQUIRE(closed(app));
}

TEST_CASE("a macro is renamed by double-clicking it in the macros list", "[ui]")
{
	// Reference manual p. 1558: double-click the macro, enter a name for it, and press Return to confirm it.
	test::UiApp app({1000, 760});
	app.appUi().showProject();
	openDialog(app);
	click(app, "#keycommands-show-macros");
	click(app, "#keycommands-new-macro"); // Macro 1, selected
	REQUIRE(elements(app, "#keycommands-macros listitem").size() == 1);
	// Double-clicking it focuses its name field, so selecting the name, typing and Rename Macro rename it. (That the
	// typed text lands in the field shows the double-click focused it.)
	doubleClick(app, "#keycommands-macros listitem");
	CHECK(element(app, "#keycommands-macro-name").at("value") == "Macro 1");
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "Restart");
	click(app, "#keycommands-rename-macro");
	CHECK(element(app, "#keycommands-macros listitem").value("text", std::string()) == "Restart");
	ok(app);
	const Json macros = app.call("settings.get", {{"key", "ui.macros"}}).at("value");
	REQUIRE(macros.size() == 1);
	CHECK(macros[0].at("name") == "Restart");
}

TEST_CASE("the key commands dialog saves, loads, deletes, exports and imports presets", "[ui]")
{
	test::UiApp app({1000, 760});
	app.shell().showOsFileDialogs(false);
	openDialog(app);
	select(app, "transport.stop");
	assign(app, "F12");
	type(app, "#keycommands-preset-name", "Mine");
	click(app, "#keycommands-save-preset"); // applies, then saves
	const Json mine = Json::array({{{"command", "transport.stop"}, {"keys", {"Num0", "F12"}}}});
	CHECK(app.call("settings.get", {{"key", "ui.keyCommands"}}).at("value") == mine);
	CHECK(app.call("settings.get", {{"key", "ui.keyCommandPresets"}}).at("value") ==
		Json({{"Mine", {{"keyCommands", mine}, {"macros", Json::array()}}}}));
	CHECK(element(app, "#keycommands-preset").at("value") == "Mine");
	click(app, "#keycommands-defaults");
	confirm(app);
	ok(app);
	CHECK(app.call("settings.get", {{"key", "ui.keyCommands"}}).at("value") == Json::array());

	// Loading a preset replaces the key commands and macros in the dialog; OK applies them.
	openDialog(app);
	choose(app, "#keycommands-preset", 1);
	select(app, "transport.stop");
	CHECK(shownKeys(app) == Strings{"Num0", "F12"});
	ok(app);
	CHECK(app.call("settings.get", {{"key", "ui.keyCommands"}}).at("value") == mine);

	// Export and Import through the file dialogs.
	const test::TempDir dir;
	const auto answer = [&app](const std::filesystem::path& file)
	{
		REQUIRE(app.waitUntil([&app] { return !app.shell().fileDialogs().empty(); }));
		app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {file});
	};
	openDialog(app);
	click(app, "#keycommands-export");
	answer(dir.path() / "Studio.json");
	REQUIRE(app.waitUntil([&dir] { return std::filesystem::exists(dir.path() / "Studio.json"); }));
	click(app, "#keycommands-import");
	answer(dir.path() / "Studio.json");
	REQUIRE(app.waitUntil(
		[&app] { return app.call("settings.get", {{"key", "ui.keyCommandPresets"}}).at("value").contains("Studio"); }));
	app.shell().step(std::chrono::milliseconds(0)); // the presets list follows

	choose(app, "#keycommands-preset", 1); // Mine
	click(app, "#keycommands-delete-preset");
	CHECK(app.call("settings.get", {{"key", "ui.keyCommandPresets"}}).at("value").size() == 1);
	click(app, "#keycommands-cancel");
	CHECK(closed(app));
}

TEST_CASE("File > Export > Key Command Assignments writes an HTML list of every key", "[ui]")
{
	// Reference manual pp. 1559-1560: a file that lists all key command assignments, exported as .html.
	test::UiApp app({1000, 760});
	app.appUi().showProject();
	app.shell().showOsFileDialogs(false);
	// The menu item is there, and a customized key shows up as the platform writes it.
	REQUIRE(elements(app, "menuitem[command=\"keyCommands.exportAssignments\"]").size() == 1);
	app.call("settings.set",
		{{"values", {{"ui.keyCommands", Json::array({{{"command", "transport.stop"}, {"keys", {"F11"}}}})}}}});
	REQUIRE(app.waitUntil([&app] { return app.keys().keysFor("transport.stop") == Strings{"F11"}; }));

	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "assignments";
	app.keys().run("keyCommands.exportAssignments", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return !app.shell().fileDialogs().empty(); }));
	CHECK(app.shell().fileDialogs().begin()->second.kind == ui::FileDialogKind::Save);
	app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {file});
	// A file without an extension gets .html.
	const std::filesystem::path written = dir.path() / "assignments.html";
	REQUIRE(app.waitUntil([&written] { return std::filesystem::exists(written); }));

	const std::optional<std::string> html = files::readFile(written);
	REQUIRE(html.has_value());
	CHECK_THAT(html.value(), ContainsSubstring("<title>Key Command Assignments</title>"));
	// Every assignment: the category, the command and the keys, including the customized one and a default.
	CHECK_THAT(html.value(), ContainsSubstring("<td>transport</td>"));
	CHECK_THAT(html.value(), ContainsSubstring("transport.stop"));
	CHECK_THAT(html.value(), ContainsSubstring("F11"));
	CHECK_THAT(html.value(), ContainsSubstring("Space")); // transport.toggle's default key
	CHECK_THAT(html.value(), ContainsSubstring("</html>"));
}

TEST_CASE("tool modifiers are assigned by holding the keys and clicking Assign", "[ui]")
{
	// Reference manual p. 1574.
	test::UiApp app({1000, 760});
	app.keys().run("window.preferences", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return !elements(app, "#preferences").empty(); }));
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#pages-editing-tool-modifiers");
	click(app, "#pref-tool-assign-dragDrop-copy", {"ctrl", "shift"});
	CHECK(element(app, "#pref-tool-dragDrop-copy").at("value") == "ctrl+shift");
	// Modifiers another action of the category has need a confirmation; Cancel keeps both as they are.
	click(app, "#pref-tool-assign-dragDrop-sharedCopy", {"ctrl", "shift"});
	cancelConfirm(app);
	CHECK(element(app, "#pref-tool-dragDrop-copy").at("value") == "ctrl+shift");
	CHECK(element(app, "#pref-tool-dragDrop-sharedCopy").at("value") != "ctrl+shift");
	// Overwrite takes them from the other action, which is left without any.
	click(app, "#pref-tool-assign-dragDrop-sharedCopy", {"ctrl", "shift"});
	confirm(app);
	CHECK(element(app, "#pref-tool-dragDrop-sharedCopy").at("value") == "ctrl+shift");
	CHECK(element(app, "#pref-tool-dragDrop-copy").at("value") == "");
	click(app, "#preferences-ok");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#preferences").empty(); }));
	CHECK(app.call("settings.get", {{"key", "ui.toolModifiers"}}).at("value") ==
		Json({{"dragDrop.copy", ""}, {"dragDrop.sharedCopy", "ctrl+shift"}}));
}
