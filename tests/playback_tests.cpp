// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/audition.h"
#include "engine/audio/playback.h"
#include "engine/audio/transport.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/graph/events.h"
#include "engine/graph/processor.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace
{
	// Every sample of a ramp file is different: frame f of channel c is (f + 1) / 65536, negated on odd channels.
	// The values are exact in float, so playback can be checked sample by sample.
	float ramp(int64_t frame, int channel)
	{
		const float value = static_cast<float>(frame + 1) / 65536.0f;
		return channel % 2 == 0 ? value : -value;
	}

	std::string writeRamp(const gnubase::test::TempDir& dir, const std::string& name, int channels, int64_t frames,
		int sampleRate = 48000)
	{
		gnubase::test::Audio audio;
		audio.channels = channels;
		audio.sampleRate = sampleRate;
		for (int64_t f = 0; f < frames; ++f)
			for (int c = 0; c < channels; ++c)
				audio.samples.push_back(ramp(f, c));
		const std::filesystem::path file = dir.path() / (name + ".wav");
		gnubase::test::writeWav(file, audio);
		return file.u8string();
	}

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	ObjectId idOf(const Json& result, const char* kind)
	{
		return result.at(kind).at("id").get<ObjectId>();
	}

	// An audio event as the test expects it to play: in the order added, so later ones are in front.
	struct TestEvent
	{
		ObjectId track = 0;
		int64_t start = 0;  // project samples (linear tracks)
		int64_t offset = 0; // clip frames
		int64_t length = 0;
		int clipChannels = 1;
		double volume = 0.0; // dB
		bool mute = false;
	};

	struct TestTrack
	{
		ObjectId id = 0;
		int channels = 2;
	};

	class Song
	{
	public:
		explicit Song(int blockSize)
		{
			run(client, "audio.setDevice", {{"blockSize", blockSize}, {"inputChannels", 0}, {"captureSeconds", 1}});
		}

		TestTrack addTrack(int channels)
		{
			const TestTrack track{
				idOf(run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", channels}}),
					"track"),
				channels};
			m_tracks.push_back(track);
			return track;
		}

		ObjectId addClip(const std::string& path)
		{
			return idOf(run(client, "clip.add", {{"media", idOf(run(client, "media.add", {{"path", path}}), "media")}}),
				"clip");
		}

		void add(const TestEvent& event, ObjectId clip)
		{
			run(client, "audioEvent.add",
				{{"track", event.track}, {"clip", clip}, {"position", event.start}, {"offset", event.offset},
					{"length", event.length}, {"volume", event.volume}, {"mute", event.mute}});
			m_events.push_back(event);
		}

		// The expected output channel at a project position: per track the front event that is not muted, summed.
		float expected(int64_t position, int outputChannel) const
		{
			float sum = 0.0f;
			for (const TestTrack& track : m_tracks)
			{
				const TestEvent* front = nullptr;
				for (const TestEvent& event : m_events)
					if (event.track == track.id && !event.mute && position >= event.start &&
						position < event.start + event.length)
						front = &event;
				if (front == nullptr)
					continue;
				// A mono track reaches both output channels at -3 dB; a clip plays on the track's channels one to
				// one, and a mono clip on all of them.
				const int trackChannel = track.channels == 1 ? 0 : outputChannel;
				const int clipChannel = front->clipChannels == 1 ? 0 : trackChannel;
				if (clipChannel >= front->clipChannels)
					continue;
				const auto gain = static_cast<float>(std::pow(10.0, front->volume / 20.0));
				const float sample = ramp(front->offset + (position - front->start), clipChannel) * gain;
				sum += track.channels == 1 ? sample * static_cast<float>(std::pow(10.0, -3.0 / 20.0)) : sample;
			}
			return sum;
		}

		std::vector<float> capture(int channel, int64_t frames)
		{
			return run(client, "audio.capture", {{"channel", channel}, {"start", 0}, {"frames", frames}})
				.at("samples")
				.get<std::vector<float>>();
		}

		Engine engine;
		Client client{engine};

	private:
		std::vector<TestTrack> m_tracks;
		std::vector<TestEvent> m_events;
	};

	// Project positions of consecutive frames from start: the transport's cycle model.
	std::vector<int64_t> positions(int64_t start, int64_t frames, int64_t left = 0, int64_t right = 0)
	{
		std::vector<int64_t> result;
		int64_t position = start;
		for (int64_t i = 0; i < frames; ++i)
		{
			result.push_back(position);
			const bool cycles = right != left && position < right;
			position = cycles && position + 1 == right ? left : position + 1;
		}
		return result;
	}

	// Checks both output channels against the expected playback of the given positions; -1 is a stopped frame.
	void checkOutput(Song& song, const std::vector<int64_t>& played)
	{
		for (int channel = 0; channel < 2; ++channel)
		{
			const std::vector<float> output = song.capture(channel, static_cast<int64_t>(played.size()));
			REQUIRE(output.size() == played.size());
			size_t wrong = 0;
			size_t first = 0;
			for (size_t i = 0; i < played.size(); ++i)
			{
				const float expected = played[i] < 0 ? 0.0f : song.expected(played[i], channel);
				if (output[i] != expected && wrong++ == 0)
					first = i;
			}
			INFO("channel " << channel << ": " << wrong << " wrong frames, the first at frame " << first
							<< " (position " << played[first] << "): " << output[first] << " instead of "
							<< (played[first] < 0 ? 0.0f : song.expected(played[first], channel)));
			CHECK(wrong == 0);
		}
		CHECK(run(song.client, "audio.get").at("playbackDropouts") == 0);
	}
}

TEST_CASE("an audio event plays its section of the clip at its sample position, for any block size", "[integration]")
{
	const int blockSize = GENERATE(1, 64, 100, 333);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Song song(blockSize);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "mono", 1, 2000));
	song.add({track.id, 100, 50, 300}, clip);

	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 1000}});
	checkOutput(song, positions(0, 1000));
}

TEST_CASE("where audio events overlap, only the one added last plays", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(64);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "long", 2, 5000));
	song.add({track.id, 0, 0, 1000, 2}, clip);
	song.add({track.id, 300, 2000, 200, 2}, clip);  // in front of the first
	song.add({track.id, 800, 4000, 400, 2}, clip);  // in front of the first one's end
	song.add({track.id, 1500, 100, 100, 2}, clip);  // after a gap
	song.add({track.id, 1600, 3000, 100, 2}, clip); // right after the previous one

	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 2000}});
	checkOutput(song, positions(0, 2000));
}

TEST_CASE("event volume scales an event, and a muted event lets the one behind it play", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(64);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "mono", 1, 3000));
	song.add({track.id, 0, 0, 1000, 1, -6.0}, clip);
	song.add({track.id, 200, 1000, 300, 1, 0.0, true}, clip); // muted, in front
	song.add({track.id, 600, 2000, 300, 1, 3.0}, clip);

	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 1200}});
	const std::vector<float> left = song.capture(0, 1200);
	for (const int64_t position : {0, 199, 200, 499, 500, 599, 600, 899, 900, 999, 1000})
	{
		INFO("position " << position);
		CHECK_THAT(left[static_cast<size_t>(position)], WithinAbs(song.expected(position, 0), 1e-7));
	}
	CHECK(left[1000] == 0.0f);
}

