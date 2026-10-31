// SPDX-License-Identifier: GPL-3.0-or-later
// Time formats for rulers and position displays (#84): positions read as the engine converts them, and ruler marks
// at the positions the engine has for bars, beats, seconds, frames and samples, also across tempo and signature
// changes; positions and lengths typed into position displays.
#include "engine/schema.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"
#include "engine/time/timecode.h"
#include "ui/timeformat.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using time::g_ticksPerQuarter;

namespace
{
	// 120 BPM in 4/4 for two bars, then 60 BPM in 3/4.
	ui::TimeContext changingContext()
	{
		ui::TimeContext context;
		context.tempo = time::TempoMap({{0, 120.0, false}, {8 * g_ticksPerQuarter, 60.0, false}});
		context.signatures = time::SignatureMap({{0, {4, 4}}, {2, {3, 4}}});
		context.sampleRate = 44100.0;
		return context;
	}

	std::vector<ui::RulerMark> marks(
		ui::TimeFormat format, const ui::TimeContext& context, double from, double to, double minimum)
	{
		std::vector<ui::RulerMark> marks;
		ui::rulerScale(format, context).marks(from, to, minimum, marks);
		return marks;
	}

	std::vector<ui::RulerMark> labelled(const std::vector<ui::RulerMark>& all)
	{
		std::vector<ui::RulerMark> majors;
		for (const ui::RulerMark& mark : all)
			if (!mark.label.empty())
				majors.push_back(mark);
		return majors;
	}
}

TEST_CASE("time formats have names for commands and settings", "[unit]")
{
	for (const ui::TimeFormat format : {ui::TimeFormat::BarsBeats, ui::TimeFormat::Seconds, ui::TimeFormat::Timecode,
			 ui::TimeFormat::Samples, ui::TimeFormat::UserFrames})
		CHECK(ui::timeFormatFromName(ui::timeFormatName(format)) == format);
	CHECK(ui::timeFormatName(ui::TimeFormat::BarsBeats) == "barsBeats");
	CHECK(ui::timeFormatName(ui::TimeFormat::UserFrames) == "userFrames");
	CHECK(!ui::timeFormatFromName("bars").has_value());
}

TEST_CASE("positions read in every time format as the engine converts them", "[unit]")
{
	const ui::TimeContext context = changingContext();
	for (const time::Samples sample : {time::Samples{0}, time::Samples{22050}, time::Samples{176400},
			 time::Samples{176400 + 44100 + 441}, time::Samples{1234567}})
	{
		INFO("sample " << sample);
		const double seconds = static_cast<double>(sample) / context.sampleRate;
		CHECK(ui::formatTime(ui::TimeFormat::BarsBeats, seconds, context) ==
			time::formatBarsBeats(context.signatures.toBarsBeats(context.tempo.toTicks(sample, context.sampleRate))));
		CHECK(ui::formatTime(ui::TimeFormat::Seconds, seconds, context) == time::formatSeconds(seconds));
		CHECK(ui::formatTime(ui::TimeFormat::Timecode, seconds, context) ==
			time::formatTimecode(time::toTimecode(seconds, time::g_fps25), time::g_fps25));
		CHECK(ui::formatTime(ui::TimeFormat::Samples, seconds, context) == std::to_string(sample));
		CHECK(ui::formatTime(ui::TimeFormat::UserFrames, seconds, context) ==
			time::formatTimecode(time::toTimecode(seconds, {30, false, false}), {30, false, false}));
	}
	// Two bars of 4/4 at 120 BPM take 4 seconds; then a quarter takes a second.
	CHECK(ui::formatTime(ui::TimeFormat::BarsBeats, 4.0, context) == "3.1.1.0");
	CHECK(ui::formatTime(ui::TimeFormat::BarsBeats, 5.0, context) == "3.2.1.0");
	CHECK(ui::formatTime(ui::TimeFormat::Seconds, 5.0, context) == "0:00:05.000");
	CHECK(ui::formatTime(ui::TimeFormat::Timecode, 5.5, context) == "00:00:05:12");
	CHECK(ui::formatTime(ui::TimeFormat::Samples, 5.0, context) == "220500");

	// The project's frame rate, e.g. drop-frame timecode, and the user's own.
	ui::TimeContext drop = context;
	drop.frameRate = time::g_fps30PullDownDrop;
	drop.userFramesPerSecond = 24;
	CHECK(ui::formatTime(ui::TimeFormat::Timecode, 60.07, drop) == "00:01:00;02");
	CHECK(ui::formatTime(ui::TimeFormat::UserFrames, 1.5, drop) == "00:00:01:12");
}

