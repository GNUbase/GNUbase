// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of GNUbase's UI as the app shows it (#59), through the harness in uiapp.h: the app's own UI setup on the
// dummy device, driven with ui::Automation as MCP's UI tools drive it, with screenshots compared with golden images.
#include "uiapp.h"

#include "engine/document/snap.h"
#include "engine/time/tempomap.h"
#include "scripting/runtime.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/eventdisplay.h"
#include "ui/rmlui.h"
#include "ui/themes.h"
#include "ui/timeformat.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Property.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
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
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "Bass");
	CHECK(app.ui().dom(std::nullopt, "#name", 0).at("elements")[0].at("value") == "Bass");
	const ui::Image field = app.ui().screenshot(std::nullopt, selector("#name"));
	CHECK(field.width == static_cast<int>(200.0F * app.main().dpRatio()));
}

TEST_CASE("UI tests run the UI from another resources folder, which a restart keeps", "[ui]")
{
	// A copy of GNUbase's resources with a button in the transport controls that has a command that doesn't exist.
	const test::TempDir folder;
	const std::filesystem::path resources = folder.path() / "resources";
	std::filesystem::copy(GNUBASE_RESOURCES_DIR, resources, std::filesystem::copy_options::recursive);
	const std::filesystem::path file = resources / "ui" / "transport.rml";
	std::string text = test::readText(file);
	const std::string from = R"(command="locate.nextMarker")";
	const size_t at = text.find(from);
	REQUIRE(at != std::string::npos);
	text.replace(at, from.size(), R"(command="locate.nextMarkr")");
	std::ofstream(file, std::ios::binary) << text;

	// The transport panel's controls are refused, so it closes again; the time display, whose controls are fine,
	// opens from the same copy. A restart that fell back to GNUbase's resources would open the transport panel.
	test::UiApp app({800, 600}, resources);
	for (int run = 0; run < 2; ++run)
	{
		INFO((run == 0 ? "started" : "restarted"));
		app::AppUi& appUi = app.appUi();
		appUi.toggleTransportPanel();
		app.shell().step(std::chrono::milliseconds(0));
		CHECK(appUi.transportPanel() == nullptr);
		CHECK(app.shell().windows().size() == 1);
		appUi.toggleTimeDisplay();
		app.shell().step(std::chrono::milliseconds(0));
		CHECK(appUi.timeDisplay() != nullptr);
		CHECK(app.shell().windows().size() == 2);
		appUi.toggleTimeDisplay();
		app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(appUi.timeDisplay() == nullptr);
		if (run == 0)
			app.restart();
	}
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
	app.call("project.set", {{"length", 60.0}}); // the overview line as the golden image has it
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

TEST_CASE("the project cursor and the cross-hair cursor are drawn only inside the event display's time area", "[ui]")
{
	// CI's window size, with the left zone and the track list left of the event display and the right zone right of it
	// (#731, #788).
	test::UiApp app({1024, 600});
	ui::EventDisplay& display = app.appUi().showProject();
	app.setDpRatio(1.0F);
	app.ui().key(std::nullopt, "Mod+Alt+L");
	app.ui().key(std::nullopt, "Mod+Alt+R");
	app.call("track.add", {{"type", "midi"}});
	app.call("transport.locate", {{"position", 72000}}); // 1.5 seconds: 150 dp into the view at 0.01 seconds per dp
	REQUIRE(app.waitUntil(
		[&app, &display]
		{
			return visible(app, "#left-zone") && visible(app, "#right-zone") &&
				count(app, ".track-list listitem") == 1 && display.cursor() == 1.5;
		}));
	app.shell().step(std::chrono::milliseconds(0));
	const auto x = [&app](const char* selector) { return element(app, selector).at("box").at("x").get<int>(); };
	const int timelineLeft = x(".event-display .timeline");
	const int timelineRight = timelineLeft + element(app, ".event-display .timeline").at("box").at("width").get<int>();
	CHECK(x(".project-cursor") - timelineLeft == 150);
	// 400 dp left of the timeline is past the track list and its splitter, inside the left zone.
	const int leftZoneLeft = x("#left-zone");
	const int leftZoneRight = leftZoneLeft + element(app, "#left-zone").at("box").at("width").get<int>();
	CHECK(leftZoneLeft < timelineLeft - 400);
	CHECK(timelineLeft - 400 < leftZoneRight);

	// The zones beside the event display as they look with the cursor inside the view; it must never change them.
	const ui::Image leftZone = app.ui().screenshot(std::nullopt, selector("#left-zone"));
	const ui::Image trackList = app.ui().screenshot(std::nullopt, selector("#track-list"));
	const ui::Image rightZone = app.ui().screenshot(std::nullopt, selector("#right-zone"));
	const auto unchanged = [&app, &leftZone, &trackList, &rightZone]
	{
		app.shell().step(std::chrono::milliseconds(0));
		for (const auto& [zone, before] : {std::make_pair("#left-zone", &leftZone),
				 std::make_pair("#track-list", &trackList), std::make_pair("#right-zone", &rightZone)})
		{
			const test::ImageDiff diff =
				test::compareImages(app.ui().screenshot(std::nullopt, selector(zone)), *before, {});
			INFO(zone << ": " << diff.report);
			CHECK(diff.equal);
		}
	};

	// The view scrolled right past the cursor: 100 dp left of the view's start, it would be over the track list.
	display.scrollTo(2.5);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(x(".project-cursor") == timelineLeft - 100);
	unchanged();

	// 400 dp left of the view's start, it would be over the left zone.
	display.scrollTo(5.5);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(x(".project-cursor") == timelineLeft - 400);
	unchanged();

	// The cursor 100 dp right of the view's end, where it would be over the right zone.
	display.scrollTo(0.0);
	const int64_t end = std::llround((display.visibleLength() + 1.0) * 48000.0);
	app.call("transport.locate", {{"position", end}});
	REQUIRE(app.waitUntil([&display, end] { return display.cursor() == static_cast<double>(end) / 48000.0; }));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(x(".project-cursor") == timelineRight + 100);
	unchanged();

	// The vertical cross-hair cursor shows over the event lanes. Nothing moves it outside them, but if anything did,
	// it would be clipped as the project cursor is: left of the lanes over the track list and the left zone, right of
	// them over the right zone.
	const Json lanes = element(app, "event-lanes.events").at("box");
	app.ui().move(std::nullopt, {"", 0, ui::Point{lanes.at("x").get<int>() + 50, lanes.at("y").get<int>() + 10}});
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(visible(app, ".cross-hair-vertical"));
	CHECK(x(".cross-hair-vertical") == timelineLeft + 50);
	Rml::Element* vertical = app.main().context().GetDocument(0)->QuerySelector(".cross-hair-vertical");
	REQUIRE(vertical != nullptr);
	for (const int left : {-100, -400, timelineRight - timelineLeft + 100})
	{
		INFO(left);
		vertical->SetProperty(Rml::PropertyId::Left, Rml::Property(static_cast<float>(left), Rml::Unit::DP));
		app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(visible(app, ".cross-hair-vertical"));
		unchanged();
	}
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
	CHECK(app.call("project.get").at("timeFormat") == "barsBeats");

	// With Ctrl (Cmd), a format is the primary time format too, for all windows (reference manual p. 69).
	click(app, selector(".ruler-format"));
	app.ui().click(std::nullopt, menuItem(app, "#ruler-format-menu", "Timecode"), ui::MouseButton::Left, 1, {"ctrl"});
	CHECK(window.display.rulerFormat() == ui::TimeFormat::Timecode);
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("timeFormat") == "timecode"; }));
	click(app, selector(".ruler-format"));
	click(app, menuItem(app, "#ruler-format-menu", "Samples"));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.display.rulerFormat() == ui::TimeFormat::Samples);
	CHECK(app.call("project.get").at("timeFormat") == "timecode");
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
	CHECK(display.visibleLength() == Catch::Approx(ui::g_defaultProjectLength));

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
	app.ui().key(std::nullopt, "Mod+A");
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

