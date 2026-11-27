// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of the mouse pointer by context (#469, #1857, docs/features/pointers.md): hovering an element through
// ui::Automation shows the pointer that says what a drag or click there does, as MCP's ui_windows reports the one the
// window system was asked for. The event lanes' other tool pointers are in uiapp_tests.cpp and eventediting_tests.cpp,
// the sample and audio part editors' in their own tests.
#include "uiapp.h"

#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/keyeditor.h"
#include "ui/midieditor.h"
#include "ui/tracklist.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	ui::Target at(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	ui::Target point(int x, int y)
	{
		return {"", 0, ui::Point{x, y}};
	}

	// The middle of the one element the selector matches, in the window's pixels.
	ui::Target middle(test::UiApp& app, const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		const Json& box = found[0].at("box");
		return point(box.at("x").get<int>() + (box.at("width").get<int>() / 2),
			box.at("y").get<int>() + (box.at("height").get<int>() / 2));
	}

	// The pointer that shows, by the name the style sheets give it: empty for the OS's arrow.
	std::string pointer(test::UiApp& app)
	{
		app.shell().step(std::chrono::milliseconds(0));
		return app.ui().windows().at("cursor").get<std::string>();
	}

	std::string pointerOver(test::UiApp& app, const ui::Target& target)
	{
		app.ui().move(std::nullopt, target);
		return pointer(app);
	}
}

TEST_CASE("splitters show the OS's double arrow in the direction they move, also while a drag leaves them", "[ui]")
{
	test::UiApp app({1000, 700});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	app.appUi().docking().setVisible("project", "left", true);
	app.appUi().docking().setVisible("project", "lower", true);
	app.shell().step(std::chrono::milliseconds(0));

	// Left-right between the zones of a row and beside the track list, up-down above the lower zone.
	CHECK(pointerOver(app, middle(app, "splitter[for=left]")) == "ew-resize");
	CHECK(pointerOver(app, middle(app, "splitter[for=trackList]")) == "ew-resize");
	CHECK(pointerOver(app, middle(app, "splitter[for=lower]")) == "ns-resize");
	CHECK(pointerOver(app, point(5, 5)).empty());

	// The left zone stops at its minimum of 120 dp, so the mouse leaves the splitter over the zone; the pointer stays
	// the splitter's until the button goes up.
	const ui::Target left = middle(app, "splitter[for=left]");
	const ui::Point from = left.point.value_or(ui::Point{});
	app.ui().press(std::nullopt, left, ui::MouseButton::Left);
	app.ui().move(std::nullopt, point(from.x - 100, from.y));
	app.ui().move(std::nullopt, point(40, from.y));
	CHECK(pointer(app) == "ew-resize");
	app.ui().release(std::nullopt, point(40, from.y), ui::MouseButton::Left);
	CHECK(pointerOver(app, point(41, from.y)) != "ew-resize");
}

TEST_CASE("the MIDI inserts' divider and the edit history's separator show the up-down double arrow", "[ui]")
{
	test::UiApp app({1200, 900});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	app.appUi().docking().open("inspector");
	app.appUi().trackList()->select({app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>()});
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json found =
				app.ui().dom(std::nullopt, ".inspector [section=modifiers] .section-header", 0).at("elements");
			return found.size() == 1 && found[0].at("visible").get<bool>();
		}));
	// The sections above the MIDI inserts closed, so that their divider is in view.
	for (const char* section : {"basics", "routing", "modifiers"})
		app.ui().click(std::nullopt, at(".inspector [section=" + std::string(section) + "] .section-header"),
			ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil(
		[&app] { return !app.ui().dom(std::nullopt, ".inspector .midi-divider[gap=0]", 0).at("elements").empty(); }));
	CHECK(pointerOver(app, middle(app, ".inspector .midi-divider[gap=0]")) == "ns-resize");

	app.keys().run("window.history", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil(
		[&app] { return app.ui().dom(std::nullopt, "#history .history-separator", 0).at("elements").size() == 1; }));
	CHECK(pointerOver(app, middle(app, "#history .history-separator")) == "ns-resize");
	CHECK(pointerOver(app, point(5, 5)).empty());
}

TEST_CASE("text fields show the text I-beam", "[ui]")
{
	test::UiApp app({1024, 500});
	app.appUi().showProject();
	app.keys().run("tempo.insertAtCursor", Json::object(), app.main(), nullptr);
	REQUIRE(
		app.waitUntil([&app] { return !app.ui().dom(std::nullopt, "#insert-tempo-value", 0).at("elements").empty(); }));
	CHECK(pointerOver(app, at("#insert-tempo-value")) == "text");
	CHECK(pointerOver(app, at("#insert-tempo-ok")).empty());
}

