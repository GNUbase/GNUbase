// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI quantizing (#132). The reference cases follow the reference manual's descriptions of the quantize functions
// and the quantize panel: a grid moves notes to the closest position, strength moves them part of the way, swing
// offsets every second grid position, notes outside the catch range and inside the safe range stay put, and
// quantizing starts from the original positions unless it is soft (iterative).
#include "engine/engine.h"
#include "engine/midi/quantize.h"
#include "engine/midi/quantizecommands.h"
#include "engine/settings/store.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using namespace gnubase::engine;
using namespace gnubase::engine::midi;
using Catch::Matchers::ContainsSubstring;
using time::Ticks;

namespace
{
	constexpr Ticks g_quarter = time::g_ticksPerQuarter;
	constexpr Ticks g_sixteenth = g_quarter / 4;

	// A fraction of a quarter note, in ticks; exact for the fractions the cases use.
	constexpr Ticks quarters(int64_t numerator, int64_t denominator = 1)
	{
		return g_quarter * numerator / denominator;
	}

	// A part from the project's start, long enough for the notes of every case.
	constexpr PartSpan g_part{0, quarters(64)};

	QuantizeSettings gridOf(const std::string& value)
	{
		QuantizeSettings settings;
		const std::optional<NoteValue> grid = noteValueFromString(value);
		REQUIRE(grid.has_value());
		settings.grid = grid.value_or(NoteValue{});
		return settings;
	}

	Note noteAt(Ticks position, Ticks length = g_sixteenth, ObjectId id = 1)
	{
		Note note;
		note.id = id;
		note.position = position;
		note.length = length;
		note.velocity = 40000;
		return note;
	}

	// A 4/4 groove of one bar with points at the given positions.
	Groove grooveAt(const std::vector<Ticks>& positions)
	{
		Groove groove;
		groove.length = quarters(4);
		for (const Ticks position : positions)
			groove.points.push_back({position, std::nullopt, std::nullopt});
		return groove;
	}

	struct StartCase
	{
		const char* what;
		QuantizeSettings settings;
		Ticks before;
		Ticks after;
	};

	QuantizeSettings with(QuantizeSettings settings, void (*change)(QuantizeSettings&))
	{
		change(settings);
		return settings;
	}

	std::vector<StartCase> startCases()
	{
		return {
			{"a 1/16 grid moves a late note back", gridOf("1/16"), g_sixteenth * 3 / 10, 0},
			{"a 1/16 grid moves an early note forward", gridOf("1/16"), g_sixteenth * 7 / 10, g_sixteenth},
			{"a note halfway between grid positions goes to the later one", gridOf("1/16"), g_sixteenth / 2,
				g_sixteenth},
			{"a 1/4 grid", gridOf("1/4"), quarters(14, 10), quarters(1)},
			{"a 1/8 triplet grid", gridOf("1/8T"), quarters(3, 10), quarters(1, 3)},
			{"a dotted 1/4 grid starts again at the bar", gridOf("1/4."), quarters(41, 10), quarters(4)},
			{"a dotted 1/4 grid within the bar", gridOf("1/4."), quarters(34, 10), quarters(3)},
			{"quintuplets divide four quarters into five",
				with(gridOf("1/4"), [](QuantizeSettings& s) { s.tuplet = 5; }), quarters(85, 100), quarters(4, 5)},
			{"swing 50% offsets every second grid position by a sixth of a step",
				with(gridOf("1/8"), [](QuantizeSettings& s) { s.swing = 0.5; }), quarters(1, 2), quarters(7, 12)},
			{"swing leaves the other grid positions", with(gridOf("1/8"), [](QuantizeSettings& s) { s.swing = 0.5; }),
				quarters(1) + 1000, quarters(1)},
			{"swing 100% is a triplet feel", with(gridOf("1/8"), [](QuantizeSettings& s) { s.swing = 1.0; }),
				quarters(6, 10), quarters(2, 3)},
			{"swing doesn't apply to triplet grids", with(gridOf("1/8T"), [](QuantizeSettings& s) { s.swing = 1.0; }),
				quarters(1, 3), quarters(1, 3)},
			{"strength 50% moves halfway", with(gridOf("1/16"), [](QuantizeSettings& s) { s.strength = 0.5; }),
				g_sixteenth * 4 / 10, g_sixteenth * 2 / 10},
			{"strength 0% doesn't move", with(gridOf("1/16"), [](QuantizeSettings& s) { s.strength = 0.0; }),
				g_sixteenth * 4 / 10, g_sixteenth * 4 / 10},
			{"notes outside the catch range stay put",
				with(gridOf("1/16"), [](QuantizeSettings& s) { s.catchRange = g_sixteenth / 4; }), g_sixteenth * 3 / 10,
				g_sixteenth * 3 / 10},
			{"notes inside the catch range move",
				with(gridOf("1/16"), [](QuantizeSettings& s) { s.catchRange = g_sixteenth / 4; }), g_sixteenth * 2 / 10,
				0},
			{"notes inside the safe range stay put",
				with(gridOf("1/16"), [](QuantizeSettings& s) { s.safeRange = g_sixteenth / 10; }),
				g_sixteenth * 5 / 100, g_sixteenth * 5 / 100},
			{"notes outside the safe range move",
				with(gridOf("1/16"), [](QuantizeSettings& s) { s.safeRange = g_sixteenth / 10; }), g_sixteenth * 2 / 10,
				0},
			{"a groove moves notes to its positions",
				with({}, [](QuantizeSettings& s) { s.groove = grooveAt({0, quarters(11, 10), quarters(2)}); }),
				quarters(1), quarters(11, 10)},
			{"a groove repeats",
				with({}, [](QuantizeSettings& s) { s.groove = grooveAt({0, quarters(11, 10), quarters(2)}); }),
				quarters(5), quarters(51, 10)},
			{"a groove starts at its origin",
				with({},
					[](QuantizeSettings& s)
					{
						s.groove = grooveAt({0, quarters(11, 10), quarters(2)});
						s.origin = quarters(1, 10);
					}),
				quarters(12, 10), quarters(12, 10)},
			{"max. move limits how far a groove moves notes",
				with({},
					[](QuantizeSettings& s)
					{
						s.groove = grooveAt({0, quarters(11, 10), quarters(2)});
						s.maxMove = quarters(5, 100);
					}),
				quarters(1), quarters(105, 100)},
			{"without pre-quantizing, the nearest groove position",
				with({},
					[](QuantizeSettings& s) { s.groove = grooveAt({0, quarters(1), quarters(16, 10), quarters(3)}); }),
				quarters(125, 100), quarters(1)},
			{"pre-quantizing moves notes to a grid before the groove",
				with({},
					[](QuantizeSettings& s)
					{
						s.groove = grooveAt({0, quarters(1), quarters(16, 10), quarters(3)});
						s.preQuantize = NoteValue{2, NoteValueKind::Straight};
					}),
				quarters(125, 100), quarters(16, 10)},
		};
	}
}

