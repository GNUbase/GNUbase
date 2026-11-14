// SPDX-License-Identifier: GPL-3.0-or-later
// The key editor in the app (#127, docs/features/key-editor.md), driven with ui::Automation as MCP's UI tools drive
// it: the note display and its keyboard, every tool, the transpose palette, the info line, step and MIDI input and the
// inspector's Chord Editing section. tests/keyeditor_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/keyeditor.h"
#include "ui/midieditor.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

	std::string noteSelector(uint64_t id)
	{
		return ".key-note[note=\"" + std::to_string(id) + "\"]";
	}

	// A MIDI track (or a track of the type) with a one-bar part at the project start, open in the editor window.
	struct Fixture
	{
		explicit Fixture(ui::Size size = {1200, 800}, const std::string& type = "midi") : app(size)
		{
			track = app.call("track.add", {{"type", type}}).at("track").at("id").get<uint64_t>();
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

		// Adds notes {position, pitch, length} to the part and waits until the display shows them; returns their ids.
		std::vector<uint64_t> add(const std::vector<std::tuple<int64_t, int, int64_t>>& notes)
		{
			Json items = Json::array();
			for (const auto& [position, pitch, length] : notes)
				items.push_back({{"position", position}, {"length", length}, {"pitch", pitch}, {"velocity", 0.5}});
			const Json added = app.call("note.add", {{"part", part}, {"notes", items}});
			std::vector<uint64_t> ids;
			for (const Json& note : added.at("notes"))
				ids.push_back(note.at("id").get<uint64_t>());
			const size_t count = partNotes().size();
			REQUIRE(app.waitUntil([&] { return elements(".key-note").size() == count; }));
			scroll(notes.empty() ? 60 : std::get<1>(notes.front()));
			return ids;
		}

		void scroll(int pitch)
		{
			REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow()->keyEditor()->scrollToPitch(pitch); }));
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json partNotes()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == part)
				{
					std::vector<Json> sorted(each.at("notes").begin(), each.at("notes").end());
					std::sort(sorted.begin(), sorted.end(),
						[](const Json& a, const Json& b)
						{
							return std::make_pair(a.at("position").get<int64_t>(), a.at("pitch").get<int>()) <
								std::make_pair(b.at("position").get<int64_t>(), b.at("pitch").get<int>());
						});
					return sorted;
				}
			return Json::array();
		}

		Json note(uint64_t id)
		{
			for (const Json& each : partNotes())
				if (each.at("id") == id)
					return each;
			return nullptr;
		}

		Json view() { return app.call("editor.get", {{"editor", "window"}}); }

		// Waits until the display shows the part's notes as the engine has them, each at its time and pitch.
		void settle()
		{
			REQUIRE(app.waitUntil(
				[&]
				{
					const Json grid = box(".key-editor-grid");
					const auto width = grid.at("width").get<double>();
					const Json notes = partNotes();
					if (elements(".key-note").size() != notes.size())
						return false;
					return std::all_of(notes.begin(), notes.end(),
						[&](const Json& note)
						{
							const Json found = elements(noteSelector(note.at("id").get<uint64_t>()));
							if (found.empty())
								return false;
							const Json shown = found.at(0).at("box");
							const double x = grid.at("x").get<double>() +
								(static_cast<double>(note.at("position").get<int64_t>()) / g_bar * width);
							const double length = static_cast<double>(note.at("length").get<int64_t>()) / g_bar * width;
							const double y = grid.at("y").get<double>() +
								((127.0 - note.at("pitch").get<double>()) / 128.0 * grid.at("height").get<double>());
							return std::abs(shown.at("x").get<double>() - x) <= 1.5 &&
								std::abs(shown.at("width").get<double>() - length) <= 1.5 &&
								std::abs(shown.at("y").get<double>() - y) <= 1.5;
						});
				}));
		}

		std::vector<uint64_t> selection()
		{
			std::vector<uint64_t> notes;
			const Json shown = view();
			for (const Json& selected : shown.at("selection"))
				notes.push_back(selected.at("note").get<uint64_t>());
			return notes;
		}

		// A point of the note display: at sixteenths from the project start, on the pitch's row.
		ui::Target at(double sixteenths, int pitch)
		{
			const Json grid = box(".key-editor-grid");
			const double x = grid.at("x").get<double>() + (sixteenths / 16.0 * grid.at("width").get<double>());
			const double y = grid.at("y").get<double>() + ((127.5 - pitch) / 128.0 * grid.at("height").get<double>());
			return {"", 0, ui::Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}};
		}

		void click(const ui::Target& target, int count = 1, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(window, target, ui::MouseButton::Left, count, modifiers);
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
			REQUIRE(has(".key-tool[key-tool=" + name + "]", "checked"));
		}

		// Gives the track the instrument noteProbe, so that plays() hears what the keyboard's keys play (reference
		// manual p. 1204).
		void probe() { sound.emplace(app, track); }

		// Renders until the note ons and offs the instrument played since the last clearSound() are the expected ones.
		bool plays(const std::vector<std::vector<int>>& expected)
		{
			if (!sound)
				FAIL("probe() first");
			return sound->plays(expected);
		}

		void clearSound()
		{
			if (!sound)
				FAIL("probe() first");
			sound->clear();
		}

		// A point on the keyboard: x across the window, in the middle of the pitch's row.
		ui::Target key(int x, int pitch)
		{
			const Json grid = box(".key-editor-grid");
			const double y = grid.at("y").get<double>() + ((127.5 - pitch) / 128.0 * grid.at("height").get<double>());
			return {"", 0, ui::Point{x, static_cast<int>(std::lround(y))}};
		}

		test::UiApp app;
		std::optional<ui::WindowId> window;
		uint64_t track = 0;
		uint64_t part = 0;
		std::optional<test::NoteProbe> sound;
	};
}

