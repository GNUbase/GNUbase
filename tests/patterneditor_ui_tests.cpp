// SPDX-License-Identifier: GPL-3.0-or-later
// The pattern editor in the app (#181, docs/features/pattern-editor.md), driven with ui::Automation as MCP's UI tools
// drive it: the step lanes of a pattern event's pattern, the draw and step selection tools, the lane controls, the
// parameter lane, the toolbar's settings and pattern management. tests/patterns_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_bar = 4 * time::g_ticksPerQuarter;

	ui::Target of(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	std::string cell(int pitch, int index)
	{
		return ".pattern-lane[pitch=\"" + std::to_string(pitch) + "\"] .pattern-step[index=\"" + std::to_string(index) +
			"\"]";
	}

	// An instrument track with a one-bar pattern event of a new drum pattern, open in the editor window.
	struct Fixture
	{
		Fixture()
		{
			track = app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
			pattern = app.call("pattern.add", {{"track", track}}).at("pattern").at("id").get<uint64_t>();
			part =
				app.call("midiPart.add",
					   {{"track", track}, {"position", 0}, {"length", g_bar}, {"name", "Beat"}, {"pattern", pattern}})
					.at("part")
					.at("id")
					.get<uint64_t>();
			app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part})}});
			REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
			window = app.appUi().editorWindowId();
			REQUIRE(app.waitUntil([&] { return elements(".pattern-editor .pattern-step").size() == 64; }));
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

		std::string text(const std::string& selector)
		{
			const Json found = elements(selector);
			return found.empty() ? "" : found.at(0).value("text", "");
		}

		Json patternJson(uint64_t id = 0)
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("patterns"))
				if (each.at("id") == (id != 0 ? id : pattern))
					return each;
			return nullptr;
		}

		// The indexes of the steps of the lane of that pitch.
		std::vector<int> steps(int pitch)
		{
			std::vector<int> indexes;
			const Json json = patternJson();
			for (const Json& lane : json.at("lanes"))
				if (lane.at("pitch") == pitch)
					for (const Json& step : lane.at("steps"))
						indexes.push_back(step.at("index").get<int>());
			return indexes;
		}

		void click(const ui::Target& target, int count = 1, std::vector<std::string> modifiers = {})
		{
			app.ui().click(window, target, ui::MouseButton::Left, count, std::move(modifiers));
			app.shell().step(std::chrono::milliseconds(0));
		}

		ui::Target center(const std::string& selector, double down = 0.5)
		{
			const Json box = elements(selector).at(0).at("box");
			return {"", 0,
				ui::Point{
					static_cast<int>(std::lround(box.at("x").get<double>() + (box.at("width").get<double>() / 2))),
					static_cast<int>(
						std::lround(box.at("y").get<double>() + (box.at("height").get<double>() * down)))}};
		}

		test::UiApp app{{1200, 800}};
		std::optional<ui::WindowId> window;
		uint64_t track = 0;
		uint64_t pattern = 0;
		uint64_t part = 0;
	};
}

TEST_CASE("the pattern editor shows a pattern event's step lanes, and the draw tool adds and removes steps", "[ui]")
{
	Fixture f;
	CHECK(f.app.call("editor.get", {{"editor", "window"}}).at("content") == "pattern");
	CHECK(f.elements(".pattern-lane").size() == 4);
	CHECK(f.text(".pattern-lane[pitch=\"36\"] .pattern-lane-name") == "Bass Drum 1");
	CHECK(f.text("#pattern-select") == "Pattern 1");
	CHECK(f.text("#pattern-length") == "16 Steps");
	CHECK(f.has(".pattern-mode[pattern-mode=drum]", "checked"));
	CHECK(f.has(".pattern-tool[pattern-tool=draw]", "checked"));

	// A click adds a step, another removes it (p. 923).
	f.click(of(cell(38, 4)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(38) == std::vector<int>{4}; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(cell(38, 4), "on"); }));
	f.click(of(cell(38, 4)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(38).empty(); }));
	// A drag to the right adds a series of steps, in one undo step.
	f.app.ui().drag(f.window, f.center(cell(36, 0)), f.center(cell(36, 3)));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{0, 1, 2, 3}; }));
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return !f.has(cell(36, 0), "on"); }));
	CHECK(f.steps(36).empty());
}