TEST_CASE("quantizing note starts matches the reference cases", "[unit]")
{
	for (const StartCase& c : startCases())
	{
		INFO(c.what);
		Note note = noteAt(c.before);
		const QuantizeGrid grid(c.settings, time::SignatureMap{});
		CHECK(quantizeStart(note, g_part, grid) == (c.after != c.before));
		CHECK(note.position == c.after);
		CHECK(note.length == g_sixteenth); // the note lengths are maintained
	}
}

TEST_CASE("the grid follows time signature changes", "[unit]")
{
	// A 3/4 bar, then 4/4: a 1/2 grid has positions at 0, 2 and 3 quarters, then 5 and 7.
	const time::SignatureMap signatures({{0, {3, 4}}, {1, {4, 4}}});
	const QuantizeGrid grid(gridOf("1/2"), signatures);
	CHECK(grid.nearest(quarters(26, 10)).position == quarters(3));
	CHECK(grid.nearest(quarters(24, 10)).position == quarters(2));
	CHECK(grid.nearest(quarters(42, 10)).position == quarters(5));
	CHECK(grid.next(quarters(2)) == quarters(3));
	CHECK(grid.next(quarters(3)) == quarters(5));
}

TEST_CASE("notes don't move before their part's start", "[unit]")
{
	Note note = noteAt(100);
	// The part starts 1000 ticks after a grid position: the note would move 1100 ticks back.
	CHECK_FALSE(quantizeStart(note, PartSpan{1000, quarters(64)}, QuantizeGrid(gridOf("1/16"), time::SignatureMap{})));
	CHECK(note.position == 100);
	// From 300 ticks before the next grid position, it moves within the part.
	Note late = noteAt(g_sixteenth - 1300);
	CHECK(quantizeStart(late, PartSpan{1000, quarters(64)}, QuantizeGrid(gridOf("1/16"), time::SignatureMap{})));
	CHECK(late.position == g_sixteenth - 1000);
}

TEST_CASE("notes don't move to or past their part's end", "[unit]")
{
	// A one-bar part: a note at 3.8 quarters would move to the next bar's downbeat, where it no longer plays.
	const PartSpan bar{0, quarters(4)};
	Note note = noteAt(quarters(38, 10));
	CHECK(!quantizeStart(note, bar, QuantizeGrid(gridOf("1/4"), time::SignatureMap{})));
	CHECK(note.position == quarters(38, 10));
	CHECK(!note.origin.has_value());

	SECTION("randomizing doesn't take notes past the end either")
	{
		QuantizeSettings settings;
		settings.groove = grooveAt({0, quarters(4) - 5});
		settings.randomize = 1000;
		const QuantizeGrid grid(settings, time::SignatureMap{});
		size_t atLastTick = 0;
		for (ObjectId id = 1; id <= 50; ++id)
		{
			INFO(id);
			Note late = noteAt(quarters(39, 10), g_sixteenth, id);
			CHECK(quantizeStart(late, bar, grid));
			CHECK(late.position >= quarters(4) - 1005);
			CHECK(late.position < quarters(4));
			atLastTick += late.position == quarters(4) - 1 ? 1 : 0;
		}
		CHECK(atLastTick > 0);
	}
}

TEST_CASE("randomizing doesn't keep notes at the part's start from being quantized", "[unit]")
{
	QuantizeSettings settings = gridOf("1/16");
	settings.randomize = g_sixteenth / 8;
	const QuantizeGrid grid(settings, time::SignatureMap{});
	// Downbeats played 300 ticks late: the random offset would take about half of them before the part's start.
	for (ObjectId id = 1; id <= 50; ++id)
	{
		INFO(id);
		Note note = noteAt(300, g_sixteenth, id);
		CHECK(quantizeStart(note, g_part, grid));
		CHECK(note.position >= 0);
		CHECK(note.position <= settings.randomize);
		CHECK(note.origin.has_value());
	}
}

TEST_CASE("quantizing starts from the original positions unless it is soft", "[unit]")
{
	QuantizeSettings half = gridOf("1/16");
	half.strength = 0.5;
	const QuantizeGrid grid(half, time::SignatureMap{});

	SECTION("again from the original: the same result")
	{
		Note note = noteAt(g_sixteenth * 4 / 10);
		quantizeStart(note, g_part, grid);
		CHECK(note.position == g_sixteenth * 2 / 10);
		CHECK_FALSE(quantizeStart(note, g_part, grid));
		CHECK(note.position == g_sixteenth * 2 / 10);
	}
	SECTION("soft (iterative) quantizing moves closer every time")
	{
		half.soft = true;
		const QuantizeGrid soft(half, time::SignatureMap{});
		Note note = noteAt(g_sixteenth * 4 / 10);
		quantizeStart(note, g_part, soft);
		CHECK(note.position == g_sixteenth * 2 / 10);
		quantizeStart(note, g_part, soft);
		CHECK(note.position == g_sixteenth / 10);
		// Resetting goes back to before the first time.
		CHECK(resetQuantize(note));
		CHECK(note.position == g_sixteenth * 4 / 10);
	}
	SECTION("other settings start from the original, not from the last result")
	{
		Note note = noteAt(quarters(6, 10));
		quantizeStart(note, g_part, QuantizeGrid(gridOf("1/4"), time::SignatureMap{}));
		CHECK(note.position == quarters(1));
		quantizeStart(note, g_part, QuantizeGrid(gridOf("1/16"), time::SignatureMap{}));
		CHECK(note.position == quarters(1, 2));
	}
}

