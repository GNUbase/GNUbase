// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/arranger.h"
#include "engine/audio/transport.h"
#include "engine/document/document.h"
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
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

TEST_CASE("a new plan while playing: playback finds its step again", "[unit]")
{
	const uint32_t blockSize = GENERATE(1u, 64u, 100u);
	INFO("block size " << blockSize);
	ArrangerPlan plan = abPlan();
	plan.steps = {{1, 1, ArrangerRepeat::Normal}, {0, 1, ArrangerRepeat::Normal}, {2, 1, ArrangerRepeat::Normal}};
	Transport transport;
	REQUIRE(transport.locate(1100)); // in A, step 1 of B, A, C
	REQUIRE(transport.play());
	CHECK(play(transport, plan, 100, blockSize) == path({{1100, 1200}}, 100));
	CHECK(transport.arranger().step == 1);
	SECTION("a step removed before the one playing")
	{
		ArrangerPlan edited = plan;
		edited.steps.erase(edited.steps.begin()); // A, C: A is step 0 now
		CHECK(play(transport, edited, 700, blockSize) == path({{1200, 1300}, {3000, 3500}}, 700));
	}
	SECTION("another chain with fewer steps")
	{
		ArrangerPlan other = plan;
		other.steps = {{0, 2, ArrangerRepeat::Normal}}; // A twice
		CHECK(play(transport, other, 50, blockSize) == path({{1200, 1250}}, 50));
		CHECK(transport.arranger().step == 0);
		CHECK(play(transport, other, 450, blockSize) == path({{1250, 1300}, {1000, 1300}}, 450));
	}
}

TEST_CASE("arranger mode switched on while playing enters the chain where playback is", "[unit]")
{
	const uint32_t blockSize = GENERATE(1u, 64u, 100u);
	INFO("block size " << blockSize);
	ArrangerPlan off = abPlan();
	off.active = false;
	off.steps = {{0, 1, ArrangerRepeat::Normal}, {2, 1, ArrangerRepeat::Normal}}; // A, C
	Transport transport;
	REQUIRE(transport.locate(1100)); // in A
	REQUIRE(transport.play());
	CHECK(play(transport, off, 2100, blockSize) == path({{1100, 3200}}, 2100)); // linear into C
	ArrangerPlan on = off;
	on.active = true;
	CHECK(play(transport, on, 400, blockSize) == path({{3200, 3600}}, 400)); // on in C, no jump back
	CHECK(transport.arranger().step == -1);                                  // past the chain
}

TEST_CASE("with a pre-roll playback enters the chain where it starts without it", "[unit]")
{
	const uint32_t blockSize = GENERATE(1u, 64u, 100u);
	INFO("block size " << blockSize);
	ArrangerPlan plan = abPlan();
	plan.steps = {{0, 1, ArrangerRepeat::Normal}, {2, 1, ArrangerRepeat::Normal}}; // A, C
	Transport transport;
	TransportSettings settings;
	settings.preRoll = true;
	settings.preRollAmount = 200;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(1000)); // where play puts playback from outside the chain: the first step's start
	REQUIRE(transport.play());
	CHECK(play(transport, plan, 1100, blockSize) == path({{800, 1300}, {3000, 3500}}, 1100));
}

TEST_CASE("a real plan change during the pre-roll keeps the chain playing", "[unit]")
{
	const uint32_t blockSize = GENERATE(1u, 64u, 100u);
	INFO("block size " << blockSize);
	ArrangerPlan plan = abPlan();
	plan.steps = {{0, 1, ArrangerRepeat::Normal}, {2, 1, ArrangerRepeat::Normal}}; // A, C
	Transport transport;
	TransportSettings settings;
	settings.preRoll = true;
	settings.preRollAmount = 200;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(1000)); // play starts at A (1000); the pre-roll plays from 800
	REQUIRE(transport.play());
	CHECK(play(transport, plan, 100, blockSize) == path({{800, 900}}, 100)); // in the pre-roll, before A
	// A chain edit that really changes the plan (A now repeats twice) while still in the pre-roll: playback enters at
	// the start without the pre-roll, so it keeps the chain at A instead of going linear from the pre-roll position.
	ArrangerPlan edited = plan;
	edited.steps = {{0, 2, ArrangerRepeat::Normal}, {2, 1, ArrangerRepeat::Normal}}; // A twice, C
	CHECK(play(transport, edited, 700, blockSize) == path({{900, 1300}, {1000, 1300}}, 700));
	CHECK(transport.arranger().step == 1); // on to C
}

