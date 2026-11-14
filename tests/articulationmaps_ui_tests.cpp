// SPDX-License-Identifier: GPL-3.0-or-later
// The articulation map setup and the inspector's Articulation Map section in the app (#175,
// docs/features/articulation-maps.md), and the Articulations lane, the info line's Articulations field and the list
// editor's directions and attributes (#1112), driven with ui::Automation as MCP's UI tools drive them.
// tests/articulationmaps_tests.cpp checks the maps in the engine.
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	// The project window with the inspector, showing an instrument track.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			app.setDpRatio(1.0F);
			app.appUi().docking().open("inspector");
			track = app.call("track.add", {{"type", "instrument"}, {"name", "Strings"}})
						.at("track")
						.at("id")
						.get<uint64_t>();
			REQUIRE(app.waitUntil(
				[this]
				{
					return display.project().is_object() &&
						display.project().value("tracks", Json::array()).size() == 1;
				}));
			app.appUi().trackList()->select({track});
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		bool shown(const std::string& selector)
		{
			const Json found = elements(selector);
			return !found.empty() && found[0].at("visible").get<bool>();
		}

		void click(const std::string& selector)
		{
			INFO(selector);
			REQUIRE(app.waitUntil([&] { return shown(selector); }));
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Types into a text field and presses Enter.
		void write(const std::string& selector, const std::string& value)
		{
			click(selector);
			app.ui().key(std::nullopt, "Mod+A");
			app.ui().text(std::nullopt, value);
			app.ui().key(std::nullopt, "Enter");
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json maps() { return app.call("project.get").at("articulationMaps"); }

		// The names the setup's list shows.
		std::vector<std::string> listed()
		{
			std::vector<std::string> names;
			for (const Json& item : elements("#articulation-maps listitem"))
				names.push_back(item.value("text", std::string()));
			return names;
		}

		// Answers the OS's file dialog that is open with a file.
		void answer(const std::filesystem::path& file)
		{
			REQUIRE(app.waitUntil([this] { return app.shell().fileDialogs().size() == 1; }));
			app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {file});
		}

		std::string message()
		{
			const Json found = elements("#articulation-maps-message");
			return found.empty() ? std::string() : found[0].value("text", std::string());
		}

		uint64_t trackMap()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("tracks"))
				if (each.at("id") == track)
					return each.value("articulationMap", uint64_t{0});
			return 0;
		}

		test::UiApp app{{1200, 800}};
		ui::EventDisplay& display;
		uint64_t track = 0;
	};
}

TEST_CASE("the articulation map setup adds and edits maps, and the inspector assigns them", "[ui]")
{
	Fixture f;
	// The inspector's section opens the setup for the track; the sections above it closed, so that it is in view.
	for (const char* section : {"basics", "routing", "modifiers"})
		f.click(".inspector [section=" + std::string(section) + "] .section-header");
	f.click(".inspector .inspector-articulation-setup");
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
	f.app.shell().step(std::chrono::milliseconds(0)); // its Lua fills it when it shows

	// Add Map, renamed in its name field.
	f.click("#articulation-map-add");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().size() == 1; }));
	f.write("#articulation-map input[kind=map][key=name]", "Strings");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().at(0).at("name") == "Strings"; }));

	// An articulation and a sound slot with it, triggered by key 37 and switching with the key switch 24.
	f.click("#articulation-add");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().at(0).at("articulations").size() == 1; }));
	f.write("#articulation-map input[kind=articulation][key=name]", "Pizzicato");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().at(0).at("articulations").at(0).at("name") == "Pizzicato"; }));
	f.click("#articulation-slot-add");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().at(0).at("slots").size() == 1; }));
	f.write("#articulation-map input[kind=slot][key=articulations]", "Pizzicato");
	f.write("#articulation-map input[kind=slot][key=trigger]", "37");
	REQUIRE(f.app.waitUntil(
		[&f]
		{
			const Json slot = f.maps().at(0).at("slots").at(0);
			return slot.at("articulations") == Json{"Pizzicato"} && slot.at("trigger") == 37;
		}));
	f.click("#articulation-output-add");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().at(0).at("slots").at(0).at("outputs").size() == 1; }));
	f.write("#articulation-map input[kind=output][key=data1]", "24");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().at(0).at("slots").at(0).at("outputs").at(0).at("data1") == 24; }));
	f.write("#articulation-map input[kind=modifier][key=transpose]", "12");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().at(0).at("slots").at(0).at("transpose") == 12; }));

	// What the engine refuses shows its message: the articulation isn't the map's.
	f.write("#articulation-map input[kind=slot][key=articulations]", "Legato");
	REQUIRE(f.app.waitUntil(
		[&f]
		{
			const Json message = f.elements("#articulation-maps-message");
			return !message.empty() && message[0].value("text", std::string()).find("Legato") != std::string::npos;
		}));
	CHECK(f.maps().at(0).at("slots").at(0).at("articulations") == Json{"Pizzicato"});

	// Assign to Track, then the inspector's selector takes it off again.
	f.click("#articulation-map-assign");
	const auto id = f.maps().at(0).at("id").get<uint64_t>();
	REQUIRE(f.app.waitUntil([&f, id] { return f.trackMap() == id; }));
	f.click("#articulation-maps-close");
	REQUIRE(f.app.waitUntil([&f] { return f.elements("#articulation-maps-dialog").empty(); }));
	f.click(".inspector select[control=articulationMap]");
	f.app.ui().key(std::nullopt, "Up");
	f.click(".inspector select[control=articulationMap]");
	REQUIRE(f.app.waitUntil([&f] { return f.trackMap() == 0; }));

	// The MIDI menu opens the setup too; Remove Map removes the map.
	f.click("#menu-bar button[menu=midi-menu]");
	f.click("menuitem[command=\"midi.articulationMapSetup\"]");
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.click("#articulation-map-remove");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().empty(); }));
}

