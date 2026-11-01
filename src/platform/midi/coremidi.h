// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/midi/port.h"

#include <memory>

namespace gnubase::midi
{
	// macOS MIDI through CoreMIDI (backend "coremidi"): its sources are inputs, its destinations outputs, including the
	// virtual ones other applications create. Ports speak Universal MIDI Packets in the MIDI 1.0 protocol, and CoreMIDI
	// translates for MIDI 2.0 endpoints. A port's id is CoreMIDI's unique id of the endpoint. CoreMIDI's timestamps
	// become host time, and outputs leave scheduling to CoreMIDI. Ports coming and going are noticed through CoreMIDI's
	// setup notifications.
	std::unique_ptr<Backend> makeCoreMidiBackend();
}
