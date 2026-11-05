// SPDX-License-Identifier: GPL-3.0-or-later
// The list editor in the app (#130, docs/features/list-editor.md), driven with ui::Automation as MCP's UI tools drive
// it: Open List Editor, the event list with every event type, editing values of several events and SysEx, the filters
// bar and the Focus menu, the draw and erase tools, moving events and the value display, and gestures released outside
// the editor or never released. tests/listeditor_tests.cpp checks the engine's side.
#include "engine/midi/midivalues.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;
	constexpr double g_maxController = 4294967295.0;

	ui::Target of(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	double controller(int value)
	{
		return engine::midi::controllerFromMidi1(static_cast<uint8_t>(value)) / g_maxController;
	}

	std::string row(const std::string& kind, uint64_t id)
	{
		return ".list-editor-rows .list-row[" + kind + "=\"" + std::to_string(id) + "\"]";
	}

	// A MIDI track with a one-bar part at the project start, with a note and a controller event of each type, open in
	// the list editor through Open List Editor.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}, {"name", "Keys"}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
			const Json notes = app.call("note.add",
				{{"part", part},
					{"notes",
						Json::array({{{"position", g_quarter}, {"length", g_quarter}, {"pitch", 64},
										 {"velocity", 80.0 / 127.0}},
							{{"position", 0}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 64.0 / 127.0}}})}});
			high = notes.at("notes").at(0).at("id").get<uint64_t>();
			low = notes.at("notes").at(1).at("id").get<uint64_t>();
			const Json controllers = app.call("controller.add",
				{{"part", part},
					{"controllers",
						Json::array({{{"position", 0}, {"number", 7}, {"value", controller(100)}},
							{{"position", 2 * g_quarter}, {"type", "program"}, {"number", 5}, {"channel", 1}},
							{{"position", 2 * g_quarter}, {"type", "pitchBend"},
								{"value", engine::midi::pitchBendFromMidi1(8192 + 4096) / g_maxController}},
							{{"position", 3 * g_quarter}, {"type", "channelPressure"}, {"value", controller(50)}},
							{{"position", 3 * g_quarter}, {"type", "polyPressure"}, {"number", 62},
								{"value", controller(40)}},
							{{"position", (3 * g_quarter) + 1}, {"type", "sysEx"},
								{"data", {0xF0, 0x41, 0x10, 0xF7}}}})}});
			for (const Json& each : controllers.at("controllers"))
				events.push_back(each.at("id").get<uint64_t>());
			REQUIRE(app.waitUntil(
				[&] { return display.project().is_object() && display.project().at("midiParts").size() == 1; }));
			display.select({part});
			app.shell().step(std::chrono::milliseconds(0));
			app.keys().run("editor.openList", Json::object(), app.main(), nullptr);
			REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
			window = app.appUi().editorWindowId();
			REQUIRE(app.waitUntil([&] { return types().size() == 8; }));
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

		// The rows' event types, from the top.
		std::vector<std::string> types()
		{
			std::vector<std::string> shown;
			for (const Json& each : elements(".list-editor-rows .list-row"))
				shown.push_back(each.at("attributes").value("type", ""));
			return shown;
		}

		std::string cell(const std::string& selector, const std::string& field)
		{
			const Json found = elements(selector + " textdisplay[field=" + field + "]");
			return found.empty() ? "" : found.at(0).at("attributes").value("value", "");
		}

		Json event(const char* key, uint64_t id)
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("midiParts").at(0).at(key))
				if (each.at("id") == id)
					return each;
			return nullptr;
		}

		Json view() { return app.call("editor.get", {{"editor", "window"}}); }

		void click(const ui::Target& target, int count = 1, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(window, target, ui::MouseButton::Left, count, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void type(const std::string& selector, const std::string& text, const std::string& enter = "Enter")
		{
			click(of(selector), 2);
			app.ui().key(window, "Ctrl+A");
			app.ui().text(window, text);
			app.ui().key(window, enter);
			app.shell().step(std::chrono::milliseconds(0));
		}

		// A point of a lane, the event display's or the value display's, at a fraction of its width.
		ui::Target at(const std::string& lane, double fraction)
		{
			const Json shown = box(lane);
			const double x = shown.at("x").get<double>() + (fraction * shown.at("width").get<double>());
			const double y = shown.at("y").get<double>() + (std::min(shown.at("height").get<double>(), 20.0) / 2.0);
			return {"", 0, ui::Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}};
		}

		void tool(const std::string& name)
		{
			click(of(".list-tool[list-tool=" + name + "]"));
			REQUIRE(has(".list-tool[list-tool=" + name + "]", "checked"));
		}

		// The vertical middle of the element, and the place in a lane at a fraction of its width, in the window's
		// pixels.
		int middle(const std::string& selector)
		{
			const Json shown = box(selector);
			return static_cast<int>(std::lround(shown.at("y").get<double>() + (shown.at("height").get<double>() / 2)));
		}

		int xAt(const std::string& lane, double fraction)
		{
			const Json shown = box(lane);
			return static_cast<int>(
				std::lround(shown.at("x").get<double>() + (fraction * shown.at("width").get<double>())));
		}

		static ui::Target point(int x, int y) { return {"", 0, ui::Point{x, y}}; }

		// The right end of the editor window's toolbar's empty space, where a click does nothing.
		int spare()
		{
			const Json shown = box(".midi-editor-toolbar .spacer");
			return static_cast<int>(std::lround(shown.at("x").get<double>() + shown.at("width").get<double>() - 5));
		}

		// Where a drag picks up the note's event in the event display, and the place half the event display further.
		ui::Point grip(uint64_t note)
		{
			const Json shown = box(row("note", note) + " .list-event");
			return {static_cast<int>(std::lround(shown.at("x").get<double>() + 10)),
				static_cast<int>(std::lround(shown.at("y").get<double>() + (shown.at("height").get<double>() / 2)))};
		}

		int halfway(uint64_t note)
		{
			return grip(note).x +
				static_cast<int>(
					std::lround(box(row("note", note) + " .list-event-lane").at("width").get<double>() / 2));
		}

		size_t steps() { return app.call("history.get").at("steps").size(); }

		size_t notes() { return app.call("project.get").at("midiParts").at(0).at("notes").size(); }

		// The value display's bar of the row, as a fraction of its lane.
		double bar(const std::string& selector)
		{
			return box(selector + " .list-value").at("width").get<double>() /
				box(selector + " .list-value-lane").at("width").get<double>();
		}

		// Selects a note with a click on its row and waits for the engine's view: whatever the list editor sent before
		// has arrived then.
		void settle(uint64_t note)
		{
			click(of(row("note", note) + " .list-type"));
			REQUIRE(app.waitUntil(
				[&]
				{
					const Json selection = view().at("selection");
					return selection.size() == 1 && selection.at(0).value("note", uint64_t{0}) == note;
				}));
		}

		// The editor window's context: through it a test presses a button and keeps it down, as a mouseup lost
		// outside the window leaves it, where RmlUi has no element to send the mouseup to, or moves the mouse in
		// steps of its own.
		Rml::Context& context()
		{
			const ui::Window* shown = app.shell().find(app.appUi().editorWindowId());
			if (shown == nullptr)
				throw std::logic_error("the editor window doesn't show");
			return shown->context();
		}

		void press(int x, int y)
		{
			context().ProcessMouseMove(x, y, 0);
			context().ProcessMouseButtonDown(0, 0);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void moveTo(int x, int y)
		{
			context().ProcessMouseMove(x, y, 0);
			app.shell().step(std::chrono::milliseconds(0));
		}

		test::UiApp app{{1400, 800}};
		ui::EventDisplay& display;
		std::optional<ui::WindowId> window;
		uint64_t track = 0;
		uint64_t part = 0;
		uint64_t low = 0;
		uint64_t high = 0;
		std::vector<uint64_t> events; // volume, program, pitch bend, aftertouch, poly pressure, SysEx
	};
}

