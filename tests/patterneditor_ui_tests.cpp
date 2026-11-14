// SPDX-License-Identifier: GPL-3.0-or-later
// The pattern editor in the app (#181, docs/features/pattern-editor.md), driven with ui::Automation as MCP's UI tools
// drive it: the step lanes of a pattern event's pattern, the draw and step selection tools, the lane controls, the
// parameter lane, the toolbar's settings and pattern management. tests/patterns_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/midieditor.h"
#include "ui/patterneditor.h"
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

		// The lane of that pitch, null for none.
		Json lane(int pitch)
		{
			const Json json = patternJson();
			for (const Json& each : json.at("lanes"))
				if (each.at("pitch") == pitch)
					return each;
			return nullptr;
		}

		// The indexes of the steps of the lane of that pitch.
		std::vector<int> steps(int pitch)
		{
			std::vector<int> indexes;
			const Json found = lane(pitch);
			if (found.is_object())
				for (const Json& step : found.at("steps"))
					indexes.push_back(step.at("index").get<int>());
			return indexes;
		}

		// Waits until the editor has the pattern as the engine has it: it hears of a change and then asks for the
		// project, which takes a while in sanitized builds.
		void settle()
		{
			REQUIRE(app.waitUntil(
				[&]
				{
					ui::MidiEditor* shown = app.appUi().editorWindow();
					const ui::PatternEditor* editor = shown != nullptr ? shown->patternEditor() : nullptr;
					if (editor == nullptr)
						return false;
					const Json expected = patternJson();
					for (const Json& each : editor->project().value("patterns", Json::array()))
						if (each.at("id") == pattern)
							return each == expected;
					return false;
				}));
		}

		void click(const ui::Target& target, int count = 1, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(window, target, ui::MouseButton::Left, count, modifiers);
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

TEST_CASE("a draw gesture keeps its steps when the project changes under it, and a refused one goes", "[ui]")
{
	Fixture f;
	// A change from elsewhere while the gesture goes on, e.g. a script's, doesn't drop the steps it drew (#1040).
	f.app.ui().press(f.window, f.center(cell(36, 0)), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.has(cell(36, 0), "on"));
	f.app.call("pattern.set", {{"pattern", f.pattern}, {"swing", 0.5}});
	f.settle();
	f.app.ui().move(f.window, f.center(cell(36, 1)));
	f.app.ui().move(f.window, f.center(cell(36, 2)));
	f.app.ui().release(f.window, f.center(cell(36, 2)), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{0, 1, 2}; });
	REQUIRE(f.steps(36) == std::vector<int>{0, 1, 2});
	CHECK(f.patternJson().at("swing") == 0.5);

	// Steps that changed meanwhile on the lanes the gesture doesn't edit stay as the engine has them (#1091).
	f.app.ui().press(f.window, f.center(cell(42, 0)), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.has(cell(42, 0), "on"));
	Json lanes = f.patternJson().at("lanes");
	for (Json& each : lanes)
		if (each.at("pitch") == 38)
			each["steps"] = {{{"index", 8}}};
	f.app.call("pattern.set", {{"pattern", f.pattern}, {"lanes", lanes}});
	f.settle();
	f.app.ui().move(f.window, f.center(cell(42, 1)));
	f.app.ui().release(f.window, f.center(cell(42, 1)), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.waitUntil([&] { return f.steps(42) == std::vector<int>{0, 1}; });
	REQUIRE(f.steps(42) == std::vector<int>{0, 1});
	CHECK(f.steps(38) == std::vector<int>{8});
	CHECK(f.steps(36) == std::vector<int>{0, 1, 2});
	f.app.call("history.undo");
	CHECK(f.steps(42).empty());
	CHECK(f.steps(38) == std::vector<int>{8});

	// Steps the pattern no longer has room for are refused: the editor shows the engine's pattern again.
	f.app.ui().press(f.window, f.center(cell(38, 4)), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.call("pattern.set", {{"pattern", f.pattern}, {"length", 4}});
	f.settle();
	f.app.ui().release(f.window, f.center(cell(38, 4)), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".pattern-lane[pitch=\"38\"] .pattern-step").size() == 4; }));
	CHECK(f.steps(38).empty());
	CHECK(f.text("#pattern-length") == "4 Steps");
}

