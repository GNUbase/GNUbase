// SPDX-License-Identifier: GPL-3.0-or-later
// The controller lanes in the app (#128, docs/features/controller-lanes.md), driven with ui::Automation as MCP's UI
// tools drive them: the velocity lane's bars, controller lanes made from the menus, the tools on the lanes, the line
// tool's modes, the lane menus' functions, Auto Select Controllers, lane presets, and playback of what was drawn.
// tests/controllerlanes_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "uiapp.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
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

	// An instrument track with a one-bar part at the project start, open in the editor window.
	struct Fixture
	{
		Fixture()
		{
			track = app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
			part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}, {"name", "Keys"}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
			app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part})}});
			REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
			window = app.appUi().editorWindowId();
			REQUIRE(app.waitUntil([&] { return elements(".controller-lane-area").size() == 1; }));
		}

		Json elements(const std::string& selector) { return app.ui().dom(window, selector, 0).at("elements"); }

		Json box(const std::string& selector) { return elements(selector).at(0).at("box"); }

		static std::string area(int lane) { return R"(.controller-lane-area[lane=")" + std::to_string(lane) + R"("])"; }

		// Adds notes a sixteenth long at the sixteenths, velocity 0.5; returns their ids.
		std::vector<uint64_t> add(const std::vector<int>& sixteenths)
		{
			Json items = Json::array();
			for (const int at : sixteenths)
				items.push_back(
					{{"position", at * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 60}, {"velocity", 0.5}});
			const Json added = app.call("note.add", {{"part", part}, {"notes", items}});
			std::vector<uint64_t> ids;
			for (const Json& note : added.at("notes"))
				ids.push_back(note.at("id").get<uint64_t>());
			const size_t count = midiPart().at("notes").size();
			REQUIRE(app.waitUntil([&] { return elements(area(0) + " .controller-bar").size() == count; }));
			return ids;
		}

		Json midiPart()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == part)
					return each;
			return nullptr;
		}

		Json note(uint64_t id)
		{
			const Json found = midiPart();
			for (const Json& each : found.at("notes"))
				if (each.at("id") == id)
					return each;
			return nullptr;
		}

		// The part's controller events by position.
		std::vector<Json> controllers()
		{
			const Json found = midiPart().at("controllers");
			std::vector<Json> sorted(found.begin(), found.end());
			std::sort(sorted.begin(), sorted.end(),
				[](const Json& a, const Json& b) { return a.at("position") < b.at("position"); });
			return sorted;
		}

		Json view() { return app.call("editor.get", {{"editor", "window"}}); }

		// A point of a lane: at sixteenths from the project start, at a value from 0 (the lane's bottom) to 1.
		ui::Target at(int lane, double sixteenths, double value)
		{
			const Json shown = box(area(lane));
			const double x = shown.at("x").get<double>() + (sixteenths / 16.0 * shown.at("width").get<double>());
			const double y = shown.at("y").get<double>() + ((1.0 - value) * (shown.at("height").get<double>() - 1.0));
			return {"", 0, ui::Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}};
		}

		void click(const ui::Target& target, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(window, target, ui::MouseButton::Left, 1, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void drag(const ui::Target& from, const ui::Target& to)
		{
			app.ui().drag(window, from, to);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void tool(const std::string& name)
		{
			click(of(".key-tool[key-tool=" + name + "]"));
			REQUIRE(app.waitUntil([&] { return view().is_object(); }));
		}

		// Opens a menu with its button and clicks the item.
		void choose(const std::string& button, const std::string& item)
		{
			click(of(button));
			REQUIRE(app.waitUntil([&] { return !elements(item).empty(); }));
			click(of(item));
		}

		std::vector<Json> lanes() { return view().at("lanes").get<std::vector<Json>>(); }

		test::UiApp app{{1200, 800}};
		std::optional<ui::WindowId> window;
		uint64_t track = 0;
		uint64_t part = 0;
	};
}

TEST_CASE("the velocity lane shows the notes' velocities as bars that the tools change", "[ui]")
{
	Fixture f;
	CHECK(f.elements(".controller-lane-type").at(0).at("text") == "Velocity");
	const std::vector<uint64_t> ids = f.add({0, 4, 8, 12});
	// Each bar at its note's time, as high as its velocity.
	const Json lane = f.box(Fixture::area(0));
	const Json second = f.box(".controller-bar[note=\"" + std::to_string(ids[1]) + "\"]");
	CHECK(std::abs(second.at("x").get<double>() - lane.at("x").get<double>() - (lane.at("width").get<double>() / 4)) <=
		1);
	CHECK(std::abs(second.at("height").get<double>() - (lane.at("height").get<double>() / 2)) <= 1);

	// A drag on a bar with the object selection tool changes its velocity by as much as the mouse went.
	f.drag(f.at(0, 4.1, 0.5), f.at(0, 4.1, 0.75));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).at("velocity").get<double>() > 0.7; }));
	CHECK(f.note(ids[1]).at("velocity").get<double>() == Approx(0.75).margin(0.03));
	CHECK(f.note(ids[0]).at("velocity").get<double>() == Approx(0.5).margin(1e-4));

	// The draw tool paints the velocities of the notes it goes over.
	f.tool("draw");
	f.drag(f.at(0, 7, 1.0), f.at(0, 13, 1.0));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[3]).at("velocity").get<double>() > 0.95; }));
	CHECK(f.note(ids[2]).at("velocity").get<double>() > 0.95);
	CHECK(f.note(ids[0]).at("velocity").get<double>() == Approx(0.5).margin(1e-4));

	// The line tool draws a ramp over them.
	f.tool("line");
	f.drag(f.at(0, 0, 0.0), f.at(0, 12, 0.75));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("velocity").get<double>() < 0.05; }));
	CHECK(f.note(ids[1]).at("velocity").get<double>() == Approx(0.25).margin(0.03));
	CHECK(f.note(ids[3]).at("velocity").get<double>() == Approx(0.75).margin(0.03));
}

