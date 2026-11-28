// SPDX-License-Identifier: GPL-3.0-or-later
// The key editor in the app (#127, docs/features/key-editor.md), driven with ui::Automation as MCP's UI tools drive
// it: the note display and its keyboard, every tool, the transpose palette, the info line, step and MIDI input and the
// inspector's Chord Editing section. tests/keyeditor_tests.cpp checks the engine's side.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/keyeditor.h"
#include "ui/midieditor.h"
#include "ui/rmlui.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

	// A recorded MPE note's tuning, a point every few milliseconds (#1671): count points over the note's length, one
	// at 10 semitones, the next at -10.
	Json densePoints(int count, int64_t length)
	{
		Json points = Json::array();
		for (int i = 0; i < count; ++i)
			points.push_back(
				{{"type", "tuning"}, {"position", length * i / count}, {"value", i % 2 == 0 ? 10.0 : -10.0}});
		return points;
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

		// Whether the note is shown on the pitch's row (at() takes the time too, which the row doesn't depend on).
		bool on(uint64_t id, double sixteenths, int pitch)
		{
			const Json found = elements(noteSelector(id));
			if (found.empty())
				return false;
			const auto y = static_cast<double>(at(sixteenths, pitch).point.value_or(ui::Point{}).y);
			const Json shown = found.at(0).at("box");
			return shown.at("y").get<double>() <= y &&
				y <= shown.at("y").get<double>() + shown.at("height").get<double>();
		}

		// Pixels of the editor window's layout in desktop coordinates, which setBounds takes: at a monitor scale of 2,
		// as on a HiDPI Mac, half as many (#1746).
		int desktop(double pixels)
		{
			const ui::WindowId id = app.appUi().editorWindowId();
			const ui::Window* shown = app.shell().find(id);
			REQUIRE(shown != nullptr);
			const double scale = static_cast<double>(app.shell().bounds(id).width) /
				static_cast<double>(std::max(shown->context().GetDimensions().x, 1));
			return static_cast<int>(std::lround(pixels * scale));
		}

		// Waits until a note with at least four expression points per pixel shows one line per pixel at most, and
		// lines for four in five pixels at least, as the lines built for the width it has now (#1671; a line joined
		// for fewer pixels spans several). Returns the note's width.
		double linesFit(uint64_t id)
		{
			const std::string steps = noteSelector(id) + " .key-expression-step";
			double width = 0.0;
			double shown = 0.0;
			const bool fit = app.waitUntil(
				[&]
				{
					width = box(noteSelector(id)).at("width").get<double>();
					shown = static_cast<double>(elements(steps).size());
					return shown <= width + 2 && shown >= width * 0.8;
				});
			INFO("width " << width << ", lines " << shown);
			CHECK(fit);
			return width;
		}

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

namespace
{
	// Adds the style sheet to the key editor's document, as a theme or another style change would.
	void restyle(Fixture& f, const std::string& rcss)
	{
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
		style->MergeStyleSheetContainer(*Rml::Factory::InstanceStyleSheetString(rcss));
		document->SetStyleSheetContainer(std::move(style));
		f.app.shell().step(std::chrono::milliseconds(0));
	}

	double rowHeight(Fixture& f)
	{
		return f.box(".key-editor-row").at("height").get<double>();
	}

	double keyboardHeight(Fixture& f)
	{
		return f.box(".key-editor-keys").at("height").get<double>();
	}

	// A drag on the keyboard's left part, dx px to the right.
	void zoomDrag(Fixture& f, int dx)
	{
		const int left = f.box(".key-editor-keys").at("x").get<int>() + 4;
		const int high = f.box(".key-editor-body").at("y").get<int>() + 150;
		f.drag({"", 0, ui::Point{left, high}}, {"", 0, ui::Point{left + dx, high}});
	}
}

TEST_CASE("the keyboard's zoom scales the rows' height that the style sheet gives them", "[ui]")
{
	// A theme or style change that gives the rows another height is what the zoom scales (#1369), and the keyboard
	// is as high as the rows without a height of its own (#1547).
	Fixture f({1024, 600});
	f.app.setDpRatio(1.0F);
	f.add({{0, 60, g_quarter}});
	restyle(f, ".key-editor-rows .key-editor-row { height: 14dp; }");
	f.scroll(127);
	CHECK(std::abs(rowHeight(f) - 14.0) <= 0.5);
	CHECK(std::abs(keyboardHeight(f) - (128 * 14.0)) <= 1);

	// A drag 100 px to the right doubles the rows' height.
	zoomDrag(f, 100);
	CHECK(std::abs(rowHeight(f) - 28.0) <= 0.5);
	CHECK(std::abs(keyboardHeight(f) - (128 * 28.0)) <= 1);
}

TEST_CASE("a zoomed key editor follows a style change to the rows' height", "[ui]")
{
	// The zoom scales the height the style sheet gives the rows, not the one it gave them itself (#1547).
	Fixture f({1024, 600});
	f.app.setDpRatio(1.0F);
	f.add({{0, 60, g_quarter}});
	f.scroll(127);
	zoomDrag(f, 100);
	CHECK(std::abs(rowHeight(f) - 20.0) <= 0.5);

	// At that zoom the next press on the keyboard's left part scales the new height, a click or a drag.
	restyle(f, ".key-editor-rows .key-editor-row { height: 14dp; }");
	const int left = f.box(".key-editor-keys").at("x").get<int>() + 4;
	f.click({"", 0, ui::Point{left, f.box(".key-editor-body").at("y").get<int>() + 150}});
	CHECK(std::abs(rowHeight(f) - 28.0) <= 0.5);
	CHECK(std::abs(keyboardHeight(f) - (128 * 28.0)) <= 1);

	// Back at zoom 1 the rows have the style sheet's height and follow its next change at once.
	zoomDrag(f, -100);
	CHECK(std::abs(rowHeight(f) - 14.0) <= 0.5);
	restyle(f, ".key-editor-body .key-editor-rows .key-editor-row { height: 12dp; }");
	CHECK(std::abs(rowHeight(f) - 12.0) <= 0.5);
	CHECK(std::abs(keyboardHeight(f) - (128 * 12.0)) <= 1);
}

TEST_CASE("the keyboard's zoom zooms nothing when the style sheet gives the rows no length", "[ui]")
{
	// Rows sized by their content or a minimum, not a height, have nothing to scale: the zoom leaves them and the
	// keyboard as they are (#1547).
	Fixture f({1024, 600});
	f.app.setDpRatio(1.0F);
	f.add({{0, 60, g_quarter}});
	restyle(f, ".key-editor-rows .key-editor-row { height: auto; min-height: 10dp; }");
	f.scroll(60); // away from the top, where a zoom would move the pitches in view
	const double top = f.box(".key-editor-key[pitch=\"60\"]").at("y").get<double>();
	CHECK(std::abs(rowHeight(f) - 10.0) <= 0.5);
	zoomDrag(f, 100);
	CHECK(std::abs(rowHeight(f) - 10.0) <= 0.5);
	CHECK(std::abs(keyboardHeight(f) - 1280.0) <= 1);
	CHECK(std::abs(f.box(".key-editor-key[pitch=\"60\"]").at("y").get<double>() - top) <= 1);

	// The rows keep no height of the zoom's: a later style's length applies at once (#1627).
	restyle(f, ".key-editor-body .key-editor-rows .key-editor-row { height: 12dp; min-height: 0; }");
	CHECK(std::abs(rowHeight(f) - 12.0) <= 0.5);
	CHECK(std::abs(keyboardHeight(f) - (128 * 12.0)) <= 1);
}

TEST_CASE("the keyboard's zoom snaps to 1 near it, and the rows follow a style change at once again", "[ui]")
{
	// A drag back rarely lands on exactly the zoom it started from; near 1 the zoom is 1, so the zoom's heights go
	// and a theme or style change reaches the rows without a press on the keyboard (#1627).
	Fixture f({1024, 600});
	f.app.setDpRatio(1.0F);
	f.add({{0, 60, g_quarter}});
	f.scroll(127);
	zoomDrag(f, 100);
	CHECK(std::abs(rowHeight(f) - 20.0) <= 0.5);
	zoomDrag(f, -97); // 3 dp short of zoom 1
	CHECK(std::abs(rowHeight(f) - 10.0) <= 0.5);
	restyle(f, ".key-editor-body .key-editor-rows .key-editor-row { height: 12dp; }");
	CHECK(std::abs(rowHeight(f) - 12.0) <= 0.5);

	// Further away it zooms as before.
	zoomDrag(f, 20);
	CHECK(std::abs(rowHeight(f) - (12.0 * std::exp2(0.2))) <= 0.5);
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
		// The key editor is built before the engine answers: a press needs its view and the project, which the one
		// part in the panel's window shows.
		REQUIRE(f.app.waitUntil([&] { return f.elements(".editor-part").size() == 1; }));
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

TEST_CASE("a key of the keyboard held while the lower zone's Editor panel is hidden or floated ends its pitch", "[ui]")
{
	// The panel in the lower zone of the project window, its key editor kept when the panel goes: the held key's pitch
	// ends however the panel goes, at the latest when the mouse is released.
	Fixture f({1024, 600}, "instrument");
	f.app.setDpRatio(1.0F);
	f.probe();
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.appUi().showProject();
	f.window = std::nullopt;
	ui::Docking& docking = f.app.appUi().docking();
	const auto keyEditor = [&]() -> ui::KeyEditor*
	{
		ui::MidiEditor* editor = f.app.appUi().lowerZoneEditor();
		return editor != nullptr ? editor->keyEditor() : nullptr;
	};
	const auto pressDocked = [&]
	{
		f.app.call("editor.open", {{"editor", "lower"}, {"parts", Json::array({f.part})}});
		REQUIRE(f.app.waitUntil([&] { return keyEditor() != nullptr; }));
		REQUIRE(f.app.waitUntil([&] { return f.elements(".editor-part").size() == 1; }));
		REQUIRE(f.app.waitUntil([&] { return f.elements(".key-editor-keys").size() == 1; }));
		REQUIRE(f.app.waitUntil([&] { return keyEditor() != nullptr && keyEditor()->scrollToPitch(60); }));
		f.app.shell().step(std::chrono::milliseconds(0));
		const Json keyboard = f.box(".key-editor-keys");
		f.app.ui().press(f.window, f.key(keyboard.at("x").get<int>() + 2, 60), ui::MouseButton::Left, {});
		f.app.shell().step(std::chrono::milliseconds(0));
		CHECK(f.plays({{0x90, 60, 100}}));
	};

	// The tab hidden.
	pressDocked();
	docking.setTabHidden("editor", true);
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.ui().release(f.window, ui::Target{"", 0, ui::Point{500, 300}}, ui::MouseButton::Left, {});
	CHECK(f.plays({{0x90, 60, 100}, {0x80, 60, 0}}));
	docking.setTabHidden("editor", false);
	f.app.shell().step(std::chrono::milliseconds(0));

	// The panel opened in a window of its own.
	f.clearSound();
	pressDocked();
	docking.setFloating("editor", true);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(docking.windowOf("editor") != nullptr);
	f.app.ui().release(f.window, ui::Target{"", 0, ui::Point{500, 300}}, ui::MouseButton::Left, {});
	CHECK(f.plays({{0x90, 60, 100}, {0x80, 60, 0}}));
}

TEST_CASE("the object selection tool selects, moves, resizes, inserts and deletes notes", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 62, g_quarter}});
	// A click selects, Ctrl and a click adds, a click in the empty grid deselects (p. 1211).
	f.click(of(noteSelector(ids[0])));
	REQUIRE(f.app.waitUntil([&] { return f.has(noteSelector(ids[0]), "selected"); }));
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
	{
		const ui::Target from = f.at(7.95, 62);
		const Json box = f.box(noteSelector(ids[1]));
		f.drag(from, f.at(12, 62));
		Json note;
		const bool resized = f.app.waitUntil(
			[&]
			{
				note = f.note(ids[1]);
				return note.at("length") == 2 * g_quarter;
			});
		const ui::Point pressed = from.point.value_or(ui::Point{});
		INFO("pressed at " << pressed.x << ", " << pressed.y << " on the note's box " << box.dump() << ", the note now "
						   << note.dump());
		REQUIRE(resized);
	}
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

