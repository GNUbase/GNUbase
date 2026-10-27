// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"
#include "engine/time/timecode.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <stdexcept>

using namespace gnubase::time;

namespace
{
	constexpr Ticks g_quarter = g_ticksPerQuarter;

	// 120 BPM for 4 quarters, then 60 BPM.
	TempoMap steppedTempo()
	{
		return TempoMap({{0, 120.0, false}, {4 * g_quarter, 60.0, false}});
	}

	// 120 BPM for 4 quarters, step to 60 BPM, ramp to 180 BPM over the next 8 quarters, then constant.
	TempoMap mixedTempo()
	{
		return TempoMap({{0, 120.0, false}, {4 * g_quarter, 60.0, false}, {12 * g_quarter, 180.0, true}});
	}
}

TEST_CASE("ticks per quarter divide evenly into binary values, tuplets and display ticks", "[unit]")
{
	CHECK(g_ticksPerQuarter % (1 << 14) == 0);
	CHECK(g_ticksPerQuarter % 3 == 0);
	CHECK(g_ticksPerQuarter % 5 == 0);
	CHECK(g_ticksPerQuarter % 7 == 0);
	for (const int displayTicksPerSixteenth : {24, 48, 96, 120, 192, 240, 384, 480, 960})
		CHECK((g_ticksPerQuarter / 4) % displayTicksPerSixteenth == 0);
}

TEST_CASE("a fixed tempo converts exactly", "[unit]")
{
	const TempoMap tempo(120.0);
	CHECK(tempo.toSeconds(g_quarter) == 0.5);
	CHECK(tempo.toSamples(g_quarter, 48000.0) == 24000);
	CHECK(tempo.toTicks(Samples{24000}, 48000.0) == g_quarter);
	CHECK(tempo.bpmAt(100 * g_quarter) == 120.0);
}

TEST_CASE("a tempo step changes the speed at its position", "[unit]")
{
	const TempoMap tempo = steppedTempo();
	CHECK(tempo.toSeconds(4 * g_quarter) == 2.0);
	CHECK(tempo.toSeconds(5 * g_quarter) == 3.0); // one quarter at 60 BPM
	CHECK(tempo.toTicks(3.0) == 5 * g_quarter);
	CHECK(tempo.bpmAt((4 * g_quarter) - 1) == 120.0);
	CHECK(tempo.bpmAt(4 * g_quarter) == 60.0);
}

TEST_CASE("a tempo ramp changes the speed linearly per beat", "[unit]")
{
	const TempoMap tempo = mixedTempo();
	// From 60 to 180 BPM over 8 quarters: k = 15 BPM per beat, duration = 60 / k * ln(180 / 60).
	const double rampSeconds = 4.0 * std::log(3.0);
	CHECK(tempo.bpmAt(8 * g_quarter) == Catch::Approx(120.0));
	CHECK(tempo.toSeconds(12 * g_quarter) == Catch::Approx(2.0 + rampSeconds).epsilon(1e-12));
	CHECK(tempo.toSeconds(13 * g_quarter) == Catch::Approx(2.0 + rampSeconds + (60.0 / 180.0)).epsilon(1e-12));
	CHECK(tempo.toTicks(2.0 + rampSeconds) == 12 * g_quarter);
}

TEST_CASE("sample positions survive a round trip through ticks across steps and ramps", "[unit]")
{
	const TempoMap tempo = mixedTempo();
	for (const double sampleRate : {44100.0, 48000.0, 96000.0, 192000.0})
	{
		bool exact = true;
		const auto end = static_cast<Samples>(20.0 * sampleRate);
		for (Samples sample = 0; sample < end; sample += 37)
			exact = exact && tempo.toSamples(tempo.toTicks(sample, sampleRate), sampleRate) == sample;
		CHECK(exact);
	}
}

TEST_CASE("musical grid positions survive a round trip through seconds", "[unit]")
{
	const TempoMap tempo = mixedTempo();
	bool exact = true;
	for (Ticks tick = 0; tick < 24 * g_quarter; tick += g_quarter / 12) // every triplet sixteenth
		exact = exact && tempo.toTicks(tempo.toSeconds(tick)) == tick;
	CHECK(exact);
}

TEST_CASE("tempo maps reject invalid points", "[unit]")
{
	CHECK_THROWS_AS(TempoMap({{g_quarter, 120.0, false}}), std::invalid_argument);                     // not at 0
	CHECK_THROWS_AS(TempoMap({{0, 120.0, false}, {0, 90.0, false}}), std::invalid_argument);           // not increasing
	CHECK_THROWS_AS(TempoMap({{0, 120.0, false}, {g_quarter, 2000.0, false}}), std::invalid_argument); // too fast
	CHECK_THROWS_AS(TempoMap(std::vector<TempoPoint>{}), std::invalid_argument);                       // empty
	CHECK_THROWS_AS(TempoMap(0.0), std::invalid_argument);                                             // too slow
}

TEST_CASE("bars start where the time signatures say", "[unit]")
{
	// 4/4 for bars 1-2, 3/4 for bars 3-4, 6/8 from bar 5 (zero-based bars 0, 2, 4).
	const SignatureMap signatures({{0, {4, 4}}, {2, {3, 4}}, {4, {6, 8}}});
	CHECK(signatures.barStart(0) == 0);
	CHECK(signatures.barStart(2) == 8 * g_quarter);
	CHECK(signatures.barStart(3) == 11 * g_quarter);
	CHECK(signatures.barStart(4) == 14 * g_quarter);
	CHECK(signatures.barStart(5) == 17 * g_quarter); // 6/8 bar = 3 quarters
	CHECK(signatures.signatureAt(16 * g_quarter).numerator == 6);
}

