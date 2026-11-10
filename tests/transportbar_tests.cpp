// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of the transport controls (#90, ui/transport.h): every control of the project window's transport bar, the
// transport panel and the time display window, driven through ui::Automation as MCP's UI tools drive them
// (tests/uiapp.h), and the transport's key commands. Controls with a command that doesn't exist or parameters it
// doesn't take are refused (#446).
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "scripting/runtime.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "ui/transport.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
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

	// The project window with its transport bar shown. The shell fits windows into their monitor, 1024 pixels wide on
	// CI, where a dp ratio of 1 cuts off the transport bar's last sections: at 0.8 they all fit.
	ui::EventDisplay& showBar(test::UiApp& app, float dpRatio = 0.8F)
	{
		app.setDpRatio(dpRatio);
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
		app.ui().key(window, "Mod+A");
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
	test::UiApp app({1024, 700});
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
	const std::array<std::pair<std::string, std::string>, 4> switches{
		{{"#cycle", "cycle"}, {"#punch-in", "punchIn"}, {"#punch-out", "punchOut"}, {"#metronome", "metronome"}}};
	for (const auto& each : switches)
	{
		const std::string& button = each.first;
		const std::string& setting = each.second;
		INFO(button);
		// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
		CHECK(!checked(app, button));
		click(app, button);
		REQUIRE(waitFor(app, [&setting](const Json& state) { return state.at(setting) == true; }));
		REQUIRE(app.waitUntil([&app, &button] { return checked(app, button); }));
		app.call("transport.set", {{setting, false}});
		REQUIRE(app.waitUntil([&app, &button] { return !checked(app, button); }));
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

TEST_CASE("Space starts and stops playback after a click on a toolbar or transport button (#456)", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.ui().key(std::nullopt, "F2");
	ui::Window* panel = app.appUi().transportPanel();
	REQUIRE(panel != nullptr);

	// A click beside the button's icon focuses the button itself, but Space runs its key command rather than clicking
	// the button again.
	const auto spaceToggles = [&app](const std::string& button, std::optional<ui::WindowId> window)
	{
		INFO(button);
		const bool on = !checked(app, button, window);
		const Json box = element(app, button, window).at("box");
		const ui::Point edge{box.at("x").get<int>() + 2, box.at("y").get<int>() + (box.at("height").get<int>() / 2)};
		app.ui().click(window, {"", 0, edge}, ui::MouseButton::Left, 1, {});
		REQUIRE(app.waitUntil([&app, &button, window, on] { return checked(app, button, window) == on; }));
		const Json focus = app.ui().key(window, "Space").at("focus");
		CHECK(focus.value("id", std::string()) == button.substr(1));
		REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == true; }));
		app.ui().key(window, "Space");
		REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == false; }));
		CHECK(checked(app, button, window) == on);
	};
	spaceToggles("#cycle", std::nullopt);
	spaceToggles("#metronome", panel->id());
	spaceToggles("#snap", std::nullopt);
}

TEST_CASE("a narrow window cuts off the transport bar's last sections instead of drawing them over each other", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app, 1.0F); // about 1180 dp of sections in 1024 dp
	Rml::Context& context = app.main().context();
	Rml::Element* bar = nullptr;
	for (int i = 0; i < context.GetNumDocuments(); ++i)
		if (Rml::Element* found = context.GetDocument(i)->GetElementById("transport-bar"))
			bar = found;
	REQUIRE(bar != nullptr);
	const float right = bar->GetAbsoluteLeft() + bar->GetOffsetWidth();

	// The sections keep their controls' width, one after the other, and the last ones go past the bar's right edge.
	Rml::ElementList sections;
	bar->QuerySelectorAll(sections, ".transport-section");
	REQUIRE(sections.size() == 7);
	float end = bar->GetAbsoluteLeft();
	for (Rml::Element* section : sections)
	{
		INFO(section->GetAttribute<Rml::String>("title", ""));
		CHECK(section->GetAbsoluteLeft() >= end);
		end = section->GetAbsoluteLeft() + section->GetOffsetWidth();
		Rml::ElementList controls;
		section->QuerySelectorAll(controls, "*");
		for (Rml::Element* control : controls)
			CHECK(control->GetAbsoluteLeft() + control->GetOffsetWidth() <= end + 0.5F);
	}
	CHECK(end > right);

	// What shows is what a click reaches: the element in the middle of each control in the bar is the control or in it.
	Rml::ElementList controls;
	bar->QuerySelectorAll(controls, ".transport-section > *");
	for (Rml::Element* control : controls)
	{
		const Rml::Vector2f middle(control->GetAbsoluteLeft() + (control->GetOffsetWidth() / 2.0F),
			control->GetAbsoluteTop() + (control->GetOffsetHeight() / 2.0F));
		if (middle.x >= right)
			continue; // cut off
		INFO(control->GetId());
		const Rml::Element* hit = context.GetElementAtPoint(middle);
		while (hit != nullptr && hit != control)
			hit = hit->GetParentNode();
		CHECK(hit == control);
	}
	click(app, "#punch-in");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("punchIn") == true; }));
}

