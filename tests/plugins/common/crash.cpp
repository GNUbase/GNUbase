// SPDX-License-Identifier: GPL-3.0-or-later
// Crash on demand: crashes with an access violation as soon as "Crash" is set to On, on whichever thread sets it. A
// parameter event while processing crashes the audio thread and a flush while inactive the main thread. "Crash on load"
// is harmless to set, but loading a state saved with it On crashes while loading. Only for tests that run it in a
// separate process.
#include "testplugin.h"

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_crashId = 1;
		constexpr uint32_t g_crashOnLoadId = 2;
		const char* const g_onOff[] = {"Off", "On"};

		class Crash : public TestPlugin
		{
		public:
			Crash() : TestPlugin({onOffParam(g_crashId, "Crash"), onOffParam(g_crashOnLoadId, "Crash on load")}) {}

			bool loadState(const uint8_t* data, size_t size) override
			{
				if (!TestPlugin::loadState(data, size))
					return false;
				double onLoad = 0.0;
				if (paramValue(g_crashOnLoadId, onLoad) && onLoad >= 0.5)
					crashNow();
				return true;
			}

		protected:
			void paramChanged(uint32_t id, double value) override
			{
				if (id == g_crashId && value >= 0.5)
					crashNow();
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
			"org.gnubase.test.crash", "GNUbase Test Crash", "Crashes when its Crash parameter is set", false};
		return descriptor;
	}

	std::unique_ptr<TestPlugin> createPlugin(Host& /*host*/)
	{
		return std::make_unique<Crash>();
	}
}
