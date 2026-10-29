// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/graph/graph.h"
#include "engine/plugins/hostmanager.h"
#include "engine/plugins/pluginslot.h"
#include "platform/rt/realtime.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"
#include "plugin/remote/proxy.h"
#include "pluginhost/formats.h"
#include "pluginhost/vst3/presetfile.h"
#include "pluginhost/vst3/vst3format.h"
#include "vst3adapter/adaptertest.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// CMake passes the full paths; the fallbacks only serve tools that compile this file on its own.
#ifndef GNUBASE_VST3_ADAPTERTEST
#define GNUBASE_VST3_ADAPTERTEST "gnubase-vst3-adaptertest.vst3"
#endif
#ifndef GNUBASE_PLUGINHOST
#define GNUBASE_PLUGINHOST "gnubase-pluginhost"
#endif
#ifndef GNUBASE_TEST_PLUGINS_DIR
#define GNUBASE_TEST_PLUGINS_DIR "plugins"
#endif

using namespace gnubase;
using namespace gnubase::tests::vst3adapter;
using Catch::Approx;
using namespace std::chrono_literals;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr uint32_t g_maxFrames = 256;
	constexpr uint32_t g_defaultLatency = 64;
	// A plugin's own note expression: VST3's first custom note expression type.
	constexpr uint32_t g_customExpression = plugin::g_firstPluginExpression + 100000;

	std::string pluginId()
	{
		return std::string("vst3:") + g_classId;
	}

	// The host side of a plugin, in-process or the engine's side of a remote one, recording what the plugin asks for.
	// The counts are atomic: a remote plugin's notifications arrive on the host manager's job thread.
	class RecordingHost final : public plugin::Host,
								public plugin::HostParams,
								public plugin::HostLatency,
								public plugin::HostState,
								public plugin::HostAudioPorts,
								public plugin::HostNotePorts
	{
	public:
		void requestRestart() override { ++restarts; }
		void requestProcess() override {}
		void requestCallback() override { ++callbacks; }
		void* extension(std::string_view id) override
		{
			return plugin::provide<plugin::HostParams, plugin::HostLatency, plugin::HostState, plugin::HostAudioPorts,
				plugin::HostNotePorts>(this, id);
		}

		void rescanParams(uint32_t what) override { rescans |= what; }
		void requestParamFlush() override { ++flushRequests; }
		void latencyChanged() override { ++latencyChanges; }
		void markStateDirty() override { ++dirty; }
		void rescanAudioPorts() override { ++audioPortRescans; }
		void rescanNotePorts(uint32_t /*what*/) override { ++notePortRescans; }

		std::atomic<int> restarts{0};
		std::atomic<int> callbacks{0};
		std::atomic<uint32_t> rescans{0};
		std::atomic<int> flushRequests{0};
		std::atomic<int> latencyChanges{0};
		std::atomic<int> dirty{0};
		std::atomic<int> audioPortRescans{0};
		std::atomic<int> notePortRescans{0};
	};

	// The adapter's format, the test module loaded, and instances of its plugin.
	class Vst3Fixture
	{
	public:
		explicit Vst3Fixture(const std::string& path = GNUBASE_VST3_ADAPTERTEST)
			: m_format(pluginhost::makeVst3Format()), m_factory(m_format->load(path))
		{
			REQUIRE(m_factory->count() > 0);
		}

		plugin::Factory& factory() const { return *m_factory; }

		// The module's first plugin, initialized.
		std::unique_ptr<plugin::Plugin> create()
		{
			std::unique_ptr<plugin::Plugin> plugin = m_factory->create(m_factory->descriptor(0).id, host);
			REQUIRE(plugin != nullptr);
			REQUIRE(plugin->init());
			return plugin;
		}

		RecordingHost host;

	private:
		std::unique_ptr<pluginhost::Format> m_format;
		std::unique_ptr<plugin::Factory> m_factory;
	};

	template <typename Ext> Ext& ext(plugin::Plugin& plugin)
	{
		Ext* found = plugin::extension<Ext>(plugin);
		REQUIRE(found != nullptr);
		return *found;
	}

	plugin::Event paramEvent(uint32_t offset, uint32_t param, double value)
	{
		plugin::Event event;
		event.offset = offset;
		event.type = plugin::EventType::ParamValue;
		event.param = param;
		event.value = value;
		return event;
	}

	plugin::Event noteEvent(plugin::EventType type, uint32_t offset, int16_t key, double velocity)
	{
		plugin::Event event;
		event.offset = offset;
		event.type = type;
		event.port = 0;
		event.channel = 0;
		event.key = key;
		event.value = velocity;
		return event;
	}

	// Processes blocks of an active plugin with buffers for all its ports. The main input gets the input on its left
	// channel and the input times -0.5 on its right one.
	class Runner
	{
	public:
		explicit Runner(plugin::Plugin& plugin) : m_plugin(plugin)
		{
			setUp(true, m_inputs, m_inputSamples, m_inputPointers);
			setUp(false, m_outputs, m_outputSamples, m_outputPointers);
		}

		// The left channel of the main output.
		std::vector<float> run(const std::vector<float>& input, const std::vector<plugin::Event>& events = {},
			const std::vector<plugin::TransportSegment>& transport = {}, const std::vector<float>& sidechain = {})
		{
			prepare(input, events, transport, sidechain);
			REQUIRE(process() == plugin::ProcessStatus::Continue);
			return {m_outputPointers[0][0], m_outputPointers[0][0] + m_frames};
		}

		// Fills the inputs of a block, without processing it.
		void prepare(const std::vector<float>& input, const std::vector<plugin::Event>& events = {},
			const std::vector<plugin::TransportSegment>& transport = {}, const std::vector<float>& sidechain = {})
		{
			const auto frames = static_cast<uint32_t>(input.size());
			m_frames = frames;
			for (size_t port = 0; port < m_inputs.size(); ++port)
			{
				for (uint32_t c = 0; c < m_inputs[port].channelCount; ++c)
				{
					float* samples = m_inputPointers[port][c];
					for (uint32_t i = 0; i < frames; ++i)
					{
						const std::vector<float>& source = port == 0 ? input : sidechain;
						const float x = i < source.size() ? source[i] : 0.0f;
						samples[i] = c == 0 ? x : -0.5f * x;
					}
				}
			}
			m_in.clear();
			for (const plugin::Event& event : events)
				REQUIRE(m_in.push(event));
			m_segments = transport;
			if (m_segments.empty())
				m_segments.emplace_back();
		}

		// Adds a system exclusive message to the prepared block's events.
		void prepareSysex(uint32_t offset, const std::vector<uint8_t>& bytes)
		{
			REQUIRE(m_in.pushSysex(offset, 0, bytes.data(), static_cast<uint32_t>(bytes.size())));
		}

		// Processes the prepared block. Real-time safe, as far as the test goes.
		plugin::ProcessStatus process() noexcept
		{
			out.clear();
			const plugin::Process process{m_frames, m_steadyTime, plugin::ProcessMode::Realtime, m_inputs.data(),
				static_cast<uint32_t>(m_inputs.size()), m_outputs.data(), static_cast<uint32_t>(m_outputs.size()), m_in,
				out, m_segments.data(), static_cast<uint32_t>(m_segments.size())};
			const plugin::ProcessStatus status = m_plugin.process(process);
			m_steadyTime += m_frames;
			return status;
		}

		const plugin::AudioBuffer& output(size_t port) const { return m_outputs[port]; }

		plugin::EventList out{1024, 4096};

	private:
		void setUp(bool input, std::vector<plugin::AudioBuffer>& buffers, std::vector<std::vector<float>>& samples,
			std::vector<std::vector<float*>>& pointers)
		{
			auto& ports = ext<plugin::AudioPorts>(m_plugin);
			const uint32_t count = ports.audioPortCount(input);
			buffers.resize(count);
			samples.resize(count);
			pointers.resize(count);
			for (uint32_t port = 0; port < count; ++port)
			{
				plugin::AudioPortInfo info;
				REQUIRE(ports.audioPortInfo(input, port, info));
				samples[port].assign(size_t{info.channelCount} * g_maxFrames, 0.0f);
				for (uint32_t c = 0; c < info.channelCount; ++c)
					pointers[port].push_back(samples[port].data() + (size_t{c} * g_maxFrames));
				buffers[port].channels = pointers[port].data();
				buffers[port].channelCount = info.channelCount;
			}
		}

		plugin::Plugin& m_plugin;
		std::vector<plugin::AudioBuffer> m_inputs;
		std::vector<plugin::AudioBuffer> m_outputs;
		std::vector<std::vector<float>> m_inputSamples;
		std::vector<std::vector<float>> m_outputSamples;
		std::vector<std::vector<float*>> m_inputPointers;
		std::vector<std::vector<float*>> m_outputPointers;
		plugin::EventList m_in{1024, 4096};
		std::vector<plugin::TransportSegment> m_segments;
		uint32_t m_frames = 0;
		int64_t m_steadyTime = 0;
	};

	void start(plugin::Plugin& plugin)
	{
		REQUIRE(plugin.activate(g_sampleRate, 1, g_maxFrames));
		REQUIRE(plugin.startProcessing());
	}

	void stop(plugin::Plugin& plugin)
	{
		plugin.stopProcessing();
		plugin.deactivate();
	}

	// A ramp, so delays and gains show in every sample.
	std::vector<float> ramp(uint32_t frames, float first = 0.001f)
	{
		std::vector<float> samples(frames);
		for (uint32_t i = 0; i < frames; ++i)
			samples[i] = first + (0.001f * static_cast<float>(i));
		return samples;
	}

	const plugin::Event* find(const plugin::EventList& list, const std::function<bool(const plugin::Event&)>& match)
	{
		for (const plugin::Event& event : list)
		{
			if (match(event))
				return &event;
		}
		return nullptr;
	}

	bool isParam(const plugin::Event& event, plugin::EventType type, uint32_t param)
	{
		return event.type == type && event.param == param;
	}

	double valueOf(plugin::Plugin& plugin, uint32_t param)
	{
		double value = -1.0;
		REQUIRE(ext<plugin::Params>(plugin).paramValue(param, value));
		return value;
	}

	// One of the shared test plugins (tests/plugins/README.md) as a VST3 bundle.
	std::string testPlugin(const std::string& name)
	{
		return (std::filesystem::path(GNUBASE_TEST_PLUGINS_DIR) / "vst3" / ("gnubase-test-" + name + ".vst3")).string();
	}

	// An input bus, a plugin slot and an output bus, in a compiled graph, as the engine hosts a plugin.
	class SlotGraph
	{
	public:
		explicit SlotGraph(
			std::unique_ptr<plugin::Plugin> plugin, plugin::ProcessMode mode = plugin::ProcessMode::Offline)
			: m_slot(std::make_shared<engine::plugins::PluginSlot>(std::move(plugin)))
		{
			m_slot->setProcessMode(mode);
			engine::graph::GraphDesc desc;
			desc.nodes = {{engine::graph::NodeKind::InputBus, 1, 2, nullptr},
				{engine::graph::NodeKind::PluginSlot, 2, 2, m_slot},
				{engine::graph::NodeKind::OutputBus, 3, 2, nullptr}};
			desc.audio = {{0, 1}, {1, 2}};
			m_graph = std::make_unique<engine::graph::CompiledGraph>(desc, g_sampleRate, g_maxFrames);
		}

		engine::plugins::PluginSlot& slot() const { return *m_slot; }

		// A block of `input` (the right channel: its negative), with a parameter change if `change`; both output
		// channels one after the other.
		std::vector<float> render(const std::vector<float>& input, uint32_t param = 0, double value = 0.0,
			uint32_t offset = 0, bool change = false)
		{
			const auto frames = static_cast<uint32_t>(input.size());
			float* const* in = m_graph->audio(0);
			for (uint32_t i = 0; i < frames; ++i)
			{
				in[0][i] = input[i];
				in[1][i] = -input[i];
			}
			if (change)
			{
				engine::graph::ProcessEvent event;
				event.offset = offset;
				event.type = engine::graph::ProcessEvent::Type::Parameter;
				event.parameter = param;
				event.parameterValue = value;
				m_graph->events(1).add(event);
			}
			m_graph->process(frames);
			float* const* out = m_graph->audio(2);
			std::vector<float> output(out[0], out[0] + frames);
			output.insert(output.end(), out[1], out[1] + frames);
			return output;
		}

	private:
		std::shared_ptr<engine::plugins::PluginSlot> m_slot;
		std::unique_ptr<engine::graph::CompiledGraph> m_graph;
	};

	engine::plugins::HostManager::Options hostOptions()
	{
		engine::plugins::HostManager::Options options;
		options.executable = GNUBASE_PLUGINHOST;
		return options;
	}

	bool silent(const std::vector<float>& samples)
	{
		return std::all_of(samples.begin(), samples.end(), [](float x) { return x == 0.0f; });
	}

	// Waits until the condition holds, for up to five seconds.
	template <typename Condition> bool eventually(Condition condition)
	{
		const auto deadline = std::chrono::steady_clock::now() + 5s;
		while (!condition())
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(5ms);
		}
		return true;
	}

	std::atomic<int> g_violations{0};

	void countViolation(const char* /*what*/) noexcept
	{
		++g_violations;
	}

	plugin::TransportSegment segment(uint32_t offset, double tempo)
	{
		plugin::TransportSegment result;
		result.offset = offset;
		result.transport.flags = plugin::Transport::Playing | plugin::Transport::TempoValid;
		result.transport.tempo = tempo;
		return result;
	}
}

