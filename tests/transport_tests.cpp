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
#include <atomic>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <thread>
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

	// Plays frames in blocks and returns the positions of the frames that recorded, in the order they played.
	std::vector<Samples> recordedPositions(Transport& transport, size_t frames, uint32_t blockSize)
	{
		std::vector<Samples> result;
		for (size_t done = 0; done < frames; done += blockSize)
		{
			const auto count = static_cast<uint32_t>(std::min<size_t>(blockSize, frames - done));
			const TransportBlock& block = transport.advance(count);
			bool any = false;
			for (size_t s = 0; s < block.segmentCount; ++s)
			{
				const graph::TransportSegment& segment = block.segments[s];
				any = any || segment.recording;
				if (segment.recording)
					for (uint32_t i = 0; i < segment.frames; ++i)
						result.push_back(segment.position + i);
			}
			REQUIRE(block.recording == any);
		}
		return result;
	}

	std::vector<Samples> range(Samples from, Samples to)
	{
		std::vector<Samples> result;
		for (Samples p = from; p < to; ++p)
			result.push_back(p);
		return result;
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

TEST_CASE("the cursor can be read on another thread while the command thread locates", "[unit]")
{
	// The cue thread reads the cursor every few milliseconds while a device runs. Each read is a cursor the transport
	// had: one of the locates, never older than the one read before it.
	constexpr Samples locates = 20000;
	Transport transport;
	std::atomic<bool> done{false};
	std::atomic<bool> consistent{true};
	std::thread audio(
		[&]
		{
			while (!done)
				transport.update();
		});
	std::thread cue(
		[&]
		{
			Samples last = 0;
			while (!done)
			{
				const Samples position = transport.position();
				if (position < last || position > locates)
					consistent = false;
				last = position;
			}
		});
	for (Samples position = 1; position <= locates; ++position)
		while (!transport.locate(position)) // the request queue is full until the audio thread catches up
			std::this_thread::yield();
	done = true;
	audio.join();
	cue.join();
	CHECK(consistent);
	transport.update();
	CHECK(transport.position() == locates);
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

TEST_CASE("punch in starts recording at the left locator, sample-exactly", "[unit]")
{
	const uint32_t blockSize = GENERATE(32u, 100u, 257u);
	Transport transport;
	TransportSettings settings;
	settings.left = 1000;
	settings.right = 3000;
	settings.punchIn = true;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(500));
	REQUIRE(transport.play()); // playback alone records from the punch in position on
	CHECK(recordedPositions(transport, 3000, blockSize) == range(1000, 3500));
	CHECK(transport.sync());
	CHECK(transport.recording());
	CHECK(!transport.sync()); // nothing new

	// Started after the punch in position it doesn't record; neither does recording asked for before it.
	REQUIRE(transport.stop());
	REQUIRE(transport.locate(1500));
	REQUIRE(transport.play());
	CHECK(recordedPositions(transport, 1000, blockSize).empty());
	REQUIRE(transport.stop());
	REQUIRE(transport.locate(200));
	REQUIRE(transport.record(true));
	CHECK(recordedPositions(transport, 1000, blockSize) == range(1000, 1200));
}

TEST_CASE("punch out ends recording at the right locator and playback goes on", "[unit]")
{
	const uint32_t blockSize = GENERATE(32u, 100u, 257u);
	Transport transport;
	TransportSettings settings;
	settings.left = 1000;
	settings.right = 3000;
	settings.punchOut = true;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(500));
	REQUIRE(transport.record(true)); // starts playback and recording at the cursor
	CHECK(transport.playing());
	CHECK(recordedPositions(transport, 4000, blockSize) == range(500, 3000));
	CHECK(transport.sync());
	CHECK(transport.playing());
	CHECK(!transport.recording());
	CHECK(transport.position() == 4500);
}

