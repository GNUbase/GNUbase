// SPDX-License-Identifier: GPL-3.0-or-later
// The chord pads zone in the project window (#174, docs/features/chord-pads.md), driven with ui::Automation as MCP's
// UI tools drive it: shown and hidden from the Project menu, pads played and recorded with the mouse, typed, swapped,
// changed with the toolbars and the wheel, presets and the root key, and the chord assistant's suggestions.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/docking.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	struct Window
	{
		Window()
		{
			app.appUi().showProject();
			app.setDpRatio(0.5F);
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			return app.call(command, parameters);
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		bool hasClass(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.size() != 1)
				return false;
			const Json classes = found[0].value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		std::string text(const std::string& selector)
		{
			const Json found = elements(selector);
			return found.size() == 1 ? found[0].value("text", std::string()) : std::string();
		}

		void click(const std::string& selector, int count = 1, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, count, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void step() { app.shell().step(std::chrono::milliseconds(0)); }

		// Opens the chord pads with Project > Show/Hide Chord Pads.
		void open()
		{
			app.keys().run("window.chordPads", Json::object(), app.main(), nullptr);
			REQUIRE(app.waitUntil([&] { return elements(".chord-pad").size() == 12; }));
		}

		Json pads() { return call("chordPads.get").at("pads"); }

		static std::string pad(int index) { return ".chord-pad[pad=\"" + std::to_string(index) + "\"]"; }

		test::UiApp app{{1200, 700}};
	};

	// The notes of the track's MIDI parts, by pitch.
	std::vector<int> recordedPitches(const Json& project, uint64_t track)
	{
		std::vector<int> pitches;
		for (const Json& part : project.at("midiParts"))
			if (part.at("track") == track)
				for (const Json& note : part.at("notes"))
					pitches.push_back(note.at("pitch").get<int>());
		std::sort(pitches.begin(), pitches.end());
		return pitches;
	}
}

TEST_CASE("Project > Show/Hide Chord Pads shows the pads in the lower zone and hides them", "[ui]")
{
	Window window;
	window.open();
	CHECK(window.app.appUi().docking().showing("chordPads"));
	CHECK(window.text(Window::pad(0) + " .chord-pad-chord") == "C");
	CHECK(window.text(Window::pad(4) + " .chord-pad-degree") == "V");
	CHECK(window.text(Window::pad(0) + " .chord-pad-key") == "C1");
	CHECK(window.hasClass(Window::pad(7), "unassigned"));
	// The Project menu has the item.
	CHECK(!window.elements("menuitem[command=\"window.chordPads\"]").empty());
	window.app.keys().run("window.chordPads", Json::object(), window.app.main(), nullptr);
	CHECK(window.app.waitUntil([&] { return !window.app.appUi().docking().showing("chordPads"); }));
}

TEST_CASE("a pad held with the mouse plays its chord, which a record-enabled track records", "[ui]")
{
	Window window;
	const Json added = window.call("track.add", {{"type", "instrument"}});
	const auto track = added.at("track").at("id").get<uint64_t>();
	window.call("channel.set",
		{{"channel", added.at("channel").at("id")}, {"instrument", "noteProbe"}, {"recordEnable", true}});
	window.open();
	window.call("transport.record");
	window.call("audio.render", {{"frames", 4800}});
	window.app.ui().press(std::nullopt, {Window::pad(5), 0, std::nullopt}, ui::MouseButton::Left);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.hasClass(Window::pad(5), "triggered"); }));
	CHECK(window.hasClass(Window::pad(5), "selected"));
	window.call("audio.render", {{"frames", 4800}});
	window.app.ui().release(std::nullopt, {Window::pad(5), 0, std::nullopt}, ui::MouseButton::Left);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return !window.hasClass(Window::pad(5), "triggered"); }));
	window.call("audio.render", {{"frames", 4800}});
	window.call("transport.stop");
	window.call("audio.render", {{"frames", 4800}});
	REQUIRE(window.app.waitUntil(
		[&] { return recordedPitches(window.call("project.get"), track).size() == 4; }, std::chrono::seconds(10)));
	CHECK(recordedPitches(window.call("project.get"), track) == std::vector<int>{57, 69, 72, 76});
}

