// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/schema.h"

#include <algorithm>

namespace gnubase::engine
{
	namespace
	{
		bool hasType(const Json& value, const std::string& type)
		{
			if (type == "object")
				return value.is_object();
			if (type == "array")
				return value.is_array();
			if (type == "string")
				return value.is_string();
			if (type == "integer")
				return value.is_number_integer();
			if (type == "number")
				return value.is_number();
			if (type == "boolean")
				return value.is_boolean();
			return type == "null" && value.is_null();
		}

		// The schema's "type" as a message ("string or null"), or empty if the value has one of the types.
		std::string typeMismatch(const Json& value, const Json& type)
		{
			const Json types = type.is_array() ? type : Json::array({type});
			std::string names;
			for (const Json& name : types)
			{
				if (hasType(value, name.get<std::string>()))
					return {};
				names += (names.empty() ? "" : " or ") + name.get<std::string>();
			}
			return names;
		}
	}

	std::string validate(const Json& value, const Json& schema, const std::string& name)
	{
		if (const auto type = schema.find("type"); type != schema.end())
		{
			if (const std::string expected = typeMismatch(value, *type); !expected.empty())
				return name + " must be " + expected;
		}
		if (const auto choices = schema.find("enum");
			choices != schema.end() && std::find(choices->begin(), choices->end(), value) == choices->end())
			return name + " must be one of " + choices->dump();
		if (const auto minimum = schema.find("minimum");
			value.is_number() && minimum != schema.end() && value.get<double>() < minimum->get<double>())
			return name + " must be at least " + minimum->dump();
		if (const auto maximum = schema.find("maximum");
			value.is_number() && maximum != schema.end() && value.get<double>() > maximum->get<double>())
			return name + " must be at most " + maximum->dump();

		if (value.is_object())
		{
			for (const Json& required : schema.value("required", Json::array()))
			{
				if (!value.contains(required.get<std::string>()))
					return name + "." + required.get<std::string>() + " is missing";
			}
			const Json properties = schema.value("properties", Json::object());
			const auto additional = schema.find("additionalProperties");
			const bool closed = additional != schema.end() && *additional == false;
			for (const auto& [key, item] : value.items())
			{
				std::string member = name;
				member += '.';
				member += key;
				if (const auto property = properties.find(key); property != properties.end())
				{
					if (std::string problem = validate(item, *property, member); !problem.empty())
						return problem;
				}
				else if (additional != schema.end() && additional->is_object())
				{
					if (std::string problem = validate(item, *additional, member); !problem.empty())
						return problem;
				}
				else if (closed)
				{
					return member + " is not allowed";
				}
			}
		}

		if (const auto items = schema.find("items"); value.is_array() && items != schema.end())
		{
			for (size_t i = 0; i < value.size(); ++i)
			{
				std::string element = name;
				element += '[';
				element += std::to_string(i);
				element += ']';
				if (std::string problem = validate(value[i], *items, element); !problem.empty())
					return problem;
			}
		}
		return {};
	}
}
