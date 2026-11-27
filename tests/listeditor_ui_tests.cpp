// SPDX-License-Identifier: GPL-3.0-or-later
// The list editor in the app (#130, docs/features/list-editor.md), driven with ui::Automation as MCP's UI tools drive
// it: Open List Editor, the event list with every event type, editing values of several events and SysEx, the filters
// bar and the Focus menu, the draw and erase tools, moving events and the value display, and gestures released outside
// the editor or never released. tests/listeditor_tests.cpp checks the engine's side.
#include "engine/midi/midivalues.h"
#include "engine/time/tempomap.h"
#include "testutils/commandholds.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/timeformat.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
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
			REQUIRE(app.waitUntil([&] { return display.project().at("midiParts").size() == 1; }));
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
			app.ui().key(window, "Mod+A");
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

		// More rows than the editor shows.
		void fillList()
		{
			Json controllers = Json::array();
			for (int i = 0; i < 60; ++i)
				controllers.push_back({{"position", i + 1}, {"number", 1}, {"value", controller(i)}});
			app.call("controller.add", {{"part", part}, {"controllers", controllers}});
			REQUIRE(app.waitUntil([&] { return types().size() == 68; }));
		}

		// Pans the list up with a finger on the type column, where a press starts no gesture, and returns how far the
		// rows went. before runs right before the touch, with no frame in between.
		double pan(const std::function<void()>& before = {})
		{
			const std::string first = row("note", low);
			const Json type = box(first + " .list-type");
			const auto x = static_cast<float>(type.at("x").get<double>() + (type.at("width").get<double>() / 2));
			const auto y = static_cast<float>(middle(first) + 200);
			const double top = box(first).at("y").get<double>();
			const auto touch = [x](float at) { return Rml::TouchList{Rml::Touch{1, Rml::Vector2f(x, at)}}; };
			Rml::Context& shown = context();
			if (before)
				before();
			shown.ProcessTouchStart(touch(y), 0);
			for (const float moved : {30.0F, 60.0F, 90.0F})
				shown.ProcessTouchMove(touch(y - moved), 0);
			shown.ProcessTouchEnd(touch(y - 90.0F), 0);
			app.shell().step(std::chrono::milliseconds(0));
			return top - box(first).at("y").get<double>();
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
		f.app.ui().dom(std::nullopt, "#midi-menu menuitem[command=\"editor.openList\"]", 0).at("elements").size() == 1);
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

	// Once it is closed, Open/Close Editor opens the key editor, in the window with Double-click opens Editor in a
	// Window (p. 1603).
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.call("settings.set", {{"key", "editing.doubleClickOpensEditorIn"}, {"value", "window"}});
	f.app.keys().run("editor.openClose", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() != nullptr; }));
	f.window = f.app.appUi().editorWindowId();
	REQUIRE(f.app.waitUntil([&] { return !f.elements(".key-editor").empty(); }));
	CHECK(f.elements(".list-editor").empty());
}

TEST_CASE("the draw tool shows the pencil over the event display and the erase tool the eraser over the rows", "[ui]")
{
	Fixture f;
	const std::string note = row("note", f.low);
	const auto pointer = [&f](const ui::Target& at)
	{
		f.app.ui().move(f.window, at);
		f.app.shell().step(std::chrono::milliseconds(0));
		return f.app.ui().windows().at("cursor").get<std::string>();
	};
	CHECK(pointer(f.at(note + " .list-event-lane", 0.7)).empty());
	f.tool("draw");
	CHECK(pointer(f.at(note + " .list-event-lane", 0.7)) == "tool-draw");
	CHECK(pointer(f.at(".list-filler .list-event-lane", 0.5)) == "tool-draw"); // the empty row under the events
	CHECK(pointer(of(note + " .list-type")).empty());
	f.tool("erase");
	CHECK(pointer(of(note + " .list-type")) == "tool-erase");
	CHECK(pointer(f.at(note + " .list-value-lane", 0.5)) == "tool-erase");
	CHECK(pointer(f.at(".list-filler .list-event-lane", 0.5)).empty());
	f.tool("objectSelection");
	CHECK(pointer(of(note + " .list-type")).empty());

	// The rows of a part that isn't editable keep the arrow under the erase tool, as a press there deletes nothing.
	const auto second = f.app.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
							.at("part")
							.at("id")
							.get<uint64_t>();
	const auto other =
		f.app
			.call("note.add",
				{{"part", second},
					{"notes",
						Json::array(
							{{{"position", 0}, {"length", g_quarter}, {"pitch", 67}, {"velocity", 50.0 / 127.0}}})}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	f.app.call("editor.open", {{"editor", "window"}, {"parts", {f.part, second}}});
	f.app.call("editor.set", {{"editor", "window"}, {"activePart", f.part}, {"editingMode", "activePart"}});
	REQUIRE(f.app.waitUntil([&] { return f.has(row("note", other), "inactive"); }));
	f.tool("erase");
	CHECK(pointer(of(row("note", other) + " .list-type")).empty());
	CHECK(pointer(of(note + " .list-type")) == "tool-erase");
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
	// Rendering the rows again without a new project keeps the time context that show built.
	const uint64_t builds = ui::projectTimeBuilds();
	f.click(of(".list-filter[event-type=note]"));
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 6; }));
	CHECK(f.has(".list-filter[event-type=note]", "checked"));
	f.click(of(".list-filter[event-type=note]"));
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 8; }));
	CHECK(ui::projectTimeBuilds() == builds);
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

	// SysEx has no channel: Focus on Event Channels with it selected hides nothing, the SysEx event neither.
	f.click(of(row("controller", f.events[5]) + " .list-type"));
	f.click(of("#list-focus"));
	f.click(of("#list-focus-menu menuitem[focus=channel]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#list-focus").at(0).value("text", "") == "Event Channels"; }));
	CHECK(f.types().size() == 9);
	CHECK(f.has(row("controller", f.events[5]), "selected"));
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
	// waitUntil steps the UI only while its condition is false: the list may not have heard of the selection yet.
	CHECK(f.app.waitUntil([&] { return f.types() == std::vector<std::string>{"note", "note"}; }));
	CHECK(f.app.waitUntil([&] { return f.has(low, "selected"); }));
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
	// The bar shows the engine's value a step or more after the engine has it (#892).
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.bar(volume) - 0.5) < 0.05; }));
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

