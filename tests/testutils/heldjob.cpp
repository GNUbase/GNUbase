// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/heldjob.h"

#include "engine/engine.h"

#include <condition_variable>
#include <mutex>
#include <utility>

namespace gnubase::test
{
	struct HeldJob::State
	{
		std::string kind;
		mutable std::mutex mutex;
		std::condition_variable changed;
		uint64_t job = 0; // the held one, once it is held
		bool released = false;
	};

	HeldJob::HeldJob(engine::Engine& engine, std::string kind) : m_engine(engine), m_state(std::make_shared<State>())
	{
		m_state->kind = std::move(kind);
		m_engine.setJobProgressHook(
			[state = m_state](uint64_t job, const std::string& jobKind)
			{
				if (jobKind != state->kind)
					return;
				std::unique_lock lock(state->mutex);
				if (state->released || (state->job != 0 && state->job != job))
					return;
				state->job = job;
				state->changed.notify_all();
				state->changed.wait_for(lock, std::chrono::minutes(1), [&state] { return state->released; });
			});
	}

	HeldJob::~HeldJob()
	{
		release();
		m_engine.setJobProgressHook({});
	}

	bool HeldJob::held() const
	{
		const std::scoped_lock lock(m_state->mutex);
		return m_state->job != 0;
	}

	bool HeldJob::waitUntilHeld(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m_state->mutex);
		return m_state->changed.wait_for(lock, timeout, [this] { return m_state->job != 0; });
	}

	uint64_t HeldJob::job() const
	{
		const std::scoped_lock lock(m_state->mutex);
		return m_state->job;
	}

	void HeldJob::release()
	{
		{
			const std::scoped_lock lock(m_state->mutex);
			m_state->released = true;
		}
		m_state->changed.notify_all();
	}
}
