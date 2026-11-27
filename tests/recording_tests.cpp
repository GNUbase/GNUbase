// SPDX-License-Identifier: GPL-3.0-or-later
// Audio recording (#96, reference manual pp. 335-346): record-enabled tracks record their input bus into files
// written off the audio thread, cycle laps become takes, the audio record modes, and audio pre-record.
#include "engine/audio/recorder.h"
#include "engine/audio/sequencer.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/heldjob.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using gnubase::engine::AudioFileReader;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Event;
using gnubase::engine::Json;
using gnubase::engine::Message;
using gnubase::engine::ObjectId;
using gnubase::engine::Result;

namespace
{
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

	ObjectId idOf(const Json& result, const char* kind)
	{
		return result.at(kind).at("id").get<ObjectId>();
	}

	// Waits for the event name, such as recording.added, which the engine announces once a recording is in the
	// project; null after 10 s. failures gets the reasons of the recording.failed events before it.
	Json waitFor(Client& client, const std::string& name, std::vector<std::string>* failures = nullptr)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> message = client.wait(std::chrono::milliseconds(50));
			const auto* event = message ? std::get_if<Event>(&*message) : nullptr;
			if (event != nullptr && event->name == name)
				return event->data;
			if (event != nullptr && event->name == "recording.failed" && failures != nullptr)
				failures->push_back(event->data.at("reason").get<std::string>());
		}
		return nullptr;
	}

	Json waitForRecording(Client& client)
	{
		return waitFor(client, "recording.added");
	}

	// Frame f of channel c of a ramp file: (f + 1) / 65536, negated on the second channel; exact in float.
	float ramp(int64_t frame, int channel)
	{
		const float value = static_cast<float>(frame + 1) / 65536.0f;
		return channel == 0 ? value : -value;
	}

	std::string writeRamp(const gnubase::test::TempDir& dir, int64_t frames)
	{
		gnubase::test::Audio audio;
		audio.channels = 2;
		for (int64_t f = 0; f < frames; ++f)
			for (int c = 0; c < 2; ++c)
				audio.samples.push_back(ramp(f, c));
		const std::filesystem::path file = dir.path() / "ramp.wav";
		gnubase::test::writeWav(file, audio);
		return file.u8string();
	}

	// The audio events of a track, sorted by position.
	std::vector<Json> eventsOn(Client& client, ObjectId track)
	{
		std::vector<Json> events;
		const Json project = run(client, "project.get");
		for (const Json& event : project.at("audioEvents"))
			if (event.at("track") == track)
				events.push_back(event);
		std::sort(events.begin(), events.end(),
			[](const Json& a, const Json& b) { return a.at("position") < b.at("position"); });
		return events;
	}

	// The path of the media file that an audio event plays, or empty.
	std::string pathOf(Client& client, const Json& event)
	{
		const Json project = run(client, "project.get");
		for (const Json& clip : project.at("clips"))
			if (clip.at("id") == event.at("clip"))
				for (const Json& media : project.at("media"))
					if (media.at("id") == clip.at("media"))
						return media.at("path").get<std::string>();
		return {};
	}

	// One channel of an audio file.
	std::vector<float> channelOf(const std::string& path, int channel)
	{
		const auto reader = AudioFileReader::open(path);
		const int channels = reader->info().channels;
		std::vector<float> interleaved(static_cast<size_t>(reader->info().frames * channels));
		interleaved.resize(reader->read(interleaved.data(), static_cast<size_t>(reader->info().frames)) *
			static_cast<size_t>(channels));
		std::vector<float> samples;
		for (auto i = static_cast<size_t>(channel); i < interleaved.size(); i += static_cast<size_t>(channels))
			samples.push_back(interleaved[i]);
		return samples;
	}

	std::vector<float> recorded(Client& client, int channel, int64_t start, int64_t frames)
	{
		return run(client, "audio.captureInput", {{"channel", channel}, {"start", start}, {"frames", frames}})
			.at("samples")
			.get<std::vector<float>>();
	}

	std::vector<float> slice(const std::vector<float>& samples, int64_t start, int64_t frames)
	{
		return {samples.begin() + start, samples.begin() + start + frames};
	}

	// A saved project (recordings go into its media folder) with a record-enabled stereo audio track on the main
	// input bus, which doesn't monitor: the loopback would feed it back. It records 32-bit float files, which hold
	// the input exactly.
	struct Setup
	{
		explicit Setup(const Json& device)
		{
			run(client, "audio.setDevice", device);
			run(client, "settings.set", {{"key", "audio.autoMonitoring"}, {"value", "manual"}});
			run(client, "project.set", {{"recordBitDepth", "float32"}});
			run(client, "project.save", {{"path", (dir.path() / "take.dawproject").u8string()}});
			const Json added = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}});
			track = idOf(added, "track");
			run(client, "channel.set", {{"channel", added.at("channel").at("id")}, {"recordEnable", true}});
		}

		// Stops, and renders on until the input that was still on its way has arrived: the recording ends.
		Json stop()
		{
			run(client, "transport.stop");
			run(client, "audio.render", {{"frames", 640}});
			return waitForRecording(client);
		}

		gnubase::test::TempDir dir;
		Engine engine;
		Client client{engine};
		ObjectId track = 0;
	};

	Json sineInput()
	{
		return {{"processor", "sine"}, {"parameters", {{"Frequency", 440.0}, {"Level", 0.5}}}};
	}

	// Hands a Recorder prepared for one channel and g_blockFrames frames block number index of the device's input,
	// recorded at its own position or not recorded.
	constexpr uint32_t g_blockFrames = 64;

	void processBlock(gnubase::engine::Recorder& recorder, gnubase::engine::PlayedPositions& played, uint64_t index,
		bool recording, const gnubase::engine::MidiTap::Sequencers& sequencers = {})
	{
		namespace engine = gnubase::engine;
		std::vector<float> input(g_blockFrames, 0.25f);
		const float* const inputs[] = {input.data()};
		const gnubase::audio::MidiBuffer midiIn;
		gnubase::audio::MidiBuffer midiOut;
		const uint64_t position = index * g_blockFrames;
		const engine::graph::TransportSegment segment{0, g_blockFrames, static_cast<int64_t>(position), recording};
		engine::graph::TransportBlock transport;
		transport.playing = true;
		transport.segments = &segment;
		transport.segmentCount = 1;
		transport.recording = recording;
		played.add(position, g_blockFrames, transport);
		recorder.process({inputs, nullptr, g_blockFrames, position, 0, midiIn, midiOut}, played, 0, sequencers);
	}

	// A setup that records one mono track into folder.
	gnubase::engine::RecordSetup recordInto(const std::filesystem::path& folder)
	{
		gnubase::engine::RecordSetup setup;
		setup.folder = folder;
		setup.format.sampleFormat = gnubase::engine::SampleFormat::Float32;
		setup.tracks = {{1, "Audio", {0}}};
		return setup;
	}
}

TEST_CASE("recording through the dummy device matches its input sample-exactly", "[integration]")
{
	// A ramp leaves the outputs at 1000 and comes back through the loopback 257 frames later.
	Setup setup(
		{{"blockSize", 64}, {"captureSeconds", 1}, {"inputLatency", 100}, {"outputLatency", 157}, {"loopback", true}});
	Client& client = setup.client;
	const Json player = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}});
	run(client, "channel.set", {{"channel", player.at("channel").at("id")}, {"input", 0}});
	const ObjectId media = idOf(run(client, "media.add", {{"path", writeRamp(setup.dir, 500)}}), "media");
	const ObjectId clip = idOf(run(client, "clip.add", {{"media", media}}), "clip");
	run(client, "audioEvent.add", {{"track", idOf(player, "track")}, {"clip", clip}, {"position", 1000}});

	CHECK(run(client, "transport.record").at("recording") == true);
	run(client, "audio.render", {{"frames", 4800}});
	const Json added = setup.stop();
	REQUIRE(added.is_object());

	// One event that plays the take where it was played, from a 32-bit float WAV file in the media folder.
	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("position") == 0);
	CHECK(events[0].at("offset") == 0);
	CHECK(events[0].at("length") == 4800);
	CHECK(added.at("events") == Json::array({events[0].at("id")}));
	const std::string path = pathOf(client, events[0]);
	CHECK(std::filesystem::u8path(path).parent_path() == setup.dir.path() / "Audio");
	const auto reader = AudioFileReader::open(path);
	CHECK(reader->info().sampleFormat == gnubase::engine::SampleFormat::Float32);
	CHECK(reader->info().sampleRate == 48000.0);
	CHECK(reader->info().channels == 2);
	for (int c = 0; c < 2; ++c)
	{
		const std::vector<float> samples = channelOf(path, c);
		REQUIRE(samples.size() == 4800);
		CHECK(samples == recorded(client, c, 0, 4800)); // what the input brought, placed where it was played
		for (int64_t f = 0; f < 4800; ++f)
		{
			const float expected = f >= 1000 && f < 1500 ? ramp(f - 1000, c) : 0.0f;
			if (samples[static_cast<size_t>(f)] != expected)
				FAIL("channel " << c << " frame " << f << ": " << samples[static_cast<size_t>(f)]);
		}
	}

	// One undo step takes the recording out of the project again.
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Record");
	run(client, "history.undo");
	CHECK(eventsOn(client, setup.track).empty());
}

