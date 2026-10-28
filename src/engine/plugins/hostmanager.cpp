// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/plugins/hostmanager.h"

#include "engine/engine.h"
#include "platform/log.h"
#include "platform/process.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <set>
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
	}

	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when constructed; only out of memory throws
	HostManager::~HostManager()
	{
		{
			const std::scoped_lock lock(m_monitorMutex);
			m_stopping = true;
		}
		m_monitorWake.notify_all();
		if (m_monitor.joinable())
			m_monitor.join();
		// Remote plugins that outlive the manager keep their host processes; the rest end now, without the lock.
		std::map<std::string, Host> hosts;
		{
			const std::scoped_lock lock(m_shared->mutex);
			hosts.swap(m_shared->hosts);
		}
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

	std::shared_ptr<HostConnection> HostManager::hostFor(Shared& shared, const std::string& group) const
	{
		Host& host = shared.hosts[group];
		if (host.connection && !host.connection->exited() && !host.connection->hung())
			return host.connection;

		// A new process, with the lowest log file number no other process of this manager uses.
		std::set<int> used;
		for (const auto& [name, other] : shared.hosts)
			used.insert(other.log);
		int log = 1;
		while (used.count(log) != 0)
			++log;
		plugin::remote::LaunchOptions options;
		options.executable = m_options.executable.empty() ? defaultExecutable() : m_options.executable;
		options.logFile = "gnubase-pluginhost-" + std::to_string(log) + ".log";
		options.timeout = m_options.launchTimeout;
		try
		{
			host.connection = HostConnection::launch(options);
		}
		catch (...)
		{
			shared.hosts.erase(group);
			throw;
		}
		host.log = log;
		logging::info("Plugin host {} runs the group {}", host.connection->processId(), group);
		return host.connection;
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

	std::unique_ptr<RemotePlugin> HostManager::create(const PluginRef& plugin)
	{
		rt::assertNotRealtime("HostManager::create");
		startMonitor();
		std::unique_ptr<RemotePlugin> remote;
		const std::scoped_lock lock(m_shared->mutex);
		const uint64_t id = m_shared->nextInstance++;
		const std::string group = groupOf(*m_shared, id, plugin);
		const std::shared_ptr<HostConnection> host = hostFor(*m_shared, group);
		remote = std::make_unique<RemotePlugin>(plugin.id, plugin.path, host, m_options.deadlines);
		m_shared->instances[id] = Instance{remote.get(), plugin, group, host, false};
		remote->onDestroy(
			[weak = std::weak_ptr<Shared>(m_shared), id]
			{
				const std::shared_ptr<Shared> shared = weak.lock();
				if (!shared)
					return;
				std::vector<std::shared_ptr<HostConnection>> dropped;
				{
					const std::scoped_lock unregister(shared->mutex);
					shared->instances.erase(id);
					dropped = dropUnused(*shared);
				}
			});
		return remote;
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

	std::vector<std::string> HostManager::setMode(HostingMode mode)
	{
		{
			const std::scoped_lock lock(m_shared->mutex);
			m_shared->mode = mode;
		}
		return reload();
	}

	std::vector<std::string> HostManager::setOverride(const std::string& plugin, std::optional<HostingMode> mode)
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

	std::vector<std::string> HostManager::reload()
	{
		rt::assertNotRealtime("HostManager::reload");
		std::vector<std::string> errors;
		std::vector<uint64_t> ids;
		{
			const std::scoped_lock lock(m_shared->mutex);
			for (const auto& [id, instance] : m_shared->instances)
				ids.push_back(id);
		}
		// Plugins are created, moved and destroyed on this thread only, so the entries stay while the lock is free.
		for (const uint64_t id : ids)
		{
			RemotePlugin* plugin = nullptr;
			std::string group;
			std::shared_ptr<HostConnection> host;
			try
			{
				const std::scoped_lock lock(m_shared->mutex);
				const auto found = m_shared->instances.find(id);
				if (found == m_shared->instances.end() || found->second.plugin->failure() != Failure::None)
					continue;
				group = groupOf(*m_shared, id, found->second.ref);
				if (group == found->second.group)
					continue;
				plugin = found->second.plugin;
				host = hostFor(*m_shared, group);
			}
			catch (const std::exception& error)
			{
				errors.push_back(std::to_string(id) + ": " + error.what());
				continue;
			}
			std::string error;
			if (!plugin->moveTo(host, error))
			{
				errors.push_back(std::to_string(id) + ": " + error);
				logging::warning("Cannot move plugin instance {} to the group {}: {}", id, group, error);
				continue;
			}
			const std::scoped_lock lock(m_shared->mutex);
			Instance& instance = m_shared->instances.at(id);
			instance.group = group;
			instance.host = std::move(host);
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

	void HostManager::onFailure(std::function<void()> callback)
	{
		const std::scoped_lock lock(m_monitorMutex);
		m_onFailure = std::move(callback);
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
		host->kill();
		return true;
	}

	void HostManager::startMonitor()
	{
		const std::scoped_lock lock(m_monitorMutex);
		if (!m_monitor.joinable() && !m_stopping)
			m_monitor = std::thread([this] { monitor(); });
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
					if (instance.host->checkExited())
					{
						report.reason = "crashed";
					}
					else if (failure == Failure::Deadline)
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
			if (found && m_onFailure)
			{
				const std::function<void()> callback = m_onFailure;
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
				"Sets the hosting mode (see pluginhost.get). Plugins whose process changes are moved to their new "
				"process with their state, between two blocks, so they keep sounding the same; plugins that can't "
				"move stay where they are and are listed in errors. Emits pluginhost.changed.",
				Json{{"type", "object"}, {"additionalProperties", false}, {"required", Json::array({"mode"})},
					{"properties", {{"mode", modeSchema(false)}}}},
				statusSchema(), false},
			[this, status](CommandContext& context)
			{
				const std::vector<std::string> errors =
					setMode(*parseHostingMode(context.parameters().at("mode").get<std::string>()));
				Json result = status(errors);
				context.emit("pluginhost.changed", result);
				return result;
			});
		engine.registerCommand(
			{"pluginhost.setOverride", 1,
				"Gives a plugin (its id, e.g. \"clap:com.example.synth\") a hosting mode of its own, e.g. "
				"\"perInstance\" to isolate an unstable plugin, or removes its override with null. Moves the "
				"plugin's instances like pluginhost.setMode. Emits pluginhost.changed.",
				Json{{"type", "object"}, {"additionalProperties", false}, {"required", Json::array({"plugin", "mode"})},
					{"properties", {{"plugin", {{"type", "string"}}}, {"mode", modeSchema(true)}}}},
				statusSchema(), false},
			[this, status](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Json& mode = parameters.at("mode");
				const std::vector<std::string> errors = setOverride(parameters.at("plugin").get<std::string>(),
					mode.is_null() ? std::nullopt : parseHostingMode(mode.get<std::string>()));
				Json result = status(errors);
				context.emit("pluginhost.changed", result);
				return result;
			});
		engine.registerCommand(
			{"pluginhost.check", 1,
				"Reports the plugins that failed since the last check because their host process crashed, missed a "
				"processing deadline (reason \"hung\") or broke the protocol, and emits plugin.failed for each with "
				"its instance, plugin id, process and reason. A failed plugin is bypassed and outputs silence. The "
				"engine runs this on its own as soon as a failure is detected.",
				Json{{"type", "object"}, {"additionalProperties", false}},
				Json::parse(R"({"type": "object", "required": ["failures"], "properties": {
					"failures": {"type": "array", "items": {"type": "object"}}}})"),
				false},
			[this](CommandContext& context)
			{
				Json failures = Json::array();
				for (const PluginFailure& failure : takeFailures())
				{
					Json entry = {{"instance", failure.instance}, {"plugin", failure.plugin},
						{"process", failure.process}, {"reason", failure.reason}};
					context.emit("plugin.failed", entry);
					failures.push_back(std::move(entry));
				}
				return Json{{"failures", std::move(failures)}};
			});
	}
}
