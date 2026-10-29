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
		uint32_t ramp = 0;             // parameter changes: frames over which the value moves in a straight line from
									   // the current one to the new one, the frame at offset still having the current
									   // one; 0 jumps. Processors that don't glide take the new value at once.
		const uint8_t* data = nullptr; // SysEx; valid during the block
		uint32_t size = 0;
	};

	// The events of one block from one or more sources, sorted by offset. Each source's events keep their order, so a
	// note of no length or a note-on followed by pressure on it arrives as sent. At one offset a source's events go
	// before the earlier sources' events that rank after them: note-offs first, then the other events, then note-ons,
	// so a note that starts where a note of the same pitch from another source ends still sounds. Memory is allocated
	// in the constructor only, so adding, merging, reading and clearing are real-time safe.
	class EventBuffer
	{
	public:
		explicit EventBuffer(size_t capacity = 0) : m_events(capacity) {}

		// Inserts an event of the current source by its offset, after the source's previous event unless that one
		// comes later. Returns false and drops the event if the buffer is full.
		bool add(const ProcessEvent& event) noexcept
		{
			if (m_size == m_events.size())
				return false;
			insert(event);
			return true;
		}

		// The events added from now on come from another source.
		void nextSource() noexcept { m_after = 0; }

		// Adds the events of another source. In a full buffer a note-off takes the place of the last event that isn't
		// one, so that no note hangs; other events are dropped.
		void merge(const EventBuffer& source) noexcept
		{
			nextSource();
			for (const ProcessEvent& event : source)
				if (m_size < m_events.size() || (event.type == ProcessEvent::Type::NoteOff && dropLastNotNoteOff()))
					insert(event);
		}

		void clear() noexcept
		{
			m_size = 0;
			m_after = 0;
		}

		size_t size() const { return m_size; }
		bool empty() const { return m_size == 0; }
		size_t capacity() const { return m_events.size(); }

		const ProcessEvent& operator[](size_t index) const { return m_events[index]; }
		const ProcessEvent* begin() const { return m_events.data(); }
		const ProcessEvent* end() const { return m_events.data() + m_size; }

	private:
		// Before the first event at the offset that ranks after it, but not before the source's previous event.
		void insert(const ProcessEvent& event) noexcept
		{
			size_t i = m_after > 0 && m_events[m_after - 1].offset <= event.offset ? m_after : 0;
			const int rank = rankOf(event.type);
			while (i < m_size && !comesAfter(m_events[i], event.offset, rank))
				++i;
			for (size_t j = m_size; j > i; --j)
				m_events[j] = m_events[j - 1];
			m_events[i] = event;
			++m_size;
			m_after = i + 1;
		}

		bool dropLastNotNoteOff() noexcept
		{
			for (size_t i = m_size; i > 0; --i)
			{
				if (m_events[i - 1].type == ProcessEvent::Type::NoteOff)
					continue;
				for (size_t j = i; j < m_size; ++j)
					m_events[j - 1] = m_events[j];
				--m_size;
				if (i <= m_after)
					--m_after;
				return true;
			}
			return false;
		}

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
		size_t m_after = 0; // the current source's next event goes after this many, or 0
	};
}