// RmlUi doesn't scroll on a touch move while there is a drag element: only a press that starts a gesture makes one.
TEST_CASE("a touch pan over the type column scrolls the list", "[ui]")
{
	Fixture f;
	f.fillList();
	CHECK(f.pan() > 50.0);
}

// RmlUi picks the drag element right after a press's mousedown, from the computed values: they must be up to date
// when the class that makes the body a drag element goes, even with no frame before that press.
TEST_CASE("a touch pan whose start ends a gesture whose mouseup never came scrolls the list", "[ui]")
{
	Fixture f;
	f.fillList();
	const ui::Point grip = f.grip(f.low);
	f.press(grip.x, grip.y);
	REQUIRE(f.has(".list-editor-body", "in-gesture"));
	CHECK(f.pan() > 50.0);
	CHECK(!f.has(".list-editor-body", "in-gesture"));
}

TEST_CASE("a touch pan in the same frame as the mouseup that ended a gesture scrolls the list", "[ui]")
{
	Fixture f;
	f.fillList();
	const ui::Point grip = f.grip(f.low);
	f.press(grip.x, grip.y);
	REQUIRE(f.has(".list-editor-body", "in-gesture"));
	CHECK(f.pan([&] { f.context().ProcessMouseButtonUp(0, 0); }) > 50.0);
	CHECK(!f.has(".list-editor-body", "in-gesture"));
}

TEST_CASE("a draw dragged in the event display is a multiple of the length quantize's length", "[ui]")
{
	Fixture f;
	f.tool("draw");
	const std::string filler = ".list-filler .list-event-lane";
	const int y = f.middle(filler);
	f.app.ui().drag(f.window, Fixture::point(f.xAt(filler, 0.26), y), Fixture::point(f.xAt(filler, 0.74), y));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.notes() == 3; }));
	const Json project = f.app.call("project.get");
	for (const Json& each : project.at("midiParts").at(0).at("notes"))
		if (each.at("id") != f.low && each.at("id") != f.high)
		{
			CHECK(each.at("position") == g_quarter);
			CHECK(each.at("length") == 2 * g_quarter);
		}
}

TEST_CASE("the event list takes a typed end, length, channel and release velocity, with spaces and pitch names in "
		  "either case",
	"[ui]")
{
	Fixture f;
	const std::string low = row("note", f.low);
	const auto note = [&](const char* key) { return f.event("notes", f.low).at(key).get<double>(); };
	f.type(low + " textdisplay[field=end]", "1.3.1.0");
	REQUIRE(f.app.waitUntil([&] { return note("length") == 2 * g_quarter; }));
	f.type(low + " textdisplay[field=length]", "0.3.0.0");
	REQUIRE(f.app.waitUntil([&] { return note("length") == 3 * g_quarter; }));
	f.type(low + " textdisplay[field=channel]", "3");
	REQUIRE(f.app.waitUntil([&] { return note("channel") == 2; }));
	f.type(low + " textdisplay[field=secondary]", "90");
	REQUIRE(f.app.waitUntil([&] { return std::abs(note("releaseVelocity") - (90.0 / 127.0)) < 1e-4; }));
	f.type(low + " textdisplay[field=value]", " 70 ");
	REQUIRE(f.app.waitUntil([&] { return std::abs(note("velocity") - (70.0 / 127.0)) < 1e-4; }));
	f.type(low + " textdisplay[field=subtype]", "d3");
	REQUIRE(f.app.waitUntil([&] { return note("pitch") == 62; }));
	// A typed velocity of 0 is 1, the least the value display sets too: 0 would be a note off.
	f.type(low + " textdisplay[field=value]", "0");
	REQUIRE(f.app.waitUntil([&] { return std::abs(note("velocity") - (1.0 / 127.0)) < 1e-4; }));
	CHECK(f.app.waitUntil([&] { return f.cell(low, "value") == "1"; }));
}

