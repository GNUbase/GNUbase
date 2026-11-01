// SPDX-License-Identifier: GPL-3.0-or-later
// Audio file import (#92): audioFile.import puts audio files on tracks as events, copying, converting and splitting
// them on the way as the import options say.
#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;
namespace test = gnubase::test;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

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
		REQUIRE(!result.ok());
		return result.error;
	}

	std::string utf8(const fs::path& path)
	{
		return path.u8string();
	}

	std::string dataFile(const char* name)
	{
		return utf8(fs::path(GNUBASE_TEST_DATA_DIR) / name);
	}

	// A different ramp per channel, so that every channel and frame can be told apart.
	std::vector<float> rampSamples(int channels, int64_t frames)
	{
		std::vector<float> samples(static_cast<size_t>(frames) * static_cast<size_t>(channels));
		for (int64_t frame = 0; frame < frames; ++frame)
			for (int channel = 0; channel < channels; ++channel)
				samples[(static_cast<size_t>(frame) * static_cast<size_t>(channels)) + static_cast<size_t>(channel)] =
					(static_cast<float>(channel + 1) * 0.1f) + (static_cast<float>(frame % 100) * 0.001f);
		return samples;
	}

	void writeFile(const fs::path& path, int64_t frames, int channels = 2, double sampleRate = 48000.0,
		AudioFormat format = AudioFormat::Wav, SampleFormat sampleFormat = SampleFormat::Float32,
		Rf64 rf64 = Rf64::Auto)
	{
		fs::create_directories(path.parent_path());
		WriteOptions options;
		options.format = format;
		options.sampleFormat = sampleFormat;
		options.sampleRate = sampleRate;
		options.channels = channels;
		options.rf64 = rf64;
		const auto writer = AudioFileWriter::create(utf8(path), options);
		const std::vector<float> samples = rampSamples(channels, frames);
		writer->write(samples.data(), static_cast<size_t>(frames));
		writer->close();
	}

	// A Wave64 file of 16-bit mono PCM at 48 kHz, which AudioFileWriter doesn't write: a riff GUID with the file's
	// size, the wave GUID, and fmt and data chunks, each a GUID and a 64-bit size that counts the chunk's 24-byte
	// header, padded to 8 bytes.
	void writeWave64(const fs::path& path, int64_t frames)
	{
		std::vector<uint8_t> bytes;
		const auto integer = [&bytes](uint64_t value, unsigned size)
		{
			for (unsigned i = 0; i < size; ++i)
				bytes.push_back(static_cast<uint8_t>(value >> (8U * i)));
		};
		const auto guid = [&bytes](const char* fourcc, bool riff)
		{
			static constexpr uint8_t riffTail[12] = {0x2e, 0x91, 0xcf, 0x11, 0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0, 0};
			static constexpr uint8_t tail[12] = {
				0xf3, 0xac, 0xd3, 0x11, 0x8c, 0xd1, 0x00, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a};
			bytes.insert(bytes.end(), fourcc, fourcc + 4);
			bytes.insert(bytes.end(), riff ? std::begin(riffTail) : std::begin(tail),
				riff ? std::end(riffTail) : std::end(tail));
		};
		const auto dataSize = static_cast<uint64_t>(frames) * 2;
		const uint64_t padding = (8 - (dataSize % 8)) % 8;
		guid("riff", true);
		integer(40 + 40 + 24 + dataSize + padding, 8);
		guid("wave", false);
		guid("fmt ", false);
		integer(24 + 16, 8);
		integer(1, 2); // PCM
		integer(1, 2); // channels
		integer(48000, 4);
		integer(96000, 4); // bytes per second
		integer(2, 2);     // bytes per frame
		integer(16, 2);    // bits per sample
		guid("data", false);
		integer(24 + dataSize, 8);
		for (int64_t frame = 0; frame < frames; ++frame)
			integer(static_cast<uint64_t>((frame % 100) * 100), 2);
		bytes.resize(bytes.size() + padding);
		std::ofstream(path, std::ios::binary)
			.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	std::vector<float> readAll(const std::string& path)
	{
		const auto reader = AudioFileReader::open(path);
		const auto channels = static_cast<size_t>(reader->info().channels);
		std::vector<float> samples(static_cast<size_t>(reader->info().frames) * channels);
		samples.resize(reader->read(samples.data(), static_cast<size_t>(reader->info().frames)) * channels);
		return samples;
	}

	// Ticks at 120 BPM, the new project's tempo.
	int64_t ticksAt(int64_t samples)
	{
		return gnubase::time::TempoMap(120.0).toTicks(samples, 48000.0);
	}

	Json byId(const Json& list, const Json& id)
	{
		for (const Json& item : list)
			if (item.at("id") == id)
				return item;
		FAIL_CHECK("no " << id);
		return {};
	}

	Json waitForJob(Client& client, uint64_t job)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (std::chrono::steady_clock::now() < deadline)
		{
			client.wait(std::chrono::milliseconds(20));
			if (Json state = run(client, "job.get", {{"job", job}}).at("job"); state.at("state") != "running")
				return state;
		}
		FAIL_CHECK("the job didn't finish");
		return {};
	}

	// An engine with a saved project, so that imports can copy and convert into its media folder.
	struct SavedProject
	{
		test::TempDir dir;
		Engine engine;
		Client client{engine};
		fs::path folder = dir.path() / "project";
		fs::path media = folder / "Audio";
		fs::path library = dir.path() / "library";

		SavedProject()
		{
			fs::create_directories(folder);
			run(client, "project.save", {{"path", utf8(folder / "Song.dawproject")}, {"embedMedia", false}});
		}
	};
}

