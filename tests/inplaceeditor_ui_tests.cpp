// SPDX-License-Identifier: GPL-3.0-or-later
// The in-place editor in the app (#131, docs/features/in-place-editor.md), driven with ui::Automation as MCP's UI
// tools drive it: opened from the MIDI menu, Edit In-Place and the inspector, a key editor over its track's lane with
// the notes on the project window's timeline, editing notes there without the event display taking the presses, and
// closing. tests/inplaceeditor_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/eventdisplay.h"
#include "ui/inplaceeditor.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;

	// A MIDI track, selected, with a one-bar part "Keys" in the second bar and two notes, C3 on its first beat and
	// E3 on its second, each a quarter long, and an audio track below it, in the project window at a dp ratio of 1.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			app.setDpRatio(1.0F);
			track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			audio = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
			part =
				app.call("midiPart.add", {{"track", track}, {"position", g_bar}, {"length", g_bar}, {"name", "Keys"}})
					.at("part")
					.at("id")
					.get<uint64_t>();
			const Json added = app.call("note.add",
				{{"part", part},
					{"notes",
						{{{"position", 0}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 0.5}},
							{{"position", g_quarter}, {"length", g_quarter}, {"pitch", 64}, {"velocity", 0.5}}}}});
			for (const Json& note : added.at("notes"))
				notes.push_back(note.at("id").get<uint64_t>());
			REQUIRE(app.waitUntil(
				[this]
				{
					const Json& project = display.project();
					return project.is_object() && project.at("midiParts").size() == 1 &&
						project.at("midiParts").at(0).at("notes").size() == 2;
				}));
			app.appUi().trackList()->select({track});
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		Json box(const std::string& selector) { return elements(selector).at(0).at("box"); }

		bool has(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.empty())
				return false;
			const Json names = found.at(0).value("classes", Json::array());
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		std::string editor() const { return ".in-place-editor[track=\"" + std::to_string(track) + "\"]"; }

		std::string note(uint64_t id) const { return editor() + " .key-note[note=\"" + std::to_string(id) + "\"]"; }

		Json view() { return app.call("editor.get", {{"editor", "inPlace:" + std::to_string(track)}}); }

		void click(const ui::Target& target, int count = 1)
		{
			app.ui().click(std::nullopt, target, ui::MouseButton::Left, count, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		void click(const std::string& selector)
		{
			INFO(selector);
			REQUIRE(app.waitUntil(
				[&]
				{
					const Json found = elements(selector);
					return !found.empty() && found[0].at("visible").get<bool>();
				}));
			click(ui::Target{selector, 0, std::nullopt});
		}

		// The x of a time on the event display's timeline, in the window's pixels: at 120 BPM, project ticks.
		double timelineX(int64_t ticks)
		{
			const double seconds = static_cast<double>(ticks) / g_quarter * 0.5;
			const ui::TimelineView& shown = display.view();
			return box(".event-area").at("x").get<double>() + ((seconds - shown.start) / shown.scale);
		}

		// A point of the in-place editor's grid at project ticks, on the pitch's row.
		ui::Target at(int64_t ticks, int pitch)
		{
			const Json grid = box(editor() + " .key-editor-grid");
			const double y = grid.at("y").get<double>() + ((127.5 - pitch) / 128.0 * grid.at("height").get<double>());
			return {
				"", 0, ui::Point{static_cast<int>(std::lround(timelineX(ticks))), static_cast<int>(std::lround(y))}};
		}

		// Waits until the editor shows the notes at their times on the timeline.
		void settle(size_t count)
		{
			REQUIRE(app.waitUntil(
				[&]
				{
					const Json shown = elements(editor() + " .key-note");
					if (shown.size() != count)
						return false;
					const Json notes = app.call("project.get").at("midiParts").at(0).at("notes");
					return std::all_of(notes.begin(), notes.end(),
						[&](const Json& each)
						{
							const Json found = elements(note(each.at("id").get<uint64_t>()));
							if (found.empty())
								return false;
							const int64_t start = g_bar + each.at("position").get<int64_t>();
							const double x = found.at(0).at("box").at("x").get<double>();
							const double width = found.at(0).at("box").at("width").get<double>();
							return std::abs(x - timelineX(start)) <= 1.5 &&
								std::abs(x + width - timelineX(start + each.at("length").get<int64_t>())) <= 1.5;
						});
				}));
		}

		void scroll(int pitch)
		{
			REQUIRE(
				app.waitUntil([&] { return app.appUi().inPlaceEditors()->keyEditor(track)->scrollToPitch(pitch); }));
			app.shell().step(std::chrono::milliseconds(0));
		}

		test::UiApp app{ui::Size{1200, 800}};
		ui::EventDisplay& display;
		uint64_t track = 0;
		uint64_t audio = 0;
		uint64_t part = 0;
		std::vector<uint64_t> notes;
	};
}

