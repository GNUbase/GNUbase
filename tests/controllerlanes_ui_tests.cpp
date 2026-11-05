// SPDX-License-Identifier: GPL-3.0-or-later
// The controller lanes in the app (#128, docs/features/controller-lanes.md), driven with ui::Automation as MCP's UI
// tools drive them: the velocity lane's bars, controller lanes made from the menus, the tools on the lanes, the line
// tool's modes, the lane menus' functions, Auto Select Controllers, lane presets, and playback of what was drawn.
// tests/controllerlanes_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>
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

		// Whether the element shows: menus are in the document also while they're closed.
		bool visible(const std::string& selector)
		{
			const Json found = elements(selector);
			return !found.empty() && found.at(0).at("visible") == true;
		}

		// Opens a menu with its button and clicks the item.
		void choose(const std::string& button, const std::string& item)
		{
			click(of(button));
			REQUIRE(app.waitUntil([&] { return visible(item); }));
			click(of(item));
		}

		size_t steps() { return app.call("history.get").at("steps").size(); }

		// The editor window's context: through it a test presses a button and keeps it down, as a mouseup lost
		// outside the window leaves it, where RmlUi has no element to send the mouseup to.
		Rml::Context& context()
		{
			const ui::Window* shown = app.shell().find(app.appUi().editorWindowId());
			if (shown == nullptr)
				throw std::logic_error("the editor window doesn't show");
			return shown->context();
		}

		void press(const ui::Target& target)
		{
			const ui::Point point = target.point.value_or(ui::Point{});
			context().ProcessMouseMove(point.x, point.y, 0);
			context().ProcessMouseButtonDown(0, 0);
			app.shell().step(std::chrono::milliseconds(0));
		}

		// The right end of the editor window's toolbar's empty space, where a click does nothing.
		ui::Target spare()
		{
			const Json shown = box(".midi-editor-toolbar .spacer");
			return {"", 0,
				ui::Point{
					static_cast<int>(std::lround(shown.at("x").get<double>() + shown.at("width").get<double>() - 5)),
					static_cast<int>(
						std::lround(shown.at("y").get<double>() + (shown.at("height").get<double>() / 2)))}};
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

	// A second click on the line tool opens its modes, whose menu is in the document but closed until then. Square
	// goes from the start value to the end value and back every period, the quantize grid; the length quantize sets
	// the density of the events.
	f.app.call("editor.set", {{"editor", "window"}, {"lengthQuantize", "1/8"}});
	REQUIRE(f.app.waitUntil([&] { return f.elements("#key-length-quantize").at(0).at("text") == "1/8"; }));
	CHECK(!f.visible("#key-line-menu menuitem[line-mode=square]"));
	f.choose(".key-tool[key-tool=line]", "#key-line-menu menuitem[line-mode=square]");
	f.drag(f.at(0, 0, 0.0), f.at(0, 8, 1.0));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 5; })); // every eighth
	std::vector<double> values;
	for (const Json& each : f.controllers())
		values.push_back(std::round(each.at("value").get<double>()));
	CHECK(values == std::vector<double>{0.0, 1.0, 0.0, 1.0, 0.0});

	// Parabola, sine and triangle with an event every 1/32 (half a sixteenth): the values at a thirty-second, a
	// sixteenth and a quarter tell them apart.
	f.app.call("editor.set", {{"editor", "window"}, {"lengthQuantize", "1/32"}});
	REQUIRE(f.app.waitUntil([&] { return f.elements("#key-length-quantize").at(0).at("text") == "1/32"; }));
	const auto valueAt = [&](int64_t position)
	{
		for (const Json& each : f.controllers())
			if (each.at("position") == position)
				return each.at("value").get<double>();
		return -1.0;
	};
	// Draws in the mode and waits until the value at the position shows that the line arrived.
	const auto mode = [&](const std::string& name, int64_t position, double expected)
	{
		f.choose(".key-tool[key-tool=line]", "#key-line-menu menuitem[line-mode=" + name + "]");
		f.drag(f.at(0, 0, 0.0), f.at(0, 8, 1.0));
		REQUIRE(f.app.waitUntil([&] { return std::abs(valueAt(position) - expected) < 0.01; }));
		CHECK(f.controllers().size() == 17);
	};
	mode("parabola", 4 * g_sixteenth, 0.25); // slowly first: a quarter of the way at half the time
	CHECK(valueAt(g_sixteenth) == Approx(1.0 / 64.0).margin(0.01));
	mode("sine", g_sixteenth / 2, 0.146); // up and back down once a quarter
	CHECK(valueAt(2 * g_sixteenth) == Approx(1.0).margin(0.01));
	CHECK(valueAt(4 * g_sixteenth) == Approx(0.0).margin(0.01));
	mode("triangle", g_sixteenth / 2, 0.25);
	CHECK(valueAt(g_sixteenth) == Approx(0.5).margin(0.01));
	CHECK(valueAt(2 * g_sixteenth) == Approx(1.0).margin(0.01));

	// Paint paints like the draw tool: an event every length quantize step, replacing those there.
	const std::vector<Json> triangle = f.controllers();
	f.choose(".key-tool[key-tool=line]", "#key-line-menu menuitem[line-mode=paint]");
	f.drag(f.at(0, 0.25, 0.75), f.at(0, 4.75, 0.75));
	REQUIRE(f.app.waitUntil([&] { return std::abs(valueAt(g_sixteenth) - 0.75) < 0.03; }));
	CHECK(f.controllers().size() == 17);
	const std::vector<Json> painted = f.controllers();
	for (size_t i = 0; i < painted.size() && i < triangle.size(); ++i)
	{
		const auto position = painted[i].at("position").get<int64_t>();
		INFO("position " << position);
		CHECK(position == triangle[i].at("position"));
		const double expected = position <= 9 * (g_sixteenth / 2) ? 0.75 : triangle[i].at("value").get<double>();
		CHECK(painted[i].at("value").get<double>() == Approx(expected).margin(0.03));
	}
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

