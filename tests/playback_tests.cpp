// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/playback.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/graph/events.h"
#include "engine/graph/processor.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase::engine;
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

TEST_CASE("real-time playback renders the same as offline", "[integration]")
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
