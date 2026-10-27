// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/rt/rtutil.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

#ifdef _MSC_VER
// Padding from alignas is intended: producer and consumer positions stay on separate cache lines.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace gnubase::rt
{
	// Bounded lock-free queue for any number of producer and consumer threads (Dmitry Vyukov's design).
	// Memory is allocated in the constructor only; tryPush and tryPop never allocate, lock or block.
	template <typename T> class MpmcQueue
	{
	public:
		// The capacity is rounded up to a power of two.
		explicit MpmcQueue(size_t capacity)
			: m_mask(detail::roundUpToPowerOfTwo(capacity < 2 ? 2 : capacity) - 1),
			  m_cells(std::make_unique<Cell[]>(m_mask + 1))
		{
			for (size_t i = 0; i <= m_mask; ++i)
				m_cells[i].sequence.store(i, std::memory_order_relaxed);
		}

		size_t capacity() const { return m_mask + 1; }

		// Returns false if the queue is full; the value is left untouched then.
		template <typename U> bool tryPush(U&& value)
		{
			size_t position = m_pushPosition.load(std::memory_order_relaxed);
			for (;;)
			{
				Cell& cell = m_cells[position & m_mask];
				const size_t sequence = cell.sequence.load(std::memory_order_acquire);
				const auto distance = static_cast<std::ptrdiff_t>(sequence - position);
				if (distance == 0)
				{
					if (m_pushPosition.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
					{
						cell.value = std::forward<U>(value);
						cell.sequence.store(position + 1, std::memory_order_release);
						return true;
					}
				}
				else if (distance < 0)
				{
					return false;
				}
				else
				{
					position = m_pushPosition.load(std::memory_order_relaxed);
				}
			}
		}

		// Returns false if the queue is empty.
		bool tryPop(T& value)
		{
			size_t position = m_popPosition.load(std::memory_order_relaxed);
			for (;;)
			{
				Cell& cell = m_cells[position & m_mask];
				const size_t sequence = cell.sequence.load(std::memory_order_acquire);
				const auto distance = static_cast<std::ptrdiff_t>(sequence - (position + 1));
				if (distance == 0)
				{
					if (m_popPosition.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
					{
						value = std::move(cell.value);
						cell.sequence.store(position + m_mask + 1, std::memory_order_release);
						return true;
					}
				}
				else if (distance < 0)
				{
					return false;
				}
				else
				{
					position = m_popPosition.load(std::memory_order_relaxed);
				}
			}
		}

	private:
		struct Cell
		{
			std::atomic<size_t> sequence;
			T value;
		};

		size_t m_mask;
		std::unique_ptr<Cell[]> m_cells;
		alignas(detail::g_cacheLineSize) std::atomic<size_t> m_pushPosition{0};
		alignas(detail::g_cacheLineSize) std::atomic<size_t> m_popPosition{0};
	};
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
