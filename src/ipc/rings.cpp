// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/rings.h"

#include <cstring>

namespace gnubase::ipc
{
	namespace
	{
		constexpr auto g_headerSize = static_cast<uint32_t>(sizeof(MessageHeader));

		// Bytes a message takes in the ring: its header and its payload rounded up to 8 bytes.
		constexpr uint32_t recordSize(uint32_t payload)
		{
			return g_headerSize + ((payload + 7U) & ~7U);
		}
	}

	MessageRing::MessageRing(RingIndices& indices, std::byte* data, uint32_t capacity) noexcept
		: m_indices(&indices), m_data(data), m_capacity(capacity), m_mask(capacity - 1)
	{
	}

	bool MessageRing::tryWrite(uint16_t type, uint16_t version, const void* payload, uint32_t size) noexcept
	{
		if (type == 0 || m_data == nullptr || size > maxMessageSize())
			return false;
		const uint32_t need = recordSize(size);
		uint32_t position = m_head & m_mask;
		const uint32_t toEnd = m_capacity - position;
		const uint32_t total = need > toEnd ? toEnd + need : need;
		if (m_capacity - (m_head - m_cachedTail) < total)
		{
			const uint32_t tail = m_indices->tail.load(std::memory_order_acquire);
			if (m_head - tail > m_capacity)
				return false; // the consumer's index is impossible; it will find out itself
			m_cachedTail = tail;
			if (m_capacity - (m_head - tail) < total)
				return false;
		}
		uint32_t head = m_head;
		if (need > toEnd)
		{
			const MessageHeader padding{toEnd - g_headerSize, 0, 0};
			std::memcpy(m_data + position, &padding, sizeof(padding));
			head += toEnd;
			position = 0;
		}
		const MessageHeader header{size, type, version};
		std::memcpy(m_data + position, &header, sizeof(header));
		if (size > 0)
			std::memcpy(m_data + position + g_headerSize, payload, size);
		m_head = head + need;
		m_indices->head.store(m_head, std::memory_order_release);
		return true;
	}

	bool MessageRing::readable() const noexcept
	{
		if (m_corrupt || m_tail != m_cachedHead)
			return true;
		const uint32_t head = m_indices->head.load(std::memory_order_acquire);
		return head != m_tail;
	}

	ReadStatus MessageRing::tryRead(MessageHeader& header, void* buffer, size_t bufferSize) noexcept
	{
		if (m_corrupt || m_data == nullptr)
			return m_corrupt ? ReadStatus::Corrupt : ReadStatus::Empty;
		if (m_tail == m_cachedHead)
		{
			const uint32_t head = m_indices->head.load(std::memory_order_acquire);
			// The producer only ever publishes whole records, which are multiples of 8 bytes.
			if (head - m_tail > m_capacity || head % 8 != 0)
				return fail();
			m_cachedHead = head;
			if (head == m_tail)
				return ReadStatus::Empty;
		}
		uint32_t tail = m_tail;
		// At most a padding record, then a message.
		for (int record = 0; record < 2; ++record)
		{
			const uint32_t available = m_cachedHead - tail;
			const uint32_t position = tail & m_mask;
			const uint32_t toEnd = m_capacity - position;
			if (available < g_headerSize)
				return fail();
			MessageHeader next;
			std::memcpy(&next, m_data + position, sizeof(next));
			if (next.size > maxMessageSize())
				return fail();
			const uint32_t need = recordSize(next.size);
			if (next.type == 0)
			{
				// Padding fills the ring up to its end, and a message always follows it.
				if (need != toEnd || need >= available)
					return fail();
				tail += need;
				continue;
			}
			if (need > toEnd || need > available || next.size > bufferSize)
				return fail();
			if (next.size > 0)
				std::memcpy(buffer, m_data + position + g_headerSize, next.size);
			header = next;
			m_tail = tail + need;
			m_indices->tail.store(m_tail, std::memory_order_release);
			return ReadStatus::Ok;
		}
		return fail();
	}

	ReadStatus MessageRing::fail() noexcept
	{
		m_corrupt = true;
		return ReadStatus::Corrupt;
	}

	BlockRing::BlockRing(RingIndices& indices, std::byte* data, uint32_t slotSize, uint32_t slotCount) noexcept
		: m_indices(&indices), m_data(data), m_slotSize(slotSize), m_count(slotCount), m_mask(slotCount - 1)
	{
	}

	std::byte* BlockRing::beginWrite() noexcept
	{
		if (m_data == nullptr)
			return nullptr;
		if (m_head - m_cachedTail >= m_count)
		{
			const uint32_t tail = m_indices->tail.load(std::memory_order_acquire);
			if (m_head - tail >= m_count)
				return nullptr; // full, or the consumer's index is impossible
			m_cachedTail = tail;
		}
		return slot(m_head);
	}

	void BlockRing::commitWrite() noexcept
	{
		++m_head;
		m_indices->head.store(m_head, std::memory_order_release);
	}

	std::byte* BlockRing::beginRead() noexcept
	{
		if (m_corrupt || m_data == nullptr)
			return nullptr;
		if (m_tail == m_cachedHead)
		{
			const uint32_t head = m_indices->head.load(std::memory_order_acquire);
			if (head - m_tail > m_count)
			{
				m_corrupt = true;
				return nullptr;
			}
			m_cachedHead = head;
			if (head == m_tail)
				return nullptr;
		}
		return slot(m_tail);
	}

	void BlockRing::endRead() noexcept
	{
		++m_tail;
		m_indices->tail.store(m_tail, std::memory_order_release);
	}
}