TEST_CASE("pads are typed, swapped, changed with the toolbars and the wheel, and filled from presets", "[ui]")
{
	Window window;
	window.open();

	// A double click opens the chord field: Enter assigns the chord typed.
	window.click(Window::pad(8), 2);
	REQUIRE(window.app.waitUntil([&] { return window.hasClass("#chord-pads-field", "shown"); }));
	window.app.ui().key(std::nullopt, "Ctrl+A");
	window.app.ui().text(std::nullopt, "Fmaj7");
	window.app.ui().key(std::nullopt, "Enter");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[8].at("chord") == "Fmaj7"; }));
	CHECK(window.app.waitUntil([&] { return window.text(Window::pad(8) + " .chord-pad-chord") == "Fmaj7"; }));

	// A pad dragged onto another swaps them.
	window.app.ui().drag(std::nullopt, {Window::pad(0), 0, std::nullopt}, {Window::pad(1), 0, std::nullopt});
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("chord") == "C"; }));
	CHECK(window.pads()[0].at("chord") == "Dm");

	// The extended toolbar on the selected pad (the one dropped on): Transpose Up, More Tensions, Lock.
	window.click("#chord-pads-transposeUp");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("chord") == "C#"; }));
	window.click("#chord-pads-moreTensions");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("chord") == "C#maj7"; }));
	window.click("#chord-pads-lock");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[1].at("locked") == true; }));
	CHECK(window.app.waitUntil([&] { return window.hasClass(Window::pad(1), "locked"); }));

	// The wheel over a pad: down the next voicing, with Shift up a semitone.
	window.app.ui().wheel(std::nullopt, {Window::pad(3), 0, std::nullopt}, 0.0F, 1.0F);
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.pads()[3].at("voicing") == 1; }));
	window.app.ui().wheel(std::nullopt, {Window::pad(3), 0, std::nullopt}, 0.0F, -1.0F, {"shift"});
	window.step();
	REQUIRE(window.app.waitUntil([&] { return window.pads()[3].at("chord") == "F#"; }));

	// Select All and Remove Assignment: the locked pad stays.
	window.click("#chord-pads-selectAll");
	window.click("#chord-pads-remove");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[0].at("chord").get<std::string>().empty(); }));
	CHECK(window.pads()[1].at("chord") == "C#maj7");

	// A preset with what to load, and the root key with Transpose Pads.
	window.call("chordPads.setPad", {{"pad", 1}, {"locked", false}});
	window.click("#chord-pads-loadPreset");
	REQUIRE(window.app.waitUntil([&] { return window.pads()[0].at("chord") == "C"; }));
	window.click("#chord-pads-transposePads");
	window.step();
	window.click("#chord-pads-root");
	window.app.ui().key(std::nullopt, "Down");
	window.app.ui().key(std::nullopt, "Down");
	window.click("#chord-pads-root");
	REQUIRE(window.app.waitUntil([&] { return window.call("chordPads.get").at("rootKey") == 2; }));
	CHECK(window.pads()[0].at("chord") == "D");
}

TEST_CASE("the chord assistant suggests chords after the selected pad's and assigns them", "[ui]")
{
	Window window;
	window.open();
	window.call("chordPads.assign", {{"assignments", {{{"pad", 6}, {"chord", ""}}}}});
	window.click("#chord-pads-assistant");
	window.click(Window::pad(0));
	REQUIRE(window.app.waitUntil([&] { return window.elements(".chord-suggestion").size() == 24; }));
	CHECK(window.text(".chord-suggestion[chord=\"F\"] .chord-suggestion-degree") == "IV");
	CHECK(window.hasClass(".chord-suggestion[chord=\"F\"]", "level-0"));

	// A double click assigns the suggestion to the first unassigned pad.
	window.click(".chord-suggestion[chord=\"F\"]", 2);
	REQUIRE(window.app.waitUntil([&] { return window.pads()[6].at("chord") == "F"; }));

	// The Circle of Fifths: the root key's chord first, the key's chords with their degrees.
	window.click("#chord-pads-page-circle");
	REQUIRE(
		window.app.waitUntil([&] { return window.elements(".chord-circle-outer .chord-suggestion").size() == 12; }));
	CHECK(window.text(".chord-circle-outer .chord-suggestion[chord=\"G\"] .chord-suggestion-degree") == "V");
	CHECK(window.hasClass(".chord-circle-inner .chord-suggestion[chord=\"Am\"]", "in-key"));
}
