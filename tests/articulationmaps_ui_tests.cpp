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
			REQUIRE(app.waitUntil([this] { return display.project().value("tracks", Json::array()).size() == 1; }));
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
			for (const Json& item : elements("#articulation-maps listitem .map-name"))
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

		test::UiApp app{{1024, 768}}; // the setup, with a map in it, is taller: it scrolls (#1933)
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
			const Json selected = f.elements("#articulation-maps listitem.selected .map-name");
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
		const Json item = f.elements("#articulation-maps listitem.selected .map-name");
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
	CHECK_FALSE(f.shown("#articulation-key-switches")); // nor one to import key switches from
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

TEST_CASE("Add Map with Import Key Switches makes the track's instrument's key switches the map's sound slots", "[ui]")
{
	// The test synth reports the key switches Sustain and Staccato (p. 1284).
	Fixture f;
	const Json project = f.app.call("project.get");
	f.app.call("channel.set", {{"channel", project.at("tracks").at(0).at("channel")}, {"instrument", "testSynth"}});
	f.app.keys().run("midi.articulationMapSetup", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
	f.app.shell().step(std::chrono::milliseconds(0));

	f.click("#articulation-map-import-key-switches");
	f.click("#articulation-map-add");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().size() == 1; }));
	const Json map = f.maps().at(0);
	REQUIRE(map.at("slots").size() == 2);
	CHECK(map.at("slots").at(0).at("name") == "Sustain");
	CHECK(map.at("slots").at(1).at("name") == "Staccato");
	CHECK(map.at("slots").at(1).at("trigger") == 1);

	// Unchecked, Add Map adds an empty map.
	f.click("#articulation-map-import-key-switches");
	f.click("#articulation-map-add");
	REQUIRE(f.app.waitUntil([&f] { return f.maps().size() == 2; }));
	CHECK(f.maps().at(1).at("slots").empty());
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

		bool shown(const std::string& selector)
		{
			const Json found = elements(selector);
			return !found.empty() && found.at(0).at("visible").get<bool>();
		}

		// Whether the first element's border box lies inside the editor's window.
		bool inside(const std::string& selector)
		{
			const Json shown = box(selector);
			const Json windows = app.ui().windows();
			for (const Json& each : windows.at("windows"))
				if (each.at("id") == window)
					return shown.at("x").get<double>() >= 0.0 && shown.at("y").get<double>() >= 0.0 &&
						shown.at("x").get<double>() + shown.at("width").get<double>() <=
						each.at("width").get<double>() &&
						shown.at("y").get<double>() + shown.at("height").get<double>() <=
						each.at("height").get<double>();
			return false;
		}

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
	// #1498: the lane keeps its elements while their RML is the same, so a width code set on a kept element must go
	// again. A slot that loses its color loses its class .light or .dark, so its elements are new and have no color.
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
	const std::string arco = inLane(" .articulation-row[articulation-row=\"0\"] .articulation-item.direction");
	REQUIRE(f.app.waitUntil([&] { return f.elements(arco).size() == 1; }));
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
	const std::string triangle =
		".track-list listitem[value=\"" + std::to_string(track) + "\"] .in-place-toolbar-toggle";
	REQUIRE(app.waitUntil([&] { return visible(triangle); }));
	click(of(triangle));
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

TEST_CASE("the articulation map setup filters its maps, combines articulations and has the sound slot actions", "[ui]")
{
	Fixture f;
	const Json strings =
		f.app
			.call("articulationMap.add",
				{{"name", "Strings"}, {"groups", {"Technique", "Legato"}},
					{"articulations",
						{{{"name", "Arco"}, {"type", "direction"}, {"group", 0}},
							{{"name", "Pizzicato"}, {"type", "direction"}, {"group", 0}},
							{{"name", "Legato"}, {"type", "attribute"}, {"group", 1}}}},
					{"slots",
						{{{"name", "Arco"}, {"trigger", 36}},
							{{"name", "Pizzicato"}, {"articulations", {"Pizzicato"}}, {"trigger", 37}}}}})
			.at("map");
	f.app.call("articulationMap.add", {{"name", "Brass"}});
	f.app.call("track.set", {{"track", f.track}, {"articulationMap", strings.at("id")}});
	f.click("#menu-bar button[menu=midi-menu]");
	f.click("menuitem[command=\"midi.articulationMapSetup\"]");
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	const auto slots = [&f] { return f.maps().at(0).at("slots"); };
	const auto names = [&slots]
	{
		std::vector<std::string> found;
		for (const Json& slot : slots())
			found.push_back(slot.at("name").get<std::string>());
		return found;
	};
	const auto triggers = [&slots]
	{
		std::vector<int> found;
		for (const Json& slot : slots())
			found.push_back(slot.at("trigger").is_null() ? -1 : slot.at("trigger").get<int>());
		return found;
	};
	using Names = std::vector<std::string>;

	// The maps list (p. 1267): the used map marked P, Show only Maps used in this Project and the search.
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == Names{"Strings", "Brass"}; }));
	CHECK(f.elements("#articulation-maps .used-mark").size() == 1);
	f.click("#articulation-maps-used");
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == Names{"Strings"}; }));
	f.click("#articulation-maps-used");
	f.write("#articulation-maps-search", "bra");
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == Names{"Brass"}; }));
	f.click("#articulation-maps-search");
	f.app.ui().key(std::nullopt, "Mod+A");
	f.app.ui().key(std::nullopt, "Backspace");
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == Names{"Strings", "Brass"}; }));
	// The search selected Brass, the only map it listed (#1667): Strings again.
	f.click("#articulation-maps listitem");
	REQUIRE(f.app.waitUntil([&f] { return f.elements("#articulation-maps listitem.selected .map-name").size() == 1; }));
	REQUIRE(f.app.waitUntil(
		[&f]
		{
			return f.elements("#articulation-maps listitem.selected .map-name").at(0).value("text", std::string()) ==
				"Strings";
		}));

	// The articulation combinations (pp. 1275-1276): Legato checked makes an add-on slot of it; the search shows
	// the articulations with one of its terms.
	f.click("#articulation-combinations input[articulation=Legato]");
	f.click("#articulation-slot-add-on");
	REQUIRE(f.app.waitUntil([&slots] { return slots().size() == 3; }));
	CHECK(slots().at(2).at("name") == "Legato");
	CHECK(slots().at(2).at("articulations") == Json{"Legato"});
	CHECK(slots().at(2).at("addOn") == true);
	f.write("#articulation-combination-search", "pizz OR leg");
	REQUIRE(f.app.waitUntil([&f] { return !f.shown(".combination[name=Arco]"); }));
	CHECK(f.shown(".combination[name=Pizzicato]"));
	CHECK(f.shown(".combination[name=Legato]"));

	// The new MIDI modifiers of the picked slot, which an add-on slot doesn't have (#1667).
	CHECK(f.elements("#articulation-modifiers .add-on-modifiers").size() == 1);
	CHECK(f.elements("#articulation-map input[kind=modifier]").empty());
	f.click("input[name=articulation-pick-slot][index=\"2\"]");
	f.write("#articulation-map input[kind=modifier][key=attack]", "15");
	REQUIRE(f.app.waitUntil([&slots] { return slots().at(1).at("attack") == 15; }));
	f.write("#articulation-map input[kind=modifier][key=maxVelocity]", "100");
	REQUIRE(f.app.waitUntil([&slots] { return slots().at(1).at("maxVelocity") == 100; }));

	// The sound slot actions (p. 1273): Move Up and Down, Copy, Paste and Cut.
	f.click("input[name=articulation-pick-slot][index=\"2\"]");
	f.click("#articulation-slot-up");
	REQUIRE(f.app.waitUntil([&names] { return names() == Names{"Pizzicato", "Arco", "Legato"}; }));
	f.click("#articulation-slot-down");
	REQUIRE(f.app.waitUntil([&names] { return names() == Names{"Arco", "Pizzicato", "Legato"}; }));
	f.click("#articulation-slot-copy");
	f.click("#articulation-slot-paste");
	REQUIRE(f.app.waitUntil([&names] { return names() == Names{"Arco", "Pizzicato", "Pizzicato", "Legato"}; }));
	f.click("#articulation-slot-cut");
	REQUIRE(f.app.waitUntil([&names] { return names() == Names{"Arco", "Pizzicato", "Legato"}; }));

	// The remote triggers: Transpose Up of the picked slot, Reassign All, Transpose All and Remove All.
	f.click("input[name=articulation-pick-slot][index=\"1\"]");
	f.click("#articulation-trigger-up");
	REQUIRE(f.app.waitUntil([&triggers] { return triggers() == std::vector<int>{37, 37, -1}; }));
	f.write("#articulation-reassign-start", "60");
	f.click("#articulation-triggers-reassign");
	REQUIRE(f.app.waitUntil([&triggers] { return triggers() == std::vector<int>{60, 61, 62}; }));
	f.click("#articulation-triggers-transpose"); // by 12 semitones
	REQUIRE(f.app.waitUntil([&triggers] { return triggers() == std::vector<int>{72, 73, 74}; }));
	f.click("#articulation-triggers-remove");
	REQUIRE(f.app.waitUntil([&triggers] { return triggers() == std::vector<int>{-1, -1, -1}; }));
}

