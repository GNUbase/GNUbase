// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

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
				if (m_holders[pitch] == 0)
					continue;
				--m_holders[pitch];
				if (m_holders[pitch] == 0)
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

	// Snap Playback to Musical Grid (reference manual p. 1349, #1089): while the transport plays, the chord pads'
	// note-ons wait for the next grid position, and a note-off waits with its note-on. Fixed arrays and no
	// allocations, for the audio thread.
	class PadSnap
	{
	public:
		static constexpr size_t g_capacity = 256;

		// One block of the chord pads' notes, starting at position (samples on the transport) and frames long:
		// nextGrid(sample) is the first grid position at or after a sample. With snap off or the transport stopped,
		// what waits plays at the block's start and the rest as it comes.
		template <typename NextGrid> void process(audio::MidiBuffer& pads, bool snap, bool playing, int64_t position,
			uint32_t frames, NextGrid&& nextGrid) noexcept
		{
			if (pads.empty() && m_pending == 0)
				return;
			const int64_t end = position + frames;
			size_t count = 0;
			const auto out = [this, &count](audio::MidiMessage message, int64_t offset)
			{
				if (count == m_out.size())
					return;
				message.offset = static_cast<uint32_t>(std::max<int64_t>(offset, 0));
				m_out.at(count++) = message;
			};
			// What waited and is due.
			size_t kept = 0;
			for (size_t i = 0; i < m_pending; ++i)
			{
				if (!snap || !playing || m_waiting.at(i).at < end)
					out(m_waiting.at(i).message, m_waiting.at(i).at - position);
				else
					m_waiting.at(kept++) = m_waiting.at(i);
			}
			m_pending = kept;
			for (const audio::MidiMessage& message : pads)
			{
				const int status = message.bytes[0] & 0xF0;
				const bool on = status == 0x90 && message.bytes[2] > 0;
				const bool off = status == 0x80 || (status == 0x90 && message.bytes[2] == 0);
				int64_t at = position + message.offset;
				if (snap && playing && on)
				{
					at = std::max(nextGrid(at), at);
				}
				else if (snap && playing && off)
				{
					// After its note-on, if that one waits.
					for (size_t i = 0; i < m_pending; ++i)
					{
						const audio::MidiMessage& waiting = m_waiting.at(i).message;
						if ((waiting.bytes[0] & 0xF0) == 0x90 &&
							(waiting.bytes[0] & 0x0F) == (message.bytes[0] & 0x0F) &&
							waiting.bytes[1] == message.bytes[1])
							at = std::max(at, m_waiting.at(i).at);
					}
				}
				if (at < end || m_pending == m_waiting.size())
					out(message, at - position);
				else
					m_waiting.at(m_pending++) = {message, at};
			}
			std::stable_sort(m_out.begin(), m_out.begin() + static_cast<std::ptrdiff_t>(count),
				[](const audio::MidiMessage& a, const audio::MidiMessage& b) { return a.offset < b.offset; });
			pads.clear();
			for (size_t i = 0; i < count; ++i)
				pads.add(m_out.at(i));
		}

	private:
		struct Waiting
		{
			audio::MidiMessage message;
			int64_t at = 0; // on the transport, in samples
		};
		std::array<Waiting, g_capacity> m_waiting{};
		size_t m_pending = 0;
		std::array<audio::MidiMessage, g_capacity> m_out{};
	};
}
