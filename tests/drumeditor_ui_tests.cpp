// SPDX-License-Identifier: GPL-3.0-or-later
// The drum editor in the app (#129, docs/features/drum-editor.md), driven with ui::Automation as MCP's UI tools drive
// it: the drum sound list of the track's drum map, its notes as diamonds on the sounds' rows, the drumstick, the Map
// menu and Drum-Map Link. tests/drummaps_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_sixteenth = g_quarter / 4;
	constexpr int64_t g_bar = 4 * g_quarter;

	ui::Target of(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	std::string sound(int pitch)
	{
		return ".drum-sound[pitch=\"" + std::to_string(pitch) + "\"]";
	}

	// A MIDI track using a copy of the GM Map, with a one-bar part at the project start: Open/Close Editor opens it
	// in the drum editor.
	struct Fixture
	{
		Fixture()
		{
			track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			const Json project = app.call("project.get");
			map = app.call("drumMap.add", {{"copy", project.at("drumMaps").at(0).at("id")}, {"name", "Kit"}})
					  .at("map")
					  .at("id")
					  .get<uint64_t>();
			app.call("track.set", {{"track", track}, {"drumMap", map}});
			part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}, {"name", "Beat"}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
			app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part})}});
			REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
			window = app.appUi().editorWindowId();
			REQUIRE(app.waitUntil([&] { return elements(".drum-editor .drum-sound").size() == 128; }));
		}

		Json elements(const std::string& selector) { return app.ui().dom(window, selector, 0).at("elements"); }

		Json box(const std::string& selector) { return elements(selector).at(0).at("box"); }

		bool has(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.empty())
				return false;
			const Json names = found.at(0).value("classes", Json::array());
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		Json partNotes()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == part)
				{
					std::vector<Json> sorted(each.at("notes").begin(), each.at("notes").end());
					std::sort(sorted.begin(), sorted.end(), [](const Json& a, const Json& b)
						{ return a.at("position").get<int64_t>() < b.at("position").get<int64_t>(); });
					return sorted;
				}
			return Json::array();
		}

		Json drumMap()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("drumMaps"))
				if (each.at("id") == map)
					return each;
			return nullptr;
		}

		void scroll(int pitch)
		{
			REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow()->keyEditor()->scrollToPitch(pitch); }));
			app.shell().step(std::chrono::milliseconds(0));
		}

		// A point of the note display: at sixteenths from the project start, on the sound's row.
		ui::Target at(double sixteenths, int pitch)
		{
			const Json grid = box(".key-editor-grid");
			const Json row = box(sound(pitch));
			const double x = grid.at("x").get<double>() + (sixteenths / 16.0 * grid.at("width").get<double>());
			const double y = row.at("y").get<double>() + (row.at("height").get<double>() / 2.0);
			return {"", 0, ui::Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}};
		}

		void click(const ui::Target& target, int count = 1)
		{
			app.ui().click(window, target, ui::MouseButton::Left, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Types into a field of the drum sound list, then waits until the list shows what the engine made of it.
		void type(const std::string& selector, const std::string& text, const std::string& shown)
		{
			click(of(selector), 2);
			app.ui().key(window, "Mod+A");
			app.ui().text(window, text);
			app.ui().key(window, "Enter");
			app.shell().step(std::chrono::milliseconds(0));
			REQUIRE(app.waitUntil(
				[&]
				{
					const Json found = elements(selector);
					return !found.empty() && found.at(0).at("attributes").value("value", "") == shown;
				}));
		}

		test::UiApp app{{1200, 800}};
		std::optional<ui::WindowId> window;
		uint64_t track = 0;
		uint64_t map = 0;
		uint64_t part = 0;
	};
}

TEST_CASE("the drum editor lists the drum map's sounds and shows notes as diamonds on their rows", "[ui]")
{
	Fixture f;
	CHECK(f.app.call("editor.get", {{"editor", "window"}}).at("content") == "drum");
	CHECK(f.elements(sound(36) + " .drum-sound-name").at(0).at("attributes").at("value") == "Bass Drum 1");
	CHECK(f.elements(sound(36) + " .drum-sound-pitch").at(0).at("text") == "C1");
	CHECK(f.elements("#drum-map").at(0).value("text", "") == "Kit");
	f.app.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", g_quarter}, {"length", g_sixteenth}, {"pitch", 38}, {"velocity", 0.5}},
					{{"position", 0}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 2; }));
	f.scroll(37);
	// Each diamond is on its sound's row, centered on its start.
	const Json grid = f.box(".key-editor-grid");
	const Json snare = f.box(".drum-note[note=\"" + std::to_string(f.partNotes()[1].at("id").get<uint64_t>()) + "\"]");
	const Json row = f.box(sound(38));
	const double center = snare.at("x").get<double>() + (snare.at("width").get<double>() / 2.0);
	CHECK(std::abs(center - grid.at("x").get<double>() - (grid.at("width").get<double>() / 4.0)) <= 2.0);
	CHECK(snare.at("y").get<double>() >= row.at("y").get<double>() - 4.0);
	CHECK(snare.at("y").get<double>() + snare.at("height").get<double>() <=
		row.at("y").get<double>() + row.at("height").get<double>() + 4.0);

	// The drum sound list changes the map: the mute button and the O-note typed.
	f.click(of(sound(38) + " .drum-sound-mute"));
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(38).at("mute") == true; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(sound(38) + " .drum-sound-mute", "checked"); }));
	f.type(sound(36) + " textdisplay[sound-field=oNote]", "C2", "C2");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(36).at("oNote") == 48; }));
	f.type(sound(36) + " textdisplay[sound-field=channel]", "Any", "Any");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(36).at("channel").is_null(); }));
	f.type(sound(36) + " textdisplay[sound-field=name]", "Kick", "Kick");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(36).at("name") == "Kick"; }));
}

