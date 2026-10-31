// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of the transport controls (#90, ui/transport.h): every control of the project window's transport bar, the
// transport panel and the time display window, driven through ui::Automation as MCP's UI tools drive them
// (tests/uiapp.h), and the transport's key commands.
#include "uiapp.h"

#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/eventdisplay.h"
#include "ui/transport.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	ui::Target at(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	Json element(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		const Json found = app.ui().dom(window, selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		return found[0];
	}

	bool checked(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		const Json names = element(app, selector, window).value("classes", Json::array());
		return std::find(names.begin(), names.end(), "checked") != names.end();
	}

	// What a display shows: a text display's value, another element's text.
	std::string shows(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		const Json found = element(app, selector, window);
		const Json attributes = found.value("attributes", Json::object());
		if (attributes.contains("value"))
			return attributes.at("value").get<std::string>();
		return found.value("text", std::string());
	}

	void click(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		app.ui().click(window, at(selector), ui::MouseButton::Left, 1, {});
	}

	Json transport(test::UiApp& app)
	{
		return app.call("transport.get");
	}

	// The project window with its transport bar shown.
	ui::EventDisplay& showBar(test::UiApp& app)
	{
		ui::EventDisplay& display = app.appUi().showProject();
		app.appUi().docking().setVisible("project", "transportBar", true);
		app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(app.appUi().transportBar() != nullptr);
		REQUIRE(element(app, "#transport-bar").at("visible").get<bool>());
		return display;
	}

	// Double-clicks a display, types into it and presses Enter.
	void type(test::UiApp& app, const std::string& selector, const std::string& text,
		std::optional<ui::WindowId> window = std::nullopt)
	{
		app.ui().click(window, at(selector), ui::MouseButton::Left, 2, {});
		app.ui().key(window, "Ctrl+A");
		app.ui().text(window, text);
		app.ui().key(window, "Enter");
	}

	// Waits until the condition about the engine's transport holds.
	bool waitFor(test::UiApp& app, const std::function<bool(const Json&)>& condition)
	{
		return app.waitUntil([&app, &condition] { return condition(transport(app)); });
	}
}

TEST_CASE("the transport bar's buttons run the transport and show its state", "[ui]")
{
	test::UiApp app({1400, 700});
	showBar(app);
	app.call("transport.set", {{"left", 48000}, {"right", 96000}});

	// Locators: 120 BPM at 48 kHz, so a quarter is 24000 samples.
	REQUIRE(app.waitUntil([&app] { return shows(app, "#left-locator") == "1.3.1.0"; }));
	CHECK(shows(app, "#right-locator") == "2.1.1.0");
	CHECK(shows(app, "#locator-range") == "0.2.0.0");
	click(app, "#go-to-right");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 96000; }));
	click(app, "#go-to-left");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));

	// Rewind and forward move by a second.
	click(app, "#forward");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 96000; }));
	click(app, "#rewind");
	click(app, "#rewind");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));

	// The switches follow the engine's state, whoever changes it.
	for (const char* id : {"cycle", "punch-in", "punch-out", "metronome"})
	{
		INFO(id);
		const std::string setting = std::string(id) == "punch-in" ? "punchIn"
			: std::string(id) == "punch-out"                      ? "punchOut"
																  : id;
		CHECK_FALSE(checked(app, std::string("#") + id));
		click(app, std::string("#") + id);
		REQUIRE(waitFor(app, [&setting](const Json& state) { return state.at(setting) == true; }));
		REQUIRE(app.waitUntil([&app, id] { return checked(app, std::string("#") + id); }));
		app.call("transport.set", {{setting, false}});
		REQUIRE(app.waitUntil([&app, id] { return !checked(app, std::string("#") + id); }));
	}

	// Start, record, stop.
	click(app, "#start");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == true; }));
	REQUIRE(app.waitUntil([&app] { return checked(app, "#start"); }));
	click(app, "#record");
	REQUIRE(app.waitUntil([&app] { return checked(app, "#record"); }));
	CHECK(transport(app).at("recording") == true);
	click(app, "#record"); // punches out, playback goes on
	REQUIRE(app.waitUntil([&app] { return !checked(app, "#record"); }));
	CHECK(transport(app).at("playing") == true);
	click(app, "#stop");
	REQUIRE(app.waitUntil([&app] { return !checked(app, "#start"); }));
	CHECK(transport(app).at("playing") == false);
}