TEST_CASE("lock record ignores the automatic punch out at the right locator", "[unit]")
{
	const uint32_t blockSize = GENERATE(32u, 100u, 257u);
	Transport transport;
	TransportSettings settings;
	settings.left = 1000;
	settings.right = 3000;
	settings.punchOut = true;
	settings.stopAfterPunchOut = true;
	settings.lockRecord = true;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(500));
	REQUIRE(transport.record(true));
	CHECK(recordedPositions(transport, 4000, blockSize) == range(500, 4500));
	CHECK(!transport.sync()); // still playing and recording
	CHECK(transport.recording());
	CHECK(transport.advance(blockSize).recording);
}

TEST_CASE("cycle recording with punch in and out records every lap, nothing before the left locator", "[unit]")
{
	const uint32_t blockSize = GENERATE(32u, 100u, 257u);
	TransportSettings settings = cycle(1000, 2000);
	settings.punchIn = true;
	settings.punchOut = true;
	settings.stopAfterPunchOut = true; // a cycle plays on
	Transport transport;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(500));
	REQUIRE(transport.play());
	std::vector<Samples> expected = model(settings, 500, 3000);
	expected.pop_back();
	expected.erase(expected.begin(), expected.begin() + 500);
	CHECK(recordedPositions(transport, 3000, blockSize) == expected);
	CHECK(transport.advance(blockSize).playing);
}

TEST_CASE("pre-roll starts playback before the cursor, and recording from stop at the cursor", "[unit]")
{
	const uint32_t blockSize = GENERATE(32u, 100u, 257u);
	Transport transport;
	TransportSettings settings;
	settings.preRoll = true;
	settings.preRollAmount = 1000;
	settings.returnToStart = true;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(5000));
	CHECK(transport.startPosition() == 4000);
	REQUIRE(transport.record(true));
	CHECK(transport.advance(blockSize).position == 4000);
	transport.locate(5000);
	REQUIRE(transport.stop());
	transport.update();

	REQUIRE(transport.record(true));
	const std::vector<Samples> recorded = recordedPositions(transport, 2000, blockSize);
	CHECK(recorded == range(5000, 6000));
	REQUIRE(transport.stop());
	transport.update();
	CHECK(transport.position() == 5000); // returns to the cursor, not to where the pre-roll started

	// Not before the project start; without pre-roll playback starts at the cursor.
	REQUIRE(transport.locate(300));
	CHECK(transport.startPosition() == 0);
	REQUIRE(transport.play());
	CHECK(transport.advance(blockSize).position == 0);
	REQUIRE(transport.stop());
	settings.preRoll = false;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(300));
	REQUIRE(transport.play());
	CHECK(transport.advance(blockSize).position == 300);
}

TEST_CASE("stop after punch out stops playback at the right locator plus the post-roll", "[unit]")
{
	const uint32_t blockSize = GENERATE(32u, 100u, 257u);
	const bool postRoll = GENERATE(false, true);
	Transport transport;
	TransportSettings settings;
	settings.left = 1000;
	settings.right = 2000;
	settings.punchIn = true;
	settings.punchOut = true;
	settings.stopAfterPunchOut = true;
	settings.postRoll = postRoll;
	settings.postRollAmount = 500;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.play());
	const Samples stop = postRoll ? 2500 : 2000;
	Samples last = -1; // the last frame played
	std::vector<Samples> recorded;
	for (int block = 0; block < 100; ++block)
	{
		const TransportBlock& played = transport.advance(blockSize);
		if (!played.playing)
			break;
		for (size_t s = 0; s < played.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = played.segments[s];
			last = segment.position + segment.frames - 1;
			if (segment.recording)
				for (uint32_t i = 0; i < segment.frames; ++i)
					recorded.push_back(segment.position + i);
		}
	}
	CHECK(recorded == range(1000, 2000));
	CHECK(last >= stop - 1);
	CHECK(last < stop + blockSize); // it stops at the next block
	CHECK(transport.position() == stop);
	CHECK(transport.sync());
	CHECK(!transport.playing());
	CHECK(!transport.recording());

	// Punching out by hand doesn't stop.
	REQUIRE(transport.locate(1500));
	REQUIRE(transport.record(true));
	transport.advance(blockSize);
	REQUIRE(transport.record(false));
	recordedPositions(transport, 2000, blockSize);
	CHECK(transport.playing());
}

