// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "mcp/server.h"

namespace gnubase::app
{
	// Adds the MCP tool script_run: it runs Lua code with GNUbase's API in a sandbox (untrusted: no files, processes
	// or bytecode, at most a minute of Lua code) and returns what the script printed.
	void addScriptTool(mcp::Server& server, engine::Engine& engine);
}
