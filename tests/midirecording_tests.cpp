// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI recording (#97, reference manual pp. 347-355, 1609-1616): record-enabled MIDI and instrument tracks record
// their MIDI input into parts, by the MIDI record mode, with record filter, auto quantize, snapping parts to bars,
// the catch range and retrospective recording, and they play all MIDI inputs while monitoring or record-enabled.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "platform/audio/device.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	// 120 BPM at 48 kHz: a beat is 24000 samples.
	constexpr int64_t g_beat = 24000;
	constexpr int64_t g_beatTicks = 1720320;

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
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	Json message(int64_t position, std::vector<int> bytes)
	{
		return {{"position", position}, {"bytes", std::move(bytes)}};
	}

	// Waits for the event, null after 10 s.
	Json waitFor(Client& client, const std::string& name)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> next = client.wait(std::chrono::milliseconds(50));
			const auto* event = next ? std::get_if<Event>(&*next) : nullptr;
			if (event != nullptr && event->name == name)
				return event->data;
		}
		return nullptr;
	}

	// The note-ons (0x90) and note-offs (0x80) an instrument played, as [status, pitch, frame].
	std::vector<std::vector<int64_t>> notesPlayed(Client& client)
	{
		std::vector<std::vector<int64_t>> notes;
		const Json capture = run(client, "midi.capture");
		for (const Json& captured : capture.at("captured"))
		{
			const auto status = captured.at("bytes").at(0).get<int64_t>() & 0xF0;
			if (status == 0x90 || status == 0x80)
				notes.push_back(
					{status, captured.at("bytes").at(1).get<int64_t>(), captured.at("position").get<int64_t>()});
		}
		return notes;
	}

	// A record-enabled instrument track playing the note probe, on the dummy device.
	struct Setup
	{
		explicit Setup(int blockSize = 64, std::vector<std::unique_ptr<gnubase::midi::Backend>> midi = {})
			: engine(gnubase::audio::makeBackends(), {}, std::move(midi))
		{
			run(client, "audio.setDevice", {{"blockSize", blockSize}, {"inputChannels", 0}, {"captureSeconds", 1}});
			const Json added = run(client, "track.add", {{"type", "instrument"}});
			track = added.at("track").at("id").get<ObjectId>();
			channel = added.at("channel").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}, {"recordEnable", true}});
		}

		void inject(std::vector<Json> messages)
		{
			run(client, "midi.inject", {{"channel", channel}, {"messages", std::move(messages)}});
		}

		// Stops and waits until the recording is in the project.
		Json stop()
		{
			run(client, "transport.stop");
			run(client, "audio.render", {{"frames", 640}});
			return waitFor(client, "recording.added");
		}

		std::vector<Json> parts()
		{
			std::vector<Json> result;
			const Json project = run(client, "project.get");
			for (const Json& part : project.at("midiParts"))
				if (part.at("track") == track)
					result.push_back(part);
			std::sort(result.begin(), result.end(),
				[](const Json& a, const Json& b) { return a.at("position") < b.at("position"); });
			return result;
		}

		Engine engine;
		Client client{engine};
		ObjectId track = 0;
		ObjectId channel = 0;
	};

	// The notes of a part as [position, length, pitch], sorted.
	std::vector<std::vector<int64_t>> notesOf(const Json& part)
	{
		std::vector<std::vector<int64_t>> notes;
		for (const Json& note : part.at("notes"))
			notes.push_back({note.at("position").get<int64_t>(), note.at("length").get<int64_t>(),
				note.at("pitch").get<int64_t>()});
		std::sort(notes.begin(), notes.end());
		return notes;
	}

	Json note(int64_t position, int64_t length, int pitch)
	{
		return {{"position", position}, {"length", length}, {"pitch", pitch}, {"velocity", 0.75}};
	}
}

