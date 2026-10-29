// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/transport.h"
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/processors/builtin.h"
#include "engine/time/tempomap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;
using gnubase::time::Samples;
using graph::TransportBlock;

namespace
{
	// The project position of every frame of a block; -1 while stopped.
	std::vector<Samples> positions(const TransportBlock& block, uint32_t frames)
	{
		std::vector<Samples> result(frames, -1);
		uint32_t expected = 0;
		for (size_t s = 0; s < block.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = block.segments[s];
			REQUIRE(segment.offset == expected); // segments cover the block in order
			REQUIRE(segment.frames > 0);
			for (uint32_t i = 0; i < segment.frames; ++i)
				result[segment.offset + i] = segment.position + i;
			expected += segment.frames;
		}
		REQUIRE((block.playing ? expected == frames : block.segmentCount == 0));
		return result;
	}

	// The reference model: one sample at a time, the cycle jumps from the right to the left locator.
	std::vector<Samples> model(const TransportSettings& settings, Samples start, size_t frames)
	{
		std::vector<Samples> result;
		Samples position = start;
		for (size_t i = 0; i < frames; ++i)
		{
			result.push_back(position);
			const bool cycles = settings.cycle && settings.right != settings.left && position < settings.right;
			position = cycles && position + 1 == settings.right ? settings.left : position + 1;
		}
		result.push_back(position); // where the next block starts
		return result;
	}

	// Plays frames in blocks and returns every frame's position, then the position after the last block.
	std::vector<Samples> play(Transport& transport, size_t frames, uint32_t blockSize)
	{
		std::vector<Samples> result;
		for (size_t done = 0; done < frames; done += blockSize)
		{
			const auto block = static_cast<uint32_t>(std::min<size_t>(blockSize, frames - done));
			const std::vector<Samples> part = positions(transport.advance(block), block);
			result.insert(result.end(), part.begin(), part.end());
		}
		result.push_back(transport.position());
		return result;
	}

	TransportSettings cycle(Samples left, Samples right)
	{
		TransportSettings settings;
		settings.left = left;
		settings.right = right;
		settings.cycle = true;
		return settings;
	}

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
}

TEST_CASE("a stopped transport has no segments and stays at the cursor", "[unit]")
{
	Transport transport;
	REQUIRE(transport.locate(1234));
	const TransportBlock& block = transport.advance(64);
	CHECK(!block.playing);
	CHECK(block.segmentCount == 0);
	CHECK(block.position == 1234);
	CHECK(transport.position() == 1234);
	CHECK(!transport.playing());
}

TEST_CASE("the cursor shows a waiting locate until the audio thread applies it", "[unit]")
{
	Transport transport;
	REQUIRE(transport.locate(1234));
	CHECK(transport.position() == 1234); // no block has applied it yet
	REQUIRE(transport.locate(500));
	CHECK(transport.position() == 500); // the last one counts
	REQUIRE(transport.play());
	transport.advance(64);
	CHECK(transport.position() == 564); // applied: playback goes on from there
	transport.advance(64);
	CHECK(transport.position() == 628);
	REQUIRE(transport.locate(10));
	CHECK(transport.position() == 10);
	transport.advance(64);
	CHECK(transport.position() == 74);
}

TEST_CASE("playback advances the cursor one block at a time", "[unit]")
{
	Transport transport;
	REQUIRE(transport.locate(100));
	REQUIRE(transport.play());
	const TransportBlock& block = transport.advance(64);
	CHECK(block.playing);
	CHECK(block.position == 100);
	REQUIRE(block.segmentCount == 1);
	CHECK(block.segments[0].offset == 0);
	CHECK(block.segments[0].frames == 64);
	CHECK(block.segments[0].position == 100);
	CHECK(transport.position() == 164);
	CHECK(transport.playing());
}

