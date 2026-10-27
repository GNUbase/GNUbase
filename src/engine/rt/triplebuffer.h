// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace gnubase::rt
{
	namespace detail
	{
		inline constexpr uint8_t g_tripleBufferIndexMask = 3;
		inline constexpr uint8_t g_tripleBufferNewData = 4;
	}

	// Hands the latest value from one writer thread to one reader thread, e.g. meters and positions to the UI.
	// Neither side ever waits; the reader skips values it was too slow to see. No allocations after construction.
	template <typename T> class TripleBuffer
	{
	public:
		// Writer: fill writeBuffer(), then publish() it.
		T& writeBuffer() { return m_buffers[m_writeIndex]; }

		void publish()
		{
			const auto newData = static_cast<uint8_t>(m_writeIndex | detail::g_tripleBufferNewData);
			m_writeIndex = m_middle.exchange(newData, std::memory_order_acq_rel) & detail::g_tripleBufferIndexMask;
		}

		// Reader: switches readBuffer() to the newest published value. Returns false if nothing new was published.
		bool update()
		{
			if ((m_middle.load(std::memory_order_relaxed) & detail::g_tripleBufferNewData) == 0)
				return false;
			m_readIndex = m_middle.exchange(m_readIndex, std::memory_order_acq_rel) & detail::g_tripleBufferIndexMask;
			return true;
		}

		const T& readBuffer() const { return m_buffers[m_readIndex]; }

	private:
		std::array<T, 3> m_buffers{};
		uint8_t m_writeIndex = 0;
		uint8_t m_readIndex = 1;
		std::atomic<uint8_t> m_middle{2};
	};
}
