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
#include <numeric>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;
	// The quantize grid, a sixteenth: a click in the middle of one stays in it whichever pixel the click rounds to.
	constexpr int64_t g_sixteenth = g_quarter / 4;

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
	f.click(f.at(g_bar + (2 * g_quarter) + (g_sixteenth / 2), 62), 2);
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
	f.click(f.at(g_bar + (3 * g_quarter) + (g_sixteenth / 2), 62));
	f.settle(4);
	// While a note is being drawn, zooming the project window moves the notes and the drawn one with the timeline.
	f.app.ui().press(std::nullopt, f.at(g_bar + (g_sixteenth / 2), 63), ui::MouseButton::Left, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	const std::string drawn = f.editor() + " .key-note.drawing";
	REQUIRE(f.elements(drawn).size() == 1);
	f.display.zoomIn();
	f.settle(5); // the four notes and the one being drawn
	CHECK(std::abs(f.box(drawn).at("x").get<double>() - f.timelineX(g_bar)) <= 1.5);
	f.app.ui().release(std::nullopt, f.at(g_bar + (g_sixteenth / 2), 63), ui::MouseButton::Left, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements(drawn).empty());
	f.display.zoomOut(); // the part added next in view again
	f.settle(5);
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
			// The new project's one track, not the old project's track with the same id: the list has had the
			// project.reset that clears its selection.
			const Json& tracks = project.at("tracks");
			return tracks.size() == 1 && tracks.at(0).at("id") == track;
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

TEST_CASE("two in-place editors and the lower zone's key editor have menus and selections of their own", "[ui]")
{
	Fixture f;
	const uint64_t strings = f.app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
	const uint64_t stringsPart =
		f.app.call("midiPart.add", {{"track", strings}, {"position", 0}, {"length", g_bar}, {"name", "Strings"}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	const uint64_t held =
		f.app
			.call("note.add",
				{{"part", stringsPart},
					{"notes", {{{"position", g_quarter}, {"length", g_bar / 2}, {"pitch", 62}, {"velocity", 0.5}}}}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("midiParts").size() == 2; }));
	f.app.appUi().trackList()->select({f.track, strings});
	f.app.keys().run("editor.openInPlace", Json::object(), f.app.main(), nullptr);
	const std::string other = ".in-place-editor[track=\"" + std::to_string(strings) + "\"]";
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(other + " .key-note").size() == 1; }));
	CHECK(f.app.call("editor.inPlace").at("tracks") == Json::array({f.track, strings}));
	f.settle(2);
	// The instrument track's lane grows too, and its note lies at its time.
	REQUIRE(f.app.waitUntil([&] { return f.display.trackHeight(strings) == ui::g_inPlaceHeight; }));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json found = f.elements(other + " .key-note");
			return !found.empty() &&
				std::abs(found.at(0).at("box").at("x").get<double>() - f.timelineX(g_quarter)) <= 1.5;
		}));

	// The lower zone's key editor beside them.
	f.app.call("editor.open", {{"editor", "lower"}, {"parts", {f.part}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".midi-editor .key-note").size() == 2; }));
	// Each editor's menus have ids of their own (ui::menuIdSuffix).
	for (const std::string& suffix :
		{std::string(), "-inPlace-" + std::to_string(f.track), "-inPlace-" + std::to_string(strings)})
	{
		INFO(suffix);
		CHECK(f.elements("#key-length-menu" + suffix).size() == 1);
		CHECK(f.elements("#key-editor-toolbox" + suffix).size() == 1);
		if (!suffix.empty())
			CHECK(f.elements("#in-place-editing-mode-menu" + suffix).size() == 1);
	}

	// A note selected in one editor is selected in its view only. The instrument track's lane is scrolled to the top
	// first: where the window opens smaller, as on CI's macOS runner (80 % of a smaller monitor), the lower zone
	// leaves the event display too little height to show it below the other two tracks.
	const std::vector<uint64_t>& lanes = f.display.laneTracks();
	const auto lane = std::find(lanes.begin(), lanes.end(), strings);
	REQUIRE(lane != lanes.end());
	f.display.scrollVertically(f.display.laneTops().at(static_cast<size_t>(lane - lanes.begin())));
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().inPlaceEditors()->keyEditor(strings)->scrollToPitch(62); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.click(other + " .key-note[note=\"" + std::to_string(held) + "\"]");
	const auto viewOf = [&](const std::string& editor) { return f.app.call("editor.get", {{"editor", editor}}); };
	const std::string stringsView = "inPlace:" + std::to_string(strings);
	REQUIRE(f.app.waitUntil([&] { return viewOf(stringsView).at("selection").size() == 1; }));
	CHECK(f.view().at("selection").empty());
	CHECK(viewOf("lower").at("selection").empty());
	CHECK(f.display.selection().empty());
	f.display.scrollVertically(0.0);
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return std::abs(f.box(f.editor()).at("y").get<double>() - f.box(".event-area").at("y").get<double>()) < 1.0;
		}));

	// The toolbar's multiple part controls (reference manual p. 1260): Part Editing Mode and Activate Part for
	// Editing, for the editor whose toolbar they are in.
	const uint64_t later =
		f.app.call("midiPart.add", {{"track", f.track}, {"position", 3 * g_bar}, {"length", g_bar}, {"name", "More"}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	REQUIRE(f.app.waitUntil([&] { return f.view().at("parts").size() == 2; }));
	f.click(f.editor() + " .in-place-toolbar-toggle");
	const std::string suffix = "-inPlace-" + std::to_string(f.track);
	f.click(f.editor() + " .in-place-editing-mode");
	CHECK(f.elements("#in-place-editing-mode-menu" + suffix + " menuitem").size() == 2); // All Parts, Active Part
	f.click("#in-place-editing-mode-menu" + suffix + " menuitem[editing-mode=activePart]");
	REQUIRE(f.app.waitUntil([&] { return f.view().at("editingMode") == "activePart"; }));
	CHECK(viewOf(stringsView).at("editingMode") == "allParts");
	f.click(f.editor() + " .in-place-active-part");
	f.click("#in-place-active-part-menu" + suffix + " menuitem[active-part=\"" + std::to_string(later) + "\"]");
	REQUIRE(f.app.waitUntil([&] { return f.view().at("activePart") == later; }));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json found = f.elements(f.editor() + " .in-place-active-part");
			return !found.empty() && found.at(0).value("text", std::string()) == "More";
		}));
	CHECK(viewOf(stringsView).at("activePart") == stringsPart);

	// Closing one leaves the other's menus.
	f.app.call("editor.inPlace", {{"tracks", {f.track}}, {"open", false}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor()).empty(); }));
	CHECK(f.elements("#key-length-menu" + suffix).empty());
	CHECK(f.elements("#in-place-editing-mode-menu" + suffix).empty());
	CHECK(f.elements("#in-place-active-part-menu" + suffix).empty());
	CHECK(f.elements("#key-length-menu-inPlace-" + std::to_string(strings)).size() == 1);
	CHECK(f.elements("#key-length-menu").size() == 1);
}