TEST_CASE("the VST3 adapter lists the audio module classes of a module", "[integration]")
{
	Vst3Fixture fixture;
	REQUIRE(fixture.factory().count() == 1); // the controller class is no plugin of its own
	const plugin::Descriptor& descriptor = fixture.factory().descriptor(0);
	CHECK(descriptor.id == pluginId());
	CHECK(descriptor.format == "vst3");
	CHECK(descriptor.formatId == g_classId);
	CHECK(descriptor.name == "Adapter Test");
	CHECK(descriptor.vendor == "GNUbase");
	CHECK(descriptor.version == "1.0.0");
	CHECK(descriptor.features == std::vector<std::string>{"audio-effect", "utility"});
	CHECK(fixture.factory().create("vst3:00000000000000000000000000000000", fixture.host) == nullptr);
	CHECK_THROWS(pluginhost::makeVst3Format()->load("no such module.vst3"));
}

TEST_CASE("a VST3 plugin's buses, parameters and units become ports and parameter infos", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();

	auto& ports = ext<plugin::AudioPorts>(*plugin);
	REQUIRE(ports.audioPortCount(true) == 2);
	REQUIRE(ports.audioPortCount(false) == 1);
	plugin::AudioPortInfo port;
	REQUIRE(ports.audioPortInfo(true, 0, port));
	CHECK(port.name == "Input");
	CHECK(port.channelCount == 2);
	CHECK(port.layout == plugin::ChannelLayout::Stereo);
	CHECK(port.flags == plugin::AudioPortInfo::Main);
	REQUIRE(ports.audioPortInfo(true, 1, port));
	CHECK(port.name == "Sidechain");
	CHECK(port.flags == plugin::AudioPortInfo::Optional);

	auto& notes = ext<plugin::NotePorts>(*plugin);
	REQUIRE(notes.notePortCount(true) == 1);
	REQUIRE(notes.notePortCount(false) == 1);
	plugin::NotePortInfo notePort;
	REQUIRE(notes.notePortInfo(true, 0, notePort));
	CHECK(notePort.name == "MIDI In");
	CHECK(
		notePort.dialects == (plugin::NotePortInfo::Notes | plugin::NotePortInfo::Midi | plugin::NotePortInfo::Midi2));

	auto& params = ext<plugin::Params>(*plugin);
	REQUIRE(params.paramCount() == ParamCount);
	plugin::ParamInfo info;
	REQUIRE(params.paramInfo(Gain, info));
	CHECK(info.id == Gain);
	CHECK(info.name == "Gain");
	CHECK(info.module == "Amp");
	CHECK(info.flags == plugin::ParamInfo::Automatable);
	CHECK(info.minimum == 0.0);
	CHECK(info.maximum == 1.0);
	CHECK(info.defaultValue == 0.5);
	REQUIRE(params.paramInfo(Bypass, info));
	CHECK(info.flags == (plugin::ParamInfo::Automatable | plugin::ParamInfo::Bypass | plugin::ParamInfo::Stepped));
	CHECK(info.maximum == 1.0);
	CHECK(info.stepCount == 1);
	REQUIRE(params.paramInfo(Mode, info));
	CHECK(info.flags == (plugin::ParamInfo::Automatable | plugin::ParamInfo::Enum | plugin::ParamInfo::Stepped));
	CHECK(info.maximum == 1.0); // VST3 parameters are normalized, stepped ones too, with the step count
	CHECK(info.stepCount == 2);
	CHECK(info.defaultValue == 0.0);
	CHECK(info.module == "Amp");
	REQUIRE(params.paramInfo(Program, info));
	CHECK(info.defaultValue == 1.0); // 0.7 lies between two steps: the last of three, by VST3's rule
	REQUIRE(params.paramInfo(NoteCount, info));
	CHECK(info.flags == plugin::ParamInfo::ReadOnly);
	CHECK(info.module.empty());
	REQUIRE(params.paramInfo(EditTrigger, info));
	CHECK((info.flags & plugin::ParamInfo::Hidden) != 0);
	// A title without a terminator is read up to its 128th character; an unpaired surrogate becomes U+FFFD.
	REQUIRE(params.paramInfo(Odd, info));
	CHECK(info.name ==
		"\xEF\xBF\xBD"
		"Odd" +
			std::string(124, 'x'));

	// Texts come from the controller, for the step a value falls on.
	std::string text;
	REQUIRE(params.paramValueToText(Gain, 0.25, text));
	CHECK(text == "0.50");
	REQUIRE(params.paramValueToText(Mode, 1.0, text));
	CHECK(text == "Mute");
	REQUIRE(params.paramValueToText(Mode, 0.6, text));
	CHECK(text == "Invert");
	REQUIRE(params.paramValueToText(Mode, 0.7, text));
	CHECK(text == "Mute");
	double value = 0.0;
	REQUIRE(params.paramTextToValue(Mode, "Invert", value));
	CHECK(value == 0.5);
	REQUIRE(params.paramTextToValue(Gain, "1.5", value));
	CHECK(value == 0.75);
	CHECK(!params.paramTextToValue(Mode, "Loud", value));
	// The component greeted the controller through a message the host allocated: they are connected.
	REQUIRE(params.paramValueToText(EditTrigger, 0.0, text));
	CHECK(text == "Connected");

	CHECK(valueOf(*plugin, Gain) == 0.5);
	CHECK(plugin->extension("gnubase.gui") == nullptr); // editors are #69
}