TEST_CASE(
	"a recording after a count-in starts where playback starts, with the input's first frame after it", "[integration]")
{
	// A sine whose phase tells the device's frames apart: 441 Hz is three quarters of a cycle on after 1.75 s.
	const Json input = {{"processor", "sine"}, {"parameters", {{"Frequency", 441.0}, {"Level", 0.5}}}};
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", input}});
	Client& client = setup.client;
	run(client, "metronome.set",
		{{"countIn",
			{{"enabled", true}, {"signature", "custom"},
				{"customSignature", {{"numerator", 7}, {"denominator", 8}}}}}});
	run(client, "transport.locate", {{"position", 1000}});
	CHECK(run(client, "transport.record").at("countingIn") == true);
	constexpr int64_t countIn = 84000; // a bar of 7/8 at 120 BPM: 1.75 s
	run(client, "audio.render", {{"frames", countIn + 4800}});
	setup.stop();

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("position") == 1000);
	CHECK(events[0].at("length") == 4800);
	const std::vector<float> samples = channelOf(pathOf(client, events[0]), 0);
	REQUIRE(samples.size() == 4800);
	for (const int64_t f : {int64_t{0}, int64_t{1}, int64_t{4799}})
	{
		const double cycles = static_cast<double>(countIn + f) * 441.0 / 48000.0;
		const double expected = 0.5 * std::sin(2.0 * 3.14159265358979323846 * (cycles - std::floor(cycles)));
		INFO("frame " << f);
		CHECK(std::abs(samples[static_cast<size_t>(f)] - expected) < 1e-4);
	}
	CHECK(samples[0] < -0.49f); // not the input's first frame, which is 0
}

TEST_CASE("recordings are written in the project setup's record file type and bit depth", "[integration]")
{
	using gnubase::engine::AudioFormat;
	using gnubase::engine::SampleFormat;
	struct Case
	{
		const char* type;
		const char* depth;
		AudioFormat format;
		SampleFormat sampleFormat;
		const char* extension;
		float tolerance; // the quantization step
	};
	const Case cases[] = {
		{"wav", "int16", AudioFormat::Wav, SampleFormat::Int16, ".wav", 1.0f / 32768.0f},
		{"wav", "int32", AudioFormat::Wav, SampleFormat::Int32, ".wav", 1.0f / 8388608.0f},
		{"wav", "float64", AudioFormat::Wav, SampleFormat::Float64, ".wav", 0.0f},
		{"aiff", "int24", AudioFormat::Aiff, SampleFormat::Int24, ".aif", 1.0f / 8388608.0f},
		{"aiff", "int32", AudioFormat::Aiff, SampleFormat::Int32, ".aif", 1.0f / 8388608.0f},
		{"flac", "int16", AudioFormat::Flac, SampleFormat::Int16, ".flac", 1.0f / 32768.0f},
		{"flac", "int24", AudioFormat::Flac, SampleFormat::Int24, ".flac", 1.0f / 8388608.0f},
		{"broadcastWav", "float32", AudioFormat::Wav, SampleFormat::Float32, ".wav", 0.0f},
	};
	for (const Case& each : cases)
	{
		DYNAMIC_SECTION(each.type << " " << each.depth)
		{
			Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
			Client& client = setup.client;
			run(client, "project.set", {{"recordFileType", each.type}, {"recordBitDepth", each.depth}});
			run(client, "transport.record");
			run(client, "audio.render", {{"frames", 2000}});
			REQUIRE(setup.stop().is_object());

			const std::vector<Json> events = eventsOn(client, setup.track);
			REQUIRE(events.size() == 1);
			const std::string path = pathOf(client, events[0]);
			CHECK(std::filesystem::u8path(path).extension() == each.extension);
			const auto reader = AudioFileReader::open(path);
			CHECK(reader->info().format == each.format);
			CHECK(reader->info().sampleFormat == each.sampleFormat);
			CHECK(reader->info().broadcast.has_value() == (std::string(each.type) == "broadcastWav"));
			const std::vector<float> file = channelOf(path, 0);
			const std::vector<float> input = recorded(client, 0, 0, 2000);
			REQUIRE(file.size() == input.size());
			for (size_t f = 0; f < file.size(); ++f)
				if (std::abs(file[f] - input[f]) > each.tolerance)
					FAIL("frame " << f << ": " << file[f] << " instead of " << input[f]);
		}
	}
}

TEST_CASE("audio.waveOver4GB split records Wave files without RF64, which go on in a new file (#2096)", "[integration]")
{
	// Files of at most 10000 bytes instead of 4 GB: 2000 stereo 32-bit float frames need two.
	struct Limit
	{
		Limit() { gnubase::engine::detail::setFileSizeLimit(10000); }
		~Limit() { gnubase::engine::detail::setFileSizeLimit(0); }
		Limit(const Limit&) = delete;
		Limit& operator=(const Limit&) = delete;
		Limit(Limit&&) = delete;
		Limit& operator=(Limit&&) = delete;
	} const limit;
	const std::string choice = GENERATE("rf64", "split");
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "audio.waveOver4GB"}, {"value", choice}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 2000}});
	REQUIRE(setup.stop().is_object());
	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == (choice == "split" ? 2 : 1));
	size_t frames = 0;
	for (const Json& event : events)
		frames += channelOf(pathOf(client, event), 0).size();
	CHECK(frames == 2000);
}

TEST_CASE("Broadcast Wave recordings describe the track and where the file starts", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "track.set", {{"track", setup.track}, {"name", "Lead vocals"}});
	run(client, "project.set",
		{{"recordFileType", "broadcastWav"}, {"recordBitDepth", "int24"}, {"startTime", 3600.0}});
	run(client, "transport.locate", {{"position", 48000}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 1000}});
	REQUIRE(setup.stop().is_object());

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("position") == 48000);
	const auto reader = AudioFileReader::open(pathOf(client, events[0]));
	CHECK(reader->info().sampleFormat == gnubase::engine::SampleFormat::Int24);
	REQUIRE(reader->info().broadcast.has_value());
	const gnubase::engine::BroadcastInfo info = reader->info().broadcast.value_or(gnubase::engine::BroadcastInfo{});
	CHECK(info.description == "Lead vocals");
	CHECK(info.originator == "GNUbase");
	CHECK(info.originationDate.size() == 10);
	CHECK(info.originationTime.size() == 8);
	CHECK(info.timeReference == 3601ULL * 48000); // the project start shows 1:00:00, and the file starts 1 s later
}