TEST_CASE("cycle playback jumps from the right to the left locator at the exact sample", "[unit]")
{
	Transport transport;
	REQUIRE(transport.set(cycle(100, 200)));
	REQUIRE(transport.locate(150));
	REQUIRE(transport.play());
	const TransportBlock& block = transport.advance(100);
	REQUIRE(block.segmentCount == 2);
	CHECK(block.segments[0].offset == 0);
	CHECK(block.segments[0].frames == 50);
	CHECK(block.segments[0].position == 150);
	CHECK(block.segments[1].offset == 50);
	CHECK(block.segments[1].frames == 50);
	CHECK(block.segments[1].position == 100);
	CHECK(transport.position() == 150);
}

TEST_CASE("a cycle shorter than a block repeats within the block", "[unit]")
{
	Transport transport;
	REQUIRE(transport.set(cycle(10, 13)));
	REQUIRE(transport.locate(10));
	REQUIRE(transport.play());
	CHECK(positions(transport.advance(10), 10) == std::vector<Samples>{10, 11, 12, 10, 11, 12, 10, 11, 12, 10});
	CHECK(transport.position() == 11);

	// A cycle of one sample: every frame is the left locator.
	REQUIRE(transport.set(cycle(5, 6)));
	REQUIRE(transport.locate(5));
	CHECK(positions(transport.advance(8), 8) == std::vector<Samples>(8, 5));
}

TEST_CASE("with the right locator before the left one, cycle playback skips the range", "[unit]")
{
	Transport transport;
	REQUIRE(transport.set(cycle(200, 100)));
	REQUIRE(transport.locate(50));
	REQUIRE(transport.play());
	const TransportBlock& block = transport.advance(100);
	REQUIRE(block.segmentCount == 2);
	CHECK(block.segments[0].frames == 50);
	CHECK(block.segments[1].position == 200);
	CHECK(transport.position() == 250);
}

TEST_CASE("playback after the right locator plays on, and without cycle it ignores the locators", "[unit]")
{
	Transport transport;
	REQUIRE(transport.set(cycle(100, 200)));
	REQUIRE(transport.locate(300));
	REQUIRE(transport.play());
	CHECK(transport.advance(100).segmentCount == 1);
	CHECK(transport.position() == 400);

	TransportSettings off = cycle(100, 200);
	off.cycle = false;
	REQUIRE(transport.set(off));
	REQUIRE(transport.locate(150));
	CHECK(transport.advance(100).segmentCount == 1);
	CHECK(transport.position() == 250);

	// Equal locators make no cycle.
	REQUIRE(transport.set(cycle(100, 100)));
	REQUIRE(transport.locate(50));
	CHECK(transport.advance(100).segmentCount == 1);
}

TEST_CASE("cycle playback matches the sample-by-sample model for any block size", "[unit]")
{
	const uint32_t blockSize = GENERATE(1u, 7u, 64u, 100u, 333u, 4096u);
	const TransportSettings settings = GENERATE(cycle(1000, 1777), cycle(0, 1), cycle(500, 300), cycle(64, 128));
	const Samples start = GENERATE(Samples{0}, Samples{999}, Samples{1776}, Samples{2000});
	INFO("block " << blockSize << ", locators " << settings.left << " to " << settings.right << ", from " << start);

	Transport transport;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(start));
	REQUIRE(transport.play());
	CHECK(play(transport, 10000, blockSize) == model(settings, start, 10000));
}

TEST_CASE("start from cycle start begins playback at the left locator", "[unit]")
{
	Transport transport;
	TransportSettings settings;
	settings.left = 480;
	settings.right = 960;
	settings.startMode = StartMode::CycleStart;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(5000));
	REQUIRE(transport.play());
	CHECK(transport.advance(10).position == 480);
	CHECK(transport.position() == 490);
}

