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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
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
using gnubase::engine::MeterFrame;
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
		explicit Signal(double rate = g_rate) : m_rate(rate) {}

		void sine(double seconds, double dbfs, double frequency = 1000.0, double phase = 0.0)
		{
			const auto frames = static_cast<size_t>(std::lround(seconds * m_rate));
			const double amplitude = fromDecibels(dbfs);
			for (size_t i = 0; i < frames; ++i, ++m_frame)
				m_samples.push_back(static_cast<float>(
					amplitude * std::sin((2.0 * g_pi * frequency * static_cast<double>(m_frame) / m_rate) + phase)));
		}

		// Drops the samples so far; what comes next goes on from there.
		void clear() { m_samples.clear(); }

		void silence(double seconds)
		{
			const auto frames = static_cast<size_t>(std::lround(seconds * m_rate));
			m_samples.resize(m_samples.size() + frames);
			m_frame += static_cast<int64_t>(frames);
		}

		// One sample at a level, as part of the signal.
		void impulse(float level)
		{
			m_samples.push_back(level);
			++m_frame;
		}

		// Feeds the signal to a meter block by block.
		template <class Meter> void play(Meter& meter, bool measuring = true, uint32_t blockSize = g_block) const
		{
			std::vector<float> block(blockSize);
			for (size_t start = 0; start < m_samples.size(); start += blockSize)
			{
				const size_t frames = std::min<size_t>(blockSize, m_samples.size() - start);
				std::copy_n(m_samples.begin() + static_cast<std::ptrdiff_t>(start), frames, block.begin());
				const std::array<const float*, 2> audio{block.data(), block.data()};
				if constexpr (std::is_same_v<Meter, LoudnessMeter>)
					meter.process(audio.data(), 2, static_cast<uint32_t>(frames), measuring);
				else
					meter.process(audio.data(), 2, static_cast<uint32_t>(frames), m_rate);
			}
		}

		const std::vector<float>& samples() const { return m_samples; }

	private:
		double m_rate;
		std::vector<float> m_samples;
		int64_t m_frame = 0;
	};

	LoudnessReading measure(const Signal& signal, double rate = g_rate)
	{
		LoudnessMeter meter;
		meter.prepare(rate);
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

	// Waits until the audio device is lost, or isn't any more.
	bool waitForLost(Client& client, bool lost)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (run(client, "audio.get").at("lost").is_null() != lost)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}

	// A sine on the dummy device's input, monitored by a record-enabled audio track, with the transport playing, so
	// that audio.render adds to the loudness measurement. Returns the channel.
	ObjectId measuring(Client& client)
	{
		run(client, "audio.setDevice",
			{{"blockSize", 64},
				{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
		const auto channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
		run(client, "settings.set", {{"key", "audio.autoMonitoring"}, {"value", "whileRecordEnabled"}}); // playing too
		run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}});
		run(client, "transport.play");
		return channel;
	}

	// Ends a thread that runs until done, also when a test leaves with a failed REQUIRE.
	class StopAndJoin
	{
	public:
		StopAndJoin(std::atomic<bool>& done, std::thread& thread) : m_done(done), m_thread(thread) {}
		~StopAndJoin()
		{
			m_done = true;
			if (m_thread.joinable())
				m_thread.join();
		}
		StopAndJoin(const StopAndJoin&) = delete;
		StopAndJoin& operator=(const StopAndJoin&) = delete;

	private:
		std::atomic<bool>& m_done;
		std::thread& m_thread;
	};
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
	CHECK(decibels(reading.truePeakMax[0]) == Approx(-30.0).margin(0.1));
	CHECK(reading.seconds == Approx(10.0).margin(0.1));
}

