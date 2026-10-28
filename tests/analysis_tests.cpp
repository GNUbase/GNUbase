// SPDX-License-Identifier: GPL-3.0-or-later
// Audio analysis (#28): levels, loudness and spectra of generated reference signals, and the commands that measure,
// compare and record the captured output.
#include "engine/analysis/analysis.h"
#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"
#include "mcp/server.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;
namespace analysis = gnubase::engine::analysis;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;

	// A sine at an amplitude in dBFS, i.e. its peak level.
	std::vector<float> sine(double frequency, double dbfs, double seconds, double sampleRate = 48000.0)
	{
		const double amplitude = std::pow(10.0, dbfs / 20.0);
		std::vector<float> samples(static_cast<size_t>(std::lround(seconds * sampleRate)));
		for (size_t i = 0; i < samples.size(); ++i)
			samples[i] =
				static_cast<float>(amplitude * std::sin(2.0 * g_pi * frequency * static_cast<double>(i) / sampleRate));
		return samples;
	}

	// The EBU Tech 3341 loudness test signals: a stereo 1 kHz sine in sections of {dBFS, seconds}.
	std::vector<std::vector<float>> sections(
		const std::vector<std::pair<double, double>>& levels, double sampleRate = 48000.0)
	{
		std::vector<float> channel;
		for (const auto& [dbfs, seconds] : levels)
		{
			const std::vector<float> part = sine(1000.0, dbfs, seconds, sampleRate);
			channel.insert(channel.end(), part.begin(), part.end());
		}
		return {channel, channel};
	}

	double loudness(const std::vector<std::vector<float>>& channels, double sampleRate = 48000.0)
	{
		const std::optional<double> lufs = analysis::integratedLoudness(channels, sampleRate);
		REQUIRE(lufs.has_value());
		return lufs.value_or(0.0);
	}

	// Runs a command and fails the test if it fails.
	Json call(Client& client, const std::string& command, const Json& parameters = Json::object())
	{
		const gnubase::engine::Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	// The device input: a sine of the given peak level in dBFS.
	Json sineInput(double frequency, double dbfs)
	{
		return {
			{"processor", "sine"}, {"parameters", {{"Frequency", frequency}, {"Level", std::pow(10.0, dbfs / 20.0)}}}};
	}

	// An engine whose audio track plays a sine on the device input to both outputs, rendered offline.
	void renderSine(Client& client, double frequency, double dbfs, int64_t frames, double sampleRate = 48000.0)
	{
		call(client, "project.new");
		call(client, "track.add", {{"type", "audio"}});
		call(client, "audio.setDevice",
			{{"sampleRate", sampleRate}, {"captureSeconds", 30}, {"input", sineInput(frequency, dbfs)}});
		call(client, "audio.render", {{"frames", frames}});
	}

	std::vector<float> readChannel(const std::string& path, int channel)
	{
		const auto reader = gnubase::engine::AudioFileReader::open(path);
		const int channels = reader->info().channels;
		std::vector<float> frames(static_cast<size_t>(reader->info().frames) * static_cast<size_t>(channels));
		frames.resize(
			reader->read(frames.data(), frames.size() / static_cast<size_t>(channels)) * static_cast<size_t>(channels));
		std::vector<float> samples;
		for (auto i = static_cast<size_t>(channel); i < frames.size(); i += static_cast<size_t>(channels))
			samples.push_back(frames[i]);
		return samples;
	}
}

TEST_CASE("peak and RMS levels of generated signals", "[unit]")
{
	const analysis::Levels tone = analysis::levels(sine(1000.0, -6.0206, 1.0));
	CHECK_THAT(tone.peak, WithinAbs(0.5, 1e-6));
	CHECK_THAT(tone.rms, WithinAbs(0.5 / std::sqrt(2.0), 1e-6));

	const analysis::Levels offset = analysis::levels(std::vector<float>(1000, -0.25f));
	CHECK(offset.peak == 0.25);
	CHECK_THAT(offset.rms, WithinAbs(0.25, 1e-12));

	const analysis::Levels nothing = analysis::levels({});
	CHECK(nothing.peak == 0.0);
	CHECK(nothing.rms == 0.0);

	CHECK_THAT(analysis::decibels(0.5).value_or(0.0), WithinAbs(-6.0206, 1e-4));
	CHECK_THAT(analysis::decibels(1.0).value_or(1.0), WithinAbs(0.0, 1e-12));
	CHECK_FALSE(analysis::decibels(0.0).has_value()); // silence has no level
}

