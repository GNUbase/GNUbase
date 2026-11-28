// SPDX-License-Identifier: GPL-3.0-or-later
// Metering and loudness (#111, reference manual pp. 533-540): level meters, the master meter's true peak and the
// EBU R 128 loudness meter, against the reference values of EBU Tech 3341 and 3342.
#include "engine/analysis/analysis.h"
#include "engine/audio/meters.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/heldjob.h"
#include "testutils/stopandjoin.h"
#include "testutils/testutils.h"

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
#include <sstream>
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
using gnubase::engine::g_maxMeterChannels;
using gnubase::engine::Json;
using gnubase::engine::LevelMeter;
using gnubase::engine::LevelReading;
using gnubase::engine::LoudnessMeter;
using gnubase::engine::LoudnessReading;
using gnubase::engine::MeterFrame;
using gnubase::engine::ObjectId;
using gnubase::engine::Result;
using gnubase::test::StopAndJoin;

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

	// How long the race tests play their rounds at most, on the wall clock. Each round waits for the other thread, and
	// when more threads run than there are cores (CI runs four tests at a time), each wait can cost a time slice: the
	// rounds could then take many minutes, beyond the 300 s after which ctest fails a test, with correct code. An
	// overloaded run checks fewer rounds instead, but never fewer than g_minRaceRounds, whatever the clock says, so
	// that a starved run still checks the race instead of passing empty, and fails if it doesn't (reportRounds). Even
	// the slowest rates seen, 44 rounds a second (Windows, more busy threads than cores) and 19 (375 rounds in 20 s,
	// the overload of #1270), play those in about 23 s and 53 s, far below the timeout. An unloaded run plays all
	// rounds well within the budget (each test took under a second in a Debug build on an x86-64 desktop). A run the
	// budget cut short says so: Catch2's warning reaches ctest's log, which CI shows only for a failing test, so CI's
	// test steps list the warnings of passing ones too (.github/workflows/ci.yml).
	constexpr auto g_raceBudget = std::chrono::seconds(20);
	constexpr int g_minRaceRounds = 1000;

	class RaceClock
	{
	public:
		explicit RaceClock(std::chrono::steady_clock::duration budget = g_raceBudget)
			: m_end(std::chrono::steady_clock::now() + budget)
		{
		}

		// Whether the race test plays its round: the first g_minRaceRounds always, then while the budget lasts.
		[[nodiscard]] bool running(int round) const
		{
			return round <= g_minRaceRounds || std::chrono::steady_clock::now() < m_end;
		}

	private:
		std::chrono::steady_clock::time_point m_end;
	};

	// The race tests' names in their cut-short warning.
	constexpr const char* g_labelRead = "block ending during a read";
	constexpr const char* g_labelReset = "first block after a reset";

	// The warning of a race test that the budget cut short. Catch2's console reporter wraps a message at 77 characters
	// (width 80, less one, less the indent), and CI's test steps pick the line out of ctest's log, so it has to fit.
	std::string cutShortMessage(const char* test, int checked, int rounds)
	{
		std::ostringstream message;
		message << "race test cut short: " << test << ", " << checked << " of " << rounds << " rounds";
		return message.str();
	}

	// Says how many rounds a race test checked when the budget ended them early (the budget is g_raceBudget), and fails
	// when it was fewer than the minimum.
	void reportRounds(const char* test, int checked, int rounds)
	{
		if (checked < rounds)
			WARN(cutShortMessage(test, checked, rounds));
		CHECK(checked >= std::min(rounds, g_minRaceRounds));
	}
}

TEST_CASE("a race test's cut-short warning fits on one line of ctest's log", "[unit]")
{
	constexpr size_t consoleLineWidth = 77;
	for (const char* test : {g_labelRead, g_labelReset})
	{
		INFO(test);
		const std::string message = cutShortMessage(test, 99999, 99999);
		CHECK(message.size() <= consoleLineWidth);
		CHECK(message.find("race test cut short: ") == 0);
	}
}