TEST_CASE("the Insert Velocity and Length Quantize controls give a drawn note its velocity and length", "[ui]")
{
	Fixture f;
	f.type("#list-insert-velocity", "90");
	REQUIRE(
		f.app.waitUntil([&] { return std::abs(f.view().at("insertVelocity").get<double>() - (90.0 / 127.0)) < 1e-4; }));
	f.click(of("#list-length-quantize"));
	f.click(of(R"(#list-length-menu menuitem[length="1/4"])"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("lengthQuantize") == "1/4"; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#list-length-quantize").at(0).value("text", "") == "1/4"; }));
	f.tool("draw");
	f.click(f.at(".list-filler .list-event-lane", 0.51));
	REQUIRE(f.app.waitUntil([&] { return f.notes() == 3; }));
	const Json drawn = f.app.call("project.get").at("midiParts").at(0).at("notes").back();
	CHECK(drawn.at("position") == 2 * g_quarter);
	CHECK(drawn.at("length") == g_quarter);
	CHECK(std::abs(drawn.at("velocity").get<double>() - (90.0 / 127.0)) < 1e-4);
}

TEST_CASE("a value typed for selected events of two parts changes them in one undo step", "[ui]")
{
	Fixture f;
	const auto second = f.app.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
							.at("part")
							.at("id")
							.get<uint64_t>();
	const auto other =
		f.app
			.call("note.add",
				{{"part", second},
					{"notes",
						Json::array(
							{{{"position", 0}, {"length", g_quarter}, {"pitch", 67}, {"velocity", 50.0 / 127.0}}})}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	f.app.call("editor.open", {{"editor", "window"}, {"parts", {f.part, second}}});
	REQUIRE(f.app.waitUntil([&] { return !f.elements(row("note", other)).empty(); }));
	f.click(of(row("note", f.low) + " .list-type"));
	f.click(of(row("note", other) + " .list-type"), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selection").size() == 2; }));
	const size_t before = f.steps();
	f.type(row("note", f.low) + " textdisplay[field=value]", "74");
	const auto velocity = [&]
	{
		const Json project = f.app.call("project.get");
		for (const Json& part : project.at("midiParts"))
			for (const Json& each : part.at("notes"))
				if (each.at("id") == other)
					return each.at("velocity").get<double>();
		return 0.0;
	};
	REQUIRE(f.app.waitUntil([&] { return std::abs(velocity() - (60.0 / 127.0)) < 1e-4; }));
	CHECK(std::abs(f.event("notes", f.low).at("velocity").get<double>() - (74.0 / 127.0)) < 1e-4);
	CHECK(f.steps() == before + 1);
}

TEST_CASE("a value display drag across rows sets each one's value in one undo step, moving only the bars", "[ui]")
{
	Fixture f;
	const size_t before = f.steps();
	const std::string low = row("note", f.low) + " .list-value-lane";
	const std::string volume = row("controller", f.events[0]) + " .list-value-lane";
	const int lowY = f.middle(low);
	const int volumeY = f.middle(volume);
	const int threeQuarters = f.xAt(low, 0.75);
	const int half = f.xAt(volume, 0.5);
	const int quarter = f.xAt(volume, 0.25);
	f.press(threeQuarters, lowY);
	// The moves change the bars alone: they neither render the rows again nor build a time context of their own.
	const uint64_t builds = ui::projectTimeBuilds();
	f.moveTo(half, volumeY);
	f.moveTo(quarter, volumeY);
	CHECK(ui::projectTimeBuilds() == builds);
	CHECK(std::abs(f.bar(row("note", f.low)) - 0.75) < 0.05);
	CHECK(std::abs(f.bar(row("controller", f.events[0])) - 0.25) < 0.05);
	f.context().ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil(
		[&] { return std::abs(f.event("controllers", f.events[0]).at("value").get<double>() - 0.25) < 0.02; }));
	CHECK(std::abs(f.event("notes", f.low).at("velocity").get<double>() - 0.75) < 0.02);
	CHECK(f.steps() == before + 1);
}

// A Shift-click right after a click: the click's editor.changed, with its note alone, can come while the Shift-click's
// editor.set is still out. The list keeps both notes selected, so that a move right then takes both.
TEST_CASE("a Shift-click's selection stays while the view of the click before it comes in", "[ui]")
{
	test::CommandHolds holds; // declared before the fixture: its engine's command thread waits in hold()
	Fixture f;
	const test::ReleaseHolds releaseHolds{holds};
	f.app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", engine::CommandInfo{}.parameters, Json::object(), false},
		[&holds](engine::CommandContext&) { return holds.hold(); });
	engine::Client holder(f.app.engine(), "holder");
	const ui::EngineBinding& binding = f.app.appUi().binding();

	// The click's editor.set queues behind the first hold, the Shift-click's behind the second.
	holder.send("test.hold");
	REQUIRE(holds.waitEntered(1));
	f.click(of(row("note", f.low) + " .list-type"));
	const size_t clicked = binding.waiting("editor.set");
	holder.send("test.hold");
	f.click(of(row("note", f.high) + " .list-type"), 1, {"shift"});
	const size_t shiftClicked = binding.waiting("editor.set") - clicked;
	REQUIRE(shiftClicked > 0);
	CHECK(f.has(row("note", f.low), "selected"));
	CHECK(f.has(row("note", f.high), "selected"));

	// The click's view comes in: both notes stay selected.
	holds.release(1);
	REQUIRE(holds.waitEntered(2));
	REQUIRE(f.app.waitUntil([&] { return binding.waiting("editor.set") == shiftClicked; }));
	CHECK(f.has(row("note", f.low), "selected"));
	CHECK(f.has(row("note", f.high), "selected"));

	holds.release(2);
	REQUIRE(f.app.waitUntil([&] { return binding.waiting("editor.set") == 0; }));
	CHECK(f.view().at("selection").size() == 2);
	CHECK(f.has(row("note", f.low), "selected"));
	CHECK(f.has(row("note", f.high), "selected"));
}

TEST_CASE("a press whose selection the engine refuses shows the view's selection again in the list", "[ui]")
{
	// The list shows a note the engine no longer has, as when another client has just deleted it: the press's
	// editor.set is refused, and once it is answered the list shows the view's selection again, not the press's, as the
	// key editor does (#1897).
	Fixture f;
	ui::EngineBinding& binding = f.app.appUi().binding();
	// No read is out that would show the engine's project or view again before the press is answered.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return binding.waiting("project.get") == 0 && binding.waiting("editor.get") == 0 &&
				binding.waiting("editor.set") == 0;
		}));
	Json project = f.app.call("project.get");
	const Json view = f.view();
	uint64_t gone = 0;
	for (Json& part : project.at("midiParts"))
		if (part.at("id") == f.part)
		{
			Json note = part.at("notes").at(0);
			gone = note.at("id").get<uint64_t>() + 1000;
			note["id"] = gone;
			note["pitch"] = 62;
			part.at("notes").push_back(note);
		}
	REQUIRE(gone != 0);
	f.app.appUi().editorWindow()->listEditor()->show(project, view);
	REQUIRE(!f.elements(row("note", gone)).empty());
	REQUIRE(!f.has(row("note", gone), "selected"));
	// A press on it in the event display released over the window's toolbar, where it was pressed: no edit and no
	// click, which would activate the part with an editor.set of its own, whose editor.changed would show the view
	// again anyway.
	const ui::Point grip = f.grip(gone);
	const int outside = f.middle(".midi-editor-toolbar");
	const size_t before = f.steps();
	f.app.ui().press(f.window, Fixture::point(grip.x, grip.y), ui::MouseButton::Left);
	CHECK(f.has(row("note", gone), "selected")); // at once: no step has handed on the engine's answer yet
	f.app.ui().release(f.window, Fixture::point(grip.x, outside), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return binding.waiting("editor.set") == 0; }));
	CHECK(!f.has(row("note", gone), "selected"));
	CHECK(f.view().at("selection") == view.at("selection"));
	CHECK(f.steps() == before);
}

