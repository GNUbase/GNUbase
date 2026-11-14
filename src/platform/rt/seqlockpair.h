// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

namespace gnubase::rt
{
	// Two values that one writer thread stores together and any number of reader threads load together, without
	// locks: a sequence lock. The writer never waits, so it may be an audio thread; a reader retries while a store is
	// under way, so readers must not be audio threads. A reader never sees half a store, and never a pair older than
	// the last one it loaded. Both types must be lock-free atomics.
	template <typename A, typename B> class SeqLockPair
	{
	public:
		static_assert(std::atomic<A>::is_always_lock_free && std::atomic<B>::is_always_lock_free);

		// The writer thread.
		void store(A a, B b) noexcept
		{
			const uint64_t sequence = m_sequence.load(std::memory_order_relaxed);
			m_sequence.store(sequence + 1, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			m_a.store(a, std::memory_order_relaxed);
			m_b.store(b, std::memory_order_relaxed);
			m_sequence.store(sequence + 2, std::memory_order_release);
		}

		// Any thread but an audio thread.
		std::pair<A, B> load() const noexcept
		{
			for (;;)
			{
				const uint64_t before = m_sequence.load(std::memory_order_acquire);
				const A a = m_a.load(std::memory_order_relaxed);
				const B b = m_b.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_acquire);
				if ((before & 1U) == 0 && m_sequence.load(std::memory_order_relaxed) == before)
					return {a, b};
				std::this_thread::yield();
			}
		}

	private:
		std::atomic<uint64_t> m_sequence{0}; // odd while a store is under way
		std::atomic<A> m_a{};
		std::atomic<B> m_b{};
	};
}