TEST_CASE("a view older than a click that deselects leaves the notes and the controller lanes deselected", "[ui]")
{
	// On a loaded machine the engine's answers to an earlier selection, such as its editor.changed, can come in after a
	// click selected anew, before the click's own answer. Until that, the notes stay as the click selected them, so a
	// drag right after the click moves only those (#1817), and the controller lanes show the click's selection too, so
	// that a lane edit acts on the same notes (#1897).
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 62, g_quarter}});
	f.click(of(noteSelector(ids[0])));
	f.click(of(noteSelector(ids[1])), 1, {"ctrl"});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note.selected").size() == 2; }));
	f.settle();
	const Json project = f.app.call("project.get");
	const Json older = f.view();
	REQUIRE(older.at("selection").size() == 2);
	// A click in the empty grid deselects; the view with both notes selected comes in before its answer.
	f.app.ui().click(f.window, f.at(12.5, 70), ui::MouseButton::Left, 1, {});
	f.app.appUi().editorWindow()->keyEditor()->show(project, older, 0);
	CHECK(f.elements(".key-note.selected").empty());
	CHECK(f.elements(".controller-bar").size() == 2);
	CHECK(f.elements(".controller-bar.selected").empty());
	f.drag(f.at(2, 60), f.at(4.2, 63));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 63; }));
	f.settle();
	CHECK(f.note(ids[1]).at("pitch") == 62);
	CHECK(f.note(ids[1]).at("position") == g_quarter);
}

TEST_CASE("a press whose selection the engine refuses shows the view's selection again", "[ui]")
{
	// The display shows a note the engine no longer has, as when another client has just deleted it: the press's
	// editor.set is refused, and once it is answered the notes and the controller lanes show the view's selection
	// again, not the press's (#1897).
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}});
	f.settle();
	Json project = f.app.call("project.get");
	const Json view = f.view();
	REQUIRE(view.at("selection").empty());
	uint64_t gone = 0;
	for (Json& part : project.at("midiParts"))
		if (part.at("id") == f.part)
		{
			Json note = part.at("notes").at(0);
			gone = note.at("id").get<uint64_t>() + 1000;
			note["id"] = gone;
			note["position"] = g_quarter;
			note["pitch"] = 62;
			part.at("notes").push_back(note);
		}
	REQUIRE(gone != 0);
	// No read is out that would show the engine's project again before the click is answered.
	ui::EngineBinding& binding = f.app.appUi().binding();
	REQUIRE(f.app.waitUntil([&] { return binding.waiting("project.get") == 0 && binding.waiting("editor.get") == 0; }));
	f.app.appUi().editorWindow()->keyEditor()->show(project, view, 0);
	REQUIRE(f.elements(".key-note").size() == 2);
	// A move of it released outside the key editor: no edit and no click, which would activate the part with an
	// editor.set of its own, whose editor.changed would show the view again anyway.
	f.app.ui().press(f.window, of(noteSelector(gone)), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.ui().move(f.window, f.at(8.5, 62));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.ui().release(f.window, of("#editor-keep-contents"), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note.selected").empty(); }));
	CHECK(f.elements(".controller-bar.selected").empty());
	CHECK(f.selection().empty());
	CHECK(f.partNotes().size() == 1);
	CHECK(f.note(ids[0]).at("position") == 0);
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

TEST_CASE("a move released outside the key editor ends without its effect, a draw released there draws", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_sixteenth}});
	const auto x = [&] { return f.box(noteSelector(ids[0])).at("x").get<double>(); };
	const double before = x();
	// A move of the note released over the editor window's toolbar, outside the key editor, ends there with the note
	// where it was: back over the editor it doesn't follow the mouse, and a click on the parts' row above the grid
	// moves nothing.
	f.app.ui().press(f.window, f.at(0.5, 60), ui::MouseButton::Left);
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{ids[0]}; }));
	f.app.ui().move(f.window, f.at(6.0, 60));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.ui().move(f.window, f.at(6.5, 60));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return std::abs(x() - before) > 1.5; }));
	f.app.ui().release(f.window, of("#editor-keep-contents"), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return std::abs(x() - before) <= 1.5; }));
	f.app.ui().move(f.window, f.at(8.5, 60));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(std::abs(x() - before) <= 1.5);
	f.click(of(".editor-parts"));
	CHECK_FALSE(f.app.waitUntil([&] { return f.note(ids[0]).at("position") != 0; }, std::chrono::milliseconds(300)));
	CHECK(f.note(ids[0]).at("pitch") == 60);
	// The draw tool's drag released above the editor draws as one released in it: from 8 sixteenths over three and a
	// half, a note of four.
	f.tool("draw");
	const ui::Point later = f.at(11.5, 64).point.value_or(ui::Point{});
	f.drag(f.at(8.3, 64), {"", 0, ui::Point{later.x, f.box(".key-editor").at("y").get<int>() - 4}});
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 2; }));
	CHECK(f.partNotes()[1].at("position") == 8 * g_sixteenth);
	CHECK(f.partNotes()[1].at("length") == 4 * g_sixteenth);
	CHECK(f.partNotes()[1].at("pitch") == 64);
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note.drawing").empty(); }));
}

TEST_CASE(
	"a draw whose release was lost ends without its effect as the mouse leaves the window or at the next press", "[ui]")
{
	Fixture f;
	f.scroll(64);
	f.tool("draw");
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Context& context = window->context();
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	// The mouse leaves the window before it moves, as when the window loses the focus: no drag starts, so no dragend
	// comes, and the draw ends at the leave, without its effect: the drawn note doesn't follow the mouse back over the
	// editor with the button up (#1756).
	f.app.ui().press(f.window, f.at(4.3, 64), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.elements(".key-note.drawing").size() == 1);
	context.ProcessMouseLeave();
	context.ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements(".key-note.drawing").empty());
	CHECK_FALSE(f.has(".key-editor", "in-gesture"));
	f.app.ui().move(f.window, f.at(8.5, 64));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements(".key-note.drawing").empty());
	CHECK_FALSE(f.app.waitUntil([&] { return !f.partNotes().empty(); }, std::chrono::milliseconds(300)));

	// A draw whose mouseup never comes at all ends at the next press, here on the drawn note: the end takes the note
	// out while the press still has it as its target, and the press draws nothing either.
	clock->advance(1.0); // no double click with the press before
	f.app.ui().press(f.window, f.at(4.3, 64), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.elements(".key-note.drawing").size() == 1);
	const Json drawn = f.box(".key-note.drawing");
	const ui::Point on{
		static_cast<int>(std::lround(drawn.at("x").get<double>() + (drawn.at("width").get<double>() / 2))),
		static_cast<int>(std::lround(drawn.at("y").get<double>() + (drawn.at("height").get<double>() / 2)))};
	const Rml::Element* hit = context.GetElementAtPoint({static_cast<float>(on.x), static_cast<float>(on.y)});
	REQUIRE(hit != nullptr);
	REQUIRE(hit->IsClassSet("drawing"));
	clock->advance(1.0);
	f.app.ui().press(f.window, {"", 0, on}, ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements(".key-note.drawing").empty());
	f.app.ui().release(f.window, {"", 0, on}, ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(f.app.waitUntil([&] { return !f.partNotes().empty(); }, std::chrono::milliseconds(300)));
	CHECK(f.elements(".key-note.drawing").empty());
	CHECK_FALSE(f.has(".key-editor", "in-gesture"));
}

TEST_CASE("a move whose release was lost ends without its effect as the mouse leaves the window", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_sixteenth}});
	const auto x = [&] { return f.box(noteSelector(ids[0])).at("x").get<double>(); };
	const double before = x();
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Context& context = window->context();
	// The mouse leaves the window before it moves, as when the window loses the focus: no drag starts, so no dragend
	// comes. The move ends at the leave (#1756): the grid is no drag element any more, the note doesn't follow the
	// mouse back over the grid with the button up, and the button going up there moves nothing.
	f.app.ui().press(f.window, f.at(0.5, 60), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.has(".key-editor", "in-gesture"));
	context.ProcessMouseLeave();
	context.ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(f.has(".key-editor", "in-gesture"));
	f.app.ui().move(f.window, f.at(6.5, 62));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(std::abs(x() - before) <= 1.5);
	f.app.ui().release(f.window, f.at(6.5, 62), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(f.app.waitUntil([&] { return f.note(ids[0]).at("position") != 0; }, std::chrono::milliseconds(300)));
	CHECK(f.note(ids[0]).at("pitch") == 60);
	f.settle();
}

TEST_CASE("a resize released outside the key editor ends without its effect", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}});
	const auto width = [&] { return f.box(noteSelector(ids[0])).at("width").get<double>(); };
	// A resize in the editor ends with the release: the grid is a drag element only while it goes on.
	f.drag(f.at(3.95, 60), f.at(6, 60));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("length") == 6 * g_sixteenth; }));
	CHECK_FALSE(f.has(".key-editor", "in-gesture"));
	// The note keeps the resize's preview until the engine's change arrives, so settle() alone can pass before it, and
	// the change arriving during the next resize puts the note back at its length. The info line shows the editor's
	// notes: once it has the new end, the change has arrived.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return f.elements(".key-editor-info textdisplay[field=end]").at(0).at("attributes").at("value") ==
				"1.2.3.0";
		}));
	f.settle();
	const double before = width();
	// At the note's end to the right, then released over the editor window's toolbar: the note keeps its length.
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->advance(1.0); // no double click with the press before
	f.app.ui().press(f.window, f.at(5.95, 60), ui::MouseButton::Left);
	f.app.ui().move(f.window, f.at(8.5, 60));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.ui().move(f.window, f.at(9.5, 60));
	f.app.shell().step(std::chrono::milliseconds(0));
	{
		double now = 0.0;
		const bool wider = f.app.waitUntil(
			[&]
			{
				now = width();
				return now > before + 10;
			});
		// A press that starts a move instead of a resize, or a re-render that loses the preview, shows here (#1808).
		INFO("the note " << now << " px wide, " << before << " before the resize, the key editor "
						 << (f.has(".key-editor", "in-gesture") ? "in" : "not in") << " a gesture");
		REQUIRE(wider);
	}
	// The display shown again during the resize, as when the engine's answers to the last edit come in only now, still
	// shows the note where the mouse takes it, although the mouse doesn't move (#1789).
	f.app.call("note.add",
		{{"part", f.part},
			{"notes",
				Json::array(
					{{{"position", 12 * g_sixteenth}, {"length", g_sixteenth}, {"pitch", 64}, {"velocity", 0.5}}})}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note").size() == 2; }));
	CHECK(width() > before + 10);
	f.app.ui().release(f.window, of("#editor-keep-contents"), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return std::abs(width() - before) <= 1.5; }));
	CHECK_FALSE(f.has(".key-editor", "in-gesture"));
	CHECK_FALSE(f.app.waitUntil(
		[&] { return f.note(ids[0]).at("length") != 6 * g_sixteenth; }, std::chrono::milliseconds(300)));
	f.settle();
}