TEST_CASE("a move takes every selected event, none before its part's start, and the erase tool erases in the event "
		  "display",
	"[ui]")
{
	Fixture f;
	f.click(of(row("note", f.low) + " .list-type"));
	f.click(of(row("note", f.high) + " .list-type"), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selection").size() == 2; }));
	const size_t before = f.steps();
	// A quarter earlier the low note, at the part's start, would leave the part: nothing moves.
	const ui::Point grip = f.grip(f.high);
	const auto quarter =
		static_cast<int>(std::lround(f.box(row("note", f.high) + " .list-event-lane").at("width").get<double>() / 4));
	f.app.ui().drag(f.window, Fixture::point(grip.x, grip.y), Fixture::point(grip.x - quarter, grip.y));
	f.app.shell().step(std::chrono::milliseconds(0));
	// Half a bar later both move, in one undo step: the first move sent nothing before this one.
	f.app.ui().drag(f.window, Fixture::point(grip.x, grip.y), Fixture::point(f.halfway(f.high), grip.y));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.high).at("position") == 3 * g_quarter; }));
	CHECK(f.event("notes", f.low).at("position") == 2 * g_quarter);
	CHECK(f.steps() == before + 1);

	f.tool("erase");
	f.click(of(row("note", f.high) + " .list-event"));
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.high).is_null(); }));
}