TEST_CASE("reset quantize puts the original timing back, freezing makes the quantized one permanent", "[unit]")
{
	const QuantizeGrid sixteenths(gridOf("1/16"), time::SignatureMap{});
	const QuantizeGrid quarterGrid(gridOf("1/4"), time::SignatureMap{});
	Note note = noteAt(quarters(6, 10), quarters(11, 10));
	const Note original = note;

	SECTION("reset after quantizing starts and ends")
	{
		quantizeStart(note, g_part, quarterGrid);
		quantizeEnd(note, g_part, quarterGrid);
		CHECK(note.position == quarters(1));
		CHECK(note.length == quarters(1)); // the original end at 1.7 quarters goes to 2
		REQUIRE(note.origin.has_value());
		const NoteTiming before = note.origin.value_or(QuantizeOrigin{}).original;
		CHECK(before.position == quarters(6, 10));
		CHECK(before.length == quarters(11, 10));
		CHECK(resetQuantize(note));
		CHECK(note == original);
		CHECK_FALSE(resetQuantize(note));
	}
	SECTION("freeze")
	{
		quantizeStart(note, g_part, quarterGrid);
		CHECK(freezeQuantize(note));
		CHECK_FALSE(note.origin.has_value());
		CHECK_FALSE(resetQuantize(note));
		CHECK(note.position == quarters(1));
		// Quantizing now starts from the frozen position.
		CHECK_FALSE(quantizeStart(note, g_part, sixteenths));
		CHECK(note.position == quarters(1));
	}
	SECTION("a note moved by hand after quantizing")
	{
		quantizeStart(note, g_part, quarterGrid);
		note.position = quarters(13, 10); // as if moved by another command
		CHECK_FALSE(resetQuantize(note));
		CHECK(note.position == quarters(13, 10));
		// Its new position is its original now.
		quantizeStart(note, g_part, sixteenths);
		CHECK(note.position == quarters(5, 4));
		REQUIRE(note.origin.has_value());
		CHECK(note.origin.value_or(QuantizeOrigin{}).original.position == quarters(13, 10));
	}
	SECTION("a velocity changed after quantizing stays; the note still starts from and resets to its original")
	{
		quantizeStart(note, g_part, quarterGrid);
		note.velocity = 1000; // as if changed by another command
		CHECK(quantizeStart(note, g_part, sixteenths));
		CHECK(note.position == quarters(1, 2)); // from the original 0.6 quarters, not from 1
		CHECK(note.velocity == 1000);
		CHECK(resetQuantize(note));
		CHECK(note.position == quarters(6, 10));
		CHECK(note.length == quarters(11, 10));
		CHECK(note.velocity == 1000);
		CHECK(!note.origin.has_value());
	}
	SECTION("a length changed after quantizing the start stays; resetting puts the start back")
	{
		quantizeStart(note, g_part, quarterGrid);
		note.length = quarters(2);
		CHECK(resetQuantize(note));
		CHECK(note.position == quarters(6, 10));
		CHECK(note.length == quarters(2));
	}
	SECTION("for the length functions, a length changed by hand is a move: they start from it")
	{
		quantizeStart(note, g_part, quarterGrid);
		quantizeEnd(note, g_part, quarterGrid); // the original end at 1.7 quarters goes to 2
		CHECK(note.length == quarters(1));
		note.length = quarters(16, 10); // the end at 2.6 quarters, by hand
		CHECK(quantizeEnd(note, g_part, quarterGrid));
		CHECK(note.length == quarters(2)); // 2.6 goes to 3, not the original end to 2
		REQUIRE(note.origin.has_value());
		CHECK(note.origin.value_or(QuantizeOrigin{}).original.length == quarters(16, 10));
	}
	SECTION("a note that quantizing leaves where it was has no origin")
	{
		Note onGrid = noteAt(quarters(1));
		CHECK_FALSE(quantizeStart(onGrid, g_part, quarterGrid));
		CHECK_FALSE(onGrid.origin.has_value());
	}
}

TEST_CASE("quantizing note ends and lengths", "[unit]")
{
	SECTION("ends move to the nearest grid position, starts stay")
	{
		Note note = noteAt(0, quarters(11, 10));
		CHECK(quantizeEnd(note, g_part, QuantizeGrid(gridOf("1/4"), time::SignatureMap{})));
		CHECK(note.position == 0);
		CHECK(note.length == quarters(1));
	}
	SECTION("an end that would come before the start goes to the grid position after it")
	{
		Note note = noteAt(quarters(105, 100), quarters(1, 10));
		quantizeEnd(note, g_part, QuantizeGrid(gridOf("1/4"), time::SignatureMap{}));
		CHECK(note.position == quarters(105, 100));
		CHECK(note.length == quarters(2) - quarters(105, 100));
	}
	SECTION("a fixed length")
	{
		Note note = noteAt(1234, quarters(3));
		CHECK(quantizeLength(note, g_part, QuantizeGrid(gridOf("1/16"), time::SignatureMap{}), quarters(1, 2)));
		CHECK(note.position == 1234);
		CHECK(note.length == quarters(1, 2));
	}
	SECTION("linked to the grid")
	{
		Note note = noteAt(100, quarters(3));
		quantizeLength(note, g_part, QuantizeGrid(gridOf("1/16"), time::SignatureMap{}), std::nullopt);
		CHECK(note.length == g_sixteenth);
	}
	SECTION("linked to a swing grid: long and short steps")
	{
		QuantizeSettings swing = gridOf("1/8");
		swing.swing = 1.0;
		const QuantizeGrid grid(swing, time::SignatureMap{});
		Note first = noteAt(0, quarters(3));
		Note second = noteAt(quarters(1, 2), quarters(3));
		quantizeLength(first, g_part, grid, std::nullopt);
		quantizeLength(second, g_part, grid, std::nullopt);
		CHECK(first.length == quarters(2, 3));
		CHECK(second.length == quarters(1, 3));
	}
	SECTION("linked lengths respect the catch range")
	{
		QuantizeSettings settings = gridOf("1/16");
		settings.catchRange = g_sixteenth / 4;
		Note note = noteAt(g_sixteenth * 3 / 10, quarters(3));
		CHECK_FALSE(quantizeLength(note, g_part, QuantizeGrid(settings, time::SignatureMap{}), std::nullopt));
		CHECK(note.length == quarters(3));
	}
	SECTION("resetting undoes length changes too")
	{
		Note note = noteAt(100, quarters(3));
		quantizeLength(note, g_part, QuantizeGrid(gridOf("1/16"), time::SignatureMap{}), quarters(1));
		CHECK(resetQuantize(note));
		CHECK(note.length == quarters(3));
	}
}

TEST_CASE("randomizing moves quantized notes within its range, the same way for the same seed", "[unit]")
{
	QuantizeSettings settings = gridOf("1/16");
	settings.randomize = g_sixteenth / 8;
	const auto quantizeAll = [](const QuantizeSettings& s)
	{
		const QuantizeGrid grid(s, time::SignatureMap{});
		std::vector<Ticks> positions;
		for (ObjectId id = 1; id <= 50; ++id)
		{
			Note note = noteAt(g_sixteenth * 9 / 10, g_sixteenth, id);
			quantizeStart(note, g_part, grid);
			positions.push_back(note.position);
		}
		return positions;
	};
	const std::vector<Ticks> first = quantizeAll(settings);
	for (const Ticks position : first)
	{
		CHECK(position >= g_sixteenth - settings.randomize);
		CHECK(position <= g_sixteenth + settings.randomize);
	}
	CHECK(std::set<Ticks>(first.begin(), first.end()).size() > 40);
	CHECK(quantizeAll(settings) == first);
	settings.seed = 7;
	CHECK(quantizeAll(settings) != first);

	SECTION("notes the catch range leaves alone aren't randomized")
	{
		settings.catchRange = g_sixteenth / 20;
		Note note = noteAt(g_sixteenth / 2);
		CHECK_FALSE(quantizeStart(note, g_part, QuantizeGrid(settings, time::SignatureMap{})));
	}
}

