// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plugin/plugin.h"
#include "pluginhost/formats.h"

#include <cstdint>
#include <memory>
#include <string_view>

// Test plugins built into the plugin host process, so hosting is tested without plugin files (the test plugins in
// plugin formats are #63). Tests also create them in-process, to compare their output with the hosted one.
//   test:gain  stereo gain: parameter 0 "Gain" in dB (-96 to 24), parameter 1 "Invert" (0 or 1), applied at the
//              sample of their events; its state is the parameter values
//   test:hang  stereo pass-through that hangs in its first process() call and never returns
namespace gnubase::pluginhost
{
	enum TestGainParam : uint8_t
	{
		Gain = 0,
		Invert = 1,
	};

	// The format "test", which has no files.
	std::unique_ptr<Format> makeTestFormat();

	// A test plugin by id, not initialized, or nullptr.
	std::unique_ptr<plugin::Plugin> createTestPlugin(std::string_view id, plugin::Host& host);
}
