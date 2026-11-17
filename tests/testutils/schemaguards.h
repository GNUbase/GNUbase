// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "engine/schema.h"
#include <algorithm>
#include <string>
#include <vector>

// Guard the command, settings and UI tool schemas against promises the validator cannot keep.
namespace gnubase::test
{
	using engine::g_patternMaxLength;
	using engine::Json;
	inline void countPatterns(
		const Json& schema, int& patterns, std::vector<std::string>& unbounded, const std::string& where)
	{
		if (schema.is_object())
		{
			if (const auto pattern = schema.find("pattern"); pattern != schema.end() && pattern->is_string())
			{
				++patterns;
				// Within the ceiling, so that the references show the limit validate checks.
				if (const auto maxLength = schema.find("maxLength"); maxLength == schema.end() ||
					!maxLength->is_number_integer() || *maxLength < 0 || maxLength->get<size_t>() > g_patternMaxLength)
					unbounded.push_back(where);
			}
			for (const auto& [key, item] : schema.items())
			{
				std::string path = where;
				path += '.';
				path += key;
				countPatterns(item, patterns, unbounded, path);
			}
		}
		else if (schema.is_array())
		{
			for (const Json& item : schema)
				countPatterns(item, patterns, unbounded, where);
		}
	}

	inline void uncheckedKeywords(const Json& schema, std::vector<std::string>& found, const std::string& where)
	{
		static const std::vector<std::string> counts = {"minLength", "maxLength", "minItems", "maxItems"};
		static const std::vector<std::string> bounds = {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"};
		// What validate checks, what only describes, and minProperties, which needsOne leaves to the command.
		static const std::vector<std::string> known = {"type", "properties", "required", "additionalProperties",
			"items", "enum", "pattern", "minLength", "maxLength", "minItems", "maxItems", "minimum", "maximum",
			"exclusiveMinimum", "exclusiveMaximum", "description", "minProperties"};
		if (!schema.is_object())
			return;
		for (const auto& [key, item] : schema.items())
		{
			std::string path = where;
			path += '.';
			path += key;
			const bool isCount = std::find(counts.begin(), counts.end(), key) != counts.end();
			const bool isBound = std::find(bounds.begin(), bounds.end(), key) != bounds.end();
			if (std::find(known.begin(), known.end(), key) == known.end())
			{
				found.push_back(path);
			}
			else if ((isCount && (!item.is_number_integer() || item < 0)) || (isBound && !item.is_number()))
			{
				std::string value = path;
				value += ' ';
				value += item.dump();
				found.push_back(value);
			}
			if (key == "properties" && item.is_object())
			{
				for (const auto& [property, propertySchema] : item.items())
				{
					std::string member = path;
					member += '.';
					member += property;
					uncheckedKeywords(propertySchema, found, member);
				}
			}
			else if (key == "items" || key == "additionalProperties")
			{
				uncheckedKeywords(item, found, path);
			}
		}
	}
}
