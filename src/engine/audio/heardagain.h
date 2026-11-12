// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace gnubase::engine
{
	// How a MIDI track is heard again in a block (AutomationPlayer::heardAgain): after playback moved while it was
	// silent, or not.
	enum class HeardAgain : uint8_t
	{
		No,
		// Only its mute button muted it: its parts played into the mute, and its strip sends the latest values the mute
		// dropped (ChannelStrip), except of the kinds its channel sends again, which are chased whichever track has
		// the latest.
		Muted,
		// Soloed out or disabled: its parts weren't in the sequence, so they are chased.
		Quiet,
	};
}