TEST_CASE("a groove moves velocities and lengths toward its own by their strengths", "[unit]")
{
	QuantizeSettings settings;
	Groove groove = grooveAt({0, quarters(1)});
	groove.points[1].velocity = 60000;
	groove.points[1].length = quarters(1, 2);
	settings.groove = groove;
	settings.velocityStrength = 0.5;
	settings.lengthStrength = 1.0;
	Note note = noteAt(quarters(9, 10), quarters(1));
	CHECK(quantizeStart(note, g_part, QuantizeGrid(settings, time::SignatureMap{})));
	CHECK(note.position == quarters(1));
	CHECK(note.velocity == 50000);
	CHECK(note.length == quarters(1, 2));
	CHECK(resetQuantize(note));
	CHECK(note.velocity == 40000);
	CHECK(note.length == quarters(1));
}

TEST_CASE("a groove's velocity and length don't stick when quantizing again", "[unit]")
{
	QuantizeSettings settings;
	Groove groove = grooveAt({0, quarters(1)});
	groove.points[1].velocity = 60000;
	groove.points[1].length = quarters(1, 2);
	settings.groove = groove;
	settings.velocityStrength = 1.0;
	settings.lengthStrength = 1.0;
	Note note = noteAt(quarters(9, 10), quarters(1));
	quantizeStart(note, g_part, QuantizeGrid(settings, time::SignatureMap{}));
	CHECK(note.velocity == 60000);
	CHECK(note.length == quarters(1, 2));

	SECTION("at 0%, the original velocity and length")
	{
		settings.velocityStrength = 0.0;
		settings.lengthStrength = 0.0;
		CHECK(quantizeStart(note, g_part, QuantizeGrid(settings, time::SignatureMap{})));
		CHECK(note.position == quarters(1));
		CHECK(note.velocity == 40000);
		CHECK(note.length == quarters(1));
	}
	SECTION("with a grid, the original velocity")
	{
		CHECK(quantizeStart(note, g_part, QuantizeGrid(gridOf("1/4"), time::SignatureMap{})));
		CHECK(note.position == quarters(1));
		CHECK(note.velocity == 40000);
	}
	SECTION("resetting puts back the velocity and length the groove changed")
	{
		CHECK(resetQuantize(note));
		CHECK(note.velocity == 40000);
		CHECK(note.length == quarters(1));
	}
}

TEST_CASE("a groove is extracted from a part's note starts", "[unit]")
{
	MidiPart part;
	part.length = quarters(4);
	const auto add = [&part](ObjectId id, Ticks position, Ticks length, uint16_t velocity)
	{
		Note note = noteAt(position, length, id);
		note.velocity = velocity;
		part.notes[id] = note;
	};
	add(1, quarters(11, 10), quarters(1, 4), 30000);
	add(2, 0, quarters(1, 2), 50000);
	add(3, quarters(11, 10), quarters(1, 2), 20000);
	add(4, quarters(4), quarters(1), 50000); // at the groove's end: left out
	const Groove groove = extractGroove(part, quarters(4));
	CHECK(groove.length == quarters(4));
	REQUIRE(groove.points.size() == 2);
	CHECK(groove.points[0].position == 0);
	CHECK(groove.points[0].velocity == 50000);
	CHECK(groove.points[1].position == quarters(11, 10));
	CHECK(groove.points[1].velocity == 30000);
	CHECK(groove.points[1].length == quarters(1, 2));
}

TEST_CASE("note values and quantize settings read and write JSON", "[unit]")
{
	CHECK(noteValueFromString("1/16") == NoteValue{16, NoteValueKind::Straight});
	CHECK(noteValueFromString("1/8T") == NoteValue{8, NoteValueKind::Triplet});
	CHECK(noteValueFromString("1/4.") == NoteValue{4, NoteValueKind::Dotted});
	CHECK(NoteValue{4, NoteValueKind::Dotted}.ticks() == quarters(3, 2));
	CHECK(NoteValue{128, NoteValueKind::Triplet}.ticks() * 3 * 32 == quarters(2));
	for (const char* invalid : {"1/3", "1/256", "16", "1/16x", "", "1/0", "2/16"})
	{
		INFO(invalid);
		CHECK_FALSE(noteValueFromString(invalid).has_value());
	}
	CHECK(toString(NoteValue{8, NoteValueKind::Triplet}) == "1/8T");

	QuantizeSettings settings = gridOf("1/8");
	settings.swing = 0.25;
	settings.catchRange = 100;
	settings.groove = grooveAt({0, quarters(1)});
	settings.groove->points[1].velocity = 1234;
	settings.preQuantize = NoteValue{16, NoteValueKind::Straight};
	const QuantizeSettings back = quantizeSettingsFromJson(toJson(settings));
	CHECK(toJson(back) == toJson(settings));
	REQUIRE(back.groove.has_value());
	CHECK(back.groove.value_or(Groove{}).points.at(1).velocity == 1234);

	const QuantizeSettings changed = quantizeSettingsFromJson({{"strength", 0.5}}, settings);
	CHECK(changed.strength == 0.5);
	CHECK(changed.grid == settings.grid);
	CHECK_FALSE(quantizeSettingsFromJson({{"groove", nullptr}}, settings).groove.has_value());

	for (const Json& invalid :
		{Json{{"grid", "1/3"}}, Json{{"strength", 1.5}}, Json{{"swing", -0.1}}, Json{{"tuplet", 2}},
			Json{{"catchRange", -1}}, Json{{"seed", -1}}, Json{{"groove", {{"length", 0}, {"points", Json::array()}}}},
			Json{{"groove", {{"length", 10}, {"points", {{{"position", 10}}}}}}}})
	{
		INFO(invalid.dump());
		CHECK_THROWS_AS(quantizeSettingsFromJson(invalid), std::invalid_argument);
	}
}

namespace
{
	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	struct Fixture
	{
		Engine engine;
		Client client{engine};
		ObjectId track = 0;

		// Without a settings folder, the user's presets are kept in memory only.
		explicit Fixture(const std::filesystem::path& settings = {}) : engine(gnubase::audio::makeBackends(), settings)
		{
			track = run(client, "track.add", {{"type", "instrument"}}).at("track").at("id").get<ObjectId>();
		}

		ObjectId addPart(Ticks position, Ticks length = quarters(4), ObjectId onTrack = 0)
		{
			return run(client, "midiPart.add",
				{{"track", onTrack == 0 ? track : onTrack}, {"position", position}, {"length", length}})
				.at("part")
				.at("id")
				.get<ObjectId>();
		}

		std::vector<ObjectId> addNotes(ObjectId part, const std::vector<Ticks>& positions)
		{
			Json notes = Json::array();
			for (const Ticks position : positions)
				notes.push_back({{"position", position}, {"length", g_sixteenth}, {"pitch", 60}, {"velocity", 0.5}});
			const Json added = run(client, "note.add", {{"part", part}, {"notes", notes}});
			std::vector<ObjectId> ids;
			for (const Json& note : added.at("notes"))
				ids.push_back(note.at("id").get<ObjectId>());
			return ids;
		}

		// The part's note positions, by note id.
		std::vector<Ticks> positions(ObjectId part)
		{
			const Json project = run(client, "project.get");
			std::vector<Ticks> result;
			for (const Json& p : project.at("midiParts"))
				if (p.at("id") == part)
					for (const Json& note : p.at("notes"))
						result.push_back(note.at("position").get<Ticks>());
			return result;
		}

		size_t historySteps() { return run(client, "history.get").at("steps").size(); }
	};
}