TEST_CASE("the snap buttons, the snap and grid type menus and J change the project's snap settings", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	ui::EventDisplay& display = window.display;
	const auto snap = [&app] { return app.call("snap.get"); };

	// The toggle button and J switch snap; the button shows it.
	click(app, selector("#snap"));
	REQUIRE(app.waitUntil([&snap] { return snap().at("on") == true; }));
	REQUIRE(app.waitUntil([&app] { return hasClass(app, "#snap", "checked"); }));
	REQUIRE(app.waitUntil([&display] { return display.snap().on; }));
	app.ui().key(std::nullopt, "J");
	REQUIRE(app.waitUntil([&snap] { return snap().at("on") == false; }));
	REQUIRE(app.waitUntil([&app] { return !hasClass(app, "#snap", "checked"); }));
	click(app, selector("#snap-zero-crossing"));
	REQUIRE(app.waitUntil([&snap] { return snap().at("zeroCrossing") == true; }));

	// The snap type menu checks the type; its button shows it.
	choose(app, "#snap-type", "#snap-type-menu", "Events + Grid + Cursor");
	REQUIRE(app.waitUntil([&snap] { return snap().at("type") == "eventsGridCursor"; }));
	REQUIRE(app.waitUntil([&app] { return element(app, "#snap-type").value("text", "") == "Events + Grid + Cursor"; }));
	click(app, selector("#snap-type"));
	CHECK(hasClass(app, "#snap-type-menu > menuitem[snap-type=eventsGridCursor]", "checked"));
	CHECK(!hasClass(app, "#snap-type-menu > menuitem[snap-type=grid]", "checked"));
	app.ui().key(std::nullopt, "Escape");

	// The grid type menu has the ruler format's grid types (reference manual pp. 107-108).
	choose(app, "#grid-type", "#grid-type-menu", "Use Quantize");
	REQUIRE(app.waitUntil([&snap] { return snap().at("grid") == "quantize"; }));
	REQUIRE(app.waitUntil([&app] { return element(app, "#grid-type").value("text", "") == "Use Quantize"; }));
	CHECK(count(app, "#grid-type-menu > menuitem") == 4);
	display.setRulerFormat(ui::TimeFormat::Seconds);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(count(app, "#grid-type-menu > menuitem") == 4);
	choose(app, "#grid-type", "#grid-type-menu", "10ms");
	REQUIRE(app.waitUntil([&snap] { return snap().at("grid") == "10ms"; }));
	display.setRulerFormat(ui::TimeFormat::Timecode);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(count(app, "#grid-type-menu > menuitem") == 6);
	choose(app, "#grid-type", "#grid-type-menu", "1/4 Frame");
	REQUIRE(app.waitUntil([&snap] { return snap().at("grid") == "quarterFrame"; }));
	display.setRulerFormat(ui::TimeFormat::Samples);
	app.shell().step(std::chrono::milliseconds(0));
	choose(app, "#grid-type", "#grid-type-menu", "44100 Samples");
	REQUIRE(app.waitUntil([&app] { return element(app, "#grid-type").value("text", "") == "44100 Samples"; }));
	click(app, selector("#grid-type"));
	CHECK(hasClass(app, "#grid-type-menu > menuitem[grid-type=44100samples]", "checked"));
	app.ui().key(std::nullopt, "Escape");

	// Each format keeps its grid type (#413): back in seconds and in bars and beats, the button shows the one chosen
	// there, the menu checks it, and it is the project's grid.
	display.setRulerFormat(ui::TimeFormat::Seconds);
	REQUIRE(app.waitUntil([&app] { return element(app, "#grid-type").value("text", "") == "10ms"; }));
	REQUIRE(app.waitUntil([&snap] { return snap().at("format") == "seconds" && snap().at("grid") == "10ms"; }));
	click(app, selector("#grid-type"));
	CHECK(hasClass(app, "#grid-type-menu > menuitem[grid-type=10ms]", "checked"));
	app.ui().key(std::nullopt, "Escape");
	display.setRulerFormat(ui::TimeFormat::BarsBeats);
	REQUIRE(app.waitUntil([&app] { return element(app, "#grid-type").value("text", "") == "Use Quantize"; }));
	REQUIRE(app.waitUntil([&snap] { return snap().at("grid") == "quantize"; }));
	click(app, selector("#grid-type"));
	CHECK(hasClass(app, "#grid-type-menu > menuitem[grid-type=quantize]", "checked"));
	app.ui().key(std::nullopt, "Escape");
}

