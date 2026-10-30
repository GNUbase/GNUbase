// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of GNUbase's UI as the app shows it (#59), through the harness in uiapp.h: the app's own UI setup on the
// dummy device, driven with ui::Automation as MCP's UI tools drive it, with screenshots compared with golden images.
#include "uiapp.h"

#include "engine/time/tempomap.h"
#include "scripting/runtime.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/eventdisplay.h"
#include "ui/themes.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	// Opens the rename dialog when the button is clicked, as an app's button would.
	struct DialogOpener final : Rml::EventListener
	{
		void ProcessEvent(Rml::Event& event) override
		{
			Rml::ElementDocument* dialog = event.GetCurrentElement()->GetContext()->LoadDocument(
				std::string(GNUBASE_TEST_DATA_DIR) + "/ui/automation-dialog.rml");
			REQUIRE(dialog != nullptr);
			dialog->Show(Rml::ModalFlag::Modal);
		}
	};

	ui::Target selector(const char* text)
	{
		return {text, 0, std::nullopt};
	}

	// The one element that the selector matches, as MCP's ui_dom shows it.
	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		return found[0];
	}

	size_t count(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements").size();
	}

	bool visible(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).at("visible").get<bool>();
	}

	bool hasClass(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json classes = element(app, selector).value("classes", Json::array());
		return std::find(classes.begin(), classes.end(), name) != classes.end();
	}

	void click(test::UiApp& app, const ui::Target& target)
	{
		app.ui().click(std::nullopt, target, ui::MouseButton::Left, 1, {});
	}

	// The menu's item with that text, for the UI tools to click.
	ui::Target menuItem(test::UiApp& app, const std::string& menu, const std::string& text)
	{
		const std::string items = menu + " > menuitem";
		const Json found = app.ui().dom(std::nullopt, items, 0).at("elements");
		size_t i = 0;
		while (i < found.size() && found[i].value("text", "") != text)
			++i;
		INFO("the menu " << menu << " has no item " << text);
		REQUIRE(i < found.size());
		return {items, static_cast<int>(i), std::nullopt};
	}

	// Opens a menu with its button and clicks the item with that text, which closes the menu.
	void choose(test::UiApp& app, const char* button, const std::string& menu, const std::string& text)
	{
		click(app, selector(button));
		REQUIRE(visible(app, menu));
		click(app, menuItem(app, menu, text));
		CHECK(!visible(app, menu));
	}

	// The project window in a main window of 800 by 300 dp, at a dp ratio of 1.
	struct ProjectWindow
	{
		ProjectWindow() : display(app.appUi().showProject())
		{
			app.setDpRatio(1.0F);
			app.shell().step(std::chrono::milliseconds(0));
		}

		test::UiApp app{{800, 300}};
		ui::EventDisplay& display;
	};
}

TEST_CASE("UI tests run GNUbase's UI on the dummy device, with its fonts and theme, at a fixed dp ratio", "[ui]")
{
	test::UiApp app;
	CHECK(app.call("audio.get").at("device").at("backend") == "dummy");
	CHECK(app.shell().theme() == ui::g_defaultTheme);
	REQUIRE(app.shell().windows().size() == 1);
	CHECK(app.main().main());

	// The same dp ratio gives the same pixels on every monitor. The theme styles the document's first box.
	app.show("sample.rml");
	for (const int ratio : {1, 2})
	{
		app.setDpRatio(static_cast<float>(ratio));
		CHECK(app.main().dpRatio() == static_cast<float>(ratio));
		const std::string name = "app-sample-" + std::to_string(ratio) + "x";
		test::UiApp::checkGolden(name, app.screenshot(name, {320, 240}));
	}
}