TEST_CASE("quantize.apply quantizes a part on the project's grid in one undo step", "[integration]")
{
	Fixture f;
	// The part starts half a sixteenth into the project, so the grid is half a sixteenth off from the part's start.
	const ObjectId part = f.addPart(g_sixteenth / 2);
	f.addNotes(part, {g_sixteenth * 3 / 10, g_sixteenth * 18 / 10});
	const size_t steps = f.historySteps();

	const Json result = run(f.client, "quantize.apply", {{"part", part}, {"settings", {{"grid", "1/16"}}}});
	CHECK(result.at("notes").size() == 2);
	CHECK(f.positions(part) == std::vector<Ticks>{g_sixteenth / 2, g_sixteenth * 3 / 2});
	CHECK(f.historySteps() == steps + 1);
	CHECK(run(f.client, "history.get").at("steps").back().at("action") == "Quantize");
	const Json note = run(f.client, "project.get").at("midiParts")[0].at("notes")[0];
	CHECK(note.at("origin").at("original").at("position") == g_sixteenth * 3 / 10);
	CHECK(note.at("origin").at("quantized").at("position") == g_sixteenth / 2);

	run(f.client, "history.undo");
	CHECK(f.positions(part) == std::vector<Ticks>{g_sixteenth * 3 / 10, g_sixteenth * 18 / 10});
	run(f.client, "history.redo");
	CHECK(f.positions(part) == std::vector<Ticks>{g_sixteenth / 2, g_sixteenth * 3 / 2});
}

TEST_CASE("quantize commands take a part's notes, several parts or the project's settings", "[integration]")
{
	Fixture f;
	const ObjectId first = f.addPart(0);
	const ObjectId second = f.addPart(quarters(4));
	const std::vector<ObjectId> notes = f.addNotes(first, {quarters(3, 10), quarters(13, 10)});
	f.addNotes(second, {quarters(1, 10)});

	SECTION("only the given notes")
	{
		run(f.client, "quantize.apply", {{"part", first}, {"notes", {notes[1]}}, {"settings", {{"grid", "1/4"}}}});
		CHECK(f.positions(first) == std::vector<Ticks>{quarters(3, 10), quarters(1)});
	}
	SECTION("several parts, with the project's settings")
	{
		run(f.client, "quantize.set", {{"settings", {{"grid", "1/4"}}}});
		run(f.client, "quantize.apply", {{"parts", {first, second}}});
		CHECK(f.positions(first) == std::vector<Ticks>{0, quarters(1)});
		CHECK(f.positions(second) == std::vector<Ticks>{0});
	}
	SECTION("a preset, with settings changed on top")
	{
		run(f.client, "quantize.apply", {{"part", first}, {"preset", "1/4"}, {"settings", {{"strength", 0.5}}}});
		CHECK(f.positions(first) == std::vector<Ticks>{quarters(3, 20), quarters(23, 20)});
	}
	SECTION("in a transaction")
	{
		const size_t steps = f.historySteps();
		run(f.client, "history.transaction",
			{{"description", "Quantize both"},
				{"commands",
					{{{"command", "quantize.apply"}, {"parameters", {{"part", first}}}},
						{{"command", "quantize.apply"}, {"parameters", {{"part", second}}}}}}});
		CHECK(f.historySteps() == steps + 1);
	}
	SECTION("ends, lengths, reset and freeze")
	{
		// The ends at 0.55 and 1.55 quarters go to 1 and 2; the starts stay, and then move with their lengths kept.
		run(f.client, "quantize.ends", {{"part", first}, {"settings", {{"grid", "1/4"}}}});
		run(f.client, "quantize.lengths", {{"part", second}, {"length", "1/2"}});
		run(f.client, "quantize.apply", {{"part", first}, {"settings", {{"grid", "1/4"}}}});
		Json project = run(f.client, "project.get");
		CHECK(project.at("midiParts")[0].at("notes")[0].at("length") == quarters(1) - quarters(3, 10));
		CHECK(project.at("midiParts")[1].at("notes")[0].at("length") == quarters(2));

		run(f.client, "quantize.freeze", {{"part", second}});
		const Json reset = run(f.client, "quantize.reset", {{"parts", {first, second}}});
		CHECK(reset.at("notes").size() == 2);
		project = run(f.client, "project.get");
		CHECK(f.positions(first) == std::vector<Ticks>{quarters(3, 10), quarters(13, 10)});
		CHECK(project.at("midiParts")[0].at("notes")[0].at("length") == g_sixteenth);
		CHECK(project.at("midiParts")[1].at("notes")[0].at("length") == quarters(2)); // frozen
	}
	SECTION("lengths in ticks")
	{
		run(f.client, "quantize.lengths", {{"part", second}, {"length", 1000}});
		CHECK(run(f.client, "project.get").at("midiParts")[1].at("notes")[0].at("length") == 1000);
	}
	SECTION("invalid targets and settings fail with a reason")
	{
		CHECK_THAT(reject(f.client, "quantize.apply", Json::object()), ContainsSubstring("part or parts"));
		CHECK_THAT(reject(f.client, "quantize.apply", {{"part", first}, {"parts", {second}}}),
			ContainsSubstring("part or parts"));
		CHECK_THAT(reject(f.client, "quantize.apply", {{"part", first}, {"notes", {notes[0] + 1000}}}),
			ContainsSubstring("no note"));
		CHECK_THAT(reject(f.client, "quantize.apply", {{"part", 9999}}), ContainsSubstring("no MIDI part"));
		CHECK_THAT(reject(f.client, "quantize.apply", {{"part", first}, {"settings", {{"grid", "1/3"}}}}),
			ContainsSubstring("grid"));
		CHECK_THAT(reject(f.client, "quantize.apply", {{"part", first}, {"preset", "nonsense"}}),
			ContainsSubstring("no quantize preset"));
		CHECK_THAT(reject(f.client, "quantize.lengths", {{"part", first}, {"length", 0}}), ContainsSubstring("length"));
		CHECK(f.positions(first) == std::vector<Ticks>{quarters(3, 10), quarters(13, 10)});
	}
}

TEST_CASE("a linear part quantizes on the project's grid", "[integration]")
{
	Fixture f;
	const ObjectId linear = run(f.client, "track.add", {{"type", "instrument"}, {"timeBase", "linear"}})
								.at("track")
								.at("id")
								.get<ObjectId>();
	// At 120 BPM and 48 kHz, 12000 samples are an eighth note.
	const ObjectId part = f.addPart(12000, 96000, linear);
	f.addNotes(part, {quarters(1, 10)});
	run(f.client, "quantize.apply", {{"part", part}, {"settings", {{"grid", "1/4"}}}});
	CHECK(f.positions(part) == std::vector<Ticks>{quarters(1, 2)});
}

