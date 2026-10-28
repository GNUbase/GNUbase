// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"

#include <iosfwd>
#include <string>

// JSON schemas in words, for the references generated from the command registry: docs/lua.md and docs/mcp.md.
namespace gnubase::engine
{
	// The schema's type: "integer", "string or null", "one of "a" | "b"", "array of integer", or "any".
	std::string schemaType(const Json& schema);

	// The schema's properties as a Markdown list under a heading, one "- `name`: type, required" each; nothing if it
	// has no properties.
	void writeProperties(std::ostream& out, const Json& schema, const std::string& heading);
}
