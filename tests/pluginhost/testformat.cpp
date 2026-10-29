// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/testformat.h"

#include "plugin/extensions.h"
#include "plugins/common/testplugin.h"

#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

// Every test plugin defines pluginDescriptor() and createPlugin(); tests/plugins/CMakeLists.txt prefixes them with the
// plugin's name for this library, so all of them link into one program.
namespace gnubase::testplugins
{
	const Descriptor& gainPluginDescriptor();
	std::unique_ptr<TestPlugin> gainCreatePlugin(Host& host);
	const Descriptor& synthPluginDescriptor();
	std::unique_ptr<TestPlugin> synthCreatePlugin(Host& host);
	const Descriptor& latencyPluginDescriptor();
	std::unique_ptr<TestPlugin> latencyCreatePlugin(Host& host);
	const Descriptor& paramstressPluginDescriptor();
	std::unique_ptr<TestPlugin> paramstressCreatePlugin(Host& host);
	const Descriptor& crashPluginDescriptor();
	std::unique_ptr<TestPlugin> crashCreatePlugin(Host& host);
	const Descriptor& hangPluginDescriptor();
	std::unique_ptr<TestPlugin> hangCreatePlugin(Host& host);
	const Descriptor& stateblobPluginDescriptor();
	std::unique_ptr<TestPlugin> stateblobCreatePlugin(Host& host);
}

namespace gnubase::testformat
{
	namespace
	{
		using namespace plugin;

		struct Entry
		{
			const char* name;
			const testplugins::Descriptor& (*descriptor)();
			std::unique_ptr<testplugins::TestPlugin> (*create)(testplugins::Host& host);
		};

		// test:stall's parameters.
		constexpr uint32_t g_stallId = 1;  // Stall, in ms
		constexpr uint32_t g_valueId = 2;  // Value, which does nothing
		constexpr uint32_t g_resetsId = 3; // Resets, which counts the plugin's resets

		// test:stall, only in this format: passes its input through, and setting Stall to more than 0 ms makes the
		// plugin take that long once, on the thread that sets it. A parameter event while processing makes that block
		// late, like a transient overrun. Value and Resets show in its state which parameter changes and resets
		// reached it.
		class Stall final : public testplugins::TestPlugin
		{
		public:
			Stall()
				: TestPlugin({param(g_stallId, "Stall", 10000.0, "ms"), param(g_valueId, "Value", 1.0, ""),
					  param(g_resetsId, "Resets", 1.0e9, "")})
			{
			}

			void resetState() override
			{
				double resets = 0.0;
				paramValue(g_resetsId, resets);
				setParam(g_resetsId, resets + 1.0);
			}

		protected:
			void paramChanged(uint32_t id, double value) override
			{
				if (id == g_stallId && value > 0.0)
					std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(value));
			}

		private:
			static testplugins::ParamInfo param(uint32_t id, const char* name, double maximum, const char* unit)
			{
				testplugins::ParamInfo info;
				info.id = id;
				info.name = name;
				info.maxValue = maximum;
				info.unit = unit;
				return info;
			}
		};

		const testplugins::Descriptor& stallDescriptor()
		{
			static const testplugins::Descriptor descriptor{"org.gnubase.test.stall", "GNUbase Test Stall",
				"Takes longer once when its Stall parameter is set", false};
			return descriptor;
		}

		std::unique_ptr<testplugins::TestPlugin> createStall(testplugins::Host& /*host*/)
		{
			return std::make_unique<Stall>();
		}

		const std::array g_entries{
			Entry{"gain", testplugins::gainPluginDescriptor, testplugins::gainCreatePlugin},
			Entry{"synth", testplugins::synthPluginDescriptor, testplugins::synthCreatePlugin},
			Entry{"latency", testplugins::latencyPluginDescriptor, testplugins::latencyCreatePlugin},
			Entry{"paramstress", testplugins::paramstressPluginDescriptor, testplugins::paramstressCreatePlugin},
			Entry{"crash", testplugins::crashPluginDescriptor, testplugins::crashCreatePlugin},
			Entry{"hang", testplugins::hangPluginDescriptor, testplugins::hangCreatePlugin},
			Entry{"stateblob", testplugins::stateblobPluginDescriptor, testplugins::stateblobCreatePlugin},
			Entry{"stall", stallDescriptor, createStall},
		};

		Descriptor describe(const Entry& entry)
		{
			const testplugins::Descriptor& own = entry.descriptor();
			Descriptor descriptor;
			descriptor.id = std::string("test:") + entry.name;
			descriptor.format = "test";
			descriptor.formatId = own.id;
			descriptor.name = own.name;
			descriptor.vendor = "GNUbase";
			descriptor.version = "1";
			descriptor.features = {own.instrument ? "instrument" : "audio-effect"};
			return descriptor;
		}

		bool isPerNote(const Event& event)
		{
			return event.noteId != -1 || event.key != -1;
		}