TEST_CASE("quantize settings and auto quantize belong to the project, outside undo", "[integration]")
{
	Fixture f;
	const ObjectId part = f.addPart(0);
	f.addNotes(part, {quarters(3, 10)});
	const size_t steps = f.historySteps();

	const Json set = run(f.client, "quantize.set",
		{{"preset", "1/8 Triplet"}, {"settings", {{"strength", 0.75}}}, {"autoQuantize", true}});
	CHECK(set.at("settings").at("grid") == "1/8T");
	CHECK(set.at("settings").at("strength") == 0.75);
	CHECK(set.at("autoQuantize") == true);
	CHECK(f.historySteps() == steps);
	run(f.client, "quantize.apply", {{"part", part}});
	CHECK(f.historySteps() == steps + 1);

	const test::TempDir dir;
	const std::string file = (dir.path() / "quantized.dawproject").u8string();
	run(f.client, "project.save", {{"path", file}});
	run(f.client, "project.new");
	CHECK(run(f.client, "quantize.get").at("autoQuantize") == false);
	CHECK(run(f.client, "quantize.get").at("settings").at("grid") == "1/16");

	run(f.client, "project.load", {{"path", file}});
	const Json loaded = run(f.client, "quantize.get");
	CHECK(loaded.at("settings") == set.at("settings"));
	CHECK(loaded.at("autoQuantize") == true);
	// The original positions came along: resetting still works.
	const ObjectId loadedPart = run(f.client, "project.get").at("midiParts")[0].at("id").get<ObjectId>();
	run(f.client, "quantize.reset", {{"part", loadedPart}});
	CHECK(f.positions(loadedPart) == std::vector<Ticks>{quarters(3, 10)});
}

TEST_CASE("quantize presets: factory grids, saved settings and grooves", "[integration]")
{
	Fixture f;
	const Json presets = run(f.client, "quantize.presets").at("presets");
	const auto named = [](const Json& list, const std::string& name)
	{ return std::find_if(list.begin(), list.end(), [&name](const Json& p) { return p.at("name") == name; }); };
	for (const char* name : {"1/1", "1/16", "1/128", "1/8 Triplet", "1/64 Triplet", "1/4 Dotted"})
	{
		INFO(name);
		const auto preset = named(presets, name);
		REQUIRE(preset != presets.end());
		CHECK(preset->at("factory") == true);
	}
	CHECK(named(presets, "1/4 Dotted")->at("settings").at("grid") == "1/4.");

	run(f.client, "quantize.set", {{"settings", {{"grid", "1/8"}, {"swing", 0.5}}}});
	run(f.client, "quantize.savePreset", {{"name", "Shuffle"}});
	run(f.client, "quantize.savePreset", {{"name", "Loose"}, {"settings", {{"strength", 0.5}}}});
	Json list = run(f.client, "quantize.presets").at("presets");
	CHECK(named(list, "Shuffle")->at("settings").at("swing") == 0.5);
	CHECK(named(list, "Shuffle")->at("factory") == false);
	CHECK(named(list, "Loose")->at("settings").at("grid") == "1/8"); // the current settings, changed
	CHECK(named(list, "Loose")->at("settings").at("strength") == 0.5);

	run(f.client, "quantize.renamePreset", {{"name", "Shuffle"}, {"newName", "Swing 8"}});
	run(f.client, "quantize.removePreset", {{"name", "Loose"}});
	list = run(f.client, "quantize.presets").at("presets");
	CHECK(named(list, "Shuffle") == list.end());
	CHECK(named(list, "Loose") == list.end());
	CHECK(named(list, "Swing 8") != list.end());
	CHECK(list.size() == presets.size() + 1);

	CHECK_THAT(reject(f.client, "quantize.removePreset", {{"name", "1/16"}}), ContainsSubstring("factory"));
	CHECK_THAT(reject(f.client, "quantize.savePreset", {{"name", "1/16"}}), ContainsSubstring("factory"));
	CHECK_THAT(reject(f.client, "quantize.renamePreset", {{"name", "Swing 8"}, {"newName", "1/4"}}),
		ContainsSubstring("factory"));
	CHECK_THAT(
		reject(f.client, "quantize.removePreset", {{"name", "Nothing"}}), ContainsSubstring("no quantize preset"));
	CHECK_THAT(reject(f.client, "quantize.savePreset", {{"name", ""}}), ContainsSubstring("name"));

	// Presets belong to the engine, not to the project.
	run(f.client, "project.new");
	list = run(f.client, "quantize.presets").at("presets");
	CHECK(named(list, "Swing 8") != list.end());
}

TEST_CASE("a groove extracted from a part quantizes another one", "[integration]")
{
	Fixture f;
	const ObjectId drums = f.addPart(0);
	f.addNotes(drums, {0, quarters(11, 10), quarters(2), quarters(31, 10)});
	const Json preset = run(f.client, "quantize.extractGroove", {{"part", drums}, {"name", "Late beats"}}).at("preset");
	CHECK(preset.at("name") == "Late beats");
	CHECK(preset.at("settings").at("groove").at("length") == quarters(4));
	CHECK(preset.at("settings").at("groove").at("points").size() == 4);

	const ObjectId played = f.addPart(quarters(4));
	f.addNotes(played, {quarters(9, 10), quarters(3)});
	run(f.client, "quantize.apply", {{"part", played}, {"preset", "Late beats"}});
	CHECK(f.positions(played) == std::vector<Ticks>{quarters(11, 10), quarters(31, 10)});

	SECTION("a groove from positions")
	{
		run(f.client, "quantize.savePreset",
			{{"name", "Pushed"},
				{"settings",
					{{"groove",
						{{"length", quarters(2)},
							{"points", {{{"position", 0}}, {{"position", quarters(9, 10)}, {"velocity", 1.0}}}}}}}}});
		run(f.client, "quantize.apply", {{"part", played}, {"preset", "Pushed"}});
		CHECK(f.positions(played) == std::vector<Ticks>{quarters(9, 10), quarters(29, 10)});
	}
	SECTION("the default name is the part's")
	{
		CHECK(run(f.client, "quantize.extractGroove", {{"part", drums}}).at("preset").at("name") == "Instrument 01");
	}
	SECTION("a groove needs notes")
	{
		const ObjectId empty = f.addPart(quarters(8));
		CHECK_THAT(reject(f.client, "quantize.extractGroove", {{"part", empty}}), ContainsSubstring("no notes"));
	}
}

