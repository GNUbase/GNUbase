// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/sampleloader.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/processors/builtin.h"
#include "engine/processors/sampler.h"
#include "engine/time/tempomap.h"
#include "platform/log.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using namespace gnubase::engine::graph;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using processors::Sample;
using processors::Sampler;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr double g_pi = 3.14159265358979323846;

	// A mono sample whose frame i is (i + 1) / 1024: every frame different, so where a voice reads shows.
	std::unique_ptr<const Sample> ramp(int64_t frames, double sampleRate = g_sampleRate)
	{
		auto sample = std::make_unique<Sample>();
		sample->sampleRate = sampleRate;
		for (int64_t i = 0; i < frames; ++i)
			sample->left.push_back(static_cast<float>(i + 1) / 1024.0f);
		return sample;
	}

	float rampAt(double frame)
	{
		return static_cast<float>(frame + 1.0) / 1024.0f;
	}

	ProcessEvent parameter(uint32_t offset, uint32_t id, double value)
	{
		ProcessEvent event;
		event.offset = offset;
		event.type = ProcessEvent::Type::Parameter;
		event.parameter = id;
		event.parameterValue = value;
		return event;
	}

	ProcessEvent note(uint32_t offset, bool on, uint8_t pitch, uint16_t velocity = 0xffff)
	{
		ProcessEvent event;
		event.offset = offset;
		event.type = on ? ProcessEvent::Type::NoteOn : ProcessEvent::Type::NoteOff;
		event.key = pitch;
		event.velocity = velocity;
		return event;
	}

	// Runs a sampler on its own: stereo blocks with the given events.
	class Bench
	{
	public:
		explicit Bench(Sampler& sampler, uint32_t frames = 64) : m_sampler(sampler), m_frames(frames)
		{
			sampler.prepareFor(g_sampleRate, frames);
			m_samples.resize(2 * static_cast<size_t>(frames));
			m_pointers = {m_samples.data(), m_samples.data() + frames};
			// No release, so note-offs end voices at once unless a test sets one.
			add(parameter(0, Sampler::Release, 0.0));
		}

		void add(const ProcessEvent& event) { REQUIRE(m_events.add(event)); }

		// Processes blocks and returns the left and right channels.
		std::pair<std::vector<float>, std::vector<float>> run(int blocks)
		{
			std::vector<float> left;
			std::vector<float> right;
			for (int b = 0; b < blocks; ++b)
			{
				m_out.clear();
				m_sampler.process({m_pointers.data(), 2, m_frames, m_events, m_out, TransportBlock{}});
				m_events.clear();
				left.insert(left.end(), m_samples.begin(), m_samples.begin() + m_frames);
				right.insert(right.end(), m_samples.begin() + m_frames, m_samples.end());
			}
			return {left, right};
		}

		std::vector<float> left(int blocks) { return run(blocks).first; }

	private:
		Sampler& m_sampler;
		uint32_t m_frames;
		std::vector<float> m_samples;
		std::vector<float*> m_pointers;
		EventBuffer m_events{g_maxEventsPerBlock};
		EventBuffer m_out{g_maxEventsPerBlock};
	};

	std::vector<float> expected(const std::vector<double>& frames)
	{
		std::vector<float> values;
		values.reserve(frames.size());
		for (const double frame : frames)
			values.push_back(rampAt(frame));
		return values;
	}

	std::vector<float> first(const std::vector<float>& values, size_t count)
	{
		return {values.begin(), values.begin() + static_cast<std::ptrdiff_t>(std::min(count, values.size()))};
	}
}

TEST_CASE("the sampler plays its sample unchanged at the root key and an octave faster an octave higher", "[unit]")
{
	Sampler sampler;
	sampler.setSample(ramp(256));
	Bench bench(sampler);
	bench.add(note(0, true, 60));
	const auto [left, right] = bench.run(2);
	CHECK(first(left, 4) == expected({0, 1, 2, 3}));
	CHECK(left == right);
	CHECK(left[127] == rampAt(127));

	Sampler higher;
	higher.setSample(ramp(256));
	Bench octave(higher);
	octave.add(note(0, true, 72));
	const std::vector<float> played = octave.left(3);
	CHECK(first(played, 4) == expected({0, 2, 4, 6}));
	// It ends with the sample: 128 frames at twice the speed.
	CHECK(played[127] == rampAt(254));
	CHECK(played[128] == 0.0f);
	CHECK(higher.activeVoices() == 0);
}

TEST_CASE(
	"the sampler tunes by octave, coarse and fine, follows the sample's rate and can play at a fixed pitch", "[unit]")
{
	SECTION("a sample at half the rate plays half as fast")
	{
		Sampler sampler;
		sampler.setSample(ramp(256, g_sampleRate / 2.0));
		Bench bench(sampler);
		bench.add(note(0, true, 72));
		CHECK(first(bench.left(1), 3) == expected({0, 1, 2}));
	}
	SECTION("octave and coarse")
	{
		Sampler sampler;
		sampler.setSample(ramp(256));
		Bench bench(sampler);
		bench.add(parameter(0, Sampler::Octave, 1.0));
		bench.add(parameter(0, Sampler::Coarse, -12.0));
		bench.add(note(0, true, 72));
		CHECK(first(bench.left(1), 3) == expected({0, 2, 4}));
	}
	SECTION("fixed pitch ignores the key")
	{
		Sampler sampler;
		sampler.setSample(ramp(256));
		Bench bench(sampler);
		bench.add(parameter(0, Sampler::FixedPitch, 1.0));
		bench.add(note(0, true, 40));
		CHECK(first(bench.left(1), 3) == expected({0, 1, 2}));
	}
	SECTION("the root key moves the pitch")
	{
		Sampler sampler;
		sampler.setSample(ramp(256));
		Bench bench(sampler);
		bench.add(parameter(0, Sampler::RootKey, 48.0));
		bench.add(note(0, true, 60));
		CHECK(first(bench.left(1), 3) == expected({0, 2, 4}));
	}
	SECTION("pitch bend over its range")
	{
		Sampler sampler;
		sampler.setSample(ramp(256));
		Bench bench(sampler);
		bench.add(parameter(0, Sampler::PitchBendRange, 12.0));
		ProcessEvent bend;
		bend.type = ProcessEvent::Type::PitchBend;
		bend.value = 0xffffffffU;
		bench.add(bend);
		bench.add(note(0, true, 60));
		const std::vector<float> played = bench.left(1);
		CHECK_THAT(played[1], WithinAbs(rampAt(2.0), 1e-6));
	}
}

TEST_CASE("the sampler plays only the keys in its key range", "[unit]")
{
	Sampler sampler;
	sampler.setSample(ramp(256));
	Bench bench(sampler);
	bench.add(parameter(0, Sampler::KeyLow, 50.0));
	bench.add(parameter(0, Sampler::KeyHigh, 70.0));
	bench.add(note(0, true, 49));
	bench.add(note(0, true, 71));
	bench.run(1);
	CHECK(sampler.activeVoices() == 0);
	bench.add(note(0, true, 50));
	bench.add(note(0, true, 70));
	bench.run(1);
	CHECK(sampler.activeVoices() == 2);
}

TEST_CASE("the sampler plays from its start to its end, reversed if set", "[unit]")
{
	SECTION("start and end")
	{
		Sampler sampler;
		sampler.setSample(ramp(100));
		Bench bench(sampler);
		bench.add(parameter(0, Sampler::Start, 0.25));
		bench.add(parameter(0, Sampler::End, 0.5));
		bench.add(note(0, true, 60));
		const std::vector<float> played = bench.left(1);
		CHECK(first(played, 2) == expected({25, 26}));
		CHECK(played[24] == rampAt(49));
		CHECK(played[25] == 0.0f);
	}
	SECTION("reverse")
	{
		Sampler sampler;
		sampler.setSample(ramp(100));
		Bench bench(sampler);
		bench.add(parameter(0, Sampler::Reverse, 1.0));
		bench.add(parameter(0, Sampler::Start, 0.25)); // from the end: frame 74 is the first
		bench.add(note(0, true, 60));
		CHECK(first(bench.left(1), 3) == expected({74, 73, 72}));
	}
}