TEST_CASE("the key editor shows its part's notes on a grid of pitches beside a keyboard", "[ui]")
{
	Fixture f;
	CHECK(f.elements(".key-editor-key").size() == 128);
	CHECK(f.elements(".key-editor-key.black").size() == 53);
	CHECK(f.elements(".key-editor-key[pitch=\"60\"]").at(0).at("text") == "C3");
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 64, 2 * g_quarter}});
	// Each note at its time and pitch: the grid's width is the part's bar.
	const Json grid = f.box(".key-editor-grid");
	const Json second = f.box(noteSelector(ids[1]));
	const double width = grid.at("width").get<double>();
	CHECK(std::abs(second.at("x").get<double>() - grid.at("x").get<double>() - (width / 4)) <= 1);
	CHECK(std::abs(second.at("width").get<double>() - (width / 2)) <= 1);
	const double row = grid.at("height").get<double>() / 128;
	CHECK(std::abs(second.at("y").get<double>() - grid.at("y").get<double>() - ((127 - 64) * row)) <= 1);
	CHECK(std::abs(second.at("height").get<double>() - row) <= 1);
	// The parts by track above the notes; the keys of selected notes are lit.
	CHECK(f.elements(".editor-part-row").size() == 1);
	f.click(of(noteSelector(ids[1])));
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-editor-key[pitch=\"64\"]", "selected"); }));
	CHECK(f.has(noteSelector(ids[1]), "selected"));
	CHECK_FALSE(f.has(".key-editor-key[pitch=\"60\"]", "selected"));
}

TEST_CASE("the key editor's keyboard is a piano keyboard aligned with the grid's rows", "[ui]")
{
	Fixture f({1024, 600});
	const auto blackKey = [](int pitch)
	{
		const int key = pitch % 12;
		return key == 1 || key == 3 || key == 6 || key == 8 || key == 10;
	};
	// At a fractional UI scale too: the keys are placed in percent of the keyboard, the grid's rows are stacked
	// blocks, and the two must not drift apart by rounding. The golden image below is at a dp ratio of 1.
	double row = 0.0;
	int width = 0;
	for (const float ratio : {1.5F, 1.0F})
	{
		INFO("dp ratio " << ratio);
		f.app.setDpRatio(ratio);
		f.scroll(66); // the octave of C3 in view, for the mouse below
		const Json keyboard = f.box(".key-editor-keys");
		const Json grid = f.box(".key-editor-grid");
		const auto top = grid.at("y").get<double>();
		row = grid.at("height").get<double>() / 128;
		const auto x = keyboard.at("x").get<int>();
		width = keyboard.at("width").get<int>();
		CHECK(std::abs(keyboard.at("y").get<double>() - top) <= 1);
		CHECK(std::abs(keyboard.at("height").get<double>() - grid.at("height").get<double>()) <= 1);
		for (int pitch = 0; pitch < 128; ++pitch)
		{
			INFO("pitch " << pitch);
			const Json key = f.box(".key-editor-key[pitch=\"" + std::to_string(pitch) + "\"]");
			CHECK(key.at("x").get<int>() == x);
			if (blackKey(pitch))
			{
				// Narrower than the white keys, drawn at the left, exactly as high as the pitch's row.
				CHECK(key.at("width").get<int>() < width * 3 / 4);
				CHECK(key.at("width").get<int>() > width / 2);
				CHECK(std::abs(key.at("y").get<double>() - top - ((127 - pitch) * row)) <= 1);
				CHECK(std::abs(key.at("height").get<double>() - row) <= 1);
			}
			else if (pitch < 120)
			{
				// The white keys of a whole octave share its 12 rows equally and span the keyboard's width.
				CHECK(key.at("width").get<int>() == width);
				CHECK(std::abs(key.at("height").get<double>() - (row * 12 / 7)) <= 1);
				const int octave = pitch / 12;
				int white = 0;
				for (int each = octave * 12; each < pitch; ++each)
					white += blackKey(each) ? 0 : 1;
				const double bottom = top + ((128 - (octave * 12)) * row) - (white * row * 12 / 7);
				CHECK(std::abs(key.at("y").get<double>() + key.at("height").get<double>() - bottom) <= 1);
			}
		}
		// The top octave ends with the keyboard at G8 (127): its white keys are as high as the others, the last one
		// cut.
		CHECK(std::abs(f.box(".key-editor-key[pitch=\"124\"]").at("height").get<double>() - (row * 12 / 7)) <= 1);
		CHECK(std::abs(f.box(".key-editor-key[pitch=\"127\"]").at("y").get<double>() - top) <= 1);
		CHECK(f.elements(".key-editor-key[pitch=\"48\"]").at(0).at("text") == "C2");

		// The mouse over a black key's row: at the left on the black key, at the right on the white key next to it,
		// whichever's share of the octave that is. C3's octave starts at the bottom of row 60.
		const double octaveBottom = top + ((128 - 60) * row);
		const auto pitchAt = [&](int at, double rows)
		{
			const Json under = f.app.ui().move(
				f.window, {"", 0, ui::Point{at, static_cast<int>(std::lround(octaveBottom - (rows * row)))}});
			const Json& element = under.at("element");
			return element.is_null() ? -1 : std::stoi(element.at("attributes").value("pitch", std::string("-1")));
		};
		// Just right of the keyboard's left part, where a drag zooms (p. 1261): over the keys themselves.
		const int left = x + static_cast<int>(std::ceil(12.0 * ratio)) + 2;
		const int right = x + width - 2;
		CHECK(pitchAt(left, 1.3) == 61);   // C#3
		CHECK(pitchAt(right, 1.3) == 60);  // C3, up to 12/7 rows
		CHECK(pitchAt(right, 1.9) == 62);  // D3
		CHECK(pitchAt(left, 3.5) == 63);   // D#3
		CHECK(pitchAt(right, 3.2) == 62);  // D3, up to 24/7 rows
		CHECK(pitchAt(right, 3.7) == 64);  // E3
		CHECK(pitchAt(left, 10.5) == 70);  // A#3
		CHECK(pitchAt(right, 10.1) == 69); // A3, up to 72/7 rows
		CHECK(pitchAt(right, 10.5) == 71); // B3
	}

	// The keyboard as it looks in the default theme, a black and a white key lit for selected notes: the two octaves
	// from C2 to B3.
	const std::vector<uint64_t> ids = f.add({{0, 61, g_quarter}, {g_quarter, 64, g_quarter}});
	// Selected through the engine: a click would leave the focus, and its ring, in the note display.
	f.app.call("editor.set",
		{{"editor", "window"},
			{"selection", Json::array({{{"part", f.part}, {"note", ids[0]}}, {{"part", f.part}, {"note", ids[1]}}})}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return f.has(".key-editor-key[pitch=\"61\"]", "selected") &&
				f.has(".key-editor-key[pitch=\"64\"]", "selected");
		}));
	f.scroll(60);
	const Json body = f.box(".key-editor-body");
	// The inspector's sections change with the selection, which could move the note display.
	const auto shownTop = f.box(".key-editor-grid").at("y").get<double>();
	const auto shownX = f.box(".key-editor-keys").at("x").get<int>();
	const auto from = static_cast<int>(std::lround(shownTop + ((127 - 71) * row)));
	const auto to = static_cast<int>(std::lround(shownTop + ((128 - 48) * row)));
	REQUIRE(from >= body.at("y").get<int>());
	REQUIRE(to <= body.at("y").get<int>() + body.at("height").get<int>());
	const ui::Image shown = f.app.ui().screenshot(f.window, {});
	REQUIRE(shownX >= 0);
	REQUIRE(shownX + width <= shown.width);
	ui::Image keys{width, to - from, {}};
	for (int y = from; y < to; ++y)
	{
		const auto begin = shown.pixels.begin() + (((static_cast<ptrdiff_t>(y) * shown.width) + shownX) * 4);
		keys.pixels.insert(keys.pixels.end(), begin, begin + (static_cast<ptrdiff_t>(width) * 4));
	}
	test::UiApp::checkGolden("key-editor-keyboard", keys);
}

