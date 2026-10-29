// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/jobs/jobs.h"

#include "engine/engine.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <future>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		// An absolute, normalized path in lower case, so that folders that differ only in case count as the same:
		// they are on some file systems, and a false match only refuses a second job.
		std::string comparable(const std::filesystem::path& folder)
		{
			std::error_code error;
			std::string text = std::filesystem::absolute(folder, error).lexically_normal().generic_u8string();
			while (text.size() > 1 && text.back() == '/')
				text.pop_back();
			std::transform(text.begin(), text.end(), text.begin(),
				[](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
			return text;
		}

		// Whether inner is outer or inside it; both comparable.
		bool within(const std::string& inner, const std::string& outer)
		{
			return inner.compare(0, outer.size(), outer) == 0 &&
				(inner.size() == outer.size() || inner[outer.size()] == '/' || outer.back() == '/');
		}
	}

	struct Jobs::Job
	{
		JobId id = 0;
		std::string kind;
		std::atomic<bool> cancel{false};
		std::thread thread;
		std::string folder; // where it writes, as comparable() makes it; empty for nowhere in particular

		// Guarded by Jobs::m_mutex.
		std::string state = "running"; // then "done", "failed" or "cancelled"
		double progress = 0.0;
		std::string detail;
		Json result;
		std::string error;
		double announced = -1.0; // the progress last announced with job.progress
	};

	class Jobs::Context : public JobContext
	{
	public:
		Context(Jobs& jobs, Job& job) : m_jobs(jobs), m_job(job) {}

		bool cancelled() const override { return m_job.cancel.load(std::memory_order_relaxed); }

		void progress(double fraction, const std::string& detail) override
		{
			Json event;
			{
				const std::scoped_lock lock(m_jobs.m_mutex);
				m_job.progress = std::clamp(fraction, m_job.progress, 1.0); // never goes back
				if (m_job.progress < m_job.announced + 0.01 && detail == m_job.detail)
					return;
				m_job.detail = detail;
				m_job.announced = m_job.progress;
				event = {
					{"job", m_job.id}, {"kind", m_job.kind}, {"progress", m_job.progress}, {"detail", m_job.detail}};
			}
			m_jobs.m_engine.broadcast("job.progress", std::move(event));
		}

		Json command(std::string name, Json parameters) override
		{
			auto promise = std::make_shared<std::promise<Result>>();
			std::future<Result> future = promise->get_future();
			m_jobs.m_engine.post(std::move(name), std::move(parameters),
				[promise](Result result) { promise->set_value(std::move(result)); });
			Result result = future.get();
			if (!result.ok())
				throw CommandError(result.error);
			return std::move(result.value);
		}

	private:
		Jobs& m_jobs;
		Job& m_job;
	};

	Jobs::Jobs(Engine& engine) : m_engine(engine)
	{
	}

	Jobs::~Jobs()
	{
		{
			const std::scoped_lock lock(m_mutex);
			for (const auto& entry : m_jobs)
				entry.second->cancel = true;
		}
		// Only start adds jobs, and the engine's command thread, which runs it, has ended. So the map holds still,
		// while the jobs, which lock m_mutex to finish, end.
		for (const auto& entry : m_jobs)
			if (entry.second->thread.joinable())
				entry.second->thread.join();
	}

	Json Jobs::toJson(const Job& job)
	{
		return {{"id", job.id}, {"kind", job.kind}, {"state", job.state}, {"progress", job.progress},
			{"detail", job.detail}, {"result", job.result},
			{"error", job.error.empty() ? Json(nullptr) : Json(job.error)}};
	}

	void Jobs::finish(Job& job, const char* state, Json result, std::string error)
	{
		Json event;
		{
			const std::scoped_lock lock(m_mutex);
			job.state = state;
			if (job.state == "done")
				job.progress = 1.0;
			job.result = std::move(result);
			job.error = std::move(error);
			event = {{"job", toJson(job)}};
		}
		m_engine.broadcast("job.finished", std::move(event));
	}

	Json Jobs::schema()
	{
		return {{"type", "object"},
			{"properties",
				{{"id", {{"type", "integer"}}}, {"kind", {{"type", "string"}}},
					{"state", {{"type", "string"}, {"enum", {"running", "done", "failed", "cancelled"}}}},
					{"progress", {{"type", "number"}}}, {"detail", {{"type", "string"}}},
					{"result", {{"type", {"object", "null"}}}}, {"error", {{"type", {"string", "null"}}}}}},
			{"required", {"id", "kind", "state", "progress", "detail", "result", "error"}}};
	}

	bool Jobs::writesInto(const std::filesystem::path& folder) const
	{
		const std::string wanted = comparable(folder);
		const std::scoped_lock lock(m_mutex);
		return std::any_of(m_jobs.begin(), m_jobs.end(),
			[&wanted](const auto& entry)
			{
				const Job& job = *entry.second;
				if (job.state != "running" || job.folder.empty())
					return false;
				return within(wanted, job.folder) || within(job.folder, wanted);
			});
	}

	Json Jobs::start(std::string kind, Work work, const std::filesystem::path& folder)
	{
		// Threads of finished jobs end right after announcing it; joined here, they don't pile up.
		std::vector<std::shared_ptr<Job>> finished;
		auto job = std::make_shared<Job>();
		Json started;
		{
			const std::scoped_lock lock(m_mutex);
			for (const auto& entry : m_jobs)
				if (entry.second->state != "running" && entry.second->thread.joinable())
					finished.push_back(entry.second);
			job->id = m_nextId++;
			job->kind = std::move(kind);
			if (!folder.empty())
				job->folder = comparable(folder);
			m_jobs[job->id] = job;
			started = toJson(*job);
		}
		for (const auto& done : finished)
			done->thread.join();

		// Announced before the thread starts, so that no job.progress or job.finished can come first.
		m_engine.broadcast("job.started", {{"job", started}});
		try
		{
			job->thread = std::thread(
				[this, job, work = std::move(work)]
				{
					Context context(*this, *job);
					try
					{
						Json result = work(context);
						finish(*job, "done", std::move(result), {});
					}
					catch (const JobCancelled&)
					{
						finish(*job, "cancelled", nullptr, {});
					}
					catch (const std::exception& error)
					{
						finish(*job, "failed", nullptr, error.what());
					}
				});
		}
		catch (const std::system_error& error)
		{
			finish(*job, "failed", nullptr, error.what());
		}
		return started;
	}

	void Jobs::registerCommands(Engine& engine)
	{
		const Json jobSchema = schema();
		const auto jobParameter = []
		{
			return Json{{"type", "object"}, {"properties", {{"job", {{"type", "integer"}}}}}, {"required", {"job"}},
				{"additionalProperties", false}};
		};
		const auto find = [this](const Json& parameters)
		{
			const JobId id = parameters.at("job").get<JobId>();
			const auto found = m_jobs.find(id);
			if (found == m_jobs.end())
				throw CommandError("no job " + std::to_string(id));
			return found->second;
		};

		engine.registerCommand(
			{"job.list", 1,
				"Every background job since the engine started, such as backups: id, kind, state (running, done, "
				"failed or cancelled), progress from 0 to 1, what it is doing, and its result or error once it ended.",
				Json{{"type", "object"}, {"additionalProperties", false}},
				{{"type", "object"}, {"properties", {{"jobs", {{"type", "array"}, {"items", jobSchema}}}}},
					{"required", {"jobs"}}},
				true},
			[this](CommandContext&)
			{
				const std::scoped_lock lock(m_mutex);
				Json list = Json::array();
				for (const auto& entry : m_jobs)
					list.push_back(toJson(*entry.second));
				return Json{{"jobs", std::move(list)}};
			});
		engine.registerCommand(
			{"job.get", 1, "A background job, as job.list shows it.", jobParameter(),
				{{"type", "object"}, {"properties", {{"job", jobSchema}}}, {"required", {"job"}}}, true},
			[this, find](CommandContext& context)
			{
				const std::scoped_lock lock(m_mutex);
				return Json{{"job", toJson(*find(context.parameters()))}};
			});
		engine.registerCommand(
			{"job.cancel", 1,
				"Asks a running job to stop. It ends soon after as cancelled, with a job.finished event, and cleans "
				"up what it made; a job that has ended stays as it is.",
				jobParameter(), {{"type", "object"}, {"properties", {{"job", jobSchema}}}, {"required", {"job"}}},
				false},
			[this, find](CommandContext& context)
			{
				const std::scoped_lock lock(m_mutex);
				const std::shared_ptr<Job> job = find(context.parameters());
				job->cancel = true;
				return Json{{"job", toJson(*job)}};
			});
	}
}