TEST_CASE("Open List Editor lists every event of the selected parts in playback order with its values", "[ui]")
{
	Fixture f;
	CHECK(f.app.keys().keysFor("editor.openList").size() == 1);
	CHECK(
		f.app.ui().dom(std::nullopt, "#edit-menu menuitem[command=\"editor.openList\"]", 0).at("elements").size() == 1);
	CHECK(f.view().at("content") == "list");
	CHECK(f.elements(".key-editor").empty());
	CHECK(f.types() ==
		std::vector<std::string>{
			"note", "controller", "note", "program", "pitchBend", "channelPressure", "polyPressure", "sysEx"});
	const std::string note = row("note", f.low);
	CHECK(f.elements(note + " .list-type").at(0).value("text", "") == "Note");
	CHECK(f.cell(note, "start") == "1.1.1.0");
	CHECK(f.cell(note, "end") == "1.2.1.0");
	CHECK(f.cell(note, "length") == "0.1.0.0");
	CHECK(f.cell(note, "subtype") == "C3");
	CHECK(f.cell(note, "value") == "64");
	CHECK(f.cell(note, "secondary") == "64");
	CHECK(f.cell(note, "channel") == "1");
	const std::string volume = row("controller", f.events[0]);
	CHECK(f.cell(volume, "subtype") == "7");
	CHECK(f.cell(volume, "value") == "100");
	CHECK(f.cell(volume, "end").empty());
	CHECK(f.cell(row("controller", f.events[1]), "subtype") == "5");
	CHECK(f.cell(row("controller", f.events[1]), "channel") == "2");
	CHECK(f.cell(row("controller", f.events[2]), "subtype") == "4096");
	CHECK(f.cell(row("controller", f.events[3]), "subtype") == "50");
	CHECK(f.cell(row("controller", f.events[4]), "subtype") == "D3");
	CHECK(f.cell(row("controller", f.events[4]), "value") == "40");
	CHECK(f.cell(row("controller", f.events[5]), "attributes") == "F0 41 10 F7");
	CHECK(f.cell(row("controller", f.events[5]), "channel").empty());

	// The event display shows each event at its time; the value display the velocity as a bar (p. 1252).
	const Json lane = f.box(note + " .list-event-lane");
	const Json second = f.box(row("note", f.high) + " .list-event");
	CHECK(std::abs(second.at("x").get<double>() -
			  (lane.at("x").get<double>() + (lane.at("width").get<double>() / 4))) <= 1.5);
	CHECK(std::abs(second.at("width").get<double>() - (lane.at("width").get<double>() / 4)) <= 1.5);
	const Json values = f.box(note + " .list-value-lane");
	CHECK(std::abs(f.box(note + " .list-value").at("width").get<double>() -
			  (values.at("width").get<double>() * 64 / 127)) <= 1.5);
	CHECK(f.elements(row("controller", f.events[5]) + " .list-value").empty());

	// Once it is closed, Open/Close Editor opens the key editor.
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.keys().run("editor.openClose", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() != nullptr; }));
	f.window = f.app.appUi().editorWindowId();
	REQUIRE(f.app.waitUntil([&] { return !f.elements(".key-editor").empty(); }));
	CHECK(f.elements(".list-editor").empty());
}