TEST_CASE("a key held in an in-place editor that closes ends its pitch", "[ui]")
{
	// A key of the keyboard plays its pitch until the mouse is released (p. 1204); the in-place editor closing ends it
	// with the editor.
	Fixture f;
	const uint64_t strings = f.app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
	f.app.call("midiPart.add", {{"track", strings}, {"position", g_bar}, {"length", g_bar}, {"name", "Strings"}});
	REQUIRE(f.app.waitUntil([&] { return f.display.project().at("midiParts").size() == 2; }));
	// The instrument noteProbe, so that the notes the keyboard sends are heard.
	test::NoteProbe sound(f.app, strings);

	f.app.appUi().trackList()->select({strings});
	f.app.call("editor.inPlace", {{"tracks", {strings}}, {"open", true}});
	const std::string lane = ".in-place-editor[track=\"" + std::to_string(strings) + "\"]";
	REQUIRE(f.app.waitUntil([&] { return f.elements(lane + " .key-editor-keys").size() == 1; }));
	// The lane in view, and C3 in its rows.
	const std::vector<uint64_t>& lanes = f.display.laneTracks();
	const auto found = std::find(lanes.begin(), lanes.end(), strings);
	REQUIRE(found != lanes.end());
	f.display.scrollVertically(f.display.laneTops().at(static_cast<size_t>(found - lanes.begin())));
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().inPlaceEditors()->keyEditor(strings)->scrollToPitch(60); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json keyboard = f.box(lane + " .key-editor-keys");
	const Json grid = f.box(lane + " .key-editor-grid");
	const double y = grid.at("y").get<double>() + ((127.5 - 60) / 128.0 * grid.at("height").get<double>());
	// The key is in view: inside the lane's box and the window, wherever the lower zone and the window's size leave it.
	const Json shown = f.box(lane);
	const Json area = f.box(".event-area");
	REQUIRE(y >= shown.at("y").get<double>());
	REQUIRE(y <= shown.at("y").get<double>() + shown.at("height").get<double>());
	REQUIRE(y >= area.at("y").get<double>());
	REQUIRE(y <= area.at("y").get<double>() + area.at("height").get<double>());
	f.app.ui().press(std::nullopt,
		{"", 0, ui::Point{keyboard.at("x").get<int>() + 2, static_cast<int>(std::lround(y))}}, ui::MouseButton::Left,
		{});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(sound.plays({{0x90, 60, 100}}));

	f.app.call("editor.inPlace", {{"tracks", {strings}}, {"open", false}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(lane).empty(); }));
	CHECK(sound.plays({{0x90, 60, 100}, {0x80, 60, 0}}));
}

