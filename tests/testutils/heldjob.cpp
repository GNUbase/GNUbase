// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/heldjob.h"

#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
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

		engine::Json run(engine::Client& client, const std::string& command, engine::Json parameters)
		{
			const engine::Result result = client.call(command, std::move(parameters));
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}
	}

	struct HeldJob::State
	{
		std::string kind;
		std::chrono::milliseconds hold{0};
		size_t count = 1; // how many it holds
		mutable std::mutex mutex;
		std::condition_variable changed;
		std::vector<uint64_t> jobs; // the ones that got to their first job.progress, in that order
		bool released = false;      // by the test
		bool lapsed = false;        // a hold, before the test released the jobs
		int holds = 0;              // how often the hook held a job

		bool allThere() const { return jobs.size() >= count; }
		bool holding() const { return allThere() && !released && !lapsed; }
	};

	HeldJob::HeldJob(engine::Engine& engine, std::string kind, std::chrono::milliseconds hold, size_t jobs)
		: m_engine(engine), m_state(std::make_shared<State>())
	{
		{
			Holders& all = holders();
			const std::scoped_lock lock(all.mutex);
			if (!all.engines.insert(&engine).second)
				throw std::logic_error("another HeldJob holds this engine's jobs");
		}
		m_state->kind = std::move(kind);
		m_state->hold = hold;
		m_state->count = jobs;
		m_engine.setJobProgressHook(
			[state = m_state](uint64_t job, const std::string& jobKind)
			{
				if (jobKind != state->kind)
					return;
				std::unique_lock lock(state->mutex);
				// Each job once, the first count of them.
				if (state->released || state->allThere() ||
					std::find(state->jobs.begin(), state->jobs.end(), job) != state->jobs.end())
					return;
				state->jobs.push_back(job);
				++state->holds;
				state->changed.notify_all();
				if (!state->changed.wait_for(lock, state->hold, [&state] { return state->released; }))
				{
					state->lapsed = true;
					state->changed.notify_all();
				}
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
		return m_state->holding();
	}

	bool HeldJob::waitUntilHeld(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m_state->mutex);
		return m_state->changed.wait_for(lock, timeout, [this] { return m_state->allThere(); }) && m_state->holding();
	}

	uint64_t HeldJob::job() const
	{
		const std::scoped_lock lock(m_state->mutex);
		return m_state->jobs.empty() ? 0 : m_state->jobs.front();
	}

	int HeldJob::holds() const
	{
		const std::scoped_lock lock(m_state->mutex);
		return m_state->holds;
	}

	bool HeldJob::release()
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

	engine::Json waitForJob(engine::Client& client, uint64_t job, std::vector<engine::Json>* progress)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		engine::Json finished; // null until the job finishes
		while (finished.is_null() && std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<engine::Message> message = client.wait(std::chrono::milliseconds(100));
			if (!message)
			{
				// A wait for another job may have taken this job's events: once none are left, job.get tells.
				if (engine::Json state = run(client, "job.get", {{"job", job}}).at("job");
					state.at("state") != "running")
					finished = std::move(state);
				continue;
			}
			const auto* event = std::get_if<engine::Event>(&*message);
			if (event == nullptr)
				continue;
			if (event->name == "job.progress" && event->data.at("job") == job && progress != nullptr)
				progress->push_back(event->data);
			if (event->name == "job.finished" && event->data.at("job").at("id") == job)
				finished = event->data.at("job");
		}
		INFO("job " << job << " didn't finish");
		REQUIRE(!finished.is_null());
		return finished;
	}

	engine::Json whileHeld(engine::Engine& engine, engine::Client& client, const std::string& command,
		engine::Json parameters, const std::function<void(uint64_t job)>& during, std::vector<engine::Json>* progress)
	{
		HeldJob held(engine, command);
		const engine::Json started = run(client, command, std::move(parameters)).at("job");
		CHECK(started.at("kind") == command);
		const auto job = started.at("id").get<uint64_t>();
		REQUIRE(held.waitUntilHeld());
		CHECK(held.job() == job);
		during(job);
		CHECK(run(client, "job.get", {{"job", job}}).at("job").at("state") == "running");
		{
			INFO("the hold lapsed before " << command << "'s job was released");
			CHECK(held.release());
		}
		return waitForJob(client, job, progress);
	}
}