TEST_CASE("the transport bar goes to markers, the project's ends and the selection", "[ui]")
{
	test::UiApp app({1024, 700});
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
	const auto partShown = [&app]
	{
		Rml::Context& context = app.main().context();
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (auto* lanes =
					dynamic_cast<ui::EventLanes*>(context.GetDocument(i)->QuerySelector("event-lanes.events")))
				return std::any_of(lanes->lanes().begin(), lanes->lanes().end(),
					[](const ui::EventLanes::Lane& lane) { return !lane.events.empty(); });
		return false;
	};
	REQUIRE(app.waitUntil(partShown));
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
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("transport.locate", {{"position", 72000}}); // 1.5 seconds
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.4.1.0"; }));
	CHECK(shows(app, "#secondary-time") == "00:00:01:12");

	// Leaving a display without typing changes nothing, also at a position off its rounded value.
	app.call("transport.locate", {{"position", 72001}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 72001; }));
	app.ui().click(std::nullopt, at("#primary-time"), ui::MouseButton::Left, 2, {});
	CHECK(element(app, "#primary-time").value("classes", Json::array()).dump().find("editing") != std::string::npos);
	click(app, "#signature");
	app.ui().click(std::nullopt, at("#left-locator"), ui::MouseButton::Left, 2, {});
	app.ui().key(std::nullopt, "Enter");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(transport(app).at("position") == 72001);
	CHECK(transport(app).at("left") == 0);

	// Typing the position it shows moves the cursor onto it.
	type(app, "#primary-time", "1.4.1.0");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 72000; }));

	// The format menus: the primary format is the project's time format (reference manual p. 317), the secondary one
	// is saved in ui.timeFormats.
	click(app, "#primary-format");
	click(app, "menuitem[slot=primary][format=seconds]");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));
	CHECK(element(app, "menuitem[slot=primary][format=seconds]").at("attributes").contains("checked"));
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("timeFormat") == "seconds"; }));
	CHECK(shows(app, "#left-locator") == "0:00:00.000"); // the locators show the primary format

	// Exchange Time Formats, with its button and its key.
	click(app, "#exchange-formats");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "00:00:01:12"; }));
	CHECK(shows(app, "#secondary-time") == "0:00:01.500");
	app.ui().key(std::nullopt, ".");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));

	// The secondary format menu.
	click(app, "#secondary-format");
	click(app, "menuitem[slot=secondary][format=barsBeats]");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "1.4.1.0"; }));
	CHECK(element(app, "menuitem[slot=secondary][format=barsBeats]").at("attributes").contains("checked"));
	CHECK_FALSE(element(app, "menuitem[slot=secondary][format=timecode]").at("attributes").contains("checked"));
	REQUIRE(app.waitUntil([&app]
		{ return app.call("settings.get", {{"key", "ui.timeFormats"}}).at("value").at("secondary") == "barsBeats"; }));
	CHECK(shows(app, "#primary-time") == "0:00:01.500");

	// Another client's change of the setting and of the project's time format; a primary format in the setting, as
	// before #537, is the project's.
	app.call("settings.set",
		{{"key", "ui.timeFormats"},
			{"value", {{"primary", "timecode"}, {"secondary", "samples"}, {"timeDisplay", "barsBeats"}}}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "72000"; }));
	CHECK(shows(app, "#primary-time") == "0:00:01.500");
	app.call("project.set", {{"timeFormat", "barsBeats"}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.4.1.0"; }));
	app.call("history.undo"); // the project's time format is a project setting, undone as one
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));
	app.call("history.redo");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.4.1.0"; }));

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

TEST_CASE("the transport bar and the transport panel show the same time formats", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);
	const ui::WindowId panel = app.appUi().transportPanel()->id();
	app.call("transport.locate", {{"position", 72000}}); // 1.5 seconds
	REQUIRE(app.waitUntil([&app, panel] { return shows(app, "#secondary-time", panel) == "00:00:01:12"; }));

	// A format picked in the panel, then exchanged twice with the key, which the bar's controls run.
	click(app, "#primary-format", panel);
	click(app, "menuitem[slot=primary][format=seconds]", panel);
	REQUIRE(app.waitUntil([&app, panel] { return shows(app, "#primary-time", panel) == "0:00:01.500"; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));
	app.ui().key(std::nullopt, ".");
	REQUIRE(app.waitUntil([&app, panel] { return shows(app, "#primary-time", panel) == "00:00:01:12"; }));
	app.ui().key(std::nullopt, ".");
	REQUIRE(app.waitUntil([&app, panel] { return shows(app, "#primary-time", panel) == "0:00:01.500"; }));
	CHECK(shows(app, "#secondary-time", panel) == "00:00:01:12");
	CHECK(element(app, "menuitem[slot=primary][format=seconds]", panel).at("attributes").contains("checked"));
	CHECK(element(app, "menuitem[slot=secondary][format=timecode]", panel).at("attributes").contains("checked"));
	CHECK_FALSE(element(app, "menuitem[slot=primary][format=timecode]", panel).at("attributes").contains("checked"));
	CHECK(shows(app, "#primary-time") == "0:00:01.500");
	CHECK(shows(app, "#secondary-time") == "00:00:01:12");
	CHECK(app.call("project.get").at("timeFormat") == "seconds");
	const Json formats = app.call("settings.get", {{"key", "ui.timeFormats"}}).at("value");
	CHECK(!formats.contains("primary"));
	CHECK(formats.at("secondary") == "timecode");
}