TEST_CASE("audioFile.import puts files of every supported format on new tracks at the position", "[integration]")
{
	const test::TempDir dir;
	struct Written
	{
		const char* name;
		AudioFormat format;
		SampleFormat sampleFormat;
		int channels;
	};
	std::vector<std::string> files;
	for (const Written& file : {Written{"int16.wav", AudioFormat::Wav, SampleFormat::Int16, 1},
			 Written{"int24.wav", AudioFormat::Wav, SampleFormat::Int24, 2},
			 Written{"float.wav", AudioFormat::Wav, SampleFormat::Float32, 2},
			 Written{"take.aiff", AudioFormat::Aiff, SampleFormat::Int24, 2},
			 Written{"take.flac", AudioFormat::Flac, SampleFormat::Int16, 1}})
	{
		writeFile(dir.path() / file.name, 4800, file.channels, 48000.0, file.format, file.sampleFormat);
		files.push_back(utf8(dir.path() / file.name));
	}
	writeFile(dir.path() / "rf64.wav", 4800, 2, 48000.0, AudioFormat::Wav, SampleFormat::Int24, Rf64::Always);
	files.push_back(utf8(dir.path() / "rf64.wav"));
	writeWave64(dir.path() / "take.w64", 4800);
	REQUIRE(AudioFileReader::open(utf8(dir.path() / "take.w64"))->info().frames == 4800);
	files.push_back(utf8(dir.path() / "take.w64"));
	files.push_back(dataFile("sine440.ogg"));
	files.push_back(dataFile("sine440.mp3"));

	Engine engine;
	Client client(engine);
	const Json result = run(client, "audioFile.import", {{"files", files}, {"position", 96000}});
	REQUIRE(result.at("tracks").size() == files.size());
	REQUIRE(result.at("events").size() == files.size());
	REQUIRE(result.at("media").size() == files.size());
	CHECK(result.at("job").is_null());

	const Json project = run(client, "project.get");
	REQUIRE(project.at("tracks").size() == files.size());
	for (size_t i = 0; i < files.size(); ++i)
	{
		INFO(files[i]);
		const Json media = byId(project.at("media"), result.at("media")[i]);
		CHECK(media.at("path") == files[i]); // without copy, the file stays where it is
		const Json track = byId(project.at("tracks"), result.at("tracks")[i]);
		CHECK(track.at("type") == "audio");
		CHECK(track.at("name") == fs::u8path(files[i]).stem().u8string());
		const Json channel = byId(project.at("channels"), track.at("channel"));
		CHECK(channel.at("format") == (media.at("channels") == 1 ? "mono" : "stereo"));
		const Json event = byId(project.at("audioEvents"), result.at("events")[i]);
		CHECK(event.at("track") == track.at("id"));
		CHECK(event.at("position") == ticksAt(96000)); // new tracks are musical
		CHECK(event.at("offset") == 0);
		CHECK(event.at("length") == media.at("length"));
	}

	// The whole import is one undo step.
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Import audio files");
	run(client, "history.undo");
	const Json undone = run(client, "project.get");
	CHECK(undone.at("tracks").empty());
	CHECK(undone.at("media").empty());
	CHECK(undone.at("audioEvents").empty());
}