TEST_CASE("a VST3 plugin processes audio with sample-accurate parameter changes and its latency", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	CHECK(ext<plugin::Latency>(*plugin).latency() == g_defaultLatency);
	CHECK(ext<plugin::Tail>(*plugin).tail() == g_tail);
	Runner runner(*plugin);

	// Gain 0.5 (x1) until frame 100 of the first block, then 1.0 (x2); Mode Invert from frame 30 of the second.
	const std::vector<float> first = ramp(128);
	const std::vector<float> second = ramp(128, 0.2f);
	std::vector<float> output = runner.run(first, {paramEvent(100, Gain, 1.0)});
	std::vector<float> more = runner.run(second, {paramEvent(30, Mode, 0.5)});
	output.insert(output.end(), more.begin(), more.end());

	std::vector<float> input = first;
	input.insert(input.end(), second.begin(), second.end());
	for (size_t i = 0; i < output.size(); ++i)
	{
		// The plugin delays by its latency.
		float expected = 0.0f;
		if (i >= g_defaultLatency)
		{
			const size_t n = i - g_defaultLatency;
			const float gain = n < 100 ? 1.0f : 2.0f;
			const float sign = n >= 128 + 30 ? -1.0f : 1.0f;
			expected = input[n] * gain * sign;
		}
		INFO("frame " << i);
		REQUIRE(output[i] == expected);
	}

	// The controller learns the host's values on the main thread.
	CHECK(fixture.host.callbacks > 0);
	plugin->onMainThread();
	CHECK(valueOf(*plugin, Gain) == 1.0);
	CHECK(valueOf(*plugin, Mode) == 0.5);
	stop(*plugin);
}

TEST_CASE("notes, expressions, MIDI 1.0, MIDI 2.0 and sysex reach a VST3 plugin and come back", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	Runner runner(*plugin);

	std::vector<plugin::Event> events;
	events.push_back(noteEvent(plugin::EventType::NoteOn, 10, 60, 0.5));
	plugin::Event tuning = noteEvent(plugin::EventType::NoteExpression, 20, 60, 0.0);
	tuning.expression = plugin::expressionId(plugin::NoteExpression::Tuning);
	tuning.value = 12.0;
	events.push_back(tuning);
	// The plugin's own expressions are its VST3 note expression types, from g_firstPluginExpression on.
	plugin::Event custom = noteEvent(plugin::EventType::NoteExpression, 25, 60, 0.3);
	custom.expression = g_customExpression;
	events.push_back(custom);
	plugin::Event foreign = noteEvent(plugin::EventType::NoteOn, 26, 70, 1.0);
	foreign.space = 7; // an extension's namespace, which means nothing to VST3
	events.push_back(foreign);
	plugin::Event midi;
	midi.offset = 30;
	midi.type = plugin::EventType::Midi;
	midi.port = 0;
	midi.data[0] = 0x90 | (64 << 8) | (100 << 16); // note on, key 64, velocity 100
	events.push_back(midi);
	plugin::Event midi2;
	midi2.offset = 40;
	midi2.type = plugin::EventType::Midi2;
	midi2.port = 0;
	midi2.data[0] = (0x4U << 28) | (0x91U << 16) | (62U << 8); // MIDI 2.0 note on, channel 2, key 62
	midi2.data[1] = 0xffffU << 16;                             // full velocity
	events.push_back(midi2);
	plugin::Event cc = midi;
	cc.offset = 45;
	cc.data[0] = 0xb0 | (uint32_t{g_gainController} << 8) | (127 << 16); // CC 7, mapped to Gain
	events.push_back(cc);
	events.push_back(noteEvent(plugin::EventType::NoteOff, 60, 60, 0.0));
	runner.run(ramp(128), events);

	const plugin::EventList& out = runner.out;
	const plugin::Event* noteOn =
		find(out, [](const plugin::Event& e) { return e.type == plugin::EventType::NoteOn && e.key == 60; });
	REQUIRE(noteOn != nullptr);
	CHECK(noteOn->offset == 10);
	CHECK(noteOn->value == Approx(0.5));
	CHECK(noteOn->noteId >= 0); // the adapter gives every note an id
	const int32_t noteId = noteOn->noteId;

	const plugin::Event* expression =
		find(out, [](const plugin::Event& e) { return e.type == plugin::EventType::NoteExpression; });
	REQUIRE(expression != nullptr);
	CHECK(expression->offset == 20);
	CHECK(expression->expression == plugin::expressionId(plugin::NoteExpression::Tuning));
	CHECK(expression->noteId == noteId);
	CHECK(expression->value == Approx(12.0));
	const plugin::Event* customOut = find(out, [](const plugin::Event& e)
		{ return e.type == plugin::EventType::NoteExpression && e.expression == g_customExpression; });
	REQUIRE(customOut != nullptr);
	CHECK(customOut->offset == 25);
	CHECK(customOut->noteId == noteId);
	CHECK(customOut->value == Approx(0.3));
	CHECK(find(out, [](const plugin::Event& e) { return e.key == 70; }) == nullptr);
	const plugin::Event* lastTuning =
		find(out, [](const plugin::Event& e) { return isParam(e, plugin::EventType::ParamValue, LastTuning); });
	REQUIRE(lastTuning != nullptr);
	CHECK(lastTuning->value == Approx((12.0 / 240.0) + 0.5));
	CHECK((lastTuning->flags & plugin::Event::DontRecord) != 0);

	const plugin::Event* fromMidi =
		find(out, [](const plugin::Event& e) { return e.type == plugin::EventType::NoteOn && e.key == 64; });
	REQUIRE(fromMidi != nullptr);
	CHECK(fromMidi->offset == 30);
	CHECK(fromMidi->value == Approx(100.0 / 127.0));
	const plugin::Event* fromMidi2 =
		find(out, [](const plugin::Event& e) { return e.type == plugin::EventType::NoteOn && e.key == 62; });
	REQUIRE(fromMidi2 != nullptr);
	CHECK(fromMidi2->offset == 40);
	CHECK(fromMidi2->channel == 1);
	CHECK(fromMidi2->value == Approx(1.0));

	// CC 7 changed Gain through the plugin's MIDI mapping, and the engine's mirror learns it.
	const plugin::Event* gain =
		find(out, [](const plugin::Event& e) { return isParam(e, plugin::EventType::ParamValue, Gain); });
	REQUIRE(gain != nullptr);
	CHECK(gain->offset == 45);
	CHECK(gain->value == 1.0);

	const plugin::Event* noteOff =
		find(out, [](const plugin::Event& e) { return e.type == plugin::EventType::NoteOff && e.key == 60; });
	REQUIRE(noteOff != nullptr);
	CHECK(noteOff->offset == 60);
	CHECK(noteOff->noteId == noteId);

	// Three note ons, counted by the plugin at their frames.
	const plugin::Event* lastCount = nullptr;
	for (const plugin::Event& event : out)
	{
		if (isParam(event, plugin::EventType::ParamValue, NoteCount))
			lastCount = &event;
	}
	REQUIRE(lastCount != nullptr);
	CHECK(lastCount->offset == 40);
	CHECK(lastCount->value == Approx(3.0 / 128.0));
	// Output events are in order.
	CHECK(std::is_sorted(
		out.begin(), out.end(), [](const plugin::Event& a, const plugin::Event& b) { return a.offset < b.offset; }));

	// System exclusive messages pass with their bytes.
	const std::vector<uint8_t> sysex{0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7};
	runner.prepare(ramp(64));
	runner.prepareSysex(5, sysex);
	REQUIRE(runner.process() == plugin::ProcessStatus::Continue);
	const plugin::Event* echoed =
		find(runner.out, [](const plugin::Event& e) { return e.type == plugin::EventType::Sysex; });
	REQUIRE(echoed != nullptr);
	CHECK(echoed->offset == 5);
	REQUIRE(echoed->data[1] == sysex.size());
	CHECK(std::memcmp(runner.out.bytes(*echoed), sysex.data(), sysex.size()) == 0);
	stop(*plugin);
}