TEST_CASE("a project read before the editor's last pattern.set doesn't undo it", "[ui]")
{
	Fixture f;
	ui::PatternEditor* editor = f.app.appUi().editorWindow()->patternEditor();
	REQUIRE(editor != nullptr);
	const Json old = editor->project();
	const Json view = f.app.call("editor.get", {{"editor", "window"}});
	f.click(of(cell(36, 0)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{0}; }));
	f.settle();
	// A project that was read before it shows after it, e.g. a project.get answered before it, or the project the
	// editor has with a newer view (#1202): the lanes sent stay, and the next gesture keeps them.
	editor->show(old, view, 0);
	CHECK(f.has(cell(36, 0), "on"));
	f.click(of(cell(38, 4)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(38) == std::vector<int>{4}; }));
	CHECK(f.steps(36) == std::vector<int>{0});
	f.settle();
	// The same while a gesture goes on: it doesn't take the other lanes from that project.
	f.app.ui().press(f.window, f.center(cell(42, 0)), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	editor->show(old, view, 0);
	f.app.ui().release(f.window, f.center(cell(42, 0)), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.steps(42) == std::vector<int>{0}; }));
	CHECK(f.steps(36) == std::vector<int>{0});
	CHECK(f.steps(38) == std::vector<int>{4});
}

TEST_CASE("a gesture right after Double Length keeps the lanes it sent", "[ui]")
{
	Fixture f;
	ui::PatternEditor* editor = f.app.appUi().editorWindow()->patternEditor();
	REQUIRE(editor != nullptr);
	f.click(of(cell(36, 0)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{0}; }));
	f.settle();
	const Json old = editor->project();
	const Json view = f.app.call("editor.get", {{"editor", "window"}});
	f.click(of("#pattern-double-length"));
	REQUIRE(f.app.waitUntil([&] { return f.patternJson().at("length") == 32; }));
	// A project read before Double Length's pattern.set shows after it (#1347): the lanes it sent stay, and the next
	// gesture keeps them.
	editor->show(old, view, 0);
	CHECK(f.has(cell(36, 16), "on"));
	f.click(of(cell(38, 20)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(38) == std::vector<int>{20}; }));
	CHECK(f.steps(36) == std::vector<int>{0, 16});
	CHECK(f.patternJson().at("length") == 32);
	// That project shown once more after the gesture still has the length Double Length sent (#1410).
	editor->show(old, view, 0);
	CHECK(f.text("#pattern-length") == "32 Steps");
	CHECK(f.elements(".pattern-lane[pitch=\"36\"] .pattern-step").size() == 32);
	CHECK(f.has(cell(36, 16), "on"));
	CHECK(f.has(cell(38, 20), "on"));
}

TEST_CASE("a gesture right after a mode switch edits the pattern as the engine has it", "[ui]")
{
	Fixture f;
	ui::PatternEditor* editor = f.app.appUi().editorWindow()->patternEditor();
	REQUIRE(editor != nullptr);
	f.click(of(cell(36, 0)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{0}; }));
	f.settle();
	const Json old = editor->project();
	const Json view = f.app.call("editor.get", {{"editor", "window"}});
	// Switching from drum to melodic mode clears the lanes (p. 913).
	f.click(of(".pattern-mode[pattern-mode=poly]"));
	REQUIRE(f.app.waitUntil([&] { return f.has(".pattern-mode[pattern-mode=poly]", "checked"); }));
	REQUIRE(f.patternJson().at("lanes").empty());
	// A project read before the switch shows after it (#1410): the pattern shows as the engine has it, without the
	// lanes, and the next gesture doesn't bring them back.
	editor->show(old, view, 0);
	CHECK(f.has(".pattern-mode[pattern-mode=poly]", "checked"));
	CHECK_FALSE(f.has(cell(36, 0), "on"));
	f.click(of(cell(60, 4)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(60) == std::vector<int>{4}; }));
	CHECK(f.patternJson().at("lanes").size() == 1);
	CHECK(f.patternJson().at("mode") == "poly");
}