TEST_CASE("transport time format choices and exchange do not add undo steps", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("transport.locate", {{"position", 72000}});
	app.call("project.set", {{"tempo", 130.0}});
	const size_t before = app.call("history.get").at("steps").size();

	click(app, "#primary-format");
	click(app, "menuitem[slot=primary][format=seconds]");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));
	CHECK(app.call("history.get").at("steps").size() == before);

	click(app, "#exchange-formats");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "00:00:01:12"; }));
	CHECK(shows(app, "#secondary-time") == "0:00:01.500");
	CHECK(app.call("history.get").at("steps").size() == before);

	// Undo reaches the earlier tempo edit, leaving both exchanged display formats in place.
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo")[0].at("bpm") == 120.0; }));
	CHECK(shows(app, "#primary-time") == "00:00:01:12");
	CHECK(shows(app, "#secondary-time") == "0:00:01.500");
	CHECK(app.call("project.get").at("timeFormat") == "timecode");
	app.call("history.redo");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo")[0].at("bpm") == 130.0; }));
	CHECK(shows(app, "#primary-time") == "00:00:01:12");
	CHECK(shows(app, "#secondary-time") == "0:00:01.500");
}

TEST_CASE("the tempo and signature fields set the project's first tempo and signature", "[ui]")
{
	test::UiApp app({1024, 700});
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
	test::UiApp app({1024, 700});
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
	app.ui().key(std::nullopt, "Mod+A");
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
	app.ui().key(panel->id(), "Mod+A");
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

namespace
{
	// Steps the shell for a while, e.g. for what the UI sends to arrive.
	void settle(test::UiApp& app, std::chrono::milliseconds time = std::chrono::milliseconds(300))
	{
		app.waitUntil([] { return false; }, time);
	}

	int64_t position(test::UiApp& app)
	{
		return transport(app).at("position").get<int64_t>();
	}

	// Waits until the position stays where it is, and returns it.
	int64_t stopped(test::UiApp& app)
	{
		settle(app);
		const int64_t at = position(app);
		settle(app);
		CHECK(position(app) == at);
		return at;
	}

	size_t undoSteps(test::UiApp& app)
	{
		return app.call("history.get").at("steps").size();
	}

	double tempo(test::UiApp& app)
	{
		return app.call("project.get").at("tempo").at(0).at("bpm").get<double>();
	}

	bool hasClass(test::UiApp& app, const std::string& selector, const std::string& name,
		std::optional<ui::WindowId> window = std::nullopt)
	{
		const Json names = element(app, selector, window).value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	bool visible(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		return element(app, selector, window).at("visible").get<bool>();
	}

	// Presses or releases the left mouse button where the mouse is in the main window.
	void mouseButton(test::UiApp& app, bool down)
	{
		Rml::Context& context = app.main().context();
		if (down)
			context.ProcessMouseButtonDown(0, 0);
		else
			context.ProcessMouseButtonUp(0, 0);
		app.shell().step(std::chrono::milliseconds(0));
	}
}

TEST_CASE("Rewind and Forward wind while their key or button is held", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);

	// Held, Forward steps a second and then winds on until it is released.
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Down);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 2 * 48000; }));
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Up);
	const int64_t wound = stopped(app);

	// Rewind winds back.
	app.ui().key(std::nullopt, "Num-", ui::KeyAction::Down);
	REQUIRE(waitFor(app, [wound](const Json& state) { return state.at("position") < wound - 48000; }));
	app.ui().key(std::nullopt, "Num-", ui::KeyAction::Up);
	stopped(app);

	// Fast Forward winds faster; it stops when its key goes up, also after Shift.
	app.call("transport.locate", {{"position", 0}});
	app.ui().key(std::nullopt, "Shift+Num+", ui::KeyAction::Down);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 8 * 48000; }));
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Up);
	stopped(app);

	// The buttons wind while the mouse button is held on them.
	app.call("transport.locate", {{"position", 0}});
	app.ui().move(std::nullopt, at("#forward"));
	mouseButton(app, true);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 2 * 48000; }));
	mouseButton(app, false);
	const int64_t forward = stopped(app);
	app.ui().move(std::nullopt, at("#rewind"));
	mouseButton(app, true);
	REQUIRE(waitFor(app, [forward](const Json& state) { return state.at("position") < forward - 48000; }));
	mouseButton(app, false);
	stopped(app);

	// The mouse leaving the button held ends winding, before the mouse button goes up; so does the window losing the
	// keyboard focus.
	app.call("transport.locate", {{"position", 0}});
	app.ui().move(std::nullopt, at("#forward"));
	mouseButton(app, true);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 2 * 48000; }));
	app.ui().move(std::nullopt, at("#primary-time"));
	stopped(app);
	mouseButton(app, false);
	app.ui().move(std::nullopt, at("#forward"));
	mouseButton(app, true);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 4 * 48000; }));
	app.shell().keyReleased(app.main(), ui::KeyPress{Rml::Input::KI_UNKNOWN, 0});
	stopped(app);
	mouseButton(app, false);

	// A key pressed and released at once steps a second.
	app.call("transport.locate", {{"position", 0}});
	app.ui().key(std::nullopt, "Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	CHECK(stopped(app) == 48000);
}

