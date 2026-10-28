// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/processors/builtin.h"
#include "engine/processors/gainpan.h"
#include "engine/processors/generators.h"
#include "engine/processors/testsynth.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace gnubase::engine;
using namespace gnubase::engine::graph;
using namespace gnubase::engine::processors;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr double g_pi = 3.14159265358979323846;

	// Runs a processor on its own: blocks of frames, the given events in the first block, silence as input.
	class Bench
	{
	public:
		Bench(Processor& processor, int channels, uint32_t frames)
			: m_processor(processor), m_samples(static_cast<size_t>(channels) * frames), m_channels(channels),
			  m_frames(frames)
		{
			processor.prepareFor(g_sampleRate, frames);
			for (int c = 0; c < channels; ++c)
				m_pointers.push_back(m_samples.data() + (static_cast<size_t>(c) * frames));
		}

		void add(const ProcessEvent& event) { REQUIRE(m_events.add(event)); }

		// Sets every input sample.
		void fill(float value) { std::fill(m_samples.begin(), m_samples.end(), value); }

		// Processes one block and returns it, channel after channel.
		std::vector<float> process()
		{
			m_out.clear();
			m_processor.process({m_pointers.data(), m_channels, m_frames, m_events, m_out, TransportBlock{}});
			m_events.clear();
			return m_samples;
		}

		std::vector<float> run(int blocks)
		{
			std::vector<float> all;
			for (int b = 0; b < blocks; ++b)
			{
				std::fill(m_samples.begin(), m_samples.end(), 0.0f);
				const std::vector<float> block = process();
				all.insert(all.end(), block.begin(), block.end());
			}
			return all;
		}

	private:
		Processor& m_processor;
		std::vector<float> m_samples;
		std::vector<float*> m_pointers;
		int m_channels;
		uint32_t m_frames;
		EventBuffer m_events{g_maxEventsPerBlock};
		EventBuffer m_out{g_maxEventsPerBlock};
	};

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

	float peak(const std::vector<float>& samples, size_t begin = 0, size_t end = SIZE_MAX)
	{
		float result = 0.0f;
		for (size_t i = begin; i < std::min(end, samples.size()); ++i)
			result = std::max(result, std::abs(samples[i]));
		return result;
	}
}

TEST_CASE("the sine generator writes a continuous sine to every channel", "[unit]")
{
	Sine sine;
	Bench bench(sine, 2, 64);
	bench.add(parameter(0, Sine::Frequency, 1000.0));
	const std::vector<float> first = bench.process();
	const std::vector<float> second = bench.process();

	for (uint32_t i = 0; i < 64; ++i)
	{
		const double expected = 0.5 * std::sin(2.0 * g_pi * 1000.0 * i / g_sampleRate);
		CHECK_THAT(first[i], WithinAbs(expected, 1e-6));
		CHECK(first[64 + i] == first[i]); // the right channel
		const double next = 0.5 * std::sin(2.0 * g_pi * 1000.0 * (64 + i) / g_sampleRate);
		CHECK_THAT(second[i], WithinAbs(next, 1e-6));
	}
}

TEST_CASE("parameter changes take effect at their sample", "[unit]")
{
	Sine sine;
	Bench bench(sine, 1, 64);
	bench.add(parameter(0, Sine::Frequency, 1000.0));
	bench.add(parameter(32, Sine::Level, 0.0));
	const std::vector<float> block = bench.process();
	CHECK(peak(block, 1, 32) > 0.1f);
	CHECK(peak(block, 32) == 0.0f);
}

TEST_CASE("the noise generator is deterministic and stays within its level", "[unit]")
{
	Noise first;
	Noise second;
	Bench a(first, 1, 256);
	Bench b(second, 1, 256);
	a.add(parameter(0, Noise::Level, 0.25));
	b.add(parameter(0, Noise::Level, 0.25));
	const std::vector<float> samples = a.run(4);
	CHECK(samples == b.run(4));
	CHECK(peak(samples) <= 0.25f);
	CHECK(peak(samples) > 0.2f);
	double sum = 0.0;
	for (const float sample : samples)
		sum += sample;
	CHECK(std::abs(sum / static_cast<double>(samples.size())) < 0.02);
}

