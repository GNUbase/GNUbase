// SPDX-License-Identifier: GPL-3.0-or-later
// Exporting audio mixdowns (#115, docs/features/mixdown.md): mixdown.export and mixdown.quickExport.
#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;

namespace
{
	namespace fs = std::filesystem;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		// An export renders the whole project before it answers, which under sanitizers on a busy runner takes longer
		// than the client's default 10 s.
		const Result result = client.call(command, std::move(parameters), std::chrono::seconds(60));
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

	struct Read
	{
		AudioFileInfo info;
		std::vector<float> samples; // interleaved
	};

	Read readFile(const std::string& path)
	{
		const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(path);
		Read result{reader->info(), {}};
		result.samples.resize(static_cast<size_t>(result.info.frames * result.info.channels));
		REQUIRE(reader->read(result.samples.data(), static_cast<size_t>(result.info.frames)) ==
			static_cast<size_t>(result.info.frames));
		return result;
	}

	// Where two sample sequences first differ, or -1 if they are the same.
	int64_t firstDifference(const std::vector<float>& actual, const std::vector<float>& expected)
	{
		const size_t common = std::min(actual.size(), expected.size());
		for (size_t i = 0; i < common; ++i)
			if (actual[i] != expected[i])
			{
				UNSCOPED_INFO("sample " << i << " is " << actual[i] << " instead of " << expected[i]);
				return static_cast<int64_t>(i);
			}
		if (actual.size() != expected.size())
		{
			UNSCOPED_INFO(actual.size() << " samples instead of " << expected.size());
			return static_cast<int64_t>(common);
		}
		return -1;
	}

	float peakOf(const std::vector<float>& samples)
	{
		float peak = 0.0f;
		for (const float sample : samples)
			peak = std::max(peak, std::abs(sample));
		return peak;
	}

	// A project with stereo audio tracks that play a stereo ramp, every sample different, through the main mix.
	class Session
	{
	public:
		Session()
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
			gnubase::test::Audio audio;
			audio.channels = 2;
			for (int frame = 0; frame < 6000; ++frame)
				for (int c = 0; c < 2; ++c)
					audio.samples.push_back((c == 0 ? 1.0f : -1.0f) * static_cast<float>(frame + 1) / 8192.0f);
			const fs::path file = media.path() / "ramp.wav";
			gnubase::test::writeWav(file, audio);
			const Json added = run(client, "media.add", {{"path", file.u8string()}});
			clip = run(client, "clip.add", {{"media", added.at("media").at("id")}}).at("clip").at("id").get<ObjectId>();
		}

		// A track with the ramp at position, length frames of it; returns its channel.
		ObjectId addTrack(const std::string& name, int64_t position, int64_t length = 6000)
		{
			const Json added = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}});
			const auto track = added.at("track").at("id").get<ObjectId>();
			run(client, "track.set", {{"track", track}, {"name", name}});
			run(client, "audioEvent.add",
				{{"track", track}, {"clip", clip}, {"position", position}, {"offset", 0}, {"length", length}});
			return added.at("channel").at("id").get<ObjectId>();
		}

		Json exportMixdown(Json parameters)
		{
			parameters["folder"] = folder.path().u8string();
			if (!parameters.contains("name"))
				parameters["name"] = "mix";
			return run(client, "mixdown.export", std::move(parameters));
		}

		std::string exportedFile(Json parameters)
		{
			const Json result = exportMixdown(std::move(parameters));
			REQUIRE(result.at("files").size() == 1);
			return result.at("files").at(0).at("path").get<std::string>();
		}

		Engine engine;
		Client client{engine};
		gnubase::test::TempDir media;
		gnubase::test::TempDir folder;
		ObjectId clip = 0;
	};

	// What an offline render of the main mix gives from start, interleaved.
	std::vector<float> rendered(Session& project, int64_t start, int64_t frames)
	{
		run(project.client, "transport.locate", {{"position", start}});
		run(project.client, "transport.play");
		run(project.client, "audio.render", {{"frames", frames}});
		run(project.client, "transport.stop");
		const Json parameters = {{"start", 0}, {"frames", frames}};
		Json left = parameters;
		left["channel"] = 0;
		Json right = parameters;
		right["channel"] = 1;
		const auto l = run(project.client, "audio.capture", left).at("samples").get<std::vector<float>>();
		const auto r = run(project.client, "audio.capture", right).at("samples").get<std::vector<float>>();
		std::vector<float> interleaved;
		for (size_t i = 0; i < l.size(); ++i)
		{
			interleaved.push_back(l[i]);
			interleaved.push_back(r[i]);
		}
		return interleaved;
	}

	void waitUntilActive(Client& client, const Json& slot)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		std::string status;
		while (status != "active" && std::chrono::steady_clock::now() < deadline)
		{
			status = run(client, "plugin.get", {{"slot", slot}}).at("slot").at("status").get<std::string>();
			if (status != "active")
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(status == "active");
		run(client, "audio.updatePlugins");
	}
}