TEST_CASE(
	"a poly pressure lane takes the pitch of the first selected note, a program change lane draws programs", "[ui]")
{
	Fixture f;
	f.add({0});
	const Json added = f.app.call("note.add",
		{{"part", f.part},
			{"notes", {{{"position", g_quarter}, {"length", g_sixteenth}, {"pitch", 64}, {"velocity", 0.5}}}}});
	const uint64_t high = added.at("notes").at(0).at("id").get<uint64_t>();
	f.app.call("editor.set", {{"editor", "window"}, {"selection", {{{"part", f.part}, {"note", high}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note.selected").size() == 1; }));
	f.choose("#controller-lane-create", R"(#controller-create-menu menuitem[lane-type="polyPressure"])");
	REQUIRE(f.app.waitUntil([&] { return f.lanes().size() == 2; }));
	CHECK(f.lanes()[1] == Json{{"type", "polyPressure"}, {"number", 64}});
	// Its events are poly pressure of that pitch.
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(1)).size() == 1; }));
	f.tool("draw");
	f.click(f.at(1, 4.5, 0.5));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 1; }));
	CHECK(f.controllers()[0].at("type") == "polyPressure");
	CHECK(f.controllers()[0].at("number") == 64);
	CHECK(f.controllers()[0].at("value").get<double>() == Approx(0.5).margin(0.03));

	// On a program change lane the value is the program, 0 at the bottom, 127 at the top.
	f.app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "program"}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".controller-lane-type").at(0).at("text") == "Program Change"; }));
	f.click(f.at(0, 8.5, 1.0));
	f.click(f.at(0, 12.5, 0.5));
	const auto programs = [&]
	{
		std::vector<int> found;
		for (const Json& each : f.controllers())
			if (each.at("type") == "program")
				found.push_back(each.at("number").get<int>());
		return found;
	};
	REQUIRE(f.app.waitUntil([&] { return programs().size() == 2; }));
	CHECK(programs()[0] == 127);
	CHECK(std::abs(programs()[1] - 64) <= 2);
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(0) + " .controller-step").size() == 2; }));
}

