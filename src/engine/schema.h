// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace gnubase::engine
{
	using Json = nlohmann::json;

	// Checks a value against a JSON schema and returns what is wrong, or an empty string if it fits. Supports the
	// part of JSON Schema that commands use: type (one or a list), properties, required, additionalProperties (false,
	// or a schema for the properties that properties doesn't name), items, enum, pattern, maxLength, minimum and
	// maximum. name is where the value comes from in messages, such as "parameters.gain must be number". A string
	// longer than its maxLength (in characters, not bytes) is refused before its pattern is matched, so a schema with
	// a pattern needs a maxLength for strings from outside: matching may recurse once per character (#1059).
	std::string validate(const Json& value, const Json& schema, const std::string& name = "parameters");

	// A command's parameter schema none of whose properties is required, though the command needs one of them, such
	// as a pad or a chord: "minProperties": 1, which validate leaves to the command and its message. The key commands
	// don't offer such a command without parameters (ui::KeyCommands::commands).
	Json needsOne(Json schema);
}
