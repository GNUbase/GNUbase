// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of the docking system (#56, #372, ui/docking.h): the project window's zones, splitters, tab groups and
// strips, panels in windows of their own and a test window's sections in a setup pane, driven through ui::Automation as
// MCP's UI tools drive them (tests/uiapp.h), through key commands and through the user setting ui.layout, and the
// layout after a restart.
#include "uiapp.h"

#include "ui/automation.h"
#include "ui/docking.h"

#include <RmlUi/Core/Element.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

	// A test panel in the lower zone, which marks what it fills and counts how many of it there are.
	struct TestPanel
	{
		explicit TestPanel(test::UiApp& app)
		{
			app.appUi().docking().add({"test", "Test", "project", "lower", [this](Rml::Element& element)
				{
					element.SetInnerRML("<div id='test-content'>Test</div>");
					++*alive;
					return std::shared_ptr<void>(nullptr, [count = alive](void*) { --*count; });
				}});
		}

		std::shared_ptr<int> alive = std::make_shared<int>(0);
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

	void closeRequest(ui::WindowId window)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
		event.window.windowID = window;
		REQUIRE(SDL_PushEvent(&event));
	}
}

TEST_CASE("the project window's zones and strips show and hide with buttons, keys, menus and ui.layout", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	for (const char* zone :
		{"#left-zone", "#lower-zone", "#right-zone", "#info-line", "#status-line", "#transport-bar"})
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

TEST_CASE("tab groups move, hide and step through their tabs and remember the active one", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	app.appUi().docking().setVisible("project", "lower", true);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(tabs(app, "lower") == std::vector<std::string>{"editor", "mixer"});

	app.ui().click(std::nullopt, at("tab[panel=mixer]"), ui::MouseButton::Left, 1, {});
	waitUntilSaved(app);
	CHECK(savedLayout(app).at("windows").at("project").at("zones").at("lower").at("active") == "mixer");

	// Dragged onto another tab, a tab takes its place.
	app.ui().drag(std::nullopt, middle(app, "tab[panel=mixer]"), middle(app, "tab[panel=editor]"));
	REQUIRE(app.waitUntil([&app] { return tabs(app, "lower") == std::vector<std::string>{"mixer", "editor"}; }));
	CHECK(element(app, "tab[panel=mixer]").value("pseudoClasses", Json::array()).dump().find("selected") !=
		std::string::npos);

	// The tab menu shows and hides tabs.
	app.ui().click(std::nullopt, at("tab[panel=editor]"), ui::MouseButton::Right, 1, {});
	CHECK(has(element(app, "#tab-menu-lower"), "open"));
	app.ui().click(std::nullopt, at("menuitem[tab=editor]"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return tabs(app, "lower") == std::vector<std::string>{"mixer"}; }));
	app.appUi().docking().setTabHidden("editor", false);
	CHECK(tabs(app, "lower") == std::vector<std::string>{"mixer", "editor"});

	// The keys go through the tabs of the zone with the focus, which shows it.
	app.ui().click(std::nullopt, at("#lower-zone panels"), ui::MouseButton::Left, 1, {});
	CHECK(has(element(app, "#lower-zone"), "focused"));
	CHECK(!has(element(app, "#main-zone"), "focused"));
	app.ui().key(std::nullopt, "Mod+Alt+Right");
	CHECK(app.appUi().docking().layout().at("windows").at("project").at("zones").at("lower").at("active") == "editor");
	app.ui().key(std::nullopt, "Mod+Alt+Right"); // around to the first
	CHECK(app.appUi().docking().layout().at("windows").at("project").at("zones").at("lower").at("active") == "mixer");
	app.ui().click(std::nullopt, at("#event-display"), ui::MouseButton::Left, 1, {});
	CHECK(has(element(app, "#main-zone"), "focused"));
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

	app.ui().click(std::nullopt, at("tab[panel=test]"), ui::MouseButton::Right, 1, {});
	app.ui().click(std::nullopt, at("#tab-menu-lower menuitem[action=float]"), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&docking] { return docking.windowOf("test") != nullptr; }));
	const ui::WindowId window = docking.windowOf("test")->id();
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
	CHECK(tabs(app, "lower") == std::vector<std::string>{"editor", "mixer", "test"});
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

TEST_CASE("the layout comes back exactly after a restart", "[ui]")
{
	test::UiApp app({1000, 700});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	ui::Docking& docking = app.appUi().docking();
	docking.setVisible("project", "left", true);
	docking.setVisible("project", "lower", true);
	docking.setVisible("project", "statusLine", true);
	docking.setVisible("project", "overviewLine", false);
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
		for (const char* selector : {"#left-zone", "#main-zone", "#lower-zone", "#right-zone", "#toolbar",
				 "#status-line", "#info-line", "#transport-bar", ".overview"})
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
	CHECK(before["lower"] == Json{"editor"});
	CHECK(before["right"] == Json{"media"});
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
