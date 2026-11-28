// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"
#include "ui/widgetutils.h"

#include <string>

// A MIDI track's program selector, which the inspector and the track list share (docs/features/midi-devices.md).
namespace gnubase::ui::midipatches
{
	// A channel's patch as the selector's value: "off", or the program and bank, "5:300" ("5:" without a bank).
	std::string value(const engine::Json& channel);

	// The selector's choices (reference manual pp. 1108, 1113): the patches by name of the channel's device in
	// `devices` (the user setting midi.devices), or without one programs 1 to 128 (keeping the bank), always with Off,
	// and the channel's own patch if no choice has it.
	widgets::Options options(const engine::Json& devices, const engine::Json& channel);

	// The channel.set parameters that choose the patch of a value, or null when the channel has it already.
	engine::Json choose(const engine::Json& channel, const std::string& value);
}