		EventType noteType(testplugins::EventType type)
		{
			switch (type)
			{
			case testplugins::EventType::NoteOn:
				return EventType::NoteOn;
			case testplugins::EventType::NoteOff:
				return EventType::NoteOff;
			default:
				return EventType::NoteEnd;
			}
		}

		// The block's events as the test plugins see them.
		class EventInput final : public testplugins::EventInput
		{
		public:
			explicit EventInput(const EventList& events) : m_events(events) {}

			uint32_t size() const override { return static_cast<uint32_t>(m_events.size()); }

			bool get(uint32_t index, testplugins::Event& event) const override
			{
				const Event& from = m_events[index];
				if (from.space != g_coreEventSpace)
					return false;
				event.time = from.offset;
				switch (from.type)
				{
				case EventType::NoteOn:
				case EventType::NoteOff:
					event.type = from.type == EventType::NoteOn ? testplugins::EventType::NoteOn
																: testplugins::EventType::NoteOff;
					event.noteId = from.noteId;
					event.port = from.port;
					event.channel = from.channel;
					event.key = from.key;
					event.value = from.value;
					return true;
				case EventType::ParamValue:
					if (isPerNote(from))
						return false;
					event.type = testplugins::EventType::ParamValue;
					event.paramId = from.param;
					event.value = from.value;
					return true;
				default:
					return false;
				}
			}

		private:
			const EventList& m_events;
		};

		class EventOutput final : public testplugins::EventOutput
		{
		public:
			explicit EventOutput(EventList& events) : m_events(events) {}

			bool push(const testplugins::Event& event) override
			{
				Event to;
				to.offset = event.time;
				switch (event.type)
				{
				case testplugins::EventType::NoteOn:
				case testplugins::EventType::NoteOff:
				case testplugins::EventType::NoteEnd:
					to.type = noteType(event.type);
					to.noteId = event.noteId;
					to.port = event.port;
					to.channel = event.channel;
					to.key = event.key;
					to.value = event.value;
					break;
				case testplugins::EventType::ParamValue:
					to.type = EventType::ParamValue;
					to.param = event.paramId;
					to.value = event.value;
					break;
				case testplugins::EventType::GestureBegin:
				case testplugins::EventType::GestureEnd:
					to.type = event.type == testplugins::EventType::GestureBegin ? EventType::ParamGestureBegin
																				 : EventType::ParamGestureEnd;
					to.param = event.paramId;
					break;
				}
				return m_events.push(to);
			}

		private:
			EventList& m_events;
		};