TEST_CASE(
	"Broadcast Wave recordings take the description, author and reference of the user settings (#580)", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "track.set", {{"track", setup.track}, {"name", "Lead vocals"}});
	// The author and the reference are cut to their fields' 32 bytes.
	const std::string author(40, 'a');
	run(client, "settings.set",
		{{"values",
			{{"audio.broadcastDescription", "Session one"}, {"audio.broadcastAuthor", author},
				{"audio.broadcastReference", "REF-0042"}}}});
	run(client, "project.set", {{"recordFileType", "broadcastWav"}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(setup.stop().is_object());

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	const auto reader = AudioFileReader::open(pathOf(client, events[0]));
	const gnubase::engine::BroadcastInfo info = reader->info().broadcast.value_or(gnubase::engine::BroadcastInfo{});
	CHECK(info.description == "Session one");
	CHECK(info.originator == author.substr(0, 32));
	CHECK(info.originatorReference == "REF-0042");
}

TEST_CASE("Wave and AIFF recordings carry the description, author and reference as text (#580)", "[integration]")
{
	// The text of the first chunk with this id: a WAV size is little-endian and its text ends with a null character,
	// an AIFF size is big-endian.
	const auto chunkText = [](const std::string& path, const std::string& id, bool bigEndian)
	{
		std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const size_t at = bytes.find(id);
		if (at == std::string::npos)
			return std::string("(none)");
		size_t size = 0;
		for (size_t i = 0; i < 4; ++i)
			size |= size_t{static_cast<unsigned char>(bytes[at + 4 + i])} << (8 * (bigEndian ? 3 - i : i));
		const std::string text = bytes.substr(at + 8, size);
		return text.substr(0, text.find('\0'));
	};
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "track.set", {{"track", setup.track}, {"name", "Lead vocals"}});
	run(client, "settings.set", {{"values", {{"audio.broadcastReference", "REF-0042"}}}});
	const bool aiff = GENERATE(false, true);
	run(client, "project.set", {{"recordFileType", aiff ? "aiff" : "wav"}, {"recordBitDepth", "int24"}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(setup.stop().is_object());

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	const std::string path = pathOf(client, events[0]);
	CHECK(AudioFileReader::open(path)->info().frames > 0);
	CHECK(chunkText(path, aiff ? "NAME" : "ICMT", aiff) == "Lead vocals"); // the description, empty in the settings
	CHECK(chunkText(path, aiff ? "AUTH" : "IART", aiff) == "GNUbase");
	CHECK(chunkText(path, aiff ? "ANNO" : "ISRC", aiff) == "REF-0042");
}

TEST_CASE("a Broadcast Wave recording's time reference includes the pre-recorded input", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "audio.preRecordSeconds"}, {"value", 0.05}}); // 2400 frames
	run(client, "project.set", {{"recordFileType", "broadcastWav"}, {"startTime", 3600.0}});
	run(client, "audio.render", {{"frames", 4800}}); // stopped
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 1000}});
	REQUIRE(setup.stop().is_object());

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("offset") == 2400);
	const auto reader = AudioFileReader::open(pathOf(client, events[0]));
	REQUIRE(reader->info().broadcast.has_value());
	CHECK(
		reader->info().broadcast.value_or(gnubase::engine::BroadcastInfo{}).timeReference == (3600ULL * 48000) - 2400);
}

TEST_CASE("a pre-record that reaches back before 0:00:00 gives a time reference of 0 (#580)", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "audio.preRecordSeconds"}, {"value", 0.05}}); // 2400 frames
	run(client, "project.set", {{"recordFileType", "broadcastWav"}, {"startTime", 0.0}});
	run(client, "audio.render", {{"frames", 4800}}); // stopped
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 1000}});
	REQUIRE(setup.stop().is_object());

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("offset") == 2400);
	const auto reader = AudioFileReader::open(pathOf(client, events[0]));
	REQUIRE(reader->info().broadcast.has_value());
	CHECK(reader->info().broadcast.value_or(gnubase::engine::BroadcastInfo{}).timeReference == 0);
}

TEST_CASE("a long track name is cut between two UTF-8 characters for the description and the file name (#580)",
	"[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	// An ASCII character, 127 two-byte characters and a three-byte one: 258 bytes, so the file name's 200 would split
	// the 100th two-byte character and the description's 256 the three-byte one.
	std::string name = "a";
	for (int i = 0; i < 127; ++i)
		name += "\xc3\xa9";
	name += "\xe2\x82\xac";
	run(client, "track.set", {{"track", setup.track}, {"name", name}});
	run(client, "project.set", {{"recordFileType", "broadcastWav"}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(setup.stop().is_object());

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	const std::string path = pathOf(client, events[0]);
	CHECK(std::filesystem::u8path(path).filename().u8string() == name.substr(0, 199) + ".wav");
	const auto reader = AudioFileReader::open(path);
	CHECK(reader->info().broadcast.value_or(gnubase::engine::BroadcastInfo{}).description == name.substr(0, 255));
}

TEST_CASE("a change of only the start time reaches the next Broadcast Wave recording (#581)", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "project.set", {{"recordFileType", "broadcastWav"}, {"startTime", 3600.0}});
	const auto record = [&](int64_t position)
	{
		run(client, "transport.locate", {{"position", position}});
		run(client, "transport.record");
		run(client, "audio.render", {{"frames", 640}});
		REQUIRE(setup.stop().is_object());
		std::string path;
		for (const Json& event : eventsOn(client, setup.track))
			if (event.at("position") == position)
				path = pathOf(client, event);
		REQUIRE(!path.empty());
		return AudioFileReader::open(path)->info().broadcast.value_or(gnubase::engine::BroadcastInfo{}).timeReference;
	};
	CHECK(record(0) == 3600ULL * 48000);
	run(client, "project.set", {{"startTime", 7200.0}});
	CHECK(record(48000) == 7201ULL * 48000);
	// 23:59:59 and 2 s into the project: past midnight, which the time reference doesn't wrap at.
	run(client, "project.set", {{"startTime", 86399.0}});
	CHECK(record(96000) == 86401ULL * 48000);
}

TEST_CASE("cycle recording makes a take per lap, all in one file", "[integration]")
{
	std::string mode = "keepHistory";
	SECTION("keep history keeps every lap")
	{
	}
	SECTION("replace keeps what the last lap recorded")
	{
		mode = "replace";
	}

	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	CHECK(run(client, "transport.set", {{"left", 500}, {"right", 2000}, {"cycle", true}, {"audioRecordMode", mode}})
			  .at("audioRecordMode") == mode);
	run(client, "transport.locate", {{"position", 500}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 3700}}); // two laps of 1500 frames and 700 of a third
	REQUIRE(setup.stop().is_object());

	const std::vector<Json> events = eventsOn(client, setup.track);
	const std::string path = pathOf(client, events.at(0));
	const std::vector<float> file = channelOf(path, 0);
	REQUIRE(file.size() == 3700);
	if (mode == "keepHistory")
	{
		REQUIRE(events.size() == 3);
		std::vector<int64_t> offsets;
		for (const Json& event : events)
		{
			CHECK(event.at("position") == 500);
			CHECK(pathOf(client, event) == path);
			offsets.push_back(event.at("offset").get<int64_t>());
		}
		std::sort(offsets.begin(), offsets.end());
		CHECK(offsets == std::vector<int64_t>{0, 1500, 3000});
	}
	else
	{
		// The third lap replaced the start of the second, which replaced the first.
		REQUIRE(events.size() == 2);
		CHECK(events[0].at("position") == 500);
		CHECK(events[0].at("offset") == 3000);
		CHECK(events[0].at("length") == 700);
		CHECK(events[1].at("position") == 1200);
		CHECK(events[1].at("offset") == 2200);
		CHECK(events[1].at("length") == 800);
	}
	// What the input recorded last at each position, which audio.captureInput keeps too.
	CHECK(slice(file, 3000, 700) == recorded(client, 0, 500, 700));
	CHECK(slice(file, 2200, 800) == recorded(client, 0, 1200, 800));
}

TEST_CASE("a four-lap cycle recording makes four takes on lanes of their own, the last one playing", "[integration]")
{
	const std::string mode = GENERATE("keepHistory", "cycleHistoryReplace");
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "transport.set", {{"left", 500}, {"right", 2000}, {"cycle", true}, {"audioRecordMode", mode}});
	run(client, "transport.locate", {{"position", 500}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 5800}}); // three laps of 1500 frames and 1300 of a fourth
	const Json added = setup.stop();
	REQUIRE(added.is_object());
	CHECK(added.at("events").size() == 4);

	std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 4);
	std::sort(events.begin(), events.end(), [](const Json& a, const Json& b) { return a.at("lane") < b.at("lane"); });
	for (size_t lap = 0; lap < 4; ++lap)
	{
		INFO("lap " << lap);
		CHECK(events[lap].at("lane") == lap);
		CHECK(events[lap].at("position") == 500);
		CHECK(events[lap].at("offset") == 1500 * static_cast<int64_t>(lap));
		CHECK(events[lap].at("length") == (lap < 3 ? 1500 : 1300));
	}
	// The last take was added last, so it plays where the takes overlap.
	CHECK(events[3].at("id") == added.at("events").at(3));
	// Its clip has a region per take (reference manual p. 226).
	const Json project = run(client, "project.get");
	for (const Json& clip : project.at("clips"))
		if (clip.at("id") == added.at("clips").at(0))
			CHECK(clip.at("regions").size() == 4);
	const Json info = run(client, "event.get", {{"events", Json::array({events[2].at("id")})}});
	CHECK(info.at("events").at(0).at("lane") == 2);
}