TEST_CASE("the articulation map setup shows changes from elsewhere, and imports and exports maps", "[ui]")
{
	Fixture f;
	f.click("#menu-bar button[menu=midi-menu]");
	f.click("menuitem[command=\"midi.articulationMapSetup\"]");
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
	f.app.shell().step(std::chrono::milliseconds(0));

	// A map added and renamed through the engine API shows; an edit after that keeps the new name.
	const auto id = f.app.call("articulationMap.add", {{"name", "Brass"}}).at("map").at("id").get<uint64_t>();
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == std::vector<std::string>{"Brass"}; }));
	f.app.call("articulationMap.set", {{"map", id}, {"name", "Horns"}});
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == std::vector<std::string>{"Horns"}; }));
	f.click("#articulation-add");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().at(0).at("articulations").size() == 1; }));
	CHECK(f.maps().at(0).at("name") == "Horns");

	// Export Map writes the file the user chose; Import Map adds it as a new map, which the setup selects.
	const test::TempDir dir;
	f.click("#articulation-map-export");
	f.answer(dir.path() / "Horns");
	const std::filesystem::path file = dir.path() / "Horns.articulationmap";
	REQUIRE(f.app.waitUntil([&file] { return std::filesystem::exists(file); }));
	f.app.call("articulationMap.set", {{"map", id}, {"name", "Strings"}});
	f.click("#articulation-map-import");
	f.answer(file);
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == std::vector<std::string>{"Strings", "Horns"}; }));
	CHECK(f.maps().at(1).at("articulations").size() == 1);
	REQUIRE(f.app.waitUntil(
		[&f]
		{
			const Json selected = f.elements("#articulation-maps listitem.selected");
			return selected.size() == 1 && selected[0].value("text", std::string()) == "Horns";
		}));

	// An undo shows too; an import that fails shows why.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == std::vector<std::string>{"Strings"}; }));
	std::ofstream(dir.path() / "Broken.articulationmap") << "not a map";
	f.click("#articulation-map-import");
	f.answer(dir.path() / "Broken.articulationmap");
	REQUIRE(f.app.waitUntil([&f] { return !f.message().empty(); }));
	CHECK(f.listed() == std::vector<std::string>{"Strings"});

	// Export Map offers the map's name as the file name, without what file names can't have, and shows why an
	// export failed.
	f.app.call("articulationMap.set", {{"map", id}, {"name", "Strings/Pizz"}});
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == std::vector<std::string>{"Strings/Pizz"}; }));
	f.click("#articulation-map-export");
	REQUIRE(f.app.waitUntil([&f] { return f.app.shell().fileDialogs().size() == 1; }));
	CHECK(f.app.shell().fileDialogs().begin()->second.location.u8string() == "Strings_Pizz.articulationmap");
	f.answer(dir.path() / "missing" / "Strings");
	REQUIRE(f.app.waitUntil([&f] { return f.message().find("can't write") != std::string::npos; }));
}

