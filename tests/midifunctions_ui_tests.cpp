// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI menu (#133, docs/features/midi-functions.md, reference manual pp. 1118-1131) in the app, driven with
// ui::Automation as MCP's UI tools drive it: MIDI > Functions on the project window's selected MIDI parts and on an
// editor's selected notes, and the Transpose Setup, MIDI Merge Options, Dissolve Part, Velocity, Delete Notes and
// Restrict Polyphony dialogs. tests/midifunctions_tests.cpp checks the functions themselves.
#include "engine/midi/midivalues.h"
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
					const Json parts = display.project().value("midiParts", Json::array());
					return std::any_of(
						parts.begin(), parts.end(), [added](const Json& p) { return p.at("id") == added; });
				},
				std::chrono::seconds(30))); // the app's first steps can take long on a busy machine
			return added;
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		bool shown(const std::string& selector)
		{
			const Json found = elements(selector);
			return !found.empty() && found[0].at("visible").get<bool>();
		}

		bool disabled(const std::string& selector)
		{
			const Json found = elements(selector);
			return !found.empty() && found[0].contains("attributes") && found[0].at("attributes").contains("disabled");
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

	// Keep Notes in Range: A flat (68) leaves 60-66 and goes to the limit, as no octave of it fits.
	f.dialog("midi.transposeSetup", "Transpose Setup");
	f.click("#midi-function-range");
	f.write("#midi-function-highest", "66");
	f.write("#midi-function-lowest", "60");
	f.ok();
	CHECK(f.are("pitch", {62, 65, 66}));

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

TEST_CASE("MIDI > Dissolve Part separates channels or pitches onto new tracks or lanes", "[ui]")
{
	Fixture f;
	f.app.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 3 * g_quarter}, {"length", 10}, {"pitch", 72}, {"velocity", 0.5}, {"channel", 1}}}}});
	const auto muted = [&f]
	{
		const Json parts = f.app.call("project.get").at("midiParts");
		return std::any_of(parts.begin(), parts.end(),
			[&f](const Json& part) { return part.at("id") == f.part && part.at("mute") == true; });
	};
	f.display.select({f.part});

	// Separate Channels, the default: a track for each channel, and the part is muted (p. 1122).
	f.dialog("midi.dissolvePart", "Dissolve Part");
	f.ok();
	CHECK(f.app.waitUntil([&] { return f.count("tracks") == 3 && f.count("midiParts") == 3; }));
	CHECK(muted());

	// Separate Pitches.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.count("tracks") == 1; }));
	f.dialog("midi.dissolvePart", "Dissolve Part");
	f.click("#midi-function-pitches");
	f.ok();
	CHECK(f.app.waitUntil([&] { return f.count("tracks") == 5 && f.count("midiParts") == 5; }));

	// Dissolve to Lanes: no new tracks, and Optimized Display is off with it (p. 1121).
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.count("tracks") == 1; }));
	f.dialog("midi.dissolvePart", "Dissolve Part");
	CHECK(!f.disabled("#midi-function-optimize"));
	f.click("#midi-function-pitches");
	f.click("#midi-function-lanes");
	CHECK(f.app.waitUntil([&] { return f.disabled("#midi-function-optimize"); }));
	f.ok();
	CHECK(f.app.waitUntil(
		[&]
		{
			const Json parts = f.app.call("project.get").at("midiParts");
			std::vector<int64_t> lanes;
			for (const Json& part : parts)
				lanes.push_back(part.at("lane").get<int64_t>());
			std::sort(lanes.begin(), lanes.end());
			return f.count("tracks") == 1 && lanes == std::vector<int64_t>{0, 1, 2, 3, 4};
		}));
	CHECK(muted());
}

