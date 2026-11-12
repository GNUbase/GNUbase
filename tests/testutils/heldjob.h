// SPDX-License-Identifier: GPL-3.0-or-later
// Holds a job at its first job.progress until the test releases it, through Engine::setJobProgressHook, so that what
// the test does meanwhile, such as job.cancel or an edit, comes before the job goes on, however the threads are
// scheduled (#1079, #1154). Only the job's thread waits: events and commands go on meanwhile.
#pragma once

#include <chrono>
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
		// Holds the first job of the given kind, such as "media.locate", that sends a job.progress from now on, for a
		// minute at most. One HeldJob per engine at a time: a second throws std::logic_error. It must not outlive the
		// engine.
		HeldJob(engine::Engine& engine, std::string kind);

		// Releases the job and holds no other.
		~HeldJob();
		HeldJob(const HeldJob&) = delete;
		HeldJob& operator=(const HeldJob&) = delete;
		HeldJob(HeldJob&&) = delete;
		HeldJob& operator=(HeldJob&&) = delete;

		// Whether the job got to its first job.progress, for tests that wait while doing something else.
		bool held() const;

		// Waits until the job is at its first job.progress; false if it didn't get there in time.
		bool waitUntilHeld(std::chrono::milliseconds timeout = std::chrono::seconds(30));

		// The held job's id; 0 until it is held.
		uint64_t job() const;

		// Lets the job go on.
		void release();

	private:
		struct State;

		engine::Engine& m_engine;
		std::shared_ptr<State> m_state; // shared with the hook, which may still run on the job's thread after release
	};
}
