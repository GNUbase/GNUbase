// SPDX-License-Identifier: GPL-3.0-or-later
// Time-stretching and pitch-shifting (#142): the wrapper around Rubber Band Library in its offline and real-time
// modes, measured on test signals, and the media.stretch command.
#include "engine/audiofile/audiofile.h"
#include "engine/dsp/timestretch.h"
#include "engine/engine.h"
#include "platform/rt/realtime.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using Catch::Generators::from_range;
using Catch::Matchers::WithinAbs;
using namespace gnubase::engine;
using namespace gnubase::engine::dsp;
namespace test = gnubase::test;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;
	constexpr double g_rate = 48000.0;

	using Channels = std::vector<std::vector<float>>;

	std::vector<float> sine(double frequency, size_t frames, double amplitude = 0.5)
	{
		std::vector<float> samples(frames);
		for (size_t i = 0; i < frames; ++i)
			samples[i] =
				static_cast<float>(amplitude * std::sin(2.0 * g_pi * frequency * static_cast<double>(i) / g_rate));
		return samples;
	}

	// Eight harmonics of 220 Hz at falling levels: a steady tone with a waveform the phases of its partials shape.
	std::vector<float> harmonicTone(size_t frames)
	{
		std::vector<float> samples(frames);
		for (size_t i = 0; i < frames; ++i)
		{
			double value = 0.0;
			for (int harmonic = 1; harmonic <= 8; ++harmonic)
				value += std::sin(2.0 * g_pi * 220.0 * harmonic * static_cast<double>(i) / g_rate) / harmonic;
			samples[i] = static_cast<float>(0.25 * value);
		}
		return samples;
	}

	double rms(const std::vector<float>& samples, size_t begin, size_t end)
	{
		double sum = 0.0;
		for (size_t i = begin; i < end; ++i)
			sum += static_cast<double>(samples[i]) * samples[i];
		return std::sqrt(sum / static_cast<double>(end - begin));
	}

	double decibels(double ratio)
	{
		return 20.0 * std::log10(ratio);
	}

	// The frequency of a sine from its rising zero crossings between begin and end, interpolated between samples.
	double frequency(const std::vector<float>& samples, size_t begin, size_t end)
	{
		double first = -1.0;
		double last = -1.0;
		int crossings = 0;
		for (size_t i = begin + 1; i < end; ++i)
			if (samples[i - 1] < 0.0f && samples[i] >= 0.0f)
			{
				const double at = static_cast<double>(i - 1) + (-samples[i - 1] / (samples[i] - samples[i - 1]));
				if (crossings == 0)
					first = at;
				last = at;
				++crossings;
			}
		REQUIRE(crossings > 2);
		return (crossings - 1) / (last - first) * g_rate;
	}

	double cents(double measured, double expected)
	{
		return 1200.0 * std::log2(measured / expected);
	}

	StretchSettings settingsFor(StretchMode mode, int channels = 1)
	{
		StretchSettings settings;
		settings.channels = channels;
		settings.sampleRate = g_rate;
		settings.mode = mode;
		return settings;
	}

	// Plays input through a real-time stretcher the way an audio track pulls it: blockFrames output frames per block,
	// input fed as the stretcher asks for it, with the start pad before and silence after it. Every buffer is
	// allocated up front; the block loop runs in a real-time context, so the RT checker sees every allocation there.
	// beforeBlock may change the stretcher's settings between blocks. Returns outputFrames frames per channel after
	// the latency.
	Channels playRealtime(TimeStretcher& stretcher, const Channels& input, size_t outputFrames, size_t blockFrames,
		const std::function<void(TimeStretcher&, size_t)>& beforeBlock = {})
	{
		const size_t channels = input.size();
		const size_t inputFrames = input.front().size();
		const size_t maxBlock = stretcher.settings().maxBlockFrames;
		const size_t latency = stretcher.latency();
		Channels output(channels, std::vector<float>(latency + outputFrames));
		Channels scratch(channels, std::vector<float>(maxBlock));
		std::vector<const float*> in(channels);
		std::vector<float*> out(channels);
		// Input position, counting the start pad: frame inputPosition - pad of the input is next.
		size_t inputPosition = 0;
		const size_t pad = stretcher.startPad();
		size_t produced = 0;
		{
			const gnubase::rt::ScopedRealtimeContext realtime;
			for (size_t block = 0; produced < output.front().size(); ++block)
			{
				if (beforeBlock)
					beforeBlock(stretcher, block);
				const size_t wanted = std::min(blockFrames, output.front().size() - produced);
				while (stretcher.available() < wanted)
				{
					const size_t frames = std::clamp<size_t>(stretcher.inputRequired(), 1, maxBlock);
					for (size_t channel = 0; channel < channels; ++channel)
					{
						for (size_t i = 0; i < frames; ++i)
						{
							const size_t position = inputPosition + i;
							scratch[channel][i] =
								position >= pad && position - pad < inputFrames ? input[channel][position - pad] : 0.0f;
						}
						in[channel] = scratch[channel].data();
					}
					stretcher.process(in.data(), frames, false);
					inputPosition += frames;
				}
				for (size_t channel = 0; channel < channels; ++channel)
					out[channel] = output[channel].data() + produced;
				produced += stretcher.retrieve(out.data(), wanted);
			}
		}
		for (std::vector<float>& channel : output)
			channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(latency));
		return output;
	}

	std::atomic<int> g_violations{0};

	void countViolation(const char* /*what*/) noexcept
	{
		++g_violations;
	}
}