TEST_CASE("the loudness meter reads a steady sine at other sample rates too", "[unit]")
{
	const double rate = GENERATE(44100.0, 96000.0);
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

TEST_CASE("the loudness meter reads EBU Tech 3341's changing levels", "[unit]")
{
	// The meter's reading after each block, from a time on, all within 0.1 LU of -23 LUFS.
	const auto steady = [](const Signal& signal, double from, bool shortTerm)
	{
		LoudnessMeter meter;
		meter.prepare(g_rate);
		const std::vector<float>& samples = signal.samples();
		for (size_t start = 0; start + g_block <= samples.size(); start += g_block)
		{
			const std::array<const float*, 2> audio{samples.data() + start, samples.data() + start};
			meter.process(audio.data(), 2, g_block, true);
			if (static_cast<double>(start) < from * g_rate)
				continue;
			const LoudnessReading reading = meter.read();
			INFO("at " << static_cast<double>(start) / g_rate << " s");
			CHECK((shortTerm ? reading.shortTerm : reading.momentary).value_or(0.0) == Approx(-23.0).margin(0.1));
		}
	};
	SECTION("case 9: the short-term loudness of 1.34 s at -20 and 1.66 s at -30 dBFS, after 3 s")
	{
		Signal signal;
		for (int period = 0; period < 4; ++period)
		{
			signal.sine(1.34, -20.0);
			signal.sine(1.66, -30.0);
		}
		steady(signal, 3.1, true);
	}
	SECTION("case 12: the momentary loudness of 0.18 s at -20 and 0.22 s at -30 dBFS, after 1 s")
	{
		Signal signal;
		for (int period = 0; period < 25; ++period)
		{
			signal.sine(0.18, -20.0);
			signal.sine(0.22, -30.0);
		}
		steady(signal, 1.1, false);
	}
}

TEST_CASE("the integrated loudness of EBU Tech 3341's case 5 is -23 LUFS", "[unit][slow]")
{
	// -26, -20 and -26 dBFS for 20, 20.1 and 20 s.
	Signal signal;
	signal.sine(20.0, -26.0);
	signal.sine(20.1, -20.0);
	signal.sine(20.0, -26.0);
	CHECK(measure(signal).integrated.value_or(0.0) == Approx(-23.0).margin(0.1));
}

TEST_CASE("the true peak of sines between the samples is within EBU Tech 3341's tolerance", "[unit]")
{
	// Sines at a quarter, a sixth and an eighth of the sample rate, with phases that put their peaks as far between
	// the samples as they get, at -6 dBTP and, with samples up to 0 dBFS, at +3 dBTP: within +0.2 and -0.4 dB.
	const double rate = GENERATE(44100.0, 48000.0, 96000.0);
	const auto [divisor, phase, level, samples] =
		GENERATE(std::tuple{4.0, g_pi / 4.0, -6.0, -9.01}, std::tuple{6.0, g_pi / 3.0, -6.0, -7.25},
			std::tuple{8.0, g_pi / 8.0, -6.0, -6.69}, std::tuple{4.0, g_pi / 4.0, 3.01, 0.0});
	INFO(rate << " Hz, a sine at 1/" << divisor << " of it, " << level << " dBTP");
	LoudnessMeter meter;
	meter.prepare(rate);
	Signal signal(rate);
	signal.sine(0.5, level, rate / divisor, phase);
	signal.play(meter, false); // past the overshoot of its sudden start
	signal.clear();
	signal.sine(1.0, level, rate / divisor, phase);
	signal.play(meter);
	const double measured = decibels(meter.read().truePeakMax[0]);
	CHECK(measured <= level + 0.2);
	CHECK(measured >= level - 0.4);
	// The samples themselves stay below it: only the interpolator finds that peak.
	LevelMeter samplePeak;
	signal.play(samplePeak);
	CHECK(decibels(samplePeak.read().peakMax[0]) == Approx(samples).margin(0.02));
}

TEST_CASE("the master meter's RMS level follows its resolution and AES17", "[unit]")
{
	// A second of silence, 100 ms of a sine at -6 dBFS, then 20 ms of silence.
	Signal signal;
	signal.silence(1.0);
	signal.sine(0.1, -6.0);
	signal.silence(0.02);
	const double sineRms = -6.0 - 3.0103;

	LevelMeter slow;
	slow.setRms(1000, false);
	signal.play(slow);
	// The last second holds the whole sine: a tenth of its mean square.
	CHECK(decibels(slow.read().rms[0]) == Approx(sineRms - 10.0).margin(0.05));
	CHECK(decibels(slow.read().rmsMax[0]) == Approx(sineRms - 10.0).margin(0.05));

	LevelMeter fast;
	fast.setRms(10, false);
	signal.play(fast);
	CHECK(fast.read().rms[0] == 0.0f); // the last 10 ms are silent
	CHECK(decibels(fast.read().rmsMax[0]) == Approx(sineRms).margin(0.05));
	CHECK(decibels(fast.read().peakMax[0]) == Approx(-6.0).margin(0.01));

	// AES17 adds 3 dB, also to the maximum, so that a sine's RMS level is its peak level.
	fast.setRms(10, true);
	CHECK(decibels(fast.read().rmsMax[0]) == Approx(-6.0).margin(0.02));
	CHECK(decibels(fast.readFrame().rmsMax[1]) == Approx(-6.0).margin(0.02));
	CHECK(decibels(fast.read().peakMax[0]) == Approx(-6.0).margin(0.01)); // peaks stay

	// A new resolution while it runs: the window starts again at that length.
	slow.setRms(10, false);
	Signal more;
	more.sine(0.05, -6.0);
	more.play(slow);
	CHECK(decibels(slow.read().rms[0]) == Approx(sineRms).margin(0.05));

	// Resolutions out of range are clamped: a millisecond still reads a steady sine, a second at most.
	LevelMeter shortest;
	shortest.setRms(0, false);
	Signal steady;
	steady.sine(0.5, -6.0);
	steady.play(shortest);
	CHECK(decibels(shortest.read().rms[0]) == Approx(sineRms).margin(0.2));
	LevelMeter longest;
	longest.setRms(5000, false);
	Signal half;
	half.sine(1.0, -6.0);
	half.silence(0.5);
	half.play(longest);
	CHECK(decibels(longest.read().rms[0]) == Approx(sineRms - 3.0103).margin(0.05)); // the last second, half sine
}

TEST_CASE("the per-frame reading holds each peak until it is read", "[unit]")
{
	// A single loud sample, then half a second of silence: the 100 ms peak has moved on, the frame's peak hasn't.
	Signal signal;
	signal.silence(0.01);
	signal.impulse(0.8f);
	signal.silence(0.5);
	LevelMeter level;
	signal.play(level);
	CHECK(level.read().peak[0] == 0.0f);
	LevelReading frame = level.readFrame();
	REQUIRE(frame.channels == 2);
	CHECK(frame.peak[0] == Approx(0.8f));
	CHECK(frame.peak[1] == Approx(0.8f));
	CHECK(frame.peakMax[0] == Approx(0.8f));
	frame = level.readFrame(); // read: the next frame starts from nothing
	CHECK(frame.peak[0] == 0.0f);
	CHECK(frame.peakMax[0] == Approx(0.8f));

	LoudnessMeter loudness;
	loudness.prepare(g_rate);
	signal.play(loudness, false);
	CHECK(loudness.read().truePeak[0] == 0.0f);
	CHECK(loudness.readFrame().truePeak[0] == Approx(0.8f).margin(0.001));
	CHECK(loudness.readFrame().truePeak[0] == 0.0f);

	// Read in the middle of a 100 ms step: the next frame holds only what came after.
	Signal spike;
	spike.impulse(0.8f);
	spike.silence(0.001);
	spike.play(loudness, false);
	CHECK(loudness.readFrame().truePeak[0] == Approx(0.8f).margin(0.001));
	Signal quiet;
	quiet.silence(0.002); // a block of the same step
	quiet.play(loudness, false);
	CHECK(loudness.readFrame().truePeak[0] == 0.0f);
	spike.play(level);
	CHECK(level.readFrame().peak[0] == Approx(0.8f));
	quiet.play(level);
	CHECK(level.readFrame().peak[0] == 0.0f);
}

TEST_CASE("the 100 ms peak doesn't depend on the block size", "[unit]")
{
	// A loud sample 100 frames before the first 100 ms window ends, then silence: only that window holds it, also
	// when a block spans the window's end.
	const uint32_t blockSize = GENERATE(1U, 17U, 256U, 4096U);
	INFO("blocks of " << blockSize);
	Signal signal;
	signal.silence(4700.0 / g_rate);
	signal.impulse(0.9f);
	signal.silence(4899.0 / g_rate); // to the end of the second window, at 9600 frames
	LevelMeter meter;
	signal.play(meter, true, blockSize);
	CHECK(meter.read().peak[0] == 0.0f);
	CHECK(meter.read().peakMax[0] == Approx(0.9f));
}

TEST_CASE("the loudness meter keeps its measurement through a restart at the same sample rate", "[unit]")
{
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal signal;
	signal.sine(5.0, -23.0);
	signal.play(meter);
	REQUIRE(meter.read().seconds == Approx(5.0));

	meter.prepare(g_rate, true); // a device reset
	LoudnessReading reading = meter.read();
	CHECK(reading.seconds == Approx(5.0));
	CHECK(reading.integrated.value_or(0.0) == Approx(-23.0).margin(0.1));
	CHECK(!reading.momentary); // follows the signal, which starts again
	Signal quieter;
	quieter.sine(5.0, -33.0);
	quieter.play(meter);
	reading = meter.read();
	CHECK(reading.seconds == Approx(10.0));
	CHECK(reading.momentaryMax.value_or(0.0) == Approx(-23.0).margin(0.1));
	CHECK(decibels(reading.truePeakMax[0]) == Approx(-23.0).margin(0.1));
	// Both halves count, the quieter one less: its blocks lie above the relative gate, 10 LU below the louder.
	CHECK(reading.integrated.value_or(0.0) == Approx(-25.6).margin(0.2));

	meter.prepare(44100.0, true); // another rate: the measurement starts over
	CHECK(meter.read().seconds == 0.0);
	CHECK(!meter.read().integrated);
	Signal again(44100.0);
	again.sine(1.0, -23.0);
	again.play(meter);
	REQUIRE(meter.read().seconds > 0.0);
	meter.prepare(44100.0); // not kept
	CHECK(meter.read().seconds == 0.0);
}

TEST_CASE("a device reset keeps the loudness measurement, a new device starts it over", "[integration]")
{
	Engine engine;
	Client client(engine);
	const Json device = {
		{"blockSize", 64}, {"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}};
	run(client, "audio.setDevice", device);
	const auto channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	run(client, "settings.set", {{"key", "audio.autoMonitoring"}, {"value", "whileRecordEnabled"}}); // playing too
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 2 * 48000}});
	run(client, "audio.reset");
	run(client, "audio.render", {{"frames", 48000}});
	const Json loudness = run(client, "meter.get").at("loudness");
	CHECK(loudness.at("seconds").get<double>() == Approx(3.0).margin(0.1));
	CHECK(loudness.at("integrated").get<double>() == Approx(-6.02).margin(0.1));

	run(client, "audio.setDevice", device);
	CHECK(run(client, "meter.get").at("loudness").at("seconds") == 0.0);
}

