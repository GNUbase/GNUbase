// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI menu (#133, docs/features/midi-functions.md, reference manual pp. 1118-1131) in the app, driven with
// ui::Automation as MCP's UI tools drive it: MIDI > Functions on the project window's selected MIDI parts and on an
// editor's selected notes, and the Transpose Setup, MIDI Merge Options, Dissolve Part and Restrict Polyphony dialogs.
// tests/midifunctions_tests.cpp checks the functions themselves.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;

	// A MIDI track with a one-bar part "Keys" at the project start and three notes, each an 1/8 long with velocity
	// 0.5: C3 on the first beat, E3 on the second and G3 on the third, in the project window.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			part = addPart(0, {{0, 60}, {g_quarter, 64}, {2 * g_quarter, 67}});
		}

		// A part of a bar at position on the track, with 1/8 notes at the positions and pitches.
		uint64_t addPart(int64_t position, const std::vector<std::pair<int64_t, int>>& played)
		{
			const uint64_t added =
				app.call(
					   "midiPart.add", {{"track", track}, {"position", position}, {"length", g_bar}, {"name", "Keys"}})
					.at("part")
					.at("id")
					.get<uint64_t>();
			Json items = Json::array();
			for (const auto& [at, pitch] : played)
				items.push_back({{"position", at}, {"length", g_quarter / 2}, {"pitch", pitch}, {"velocity", 0.5}});
			const Json result = app.call("note.add", {{"part", added}, {"notes", items}});
			for (const Json& note : result.at("notes"))
				notes.push_back(note.at("id").get<uint64_t>());
			REQUIRE(app.waitUntil(
				[this, added]
				{
					const Json parts = display.project().is_object()
						? display.project().value("midiParts", Json::array())
						: Json::array();
					return std::any_of(
						parts.begin(), parts.end(), [added](const Json& p) { return p.at("id") == added; });
				}));
			return added;
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		bool shown(const std::string& selector)
		{
			const Json found = elements(selector);
			return !found.empty() && found[0].at("visible").get<bool>();
		}

		std::string text(const std::string& selector)
		{
			const Json found = elements(selector);
			return found.empty() ? std::string() : found[0].value("text", std::string());
		}

		void click(const std::string& selector)
		{
			INFO(selector);
			REQUIRE(app.waitUntil([&] { return shown(selector); }));
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		// MIDI > item, or MIDI > Functions > item.
		void menu(const std::string& command, bool functions = false)
		{
			click("#menu-bar button[menu=midi-menu]");
			if (functions)
			{
				app.ui().move(std::nullopt, {"#midi-functions-submenu", 0, std::nullopt});
				app.shell().step(std::chrono::milliseconds(0));
			}
			click("menuitem[command=\"" + command + "\"]");
		}

		// Types text into a text field.
		void write(const std::string& selector, const std::string& value)
		{
			click(selector);
			app.ui().key(std::nullopt, "Mod+A");
			app.ui().text(std::nullopt, value);
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Opens a drop-down list, moves the selection that many options down with the arrow keys, and closes it.
		void choose(const std::string& select, int down)
		{
			click(select);
			for (int i = 0; i < down; ++i)
				app.ui().key(std::nullopt, "Down");
			click(select);
		}

		// Opens a dialog from the MIDI menu.
		void dialog(const std::string& command, const std::string& title)
		{
			menu(command);
			REQUIRE(app.waitUntil([this] { return shown("#midi-function"); }));
			app.shell().step(std::chrono::milliseconds(0)); // its Lua fills it when it shows
			CHECK(text("#midi-function-title") == title);
		}

		// OK, which closes the dialog once the engine took the function.
		void ok()
		{
			click("#midi-function-ok");
			CHECK(app.waitUntil([this] { return elements("#midi-function").empty(); }));
		}

		// The notes of a part, by position.
		std::vector<Json> notesOf(uint64_t id)
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == id)
				{
					std::vector<Json> sorted(each.at("notes").begin(), each.at("notes").end());
					std::stable_sort(sorted.begin(), sorted.end(), [](const Json& a, const Json& b)
						{ return a.at("position").get<int64_t>() < b.at("position").get<int64_t>(); });
					return sorted;
				}
			return {};
		}

		std::vector<int64_t> values(const char* field, uint64_t of = 0)
		{
			std::vector<int64_t> found;
			for (const Json& note : notesOf(of != 0 ? of : part))
				found.push_back(note.at(field).get<int64_t>());
			return found;
		}

		bool are(const char* field, const std::vector<int64_t>& expected)
		{
			return app.waitUntil([&] { return values(field) == expected; });
		}

		size_t count(const char* kind) { return app.call("project.get").at(kind).size(); }

		test::UiApp app{{1024, 720}};
		ui::EventDisplay& display;
		uint64_t track = 0;
		uint64_t part = 0;
		std::vector<uint64_t> notes;
	};
}

