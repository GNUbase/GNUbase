// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/delay.h"
#include "engine/dsp/dither.h"
#include "engine/dsp/equalizer.h"
#include "engine/graph/events.h"
#include "engine/graph/graph.h"
#include "engine/graph/processor.h"
#include "engine/processors/builtin.h"
#include "engine/processors/builtinplugin.h"
#include "engine/processors/gainpan.h"
#include "plugin/events.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace gnubase;
using namespace gnubase::plugin;
using Catch::Matchers::WithinAbs;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr uint32_t g_frames = 64;

	using engine::processors::BuiltinFactory;

	// Records what plugins ask of it.
	class TestHost final : public Host, public HostParams, public HostState, public HostLatency
	{
	public:
		void requestRestart() override { ++restarts; }
		void requestProcess() override {}
		void requestCallback() override {}
		void* extension(std::string_view id) override { return provide<HostParams, HostState, HostLatency>(this, id); }

		void rescanParams(uint32_t what) override { rescans.push_back(what); }
		void requestParamFlush() override {}
		void markStateDirty() override { ++dirty; }
		void latencyChanged() override { ++latencyChanges; }

		int restarts = 0;
		int dirty = 0;
		int latencyChanges = 0;
		std::vector<uint32_t> rescans;
	};

	// Processes a plugin with one stereo input and output, the way the engine's plugin slot will.
	class Runner
	{
	public:
		explicit Runner(Plugin& plugin, uint32_t inputs = 1)
			: m_plugin(plugin), m_in(size_t{2} * g_frames), m_out(size_t{2} * g_frames), m_inputs(inputs)
		{
			for (uint32_t c = 0; c < 2; ++c)
			{
				m_inPointers.push_back(m_in.data() + (static_cast<size_t>(c) * g_frames));
				m_outPointers.push_back(m_out.data() + (static_cast<size_t>(c) * g_frames));
			}
			m_transport.transport.flags = Transport::Playing | Transport::TempoValid;
		}

		void fill(float value) { std::fill(m_in.begin(), m_in.end(), value); }
		void add(const Event& event) { REQUIRE(m_events.push(event)); }

		// One block; returns the output, channel after channel.
		std::vector<float> process(ProcessStatus expected = ProcessStatus::Continue)
		{
			const AudioBuffer input{m_inPointers.data(), 2, 0};
			AudioBuffer output{m_outPointers.data(), 2, 0};
			const Process process{g_frames, m_steadyTime, ProcessMode::Realtime, &input, m_inputs, &output, 1, m_events,
				m_eventsOut, &m_transport, 1};
			CHECK(m_plugin.process(process) == expected);
			m_events.clear();
			m_steadyTime += g_frames;
			m_transport.transport.position += g_frames;
			return m_out;
		}

		const std::vector<float>& input() const { return m_in; }

	private:
		Plugin& m_plugin;
		std::vector<float> m_in;
		std::vector<float> m_out;
		std::vector<float*> m_inPointers;
		std::vector<float*> m_outPointers;
		uint32_t m_inputs;
		EventList m_events{64};
		EventList m_eventsOut{64};
		TransportSegment m_transport;
		int64_t m_steadyTime = 0;
	};

	Event paramValue(uint32_t offset, uint32_t param, double value)
	{
		Event event;
		event.offset = offset;
		event.type = EventType::ParamValue;
		event.param = param;
		event.value = value;
		return event;
	}

	std::unique_ptr<Plugin> createActive(Factory& factory, std::string_view id, Host& host)
	{
		std::unique_ptr<Plugin> plugin = factory.create(id, host);
		REQUIRE(plugin);
		REQUIRE(plugin->init());
		REQUIRE(plugin->activate(g_sampleRate, 1, g_frames));
		REQUIRE(plugin->startProcessing());
		return plugin;
	}
}