TEST_CASE("a click on a key of the keyboard plays its pitch on the track's instrument while the mouse is down", "[ui]")
{
	// Reference manual p. 1204.
	Fixture f({1024, 600}, "instrument");
	f.app.setDpRatio(1.0F);
	f.probe();
	// Notes of the keys' pitches, C3 selected: a click on a key leaves the selection as it was. The selection goes
	// first, as the inspector's sections change with it and could move the note display.
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 61, g_quarter}});
	f.app.call(
		"editor.set", {{"editor", "window"}, {"selection", Json::array({{{"part", f.part}, {"note", ids[0]}}})}});
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-editor-key[pitch=\"60\"]", "selected"); }));
	f.scroll(60);
	const Json keyboard = f.box(".key-editor-keys");
	const int left = keyboard.at("x").get<int>() + 2;
	const int right = keyboard.at("x").get<int>() + keyboard.at("width").get<int>() - 2;

	// The right part of a white key beside a black key: in C#3's row, at the keyboard's right, C3 plays until the
	// mouse is released.
	f.app.ui().press(f.window, f.key(right, 61), ui::MouseButton::Left, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.plays({{0x90, 60, 100}}));
	f.app.ui().release(f.window, f.key(right, 61), ui::MouseButton::Left, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.plays({{0x90, 60, 100}, {0x80, 60, 0}}));

	// At the left of that row the black key, C#3; released outside the key editor, over the editor window's toolbar,
	// it ends too. Neither click selects or deselects a note.
	f.clearSound();
	f.app.ui().press(f.window, f.key(left, 61), ui::MouseButton::Left, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.plays({{0x90, 61, 100}}));
	f.app.ui().release(f.window, of("#editor-keep-contents"), ui::MouseButton::Left, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.plays({{0x90, 61, 100}, {0x80, 61, 0}}));
	CHECK(f.selection() == std::vector<uint64_t>{ids[0]});

	// Released outside the window, where no mouseup arrives, D3 ends when the mouse leaves the window.
	f.clearSound();
	f.app.ui().press(f.window, f.key(right, 62), ui::MouseButton::Left, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.plays({{0x90, 62, 100}}));
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Context& context = window->context();
	context.ProcessMouseMove(-50, -50, 0);
	context.ProcessMouseLeave();
	context.ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.plays({{0x90, 62, 100}, {0x80, 62, 0}}));
}

