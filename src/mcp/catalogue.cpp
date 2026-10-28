// SPDX-License-Identifier: GPL-3.0-or-later
#include "mcp/catalogue.h"

#include "engine/schematext.h"

#include <algorithm>
#include <sstream>

namespace gnubase::mcp
{
	using engine::Json;

	namespace
	{
		// The result of a request that takes no parameters, sorted by key.
		Json list(Server& server, const char* method, const char* items, const char* key)
		{
			Json result = server.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}})
							  .value_or(Json())
							  .at("result")
							  .at(items);
			std::sort(result.begin(), result.end(), [key](const Json& a, const Json& b)
				{ return a.at(key).get<std::string>() < b.at(key).get<std::string>(); });
			return result;
		}
	}

	std::string catalogue(Server& server)
	{
		std::ostringstream out;
		std::string area;
		for (const Json& tool : list(server, "tools/list", "tools", "name"))
		{
			const auto name = tool.at("name").get<std::string>();
			const std::string toolArea = name.substr(0, name.find('_'));
			if (toolArea != area)
			{
				area = toolArea;
				out << (out.tellp() > 0 ? "\n" : "") << "### " << area << '\n';
			}
			out << "\n#### `" << name << "`\n\n";
			std::string facts;
			if (const auto title = tool.find("title"); title != tool.end())
				facts = "Engine command `" + title->get<std::string>() + "`.";
			if (tool.value("annotations", Json::object()).value("readOnlyHint", false))
				facts += std::string(facts.empty() ? "" : " ") + "Read-only.";
			if (!facts.empty())
				out << facts << "\n\n";
			out << tool.at("description").get<std::string>() << '\n';
			engine::writeProperties(out, tool.at("inputSchema"), "Arguments");
			if (const auto output = tool.find("outputSchema"); output != tool.end())
				engine::writeProperties(out, *output, "Result");
		}

		out << "\n### Resources\n\nQueries without required arguments can also be read as resources:\n\n";
		for (const Json& resource : list(server, "resources/list", "resources", "uri"))
			out << "- `" << resource.at("uri").get<std::string>()
				<< "`: " << resource.at("description").get<std::string>() << '\n';
		return out.str();
	}
}