TEST_CASE("the sampler's loop modes", "[unit]")
{
	// A sample of 32 frames, its loop from frame 8 to 16.
	const auto loopBench = [](Sampler& sampler, Sampler::Loop mode)
	{
		sampler.setSample(ramp(32));
		auto bench = std::make_unique<Bench>(sampler, 64);
		bench->add(parameter(0, Sampler::LoopMode, static_cast<double>(mode)));
		bench->add(parameter(0, Sampler::LoopStart, 0.25));
		bench->add(parameter(0, Sampler::LoopEnd, 0.5));
		bench->add(note(0, true, 60));
		return bench;
	};

	SECTION("no loop plays once")
	{
		Sampler sampler;
		const auto bench = loopBench(sampler, Sampler::Loop::None);
		const std::vector<float> played = bench->left(1);
		CHECK(played[31] == rampAt(31));
		CHECK(played[32] == 0.0f);
	}
	SECTION("continuous loops until the voice ends")
	{
		Sampler sampler;
		const auto bench = loopBench(sampler, Sampler::Loop::Continuous);
		const std::vector<float> played = bench->left(1);
		CHECK(first(played, 18) == expected({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 8, 9}));
		CHECK(played[63] == rampAt(8 + ((63 - 8) % 8)));
		bench->add(note(0, false, 60));
		CHECK(bench->left(1)[0] == 0.0f);
	}
	SECTION("alternate plays the loop forwards and backwards")
	{
		Sampler sampler;
		const auto bench = loopBench(sampler, Sampler::Loop::Alternate);
		const std::vector<float> played = bench->left(1);
		CHECK(std::vector<float>(played.begin() + 14, played.begin() + 26) ==
			expected({14, 15, 16, 15, 14, 13, 12, 11, 10, 9, 8, 9}));
	}
	SECTION("once loops one time, then plays to the end")
	{
		Sampler sampler;
		const auto bench = loopBench(sampler, Sampler::Loop::Once);
		const std::vector<float> played = bench->left(1);
		CHECK(std::vector<float>(played.begin() + 15, played.begin() + 18) == expected({15, 8, 9}));
		CHECK(played[39] == rampAt(31));
		CHECK(played[40] == 0.0f);
	}
	SECTION("until release loops while the key is held, then plays to the end")
	{
		Sampler sampler;
		const auto bench = loopBench(sampler, Sampler::Loop::UntilRelease);
		bench->add(parameter(0, Sampler::Release, 1000.0));
		bench->add(note(20, false, 60)); // at frame 12, on the second pass through the loop
		const std::vector<float> played = bench->left(1);
		CHECK(std::vector<float>(played.begin() + 15, played.begin() + 17) == expected({15, 8}));
		// After the release it plays on past the loop's end to the sample's, fading out slowly.
		CHECK_THAT(played[24], WithinAbs(rampAt(16), 1e-4));
		CHECK_THAT(played[39], WithinAbs(rampAt(31), 1e-4));
		CHECK(played[40] == 0.0f);
	}
	SECTION("one shot ignores loops and note-offs")
	{
		Sampler sampler;
		const auto bench = loopBench(sampler, Sampler::Loop::Continuous);
		bench->add(parameter(0, Sampler::OneShot, 1.0));
		bench->add(note(4, false, 60));
		const std::vector<float> played = bench->left(1);
		CHECK(played[16] == rampAt(16));
		CHECK(played[31] == rampAt(31));
		CHECK(played[32] == 0.0f);
	}
}

TEST_CASE("in slice mode each key from the root key plays a slice at the sample's pitch", "[unit]")
{
	Sampler sampler;
	sampler.setSample(ramp(64));
	Bench bench(sampler);
	bench.add(parameter(0, Sampler::PlaybackMode, 1.0));
	bench.add(parameter(0, Sampler::Slices, 4.0));
	bench.add(note(0, true, 59)); // below the root key: no slice
	bench.add(note(0, true, 62)); // the third slice, frames 32 to 47
	const std::vector<float> played = bench.left(1);
	CHECK(first(played, 2) == expected({32, 33}));
	CHECK(played[15] == rampAt(47));
	CHECK(played[16] == 0.0f);
	bench.add(note(0, true, 64));
	bench.run(1);
	CHECK(sampler.activeVoices() == 0);
}

TEST_CASE("in monophonic mode a new note takes over and a held note plays again when it ends", "[unit]")
{
	Sampler sampler;
	sampler.setSample(ramp(1000));
	Bench bench(sampler);
	bench.add(parameter(0, Sampler::Monophonic, 1.0));
	bench.add(note(0, true, 60));
	bench.add(note(10, true, 72));
	std::vector<float> played = bench.left(1);
	// The note taken over fades out in 5 ms (240 frames) instead of stopping with a click.
	CHECK(sampler.activeVoices() == 2);
	CHECK_THAT(played[10], WithinAbs(rampAt(0) + rampAt(10), 1e-4));
	CHECK_THAT(played[11], WithinAbs(rampAt(2) + (rampAt(11) * (1.0 - (2.0 / 240.0))), 1e-4));
	played = bench.left(4);
	CHECK(sampler.activeVoices() == 1);
	CHECK(played[255] == rampAt(2.0 * (54 + 255)));
	bench.add(note(0, false, 72));
	played = bench.left(5);
	CHECK(sampler.activeVoices() == 1);
	CHECK(played[300] == rampAt(300)); // 60 again, from the start
	bench.add(note(0, false, 60));
	bench.run(1);
	CHECK(sampler.activeVoices() == 0);

	SECTION("legato only changes the pitch")
	{
		bench.add(parameter(0, Sampler::Legato, 1.0));
		bench.add(note(0, true, 60));
		bench.add(note(10, true, 72));
		played = bench.left(1);
		CHECK(played[10] == rampAt(10));
		CHECK(played[11] == rampAt(12));
	}
}

TEST_CASE("notes held from polyphonic mode end when monophonic mode is switched on meanwhile", "[unit]")
{
	Sampler sampler;
	sampler.setSample(ramp(32));
	Bench bench(sampler);
	bench.add(parameter(0, Sampler::LoopMode, static_cast<double>(Sampler::Loop::Continuous)));
	bench.add(parameter(0, Sampler::LoopStart, 0.25));
	bench.add(parameter(0, Sampler::LoopEnd, 0.5));
	bench.add(note(0, true, 60));
	bench.add(note(0, true, 64));
	bench.add(note(0, true, 67));
	bench.run(1);
	CHECK(sampler.activeVoices() == 3);
	bench.add(parameter(0, Sampler::Monophonic, 1.0));
	bench.add(note(0, false, 64));
	bench.add(note(0, false, 60));
	bench.add(note(0, false, 67));
	bench.run(1);
	CHECK(sampler.activeVoices() == 0);
}

TEST_CASE("a legato note after switching to monophonic mode ends the other notes held from polyphonic mode", "[unit]")
{
	Sampler sampler;
	sampler.setSample(ramp(32));
	Bench bench(sampler);
	bench.add(parameter(0, Sampler::LoopMode, static_cast<double>(Sampler::Loop::Continuous)));
	bench.add(parameter(0, Sampler::LoopStart, 0.25));
	bench.add(parameter(0, Sampler::LoopEnd, 0.5));
	bench.add(note(0, true, 60));
	bench.add(note(0, true, 64));
	bench.run(1);
	CHECK(sampler.activeVoices() == 2);
	bench.add(parameter(0, Sampler::Monophonic, 1.0));
	bench.add(parameter(0, Sampler::Legato, 1.0));
	bench.add(note(0, true, 67));
	bench.run(5); // the note it doesn't take over fades out in 5 ms (240 frames)
	CHECK(sampler.activeVoices() == 1);
	// The keys still held from polyphonic mode are known as held: releasing goes back to the latest of them (#1180).
	bench.add(note(0, false, 67));
	bench.run(1);
	CHECK(sampler.activeVoices() == 1);
	bench.add(note(0, false, 64));
	bench.run(1);
	CHECK(sampler.activeVoices() == 1);
	bench.add(note(0, false, 60));
	bench.run(1);
	CHECK(sampler.activeVoices() == 0);
}

TEST_CASE("a monophonic note that took over a key held from polyphonic mode goes back to it when it ends", "[unit]")
{
	Sampler sampler;
	sampler.setSample(ramp(1000));
	Bench bench(sampler);
	bench.add(note(0, true, 60));
	bench.run(1);
	bench.add(parameter(0, Sampler::Monophonic, 1.0));
	bench.add(note(0, true, 72));
	bench.run(5); // 60 fades out in 5 ms (240 frames)
	CHECK(sampler.activeVoices() == 1);
	bench.add(note(0, false, 72));
	const std::vector<float> played = bench.left(5);
	CHECK(sampler.activeVoices() == 1);
	CHECK(played[300] == rampAt(300)); // 60 again, from the start
	bench.add(note(0, false, 60));
	bench.run(1);
	CHECK(sampler.activeVoices() == 0);
}

TEST_CASE("a key held from polyphonic mode that still sounds isn't played twice in monophonic mode", "[unit]")
{
	Sampler sampler;
	sampler.setSample(ramp(32));
	Bench bench(sampler);
	bench.add(parameter(0, Sampler::LoopMode, static_cast<double>(Sampler::Loop::Continuous)));
	bench.add(parameter(0, Sampler::LoopStart, 0.25));
	bench.add(parameter(0, Sampler::LoopEnd, 0.5));
	bench.add(note(0, true, 60));
	bench.add(note(0, true, 64));
	bench.run(1);
	bench.add(parameter(0, Sampler::Monophonic, 1.0));
	// 64 plays the monophonic note; once it ends, 60 sounds on in its own voice.
	bench.add(note(0, false, 64));
	bench.run(1);
	CHECK(sampler.activeVoices() == 1);
	bench.add(note(0, false, 60));
	bench.run(1);
	CHECK(sampler.activeVoices() == 0);
}