TEST_CASE("the impulse generator writes single samples at its interval across blocks", "[unit]")
{
	Impulse impulse;
	Bench bench(impulse, 1, 40);
	bench.add(parameter(0, Impulse::Interval, 0.001)); // 48 samples
	bench.add(parameter(0, Impulse::Level, 0.75));
	const std::vector<float> samples = bench.run(5);
	for (size_t i = 0; i < samples.size(); ++i)
	{
		INFO("sample " << i);
		CHECK(samples[i] == (i % 48 == 0 ? 0.75f : 0.0f));
	}
}

TEST_CASE("gain and pan scale a stereo signal, with the center at unity", "[unit]")
{
	GainPan gainPan;
	Bench bench(gainPan, 2, 16);
	bench.fill(1.0f);
	CHECK(bench.process() == std::vector<float>(32, 1.0f));

	bench.fill(1.0f);
	bench.add(parameter(0, GainPan::Gain, 20.0 * std::log10(0.5)));
	bench.add(parameter(8, GainPan::Pan, 1.0));
	const std::vector<float> block = bench.process();
	for (size_t i = 0; i < 16; ++i)
	{
		INFO("frame " << i);
		CHECK_THAT(block[i], WithinAbs(i < 8 ? 0.5 : 0.0, 1e-6));
		CHECK_THAT(block[16 + i], WithinAbs(0.5, 1e-6));
	}
}

TEST_CASE("gain applies to every channel of nodes that are not stereo", "[unit]")
{
	GainPan gainPan;
	Bench bench(gainPan, 3, 4);
	bench.fill(1.0f);
	bench.add(parameter(0, GainPan::Gain, -6.0));
	bench.add(parameter(0, GainPan::Pan, -1.0));
	for (const float sample : bench.process())
		CHECK_THAT(sample, WithinAbs(std::pow(10.0, -6.0 / 20.0), 1e-6));
}

TEST_CASE("the test synth starts notes at their sample and stops them after the release", "[unit]")
{
	TestSynth synth;
	Bench bench(synth, 2, 480);
	bench.add(parameter(0, TestSynth::Attack, 0.0));
	bench.add(parameter(0, TestSynth::Release, 0.001)); // 48 samples
	bench.add(note(100, true, 69));
	const std::vector<float> on = bench.process();
	CHECK(peak(on, 0, 100) == 0.0f);
	CHECK(peak(on, 100, 480) > 0.2f);
	CHECK(synth.activeVoices() == 1);

	bench.add(note(10, false, 69));
	const std::vector<float> off = bench.process();
	CHECK(peak(off, 10 + 48, 480) == 0.0f);
	CHECK(synth.activeVoices() == 0);
}

TEST_CASE("the test synth plays notes together and reuses the oldest voice when all are busy", "[unit]")
{
	TestSynth synth;
	Bench bench(synth, 1, 256);
	for (uint8_t i = 0; i < g_testSynthVoices + 4; ++i)
		bench.add(note(i, true, static_cast<uint8_t>(40 + i)));
	const std::vector<float> block = bench.process();
	CHECK(synth.activeVoices() == g_testSynthVoices);
	CHECK(peak(block) <= 0.25f * g_testSynthVoices);

	// Pitches 40 to 43 were taken over: their note-offs release nothing.
	for (uint8_t pitch = 40; pitch < 44; ++pitch)
		bench.add(note(0, false, pitch));
	bench.process();
	CHECK(synth.activeVoices() == g_testSynthVoices);

	// A note-on with velocity 0 is a note-off.
	bench.add(parameter(0, TestSynth::Release, 0.0));
	bench.add(note(0, true, 44, 0));
	bench.process();
	CHECK(synth.activeVoices() == g_testSynthVoices - 1);
}