// The press selects the event, and the Focus filter then rebuilds the rows: RmlUi starts no drag if that happens under
// the press itself.
TEST_CASE("a move of an unselected event with a Focus filter on released outside the window ends there", "[ui]")
{
	Fixture f;
	f.click(of("#list-focus"));
	f.click(of("#list-focus-menu menuitem[focus=type]"));
	REQUIRE(f.types().size() == 8);
	const ui::Point grip = f.grip(f.low);
	const int half = f.halfway(f.low);
	const int below = f.context().GetDimensions().y + 40;
	f.app.ui().drag(f.window, Fixture::point(grip.x, grip.y), Fixture::point(half, below));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.low).at("position") == 2 * g_quarter; }));
	REQUIRE(f.app.waitUntil([&] { return f.types() == std::vector<std::string>{"note", "note"}; }));
	CHECK(f.has(row("note", f.low), "selected"));
}

// The next tests lose a mouseup, as when the mouse goes up outside the window: they press through the editor window's
// context and never release the button.
TEST_CASE("a press anywhere in the window ends a gesture whose mouseup never came, without its effect", "[ui]")
{
	Fixture f;
	const int outside = f.middle(".midi-editor-toolbar");
	const ui::Point grip = f.grip(f.low);
	const int spare = f.spare();
	// A move ending there would move the note on by an eighth or more, two steps of the quantize grid.
	const double ticks = (spare - grip.x) / f.box(row("note", f.low) + " .list-event-lane").at("width").get<double>() *
		static_cast<double>(g_bar);
	INFO("the toolbar's empty space ends at " << spare << ", the note is pressed at " << grip.x << ", " << ticks
											  << " ticks before it");
	REQUIRE(ticks >= static_cast<double>(g_quarter) / 2);
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

TEST_CASE("a value display drag whose release was lost ends without its effect as the mouse leaves the window", "[ui]")
{
	// The mouse leaves the window before it moves, as when the window loses the focus: no drag starts, so no dragend
	// comes. The drag ends at the leave, without its effect (#1756): the mouse back over the value display with the
	// button up sets nothing.
	Fixture f;
	const size_t before = f.steps();
	const std::string volume = row("controller", f.events[0]);
	const std::string values = volume + " .list-value-lane";
	const int y = f.middle(values);
	f.press(f.xAt(values, 0.25), y);
	REQUIRE(std::abs(f.bar(volume) - 0.25) < 0.05);
	f.context().ProcessMouseLeave();
	f.context().ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(std::abs(f.bar(volume) - controller(100)) < 0.05);
	f.moveTo(f.xAt(values, 0.75), y);
	CHECK(std::abs(f.bar(volume) - controller(100)) < 0.05);
	f.settle(f.low);
	CHECK(std::abs(f.event("controllers", f.events[0]).at("value").get<double>() - controller(100)) < 1e-9);
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

TEST_CASE("Delete and Backspace delete the selected events, but not while a value is typed", "[ui]")
{
	Fixture f;
	// Delete removes the selected note and controller event in one undo step (pp. 1255, 1258).
	f.click(of(row("note", f.low) + " .list-type"));
	f.click(of(row("controller", f.events[0]) + " .list-type"), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("controllerSelection").size() == 1; }));
	REQUIRE(f.view().at("selection").size() == 1);
	const size_t before = f.steps();
	f.app.ui().key(f.window, "Delete");
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 6; }));
	CHECK(f.event("notes", f.low).is_null());
	CHECK(f.event("controllers", f.events[0]).is_null());
	CHECK(f.event("notes", f.high) != nullptr);
	CHECK(f.steps() == before + 1);

	// Backspace while a value is typed edits the text; once the SysEx message is selected, it deletes the message.
	const std::string sysEx = row("controller", f.events[5]);
	f.click(of(sysEx + " textdisplay[field=start]"), 2);
	f.app.ui().key(f.window, "Backspace");
	f.app.ui().key(f.window, "Escape");
	f.app.shell().step(std::chrono::milliseconds(0));
	f.click(of(sysEx + " .list-type"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("controllerSelection").size() == 1; }));
	CHECK(f.event("controllers", f.events[5]) != nullptr);
	f.app.ui().key(f.window, "Backspace");
	REQUIRE(f.app.waitUntil([&] { return f.event("controllers", f.events[5]).is_null(); }));
	CHECK(f.app.waitUntil([&] { return f.types().size() == 5; }));
}

