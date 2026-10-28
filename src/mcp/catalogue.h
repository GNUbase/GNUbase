// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mcp/server.h"

#include <string>

namespace gnubase::mcp
{
	// The tool catalogue of docs/mcp.md in Markdown, from what the server lists (tools/list, resources/list): every
	// tool by area, with its engine command, whether it only reads, its description, arguments and result; then the
	// resources.
	std::string catalogue(Server& server);
}
