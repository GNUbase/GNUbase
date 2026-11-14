// SPDX-License-Identifier: GPL-3.0-or-later
// The drum editor in the app (#129, docs/features/drum-editor.md), driven with ui::Automation as MCP's UI tools drive
// it: the drum sound list of the track's drum map, its notes as diamonds on the sounds' rows, the drumstick, the Map
// menu and Drum-Map Link, Pitch Visibility, Show Note Length, auditioning sounds and the Drum Map Setup.
// tests/drummaps_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/rmlui.h"
#include "uiapp.h"

#include <RmlUi/Core/Core.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
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
		explicit Fixture(const std::string& type = "midi")
		{
			track = app.call("track.add", {{"type", type}}).at("track").at("id").get<uint64_t>();
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

		// A point inside an element's box, dx from its left edge, in the middle of its height.
		ui::Target inside(const std::string& selector, double dx)
		{
			const Json found = box(selector);
			return {"", 0,
				ui::Point{static_cast<int>(std::lround(found.at("x").get<double>() + dx)),
					static_cast<int>(
						std::lround(found.at("y").get<double>() + (found.at("height").get<double>() / 2.0)))}};
		}

		std::string noteSelector(size_t index)
		{
			return "[note=\"" + std::to_string(partNotes()[index].at("id").get<uint64_t>()) + "\"]";
		}

		// Answers the OS's file dialog that is open with a file.
		void answer(const std::filesystem::path& file)
		{
			REQUIRE(app.waitUntil([this] { return app.shell().fileDialogs().size() == 1; }));
			app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {file});
		}

		// A double click at from whose second press drags to to, through via.
		void doubleClickDrag(const ui::Target& from, const ui::Target& via, const ui::Target& to)
		{
			auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
			REQUIRE(clock != nullptr);
			clock->freeze(true); // the two presses are within the double click time however long a frame takes
			const std::shared_ptr<void> thaw(nullptr, [clock](void*) { clock->freeze(false); });
			app.ui().click(window, from, ui::MouseButton::Left, 1, {});
			app.ui().press(window, from, ui::MouseButton::Left);
			app.ui().move(window, via);
			app.ui().release(window, to, ui::MouseButton::Left);
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Drags a note's end, then waits until the editor shows the new length in its info line.
		void resize(const ui::Target& from, const ui::Target& to)
		{
			const std::string before = value("textdisplay[field=length]");
			app.ui().drag(window, from, to);
			app.shell().step(std::chrono::milliseconds(0));
			REQUIRE(app.waitUntil([&] { return value("textdisplay[field=length]") != before; }));
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

		// Types into a text field of the Drum Map Setup and presses Enter.
		void write(const std::string& selector, const std::string& text)
		{
			INFO(selector);
			REQUIRE(app.waitUntil([&] { return !elements(selector).empty(); }));
			click(of(selector));
			app.ui().key(window, "Mod+A");
			app.ui().text(window, text);
			app.ui().key(window, "Enter");
			app.shell().step(std::chrono::milliseconds(0));
		}

		std::string value(const std::string& selector)
		{
			const Json found = elements(selector);
			return found.empty() ? std::string() : found.at(0).at("attributes").value("value", std::string());
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
	// The MIDI menu's Drum Map Setup and O-Note Conversion (pp. 1235, 1240).
	for (const char* command : {"drumMap.setup", "midi.convertONotes"})
		CHECK(app.ui()
				  .dom(std::nullopt, std::string("#midi-menu menuitem[command=\"") + command + "\"]", 0)
				  .at("elements")
				  .size() == 1);
	const Json gm = app.call("project.get").at("drumMaps").at(0).at("id");
	app.call("drumMap.set", {{"map", gm}, {"sounds", {{{"pitch", 36}, {"oNote", 24}}}}});
	app.call("track.set", {{"track", track}, {"drumMap", gm}});
	app.call("note.add",
		{{"part", part}, {"notes", {{{"position", 0}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}}}}});
	app.keys().run("midi.convertONotes", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil(
		[&] { return app.call("project.get").at("midiParts").at(0).at("notes").at(0).at("pitch") == 24; }));
	app.call("track.set", {{"track", track}, {"drumMap", 0}});
	app.keys().run("editor.openDrum", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	CHECK(app.call("editor.get", {{"editor", "window"}}).at("content") == "drum");
	const ui::WindowId window = app.appUi().editorWindowId();
	// Without a drum map the list names the pitches only.
	REQUIRE(app.waitUntil([&] { return app.ui().dom(window, ".drum-sound", 0).at("elements").size() == 128; }));
	CHECK(app.ui().dom(window, ".drum-sound-name", 0).at("elements").empty());
}

TEST_CASE("the drum sound list types an output, reverses with Pitch Visibility and auditions sounds", "[ui]")
{
	Fixture f("instrument");
	f.scroll(36);
	f.type(sound(36) + " textdisplay[sound-field=output]", "Module", "Module");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(36).at("output") == "Module"; }));
	f.type(sound(36) + " textdisplay[sound-field=output]", "Default", "Default");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(36).at("output").get<std::string>().empty(); }));

	// Pitch Visibility's Reverse Drum Sound List (pp. 1223-1224, 1234): the list from the bottom.
	CHECK(f.elements(".drum-sound").at(0).at("attributes").at("pitch") == "0");
	f.click(of("#drum-pitch-options"));
	f.click(of("#drum-pitch-menu menuitem[setting=reverseSounds]"));
	REQUIRE(
		f.app.waitUntil([&] { return f.app.call("editor.get", {{"editor", "window"}}).at("reverseSounds") == true; }));
	CHECK(f.elements(".drum-sound").at(0).at("attributes").at("pitch") == "0"); // Pitch Visibility is off
	f.click(of(".key-input[setting=pitchVisibility]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-sound").at(0).at("attributes").at("pitch") == "127"; }));

	// A click on a sound's pitch plays it as its I-note comes in: the O-note on the sound's channel (p. 1236).
	f.app.call("drumMap.set",
		{{"map", f.map},
			{"sounds",
				{{{"pitch", 36}, {"iNote", 45}, {"oNote", 35}, {"channel", 3}}, {{"pitch", 45}, {"iNote", 100}}}}});
	f.app.call("audio.setDevice", {{"blockSize", 128}, {"inputChannels", 0}, {"captureSeconds", 1}});
	f.app.call("channel.set",
		{{"channel", f.app.call("project.get").at("tracks").at(0).at("channel")}, {"instrument", "noteProbe"}});
	f.app.call("audio.render", {{"frames", 128}}); // the new graph
	f.app.call("midi.capture", {{"start", true}});
	f.scroll(36);
	f.click(of(sound(36) + " .drum-sound-pitch"));
	std::vector<std::vector<int>> played;
	CHECK(f.app.waitUntil(
		[&]
		{
			f.app.call("audio.render", {{"frames", 128}});
			const Json captured = f.app.call("midi.capture");
			INFO(captured.dump());
			for (const Json& message : captured.at("captured"))
			{
				const auto bytes = message.at("bytes").get<std::vector<int>>();
				if ((bytes.at(0) & 0xE0) == 0x80)
					played.push_back({bytes.at(0), bytes.at(1)});
			}
			return played == std::vector<std::vector<int>>{{0x93, 35}, {0x83, 35}};
		}));
	// A sound whose I-note an earlier sound has too plays itself, not the earlier one.
	f.app.call("drumMap.set", {{"map", f.map}, {"sounds", Json::array({{{"pitch", 47}, {"iNote", 45}}})}});
	f.app.call("audio.render", {{"frames", 128}});
	f.app.call("midi.capture", {{"start", true}});
	played.clear();
	f.scroll(47);
	f.click(of(sound(47) + " .drum-sound-pitch"));
	CHECK(f.app.waitUntil(
		[&]
		{
			f.app.call("audio.render", {{"frames", 128}});
			const Json captured = f.app.call("midi.capture");
			for (const Json& message : captured.at("captured"))
			{
				const auto bytes = message.at("bytes").get<std::vector<int>>();
				if ((bytes.at(0) & 0xE0) == 0x80)
					played.push_back({bytes.at(0), bytes.at(1)});
			}
			return played == std::vector<std::vector<int>>{{0x99, 47}, {0x89, 47}};
		}));
}

