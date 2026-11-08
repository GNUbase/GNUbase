// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <tuple>
#include <vector>

// MIDI effects (#135, reference manual pp. 1104-1106): the MIDI insert and send slots of a MIDI or instrument track,
// each holding one of GNUbase's built-in MIDI effects (processors/midieffects.h). The track's sequencer runs them on
// what the track plays (docs/features/midi-effects.md).
namespace gnubase::engine::midi
{
	inline constexpr size_t g_effectSlots = 4; // per section, as on pp. 1104 and 1106

	struct EffectSlot
	{
		std::string effect; // a built-in MIDI effect's id (processors::midiEffects), or empty: no effect
		bool on = true;
		std::vector<double> parameters; // the effect's parameter values, by parameter id
	};

	// The MIDI inserts or the MIDI sends of a track.
	struct EffectRack
	{
		bool bypass = false; // bypasses every slot of the section
		bool pre = false;    // sends only: they get the events before the inserts
		std::array<EffectSlot, g_effectSlots> slots;
	};

	inline bool operator==(const EffectSlot& a, const EffectSlot& b)
	{
		return std::tie(a.effect, a.on, a.parameters) == std::tie(b.effect, b.on, b.parameters);
	}

	inline bool operator==(const EffectRack& a, const EffectRack& b)
	{
		return std::tie(a.bypass, a.pre, a.slots) == std::tie(b.bypass, b.pre, b.slots);
	}
}