TEST_CASE("pitch scales come from semitones and cents", "[unit]")
{
	CHECK(pitchScale(0.0, 0.0) == 1.0);
	CHECK_THAT(pitchScale(12.0, 0.0), WithinAbs(2.0, 1e-12));
	CHECK_THAT(pitchScale(-12.0, 0.0), WithinAbs(0.5, 1e-12));
	CHECK_THAT(pitchScale(7.0, 0.0), WithinAbs(std::pow(2.0, 7.0 / 12.0), 1e-12));
	CHECK_THAT(pitchScale(0.0, 100.0), WithinAbs(pitchScale(1.0, 0.0), 1e-12));
	CHECK_THAT(pitchScale(2.0, -50.0), WithinAbs(pitchScale(1.5, 0.0), 1e-12));
	CHECK(stretchedLength(48000, 1.5) == 72000);
	CHECK(stretchedLength(3, 0.5) == 2); // rounded
}

TEST_CASE("modes and presets have names", "[unit]")
{
	for (const std::string& name : stretchPresetNames())
	{
		const auto preset = stretchPresetFromString(name);
		REQUIRE(preset.has_value());
		CHECK(toString(preset.value_or(StretchPreset::Quality)) == name);
	}
	for (const std::string& name : stretchModeNames())
	{
		const auto mode = stretchModeFromString(name);
		REQUIRE(mode.has_value());
		CHECK(toString(mode.value_or(StretchMode::Offline)) == name);
	}
	CHECK(stretchPresetNames().size() == 8);
	CHECK(stretchPresetNames().front() == "quality");
	CHECK(stretchModeNames() == std::vector<std::string>{"realtime", "offline"});
	CHECK_FALSE(stretchPresetFromString("unknown").has_value());
	CHECK_FALSE(stretchModeFromString("").has_value());
}

TEST_CASE("invalid settings are refused", "[unit]")
{
	StretchSettings settings = settingsFor(StretchMode::Offline);
	SECTION("ratio")
	{
		settings.ratio = 0.0;
	}
	SECTION("ratio too large")
	{
		settings.ratio = g_maxStretchRatio * 2.0;
	}
	SECTION("pitch")
	{
		settings.semitones = 60.0;
	}
	SECTION("channels")
	{
		settings.channels = 0;
	}
	SECTION("sample rate")
	{
		settings.sampleRate = 0.0;
	}
	SECTION("block size")
	{
		settings.maxBlockFrames = 0;
	}
	SECTION("real-time ratio")
	{
		settings.mode = StretchMode::RealTime;
		settings.ratio = g_maxRealtimeRatio * 1.5;
	}
	SECTION("real-time pitch")
	{
		settings.mode = StretchMode::RealTime;
		settings.semitones = -30.0;
	}
	CHECK_THROWS_AS(TimeStretcher(settings), std::invalid_argument);
}