// #565: the grid type menu has the engine's grid types of the ruler's format, and a pick sends that format with it.
TEST_CASE("the grid type menu has the format's grid types and picks them in the format shown", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	ui::EventDisplay& display = window.display;
	const auto snap = [&app] { return app.call("snap.get"); };

	for (size_t i = 0; i < engine::g_gridFormatCount; ++i)
	{
		const auto format = static_cast<engine::GridFormat>(i);
		const ui::TimeFormat ruler =
			ui::timeFormatFromName(engine::gridFormatName(format)).value_or(ui::TimeFormat::BarsBeats);
		CHECK(ui::timeFormatName(ruler) == engine::gridFormatName(format));
		display.setRulerFormat(ruler);
		app.shell().step(std::chrono::milliseconds(0));
		std::vector<std::string> items;
		const Json menu = app.ui().dom(std::nullopt, "#grid-type-menu > menuitem", 0);
		for (const Json& item : menu.at("elements"))
			items.push_back(item.at("attributes").at("grid-type").get<std::string>());
		std::vector<std::string> expected;
		for (const engine::GridType grid : engine::gridTypes(format))
			expected.emplace_back(engine::gridTypeName(grid));
		INFO(engine::gridFormatName(format));
		CHECK(items == expected);
	}

	// The ruler shows fps (User) when the engine switches to timecode from elsewhere: a grid type picked from the
	// menu that is still shown is the user frames', and the ruler stays there.
	REQUIRE(app.waitUntil([&snap] { return snap().at("format") == "userFrames"; }));
	click(app, selector("#grid-type"));
	REQUIRE(visible(app, "#grid-type-menu"));
	app.call("snap.set", {{"format", "timecode"}});
	click(app, menuItem(app, "#grid-type-menu", "1/2 Frame"));
	for (int i = 0; i < 20; ++i)
	{
		app.shell().step(std::chrono::milliseconds(5));
		CHECK(display.rulerFormat() == ui::TimeFormat::UserFrames);
	}
	REQUIRE(app.waitUntil([&snap] { return snap().at("grids").at("userFrames") == "halfFrame"; }));
	CHECK(snap().at("format") == "userFrames");
	CHECK(snap().at("grids").at("timecode") == "frame");
	CHECK(display.rulerFormat() == ui::TimeFormat::UserFrames);
}

