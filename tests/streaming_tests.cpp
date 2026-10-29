// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/diskstream.h"
#include "engine/audiofile/resampler.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase::engine;
namespace test = gnubase::test;
using namespace std::chrono_literals;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;

	// A made-up file whose every sample is known: a ramp per channel, the second channel inverted. Reading can be
	// slowed down to simulate a slow disk.
	class SignalReader final : public AudioFileReader
	{
	public:
		SignalReader(int channels, double sampleRate, gnubase::time::Samples frames,
			std::chrono::microseconds delayPerRead = 0us)
			: m_delay(delayPerRead)
		{
			m_info.channels = channels;
			m_info.sampleRate = sampleRate;
			m_info.frames = frames;
		}

		static float value(gnubase::time::Samples frame, int channel)
		{
			const float ramp = static_cast<float>((frame % 4096) - 2048) / 4096.0f;
			return channel == 0 ? ramp : -ramp;
		}

		size_t read(float* buffer, size_t frames) override
		{
			if (m_delay.count() > 0)
				std::this_thread::sleep_for(m_delay);
			const auto count = static_cast<size_t>(std::min<gnubase::time::Samples>(
				static_cast<gnubase::time::Samples>(frames), m_info.frames - m_position));
			for (size_t i = 0; i < count; ++i)
				for (int channel = 0; channel < m_info.channels; ++channel)
					buffer[(i * static_cast<size_t>(m_info.channels)) + static_cast<size_t>(channel)] =
						value(m_position + static_cast<gnubase::time::Samples>(i), channel);
			m_position += static_cast<gnubase::time::Samples>(count);
			return count;
		}

	protected:
		bool seekTo(gnubase::time::Samples frame) override
		{
			m_position = frame;
			return true;
		}

	private:
		std::chrono::microseconds m_delay;
		gnubase::time::Samples m_position = 0;
	};

	// Waits until the disk thread has filled the stream's buffer, as the transport will before playing.
	bool waitUntilReady(const DiskStream& stream)
	{
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		while (!stream.ready())
		{
			if (std::chrono::steady_clock::now() > deadline)
				return false;
			std::this_thread::sleep_for(1ms);
		}
		return true;
	}

	// Reads the whole stream the way an audio thread does: blockFrames at a time, at the pace of real time if
	// realTime is set.
	std::vector<float> play(DiskStream& stream, gnubase::time::Samples frames, size_t blockFrames, bool realTime)
	{
		const auto channels = static_cast<size_t>(stream.channels());
		std::vector<float> output(static_cast<size_t>(frames) * channels);
		const auto period = std::chrono::duration<double>(static_cast<double>(blockFrames) / stream.sampleRate());
		const auto start = std::chrono::steady_clock::now();
		size_t done = 0;
		for (size_t block = 0; done < static_cast<size_t>(frames); ++block)
		{
			if (realTime)
				std::this_thread::sleep_until(
					start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(period * block));
			const size_t count = std::min(blockFrames, static_cast<size_t>(frames) - done);
			stream.read(output.data() + (done * channels), count);
			done += count;
		}
		return output;
	}

	// How many frames of output differ from the made-up file's samples.
	size_t mismatches(const std::vector<float>& output, int channels, gnubase::time::Samples first)
	{
		size_t wrong = 0;
		const auto count = output.size() / static_cast<size_t>(channels);
		for (size_t frame = 0; frame < count; ++frame)
			for (int channel = 0; channel < channels; ++channel)
				if (output[(frame * static_cast<size_t>(channels)) + static_cast<size_t>(channel)] !=
					SignalReader::value(first + static_cast<gnubase::time::Samples>(frame), channel))
				{
					++wrong;
					break;
				}
		return wrong;
	}

	std::vector<float> sine(double frequency, double sampleRate, size_t frames, double amplitude = 0.5)
	{
		std::vector<float> samples(frames);
		for (size_t i = 0; i < frames; ++i)
			samples[i] =
				static_cast<float>(amplitude * std::sin(2.0 * g_pi * frequency * static_cast<double>(i) / sampleRate));
		return samples;
	}

	std::vector<float> resampleAll(const std::vector<float>& input, double from, double to, int channels, size_t piece)
	{
		Resampler resampler(from, to, channels, piece);
		std::vector<float> output;
		const auto frames = input.size() / static_cast<size_t>(channels);
		for (size_t done = 0; done < frames; done += piece)
			resampler.process(
				input.data() + (done * static_cast<size_t>(channels)), std::min(piece, frames - done), output);
		resampler.finish(output);
		return output;
	}

	double rms(const std::vector<float>& samples, size_t first, size_t last)
	{
		double sum = 0.0;
		for (size_t i = first; i < last; ++i)
			sum += static_cast<double>(samples[i]) * samples[i];
		return std::sqrt(sum / static_cast<double>(last - first));
	}

	double frequency(const std::vector<float>& samples, double sampleRate, size_t first, size_t last)
	{
		int crossings = 0;
		for (size_t i = first + 1; i < last; ++i)
			if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f))
				++crossings;
		return crossings / 2.0 / (static_cast<double>(last - first) / sampleRate);
	}
}