TEST_CASE("a key released in polyphonic mode doesn't play again when monophonic mode is back", "[unit]")
{
	Sampler sampler;
	sampler.setSample(ramp(32));
	Bench bench(sampler);
	bench.add(parameter(0, Sampler::LoopMode, static_cast<double>(Sampler::Loop::Continuous)));
	bench.add(parameter(0, Sampler::LoopStart, 0.25));
	bench.add(parameter(0, Sampler::LoopEnd, 0.5));
	bench.add(parameter(0, Sampler::Monophonic, 1.0));
	bench.add(note(0, true, 60));
	bench.run(1);
	bench.add(parameter(0, Sampler::Monophonic, 0.0));
	bench.add(note(0, false, 60));
	bench.run(1);
	CHECK(sampler.activeVoices() == 0);
	bench.add(parameter(0, Sampler::Monophonic, 1.0));
	bench.add(note(0, true, 64));
	bench.run(1);
	bench.add(note(0, false, 64));
	bench.run(1);
	CHECK(sampler.activeVoices() == 0);
}

TEST_CASE("a loop shorter than a step stays a loop at high pitches", "[unit]")
{
	// A loop of two frames, from frame 8 to 10, played five octaves up: 32 frames a step.
	const auto play = [](Sampler::Loop mode)
	{
		Sampler sampler;
		sampler.setSample(ramp(64));
		Bench bench(sampler);
		bench.add(parameter(0, Sampler::LoopMode, static_cast<double>(mode)));
		bench.add(parameter(0, Sampler::LoopStart, 0.125));
		bench.add(parameter(0, Sampler::LoopEnd, 10.0 / 64.0));
		bench.add(parameter(0, Sampler::Octave, 5.0));
		bench.add(note(0, true, 60));
		const std::vector<float> played = bench.left(2);
		CHECK(sampler.activeVoices() == 1);
		for (size_t i = 1; i < played.size(); ++i)
		{
			INFO("frame " << i);
			CHECK(played[i] >= rampAt(7.0) - 1e-6f);
			CHECK(played[i] <= rampAt(11.0) + 1e-6f);
		}
	};
	play(Sampler::Loop::Continuous);
	play(Sampler::Loop::Alternate);
}

TEST_CASE("drive is part of the filter: off with it", "[unit]")
{
	const auto peak = [](bool filter)
	{
		auto ones = std::make_unique<Sample>();
		ones->left.assign(4800, 0.9f);
		Sampler sampler;
		sampler.setSample(std::move(ones));
		Bench bench(sampler, 480);
		bench.add(parameter(0, Sampler::FilterOn, filter ? 1.0 : 0.0));
		bench.add(parameter(0, Sampler::Cutoff, 20000.0));
		bench.add(parameter(0, Sampler::Drive, 24.0));
		bench.add(note(0, true, 60));
		return bench.left(1)[479];
	};
	CHECK(peak(false) == 0.9f);
	CHECK(peak(true) > 0.99f); // tanh(0.9 * 15.8) is about 1, through a low pass that lets DC through
}

TEST_CASE("the sampler frees the sample it played once a hot swap is through", "[unit]")
{
	Sampler sampler;
	std::shared_ptr<const Sample> old = ramp(64);
	const std::weak_ptr<const Sample> played = old;
	sampler.setSample(std::move(old));
	Bench bench(sampler);
	bench.run(1);
	sampler.setSample(ramp(64));
	sampler.collect();
	CHECK(!played.expired()); // the audio thread may still play it
	bench.run(1);
	sampler.collect();
	CHECK(played.expired());
}

TEST_CASE("sharedSample reads a file once while something uses its sample", "[unit]")
{
	const gnubase::test::TempDir folder;
	gnubase::test::Audio audio;
	audio.channels = 2;
	audio.samples = {0.5f, -0.5f, 0.25f, -0.25f};
	gnubase::test::writeWav(folder.path() / "two.wav", audio);
	const std::string path = (folder.path() / "two.wav").u8string();
	std::shared_ptr<const Sample> first = processors::sharedSample(path);
	REQUIRE(first->frames() == 2);
	CHECK(first->right[1] == -0.25f);
	CHECK(processors::sharedSample(path) == first);
	const std::weak_ptr<const Sample> kept = first;
	first.reset();
	CHECK(kept.expired());
	CHECK(processors::sharedSample((folder.path() / "missing.wav").u8string())->frames() == 0);
}

TEST_CASE("the sample loader reads each file once on a thread of its own", "[unit]")
{
	std::mutex mutex;
	std::condition_variable wake;
	bool open = false;
	std::atomic<int> reads{0};
	std::atomic<int> loaded{0};
	SampleLoader loader([&] { ++loaded; },
		[&](const std::string& path)
		{
			std::unique_lock lock(mutex);
			wake.wait(lock, [&] { return open; });
			++reads;
			auto sample = std::make_shared<Sample>();
			sample->left.assign(path.size(), 0.5f);
			return std::shared_ptr<const Sample>(std::move(sample));
		});
	// Asking returns while the file is read; a file asked for again is read once.
	loader.request("a.wav");
	loader.request("a.wav");
	loader.request("bb.wav");
	CHECK(loader.sample("a.wav") == nullptr);
	CHECK(!loader.waitUntilRead(std::chrono::milliseconds(20)));
	{
		const std::scoped_lock lock(mutex);
		open = true;
	}
	wake.notify_all();
	REQUIRE(loader.waitUntilRead(std::chrono::seconds(30)));
	CHECK(reads == 2);
	CHECK(loaded == 2);
	const std::shared_ptr<const Sample> a = loader.sample("a.wav");
	REQUIRE(a != nullptr);
	CHECK(a->frames() == 5);
	loader.keepOnly({"bb.wav"});
	CHECK(loader.sample("a.wav") == nullptr);
	CHECK(loader.sample("bb.wav") != nullptr);
	loader.request("a.wav"); // read again once let go of
	REQUIRE(loader.waitUntilRead(std::chrono::seconds(30)));
	CHECK(reads == 3);
}

TEST_CASE("the sample loader drops the files waiting that nothing wants any more", "[unit]")
{
	std::mutex mutex;
	std::condition_variable wake;
	bool open = false;
	bool reading = false;
	std::vector<std::string> read;
	SampleLoader loader({},
		[&](const std::string& path)
		{
			std::unique_lock lock(mutex);
			reading = true;
			wake.notify_all();
			wake.wait(lock, [&] { return open; });
			read.push_back(path);
			return std::make_shared<const Sample>();
		});
	loader.request("a.wav");
	loader.request("b.wav");
	loader.request("c.wav");
	// Once a.wav is being read, b.wav waits, and isn't read once nothing wants it.
	{
		std::unique_lock lock(mutex);
		REQUIRE(wake.wait_for(lock, std::chrono::seconds(30), [&] { return reading; }));
	}
	loader.keepOnly({"c.wav"});
	{
		const std::scoped_lock lock(mutex);
		open = true;
	}
	wake.notify_all();
	REQUIRE(loader.waitUntilRead(std::chrono::seconds(30)));
	const std::scoped_lock lock(mutex);
	CHECK(read == std::vector<std::string>{"a.wav", "c.wav"});
	CHECK(loader.sample("b.wav") == nullptr);
	CHECK(loader.sample("c.wav") != nullptr);
}

TEST_CASE("a read that throws leaves the sample loader running with an empty sample", "[unit]")
{
	std::atomic<int> reads{0};
	SampleLoader loader({},
		[&](const std::string& path) -> std::shared_ptr<const Sample>
		{
			if (++reads == 1)
				throw std::runtime_error("can't read " + path);
			return ramp(8);
		});
	loader.request("bad.wav");
	loader.request("good.wav");
	REQUIRE(loader.waitUntilRead(std::chrono::seconds(30)));
	const std::shared_ptr<const Sample> bad = loader.sample("bad.wav");
	REQUIRE(bad != nullptr);
	CHECK(bad->frames() == 0);
	const std::shared_ptr<const Sample> good = loader.sample("good.wav");
	REQUIRE(good != nullptr);
	CHECK(good->frames() == 8);
}

TEST_CASE("a file asked for again while its callback runs, after it was let go, is read again", "[unit]")
{
	std::atomic<int> reads{0};
	std::atomic<int> calls{0};
	SampleLoader* self = nullptr;
	SampleLoader loader(
		[&]
		{
			// As an update on the command thread would, while the loader thread is still in the callback.
			if (++calls == 1)
			{
				self->keepOnly({});
				self->request("a.wav");
			}
		},
		[&](const std::string&)
		{
			++reads;
			return ramp(4);
		});
	self = &loader;
	loader.request("a.wav");
	REQUIRE(loader.waitUntilRead(std::chrono::seconds(30)));
	CHECK(reads == 2);
	CHECK(loader.sample("a.wav") != nullptr);
}

TEST_CASE("a callback that throws leaves the sample loader running", "[unit]")
{
	std::atomic<int> calls{0};
	SampleLoader loader(
		[&]
		{
			if (++calls == 1)
				throw std::runtime_error("no update");
		},
		[](const std::string&) { return ramp(4); });
	loader.request("a.wav");
	loader.request("b.wav");
	REQUIRE(loader.waitUntilRead(std::chrono::seconds(30)));
	CHECK(calls == 2);
	CHECK(loader.sample("a.wav") != nullptr);
	CHECK(loader.sample("b.wav") != nullptr);
}

