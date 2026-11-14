// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/articulationnames.h"

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
}
