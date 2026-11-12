// SPDX-License-Identifier: GPL-3.0-or-later
// Audio recording (#96, reference manual pp. 335-346): record-enabled tracks record their input bus into files
// written off the audio thread, cycle laps become takes, the audio record modes, and audio pre-record.
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
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
		{"wav", "float64", AudioFormat::Wav, SampleFormat::Float64, ".wav", 0.0f},
		{"aiff", "int24", AudioFormat::Aiff, SampleFormat::Int24, ".aif", 1.0f / 8388608.0f},
		{"aiff", "int32", AudioFormat::Aiff, SampleFormat::Int32, ".aif", 1.0f / 8388608.0f},
		{"flac", "int16", AudioFormat::Flac, SampleFormat::Int16, ".flac", 1.0f / 32768.0f},
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
