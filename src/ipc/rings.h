// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ipc/protocol.h"

#include <cstddef>
#include <cstdint>

namespace gnubase::ipc
{
	enum class ReadStatus : uint8_t
	{
		Ok,
		Empty,
		Corrupt, // the peer broke the protocol: impossible indices, sizes or headers. Stays so for the reader.
	};

	// Single-producer, single-consumer ring of variable-sized messages in shared memory: events on the audio path and
	// control messages. Each message is a MessageHeader and its payload, padded to 8 bytes; a message that does not fit
	// before the end of the ring starts at its beginning after a padding record. The reader never trusts the memory:
	// it checks every index and header, copies messages out and never reads outside the ring. tryWrite and tryRead
	// never allocate, lock or block.
	class MessageRing
	{
	public:
		MessageRing() = default;
		// `capacity` is a power of two of at least 64 bytes; `data` is 8-byte aligned.
		MessageRing(RingIndices& indices, std::byte* data, uint32_t capacity) noexcept;

		// The largest payload one message can have: half the ring minus a header, so a message always fits after
		// padding.
		uint32_t maxMessageSize() const noexcept
		{
			return (m_capacity / 2) - static_cast<uint32_t>(sizeof(MessageHeader));
		}

		// Producer only. Returns false if the ring has no room, the message is larger than maxMessageSize, or `type`
		// is 0 (reserved for padding).
		bool tryWrite(uint16_t type, uint16_t version, const void* payload, uint32_t size) noexcept;

		// Consumer only. Copies the next message's payload into `buffer` and its header into `header`. A message
		// larger than `bufferSize` counts as corrupt, so the buffer should hold maxMessageSize bytes, or the largest
		// message the protocol allows on this ring.
		ReadStatus tryRead(MessageHeader& header, void* buffer, size_t bufferSize) noexcept;

		// Consumer only: true if tryRead would not return Empty.
		bool readable() const noexcept;

	private:
		ReadStatus fail() noexcept;

		RingIndices* m_indices = nullptr;
		std::byte* m_data = nullptr;
		uint32_t m_capacity = 0;
		uint32_t m_mask = 0;
		// Each side's own index lives here, so the peer cannot change it; the copy in shared memory only informs the
		// peer.
		uint32_t m_head = 0;       // producer
		uint32_t m_cachedTail = 0; // producer's last view of the consumer
		uint32_t m_tail = 0;       // consumer
		uint32_t m_cachedHead = 0; // consumer's last view of the producer
		bool m_corrupt = false;    // consumer
	};

	// Single-producer, single-consumer ring of fixed-size slots in shared memory, for audio blocks. Both sides work in
	// the slots directly: the producer fills a slot and commits it, the consumer reads it and hands it back. Never
	// allocates, locks or blocks.
	class BlockRing
	{
	public:
		BlockRing() = default;
		// `slotCount` is a power of two.
		BlockRing(RingIndices& indices, std::byte* data, uint32_t slotSize, uint32_t slotCount) noexcept;

		uint32_t slotSize() const noexcept { return m_slotSize; }

		// Producer only: the next free slot, or nullptr if all are in use. commitWrite hands it to the consumer.
		std::byte* beginWrite() noexcept;
		void commitWrite() noexcept;

		// Consumer only: the oldest committed slot, or nullptr if there is none or the ring is corrupt. endRead hands
		// it back to the producer.
		std::byte* beginRead() noexcept;
		void endRead() noexcept;

		// True once the consumer found impossible indices.
		bool corrupt() const noexcept { return m_corrupt; }

	private:
		std::byte* slot(uint32_t index) const noexcept { return m_data + (size_t{index & m_mask} * m_slotSize); }

		RingIndices* m_indices = nullptr;
		std::byte* m_data = nullptr;
		uint32_t m_slotSize = 0;
		uint32_t m_count = 0;
		uint32_t m_mask = 0;
		uint32_t m_head = 0;
		uint32_t m_cachedTail = 0;
		uint32_t m_tail = 0;
		uint32_t m_cachedHead = 0;
		bool m_corrupt = false;
	};
}
