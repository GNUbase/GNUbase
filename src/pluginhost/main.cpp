// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/formats.h"
#include "pluginhost/host.h"

// Plugin host process: runs third-party plugins outside of GNUbase (docs/architecture.md, "Plugin hosting and IPC").
// The engine starts it:
//   gnubase-pluginhost --connect <connection> [--log <file name>]
// Exit codes: 0 the engine is gone, 1 unexpected error, 2 invalid command line, 3 the connection can't be opened or
// broke, 4 the engine speaks another protocol version.
int main(int argc, char** argv)
{
	try
	{
		return gnubase::pluginhost::runPluginHost(argc, argv, gnubase::pluginhost::builtinFormats());
	}
	catch (...)
	{
		return 1; // even reporting the error failed
	}
}
