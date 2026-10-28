// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

// Events of the GNUbase plugin API (docs/plugin-api.md, "Events and notes"). An event is a fixed-size record without
// pointers, so the plugin IPC (#62) copies it into shared memory as it is, between 32- and 64-bit processes alike.
namespace gnubase::plugin
{
	enum class EventType : uint8_t
	{
		NoteOn,            // value: velocity 0-1
		NoteOff,           // value: release velocity 0-1
		NoteChoke,         // stop the matching voices at once, without a release
		NoteEnd,           // output only: a voice has ended, so the host can free what it holds for the note
		NoteExpression,    // expression: a NoteExpression; value: its value
		ParamValue,        // param: the parameter; value: the new plain value. A note match makes it per voice.
		ParamMod,          // param: the parameter; value: the modulation amount, added to the value without changing it
		ParamGestureBegin, // output only: the user grabbed the parameter in the plugin's editor
		ParamGestureEnd,   // output only: the user let it go
		Midi,              // data[0]: up to three MIDI 1.0 bytes, the status byte in the lowest byte
		Midi2,             // data[0..3]: one MIDI 2.0 universal MIDI packet of up to four words
		Sysex,             // data[0]: the offset of the message in the list's bytes, data[1]: its size
	};

	// Per-note expressions, the common set of the formats that have them (#176).
	enum class NoteExpression : uint8_t
	{
		Volume,     // linear gain, 0-4
		Pan,        // 0 left, 0.5 center, 1 right
		Tuning,     // semitones, -120 to 120
		Vibrato,    // 0-1
		Expression, // 0-1
		Brightness, // 0-1
		Pressure,   // 0-1
	};

	// A note, expression, parameter change or MIDI message at a frame offset within one block. Notes are matched by
	// note id, port, channel and key, where -1 is a wildcard, as in CLAP.
	struct alignas(8) Event
	{
		enum Flags : uint8_t
		{
			Live = 1,       // played live, not from the timeline
			DontRecord = 2, // an output the host must not record, e.g. a parameter the plugin changes itself
		};

		uint32_t offset = 0; // frame within the block
		EventType type = EventType::NoteOn;
		uint8_t reserved0 = 0;
		uint16_t flags = 0;
		int32_t noteId = -1;
		int16_t port = -1; // note port index
		int16_t channel = -1;
		int16_t key = -1;
		NoteExpression expression = NoteExpression::Volume;
		uint8_t reserved1 = 0;
		uint32_t param = 0;
		double value = 0.0;
		uint32_t data[4] = {};
	};

	static_assert(std::is_trivially_copyable_v<Event> && std::is_standard_layout_v<Event>);
	static_assert(sizeof(Event) == 48 && offsetof(Event, value) == 24, "Event has the same layout in every process");

	// The events of one block in order of their offsets, plus the bytes of their system exclusive messages. Memory is
	// allocated in the constructor only, so pushing, reading and clearing are real-time safe.
	class EventList
	{
	public:
		explicit EventList(size_t capacity = 0, size_t byteCapacity = 0) : m_events(capacity), m_bytes(byteCapacity) {}

		// Appends the event. Returns false and drops it if the list is full or the event is earlier than the last one:
		// whoever fills a list fills it in order.
		bool push(const Event& event) noexcept
		{
			if (m_size == m_events.size() || (m_size > 0 && event.offset < m_events[m_size - 1].offset))
				return false;
			m_events[m_size++] = event;
			return true;
		}

		// Appends a system exclusive message (F0 to F7) at the offset, copying its bytes.
		bool pushSysex(uint32_t offset, int16_t port, const uint8_t* bytes, uint32_t size) noexcept
		{
			if (m_byteCount + size > m_bytes.size())
				return false;
			Event event;
			event.offset = offset;
			event.type = EventType::Sysex;
			event.port = port;
			event.data[0] = static_cast<uint32_t>(m_byteCount);
			event.data[1] = size;
			if (!push(event))
				return false;
			if (size > 0)
				std::memcpy(m_bytes.data() + m_byteCount, bytes, size);
			m_byteCount += size;
			return true;
		}

		// The bytes of a Sysex event of this list.
		const uint8_t* bytes(const Event& event) const { return m_bytes.data() + event.data[0]; }

		void clear() noexcept
		{
			m_size = 0;
			m_byteCount = 0;
		}

		size_t size() const { return m_size; }
		bool empty() const { return m_size == 0; }
		size_t capacity() const { return m_events.size(); }

		const Event& operator[](size_t index) const { return m_events[index]; }
		const Event* begin() const { return m_events.data(); }
		const Event* end() const { return m_events.data() + m_size; }

	private:
		std::vector<Event> m_events; // sized to the capacity; the first m_size are in use
		std::vector<uint8_t> m_bytes;
		size_t m_size = 0;
		size_t m_byteCount = 0;
	};
}