TEST_CASE("the plugin API describes a built-in processor through its factory and extensions", "[unit]")
{
	BuiltinFactory factory;
	TestHost host;
	REQUIRE(factory.count() == engine::processors::builtins().size());
	CHECK(factory.descriptor(0).format == "builtin");
	CHECK(factory.create("builtin:unknown", host) == nullptr);

	const std::unique_ptr<Plugin> plugin = factory.create("builtin:gainPan", host);
	REQUIRE(plugin);
	REQUIRE(plugin->init());
	CHECK(plugin->descriptor().id == "builtin:gainPan");
	CHECK(plugin->descriptor().features == std::vector<std::string>{"audio-effect"});

	SECTION("extensions are found by id, and unknown ones are not")
	{
		CHECK(extension<Params>(*plugin) != nullptr);
		CHECK(extension<State>(*plugin) != nullptr);
		CHECK(extension<Latency>(*plugin)->latency() == 0);
		CHECK(extension<Tail>(*plugin)->tail() == 0);
		CHECK(extension<Gui>(*plugin) == nullptr);
		CHECK(extension<LowLatency>(*plugin) == nullptr);
		CHECK(plugin->extension("gnubase.unknown") == nullptr);
		// Each lookup returns the address of its own base class.
		CHECK(static_cast<void*>(extension<Params>(*plugin)) != static_cast<void*>(extension<State>(*plugin)));
	}

	SECTION("audio ports")
	{
		const AudioPorts* ports = extension<AudioPorts>(*plugin);
		REQUIRE(ports != nullptr);
		REQUIRE(ports->audioPortCount(true) == 1);
		REQUIRE(ports->audioPortCount(false) == 1);
		AudioPortInfo info;
		REQUIRE(ports->audioPortInfo(false, 0, info));
		CHECK(info.channelCount == 2);
		CHECK(info.layout == ChannelLayout::Stereo);
		CHECK(info.flags == AudioPortInfo::Main);
		CHECK(!ports->audioPortInfo(false, 1, info));
	}

	SECTION("parameters with their ranges, values and texts")
	{
		const Params* params = extension<Params>(*plugin);
		REQUIRE(params->paramCount() == 2);
		ParamInfo info;
		REQUIRE(params->paramInfo(0, info));
		CHECK(info.id == engine::processors::GainPan::Gain);
		CHECK(info.name == "Gain");
		CHECK(info.minimum == -96.0);
		CHECK(info.maximum == 12.0);
		CHECK((info.flags & ParamInfo::Automatable) != 0);
		double value = 1.0;
		REQUIRE(params->paramValue(engine::processors::GainPan::Gain, value));
		CHECK(value == 0.0);
		std::string text;
		REQUIRE(params->paramValueToText(engine::processors::GainPan::Gain, -6.0, text));
		CHECK(text == "-6.00 dB");
		REQUIRE(params->paramTextToValue(engine::processors::GainPan::Gain, "-3.5 dB", value));
		CHECK(value == -3.5);
		CHECK(!params->paramValue(99, value));
	}

	SECTION("an instrument has no audio input")
	{
		const std::unique_ptr<Plugin> synth = factory.create("builtin:testSynth", host);
		REQUIRE(synth->init());
		CHECK(synth->descriptor().features == std::vector<std::string>{"instrument"});
		CHECK(extension<AudioPorts>(*synth)->audioPortCount(true) == 0);
	}
}

TEST_CASE("the plugin API processes a built-in processor with sample-accurate parameter changes", "[unit]")
{
	BuiltinFactory factory;
	TestHost host;
	const std::unique_ptr<Plugin> plugin = createActive(factory, "builtin:gainPan", host);
	Runner runner(*plugin);
	runner.fill(1.0f);

	// -6.0206 dB halves the level from frame 16 on.
	const double half = 20.0 * std::log10(0.5);
	runner.add(paramValue(16, engine::processors::GainPan::Gain, half));
	std::vector<float> out = runner.process();
	for (uint32_t c = 0; c < 2; ++c)
		for (uint32_t i = 0; i < g_frames; ++i)
			CHECK_THAT(out[(c * g_frames) + i], WithinAbs(i < 16 ? 1.0 : 0.5, 1e-6));
	CHECK(runner.input()[20] == 1.0f); // the input is left alone

	double value = 0.0;
	REQUIRE(extension<Params>(*plugin)->paramValue(engine::processors::GainPan::Gain, value));
	CHECK(value == half);

	// Panning right silences the left channel from the block's first frame.
	runner.add(paramValue(0, engine::processors::GainPan::Pan, 1.0));
	out = runner.process();
	CHECK_THAT(out[0], WithinAbs(0.0, 1e-6));
	CHECK_THAT(out[g_frames], WithinAbs(0.5, 1e-6));

	plugin->stopProcessing();
	plugin->deactivate();
}

