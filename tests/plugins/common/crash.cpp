// SPDX-License-Identifier: GPL-3.0-or-later
// Crash on demand: crashes with an access violation as soon as "Crash" is set to On, on whichever thread sets it. A
// parameter event while processing crashes the audio thread, a flush while inactive the main thread, and loading a
// state saved with "Crash" On crashes while loading it. Only for tests that run it in a separate process.
#include "testplugin.h"

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_crashId = 1;
		const char* const g_onOff[] = {"Off", "On"};

		class Crash : public TestPlugin
		{
		public:
			Crash() : TestPlugin({crashParam()}) {}

		protected:
			void paramChanged(uint32_t id, double value) override
			{
				if (id == g_crashId && value >= 0.5)
					crashNow();
			}

		private:
			static ParamInfo crashParam()
			{
				ParamInfo info;
				info.id = g_crashId;
				info.name = "Crash";
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
