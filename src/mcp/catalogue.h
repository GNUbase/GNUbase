// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mcp/server.h"

#include <string>

namespace gnubase::mcp
{
	// The MCP tool catalogue in Markdown (docs/api-reference.md), from what the server lists (tools/list,
	// resources/list): every tool by area, with its engine command, whether it only reads, its description, arguments
	// and result; then the resources.
	std::string catalogue(Server& server);
}
