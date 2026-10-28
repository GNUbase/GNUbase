// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/schematext.h"

#include <algorithm>
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
		if (type.is_array())
		{
			std::string text;
			for (const Json& name : type)
				text += (text.empty() ? "" : " or ") + name.get<std::string>();
			return text;
		}
		if (!type.is_string())
			return "any";
		if (type == "array" && schema.contains("items"))
			return "array of " + schemaType(schema.at("items"));
		return type.get<std::string>();
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
