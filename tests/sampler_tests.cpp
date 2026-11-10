// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/processors/builtin.h"
#include "engine/processors/sampler.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
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
	CHECK(sampler.activeVoices() == 1);
	CHECK(first(std::vector<float>(played.begin() + 10, played.end()), 2) == expected({0, 2}));
	bench.add(note(0, false, 72));
	played = bench.left(1);
	CHECK(first(played, 2) == expected({0, 1})); // 60 again, from the start
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
	CHECK(section.at("track").at("name") == "Sampler 03");
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

	CHECK_THAT(reject(client, "sampler.create", {{"clip", session.clip}, {"start", 0}, {"end", 48001}}),
		ContainsSubstring("within the clip"));
	CHECK_THAT(reject(client, "sampler.create", {{"clip", 999}}), ContainsSubstring("no clip 999"));
	CHECK_THAT(reject(client, "sampler.create", {{"start", 5}}), ContainsSubstring("need event or clip"));

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

	const auto audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "sampler.load", {{"track", audio}, {"clip", session.clip}}),
		ContainsSubstring("no sampler track"));
	CHECK_THAT(reject(client, "sampler.load", {{"track", track}}), ContainsSubstring("either event or clip"));

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
	const auto track = run(client, "sampler.create", {{"clip", session.clip}}).at("track").at("id");
	const Json normalized = run(client, "sampler.normalize", {{"track", track}, {"level", -6.0}});
	// The sine's peak is just under 0.5 (-6 dBFS), near its start.
	CHECK_THAT(instrumentParameter(normalized.at("channel"), Sampler::Gain), WithinAbs(0.0, 0.1));
	const Json full = run(client, "sampler.normalize", {{"track", track}});
	CHECK_THAT(instrumentParameter(full.at("channel"), Sampler::Gain), WithinAbs(6.02, 0.1));
	const auto empty = run(client, "sampler.create").at("track").at("id");
	CHECK_THAT(reject(client, "sampler.normalize", {{"track", empty}}), ContainsSubstring("no sample"));
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
	const gnubase::test::Audio phrase = render(client, 2 * 48000);
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