TEST_CASE("audio pre-record keeps the input from before recording started in the file", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "audio.preRecordSeconds"}, {"value", 0.05}}); // 2400 frames
	run(client, "audio.render", {{"frames", 4800}});                                   // stopped
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 1000}});
	REQUIRE(setup.stop().is_object());

	// The event starts where recording started; the pre-recorded input is before it in the file.
	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("position") == 0);
	CHECK(events[0].at("offset") == 2400);
	CHECK(events[0].at("length") == 1000);
	const std::vector<float> file = channelOf(pathOf(client, events[0]), 0);
	REQUIRE(file.size() == 3400);
	CHECK(slice(file, 2400, 1000) == recorded(client, 0, 0, 1000));
	CHECK(std::any_of(file.begin(), file.begin() + 2400, [](float sample) { return sample != 0.0f; }));
}

TEST_CASE("punch recording replaces only the punch range, after the pre-roll and before the post-roll", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	const ObjectId media = idOf(run(client, "media.add", {{"path", writeRamp(setup.dir, 9600)}}), "media");
	const ObjectId clip = idOf(run(client, "clip.add", {{"media", media}}), "clip");
	run(client, "audioEvent.add", {{"track", setup.track}, {"clip", clip}, {"position", 0}});
	run(client, "settings.set", {{"key", "audio.stopAfterPunchOut"}, {"value", true}});
	run(client, "transport.set",
		{{"left", 2400}, {"right", 4800}, {"punchIn", true}, {"punchOut", true}, {"preRoll", true},
			{"preRollAmount", 1200}, {"postRoll", true}, {"postRollAmount", 960}, {"audioRecordMode", "replace"}});
	run(client, "transport.locate", {{"position", 2400}});
	const Json started = run(client, "transport.record");
	CHECK(started.at("playing") == true);
	CHECK(started.at("recording") == false); // the pre-roll first
	CHECK(started.at("position") == 1200);
	run(client, "audio.render", {{"frames", 9600}});
	REQUIRE(waitForRecording(client).is_object()); // the punch out ended the recording
	const Json transport = run(client, "transport.get");
	CHECK(transport.at("playing") == false); // stopped after the post-roll
	CHECK(transport.at("recording") == false);
	CHECK(transport.at("position") == 5760);

	// The take replaced the event between the locators and nothing else.
	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 3);
	const auto span = [](const Json& event)
	{ return std::pair{event.at("position").get<int64_t>(), event.at("length").get<int64_t>()}; };
	CHECK(span(events[0]) == std::pair<int64_t, int64_t>{0, 2400});
	CHECK(span(events[1]) == std::pair<int64_t, int64_t>{2400, 2400});
	CHECK(span(events[2]) == std::pair<int64_t, int64_t>{4800, 4800});
	const std::vector<float> take = channelOf(pathOf(client, events[1]), 0);
	REQUIRE(take.size() == 2400);
	CHECK(take == recorded(client, 0, 2400, 2400));
}

TEST_CASE("switching off stop after punch out during the post-roll lets playback go on", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "audio.stopAfterPunchOut"}, {"value", true}});
	run(client, "transport.set",
		{{"left", 640}, {"right", 1280}, {"punchIn", true}, {"punchOut", true}, {"postRoll", true},
			{"postRollAmount", 1280}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 1920}}); // punched out at 1280, the stop waits at 2560
	REQUIRE(waitForRecording(client).is_object());
	run(client, "settings.set", {{"key", "audio.stopAfterPunchOut"}, {"value", false}});
	run(client, "audio.render", {{"frames", 1280}});
	const Json transport = run(client, "transport.get");
	CHECK(transport.at("playing") == true);
	CHECK(transport.at("position") == 3200);
	run(client, "transport.stop");
}

TEST_CASE("only record-enabled audio tracks record, and nothing records without recording", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	const ObjectId idle = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 640}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});
	run(client, "transport.record", {{"record", false}}); // punches out; playback goes on
	run(client, "audio.render", {{"frames", 640}});
	const Json added = waitForRecording(client);
	REQUIRE(added.is_object());
	run(client, "transport.stop");

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("position") == 640);
	CHECK(events[0].at("length") == 640);
	CHECK(eventsOn(client, idle).empty());
	CHECK(std::filesystem::directory_iterator(setup.dir.path() / "Audio") != std::filesystem::directory_iterator());
}

TEST_CASE("tracks of the same name record into files of their own, next to the ones in the folder", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	const Json other = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}});
	run(client, "channel.set", {{"channel", other.at("channel").at("id")}, {"recordEnable", true}});
	const std::vector<ObjectId> tracks{setup.track, idOf(other, "track")};
	for (const ObjectId track : tracks)
		run(client, "track.set", {{"track", track}, {"name", "Vocals"}});
	const std::filesystem::path audio = setup.dir.path() / "Audio";
	std::filesystem::create_directories(audio);
	std::ofstream(audio / "VOCALS.wav") << "there"; // differs only in case: taken on every file system
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(setup.stop().is_object());

	std::set<std::string> names;
	for (const ObjectId track : tracks)
		for (const Json& event : eventsOn(client, track))
			names.insert(std::filesystem::u8path(pathOf(client, event)).filename().u8string());
	CHECK(names == std::set<std::string>{"Vocals-2.wav", "Vocals-3.wav"});
	CHECK(gnubase::test::readText(audio / "VOCALS.wav") == "there");
}

TEST_CASE(
	"a track named after a Windows device records into a file of its own name with _ added (#531)", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	const auto name = GENERATE(as<std::string>{}, "con", "Nul .mix", "COM1", "lpt9.take", "Console");
	run(client, "track.set", {{"track", setup.track}, {"name", name}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(setup.stop().is_object());

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	const std::string file = std::filesystem::u8path(pathOf(client, events[0])).filename().u8string();
	if (name == "Console")
		CHECK(file == "Console.wav");
	else if (name == "Nul .mix")
		CHECK(file == "Nul _.mix.wav");
	else
		CHECK(
			file == name.substr(0, name.find('.')) + "_" + name.substr(std::min(name.find('.'), name.size())) + ".wav");
}

TEST_CASE("a recording isn't added to a project opened while it was recorded", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	const auto before = run(client, "project.revision").at("project").get<uint64_t>();
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});

	// The new project has an armed audio track too, which may well have the old track's id.
	run(client, "project.new");
	CHECK(run(client, "project.revision").at("project").get<uint64_t>() != before);
	const Json added = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}});
	run(client, "channel.set", {{"channel", added.at("channel").at("id")}, {"recordEnable", true}});
	run(client, "audio.render", {{"frames", 640}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	const Json failed = waitFor(client, "recording.failed");
	REQUIRE(failed.is_object());
	CHECK_THAT(failed.at("reason").get<std::string>(), ContainsSubstring("another project"));
	CHECK(run(client, "project.get").at("audioEvents").empty());
	CHECK(run(client, "history.get").at("steps").back().at("action") != "Record");
}

TEST_CASE("the takes of a track removed while recording are skipped, the others still land", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	const Json other = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}});
	run(client, "channel.set", {{"channel", other.at("channel").at("id")}, {"recordEnable", true}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});
	run(client, "track.remove", {{"track", idOf(other, "track")}});
	run(client, "audio.render", {{"frames", 640}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	std::vector<std::string> failures;
	const Json added = waitFor(client, "recording.added", &failures);
	REQUIRE(added.is_object());
	REQUIRE(failures.size() == 1);
	CHECK_THAT(failures[0], ContainsSubstring("removed"));

	const std::vector<Json> events = eventsOn(client, setup.track);
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("position") == 0);
	CHECK(events[0].at("length") == 1280);
	CHECK(added.at("events") == Json::array({events[0].at("id")}));
}

