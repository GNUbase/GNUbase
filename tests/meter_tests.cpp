// SPDX-License-Identifier: GPL-3.0-or-later
// Metering and loudness (#111, reference manual pp. 533-540): level meters, the master meter's true peak and the
// EBU R 128 loudness meter, against the reference values of EBU Tech 3341 and 3342.
#include "engine/analysis/analysis.h"
#include "engine/audio/meters.h"
#include "engine/document/document.h"
#include "engine/engine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;
using gnubase::engine::LevelMeter;
using gnubase::engine::LevelReading;
using gnubase::engine::LoudnessMeter;
using gnubase::engine::LoudnessReading;
using gnubase::engine::ObjectId;
using gnubase::engine::Result;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;
	constexpr double g_rate = 48000.0;
	constexpr uint32_t g_block = 256;

	double fromDecibels(double db)
	{
		return std::pow(10.0, db / 20.0);
	}

	double decibels(double level)
	{
		return 20.0 * std::log10(level);
	}

	// A stereo signal, both channels alike, made of parts: seconds of a 1 kHz sine at a level in dBFS each.
	class Signal
	{
	public:
		void sine(double seconds, double dbfs, double frequency = 1000.0, double phase = 0.0)
		{
			const auto frames = static_cast<size_t>(std::lround(seconds * g_rate));
			const double amplitude = fromDecibels(dbfs);
			for (size_t i = 0; i < frames; ++i, ++m_frame)
				m_samples.push_back(static_cast<float>(
					amplitude * std::sin((2.0 * g_pi * frequency * static_cast<double>(m_frame) / g_rate) + phase)));
		}

		// Drops the samples so far; what comes next goes on from there.
		void clear() { m_samples.clear(); }

		void silence(double seconds) { m_samples.resize(m_samples.size() + static_cast<size_t>(seconds * g_rate)); }

		// Feeds the signal to a meter block by block.
		template <class Meter> void play(Meter& meter, bool measuring = true) const
		{
			std::vector<float> block(g_block);
			for (size_t start = 0; start < m_samples.size(); start += g_block)
			{
				const size_t frames = std::min<size_t>(g_block, m_samples.size() - start);
				std::copy_n(m_samples.begin() + static_cast<std::ptrdiff_t>(start), frames, block.begin());
				const std::array<const float*, 2> audio{block.data(), block.data()};
				if constexpr (std::is_same_v<Meter, LoudnessMeter>)
					meter.process(audio.data(), 2, static_cast<uint32_t>(frames), measuring);
				else
					meter.process(audio.data(), 2, static_cast<uint32_t>(frames), g_rate);
			}
		}

		const std::vector<float>& samples() const { return m_samples; }

	private:
		std::vector<float> m_samples;
		int64_t m_frame = 0;
	};

	LoudnessReading measure(const Signal& signal)
	{
		LoudnessMeter meter;
		meter.prepare(g_rate);
		signal.play(meter);
		return meter.read();
	}

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}
}

TEST_CASE("a level meter reads a sine's peak and RMS level and keeps their maxima", "[unit]")
{
	LevelMeter meter;
	Signal loud;
	loud.sine(1.0, -6.0);
	loud.play(meter);
	LevelReading reading = meter.read();
	REQUIRE(reading.channels == 2);
	CHECK(decibels(reading.peak[0]) == Approx(-6.0).margin(0.01));
	CHECK(decibels(reading.rms[0]) == Approx(-9.01).margin(0.05)); // a sine's RMS is 3.01 dB below its peak
	CHECK(decibels(reading.peakMax[1]) == Approx(-6.0).margin(0.01));

	// Quieter: the levels follow, the maxima stay until a reset.
	Signal quiet;
	quiet.sine(2.0, -20.0);
	quiet.play(meter);
	reading = meter.read();
	CHECK(decibels(reading.peak[0]) == Approx(-20.0).margin(0.01));
	CHECK(decibels(reading.rms[0]) == Approx(-23.01).margin(0.05));
	CHECK(decibels(reading.peakMax[0]) == Approx(-6.0).margin(0.01));
	CHECK(decibels(reading.rmsMax[0]) == Approx(-9.01).margin(0.05));
	meter.reset();
	CHECK(meter.read().peakMax[0] == 0.0f);
	quiet.play(meter);
	CHECK(decibels(meter.read().peakMax[0]) == Approx(-20.0).margin(0.01));
}

