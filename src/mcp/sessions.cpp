// SPDX-License-Identifier: GPL-3.0-or-later
#include "mcp/sessions.h"

#include "mcp/transport.h"

#include <algorithm>
#include <utility>

namespace gnubase::mcp
{
	EventQueue::EventQueue(size_t capacity) : m_capacity(std::max<size_t>(capacity, 1))
	{
	}

	void EventQueue::post(const engine::Event& event)
	{
		{
			const std::scoped_lock lock(m_mutex);
			if (m_events.size() == m_capacity)
			{
				m_events.pop_front();
				++m_dropped;
			}
			m_events.push_back(event);
		}
		m_arrived.notify_all();
	}

	EventQueue::Taken EventQueue::take(const std::vector<std::string>& names, std::chrono::milliseconds timeout)
	{
		const auto wanted = [&names](const engine::Event& event)
		{ return names.empty() || std::find(names.begin(), names.end(), event.name) != names.end(); };
		std::unique_lock lock(m_mutex);
		m_arrived.wait_for(
			lock, timeout, [&] { return m_closed || std::any_of(m_events.begin(), m_events.end(), wanted); });
		Taken taken;
		const auto kept = std::stable_partition(
			m_events.begin(), m_events.end(), [&wanted](const engine::Event& event) { return !wanted(event); });
		std::move(kept, m_events.end(), std::back_inserter(taken.events));
		m_events.erase(kept, m_events.end());
		taken.dropped = std::exchange(m_dropped, 0);
		return taken;
	}

	void EventQueue::close()
	{
		{
			const std::scoped_lock lock(m_mutex);
			m_closed = true;
		}
		m_arrived.notify_all();
	}

	Sessions::Sessions(engine::Engine& engine, SessionLimits limits)
		: m_limits(limits), m_listener(engine, [this](const engine::Event& event) { post(event); })
	{
	}

	std::string Sessions::start(bool expires)
	{
		const std::scoped_lock lock(m_mutex);
		const Clock::time_point now = Clock::now();
		for (auto session = m_sessions.begin(); session != m_sessions.end();)
		{
			if (!idle(session->second, now))
			{
				++session;
				continue;
			}
			session->second.events->close();
			session = m_sessions.erase(session);
		}
		std::string id = makeToken();
		m_sessions[id] = {std::make_shared<EventQueue>(m_limits.queuedEvents), expires, now};
		return id;
	}

	std::shared_ptr<EventQueue> Sessions::find(const std::string& id)
	{
		const std::scoped_lock lock(m_mutex);
		const auto session = m_sessions.find(id);
		if (session == m_sessions.end())
			return nullptr;
		const Clock::time_point now = Clock::now();
		if (idle(session->second, now))
		{
			session->second.events->close();
			m_sessions.erase(session);
			return nullptr;
		}
		session->second.used = now;
		return session->second.events;
	}

	bool Sessions::end(const std::string& id)
	{
		const std::scoped_lock lock(m_mutex);
		const auto session = m_sessions.find(id);
		if (session == m_sessions.end())
			return false;
		session->second.events->close();
		m_sessions.erase(session);
		return true;
	}

	bool Sessions::idle(const Session& session, Clock::time_point now) const
	{
		return session.expires && now - session.used >= m_limits.idleTime;
	}

	void Sessions::post(const engine::Event& event)
	{
		const std::scoped_lock lock(m_mutex);
		for (const auto& [id, session] : m_sessions)
			session.events->post(event);
	}
}
