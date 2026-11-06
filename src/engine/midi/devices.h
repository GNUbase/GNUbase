// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"

namespace gnubase::engine
{
	class Engine;
}

// MIDI devices (#136, reference manual pp. 1108-1114, docs/features/midi-devices.md): representations of external
// instruments with their patches by name, kept in the user setting midi.devices.
namespace gnubase::engine::midi
{
	// The schema of midi.devices: devices by name, each with banks of patches ({name, program, bank, folder}).
	Json devicesSchema();

	// Registers midiDevice.install and midiDevice.remove, which change midi.devices.
	void registerDeviceCommands(Engine& engine);
}