TEST_CASE("controller lanes come from the menus and the tools add, select, change and erase events", "[ui]")
{
	Fixture f;
	f.add({0});
	// Create Controller Lane adds a lane of the type chosen.
	f.choose("#controller-lane-create", R"(#controller-create-menu menuitem[lane-type="controller"][lane-number="1"])");
	REQUIRE(f.app.waitUntil([&] { return f.lanes().size() == 2; }));
	CHECK(f.lanes()[1] == Json{{"type", "controller"}, {"number", 1}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(1)).size() == 1; }));
	CHECK(f.elements(".controller-lane-type").at(1).at("text") == "Modulation (CC 1)");

	// The draw tool adds an event at the snapped position, with the value at the mouse.
	f.tool("draw");
	f.click(f.at(1, 4.5, 0.5));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 1; }));
	Json event = f.controllers()[0];
	CHECK(event.at("number") == 1);
	CHECK(event.at("position") == 4 * g_sixteenth);
	CHECK(event.at("value").get<double>() == Approx(0.5).margin(0.03));
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(1) + " .controller-step").size() == 1; }));
	// A drag paints an event every length quantize step, replacing those there.
	f.drag(f.at(1, 2.5, 0.25), f.at(1, 6.5, 0.25));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 5; }));
	for (const Json& each : f.controllers())
		CHECK(each.at("value").get<double>() == Approx(0.25).margin(0.03));
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(1) + " .controller-step").size() == 5; }));

	// Object selection: a click selects an event, a drag changes the selected events' values.
	f.tool("objectSelection");
	const uint64_t third = f.controllers()[2].at("id").get<uint64_t>();
	f.click(f.at(1, 4.5, 0.1));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("controllerSelection").size() == 1; }));
	CHECK(f.view().at("controllerSelection").at(0).at("controller") == third);
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json found = f.elements(".controller-step.selected");
			return found.size() == 1;
		}));
	f.drag(f.at(1, 4.5, 0.1), f.at(1, 4.5, 0.6));
	REQUIRE(f.app.waitUntil([&] { return f.controllers()[2].at("value").get<double>() > 0.7; }));
	CHECK(f.controllers()[1].at("value").get<double>() == Approx(0.25).margin(0.03));

	// The erase tool removes the event clicked.
	f.tool("erase");
	f.click(f.at(1, 2.5, 0.1));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 4; }));
}

TEST_CASE("the line tool's modes draw lines, parabolas and periodic curves on a controller lane", "[ui]")
{
	Fixture f;
	// The quantize grid, the period of the periodic modes below, comes with the project that the editor reads after
	// quantize.changed. It is set first, so that the project showing the line's events has it.
	f.app.call("quantize.set", {{"settings", {{"grid", "1/4"}}}});
	f.app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "pitchBend"}}}}});
	f.app.call("editor.set", {{"editor", "window"}, {"lengthQuantize", "1/16"}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".controller-lane-type").at(0).at("text") == "Pitchbend"; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#key-length-quantize").at(0).at("text") == "1/16"; }));
	f.tool("line");
	f.drag(f.at(0, 0, 0.0), f.at(0, 8, 1.0));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 9; })); // every sixteenth, and the end
	CHECK(f.controllers()[4].at("type") == "pitchBend");
	CHECK(f.controllers()[4].at("value").get<double>() == Approx(0.5).margin(0.05));
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(0) + " .controller-step").size() == 9; }));

	// A second click on the line tool opens its modes. Square goes from the start value to the end value and back
	// every period, the quantize grid; the length quantize sets the density of the events.
	f.app.call("editor.set", {{"editor", "window"}, {"lengthQuantize", "1/8"}});
	REQUIRE(f.app.waitUntil([&] { return f.elements("#key-length-quantize").at(0).at("text") == "1/8"; }));
	f.choose(".key-tool[key-tool=line]", "#key-line-menu menuitem[line-mode=square]");
	f.drag(f.at(0, 0, 0.0), f.at(0, 8, 1.0));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 5; })); // every eighth
	std::vector<double> values;
	for (const Json& each : f.controllers())
		values.push_back(std::round(each.at("value").get<double>()));
	CHECK(values == std::vector<double>{0.0, 1.0, 0.0, 1.0, 0.0});
}