TEST_CASE("an exported mixdown is bit-identical to an offline render of the same range", "[integration]")
{
	Session exported;
	exported.addTrack("Ramp", 1000);
	run(exported.client, "transport.locate", {{"position", 777}});
	const Json result = exported.exportMixdown({{"start", 1500}, {"end", 4500}, {"sampleFormat", "float32"}});
	REQUIRE(result.at("files").size() == 1);
	const Json& file = result.at("files").at(0);
	CHECK_THAT(file.at("path").get<std::string>(), EndsWith("mix.wav"));
	CHECK(file.at("frames") == 3000);
	const Read read = readFile(file.at("path").get<std::string>());
	CHECK(read.info.format == AudioFormat::Wav);
	CHECK(read.info.sampleFormat == SampleFormat::Float32);
	CHECK(read.info.channels == 2);
	CHECK(read.info.sampleRate == 48000.0);

	Session offline;
	offline.addTrack("Ramp", 1000);
	const std::vector<float> expected = rendered(offline, 1500, 3000);
	CHECK(peakOf(expected) > 0.4f);
	CHECK(firstDifference(read.samples, expected) == -1);

	// The transport is back where it was.
	const Json transport = run(exported.client, "transport.get");
	CHECK(transport.at("position") == 777);
	CHECK(transport.at("playing") == false);
}

TEST_CASE("a mixdown exports the locator range, and a file per channel", "[integration]")
{
	Session project;
	const ObjectId bass = project.addTrack("Bass", 0);
	const ObjectId drums = project.addTrack("Drums", 2000);
	run(project.client, "transport.set",
		{{"left", 1000}, {"right", 5000}, {"cycle", true}, {"preRoll", true}, {"preRollAmount", 500}});
	const Json result =
		project.exportMixdown({{"name", "stems"}, {"channels", {bass, drums}}, {"sampleFormat", "float32"}});
	REQUIRE(result.at("files").size() == 2);
	CHECK_THAT(result.at("files").at(0).at("path").get<std::string>(), EndsWith("stems - Bass.wav"));
	CHECK(result.at("files").at(0).at("channel") == bass);
	CHECK_THAT(result.at("files").at(1).at("path").get<std::string>(), EndsWith("stems - Drums.wav"));
	const Read bassFile = readFile(result.at("files").at(0).at("path").get<std::string>());
	const Read drumsFile = readFile(result.at("files").at(1).at("path").get<std::string>());
	CHECK(bassFile.info.frames == 4000);

	// A channel's file is what the main mix plays with the other channel muted. Mute glides over 5 ms, so 512 frames
	// play first.
	run(project.client, "channel.set", {{"channel", drums}, {"mute", true}});
	run(project.client, "audio.render", {{"frames", 512}});
	CHECK(firstDifference(readFile(project.exportedFile({{"name", "bass alone"}, {"sampleFormat", "float32"}})).samples,
			  bassFile.samples) == -1);
	run(project.client, "channel.set", {{"channel", drums}, {"mute", false}});
	run(project.client, "channel.set", {{"channel", bass}, {"mute", true}});
	run(project.client, "audio.render", {{"frames", 512}});
	CHECK(
		firstDifference(readFile(project.exportedFile({{"name", "drums alone"}, {"sampleFormat", "float32"}})).samples,
			drumsFile.samples) == -1);
	CHECK(run(project.client, "transport.get").at("cycle") == true);
}

