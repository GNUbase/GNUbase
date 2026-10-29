// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/windowed.h"

#include "app/headless.h"
#include "engine/engine.h"
#include "platform/log.h"
#include "ui/gpurenderer.h"
#include "ui/livereload.h"
#include "ui/rmlui.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"

#include <chrono>
#include <filesystem>
#include <memory>
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

		// The user's UI scale, from the user setting ui.scale.
		float uiScale(engine::Engine& engine)
		{
			engine::Client client(engine);
			const engine::Result result = client.call("settings.get", {{"key", "ui.scale"}});
			return result.ok() ? result.value.at("value").get<float>() : 1.0F;
		}
	}

	int runWindowed(const Options& options, std::ostream& err)
	{
		engine::Engine engine(audio::makeBackends(), std::filesystem::u8path(options.settings));
		if (!setUpEngine(engine, options, err))
			return g_exitSetup;

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
		ui::Shell shell(*windows, *renderer);
		shell.setUiScale(uiScale(engine));
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