TEST_CASE("the toolbar's tool buttons and the keys choose the tools, a second click opens a tool's modes", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	const ui::EventDisplay& display = window.display;

	// A tool button chooses its tool and shows it checked.
	click(app, selector("button[tool=split]"));
	CHECK(display.tool() == ui::Tool::Split);
	CHECK(hasClass(app, "button[tool=split]", "checked"));
	CHECK(!hasClass(app, "button[tool=objectSelection]", "checked"));
	CHECK(element(app, "#event-display").at("attributes").at("tool") == "split");

	// The reference's keys (manual p. 1569): 1 to 9 and F9 and F10 go through the tools.
	const std::vector<std::pair<const char*, ui::Tool>> keys = {{"1", ui::Tool::ObjectSelection},
		{"2", ui::Tool::RangeSelection}, {"3", ui::Tool::Split}, {"4", ui::Tool::Glue}, {"5", ui::Tool::Erase},
		{"6", ui::Tool::Zoom}, {"7", ui::Tool::Mute}, {"8", ui::Tool::Draw}, {"9", ui::Tool::Play},
		{"F9", ui::Tool::Line}, {"F10", ui::Tool::Play}};
	for (const auto& [key, tool] : keys)
	{
		INFO(key);
		app.ui().key(std::nullopt, key);
		CHECK(display.tool() == tool);
	}
	app.ui().key(std::nullopt, "Alt+Shift+1");
	CHECK(display.combineSelectionTools());
	REQUIRE(app.waitUntil([&app] { return hasClass(app, "#combine-selection-tools", "checked"); }));
	click(app, selector("#combine-selection-tools"));
	CHECK(!display.combineSelectionTools());

	// A second click on the object selection tool opens its modes: sizing moves contents.
	app.ui().key(std::nullopt, "1");
	choose(app, "button[tool=objectSelection]", "#object-selection-menu", "Sizing Moves Contents");
	CHECK(display.sizingMovesContents());
	click(app, selector("button[tool=objectSelection]"));
	CHECK(hasClass(app, "#object-selection-menu > menuitem[sizing=moveContents]", "checked"));
	CHECK(!hasClass(app, "#object-selection-menu > menuitem[sizing=normal]", "checked"));
	app.ui().key(std::nullopt, "Escape");
	// The line tool's: the first click chooses it, the second opens its modes.
	click(app, selector("button[tool=line]"));
	CHECK(display.tool() == ui::Tool::Line);
	CHECK(!visible(app, "#line-menu"));
	choose(app, "button[tool=line]", "#line-menu", "Parabola");
	CHECK(display.lineMode() == ui::LineMode::Parabola);
	// The play tool's: play and scrub (reference manual pp. 252-253).
	click(app, selector("button[tool=play]"));
	CHECK(!visible(app, "#play-menu"));
	choose(app, "button[tool=play]", "#play-menu", "Scrub");
	CHECK(display.playMode() == ui::PlayMode::Scrub);
	CHECK(element(app, "#event-display").at("attributes").at("play-mode") == "scrub");
	click(app, selector("button[tool=play]"));
	CHECK(hasClass(app, "#play-menu > menuitem[play-mode=scrub]", "checked"));
	CHECK(!hasClass(app, "#play-menu > menuitem[play-mode=play]", "checked"));
	app.ui().key(std::nullopt, "Escape");
}

TEST_CASE(
	"a right click in the event display opens the toolbox or the context menu, by the setting and modifiers", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	const ui::EventDisplay& display = window.display;
	const Json lanes = element(app, "event-lanes.events").at("box");
	const ui::Point at{lanes.at("x").get<int>() + 100, lanes.at("y").get<int>() + 20};
	const auto rightClick = [&app, &at](const std::vector<std::string>& modifiers)
	{
		app.ui().click(std::nullopt, {"", 0, at}, ui::MouseButton::Right, 1, modifiers);
		app.shell().step(std::chrono::milliseconds(0));
	};

	// Off, as by default: the context menu, and with any modifier key the toolbox (reference manual p. 65).
	rightClick({});
	REQUIRE(visible(app, "#event-display-menu"));
	CHECK(!visible(app, "#toolbox"));
	CHECK(element(app, "#event-display-menu").at("box").at("x") == at.x);
	app.ui().key(std::nullopt, "Escape");
	rightClick({"shift"});
	CHECK(visible(app, "#toolbox"));
	CHECK(!visible(app, "#event-display-menu"));
	app.ui().key(std::nullopt, "Escape");

	// On (p. 1602): the toolbox at the mouse, with the tool chosen checked; a click on a tool chooses it and closes
	// it. With a modifier key, the context menu, whose Tools submenu chooses tools too.
	app.call("settings.set", {{"key", "ui.toolboxOnRightClick"}, {"value", true}});
	REQUIRE(
		app.waitUntil([&app] { return element(app, ".event-area").at("attributes").at("contextmenu") == "toolbox"; }));
	rightClick({});
	REQUIRE(visible(app, "#toolbox"));
	CHECK(!visible(app, "#event-display-menu"));
	CHECK(element(app, "#toolbox").at("box").at("x") == at.x);
	CHECK(hasClass(app, "#toolbox > menuitem[tool=objectSelection]", "checked"));
	click(app, selector("#toolbox > menuitem[tool=draw]"));
	CHECK(display.tool() == ui::Tool::Draw);
	CHECK(!visible(app, "#toolbox"));
	CHECK(hasClass(app, "button[tool=draw]", "checked"));
	// The mouse rests over the lanes where the toolbox was: the pointer is the tool's at once.
	CHECK(app.waitUntil([&app] { return app.ui().windows().at("cursor") == "tool-draw"; }));
	for (const char* modifier : {"ctrl", "alt", "shift"})
	{
		INFO(modifier);
		rightClick({modifier});
		CHECK(visible(app, "#event-display-menu"));
		CHECK(!visible(app, "#toolbox"));
		app.ui().key(std::nullopt, "Escape");
	}
	rightClick({"ctrl"});
	app.ui().move(std::nullopt, selector("#tools-submenu"));
	click(app, menuItem(app, "#tools-submenu > menu", "Split"));
	CHECK(display.tool() == ui::Tool::Split);
	CHECK(!visible(app, "#event-display-menu"));
	// Its edit commands act on the selection, as the Edit menu's do.
	CHECK(count(app, "#event-display-menu > menuitem[command=edit.delete]") == 1);

	// The setting survives in the settings, for the next start; off again, the context menu.
	CHECK(app.call("settings.get", {{"key", "ui.toolboxOnRightClick"}}).at("value") == true);
	app.call("settings.set", {{"key", "ui.toolboxOnRightClick"}, {"value", false}});
	REQUIRE(app.waitUntil(
		[&app] { return element(app, ".event-area").at("attributes").at("contextmenu") == "event-display-menu"; }));
}