TEST_CASE("the lane menus change and remove lanes, select their events and apply play probability", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({0, 4});
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 0}, {"number", 7}, {"value", 0.5}},
					{{"position", g_quarter}, {"type", "channelPressure"}, {"value", 0.5}}}}});
	f.app.call(
		"editor.set", {{"editor", "window"}, {"lanes", {{{"type", "velocity"}}, {{"type", "channelPressure"}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(1) + " .controller-step").size() == 1; }));
	// Show Used Controllers adds a lane for every kind of event used that has none.
	f.choose("#controller-lane-create", "#controller-create-menu menuitem[lane-action=showUsed]");
	REQUIRE(f.app.waitUntil([&] { return f.lanes().size() == 3; }));
	CHECK(f.lanes()[2] == Json{{"type", "controller"}, {"number", 7}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".controller-lane-type").size() == 3; }));

	// A lane's menu changes its type, selects its events and removes it.
	f.choose(".controller-lane-type[lane-menu=\"2\"]", "#controller-lane-menu menuitem[lane-type=probability]");
	REQUIRE(f.app.waitUntil([&] { return f.lanes()[2].at("type") == "probability"; }));
	f.choose(".controller-lane-type[lane-menu=\"1\"]", "#controller-lane-menu menuitem[lane-action=selectAll]");
	REQUIRE(f.app.waitUntil([&] { return f.view().at("controllerSelection").size() == 1; }));
	f.choose(".controller-lane-type[lane-menu=\"1\"]", "#controller-lane-menu menuitem[lane-action=remove]");
	REQUIRE(f.app.waitUntil([&] { return f.lanes().size() == 2; }));
	CHECK(f.lanes()[1].at("type") == "probability");

	// Play probability drawn on its lane, then applied: notes of probability 0 go.
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(1) + " .controller-bar").size() == 2; }));
	f.tool("draw");
	f.click(f.at(1, 4.1, 0.0));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).value("probability", 1.0) < 0.05; }));
	f.choose(".controller-lane-type[lane-menu=\"1\"]", "#controller-lane-menu menuitem[lane-action=applyProbability]");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).is_null(); }));
	CHECK_FALSE(f.note(ids[0]).is_null());
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(1) + " .controller-bar").size() == 1; }));
	// Reset sets it back to 100 %.
	f.app.call("note.set", {{"part", f.part}, {"notes", {{{"id", ids[0]}, {"probability", 0.5}}}}});
	f.choose(".controller-lane-type[lane-menu=\"1\"]", "#controller-lane-menu menuitem[lane-action=resetProbability]");
	REQUIRE(f.app.waitUntil([&] { return !f.note(ids[0]).contains("probability"); }));

	// Show/Hide Controller Lanes, and Show Velocity Lane Only.
	f.choose("#controller-lane-create", "#controller-create-menu menuitem[lane-action=toggle]");
	REQUIRE(f.app.waitUntil([&] { return f.view().at("showLanes") == false; }));
	f.choose("#controller-lane-create", "#controller-create-menu menuitem[lane-action=velocityOnly]");
	REQUIRE(f.app.waitUntil([&] { return f.view().at("showLanes") == true && f.lanes().size() == 1; }));
	CHECK(f.lanes()[0].at("type") == "velocity");
}