TEST_CASE("return to start position on stop goes back to where playback started", "[unit]")
{
	Transport transport;
	REQUIRE(transport.locate(1000));
	REQUIRE(transport.play());
	transport.advance(500);
	REQUIRE(transport.stop());
	transport.update();
	CHECK(transport.position() == 1500); // off: the cursor stays where playback stopped

	TransportSettings settings;
	settings.returnToStart = true;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.play());
	transport.advance(500);
	REQUIRE(transport.stop());
	transport.update();
	CHECK(transport.position() == 1500);

	// Locating while playing moves the start position.
	REQUIRE(transport.play());
	transport.advance(100);
	REQUIRE(transport.locate(3000));
	transport.advance(100);
	REQUIRE(transport.stop());
	transport.update();
	CHECK(transport.position() == 3000);
	CHECK(!transport.playing());
}

TEST_CASE("the position probe outputs each frame's project position, sample-exact across cycle jumps", "[unit]")
{
	const uint32_t blockSize = GENERATE(32u, 100u, 257u);
	graph::GraphDesc desc;
	desc.nodes.push_back({graph::NodeKind::Builtin, 0, 1, processors::createBuiltin("positionProbe")});
	const graph::CompiledGraph compiled(desc, 48000.0, blockSize);

	const TransportSettings settings = cycle(2000, 2345);
	Transport transport;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(1900));

	std::vector<float> rendered;
	const auto render = [&](size_t frames)
	{
		for (size_t done = 0; done < frames; done += blockSize)
		{
			const auto block = static_cast<uint32_t>(std::min<size_t>(blockSize, frames - done));
			compiled.process(block, transport.advance(block));
			const float* samples = compiled.audio(0)[0];
			rendered.insert(rendered.end(), samples, samples + block);
		}
	};
	render(300); // stopped
	REQUIRE(transport.play());
	render(3000);

	std::vector<float> expected(300, -1.0f);
	const std::vector<Samples> playing = model(settings, 1900, 3000);
	for (size_t i = 0; i < 3000; ++i)
		expected.push_back(static_cast<float>(playing[i]));
	CHECK(rendered == expected);
}

TEST_CASE("transport commands play, stop, locate and set the locators", "[integration]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);

	Json state = run(client, "transport.get");
	CHECK(state ==
		Json{{"playing", false}, {"position", 0}, {"left", 0}, {"right", 0}, {"cycle", false}, {"startMode", "cursor"},
			{"returnToStart", false}});

	state = run(client, "transport.set", {{"left", 48000}, {"right", 96000}, {"cycle", true}});
	CHECK(state.at("left") == 48000);
	CHECK(state.at("right") == 96000);
	CHECK(state.at("cycle") == true);
	state = run(client, "transport.set", {{"returnToStart", true}}); // the rest stays
	CHECK(state.at("left") == 48000);
	CHECK(state.at("returnToStart") == true);

	CHECK(run(client, "transport.locate", {{"position", 70000}}).at("position") == 70000);
	CHECK(run(client, "transport.locate", {{"target", "rightLocator"}}).at("position") == 96000);
	CHECK(run(client, "transport.locate", {{"target", "leftLocator"}}).at("position") == 48000);
	CHECK(run(client, "transport.locate", {{"target", "projectStart"}}).at("position") == 0);
	CHECK(run(client, "transport.locate", {{"target", "projectEnd"}}).at("position") == 0); // an empty project

	run(client, "transport.locate", {{"position", 1000}});
	state = run(client, "transport.set", {{"left", "cursor"}});
	CHECK(state.at("left") == 1000);
	state = run(client, "transport.exchangeLocators");
	CHECK(state.at("left") == 96000);
	CHECK(state.at("right") == 1000);

	CHECK(run(client, "transport.play").at("playing") == true);
	CHECK(run(client, "transport.toggle").at("playing") == false);
	CHECK(run(client, "transport.toggle").at("playing") == true);
	CHECK(run(client, "transport.stop").at("playing") == false);

	std::vector<std::string> names;
	while (const auto message = listener.poll())
		if (const auto* event = std::get_if<Event>(&*message))
			names.push_back(event->name);
	CHECK(names.size() == 14);
	CHECK(std::all_of(names.begin(), names.end(), [](const std::string& name) { return name == "transport.changed"; }));

	CHECK_THAT(reject(client, "transport.locate", {{"position", -1}}), ContainsSubstring("position"));
	CHECK_THAT(reject(client, "transport.locate"), ContainsSubstring("position or target"));
	CHECK_THAT(reject(client, "transport.locate", {{"target", "nowhere"}}), ContainsSubstring("target"));
	CHECK_THAT(reject(client, "transport.set", {{"right", -5}}), ContainsSubstring("right"));
	CHECK_THAT(reject(client, "transport.set", {{"left", "somewhere"}}), ContainsSubstring("left"));
	CHECK_THAT(reject(client, "transport.set", {{"startMode", "selection"}}), ContainsSubstring("startMode"));
}

