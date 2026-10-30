// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plugin/plugin.h"
#include "plugin/remote/hostconnection.h"
#include "plugin/remote/proxy.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
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
	// state.
	//
	// Threads: everything that waits for a host process runs on the manager's job thread, one job after the other, so
	// the command thread never waits for a plugin: starting processes, creating plugins, moving them, and fetching
	// what their notifications say changed. Callers run their own slow plugin calls (activate, states) there with
	// run(). Host processes are started from the job thread only, which lives as long as the manager: on Linux a
	// process ends with the thread that started it. A monitor thread watches the remote plugins and their processes
	// and reports failures; the engine announces them as plugin.failed events.
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
		// Ends the job and monitor threads; queued jobs are dropped, their futures report broken promises. Remote
		// plugins that outlive the manager fail (Failure::HostGone).
		~HostManager();
		HostManager(const HostManager&) = delete;
		HostManager& operator=(const HostManager&) = delete;
		HostManager(HostManager&&) = delete;
		HostManager& operator=(HostManager&&) = delete;

		// [any] The plugin host program for processes started from now on; empty: the one next to the running
		// program. Tests use one with test plugins.
		void setExecutable(std::filesystem::path executable);

		// [any] Runs `job` on the job thread after the jobs before it. The future reports its exception, if any.
		std::future<void> run(std::function<void()> job);

		// [any] Creates a remote plugin on the job thread: in the host process the hosting mode picks, started if
		// needed, and initialized there. `engineHost` gets what the plugin asks of the engine; it must outlive the
		// plugin. The future throws plugin::remote::LaunchError if the process can't be started, and std::runtime_error
		// if the plugin can't be created.
		std::future<std::unique_ptr<plugin::remote::RemotePlugin>> load(
			const PluginRef& plugin, plugin::Host* engineHost = nullptr);

		// [not the job thread] A remote plugin as load() makes it, but not initialized, and waits for it. For tests.
		std::unique_ptr<plugin::remote::RemotePlugin> create(
			const PluginRef& plugin, plugin::Host* engineHost = nullptr);

		HostingMode mode() const;
		std::map<std::string, HostingMode> overrides() const;

		// [any] Change the hosting mode, or a plugin's override (nothing: none), at once, and move the plugins whose
		// process changes with their state on the job thread. The future has the plugins that could not move, with the
		// reason; they stay where they are. When the moves are done, onReport is called.
		std::future<std::vector<std::string>> setMode(HostingMode mode);
		std::future<std::vector<std::string>> setOverride(const std::string& plugin, std::optional<HostingMode> mode);

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
			std::string log;   // its log file's name in the log folder
			std::vector<InstanceStatus> instances;
		};

		// The host processes with plugins, by process id.
		std::vector<HostStatus> hosts() const;

		// The engine's id of a plugin this manager created.
		uint64_t instanceOf(const plugin::remote::RemotePlugin& plugin) const;

		// Failures that were not taken yet, oldest first.
		std::vector<PluginFailure> takeFailures();
		// The results of hosting mode changes whose moves are done and that were not taken yet, oldest first: the
		// plugins that could not move.
		std::vector<std::vector<std::string>> takeMoves();

		// Called on the monitor or the job thread when there are failures or moves to take. Must not wait for jobs.
		void onReport(std::function<void()> callback);

		// [not the job thread] Ends a host process at once, as a crash would. For tests.
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
			bool busy = false; // a job uses the plugin: it isn't destroyed meanwhile
		};

		// What the remote plugins share with the manager; they unregister themselves, even after it is gone.
		struct Shared
		{
			mutable std::mutex mutex;
			std::condition_variable idle; // an instance is no longer busy
			std::map<uint64_t, Instance> instances;
			// By group: the group's process, then any that started while the ones before were silent.
			std::map<std::string, std::vector<std::shared_ptr<plugin::remote::HostConnection>>> hosts;
			std::vector<PluginFailure> failures;
			std::vector<std::vector<std::string>> moves;
			HostingMode mode = g_defaultHostingMode;
			std::map<std::string, HostingMode> overrides;
			uint64_t nextInstance = 1;
			std::filesystem::path executable;
			bool closed = false; // the manager is gone

			// The job queue.
			std::mutex jobMutex;
			std::condition_variable jobWake;
			std::deque<std::function<void()>> jobs;
			bool stopping = false;
		};

		static std::string groupOf(const Shared& shared, uint64_t instance, const PluginRef& plugin);
		std::shared_ptr<plugin::remote::HostConnection> hostFor(const std::string& group);
		bool answers(const plugin::remote::HostConnection& host) const; // new plugins may go there
		bool awayFromGroup(const Instance& instance) const;             // with Shared::mutex locked
		static void post(Shared& shared, std::function<void()> job);
		static std::vector<std::shared_ptr<plugin::remote::HostConnection>> dropUnused(Shared& shared);
		static void refresh(Shared& shared, uint64_t id);
		std::unique_ptr<plugin::remote::RemotePlugin> make(const PluginRef& plugin, plugin::Host* engineHost);
		std::future<std::vector<std::string>> reload();
		std::vector<std::string> moveAll();
		void jobs();
		void monitor();
		void report();

		Options m_options;
		std::shared_ptr<Shared> m_shared;
		std::thread::id m_jobThreadId;
		std::thread m_jobThread;

		std::mutex m_monitorMutex; // guards the callback and the monitor's stop
		std::condition_variable m_monitorWake;
		std::function<void()> m_onReport;
		bool m_stopping = false;
		std::thread m_monitor;
	};
}