TEST_CASE("only a track's own lane grows for its in-place editor, which a folded folder hides", "[ui]")
{
	Fixture f;
	f.app.keys().run("editor.openInPlace", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	// With its lanes shown (#99), they keep the track's own height.
	f.display.showLanes(f.track, true);
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const std::vector<uint64_t>& lanes = f.display.laneTracks();
			return std::count(lanes.begin(), lanes.end(), f.track) > 1;
		}));
	const std::vector<double>& tops = f.display.laneTops();
	const double zoom = f.display.view().trackZoom;
	for (size_t i = 0; i < f.display.laneTracks().size(); ++i)
		if (f.display.laneTracks()[i] == f.track)
		{
			INFO(i);
			const double height = f.display.laneTakes()[i] < 0 ? ui::g_inPlaceHeight : ui::g_trackHeight;
			CHECK(std::abs(tops[i + 1] - tops[i] - (height * zoom)) < 0.01);
		}
	f.display.showLanes(f.track, false);

	// In a folder: folded, the editor hides with the track's lane; unfolded, it is over the lane again.
	const uint64_t folder = f.app.call("track.add", {{"type", "folder"}}).at("track").at("id").get<uint64_t>();
	f.app.call("track.move", {{"track", f.track}, {"parent", folder}, {"index", 0}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const std::vector<uint64_t>& lanes = f.display.laneTracks();
			return std::find(lanes.begin(), lanes.end(), folder) != lanes.end() && lanes.size() == 3;
		}));
	f.app.appUi().trackList()->setFolded(folder, true);
	REQUIRE(f.app.waitUntil([&] { return f.has(f.editor(), "hidden"); }));
	CHECK(!f.elements(f.editor()).at(0).at("visible").get<bool>());
	f.app.appUi().trackList()->setFolded(folder, false);
	REQUIRE(f.app.waitUntil([&] { return !f.has(f.editor(), "hidden"); }));
	f.settle(2);
}

TEST_CASE("a time-linear track's in-place editor has its notes at their times on the timeline", "[ui]")
{
	Fixture f;
	f.app.call("track.set", {{"track", f.track}, {"timeBase", "linear"}});
	f.app.keys().run("editor.openInPlace", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	f.settle(2);
	f.display.zoomIn();
	f.settle(2);
}

TEST_CASE("quantizing in an in-place editor quantizes its notes, not the project window's selected parts", "[ui]")
{
	Fixture f;
	// Two notes off the sixteenths' grid.
	const Json added = f.app.call("note.add",
		{{"part", f.part},
			{"notes",
				{{{"position", (2 * g_quarter) + 7}, {"length", g_sixteenth}, {"pitch", 67}, {"velocity", 0.5}},
					{{"position", (3 * g_quarter) + 7}, {"length", g_sixteenth}, {"pitch", 69}, {"velocity", 0.5}}}}});
	const uint64_t first = added.at("notes").at(0).at("id").get<uint64_t>();
	f.app.call("quantize.set", {{"preset", "1/16"}});
	f.app.keys().run("editor.openInPlace", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 4; }));
	f.display.select({f.part});
	f.scroll(67);
	f.click(f.note(first));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("selection").size() == 1; }));
	// Q takes the selection from the in-place editors' copy of the view, which follows the engine's by an event.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json* shown = f.app.appUi().inPlaceEditors()->view(f.track);
			return shown != nullptr && shown->value("selection", Json::array()).size() == 1;
		}));
	CHECK(f.display.selection() == std::vector<uint64_t>{f.part});

	// Q, with the focus in the in-place editor: its selected note, not the selected part's notes.
	f.app.ui().key(std::nullopt, "Q");
	const auto position = [&](int pitch)
	{
		const Json project = f.app.call("project.get");
		for (const Json& note : project.at("midiParts").at(0).at("notes"))
			if (note.at("pitch") == pitch)
				return note.at("position").get<int64_t>();
		return int64_t{-1};
	};
	REQUIRE(f.app.waitUntil([&] { return position(67) == 2 * g_quarter; }));
	CHECK(position(69) == (3 * g_quarter) + 7);
}