TEST_CASE("the in-place editor opens over its track's lane with the notes on the timeline", "[ui]")
{
	Fixture f;
	CHECK(f.elements(f.editor()).empty());
	// MIDI > Open In-Place Editor, for the selected track; an audio track selected too has none.
	f.app.appUi().trackList()->select({f.track, f.audio});
	f.click("#menu-bar button[menu=midi-menu]");
	f.click("menuitem[command=\"editor.openInPlace\"]");
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	CHECK(f.app.call("editor.inPlace").at("tracks") == Json::array({f.track}));
	CHECK(f.view().at("parts") == Json::array({f.part}));

	// The track's lane grows to hold it, and the editor lies over the lane.
	REQUIRE(f.app.waitUntil([&] { return f.display.trackHeight(f.track) == ui::g_inPlaceHeight; }));
	CHECK(f.display.trackHeight(f.audio) == ui::g_trackHeight);
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json shown = f.box(f.editor());
			const Json area = f.box(".event-area");
			const std::vector<double>& tops = f.display.laneTops();
			return std::abs(shown.at("y").get<double>() - area.at("y").get<double>() - tops[0]) <= 1 &&
				std::abs(shown.at("height").get<double>() - (tops[1] - tops[0])) <= 1 &&
				std::abs(shown.at("width").get<double>() - area.at("width").get<double>()) <= 1;
		}));
	// Its notes are where the ruler has their times, and its part's box spans the part's bar.
	f.settle(2);
	const Json partBox = f.box(f.editor() + " .editor-part[part=\"" + std::to_string(f.part) + "\"]");
	CHECK(std::abs(partBox.at("x").get<double>() - f.timelineX(g_bar)) <= 1.5);
	CHECK(std::abs(partBox.at("x").get<double>() + partBox.at("width").get<double>() - f.timelineX(2 * g_bar)) <= 1.5);
	// Zooming and scrolling the project window keep them there.
	f.display.zoomIn();
	f.settle(2);
	f.display.scrollTo(1.0);
	f.settle(2);

	// The toolbar and the info line show with the toggle.
	CHECK_FALSE(f.elements(f.editor() + " .key-editor-toolbar").at(0).at("visible").get<bool>());
	f.click(f.editor() + " .in-place-toolbar-toggle");
	CHECK(f.has(f.editor(), "toolbar-shown"));
	CHECK(f.elements(f.editor() + " .key-editor-toolbar").at(0).at("visible").get<bool>());
	f.settle(2); // the grid's height changed, not its time axis

	// Edit In-Place closes it again: the lane is as high as before.
	f.app.keys().run("editor.editInPlace", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor()).empty(); }));
	CHECK(f.display.trackHeight(f.track) == ui::g_trackHeight);
	CHECK_FALSE(f.view().at("open").get<bool>());
	// An engine client opens it too, and a new project closes it.
	f.app.call("editor.inPlace", {{"tracks", {f.track}}, {"open", true}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	f.app.call("project.new");
	REQUIRE(f.app.waitUntil([&] { return f.elements(".in-place-editor").empty(); }));
}

TEST_CASE("the in-place editor edits notes as the key editor does, and the event display leaves them", "[ui]")
{
	Fixture f;
	f.app.keys().run("editor.openInPlace", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	f.settle(2);
	f.scroll(62);

	// A click selects a note in the in-place editor's view; the project window's selection stays empty.
	f.click(f.note(f.notes[1]));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selection").size() == 1; }));
	CHECK(f.view().at("selection").at(0).at("note") == f.notes[1]);
	CHECK(f.has(f.note(f.notes[1]), "selected"));
	CHECK(f.display.selection().empty());

	// A double click on the empty grid inserts a note there, not a part on the lane.
	f.click(f.at(g_bar + (2 * g_quarter) + 10, 62), 2);
	f.settle(3);
	const Json project = f.app.call("project.get");
	CHECK(project.at("midiParts").size() == 1);
	const Json added = project.at("midiParts").at(0).at("notes");
	CHECK(std::any_of(added.begin(), added.end(),
		[](const Json& each) { return each.at("pitch") == 62 && each.at("position") == 2 * g_quarter; }));

	// With the toolbar shown, the draw tool draws a note at the snapped time.
	f.click(f.editor() + " .in-place-toolbar-toggle");
	f.click(f.editor() + " .key-tool[key-tool=draw]");
	REQUIRE(f.has(f.editor() + " .key-tool[key-tool=draw]", "checked"));
	f.scroll(62);
	f.click(f.at(g_bar + (3 * g_quarter) + 10, 62));
	f.settle(4);
	// A part added to the track shows in the row of parts; a click on it activates it.
	const uint64_t later =
		f.app.call("midiPart.add", {{"track", f.track}, {"position", 3 * g_bar}, {"length", g_bar}, {"name", "More"}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	const std::string laterBox = f.editor() + " .editor-part[part=\"" + std::to_string(later) + "\"]";
	f.click(laterBox);
	REQUIRE(f.app.waitUntil([&] { return f.view().at("activePart") == later; }));
	CHECK(f.app.waitUntil([&] { return f.has(laterBox, "active"); }));
	CHECK(f.display.selection().empty());
}

TEST_CASE("the inspector's Edit In-Place opens and closes the selected track's in-place editor", "[ui]")
{
	Fixture f;
	f.app.appUi().docking().open("inspector");
	f.app.shell().step(std::chrono::milliseconds(0));
	const std::string button = "button[control=editInPlace]";
	REQUIRE(f.app.waitUntil([&] { return !f.has(button, "unavailable") && !f.elements(button).empty(); }));
	f.click(button);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json found = f.elements(button);
			return found.at(0).value("attributes", Json::object()).contains("checked");
		}));
	f.click(button);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor()).empty(); }));
	// An audio track has none.
	f.app.appUi().trackList()->select({f.audio});
	REQUIRE(f.app.waitUntil([&] { return f.has(button, "unavailable"); }));
}

