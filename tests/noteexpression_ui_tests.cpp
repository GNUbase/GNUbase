// SPDX-License-Identifier: GPL-3.0-or-later
// The key editor's expression editor (#1512, docs/features/note-expression.md) while the engine's answers come late:
// edits made before the project read of the last one arrived (#1795). tests/keyeditor_ui_tests.cpp checks its
// gestures, tests/noteexpression_tests.cpp the engine's note.replaceExpression. Also MIDI > Note Expression (#1515) and
// the expressions the inspector hides (#2120).
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/keyeditor.h"
#include "ui/midieditor.h"
#include "ui/shell.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;

	// Two MIDI tracks, each with a one-bar part at the project start and a note over its first half beat, pitch 60
	// and 62, both open in the editor window at CI's window size, both notes selected and the expression editor open
	// on them. The first note has a tuning point.
	struct Fixture
	{
		Fixture() : app({1024, 600})
		{
			for (size_t i = 0; i < parts.size(); ++i)
			{
				const uint64_t track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
				parts.at(i) =
					app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}, {"name", "Keys"}})
						.at("part")
						.at("id")
						.get<uint64_t>();
				Json note = {{"position", 0}, {"length", 2 * g_quarter}, {"pitch", 60 + (2 * static_cast<int>(i))},
					{"velocity", 0.5}};
				if (i == 0)
					note["expressions"] = Json::array({tuning});
				notes.at(i) = app.call("note.add", {{"part", parts.at(i)}, {"notes", Json::array({note})}})
								  .at("notes")
								  .at(0)
								  .at("id")
								  .get<uint64_t>();
			}
			app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({parts[0], parts[1]})}});
			REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
			window = app.appUi().editorWindowId();
			REQUIRE(app.waitUntil([&] { return elements(".key-note").size() == 2; }));
			REQUIRE(app.waitUntil([&] { return keys().scrollToPitch(61); }));
			app.call("editor.set",
				{{"editor", "window"}, {"expressionDoubleClick", true},
					{"selection",
						Json::array(
							{{{"part", parts[0]}, {"note", notes[0]}}, {{"part", parts[1]}, {"note", notes[1]}}})}});
			REQUIRE(app.waitUntil([&] { return has(".key-input[setting=expressionDoubleClick]", "checked"); }));
			REQUIRE(app.waitUntil([&] { return elements(".key-note.selected").size() == 2; }));
			app.shell().step(std::chrono::milliseconds(0));
			const std::string first = ".key-note[note=\"" + std::to_string(notes[0]) + "\"]";
			app.ui().click(window, {first, 0, std::nullopt}, ui::MouseButton::Left, 2, {});
			REQUIRE(app.waitUntil([&] { return !has(".key-expression-editor", "off"); }));
			REQUIRE(app.waitUntil([&] { return lines() == 1; }));
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json elements(const std::string& selector) { return app.ui().dom(window, selector, 0).at("elements"); }

		bool has(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.empty())
				return false;
			const Json names = found.at(0).value("classes", Json::array());
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		ui::KeyEditor& keys() { return *app.appUi().editorWindow()->keyEditor(); }

		// The lines the expression editor shows, one per point.
		size_t lines() { return elements(".key-expression-area .key-expression-step").size(); }

		// The note's points as the engine has them, sorted by position.
		Json points(size_t note)
		{
			const Json project = app.call("project.get"); // kept: a range-for keeps no temporary it reaches into
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == parts.at(note))
					for (const Json& found : each.at("notes"))
						if (found.at("id") == notes.at(note))
							return found.value("expressions", Json::array());
			return nullptr;
		}

		// A click in the expression editor's area, x and y in parts of its width and height, straight into the
		// window's context: the UI hands on no answer of the engine, so a click after it starts from the project as
		// the editor has it now.
		void click(double x, double y)
		{
			const ui::Window* shown = window ? app.shell().find(*window) : nullptr;
			REQUIRE(shown != nullptr);
			const Json area = elements(".key-expression-area").at(0).at("box");
			Rml::Context& context = shown->context();
			context.ProcessMouseMove(
				static_cast<int>(std::lround(area.at("x").get<double>() + (x * area.at("width").get<double>()))),
				static_cast<int>(std::lround(area.at("y").get<double>() + (y * area.at("height").get<double>()))), 0);
			context.ProcessMouseButtonDown(0, 0);
			context.ProcessMouseButtonUp(0, 0);
		}

		const Json tuning = {{"type", "tuning"}, {"position", g_quarter / 2}, {"value", 12.0}};
		test::UiApp app;
		std::optional<ui::WindowId> window;
		std::array<uint64_t, 2> parts{};
		std::array<uint64_t, 2> notes{};
	};
}