TEST_CASE("a move shows its notes at the time under the mouse as the editor's range changes during it", "[ui]")
{
	// An in-place editor's range follows the project window's scroll and zoom (setTimeRange): during a move the notes
	// go where the release would put them at once, not only at the next mouse move (#1808).
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_sixteenth}});
	const Json grid = f.box(".key-editor-grid");
	const auto x = [&] { return f.box(noteSelector(ids[0])).at("x").get<double>(); };
	const auto shownAt = [&](double fraction)
	{ return grid.at("x").get<double>() + (fraction * grid.at("width").get<double>()); };
	f.app.ui().press(f.window, f.at(0.5, 60), ui::MouseButton::Left);
	f.app.ui().move(f.window, f.at(4.0, 60));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.ui().move(f.window, f.at(4.25, 60));
	f.app.shell().step(std::chrono::milliseconds(0));
	// From half a sixteenth to 4.25: the note 4 sixteenths on, snapped.
	REQUIRE(f.app.waitUntil([&] { return std::abs(x() - shownAt(4.0 / 16.0)) <= 1.5; }));
	// The range twice as long: the mouse is at 8.5 sixteenths now, the note 8 sixteenths on.
	f.app.appUi().editorWindow()->keyEditor()->setTimeRange(std::make_pair(int64_t{0}, 2 * g_bar));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(std::abs(x() - shownAt(8.0 / 32.0)) <= 1.5);
	f.app.ui().release(f.window, f.at(4.25, 60), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("position") == 8 * g_sixteenth; }));
	CHECK(f.note(ids[0]).at("pitch") == 60);
}

TEST_CASE("the key editor's draw and the drag on the keyboard's left part go on past the window's edge until the "
		  "button goes up",
	"[ui]")
{
	Fixture f({1024, 600});
	f.app.setDpRatio(1.0F);
	f.scroll(64);
	f.tool("draw");
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Context& context = window->context();
	// To the right over 3.5 sixteenths, then 40 px past the window's right edge, where the platform captures the mouse
	// while the button is held and nothing is hovered: released there, the note reaches the sixteenth out there.
	const Json grid = f.box(".key-editor-grid");
	f.app.ui().press(f.window, f.at(0.3, 64), ui::MouseButton::Left);
	f.app.ui().move(f.window, f.at(3.5, 64));
	f.app.shell().step(std::chrono::milliseconds(0));
	const int y = f.at(3.5, 64).point.value_or(ui::Point{}).y;
	const int out = context.GetDimensions().x + 40;
	context.ProcessMouseMove(out, y, 0);
	context.ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 1; }));
	const double sixteenths = (out - grid.at("x").get<double>()) / grid.at("width").get<double>() * 16.0;
	CHECK(f.partNotes()[0].at("position") == 0);
	CHECK(f.partNotes()[0].at("length") == static_cast<int64_t>(std::ceil(sixteenths)) * g_sixteenth);
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note.drawing").empty(); }));
	CHECK_FALSE(f.has(".key-editor", "in-gesture"));

	// The keyboard's left part dragged 60 px past the window's left edge zooms out by as much as in the window. Zoomed
	// in first, as the left zone puts the keyboard about 200 px into the window: from 1, a drag to the edge alone would
	// reach the least zoom (#1756).
	const int left = f.box(".key-editor-keys").at("x").get<int>() + 4;
	const int high = f.box(".key-editor-body").at("y").get<int>() + 150;
	REQUIRE(std::abs(f.box(".key-editor-row").at("height").get<double>() - 10.0) <= 0.5);
	f.drag({"", 0, ui::Point{left, high}}, {"", 0, ui::Point{left + 200, high}});
	REQUIRE(std::abs(f.box(".key-editor-row").at("height").get<double>() - 40.0) <= 0.5);
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->advance(1.0); // no double click with the press before
	f.app.ui().press(f.window, {"", 0, ui::Point{left, high}}, ui::MouseButton::Left);
	f.app.ui().move(f.window, {"", 0, ui::Point{left - 4, high}});
	f.app.shell().step(std::chrono::milliseconds(0));
	context.ProcessMouseMove(-60, high, 0);
	context.ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	const double zoomed = 40.0 * std::exp2((-60.0 - left) / 100.0);
	REQUIRE(zoomed > 3.0); // not the least zoom's 2.5 px
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - zoomed) <= 0.5);
	CHECK_FALSE(f.has(".key-editor", "in-gesture"));
	// Back in the window with the button up, the mouse zooms no more.
	f.app.ui().move(f.window, {"", 0, ui::Point{left + 200, high}});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(std::abs(f.box(".key-editor-row").at("height").get<double>() - zoomed) <= 0.5);
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

TEST_CASE("a gesture's undo step has the same name over notes of one part and of two", "[ui]")
{
	// #2014: one part's edit goes as its lone note.* command, several parts' in a history.transaction, which takes the
	// name the engine gives the lone command's step.
	Fixture f({1200, 800}, "instrument");
	const uint64_t first = f.add({{0, 60, g_quarter}}).at(0);
	const uint64_t track = f.app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
	const uint64_t part = f.app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", g_bar}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
	const uint64_t second =
		f.app
			.call("note.add",
				{{"part", part},
					{"notes", {{{"position", 0}, {"length", g_quarter}, {"pitch", 64}, {"velocity", 0.5}}}}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	// Both tracks have a map with an attribute, for the info line's articulations pop-up menu.
	const Json map = f.app
						 .call("articulationMap.add",
							 {{"name", "Strings"}, {"groups", {"Length"}},
								 {"articulations", {{{"name", "Staccato"}, {"type", "attribute"}, {"group", 0}}}}})
						 .at("map")
						 .at("id");
	for (const uint64_t each : {f.track, track})
		f.app.call("track.set", {{"track", each}, {"articulationMap", map}});
	f.app.call("editor.open", {{"editor", "window"}, {"parts", {f.part, part}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note").size() == 2; }));
	f.scroll(62);
	// The notes' boxes, rounded, to wait for the display to show the notes as they were after an undo.
	const auto boxes = [&f]
	{
		std::vector<std::pair<long, long>> found;
		for (const Json& each : f.elements(".key-note"))
			found.emplace_back(
				std::lround(each.at("box").at("x").get<double>()), std::lround(each.at("box").at("y").get<double>()));
		std::sort(found.begin(), found.end());
		return found;
	};
	const auto shown = boxes();

	// The gesture on the first note, selected alone or with the second one: a click in the empty grid gives the note
	// display the keyboard focus, then the view selects them, so that the gesture's press is no double click. Returns
	// the gesture's undo step's name, after undoing it.
	const auto name = [&](bool both, const std::function<void()>& gesture)
	{
		f.tool("objectSelection");
		f.click(f.at(12.5, 70));
		REQUIRE(f.app.waitUntil([&] { return f.selection().empty(); }));
		Json selection = {{{"part", f.part}, {"note", first}}};
		if (both)
			selection.push_back({{"part", part}, {"note", second}});
		f.app.call("editor.set", {{"editor", "window"}, {"selection", selection}});
		REQUIRE(f.app.waitUntil([&] { return f.selection().size() == (both ? 2U : 1U); }));
		REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note.selected").size() == (both ? 2U : 1U); }));
		REQUIRE(f.app.waitUntil([&] { return f.has(".key-editor-info", "multiple") == both; }));
		const auto position = f.app.call("history.get").at("position").get<size_t>();
		gesture();
		REQUIRE(f.app.waitUntil([&] { return f.app.call("history.get").at("position") == position + 1; }));
		const Json history = f.app.call("history.get");
		const std::string action = history.at("steps").at(position).at("action").get<std::string>();
		f.app.call("history.undo");
		REQUIRE(f.app.waitUntil([&] { return boxes() == shown; }));
		return action;
	};
	const auto same = [&](const std::string& expected, const std::function<void()>& gesture)
	{
		INFO(expected);
		CHECK(name(false, gesture) == expected);
		CHECK(name(true, gesture) == expected);
	};
	// A move or resize in the editor sets the notes' values (note.set); a move onto another in-place editor moves them
	// there (note.move, "Move notes", tests/inplaceeditor_ui_tests.cpp).
	same("Change notes", [&] { f.drag(f.at(2, 60), f.at(6, 60)); });
	same("Change notes", [&] { f.drag(f.at(3.9, 60), f.at(6, 60)); });
	same("Remove notes", [&] { f.app.ui().key(f.window, "Delete"); });
	same("Change notes", [&] { f.app.ui().key(f.window, "Up"); });
	same("Split notes",
		[&]
		{
			f.tool("split");
			f.click(f.at(2, 60));
		});
	// Articulations typed on the info line and chosen from its pop-up menu.
	same("Change notes",
		[&]
		{
			f.click(of(".key-editor-info textdisplay[field=articulations]"), 2);
			f.app.ui().key(f.window, "Mod+A");
			f.app.ui().text(f.window, "Staccato");
			f.app.ui().key(f.window, "Enter");
		});
	same("Change notes",
		[&]
		{
			f.click(of(".key-editor-info button.articulation-menu"));
			const std::string staccato = "#key-articulations-menu menuitem[articulation=Staccato]";
			REQUIRE(f.app.waitUntil(
				[&]
				{
					const Json found = f.elements(staccato);
					return !found.empty() && found.at(0).at("visible") == true;
				}));
			f.click(of(staccato));
		});
	// The erase tool's click deletes the note clicked alone; its drag is the drum editor's drumstick
	// (tests/drumeditor_ui_tests.cpp).
	CHECK(name(true,
			  [&]
			  {
				  f.tool("erase");
				  f.click(of(noteSelector(first)));
			  }) == "Remove notes");
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
	CHECK(f.elements("#key-editor-toolbox > menuitem").size() == 8); // with the mute tool (#1684)
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

TEST_CASE("a start typed on the info line before the part's start moves the selected notes as far as it", "[ui]")
{
	Fixture f;
	f.app.call("midiPart.set", {{"part", f.part}, {"position", g_bar}});
	const std::vector<uint64_t> ids = f.add({{g_quarter, 60, g_quarter}, {2 * g_quarter, 64, g_quarter}});
	f.click(of(noteSelector(ids[0])));
	f.click(of(noteSelector(ids[1])), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-editor-info", "multiple"); }));
	CHECK(f.elements(".key-editor-info textdisplay[field=start]").at(0).at("attributes").at("value") == "2.2.1.0");
	f.click(of(".key-editor-info textdisplay[field=start]"), 2);
	f.app.ui().key(f.window, "Mod+A");
	f.app.ui().text(f.window, "1.3.1.0");
	f.app.ui().key(f.window, "Enter");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("position") == 0; }));
	f.settle();
	CHECK(f.note(ids[1]).at("position") == g_quarter);
}

TEST_CASE("Up and Down transpose the selected notes, Delete and Backspace delete them", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids =
		f.add({{0, 60, g_quarter}, {g_quarter, 64, g_quarter}, {2 * g_quarter, 67, g_quarter}});
	f.click(of(noteSelector(ids[0])));
	f.click(of(noteSelector(ids[1])), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.selection().size() == 2; }));
	// By a semitone, with Shift by an octave (p. 1214).
	f.app.ui().key(f.window, "Up");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).at("pitch") == 65; }));
	f.settle();
	CHECK(f.note(ids[0]).at("pitch") == 61);
	f.app.ui().key(f.window, "Shift+Down");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 49; }));
	f.settle();
	f.app.ui().key(f.window, "Shift+Up");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 61; }));
	f.settle();
	f.app.ui().key(f.window, "Down");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 60; }));
	f.settle();
	CHECK(f.note(ids[1]).at("pitch") == 64);
	CHECK(f.note(ids[2]).at("pitch") == 67);

	// Delete removes the selected notes in one step; after its undo Backspace does too.
	f.app.ui().key(f.window, "Delete");
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 1; }));
	f.settle();
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 3; }));
	f.click(of(noteSelector(ids[2])));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{ids[2]}; }));
	f.app.ui().key(f.window, "Backspace");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[2]).is_null(); }));
	f.settle();
	CHECK(f.partNotes().size() == 2);

	// They are key commands of the key editor's focus zone (#679), which the Key Commands dialog binds to other keys:
	// with Transpose Up on J, J transposes, and Up does nothing more.
	f.app.call("settings.set",
		{{"key", "ui.keyCommands"}, {"value", {{{"command", "keyEditor.transposeUp"}, {"keys", {"J"}}}}}});
	REQUIRE(f.app.waitUntil(
		[&] { return f.app.keys().keysFor("keyEditor.transposeUp") == std::vector<std::string>{"J"}; }));
	f.click(of(noteSelector(ids[0])));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{ids[0]}; }));
	f.app.ui().key(f.window, "J");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 61; }));
	f.settle();
	f.app.ui().key(f.window, "Shift+Down");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("pitch") == 49; }));
	f.settle();
	f.app.ui().key(f.window, "Up");
	f.app.call("engine.version"); // the engine has what the UI sent before
	CHECK(f.note(ids[0]).at("pitch") == 49);
	CHECK(f.note(ids[1]).at("pitch") == 64);
}

