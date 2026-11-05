// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/arranger.h"
#include "engine/audio/transport.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using gnubase::time::Samples;

namespace
{
	// Plays frames in blocks along a plan; every frame's position, -1 where the transport stood.
	std::vector<Samples> play(Transport& transport, const ArrangerPlan& plan, size_t frames, uint32_t blockSize)
	{
		std::vector<Samples> result;
		for (size_t done = 0; done < frames; done += blockSize)
		{
			const auto count = static_cast<uint32_t>(std::min<size_t>(blockSize, frames - done));
			const graph::TransportBlock& block = transport.advance(count, &plan);
			std::vector<Samples> part(count, -1);
			uint32_t expected = 0;
			for (size_t s = 0; s < block.segmentCount; ++s)
			{
				const graph::TransportSegment& segment = block.segments[s];
				REQUIRE(segment.offset == expected);
				REQUIRE(segment.frames > 0);
				for (uint32_t i = 0; i < segment.frames; ++i)
					part[segment.offset + i] = segment.position + i;
				expected += segment.frames;
			}
			result.insert(result.end(), part.begin(), part.end());
		}
		return result;
	}

	// The model: the ranges one after the other, then on from the end of the last one.
	std::vector<Samples> path(const std::vector<std::pair<Samples, Samples>>& ranges, size_t frames)
	{
		std::vector<Samples> result;
		for (const auto& [start, end] : ranges)
			for (Samples p = start; p < end && result.size() < frames; ++p)
				result.push_back(p);
		Samples p = ranges.back().second;
		while (result.size() < frames)
			result.push_back(p++);
		return result;
	}

	ArrangerPlan abPlan()
	{
		ArrangerPlan plan;
		plan.active = true;
		plan.sections = {{1000, 1300, {}, 1}, {2000, 2100, {}, 2}, {3000, 3500, {}, 3}}; // A, B, C
		return plan;
	}
}

TEST_CASE("arranger playback plays the chain's steps and repeats at the exact sample, for any block size", "[unit]")
{
	const uint32_t blockSize = GENERATE(1u, 64u, 100u, 333u, 1024u);
	INFO("block size " << blockSize);
	ArrangerPlan plan = abPlan();
	plan.steps = {{1, 2, ArrangerRepeat::Normal}, {0, 1, ArrangerRepeat::Normal}, {2, 1, ArrangerRepeat::Normal}};
	Transport transport;
	REQUIRE(transport.locate(2050)); // in B, the first step: from there
	REQUIRE(transport.play());
	const std::vector<Samples> played = play(transport, plan, 1500, blockSize);
	CHECK(played == path({{2050, 2100}, {2000, 2100}, {1000, 1300}, {3000, 3500}}, 1500));
	const ArrangerState state = transport.arranger();
	CHECK(state.step == -1); // after the chain: on from the end of its last event
}

TEST_CASE("arranger mode without a chain or switched off plays linearly and cycles as usual", "[unit]")
{
	ArrangerPlan plan = abPlan();
	plan.active = false;
	plan.steps = {{1, 2, ArrangerRepeat::Normal}};
	Transport transport;
	TransportSettings settings;
	settings.left = 100;
	settings.right = 200;
	settings.cycle = true;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(150));
	REQUIRE(transport.play());
	CHECK(play(transport, plan, 100, 64) == path({{150, 200}, {100, 150}}, 100));
	plan.active = true; // arranger mode wins over the cycle, outside the chain too
	REQUIRE(transport.locate(150));
	CHECK(play(transport, plan, 100, 64) == path({{150, 250}}, 100));
}

TEST_CASE("pause after repeats stops at the next step's start", "[unit]")
{
	ArrangerPlan plan = abPlan();
	plan.steps = {{0, 1, ArrangerRepeat::Pause}, {1, 1, ArrangerRepeat::Normal}};
	Transport transport;
	REQUIRE(transport.locate(1200));
	REQUIRE(transport.play());
	const std::vector<Samples> played = play(transport, plan, 300, 64);
	std::vector<Samples> expected = path({{1200, 1300}}, 100);
	expected.resize(300, -1);
	CHECK(played == expected);
	CHECK(transport.position() == 2000);
	CHECK(transport.arranger().step == 1);
	CHECK(transport.sync());
	CHECK_FALSE(transport.playing());

	REQUIRE(transport.play()); // on with the next step
	CHECK(play(transport, plan, 150, 64) == path({{2000, 2100}}, 150));
}

