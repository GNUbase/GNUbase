// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of the docking system (#56, #372, ui/docking.h): the project window's zones, splitters, tab groups and
// strips, panels in windows of their own and a test window's sections in a setup pane, driven through ui::Automation as
// MCP's UI tools drive them (tests/uiapp.h), through key commands and through the user setting ui.layout, and the
// layout after a restart. Which window is in front, minimized or has the focus, on the fake window system.
#include "uiapp.h"

#include "engine/engine.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "ui/livereload.h"
#include "ui/rmlui.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Variant.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	ui::Target at(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	ui::Target point(int x, int y)
	{
		return {"", 0, ui::Point{x, y}};
	}

	// The elements that the selector matches in the window, as MCP's ui_dom shows them.
	Json elements(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		return app.ui().dom(window, selector, 0).at("elements");
	}

	Json element(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		const Json found = elements(app, selector, window);
		REQUIRE(found.size() == 1);
		return found[0];
	}

	bool has(const Json& element, const std::string& name)
	{
		const Json names = element.value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	bool shown(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		return found.size() == 1 && found[0].at("visible").get<bool>();
	}

	int boxOf(test::UiApp& app, const std::string& selector, const char* side)
	{
		return element(app, selector).at("box").at(side).get<int>();
	}

	// The middle of the element, in the window's pixels.
	ui::Target middle(test::UiApp& app, const std::string& selector)
	{
		const Json box = element(app, selector).at("box");
		return point(box.at("x").get<int>() + (box.at("width").get<int>() / 2),
			box.at("y").get<int>() + (box.at("height").get<int>() / 2));
	}

	ui::Target moved(const ui::Target& from, int dx, int dy)
	{
		const ui::Point at = from.point.value_or(ui::Point{});
		return point(at.x + dx, at.y + dy);
	}

	// The panels of the zone's tabs, in order.
	std::vector<std::string> tabs(test::UiApp& app, const std::string& zone)
	{
		std::vector<std::string> result;
		for (const Json& tab : elements(app, "#" + zone + "-zone tab"))
			result.push_back(tab.at("attributes").at("panel").get<std::string>());
		return result;
	}

	Json savedLayout(test::UiApp& app)
	{
		return app.call("settings.get", {{"key", "ui.layout"}}).at("value");
	}

	// Waits until ui.layout has what the docking system has, so that nothing is still on its way to the engine.
	void waitUntilSaved(test::UiApp& app)
	{
		REQUIRE(app.waitUntil([&app] { return savedLayout(app) == app.appUi().docking().layout(); }));
	}

	// A test panel in the lower zone, which marks what it fills with #<id>-content, counts how many of it there are and
	// how many went after the element they filled.
	struct TestPanel
	{
		explicit TestPanel(test::UiApp& app, const std::string& id = "test")
		{
			app.appUi().docking().add(
				{id, "Test", "project", "lower", [this, content = id + "-content"](Rml::Element& element)
					{
						element.SetInnerRML("<div id='" + content + "'>Test</div>");
						++*alive;
						return std::shared_ptr<void>(nullptr,
							[count = alive, late = late, filled = element.GetObserverPtr()](void*)
							{
								--*count;
								if (!filled)
									++*late;
							});
					}});
		}

		std::shared_ptr<int> alive = std::make_shared<int>(0);
		std::shared_ptr<int> late = std::make_shared<int>(0);
	};

	// tests/data/ui/sections.rml as the docking window "test".
	void showSections(test::UiApp& app)
	{
		app.appUi().docking().attach("test", app.show("sections.rml"));
		app.shell().step(std::chrono::milliseconds(0));
	}

	// The sections of the element, in order: those shown, or with all, the hidden ones too.
	std::vector<std::string> sections(test::UiApp& app, const std::string& selector, bool all = false)
	{
		std::vector<std::string> result;
		for (const Json& section : elements(app, selector + " [section]"))
			if (all || section.at("visible").get<bool>())
				result.push_back(section.at("attributes").at("section").get<std::string>());
		return result;
	}

	// The setup pane's items, in order, with the checked ones marked by a *.
	std::vector<std::string> paneItems(test::UiApp& app)
	{
		std::vector<std::string> result;
		for (const Json& item : elements(app, "[setup=channel] [setup-section]"))
			result.push_back(item.at("attributes").at("setup-section").get<std::string>() +
				(item.at("attributes").contains("checked") ? "*" : ""));
		return result;
	}

	using Names = std::vector<std::string>;

	// Appends a channel with the sections of tests/data/ui/sections.rml to the window's main zone, as the mixer adds
	// one for a new track, and has the docking system arrange it.
	Rml::Element& addChannel(test::UiApp& app, Rml::ElementDocument& document, const std::string& id)
	{
		Rml::Element* zone = document.GetElementById("main-zone");
		REQUIRE(zone != nullptr);
		Rml::Element* channel = zone->AppendChild(document.CreateElement("div"));
		channel->SetId(id);
		channel->SetClass("channel", true);
		channel->SetAttribute("sections", "channel");
		channel->SetInnerRML(R"(<div class="channel-name">New</div>
			<div section="routing" label="Routing"/>
			<div section="inserts" label="Inserts"/>
			<div section="eq" label="EQ" default="hidden"/>
			<div section="sends" label="Sends"/>
			<div section="fader" label="Fader"/>
			<div class="channel-footer"/>)");
		app.appUi().docking().arrangeSections(*channel);
		return *channel;
	}

	void closeRequest(ui::WindowId window)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
		event.window.windowID = window;
		REQUIRE(SDL_PushEvent(&event));
	}

	// The docking system on the fake window system, which knows which window is in front, minimized or has the focus,
	// with a main window as the project window and a test panel in its lower zone.
	struct FakeDocking
	{
		FakeDocking() { docking.add({"test", "Test", "project", "lower", {}}); }

		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::WindowId main = ui.shell.open({}).id();
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		ui::Docking docking{
			engine, binding, ui.shell, std::filesystem::path(GNUBASE_RESOURCES_DIR) / "ui" / "panel-window.rml"};
	};
}