TEST_CASE("expression edits made before the project read of the last one arrived all stay", "[ui]")
{
	// #1795: the editor reads the whole project after every change, so an edit made before the read of the one before
	// it arrived starts from the project as it was before that one. Each edit must keep what the one before did.
	Fixture f;
	const size_t steps = f.app.call("history.get").at("steps").size();
	// The erase tool deletes the first note's point; the draw tool draws two more on both notes, then the erase tool
	// deletes the last one drawn, which this copy of the project doesn't have yet. All before any answer is handed on.
	f.keys().setTool(ui::KeyTool::Erase);
	f.click(0.25, 0.5);
	f.keys().setTool(ui::KeyTool::Draw);
	f.click(0.6, 0.25);
	f.click(0.85, 0.75);
	f.keys().setTool(ui::KeyTool::Erase);
	f.click(0.85, 0.5);

	// The engine has done what the UI sent before this call. Only the first point drawn stays, on both notes.
	f.app.call("engine.version");
	for (size_t note = 0; note < 2; ++note)
	{
		const Json points = f.points(note);
		INFO("note " << note << ": " << points.dump());
		REQUIRE(points.size() == 1);
		CHECK(points.at(0).at("type") == "tuning");
		CHECK(std::abs(points.at(0).at("position").get<double>() - (0.6 * 2 * g_quarter)) <= g_quarter / 16.0);
		CHECK(std::abs(points.at(0).at("value").get<double>() - 24.0) <= 2.0);
	}
	// The editor shows them once the answers arrive. Each edit is an undo step of its own, with the same name whether
	// it changed one part, as the first erase did, or both (#1831).
	REQUIRE(f.app.waitUntil([&] { return f.lines() == 2; }));
	const Json history = f.app.call("history.get").at("steps");
	REQUIRE(history.size() == steps + 4);
	for (size_t i = steps; i < history.size(); ++i)
		CHECK(history.at(i).at("action") == "Change note expression");

	// Now that the editor has the project with its last edit, an erase where neither note has a point sends nothing:
	// the project doesn't count as modified, also with notes of two parts, which would go as one history.transaction.
	const Json revision = f.app.call("project.revision");
	f.click(0.1, 0.5);
	f.app.call("engine.version");
	CHECK(f.app.call("project.revision") == revision);
	CHECK(f.app.call("history.get").at("steps").size() == steps + 4);

	// Undoing the four edits brings the first note's point back and leaves the other one without points.
	for (int i = 0; i < 4; ++i)
		f.app.call("history.undo");
	CHECK(f.points(0) == Json::array({f.tuning}));
	CHECK(f.points(1) == Json::array());
}

TEST_CASE(
	"an expression edit made after the last one was answered, but before the read after it is shown, keeps it", "[ui]")
{
	// #1795, review of #1821: the engine's answer to an edit and its history.changed come in one step, the project read
	// and the view that show it in the steps after. An edit in between starts from the project before the last one.
	Fixture f;
	const Json old = f.app.call("project.get");
	const Json view = f.app.call("editor.get", {{"editor", "window"}});
	// A point drawn on both notes, answered and shown.
	f.keys().setTool(ui::KeyTool::Draw);
	f.click(0.6, 0.25);
	f.app.call("engine.version");
	REQUIRE(f.points(1).size() == 1);
	REQUIRE(f.app.waitUntil([&] { return f.lines() == 3; }));
	// The editor shows the project as it was before the draw, read before the draw was answered, as it does until the
	// read after the answer arrives. An erase where the point is must delete it, also where this copy has no point.
	f.keys().show(old, view, 0);
	REQUIRE(f.lines() == 1);
	f.keys().setTool(ui::KeyTool::Erase);
	f.click(0.6, 0.5);
	f.app.call("engine.version");
	CHECK(f.points(0) == Json::array({f.tuning}));
	CHECK(f.points(1) == Json::array());
}

TEST_CASE("an expression edit that changed nothing leaves the editor as it was", "[ui]")
{
	// #1831: an edit that changes nothing brings no history.changed, so no read of the project after it. Answered
	// after the read shown, it must not count as missing from it: else every gesture would send each note it reached,
	// and one over notes of two parts that changes nothing would go as a history.transaction, which counts as a change.
	Fixture f;
	ui::EngineBinding& binding = f.app.appUi().binding();
	// A point drawn on both notes, answered and shown.
	f.keys().setTool(ui::KeyTool::Draw);
	f.click(0.6, 0.25);
	REQUIRE(f.app.waitUntil([&] { return f.lines() == 3; }));
	const Json project = f.app.call("project.get");
	const Json view = f.app.call("editor.get", {{"editor", "window"}});
	const uint64_t read = binding.handed(); // a read after the draw was answered
	// While the editor shows a read from before the draw, an erase where neither note has a point goes to both
	// notes, as the draw might have drawn there, and changes nothing. (As a history.transaction it counts as a change
	// of the project, #1258.)
	f.keys().show(project, view, 0);
	f.keys().setTool(ui::KeyTool::Erase);
	f.click(0.1, 0.5);
	REQUIRE(binding.waiting("history.transaction") == 1);
	REQUIRE(f.app.waitUntil([&] { return binding.waiting("history.transaction") == 0; }));
	const Json revision = f.app.call("project.revision");
	// The read after the draw shows now, as it would have before the erase was answered. With no edit missing from
	// it, another erase where there is nothing sends nothing.
	f.keys().show(project, view, read);
	f.click(0.2, 0.5);
	CHECK(binding.waiting("history.transaction") == 0);
	CHECK(binding.waiting("note.replaceExpression") == 0);
	f.app.call("engine.version");
	CHECK(f.app.call("project.revision") == revision);
}