TEST_CASE("MIDI injected through MCP is recorded with exact timing and plays back where it was played", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	Setup setup(blockSize);
	Client& client = setup.client;
	CHECK(run(client, "transport.record").at("recording") == true); // from frame 0 at position 0
	setup.inject({message(1000, {0x90, 60, 100}), message(2500, {0x90, 64, 90}), message(3000, {0x80, 60, 0}),
		message(4000, {0xB0, 1, 64}), message(5000, {0x90, 64, 0})});
	run(client, "audio.render", {{"frames", 6000}});
	const Json added = setup.stop();
	REQUIRE(added.is_object());
	CHECK(added.at("parts").size() == 1);

	const std::vector<Json> parts = setup.parts();
	REQUIRE(parts.size() == 1);
	CHECK(parts[0].at("position") == 0); // snapped to the bars around it, by default
	CHECK(parts[0].at("length") == 4 * g_beatTicks);
	REQUIRE(parts[0].at("notes").size() == 2);
	REQUIRE(parts[0].at("controllers").size() == 1);
	CHECK(parts[0].at("controllers")[0].at("type") == "controller");
	CHECK(parts[0].at("controllers")[0].at("number") == 1);
	CHECK(std::abs(parts[0].at("controllers")[0].at("value").get<double>() - 0.5) < 0.01);

	// Playing it back, the instrument gets each note at the frame it was played at, counted from where playback
	// starts.
	const auto start = run(client, "audio.get").at("position").get<int64_t>();
	run(client, "midi.capture", {{"start", true}});
	run(client, "transport.locate", {{"position", 0}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 6000}});
	CHECK(notesPlayed(client) ==
		std::vector<std::vector<int64_t>>{
			{0x90, 60, start + 1000}, {0x90, 64, start + 2500}, {0x80, 60, start + 3000}, {0x80, 64, start + 5000}});
}

TEST_CASE("only record-enabled MIDI tracks record, and only what comes while the transport records", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const Json other = run(client, "track.add", {{"type", "midi"}});
	run(client, "channel.set", {{"channel", other.at("channel").at("id")}, {"output", setup.channel}});
	run(client, "midi.inject",
		{{"channel", other.at("channel").at("id")},
			{"messages", {message(1000, {0x90, 50, 100}), message(1500, {0x80, 50, 0})}}});
	setup.inject({message(200, {0x90, 40, 100}), message(300, {0x80, 40, 0})}); // before recording
	run(client, "audio.render", {{"frames", 640}});
	run(client, "transport.record");
	setup.inject({message(1000, {0x90, 62, 100}), message(1500, {0x80, 62, 0})});
	run(client, "audio.render", {{"frames", 2000}});
	REQUIRE(setup.stop().is_object());
	const std::vector<Json> parts = setup.parts();
	REQUIRE(parts.size() == 1);
	REQUIRE(parts[0].at("notes").size() == 1);
	CHECK(parts[0].at("notes")[0].at("pitch") == 62);
	const Json project = run(client, "project.get");
	for (const Json& part : project.at("midiParts"))
		CHECK(part.at("track") == setup.track);

	// Disarmed, the track records nothing.
	run(client, "channel.set", {{"channel", setup.channel}, {"recordEnable", false}});
	run(client, "transport.record");
	setup.inject({message(5000, {0x90, 62, 100}), message(5500, {0x80, 62, 0})});
	run(client, "audio.render", {{"frames", 4000}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	CHECK(setup.parts().size() == 1);
}

TEST_CASE("cycle recording mixes every lap into one part, notes held at a lap's end end there", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "midi.snapPartsToBars"}, {"value", false}});
	run(client, "transport.set", {{"left", 0}, {"right", 4 * g_beat}, {"cycle", true}});
	run(client, "transport.record");
	setup.inject(
		{message(g_beat, {0x90, 60, 100}), message(2 * g_beat, {0x80, 60, 0}), message(3 * g_beat, {0x90, 62, 100}),
			message((4 + 2) * g_beat, {0x90, 64, 100}), message((4 + 3) * g_beat, {0x80, 64, 0})});
	run(client, "audio.render", {{"frames", 8 * g_beat}});
	REQUIRE(setup.stop().is_object());
	const std::vector<Json> parts = setup.parts();
	REQUIRE(parts.size() == 1);
	CHECK(parts[0].at("position") == 0);
	CHECK(parts[0].at("length") == 4 * g_beatTicks);
	CHECK(notesOf(parts[0]) ==
		std::vector<std::vector<int64_t>>{
			{g_beatTicks, g_beatTicks, 60}, {2 * g_beatTicks, g_beatTicks, 64}, {3 * g_beatTicks, g_beatTicks, 62}});
}