TEST_CASE("the articulation map setup's slot actions send what they say, and its rows follow the changes", "[ui]")
{
	Fixture f;
	const Json strings =
		f.app
			.call("articulationMap.add",
				{{"name", "Strings"}, {"groups", {"Technique", "Legato"}},
					{"articulations",
						{{{"name", "Arco"}, {"type", "direction"}, {"group", 0}},
							{{"name", "Pizzicato"}, {"type", "direction"}, {"group", 0}},
							{{"name", "Legato"}, {"type", "attribute"}, {"group", 1}}}},
					{"slots",
						{{{"name", "Arco"}, {"trigger", 36}},
							{{"name", "Pizzicato"}, {"articulations", {"Pizzicato"}}, {"trigger", 37},
								{"outputs", {{{"status", "note"}, {"data1", 25}, {"data2", 100}}}}},
							{{"name", "Legato"}, {"articulations", {"Legato"}}, {"trigger", 38}, {"addOn", true}}}}})
			.at("map");
	const Json brass =
		f.app.call("articulationMap.add", {{"name", "Brass"}, {"slots", {{{"name", "Open"}, {"trigger", 40}}}}})
			.at("map");
	f.app.call("track.set", {{"track", f.track}, {"articulationMap", strings.at("id")}});
	f.click("#menu-bar button[menu=midi-menu]");
	f.click("menuitem[command=\"midi.articulationMapSetup\"]");
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#articulation-maps-dialog"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	using Names = std::vector<std::string>;
	using Triggers = std::vector<int>;
	const auto slots = [&f](size_t map = 0) { return f.maps().at(map).at("slots"); };
	const auto names = [&slots]
	{
		Names found;
		for (const Json& slot : slots())
			found.push_back(slot.at("name").get<std::string>());
		return found;
	};
	const auto triggers = [&slots](size_t map = 0)
	{
		Triggers found;
		for (const Json& slot : slots(map))
			found.push_back(slot.at("trigger").is_null() ? -1 : slot.at("trigger").get<int>());
		return found;
	};
	const auto marks = [&f] { return f.elements("#articulation-maps .used-mark").size(); };
	const auto pickSlot = [&f](int index)
	{ f.click("input[name=articulation-pick-slot][index=\"" + std::to_string(index) + "\"]"); };
	const auto picked = [&f](int index)
	{
		return f.elements("input[name=articulation-pick-slot][index=\"" + std::to_string(index) + "\"][checked]")
				   .size() == 1;
	};
	const auto mapName = [&f]
	{
		const Json found = f.elements("#articulation-map input[kind=map][key=name]");
		return found.empty() ? std::string() : found.at(0).at("attributes").value("value", std::string());
	};
	// Chooses the next option of a drop-down.
	const auto nextOption = [&f](const std::string& selector)
	{
		f.click(selector);
		f.app.ui().key(std::nullopt, "Down");
		f.click(selector);
	};

	// The P marks follow the tracks' maps, changed elsewhere (track.changed) or with Assign to Track (#1667).
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == Names{"Strings", "Brass"}; }));
	CHECK(marks() == 1);
	f.app.call("track.set", {{"track", f.track}, {"articulationMap", 0}});
	REQUIRE(f.app.waitUntil([&marks] { return marks() == 0; }));
	f.click("#articulation-map-assign");
	REQUIRE(f.app.waitUntil([&marks] { return marks() == 1; }));

	// A search that leaves the selected map out selects the first one it lists, and one that lists none shows no
	// map (#1667).
	REQUIRE(mapName() == "Strings");
	f.write("#articulation-maps-search", "bra");
	REQUIRE(f.app.waitUntil([&f] { return f.listed() == Names{"Brass"}; }));
	REQUIRE(f.app.waitUntil([&mapName] { return mapName() == "Brass"; }));
	f.write("#articulation-maps-search", "none");
	REQUIRE(f.app.waitUntil([&f] { return f.listed().empty(); }));
	CHECK(f.elements("#articulation-map.unavailable").size() == 1);
	f.write("#articulation-maps-search", "str");
	REQUIRE(f.app.waitUntil([&mapName] { return mapName() == "Strings"; }));
	CHECK(f.elements("#articulation-map.unavailable").empty());

	// The combinations follow a renamed articulation and the articulations typed for the picked slot (#1667).
	f.write("#articulation-list input[kind=articulation][index=\"2\"][key=name]", "Pizz");
	REQUIRE(f.app.waitUntil([&f] { return f.shown(".combination[name=Pizz]"); }));
	CHECK(f.elements(".combination[name=Pizzicato]").empty());
	f.write("#articulation-slots input[kind=slot][index=\"1\"][key=articulations]", "Arco");
	REQUIRE(f.app.waitUntil(
		[&f] { return f.elements("#articulation-combinations input[articulation=Arco][checked]").size() == 1; }));

	// Apply to Slot: the picked slot gets the checked articulations.
	f.click("#articulation-combinations input[articulation=Arco]");
	f.click("#articulation-combinations input[articulation=Legato]");
	f.click("#articulation-combination-apply");
	REQUIRE(f.app.waitUntil([&slots] { return slots().at(0).at("articulations") == Json{"Legato"}; }));

	// As Default refused for an add-on slot keeps it picked; Duplicate leaves the trigger out and picks the copy;
	// As Default moves the picked slot to the top.
	pickSlot(3);
	f.click("#articulation-slot-default");
	REQUIRE(f.app.waitUntil([&f] { return f.message().find("default") != std::string::npos; }));
	CHECK(names() == Names{"Arco", "Pizzicato", "Legato"});
	CHECK(picked(3));
	pickSlot(2);
	f.click("#articulation-slot-duplicate");
	REQUIRE(f.app.waitUntil([&names] { return names() == Names{"Arco", "Pizzicato", "Pizzicato", "Legato"}; }));
	CHECK(triggers() == Triggers{36, 37, -1, 38});
	CHECK(picked(3));
	f.click("#articulation-slot-default");
	REQUIRE(f.app.waitUntil([&triggers] { return triggers() == Triggers{-1, 36, 37, 38}; }));
	CHECK(picked(1));

	// The clipboard keeps the slot as it was copied: an output mapping added after Copy doesn't paste (#1667).
	pickSlot(3);
	f.click("#articulation-slot-copy");
	f.click("#articulation-output-add");
	REQUIRE(f.app.waitUntil([&slots] { return slots().at(2).at("outputs").size() == 2; }));
	f.click("#articulation-slot-paste");
	REQUIRE(f.app.waitUntil([&slots] { return slots().size() == 5; }));
	CHECK(slots().at(3).at("outputs").size() == 1);
	CHECK(triggers() == Triggers{-1, 36, 37, 37, 38});

	// Transpose Down of the picked slot.
	pickSlot(2);
	f.click("#articulation-trigger-down");
	REQUIRE(f.app.waitUntil([&triggers] { return triggers() == Triggers{-1, 35, 37, 37, 38}; }));

	// Reassign All on the white keys from 59 with Skip First Slot: the first slot keeps none.
	f.write("#articulation-reassign-start", "59");
	nextOption("#articulation-reassign-keys");
	f.click("#articulation-reassign-skip");
	f.click("#articulation-triggers-reassign");
	REQUIRE(f.app.waitUntil([&triggers] { return triggers() == Triggers{-1, 59, 60, 62, 64}; }));

	// Transpose All by an octave, of all maps.
	f.write("#articulation-transpose-by", "1");
	nextOption("#articulation-transpose-unit");
	f.click("#articulation-transpose-all");
	f.click("#articulation-triggers-transpose");
	REQUIRE(f.app.waitUntil([&triggers] { return triggers() == Triggers{-1, 71, 72, 74, 76}; }));
	CHECK(triggers(1) == Triggers{52});
	CHECK(f.maps().at(1).at("id") == brass.at("id"));
}