TEST_CASE("the step selection tool selects and deletes steps, and the edit functions tie and split them", "[ui]")
{
	Fixture f;
	f.app.call("pattern.set",
		{{"pattern", f.pattern},
			{"lanes", {{{"pitch", 42}, {"steps", {{{"index", 0}}, {{"index", 1}}, {{"index", 8}}}}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.has(cell(42, 8), "on"); }));
	f.click(of(".pattern-tool[pattern-tool=select]"));
	REQUIRE(f.has(".pattern-tool[pattern-tool=select]", "checked"));
	// Tie Adjacent Steps without a selection ties all.
	f.click(of("#pattern-tie"));
	REQUIRE(f.app.waitUntil([&] { return f.steps(42) == std::vector<int>{0, 8}; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(cell(42, 1), "tied"); }));
	f.click(of("#pattern-split"));
	REQUIRE(f.app.waitUntil([&] { return f.steps(42) == std::vector<int>{0, 1, 8}; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(cell(42, 1), "on"); }));
	// A click selects, Shift adds; Delete deletes the selected steps.
	f.click(of(cell(42, 1)));
	f.click(of(cell(42, 8)), 1, {"shift"});
	REQUIRE(f.has(cell(42, 8), "selected"));
	REQUIRE(f.has(cell(42, 1), "selected"));
	CHECK(!f.has(cell(42, 0), "selected"));
	f.app.ui().key(f.window, "Delete");
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.steps(42) == std::vector<int>{0}; }));
	// A double click adds a step (p. 924).
	f.click(of(cell(42, 5)), 2);
	REQUIRE(f.app.waitUntil([&] { return f.steps(42) == std::vector<int>{0, 5}; }));
}

TEST_CASE("the lane controls, the parameter lane and the toolbar change the pattern", "[ui]")
{
	Fixture f;
	f.click(of(cell(36, 0)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{0}; }));
	// Mute and solo.
	f.click(of(".pattern-lane[pitch=\"38\"] .pattern-lane-mute"));
	REQUIRE(f.app.waitUntil([&] { return f.patternJson().at("lanes").at(1).at("mute") == true; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(".pattern-lane[pitch=\"38\"] .pattern-lane-mute", "checked"); }));

	// The parameter lane shows the selected lane's steps; a click near the top of a bar sets a high velocity.
	REQUIRE(f.app.waitUntil([&] { return f.elements(".pattern-bar").size() == 1; }));
	f.click(f.center(".pattern-bar-cell[index=\"0\"]", 0.05));
	REQUIRE(f.app.waitUntil(
		[&] { return f.patternJson().at("lanes").at(0).at("steps").at(0).at("velocity").get<int>() > 115; }));
	f.click(of(".pattern-parameter[parameter=probability]"));
	REQUIRE(f.has(".pattern-parameter[parameter=probability]", "checked"));
	f.click(f.center(".pattern-bar-cell[index=\"0\"]", 0.5));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return std::abs(f.patternJson().at("lanes").at(0).at("steps").at(0).at("probability").get<double>() - 0.5) <
				0.1;
		}));

	// Number of Steps, Step Resolution and Play Direction from their menus, Double Length with the steps.
	f.click(of("#pattern-length"));
	f.click(of("#pattern-length-menu menuitem[length=\"8\"]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".pattern-lane[pitch=\"36\"] .pattern-step").size() == 8; }));
	f.click(of("#pattern-resolution"));
	f.click(of("#pattern-resolution-menu menuitem[resolution=\"1/8\"]"));
	REQUIRE(f.app.waitUntil([&] { return f.patternJson().at("resolution") == "1/8"; }));
	f.click(of("#pattern-direction"));
	f.click(of("#pattern-direction-menu menuitem[direction=\"alternating\"]"));
	REQUIRE(f.app.waitUntil([&] { return f.patternJson().at("direction") == "alternating"; }));
	f.click(of("#pattern-double-length"));
	REQUIRE(f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{0, 8}; }));
	CHECK(f.patternJson().at("length") == 16);

	// Add Lane and melodic mode, which shows a row per pitch.
	f.click(of("#pattern-add-lane"));
	REQUIRE(f.app.waitUntil([&] { return f.patternJson().at("lanes").size() == 5; }));
	f.click(of(".pattern-mode[pattern-mode=poly]"));
	REQUIRE(f.app.waitUntil([&] { return f.patternJson().at("mode") == "poly"; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".pattern-lane").size() == 25; }));
	f.click(of(cell(60, 2)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(60) == std::vector<int>{2}; }));
}

