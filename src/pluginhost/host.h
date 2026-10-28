// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ipc/connection.h"
#include "plugin/remote/protocol.h"
#include "plugin/remote/stub.h"
#include "pluginhost/formats.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::pluginhost
{
	// The plugin host process (docs/architecture.md, "Plugin hosting and IPC"). Its main thread runs the control loop:
	// it answers the engine's handshake, creates plugin instances through the format adapters, and passes every
	// main-thread request for an instance to its stub, whose real-time thread processes the instance's blocks. It
	// exits when the engine closes the control connection or its process is gone.
	class PluginHost
	{
	public:
		// `control` is the engine's control connection, opened.
		PluginHost(ipc::Connection control, Formats formats);
		~PluginHost();
		PluginHost(const PluginHost&) = delete;
		PluginHost& operator=(const PluginHost&) = delete;

		// Runs until the engine is gone. Returns the exit code: 0 when the engine closed the connection or exited, 3
		// if it broke the protocol, 4 if it speaks another protocol version.
		int run();

	private:
		void handle(plugin::remote::Message type, const plugin::remote::ControlHeader& header,
			const std::vector<uint8_t>& body);
		bool create(
			uint32_t instance, plugin::remote::Reader& request, plugin::remote::Writer& reply, std::string& error);
		bool destroy(uint32_t instance, std::string& error);
		void send(plugin::remote::Message type, const plugin::remote::ControlHeader& header,
			const std::vector<uint8_t>& body);
		bool releaseStubs();

		ipc::Connection m_control;
		Formats m_formats; // before the stubs: plugin files stay loaded until their instances are gone
		std::map<uint32_t, std::unique_ptr<plugin::remote::PluginStub>> m_stubs;
		bool m_greeted = false;
		int m_exitCode = -1; // set when the loop should end
	};
}
