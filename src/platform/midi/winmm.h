// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/midi/port.h"

#include <memory>

namespace gnubase::midi
{
	// Windows MIDI through WinMM (backend "winmm"). On Windows with Windows MIDI Services, WinMM goes through the new
	// MIDI service and also sees its loopbacks and MIDI 2.0 devices, as MIDI 1.0 ports. A port's id is its name,
	// numbered from the second port of the same name ("USB MIDI #2"). Inputs are stamped with the host time they arrive
	// at. Plugging devices in and out is noticed through device interface notifications.
	std::unique_ptr<Backend> makeWinmmBackend();
}