TEST_CASE("audio tracks sum at the output bus, and clips play on the track's channels", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(100);
	const TestTrack stereo = song.addTrack(2);
	const TestTrack mono = song.addTrack(1);
	const ObjectId stereoClip = song.addClip(writeRamp(dir, "stereo", 2, 2000));
	const ObjectId monoClip = song.addClip(writeRamp(dir, "mono", 1, 2000));
	song.add({stereo.id, 0, 0, 500, 2}, stereoClip);
	song.add({stereo.id, 500, 0, 500, 1}, monoClip);    // a mono clip on both channels
	song.add({mono.id, 250, 1000, 500, 2}, stereoClip); // a stereo clip on a mono track plays its first channel

	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 1200}});
	checkOutput(song, positions(0, 1200));
}

TEST_CASE("cycle playback wraps audio events at the exact sample, for any block size", "[integration]")
{
	const int blockSize = GENERATE(1, 64, 100, 333, 1024);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Song song(blockSize);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "stereo", 2, 20000));
	song.add({track.id, 500, 0, 700, 2}, clip);      // across the left locator
	song.add({track.id, 1200, 3000, 200, 2}, clip);  // inside the cycle
	song.add({track.id, 1500, 6000, 1000, 2}, clip); // across the right locator
	run(song.client, "transport.set", {{"left", 1000}, {"right", 1600}, {"cycle", true}});
	run(song.client, "transport.locate", {{"position", 900}});

	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 4000}});
	checkOutput(song, positions(900, 4000, 1000, 1600));
}

TEST_CASE("playback starts where the transport is located and goes silent when it stops", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(64);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "mono", 1, 10000));
	song.add({track.id, 0, 0, 10000}, clip);

	run(song.client, "transport.locate", {{"position", 4321}});
	run(song.client, "audio.render", {{"frames", 300}}); // stopped
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 500}});
	run(song.client, "transport.locate", {{"position", 100}}); // while playing
	run(song.client, "audio.render", {{"frames", 500}});
	run(song.client, "transport.stop");
	run(song.client, "audio.render", {{"frames", 200}});

	std::vector<int64_t> played(300, -1);
	for (const int64_t position : positions(4321, 500))
		played.push_back(position);
	for (const int64_t position : positions(100, 500))
		played.push_back(position);
	played.insert(played.end(), 200, -1);
	checkOutput(song, played);
}

TEST_CASE("a render of overlapping events, clip offsets and a loop wrap matches its golden file", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(128);
	const TestTrack first = song.addTrack(2);
	const TestTrack second = song.addTrack(1);
	const ObjectId stereo = song.addClip(writeRamp(dir, "stereo", 2, 20000));
	const ObjectId mono = song.addClip(writeRamp(dir, "mono", 1, 20000));
	song.add({first.id, 0, 1000, 2000, 2}, stereo);
	song.add({first.id, 1500, 9000, 800, 2, -3.0}, stereo);
	song.add({second.id, 1000, 500, 3000, 1, -12.0}, mono);
	run(song.client, "transport.set", {{"left", 1200}, {"right", 2600}, {"cycle", true}});
	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 6000}});
	checkOutput(song, positions(0, 6000, 1200, 2600));

	gnubase::test::Audio audio;
	audio.channels = 2;
	const std::vector<float> left = song.capture(0, 6000);
	const std::vector<float> right = song.capture(1, 6000);
	for (size_t i = 0; i < left.size(); ++i)
	{
		audio.samples.push_back(left[i]);
		audio.samples.push_back(right[i]);
	}
	const gnubase::test::AudioDiff diff = gnubase::test::compareWithGolden("audio-playback-cycle", audio, 0.0f);
	INFO(diff.report);
	CHECK(diff.equal);
}

TEST_CASE("clips at another sample rate play converted to the project's rate", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(64);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "half-rate", 1, 2000, 24000));
	run(song.client, "audioEvent.add", {{"track", track.id}, {"clip", clip}, {"position", 1000}});

	run(song.client, "transport.play");
	run(song.client, "audio.render", {{"frames", 6000}});
	const std::vector<float> output = song.capture(0, 6000);
	for (size_t i = 0; i < 1000; ++i)
		REQUIRE(output[i] == 0.0f);
	// Two project samples per clip frame: the ramp rises half as fast.
	for (const int64_t frame : {200, 1000, 1800})
	{
		INFO("clip frame " << frame);
		CHECK_THAT(output[static_cast<size_t>(1000 + (2 * frame))], WithinAbs(ramp(frame, 0), 2e-4));
	}
	for (size_t i = 5000; i < 6000; ++i)
		REQUIRE(output[i] == 0.0f);
}

TEST_CASE("real-time playback renders the same as offline", "[integration][realtime]")
{
	const gnubase::test::TempDir dir;
	const std::string path = writeRamp(dir, "stereo", 2, 48000);
	const auto build = [&path](Song& song)
	{
		const TestTrack track = song.addTrack(2);
		const ObjectId clip = song.addClip(path);
		song.add({track.id, 0, 0, 6000, 2}, clip);
		song.add({track.id, 3000, 20000, 6000, 2}, clip);
		run(song.client, "transport.set", {{"left", 2000}, {"right", 7000}, {"cycle", true}});
		run(song.client, "transport.play");
	};
	constexpr int64_t frames = 14400; // 0.3 s
	Song offline(256);
	build(offline);
	run(offline.client, "audio.render", {{"frames", frames}});

	Song realtime(256);
	build(realtime);
	run(realtime.client, "audio.start");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (run(realtime.client, "audio.get").at("position").get<int64_t>() < frames &&
		std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	run(realtime.client, "audio.stop");

	CHECK(run(realtime.client, "audio.get").at("playbackDropouts") == 0);
	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> expected = offline.capture(channel, frames);
		const std::vector<float> actual = realtime.capture(channel, frames);
		REQUIRE(actual.size() == expected.size());
		const auto difference = std::mismatch(actual.begin(), actual.end(), expected.begin());
		INFO("channel " << channel << ", first difference at frame " << (difference.first - actual.begin()));
		CHECK(difference.first == actual.end());
	}
}

namespace
{
	// Playback that runs audio blocks while a cue is under way: after the cue has read where the voices are and
	// before it acts on that, as when the audio thread runs on while the cue thread is between the two.
	class RacingPlayback : public Playback
	{
	public:
		using Playback::Playback;

		std::function<void()> whileCueing; // runs once, in the next cue

	protected:
		void voicesRead() override
		{
			if (whileCueing)
				std::exchange(whileCueing, nullptr)();
		}
	};

	// Plays a playlist block by block through a TrackPlayer, as the audio thread does, and counts the frames that
	// differ from the ramp it plays.
	class Blocks
	{
	public:
		Blocks(std::shared_ptr<const Playlist> playlist, ObjectId channel, Playback& playback, uint32_t size)
			: m_player(std::move(playlist), channel, playback), m_playback(playback), m_output(size)
		{
			m_player.prepareFor(48000.0, size);
		}

