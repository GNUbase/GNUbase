// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace gnubase::engine::midi
{
	// What happens to a sounding chord's notes when another chord plays over it (reference manual p. 1379).
	enum class PadOverlap : uint8_t
	{
		HoldFirst, // its notes go on until its own off; notes the chords share sound once and don't start again
		Legato,    // its notes end, but those the new chord shares go on without starting again
		StopFirst, // all its notes end, and the new chord's all start
	};

	// The notes the chord pads play (#1089): which trigger, such as a remote key or a pad, holds which pitches. Each
	// pitch sounds once, whoever holds it, on the MIDI channel of its voice in the chord that started it (the highest
	// voice on channel 1, the next on channel 2, reference manual p. 1368), and ends when nobody holds it any more.
	// Fixed arrays and no allocations, so the audio thread can use it for the remote keys.
	class PadVoices
	{
	public:
		static constexpr size_t g_triggers = 128;
		static constexpr size_t g_maxPitches = 8;

		bool holds(size_t trigger) const { return trigger < g_triggers && m_counts[trigger] > 0; }

		// The trigger plays pitches (from the lowest up) at velocity over what the others hold. emit(on, pitch,
		// velocity, channel) gets the note-ons and note-offs, the offs with velocity 0. A trigger that holds a chord
		// already ends it first.
		template <typename Emit>
		void on(size_t trigger, const uint8_t* pitches, size_t count, uint8_t velocity, PadOverlap overlap, Emit&& emit)
		{
			if (trigger >= g_triggers)
				return;
			off(trigger, emit);
			count = std::min(count, g_maxPitches);
			if (overlap == PadOverlap::StopFirst)
				for (size_t other = 0; other < g_triggers; ++other)
					off(other, emit);
			for (size_t i = 0; i < count; ++i)
			{
				const uint8_t pitch = pitches[i] & 0x7F;
				m_pitches[trigger][i] = pitch;
				if (m_holders[pitch]++ == 0)
				{
					m_channels[pitch] = static_cast<uint8_t>(std::min<size_t>(count - 1 - i, 15));
					emit(true, pitch, velocity, m_channels[pitch]);
				}
			}
			m_counts[trigger] = static_cast<uint8_t>(count);
			if (overlap == PadOverlap::Legato)
				for (size_t other = 0; other < g_triggers; ++other)
					if (other != trigger)
						off(other, emit);
		}

		// The trigger's chord ends: its pitches that nobody else holds.
		template <typename Emit> void off(size_t trigger, Emit&& emit)
		{
			if (!holds(trigger))
				return;
			for (size_t i = 0; i < m_counts[trigger]; ++i)
			{
				const uint8_t pitch = m_pitches[trigger][i];
				if (m_holders[pitch] > 0 && --m_holders[pitch] == 0)
					emit(false, pitch, uint8_t{0}, m_channels[pitch]);
			}
			m_counts[trigger] = 0;
		}

	private:
		std::array<uint8_t, g_triggers> m_counts{};
		std::array<std::array<uint8_t, g_maxPitches>, g_triggers> m_pitches{};
		std::array<uint8_t, 128> m_holders{};  // per pitch: how many triggers hold it
		std::array<uint8_t, 128> m_channels{}; // per pitch: the channel it sounds on
	};
}
