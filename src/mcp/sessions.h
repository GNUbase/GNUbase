// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// MCP sessions and the engine events they receive for events_take (#273).
namespace gnubase::mcp
{
	struct SessionLimits
	{
		size_t queuedEvents = 1000; // per session; beyond, the oldest events are dropped
		std::chrono::steady_clock::duration idleTime = std::chrono::minutes(30); // for sessions that expire
	};

	// The engine events of one session that haven't been taken yet, oldest first. When it is full, the oldest event
	// is dropped and counted. Thread-safe.
	class EventQueue
	{
	public:
		explicit EventQueue(size_t capacity);

		void post(const engine::Event& event);

		struct Taken
		{
			std::vector<engine::Event> events;
			uint64_t dropped = 0; // since the last take
		};

		// Takes the events with one of the names, or every event for no names, waiting up to timeout for the first.
		// The others stay queued.
		Taken take(const std::vector<std::string>& names, std::chrono::milliseconds timeout);

		// Ends the session: a waiting take returns at once.
		void close();

	private:
		const size_t m_capacity;
		std::mutex m_mutex;
		std::condition_variable m_arrived;
		std::deque<engine::Event> m_events;
		uint64_t m_dropped = 0;
		bool m_closed = false;
	};

	// The sessions of an MCP server. Each receives every engine event from its start on, through one engine listener,
	// so no engine queue grows while nobody asks. Thread-safe.
	class Sessions
	{
	public:
		Sessions(engine::Engine& engine, SessionLimits limits);

		// Starts a session and returns its id, a random token. A session that expires ends when it isn't used for
		// the idle time; starting a session ends those.
		std::string start(bool expires);

		// The session's events, or null if it ended or expired. Counts as using it.
		std::shared_ptr<EventQueue> find(const std::string& id);

		// Ends a session. False if there is none.
		bool end(const std::string& id);

	private:
		using Clock = std::chrono::steady_clock;

		struct Session
		{
			std::shared_ptr<EventQueue> events;
			bool expires = false;
			Clock::time_point used;
		};

		bool idle(const Session& session, Clock::time_point now) const;
		void post(const engine::Event& event);

		const SessionLimits m_limits;
		std::mutex m_mutex;
		std::map<std::string, Session> m_sessions;
		engine::Listener m_listener; // last, so it stops before the sessions go
	};
}
