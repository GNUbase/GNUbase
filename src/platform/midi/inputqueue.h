// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"
#include "platform/hosttime.h"
#include "platform/rt/mpmcqueue.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gnubase::midi
{
	// Carries MIDI input from driver threads to the audio thread and turns host times into frame offsets. Any number of
	// threads push; one audio thread takes block by block. Memory is allocated in the constructor only.
	//
	// A block that starts at host time blockTime gets the messages that arrived during the block duration before it,
	// at the offsets of their arrival: one block of latency, without jitter. Later messages wait for their block;
	// earlier ones, late because a block came late, go to offset 0. Offsets never decrease.
	class InputQueue
	{
	public:
		explicit InputQueue(size_t capacity = 4096) : m_queue(capacity) {}

		// Returns false and drops the message if the queue is full.
		bool push(int port, const audio::MidiMessage& message, HostTime time) noexcept
		{
			if (m_queue.tryPush(Entry{port, message, time}))
				return true;
			m_dropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		// Audio thread. Calls handle(port, message) for each message of the block, message.offset set.
		template <typename Handle>
		void take(HostTime blockTime, double sampleRate, uint32_t frames, Handle&& handle) noexcept
		{
			if (frames == 0)
				return;
			const double framesPerNanosecond = sampleRate / 1e9;
			const auto blockStart = blockTime - static_cast<HostTime>(std::llround(frames / framesPerNanosecond));
			uint32_t last = 0;
			for (;;)
			{
				if (!m_hasPending)
				{
					if (!m_queue.tryPop(m_pending))
						return;
					m_hasPending = true;
				}
				if (m_pending.time >= blockTime)
					return; // for a later block
				const double offset =
					std::round(static_cast<double>(m_pending.time - blockStart) * framesPerNanosecond);
				last = std::max(last, static_cast<uint32_t>(std::clamp(offset, 0.0, frames - 1.0)));
				audio::MidiMessage message = m_pending.message;
				message.offset = last;
				m_hasPending = false;
				handle(m_pending.port, message);
			}
		}

		// Drops every message. Not while the audio thread takes.
		void clear() noexcept
		{
			Entry entry;
			while (m_queue.tryPop(entry))
			{
			}
			m_hasPending = false;
		}

		// Messages dropped because the queue was full.
		size_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

	private:
		struct Entry
		{
			int port = 0;
			audio::MidiMessage message;
			HostTime time = 0;
		};

		rt::MpmcQueue<Entry> m_queue;
		Entry m_pending; // popped, but for a later block
		bool m_hasPending = false;
		std::atomic<size_t> m_dropped{0};
	};
}