TEST_CASE(
	"edits in a VST3 plugin's controller become gestures and values for the host and the processor", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	auto& params = ext<plugin::Params>(*plugin);

	// While the plugin doesn't process, flushParams gives the controller the value at once: it edits Gain.
	plugin::EventList in(4);
	plugin::EventList out(16);
	REQUIRE(in.push(paramEvent(0, EditTrigger, 1.0)));
	params.flushParams(in, out);
	REQUIRE(out.size() == 3);
	CHECK(isParam(out[0], plugin::EventType::ParamGestureBegin, Gain));
	CHECK(isParam(out[1], plugin::EventType::ParamValue, Gain));
	CHECK(out[1].value == 0.25);
	CHECK((out[1].flags & plugin::Event::DontRecord) == 0); // the user's edit is recorded
	CHECK(isParam(out[2], plugin::EventType::ParamGestureEnd, Gain));
	CHECK(fixture.host.flushRequests > 0);
	CHECK(valueOf(*plugin, Gain) == 0.25);

	// The processor learns it with its first block: gain x0.5.
	start(*plugin);
	Runner runner(*plugin);
	const std::vector<float> input = ramp(128);
	std::vector<float> output = runner.run(input);
	CHECK(output[g_defaultLatency + 10] == input[10] * 0.5f);

	// While it processes, the controller learns the host's value on the main thread, and its edit comes out of the
	// next block, at its start.
	runner.run(input, {paramEvent(0, EditTrigger, 1.0)});
	plugin->onMainThread();
	runner.run(input);
	REQUIRE(runner.out.size() >= 3);
	CHECK(isParam(runner.out[0], plugin::EventType::ParamGestureBegin, Gain));
	CHECK(isParam(runner.out[1], plugin::EventType::ParamValue, Gain));
	CHECK(runner.out[1].offset == 0);
	CHECK(isParam(runner.out[2], plugin::EventType::ParamGestureEnd, Gain));

	// flushParams while processing runs the processor without audio.
	in.clear();
	out.clear();
	REQUIRE(in.push(paramEvent(0, Gain, 1.0)));
	params.flushParams(in, out);
	output = runner.run(input);
	CHECK(output[g_defaultLatency + 10] == input[10] * 2.0f);
	stop(*plugin);
}

TEST_CASE("transport changes split a block for a VST3 plugin", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	Runner runner(*plugin);
	const std::vector<float> input = ramp(128);
	const std::vector<float> output =
		runner.run(input, {paramEvent(100, Gain, 1.0)}, {segment(0, 120.0), segment(64, 150.0)});

	// One process call per segment: the plugin reports the tempo at the start of each.
	std::vector<std::pair<uint32_t, double>> tempos;
	for (const plugin::Event& event : runner.out)
	{
		if (isParam(event, plugin::EventType::ParamValue, Tempo))
			tempos.emplace_back(event.offset, event.value);
	}
	REQUIRE(tempos.size() == 2);
	CHECK(tempos[0].first == 0);
	CHECK(tempos[0].second == Approx(0.12));
	CHECK(tempos[1].first == 64);
	CHECK(tempos[1].second == Approx(0.15));
	// The audio is the same as without the split, with the gain change in the second sub-block.
	for (size_t i = g_defaultLatency; i < output.size(); ++i)
		REQUIRE(output[i] == input[i - g_defaultLatency]);
	stop(*plugin);
}

TEST_CASE("a VST3 plugin's state is a preset file with both states", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	plugin::EventList in(4);
	plugin::EventList out(16);
	REQUIRE(in.push(paramEvent(0, Gain, 0.8)));
	REQUIRE(in.push(paramEvent(0, Mode, 1.0)));
	ext<plugin::Params>(*plugin).flushParams(in, out);
	in.clear();
	REQUIRE(in.push(paramEvent(0, EditTrigger, 1.0))); // counted in the controller's state, sets Gain to 0.25
	ext<plugin::Params>(*plugin).flushParams(in, out);
	in.clear();
	REQUIRE(in.push(paramEvent(0, Gain, 0.8)));
	ext<plugin::Params>(*plugin).flushParams(in, out);
	// The component learns values with its next block.
	start(*plugin);
	Runner(*plugin).run(ramp(16));
	stop(*plugin);

	std::vector<uint8_t> state;
	REQUIRE(ext<plugin::State>(*plugin).saveState(state, plugin::StateContext::Project));
	pluginhost::vst3::PresetFile preset;
	REQUIRE(pluginhost::vst3::readPresetFile(state.data(), state.size(), preset));
	CHECK(preset.classId == g_classId);
	CHECK(preset.hasController);
	CHECK(preset.controller.size() == 8);

	const std::unique_ptr<plugin::Plugin> other = fixture.create();
	fixture.host.rescans = 0;
	REQUIRE(ext<plugin::State>(*other).loadState(state.data(), state.size(), plugin::StateContext::Project));
	CHECK(fixture.host.rescans == plugin::HostParams::Values);
	CHECK(valueOf(*other, Gain) == Approx(0.8));
	CHECK(valueOf(*other, Mode) == 1.0);
	std::vector<uint8_t> again;
	REQUIRE(ext<plugin::State>(*other).saveState(again, plugin::StateContext::Project));
	CHECK(again == state);

	// A state of another class, or no preset file at all, is refused.
	preset.classId = "00000000000000000000000000000000";
	const std::vector<uint8_t> foreign = pluginhost::vst3::writePresetFile(preset);
	CHECK(!ext<plugin::State>(*other).loadState(foreign.data(), foreign.size(), plugin::StateContext::Project));
	const std::vector<uint8_t> garbage(64, 0x55);
	CHECK(!ext<plugin::State>(*other).loadState(garbage.data(), garbage.size(), plugin::StateContext::Project));
}