TEST_CASE("the plugin API restores a plugin's state in another instance", "[unit]")
{
	BuiltinFactory factory;
	TestHost host;
	const std::unique_ptr<Plugin> source = factory.create("builtin:gainPan", host);
	REQUIRE(source->init());

	// While a plugin isn't processing, parameter changes go through flushParams on the main thread.
	EventList in(8);
	EventList out(8);
	REQUIRE(in.push(paramValue(0, engine::processors::GainPan::Gain, -12.0)));
	REQUIRE(in.push(paramValue(0, engine::processors::GainPan::Pan, -0.5)));
	extension<Params>(*source)->flushParams(in, out);
	CHECK(out.empty());

	std::vector<uint8_t> state;
	REQUIRE(extension<State>(*source)->saveState(state, StateContext::Project));
	REQUIRE(source->activate(g_sampleRate, 1, g_frames));
	REQUIRE(source->startProcessing());

	TestHost otherHost;
	const std::unique_ptr<Plugin> copy = createActive(factory, "builtin:gainPan", otherHost);
	CHECK(!extension<State>(*copy)->loadState(state.data(), 3, StateContext::Project));
	REQUIRE(extension<State>(*copy)->loadState(state.data(), state.size(), StateContext::Project));
	CHECK(otherHost.rescans == std::vector<uint32_t>{HostParams::Values});

	double value = 0.0;
	REQUIRE(extension<Params>(*copy)->paramValue(engine::processors::GainPan::Pan, value));
	CHECK(value == -0.5);

	// Both sound the same.
	Runner sourceRunner(*source);
	Runner copyRunner(*copy);
	sourceRunner.fill(0.25f);
	copyRunner.fill(0.25f);
	const std::vector<float> sourceOut = sourceRunner.process();
	const std::vector<float> copyOut = copyRunner.process();
	CHECK(sourceOut == copyOut);
	CHECK_THAT(sourceOut[0], WithinAbs(0.25 * std::pow(10.0, -12.0 / 20.0), 1e-6));
}

TEST_CASE("plugin event lists keep their order and carry system exclusive messages", "[unit]")
{
	EventList list(3, 8);
	REQUIRE(list.push(paramValue(5, 0, 1.0)));
	CHECK(!list.push(paramValue(3, 0, 1.0))); // earlier than the last one
	REQUIRE(list.push(paramValue(5, 1, 1.0)));

	const uint8_t sysex[] = {0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7};
	REQUIRE(list.pushSysex(9, 0, sysex, sizeof(sysex)));
	CHECK(!list.pushSysex(10, 0, sysex, sizeof(sysex))); // no room for the bytes
	CHECK(!list.push(paramValue(10, 0, 1.0)));           // full
	REQUIRE(list.size() == 3);
	CHECK(list[2].type == EventType::Sysex);
	CHECK(list[2].data[1] == sizeof(sysex));
	CHECK(std::memcmp(list.bytes(list[2]), sysex, sizeof(sysex)) == 0);

	list.clear();
	CHECK(list.empty());
	REQUIRE(list.pushSysex(0, 0, sysex, sizeof(sysex)));
	CHECK(list.bytes(list[0])[0] == 0xf0);
}