TEST_CASE("reset quantize leaves notes moved with note.set since", "[integration]")
{
	Fixture f;
	const ObjectId part = f.addPart(0);
	const std::vector<ObjectId> notes = f.addNotes(part, {quarters(3, 10), quarters(13, 10)});
	run(f.client, "quantize.apply", {{"part", part}, {"settings", {{"grid", "1/4"}}}});
	run(f.client, "note.set", {{"part", part}, {"notes", {{{"id", notes[1]}, {"position", quarters(17, 10)}}}}});
	// A note whose velocity changed still resets, and keeps its new velocity.
	run(f.client, "note.set", {{"part", part}, {"notes", {{{"id", notes[0]}, {"velocity", 1.0}}}}});
	const Json reset = run(f.client, "quantize.reset", {{"part", part}});
	CHECK(reset.at("notes").size() == 1);
	CHECK(f.positions(part) == std::vector<Ticks>{quarters(3, 10), quarters(17, 10)});
	CHECK(run(f.client, "project.get").at("midiParts")[0].at("notes")[0].at("velocity") == 1.0);
}

TEST_CASE("quantize.apply keeps notes inside their part", "[integration]")
{
	Fixture f;
	// A one-bar part: the note at 3.8 quarters stays rather than move to the next bar, where it wouldn't play.
	const ObjectId part = f.addPart(0, quarters(4));
	f.addNotes(part, {quarters(38, 10)});
	CHECK(run(f.client, "quantize.apply", {{"part", part}, {"settings", {{"grid", "1/4"}}}}).at("notes").empty());
	CHECK(f.positions(part) == std::vector<Ticks>{quarters(38, 10)});
}

TEST_CASE("a groove extracted from a part that starts later is in phase with it", "[integration]")
{
	Fixture f;
	// The part starts on beat 2, and its notes come a tenth of a quarter late: at 1.1 and 3.1 quarters.
	const ObjectId drums = f.addPart(quarters(1));
	f.addNotes(drums, {quarters(1, 10), quarters(21, 10)});
	const Json preset = run(f.client, "quantize.extractGroove", {{"part", drums}, {"name", "Late"}}).at("preset");
	CHECK(preset.at("settings").at("origin") == quarters(1));

	// The groove repeats every bar from beat 2: at 5.1 and 7.1 quarters in a part from 4 quarters.
	const ObjectId played = f.addPart(quarters(4));
	f.addNotes(played, {quarters(9, 10), quarters(29, 10)});
	run(f.client, "quantize.apply", {{"part", played}, {"preset", "Late"}});
	CHECK(f.positions(played) == std::vector<Ticks>{quarters(11, 10), quarters(31, 10)});
}

namespace
{
	const char* const g_presetsFile = "quantize-presets.json";

	// The user's presets, as quantize.presets lists them.
	Json userPresets(Client& client)
	{
		Json user = Json::array();
		const Json all = run(client, "quantize.presets");
		for (const Json& preset : all.at("presets"))
			if (preset.at("factory") == false)
				user.push_back(preset);
		return user;
	}

	std::vector<std::string> names(const Json& presets)
	{
		std::vector<std::string> result;
		result.reserve(presets.size());
		for (const Json& preset : presets)
			result.push_back(preset.at("name").get<std::string>());
		return result;
	}

	Json readJson(const std::filesystem::path& file)
	{
		return Json::parse(test::readText(file));
	}

	void writePresets(const std::filesystem::path& folder, const Json& presets)
	{
		std::ofstream(folder / g_presetsFile, std::ios::binary)
			<< Json{{"version", 1}, {"values", {{"quantize.presets", presets}}}}.dump();
	}
}

TEST_CASE("the user's quantize presets come back when GNUbase starts again", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / g_presetsFile;
	Json saved;
	{
		Fixture f(dir.path());
		CHECK(userPresets(f.client).empty());
		CHECK(!std::filesystem::exists(file)); // factory presets aren't written
		run(f.client, "quantize.set",
			{{"settings", {{"grid", "1/8"}, {"swing", 0.5}, {"seed", 12345678901234}, {"strength", 0.1}}}});
		run(f.client, "quantize.savePreset", {{"name", "Shuffle"}});
		const ObjectId drums = f.addPart(quarters(1));
		f.addNotes(drums, {quarters(1, 10), quarters(21, 10)});
		run(f.client, "quantize.extractGroove", {{"part", drums}, {"name", "Late"}});
		run(f.client, "quantize.savePreset", {{"name", "Gone"}});
		run(f.client, "quantize.renamePreset", {{"name", "Shuffle"}, {"newName", "Swing 8"}});
		run(f.client, "quantize.removePreset", {{"name", "Gone"}});
		saved = userPresets(f.client);
		CHECK(names(saved) == std::vector<std::string>{"Swing 8", "Late"});
	}

	// The file has each of the user's presets with its name and settings, and no factory preset.
	const Json document = readJson(file);
	CHECK(document.at("version") == 1);
	const Json& stored = document.at("values").at("quantize.presets");
	REQUIRE(stored.size() == 2);
	for (size_t i = 0; i < stored.size(); ++i)
	{
		CHECK(stored[i].at("name") == saved[i].at("name"));
		CHECK(stored[i].at("settings") == saved[i].at("settings"));
	}

	{
		Fixture next(dir.path());
		CHECK(userPresets(next.client) == saved);
		// The groove quantizes as it did in the engine that made it.
		const ObjectId played = next.addPart(quarters(4));
		next.addNotes(played, {quarters(9, 10), quarters(29, 10)});
		run(next.client, "quantize.apply", {{"part", played}, {"preset", "Late"}});
		CHECK(next.positions(played) == std::vector<Ticks>{quarters(11, 10), quarters(31, 10)});
		run(next.client, "quantize.removePreset", {{"name", "Late"}});
		run(next.client, "quantize.removePreset", {{"name", "Swing 8"}});
	}
	CHECK(readJson(file).at("values") == Json::object()); // no presets is the default
	Fixture last(dir.path());
	CHECK(userPresets(last.client).empty());
}