TEST_CASE("the Articulations lane collapses and expands its groups and hides its sound slots lane", "[ui]")
{
	// #1580, reference manual pp. 1287-1288.
	Editor f;
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 4 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	f.app.call("note.set", {{"part", f.part}, {"notes", {{{"id", f.notes[2]}, {"articulations", {"Staccato"}}}}}});
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item")).size() == 2; }));
	const auto view = [&f] { return f.app.call("editor.get", {{"editor", "window"}}); };
	// The lane's controls (#1658, pp. 1287-1288), under the articulations' names.
	const auto control = [&f](const std::string& action)
	{
		const std::string button = inLane(" .articulation-control[lane-action=" + action + "]");
		const Json names = f.elements(inLane(" .articulation-name"));
		const Json last = names.at(names.size() - 1).at("box");
		CHECK(
			f.box(button).at("y").get<double>() >= last.at("y").get<double>() + last.at("height").get<double>() - 0.5);
		const Json header = f.box(inLane(" .controller-lane-header"));
		CHECK(f.box(button).at("x").get<double>() + f.box(button).at("width").get<double>() <=
			header.at("x").get<double>() + header.at("width").get<double>() + 0.5);
		f.click(of(button));
	};

	// Collapse All Groups: a row per group with the group's name beside it, and its articulations in it.
	control("collapseGroups");
	REQUIRE(f.app.waitUntil([&] { return view().at("collapsedGroups") == Json::array({0, 1}); }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-row")).size() == 3; }));
	const Json names = f.elements(inLane(" .articulation-name"));
	REQUIRE(names.size() == 2);
	CHECK(names.at(0).value("text", std::string()) == "Technique");
	CHECK(names.at(1).value("text", std::string()) == "Length");
	CHECK(f.elements(inLane(" .articulation-row.collapsed .articulation-item")).size() == 2);
	CHECK(f.elements(inLane(" .articulation-row.collapsed .articulation-item.direction"))
			  .at(0)
			  .value("text", std::string()) == "Pizzicato");
	// The erase tool removes an articulation in a collapsed row too.
	f.tool("erase");
	f.click(f.at(2, 9.0));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[2]).empty(); }));

	// A click on a group's name expands it, a click on an articulation's name collapses its group.
	f.click(of(inLane(" .articulation-name[articulation-group=\"0\"]")));
	REQUIRE(f.app.waitUntil([&] { return view().at("collapsedGroups") == Json::array({1}); }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-row")).size() == 4; }));
	f.click(of(inLane(" .articulation-name[articulation-group=\"0\"]")));
	REQUIRE(f.app.waitUntil([&] { return view().at("collapsedGroups") == Json::array({0, 1}); }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-row")).size() == 3; }));
	control("expandGroups");
	REQUIRE(f.app.waitUntil([&] { return view().at("collapsedGroups").empty(); }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-row")).size() == 4; }));

	// Show/Hide Sound Slots Lane: its row beside the lane's name stays, without the slots.
	REQUIRE(f.elements(inLane(" .sound-slot")).size() == 3);
	control("soundSlots");
	REQUIRE(f.app.waitUntil([&] { return view().at("soundSlotsLane") == false; }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .sound-slot")).empty(); }));
	control("soundSlots");
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .sound-slot")).size() == 3; }));

	// They are the lane's, not items of the lanes' context menu; the other lanes don't have them.
	CHECK(f.elements("#controller-lane-context-menu menuitem[lane-action=collapseGroups]").empty());
	f.app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "velocity"}}}}});
	REQUIRE(f.app.waitUntil([&f] { return f.elements(".controller-lane[lane-kind=velocity]").size() == 1; }));
	CHECK(f.elements(".articulation-control").empty());
}

TEST_CASE(
	"the Articulations lane selects with the object selection tool, deletes with the keys and makes resets", "[ui]")
{
	// #1580, reference manual pp. 1289-1290.
	Editor f;
	const Json added = f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 4 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	const auto direction = added.at("controllers").at(0).at("id").get<uint64_t>();
	f.app.call("note.set",
		{{"part", f.part},
			{"notes",
				{{{"id", f.notes[1]}, {"articulations", {"Staccato", "Other"}}},
					{{"id", f.notes[2]}, {"articulations", {"Staccato"}}}}}});
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item")).size() == 3; }));
	const auto view = [&f] { return f.app.call("editor.get", {{"editor", "window"}}); };

	// A click selects a direction, or an attribute's note; Shift adds to the selection.
	f.click(f.at(2, 6.0));
	REQUIRE(f.app.waitUntil([&]
		{ return view().at("controllerSelection") == Json::array({{{"part", f.part}, {"controller", direction}}}); }));
	f.click(f.at(3, 5.0));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return view().at("selection") ==
				Json::array({{{"part", f.part}, {"note", f.notes[1]}, {"articulations", {"Staccato"}}}});
		}));
	CHECK(view().at("controllerSelection").empty());
	// The lane has the selection too before Shift adds to it.
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.attribute.selected")).size() == 1; }));
	f.app.ui().click(f.window, f.at(2, 6.0), ui::MouseButton::Left, 1, {"shift"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return view().at("controllerSelection").size() == 1; }));
	CHECK(view().at("selection").size() == 1);
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.selected")).size() == 2; }));

	// Delete removes the selected direction and the map's attributes of the selected note, not the note.
	f.app.ui().key(f.window, "Delete");
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&f] { return f.midiPart().at("controllers").empty(); }));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[1]) == Json{"Other"}; }));
	CHECK(f.midiPart().at("notes").size() == 3);
	CHECK(f.articulations(f.notes[2]) == Json{"Staccato"});
	// Backspace too, once the lane shows what Delete left.
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item")).size() == 1; }));
	f.click(f.at(3, 9.0));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return view().at("selection") ==
				Json::array({{{"part", f.part}, {"note", f.notes[2]}, {"articulations", {"Staccato"}}}});
		}));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.attribute.selected")).size() == 1; }));
	f.app.ui().key(f.window, "Backspace");
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[2]).empty(); }));
	CHECK(f.midiPart().at("notes").size() == 3);

	// The event pop-up menu's Reset makes a direction a reset (p. 1290), and again a direction.
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 4 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.direction")).size() == 1; }));
	// Reset is checked on a reset (#1658).
	const auto reset = [&f](const ui::Target& target, ui::MouseButton button, bool checked)
	{
		f.app.ui().click(f.window, target, button, 1, {});
		f.app.shell().step(std::chrono::milliseconds(0));
		const std::string item = "#controller-articulation-menu menuitem[lane-action=reset]";
		REQUIRE(f.app.waitUntil(
			[&]
			{
				const Json found = f.elements(item);
				return !found.empty() && found.at(0).at("visible") == true;
			}));
		CHECK(f.elements(item).at(0).at("attributes").contains("checked") == checked);
		f.click(of(item));
	};
	// The object selection tool's arrow in the upper left corner of a selected direction, or of the direction the
	// mouse is over, opens the menu (#1658, p. 1290); a right click does too.
	CHECK(f.elements(inLane(" .articulation-item.direction .articulation-menu-arrow")).size() == 1);
	f.click(f.at(2, 6.0));
	const std::string arrow = inLane(" .articulation-item.direction.selected .articulation-menu-arrow");
	REQUIRE(f.app.waitUntil([&] { return f.elements(arrow).size() == 1 && f.elements(arrow).at(0).at("visible"); }));
	reset(of(arrow), ui::MouseButton::Left, false);
	REQUIRE(f.app.waitUntil([&f] { return f.midiPart().at("controllers").at(0).value("reset", false); }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.reset")).size() == 1; }));
	reset(of(inLane(" .articulation-item.reset")), ui::MouseButton::Right, true);
	REQUIRE(f.app.waitUntil([&f] { return !f.midiPart().at("controllers").at(0).value("reset", false); }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.reset")).empty(); }));
	// The other tools have no arrows.
	f.tool("draw");
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-menu-arrow")).empty(); }));
}

TEST_CASE("the Articulations lane deletes the attributes selected, and shows when it has the keyboard focus", "[ui]")
{
	// #1658, reference manual pp. 100, 1289.
	Editor f;
	Json map = f.app.call("project.get").at("articulationMaps").at(0);
	map["groups"].push_back("Dynamics");
	map["articulations"].push_back({{"name", "Accent"}, {"type", "attribute"}, {"group", 2}});
	f.app.call("articulationMap.set",
		{{"map", map.at("id")}, {"groups", map.at("groups")}, {"articulations", map.at("articulations")}});
	const auto both = [&f]
	{
		f.app.call("note.set",
			{{"part", f.part}, {"notes", {{{"id", f.notes[1]}, {"articulations", {"Staccato", "Accent"}}}}}});
		REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.attribute")).size() == 2; }));
	};
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-row")).size() == 5; }));
	both();
	const auto selected = [&f](const char* row)
	{
		return f
			.elements(inLane(" .articulation-item.attribute.selected[articulation-row=\"" + std::string(row) + "\"]"))
			.size();
	};
	const std::string focused = ".controller-lane[lane-kind=articulations].focused";
	CHECK(f.elements(focused).empty());

	// A click selects the attribute pressed, with its note, and gives the lane the keyboard focus, which it shows.
	f.click(f.at(3, 5.0));
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 1; }));
	CHECK(selected("3") == 0);
	CHECK(f.elements(focused).size() == 1);
	// Delete removes that attribute, not the note's other one.
	f.app.ui().key(f.window, "Delete");
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[1]) == Json{"Accent"}; }));

	// Shift adds an attribute of a selected note and removes it again; the note stays selected while one is.
	both();
	f.click(f.at(3, 5.0));
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 1; }));
	f.app.ui().click(f.window, f.at(4, 5.0), ui::MouseButton::Left, 1, {"shift"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 1 && selected("3") == 1; }));
	f.app.ui().click(f.window, f.at(3, 5.0), ui::MouseButton::Left, 1, {"shift"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 0 && selected("3") == 1; }));
	CHECK(f.app.call("editor.get", {{"editor", "window"}}).at("selection") ==
		Json::array({{{"part", f.part}, {"note", f.notes[1]}, {"articulations", {"Accent"}}}}));
	f.app.ui().key(f.window, "Backspace");
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[1]) == Json{"Staccato"}; }));

	// A note selected elsewhere has all its attributes selected.
	both();
	f.app.call("editor.set", {{"editor", "window"}, {"selection", Json::array()}});
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 0 && selected("3") == 0; }));
	f.app.call("editor.set", {{"editor", "window"}, {"selection", {{{"part", f.part}, {"note", f.notes[1]}}}}});
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 1 && selected("3") == 1; }));

	// A click outside the lanes takes the keyboard focus, and the lane no longer shows it.
	REQUIRE(f.app.waitUntil([&] { return !f.elements(focused).empty(); }));
	f.tool("draw");
	REQUIRE(f.app.waitUntil([&] { return f.elements(focused).empty(); }));
}

