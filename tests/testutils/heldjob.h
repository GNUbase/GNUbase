// SPDX-License-Identifier: GPL-3.0-or-later
// Holds a job at its first job.progress until the test releases it, through Engine::setJobProgressHook, so that what
// the test does meanwhile, such as job.cancel or an edit, comes before the job goes on, however the threads are
// scheduled (#1079, #1154). Only the job's thread waits: events and commands go on meanwhile.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace gnubase::engine
{
	class Engine;
}

namespace gnubase::test
{
	class HeldJob
	{
	public:
		// Holds the first job of the given kind, such as "media.locate", that sends a job.progress from now on, or the
		// first jobs of the kind, each at its first job.progress, for a minute at most. One HeldJob per engine at a
		// time: a second throws std::logic_error. It must not outlive the engine.
		HeldJob(engine::Engine& engine, std::string kind, size_t jobs = 1);

		// Releases the jobs and holds no other.
		~HeldJob();
		HeldJob(const HeldJob&) = delete;
		HeldJob& operator=(const HeldJob&) = delete;
		HeldJob(HeldJob&&) = delete;
		HeldJob& operator=(HeldJob&&) = delete;

		// Whether all the jobs got to their first job.progress, for tests that wait while doing something else.
		bool held() const;

		// Waits until all the jobs are at their first job.progress; false if they didn't get there in time.
		bool waitUntilHeld(std::chrono::milliseconds timeout = std::chrono::seconds(30));

		// The first held job's id; 0 until one is held.
		uint64_t job() const;

		// Lets the jobs go on.
		void release();

	private:
		struct State;

		engine::Engine& m_engine;
		std::shared_ptr<State> m_state; // shared with the hook, which may still run on the job's thread after release
	};
}