TEST_CASE("a VST3 plugin's latency change asks for a restart", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	Runner runner(*plugin);
	runner.run(ramp(64), {paramEvent(0, Latency, 0.1)});
	CHECK(fixture.host.restarts == 0);
	plugin->onMainThread(); // the controller learns the value and restarts the component
	CHECK(fixture.host.latencyChanges == 1);
	CHECK(fixture.host.restarts == 1);
	stop(*plugin);
	REQUIRE(plugin->activate(g_sampleRate, 1, g_maxFrames));
	CHECK(ext<plugin::Latency>(*plugin).latency() == 100);
	plugin->deactivate();
}

TEST_CASE("a VST3 plugin's program list gives presets and note names", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	auto& presets = ext<plugin::Presets>(*plugin);
	const std::vector<plugin::PresetInfo> list = presets.presets();
	REQUIRE(list.size() == 3);
	CHECK(list[0].name == "Init");
	CHECK(list[1].name == "Loud");
	CHECK(list[2].name == "Quiet");
	CHECK(list[1].category == "Factory");
	CHECK(list[1].flags == plugin::PresetInfo::Factory);
	CHECK(list[1].location.empty());

	REQUIRE(presets.loadPreset(list[1]));
	CHECK(valueOf(*plugin, Program) == 0.5);
	CHECK(valueOf(*plugin, Gain) == 1.0); // the controller follows its program
	CHECK((fixture.host.rescans & plugin::HostParams::Values) != 0);
	// The processor gets the program change with its next block: Loud is x2.
	start(*plugin);
	Runner runner(*plugin);
	const std::vector<float> input = ramp(128);
	const std::vector<float> output = runner.run(input);
	CHECK(output[g_defaultLatency + 3] == input[3] * 2.0f);
	stop(*plugin);

	plugin::PresetInfo unknown = list[1];
	unknown.key = "7:0";
	CHECK(!presets.loadPreset(unknown));

	const std::vector<plugin::NoteName> names = ext<plugin::NoteNames>(*plugin).noteNames();
	REQUIRE(names.size() == 2);
	CHECK(names[0].key == 36);
	CHECK(names[0].name == "Kick");
	CHECK(names[1].key == 38);
	CHECK(names[1].name == "Snare");
}

TEST_CASE("a VST3 plugin's buses take other layouts and the side-chain can be activated", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	auto& configurable = ext<plugin::ConfigurableAudioPorts>(*plugin);
	auto& ports = ext<plugin::AudioPorts>(*plugin);

	// Mono on the main input and output: the plugin accepts it.
	std::vector<plugin::AudioPortRequest> requests(2);
	requests[0] = {true, 0, 1, plugin::ChannelLayout::Mono, 0};
	requests[1] = {false, 0, 1, plugin::ChannelLayout::Mono, 0};
	REQUIRE(configurable.applyAudioPortRequests(requests));
	plugin::AudioPortInfo info;
	REQUIRE(ports.audioPortInfo(false, 0, info));
	CHECK(info.channelCount == 1);
	CHECK(info.layout == plugin::ChannelLayout::Mono);

	// 5.1 is refused, and nothing changes.
	requests.resize(1);
	requests[0] = {false, 0, 6, plugin::ChannelLayout::Surround, 0x3f};
	CHECK(!configurable.applyAudioPortRequests(requests));
	REQUIRE(ports.audioPortInfo(false, 0, info));
	CHECK(info.layout == plugin::ChannelLayout::Mono);
	requests[0] = {true, 0, 2, plugin::ChannelLayout::Stereo, 0};
	requests.push_back({false, 0, 2, plugin::ChannelLayout::Stereo, 0});
	REQUIRE(configurable.applyAudioPortRequests(requests));

	// The side-chain is off by default; activated, it is mixed into the output.
	auto& activation = ext<plugin::AudioPortsActivation>(*plugin);
	REQUIRE(activation.setAudioPortActive(true, 1, true));
	start(*plugin);
	CHECK(!activation.setAudioPortActive(true, 1, false)); // only while inactive
	Runner runner(*plugin);
	const std::vector<float> input = ramp(128);
	const std::vector<float> side = ramp(128, 0.5f);
	const std::vector<float> output = runner.run(input, {}, {}, side);
	CHECK(output[g_defaultLatency + 7] == input[7] + side[7]);
	stop(*plugin);
}

TEST_CASE("the VST3 preset file keeps both states and rejects damaged files", "[unit]")
{
	pluginhost::vst3::PresetFile preset;
	preset.classId = g_classId;
	preset.component = {1, 2, 3};
	preset.controller = {4, 5};
	preset.hasController = true;
	const std::vector<uint8_t> bytes = pluginhost::vst3::writePresetFile(preset);
	REQUIRE(bytes.size() == 48 + 5 + 8 + (2 * 20));
	CHECK(std::memcmp(bytes.data(), "VST3", 4) == 0);

	pluginhost::vst3::PresetFile read;
	REQUIRE(pluginhost::vst3::readPresetFile(bytes.data(), bytes.size(), read));
	CHECK(read.classId == preset.classId);
	CHECK(read.component == preset.component);
	CHECK(read.controller == preset.controller);
	CHECK(read.hasController);

	for (size_t size = 0; size < bytes.size(); ++size)
		CHECK(!pluginhost::vst3::readPresetFile(bytes.data(), size, read));
	std::vector<uint8_t> damaged = bytes;
	damaged[bytes.size() - 8] = 0xff; // the second chunk's size beyond the end
	CHECK(!pluginhost::vst3::readPresetFile(damaged.data(), damaged.size(), read));
}

TEST_CASE("a VST3 plugin processes in the plugin host process as it does in-process", "[integration]")
{
	engine::plugins::HostManager manager(hostOptions());
	std::unique_ptr<plugin::remote::RemotePlugin> remote =
		manager.create({pluginId(), GNUBASE_VST3_ADAPTERTEST, "GNUbase"});
	REQUIRE(remote->init());
	CHECK(remote->descriptor().name == "Adapter Test");
	CHECK(remote->paramCount() == ParamCount);
	std::string text;
	REQUIRE(remote->paramValueToText(Gain, 0.25, text));
	CHECK(text == "0.50");

	Vst3Fixture fixture;
	SlotGraph local(fixture.create());
	SlotGraph hosted(std::move(remote));
	bool sounded = false;
	for (uint32_t block = 0; block < 12; ++block)
	{
		const std::vector<float> input = ramp(200, 0.01f * static_cast<float>(block));
		const uint32_t offset = (block * 37) % 200;
		const double gain = 0.1 * static_cast<double>(block % 10);
		const std::vector<float> expected = local.render(input, Gain, gain, offset, true);
		const std::vector<float> actual = hosted.render(input, Gain, gain, offset, true);
		REQUIRE(actual == expected);
		sounded = sounded || !silent(actual);
	}
	CHECK(sounded);
	CHECK(!hosted.slot().failed());
}

TEST_CASE("a VST3 plugin processes notes, MIDI, parameters and transport changes without allocating", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	Runner runner(*plugin);
	plugin::EventList flushIn(4);
	plugin::EventList flushOut(64);
	REQUIRE(flushIn.push(paramEvent(0, Mode, 0.5)));

	plugin::Event midi;
	midi.offset = 50;
	midi.type = plugin::EventType::Midi;
	midi.port = 0;
	midi.data[0] = 0xb0 | (uint32_t{g_gainController} << 8) | (64 << 16);
	plugin::Event tuning = noteEvent(plugin::EventType::NoteExpression, 40, 60, 0.0);
	tuning.expression = plugin::expressionId(plugin::NoteExpression::Tuning);
	tuning.value = -3.0;
	const std::vector<plugin::Event> events{noteEvent(plugin::EventType::NoteOn, 0, 60, 0.8), paramEvent(10, Gain, 0.3),
		tuning, midi, noteEvent(plugin::EventType::NoteOff, 90, 60, 0.0)};
	const std::vector<plugin::TransportSegment> transport{segment(0, 120.0), segment(70, 90.0)};
	const std::vector<uint8_t> sysex{0xf0, 0x01, 0x02, 0xf7};
	// An edit in the controller, which the next block reports.
	runner.prepare(ramp(128), {paramEvent(0, EditTrigger, 1.0)});
	REQUIRE(runner.process() == plugin::ProcessStatus::Continue);
	plugin->onMainThread();

	const std::atomic<int>& violations = g_violations;
	const rt::ViolationHandler previous = rt::setViolationHandler(&countViolation);
	g_violations = 0;
	for (int block = 0; block < 4; ++block)
	{
		runner.prepare(ramp(128), events, transport);
		runner.prepareSysex(100, sysex);
		plugin::ProcessStatus status = plugin::ProcessStatus::Error;
		{
			const rt::ScopedRealtimeContext realtime;
			status = runner.process();
			flushIn.clear();
			REQUIRE(flushIn.push(paramEvent(0, Mode, block % 2 == 0 ? 0.0 : 0.5)));
		}
		REQUIRE(status == plugin::ProcessStatus::Continue);
		REQUIRE(!runner.out.empty());
		{
			const rt::ScopedRealtimeContext realtime;
			flushOut.clear();
			plugin::extension<plugin::Params>(*plugin)->flushParams(flushIn, flushOut);
		}
	}
	rt::setViolationHandler(previous);
	CHECK(violations == 0);
	stop(*plugin);
}