TEST_CASE("UI tests click through GNUbase's UI and read it back, as MCP's UI tools do", "[ui]")
{
	test::UiApp app;
	DialogOpener opener;
	app.show("automation.rml").GetElementById("open")->AddEventListener(Rml::EventId::Click, &opener);

	CHECK(
		app.ui().click(std::nullopt, selector("#open"), ui::MouseButton::Left, 1, {}).at("element").at("id") == "open");
	const auto dialog = [&app] { return app.ui().dom(std::nullopt, "#message", 0).at("elements"); };
	REQUIRE(app.waitUntil([&dialog] { return !dialog().empty(); }));
	CHECK(dialog()[0].at("text") == "Enter a new name for Audio 01.");

	app.ui().click(std::nullopt, selector("#name"), ui::MouseButton::Left, 1, {});
	app.ui().key(std::nullopt, "Ctrl+A");
	app.ui().text(std::nullopt, "Bass");
	CHECK(app.ui().dom(std::nullopt, "#name", 0).at("elements")[0].at("value") == "Bass");
	const ui::Image field = app.ui().screenshot(std::nullopt, selector("#name"));
	CHECK(field.width == static_cast<int>(200.0F * app.main().dpRatio()));
}

TEST_CASE("settings changed through the engine restyle GNUbase's UI", "[ui]")
{
	test::UiApp app;
	app.show("widgets.rml");
	app.setDpRatio(1.0F);
	const ui::Image before = app.screenshot("app-theme-before", {560, 360});

	app.call("settings.set", {{"key", "ui.theme"}, {"value", "dark"}});
	REQUIRE(app.waitUntil([&app] { return app.shell().theme() == "dark"; }));
	const ui::Image dark = app.screenshot("app-theme-dark", {560, 360});
	CHECK(!test::compareImages(dark, before, {8}).equal);

	// ui.scale applies on top of the monitor's scale.
	const float monitorScale = app.main().dpRatio() / app.shell().uiScale();
	app.call("settings.set", {{"key", "ui.scale"}, {"value", 2.0}});
	REQUIRE(
		app.waitUntil([&app, monitorScale] { return std::abs(app.main().dpRatio() - (2.0F * monitorScale)) < 0.01F; }));
}

TEST_CASE("the project window shows events and the project cursor where the engine has them", "[ui]")
{
	test::UiApp app({800, 300});
	ui::EventDisplay& display = app.appUi().showProject();
	app.setDpRatio(1.0F);
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	app.call("midiPart.add", // from 1 to 3 seconds at 120 BPM
		{{"track", track.at("id")}, {"position", 2 * time::g_ticksPerQuarter},
			{"length", 4 * time::g_ticksPerQuarter}});
	app.call("transport.locate", {{"position", 72000}}); // 1.5 seconds
	const auto element = [&app](const char* selector)
	{ return app.ui().dom(std::nullopt, selector, 0).at("elements").at(0); };
	REQUIRE(app.waitUntil(
		[&]
		{
			auto* lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && !lanes->lanes().empty() && !lanes->lanes()[0].events.empty() &&
				display.cursor() == 1.5;
		}));
	app.shell().step(std::chrono::milliseconds(0));

	// As MCP's UI tools see it: the view in the canvas elements' attributes, the cursor 1.5 seconds right of the
	// lanes' left edge at 0.01 seconds per dp.
	const Json lanes = element("event-lanes.events");
	CHECK(lanes.at("attributes").at("scale") == "0.01");
	CHECK(element(".project-cursor").at("box").at("x").get<int>() - lanes.at("box").at("x").get<int>() == 150);
	test::UiApp::checkGolden("event-display", app.screenshot("event-display", {800, 300}));

	// Ctrl and the mouse wheel zoom in around the mouse, through the UI tools.
	const ui::Target over{
		"", 0, ui::Point{lanes.at("box").at("x").get<int>() + 150, lanes.at("box").at("y").get<int>() + 10}};
	app.ui().wheel(std::nullopt, over, 0.0F, -3.0F, {"ctrl"});
	CHECK(display.view().scale < 0.01);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(element(".project-cursor").at("box").at("x").get<int>() - lanes.at("box").at("x").get<int>() == 150);
}