TEST_CASE("Nudge Cursor Left and Right move the project cursor by a grid step", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);

	// The grid is on bars by default: 4 quarters at 120 BPM, 96000 samples.
	app.ui().key(std::nullopt, "Mod+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 96000; }));
	app.ui().key(std::nullopt, "Mod+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 192000; }));
	app.ui().key(std::nullopt, "Mod+Num-");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 96000; }));

	// On beats, by a beat; never before the project start.
	app.call("snap.set", {{"grid", "beat"}});
	settle(app);
	app.ui().key(std::nullopt, "Mod+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 120000; }));
	app.call("transport.locate", {{"position", 1000}});
	settle(app);
	app.ui().key(std::nullopt, "Mod+Num-");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));
	app.ui().key(std::nullopt, "Mod+Num-");
	CHECK(stopped(app) == 0);

	// A linear grid: seconds.
	app.call("snap.set", {{"grid", "second"}});
	settle(app);
	app.ui().key(std::nullopt, "Mod+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
}

TEST_CASE("the mouse wheel changes a display's value", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("transport.set", {{"left", 48000}, {"right", 96000}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#left-locator") == "1.3.1.0"; }));

	// A scroll sideways, as a touchpad or a tilt wheel sends, moves nothing.
	app.ui().wheel(std::nullopt, at("#primary-time"), 1, 0);
	app.ui().wheel(std::nullopt, at("#left-locator"), -1, 0);
	settle(app);
	CHECK(position(app) == 0);
	CHECK(transport(app).at("left") == 48000);

	// Up for later, by a beat in bars and beats.
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 24000; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -2);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 72000; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));

	// By a frame in timecode (25 fps), a second in seconds; never before the project start.
	app.ui().wheel(std::nullopt, at("#secondary-time"), 0, -1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 49920; }));
	app.call("project.set", {{"timeFormat", "seconds"}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#left-locator") == "0:00:01.000"; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 3);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));

	// The locators, and the range's length, which moves the right locator.
	app.ui().wheel(std::nullopt, at("#left-locator"), 0, 1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 0; }));
	app.ui().wheel(std::nullopt, at("#right-locator"), 0, -1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 144000; }));
	app.ui().wheel(std::nullopt, at("#locator-range"), 0, 1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 96000; }));
}

TEST_CASE("positions typed or wheeled past the largest project position stop there (#457)", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("project.set", {{"timeFormat", "seconds"}});
	app.call("settings.set",
		{{"key", "ui.timeFormats"}, {"value", {{"secondary", "samples"}, {"timeDisplay", "barsBeats"}}}});
	app.call("transport.locate", {{"position", 48000}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "48000"; }));
	// The project's time format comes with its own reply, maybe after the setting's.
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.000"; }));
	const std::string maximum = std::to_string(time::g_maxPosition);

	// Fifteen digits of seconds are more samples than 64 bits hold: the cursor goes to the largest position.
	type(app, "#primary-time", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == time::g_maxPosition; }));
	REQUIRE(app.waitUntil([&app, &maximum] { return shows(app, "#secondary-time") == maximum; }));

	// Past the largest position in samples, which the engine would refuse.
	app.call("transport.locate", {{"position", 48000}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	type(app, "#secondary-time", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == time::g_maxPosition; }));
	REQUIRE(app.waitUntil([&app, &maximum] { return shows(app, "#secondary-time") == maximum; }));

	// The wheel stops there too, and the next turn goes back from there.
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1);
	settle(app);
	CHECK(position(app) == time::g_maxPosition);
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == time::g_maxPosition - 48000; }));

	// Turns far larger than a wheel's, as injected input may send, are a billion steps: a billion seconds here.
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1e13F);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == time::g_maxPosition; }));
	REQUIRE(app.waitUntil([&app, &maximum] { return shows(app, "#secondary-time") == maximum; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 1e13F);
	REQUIRE(waitFor(app,
		[](const Json& state) { return state.at("position") == time::g_maxPosition - (int64_t{48000} * 1000000000); }));

	// The locators and the range's length.
	type(app, "#left-locator", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == time::g_maxPosition; }));
	app.call("transport.set", {{"left", 48000}, {"right", 96000}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 48000; }));
	type(app, "#right-locator", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == time::g_maxPosition; }));
	app.call("transport.set", {{"right", 96000}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 96000; }));
	type(app, "#locator-range", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == time::g_maxPosition; }));

	// A billion beats in bars and beats: 500 million seconds at 120 BPM.
	app.call("project.set", {{"timeFormat", "barsBeats"}});
	app.call("transport.locate", {{"position", 0}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.1.1.0"; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1e13F);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == int64_t{24000} * 1000000000; }));
}

TEST_CASE("a drag or wheel gesture on the tempo field is one undo step", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "120"; }));
	const size_t before = undoSteps(app);

	// A drag up raises the tempo in many steps of the field, and sets it once, at its end.
	const Json box = element(app, "#tempo").at("box");
	const int x = static_cast<int>(box.at("x").get<double>() + (box.at("width").get<double>() / 2));
	const int y = static_cast<int>(box.at("y").get<double>() + (box.at("height").get<double>() / 2));
	app.ui().drag(std::nullopt, at("#tempo"), {std::string(), 0, ui::Point{x, y - 40}});
	// The field keeps where the drag took it, also before the project's tempo is read again: wheel turns go on from
	// there.
	CHECK(element(app, "#tempo").at("attributes").at("value") != "120");
	REQUIRE(app.waitUntil([&app] { return tempo(app) > 121.0; }));
	settle(app);
	CHECK(undoSteps(app) == before + 1);
	const double dragged = tempo(app);

	// Turns of the wheel in a row are one step too.
	for (int turn = 0; turn < 3; ++turn)
		app.ui().wheel(std::nullopt, at("#tempo"), 0, -1);
	REQUIRE(app.waitUntil([&app, dragged] { return std::abs(tempo(app) - (dragged + 3.0)) < 0.001; }));
	settle(app);
	CHECK(undoSteps(app) == before + 2);

	app.call("history.undo");
	REQUIRE(app.waitUntil([&app, dragged] { return tempo(app) == dragged; }));
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "120"; }));
}

