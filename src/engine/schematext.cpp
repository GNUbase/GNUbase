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
		if (const auto choices = schema.find("enum"); choices != schema.end())
		{
			std::string text;
			for (const Json& choice : *choices)
				text += (text.empty() ? "" : " | ") + choice.dump();
			return "one of " + text;
		}
		const Json type = schema.value("type", Json());
		std::string text;
		if (type.is_array())
		{
			for (const Json& name : type)
				text += (text.empty() ? "" : " or ") + name.get<std::string>();
		}
		else if (type.is_string())
		{
			text = type.get<std::string>();
		}
		else
		{
			return "any";
		}
		// The limit validate checks, which may be lower than the schema's maxLength (g_patternMaxLength).
		const std::optional<size_t> maxLength = maxStringLength(schema);
		const std::string limit = maxLength ? std::to_string(*maxLength) + " characters" : std::string();
		if (const auto pattern = schema.find("pattern"); pattern != schema.end())
			return text + " matching `" + pattern->get<std::string>() + "`" + (maxLength ? ", at most " + limit : "");
		if (maxLength)
			return text + " of at most " + limit;
		if (type == "array" && schema.contains("items"))
			return "array of " + schemaType(schema.at("items"));
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