TEST_CASE("repeat forever loops a step until another one takes over at the next jump point", "[unit]")
{
	ArrangerPlan plan = abPlan();
	plan.jump = ArrangerJump::OneBeat;
	plan.sections[0].jumps = {1100, 1200, 1300}; // beats of A
	plan.steps = {{0, 1, ArrangerRepeat::Forever}, {2, 1, ArrangerRepeat::Normal}};
	Transport transport;
	REQUIRE(transport.locate(1000));
	REQUIRE(transport.play());
	CHECK(play(transport, plan, 750, 50) == path({{1000, 1300}, {1000, 1300}, {1000, 1150}}, 750));
	CHECK(transport.arranger().repeat == 2);

	// Chosen at 1150: the step takes over at the next beat, 1200.
	ArrangerState chosen;
	chosen.step = 1;
	REQUIRE(transport.arrange(chosen, true));
	CHECK(play(transport, plan, 200, 50) == path({{1150, 1200}, {3000, 3150}}, 200));
	CHECK(transport.arranger().step == 1);
}

TEST_CASE("a triggered event loops until another one is triggered", "[unit]")
{
	ArrangerPlan plan = abPlan();
	plan.jump = ArrangerJump::End;
	for (ArrangerSection& section : plan.sections)
		section.jumps = {section.end};
	plan.steps = {{0, 1, ArrangerRepeat::Normal}};
	Transport transport;
	REQUIRE(transport.locate(1000));
	REQUIRE(transport.play());
	ArrangerState trigger;
	trigger.section = 1;
	REQUIRE(transport.arrange(trigger, true));
	CHECK(play(transport, plan, 600, 100) == path({{1000, 1300}, {2000, 2100}, {2000, 2100}, {2000, 2100}}, 600));
	CHECK(transport.arranger().section == 1);
}

