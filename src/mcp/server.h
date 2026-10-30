// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "mcp/sessions.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The built-in MCP server, so AI agents and other MCP clients can use every engine command. Tools and resources are
// generated from the engine's command registry. The protocol here doesn't depend on a transport; transport.h has
// stdio and HTTP.
namespace gnubase::mcp
{
	// The MCP revision this server speaks.
	inline constexpr std::string_view g_protocolVersion = "2025-06-18";

	// Speaks MCP (JSON-RPC 2.0) for one engine:
	// - every engine command is a tool, named with '_' instead of '.' because MCP clients accept only letters, digits,
	//   '_' and '-' (track.add is track_add); queries are marked read-only;
	// - every query without required parameters is also a resource, gnubase://<command>, e.g. gnubase://project.get;
	// - a session receives every engine event from its start on, and the tool events_take takes them.
	// Thread-safe: each request talks to the engine through a client of its own.
	class Server
	{
	public:
		// A tool that isn't an engine command, such as script_run.
		struct Tool
		{
			std::string name;
			std::string description;
			engine::Json inputSchema;  // arguments are checked against it before run
			engine::Json outputSchema; // or null
			bool readOnly = false;
			// Returns the structured result. A thrown std::exception becomes a tool result with isError and its
			// message.
			std::function<engine::Json(const engine::Json& arguments)> run;
			// Instead of run, for a tool that returns a picture, such as a screenshot: returns a PNG file's bytes,
			// which the client gets as image content.
			std::function<std::vector<uint8_t>(const engine::Json& arguments)> image;
		};

		explicit Server(engine::Engine& engine, SessionLimits limits = {});

		// Adds a tool next to the engine's commands. Call it before the server handles messages.
		void addTool(Tool tool);

		// Starts a session and returns its id. It receives every engine event from now on, for events_take. A
		// session that expires ends when it isn't used for the idle time of the SessionLimits.
		std::string startSession(bool expires = true);

		// Ends a session. False if there is none: it ended or expired.
		bool endSession(const std::string& id);

		// Whether a session runs. Counts as using it.
		bool hasSession(const std::string& id);

		// One JSON-RPC message from the client. Returns the response, or nothing for notifications and responses.
		// Outside a session, events_take fails.
		std::optional<engine::Json> handle(const engine::Json& message);

		// The same in a session. initialize starts one that expires if session is empty, and sets session to its id.
		std::optional<engine::Json> handle(const engine::Json& message, std::string& session);

		// The same for message text. Text that isn't JSON gets a parse error.
		std::optional<std::string> handle(std::string_view message);
		std::optional<std::string> handle(std::string_view message, std::string& session);

	private:
		std::optional<engine::Json> respond(const engine::Json& message, std::string* session);
		std::optional<std::string> respond(std::string_view message, std::string* session);
		engine::Json dispatch(const std::string& method, const engine::Json& params, std::string* session);
		engine::Json listTools() const;
		engine::Json callTool(const engine::Json& params, const std::string& session);
		engine::Json takeEvents(const engine::Json& arguments, const std::string& session);
		engine::Json listResources() const;
		engine::Json readResource(const engine::Json& params);

		engine::Engine& m_engine;
		std::vector<Tool> m_tools;
		Sessions m_sessions;
	};
}
