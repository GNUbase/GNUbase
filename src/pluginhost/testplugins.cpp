// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/testplugins.h"

#include "plugin/extensions.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace gnubase::pluginhost
{
	namespace
	{
		using namespace plugin;

		// The range of test:gain's Gain parameter, in dB.
		constexpr double g_minimumGain = -96.0;
		constexpr double g_maximumGain = 24.0;

		Descriptor testDescriptor(const char* id, const char* name)
		{
			Descriptor descriptor;
			descriptor.id = std::string("test:") + id;
			descriptor.format = "test";
			descriptor.formatId = id;
			descriptor.name = name;
			descriptor.vendor = "GNUbase";
			descriptor.version = "1";
			descriptor.features = {"audio-effect"};
			return descriptor;
		}

		bool stereoPort(bool input, uint32_t index, AudioPortInfo& info)
		{
			if (index != 0)
				return false;
			info.id = 0;
			info.name = input ? "Input" : "Output";
			info.channelCount = 2;
			info.layout = ChannelLayout::Stereo;
			info.flags = AudioPortInfo::Main;
			return true;
		}

		bool isStereo(const Process& process)
		{
			return process.audioInputCount == 1 && process.audioOutputCount == 1 &&
				process.audioInputs[0].channelCount == 2 && process.audioOutputs[0].channelCount == 2;
		}

		// Gain and polarity, applied at the sample of each parameter event.
		class TestGain final : public Plugin,
							   public AudioPorts,
							   public Params,
							   public State,
							   public Latency,
							   public Tail
		{
		public:
			TestGain() = default;

			const Descriptor& descriptor() const override { return m_descriptor; }
			bool init() override { return true; }
			bool activate(double /*sampleRate*/, uint32_t /*minFrames*/, uint32_t /*maxFrames*/) override
			{
				return true;
			}
			void deactivate() override {}
			bool startProcessing() noexcept override { return true; }
			void stopProcessing() noexcept override {}
			void reset() noexcept override {}

			ProcessStatus process(const Process& process) noexcept override
			{
				if (!isStereo(process))
					return ProcessStatus::Error;
				const AudioBuffer& in = process.audioInputs[0];
				AudioBuffer& out = process.audioOutputs[0];
				uint32_t position = 0;
				for (const Event& event : process.eventsIn)
				{
					const uint32_t offset = std::min(event.offset, process.frames);
					render(in, out, position, offset);
					position = offset;
					apply(event);
				}
				render(in, out, position, process.frames);
				out.constantMask = in.constantMask;
				return ProcessStatus::Continue;
			}

			void* extension(std::string_view id) override
			{
				return provide<AudioPorts, Params, State, Latency, Tail>(this, id);
			}

			// AudioPorts
			uint32_t audioPortCount(bool /*input*/) const override { return 1; }
			bool audioPortInfo(bool input, uint32_t index, AudioPortInfo& info) const override
			{
				return stereoPort(input, index, info);
			}

			// Params
			uint32_t paramCount() const override { return 2; }

			bool paramInfo(uint32_t index, ParamInfo& info) const override
			{
				if (index >= paramCount())
					return false;
				info = ParamInfo{};
				info.id = index;
				info.flags = ParamInfo::Automatable;
				if (index == Gain)
				{
					info.name = "Gain";
					info.minimum = g_minimumGain;
					info.maximum = g_maximumGain;
				}
				else
				{
					info.name = "Invert";
					info.flags |= ParamInfo::Stepped;
					info.maximum = 1.0;
				}
				return true;
			}

			bool paramValue(uint32_t param, double& value) const override
			{
				if (param >= paramCount())
					return false;
				value = m_values[param];
				return true;
			}

			bool paramValueToText(uint32_t param, double value, std::string& text) const override
			{
				if (param >= paramCount())
					return false;
				std::array<char, 32> buffer{};
				if (param == Gain)
					std::snprintf(buffer.data(), buffer.size(), "%.1f dB", value);
				else
					std::snprintf(buffer.data(), buffer.size(), "%s", value >= 0.5 ? "On" : "Off");
				text = buffer.data();
				return true;
			}

			bool paramTextToValue(uint32_t param, std::string_view text, double& value) const override
			{
				if (param >= paramCount())
					return false;
				if (param == Invert)
				{
					value = text == "On" ? 1.0 : 0.0;
					return text == "On" || text == "Off";
				}
				const std::string copy(text);
				char* end = nullptr;
				value = std::strtod(copy.c_str(), &end);
				return end != copy.c_str();
			}

			void flushParams(const EventList& in, EventList& /*out*/) noexcept override
			{
				for (const Event& event : in)
					apply(event);
			}

			// State: "GNBG", then the two values.
			bool saveState(std::vector<uint8_t>& data, StateContext /*context*/) override
			{
				data.assign(4 + sizeof(m_values), 0);
				std::memcpy(data.data(), "GNBG", 4);
				std::memcpy(data.data() + 4, m_values.data(), sizeof(m_values));
				return true;
			}

			bool loadState(const uint8_t* data, size_t size, StateContext /*context*/) override
			{
				if (size != 4 + sizeof(m_values) || std::memcmp(data, "GNBG", 4) != 0)
					return false;
				std::array<double, 2> values{};
				std::memcpy(values.data(), data + 4, sizeof(values));
				for (uint32_t param = 0; param < paramCount(); ++param)
					set(param, values[param]);
				return true;
			}

			// Latency, Tail
			uint32_t latency() const override { return 0; }
			uint32_t tail() const noexcept override { return 0; }

		private:
			void apply(const Event& event) noexcept
			{
				if (event.type == EventType::ParamValue && event.noteId == -1 && event.key == -1)
					set(event.param, event.value);
			}

			void set(uint32_t param, double value) noexcept
			{
				if (param >= paramCount())
					return;
				// The ranges paramInfo reports, without its allocations: this runs on the audio thread.
				m_values[param] =
					param == Gain ? std::clamp(value, g_minimumGain, g_maximumGain) : std::clamp(value, 0.0, 1.0);
				const auto gain = static_cast<float>(std::pow(10.0, m_values[Gain] / 20.0));
				m_factor = m_values[Invert] >= 0.5 ? -gain : gain;
			}

			void render(const AudioBuffer& in, const AudioBuffer& out, uint32_t begin, uint32_t end) const noexcept
			{
				for (uint32_t c = 0; c < 2; ++c)
				{
					for (uint32_t i = begin; i < end; ++i)
						out.channels[c][i] = in.channels[c][i] * m_factor;
				}
			}

			Descriptor m_descriptor = testDescriptor("gain", "Test Gain");
			std::array<double, 2> m_values{};
			float m_factor = 1.0f;
		};

		// Hangs in process(), as a plugin with a deadlock would.
		class TestHang final : public Plugin, public AudioPorts
		{
		public:
			const Descriptor& descriptor() const override { return m_descriptor; }
			bool init() override { return true; }
			bool activate(double /*sampleRate*/, uint32_t /*minFrames*/, uint32_t /*maxFrames*/) override
			{
				return true;
			}
			void deactivate() override {}
			bool startProcessing() noexcept override { return true; }
			void stopProcessing() noexcept override {}
			void reset() noexcept override {}

			ProcessStatus process(const Process& /*process*/) noexcept override
			{
				for (;;)
					std::this_thread::sleep_for(std::chrono::seconds(1));
			}

			void* extension(std::string_view id) override { return provide<AudioPorts>(this, id); }

			uint32_t audioPortCount(bool /*input*/) const override { return 1; }
			bool audioPortInfo(bool input, uint32_t index, AudioPortInfo& info) const override
			{
				return stereoPort(input, index, info);
			}

		private:
			Descriptor m_descriptor = testDescriptor("hang", "Test Hang");
		};

		class TestFactory final : public Factory
		{
		public:
			size_t count() const override { return m_descriptors.size(); }
			const Descriptor& descriptor(size_t index) const override { return m_descriptors.at(index); }
			std::unique_ptr<Plugin> create(std::string_view id, Host& host) override
			{
				return createTestPlugin(id, host);
			}

		private:
			std::vector<Descriptor> m_descriptors{
				testDescriptor("gain", "Test Gain"), testDescriptor("hang", "Test Hang")};
		};

		class TestFormat final : public Format
		{
		public:
			std::string_view name() const override { return "test"; }
			std::unique_ptr<Factory> load(const std::string& /*path*/) override
			{
				return std::make_unique<TestFactory>();
			}
		};
	}

	std::unique_ptr<Format> makeTestFormat()
	{
		return std::make_unique<TestFormat>();
	}

	std::unique_ptr<plugin::Plugin> createTestPlugin(std::string_view id, plugin::Host& /*host*/)
	{
		if (id == "test:gain")
			return std::make_unique<TestGain>();
		if (id == "test:hang")
			return std::make_unique<TestHang>();
		return nullptr;
	}
}
