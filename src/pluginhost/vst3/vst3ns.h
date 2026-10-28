// SPDX-License-Identifier: GPL-3.0-or-later
// The VST3 SDK's C++ namespace as `vst`, e.g. vst::FUnknown and vst::Vst::IComponent. GNUbase code never spells the
// SDK's namespace out: CMake reads its name from the SDK's headers and passes it as GNUBASE_VST3_NS (the vst3-sdk
// target in the top-level CMakeLists.txt).
#pragma once

#include <pluginterfaces/base/funknown.h>

#ifndef GNUBASE_VST3_NS
#error "GNUBASE_VST3_NS is not defined: link the vst3-sdk target"
#endif

namespace vst = GNUBASE_VST3_NS;