TEST_CASE("a race test's rounds run at least the minimum when the budget is over, then stop", "[unit]")
{
	const RaceClock over(std::chrono::steady_clock::duration::zero());
	CHECK(over.running(1));
	CHECK(over.running(g_minRaceRounds));
	CHECK(!over.running(g_minRaceRounds + 1));
	const RaceClock plenty(std::chrono::hours(1));
	CHECK(plenty.running(g_minRaceRounds + 1));
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

	// The master meter's reset keeps the loudness measurement and the channels' maxima, the loudness meter's the
	// main mix's maxima (reference manual pp. 535 and 538).
	run(client, "meter.reset", {{"meters", "main"}});
	meters = run(client, "meter.get");
	CHECK(meters.at("main").at("peakMax").at(0).is_null());
	CHECK(meters.at("main").at("rmsMax").at(0).is_null());
	CHECK(meters.at("loudness").at("integrated").get<double>() == Approx(-6.02).margin(0.1));
	CHECK(meters.at("loudness").at("seconds").get<double>() == Approx(4.0).margin(0.1));
	CHECK(meters.at("main").at("truePeakMax").at(0).get<double>() == Approx(-6.02).margin(0.1));
	CHECK_FALSE(meters.at("channels").at(0).at("rmsMax").at(0).is_null());
	run(client, "audio.render", {{"frames", 4800}});
	run(client, "meter.reset", {{"meters", "loudness"}});
	meters = run(client, "meter.get");
	CHECK(meters.at("loudness").at("integrated").is_null());
	CHECK(meters.at("loudness").at("seconds") == 0.0);
	CHECK(meters.at("main").at("truePeakMax").at(0).is_null());
	CHECK(meters.at("main").at("peakMax").at(0).get<double>() == Approx(-6.02).margin(0.1));
	CHECK_FALSE(meters.at("channels").at(0).at("rmsMax").at(0).is_null());
	CHECK_THAT(reject(client, "meter.reset", {{"meters", "master"}}), ContainsSubstring("meters"));

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

TEST_CASE(
	"Hold on Stop keeps the loudness measurement at the stop, Measure Loudness off measures nothing", "[integration]")
{
	// A 1 kHz sine at -6 dBFS, monitored, so the main mix plays it while the transport plays and stands.
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	run(client, "settings.set", {{"key", "audio.autoMonitoring"}, {"value", "whileRecordEnabled"}});
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}});
	const auto playFor = [&client](int64_t frames)
	{
		run(client, "transport.play");
		run(client, "audio.render", {{"frames", frames}});
		run(client, "transport.stop");
		run(client, "audio.render", {{"frames", 4800}});
		return run(client, "meter.get");
	};

	// Hold on Stop, the default (reference manual p. 540): Integrated, Range and True Peak stay after the stop.
	CHECK(run(client, "settings.get", {{"key", "meter.holdOnStop"}}).at("value") == true);
	Json meters = playFor(48000);
	CHECK(meters.at("loudness").at("integrated").get<double>() == Approx(-6.02).margin(0.1));
	CHECK(meters.at("loudness").at("seconds").get<double>() == Approx(1.0).margin(0.1));
	CHECK(meters.at("main").at("truePeakMax").at(0).get<double>() == Approx(-6.02).margin(0.1));
	// Off, the stop starts the measurement over.
	run(client, "settings.set", {{"key", "meter.holdOnStop"}, {"value", false}});
	meters = playFor(48000);
	CHECK(meters.at("loudness").at("integrated").is_null());
	CHECK(meters.at("loudness").at("seconds") == 0.0);
	CHECK(meters.at("main").at("truePeakMax").at(0).is_null());
	CHECK(meters.at("loudness").at("momentary").get<double>() == Approx(-6.02).margin(0.1)); // follows the signal

	// Measure Loudness off (p. 537): playback measures nothing; the momentary loudness still follows the signal.
	run(client, "settings.set", {{"key", "meter.holdOnStop"}, {"value", true}});
	run(client, "settings.set", {{"key", "meter.measureLoudness"}, {"value", false}});
	meters = playFor(48000);
	CHECK(meters.at("loudness").at("integrated").is_null());
	CHECK(meters.at("loudness").at("seconds") == 0.0);
	CHECK(meters.at("loudness").at("momentary").get<double>() == Approx(-6.02).margin(0.1));
	run(client, "settings.set", {{"key", "meter.measureLoudness"}, {"value", true}});
	CHECK(playFor(24000).at("loudness").at("seconds").get<double>() == Approx(0.5).margin(0.1));
}

TEST_CASE(
	"meter.reset resets the master meter's RMS Max. or Peak Max. alone, or only the channels' maxima", "[integration]")
{
	// A 1 kHz sine at -6 dBFS, monitored while the transport stands.
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}});
	run(client, "audio.render", {{"frames", 48000}});

	// Ctrl-click on RMS Max. (reference manual p. 535): only the main mix's RMS maximum, which counts again from the
	// next RMS slice.
	run(client, "meter.reset", {{"meters", "mainRmsMax"}});
	Json meters = run(client, "meter.get");
	CHECK(meters.at("main").at("rmsMax").at(0).is_null());
	CHECK(meters.at("main").at("peakMax").at(0).get<double>() == Approx(-6.02).margin(0.05));
	CHECK(meters.at("main").at("truePeakHold").at(0).get<double>() == Approx(-6.02).margin(0.1));
	CHECK_FALSE(meters.at("channels").at(0).at("rmsMax").at(0).is_null());
	run(client, "audio.render", {{"frames", 4800}});
	CHECK(run(client, "meter.get").at("main").at("rmsMax").at(1).get<double>() == Approx(-9.03).margin(0.1));

	// Ctrl-click on Peak Max.: only the true peak hold it shows.
	run(client, "meter.reset", {{"meters", "mainTruePeakHold"}});
	meters = run(client, "meter.get");
	CHECK(meters.at("main").at("truePeakHold").at(0).is_null());
	CHECK(meters.at("main").at("peakMax").at(0).get<double>() == Approx(-6.02).margin(0.05));
	CHECK(meters.at("main").at("rmsMax").at(0).get<double>() == Approx(-9.03).margin(0.1));
	run(client, "audio.render", {{"frames", 4800}});
	CHECK(run(client, "meter.get").at("main").at("truePeakHold").at(1).get<double>() == Approx(-6.02).margin(0.1));

	// The channel meters' peak level (p. 468): every channel's maxima, not the main mix's.
	run(client, "meter.reset", {{"meters", "channels"}});
	meters = run(client, "meter.get");
	CHECK(meters.at("channels").at(0).at("peakMax").at(0).is_null());
	CHECK(meters.at("channels").at(0).at("rmsMax").at(0).is_null());
	CHECK(meters.at("main").at("peakMax").at(0).get<double>() == Approx(-6.02).margin(0.05));
	CHECK(meters.at("main").at("truePeakHold").at(0).get<double>() == Approx(-6.02).margin(0.1));
}