TEST_CASE("the mixing effects are built-in plugins with their latency, tail and named values", "[unit]")
{
	BuiltinFactory factory;
	TestHost host;

	SECTION("the limiter reports its look-ahead as latency, the delay and the reverb their tails")
	{
		const std::unique_ptr<Plugin> limiter = createActive(factory, "builtin:limiter", host);
		CHECK(extension<Latency>(*limiter)->latency() == 48); // 1 ms
		CHECK(extension<Tail>(*limiter)->tail() == 48);

		const std::unique_ptr<Plugin> reverb = createActive(factory, "builtin:reverb", host);
		// Predelay 20 ms plus 1.5 times the decay of 2 s.
		CHECK_THAT(static_cast<double>(extension<Tail>(*reverb)->tail()), WithinAbs(3.02 * g_sampleRate, 1.0));

		const std::unique_ptr<Plugin> delay = createActive(factory, "builtin:delay", host);
		Runner runner(*delay);
		runner.add(paramValue(0, engine::dsp::Delay::Feedback, 100.0));
		runner.process();
		CHECK(extension<Tail>(*delay)->tail() == g_infiniteTail);
	}

	SECTION("stepped parameters, and the names of their values")
	{
		using engine::dsp::Equalizer;
		const std::unique_ptr<Plugin> eq = factory.create("builtin:eq", host);
		REQUIRE(eq->init());
		const Params* params = extension<Params>(*eq);
		REQUIRE(params->paramCount() == Equalizer::g_parameterCount);
		ParamInfo info;
		REQUIRE(params->paramInfo(Equalizer::parameter(0, Equalizer::Type), info));
		CHECK(info.name == "Band 1 type");
		CHECK((info.flags & ParamInfo::Stepped) != 0);
		CHECK((info.flags & ParamInfo::Enum) != 0);
		CHECK(info.maximum == 4.0);
		std::string text;
		REQUIRE(params->paramValueToText(info.id, 3.0, text));
		CHECK(text == "Low cut");
		double value = 0.0;
		REQUIRE(params->paramTextToValue(info.id, "High cut", value));
		CHECK(value == 4.0);
		REQUIRE(params->paramValueToText(Equalizer::parameter(0, Equalizer::Frequency), 1000.0, text));
		CHECK(text == "1000.00 Hz");

		const std::unique_ptr<Plugin> dither = factory.create("builtin:dither", host);
		REQUIRE(dither->init());
		REQUIRE(extension<Params>(*dither)->paramInfo(0, info));
		CHECK(info.id == engine::dsp::Dither::Bits);
		CHECK((info.flags & ParamInfo::Stepped) != 0);
		CHECK((info.flags & ParamInfo::Enum) == 0);
		REQUIRE(extension<Params>(*dither)->paramValueToText(info.id, 16.0, text));
		CHECK(text == "16");
	}

	SECTION("an effect sounds the same through the plugin API as a processor, and forgets its past on reset")
	{
		using engine::dsp::Delay;
		const std::unique_ptr<Plugin> plugin = createActive(factory, "builtin:delay", host);
		Runner runner(*plugin);
		const std::shared_ptr<engine::graph::Processor> processor = engine::processors::createBuiltin("delay");
		processor->prepareFor(g_sampleRate, g_frames);
		std::vector<float> samples(size_t{2} * g_frames);
		float* const channels[] = {samples.data(), samples.data() + g_frames};
		engine::graph::EventBuffer events(8);
		engine::graph::EventBuffer eventsOut(8);
		const auto change = [&](uint32_t offset, uint32_t id, double value)
		{
			runner.add(paramValue(offset, id, value));
			engine::graph::ProcessEvent event;
			event.offset = offset;
			event.type = engine::graph::ProcessEvent::Type::Parameter;
			event.parameter = id;
			event.parameterValue = value;
			events.add(event);
		};

		std::vector<std::vector<float>> first;
		for (int block = 0; block < 4; ++block)
		{
			if (block == 0)
			{
				change(0, Delay::Time, 1.0); // 48 frames
				change(10, Delay::Feedback, 50.0);
				change(30, Delay::Mix, 60.0);
			}
			const float level = block == 0 ? 0.25f : 0.0f;
			runner.fill(level);
			std::fill(samples.begin(), samples.end(), level);
			const std::vector<float> out = runner.process();
			processor->process({channels, 2, g_frames, events, eventsOut, engine::graph::TransportBlock{}});
			events.clear();
			CHECK(out == samples);
			first.push_back(out);
		}
		const std::vector<float> silence(size_t{2} * g_frames, 0.0f);
		CHECK(first[3] != silence); // echoes of the first block

		(*plugin).reset();
		runner.fill(0.0f);
		CHECK(runner.process() == silence);
	}
}