TEST_CASE("a MIDI track driving the test synth renders the expected audio", "[integration]")
{
	// MIDI channel -> test synth -> gain and pan -> stereo output, as plugin slots on an instrument track will be.
	GraphDesc desc;
	desc.nodes.push_back({NodeKind::Channel, 1, 0, nullptr});
	desc.nodes.push_back({NodeKind::Builtin, 0, 2, createBuiltin("testSynth")});
	desc.nodes.push_back({NodeKind::Builtin, 0, 2, createBuiltin("gainPan")});
	desc.nodes.push_back({NodeKind::OutputBus, 2, 2, nullptr});
	desc.events = {{0, 1}};
	desc.audio = {{1, 2}, {2, 3}};
	constexpr uint32_t blockSize = 128;
	const CompiledGraph graph(desc, g_sampleRate, blockSize);

	// A C major chord, one note after the other, panned left halfway through and released at the end.
	struct Scheduled
	{
		int64_t time;
		int node;
		ProcessEvent event;
	};
	const std::vector<Scheduled> events = {
		{0, 0, note(0, true, 60, 0xc000)},
		{4800, 0, note(0, true, 64, 0x9000)},
		{9600, 0, note(0, true, 67, 0xffff)},
		{12000, 2, parameter(0, GainPan::Pan, -0.75)},
		{16800, 0, note(0, false, 60)},
		{16800, 0, note(0, false, 64)},
		{16800, 0, note(0, false, 67)},
	};
	constexpr int64_t length = int64_t{188} * blockSize; // about half a second

	gnubase::test::Audio audio;
	audio.channels = 2;
	audio.sampleRate = static_cast<int>(g_sampleRate);
	for (int64_t start = 0; start < length; start += blockSize)
	{
		for (const Scheduled& scheduled : events)
			if (scheduled.time >= start && scheduled.time < start + blockSize)
			{
				ProcessEvent event = scheduled.event;
				event.offset = static_cast<uint32_t>(scheduled.time - start);
				REQUIRE(graph.events(scheduled.node).add(event));
			}
		graph.process(blockSize);
		for (uint32_t i = 0; i < blockSize; ++i)
			for (int c = 0; c < 2; ++c)
				audio.samples.push_back(graph.audio(3)[c][i]);
	}

	const gnubase::test::AudioDiff diff = gnubase::test::compareWithGolden("test-synth-chord", audio, 1e-5f);
	INFO(diff.report);
	CHECK(diff.equal);
}

TEST_CASE("processor.list describes every built-in processor and its parameters", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Result result = client.call("processor.list");
	REQUIRE(result.ok());
	const Json& list = result.value.at("processors");

	std::vector<std::string> ids;
	for (const Json& processor : list)
		ids.push_back(processor.at("id").get<std::string>());
	CHECK(ids ==
		std::vector<std::string>{"compressor", "delay", "dither", "eq", "gainPan", "gate", "impulse", "limiter",
			"noise", "positionProbe", "reverb", "sine", "testSynth"});

	const Json& synth = list[12];
	CHECK(synth.at("instrument") == true);
	CHECK(list[0].at("instrument") == false);
	const Json& release = synth.at("parameters")[TestSynth::Release];
	CHECK(release.at("id") == TestSynth::Release);
	CHECK(release.at("name") == "Release");
	CHECK(release.at("unit") == "s");
	CHECK(release.at("default") == 0.05);
	CHECK(release.at("stepped") == false);
	CHECK(release.at("options").empty());

	// Stepped parameters, with names for their values or without.
	const Json& eq = list[3];
	CHECK(eq.at("name") == "Parametric EQ");
	const Json& type = eq.at("parameters")[2];
	CHECK(type.at("name") == "Band 1 type");
	CHECK(type.at("stepped") == true);
	CHECK(type.at("options") == Json{"Bell", "Low shelf", "High shelf", "Low cut", "High cut"});
	CHECK(type.at("default") == 3); // a low cut
	const Json& bits = list[2].at("parameters")[0];
	CHECK(bits.at("name") == "Bits");
	CHECK(bits.at("stepped") == true);
	CHECK(bits.at("options").empty());
}

TEST_CASE("every built-in processor can be created, and unknown ones can't", "[unit]")
{
	for (const BuiltinInfo& info : builtins())
		CHECK(createBuiltin(info.id) != nullptr);
	CHECK_THROWS_WITH(createBuiltin("chorus"), ContainsSubstring("no built-in processor chorus"));
}
