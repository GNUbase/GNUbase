// SPDX-License-Identifier: GPL-3.0-or-later
// The audio functions (#147, docs/features/audio-functions.md): Detect Silence, the spectrum and the statistics of
// reference signals whose values are known, and the commands that process events with them.
// tests/audiofunctions_ui_tests.cpp drives their dialogs in the app.
#include "engine/analysis/statistics.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;
	constexpr int g_rate = 48000;

	// Audio of seconds at 48 kHz, each channel the value of signal(channel, seconds) at each frame.
	test::Audio make(double seconds, int channels, const std::function<double(int, double)>& signal)
	{
		test::Audio audio;
		audio.sampleRate = g_rate;
		audio.channels = channels;
		const auto frames = static_cast<size_t>(std::llround(seconds * g_rate));
		audio.samples.resize(frames * static_cast<size_t>(channels));
		for (size_t i = 0; i < frames; ++i)
			for (int c = 0; c < channels; ++c)
				audio.samples[(i * static_cast<size_t>(channels)) + static_cast<size_t>(c)] =
					static_cast<float>(signal(c, static_cast<double>(i) / g_rate));
		return audio;
	}

	double sine(double frequency, double amplitude, double t)
	{
		return amplitude * std::sin(2.0 * g_pi * frequency * t);
	}

	// A 1 kHz tone of amplitude 0.5 (-6 dBFS) from 0.5 to 1.5 seconds and one of 0.25 (-12 dBFS) from 2 to 2.5
	// seconds, in three seconds.
	test::Audio tones()
	{
		return make(3.0, 1,
			[](int, double t)
			{
				if (t >= 0.5 && t < 1.5)
					return sine(1000.0, 0.5, t - 0.5);
				if (t >= 2.0 && t < 2.5)
					return sine(1000.0, 0.25, t - 2.0);
				return 0.0;
			});
	}

	std::vector<engine::analysis::Section> sections(
		const test::Audio& audio, const engine::analysis::SilenceSettings& settings)
	{
		engine::analysis::SilenceDetector detector(audio.sampleRate, audio.channels, settings);
		// In blocks that don't divide the sections, as files are read.
		for (size_t frame = 0; frame < audio.frames(); frame += 1000)
			detector.feed(audio.samples.data() + (frame * static_cast<size_t>(audio.channels)),
				std::min<size_t>(1000, audio.frames() - frame));
		return detector.finish();
	}

	int64_t at(double seconds)
	{
		return std::llround(seconds * g_rate);
	}

	// Whether sections are those from and to, each within a millisecond.
	bool near(
		const std::vector<engine::analysis::Section>& found, const std::vector<std::pair<double, double>>& expected)
	{
		if (found.size() != expected.size())
			return false;
		for (size_t i = 0; i < found.size(); ++i)
			if (std::abs(found[i].start - at(expected[i].first)) > 48 ||
				std::abs(found[i].end - at(expected[i].second)) > 48)
				return false;
		return true;
	}

	struct Fixture
	{
		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const engine::Result result = client.call(command, parameters);
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, const Json& parameters)
		{
			const engine::Result result = client.call(command, parameters);
			REQUIRE(!result.ok());
			return result.error;
		}

		// A clip of audio, and an event that plays all of it on a linear audio track from one second.
		int64_t add(const test::Audio& audio, const std::string& name)
		{
			const std::string file = (dir.path() / (name + ".wav")).u8string();
			test::writeWav(file, audio);
			const Json media = call("media.add",
				{{"path", file}, {"sampleRate", audio.sampleRate}, {"channels", audio.channels},
					{"length", static_cast<int64_t>(audio.frames())}})
								   .at("media");
			clip = call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
			if (track == 0)
				track =
					call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<int64_t>();
			event = call("audioEvent.add",
				{{"track", track}, {"clip", clip}, {"position", g_rate}, {"offset", 0},
					{"length", static_cast<int64_t>(audio.frames())}})
						.at("event")
						.at("id")
						.get<int64_t>();
			return clip;
		}

		Json statistics(const test::Audio& audio)
		{
			return call("clip.statistics", {{"clip", add(audio, "statistics")}});
		}

		test::TempDir dir;
		engine::Engine engine;
		engine::Client client{engine};
		int64_t track = 0;
		int64_t clip = 0;
		int64_t event = 0;
	};

	double number(const Json& value)
	{
		REQUIRE(value.is_number());
		return value.get<double>();
	}
}