TEST_CASE("stretched audio has the stretched duration", "[unit]")
{
	const StretchMode mode = GENERATE(StretchMode::Offline, StretchMode::RealTime);
	const double ratio = GENERATE(0.5, 0.8, 1.5, 2.0);
	CAPTURE(toString(mode), ratio);
	const size_t frames = 48000;
	const std::vector<float> input = sine(440.0, frames);
	StretchSettings settings = settingsFor(mode);
	settings.ratio = ratio;
	const Channels output = stretch({input}, settings);
	REQUIRE(output.size() == 1);
	const auto expected = static_cast<size_t>(stretchedLength(static_cast<int64_t>(frames), ratio));
	REQUIRE(output.front().size() == expected);
	// The sine fills the output from start to end: aligned at the start, not cut short or padded with silence.
	const double inputLevel = rms(input, 0, frames);
	const size_t edge = 4800; // 100 ms
	CHECK(decibels(rms(output.front(), 0, edge) / inputLevel) > -3.0);
	CHECK(decibels(rms(output.front(), expected - edge, expected) / inputLevel) > -3.0);
	CHECK(std::abs(decibels(rms(output.front(), edge, expected - edge) / inputLevel)) < 1.0);
	// Same pitch.
	CHECK(std::abs(cents(frequency(output.front(), edge, expected - edge), 440.0)) < 2.0);
}

TEST_CASE("a pitch shift moves a sine by the interval", "[unit]")
{
	const StretchMode mode = GENERATE(StretchMode::Offline, StretchMode::RealTime);
	struct Shift
	{
		double semitones;
		double cents;
	};
	const Shift shift =
		GENERATE(Shift{7.0, 0.0}, Shift{-5.0, 0.0}, Shift{12.0, 0.0}, Shift{0.0, 50.0}, Shift{-12.0, -30.0});
	CAPTURE(toString(mode), shift.semitones, shift.cents);
	const size_t frames = 96000;
	StretchSettings settings = settingsFor(mode);
	settings.semitones = shift.semitones;
	settings.cents = shift.cents;
	const Channels output = stretch({sine(440.0, frames)}, settings);
	REQUIRE(output.front().size() == frames);
	const double expected = 440.0 * pitchScale(shift.semitones, shift.cents);
	CHECK(std::abs(cents(frequency(output.front(), 9600, frames - 9600), expected)) < 1.0);
}

TEST_CASE("every preset stretches and shifts", "[unit]")
{
	const StretchMode mode = GENERATE(StretchMode::Offline, StretchMode::RealTime);
	const std::string name = GENERATE(from_range(stretchPresetNames()));
	CAPTURE(toString(mode), name);
	const size_t frames = 48000;
	StretchSettings settings = settingsFor(mode, 2);
	settings.preset = stretchPresetFromString(name).value_or(StretchPreset::Quality);
	settings.ratio = 1.25;
	settings.semitones = 3.0;
	const std::vector<float> input = sine(330.0, frames);
	const Channels output = stretch({input, input}, settings);
	REQUIRE(output.size() == 2);
	const size_t expected = 60000;
	REQUIRE(output[0].size() == expected);
	REQUIRE(output[1].size() == expected);
	// Short windows and phase resets keep the timing of drums at the expense of pitch accuracy.
	const double tolerance = settings.preset == StretchPreset::Drums ? 30.0 : 5.0;
	CHECK(std::abs(cents(frequency(output[0], 9600, expected - 9600), 330.0 * pitchScale(3.0, 0.0))) < tolerance);
	CHECK(std::abs(decibels(rms(output[1], 9600, expected - 9600) / rms(input, 0, frames))) < 1.5);
}

