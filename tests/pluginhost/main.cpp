// SPDX-License-Identifier: GPL-3.0-or-later
// The tests' plugin host: the shipped host's code (pluginhost/host.h) with the test format added, so tests run the
// test plugins in a plugin host process the way GNUbase runs third-party plugins.
//   gnubase-testpluginhost --connect <connection> [--log <file name>]   a plugin host, as gnubase-pluginhost
//   gnubase-testpluginhost --play-engine <file>   plays an engine whose host's main thread hangs: starts a plugin
//                                                 host, creates test:hang in it, makes it hang on the host's main
//                                                 thread, writes the host's process id to the file, then waits to be
//                                                 killed
#include "platform/process.h"
#include "plugin/events.h"
#include "plugin/remote/hostconnection.h"
#include "plugin/remote/protocol.h"
#include "pluginhost/formats.h"
#include "pluginhost/host.h"
#include "pluginhost/testformat.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

namespace
{
	using namespace gnubase;
	using namespace std::chrono_literals;

	int playEngine(const char* file)
	{
		plugin::remote::LaunchOptions options;
		options.executable = platform::currentExecutablePath();
		options.logFile = "gnubase-testpluginhost-hung.log";
		const std::shared_ptr<plugin::remote::HostConnection> host = plugin::remote::HostConnection::launch(options);

		plugin::remote::Writer create;
		create.string("test:hang");
		create.string("");
		const uint32_t instance = host->newInstance();
		if (!host->request(plugin::remote::Message::Create, instance, create.data(), 10s).ok)
			return 5;
		// Hang on: the plugin hangs while it flushes, on the host's main thread.
		plugin::EventList events(1);
		plugin::Event hang;
		hang.type = plugin::EventType::ParamValue;
		hang.param = 1;
		hang.value = 1.0;
		events.push(hang);
		plugin::remote::Writer flush;
		plugin::remote::write(flush, events);
		if (host->request(plugin::remote::Message::FlushParams, instance, flush.data(), 500ms).ok)
			return 6; // it answered, so it doesn't hang

		std::filesystem::path partial = std::filesystem::u8path(file);
		partial += ".partial";
		std::ofstream(partial) << host->processId() << '\n';
		std::filesystem::rename(partial, std::filesystem::u8path(file));
		for (;;)
			std::this_thread::sleep_for(1s);
	}
}

int main(int argc, char** argv)
{
	try
	{
		if (argc == 3 && std::string_view(argv[1]) == "--play-engine")
			return playEngine(argv[2]);
		pluginhost::Formats formats = pluginhost::builtinFormats();
		formats.add(testformat::makeTestFormat());
		return pluginhost::runPluginHost(argc, argv, std::move(formats));
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "gnubase-testpluginhost: %s\n", error.what());
		return 1;
	}
}