TEST_CASE("Show Note Length shows boxes, and the drumstick deletes a sound's notes with a drag", "[ui]")
{
	Fixture f;
	f.app.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", g_sixteenth}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 2 * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 38}, {"velocity", 0.5}},
					{{"position", 3 * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 8 * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 5; }));
	f.click(of(".key-input[setting=showNoteLength]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").empty() && f.elements(".key-note").size() == 5; }));
	f.click(of(".key-input[setting=showNoteLength]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 5; }));

	// From the first note to the fourth sixteenth: the bass drum's notes there go, the snare's and the later one stay.
	f.scroll(37);
	f.click(of(".key-tool[key-tool=draw]"));
	f.app.ui().drag(f.window, f.at(0.0, 36), f.at(3.5, 36));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 2; }));
	CHECK(f.partNotes()[0].at("pitch") == 38);
	CHECK(f.partNotes()[1].at("position") == 8 * g_sixteenth);
	f.app.call("history.undo"); // one undo step
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 5; }));
}

TEST_CASE("the Drum Map Setup lists the maps, renames them and changes their sounds", "[ui]")
{
	Fixture f;
	f.click(of("#drum-map"));
	f.click(of("#drum-map-menu menuitem[drum-map-action=setup]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#drum-maps listitem").size() == 2; })); // GM Map, Kit
	// The editor's track's map shows.
	REQUIRE(f.app.waitUntil([&] { return f.value("#drum-map-name") == "Kit"; }));
	f.write("#drum-map-name", "Rock Kit");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("name") == "Rock Kit"; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#drum-maps listitem").at(1).value("text", "") == "Rock Kit"; }));

	// A sound at the top of the list, where it shows without scrolling.
	const std::string kick = ".sound-row[pitch=\"2\"] ";
	f.write(kick + "input[key=oNote]", "C2");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(2).at("oNote") == 48; }));
	f.write(kick + "input[key=iNote]", "40");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(2).at("iNote") == 40; }));
	f.write(kick + "input[key=channel]", "Any");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(2).at("channel").is_null(); }));
	f.write(kick + "input[key=channel]", "3");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(2).at("channel") == 2; }));
	f.write(kick + "input[key=output]", "Module");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(2).at("output") == "Module"; }));
	f.write(kick + "input[key=name]", "Kick");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(2).at("name") == "Kick"; }));
	f.write(kick + "input[key=snap]", "1/8");
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(2).at("snap") == "1/8"; }));
	f.click(of(kick + "input[key=mute]"));
	REQUIRE(f.app.waitUntil([&] { return f.drumMap().at("sounds").at(2).at("mute") == true; }));
	// A snap that isn't a note value: the engine's message, and the field as it was.
	f.write(kick + "input[key=snap]", "1/7");
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#drum-maps-message").at(0).value("text", "").empty(); }));
	CHECK(f.value(kick + "input[key=snap]") == "1/8");

	// New Map adds a map and shows it; Remove removes it.
	f.click(of("#drum-map-new"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("drumMaps").size() == 3; }));
	REQUIRE(f.app.waitUntil([&] { return f.value("#drum-map-name") == "Empty Map"; }));
	f.click(of("#drum-map-remove"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("drumMaps").size() == 2; }));
	// A map added elsewhere shows.
	f.app.call("drumMap.add", {{"name", "From Lua"}});
	REQUIRE(f.app.waitUntil([&] { return f.elements("#drum-maps listitem").size() == 3; }));
}

