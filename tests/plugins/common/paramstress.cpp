// SPDX-License-Identifier: GPL-3.0-or-later
// Parameter stress: 1024 parameters of every kind with ids that are not contiguous, and automation output. While
// "Emit automation" is on, every block changes eight parameters itself, each inside a gesture, like a user turning
// knobs in the plugin's editor. The audio passes through unchanged.
#include "testplugin.h"

#include <cmath>
#include <string>

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_emitId = 1;
		constexpr uint32_t g_stressCount = 1023; // plus "Emit automation"
		constexpr uint32_t g_perBlock = 8;
		const char* const g_onOff[] = {"Off", "On"};
		const char* const g_shapes[] = {"Sine", "Triangle", "Saw", "Square"};

		uint32_t stressId(uint32_t index)
		{
			return 100 + (index * 7);
		}

		std::vector<ParamInfo> stressParams()
		{
			static const std::vector<std::string> names = []
			{
				std::vector<std::string> list;
				list.reserve(g_stressCount);
				for (uint32_t i = 0; i < g_stressCount; ++i)
					list.push_back("Parameter " + std::to_string(i + 1));
				return list;
			}();
			std::vector<ParamInfo> params;
			ParamInfo emit;
			emit.id = g_emitId;
			emit.name = "Emit automation";
			emit.maxValue = 1.0;
			emit.stepped = true;
			emit.valueNames = g_onOff;
			params.push_back(emit);
			for (uint32_t i = 0; i < g_stressCount; ++i)
			{
				ParamInfo info;
				info.id = stressId(i);
				info.name = names[i].c_str();
				switch (i % 4)
				{
				case 0: // continuous
					info.defaultValue = 0.5;
					break;
				case 1: // integer
					info.minValue = -100.0;
					info.maxValue = 100.0;
					info.stepped = true;
					break;
				case 2: // list
					info.maxValue = 3.0;
					info.stepped = true;
					info.valueNames = g_shapes;
					break;
				default: // switch
					info.stepped = true;
					info.valueNames = g_onOff;
					break;
				}
				params.push_back(info);
			}
			return params;
		}

		class ParamStress : public TestPlugin
		{
		public:
			ParamStress() : TestPlugin(stressParams()) {}

			void process(const ProcessBlock& block) override
			{
				TestPlugin::process(block);
				double emit = 0.0;
				paramValue(g_emitId, emit);
				if (emit < 0.5 || block.outEvents == nullptr)
					return;
				for (uint32_t k = 0; k < g_perBlock; ++k)
				{
					const ParamInfo& info = params()[1 + ((m_cursor + k) % g_stressCount)];
					const double phase = std::fmod(static_cast<double>(m_counter + k) * 0.618033988749895, 1.0);
					double value = info.minValue + (phase * (info.maxValue - info.minValue));
					if (info.stepped)
						value = std::round(value);
					setParam(info.id, value);
					Event event;
					event.time = k * block.frames / g_perBlock;
					event.paramId = info.id;
					event.value = value;
					event.type = EventType::GestureBegin;
					block.outEvents->push(event);
					event.type = EventType::ParamValue;
					block.outEvents->push(event);
					event.type = EventType::GestureEnd;
					block.outEvents->push(event);
				}
				m_cursor = (m_cursor + g_perBlock) % g_stressCount;
				m_counter += g_perBlock;
			}

		private:
			uint32_t m_cursor = 0;
			uint64_t m_counter = 0;
		};
	}

	const Descriptor& pluginDescriptor()
	{
		static const Descriptor descriptor{"org.gnubase.test.paramstress", "GNUbase Test Parameter Stress",
			"1024 parameters with automation output", false};
		return descriptor;
	}

	std::unique_ptr<TestPlugin> createPlugin(Host& /*host*/)
	{
		return std::make_unique<ParamStress>();
	}
}