TEST_CASE("the loudness meter reads EBU Tech 3341's steady sines exactly", "[unit]")
{
	// Cases 1 and 2: a stereo 1 kHz sine at -23 and -33 dBFS reads -23 and -33 LUFS, within 0.1 LU.
	const double level = GENERATE(-23.0, -33.0);
	Signal signal;
	signal.sine(20.0, level);
	const LoudnessReading reading = measure(signal);
	REQUIRE(reading.momentary.has_value());
	REQUIRE(reading.shortTerm.has_value());
	REQUIRE(reading.integrated.has_value());
	CHECK(reading.momentary.value_or(0.0) == Approx(level).margin(0.1));
	CHECK(reading.shortTerm.value_or(0.0) == Approx(level).margin(0.1));
	CHECK(reading.integrated.value_or(0.0) == Approx(level).margin(0.1));
	CHECK(reading.momentaryMax.value_or(0.0) == Approx(level).margin(0.1));
	CHECK(reading.shortTermMax.value_or(0.0) == Approx(level).margin(0.1));
	CHECK(reading.seconds == Approx(20.0));
}

TEST_CASE("the integrated loudness gates quiet passages as EBU Tech 3341 says", "[unit][slow]")
{
	// Case 3: -36, -23 and -36 dBFS for 10, 60 and 10 s; case 4 adds -72 dBFS around it, below the absolute gate.
	Signal three;
	three.sine(10.0, -36.0);
	three.sine(60.0, -23.0);
	three.sine(10.0, -36.0);
	CHECK(measure(three).integrated.value_or(0.0) == Approx(-23.0).margin(0.1));

	Signal four;
	four.sine(10.0, -72.0);
	four.sine(10.0, -36.0);
	four.sine(60.0, -23.0);
	four.sine(10.0, -36.0);
	four.sine(10.0, -72.0);
	CHECK(measure(four).integrated.value_or(0.0) == Approx(-23.0).margin(0.1));
}

TEST_CASE("the loudness range reads EBU Tech 3342's test signals", "[unit][slow]")
{
	// Cases 1 to 3: 20 s at one level, then 20 s at another: the range is their difference, within 1 LU.
	const auto [first, second, expected] =
		GENERATE(std::tuple{-20.0, -30.0, 10.0}, std::tuple{-20.0, -15.0, 5.0}, std::tuple{-40.0, -20.0, 20.0});
	Signal signal;
	signal.sine(20.0, first);
	signal.sine(20.0, second);
	const LoudnessReading reading = measure(signal);
	REQUIRE(reading.range.has_value());
	CHECK(reading.range.value_or(0.0) == Approx(expected).margin(1.0));
}

TEST_CASE("the integrated loudness matches the offline measurement for noise", "[unit]")
{
	const uint32_t seed = GENERATE(7U, 8U);
	std::mt19937 random(seed);
	std::normal_distribution<float> noise(0.0f, 0.1f);
	std::vector<float> samples(static_cast<size_t>(10 * g_rate));
	for (float& sample : samples)
		sample = noise(random);
	LoudnessMeter meter;
	meter.prepare(g_rate);
	for (size_t start = 0; start < samples.size(); start += g_block)
	{
		const std::array<const float*, 2> audio{samples.data() + start, samples.data() + start};
		meter.process(audio.data(), 2, g_block, true);
	}
	const std::optional<double> offline = gnubase::engine::analysis::integratedLoudness({samples, samples}, g_rate);
	REQUIRE(offline.has_value());
	CHECK(meter.read().integrated.value_or(0.0) == Approx(offline.value_or(0.0)).margin(0.1));
}

TEST_CASE("the true peak finds the peaks between the samples", "[unit]")
{
	// A sine at a quarter of the sample rate, 45 degrees off: every sample is 3 dB below its peaks. The meter
	// measures once the sine runs, past the overshoot of its sudden start.
	Signal signal;
	signal.sine(0.5, -1.0, g_rate / 4.0, g_pi / 4.0);
	LoudnessMeter meter;
	meter.prepare(g_rate);
	signal.play(meter, false);
	LevelMeter samplePeak;
	signal.clear();
	signal.sine(1.0, -1.0, g_rate / 4.0, g_pi / 4.0);
	signal.play(meter);
	signal.play(samplePeak);
	const LoudnessReading reading = meter.read();
	REQUIRE(reading.channels == 2);
	CHECK(decibels(reading.truePeak[0]) == Approx(-1.0).margin(0.05));
	CHECK(decibels(reading.truePeakMax[1]) == Approx(-1.0).margin(0.05));
	CHECK(decibels(samplePeak.read().peak[0]) == Approx(-4.01).margin(0.05));
}