TEST_CASE("opening a panel in a window of its own brings that window to the front with the focus", "[unit]")
{
	FakeDocking fixture;
	test::FakeWindowSystem& windows = fixture.ui.windows;
	fixture.docking.setFloating("test", true);
	const ui::Window* window = fixture.docking.windowOf("test");
	REQUIRE(window != nullptr);
	const ui::WindowId panel = window->id();
	CHECK(windows.front() == panel);

	SECTION("behind the project window")
	{
		windows.raise(fixture.main); // the user clicks the project window
		CHECK(windows.front() == fixture.main);
		CHECK(windows.focused() == fixture.main);
	}
	SECTION("minimized")
	{
		windows.minimize(panel);
		CHECK(windows.minimized(panel));
		CHECK(windows.focused() == fixture.main);
	}
	SECTION("minimized behind the project window")
	{
		windows.minimize(panel);
		windows.raise(fixture.main);
		CHECK(windows.minimized(panel));
	}
	const Json layout = fixture.docking.layout();
	fixture.docking.open("test");
	CHECK(!windows.minimized(panel));
	CHECK(windows.front() == panel);
	CHECK(windows.focused() == panel);
	// It stays in its window, and the layout stays as it was: the zone it docks in doesn't show.
	CHECK(fixture.docking.windowOf("test") == window);
	CHECK(fixture.docking.layout() == layout);
}