TEST_CASE("the sample loader waits only for the files asked for, and its end for the file it reads", "[unit]")
{
	std::mutex mutex;
	std::condition_variable wake;
	bool open = false;
	bool reading = false;
	std::vector<std::string> read;
	auto loader = std::make_unique<SampleLoader>(std::function<void()>{},
		[&](const std::string& path)
		{
			std::unique_lock lock(mutex);
			if (path == "slow.wav")
			{
				reading = true;
				wake.notify_all();
				wake.wait(lock, [&] { return open; });
			}
			read.push_back(path);
			return ramp(4);
		});
	loader->request("slow.wav");
	{
		std::unique_lock lock(mutex);
		REQUIRE(wake.wait_for(lock, std::chrono::seconds(30), [&] { return reading; }));
	}
	loader->request("next.wav");
	// Not ending yet: the wait for the end gives up rather than hanging the run (#1337).
	CHECK(!loader->waitUntilEnding(std::chrono::milliseconds(0)));
	// No wait for a file that is read but not asked for (#1180); one for the file behind it.
	CHECK(loader->waitUntilRead(std::chrono::milliseconds(0), {"other.wav"}));
	CHECK(!loader->waitUntilRead(std::chrono::milliseconds(20), {"next.wav"}));

	// Its end waits for the file it reads and leaves the others unread. The read stays held back until the end has
	// begun, so that the check doesn't rely on the ending thread winning a race against a sleep (#1272).
	SampleLoader* const endingLoader = loader.get();
	std::thread ending([&loader] { loader.reset(); });
	// The read is let go whether the wait failed or not, so that a lost notify fails the test rather than hanging it.
	const bool ended = endingLoader->waitUntilEnding(std::chrono::seconds(30));
	{
		const std::scoped_lock lock(mutex);
		open = true;
	}
	wake.notify_all();
	ending.join();
	REQUIRE(ended);
	CHECK(read == std::vector<std::string>{"slow.wav"});
}

TEST_CASE("sharedSample reads a file asked for while it is read once", "[unit]")
{
	const gnubase::test::TempDir folder;
	gnubase::test::Audio audio;
	audio.samples.assign(static_cast<size_t>(10 * g_sampleRate), 0.25f); // long enough for the reads to meet
	gnubase::test::writeWav(folder.path() / "long.wav", audio);
	const std::string path = (folder.path() / "long.wav").u8string();
	std::array<std::shared_ptr<const Sample>, 4> samples;
	std::vector<std::thread> threads;
	threads.reserve(samples.size());
	for (std::shared_ptr<const Sample>& sample : samples)
		threads.emplace_back([&sample, &path] { sample = processors::sharedSample(path); });
	for (std::thread& thread : threads)
		thread.join();
	CHECK(samples[0]->frames() == static_cast<int64_t>(audio.samples.size()));
	for (const std::shared_ptr<const Sample>& sample : samples)
		CHECK(sample == samples[0]);
}

TEST_CASE("the sampler's amp section: volume, pan, sample gain, velocity and envelope", "[unit]")
{
	auto constant = std::make_unique<Sample>();
	constant->left.assign(48000, 0.5f);
	constant->right.assign(48000, 0.25f);
	Sampler sampler;
	sampler.setSample(std::move(constant));
	Bench bench(sampler, 480);
	bench.add(parameter(0, Sampler::Volume, -6.0));
	bench.add(parameter(0, Sampler::Gain, 6.0));
	bench.add(parameter(0, Sampler::Pan, 0.5));
	bench.add(note(0, true, 60, 0x8000));
	auto [left, right] = bench.run(1);
	const double velocity = 0x8000 / 65535.0;
	CHECK_THAT(left[0], WithinAbs(0.5 * velocity * 0.5, 1e-6));
	CHECK_THAT(right[0], WithinAbs(0.25 * velocity, 1e-6));

	SECTION("attack, decay to the sustain level, release")
	{
		Sampler shaped;
		auto ones = std::make_unique<Sample>();
		ones->left.assign(48000, 1.0f);
		shaped.setSample(std::move(ones));
		Bench envelope(shaped, 480);
		envelope.add(parameter(0, Sampler::Attack, 5.0)); // 240 frames
		envelope.add(parameter(0, Sampler::Decay, 5.0));  // 240 frames
		envelope.add(parameter(0, Sampler::Sustain, 50.0));
		envelope.add(parameter(0, Sampler::Release, 5.0)); // 240 frames
		envelope.add(note(0, true, 60));
		const std::vector<float> played = envelope.left(2);
		CHECK_THAT(played[119], WithinAbs(0.5, 0.01));
		CHECK_THAT(played[239], WithinAbs(1.0, 0.01));
		CHECK_THAT(played[359], WithinAbs(0.75, 0.01));
		CHECK_THAT(played[959], WithinAbs(0.5, 1e-9));
		envelope.add(note(0, false, 60));
		const std::vector<float> released = envelope.left(1);
		CHECK_THAT(released[119], WithinAbs(0.25, 0.01));
		CHECK(released[240] == 0.0f);
		CHECK(shaped.activeVoices() == 0);
	}
}

TEST_CASE("the sampler's filter: low pass with key follow, high pass and band pass", "[unit]")
{
	// A 5 kHz sine through each filter at 500 Hz: the low pass and band pass take it down, the high pass doesn't.
	const auto level = [](double type, double cutoff, uint8_t key, double keyFollow)
	{
		auto sine = std::make_unique<Sample>();
		for (int i = 0; i < 48000; ++i)
			sine->left.push_back(static_cast<float>(0.5 * std::sin(2.0 * g_pi * 5000.0 * i / g_sampleRate)));
		Sampler sampler;
		sampler.setSample(std::move(sine));
		Bench bench(sampler, 4800);
		bench.add(parameter(0, Sampler::FixedPitch, 1.0));
		bench.add(parameter(0, Sampler::FilterOn, 1.0));
		bench.add(parameter(0, Sampler::FilterType, type));
		bench.add(parameter(0, Sampler::Cutoff, cutoff));
		bench.add(parameter(0, Sampler::KeyFollow, keyFollow));
		bench.add(note(0, true, key));
		const std::vector<float> played = bench.left(2);
		float peak = 0.0f;
		for (size_t i = 4800; i < played.size(); ++i)
			peak = std::max(peak, std::abs(played[i]));
		return peak;
	};
	CHECK(level(0.0, 500.0, 60, 0.0) < 0.01f);
	CHECK(level(1.0, 500.0, 60, 0.0) > 0.45f);
	CHECK(level(2.0, 500.0, 60, 0.0) < 0.1f);
	CHECK(level(0.0, 20000.0, 60, 0.0) > 0.45f);
	// Three octaves up with full key follow, the cutoff is at 4 kHz: the sine comes through far louder.
	CHECK(level(0.0, 500.0, 96, 100.0) > 0.2f);
}

TEST_CASE("a sampler without a sample plays nothing, and a new sample plays from the next note", "[unit]")
{
	Sampler sampler;
	Bench bench(sampler);
	bench.add(note(0, true, 60));
	const std::vector<float> silent = bench.left(1);
	CHECK(std::all_of(silent.begin(), silent.end(), [](float value) { return value == 0.0f; }));
	CHECK(sampler.activeVoices() == 0);
	sampler.setSample(ramp(256));
	bench.add(note(0, true, 60));
	CHECK(first(bench.left(1), 2) == expected({0, 1}));
}

TEST_CASE("processor.list has the sampler, an instrument", "[unit]")
{
	const processors::BuiltinInfo* info = processors::findBuiltin("sampler");
	REQUIRE(info != nullptr);
	CHECK(info->instrument);
	CHECK(info->parameters.size() == Sampler::Gain + 1);
	for (size_t i = 0; i < info->parameters.size(); ++i)
		CHECK(info->parameters[i].id == i);
	CHECK(info->parameters[Sampler::LoopMode].options.size() == 6);
}

namespace
{
	namespace fs = std::filesystem;

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

	double instrumentParameter(const Json& channel, uint32_t id)
	{
		for (const Json& item : channel.at("instrumentParameters"))
			if (item.at("parameter") == id)
				return item.at("value").get<double>();
		return -1.0;
	}