TEST_CASE("a chain edit while the chain plays back into the project keeps the step and its repeat", "[unit]")
{
	const uint32_t blockSize = GENERATE(1u, 64u, 100u);
	INFO("block size " << blockSize);
	ArrangerPlan plan = abPlan();
	plan.steps = {{2, 1, ArrangerRepeat::Normal}, {0, 2, ArrangerRepeat::Normal}}; // C, then A twice
	Transport transport;
	REQUIRE(transport.locate(3000)); // playback starts at C
	REQUIRE(transport.play());
	// C, then A: the chain jumps back to before the start, where the position stays while A plays.
	CHECK(play(transport, plan, 900, blockSize) == path({{3000, 3500}, {1000, 1300}, {1000, 1100}}, 900));
	CHECK(transport.arranger().step == 1);
	CHECK(transport.arranger().repeat == 1);
	// A real plan change while A plays (A gets one more repeat): the step playing and its repeat are kept, not
	// re-found at the start position, which lies in C.
	ArrangerPlan edited = plan;
	edited.steps[1].repeats = 3;
	CHECK(play(transport, edited, 450, blockSize) == path({{1100, 1300}, {1000, 1250}}, 450));
	CHECK(transport.arranger().step == 1);
	CHECK(transport.arranger().repeat == 2);
}

TEST_CASE("a new plan finds the step and the event waiting again by what they play, not by their old index", "[unit]")
{
	const uint32_t blockSize = GENERATE(1u, 64u, 100u);
	INFO("block size " << blockSize);
	ArrangerPlan plan = abPlan();
	plan.jump = ArrangerJump::End;
	for (ArrangerSection& section : plan.sections)
		section.jumps = {section.end};
	plan.steps = {{0, 3, ArrangerRepeat::Normal}, {2, 1, ArrangerRepeat::Normal}}; // A three times, C

	SECTION("a step chosen while playing is found again after a step is inserted before it")
	{
		Transport transport;
		REQUIRE(transport.locate(1000));
		REQUIRE(transport.play());
		CHECK(play(transport, plan, 100, blockSize) == path({{1000, 1100}}, 100));
		ArrangerState chosen;
		chosen.step = 1; // C, waiting for A's end
		REQUIRE(transport.arrange(chosen, true));
		// A step is inserted before the one waiting, so its index moves from 1 to 2.
		ArrangerPlan edited = plan;
		edited.steps.insert(edited.steps.begin(), {1, 1, ArrangerRepeat::Normal}); // B first: B, A, C
		CHECK(play(transport, edited, 250, blockSize) == path({{1100, 1300}, {3000, 3050}}, 250));
		CHECK(transport.arranger().step == 2); // C takes over, not the step now at the old index (A again)
	}
	SECTION("the step playing is found again by its event, keeping its repeat")
	{
		Transport transport;
		REQUIRE(transport.locate(1000));
		REQUIRE(transport.play());
		// A's three repeats, 100 samples into the third one.
		CHECK(play(transport, plan, 700, blockSize) == path({{1000, 1300}, {1000, 1300}, {1000, 1100}}, 700));
		CHECK(transport.arranger().step == 0);
		CHECK(transport.arranger().repeat == 2);
		// C moves before A: the step playing is found again at its new index, with the repeat it had.
		ArrangerPlan edited = plan;
		edited.steps = {{2, 1, ArrangerRepeat::Normal}, {0, 3, ArrangerRepeat::Normal}}; // C, A three times
		CHECK(play(transport, edited, 150, blockSize) == path({{1100, 1250}}, 150));
		CHECK(transport.arranger().step == 1);
		CHECK(transport.arranger().repeat == 2);
		// The last repeat ends and playback goes on linearly, not back to A's start.
		CHECK(play(transport, edited, 250, blockSize) == path({{1250, 1500}}, 250));
		CHECK(transport.arranger().step == -1);
	}
	SECTION("a chain that uses one event in two steps goes on at the one playing")
	{
		ArrangerPlan twice = abPlan();
		twice.steps = {
			{0, 1, ArrangerRepeat::Normal}, {1, 1, ArrangerRepeat::Normal}, {0, 1, ArrangerRepeat::Normal}}; // A, B, A
		Transport transport;
		REQUIRE(transport.locate(2000)); // B
		REQUIRE(transport.play());
		CHECK(play(transport, twice, 200, blockSize) == path({{2000, 2100}, {1000, 1100}}, 200));
		CHECK(transport.arranger().step == 2); // the second A
		// B goes, so the second A moves from index 2 to 1: playback goes on at it, not at the first A.
		ArrangerPlan edited = twice;
		edited.steps = {{0, 1, ArrangerRepeat::Normal}, {0, 1, ArrangerRepeat::Normal}}; // A, A
		CHECK(play(transport, edited, 100, blockSize) == path({{1100, 1200}}, 100));
		CHECK(transport.arranger().step == 1);
	}
}