TEST_CASE("with Show Note Length the object selection tool and the drumstick resize notes at their ends", "[ui]")
{
	Fixture f;
	f.app.call("note.add",
		{{"part", f.part}, {"notes", {{{"position", 0}, {"length", g_quarter}, {"pitch", 36}, {"velocity", 0.5}}}}});
	f.click(of(".key-input[setting=showNoteLength]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").empty() && f.elements(".key-note").size() == 1; }));
	f.scroll(36);
	const auto end = [&] { return f.inside(".key-note", f.box(".key-note").at("width").get<double>() - 2.0); };
	// The object selection tool drags the end to the sixth sixteenth (p. 1242).
	f.click(of(".key-note"));
	f.resize(end(), f.at(6.0, 36));
	CHECK(f.partNotes()[0].at("length") == 6 * g_sixteenth);
	// The drumstick changes the length at the note's ends too, and still deletes the note in its middle.
	f.click(of(".key-tool[key-tool=draw]"));
	f.resize(end(), f.at(8.0, 36));
	CHECK(f.partNotes()[0].at("length") == 8 * g_sixteenth);
	CHECK(f.partNotes()[0].at("position") == 0);
	f.resize(f.inside(".key-note", 2.0), f.at(2.0, 36));
	CHECK(f.partNotes()[0].at("position") == 2 * g_sixteenth);
	CHECK(f.partNotes()[0].at("length") == 6 * g_sixteenth);
	f.click(f.at(4.0, 36));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().empty(); }));
}