TEST_CASE("the drumstick inserts notes of the sound's snap with Drum-Map Link and deletes them", "[ui]")
{
	Fixture f;
	f.app.call("drumMap.set", {{"map", f.map}, {"sounds", {{{"pitch", 42}, {"snap", "1/8"}}}}});
	f.scroll(42);
	f.click(of(".key-tool[key-tool=draw]"));
	REQUIRE(f.has(".key-tool[key-tool=draw]", "checked"));
	CHECK(f.elements(".key-tool[key-tool=draw]").at(0).value("text", "") == "Drumstick");
	f.click(of("#key-length-quantize"));
	f.click(of("#key-length-menu menuitem[length=\"drumMap\"]"));
	REQUIRE(
		f.app.waitUntil([&] { return f.elements("#key-length-quantize").at(0).value("text", "") == "Drum-Map Link"; }));
	CHECK(f.app.call("editor.get", {{"editor", "window"}}).at("lengthQuantize") == "drumMap");
	// A click inserts a note at the snapped position, as long as the hi-hat's snap.
	f.click(f.at(4.2, 42));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 1; }));
	CHECK(f.partNotes()[0].at("pitch") == 42);
	CHECK(f.partNotes()[0].at("position") == g_quarter);
	CHECK(f.partNotes()[0].at("length") == g_quarter / 2);
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 1; }));
	// A click on a note deletes it (p. 1242).
	f.click(of(".drum-note"));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().empty(); }));
	// A drag to the right inserts notes at every step of the grid (p. 1241).
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").empty(); }));
	f.app.ui().drag(f.window, f.at(0.2, 36), f.at(3.5, 36));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 4; }));
	CHECK(f.partNotes()[3].at("position") == 3 * g_sixteenth);
}

TEST_CASE("the Map menu assigns, adds, copies and removes drum maps", "[ui]")
{
	Fixture f;
	f.click(of("#drum-map"));
	CHECK(f.elements("#drum-map-menu menuitem[drum-map]").size() == 3); // No Drum Map, GM Map, Kit
	CHECK(f.elements("#drum-map-menu menuitem[checked]").at(0).value("text", "") == "Kit");
	f.click(of("#drum-map-menu menuitem[drum-map-action=new]"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("drumMaps").size() == 3; }));
	// The new map is the track's.
	REQUIRE(f.app.waitUntil([&] { return f.elements("#drum-map").at(0).value("text", "") == "Empty Map"; }));
	REQUIRE(f.app.waitUntil(
		[&] { return f.elements(sound(0) + " .drum-sound-name").at(0).at("attributes").at("value") == "Sound 1"; }));
	f.click(of("#drum-map"));
	f.click(of("#drum-map-menu menuitem[drum-map-action=remove]"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("drumMaps").size() == 2; }));
	// Without a map, the list names the pitches.
	REQUIRE(f.app.waitUntil([&] { return f.elements("#drum-map").at(0).value("text", "") == "No Drum Map"; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(sound(36) + " .drum-sound-name").empty(); }));
	f.click(of("#drum-map"));
	f.click(of("#drum-map-menu menuitem[drum-map=\"" + std::to_string(f.map) + "\"]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#drum-map").at(0).value("text", "") == "Kit"; }));
}

TEST_CASE("Open Drum Editor opens the selected MIDI parts in the drum editor window", "[ui]")
{
	test::UiApp app{{1200, 800}};
	ui::EventDisplay& display = app.appUi().showProject();
	const auto track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	REQUIRE(
		app.waitUntil([&] { return display.project().is_object() && display.project().at("midiParts").size() == 1; }));
	display.select({part});
	app.shell().step(std::chrono::milliseconds(0));
	for (const char* command : {"editor.openDrum", "drumMap.load", "drumMap.save"})
		CHECK(app.ui()
				  .dom(std::nullopt, std::string("#edit-menu menuitem[command=\"") + command + "\"]", 0)
				  .at("elements")
				  .size() == 1);
	app.keys().run("editor.openDrum", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	CHECK(app.call("editor.get", {{"editor", "window"}}).at("content") == "drum");
	const ui::WindowId window = app.appUi().editorWindowId();
	// Without a drum map the list names the pitches only.
	REQUIRE(app.waitUntil([&] { return app.ui().dom(window, ".drum-sound", 0).at("elements").size() == 128; }));
	CHECK(app.ui().dom(window, ".drum-sound-name", 0).at("elements").empty());
}