TEST_CASE("the transport publishes the plan its arranger state walks", "[unit]")
{
	ArrangerPlan plan = abPlan();
	plan.steps = {{0, 1, ArrangerRepeat::Normal}};
	Transport transport;
	REQUIRE(transport.locate(1000));
	REQUIRE(transport.play());
	CHECK(play(transport, plan, 10, 10) == path({{1000, 1010}}, 10));
	CHECK(transport.arrangerAndPlan().second == &plan);
	CHECK(transport.arrangerAndPlan().first == transport.arranger());
	ArrangerPlan other = plan;
	other.steps = {{0, 2, ArrangerRepeat::Normal}};
	CHECK(play(transport, other, 10, 10) == path({{1010, 1020}}, 10));
	CHECK(transport.arrangerAndPlan().second == &other);
}

TEST_CASE("a stop or a cycle jump ends the pre-roll the transport remembers", "[unit]")
{
	ArrangerPlan plan = abPlan();
	plan.steps = {{0, 1, ArrangerRepeat::Normal}}; // A
	Transport transport;
	TransportSettings settings;
	settings.preRoll = true;
	settings.preRollAmount = 200;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(1000)); // play starts at A; the pre-roll plays from 800
	REQUIRE(transport.play());
	CHECK(play(transport, plan, 50, 50) == path({{800, 850}}, 50)); // in the pre-roll, before A

	SECTION("a stop during the pre-roll")
	{
		REQUIRE(transport.stop());
		// A chain edit while stopped: playback is no longer rolling in, so the new plan enters where the transport
		// stands, outside the chain, not at the start the pre-roll was for.
		ArrangerPlan edited = plan;
		edited.steps = {{0, 2, ArrangerRepeat::Normal}};
		CHECK(play(transport, edited, 10, 10) == std::vector<Samples>(10, -1)); // stopped: nothing plays
		CHECK(transport.arranger().step == -1);
	}
	SECTION("a cycle jump during the pre-roll")
	{
		// Cycling with the plan off: the right locator comes before the start, so the jump lands in the pre-roll.
		ArrangerPlan off = plan;
		off.active = false;
		Transport cycling;
		TransportSettings cycleSettings;
		cycleSettings.cycle = true;
		cycleSettings.left = 500;
		cycleSettings.right = 1020;
		cycleSettings.preRoll = true;
		cycleSettings.preRollAmount = 50;
		REQUIRE(cycling.set(cycleSettings));
		REQUIRE(cycling.locate(1050)); // in A; the pre-roll plays from 1000
		REQUIRE(cycling.play());
		CHECK(play(cycling, off, 30, 30) == path({{1000, 1020}, {500, 510}}, 30));
		// Arranger mode switched on: playback enters where it is (500, outside the chain), not at the stale start.
		CHECK(play(cycling, plan, 10, 10) == path({{510, 520}}, 10));
		CHECK(cycling.arranger().step == -1);
	}
}

