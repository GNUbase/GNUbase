// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI output (#2173, reference manual p. 349): a MIDI track routed to a MIDI output port plays its events there, at
// the host times of their frames, through a queue the audio thread fills and a thread of its own empties.
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "platform/audio/device.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"
#include "testutils/testutils.h"

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

		// Waits until count notes arrived, at most 10 s.
		std::vector<Arrived> wait(size_t count)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			for (;;)
			{
				{
					const std::scoped_lock lock(mutex);
					if (arrived.size() >= count || std::chrono::steady_clock::now() > deadline)
						return arrived;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		// Plays the first block and waits until count notes arrived; their times are relative to the first's.
		std::vector<Arrived> play(size_t count)
		{
			run(client, "audio.render", {{"frames", 64}}); // the new graph
			run(client, "transport.play");
			run(client, "audio.render", {{"frames", 4096}});
			std::vector<Arrived> result = wait(count);
			const gnubase::HostTime first = result.empty() ? 0 : result.front().time;
			for (Arrived& note : result)
				note.time -= first;
			return result;
		}

		// Renders a block; the host times before and after it.
		std::pair<gnubase::HostTime, gnubase::HostTime> render()
		{
			const gnubase::HostTime before = gnubase::hostTime();
			run(client, "audio.render", {{"frames", 4096}});
			return {before, gnubase::hostTime()};
		}

		Json channel(ObjectId id)
		{
			const Json project = run(client, "project.get");
			for (const Json& found : project.at("channels"))
				if (found.at("id") == id)
					return found;
			return Json::object();
		}

		gnubase::midi::DummyBackend& backend;
		Engine engine;
		Client client{engine};
		std::mutex mutex;
		std::vector<Arrived> arrived;
		std::unique_ptr<gnubase::midi::Input> input; // closes before the engine's backend goes
	};

	Json toSynth()
	{
		return {{"midiPort", "dummy/out/Synth"}};
	}
}

TEST_CASE("a MIDI track routed to a MIDI output port plays its notes there at their frames' times", "[integration]")
{
	Setup setup;
	Json routing = toSynth();
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
		setup.midiTrack({{0, g_quarter / 16, 60}, {g_quarter / 8, g_quarter / 32, 62}}, toSynth());
		setup.midiTrack({{g_quarter / 32, g_quarter / 32, 67}, {g_quarter / 8, g_quarter / 32, 69}}, toSynth());
		return setup.play(8);
	};
	const std::vector<Arrived> alone = played(1);
	CHECK(alone.size() == 8);
	CHECK(played(4) == alone);
}

TEST_CASE("a MIDI channel that leaves its MIDI output port ends its notes there in the next block", "[integration]")
{
	Setup setup;
	const ObjectId channel = setup.midiTrack({{0, 2 * g_quarter, 60}}, toSynth());
	REQUIRE(setup.play(1).size() == 1);
	SECTION("routed to no port")
	{
		run(setup.client, "channel.set", {{"channel", channel}, {"midiPort", ""}});
	}
	SECTION("removed")
	{
		run(setup.client, "track.remove", {{"track", setup.channel(channel).at("track")}});
	}
	const auto [before, after] = setup.render();
	const std::vector<Arrived> arrived = setup.wait(2);
	REQUIRE(arrived.size() == 2);
	CHECK(arrived[1].bytes == std::vector<int>{0x80, 60});
	CHECK(arrived[1].time >= before);
	CHECK(arrived[1].time <= after);
}

TEST_CASE("a note that another channel on the MIDI output port holds sounds on when one leaves", "[integration]")
{
	Setup setup;
	const ObjectId first = setup.midiTrack({{0, 2 * g_quarter, 60}}, toSynth());
	const ObjectId second = setup.midiTrack({{0, 2 * g_quarter, 60}}, toSynth());
	REQUIRE(setup.play(2).size() == 2);
	run(setup.client, "track.remove", {{"track", setup.channel(first).at("track")}});
	setup.render();
	run(setup.client, "track.remove", {{"track", setup.channel(second).at("track")}});
	const auto [before, after] = setup.render();
	const std::vector<Arrived> arrived = setup.wait(3);
	REQUIRE(arrived.size() == 3);
	CHECK(arrived[2].bytes == std::vector<int>{0x80, 60});
	CHECK(arrived[2].time >= before); // when the second left, not the first
	CHECK(arrived[2].time <= after);
}

TEST_CASE("MIDI output goes out one output latency ahead, also after the device reports a new one", "[integration]")
{
	Setup setup;
	setup.midiTrack({{0, g_quarter / 16, 60}}, toSynth());
	run(setup.client, "audio.render", {{"frames", 64}}); // the graph
	run(setup.client, "audio.updateLatency", {{"outputLatency", 4800}});
	run(setup.client, "transport.play");
	const auto [before, after] = setup.render();
	const std::vector<Arrived> arrived = setup.wait(1);
	REQUIRE(!arrived.empty());
	constexpr gnubase::HostTime ahead = 100000000; // 4800 frames at 48 kHz
	CHECK(arrived[0].time >= before + ahead);
	CHECK(arrived[0].time <= after + ahead);
}

TEST_CASE("a MIDI output port that isn't connected keeps the routing and plays nothing", "[integration]")
{
	Setup setup;
	const ObjectId missing = setup.midiTrack({{0, g_quarter / 16, 60}}, {{"midiPort", "dummy/out/Missing"}});
	CHECK(setup.channel(missing).at("midiPort") == "dummy/out/Missing");
	setup.midiTrack({{g_quarter / 32, g_quarter / 32, 64}}, toSynth());
	const std::vector<Arrived> arrived = setup.play(1);
	REQUIRE(!arrived.empty());
	CHECK(arrived[0].bytes == std::vector<int>{0x90, 64}); // the connected port's note comes first
}

TEST_CASE("a MIDI channel's MIDI output port is undone, saved and loaded", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId channel = setup.midiTrack({}, toSynth());
	const auto synth = run(client, "track.add", {{"type", "instrument"}}).at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"output", synth}});
	run(client, "history.undo");
	CHECK(setup.channel(channel).at("midiPort") == "dummy/out/Synth");
	CHECK(setup.channel(channel).at("output") == 0);
	run(client, "channel.set", {{"channel", channel}, {"output", synth}});
	run(client, "channel.set", {{"channel", channel}, {"midiPort", "dummy/out/Synth"}});
	run(client, "history.undo");
	CHECK(setup.channel(channel).at("midiPort") == "");
	CHECK(setup.channel(channel).at("output") == synth);
	run(client, "history.redo");

	const Json before = run(client, "project.get");
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "ports.dawproject").u8string();
	run(client, "project.save", {{"path", file}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	CHECK(run(client, "project.get").at("channels") == before.at("channels"));
	CHECK(setup.channel(channel).at("midiPort") == "dummy/out/Synth");

	// Only MIDI channels keep one.
	Json saved = before;
	for (Json& found : saved.at("channels"))
		found["midiPort"] = "dummy/out/Synth";
	const Project loaded = projectFromJson(saved);
	for (const auto& [id, found] : loaded.channels)
		CHECK(found.midiPort ==
			(loaded.trackById.at(found.track).type == TrackType::Midi ? "dummy/out/Synth" : std::string()));
}

TEST_CASE("a MIDI channel plays to an instrument or to a MIDI output port", "[unit]")
{
	Setup setup;
	Client& client = setup.client;
	const ObjectId channel = setup.midiTrack({}, toSynth());
	const auto get = [&] { return setup.channel(channel); };
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
