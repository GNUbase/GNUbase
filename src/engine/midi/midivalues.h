// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// GNUbase keeps MIDI values at the resolution of MIDI 2.0: 16-bit velocities, 32-bit controllers, pressure and pitch
// bend. These convert from and to MIDI 1.0 with the MIDI 2.0 specification's min-center-max scaling: 0, the center
// and the maximum stay where they are, and scaling down undoes scaling up, so MIDI 1.0 data passes through unchanged.
namespace gnubase::engine::midi
{
	inline constexpr uint32_t g_pitchBendCenter = 0x80000000U; // no bend

	// value has fromBits bits; the result toBits, where fromBits < toBits <= 32.
	uint32_t scaleUp(uint32_t value, int fromBits, int toBits);
	// value has fromBits bits; the result toBits, where toBits < fromBits <= 32.
	uint32_t scaleDown(uint32_t value, int fromBits, int toBits);

	inline uint16_t velocityFromMidi1(uint8_t velocity)
	{
		return static_cast<uint16_t>(scaleUp(velocity, 7, 16));
	}

	inline uint8_t velocityToMidi1(uint16_t velocity)
	{
		return static_cast<uint8_t>(scaleDown(velocity, 16, 7));
	}

	// For a note-on: MIDI 1.0 takes velocity 0 as a note-off, so a soft note stays at 1.
	inline uint8_t noteOnVelocityToMidi1(uint16_t velocity)
	{
		const uint8_t result = velocityToMidi1(velocity);
		return result == 0 ? 1 : result;
	}

	// Controllers, channel and poly pressure.
	inline uint32_t controllerFromMidi1(uint8_t value)
	{
		return scaleUp(value, 7, 32);
	}

	inline uint8_t controllerToMidi1(uint32_t value)
	{
		return static_cast<uint8_t>(scaleDown(value, 32, 7));
	}

	// MIDI 1.0 pitch bend has 14 bits, 8192 being no bend.
	inline uint32_t pitchBendFromMidi1(uint16_t value)
	{
		return scaleUp(value, 14, 32);
	}

	inline uint16_t pitchBendToMidi1(uint32_t value)
	{
		return static_cast<uint16_t>(scaleDown(value, 32, 14));
	}
}
