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
		std::filesystem::path errorOutput;        // the host's standard error goes there; empty: GNUbase's own
		std::chrono::milliseconds timeout{10000}; // for starting and the handshake
		// The versions sent in the handshake; tests change them to check that hosts refuse other versions.
		uint32_t protocolVersion = g_protocolVersion;
		uint32_t apiVersion = g_apiVersion;
	};

	// The engine's side of one plugin host process: it starts the process, shakes hands over the control connection
	// and runs the process's plugin control thread, which reads replies and notifications and watches the process.
	// Requests may come from any thread except audio threads; they wait for their reply until a timeout. Only the
	// plugin control thread touches the process once it runs, so asking whether the host exited never waits.
	// Destroying it closes the connection, which makes the host exit, and kills a host that doesn't.
	class HostConnection
	{
	public:
		struct Reply
		{
			bool ok = false;
			std::string error; // why it failed: the host's message, a timeout, or the host is gone
			std::vector<uint8_t> body;
			// The request went out but its reply didn't come in time: the host may still carry it out later.
			bool late = false;
		};

		// What a plugin instance asks of the engine. Called on the plugin control thread; must be quick, must not block
		// and must not send requests, whose replies that thread reads.
		using Listener = std::function<void(Notification notification, uint32_t detail)>;

		// Starts a host process and shakes hands with it. Throws LaunchError. On Linux a host ends when the thread that
		// started it ends (platform::endWithParent), so call it from a thread that lives as long as the host, such as
		// the HostManager's job thread.
		static std::shared_ptr<HostConnection> launch(const LaunchOptions& options);

		~HostConnection();
		HostConnection(const HostConnection&) = delete;
		HostConnection& operator=(const HostConnection&) = delete;
		HostConnection(HostConnection&&) = delete;
		HostConnection& operator=(HostConnection&&) = delete;

		uint32_t processId() const { return m_processId; }

		// [any] The process has exited or crashed, or broke the protocol; it answers no more requests. The plugin
		// control thread looks at the process every 5 ms.
		bool exited() const noexcept { return m_exited.load(std::memory_order_acquire); }
		// [any] One of its plugins missed a deadline: no new plugins go there, and its other plugins fail at their
		// next block instead of waiting for their own deadline.
		bool hung() const noexcept { return m_hung.load(std::memory_order_acquire); }
		void markHung() noexcept { m_hung.store(true, std::memory_order_release); }
		// [any] Its main thread didn't answer the last request in time: no new plugins go there until it answers one.
		// Its plugins go on processing.
		bool unresponsive() const noexcept { return m_unresponsive.load(std::memory_order_acquire); }

		// Ends the process at once, as a crash would; the plugin control thread kills it. Returns once exited() is
		// true, after a second at most. Never holds a lock meanwhile. Tests use it.
		void kill();

		// A new id for a plugin instance in this host.
		uint32_t newInstance() noexcept { return m_nextInstance.fetch_add(1, std::memory_order_relaxed); }

		// Sends a request and waits for its reply. Not real-time. A request larger than g_maxControlMessage fails at
		// once without reaching the host.
		Reply request(
			Message type, uint32_t instance, const std::vector<uint8_t>& body, std::chrono::milliseconds timeout);

		// Sends a request without waiting for its reply, which is dropped when it comes: e.g. to destroy an instance
		// whose Create timed out but may still succeed. False if it could not be sent.
		bool post(Message type, uint32_t instance, const std::vector<uint8_t>& body, std::chrono::milliseconds timeout);

		// [any, not real-time] Sets who gets the notifications of an instance; an empty function removes it. Once it
		// returns, the previous listener is not running and is not called again. Must not be called from a listener.
		void listen(uint32_t instance, Listener listener);

	private:
		struct Pending
		{
			bool done = false;
			Reply reply;
		};

		HostConnection() = default;
		bool send(Message type, uint32_t instance, uint32_t request, const std::vector<uint8_t>& body,
			std::chrono::milliseconds timeout, std::string& error);
		void run();
		void received(const ControlMessage& message);
		void finish(); // the process is gone: fails every waiting request

		ipc::Connection m_control;
		uint32_t m_processId = 0;
		platform::ChildProcess m_process; // the plugin control thread's while it runs

		std::timed_mutex m_sendMutex; // one sender at a time on the control ring; others wait their own timeout at most
		std::unique_ptr<MessageWriter> m_writer;
		std::mutex m_mutex; // guards everything below
		std::condition_variable m_replied;
		std::map<uint32_t, Pending*> m_pending;
		std::mutex m_dispatchMutex;               // held while a listener runs, and by listen()
		std::map<uint32_t, Listener> m_listeners; // guarded by m_dispatchMutex
		uint32_t m_nextRequest = 1;

		std::atomic<uint32_t> m_nextInstance{1};
		std::atomic<bool> m_exited{false};
		std::atomic<bool> m_hung{false};
		std::atomic<bool> m_unresponsive{false};
		std::atomic<bool> m_killRequested{false};
		std::atomic<bool> m_stopping{false};
		std::thread m_thread; // the plugin control thread
	};
}
