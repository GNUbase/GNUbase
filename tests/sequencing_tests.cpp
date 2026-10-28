// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <set>
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

	ObjectId idOf(const Json& result, const char* kind)
	{
		return result.at(kind).at("id").get<ObjectId>();
	}

	ObjectId channelOf(Client& client, ObjectId track)
	{
		const Json project = run(client, "project.get");
		ObjectId channel = 0;
		for (const Json& t : project.at("tracks"))
			if (t.at("id").get<ObjectId>() == track)
				channel = t.at("channel").get<ObjectId>();
		REQUIRE(channel != 0);
		return channel;
	}

	// Ticks for a number of samples at the default 120 BPM and 48 kHz: 1720320 ticks per 24000 samples, exact for
	// multiples of 25 samples.
	int64_t ticks(int64_t samples)
	{
		REQUIRE(samples % 25 == 0);
		return samples / 25 * 1792;
	}

	struct TestNote
	{
		int64_t start = 0; // project samples
		int64_t end = 0;
		int pitch = 60;
	};

	// An instrument track playing the note probe, with a MIDI track routed to it; each has a MIDI part from 0.
	class Song
	{
	public:
		explicit Song(int blockSize)
		{
			run(client, "audio.setDevice", {{"blockSize", blockSize}, {"inputChannels", 0}, {"captureSeconds", 1}});
			instrument = idOf(run(client, "track.add", {{"type", "instrument"}}), "track");
			midi = idOf(run(client, "track.add", {{"type", "midi"}}), "track");
			run(client, "channel.set", {{"channel", channelOf(client, instrument)}, {"instrument", "noteProbe"}});
			run(client, "channel.set",
				{{"channel", channelOf(client, midi)}, {"output", channelOf(client, instrument)}});
			for (const ObjectId track : {instrument, midi})
				m_parts.push_back(
					idOf(run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", ticks(48000)}}),
						"part"));
		}

		// Adds a note to the instrument track's part (0) or the MIDI track's (1).
		void add(int part, const TestNote& note)
		{
			run(client, "note.add",
				{{"part", m_parts[static_cast<size_t>(part)]},
					{"notes",
						{{{"position", ticks(note.start)}, {"length", ticks(note.end - note.start)},
							{"pitch", note.pitch}, {"velocity", 0.8}}}}});
			notes.push_back(note);
		}

		std::vector<float> capture(int channel, int64_t frames)
		{
			return run(client, "audio.capture", {{"channel", channel}, {"start", 0}, {"frames", frames}})
				.at("samples")
				.get<std::vector<float>>();
		}

		// Checks the probe's output against the notes that should sound at the positions played, -1 when stopped:
		// after a jump only notes that start from there on.
		void check(const std::vector<int64_t>& played)
		{
			const std::vector<float> count = capture(0, static_cast<int64_t>(played.size()));
			const std::vector<float> pitches = capture(1, static_cast<int64_t>(played.size()));
			std::set<const TestNote*> sounding;
			size_t wrong = 0;
			size_t first = 0;
			for (size_t i = 0; i < played.size(); ++i)
			{
				const int64_t position = played[i];
				if (position < 0 || i == 0 || played[i - 1] + 1 != position)
					sounding.clear();
				for (auto note = sounding.begin(); note != sounding.end();)
					note = (*note)->end <= position ? sounding.erase(note) : std::next(note);
				for (const TestNote& note : notes)
					if (note.start == position)
						sounding.insert(&note);
				float sum = 0.0f;
				for (const TestNote* note : sounding)
					sum += static_cast<float>(note->pitch);
				if ((count[i] != static_cast<float>(sounding.size()) || pitches[i] != sum) && wrong++ == 0)
					first = i;
			}
			INFO(wrong << " wrong frames, the first at frame " << first << " (position " << played[first] << ")");
			CHECK(wrong == 0);
		}

		Engine engine;
		Client client{engine};
		ObjectId instrument = 0;
		ObjectId midi = 0;
		std::vector<TestNote> notes;

	private:
		std::vector<ObjectId> m_parts;
	};

	std::vector<int64_t> positions(int64_t start, int64_t frames, int64_t left = 0, int64_t right = 0)
	{
		std::vector<int64_t> result;
		int64_t position = start;
		for (int64_t i = 0; i < frames; ++i)
		{
			result.push_back(position);
			const bool cycles = right > left && position < right;
			position = cycles && position + 1 == right ? left : position + 1;
		}
		return result;
	}
}