TEST_CASE("pattern management makes a part a pattern event, duplicates and converts patterns", "[ui]")
{
	Fixture f;
	// A part without a pattern offers new ones.
	const uint64_t plain = f.app.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
							   .at("part")
							   .at("id")
							   .get<uint64_t>();
	f.app.call("editor.set", {{"editor", "window"}, {"parts", {f.part, plain}}, {"activePart", plain}});
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#pattern-new-melodic").empty(); }));
	f.click(of("#pattern-new-melodic"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("patterns").size() == 2; }));
	const Json added = f.app.call("project.get").at("patterns").at(1);
	CHECK(added.at("mode") == "poly");
	REQUIRE(f.app.waitUntil([&] { return f.text("#pattern-select") == "Pattern 2"; }));

	// The menu lists the track's patterns, the part's checked; choosing one makes the part play it.
	f.click(of("#pattern-select"));
	CHECK(f.elements("#pattern-menu menuitem[pattern-id]").size() == 2);
	CHECK(f.elements("#pattern-menu menuitem[checked]").at(0).value("text", "") == "Pattern 2");
	f.click(of("#pattern-menu menuitem[pattern-id=\"" + std::to_string(f.pattern) + "\"]"));
	REQUIRE(f.app.waitUntil([&] { return f.text("#pattern-select") == "Pattern 1"; }));
	f.click(of("#pattern-select"));
	f.click(of("#pattern-menu menuitem[pattern-action=duplicate]"));
	REQUIRE(f.app.waitUntil([&] { return f.text("#pattern-select") == "Pattern 1 Copy"; }));
	f.click(of("#pattern-select"));
	f.click(of("#pattern-menu menuitem[pattern-action=convert]"));
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#pattern-new-drum").empty(); }));
	const Json converted = f.app.call("project.get");
	for (const Json& each : converted.at("midiParts"))
		if (each.at("id") == plain)
			CHECK(each.at("pattern") == 0);
}

TEST_CASE("Open Pattern Editor opens the selected MIDI parts in the pattern editor window", "[ui]")
{
	test::UiApp app{{1200, 800}};
	ui::EventDisplay& display = app.appUi().showProject();
	const auto track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	REQUIRE(
		app.waitUntil([&] { return display.project().is_object() && display.project().at("midiParts").size() == 1; }));
	display.select({part});
	app.shell().step(std::chrono::milliseconds(0));
	for (const char* command : {"editor.openPattern", "pattern.loadPreset", "pattern.savePreset"})
		CHECK(app.ui()
				  .dom(std::nullopt, std::string("#edit-menu menuitem[command=\"") + command + "\"]", 0)
				  .at("elements")
				  .size() == 1);
	app.keys().run("editor.openPattern", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	CHECK(app.call("editor.get", {{"editor", "window"}}).at("content") == "pattern");
	const ui::WindowId window = app.appUi().editorWindowId();
	// The part plays no pattern yet: the editor offers new ones.
	REQUIRE(app.waitUntil([&] { return app.ui().dom(window, "#pattern-new-drum", 0).at("elements").size() == 1; }));
}