TEST_CASE("recording.add applies the audio record modes to the events a take overlaps", "[integration]")
{
	const gnubase::test::TempDir dir;
	Engine engine;
	Client client(engine);
	const std::string file = writeRamp(dir, 2000);
	const ObjectId track = idOf(run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}), "track");
	const ObjectId media = idOf(run(client, "media.add", {{"path", file}}), "media");
	const ObjectId clip = idOf(run(client, "clip.add", {{"media", media}}), "clip");
	const ObjectId existing = idOf(
		run(client, "audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}, {"length", 1000}}), "event");
	const auto add = [&](const std::string& mode, const Json& takes)
	{
		return run(client, "recording.add",
			{{"mode", mode}, {"tracks", Json::array({{{"track", track}, {"path", file}, {"takes", takes}}})}});
	};
	// [position, length] of the track's events, by position.
	const auto spans = [&]
	{
		std::vector<std::pair<int64_t, int64_t>> result;
		for (const Json& event : eventsOn(client, track))
			result.emplace_back(event.at("position").get<int64_t>(), event.at("length").get<int64_t>());
		return result;
	};
	const Json one = Json::array({{{"position", 400}, {"offset", 0}, {"length", 200}}});
	const Json two = Json::array(
		{{{"position", 400}, {"offset", 0}, {"length", 200}}, {{"position", 500}, {"offset", 200}, {"length", 200}}});
	using Spans = std::vector<std::pair<int64_t, int64_t>>;

	SECTION("keep history keeps what was there")
	{
		const Json result = add("keepHistory", one);
		CHECK(spans() == Spans{{0, 1000}, {400, 200}});
		CHECK(result.at("events").size() == 1);
		CHECK(result.at("media").size() == 1);
	}
	SECTION("replace cuts out what a take overlaps")
	{
		add("replace", one);
		CHECK(spans() == Spans{{0, 400}, {400, 200}, {600, 400}});
	}
	SECTION("cycle history + replace keeps every take and replaces what was there")
	{
		add("cycleHistoryReplace", two);
		CHECK(spans() == Spans{{0, 400}, {400, 200}, {500, 200}, {700, 300}});
	}
	SECTION("cycle history + replace cuts out only what each take overlaps")
	{
		add("cycleHistoryReplace",
			Json::array({{{"position", 100}, {"offset", 0}, {"length", 100}},
				{{"position", 700}, {"offset", 100}, {"length", 100}}}));
		CHECK(spans() == Spans{{0, 100}, {100, 100}, {200, 500}, {700, 100}, {800, 200}});
	}
	SECTION("a track's files, as a split recording sends them, are one recording that keeps every take")
	{
		const Json result = run(client, "recording.add",
			{{"mode", "cycleHistoryReplace"}, {"cycle", true},
				{"tracks",
					Json::array({{{"track", track}, {"path", file}, {"takes", Json::array({two[0]})}},
						{{"track", track}, {"path", file}, {"takes", Json::array({two[1]})}}})}});
		CHECK(result.at("events").size() == 2);
		CHECK(spans() == Spans{{0, 400}, {400, 200}, {500, 200}, {700, 300}});
		std::vector<std::pair<int64_t, int64_t>> lanes; // [position, lane]: the laps stack across the files
		for (const Json& event : eventsOn(client, track))
			lanes.emplace_back(event.at("position").get<int64_t>(), event.at("lane").get<int64_t>());
		std::sort(lanes.begin(), lanes.end());
		CHECK(lanes == std::vector<std::pair<int64_t, int64_t>>{{0, 0}, {400, 0}, {500, 1}, {700, 0}});
	}
	SECTION("a lap that the split cuts stays on one lane, and the regions follow the laps across the files (#2102)")
	{
		// Three laps over [0, 1000), the first file full after 1500 frames: the second lap goes on in the next file.
		const std::string mode = GENERATE("keepHistory", "cycleHistoryReplace");
		const Json result = run(client, "recording.add",
			{{"mode", mode}, {"cycle", true},
				{"tracks",
					Json::array({{{"track", track}, {"path", file},
									 {"takes",
										 Json::array({{{"position", 0}, {"offset", 0}, {"length", 1000}},
											 {{"position", 0}, {"offset", 1000}, {"length", 500}}})}},
						{{"track", track}, {"path", file},
							{"takes",
								Json::array({{{"position", 500}, {"offset", 0}, {"length", 500}, {"continues", true}},
									{{"position", 0}, {"offset", 500}, {"length", 1000}}})}}})}});
		std::vector<std::pair<int64_t, int64_t>> lanes; // [offset, lane] of the takes' events
		for (const Json& event : eventsOn(client, track))
			if (event.at("id") != existing)
				lanes.emplace_back(event.at("offset").get<int64_t>(), event.at("lane").get<int64_t>());
		std::sort(lanes.begin(), lanes.end());
		const int64_t first = mode == "keepHistory" ? 1 : 0; // after the existing event's lane, or in its place
		// Offsets 0 and 1000 are the first file's, 0 and 500 the second's: lap 1, lap 2 in both files, lap 3.
		CHECK(lanes ==
			std::vector<std::pair<int64_t, int64_t>>{{0, first}, {0, first + 1}, {500, first + 2}, {1000, first + 1}});
		std::vector<std::vector<std::string>> names; // the regions' names per clip
		const Json project = run(client, "project.get");
		for (const Json& id : result.at("clips"))
			for (const Json& each : project.at("clips"))
				if (each.at("id") == id)
				{
					names.emplace_back();
					for (const Json& region : each.at("regions"))
						names.back().push_back(region.at("name").get<std::string>());
				}
		CHECK(names == std::vector<std::vector<std::string>>{{"Take 1", "Take 2"}, {"Take 2", "Take 3"}});
	}
	SECTION("replace keeps only the last take where takes overlap")
	{
		add("replace", two);
		CHECK(spans() == Spans{{0, 400}, {400, 100}, {500, 200}, {700, 300}});
	}
	SECTION("the takes of a cycle recording go onto lanes after those of the events where they lie")
	{
		// [position, lane] of the track's events, by position and lane.
		const auto lanes = [&]
		{
			std::vector<std::pair<int64_t, int64_t>> result;
			for (const Json& event : eventsOn(client, track))
				result.emplace_back(event.at("position").get<int64_t>(), event.at("lane").get<int64_t>());
			std::sort(result.begin(), result.end());
			return result;
		};
		using Lanes = std::vector<std::pair<int64_t, int64_t>>;
		const std::string mode = GENERATE("keepHistory", "cycleHistoryReplace", "replace");
		add(mode, two);
		if (mode == "keepHistory")
			CHECK(lanes() == Lanes{{0, 0}, {400, 1}, {500, 2}});
		else if (mode == "cycleHistoryReplace") // what they overlapped is gone
			CHECK(lanes() == Lanes{{0, 0}, {400, 0}, {500, 1}, {700, 0}});
		else // only the last take stays where they overlap: no lanes
			CHECK(lanes() == Lanes{{0, 0}, {400, 0}, {500, 0}, {700, 0}});
		run(client, "history.undo");
		CHECK(lanes() == Lanes{{0, 0}});
	}
	SECTION("the lanes of the events in an audio part are the part's: the takes go after the part's lane")
	{
		run(client, "event.toPart", {{"events", {existing}}});
		run(client, "event.set", {{"events", {existing}}, {"lane", 2}}); // a lane in the part (audio part editor)
		add("keepHistory", two);
		std::vector<std::pair<int64_t, int64_t>> lanes;
		for (const Json& event : eventsOn(client, track))
			lanes.emplace_back(event.at("position").get<int64_t>(), event.at("lane").get<int64_t>());
		std::sort(lanes.begin(), lanes.end());
		CHECK(lanes == std::vector<std::pair<int64_t, int64_t>>{{0, 2}, {400, 1}, {500, 2}});
	}
	SECTION("a recording of one take stays on the first lane")
	{
		add("keepHistory", one);
		for (const Json& event : eventsOn(client, track))
			CHECK(event.at("lane") == 0);
	}
	SECTION("only takes that overlap another take go onto lanes of their own")
	{
		// The third take, after a locate during recording, overlaps no other take: it stays on the first lane.
		add("keepHistory",
			Json::array({{{"position", 400}, {"offset", 0}, {"length", 200}},
				{{"position", 500}, {"offset", 200}, {"length", 200}},
				{{"position", 1500}, {"offset", 400}, {"length", 100}}}));
		std::vector<std::pair<int64_t, int64_t>> lanes;
		for (const Json& event : eventsOn(client, track))
			lanes.emplace_back(event.at("position").get<int64_t>(), event.at("lane").get<int64_t>());
		std::sort(lanes.begin(), lanes.end());
		CHECK(lanes == std::vector<std::pair<int64_t, int64_t>>{{0, 0}, {400, 1}, {500, 2}, {1500, 0}});
	}
	SECTION("the takes' lanes stop at the last lane event.set can set")
	{
		run(client, "event.set", {{"events", {existing}}, {"lane", 999}});
		add("keepHistory",
			Json::array({{{"position", 400}, {"offset", 0}, {"length", 200}},
				{{"position", 400}, {"offset", 200}, {"length", 200}},
				{{"position", 400}, {"offset", 400}, {"length", 200}}}));
		for (const Json& event : eventsOn(client, track))
			if (event.at("id") != existing)
				CHECK(event.at("lane") == 1000);
	}
	SECTION("a cycle recording makes a region of each take on its clip")
	{
		const bool cycle = GENERATE(false, true);
		const Json added = run(client, "recording.add",
			{{"cycle", cycle}, {"tracks", Json::array({{{"track", track}, {"path", file}, {"takes", two}}})}});
		Json regions = Json::array();
		const Json project = run(client, "project.get");
		for (const Json& each : project.at("clips"))
			if (each.at("id") == added.at("clips").at(0))
				regions = each.at("regions");
		if (!cycle) // a linear recording's takes, after a locate, are no laps
		{
			CHECK(regions.empty());
		}
		else
		{
			REQUIRE(regions.size() == 2);
			CHECK(regions[0].at("name") == "Take 1");
			CHECK(regions[0].at("start") == 0);
			CHECK(regions[0].at("end") == 200);
			CHECK(regions[1].at("name") == "Take 2");
			CHECK(regions[1].at("start") == 200);
			CHECK(regions[1].at("end") == 400);
		}
	}
	SECTION("a locked event stays")
	{
		run(client, "event.set", {{"events", {existing}}, {"lock", true}});
		add("replace", one);
		CHECK(spans() == Spans{{0, 1000}, {400, 200}});
	}
	SECTION("refusals")
	{
		CHECK_THAT(
			reject(client, "recording.add",
				{{"mode", "merge"}, {"tracks", Json::array({{{"track", track}, {"path", file}, {"takes", one}}})}}),
			ContainsSubstring("mode"));
		const ObjectId midi = idOf(run(client, "track.add", {{"type", "midi"}}), "track");
		CHECK_THAT(reject(client, "recording.add",
					   {{"tracks", Json::array({{{"track", midi}, {"path", file}, {"takes", one}}})}}),
			ContainsSubstring("audio track"));
		CHECK_THAT(reject(client, "recording.add",
					   {{"tracks",
						   Json::array({{{"track", track}, {"path", file},
							   {"takes", Json::array({{{"position", 0}, {"offset", 1900}, {"length", 200}}})}}})}}),
			ContainsSubstring("file"));
		CHECK(spans() == Spans{{0, 1000}});
	}
}

TEST_CASE("the audio record mode is a transport setting", "[integration]")
{
	Engine engine;
	Client client(engine);
	CHECK(run(client, "transport.get").at("audioRecordMode") == "keepHistory");
	CHECK(run(client, "transport.set", {{"audioRecordMode", "cycleHistoryReplace"}}).at("audioRecordMode") ==
		"cycleHistoryReplace");
	CHECK_THAT(reject(client, "transport.set", {{"audioRecordMode", "merge"}}), ContainsSubstring("audioRecordMode"));
	CHECK(run(client, "transport.get").at("audioRecordMode") == "cycleHistoryReplace");
}

TEST_CASE("Recorder::setup doesn't wait for the disk thread, which reports a recording under its lock", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	std::mutex mutex;
	std::condition_variable changed;
	bool finishing = false;
	bool setUp = false;
	bool sawSetUp = false;
	{
		// The disk thread holds its lock while it reports the recording: it waits there for setup to come back.
		engine::Recorder recorder(
			[&](const std::string& name, const Json&)
			{
				if (name != "recording.finished")
					return;
				std::unique_lock lock(mutex);
				finishing = true;
				changed.notify_all();
				sawSetUp = changed.wait_for(lock, 5s, [&] { return setUp; });
			});
		recorder.prepare(1, g_blockFrames);
		recorder.setup(recordInto(dir.path()));

		// A recorded block, then one that isn't: the recording ends.
		engine::PlayedPositions played(size_t{4} * g_blockFrames);
		processBlock(recorder, played, 0, true);
		processBlock(recorder, played, 1, false);
		{
			std::unique_lock lock(mutex);
			REQUIRE(changed.wait_for(lock, 5s, [&] { return finishing; }));
		}
		recorder.setup({});
		{
			const std::scoped_lock lock(mutex);
			setUp = true;
		}
		changed.notify_all();
	}
	CHECK(sawSetUp);
}

TEST_CASE("a recording the audio thread started keeps its setup when the next one comes first", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	const std::filesystem::path first = dir.path() / "first";
	const std::filesystem::path second = dir.path() / "second";
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::filesystem::path> paths;
	bool released = false;
	{
		// The disk thread waits in the report of the first recording while the audio thread starts the second one
		// and the next setup comes.
		engine::Recorder recorder(
			[&](const std::string& name, const Json& data)
			{
				if (name != "recording.finished")
					return;
				std::unique_lock lock(mutex);
				for (const Json& track : data.at("tracks"))
					paths.push_back(std::filesystem::u8path(track.at("path").get<std::string>()));
				changed.notify_all();
				changed.wait_for(lock, 5s, [&] { return released; });
			});
		recorder.prepare(1, g_blockFrames);
		recorder.setup(recordInto(first));
		engine::PlayedPositions played(size_t{8} * g_blockFrames);
		processBlock(recorder, played, 0, true);
		processBlock(recorder, played, 1, false);
		{
			std::unique_lock lock(mutex);
			REQUIRE(changed.wait_for(lock, 5s, [&] { return !paths.empty(); }));
		}
		processBlock(recorder, played, 2, true);
		recorder.setup(recordInto(second));
		processBlock(recorder, played, 3, false);
		{
			std::unique_lock lock(mutex);
			released = true;
			changed.notify_all();
			REQUIRE(changed.wait_for(lock, 5s, [&] { return paths.size() == 2; }));
		}
	}
	for (const std::filesystem::path& path : paths)
		CHECK(path.parent_path() == first);
	CHECK(!std::filesystem::exists(second));
}

TEST_CASE("the retrospective buffer doesn't wait while the disk thread creates a recording's files", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	// A file where the record folder should be: the disk thread reports the failure while it creates the files, and
	// waits there for the retrospective buffer to answer.
	const std::filesystem::path notAFolder = dir.path() / "file";
	std::ofstream(notAFolder) << "not a folder";
	std::mutex mutex;
	std::condition_variable changed;
	bool failing = false;
	bool answered = false;
	bool sawAnswer = false;
	{
		engine::Recorder recorder(
			[&](const std::string& name, const Json&)
			{
				if (name != "recording.failed")
					return;
				std::unique_lock lock(mutex);
				if (failing)
					return;
				failing = true;
				changed.notify_all();
				sawAnswer = changed.wait_for(lock, 5s, [&] { return answered; });
			});
		recorder.prepare(1, g_blockFrames);
		recorder.setup(recordInto(notAFolder));
		engine::PlayedPositions played(size_t{4} * g_blockFrames);
		processBlock(recorder, played, 0, true);
		{
			std::unique_lock lock(mutex);
			REQUIRE(changed.wait_for(lock, 5s, [&] { return failing; }));
		}
		CHECK(recorder.retrospective(1).empty());
		recorder.clearRetrospective(0);
		{
			const std::scoped_lock lock(mutex);
			answered = true;
		}
		changed.notify_all();
	}
	CHECK(sawAnswer);
}

TEST_CASE("the retrospective buffer leaves a recording's start to the disk thread", "[unit]")
{
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	// A file where the record folder should be: the thread that starts a recording reports the failure.
	const std::filesystem::path notAFolder = dir.path() / "file";
	std::ofstream(notAFolder) << "not a folder";
	const std::thread::id commandThread = std::this_thread::get_id();
	std::atomic<bool> startedOnCommandThread{false};
	{
		engine::Recorder recorder(
			[&](const std::string& name, const Json&)
			{
				if (name == "recording.failed" && std::this_thread::get_id() == commandThread)
					startedOnCommandThread = true;
			});
		recorder.prepare(1, g_blockFrames);
		recorder.setup(recordInto(notAFolder));
		// The command comes right after the chunk that starts a recording, well within the disk thread's poll.
		engine::PlayedPositions played(size_t{64} * g_blockFrames);
		for (uint64_t index = 0; index < 32; index += 2)
		{
			processBlock(recorder, played, index, true);
			if (index % 4 == 0)
				CHECK(recorder.retrospective(1).empty());
			else
				recorder.clearRetrospective(0);
			processBlock(recorder, played, index + 1, false);
		}
	}
	CHECK_FALSE(startedOnCommandThread);
}

TEST_CASE("MIDI input that finds no free MIDI chunk is counted (#641)", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	std::mutex mutex;
	std::condition_variable changed;
	bool finishing = false;
	bool released = false;
	{
		// The disk thread waits in the report of a recording while the audio thread hands it MIDI input.
		engine::Recorder recorder(
			[&](const std::string& name, const Json&)
			{
				if (name != "recording.finished")
					return;
				std::unique_lock lock(mutex);
				finishing = true;
				changed.notify_all();
				changed.wait_for(lock, 5s, [&] { return released; });
			});
		recorder.prepare(1, g_blockFrames);
		recorder.setup(recordInto(dir.path()));
		engine::PlayedPositions played(size_t{4} * g_blockFrames);
		processBlock(recorder, played, 0, true);
		processBlock(recorder, played, 1, false);
		{
			std::unique_lock lock(mutex);
			REQUIRE(changed.wait_for(lock, 5s, [&] { return finishing; }));
		}
		// The pool's 256 MIDI chunks take the first 256 blocks' input, two events each.
		const auto sequencer = std::make_shared<engine::MidiSequencer>();
		const engine::MidiTap::Sequencers sequencers = {{1, sequencer}};
		for (uint64_t index = 2; index < 2 + 300; ++index)
		{
			sequencer->input().clear();
			sequencer->input().add({});
			sequencer->input().add({});
			processBlock(recorder, played, index, false, sequencers);
		}
		CHECK(recorder.droppedMidi() == uint64_t{2} * 44);
		{
			const std::scoped_lock lock(mutex);
			released = true;
		}
		changed.notify_all();
	}
}

TEST_CASE("audio pre-record keeps only the armed tracks' input channels (#531)", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	std::mutex mutex;
	std::condition_variable changed;
	std::string path;
	{
		engine::Recorder recorder(
			[&](const std::string& name, const Json& data)
			{
				if (name != "recording.finished")
					return;
				const std::scoped_lock lock(mutex);
				path = data.at("tracks").at(0).at("path").get<std::string>();
				changed.notify_all();
			});
		// Four input channels, each frame of channel c c + 1 and the block's index tenths; the track records the third.
		recorder.prepare(4, g_blockFrames);
		engine::RecordSetup setup = recordInto(dir.path());
		setup.tracks = {{1, "Audio", {2}}};
		setup.preRecordFrames = int64_t{2} * g_blockFrames;
		recorder.setup(setup);
		engine::PlayedPositions played(size_t{8} * g_blockFrames);
		const auto block = [&](uint64_t index, bool recording)
		{
			std::vector<std::vector<float>> input(4);
			std::vector<const float*> inputs;
			for (size_t c = 0; c < input.size(); ++c)
			{
				input[c].assign(g_blockFrames, static_cast<float>(c + 1) + (static_cast<float>(index) / 10.0f));
				inputs.push_back(input[c].data());
			}
			const gnubase::audio::MidiBuffer midiIn;
			gnubase::audio::MidiBuffer midiOut;
			const engine::MidiTap::Sequencers sequencers;
			const uint64_t position = index * g_blockFrames;
			const engine::graph::TransportSegment segment{0, g_blockFrames, static_cast<int64_t>(position), recording};
			engine::graph::TransportBlock transport;
			transport.playing = true;
			transport.segments = &segment;
			transport.segmentCount = 1;
			transport.recording = recording;
			played.add(position, g_blockFrames, transport);
			recorder.process(
				{inputs.data(), nullptr, g_blockFrames, position, 0, midiIn, midiOut}, played, 0, sequencers);
		};
		for (uint64_t index = 0; index < 3; ++index)
			block(index, false);
		block(3, true);
		block(4, false);
		std::unique_lock lock(mutex);
		REQUIRE(changed.wait_for(lock, 5s, [&] { return !path.empty(); }));
	}
	// The last two blocks before recording started, then the recorded one, all of the third channel.
	const std::vector<float> file = channelOf(path, 0);
	REQUIRE(file.size() == 3 * g_blockFrames);
	for (size_t f = 0; f < file.size(); ++f)
	{
		const size_t block = (f / g_blockFrames) + 1;
		REQUIRE(file[f] == 3.0f + (static_cast<float>(block) / 10.0f));
	}
}

TEST_CASE("a pre-record history that can't be allocated fails the pre-record, not the recording (#531)", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> failures;
	bool finished = false;
	{
		engine::Recorder recorder(
			[&](const std::string& name, const Json& data)
			{
				const std::scoped_lock lock(mutex);
				if (name == "recording.failed")
					failures.push_back(data.at("reason").get<std::string>());
				finished = finished || name == "recording.finished";
				changed.notify_all();
			});
		recorder.prepare(1, g_blockFrames);
		engine::RecordSetup setup = recordInto(dir.path());
		setup.preRecordFrames = int64_t{1} << 62; // more than a vector holds
		recorder.setup(setup);
		engine::PlayedPositions played(size_t{4} * g_blockFrames);
		processBlock(recorder, played, 0, false);
		processBlock(recorder, played, 1, true);
		processBlock(recorder, played, 2, false);
		std::unique_lock lock(mutex);
		REQUIRE(changed.wait_for(lock, 5s, [&] { return finished; }));
	}
	REQUIRE(failures.size() == 1);
	CHECK_THAT(failures[0], ContainsSubstring("pre-record"));
}

TEST_CASE("a recording without a folder it can record into fails once and writes no file (#611)", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	std::filesystem::path folder;
	SECTION("none, as without a settings or a temporary folder")
	{
	}
	SECTION("a file where the folder would be")
	{
		folder = dir.path() / "file";
		std::ofstream(folder) << "not a folder";
	}
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> failures;
	bool finished = false;
	{
		engine::Recorder recorder(
			[&](const std::string& name, const Json& data)
			{
				const std::scoped_lock lock(mutex);
				if (name == "recording.failed")
					failures.push_back(data.at("reason").get<std::string>());
				finished = finished || name == "recording.finished";
				changed.notify_all();
			});
		recorder.prepare(1, g_blockFrames);
		recorder.setup(recordInto(folder));
		engine::PlayedPositions played(size_t{4} * g_blockFrames);
		processBlock(recorder, played, 0, true);
		processBlock(recorder, played, 1, false);
		std::unique_lock lock(mutex);
		REQUIRE(changed.wait_for(lock, 5s, [&] { return !failures.empty(); }));
	} // the disk thread has ended
	REQUIRE(failures.size() == 1);
	CHECK_THAT(failures[0], ContainsSubstring("folder"));
	CHECK(!finished);
	CHECK(!std::filesystem::exists("Audio.wav")); // not in the working folder either
}

TEST_CASE(
	"an offline render that waits for the recorder's chunks loses none while the disk thread is slow (#531)", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> paths;
	{
		// The disk thread takes a while to report the first recording, as on a slow disk.
		engine::Recorder recorder(
			[&](const std::string& name, const Json& data)
			{
				if (name != "recording.finished")
					return;
				std::unique_lock lock(mutex);
				paths.push_back(data.at("tracks").at(0).at("path").get<std::string>());
				changed.notify_all();
				if (paths.size() == 1)
				{
					lock.unlock();
					std::this_thread::sleep_for(300ms);
				}
			});
		recorder.prepare(1, g_blockFrames);
		recorder.setup(recordInto(dir.path()));
		engine::PlayedPositions played(size_t{4} * g_blockFrames);
		processBlock(recorder, played, 0, true);
		processBlock(recorder, played, 1, false);
		{
			std::unique_lock lock(mutex);
			REQUIRE(changed.wait_for(lock, 5s, [&] { return paths.size() == 1; }));
		}
		// Twice the chunks the pool holds (65536 frames), each block after the wait an offline render makes.
		constexpr uint64_t blocks = 2048;
		for (uint64_t index = 2; index < 2 + blocks; ++index)
		{
			REQUIRE(recorder.waitForChunks(5s));
			processBlock(recorder, played, index, true);
		}
		processBlock(recorder, played, 2 + blocks, false);
		CHECK(recorder.dropped() == 0);
		std::unique_lock lock(mutex);
		REQUIRE(changed.wait_for(lock, 5s, [&] { return paths.size() == 2; }));
		CHECK(channelOf(paths[1], 0).size() == blocks * g_blockFrames);
	}
}

TEST_CASE("the first save of a project copies its recordings into the project's media folder (#611)", "[integration]")
{
	const gnubase::test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	const Json added = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}});
	run(client, "channel.set", {{"channel", added.at("channel").at("id")}, {"recordEnable", true}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitForRecording(client).is_object());
	const std::vector<Json> events = eventsOn(client, idOf(added, "track"));
	REQUIRE(events.size() == 1);
	const std::filesystem::path recording = std::filesystem::u8path(pathOf(client, events[0]));
	CHECK(recording.parent_path() == dir.path() / "settings" / "Recordings");

	SECTION("once, also when it is saved again meanwhile")
	{
		gnubase::test::HeldJob held(engine, "project.save");
		const Json saved = run(client, "project.save", {{"path", (dir.path() / "Song.dawproject").u8string()}});
		REQUIRE(saved.at("job").is_object());
		REQUIRE(held.waitUntilHeld());
		CHECK(run(client, "project.save").at("job").is_null()); // the copies run already
		REQUIRE(held.release());
		REQUIRE(gnubase::test::waitForJob(client, saved.at("job").at("id").get<uint64_t>()).at("state") == "done");
		const std::filesystem::path audio = dir.path() / "Audio";
		CHECK(pathOf(client, events[0]) == (audio / recording.filename()).u8string());
		CHECK(channelOf(pathOf(client, events[0]), 0) == channelOf(recording.u8string(), 0));
		CHECK(!std::filesystem::exists(audio / (recording.stem().u8string() + "-2.wav")));
	}

	SECTION("but not into a project folder inside the recordings folder")
	{
		const std::filesystem::path inside = recording.parent_path() / "Song.dawproject";
		CHECK(run(client, "project.save", {{"path", inside.u8string()}}).at("job").is_null());
		CHECK(pathOf(client, events[0]) == recording.u8string());
	}
}