TEST_CASE("opening the setup, the profile manager or the workspaces organizer again keeps one set of handlers", "[ui]")
{
	Fixture f;
	// How many gnubase.on handlers the UI's Lua has for an event: the count comes back as the message of a run that
	// fails on purpose, between two #.
	const auto handlers = [&f](const std::string& event)
	{
		const std::string error =
			f.app.lua()
				.run("error('#' .. #(debug.getregistry()['gnubase.handlers']['" + event + "'] or {}) .. '#', 0)",
					"count")
				.value_or("");
		const size_t from = error.find('#') + 1;
		REQUIRE(from > 0);
		return std::stoi(error.substr(from, error.find('#', from) - from));
	};
	// Each dialog: its UI command, its document, its close button, and an event its Lua follows.
	const std::vector<std::vector<std::string>> dialogs = {
		{"midi.articulationMapSetup", "#articulation-maps-dialog", "#articulation-maps-close", "articulationMap.added"},
		{"window.profiles", "#profiles-dialog", "#profile-close", "profile.changed"},
		{"window.workspaces", "#workspaces-dialog", "#workspaces-close", "workspace.changed"}};
	for (const std::vector<std::string>& dialog : dialogs)
	{
		INFO(dialog[0]);
		const int before = handlers(dialog[3]);
		for (int open = 0; open < 2; ++open)
		{
			f.app.keys().run(dialog[0], Json::object(), f.app.main(), nullptr);
			REQUIRE(f.app.waitUntil([&] { return f.shown(dialog[1]); }));
			f.app.shell().step(std::chrono::milliseconds(0));
			CHECK(f.elements(dialog[1]).size() == 1);
			CHECK(handlers(dialog[3]) == before + 1);
		}
		f.click(dialog[2]);
		REQUIRE(f.app.waitUntil([&] { return f.elements(dialog[1]).empty(); }));
		CHECK(handlers(dialog[3]) == before);
	}
}

