// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gnubase::engine::graph
{
	// A MIDI event or parameter change at a sample offset within one processing block. MIDI values have the
	// resolution of MIDI 2.0 (see engine/midi/midivalues.h for MIDI 1.0).
	struct ProcessEvent
	{
		enum class Type : uint8_t
		{
			NoteOn,
			NoteOff,
			Controller,      // key: the controller number; value
			PitchBend,       // value: 0x80000000 is no bend
			ChannelPressure, // value
			PolyPressure,    // key: the pitch; value
			Program,         // key: the program; value: the bank, or 0xFFFFFFFF for none
			SysEx,           // data and size: the whole message, from 0xF0 to 0xF7
			Parameter,
		};

		uint32_t offset = 0; // frame within the block
		Type type = Type::NoteOn;
		uint8_t channel = 0;           // MIDI channel, 0-15
		uint8_t key = 0;               // notes: the pitch; see Type for the others
		uint16_t velocity = 0;         // notes, 0-65535
		uint32_t value = 0;            // 0-4294967295; see Type
		uint32_t parameter = 0;        // parameter changes: the processor's parameter id
		double parameterValue = 0.0;   // parameter changes: the new value
		const uint8_t* data = nullptr; // SysEx; valid during the block
		uint32_t size = 0;
	};

	// The events of one block, sorted by offset. At one offset note-offs come first, then the other events, then
	// note-ons, whatever order they were added in, so a note that starts where a note of the same pitch from another
	// source ends still sounds. Memory is allocated in the constructor only, so adding, reading and clearing are
	// real-time safe.
	class EventBuffer
	{
	public:
		explicit EventBuffer(size_t capacity = 0) : m_events(capacity) {}

		// Inserts the event by its offset and rank, after the events of the same offset and rank. Returns false and
		// drops the event if the buffer is full.
		bool add(const ProcessEvent& event) noexcept
		{
			if (m_size == m_events.size())
				return false;
			const int rank = rankOf(event.type);
			size_t i = m_size;
			for (; i > 0 && comesAfter(m_events[i - 1], event.offset, rank); --i)
				m_events[i] = m_events[i - 1];
			m_events[i] = event;
			++m_size;
			return true;
		}

		void clear() noexcept { m_size = 0; }

		size_t size() const { return m_size; }
		bool empty() const { return m_size == 0; }
		size_t capacity() const { return m_events.size(); }

		const ProcessEvent& operator[](size_t index) const { return m_events[index]; }
		const ProcessEvent* begin() const { return m_events.data(); }
		const ProcessEvent* end() const { return m_events.data() + m_size; }

	private:
		static int rankOf(ProcessEvent::Type type) noexcept
		{
			if (type == ProcessEvent::Type::NoteOff)
				return 0;
			return type == ProcessEvent::Type::NoteOn ? 2 : 1;
		}

		static bool comesAfter(const ProcessEvent& existing, uint32_t offset, int rank) noexcept
		{
			return existing.offset > offset || (existing.offset == offset && rankOf(existing.type) > rank);
		}

		std::vector<ProcessEvent> m_events; // sized to the capacity; the first m_size are in use
		size_t m_size = 0;
	};
}
