// SPDX-License-Identifier: GPL-3.0-or-later
// Hang on demand: stops responding forever as soon as "Hang" is set to On, on whichever thread sets it. A parameter
// event while processing hangs the audio thread and a flush while inactive the main thread. "Hang on load" is harmless
// to set, but loading a state saved with it On hangs while loading. Only for tests that run it in a separate process.
#include "testplugin.h"

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_hangId = 1;
		constexpr uint32_t g_hangOnLoadId = 2;
		const char* const g_onOff[] = {"Off", "On"};

		class Hang : public TestPlugin
		{
		public:
			Hang() : TestPlugin({onOffParam(g_hangId, "Hang"), onOffParam(g_hangOnLoadId, "Hang on load")}) {}

			bool loadState(const uint8_t* data, size_t size) override
			{
				if (!TestPlugin::loadState(data, size))
					return false;
				double onLoad = 0.0;
				if (paramValue(g_hangOnLoadId, onLoad) && onLoad >= 0.5)
					hangNow();
				return true;
			}

		protected:
			void paramChanged(uint32_t id, double value) override
			{
				if (id == g_hangId && value >= 0.5)
					hangNow();
			}

		private:
			static ParamInfo onOffParam(uint32_t id, const char* name)
			{
				ParamInfo info;
				info.id = id;
				info.name = name;
				info.stepped = true;
				info.valueNames = g_onOff;
				return info;
			}
		};
	}

	const Descriptor& pluginDescriptor()
	{
		static const Descriptor descriptor{
			"org.gnubase.test.hang", "GNUbase Test Hang", "Hangs when its Hang parameter is set", false};
		return descriptor;
	}

	std::unique_ptr<TestPlugin> createPlugin(Host& /*host*/)
	{
		return std::make_unique<Hang>();
	}
}