TEST_CASE("the master meter holds the true peak maximum, which only its own reset clears", "[integration]")
{
	// An 8 kHz sine at 48 kHz, 6 samples a period: its samples reach only 0.866 of its peaks, -7.27 dBFS for a
	// -6.02 dBTP sine. Monitored while the transport stands, so the loudness meter's truePeakMax counts nothing.
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 8000.0}, {"Level", 0.5}}}}}});
	const auto channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}});
	// The sine's sudden start overshoots between the samples: the master meter's reset starts over past it.
	run(client, "audio.render", {{"frames", 4800}});
	CHECK(run(client, "meter.get").at("main").at("truePeakHold").at(0).get<double>() > -6.0);
	run(client, "meter.reset", {{"meters", "main"}});
	run(client, "audio.render", {{"frames", 48000}});

	Json main = run(client, "meter.get").at("main");
	REQUIRE(main.at("truePeakHold").size() == 2);
	CHECK(main.at("peakMax").at(0).get<double>() == Approx(-7.27).margin(0.05));
	CHECK(main.at("truePeakHold").at(0).get<double>() == Approx(-6.02).margin(0.1));
	CHECK(main.at("truePeakHold").at(1).get<double>() == Approx(-6.02).margin(0.1));
	CHECK(main.at("truePeakMax").at(0).is_null());
	MeterFrame frame;
	engine.readMeters(frame);
	CHECK(decibels(frame.loudness.truePeakHold[0]) == Approx(-6.02).margin(0.1));
	CHECK(frame.loudness.truePeak[0] <= frame.loudness.truePeakHold[0]);
	CHECK(decibels(frame.loudness.truePeak[0]) == Approx(-6.02).margin(0.1));

	// Reset Loudness keeps it, the master meter's reset clears it, and it counts again from the next block.
	run(client, "meter.reset", {{"meters", "loudness"}});
	CHECK(run(client, "meter.get").at("main").at("truePeakHold").at(0).get<double>() == Approx(-6.02).margin(0.1));
	run(client, "meter.reset", {{"meters", "main"}});
	CHECK(run(client, "meter.get").at("main").at("truePeakHold").at(0).is_null());
	engine.readMeters(frame);
	CHECK(frame.loudness.truePeak[0] == 0.0f);
	run(client, "audio.render", {{"frames", 4800}});
	CHECK(run(client, "meter.get").at("main").at("truePeakHold").at(1).get<double>() == Approx(-6.02).margin(0.1));
	run(client, "meter.reset");
	CHECK(run(client, "meter.get").at("main").at("truePeakHold").at(1).is_null());
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

