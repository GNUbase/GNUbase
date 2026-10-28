// SPDX-License-Identifier: GPL-3.0-or-later
// Latency reporter: delays its input by the reported latency, so hosts can test delay compensation. The latency is a
// parameter; changing it while active asks the host for a restart, and the new latency applies on the next activate.
#include "testplugin.h"

#include <algorithm>

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_latencyId = 1;

		class Latency : public TestPlugin
		{
		public:
			explicit Latency(Host& host) : TestPlugin({latencyParam()}), m_host(host) {}

			void activate(double sampleRate, uint32_t maxFrames) override
			{
				TestPlugin::activate(sampleRate, maxFrames);
				double samples = 0.0;
				paramValue(g_latencyId, samples);
				m_latency = static_cast<uint32_t>(samples);
				for (std::vector<float>& line : m_lines)
					line.assign(m_latency, 0.0f);
				m_position = 0;
				m_active.store(true);
			}

			void deactivate() override { m_active.store(false); }

			void resetState() override
			{
				for (std::vector<float>& line : m_lines)
					std::fill(line.begin(), line.end(), 0.0f);
				m_position = 0;
			}

			uint32_t latency() const override { return m_latency; }

		protected:
			void paramChanged(uint32_t id, double value) override
			{
				if (id == g_latencyId && m_active.load() && static_cast<uint32_t>(value) != m_latency)
					m_host.requestRestart();
			}

			void render(const ProcessBlock& block, uint32_t from, uint32_t to) override
			{
				if (m_latency == 0)
				{
					TestPlugin::render(block, from, to);
					return;
				}
				for (uint32_t i = from; i < to; ++i)
				{
					for (uint32_t c = 0; c < block.outputChannels; ++c)
					{
						std::vector<float>& line = m_lines[c];
						const float in = c < block.inputChannels ? block.inputs[c][i] : 0.0f;
						block.outputs[c][i] = line[m_position];
						line[m_position] = in;
					}
					m_position = m_position + 1 == m_latency ? 0 : m_position + 1;
				}
			}

		private:
			static ParamInfo latencyParam()
			{
				ParamInfo info;
				info.id = g_latencyId;
				info.name = "Latency";
				info.minValue = 0.0;
				info.maxValue = 48000.0;
				info.defaultValue = 64.0;
				info.stepped = true;
				info.unit = "samples";
				return info;
			}

			Host& m_host;
			std::atomic<bool> m_active{false};
			uint32_t m_latency = 0;
			uint32_t m_position = 0;
			std::vector<float> m_lines[g_channels];
		};
	}

	const Descriptor& pluginDescriptor()
	{
		static const Descriptor descriptor{
			"org.gnubase.test.latency", "GNUbase Test Latency", "Delays its input by the latency it reports", false};
		return descriptor;
	}

	std::unique_ptr<TestPlugin> createPlugin(Host& host)
	{
		return std::make_unique<Latency>(host);
	}
}