TEST_CASE("values typed into the event list change the selected events, by as much or to the same value", "[ui]")
{
	Fixture f;
	const std::string low = row("note", f.low);
	const std::string high = row("note", f.high);
	f.click(of(low + " .list-type"));
	f.click(of(high + " .list-type"), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selection").size() == 2; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(high, "selected"); }));

	// The other selected notes keep their distance (p. 1254).
	f.type(low + " textdisplay[field=value]", "70");
	REQUIRE(f.app.waitUntil([&] { return f.cell(high, "value") == "86"; }));
	CHECK(f.cell(low, "value") == "70");
	f.type(low + " textdisplay[field=subtype]", "D3");
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.high).at("pitch") == 66; }));
	CHECK(f.event("notes", f.low).at("pitch") == 62);
	// With Ctrl (Cmd) every selected note takes the value.
	f.type(low + " textdisplay[field=value]", "100", "Ctrl+Enter");
	REQUIRE(f.app.waitUntil([&] { return f.cell(high, "value") == "100"; }));
	CHECK(f.cell(low, "value") == "100");

	// A start typed moves the event, and the list sorts it again (p. 1251); a selected controller event moves alone.
	const std::string volume = row("controller", f.events[0]);
	f.click(of(volume + " .list-type"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("controllerSelection").size() == 1; }));
	f.type(volume + " textdisplay[field=start]", "1.3.1.0");
	REQUIRE(f.app.waitUntil([&] { return f.event("controllers", f.events[0]).at("position") == 2 * g_quarter; }));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return f.types() ==
				std::vector<std::string>{
					"note", "note", "controller", "program", "pitchBend", "channelPressure", "polyPressure", "sysEx"};
		}));
	CHECK(f.event("notes", f.low).at("position") == 0);
	f.type(row("controller", f.events[2]) + " textdisplay[field=subtype]", "-8192");
	REQUIRE(f.app.waitUntil([&] { return f.event("controllers", f.events[2]).at("value") == 0.0; }));

	// SysEx: its bytes in hexadecimal (pp. 1255, 1258).
	f.type(row("controller", f.events[5]) + " textdisplay[field=attributes]", "F0 43 12 00 F7");
	REQUIRE(f.app.waitUntil(
		[&] { return f.event("controllers", f.events[5]).at("data") == Json::array({0xF0, 0x43, 0x12, 0x00, 0xF7}); }));
	REQUIRE(f.app.waitUntil([&] { return f.cell(row("controller", f.events[5]), "attributes") == "F0 43 12 00 F7"; }));
}