TEST_CASE("the ruler's time format menu opens from its arrow button and with a right click in the ruler", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	CHECK(!visible(app, "#ruler-format-menu"));

	// The arrow at the ruler's right end opens it; the ruler's format is checked.
	click(app, selector(".ruler-format"));
	REQUIRE(visible(app, "#ruler-format-menu"));
	CHECK(hasClass(app, "#ruler-format-menu > [format=barsBeats]", "checked"));
	click(app, menuItem(app, "#ruler-format-menu", "Seconds"));
	CHECK(window.display.rulerFormat() == ui::TimeFormat::Seconds);
	CHECK(element(app, "ruler").at("attributes").at("format") == "seconds");
	CHECK(hasClass(app, "#ruler-format-menu > [format=seconds]", "checked"));
	CHECK(!hasClass(app, "#ruler-format-menu > [format=barsBeats]", "checked"));

	// A right click anywhere in the ruler opens it at the mouse.
	const Json ruler = element(app, "ruler").at("box");
	const ui::Point at{ruler.at("x").get<int>() + 100, ruler.at("y").get<int>() + 4};
	app.ui().click(std::nullopt, {"", 0, at}, ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#ruler-format-menu"));
	CHECK(element(app, "#ruler-format-menu").at("box").at("x") == at.x);
	const std::vector<std::pair<std::string, ui::TimeFormat>> formats = {{"Timecode", ui::TimeFormat::Timecode},
		{"Samples", ui::TimeFormat::Samples}, {"fps (User)", ui::TimeFormat::UserFrames},
		{"Bars+Beats", ui::TimeFormat::BarsBeats}};
	for (const auto& [text, format] : formats)
	{
		if (!visible(app, "#ruler-format-menu"))
			click(app, selector(".ruler-format"));
		click(app, menuItem(app, "#ruler-format-menu", text));
		CHECK(window.display.rulerFormat() == format);
	}
}

TEST_CASE("the zoom presets menu adds, applies and organizes presets, and zooms full and to cycle markers", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	ui::EventDisplay& display = window.display;
	const auto presets = [&app] { return app.call("settings.get", {{"key", "ui.zoomPresets"}}).at("value"); };

	// Add: the zoom as it is, under a name typed into a dialog. Escape cancels it.
	display.zoomTo(0.0, 4.0);
	choose(app, "#zoom-presets", "#zoom-presets-menu", "Add...");
	REQUIRE(app.waitUntil([&app] { return count(app, "#zoom-preset-name") == 1; }));
	app.ui().text(std::nullopt, "Detail");
	app.ui().key(std::nullopt, "Escape");
	REQUIRE(app.waitUntil([&app] { return count(app, "#zoom-preset-name") == 0; }));
	choose(app, "#zoom-presets", "#zoom-presets-menu", "Add...");
	REQUIRE(app.waitUntil([&app] { return count(app, "#zoom-preset-name") == 1; }));
	app.ui().text(std::nullopt, "Detail");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&display] { return display.zoomPresets().size() == 1; }));
	CHECK(presets() == Json::array({{{"name", "Detail"}, {"seconds", 4.0}}}));

	// The presets come first in the menu; one applies, keeping the left edge.
	display.zoomFull();
	display.scrollTo(7.0);
	choose(app, "#zoom-presets", "#zoom-presets-menu", "Detail");
	CHECK(display.visibleLength() == Catch::Approx(4.0));
	CHECK(display.view().start == Catch::Approx(7.0));
	choose(app, "#zoom-presets", "#zoom-presets-menu", "Zoom Full");
	CHECK(display.view().start == 0.0);
	CHECK(display.visibleLength() == Catch::Approx(ui::g_minimumProjectLength));

	// The project's cycle markers follow Zoom Full; one zooms to its range.
	app.call("marker.add",
		{{"type", "cycle"}, {"name", "Chorus"}, {"position", 8 * time::g_ticksPerQuarter},
			{"end", 16 * time::g_ticksPerQuarter}}); // from 4 to 8 seconds at 120 BPM
	REQUIRE(app.waitUntil([&app] { return count(app, "#zoom-presets-menu > menuitem[cycle-marker]") == 1; }));
	choose(app, "#zoom-presets", "#zoom-presets-menu", "Chorus");
	CHECK(display.view().start == Catch::Approx(4.0));
	CHECK(display.visibleLength() == Catch::Approx(4.0));

	// Organize: a dialog lists the presets to rename and delete; OK applies the changes, Cancel drops them.
	app.call("settings.set",
		{{"key", "ui.zoomPresets"},
			{"value",
				Json::array({{{"name", "Detail"}, {"seconds", 4.0}}, {{"name", "Overview"}, {"seconds", 120.0}}})}});
	REQUIRE(app.waitUntil([&app] { return count(app, "#zoom-presets-menu > menuitem[preset]") == 2; }));
	const auto organize = [&app]
	{
		choose(app, "#zoom-presets", "#zoom-presets-menu", "Organize...");
		REQUIRE(app.waitUntil([&app] { return count(app, "#zoom-presets-dialog") == 1; }));
		REQUIRE(count(app, "#zoom-presets-dialog .preset") == 2);
	};
	organize();
	click(app, {"#zoom-presets-dialog .preset button", 1, std::nullopt});
	CHECK(!visible(app, "#zoom-presets-dialog .preset.deleted"));
	click(app, selector("#zoom-presets-dialog [result=cancel]"));
	REQUIRE(app.waitUntil([&app] { return count(app, "#zoom-presets-dialog") == 0; }));
	CHECK(display.zoomPresets().size() == 2);

	organize();
	click(app, {"#zoom-presets-dialog .preset input", 0, std::nullopt});
	app.ui().key(std::nullopt, "Ctrl+A");
	app.ui().text(std::nullopt, "Close-up");
	click(app, {"#zoom-presets-dialog .preset button", 1, std::nullopt}); // deletes Overview
	click(app, selector("#zoom-presets-dialog button.primary"));
	REQUIRE(app.waitUntil([&app] { return count(app, "#zoom-presets-dialog") == 0; }));
	CHECK(display.zoomPresets() == Json::array({{{"name", "Close-up"}, {"seconds", 4.0}}}));
	REQUIRE(app.waitUntil([&] { return presets() == display.zoomPresets(); }));
	REQUIRE(app.waitUntil([&app] { return count(app, "#zoom-presets-menu > menuitem[preset]") == 1; }));
	click(app, selector("#zoom-presets"));
	CHECK(element(app, "#zoom-presets-menu > menuitem[preset]").at("text") == "Close-up");
}