TEST_CASE("MIDI > Merge MIDI in Loop merges between the locators, and Bounce MIDI combines parts", "[ui]")
{
	Fixture f;
	f.app.call("transport.set", {{"left", 0}, {"right", 480000}});

	// Without a selected track, onto a new MIDI track.
	f.dialog("midi.mergeInLoop", "MIDI Merge Options");
	f.ok();
	CHECK(f.app.waitUntil([&] { return f.count("tracks") == 2 && f.count("midiParts") == 2; }));

	// Merge MIDI in Loop takes the tracks that play: with only an audio track soloed, none does, and the dialog shows
	// the engine's answer.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.count("tracks") == 1; }));
	const Json audio = f.app.call("track.add", {{"type", "audio"}}).at("track");
	f.app.call("channel.set", {{"channel", audio.at("channel")}, {"solo", true}});
	f.dialog("midi.mergeInLoop", "MIDI Merge Options");
	f.click("#midi-function-ok");
	CHECK(f.app.waitUntil([&] { return !f.text("#midi-function-message").empty(); }));
	CHECK(f.shown("#midi-function"));
	f.click("#midi-function-cancel");
	REQUIRE(f.app.waitUntil([&] { return f.elements("#midi-function").empty(); }));
	f.app.call("history.undo"); // the solo
	f.app.call("history.undo"); // the audio track
	REQUIRE(f.app.waitUntil([&] { return f.count("tracks") == 1; }));

	// Onto the selected track, erasing what was there: one part with the three notes, which Include Inserts, on by
	// default, plays the track's MIDI modifiers into.
	std::vector<int> pitches;
	for (const Json& note : f.notesOf(f.part))
		pitches.push_back(note.at("pitch").get<int>() + 12);
	std::sort(pitches.begin(), pitches.end());
	const Json channel = f.app.call("project.get").at("tracks")[0].at("channel");
	f.app.call("channel.set", {{"channel", channel}, {"modifiers", {{"transpose", 12}}}});
	f.app.appUi().trackList()->select({f.track});
	f.dialog("midi.mergeInLoop", "MIDI Merge Options");
	const Json inserts = f.elements("#midi-function-inserts");
	REQUIRE(inserts.size() == 1);
	CHECK((inserts[0].contains("attributes") && inserts[0].at("attributes").contains("checked")));
	f.click("#midi-function-erase");
	f.ok();
	CHECK(f.app.waitUntil(
		[&]
		{
			const Json parts = f.app.call("project.get").at("midiParts");
			if (f.count("tracks") != 1 || parts.size() != 1 || parts[0].at("id") == f.part)
				return false;
			std::vector<int> merged;
			for (const Json& note : parts[0].at("notes"))
				merged.push_back(note.at("pitch").get<int>());
			std::sort(merged.begin(), merged.end());
			return merged == pitches;
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

namespace
{
	// A part of two bars with notes and controllers that every MIDI > Functions item without a dialog changes, each
	// in its own way: overlaps of the same pitch and of others, a double, a note that ends while the sustain pedal is
	// down, and a dense volume curve with modulation.
	uint64_t addRichPart(test::UiApp& app, uint64_t track, int64_t position)
	{
		const uint64_t part =
			app.call("midiPart.add", {{"track", track}, {"position", position}, {"length", 2 * g_bar}})
				.at("part")
				.at("id")
				.get<uint64_t>();
		const auto note = [](int64_t at, int64_t length, int pitch, double velocity)
		{ return Json{{"position", at}, {"length", length}, {"pitch", pitch}, {"velocity", velocity}}; };
		app.call("note.add",
			{{"part", part},
				{"notes",
					{note(0, 2 * g_quarter, 60, 0.5), note(g_quarter, 2 * g_quarter, 64, 0.6),
						note(g_quarter, g_quarter / 2, 64, 0.4), note(2 * g_quarter, g_quarter, 64, 0.3),
						note(4 * g_quarter, g_quarter / 4, 67, 0.7)}}});
		const auto cc = [](int64_t at, int number, double value)
		{ return Json{{"position", at}, {"number", number}, {"value", value}}; };
		app.call("controller.add",
			{{"part", part},
				{"controllers",
					{cc((4 * g_quarter) - 10, 64, 1.0), cc(5 * g_quarter, 64, 0.0), cc(0, 7, 0.1), cc(5, 7, 0.2),
						cc(10, 7, 0.3), cc(15, 7, 0.4), cc(0, 1, 0.5)}}});
		return part;
	}

	// What a part plays and a track's automation, without ids, for comparing two of them.
	Json withoutIds(Json items)
	{
		std::vector<std::string> sorted;
		for (Json& item : items)
		{
			item.erase("id");
			item.erase("track");
			if (item.contains("points"))
				item["points"] = withoutIds(item.at("points"));
			sorted.push_back(item.dump());
		}
		std::sort(sorted.begin(), sorted.end());
		return sorted;
	}

	Json contentOf(test::UiApp& app, uint64_t part, uint64_t track)
	{
		const Json project = app.call("project.get");
		for (const Json& each : project.at("midiParts"))
			if (each.at("id") == part)
				return {{"notes", withoutIds(each.at("notes"))}, {"controllers", withoutIds(each.at("controllers"))},
					{"automation", withoutIds(app.call("automationLane.list", {{"track", track}}).at("lanes"))}};
		return nullptr;
	}
}

TEST_CASE("Every item of MIDI > Functions runs its engine function with its settings", "[ui][slow]")
{
	Fixture f;
	// The menu runs each on parts of one track, the engine on the same parts of another.
	const uint64_t other = f.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	struct Item
	{
		const char* command;
		const char* engine;
		Json parameters;
	};
	const std::vector<Item> items = {
		{"midi.functionsLegato", "midi.legato", {{"overlap", 0}, {"betweenSelected", false}}},
		{"midi.functionsFixedLengths", "midi.fixLengths", {{"length", g_quarter / 4}}}, // the quantize grid
		{"midi.functionsFixedVelocity", "midi.fixVelocities", {{"velocity", 100.0 / 127.0}}},
		{"midi.functionsPedalsToNoteLength", "midi.pedalToNoteLength", Json::object()},
		{"midi.functionsDeleteOverlapsMono", "midi.deleteOverlaps", {{"mode", "mono"}}},
		{"midi.functionsDeleteOverlapsPoly", "midi.deleteOverlaps", {{"mode", "poly"}}},
		{"midi.functionsDeleteDoubles", "midi.deleteDoubles", Json::object()},
		{"midi.functionsDeleteControllers", "midi.deleteControllers", Json::object()},
		{"midi.functionsDeleteContinuousControllers", "midi.deleteContinuousControllers", Json::object()},
		{"midi.functionsThinOut", "midi.thinOut", Json::object()},
		{"midi.functionsExtractAutomation", "midi.extractAutomation", Json::object()},
		{"midi.functionsReverse", "midi.reverse", Json::object()},
		{"midi.functionsMirror", "midi.invert", Json::object()},
	};

	// Every item of the submenu is here or opens a dialog that a test of its own runs.
	std::vector<std::string> listed;
	for (const Json& item : f.elements("#midi-functions-submenu menuitem"))
		if (item.contains("attributes") && item.at("attributes").contains("command"))
			listed.push_back(item.at("attributes").at("command").get<std::string>());
	std::vector<std::string> expected = {"midi.functionsVelocity", "midi.functionsDeleteNotes",
		"midi.functionsRestrictPolyphony", "midi.calculateTempo"};
	for (const Item& item : items)
		expected.emplace_back(item.command);
	std::sort(listed.begin(), listed.end());
	std::sort(expected.begin(), expected.end());
	CHECK(listed == expected);

	int64_t position = g_bar;
	std::vector<Json> results;
	for (const Item& item : items)
	{
		INFO(item.command);
		const uint64_t viaMenu = addRichPart(f.app, f.track, position);
		const uint64_t viaEngine = addRichPart(f.app, other, position);
		position += 2 * g_bar;
		const Json before = contentOf(f.app, viaMenu, f.track);
		REQUIRE(f.app.waitUntil(
			[&]
			{
				const Json parts = f.display.project().value("midiParts", Json::array());
				return std::any_of(
					parts.begin(), parts.end(), [viaMenu](const Json& p) { return p.at("id") == viaMenu; });
			}));
		f.display.select({viaMenu});
		f.menu(item.command, true);
		CHECK(f.app.waitUntil([&] { return contentOf(f.app, viaMenu, f.track) != before; }));
		Json parameters = item.parameters;
		parameters["parts"] = {viaEngine};
		f.app.call(item.engine, parameters);
		const Json result = contentOf(f.app, viaMenu, f.track);
		CHECK(result == contentOf(f.app, viaEngine, other));
		results.push_back(result);
	}
	// No two items do the same, so none can stand in for another.
	for (size_t i = 0; i < results.size(); ++i)
		for (size_t j = i + 1; j < results.size(); ++j)
		{
			INFO(items[i].command << " and " << items[j].command);
			CHECK(results[i] != results[j]);
		}
}

TEST_CASE("MIDI > Functions > Legato between selected notes and Fixed Lengths with tuplets", "[ui]")
{
	Fixture f;
	// Fixed Lengths takes the grid's step with its tuplet, as Edit > Quantize Lengths does: a 1/8 triplet.
	f.app.call("quantize.set", {{"settings", {{"grid", "1/8"}, {"tuplet", 3}}}});
	f.display.select({f.part});
	f.menu("midi.functionsFixedLengths", true);
	CHECK(f.are("length", {g_quarter / 3, g_quarter / 3, g_quarter / 3}));

	// Legato Mode: Between Selected Notes Only (p. 1599): the first of two selected notes reaches the second.
	f.app.call("settings.set", {{"key", "editing.legatoBetweenSelected"}, {"value", true}});
	f.app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part})}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() != nullptr; }));
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Element* zone = window->context().GetDocument(0)->GetElementById("main-zone");
	REQUIRE(zone != nullptr);
	f.app.call("editor.set",
		{{"editor", "window"},
			{"selection",
				Json::array({{{"part", f.part}, {"note", f.notes[0]}}, {{"part", f.part}, {"note", f.notes[2]}}})}});
	f.app.shell().step(std::chrono::milliseconds(0)); // editor.changed
	f.app.keys().run("midi.functionsLegato", Json::object(), *window, zone);
	CHECK(f.are("length", {2 * g_quarter, g_quarter / 3, g_quarter / 3}));
}