TEST_CASE("MIDI > Functions edit the selected MIDI parts", "[ui]")
{
	Fixture f;
	// Without a selection there is nothing to edit.
	CHECK(!f.app.keys().canRun("midi.functionsLegato", f.app.main(), nullptr));
	f.display.select({f.part});
	CHECK(f.app.keys().canRun("midi.functionsLegato", f.app.main(), nullptr));

	// Legato with the Legato Overlap setting, in ticks of 480 per quarter: the last note keeps its length.
	f.menu("midi.functionsLegato", true);
	CHECK(f.are("length", {g_quarter, g_quarter, g_quarter / 2}));
	f.app.call("settings.set", {{"key", "editing.legatoOverlap"}, {"value", -240}});
	f.menu("midi.functionsLegato", true);
	CHECK(f.are("length", {g_quarter / 2, g_quarter / 2, g_quarter / 2}));

	// Fixed Lengths in the project window: the quantize grid, 1/16.
	f.menu("midi.functionsFixedLengths", true);
	CHECK(f.are("length", {g_quarter / 4, g_quarter / 4, g_quarter / 4}));

	// Fixed Velocity: the default insert velocity, 100.
	f.menu("midi.functionsFixedVelocity", true);
	CHECK(f.app.waitUntil(
		[&]
		{
			const std::vector<Json> notes = f.notesOf(f.part);
			return std::all_of(notes.begin(), notes.end(),
				[](const Json& note) { return std::abs(note.at("velocity").get<double>() - (100.0 / 127.0)) < 0.005; });
		}));

	// Reverse plays them backwards, Mirror back again.
	f.menu("midi.functionsReverse", true);
	CHECK(f.are("pitch", {67, 64, 60}));
	f.menu("midi.functionsMirror", true);
	CHECK(f.are("pitch", {60, 64, 67}));

	// Each is one undo step.
	f.app.call("history.undo");
	CHECK(f.are("pitch", {67, 64, 60}));
}

TEST_CASE("MIDI > Functions take an editor's selected notes, its length quantize and insert velocity", "[ui]")
{
	Fixture f;
	f.app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part})}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() != nullptr; }));
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Element* zone = window->context().GetDocument(0)->GetElementById("main-zone");
	REQUIRE(zone != nullptr);
	f.app.call("editor.set",
		{{"editor", "window"}, {"selection", Json::array({{{"part", f.part}, {"note", f.notes[1]}}})},
			{"lengthQuantize", "1/4"}, {"insertVelocity", 0.25}});
	f.app.shell().step(std::chrono::milliseconds(0)); // editor.changed

	f.app.keys().run("midi.functionsFixedLengths", Json::object(), *window, zone);
	CHECK(f.are("length", {g_quarter / 2, g_quarter, g_quarter / 2}));
	f.app.keys().run("midi.functionsFixedVelocity", Json::object(), *window, zone);
	CHECK(f.app.waitUntil(
		[&]
		{
			const std::vector<Json> notes = f.notesOf(f.part);
			const auto near = [](const Json& note, double velocity)
			{ return std::abs(note.at("velocity").get<double>() - velocity) < 0.001; };
			return notes.size() == 3 && near(notes[1], 0.25) && near(notes[0], 0.5);
		}));

	// Without a selected note, every note the editor edits.
	f.app.call("editor.set", {{"editor", "window"}, {"selection", Json::array()}});
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.keys().run("midi.functionsFixedLengths", Json::object(), *window, zone);
	CHECK(f.are("length", {g_quarter, g_quarter, g_quarter}));
}