TEST_CASE("the Articulations lane's frame takes no room, so that its area lines up with the other lanes'", "[ui]")
{
	// #1688, reference manual p. 100: directions and attributes are placed in percent of the area, so it has the same
	// x and width as another lane's area, with the keyboard focus and without.
	Editor f;
	f.app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "velocity"}}, {{"type", "articulations"}}}}});
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-row")).size() == 4; }));
	const auto linedUp = [&f]
	{
		f.app.shell().step(std::chrono::milliseconds(0));
		const Json velocity = f.box(".controller-lane[lane-kind=velocity] .controller-lane-area");
		const Json articulations = f.box(inLane(" .controller-lane-area"));
		return std::abs(velocity.at("x").get<double>() - articulations.at("x").get<double>()) < 0.5 &&
			std::abs(velocity.at("width").get<double>() - articulations.at("width").get<double>()) < 0.5;
	};
	CHECK(!f.shown(inLane(" .controller-lane-frame")));
	CHECK(linedUp());
	f.click(f.at(1, 1.0));
	REQUIRE(f.app.waitUntil([&f] { return f.shown(inLane(".focused .controller-lane-frame")); }));
	CHECK(linedUp());
	// The frame lies over the whole lane.
	const Json lane = f.box(inLane(""));
	const Json frame = f.box(inLane(" .controller-lane-frame"));
	for (const char* key : {"x", "y", "width", "height"})
		CHECK(std::abs(frame.at(key).get<double>() - lane.at(key).get<double>()) < 0.5);
}

