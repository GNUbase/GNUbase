// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/connection.h"
#include "platform/log.h"
#include "pluginhost/formats.h"
#include "pluginhost/host.h"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

// Plugin host process: runs third-party plugins outside of GNUbase (docs/architecture.md, "Plugin hosting and IPC").
// The engine starts it:
//   gnubase-pluginhost --connect <connection> [--log <file name>]
// Exit codes: 0 the engine is gone, 1 unexpected error, 2 invalid command line, 3 the connection can't be opened or
// broke, 4 the engine speaks another protocol version.
namespace
{
	int run(int argc, char** argv)
	{
		std::string connection;
		std::string logFile = "gnubase-pluginhost.log";
		for (int i = 1; i + 1 < argc; i += 2)
		{
			const std::string_view option = argv[i];
			if (option == "--connect")
				connection = argv[i + 1];
			else if (option == "--log")
				logFile = argv[i + 1];
		}
		if (connection.empty())
		{
			std::fprintf(stderr, "usage: gnubase-pluginhost --connect <connection> [--log <file name>]\n");
			return 2;
		}

		try
		{
			const gnubase::logging::Session logSession(gnubase::logging::defaultDirectory() / logFile);
			gnubase::ipc::Connection control;
			try
			{
				control = gnubase::ipc::Connection::open(connection);
			}
			catch (const gnubase::ipc::Error& error)
			{
				gnubase::logging::error("{}", std::string_view(error.what()));
				return 3;
			}
			gnubase::pluginhost::PluginHost host(std::move(control), gnubase::pluginhost::builtinFormats());
			return host.run();
		}
		catch (const std::exception& error)
		{
			std::fprintf(stderr, "GNUbase plugin host: %s\n", error.what());
			return 1;
		}
	}
}

int main(int argc, char** argv)
{
	try
	{
		return run(argc, argv);
	}
	catch (...)
	{
		return 1; // even reporting the error failed
	}
}
