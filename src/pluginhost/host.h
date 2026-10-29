// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ipc/connection.h"
#include "plugin/remote/protocol.h"
#include "plugin/remote/stub.h"
#include "pluginhost/formats.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gnubase::pluginhost
{
	// The plugin host process (docs/architecture.md, "Plugin hosting and IPC"). Its main thread runs the control loop:
	// it answers the engine's handshake, creates plugin instances through the format adapters, and passes every
	// main-thread request for an instance to its stub, whose real-time thread processes the instance's blocks. It
	// exits when the engine closes the control connection or its process is gone. A watchdog thread of its own ends
	// the process when the engine is gone even if a plugin blocks the main thread.
	class PluginHost
	{
	public:
		// `control` is the engine's control connection, opened.
		PluginHost(ipc::Connection control, Formats formats);
		~PluginHost();
		PluginHost(const PluginHost&) = delete;
		PluginHost& operator=(const PluginHost&) = delete;
		PluginHost(PluginHost&&) = delete;
		PluginHost& operator=(PluginHost&&) = delete;

		// Runs until the engine is gone. Returns the exit code: 0 when the engine closed the connection or exited, 3
		// if it broke the protocol, 4 if it speaks another protocol version.
		int run();

	private:
		void handle(const plugin::remote::ControlMessage& message);
		bool create(
			uint32_t instance, plugin::remote::Reader& request, plugin::remote::Writer& reply, std::string& error);
		bool destroy(uint32_t instance, std::string& error);
		void send(plugin::remote::Message type, const plugin::remote::ControlHeader& header,
			const std::vector<uint8_t>& body);
		void reply(const plugin::remote::ControlHeader& header, bool ok, const std::string& error,
			const std::vector<uint8_t>& body);
		bool releaseStubs();
		void watchEngine(uint32_t engine);

		ipc::Connection m_control;
		plugin::remote::MessageWriter m_writer;
		Formats m_formats; // before the stubs: plugin files stay loaded until their instances are gone
		std::map<uint32_t, std::unique_ptr<plugin::remote::PluginStub>> m_stubs;
		bool m_greeted = false;
		int m_exitCode = -1; // set when the loop should end

		std::mutex m_watchMutex;
		std::condition_variable m_watchWake;
		bool m_watchStop = false;
		std::thread m_watchdog;
	};

	// The plugin host program: parses the command line (--connect <connection> [--log <file name>]), opens the
	// connection and runs a PluginHost with the formats. Returns the exit code: 0 the engine is gone, 1 unexpected
	// error, 2 invalid command line, 3 the connection can't be opened or broke, 4 the engine speaks another protocol
	// version.
	int runPluginHost(int argc, char** argv, Formats formats);
}
