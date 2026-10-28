// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The VST3 SDK's C++ namespace, as `vst`. CMake reads its name from the SDK and passes it as GNUBASE_VST3_NS (see the
// top-level CMakeLists.txt), so GNUbase code writes vst::IPluginFactory, vst::Vst::IComponent and so on. Include it
// before any SDK header that GNUbase code uses names of.
#ifndef GNUBASE_VST3_NS
#error "GNUBASE_VST3_NS is not defined: link the vst3-interfaces target"
#endif

#include "pluginterfaces/base/funknown.h"

namespace vst = GNUBASE_VST3_NS;