TEST_CASE("a right click on an event selects it for the context menu, unless it's selected already", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	const ui::EventDisplay& display = window.display;
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	for (const int quarter : {2, 6}) // from 1 to 2 and from 3 to 4 seconds at 120 BPM, 100 to 200 and 300 to 400 dp
		app.call("midiPart.add",
			{{"track", track.at("id")}, {"position", quarter * time::g_ticksPerQuarter},
				{"length", 2 * time::g_ticksPerQuarter}});
	const auto parts = [&app]
	{
		auto* lanes =
			dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
		return lanes == nullptr || lanes->lanes().empty() ? size_t{0} : lanes->lanes()[0].events.size();
	};
	REQUIRE(app.waitUntil([&parts] { return parts() == 2; }));
	const Json lanes = element(app, "event-lanes.events").at("box");
	const auto at = [&lanes](int x) { return ui::Point{lanes.at("x").get<int>() + x, lanes.at("y").get<int>() + 20}; };
	const auto press = [&app](ui::Point point, ui::MouseButton button, const std::vector<std::string>& modifiers)
	{
		app.ui().click(std::nullopt, {"", 0, point}, button, 1, modifiers);
		app.shell().step(std::chrono::milliseconds(0));
	};
	press(at(150), ui::MouseButton::Left, {});
	REQUIRE(display.selection().size() == 1);
	const uint64_t first = display.selection()[0];

	// Another event: the context menu acts on it, not on the one selected before.
	press(at(350), ui::MouseButton::Right, {});
	REQUIRE(visible(app, "#event-display-menu"));
	REQUIRE(display.selection().size() == 1);
	const uint64_t second = display.selection()[0];
	CHECK(second != first);
	click(app, selector("#event-display-menu > menuitem[command=edit.delete]"));
	REQUIRE(app.waitUntil([&parts] { return parts() == 1; }));
	CHECK(app.call("event.get", {{"events", Json::array({first})}}).at("events").size() == 1);

	// A selected event keeps the selection, the others in it too; so does a right click on nothing, and one that
	// opens the toolbox.
	app.call("midiPart.add",
		{{"track", track.at("id")}, {"position", 6 * time::g_ticksPerQuarter},
			{"length", 2 * time::g_ticksPerQuarter}});
	REQUIRE(app.waitUntil([&parts] { return parts() == 2; }));
	press(at(150), ui::MouseButton::Left, {});
	press(at(350), ui::MouseButton::Left, {"ctrl"});
	REQUIRE(display.selection().size() == 2);
	const std::vector<uint64_t> both = display.selection();
	press(at(150), ui::MouseButton::Right, {});
	CHECK(display.selection() == both);
	app.ui().key(std::nullopt, "Escape");
	press(at(500), ui::MouseButton::Right, {});
	CHECK(display.selection() == both);
	app.ui().key(std::nullopt, "Escape");
	press(at(500), ui::MouseButton::Left, {}); // selects nothing
	press(at(150), ui::MouseButton::Left, {});
	press(at(350), ui::MouseButton::Right, {"shift"});
	CHECK(visible(app, "#toolbox"));
	CHECK(display.selection() == std::vector<uint64_t>{first});
}