// A drag on these does what the element plainly is for, so they keep the OS's arrow (docs/features/pointers.md).
TEST_CASE("dialog title bars, dock tabs and value widgets keep the OS's arrow", "[ui]")
{
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	app.appUi().docking().setVisible("project", "lower", true);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(pointerOver(app, at("tab[panel=mixer]")).empty());

	app.keys().run("tempo.insertAtCursor", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app]
		{ return !app.ui().dom(std::nullopt, "#insert-tempo-dialog .dialog-titlebar", 0).at("elements").empty(); }));
	CHECK(pointerOver(app, at("#insert-tempo-dialog .dialog-titlebar")).empty());
}

TEST_CASE("the time warp tool shows a clock with arrows over the event lanes", "[ui]")
{
	test::UiApp app({800, 300});
	app.setDpRatio(1.0F);
	app.appUi().showProject();
	app.call("track.add", {{"type", "midi"}});
	app.shell().step(std::chrono::milliseconds(0));
	const ui::Target lanes = middle(app, "event-lanes.events");
	app.keys().run("tool.timeWarp", Json::object(), app.main(), nullptr);
	CHECK(pointerOver(app, lanes) == "tool-timewarp");
}

TEST_CASE("the key editor's tools show their pointers over the note display, the trim tool a knife", "[ui]")
{
	test::UiApp app({1200, 800});
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	const Json part =
		app.call("midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 4 * time::g_ticksPerQuarter}})
			.at("part");
	app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part.at("id")})}});
	REQUIRE(app.waitUntil([&app] { return app.appUi().editorWindow() != nullptr; }));
	const ui::WindowId window = app.appUi().editorWindowId();
	REQUIRE(app.waitUntil([&] { return app.ui().dom(window, ".editor-part", 0).at("elements").size() == 1; }));
	const auto over = [&app, window](const std::string& tool)
	{
		if (!tool.empty())
			app.ui().click(window, at(".key-tool[key-tool=" + tool + "]"), ui::MouseButton::Left, 1, {});
		app.ui().move(window, at(".key-editor-grid"));
		return pointer(app);
	};
	CHECK(over("").empty()); // the object selection tool
	CHECK(over("draw") == "tool-draw");
	CHECK(over("line") == "tool-line");
	CHECK(over("erase") == "tool-erase");
	CHECK(over("trim") == "tool-split");
	CHECK(over("split") == "tool-split");
	CHECK(over("glue") == "tool-glue");
	CHECK(over("mute") == "tool-mute");
	CHECK(over("objectSelection").empty());
}

TEST_CASE("a note's ends in the key editor show the resize pointer, unless Shift or Ctrl adds the note instead", "[ui]")
{
	test::UiApp app({1200, 800});
	app.setDpRatio(1.0F);
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	const Json part =
		app.call("midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 4 * time::g_ticksPerQuarter}})
			.at("part");
	app.call("note.add",
		{{"part", part.at("id")},
			{"notes", {{{"position", 0}, {"length", 2 * time::g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 0.5}}}}});
	app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part.at("id")})}});
	REQUIRE(app.waitUntil([&app] { return app.appUi().editorWindow() != nullptr; }));
	const ui::WindowId window = app.appUi().editorWindowId();
	REQUIRE(app.waitUntil([&] { return app.ui().dom(window, ".key-note", 0).at("elements").size() == 1; }));
	REQUIRE(app.waitUntil([&app] { return app.appUi().editorWindow()->keyEditor()->scrollToPitch(60); }));
	app.shell().step(std::chrono::milliseconds(0));
	const Json box = app.ui().dom(window, ".key-note", 0).at("elements").at(0).at("box");
	const int x = box.at("x").get<int>();
	const int y = box.at("y").get<int>() + (box.at("height").get<int>() / 2);
	const int width = box.at("width").get<int>();
	const auto over = [&app, window](int at, int height, const std::vector<std::string>& modifiers = {})
	{
		app.ui().move(window, point(at, height), modifiers);
		return pointer(app);
	};
	// Within 6 dp of an end a press resizes the note (pp. 1213-1214), elsewhere it moves it.
	CHECK(over(x + 2, y) == "resize");
	CHECK(over(x + width - 2, y) == "resize");
	CHECK(over(x + (width / 2), y).empty());
	// The modifiers held while the mouse moves count.
	CHECK(over(x + width - 2, y, {"shift"}).empty());
	CHECK(over(x + width - 3, y, {"ctrl"}).empty());
	CHECK(over(x + width - 2, y) == "resize");
	// The draw tool draws there.
	app.ui().click(window, at(".key-tool[key-tool=draw]"), ui::MouseButton::Left, 1, {});
	CHECK(over(x + width - 2, y) == "tool-draw");
}

