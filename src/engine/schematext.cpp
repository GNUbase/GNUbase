// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/schematext.h"

#include "engine/schema.h"

#include <algorithm>
#include <optional>
#include <ostream>

namespace gnubase::engine
{
	std::string schemaType(const Json& schema)
	{
		const Json type = schema.value("type", Json());
		std::string text;
		if (const auto choices = schema.find("enum"); choices != schema.end())
		{
			text = "one of ";
			bool first = true;
			for (const Json& choice : *choices)
			{
				text += (first ? "" : " | ") + choice.dump();
				first = false;
			}
		}
		else if (type.is_array())
		{
			for (const Json& name : type)
				text += (text.empty() ? "" : " or ") + name.get<std::string>();
		}
		else if (type.is_string())
			text = type.get<std::string>();
		else
			text = "any";
		const std::optional<size_t> maxLength = maxStringLength(schema);
		if (const auto pattern = schema.find("pattern"); pattern != schema.end())
			text += " matching `" + pattern->get<std::string>() + "`" +
				(maxLength ? ", at most " + std::to_string(*maxLength) + " characters" : "");
		else if (maxLength)
			text += " of at most " + std::to_string(*maxLength) + " characters";
		else if (type == "array" && schema.contains("items"))
			text += " of " + schemaType(schema.at("items"));
		const auto bound = [&](const char* key, const char* phrase, const char* unit = "")
		{
			if (const auto value = schema.find(key); value != schema.end())
			{
				text += std::string(", ") + phrase + " " + value->dump();
				if (*unit != '\0')
					text += std::string(" ") + unit + (*value == 1 ? "" : "s");
			}
		};
		bound("minLength", "at least", "character");
		bound("minItems", "at least", "item");
		bound("maxItems", "at most", "item");
		bound("minimum", "at least");
		bound("maximum", "at most");
		bound("exclusiveMinimum", "more than");
		bound("exclusiveMaximum", "less than");
		return text;
	}

	void writeProperties(std::ostream& out, const Json& schema, const std::string& heading)
	{
		const Json properties = schema.value("properties", Json::object());
		if (properties.empty())
			return;
		const Json required = schema.value("required", Json::array());
		out << '\n' << heading << ":\n\n";
		for (const auto& [name, property] : properties.items())
		{
			const bool isRequired = std::find(required.begin(), required.end(), name) != required.end();
			out << "- `" << name << "`: " << schemaType(property) << (isRequired ? ", required" : "");
			if (const auto description = property.find("description"); description != property.end())
				out << ". " << description->get<std::string>();
			out << '\n';
		}
	}
}
