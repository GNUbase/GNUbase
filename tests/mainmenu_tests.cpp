// SPDX-License-Identifier: GPL-3.0-or-later
// The main menu bar (#392, ui/mainmenu.h): the project window's menus run engine and UI commands as keys do, show
// their keys and are disabled while their command can't run. The tests click through them with ui::Automation, as
// MCP's ui_click does. Items with a command that doesn't exist or parameters it doesn't take are refused (#446).
#include "engine/engine.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "ui/mainmenu.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
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

TEST_CASE("Transport > Audio Record Mode sets the transport's audio record mode", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#audio-record-mode-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#audio-record-mode-replace");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("audioRecordMode") == "replace"; }));
}

TEST_CASE("Transport > MIDI Record Mode sets the transport's MIDI record mode", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#midi-record-mode-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#midi-record-mode-merge");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("midiRecordMode") == "merge"; }));
}

TEST_CASE("Transport > MIDI Record Mode sets the transport's MIDI cycle record mode", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#midi-record-mode-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#midi-cycle-record-mode-stacked");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("midiCycleRecordMode") == "stacked"; }));
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

TEST_CASE("the menu bar's menus follow the mouse and Left and Right, Alt opens them, and submenus have arrows", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();

	click(app, "#menu-bar button[menu=file-menu]");
	REQUIRE(shown(app, "#file-menu"));
	CHECK(shown(app, "#import-submenu > .menu-arrow"));
	CHECK(elements(app, "menuitem[command=\"file.save\"] .menu-arrow").empty());
	app.ui().move(std::nullopt, {"#menu-bar button[menu=edit-menu]", 0, std::nullopt});
	CHECK(!shown(app, "#file-menu"));
	REQUIRE(shown(app, "#edit-menu"));
	app.ui().move(std::nullopt, {"#menu-bar", 0, ui::Point{0, 0}});
	app.ui().key(std::nullopt, "Right");
	CHECK(!shown(app, "#edit-menu"));
	CHECK(shown(app, "#media-menu"));
	app.ui().key(std::nullopt, "Left");
	CHECK(shown(app, "#edit-menu"));
	app.ui().key(std::nullopt, "Escape");
	CHECK(!shown(app, "#edit-menu"));

#ifndef __APPLE__
	app.ui().key(std::nullopt, "Alt");
	CHECK(shown(app, "#file-menu"));
	app.ui().key(std::nullopt, "Alt");
	CHECK(!shown(app, "#file-menu"));
#endif
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

TEST_CASE("the main menu refuses items with a command it doesn't know or parameters the command doesn't take (#446)",
	"[unit]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	ui::Window& window = fake.shell.open(ui::WindowSpec{});
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding);
	keys.add({"view.zoomIn", "Zooms in.", {}});
	const auto load = [&window](const std::string& items)
	{ return window.context().LoadDocumentFromMemory("<rml><body><menu>" + items + "</menu></body></rml>"); };
	const std::string good =
		R"(<menuitem command="transport.play">Start</menuitem>)"
		R"(<menuitem command="transport.locate" parameters='{"target": "leftLocator"}'>L</menuitem>)"
		R"(<menuitem command="view.zoomIn">Zoom In</menuitem>)";

	Rml::ElementDocument* document = load(good);
	REQUIRE(document != nullptr);
	CHECK_NOTHROW(std::make_unique<ui::MainMenu>(keys, window, *document));
	document->Close();

	struct Bad
	{
		const char* item;
		const char* named; // in the message
	};
	const Bad bad[] = {
		{R"(<menuitem command="transport.ply">Start</menuitem>)", "transport.ply"},
		{R"(<menuitem command="">Nothing</menuitem>)", "no command"},
		{R"(<menuitem command="transport.locate" parameters='{"target": 5}'>L</menuitem>)", "target"},
		{R"(<menuitem command="transport.locate" parameters='{"traget": "leftLocator"}'>L</menuitem>)", "traget"},
		{R"(<menuitem command="view.zoomIn" parameters='{"factor": 2}'>Zoom In</menuitem>)", "view.zoomIn"},
		{R"(<menuitem command="transport.play" parameters='{"target"'>Start</menuitem>)", "JSON object"},
		{R"(<menuitem command="transport.play" parameters='[]'>Start</menuitem>)", "JSON object"},
	};
	for (const Bad& item : bad)
	{
		INFO(item.item);
		document = load(good + item.item);
		REQUIRE(document != nullptr);
		CHECK_THROWS_AS(std::make_unique<ui::MainMenu>(keys, window, *document), std::invalid_argument);
		CHECK_THROWS_WITH(std::make_unique<ui::MainMenu>(keys, window, *document), ContainsSubstring(item.named));
		// The document is as it was: no item got its keys.
		CHECK(document->QuerySelector(".menu-keys") == nullptr);
		document->Close();
	}
}

TEST_CASE(
	"the command items of every shipped document name commands that exist, with parameters they take (#446)", "[ui]")
{
	test::UiApp app({900, 640});
	// The documents whose menus and buttons run commands attach without throwing: the project window's menu bar and
	// transport bar, the transport panel and the time display window.
	app.appUi().showProject();
	app.appUi().toggleTransportPanel();
	CHECK(app.appUi().transportPanel() != nullptr);
	app.appUi().toggleTimeDisplay();
	CHECK(app.appUi().timeDisplay() != nullptr);

	// And so does every item in the documents and fragments in resources/ui, whatever attaches them.
	const ui::KeyCommands& keys = app.keys();
	int items = 0;
	int calls = 0;
	const std::regex uiCommand(R"(ui\.command\s*\{\s*command\s*=\s*['"]([^'"]*)['"]\s*\})");
	for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "ui"))
	{
		if (entry.path().extension() != ".rml")
			continue;
		INFO(entry.path().filename().u8string());
		std::ifstream file(entry.path(), std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		const bool fragment = text.find("<rml") == std::string::npos; // such as transport.rml
		Rml::ElementDocument* document = app.main().context().LoadDocumentFromMemory(
			fragment ? std::string("<rml><body/></rml>") : text, entry.path().generic_u8string());
		REQUIRE(document != nullptr);
		if (fragment)
			document->SetInnerRML(text);
		Rml::ElementList found;
		document->QuerySelectorAll(found, "[command]");
		for (const Rml::Element* element : found)
		{
			const auto command = element->GetAttribute<Rml::String>("command", "");
			const auto parameters = element->GetAttribute<Rml::String>("parameters", "{}");
			INFO(command << " " << parameters);
			CHECK_NOTHROW(keys.check(command, Json::parse(parameters, nullptr, false)));
			++items;
		}
		document->Close();

		// Buttons that run a key command from Lua: gnubase.ui.command{command = 'project.addTrack'}.
		for (auto call = std::sregex_iterator(text.begin(), text.end(), uiCommand); call != std::sregex_iterator();
			++call)
		{
			const std::string command = (*call)[1].str();
			INFO(command);
			CHECK_NOTHROW(keys.check(command, Json::object()));
			++calls;
		}
	}
	CHECK(items > 100);
	CHECK(calls >= 3);
}
