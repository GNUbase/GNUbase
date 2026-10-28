// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"

#include <optional>
#include <string>
#include <string_view>

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
	// - every query without required parameters is also a resource, gnubase://<command>, e.g. gnubase://project.get.
	// Thread-safe: each request talks to the engine through a client of its own.
	class Server
	{
	public:
		explicit Server(engine::Engine& engine);

		// One JSON-RPC message from the client. Returns the response, or nothing for notifications and responses.
		std::optional<engine::Json> handle(const engine::Json& message);

		// The same for message text. Text that isn't JSON gets a parse error.
		std::optional<std::string> handle(std::string_view message);

	private:
		engine::Json dispatch(const std::string& method, const engine::Json& params);
		engine::Json listTools() const;
		engine::Json callTool(const engine::Json& params);
		engine::Json listResources() const;
		engine::Json readResource(const engine::Json& params);

		engine::Engine& m_engine;
	};
}