TEST_CASE("the in-place editor of a removed track shows again when an undo brings the track back", "[ui]")
{
	Fixture f;
	f.app.keys().run("editor.openInPlace", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	f.app.call("track.remove", {{"track", f.track}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor()).empty(); }));
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	REQUIRE(f.app.waitUntil([&] { return f.display.trackHeight(f.track) == ui::g_inPlaceHeight; }));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json shown = f.box(f.editor());
			const std::vector<double>& tops = f.display.laneTops();
			return std::abs(shown.at("y").get<double>() - f.box(".event-area").at("y").get<double>() - tops[0]) <= 1 &&
				std::abs(shown.at("height").get<double>() - (tops[1] - tops[0])) <= 1;
		}));
	f.settle(2);
	// Edit In-Place closes the editor shown.
	f.app.keys().run("editor.editInPlace", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor()).empty(); }));
	CHECK(f.app.call("editor.inPlace").at("tracks").empty());
}

TEST_CASE("the inspector's Edit In-Place starts unchecked in another project", "[ui]")
{
	Fixture f;
	f.app.appUi().docking().open("inspector");
	f.app.shell().step(std::chrono::milliseconds(0));
	const std::string button = "button[control=editInPlace]";
	const auto checked = [&]
	{
		const Json found = f.elements(button);
		return !found.empty() && found.at(0).value("attributes", Json::object()).contains("checked");
	};
	REQUIRE(f.app.waitUntil([&] { return !f.has(button, "unavailable") && !f.elements(button).empty(); }));
	f.click(button);
	REQUIRE(f.app.waitUntil(checked));

	// The new project's ids start again: its first MIDI track may have the id of the track whose editor was open.
	f.app.call("project.new");
	const uint64_t track = f.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json& project = f.display.project();
			if (!project.is_object())
				return false;
			const Json& tracks = project.at("tracks");
			return std::any_of(tracks.begin(), tracks.end(), [&](const Json& each) { return each.at("id") == track; });
		}));
	f.app.appUi().trackList()->select({track});
	REQUIRE(f.app.waitUntil([&] { return !f.has(button, "unavailable") && !f.elements(button).empty(); }));
	for (int i = 0; i < 5; ++i)
		f.app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(checked());
	f.click(button);
	REQUIRE(f.app.waitUntil([&] { return f.app.call("editor.inPlace").at("tracks") == Json::array({track}); }));
	CHECK(f.app.waitUntil(checked));
}