		// A test plugin as a plugin of the plugin API.
		class TestPlugin final : public Plugin,
								 public AudioPorts,
								 public NotePorts,
								 public Params,
								 public State,
								 public Latency,
								 public Tail,
								 private testplugins::Host
		{
		public:
			TestPlugin(const Entry& entry, plugin::Host& host)
				: m_descriptor(describe(entry)), m_instrument(entry.descriptor().instrument), m_host(host),
				  m_core(entry.create(*this))
			{
			}

			const Descriptor& descriptor() const override { return m_descriptor; }

			bool init() override
			{
				m_hostLatency = plugin::extension<HostLatency>(m_host);
				m_hostParams = plugin::extension<HostParams>(m_host);
				return true;
			}

			bool activate(double sampleRate, uint32_t /*minFrames*/, uint32_t maxFrames) override
			{
				m_core->activate(sampleRate, maxFrames);
				// As a CLAP plugin does: a latency that changed is reported while activating.
				const uint32_t latency = m_core->latency();
				if (latency != m_reportedLatency && m_hostLatency != nullptr)
					m_hostLatency->latencyChanged();
				m_reportedLatency = latency;
				return true;
			}

			void deactivate() override { m_core->deactivate(); }
			bool startProcessing() noexcept override { return true; }
			void stopProcessing() noexcept override {}
			void reset() noexcept override { m_core->resetState(); }

			ProcessStatus process(const Process& process) noexcept override
			{
				if (process.audioOutputCount < 1 || (!m_instrument && process.audioInputCount < 1))
					return ProcessStatus::Error;
				const EventInput in(process.eventsIn);
				EventOutput out(process.eventsOut);
				testplugins::ProcessBlock block;
				if (!m_instrument)
				{
					block.inputs = process.audioInputs[0].channels;
					block.inputChannels = process.audioInputs[0].channelCount;
				}
				block.outputs = process.audioOutputs[0].channels;
				block.outputChannels = process.audioOutputs[0].channelCount;
				process.audioOutputs[0].constantMask = 0;
				block.frames = process.frames;
				block.events = &in;
				block.outEvents = &out;
				m_core->process(block);
				return ProcessStatus::Continue;
			}

			void* extension(std::string_view id) override
			{
				return provide<AudioPorts, NotePorts, Params, State, Latency, Tail>(this, id);
			}

			// AudioPorts: one stereo output, and one stereo input for effects.
			uint32_t audioPortCount(bool input) const override { return input && m_instrument ? 0 : 1; }

			bool audioPortInfo(bool input, uint32_t index, AudioPortInfo& info) const override
			{
				if (index >= audioPortCount(input))
					return false;
				info = AudioPortInfo{};
				info.name = input ? "Main In" : "Main Out";
				info.channelCount = testplugins::g_channels;
				info.layout = ChannelLayout::Stereo;
				info.flags = AudioPortInfo::Main;
				return true;
			}

			// NotePorts: one input.
			uint32_t notePortCount(bool input) const override { return input ? 1 : 0; }

			bool notePortInfo(bool input, uint32_t index, NotePortInfo& info) const override
			{
				if (!input || index != 0)
					return false;
				info = NotePortInfo{};
				info.name = "Notes";
				info.dialects = NotePortInfo::Notes;
				return true;
			}

			// Params
			uint32_t paramCount() const override { return static_cast<uint32_t>(m_core->params().size()); }

			bool paramInfo(uint32_t index, ParamInfo& info) const override
			{
				if (index >= paramCount())
					return false;
				const testplugins::ParamInfo& param = m_core->params()[index];
				info = ParamInfo{};
				info.id = param.id;
				info.flags = ParamInfo::Automatable;
				if (param.stepped)
				{
					info.flags |= ParamInfo::Stepped;
					info.stepCount = static_cast<uint32_t>(std::lround(param.maxValue - param.minValue));
				}
				if (param.valueNames != nullptr)
					info.flags |= ParamInfo::Enum;
				info.name = param.name;
				info.minimum = param.minValue;
				info.maximum = param.maxValue;
				info.defaultValue = param.defaultValue;
				return true;
			}

			bool paramValue(uint32_t param, double& value) const override { return m_core->paramValue(param, value); }

			bool paramValueToText(uint32_t param, double value, std::string& text) const override
			{
				std::array<char, 64> buffer{};
				if (!m_core->valueToText(param, value, buffer.data(), buffer.size()))
					return false;
				text = buffer.data();
				return true;
			}

			bool paramTextToValue(uint32_t param, std::string_view text, double& value) const override
			{
				return m_core->textToValue(param, std::string(text).c_str(), value);
			}

			void flushParams(const EventList& in, EventList& /*out*/) noexcept override
			{
				for (const Event& event : in)
				{
					if (event.space == g_coreEventSpace && event.type == EventType::ParamValue && !isPerNote(event))
						m_core->setParam(event.param, event.value);
				}
			}

			// State
			bool saveState(std::vector<uint8_t>& data, StateContext /*context*/) override
			{
				data = m_core->saveState();
				return true;
			}

			bool loadState(const uint8_t* data, size_t size, StateContext /*context*/) override
			{
				if (!m_core->loadState(data, size))
					return false;
				if (m_hostParams != nullptr)
					m_hostParams->rescanParams(HostParams::Values);
				return true;
			}

			// Latency, Tail
			uint32_t latency() const override { return m_core->latency(); }
			uint32_t tail() const noexcept override { return 0; }

		private:
			// testplugins::Host
			void requestRestart() override { m_host.requestRestart(); }

			Descriptor m_descriptor;
			bool m_instrument = false;
			plugin::Host& m_host;
			HostLatency* m_hostLatency = nullptr;
			HostParams* m_hostParams = nullptr;
			uint32_t m_reportedLatency = 0; // as for a CLAP host, no latency until the plugin says so
			std::unique_ptr<testplugins::TestPlugin> m_core;
		};

		class TestFactory final : public Factory
		{
		public:
			TestFactory()
			{
				for (const Entry& entry : g_entries)
					m_descriptors.push_back(describe(entry));
			}

			size_t count() const override { return m_descriptors.size(); }
			const Descriptor& descriptor(size_t index) const override { return m_descriptors.at(index); }
			std::unique_ptr<Plugin> create(std::string_view id, Host& host) override
			{
				return createTestPlugin(id, host);
			}

		private:
			std::vector<Descriptor> m_descriptors;
		};

		class TestFormat final : public pluginhost::Format
		{
		public:
			std::string_view name() const override { return "test"; }
			std::unique_ptr<Factory> load(const std::string& /*path*/) override
			{
				return std::make_unique<TestFactory>();
			}
		};
	}

	std::unique_ptr<pluginhost::Format> makeTestFormat()
	{
		return std::make_unique<TestFormat>();
	}

	std::unique_ptr<plugin::Plugin> createTestPlugin(std::string_view id, plugin::Host& host)
	{
		for (const Entry& entry : g_entries)
		{
			if (id == std::string("test:") + entry.name)
				return std::make_unique<TestPlugin>(entry, host);
		}
		return nullptr;
	}
}
