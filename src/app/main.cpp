// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "ui/ui.h"

#include <cstdio>

int main()
{
	std::printf("GNUbase %s (%s)\n", gnubase::engine::version(), gnubase::ui::libraryVersions().c_str());
	return 0;
}
