// SPDX-License-Identifier: GPL-3.0-or-later
// Hang on demand: stops responding forever as soon as "Hang" is set to On, on whichever thread sets it. A parameter
// event while processing hangs the audio thread, a flush while inactive the main thread, and loading a state saved with
// "Hang" On hangs while loading it. Only for tests that run it in a separate process.
#include "testplugin.h"

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_hangId = 1;
		const char* const g_onOff[] = {"Off", "On"};

		class Hang : public TestPlugin
		{
		public:
			Hang() : TestPlugin({hangParam()}) {}

		protected:
			void paramChanged(uint32_t id, double value) override
			{
				if (id == g_hangId && value >= 0.5)
					hangNow();
			}

		private:
			static ParamInfo hangParam()
			{
				ParamInfo info;
				info.id = g_hangId;
				info.name = "Hang";
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