TEST_CASE("the pattern editor takes no edit of the steps before a mode switch's answer", "[ui]")
{
	Fixture f;
	f.click(of(cell(36, 0)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{0}; }));
	f.settle();
	// The answer comes only in a step: the click on a step goes while the switch is out, with the drum lanes still
	// shown, and doesn't bring them back into the cleared melodic pattern (#1410).
	f.app.ui().click(f.window, of(".pattern-mode[pattern-mode=poly]"), ui::MouseButton::Left, 1, {});
	f.app.ui().click(f.window, of(cell(36, 4)), ui::MouseButton::Left, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.has(".pattern-mode[pattern-mode=poly]", "checked"); }));
	f.settle();
	CHECK(f.patternJson().at("mode") == "poly");
	CHECK(f.patternJson().at("lanes").empty());
	CHECK(f.elements(".pattern-step.on").empty());
}

TEST_CASE("while a mode switch is out, the editor neither edits the lanes nor shows them in the new mode", "[ui]")
{
	Fixture f;
	ui::PatternEditor* editor = f.app.appUi().editorWindow()->patternEditor();
	REQUIRE(editor != nullptr);
	f.click(of(cell(36, 0)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{0}; }));
	f.settle();
	const Json old = editor->project();
	const Json view = f.app.call("editor.get", {{"editor", "window"}});
	// The answer comes only in a step: the mute goes while the switch is out. It doesn't show, and doesn't bring the
	// drum lanes back into the cleared melodic pattern (#1532, #1535).
	const std::string mute = ".pattern-lane[pitch=\"38\"] .pattern-lane-mute";
	f.app.ui().click(f.window, of(".pattern-mode[pattern-mode=poly]"), ui::MouseButton::Left, 1, {});
	f.app.ui().click(f.window, of(mute), ui::MouseButton::Left, 1, {});
	CHECK_FALSE(f.has(mute, "checked"));
	// A project read before the switch shows as it is until the answer, not its drum lanes as rows of melodic mode.
	editor->show(old, view, 0);
	CHECK(f.has(".pattern-mode[pattern-mode=drum]", "checked"));
	CHECK(f.elements(".pattern-lane").size() == 4);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.has(".pattern-mode[pattern-mode=poly]", "checked"); }));
	f.settle();
	CHECK(f.patternJson().at("mode") == "poly");
	CHECK(f.patternJson().at("lanes").empty());
	CHECK(f.elements(".pattern-step.on").empty());
}

TEST_CASE("a shorter Number of Steps edits the pattern as the engine has it, after its answer only", "[ui]")
{
	Fixture f;
	ui::PatternEditor* editor = f.app.appUi().editorWindow()->patternEditor();
	REQUIRE(editor != nullptr);
	f.click(of(cell(36, 12)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(36) == std::vector<int>{12}; }));
	f.settle();
	const Json old = editor->project();
	const Json view = f.app.call("editor.get", {{"editor", "window"}});
	// 8 Steps drops the step at 12 (#1410).
	f.click(of("#pattern-length"));
	f.click(of("#pattern-length-menu menuitem[length=\"8\"]"));
	REQUIRE(f.app.waitUntil([&] { return f.text("#pattern-length") == "8 Steps"; }));
	REQUIRE(f.steps(36).empty());
	// A project read before it shows after it (#1532): 8 steps without the step that went, also not as a tie further
	// left, and the next gesture's pattern.set doesn't bring it back, which the engine would refuse.
	editor->show(old, view, 0);
	CHECK(f.text("#pattern-length") == "8 Steps");
	CHECK(f.elements(".pattern-lane[pitch=\"36\"] .pattern-step").size() == 8);
	CHECK(f.elements(".pattern-step.on").empty());
	CHECK(f.elements(".pattern-step.tied").empty());
	f.click(of(cell(38, 2)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(38) == std::vector<int>{2}; }));
	CHECK(f.steps(36).empty());
	f.settle();
	// Until the answer of 4 Steps the editor takes no edit of the steps: a click on a step right after it goes.
	f.click(of("#pattern-length"));
	f.app.ui().click(f.window, of("#pattern-length-menu menuitem[length=\"4\"]"), ui::MouseButton::Left, 1, {});
	f.app.ui().click(f.window, of(cell(42, 1)), ui::MouseButton::Left, 1, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.text("#pattern-length") == "4 Steps"; }));
	f.settle();
	CHECK(f.steps(42).empty());
	CHECK(f.steps(38) == std::vector<int>{2});
}