		void play()
		{
			std::fill(m_output.begin(), m_output.end(), 0.0f);
			const std::array<float*, 1> channels{m_output.data()};
			const auto size = static_cast<uint32_t>(m_output.size());
			const graph::TransportSegment segment{0, size, position};
			const graph::TransportBlock transport{true, position, &segment, 1};
			const graph::ProcessContext context{channels.data(), 1, size, m_in, m_out, transport};
			m_playback.beginBlock();
			m_player.process(context);
			m_playback.endBlock();
			for (size_t i = 0; i < m_output.size(); ++i)
				if (m_output[i] != ramp(position + static_cast<int64_t>(i), 0))
					++wrong;
			position += size;
		}

		int64_t position = 0;
		size_t wrong = 0;

	private:
		TrackPlayer m_player;
		Playback& m_playback;
		std::vector<float> m_output;
		graph::EventBuffer m_in;
		graph::EventBuffer m_out;
	};
}

TEST_CASE("a cue leaves a voice alone that the audio thread starts reading meanwhile", "[integration]")
{
	// Playback stops in a long event and starts again where it stopped, from the voice that stopped there. The cue
	// thread reads where the voices are before the first block, sees that voice idle for a while, and would cue it
	// ahead; meanwhile the audio thread reads on from it. The cue must not pull the stream away from under it.
	const gnubase::test::TempDir dir;
	const std::string path = writeRamp(dir, "mono", 1, 48000);
	constexpr ObjectId channel = 1;
	constexpr ObjectId media = 2;
	const auto section = [](gnubase::time::Samples start, gnubase::time::Samples end, gnubase::time::Samples frame)
	{
		PlaylistEntry entry;
		entry.start = start;
		entry.end = end;
		entry.media = media;
		entry.frame = frame;
		return entry;
	};
	Playlist sections;
	sections.layers.push_back({section(0, 30000, 0), section(30000, 31000, 40000)});
	const auto playlist = std::make_shared<const Playlist>(std::move(sections));
	RacingPlayback playback(1);
	playback.setTracks({{channel, playlist}}, {{media, path}}, 48000.0);
	constexpr uint32_t blockSize = 4000;
	Blocks blocks(playlist, channel, playback, blockSize);
	constexpr auto timeout = std::chrono::seconds(5);

	Playback::Cue cue;
	cue.window = blockSize;
	playback.cue(cue);
	for (int i = 0; i < 5; ++i)
	{
		REQUIRE(playback.waitUntilBuffered(timeout));
		blocks.play();
	}
	for (int i = 0; i < 3; ++i) // stopped
	{
		playback.beginBlock();
		playback.endBlock();
	}

	// Starting again: cued at the cursor first, then by the cue thread while the audio thread plays.
	cue.position = blocks.position;
	playback.cue(cue);
	REQUIRE(playback.waitUntilBuffered(timeout));
	cue.playing = true;
	cue.window = 48000;
	playback.whileCueing = [&blocks] { blocks.play(); };
	playback.cue(cue);
	REQUIRE(playback.waitUntilBuffered(timeout));
	blocks.play();

	CHECK(blocks.position == 28000);
	CHECK(blocks.wrong == 0);
	CHECK(playback.dropouts() == 0);
}

TEST_CASE("an exact cue while playback runs on elsewhere leaves the voice it reads alone", "[integration]")
{
	// A locate while playing, or an audition that replaces another, cues the streams exactly at the new position
	// while the audio thread still plays on from the old one until it goes there: both need a voice.
	const gnubase::test::TempDir dir;
	const std::string path = writeRamp(dir, "mono", 1, 48000);
	constexpr ObjectId channel = 1;
	constexpr ObjectId media = 2;
	PlaylistEntry entry;
	entry.end = 48000;
	entry.media = media;
	Playlist sections;
	sections.layers.push_back({entry});
	const auto playlist = std::make_shared<const Playlist>(std::move(sections));
	Playback playback(4);
	playback.setTracks({{channel, playlist}}, {{media, path}}, 48000.0);
	constexpr uint32_t blockSize = 256;
	Blocks blocks(playlist, channel, playback, blockSize);
	constexpr auto timeout = std::chrono::seconds(5);

	Playback::Cue cue;
	cue.window = blockSize;
	playback.cue(cue);
	REQUIRE(playback.waitUntilBuffered(timeout));
	for (int i = 0; i < 3; ++i)
		blocks.play();
	cue.position = 24000;
	cue.playsOn = blocks.position;
	playback.cue(cue);
	REQUIRE(playback.waitUntilBuffered(timeout));
	for (int i = 0; i < 2; ++i) // until the audio thread takes the locate
		blocks.play();
	CHECK(blocks.wrong == 0);
	blocks.position = 24000;
	blocks.play();

	CHECK(blocks.wrong == 0);
	CHECK(playback.dropouts() == 0);
}

TEST_CASE("an exact cue while playback runs on elsewhere leaves the voices waiting ahead of it alone", "[integration]")
{
	// Playback reaches the start of the next entry before it goes where the exact cue is: the voice waiting there for
	// it must still be there. Two files with the same ramp, so that every frame plays ramp(position).
	const gnubase::test::TempDir dir;
	const std::string first = writeRamp(dir, "first", 1, 48000);
	const std::string second = writeRamp(dir, "second", 1, 48000);
	constexpr ObjectId channel = 1;
	const auto section = [](ObjectId media, gnubase::time::Samples start, gnubase::time::Samples end)
	{
		PlaylistEntry entry;
		entry.start = start;
		entry.end = end;
		entry.media = media;
		entry.frame = start;
		return entry;
	};
	Playlist sections;
	sections.layers.push_back({section(2, 0, 1000), section(3, 1000, 48000)});
	const auto playlist = std::make_shared<const Playlist>(std::move(sections));
	Playback playback(4);
	playback.setTracks({{channel, playlist}}, {{2, first}, {3, second}}, 48000.0);
	constexpr uint32_t blockSize = 256;
	Blocks blocks(playlist, channel, playback, blockSize);
	constexpr auto timeout = std::chrono::seconds(5);

	Playback::Cue cue;
	cue.window = blockSize;
	playback.cue(cue);
	REQUIRE(playback.waitUntilBuffered(timeout));
	blocks.play();
	// The cue thread: a voice of the second file waits at 1000.
	cue.position = blocks.position;
	cue.playing = true;
	cue.window = 4800;
	playback.cue(cue);
	REQUIRE(playback.waitUntilBuffered(timeout));
	// A locate into the second file while playing: cued exactly there, playback plays on past 1000 meanwhile.
	cue.position = 24000;
	cue.playing = false;
	cue.window = blockSize;
	cue.playsOn = blocks.position;
	playback.cue(cue);
	REQUIRE(playback.waitUntilBuffered(timeout));
	for (int i = 0; i < 5; ++i)
		blocks.play();
	CHECK(blocks.position > 1000);
	CHECK(blocks.wrong == 0);
	blocks.position = 24000;
	blocks.play();

	CHECK(blocks.wrong == 0);
	CHECK(playback.dropouts() == 0);
}