TEST_CASE("a recording that adds nothing leaves the project unmodified (#546)", "[integration]")
{
	Setup setup({{"blockSize", 64}, {"captureSeconds", 1}, {"input", sineInput()}});
	Client& client = setup.client;
	const auto modified = [&client] { return run(client, "project.revision").at("modified").get<bool>(); };
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 640}});
	SECTION("another project was opened")
	{
		run(client, "project.new");
	}
	SECTION("every recorded track was removed")
	{
		run(client, "track.remove", {{"track", setup.track}});
		run(client, "project.save");
	}
	REQUIRE(!modified());
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitFor(client, "recording.failed").is_object()); // recording.add runs on the command thread
	CHECK(!modified());
}

namespace
{
	// Writes the first 100 frames, or limit, then fails: as a full disk, or as AIFF and WAV without RF64 at 4 GB.
	template <typename Error> class Full final : public gnubase::engine::AudioFileWriter
	{
	public:
		explicit Full(std::unique_ptr<gnubase::engine::AudioFileWriter> writer, size_t limit = 100)
			: m_writer(std::move(writer)), m_limit(limit)
		{
		}
		void write(const float* buffer, size_t frames) override
		{
			if (m_frames + frames > m_limit)
				throw Error("the file is full");
			m_writer->write(buffer, frames);
			m_frames += frames;
		}
		void close() override { m_writer->close(); }

	private:
		std::unique_ptr<gnubase::engine::AudioFileWriter> m_writer;
		size_t m_limit;
		size_t m_frames = 0;
	};
}