TEST_CASE("MIDI > Transpose Setup transposes by semitones and corrects to a scale", "[ui]")
{
	Fixture f;
	CHECK(!f.app.keys().canRun("midi.transposeSetup", f.app.main(), nullptr));
	f.display.select({f.part});
	f.dialog("midi.transposeSetup", "Transpose Setup");
	f.write("#midi-function-semitones", "2");
	f.ok();
	CHECK(f.are("pitch", {62, 66, 69}));

	// C major to C minor: D stays, F# goes to the closest note of C minor (F, the lower one), A to A flat.
	f.dialog("midi.transposeSetup", "Transpose Setup");
	f.click("#midi-function-scale");
	f.choose("#midi-function-to-type", 1);
	f.ok();
	CHECK(f.are("pitch", {62, 65, 68}));

	// An answer the engine refuses keeps the dialog open with its message.
	f.dialog("midi.transposeSetup", "Transpose Setup");
	f.write("#midi-function-semitones", "200");
	f.click("#midi-function-ok");
	CHECK(f.shown("#midi-function"));
	CHECK(!f.text("#midi-function-message").empty());
	f.click("#midi-function-cancel");
	CHECK(f.app.waitUntil([&] { return f.elements("#midi-function").empty(); }));
}

TEST_CASE("MIDI > Functions > Restrict Polyphony limits the voices", "[ui]")
{
	Fixture f;
	const uint64_t chord = f.addPart(g_bar, {{0, 60}, {0, 64}, {0, 67}});
	f.display.select({chord});
	f.menu("midi.functionsRestrictPolyphony", true);
	REQUIRE(f.app.waitUntil([&] { return f.shown("#midi-function"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.text("#midi-function-title") == "Restrict Polyphony");
	f.write("#midi-function-voices", "2");
	f.ok();
	CHECK(f.app.waitUntil([&] { return f.notesOf(chord).size() == 2; }));
	CHECK(f.notesOf(f.part).size() == 3);
}

TEST_CASE("MIDI > Dissolve Part separates pitches onto new tracks", "[ui]")
{
	Fixture f;
	f.display.select({f.part});
	f.dialog("midi.dissolvePart", "Dissolve Part");
	f.click("#midi-function-pitches");
	f.ok();
	// A track for each pitch, and the emptied part is gone.
	CHECK(f.app.waitUntil([&] { return f.count("tracks") == 4 && f.count("midiParts") == 3; }));
}

TEST_CASE("MIDI > Merge MIDI in Loop merges between the locators, and Bounce MIDI combines parts", "[ui]")
{
	Fixture f;
	f.app.call("transport.set", {{"left", 0}, {"right", 480000}});

	// Without a selected track, onto a new MIDI track.
	f.dialog("midi.mergeInLoop", "MIDI Merge Options");
	f.ok();
	CHECK(f.app.waitUntil([&] { return f.count("tracks") == 2 && f.count("midiParts") == 2; }));

	// Onto the selected track, erasing what was there: one part with the three notes.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.count("tracks") == 1; }));
	f.app.appUi().trackList()->select({f.track});
	f.dialog("midi.mergeInLoop", "MIDI Merge Options");
	f.click("#midi-function-erase");
	f.ok();
	CHECK(f.app.waitUntil(
		[&]
		{
			const Json parts = f.app.call("project.get").at("midiParts");
			return f.count("tracks") == 1 && parts.size() == 1 && parts[0].at("id") != f.part &&
				parts[0].at("notes").size() == 3;
		}));

	// Bounce MIDI: two parts on the track into one.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.count("midiParts") == 1; }));
	const uint64_t second = f.addPart(g_bar, {{0, 72}});
	CHECK(!f.app.keys().canRun("midi.bounceMidi", f.app.main(), nullptr));
	f.display.select({f.part, second});
	f.menu("midi.bounceMidi");
	CHECK(f.app.waitUntil(
		[&]
		{
			const Json parts = f.app.call("project.get").at("midiParts");
			return parts.size() == 1 && parts[0].at("notes").size() == 4;
		}));
}