TEST_CASE("the auto-scroll button switches auto-scroll, its menu sets the mode, and it shows suspension", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	ui::EventDisplay& display = window.display;

	click(app, selector("#auto-scroll"));
	CHECK(display.autoScroll());
	CHECK(hasClass(app, "#auto-scroll", "checked"));

	// The settings menu sets the mode and whether editing suspends auto-scroll, each checked while it's on.
	click(app, selector("#auto-scroll-settings"));
	CHECK(hasClass(app, "#page-scroll", "checked"));
	CHECK(!hasClass(app, "#stationary-cursor", "checked"));
	click(app, selector("#stationary-cursor"));
	CHECK(display.autoScrollMode() == ui::AutoScrollMode::Stationary);
	choose(app, "#auto-scroll-settings", "#auto-scroll-menu", "Suspend Auto-Scroll when Editing");
	CHECK(display.suspendAutoScrollWhenEditing());
	click(app, selector("#auto-scroll-settings"));
	CHECK(hasClass(app, "#stationary-cursor", "checked"));
	CHECK(!hasClass(app, "#page-scroll", "checked"));
	CHECK(hasClass(app, "#suspend-auto-scroll", "checked"));
	app.ui().key(std::nullopt, "Escape");

	// While playing, a click in the event lanes suspends auto-scroll, which the button shows; a click on the button
	// resumes it.
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	app.call("transport.play");
	app.call("transport.locate", {{"position", 48000 * 30}});
	REQUIRE(app.waitUntil([&display] { return display.view().start > 20.0; })); // the cursor in the middle
	const Json lanes = element(app, "event-lanes.events").at("box");
	click(app, {"", 0, ui::Point{lanes.at("x").get<int>() + 100, lanes.at("y").get<int>() + 10}});
	CHECK(display.autoScrollSuspended());
	CHECK(hasClass(app, "#auto-scroll", "suspended"));
	click(app, selector("#auto-scroll"));
	CHECK(display.autoScroll());
	CHECK(!display.autoScrollSuspended());
	CHECK(hasClass(app, "#auto-scroll", "checked"));
	CHECK(!hasClass(app, "#auto-scroll", "suspended"));

	// Another click switches it off.
	click(app, selector("#auto-scroll"));
	CHECK(!display.autoScroll());
	CHECK(!hasClass(app, "#auto-scroll", "checked"));
	app.call("transport.stop");
	app.call("audio.stop");
}

