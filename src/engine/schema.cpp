// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/schema.h"

#include <algorithm>
#include <regex>

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

		// The characters of a UTF-8 string: code points, as JSON Schema's minLength and maxLength count them. Strings
		// from Lua needn't be UTF-8, and stray continuation bytes count as no characters.
		size_t characters(const std::string& text)
		{
			return static_cast<size_t>(std::count_if(
				text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
		}

		// Whether a string has more characters than maxLength. Since stray continuation bytes count as none, the bytes
		// are bounded too: at most 4 per character (without overflowing for a large maxLength).
		bool tooLong(const std::string& text, size_t maxLength)
		{
			return (text.size() + 3) / 4 > maxLength || characters(text) > maxLength;
		}

		// "1 item", "2 items".
		std::string count(const Json& number, const char* noun)
		{
			return number.dump() + " " + noun + (number == 1 ? "" : "s");
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
		if (value.is_string())
		{
			const auto& text = value.get_ref<const std::string&>();
			// Before the pattern, so that it never matches more than g_patternMaxLength characters: some standard
			// libraries match recursively, about one stack frame per repeated character, and a long string overflowed
			// the stack (#1059).
			if (const std::optional<size_t> maxLength = maxStringLength(schema); maxLength && tooLong(text, *maxLength))
				return name + " must be at most " + std::to_string(*maxLength) + " characters";
			if (const auto minLength = schema.find("minLength");
				minLength != schema.end() && characters(text) < minLength->get<size_t>())
				return name + " must be at least " + count(*minLength, "character");
			// ponytail: compiles the pattern on every check; cache them if a pattern ever sits on a hot path.
			if (const auto pattern = schema.find("pattern");
				pattern != schema.end() && !std::regex_search(text, std::regex(pattern->get<std::string>())))
				return name + " must match " + pattern->get<std::string>();
		}
		if (value.is_number())
		{
			const auto number = value.get<double>();
			// Written so that a NaN, which an in-process client such as Lua can send, is outside the range: every
			// comparison with it is false (#1131).
			if (const auto minimum = schema.find("minimum");
				minimum != schema.end() && !(number >= minimum->get<double>()))
				return name + " must be at least " + minimum->dump();
			if (const auto maximum = schema.find("maximum");
				maximum != schema.end() && !(number <= maximum->get<double>()))
				return name + " must be at most " + maximum->dump();
			if (const auto minimum = schema.find("exclusiveMinimum");
				minimum != schema.end() && number <= minimum->get<double>())
				return name + " must be more than " + minimum->dump();
			if (const auto maximum = schema.find("exclusiveMaximum");
				maximum != schema.end() && number >= maximum->get<double>())
				return name + " must be less than " + maximum->dump();
		}
		if (value.is_array())
		{
			if (const auto minItems = schema.find("minItems");
				minItems != schema.end() && value.size() < minItems->get<size_t>())
				return name + " must have at least " + count(*minItems, "item");
			if (const auto maxItems = schema.find("maxItems");
				maxItems != schema.end() && value.size() > maxItems->get<size_t>())
				return name + " must have at most " + count(*maxItems, "item");
		}

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

	std::optional<size_t> maxStringLength(const Json& schema)
	{
		std::optional<size_t> limit;
		if (const auto maxLength = schema.find("maxLength"); maxLength != schema.end())
			limit = maxLength->get<size_t>();
		if (schema.contains("pattern"))
			limit = std::min(limit.value_or(g_patternMaxLength), g_patternMaxLength);
		return limit;
	}

	Json needsOne(Json schema)
	{
		schema["minProperties"] = 1;
		return schema;
	}
}