TEST_CASE("the tempo field changes the tempo only when a number is typed, also the one it shows", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	// A tempo with more decimals than the field shows, e.g. from a MIDI file.
	app.call("project.set", {{"tempo", 130.0001}});
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "130.0001"; }));
	const size_t before = undoSteps(app);

	// Enter or leaving the field without typing keeps the tempo and adds no undo step.
	app.ui().click(std::nullopt, at("#tempo"), ui::MouseButton::Left, 2, {});
	CHECK(element(app, "#tempo input").at("value") == "130.000");
	app.ui().key(std::nullopt, "Enter");
	app.ui().click(std::nullopt, at("#tempo"), ui::MouseButton::Left, 2, {});
	click(app, "#signature");
	CHECK(!hasClass(app, "#tempo", "editing"));
	settle(app);
	CHECK(tempo(app) == 130.0001);
	CHECK(undoSteps(app) == before);

	// Typing the number it shows sets that tempo.
	type(app, "#tempo", "130.000");
	REQUIRE(app.waitUntil([&app] { return tempo(app) == 130.0; }));
	settle(app);
	CHECK(undoSteps(app) == before + 1);
}

TEST_CASE("typing the position the primary time display showed during playback jumps back to it", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	app.call("transport.play");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 0; }));

	app.ui().click(std::nullopt, at("#primary-time"), ui::MouseButton::Left, 2, {});
	const std::string shown = element(app, "#primary-time input").at("value").get<std::string>();
	const int64_t from = position(app);
	REQUIRE(waitFor(app, [from](const Json& state) { return state.at("position") > from + 24000; })); // a beat on
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, shown);
	const int64_t typed = position(app);
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(waitFor(app, [typed](const Json& state) { return state.at("position") < typed; }));
	CHECK(transport(app).at("playing") == true);
	app.call("transport.stop");
	app.call("audio.stop");
}

TEST_CASE("the time display window shows beat counts only and changes its size", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	ui::Window* display = app.appUi().timeDisplay();
	REQUIRE(display != nullptr);
	const ui::WindowId window = display->id();
	app.call("transport.locate", {{"position", 72000}}); // 1.4.1.0
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "1.4.1.0"; }));
	CHECK(hasClass(app, "#time-display", "size-1", window));
	const ui::Size normal = app.shell().bounds(window).size();

	// Show Beat Count Only: the beat in its bar.
	app.ui().click(window, at("#time-display"), ui::MouseButton::Right, 1, {});
	click(app, "#beat-count-only", window);
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "4"; }));
	CHECK(element(app, "#beat-count-only", window).at("attributes").contains("checked"));
	CHECK(app.call("settings.get", {{"key", "ui.timeDisplay"}}).at("value").at("beatCountOnly") == true);
	app.call("transport.locate", {{"position", 96000}});
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "1"; }));

	// Increase and Decrease Window Size: the window and its digits, whose sizes the theme has.
	app.ui().click(window, at("#time-display"), ui::MouseButton::Right, 1, {});
	click(app, "#increase-size", window);
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-2", window); }));
	const ui::Size larger = app.shell().bounds(window).size();
	CHECK(larger.width > normal.width);
	CHECK(larger.height > normal.height);
	CHECK(app.call("settings.get", {{"key", "ui.timeDisplay"}}).at("value").at("size") == 2);
	for (int smaller = 0; smaller < 4; ++smaller)
		app.keys().run("timeDisplay.decreaseSize", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-0", window); }));
	CHECK(app.shell().bounds(window).size().width < normal.width);
	CHECK(app.call("settings.get", {{"key", "ui.timeDisplay"}}).at("value").at("size") == 0);

	// Both stay for the window opened again.
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	display = app.appUi().timeDisplay();
	REQUIRE(display != nullptr);
	const ui::WindowId again = display->id();
	REQUIRE(app.waitUntil([&app, again] { return shows(app, "#time-display", again) == "1"; }));
	REQUIRE(app.waitUntil([&app, again] { return hasClass(app, "#time-display", "size-0", again); }));
	CHECK(app.shell().bounds(again).size().width < normal.width);
}