	// A project with a mono sample in the pool: a 220 Hz sine, one second at 48 kHz, decaying.
	class Session
	{
	public:
		Session()
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 10}});
			gnubase::test::Audio audio;
			audio.channels = 1;
			for (int i = 0; i < 48000; ++i)
				audio.samples.push_back(static_cast<float>(
					0.5 * std::exp(-3.0 * i / g_sampleRate) * std::sin(2.0 * g_pi * 220.0 * i / g_sampleRate)));
			file = folder.path() / "sine.wav";
			gnubase::test::writeWav(file, audio);
			const Json media = run(client, "media.add", {{"path", file.u8string()}});
			clip = run(client, "clip.add", {{"media", media.at("media").at("id")}}).at("clip").at("id").get<ObjectId>();
		}

		Engine engine;
		Client client{engine};
		gnubase::test::TempDir folder;
		fs::path file;
		ObjectId clip = 0;
	};

	// Renders the main mix from the project start, interleaved stereo, after the notes that sounded before have
	// faded out.
	gnubase::test::Audio render(Client& client, int64_t frames)
	{
		const auto start = run(client, "audio.render", {{"frames", 12000}}).at("capturedFrames").get<int64_t>();
		run(client, "transport.locate", {{"position", 0}});
		run(client, "transport.play");
		run(client, "audio.render", {{"frames", frames}});
		run(client, "transport.stop");
		const Json parameters = {{"start", start}, {"frames", frames}};
		Json left = parameters;
		left["channel"] = 0;
		Json right = parameters;
		right["channel"] = 1;
		const auto l = run(client, "audio.capture", left).at("samples").get<std::vector<float>>();
		const auto r = run(client, "audio.capture", right).at("samples").get<std::vector<float>>();
		gnubase::test::Audio audio;
		audio.channels = 2;
		audio.sampleRate = static_cast<int>(g_sampleRate);
		for (size_t i = 0; i < l.size(); ++i)
		{
			audio.samples.push_back(l[i]);
			audio.samples.push_back(r[i]);
		}
		return audio;
	}

	// A part at the project start with notes: pitch, start and length in quarter notes (24000 frames at the
	// default 120 BPM).
	void addNotes(Client& client, ObjectId track, const std::vector<std::array<int64_t, 3>>& notes)
	{
		const int64_t quarter = gnubase::time::g_ticksPerQuarter;
		const Json part =
			run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 16 * quarter}}).at("part");
		Json list = Json::array();
		for (const auto& [pitch, position, length] : notes)
			list.push_back(
				{{"pitch", pitch}, {"position", position * quarter}, {"length", length * quarter}, {"velocity", 1.0}});
		run(client, "note.add", {{"part", part.at("id")}, {"notes", list}});
	}
}

TEST_CASE("sampler.create makes a sampler track from a clip, an event's section or nothing", "[integration]")
{
	Session session;
	Client& client = session.client;
	const Json empty = run(client, "sampler.create");
	CHECK(empty.at("track").at("name") == "Sampler 01");
	CHECK(empty.at("track").at("type") == "instrument");
	CHECK(empty.at("channel").at("instrument") == "sampler");
	CHECK(empty.at("channel").at("sample") == 0);

	const Json whole = run(client, "sampler.create", {{"clip", session.clip}, {"name", "Sine"}});
	CHECK(whole.at("track").at("name") == "Sine");
	CHECK(whole.at("channel").at("sample") == session.clip);
	CHECK(whole.at("channel").at("instrumentParameters").empty());

	const Json section = run(client, "sampler.create", {{"clip", session.clip}, {"start", 12000}, {"end", 36000}});
	CHECK(section.at("track").at("name") == "Sampler 02");
	CHECK(instrumentParameter(section.at("channel"), Sampler::Start) == 0.25);
	CHECK(instrumentParameter(section.at("channel"), Sampler::End) == 0.75);

	// From an audio event: its section of the clip.
	const auto audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id");
	const auto event = run(client, "audioEvent.add",
		{{"track", audio}, {"clip", session.clip}, {"position", 0}, {"offset", 24000}, {"length", 24000}})
						   .at("event")
						   .at("id");
	const Json fromEvent = run(client, "sampler.create", {{"event", event}});
	CHECK(fromEvent.at("channel").at("sample") == session.clip);
	CHECK(instrumentParameter(fromEvent.at("channel"), Sampler::Start) == 0.5);
	CHECK(instrumentParameter(fromEvent.at("channel"), Sampler::End) == -1.0); // the default, the clip's end

	// The first number no track has: with Sampler 01 gone, Sampler 01 again, then Sampler 04.
	run(client, "track.remove", {{"track", empty.at("track").at("id")}});
	CHECK(run(client, "sampler.create").at("track").at("name") == "Sampler 01");
	CHECK(run(client, "sampler.create").at("track").at("name") == "Sampler 04");

	CHECK_THAT(reject(client, "sampler.create", {{"clip", session.clip}, {"start", 0}, {"end", 48001}}),
		ContainsSubstring("within the clip"));
	CHECK_THAT(reject(client, "sampler.create", {{"clip", 999}}), ContainsSubstring("no clip 999"));
	CHECK_THAT(reject(client, "sampler.create", {{"start", 5}}), ContainsSubstring("need event, clip or path"));

	// From a file, which goes into the pool, below a track and into a bus as track.add places tracks.
	const Json project = run(client, "project.get");
	Json output;
	for (const Json& bus : project.at("busses"))
		if (bus.at("direction") == "output")
			output = bus.at("id");
	const Json fromFile = run(client, "sampler.create",
		{{"path", session.file.u8string()}, {"parent", 0}, {"index", 0}, {"output", output}, {"start", 4800}});
	CHECK(fromFile.at("channel").at("sample") != session.clip);
	CHECK(fromFile.at("channel").at("output") == output);
	CHECK(instrumentParameter(fromFile.at("channel"), Sampler::Start) == 0.1);
	const Json withFile = run(client, "project.get");
	CHECK(withFile.at("tracks").at(0).at("id") == fromFile.at("track").at("id"));
	CHECK(withFile.at("clips").size() == project.at("clips").size() + 1);
	CHECK_THAT(reject(client, "sampler.create", {{"path", session.file.u8string()}, {"clip", session.clip}}),
		ContainsSubstring("one of event, clip and path"));
	CHECK(!run(client, "history.get").at("steps").empty());
	run(client, "history.undo"); // the clip and media file go with the track
	CHECK(run(client, "project.get").at("clips").size() == project.at("clips").size());

	// One undo step each.
	const size_t tracks = run(client, "project.get").at("tracks").size();
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("tracks").size() == tracks - 1);
}

TEST_CASE("sampler.load swaps the sample, resetting the settings unless they are kept", "[integration]")
{
	Session session;
	Client& client = session.client;
	const Json created = run(client, "sampler.create");
	const auto track = created.at("track").at("id").get<ObjectId>();
	const auto channel = created.at("channel").at("id").get<ObjectId>();
	const auto setParameter = [&](uint32_t id, double value)
	{
		run(client, "parameter.set",
			{{"channel", channel}, {"target", {{"type", "instrument"}, {"parameter", id}}}, {"value", value}});
	};
	setParameter(Sampler::Volume, -12.0);
	setParameter(Sampler::RootKey, 48.0);

	Json loaded = run(client, "sampler.load", {{"track", track}, {"clip", session.clip}, {"keepSettings", true}});
	CHECK(loaded.at("channel").at("sample") == session.clip);
	CHECK(instrumentParameter(loaded.at("channel"), Sampler::Volume) == -12.0);
	CHECK(instrumentParameter(loaded.at("channel"), Sampler::RootKey) == -1.0);

	setParameter(Sampler::Volume, -12.0);
	loaded = run(client, "sampler.load", {{"track", track}, {"clip", session.clip}, {"end", 24000}});
	CHECK(instrumentParameter(loaded.at("channel"), Sampler::Volume) == -1.0);
	CHECK(instrumentParameter(loaded.at("channel"), Sampler::End) == 0.5);

	// A file goes into the pool first, in the same undo step.
	const size_t clips = run(client, "project.get").at("clips").size();
	loaded = run(client, "sampler.load", {{"track", track}, {"path", session.file.u8string()}});
	CHECK(loaded.at("channel").at("sample") != session.clip);
	CHECK(run(client, "project.get").at("clips").size() == clips + 1);
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("clips").size() == clips);
	CHECK_THAT(
		reject(client, "sampler.load", {{"track", track}, {"path", (session.folder.path() / "none.wav").u8string()}}),
		ContainsSubstring("none.wav"));

	const auto audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "sampler.load", {{"track", audio}, {"clip", session.clip}}),
		ContainsSubstring("no sampler track"));
	CHECK_THAT(reject(client, "sampler.load", {{"track", track}}), ContainsSubstring("one of event, clip and path"));

	// Another instrument drops the sample.
	run(client, "channel.set", {{"channel", channel}, {"instrument", "testSynth"}});
	const Json channels = run(client, "project.get").at("channels");
	for (const Json& item : channels)
		if (item.at("id") == channel)
			CHECK(item.at("sample") == 0);
}