TEST_CASE("in mono mode the draw tool moves tied steps to the pitch drawn", "[ui]")
{
	Fixture f;
	f.app.call("pattern.set",
		{{"pattern", f.pattern}, {"mode", "mono"},
			{"lanes", {{{"pitch", 60}, {"steps", {{{"index", 0}, {"length", 4}, {"velocity", 90}}}}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.has(cell(60, 3), "tied"); }));
	// In the middle of a tie, the tied steps from there on move (p. 913).
	f.click(of(cell(62, 2)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(62) == std::vector<int>{2}; }));
	CHECK(f.lane(60).at("steps").at(0).at("length") == 2);
	CHECK(f.lane(62).at("steps").at(0).at("length") == 2);
	CHECK(f.lane(62).at("steps").at(0).at("velocity") == 90);
	// At its start, all of them.
	REQUIRE(f.app.waitUntil([&] { return f.has(cell(60, 1), "tied"); }));
	f.click(of(cell(64, 0)));
	REQUIRE(f.app.waitUntil([&] { return f.steps(64) == std::vector<int>{0}; }));
	CHECK(f.steps(60).empty());
	CHECK(f.lane(64).at("steps").at(0).at("length") == 2);
}

TEST_CASE("Double Step Resolution and Double Length leave the lanes' rhythms as they were", "[ui]")
{
	Fixture f;
	const Json one = Json::array({{{"index", 1}}});
	f.app.call("pattern.set",
		{{"pattern", f.pattern},
			{"lanes",
				{{{"pitch", 36}, {"steps", one}}, {{"pitch", 38}, {"length", 3}, {"steps", one}},
					{{"pitch", 42}, {"resolution", "1/8"}, {"steps", one}},
					{{"pitch", 46}, {"length", 100}, {"steps", {{{"index", 99}}}}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.has(cell(46, 99), "on"); }));
	// The lanes that follow the pattern's resolution get twice the steps, those of their own length too; one that
	// can't keeps the resolution as its own, and one of its own resolution stays.
	f.click(of("#pattern-double-resolution"));
	REQUIRE(f.app.waitUntil([&] { return f.patternJson().at("resolution") == "1/32"; }));
	CHECK(f.patternJson().at("length") == 32);
	CHECK(f.lane(36).at("steps").at(0) ==
		Json({{"index", 2}, {"length", 2}, {"velocity", 100}, {"repeats", 1}, {"offset", 0.0}, {"probability", 1.0},
			{"velocityVariance", 0.0}, {"gate", 1.0}}));
	CHECK(f.lane(38).at("length") == 6);
	CHECK(f.steps(38) == std::vector<int>{2});
	CHECK(f.lane(42).at("resolution") == "1/8");
	CHECK(f.lane(42).at("length") == 16);
	CHECK(f.steps(42) == std::vector<int>{1});
	CHECK(f.lane(46).at("length") == 100);
	CHECK(f.lane(46).at("resolution") == "1/16");
	CHECK(f.steps(46) == std::vector<int>{99});
	// Double Length repeats the steps of the lanes that follow the pattern's length; the others loop on their own.
	REQUIRE(f.app.waitUntil([&] { return f.elements(".pattern-lane[pitch=\"36\"] .pattern-step").size() == 32; }));
	f.click(of("#pattern-double-length"));
	REQUIRE(f.app.waitUntil([&] { return f.patternJson().at("length") == 64; }));
	CHECK(f.steps(36) == std::vector<int>{2, 34});
	CHECK(f.steps(38) == std::vector<int>{2});
	CHECK(f.lane(46).at("length") == 100);
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
	REQUIRE(app.waitUntil([&] { return display.project().at("midiParts").size() == 1; }));
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