TEST_CASE("jump points for 4 bars, 2 bars, 1 bar and 1 beat from an event that starts in the middle of a bar", "[unit]")
{
	using gnubase::time::Position;
	using gnubase::time::TimeBase;
	Project project; // 120 BPM at 48 kHz: a quarter is 24000 samples
	project.signatures = gnubase::time::SignatureMap({{0, {4, 4}}, {2, {3, 4}}});
	const auto bar = [&project](int64_t b) { return project.signatures.barStart(b); };
	ArrangerEvent event;
	event.id = 1;
	event.position = {TimeBase::Musical, gnubase::time::g_ticksPerQuarter * 2}; // beat 3 of bar 1
	event.end = {TimeBase::Musical, bar(7)};
	project.arranger.events.push_back(event);
	project.arrangerMode = true;
	const auto points = [&project](ArrangerJump jump)
	{
		project.arrangerJump = jump;
		return arrangerPlan(project).sections.at(0).jumps;
	};
	// Bar lines: 96000, 192000 (3/4 from there), 264000, 336000, 408000, 480000; the event ends at 552000.
	CHECK(points(ArrangerJump::OneBar) == std::vector<Samples>{96000, 192000, 264000, 336000, 408000, 480000, 552000});
	CHECK(points(ArrangerJump::TwoBars) == std::vector<Samples>{96000, 264000, 408000, 552000});
	CHECK(points(ArrangerJump::FourBars) == std::vector<Samples>{96000, 408000, 552000});
	std::vector<Samples> beats;
	for (Samples at = 72000; at < 552000; at += 24000)
		beats.push_back(at);
	beats.push_back(552000);
	CHECK(points(ArrangerJump::OneBeat) == beats);
	CHECK(points(ArrangerJump::End) == std::vector<Samples>{552000});
	CHECK(points(ArrangerJump::None).empty());
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

TEST_CASE("an edit during the pre-roll keeps the chain playing", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song;
	song.addAudio(dir);
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.b}}, {{"event", song.a}}}}});
	run(song.client, "arranger.setMode", {{"mode", true}});
	run(song.client, "transport.set", {{"preRoll", true}, {"preRollAmount", 300}});
	run(song.client, "transport.locate", {{"position", 2000}});
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 128}});                      // in the pre-roll, before B
	run(song.client, "arranger.set", {{"event", song.a}, {"name", "Verse"}}); // an edit that leaves the plan as it is
	run(song.client, "audio.render", {{"frames", 1300}});
	checkOutput(song, path({{1700, 2500}, {1000, 1600}}, 1428));
}

TEST_CASE("chain playback in real time renders the same as offline", "[integration][realtime]")
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

TEST_CASE("transport.play while playing ends a Repeat Forever loop", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(100);
	song.addAudio(dir);
	run(song.client, "arranger.setChain",
		{{"steps", {{{"event", song.a}, {"mode", "repeatForever"}}, {{"event", song.b}}}}});
	run(song.client, "arranger.setMode", {{"mode", true}});
	run(song.client, "transport.locate", {{"position", 1000}}); // in A, the Forever step 0
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 700}}); // A (1000-1600) loops: repeat 1 at 1100
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == 0);
	CHECK(run(song.client, "transport.get").at("arranger").at("repeat") == 1);

	// Play once again while it loops: the next step takes over (reference manual p. 407), at once with no jump mode.
	run(song.client, "transport.play");
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == 1);
	CHECK(run(song.client, "transport.get").at("position") == 2000); // B's start

	// A Forever step that is the last one: Play goes on past the chain, linearly from where playback is.
	run(song.client, "transport.stop");
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.a}, {"mode", "repeatForever"}}}}});
	run(song.client, "transport.locate", {{"position", 1000}});
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 700}}); // A loops: repeat 1 at 1100
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == 0);
	run(song.client, "transport.play"); // ends the loop: past the chain
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == -1);
	run(song.client, "audio.render", {{"frames", 600}}); // linear from 1100, not looping back to 1000
	CHECK(run(song.client, "transport.get").at("position") == 1700);
}

TEST_CASE("transport.play keeps a triggered event that isn't in the chain", "[integration]")
{
	Song song(100);
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.a}}}}}); // the chain has only A
	run(song.client, "arranger.setMode", {{"mode", true}});
	run(song.client, "transport.locate", {{"position", 2000}});
	run(song.client, "arranger.trigger", {{"event", song.b}}); // B loops, and isn't a step of the chain
	CHECK(run(song.client, "transport.get").at("arranger").at("event") == song.b);
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == -1);
	// Play keeps the looping event instead of replacing it with the chain's first step.
	run(song.client, "transport.play");
	CHECK(run(song.client, "transport.get").at("arranger").at("event") == song.b);
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == -1);
	CHECK(run(song.client, "transport.get").at("position") == 2000);
}

TEST_CASE("a step chosen while playing still takes over after a step is inserted before it", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(100);
	song.addAudio(dir);
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.a}}, {{"event", song.b}}}}}); // A, B
	run(song.client, "arranger.setMode", {{"mode", true}, {"jumpMode", "end"}});
	run(song.client, "transport.locate", {{"position", 1000}});
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 100}});  // in A, at 1100
	run(song.client, "arranger.goToStep", {{"step", 1}}); // B waits for the end of A
	// A step is inserted before the one waiting, so its index moves from 1 to 2.
	run(song.client, "arranger.setChain",
		{{"steps", {{{"event", song.a}}, {{"event", song.a}}, {{"event", song.b}}}}}); // A, A, B
	run(song.client, "audio.render", {{"frames", 900}});
	// A ends at 1600 and the chosen step takes over there, by its event: B plays, not the second A the old index
	// now names.
	checkOutput(song, path({{1000, 1600}, {2000, 2400}}, 1000));
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == 2);
	CHECK(run(song.client, "transport.get").at("position") == 2400);
}

