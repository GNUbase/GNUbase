// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/heldjob.h"

#include "engine/engine.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gnubase::test
{
	namespace
	{
		// The engines whose jobs a HeldJob holds, so that a second one per engine is refused: it would replace the
		// first one's hook and, when it goes, unhook it.
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

	struct HeldJob::State
	{
		std::string kind;
		mutable std::mutex mutex;
		std::condition_variable changed;
		size_t count = 1;           // how many it holds
		std::vector<uint64_t> jobs; // the held ones, in the order they got there
		bool released = false;

		bool allHeld() const { return jobs.size() >= count; }
	};

	HeldJob::HeldJob(engine::Engine& engine, std::string kind, size_t jobs)
		: m_engine(engine), m_state(std::make_shared<State>())
	{
		{
			Holders& all = holders();
			const std::scoped_lock lock(all.mutex);
			if (!all.engines.insert(&engine).second)
				throw std::logic_error("another HeldJob holds this engine's jobs");
		}
		m_state->kind = std::move(kind);
		m_state->count = jobs;
		m_engine.setJobProgressHook(
			[state = m_state](uint64_t job, const std::string& jobKind)
			{
				if (jobKind != state->kind)
					return;
				std::unique_lock lock(state->mutex);
				if (state->released)
					return;
				if (std::find(state->jobs.begin(), state->jobs.end(), job) == state->jobs.end())
				{
					if (state->allHeld())
						return;
					state->jobs.push_back(job);
				}
				state->changed.notify_all();
				state->changed.wait_for(lock, std::chrono::minutes(1), [&state] { return state->released; });
			});
	}

	HeldJob::~HeldJob()
	{
		release();
		m_engine.setJobProgressHook({});
		Holders& all = holders();
		const std::scoped_lock lock(all.mutex);
		all.engines.erase(&m_engine);
	}

	bool HeldJob::held() const
	{
		const std::scoped_lock lock(m_state->mutex);
		return m_state->allHeld();
	}

	bool HeldJob::waitUntilHeld(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m_state->mutex);
		return m_state->changed.wait_for(lock, timeout, [this] { return m_state->allHeld(); });
	}

	uint64_t HeldJob::job() const
	{
		const std::scoped_lock lock(m_state->mutex);
		return m_state->jobs.empty() ? 0 : m_state->jobs.front();
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