TEST_CASE(
	"meter.position measures the channels after the input gain, after the fader or after the panner", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const Json added = run(client, "track.add", {{"type", "audio"}, {"channels", 1}});
	const auto channel = added.at("channel").at("id").get<ObjectId>();
	// The mono track's input, the stereo sine's two -6 dBFS sides at -3 dB each: -3 dBFS, 6 dB up in the pre section,
	// 10 dB down at the fader, and at the center 3 dB down on each side with the -3 dB pan law.
	run(client, "channel.set",
		{{"channel", channel}, {"recordEnable", true}, {"pre", {{"gain", 6.0}}}, {"volume", -10.0}});
	// A plugin slot splits the strip in two nodes (StripPart): the one before the slot measures the input.
	if (GENERATE(false, true))
		gnubase::test::waitForPlugin(
			client, run(client, "plugin.insert", {{"channel", channel}, {"plugin", "builtin:eq"}}).at("slot").at("id"));
	const auto peaks = [&client, channel](const std::string& position)
	{
		run(client, "settings.set", {{"key", "meter.position"}, {"value", position}});
		run(client, "audio.render", {{"frames", 9600}});
		return run(client, "meter.get", {{"channels", {channel}}}).at("channels").at(0).at("peak");
	};

	Json peak = peaks("postPanner"); // the default: both sides of the panner
	REQUIRE(peak.size() == 2);
	CHECK(peak.at(0).get<double>() == Approx(-10.02).margin(0.05));
	CHECK(peak.at(1).get<double>() == Approx(-10.02).margin(0.05));
	peak = peaks("postFader"); // the fader's gain without the panner: the channel's one side
	REQUIRE(peak.size() == 1);
	CHECK(peak.at(0).get<double>() == Approx(-7.01).margin(0.05));
	peak = peaks("input");
	REQUIRE(peak.size() == 1);
	CHECK(peak.at(0).get<double>() == Approx(2.99).margin(0.05));

	// Muting silences the channel after the fader, not its input.
	run(client, "channel.set", {{"channel", channel}, {"mute", true}});
	CHECK(peaks("input").at(0).get<double>() == Approx(2.99).margin(0.05));
	CHECK(peaks("postFader").at(0).is_null());
}

TEST_CASE("a MIDI channel's meter shows the velocity of the loudest note that sounds", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
	const auto channel = run(client, "track.add", {{"type", "midi"}}).at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"monitor", true}});
	// Plays a MIDI message into the track for 200 ms: the meter's peak is then that of the notes that sound.
	const auto play = [&client, channel](const std::vector<int>& bytes)
	{
		run(client, "midi.inject", {{"channel", channel}, {"messages", {{{"position", 0}, {"bytes", bytes}}}}});
		run(client, "audio.render", {{"frames", 9600}});
		return run(client, "meter.get", {{"channels", {channel}}}).at("channels").at(0);
	};

	Json meter = play({0x90, 60, 127}); // full velocity: 0 dBFS
	REQUIRE(meter.at("peak").size() == 1);
	CHECK(meter.at("peak").at(0).get<double>() == Approx(0.0).margin(0.01));
	meter = play({0x90, 64, 64}); // the louder note still sounds
	CHECK(meter.at("peak").at(0).get<double>() == Approx(0.0).margin(0.01));
	meter = play({0x80, 60, 0});
	CHECK(meter.at("peak").at(0).get<double>() == Approx(20.0 * std::log10(64.0 / 127.0)).margin(0.01));
	meter = play({0x80, 64, 0});
	CHECK(meter.at("peak").at(0).is_null());
	CHECK(meter.at("peakMax").at(0).get<double>() == Approx(0.0).margin(0.01));

	// Muting ends the notes that sound.
	CHECK(
		play({0x90, 60, 100}).at("peak").at(0).get<double>() == Approx(20.0 * std::log10(100.0 / 127.0)).margin(0.01));
	run(client, "channel.set", {{"channel", channel}, {"mute", true}});
	run(client, "audio.render", {{"frames", 9600}});
	CHECK(run(client, "meter.get", {{"channels", {channel}}}).at("channels").at(0).at("peak").at(0).is_null());
}

TEST_CASE("a level meter channel that comes back starts without what it measured before", "[unit]")
{
	// Also without a reset, which clears the published maxima itself.
	const bool reset = GENERATE(true, false);
	INFO((reset ? "with" : "without") << " a reset");
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
	if (reset)
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
	// Also loud low frequencies while it stood, which the K-weighting's high pass would carry into the first blocks.
	const auto [frequency, level] = GENERATE(table<double, double>({{1000.0, -10.0}, {60.0, 0.0}}));
	const double stood = GENERATE(3.0, 3.05); // also ending off the 100 ms grid
	INFO(stood << " s of " << frequency << " Hz at " << level << " dBFS while it stood");
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal quiet;
	quiet.sine(5.0, -30.0);
	Signal loud;
	loud.sine(stood, level, frequency);
	quiet.play(meter);
	loud.play(meter, false); // stopped
	quiet.play(meter);
	const LoudnessReading reading = meter.read();
	CHECK(reading.integrated.value_or(0.0) == Approx(-30.0).margin(0.1));
	CHECK(reading.momentaryMax.value_or(0.0) == Approx(-30.0).margin(0.1));
	CHECK(decibels(reading.truePeakMax[0]) == Approx(-30.0).margin(0.1));
	CHECK(reading.seconds == Approx(10.0).margin(0.1));
}

TEST_CASE("the loudness blocks start where playback starts, also after a stop within one step", "[unit]")
{
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal before;
	before.sine(1.02, -30.0);
	before.play(meter);
	Signal pause;
	pause.silence(0.02); // shorter than the rest of its step
	pause.play(meter, false);
	Signal after;
	after.sine(1.0, -10.0);
	after.play(meter);
	// No block after the start holds a step from before the stop, so the quiet blocks all lie below the relative gate.
	CHECK(meter.read().integrated.value_or(0.0) == Approx(-10.0).margin(0.1));
}

