// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
namespace test = gnubase::test;
using Catch::Matchers::ContainsSubstring;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;

	std::string utf8(const std::filesystem::path& path)
	{
		return path.u8string();
	}

	std::string dataFile(const char* name)
	{
		return utf8(std::filesystem::path(GNUBASE_TEST_DATA_DIR) / name);
	}

	// Sines at a different pitch per channel, starting with full scale and silence to test the extremes.
	test::Audio testSignal(int channels, int sampleRate, size_t frames)
	{
		test::Audio audio;
		audio.channels = channels;
		audio.sampleRate = sampleRate;
		audio.samples.resize(frames * static_cast<size_t>(channels));
		for (size_t frame = 0; frame < frames; ++frame)
			for (int channel = 0; channel < channels; ++channel)
			{
				const double pitch = 220.0 * (channel + 1);
				auto value =
					static_cast<float>(0.8 * std::sin(2.0 * g_pi * pitch * static_cast<double>(frame) / sampleRate));
				if (frame < 3)
					value = std::vector<float>{1.0f, -1.0f, 0.0f}[frame];
				audio.samples[(frame * static_cast<size_t>(channels)) + static_cast<size_t>(channel)] = value;
			}
		return audio;
	}

	// Writes audio in three pieces, to test that writes add up.
	void writeFile(const std::string& path, const test::Audio& audio, WriteOptions options)
	{
		options.sampleRate = audio.sampleRate;
		options.channels = audio.channels;
		const auto writer = AudioFileWriter::create(path, options);
		const size_t frames = audio.frames();
		const size_t first = frames / 3;
		const auto channels = static_cast<size_t>(audio.channels);
		writer->write(audio.samples.data(), first);
		writer->write(audio.samples.data() + (first * channels), first);
		writer->write(audio.samples.data() + (2 * first * channels), frames - (2 * first));
		writer->close();
	}

	test::Audio readAll(AudioFileReader& reader)
	{
		test::Audio audio;
		audio.channels = reader.info().channels;
		audio.sampleRate = static_cast<int>(reader.info().sampleRate);
		std::vector<float> buffer(1000 * static_cast<size_t>(audio.channels));
		while (const size_t frames = reader.read(buffer.data(), 1000))
			audio.samples.insert(audio.samples.end(), buffer.begin(),
				buffer.begin() + static_cast<std::ptrdiff_t>(frames * static_cast<size_t>(audio.channels)));
		return audio;
	}

	test::Audio readAll(const std::string& path)
	{
		const auto reader = AudioFileReader::open(path);
		return readAll(*reader);
	}

	std::string head(const std::string& path, size_t bytes)
	{
		std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
		std::string text(bytes, '\0');
		file.read(text.data(), static_cast<std::streamsize>(bytes));
		return text;
	}

	struct Case
	{
		AudioFormat format;
		SampleFormat sampleFormat;
		int channels;
		int sampleRate;
		float tolerance; // one step of the format, plus float rounding
	};
}

TEST_CASE("every writable format reads back through an independent decoder", "[integration]")
{
	const Case cases[] = {
		{AudioFormat::Wav, SampleFormat::Int16, 2, 48000, 1.0f / 32768.0f},
		{AudioFormat::Wav, SampleFormat::Int24, 3, 96000, 1.0f / 8388608.0f},
		{AudioFormat::Wav, SampleFormat::Int32, 1, 44100, 1e-6f},
		{AudioFormat::Wav, SampleFormat::Float32, 6, 48000, 0.0f},
		{AudioFormat::Aiff, SampleFormat::Int16, 2, 44100, 1.0f / 32768.0f},
		{AudioFormat::Aiff, SampleFormat::Int24, 1, 96000, 1.0f / 8388608.0f},
		{AudioFormat::Aiff, SampleFormat::Int32, 2, 88200, 1e-6f},
		{AudioFormat::Flac, SampleFormat::Int16, 2, 44100, 1.0f / 32768.0f},
		{AudioFormat::Flac, SampleFormat::Int24, 2, 192000, 1.0f / 8388608.0f},
	};
	const test::TempDir dir;
	int index = 0;
	for (const Case& c : cases)
	{
		INFO("case " << index);
		const std::string path = utf8(dir.path() / ("case" + std::to_string(index++)));
		// An odd number of frames, so some cases need a pad byte after their data.
		const test::Audio audio = testSignal(c.channels, c.sampleRate, 5001);
		WriteOptions options;
		options.format = c.format;
		options.sampleFormat = c.sampleFormat;
		writeFile(path, audio, options);

		const auto reader = AudioFileReader::open(path);
		const AudioFileInfo& info = reader->info();
		CHECK(info.format == c.format);
		CHECK(info.sampleFormat == c.sampleFormat);
		CHECK(info.sampleRate == c.sampleRate);
		CHECK(info.channels == c.channels);
		CHECK(info.frames == 5001);
		const test::AudioDiff diff = test::compareAudio(readAll(*reader), audio, c.tolerance * 1.01f);
		INFO(diff.report);
		CHECK(diff.equal);
	}
}

