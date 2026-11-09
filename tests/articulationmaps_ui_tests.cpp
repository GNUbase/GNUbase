// SPDX-License-Identifier: GPL-3.0-or-later
// The articulation map setup and the inspector's Articulation Map section in the app (#175,
// docs/features/articulation-maps.md), driven with ui::Automation as MCP's UI tools drive them.
// tests/articulationmaps_tests.cpp checks the maps in the engine.
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>

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
