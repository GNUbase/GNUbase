// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pluginhost/formats.h"

#include <memory>

// The VST3 adapter (#68): loads .vst3 modules and adapts their audio module classes to the GNUbase plugin API, with
// the format mapping of docs/plugin-api.md. The only code that talks to VST3 is in src/pluginhost/vst3.
//
// A plugin's id is "vst3:" and its class id as 32 hex digits, the form VST3 preset files and DAWproject use: on
// Windows in COM order, as other hosts write it there. Ids are compared without case. The
// path is the module: a .vst3 bundle folder, or on Windows also a single .vst3 file.
namespace gnubase::pluginhost
{
	// The format "vst3".
	std::unique_ptr<Format> makeVst3Format();
}