TEST_CASE("MIDI parts hold pitch bend, pressure, program changes and SysEx as controller events", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = idOf(run(client, "track.add", {{"type", "instrument"}}), "track");
	const ObjectId part =
		idOf(run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 960}}), "part");
	const Json added = run(client, "controller.add",
		{{"part", part},
			{"controllers",
				{{{"position", 1}, {"number", 7}, {"value", 1.0}},
					{{"position", 2}, {"type", "pitchBend"}, {"value", 0.5}, {"channel", 3}},
					{{"position", 3}, {"type", "channelPressure"}, {"value", 0.25}},
					{{"position", 4}, {"type", "polyPressure"}, {"number", 61}, {"value", 1.0}},
					{{"position", 5}, {"type", "program"}, {"number", 12}, {"bank", 300}},
					{{"position", 6}, {"type", "program"}, {"number", 13}},
					{{"position", 7}, {"type", "sysEx"}, {"data", {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}}}}}})
						   .at("controllers");
	REQUIRE(added.size() == 7);
	CHECK(added[0].at("type") == "controller");
	CHECK(added[1].at("type") == "pitchBend");
	CHECK(added[4].at("bank") == 300);
	CHECK(!added[5].contains("bank"));
	CHECK(added[6].at("data") == Json{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});

	const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
	const std::vector<RenderController>& controllers = snapshot.channels[0].controllers;
	REQUIRE(controllers.size() == 7);
	CHECK(controllers[1].type == ControllerType::PitchBend);
	CHECK(controllers[1].value == 0x80000000U); // the center: no bend
	CHECK(controllers[1].channel == 3);
	CHECK(controllers[3].type == ControllerType::PolyPressure);
	CHECK(controllers[3].number == 61);
	CHECK(controllers[4].type == ControllerType::Program);
	CHECK(controllers[4].value == 300);
	CHECK(controllers[5].value == g_noBank);
	CHECK(controllers[6].data == std::vector<uint8_t>{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});

	// The project survives undo and redo with them.
	const Json before = run(client, "project.get");
	run(client, "history.undo");
	run(client, "history.redo");
	CHECK(run(client, "project.get") == before);

	const auto bad = [&](const Json& controller)
	{ return reject(client, "controller.add", {{"part", part}, {"controllers", {controller}}}); };
	CHECK_THAT(bad({{"position", 0}, {"type", "aftertouch"}, {"value", 0.5}}), ContainsSubstring("type"));
	CHECK_THAT(bad({{"position", 0}, {"type", "program"}, {"number", 1}, {"bank", 16384}}), ContainsSubstring("bank"));
	CHECK_THAT(bad({{"position", 0}, {"type", "program"}}), ContainsSubstring("number"));
	CHECK_THAT(bad({{"position", 0}, {"type", "pitchBend"}}), ContainsSubstring("value"));
	CHECK_THAT(bad({{"position", 0}, {"type", "sysEx"}, {"data", {0xF0, 0x01}}}), ContainsSubstring("0xF7"));
	CHECK_THAT(bad({{"position", 0}, {"type", "sysEx"}, {"data", {0xF0, 0x80, 0xF7}}}), ContainsSubstring("data"));
}