TEST_CASE("audioFile.import lays files out one after another on one track", "[integration]")
{
	const test::TempDir dir;
	writeFile(dir.path() / "a.wav", 4800);
	writeFile(dir.path() / "b.wav", 4410, 2, 44100.0); // 4800 frames of the project's 48 kHz
	writeFile(dir.path() / "c.wav", 100);
	const Json files = {utf8(dir.path() / "a.wav"), utf8(dir.path() / "b.wav"), utf8(dir.path() / "c.wav")};

	Engine engine;
	Client client(engine);

	SECTION("on a new track with oneTrack")
	{
		const Json result =
			run(client, "audioFile.import", {{"files", files}, {"position", 48000}, {"oneTrack", true}});
		REQUIRE(result.at("tracks").size() == 1);
		const Json project = run(client, "project.get");
		REQUIRE(project.at("audioEvents").size() == 3);
		const std::vector<int64_t> starts{48000, 52800, 57600};
		for (size_t i = 0; i < 3; ++i)
		{
			const Json event = byId(project.at("audioEvents"), result.at("events")[i]);
			CHECK(event.at("track") == result.at("tracks")[0]);
			CHECK(event.at("position") == ticksAt(starts[i]));
		}
		CHECK(project.at("tracks")[0].at("name") == "a");
	}

	SECTION("on a track that is there, in its time base")
	{
		const Json track =
			run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"name", "Drums"}}).at("track");
		const Json result = run(client, "audioFile.import", {{"files", files}, {"track", track.at("id")}});
		CHECK(result.at("tracks").empty());
		const Json project = run(client, "project.get");
		REQUIRE(project.at("tracks").size() == 1);
		const std::vector<int64_t> starts{0, 4800, 9600};
		for (size_t i = 0; i < 3; ++i)
		{
			const Json event = byId(project.at("audioEvents"), result.at("events")[i]);
			CHECK(event.at("track") == track.at("id"));
			CHECK(event.at("position") == starts[i]);
		}
	}
}

TEST_CASE("audioFile.import converts files into the media folder at the project's sample rate", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "slow.wav", 4410, 2, 44100.0);
	writeFile(song.library / "fine.wav", 4800, 2, 48000.0);
	const Json files = {utf8(song.library / "slow.wav"), utf8(song.library / "fine.wav"), dataFile("sine440.mp3")};
	const Json result = run(song.client, "audioFile.import", {{"files", files}, {"convert", true}});
	const Json project = run(song.client, "project.get");

	// The 44.1 kHz file and the MP3 become 48 kHz WAV files in the media folder; the 48 kHz WAV file stays.
	const Json slow = byId(project.at("media"), result.at("media")[0]);
	CHECK(slow.at("path") == utf8(song.media / "slow.wav"));
	CHECK(slow.at("sampleRate") == 48000.0);
	CHECK(slow.at("length") == 4800);
	const AudioFileInfo info = AudioFileReader::open(utf8(song.media / "slow.wav"))->info();
	CHECK(info.format == AudioFormat::Wav);
	CHECK(info.sampleRate == 48000.0);
	CHECK(info.channels == 2);
	CHECK(byId(project.at("media"), result.at("media")[1]).at("path") == files[1]);
	const Json mp3 = byId(project.at("media"), result.at("media")[2]);
	CHECK(mp3.at("path") == utf8(song.media / "sine440.wav"));
	CHECK(AudioFileReader::open(utf8(song.media / "sine440.wav"))->info().format == AudioFormat::Wav);
	// The clips keep the names of the files imported.
	CHECK(byId(project.at("clips"), byId(project.at("audioEvents"), result.at("events")[2]).at("clip")).at("name") ==
		"sine440");
}