TEST_CASE("Detect Silence finds the tones of a signal, with its thresholds, minimum times and rolls", "[unit]")
{
	engine::analysis::SilenceSettings settings;
	settings.minimumClosed = 0.0; // the tones' zero crossings don't close a section
	CHECK(near(sections(tones(), settings), {{0.5, 1.5}, {2.0, 2.5}}));

	// The quieter tone doesn't reach an open threshold of -10 dBFS.
	settings.openThreshold = -10.0;
	CHECK(near(sections(tones(), settings), {{0.5, 1.5}}));

	// Between the thresholds a sound keeps a section open that it doesn't open: a tone at -25 dBFS after a loud one.
	settings = {};
	const test::Audio tail = make(2.0, 1,
		[](int, double t)
		{
			if (t < 0.5)
				return sine(500.0, 0.5, t);
			if (t < 1.0)
				return sine(500.0, 0.056, t);
			if (t >= 1.5)
				return sine(500.0, 0.056, t);
			return 0.0;
		});
	CHECK(near(sections(tail, settings), {{0.0, 1.0}}));

	// Silences shorter than the minimum time closed stay, sounds shorter than the minimum time open go.
	const test::Audio gaps = make(1.0, 2,
		[](int channel, double t)
		{
			if (channel == 1 && t >= 0.1 && t < 0.3)
				return sine(440.0, 0.5, t); // the right channel opens as well
			if (channel == 0 && t >= 0.35 && t < 0.6)
				return sine(440.0, 0.5, t);
			if (channel == 0 && t >= 0.8 && t < 0.805)
				return 0.9; // a click of 5 ms
			return 0.0;
		});
	settings.minimumClosed = 0.1;
	settings.minimumOpen = 0.01;
	CHECK(near(sections(gaps, settings), {{0.1, 0.6}}));
	settings.minimumClosed = 0.03;
	settings.minimumOpen = 0.0;
	CHECK(near(sections(gaps, settings), {{0.1, 0.3}, {0.35, 0.6}, {0.8, 0.805}}));

	// Pre-roll and post-roll widen the sections, within the audio, and join those that then overlap.
	settings.minimumOpen = 0.01;
	settings.preRoll = 0.2;
	settings.postRoll = 0.1;
	CHECK(near(sections(gaps, settings), {{0.0, 0.7}}));

	// Silence has no sections.
	CHECK(sections(make(1.0, 1, [](int, double) { return 0.0; }), {}).empty());
}