TEST_CASE("an exact cue with nothing playing on reuses the voice the audio thread read", "[integration]")
{
	// Offline rendering cues exactly before every block, also where playback jumps: it needs no more voices than
	// the block plays, here one, which the jump takes along.
	const gnubase::test::TempDir dir;
	const std::string path = writeRamp(dir, "mono", 1, 48000);
	constexpr ObjectId channel = 1;
	constexpr ObjectId media = 2;
	PlaylistEntry entry;
	entry.end = 48000;
	entry.media = media;
	Playlist sections;
	sections.layers.push_back({entry});
	const auto playlist = std::make_shared<const Playlist>(std::move(sections));
	Playback playback(4);
	playback.setTracks({{channel, playlist}}, {{media, path}}, 48000.0);
	constexpr uint32_t blockSize = 256;
	Blocks blocks(playlist, channel, playback, blockSize);
	constexpr auto timeout = std::chrono::seconds(5);

	Playback::Cue cue;
	cue.window = blockSize;
	playback.cue(cue);
	REQUIRE(playback.waitUntilBuffered(timeout));
	blocks.play();
	cue.position = 24000;
	playback.cue(cue);
	REQUIRE(playback.waitUntilBuffered(timeout));
	blocks.position = 24000;
	blocks.play();

	CHECK(blocks.wrong == 0);
	CHECK(playback.voices() == 1);
}

namespace
{
	// The whole ramp file of media 2.
	std::shared_ptr<const Playlist> wholeRamp()
	{
		PlaylistEntry entry;
		entry.end = 48000;
		entry.media = 2;
		Playlist sections;
		sections.layers.push_back({entry});
		return std::make_shared<const Playlist>(std::move(sections));
	}

	// One entry with the whole ramp file on channel 1, played through four voices in blocks of 256 frames.
	struct OneEntry
	{
		OneEntry() { playback.setTracks({{1, playlist}}, {{2, writeRamp(dir, "mono", 1, 48000)}}, 48000.0); }

		gnubase::test::TempDir dir;
		std::shared_ptr<const Playlist> playlist = wholeRamp();
		RacingPlayback playback{4};
		Blocks blocks{playlist, 1, playback, 256};
	};
}

TEST_CASE("an exact cue at the frame that playback plays on from gets a voice of its own", "[integration]")
{
	// An audition that starts at exactly the frame the one before reads next, or a locate there: the voice there plays
	// on with the old position until playback goes to the new one, which needs a voice of its own.
	constexpr auto timeout = std::chrono::seconds(5);
	OneEntry one;
	Playback::Cue cue;
	cue.window = 256;
	one.playback.cue(cue);
	REQUIRE(one.playback.waitUntilBuffered(timeout));
	for (int i = 0; i < 3; ++i)
		one.blocks.play();
	const int64_t next = one.blocks.position;
	cue.position = next;
	cue.playsOn = next;
	one.playback.cue(cue);
	REQUIRE(one.playback.waitUntilBuffered(timeout));
	for (int i = 0; i < 2; ++i) // until the audio thread goes to the new position
		one.blocks.play();
	CHECK(one.blocks.wrong == 0);
	one.blocks.position = next;
	one.blocks.play();

	CHECK(one.blocks.wrong == 0);
	CHECK(one.playback.dropouts() == 0);
}

TEST_CASE(
	"an exact cue while playback runs on elsewhere leaves the voice alone that waits where it goes", "[integration]")
{
	// A locate while the one before still waits for the audio thread: playback plays on until it goes to the first
	// locate's position, whose voice waits there, so the second locate needs a voice of its own.
	constexpr auto timeout = std::chrono::seconds(5);
	OneEntry one;
	Playback::Cue cue;
	cue.window = 256;
	one.playback.cue(cue);
	REQUIRE(one.playback.waitUntilBuffered(timeout));
	one.blocks.play();
	cue.position = 24000;
	cue.playsOn = one.blocks.position;
	one.playback.cue(cue);
	REQUIRE(one.playback.waitUntilBuffered(timeout));
	cue.position = 36000;
	cue.playsOn = 24000;
	one.playback.cue(cue);
	REQUIRE(one.playback.waitUntilBuffered(timeout));
	one.blocks.position = 24000;
	one.blocks.play();
	CHECK(one.blocks.wrong == 0);
	one.blocks.position = 36000;
	one.blocks.play();

	CHECK(one.blocks.wrong == 0);
	CHECK(one.playback.dropouts() == 0);
}

TEST_CASE(
	"a cue at a start leaves the voice alone that the audio thread starts reading there meanwhile", "[integration]")
{
	// The cue thread while a count-in ends or a start goes out: it reads where playback starts before the audio
	// thread plays its first block and the voices after that block. The voice the audio thread started reading must
	// stay where it reads, not go back to the start, which would leave the second block silent.
	constexpr auto timeout = std::chrono::seconds(5);
	OneEntry one;
	Playback::Cue cue;
	cue.window = 256;
	one.playback.cue(cue);
	REQUIRE(one.playback.waitUntilBuffered(timeout));
	cue.playing = true;
	cue.starting = true;
	cue.window = 4800;
	one.playback.whileCueing = [&one] { one.blocks.play(); };
	one.playback.cue(cue);
	CHECK(!one.playback.whileCueing);
	REQUIRE(one.playback.waitUntilBuffered(timeout));
	one.blocks.play();

	CHECK(one.blocks.position == 512);
	CHECK(one.blocks.wrong == 0);
	CHECK(one.playback.dropouts() == 0);
}

TEST_CASE("a cue at a start gives an entry there without a voice one at the start itself", "[integration]")
{
	// As for a playing transport, but playback hasn't got past the start yet: a voice 0.1 seconds ahead would leave
	// that much silent.
	constexpr auto timeout = std::chrono::seconds(5);
	OneEntry one;
	Playback::Cue cue;
	cue.playing = true;
	cue.starting = true;
	cue.window = 4800;
	one.playback.cue(cue);
	REQUIRE(one.playback.waitUntilBuffered(timeout));
	one.blocks.play();

	CHECK(one.blocks.wrong == 0);
	CHECK(one.playback.dropouts() == 0);
}

TEST_CASE("auditioning plays only the pressed audio track, past its channel and without the transport", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Song song(blockSize);
	const Json added = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}});
	const ObjectId track = idOf(added, "track");
	const TestTrack other = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "stereo", 2, 4000));
	song.add({track, 100, 0, 2000, 2}, clip);
	song.add({other.id, 0, 3000, 1000, 2}, clip);
	// Its channel's fader and mute don't apply (reference manual p. 252).
	run(song.client, "channel.set", {{"channel", idOf(added, "channel")}, {"volume", -20.0}, {"mute", true}});
	const Json transport = run(song.client, "transport.get");

	CHECK(run(song.client, "audition.start", {{"track", track}, {"position", 50}}) ==
		Json{{"track", track}, {"media", 0}, {"clip", 0}, {"mode", "play"}});
	run(song.client, "audio.render", {{"frames", 1000}});
	CHECK(run(song.client, "audition.stop") == Json{{"track", 0}, {"media", 0}, {"clip", 0}, {"mode", "off"}});
	run(song.client, "audio.render", {{"frames", 200}});

	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = song.capture(channel, 1200);
		size_t wrong = 0;
		for (int64_t i = 0; i < 1200; ++i)
		{
			const int64_t position = 50 + i;
			const float expected = i < 1000 && position >= 100 ? ramp(position - 100, channel) : 0.0f;
			if (output[static_cast<size_t>(i)] != expected && wrong++ == 0)
				UNSCOPED_INFO("channel " << channel << ", frame " << i << ": " << output[static_cast<size_t>(i)]
										 << " instead of " << expected);
		}
		CHECK(wrong == 0);
	}
	CHECK(run(song.client, "transport.get") == transport); // the transport never played or moved
}