TEST_CASE("the loudness meter measures up to where playback stops, also within a step", "[unit]")
{
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal played;
	played.sine(3.0, -30.0);
	played.sine(0.05, -10.0); // the last 50 ms before the stop, in a step that ends while the transport stands
	played.play(meter);
	Signal stopped;
	stopped.silence(1.0);
	stopped.play(meter, false);
	const LoudnessReading reading = meter.read();
	CHECK(reading.seconds == Approx(3.05));
	CHECK(decibels(reading.truePeakMax[0]) == Approx(-10.0).margin(0.1));
	// The 400 ms and 3 s blocks that end at the stop: 50 ms at -10 dBFS, the rest at -30.
	const auto loudness = [](double loud, double total)
	{ return 10.0 * std::log10(((loud * 0.1) + ((total - loud) * 0.001)) / total); };
	CHECK(reading.momentaryMax.value_or(0.0) == Approx(loudness(0.05, 0.4)).margin(0.1));
	CHECK(reading.shortTermMax.value_or(0.0) == Approx(loudness(0.05, 3.0)).margin(0.1));
	// 27 blocks at -30 LUFS from 0.4 to 3 s, and the one that ends at the stop.
	const double last = std::pow(10.0, loudness(0.05, 0.4) / 10.0);
	CHECK(reading.integrated.value_or(0.0) == Approx(10.0 * std::log10(((27.0 * 0.001) + last) / 28.0)).margin(0.1));
}

TEST_CASE("a reset while playing starts a fresh step", "[unit]")
{
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal signal;
	signal.sine(3.0, -30.0);
	signal.sine(0.05, -10.0); // the 50 ms before the reset, in a step that goes on after it
	signal.play(meter);
	meter.reset();
	signal.clear();
	signal.sine(3.0, -30.0);
	signal.play(meter);
	Signal stopped;
	stopped.silence(1.0);
	stopped.play(meter, false);
	const LoudnessReading reading = meter.read();
	CHECK(reading.seconds == Approx(3.0));
	CHECK(decibels(reading.truePeakMax[0]) == Approx(-30.0).margin(0.1));
	CHECK(reading.momentaryMax.value_or(0.0) == Approx(-30.0).margin(0.1));
	CHECK(reading.integrated.value_or(0.0) == Approx(-30.0).margin(0.1));
}

TEST_CASE("a reset between the last played block and the stop leaves nothing to measure", "[unit]")
{
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal played;
	played.sine(3.05, -10.0); // ends within a step
	played.play(meter);
	meter.reset();
	Signal stopped;
	stopped.silence(1.0);
	stopped.play(meter, false);
	const LoudnessReading reading = meter.read();
	CHECK(reading.seconds == 0.0);
	CHECK(reading.truePeakMax[0] == 0.0f);
	CHECK(!reading.momentaryMax);
	CHECK(!reading.shortTermMax);
	CHECK(!reading.integrated);
}

TEST_CASE("the true peak reads between the samples from the 24th sample after playback starts", "[unit]")
{
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal stopped;
	stopped.silence(0.1);
	stopped.play(meter, false);
	// The 12th and 13th samples after the start at -6 dBFS, with silence around: the peak between them lies 11.5
	// samples before the 24th, the first sample whose interpolator holds only audio from after the start.
	Signal played;
	played.silence(11.0 / g_rate);
	played.impulse(0.5f);
	played.impulse(0.5f);
	played.silence(0.2);
	played.play(meter);
	CHECK(decibels(meter.read().truePeakMax[0]) > decibels(0.5) + 1.5);
}

TEST_CASE("a loudness meter channel that comes back starts without what it measured before", "[unit]")
{
	LoudnessMeter meter;
	meter.prepare(g_rate);
	Signal loud;
	loud.sine(1.0, -6.0);
	loud.play(meter); // stereo
	const std::vector<float> silence(g_block, 0.0f);
	for (int block = 0; block < 100; ++block) // mono for a while: the second channel goes away
	{
		const std::array<const float*, 1> audio{silence.data()};
		meter.process(audio.data(), 1, g_block, true);
	}
	// It comes back silent: no old true peak or maximum, also before it publishes again.
	const std::array<const float*, 2> audio{silence.data(), silence.data()};
	for (int block = 0; block < 20; ++block)
	{
		meter.process(audio.data(), 2, g_block, true);
		const LoudnessReading reading = block == 0 ? meter.read() : meter.readFrame();
		INFO("block " << block);
		REQUIRE(reading.channels == 2);
		CHECK(reading.truePeak[1] == 0.0f);
		CHECK(reading.truePeakMax[1] == 0.0f);
	}
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

TEST_CASE("meter.get refuses a channel deleted while no device runs", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}});
	const Json added = run(client, "track.add", {{"type", "audio"}});
	const auto channel = added.at("channel").at("id").get<ObjectId>();
	run(client, "audio.render", {{"frames", 4800}});
	run(client, "audio.simulateDeviceLoss", {{"connected", false}}); // no fallback: no device, no graph builds
	REQUIRE(waitForLost(client, true));
	run(client, "track.remove", {{"track", added.at("track").at("id")}});
	CHECK_THAT(reject(client, "meter.get", {{"channels", {channel}}}), ContainsSubstring(std::to_string(channel)));
	CHECK(run(client, "meter.get").at("channels").empty());
}