TEST_CASE("WAV files written by other programs read back exactly", "[integration]")
{
	const test::TempDir dir;
	const test::Audio audio = testSignal(2, 48000, 3000);
	const std::string path = utf8(dir.path() / "other.wav");
	test::writeWav(path, audio);
	CHECK(test::compareAudio(readAll(path), audio, 0.0f).equal);
}

TEST_CASE("BWF files keep their description chunk", "[integration]")
{
	const test::TempDir dir;
	const std::string path = utf8(dir.path() / "take.wav");
	BroadcastInfo broadcast;
	broadcast.description = "Take 3, vocals";
	broadcast.originator = "GNUbase";
	broadcast.originatorReference = "GNU-0001";
	broadcast.originationDate = "2026-10-27";
	broadcast.originationTime = "21:05:00";
	broadcast.timeReference = 3600ULL * 48000;
	WriteOptions options;
	options.broadcast = broadcast;
	writeFile(path, testSignal(2, 48000, 1000), options);

	const auto reader = AudioFileReader::open(path);
	REQUIRE(reader->info().broadcast.has_value());
	const BroadcastInfo read = reader->info().broadcast.value_or(BroadcastInfo{});
	CHECK(read.description == broadcast.description);
	CHECK(read.originator == broadcast.originator);
	CHECK(read.originatorReference == broadcast.originatorReference);
	CHECK(read.originationDate == broadcast.originationDate);
	CHECK(read.originationTime == broadcast.originationTime);
	CHECK(read.timeReference == broadcast.timeReference);
	CHECK(reader->info().frames == 1000);
}

TEST_CASE("WAV files become RF64 when asked, and reserve room for it otherwise", "[integration]")
{
	const test::TempDir dir;
	const test::Audio audio = testSignal(2, 48000, 2000);
	WriteOptions options;
	options.sampleFormat = SampleFormat::Float32;

	const std::string always = utf8(dir.path() / "always.wav");
	options.rf64 = Rf64::Always;
	writeFile(always, audio, options);
	CHECK(head(always, 4) == "RF64");
	CHECK(head(always, 16).substr(12, 4) == "ds64");
	CHECK(test::compareAudio(readAll(always), audio, 0.0f).equal);

	const std::string automatic = utf8(dir.path() / "auto.wav");
	options.rf64 = Rf64::Auto;
	writeFile(automatic, audio, options);
	CHECK(head(automatic, 4) == "RIFF"); // small files stay plain WAV
	CHECK(head(automatic, 16).substr(12, 4) == "JUNK");
	CHECK(test::compareAudio(readAll(automatic), audio, 0.0f).equal);
}

TEST_CASE("Ogg Vorbis and MP3 files decode to their signal", "[integration]")
{
	// Half a second of a 440 Hz sine at half scale on the left channel and silence on the right, at 44.1 kHz.
	for (const auto& [name, format] :
		{std::pair{"sine440.ogg", AudioFormat::OggVorbis}, {"sine440.mp3", AudioFormat::Mp3}})
	{
		INFO(name);
		const auto reader = AudioFileReader::open(dataFile(name));
		CHECK(reader->info().format == format);
		CHECK(reader->info().sampleRate == 44100.0);
		REQUIRE(reader->info().channels == 2);
		CHECK(std::abs(reader->info().frames - 22050) < 2400); // MP3 may add encoder padding
		const test::Audio audio = readAll(*reader);
		CHECK(static_cast<int64_t>(audio.frames()) == reader->info().frames);

		// The middle 0.3 seconds: level and pitch of the left channel, silence on the right.
		double sum = 0.0;
		double right = 0.0;
		int crossings = 0;
		const size_t start = 4410;
		const size_t end = start + 13230;
		for (size_t frame = start; frame < end; ++frame)
		{
			const float left = audio.samples[frame * 2];
			sum += static_cast<double>(left) * left;
			right = std::max(right, static_cast<double>(std::abs(audio.samples[(frame * 2) + 1])));
			if ((audio.samples[(frame - 1) * 2] < 0.0f) != (left < 0.0f))
				++crossings;
		}
		const double rms = std::sqrt(sum / static_cast<double>(end - start));
		CHECK(std::abs(rms - (0.5 / std::sqrt(2.0))) < 0.02);
		CHECK(right < 0.01);
		CHECK(std::abs((crossings / 2.0 / 0.3) - 440.0) < 5.0);
	}
}