TEST_CASE("a mixdown is written as WAV, AIFF or FLAC in their sample formats", "[integration]")
{
	Session project;
	project.addTrack("Ramp", 0);
	const Json range = {{"start", 0}, {"end", 3000}};
	Json parameters = range;
	parameters["sampleFormat"] = "float32";
	const Read reference = readFile(project.exportedFile(parameters));

	struct Case
	{
		const char* format;
		const char* sampleFormat;
		AudioFormat expected;
		SampleFormat expectedSamples;
		const char* extension;
	};
	for (const Case& item : {Case{"wav", "int16", AudioFormat::Wav, SampleFormat::Int16, ".wav"},
			 Case{"wav", "float64", AudioFormat::Wav, SampleFormat::Float64, ".wav"},
			 Case{"aiff", "int24", AudioFormat::Aiff, SampleFormat::Int24, ".aif"},
			 Case{"flac", "int16", AudioFormat::Flac, SampleFormat::Int16, ".flac"},
			 Case{"flac", "int24", AudioFormat::Flac, SampleFormat::Int24, ".flac"}})
	{
		INFO(item.format << " " << item.sampleFormat);
		parameters = range;
		parameters["format"] = item.format;
		parameters["sampleFormat"] = item.sampleFormat;
		const std::string path = project.exportedFile(parameters);
		CHECK_THAT(path, EndsWith(item.extension));
		const Read read = readFile(path);
		CHECK(read.info.format == item.expected);
		CHECK(read.info.sampleFormat == item.expectedSamples);
		CHECK(read.info.frames == 3000);
		REQUIRE(read.samples.size() == reference.samples.size());
		const float step = item.expectedSamples == SampleFormat::Int16 ? 1.0f / 32768.0f : 1.0f / 8388608.0f;
		float error = 0.0f;
		for (size_t i = 0; i < read.samples.size(); ++i)
			error = std::max(error, std::abs(read.samples[i] - reference.samples[i]));
		CHECK(error <= step);
	}
	// The default is a 24-bit WAV file.
	const Read standard = readFile(project.exportedFile(range));
	CHECK(standard.info.format == AudioFormat::Wav);
	CHECK(standard.info.sampleFormat == SampleFormat::Int24);
}

TEST_CASE("a mixdown converts the sample rate, dithers and normalizes", "[integration]")
{
	Session project;
	project.addTrack("Ramp", 0);

	const Read converted = readFile(
		project.exportedFile({{"start", 0}, {"end", 3000}, {"sampleRate", 44100}, {"sampleFormat", "float32"}}));
	CHECK(converted.info.sampleRate == 44100.0);
	CHECK(converted.info.frames == Resampler::outputLength(3000, 48000.0, 44100.0));

	const Json range = {{"start", 0}, {"end", 3000}, {"sampleFormat", "int16"}};
	const Read plain = readFile(project.exportedFile(range));
	Json dithered = range;
	dithered["dither"] = true;
	const Read first = readFile(project.exportedFile(dithered));
	const Read second = readFile(project.exportedFile(dithered));
	CHECK(firstDifference(first.samples, second.samples) == -1); // the same noise every time
	CHECK(firstDifference(first.samples, plain.samples) >= 0);
	float error = 0.0f;
	for (size_t i = 0; i < plain.samples.size(); ++i)
		error = std::max(error, std::abs(first.samples[i] - plain.samples[i]));
	CHECK(error <= 2.0f / 32768.0f);

	const Read loud = readFile(project.exportedFile({{"start", 0}, {"end", 3000}, {"sampleFormat", "float32"}}));
	const Read normalized =
		readFile(project.exportedFile({{"start", 0}, {"end", 3000}, {"sampleFormat", "float32"}, {"normalize", -1.0}}));
	const float gain = std::pow(10.0f, -1.0f / 20.0f) / peakOf(loud.samples);
	CHECK(std::abs(peakOf(normalized.samples) - std::pow(10.0f, -1.0f / 20.0f)) < 1e-6f);
	CHECK(std::abs(normalized.samples[100] - (loud.samples[100] * gain)) < 1e-6f);
}

