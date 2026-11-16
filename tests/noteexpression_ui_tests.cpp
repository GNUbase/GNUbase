// SPDX-License-Identifier: GPL-3.0-or-later
// The key editor's expression editor (#1512, docs/features/note-expression.md) while the engine's answers come late:
// edits made before the project read of the last one arrived (#1795). tests/keyeditor_ui_tests.cpp checks its
// gestures, tests/noteexpression_tests.cpp the engine's note.replaceExpression.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/keyeditor.h"
#include "ui/midieditor.h"
#include "ui/shell.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

	// A MIDI track with a one-bar part at the project start, open in the editor window, at CI's window size.
	struct Fixture
	{
		Fixture() : app({1024, 600})
		{
			const uint64_t track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}, {"name", "Keys"}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
			app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part})}});
			REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
			window = app.appUi().editorWindowId();
			REQUIRE(app.waitUntil([&] { return elements(".editor-part").size() == 1; }));
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

		ui::KeyEditor& keys() { return *app.appUi().editorWindow()->keyEditor(); }

		// The note's points as the engine has them, sorted by position.
		Json points(uint64_t id)
		{
			const Json project = app.call("project.get"); // kept: a range-for keeps no temporary it reaches into
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == part)
					for (const Json& note : each.at("notes"))
						if (note.at("id") == id)
							return note.value("expressions", Json::array());
			return nullptr;
		}

		test::UiApp app;
		std::optional<ui::WindowId> window;
		uint64_t part = 0;
	};
}

TEST_CASE("expression edits made before the project read of the last one arrived all stay", "[ui]")
{
	// #1795: the editor reads the whole project after every change, so an edit made before the read of the one before
	// it arrived starts from the project as it was before that one. Each edit must keep what the one before did.
	Fixture f;
	const Json tuning = {{"type", "tuning"}, {"position", g_quarter / 2}, {"value", 12.0}};
	const uint64_t id = f.app
							.call("note.add",
								{{"part", f.part},
									{"notes",
										{{{"position", 0}, {"length", 2 * g_quarter}, {"pitch", 60}, {"velocity", 0.5},
											{"expressions", Json::array({tuning})}}}}})
							.at("notes")
							.at(0)
							.at("id")
							.get<uint64_t>();
	const std::string note = ".key-note[note=\"" + std::to_string(id) + "\"]";
	REQUIRE(f.app.waitUntil([&] { return f.elements(note).size() == 1; }));
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow()->keyEditor()->scrollToPitch(60); }));
	f.app.call("editor.set", {{"editor", "window"}, {"expressionDoubleClick", true}});
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-input[setting=expressionDoubleClick]", "checked"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.ui().click(f.window, {note, 0, std::nullopt}, ui::MouseButton::Left, 2, {});
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-expression-editor", "off"); }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-area .key-expression-step").size() == 1; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	const size_t steps = f.app.call("history.get").at("steps").size();

	// Clicks in the expression editor's area, x and y in parts of its width and height, straight into the window's
	// context: the UI hands on no answer of the engine in between, so every edit starts from the project as the
	// editor has it now.
	const Json area = f.box(".key-expression-area");
	ui::Window* shown = f.window ? f.app.shell().find(*f.window) : nullptr;
	REQUIRE(shown != nullptr);
	Rml::Context& context = shown->context();
	const auto click = [&](double x, double y)
	{
		context.ProcessMouseMove(
			static_cast<int>(std::lround(area.at("x").get<double>() + (x * area.at("width").get<double>()))),
			static_cast<int>(std::lround(area.at("y").get<double>() + (y * area.at("height").get<double>()))), 0);
		context.ProcessMouseButtonDown(0, 0);
		context.ProcessMouseButtonUp(0, 0);
	};
	// The erase tool deletes the point, the draw tool draws two more, then the erase tool deletes the last one drawn,
	// which this copy of the project doesn't have yet.
	f.keys().setTool(ui::KeyTool::Erase);
	click(0.25, 0.5);
	f.keys().setTool(ui::KeyTool::Draw);
	click(0.6, 0.25);
	click(0.85, 0.75);
	f.keys().setTool(ui::KeyTool::Erase);
	click(0.85, 0.5);

	// The engine has done what the UI sent before this call. Only the first point drawn stays.
	f.app.call("engine.version");
	const Json points = f.points(id);
	INFO(points.dump());
	REQUIRE(points.size() == 1);
	CHECK(points.at(0).at("type") == "tuning");
	CHECK(std::abs(points.at(0).at("position").get<double>() - (0.6 * 2 * g_quarter)) <= g_quarter / 16.0);
	CHECK(std::abs(points.at(0).at("value").get<double>() - 24.0) <= 2.0);
	// The editor shows it once the answers arrive.
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-area .key-expression-step").size() == 1; }));
	// Each edit is an undo step of its own, and undoing them all brings the point back.
	CHECK(f.app.call("history.get").at("steps").size() == steps + 4);
	for (int i = 0; i < 4; ++i)
		f.app.call("history.undo");
	CHECK(f.points(id) == Json::array({tuning}));
}
