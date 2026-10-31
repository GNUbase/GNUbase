// SPDX-License-Identifier: GPL-3.0-or-later
// The main menu bar (#392, ui/mainmenu.h): the project window's menus run engine and UI commands as keys do, show
// their keys and are disabled while their command can't run. The tests click through them with ui::Automation, as
// MCP's ui_click does.
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/keycommands.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
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

	bool shown(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		return found.size() == 1 && found[0].at("visible").get<bool>();
	}

	bool disabled(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).value("attributes", Json::object()).contains("disabled");
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// The keys a menu item shows.
	std::string keysOf(test::UiApp& app, const std::string& item)
	{
		const Json found = elements(app, item + " .menu-keys");
		return found.empty() ? std::string() : found[0].value("text", std::string());
	}

	std::string keyName(const char* keys)
	{
		return ui::keyName(ui::parseKeys(keys));
	}
}

TEST_CASE("Studio > Studio Setup in the main menu opens the studio setup dialog", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	CHECK(shown(app, "#menu-bar"));
	CHECK(!shown(app, "#studio-menu"));

	click(app, "#menu-bar button[menu=studio-menu]");
	REQUIRE(shown(app, "#studio-menu"));
	click(app, "menuitem[command=\"window.studioSetup\"]");
	REQUIRE(app.waitUntil([&app] { return !elements(app, "#studio-setup").empty(); }));
	CHECK(!shown(app, "#studio-menu")); // the click closed the menu
}

TEST_CASE("main menu items show their keys and run engine commands with their parameters", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.call("transport.set", {{"left", 48000}, {"right", 96000}});
	app.shell().step(std::chrono::milliseconds(0));

	CHECK(keysOf(app, "menuitem[command=\"history.undo\"]") == keyName("Mod+Z"));
	CHECK(keysOf(app, "menuitem[command=\"transport.toggle\"]") == "Space");
	CHECK(keysOf(app, "menuitem[command=\"edit.delete\"]") == "Delete"); // the first of its keys
	CHECK(keysOf(app, "menuitem[command=\"window.studioSetup\"]").empty());

	// Transport > Locators > Go to Right Locator Position: transport.locate with the item's parameters.
	CHECK(keysOf(app, "#locate-right-locator") == "Num2");
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#locators-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#locate-right-locator");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("position") == 96000; }));
	CHECK(app.call("engine.commandLog").at("commands").back().at("client") == "ui");

	// UI commands run in the window the menu is in: Edit > Select > All selects through the event display, in a
	// submenu that shows while the mouse is over its item.
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	app.call("midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 1920}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			auto* lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && !lanes->lanes().empty() && !lanes->lanes()[0].events.empty();
		}));
	click(app, "#menu-bar button[menu=edit-menu]");
	app.ui().move(std::nullopt, {"#select-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "menuitem[command=\"edit.selectAll\"]");
	click(app, "#menu-bar button[menu=edit-menu]");
	click(app, "menuitem[command=\"edit.delete\"]");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("midiParts").empty(); }));
}

TEST_CASE("main menu items are disabled while their command can't run", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));

	// Nothing to undo or redo in a new project.
	CHECK(disabled(app, "menuitem[command=\"history.undo\"]"));
	CHECK(disabled(app, "menuitem[command=\"history.redo\"]"));
	CHECK(!disabled(app, "menuitem[command=\"transport.toggle\"]"));

	app.call("track.add", {{"type", "audio"}});
	REQUIRE(app.waitUntil([&app] { return !disabled(app, "menuitem[command=\"history.undo\"]"); }));
	CHECK(disabled(app, "menuitem[command=\"history.redo\"]"));

	click(app, "#menu-bar button[menu=edit-menu]");
	click(app, "menuitem[command=\"history.undo\"]");
	REQUIRE(app.waitUntil([&app] { return !disabled(app, "menuitem[command=\"history.redo\"]"); }));
	CHECK(disabled(app, "menuitem[command=\"history.undo\"]"));
	CHECK(app.call("project.get").at("tracks").empty());

	// A click on a disabled item does nothing, and the menu stays open.
	click(app, "#menu-bar button[menu=edit-menu]");
	click(app, "menuitem[command=\"history.undo\"]");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.call("project.get").at("tracks").empty());
	CHECK(!disabled(app, "menuitem[command=\"history.redo\"]"));
}

TEST_CASE("key commands say whether a command can run, for menu items", "[ui]")
{
	test::UiApp app;
	ui::KeyCommands& keys = app.keys();
	bool enabled = false;
	keys.add({"test.command", "A test command.", {}});
	CHECK(keys.canRun("test.command", app.main(), nullptr));
	CHECK(keys.canRun("transport.play", app.main(), nullptr));
	CHECK(!keys.canRun("test.missing", app.main(), nullptr));

	keys.enableWhen("test.command", [&enabled](ui::Window& /*window*/, Rml::Element* /*zone*/) { return enabled; });
	CHECK(!keys.canRun("test.command", app.main(), nullptr));
	enabled = true;
	CHECK(keys.canRun("test.command", app.main(), nullptr));
	CHECK_THROWS(keys.enableWhen("test.missing", {}));
}

TEST_CASE("the menu bar shows and hides like the project window's strips, and File > Quit quits", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();

	// Show Menu Bar, in the Set up Window Layout menu.
	click(app, "#window-layout");
	click(app, "menuitem[toggle=menuBar]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#menu-bar"); }));
	app.keys().run("window.menuBar", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#menu-bar"); }));

	CHECK(keysOf(app, "menuitem[command=\"file.quit\"]") == keyName("Mod+Q"));
	click(app, "#menu-bar button[menu=file-menu]");
	click(app, "menuitem[command=\"file.quit\"]");
	REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
}