TEST_CASE("the loudness meter measures only while asked, and a reset starts over", "[unit]")
{
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal signal;
	signal.sine(5.0, -23.0);
	signal.play(meter, false); // stopped: momentary and short-term loudness follow, nothing accumulates
	LoudnessReading reading = meter.read();
	CHECK(reading.momentary.value_or(0.0) == Approx(-23.0).margin(0.1));
	CHECK(!reading.integrated);
	CHECK(!reading.momentaryMax);
	CHECK(reading.truePeakMax[0] == 0.0f);
	CHECK(reading.seconds == 0.0);

	signal.play(meter);
	CHECK(meter.read().integrated.value_or(0.0) == Approx(-23.0).margin(0.1));
	meter.reset();
	reading = meter.read();
	CHECK(!reading.integrated);
	CHECK(reading.seconds == 0.0);
	Signal quiet;
	quiet.sine(5.0, -40.0);
	quiet.play(meter);
	CHECK(meter.read().integrated.value_or(0.0) == Approx(-40.0).margin(0.1));
	CHECK(meter.read().seconds == Approx(5.0));
}

TEST_CASE("silence has no loudness", "[unit]")
{
	Signal silence;
	silence.silence(5.0);
	const LoudnessReading reading = measure(silence);
	CHECK(!reading.momentary);
	CHECK(!reading.shortTerm);
	CHECK(!reading.integrated);
	CHECK(!reading.range);
}

TEST_CASE("meter.get reads a channel, the main mix and its loudness through the dummy device", "[integration]")
{
	// A 1 kHz sine at -6 dBFS on the inputs, monitored by a record-enabled audio track while the transport stands.
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const Json added = run(client, "track.add", {{"type", "audio"}});
	const auto channel = added.at("channel").at("id").get<ObjectId>();
	run(client, "settings.set", {{"key", "audio.autoMonitoring"}, {"value", "whileRecordEnabled"}}); // playing too
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}});
	run(client, "audio.render", {{"frames", 48000}});

	Json meters = run(client, "meter.get", {{"channels", {channel}}});
	REQUIRE(meters.at("channels").size() == 1);
	const Json& strip = meters.at("channels").at(0);
	CHECK(strip.at("channel") == channel);
	REQUIRE(!strip.at("peak").empty());
	CHECK(strip.at("peak").at(0).get<double>() == Approx(-6.02).margin(0.05));
	CHECK(strip.at("rms").at(0).get<double>() == Approx(-9.03).margin(0.1));
	const Json& main = meters.at("main");
	REQUIRE(main.at("peak").size() == 2);
	CHECK(main.at("peak").at(1).get<double>() == Approx(-6.02).margin(0.05));
	CHECK(main.at("truePeak").at(0).get<double>() == Approx(-6.02).margin(0.1));
	// Both channels at -6 dBFS: as EBU Tech 3341's sines, the momentary loudness is their level.
	const Json& loudness = meters.at("loudness");
	CHECK(loudness.at("momentary").get<double>() == Approx(-6.02).margin(0.1));
	CHECK(loudness.at("shortTerm").is_null());  // only 1 s so far
	CHECK(loudness.at("integrated").is_null()); // stopped: nothing measured
	CHECK(loudness.at("seconds") == 0.0);

	// Playing, it measures.
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 4 * 48000}});
	meters = run(client, "meter.get");
	CHECK(meters.at("loudness").at("integrated").get<double>() == Approx(-6.02).margin(0.1));
	CHECK(meters.at("loudness").at("shortTermMax").get<double>() == Approx(-6.02).margin(0.1));
	CHECK(meters.at("loudness").at("seconds").get<double>() == Approx(4.0).margin(0.1));
	CHECK(meters.at("main").at("truePeakMax").at(0).get<double>() == Approx(-6.02).margin(0.1));

	// A reset starts over; with meter.resetOnStart every start does.
	run(client, "meter.reset");
	meters = run(client, "meter.get");
	CHECK(meters.at("loudness").at("integrated").is_null());
	CHECK(meters.at("main").at("peakMax").at(0).is_null());
	CHECK(meters.at("channels").at(0).at("rmsMax").at(0).is_null());
	run(client, "audio.render", {{"frames", 48000}});
	CHECK(run(client, "meter.get").at("loudness").at("seconds").get<double>() == Approx(1.0).margin(0.1));
	run(client, "settings.set", {{"key", "meter.resetOnStart"}, {"value", true}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 4800}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 24000}});
	CHECK(run(client, "meter.get").at("loudness").at("seconds").get<double>() == Approx(0.5).margin(0.1));

	CHECK_THAT(reject(client, "meter.get", {{"channels", {999}}}), ContainsSubstring("999"));
}