TEST_CASE("auditioning up to an end plays the track to there and then nothing", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Song song(blockSize);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "stereo", 2, 4000));
	song.add({track.id, 0, 0, 4000, 2}, clip);

	CHECK_THAT(song.client.call("audition.start", {{"track", track.id}, {"position", 100}, {"end", 100}}).error,
		ContainsSubstring("end must be after position"));
	// The sample editor's Play back Slice: from 100 to 1000, which is in the middle of a block of 64.
	CHECK(run(song.client, "audition.start", {{"track", track.id}, {"position", 100}, {"end", 1000}}) ==
		Json{{"track", track.id}, {"media", 0}, {"clip", 0}, {"mode", "play"}});
	run(song.client, "audio.render", {{"frames", 1200}});
	CHECK(run(song.client, "audition.stop") == Json{{"track", 0}, {"media", 0}, {"clip", 0}, {"mode", "off"}});
	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = song.capture(channel, 1200);
		size_t wrong = 0;
		for (int64_t i = 0; i < 1200; ++i)
		{
			const int64_t position = 100 + i;
			const float expected = position < 1000 ? ramp(position, channel) : 0.0f;
			if (output[static_cast<size_t>(i)] != expected && wrong++ == 0)
				UNSCOPED_INFO("channel " << channel << ", frame " << i << ": " << output[static_cast<size_t>(i)]
										 << " instead of " << expected);
		}
		CHECK(wrong == 0);
	}
}

TEST_CASE("auditioning a media file plays its frames, also outside the events that play it", "[integration]")
{
	// The sample editor's play tool plays the clip (reference manual p. 630): its media file from a frame, also in
	// the shaded part outside the event and past its end, without the event's volume and mute.
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Song song(blockSize);
	const TestTrack track = song.addTrack(2);
	const ObjectId media = idOf(run(song.client, "media.add", {{"path", writeRamp(dir, "stereo", 2, 4000)}}), "media");
	const ObjectId clip = idOf(run(song.client, "clip.add", {{"media", media}}), "clip");
	run(song.client, "audioEvent.add",
		{{"track", track.id}, {"clip", clip}, {"position", 0}, {"offset", 1000}, {"length", 1000}, {"volume", -20.0},
			{"mute", true}});
	const Json transport = run(song.client, "transport.get");

	// From frame 500, before the event, to 3900 and on past the file's end, where it plays nothing.
	CHECK(run(song.client, "audition.start", {{"media", media}, {"position", 500}, {"end", 900}}) ==
		Json{{"track", 0}, {"media", media}, {"clip", 0}, {"mode", "play"}});
	run(song.client, "audio.render", {{"frames", 500}});
	CHECK(run(song.client, "audition.start", {{"media", media}, {"position", 3900}}) ==
		Json{{"track", 0}, {"media", media}, {"clip", 0}, {"mode", "play"}});
	run(song.client, "audio.render", {{"frames", 300}});
	CHECK(run(song.client, "audition.stop") == Json{{"track", 0}, {"media", 0}, {"clip", 0}, {"mode", "off"}});
	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = song.capture(channel, 800);
		size_t wrong = 0;
		for (int64_t i = 0; i < 800; ++i)
		{
			float expected = 0.0f;
			if (i < 400)
				expected = ramp(500 + i, channel);
			else if (i >= 500 && i < 600)
				expected = ramp(3900 + i - 500, channel);
			if (output[static_cast<size_t>(i)] != expected && wrong++ == 0)
				UNSCOPED_INFO("channel " << channel << ", frame " << i << ": " << output[static_cast<size_t>(i)]
										 << " instead of " << expected);
		}
		CHECK(wrong == 0);
	}
	CHECK(run(song.client, "transport.get") == transport);

	// Its errors: a media file that isn't there, a frame past its end, both a track and a media file, neither.
	CHECK(song.client.call("audition.start", {{"media", 999}, {"position", 0}}).error == "no media file 999");
	CHECK_THAT(song.client.call("audition.start", {{"media", media}, {"position", 4001}}).error,
		ContainsSubstring("position must be 0 to 4000"));
	CHECK_THAT(song.client.call("audition.scrub", {{"media", media}, {"track", track.id}, {"position", 0}}).error,
		ContainsSubstring("a track, a media file or a clip"));
	CHECK_THAT(song.client.call("audition.start", {{"position", 0}}).error,
		ContainsSubstring("a track, a media file or a clip"));

	// Positions are the file's frames: the last 500 frames of a file at half the project's rate play for 1000
	// samples.
	const ObjectId slow =
		idOf(run(song.client, "media.add", {{"path", writeRamp(dir, "slow", 1, 1000, 24000)}}), "media");
	run(song.client, "audition.start", {{"media", slow}, {"position", 500}});
	run(song.client, "audio.render", {{"frames", 1200}});
	run(song.client, "audition.stop");
	const auto output = run(song.client, "audio.capture", {{"channel", 0}, {"start", 800}, {"frames", 1200}})
							.at("samples")
							.get<std::vector<float>>();
	CHECK(std::abs(output[10] - ramp(505, 0)) < 1e-4f);
	CHECK(std::abs(output[600] - ramp(800, 0)) < 1e-4f);
	CHECK(std::all_of(output.begin() + 1010, output.end(), [](float sample) { return sample == 0.0f; }));
}

TEST_CASE("scrubbing a media file plays its frames and leaves the cursor alone", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(64);
	song.addTrack(2);
	const ObjectId media = idOf(run(song.client, "media.add", {{"path", writeRamp(dir, "mono", 1, 4000)}}), "media");
	const Json transport = run(song.client, "transport.get");

	// No event plays the file. 480 frames back in 0.01 seconds play backwards at normal speed.
	CHECK(run(song.client, "audition.scrub", {{"media", media}, {"position", 1000}}) ==
		Json{{"track", 0}, {"media", media}, {"clip", 0}, {"mode", "scrub"}});
	run(song.client, "audition.scrub", {{"media", media}, {"position", 520}, {"seconds", 0.01}});
	run(song.client, "audio.render", {{"frames", 600}});
	run(song.client, "audition.stop");
	CHECK(run(song.client, "transport.get") == transport); // the sample editor locates the cursor itself
	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = song.capture(channel, 600);
		size_t wrong = 0;
		for (size_t i = 0; i < 600; ++i)
		{
			const float expected = i < 480 ? ramp(1000 - static_cast<int64_t>(i), 0) : 0.0f;
			if (output[i] != expected && wrong++ == 0)
				UNSCOPED_INFO(
					"channel " << channel << ", frame " << i << ": " << output[i] << " instead of " << expected);
		}
		CHECK(wrong == 0);
	}
}