TEST_CASE("the project window's zones and strips show and hide with buttons, keys, menus and ui.layout", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	for (const char* zone :
		{"#channel-zone", "#left-zone", "#lower-zone", "#right-zone", "#info-line", "#status-line", "#transport-bar"})
	{
		INFO(zone);
		CHECK(!shown(app, zone)); // the defaults
	}
	CHECK(shown(app, "#toolbar"));
	CHECK(shown(app, ".overview"));

	app.ui().click(std::nullopt, at("#show-left-zone"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#left-zone"); }));
	CHECK(element(app, "#show-left-zone").at("attributes").contains("checked"));
	CHECK(shown(app, "splitter[for=left]"));

	app.ui().key(std::nullopt, "Mod+Alt+R");
	CHECK(shown(app, "#right-zone"));
	app.ui().key(std::nullopt, "Mod+Alt+E");
	CHECK(shown(app, "#lower-zone"));
	CHECK(element(app, "#show-lower-zone").at("attributes").contains("checked"));
	app.ui().key(std::nullopt, "Mod+I");
	CHECK(shown(app, "#info-line"));
	app.ui().key(std::nullopt, "Mod+Alt+T");
	CHECK(shown(app, "#transport-bar"));
	app.ui().key(std::nullopt, "Alt+O");
	CHECK(!shown(app, ".overview"));
	CHECK(has(element(app, "body"), "no-overviewLine"));

	// Set up Window Layout
	app.ui().click(std::nullopt, at("#window-layout"), ui::MouseButton::Left, 1, {});
	app.ui().click(std::nullopt, at("menuitem[toggle=statusLine]"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#status-line"); }));
	CHECK(element(app, "menuitem[toggle=left]").at("attributes").contains("checked"));
	CHECK(!element(app, "menuitem[toggle=overviewLine]").at("attributes").contains("checked"));

	waitUntilSaved(app);
	const Json layout = savedLayout(app);
	CHECK(layout.at("windows").at("project").at("zones").at("lower").at("visible") == true);
	CHECK(layout.at("windows").at("project").at("strips").at("overviewLine") == false);

	// Any client changes the layout through the user setting, as MCP and Lua do.
	Json hidden = layout;
	hidden["windows"]["project"]["strips"]["toolbar"] = false;
	hidden["windows"]["project"]["zones"]["left"]["visible"] = false;
	app.call("settings.set", {{"key", "ui.layout"}, {"value", hidden}});
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#toolbar") && !shown(app, "#left-zone"); }));
	CHECK(app.appUi().docking().layout() == hidden);

	// Back to the defaults.
	app.call("settings.reset", {{"key", "ui.layout"}});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#toolbar") && !shown(app, "#lower-zone"); }));
	CHECK(shown(app, ".overview"));
}

TEST_CASE("splitters size the zones down to their minimum, and a double click hides one", "[ui]")
{
	test::UiApp app({1000, 700});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	ui::Docking& docking = app.appUi().docking();
	docking.setVisible("project", "left", true);
	docking.setVisible("project", "lower", true);
	docking.setVisible("project", "right", true);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(boxOf(app, "#left-zone", "width") == 240);
	CHECK(boxOf(app, "#right-zone", "width") == 240);
	CHECK(boxOf(app, "#lower-zone", "height") == 240);

	const ui::Target left = middle(app, "splitter[for=left]");
	app.ui().drag(std::nullopt, left, moved(left, 60, 0));
	CHECK(boxOf(app, "#left-zone", "width") == 300);
	const ui::Target right = middle(app, "splitter[for=right]");
	app.ui().drag(std::nullopt, right, moved(right, -20, 0)); // the zone after the splitter grows
	CHECK(boxOf(app, "#right-zone", "width") == 260);
	const ui::Target lower = middle(app, "splitter[for=lower]");
	app.ui().drag(std::nullopt, lower, moved(lower, 0, -40));
	CHECK(boxOf(app, "#lower-zone", "height") == 280);
	waitUntilSaved(app);
	const Json zones = savedLayout(app).at("windows").at("project").at("zones");
	CHECK(zones.at("left").at("size") == 300);
	CHECK(zones.at("right").at("size") == 260);
	CHECK(zones.at("lower").at("size") == 280);

	// Not below a zone's minimum, nor so far that the main zone goes below its own.
	const ui::Target left2 = middle(app, "splitter[for=left]");
	app.ui().drag(std::nullopt, left2, moved(left2, -400, 0));
	CHECK(boxOf(app, "#left-zone", "width") == 120);
	const ui::Target left3 = middle(app, "splitter[for=left]");
	app.ui().drag(std::nullopt, left3, moved(left3, 900, 0));
	CHECK(boxOf(app, "#left-zone", "width") == 1000 - 260 - (2 * 8) - 200); // the column keeps 200 dp

	// A double click collapses the zone; showing it again restores its size.
	app.ui().click(std::nullopt, middle(app, "splitter[for=lower]"), ui::MouseButton::Left, 2, {});
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#lower-zone"); }));
	CHECK(!shown(app, "splitter[for=lower]"));
	app.ui().key(std::nullopt, "Mod+Alt+E");
	CHECK(boxOf(app, "#lower-zone", "height") == 280);
}

TEST_CASE(
	"the Channel zone, left of the left zone, shows and hides with its button, the menu, its key and ui.layout", "[ui]")
{
	test::UiApp app({1000, 700});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	CHECK(!shown(app, "#channel-zone"));
	CHECK(!shown(app, "splitter[for=channel]"));

	app.ui().click(std::nullopt, at("#show-channel-zone"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#channel-zone"); }));
	CHECK(element(app, "#show-channel-zone").at("attributes").contains("checked"));
	CHECK(shown(app, "splitter[for=channel]"));
	CHECK(tabs(app, "channel") == std::vector<std::string>{"channel"});
	CHECK(boxOf(app, "#channel-zone", "width") == 200);

	// Its splitter sizes it, taking the room from the main zone, whether the left zone shows or not.
	const ui::Target splitter = middle(app, "splitter[for=channel]");
	app.ui().drag(std::nullopt, splitter, moved(splitter, 50, 0));
	CHECK(boxOf(app, "#channel-zone", "width") == 250);
	app.ui().key(std::nullopt, "Mod+Alt+L");
	REQUIRE(shown(app, "#left-zone"));
	CHECK(boxOf(app, "#channel-zone", "x") + boxOf(app, "#channel-zone", "width") <= boxOf(app, "#left-zone", "x"));
	const ui::Target again = middle(app, "splitter[for=channel]");
	app.ui().drag(std::nullopt, again, moved(again, 150, 0));
	CHECK(boxOf(app, "#channel-zone", "width") == 400);
	CHECK(boxOf(app, "#left-zone", "width") == 240);
	waitUntilSaved(app);
	CHECK(savedLayout(app).at("windows").at("project").at("zones").at("channel").at("size") == 400);

	// It is a focus zone, with key commands of its own.
	std::string ranIn;
	app.keys().add(
		{"test.channelZone", "Records the focus zone it ran in.", [&ranIn](ui::Window& /*window*/, Rml::Element* zone)
			{ ranIn = zone == nullptr ? std::string() : zone->GetAttribute<Rml::String>("focuszone", ""); }});
	app.keys().bind({"F9", "test.channelZone", Json::object(), "channel"});
	app.ui().click(std::nullopt, at("#channel-zone panels"), ui::MouseButton::Left, 1, {});
	CHECK(has(element(app, "#channel-zone"), "focused"));
	CHECK(!has(element(app, "#main-zone"), "focused"));
	app.ui().key(std::nullopt, "F9");
	CHECK(ranIn == "channel");

	app.ui().key(std::nullopt, "Mod+Alt+C");
	CHECK(!shown(app, "#channel-zone"));
	CHECK(!shown(app, "splitter[for=channel]"));
	CHECK(!element(app, "#show-channel-zone").at("attributes").contains("checked"));

	// Set up Window Layout
	app.ui().click(std::nullopt, at("#window-layout"), ui::MouseButton::Left, 1, {});
	app.ui().click(std::nullopt, at("menuitem[toggle=channel]"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#channel-zone"); }));
	CHECK(boxOf(app, "#channel-zone", "width") == 400);
	waitUntilSaved(app);
	CHECK(savedLayout(app).at("windows").at("project").at("zones").at("channel").at("visible") == true);

	// Any client shows and hides it through the user setting.
	Json layout = savedLayout(app);
	layout["windows"]["project"]["zones"]["channel"]["visible"] = false;
	app.call("settings.set", {{"key", "ui.layout"}, {"value", layout}});
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#channel-zone"); }));
	layout["windows"]["project"]["zones"]["channel"] = {{"visible", true}, {"size", 180}};
	app.call("settings.set", {{"key", "ui.layout"}, {"value", layout}});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#channel-zone"); }));
	CHECK(boxOf(app, "#channel-zone", "width") == 180);
}

TEST_CASE("the lower zone has its tabs at its bottom, with Close Lower Zone to their left", "[ui]")
{
	test::UiApp app({1000, 700});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	app.ui().key(std::nullopt, "Mod+Alt+E");
	REQUIRE(shown(app, "#lower-zone"));
	app.shell().step(std::chrono::milliseconds(0));

	const int zoneBottom = boxOf(app, "#lower-zone", "y") + boxOf(app, "#lower-zone", "height");
	const int tabsTop = boxOf(app, "#lower-zone tabs", "y");
	CHECK(boxOf(app, "#lower-zone panels", "y") + boxOf(app, "#lower-zone panels", "height") <= tabsTop);
	CHECK(tabsTop + boxOf(app, "#lower-zone tabs", "height") == zoneBottom);
	const Json lowerTabs = elements(app, "#lower-zone tab");
	REQUIRE(!lowerTabs.empty());
	const Json first = lowerTabs[0].at("box");
	CHECK(first.at("y").get<int>() >= tabsTop);
	// The other zones keep theirs at the top.
	app.ui().key(std::nullopt, "Mod+Alt+R");
	CHECK(boxOf(app, "#right-zone tabs", "y") == boxOf(app, "#right-zone", "y"));

	// Close Lower Zone, left of the tabs and in their row, hides the zone, and the focus goes to the main zone, so that
	// the lower zone's keys no longer apply.
	REQUIRE(shown(app, "#close-lower-zone"));
	const Json close = element(app, "#close-lower-zone").at("box");
	CHECK(close.at("x").get<int>() + close.at("width").get<int>() <= first.at("x").get<int>());
	CHECK(close.at("y").get<int>() >= tabsTop);
	CHECK(close.at("y").get<int>() + close.at("height").get<int>() <= zoneBottom);
	app.ui().click(std::nullopt, at("#close-lower-zone"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#lower-zone"); }));
	CHECK(!element(app, "#show-lower-zone").at("attributes").contains("checked"));
	CHECK(has(element(app, "#main-zone"), "focused"));
	CHECK(!has(element(app, "#lower-zone"), "focused"));
	const Json active =
		app.appUi().docking().layout().at("windows").at("project").at("zones").at("lower").value("active", Json());
	app.ui().key(std::nullopt, "Mod+Alt+Right");
	CHECK(app.appUi().docking().layout().at("windows").at("project").at("zones").at("lower").value("active", Json()) ==
		active);
	waitUntilSaved(app);
	CHECK(savedLayout(app).at("windows").at("project").at("zones").at("lower").at("visible") == false);
}

TEST_CASE("tab groups move, hide and step through their tabs and remember the active one", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	app.appUi().docking().setVisible("project", "lower", true);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(tabs(app, "lower") == std::vector<std::string>{"editor", "mixer", "pool", "chordPads"});

	app.ui().click(std::nullopt, at("tab[panel=mixer]"), ui::MouseButton::Left, 1, {});
	waitUntilSaved(app);
	CHECK(savedLayout(app).at("windows").at("project").at("zones").at("lower").at("active") == "mixer");

	// Dragged onto another tab, a tab takes its place.
	app.ui().drag(std::nullopt, middle(app, "tab[panel=mixer]"), middle(app, "tab[panel=editor]"));
	REQUIRE(app.waitUntil(
		[&app] { return tabs(app, "lower") == std::vector<std::string>{"mixer", "editor", "pool", "chordPads"}; }));
	CHECK(element(app, "tab[panel=mixer]").value("pseudoClasses", Json::array()).dump().find("selected") !=
		std::string::npos);

	// The tab menu shows and hides tabs.
	app.ui().click(std::nullopt, at("tab[panel=editor]"), ui::MouseButton::Right, 1, {});
	CHECK(has(element(app, "#tab-menu-lower"), "open"));
	app.ui().click(std::nullopt, at("menuitem[tab=editor]"), ui::MouseButton::Left, 1, {});
	REQUIRE(
		app.waitUntil([&app] { return tabs(app, "lower") == std::vector<std::string>{"mixer", "pool", "chordPads"}; }));
	app.appUi().docking().setTabHidden("editor", false);
	CHECK(tabs(app, "lower") == std::vector<std::string>{"mixer", "editor", "pool", "chordPads"});

	// The keys go through the tabs of the zone with the focus, which shows it.
	app.ui().click(std::nullopt, at("#lower-zone panels"), ui::MouseButton::Left, 1, {});
	CHECK(has(element(app, "#lower-zone"), "focused"));
	CHECK(!has(element(app, "#main-zone"), "focused"));
	app.ui().key(std::nullopt, "Mod+Alt+Right");
	CHECK(app.appUi().docking().layout().at("windows").at("project").at("zones").at("lower").at("active") == "editor");
	app.ui().key(std::nullopt, "Mod+Alt+Right");
	CHECK(app.appUi().docking().layout().at("windows").at("project").at("zones").at("lower").at("active") == "pool");
	app.ui().key(std::nullopt, "Mod+Alt+Right");
	CHECK(
		app.appUi().docking().layout().at("windows").at("project").at("zones").at("lower").at("active") == "chordPads");
	app.ui().key(std::nullopt, "Mod+Alt+Right"); // around to the first
	CHECK(app.appUi().docking().layout().at("windows").at("project").at("zones").at("lower").at("active") == "mixer");
	app.ui().click(std::nullopt, at("#event-display"), ui::MouseButton::Left, 1, {});
	CHECK(has(element(app, "#main-zone"), "focused"));
}

TEST_CASE("Tab and Shift+Tab move the keyboard focus through the zones", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	ui::Docking& docking = app.appUi().docking();
	for (const char* zone : {"left", "lower", "right"})
		docking.setVisible("project", zone, true);
	app.shell().step(std::chrono::milliseconds(0));
	const auto focused = [&app]
	{
		std::string found;
		for (const char* zone : {"#main-zone", "#channel-zone", "#left-zone", "#lower-zone", "#right-zone"})
			if (has(element(app, zone), "focused"))
				found += (found.empty() ? "" : ",") + std::string(zone);
		return found;
	};
	app.ui().click(std::nullopt, at("#event-display"), ui::MouseButton::Left, 1, {});
	CHECK(focused() == "#main-zone");

	// Forwards in the reference's order (p. 100), around to the project zone again, and back with Shift.
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused() == "#left-zone");
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused() == "#lower-zone");
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused() == "#right-zone");
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused() == "#main-zone");
	app.ui().key(std::nullopt, "Shift+Tab");
	CHECK(focused() == "#right-zone");
	app.ui().key(std::nullopt, "Shift+Tab");
	CHECK(focused() == "#lower-zone");

	// A key command of a focus zone runs in the zone that has the focus: the lower zone's next tab.
	app.ui().key(std::nullopt, "Mod+Alt+Right");
	CHECK(docking.layout().at("windows").at("project").at("zones").at("lower").value("active", "") == "mixer");

	// Hidden zones are skipped.
	docking.setVisible("project", "right", false);
	app.shell().step(std::chrono::milliseconds(0));
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused() == "#main-zone");
	app.ui().key(std::nullopt, "Shift+Tab");
	CHECK(focused() == "#lower-zone");

	// The Channel zone comes right after the project zone (#380).
	docking.setVisible("project", "channel", true);
	app.shell().step(std::chrono::milliseconds(0));
	app.ui().click(std::nullopt, at("#event-display"), ui::MouseButton::Left, 1, {});
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused() == "#channel-zone");
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused() == "#left-zone");
	app.ui().key(std::nullopt, "Shift+Tab");
	CHECK(focused() == "#channel-zone");
	app.ui().key(std::nullopt, "Shift+Tab");
	CHECK(focused() == "#main-zone");

	// In a text field, Tab moves between controls as before, not to the next zone.
	Rml::Context& context = app.main().context();
	Rml::ElementDocument* document = context.GetFocusElement()->GetOwnerDocument();
	Rml::Element* field = document->GetElementById("lower-zone")->AppendChild(document->CreateElement("input"));
	field->SetAttribute("type", "text");
	REQUIRE(field->Focus());
	app.ui().key(std::nullopt, "Tab");
	CHECK(!context.GetFocusElement()->HasAttribute("zone"));

	// Ctrl+Tab isn't the zones' key.
	app.ui().click(std::nullopt, at("#event-display"), ui::MouseButton::Left, 1, {});
	app.ui().key(std::nullopt, "Ctrl+Tab");
	CHECK(!context.GetFocusElement()->HasAttribute("zone"));

	// An open menu keeps its keys: Tab and Shift+Tab step through its items and round its ends, without the focus
	// leaving it for a zone or a menu bar button (#524).
	app.ui().click(std::nullopt, at("button[menu=edit-menu]"), ui::MouseButton::Left, 1, {});
	REQUIRE(has(element(app, "#edit-menu"), "open"));
	Rml::Element* menu = document->GetElementById("edit-menu");
	REQUIRE(menu != nullptr);
	std::vector<Rml::Element*> items;
	for (int i = 0; i < menu->GetNumChildren(); ++i)
		if (menu->GetChild(i)->GetTagName() == "menuitem" && !menu->GetChild(i)->HasAttribute("disabled"))
			items.push_back(menu->GetChild(i));
	REQUIRE(items.size() > 1);
	for (const char* key : {"Tab", "Shift+Tab"})
	{
		const bool forwards = std::string(key) == "Tab";
		for (size_t step = 0; step <= items.size(); ++step)
		{
			app.ui().key(std::nullopt, key);
			// From the menu itself, the first or last item, then the others in turn, and round to the first or last.
			const size_t index = step % items.size();
			CHECK(context.GetFocusElement() == items[forwards ? index : items.size() - 1 - index]);
		}
		CHECK(has(element(app, "#edit-menu"), "open"));
		menu->Focus();
	}
	app.ui().key(std::nullopt, "Escape");
}

TEST_CASE("a panel moves between its zone and a window of its own", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	const TestPanel panel(app);
	ui::Docking& docking = app.appUi().docking();
	docking.setVisible("project", "lower", true);
	docking.showTab("test");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(shown(app, "#test-content"));
	CHECK(*panel.alive == 1);
	// A panel shows while it is its zone's active tab and the zone shows, or while it has a window of its own.
	CHECK(docking.showing("test"));
	CHECK_FALSE(docking.showing("mixer"));
	docking.setVisible("project", "lower", false);
	CHECK_FALSE(docking.showing("test"));
	CHECK(*panel.alive == 1); // it stays while it is hidden
	docking.setVisible("project", "lower", true);

	app.ui().click(std::nullopt, at("tab[panel=test]"), ui::MouseButton::Right, 1, {});
	app.ui().click(std::nullopt, at("#tab-menu-lower menuitem[action=float]"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&docking] { return docking.windowOf("test") != nullptr; }));
	const ui::WindowId window = docking.windowOf("test")->id();
	CHECK(docking.showing("test"));
	CHECK(elements(app, "tab[panel=test]").empty());
	CHECK(elements(app, "#test-content").empty());
	CHECK(element(app, "#test-content", window).at("visible") == true);
	CHECK(*panel.alive == 1); // the one in the zone went
	const Json windows = app.ui().windows().at("windows");
	REQUIRE(windows.size() == 2);
	CHECK(windows[1].at("documents")[0].at("title") == "Test");

	// Dock puts it back into its zone.
	app.ui().click(window, at("#dock"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&docking] { return docking.windowOf("test") == nullptr; }));
	CHECK(app.shell().find(window) == nullptr);
	CHECK(tabs(app, "lower") == std::vector<std::string>{"editor", "mixer", "pool", "chordPads", "test"});
	CHECK(*panel.alive == 1);

	// So does closing its window, which keeps where it was for the next time.
	docking.setFloating("test", true);
	ui::Window* again = docking.windowOf("test");
	REQUIRE(again != nullptr);
	const ui::Rect bounds{120, 140, 640, 400};
	app.shell().setBounds(again->id(), bounds);
	const ui::Rect placed = app.shell().bounds(again->id());
	closeRequest(again->id());
	REQUIRE(app.waitUntil([&docking] { return docking.windowOf("test") == nullptr; }));
	CHECK(shown(app, "tab[panel=test]"));
	const Json saved = docking.layout().at("panels").at("test").at("bounds");
	CHECK(saved == Json{{"x", placed.x}, {"y", placed.y}, {"width", placed.width}, {"height", placed.height}});
	docking.setFloating("test", true);
	REQUIRE(docking.windowOf("test") != nullptr);
	CHECK(app.shell().bounds(docking.windowOf("test")->id()) == placed);
}

TEST_CASE("documents that live reload loads again are docking windows again", "[ui]")
{
	// A copy of the resources, whose files the test can change.
	const test::TempDir resources;
	std::filesystem::copy(GNUBASE_RESOURCES_DIR, resources.path(), std::filesystem::copy_options::recursive);
	test::UiApp app({1000, 700}, resources.path());
	app.appUi().showProject();
	const TestPanel docked(app);
	const TestPanel floating(app, "floating");
	ui::Docking& docking = app.appUi().docking();
	docking.setVisible("project", "lower", true);
	docking.showTab("test");
	docking.setFloating("floating", true);
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(docking.windowOf("floating") != nullptr);
	const ui::WindowId window = docking.windowOf("floating")->id();

	// The project window's document and the panel window's are loaded again, once for each file that changes. What
	// the panels' create returned for the old documents goes, and they fill the new ones.
	for (const char* name : {"project.rml", "panel-window.rml"})
	{
		INFO(name);
		const std::filesystem::path file = resources.path() / "ui" / name;
		std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(3));
		CHECK(ui::reloadChangedDocuments(app.appUi().rmlui().files(), app.shell()) >= 2);
		app.shell().step(std::chrono::milliseconds(0));
		CHECK(*docked.alive == 1);
		CHECK(*floating.alive == 1);
		CHECK(*docked.late == 0);
		CHECK(*floating.late == 0);
		CHECK(elements(app, "#test-content").size() == 1);
		CHECK(shown(app, "#test-content"));
		CHECK(docking.showing("test"));
		CHECK(element(app, "#floating-content", window).at("visible") == true);
		CHECK(tabs(app, "lower") == std::vector<std::string>{"editor", "mixer", "pool", "chordPads", "test"});
		CHECK(shown(app, "#lower-zone"));
	}

	// An event that names anything but another document of the same window changes nothing.
	Rml::ElementDocument* project = nullptr;
	for (int i = 0; i < app.main().context().GetNumDocuments(); ++i)
		if (Rml::ElementDocument* document = app.main().context().GetDocument(i);
			document->GetElementById("event-display") != nullptr)
			project = document;
	ui::Window* panelWindow = app.shell().find(window);
	REQUIRE(project != nullptr);
	REQUIRE(panelWindow != nullptr);
	for (void* named : {static_cast<void*>(project->GetElementById("event-display")),
			 static_cast<void*>(panelWindow->context().GetDocument(0)), static_cast<void*>(project),
			 static_cast<void*>(nullptr)})
	{
		project->DispatchEvent(ui::g_documentReloadEvent, {{"document", Rml::Variant(named)}});
		app.shell().step(std::chrono::milliseconds(0));
		CHECK(*docked.alive == 1);
		CHECK(elements(app, "#test-content").size() == 1);
		CHECK(docking.showing("test"));
	}

	// A panel window document without #panel-content leaves the window empty until its panel docks.
	const std::filesystem::path panelFile = resources.path() / "ui" / "panel-window.rml";
	const std::filesystem::file_time_type before = std::filesystem::last_write_time(panelFile);
	{
		std::ofstream out(panelFile, std::ios::binary);
		out << R"(<rml><head><title>Panel</title></head><body class="docking">
			<div id="toolbar" class="toolbar"><button id="dock">Dock</button></div></body></rml>)";
	}
	std::filesystem::last_write_time(panelFile, before + std::chrono::seconds(3));
	CHECK(ui::reloadChangedDocuments(app.appUi().rmlui().files(), app.shell()) >= 2);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(*floating.alive == 0);
	CHECK(*floating.late == 0);
	CHECK(!docking.showing("floating"));
	CHECK(app.shell().find(window) != nullptr);
	CHECK(*docked.alive == 1);

	// Toggles, keys and splitters show and hide zones.
	app.ui().click(std::nullopt, at("#show-left-zone"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#left-zone"); }));
	CHECK(element(app, "#show-left-zone").at("attributes").contains("checked"));
	CHECK(shown(app, "splitter[for=left]"));
	app.ui().key(std::nullopt, "Mod+Alt+R");
	CHECK(shown(app, "#right-zone"));
	app.ui().click(std::nullopt, middle(app, "splitter[for=right]"), ui::MouseButton::Left, 2, {});
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#right-zone"); }));
	waitUntilSaved(app);
	CHECK(savedLayout(app).at("windows").at("project").at("zones").at("right").at("visible") == false);

	// A tab shows its panel, and the tab menu hides tabs.
	app.ui().click(std::nullopt, at("tab[panel=mixer]"), ui::MouseButton::Left, 1, {});
	waitUntilSaved(app);
	CHECK(savedLayout(app).at("windows").at("project").at("zones").at("lower").at("active") == "mixer");
	CHECK(!docking.showing("test"));
	app.ui().click(std::nullopt, at("tab[panel=pool]"), ui::MouseButton::Right, 1, {});
	CHECK(has(element(app, "#tab-menu-lower"), "open"));
	app.ui().click(std::nullopt, at("menuitem[tab=pool]"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil(
		[&app] { return tabs(app, "lower") == std::vector<std::string>{"editor", "mixer", "chordPads", "test"}; }));

	// The panel window's Dock button puts its panel back into its zone.
	app.ui().click(window, at("#dock"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&docking] { return docking.windowOf("floating") == nullptr; }));
	CHECK(app.shell().find(window) == nullptr);
	CHECK(tabs(app, "lower") == std::vector<std::string>{"editor", "mixer", "chordPads", "test", "floating"});
	CHECK(*floating.alive == 1);
	CHECK(*docked.alive == 1);
}

TEST_CASE("the layout comes back exactly after a restart", "[ui]")
{
	test::UiApp app({1000, 700});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	ui::Docking& docking = app.appUi().docking();
	docking.setVisible("project", "channel", true);
	docking.setVisible("project", "left", true);
	docking.setVisible("project", "lower", true);
	docking.setVisible("project", "statusLine", true);
	docking.setVisible("project", "overviewLine", false);
	docking.setSize("project", "channel", 230);
	docking.setSize("project", "left", 310);
	docking.setSize("project", "lower", 190);
	docking.moveTab("visibility", "inspector");
	docking.showTab("inspector");
	docking.setTabHidden("meter", true);
	docking.setFloating("mixer", true);
	REQUIRE(docking.windowOf("mixer") != nullptr);
	app.shell().setBounds(docking.windowOf("mixer")->id(), {200, 150, 600, 380});
	const ui::Rect bounds = app.shell().bounds(docking.windowOf("mixer")->id());
	// The window's bounds are saved within half a second of a move.
	REQUIRE(app.waitUntil(
		[&docking, &bounds]
		{
			const Json saved = docking.layout().at("panels").at("mixer").value("bounds", Json());
			return saved == Json{{"x", bounds.x}, {"y", bounds.y}, {"width", bounds.width}, {"height", bounds.height}};
		}));
	waitUntilSaved(app);

	const auto snapshot = [&app]
	{
		Json result;
		for (const char* selector : {"#channel-zone", "#left-zone", "#main-zone", "#lower-zone", "#right-zone",
				 "#toolbar", "#status-line", "#info-line", "#transport-bar", ".overview"})
		{
			const Json found = elements(app, selector);
			result[selector] = found.size() == 1 && found[0].at("visible").get<bool>() ? found[0].at("box") : Json();
		}
		result["left"] = tabs(app, "left");
		result["lower"] = tabs(app, "lower");
		result["right"] = tabs(app, "right");
		result["active"] = element(app, "#left-zone tab:selected").at("attributes").at("panel");
		return result;
	};
	const Json before = snapshot();
	const Json layout = docking.layout();
	CHECK(before["left"] == Json{"visibility", "inspector"});
	CHECK(before["lower"] == Json{"editor", "pool", "chordPads"});
	CHECK(before["right"] == Json{"media", "history", "quantize"});
	CHECK(before["active"] == "inspector");

	app.restart();
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.appUi().docking().layout() == layout);
	CHECK(snapshot() == before);
	ui::Window* mixer = app.appUi().docking().windowOf("mixer");
	REQUIRE(mixer != nullptr);
	CHECK(app.shell().bounds(mixer->id()) == bounds);
}