// UI tests in Lua (#208): ctest runs each tests/lua/ui/*.lua through this test case, which GNUBASE_LUA_UI_TEST tells
// which file, in the source folder. The file runs in the app's UI Lua on the UI thread, and prints its report.
TEST_CASE("a UI test written in Lua passes", "[.][ui][lua-ui]")
{
	const char* variable = std::getenv("GNUBASE_LUA_UI_TEST");
	const std::string file = variable != nullptr ? variable : "";
	REQUIRE(!file.empty());
	test::UiApp app;
	const std::optional<std::string> error = app.lua().runFile(std::filesystem::u8path(file));
	INFO(error.value_or(""));
	// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
	CHECK(!error.has_value());
}

TEST_CASE("keys zoom the project window", "[ui]")
{
	test::UiApp app({800, 300});
	const ui::EventDisplay& display = app.appUi().showProject();
	const double scale = display.view().scale;

	// The reference's keys in the project zone, which the project window has when it opens.
	app.ui().key(std::nullopt, "H");
	CHECK(display.view().scale == scale / 2);
	app.ui().key(std::nullopt, "Shift+H");
	CHECK(display.view().trackZoom > 1.0);
	app.ui().key(std::nullopt, "Shift+G");
	CHECK(display.view().trackZoom == 1.0);

	// And after a click into the event display.
	app.ui().click(std::nullopt, {".event-area", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.ui().key(std::nullopt, "G");
	CHECK(display.view().scale == scale);

	// Zoom undo through its command, as a menu item runs it.
	app.keys().run("zoom.undo", Json::object(), app.main(), nullptr);
	CHECK(display.view().scale == scale / 2);
	CHECK(app.keys().keysFor("zoom.in") == std::vector<std::string>{"H"});

	app.ui().key(std::nullopt, "Shift+F");
	CHECK(display.view().start == 0.0);
	CHECK(std::abs(display.visibleLength() - display.projectLength()) < 1e-9);
	app.ui().key(std::nullopt, "F");
	CHECK(display.autoScroll());
}

TEST_CASE("keys edit the selected events in the project window", "[ui]")
{
	test::UiApp app({800, 300});
	ui::EventDisplay& display = app.appUi().showProject();
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	const auto part = app.call("midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 4 * 1720320}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	const auto parts = [&app] { return app.call("project.get").at("midiParts"); };

	// The reference's keys (manual p. 1564): select all, duplicate, mute, nudge, delete with Backspace.
	// Until the display shows the part.
	REQUIRE(app.waitUntil(
		[&app, &display, part]
		{
			app.ui().key(std::nullopt, "Mod+A");
			return display.selection() == std::vector<uint64_t>{part};
		}));
	app.ui().key(std::nullopt, "Mod+D");
	REQUIRE(app.waitUntil([&parts] { return parts().size() == 2; }));
	REQUIRE(
		app.waitUntil([&display, part] { return display.selection().size() == 1 && display.selection()[0] != part; }));
	app.ui().key(std::nullopt, "Shift+M");
	REQUIRE(app.waitUntil([&parts] { return parts()[1].at("mute") == true; }));
	app.ui().key(std::nullopt, "Mod+Right");
	REQUIRE(app.waitUntil([&parts] { return parts()[1].at("position") == 5 * 1720320; }));
	app.ui().key(std::nullopt, "Backspace");
	REQUIRE(app.waitUntil([&parts] { return parts().size() == 1; }));
	CHECK(app.keys().keysFor("edit.delete") == std::vector<std::string>{"Delete", "Backspace"});
}