TEST_CASE("a mixdown takes a unique file name or overwrites", "[integration]")
{
	Session project;
	project.addTrack("Ramp", 0);
	const Json range = {{"start", 0}, {"end", 1000}};
	const std::string first = project.exportedFile(range);
	const std::string second = project.exportedFile(range);
	CHECK_THAT(second, EndsWith("mix (2).wav"));
	Json overwrite = range;
	overwrite["conflicts"] = "overwrite";
	CHECK(project.exportedFile(overwrite) == first);
	CHECK(std::distance(fs::directory_iterator(project.folder.path()), fs::directory_iterator()) == 2);
}

TEST_CASE("a mixdown refuses what it can't export and leaves no files", "[integration]")
{
	Session project;
	const ObjectId channel = project.addTrack("Ramp", 0);
	const auto midi = run(project.client, "track.add", {{"type", "midi"}}).at("channel").at("id").get<ObjectId>();
	const auto refuse = [&project](Json parameters)
	{
		parameters["folder"] = project.folder.path().u8string();
		parameters["name"] = "mix";
		return reject(project.client, "mixdown.export", std::move(parameters));
	};
	CHECK_THAT(refuse({{"start", 1000}, {"end", 1000}}), ContainsSubstring("empty"));
	CHECK_THAT(refuse({{"start", 0}, {"end", 1000}, {"channels", {9999}}}), ContainsSubstring("9999"));
	CHECK_THAT(refuse({{"start", 0}, {"end", 1000}, {"channels", {midi}}}), ContainsSubstring("no audio"));
	CHECK_THAT(refuse({{"start", 0}, {"end", 1000}, {"channels", {channel, channel}}}), ContainsSubstring("twice"));
	CHECK_THAT(refuse({{"start", 0}, {"end", 1000}, {"format", "flac"}, {"sampleFormat", "float32"}}),
		ContainsSubstring("FLAC"));
	CHECK_THAT(refuse({{"start", 0}, {"end", 1000}, {"format", "aiff"}, {"sampleFormat", "float64"}}),
		ContainsSubstring("AIFF"));

	// The graph would run at the device's rate and the events at the project's.
	run(project.client, "audio.setDevice",
		{{"sampleRate", 44100}, {"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	CHECK_THAT(refuse({{"start", 0}, {"end", 1000}}), ContainsSubstring("44100 Hz"));
	CHECK_THAT(reject(project.client, "mixdown.quickExport", {{"path", (project.folder.path() / "q.wav").u8string()}}),
		ContainsSubstring("sample rate"));
	CHECK(fs::directory_iterator(project.folder.path()) == fs::directory_iterator());
}

TEST_CASE("a quick export writes the main mix of every event", "[integration]")
{
	Session project;
	project.addTrack("Ramp", 2000, 4800);
	const fs::path path = project.folder.path() / "quick.wav";
	const Json result = run(project.client, "mixdown.quickExport", {{"path", path.u8string()}});
	CHECK(result.at("frames") == 4800);
	const Read quick = readFile(path.u8string());
	CHECK(quick.info.sampleFormat == SampleFormat::Int24);
	CHECK(quick.info.sampleRate == 48000.0);
	CHECK(
		firstDifference(quick.samples, readFile(project.exportedFile({{"start", 2000}, {"end", 6800}})).samples) == -1);

	Session empty;
	CHECK_THAT(reject(empty.client, "mixdown.quickExport", {{"path", path.u8string()}}), ContainsSubstring("events"));
}

TEST_CASE("a quick export keeps the tail of effects", "[integration][slow]")
{
	Session project;
	const ObjectId channel = project.addTrack("Ramp", 0, 4800);
	const Json slot =
		run(project.client, "plugin.insert", {{"channel", channel}, {"plugin", "builtin:delay"}}).at("slot").at("id");
	waitUntilActive(project.client, slot);
	const fs::path path = project.folder.path() / "tail.wav";
	const auto frames = run(project.client, "mixdown.quickExport", {{"path", path.u8string()}}).at("frames");
	CHECK(frames > 4800 + 24000); // at least the first echo, 500 ms on
	CHECK(frames < 4800 + (30 * 48000));
	const Read read = readFile(path.u8string());
	CHECK(std::max(std::abs(read.samples[read.samples.size() - 2]), std::abs(read.samples.back())) > 0.0f);
}