TEST_CASE("a locate during the pre-roll moves the record start, and recording again cancels the stop", "[unit]")
{
	const uint32_t blockSize = GENERATE(32u, 100u);
	Transport transport;
	TransportSettings settings;
	settings.preRoll = true;
	settings.preRollAmount = 1000;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(5000));
	REQUIRE(transport.record(true)); // plays from 4000, records from 5000
	transport.advance(blockSize);
	REQUIRE(transport.locate(8000)); // during the pre-roll: recording starts there
	CHECK(recordedPositions(transport, 500, blockSize) == range(8000, 8500));
	REQUIRE(transport.stop());

	// Recording again during the post-roll stops no more.
	settings.preRoll = false;
	settings.left = 1000;
	settings.right = 2000;
	settings.punchIn = true;
	settings.punchOut = true;
	settings.stopAfterPunchOut = true;
	settings.postRoll = true;
	settings.postRollAmount = 1000;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(0));
	REQUIRE(transport.play());
	recordedPositions(transport, 2200, blockSize); // punched out at 2000
	REQUIRE(transport.record(true));
	CHECK(recordedPositions(transport, 2000, blockSize).size() == 2000);
	CHECK(transport.advance(blockSize).playing);
}

TEST_CASE("a locate while recording waits for the punch in keeps waiting for it", "[unit]")
{
	const bool preRoll = GENERATE(false, true);
	Transport transport;
	TransportSettings settings;
	settings.left = 4800;
	settings.right = 9600;
	settings.punchIn = true;
	settings.preRoll = preRoll;
	settings.preRollAmount = 500;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(1000));
	REQUIRE(transport.record(true)); // waits for the left locator
	transport.advance(64);
	REQUIRE(transport.locate(2000)); // still before it
	CHECK(recordedPositions(transport, 3000, 64) == range(4800, 5000));
	REQUIRE(transport.stop());
	REQUIRE(transport.locate(1000));
	REQUIRE(transport.record(true));
	transport.advance(64);
	REQUIRE(transport.locate(6000)); // past it: recording starts there
	CHECK(recordedPositions(transport, 640, 64) == range(6000, 6640));
}

TEST_CASE("recording from stop in a cycle with punch in starts at the punch in if the cycle never reaches the cursor",
	"[unit]")
{
	TransportSettings settings = cycle(1000, 2000);
	settings.punchIn = true;
	settings.preRoll = true;
	settings.preRollAmount = 1500;
	Transport transport;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(2200)); // the pre-roll starts at 700, before the cycle
	REQUIRE(transport.record(true));
	CHECK(!transport.recording());
	const std::vector<Samples> recorded = recordedPositions(transport, 1000, 64);
	REQUIRE(!recorded.empty());
	CHECK(recorded.front() == 1000);
	CHECK(transport.sync()); // the punch in, which the command thread takes over
	CHECK(transport.recording());
}

TEST_CASE("recording from stop in a cycle starts where the pre-roll starts if the cycle never reaches it", "[unit]")
{
	TransportSettings settings = cycle(1000, 2000);
	settings.preRoll = true;
	settings.preRollAmount = 500;
	Transport transport;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(2200)); // the pre-roll starts at 1700, in the cycle, which jumps back at 2000
	REQUIRE(transport.record(true));
	CHECK(transport.recording()); // the command thread expects what the audio thread does
	const std::vector<Samples> recorded = recordedPositions(transport, 1000, 64);
	REQUIRE(!recorded.empty());
	CHECK(recorded.front() == 1700);
	CHECK(!transport.sync());
}