TEST_CASE("the context menu's range commands wait for a selection range, which a right click leaves alone", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	const ui::EventDisplay& display = window.display;
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	app.call("midiPart.add", // from 1 to 3 seconds at 120 BPM, 100 to 300 dp
		{{"track", track.at("id")}, {"position", 2 * time::g_ticksPerQuarter},
			{"length", 4 * time::g_ticksPerQuarter}});
	const auto parts = [&app] { return app.call("project.get").at("midiParts").size(); };
	REQUIRE(app.waitUntil(
		[&app]
		{
			auto* lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && !lanes->lanes().empty() && !lanes->lanes()[0].events.empty();
		}));
	const Json lanes = element(app, "event-lanes.events").at("box");
	const auto at = [&lanes](int x) { return ui::Point{lanes.at("x").get<int>() + x, lanes.at("y").get<int>() + 20}; };
	const auto rightClick = [&app](ui::Point point)
	{
		app.ui().click(std::nullopt, {"", 0, point}, ui::MouseButton::Right, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(visible(app, "#event-display-menu"));
	};
	const auto disabled = [&app](const char* command)
	{
		return element(app, std::string("#event-display-menu > menuitem[command=") + command + "]")
			.at("attributes")
			.contains("disabled");
	};
	const std::vector<const char*> rangeCommands = {"edit.cut", "edit.copy", "edit.splitRange", "edit.cropRange"};

	// The object selection tool selects the part for the menu, whose range commands have no range to act on.
	rightClick(at(200));
	CHECK(display.selection().size() == 1);
	for (const char* command : rangeCommands)
	{
		INFO(command);
		CHECK(disabled(command));
	}
	CHECK(!disabled("edit.delete"));
	CHECK(!disabled("edit.paste"));
	// A disabled item does nothing.
	click(app, selector("#event-display-menu > menuitem[command=edit.cut]"));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(parts() == 1);
	CHECK(display.selection().size() == 1);
	app.ui().key(std::nullopt, "Escape");

	// The range selection tool: a range from 1.5 to 2.5 seconds. A right click on the part leaves it unselected, and
	// the range commands act on the range.
	app.ui().key(std::nullopt, "2");
	app.ui().drag(std::nullopt, {"", 0, at(150)}, {"", 0, at(250)});
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(display.range().has_value());
	rightClick(at(200));
	CHECK(display.selection().empty());
	REQUIRE(display.range().has_value());
	REQUIRE(app.waitUntil([&] { return !disabled("edit.cut"); }));
	for (const char* command : rangeCommands)
	{
		INFO(command);
		CHECK(!disabled(command));
	}
	// Cut takes the middle of the part out: two parts are left.
	click(app, selector("#event-display-menu > menuitem[command=edit.cut]"));
	REQUIRE(app.waitUntil([&parts] { return parts() == 2; }));
}

TEST_CASE("a tool chosen with a key command shows its name for a moment, unless the setting is off", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	// RmlUi's time stands still but for advance(), so that the notification's 1.5 seconds take none.
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true);
	const std::shared_ptr<void> thaw(nullptr, [clock](void*) { clock->freeze(false); });
	const auto after = [&app, clock](double seconds)
	{
		clock->advance(seconds);
		app.shell().step(std::chrono::milliseconds(0));
		return visible(app, "#tool-notification");
	};
	CHECK(!visible(app, "#tool-notification"));
	// 8 chooses the draw tool: its name shows over the event display (reference manual p. 1602), then goes.
	app.ui().key(std::nullopt, "8");
	REQUIRE(visible(app, "#tool-notification"));
	CHECK(element(app, "#tool-notification").at("text") == "Draw");
	CHECK(after(1.4));
	CHECK(!after(0.2));
	app.ui().key(std::nullopt, "Alt+Shift+1");
	CHECK(element(app, "#tool-notification").at("text") == "Combine Selection Tools On");
	CHECK(after(1.4));
	CHECK(!after(0.2));

	// A tool button shows none: the notification is for keys.
	click(app, selector("button[tool=split]"));
	CHECK(!visible(app, "#tool-notification"));
	// Nor a key with the setting off.
	app.call("settings.set", {{"key", "ui.toolSwitchNotification"}, {"value", false}});
	app.shell().step(std::chrono::milliseconds(0));
	app.ui().key(std::nullopt, "3");
	CHECK(window.display.tool() == ui::Tool::Split);
	CHECK(!visible(app, "#tool-notification"));
}