TEST_CASE(
	"recording.add puts MIDI into new parts, merges it into a part or replaces what the part had", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const auto existing = [&client, &setup]
	{
		const ObjectId part =
			run(client, "midiPart.add", {{"track", setup.track}, {"position", 0}, {"length", 4 * g_beatTicks}})
				.at("part")
				.at("id")
				.get<ObjectId>();
		run(client, "note.add",
			{{"part", part}, {"notes", {note(0, g_beatTicks, 48), note(2 * g_beatTicks, g_beatTicks, 50)}}});
		return part;
	};
	const auto record = [&client, &setup](const std::string& mode)
	{
		return run(client, "recording.add",
			{{"midiMode", mode},
				{"tracks",
					{{{"track", setup.track}, {"takes", {{{"position", 2 * g_beat}, {"length", 4 * g_beat}}}},
						{"notes", {note(2 * g_beat, g_beat, 60), note(5 * g_beat, g_beat, 62)}}}}}});
	};

	SECTION("new parts")
	{
		existing();
		CHECK(record("newParts").at("parts").size() == 1);
		const std::vector<Json> parts = setup.parts();
		REQUIRE(parts.size() == 2);
		CHECK(parts[1].at("position") == 2 * g_beatTicks); // not snapped without snapToBars
		CHECK(parts[1].at("length") == 4 * g_beatTicks);
		CHECK(notesOf(parts[1]) ==
			std::vector<std::vector<int64_t>>{{0, g_beatTicks, 60}, {3 * g_beatTicks, g_beatTicks, 62}});
		CHECK(notesOf(parts[0]).size() == 2);
		run(client, "history.undo"); // one undo step
		CHECK(setup.parts().size() == 1);
	}
	SECTION("merge")
	{
		const ObjectId part = existing();
		CHECK(record("merge").at("parts") == Json::array({part}));
		const std::vector<Json> parts = setup.parts();
		REQUIRE(parts.size() == 1);
		CHECK(parts[0].at("position") == 0);
		CHECK(parts[0].at("length") == 6 * g_beatTicks); // grown to hold the recording
		CHECK(notesOf(parts[0]) ==
			std::vector<std::vector<int64_t>>{{0, g_beatTicks, 48}, {2 * g_beatTicks, g_beatTicks, 50},
				{2 * g_beatTicks, g_beatTicks, 60}, {5 * g_beatTicks, g_beatTicks, 62}});
	}
	SECTION("replace")
	{
		existing();
		record("replace");
		const std::vector<Json> parts = setup.parts();
		REQUIRE(parts.size() == 1);
		CHECK(notesOf(parts[0]) ==
			std::vector<std::vector<int64_t>>{
				{0, g_beatTicks, 48}, {2 * g_beatTicks, g_beatTicks, 60}, {5 * g_beatTicks, g_beatTicks, 62}});
	}
	SECTION("snapped to bars")
	{
		run(client, "recording.add",
			{{"snapToBars", true},
				{"tracks",
					{{{"track", setup.track}, {"takes", {{{"position", 5 * g_beat}, {"length", 4 * g_beat}}}},
						{"notes", {note(6 * g_beat, g_beat, 60)}}}}}});
		const std::vector<Json> parts = setup.parts();
		REQUIRE(parts.size() == 1);
		CHECK(parts[0].at("position") == 4 * g_beatTicks); // bar 2 to bar 4
		CHECK(parts[0].at("length") == 8 * g_beatTicks);
		CHECK(notesOf(parts[0]) == std::vector<std::vector<int64_t>>{{2 * g_beatTicks, g_beatTicks, 60}});
	}
}

TEST_CASE("recording.add checks what it gets for MIDI and audio tracks", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	CHECK_THAT(reject(client, "recording.add",
				   {{"tracks", {{{"track", setup.track}, {"path", "x.wav"}, {"takes", Json::array()}}}}}),
		ContainsSubstring("records MIDI"));
	CHECK_THAT(reject(client, "recording.add", {{"tracks", {{{"track", setup.track}, {"takes", Json::array()}}}}}),
		ContainsSubstring("needs a take"));
	const ObjectId audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "recording.add",
				   {{"tracks", {{{"track", audio}, {"takes", {{{"position", 0}, {"length", 10}}}}}}}}),
		ContainsSubstring("needs the path"));
	CHECK_THAT(reject(client, "recording.add", {{"midiMode", "overdub"}, {"tracks", Json::array()}}),
		ContainsSubstring("midiMode"));
}