TEST_CASE("meter.rmsResolution and meter.rmsAes17 set the main mix's RMS level", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}});
	run(client, "settings.set", {{"key", "meter.rmsAes17"}, {"value", true}});
	run(client, "settings.set", {{"key", "meter.rmsResolution"}, {"value", 50}});
	run(client, "audio.render", {{"frames", 9600}});
	const Json meters = run(client, "meter.get", {{"channels", {channel}}});
	CHECK(meters.at("main").at("rms").at(0).get<double>() == Approx(-6.02).margin(0.1));
	// Only the main mix's: the channels keep 300 ms without the offset.
	CHECK(meters.at("channels").at(0).at("rms").at(0).get<double>() == Approx(-9.03).margin(0.1));
	reject(client, "settings.set", {{"key", "meter.rmsResolution"}, {"value", 0}});
	reject(client, "settings.set", {{"key", "meter.rmsResolution"}, {"value", 1001}});
}

TEST_CASE("Engine::readMeters reads every meter at once without a command", "[integration]")
{
	Engine engine;
	Client client(engine);
	MeterFrame frame;
	engine.readMeters(frame); // nothing yet
	CHECK(frame.levels.empty());
	CHECK(frame.main.channels == 0);

	run(client, "audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}});
	run(client, "audio.render", {{"frames", 48000}});

	frame.channels = {channel, 999};
	engine.readMeters(frame);
	REQUIRE(frame.levels.size() == 2);
	REQUIRE(frame.levels[0].channels == 2);
	CHECK(decibels(frame.levels[0].peak[0]) == Approx(-6.02).margin(0.05));
	CHECK(decibels(frame.levels[0].rms[1]) == Approx(-9.03).margin(0.1));
	CHECK(frame.levels[1].channels == 0); // no such channel
	REQUIRE(frame.main.channels == 2);
	CHECK(decibels(frame.main.peak[1]) == Approx(-6.02).margin(0.05));
	CHECK(frame.loudness.momentary.value_or(0.0) == Approx(-6.02).margin(0.1));
	CHECK(decibels(frame.loudness.truePeak[0]) == Approx(-6.02).margin(0.1));

	// Read: the peaks start from nothing until the device plays again; the RMS levels and maxima stay.
	engine.readMeters(frame);
	CHECK(frame.levels[0].peak[0] == 0.0f);
	CHECK(frame.main.peak[0] == 0.0f);
	CHECK(frame.loudness.truePeak[0] == 0.0f);
	CHECK(decibels(frame.levels[0].rms[0]) == Approx(-9.03).margin(0.1));
	CHECK(decibels(frame.main.peakMax[0]) == Approx(-6.02).margin(0.05));
	run(client, "audio.render", {{"frames", 64}});
	engine.readMeters(frame);
	CHECK(frame.levels[0].peak[0] > 0.0f); // one block of the sine

	frame.channels.clear();
	engine.readMeters(frame);
	CHECK(frame.levels.empty());
}

TEST_CASE("the RMS level averages its resolution at other sample rates too", "[unit]")
{
	const double rate = GENERATE(44100.0, 96000.0);
	Signal signal(rate);
	signal.silence(1.0);
	signal.sine(0.1, -6.0);
	signal.silence(0.02);
	const double sineRms = -6.0 - 3.0103;

	LevelMeter slow;
	slow.setRms(1000, false);
	signal.play(slow);
	CHECK(decibels(slow.read().rms[0]) == Approx(sineRms - 10.0).margin(0.05));

	LevelMeter fast;
	fast.setRms(10, false);
	signal.play(fast);
	CHECK(fast.read().rms[0] == 0.0f);
	CHECK(decibels(fast.read().rmsMax[0]) == Approx(sineRms).margin(0.05));
}

TEST_CASE("a frame's peak is never above its maximum, whatever the audio thread does meanwhile", "[unit]")
{
	// The audio thread raises the level with every block of a frame, as fast as it can, while a frame reads.
	LevelMeter meter;
	std::atomic<bool> done{false};
	std::atomic<int> above{0};
	std::thread reader(
		[&]
		{
			do
			{
				const LevelReading frame = meter.readFrame();
				if (frame.peak[0] > frame.peakMax[0])
					++above;
			} while (!done);
		});
	const StopAndJoin stop(done, reader);
	constexpr int blocks = 500000;
	for (int block = 1; block <= blocks; ++block)
	{
		const float sample = static_cast<float>(block) / blocks;
		const float* const channel = &sample;
		meter.process(&channel, 1, 1, g_rate);
	}
	done = true;
	reader.join();
	CHECK(above == 0);
}

TEST_CASE("Engine::readMeters gives a channel listed twice the same peak both times", "[integration]")
{
	// The inspector and the mixer can show one channel at once.
	Engine engine;
	Client client(engine);
	const ObjectId channel = measuring(client);
	run(client, "audio.render", {{"frames", 9600}});
	MeterFrame frame;
	frame.channels = {channel, 999, channel};
	engine.readMeters(frame);
	REQUIRE(frame.levels.size() == 3);
	REQUIRE(frame.levels[2].channels == 2);
	CHECK(decibels(frame.levels[0].peak[0]) == Approx(-6.02).margin(0.05));
	CHECK(frame.levels[2].peak[0] == frame.levels[0].peak[0]);
	CHECK(frame.levels[2].rms[1] == frame.levels[0].rms[1]);
	CHECK(frame.levels[1].channels == 0);
}

TEST_CASE(
	"Engine::readMeters runs on a thread of its own while the device runs and the graph is rebuilt", "[integration]")
{
	Engine engine;
	Client client(engine);
	const ObjectId channel = measuring(client);
	run(client, "audio.start");
	std::atomic<bool> done{false};
	std::atomic<bool> complete{true};
	std::atomic<int> reads{0};
	std::thread reader(
		[&]
		{
			MeterFrame frame;
			frame.channels = {channel, 999};
			do
			{
				engine.readMeters(frame);
				if (frame.levels.size() != 2)
					complete = false;
				++reads;
			} while (!done);
		});
	const StopAndJoin stop(done, reader);
	for (int round = 0; round < 20; ++round)
	{
		const Json added = run(client, "track.add", {{"type", "audio"}});
		run(client, "track.remove", {{"track", added.at("track").at("id")}});
	}
	done = true;
	reader.join();
	run(client, "audio.stop");
	CHECK(complete);
	CHECK(reads > 0);
}

TEST_CASE("the loudness measurement goes on when a lost device returns, and starts over on a fallback", "[integration]")
{
	Engine engine;
	Client client(engine);
	const bool fallback = GENERATE(false, true);
	INFO((fallback ? "with" : "without") << " a fallback device");
	if (fallback)
		run(client, "settings.set", {{"key", "audio.fallbackDevice"}, {"value", "dummy"}});
	measuring(client);
	run(client, "audio.render", {{"frames", 48000}});
	REQUIRE(run(client, "meter.get").at("loudness").at("seconds").get<double>() == Approx(1.0).margin(0.1));

	run(client, "audio.simulateDeviceLoss", {{"connected", false}});
	REQUIRE(waitForLost(client, true));
	run(client, "audio.simulateDeviceLoss", {{"connected", true}});
	REQUIRE(waitForLost(client, false));
	const Json loudness = run(client, "meter.get").at("loudness");
	if (fallback)
	{
		CHECK(loudness.at("seconds") == 0.0);
		CHECK(loudness.at("integrated").is_null());
	}
	else
	{
		CHECK(loudness.at("seconds").get<double>() == Approx(1.0).margin(0.1));
		CHECK(loudness.at("integrated").get<double>() == Approx(-6.02).margin(0.1));
	}
}