TEST_CASE("a setup pane shows, hides and reorders a window's sections, as ui.layout does", "[ui]")
{
	test::UiApp app({1000, 700});
	showSections(app);
	CHECK(sections(app, "#channel-1") == Names{"routing", "inserts", "sends", "fader"}); // eq is hidden by default
	CHECK(sections(app, "#channel-2", true) == Names{"routing", "inserts", "eq", "sends", "fader"});
	CHECK(paneItems(app) == Names{"routing*", "inserts*", "eq", "sends*", "fader*"});
	CHECK(element(app, "[setup-section=eq]").at("text") == "EQ");

	// The pane stays open while its items show and hide sections.
	app.ui().click(std::nullopt, at("#window-layout"), ui::MouseButton::Left, 1, {});
	REQUIRE(has(element(app, "#layout-pane"), "open"));
	app.ui().click(std::nullopt, at("[setup-section=eq]"), ui::MouseButton::Left, 1, {});
	app.ui().click(std::nullopt, at("[setup-section=inserts]"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return sections(app, "#channel-1") == Names{"routing", "eq", "sends", "fader"}; }));
	CHECK(sections(app, "#channel-2") == Names{"routing", "eq", "sends", "fader"});
	CHECK(paneItems(app) == Names{"routing*", "inserts", "eq*", "sends*", "fader*"});
	CHECK(has(element(app, "#layout-pane"), "open"));

	// An item dragged onto another takes its place, in the pane and in every channel, between the channel's name and
	// its footer.
	app.ui().drag(std::nullopt, middle(app, "[setup-section=fader]"), middle(app, "[setup-section=routing]"));
	REQUIRE(app.waitUntil(
		[&app] { return sections(app, "#channel-1", true) == Names{"fader", "routing", "inserts", "eq", "sends"}; }));
	CHECK(sections(app, "#channel-2", true) == Names{"fader", "routing", "inserts", "eq", "sends"});
	CHECK(paneItems(app) == Names{"fader*", "routing*", "inserts", "eq*", "sends*"});
	CHECK(has(element(app, "#layout-pane"), "open"));
	const Json children = elements(app, "#channel-1 > *");
	CHECK(has(children.front(), "channel-name"));
	CHECK(has(children.back(), "channel-footer"));

	// The pane's other items show and hide strips and zones.
	app.ui().click(std::nullopt, at("#layout-pane [toggle=left]"), ui::MouseButton::Left, 1, {});
	app.ui().click(std::nullopt, at("#layout-pane [toggle=meterBridge]"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return shown(app, "#left-zone") && !shown(app, "#meter-bridge"); }));

	waitUntilSaved(app);
	CHECK(savedLayout(app).at("windows").at("test").at("sections").at("channel") ==
		Json{{"order", {"fader", "routing", "inserts", "eq", "sends"}}, {"hidden", {"inserts"}}});

	// Any client changes them through the user setting, as MCP and Lua do. Sections it leaves out follow in the
	// document's order, and names that aren't sections are ignored.
	Json layout = savedLayout(app);
	layout["windows"]["test"]["sections"]["channel"] = {
		{"order", {"sends", "mixer", "fader"}}, {"hidden", {"routing"}}};
	app.call("settings.set", {{"key", "ui.layout"}, {"value", layout}});
	REQUIRE(app.waitUntil(
		[&app] { return sections(app, "#channel-1", true) == Names{"sends", "fader", "routing", "inserts", "eq"}; }));
	CHECK(sections(app, "#channel-2") == Names{"sends", "fader", "inserts", "eq"});
	CHECK(paneItems(app) == Names{"sends*", "fader*", "routing", "inserts*", "eq*"});

	// The engine API checks the setting's shape.
	Json bad = layout;
	bad["windows"]["test"]["sections"]["channel"]["order"] = "fader";
	CHECK(!engine::Client(app.engine()).call("settings.set", {{"key", "ui.layout"}, {"value", bad}}).ok());

	// Back to the defaults.
	app.call("settings.reset", {{"key", "ui.layout"}});
	REQUIRE(
		app.waitUntil([&app] { return sections(app, "#channel-1") == Names{"routing", "inserts", "sends", "fader"}; }));
	CHECK(paneItems(app) == Names{"routing*", "inserts*", "eq", "sends*", "fader*"});
}

