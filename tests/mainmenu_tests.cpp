// SPDX-License-Identifier: GPL-3.0-or-later
// The main menu bar (#392, ui/mainmenu.h): the project window's menus run engine and UI commands as keys do, show
// their keys and are disabled while their command can't run. The tests click through them with ui::Automation, as
// MCP's ui_click does. Items with a command that doesn't exist or parameters it doesn't take are refused (#446), and
// the transport windows whose controls are refused close again (#669).
#include "engine/engine.h"
#include "testutils/fakeui.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "ui/mainmenu.h"
#include "ui/rmlui.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/SystemInterface.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstring>
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
	Json located;
	const Json log = app.call("engine.commandLog");
	for (const Json& entry : log.at("commands"))
		if (entry.at("command") == "transport.locate")
			located = entry;
	REQUIRE(!located.is_null());
	CHECK(located.at("client") == "ui");

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

// What a release and an item's own command change shows in the DOM that MCP's UI tools read right after the input,
// without a step of the shell (#1173): the event display selects when the mouse button is released, and Edit >
// Select changes what the items for selected events can do.
TEST_CASE(
	"the main menu's items follow a release's selection and an item's own command without a step of the shell", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	app.call("midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 1920}});
	ui::EventLanes* lanes = nullptr;
	REQUIRE(app.waitUntil(
		[&app, &lanes]
		{
			lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && !lanes->lanes().empty() && !lanes->lanes()[0].events.empty();
		}));
	const std::string quantize = "menuitem[command=\"edit.quantize\"]";
	REQUIRE(disabled(app, quantize));
	const auto select = [&app](const char* command)
	{
		click(app, "#menu-bar button[menu=edit-menu]");
		app.ui().move(std::nullopt, {"#select-submenu", 0, std::nullopt});
		app.shell().step(std::chrono::milliseconds(0));
		app.ui().click(std::nullopt, {std::string("menuitem[command=\"") + command + "\"]", 0, std::nullopt},
			ui::MouseButton::Left, 1, {});
	};

	// Edit > Select > All and None: Quantize follows right after the click on the item.
	select("edit.selectAll");
	CHECK(!disabled(app, quantize));
	select("edit.selectNone");
	CHECK(disabled(app, quantize));

	// A Ctrl-click on the part selects it, and another one takes it out of the selection when the button is released.
	const ui::TimeSpan& part = lanes->lanes()[0].events[0];
	const ui::Target middle{"", 0,
		ui::Point{static_cast<int>(lanes->GetAbsoluteLeft() + ((part.start + part.end()) / 2.0)),
			static_cast<int>(lanes->GetAbsoluteTop() + (lanes->lanes()[0].height / 2.0F))}};
	app.ui().click(std::nullopt, middle, ui::MouseButton::Left, 1, {"ctrl"});
	CHECK(!disabled(app, quantize));
	if (auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface()))
		clock->advance(1.0); // no double click
	app.ui().click(std::nullopt, middle, ui::MouseButton::Left, 1, {"ctrl"});
	CHECK(disabled(app, quantize));
}

// A click's own listeners may change what the clicked item can do: the menu follows them before it looks for the
// item (#1120, #1176).
TEST_CASE("the main menu follows what a click's listeners changed before it runs the clicked item", "[unit]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	ui::Window& window = fake.shell.open(ui::WindowSpec{});
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding);
	bool enabled = false;
	int runs = 0;
	keys.add({"test.command", "A test command.", [&runs](ui::Window& /*window*/, Rml::Element* /*zone*/) { ++runs; }});
	keys.enableWhen("test.command", [&enabled](ui::Window& /*window*/, Rml::Element* /*zone*/) { return enabled; });
	Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(
		R"(<rml><head><style>body { width: 100%; height: 100%; } menuitem { display: block; height: 20dp; }</style>)"
		R"(</head><body><menuitem id="item" command="test.command"/></body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	Rml::Element* item = document->GetElementById("item");
	REQUIRE(item != nullptr);

	// The item's own listener lets its command run, before the menu's listener on the document.
	class Enabler final : public Rml::EventListener
	{
	public:
		explicit Enabler(bool& enabled) : m_enabled(enabled) {}
		void ProcessEvent(Rml::Event& /*event*/) override { m_enabled = true; }

	private:
		bool& m_enabled;
	} enabler(enabled);
	item->AddEventListener(Rml::EventId::Click, &enabler);
	{
		const ui::MainMenu menu(keys, window, *document);
		REQUIRE(item->HasAttribute("disabled"));
		ui::Automation(fake.shell).click(std::nullopt, {"#item", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		CHECK(runs == 1);
		CHECK(!item->HasAttribute("disabled"));
	}
	item->RemoveEventListener(Rml::EventId::Click, &enabler);
	document->Close();
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

TEST_CASE("a transport window whose controls are refused closes again (#669)", "[ui]")
{
	struct Broken
	{
		const char* file; // in resources/ui
		const char* from;
		const char* to;
		bool panel; // the transport panel, otherwise the time display window
	};
	const Broken broken[] = {
		// A button with a command that doesn't exist, in the transport controls that fill the transport panel.
		{"transport.rml", R"(command="locate.nextMarker")", R"(command="locate.nextMarkr")", true},
		// A menu item that gives a UI command parameters, in the time display window's context menu.
		{"time-display.rml", R"(command="timeDisplay.increaseSize")",
			R"(command="timeDisplay.increaseSize" parameters='{"steps": 2}')", false},
	};
	for (const Broken& each : broken)
	{
		INFO(each.file);
		// A copy of GNUbase's resources with the control changed.
		const test::TempDir folder;
		const std::filesystem::path resources = folder.path() / "resources";
		std::filesystem::copy(GNUBASE_RESOURCES_DIR, resources, std::filesystem::copy_options::recursive);
		const std::filesystem::path file = resources / "ui" / each.file;
		std::string text = test::readText(file);
		const size_t at = text.find(each.from);
		REQUIRE(at != std::string::npos);
		text.replace(at, std::strlen(each.from), each.to);
		std::ofstream(file, std::ios::binary) << text;

		// The project window isn't shown: the broken transport.rml would refuse its transport bar too.
		test::UiApp app({900, 640}, resources);
		app::AppUi& appUi = app.appUi();
		const auto toggleBroken = [&appUi, &each]
		{
			if (each.panel)
				appUi.toggleTransportPanel();
			else
				appUi.toggleTimeDisplay();
		};
		const auto brokenShown = [&appUi, &each]
		{ return each.panel ? appUi.transportPanel() != nullptr : appUi.timeDisplay() != nullptr; };

		// Twice, as F2 pressed again would: the window opens, its controls are refused and it closes again.
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			toggleBroken();
			app.shell().step(std::chrono::milliseconds(0));
			CHECK(!brokenShown());
			CHECK(app.shell().windows().size() == 1);
		}

		// The other window, whose controls are fine, opens from the same copy.
		if (each.panel)
			appUi.toggleTimeDisplay();
		else
			appUi.toggleTransportPanel();
		app.shell().step(std::chrono::milliseconds(0));
		CHECK((each.panel ? appUi.timeDisplay() != nullptr : appUi.transportPanel() != nullptr));
		CHECK(app.shell().windows().size() == 2);
	}
}