TEST_CASE("setup panes show and hide the transport bar's and the transport panel's sections", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	CHECK(visible(app, "#transport-bar [section=locators]"));

	// A right click on the bar opens its setup pane, which stays open while its buttons hide sections.
	app.ui().click(std::nullopt, at("#transport-bar [section=click]"), ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#transport-setup"));
	click(app, "#transport-setup [setup-section=locators]");
	click(app, "#transport-setup [setup-section=punchPoints]");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(!visible(app, "#transport-bar [section=locators]"));
	CHECK(!visible(app, "#transport-bar [section=punchPoints]"));
	CHECK(visible(app, "#transport-bar [section=transportControls]"));
	const Json layout = app.call("settings.get", {{"key", "ui.layout"}}).at("value");
	CHECK(layout.at("windows").at("project").at("sections").at("transport").at("hidden") ==
		Json::array({"locators", "punchPoints"}));

	// The transport panel has sections of its own.
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);
	const ui::WindowId panel = app.appUi().transportPanel()->id();
	CHECK(visible(app, "[section=locators]", panel));
	app.ui().click(panel, at("[section=click]"), ui::MouseButton::Right, 1, {});
	click(app, "#transport-setup [setup-section=click]", panel);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(!visible(app, "[section=click]", panel));
	CHECK(visible(app, "#transport-bar [section=click]"));

	// Show All shows every section of the bar again.
	app.ui().click(std::nullopt, at("#transport-bar [section=click]"), ui::MouseButton::Right, 1, {});
	click(app, "#transport-setup [setup-all]");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(visible(app, "#transport-bar [section=locators]"));
	CHECK(visible(app, "#transport-bar [section=punchPoints]"));
	CHECK(!visible(app, "[section=click]", panel));

	// The panel opened again keeps its own.
	app.ui().key(std::nullopt, "F2");
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);
	CHECK(!visible(app, "[section=click]", app.appUi().transportPanel()->id()));
}

TEST_CASE("setup panes save and recall presets and reset their sections to the defaults", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	const auto openPane = [&app]
	{
		if (!visible(app, "#transport-setup"))
			app.ui().click(std::nullopt, at("#transport-bar [section=click]"), ui::MouseButton::Right, 1, {});
		REQUIRE(visible(app, "#transport-setup"));
	};
	const auto exists = [&app](const std::string& selector)
	{ return !app.ui().dom(std::nullopt, selector, 0).at("elements").empty(); };
	const auto presets = [&app] { return app.call("settings.get", {{"key", "ui.setupPresets"}}).at("value"); };
	openPane();
	click(app, "#transport-setup [setup-section=locators]");
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(!visible(app, "#transport-bar [section=locators]"));

	// Save Preset names the sections as they are, in the user setting ui.setupPresets, which profiles carry.
	openPane();
	click(app, "#transport-setup [setup-preset-save]");
	REQUIRE(app.waitUntil([&exists] { return exists("#setup-preset-name"); }));
	click(app, "#setup-preset-name");
	app.ui().text(std::nullopt, "Compact");
	click(app, "#setup-preset-ok");
	REQUIRE(app.waitUntil([&presets] { return presets().contains("transport"); }));
	const Json saved = presets().at("transport");
	REQUIRE(saved.size() == 1);
	CHECK(saved.at(0).at("name") == "Compact");
	CHECK(saved.at(0).at("hidden") == Json::array({"locators"}));
	CHECK(saved.at(0).at("order").at(0) == "locators");
	REQUIRE(app.waitUntil([&exists] { return exists("#transport-setup [setup-preset=Compact]"); }));

	// Reset All: the default order and sections.
	openPane();
	click(app, "#transport-setup [setup-reset]");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(visible(app, "#transport-bar [section=locators]"));
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json layout = app.call("settings.get", {{"key", "ui.layout"}}).at("value");
			return !layout.at("windows").at("project").value("sections", Json::object()).contains("transport");
		}));

	// The preset brings its sections back.
	openPane();
	click(app, "#transport-setup [setup-preset=Compact]");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(!visible(app, "#transport-bar [section=locators]"));
	CHECK(visible(app, "#transport-bar [section=punchPoints]"));

	// Another client's presets fill the pane too.
	app.call("settings.set", {{"key", "ui.setupPresets"}, {"value", {{"transport", Json::array()}}}});
	REQUIRE(app.waitUntil([&exists] { return !exists("#transport-setup [setup-preset=Compact]"); }));
}