TEST_CASE("a file that a write fails for ends its take there and still lands (#580)", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> failures;
	Json finished;
	{
		engine::Recorder recorder(
			[&](const std::string& name, const Json& data)
			{
				const std::scoped_lock lock(mutex);
				if (name == "recording.failed")
					failures.push_back(data.at("reason").get<std::string>());
				else
					finished = data;
				changed.notify_all();
			},
			[](const std::string& path, const engine::WriteOptions& options)
			{
				std::unique_ptr<engine::AudioFileWriter> writer = engine::AudioFileWriter::create(path, options);
				if (path.find("Full") != std::string::npos)
					writer = std::make_unique<Full<engine::AudioFileError>>(std::move(writer));
				return writer;
			});
		recorder.prepare(1, g_blockFrames);
		engine::RecordSetup setup = recordInto(dir.path());
		setup.tracks = {{1, "Full", {0}}, {2, "Audio", {0}}};
		recorder.setup(setup);
		engine::PlayedPositions played(size_t{8} * g_blockFrames);
		for (uint64_t index = 0; index < 4; ++index)
			processBlock(recorder, played, index, true);
		processBlock(recorder, played, 4, false);
		std::unique_lock lock(mutex);
		REQUIRE(changed.wait_for(lock, 5s, [&] { return finished.is_object(); }));
	}
	REQUIRE(failures.size() == 1);
	CHECK_THAT(failures[0], ContainsSubstring("the file is full"));
	const Json& tracks = finished.at("tracks");
	REQUIRE(tracks.size() == 2);
	for (const Json& track : tracks)
	{
		const bool full = track.at("track") == 1;
		INFO("track " << track.at("track"));
		const Json& takes = track.at("takes");
		REQUIRE(takes.size() == 1);
		CHECK(takes[0].at("position") == 0);
		CHECK(takes[0].at("length") == (full ? 1 : 4) * g_blockFrames); // the block that didn't fit isn't in it
		CHECK(channelOf(track.at("path").get<std::string>(), 0).size() == (full ? 1 : 4) * g_blockFrames);
	}
}