TEST_CASE("the filters bar hides event types and the Focus menu shows the selected event's kind", "[ui]")
{
	Fixture f;
	// A filter hides its type; Ctrl (Cmd) shows that type alone, and again all of them (p. 1254).
	f.click(of(".list-filter[event-type=note]"));
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 6; }));
	CHECK(f.has(".list-filter[event-type=note]", "checked"));
	f.click(of(".list-filter[event-type=note]"));
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 8; }));
	f.click(of(".list-filter[event-type=sysEx]"), 1, {"ctrl"});
	REQUIRE(f.app.waitUntil([&] { return f.types() == std::vector<std::string>{"sysEx"}; }));
	f.click(of(".list-filter[event-type=sysEx]"), 1, {"ctrl"});
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 8; }));

	// Focus on Event Types and Subtype: notes of the selected note's pitch (p. 1250).
	f.app.call("note.add",
		{{"part", f.part},
			{"notes",
				Json::array(
					{{{"position", 3 * g_quarter}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 0.5}}})}});
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 9; }));
	f.click(of(row("note", f.low) + " .list-type"));
	f.click(of("#list-focus"));
	f.click(of("#list-focus-menu menuitem[focus=subtype]"));
	REQUIRE(f.app.waitUntil([&] { return f.types() == std::vector<std::string>{"note", "note"}; }));
	f.click(of("#list-focus"));
	f.click(of("#list-focus-menu menuitem[focus=channel]"));
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 7; })); // not the program change on channel 2 or SysEx
	f.click(of("#list-focus"));
	f.click(of("#list-focus-menu menuitem[focus=none]"));
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 9; }));
}

TEST_CASE("the list editor draws, erases and moves events and edits values in the value display", "[ui]")
{
	Fixture f;
	// The draw tool draws the Insert Event Type where the event display is clicked, snapped (p. 1254).
	f.tool("draw");
	f.click(of("#list-insert-type"));
	f.click(of("#list-insert-type-menu menuitem[insert-type=controller]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#list-insert-type").at(0).value("text", "") == "Controller"; }));
	f.click(f.at(".list-filler .list-event-lane", 0.26));
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 9; }));
	const Json drawn = f.app.call("project.get").at("midiParts").at(0).at("controllers").back();
	CHECK(drawn.at("position") == g_quarter);
	CHECK(drawn.at("number") == 1);
	f.click(of("#list-insert-type"));
	f.click(of("#list-insert-type-menu menuitem[insert-type=note]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#list-insert-type").at(0).value("text", "") == "Note"; }));
	f.click(f.at(".list-filler .list-event-lane", 0.76));
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 10; }));
	const Json note = f.app.call("project.get").at("midiParts").at(0).at("notes").back();
	CHECK(note.at("position") == 3 * g_quarter);
	CHECK(note.at("length") == g_quarter / 4);

	// The erase tool deletes the event clicked.
	f.tool("erase");
	f.click(of(row("controller", f.events[5]) + " .list-type"));
	REQUIRE(f.app.waitUntil([&] { return f.event("controllers", f.events[5]).is_null(); }));

	// Dragging an event moves it, snapped (p. 1255).
	f.tool("objectSelection");
	const std::string low = row("note", f.low);
	const Json lane = f.box(low + " .list-event-lane");
	const double width = lane.at("width").get<double>();
	const Json box = f.box(low + " .list-event");
	const int y = static_cast<int>(std::lround(box.at("y").get<double>() + (box.at("height").get<double>() / 2)));
	const int x = static_cast<int>(std::lround(box.at("x").get<double>() + 10));
	f.app.ui().drag(
		f.window, {"", 0, ui::Point{x, y}}, {"", 0, ui::Point{x + static_cast<int>(std::lround(width / 2)), y}});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.low).at("position") == 2 * g_quarter; }));

	// A drag in the value display sets the values it crosses (p. 1255).
	const std::string volume = row("controller", f.events[0]);
	REQUIRE(f.app.waitUntil([&] { return !f.elements(volume).empty(); }));
	f.app.ui().drag(f.window, f.at(volume + " .list-value-lane", 0.5), f.at(volume + " .list-value-lane", 0.25));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil(
		[&] { return std::abs(f.event("controllers", f.events[0]).at("value").get<double>() - 0.25) < 0.02; }));
}