TEST_CASE("dragging the keyboard's left part zooms the pitches and scrolls the note display", "[ui]")
{
	// Reference manual p. 1261: to the right or left it zooms vertically, up or down it scrolls.
	Fixture f({1024, 600});
	f.app.setDpRatio(1.0F);
	f.add({{0, 60, g_quarter}, {g_quarter, 64, 2 * g_quarter}});
	f.scroll(127); // the top of the pitches in view, where a zoom in keeps them
	const Json keyboard = f.box(".key-editor-keys");
	const Json body = f.box(".key-editor-body");
	const int left = keyboard.at("x").get<int>() + 4; // the keyboard's left 12 dp
	const double top = keyboard.at("y").get<double>();
	const auto at = [&](int x, int y) { return ui::Target{"", 0, ui::Point{x, y}}; };
	const int high = body.at("y").get<int>() + 150;
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - 10.0) <= 0.5);

	// A drag 100 px to the right doubles the rows' height; the pitch at the top of the view stays there.
	f.drag(at(left, high), at(left + 100, high));
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - 20.0) <= 0.5);
	CHECK(std::abs(f.box(".key-editor-keys").at("height").get<double>() - 2560.0) <= 1);
	CHECK(std::abs(f.box(".key-editor-grid").at("height").get<double>() - 2560.0) <= 1);
	CHECK(std::abs(f.box(".key-editor-key[pitch=\"127\"]").at("y").get<double>() - top) <= 1);

	// A drag 80 px up scrolls the pitches down by as much; the rows keep their height.
	f.drag(at(left, high), at(left, high - 80));
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - 20.0) <= 0.5);
	CHECK(std::abs(f.box(".key-editor-key[pitch=\"127\"]").at("y").get<double>() - (top - 80)) <= 1);

	// Two drags 200 px to the left zoom out, the second to the least: rows of 2.5 dp.
	f.drag(at(left, high), at(left - 200, high));
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - 5.0) <= 0.5);
	f.drag(at(left, high), at(left - 200, high));
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - 2.5) <= 0.5);
	CHECK(std::abs(f.box(".key-editor-keys").at("height").get<double>() - 320.0) <= 1);

	// A drag 500 px to the right zooms in to the most: rows of 40 dp.
	f.drag(at(left, high), at(left + 500, high));
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - 40.0) <= 0.5);
	CHECK(std::abs(f.box(".key-editor-keys").at("height").get<double>() - 5120.0) <= 1);

	// A click without a drag leaves the zoom as it is.
	f.click(at(left, high));
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - 40.0) <= 0.5);
}

TEST_CASE("the keyboard's zoom scales the rows' height that the style sheet gives them", "[ui]")
{
	// A theme or style change that gives the rows another height is what the zoom scales (#1369).
	Fixture f({1024, 600});
	f.app.setDpRatio(1.0F);
	f.add({{0, 60, g_quarter}});
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Context& context = window->context();
	Rml::ElementDocument* document = nullptr;
	for (int i = 0; i < context.GetNumDocuments(); ++i)
		if (context.GetDocument(i)->QuerySelector(".key-editor-rows") != nullptr)
			document = context.GetDocument(i);
	REQUIRE(document != nullptr);
	auto style = Rml::MakeShared<Rml::StyleSheetContainer>();
	style->MergeStyleSheetContainer(*document->GetStyleSheetContainer());
	style->MergeStyleSheetContainer(*Rml::Factory::InstanceStyleSheetString(
		".key-editor-rows .key-editor-row { height: 14dp; } .key-editor-body .key-editor-keys { height: 1792dp; }"));
	document->SetStyleSheetContainer(std::move(style));
	f.scroll(127);
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - 14.0) <= 0.5);

	// A drag 100 px to the right doubles the rows' height.
	const Json keyboard = f.box(".key-editor-keys");
	const int left = keyboard.at("x").get<int>() + 4;
	const int high = f.box(".key-editor-body").at("y").get<int>() + 150;
	f.drag({"", 0, ui::Point{left, high}}, {"", 0, ui::Point{left + 100, high}});
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - 28.0) <= 0.5);
	CHECK(std::abs(f.box(".key-editor-keys").at("height").get<double>() - (128 * 28.0)) <= 1);
}

TEST_CASE("a click and a drag on the keyboard's left part leave the selected notes and the history alone", "[ui]")
{
	// The zoom gesture's release edits nothing, and no note an earlier press remembered (review of #1365).
	Fixture f({1024, 600});
	f.app.setDpRatio(1.0F);
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 64, 2 * g_quarter}});
	// A click on the second note selects it; the press remembers it as the note pressed.
	f.click(f.at(8.5, 64));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{ids[1]}; }));
	f.scroll(127);
	const Json before = f.partNotes();
	const auto steps = f.app.call("history.get").at("steps").size();
	const Json keyboard = f.box(".key-editor-keys");
	const Json body = f.box(".key-editor-body");
	const int left = keyboard.at("x").get<int>() + 4; // the keyboard's left 12 dp
	const auto at = [&](int x, int y) { return ui::Target{"", 0, ui::Point{x, y}}; };
	const int high = body.at("y").get<int>() + 100;

	// A click there plays its pitch and edits nothing.
	f.click(at(left, high));
	CHECK(f.selection() == std::vector<uint64_t>{ids[1]});
	CHECK(f.app.call("history.get").at("steps").size() == steps);
	CHECK(f.partNotes() == before);

	// A drag there zooms, and its release doesn't stretch the selected note to where the mouse went.
	f.drag(at(left, high), at(left + 80, high + 60));
	CHECK(f.box(".key-editor-row").at("height").get<double>() > 10.5);
	CHECK(f.selection() == std::vector<uint64_t>{ids[1]});
	CHECK(f.app.call("history.get").at("steps").size() == steps);
	CHECK(f.partNotes() == before);
}