TEST_CASE("the info line's pop-up menu changes only the selected notes of tracks with the first one's map", "[ui]")
{
	// #1658, reference manual p. 1290.
	Editor f;
	const auto other = f.app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
	const auto part = f.app.call("midiPart.add", {{"track", other}, {"position", 0}, {"length", g_bar}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	const auto note = f.app
						  .call("note.add",
							  {{"part", part},
								  {"notes",
									  Json::array({{{"position", 2 * g_sixteenth}, {"length", g_sixteenth},
										  {"pitch", 64}, {"velocity", 0.5}}})}})
						  .at("notes")
						  .at(0)
						  .at("id")
						  .get<uint64_t>();
	// A track with another map, which has an attribute of the same name (#1688).
	const auto brass = f.app
						   .call("articulationMap.add",
							   {{"name", "Brass"}, {"groups", {"Length"}},
								   {"articulations", {{{"name", "Staccato"}, {"type", "attribute"}, {"group", 0}}}}})
						   .at("map")
						   .at("id");
	const auto third = f.app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
	f.app.call("track.set", {{"track", third}, {"articulationMap", brass}});
	const auto thirdPart = f.app.call("midiPart.add", {{"track", third}, {"position", 0}, {"length", g_bar}})
							   .at("part")
							   .at("id")
							   .get<uint64_t>();
	const auto thirdNote = f.app
							   .call("note.add",
								   {{"part", thirdPart},
									   {"notes",
										   Json::array({{{"position", 2 * g_sixteenth}, {"length", g_sixteenth},
											   {"pitch", 65}, {"velocity", 0.5}}})}})
							   .at("notes")
							   .at(0)
							   .at("id")
							   .get<uint64_t>();
	f.app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part, part, thirdPart})}});
	f.app.call("editor.set",
		{{"editor", "window"},
			{"selection",
				{{{"part", f.part}, {"note", f.notes[0]}}, {{"part", part}, {"note", note}},
					{{"part", thirdPart}, {"note", thirdNote}}}}});
	REQUIRE(
		f.app.waitUntil([&f] { return f.app.call("editor.get", {{"editor", "window"}}).at("selection").size() == 3; }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(".key-editor-info.empty").empty(); }));
	f.click(of(".key-editor-info button.articulation-menu"));
	const std::string staccato = "#key-articulations-menu menuitem[articulation=Staccato]";
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json found = f.elements(staccato);
			return !found.empty() && found.at(0).at("visible") == true;
		}));
	f.click(of(staccato));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[0]) == Json{"Staccato"}; }));
	// The other track has no map, the third another one: their notes get none.
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json project = f.app.call("project.get");
	for (const Json& each : project.at("midiParts"))
		if (each.at("id") == part || each.at("id") == thirdPart)
			CHECK(each.at("notes").at(0).value("articulations", Json::array()).empty());
}

TEST_CASE("the info line and the list editor show a warning sign for articulations the map doesn't have", "[ui]")
{
	// #1580, reference manual p. 1291.
	Editor f;
	f.app.call("note.set",
		{{"part", f.part},
			{"notes",
				{{{"id", f.notes[0]}, {"articulations", {"Staccato"}}},
					{{"id", f.notes[1]}, {"articulations", {"Staccato", "Legato"}}}}}});
	const std::string field = ".key-editor-info .info-field[info-field=articulations]";
	const auto warns = [&f](const std::string& selector)
	{
		const Json found = f.elements(selector + " .articulation-warning");
		return !found.empty() && found.at(0).at("visible") == true;
	};
	const auto select = [&f](uint64_t note)
	{ f.app.call("editor.set", {{"editor", "window"}, {"selection", {{{"part", f.part}, {"note", note}}}}}); };
	select(f.notes[0]);
	REQUIRE(
		f.app.waitUntil([&f] { return f.value(".key-editor-info textdisplay[field=articulations]") == "Staccato"; }));
	CHECK_FALSE(warns(field));
	select(f.notes[1]);
	REQUIRE(f.app.waitUntil([&] { return warns(field); }));

	// The list editor: a note's attributes and a direction the map doesn't have.
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers", {{{"position", 0}, {"type", "articulation"}, {"articulation", "Tremolo"}}}}});
	f.app.call("editor.set", {{"editor", "window"}, {"content", "list"}});
	const auto row = [](const char* kind, uint64_t id)
	{ return std::string(".list-editor-rows .list-row[") + kind + "=\"" + std::to_string(id) + "\"]"; };
	REQUIRE(f.app.waitUntil([&] { return warns(row("note", f.notes[1])); }));
	CHECK_FALSE(warns(row("note", f.notes[0])));
	const auto direction = f.midiPart().at("controllers").at(0).at("id").get<uint64_t>();
	REQUIRE(f.app.waitUntil([&] { return warns(row("controller", direction)); }));
	f.app.call("controller.set", {{"part", f.part}, {"controllers", {{{"id", direction}, {"articulation", "Arco"}}}}});
	REQUIRE(f.app.waitUntil([&] { return !warns(row("controller", direction)); }));
}