TEST_CASE("a mono channel's second side starts over when it moves to a mono bus and back", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 1000.0}, {"Level", 0.5}}}}}});
	const auto channel =
		run(client, "track.add", {{"type", "audio"}, {"channels", 1}}).at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", true}});
	const auto strip = [&] { return run(client, "meter.get", {{"channels", {channel}}}).at("channels").at(0); };
	run(client, "audio.render", {{"frames", 9600}});
	REQUIRE(strip().at("peakMax").size() == 2);
	CHECK(strip().at("peakMax").at(1).get<double>() == Approx(-6.02).margin(0.05)); // the centre

	// Hard left, so that the second side stays silent from here on, then to a mono bus and back, without a reset.
	ObjectId stereo = 0;
	const Json project = run(client, "project.get");
	for (const Json& bus : project.at("busses"))
		if (bus.at("mainMix") == true)
			stereo = bus.at("id").get<ObjectId>();
	REQUIRE(stereo != 0);
	const Json mono = run(client, "bus.add", {{"direction", "output"}, {"channels", 1}}).at("bus").at("id");
	run(client, "channel.set", {{"channel", channel}, {"pan", -1.0}, {"output", mono}});
	run(client, "audio.render", {{"frames", 9600}});
	CHECK(strip().at("peakMax").size() == 1);
	run(client, "channel.set", {{"channel", channel}, {"output", stereo}});
	run(client, "audio.render", {{"frames", 9600}});
	const Json back = strip();
	REQUIRE(back.at("peakMax").size() == 2);
	CHECK(back.at("peak").at(0).get<double>() == Approx(-3.01).margin(0.05)); // hard left, -3 dB pan law
	CHECK(back.at("peak").at(1).is_null());
	CHECK(back.at("rms").at(1).is_null());
	CHECK(back.at("peakMax").at(1).is_null());
	CHECK(back.at("rmsMax").at(1).is_null());
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

TEST_CASE("a frame loses no peak to a block that ends while it reads", "[unit]")
{
	// Without a reset the maximum read limits no peak, if the audio thread publishes it before it raises the held peak
	// and the reader takes the peak before it reads the maximum. Each round one block, the loudest so far, races one
	// frame's read; that frame or the next one has its peak. The rounds end early under load, but not before the
	// minimum (RaceClock).
	constexpr int rounds = 50000;
	const auto level = [](int round) { return static_cast<float>(round) / rounds; };
	LevelMeter meter;
	std::atomic<bool> done{false};
	std::atomic<int> started{0};
	std::atomic<int> played{0};
	std::atomic<int> checked{0};
	std::atomic<int> lost{0};
	std::thread reader(
		[&]
		{
			for (int round = 1; round <= rounds; ++round)
			{
				while (started < round && !done)
					std::this_thread::yield();
				if (started < round)
					return; // the rounds ended before this one
				const float racing = meter.readFrame().peak[0];
				while (played < round && !done)
					std::this_thread::yield();
				const float after = meter.readFrame().peak[0];
				if (std::max(racing, after) != level(round) && !done)
					++lost;
				checked = round;
			}
		});
	const StopAndJoin stop(done, reader);
	const RaceClock clock;
	for (int round = 1; round <= rounds && clock.running(round); ++round)
	{
		const float sample = level(round);
		const float* const channel = &sample;
		started = round;
		meter.process(&channel, 1, 1, g_rate);
		played = round;
		while (checked < round)
			std::this_thread::yield();
	}
	done = true;
	reader.join();
	reportRounds(g_labelRead, checked, rounds);
	CHECK(lost == 0);
}

