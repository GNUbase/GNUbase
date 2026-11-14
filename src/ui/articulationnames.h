// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace gnubase::ui
{
	// Articulation names separated by commas, as the info line's Articulations field and the list editor's Attributes
	// column show them ("Staccato, Pizzicato"), and the names typed there, without the spaces around them (#1568).
	std::string joinNames(const std::vector<std::string>& names);
	std::vector<std::string> splitNames(const std::string& text);
}
