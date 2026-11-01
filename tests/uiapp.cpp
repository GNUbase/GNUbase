// SPDX-License-Identifier: GPL-3.0-or-later
#include "uiapp.h"

#include "app/headless.h"

#include <RmlUi/Core/Context.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#if defined(__linux__) && defined(GNUBASE_SANITIZED)
#include <dlfcn.h>
#include <link.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace gnubase::test
{
	void useTestDisplay()
	{
#ifdef __linux__
		SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_DEFAULT); // SDL_VIDEO_DRIVER wins
#endif
	}

	void gpuUnavailable(const ui::GpuUnavailable& error)
	{
		const char* require = std::getenv("GNUBASE_REQUIRE_GPU");
		if (require != nullptr && std::string(require) == "1")
			FAIL(error.what());
		SKIP(error.what());
	}

	void keepVulkanDriversLoaded()
	{
#if defined(__linux__) && defined(GNUBASE_SANITIZED)
		dl_iterate_phdr(
			[](dl_phdr_info* info, size_t /*size*/, void* /*data*/)
			{
				if (std::string(info->dlpi_name).find("libvulkan_") != std::string::npos) [[maybe_unused]]
					void* kept = dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
				return 0;
			},
			nullptr);
#endif
	}

	UiApp::UiApp(ui::Size size, std::vector<std::unique_ptr<midi::Backend>> midiBackends,
		std::vector<std::unique_ptr<audio::Backend>> audioBackends)
		: m_size(size)
	{
		start(std::move(midiBackends), std::move(audioBackends));
	}

	void UiApp::restart()
	{
		m_automation.reset();
		m_ui.reset();
		m_engine.reset();
		start(midi::makeBackends(), audio::makeBackends());
	}

	void UiApp::start(std::vector<std::unique_ptr<midi::Backend>> midiBackends,
		std::vector<std::unique_ptr<audio::Backend>> audioBackends)
	{
		m_engine =
			std::make_unique<engine::Engine>(std::move(audioBackends), m_settings.path(), std::move(midiBackends));
		std::ostringstream problems;
		const bool ready = app::setUpEngine(*m_engine, app::Options{}, problems); // the dummy device, the app's default
		INFO(problems.str());
		REQUIRE(ready);

		useTestDisplay();
		ui::WindowSpec main;
		main.size = m_size;
		main.hidden = true; // tests don't flash windows on the desktop
		try
		{
			m_ui = std::make_unique<app::AppUi>(
				*m_engine, std::filesystem::path(GNUBASE_RESOURCES_DIR), main, std::cout, 4);
		}
		catch (const ui::GpuUnavailable& error)
		{
			gpuUnavailable(error);
		}
		keepVulkanDriversLoaded();
		m_automation = std::make_unique<ui::Automation>(m_ui->shell());
		shell().step(std::chrono::milliseconds(0));
	}

	engine::Json UiApp::call(const std::string& command, const engine::Json& parameters)
	{
		const engine::Result result = engine::Client(*m_engine).call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	Rml::ElementDocument& UiApp::show(const std::string& name)
	{
		Rml::ElementDocument* document =
			main().context().LoadDocument(std::string(GNUBASE_TEST_DATA_DIR) + "/ui/" + name);
		REQUIRE(document != nullptr);
		document->Show();
		main().requestRender();
		shell().step(std::chrono::milliseconds(0));
		return *document;
	}

	bool UiApp::waitUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!done())
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			shell().step(std::chrono::milliseconds(10));
		}
		return true;
	}

	void UiApp::setDpRatio(float ratio)
	{
		const float monitorScale = main().dpRatio() / shell().uiScale();
		shell().setUiScale(ratio / monitorScale);
		shell().step(std::chrono::milliseconds(0));
	}

	ui::Image UiApp::screenshot(const std::string& name, ui::Size dp)
	{
		const std::optional<ui::Image> captured = shell().screenshot(main().id());
		REQUIRE(captured.has_value());
		const ui::Image& image = captured.value_or(ui::Image{});
		const auto width = static_cast<int>(static_cast<float>(dp.width) * main().dpRatio());
		const auto height = static_cast<int>(static_cast<float>(dp.height) * main().dpRatio());
		REQUIRE(image.width >= width);
		REQUIRE(image.height >= height);
		ui::Image cropped{width, height, {}};
		for (int row = 0; row < height; ++row)
		{
			const auto begin = image.pixels.begin() + (static_cast<ptrdiff_t>(row) * image.width * 4);
			cropped.pixels.insert(cropped.pixels.end(), begin, begin + (static_cast<ptrdiff_t>(width) * 4));
		}
		const std::filesystem::path folder = outputDirectory() / "ui";
		std::filesystem::create_directories(folder);
		CHECK(ui::writePng(cropped, folder / (name + ".png")));
		return cropped;
	}

	void UiApp::checkGolden(const std::string& name, const ui::Image& image)
	{
		const ImageDiff diff = compareWithGolden("ui/" + name, image, {8, image.pixels.size() / 4 / 500});
		INFO(diff.report);
		CHECK(diff.equal);
	}
}