TEST_CASE("the K-weighting filter has the coefficients of ITU-R BS.1770 at 48 kHz", "[unit]")
{
	const auto [shelf, highPass] = analysis::kWeighting(48000.0);
	CHECK_THAT(shelf.b0, WithinAbs(1.53512485958697, 1e-9));
	CHECK_THAT(shelf.b1, WithinAbs(-2.69169618940638, 1e-9));
	CHECK_THAT(shelf.b2, WithinAbs(1.19839281085285, 1e-9));
	CHECK_THAT(shelf.a1, WithinAbs(-1.69065929318241, 1e-9));
	CHECK_THAT(shelf.a2, WithinAbs(0.73248077421585, 1e-9));
	CHECK(highPass.b0 == 1.0);
	CHECK(highPass.b1 == -2.0);
	CHECK(highPass.b2 == 1.0);
	CHECK_THAT(highPass.a1, WithinAbs(-1.99004745483398, 1e-9));
	CHECK_THAT(highPass.a2, WithinAbs(0.99007225036621, 1e-9));
}

TEST_CASE("integrated loudness matches the EBU Tech 3341 test signals", "[unit]")
{
	CHECK_THAT(loudness(sections({{-23.0, 20.0}})), WithinAbs(-23.0, 0.1));
	CHECK_THAT(loudness(sections({{-33.0, 20.0}})), WithinAbs(-33.0, 0.1));
	// The relative gate leaves out the quiet parts; the absolute gate the silent ones.
	CHECK_THAT(loudness(sections({{-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}})), WithinAbs(-23.0, 0.1));
	CHECK_THAT(loudness(sections({{-72.0, 10.0}, {-36.0, 10.0}, {-23.0, 20.0}, {-36.0, 10.0}, {-72.0, 10.0}})),
		WithinAbs(-23.0, 0.1));
	CHECK_THAT(loudness(sections({{-26.0, 20.0}, {-20.0, 20.1}, {-26.0, 20.0}})), WithinAbs(-23.0, 0.1));

	// Other sample rates, and BS.1770's reference: a full-scale 1 kHz sine in one channel is -3.01 LUFS.
	CHECK_THAT(loudness(sections({{-23.0, 20.0}}, 44100.0), 44100.0), WithinAbs(-23.0, 0.1));
	CHECK_THAT(loudness(sections({{-23.0, 20.0}}, 96000.0), 96000.0), WithinAbs(-23.0, 0.1));
	CHECK_THAT(loudness({sine(1000.0, 0.0, 5.0)}), WithinAbs(-3.01, 0.05));

	// Less than one 400 ms block, or nothing above the absolute gate, has no loudness.
	CHECK_FALSE(analysis::integratedLoudness({sine(1000.0, -20.0, 0.3)}, 48000.0).has_value());
	CHECK_FALSE(analysis::integratedLoudness({std::vector<float>(48000, 0.0f)}, 48000.0).has_value());
	CHECK_FALSE(analysis::integratedLoudness({}, 48000.0).has_value());
}

TEST_CASE("the spectrum summary finds the strongest frequencies and the level of octave bands", "[unit]")
{
	std::vector<float> mix = sine(440.0, -6.0206, 1.0);
	const std::vector<float> quieter = sine(3000.0, -20.0, 1.0);
	for (size_t i = 0; i < mix.size(); ++i)
		mix[i] += quieter[i];
	const analysis::Spectrum spectrum = analysis::spectrum(mix, 48000.0, 5);
	REQUIRE(spectrum.peaks.size() == 2); // no leakage passes as a peak
	CHECK_THAT(spectrum.peaks[0].frequency, WithinAbs(440.0, 0.5));
	CHECK_THAT(spectrum.peaks[0].level, WithinAbs(-6.02, 0.1));
	CHECK_THAT(spectrum.peaks[1].frequency, WithinAbs(3000.0, 0.5));
	CHECK_THAT(spectrum.peaks[1].level, WithinAbs(-20.0, 0.1));

	// Octave bands from 31.25 Hz to 16 kHz; each has the power of what it contains, like an RMS level.
	REQUIRE(spectrum.bands.size() == 10);
	CHECK(spectrum.bands.front().frequency == 31.25);
	CHECK(spectrum.bands.back().frequency == 16000.0);
	CHECK(spectrum.bands[4].frequency == 500.0);
	CHECK_THAT(spectrum.bands[4].level.value_or(0.0), WithinAbs(-9.03, 0.1));  // 440 Hz
	CHECK_THAT(spectrum.bands[7].level.value_or(0.0), WithinAbs(-23.01, 0.1)); // 3 kHz
	CHECK(spectrum.bands[5].level.value_or(0.0) < -80.0);                      // 1 kHz: nothing there

	// Fewer peaks when asked, short signals, and silence.
	CHECK(analysis::spectrum(mix, 48000.0, 1).peaks.size() == 1);
	const analysis::Spectrum shortTone = analysis::spectrum(sine(1000.0, -6.0206, 0.05), 48000.0, 5);
	REQUIRE(shortTone.peaks.size() == 1);
	CHECK_THAT(shortTone.peaks[0].frequency, WithinAbs(1000.0, 1.0));
	const analysis::Spectrum silence = analysis::spectrum(std::vector<float>(4800, 0.0f), 48000.0, 5);
	CHECK(silence.peaks.empty());
	CHECK_FALSE(silence.bands[5].level.has_value());
	CHECK(analysis::spectrum({}, 48000.0, 5).peaks.empty());

	// At a lower sample rate there are only the bands below the Nyquist frequency.
	CHECK(analysis::spectrum(sine(440.0, -6.0, 1.0, 8000.0), 8000.0, 5).bands.size() == 8);
}

