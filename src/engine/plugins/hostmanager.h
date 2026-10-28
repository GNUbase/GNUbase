// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plugin/remote/hostconnection.h"
#include "plugin/remote/proxy.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gnubase::engine
{
	class Engine;
}

// Plugins that run in plugin host processes (docs/architecture.md, "Plugin hosting and IPC").
namespace gnubase::engine::plugins
{
	// How plugins are grouped into host processes: a user setting, with an override per plugin (e.g. to isolate an
	// unstable one). A crash takes down the plugins of its process only, while each process costs memory and a
	// little time per block.
	enum class HostingMode : uint8_t
	{
		Shared,      // every plugin in one host process
		PerVendor,   // one process per plugin vendor
		PerPlugin,   // one process per plugin, shared by its instances
		PerInstance, // one process per plugin instance
	};

	// "shared", "perVendor", "perPlugin", "perInstance".
	const char* toString(HostingMode mode);
	std::optional<HostingMode> parseHostingMode(std::string_view name);

	// Until the IPC budget benchmark (#76) decides: one process per vendor, so a crash takes down one vendor's plugins
	// only, while the number of processes stays small.
	inline constexpr HostingMode g_defaultHostingMode = HostingMode::PerVendor;

	// What the engine knows of a plugin before it is created: its id, its file and its vendor (from the plugin scan,
	// #66).
	struct PluginRef
	{
		std::string id; // "<format>:<format id>"
		std::string path;
		std::string vendor;
	};

	// A plugin that stopped working because its host process crashed, hung or broke the protocol. It is bypassed and
	// outputs silence; reloading it is #65.
	struct PluginFailure
	{
		uint64_t instance = 0;
		std::string plugin;
		uint32_t process = 0;
		std::string reason; // "crashed", "hung" or "protocol"
	};

	// The engine's host connection manager. It starts plugin host processes and decides which process runs a plugin,
	// by the hosting mode; when the mode changes, it moves the affected plugins to their new process with their
	// state. A monitor thread watches the remote plugins and their processes and reports failures (onFailure), which
	// the engine announces as plugin.failed events. Plugins are created and moved on the command thread.
	class HostManager
	{
	public:
		struct Options
		{
			std::filesystem::path executable; // gnubase-pluginhost; empty: the one next to the running program
			plugin::remote::Deadlines deadlines;
			std::chrono::milliseconds launchTimeout{10000};
			std::chrono::milliseconds monitorInterval{20};
		};

		HostManager();
		explicit HostManager(Options options);
		~HostManager();
		HostManager(const HostManager&) = delete;
		HostManager& operator=(const HostManager&) = delete;

		// A remote plugin in the host process the hosting mode picks for it, started if needed; init() creates the
		// instance there. Throws plugin::remote::LaunchError if the process can't be started.
		std::unique_ptr<plugin::remote::RemotePlugin> create(const PluginRef& plugin);

		HostingMode mode() const;
		std::map<std::string, HostingMode> overrides() const;

		// Change the hosting mode, or a plugin's override (nothing: none), and move the plugins whose process changes,
		// with their state. Returns the plugins that could not move, with the reason; they stay where they are.
		std::vector<std::string> setMode(HostingMode mode);
		std::vector<std::string> setOverride(const std::string& plugin, std::optional<HostingMode> mode);

		struct InstanceStatus
		{
			uint64_t instance = 0;
			std::string plugin;
			bool failed = false;
		};

		struct HostStatus
		{
			uint32_t process = 0;
			std::string group; // what its plugins have in common: "shared", "vendor:<vendor>", "plugin:<id>", ...
			std::string state; // "running", "hung" or "exited"
			std::vector<InstanceStatus> instances;
		};

		// The host processes with plugins, by process id.
		std::vector<HostStatus> hosts() const;

		// The engine's id of a plugin this manager created.
		uint64_t instanceOf(const plugin::remote::RemotePlugin& plugin) const;

		// Failures that were not taken yet, oldest first.
		std::vector<PluginFailure> takeFailures();

		// Called on the monitor thread when there are new failures to take.
		void onFailure(std::function<void()> callback);

		// Ends a host process at once, as a crash would. For tests.
		bool kill(uint32_t process);

		// Registers pluginhost.get, pluginhost.setMode, pluginhost.setOverride and pluginhost.check.
		void registerCommands(Engine& engine);

	private:
		struct Instance
		{
			plugin::remote::RemotePlugin* plugin = nullptr;
			PluginRef ref;
			std::string group;
			std::shared_ptr<plugin::remote::HostConnection> host;
			bool reported = false;
		};

		struct Host
		{
			std::shared_ptr<plugin::remote::HostConnection> connection;
			int log = 0; // its log file's number
		};

		// What the remote plugins share with the manager; they unregister themselves, even after it is gone.
		struct Shared
		{
			mutable std::mutex mutex;
			std::map<uint64_t, Instance> instances;
			std::map<std::string, Host> hosts; // by group
			std::vector<PluginFailure> failures;
			HostingMode mode = g_defaultHostingMode;
			std::map<std::string, HostingMode> overrides;
			uint64_t nextInstance = 1;
		};

		static std::string groupOf(const Shared& shared, uint64_t instance, const PluginRef& plugin);
		std::shared_ptr<plugin::remote::HostConnection> hostFor(Shared& shared, const std::string& group) const;
		static std::vector<std::shared_ptr<plugin::remote::HostConnection>> dropUnused(Shared& shared);
		std::vector<std::string> reload();
		void monitor();
		void startMonitor();

		Options m_options;
		std::shared_ptr<Shared> m_shared;

		std::mutex m_monitorMutex; // guards the callback and the monitor's start and stop
		std::condition_variable m_monitorWake;
		std::function<void()> m_onFailure;
		bool m_stopping = false;
		std::thread m_monitor;
	};
}