TEST_CASE("with the on-screen keyboard open, Delete and Backspace still delete the selected notes", "[ui]")
{
	// The on-screen keyboard takes the keys of the key commands, except the reference's exceptions, Delete among them
	// (p. 332).
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 64, g_quarter}});
	f.app.ui().key(f.window, "Alt+K");
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().keyboard() != nullptr; }));
	f.click(of(noteSelector(ids[0])));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{ids[0]}; }));
	f.app.ui().key(f.window, "Delete");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).is_null(); }));
	f.click(of(noteSelector(ids[1])));
	REQUIRE(f.app.waitUntil([&] { return f.selection() == std::vector<uint64_t>{ids[1]}; }));
	f.app.ui().key(f.window, "Backspace");
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).is_null(); }));
	CHECK(f.app.appUi().keyboard() != nullptr);
}

TEST_CASE("in the lower zone Up, Down, Delete, Mod+X and Mod+C don't edit the project window's selection", "[ui]")
{
	// The keys are bound in the project window's focus zone too: Delete to edit.delete, Up and Down to
	// track.selectPrevious and track.selectNext. The note display takes them first (p. 1214). Mod+X and Mod+C, edit.cut
	// and edit.copy, take the project window's selected events only while it has the focus (#1910).
	Fixture f({1024, 768});
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	ui::EventDisplay& display = f.app.appUi().showProject();
	f.window = f.app.main().id();
	const uint64_t other = f.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const Json added = f.app.call("note.add",
		{{"part", f.part},
			{"notes",
				Json::array({{{"position", 0}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 0.5}},
					{{"position", g_quarter}, {"length", g_quarter}, {"pitch", 64}, {"velocity", 0.5}}})}});
	const uint64_t first = added.at("notes").at(0).at("id").get<uint64_t>();
	f.app.call("editor.open", {{"editor", "lower"}, {"parts", Json::array({f.part})}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note").size() == 2; }));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			ui::MidiEditor* editor = f.app.appUi().lowerZoneEditor();
			return editor != nullptr && editor->keyEditor() != nullptr && editor->keyEditor()->scrollToPitch(60);
		}));
	f.app.shell().step(std::chrono::milliseconds(0));
	const auto selected = [&f] { return f.app.call("editor.get", {{"editor", "lower"}}).at("selection").size(); };
	f.click(of(noteSelector(first)));
	REQUIRE(f.app.waitUntil([&] { return selected() == 1; }));
	// The part and the other track selected in the project window.
	display.select({f.part});
	f.app.appUi().trackList()->select({other});
	f.app.shell().step(std::chrono::milliseconds(0));

	f.app.ui().key(f.window, "Up");
	REQUIRE(f.app.waitUntil([&] { return f.note(first).at("pitch") == 61; }));
	f.settle();
	CHECK(f.app.appUi().trackList()->selection() == std::vector<uint64_t>{other});

	f.app.ui().key(f.window, "Delete");
	REQUIRE(f.app.waitUntil([&] { return f.note(first).is_null(); }));
	f.settle();
	CHECK(f.partNotes().size() == 1);
	CHECK(display.selection() == std::vector<uint64_t>{f.part});

	// The part being edited stays, and nothing is copied.
	const auto parts = [&f] { return f.app.call("project.get").at("midiParts").size(); };
	const size_t before = parts();
	f.app.ui().key(f.window, "Mod+X");
	f.app.ui().key(f.window, "Mod+C");
	f.settle();
	CHECK(parts() == before);
	CHECK(f.partNotes().size() == 1);
	CHECK(display.selection() == std::vector<uint64_t>{f.part});
	CHECK(!display.canRun("edit.paste"));
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

