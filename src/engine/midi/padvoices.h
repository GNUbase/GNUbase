// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gnubase::engine::midi
{
	// Sorts first to last stably, in place, without allocating as std::stable_sort may: for the few notes of a block.
	template <typename T, typename Less> void sortInPlace(T* first, T* last, Less less) noexcept
	{
		for (T* i = first; i != last; ++i)
		{
			T value = *i;
			T* at = i;
			for (; at != first && less(value, *(at - 1)); --at)
				*at = *(at - 1);
			*at = value;
		}
	}

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

	// The chord pads' pattern as the audio thread plays it (reference manual pp. 1379-1380): at most 64 notes on the
	// chord's voices, from the lowest (0) up, repeating every length ticks.
	struct PadPatternSteps
	{
		static constexpr size_t g_maxNotes = 64;
		struct Step
		{
			int64_t position = 0; // ticks from the pattern's start
			int64_t length = 0;
			uint8_t voice = 0;
			uint8_t velocity = 100;
		};
		bool on = false;
		bool velocityFromKeyboard = false;
		int64_t length = 0;
		size_t count = 0;
		std::array<Step, g_maxNotes> steps{};
	};

	// The pattern player (#1089): while the chord pads hold a chord, its notes don't play; the pattern does, with the
	// held pitches on its voices (the lowest pitch on voice 0; voices beyond the chord an octave above its lower
	// ones), from the moment the first chord comes, on and on, the next chord's pitches taking over as it comes. When
	// nothing is held any more it ends. Fixed arrays and no allocations, for the audio thread.
	class PadPattern
	{
	public:
		// One block of a player's chord notes, frames long, at ticksPerFrame (the tempo).
		void process(
			audio::MidiBuffer& pads, const PadPatternSteps& pattern, uint32_t frames, double ticksPerFrame) noexcept
		{
			if (!pattern.on || pattern.length <= 0 || pattern.count == 0 || !(ticksPerFrame > 0.0))
			{
				if (m_running)
					stop(pads, 0);
				return;
			}
			std::array<audio::MidiMessage, 64> changes{};
			size_t count = 0;
			for (const audio::MidiMessage& message : pads)
				if (count < changes.size())
					changes.at(count++) = message;
			pads.clear();
			// The block in sections between the chord's changes, the pattern going on through each.
			uint32_t at = 0;
			for (size_t i = 0; i <= count; ++i)
			{
				const uint32_t until = i < count ? std::min(changes.at(i).offset, frames) : frames;
				if (m_running && until > at)
					play(pads, pattern, at, until, ticksPerFrame);
				at = std::max(at, until);
				if (i < count)
					hold(changes.at(i), pads, at);
			}
		}

	private:
		struct Out
		{
			uint32_t frame = 0;
			bool on = false;
			uint8_t pitch = 0;
			uint8_t velocity = 0;
			uint8_t channel = 0;
		};

		void hold(const audio::MidiMessage& message, audio::MidiBuffer& pads, uint32_t at) noexcept
		{
			const int status = message.bytes[0] & 0xF0;
			const auto pitch = static_cast<size_t>(message.bytes[1] & 0x7F);
			if (status == 0x90 && message.bytes[2] > 0)
			{
				m_held.at(pitch) = true;
				m_velocity = message.bytes[2];
				if (!m_running)
				{
					m_running = true;
					m_tick = 0.0;
				}
			}
			else if (status == 0x80 || status == 0x90)
			{
				m_held.at(pitch) = false;
				if (m_running && std::none_of(m_held.begin(), m_held.end(), [](bool down) { return down; }))
					stop(pads, at);
			}
		}

		// The pattern's notes from the tick it reached up to frame until, from frame from.
		void play(audio::MidiBuffer& pads, const PadPatternSteps& pattern, uint32_t from, uint32_t until,
			double ticksPerFrame) noexcept
		{
			std::array<uint8_t, 8> voices{};
			size_t count = 0;
			for (size_t pitch = 0; pitch < m_held.size() && count < voices.size(); ++pitch)
				if (m_held.at(pitch))
					voices.at(count++) = static_cast<uint8_t>(pitch);
			const double end = m_tick + (static_cast<double>(until - from) * ticksPerFrame);
			const auto frameAt = [this, from, until, ticksPerFrame](double tick)
			{
				const double frame = from + (std::max(tick - m_tick, 0.0) / ticksPerFrame);
				return static_cast<uint32_t>(std::min(frame, static_cast<double>(until - 1)));
			};
			std::array<Out, 2 * g_maxSounding> outs{};
			size_t n = 0;
			const auto length = static_cast<double>(pattern.length);
			for (size_t s = 0; s < pattern.count && count > 0; ++s)
			{
				const PadPatternSteps::Step& step = pattern.steps.at(s);
				const auto position = static_cast<double>(step.position);
				// Each time the step comes in this block: the repeats of the pattern from the first at or after the
				// tick reached.
				const auto first = static_cast<int64_t>(std::ceil((m_tick - position) / length));
				for (int64_t repeat = first; m_sounding < g_maxSounding && n < outs.size(); ++repeat)
				{
					const double tick = position + (static_cast<double>(repeat) * length);
					if (!(tick < end))
						break;
					const size_t voice = step.voice;
					const int pitch = std::min(voices.at(voice % count) + (static_cast<int>(voice / count) * 12), 127);
					const auto channel =
						static_cast<uint8_t>(std::min<size_t>(count - 1 - std::min(voice, count - 1), 15));
					const uint8_t velocity = pattern.velocityFromKeyboard ? m_velocity : step.velocity;
					outs.at(n++) = {frameAt(tick), true, static_cast<uint8_t>(pitch), velocity, channel};
					m_pitches.at(m_sounding) = static_cast<uint8_t>(pitch);
					m_channels.at(m_sounding) = channel;
					m_ends.at(m_sounding) = tick + static_cast<double>(std::max<int64_t>(step.length, 1));
					++m_sounding;
				}
			}
			for (size_t i = 0; i < m_sounding && n < outs.size();)
			{
				if (m_ends.at(i) < end)
				{
					outs.at(n++) = {frameAt(m_ends.at(i)), false, m_pitches.at(i), 0, m_channels.at(i)};
					--m_sounding;
					m_ends.at(i) = m_ends.at(m_sounding);
					m_pitches.at(i) = m_pitches.at(m_sounding);
					m_channels.at(i) = m_channels.at(m_sounding);
				}
				else
				{
					++i;
				}
			}
			sortInPlace(outs.data(), outs.data() + n,
				[](const Out& a, const Out& b) { return a.frame < b.frame || (a.frame == b.frame && !a.on && b.on); });
			for (size_t i = 0; i < n; ++i)
				send(pads, outs.at(i).on, outs.at(i).pitch, outs.at(i).velocity, outs.at(i).channel, outs.at(i).frame);
			m_tick = end;
		}

		void stop(audio::MidiBuffer& pads, uint32_t at) noexcept
		{
			for (size_t i = 0; i < m_sounding; ++i)
				send(pads, false, m_pitches.at(i), 0, m_channels.at(i), at);
			m_sounding = 0;
			m_running = false;
			m_held.fill(false);
		}

		static void send(
			audio::MidiBuffer& pads, bool on, uint8_t pitch, uint8_t velocity, uint8_t channel, uint32_t frame) noexcept
		{
			audio::MidiMessage message;
			message.size = 3;
			message.bytes = {static_cast<uint8_t>((on ? 0x90 : 0x80) | channel), pitch, velocity};
			message.offset = frame;
			pads.add(message);
		}

		static constexpr size_t g_maxSounding = 64;
		std::array<bool, 128> m_held{};
		uint8_t m_velocity = 100;
		bool m_running = false;
		double m_tick = 0.0; // ticks since the pattern started
		size_t m_sounding = 0;
		std::array<uint8_t, g_maxSounding> m_pitches{};
		std::array<uint8_t, g_maxSounding> m_channels{};
		std::array<double, g_maxSounding> m_ends{}; // in ticks since the start
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
			sortInPlace(m_out.data(), m_out.data() + count,
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