TEST_CASE("Open In-Place Editor and Edit In-Place wait for a MIDI or instrument track to be selected", "[ui]")
{
	Fixture f;
	for (const char* command : {"editor.openInPlace", "editor.editInPlace"})
	{
		INFO(command);
		f.app.appUi().trackList()->select({});
		CHECK(!f.app.keys().canRun(command, f.app.main(), nullptr));
		f.app.appUi().trackList()->select({f.audio});
		CHECK(!f.app.keys().canRun(command, f.app.main(), nullptr));
		f.app.appUi().trackList()->select({f.audio, f.track});
		CHECK(f.app.keys().canRun(command, f.app.main(), nullptr));
	}
}

TEST_CASE("the fade lines of the events below a track with an in-place editor and its lanes shown are on them", "[ui]")
{
	Fixture f;
	// A faded audio event on the audio track below.
	const Json media =
		f.app.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 480000}})
			.at("media");
	const Json clip = f.app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id");
	const uint64_t event =
		f.app.call("audioEvent.add", {{"track", f.audio}, {"clip", clip}, {"position", 0}, {"length", 48000}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	f.app.call("audioEvent.setFades", {{"event", event}, {"fadeIn", {{"length", 2400}, {"shape", "linear"}}}});
	f.app.keys().run("editor.openInPlace", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.elements(f.editor() + " .key-note").size() == 2; }));
	f.display.showLanes(f.track, true);
	const auto* fades = dynamic_cast<const ui::FadeLines*>(f.display.eventArea().QuerySelector("fade-lines"));
	REQUIRE(fades != nullptr);
	// The fades canvas's lanes are as high as the event display's, so the fade is drawn on its event.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const std::vector<uint64_t>& lanes = f.display.laneTracks();
			const std::vector<double>& tops = f.display.laneTops();
			const std::vector<ui::FadeLines::Lane>& shown = fades->lanes();
			if (std::count(lanes.begin(), lanes.end(), f.track) < 2 || shown.size() != lanes.size() ||
				tops.size() != lanes.size() + 1)
				return false;
			std::vector<double> heights(tops.size());
			std::adjacent_difference(tops.begin(), tops.end(), heights.begin()); // from the second on
			if (!std::equal(shown.begin(), shown.end(), heights.begin() + 1,
					[](const ui::FadeLines::Lane& lane, double height)
					{ return std::abs(lane.height - height) <= 0.01; }))
				return false;
			const auto audio = std::find(lanes.begin(), lanes.end(), f.audio) - lanes.begin();
			return shown[static_cast<size_t>(audio)].fades.size() == 1;
		}));
}

TEST_CASE("a newly opened in-place editor scrolls to its notes", "[ui]")
{
	Fixture f;
	f.app.keys().run("editor.openInPlace", Json::object(), f.app.main(), nullptr);
	// Without scrollToPitch: its notes are inside the note display's visible box.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json bodies = f.elements(f.editor() + " .key-editor-body");
			if (bodies.empty())
				return false;
			const Json body = bodies.at(0).at("box");
			const double top = body.at("y").get<double>();
			const double bottom = top + body.at("height").get<double>();
			return std::all_of(f.notes.begin(), f.notes.end(),
				[&](uint64_t id)
				{
					const Json found = f.elements(f.note(id));
					if (found.empty())
						return false;
					const double y = found.at(0).at("box").at("y").get<double>();
					return y >= top && y + found.at(0).at("box").at("height").get<double>() <= bottom;
				});
		}));
}
