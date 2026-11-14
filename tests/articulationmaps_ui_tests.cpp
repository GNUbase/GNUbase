// SPDX-License-Identifier: GPL-3.0-or-later
// The articulation map setup and the inspector's Articulation Map section in the app (#175,
// docs/features/articulation-maps.md), driven with ui::Automation as MCP's UI tools drive them.
// tests/articulationmaps_tests.cpp checks the maps in the engine.
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