TEST_CASE("bars and beats on the ruler are where the tempo and signature maps put them", "[unit]")
{
	const ui::TimeContext context = changingContext();
	// Labels at least 1.5 seconds apart: a label every bar, 2 seconds in 4/4 at 120 BPM, 3 in 3/4 at 60.
	const std::vector<ui::RulerMark> bars = labelled(marks(ui::TimeFormat::BarsBeats, context, 0.0, 12.0, 1.5));
	REQUIRE(bars.size() == 5);
	const std::vector<double> starts{0.0, 2.0, 4.0, 7.0, 10.0};
	for (size_t i = 0; i < bars.size(); ++i)
	{
		CHECK(bars[i].position == Catch::Approx(starts[i]));
		CHECK(bars[i].label == std::to_string(i + 1));
	}
	// Beats between them as minor ticks: four in a bar of 4/4, three in 3/4.
	const std::vector<ui::RulerMark> all = marks(ui::TimeFormat::BarsBeats, context, 0.0, 6.9, 1.5);
	CHECK(all.size() == 4 + 4 + 3);
	CHECK(all[1].position == Catch::Approx(0.5));
	CHECK(all[9].position == Catch::Approx(5.0));

	// Zoomed in, every beat is labelled, the first of a bar with the bar only.
	const std::vector<ui::RulerMark> beats = labelled(marks(ui::TimeFormat::BarsBeats, context, 3.0, 6.5, 0.4));
	REQUIRE(beats.size() == 5);
	CHECK(beats[0].label == "2.3");
	CHECK(beats[1].label == "2.4");
	CHECK(beats[2].label == "3");
	CHECK(beats[3].label == "3.2");
	CHECK(beats[3].position == Catch::Approx(5.0));
	CHECK(beats[4].label == "3.3");

	// Further in: sixteenths, then display ticks.
	const std::vector<ui::RulerMark> sixteenths = labelled(marks(ui::TimeFormat::BarsBeats, context, 0.0, 0.5, 0.1));
	REQUIRE(sixteenths.size() == 4);
	CHECK(sixteenths[1].label == "1.1.2");
	CHECK(sixteenths[1].position == Catch::Approx(0.125));
	const std::vector<ui::RulerMark> ticks = labelled(marks(ui::TimeFormat::BarsBeats, context, 0.0, 0.1, 0.06));
	REQUIRE(ticks.size() >= 2);
	CHECK(ticks[1].label == "1.1.1.60");

	// Zoomed out, every fourth bar (as far apart as the shortest bars at the fastest tempo need), and nothing before
	// the project start.
	const std::vector<ui::RulerMark> far = labelled(marks(ui::TimeFormat::BarsBeats, context, -10.0, 40.0, 5.0));
	REQUIRE(far.size() >= 3);
	CHECK(far[0].position == 0.0);
	CHECK(far[1].label == "5");
	CHECK(far[1].position == Catch::Approx(10.0));
	CHECK(far[2].label == "9");
}