TEST_CASE("Show Note Expression Data shows the notes' expression over them", "[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, 2 * g_quarter}, {2 * g_quarter, 62, g_quarter}});
	const auto point = [](const char* type, int64_t position, double value)
	{ return Json{{"type", type}, {"position", position}, {"value", value}}; };
	f.app.call("note.set",
		{{"part", f.part},
			{"notes",
				Json::array({{{"id", ids[0]},
								 {"expressions",
									 Json::array({point("tuning", 0, 0.0), point("tuning", g_quarter, 24.0),
										 point("pressure", g_quarter, 1.0)})}},
					{{"id", ids[1]},
						{"expressions", Json::array({point("volume", 0, 3.0), point("tuning", 0, 60.0)})}}})}});
	f.settle();
	const std::string steps = noteSelector(ids[0]) + " .key-expression-step";
	CHECK(f.elements(steps).empty()); // off by default (reference manual p. 1303)
	f.click(of(".key-input[setting=showExpression]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(steps).size() == 3; }));
	CHECK(f.view().at("showExpression") == true);
	CHECK(f.has(".key-input[setting=showExpression]", "checked"));
	// Each point is a line from it to the next one at its value: tuning in MPE's range of 48 semitones each way, the
	// others 0 to 1, from the note's bottom to its top.
	const Json note = f.box(noteSelector(ids[0]));
	const auto near = [](const Json& value, double expected)
	{ return std::abs(value.get<double>() - expected) <= 2.0; };
	const double x = note.at("x").get<double>();
	const double y = note.at("y").get<double>();
	const double width = note.at("width").get<double>();
	const double height = note.at("height").get<double>();
	const Json shown = f.elements(steps);
	CHECK(near(shown.at(0).at("box").at("x"), x));
	CHECK(near(shown.at(0).at("box").at("width"), width / 2));
	CHECK(near(shown.at(0).at("box").at("y"), y + (height / 2)));
	CHECK(near(shown.at(1).at("box").at("x"), x + (width / 2)));
	CHECK(near(shown.at(1).at("box").at("y"), y + (height / 4)));
	CHECK(near(shown.at(2).at("box").at("x"), x + (width / 2))); // pressure
	CHECK(near(shown.at(2).at("box").at("y"), y));
	// Volume goes from 0 to 4, and tuning beyond 48 semitones shows at the edge.
	const Json second = f.box(noteSelector(ids[1]));
	const Json other = f.elements(noteSelector(ids[1]) + " .key-expression-step");
	REQUIRE(other.size() == 2);
	CHECK(near(other.at(0).at("box").at("y"),
		second.at("y").get<double>() + (second.at("height").get<double>() / 4))); // volume 3
	CHECK(near(other.at(1).at("box").at("y"), second.at("y").get<double>()));     // tuning 60
	f.click(of(".key-input[setting=showExpression]"));
	REQUIRE(f.app.waitUntil([&] { return f.elements(steps).empty(); }));
}

TEST_CASE(
	"a double click opens the note expression editor, where the draw tool draws points and the erase tool deletes "
	"them",
	"[ui]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, 2 * g_quarter}, {3 * g_quarter, 64, g_quarter}});
	f.settle();
	f.click(of(".key-input[setting=expressionDoubleClick]"));
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-input[setting=expressionDoubleClick]", "checked"); }));
	CHECK(f.has(".key-expression-editor", "off"));
	// With Double-Click Opens Note Expression Editor a double click opens it on the note instead of deleting it
	// (reference manual p. 1308), over the note's time, with tuning chosen.
	f.click(of(noteSelector(ids[0])), 2);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-expression-editor", "off"); }));
	CHECK(f.partNotes().size() == 2);
	CHECK(f.selection() == std::vector<uint64_t>{ids[0]});
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json editor = f.box(".key-expression-editor");
	const Json note = f.box(noteSelector(ids[0]));
	CHECK(std::abs(editor.at("x").get<double>() - note.at("x").get<double>()) <= 1.5);
	CHECK(std::abs(editor.at("width").get<double>() - note.at("width").get<double>()) <= 1.5);
	CHECK(f.elements(".key-expression-type").at(0).at("text") == "Tuning");

	// Parameter Selection chooses the expression (p. 1309).
	f.click(of(".key-expression-type"));
	f.click(of("#key-expression-menu > menuitem[expression-type=pressure]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("expressionType") == "pressure"; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-type").at(0).at("text") == "Pressure"; }));

	// A point in the editor's area: x and y in parts of its width and height from the top left.
	const auto area = [&f](double x, double y)
	{
		const Json box = f.box(".key-expression-area");
		return ui::Target{"", 0,
			ui::Point{static_cast<int>(std::lround(box.at("x").get<double>() + (x * box.at("width").get<double>()))),
				static_cast<int>(std::lround(box.at("y").get<double>() + (y * box.at("height").get<double>())))}};
	};
	const auto points = [&f, &ids]
	{
		const Json found = f.note(ids[0]);
		return found.is_null() ? Json::array() : found.value("expressions", Json::array());
	};
	// The draw tool draws a point where it clicks (p. 1311), its value from the area's height.
	f.tool("draw");
	f.click(area(0.25, 0.25));
	REQUIRE(f.app.waitUntil([&] { return points().size() == 1; }));
	CHECK(points().at(0).at("type") == "pressure");
	CHECK(std::abs(points().at(0).at("position").get<double>() - (g_quarter / 2.0)) <= g_sixteenth / 4.0);
	CHECK(std::abs(points().at(0).at("value").get<double>() - 0.75) <= 0.05);
	// The editor shows it once the note has it.
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-area .key-expression-step").size() == 1; }));
	// A click a few pixels to the right, on the line drawn from that point, draws one there too.
	// After the frame a line that took mouse events would be the next press's target; the lines take none.
	f.app.shell().step(std::chrono::milliseconds(0));
	const ui::Point first = area(0.25, 0.25).point.value_or(ui::Point{});
	f.click(ui::Target{"", 0, ui::Point{first.x + 6, first.y}});
	REQUIRE(f.app.waitUntil([&] { return points().size() == 2; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-area .key-expression-step").size() == 2; }));
	// A drag draws the points along the way, replacing those there, and stays with the expression editor.
	f.drag(area(0.5, 0.9), area(0.75, 0.1));
	REQUIRE(f.app.waitUntil([&] { return points().size() >= 4; }));
	CHECK(std::abs(points().at(2).at("value").get<double>() - 0.1) <= 0.05);
	CHECK(std::abs(points().back().at("value").get<double>() - 0.9) <= 0.05);
	CHECK(!f.has(".key-expression-editor", "off"));
	// Once the editor shows the drag, and then the undo: it reads the project one read at a time, so that the step
	// counts below can't be met by an older read, and the erase below goes to the points the note has (#1784).
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-area .key-expression-step").size() >= 4; }));
	// One undo step brings back the points before the drag.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return points().size() == 2; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-area .key-expression-step").size() == 2; }));
	f.app.call("history.redo");
	REQUIRE(f.app.waitUntil([&] { return points().size() >= 4; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-area .key-expression-step").size() >= 4; }));
	// The erase tool deletes the points it is dragged over, of the chosen expression only.
	f.app.call("note.set",
		{{"part", f.part},
			{"notes",
				Json::array({{{"id", ids[0]},
					{"expressions",
						Json::array({{{"type", "tuning"}, {"position", 0}, {"value", 2.0}},
							{{"type", "pressure"}, {"position", g_quarter / 2}, {"value", 0.5}},
							{{"type", "pressure"}, {"position", g_quarter}, {"value", 0.5}}})}}})}});
	REQUIRE(f.app.waitUntil([&] { return points().size() == 3; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-area .key-expression-step").size() == 2; }));
	f.tool("erase");
	f.drag(area(0.2, 0.5), area(0.95, 0.5));
	REQUIRE(f.app.waitUntil([&] { return points().size() == 1; }));
	CHECK(points().at(0).at("type") == "tuning");

	// A click in the note display outside it closes it and changes nothing else.
	f.click(f.at(12.5, 70));
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-expression-editor", "off"); }));
	CHECK(f.selection() == std::vector<uint64_t>{ids[0]});
	CHECK(f.partNotes().size() == 2);
	// Without the setting a double click deletes the note as before.
	f.click(of(".key-input[setting=expressionDoubleClick]"));
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-input[setting=expressionDoubleClick]", "checked"); }));
	f.tool("objectSelection");
	f.click(of(noteSelector(ids[1])), 2);
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).is_null(); }));
	CHECK(f.has(".key-expression-editor", "off"));
}

TEST_CASE("the expression editor opens on the double-clicked note and edits the selected notes of two parts at their "
		  "times, in one undo step",
	"[ui]")
{
	Fixture f;
	// A second track's part over the same bar, open in the editor with the first one.
	const uint64_t other = f.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const uint64_t otherPart =
		f.app.call("midiPart.add", {{"track", other}, {"position", 0}, {"length", g_bar}, {"name", "Other"}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	const auto add = [&f](uint64_t part, int64_t position, int pitch)
	{
		return f.app
			.call("note.add",
				{{"part", part},
					{"notes",
						Json::array(
							{{{"position", position}, {"length", g_quarter}, {"pitch", pitch}, {"velocity", 0.5}}})}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t first = add(f.part, 0, 60);
	const uint64_t middle = add(otherPart, g_quarter, 56);
	const uint64_t last = add(f.part, 2 * g_quarter, 64);
	f.app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part, otherPart})}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note").size() == 3; }));
	f.scroll(60);
	const auto selector = [](uint64_t part, uint64_t id)
	{ return ".key-note[part=\"" + std::to_string(part) + "\"][note=\"" + std::to_string(id) + "\"]"; };
	// Laid out: each note a quarter of the bar wide.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json shown = f.elements(".key-note");
			const double quarter = f.box(".key-editor-grid").at("width").get<double>() / 4;
			return std::all_of(shown.begin(), shown.end(),
				[&](const Json& note) { return std::abs(note.at("box").at("width").get<double>() - quarter) <= 1.5; });
		}));
	const auto points = [&f](uint64_t part, uint64_t id)
	{
		const Json project = f.app.call("project.get");
		for (const Json& each : project.at("midiParts"))
			if (each.at("id") == part)
				for (const Json& note : each.at("notes"))
					if (note.at("id") == id)
						return note.value("expressions", Json::array());
		return Json::array();
	};
	f.app.call("editor.set",
		{{"editor", "window"}, {"expressionDoubleClick", true},
			{"selection",
				Json::array({{{"part", f.part}, {"note", first}}, {{"part", otherPart}, {"note", middle}},
					{{"part", f.part}, {"note", last}}})}});
	REQUIRE(f.app.waitUntil([&] { return f.selection().size() == 3; }));
	// Once the editor has the view too: before, the double click would select or delete the note it is on.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return f.elements(".key-note.selected").size() == 3 &&
				f.has(".key-input[setting=expressionDoubleClick]", "checked");
		}));
	f.app.shell().step(std::chrono::milliseconds(0));

	// A double click on a selected note opens the editor on all of them (reference manual p. 1308): from the first
	// one's start to the last one's end, centered on the note clicked.
	f.click(of(selector(f.part, last)), 2);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-expression-editor", "off"); }));
	CHECK(f.selection().size() == 3);
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json editor = f.box(".key-expression-editor");
	const Json grid = f.box(".key-editor-grid");
	const Json clicked = f.box(selector(f.part, last));
	CHECK(std::abs(editor.at("x").get<double>() - grid.at("x").get<double>()) <= 1.5);
	CHECK(std::abs(editor.at("width").get<double>() - (grid.at("width").get<double>() * 3 / 4)) <= 1.5);
	CHECK(std::abs(editor.at("y").get<double>() + (editor.at("height").get<double>() / 2) -
			  (clicked.at("y").get<double>() + (clicked.at("height").get<double>() / 2))) <= 1.5);

	const auto area = [&f](double x, double y)
	{
		const Json box = f.box(".key-expression-area");
		return ui::Target{"", 0,
			ui::Point{static_cast<int>(std::lround(box.at("x").get<double>() + (x * box.at("width").get<double>()))),
				static_cast<int>(std::lround(box.at("y").get<double>() + (y * box.at("height").get<double>())))}};
	};
	const auto near = [](const Json& value, double expected, double tolerance)
	{ return std::abs(value.get<double>() - expected) <= tolerance; };
	// A click reaches the note at its time only, at its position from that note's start.
	f.tool("draw");
	f.click(area(0.5, 0.25));
	REQUIRE(f.app.waitUntil([&] { return points(otherPart, middle).size() == 1; }));
	CHECK(near(points(otherPart, middle).at(0).at("position"), g_quarter / 2.0, g_sixteenth / 4.0));
	CHECK(near(points(otherPart, middle).at(0).at("value"), 24.0, 2.0));
	CHECK(points(f.part, first).empty());
	CHECK(points(f.part, last).empty());
	// A drag over all three at one height gives each note one point: the mouse's other positions change no value.
	f.drag(area(0.02, 0.75), area(0.98, 0.75));
	REQUIRE(f.app.waitUntil([&] { return points(f.part, last).size() == 1; }));
	for (const auto& [part, id] : {std::pair{f.part, first}, std::pair{otherPart, middle}, std::pair{f.part, last}})
	{
		REQUIRE(points(part, id).size() == 1);
		CHECK(near(points(part, id).at(0).at("value"), -24.0, 2.0));
	}
	// The editor reads the project after each change, one read at a time. Until it shows the drag, it may still show
	// the read from before it, whose one point looks like the one the undo leaves; the drag's read could then come
	// right before the erase below, which would find no point at the mouse and delete none (#1784).
	const std::string steps = ".key-expression-area .key-expression-step";
	REQUIRE(f.app.waitUntil([&] { return f.elements(steps).size() == 3; }));
	// The notes of both parts change in one undo step.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return points(f.part, last).empty(); }));
	CHECK(points(f.part, first).empty());
	REQUIRE(points(otherPart, middle).size() == 1);
	CHECK(near(points(otherPart, middle).at(0).at("value"), 24.0, 2.0));
	REQUIRE(f.app.waitUntil([&] { return f.elements(steps).size() == 1; }));

	// The erase tool's click deletes the point within a few dp of it.
	f.tool("erase");
	f.click(area(0.5, 0.5));
	REQUIRE(f.app.waitUntil([&] { return points(otherPart, middle).empty(); }));
	// Once the editor shows it: until then its copy of the note has the point, which the draw below would send back.
	REQUIRE(f.app.waitUntil([&] { return f.elements(steps).empty(); }));
	// Volume goes from 0 to 4.
	f.app.call("editor.set", {{"editor", "window"}, {"expressionType", "volume"}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-type").at(0).at("text") == "Volume"; }));
	f.tool("draw");
	f.click(area(0.5, 0.25));
	REQUIRE(f.app.waitUntil([&] { return points(otherPart, middle).size() == 1; }));
	CHECK(points(otherPart, middle).at(0).at("type") == "volume");
	CHECK(near(points(otherPart, middle).at(0).at("value"), 3.0, 0.1));
	// With Shift tuning goes in semitone steps (Vertical Snap, p. 1309).
	f.app.call("editor.set", {{"editor", "window"}, {"expressionType", "tuning"}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-expression-type").at(0).at("text") == "Tuning"; }));
	f.click(area(0.75, 0.3), 1, {"shift"});
	REQUIRE(f.app.waitUntil([&] { return points(f.part, last).size() == 1; }));
	const double snapped = points(f.part, last).at(0).at("value").get<double>();
	CHECK(snapped == std::round(snapped));
	CHECK(std::abs(snapped - 19.0) <= 2.0);

	// A selection without notes closes it.
	f.app.call("editor.set", {{"editor", "window"}, {"selection", Json::array()}});
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-expression-editor", "off"); }));
}

TEST_CASE("the expression editor stays in the note display, is never narrower than its button, and its draw ends "
		  "where the button goes up outside the window",
	"[ui]")
{
	Fixture f;
	// A short note on the top row.
	const std::vector<uint64_t> ids = f.add({{0, 127, g_sixteenth / 2}});
	f.settle();
	f.app.call("editor.set", {{"editor", "window"}, {"expressionDoubleClick", true}});
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-input[setting=expressionDoubleClick]", "checked"); }));
	f.click(of(noteSelector(ids[0])), 2);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-expression-editor", "off"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json editor = f.box(".key-expression-editor");
	const Json grid = f.box(".key-editor-grid");
	CHECK(editor.at("y").get<double>() >= grid.at("y").get<double>() - 0.5);
	CHECK(editor.at("y").get<double>() + editor.at("height").get<double>() <=
		grid.at("y").get<double>() + grid.at("height").get<double>() + 0.5);
	CHECK(editor.at("width").get<double>() >= f.box(".key-expression-type").at("width").get<double>());
	CHECK(editor.at("width").get<double>() > f.box(noteSelector(ids[0])).at("width").get<double>());

	// A draw released outside the window, where no mouseup arrives, ends at the drag's end, with what it drew.
	const auto points = [&f, &ids]
	{
		const Json found = f.note(ids[0]);
		return found.is_null() ? Json::array() : found.value("expressions", Json::array());
	};
	f.tool("draw");
	const Json area = f.box(".key-expression-area");
	f.app.ui().press(f.window,
		ui::Target{"", 0,
			ui::Point{static_cast<int>(std::lround(area.at("x").get<double>() + (area.at("width").get<double>() / 2))),
				static_cast<int>(std::lround(area.at("y").get<double>() + (area.at("height").get<double>() / 2)))}},
		ui::MouseButton::Left, {});
	f.app.shell().step(std::chrono::milliseconds(0));
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Context& context = window->context();
	context.ProcessMouseMove(-50, -50, 0);
	context.ProcessMouseLeave();
	context.ProcessMouseButtonUp(0, 0);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return !points().empty(); }));
	const Json drawn = points();
	// The mouse back over the editor with the button up draws nothing, and a click outside it only closes it.
	f.app.ui().move(f.window, f.at(1, 127), {});
	f.app.shell().step(std::chrono::milliseconds(0));
	f.click(f.at(12.5, 127));
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-expression-editor", "off"); }));
	f.settle();
	CHECK(points() == drawn);
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return points().empty(); }));
}

TEST_CASE("the expression editor on a short note at the end of the bottom row stays in the note display and draws at "
		  "the note's last tick at its right edge",
	"[ui]")
{
	Fixture f;
	const int64_t length = g_sixteenth; // on the quantize grid, which a click on a note snaps it to
	const std::vector<uint64_t> ids = f.add({{g_bar - length, 0, length}});
	f.settle();
	f.app.call("editor.set", {{"editor", "window"}, {"expressionDoubleClick", true}});
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-input[setting=expressionDoubleClick]", "checked"); }));
	f.click(of(noteSelector(ids[0])), 2);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-expression-editor", "off"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	// As wide as its button, which is wider than the note: it ends at the note display's right edge, not past it, and
	// stays above its bottom row.
	const Json editor = f.box(".key-expression-editor");
	const Json grid = f.box(".key-editor-grid");
	CHECK(editor.at("width").get<double>() > f.box(noteSelector(ids[0])).at("width").get<double>());
	CHECK(editor.at("x").get<double>() + editor.at("width").get<double>() <=
		grid.at("x").get<double>() + grid.at("width").get<double>() + 0.5);
	CHECK(editor.at("y").get<double>() + editor.at("height").get<double>() <=
		grid.at("y").get<double>() + grid.at("height").get<double>() + 0.5);

	// A draw that ends at the right edge leaves its last point at the note's last tick, where it still plays.
	f.tool("draw");
	const Json area = f.box(".key-expression-area");
	const auto point = [&area](double x, double y)
	{
		return ui::Target{"", 0,
			ui::Point{static_cast<int>(std::ceil(area.at("x").get<double>() + (x * area.at("width").get<double>()))),
				static_cast<int>(std::lround(area.at("y").get<double>() + (y * area.at("height").get<double>())))}};
	};
	f.drag(point(0.5, 0.75), point(1.0, 0.25));
	REQUIRE(f.app.waitUntil([&] { return !f.note(ids[0]).value("expressions", Json::array()).empty(); }));
	const Json points = f.note(ids[0]).at("expressions");
	CHECK(points.back().at("position") == length - 1);
	CHECK(std::abs(points.back().at("value").get<double>() - 24.0) <= 2.0);
}

