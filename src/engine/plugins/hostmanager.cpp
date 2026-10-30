// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/plugins/hostmanager.h"

#include "engine/engine.h"
#include "platform/log.h"
#include "platform/process.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace gnubase::engine::plugins
{
	namespace
	{
		using plugin::remote::Failure;
		using plugin::remote::HostConnection;
		using plugin::remote::RemotePlugin;

		constexpr HostingMode g_modes[] = {
			HostingMode::Shared, HostingMode::PerVendor, HostingMode::PerPlugin, HostingMode::PerInstance};

		std::filesystem::path defaultExecutable()
		{
#ifdef _WIN32
			const char* name = "gnubase-pluginhost.exe";
#else
			const char* name = "gnubase-pluginhost";
#endif
			return platform::currentExecutablePath().parent_path() / name;
		}

		const char* stateOf(const HostConnection& host)
		{
			if (host.exited())
				return "exited";
			return host.hung() ? "hung" : "running";
		}

		Json modeSchema(bool nullable)
		{
			Json names = Json::array();
			for (const HostingMode mode : g_modes)
				names.push_back(toString(mode));
			if (nullable)
				names.push_back(nullptr);
			return {{"type", nullable ? Json{"string", "null"} : Json("string")}, {"enum", names}};
		}

		Json statusSchema()
		{
			return Json::parse(R"({"type": "object", "required": ["mode", "defaultMode", "overrides", "hosts"],
				"properties": {"mode": {"type": "string"}, "defaultMode": {"type": "string"},
					"overrides": {"type": "object"}, "hosts": {"type": "array", "items": {"type": "object",
						"required": ["process", "group", "state", "instances"], "properties": {
							"process": {"type": "integer"}, "group": {"type": "string"}, "state": {"type": "string"},
							"instances": {"type": "array"}}}},
					"errors": {"type": "array", "items": {"type": "string"}}}})");
		}
	}

	const char* toString(HostingMode mode)
	{
		switch (mode)
		{
		case HostingMode::Shared:
			return "shared";
		case HostingMode::PerVendor:
			return "perVendor";
		case HostingMode::PerPlugin:
			return "perPlugin";
		case HostingMode::PerInstance:
			return "perInstance";
		}
		return "shared";
	}

	std::optional<HostingMode> parseHostingMode(std::string_view name)
	{
		for (const HostingMode mode : g_modes)
		{
			if (name == toString(mode))
				return mode;
		}
		return std::nullopt;
	}

	HostManager::HostManager() : HostManager(Options())
	{
	}

	HostManager::HostManager(Options options) : m_options(std::move(options)), m_shared(std::make_shared<Shared>())
	{
		m_shared->executable = m_options.executable;
		m_jobThread = std::thread([this] { jobs(); });
		m_jobThreadId = m_jobThread.get_id();
		m_monitor = std::thread([this] { monitor(); });
	}

	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when constructed; only out of memory throws
	HostManager::~HostManager()
	{
		std::deque<std::function<void()>> dropped;
		{
			const std::scoped_lock lock(m_shared->jobMutex);
			m_shared->stopping = true;
			dropped.swap(m_shared->jobs);
		}
		m_shared->jobWake.notify_all();
		m_jobThread.join();
		dropped.clear(); // their futures report broken promises
		{
			const std::scoped_lock lock(m_monitorMutex);
			m_stopping = true;
		}
		m_monitorWake.notify_all();
		m_monitor.join();

		// Remote plugins that outlive the manager fail: on Linux their processes ended with the job thread. The rest of
		// the processes end now, without the lock.
		std::map<std::string, Host> hosts;
		{
			const std::scoped_lock lock(m_shared->mutex);
			m_shared->closed = true;
			for (auto& [id, instance] : m_shared->instances)
				instance.plugin->fail(Failure::HostGone);
			hosts.swap(m_shared->hosts);
		}
	}

	void HostManager::setExecutable(std::filesystem::path executable)
	{
		const std::scoped_lock lock(m_shared->mutex);
		m_shared->executable = std::move(executable);
	}

	void HostManager::post(Shared& shared, std::function<void()> job)
	{
		{
			const std::scoped_lock lock(shared.jobMutex);
			if (shared.stopping)
				return; // dropped: its promise breaks
			shared.jobs.push_back(std::move(job));
		}
		shared.jobWake.notify_one();
	}

	void HostManager::jobs()
	{
		for (;;)
		{
			std::function<void()> job;
			{
				std::unique_lock lock(m_shared->jobMutex);
				m_shared->jobWake.wait(lock, [this] { return m_shared->stopping || !m_shared->jobs.empty(); });
				if (m_shared->stopping)
					return;
				job = std::move(m_shared->jobs.front());
				m_shared->jobs.pop_front();
			}
			try
			{
				job();
			}
			catch (const std::exception& error)
			{
				logging::error("A plugin job failed: {}", std::string_view(error.what()));
			}
		}
	}

	std::future<void> HostManager::run(std::function<void()> job)
	{
		auto promise = std::make_shared<std::promise<void>>();
		std::future<void> result = promise->get_future();
		post(*m_shared,
			[promise, job = std::move(job)]
			{
				try
				{
					job();
					promise->set_value();
				}
				catch (...)
				{
					promise->set_exception(std::current_exception());
				}
			});
		return result;
	}

	std::string HostManager::groupOf(const Shared& shared, uint64_t instance, const PluginRef& plugin)
	{
		const auto found = shared.overrides.find(plugin.id);
		switch (found != shared.overrides.end() ? found->second : shared.mode)
		{
		case HostingMode::Shared:
			return "shared";
		case HostingMode::PerVendor:
			return "vendor:" + (plugin.vendor.empty() ? std::string("unknown") : plugin.vendor);
		case HostingMode::PerPlugin:
			return "plugin:" + plugin.id;
		case HostingMode::PerInstance:
			return "instance:" + std::to_string(instance);
		}
		return "shared";
	}

	std::shared_ptr<HostConnection> HostManager::hostFor(const std::string& group)
	{
		// [job thread] Only this thread starts processes, so none of a group starts meanwhile.
		int log = 1;
		plugin::remote::LaunchOptions options;
		{
			const std::scoped_lock lock(m_shared->mutex);
			options.executable = m_shared->executable.empty() ? defaultExecutable() : m_shared->executable;
			if (const auto found = m_shared->hosts.find(group); found != m_shared->hosts.end())
			{
				const std::shared_ptr<HostConnection>& connection = found->second.connection;
				if (connection && !connection->exited() && !connection->hung() && !connection->unresponsive())
					return connection;
			}
			// A new process, with the lowest log file number no other process of this manager uses.
			std::set<int> used;
			for (const auto& [name, other] : m_shared->hosts)
				used.insert(other.log);
			while (used.count(log) != 0)
				++log;
		}
		options.logFile = "gnubase-pluginhost-" + std::to_string(log) + ".log";
		// What a plugin writes to standard error, e.g. a crash message, goes next to the log, from this launch on.
		std::error_code error;
		const std::filesystem::path directory = logging::defaultDirectory();
		std::filesystem::create_directories(directory, error);
		options.errorOutput = directory / ("gnubase-pluginhost-" + std::to_string(log) + "-stderr.log");
		std::ofstream(options.errorOutput, std::ios::trunc).close();
		options.timeout = m_options.launchTimeout;
		std::shared_ptr<HostConnection> connection = HostConnection::launch(options);
		std::shared_ptr<HostConnection> replaced; // a hung or exited one: its failed plugins keep it
		{
			const std::scoped_lock lock(m_shared->mutex);
			Host& host = m_shared->hosts[group];
			replaced = std::move(host.connection);
			host.connection = connection;
			host.log = log;
		}
		logging::info("Plugin host {} runs the group {}", connection->processId(), group);
		return connection;
	}

	std::vector<std::shared_ptr<HostConnection>> HostManager::dropUnused(Shared& shared)
	{
		std::vector<std::shared_ptr<HostConnection>> dropped;
		for (auto it = shared.hosts.begin(); it != shared.hosts.end();)
		{
			const bool used = std::any_of(shared.instances.begin(), shared.instances.end(),
				[&](const auto& entry) { return entry.second.host == it->second.connection; });
			if (used)
			{
				++it;
			}
			else
			{
				dropped.push_back(std::move(it->second.connection));
				it = shared.hosts.erase(it);
			}
		}
		return dropped; // the caller destroys them without the lock: closing waits for the process
	}

	std::unique_ptr<RemotePlugin> HostManager::make(const PluginRef& plugin, plugin::Host* engineHost)
	{
		// [job thread]
		uint64_t id = 0;
		std::string group;
		{
			const std::scoped_lock lock(m_shared->mutex);
			id = m_shared->nextInstance++;
			group = groupOf(*m_shared, id, plugin);
		}
		const std::shared_ptr<HostConnection> host = hostFor(group);
		auto remote = std::make_unique<RemotePlugin>(plugin.id, plugin.path, host, m_options.deadlines, engineHost);
		{
			const std::scoped_lock lock(m_shared->mutex);
			m_shared->instances[id] = Instance{remote.get(), plugin, group, host, false, false};
		}
		const std::weak_ptr<Shared> weak = m_shared;
		remote->onDestroy(
			[weak, id]
			{
				const std::shared_ptr<Shared> shared = weak.lock();
				if (!shared)
					return;
				std::vector<std::shared_ptr<HostConnection>> dropped;
				{
					// A job that uses the plugin finishes first.
					std::unique_lock lock(shared->mutex);
					shared->idle.wait(lock,
						[&]
						{
							const auto found = shared->instances.find(id);
							return found == shared->instances.end() || !found->second.busy;
						});
					shared->instances.erase(id);
					if (!shared->closed)
						dropped = dropUnused(*shared);
				}
			});
		// What the plugin's notifications say changed is fetched on the job thread.
		remote->onNotification(
			[weak, id]
			{
				if (const std::shared_ptr<Shared> shared = weak.lock())
				{
					post(*shared,
						[weak, id]
						{
							if (const std::shared_ptr<Shared> current = weak.lock())
								refresh(*current, id);
						});
				}
			});
		return remote;
	}

	void HostManager::refresh(Shared& shared, uint64_t id)
	{
		RemotePlugin* plugin = nullptr;
		{
			const std::scoped_lock lock(shared.mutex);
			const auto found = shared.instances.find(id);
			if (found == shared.instances.end())
				return;
			found->second.busy = true;
			plugin = found->second.plugin;
		}
		try
		{
			plugin->refresh();
		}
		catch (const std::exception& error)
		{
			logging::error("Plugin instance {} could not refresh: {}", id, std::string_view(error.what()));
		}
		{
			const std::scoped_lock lock(shared.mutex);
			shared.instances.at(id).busy = false;
		}
		shared.idle.notify_all();
	}

	std::future<std::unique_ptr<RemotePlugin>> HostManager::load(const PluginRef& plugin, plugin::Host* engineHost)
	{
		auto promise = std::make_shared<std::promise<std::unique_ptr<RemotePlugin>>>();
		std::future<std::unique_ptr<RemotePlugin>> result = promise->get_future();
		auto ref = std::make_shared<const PluginRef>(plugin); // so copying the job never throws
		post(*m_shared,
			[this, promise, ref, engineHost]
			{
				try
				{
					std::unique_ptr<RemotePlugin> remote = make(*ref, engineHost);
					if (!remote->init())
						throw std::runtime_error("cannot create the plugin " + ref->id);
					promise->set_value(std::move(remote));
				}
				catch (...)
				{
					promise->set_exception(std::current_exception());
				}
			});
		return result;
	}

	std::unique_ptr<RemotePlugin> HostManager::create(const PluginRef& plugin, plugin::Host* engineHost)
	{
		rt::assertNotRealtime("HostManager::create");
		if (std::this_thread::get_id() == m_jobThreadId)
			throw std::logic_error("HostManager::create waits for the job thread: use make there");
		auto promise = std::make_shared<std::promise<std::unique_ptr<RemotePlugin>>>();
		std::future<std::unique_ptr<RemotePlugin>> result = promise->get_future();
		auto ref = std::make_shared<const PluginRef>(plugin); // so copying the job never throws
		post(*m_shared,
			[this, promise, ref, engineHost]
			{
				try
				{
					promise->set_value(make(*ref, engineHost));
				}
				catch (...)
				{
					promise->set_exception(std::current_exception());
				}
			});
		return result.get();
	}

	HostingMode HostManager::mode() const
	{
		const std::scoped_lock lock(m_shared->mutex);
		return m_shared->mode;
	}

	std::map<std::string, HostingMode> HostManager::overrides() const
	{
		const std::scoped_lock lock(m_shared->mutex);
		return m_shared->overrides;
	}

	std::future<std::vector<std::string>> HostManager::setMode(HostingMode mode)
	{
		{
			const std::scoped_lock lock(m_shared->mutex);
			m_shared->mode = mode;
		}
		return reload();
	}

	std::future<std::vector<std::string>> HostManager::setOverride(
		const std::string& plugin, std::optional<HostingMode> mode)
	{
		{
			const std::scoped_lock lock(m_shared->mutex);
			if (mode)
				m_shared->overrides[plugin] = *mode;
			else
				m_shared->overrides.erase(plugin);
		}
		return reload();
	}

	std::future<std::vector<std::string>> HostManager::reload()
	{
		auto promise = std::make_shared<std::promise<std::vector<std::string>>>();
		std::future<std::vector<std::string>> result = promise->get_future();
		post(*m_shared,
			[this, promise]
			{
				try
				{
					std::vector<std::string> errors = moveAll();
					{
						const std::scoped_lock lock(m_shared->mutex);
						m_shared->moves.push_back(errors);
					}
					report();
					promise->set_value(std::move(errors));
				}
				catch (...)
				{
					promise->set_exception(std::current_exception());
				}
			});
		return result;
	}

	std::vector<std::string> HostManager::moveAll()
	{
		// [job thread]
		std::vector<std::string> errors;
		std::vector<uint64_t> ids;
		{
			const std::scoped_lock lock(m_shared->mutex);
			for (const auto& [id, instance] : m_shared->instances)
				ids.push_back(id);
		}
		for (const uint64_t id : ids)
		{
			RemotePlugin* plugin = nullptr;
			std::string group;
			{
				const std::scoped_lock lock(m_shared->mutex);
				const auto found = m_shared->instances.find(id);
				if (found == m_shared->instances.end() || found->second.plugin->failure() != Failure::None)
					continue;
				group = groupOf(*m_shared, id, found->second.ref);
				if (group == found->second.group)
					continue;
				// The plugin stays while it moves: its destruction waits.
				found->second.busy = true;
				plugin = found->second.plugin;
			}
			std::shared_ptr<HostConnection> host;
			std::string error;
			bool moved = false;
			try
			{
				host = hostFor(group);
				moved = plugin->moveTo(host, error);
			}
			catch (const std::exception& exception)
			{
				error = exception.what();
			}
			if (!moved)
			{
				errors.push_back(std::to_string(id) + ": " + error);
				logging::warning("Cannot move plugin instance {} to the group {}: {}", id, group, error);
			}
			{
				const std::scoped_lock lock(m_shared->mutex);
				Instance& instance = m_shared->instances.at(id);
				if (moved)
				{
					instance.group = group;
					instance.host = std::move(host);
				}
				instance.busy = false;
			}
			m_shared->idle.notify_all();
		}
		std::vector<std::shared_ptr<HostConnection>> dropped;
		{
			const std::scoped_lock lock(m_shared->mutex);
			dropped = dropUnused(*m_shared);
		}
		return errors;
	}

	std::vector<HostManager::HostStatus> HostManager::hosts() const
	{
		const std::scoped_lock lock(m_shared->mutex);
		std::map<HostConnection*, HostStatus> hosts;
		for (const auto& [group, host] : m_shared->hosts)
		{
			HostStatus& status = hosts[host.connection.get()];
			status.process = host.connection->processId();
			status.group = group;
			status.state = stateOf(*host.connection);
		}
		for (const auto& [id, instance] : m_shared->instances)
		{
			HostStatus& status = hosts[instance.host.get()];
			status.process = instance.host->processId();
			status.group = instance.group;
			status.state = stateOf(*instance.host);
			status.instances.push_back({id, instance.ref.id, instance.plugin->failure() != Failure::None});
		}
		std::vector<HostStatus> list;
		list.reserve(hosts.size());
		for (auto& [connection, status] : hosts)
			list.push_back(std::move(status));
		std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.process < b.process; });
		return list;
	}

	uint64_t HostManager::instanceOf(const RemotePlugin& plugin) const
	{
		const std::scoped_lock lock(m_shared->mutex);
		for (const auto& [id, instance] : m_shared->instances)
		{
			if (instance.plugin == &plugin)
				return id;
		}
		return 0;
	}

	std::vector<PluginFailure> HostManager::takeFailures()
	{
		const std::scoped_lock lock(m_shared->mutex);
		return std::exchange(m_shared->failures, {});
	}

	std::vector<std::vector<std::string>> HostManager::takeMoves()
	{
		const std::scoped_lock lock(m_shared->mutex);
		return std::exchange(m_shared->moves, {});
	}

	void HostManager::onReport(std::function<void()> callback)
	{
		const std::scoped_lock lock(m_monitorMutex);
		m_onReport = std::move(callback);
	}

	void HostManager::report()
	{
		std::function<void()> callback;
		{
			const std::scoped_lock lock(m_monitorMutex);
			callback = m_onReport;
		}
		if (callback)
			callback();
	}

	bool HostManager::kill(uint32_t process)
	{
		std::shared_ptr<HostConnection> host;
		{
			const std::scoped_lock lock(m_shared->mutex);
			for (const auto& [id, instance] : m_shared->instances)
			{
				if (instance.host->processId() == process)
					host = instance.host;
			}
			for (const auto& [group, candidate] : m_shared->hosts)
			{
				if (candidate.connection->processId() == process)
					host = candidate.connection;
			}
		}
		if (!host)
			return false;
		host->kill(); // without a lock: the monitor and the plugin control thread go on meanwhile
		return true;
	}

	void HostManager::monitor()
	{
		std::unique_lock monitorLock(m_monitorMutex);
		while (!m_stopping)
		{
			m_monitorWake.wait_for(monitorLock, m_options.monitorInterval);
			if (m_stopping)
				break;
			bool found = false;
			{
				const std::scoped_lock lock(m_shared->mutex);
				for (auto& [id, instance] : m_shared->instances)
				{
					const Failure failure = instance.plugin->failure();
					if (instance.reported || (failure == Failure::None && !instance.host->exited()))
						continue;
					// A plugin whose host is gone fails at once, without waiting for its next deadline.
					instance.plugin->fail(Failure::HostGone);
					PluginFailure report{id, instance.ref.id, instance.host->processId(), "crashed"};
					if (failure == Failure::Deadline)
					{
						report.reason = "hung";
						instance.host->markHung();
					}
					else if (failure == Failure::Protocol)
					{
						report.reason = "protocol";
					}
					logging::error("Plugin instance {} ({}) failed: its host process {} {}", id, report.plugin,
						report.process, report.reason);
					instance.reported = true;
					m_shared->failures.push_back(std::move(report));
					found = true;
				}
			}
			if (found && m_onReport)
			{
				const std::function<void()> callback = m_onReport;
				monitorLock.unlock();
				callback();
				monitorLock.lock();
			}
		}
	}

	void HostManager::registerCommands(Engine& engine)
	{
		const auto status = [this](const std::vector<std::string>& errors)
		{
			Json overrides = Json::object();
			for (const auto& [plugin, mode] : this->overrides())
				overrides[plugin] = toString(mode);
			Json hostList = Json::array();
			for (const HostStatus& host : hosts())
			{
				Json instances = Json::array();
				for (const InstanceStatus& instance : host.instances)
					instances.push_back(
						{{"instance", instance.instance}, {"plugin", instance.plugin}, {"failed", instance.failed}});
				hostList.push_back({{"process", host.process}, {"group", host.group}, {"state", host.state},
					{"instances", std::move(instances)}});
			}
			Json result = {{"mode", toString(mode())}, {"defaultMode", toString(g_defaultHostingMode)},
				{"overrides", std::move(overrides)}, {"hosts", std::move(hostList)}};
			if (!errors.empty())
				result["errors"] = errors;
			return result;
		};

		engine.registerCommand(
			{"pluginhost.get", 1,
				"How plugins are grouped into plugin host processes, and the processes. mode: the hosting mode, "
				"\"shared\" (one process for every plugin), \"perVendor\", \"perPlugin\" or \"perInstance\"; "
				"defaultMode: the mode GNUbase starts with; overrides: plugin ids with a mode of their own. hosts: "
				"each process with its id (process), its group (\"shared\", \"vendor:<vendor>\", \"plugin:<id>\" or "
				"\"instance:<instance>\"), its state (\"running\", \"hung\" or \"exited\") and its plugin instances, "
				"whether they failed.",
				Json{{"type", "object"}, {"additionalProperties", false}}, statusSchema(), true},
			[status](CommandContext&) { return status({}); });
		engine.registerCommand(
			{"pluginhost.setMode", 1,
				"Sets the hosting mode (see pluginhost.get) and returns at once. Plugins whose process changes then "
				"move "
				"to their new process in the background, with their state, between two blocks, so they keep sounding "
				"the same. When they did, pluginhost.changed is emitted with the new status, and with the plugins "
				"that could not move and stay where they are in errors.",
				Json{{"type", "object"}, {"additionalProperties", false}, {"required", Json::array({"mode"})},
					{"properties", {{"mode", modeSchema(false)}}}},
				statusSchema(), false},
			[this, status](CommandContext& context)
			{
				setMode(*parseHostingMode(context.parameters().at("mode").get<std::string>()));
				return status({});
			});
		engine.registerCommand(
			{"pluginhost.setOverride", 1,
				"Gives a plugin (its id, e.g. \"clap:com.example.synth\") a hosting mode of its own, e.g. "
				"\"perInstance\" to isolate an unstable plugin, or removes its override with null. Moves the "
				"plugin's instances like pluginhost.setMode, which emits pluginhost.changed when they moved.",
				Json{{"type", "object"}, {"additionalProperties", false}, {"required", Json::array({"plugin", "mode"})},
					{"properties", {{"plugin", {{"type", "string"}}}, {"mode", modeSchema(true)}}}},
				statusSchema(), false},
			[this, status](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Json& mode = parameters.at("mode");
				setOverride(parameters.at("plugin").get<std::string>(),
					mode.is_null() ? std::nullopt : parseHostingMode(mode.get<std::string>()));
				return status({});
			});
		engine.registerCommand(
			{"pluginhost.check", 1,
				"Reports the plugins that failed since the last check because their host process crashed, missed a "
				"processing deadline (reason \"hung\") or broke the protocol, and emits plugin.failed for each with "
				"its instance, plugin id, process and reason. A failed plugin is bypassed and outputs silence. Also "
				"emits pluginhost.changed for each hosting mode change whose moves are done. The engine runs this on "
				"its own as soon as a failure is detected or the moves are done.",
				Json{{"type", "object"}, {"additionalProperties", false}},
				Json::parse(R"({"type": "object", "required": ["failures"], "properties": {
					"failures": {"type": "array", "items": {"type": "object"}}}})"),
				false},
			[this, status](CommandContext& context)
			{
				Json failures = Json::array();
				for (const PluginFailure& failure : takeFailures())
				{
					Json entry = {{"instance", failure.instance}, {"plugin", failure.plugin},
						{"process", failure.process}, {"reason", failure.reason}};
					context.emit("plugin.failed", entry);
					failures.push_back(std::move(entry));
				}
				for (const std::vector<std::string>& errors : takeMoves())
					context.emit("pluginhost.changed", status(errors));
				return Json{{"failures", std::move(failures)}};
			});
	}
}
