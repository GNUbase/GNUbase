// SPDX-License-Identifier: GPL-3.0-or-later
// Gain: a stereo effect with one parameter, applied from the event's sample offset on, without smoothing, so tests
// can check exact sample values.
#include "testplugin.h"

#include <cmath>

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_gainId = 1;

		class Gain : public TestPlugin
		{
		public:
			Gain() : TestPlugin({gainParam()}) {}

		protected:
			void render(const ProcessBlock& block, uint32_t from, uint32_t to) override
			{
				double db = 0.0;
				paramValue(g_gainId, db);
				const auto factor = static_cast<float>(std::pow(10.0, db / 20.0));
				for (uint32_t c = 0; c < block.outputChannels; ++c)
					for (uint32_t i = from; i < to; ++i)
						block.outputs[c][i] = c < block.inputChannels ? block.inputs[c][i] * factor : 0.0f;
			}

		private:
			static ParamInfo gainParam()
			{
				ParamInfo info;
				info.id = g_gainId;
				info.name = "Gain";
				info.minValue = -60.0;
				info.maxValue = 12.0;
				info.defaultValue = 0.0;
				info.unit = "dB";
				return info;
			}
		};
	}

	const Descriptor& pluginDescriptor()
	{
		static const Descriptor descriptor{
			"org.gnubase.test.gain", "GNUbase Test Gain", "Multiplies the input by a gain in dB", false};
		return descriptor;
	}

	std::unique_ptr<TestPlugin> createPlugin(Host& /*host*/)
	{
		return std::make_unique<Gain>();
	}
}