TEST_CASE("Alt and a drag in the event display copy the selected events", "[ui]")
{
	Fixture f;
	// The low note and the volume event, half a bar later, with all their values, in one undo step (p. 1255).
	f.click(of(row("note", f.low) + " .list-type"));
	f.click(of(row("controller", f.events[0]) + " .list-type"), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("controllerSelection").size() == 1; }));
	const size_t before = f.steps();
	const ui::Point grip = f.grip(f.low);
	f.app.ui().drag(f.window, Fixture::point(grip.x, grip.y), Fixture::point(f.halfway(f.low), grip.y), {"alt"});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.types().size() == 10; }));
	CHECK(f.event("notes", f.low).at("position") == 0);
	CHECK(f.event("controllers", f.events[0]).at("position") == 0);
	const Json part = f.app.call("project.get").at("midiParts").at(0);
	const Json note = part.at("notes").back();
	CHECK(note.at("position") == 2 * g_quarter);
	CHECK(note.at("pitch") == 60);
	CHECK(std::abs(note.at("velocity").get<double>() - (64.0 / 127.0)) < 1e-4);
	const Json volume = part.at("controllers").back();
	CHECK(volume.at("position") == 2 * g_quarter);
	CHECK(volume.at("number") == 7);
	CHECK(std::abs(volume.at("value").get<double>() - controller(100)) < 1e-9);
	CHECK(f.steps() == before + 1);
}

TEST_CASE("a drag at a note's end in the event display resizes the selected notes", "[ui]")
{
	Fixture f;
	// Both notes a quarter longer, snapped, in one undo step; the controller event selected too stays (p. 1255).
	f.click(of(row("note", f.low) + " .list-type"));
	f.click(of(row("note", f.high) + " .list-type"), 1, {"shift"});
	f.click(of(row("controller", f.events[0]) + " .list-type"), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("controllerSelection").size() == 1; }));
	const size_t before = f.steps();
	const Json shown = f.box(row("note", f.low) + " .list-event");
	const int x = static_cast<int>(std::lround(shown.at("x").get<double>() + shown.at("width").get<double>() - 2));
	const int y = f.middle(row("note", f.low) + " .list-event");
	const auto quarter =
		static_cast<int>(std::lround(f.box(row("note", f.low) + " .list-event-lane").at("width").get<double>() / 4));
	f.app.ui().drag(f.window, Fixture::point(x, y), Fixture::point(x + quarter, y));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.event("notes", f.high).at("length") == 2 * g_quarter; }));
	CHECK(f.event("notes", f.low).at("length") == 2 * g_quarter);
	CHECK(f.event("notes", f.low).at("position") == 0);
	CHECK(f.event("notes", f.high).at("position") == g_quarter);
	CHECK(f.event("controllers", f.events[0]).at("position") == 0);
	CHECK(f.steps() == before + 1);
}

TEST_CASE("a click in the event display without a drag changes no event, off the quantize grid too", "[ui]")
{
	Fixture f;
	// The low note ends a tenth of a quarter after the first one, off the grid; the SysEx message is a tick after
	// three.
	const std::string low = row("note", f.low);
	const double width = f.box(low + " .list-event").at("width").get<double>();
	f.app.call("note.set",
		{{"part", f.part}, {"notes", Json::array({{{"id", f.low}, {"length", g_quarter + (g_quarter / 10)}}})}});
	REQUIRE(f.event("notes", f.low).at("length") == g_quarter + (g_quarter / 10));
	REQUIRE(f.app.waitUntil([&] { return f.box(low + " .list-event").at("width").get<double>() > width; }));
	const size_t before = f.steps();

	// A press and release at the note's end selects it, and its length stays.
	const Json shown = f.box(low + " .list-event");
	f.click(
		Fixture::point(static_cast<int>(std::lround(shown.at("x").get<double>() + shown.at("width").get<double>() - 2)),
			f.middle(low + " .list-event")));
	f.settle(f.high);
	CHECK(f.event("notes", f.low).at("length") == g_quarter + (g_quarter / 10));

	// An Alt-click on the SysEx message copies nothing, a click moves it nowhere.
	const Json sysEx = f.box(row("controller", f.events[5]) + " .list-event");
	const ui::Target middle = Fixture::point(
		static_cast<int>(std::lround(sysEx.at("x").get<double>() + (sysEx.at("width").get<double>() / 2))),
		static_cast<int>(std::lround(sysEx.at("y").get<double>() + (sysEx.at("height").get<double>() / 2))));
	f.click(middle, 1, {"alt"});
	f.click(middle);
	f.settle(f.high);
	CHECK(f.event("controllers", f.events[5]).at("position") == (3 * g_quarter) + 1);
	CHECK(f.types().size() == 8);
	CHECK(f.steps() == before);
}