TEST_CASE("Ctrl and the draw tool paint controller events only where the mouse went", "[ui]")
{
	Fixture f;
	f.app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "controller"}, {"number", 1}}}}});
	REQUIRE(
		f.app.waitUntil([&] { return f.elements(".controller-lane-type").at(0).at("text") == "Modulation (CC 1)"; }));
	f.tool("draw");
	// Without Ctrl an event every length quantize step, a sixteenth: three from the first sixteenth to the third.
	f.drag(f.at(0, 0.5, 0.5), f.at(0, 2.5, 0.5));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 3; }));
	// With Ctrl one where the mouse was pressed and one at each of the drag's eight moves, off the grid; those there
	// go.
	f.app.ui().drag(f.window, f.at(0, 0.5, 0.25), f.at(0, 2.5, 0.25), {"ctrl"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 9; }));
	for (const Json& each : f.controllers())
	{
		CHECK(each.at("value").get<double>() == Approx(0.25).margin(0.03));
		CHECK(each.at("position").get<int64_t>() > g_sixteenth / 4);
	}
	CHECK(f.controllers()[0].at("position").get<int64_t>() % g_sixteenth != 0);
}

TEST_CASE("Remove Preset removes a controller lane preset", "[ui]")
{
	Fixture f;
	f.app.call("settings.set",
		{{"key", "ui.controllerLanePresets"},
			{"value", Json::parse(R"([{"name": "Expressive", "lanes": [{"type": "pitchBend"}]},
				{"name": "Plain", "lanes": [{"type": "velocity"}]}])")}});
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#controller-remove-preset").empty(); }));
	f.click(of("#controller-lane-setup"));
	REQUIRE(f.app.waitUntil([&] { return f.visible("#controller-remove-preset"); }));
	f.app.ui().move(f.window, of("#controller-remove-preset"));
	f.app.shell().step(std::chrono::milliseconds(0));
	const std::string expressive = "#controller-setup-menu menuitem[remove-preset=Expressive]";
	REQUIRE(f.app.waitUntil([&] { return f.visible(expressive); }));
	f.click(of(expressive));
	const auto presets = [&] { return f.app.call("settings.get", {{"key", "ui.controllerLanePresets"}}).at("value"); };
	REQUIRE(f.app.waitUntil([&] { return presets().size() == 1; }));
	CHECK(presets().at(0).at("name") == "Plain");
	// The menu lists what's left.
	REQUIRE(
		f.app.waitUntil([&] { return f.elements("#controller-setup-menu menuitem[lane-preset=Expressive]").empty(); }));
	CHECK(f.elements("#controller-setup-menu menuitem[lane-preset=Plain]").size() == 1);
}

TEST_CASE("Apply and Reset MIDI Velocity Variance change the notes of every part as one undo step", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({0, 4});
	const uint64_t second = f.app.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
								.at("part")
								.at("id")
								.get<uint64_t>();
	const uint64_t third =
		f.app
			.call("note.add",
				{{"part", second},
					{"notes", {{{"position", 0}, {"length", g_sixteenth}, {"pitch", 60}, {"velocity", 0.5}}}}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	f.app.call("note.set",
		{{"part", f.part},
			{"notes", {{{"id", ids[0]}, {"velocityVariance", 0.5}}, {{"id", ids[1]}, {"velocityVariance", 0.5}}}}});
	f.app.call("note.set", {{"part", second}, {"notes", {{{"id", third}, {"velocityVariance", 0.5}}}}});
	f.app.call("editor.open", {{"editor", "window"}, {"parts", {f.part, second}}});
	f.app.call(
		"editor.set", {{"editor", "window"}, {"lanes", {{{"type", "velocity"}}, {{"type", "velocityVariance"}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(1) + " .controller-bar").size() == 3; }));
	// The notes of both parts that have a velocity variance, and their velocities.
	const auto varied = [&]
	{
		size_t count = 0;
		for (const Json& each : f.app.call("project.get").at("midiParts"))
			for (const Json& note : each.at("notes"))
				count += note.contains("velocityVariance") ? 1 : 0;
		return count;
	};
	const auto velocities = [&]
	{
		std::vector<double> found;
		for (const Json& each : f.app.call("project.get").at("midiParts"))
			for (const Json& note : each.at("notes"))
				found.push_back(note.at("velocity").get<double>());
		return found;
	};
	REQUIRE(varied() == 3);

	// Apply gives the notes of both parts the velocity they play with, in one history.transaction: one undo step.
	f.choose(R"(.controller-lane-type[lane-menu="1"])", "#controller-lane-menu menuitem[lane-action=applyVariance]");
	REQUIRE(f.app.waitUntil([&] { return varied() == 0; }));
	for (const double velocity : velocities())
		CHECK(velocity >= 0.5 - 1e-4);
	f.app.call("history.undo");
	CHECK(varied() == 3);
	for (const double velocity : velocities())
		CHECK(velocity == Approx(0.5).margin(1e-4));

	// Reset sets the variance back to 0 and keeps the velocities, also one undo step.
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(1) + " .controller-bar").size() == 3; }));
	f.choose(R"(.controller-lane-type[lane-menu="1"])", "#controller-lane-menu menuitem[lane-action=resetVariance]");
	REQUIRE(f.app.waitUntil([&] { return varied() == 0; }));
	for (const double velocity : velocities())
		CHECK(velocity == Approx(0.5).margin(1e-4));
	f.app.call("history.undo");
	CHECK(varied() == 3);
}

TEST_CASE("the line tool and painting replace the events of every channel and keep the lane's channel", "[ui]")
{
	Fixture f;
	const auto event = [](int64_t position, int channel)
	{ return Json{{"position", position}, {"number", 1}, {"value", 0.5}, {"channel", channel}}; };
	f.app.call("controller.add",
		{{"part", f.part},
			{"controllers",
				{event(0, 2), event(2 * g_sixteenth, 2), event(4 * g_sixteenth, 2), event(6 * g_sixteenth, 3),
					event(12 * g_sixteenth, 2)}}});
	f.app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "controller"}, {"number", 1}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(0) + " .controller-step").size() == 5; }));
	const auto channels = [&]
	{
		std::vector<int> found;
		for (const Json& each : f.controllers())
			found.push_back(each.at("channel").get<int>());
		return found;
	};

	// A line over the first half bar: an event every sixteenth on the channel of the events there, and none of
	// another channel left in between.
	f.tool("line");
	f.drag(f.at(0, 0, 0.0), f.at(0, 8, 1.0));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 10; }));
	CHECK(channels() == std::vector<int>(10, 2));
	CHECK(f.controllers()[4].at("value").get<double>() == Approx(0.5).margin(0.03));
	// One undo step brings the events back.
	f.app.call("history.undo");
	CHECK(channels() == std::vector<int>({2, 2, 2, 3, 2}));

	// Painting where the lane has no events takes the channel of its first event in the part.
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(0) + " .controller-step").size() == 5; }));
	f.tool("draw");
	f.click(f.at(0, 14.5, 0.5));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 6; }));
	CHECK(f.controllers().back().at("channel") == 2);
	CHECK(f.controllers().back().at("position") == 14 * g_sixteenth);
}