TEST_CASE("a full file, as a Wave file without RF64 or an AIFF file at 4 GB, goes on in a new file (#580)", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> failures;
	Json finished;
	{
		engine::Recorder recorder(
			[&](const std::string& name, const Json& data)
			{
				const std::scoped_lock lock(mutex);
				if (name == "recording.failed")
					failures.push_back(data.at("reason").get<std::string>());
				else
					finished = data;
				changed.notify_all();
			},
			[](const std::string& path, const engine::WriteOptions& options)
			{ return std::make_unique<Full<engine::AudioFileFull>>(engine::AudioFileWriter::create(path, options)); });
		recorder.prepare(1, g_blockFrames);
		engine::RecordSetup setup = recordInto(dir.path());
		setup.format.broadcast = engine::BroadcastInfo{};
		setup.format.broadcast->timeReference = 1000;
		recorder.setup(setup);
		engine::PlayedPositions played(size_t{8} * g_blockFrames);
		for (uint64_t index = 0; index < 3; ++index)
			processBlock(recorder, played, index, true);
		processBlock(recorder, played, 3, false);
		std::unique_lock lock(mutex);
		REQUIRE(changed.wait_for(lock, 5s, [&] { return finished.is_object(); }));
	}
	CHECK(failures.empty());
	// Each file takes one block: the take goes on in the next file, at the next position, and says so (#2102). The
	// Broadcast Wave time reference counts on from the file before (#2096).
	const Json& tracks = finished.at("tracks");
	REQUIRE(tracks.size() == 3);
	const char* const names[] = {"Audio.wav", "Audio-2.wav", "Audio-3.wav"};
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		INFO("file " << i);
		const Json& track = tracks[i];
		CHECK(track.at("track") == 1);
		const std::string path = track.at("path").get<std::string>();
		CHECK(std::filesystem::u8path(path).filename().u8string() == names[i]);
		Json take = {{"position", i * g_blockFrames}, {"offset", 0}, {"length", g_blockFrames}};
		if (i > 0)
			take["continues"] = true;
		CHECK(track.at("takes") == Json::array({take}));
		CHECK(channelOf(path, 0).size() == g_blockFrames);
		CHECK(engine::AudioFileReader::open(path)->info().broadcast.value_or(engine::BroadcastInfo{}).timeReference ==
			1000 + (i * g_blockFrames));
	}
}

TEST_CASE("a next file that can't be written is removed, and the full one still lands (#2096)", "[unit]")
{
	using namespace std::chrono_literals;
	namespace engine = gnubase::engine;
	const gnubase::test::TempDir dir;
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> failures;
	Json finished;
	{
		engine::Recorder recorder(
			[&](const std::string& name, const Json& data)
			{
				const std::scoped_lock lock(mutex);
				if (name == "recording.failed")
					failures.push_back(data.at("reason").get<std::string>());
				else
					finished = data;
				changed.notify_all();
			},
			[](const std::string& path, const engine::WriteOptions& options)
			{
				std::unique_ptr<engine::AudioFileWriter> writer = engine::AudioFileWriter::create(path, options);
				// The next file has its header, but its first write fails, as on a full disk.
				if (path.find("Audio-2") != std::string::npos)
					return std::unique_ptr<engine::AudioFileWriter>(
						std::make_unique<Full<engine::AudioFileError>>(std::move(writer), 0));
				return std::unique_ptr<engine::AudioFileWriter>(
					std::make_unique<Full<engine::AudioFileFull>>(std::move(writer)));
			});
		recorder.prepare(1, g_blockFrames);
		recorder.setup(recordInto(dir.path()));
		engine::PlayedPositions played(size_t{8} * g_blockFrames);
		for (uint64_t index = 0; index < 3; ++index)
			processBlock(recorder, played, index, true);
		processBlock(recorder, played, 3, false);
		std::unique_lock lock(mutex);
		REQUIRE(changed.wait_for(lock, 5s, [&] { return finished.is_object(); }));
	}
	REQUIRE(failures.size() == 1);
	CHECK_THAT(failures[0], ContainsSubstring("in a new file"));
	const Json& tracks = finished.at("tracks");
	REQUIRE(tracks.size() == 1);
	CHECK(tracks[0].at("takes") == Json::array({{{"position", 0}, {"offset", 0}, {"length", g_blockFrames}}}));
	// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
	CHECK(!std::filesystem::exists(dir.path() / "Audio-2.wav"));
}
