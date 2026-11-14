// SPDX-License-Identifier: GPL-3.0-or-later
// Holds a job at its first job.progress until the test releases it, through Engine::setJobProgressHook, so that what
// the test does meanwhile, such as job.cancel or an edit, comes before the job goes on, however the threads are
// scheduled (#1079, #1154). Only the job's thread waits: events and commands go on meanwhile.
#pragma once

#include "engine/schema.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::engine
{
	class Client;
	class Engine;
}

namespace gnubase::test
{
	class HeldJob
	{
	public:
		// Holds the first job of the given kind, such as "media.locate", that sends a job.progress from now on, or the
		// first jobs of the kind, each at its first job.progress, once, for hold at most: then the hold lapses, and the
		// job goes on as if released and is held at none of its later job.progress events. One HeldJob per engine at a
		// time: a second throws std::logic_error and leaves the first one holding. It must not outlive the engine.
		HeldJob(engine::Engine& engine, std::string kind, std::chrono::milliseconds hold = std::chrono::minutes(1),
			size_t jobs = 1);

		// Releases the jobs and holds no other.
		~HeldJob();
		HeldJob(const HeldJob&) = delete;
		HeldJob& operator=(const HeldJob&) = delete;
		HeldJob(HeldJob&&) = delete;
		HeldJob& operator=(HeldJob&&) = delete;

		// Whether all the jobs are held at their first job.progress now: not before they got there, nor once they were
		// released or a hold lapsed.
		bool held() const;

		// Waits until all the jobs are at their first job.progress and returns whether they are held there; false if
		// they didn't get there in time, or if they were released or a hold lapsed already.
		bool waitUntilHeld(std::chrono::milliseconds timeout = std::chrono::seconds(30));

		// The id of the first job that got to its first job.progress, held there or not anymore; 0 until then.
		uint64_t job() const;

		// How often a job was held: 0 until the job got to its first job.progress, then 1 per job that did, also after
		// a release or a lapsed hold, since a job is held at none of its later job.progress events.
		int holds() const;

		// Lets the jobs go on and holds no other. Returns whether all the jobs were held until now: false if one didn't
		// get to its first job.progress yet or a hold lapsed, so that it may have gone on before what the test did.
		bool release();

	private:
		struct State;

		engine::Engine& m_engine;
		std::shared_ptr<State> m_state; // shared with the hook, which may still run on the job's thread after release
	};

	// Waits for the job's job.finished event, or, once the client has no events left, for job.get to tell that it
	// ended, a minute at most, and returns the job. Progress events go to progress, if given.
	engine::Json waitForJob(engine::Client& client, uint64_t job, std::vector<engine::Json>* progress = nullptr);

	// Starts a job with the command, through client, and holds it at its first job.progress while during runs with the
	// job's id; then checks that the job still runs and was held all along, lets it go on and returns the job once it
	// ended (#1154). So what during does comes before anything the job does after its first job.progress, however the
	// threads are scheduled. Progress events go to progress, if given. The job is held for hold at most: a hold that
	// lapses before during is done fails the test.
	engine::Json whileHeld(engine::Engine& engine, engine::Client& client, const std::string& command,
		engine::Json parameters, const std::function<void(uint64_t job)>& during,
		std::vector<engine::Json>* progress = nullptr, std::chrono::milliseconds hold = std::chrono::minutes(1));
}