TEST_CASE("the transport controls refuse buttons and menu items with a command that doesn't exist or parameters it "
		  "doesn't take (#446)",
	"[unit]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	ui::Window& window = fake.shell.open(ui::WindowSpec{});
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding);
	keys.add({"timeDisplay.increaseSize", "Makes the time display bigger.", {}});
	keys.add({"wind.forward", "Winds forward while held.", {}, [] {}});
	const auto run = [](const std::string& /*command*/, const Json& /*parameters*/) {};
	const auto check = [&keys](const std::string& command, const Json& parameters) { keys.check(command, parameters); };
	const auto load = [&window](const std::string& controls)
	{ return window.context().LoadDocumentFromMemory("<rml><body>" + controls + "</body></rml>"); };
	const std::string good =
		R"(<button command="transport.toggle" parameters='{"setting": "cycle"}'>Cycle</button>)"
		R"(<button command="wind.forward" hold="">Forward</button>)"
		R"(<menu><menuitem command="timeDisplay.increaseSize">Increase Window Size</menuitem></menu>)";

	Rml::ElementDocument* document = load(good);
	REQUIRE(document != nullptr);
	CHECK_NOTHROW(std::make_unique<ui::TransportControls>(engine, binding, *document, run, nullptr, check));
	document->Close();

	struct Bad
	{
		const char* control;
		const char* named; // in the message
	};
	const Bad bad[] = {
		{R"(<button command="transport.toggel">Cycle</button>)", "transport.toggel"},
		{R"(<button command="transport.toggle" parameters='{"setting": 1}'>Cycle</button>)", "setting"},
		{R"(<button command="wind.backward" hold="">Rewind</button>)", "wind.backward"},
		{R"(<button command="wind.forward" hold="" parameters='{"fast": true}'>Forward</button>)", "wind.forward"},
		{R"(<menu><menuitem command="timeDisplay.increaseSize" parameters='{"steps": 2}'>Bigger</menuitem></menu>)",
			"timeDisplay.increaseSize"},
		{R"(<button command="transport.play" parameters='{"setting"'>Start</button>)", "JSON object"},
	};
	for (const Bad& control : bad)
	{
		INFO(control.control);
		document = load(good + control.control);
		REQUIRE(document != nullptr);
		CHECK_THROWS_AS(std::make_unique<ui::TransportControls>(engine, binding, *document, run, nullptr, check),
			std::invalid_argument);
		CHECK_THROWS_WITH(std::make_unique<ui::TransportControls>(engine, binding, *document, run, nullptr, check),
			ContainsSubstring(control.named));
		document->Close();
	}
	// Without a check, only parameters that aren't a JSON object.
	document = load(good + R"(<button command="transport.toggel">Cycle</button>)");
	REQUIRE(document != nullptr);
	CHECK_NOTHROW(std::make_unique<ui::TransportControls>(engine, binding, *document, run));
	document->Close();
	document = load(good + R"(<button command="transport.play" parameters='[]'>Start</button>)");
	REQUIRE(document != nullptr);
	CHECK_THROWS_AS(std::make_unique<ui::TransportControls>(engine, binding, *document, run), std::invalid_argument);
	document->Close();
}

namespace
{
	// The first element in the window's documents that matches the selector. Only drawing updates RmlUi here, so its
	// box is where the window last drew it.
	Rml::Element& drawn(ui::Window& window, const std::string& selector)
	{
		Rml::Context& context = window.context();
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (Rml::Element* found = context.GetDocument(i)->QuerySelector(selector))
				return *found;
		throw std::invalid_argument("no " + selector);
	}

	// What a time display shows: a text display's value, another element's text.
	std::string value(Rml::Element& display)
	{
		return display.GetTagName() == "textdisplay" ? display.GetAttribute<Rml::String>("value", "")
													 : display.GetInnerRML();
	}

	// Where the project window draws the project cursor for the position its event display has: pixels right of the
	// lanes' left edge.
	double cursorPixels(test::UiApp& app, const ui::EventDisplay& display)
	{
		return (display.cursor() - display.view().start) / display.view().scale * app.main().dpRatio();
	}
}