TEST_CASE("resampling keeps pitch, level and timing", "[unit]")
{
	for (const double to : {48000.0, 96000.0, 22050.0})
	{
		INFO("44100 to " << to);
		std::vector<float> input = sine(1000.0, 44100.0, 44100);
		input[22050] = 1.0f; // a click at half a second
		const std::vector<float> output = resampleAll(input, 44100.0, to, 1, 1000);
		CHECK(output.size() == static_cast<size_t>(std::lround(to)));

		const auto at = [to](double seconds) { return static_cast<size_t>(seconds * to); };
		CHECK(std::abs(frequency(output, to, at(0.1), at(0.4)) - 1000.0) < 5.0);
		CHECK(std::abs(rms(output, at(0.1), at(0.4)) - (0.5 / std::sqrt(2.0))) < 0.002);
		// The click stands out from the sine where it was, half a second in.
		size_t peak = at(0.45);
		for (size_t i = at(0.45); i < at(0.55); ++i)
		{
			const double expected = 0.5 * std::sin(2.0 * g_pi * 1000.0 * static_cast<double>(i) / to);
			const double previous = 0.5 * std::sin(2.0 * g_pi * 1000.0 * static_cast<double>(peak) / to);
			if (std::abs(output[i] - expected) > std::abs(output[peak] - previous))
				peak = i;
		}
		CHECK(std::abs(static_cast<double>(peak) - (0.5 * to)) <= 1.0);
	}
}

TEST_CASE("resampling removes what lies above the new sample rate's limit", "[unit]")
{
	// 30 kHz can exist at 96 kHz, but not at 44.1 kHz, whose limit is 22.05 kHz.
	const std::vector<float> output = resampleAll(sine(30000.0, 96000.0, 96000), 96000.0, 44100.0, 1, 4096);
	CHECK(rms(output, 4410, 39690) < 1e-4); // more than 80 dB down
}

TEST_CASE("resampling in pieces gives what resampling at once gives", "[unit]")
{
	std::vector<float> stereo(size_t{2} * 20000);
	for (size_t i = 0; i < stereo.size(); ++i)
		stereo[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.013) * 0.7);
	const std::vector<float> whole = resampleAll(stereo, 48000.0, 44100.0, 2, 20000);
	const std::vector<float> pieces = resampleAll(stereo, 48000.0, 44100.0, 2, 77);
	REQUIRE(whole.size() == pieces.size());
	float worst = 0.0f;
	for (size_t i = 0; i < whole.size(); ++i)
		worst = std::max(worst, std::abs(whole[i] - pieces[i]));
	CHECK(worst < 1e-6f);
	CHECK(resampleAll(stereo, 48000.0, 48000.0, 2, 1000) == stereo); // equal rates pass through
}

TEST_CASE("a stream delivers its file to the audio thread, then silence", "[integration]")
{
	DiskStreamer streamer;
	const auto stream = streamer.open(std::make_unique<SignalReader>(2, 48000.0, 20000), 48000.0);
	CHECK(stream->channels() == 2);
	CHECK(stream->frames() == 20000);
	REQUIRE(waitUntilReady(*stream));
	const std::vector<float> output = play(*stream, 20000, 16, false);
	CHECK(mismatches(output, 2, 0) == 0);

	std::vector<float> after(64, 1.0f);
	CHECK(stream->read(after.data(), 32) == 0); // the end: silence, and no dropouts
	CHECK(std::all_of(after.begin(), after.end(), [](float value) { return value == 0.0f; }));
	CHECK(stream->dropouts() == 0);
}

TEST_CASE("tiny buffers and a slow disk cause no dropouts", "[integration][slow]")
{
	// The disk takes 2 ms for each block of 1024 frames, about 21 ms of audio; the audio thread asks for 16 frames
	// at a time, in real time, for a second.
	DiskStreamer streamer;
	DiskStreamSettings settings;
	settings.blockFrames = 1024;
	settings.blocks = 16;
	const auto stream = streamer.open(std::make_unique<SignalReader>(2, 48000.0, 48000, 2000us), 48000.0, settings);
	REQUIRE(waitUntilReady(*stream));
	const std::vector<float> output = play(*stream, 48000, 16, true);
	CHECK(stream->dropouts() == 0);
	CHECK(mismatches(output, 2, 0) == 0);
}

TEST_CASE("a disk too slow for real time shows as dropouts, and the stream keeps its timing", "[integration][slow]")
{
	// 30 ms for 256 frames, about 5 ms of audio: the disk can't keep up.
	DiskStreamer streamer;
	DiskStreamSettings settings;
	settings.blockFrames = 256;
	settings.blocks = 4;
	const auto stream = streamer.open(std::make_unique<SignalReader>(1, 48000.0, 24000, 30000us), 48000.0, settings);
	REQUIRE(waitUntilReady(*stream));
	const std::vector<float> output = play(*stream, 24000, 64, true);
	CHECK(stream->dropouts() > 0);
	// Frames that arrived are where they belong; the others are silence.
	size_t wrong = 0;
	for (size_t frame = 0; frame < output.size(); ++frame)
		if (output[frame] != 0.0f && output[frame] != SignalReader::value(static_cast<int64_t>(frame), 0))
			++wrong;
	CHECK(wrong == 0);
}