TEST_CASE("the articulation map setup opened again for another track assigns to that track", "[ui]")
{
	Fixture f;
	// Track A, the fixture's, has the second of two maps; track B none.
	f.app.call("articulationMap.add", {{"name", "Brass"}});
	const auto second = f.app.call("articulationMap.add", {{"name", "Strings"}}).at("map").at("id").get<uint64_t>();
	f.app.call("track.set", {{"track", f.track}, {"articulationMap", second}});
	const auto other =
		f.app.call("track.add", {{"type", "instrument"}, {"name", "Winds"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(f.app.waitUntil([&f] { return f.display.project().value("tracks", Json::array()).size() == 2; }));
	// The setup for B, then for A, which selects A's map, then for B again, which keeps it: Assign to Track
	// assigns it to B.
	for (const uint64_t track : {other, f.track, other})
	{
		f.app.appUi().trackList()->select({track});
		f.app.shell().step(std::chrono::milliseconds(0));
		f.app.keys().run("midi.articulationMapSetup", Json::object(), f.app.main(), nullptr);
		REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
		f.app.shell().step(std::chrono::milliseconds(0));
	}
	CHECK(f.elements("#articulation-maps-dialog").size() == 1);
	f.click("#articulation-map-assign");
	REQUIRE(f.app.waitUntil(
		[&f, other, second]
		{
			const Json project = f.app.call("project.get");
			for (const Json& each : project.at("tracks"))
				if (each.at("id") == other)
					return each.value("articulationMap", uint64_t{0}) == second;
			return false;
		}));
	CHECK(f.trackMap() == second);
}

TEST_CASE("the articulation map setup opened again without a track can't assign, and for its track keeps the map shown",
	"[ui]")
{
	Fixture f;
	// The fixture's track has the second of two maps.
	f.app.call("articulationMap.add", {{"name", "Brass"}});
	const auto second = f.app.call("articulationMap.add", {{"name", "Strings"}}).at("map").at("id").get<uint64_t>();
	f.app.call("track.set", {{"track", f.track}, {"articulationMap", second}});
	const auto open = [&f]
	{
		f.app.keys().run("midi.articulationMapSetup", Json::object(), f.app.main(), nullptr);
		REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
		f.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto selected = [&f]
	{
		const Json item = f.elements("#articulation-maps listitem.selected");
		return item.size() == 1 ? item[0].value("text", std::string()) : std::string();
	};
	open();
	REQUIRE(f.app.waitUntil([&] { return selected() == "Strings"; }));
	// Browsed to the first map, then opened again for the same track: it stays.
	f.app.ui().click(std::nullopt, {"#articulation-maps listitem", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return selected() == "Brass"; }));
	open();
	CHECK(selected() == "Brass");
	CHECK(f.shown("#articulation-map-assign"));
	// Without a MIDI or instrument track selected there is no track to assign to, as when it opens fresh.
	f.app.appUi().trackList()->select({});
	f.app.shell().step(std::chrono::milliseconds(0));
	open();
	CHECK_FALSE(f.shown("#articulation-map-assign"));
	// The track again: its map, and Assign to Track.
	f.app.appUi().trackList()->select({f.track});
	f.app.shell().step(std::chrono::milliseconds(0));
	open();
	CHECK(selected() == "Strings");
	CHECK(f.shown("#articulation-map-assign"));
	CHECK(f.elements("#articulation-maps-dialog").size() == 1);
}

TEST_CASE("the articulation map setup opened without a track can't assign, and a new project drops its track", "[ui]")
{
	Fixture f;
	const auto open = [&f]
	{
		f.app.keys().run("midi.articulationMapSetup", Json::object(), f.app.main(), nullptr);
		REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
		f.app.shell().step(std::chrono::milliseconds(0));
	};
	// Opened fresh without a MIDI or instrument track selected: the map shows, but there is no track to assign it to.
	f.app.call("articulationMap.add", {{"name", "Brass"}});
	f.app.appUi().trackList()->select({});
	f.app.shell().step(std::chrono::milliseconds(0));
	open();
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-map"); }));
	CHECK_FALSE(f.shown("#articulation-map-assign"));
	f.click("#articulation-maps-close");
	REQUIRE(f.app.waitUntil([&f] { return f.elements("#articulation-maps-dialog").empty(); }));

	// Opened for the track, then a new project: object ids are per project, so the setup has no track any more.
	f.app.appUi().trackList()->select({f.track});
	f.app.shell().step(std::chrono::milliseconds(0));
	open();
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-map-assign"); }));
	CHECK(f.elements("body[track]").size() == 1);
	f.app.call("project.new");
	REQUIRE(f.app.waitUntil([&f] { return f.elements("body[track]").empty(); }));
	CHECK(f.elements("#articulation-map-assign.unavailable").size() == 1);

	// Opened again for a track of the new project: that track's setup, which assigns to it.
	const auto map = f.app.call("articulationMap.add", {{"name", "Piano"}}).at("map").at("id").get<uint64_t>();
	const auto track =
		f.app.call("track.add", {{"type", "midi"}, {"name", "Piano"}}).at("track").at("id").get<uint64_t>();
	// The old project has one track too, so wait for the new one by name, not by count.
	REQUIRE(f.app.waitUntil(
		[&f]
		{
			const Json tracks = f.display.project().value("tracks", Json::array());
			return tracks.size() == 1 && tracks.at(0).value("name", std::string()) == "Piano";
		}));
	f.app.appUi().trackList()->select({track});
	f.app.shell().step(std::chrono::milliseconds(0));
	open();
	f.click("#articulation-map-assign");
	f.track = track;
	REQUIRE(f.app.waitUntil([&f, map] { return f.trackMap() == map; }));
}

namespace
{
	constexpr int64_t g_sixteenth = time::g_ticksPerQuarter / 4;
	constexpr int64_t g_bar = 16 * g_sixteenth;

	// Elements of the Articulations lane.
	std::string inLane(const std::string& selector)
	{
		return ".controller-lane[lane-kind=articulations]" + selector;
	}

	ui::Target of(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	// An instrument track with the map Strings: Arco and Pizzicato directions of the group Technique, Staccato an
	// attribute of the group Length; the slots Arco (the default, blue), Pizzicato (red) and Staccato. Its one-bar part
	// has notes two sixteenths long at the first, fifth and ninth sixteenth and is open in the editor window, the key
	// editor with an Articulations lane, or the list editor.
	struct Editor
	{
		explicit Editor(const std::string& content = "key")
		{
			app.setDpRatio(1.0F);
			track = app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
			const Json map = app.call("articulationMap.add",
				{{"name", "Strings"}, {"groups", {"Technique", "Length"}},
					{"articulations",
						{{{"name", "Arco"}, {"type", "direction"}, {"group", 0}},
							{{"name", "Pizzicato"}, {"type", "direction"}, {"group", 0}},
							{{"name", "Staccato"}, {"type", "attribute"}, {"group", 1}}}},
					{"slots",
						{{{"name", "Arco"}, {"color", "#2040a0"}},
							{{"name", "Pizzicato"}, {"articulations", {"Pizzicato"}}, {"color", "#c04020"}},
							{{"name", "Staccato"}, {"articulations", {"Staccato"}}}}}});
			app.call("track.set", {{"track", track}, {"articulationMap", map.at("map").at("id")}});
			part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
			Json items = Json::array();
			for (const int at : {0, 4, 8})
				items.push_back(
					{{"position", at * g_sixteenth}, {"length", 2 * g_sixteenth}, {"pitch", 60}, {"velocity", 0.5}});
			const Json added = app.call("note.add", {{"part", part}, {"notes", items}});
			for (const Json& note : added.at("notes"))
				notes.push_back(note.at("id").get<uint64_t>());
			app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part})}, {"content", content}});
			REQUIRE(app.waitUntil([this] { return app.appUi().editorWindow() != nullptr; }));
			window = app.appUi().editorWindowId();
			if (content == "key")
			{
				app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "articulations"}}}}});
				REQUIRE(app.waitUntil([this] { return elements(inLane(" .articulation-row")).size() == 4; }));
			}
		}

		Json elements(const std::string& selector) { return app.ui().dom(window, selector, 0).at("elements"); }

		Json box(const std::string& selector) { return elements(selector).at(0).at("box"); }

		std::string value(const std::string& selector)
		{
			const Json found = elements(selector);
			return found.empty() ? std::string() : found.at(0).at("attributes").value("value", std::string());
		}

		// A point of a row of the lane, the sound slots lane 0 and the articulations from 1, at sixteenths from the
		// part's start.
		ui::Target at(size_t row, double sixteenths)
		{
			const Json area = box(inLane(" .controller-lane-area"));
			const Json shown = elements(inLane(" .articulation-row")).at(row).at("box");
			const double x = area.at("x").get<double>() + (sixteenths / 16.0 * area.at("width").get<double>());
			const double y = shown.at("y").get<double>() + (shown.at("height").get<double>() / 2.0);
			return {"", 0, ui::Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}};
		}

		void click(const ui::Target& target, int count = 1)
		{
			app.ui().click(window, target, ui::MouseButton::Left, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		void tool(const std::string& name) { click(of(".key-tool[key-tool=" + name + "]")); }

		// Types into a text display and presses Enter.
		void type(const std::string& selector, const std::string& text)
		{
			click(of(selector), 2);
			app.ui().key(window, "Mod+A");
			if (text.empty())
				app.ui().key(window, "Backspace");
			else
				app.ui().text(window, text);
			app.ui().key(window, "Enter");
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json midiPart() { return app.call("project.get").at("midiParts").at(0); }

		Json note(uint64_t id)
		{
			const Json part = midiPart();
			for (const Json& each : part.at("notes"))
				if (each.at("id") == id)
					return each;
			return nullptr;
		}

		Json articulations(uint64_t id) { return note(id).value("articulations", Json::array()); }

		// The slot the sound slots lane shows for a note.
		std::string slot(uint64_t id)
		{
			const Json found = elements(inLane(" .sound-slot[note=\"" + std::to_string(id) + "\"]"));
			return found.empty() ? std::string() : found.at(0).at("attributes").value("title", std::string());
		}

		test::UiApp app{{1200, 800}};
		ui::WindowId window{};
		uint64_t track = 0;
		uint64_t part = 0;
		std::vector<uint64_t> notes;
	};
}

TEST_CASE(
	"the Articulations lane shows the sound slots, directions and attributes, which its tools enter and remove", "[ui]")
{
	// Reference manual pp. 1287-1289.
	Editor f;
	// The articulations' names beside their rows.
	const Json names = f.elements(inLane(" .articulation-name"));
	REQUIRE(names.size() == 3);
	CHECK(names.at(1).value("text", std::string()) == "Pizzicato");
	// Each note plays the default slot.
	REQUIRE(f.app.waitUntil([&f] { return f.slot(f.notes[2]) == "Arco"; }));
	CHECK(f.slot(f.notes[0]) == "Arco");

	// The draw tool adds a direction in the Pizzicato row where it clicks, on the quantize grid: it lasts to the
	// part's end, and the notes from there play its slot, in the slot's color.
	f.tool("draw");
	f.click(f.at(2, 4.2));
	REQUIRE(f.app.waitUntil([&f] { return f.midiPart().at("controllers").size() == 1; }));
	const Json direction = f.midiPart().at("controllers").at(0);
	CHECK(direction.at("type") == "articulation");
	CHECK(direction.at("articulation") == "Pizzicato");
	CHECK(direction.at("position") == 4 * g_sixteenth);
	REQUIRE(f.app.waitUntil([&f] { return f.slot(f.notes[1]) == "Pizzicato"; }));
	CHECK(f.slot(f.notes[0]) == "Arco");
	CHECK(f.slot(f.notes[2]) == "Pizzicato");
	const Json area = f.box(inLane(" .controller-lane-area"));
	const Json bar = f.box(inLane(" .articulation-item.direction"));
	CHECK(std::abs(bar.at("width").get<double>() - (area.at("width").get<double>() * 12.0 / 16.0)) <= 2.0);
	const ui::Image shown =
		f.app.ui().screenshot(f.window, of(inLane(" .sound-slot[note=\"" + std::to_string(f.notes[1]) + "\"]")));
	REQUIRE(shown.width > 4);
	const std::array<uint8_t, 4> pixel = shown.pixel(shown.width - 3, shown.height / 2);
	CHECK((pixel[0] == 0xc0 && pixel[1] == 0x40 && pixel[2] == 0x20));

	// An attribute goes on the note under the click; a click on a direction with the draw tool removes it.
	f.click(f.at(3, 9.0));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[2]) == Json{"Staccato"}; }));
	CHECK(f.articulations(f.notes[1]).empty());
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.attribute")).size() == 1; }));
	f.click(f.at(2, 12.0));
	REQUIRE(f.app.waitUntil([&f] { return f.midiPart().at("controllers").empty(); }));
	REQUIRE(f.app.waitUntil([&f] { return f.slot(f.notes[1]) == "Arco"; }));
	CHECK(f.slot(f.notes[2]) == "Staccato");

	// The erase tool removes the attribute.
	f.tool("erase");
	f.click(f.at(3, 9.0));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[2]).empty(); }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item")).empty(); }));
}