TEST_CASE("the transport bar goes to markers, the project's ends and the selection", "[ui]")
{
	test::UiApp app({1400, 700});
	ui::EventDisplay& display = showBar(app);
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	const Json part =
		app.call("midiPart.add", {{"track", track.at("id")}, {"position", 4 * g_quarter}, {"length", 4 * g_quarter}})
			.at("part");
	app.call("note.add",
		{{"part", part.at("id")},
			{"notes", {{{"position", 3 * g_quarter}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 1.0}}}}});

	// Without markers: to zero and to the project end, the end of the note at 4 seconds.
	click(app, "#next-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 192000; }));
	click(app, "#previous-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));

	app.call("marker.add", {{"position", 2 * g_quarter}});
	click(app, "#next-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	click(app, "#next-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 192000; }));
	click(app, "#previous-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));

	// Locators to Selection: around the selected part, 2 to 4 seconds, with the button and with P in the project.
	REQUIRE(app.waitUntil(
		[&app]
		{
			Rml::Context& context = app.main().context();
			for (int i = 0; i < context.GetNumDocuments(); ++i)
				if (auto* lanes =
						dynamic_cast<ui::EventLanes*>(context.GetDocument(i)->QuerySelector("event-lanes.events")))
					return std::any_of(lanes->lanes().begin(), lanes->lanes().end(),
						[](const ui::EventLanes::Lane& lane) { return !lane.events.empty(); });
			return false;
		}));
	display.select({part.at("id").get<uint64_t>()});
	click(app, "#locators-to-selection");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 96000 && state.at("right") == 192000; }));
	app.call("transport.set", {{"left", 0}, {"right", 0}});
	app.ui().click(std::nullopt, at("#event-display"), ui::MouseButton::Left, 1, {}); // the focus zone "project"
	display.select({part.at("id").get<uint64_t>()});
	app.ui().key(std::nullopt, "P");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 96000 && state.at("right") == 192000; }));
}

TEST_CASE("the time displays show the cursor in their formats and take positions typed into them", "[ui]")
{
	test::UiApp app({1400, 700});
	showBar(app);
	app.call("transport.locate", {{"position", 72000}}); // 1.5 seconds
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.4.1.0"; }));
	CHECK(shows(app, "#secondary-time") == "00:00:01:12");

	// The format menus, saved in ui.timeFormats.
	click(app, "#primary-format");
	click(app, "menuitem[slot=primary][format=seconds]");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));
	CHECK(element(app, "menuitem[slot=primary][format=seconds]").at("attributes").contains("checked"));
	REQUIRE(app.waitUntil([&app]
		{ return app.call("settings.get", {{"key", "ui.timeFormats"}}).at("value").at("primary") == "seconds"; }));
	CHECK(shows(app, "#left-locator") == "0:00:00.000"); // the locators show the primary format

	// Exchange Time Formats, with its button and its key.
	click(app, "#exchange-formats");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "00:00:01:12"; }));
	CHECK(shows(app, "#secondary-time") == "0:00:01.500");
	app.ui().key(std::nullopt, ".");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));

	// Another client's change of the setting.
	app.call("settings.set",
		{{"key", "ui.timeFormats"},
			{"value", {{"primary", "barsBeats"}, {"secondary", "samples"}, {"timeDisplay", "barsBeats"}}}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "72000"; }));

	// Positions typed in the display's format.
	type(app, "#primary-time", "3");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 192000; }));
	type(app, "#secondary-time", "24000");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 24000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.2.1.0"; }));
	type(app, "#left-locator", "2");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 96000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#left-locator") == "2.1.1.0"; }));
	type(app, "#right-locator", "4.3");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 336000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#right-locator") == "4.3.1.0"; }));
	type(app, "#locator-range", "1");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 192000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#locator-range") == "1.0.0.0"; }));

	// Text that isn't a position changes nothing and goes.
	type(app, "#left-locator", "soon");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(transport(app).at("left") == 96000);
	CHECK(shows(app, "#left-locator") == "2.1.1.0");
}