// ---- The shared test plugins (tests/plugins/README.md): components that are their own controllers ----

TEST_CASE("the VST3 adapter loads, activates and processes every VST3 test plugin", "[integration]")
{
	for (const std::string name : {"gain", "synth", "latency", "paramstress", "crash", "hang", "stateblob"})
	{
		INFO(name);
		Vst3Fixture fixture(testPlugin(name));
		REQUIRE(fixture.factory().count() == 1);
		const plugin::Descriptor& descriptor = fixture.factory().descriptor(0);
		CHECK(descriptor.id == "vst3:" + descriptor.formatId);
		CHECK(descriptor.formatId.size() == 32);
		CHECK(descriptor.vendor == "GNUbase");
		const bool instrument = std::find(descriptor.features.begin(), descriptor.features.end(), "instrument") !=
			descriptor.features.end();
		CHECK(instrument == (name == "synth"));

		const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
		CHECK(ext<plugin::Params>(*plugin).paramCount() > 0);
		auto& ports = ext<plugin::AudioPorts>(*plugin);
		REQUIRE(ports.audioPortCount(false) == 1);
		plugin::AudioPortInfo info;
		REQUIRE(ports.audioPortInfo(false, 0, info));
		CHECK(info.channelCount == 2);
		CHECK(ports.audioPortCount(true) == (name == "synth" ? 0U : 1U));

		// A block without events is safe for crash and hang too.
		start(*plugin);
		Runner runner(*plugin);
		runner.run(ramp(64));
		stop(*plugin);
	}
}

TEST_CASE("the VST3 gain test plugin sounds the same through the plugin host process", "[integration]")
{
	Vst3Fixture fixture(testPlugin("gain"));
	const std::string id = fixture.factory().descriptor(0).id;
	engine::plugins::HostManager manager(hostOptions());
	std::unique_ptr<plugin::remote::RemotePlugin> remote = manager.create({id, testPlugin("gain"), "GNUbase"});
	REQUIRE(remote->init());
	SlotGraph local(fixture.create());
	SlotGraph hosted(std::move(remote));

	// Parameter 1 is the normalized gain, -60 to +12 dB; it starts at 0 dB.
	constexpr uint32_t gainParam = 1;
	const std::vector<float> input = ramp(128, 0.1f);
	const std::vector<float> unity = local.render(input);
	CHECK(std::equal(input.begin(), input.end(), unity.begin()));
	CHECK(hosted.render(input) == unity);
	for (uint32_t block = 1; block < 10; ++block)
	{
		const double value = 0.1 * static_cast<double>(block);
		const uint32_t offset = (block * 29) % 128;
		const std::vector<float> expected = local.render(input, gainParam, value, offset, true);
		REQUIRE(hosted.render(input, gainParam, value, offset, true) == expected);
		CHECK(expected[offset] != input[offset]); // the new gain applies from the event's sample
	}
	CHECK(!hosted.slot().failed());
}

TEST_CASE("the VST3 synth test plugin plays notes through the adapter", "[integration]")
{
	Vst3Fixture fixture(testPlugin("synth"));
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	REQUIRE(ext<plugin::NotePorts>(*plugin).notePortCount(true) == 1);
	start(*plugin);
	Runner runner(*plugin);
	const std::vector<float> nothing(128);
	CHECK(silent(runner.run(nothing)));
	const std::vector<float> note = runner.run(nothing, {noteEvent(plugin::EventType::NoteOn, 32, 69, 0.8)});
	CHECK(std::all_of(note.begin(), note.begin() + 32, [](float x) { return x == 0.0f; }));
	CHECK(!silent(note));
	CHECK(!silent(runner.run(nothing)));
	runner.run(nothing, {noteEvent(plugin::EventType::NoteOff, 0, 69, 0.0)});
	// The release lasts 20 ms by default.
	std::vector<float> last;
	for (int block = 0; block < 20; ++block)
		last = runner.run(nothing);
	CHECK(silent(last));
	stop(*plugin);
}

TEST_CASE("the VST3 latency test plugin's latency change restarts it through the adapter", "[integration]")
{
	Vst3Fixture fixture(testPlugin("latency"));
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	CHECK(ext<plugin::Latency>(*plugin).latency() == 64);
	Runner runner(*plugin);
	const std::vector<float> input = ramp(128);
	const std::vector<float> output = runner.run(input);
	for (size_t i = 0; i < output.size(); ++i)
		REQUIRE(output[i] == (i < 64 ? 0.0f : input[i - 64]));

	// A stepped parameter of 48000 steps, one per sample, with VST3's normalized values from 0 to 1.
	constexpr uint32_t latencyParam = 1;
	plugin::ParamInfo info;
	REQUIRE(ext<plugin::Params>(*plugin).paramInfo(0, info));
	CHECK(info.id == latencyParam);
	CHECK(info.maximum == 1.0);
	CHECK(info.stepCount == 48000);
	runner.run(input, {paramEvent(0, latencyParam, 100.0 / 48000.0)});
	CHECK(fixture.host.restarts == 0);
	plugin->onMainThread(); // the controller learns the value and passes the plugin's restart on
	CHECK(fixture.host.latencyChanges == 1);
	CHECK(fixture.host.restarts == 1);
	stop(*plugin);
	REQUIRE(plugin->activate(g_sampleRate, 1, g_maxFrames));
	CHECK(ext<plugin::Latency>(*plugin).latency() == 100);
	plugin->deactivate();
}

TEST_CASE(
	"the VST3 parameter stress test plugin's 1024 parameters and their automation pass the adapter", "[integration]")
{
	Vst3Fixture fixture(testPlugin("paramstress"));
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	auto& params = ext<plugin::Params>(*plugin);
	REQUIRE(params.paramCount() == 1024);
	std::vector<uint32_t> ids;
	bool contiguous = true;
	bool inRange = true;
	for (uint32_t i = 0; i < params.paramCount(); ++i)
	{
		plugin::ParamInfo info;
		REQUIRE(params.paramInfo(i, info));
		contiguous = contiguous && info.id == i;
		ids.push_back(info.id);
		double value = -1.0;
		REQUIRE(params.paramValue(info.id, value));
		inRange = inRange && value >= info.minimum && value <= info.maximum;
	}
	CHECK(!contiguous);
	CHECK(inRange);
	std::sort(ids.begin(), ids.end());
	CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

	// "Emit automation" on: every block changes eight parameters, which the host learns as the plugin's own changes.
	constexpr uint32_t emitParam = 1;
	start(*plugin);
	Runner runner(*plugin);
	runner.run(ramp(64), {paramEvent(0, emitParam, 1.0)});
	runner.run(ramp(64));
	std::vector<plugin::Event> changes;
	for (const plugin::Event& event : runner.out)
	{
		if (event.type == plugin::EventType::ParamValue && event.param != emitParam)
			changes.push_back(event);
	}
	REQUIRE(changes.size() >= 8);
	for (const plugin::Event& event : changes)
	{
		CHECK((event.flags & plugin::Event::DontRecord) != 0);
		CHECK(std::binary_search(ids.begin(), ids.end(), event.param));
	}
	plugin->onMainThread();
	CHECK(valueOf(*plugin, changes.back().param) == Approx(changes.back().value));
	stop(*plugin);
}

