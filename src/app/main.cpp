// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "platform/log.h"
#include "ui/ui.h"

#include <cstdio>
#include <exception>

int main()
{
	try
	{
		const gnubase::logging::Session logSession(gnubase::logging::defaultDirectory() / "gnubase.log");
		gnubase::logging::info("GNUbase {} ({})", gnubase::engine::version(), gnubase::ui::libraryVersions());
		std::printf("GNUbase %s (%s)\n", gnubase::engine::version(), gnubase::ui::libraryVersions().c_str());
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "GNUbase: %s\n", error.what());
		return 1;
	}
}