TEST_CASE("a frame loses no peak of the first block after a reset, also when it is quieter than the held one", "[unit]")
{
	// Each round a loud block is held, a reset follows, as Reset on Start does it on the audio thread, and then a
	// quieter block, while the reader reads frames all the time; one of the round's frames has the quieter block's
	// peak, which the first block after a reset hands over in place of the held one. Raising the held peak instead, the
	// quieter block wrote nothing, and a frame that took the loud peak could read the reset's maximum of 0 while the
	// block's maximum still waited in the audio thread's store buffer, so that its peak never reached a frame. Nothing
	// can force that reordering, but this gives it room: all channels play and the last one is checked, so that only
	// the loads in read() lie between the reader taking the peak and loading the maximum, and the reader keeps the
	// maxima's cache line contended. With raise in place of the handover it failed in nearly every run of optimized
	// builds on x86-64 desktops (MSVC, GCC and Clang), though not in unoptimized ones: of CI's jobs only the Release
	// builds (windows-x64, linux-x64-gcc) can be expected to catch it, while a local Debug run and the sanitizer jobs,
	// which build Debug, say nothing about raise. Measured on CI's runners (2026-11-20, hosted, four virtual
	// processors, this test alone in 20 runs, then four copies at a time in 20 runs): raise failed windows-x64 in 20 of
	// 20 and 17 of 20 runs, linux-x64-gcc in 19 of 20 and 0 of 20 (#1244). ctest runs other tests beside this one, not
	// copies of it, so the second figure only says that the race can be missed there. The rounds end early under load,
	// but not before the minimum (RaceClock).
	constexpr int rounds = 50000;
	constexpr auto last = static_cast<size_t>(g_maxMeterChannels - 1);
	// Both levels are new every round, so that a frame with the quieter peak, whether it took it or limited the loud
	// one to it, took a peak written after the round began, and reads the round from playing.
	const auto loudLevel = [](int round) { return 0.5f + (0.5f * static_cast<float>(round) / rounds); };
	const auto quietLevel = [](int round) { return 0.5f * static_cast<float>(round) / rounds; };
	LevelMeter meter;
	std::atomic<bool> done{false};
	std::atomic<int> playing{0};     // the round whose blocks play
	std::atomic<uint32_t> frames{0}; // read so far, wrapping around
	std::atomic<int> heard{0};       // the last round whose quieter peak a frame had
	std::thread reader(
		[&]
		{
			do
			{
				const float peak = meter.readFrame().peak[last];
				// The round can't end before this frame counts, so a frame with its quieter peak still finds it here.
				if (const int round = playing; peak == quietLevel(round))
					heard = round;
				// Now and then the reader lets the audio thread run, should they share a core (CI runs tests in
				// parallel); otherwise each round could take the reader's whole time slice.
				if (++frames % 16 == 0)
					std::this_thread::yield();
			} while (!done);
		});
	const StopAndJoin stop(done, reader);
	const RaceClock clock;
	int checked = 0;
	int lost = 0;
	for (int round = 1; round <= rounds && clock.running(round); ++round)
	{
		const float loud = loudLevel(round);
		const float quiet = quietLevel(round);
		std::array<const float*, g_maxMeterChannels> loudChannels{};
		std::array<const float*, g_maxMeterChannels> quietChannels{};
		loudChannels.fill(&loud);
		quietChannels.fill(&quiet);
		playing = round;
		meter.process(loudChannels.data(), g_maxMeterChannels, 1, g_rate);
		meter.reset();
		meter.process(quietChannels.data(), g_maxMeterChannels, 1, g_rate);
		// A read-modify-write, which orders the blocks before the frames counted after it: the frame under way may
		// have taken the peaks before the quieter block, the next one takes its peak if no frame did.
		const uint32_t read = frames.fetch_add(0);
		while (frames - read < 2)
			std::this_thread::yield();
		if (heard != round)
			++lost;
		checked = round;
	}
	done = true;
	reader.join();
	reportRounds(g_labelReset, checked, rounds);
	CHECK(lost == 0);
}

TEST_CASE("a frame's peak is never above its maximum, also when the meter is reset meanwhile", "[unit]")
{
	// meter.reset comes from the command thread, at any point of the audio thread's blocks and of a frame's read.
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
	const StopAndJoin stopReader(done, reader);
	std::thread resetter(
		[&]
		{
			do
				meter.reset();
			while (!done);
		});
	const StopAndJoin stopResetter(done, resetter);
	constexpr int blocks = 500000;
	for (int block = 1; block <= blocks; ++block)
	{
		const float sample = static_cast<float>(block) / blocks;
		const float* const channel = &sample;
		meter.process(&channel, 1, 1, g_rate);
	}
	done = true;
	reader.join();
	resetter.join();
	CHECK(above == 0);
}

TEST_CASE("a frame's peak holds nothing from before a reset", "[unit]")
{
	Signal loud;
	loud.impulse(0.9f);
	Signal quiet;
	quiet.impulse(0.2f);
	LevelMeter meter;

	// Nothing played since the reset: the held peak went with the maximum.
	loud.play(meter);
	meter.reset();
	LevelReading frame = meter.readFrame();
	REQUIRE(frame.channels == 2);
	CHECK(frame.peak[0] == 0.0f);
	CHECK(frame.peak[1] == 0.0f);
	CHECK(frame.peakMax[0] == 0.0f);

	// A quieter block since the reset, as when playback starts after a loud signal stopped: its peak is the frame's.
	loud.play(meter);
	meter.reset();
	quiet.play(meter);
	frame = meter.readFrame();
	CHECK(frame.peak[0] == Approx(0.2f));
	CHECK(frame.peakMax[0] == Approx(0.2f));

	// What comes after a reset is held as ever, however loud, and read once.
	meter.reset();
	loud.play(meter);
	quiet.play(meter);
	frame = meter.readFrame();
	CHECK(frame.peak[0] == Approx(0.9f));
	CHECK(frame.peakMax[0] == Approx(0.9f));
	frame = meter.readFrame();
	CHECK(frame.peak[0] == 0.0f);
	CHECK(frame.peakMax[0] == Approx(0.9f));
}