TEST_CASE("the info line's and the list editor's pop-up menus choose the map's articulations", "[ui]")
{
	// #1580, reference manual pp. 1290-1291.
	Editor f;
	const auto choose = [&f](const ui::Target& opener, ui::MouseButton button, const std::string& item)
	{
		f.app.ui().click(f.window, opener, button, 1, {});
		f.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(f.app.waitUntil(
			[&]
			{
				const Json found = f.elements(item);
				return !found.empty() && found.at(0).at("visible") == true;
			}));
		f.click(of(item));
	};
	f.app.call("editor.set",
		{{"editor", "window"},
			{"selection", {{{"part", f.part}, {"note", f.notes[1]}}, {{"part", f.part}, {"note", f.notes[2]}}}}});
	const std::string field = ".key-editor-info textdisplay[field=articulations]";
	REQUIRE(f.app.waitUntil([&f] { return f.elements(".key-editor-info.empty").empty(); }));
	// The map's attributes; one chosen goes to every selected note, chosen again it goes.
	const std::string staccato = "#key-articulations-menu menuitem[articulation=Staccato]";
	choose(of(".key-editor-info button.articulation-menu"), ui::MouseButton::Left, staccato);
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[2]) == Json{"Staccato"}; }));
	CHECK(f.articulations(f.notes[1]) == Json{"Staccato"});
	CHECK(f.articulations(f.notes[0]).empty());
	REQUIRE(f.app.waitUntil([&] { return f.value(field) == "Staccato"; }));
	CHECK(f.elements("#key-articulations-menu menuitem[articulation]").size() == 1);
	// Opened again, the notes' attribute is checked (#1658); chosen, it goes and is no longer checked.
	const std::string checkedStaccato = staccato + "[checked]";
	choose(of(".key-editor-info button.articulation-menu"), ui::MouseButton::Left, checkedStaccato);
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[1]).empty(); }));
	CHECK(f.articulations(f.notes[2]).empty());
	REQUIRE(f.app.waitUntil([&] { return f.value(field).empty(); }));
	f.app.ui().click(f.window, of(".key-editor-info button.articulation-menu"), ui::MouseButton::Left, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json found = f.elements(staccato);
			return !found.empty() && found.at(0).at("visible") == true;
		}));
	CHECK(f.elements(checkedStaccato).empty());
	f.app.ui().key(f.window, "Escape");
	f.app.shell().step(std::chrono::milliseconds(0));

	// The list editor's Attributes column: a click on a note's (#1658), a right click on a direction's.
	const Json added = f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 4 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	const auto direction = added.at("controllers").at(0).at("id").get<uint64_t>();
	f.app.call("editor.set", {{"editor", "window"}, {"content", "list"}});
	const auto cell = [](const char* kind, uint64_t id)
	{
		return std::string(".list-editor-rows .list-row[") + kind + "=\"" + std::to_string(id) +
			"\"] textdisplay[field=attributes]";
	};
	REQUIRE(f.app.waitUntil([&] { return !f.elements(cell("note", f.notes[0])).empty(); }));
	choose(of(cell("note", f.notes[0])), ui::MouseButton::Left,
		"#list-articulations-menu menuitem[articulation=Staccato]");
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[0]) == Json{"Staccato"}; }));
	// Opened again, the note's attribute is checked, and stays checked while the list editor renders (#1658).
	REQUIRE(f.app.waitUntil([&] { return f.value(cell("note", f.notes[0])) == "Staccato"; }));
	f.click(of(cell("note", f.notes[0])));
	const std::string checked = "#list-articulations-menu menuitem[articulation=Staccato][checked]";
	REQUIRE(f.app.waitUntil([&] { return !f.elements(checked).empty(); }));
	f.app.call("editor.set", {{"editor", "window"}, {"insertVelocity", 0.25}});
	REQUIRE(f.app.waitUntil(
		[&f] { return f.app.call("editor.get", {{"editor", "window"}}).at("insertVelocity") == 0.25; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(f.elements(checked).empty());
	f.app.ui().key(f.window, "Escape");
	f.app.shell().step(std::chrono::milliseconds(0));
	choose(of(cell("controller", direction)), ui::MouseButton::Right,
		"#list-articulations-menu menuitem[articulation=Arco]");
	REQUIRE(f.app.waitUntil([&f] { return f.midiPart().at("controllers").at(0).at("articulation") == "Arco"; }));
	CHECK(f.elements("#list-articulations-menu menuitem[articulation]").size() == 2);
}

TEST_CASE("the list editor's pop-up menu of a row at the window's bottom lies inside the window", "[ui]")
{
	// #1580: the menu gets its items after it opened, and moves inside the window for them. With more attributes
	// than fit under the lowest row, the menu can't lie inside the window without the move (#1665).
	Editor f("list");
	Json map = f.app.call("project.get").at("articulationMaps").at(0);
	Json articulations = map.at("articulations");
	for (int extra = 1; extra <= 8; ++extra)
		articulations.push_back({{"name", "Accent " + std::to_string(extra)}, {"type", "attribute"}, {"group", 1}});
	f.app.call("articulationMap.set", {{"map", map.at("id")}, {"articulations", articulations}});
	Json items = Json::array();
	for (int at = 0; at < 64; ++at)
		items.push_back(
			{{"position", at * g_sixteenth / 4}, {"length", g_sixteenth / 4}, {"pitch", 62}, {"velocity", 0.5}});
	f.app.call("note.add", {{"part", f.part}, {"notes", items}});
	const std::string rows = ".list-editor-rows .list-row[note]";
	REQUIRE(f.app.waitUntil([&] { return f.elements(rows).size() == 67; }));
	// The lowest row that the scrolled area shows whole.
	const Json area = f.box(".list-editor-body");
	const double bottom = area.at("y").get<double>() + area.at("height").get<double>();
	std::string lowest;
	double lowestY = -1.0;
	for (const Json& row : f.elements(rows))
	{
		const Json shown = row.at("box");
		const double y = shown.at("y").get<double>();
		if (y > lowestY && y + shown.at("height").get<double>() <= bottom)
		{
			lowestY = y;
			lowest = row.at("attributes").at("note").get<std::string>();
		}
	}
	REQUIRE_FALSE(lowest.empty());
	const std::string cell = ".list-editor-rows .list-row[note=\"" + lowest + "\"] textdisplay[field=attributes]";
	const Json pressed = f.box(cell);
	f.app.ui().click(f.window, of(cell), ui::MouseButton::Right, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	const std::string menu = "#list-articulations-menu";
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json found = f.elements(menu + " menuitem[articulation]");
			return found.size() == 9 && found.at(0).at("visible") == true;
		}));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.inside(menu));
	// It didn't fit under the press: it moved up.
	CHECK(
		f.box(menu).at("y").get<double>() < pressed.at("y").get<double>() + (pressed.at("height").get<double>() / 2.0));
}