TEST_CASE("a key of the keyboard held while the key editor goes ends its pitch", "[ui]")
{
	Fixture f({1024, 600}, "instrument");
	f.app.setDpRatio(1.0F);
	f.probe();
	const auto keyEditor = [&]() -> ui::KeyEditor*
	{
		ui::MidiEditor* window = f.app.appUi().editorWindow();
		return window != nullptr ? window->keyEditor() : nullptr;
	};
	const auto press = [&](int pitch)
	{
		f.scroll(pitch);
		const Json keyboard = f.box(".key-editor-keys");
		f.app.ui().press(f.window, f.key(keyboard.at("x").get<int>() + 2, pitch), ui::MouseButton::Left, {});
		f.app.shell().step(std::chrono::milliseconds(0));
	};

	// The editor switching to the drum editor builds a key editor of its own: the key's pitch ends with the old one,
	// and the mouse going up in the new one ends nothing more.
	press(60);
	CHECK(f.plays({{0x90, 60, 100}}));
	f.app.call("editor.set", {{"editor", "window"}, {"content", "drum"}});
	REQUIRE(f.app.waitUntil([&] { return keyEditor() != nullptr && keyEditor()->drum(); }));
	CHECK(f.plays({{0x90, 60, 100}, {0x80, 60, 0}}));
	f.app.ui().release(f.window, of(".key-editor-grid"), ui::MouseButton::Left, {});
	for (int block = 0; block < 4; ++block) // what the release could send has arrived after a few UI steps and blocks
	{
		f.app.shell().step(std::chrono::milliseconds(0));
		CHECK(f.plays({{0x90, 60, 100}, {0x80, 60, 0}}));
	}

	// The editor closing through the engine API, as a key command closes it, ends the pitch with the editor window.
	f.clearSound();
	f.app.call("editor.set", {{"editor", "window"}, {"content", "key"}});
	REQUIRE(f.app.waitUntil([&] { return keyEditor() != nullptr && !keyEditor()->drum(); }));
	press(62);
	CHECK(f.plays({{0x90, 62, 100}}));
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	CHECK(f.plays({{0x90, 62, 100}, {0x80, 62, 0}}));

	// The user closing the editor window, as its close button does: the shell drops the window and its document first.
	// The document's mouseout, which RmlUi sends it as it goes, ends the pitch while the key editor is still there, and
	// the destructor, which runs after the window, is the backstop; the check holds either one, not each alone.
	f.clearSound();
	f.app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part})}});
	REQUIRE(f.app.waitUntil([&] { return keyEditor() != nullptr; }));
	f.window = f.app.appUi().editorWindowId();
	// The key editor is built before the engine answers: a press needs its view.
	REQUIRE(f.app.waitUntil([&] { return f.elements(".editor-part").size() == 1; }));
	press(64);
	CHECK(f.plays({{0x90, 64, 100}}));
	f.app.ui().close(f.window, false);
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	CHECK(f.plays({{0x90, 64, 100}, {0x80, 64, 0}}));
}

TEST_CASE("a key of the keyboard held while the lower zone's Editor panel goes ends its pitch", "[ui]")
{
	// The docking system owns the Editor panel and its key editor. A key held in the panel's window ends its pitch when
	// that window is closed, as its close button does, and when the panel is docked again. Docking::applyFloating drops
	// the panel's content before the shell closes the window of a panel docked again, so there the key editor's
	// destructor sends the note off; the editor window's check can't hold that.
	Fixture f({1024, 600}, "instrument");
	f.app.setDpRatio(1.0F);
	f.probe();
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.appUi().showProject();
	ui::Docking& docking = f.app.appUi().docking();
	const auto keyEditor = [&]() -> ui::KeyEditor*
	{
		ui::MidiEditor* editor = f.app.appUi().lowerZoneEditor();
		return editor != nullptr ? editor->keyEditor() : nullptr;
	};
	// The part in the lower zone's editor, the Editor panel in a window of its own and C3 held on its keyboard. The
	// panel's key editor is new each time: the editor view is opened again, as the panel going closes it.
	const auto floatAndPress = [&]
	{
		docking.setFloating("editor", true);
		f.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(docking.windowOf("editor") != nullptr);
		f.window = docking.windowOf("editor")->id();
		f.app.call("editor.open", {{"editor", "lower"}, {"parts", Json::array({f.part})}});
		REQUIRE(f.app.waitUntil([&] { return keyEditor() != nullptr; }));
		REQUIRE(f.app.waitUntil([&] { return f.elements(".key-editor-keys").size() == 1; }));
		REQUIRE(f.app.waitUntil([&] { return keyEditor() != nullptr && keyEditor()->scrollToPitch(60); }));
		f.app.shell().step(std::chrono::milliseconds(0));
		const Json keyboard = f.box(".key-editor-keys");
		f.app.ui().press(f.window, f.key(keyboard.at("x").get<int>() + 2, 60), ui::MouseButton::Left, {});
		f.app.shell().step(std::chrono::milliseconds(0));
		CHECK(f.plays({{0x90, 60, 100}}));
	};

	// The panel's window closed, as its close button does: the panel is docked again by the shell's close request.
	floatAndPress();
	f.app.ui().close(f.window, false);
	REQUIRE(f.app.waitUntil([&] { return docking.windowOf("editor") == nullptr; }));
	CHECK(f.plays({{0x90, 60, 100}, {0x80, 60, 0}}));

	// The panel docked again, as the window's Dock button does.
	f.clearSound();
	floatAndPress();
	docking.setFloating("editor", false);
	REQUIRE(f.app.waitUntil([&] { return docking.windowOf("editor") == nullptr; }));
	CHECK(f.plays({{0x90, 60, 100}, {0x80, 60, 0}}));
}

