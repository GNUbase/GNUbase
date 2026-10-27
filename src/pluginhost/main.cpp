// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/log.h"

#include <cstdio>
#include <exception>

// Plugin host process: runs third-party plugins outside of GNUbase (#64). Not implemented yet.
int main()
{
	try
	{
		// ponytail: one fixed file name; once the app launches several hosts (#64), it passes each its own log file.
		const gnubase::logging::Session logSession(gnubase::logging::defaultDirectory() / "gnubase-pluginhost.log");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "GNUbase plugin host: %s\n", error.what());
		return 1;
	}
}