TEST_CASE("the reply to recording from stop is exact where the pre-roll can't roll back", "[unit]")
{
	Transport transport;
	TransportSettings settings;
	settings.preRoll = true;
	settings.preRollAmount = 1000;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.record(true)); // at 0: nothing to roll back
	CHECK(transport.recording());
	transport.update();
	CHECK(!transport.sync());
	CHECK(transport.recording());
}

TEST_CASE("recording from stop in a skipped range with a pre-roll starts at the left locator", "[unit]")
{
	TransportSettings settings = cycle(2000, 1000); // skips from 1000 to 2000
	settings.preRoll = true;
	settings.preRollAmount = 1000;
	Transport transport;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(1500)); // the pre-roll starts at 500 and jumps over 1500
	REQUIRE(transport.record(true));
	CHECK(!transport.recording());
	const std::vector<Samples> recorded = recordedPositions(transport, 1000, 64);
	REQUIRE(!recorded.empty());
	CHECK(recorded.front() == 2000);
	CHECK(transport.sync());
	CHECK(transport.recording());
}

TEST_CASE("recording started by hand during the pre-roll doesn't start again later", "[unit]")
{
	TransportSettings settings = cycle(1000, 2000);
	settings.punchOut = true;
	settings.preRoll = true;
	settings.preRollAmount = 500;
	Transport transport;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(1200));
	REQUIRE(transport.record(true)); // plays from 700, waits for 1200
	transport.advance(64);
	REQUIRE(transport.record(true)); // records now, from 764
	// Punches out at 2000; the laps after it don't record, also not at the old record start.
	CHECK(recordedPositions(transport, 3000, 64) == range(764, 2000));
}

TEST_CASE("the transport clamps positions and locators to the largest position", "[unit]")
{
	Transport transport;
	TransportSettings settings;
	settings.left = -5;
	settings.right = g_maxPosition + 100;
	REQUIRE(transport.set(settings));
	CHECK(transport.settings().left == 0);
	CHECK(transport.settings().right == g_maxPosition);
	REQUIRE(transport.locate(g_maxPosition + 1));
	CHECK(transport.position() == g_maxPosition);
}

TEST_CASE("the command thread takes over what the audio thread changed only once no request waits", "[unit]")
{
	Transport transport;
	TransportSettings settings;
	settings.left = 100;
	settings.right = 1000;
	settings.punchIn = true;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.play());
	transport.advance(200);
	REQUIRE(transport.stop()); // waiting: the punch in is old news
	CHECK(!transport.sync());
	CHECK(!transport.playing());
	transport.advance(64);
	CHECK(!transport.sync()); // the audio thread stopped as asked
	CHECK(!transport.recording());
	const uint64_t changes = transport.changes();
	REQUIRE(transport.play());
	transport.advance(64);
	CHECK(transport.changes() != changes); // the cue thread sees that something happened
}

