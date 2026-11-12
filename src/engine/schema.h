// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace gnubase::engine
{
	using Json = nlohmann::json;

	// The most characters a schema's pattern is ever matched on, whatever its maxLength says: some standard libraries
	// match recursively, about one stack frame per repeated character, and a long string overflowed the stack
	// (#1059). With the editor names' pattern, on an 8 MB stack 10,000 bytes matched and 50,000 didn't, so a byte
	// took at most some 840 bytes of stack; the 256 bytes of 64 characters then stay below 220 KB, within the 512 KB
	// of secondary threads on macOS and the 1 MB of Windows, with room for patterns that recurse more (#1159).
	inline constexpr size_t g_patternMaxLength = 64;

	// Checks a value against a JSON schema and returns what is wrong, or an empty string if it fits. Supports the
	// part of JSON Schema that commands use: type (one or a list), properties, required, additionalProperties (false,
	// or a schema for the properties that properties doesn't name), items, enum, pattern, minLength, maxLength,
	// minItems, maxItems, minimum, maximum, exclusiveMinimum and exclusiveMaximum (numbers, as in JSON Schema since
	// draft 6). minProperties is left to the command (needsOne). name is where the value comes from in messages, such
	// as "parameters.gain must be number". Lengths count characters, not bytes. A string longer than its maxLength
	// limit is refused before its pattern is matched, and with a pattern that limit is at most g_patternMaxLength.
	std::string validate(const Json& value, const Json& schema, const std::string& name = "parameters");

	// The most characters validate accepts in a string for a schema: its maxLength, at most g_patternMaxLength if it
	// has a pattern, or none.
	std::optional<size_t> maxStringLength(const Json& schema);

	// A command's parameter schema none of whose properties is required, though the command needs one of them, such
	// as a pad or a chord: "minProperties": 1, which validate leaves to the command and its message. The key commands
	// don't offer such a command without parameters (ui::KeyCommands::commands).
	Json needsOne(Json schema);
}
