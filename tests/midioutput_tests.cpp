// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI output (#2173, reference manual p. 349): a MIDI track routed to a MIDI output port plays its events there, at
// the host times of their frames, through a queue the audio thread fills and a thread of its own empties.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "platform/audio/device.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	constexpr int64_t g_quarter = gnubase::time::g_ticksPerQuarter; // 24000 frames at 120 BPM and 48 kHz

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

	// A note message as it arrived at the test port: its bytes and its host time.
	struct Arrived
	{
		std::vector<int> bytes;
		gnubase::HostTime time = 0;
		bool operator==(const Arrived& other) const { return bytes == other.bytes && time == other.time; }
	};

	// An engine with a dummy MIDI loopback "Synth", on the dummy device in blocks of 4096 frames, and the notes that
	// arrive at the loopback's input.
	struct Setup
	{
		Setup() : Setup(synth()) {}

		explicit Setup(std::unique_ptr<gnubase::midi::DummyBackend> dummy)
			: backend(*dummy), engine(gnubase::audio::makeBackends(), {}, backends(std::move(dummy)))
		{
			run(client, "audio.setDevice", {{"blockSize", 4096}, {"inputChannels", 0}});
			input = backend.openInput("Synth",
				[this](const gnubase::audio::MidiMessage& message, gnubase::HostTime time)
				{
					const auto status = message.bytes[0] & 0xF0;
					if (status != 0x80 && status != 0x90)
						return;
					const std::scoped_lock lock(mutex);
					arrived.push_back({{message.bytes[0], message.bytes[1]}, time});
				});
		}

		static std::unique_ptr<gnubase::midi::DummyBackend> synth()
		{
			auto dummy = std::make_unique<gnubase::midi::DummyBackend>();
			dummy->addLoopback("Synth");
			return dummy;
		}

		static std::vector<std::unique_ptr<gnubase::midi::Backend>> backends(
			std::unique_ptr<gnubase::midi::DummyBackend> dummy)
		{
			std::vector<std::unique_ptr<gnubase::midi::Backend>> result;
			result.push_back(std::move(dummy));
			return result;
		}

		// A MIDI track with a part of notes ({position in ticks, length in ticks, pitch}), routed to the port.
		ObjectId midiTrack(const std::vector<std::vector<int64_t>>& notes, const Json& routing)
		{
			const Json added = run(client, "track.add", {{"type", "midi"}});
			const auto track = added.at("track").at("id").get<ObjectId>();
			const auto channel = added.at("channel").at("id").get<ObjectId>();
			const auto part =
				run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * g_quarter}})
					.at("part")
					.at("id")
					.get<ObjectId>();
			Json list = Json::array();
			for (const std::vector<int64_t>& note : notes)
				list.push_back({{"position", note[0]}, {"length", note[1]}, {"pitch", note[2]}, {"velocity", 0.5}});
			run(client, "note.add", {{"part", part}, {"notes", std::move(list)}});
			Json parameters = routing;
			parameters["channel"] = channel;
			run(client, "channel.set", parameters);
			return channel;
		}

		// Plays the first block and waits until count notes arrived, at most 10 s; their times are relative to the
		// first's.
		std::vector<Arrived> play(size_t count)
		{
			run(client, "audio.render", {{"frames", 64}}); // the new graph
			run(client, "transport.play");
			run(client, "audio.render", {{"frames", 4096}});
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			for (;;)
			{
				{
					const std::scoped_lock lock(mutex);
					if (arrived.size() >= count || std::chrono::steady_clock::now() > deadline)
					{
						std::vector<Arrived> result = arrived;
						for (Arrived& note : result)
							note.time -= arrived.front().time;
						return result;
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		gnubase::midi::DummyBackend& backend;
		Engine engine;
		Client client{engine};
		std::mutex mutex;
		std::vector<Arrived> arrived;
		std::unique_ptr<gnubase::midi::Input> input; // closes before the engine's backend goes
	};

	const Json g_synth = {{"midiPort", "dummy/out/Synth"}};
}

TEST_CASE("a MIDI track routed to a MIDI output port plays its notes there at their frames' times", "[integration]")
{
	Setup setup;
	Json routing = g_synth;
	routing["midiChannel"] = 2; // the notes follow the track's MIDI channel
	// Frames 0 to 1500 and 750 to 1500: at 48 kHz 15.625 ms apart.
	setup.midiTrack({{0, g_quarter / 16, 60}, {g_quarter / 32, g_quarter / 32, 64}}, routing);
	const std::vector<Arrived> arrived = setup.play(4);
	const std::vector<Arrived> expected = {
		{{0x92, 60}, 0}, {{0x92, 64}, 15625000}, {{0x82, 60}, 31250000}, {{0x82, 64}, 31250000}};
	CHECK(arrived == expected);
}

TEST_CASE("MIDI output reaches the port at the same times for any number of processing threads", "[integration]")
{
	const auto played = [](int threads)
	{
		Setup setup;
		run(setup.client, "audio.setProcessing", {{"multiProcessing", threads > 1}, {"threads", threads}});
		setup.midiTrack({{0, g_quarter / 16, 60}, {g_quarter / 8, g_quarter / 32, 62}}, g_synth);
		setup.midiTrack({{g_quarter / 32, g_quarter / 32, 67}, {g_quarter / 8, g_quarter / 32, 69}}, g_synth);
		return setup.play(8);
	};
	const std::vector<Arrived> alone = played(1);
	CHECK(alone.size() == 8);
	CHECK(played(4) == alone);
}

TEST_CASE("a MIDI channel plays to an instrument or to a MIDI output port", "[unit]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId channel = setup.midiTrack({}, g_synth);
	const auto get = [&]
	{
		const Json project = run(client, "project.get");
		for (const Json& found : project.at("channels"))
			if (found.at("id") == channel)
				return found;
		return Json::object();
	};
	CHECK(get().at("midiPort") == "dummy/out/Synth");
	const auto synth = run(client, "track.add", {{"type", "instrument"}}).at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"output", synth}});
	CHECK(get().at("midiPort") == ""); // an instrument replaces the port
	run(client, "channel.set", {{"channel", channel}, {"midiPort", "dummy/out/Synth"}});
	CHECK(get().at("output") == 0); // and the port the instrument
	CHECK_THAT(
		reject(client, "channel.set", {{"channel", channel}, {"output", synth}, {"midiPort", "dummy/out/Synth"}}),
		ContainsSubstring("not both"));
	const auto audio = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "channel.set", {{"channel", audio}, {"midiPort", "dummy/out/Synth"}}),
		ContainsSubstring("only MIDI channels"));
}