TEST_CASE("the tempo and signature fields set the project's first tempo and signature", "[ui]")
{
	test::UiApp app({1400, 700});
	showBar(app);
	REQUIRE(app.waitUntil([&app] { return shows(app, "#signature") == "4/4"; }));
	CHECK(element(app, "#tempo").at("attributes").at("value") == "120");

	type(app, "#tempo", "90");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo").at(0).at("bpm") == 90.0; }));
	type(app, "#signature", "3/4");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("signatures").at(0).at("numerator") == 3; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#signature") == "3/4"; }));
	type(app, "#signature", "3/5"); // refused by the engine
	type(app, "#signature", "three");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.call("project.get").at("signatures").at(0).at("denominator") == 4);

	// The displays follow the project's tempo: 1.5 seconds at 90 BPM are 2.25 quarters.
	app.call("transport.locate", {{"position", 72000}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.3.2.0"; }));

	// Undo puts the tempo back, and the field follows.
	app.call("history.undo");
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "120"; }));
	CHECK(shows(app, "#signature") == "4/4");
}

TEST_CASE("the transport's key commands", "[ui]")
{
	test::UiApp app({1400, 700});
	showBar(app);

	app.ui().key(std::nullopt, "Num/");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("cycle") == true; }));
	app.ui().key(std::nullopt, "C");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("metronome") == true; }));
	app.ui().key(std::nullopt, "I");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("punchIn") == true; }));
	app.ui().key(std::nullopt, "O");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("punchOut") == true; }));
	app.ui().key(std::nullopt, "Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	app.ui().key(std::nullopt, "Shift+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 240000; }));
	app.ui().key(std::nullopt, "Shift+Num-");
	app.ui().key(std::nullopt, "Num-");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));
	app.ui().key(std::nullopt, "Num*");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("recording") == true; }));
	app.ui().key(std::nullopt, "Num0");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == false; }));

	// The enter commands edit the displays of the transport bar.
	app.ui().key(std::nullopt, "Shift+L");
	CHECK(element(app, "#left-locator").value("classes", Json::array()).dump().find("editing") != std::string::npos);
	app.ui().key(std::nullopt, "Ctrl+A");
	app.ui().text(std::nullopt, "3");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 192000; }));
	for (const char* keys : {"Shift+R", "Shift+P", "Shift+D", "Shift+T", "Shift+C"})
	{
		INFO(keys);
		const Json focus = app.ui().key(std::nullopt, keys).at("focus");
		CHECK(focus.at("tag") == "input");
		app.ui().key(std::nullopt, "Escape");
	}
}

TEST_CASE("the transport panel and the time display window", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject(); // the transport bar is hidden by default

	// F2 shows the transport panel, a window of its own with the same controls.
	app.ui().key(std::nullopt, "F2");
	ui::Window* panel = app.appUi().transportPanel();
	REQUIRE(panel != nullptr);
	const ui::WindowId id = panel->id();
	click(app, "#cycle", id);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("cycle") == true; }));
	REQUIRE(app.waitUntil([&app, id] { return checked(app, "#cycle", id); }));
	app.call("transport.locate", {{"position", 96000}});
	REQUIRE(app.waitUntil([&app, id] { return shows(app, "#primary-time", id) == "2.1.1.0"; }));
	app.ui().key(std::nullopt, "F2");
	CHECK(app.appUi().transportPanel() == nullptr);

	// With the transport bar and the panel hidden, an enter command opens the panel to edit there.
	app.ui().key(std::nullopt, "Shift+P");
	panel = app.appUi().transportPanel();
	REQUIRE(panel != nullptr);
	app.ui().key(panel->id(), "Ctrl+A");
	app.ui().text(panel->id(), "1");
	app.ui().key(panel->id(), "Enter");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));

	// Closing its window closes the panel; F2 opens it again.
	SDL_Event close{};
	close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
	close.window.windowID = panel->id();
	REQUIRE(SDL_PushEvent(&close));
	REQUIRE(app.waitUntil([&app] { return app.appUi().transportPanel() == nullptr; }));
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);

	// The time display window shows the cursor in a format of its own, picked from its context menu.
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	ui::Window* display = app.appUi().timeDisplay();
	REQUIRE(display != nullptr);
	const ui::WindowId window = display->id();
	app.call("transport.locate", {{"position", 72000}});
	INFO(element(app, "#time-display", window).dump());
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "1.4.1.0"; }));
	app.ui().click(window, at("#time-display"), ui::MouseButton::Right, 1, {});
	click(app, "menuitem[format=samples]", window);
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "72000"; }));
	CHECK(app.call("settings.get", {{"key", "ui.timeFormats"}}).at("value").at("timeDisplay") == "samples");
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	CHECK(app.appUi().timeDisplay() == nullptr);
}