TEST_CASE("scrubbing a media file goes by its frames at its own sample rate", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(64);
	song.addTrack(2);
	const ObjectId slow =
		idOf(run(song.client, "media.add", {{"path", writeRamp(dir, "slow", 1, 1000, 24000)}}), "media");

	// 240 frames back at half the project's rate, in 0.01 seconds: 480 samples at normal speed, each frame twice.
	run(song.client, "audition.scrub", {{"media", slow}, {"position", 500}});
	run(song.client, "audition.scrub", {{"media", slow}, {"position", 260}, {"seconds", 0.01}});
	run(song.client, "audio.render", {{"frames", 600}});
	run(song.client, "audition.stop");
	const std::vector<float> output = song.capture(0, 600);
	size_t wrong = 0;
	for (size_t i = 0; i < 600; ++i)
	{
		const float expected = i < 480 ? ramp((1000 - static_cast<int64_t>(i)) / 2, 0) : 0.0f;
		if (output[i] != expected && wrong++ == 0)
			UNSCOPED_INFO("frame " << i << ": " << output[i] << " instead of " << expected);
	}
	CHECK(wrong == 0);
}

TEST_CASE("auditioning a clip plays its media file with the clip's fades", "[integration]")
{
	// The sample editor's play and scrub tools play its clip (reference manual p. 630), with its fades.
	const gnubase::test::TempDir dir;
	Song song(64);
	song.addTrack(2);
	const ObjectId media = idOf(run(song.client, "media.add", {{"path", writeRamp(dir, "stereo", 2, 4000)}}), "media");
	const ObjectId clip = idOf(run(song.client, "clip.add", {{"media", media}}), "clip");
	run(song.client, "clip.setFades",
		{{"clip", clip}, {"fadeIn", {{"length", 1000}, {"shape", "linear"}}},
			{"fadeOut", {{"length", 500}, {"shape", "linear"}}}});
	const auto gain = [](int64_t frame)
	{
		if (frame < 1000)
			return static_cast<float>(frame) / 1000.0f;
		return frame >= 3500 ? static_cast<float>(4000 - frame) / 500.0f : 1.0f;
	};

	// Played from its start to its end; the result names the clip and its media file.
	CHECK(run(song.client, "audition.start", {{"clip", clip}, {"position", 0}}) ==
		Json{{"track", 0}, {"media", media}, {"clip", clip}, {"mode", "play"}});
	run(song.client, "audio.render", {{"frames", 4000}});
	// Scrubbed 480 frames back from frame 1000, at normal speed, in the fade in.
	CHECK(run(song.client, "audition.scrub", {{"clip", clip}, {"position", 1000}}) ==
		Json{{"track", 0}, {"media", media}, {"clip", clip}, {"mode", "scrub"}});
	run(song.client, "audition.scrub", {{"clip", clip}, {"position", 520}, {"seconds", 0.01}});
	run(song.client, "audio.render", {{"frames", 600}});
	run(song.client, "audition.stop");
	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = song.capture(channel, 4600);
		size_t wrong = 0;
		for (int64_t i = 0; i < 4600; ++i)
		{
			float expected = 0.0f;
			if (i < 4000)
				expected = ramp(i, channel) * gain(i);
			else if (i < 4480)
				expected = ramp(1000 - (i - 4000), channel) * gain(1000 - (i - 4000));
			if (std::abs(output[static_cast<size_t>(i)] - expected) > 1e-7f && wrong++ == 0)
				UNSCOPED_INFO("channel " << channel << ", frame " << i << ": " << output[static_cast<size_t>(i)]
										 << " instead of " << expected);
		}
		CHECK(wrong == 0);
	}

	// Its errors: a clip that isn't there, a clip and a media file.
	CHECK(song.client.call("audition.start", {{"clip", 999}, {"position", 0}}).error == "no clip 999");
	CHECK_THAT(song.client.call("audition.scrub", {{"clip", clip}, {"media", media}, {"position", 0}}).error,
		ContainsSubstring("a track, a media file or a clip"));
}

TEST_CASE("auditioning a media file needs its sample rate and length", "[integration]")
{
	// A hand-edited project's media file without them, which loads as it is.
	const char* property = GENERATE("sampleRate", "length");
	INFO(property);
	const gnubase::test::TempDir dir;
	Song song(64);
	const ObjectId media = idOf(run(song.client, "media.add", {{"path", writeRamp(dir, "mono", 1, 1000)}}), "media");
	const std::string file = (dir.path() / "edited.dawproject").u8string();
	run(song.client, "project.save", {{"path", file}, {"embedMedia", false}});
	std::map<std::string, std::string> entries = gnubase::test::readZip(file);
	Json gnubase = Json::parse(entries.at("gnubase.json"));
	gnubase.at("project").at("media").at(0)[property] = std::string(property) == "length" ? -1 : 0;
	entries["gnubase.json"] = gnubase.dump();
	gnubase::test::writeZip(file, entries);
	run(song.client, "project.load", {{"path", file}});

	CHECK(song.client.call("audition.start", {{"media", media}, {"position", 0}}).error ==
		"media file " + std::to_string(media) + " has no sample rate or length");
}

TEST_CASE("auditioning needs a device and an audio track", "[integration]")
{
	Engine engine;
	Client client{engine};
	const ObjectId midi = idOf(run(client, "track.add", {{"type", "midi"}}), "track");
	const Json parameters = {{"track", midi}, {"position", 0}};
	CHECK(client.call("audition.start", parameters).error.find("no audio device") != std::string::npos);
	run(client, "audio.setDevice", {{"blockSize", 64}});
	CHECK(client.call("audition.start", parameters).error == "no audio track " + std::to_string(midi));
	CHECK_THAT(client.call("audition.scrub", {{"track", midi}, {"position", -1}}).error,
		ContainsSubstring("position must be 0 to"));
	CHECK_THAT(client.call("audition.scrub", {{"track", midi}, {"position", g_maxPosition + 1}}).error,
		ContainsSubstring("position must be 0 to"));
	CHECK(run(client, "audition.stop") == Json{{"track", 0}, {"media", 0}, {"clip", 0}, {"mode", "off"}});
}

TEST_CASE("scrubbing plays an audio track forwards and backwards at the speed positions come, and moves the cursor",
	"[integration]")
{
	const gnubase::test::TempDir dir;
	Song song(64);
	const TestTrack track = song.addTrack(2);
	const TestTrack other = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "mono", 1, 4000));
	song.add({track.id, 0, 0, 4000}, clip);
	song.add({other.id, 0, 0, 4000}, clip);

	// The first scrub only sets where it starts and moves the cursor there; 480 frames back in 0.01 seconds (480
	// frames) play backwards at normal speed.
	CHECK(run(song.client, "audition.scrub", {{"track", track.id}, {"position", 1000}}) ==
		Json{{"track", track.id}, {"media", 0}, {"clip", 0}, {"mode", "scrub"}});
	CHECK(run(song.client, "transport.get").at("position") == 1000);
	run(song.client, "audition.scrub", {{"track", track.id}, {"position", 520}, {"seconds", 0.01}});
	CHECK(run(song.client, "transport.get").at("position") == 520);
	run(song.client, "audio.render", {{"frames", 600}});
	// Then 960 frames forwards in 480: twice as fast.
	run(song.client, "audition.scrub", {{"track", track.id}, {"position", 1480}, {"seconds", 0.01}});
	run(song.client, "audio.render", {{"frames", 600}});
	run(song.client, "audition.stop");
	CHECK(run(song.client, "transport.get").at("playing") == false);

	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = song.capture(channel, 1200);
		size_t wrong = 0;
		for (size_t i = 0; i < 1200; ++i)
		{
			float expected = 0.0f; // a mono clip on both channels; silence once a scrub has played
			if (i < 480)
				expected = ramp(1000 - static_cast<int64_t>(i), 0);
			else if (i >= 600 && i < 1080)
				expected = ramp(520 + (2 * static_cast<int64_t>(i - 600)), 0);
			if (output[i] != expected && wrong++ == 0)
				UNSCOPED_INFO(
					"channel " << channel << ", frame " << i << ": " << output[i] << " instead of " << expected);
		}
		CHECK(wrong == 0);
	}
}