TEST_CASE("an expression edit of one note that changed nothing leaves the editor as it was", "[ui]")
{
	// #1831: the same with only the first note selected, where an edit goes as a lone note.replaceExpression, whose
	// answer lists no notes when it changed nothing.
	Fixture f;
	ui::EngineBinding& binding = f.app.appUi().binding();
	f.app.call("editor.set",
		{{"editor", "window"}, {"selection", Json::array({{{"part", f.parts[0]}, {"note", f.notes[0]}}})}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note.selected").size() == 1; }));
	REQUIRE(!f.has(".key-expression-editor", "off"));
	// A point drawn on the first note, answered and shown.
	f.keys().setTool(ui::KeyTool::Draw);
	f.click(0.6, 0.25);
	REQUIRE(f.app.waitUntil([&] { return f.lines() == 2; }));
	CHECK(f.points(1) == Json::array());
	const Json project = f.app.call("project.get");
	const Json view = f.app.call("editor.get", {{"editor", "window"}});
	const uint64_t read = binding.handed(); // a read after the draw was answered
	// While the editor shows a read from before the draw, an erase where the note has no point goes out, as the draw
	// might have drawn there, and changes nothing.
	f.keys().show(project, view, 0);
	f.keys().setTool(ui::KeyTool::Erase);
	f.click(0.1, 0.5);
	REQUIRE(binding.waiting("note.replaceExpression") == 1);
	REQUIRE(f.app.waitUntil([&] { return binding.waiting("note.replaceExpression") == 0; }));
	const Json revision = f.app.call("project.revision");
	// With the read after the draw shown, another erase where there is nothing sends nothing.
	f.keys().show(project, view, read);
	f.click(0.2, 0.5);
	CHECK(binding.waiting("note.replaceExpression") == 0);
	CHECK(binding.waiting("history.transaction") == 0);
	f.app.call("engine.version");
	CHECK(f.app.call("project.revision") == revision);
}

TEST_CASE("MIDI > Note Expression > Convert to Note Expression converts the editor's parts in one undo step", "[ui]")
{
	// #1515, reference manual p. 1316: each part's controllers that its track's setup assigns go into its notes.
	Fixture f;
	f.app.appUi().showProject();
	for (const char* item : {"#midi-menu #midi-note-expression-submenu", "menuitem[command=midi.convertToNoteExpression]"})
		CHECK(f.app.ui().dom(std::nullopt, item, 0).at("elements").size() == 1);
	const Json project = f.app.call("project.get");
	for (const Json& part : project.at("midiParts"))
	{
		f.app.call("noteExpression.set",
			{{"track", part.at("track")},
				{"inputs", {{{"type", "brightness"}, {"input", "cc1"}, {"min", 0.0}, {"max", 1.0}}}}});
		f.app.call("controller.add",
			{{"part", part.at("id")},
				{"controllers", {{{"position", g_quarter}, {"number", 1}, {"value", 1.0}, {"channel", 0}}}}});
	}
	const size_t steps = f.app.call("history.get").at("steps").size();
	ui::Window* window = f.window ? f.app.shell().find(*f.window) : nullptr;
	REQUIRE(window != nullptr);
	Rml::Element* zone = window->context().GetDocument(0)->GetElementById("main-zone");
	REQUIRE(zone != nullptr);
	f.app.keys().run("midi.convertToNoteExpression", Json::object(), *window, zone);
	f.app.call("engine.version");
	const Json brightness = {{"type", "brightness"}, {"position", g_quarter}, {"value", 1.0}};
	CHECK(f.points(0) == Json::array({f.tuning, brightness}));
	CHECK(f.points(1) == Json::array({brightness}));
	const Json after = f.app.call("project.get");
	for (const Json& part : after.at("midiParts"))
		CHECK(part.at("controllers").empty());
	const Json history = f.app.call("history.get").at("steps");
	REQUIRE(history.size() == steps + 1);
	CHECK(history.back().at("action") == "Convert to note expression");
}
