// SPDX-License-Identifier: GPL-3.0-or-later
// midi.inject plays MIDI into tracks as if from their MIDI input; midi.capture returns what instruments played.
#include "engine/document/document.h"
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
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
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId channelOf(const Json& added)
	{
		return added.at("channel").at("id").get<ObjectId>();
	}

	Json message(int64_t position, std::vector<int> bytes)
	{
		return {{"position", position}, {"bytes", std::move(bytes)}};
	}

	// An instrument track playing the note probe (channel 0: how many notes sound, channel 1: the sum of their
	// pitches), and a MIDI track routed to it.
	struct Probe
	{
		explicit Probe(int blockSize)
		{
			run(client, "audio.setDevice", {{"blockSize", blockSize}, {"inputChannels", 0}, {"captureSeconds", 1}});
			instrument = channelOf(run(client, "track.add", {{"type", "instrument"}}));
			const Json added = run(client, "track.add", {{"type", "midi"}});
			midi = channelOf(added);
			midiTrack = added.at("track").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", instrument}, {"instrument", "noteProbe"}});
			run(client, "channel.set", {{"channel", midi}, {"output", instrument}});
		}

		// A note from frame start to frame end, played into a channel.
		void note(ObjectId channel, int64_t start, int64_t end, int pitch)
		{
			run(client, "midi.inject",
				{{"channel", channel},
					{"messages", {message(start, {0x90, pitch, 100}), message(end, {0x80, pitch, 0})}}});
		}

		std::vector<float> capture(int channel, int64_t start, int64_t frames)
		{
			return run(client, "audio.capture", {{"channel", channel}, {"start", start}, {"frames", frames}})
				.at("samples")
				.get<std::vector<float>>();
		}

		Engine engine;
		Client client{engine};
		ObjectId instrument = 0;
		ObjectId midi = 0;
		ObjectId midiTrack = 0;
	};

	// The frames of a capture where a probe channel doesn't have the expected value, which is inside [from, to) and
	// outside.
	int64_t wrongFrames(const std::vector<float>& samples, int64_t from, int64_t to, float inside, float outside = 0.0f)
	{
		int64_t wrong = 0;
		for (size_t i = 0; i < samples.size(); ++i)
		{
			const auto frame = static_cast<int64_t>(i);
			wrong += samples[i] != (frame >= from && frame < to ? inside : outside) ? 1 : 0;
		}
		return wrong;
	}
}

TEST_CASE("MIDI injected into instrument and MIDI tracks reaches the instrument at its exact frame", "[unit]")
{
	const int blockSize = GENERATE(1, 64, 100, 333);
	INFO("block size " << blockSize);
	Probe probe(blockSize);
	probe.note(probe.instrument, 100, 400, 60);
	probe.note(probe.midi, 250, 1000, 67);
	run(probe.client, "audio.render", {{"frames", 1500}}); // the transport stands: tracks play their input anyway

	const std::vector<float> count = probe.capture(0, 0, 1500);
	const std::vector<float> pitches = probe.capture(1, 0, 1500);
	for (size_t i = 0; i < count.size(); ++i)
	{
		const auto frame = static_cast<int64_t>(i);
		const bool first = frame >= 100 && frame < 400;
		const bool second = frame >= 250 && frame < 1000;
		INFO("frame " << frame);
		REQUIRE(count[i] == static_cast<float>((first ? 1 : 0) + (second ? 1 : 0)));
		REQUIRE(pitches[i] == static_cast<float>((first ? 60 : 0) + (second ? 67 : 0)));
	}
}

TEST_CASE("injected MIDI plays along with the parts while the transport plays", "[unit]")
{
	Probe probe(64);
	const ObjectId track = run(probe.client, "project.get").at("tracks").at(0).at("id").get<ObjectId>();
	const Json part = run(probe.client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 1720320}});
	run(probe.client, "note.add", // frames 0 to 24000
		{{"part", part.at("part").at("id")},
			{"notes", {{{"position", 0}, {"length", 1720320}, {"pitch", 48}, {"velocity", 1.0}}}}});
	probe.note(probe.midi, 500, 700, 72);
	run(probe.client, "transport.play");
	run(probe.client, "audio.render", {{"frames", 1000}});
	CHECK(wrongFrames(probe.capture(0, 0, 1000), 500, 700, 2.0f, 1.0f) == 0);
	CHECK(wrongFrames(probe.capture(1, 0, 1000), 500, 700, 120.0f, 48.0f) == 0);
}

