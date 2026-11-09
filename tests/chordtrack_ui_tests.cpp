// SPDX-License-Identifier: GPL-3.0-or-later
// The chord track in the project window (#173, docs/features/chord-track.md), driven with ui::Automation as MCP's UI
// tools drive it: Add Track's Chord page and the chord events on the chord track's lane, drawn, typed, dragged and
// erased.
#include "testutils/testutils.h"
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	// The project window at a dp ratio of 0.5.
	struct Window
	{
		Window() : display(app.appUi().showProject())
		{
			app.setDpRatio(0.5F);
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			return app.call(command, parameters);
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		Json element(const std::string& selector)
		{
			const Json found = elements(selector);
			REQUIRE(found.size() == 1);
			return found[0];
		}

		bool hasClass(const std::string& selector, const std::string& name)
		{
			const Json classes = element(selector).value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		void click(const std::string& selector, int count = 1)
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		void type(const std::string& text)
		{
			app.ui().key(std::nullopt, "Mod+A");
			app.ui().text(std::nullopt, text);
			app.ui().key(std::nullopt, "Enter");
		}

		Json events() { return call("chord.get").at("events"); }

		test::UiApp app{{1000, 600}};
		ui::EventDisplay& display;
	};
}

TEST_CASE("Add Track adds the chord track once", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const auto elements = [&app](const std::string& selector)
	{ return app.ui().dom(std::nullopt, selector, 0).at("elements"); };
	const auto click = [&app](const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	};
	click("#add-track");
	REQUIRE(app.waitUntil([&] { return !elements("#add-track-dialog").empty(); }));
	// The list scrolls: Chord is below Transpose.
	click("#type-transpose");
	app.ui().key(std::nullopt, "Down");
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(elements("#track-types").at(0).at("attributes").at("value") == "chord");
	CHECK(elements("#track-name").empty());
	click("#add-track-confirm");
	CHECK(app.waitUntil([&] { return app.call("chord.get").at("track") != 0; }));
}

TEST_CASE("the chord track's lane draws, types and erases chord events", "[ui]")
{
	Window window;
	window.call("track.add", {{"type", "chord"}});
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(".chord-row", "hidden"); }));

	// The draw tool adds an undefined chord at the nearest bar and opens it: Enter takes the chord typed.
	window.display.setTool(ui::Tool::Draw);
	window.click(".chord-row");
	REQUIRE(window.app.waitUntil([&] { return window.events().size() == 1; }));
	CHECK(window.events()[0].at("chord") == "X");
	CHECK(window.events()[0].at("position").get<int64_t>() % (4 * g_quarter) == 0);
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".chord-field", "shown"); }));
	window.type("Am7");
	REQUIRE(window.app.waitUntil([&] { return window.events()[0].at("chord") == "Am7"; }));
	CHECK(window.app.waitUntil([&] { return window.element(".chord-event").value("text", std::string()) == "Am7"; }));

	// The erase tool removes the event clicked.
	window.display.setTool(ui::Tool::Erase);
	window.click(".chord-event");
	CHECK(window.app.waitUntil([&] { return window.events().empty(); }));
}

TEST_CASE("a chord event drags to the nearest bar and opens for typing on a double click", "[ui]")
{
	Window window;
	window.call("track.add", {{"type", "chord"}});
	window.call("chord.add", {{"position", 0}, {"chord", "C"}});
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-event").size() == 1; }));
	window.display.setTool(ui::Tool::ObjectSelection);
	// Three fifths of a bar (1.2 seconds at 120 BPM) to the right: onto the second bar's line.
	const Json box = window.element(".chord-event").at("box");
	const double x = box.at("x").get<double>() + (box.at("width").get<double>() / 2.0);
	const double y = box.at("y").get<double>() + (box.at("height").get<double>() / 2.0);
	const double across = 1.2 / window.display.view().scale * 0.5; // pixels, at a dp ratio of 0.5
	window.app.ui().drag(std::nullopt, {".chord-event", 0, std::nullopt},
		{"", 0, ui::Point{static_cast<int>(std::lround(x + across)), static_cast<int>(std::lround(y))}});
	REQUIRE(window.app.waitUntil([&] { return window.events().at(0).at("position") == 4 * g_quarter; }));

	window.click(".chord-event", 2);
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(".chord-field", "shown"); }));
	window.type("G7/B");
	CHECK(window.app.waitUntil([&] { return window.events().at(0).at("chord") == "G7/B"; }));
}

TEST_CASE("Project > Chord Track converts chords to MIDI and back, the Inspector sets Follow Chord Track", "[ui]")
{
	Window window;
	window.app.appUi().docking().open("inspector");
	window.app.shell().step(std::chrono::milliseconds(0));
	const auto track = window.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	window.call("chord.add", {{"position", 0}, {"chord", "C"}});
	window.call("chord.add", {{"position", 4 * g_quarter}, {"chord", "Am"}});
	const std::string row = "#track-list listitem[value=\"" + std::to_string(track) + "\"] .track-name";
	REQUIRE(window.app.waitUntil([&] { return window.elements(row).size() == 1; }));
	window.click(row);
	const auto parts = [&]
	{
		Json found = Json::array();
		const Json project = window.call("project.get");
		for (const Json& part : project.at("midiParts"))
			if (part.at("track") == track)
				found.push_back(part);
		return found;
	};

	// Chords to MIDI: the voicings on the selected track, which then follows the chord track with Voicings.
	window.app.keys().run("track.chordsToMidiSelected", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil([&] { return parts().size() == 1; }));
	CHECK(parts()[0].at("notes").size() == 8);
	const std::string follow = ".inspector select[control=followChords]";
	REQUIRE(
		window.app.waitUntil([&] { return window.element(follow).at("attributes").value("value", "") == "voicings"; }));

	// The Inspector's Follow Chord Track: Root Notes, just above Voicings.
	window.click(follow);
	window.app.ui().key(std::nullopt, "Up");
	window.click(follow);
	CHECK(window.app.waitUntil([&] { return window.call("chord.get").at("follow")[0].at("mode") == "rootNotes"; }));

	// Create Chord Events: the chords of the selected track's notes replace those in their range.
	window.call("chord.set", {{"events", {{{"event", window.events()[1].at("id")}, {"chord", "F"}}}}});
	window.app.keys().run("track.createChordEventsSelected", Json::object(), window.app.main(), nullptr);
	CHECK(window.app.waitUntil([&] { return window.events().size() == 2 && window.events()[1].at("chord") == "Am"; }));
}
