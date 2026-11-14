// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <thread>

namespace gnubase::test
{
	// Joins a referenced test thread on scope exit, optionally setting its stop flag first. The thread and flag
	// must outlive the guard. join() also lets a test finish the worker before checking its results.
	class StopAndJoin
	{
	public:
		explicit StopAndJoin(std::thread& thread) noexcept : m_thread(thread) {}
		StopAndJoin(std::atomic<bool>& stop, std::thread& thread) noexcept : m_stop(&stop), m_thread(thread) {}
		~StopAndJoin() { join(); }
		StopAndJoin(const StopAndJoin&) = delete;
		StopAndJoin& operator=(const StopAndJoin&) = delete;

		void join() const
		{
			if (m_stop != nullptr)
				*m_stop = true;
			if (m_thread.joinable())
				m_thread.join();
		}

	private:
		std::atomic<bool>* m_stop = nullptr;
		std::thread& m_thread;
	};
}