TEST_CASE("a reset shows on the Articulations lane with a width of its own, and the drum editor has the lane", "[ui]")
{
	// Reference manual pp. 1288, 1290.
	Editor f;
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}},
					{{"position", 8 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"},
						{"reset", true}}}}});
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.reset")).size() == 1; }));
	const Json area = f.box(inLane(" .controller-lane-area"));
	const Json directions = f.elements(inLane(" .articulation-item.direction"));
	REQUIRE(directions.size() == 2);
	// The direction lasts until the reset, which doesn't fill the rest of the part.
	CHECK(
		std::abs(directions.at(0).at("box").at("width").get<double>() - (area.at("width").get<double>() / 2.0)) <= 2.0);
	CHECK(directions.at(1).at("box").at("width").get<double>() < area.at("width").get<double>() / 16.0);
	CHECK(f.slot(f.notes[1]) == "Pizzicato");
	CHECK(f.slot(f.notes[2]) == "Arco");

	f.app.call("editor.set", {{"editor", "window"}, {"content", "drum"}});
	REQUIRE(f.app.waitUntil(
		[&f] { return f.elements(".drum-editor .controller-lane[lane-kind=articulations]").size() == 1; }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.reset")).size() == 1; }));
}

TEST_CASE("the key editor's info line shows and sets the selected notes' attributes", "[ui]")
{
	// Reference manual p. 1290.
	Editor f;
	f.app.call("note.set", {{"part", f.part}, {"notes", {{{"id", f.notes[0]}, {"articulations", {"Staccato"}}}}}});
	const auto select = [&f](size_t first, size_t second)
	{
		f.app.call("editor.set",
			{{"editor", "window"},
				{"selection",
					{{{"part", f.part}, {"note", f.notes.at(first)}},
						{{"part", f.part}, {"note", f.notes.at(second)}}}}});
	};
	const std::string field = ".key-editor-info textdisplay[field=articulations]";
	select(0, 1);
	REQUIRE(f.app.waitUntil([&] { return f.value(field) == "Staccato"; }));
	// Typed, the attributes go to every selected note; emptied, they go.
	select(1, 2);
	REQUIRE(f.app.waitUntil([&] { return f.value(field).empty(); }));
	f.type(field, "Staccato");
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[2]) == Json{"Staccato"}; }));
	CHECK(f.articulations(f.notes[1]) == Json{"Staccato"});
	REQUIRE(f.app.waitUntil([&] { return f.value(field) == "Staccato"; }));
	f.type(field, "");
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[2]).empty(); }));
	CHECK(f.articulations(f.notes[0]) == Json{"Staccato"});
	CHECK(f.articulations(f.notes[1]).empty());
}