TEST_CASE("arranger state survives packing", "[unit]")
{
	ArrangerState state;
	state.step = 3;
	state.repeat = 7;
	state.pendingSection = 2;
	state.entering = true;
	const auto [a, b] = pack(state);
	CHECK(unpackArranger(a, b) == state);
	CHECK(unpackArranger(pack(ArrangerState()).first, pack(ArrangerState()).second) == ArrangerState());
}

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

	// Every sample different, exact in float: frame f is (f + 1) / 65536.
	float ramp(int64_t frame)
	{
		return static_cast<float>(frame + 1) / 65536.0f;
	}

	std::string writeRamp(const gnubase::test::TempDir& dir, int64_t frames, const std::string& name)
	{
		gnubase::test::Audio audio;
		audio.channels = 1;
		audio.sampleRate = 48000;
		for (int64_t f = 0; f < frames; ++f)
			audio.samples.push_back(ramp(f));
		const std::filesystem::path file = dir.path() / (name + ".wav");
		gnubase::test::writeWav(file, audio);
		return file.u8string();
	}

	// A project with a linear arranger track and two events, A from 1000 to 1600 and B from 2000 to 2500.
	struct Song
	{
		// Positions and lengths are times scale.
		explicit Song(int blockSize = 64, int64_t scale = 1) : m_scale(scale)
		{
			run(client, "audio.setDevice", {{"blockSize", blockSize}, {"inputChannels", 0}, {"captureSeconds", 1}});
			run(client, "track.add", {{"type", "arranger"}, {"timeBase", "linear"}});
			a = idOf(run(client, "arranger.add", {{"position", 1000 * scale}, {"end", 1600 * scale}}), "event");
			b = idOf(run(client, "arranger.add", {{"position", 2000 * scale}, {"end", 2500 * scale}}), "event");
		}

		// Audio events of a mono ramp on a stereo linear audio track: clip frame = position + offset.
		void addAudio(const gnubase::test::TempDir& dir, const std::string& name = "ramp")
		{
			const ObjectId track =
				idOf(run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}}), "track");
			const ObjectId media =
				idOf(run(client, "media.add", {{"path", writeRamp(dir, 20000 * m_scale, name)}}), "media");
			const ObjectId clip = idOf(run(client, "clip.add", {{"media", media}}), "clip");
			for (const auto& [start, length, offset] :
				{std::tuple<int64_t, int64_t, int64_t>{900, 800, 0}, {1900, 800, 5000}, {2400, 1000, 9000}})
			{
				run(client, "audioEvent.add",
					{{"track", track}, {"clip", clip}, {"position", start * m_scale}, {"offset", offset * m_scale},
						{"length", length * m_scale}});
				events.emplace_back(start * m_scale, length * m_scale, offset * m_scale);
			}
		}

		float expected(int64_t position) const
		{
			float value = 0.0f;
			for (const auto& [start, length, offset] : events)
				if (position >= start && position < start + length)
					value = ramp(offset + (position - start));
			return value;
		}

		Engine engine;
		Client client{engine};
		ObjectId a = 0;
		ObjectId b = 0;
		std::vector<std::tuple<int64_t, int64_t, int64_t>> events;

	private:
		int64_t m_scale = 1;
	};

	// Both output channels against the ramp at the positions played.
	void checkOutput(Song& song, const std::vector<int64_t>& played)
	{
		for (int channel = 0; channel < 2; ++channel)
		{
			const auto output =
				run(song.client, "audio.capture", {{"channel", channel}, {"start", 0}, {"frames", played.size()}})
					.at("samples")
					.get<std::vector<float>>();
			REQUIRE(output.size() == played.size());
			size_t wrong = 0;
			size_t first = 0;
			for (size_t i = 0; i < played.size(); ++i)
				if (output[i] != song.expected(played[i]) && wrong++ == 0)
					first = i;
			INFO("channel " << channel << ": " << wrong << " wrong frames, the first at frame " << first
							<< " (position " << played[first] << ")");
			CHECK(wrong == 0);
		}
		CHECK(run(song.client, "audio.get").at("playbackDropouts") == 0);
	}
}

