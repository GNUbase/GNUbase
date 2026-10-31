// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace gnubase::engine
{
	using Json = nlohmann::json;

	// Checks a value against a JSON schema and returns what is wrong, or an empty string if it fits. Supports the
	// part of JSON Schema that commands use: type (one or a list), properties, required, additionalProperties (false,
	// or a schema for the properties that properties doesn't name), items, enum, minimum and maximum. name is where the
	// value comes from in messages, such as "parameters.gain must be number".
	std::string validate(const Json& value, const Json& schema, const std::string& name = "parameters");
}