TEST_CASE("the list editor lists directions as Articulation events and edits the notes' attributes", "[ui]")
{
	// Reference manual p. 1291.
	Editor f("list");
	const Json added = f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 4 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	const auto direction = added.at("controllers").at(0).at("id").get<uint64_t>();
	f.app.call("note.set", {{"part", f.part}, {"notes", {{{"id", f.notes[2]}, {"articulations", {"Staccato"}}}}}});
	const std::string row = ".list-editor-rows .list-row[controller=\"" + std::to_string(direction) + "\"]";
	const auto noteRow = [](uint64_t id)
	{ return ".list-editor-rows .list-row[note=\"" + std::to_string(id) + "\"] textdisplay[field=attributes]"; };
	REQUIRE(f.app.waitUntil([&] { return f.value(row + " textdisplay[field=attributes]") == "Pizzicato"; }));
	CHECK(f.elements(row + " .list-type").at(0).value("text", std::string()) == "Articulation");
	// A direction has no channel.
	CHECK(f.value(row + " textdisplay[field=channel]").empty());
	CHECK(f.elements(row + " textdisplay[field=channel]").at(0).at("attributes").contains("disabled"));
	REQUIRE(f.app.waitUntil([&] { return f.value(noteRow(f.notes[2])) == "Staccato"; }));

	// The direction's name and a note's attributes typed in the Attributes column.
	f.type(row + " textdisplay[field=attributes]", "Arco");
	REQUIRE(f.app.waitUntil([&f] { return f.midiPart().at("controllers").at(0).at("articulation") == "Arco"; }));
	f.type(noteRow(f.notes[0]), "Staccato");
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[0]) == Json{"Staccato"}; }));
	f.type(noteRow(f.notes[2]), "");
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[2]).empty(); }));
}

