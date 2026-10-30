// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/headless.h"
#include "app/options.h"
#include "app/windowed.h"
#include "engine/engine.h"
#include "platform/log.h"
#include "ui/ui.h"

#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

extern "C" void gnubaseRequestStop(int /*signal*/)
{
	gnubase::app::g_stopRequested = 1;
}

namespace
{
	namespace app = gnubase::app;

	int run(int argc, char** argv)
	{
		try
		{
			app::Options options = app::parseOptions(app::commandLineArguments(argc, argv));
			if (options.help)
			{
				std::cout << app::usage();
				return app::g_exitOk;
			}
			if (options.version)
			{
				std::cout << "GNUbase " << gnubase::engine::version() << '\n';
				return app::g_exitOk;
			}

			const gnubase::logging::Session logSession(
				gnubase::logging::defaultDirectory() / (options.headless ? "gnubase-headless.log" : "gnubase.log"));
			gnubase::logging::info("GNUbase {} ({})", gnubase::engine::version(), gnubase::ui::libraryVersions());
			options.settings = app::settingsDirectory(options).u8string();
			if (options.settings.empty())
				gnubase::logging::warning("No settings folder: this user has no home folder; settings aren't saved");
			else
				gnubase::logging::info("Settings in {}", options.settings);
			if (options.headless)
			{
				if (std::signal(SIGINT, gnubaseRequestStop) == SIG_ERR ||
					std::signal(SIGTERM, gnubaseRequestStop) == SIG_ERR)
					gnubase::logging::warning("Ctrl+C won't stop GNUbase: no signal handler");
				return app::runHeadless(options, std::cin, std::cout, std::cerr);
			}

			return app::runWindowed(options, std::cout, std::cerr);
		}
		catch (const app::UsageError& error)
		{
			std::cerr << "gnubase: " << error.what() << "\n\n" << app::usage();
			return app::g_exitUsage;
		}
		catch (const std::exception& error)
		{
			std::cerr << "gnubase: " << error.what() << '\n';
			return app::g_exitError;
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
		return gnubase::app::g_exitError; // even reporting the error failed
	}
}