TEST_CASE(
	"clip.detectSilence and event.stripSilence split events at the sounds and add regions, as one undo step", "[unit]")
{
	Fixture f;
	const int64_t clip = f.add(tones(), "tones");
	const Json parameters = {{"clip", clip}, {"minimumClosed", 0}};
	Json found = f.call("clip.detectSilence", parameters);
	REQUIRE(found.at("sections").size() == 2);
	CHECK(std::abs(found.at("sections").at(1).at("end").get<int64_t>() - at(2.5)) <= 48);

	// Within a range of the clip, in its frames.
	Json range = parameters;
	range.update({{"start", at(1.0)}, {"end", at(2.2)}});
	found = f.call("clip.detectSilence", range).at("sections");
	REQUIRE(found.size() == 2);
	CHECK(std::abs(found.at(0).at("start").get<int64_t>() - at(1.0)) <= 48);
	CHECK(std::abs(found.at(0).at("end").get<int64_t>() - at(1.5)) <= 48);
	CHECK(std::abs(found.at(1).at("end").get<int64_t>() - at(2.2)) <= 48);

	CHECK_THAT(f.fails("clip.detectSilence", {{"clip", clip}, {"openThreshold", -40}, {"closeThreshold", -30}}),
		ContainsSubstring("closeThreshold must not be higher"));
	CHECK_THAT(f.fails("clip.detectSilence", {{"clip", clip}, {"start", 10}, {"end", 5}}),
		ContainsSubstring("start before end"));
	CHECK_THAT(f.fails("clip.detectSilence", {{"clip", clip}, {"preRoll", -1}}), ContainsSubstring("preRoll"));

	// Strip Silence, Add as Regions and Apply Fades.
	const Json processed = f.call("event.stripSilence",
		{{"events", {f.event}}, {"minimumClosed", 0}, {"regions", true}, {"regionName", "Take"}, {"autoNumberStart", 3},
			{"fades", 5}});
	CHECK(processed.at("sections") == 2);
	REQUIRE(processed.at("events").size() == 2);
	REQUIRE(processed.at("regions").size() == 2);
	CHECK(f.call("history.get").at("steps").back().at("action") == "Detect silence");
	const Json project = f.call("project.get");
	REQUIRE(project.at("audioEvents").size() == 2);
	const Json first = project.at("audioEvents").at(0);
	CHECK(std::abs(first.at("offset").get<int64_t>() - at(0.5)) <= 48);
	CHECK(std::abs(first.at("length").get<int64_t>() - at(1.0)) <= 96);
	CHECK(first.at("fadeIn").at("length") == 240);
	CHECK(first.at("fadeOut").at("length") == 240);
	const Json regions = project.at("clips").at(0).at("regions");
	REQUIRE(regions.size() == 2);
	CHECK(regions.at(0).at("name") == "Take 3");
	CHECK(regions.at(1).at("name") == "Take 4");
	CHECK(regions.at(1).at("start") == project.at("audioEvents").at(1).at("offset"));

	f.call("history.undo");
	CHECK(f.call("project.get").at("audioEvents").size() == 1);
	CHECK(f.call("project.get").at("clips").at(0).at("regions").empty());

	// Regions alone leave the event as it is.
	const Json marked = f.call("event.stripSilence", {{"events", {f.event}}, {"strip", false}, {"regions", true}});
	CHECK(marked.at("events") == Json{f.event});
	CHECK(f.call("project.get").at("clips").at(0).at("regions").at(0).at("name") == "Region 1");
	CHECK(f.call("project.get").at("audioEvents").size() == 1);

	f.add(make(1.0, 1, [](int, double) { return 0.0; }), "silence");
	CHECK_THAT(f.fails("event.stripSilence", {{"events", {f.event}}}), ContainsSubstring("is silent"));
	f.call("event.set", {{"events", {f.event}}, {"lock", true}});
	CHECK_THAT(f.fails("event.stripSilence", {{"events", {f.event}}}), ContainsSubstring("is locked"));
	CHECK_THAT(f.fails("event.stripSilence", {{"events", {999}}}), ContainsSubstring("no audio event 999"));
}