TEST_CASE("after seeking, the stream continues at the new position", "[integration]")
{
	DiskStreamer streamer;
	DiskStreamSettings settings; // a buffer much smaller than the file, as with real files
	settings.blockFrames = 1024;
	settings.blocks = 4;
	const auto stream = streamer.open(std::make_unique<SignalReader>(2, 44100.0, 100000), 44100.0, settings);
	REQUIRE(waitUntilReady(*stream));
	play(*stream, 5000, 128, true); // in real time: faster than that, a small buffer runs dry
	stream->seek(70000);
	REQUIRE(waitUntilReady(*stream));
	const std::vector<float> output = play(*stream, 20000, 128, true);
	CHECK(mismatches(output, 2, 70000) == 0);
	CHECK(stream->dropouts() == 0);
}

TEST_CASE("seeking during playback never plays audio from before the seek", "[integration]")
{
	// The audio thread keeps reading while the stream seeks, as it does when the transport locates.
	DiskStreamer streamer;
	DiskStreamSettings settings;
	settings.blockFrames = 1024;
	settings.blocks = 4;
	const auto stream = streamer.open(std::make_unique<SignalReader>(1, 48000.0, 100000), 48000.0, settings);
	REQUIRE(waitUntilReady(*stream));
	play(*stream, 2000, 64, false);
	stream->seek(60000);
	const std::vector<float> output = play(*stream, 20000, 64, true);

	// Silence while the stream finds the new position, then the file from there on, without a gap.
	const auto first = static_cast<size_t>(
		std::find_if(output.begin(), output.end(), [](float value) { return value != 0.0f; }) - output.begin());
	REQUIRE(first < 10000);
	size_t wrong = 0;
	for (size_t frame = first; frame < output.size(); ++frame)
		if (output[frame] != SignalReader::value(60000 + static_cast<int64_t>(frame - first), 0))
			++wrong;
	CHECK(wrong == 0);
}

TEST_CASE("streams play files at the output sample rate, and seeking stays exact", "[integration]")
{
	const auto source = [] { return std::make_unique<SignalReader>(2, 44100.0, 44100); };
	std::vector<float> file(size_t{2} * 44100);
	source()->read(file.data(), 44100);
	const std::vector<float> expected = resampleAll(file, 44100.0, 48000.0, 2, 4096);

	DiskStreamer streamer;
	const auto stream = streamer.open(source(), 48000.0);
	CHECK(stream->frames() == 48000);
	REQUIRE(waitUntilReady(*stream));
	const std::vector<float> output = play(*stream, 48000, 64, false);
	REQUIRE(output.size() == expected.size());
	float worst = 0.0f;
	for (size_t i = 0; i < output.size(); ++i)
		worst = std::max(worst, std::abs(output[i] - expected[i]));
	CHECK(worst < 1e-5f);

	for (const int64_t position : {int64_t{30001}, int64_t{17}, int64_t{47000}})
	{
		INFO("seek to " << position);
		stream->seek(position);
		REQUIRE(waitUntilReady(*stream));
		const std::vector<float> part = play(*stream, 500, 64, false);
		worst = 0.0f;
		for (size_t i = 0; i < part.size(); ++i)
			worst = std::max(worst, std::abs(part[i] - expected[(static_cast<size_t>(position) * 2) + i]));
		CHECK(worst < 1e-4f);
	}
	CHECK(stream->dropouts() == 0);
}

TEST_CASE("media.convert writes a copy at the project's sample rate", "[integration]")
{
	const test::TempDir dir;
	const std::string input = (dir.path() / "take.wav").u8string();
	const std::string output = (dir.path() / "take 48k.wav").u8string();
	{
		WriteOptions options;
		options.sampleRate = 44100.0;
		options.channels = 1;
		const auto writer = AudioFileWriter::create(input, options);
		const std::vector<float> samples = sine(440.0, 44100.0, 44100);
		writer->write(samples.data(), samples.size());
		writer->close();
	}
	Engine engine;
	Client client(engine);
	const Result added = client.call("media.add", {{"path", input}});
	REQUIRE(added.ok());
	const Result converted =
		client.call("media.convert", {{"media", added.value.at("media").at("id")}, {"path", output}});
	INFO(converted.error);
	REQUIRE(converted.ok());
	const Json& media = converted.value.at("media");
	CHECK(media.at("sampleRate").get<double>() == 48000.0);
	CHECK(media.at("length") == 48000);
	CHECK(media.at("path") == output);
	const auto reader = AudioFileReader::open(output);
	CHECK(reader->info().sampleRate == 48000.0);
	CHECK(reader->info().sampleFormat == SampleFormat::Float32);

	CHECK(!client.call("media.convert", {{"media", 999}, {"path", output}}).ok());
	// Never over a file of the project, such as the source: undo couldn't bring it back.
	CHECK(!client.call("media.convert", {{"media", added.value.at("media").at("id")}, {"path", input}}).ok());
	CHECK(AudioFileReader::open(input)->info().frames == 44100);
}
