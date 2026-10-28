// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/graph/events.h"
#include "engine/graph/graph.h"
#include "engine/graph/processor.h"
#include "engine/processors/builtin.h"
#include "engine/processors/gainpan.h"
#include "plugin/events.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
	constexpr size_t g_maxTransportSegments = 16;

	// Adapts one of the engine's built-in processors to the plugin API, as #114 will do for the built-in plugins: a
	// main audio input (none for instruments) and output with the same channels, the processor's parameters, notes,
	// and the parameter values as its state.
	class BuiltinPlugin final : public Plugin,
								public AudioPorts,
								public Params,
								public State,
								public Latency,
								public Tail
	{
	public:
		BuiltinPlugin(const engine::processors::BuiltinInfo& info, Host& host, uint32_t channels)
			: m_info(info), m_host(host), m_channels(channels), m_values(info.parameters.size()),
			  m_pending(info.parameters.size())
		{
			m_descriptor.id = "builtin:" + info.id;
			m_descriptor.format = "builtin";
			m_descriptor.formatId = info.id;
			m_descriptor.name = info.name;
			m_descriptor.vendor = "GNUbase";
			m_descriptor.version = GNUBASE_VERSION;
			m_descriptor.features = {info.instrument ? "instrument" : "audio-effect"};
		}

		const Descriptor& descriptor() const override { return m_descriptor; }

		bool init() override
		{
			m_processor = engine::processors::createBuiltin(m_info.id);
			for (size_t i = 0; i < m_info.parameters.size(); ++i)
			{
				m_values[i] = m_info.parameters[i].defaultValue;
				m_pending[i] = false;
			}
			return true;
		}

		bool activate(double sampleRate, uint32_t /*minFrames*/, uint32_t maxFrames) override
		{
			m_processor->prepareFor(sampleRate, maxFrames);
			m_segments.resize(g_maxTransportSegments);
			return true;
		}

		void deactivate() override {}
		bool startProcessing() noexcept override { return true; }
		void stopProcessing() noexcept override {}
		void reset() noexcept override {} // graph::Processor can't forget its past yet; #114 adds that

		ProcessStatus process(const Process& process) noexcept override
		{
			const uint32_t inputs = m_info.instrument ? 0 : 1;
			if (process.audioInputCount != inputs || process.audioOutputCount != 1 ||
				process.audioOutputs[0].channelCount != m_channels ||
				(inputs == 1 && process.audioInputs[0].channelCount != m_channels) || process.transportCount == 0)
				return ProcessStatus::Error;

			// graph::Processor processes in place, in the plugin's output.
			const AudioBuffer& out = process.audioOutputs[0];
			for (uint32_t c = 0; c < m_channels; ++c)
			{
				if (inputs == 1)
					std::memcpy(out.channels[c], process.audioInputs[0].channels[c], process.frames * sizeof(float));
				else
					std::fill(out.channels[c], out.channels[c] + process.frames, 0.0f);
			}

			m_events.clear();
			m_eventsOut.clear();
			applyPending();
			for (const Event& event : process.eventsIn)
				translate(event);

			const engine::graph::TransportBlock transport = convert(process);
			m_processor->process(
				{out.channels, static_cast<int>(m_channels), process.frames, m_events, m_eventsOut, transport});
			return ProcessStatus::Continue;
		}

		void* extension(std::string_view id) override
		{
			return provide<AudioPorts, Params, State, Latency, Tail>(this, id);
		}

		// AudioPorts
		uint32_t audioPortCount(bool input) const override { return input && m_info.instrument ? 0 : 1; }

		bool audioPortInfo(bool input, uint32_t index, AudioPortInfo& info) const override
		{
			if (index >= audioPortCount(input))
				return false;
			info.id = 0;
			info.name = input ? "Input" : "Output";
			info.channelCount = m_channels;
			info.layout = m_channels == 2 ? ChannelLayout::Stereo : ChannelLayout::Mono;
			info.flags = AudioPortInfo::Main;
			return true;
		}

		// Params
		uint32_t paramCount() const override { return static_cast<uint32_t>(m_info.parameters.size()); }

		bool paramInfo(uint32_t index, ParamInfo& info) const override
		{
			if (index >= paramCount())
				return false;
			const engine::processors::ParameterInfo& parameter = m_info.parameters[index];
			info.id = parameter.id;
			info.flags = ParamInfo::Automatable;
			info.name = parameter.name;
			info.minimum = parameter.minimum;
			info.maximum = parameter.maximum;
			info.defaultValue = parameter.defaultValue;
			return true;
		}

		bool paramValue(uint32_t param, double& value) const override
		{
			const int index = indexOf(param);
			if (index < 0)
				return false;
			value = m_values[static_cast<size_t>(index)];
			return true;
		}

		bool paramValueToText(uint32_t param, double value, std::string& text) const override
		{
			const int index = indexOf(param);
			if (index < 0)
				return false;
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%.2f", value);
			text = buffer;
			const std::string& unit = m_info.parameters[static_cast<size_t>(index)].unit;
			if (!unit.empty())
				text += " " + unit;
			return true;
		}

		bool paramTextToValue(uint32_t param, std::string_view text, double& value) const override
		{
			if (indexOf(param) < 0)
				return false;
			const std::string copy(text);
			char* end = nullptr;
			value = std::strtod(copy.c_str(), &end);
			return end != copy.c_str();
		}

		void flushParams(const EventList& in, EventList& /*out*/) noexcept override
		{
			for (const Event& event : in)
				if (event.type == EventType::ParamValue)
					setValue(event.param, event.value);
		}

		// State: the parameter values, as pairs of id and value.
		bool saveState(std::vector<uint8_t>& data, StateContext /*context*/) override
		{
			data.clear();
			for (size_t i = 0; i < m_info.parameters.size(); ++i)
			{
				const uint32_t id = m_info.parameters[i].id;
				const double value = m_values[i];
				const size_t at = data.size();
				data.resize(at + sizeof(id) + sizeof(value));
				std::memcpy(data.data() + at, &id, sizeof(id));
				std::memcpy(data.data() + at + sizeof(id), &value, sizeof(value));
			}
			return true;
		}

		bool loadState(const uint8_t* data, size_t size, StateContext /*context*/) override
		{
			constexpr size_t entry = sizeof(uint32_t) + sizeof(double);
			if (size % entry != 0)
				return false;
			for (size_t at = 0; at < size; at += entry)
			{
				uint32_t id = 0;
				double value = 0.0;
				std::memcpy(&id, data + at, sizeof(id));
				std::memcpy(&value, data + at + sizeof(id), sizeof(value));
				setValue(id, value);
			}
			if (auto* hostParams = plugin::extension<HostParams>(m_host))
				hostParams->rescanParams(HostParams::Values);
			return true;
		}

		uint32_t latency() const override { return 0; }
		uint32_t tail() const noexcept override { return 0; }

	private:
		int indexOf(uint32_t param) const
		{
			for (size_t i = 0; i < m_info.parameters.size(); ++i)
				if (m_info.parameters[i].id == param)
					return static_cast<int>(i);
			return -1;
		}

		// The processor gets the value at the start of the next block.
		void setValue(uint32_t param, double value) noexcept
		{
			const int index = indexOf(param);
			if (index < 0)
				return;
			m_values[static_cast<size_t>(index)] = value;
			m_pending[static_cast<size_t>(index)] = true;
		}

		void applyPending() noexcept
		{
			for (size_t i = 0; i < m_pending.size(); ++i)
				if (m_pending[i].exchange(false))
					addParameter(0, m_info.parameters[i].id, m_values[i]);
		}

		void addParameter(uint32_t offset, uint32_t param, double value) noexcept
		{
			engine::graph::ProcessEvent event;
			event.offset = offset;
			event.type = engine::graph::ProcessEvent::Type::Parameter;
			event.parameter = param;
			event.parameterValue = value;
			m_events.add(event);
		}

		void translate(const Event& event) noexcept
		{
			if (event.type == EventType::ParamValue)
			{
				const int index = indexOf(event.param);
				if (index < 0)
					return;
				m_values[static_cast<size_t>(index)] = event.value;
				addParameter(event.offset, event.param, event.value);
			}
			else if ((event.type == EventType::NoteOn || event.type == EventType::NoteOff) && event.key >= 0)
			{
				engine::graph::ProcessEvent note;
				note.offset = event.offset;
				note.type = event.type == EventType::NoteOn ? engine::graph::ProcessEvent::Type::NoteOn
															: engine::graph::ProcessEvent::Type::NoteOff;
				note.channel = static_cast<uint8_t>(std::max<int16_t>(event.channel, 0));
				note.key = static_cast<uint8_t>(event.key);
				note.velocity = static_cast<uint16_t>(std::lround(std::clamp(event.value, 0.0, 1.0) * 65535.0));
				m_events.add(note);
			}
		}

		engine::graph::TransportBlock convert(const Process& process) noexcept
		{
			const Transport& first = process.transport[0].transport;
			engine::graph::TransportBlock block;
			block.playing = (first.flags & Transport::Playing) != 0;
			block.position = first.position;
			if (!block.playing)
				return block;
			const size_t count = std::min<size_t>(process.transportCount, m_segments.size());
			for (size_t i = 0; i < count; ++i)
			{
				const uint32_t begin = process.transport[i].offset;
				const uint32_t end = i + 1 < count ? process.transport[i + 1].offset : process.frames;
				m_segments[i] = {begin, end - begin, process.transport[i].transport.position};
			}
			block.segments = m_segments.data();
			block.segmentCount = count;
			return block;
		}

		const engine::processors::BuiltinInfo& m_info;
		Host& m_host;
		uint32_t m_channels;
		Descriptor m_descriptor;
		std::shared_ptr<engine::graph::Processor> m_processor;
		std::vector<std::atomic<double>> m_values; // written while processing, read by value() on the main thread
		std::vector<std::atomic<bool>> m_pending;  // values the processor hasn't got yet
		engine::graph::EventBuffer m_events{engine::graph::g_maxEventsPerBlock};
		engine::graph::EventBuffer m_eventsOut{engine::graph::g_maxEventsPerBlock};
		std::vector<engine::graph::TransportSegment> m_segments;
	};

	// The built-in processors as a plugin factory, with stereo ports.
	class BuiltinFactory final : public Factory
	{
	public:
		BuiltinFactory()
		{
			for (const engine::processors::BuiltinInfo& info : engine::processors::builtins())
			{
				Descriptor descriptor;
				descriptor.id = "builtin:" + info.id;
				descriptor.format = "builtin";
				descriptor.formatId = info.id;
				descriptor.name = info.name;
				m_descriptors.push_back(descriptor);
			}
		}

		size_t count() const override { return m_descriptors.size(); }
		const Descriptor& descriptor(size_t index) const override { return m_descriptors[index]; }

		std::unique_ptr<Plugin> create(std::string_view id, Host& host) override
		{
			for (const engine::processors::BuiltinInfo& info : engine::processors::builtins())
				if (id == "builtin:" + info.id)
					return std::make_unique<BuiltinPlugin>(info, host, 2);
			return nullptr;
		}

	private:
		std::vector<Descriptor> m_descriptors;
	};

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