TEST_CASE("MIDI injected for frames already processed plays at the start of the next block", "[unit]")
{
	Probe probe(64);
	run(probe.client, "audio.render", {{"frames", 128}});
	probe.note(probe.instrument, 10, 200, 60); // the device is at 128 already
	run(probe.client, "audio.render", {{"frames", 128}});
	CHECK(wrongFrames(probe.capture(0, 0, 256), 128, 200, 1.0f) == 0);

	// Messages wait for their block, in the order they came for one frame.
	run(probe.client, "midi.inject",
		{{"channel", probe.instrument},
			{"messages", {message(300, {0x90, 62, 100}), message(300, {0x80, 62, 0}), message(310, {0x80, 64, 0})}}});
	run(probe.client, "midi.inject", {{"channel", probe.instrument}, {"messages", {message(290, {0x90, 64, 1})}}});
	run(probe.client, "audio.render", {{"frames", 128}});
	CHECK(wrongFrames(probe.capture(0, 256, 128), 34, 54, 1.0f) == 0); // 64 from 290 to 310; 62 doesn't sound
}

TEST_CASE("midi.capture returns what instruments played, at frame positions on the audio device", "[unit]")
{
	Probe probe(64);
	CHECK(run(probe.client, "midi.capture") == Json{{"captured", Json::array()}, {"dropped", 0}});

	// A program change with a bank from the instrument track's part, at frame 0.
	const ObjectId track = run(probe.client, "project.get").at("tracks").at(0).at("id").get<ObjectId>();
	const Json part = run(probe.client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 1720320}});
	run(probe.client, "controller.add",
		{{"part", part.at("part").at("id")},
			{"controllers", {{{"position", 0}, {"type", "program"}, {"number", 3}, {"bank", 130}, {"channel", 2}}}}});

	// Every kind of MIDI 1.0 channel message, injected into the MIDI track; a note-on of velocity 0 is a note-off.
	run(probe.client, "midi.inject",
		{{"channel", probe.midi},
			{"messages",
				{message(20, {0xB1, 7, 100}), message(30, {0xE0, 0x00, 0x40}), message(40, {0xD3, 90}),
					message(50, {0x90, 62, 100}), message(60, {0xA0, 62, 80}), message(70, {0xC4, 9}),
					message(300, {0x90, 62, 0})}}});
	run(probe.client, "transport.play");
	run(probe.client, "audio.render", {{"frames", 320}});

	const auto entry = [&probe](int64_t position, std::vector<int> bytes)
	{ return Json{{"channel", probe.instrument}, {"position", position}, {"bytes", std::move(bytes)}}; };
	const Json captured = run(probe.client, "midi.capture");
	CHECK(captured.at("captured") ==
		Json::array({entry(0, {0xB2, 0, 1}), entry(0, {0xB2, 32, 2}), entry(0, {0xC2, 3}), entry(20, {0xB1, 7, 100}),
			entry(30, {0xE0, 0x00, 0x40}), entry(40, {0xD3, 90}), entry(50, {0x90, 62, 100}), entry(60, {0xA0, 62, 80}),
			entry(70, {0xC4, 9}), entry(300, {0x80, 62, 64})}));
	CHECK(captured.at("dropped") == 0);
	CHECK(run(probe.client, "midi.capture").at("captured").empty()); // each message once
}