TEST_CASE("arranger events and chains: names, steps, undo and the arranger track", "[integration]")
{
	Engine engine;
	Client client{engine};
	CHECK(run(client, "arranger.get").at("track") == 0);
	// The first event adds the arranger track with its first chain.
	const Json first = run(client, "arranger.add", {{"position", 0}, {"end", 1720320 * 4}});
	CHECK(first.at("event").at("name") == "A");
	Json arranger = run(client, "arranger.get");
	const auto track = arranger.at("track").get<ObjectId>();
	CHECK(track != 0);
	REQUIRE(arranger.at("chains").size() == 1);
	CHECK(arranger.at("chains")[0].at("name") == "Chain 1");
	CHECK(reject(client, "track.add", {{"type", "arranger"}}) == "a project has one arranger track at most");
	const ObjectId a = idOf(first, "event");
	const ObjectId b = idOf(run(client, "arranger.add", {{"position", 1720320 * 8}, {"end", 1720320 * 12}}), "event");
	CHECK(run(client, "arranger.get").at("events")[1].at("name") == "B");
	CHECK(reject(client, "arranger.add", {{"position", 100}, {"end", 100}}) ==
		"an arranger event ends at least one sample after its position");

	run(client, "arranger.set", {{"event", b}, {"name", "Chorus"}, {"position", 1720320 * 16}});
	arranger = run(client, "arranger.get");
	CHECK(arranger.at("events")[1].at("name") == "Chorus");
	CHECK(arranger.at("events")[1].at("end") == 1720320 * 20); // moved with its length

	run(client, "arranger.setChain",
		{{"steps", {{{"event", b}, {"repeats", 2}}, {{"event", a}, {"mode", "pauseAfterRepeats"}}}}});
	arranger = run(client, "arranger.get");
	const Json steps = arranger.at("chains")[0].at("steps");
	REQUIRE(steps.size() == 2);
	CHECK(steps[0].at("repeats") == 2);
	CHECK(steps[1].at("mode") == "pauseAfterRepeats");
	CHECK(reject(client, "arranger.setChain", {{"steps", {{{"event", 999}}}}}) == "no arranger event 999");

	// A duplicate becomes the active chain; the last chain stays.
	const ObjectId copy = idOf(run(client, "arranger.addChain", {{"duplicate", true}}), "chain");
	arranger = run(client, "arranger.get");
	CHECK(arranger.at("chain") == copy);
	CHECK(arranger.at("chains")[1].at("name") == "Chain 2");
	CHECK(arranger.at("chains")[1].at("steps") == steps);
	run(client, "arranger.removeChain");
	CHECK(run(client, "arranger.get").at("chains").size() == 1);
	CHECK(reject(client, "arranger.removeChain") == "the arranger track keeps its last chain");

	// Removing an event takes its steps out of the chains; undo brings both back.
	run(client, "arranger.remove", {{"events", {a}}});
	CHECK(run(client, "arranger.get").at("chains")[0].at("steps").size() == 1);
	run(client, "history.undo");
	CHECK(run(client, "arranger.get").at("chains")[0].at("steps") == steps);

	// Mode and jump mode are settings: no undo step.
	run(client, "arranger.setMode", {{"mode", true}, {"jumpMode", "1bar"}});
	run(client, "history.undo"); // the removal's undo was the last step: this undoes the chain's removal
	arranger = run(client, "arranger.get");
	CHECK(arranger.at("mode") == true);
	CHECK(arranger.at("jumpMode") == "1bar");
	CHECK(reject(client, "arranger.setMode", {{"jumpMode", "3bars"}}).find("jumpMode") != std::string::npos);

	// Removing the arranger track removes the events and chains.
	run(client, "track.remove", {{"track", track}});
	arranger = run(client, "arranger.get");
	CHECK(arranger.at("events").empty());
	CHECK(arranger.at("chains").empty());
}

TEST_CASE("the arranger is saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "Song.dawproject").u8string();
	Json saved;
	{
		Song song;
		run(song.client, "arranger.setChain", {{"steps", {{{"event", song.b}, {"repeats", 3}}, {{"event", song.a}}}}});
		run(song.client, "arranger.addChain", {{"name", "Short"}});
		run(song.client, "arranger.setMode", {{"mode", true}, {"jumpMode", "end"}});
		saved = run(song.client, "arranger.get");
		run(song.client, "project.save", {{"path", path}});
	}
	Engine engine;
	Client client{engine};
	run(client, "project.load", {{"path", path}});
	CHECK(run(client, "arranger.get") == saved);
}

TEST_CASE("chain playback render: arranger mode plays the chain's sections sample-exactly", "[integration]")
{
	const int blockSize = GENERATE(64, 333);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Song song(blockSize);
	song.addAudio(dir);
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.b}, {"repeats", 2}}, {{"event", song.a}}}}});
	run(song.client, "arranger.setMode", {{"mode", true}});

	// From the cursor at 0, outside the chain: playback starts at the chain's first step.
	run(song.client, "transport.play");
	CHECK(run(song.client, "transport.get").at("position") == 2000);
	run(song.client, "audio.render", {{"frames", 2600}});
	checkOutput(song, path({{2000, 2500}, {2000, 2500}, {1000, 1600}}, 2600));
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == -1); // past the chain
}