TEST_CASE("the VST3 state blob test plugin's state survives the adapter's preset file", "[integration]")
{
	Vst3Fixture fixture(testPlugin("stateblob"));
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	constexpr uint32_t seedParam = 1;
	constexpr uint32_t sizeParam = 2;
	plugin::EventList in(4);
	plugin::EventList out(16);
	// Both are stepped, one step per value, with VST3's normalized values from 0 to 1.
	constexpr double seed = 7.0 / 65535.0;
	constexpr double size = 100000.0 / (16.0 * 1024 * 1024);
	REQUIRE(in.push(paramEvent(0, seedParam, seed)));
	REQUIRE(in.push(paramEvent(0, sizeParam, size)));
	ext<plugin::Params>(*plugin).flushParams(in, out);
	CHECK(valueOf(*plugin, sizeParam) == Approx(size));

	std::vector<uint8_t> state;
	REQUIRE(ext<plugin::State>(*plugin).saveState(state, plugin::StateContext::Project));
	CHECK(state.size() > 100000);
	const std::unique_ptr<plugin::Plugin> other = fixture.create();
	REQUIRE(ext<plugin::State>(*other).loadState(state.data(), state.size(), plugin::StateContext::Project));
	CHECK(valueOf(*other, seedParam) == Approx(seed));
	std::vector<uint8_t> again;
	REQUIRE(ext<plugin::State>(*other).saveState(again, plugin::StateContext::Project));
	CHECK(again == state);

	// A damaged blob is refused by the plugin, and so by the adapter.
	pluginhost::vst3::PresetFile preset;
	REQUIRE(pluginhost::vst3::readPresetFile(state.data(), state.size(), preset));
	preset.component[preset.component.size() / 2] ^= 0x55;
	const std::vector<uint8_t> damaged = pluginhost::vst3::writePresetFile(preset);
	CHECK(!ext<plugin::State>(*other).loadState(damaged.data(), damaged.size(), plugin::StateContext::Project));
}

TEST_CASE("a crashing VST3 plugin takes down only its plugin host process", "[integration]")
{
	const Vst3Fixture fixture(testPlugin("crash"));
	const std::string id = fixture.factory().descriptor(0).id;
	engine::plugins::HostManager manager(hostOptions());
	std::unique_ptr<plugin::remote::RemotePlugin> remote = manager.create({id, testPlugin("crash"), "GNUbase"});
	REQUIRE(remote->init());
	SlotGraph graph(std::move(remote));
	const std::vector<float> input = ramp(128, 0.1f);
	CHECK(!silent(graph.render(input)));

	constexpr uint32_t crashParam = 1;
	CHECK(silent(graph.render(input, crashParam, 1.0, 10, true)));
	CHECK(graph.slot().failed());
	CHECK(silent(graph.render(input)));
	std::vector<engine::plugins::PluginFailure> failures;
	CHECK(eventually(
		[&]
		{
			failures = manager.takeFailures();
			return !failures.empty();
		}));
	REQUIRE(failures.size() == 1);
	CHECK(failures[0].plugin == id);
	CHECK(failures[0].reason == "crashed");
}

TEST_CASE("a hanging VST3 plugin is detected within the deadline", "[integration]")
{
	const Vst3Fixture fixture(testPlugin("hang"));
	const std::string id = fixture.factory().descriptor(0).id;
	engine::plugins::HostManager::Options options = hostOptions();
	options.deadlines.realtimeMinimum = 300ms;
	options.deadlines.realtimeMaximum = 300ms;
	engine::plugins::HostManager manager(options);
	std::unique_ptr<plugin::remote::RemotePlugin> remote = manager.create({id, testPlugin("hang"), "GNUbase"});
	REQUIRE(remote->init());
	SlotGraph graph(std::move(remote), plugin::ProcessMode::Realtime);
	const std::vector<float> input = ramp(128, 0.1f);
	CHECK(!silent(graph.render(input)));

	constexpr uint32_t hangParam = 1;
	const auto start = std::chrono::steady_clock::now();
	CHECK(silent(graph.render(input, hangParam, 1.0, 0, true)));
	CHECK(std::chrono::steady_clock::now() - start < 1300ms);
	CHECK(graph.slot().failed());
	std::vector<engine::plugins::PluginFailure> failures;
	CHECK(eventually(
		[&]
		{
			failures = manager.takeFailures();
			return !failures.empty();
		}));
	REQUIRE(failures.size() == 1);
	CHECK(failures[0].plugin == id);
	CHECK(failures[0].reason == "hung");
}

// ---- Stepped values, full queues, bus changes, threads, process mode and class ids ----

TEST_CASE("a VST3 plugin's stepped values follow VST3's rule for steps", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	Runner runner(*plugin);

	// Of Mode's three steps, 0.6 is the second (Invert) and 0.7 the third (Mute), as the plugin counts them, and the
	// mirror's value is the step's.
	const std::vector<float> input = ramp(128);
	std::vector<float> output = runner.run(input, {paramEvent(0, Mode, 0.6)});
	CHECK(output[g_defaultLatency + 5] == -input[5]);
	plugin->onMainThread();
	CHECK(valueOf(*plugin, Mode) == 0.5);
	output = runner.run(input, {paramEvent(0, Mode, 0.7)});
	CHECK(output[g_defaultLatency + 5] == 0.0f);
	plugin->onMainThread();
	CHECK(valueOf(*plugin, Mode) == 1.0);
	stop(*plugin);
}

TEST_CASE(
	"a VST3 plugin's processor gets a change of each of 600 parameters set while it doesn't process", "[integration]")
{
	Vst3Fixture fixture(testPlugin("paramstress"));
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	auto& params = ext<plugin::Params>(*plugin);
	REQUIRE(params.paramCount() > 600);

	// Active but not processing: the processor learns the values with its next block. While it is active, the test
	// plugin's values are the processor's, so paramValue shows what the processor got.
	REQUIRE(plugin->activate(g_sampleRate, 1, g_maxFrames));
	plugin::EventList in(1024);
	plugin::EventList out(1024);
	std::vector<std::pair<uint32_t, double>> expected;
	for (uint32_t i = 1; i <= 600; ++i)
	{
		plugin::ParamInfo info;
		REQUIRE(params.paramInfo(i, info));
		// Away from the defaults: the second step of a stepped parameter, 0.25 otherwise.
		const double value = info.stepCount > 0 ? 1.0 / info.stepCount : 0.25;
		REQUIRE(in.push(paramEvent(0, info.id, value)));
		expected.emplace_back(info.id, value);
	}
	params.flushParams(in, out);
	REQUIRE(plugin->startProcessing());
	Runner runner(*plugin);
	runner.run(ramp(64));
	size_t learned = 0;
	for (const auto& change : expected)
		learned += valueOf(*plugin, change.first) == Approx(change.second) ? 1 : 0;
	CHECK(learned == expected.size());
	stop(*plugin);
}

TEST_CASE("a full VST3 parameter queue keeps the last value of a block", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	Runner runner(*plugin);

	// 100 changes of Gain in one block, more than a queue's 64 points: the last one, x2 from frame 99, arrives.
	std::vector<plugin::Event> events;
	events.reserve(100);
	for (uint32_t i = 0; i < 100; ++i)
		events.push_back(paramEvent(i, Gain, i == 99 ? 1.0 : 0.25));
	const std::vector<float> input = ramp(256);
	const std::vector<float> output = runner.run(input, events);
	CHECK(output[g_defaultLatency + 10] == input[10] * 0.5f);
	CHECK(output[g_defaultLatency + 150] == input[150] * 2.0f);
	stop(*plugin);
}

