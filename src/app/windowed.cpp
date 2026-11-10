// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/windowed.h"

#include "app/appui.h"
#include "app/headless.h"
#include "app/resources.h"
#include "app/scripttool.h"
#include "app/uitools.h"
#include "engine/engine.h"
#include "engine/plugins/hostmanager.h"
#include "mcp/server.h"
#include "mcp/transport.h"
#include "platform/log.h"
#include "ui/livereload.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>

namespace gnubase::app
{
	namespace
	{
		// Debug builds load documents again when their RML or RCSS files change (ui/livereload.h).
#ifdef GNUBASE_LIVE_RELOAD
		constexpr bool g_liveReload = true;
#else
		constexpr bool g_liveReload = false;
#endif

		// The safe start mode's dialog (#163, reference manual pp. 1584-1586), as the OS's message boxes, since it
		// comes before the engine and the UI: the plugins, then the preferences.
		void askSafeMode(Options& options)
		{
			logging::info("Safe start mode");
			const std::string title = "GNUbase Safe Start Mode";
			if (ui::SdlWindowSystem::ask(title,
					"Deactivate all third-party plug-ins for this run? Only GNUbase's own processors are available "
					"then.",
					{"Keep Plug-ins", "Deactivate Plug-ins"}) == 1)
				options.noThirdPartyPlugins = true;
			const int preferences = ui::SdlWindowSystem::ask(title,
				"Preferences: use the current preferences, disable them for this run and start with the factory "
				"settings, or delete them and start with the factory settings. Deleting can't be undone.",
				{"Use Current", "Disable", "Delete"});
			if (preferences == 1)
				options.settingsMode = SafeSettings::Disable;
			else if (preferences == 2)
				options.settingsMode = SafeSettings::Delete;
		}

		// The UI couldn't be shown: says why, for the exit code g_exitSetup.
		int cannotShow(std::ostream& err, const std::exception& error)
		{
			err << "gnubase: cannot show the UI: " << error.what() << '\n';
			logging::error("Cannot show the UI: {}", error.what());
			return g_exitSetup;
		}
	}

	int runWindowed(const Options& options, std::ostream& out, std::ostream& err)
	{
		Options start = options;
		if (options.safeMode || safeModeKeysHeld())
			askSafeMode(start);
		engine::Engine engine(
			audio::makeBackends(), safeSettingsDirectory(start), midi::makeBackends(), factorySettings(start));
		engine.pluginHosts().setDeactivated(start.noThirdPartyPlugins);
		if (!setUpWindowedEngine(engine, options, err))
			return g_exitSetup;
		const std::optional<std::filesystem::path> resources = findResources();
		if (!resources)
		{
			err << "gnubase: cannot find its resources folder, which has the fonts and themes\n";
			logging::error("Cannot find the resources folder");
			return g_exitSetup;
		}
		logging::info("Resources from {}", resources->u8string());

		std::unique_ptr<AppUi> appUi;
		try
		{
			ui::WindowSpec main;
			main.size = {}; // 80 % of the primary monitor
			appUi = std::make_unique<AppUi>(engine, *resources, main, out);
			appUi->showProject();
			if (options.project.empty())
				appUi->openStartScreen();
		}
		catch (const std::runtime_error& error)
		{
			return cannotShow(err, error);
		}
		catch (const std::invalid_argument& error) // e.g. a menu item with a command that doesn't exist (#446)
		{
			return cannotShow(err, error);
		}
		ui::Shell& shell = appUi->shell();
		if (g_liveReload)
			shell.every(
				std::chrono::milliseconds(250), [&] { ui::reloadChangedDocuments(appUi->rmlui().files(), shell); });

		// MCP over HTTP with the UI tools, if asked for. Its tools use the shell, so it stops first.
		std::optional<mcp::Server> server;
		std::optional<mcp::HttpServer> http;
		if (options.transport == Transport::Http)
		{
			server.emplace(engine);
			addScriptTool(*server, engine);
			addUiTools(*server, shell);
			const std::string token = mcp::makeToken();
			http.emplace(*server, token, options.port);
			out << "MCP server: " << http->url() << "\nAccess token: " << token << '\n';
			out.flush();
			logging::info("Serving MCP on {}", http->url());
		}
		logging::info("UI on SDL's {} video driver, drawing through SDL_GPU {}", ui::SdlWindowSystem::driver(),
			appUi->renderer().driver());
		return shell.run();
	}
}