TEST_CASE("the order and visibility of sections come back after a restart", "[ui]")
{
	test::UiApp app({1000, 700});
	showSections(app);
	ui::Docking& docking = app.appUi().docking();
	docking.moveSection("test", "channel", "sends", "routing");
	docking.setSectionHidden("test", "channel", "fader", true);
	docking.setSectionHidden("test", "channel", "eq", false);
	CHECK(docking.sections("test", "channel") == Names{"sends", "routing", "inserts", "eq", "fader"});
	const Names shownBefore = sections(app, "#channel-2");
	CHECK(shownBefore == Names{"sends", "routing", "inserts", "eq"});
	const Names itemsBefore = paneItems(app);
	waitUntilSaved(app);
	const Json layout = docking.layout();

	app.restart();
	showSections(app);
	CHECK(app.appUi().docking().layout() == layout);
	CHECK(sections(app, "#channel-1") == shownBefore);
	CHECK(sections(app, "#channel-2") == shownBefore);
	CHECK(paneItems(app) == itemsBefore);
}

TEST_CASE("containers of sections added or rebuilt after attach show the user's order and hidden sections", "[ui]")
{
	test::UiApp app({1000, 700});
	Rml::ElementDocument& document = app.show("sections.rml");
	ui::Docking& docking = app.appUi().docking();
	docking.attach("test", document);
	docking.moveSection("test", "channel", "fader", "routing");
	docking.setSectionHidden("test", "channel", "inserts", true);
	const Names order{"fader", "routing", "inserts", "eq", "sends"};
	const Names shownOrder{"fader", "routing", "sends"};

	// A new channel takes them at once, between its name and its footer.
	addChannel(app, document, "channel-3");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(sections(app, "#channel-3", true) == order);
	CHECK(sections(app, "#channel-3") == shownOrder);
	const Json children = elements(app, "#channel-3 > *");
	CHECK(has(children.front(), "channel-name"));
	CHECK(has(children.back(), "channel-footer"));

	// So does a channel whose sections were rebuilt.
	Rml::Element* rebuilt = document.GetElementById("channel-1");
	REQUIRE(rebuilt != nullptr);
	rebuilt->SetInnerRML(R"(<div section="routing" label="Routing"/><div section="inserts" label="Inserts"/>
		<div section="eq" label="EQ" default="hidden"/><div section="sends" label="Sends"/>
		<div section="fader" label="Fader"/>)");
	docking.arrangeSections(*rebuilt);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(sections(app, "#channel-1", true) == order);
	CHECK(sections(app, "#channel-1") == shownOrder);
	CHECK(paneItems(app) == Names{"fader*", "routing*", "inserts", "eq", "sends*"});

	// A setup pane that comes later gets the set's buttons in the user's order.
	Rml::Element* menu = document.GetElementById("layout-pane");
	REQUIRE(menu != nullptr);
	Rml::Element* pane = menu->AppendChild(document.CreateElement("div"));
	pane->SetId("later-pane");
	pane->SetAttribute("setup", "channel");
	docking.arrangeSections(*pane);
	Names items;
	for (const Json& item : elements(app, "#later-pane [setup-section]"))
		items.push_back(item.at("attributes").at("setup-section").get<std::string>() +
			(item.at("attributes").contains("checked") ? "*" : ""));
	CHECK(items == Names{"fader*", "routing*", "inserts", "eq", "sends*"});

	// Later changes reach the new channel like the others.
	docking.setSectionHidden("test", "channel", "eq", false);
	CHECK(sections(app, "#channel-3") == Names{"fader", "routing", "eq", "sends"});
	CHECK(sections(app, "#channel-2") == Names{"fader", "routing", "eq", "sends"});
}