TEST_CASE("audio.analyze measures the captured output", "[integration]")
{
	const int64_t twentySeconds = int64_t{20} * 48000;
	Engine engine;
	Client client(engine);
	renderSine(client, 1000.0, -23.0, twentySeconds);

	const Json all = call(client, "audio.analyze");
	CHECK(all.at("sampleRate") == 48000.0);
	CHECK(all.at("start") == 0);
	CHECK(all.at("frames") == twentySeconds);
	CHECK_THAT(all.at("loudness").get<double>(), WithinAbs(-23.0, 0.1));
	REQUIRE(all.at("channels").size() == 2);
	const Json& left = all.at("channels").at(0);
	CHECK(left.at("channel") == 0);
	CHECK_THAT(left.at("peakDb").get<double>(), WithinAbs(-23.0, 0.01));
	CHECK_THAT(left.at("rmsDb").get<double>(), WithinAbs(-26.01, 0.01));
	CHECK_THAT(left.at("peak").get<double>(), WithinAbs(std::pow(10.0, -23.0 / 20.0), 1e-4));
	const Json& strongest = left.at("spectrum").at("peaks").at(0);
	CHECK_THAT(strongest.at("frequency").get<double>(), WithinAbs(1000.0, 0.5));
	CHECK_THAT(strongest.at("level").get<double>(), WithinAbs(-23.0, 0.1));
	CHECK(left.at("spectrum").at("bands").size() == 10);

	// A part of one channel: 100 ms is too short for a loudness.
	const Json part =
		call(client, "audio.analyze", {{"channels", {1}}, {"start", 48000}, {"frames", 4800}, {"peaks", 1}});
	CHECK(part.at("start") == 48000);
	CHECK(part.at("frames") == 4800);
	CHECK(part.at("loudness").is_null());
	REQUIRE(part.at("channels").size() == 1);
	CHECK(part.at("channels").at(0).at("channel") == 1);
	CHECK(part.at("channels").at(0).at("spectrum").at("peaks").size() == 1);

	CHECK_THAT(client.call("audio.analyze", {{"channels", {2}}}).error, ContainsSubstring("no output channel 2"));
	CHECK_THAT(client.call("audio.analyze", {{"start", twentySeconds + 1}}).error, ContainsSubstring("captured"));
	CHECK_THAT(client.call("audio.analyze", {{"frames", twentySeconds + 1}}).error, ContainsSubstring("captured"));
	CHECK_THAT(client.call("audio.analyze", {{"peaks", 21}}).error, ContainsSubstring("peaks"));
}

TEST_CASE("audio.record writes the captured output to an audio file", "[integration]")
{
	const gnubase::test::TempDir dir;
	Engine engine;
	Client client(engine);
	renderSine(client, 440.0, -6.0206, 4800);
	const std::vector<float> left = call(client, "audio.capture", {{"channel", 0}, {"start", 0}, {"frames", 4800}})
										.at("samples")
										.get<std::vector<float>>();
	const std::vector<float> right = call(client, "audio.capture", {{"channel", 1}, {"start", 0}, {"frames", 4800}})
										 .at("samples")
										 .get<std::vector<float>>();

	// Everything, as 32-bit float WAV.
	const std::string path = (dir.path() / "take.wav").u8string();
	const Json written = call(client, "audio.record", {{"path", path}});
	CHECK(written.at("path") == std::filesystem::absolute(dir.path() / "take.wav").u8string());
	CHECK(written.at("frames") == 4800);
	CHECK(written.at("channels") == Json{0, 1});
	CHECK(written.at("sampleRate") == 48000.0);
	{
		const auto reader = gnubase::engine::AudioFileReader::open(path);
		CHECK(reader->info().format == gnubase::engine::AudioFormat::Wav);
		CHECK(reader->info().sampleFormat == gnubase::engine::SampleFormat::Float32);
		CHECK(reader->info().channels == 2);
		CHECK(reader->info().sampleRate == 48000.0);
		CHECK(reader->info().frames == 4800);
	}
	CHECK(readChannel(path, 0) == left);
	CHECK(readChannel(path, 1) == right);

	// A part of one channel, as 16-bit integers.
	const std::string part = (dir.path() / "part.wav").u8string();
	call(client, "audio.record",
		{{"path", part}, {"channels", {1}}, {"start", 100}, {"frames", 1000}, {"format", "int16"}});
	const std::vector<float> partSamples = readChannel(part, 0);
	REQUIRE(partSamples.size() == 1000);
	for (size_t i = 0; i < partSamples.size(); ++i)
		CHECK_THAT(partSamples[i], WithinAbs(right[100 + i], 1.0 / 32768.0));
	CHECK(gnubase::engine::AudioFileReader::open(part)->info().sampleFormat == gnubase::engine::SampleFormat::Int16);

	CHECK_THAT(client.call("audio.record", {{"path", path}, {"format", "mp3"}}).error, ContainsSubstring("format"));
	CHECK_THAT(client.call("audio.record", {{"path", (dir.path() / "missing" / "take.wav").u8string()}}).error,
		ContainsSubstring("take.wav"));
}