TEST_CASE("the Articulations lane drops a cleared slot color and width, and its rows line up with their names", "[ui]")
{
	// #1498: the lane keeps its elements while their RML is the same, so what code set on them must go again.
	Editor f;
	const std::string first = inLane(" .sound-slot[note=\"" + std::to_string(f.notes[0]) + "\"]");
	const auto has = [&f](const std::string& selector, const std::string& name)
	{
		const Json found = f.elements(selector);
		const Json names = found.empty() ? Json::array() : found.at(0).value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	// The themes write slot names in a dark color on a light slot color and in a light one on a dark slot color.
	REQUIRE(f.app.waitUntil([&] { return has(first, "dark"); }));
	CHECK_FALSE(has(first, "light"));
	const auto arco = [&f](const std::string& color)
	{
		Json map = f.app.call("project.get").at("articulationMaps").at(0);
		map["slots"][0]["color"] = color;
		f.app.call("articulationMap.set", {{"map", map.at("id")}, {"slots", map.at("slots")}});
	};
	arco("#f0e0a0");
	REQUIRE(f.app.waitUntil([&] { return has(first, "light"); }));
	CHECK_FALSE(has(first, "dark"));
	// Cleared, the slot has the theme's look again, not the color it had.
	arco("");
	REQUIRE(f.app.waitUntil([&] { return !has(first, "light") && !has(first, "dark"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	const ui::Image shown = f.app.ui().screenshot(f.window, of(first));
	REQUIRE(shown.width > 4);
	const std::array<uint8_t, 4> pixel = shown.pixel(shown.width - 3, shown.height / 2);
	CHECK_FALSE((pixel[0] == 0xf0 && pixel[1] == 0xe0 && pixel[2] == 0xa0));

	// A direction that another one of its group now starts with has no width of its own left. Both are after the last
	// note's start, so the notes' slots and the lane's RML stay the same and the Pizzicato element is kept.
	const Json added = f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 10 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}},
					{{"position", 12 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Arco"}}}}});
	const std::string pizzicato = inLane(" .articulation-row[articulation-row=\"1\"] .articulation-item");
	const double area = f.box(inLane(" .controller-lane-area")).at("width").get<double>();
	REQUIRE(
		f.app.waitUntil([&] { return std::abs(f.box(pizzicato).at("width").get<double>() - (area / 8.0)) <= 2.0; }));
	f.app.call("controller.set",
		{{"part", f.part},
			{"controllers", {{{"id", added.at("controllers").at(1).at("id")}, {"position", 10 * g_sixteenth}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.box(pizzicato).at("width").get<double>() < area / 16.0; }));

	// The line under the sound slots lane is inside its height: each articulation's name is beside its row.
	const Json names = f.elements(inLane(" .articulation-name"));
	const Json rows = f.elements(inLane(" .articulation-row"));
	REQUIRE(rows.size() == names.size() + 1);
	for (size_t i = 0; i < names.size(); ++i)
		CHECK(std::abs(names.at(i).at("box").at("y").get<double>() - rows.at(i + 1).at("box").at("y").get<double>()) <
			0.5);
}

TEST_CASE("the Articulations lane adds no second direction where one starts, and selects its directions", "[ui]")
{
	// #1498.
	Editor f;
	f.tool("draw");
	f.click(f.at(2, 4.2));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.direction")).size() == 1; }));
	// Left of the bar, the click snaps to where it starts: nothing is added there. A click in the Arco row is.
	f.click(f.at(2, 3.8));
	f.click(f.at(1, 8.0));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.direction")).size() == 2; }));
	CHECK(f.midiPart().at("controllers").size() == 2);

	// Select All Events on Lane selects the lane's directions.
	f.click(of(inLane(" .controller-lane-type")));
	const std::string item = "#controller-lane-menu menuitem[lane-action=selectAll]";
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json found = f.elements(item);
			return !found.empty() && found.at(0).at("visible") == true;
		}));
	f.click(of(item));
	REQUIRE(f.app.waitUntil(
		[&f] { return f.app.call("editor.get", {{"editor", "window"}}).at("controllerSelection").size() == 2; }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.selected")).size() == 2; }));
}

TEST_CASE("the Articulations lane shows and edits only the parts whose track has its map", "[ui]")
{
	// #1498: a second track without a map, its part over the first one's, with notes at the same places.
	Editor f;
	const uint64_t other = f.app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
	const uint64_t part = f.app.call("midiPart.add", {{"track", other}, {"position", 0}, {"length", g_bar}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
	Json items = Json::array();
	for (const int at : {0, 4, 8})
		items.push_back(
			{{"position", at * g_sixteenth}, {"length", 2 * g_sixteenth}, {"pitch", 64}, {"velocity", 0.5}});
	f.app.call("note.add", {{"part", part}, {"notes", items}});
	f.app.call("controller.add",
		{{"part", part},
			{"controllers", {{{"position", 0}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	// The other part is the active one: the lane shows the first track's map, and only its part.
	f.app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part, part})}});
	f.app.call("editor.set", {{"editor", "window"}, {"activePart", part}, {"lanes", {{{"type", "articulations"}}}}});
	REQUIRE(f.app.waitUntil([&f] { return f.app.call("editor.get", {{"editor", "window"}}).at("parts").size() == 2; }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .sound-slot")).size() == 3; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements(inLane(" .articulation-item")).empty());
	const auto partOf = [](const Json& shown)
	{ return std::stoull(shown.at("attributes").at("part").get<std::string>()); };
	for (const Json& shown : f.elements(inLane(" .sound-slot")))
		CHECK(partOf(shown) == f.part);

	// The draw tool adds a direction and an attribute to the part of the map's track only.
	f.tool("draw");
	f.click(f.at(2, 4.0));
	REQUIRE(f.app.waitUntil([&f] { return f.midiPart().at("controllers").size() == 1; }));
	f.click(f.at(3, 9.0));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[2]) == Json{"Staccato"}; }));
	const Json project = f.app.call("project.get");
	for (const Json& each : project.at("midiParts"))
		if (each.at("id") == part)
		{
			CHECK(each.at("controllers").size() == 1);
			for (const Json& note : each.at("notes"))
				CHECK(note.value("articulations", Json::array()).empty());
		}
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item")).size() == 2; }));
	for (const Json& shown : f.elements(inLane(" .articulation-item")))
		CHECK(partOf(shown) == f.part);
}

TEST_CASE("the in-place editor has the Articulations lane", "[ui]")
{
	// Reference manual p. 1288: the key, drum and in-place editors have it.
	test::UiApp app{{1200, 800}};
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	const uint64_t track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const Json map = app.call("articulationMap.add",
		{{"name", "Strings"}, {"groups", {"Technique"}},
			{"articulations",
				{{{"name", "Arco"}, {"type", "direction"}, {"group", 0}},
					{{"name", "Pizzicato"}, {"type", "direction"}, {"group", 0}}}},
			{"slots", {{{"name", "Arco"}}, {{"name", "Pizzicato"}, {"articulations", {"Pizzicato"}}}}}});
	app.call("track.set", {{"track", track}, {"articulationMap", map.at("map").at("id")}});
	const uint64_t part = app.call("midiPart.add", {{"track", track}, {"position", g_bar}, {"length", g_bar}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
	const Json added = app.call("note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", 4 * g_sixteenth}, {"pitch", 60}, {"velocity", 0.5}},
					{{"position", 8 * g_sixteenth}, {"length", 4 * g_sixteenth}, {"pitch", 60}, {"velocity", 0.5}}}}});
	const uint64_t second = added.at("notes").at(1).at("id").get<uint64_t>();
	app.call("editor.inPlace", {{"tracks", {track}}, {"open", true}});
	app.call("editor.set", {{"editor", "inPlace:" + std::to_string(track)}, {"lanes", {{{"type", "articulations"}}}}});
	const std::string editor = ".in-place-editor[track=\"" + std::to_string(track) + "\"]";
	const std::string lane = editor + " .controller-lane[lane-kind=articulations]";
	const std::string slot = lane + " .sound-slot[note=\"" + std::to_string(second) + "\"]";
	const auto elements = [&app](const std::string& selector)
	{ return app.ui().dom(std::nullopt, selector, 0).at("elements"); };
	const auto visible = [&](const std::string& selector)
	{
		const Json found = elements(selector);
		return !found.empty() && found.at(0).at("visible") == true;
	};
	const auto click = [&app](const ui::Target& target)
	{
		app.ui().click(std::nullopt, target, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	};
	REQUIRE(app.waitUntil([&] { return elements(lane + " .sound-slot").size() == 2; }));
	CHECK(elements(slot).at(0).at("attributes").at("title") == "Arco");

	// The draw tool adds a direction in its row, where the second note starts.
	REQUIRE(app.waitUntil([&] { return visible(editor + " .in-place-toolbar-toggle"); }));
	click(of(editor + " .in-place-toolbar-toggle"));
	const std::string draw = editor + " .key-tool[key-tool=draw]";
	REQUIRE(app.waitUntil([&] { return visible(draw); }));
	click(of(draw));
	REQUIRE(app.waitUntil([&] { return visible(slot); }));
	const Json box = elements(slot).at(0).at("box");
	const Json row = elements(lane + " .articulation-row").at(2).at("box");
	click({"", 0,
		ui::Point{static_cast<int>(std::lround(box.at("x").get<double>() + 1.0)),
			static_cast<int>(std::lround(row.at("y").get<double>() + (row.at("height").get<double>() / 2.0)))}});
	REQUIRE(app.waitUntil([&] { return elements(slot).at(0).at("attributes").at("title") == "Pizzicato"; }));
	const Json direction = app.call("project.get").at("midiParts").at(0).at("controllers").at(0);
	CHECK(direction.at("articulation") == "Pizzicato");
	CHECK(direction.at("position") == 8 * g_sixteenth);
}

TEST_CASE("the articulation map setup opened right after a new project has no track", "[ui]")
{
	Fixture f;
	// project.new through MCP or Lua, then the setup before the UI's next step: the track list still has the old
	// project's track selected, so the setup opens for it, and the project.reset reaches the setup's Lua while it reads
	// the project (#1464).
	f.app.call("project.new");
	f.app.keys().run("midi.articulationMapSetup", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements("body[track]").empty());
	CHECK(f.elements("#articulation-map-assign.unavailable").size() == 1);
}