TEST_CASE("the auditioning column points at the event at the cursor, locates to an event and starts playback", "[ui]")
{
	Fixture f;
	const ui::TimeContext time = ui::projectTime(f.app.call("project.get"));
	const auto samples = [&time](int64_t ticks) { return time.tempo.toSamples(ticks, time.sampleRate); };
	const auto pointed = [&f]
	{
		std::vector<std::string> rows;
		for (const Json& each : f.elements(".list-editor-rows .list-row.at-cursor"))
			rows.push_back(each.at("attributes").value("note", each.at("attributes").value("controller", "")));
		return rows;
	};
	const std::string high = std::to_string(f.high);

	// The arrow is at the event that starts closest before the cursor (p. 1251), also between two events.
	f.app.call("transport.locate", {{"position", samples(g_quarter + (g_quarter / 2))}});
	REQUIRE(f.app.waitUntil([&] { return pointed() == std::vector<std::string>{high}; }));
	f.app.call("transport.locate", {{"position", samples(g_quarter)}});
	f.settle(f.high);
	CHECK(pointed() == std::vector<std::string>{high});
	// An event that moves without changing the list's order takes the arrow along: the program change moves from 2
	// quarters to 1.25, still after the high note, past the cursor at 1.5 quarters.
	f.app.call("transport.locate", {{"position", samples(g_quarter + (g_quarter / 2))}});
	f.app.call("controller.set",
		{{"part", f.part},
			{"controllers", Json::array({{{"id", f.events[1]}, {"position", g_quarter + (g_quarter / 4)}}})}});
	REQUIRE(f.app.waitUntil([&] { return pointed() == std::vector<std::string>{std::to_string(f.events[1])}; }));
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return pointed() == std::vector<std::string>{high}; }));

	// A click in the column moves the cursor to the event's start; a double click starts playback there, and another
	// one stops it.
	f.click(of(row("controller", f.events[3]) + " .list-audition"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("transport.get").at("position") == samples(3 * g_quarter); }));
	REQUIRE(f.app.waitUntil([&] { return pointed().size() == 1 && pointed() != std::vector<std::string>{high}; }));
	CHECK(f.app.call("transport.get").at("playing") == false);
	f.app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	f.app.call("audio.start");
	f.click(of(row("note", f.low) + " .list-audition"), 2);
	REQUIRE(f.app.waitUntil([&] { return f.app.call("transport.get").at("playing") == true; }));
	// The double click's first click locates to the note, its dblclick starts playback, and the click that ends it
	// doesn't locate again, which would play the first notes twice.
	std::vector<std::string> sent;
	const Json log = f.app.call("engine.commandLog");
	for (const Json& each : log.at("commands"))
		if (each.at("command") == "transport.locate" || each.at("command") == "transport.toggle")
			sent.push_back(each.at("command").get<std::string>());
	REQUIRE(sent.size() >= 2);
	CHECK(sent[sent.size() - 2] == "transport.locate");
	CHECK(sent.back() == "transport.toggle");
	// The arrow follows the playback position, past the events at the start.
	const std::vector<std::string> atStart = {std::to_string(f.low), std::to_string(f.events[0])};
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const std::vector<std::string> rows = pointed();
			return rows.size() == 1 && std::find(atStart.begin(), atStart.end(), rows[0]) == atStart.end();
		}));
	f.click(of(row("note", f.low) + " .list-audition"), 2);
	REQUIRE(f.app.waitUntil([&] { return f.app.call("transport.get").at("playing") == false; }));
}