TEST_CASE("dragging an unselected event with a Focus filter on selects it, filters the list and moves it", "[ui]")
{
	Fixture f;
	// With nothing selected, Focus on Event Types still shows every event; the press selects the note, the focus then
	// hides the other types and rebuilds the rows while the drag goes on (p. 1250, p. 1255).
	f.click(of("#list-focus"));
	f.click(of("#list-focus-menu menuitem[focus=type]"));
	REQUIRE(f.types().size() == 8);
	const std::string low = row("note", f.low);
	const double width = f.box(low + " .list-event-lane").at("width").get<double>();
	const Json box = f.box(low + " .list-event");
	const int y = static_cast<int>(std::lround(box.at("y").get<double>() + (box.at("height").get<double>() / 2)));
	const int x = static_cast<int>(std::lround(box.at("x").get<double>() + 10));
	f.app.ui().drag(
		f.window, {"", 0, ui::Point{x, y}}, {"", 0, ui::Point{x + static_cast<int>(std::lround(width / 2)), y}});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.low).at("position") == 2 * g_quarter; }));
	CHECK(f.types() == std::vector<std::string>{"note", "note"});
	CHECK(f.has(low, "selected"));
	CHECK(f.event("notes", f.high).at("position") == g_quarter);
}

TEST_CASE("a move, draw or value display drag released outside the list editor ends there", "[ui]")
{
	Fixture f;
	// The editor window's toolbar, above the list editor, and the list editor's header: neither is the editor's body.
	const int outside = f.middle(".midi-editor-toolbar");
	const int header = f.middle(".list-editor-header");

	// A move released over the window's toolbar, where it was pressed: nothing moves, and a later click on the
	// header doesn't move the events by the distance from the pressed point to it.
	const ui::Point grip = f.grip(f.low);
	const int half = f.halfway(f.low);
	const size_t before = f.steps();
	f.app.ui().drag(f.window, Fixture::point(grip.x, grip.y), Fixture::point(grip.x, outside));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.click(Fixture::point(half, header));
	f.settle(f.high);
	CHECK(f.event("notes", f.low).at("position") == 0);
	CHECK(f.steps() == before);
	// Released outside further on, it moves the events there, in one undo step.
	f.app.ui().drag(f.window, Fixture::point(grip.x, grip.y), Fixture::point(half, outside));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.low).at("position") == 2 * g_quarter; }));
	CHECK(f.event("notes", f.high).at("position") == g_quarter);
	CHECK(f.steps() == before + 1);

	// A drawn note released outside is drawn where it was pressed; a click on the toolbar then draws nothing.
	f.tool("draw");
	const std::string filler = ".list-filler .list-event-lane";
	const int drawAt = f.xAt(filler, 0.26);
	f.app.ui().drag(f.window, Fixture::point(drawAt, f.middle(filler)), Fixture::point(drawAt, outside));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.notes() == 3; }));
	const Json project = f.app.call("project.get");
	for (const Json& each : project.at("midiParts").at(0).at("notes"))
		if (each.at("id") != f.low && each.at("id") != f.high)
			CHECK(each.at("position") == g_quarter);
	f.tool("objectSelection");
	f.settle(f.high);
	CHECK(f.notes() == 3);

	// A drag in the value display released outside sets the values it crossed. Moving over the value display then
	// changes no bar, and a click on the header sets nothing.
	const std::string volume = row("controller", f.events[0]);
	const std::string values = volume + " .list-value-lane";
	const int valueAt = f.xAt(values, 0.5);
	f.app.ui().drag(f.window, Fixture::point(valueAt, f.middle(values)), Fixture::point(valueAt, outside));
	f.app.shell().step(std::chrono::milliseconds(0));
	const auto value = [&] { return f.event("controllers", f.events[0]).at("value").get<double>(); };
	REQUIRE(f.app.waitUntil([&] { return std::abs(value() - 0.5) < 0.02; }));
	f.app.ui().move(f.window, Fixture::point(f.xAt(values, 0.9), f.middle(values)));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(std::abs(f.bar(volume) - 0.5) < 0.05);
	f.click(Fixture::point(f.xAt(values, 0.9), header));
	f.settle(f.low);
	CHECK(std::abs(value() - 0.5) < 0.02);
}