TEST_CASE("sampler.normalize sets the sample gain so that the peak reaches the level", "[integration]")
{
	Session session;
	Client& client = session.client;
	const Json created = run(client, "sampler.create", {{"clip", session.clip}});
	const auto track = created.at("track").at("id");
	const Json normalized = run(client, "sampler.normalize", {{"track", track}, {"level", -6.0}});
	// The sine's peak is just under 0.5 (-6 dBFS), near its start.
	CHECK_THAT(instrumentParameter(normalized.at("channel"), Sampler::Gain), WithinAbs(0.0, 0.1));
	const Json full = run(client, "sampler.normalize", {{"track", track}});
	CHECK_THAT(instrumentParameter(full.at("channel"), Sampler::Gain), WithinAbs(6.02, 0.1));

	// Reversed, Start and End count from the sample's end: Start 0.5 plays the loud first half, End 0.5 the second
	// half, about 13 dB quieter (the sine decays by exp(-1.5) in half a second).
	const auto channel = created.at("channel").at("id");
	const auto setParameter = [&](uint32_t id, double value)
	{
		run(client, "parameter.set",
			{{"channel", channel}, {"target", {{"type", "instrument"}, {"parameter", id}}}, {"value", value}});
	};
	setParameter(Sampler::Reverse, 1.0);
	setParameter(Sampler::Start, 0.5);
	const Json reversed = run(client, "sampler.normalize", {{"track", track}});
	CHECK_THAT(instrumentParameter(reversed.at("channel"), Sampler::Gain), WithinAbs(6.02, 0.1));
	setParameter(Sampler::Start, 0.0);
	setParameter(Sampler::End, 0.5);
	const Json reversedEnd = run(client, "sampler.normalize", {{"track", track}});
	CHECK_THAT(instrumentParameter(reversedEnd.at("channel"), Sampler::Gain), WithinAbs(6.02 + 13.03, 0.2));

	const auto empty = run(client, "sampler.create").at("track").at("id");
	CHECK_THAT(reject(client, "sampler.normalize", {{"track", empty}}), ContainsSubstring("no sample"));

	// A missing file isn't silent.
	fs::remove(session.file);
	CHECK_THAT(reject(client, "sampler.normalize", {{"track", track}}), ContainsSubstring("missing"));
}

TEST_CASE("a track preset with another instrument takes the sampler's sample away", "[integration]")
{
	const gnubase::test::TempDir settings;
	Engine engine(gnubase::audio::makeBackends(), settings.path() / "settings");
	Client client(engine);
	const auto synth = run(client, "track.add", {{"type", "instrument"}, {"instrument", "testSynth"}});
	run(client, "track.savePreset", {{"tracks", Json::array({synth.at("track").at("id")})}, {"name", "Synth"}});
	gnubase::test::Audio audio;
	audio.samples.assign(480, 0.5f);
	gnubase::test::writeWav(settings.path() / "dc.wav", audio);
	const Json media = run(client, "media.add", {{"path", (settings.path() / "dc.wav").u8string()}});
	const auto clip = run(client, "clip.add", {{"media", media.at("media").at("id")}}).at("clip").at("id");
	const Json sampler = run(client, "sampler.create", {{"clip", clip}});
	CHECK(run(client, "pool.get").at("clips").at(0).at("used") == 1);
	run(client, "track.loadPreset", {{"tracks", Json::array({sampler.at("track").at("id")})}, {"name", "Synth"}});
	const Json project = run(client, "project.get");
	for (const Json& item : project.at("channels"))
		if (item.at("id") == sampler.at("channel").at("id"))
		{
			CHECK(item.at("instrument") == "testSynth");
			CHECK(item.at("sample") == 0);
		}
	CHECK(run(client, "pool.get").at("clips").at(0).at("used") == 0);
}

TEST_CASE(
	"a sampler track's clip counts as used in the pool, and the sample is saved with the project", "[integration]")
{
	Session session;
	Client& client = session.client;
	const Json created = run(client, "sampler.create", {{"clip", session.clip}, {"start", 4800}});
	const Json pool = run(client, "pool.get");
	CHECK(pool.at("clips").at(0).at("used") == 1);
	CHECK(run(client, "pool.removeUnused").at("clips").empty());

	const std::string path = (session.folder.path() / "sampler.dawproject").u8string();
	run(client, "project.save", {{"path", path}});
	run(client, "project.new");
	run(client, "project.load", {{"path", path}});
	const auto channel = created.at("channel").at("id");
	const Json loaded = run(client, "project.get");
	for (const Json& item : loaded.at("channels"))
		if (item.at("id") == channel)
		{
			CHECK(item.at("sample") == session.clip);
			CHECK(instrumentParameter(item, Sampler::Start) == 0.1);
		}
}

TEST_CASE("a sampler's clip can't be removed, and a missing one leaves the sampler empty", "[integration]")
{
	Session session;
	Client& client = session.client;
	const Json created = run(client, "sampler.create", {{"clip", session.clip}});
	const auto track = created.at("track").at("id");
	CHECK_THAT(reject(client, "clip.remove", {{"clip", session.clip}}), ContainsSubstring("sampler track"));

	fs::remove(session.file);
	const Json removed = run(client, "media.removeMissing");
	CHECK(removed.at("clips") == Json::array({session.clip}));
	const Json project = run(client, "project.get");
	for (const Json& item : project.at("channels"))
		if (item.at("id") == created.at("channel").at("id"))
			CHECK(item.at("sample") == 0);
	CHECK_THAT(reject(client, "sampler.normalize", {{"track", track}}), ContainsSubstring("no sample"));
	run(client, "history.undo");
	CHECK(run(client, "pool.get").at("clips").at(0).at("used") == 1);
}

TEST_CASE("track.export and track.import take a sampler track's sample along", "[integration]")
{
	Session session;
	Client& client = session.client;
	const Json created = run(client, "sampler.create", {{"clip", session.clip}, {"start", 4800}});
	const fs::path archive = session.folder.path() / "sampler.dawproject";
	run(client, "track.export",
		{{"tracks", {created.at("track").at("id")}}, {"path", archive.u8string()}, {"copyMedia", false}});
	const std::string project = (session.folder.path() / "project.dawproject").u8string();
	run(client, "project.save", {{"path", project}});

	// Into a project whose own clips have other ids.
	for (const std::string& path : {archive.u8string(), project})
	{
		INFO(path);
		run(client, "project.new");
		gnubase::test::Audio other;
		other.samples.assign(480, 0.0f);
		const fs::path silent = session.folder.path() / "other.wav";
		gnubase::test::writeWav(silent, other);
		const Json media = run(client, "media.add", {{"path", silent.u8string()}});
		const auto mine = run(client, "clip.add", {{"media", media.at("media").at("id")}}).at("clip").at("id");
		run(client, "track.import", {{"path", path}});
		const Json loaded = run(client, "project.get");
		const auto channel = std::find_if(loaded.at("channels").begin(), loaded.at("channels").end(),
			[](const Json& item) { return item.at("instrument") == "sampler"; });
		REQUIRE(channel != loaded.at("channels").end());
		CHECK(channel->at("sample") != mine);
		CHECK(instrumentParameter(*channel, Sampler::Start) == 0.1);
		// The sampler's clip plays the sine, a second long.
		int64_t frames = 0;
		for (const Json& clip : loaded.at("clips"))
			if (clip.at("id") == channel->at("sample"))
				for (const Json& file : loaded.at("media"))
					if (file.at("id") == clip.at("media"))
						frames = file.at("length").get<int64_t>();
		CHECK(frames == 48000);
	}
}

TEST_CASE("a sampler track renders its sample from MIDI notes, and swaps it while playing", "[integration]")
{
	Session session;
	Client& client = session.client;
	const Json created = run(client, "sampler.create", {{"clip", session.clip}});
	const auto track = created.at("track").at("id").get<ObjectId>();
	const auto channel = created.at("channel").at("id").get<ObjectId>();
	const auto setParameter = [&](uint32_t id, double value)
	{
		run(client, "parameter.set",
			{{"channel", channel}, {"target", {{"type", "instrument"}, {"parameter", id}}}, {"value", value}});
	};

	// At the root key the sample plays as it is, from the note's start.
	addNotes(client, track, {{60, 0, 1}});
	const gnubase::test::Audio original = render(client, 9600);
	const gnubase::test::Audio sample = gnubase::test::readWav(session.file);
	for (size_t frame = 0; frame < 9600; frame += 97)
	{
		INFO("frame " << frame);
		CHECK_THAT(original.samples[2 * frame], WithinAbs(sample.samples[frame], 1e-6));
		CHECK(original.samples[(2 * frame) + 1] == original.samples[2 * frame]);
	}

	// A phrase with a looped, filtered and panned sample, then a fifth and an octave up.
	setParameter(Sampler::LoopMode, 1.0);
	setParameter(Sampler::LoopStart, 0.1);
	setParameter(Sampler::LoopEnd, 0.2);
	setParameter(Sampler::FilterOn, 1.0);
	setParameter(Sampler::Cutoff, 1500.0);
	setParameter(Sampler::Resonance, 40.0);
	setParameter(Sampler::Attack, 20.0);
	setParameter(Sampler::Release, 100.0);
	setParameter(Sampler::Pan, -0.3);
	addNotes(client, track, {{67, 2, 1}, {72, 3, 2}});
	const gnubase::test::Audio phrase = render(client, int64_t{2} * 48000);
	const gnubase::test::AudioDiff diff = gnubase::test::compareWithGolden("sampler-track-phrase", phrase, 1e-5f);
	INFO(diff.report);
	CHECK(diff.equal);

	// Loading another sample swaps it in the instrument that plays: silence here.
	gnubase::test::Audio silence;
	silence.samples.assign(4800, 0.0f);
	const fs::path silent = session.folder.path() / "silence.wav";
	gnubase::test::writeWav(silent, silence);
	const Json media = run(client, "media.add", {{"path", silent.u8string()}});
	const Json clip = run(client, "clip.add", {{"media", media.at("media").at("id")}}).at("clip").at("id");
	run(client, "sampler.load", {{"track", track}, {"clip", clip}});
	const gnubase::test::Audio swapped = render(client, 4800);
	CHECK(std::all_of(swapped.samples.begin(), swapped.samples.end(), [](float value) { return value == 0.0f; }));
	run(client, "history.undo");
	CHECK(render(client, 4800).samples == std::vector<float>(phrase.samples.begin(), phrase.samples.begin() + 9600));
}