// #2138: a gesture's undo step has one name, whether it reaches one part, one command on its own, or two, a
// history.transaction, and one of its own for notes and controller events together.
TEST_CASE("list editor gestures keep their undo name over one part and two", "[ui][slow]")
{
	const bool both = GENERATE(false, true);
	const auto [gesture, kind] = GENERATE(table<std::string, std::string>({{"type", "notes"}, {"type", "controllers"},
		{"type", "events"}, {"values", "notes"}, {"values", "controllers"}, {"values", "events"}, {"move", "notes"},
		{"move", "controllers"}, {"move", "events"}, {"copy", "notes"}, {"copy", "controllers"}, {"copy", "events"},
		{"delete", "notes"}, {"delete", "controllers"}, {"delete", "events"}, {"resize", "notes"}}));
	CAPTURE(both, gesture, kind);
	Fixture f;
	std::string first = row(kind == "controllers" ? "controller" : "note", kind == "controllers" ? f.events[0] : f.low);
	std::string second = kind == "notes" ? row("note", f.high) : row("controller", f.events[kind == "events" ? 0 : 1]);
	if (both)
	{
		const auto other = f.app.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
							   .at("part")
							   .at("id")
							   .get<uint64_t>();
		const bool note = kind == "notes";
		const Json added = note
			? f.app.call("note.add",
				  {{"part", other},
					  {"notes", {{{"position", 0}, {"length", g_quarter}, {"pitch", 67}, {"velocity", 50.0 / 127.0}}}}})
			: f.app.call("controller.add",
				  {{"part", other}, {"controllers", {{{"position", 0}, {"number", 7}, {"value", controller(30)}}}}});
		second =
			row(note ? "note" : "controller", added.at(note ? "notes" : "controllers").at(0).at("id").get<uint64_t>());
		f.app.call("editor.open", {{"editor", "window"}, {"parts", {f.part, other}}});
		REQUIRE(f.app.waitUntil([&] { return !f.elements(second).empty(); }));
	}
	f.click(of(first + " .list-type"));
	f.click(of(second + " .list-type"), 1, {"shift"});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json view = f.view();
			return view.at("selection").size() + view.at("controllerSelection").size() == 2;
		}));
	const size_t before = f.steps();
	if (gesture == "type")
		f.type(first + " textdisplay[field=channel]", "2");
	else if (gesture == "delete")
		f.app.ui().key(f.window, "Delete");
	else if (gesture == "values")
	{
		const std::string from = first + " .list-value-lane";
		const std::string to = second + " .list-value-lane";
		f.press(f.xAt(from, 0.75), f.middle(from));
		f.moveTo(f.xAt(to, 0.25), f.middle(to));
		f.context().ProcessMouseButtonUp(0, 0);
	}
	else
	{
		// A quarter of the event display later, from the first event, or from its end to resize it.
		const Json shown = f.box(first + " .list-event");
		const double x = shown.at("x").get<double>();
		const double width = shown.at("width").get<double>();
		const int from =
			static_cast<int>(std::lround(gesture == "resize" ? x + width - 2 : x + std::min(width / 2, 10.0)));
		const int y = f.middle(first + " .list-event");
		const auto quarter =
			static_cast<int>(std::lround(f.box(first + " .list-event-lane").at("width").get<double>() / 4));
		f.app.ui().drag(f.window, Fixture::point(from, y), Fixture::point(from + quarter, y),
			gesture == "copy" ? std::vector<std::string>{"alt"} : std::vector<std::string>{});
	}
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("history.get").at("position") == before + 1; }));
	const Json history = f.app.call("history.get");
	REQUIRE(history.at("steps").size() == before + 1);
	std::string verb = "Change ";
	if (gesture == "copy")
		verb = "Add ";
	else if (gesture == "delete")
		verb = "Remove ";
	CHECK(history.at("steps").at(before).at("action") == verb + kind);
}

// #719: the list editor edits a program change's bank select, which has no column of its own in the reference manual's
// list (p. 1253, where a program change's Main Value is unused).
TEST_CASE("a program change's Main Value is its bank, typed for the selected ones and taken away when empty", "[ui]")
{
	Fixture f;
	const std::string program = row("controller", f.events[1]);
	const std::string value = program + " textdisplay[field=value]";
	const auto bank = [&](uint64_t id) { return f.event("controllers", id).value("bank", Json()); };
	// Without a bank the cell is empty, but it takes one.
	CHECK(f.cell(program, "value").empty());
	CHECK_FALSE(f.elements(value).at(0).at("attributes").contains("disabled"));
	f.type(value, "3");
	REQUIRE(f.app.waitUntil([&] { return bank(f.events[1]) == 3 && f.cell(program, "value") == "3"; }));
	// With another one selected that has bank 10 they keep their distance, and the bank goes no higher than 16383.
	const auto other =
		f.app
			.call("controller.add",
				{{"part", f.part},
					{"controllers", {{{"position", g_quarter}, {"type", "program"}, {"number", 7}, {"bank", 10}}}}})
			.at("controllers")
			.at(0)
			.at("id")
			.get<uint64_t>();
	REQUIRE(f.app.waitUntil([&] { return f.cell(row("controller", other), "value") == "10"; }));
	f.click(of(program + " .list-type"));
	f.click(of(row("controller", other) + " .list-type"), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("controllerSelection").size() == 2; }));
	f.type(value, "5");
	REQUIRE(f.app.waitUntil([&] { return bank(other) == 12; }));
	CHECK(bank(f.events[1]) == 5);
	f.type(value, "16380");
	REQUIRE(f.app.waitUntil([&] { return bank(other) == 16383; }));
	CHECK(bank(f.events[1]) == 16380);
	// Nothing typed takes both banks away, in one undo step.
	const size_t before = f.steps();
	f.click(of(value), 2);
	f.app.ui().key(f.window, "Mod+A");
	f.app.ui().key(f.window, "Backspace");
	f.app.ui().key(f.window, "Enter");
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return bank(other).is_null() && bank(f.events[1]).is_null(); }));
	CHECK(f.steps() == before + 1);
	CHECK(f.app.waitUntil([&] { return f.cell(program, "value").empty(); }));
}