TEST_CASE("a mixer channel's name, which a drag moves, shows the pointing hand, its fader the arrow", "[ui]")
{
	test::UiApp app({1200, 800});
	const Json added = app.call("track.add", {{"type", "audio"}});
	const std::string name = ".mixer-channel[channel=" + std::to_string(added.at("channel").at("id").get<uint64_t>()) +
		"] .mixer-channel-name";
	app.keys().run("window.mixer", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.appUi().mixer(1) != nullptr; }));
	const ui::WindowId mixer = app.appUi().mixerWindow(1);
	REQUIRE(app.waitUntil([&] { return app.ui().dom(mixer, name, 0).at("elements").size() == 1; }));
	app.ui().move(mixer, at(name));
	CHECK(pointer(app) == "pointer"); // p. 496
	// The fader, a value widget, keeps the OS's arrow (docs/features/pointers.md).
	const std::string fader = ".mixer-channel[channel=" + std::to_string(added.at("channel").at("id").get<uint64_t>()) +
		"] .mixer-fader fader";
	REQUIRE(app.ui().dom(mixer, fader, 0).at("elements").size() == 1);
	app.ui().move(mixer, at(fader));
	CHECK(pointer(app).empty());
}

TEST_CASE("the controller lanes show the tools' pointers in the drum and the in-place editors too", "[ui]")
{
	// tests/controllerlanes_ui_tests.cpp checks each tool on the key editor's lanes; the style sheets choose by the
	// tool of the .key-editor around them, which the drum and the in-place editors are as well.
	test::UiApp app({1200, 800});
	app.setDpRatio(1.0F);
	const uint64_t track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const Json part =
		app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * time::g_ticksPerQuarter}})
			.at("part");
	const Json lanes = {{{"type", "velocity"}}, {{"type", "controller"}, {"number", 1}}};
	SECTION("the drum editor, whose draw tool is the drumstick in the note display")
	{
		app.call("track.set", {{"track", track}, {"drumMap", app.call("project.get").at("drumMaps").at(0).at("id")}});
		app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part.at("id")})}});
		REQUIRE(app.waitUntil([&app] { return app.appUi().editorWindow() != nullptr; }));
		const ui::WindowId window = app.appUi().editorWindowId();
		app.call("editor.set", {{"editor", "window"}, {"lanes", lanes}});
		const std::string area = ".drum-editor .controller-lane[lane-kind=controller] .controller-lane-area";
		REQUIRE(app.waitUntil([&] { return app.ui().dom(window, area, 0).at("elements").size() == 1; }));
		app.ui().click(window, at(".key-tool[key-tool=draw]"), ui::MouseButton::Left, 1, {});
		app.ui().move(window, at(area));
		CHECK(pointer(app) == "tool-draw");
		app.ui().click(window, at(".key-tool[key-tool=line]"), ui::MouseButton::Left, 1, {});
		app.ui().move(window, at(area));
		CHECK(pointer(app) == "tool-line");
	}
	SECTION("the in-place editor")
	{
		app.appUi().showProject();
		app.appUi().trackList()->select({track});
		app.call("editor.inPlace", {{"tracks", {track}}, {"open", true}});
		app.call("editor.set", {{"editor", "inPlace:" + std::to_string(track)}, {"lanes", lanes}});
		const std::string editor = ".in-place-editor[track=\"" + std::to_string(track) + "\"]";
		const std::string area = editor + " .controller-lane[lane-kind=controller] .controller-lane-area";
		REQUIRE(app.waitUntil([&] { return app.ui().dom(std::nullopt, area, 0).at("elements").size() == 1; }));
		// The toolbar shows with the triangle in the track's row (reference manual p. 1259).
		app.ui().click(std::nullopt,
			at(".track-list listitem[value=\"" + std::to_string(track) + "\"] .in-place-toolbar-toggle"),
			ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
		app.ui().click(std::nullopt, at(editor + " .key-tool[key-tool=draw]"), ui::MouseButton::Left, 1, {});
		app.ui().move(std::nullopt, at(area));
		CHECK(pointer(app) == "tool-draw");
		app.ui().click(std::nullopt, at(editor + " .key-tool[key-tool=erase]"), ui::MouseButton::Left, 1, {});
		app.ui().move(std::nullopt, at(area));
		CHECK(pointer(app).empty()); // no controller event there to erase
	}
}