namespace
{
	// Reads the samplers' samples as the engine does, but holds the read of one file back until release.
	class HeldRead
	{
	public:
		HeldRead(Engine& engine, const fs::path& file) : m_state(std::make_shared<State>())
		{
			m_state->file = file.filename();
			engine.setSampleReader(
				[state = m_state](const std::string& path)
				{
					if (fs::u8path(path).filename() == state->file)
					{
						std::unique_lock lock(state->mutex);
						state->reading = true;
						state->wake.notify_all();
						state->wake.wait(lock, [&state] { return state->open; });
					}
					std::shared_ptr<const processors::Sample> sample = processors::sharedSample(path);
					const std::scoped_lock lock(state->mutex);
					state->reads.push_back(fs::u8path(path).filename());
					return sample;
				});
		}
		~HeldRead() { release(); } // so that the engine's loader can stop
		HeldRead(const HeldRead&) = delete;
		HeldRead& operator=(const HeldRead&) = delete;
		HeldRead(HeldRead&&) = delete;
		HeldRead& operator=(HeldRead&&) = delete;

		// Whether the file is being read, waiting for it at most 30 s.
		bool reading()
		{
			std::unique_lock lock(m_state->mutex);
			return m_state->wake.wait_for(lock, std::chrono::seconds(30), [this] { return m_state->reading; });
		}

		// The names of the files read so far, in order.
		std::vector<fs::path> reads()
		{
			const std::scoped_lock lock(m_state->mutex);
			return m_state->reads;
		}

		void release()
		{
			{
				const std::scoped_lock lock(m_state->mutex);
				m_state->open = true;
			}
			m_state->wake.notify_all();
		}

	private:
		struct State
		{
			fs::path file;
			std::mutex mutex;
			std::condition_variable wake;
			bool reading = false;
			bool open = false;
			std::vector<fs::path> reads;
		};
		std::shared_ptr<State> m_state;
	};

	// The dummy device's output frames so far, on its own thread in real time (audio.start).
	int64_t captured(Client& client)
	{
		return run(client, "audio.get").at("capturedFrames").get<int64_t>();
	}

	// Waits until the device has played frames more than from, at most 20 s, and returns the frames played.
	int64_t playOn(Client& client, int64_t from, int64_t frames)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (captured(client) < from + frames && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return captured(client);
	}

	// Whether the main mix's left channel is silent from frame start to end.
	bool silent(Client& client, int64_t start, int64_t end)
	{
		const auto samples = run(client, "audio.capture", {{"channel", 0}, {"start", start}, {"frames", end - start}})
								 .at("samples")
								 .get<std::vector<float>>();
		return std::all_of(samples.begin(), samples.end(), [](float value) { return value == 0.0f; });
	}

	// Waits until the main mix sounds after frame from, while the device plays on, at most 20 s.
	bool soundsAfter(Client& client, int64_t from)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const int64_t end = captured(client);
			if (end > from && !silent(client, from, end))
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return false;
	}

	// Waits until the main mix's left channel is silent for 4800 frames after frame from, while the device plays on, at
	// most 20 s; returns the frame where that silence ends, or -1.
	int64_t silentAfter(Client& client, int64_t from)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const int64_t end = captured(client);
			if (end >= from + 4800 && silent(client, end - 4800, end))
				return end;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return -1;
	}

	// A new pool clip of a file with audio.
	ObjectId addClip(Client& client, const fs::path& file, const gnubase::test::Audio& audio)
	{
		gnubase::test::writeWav(file, audio);
		const Json media = run(client, "media.add", {{"path", file.u8string()}});
		return run(client, "clip.add", {{"media", media.at("media").at("id")}}).at("clip").at("id").get<ObjectId>();
	}

	// A quarter note at the root key on every quarter of 16, so that the sampler starts its sample again and again.
	void addRepeatedNotes(Client& client, ObjectId track)
	{
		std::vector<std::array<int64_t, 3>> notes;
		notes.reserve(16);
		for (int64_t quarter = 0; quarter < 16; ++quarter)
			notes.push_back({60, quarter, 1});
		addNotes(client, track, notes);
	}
}

TEST_CASE("a sampler gets the sample read while it plays from the posted update alone", "[integration][slow][realtime]")
{
	Session session;
	Client& client = session.client;
	HeldRead held(session.engine, session.file);
	const auto track = run(client, "sampler.create").at("track").at("id").get<ObjectId>();
	addRepeatedNotes(client, track);
	run(client, "audio.start");
	run(client, "transport.play");

	// sampler.load answers while the file is read on the loader's thread, and later commands run.
	run(client, "sampler.load", {{"track", track}, {"clip", session.clip}});
	REQUIRE(held.reading());
	run(client, "transport.get");
	// Meanwhile the sampler plays the sample it had: none.
	const int64_t start = captured(client);
	const int64_t played = playOn(client, start, 9600);
	CHECK(silent(client, start, played));

	// Once the file is read, the audio.updateSamples the loader posts hands the sample over: no play, locate or
	// render runs in between.
	held.release();
	CHECK(soundsAfter(client, played));
	run(client, "audio.stop");
}

TEST_CASE("a sampler of a project loaded plays nothing until its own sample is read", "[integration][slow][realtime]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "sampler.create", {{"clip", session.clip}}).at("track").at("id").get<ObjectId>();
	addRepeatedNotes(client, track);
	const fs::path first = session.folder.path() / "first.dawproject";
	run(client, "project.save", {{"path", first.u8string()}});
	// A second version of the project, whose sampler, on the channel of the same id, plays another file.
	gnubase::test::Audio audio = gnubase::test::readWav(session.file);
	for (float& value : audio.samples)
		value = -value;
	const fs::path other = session.folder.path() / "other.wav";
	gnubase::test::writeWav(other, audio);
	const Json media = run(client, "media.add", {{"path", other.u8string()}});
	const Json clip = run(client, "clip.add", {{"media", media.at("media").at("id")}}).at("clip").at("id");
	run(client, "sampler.load", {{"track", track}, {"clip", clip}});
	const fs::path second = session.folder.path() / "second.dawproject";
	run(client, "project.save", {{"path", second.u8string()}});

	// The first version, its sample read.
	run(client, "project.load", {{"path", first.u8string()}});
	run(client, "audio.render", {{"frames", 64}});
	// The second one while its file is read: the first version's sample doesn't play meanwhile.
	HeldRead held(session.engine, other);
	run(client, "project.load", {{"path", second.u8string()}});
	REQUIRE(held.reading());
	run(client, "audio.start");
	run(client, "transport.play"); // waits for the samples in vain
	const int64_t start = captured(client);
	const int64_t played = playOn(client, start, 9600);
	CHECK(silent(client, start, played));
	held.release();
	CHECK(soundsAfter(client, played));
	run(client, "audio.stop");
}

TEST_CASE("a sampler that sounds plays its sample on while the one loaded is read", "[integration][slow][realtime]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "sampler.create", {{"clip", session.clip}}).at("track").at("id").get<ObjectId>();
	addRepeatedNotes(client, track);
	run(client, "audio.render", {{"frames", 64}}); // the sine is read
	gnubase::test::Audio silence;
	silence.samples.assign(4800, 0.0f);
	const fs::path quiet = session.folder.path() / "silence.wav";
	const ObjectId clip = addClip(client, quiet, silence);
	HeldRead held(session.engine, quiet);
	run(client, "audio.start");
	run(client, "transport.play");
	const int64_t playing = captured(client);

	// The hot swap of a sampler of the same project has no gap: the sine plays on until the new file is read.
	run(client, "sampler.load", {{"track", track}, {"clip", clip}});
	REQUIRE(held.reading());
	const int64_t start = captured(client);
	const int64_t played = playOn(client, start, 9600);
	CHECK(!silent(client, start, played));
	held.release();
	const int64_t quietEnd = silentAfter(client, played);
	CHECK(quietEnd > 0);
	CHECK(quietEnd < playing + (int64_t{16} * 24000)); // before the notes end
	run(client, "audio.stop");
}

