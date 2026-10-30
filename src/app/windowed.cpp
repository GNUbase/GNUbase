// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/windowed.h"

#include "app/headless.h"
#include "app/resources.h"
#include "engine/engine.h"
#include "platform/log.h"
#include "ui/fonts.h"
#include "ui/gpurenderer.h"
#include "ui/livereload.h"
#include "ui/rmlui.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"
#include "ui/themes.h"
#include "ui/uisettings.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>

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
	}

	int runWindowed(const Options& options, std::ostream& err)
	{
		engine::Engine engine(audio::makeBackends(), std::filesystem::u8path(options.settings));
		if (!setUpEngine(engine, options, err))
			return g_exitSetup;
		const std::optional<std::filesystem::path> resources = findResources();
		if (!resources)
		{
			err << "gnubase: cannot find its resources folder, which has the fonts and themes\n";
			logging::error("Cannot find the resources folder");
			return g_exitSetup;
		}
		logging::info("Resources from {}", resources->u8string());

		// Made in this order and destroyed in the reverse: RmlUi releases its resources through the renderer, and
		// the shell's windows need all three.
		std::unique_ptr<ui::SdlWindowSystem> windows;
		std::unique_ptr<ui::GpuRenderer> renderer;
		try
		{
			windows = std::make_unique<ui::SdlWindowSystem>();
			renderer = std::make_unique<ui::GpuRenderer>(*windows);
		}
		catch (const std::runtime_error& error)
		{
			err << "gnubase: cannot show the UI: " << error.what() << '\n';
			logging::error("Cannot show the UI: {}", error.what());
			return g_exitSetup;
		}
		ui::RmlUi rmlui(*windows);
		std::unique_ptr<ui::Themes> themes;
		try
		{
			ui::Fonts(*resources / "fonts").load();
			themes = std::make_unique<ui::Themes>(*resources);
		}
		catch (const std::runtime_error& error)
		{
			err << "gnubase: cannot load its fonts and themes: " << error.what() << '\n';
			logging::error("Cannot load the fonts and themes: {}", error.what());
			return g_exitSetup;
		}
		ui::Shell shell(*windows, *renderer);
		// The theme and the UI scale from the user settings, also when they change while GNUbase runs.
		const ui::UiSettings uiSettings(engine, shell, *themes);
		if (g_liveReload)
			shell.every(std::chrono::milliseconds(250), [&] { ui::reloadChangedDocuments(rmlui.files(), shell); });
		ui::WindowSpec main;
		main.size = {}; // 80 % of the primary monitor
		shell.open(main);
		logging::info("UI on SDL's {} video driver, drawing through SDL_GPU {}", ui::SdlWindowSystem::driver(),
			renderer->driver());
		return shell.run();
	}
}