TEST_CASE("quantize presets that can't be read are skipped and stay in the file", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / g_presetsFile;

	SECTION("presets with invalid settings or names")
	{
		const Json presets = Json::array({
			{{"name", "Loose"}, {"settings", {{"grid", "1/8"}, {"strength", 0.5}}}},
			{{"name", "Too strong"}, {"settings", {{"strength", 2}}}},
			{{"name", "Newer"}, {"settings", {{"grid", "1/8"}, {"humanize", 0.3}}}}, // a newer GNUbase's setting
			{{"name", "Typed"}, {"settings", {{"tuplet", "three"}}}},
			{{"name", "Groove"}, {"settings", {{"groove", {{"length", 10}, {"points", {{{"position", 10}}}}}}}}},
			{{"name", "1/16"}, {"settings", Json::object()}}, // a factory preset's name
			{{"name", ""}, {"settings", Json::object()}},
			{{"name", "Loose"}, {"settings", Json::object()}}, // the same name again
			{{"settings", Json::object()}},
			"Swing",
			nullptr,
		});
		writePresets(dir.path(), presets);
		Fixture f(dir.path());
		const Json user = userPresets(f.client);
		REQUIRE(user.size() == 1);
		CHECK(user[0].at("name") == "Loose");
		CHECK(user[0].at("settings").at("grid") == "1/8");
		CHECK(user[0].at("settings").at("strength") == 0.5);
		CHECK(user[0].at("settings").at("swing") == 0.0); // what the preset doesn't have is the default
		const Json listed = run(f.client, "quantize.presets");
		CHECK(listed.at("problems").size() == presets.size() - 1); // why each one was skipped
		CHECK(!listed.at("readOnly").get<bool>());

		// The skipped ones stay for the GNUbase that wrote them, and so do their names, so that nothing is lost
		// unnoticed. quantize.removePreset removes them.
		CHECK_THAT(reject(f.client, "quantize.savePreset", {{"name", "Newer"}}), ContainsSubstring("can't be read"));
		CHECK_THAT(reject(f.client, "quantize.renamePreset", {{"name", "Loose"}, {"newName", "Newer"}}),
			ContainsSubstring("exists"));
		run(f.client, "quantize.removePreset", {{"name", "Too strong"}});
		run(f.client, "quantize.savePreset", {{"name", "Too strong"}, {"settings", {{"strength", 1.0}}}});
		run(f.client, "quantize.savePreset", {{"name", "Tight"}});
		// Replaces the preset it reads, and leaves the other one of that name.
		run(f.client, "quantize.savePreset", {{"name", "Loose"}, {"settings", {{"strength", 0.25}}}});
		CHECK(names(userPresets(f.client)) == std::vector<std::string>{"Loose", "Too strong", "Tight"});
		const Json stored = readJson(file).at("values").at("quantize.presets");
		REQUIRE(stored.size() == presets.size() + 1);
		CHECK(stored[0].at("settings").at("strength") == 0.25);
		for (size_t kept = 2; kept < presets.size(); ++kept)
			CHECK(stored[kept - 1] == presets[kept]);
		CHECK(stored[presets.size() - 1].at("name") == "Too strong");
		CHECK(stored.back().at("name") == "Tight");
	}
	SECTION("a file that isn't JSON")
	{
		std::ofstream(file, std::ios::binary) << R"({"version": 1, "values": {"quantize.presets": [)";
		{
			Fixture f(dir.path());
			CHECK(userPresets(f.client).empty());
			CHECK(std::filesystem::exists(dir.path() / "quantize-presets.corrupt.json")); // kept as it was
			run(f.client, "quantize.savePreset", {{"name", "Tight"}});
		}
		Fixture next(dir.path());
		CHECK(names(userPresets(next.client)) == std::vector<std::string>{"Tight"});
	}
	SECTION("presets that aren't a list")
	{
		writePresets(dir.path(), {{"Loose", {{"strength", 0.5}}}});
		Fixture f(dir.path());
		CHECK(userPresets(f.client).empty());
		CHECK(run(f.client, "quantize.presets").at("problems").size() == 1);
	}
	SECTION("a file of a newer GNUbase is left as it is, and quantize.presets says so")
	{
		const std::string newer = R"({"version": 2, "values": {"quantize.presets": []}})";
		std::ofstream(file, std::ios::binary) << newer;
		Fixture f(dir.path());
		run(f.client, "quantize.savePreset", {{"name", "Tight"}}); // until GNUbase quits
		CHECK(names(userPresets(f.client)) == std::vector<std::string>{"Tight"});
		const Json listed = run(f.client, "quantize.presets");
		CHECK(listed.at("readOnly").get<bool>());
		CHECK_THAT(listed.at("problems").dump(), ContainsSubstring("newer"));
		CHECK(listed.at("file") == file.u8string());
		CHECK(test::readText(file) == newer);
	}
}

TEST_CASE("a quantize preset that can't be saved changes nothing", "[integration]")
{
	const test::TempDir dir;
	Fixture f(dir.path());
	const Json listed = run(f.client, "quantize.presets");
	CHECK(!listed.at("readOnly").get<bool>());
	CHECK(listed.at("problems").empty());
	std::filesystem::create_directories(dir.path() / g_presetsFile); // where the file would be written
	CHECK_THAT(reject(f.client, "quantize.savePreset", {{"name", "Tight"}}), ContainsSubstring("cannot save"));
	CHECK(userPresets(f.client).empty());
	Fixture memory;
	CHECK(run(memory.client, "quantize.presets").at("file").is_null()); // kept in memory only
}

TEST_CASE("GNUbase processes on one settings folder keep each other's quantize presets", "[integration]")
{
	const test::TempDir dir;
	Fixture first(dir.path());
	Fixture second(dir.path()); // started before the first one saved anything
	run(first.client, "quantize.savePreset", {{"name", "First"}});
	run(second.client, "quantize.savePreset", {{"name", "Second"}});
	CHECK(names(userPresets(second.client)) == std::vector<std::string>{"First", "Second"});
	// A change starts from the presets in the file, whichever process saved them.
	run(first.client, "quantize.renamePreset", {{"name", "Second"}, {"newName", "Renamed"}});
	CHECK(names(userPresets(first.client)) == std::vector<std::string>{"First", "Renamed"});
	run(second.client, "quantize.removePreset", {{"name", "First"}});
	// A change that fails still shows the presets in the file as they are now.
	CHECK_THAT(reject(first.client, "quantize.removePreset", {{"name", "First"}}), ContainsSubstring("no quantize"));
	CHECK(names(userPresets(first.client)) == std::vector<std::string>{"Renamed"});
	Fixture third(dir.path());
	CHECK(names(userPresets(third.client)) == std::vector<std::string>{"Renamed"});
}

// Every version of the quantize presets file that a GNUbase release wrote is in tests/data/quantize-presets, as
// quantize-presets.v<version>.json. When the version goes up, add the new file: older ones must keep migrating.
TEST_CASE("every earlier version of the quantize presets file still loads", "[integration]")
{
	const std::filesystem::path fixtures = std::filesystem::path(GNUBASE_TEST_DATA_DIR) / "quantize-presets";
	int found = 0;
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(fixtures))
	{
		CAPTURE(entry.path().filename().string());
		const test::TempDir dir;
		std::filesystem::copy_file(entry.path(), dir.path() / g_presetsFile);
		const size_t count = readJson(entry.path()).at("values").at("quantize.presets").size();
		{
			const settings::Store store(quantizePresetsFormat(), dir.path());
			CHECK((store.loadReport().status == settings::LoadStatus::Loaded ||
				store.loadReport().status == settings::LoadStatus::Migrated));
			CHECK(store.loadReport().problems.empty());
		}
		Fixture f(dir.path());
		CHECK(count > 0);
		CHECK(userPresets(f.client).size() == count); // every preset is read
		++found;
	}
	CHECK(found == quantizePresetsFormat().version);
}