TEST_CASE("formant preservation keeps the shifted pitch and changes the timbre", "[unit]")
{
	const StretchMode mode = GENERATE(StretchMode::Offline, StretchMode::RealTime);
	CAPTURE(toString(mode));
	const size_t frames = 48000;
	const std::vector<float> input = harmonicTone(frames);
	StretchSettings settings = settingsFor(mode);
	settings.semitones = 5.0;
	const Channels shifted = stretch({input}, settings);
	settings.preserveFormants = true;
	const Channels preserved = stretch({input}, settings);
	REQUIRE(preserved.front().size() == frames);
	CHECK(shifted != preserved);
	// The spectral envelope stays put, so the level stays close to the input's.
	CHECK(std::abs(decibels(rms(preserved.front(), 4800, frames - 4800) / rms(input, 0, frames))) < 3.0);
}

TEST_CASE("the unity ratio is near-transparent", "[unit]")
{
	const StretchMode mode = GENERATE(StretchMode::Offline, StretchMode::RealTime);
	CAPTURE(toString(mode));
	const size_t frames = 48000;
	const std::vector<float> tone = harmonicTone(frames);
	const std::vector<float> sine440 = sine(440.0, frames);
	for (const std::vector<float>* input : {&tone, &sine440})
	{
		const Channels output = stretch({*input}, settingsFor(mode));
		REQUIRE(output.front().size() == frames);
		std::vector<float> residual(frames);
		for (size_t i = 0; i < frames; ++i)
			residual[i] = output.front()[i] - (*input)[i];
		const size_t edge = 4800;
		const double level = decibels(rms(residual, edge, frames - edge) / rms(*input, edge, frames - edge));
		CAPTURE(level);
		// In real time the pitch path runs through a resampler even without a shift, so pitch can change while
		// playing; it leaves a delay of a fraction of a sample, which a null test sees (docs/time-stretch.md).
		CHECK(level < (mode == StretchMode::Offline ? -40.0 : -25.0));
		CHECK(std::abs(decibels(rms(output.front(), edge, frames - edge) / rms(*input, edge, frames - edge))) < 0.5);
	}
}

TEST_CASE("stretching is deterministic in both modes", "[unit]")
{
	const StretchMode mode = GENERATE(StretchMode::Offline, StretchMode::RealTime);
	CAPTURE(toString(mode));
	const std::vector<float> tone = harmonicTone(24000);
	const std::vector<float> high = sine(1234.5, 24000);
	StretchSettings settings = settingsFor(mode, 2);
	settings.ratio = 1.3;
	settings.semitones = -2.0;
	settings.cents = 17.0;
	const Channels first = stretch({tone, high}, settings);
	const Channels second = stretch({tone, high}, settings);
	CHECK(first == second);
}

TEST_CASE("the real-time process path neither allocates nor blocks", "[unit]")
{
	const std::string name = GENERATE(from_range(stretchPresetNames()));
	CAPTURE(name);
	StretchSettings settings = settingsFor(StretchMode::RealTime, 2);
	settings.preset = stretchPresetFromString(name).value_or(StretchPreset::Quality);
	settings.maxBlockFrames = 2048;
	TimeStretcher stretcher(settings);
	const std::vector<float> tone = harmonicTone(48000);
	const auto previous = gnubase::rt::setViolationHandler(&countViolation);
	g_violations = 0;
	// Ratio, pitch and formants change while it plays, as automation and tempo changes will.
	const Channels output = playRealtime(stretcher, {tone, tone}, 48000, 512,
		[](TimeStretcher& s, size_t block)
		{
			s.setRatio(1.0 + (0.05 * static_cast<double>(block % 8)));
			s.setPitch(static_cast<double>(block % 5) - 2.0, 25.0);
			s.setPreserveFormants(block % 2 == 0);
		});
	gnubase::rt::setViolationHandler(previous);
	CHECK(g_violations == 0);
	CHECK(output.front().size() == 48000);
}