// The mouse goes up past the window's edge, where RmlUi has no element to send the mouseup to: the gestures end with
// their effect all the same, as the markers window's header drags do.
TEST_CASE("a move, draw or value display drag released outside the window ends there", "[ui]")
{
	Fixture f;
	const Rml::Vector2i size = f.context().GetDimensions();
	const int below = size.y + 40;
	const int header = f.middle(".list-editor-header");
	const size_t before = f.steps();

	// A move released below the window moves the events by as far as the mouse went, in one undo step.
	const ui::Point grip = f.grip(f.low);
	const int half = f.halfway(f.low);
	f.app.ui().drag(f.window, Fixture::point(grip.x, grip.y), Fixture::point(half, below));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.low).at("position") == 2 * g_quarter; }));
	CHECK(f.event("notes", f.high).at("position") == g_quarter);
	CHECK(f.steps() == before + 1);

	// A draw released below the window draws the note where it was pressed.
	f.tool("draw");
	const std::string filler = ".list-filler .list-event-lane";
	const int drawAt = f.xAt(filler, 0.26);
	f.app.ui().drag(f.window, Fixture::point(drawAt, f.middle(filler)), Fixture::point(drawAt, below));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.notes() == 3; }));
	const Json project = f.app.call("project.get");
	for (const Json& each : project.at("midiParts").at(0).at("notes"))
		if (each.at("id") != f.low && each.at("id") != f.high)
			CHECK(each.at("position") == g_quarter);
	f.tool("objectSelection");
	f.settle(f.high);
	CHECK(f.notes() == 3);

	// A value display drag pulled past the window's right edge sets the value it last set there: through the context,
	// so that it leaves the lane at a value the test knows. The bar then no longer follows the mouse over the value
	// display, and a click on the header sets nothing.
	const std::string volume = row("controller", f.events[0]);
	const std::string values = volume + " .list-value-lane";
	const int y = f.middle(values);
	f.press(f.xAt(values, 0.5), y);
	f.moveTo(f.xAt(values, 0.25), y);
	f.moveTo(size.x + 40, y);
	f.context().ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	const auto value = [&] { return f.event("controllers", f.events[0]).at("value").get<double>(); };
	REQUIRE(f.app.waitUntil([&] { return std::abs(value() - 0.25) < 0.02; }));
	f.app.ui().move(f.window, Fixture::point(f.xAt(values, 0.9), y));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(std::abs(f.bar(volume) - 0.25) < 0.05);
	f.click(Fixture::point(f.xAt(values, 0.9), header));
	f.settle(f.low);
	CHECK(std::abs(value() - 0.25) < 0.02);
}

