// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

// MIDI effects (#135, reference manual pp. 1104-1106): the MIDI insert and send slots of a MIDI or instrument track,
// each holding one of GNUbase's built-in MIDI effects (processors/midieffects.h). The track's sequencer runs them on
// what the track plays (docs/features/midi-effects.md).
namespace gnubase::engine::midi
{
	inline constexpr size_t g_effectSlots = 4; // per section, as on pp. 1104 and 1106

	// The parameter events of the MIDI effects' automation for the track's sequencer: g_effectEvents, plus the slot
	// (0 to 3 the inserts, 4 to 7 the sends) times 256, plus the parameter's id.
	inline constexpr uint32_t g_effectEvents = 0x4D450000;

	struct EffectSlot
	{
		std::string effect; // a built-in MIDI effect's id (processors::midiEffects), or empty: no effect
		bool on = true;
		std::vector<double> parameters; // the effect's parameter values, by parameter id
		uint64_t output = 0; // sends only: the instrument track's channel its effect plays to; 0: the track's output
	};

	// The MIDI inserts or the MIDI sends of a track.
	struct EffectRack
	{
		bool bypass = false; // bypasses every slot of the section
		bool pre = false;    // sends only: they get the events before the MIDI modifiers and the inserts
		uint8_t divider = 0; // inserts only: the slots above the divider, which play the track's input as it records
		std::array<EffectSlot, g_effectSlots> slots;
	};

	inline bool operator==(const EffectSlot& a, const EffectSlot& b)
	{
		return std::tie(a.effect, a.on, a.parameters, a.output) == std::tie(b.effect, b.on, b.parameters, b.output);
	}

	inline bool operator==(const EffectRack& a, const EffectRack& b)
	{
		return std::tie(a.bypass, a.pre, a.divider, a.slots) == std::tie(b.bypass, b.pre, b.divider, b.slots);
	}
}