TEST_CASE("a mono channel's meter reads both sides of its panner, and muting silences both", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const Json added = run(client, "track.add", {{"type", "audio"}, {"channels", 1}});
	const auto channel = added.at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}, {"pan", 1.0}}); // hard right
	run(client, "audio.render", {{"frames", 9600}});
	Json strip = run(client, "meter.get", {{"channels", {channel}}}).at("channels").at(0);
	REQUIRE(strip.at("peak").size() == 2);
	CHECK(strip.at("peak").at(0).is_null());
	// The -3 dB pan law: hard right is 3 dB above the centre, which is the input's -6 dBFS.
	CHECK(strip.at("peak").at(1).get<double>() == Approx(-3.01).margin(0.05));

	run(client, "channel.set", {{"channel", channel}, {"mute", true}});
	run(client, "audio.render", {{"frames", 9600}});
	strip = run(client, "meter.get", {{"channels", {channel}}}).at("channels").at(0);
	CHECK(strip.at("peak").at(0).is_null());
	CHECK(strip.at("peak").at(1).is_null());
	CHECK(run(client, "meter.get").at("main").at("peak").at(1).is_null());
}

TEST_CASE("a level meter channel that comes back starts without what it measured before", "[unit]")
{
	LevelMeter meter;
	Signal loud;
	loud.sine(1.0, -6.0);
	loud.play(meter); // stereo
	const std::vector<float> mono(g_block, 0.0f);
	for (int block = 0; block < 100; ++block) // mono for a while: the second channel goes away
	{
		const std::array<const float*, 1> audio{mono.data()};
		meter.process(audio.data(), 1, g_block, g_rate);
	}
	meter.reset();
	// It comes back silent: no old peak, RMS or maximum, also before it publishes again.
	{
		const std::array<const float*, 2> audio{mono.data(), mono.data()};
		meter.process(audio.data(), 2, g_block, g_rate);
		const LevelReading early = meter.read();
		CHECK(early.peak[1] == 0.0f);
		CHECK(early.rms[1] == 0.0f);
		CHECK(early.peakMax[1] == 0.0f);
		CHECK(early.rmsMax[1] == 0.0f);
	}
	const std::vector<float> silence(g_block, 0.0f);
	for (int block = 0; block < 20; ++block)
	{
		const std::array<const float*, 2> audio{silence.data(), silence.data()};
		meter.process(audio.data(), 2, g_block, g_rate);
	}
	const LevelReading reading = meter.read();
	REQUIRE(reading.channels == 2);
	CHECK(reading.peak[1] == 0.0f);
	CHECK(reading.rms[1] == 0.0f);
	CHECK(reading.peakMax[1] == 0.0f);
	CHECK(reading.rmsMax[1] == 0.0f);
}

TEST_CASE("the loudness blocks start where playback starts, not with audio from while it stood", "[unit]")
{
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal quiet;
	quiet.sine(5.0, -30.0);
	Signal loud;
	loud.sine(GENERATE(3.0, 3.05), -10.0); // also ending off the 100 ms grid
	quiet.play(meter);
	loud.play(meter, false); // stopped
	quiet.play(meter);
	const LoudnessReading reading = meter.read();
	CHECK(reading.integrated.value_or(0.0) == Approx(-30.0).margin(0.1));
	CHECK(reading.momentaryMax.value_or(0.0) == Approx(-30.0).margin(0.1));
	CHECK(reading.seconds == Approx(10.0).margin(0.1));
}

TEST_CASE("the loudness meter reads a steady sine at 44.1 kHz too", "[unit]")
{
	constexpr double rate = 44100.0;
	LoudnessMeter meter;
	meter.prepare(rate);
	const double amplitude = fromDecibels(-23.0);
	std::vector<float> block(g_block);
	for (int64_t frame = 0; frame < static_cast<int64_t>(10 * rate); frame += g_block)
	{
		for (size_t i = 0; i < block.size(); ++i)
			block[i] =
				static_cast<float>(amplitude * std::sin(2.0 * g_pi * 1000.0 * static_cast<double>(frame + i) / rate));
		const std::array<const float*, 2> audio{block.data(), block.data()};
		meter.process(audio.data(), 2, g_block, true);
	}
	CHECK(meter.read().integrated.value_or(0.0) == Approx(-23.0).margin(0.1));
	CHECK(decibels(meter.read().truePeakMax[0]) == Approx(-23.0).margin(0.1)); // the level is the peak
}

TEST_CASE("meter.get lists the channels asked for, also before they played", "[integration]")
{
	Engine engine; // no device: nothing played
	Client client(engine);
	const auto channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	Json meters = run(client, "meter.get", {{"channels", {channel}}});
	REQUIRE(meters.at("channels").size() == 1);
	CHECK(meters.at("channels").at(0).at("peak").empty());
	CHECK(run(client, "meter.get", {{"channels", Json::array()}}).at("channels").empty());
	CHECK(run(client, "meter.get").at("channels").size() == 1);
}