TEST_CASE("midi.inject takes MIDI 1.0 channel messages for MIDI and instrument tracks", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId instrument = channelOf(run(client, "track.add", {{"type", "instrument"}}));
	const ObjectId audio = channelOf(run(client, "track.add", {{"type", "audio"}}));
	const auto inject = [&client](ObjectId channel, const Json& one)
	{ return reject(client, "midi.inject", {{"channel", channel}, {"messages", Json::array({one})}}); };
	CHECK_THAT(inject(instrument, message(0, {0x90, 60, 100})), ContainsSubstring("no audio device"));

	run(client, "audio.setDevice", {{"inputChannels", 0}});
	CHECK_THAT(inject(audio, message(0, {0x90, 60, 100})), ContainsSubstring("not a MIDI or instrument track"));
	CHECK_THAT(inject(12345, message(0, {0x90, 60, 100})), ContainsSubstring("not a MIDI or instrument track"));
	CHECK_THAT(inject(instrument, message(0, {0xF8})), ContainsSubstring("channel message"));
	CHECK_THAT(inject(instrument, message(0, {0x3C, 100})), ContainsSubstring("channel message"));
	CHECK_THAT(inject(instrument, message(0, {0x90, 60})), ContainsSubstring("3 bytes"));
	CHECK_THAT(inject(instrument, message(0, {0xC0, 1, 2})), ContainsSubstring("2 bytes"));
	CHECK_THAT(inject(instrument, message(0, {0x90, 200, 100})), ContainsSubstring("data bytes"));
	CHECK_THAT(inject(instrument, message(-1, {0x90, 60, 100})), ContainsSubstring("position"));
	CHECK(run(client, "midi.inject", {{"channel", instrument}, {"messages", Json::array()}}) == Json::object());
}

TEST_CASE("midi.inject injects all messages or none when too many wait for the audio device", "[unit]")
{
	Probe probe(64);
	Json waiting = Json::array(); // the device processes no block meanwhile
	for (int i = 0; i < 4095; ++i)
		waiting.push_back(message(1000000, {0xB0, 7, 100}));
	run(probe.client, "midi.inject", {{"channel", probe.instrument}, {"messages", waiting}});

	// One more fits, two don't: the call fails and injects neither, so a client can retry without playing any twice.
	CHECK_THAT(
		reject(probe.client, "midi.inject",
			{{"channel", probe.instrument}, {"messages", {message(0, {0x90, 60, 100}), message(64, {0x80, 60, 0})}}}),
		ContainsSubstring("none were injected"));
	run(probe.client, "audio.render", {{"frames", 128}});
	CHECK(wrongFrames(probe.capture(0, 0, 128), 0, 0, 0.0f) == 0);
	CHECK(run(probe.client, "midi.capture").at("captured").empty());
}

TEST_CASE("midi.capture with start returns what was played since, after a long stretch without reads", "[unit]")
{
	Probe probe(64);
	for (int round = 0; round < 3; ++round) // 9000 messages that nobody reads
	{
		Json messages = Json::array();
		for (int i = 0; i < 3000; ++i)
			messages.push_back(message((round * 3000) + i, {0xB0, 7, i % 128}));
		run(probe.client, "midi.inject", {{"channel", probe.instrument}, {"messages", messages}});
		run(probe.client, "audio.render", {{"frames", 3000}});
	}

	// start drops what was captured so far; the next call returns only what was played after it.
	CHECK(run(probe.client, "midi.capture", {{"start", true}}) == Json{{"captured", Json::array()}, {"dropped", 0}});
	probe.note(probe.instrument, 9100, 9200, 60);
	run(probe.client, "audio.render", {{"frames", 300}});
	CHECK(run(probe.client, "midi.capture", {{"start", false}}) ==
		Json{{"captured",
				 {{{"channel", probe.instrument}, {"position", 9100}, {"bytes", {0x90, 60, 100}}},
					 {{"channel", probe.instrument}, {"position", 9200}, {"bytes", {0x80, 60, 0}}}}},
			{"dropped", 0}});
}