TEST_CASE(
	"the project cursor and the time displays follow playback and changes made through Lua, without input", "[ui]")
{
	test::UiApp app({1024, 600});
	const ui::EventDisplay& display = showBar(app);
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	ui::Window* timeWindow = app.appUi().timeDisplay();
	REQUIRE(timeWindow != nullptr);
	Rml::Element& cursor = drawn(app.main(), ".project-cursor");
	Rml::Element& bar = drawn(app.main(), "#primary-time");
	Rml::Element& time = drawn(*timeWindow, "#time-display");
	REQUIRE(app.waitUntil([&] { return value(bar) == "1.1.1.0" && !value(time).empty(); }));
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	app.call("transport.play");

	// No input from here on. The views follow the playback position once a frame, and whenever what one shows changes
	// in a step, its window draws it in that step.
	int moved = 0;
	int counted = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while ((moved < 10 || counted < 10) && std::chrono::steady_clock::now() < deadline)
	{
		const uint64_t mainFrames = app.main().frames();
		const uint64_t timeFrames = timeWindow->frames();
		const double before = display.cursor();
		const std::string barBefore = value(bar);
		const std::string timeBefore = value(time);
		app.shell().step(std::chrono::milliseconds(20));
		if (display.cursor() != before || value(bar) != barBefore)
		{
			++moved;
			CHECK(app.main().frames() == mainFrames + 1);
			CHECK(std::abs(cursor.GetOffsetLeft() - cursorPixels(app, display)) < 1.0);
		}
		if (value(time) != timeBefore)
		{
			++counted;
			CHECK(timeWindow->frames() == timeFrames + 1);
		}
	}
	CHECK(moved >= 10);
	CHECK(counted >= 10);
	app.call("transport.stop");
	app.call("audio.stop");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == false; }));
	// The cursor is in seconds: sample / 48000 * 48000 doesn't give every sample back exactly.
	REQUIRE(app.waitUntil([&display, &app]
		{ return std::llround(display.cursor() * 48000.0) == static_cast<long long>(position(app)); }));

	// A change made through Lua, as through any other engine client: the windows draw it in the step it arrives in.
	app.shell().step(std::chrono::milliseconds(0));
	const std::string stopped = value(time);
	CHECK(!app.lua().run("gnubase.transport.locate{position = 96000}", "locate").has_value());
	bool shown = false;
	const auto locateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!shown && std::chrono::steady_clock::now() < locateDeadline)
	{
		const uint64_t mainFrames = app.main().frames();
		const uint64_t timeFrames = timeWindow->frames();
		const double before = display.cursor();
		const std::string barBefore = value(bar);
		const std::string timeBefore = value(time);
		app.shell().step(std::chrono::milliseconds(20));
		if (display.cursor() != before || value(bar) != barBefore)
		{
			CHECK(app.main().frames() == mainFrames + 1);
			CHECK(std::abs(cursor.GetOffsetLeft() - cursorPixels(app, display)) < 1.0);
		}
		if (value(time) != timeBefore)
			CHECK(timeWindow->frames() == timeFrames + 1);
		shown = display.cursor() == 2.0 && value(bar) == "2.1.1.0" && value(time) != stopped;
	}
	CHECK(shown);
}

TEST_CASE("windows whose documents nothing changed in don't draw, also during playback", "[ui][slow]")
{
	test::UiApp app({1024, 600});
	showBar(app);
	app.keys().run("window.mixer", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.appUi().mixer(1) != nullptr; }));
	const ui::Window* mixer = app.shell().find(app.appUi().mixerWindow(1));
	REQUIRE(mixer != nullptr);
	// The time display window shows only the beat, which changes twice a second at 120 BPM, while the position it
	// follows changes once a frame (#745).
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	ui::Window* timeWindow = app.appUi().timeDisplay();
	REQUIRE(timeWindow != nullptr);
	Rml::Element& beat = drawn(*timeWindow, "#time-display");
	app.call("settings.set", {{"key", "ui.timeDisplay"}, {"value", {{"beatCountOnly", true}, {"size", 1}}}});
	REQUIRE(app.waitUntil([&beat] { return value(beat) == "1"; }));
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");

	// Longer than the status line takes to ask the engine for the record time again (once a second): its answer
	// draws only what it changed.
	const auto idle = [&app, mixer, timeWindow, &beat](bool playing)
	{
		const uint64_t mainFrames = app.main().frames();
		const uint64_t mixerFrames = mixer->frames();
		int beats = 0;
		int wrong = 0; // steps in which the time display window drew without its beat changing, or the other way round
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
		while (std::chrono::steady_clock::now() < until)
		{
			const uint64_t timeFrames = timeWindow->frames();
			const std::string before = value(beat);
			app.shell().step(std::chrono::milliseconds(20));
			const bool changed = value(beat) != before;
			beats += changed ? 1 : 0;
			wrong += timeWindow->frames() == timeFrames + (changed ? 1 : 0) ? 0 : 1;
		}
		CHECK(wrong == 0); // it draws in the steps its beat changes in, and in no other
		CHECK(mixer->frames() == mixerFrames);
		if (playing)
		{
			CHECK(app.main().frames() > mainFrames + 10); // the project cursor and the transport bar's time displays
			CHECK(beats >= 3);
		}
	};
	const auto settle = [&app]
	{
		for (int i = 0; i < 10; ++i)
			app.shell().step(std::chrono::milliseconds(20));
	};
	settle(); // what the audio's events changed
	idle(false);
	app.call("transport.play");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == true; }));
	settle(); // what the transport's events changed
	idle(true);
	app.call("transport.stop");
	app.call("audio.stop");
}