TEST_CASE("clip.statistics measures reference signals", "[unit]")
{
	Fixture f;

	SECTION("a sine: levels, RMS as AES17 has it, pitch and floating point samples")
	{
		const Json result = f.statistics(make(1.0, 1, [](int, double t) { return sine(1000.0, 0.5, t); }));
		CHECK(result.at("sampleRate") == g_rate);
		CHECK(result.at("frames") == g_rate);
		const Json channel = result.at("channels").at(0);
		CHECK(number(channel.at("maxSample")) == Approx(0.5).margin(1e-4));
		CHECK(number(channel.at("minSample")) == Approx(-0.5).margin(1e-4));
		CHECK(number(channel.at("peakDb")) == Approx(-6.02).margin(0.01));
		CHECK(number(channel.at("minSampleDb")) == Approx(-6.02).margin(0.01));
		CHECK(number(channel.at("truePeakDb")) == Approx(-6.02).margin(0.05));
		CHECK(number(channel.at("averageRms")) == Approx(-6.02).margin(0.01));
		CHECK(number(channel.at("maxRms")) == Approx(-6.02).margin(0.01));
		CHECK(number(result.at("maxRmsAllChannels")) == Approx(-6.02).margin(0.01));
		CHECK(number(channel.at("pitch")) == Approx(1000.0).margin(1.0));
		CHECK(std::abs(number(channel.at("dcOffset"))) < 1e-6);
		CHECK(channel.at("bitDepth") == 32);
	}

	SECTION("between the samples: a sine at a quarter of the sample rate, its samples 3 dB below its peak")
	{
		const Json channel = f.statistics(make(1.0, 1, [](int, double t)
											  { return std::sin((2.0 * g_pi * 12000.0 * t) + (g_pi / 4.0)); }))
								 .at("channels")
								 .at(0);
		CHECK(number(channel.at("peakDb")) == Approx(-3.01).margin(0.01));
		// Within the tolerance of EBU Tech 3341's true peak tests.
		CHECK(number(channel.at("truePeakDb")) >= -0.4);
		CHECK(number(channel.at("truePeakDb")) <= 0.2);
	}

	SECTION("DC offset, bit depth and maximum RMS")
	{
		// 16-bit samples with an offset of 0.1, and a louder 100 ms in the right channel.
		const Json result = f.statistics(make(1.0, 2,
			[](int channel, double t)
			{
				double value = 0.1 + sine(100.0, 0.2, t);
				if (channel == 1)
					value = t >= 0.5 && t < 0.6 ? sine(100.0, 0.8, t) : sine(100.0, 0.1, t);
				return std::round(value * 32768.0) / 32768.0;
			}));
		const Json left = result.at("channels").at(0);
		CHECK(number(left.at("dcOffset")) == Approx(0.1).margin(1e-4));
		CHECK(number(left.at("dcOffsetPercent")) == Approx(10.0).margin(0.01));
		CHECK(number(left.at("dcOffsetDb")) == Approx(-20.0).margin(0.01));
		CHECK(left.at("bitDepth") == 16);
		const Json right = result.at("channels").at(1);
		CHECK(number(right.at("maxRms")) == Approx(20.0 * std::log10(0.8)).margin(0.05));
		CHECK(number(result.at("maxRmsAllChannels")) == Approx(20.0 * std::log10(0.8)).margin(0.05));
		CHECK(number(right.at("averageRms")) < number(right.at("maxRms")) - 5.0);

		const Json eight =
			f.statistics(make(0.1, 1, [](int, double t) { return std::round(sine(50.0, 0.5, t) * 128.0) / 128.0; }));
		CHECK(eight.at("channels").at(0).at("bitDepth") == 8);
	}

	SECTION("loudness of EBU Tech 3341 and 3342's test signals: a stereo 1 kHz sine at -23 dBFS, and at -20 and then "
			"-30 dBFS")
	{
		const double level = std::pow(10.0, -23.0 / 20.0);
		Json loudness =
			f.statistics(make(10.0, 2, [level](int, double t) { return sine(1000.0, level, t); })).at("loudness");
		CHECK(number(loudness.at("integrated")) == Approx(-23.0).margin(0.1));
		CHECK(number(loudness.at("maxMomentary")) == Approx(-23.0).margin(0.1));
		CHECK(number(loudness.at("maxShortTerm")) == Approx(-23.0).margin(0.1));
		CHECK(number(loudness.at("range")) == Approx(0.0).margin(0.2));
		CHECK(number(loudness.at("maxTruePeak")) == Approx(-23.0).margin(0.05));

		const double loud = std::pow(10.0, -20.0 / 20.0);
		const double quiet = std::pow(10.0, -30.0 / 20.0);
		loudness = f.statistics(make(20.0, 2, [&](int, double t) { return sine(1000.0, t < 10.0 ? loud : quiet, t); }))
					   .at("loudness");
		CHECK(number(loudness.at("range")) == Approx(10.0).margin(1.0));
		CHECK(number(loudness.at("maxMomentary")) == Approx(-20.0).margin(0.1));
	}

	SECTION("silence and a range of the clip")
	{
		const Json result =
			f.statistics(make(1.0, 1, [](int, double t) { return t < 0.5 ? 0.0 : sine(1000.0, 0.5, t); }));
		const Json silent = f.call("clip.statistics", {{"clip", f.clip}, {"end", g_rate / 2}});
		CHECK(silent.at("frames") == g_rate / 2);
		const Json channel = silent.at("channels").at(0);
		for (const char* name :
			{"peakDb", "truePeakDb", "minSampleDb", "maxSampleDb", "dcOffsetDb", "pitch", "averageRms", "maxRms"})
		{
			INFO(name);
			CHECK(channel.at(name).is_null());
		}
		CHECK(channel.at("bitDepth") == 0);
		for (const auto& [name, value] : silent.at("loudness").items())
		{
			INFO(name);
			CHECK(value.is_null());
		}
		CHECK(number(result.at("channels").at(0).at("peakDb")) == Approx(-6.02).margin(0.01));
		CHECK_THAT(
			f.fails("clip.statistics", {{"clip", f.clip}, {"end", 2 * g_rate}}), ContainsSubstring("within the clip"));
		CHECK_THAT(f.fails("clip.statistics", {{"clip", 999}}), ContainsSubstring("no clip 999"));
	}
}