TEST_CASE("the drumstick's click deletes only the note clicked, and a drag also when released outside", "[ui]")
{
	Fixture f;
	// A double and a close flam of the bass drum, and two more notes.
	f.app.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 0}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 10}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 4 * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 8 * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 5; }));
	f.scroll(36);
	f.click(of(".key-tool[key-tool=draw]"));
	// A click without a drag, right of the note's start, deletes only that note (p. 1242).
	const double width = f.box(".drum-note" + f.noteSelector(0)).at("width").get<double>();
	f.click(f.inside(".drum-note" + f.noteSelector(0), (width / 2.0) + 4.0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 4; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 4; }));
	// A drag from a note to the right that ends above the editor deletes the sound's notes up to there.
	const ui::Point later = f.at(9.0, 36).point.value_or(ui::Point{});
	const ui::Target outside{"", 0, ui::Point{later.x, f.box(".key-editor").at("y").get<int>() - 4}};
	f.app.ui().drag(f.window, f.at(0.0, 36), outside);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().empty(); }));
}

TEST_CASE("the drumstick's drag to the left deletes only the note clicked, not its doubles", "[ui]")
{
	Fixture f;
	f.app.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", 0}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 4 * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}},
					{{"position", 4 * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 36}, {"velocity", 0.5}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 3; }));
	f.scroll(36);
	f.click(of(".key-tool[key-tool=draw]"));
	// Only a drag to the right deletes the sound's later notes (p. 1242); to the left it is a click.
	f.app.ui().drag(f.window, f.at(4.0, 36), f.at(1.0, 36));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 2; }));
	CHECK(f.partNotes()[0].at("position") == 0);
	CHECK(f.partNotes()[1].at("position") == 4 * g_sixteenth);
}