TEST_CASE("a VST3 plugin that changes its buses while active has the host read its ports again", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	{
		Runner runner(*plugin);
		runner.run(ramp(64), {paramEvent(0, Layout, 1.0)});
	}
	plugin->onMainThread(); // the controller learns the value and restarts the component: kIoChanged
	CHECK(fixture.host.restarts == 1);
	CHECK(fixture.host.audioPortRescans == 0); // not while the ports are in use

	// Deactivated for the restart, the plugin has its new buses, and the host reads them.
	stop(*plugin);
	CHECK(fixture.host.audioPortRescans == 1);
	CHECK(fixture.host.notePortRescans == 0); // the event buses stay as they were
	auto& ports = ext<plugin::AudioPorts>(*plugin);
	plugin::AudioPortInfo info;
	REQUIRE(ports.audioPortInfo(false, 0, info));
	CHECK(info.channelCount == 1);
	CHECK(info.layout == plugin::ChannelLayout::Mono);
	REQUIRE(ports.audioPortInfo(true, 0, info));
	CHECK(info.channelCount == 1);

	// Activated again, it processes in mono.
	start(*plugin);
	Runner runner(*plugin);
	const std::vector<float> input = ramp(128);
	const std::vector<float> output = runner.run(input);
	CHECK(output[g_defaultLatency + 3] == input[3]);
	stop(*plugin);
}

TEST_CASE("component handler calls from a VST3 plugin's audio thread wait for its main thread", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	start(*plugin);
	Runner runner(*plugin);
	const std::vector<float> input = ramp(64);

	// The component edits Gain and asks for a values rescan from the audio thread: here a thread of its own.
	runner.prepare(input, {paramEvent(0, AudioEdit, 1.0)});
	plugin::ProcessStatus status = plugin::ProcessStatus::Error;
	std::thread audio([&] { status = runner.process(); });
	audio.join();
	REQUIRE(status == plugin::ProcessStatus::Continue);
	CHECK(fixture.host.rescans == 0);
	CHECK(fixture.host.flushRequests == 0);
	CHECK(fixture.host.callbacks > 0); // the main thread is asked to run them

	// Until the main thread runs, neither the host nor the processor sees them.
	runner.run(input);
	const auto isGesture = [](const plugin::Event& e)
	{ return isParam(e, plugin::EventType::ParamGestureBegin, Gain); };
	CHECK(find(runner.out, isGesture) == nullptr);
	plugin->onMainThread();
	CHECK((fixture.host.rescans & plugin::HostParams::Values) != 0);
	CHECK(fixture.host.flushRequests > 0);
	runner.run(input);
	REQUIRE(runner.out.size() >= 3);
	CHECK(isParam(runner.out[0], plugin::EventType::ParamGestureBegin, Gain));
	CHECK(isParam(runner.out[1], plugin::EventType::ParamValue, Gain));
	CHECK(runner.out[1].value == 0.75);
	CHECK(isParam(runner.out[2], plugin::EventType::ParamGestureEnd, Gain));
	stop(*plugin);
}

TEST_CASE("a VST3 plugin is set up in the process mode the host chose", "[integration]")
{
	Vst3Fixture fixture;
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	const auto modeOf = [](const plugin::EventList& out)
	{
		const plugin::Event* mode =
			find(out, [](const plugin::Event& e) { return isParam(e, plugin::EventType::ParamValue, ProcessMode); });
		return mode != nullptr ? mode->value : -1.0;
	};

	start(*plugin);
	{
		Runner runner(*plugin);
		runner.run(ramp(16));
		CHECK(modeOf(runner.out) == 0.25); // real time, in setupProcessing and in process
	}
	CHECK(!plugin->setProcessMode(plugin::ProcessMode::Offline)); // only while inactive
	stop(*plugin);

	REQUIRE(plugin->setProcessMode(plugin::ProcessMode::Offline));
	start(*plugin);
	Runner runner(*plugin);
	runner.run(ramp(16)); // Process::mode says real time, which only informs
	CHECK(modeOf(runner.out) == 0.75);
	stop(*plugin);
}

TEST_CASE("VST3 class ids are written as other hosts write them and compared without case", "[integration]")
{
	// The test plugin's class id, INLINE_UID(0x6E7A2B11, 0x4C3D4E5F, 0x8A9B0C1D, 0x2E3F4051), as VST3 preset files
	// write it on every OS: in COM order on Windows.
	const std::string known = "6E7A2B114C3D4E5F8A9B0C1D2E3F4051";
	Vst3Fixture fixture;
	CHECK(fixture.factory().descriptor(0).formatId == known);
	CHECK(fixture.factory().descriptor(0).id == "vst3:" + known);
	const std::unique_ptr<plugin::Plugin> plugin = fixture.create();
	std::vector<uint8_t> state;
	REQUIRE(ext<plugin::State>(*plugin).saveState(state, plugin::StateContext::Project));
	pluginhost::vst3::PresetFile preset;
	REQUIRE(pluginhost::vst3::readPresetFile(state.data(), state.size(), preset));
	CHECK(preset.classId == known);

	// Another host's preset with the id in lower case loads, and a plugin id in lower case finds the class.
	preset.classId = "6e7a2b114c3d4e5f8a9b0c1d2e3f4051";
	const std::vector<uint8_t> lower = pluginhost::vst3::writePresetFile(preset);
	CHECK(ext<plugin::State>(*plugin).loadState(lower.data(), lower.size(), plugin::StateContext::Preset));
	CHECK(fixture.factory().create("vst3:6e7a2b114c3d4e5f8a9b0c1d2e3f4051", fixture.host) != nullptr);
}

TEST_CASE("a VST3 plugin in a plugin host process tells the engine about edits, rescans, latency, its state and buses",
	"[integration]")
{
	RecordingHost engineHost;
	engine::plugins::HostManager manager(hostOptions());
	std::unique_ptr<plugin::remote::RemotePlugin> remote =
		manager.create({pluginId(), GNUBASE_VST3_ADAPTERTEST, "GNUbase"}, &engineHost);
	REQUIRE(remote->init());
	auto& params = ext<plugin::Params>(*remote);
	plugin::EventList in(8);
	plugin::EventList out(64);

	// An edit in the controller: its gesture and value come back from flushParams, and the state is dirty.
	REQUIRE(in.push(paramEvent(0, EditTrigger, 1.0)));
	params.flushParams(in, out);
	REQUIRE(out.size() >= 3);
	CHECK(isParam(out[0], plugin::EventType::ParamGestureBegin, Gain));
	CHECK(isParam(out[1], plugin::EventType::ParamValue, Gain));
	CHECK(out[1].value == 0.25);
	CHECK(isParam(out[2], plugin::EventType::ParamGestureEnd, Gain));
	CHECK(valueOf(*remote, Gain) == 0.25);
	CHECK(eventually([&] { return engineHost.dirty > 0; }));

	// A program change: the controller asks for a values rescan, and the engine's mirror follows the program.
	in.clear();
	out.clear();
	REQUIRE(in.push(paramEvent(0, Program, 0.5)));
	params.flushParams(in, out);
	CHECK(eventually([&] { return (engineHost.rescans & plugin::HostParams::Values) != 0; }));
	CHECK(eventually([&] { return valueOf(*remote, Gain) == 1.0; }));

	// A latency change while inactive: a notice, no restart.
	in.clear();
	out.clear();
	REQUIRE(in.push(paramEvent(0, Latency, 0.1)));
	params.flushParams(in, out);
	CHECK(eventually([&] { return engineHost.latencyChanges > 0; }));
	CHECK(engineHost.restarts == 0);

	// A change of buses while active asks for a restart; deactivated, the plugin has the engine read its new ports.
	REQUIRE(remote->activate(g_sampleRate, 1, g_maxFrames));
	REQUIRE(remote->startProcessing());
	{
		Runner runner(*remote);
		runner.run(ramp(64), {paramEvent(0, Layout, 1.0)});
	}
	CHECK(eventually([&] { return engineHost.restarts > 0; }));
	CHECK(engineHost.audioPortRescans == 0);
	remote->stopProcessing();
	remote->deactivate();
	CHECK(eventually([&] { return engineHost.audioPortRescans > 0; }));
	plugin::AudioPortInfo info;
	REQUIRE(remote->audioPortInfo(false, 0, info));
	CHECK(info.channelCount == 1);

	// The process mode reaches setupProcessing in the host process, and the processor applied the latency.
	REQUIRE(remote->setProcessMode(plugin::ProcessMode::Offline));
	REQUIRE(remote->activate(g_sampleRate, 1, g_maxFrames));
	CHECK(remote->latency() == 100);
	REQUIRE(remote->startProcessing());
	{
		Runner runner(*remote);
		runner.run(ramp(64));
	}
	CHECK(valueOf(*remote, ProcessMode) == 0.75);
	remote->stopProcessing();
	remote->deactivate();
}