TEST_CASE("reading continues where seek says, in every format", "[integration]")
{
	const test::TempDir dir;
	const test::Audio audio = testSignal(2, 48000, 6000);
	std::vector<std::string> paths{dataFile("sine440.ogg"), dataFile("sine440.mp3")};
	for (const AudioFormat format : {AudioFormat::Wav, AudioFormat::Aiff, AudioFormat::Flac})
	{
		WriteOptions options;
		options.format = format;
		options.sampleFormat = SampleFormat::Int24;
		paths.push_back(utf8(dir.path() / ("seek" + std::to_string(paths.size()))));
		writeFile(paths.back(), audio, options);
	}
	for (const std::string& path : paths)
	{
		INFO(path);
		const test::Audio whole = readAll(path);
		const auto reader = AudioFileReader::open(path);
		for (const size_t frame : {size_t{4321}, size_t{17}, size_t{0}})
		{
			reader->seek(static_cast<int64_t>(frame));
			std::vector<float> buffer(20);
			REQUIRE(reader->read(buffer.data(), 10) == 10);
			for (size_t i = 0; i < buffer.size(); ++i)
				CHECK(std::abs(buffer[i] - whole.samples[(frame * 2) + i]) < 1e-4f);
		}
		CHECK_THROWS_AS(reader->seek(reader->info().frames + 1), AudioFileError);
	}
}

TEST_CASE("the format is recognized from the content, not the file name", "[integration]")
{
	const test::TempDir dir;
	const std::string path = utf8(dir.path() / "actually-flac.wav");
	WriteOptions options;
	options.format = AudioFormat::Flac;
	options.sampleFormat = SampleFormat::Int16;
	writeFile(path, testSignal(1, 44100, 100), options);
	CHECK(AudioFileReader::open(path)->info().format == AudioFormat::Flac);
}

TEST_CASE("paths with non-ASCII characters work", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path folder = dir.path() / std::filesystem::u8path(u8"Prüfung 日本");
	std::filesystem::create_directories(folder);
	const std::string path = utf8(folder / std::filesystem::u8path(u8"Stimme ä.wav"));
	const test::Audio audio = testSignal(2, 48000, 500);
	writeFile(path, audio, {});
	CHECK(test::compareAudio(readAll(path), audio, 1.01f / 8388608.0f).equal);
}

TEST_CASE("files that can't be read say why", "[integration]")
{
	const test::TempDir dir;
	const std::string missing = utf8(dir.path() / "missing.wav");
	CHECK_THROWS_WITH(AudioFileReader::open(missing), ContainsSubstring("missing.wav"));

	const std::string text = utf8(dir.path() / "notes.wav");
	std::ofstream(std::filesystem::u8path(text)) << "This is not audio, but it is long enough to be checked.";
	CHECK_THROWS_WITH(AudioFileReader::open(text), ContainsSubstring("not a supported audio file"));

	const std::string truncated = utf8(dir.path() / "truncated.wav");
	std::ofstream(std::filesystem::u8path(truncated), std::ios::binary) << "RIFF....WAVEfmt ";
	CHECK_THROWS_AS(AudioFileReader::open(truncated), AudioFileError);
}

TEST_CASE("formats and sample formats that can't be written are rejected", "[integration]")
{
	const test::TempDir dir;
	const std::string path = utf8(dir.path() / "out");
	const auto reject = [&path](AudioFormat format, SampleFormat sampleFormat)
	{
		WriteOptions options;
		options.format = format;
		options.sampleFormat = sampleFormat;
		CHECK_THROWS_AS(AudioFileWriter::create(path, options), AudioFileError);
	};
	reject(AudioFormat::Aiff, SampleFormat::Float32);
	reject(AudioFormat::Flac, SampleFormat::Float32);
	reject(AudioFormat::Flac, SampleFormat::Int32);
	reject(AudioFormat::OggVorbis, SampleFormat::Int16);
	reject(AudioFormat::Mp3, SampleFormat::Int16);
	WriteOptions noChannels;
	noChannels.channels = 0;
	CHECK_THROWS_AS(AudioFileWriter::create(path, noChannels), AudioFileError);
}

TEST_CASE("media.add reads an audio file's sample rate, channels and length", "[integration]")
{
	const test::TempDir dir;
	const std::string path = utf8(dir.path() / "loop.wav");
	writeFile(path, testSignal(2, 44100, 4410), {});

	Engine engine;
	Client client(engine);
	const Result result = client.call("media.add", {{"path", path}});
	INFO(result.error);
	REQUIRE(result.ok());
	const Json& media = result.value.at("media");
	CHECK(media.at("sampleRate").get<double>() == 44100.0);
	CHECK(media.at("channels") == 2);
	CHECK(media.at("length") == 4410);

	const Result missing = client.call("media.add", {{"path", utf8(dir.path() / "gone.wav")}});
	CHECK_THAT(missing.error, ContainsSubstring("gone.wav"));
	const Result partial = client.call("media.add", {{"path", path}, {"sampleRate", 48000.0}});
	CHECK_THAT(partial.error, ContainsSubstring("together"));
}
