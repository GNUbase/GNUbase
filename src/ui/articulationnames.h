// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gnubase::ui
{
	// Articulation names separated by commas, as the info line's Articulations field and the list editor's Attributes
	// column show them ("Staccato, Pizzicato"), and the names typed there, without the spaces around them (#1568).
	std::string joinNames(const std::vector<std::string>& names);
	std::vector<std::string> splitNames(const std::string& text);

	// The articulation map of a track as project.get lists them, or null for none.
	const engine::Json* trackArticulationMap(const engine::Json& project, uint64_t track);
	// Whether one of the names isn't an articulation of the type (attribute or direction) of the map, or there is no
	// map: the info line and the list editor show a warning sign then (#1580, reference manual p. 1291).
	bool unknownArticulations(const engine::Json* map, const std::vector<std::string>& names, const std::string& type);
	// The pop-up menus of the info line's Articulations field and the list editor's Attributes column (#1580,
	// pp. 1290-1291): an item per articulation of the type of the map (articulation="name"), those named checked, or
	// without a map a disabled No Articulation Map.
	std::string articulationItems(
		const engine::Json* map, const std::string& type, const std::vector<std::string>& checked);
	// A note's attributes with the map's attribute name taken away if they have it, else added in place of the map's
	// attribute of its group (p. 1279), as the menus choose it.
	std::vector<std::string> toggleAttribute(
		const engine::Json& map, std::vector<std::string> names, const std::string& name);
}