TEST_CASE("the mouse pointer over the event display shows the tool, and the resize pointer at an event's edge", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	app.call("midiPart.add", // from 1 to 3 seconds at 120 BPM
		{{"track", track.at("id")}, {"position", 2 * time::g_ticksPerQuarter},
			{"length", 4 * time::g_ticksPerQuarter}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			auto* lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && !lanes->lanes().empty() && !lanes->lanes()[0].events.empty();
		}));
	const Json lanes = element(app, "event-lanes.events").at("box");
	const auto cursorAt = [&app, &lanes](int x, int y)
	{
		app.ui().move(std::nullopt, {"", 0, ui::Point{lanes.at("x").get<int>() + x, lanes.at("y").get<int>() + y}});
		app.shell().step(std::chrono::milliseconds(0));
		return app.ui().windows().at("cursor").get<std::string>();
	};

	// As MCP's ui_windows reports it: the OS's arrow for the object selection tool, the resize pointer at the
	// part's edge, the tool's own pointer for the others.
	CHECK(cursorAt(500, 20).empty());
	CHECK(cursorAt(101, 20) == "resize");
	app.ui().key(std::nullopt, "8");
	CHECK(cursorAt(500, 20) == "tool-draw");
	app.ui().key(std::nullopt, "3");
	CHECK(cursorAt(200, 20) == "tool-split");
	app.ui().key(std::nullopt, "2");
	CHECK(cursorAt(200, 20) == "tool-range");
	// Not over the toolbar.
	app.ui().move(std::nullopt, selector("button[tool=draw]"));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.ui().windows().at("cursor").get<std::string>().empty());
}

TEST_CASE("the scrub tool scrubs an audio track and moves the project cursor in the project window", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	const Json track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track");
	const Json media =
		app.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip");
	app.call(
		"audioEvent.add", {{"track", track.at("id")}, {"clip", clip.at("id")}, {"position", 0}, {"length", 240000}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			auto* lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && !lanes->lanes().empty() && !lanes->lanes()[0].events.empty();
		}));
	const Json lanes = element(app, "event-lanes.events");
	const auto at = [&lanes](int x, int y)
	{
		return ui::Target{
			"", 0, ui::Point{lanes.at("box").at("x").get<int>() + x, lanes.at("box").at("y").get<int>() + y}};
	};

	// 9, then Scrub from the play tool's modes, and a drag from 1 to 1.5 seconds.
	app.ui().key(std::nullopt, "9");
	choose(app, "button[tool=play]", "#play-menu", "Scrub");
	app.ui().drag(std::nullopt, at(100, 20), at(150, 20), {});
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("position") == 72000; }));
	CHECK(app.call("transport.get").at("playing") == false);
}

TEST_CASE("the split and range selection tools edit and select in the project window", "[ui]")
{
	ProjectWindow window;
	test::UiApp& app = window.app;
	const ui::EventDisplay& display = window.display;
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	app.call("midiPart.add", // from 1 to 3 seconds at 120 BPM
		{{"track", track.at("id")}, {"position", 2 * time::g_ticksPerQuarter},
			{"length", 4 * time::g_ticksPerQuarter}});
	const auto parts = [&app] { return app.call("project.get").at("midiParts"); };
	REQUIRE(app.waitUntil(
		[&app]
		{
			auto* lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && !lanes->lanes().empty() && !lanes->lanes()[0].events.empty();
		}));
	const Json lanes = element(app, "event-lanes.events");
	const auto at = [&lanes](int x, int y)
	{
		return ui::Target{
			"", 0, ui::Point{lanes.at("box").at("x").get<int>() + x, lanes.at("box").at("y").get<int>() + y}};
	};

	// 3 and a click at 2 seconds splits the part there.
	app.ui().key(std::nullopt, "3");
	click(app, at(200, 20));
	REQUIRE(app.waitUntil([&parts] { return parts().size() == 2; }));
	CHECK(parts()[1].at("position") == 4 * time::g_ticksPerQuarter);

	// The range selection tool's button, and a drag from 1.5 to 2.5 seconds.
	click(app, selector("button[tool=rangeSelection]"));
	app.ui().drag(std::nullopt, at(150, 20), at(250, 20), {});
	REQUIRE(display.range().has_value());
	CHECK(element(app, "#event-display").at("attributes").at("range") == "72000 120000");
	CHECK(hasClass(app, ".range-selection", "shown"));
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
	REQUIRE(app.waitUntil([&parts] { return parts()[1].at("position") == 8 * 1720320; })); // a bar, the grid's step
	app.ui().key(std::nullopt, "Backspace");
	REQUIRE(app.waitUntil([&parts] { return parts().size() == 1; }));
	CHECK(app.keys().keysFor("edit.delete") == std::vector<std::string>{"Delete", "Backspace"});
}

