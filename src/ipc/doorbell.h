// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace gnubase::ipc
{
	// Wakes a thread in another process. The sender counts up a 32-bit word in shared memory and wakes whoever waits
	// for it to change:
	// - Linux: a futex on the word (shared, not process-private).
	// - macOS: the kernel's compare-and-wait on the word, the primitive behind the system's own futex-like locks
	//   (__ulock_wait/__ulock_wake with UL_COMPARE_AND_WAIT_SHARED). Named POSIX semaphores have no timed wait there.
	// - Windows: an auto-reset event named after the connection; the word still tells a real wake-up from a stale one.
	//   The wait ends at its deadline through a high-resolution waitable timer: a plain timed wait would end at the
	//   next tick of the system timer, up to 15.6 ms late.
	// ring() never blocks or allocates, so an audio thread may call it. wait() blocks until the word changes or the
	// timeout passes; an audio thread only uses it for the bounded wait for a plugin host's answer.
	class Doorbell
	{
	public:
		Doorbell() = default;
		~Doorbell();
		Doorbell(Doorbell&& other) noexcept;
		Doorbell& operator=(Doorbell&& other) noexcept;
		Doorbell(const Doorbell&) = delete;
		Doorbell& operator=(const Doorbell&) = delete;

		// `word` lives in shared memory that outlives the doorbell. `name` (letters, digits, '-' and '_') names the
		// OS object where there is one: the creating process creates it, the peer opens it. Throws std::system_error.
		static Doorbell create(std::atomic<uint32_t>& word, const std::string& name);
		static Doorbell open(std::atomic<uint32_t>& word, const std::string& name);

		// Counts the word up and wakes the waiter.
		void ring() noexcept;

		// The word's current value, to pass to wait.
		uint32_t value() const noexcept { return m_word->load(std::memory_order_acquire); }

		// Waits until the word differs from `seen`. Returns false if it still equals `seen` after `timeout`.
		bool wait(uint32_t seen, std::chrono::nanoseconds timeout) const noexcept;

	private:
		void release() noexcept;

		std::atomic<uint32_t>* m_word = nullptr;
		void* m_event = nullptr; // Windows
		void* m_timer = nullptr; // Windows: the deadline of wait()
	};
}