TEST_CASE("the object selection tool selects, moves, resizes, inserts and deletes notes", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 62, g_quarter}});
	// A click selects, Ctrl and a click adds, a click in the empty grid deselects (p. 1211).
	f.click(of(noteSelector(ids[0])));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{ids[0]}; }));
	f.click(of(noteSelector(ids[1])), 1, {"ctrl"});
	REQUIRE(f.app.waitUntil([&] { return f.selection().size() == 2; }));
	f.click(f.at(12.5, 70));
	REQUIRE(f.app.waitUntil([&] { return f.selection().empty(); }));

	// A drag moves the note in time and pitch, snapped to the quantize grid (p. 1213).
	f.drag(f.at(2, 60), f.at(4.2, 63));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 63; }));
	f.settle();
	CHECK(f.note(ids[0]).at("position") == 2 * g_sixteenth);
	// A drag at a note's end resizes it (p. 1214).
	f.drag(f.at(7.95, 62), f.at(12, 62));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).at("length") == 2 * g_quarter; }));
	f.settle();
	CHECK(f.note(ids[1]).at("position") == g_quarter);

	// A double click in the empty grid inserts a note of the length quantize's length, on a note deletes it.
	f.click(f.at(12.5, 67), 2);
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 3; }));
	f.settle();
	const Json inserted = f.partNotes().back();
	CHECK(inserted.at("pitch") == 67);
	CHECK(inserted.at("position") == 3 * g_quarter);
	CHECK(inserted.at("length") == g_sixteenth); // Quantize Link: the 1/16 grid
	f.click(of(noteSelector(ids[0])), 2);
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).is_null(); }));
	f.settle();
}

TEST_CASE("the draw tool draws notes of the length quantize's length or a multiple of it", "[ui]")
{
	Fixture f;
	f.add({});
	f.tool("draw");
	f.click(f.at(4.5, 64));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 1; }));
	f.settle();
	CHECK(f.partNotes()[0].at("position") == g_quarter);
	CHECK(f.partNotes()[0].at("length") == g_sixteenth);
	CHECK(f.partNotes()[0].at("pitch") == 64);
	// With an eighth as the length quantize, a drag over two and a half eighths draws three.
	f.click(of("#key-length-quantize"));
	f.click(of("#key-length-menu menuitem[length=\"1/8\"]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("lengthQuantize") == "1/8"; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#key-length-quantize").at(0).value("text", "") == "1/8"; }));
	f.drag(f.at(8.5, 60), f.at(13, 60));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 2; }));
	f.settle();
	CHECK(f.partNotes()[1].at("position") == 2 * g_quarter);
	CHECK(f.partNotes()[1].at("length") == 6 * g_sixteenth);
}

TEST_CASE("the draw tool follows a quantize grid changed in the project window", "[ui]")
{
	Fixture f;
	f.add({});
	f.app.call("quantize.set", {{"settings", {{"grid", "1/8"}}}});         // no undo step, so no history.changed
	f.app.waitUntil([] { return false; }, std::chrono::milliseconds(300)); // time for the editor to fetch the project
	f.tool("draw");
	// On the eighth grid a click at three and a half sixteenths draws an eighth note (Quantize Link) at one eighth.
	f.click(f.at(3.5, 64));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 1; }));
	f.settle();
	CHECK(f.partNotes()[0].at("position") == 2 * g_sixteenth);
	CHECK(f.partNotes()[0].at("length") == 2 * g_sixteenth);
}