TEST_CASE("instrument channels play a built-in instrument", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId instrument = idOf(run(client, "track.add", {{"type", "instrument"}}), "track");
	const ObjectId audio = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
	const Json changed =
		run(client, "channel.set", {{"channel", channelOf(client, instrument)}, {"instrument", "testSynth"}});
	CHECK(changed.at("channel").at("instrument") == "testSynth");
	CHECK(engine.renderSnapshots().acquire().channels[0].instrument == "testSynth");
	run(client, "channel.set", {{"channel", channelOf(client, instrument)}, {"instrument", ""}});
	CHECK(engine.renderSnapshots().acquire().channels[0].instrument.empty());

	CHECK_THAT(reject(client, "channel.set", {{"channel", channelOf(client, instrument)}, {"instrument", "sine"}}),
		ContainsSubstring("not an instrument"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", channelOf(client, instrument)}, {"instrument", "piano"}}),
		ContainsSubstring("no built-in processor piano"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", channelOf(client, audio)}, {"instrument", "testSynth"}}),
		ContainsSubstring("instrument track"));
}

TEST_CASE("notes reach the instrument at their exact sample, from instrument and MIDI tracks, for any block size",
	"[integration]")
{
	const int blockSize = GENERATE(1, 64, 100, 333);
	INFO("block size " << blockSize);
	Song song(blockSize);
	song.add(0, {100, 400, 60});
	song.add(0, {400, 425, 62}); // starts where the one before ends
	song.add(1, {250, 1000, 67});
	song.add(1, {975, 1200, 48});
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 1500}});
	song.check(positions(0, 1500));
}

TEST_CASE("stopping and locating end the sounding notes at once", "[integration]")
{
	Song song(64);
	song.add(0, {0, 20000, 60});
	song.add(1, {300, 20000, 64});
	song.add(1, {5000, 5100, 67});
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 500}});
	run(song.client, "transport.stop");
	run(song.client, "audio.render", {{"frames", 200}});
	run(song.client, "transport.locate", {{"position", 4900}});
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 300}});
	run(song.client, "transport.locate", {{"position", 100}}); // while playing
	run(song.client, "audio.render", {{"frames", 300}});

	std::vector<int64_t> played = positions(0, 500);
	played.insert(played.end(), 200, -1);
	for (const int64_t position : positions(4900, 300))
		played.push_back(position);
	for (const int64_t position : positions(100, 300))
		played.push_back(position);
	song.check(played);
}

TEST_CASE("cycle playback ends notes at the right locator and plays the notes from the left one again", "[integration]")
{
	const int blockSize = GENERATE(1, 64, 100, 1024);
	INFO("block size " << blockSize);
	Song song(blockSize);
	song.add(0, {900, 1200, 60});  // across the left locator
	song.add(0, {1000, 1100, 62}); // at the left locator
	song.add(1, {1500, 2000, 64}); // across the right locator
	run(song.client, "transport.set", {{"left", 1000}, {"right", 1600}, {"cycle", true}});
	run(song.client, "transport.locate", {{"position", 800}});
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 3000}});
	song.check(positions(800, 3000, 1000, 1600));
}

TEST_CASE("a MIDI sequence rendered through the test synth matches its golden file", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 128}, {"inputChannels", 0}, {"captureSeconds", 1}});
	const ObjectId track = idOf(run(client, "track.add", {{"type", "instrument"}}), "track");
	run(client, "channel.set", {{"channel", channelOf(client, track)}, {"instrument", "testSynth"}});
	const ObjectId part =
		idOf(run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", ticks(48000)}}), "part");
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", ticks(0)}, {"length", ticks(6000)}, {"pitch", 60}, {"velocity", 1.0}},
					{{"position", ticks(3000)}, {"length", ticks(9000)}, {"pitch", 64}, {"velocity", 0.5}},
					{{"position", ticks(9000)}, {"length", ticks(9000)}, {"pitch", 67}, {"velocity", 0.75}}}}});
	run(client, "transport.set", {{"left", 0}, {"right", 15000}, {"cycle", true}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 24000}});

	gnubase::test::Audio audio;
	audio.channels = 2;
	audio.sampleRate = 48000;
	const std::vector<float> left = run(client, "audio.capture", {{"channel", 0}, {"start", 0}, {"frames", 24000}})
										.at("samples")
										.get<std::vector<float>>();
	const std::vector<float> right = run(client, "audio.capture", {{"channel", 1}, {"start", 0}, {"frames", 24000}})
										 .at("samples")
										 .get<std::vector<float>>();
	for (size_t i = 0; i < left.size(); ++i)
	{
		audio.samples.push_back(left[i]);
		audio.samples.push_back(right[i]);
	}
	const gnubase::test::AudioDiff diff =
		gnubase::test::compareWithGolden("midi-sequencing", audio, 1e-5f); // sin() differs by platform
	INFO(diff.report);
	CHECK(diff.equal);
}