TEST_CASE("clip.spectrum shows the levels of a signal's frequencies", "[unit]")
{
	Fixture f;
	const int64_t clip =
		f.add(make(1.0, 2, [](int channel, double t)
				  { return channel == 0 ? sine(1000.0, 0.5, t) + sine(5000.0, 0.1, t) : sine(250.0, 0.25, t); }),
			"spectrum");
	const Json result = f.call("clip.spectrum", {{"clip", clip}});
	const Json frequencies = result.at("frequencies");
	REQUIRE(frequencies.size() > 200);
	CHECK(number(frequencies.at(0)) == Approx(20.0));
	CHECK(number(frequencies.back()) <= 24000.0);
	CHECK(number(frequencies.at(24)) == Approx(40.0)); // 1/24 octave apart
	REQUIRE(result.at("channels").size() == 2);

	// The highest level within 1/24 octave of a frequency: a sine shows at the point next to it.
	const auto levelAt = [&](size_t channel, double frequency)
	{
		double highest = -999.0;
		for (size_t i = 0; i < frequencies.size(); ++i)
		{
			const Json& level = result.at("channels").at(channel).at("levels").at(i);
			if (std::abs(std::log2(number(frequencies.at(i)) / frequency)) <= 1.0 / 24.0 && !level.is_null())
				highest = std::max(highest, level.get<double>());
		}
		return highest;
	};
	CHECK(levelAt(0, 1000.0) == Approx(-6.02).margin(1.0));
	CHECK(levelAt(0, 5000.0) == Approx(-20.0).margin(1.0));
	CHECK(levelAt(0, 2500.0) < -90.0);
	CHECK(levelAt(1, 250.0) == Approx(-12.04).margin(1.0));
	CHECK(levelAt(1, 1000.0) < -90.0);

	// A short range is one zero-padded window, and silence has no levels.
	const Json silent =
		f.call("clip.spectrum", {{"clip", f.add(make(0.05, 1, [](int, double) { return 0.0; }), "silent")}});
	for (const Json& level : silent.at("channels").at(0).at("levels"))
		CHECK(level.is_null());
}