TEST_CASE("seconds, timecode and samples on the ruler step in round units", "[unit]")
{
	const ui::TimeContext context = changingContext();

	const std::vector<ui::RulerMark> seconds = labelled(marks(ui::TimeFormat::Seconds, context, 0.0, 30.0, 4.0));
	REQUIRE(seconds.size() == 6);
	CHECK(seconds[1].position == 5.0);
	CHECK(seconds[1].label == "0:00:05");
	const std::vector<ui::RulerMark> milliseconds = labelled(marks(ui::TimeFormat::Seconds, context, 1.0, 1.1, 0.015));
	REQUIRE(milliseconds.size() >= 4);
	CHECK(milliseconds[1].position == Catch::Approx(1.02));
	CHECK(milliseconds[1].label == "0:00:01.020");

	// Timecode at 25 fps: whole frames, then whole seconds and minutes.
	const std::vector<ui::RulerMark> frames = labelled(marks(ui::TimeFormat::Timecode, context, 0.0, 1.0, 0.15));
	REQUIRE(frames.size() == 5);
	CHECK(frames[1].position == Catch::Approx(0.2));
	CHECK(frames[1].label == "00:00:00:05");
	const std::vector<ui::RulerMark> minutes = labelled(marks(ui::TimeFormat::Timecode, context, 0.0, 300.0, 50.0));
	REQUIRE(minutes.size() == 5);
	CHECK(minutes[1].position == Catch::Approx(60.0));
	CHECK(minutes[1].label == "00:01:00:00");

	// Samples in decimal steps.
	const std::vector<ui::RulerMark> samples = labelled(marks(ui::TimeFormat::Samples, context, 0.0, 1.0, 0.2));
	REQUIRE(samples.size() == 5);
	CHECK(samples[1].label == "10000");
	CHECK(samples[1].position == Catch::Approx(10000.0 / 44100.0));
}

TEST_CASE("labelled ruler marks are never closer than asked, across a tempo change", "[unit]")
{
	ui::TimeContext context;
	context.tempo = time::TempoMap({{0, 60.0, false}, {16 * g_ticksPerQuarter, 240.0, true}});
	for (const ui::TimeFormat format : {ui::TimeFormat::BarsBeats, ui::TimeFormat::Seconds, ui::TimeFormat::Timecode,
			 ui::TimeFormat::Samples, ui::TimeFormat::UserFrames})
		for (const double minimum : {0.01, 0.3, 2.0, 45.0})
		{
			INFO(ui::timeFormatName(format) << ", minimum " << minimum);
			const std::vector<ui::RulerMark> majors = labelled(marks(format, context, 0.0, 200.0 * minimum, minimum));
			REQUIRE(majors.size() >= 2);
			for (size_t i = 1; i < majors.size(); ++i)
				CHECK(majors[i].position - majors[i - 1].position >= minimum * (1.0 - 1e-6));
		}
}

TEST_CASE("positions typed in every time format read back as the engine converts them", "[unit]")
{
	const ui::TimeContext context = changingContext();
	for (const ui::TimeFormat format : {ui::TimeFormat::BarsBeats, ui::TimeFormat::Seconds, ui::TimeFormat::Timecode,
			 ui::TimeFormat::Samples, ui::TimeFormat::UserFrames})
		for (const double seconds : {0.0, 4.0, 5.4, 61.2}) // on frames and display ticks
		{
			INFO(ui::timeFormatName(format) << " at " << seconds);
			const std::optional<double> typed =
				ui::parseTime(format, ui::formatTime(format, seconds, context), context);
			REQUIRE(typed.has_value());
			CHECK(*typed == Catch::Approx(seconds).margin(1e-6));
		}

	// What is left out counts from the start of the bar, minute or second.
	CHECK(ui::parseTime(ui::TimeFormat::BarsBeats, "3", context) == Catch::Approx(4.0));
	CHECK(ui::parseTime(ui::TimeFormat::BarsBeats, "3.2", context) == Catch::Approx(5.0));
	CHECK(ui::parseTime(ui::TimeFormat::BarsBeats, " 3 2 1 0 ", context) == Catch::Approx(5.0));
	CHECK(ui::parseTime(ui::TimeFormat::Seconds, "5", context) == Catch::Approx(5.0));
	CHECK(ui::parseTime(ui::TimeFormat::Seconds, "1:30.5", context) == Catch::Approx(90.5));
	CHECK(ui::parseTime(ui::TimeFormat::Timecode, "00:00:05:12", context) == Catch::Approx(5.48));
	CHECK(ui::parseTime(ui::TimeFormat::Timecode, "1:00", context) == Catch::Approx(1.0));
	CHECK(ui::parseTime(ui::TimeFormat::Samples, "88200", context) == Catch::Approx(2.0));

	// Text that isn't a position.
	for (const char* text : {"", "abc", "0.1.1.0", "1.0", "-5", "1..2", "1.2.3.4.5"})
	{
		INFO(text);
		CHECK(!ui::parseTime(ui::TimeFormat::BarsBeats, text, context).has_value());
	}
	CHECK(!ui::parseTime(ui::TimeFormat::Seconds, "1:2:3:4", context).has_value());
	CHECK(!ui::parseTime(ui::TimeFormat::Timecode, "00:00:01:25", context).has_value()); // 25 fps: frames 0 to 24
	CHECK(!ui::parseTime(ui::TimeFormat::Samples, "12.5", context).has_value());
}