TEST_CASE("the position probe outputs each frame's project position, sample-exact across cycle jumps", "[unit]")
{
	const uint32_t blockSize = GENERATE(32u, 100u, 257u);
	graph::GraphDesc desc;
	desc.nodes.push_back(
		{graph::NodeKind::Builtin, 0, ChannelFormat::Mono, processors::createBuiltin("positionProbe")});
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
		Json{{"playing", false}, {"recording", false}, {"position", 0}, {"start", 0}, {"left", 0}, {"right", 0},
			{"cycle", false}, {"startMode", "cursor"}, {"returnToStart", false}, {"punchIn", false},
			{"punchOut", false}, {"preRoll", false}, {"preRollAmount", 0}, {"postRoll", false}, {"postRollAmount", 0},
			{"metronome", false}, {"audioRecordMode", "keepHistory"}, {"midiRecordMode", "newParts"},
			{"lockRecord", false}});

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
	// Where playback started stays after it stops, until it starts again (tempo.tap).
	CHECK(run(client, "transport.get").at("start") == 1000);
	CHECK(run(client, "transport.toggle").at("playing") == false);
	CHECK(run(client, "transport.get").at("start") == 1000);
	CHECK(run(client, "transport.toggle").at("playing") == true);
	CHECK(run(client, "transport.stop").at("playing") == false);

	std::vector<std::string> names;
	while (const auto message = listener.poll())
		if (const auto* event = std::get_if<Event>(&*message))
			names.push_back(event->name);
	CHECK(names.size() == 14);
	CHECK(std::all_of(names.begin(), names.end(), [](const std::string& name) { return name == "transport.changed"; }));

	CHECK_THAT(reject(client, "transport.locate", {{"position", -1}}), ContainsSubstring("position"));
	CHECK_THAT(reject(client, "transport.locate"), ContainsSubstring("position, by or target"));
	CHECK_THAT(reject(client, "transport.locate", {{"target", "nowhere"}}), ContainsSubstring("target"));
	CHECK_THAT(reject(client, "transport.set", {{"right", -5}}), ContainsSubstring("right"));
	CHECK_THAT(reject(client, "transport.set", {{"left", g_maxPosition + 1}}), ContainsSubstring("left"));
	CHECK_THAT(reject(client, "transport.locate", {{"position", g_maxPosition + 1}}), ContainsSubstring("position"));
	CHECK(run(client, "transport.locate", {{"by", 1e300}}).at("position") == g_maxPosition); // not past the end
	CHECK(run(client, "transport.locate", {{"position", g_maxPosition}}).at("position") == g_maxPosition);
	// by from the maximum, and huge amounts both ways, as MCP and Lua may send them (#457).
	CHECK(run(client, "transport.locate", {{"by", 1.0}}).at("position") == g_maxPosition);
	CHECK(run(client, "transport.locate", {{"by", 1e15}}).at("position") == g_maxPosition);
	CHECK(run(client, "transport.locate", {{"by", -1.0}}).at("position") == g_maxPosition - 48000);
	CHECK(run(client, "transport.locate", {{"by", -1e15}}).at("position") == 0);
	CHECK(run(client, "transport.locate", {{"by", -1e300}}).at("position") == 0);           // from the start: stays
	CHECK(run(client, "transport.locate", {{"by", 1e15}}).at("position") == g_maxPosition); // from the start
	CHECK(run(client, "transport.locate", {{"by", -1e300}}).at("position") == 0);           // from the end
	CHECK_THAT(reject(client, "transport.locate", {{"by", "far"}}), ContainsSubstring("by"));
	run(client, "transport.locate", {{"position", 0}});
	CHECK_THAT(reject(client, "transport.set", {{"left", "somewhere"}}), ContainsSubstring("left"));
	CHECK_THAT(reject(client, "transport.set", {{"startMode", "selection"}}), ContainsSubstring("startMode"));
	CHECK_THAT(reject(client, "transport.set", {{"preRollAmount", -1}}), ContainsSubstring("preRollAmount"));
	CHECK_THAT(reject(client, "transport.set", {{"postRollAmount", -1}}), ContainsSubstring("postRollAmount"));
}

TEST_CASE("record starts playback and recording, punches in and out, and stop ends both", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "transport.set", {{"left", 48000}, {"startMode", "cycleStart"}});

	Json state = run(client, "transport.record");
	CHECK(state.at("playing") == true);
	CHECK(state.at("recording") == true);
	CHECK(state.at("position") == 48000);                         // from where playback starts
	state = run(client, "transport.record", {{"record", false}}); // punch out: playback goes on
	CHECK(state.at("playing") == true);
	CHECK(state.at("recording") == false);
	CHECK(run(client, "transport.record").at("recording") == true); // punch in while playing
	CHECK(run(client, "transport.get").at("recording") == true);
	state = run(client, "transport.stop");
	CHECK(state.at("playing") == false);
	CHECK(state.at("recording") == false);

	CHECK(run(client, "transport.record", {{"record", false}}).at("playing") == false); // stopped: nothing to end
	run(client, "transport.record");
	state = run(client, "transport.toggle");
	CHECK(state.at("playing") == false);
	CHECK(state.at("recording") == false);
}