TEST_CASE("the expression editor on a short note right of the note display's middle starts at the note", "[ui]")
{
	// Wider than the note, it grows to the left from the note's end only where it would end past the note display's
	// right edge: anchored at the middle, an in-place editor's would jump by its overhang as the project window scrolls
	// the note across it (#1746).
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{9 * g_sixteenth, 60, g_sixteenth / 2}});
	f.settle();
	f.app.call("editor.set", {{"editor", "window"}, {"expressionDoubleClick", true}});
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-input[setting=expressionDoubleClick]", "checked"); }));
	f.click(of(noteSelector(ids[0])), 2);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-expression-editor", "off"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json editor = f.box(".key-expression-editor");
	const Json note = f.box(noteSelector(ids[0]));
	CHECK(editor.at("width").get<double>() > note.at("width").get<double>() + 10);
	CHECK(std::abs(editor.at("x").get<double>() - note.at("x").get<double>()) <= 1.0);
}

TEST_CASE("the expression editor on a short note just inside the note display's right edge stays in it as the window "
		  "narrows",
	"[ui]")
{
	// Its button's width is in dp and the note's place in percent: a seventh narrower, the note starts nearer the right
	// edge than the editor is wide, and the window's update places it again, although a pixel with a sixth more ticks
	// renders nothing again (#1746).
	Fixture f;
	const ui::WindowId window = f.app.appUi().editorWindowId();
	const ui::Window* shown = f.app.shell().find(window);
	REQUIRE(shown != nullptr);
	f.app.call("quantize.set", {{"settings", {{"grid", "1/128"}}}}); // which the click on the note snaps it to
	f.settle();
	const double grid = f.box(".key-editor-grid").at("width").get<double>();
	// The first 128th at least 73dp from the right edge, less than 84: the editor's 72dp fit there, a seventh narrower
	// they don't.
	const double fromRight = 73.0 * shown->context().GetDensityIndependentPixelRatio();
	const auto steps = static_cast<int64_t>(std::ceil(fromRight / grid * 128));
	REQUIRE(static_cast<double>(steps) * grid / 128 < fromRight * 84 / 73);
	const std::vector<uint64_t> ids = f.add({{g_bar - (steps * g_bar / 128), 60, g_sixteenth / 2}});
	f.settle();
	f.app.call("editor.set", {{"editor", "window"}, {"expressionDoubleClick", true}});
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-input[setting=expressionDoubleClick]", "checked"); }));
	f.click(of(noteSelector(ids[0])), 2);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-expression-editor", "off"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(std::abs(f.box(".key-expression-editor").at("x").get<double>() -
				f.box(noteSelector(ids[0])).at("x").get<double>()) <= 1.0);

	const ui::Rect bounds = f.app.shell().bounds(window);
	const uint64_t frames = shown->frames();
	f.app.shell().setBounds(window, {bounds.x, bounds.y, bounds.width - f.desktop(grid / 7), bounds.height});
	REQUIRE(f.app.waitUntil(
		[&] { return shown->frames() > frames && f.box(".key-editor-grid").at("width").get<double>() < grid * 0.9; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	const Json editor = f.box(".key-expression-editor");
	const Json narrowed = f.box(".key-editor-grid");
	CHECK(editor.at("x").get<double>() + editor.at("width").get<double>() <=
		narrowed.at("x").get<double>() + narrowed.at("width").get<double>() + 0.5);
}

TEST_CASE("the note expression lines join the steps narrower than a pixel", "[ui]")
{
	// A recorded MPE note has a point every few milliseconds (#1671): the overlay and the expression editor draw a
	// line per pixel at most, not one per point.
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, 2 * g_quarter}});
	constexpr int count = 2000;
	f.app.call("note.set",
		{{"part", f.part},
			{"notes", Json::array({{{"id", ids[0]}, {"expressions", densePoints(count, 2 * g_quarter)}}})}});
	f.app.call("editor.set", {{"editor", "window"}, {"showExpression", true}, {"expressionDoubleClick", true}});
	const std::string steps = noteSelector(ids[0]) + " .key-expression-step";
	REQUIRE(f.app.waitUntil([&] { return !f.elements(steps).empty(); }));
	f.settle();
	const double width = f.box(noteSelector(ids[0])).at("width").get<double>();
	REQUIRE(width < count / 2.0);
	// The lines for that width, once the window has laid them out.
	const auto lines = [&f, &steps] { return static_cast<double>(f.elements(steps).size()); };
	CHECK(f.app.waitUntil([&] { return lines() <= width + 2; }));
	CHECK(lines() >= width / 2);
	f.click(of(noteSelector(ids[0])), 2);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-expression-editor", "off"); }));
	const std::string editorSteps = ".key-expression-area .key-expression-step";
	double area = 0.0;
	CHECK(f.app.waitUntil(
		[&]
		{
			area = f.box(".key-expression-area").at("width").get<double>();
			const Json shown = f.elements(editorSteps);
			return !shown.empty() && static_cast<double>(shown.size()) <= area + 2;
		}));
	INFO("area " << area << ", lines " << f.elements(editorSteps).size());
	CHECK(static_cast<double>(f.elements(editorSteps).size()) <= area + 2);
}

TEST_CASE(
	"the key editor window joins a dense note's expression steps by its pixels after its layout and a resize", "[ui]")
{
	// The window renders its notes before its first layout, when a pixel has no ticks yet: opened on a recorded MPE
	// note with Show Note Expression Data already on, and resized, it builds the lines again for its width (#1671).
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, 2 * g_quarter}});
	constexpr int count = 2000;
	f.app.call("note.set",
		{{"part", f.part},
			{"notes", Json::array({{{"id", ids[0]}, {"expressions", densePoints(count, 2 * g_quarter)}}})}});
	f.app.call("editor.set", {{"editor", "window"}, {"showExpression", true}});
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part})}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() != nullptr; }));
	f.window = f.app.appUi().editorWindowId();
	const std::string steps = noteSelector(ids[0]) + " .key-expression-step";
	REQUIRE(f.app.waitUntil([&] { return !f.elements(steps).empty(); }));
	const auto noteWidth = [&f, &ids] { return f.box(noteSelector(ids[0])).at("width").get<double>(); };
	const double before = f.linesFit(ids[0]);
	CHECK(before < count / 4.0);
	const ui::WindowId window = f.app.appUi().editorWindowId();
	const ui::Rect bounds = f.app.shell().bounds(window);
	// A pixel with a fifth more ticks keeps the lines the window has laid out: they are built again only when it has
	// a quarter more or fewer (#1716), not on every frame of a scroll across a tempo change in the project window's
	// in-place editor. The note is half the note display wide.
	const size_t lines = f.elements(steps).size();
	const int narrower = bounds.width - f.desktop(before * 2 / 6);
	const ui::Window* shown = f.app.shell().find(window);
	REQUIRE(shown != nullptr);
	const uint64_t frames = shown->frames();
	f.app.shell().setBounds(window, {bounds.x, bounds.y, narrower, bounds.height});
	// Drawn narrower: the window's update has fitted the lines to that layout.
	REQUIRE(f.app.waitUntil([&] { return shown->frames() > frames && noteWidth() < before * 0.88; }));
	REQUIRE(noteWidth() > before * 0.8);
	CHECK(f.elements(steps).size() == lines);
	f.app.shell().setBounds(window, {bounds.x, bounds.y, bounds.width / 2, bounds.height});
	REQUIRE(f.app.waitUntil([&] { return noteWidth() < before * 0.8; }));
	f.linesFit(ids[0]);
}

TEST_CASE("the key editor window builds a dense note's expression lines that show for the width of its next layout, "
		  "also when it is less than a quarter narrower than the one before",
	"[ui]")
{
	// The editor window is laid out first by Docking::attach, before the left zone has its size: the notes are about
	// a tenth wider then. Lines built for that box stayed, more lines than pixels, as the window's own layout gives a
	// pixel less than a quarter more ticks (#1768). Lines the window has not laid out yet are built again for any
	// other width.
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, 2 * g_quarter}});
	f.app.call("note.set",
		{{"part", f.part},
			{"notes", Json::array({{{"id", ids[0]}, {"expressions", densePoints(2000, 2 * g_quarter)}}})}});
	f.settle();
	const auto noteWidth = [&f, &ids] { return f.box(noteSelector(ids[0])).at("width").get<double>(); };
	const double before = noteWidth();
	// A sixth narrower, a fifth more ticks per pixel. The note is half the note display wide.
	const ui::WindowId window = f.app.appUi().editorWindowId();
	const ui::Rect bounds = f.app.shell().bounds(window);
	f.app.shell().setBounds(window, {bounds.x, bounds.y, bounds.width - f.desktop(before * 2 / 6), bounds.height});
	SECTION("Show Note Expression Data once the window is laid out")
	{
		REQUIRE(f.app.waitUntil([&] { return noteWidth() < before * 0.88; }));
		f.app.call("editor.set", {{"editor", "window"}, {"showExpression", true}});
	}
	SECTION("Show Note Expression Data before, on the box of the layout before")
	{
		f.app.call("editor.set", {{"editor", "window"}, {"showExpression", true}});
		ui::MidiEditor* editor = f.app.appUi().editorWindow();
		REQUIRE(editor != nullptr);
		ui::KeyEditor* keys = editor->keyEditor();
		REQUIRE(keys != nullptr);
		// As the engine's answers right after Docking::attach: no layout since the window narrowed.
		keys->show(f.app.call("project.get"), f.view());
	}
	const double width = f.linesFit(ids[0]);
	CHECK(width < before * 0.88);
	CHECK(width > before * 0.8);
}

TEST_CASE("the lower zone's key editor joins a dense note's expression steps by its pixels after its layout and as the "
		  "main window narrows",
	"[ui]")
{
	// The app's update task fits the lines of the lower zone's key editor too, which renders before its layout (#1724).
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, 2 * g_quarter}});
	constexpr int count = 4000;
	f.app.call("note.set",
		{{"part", f.part},
			{"notes", Json::array({{{"id", ids[0]}, {"expressions", densePoints(count, 2 * g_quarter)}}})}});
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.appUi().showProject();
	f.app.appUi().docking().setSize("project", "lower", 400.0);
	f.window = f.app.main().id();
	f.app.call("editor.open", {{"editor", "lower"}, {"parts", Json::array({f.part})}});
	f.app.call("editor.set", {{"editor", "lower"}, {"showExpression", true}});
	const std::string steps = noteSelector(ids[0]) + " .key-expression-step";
	REQUIRE(f.app.waitUntil([&] { return !f.elements(steps).empty(); }));
	const double before = f.linesFit(ids[0]);
	CHECK(before < count / 4.0);
	const ui::Rect bounds = f.app.shell().bounds(f.app.main().id());
	f.app.shell().setBounds(f.app.main().id(), {bounds.x, bounds.y, bounds.width / 2, bounds.height});
	REQUIRE(f.app.waitUntil([&] { return f.box(noteSelector(ids[0])).at("width").get<double>() < before * 0.8; }));
	f.linesFit(ids[0]);
}