TEST_CASE("lengths read as bars and beats from their start, or as times in the other formats", "[unit]")
{
	const ui::TimeContext context = changingContext();
	// Five quarters from the third one: a bar and a beat of 4/4 at 120 BPM.
	CHECK(ui::formatLength(ui::TimeFormat::BarsBeats, 1.0, 3.5, context) == "1.1.0.0");
	CHECK(ui::formatLength(ui::TimeFormat::BarsBeats, 1.0, 1.0, context) == "0.0.0.0");
	CHECK(ui::formatLength(ui::TimeFormat::Seconds, 1.0, 3.5, context) == "0:00:02.500");
	CHECK(ui::formatLength(ui::TimeFormat::Samples, 1.0, 3.5, context) == "110250");
	CHECK(ui::parseLength(ui::TimeFormat::BarsBeats, "1.1.0.0", 1.0, context) == Catch::Approx(2.5));
	CHECK(ui::parseLength(ui::TimeFormat::BarsBeats, "1", 1.0, context) == Catch::Approx(2.0));
	// Past the tempo change at bar 3 a beat takes a second.
	CHECK(ui::parseLength(ui::TimeFormat::BarsBeats, "0.3", 3.0, context) == Catch::Approx(1.0 + 1.0));
	CHECK(ui::parseLength(ui::TimeFormat::Seconds, "2.5", 2.0, context) == Catch::Approx(2.5));
	CHECK(ui::parseLength(ui::TimeFormat::Timecode, "00:00:01:00", 2.0, context) == Catch::Approx(1.0));
	CHECK(!ui::parseLength(ui::TimeFormat::BarsBeats, "x", 2.0, context).has_value());
}

TEST_CASE("the time context comes from the project as project.get returns it", "[unit]")
{
	const engine::Json project = {{"sampleRate", 44100.0},
		{"tempo",
			{{{"position", 0}, {"bpm", 120.0}, {"ramp", false}},
				{{"position", 8 * g_ticksPerQuarter}, {"bpm", 60.0}, {"ramp", false}}}},
		{"signatures",
			{{{"bar", 0}, {"numerator", 4}, {"denominator", 4}}, {{"bar", 2}, {"numerator", 3}, {"denominator", 4}}}}};
	ui::TimeContext context;
	REQUIRE(ui::readTimeContext(project, context));
	CHECK(context.sampleRate == 44100.0);
	CHECK(context.tempo == changingContext().tempo);
	CHECK(context.signatures == changingContext().signatures);
	CHECK(!ui::readTimeContext(
		{{"sampleRate", 48000.0}, {"tempo", engine::Json::array()}, {"signatures", engine::Json::array()}}, context));
	CHECK(context.sampleRate == 44100.0); // unchanged
}
