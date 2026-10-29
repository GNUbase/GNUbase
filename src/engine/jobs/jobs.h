// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// Background jobs: long work such as backups, which runs on a thread of its own so that neither the command thread
// nor an audio thread waits for it. A command starts a job and returns at once; the job reports its progress with
// job.progress events and its outcome with job.finished, and job.cancel stops it.
namespace gnubase::engine
{
	class Engine;

	using JobId = uint64_t;

	// Thrown by JobContext::checkCancelled to end a cancelled job.
	class JobCancelled : public std::exception
	{
	public:
		const char* what() const noexcept override { return "cancelled"; }
	};

	// What a job's work gets while it runs on the job's thread.
	class JobContext
	{
	public:
		virtual ~JobContext() = default;
		JobContext() = default;
		JobContext(const JobContext&) = delete;
		JobContext& operator=(const JobContext&) = delete;

		virtual bool cancelled() const = 0;

		// Throws JobCancelled once job.cancel asked the job to stop.
		void checkCancelled() const
		{
			if (cancelled())
				throw JobCancelled();
		}

		// How far the work is, from 0 to 1, and what it is doing, e.g. the file it copies. Announced with a
		// job.progress event when it moved on by a percent or the detail changed.
		virtual void progress(double fraction, const std::string& detail) = 0;

		// Runs a command on the command thread, after the ones already waiting, and returns its result: how a job
		// changes the project. Its events reach every client before job.finished. Throws CommandError if it fails or
		// the engine stops first. It waits for the command even if the job is cancelled meanwhile.
		virtual Json command(std::string name, Json parameters) = 0;
	};

	// The engine's jobs. Each runs on a thread of its own at normal priority, never on an audio thread.
	class Jobs
	{
	public:
		explicit Jobs(Engine& engine);

		// Cancels the running jobs and waits for them.
		~Jobs();
		Jobs(const Jobs&) = delete;
		Jobs& operator=(const Jobs&) = delete;

		// Returns what the job ends with: its result in job.get and job.finished. A JobCancelled exception ends the
		// job as cancelled, any other exception as failed with the exception's message.
		using Work = std::function<Json(JobContext& context)>;

		// Starts work as a job of the given kind, e.g. "project.backup", announces it with job.started and returns
		// the job as job.get shows it. folder, if given, is where the job writes; see writesInto. Only the command
		// thread starts jobs.
		Json start(std::string kind, Work work, const std::filesystem::path& folder = {});

		// Whether a running job writes into folder, a folder inside it or a folder around it.
		bool writesInto(const std::filesystem::path& folder) const;

		// The JSON schema of a job as job.get shows it, for the results of commands that start one.
		static Json schema();

		// job.get, job.list and job.cancel.
		void registerCommands(Engine& engine);

	private:
		struct Job;
		class Context;

		static Json toJson(const Job& job);
		void finish(Job& job, const char* state, Json result, std::string error);

		Engine& m_engine;
		mutable std::mutex m_mutex; // guards the jobs and their states
		std::map<JobId, std::shared_ptr<Job>> m_jobs;
		JobId m_nextId = 1;
	};
}
