// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ipc/connection.h"
#include "platform/process.h"
#include "plugin/remote/protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace gnubase::plugin::remote
{
	// A host process that cannot be started, or that fails the handshake.
	class LaunchError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	struct LaunchOptions
	{
		std::filesystem::path executable;         // gnubase-pluginhost
		std::string logFile;                      // the host's log file name in the log folder
		std::chrono::milliseconds timeout{10000}; // for starting and the handshake
		// The versions sent in the handshake; tests change them to check that hosts refuse other versions.
		uint32_t protocolVersion = g_protocolVersion;
		uint32_t apiVersion = g_apiVersion;
	};

	// The engine's side of one plugin host process: it starts the process, shakes hands over the control connection
	// and runs the process's plugin control thread, which reads replies and notifications and watches the process.
	// Requests may come from any thread except audio threads; they wait for their reply until a timeout.
	// Destroying it closes the connection, which makes the host exit, and kills a host that doesn't.
	class HostConnection
	{
	public:
		struct Reply
		{
			bool ok = false;
			std::string error; // why it failed: the host's message, a timeout, or the host is gone
			std::vector<uint8_t> body;
		};

		// Starts a host process and shakes hands with it. Throws LaunchError.
		static std::shared_ptr<HostConnection> launch(const LaunchOptions& options);

		~HostConnection();
		HostConnection(const HostConnection&) = delete;
		HostConnection& operator=(const HostConnection&) = delete;

		uint32_t processId() const { return m_processId; }

		// [any] The process has exited or crashed, or broke the protocol; it answers no more requests.
		bool exited() const noexcept { return m_exited.load(std::memory_order_acquire); }
		// [any] One of its plugins missed a deadline: no new plugins go there.
		bool hung() const noexcept { return m_hung.load(std::memory_order_acquire); }
		void markHung() noexcept { m_hung.store(true, std::memory_order_release); }

		// Asks the system whether the process still runs, and updates exited(). Not real-time.
		bool checkExited();

		// Ends the process at once, as a crash would. Tests use it.
		void kill();

		// A new id for a plugin instance in this host.
		uint32_t newInstance() noexcept { return m_nextInstance.fetch_add(1, std::memory_order_relaxed); }

		// Sends a request and waits for its reply. Not real-time.
		Reply request(
			Message type, uint32_t instance, const std::vector<uint8_t>& body, std::chrono::milliseconds timeout);

		// Called on the plugin control thread when a plugin of this host notifies the engine.
		void onNotification(std::function<void(uint32_t instance, Notification notification)> handler);

	private:
		struct Pending
		{
			bool done = false;
			Reply reply;
		};

		HostConnection() = default;
		void run();
		void finish(); // the process is gone: fails every waiting request

		ipc::Connection m_control;
		uint32_t m_processId = 0;
		std::mutex m_processMutex; // guards m_process
		platform::ChildProcess m_process;

		std::mutex m_sendMutex; // one sender at a time on the control ring
		std::mutex m_mutex;     // guards everything below
		std::condition_variable m_replied;
		std::map<uint32_t, Pending*> m_pending;
		uint32_t m_nextRequest = 1;
		std::function<void(uint32_t, Notification)> m_notify;

		std::atomic<uint32_t> m_nextInstance{1};
		std::atomic<bool> m_exited{false};
		std::atomic<bool> m_hung{false};
		std::atomic<bool> m_stopping{false};
		std::thread m_thread; // the plugin control thread
	};
}