TEST_CASE("the drumstick's drag changes the velocity, with Alt the pitch, with Shift the position", "[ui]")
{
	// Modifying note values while inserting notes (p. 1241).
	Fixture f;
	f.scroll(36);
	f.click(of(".key-tool[key-tool=draw]"));
	const double velocity = f.app.call("editor.get", {{"editor", "window"}}).at("insertVelocity").get<double>();
	const auto up = [&](const ui::Target& target, int dp)
	{
		ui::Target moved = target;
		moved.point->y -= dp;
		return moved;
	};
	// Up: a louder note, one step of 127 per dp, still one note of its sound.
	f.app.ui().drag(f.window, f.at(0.3, 36), up(f.at(0.3, 36), 20));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 1; }));
	CHECK(f.partNotes()[0].at("pitch") == 36);
	CHECK(std::lround(f.partNotes()[0].at("velocity").get<double>() * 127) == std::lround(velocity * 127) + 20);
	// With Alt down a row: the next sound in the list, at the insert velocity.
	f.app.ui().drag(f.window, f.at(4.3, 36), f.at(4.3, 37), {"alt"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 2; }));
	CHECK(f.partNotes()[1].at("pitch") == 37);
	CHECK(f.partNotes()[1].at("position") == 4 * g_sixteenth);
	CHECK(std::lround(f.partNotes()[1].at("velocity").get<double>() * 127) == std::lround(velocity * 127));
	// With Shift to the right: one note, where the mouse is let go, not a row of them.
	f.app.ui().drag(f.window, f.at(8.3, 36), f.at(11.3, 36), {"shift"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 3; }));
	CHECK(f.partNotes()[2].at("position") == 11 * g_sixteenth);
	CHECK(f.partNotes()[2].at("pitch") == 36);
	// With Snap off (Ctrl/Cmd) and Show Note Length to the right: one longer note.
	f.click(of(".key-input[setting=showNoteLength]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").empty() && f.elements(".key-note").size() == 3; }));
	f.app.ui().drag(f.window, f.at(12.3, 36), f.at(15.3, 36), {"ctrl"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 4; }));
	CHECK(f.partNotes()[3].at("length").get<int64_t>() > 2 * g_sixteenth);
}

TEST_CASE("the object selection tool's double click and drag inserts and deletes several notes of a sound", "[ui]")
{
	Fixture f;
	f.scroll(36);
	// Double click in the empty grid and drag to the right: a note at every step of the grid (p. 1241).
	f.doubleClickDrag(f.at(0.2, 36), f.at(2.0, 36), f.at(3.5, 36));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 4; }));
	CHECK(f.partNotes()[3].at("position") == 3 * g_sixteenth);
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 4; }));
	// Double click the second note and drag to the right: it and the later ones go, in one undo step (p. 1242).
	f.doubleClickDrag(f.at(1.0, 36), f.at(2.0, 36), f.at(3.2, 36));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 1; }));
	CHECK(f.partNotes()[0].at("position") == 0);
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 4; }));
	// A double click without a drag deletes the note alone.
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 4; }));
	f.click(f.at(2.0, 36), 2);
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 3; }));
}

TEST_CASE("the Drum Map Setup saves a map to a file and loads it", "[ui]")
{
	Fixture f;
	f.click(of("#drum-map"));
	f.click(of("#drum-map-menu menuitem[drum-map-action=setup]"));
	REQUIRE(f.app.waitUntil([&] { return f.value("#drum-map-name") == "Kit"; }));
	f.app.call("drumMap.set", {{"map", f.map}, {"sounds", {{{"pitch", 36}, {"oNote", 24}}}}});
	// Save... writes the map shown, with the extension the user left out.
	const test::TempDir dir;
	f.click(of("#drum-map-save"));
	f.answer(dir.path() / "Kit");
	const std::filesystem::path file = dir.path() / "Kit.drm";
	REQUIRE(f.app.waitUntil([&file] { return std::filesystem::exists(file); }));
	// Load... adds it to the project as a new map, which the setup lists.
	f.click(of("#drum-map-load"));
	f.answer(file);
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("drumMaps").size() == 3; }));
	const Json loaded = f.app.call("project.get").at("drumMaps").at(2);
	CHECK(loaded.at("name") == "Kit");
	CHECK(loaded.at("sounds").at(36).at("oNote") == 24);
	REQUIRE(f.app.waitUntil([&] { return f.elements("#drum-maps listitem").size() == 3; }));
}
