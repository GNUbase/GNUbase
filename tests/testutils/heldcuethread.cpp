// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/heldcuethread.h"

#include "engine/engine.h"

#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>

namespace gnubase::test
{
	namespace
	{
		// The engines whose cue thread a HeldCueThread holds, so that a second one per engine is refused: it would
		// replace the first one's hook and, when it goes, unhook it.
		struct Holders
		{
			std::mutex mutex;
			std::set<const engine::Engine*> engines;
		};

		Holders& holders()
		{
			static Holders all;
			return all;
		}
	}

	struct HeldCueThread::State
	{
		std::chrono::milliseconds hold{0};
		std::mutex mutex;
		std::condition_variable changed;
		bool arrived = false;  // the cue thread got to the hook
		bool released = false; // by the test
		bool lapsed = false;   // the hold, before the test released the thread

		bool holding() const { return arrived && !released && !lapsed; }
	};

	HeldCueThread::HeldCueThread(engine::Engine& engine, std::chrono::milliseconds hold)
		: m_engine(engine), m_state(std::make_shared<State>())
	{
		{
			Holders& all = holders();
			const std::scoped_lock lock(all.mutex);
			if (!all.engines.insert(&engine).second)
				throw std::logic_error("another HeldCueThread holds this engine's cue thread");
		}
		m_state->hold = hold;
		m_engine.setCueStartHook(
			[state = m_state]
			{
				std::unique_lock lock(state->mutex);
				if (state->arrived || state->released) // only the first start's thread
					return;
				state->arrived = true;
				state->changed.notify_all();
				if (!state->changed.wait_for(lock, state->hold, [&state] { return state->released; }))
				{
					state->lapsed = true;
					state->changed.notify_all();
				}
			});
	}

	HeldCueThread::~HeldCueThread()
	{
		release();
		m_engine.setCueStartHook({});
		Holders& all = holders();
		const std::scoped_lock lock(all.mutex);
		all.engines.erase(&m_engine);
	}

	bool HeldCueThread::waitUntilHeld(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m_state->mutex);
		return m_state->changed.wait_for(lock, timeout, [this] { return m_state->arrived; }) && m_state->holding();
	}

	bool HeldCueThread::release()
	{
		bool wasHeld = false;
		{
			const std::scoped_lock lock(m_state->mutex);
			wasHeld = m_state->holding();
			m_state->released = true;
		}
		m_state->changed.notify_all();
		return wasHeld;
	}
}
