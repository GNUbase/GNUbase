// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace gnubase::engine
{
	// The time that the engine's timed work goes by, such as due auto saves (project.autoSave), and the thread that
	// ticks with it: steady_clock, which tests move forward with advance instead of waiting.
	class Clock
	{
	public:
		using Duration = std::chrono::steady_clock::duration;
		using TimePoint = std::chrono::steady_clock::time_point;

		Clock() = default;
		~Clock(); // stops
		Clock(const Clock&) = delete;
		Clock& operator=(const Clock&) = delete;

		TimePoint now() const;

		// Calls tick on a thread of its own once period passed on the clock, then again a period after that tick,
		// until it stops: once however long the clock went on meanwhile, such as while the computer slept. tick must
		// not call the clock. Starts once.
		void start(Duration period, std::function<void()> tick);

		// Stops the ticks; one that runs finishes first.
		void stop();

		// For tests: moves the clock forward and, once it started, returns only when the thread looked at the time,
		// and ticked if a period passed. So whatever that tick queued, such as a command, is queued before what the
		// caller does afterwards.
		void advance(Duration by);

		// For tests: stops the clock where it is, so that only advance moves it on, however long they take.
		void hold();

	private:
		TimePoint current() const; // with m_mutex locked

		void run(Duration period, const std::function<void()>& tick);

		mutable std::mutex m_mutex; // guards everything below except the thread
		std::condition_variable m_changed;
		Duration m_offset{0};            // how far advance moved the clock
		std::optional<TimePoint> m_held; // where hold stopped the steady clock
		uint64_t m_advances = 0;         // how often advance moved it
		uint64_t m_seen = 0;             // m_advances when the thread last looked at the time and ticked if due
		TimePoint m_next;                // the next tick
		bool m_started = false;
		bool m_stopped = false;
		std::thread m_thread;
	};
}