TEST_CASE(
	"injected notes end when their MIDI track is rerouted or removed, except those another source holds", "[unit]")
{
	const bool playing = GENERATE(false, true);
	const bool removed = GENERATE(false, true);
	INFO((playing ? "playing, " : "stopped, ") << (removed ? "removed" : "rerouted"));
	Probe probe(64);
	const ObjectId other = channelOf(run(probe.client, "track.add", {{"type", "midi"}}));
	run(probe.client, "channel.set", {{"channel", other}, {"output", probe.instrument}});
	const auto noteOn = [&probe](ObjectId channel, int pitch)
	{ run(probe.client, "midi.inject", {{"channel", channel}, {"messages", {message(10, {0x90, pitch, 100})}}}); };
	noteOn(probe.instrument, 60); // the instrument track's own input
	noteOn(probe.midi, 64);       // both MIDI tracks hold 64
	noteOn(probe.midi, 67);
	noteOn(other, 64);
	if (playing)
		run(probe.client, "transport.play");
	run(probe.client, "audio.render", {{"frames", 500}});
	if (removed)
		run(probe.client, "track.remove", {{"track", probe.midiTrack}});
	else
		run(probe.client, "channel.set", {{"channel", probe.midi}, {"output", 0}});
	run(probe.client, "audio.render", {{"frames", 500}});

	// 67 ends at the first block of the new graph; 64 stays, as the other MIDI track holds it.
	CHECK(wrongFrames(probe.capture(0, 0, 500), 10, 500, 3.0f) == 0);
	CHECK(wrongFrames(probe.capture(1, 0, 500), 10, 500, 60.0f + 64 + 67) == 0);
	CHECK(wrongFrames(probe.capture(0, 500, 500), 0, 500, 2.0f) == 0);
	CHECK(wrongFrames(probe.capture(1, 500, 500), 0, 500, 60.0f + 64) == 0);
}

TEST_CASE("after a stop ended an injected note, the MIDI track that held it no longer keeps it sounding", "[unit]")
{
	Probe probe(64);
	const ObjectId other = channelOf(run(probe.client, "track.add", {{"type", "midi"}}));
	run(probe.client, "channel.set", {{"channel", other}, {"output", probe.instrument}});
	run(probe.client, "midi.inject", {{"channel", other}, {"messages", {message(0, {0x90, 64, 100})}}});
	run(probe.client, "transport.play");
	run(probe.client, "audio.render", {{"frames", 128}});
	run(probe.client, "transport.stop"); // the instrument ends 64
	run(probe.client, "audio.render", {{"frames", 64}});
	run(probe.client, "midi.inject", {{"channel", probe.midi}, {"messages", {message(192, {0x90, 64, 100})}}});
	run(probe.client, "audio.render", {{"frames", 64}});
	run(probe.client, "channel.set", {{"channel", probe.midi}, {"output", 0}}); // the other one doesn't hold 64
	run(probe.client, "audio.render", {{"frames", 64}});
	CHECK(wrongFrames(probe.capture(0, 0, 192), 0, 128, 1.0f) == 0);
	CHECK(wrongFrames(probe.capture(0, 192, 128), 0, 64, 1.0f) == 0);
}

TEST_CASE("an instrument track's held input doesn't keep a note sounding on the instrument that replaced it", "[unit]")
{
	// Replaced twice in one block, the audio thread skips the graph between, and the old instrument channel is freed
	// before the new one is made, so the new one can get its address.
	const bool skipped = GENERATE(false, true);
	INFO((skipped ? "replaced twice in one block" : "a block between the replacements"));
	Probe probe(64);
	const auto noteOn = [&probe](ObjectId channel, int64_t position)
	{ run(probe.client, "midi.inject", {{"channel", channel}, {"messages", {message(position, {0x90, 64, 100})}}}); };
	noteOn(probe.instrument, 10); // the transport stands, so nothing else ends what the input holds
	run(probe.client, "audio.render", {{"frames", 128}});
	run(probe.client, "channel.set", {{"channel", probe.instrument}, {"instrument", ""}});
	if (!skipped)
		run(probe.client, "audio.render", {{"frames", 64}});
	run(probe.client, "channel.set", {{"channel", probe.instrument}, {"instrument", "noteProbe"}});
	run(probe.client, "audio.render", {{"frames", skipped ? 128 : 64}});
	noteOn(probe.midi, 256);
	run(probe.client, "audio.render", {{"frames", 64}});
	run(probe.client, "track.remove", {{"track", probe.midiTrack}});
	run(probe.client, "audio.render", {{"frames", 64}});

	// The new instrument plays the MIDI track's 64 alone; it ends at the first block of the graph without that track.
	CHECK(wrongFrames(probe.capture(0, 256, 128), 0, 64, 1.0f) == 0);
	CHECK(wrongFrames(probe.capture(1, 256, 128), 0, 64, 64.0f) == 0);
}