TEST_CASE("lock record keeps a recording from stopping or punching out unless the stop is confirmed", "[integration]")
{
	Engine engine;
	Client client(engine);
	CHECK(run(client, "transport.set", {{"lockRecord", true}}).at("lockRecord") == true);

	// Playback that doesn't record stops as usual.
	run(client, "transport.play");
	CHECK(run(client, "transport.toggle").at("playing") == false);

	run(client, "transport.record");
	CHECK_THAT(reject(client, "transport.stop"), ContainsSubstring("locked"));
	CHECK_THAT(reject(client, "transport.toggle"), ContainsSubstring("locked"));
	CHECK_THAT(reject(client, "transport.toggle", {{"setting", "record"}}), ContainsSubstring("locked"));
	CHECK_THAT(reject(client, "transport.record", {{"record", false}}), ContainsSubstring("locked"));
	Json state = run(client, "transport.get");
	CHECK(state.at("playing") == true);
	CHECK(state.at("recording") == true);
	CHECK(run(client, "transport.toggle", {{"setting", "cycle"}}).at("recording") == true); // other switches work

	// A confirmed stop ends it; the lock stays for the next recording.
	state = run(client, "transport.stop", {{"confirmed", true}});
	CHECK(state.at("playing") == false);
	CHECK(state.at("recording") == false);
	CHECK(state.at("lockRecord") == true);

	// Unlocked, a recording stops as usual.
	run(client, "transport.record");
	run(client, "transport.set", {{"lockRecord", false}});
	CHECK(run(client, "transport.stop").at("playing") == false);
}

TEST_CASE("transport commands set the pre-roll and post-roll", "[integration]")
{
	Engine engine;
	Client client(engine);
	Json state = run(client, "transport.set",
		{{"preRoll", true}, {"preRollAmount", 4800}, {"postRoll", true}, {"postRollAmount", 9600}});
	CHECK(state.at("preRoll") == true);
	CHECK(state.at("preRollAmount") == 4800);
	CHECK(state.at("postRoll") == true);
	CHECK(state.at("postRollAmount") == 9600);
	CHECK(run(client, "transport.set", {{"postRoll", false}}).at("postRollAmount") == 9600); // the rest stays
	CHECK_THAT(reject(client, "transport.set", {{"preRollAmount", (3600LL * 768000) + 1}}),
		ContainsSubstring("preRollAmount"));
	CHECK(run(client, "transport.set", {{"postRollAmount", 3600LL * 768000}}).at("postRollAmount") == 3600LL * 768000);
	run(client, "transport.set", {{"postRollAmount", 9600}});

	// Playback rolls back from the cursor; recording with punch in before the left locator waits for it.
	run(client, "transport.set", {{"left", 48000}, {"right", 96000}, {"punchIn", true}});
	run(client, "transport.locate", {{"position", 10000}});
	state = run(client, "transport.record");
	CHECK(state.at("playing") == true);
	CHECK(state.at("recording") == false);
	CHECK(state.at("position") == 5200);
	run(client, "transport.stop");
	run(client, "transport.locate", {{"position", 60000}});
	CHECK(run(client, "transport.record").at("recording") == false); // after the punch in position: the pre-roll first
	run(client, "transport.stop");
	run(client, "transport.set", {{"preRoll", false}});
	CHECK(run(client, "transport.record").at("recording") == true); // from the cursor, after the punch in position
}