TEST_CASE("a window without containers of sections learns a set from the first container that appears", "[ui]")
{
	test::UiApp app({1000, 700});
	// The user's order and hidden sections from before, e.g. from a project that had channels.
	Json layout = Json::object();
	layout["windows"]["test"]["sections"]["channel"] = {{"order", {"sends", "fader"}}, {"hidden", {"routing"}}};
	app.call("settings.set", {{"key", "ui.layout"}, {"value", layout}});
	ui::Docking& docking = app.appUi().docking();
	REQUIRE(app.waitUntil([&docking, &layout] { return docking.layout() == layout; }));

	Rml::ElementDocument& document = app.show("sections-later.rml");
	docking.attach("test", document);
	CHECK(paneItems(app).empty());
	CHECK(docking.sections("test", "channel").empty());

	// An element that isn't in a docking window's document is left alone.
	const Rml::ElementPtr loose = document.CreateElement("div");
	loose->SetAttribute("sections", "channel");
	docking.arrangeSections(*loose);
	CHECK(paneItems(app).empty());

	// The first channel fills the setup pane and is arranged at once.
	addChannel(app, document, "channel-1");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(paneItems(app) == Names{"sends*", "fader*", "routing", "inserts*", "eq*"});
	CHECK(element(app, "[setup-section=eq]").at("text") == "EQ");
	CHECK(sections(app, "#channel-1") == Names{"sends", "fader", "inserts", "eq"});
	CHECK(docking.sections("test", "channel") == Names{"sends", "fader", "routing", "inserts", "eq"});

	// The next one follows it, and the pane works as on a window that had channels from the start.
	addChannel(app, document, "channel-2");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(sections(app, "#channel-2") == Names{"sends", "fader", "inserts", "eq"});
	CHECK(paneItems(app).size() == 5);
	app.ui().click(std::nullopt, at("#window-layout"), ui::MouseButton::Left, 1, {});
	app.ui().click(std::nullopt, at("[setup-section=routing]"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil(
		[&app] { return sections(app, "#channel-2") == Names{"sends", "fader", "routing", "inserts", "eq"}; }));
	CHECK(sections(app, "#channel-1") == Names{"sends", "fader", "routing", "inserts", "eq"});
}

TEST_CASE("a window learns a set from containers in a docked panel's content", "[ui]")
{
	test::UiApp app({1000, 700});
	Json layout = Json::object();
	layout["windows"]["test"]["sections"]["channel"] = {{"order", {"fader"}}, {"hidden", {"eq", "sends"}}};
	app.call("settings.set", {{"key", "ui.layout"}, {"value", layout}});
	ui::Docking& docking = app.appUi().docking();
	REQUIRE(app.waitUntil([&docking, &layout] { return docking.layout() == layout; }));

	// The panel makes its channel only when it shows, after attach has looked at the document.
	docking.add({"channels", "Channels", "test", "left", [](Rml::Element& element)
		{
			element.SetInnerRML(R"(<div id="panel-channel" class="channel" sections="channel">
				<div section="routing" label="Routing"/><div section="eq" label="EQ"/>
				<div section="sends" label="Sends"/><div section="fader" label="Fader"/></div>)");
			return std::shared_ptr<void>();
		}});
	docking.attach("test", app.show("sections-later.rml"));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(paneItems(app) == Names{"fader*", "routing*", "eq", "sends"});
	CHECK(sections(app, "#panel-channel") == Names{"fader", "routing"});
}