TEST_CASE("the expression editor stays centered on its note as the pitches zoom", "[ui]")
{
	Fixture f({1024, 600});
	f.app.setDpRatio(1.0F);
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}});
	f.scroll(60);
	f.app.call("editor.set", {{"editor", "window"}, {"expressionDoubleClick", true}});
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-input[setting=expressionDoubleClick]", "checked"); }));
	f.click(of(noteSelector(ids[0])), 2);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".key-expression-editor", "off"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	const auto centered = [&f, &ids]
	{
		const Json editor = f.box(".key-expression-editor");
		const Json note = f.box(noteSelector(ids[0]));
		return std::abs(editor.at("y").get<double>() + (editor.at("height").get<double>() / 2) -
				   (note.at("y").get<double>() + (note.at("height").get<double>() / 2))) <= 1.5;
	};
	CHECK(centered());
	const Json keyboard = f.box(".key-editor-keys");
	const int left = keyboard.at("x").get<int>() + 4; // the keyboard's left 12 dp, where a drag zooms (p. 1261)
	const int row = f.box(".key-editor-body").at("y").get<int>() + 150;
	const auto at = [](int x, int y) { return ui::Target{"", 0, ui::Point{x, y}}; };
	// From 1 to 2, to 4 and back to 1: the editor keeps its height and stays on the note's row.
	for (const auto& [dx, height] : {std::pair{100, 20.0}, std::pair{100, 40.0}, std::pair{-200, 10.0}})
	{
		f.drag(at(left, row), at(left + dx, row));
		f.app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(std::abs(f.box(".key-editor-row").at("height").get<double>() - height) <= 0.5);
		CHECK(!f.has(".key-expression-editor", "off"));
		CHECK(centered());
	}
}

TEST_CASE("the lower zone's key editor at its default size gives the note display more room than the controller lanes, "
		  "at dp ratios 1 and 1.5",
	"[ui]")
{
	// The lower zone at its default 240 dp, with the Velocity lane and the footer below the note display: the lanes
	// give way first, down to their footer, and their list scrolls (#1775). The window is 1024 x 768 pixels, as large
	// as CI's Linux screen lets it be: at a dp ratio of 1 the toolbar is two rows, the note display the larger part of
	// the editor, and in a lower zone of 400 dp the lanes keep their whole height; at 1.5, 683 x 512 dp, the toolbar
	// wraps to three rows and leaves less than the note display's 40% and the lanes, so the lanes are down to their
	// footer. At 2, 512 x 384 dp, it wraps to five rows and leaves the editor no room for the footer.
	for (const float ratio : {1.0F, 1.5F})
	{
		INFO("dp ratio " << ratio);
		const auto px = [ratio](double dp) { return dp * ratio; };
		Fixture f({1024, 768});
		f.app.call("editor.close", {{"editor", "window"}});
		REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
		f.app.setDpRatio(ratio);
		f.app.appUi().showProject();
		f.window = f.app.main().id();
		f.app.call("editor.open", {{"editor", "lower"}, {"parts", Json::array({f.part})}});
		REQUIRE(f.app.waitUntil([&] { return f.elements("#lower-zone .controller-lane").size() == 1; }));
		const auto box = [&f](const std::string& selector) { return f.box("#lower-zone " + selector); };
		const auto height = [&box](const std::string& selector) { return box(selector).at("height").get<double>(); };
		const auto bottom = [&box](const std::string& selector)
		{
			const Json found = box(selector);
			return found.at("y").get<double>() + found.at("height").get<double>();
		};
		const auto settle = [&f]
		{
			f.app.shell().step(std::chrono::milliseconds(0));
			f.app.shell().step(std::chrono::milliseconds(0));
		};
		settle();
		const double editor = height(".key-editor");
		const double body = height(".key-editor-body");
		const double lanes = height(".controller-lanes");
		const double room =
			editor - height(".key-editor-toolbar") - height(".key-editor-info") - height(".editor-parts");
		INFO("editor " << editor << ", note display " << body << ", controller lanes " << lanes << ", room " << room);
		// The note display has its 40%, or the lanes are down to their footer and it has the rest.
		CHECK(std::abs(body + lanes - room) <= 1.0);
		CHECK(((body >= (0.4 * editor) - 1.0) || (lanes <= px(22) + 0.5)));
		// The footer stays whole inside the editor.
		CHECK(height(".controller-lanes-footer") >= px(22) - 0.5);
		CHECK(bottom(".controller-lanes-footer") <= bottom(".key-editor") + 0.5);
		if (ratio > 1.0F)
		{
			CHECK(lanes <= px(22) + 0.5);
			continue;
		}
		CHECK(body >= editor / 3);
		CHECK(body > lanes);

		f.app.appUi().docking().setSize("project", "lower", 400.0);
		REQUIRE(f.app.waitUntil([&] { return height(".key-editor") > editor + px(100); }));
		settle();
		CHECK(height(".controller-lane") >= px(64) - 0.5);
		CHECK(height(".controller-lane-list") >= px(64) - 0.5);
		CHECK(bottom(".controller-lanes-footer") <= bottom(".key-editor") + 0.5);
	}
}