TEST_CASE("the transport announces punching in and out and stopping after the post-roll", "[integration]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}});
	run(client, "settings.set", {{"key", "audio.stopAfterPunchOut"}, {"value", true}});
	run(client, "transport.set",
		{{"left", 1000}, {"right", 2000}, {"punchIn", true}, {"punchOut", true}, {"postRoll", true},
			{"postRollAmount", 500}});
	run(client, "transport.play");
	const auto states = [&listener]
	{
		std::vector<std::pair<bool, bool>> result; // playing, recording
		while (const auto message = listener.poll())
			if (const auto* event = std::get_if<Event>(&*message);
				event != nullptr && event->name == "transport.changed")
				result.emplace_back(event->data.at("playing").get<bool>(), event->data.at("recording").get<bool>());
		return result;
	};
	states(); // what the commands announced
	run(client, "audio.render", {{"frames", 1500}});
	CHECK(run(client, "transport.get").at("recording") == true);
	run(client, "audio.render", {{"frames", 1000}});
	CHECK(run(client, "transport.get").at("recording") == false);
	CHECK(run(client, "transport.get").at("playing") == true); // the post-roll
	run(client, "audio.render", {{"frames", 128}});
	const Json state = run(client, "transport.get");
	CHECK(state.at("playing") == false);
	CHECK(state.at("position") == 2500);
	const std::vector<std::pair<bool, bool>> announced = states();
	REQUIRE(!announced.empty());
	CHECK(std::find(announced.begin(), announced.end(), std::pair{true, true}) != announced.end());
	CHECK(announced.back() == std::pair{false, false});
}

TEST_CASE("while the device runs, the engine announces punching in and stopping after the punch out", "[integration]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}}); // a block every 1.3 ms
	run(client, "settings.set", {{"key", "audio.stopAfterPunchOut"}, {"value", true}});
	run(client, "transport.set",
		{{"left", 24000}, {"right", 48000}, {"punchIn", true}, {"punchOut", true}, {"postRoll", true},
			{"postRollAmount", 24000}});
	run(client, "audio.start");
	run(client, "transport.play");
	// Waits up to 10 s for a transport.changed that matches; keeps the requests of all it sees.
	std::set<RequestId> announced;
	const auto waitFor = [&listener, &announced](bool playing, bool recording)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> message = listener.wait(std::chrono::milliseconds(50));
			const auto* event = message ? std::get_if<Event>(&*message) : nullptr;
			if (event == nullptr || event->name != "transport.changed")
				continue;
			announced.insert(event->request);
			if (event->data.at("playing") == playing && event->data.at("recording") == recording)
				return true;
		}
		return false;
	};
	CHECK(waitFor(true, true));   // the punch in at 0.5 s
	CHECK(waitFor(true, false));  // the punch out at 1 s
	CHECK(waitFor(false, false)); // the stop after the post-roll at 1.5 s
	const Json state = run(client, "transport.get");
	CHECK(state.at("playing") == false);
	CHECK(state.at("position") == 72000);
	run(client, "audio.stop");

	// The engine's runs of transport.update that changed nothing stay out of the command log.
	int updates = 0;
	const Json log = run(client, "engine.commandLog").at("commands");
	for (const Json& entry : log)
		if (entry.at("command") == "transport.update")
		{
			++updates;
			CHECK(announced.count(entry.at("request").get<RequestId>()) == 1);
		}
	CHECK(updates == 3);
}

TEST_CASE("toggle switches cycle, punch in and out, the metronome and recording", "[integration]")
{
	Engine engine;
	Client client(engine);
	for (const char* setting : {"cycle", "punchIn", "punchOut", "preRoll", "postRoll", "metronome"})
	{
		INFO(setting);
		CHECK(run(client, "transport.toggle", {{"setting", setting}}).at(setting) == true);
		CHECK(run(client, "transport.get").at(setting) == true);
		CHECK(run(client, "transport.set", {{setting, false}}).at(setting) == false);
		CHECK(run(client, "transport.toggle", {{"setting", setting}}).at(setting) == true);
		CHECK(run(client, "transport.toggle", {{"setting", setting}}).at(setting) == false);
	}
	CHECK(run(client, "transport.get").at("playing") == false); // none of them plays

	// Recording: on starts playback as transport.record does, off punches out and playback goes on.
	Json state = run(client, "transport.toggle", {{"setting", "record"}});
	CHECK(state.at("playing") == true);
	CHECK(state.at("recording") == true);
	state = run(client, "transport.toggle", {{"setting", "record"}});
	CHECK(state.at("playing") == true);
	CHECK(state.at("recording") == false);
	run(client, "transport.stop");
	CHECK_THAT(reject(client, "transport.toggle", {{"setting", "tempo"}}), ContainsSubstring("setting"));
}