TEST_CASE("the Articulations lane has no event pop-up menu on a direction of a part that isn't editable", "[ui]")
{
	// #1580, reference manual p. 1290: with Edit Active Part Only, Reset doesn't change the other parts.
	Editor f;
	const uint64_t second = f.app.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
								.at("part")
								.at("id")
								.get<uint64_t>();
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 4 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	f.app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part, second})}});
	f.app.call("editor.set",
		{{"editor", "window"}, {"activePart", second}, {"editingMode", "activePart"},
			{"lanes", {{{"type", "articulations"}}}}});
	const std::string direction = inLane(" .articulation-item.direction.inactive");
	REQUIRE(f.app.waitUntil([&] { return f.elements(direction).size() == 1; }));
	f.app.ui().click(f.window, of(direction), ui::MouseButton::Right, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json reset = f.elements("#controller-articulation-menu menuitem[lane-action=reset]");
	CHECK((reset.empty() || reset.at(0).at("visible") == false));
	CHECK_FALSE(f.midiPart().at("controllers").at(0).value("reset", false));
}

TEST_CASE("the mute tool mutes and unmutes the Articulations lane's directions and attributes, shown in gray", "[ui]")
{
	// #1684, reference manual p. 1289.
	Editor f;
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 4 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	f.app.call("note.set", {{"part", f.part}, {"notes", {{{"id", f.notes[2]}, {"articulations", {"Staccato"}}}}}});
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item")).size() == 2; }));
	REQUIRE(f.app.waitUntil([&f] { return f.slot(f.notes[1]) == "Pizzicato"; }));
	const std::string muted = inLane(" .articulation-item.muted");
	CHECK(f.elements(muted).empty());

	// A muted direction is gray and selects no slot: the notes after it play the default slot.
	f.tool("mute");
	f.click(f.at(2, 6.0));
	REQUIRE(f.app.waitUntil([&f] { return f.midiPart().at("controllers").at(0).value("muted", false); }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(inLane(" .articulation-item.direction.muted")).size() == 1; }));
	REQUIRE(f.app.waitUntil([&f] { return f.slot(f.notes[1]) == "Arco"; }));
	// A muted attribute too: the note no longer plays Staccato.
	CHECK(f.slot(f.notes[2]) == "Staccato");
	f.click(f.at(3, 9.0));
	REQUIRE(f.app.waitUntil(
		[&f] { return f.note(f.notes[2]).value("mutedArticulations", Json::array()) == Json{"Staccato"}; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(inLane(" .articulation-item.attribute.muted")).size() == 1; }));
	REQUIRE(f.app.waitUntil([&f] { return f.slot(f.notes[2]) == "Arco"; }));
	CHECK(f.articulations(f.notes[2]) == Json{"Staccato"});

	// Clicked again, they play again; one undo step each.
	f.click(f.at(3, 9.0));
	REQUIRE(f.app.waitUntil([&f] { return !f.note(f.notes[2]).contains("mutedArticulations"); }));
	f.click(f.at(2, 6.0));
	REQUIRE(f.app.waitUntil([&f] { return !f.midiPart().at("controllers").at(0).contains("muted"); }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(muted).empty(); }));
	REQUIRE(f.app.waitUntil([&f] { return f.slot(f.notes[1]) == "Pizzicato"; }));
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.elements(muted).size() == 1; }));
}

TEST_CASE("a direction on the Articulations lane lasts on through a muted one, as it plays", "[ui]")
{
	// #1762, #1799, reference manual p. 1289.
	Editor f;
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 4 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}},
					{{"position", 8 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Arco"},
						{"muted", true}}}}});
	const std::string pizzicato = inLane(" .articulation-item.direction:not(.muted)");
	const std::string arco = inLane(" .articulation-item.direction.muted");
	REQUIRE(f.app.waitUntil([&] { return f.elements(pizzicato).size() == 1 && f.elements(arco).size() == 1; }));
	const Json area = f.box(inLane(" .controller-lane-area"));
	const double end = area.at("x").get<double>() + area.at("width").get<double>();
	const auto right = [&f](const std::string& selector)
	{ return f.box(selector).at("x").get<double>() + f.box(selector).at("width").get<double>(); };
	// Pizzicato goes on to the part's end, and the muted Arco keeps its own span there, in gray.
	CHECK(std::abs(right(pizzicato) - end) <= 1.0);
	CHECK(std::abs(right(arco) - end) <= 1.0);
	CHECK(std::abs(f.box(arco).at("x").get<double>() -
			  (area.at("x").get<double>() + (area.at("width").get<double>() / 2.0))) <= 1.0);
	REQUIRE(f.app.waitUntil([&f] { return f.slot(f.notes[2]) == "Pizzicato"; }));
}

TEST_CASE("the Articulations lane's attribute selection is the editor view's, and Shift removes one of all", "[ui]")
{
	// #1688, reference manual p. 1289.
	Editor f;
	Json map = f.app.call("project.get").at("articulationMaps").at(0);
	map["groups"].push_back("Dynamics");
	map["articulations"].push_back({{"name", "Accent"}, {"type", "attribute"}, {"group", 2}});
	f.app.call("articulationMap.set",
		{{"map", map.at("id")}, {"groups", map.at("groups")}, {"articulations", map.at("articulations")}});
	f.app.call(
		"note.set", {{"part", f.part}, {"notes", {{{"id", f.notes[1]}, {"articulations", {"Staccato", "Accent"}}}}}});
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.attribute")).size() == 2; }));
	const auto selected = [&f](const char* row)
	{
		return f
			.elements(inLane(" .articulation-item.attribute.selected[articulation-row=\"" + std::string(row) + "\"]"))
			.size();
	};
	const auto selection = [&f] { return f.app.call("editor.get", {{"editor", "window"}}).at("selection"); };

	// A click selects the attribute pressed with its note: the view names it.
	f.click(f.at(3, 5.0));
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 1 && selected("3") == 0; }));
	CHECK(selection() == Json::array({{{"part", f.part}, {"note", f.notes[1]}, {"articulations", {"Staccato"}}}}));
	// The note selected again elsewhere, as the note display selects it, has all its attributes selected.
	f.app.call("editor.set", {{"editor", "window"}, {"selection", {{{"part", f.part}, {"note", f.notes[1]}}}}});
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 1 && selected("3") == 1; }));
	// Shift removes the one pressed of them; the note stays selected with the other one.
	f.app.ui().click(f.window, f.at(3, 5.0), ui::MouseButton::Left, 1, {"shift"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 0 && selected("3") == 1; }));
	CHECK(selection() == Json::array({{{"part", f.part}, {"note", f.notes[1]}, {"articulations", {"Accent"}}}}));
	// Lua and MCP select one attribute the same way. The lane has the keyboard focus from the press, which its state
	// class .focused shows, a frame around the whole lane in the themes, as around the zones (p. 100): Delete removes
	// that attribute.
	f.app.call("editor.set",
		{{"editor", "window"},
			{"selection", {{{"part", f.part}, {"note", f.notes[1]}, {"articulations", {"Staccato"}}}}}});
	REQUIRE(f.app.waitUntil([&] { return selected("2") == 1 && selected("3") == 0; }));
	REQUIRE(f.app.waitUntil([&] { return f.shown(inLane(".focused")); }));
	f.app.ui().key(f.window, "Delete");
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[1]) == Json{"Accent"}; }));
}