TEST_CASE("chain playback in real time renders the same as offline", "[integration]")
{
	const gnubase::test::TempDir dir;
	const auto build = [&dir](Song& song, const std::string& name)
	{
		song.addAudio(dir, name);
		run(song.client, "arranger.setChain",
			{{"steps", {{{"event", song.b}, {"repeats", 3}}, {{"event", song.a}, {"repeats", 2}}}}});
		run(song.client, "arranger.setMode", {{"mode", true}});
		run(song.client, "transport.play");
	};
	// Sections of a quarter to a third of a second, as short as a bar of fast music.
	constexpr int64_t scale = 25;
	constexpr int64_t frames = 2700 * scale;
	Song offline(256, scale);
	build(offline, "offline");
	run(offline.client, "audio.render", {{"frames", frames}});
	Song realtime(256, scale);
	build(realtime, "realtime");
	run(realtime.client, "audio.start");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (run(realtime.client, "audio.get").at("position").get<int64_t>() < frames &&
		std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	run(realtime.client, "audio.stop");
	CHECK(run(realtime.client, "audio.get").at("playbackDropouts").get<int64_t>() == 0);
	const auto capture = [](Song& song, int64_t count)
	{
		return run(song.client, "audio.capture", {{"channel", 0}, {"start", 0}, {"frames", count}})
			.at("samples")
			.get<std::vector<float>>();
	};
	const std::vector<float> actual = capture(realtime, frames);
	const std::vector<float> expected = capture(offline, frames);
	REQUIRE(actual.size() == expected.size());
	const auto difference = std::mismatch(actual.begin(), actual.end(), expected.begin());
	INFO("first difference at frame " << (difference.first - actual.begin()));
	CHECK(difference.first == actual.end());
}

TEST_CASE("arranger.goToStep and arranger.trigger choose what plays, and transport.get shows it", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(100);
	song.addAudio(dir);
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.a}}, {{"event", song.b}, {"repeats", 2}}}}});
	CHECK(
		reject(song.client, "arranger.goToStep", {{"step", 0}}) == "arranger mode is off or the active chain is empty");
	run(song.client, "arranger.setMode", {{"mode", true}});

	// Stopped: at once, the cursor goes to the step's event.
	Json transport = run(song.client, "arranger.goToStep", {{"step", 1}, {"repeat", 1}});
	CHECK(transport.at("position") == 2000);
	CHECK(transport.at("arranger") == Json{{"step", 1}, {"repeat", 1}, {"event", song.b}});
	CHECK(reject(song.client, "arranger.goToStep", {{"step", 1}, {"repeat", 2}}) == "step 1 has 2 repeats");
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 700}}); // the last repeat of B, then on after the chain
	checkOutput(song, path({{2000, 2500}}, 700));

	// Playing with the jump mode end: a triggered event takes over at the end of the one playing, then loops.
	run(song.client, "transport.stop");
	run(song.client, "arranger.goToStep", {{"step", 0}});
	run(song.client, "arranger.setMode", {{"jumpMode", "end"}});
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 100}});
	run(song.client, "arranger.trigger", {{"event", song.b}});
	run(song.client, "audio.render", {{"frames", 1500}});
	const std::vector<int64_t> tail = path({{1000, 1600}, {2000, 2500}, {2000, 2500}}, 1600);
	std::vector<int64_t> played = path({{2000, 2500}}, 700);
	played.insert(played.end(), tail.begin(), tail.end());
	checkOutput(song, played);
	CHECK(run(song.client, "transport.get").at("arranger").at("event") == song.b);
}

TEST_CASE("flatten turns the chain into a linear project that plays the same, one undo step", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song;
	song.addAudio(dir);
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.b}}, {{"event", song.a}, {"repeats", 2}}}}});
	const Json before = run(song.client, "project.get").at("audioEvents");
	CHECK(run(song.client, "arranger.flatten", {{"keepTrack", true}}) == Json{{"start", 2000}, {"end", 3700}});
	const Json arranger = run(song.client, "arranger.get");
	REQUIRE(arranger.at("events").size() == 3);
	CHECK(arranger.at("events")[0].at("start") == 2000);
	CHECK(arranger.at("events")[1].at("start") == 2500);
	CHECK(arranger.at("events")[2].at("start") == 3100);
	CHECK(arranger.at("chains")[0].at("steps").size() == 3);

	// Played linearly from the start: what lies before the chain, then B and A twice, as the chain played them.
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 3700}});
	checkOutput(song, path({{0, 2000}, {2000, 2500}, {1000, 1600}, {1000, 1600}}, 3700));

	run(song.client, "transport.stop");
	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get").at("audioEvents") == before);
	run(song.client, "arranger.flatten");
	CHECK(run(song.client, "arranger.get").at("track") == 0); // without keepTrack the arranger track goes
}