TEST_CASE("the erase, trim, split and glue tools", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 62, g_quarter},
		{2 * g_quarter, 64, g_quarter}, {3 * g_quarter, 64, 2 * g_sixteenth}});

	// Erase deletes the note clicked (p. 1212).
	f.tool("erase");
	f.click(of(noteSelector(ids[0])));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).is_null(); }));
	f.settle();

	// Trim cuts off the end from the mouse on, with Alt the start (p. 1215).
	f.tool("trim");
	f.click(f.at(7, 62));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).at("length").get<int64_t>() < g_quarter; }));
	f.settle();
	CHECK(std::abs(f.note(ids[1]).at("length").get<int64_t>() - (3 * g_sixteenth)) < g_sixteenth / 4);
	f.click(f.at(5, 62), 1, {"alt"});
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).at("position").get<int64_t>() > g_quarter; }));
	f.settle();
	CHECK(std::abs(f.note(ids[1]).at("position").get<int64_t>() - (5 * g_sixteenth)) < g_sixteenth / 4);

	// Split cuts the note in two at the snapped position (p. 1215), its expression too: the rest starts with the
	// values at the cut.
	const auto tuning = [](int64_t position, double value)
	{ return Json{{"type", "tuning"}, {"position", position}, {"value", value}}; };
	f.app.call("note.set",
		{{"part", f.part},
			{"notes",
				{{{"id", ids[2]},
					{"expressions", {tuning(0, 0.0), tuning(g_sixteenth, 2.0), tuning(3 * g_sixteenth, 4.0)}}}}}});
	f.tool("split");
	f.click(f.at(10.2, 64));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 4; }));
	f.settle();
	CHECK(f.note(ids[2]).at("length") == 2 * g_sixteenth);
	const Json pieces = f.partNotes();
	CHECK(pieces[2].at("position") == 10 * g_sixteenth);
	CHECK(pieces[2].at("length") == 2 * g_sixteenth);
	CHECK(pieces[2].at("pitch") == 64);
	CHECK(f.note(ids[2]).at("expressions") == Json::array({tuning(0, 0.0), tuning(g_sixteenth, 2.0)}));
	CHECK(pieces[2].at("expressions") == Json::array({tuning(0, 2.0), tuning(g_sixteenth, 4.0)}));

	// Glue joins the note to the next one of its pitch (p. 1215), back to the expression it had before the cut.
	f.tool("glue");
	f.click(f.at(8.5, 64));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 3; }));
	f.settle();
	CHECK(f.note(ids[2]).at("length") == g_quarter);
	CHECK(f.note(ids[2]).at("expressions") ==
		Json::array({tuning(0, 0.0), tuning(g_sixteenth, 2.0), tuning(3 * g_sixteenth, 4.0)}));
	f.click(f.at(8.5, 64));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 2; }));
	f.settle();
	CHECK(f.note(ids[2]).at("length") == 6 * g_sixteenth);
	CHECK(f.note(ids[3]).is_null());
}

TEST_CASE("the split tool splits the selected notes of every part at one position, undone in one step", "[ui]")
{
	// Reference manual p. 1215: with notes selected, all of them are split at the same position (review of #1480).
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {0, 62, g_quarter}});
	const uint64_t track = f.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const uint64_t second = f.app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}})
								.at("part")
								.at("id")
								.get<uint64_t>();
	const uint64_t other =
		f.app
			.call("note.add",
				{{"part", second},
					{"notes", {{{"position", 0}, {"length", g_quarter}, {"pitch", 64}, {"velocity", 0.5}}}}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	f.app.call("editor.open", {{"editor", "window"}, {"parts", {f.part, second}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note").size() == 3; }));
	f.scroll(60);
	// The notes of the parts by part, as [position, length] sorted.
	const auto pieces = [&]
	{
		std::vector<std::vector<std::pair<int64_t, int64_t>>> found(2);
		const Json project = f.app.call("project.get");
		for (const Json& each : project.at("midiParts"))
			for (const Json& note : each.at("notes"))
				found.at(each.at("id") == f.part ? 0 : 1)
					.emplace_back(note.at("position").get<int64_t>(), note.at("length").get<int64_t>());
		for (auto& part : found)
			std::sort(part.begin(), part.end());
		return found;
	};
	const auto before = pieces();
	f.app.call("editor.set",
		{{"editor", "window"},
			{"selection", Json::array({{{"part", f.part}, {"note", ids[0]}}, {{"part", second}, {"note", other}}})}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note.selected").size() == 2; }));

	// One click on a selected note splits both selected notes at its position; the unselected one stays whole.
	f.tool("split");
	f.click(f.at(2.2, 60));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note").size() == 5; }));
	using Pieces = std::vector<std::pair<int64_t, int64_t>>;
	const auto split = pieces();
	CHECK(split[0] == Pieces{{0, 2 * g_sixteenth}, {0, g_quarter}, {2 * g_sixteenth, 2 * g_sixteenth}});
	CHECK(split[1] == Pieces{{0, 2 * g_sixteenth}, {2 * g_sixteenth, 2 * g_sixteenth}});

	// One undo step takes back both parts' splits.
	f.app.call("history.undo");
	CHECK(pieces() == before);
}

TEST_CASE("a right click in the note display opens the toolbox, by the setting and modifiers", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}});
	const auto rightClick = [&f](const ui::Target& target, const std::vector<std::string>& modifiers)
	{
		f.app.ui().click(f.window, target, ui::MouseButton::Right, 1, modifiers);
		f.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto toolbox = [&f] { return f.elements("#key-editor-toolbox").at(0).at("visible").get<bool>(); };
	const auto tool = [&f] { return f.app.appUi().editorWindow()->keyEditor()->tool(); };

	// Off, as by default: with a modifier key the toolbox, the toolbar's tools at the mouse (reference manual p. 65),
	// the tool chosen checked; without one nothing yet, as the key editor has no context menu.
	rightClick(f.at(8, 70), {});
	CHECK(!toolbox());
	rightClick(f.at(8, 70), {"shift"});
	REQUIRE(toolbox());
	CHECK(f.elements("#key-editor-toolbox > menuitem").size() == 7);
	CHECK(f.elements("#key-editor-toolbox > menuitem[checked]").at(0).at("attributes").at("key-tool") ==
		"objectSelection");
	// A tool in it chooses the tool, as the toolbar does, and closes it.
	f.click(of("#key-editor-toolbox > menuitem[key-tool=draw]"));
	CHECK(tool() == ui::KeyTool::Draw);
	CHECK(f.has(".key-tool[key-tool=draw]", "checked"));
	CHECK(!toolbox());

	// On: a right click without a modifier opens it, also on a note, which it leaves unselected.
	f.app.call("settings.set", {{"key", "ui.toolboxOnRightClick"}, {"value", true}});
	REQUIRE(f.app.waitUntil(
		[&f] { return !f.elements(".key-editor-grid").at(0).at("attributes").value("contextmenu", "").empty(); }));
	rightClick(of(noteSelector(ids[0])), {});
	REQUIRE(toolbox());
	CHECK(f.selection().empty());
	f.click(of("#key-editor-toolbox > menuitem[key-tool=erase]"));
	CHECK(tool() == ui::KeyTool::Erase);
	CHECK(f.partNotes().size() == 1);
	rightClick(f.at(8, 70), {"ctrl"});
	CHECK(!toolbox());
	f.app.call("settings.set", {{"key", "ui.toolboxOnRightClick"}, {"value", false}});
}

TEST_CASE("the transpose palette and the info line edit the selected notes", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 64, g_quarter}});
	CHECK(f.has(".key-editor-info", "empty"));
	f.click(of(noteSelector(ids[0])));
	f.click(of(noteSelector(ids[1])), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.selection().size() == 2; }));
	// The info line shows the first selected note, in another color with several (p. 1199). The engine's selection
	// comes before the display's: the first click's editor.changed can show one note selected again until the
	// second's arrives, so a palette click before that would transpose only the first.
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-editor-info", "multiple"); }));
	CHECK(f.elements(".key-editor-info textdisplay[field=pitch]").at(0).at("attributes").at("value") == "C3");
	CHECK(f.elements(".key-editor-info textdisplay[field=start]").at(0).at("attributes").at("value") == "1.1.1.0");
	CHECK(f.elements(".key-editor-info textdisplay[field=velocity]").at(0).at("attributes").at("value") == "64");

	// Move Up, Move Down More (p. 1195).
	f.click(of(".key-transpose[semitones=\"1\"]"));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).at("pitch") == 65; }));
	f.settle();
	CHECK(f.note(ids[0]).at("pitch") == 61);
	f.click(of(".key-transpose[semitones=\"-12\"]"));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 49; }));
	f.settle();
	CHECK(f.note(ids[1]).at("pitch") == 53);

	// Typing a pitch moves every selected note by as much as the first one.
	f.click(of(".key-editor-info textdisplay[field=pitch]"), 2);
	f.app.ui().key(f.window, "Mod+A");
	f.app.ui().text(f.window, "C3");
	f.app.ui().key(f.window, "Enter");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 60; }));
	f.settle();
	CHECK(f.note(ids[1]).at("pitch") == 64);
}