TEST_CASE("a selection change in the note display keeps the lane's attributes selected of the notes that stay", "[ui]")
{
	// #1762, reference manual p. 1289.
	Editor f;
	f.app.call("note.set", {{"part", f.part}, {"notes", {{{"id", f.notes[1]}, {"articulations", {"Staccato"}}}}}});
	f.app.call("editor.set",
		{{"editor", "window"},
			{"selection", {{{"part", f.part}, {"note", f.notes[1]}, {"articulations", {"Staccato"}}}}}});
	const std::string note = ".key-note[note=\"" + std::to_string(f.notes[0]) + "\"]";
	REQUIRE(f.app.waitUntil([&] { return !f.elements(note).empty(); }));
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.attribute.selected")).size() == 1; }));
	// Shift and a click on another note adds it; the one selected on the lane keeps its entry.
	f.app.ui().click(f.window, of(note), ui::MouseButton::Left, 1, {"shift"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return f.app.call("editor.get", {{"editor", "window"}}).at("selection") ==
				Json::array({{{"part", f.part}, {"note", f.notes[1]}, {"articulations", {"Staccato"}}},
					{{"part", f.part}, {"note", f.notes[0]}}});
		}));
}

TEST_CASE("a reset's arrow lies beside it, so that a press on the reset selects it", "[ui]")
{
	// #1688, reference manual p. 1290.
	Editor f;
	const auto reset = f.app
						   .call("controller.add",
							   {{"part", f.part},
								   {"controllers",
									   {{{"position", 8 * g_sixteenth}, {"type", "articulation"},
										   {"articulation", "Pizzicato"}, {"reset", true}}}}})
						   .at("controllers")
						   .at(0)
						   .at("id")
						   .get<uint64_t>();
	REQUIRE(f.app.waitUntil([&f] { return f.elements(inLane(" .articulation-item.reset")).size() == 1; }));
	const Json box = f.box(inLane(" .articulation-item.reset"));
	const Json arrow = f.box(inLane(" .articulation-item.reset .articulation-menu-arrow"));
	CHECK(arrow.at("x").get<double>() >= box.at("x").get<double>() + box.at("width").get<double>() - 0.5);
	// A press in the reset's upper part, where the arrow was, selects it.
	const ui::Point upper{
		static_cast<int>(std::lround(box.at("x").get<double>() + (box.at("width").get<double>() / 2.0))),
		static_cast<int>(std::lround(box.at("y").get<double>() + 2.0))};
	f.click({"", 0, upper});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return f.app.call("editor.get", {{"editor", "window"}}).at("controllerSelection") ==
				Json::array({{{"part", f.part}, {"controller", reset}}});
		}));
	CHECK_FALSE(f.shown("#controller-articulation-menu"));
	// Its arrow, beside it, opens the menu.
	const std::string shownArrow = inLane(" .articulation-item.reset.selected .articulation-menu-arrow");
	REQUIRE(f.app.waitUntil([&] { return f.shown(shownArrow); }));
	f.click(of(shownArrow));
	REQUIRE(f.app.waitUntil([&f] { return f.shown("#controller-articulation-menu"); }));
}

TEST_CASE("the list editor's Attributes cells open the menu for the row clicked, and not while edited", "[ui]")
{
	// #1688, reference manual p. 1291.
	Editor f("list");
	const auto cell = [](uint64_t note)
	{ return ".list-editor-rows .list-row[note=\"" + std::to_string(note) + "\"] textdisplay[field=attributes]"; };
	const std::string menu = "#list-articulations-menu";
	const std::string staccato = menu + " menuitem[articulation=Staccato]";
	REQUIRE(f.app.waitUntil([&] { return !f.elements(cell(f.notes[1])).empty(); }));
	f.click(of(cell(f.notes[2])));
	REQUIRE(f.app.waitUntil([&] { return f.shown(staccato); }));
	// A click on another row's cell, above the menu, opens it for that row, under it.
	f.click(of(cell(f.notes[0])));
	REQUIRE(f.app.waitUntil([&] { return f.shown(staccato); }));
	CHECK(std::abs(f.box(menu).at("y").get<double>() -
			  (f.box(cell(f.notes[0])).at("y").get<double>() + f.box(cell(f.notes[0])).at("height").get<double>())) <=
		1.0);
	f.click(of(staccato));
	REQUIRE(f.app.waitUntil([&f] { return f.articulations(f.notes[0]) == Json{"Staccato"}; }));
	CHECK(f.articulations(f.notes[2]).empty());

	// A second click on the same cell closes the menu, also after the rows rendered anew while it was open (#1799).
	f.click(of(cell(f.notes[2])));
	REQUIRE(f.app.waitUntil([&] { return f.shown(staccato); }));
	f.app.call("note.add",
		{{"part", f.part},
			{"notes", {{{"position", 12 * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 62}, {"velocity", 0.5}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".list-editor-rows .list-row[note]").size() == 4; }));
	CHECK(f.shown(staccato));
	f.click(of(cell(f.notes[2])));
	REQUIRE(f.app.waitUntil([&] { return !f.shown(menu); }));

	// While its text is edited, clicks into it don't open the menu.
	REQUIRE(f.app.waitUntil([&] { return !f.shown(menu); }));
	f.click(of(cell(f.notes[2])), 2);
	REQUIRE(f.app.waitUntil([&] { return !f.elements(cell(f.notes[2]) + ".editing").empty(); }));
	CHECK_FALSE(f.shown(menu));
	f.click(of(cell(f.notes[2]) + ".editing"));
	f.click(of(cell(f.notes[2]) + ".editing"));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(f.shown(menu));
}

TEST_CASE("the event colors menu's Sound Slot colors the notes by the slot they play", "[ui]")
{
	// Reference manual pp. 1134, 1269: each note in the color of the sound slot it plays, the theme's for a slot
	// without one.
	Editor f;
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 8 * g_sixteenth}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
	f.app.call("note.set", {{"part", f.part}, {"notes", {{{"id", f.notes[0]}, {"articulations", {"Staccato"}}}}}});
	const auto color = [&f](uint64_t note)
	{
		const Json found = f.elements(".key-note[note=\"" + std::to_string(note) + "\"]");
		return found.empty() ? std::string("none") : found.at(0).at("attributes").value("color", std::string());
	};
	REQUIRE(f.app.waitUntil([&f] { return f.slot(f.notes[0]) == "Staccato" && f.slot(f.notes[2]) == "Pizzicato"; }));
	CHECK(color(f.notes[1]) != "#2040a0"); // at first in its velocity's color
	f.click(of("#key-event-colors"));
	f.click(of("#key-event-colors-menu menuitem[event-colors=soundSlot]"));
	REQUIRE(f.app.waitUntil([&] { return color(f.notes[1]) == "#2040a0"; }));
	CHECK(color(f.notes[2]) == "#c04020");
	CHECK(color(f.notes[0]).empty());
	// The slot follows the note's articulations.
	f.app.call("note.set", {{"part", f.part}, {"notes", {{{"id", f.notes[0]}, {"articulations", Json::array()}}}}});
	REQUIRE(f.app.waitUntil([&] { return color(f.notes[0]) == "#2040a0"; }));
}