TEST_CASE("ticks convert to bars, beats, sixteenths and display ticks", "[unit]")
{
	const SignatureMap signatures({{0, {4, 4}}, {2, {3, 4}}, {4, {6, 8}}});

	const BarsBeats inThreeFour = signatures.toBarsBeats((11 * g_quarter) + (g_quarter / 2));
	CHECK(inThreeFour.bar == 3);
	CHECK(inThreeFour.beat == 0);
	CHECK(inThreeFour.sixteenth == 2);
	CHECK(inThreeFour.tick == 0);
	CHECK(formatBarsBeats(inThreeFour) == "4.1.3.0");

	// In 6/8 a beat is an eighth note: two sixteenths per beat.
	const BarsBeats inSixEight = signatures.toBarsBeats((14 * g_quarter) + (3 * g_quarter / 4) + (g_quarter / 8));
	CHECK(formatBarsBeats(inSixEight) == "5.2.2.60");

	bool exact = true;
	for (Ticks tick = 0; tick < 20 * g_quarter; tick += g_quarter / 480) // every display tick
		exact = exact && signatures.toTicks(signatures.toBarsBeats(tick)) == tick;
	CHECK(exact);
}

TEST_CASE("signature maps reject invalid changes", "[unit]")
{
	CHECK_THROWS_AS(SignatureMap({{1, {4, 4}}}), std::invalid_argument);              // not at bar 0
	CHECK_THROWS_AS(SignatureMap({{0, {4, 4}}, {0, {3, 4}}}), std::invalid_argument); // not increasing
	CHECK_THROWS_AS(SignatureMap(TimeSignature{4, 3}), std::invalid_argument);        // denominator not a power of two
	CHECK_THROWS_AS(SignatureMap(TimeSignature{0, 4}), std::invalid_argument);        // no beats
	CHECK_THROWS_AS(SignatureMap(TimeSignature{4, 32}), std::invalid_argument);       // beat shorter than a sixteenth
}

TEST_CASE("timecode at integer frame rates", "[unit]")
{
	const Timecode timecode = toTimecode(3723.5, g_fps25);
	CHECK(timecode.hours == 1);
	CHECK(timecode.minutes == 2);
	CHECK(timecode.seconds == 3);
	CHECK(timecode.frames == 12);
	CHECK(formatTimecode(timecode, g_fps25) == "01:02:03:12");
	CHECK(toSeconds(timecode, g_fps25) == 3723.48);
	CHECK(formatTimecode(toTimecode(-0.5, g_fps25), g_fps25) == "-00:00:00:12");
}

TEST_CASE("pulled-down rates count nominal frames at the slower speed", "[unit]")
{
	CHECK(toSeconds({false, 1, 0, 0, 0}, g_fps24PullDown) == Catch::Approx(3603.6).epsilon(1e-15));
	CHECK(toFrameNumber({false, 1, 0, 0, 0}, g_fps30PullDown) == 108000);

	bool exact = true;
	for (int64_t frame = 0; frame < 100000; ++frame)
	{
		const double seconds = toSeconds(fromFrameNumber(frame, g_fps30PullDown), g_fps30PullDown);
		exact = exact && toFrameNumber(toTimecode(seconds, g_fps30PullDown), g_fps30PullDown) == frame;
	}
	CHECK(exact);
}

TEST_CASE("drop-frame timecode skips frame labels 0 and 1 except every tenth minute", "[unit]")
{
	CHECK(formatTimecode(fromFrameNumber(1799, g_fps30PullDownDrop), g_fps30PullDownDrop) == "00:00:59;29");
	CHECK(formatTimecode(fromFrameNumber(1800, g_fps30PullDownDrop), g_fps30PullDownDrop) == "00:01:00;02");
	CHECK(formatTimecode(fromFrameNumber(17982, g_fps30PullDownDrop), g_fps30PullDownDrop) == "00:10:00;00");

	bool exact = true;
	for (int64_t frame = 0; frame < int64_t{30} * 60 * 30; ++frame) // 30 minutes of frames
		exact = exact && toFrameNumber(fromFrameNumber(frame, g_fps30PullDownDrop), g_fps30PullDownDrop) == frame;
	CHECK(exact);
}

TEST_CASE("seconds display as hours, minutes, seconds and milliseconds", "[unit]")
{
	CHECK(formatSeconds(3723.5) == "1:02:03.500");
	CHECK(formatSeconds(0.0) == "0:00:00.000");
	CHECK(formatSeconds(-1.25) == "-0:00:01.250");
}

TEST_CASE("positions convert between musical and linear time base", "[unit]")
{
	const TempoMap tempo = steppedTempo();
	const Position musical{TimeBase::Musical, 5 * g_quarter};
	CHECK(toSamples(musical, tempo, 48000.0) == 144000); // 3 s
	const Position linear = rebase(musical, TimeBase::Linear, tempo, 48000.0);
	CHECK(linear.base == TimeBase::Linear);
	CHECK(linear.value == 144000);
	CHECK(rebase(linear, TimeBase::Musical, tempo, 48000.0).value == 5 * g_quarter);
	CHECK(toTicks(linear, tempo, 48000.0) == 5 * g_quarter);
}