TEST_CASE("scrubs that come faster than they play, or late, carry on from where the audio got to", "[integration]")
{
	const bool timed = GENERATE(true, false); // the third scrub with seconds, or as long as since the last one
	INFO((timed ? "with" : "without") << " seconds");
	const gnubase::test::TempDir dir;
	Song song(64);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "mono", 1, 4000));
	song.add({track.id, 0, 0, 4000}, clip);
	const auto scrub = [&song, &track](int64_t position, std::optional<double> seconds)
	{
		Json parameters = {{"track", track.id}, {"position", position}};
		if (seconds)
			parameters["seconds"] = *seconds;
		run(song.client, "audition.scrub", parameters);
	};

	// From 1000 to 1480 in 480 frames, of which 128 play before the next scrub comes: that one goes on from 1128,
	// not from 1480, which would leave out what the first had yet to play.
	scrub(1000, std::nullopt);
	scrub(1480, 0.01);
	run(song.client, "audio.render", {{"frames", 128}});
	scrub(2088, timed ? std::optional<double>(0.01) : std::nullopt);
	if (!timed)
	{
		run(song.client, "audio.render", {{"frames", 1}});
		const std::vector<float> output = song.capture(0, 129);
		CHECK(output[127] == ramp(1127, 0));
		CHECK(output[128] == ramp(1128, 0));
		return;
	}
	// From 1128 to 2088 in 480 frames: twice as fast.
	run(song.client, "audio.render", {{"frames", 480}});
	// Two scrubs before the audio thread plays the first: the second starts where the first would have, from 2088 back
	// to 1128, twice as fast.
	scrub(3000, 0.01);
	scrub(1128, 0.01);
	run(song.client, "audio.render", {{"frames", 592}});

	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = song.capture(channel, 1200);
		size_t wrong = 0;
		for (size_t i = 0; i < 1200; ++i)
		{
			const auto k = static_cast<int64_t>(i);
			float expected = 0.0f; // a mono clip on both channels
			if (k < 128)
				expected = ramp(1000 + k, 0);
			else if (k < 608)
				expected = ramp(1128 + (2 * (k - 128)), 0);
			else if (k < 1088)
				expected = ramp(2088 - (2 * (k - 608)), 0);
			if (output[i] != expected && wrong++ == 0)
				UNSCOPED_INFO(
					"channel " << channel << ", frame " << i << ": " << output[i] << " instead of " << expected);
		}
		CHECK(wrong == 0);
	}
	// Only the first scrub of the five is in the command log: the others would push everything else out of it.
	const Json log = run(song.client, "engine.commandLog").at("commands");
	CHECK(std::count_if(
			  log.begin(), log.end(), [](const Json& entry) { return entry.at("command") == "audition.scrub"; }) == 1);
}

TEST_CASE("an audition that stops closes the files it played", "[integration]")
{
	const gnubase::test::TempDir dir;
	PlaylistEntry entry;
	entry.end = 4000;
	entry.media = 1;
	Playlist playlist;
	playlist.layers.push_back({entry});
	const Playback::Track track{2, std::make_shared<const Playlist>(std::move(playlist))};
	Audition audition;
	audition.play({3, 0, 0}, track, {{1, writeRamp(dir, "mono", 1, 4000)}}, 48000.0, 0, 4000, 2, 64);
	CHECK(audition.voices() > 0);
	audition.stop();
	CHECK(audition.voices() == 0);
}

TEST_CASE(
	"an audition that stops frees what it played, also from the middle of an audio block", "[integration][realtime]")
{
	// The audio thread plays block after block, so that most stops come while it is in the middle of one with the
	// state before: stop frees that state, and the playlist its player plays, once the block is done.
	const gnubase::test::TempDir dir;
	PlaylistEntry entry;
	entry.end = 48000;
	entry.media = 1;
	Playlist sections;
	sections.layers.push_back({entry});
	const auto playlist = std::make_shared<const Playlist>(std::move(sections));
	const Playback::Track track{2, playlist};
	const std::map<ObjectId, std::string> paths{{1, writeRamp(dir, "mono", 1, 48000)}};
	Audition audition;
	std::atomic<bool> playing{true};
	std::thread audio(
		[&audition, &playing]
		{
			constexpr uint32_t frames = 64;
			std::vector<float> output(size_t{2} * frames);
			const std::array<float*, 2> outputs{output.data(), output.data() + frames};
			const std::vector<int> ports{0, 1};
			while (playing)
				audition.process(outputs.data(), 2, ports, frames);
		});
	const long users = playlist.use_count(); // the test's own
	size_t kept = 0;
	for (int i = 0; i < 50; ++i)
	{
		audition.play({3, 0, 0}, track, paths, 48000.0, 0, 48000, 2, 64);
		audition.stop();
		if (playlist.use_count() != users)
			++kept;
	}
	playing = false;
	audio.join();
	CHECK(kept == 0);
}

namespace
{
	// An audition that runs audio blocks while the cue thread cues it: after the cue has read where the audio thread
	// plays and before it cues the streams from there, as when the audio thread runs on in between.
	class RacingAudition : public Audition
	{
	public:
		std::function<void()> whileCueing; // runs once, in the next cue

	protected:
		void playedRead() override
		{
			if (whileCueing)
				std::exchange(whileCueing, nullptr)();
		}
	};
}

TEST_CASE("an audition whose first block plays while the cue thread cues it plays on without a gap", "[integration]")
{
	// The cue thread reads where the audio thread plays before the audition's first block, so that it has played
	// nothing of it yet, and cues its stream after that block, when the stream is already read past the start: it
	// must leave it there, not cue it back to where the audition starts, which would leave the second block silent.
	const gnubase::test::TempDir dir;
	PlaylistEntry entry;
	entry.end = 48000;
	entry.media = 1;
	Playlist sections;
	sections.layers.push_back({entry});
	const Playback::Track track{2, std::make_shared<const Playlist>(std::move(sections))};
	constexpr uint32_t frames = 256;
	RacingAudition audition;
	audition.play({3, 0, 0}, track, {{1, writeRamp(dir, "mono", 1, 48000)}}, 48000.0, 0, 48000, 1, frames);
	// play() waits for the streams at most two seconds: a slow disk thread gets more, and is reported as that.
	REQUIRE(audition.waitUntilBuffered(std::chrono::seconds(5)));
	std::vector<float> output(size_t{frames} * 2, 0.0f);
	const auto playBlock = [&audition, &output](size_t block)
	{
		const std::vector<int> ports{0};
		const std::array<float*, 1> out{output.data() + (block * frames)};
		audition.process(out.data(), 1, ports, frames);
	};
	audition.whileCueing = [&playBlock] { playBlock(0); };
	audition.cue(4800, true);
	CHECK(!audition.whileCueing); // the first block played in the cue
	REQUIRE(audition.waitUntilBuffered(std::chrono::milliseconds(5000)));
	playBlock(1);
	size_t i = 0;
	while (i < output.size() && output[i] == ramp(static_cast<int64_t>(i), 0))
		++i;
	INFO("the ramp stops at frame " << i);
	CHECK(i == output.size());
}