TEST_CASE("transport commands see a locate that the running device has not applied yet", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 8192}}); // a block every 170 ms
	run(client, "audio.start");
	CHECK(run(client, "transport.locate", {{"position", 70000}}).at("position") == 70000);
	CHECK(run(client, "transport.get").at("position") == 70000);
	CHECK(run(client, "transport.set", {{"left", "cursor"}}).at("left") == 70000);
	run(client, "transport.locate", {{"position", 1000}});
	CHECK(run(client, "transport.set", {{"right", "cursor"}}).at("right") == 1000);
	run(client, "audio.stop");
	CHECK(run(client, "transport.get").at("position") == 1000);
}

TEST_CASE("go to project end locates to the end of the last event", "[integration]")
{
	Engine engine;
	Client client(engine);
	const Json track = run(client, "track.add", {{"type", "instrument"}});
	const Json part = run(client, "midiPart.add",
		{{"track", track.at("track").at("id")}, {"position", 0}, {"length", 8 * g_ticksPerQuarter}});
	run(client, "note.add",
		{{"part", part.at("part").at("id")},
			{"notes",
				{{{"position", g_ticksPerQuarter}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 1.0}}}}});
	// At the default 120 BPM and 48 kHz a quarter note is 24000 samples; the note ends after two.
	CHECK(run(client, "transport.locate", {{"target", "projectEnd"}}).at("position") == 48000);

	// An audio event's length is in its media's frames: one second at 24 kHz is 48000 project samples.
	const Json media = run(client, "media.add",
		{{"path", "/audio/half-rate.wav"}, {"sampleRate", 24000.0}, {"channels", 1}, {"length", 24000}});
	const Json clip = run(client, "clip.add", {{"media", media.at("media").at("id")}});
	const Json audio = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}});
	run(client, "audioEvent.add",
		{{"track", audio.at("track").at("id")}, {"clip", clip.at("clip").at("id")}, {"position", 12345}});
	CHECK(run(client, "transport.locate", {{"target", "projectEnd"}}).at("position") == 12345 + 48000);
}

TEST_CASE("the engine's transport plays the device's blocks sample-exactly, offline and in cycles", "[integration]")
{
	const int blockSize = GENERATE(1, 64, 100, 333);
	INFO("block size " << blockSize);
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", blockSize}, {"captureSeconds", 1}});
	run(client, "transport.set", {{"left", 1000}, {"right", 1500}, {"cycle", true}});
	run(client, "transport.locate", {{"position", 900}});
	run(client, "audio.render", {{"frames", 300}}); // stopped: the cursor stays
	CHECK(run(client, "transport.get").at("position") == 900);

	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 5000}});
	CHECK(run(client, "transport.get").at("position") == model(cycle(1000, 1500), 900, 5000).back());
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 100}});
	CHECK(run(client, "transport.get").at("position") == model(cycle(1000, 1500), 900, 5000).back());
}