// The next tests lose a mouseup, as when the mouse goes up outside the window: they press through the editor window's
// context and never release the button.
TEST_CASE("a press anywhere in the window ends a gesture whose mouseup never came, without its effect", "[ui]")
{
	Fixture f;
	const int outside = f.middle(".midi-editor-toolbar");
	const ui::Point grip = f.grip(f.low);
	const int spare = f.spare();
	INFO("the toolbar's empty space ends at " << spare << ", the note is pressed at " << grip.x);
	REQUIRE(spare > grip.x + 50); // a move ending there would move the note on
	const size_t before = f.steps();

	// A move: a click on the editor window's toolbar, away from the pressed point, moves nothing.
	f.press(grip.x, grip.y);
	f.click(Fixture::point(spare, outside));
	f.settle(f.high);
	CHECK(f.event("notes", f.low).at("position") == 0);
	CHECK(f.steps() == before);

	// A draw: the click draws no note.
	f.tool("draw");
	const std::string filler = ".list-filler .list-event-lane";
	const int drawAt = f.xAt(filler, 0.26);
	f.press(drawAt, f.middle(filler));
	f.click(Fixture::point(spare, outside));
	f.tool("objectSelection");
	f.settle(f.low);
	CHECK(f.notes() == 2);
	CHECK(f.steps() == before);

	// A value display drag: the click sets no value, and the bar shows the event's value again.
	const std::string volume = row("controller", f.events[0]);
	const std::string values = volume + " .list-value-lane";
	const int from = f.xAt(values, 0.5);
	const int to = f.xAt(values, 0.25);
	const int y = f.middle(values);
	f.press(from, y);
	f.moveTo(to, y);
	REQUIRE(std::abs(f.bar(volume) - 0.25) < 0.05);
	f.click(Fixture::point(spare, outside));
	CHECK(std::abs(f.bar(volume) - controller(100)) < 0.05); // at once, not only once a click selects a note
	f.settle(f.high);
	CHECK(std::abs(f.event("controllers", f.events[0]).at("value").get<double>() - controller(100)) < 1e-9);
	CHECK(std::abs(f.bar(volume) - controller(100)) < 0.05);
	CHECK(f.steps() == before);
}

TEST_CASE("a press in the list editor ends a gesture whose mouseup never came, without its effect", "[ui]")
{
	Fixture f;
	const int header = f.middle(".list-editor-header");
	const ui::Point grip = f.grip(f.low);
	const int half = f.halfway(f.low);
	const size_t before = f.steps();

	// A move: a click on the list editor's header moves nothing.
	f.press(grip.x, grip.y);
	f.click(Fixture::point(half, header));
	f.settle(f.high);
	CHECK(f.event("notes", f.low).at("position") == 0);
	CHECK(f.steps() == before);

	// A value display drag: the click on the header sets no value, and the bar shows the event's value again.
	const std::string volume = row("controller", f.events[0]);
	const std::string values = volume + " .list-value-lane";
	const int from = f.xAt(values, 0.5);
	const int to = f.xAt(values, 0.25);
	const int y = f.middle(values);
	f.press(from, y);
	f.moveTo(to, y);
	REQUIRE(std::abs(f.bar(volume) - 0.25) < 0.05);
	f.click(Fixture::point(to, header));
	CHECK(std::abs(f.bar(volume) - controller(100)) < 0.05); // at once, not only once a click selects a note
	f.settle(f.low);
	CHECK(std::abs(f.event("controllers", f.events[0]).at("value").get<double>() - controller(100)) < 1e-9);
	CHECK(std::abs(f.bar(volume) - controller(100)) < 0.05);
	CHECK(f.steps() == before);
}

TEST_CASE("another button pressed and released during a move doesn't end it", "[ui]")
{
	Fixture f;
	const ui::Point grip = f.grip(f.low);
	const int half = f.halfway(f.low);
	const size_t before = f.steps();
	// The right button's mouseup doesn't end the move there: the click that then selects a note cancels it.
	f.press(grip.x, grip.y);
	f.moveTo(half, grip.y);
	f.context().ProcessMouseButtonDown(1, 0);
	f.context().ProcessMouseButtonUp(1, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	f.settle(f.high);
	CHECK(f.event("notes", f.low).at("position") == 0);
	CHECK(f.steps() == before);
	// Nor does its mousedown: the left button's mouseup ends the move, in one undo step.
	f.press(grip.x, grip.y);
	f.moveTo(half, grip.y);
	f.context().ProcessMouseButtonDown(1, 0);
	f.context().ProcessMouseButtonUp(1, 0);
	f.context().ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.low).at("position") == 2 * g_quarter; }));
	CHECK(f.event("notes", f.high).at("position") == g_quarter);
	CHECK(f.steps() == before + 1);
}