TEST_CASE("a drag on a lane released outside the lanes ends there, and one whose mouseup never came at the next press",
	"[ui]")
{
	Fixture f;
	f.app.call("editor.set", {{"editor", "window"}, {"lanes", {{{"type", "controller"}, {"number", 1}}}}});
	f.app.call("controller.add",
		{{"part", f.part}, {"controllers", {{{"position", 4 * g_sixteenth}, {"number", 1}, {"value", 0.25}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(Fixture::area(0) + " .controller-step").size() == 1; }));
	const auto value = [&] { return f.controllers().at(0).at("value").get<double>(); };
	// Waits until the lanes' commands so far have arrived: a lane added from the menu comes after them.
	size_t lanes = 1;
	const auto settle = [&]
	{
		f.choose("#controller-lane-create", R"(#controller-create-menu menuitem[lane-type="pitchBend"])");
		++lanes;
		REQUIRE(f.app.waitUntil([&] { return f.lanes().size() == lanes; }));
	};

	// A value drag released above the lanes, over the window's toolbar, sets the value there: the top.
	f.drag(f.at(0, 4.5, 0.1), f.spare());
	REQUIRE(f.app.waitUntil([&] { return value() > 0.95; }));
	const size_t before = f.steps();
	// It has ended: a press outside the lanes and a release over them change nothing.
	f.drag(f.spare(), f.at(0, 4.5, 0.5));
	settle();
	CHECK(value() > 0.95);
	CHECK(f.steps() == before);

	// A paint whose mouseup never came (released outside the window) ends without its effect at the next press.
	f.tool("draw");
	f.press(f.at(0, 10.5, 0.5));
	f.click(f.spare());
	settle();
	CHECK(f.controllers().size() == 1);
	CHECK(f.steps() == before);
	// The draw tool paints again after it.
	f.click(f.at(0, 10.5, 0.5));
	REQUIRE(f.app.waitUntil([&] { return f.controllers().size() == 2; }));
}