TEST_CASE("a project loaded while the one before still reads a sample plays its own sample", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "sampler.create", {{"clip", session.clip}}).at("track").at("id").get<ObjectId>();
	addNotes(client, track, {{60, 0, 1}});
	const fs::path first = session.folder.path() / "first.dawproject";
	run(client, "project.save", {{"path", first.u8string()}});
	gnubase::test::Audio inverted = gnubase::test::readWav(session.file);
	for (float& value : inverted.samples)
		value = -value;
	const ObjectId clip = addClip(client, session.folder.path() / "inverted.wav", inverted);
	run(client, "sampler.load", {{"track", track}, {"clip", clip}});
	run(client, "audio.render", {{"frames", 64}}); // read before the read is held
	const fs::path second = session.folder.path() / "second.dawproject";
	run(client, "project.save", {{"path", second.u8string()}});

	// The first project's sine is still being read when the second one loads: read late, it stays out.
	HeldRead held(session.engine, session.file);
	run(client, "project.load", {{"path", first.u8string()}});
	REQUIRE(held.reading());
	run(client, "project.load", {{"path", second.u8string()}});
	held.release();
	const gnubase::test::Audio rendered = render(client, 4800);
	for (size_t frame = 0; frame < 4800; frame += 97)
	{
		INFO("frame " << frame);
		CHECK_THAT(rendered.samples[2 * frame], WithinAbs(inverted.samples[frame], 1e-6));
	}
	CHECK(held.reads() == std::vector<fs::path>{"sine.wav", "inverted.wav"});
}

TEST_CASE(
	"starting playback waits for the samples, locating doesn't, and an undo drops a file waiting", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "sampler.create").at("track").at("id").get<ObjectId>();
	HeldRead held(session.engine, session.file);
	run(client, "sampler.load", {{"track", track}, {"clip", session.clip}});
	REQUIRE(held.reading());
	// Another file asked for behind it, then undone: it leaves the loader's queue unread.
	const gnubase::test::Audio audio = gnubase::test::readWav(session.file);
	const ObjectId undone = addClip(client, session.folder.path() / "undone.wav", audio);
	run(client, "sampler.load", {{"track", track}, {"clip", undone}});
	run(client, "history.undo");

	// Locating doesn't wait for a sample being read (#1180).
	const auto before = std::chrono::steady_clock::now();
	run(client, "transport.locate", {{"position", 4800}});
	CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
	// Starting playback does: it answers only after the release.
	const auto releaseStart = std::chrono::steady_clock::now();
	std::thread releasing(
		[&held]
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			held.release();
		});
	run(client, "transport.play");
	CHECK(std::chrono::steady_clock::now() - releaseStart >= std::chrono::milliseconds(100));
	releasing.join();
	CHECK(held.reads() == std::vector<fs::path>{"sine.wav"});
	run(client, "transport.stop");

	// A file asked for after the undo is read next, so the undone one was never read.
	const ObjectId later = addClip(client, session.folder.path() / "later.wav", audio);
	run(client, "sampler.load", {{"track", track}, {"clip", later}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(held.reads() == std::vector<fs::path>{"sine.wav", "later.wav"});
}

TEST_CASE("recording from stop waits for the samples like starting playback, a count-in doesn't", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "sampler.create").at("track").at("id").get<ObjectId>();
	HeldRead held(session.engine, session.file);
	run(client, "sampler.load", {{"track", track}, {"clip", session.clip}});
	REQUIRE(held.reading());

	// With a count-in, recording answers while the file is read: the count-in plays meanwhile.
	run(client, "metronome.set", {{"countIn", {{"enabled", true}}}});
	const auto before = std::chrono::steady_clock::now();
	run(client, "transport.record");
	CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
	CHECK(held.reads().empty());
	run(client, "transport.stop");

	// Without one, playback starts at once, so recording waits for the sample as transport.play does (#1200).
	run(client, "metronome.set", {{"countIn", {{"enabled", false}}}});
	const auto releaseStart = std::chrono::steady_clock::now();
	std::thread releasing(
		[&held]
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			held.release();
		});
	run(client, "transport.record");
	CHECK(std::chrono::steady_clock::now() - releaseStart >= std::chrono::milliseconds(100));
	releasing.join();
	CHECK(held.reads() == std::vector<fs::path>{"sine.wav"});
	run(client, "transport.stop");
}

TEST_CASE("an offline render logs a warning naming the file at every round of its wait for a sample", "[integration]")
{
	Session session;
	const fs::path logFile = session.folder.path() / "gnubase.log";
	const gnubase::logging::Session log(logFile);
	Client& client = session.client;
	const auto track = run(client, "sampler.create").at("track").at("id").get<ObjectId>();
	// Rounds of 50 ms instead of 30 s, and the read is held back until the log shows two warnings (#1407): their
	// count doesn't depend on when the render starts to wait, and the 20 rounds after which it gives up leave a
	// second for the polling.
	session.engine.setOfflineSampleWait(std::chrono::milliseconds(50));
	HeldRead held(session.engine, session.file);
	run(client, "sampler.load", {{"track", track}, {"clip", session.clip}});
	REQUIRE(held.reading());
	// The warnings in the log so far, each with the file it names.
	const auto warnings = [&logFile]
	{
		gnubase::logging::flush();
		const std::string text = gnubase::test::readText(logFile);
		const std::string warning = "The render still waits for the samplers' samples to be read: ";
		std::vector<std::string> files;
		for (size_t at = text.find(warning); at != std::string::npos; at = text.find(warning, at + 1))
		{
			const size_t start = at + warning.size();
			files.push_back(text.substr(start, text.find('\n', start) - start));
		}
		return files;
	};
	std::thread releasing(
		[&held, &warnings]
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (warnings().size() < 2 && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			held.release();
		});
	// Not run: a failed REQUIRE would throw past the joinable thread.
	const Result result = client.call("audio.render", {{"frames", 64}});
	releasing.join();
	INFO(result.error);
	CHECK(result.ok());
	CHECK(held.reads() == std::vector<fs::path>{"sine.wav"});
	const std::vector<std::string> files = warnings();
	CHECK(files.size() >= 2);
	for (const std::string& file : files)
		CHECK_THAT(file, ContainsSubstring("sine.wav"));
}

TEST_CASE("an offline render gives up with an error after its last round of waiting for a sample", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "sampler.create").at("track").at("id").get<ObjectId>();
	// 20 rounds of 20 ms instead of 30 s: a read that stalls fails the render after 400 ms, not 10 minutes.
	session.engine.setOfflineSampleWait(std::chrono::milliseconds(20));
	HeldRead held(session.engine, session.file);
	run(client, "sampler.load", {{"track", track}, {"clip", session.clip}});
	REQUIRE(held.reading());
	const auto before = std::chrono::steady_clock::now();
	const std::string error = reject(client, "audio.render", {{"frames", 64}});
	CHECK_THAT(error, ContainsSubstring("the render gave up waiting for the samplers' samples to be read: "));
	CHECK_THAT(error, ContainsSubstring("sine.wav")); // where to look (#1414)
	CHECK(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(400));
	// Once the read ends, a render works again.
	held.release();
	run(client, "audio.render", {{"frames", 64}});
	CHECK(held.reads() == std::vector<fs::path>{"sine.wav"});
}

TEST_CASE("an export that gives up waiting for a sample leaves no files and puts the transport back", "[integration]")
{
	// The render's limit fails mixdown.export inside the render, where the error leaves through what puts the
	// transport and the device back and removes the part files (#1414).
	Session session;
	Client& client = session.client;
	const auto track = run(client, "sampler.create").at("track").at("id").get<ObjectId>();
	addNotes(client, track, {{60, 0, 1}});
	session.engine.setOfflineSampleWait(std::chrono::milliseconds(20));
	HeldRead held(session.engine, session.file);
	run(client, "sampler.load", {{"track", track}, {"clip", session.clip}});
	REQUIRE(held.reading());
	run(client, "transport.locate", {{"position", 777}});
	run(client, "audio.start");
	const gnubase::test::TempDir out;
	const std::string error = reject(
		client, "mixdown.export", {{"folder", out.path().u8string()}, {"name", "mix"}, {"start", 0}, {"end", 4800}});
	CHECK_THAT(error, ContainsSubstring("the render gave up waiting for the samplers' samples to be read: "));
	CHECK_THAT(error, ContainsSubstring("sine.wav"));
	CHECK(fs::directory_iterator(out.path()) == fs::directory_iterator());
	const Json transport = run(client, "transport.get");
	CHECK(transport.at("position") == 777);
	CHECK(transport.at("playing") == false);
	// The device that ran runs again.
	CHECK(run(client, "audio.get").at("running") == true);
	const int64_t start = captured(client);
	CHECK(playOn(client, start, 480) >= start + 480);
	run(client, "audio.stop");
}

TEST_CASE("an offline render waits for a sample whose file takes long to read", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "sampler.create").at("track").at("id").get<ObjectId>();
	addNotes(client, track, {{60, 0, 1}});
	HeldRead held(session.engine, session.file);
	run(client, "sampler.load", {{"track", track}, {"clip", session.clip}});
	REQUIRE(held.reading());
	std::thread releasing(
		[&held]
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			held.release();
		});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(held.reads() == std::vector<fs::path>{"sine.wav"});
	releasing.join();
	// The update the loader posts runs on its own and stays out of the command log.
	run(client, "transport.get");
	const Json log = run(client, "engine.commandLog").at("commands");
	CHECK(std::none_of(
		log.begin(), log.end(), [](const Json& entry) { return entry.at("command") == "audio.updateSamples"; }));
	CHECK(std::any_of(log.begin(), log.end(), [](const Json& entry) { return entry.at("command") == "sampler.load"; }));
}