TEST_CASE("MIDI > Functions > Velocity and Delete Notes change the selected notes", "[ui]")
{
	Fixture f;
	CHECK(!f.app.keys().canRun("midi.functionsVelocity", f.app.main(), nullptr));
	f.display.select({f.part});
	// The notes' velocities as MIDI 1.0 values, as playback sends them.
	const auto velocities = [&f](int expected)
	{
		return f.app.waitUntil(
			[&]
			{
				const std::vector<Json> notes = f.notesOf(f.part);
				return notes.size() == 3 &&
					std::all_of(notes.begin(), notes.end(),
						[expected](const Json& note)
						{
							const auto velocity =
								static_cast<uint16_t>(std::lround(note.at("velocity").get<double>() * 65535.0));
							return engine::midi::velocityToMidi1(velocity) == expected;
						});
			});
	};

	// Add/Subtract, in MIDI 1.0 values as playback maps them: 64 down by 1 is 63, not 62.
	f.menu("midi.functionsVelocity", true);
	REQUIRE(f.app.waitUntil([&] { return f.shown("#midi-function"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.text("#midi-function-title") == "Velocity");
	REQUIRE(velocities(64));
	f.write("#midi-function-amount", "-1");
	f.ok();
	CHECK(velocities(63));

	// Values outside the dialog's ranges are refused in the dialog's units, and the dialog stays open.
	f.menu("midi.functionsVelocity", true);
	REQUIRE(f.app.waitUntil([&] { return f.shown("#midi-function"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.write("#midi-function-amount", "200");
	f.click("#midi-function-ok");
	CHECK(f.app.waitUntil([&] { return f.text("#midi-function-message") == "Amount must be -127 to 127"; }));
	CHECK(f.shown("#midi-function"));
	f.choose("#midi-function-type", 1); // Compress/Expand
	f.write("#midi-function-ratio", "400");
	f.click("#midi-function-ok");
	CHECK(f.app.waitUntil([&] { return f.text("#midi-function-message") == "Ratio must be 0 to 300"; }));
	f.click("#midi-function-cancel");
	CHECK(f.app.waitUntil([&] { return !f.shown("#midi-function"); }));
	CHECK(velocities(63));

	// Compress/Expand with a ratio in percent, Variance within its range, and Fixed To.
	const auto velocityDialog = [&f](int type)
	{
		f.menu("midi.functionsVelocity", true);
		REQUIRE(f.app.waitUntil([&] { return f.shown("#midi-function"); }));
		f.app.shell().step(std::chrono::milliseconds(0));
		f.choose("#midi-function-type", type);
	};
	velocityDialog(1);
	f.write("#midi-function-ratio", "50");
	f.ok();
	CHECK(velocities(31));
	velocityDialog(4);
	f.write("#midi-function-variance", "50");
	f.ok();
	CHECK(f.app.waitUntil(
		[&]
		{
			const std::vector<Json> notes = f.notesOf(f.part);
			return std::all_of(notes.begin(), notes.end(),
					   [](const Json& note) { return note.at("velocity").get<double>() <= 31.5 / 127.0; }) &&
				std::any_of(notes.begin(), notes.end(),
					[](const Json& note) { return note.at("velocity").get<double>() < 15872.0 / 65535.0; });
		}));
	velocityDialog(3);
	f.write("#midi-function-amount", "63");
	f.ok();
	CHECK(velocities(63));

	// Limit: only its fields are on.
	f.menu("midi.functionsVelocity", true);
	REQUIRE(f.app.waitUntil([&] { return f.shown("#midi-function"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.choose("#midi-function-type", 2);
	CHECK(f.app.waitUntil([&] { return f.disabled("#midi-function-amount") && !f.disabled("#midi-function-upper"); }));
	f.write("#midi-function-upper", "50");
	f.ok();
	CHECK(velocities(50));

	// Delete Notes: a note exactly at Minimum Velocity stays; one below it goes.
	f.menu("midi.functionsDeleteNotes", true);
	REQUIRE(f.app.waitUntil([&] { return f.shown("#midi-function"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.click("#midi-function-length-on");
	f.click("#midi-function-velocity-on");
	f.write("#midi-function-velocity", "50");
	f.ok();
	CHECK(velocities(50));
	f.menu("midi.functionsDeleteNotes", true);
	REQUIRE(f.app.waitUntil([&] { return f.shown("#midi-function"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.click("#midi-function-length-on");
	f.click("#midi-function-velocity-on");
	f.write("#midi-function-velocity", "51");
	f.ok();
	CHECK(f.app.waitUntil([&] { return f.notesOf(f.part).empty(); }));

	// Delete Notes: Minimum Length in ticks of 480 per quarter; Remove when under counts with both minimums only.
	const uint64_t mixed = f.addPart(g_bar, {{0, 60}, {g_quarter, 62}});
	f.app.call("midi.fixLengths", {{"parts", {mixed}}, {"notes", {f.notes.back()}}, {"length", g_quarter / 8}});
	f.display.select({mixed});
	f.menu("midi.functionsDeleteNotes", true);
	REQUIRE(f.app.waitUntil([&] { return f.shown("#midi-function"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.text("#midi-function-title") == "Delete Notes");
	CHECK(f.disabled("#midi-function-when"));
	CHECK(f.disabled("#midi-function-velocity"));
	f.write("#midi-function-length", "100"); // 1/8 is 240, 1/32 60
	f.ok();
	CHECK(f.app.waitUntil([&] { return f.notesOf(mixed).size() == 1; }));
	CHECK(f.notesOf(mixed)[0].at("pitch") == 60);
}

TEST_CASE("The MIDI menu's dialogs open one at a time", "[ui]")
{
	Fixture f;
	f.display.select({f.part});
	f.dialog("midi.transposeSetup", "Transpose Setup");
	// Another one, from MCP or another window, brings the open one to the front as it is, with what was typed.
	f.write("#midi-function-semitones", "1");
	f.app.keys().run("midi.dissolvePart", Json::object(), f.app.main(), nullptr);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements("#midi-function").size() == 1);
	CHECK(f.text("#midi-function-title") == "Transpose Setup");
	f.ok();
	CHECK(f.are("pitch", {61, 65, 68}));

	// The add track and auto fades dialogs too.
	for (const char* command : {"project.addTrack", "project.autoFades"})
	{
		INFO(command);
		const std::string selector = std::string(command) == "project.addTrack" ? "#add-track-dialog" : "#auto-fades";
		f.app.keys().run(command, Json::object(), f.app.main(), nullptr);
		f.app.keys().run(command, Json::object(), f.app.main(), nullptr);
		f.app.shell().step(std::chrono::milliseconds(0));
		CHECK(f.app.waitUntil([&] { return f.shown(selector); }));
		CHECK(f.elements(selector).size() == 1);
		f.click(std::string(command) == "project.addTrack" ? "#add-track-cancel" : "#auto-fades-cancel");
		CHECK(f.app.waitUntil([&] { return f.elements(selector).empty(); }));
	}
}