TEST_CASE("the editor window's key editor and the in-place editor keep their controller lanes' height", "[ui]")
{
	// The lanes give way only in a low editor (#1775): in the editor window of 1200 x 800 dp the note display has its
	// 40% and more, and the lane its 64 dp. The in-place editor keeps the lane's height also with its toolbar and info
	// line shown, where the note display gets what is left.
	Fixture f;
	f.app.setDpRatio(1.0F);
	REQUIRE(f.app.waitUntil([&] { return f.elements(".controller-lane").size() == 1; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.shell().step(std::chrono::milliseconds(0));
	const auto height = [&f](const std::string& selector) { return f.box(selector).at("height").get<double>(); };
	CHECK(height(".key-editor-body") >= (0.4 * height(".key-editor")) - 0.5);
	CHECK(height(".controller-lane") >= 63.5);

	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.appUi().showProject();
	f.window = f.app.main().id();
	f.app.call("editor.inPlace", {{"tracks", {f.track}}, {"open", true}});
	const std::string editor = ".in-place-editor[track=\"" + std::to_string(f.track) + "\"]";
	f.app.call("editor.set", {{"editor", "inPlace:" + std::to_string(f.track)}, {"lanes", {{{"type", "velocity"}}}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(editor + " .controller-lane").size() == 1; }));
	const std::string triangle =
		".track-list listitem[value=\"" + std::to_string(f.track) + "\"] .in-place-toolbar-toggle";
	REQUIRE(f.app.waitUntil([&] { return f.elements(triangle).at(0).at("visible") == true; }));
	f.click(of(triangle));
	REQUIRE(f.app.waitUntil([&] { return f.has(editor, "toolbar-shown"); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(height(editor + " .controller-lane") >= 63.5);
	CHECK(height(editor + " .controller-lane-list") >= 63.5);
}

TEST_CASE("the event colors menu colors the notes by their part's color, its own or else its track's", "[ui]")
{
	// The Event Colors menu's Part (reference manual pp. 1133-1134): notes in the color of their part, which is its
	// own or its track's; selected notes keep the style sheets' look, and Velocity gives them their velocities' again.
	Fixture f;
	const std::vector<uint64_t> first = f.add({{0, 60, g_quarter}, {g_quarter, 62, g_quarter}});
	f.app.call("track.set", {{"track", f.track}, {"color", "#2040e0"}});
	const auto other = f.app.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
						   .at("part")
						   .at("id")
						   .get<uint64_t>();
	const uint64_t own =
		f.app
			.call("note.add",
				{{"part", other},
					{"notes", {{{"position", 0}, {"length", g_quarter}, {"pitch", 64}, {"velocity", 0.5}}}}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	f.app.call("event.set", {{"events", {other}}, {"color", "#c04020"}});
	f.app.call("editor.open", {{"editor", "window"}, {"parts", {f.part, other}}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".key-note").size() == 3; }));
	const auto color = [&](uint64_t note)
	{
		const Json found = f.elements(noteSelector(note));
		return found.empty() ? std::string("none") : found.at(0).at("attributes").value("color", std::string());
	};
	CHECK(f.view().at("eventColors") == "velocity");

	f.click(of("#key-event-colors"));
	f.click(of("#key-event-colors-menu menuitem[event-colors=part]"));
	REQUIRE(f.app.waitUntil([&] { return color(own) == "#c04020"; }));
	CHECK(f.view().at("eventColors") == "part");
	CHECK(color(first[0]) == "#2040e0");
	CHECK(color(first[1]) == "#2040e0");
	CHECK(f.elements("#key-event-colors-menu menuitem[event-colors=part][checked]").size() == 1);
	ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
	REQUIRE(window != nullptr);
	Rml::Element* shown = nullptr;
	for (int i = 0; i < window->context().GetNumDocuments() && shown == nullptr; ++i)
		shown = window->context().GetDocument(i)->QuerySelector(noteSelector(own));
	REQUIRE(shown != nullptr);
	REQUIRE(shown->GetLocalProperty("background-color") != nullptr);
	CHECK(shown->GetLocalProperty("background-color")->Get<Rml::Colourb>() == Rml::Colourb(0xc0, 0x40, 0x20, 255));

	// A selected note shows as selected, without the color; the others keep theirs.
	f.app.call("editor.set", {{"editor", "window"}, {"selection", {{{"part", f.part}, {"note", first[0]}}}}});
	REQUIRE(f.app.waitUntil([&] { return color(first[0]).empty(); }));
	CHECK(color(first[1]) == "#2040e0");

	f.click(of("#key-event-colors"));
	f.click(of("#key-event-colors-menu menuitem[event-colors=velocity]"));
	const Json velocity = f.app.call("settings.get", {{"key", "ui.eventColors"}}).at("value").at("velocity");
	REQUIRE(f.app.waitUntil([&] { return color(own) == velocity.at(4); })); // velocity 64
	CHECK(f.view().at("eventColors") == "velocity");
}

TEST_CASE("the drum editor and the in-place editor color notes through their event colors menu", "[ui]")
{
	// The drum editor and the in-place editor have the Event Colors menu too (reference manual pp. 1133-1134), the
	// in-place editor's with its suffix. Notes of a part that isn't editable, as with Active Part, keep the style
	// sheets' look.
	Fixture f;
	const std::vector<uint64_t> first = f.add({{0, 60, g_quarter}});
	f.app.call("track.set", {{"track", f.track}, {"color", "#2040e0"}});
	const auto other = f.app.call("midiPart.add", {{"track", f.track}, {"position", g_bar}, {"length", g_bar}})
						   .at("part")
						   .at("id")
						   .get<uint64_t>();
	const uint64_t own =
		f.app
			.call("note.add",
				{{"part", other},
					{"notes", {{{"position", 0}, {"length", g_quarter}, {"pitch", 64}, {"velocity", 0.5}}}}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	f.app.call("event.set", {{"events", {other}}, {"color", "#c04020"}});
	const auto color = [&](const std::string& scope, uint64_t note)
	{
		const Json found = f.elements(scope + noteSelector(note));
		return found.empty() ? std::string("none") : found.at(0).at("attributes").value("color", std::string());
	};

	// The drum editor.
	f.app.call("editor.open", {{"editor", "window"}, {"parts", {f.part, other}}});
	f.app.call("editor.set", {{"editor", "window"}, {"content", "drum"}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 2; }));
	f.click(of("#key-event-colors"));
	f.click(of("#key-event-colors-menu menuitem[event-colors=part]"));
	REQUIRE(f.app.waitUntil([&] { return color("", own) == "#c04020"; }));
	CHECK(f.view().at("eventColors") == "part");
	CHECK(color("", first[0]) == "#2040e0");

	// The in-place editor, showing the active part only: the other part's notes keep the style sheets' look.
	f.app.call("editor.close", {{"editor", "window"}});
	REQUIRE(f.app.waitUntil([&] { return f.app.appUi().editorWindow() == nullptr; }));
	f.app.appUi().showProject();
	f.window = f.app.main().id();
	const std::string view = "inPlace:" + std::to_string(f.track);
	const std::string editor = ".in-place-editor[track=\"" + std::to_string(f.track) + "\"] ";
	f.app.call("editor.inPlace", {{"tracks", {f.track}}, {"open", true}});
	f.app.call("editor.set", {{"editor", view}, {"editingMode", "activePart"}, {"activePart", other}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(editor + ".key-note").size() == 2; }));
	const std::string triangle =
		".track-list listitem[value=\"" + std::to_string(f.track) + "\"] .in-place-toolbar-toggle";
	REQUIRE(f.app.waitUntil([&] { return f.elements(triangle).at(0).at("visible") == true; }));
	f.click(of(triangle));
	REQUIRE(f.app.waitUntil([&] { return f.elements(editor + "#key-event-colors").at(0).at("visible") == true; }));
	f.click(of(editor + "#key-event-colors"));
	f.click(of("#key-event-colors-menu-inPlace-" + std::to_string(f.track) + " menuitem[event-colors=part]"));
	REQUIRE(f.app.waitUntil([&] { return color(editor, own) == "#c04020"; }));
	CHECK(f.app.call("editor.get", {{"editor", view}}).at("eventColors") == "part");
	CHECK(color(editor, first[0]).empty());

	// Activating the first part gives its notes their track's color and takes the other part's away.
	f.app.call("editor.set", {{"editor", view}, {"activePart", f.part}});
	REQUIRE(f.app.waitUntil([&] { return color(editor, first[0]) == "#2040e0"; }));
	CHECK(color(editor, own).empty());
}

TEST_CASE("the event colors menu colors notes by velocity, pitch or channel as their setup dialogs set", "[ui]")
{
	// Velocity, the scheme at first, Pitch and Channel (reference manual pp. 1133-1136): each note in the color of its
	// velocity's range, its pitch class or its channel (ui.eventColors). Setup opens the scheme's dialog of colors,
	// whose OK keeps the picker's changes and Cancel drops them; Part and Sound Slot have no Setup. There is no
	// Default.
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {g_quarter, 61, g_quarter}});
	f.app.call("note.set", {{"part", f.part}, {"notes", {{{"id", ids[1]}, {"velocity", 1.0}, {"channel", 2}}}}});
	const Json colors = f.app.call("settings.get", {{"key", "ui.eventColors"}}).at("value");
	const auto color = [&](uint64_t note)
	{
		const Json found = f.elements(noteSelector(note));
		return found.empty() ? std::string("none") : found.at(0).at("attributes").value("color", std::string());
	};
	const auto choose = [&](const std::string& item)
	{
		f.click(of("#key-event-colors"));
		f.click(of("#key-event-colors-menu menuitem" + item));
	};
	REQUIRE(f.app.waitUntil([&] { return color(ids[1]) == colors.at("velocity").at(7); }));
	CHECK(color(ids[0]) == colors.at("velocity").at(4));
	CHECK(f.elements("#key-event-colors-menu menuitem[event-colors=default]").empty());
	choose("[event-colors=pitch]");
	REQUIRE(f.app.waitUntil([&] { return color(ids[1]) == colors.at("pitch").at(1); }));
	CHECK(color(ids[0]) == colors.at("pitch").at(0));
	choose("[event-colors=channel]");
	REQUIRE(f.app.waitUntil([&] { return color(ids[1]) == colors.at("channel").at(2); }));
	CHECK(color(ids[0]) == colors.at("channel").at(0));

	// Channel's setup: a color per channel; the picker recolors the selected one, and OK keeps it.
	const auto open = [&f] { return !f.elements("#event-colors-dialog").empty(); };
	const auto recolor = [&f](const std::string& value)
	{
		Rml::Element* picker = nullptr;
		const ui::Window* window = f.app.shell().find(f.app.appUi().editorWindowId());
		for (int i = 0; window != nullptr && i < window->context().GetNumDocuments() && picker == nullptr; ++i)
			picker = window->context().GetDocument(i)->QuerySelector("#event-colors-picker");
		REQUIRE(picker != nullptr);
		picker->SetAttribute("value", value);
		Rml::Dictionary parameters;
		parameters["value"] = Rml::String(value);
		picker->DispatchEvent(Rml::EventId::Change, parameters);
	};
	choose("[event-colors-setup]");
	REQUIRE(f.app.waitUntil(open));
	CHECK(f.elements("#event-colors-dialog .dialog-titlebar").at(0).value("text", "") == "Channel Colors Setup");
	REQUIRE(f.elements("#event-colors-list > listitem").size() == 16);
	f.click(of("#event-colors-list > listitem[value=\"3\"]"));
	recolor("#123456");
	f.click(of("#event-colors-ok"));
	REQUIRE(f.app.waitUntil([&] { return !open(); }));
	REQUIRE(f.app.waitUntil([&] { return color(ids[1]) == "#123456"; }));
	Json changed = colors;
	changed["channel"][2] = "#123456";
	CHECK(f.app.call("settings.get", {{"key", "ui.eventColors"}}).at("value") == changed);

	// Pitch's setup lists the 12 pitch classes; Cancel drops the change.
	choose("[event-colors=pitch]");
	choose("[event-colors-setup]");
	REQUIRE(f.app.waitUntil(open));
	REQUIRE(f.elements("#event-colors-list > listitem").size() == 12);
	recolor("#654321");
	f.click(of("#event-colors-cancel"));
	REQUIRE(f.app.waitUntil([&] { return !open(); }));
	CHECK(f.app.call("settings.get", {{"key", "ui.eventColors"}}).at("value") == changed);
	CHECK(color(ids[0]) == colors.at("pitch").at(0));

	// Part has no setup.
	choose("[event-colors=part]");
	REQUIRE(f.app.waitUntil([&] { return f.view().at("eventColors") == "part"; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements("#key-event-colors-menu menuitem[event-colors-setup][disabled]").size() == 1);
	// Nor has Sound Slot; back on Velocity, Setup is enabled again.
	choose("[event-colors=soundSlot]");
	REQUIRE(f.app.waitUntil([&] { return f.view().at("eventColors") == "soundSlot"; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements("#key-event-colors-menu menuitem[event-colors-setup][disabled]").size() == 1);
	choose("[event-colors=velocity]");
	REQUIRE(f.app.waitUntil([&] { return f.view().at("eventColors") == "velocity"; }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.elements("#key-event-colors-menu menuitem[event-colors-setup][disabled]").empty());
	CHECK(f.elements("#key-event-colors-menu menuitem[event-colors-setup]").size() == 1);
}

TEST_CASE("Indicate Transpositions shows the notes at the pitches they play", "[ui]")
{
	// Indicate Transpositions in the toolbar (#1766, reference manual pp. 419, 1260): a note after a transpose event
	// of 3 semitones shows 3 rows up; a drawn note gets the pitch that plays at the row under the mouse, and a move
	// changes the pitch by the rows dragged.
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 60, g_quarter}, {2 * g_quarter, 60, g_quarter}});
	f.app.call("transpose.add", {{"position", 2 * g_quarter}, {"value", 3}});
	f.settle(); // off, the notes stay on their own rows
	f.scroll(63);
	CHECK(f.on(ids[0], 0.5, 60));
	CHECK(f.on(ids[1], 8.5, 60));
	const auto on = [&f](uint64_t id, double sixteenths, int pitch) { return f.on(id, sixteenths, pitch); };
	f.click(of(".key-input[setting=indicateTranspositions]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("indicateTranspositions") == true; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(".key-input[setting=indicateTranspositions]", "checked"); }));
	f.scroll(63);
	CHECK(f.app.waitUntil([&] { return on(ids[0], 0.5, 60) && on(ids[1], 8.5, 63); }));

	f.tool("draw");
	f.click(f.at(12.5, 66));
	REQUIRE(f.app.waitUntil([&] { return f.partNotes().size() == 3; }));
	CHECK(f.partNotes()[2].at("pitch") == 63);
	const auto drawn = f.partNotes()[2].at("id").get<uint64_t>();
	CHECK(f.app.waitUntil([&] { return on(drawn, 12.5, 66); }));

	f.tool("objectSelection");
	f.drag(f.at(8.5, 63), f.at(8.5, 64));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[1]).at("pitch") == 61; }));
	CHECK(f.app.waitUntil([&] { return on(ids[1], 8.5, 64); }));

	// Off, every note is on its own row again.
	f.click(of(".key-input[setting=indicateTranspositions]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("indicateTranspositions") == false; }));
	f.settle();
	CHECK(on(ids[0], 0.5, 60));
	CHECK(on(ids[1], 8.5, 61));
	CHECK(on(drawn, 12.5, 63));
}

TEST_CASE("with Indicate Transpositions a note moved across a transpose event shows and plays where it lands", "[ui]")
{
	// A note dragged to a time after a transpose event of 3 semitones plays 3 semitones up there: the preview shows it
	// on that row and Acoustic Feedback plays that pitch, as after the release, while its pitch stays (#1766).
	Fixture f({1200, 800}, "instrument");
	f.probe();
	const std::vector<uint64_t> ids = f.add({{0, 60, g_sixteenth}});
	f.app.call("transpose.add", {{"position", 2 * g_quarter}, {"value", 3}});
	f.app.call("editor.set", {{"editor", "window"}, {"indicateTranspositions", true}, {"acousticFeedback", true}});
	REQUIRE(f.app.waitUntil([&] { return f.view().at("indicateTranspositions") == true; }));
	f.scroll(63);
	REQUIRE(f.app.waitUntil([&] { return f.on(ids[0], 0.5, 60); }));

	const double before = f.box(noteSelector(ids[0])).at("x").get<double>();
	f.app.ui().press(f.window, f.at(0.5, 60), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.plays({{0x90, 60, 100}}));
	f.app.ui().move(f.window, f.at(4.5, 60));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.box(noteSelector(ids[0])).at("x").get<double>() > before + 1.5; }));
	CHECK(f.on(ids[0], 4.5, 60));
	f.app.ui().move(f.window, f.at(8.5, 60));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.app.waitUntil([&] { return f.on(ids[0], 8.5, 63); }));
	CHECK(f.plays({{0x90, 60, 100}, {0x80, 60, 0}, {0x90, 63, 100}}));

	f.app.ui().release(f.window, f.at(8.5, 60), ui::MouseButton::Left);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.app.waitUntil([&] { return f.note(ids[0]).at("position") == 2 * g_quarter; }));
	CHECK(f.note(ids[0]).at("pitch") == 60);
	CHECK(f.app.waitUntil([&] { return f.on(ids[0], 8.5, 63); }));
	CHECK(f.plays({{0x90, 60, 100}, {0x80, 60, 0}, {0x90, 63, 100}, {0x80, 63, 0}}));
}

TEST_CASE("the drum editor shows the notes on their own rows with Indicate Transpositions on", "[ui]")
{
	// The view keeps Indicate Transpositions when the content changes to the drum editor, which has no toggle for it
	// and shows every note on its sound's row (#1766).
	Fixture f;
	const std::vector<uint64_t> ids = f.add({{0, 38, g_sixteenth}, {2 * g_quarter, 38, g_sixteenth}});
	f.app.call("transpose.add", {{"position", 2 * g_quarter}, {"value", 3}});
	f.click(of(".key-input[setting=indicateTranspositions]"));
	REQUIRE(f.app.waitUntil([&] { return f.view().at("indicateTranspositions") == true; }));
	REQUIRE(f.app.waitUntil([&] { return f.on(ids[1], 8.5, 41); }));

	f.app.call("editor.set", {{"editor", "window"}, {"content", "drum"}});
	REQUIRE(f.app.waitUntil([&] { return f.elements(".drum-note").size() == 2; }));
	CHECK(f.view().at("indicateTranspositions") == true);
	CHECK_FALSE(f.view().at("transpositions").empty());
	CHECK(f.elements(".key-input[setting=indicateTranspositions]").empty());
	f.scroll(38);
	const auto onSound = [&](uint64_t id, int pitch)
	{
		const Json note = f.box(noteSelector(id));
		const Json row = f.box(".drum-sound[pitch=\"" + std::to_string(pitch) + "\"]");
		const double center = note.at("y").get<double>() + (note.at("height").get<double>() / 2.0);
		return row.at("y").get<double>() <= center &&
			center <= row.at("y").get<double>() + row.at("height").get<double>();
	};
	CHECK(f.app.waitUntil([&] { return onSound(ids[0], 38) && onSound(ids[1], 38); }));
}