TEST_CASE("Reset on Start leaves a frame no peak from before playback started", "[integration]")
{
	Engine engine;
	Client client(engine);
	const ObjectId channel = measuring(client);
	run(client, "settings.set", {{"key", "meter.resetOnStart"}, {"value", true}});
	run(client, "audio.render", {{"frames", 9600}});
	REQUIRE(run(client, "meter.get").at("main").at("peakMax").at(0).get<double>() == Approx(-6.02).margin(0.05));
	run(client, "transport.stop");
	// The signal stops before playback starts, and nothing reads the frame's peak in between.
	run(client, "channel.set", {{"channel", channel}, {"recordEnable", false}});
	run(client, "audio.render", {{"frames", 9600}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 64}});
	MeterFrame frame;
	engine.readMeters(frame);
	REQUIRE(frame.main.channels == 2);
	CHECK(frame.main.peakMax[0] == 0.0f);
	CHECK(frame.main.peak[0] == 0.0f);
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

TEST_CASE("measurement presets keep the thresholds and those another process saved meanwhile", "[unit]")
{
	// Reference manual p. 539: presets of the meter display's thresholds, in the user setting meter.presets. Two
	// engines on one settings folder stand for two GNUbase processes.
	const gnubase::test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path());
	Client client(engine);
	Engine otherEngine(gnubase::audio::makeBackends(), dir.path());
	Client other(otherEngine);
	const auto presets = [&client] { return run(client, "settings.get", {{"key", "meter.presets"}}).at("value"); };

	// A preset holds all eight thresholds, the current ones unless given.
	run(client, "settings.set", {{"key", "meter.integratedReference"}, {"value", -16.0}});
	run(client, "meter.savePreset", {{"name", "Streaming"}});
	run(other, "meter.savePreset", {{"name", "Broadcast"}, {"values", {{"meter.truePeakReference", -2.0}}}});
	run(client, "meter.savePreset", {{"name", "Loud"}, {"values", {{"meter.momentaryMaxTolerance", 2.0}}}});
	const Json saved = presets();
	REQUIRE(saved.size() == 3);
	CHECK(saved.at("Streaming").size() == 8);
	CHECK(saved.at("Streaming").at("meter.integratedReference") == -16.0);
	CHECK(saved.at("Streaming").at("meter.truePeakReference") == -1.0);
	CHECK(saved.at("Broadcast").at("meter.truePeakReference") == -2.0);
	CHECK(saved.at("Loud").at("meter.momentaryMaxTolerance") == 2.0);
	CHECK(saved.at("Loud").at("meter.integratedReference") == -16.0);

	// Each value must be a threshold that fits its setting, and the name not empty.
	CHECK_THAT(reject(client, "meter.savePreset", {{"name", "x"}, {"values", {{"meter.truePeakReference", 9.0}}}}),
		ContainsSubstring("meter.truePeakReference"));
	CHECK_THAT(reject(client, "meter.savePreset", {{"name", "x"}, {"values", {{"meter.scale", "din"}}}}),
		ContainsSubstring("meter.scale"));
	CHECK_THAT(reject(client, "meter.savePreset", {{"name", ""}}), ContainsSubstring("name"));
	// Nor can settings.set put any other setting into a preset, which couldn't be loaded.
	CHECK_THAT(reject(client, "settings.set", {{"key", "meter.presets"}, {"value", {{"x", {{"meter.scale", "din"}}}}}}),
		ContainsSubstring("meter.presets"));

	// Removing one keeps the others, also those the other process saved.
	run(client, "meter.removePreset", {{"name", "Streaming"}});
	CHECK(presets().size() == 2);
	CHECK(presets().contains("Broadcast"));
	CHECK_THAT(reject(client, "meter.removePreset", {{"name", "Streaming"}}), ContainsSubstring("Streaming"));
}

TEST_CASE("the alignment level is one of the levels of its list", "[unit]")
{
	// Reference manual p. 534: -9, -12, -14, -15, -18, -20 or -22 dBFS.
	const gnubase::test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path());
	Client client(engine);
	run(client, "settings.set", {{"key", "meter.alignmentLevel"}, {"value", -9}});
	CHECK(run(client, "settings.get", {{"key", "meter.alignmentLevel"}}).at("value") == -9);
	CHECK_THAT(reject(client, "settings.set", {{"key", "meter.alignmentLevel"}, {"value", -16}}),
		ContainsSubstring("meter.alignmentLevel"));
}