TEST_CASE("step input and MIDI input in the key editor", "[ui]")
{
	Fixture f;
	// Step input (p. 1219): the cursor shows at the part's start, notes played go in there and it moves on.
	CHECK(f.has(".key-editor-step-cursor", "off"));
	f.click(of(".key-input[setting=stepInput]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("stepInput") == true; }));
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-editor-step-cursor", "off"); }));
	CHECK(f.has(".key-input[setting=stepInput]", "checked"));
	const double x0 = f.box(".key-editor-step-cursor").at("x").get<double>();
	f.app.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 60}}, {{"pitch", 64}}})}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note").size() == 2; }));
	REQUIRE(f.app.waitUntil([&] { return f.box(".key-editor-step-cursor").at("x").get<double>() > x0; }));
	// A click in the grid moves the cursor (p. 1220).
	f.scroll(60);
	f.click(f.at(8.25, 66));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("stepPosition") == 2 * g_quarter; }));

	// MIDI input (p. 1219): the selected note takes the played pitch, and the next note is selected.
	f.click(of(".key-input[setting=stepInput]"));
	f.click(of(".key-input[setting=midiInput]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("midiInput") == true && f.view().at("stepInput") == false; }));
	const Json notes = f.partNotes();
	f.click(of(noteSelector(notes[0].at("id").get<uint64_t>())));
	REQUIRE(f.app.waitUntil([&] { return f.selection().size() == 1; }));
	f.app.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 50}}})}});
	REQUIRE(f.app.waitUntil([&] { return f.note(notes[0].at("id").get<uint64_t>()).at("pitch") == 50; }));
	f.settle();
	REQUIRE(f.app.waitUntil([&] { return f.has(noteSelector(notes[1].at("id").get<uint64_t>()), "selected"); }));
}

TEST_CASE("the inspector's Chord Editing section shows and changes the chord of the selected notes", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {0, 64, g_quarter}, {0, 67, g_quarter}});
	f.app.appUi().docking().open("editorInspector");
	REQUIRE(f.app.waitUntil([&] { return f.elements(".editor-chord-type").size() == 1; }));
	f.app.call("editor.set",
		{{"editor", "window"},
			{"selection",
				Json::array({{{"part", f.part}, {"note", ids[0]}}, {{"part", f.part}, {"note", ids[1]}},
					{{"part", f.part}, {"note", ids[2]}}})}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".editor-chord-type").at(0).value("text", "") == "C"; }));
	f.click(of(".editor-chord[chord=minor]"));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).at("pitch") == 63; }));
	f.settle();
	REQUIRE(f.app.waitUntil([&] { return f.elements(".editor-chord-type").at(0).value("text", "") == "Cm"; }));
	f.click(of(".editor-voicing[voicing=invertUp]"));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 72; }));
	f.settle();
	CHECK(f.elements(".editor-chord-type").at(0).value("text", "") == "Cm");
}