TEST_CASE("the arrow keys select events along a track, and Up and Down step the tracks and their first events", "[ui]")
{
	test::UiApp app({800, 300});
	ui::EventDisplay& display = app.appUi().showProject();
	const auto addTrack = [&app] { return app.call("track.add", {{"type", "midi"}}).at("track").at("id"); };
	const Json upper = addTrack();
	const Json empty = addTrack();
	const Json lower = addTrack();
	const auto addPart = [&app](const Json& track, int64_t position)
	{
		return app.call("midiPart.add", {{"track", track}, {"position", position}, {"length", 1720320}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t first = addPart(upper, 0);
	const uint64_t second = addPart(upper, int64_t{4} * 1720320);
	addPart(lower, int64_t{4} * 1720320); // under the second part's start, not the first on its track
	const uint64_t lowerFirst = addPart(lower, 0);
	REQUIRE(app.waitUntil(
		[&app, &display]
		{
			app.ui().key(std::nullopt, "Mod+A");
			return display.selection().size() == 4;
		}));
	using Ids = std::vector<uint64_t>;
	const auto press = [&app](const char* key)
	{
		app.ui().key(std::nullopt, key);
		app.shell().step(std::chrono::milliseconds(0));
	};
	ui::TrackList& tracks = *app.appUi().trackList();
	const auto track = [](const Json& id) { return Ids{id.get<uint64_t>()}; };

	// Left and Right: the part before or after on its track.
	display.select({first});
	press("Right");
	CHECK(display.selection() == Ids{second});
	press("Left");
	CHECK(display.selection() == Ids{first});

	// With a part selected, Up and Down still step through the tracks, and select the first part on the new one
	// (reference manual p. 218). On a track without parts the selected part stays selected.
	display.select({second});
	tracks.select(track(upper));
	press("Down");
	CHECK(tracks.selection() == track(empty));
	CHECK(display.selection() == Ids{second});
	press("Down");
	CHECK(tracks.selection() == track(lower));
	CHECK(display.selection() == Ids{lowerFirst});
	press("Up");
	CHECK(tracks.selection() == track(empty));
	CHECK(display.selection() == Ids{lowerFirst});
	press("Up");
	CHECK(tracks.selection() == track(upper));
	CHECK(display.selection() == Ids{first});

	// With no part selected, they select only the track.
	display.select({});
	press("Down");
	CHECK(tracks.selection() == track(empty));
	CHECK(display.selection().empty());
}

TEST_CASE("Mod+K opens the Repeat Events dialog, which repeats the selected events", "[ui]")
{
	test::UiApp app({800, 300});
	ui::EventDisplay& display = app.appUi().showProject();
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	const auto part = app.call("midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 4 * 1720320}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	const auto parts = [&app] { return app.call("project.get").at("midiParts"); };
	REQUIRE(app.waitUntil(
		[&app, &display, part]
		{
			app.ui().key(std::nullopt, "Mod+A");
			return display.selection() == std::vector<uint64_t>{part};
		}));

	// Escape cancels it.
	app.ui().key(std::nullopt, "Mod+K");
	REQUIRE(app.waitUntil([&app] { return count(app, "#repeat-events-dialog") == 1; }));
	app.ui().key(std::nullopt, "Escape");
	REQUIRE(app.waitUntil([&app] { return count(app, "#repeat-events-dialog") == 0; }));
	CHECK(parts().size() == 1);
	click(app, selector(".timeline-ruler")); // the project window has the focus again, for its keys

	// Shared Copies, then a count of 3 typed into the field after a double click, and Enter, which clicks OK: three
	// shared copies, one after the other.
	app.ui().key(std::nullopt, "Mod+K");
	REQUIRE(app.waitUntil([&app] { return count(app, "#repeat-events-dialog") == 1; }));
	click(app, selector("#repeat-shared"));
	app.ui().click(std::nullopt, selector("#repeat-count"), ui::MouseButton::Left, 2, {});
	app.ui().text(std::nullopt, "3");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&app] { return count(app, "#repeat-events-dialog") == 0; }));
	REQUIRE(app.waitUntil([&parts] { return parts().size() == 4; }));
	const Json all = parts();
	CHECK(all[0].at("shared") != 0);
	for (size_t i = 1; i < 4; ++i)
	{
		CHECK(all[i].at("position") == static_cast<int64_t>(i) * 4 * 1720320);
		CHECK(all[i].at("shared") == all[0].at("shared"));
	}
	REQUIRE(app.waitUntil([&display] { return display.selection().size() == 3; }));
	CHECK(app.keys().keysFor("edit.repeat").size() == 1); // Ctrl+K, Cmd+K on macOS
}

TEST_CASE("Mod+K, a count and Enter repeat the selected events", "[ui]")
{
	test::UiApp app({800, 300});
	ui::EventDisplay& display = app.appUi().showProject();
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	const auto part = app.call("midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 4 * 1720320}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	const auto parts = [&app] { return app.call("project.get").at("midiParts"); };
	REQUIRE(app.waitUntil(
		[&app, &display, part]
		{
			app.ui().key(std::nullopt, "Mod+A");
			return display.selection() == std::vector<uint64_t>{part};
		}));

	// The dialog opens with its count being edited, all of it selected: the number typed replaces it.
	app.ui().key(std::nullopt, "Mod+K");
	REQUIRE(app.waitUntil([&app] { return count(app, "#repeat-events-dialog") == 1; }));
	REQUIRE(app.waitUntil([&app] { return hasClass(app, "#repeat-count", "editing"); }));
	app.ui().text(std::nullopt, "2");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&app] { return count(app, "#repeat-events-dialog") == 0; }));
	REQUIRE(app.waitUntil([&parts] { return parts().size() == 3; }));
	const Json all = parts();
	for (size_t i = 1; i < 3; ++i)
	{
		CHECK(all[i].at("position") == static_cast<int64_t>(i) * 4 * 1720320);
		CHECK(all[i].at("shared") == 0);
	}
}