TEST_CASE("locate by moves the cursor by seconds, not before the project start", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "transport.locate", {{"position", 96000}});
	CHECK(run(client, "transport.locate", {{"by", 1.0}}).at("position") == 144000); // 48 kHz
	CHECK(run(client, "transport.locate", {{"by", -0.5}}).at("position") == 120000);
	CHECK(run(client, "transport.locate", {{"by", -10.0}}).at("position") == 0);
	run(client, "project.set", {{"sampleRate", 44100.0}});
	CHECK(run(client, "transport.locate", {{"by", 2.0}}).at("position") == 88200);
}

TEST_CASE("project.set changes the project's first tempo and time signature, one undo step each", "[integration]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	run(client, "project.set", {{"tempo", 90.0}, {"signature", {{"numerator", 3}, {"denominator", 8}}}});
	Json project = run(client, "project.get");
	CHECK(project.at("tempo") == Json::array({{{"position", 0}, {"bpm", 90.0}, {"ramp", false}}}));
	CHECK(project.at("signatures") == Json::array({{{"bar", 0}, {"numerator", 3}, {"denominator", 8}}}));
	bool announced = false;
	while (const auto message = listener.poll())
		if (const auto* event = std::get_if<Event>(&*message))
			announced = announced || event->name == "project.changed";
	CHECK(announced);

	run(client, "history.undo");
	CHECK(run(client, "project.get").at("tempo").at(0).at("bpm") == 120.0);
	run(client, "history.redo");
	CHECK(run(client, "project.get").at("tempo").at(0).at("bpm") == 90.0);

	CHECK_THAT(reject(client, "project.set", {{"tempo", 0.5}}), ContainsSubstring("tempo"));
	CHECK_THAT(reject(client, "project.set", {{"tempo", 1000.0}}), ContainsSubstring("tempo"));
	CHECK_THAT(reject(client, "project.set", {{"signature", {{"numerator", 0}, {"denominator", 4}}}}),
		ContainsSubstring("signature"));
	CHECK_THAT(reject(client, "project.set", {{"signature", {{"numerator", 4}, {"denominator", 3}}}}),
		ContainsSubstring("signature"));
	CHECK(run(client, "project.get").at("signatures").at(0).at("numerator") == 3); // nothing changed
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

TEST_CASE("locating while the device runs and the cue thread reads the cursor", "[integration]")
{
	// The cue thread reads the cursor every 5 ms while the device runs: locating meanwhile must not race with it.
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 256}});
	run(client, "audio.start");
	for (const Samples position : {48000, 1000, 96000, 0, 12345})
	{
		run(client, "transport.locate", {{"position", position}});
		std::this_thread::sleep_for(std::chrono::milliseconds(20)); // longer than the cue interval
		CHECK(run(client, "transport.get").at("position") == position);
	}
	run(client, "transport.play");
	run(client, "transport.locate", {{"position", 500000}});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	CHECK(run(client, "transport.get").at("position").get<Samples>() >= 500000);
	run(client, "transport.stop");
	run(client, "audio.stop");
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

	// An event past the largest position: the cursor stops there, as do locators set to it.
	run(client, "audioEvent.add",
		{{"track", audio.at("track").at("id")}, {"clip", clip.at("clip").at("id")}, {"position", g_maxPosition}});
	CHECK(run(client, "transport.locate", {{"target", "projectEnd"}}).at("position") == g_maxPosition);
	CHECK(run(client, "transport.set", {{"right", "cursor"}}).at("right") == g_maxPosition);
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
