// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/rt/rtutil.h"

#include <atomic>
#include <utility>
#include <vector>

#ifdef _MSC_VER
// Padding from alignas is intended: producer and consumer data stay on separate cache lines.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace gnubase::rt
{
	// Bounded lock-free queue for exactly one producer thread and one consumer thread.
	// Memory is allocated in the constructor only; tryPush and tryPop never allocate, lock or block.
	template <typename T> class SpscQueue
	{
	public:
		// The capacity is rounded up to a power of two.
		explicit SpscQueue(size_t capacity)
			: m_slots(detail::roundUpToPowerOfTwo(capacity < 2 ? 2 : capacity)), m_mask(m_slots.size() - 1)
		{
		}

		size_t capacity() const { return m_slots.size(); }

		// Producer thread only. Returns false if the queue is full; the value is left untouched then.
		template <typename U> bool tryPush(U&& value)
		{
			const size_t head = m_head.load(std::memory_order_relaxed);
			if (head - m_cachedTail == m_slots.size())
			{
				m_cachedTail = m_tail.load(std::memory_order_acquire);
				if (head - m_cachedTail == m_slots.size())
					return false;
			}
			m_slots[head & m_mask] = std::forward<U>(value);
			m_head.store(head + 1, std::memory_order_release);
			return true;
		}

		// Consumer thread only. Returns false if the queue is empty.
		bool tryPop(T& value)
		{
			const size_t tail = m_tail.load(std::memory_order_relaxed);
			if (tail == m_cachedHead)
			{
				m_cachedHead = m_head.load(std::memory_order_acquire);
				if (tail == m_cachedHead)
					return false;
			}
			value = std::move(m_slots[tail & m_mask]);
			m_tail.store(tail + 1, std::memory_order_release);
			return true;
		}

	private:
		std::vector<T> m_slots;
		size_t m_mask;
		alignas(detail::g_cacheLineSize) std::atomic<size_t> m_head{0}; // written by the producer
		size_t m_cachedTail = 0;                                        // producer's last view of m_tail
		alignas(detail::g_cacheLineSize) std::atomic<size_t> m_tail{0}; // written by the consumer
		size_t m_cachedHead = 0;                                        // consumer's last view of m_head
	};
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
