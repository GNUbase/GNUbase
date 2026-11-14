// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/clock.h"

#include "platform/log.h"

#include <utility>

namespace gnubase::engine
{
	Clock::~Clock()
	{
		stop();
	}

	Clock::TimePoint Clock::now() const
	{
		const std::scoped_lock lock(m_mutex);
		return current();
	}

	Clock::TimePoint Clock::current() const
	{
		return m_held.value_or(std::chrono::steady_clock::now()) + m_offset;
	}

	void Clock::hold()
	{
		const std::scoped_lock lock(m_mutex);
		m_held = std::chrono::steady_clock::now();
	}

	void Clock::start(Duration period, std::function<void()> tick)
	{
		const std::scoped_lock lock(m_mutex);
		GNUBASE_ASSERT(!m_started);
		if (m_started || m_stopped)
			return;
		m_next = current() + period;
		m_thread = std::thread([this, period, tick = std::move(tick)] { run(period, tick); });
		m_started = true; // before the thread can lock, so that an advance right away waits for it
	}

	void Clock::stop()
	{
		{
			const std::scoped_lock lock(m_mutex);
			m_stopped = true;
		}
		m_changed.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}

	void Clock::advance(Duration by)
	{
		std::unique_lock lock(m_mutex);
		m_offset += by;
		const uint64_t advance = ++m_advances;
		m_changed.notify_all();
		m_changed.wait(lock, [this, advance] { return !m_started || m_stopped || m_seen >= advance; });
	}

	void Clock::run(Duration period, const std::function<void()>& tick)
	{
		std::unique_lock lock(m_mutex);
		while (!m_stopped)
		{
			const TimePoint time = current();
			if (time >= m_next)
			{
				m_next = time + period;
				lock.unlock();
				tick();
				lock.lock();
				continue; // looks at the time again before it counts as seen
			}
			m_seen = m_advances;
			m_changed.notify_all();
			m_changed.wait_for(lock, m_next - time);
		}
	}
}
