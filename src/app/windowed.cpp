// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/windowed.h"

#include "app/headless.h"
#include "app/resources.h"
#include "engine/engine.h"
#include "platform/log.h"
#include "ui/enginebinding.h"
#include "ui/fonts.h"
#include "ui/gpurenderer.h"
#include "ui/livereload.h"
#include "ui/rmlui.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"

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

		// The user's UI scale, from the user setting ui.scale, read before the UI runs.
		float uiScale(engine::Engine& engine)
		{
			engine::Client client(engine, "ui");
			const engine::Result result = client.call("settings.get", {{"key", "ui.scale"}});
			return result.ok() ? result.value.at("value").get<float>() : 1.0F;
		}
	}

	int runWindowed(const Options& options, std::ostream& err)
	{
		engine::Engine engine(audio::makeBackends(), std::filesystem::u8path(options.settings));
		if (!setUpEngine(engine, options, err))
			return g_exitSetup;
		const std::optional<std::filesystem::path> resources = findResources();
		if (!resources)
		{
			err << "gnubase: cannot find its resources folder, which has the fonts\n";
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
		try
		{
			ui::Fonts(*resources / "fonts").load();
		}
		catch (const std::runtime_error& error)
		{
			err << "gnubase: cannot load its fonts: " << error.what() << '\n';
			logging::error("Cannot load the fonts: {}", error.what());
			return g_exitSetup;
		}
		ui::Shell shell(*windows, *renderer);
		shell.setUiScale(uiScale(engine));
		ui::EngineBinding binding(engine, [&shell] { shell.wake(); });
		shell.eachStep([&binding] { return binding.update(); });
		const ui::Subscription scale = binding.on("settings.changed",
			[&shell](const engine::Event& event)
			{
				const engine::Json& values = event.data.at("values");
				if (const auto value = values.find("ui.scale"); value != values.end())
					shell.setUiScale(value->get<float>());
			});
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