TEST_CASE("auto quantize quantizes recorded notes", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "quantize.set", {{"autoQuantize", true}, {"settings", {{"grid", "1/4"}}}});
	run(client, "recording.add",
		{{"tracks",
			{{{"track", setup.track}, {"takes", {{{"position", 0}, {"length", 4 * g_beat}}}},
				{"notes", {note(g_beat + 300, g_beat, 60)}}}}}});
	const std::vector<Json> parts = setup.parts();
	REQUIRE(parts.size() == 1);
	REQUIRE(parts[0].at("notes").size() == 1);
	CHECK(parts[0].at("notes")[0].at("position") == g_beatTicks);
}

TEST_CASE("the record filter leaves MIDI out of recordings but lets it play", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "settings.set",
		{{"key", "midi.recordFilter"}, {"value", {{"types", {"controllers"}}, {"channels", {2}}}}});
	run(client, "midi.capture", {{"start", true}});
	run(client, "transport.record");
	setup.inject({message(1000, {0xB0, 7, 100}), message(1000, {0x90, 60, 100}), message(1000, {0x91, 61, 100}),
		message(1500, {0x80, 60, 0}), message(1500, {0x81, 61, 0}), message(1600, {0xE0, 0, 80})});
	run(client, "audio.render", {{"frames", 2000}});
	CHECK(run(client, "midi.capture").at("captured").size() >= 6); // everything played
	REQUIRE(setup.stop().is_object());
	const std::vector<Json> parts = setup.parts();
	REQUIRE(parts.size() == 1);
	REQUIRE(parts[0].at("notes").size() == 1);
	CHECK(parts[0].at("notes")[0].at("pitch") == 60);
	REQUIRE(parts[0].at("controllers").size() == 1);
	CHECK(parts[0].at("controllers")[0].at("type") == "pitchBend");
}

TEST_CASE("the catch range records input from just before recording started at its start", "[integration]")
{
	const bool caught = GENERATE(false, true);
	Setup setup(100);
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "midi.snapPartsToBars"}, {"value", false}});
	if (caught)
		run(client, "settings.set", {{"key", "midi.recordCatchRange"}, {"value", 50}}); // 2400 frames
	setup.inject({message(500, {0x90, 60, 100})});
	run(client, "audio.render", {{"frames", 1000}});
	run(client, "transport.record"); // from frame 1000, at position 0
	setup.inject({message(3000, {0x80, 60, 0}), message(3500, {0x90, 62, 100}), message(4000, {0x80, 62, 0})});
	run(client, "audio.render", {{"frames", 5000}});
	REQUIRE(setup.stop().is_object());
	const std::vector<Json> parts = setup.parts();
	REQUIRE(parts.size() == 1);
	const std::vector<std::vector<int64_t>> notes = notesOf(parts[0]);
	if (caught)
	{
		REQUIRE(notes.size() == 2);
		CHECK(notes[0][0] == 0);
		CHECK(notes[0][2] == 60);
	}
	else
	{
		REQUIRE(notes.size() == 1);
		CHECK(notes[0][2] == 62);
	}
}

TEST_CASE("retrospective recording inserts what a track's input played while it didn't record", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "midi.snapPartsToBars"}, {"value", false}});
	run(client, "channel.set", {{"channel", setup.channel}, {"recordEnable", false}});
	CHECK_THAT(reject(client, "midi.insertRetrospective", {{"track", setup.track}}), ContainsSubstring("nothing"));

	// In stop mode, at the cursor, with its timing.
	setup.inject({message(1000, {0x90, 60, 100}), message(1000 + g_beat, {0x80, 60, 0}),
		message(1000 + g_beat, {0x90, 62, 100}), message(1000 + (2 * g_beat), {0x80, 62, 0})});
	run(client, "audio.render", {{"frames", 1000 + (3 * g_beat)}});
	run(client, "transport.locate", {{"position", 2 * g_beat}});
	run(client, "audio.render", {{"frames", 64}});
	const Json added = run(client, "midi.insertRetrospective", {{"track", setup.track}});
	CHECK(added.at("parts").size() == 1);
	std::vector<Json> parts = setup.parts();
	REQUIRE(parts.size() == 1);
	CHECK(parts[0].at("position") == 2 * g_beatTicks);
	CHECK(notesOf(parts[0]) == std::vector<std::vector<int64_t>>{{0, g_beatTicks, 60}, {g_beatTicks, g_beatTicks, 62}});

	// New input after an insert starts the buffer afresh; during playback it goes where it played.
	const auto now = run(client, "audio.get").at("position").get<int64_t>();
	run(client, "transport.locate", {{"position", 8 * g_beat}});
	run(client, "transport.play");
	setup.inject({message(now + g_beat, {0x90, 64, 100}), message(now + (2 * g_beat), {0x80, 64, 0})});
	run(client, "audio.render", {{"frames", 3 * g_beat}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 64}});
	run(client, "midi.insertRetrospective", {{"track", setup.track}});
	parts = setup.parts();
	REQUIRE(parts.size() == 2);
	CHECK(parts[1].at("position") == 9 * g_beatTicks);
	CHECK(notesOf(parts[1]) == std::vector<std::vector<int64_t>>{{0, g_beatTicks, 64}});

	run(client, "midi.clearRetrospective", {{"track", setup.track}});
	CHECK_THAT(reject(client, "midi.insertRetrospective", {{"track", setup.track}}), ContainsSubstring("nothing"));
}