TEST_CASE("a MIDI track's held input doesn't keep a note sounding on an instrument it is routed to later", "[unit]")
{
	const bool removed = GENERATE(false, true);
	INFO((removed ? "its instrument track removed" : "routed nowhere"));
	Probe probe(64);
	const ObjectId first = run(probe.client, "project.get").at("tracks").at(0).at("id").get<ObjectId>();
	const ObjectId second = channelOf(run(probe.client, "track.add", {{"type", "instrument"}}));
	run(probe.client, "channel.set", {{"channel", second}, {"instrument", "noteProbe"}});
	const Json added = run(probe.client, "track.add", {{"type", "midi"}});
	const ObjectId other = channelOf(added);
	run(probe.client, "channel.set", {{"channel", other}, {"output", second}});
	if (!removed)
		run(probe.client, "channel.set", {{"channel", probe.midi}, {"output", 0}});
	const auto noteOn = [&probe](ObjectId channel, int64_t position)
	{ run(probe.client, "midi.inject", {{"channel", channel}, {"messages", {message(position, {0x90, 64, 100})}}}); };
	noteOn(probe.midi, 10); // on the first instrument, or nowhere
	run(probe.client, "audio.render", {{"frames", 128}});
	if (removed)
		run(probe.client, "track.remove", {{"track", first}});
	run(probe.client, "audio.render", {{"frames", 64}});
	run(probe.client, "channel.set", {{"channel", probe.midi}, {"output", second}});
	run(probe.client, "audio.render", {{"frames", 64}});
	noteOn(other, 256);
	run(probe.client, "audio.render", {{"frames", 64}});
	run(probe.client, "track.remove", {{"track", added.at("track").at("id").get<ObjectId>()}});
	run(probe.client, "audio.render", {{"frames", 64}});

	// The second instrument plays the other track's 64 alone (the first one is gone or never got a note); it ends at
	// the first block of the graph without that track.
	CHECK(wrongFrames(probe.capture(0, 256, 128), 0, 64, 1.0f) == 0);
	CHECK(wrongFrames(probe.capture(1, 256, 128), 0, 64, 64.0f) == 0);
}

TEST_CASE(
	"a MIDI track routed to an instrument in the same block another track leaves it doesn't keep its note sounding",
	"[unit]")
{
	const bool removed = GENERATE(false, true);
	INFO((removed ? "its instrument track removed" : "routed nowhere"));
	Probe probe(64);
	const ObjectId first = run(probe.client, "project.get").at("tracks").at(0).at("id").get<ObjectId>();
	const ObjectId second = channelOf(run(probe.client, "track.add", {{"type", "instrument"}}));
	run(probe.client, "channel.set", {{"channel", second}, {"instrument", "noteProbe"}});
	const Json added = run(probe.client, "track.add", {{"type", "midi"}});
	const ObjectId other = channelOf(added);
	run(probe.client, "channel.set", {{"channel", other}, {"output", second}});
	if (!removed)
		run(probe.client, "channel.set", {{"channel", probe.midi}, {"output", 0}});
	const auto noteOn = [&probe](ObjectId channel, int64_t position)
	{ run(probe.client, "midi.inject", {{"channel", channel}, {"messages", {message(position, {0x90, 64, 100})}}}); };
	noteOn(probe.midi, 10); // on the first instrument, or nowhere
	noteOn(other, 20);      // on the second instrument
	run(probe.client, "audio.render", {{"frames", 128}});
	// No block between these commands, as with scripts and MCP clients: the audio thread skips the graphs between.
	if (removed)
		run(probe.client, "track.remove", {{"track", first}});
	run(probe.client, "channel.set", {{"channel", probe.midi}, {"output", second}});
	run(probe.client, "track.remove", {{"track", added.at("track").at("id").get<ObjectId>()}});
	run(probe.client, "audio.render", {{"frames", 128}});

	// The other track's 64 ends at the first block of the graph without it: the MIDI track's 64, held on the first
	// instrument or nowhere, doesn't keep it sounding on the second.
	CHECK(wrongFrames(probe.capture(0, 20, 108), 0, 108, removed ? 2.0f : 1.0f) == 0);
	CHECK(wrongFrames(probe.capture(0, 128, 128), 0, 0, 0.0f) == 0);
}