TEST_CASE("audio.compare null-tests the captured output against a reference file", "[integration]")
{
	const gnubase::test::TempDir dir;
	Engine engine;
	Client client(engine);
	renderSine(client, 440.0, -6.0206, 4800);
	const std::string reference = (dir.path() / "reference.wav").u8string();
	call(client, "audio.record", {{"path", reference}});

	// The same audio cancels completely.
	const Json same = call(client, "audio.compare", {{"reference", reference}});
	CHECK(same.at("frames") == 4800);
	CHECK(same.at("peak") == 0.0);
	CHECK(same.at("peakDb").is_null());
	REQUIRE(same.at("channels").size() == 2);
	CHECK(same.at("channels").at(1).at("channel") == 1);
	CHECK(same.at("channels").at(1).at("rmsDb").is_null());

	// At half the level, half of the reference remains.
	renderSine(client, 440.0, -12.0412, 4800);
	const Json half = call(client, "audio.compare", {{"reference", reference}});
	CHECK_THAT(half.at("peak").get<double>(), WithinAbs(0.25, 1e-4));
	CHECK_THAT(half.at("peakDb").get<double>(), WithinAbs(-12.04, 0.01));
	CHECK_THAT(half.at("channels").at(0).at("rms").get<double>(), WithinAbs(0.25 / std::sqrt(2.0), 1e-3));

	// One channel against a mono file, from a later frame.
	const std::string mono = (dir.path() / "mono.wav").u8string();
	call(client, "audio.record", {{"path", mono}, {"channels", {1}}, {"start", 1000}, {"frames", 2000}});
	const Json part = call(client, "audio.compare", {{"reference", mono}, {"channels", {1}}, {"start", 1000}});
	CHECK(part.at("frames") == 2000);
	CHECK(part.at("peak") == 0.0);
	CHECK(part.at("channels").at(0).at("channel") == 1);
	const Json shifted = call(client, "audio.compare", {{"reference", mono}, {"channels", {1}}, {"start", 1001}});
	CHECK(shifted.at("peak").get<double>() > 0.001); // one frame off doesn't cancel

	CHECK_THAT(client.call("audio.compare", {{"reference", mono}}).error, ContainsSubstring("1 channel"));
	CHECK_THAT(client.call("audio.compare", {{"reference", mono}, {"channels", {0}}, {"start", 4000}}).error,
		ContainsSubstring("captured"));
	CHECK_THAT(client.call("audio.compare", {{"reference", (dir.path() / "none.wav").u8string()}}).error,
		ContainsSubstring("none.wav"));
	renderSine(client, 440.0, -6.0206, 4800, 44100.0);
	CHECK_THAT(client.call("audio.compare", {{"reference", reference}}).error, ContainsSubstring("sample rate"));
}

TEST_CASE("MCP clients measure the captured output with audio_analyze", "[integration]")
{
	Engine engine;
	gnubase::mcp::Server server(engine);
	{
		Client client(engine);
		renderSine(client, 1000.0, -20.0, 48000);
	}
	const Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
		{"params", {{"name", "audio_analyze"}, {"arguments", {{"channels", {0}}, {"peaks", 1}}}}}};
	const Json result = server.handle(request).value_or(Json()).at("result");
	CHECK(result.at("isError") == false);
	const Json& channel = result.at("structuredContent").at("channels").at(0);
	CHECK_THAT(channel.at("peakDb").get<double>(), WithinAbs(-20.0, 0.01));
	CHECK_THAT(channel.at("spectrum").at("peaks").at(0).at("frequency").get<double>(), WithinAbs(1000.0, 0.5));
}