TEST_CASE("MIDI and instrument tracks play all MIDI inputs while they monitor or are record-enabled", "[integration]")
{
	auto dummy = std::make_unique<gnubase::midi::DummyBackend>();
	gnubase::midi::DummyBackend& backend = *dummy;
	backend.addLoopback("Keys");
	std::vector<std::unique_ptr<gnubase::midi::Backend>> midi;
	midi.push_back(std::move(dummy));
	Setup setup(64, std::move(midi));
	Client& client = setup.client;
	run(client, "midi.ports"); // opens the inputs
	const auto keys = backend.openOutput("Keys");
	constexpr gnubase::HostTime second = 1000000000;
	gnubase::audio::MidiMessage on;
	on.size = 3;
	on.bytes = {0x90, 60, 100};
	gnubase::audio::MidiMessage off = on;
	off.bytes = {0x80, 60, 0};
	const auto played = [&](const Json& changes)
	{
		Json parameters = changes;
		parameters["channel"] = setup.channel;
		run(client, "channel.set", parameters);
		run(client, "audio.render", {{"frames", 64}}); // the new graph
		run(client, "midi.capture", {{"start", true}});
		keys->send(on, gnubase::hostTime() - second); // the next block's first frame
		run(client, "audio.render", {{"frames", 64}});
		keys->send(off, gnubase::hostTime() - second);
		run(client, "audio.render", {{"frames", 64}});
		return notesPlayed(client).size();
	};
	CHECK(played({{"recordEnable", true}}) == 2);
	CHECK(played({{"recordEnable", false}}) == 0);
	CHECK(played({{"monitor", true}}) == 2);
	CHECK(played({{"midiInput", "none"}}) == 0);

	// What a record-enabled track receives there is recorded.
	played({{"midiInput", "all"}, {"monitor", false}, {"recordEnable", true}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 64}});
	keys->send(on, gnubase::hostTime() - second);
	run(client, "audio.render", {{"frames", 640}});
	keys->send(off, gnubase::hostTime() - second);
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(setup.stop().is_object());
	REQUIRE(setup.parts().size() == 1);
	CHECK(setup.parts()[0].at("notes").size() == 1);
}

TEST_CASE("MIDI and instrument tracks have record enable, monitor and a MIDI input", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json midi = run(client, "track.add", {{"type", "midi"}});
	const Json changed = run(client, "channel.set",
		{{"channel", midi.at("channel").at("id")}, {"recordEnable", true}, {"monitor", true}, {"midiInput", "none"}})
							 .at("channel");
	CHECK(changed.at("recordEnable") == true);
	CHECK(changed.at("monitor") == true);
	CHECK(changed.at("midiInput") == "none");
	CHECK(midi.at("channel").at("midiInput") == "all");
	const Json group = run(client, "track.add", {{"type", "group"}});
	CHECK_THAT(reject(client, "channel.set", {{"channel", group.at("channel").at("id")}, {"recordEnable", true}}),
		ContainsSubstring("only audio, MIDI and instrument tracks"));
	const Json audio = run(client, "track.add", {{"type", "audio"}});
	CHECK_THAT(reject(client, "channel.set", {{"channel", audio.at("channel").at("id")}, {"midiInput", "all"}}),
		ContainsSubstring("only MIDI and instrument tracks"));
}

TEST_CASE("the MIDI record mode is a transport setting", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK(run(client, "transport.get").at("midiRecordMode") == "newParts");
	run(client, "transport.set", {{"midiRecordMode", "merge"}});
	CHECK(run(client, "transport.get").at("midiRecordMode") == "merge");
	CHECK_THAT(reject(client, "transport.set", {{"midiRecordMode", "mix"}}), ContainsSubstring("midiRecordMode"));
}