TEST_CASE("real-time playback in blocks matches the latency-compensated whole", "[unit]")
{
	StretchSettings settings = settingsFor(StretchMode::RealTime);
	settings.ratio = 1.5;
	settings.semitones = 4.0;
	const std::vector<float> input = sine(440.0, 48000);
	TimeStretcher stretcher(settings);
	const Channels played = playRealtime(stretcher, {input}, 72000, 512);
	// Same latency compensation, same samples, whatever the block size the player pulls.
	TimeStretcher again(settings);
	const Channels inBigBlocks = playRealtime(again, {input}, 72000, 4096);
	CHECK(played == inBigBlocks);
	CHECK(std::abs(cents(frequency(played.front(), 9600, 72000 - 9600), 440.0 * pitchScale(4.0, 0.0))) < 1.0);
	CHECK(stretcher.latency() > 0);
}

TEST_CASE("media.stretch writes a stretched and shifted copy", "[integration]")
{
	const test::TempDir dir;
	const std::string input = (dir.path() / "take.wav").u8string();
	const std::string output = (dir.path() / "take stretched.wav").u8string();
	{
		WriteOptions options;
		options.sampleRate = g_rate;
		options.channels = 1;
		const auto writer = AudioFileWriter::create(input, options);
		const std::vector<float> samples = sine(440.0, 48000);
		writer->write(samples.data(), samples.size());
		writer->close();
	}
	Engine engine;
	Client client(engine);
	const Result added = client.call("media.add", {{"path", input}});
	REQUIRE(added.ok());
	const Json source = added.value.at("media").at("id");
	const Result stretched = client.call("media.stretch",
		{{"media", source}, {"path", output}, {"ratio", 1.5}, {"semitones", 12}, {"preset", "efficient"}});
	INFO(stretched.error);
	REQUIRE(stretched.ok());
	const Json& media = stretched.value.at("media");
	CHECK(media.at("length") == 72000);
	CHECK(media.at("channels") == 1);
	CHECK(media.at("path") == output);
	CHECK(media.at("id") != source);

	const auto reader = AudioFileReader::open(output);
	CHECK(reader->info().sampleFormat == SampleFormat::Float32);
	REQUIRE(reader->info().frames == 72000);
	std::vector<float> samples(72000);
	REQUIRE(reader->read(samples.data(), samples.size()) == samples.size());
	CHECK(std::abs(cents(frequency(samples, 9600, 72000 - 9600), 880.0)) < 2.0);

	// The new media file is one undo step.
	CHECK(client.call("project.get").value.at("media").size() == 2);
	REQUIRE(client.call("history.undo").ok());
	CHECK(client.call("project.get").value.at("media").size() == 1);

	CHECK_FALSE(client.call("media.stretch", {{"media", 999}, {"path", output}}).ok());
	CHECK_FALSE(client.call("media.stretch", {{"media", source}, {"path", output}, {"ratio", 0}}).ok());
	CHECK_FALSE(client.call("media.stretch", {{"media", source}, {"path", output}, {"semitones", 100}}).ok());
	CHECK_FALSE(client.call("media.stretch", {{"media", source}, {"path", output}, {"preset", "fast"}}).ok());
	CHECK_FALSE(client.call("media.stretch", {{"media", source}, {"path", ""}}).ok());
	const std::string other = (dir.path() / "take faster.wav").u8string();
	CHECK_FALSE(
		client.call("media.stretch", {{"media", source}, {"path", other}, {"ratio", 8}, {"mode", "realtime"}}).ok());
	const Result realtime = client.call("media.stretch",
		{{"media", source}, {"path", other}, {"ratio", 0.5}, {"mode", "realtime"}, {"formants", true}});
	INFO(realtime.error);
	REQUIRE(realtime.ok());
	CHECK(realtime.value.at("media").at("length") == 24000);
}