TEST_CASE("audioFile.import splits channels into mono files on tracks of their own", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "room.wav", 1000, 2);
	writeFile(song.library / "voice.wav", 1000, 1);
	writeFile(song.library / "surround.wav", 1000, 6);
	const Json files = {
		utf8(song.library / "room.wav"), utf8(song.library / "voice.wav"), utf8(song.library / "surround.wav")};
	const Json result = run(song.client, "audioFile.import", {{"files", files}, {"splitChannels", true}});
	REQUIRE(result.at("tracks").size() == 9);
	const Json project = run(song.client, "project.get");

	const std::vector<std::string> names{"room-L", "room-R", "voice", "surround-L", "surround-R", "surround-C",
		"surround-LFE", "surround-Ls", "surround-Rs"};
	for (size_t i = 0; i < names.size(); ++i)
	{
		INFO(names[i]);
		const Json track = byId(project.at("tracks"), result.at("tracks")[i]);
		CHECK(track.at("name") == names[i]);
		CHECK(byId(project.at("channels"), track.at("channel")).at("format") == "mono");
		const Json media = byId(project.at("media"), result.at("media")[i]);
		CHECK(media.at("channels") == 1);
		CHECK(media.at("length") == 1000);
		if (names[i] != "voice") // a mono file stays as it is
			CHECK(media.at("path") == utf8(song.media / (names[i] + ".wav")));
		const Json event = byId(project.at("audioEvents"), result.at("events")[i]);
		CHECK(event.at("track") == track.at("id"));
		CHECK(event.at("position") == 0);
	}

	// Each file has one channel of the original.
	const std::vector<float> stereo = rampSamples(2, 1000);
	const std::vector<float> right = readAll(utf8(song.media / "room-R.wav"));
	REQUIRE(right.size() == 1000);
	for (size_t frame = 0; frame < 1000; ++frame)
		CHECK(right[frame] == stereo[(frame * 2) + 1]);

	CHECK_THAT(reject(song.client, "audioFile.import", {{"files", files}, {"splitChannels", true}, {"oneTrack", true}}),
		ContainsSubstring("splitChannels"));
}

TEST_CASE("audioFile.import copies files into the media folder with one job", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "a.wav", 480);
	writeFile(song.library / "b.wav", 480);
	writeFile(song.media / "inside.wav", 480); // already in the project folder
	const Json files = {utf8(song.library / "a.wav"), utf8(song.library / "b.wav"), utf8(song.media / "inside.wav")};
	const Json result = run(song.client, "audioFile.import", {{"files", files}, {"copy", true}});
	REQUIRE(result.at("job").is_object());
	CHECK(result.at("job").at("kind") == "audioFile.import");
	const Json job = waitForJob(song.client, result.at("job").at("id").get<uint64_t>());
	REQUIRE(job.at("state") == "done");
	CHECK(job.at("result").at("copied").size() == 2);

	const Json project = run(song.client, "project.get");
	CHECK(byId(project.at("media"), result.at("media")[0]).at("path") == utf8(song.media / "a.wav"));
	CHECK(byId(project.at("media"), result.at("media")[1]).at("path") == utf8(song.media / "b.wav"));
	CHECK(byId(project.at("media"), result.at("media")[2]).at("path") == utf8(song.media / "inside.wav"));
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Copy imported files into the project");
}

TEST_CASE("audioFile.import rejects what it can't do and then changes nothing", "[integration]")
{
	const test::TempDir dir;
	writeFile(dir.path() / "slow.wav", 441, 2, 44100.0);
	const std::string slow = utf8(dir.path() / "slow.wav");
	Engine engine;
	Client client(engine);

	CHECK_THAT(reject(client, "audioFile.import", {{"files", Json::array()}}), ContainsSubstring("files"));
	CHECK_THAT(reject(client, "audioFile.import", {{"files", {utf8(dir.path() / "gone.wav")}}}),
		ContainsSubstring("gone.wav"));
	for (const char* option : {"copy", "convert", "splitChannels"})
	{
		INFO(option);
		CHECK_THAT(reject(client, "audioFile.import", {{"files", {slow}}, {option, true}}), ContainsSubstring("save"));
	}
	const Json midi = run(client, "track.add", {{"type", "midi"}}).at("track");
	CHECK_THAT(reject(client, "audioFile.import", {{"files", {slow}}, {"track", midi.at("id")}}),
		ContainsSubstring("audio track"));
	CHECK_THAT(
		reject(client, "audioFile.import", {{"files", {slow}}, {"position", -1}}), ContainsSubstring("position"));

	// A file that fails after others were converted: nothing is imported and the converted files are removed.
	const fs::path folder = dir.path() / "project";
	fs::create_directories(folder);
	run(client, "project.save", {{"path", utf8(folder / "Song.dawproject")}, {"embedMedia", false}});
	const Json before = run(client, "project.get");
	CHECK_THAT(
		reject(client, "audioFile.import", {{"files", {slow, utf8(dir.path() / "gone.wav")}}, {"convert", true}}),
		ContainsSubstring("gone.wav"));
	CHECK(run(client, "project.get") == before);
	CHECK(!fs::exists(folder / "Audio" / "slow.wav"));
}