namespace
{
	// Waits until the device has played frames frames, at most 10 seconds.
	void playUntil(Song& song, int64_t frames)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (run(song.client, "audio.get").at("position").get<int64_t>() < frames &&
			std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	int64_t played(Song& song)
	{
		return run(song.client, "audio.get").at("position").get<int64_t>();
	}
}

TEST_CASE("scrubbing a track while the transport plays leaves the playback without dropouts", "[integration][realtime]")
{
	// Each scrub moves the cursor while the transport plays: cued exactly, as transport.locate does it, the tracks
	// play on from there without a frame of silence.
	const gnubase::test::TempDir dir;
	Song song(256);
	const TestTrack scrubbed = song.addTrack(2);
	const TestTrack other = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "stereo", 2, 96000));
	song.add({scrubbed.id, 0, 0, 96000, 2}, clip);
	song.add({other.id, 0, 0, 96000, 2}, clip);
	run(song.client, "transport.play");
	run(song.client, "audio.start");
	playUntil(song, 2400);
	// As a drag sends them, a scrub every 10 milliseconds, here forwards by a hundredth of a second each.
	for (int i = 0; i < 20; ++i)
	{
		run(song.client, "audition.scrub", {{"track", scrubbed.id}, {"position", 24000 + (i * 480)}});
		playUntil(song, played(song) + 480);
	}
	run(song.client, "audition.stop");
	playUntil(song, played(song) + 2400);
	run(song.client, "audio.stop");
	CHECK(run(song.client, "transport.get").at("playing") == true);
	CHECK(run(song.client, "audio.get").at("playbackDropouts") == 0);
}

TEST_CASE("a locate while the transport plays leaves the playback without dropouts", "[integration][realtime]")
{
	// Cued exactly at the new position while the audio thread plays on from the old one until it goes there.
	const gnubase::test::TempDir dir;
	Song song(256);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "stereo", 2, 96000));
	song.add({track.id, 0, 0, 96000, 2}, clip);
	run(song.client, "transport.play");
	run(song.client, "audio.start");
	playUntil(song, 2400);
	for (int i = 0; i < 20; ++i)
	{
		run(song.client, "transport.locate", {{"position", 24000 + (i * 480)}});
		playUntil(song, played(song) + 480);
	}
	run(song.client, "audio.stop");
	CHECK(run(song.client, "audio.get").at("playbackDropouts") == 0);
}

TEST_CASE("playback that starts away from the cursor starts without a dropout", "[integration][realtime]")
{
	// With a pre-roll, playback starts half a second before the cursor, which the cue thread cues while the transport
	// stands. Once transport.play has cued the start, the cue thread cues there too, until the audio thread plays
	// from there, rather than at the cursor, where it would take the start's voice away.
	const gnubase::test::TempDir dir;
	Song song(256);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "stereo", 2, 96000));
	song.add({track.id, 0, 0, 96000, 2}, clip);
	run(song.client, "transport.set", {{"preRoll", true}, {"preRollAmount", 24000}});
	run(song.client, "audio.start");
	for (int i = 0; i < 10; ++i)
	{
		run(song.client, "transport.locate", {{"position", 48000}});
		playUntil(song, played(song) + 960); // the cue thread cues the cursor
		run(song.client, "transport.play");
		playUntil(song, played(song) + 2400);
		run(song.client, "transport.stop");
	}
	run(song.client, "audio.stop");
	CHECK(run(song.client, "audio.get").at("playbackDropouts") == 0);
}

TEST_CASE("recording from stop with a count-in starts without a dropout", "[integration][realtime]")
{
	// While the transport counts in, the cue thread cues where playback starts after it, half a second before the
	// cursor with the pre-roll, rather than at the cursor, where it would take the start's voice away.
	const gnubase::test::TempDir dir;
	Song song(256);
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "stereo", 2, 96000));
	song.add({track.id, 0, 0, 96000, 2}, clip);
	run(song.client, "transport.set", {{"preRoll", true}, {"preRollAmount", 24000}});
	// A sixteenth at 120 BPM: the count-in takes 6000 frames, so the cue thread cues during it.
	run(song.client, "metronome.set",
		{{"countIn",
			{{"enabled", true}, {"bars", 1}, {"signature", "custom"},
				{"customSignature", {{"numerator", 1}, {"denominator", 16}}}}}});
	run(song.client, "audio.start");
	for (int i = 0; i < 10; ++i)
	{
		run(song.client, "transport.locate", {{"position", 48000}});
		playUntil(song, played(song) + 960); // the cue thread cues the cursor
		run(song.client, "transport.record");
		playUntil(song, played(song) + 6000 + 2400);
		CHECK(run(song.client, "transport.get").at("playing") == true);
		run(song.client, "transport.stop");
	}
	run(song.client, "audio.stop");
	CHECK(run(song.client, "audio.get").at("playbackDropouts") == 0);
}

TEST_CASE("an audition that starts while another plays starts without a dropout", "[integration][realtime]")
{
	// audition.start twice without audition.stop: the cue thread, which cues the streams for the first, keeps away
	// from them until the second is out, so the second starts exactly at its position and plays on from there.
	const gnubase::test::TempDir dir;
	Song song(256);
	run(song.client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 20}});
	const TestTrack track = song.addTrack(2);
	const ObjectId clip = song.addClip(writeRamp(dir, "stereo", 2, 96000));
	song.add({track.id, 0, 0, 96000, 2}, clip);
	run(song.client, "audio.start");
	run(song.client, "audition.start", {{"track", track.id}, {"position", 0}});
	playUntil(song, played(song) + 9600);
	run(song.client, "audition.start", {{"track", track.id}, {"position", 48000}});
	playUntil(song, played(song) + 9600);
	run(song.client, "audition.stop");
	playUntil(song, played(song) + 2400);
	run(song.client, "audio.stop");

	// The output: silence, the ramp from frame 0 on, the ramp from frame 48000 on, silence; nothing left out.
	const std::vector<float> output = song.capture(0, played(song));
	size_t i = 0;
	while (i < output.size() && output[i] == 0.0f)
		++i;
	int64_t frame = 0;
	int64_t second = -1; // where the second audition started in the output
	for (; i < output.size() && output[i] != 0.0f; ++i)
	{
		if (output[i] == ramp(frame, 0))
		{
			++frame;
		}
		else if (second < 0 && output[i] == ramp(48000, 0))
		{
			second = static_cast<int64_t>(i);
			frame = 48001;
		}
		else
		{
			break;
		}
	}
	INFO("the ramp stops at output frame " << i << ", ramp frame " << frame);
	CHECK(second >= 0);
	CHECK(frame > 48000 + 4800);
	CHECK(std::all_of(output.begin() + static_cast<std::ptrdiff_t>(std::min(i, output.size())), output.end(),
		[](float sample) { return sample == 0.0f; }));
}