TEST_CASE(
	"with the start mode cycleStart a left locator outside the chain starts at the chain's first step", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(100);
	song.addAudio(dir);
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.b}}, {{"event", song.a}}}}}); // B, A
	run(song.client, "arranger.setMode", {{"mode", true}});
	// The cursor is in A, but playback starts at the left locator (startMode cycleStart), which is outside the chain.
	run(song.client, "transport.set", {{"left", 3000}, {"startMode", "cycleStart"}});
	run(song.client, "transport.locate", {{"position", 1200}});
	run(song.client, "transport.play");
	CHECK(run(song.client, "transport.get").at("position") == 2000); // the chain's first step, not the left locator
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == 0);
	run(song.client, "audio.render", {{"frames", 1200}});
	checkOutput(song, path({{2000, 2500}, {1000, 1600}, {1600, 1700}}, 1200));
}

TEST_CASE("with the start mode cycleStart a left locator inside the chain keeps the step holding it", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(100);
	song.addAudio(dir);
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.b}}, {{"event", song.a}}}}}); // B, then A
	run(song.client, "arranger.setMode", {{"mode", true}});
	// The cursor is in B, but playback starts at the left locator, which the step of A holds.
	run(song.client, "transport.set", {{"left", 1200}, {"startMode", "cycleStart"}});
	run(song.client, "transport.locate", {{"position", 2200}});
	run(song.client, "transport.play");
	CHECK(run(song.client, "transport.get").at("position") == 1200); // the left locator, not the cursor
	run(song.client, "audio.render", {{"frames", 200}});
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == 1); // A's step plays
	CHECK(run(song.client, "transport.get").at("position") == 1400);
	run(song.client, "audio.render", {{"frames", 300}});
	checkOutput(song, path({{1200, 1600}, {1600, 1700}}, 500)); // A, then on after the chain
}

TEST_CASE("transport.get reports no arranger step while the plan doesn't play", "[integration]")
{
	Song song(100);
	run(song.client, "arranger.setChain", {{"steps", {{{"event", song.a}}, {{"event", song.b}}}}});
	run(song.client, "arranger.setMode", {{"mode", true}});
	CHECK(run(song.client, "arranger.goToStep", {{"step", 1}}).at("arranger").at("step") == 1);
	run(song.client, "arranger.setMode", {{"mode", false}}); // arranger mode off: the step playing is stale
	Json arranger = run(song.client, "transport.get").at("arranger");
	CHECK(arranger.at("step") == -1);
	CHECK(arranger.at("event") == 0);
	// Arranger mode on with an empty chain plays nothing either.
	run(song.client, "arranger.setMode", {{"mode", true}});
	run(song.client, "arranger.setChain", {{"steps", Json::array()}});
	CHECK(run(song.client, "transport.get").at("arranger").at("step") == -1);
}

TEST_CASE("repeats loaded from a file are capped at 999 and raised to 1", "[integration]")
{
	Engine engine;
	Client client{engine};
	run(client, "track.add", {{"type", "arranger"}, {"timeBase", "linear"}});
	const ObjectId a = idOf(run(client, "arranger.add", {{"position", 0}, {"end", 1000}}), "event");
	const ObjectId b = idOf(run(client, "arranger.add", {{"position", 1000}, {"end", 2000}}), "event");
	run(client, "arranger.setChain", {{"steps", {{{"event", a}, {"repeats", 2}}, {{"event", b}, {"repeats", 3}}}}});
	Json saved = run(client, "project.get");
	// A file from another application, with repeats outside the 1 to 999 arranger.setChain enforces.
	saved["arranger"]["chains"][0]["steps"][0]["repeats"] = 5000;
	saved["arranger"]["chains"][0]["steps"][1]["repeats"] = 0;
	const Project loaded = projectFromJson(saved);
	REQUIRE(loaded.arranger.chains.size() == 1);
	REQUIRE(loaded.arranger.chains[0].steps.size() == 2);
	CHECK(loaded.arranger.chains[0].steps[0].repeats == 999);
	CHECK(loaded.arranger.chains[0].steps[1].repeats == 1);
}
