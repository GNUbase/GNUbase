// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/articulationnames.h"

#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <optional>
#include <sstream>

namespace gnubase::ui
{
	std::string joinNames(const std::vector<std::string>& names)
	{
		std::string text;
		for (const std::string& name : names)
			text += (text.empty() ? "" : ", ") + name;
		return text;
	}

	std::vector<std::string> splitNames(const std::string& text)
	{
		std::vector<std::string> names;
		std::string name;
		for (std::istringstream stream(text); std::getline(stream, name, ',');)
		{
			name.erase(0, name.find_first_not_of(' '));
			name.erase(name.find_last_not_of(' ') + 1);
			if (!name.empty())
				names.push_back(name);
		}
		return names;
	}

	const engine::Json* trackArticulationMap(const engine::Json& project, uint64_t track)
	{
		uint64_t id = 0;
		for (const engine::Json& each : project.value("tracks", engine::Json::array()))
			if (each.at("id") == track)
				id = each.value("articulationMap", uint64_t{0});
		const auto maps = project.find("articulationMaps");
		if (id == 0 || maps == project.end())
			return nullptr;
		for (const engine::Json& map : *maps)
			if (map.at("id") == id)
				return &map;
		return nullptr;
	}

	bool unknownArticulations(const engine::Json* map, const std::vector<std::string>& names, const std::string& type)
	{
		return std::any_of(names.begin(), names.end(),
			[&](const std::string& name)
			{
				if (map == nullptr)
					return true;
				const engine::Json& articulations = map->at("articulations");
				return std::none_of(articulations.begin(), articulations.end(),
					[&](const engine::Json& each) { return each.at("name") == name && each.at("type") == type; });
			});
	}

	std::string articulationItems(
		const engine::Json* map, const std::string& type, const std::vector<std::string>& checked)
	{
		if (map == nullptr)
			return "<menuitem disabled>No Articulation Map</menuitem>";
		std::string rml;
		for (const engine::Json& articulation : map->at("articulations"))
		{
			if (articulation.at("type") != type)
				continue;
			const std::string name = articulation.at("name").get<std::string>();
			const std::string encoded = Rml::StringUtilities::EncodeRml(name);
			rml.append(R"(<menuitem type="toggle" articulation=")")
				.append(encoded)
				.append(std::find(checked.begin(), checked.end(), name) != checked.end() ? R"(" checked>)" : R"(">)")
				.append(encoded)
				.append("</menuitem>");
		}
		return rml;
	}

	std::vector<std::string> toggleAttribute(
		const engine::Json& map, std::vector<std::string> names, const std::string& name)
	{
		if (const auto found = std::find(names.begin(), names.end(), name); found != names.end())
		{
			names.erase(found);
			return names;
		}
		const engine::Json& articulations = map.at("articulations");
		const auto groupOf = [&articulations](const std::string& each) -> std::optional<int>
		{
			for (const engine::Json& articulation : articulations)
				if (articulation.at("name") == each && articulation.at("type") == "attribute")
					return articulation.at("group").get<int>();
			return std::nullopt;
		};
		const std::optional<int> group = groupOf(name);
		if (group)
			names.erase(std::remove_if(names.begin(), names.end(),
							[&](const std::string& each) { return groupOf(each) == group; }),
				names.end());
		names.push_back(name);
		return names;
	}
}