TEST_CASE("Auto Select Controllers selects the controllers of the notes selected, and they move with them", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({0, 8});
	const Json added = f.app.call("controller.add",
		{{"part", f.part}, {"controllers", {{{"position", g_sixteenth / 2}, {"number", 1}, {"value", 0.5}}}}});
	const uint64_t controller = added.at("controllers").at(0).at("id").get<uint64_t>();
	f.click(of(".key-input[setting=autoSelectControllers]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("autoSelectControllers") == true; }));
	f.app.call("editor.set", {{"editor", "window"}, {"selection", {{{"part", f.part}, {"note", ids[0]}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("controllerSelection").size() == 1; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note.selected").size() == 1; }));
	// Moving the note in the note display moves its controller event too (p. 1188).
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow()->keyEditor()->scrollToPitch(60); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json grid = f.box(".key-editor-grid");
	const auto point = [&](double sixteenths)
	{
		const double x = grid.at("x").get<double>() + (sixteenths / 16.0 * grid.at("width").get<double>());
		const double y = grid.at("y").get<double>() + (67.5 / 128.0 * grid.at("height").get<double>());
		return ui::Target{"", 0, ui::Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}};
	};
	f.drag(point(0.5), point(4.5));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("position") == g_quarter; }));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().at(0).at("position") == g_quarter + (g_sixteenth / 2); }));
	CHECK(f.controllers().at(0).at("id") == controller);
}

TEST_CASE("controller lane presets save the lanes shown and bring them back", "[ui]")
{
	Fixture f;
	f.app.call("settings.set", {{"key", "ui.controllerLanePresets"}, {"value", Json::array()}});
	f.app.call("editor.set",
		{{"editor", "window"},
			{"lanes", {{{"type", "velocity"}}, {{"type", "pitchBend"}}, {{"type", "controller"}, {"number", 11}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".controller-lane-type").size() == 3; }));
	f.choose("#controller-lane-setup", "#controller-setup-menu menuitem[lane-action=addPreset]");
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#lane-preset-name").empty(); }));
	f.app.ui().text(f.window, "Expressive");
	f.app.ui().key(f.window, "Enter");
	const auto presets = [&] { return f.app.call("settings.get", {{"key", "ui.controllerLanePresets"}}).at("value"); };
	REQUIRE(f.app.waitUntil([&] { return presets().size() == 1; }));
	CHECK(presets().at(0).at("name") == "Expressive");
	CHECK(presets().at(0).at("lanes").size() == 3);

	f.app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "velocity"}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".controller-lane-type").size() == 1; }));
	f.choose("#controller-lane-setup", "#controller-setup-menu menuitem[lane-preset=Expressive]");
	REQUIRE(f.app.waitUntil([&] { return f.lanes().size() == 3; }));
	CHECK(f.lanes()[2] == Json{{"type", "controller"}, {"number", 11}});
}

TEST_CASE("controller lanes that are gone before the presets' answer comes leave the answer alone", "[ui]")
{
	Fixture f;
	f.app.call("settings.set",
		{{"key", "ui.controllerLanePresets"},
			{"value", Json::parse(R"([{"name": "Expressive", "lanes": [{"type": "pitchBend"}]}])")}});
	// All in one step: the list editor, the key editor with new lanes, which ask for the presets, and the list editor
	// again, which takes the lanes away before the answer is handed on.
	for (const char* content : {"list", "key", "list"})
		f.app.call("editor.set", {{"editor", "window"}, {"content", content}});
	REQUIRE(f.app.waitUntil(
		[&] { return !f.elements(".list-editor").empty() && f.elements(".controller-lane-area").empty(); }));
	f.app.waitUntil([] { return false; }, std::chrono::milliseconds(300)); // time for the answers to be handed on
	f.app.call("editor.set", {{"editor", "window"}, {"content", "key"}});
	CHECK(f.app.waitUntil([&] { return f.elements(".controller-lane-area").size() == 1; }));
}

TEST_CASE("controller events drawn on a lane play back", "[ui]")
{
	Fixture f;
	f.app.call("audio.setDevice", {{"blockSize", 128}, {"inputChannels", 0}, {"captureSeconds", 1}});
	f.app.call("channel.set",
		{{"channel", f.app.call("project.get").at("tracks").at(0).at("channel")}, {"instrument", "testSynth"}});
	f.app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "controller"}, {"number", 7}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".controller-lane-type").at(0).at("text") == "Volume (CC 7)"; }));
	f.tool("draw");
	f.click(f.at(0, 0.5, 1.0));
	f.click(f.at(0, 2.5, 0.0));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 2; }));
	f.app.call("midi.capture", {{"start", true}});
	f.app.call("transport.play");
	f.app.call("audio.render", {{"frames", 13000}}); // an eighth is 12000 frames at 120 BPM
	std::vector<int> volumes;
	const Json captured = f.app.call("midi.capture");
	for (const Json& message : captured.at("captured"))
	{
		const auto bytes = message.at("bytes").get<std::vector<int>>();
		if ((bytes[0] & 0xF0) == 0xB0 && bytes[1] == 7)
			volumes.push_back(bytes[2]);
	}
	CHECK(volumes == std::vector<int>{127, 0});
	f.app.call("transport.stop");
}
