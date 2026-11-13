// SPDX-License-Identifier: GPL-3.0-or-later
#include "uiapp.h"

#include "app/headless.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#if defined(__linux__) && defined(GNUBASE_SANITIZED)
#include <dlfcn.h>
#include <link.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

	std::filesystem::path patchedResources(const std::filesystem::path& folder, const std::string& document,
		const std::string& from, const std::string& to)
	{
		const std::filesystem::path resources = folder / "resources";
		std::filesystem::copy(GNUBASE_RESOURCES_DIR, resources, std::filesystem::copy_options::recursive);
		const std::filesystem::path file = resources / "ui" / document;
		std::string text = readText(file);
		const size_t at = text.find(from);
		INFO(file.u8string());
		REQUIRE(at != std::string::npos);
		text.replace(at, from.size(), to);
		std::ofstream(file, std::ios::binary) << text;
		return resources;
	}

	UiApp::UiApp(ui::Size size, std::vector<std::unique_ptr<midi::Backend>> midiBackends,
		std::vector<std::unique_ptr<audio::Backend>> audioBackends)
		: m_size(size)
	{
		start(std::move(midiBackends), std::move(audioBackends));
	}

	UiApp::UiApp(ui::Size size, std::filesystem::path resources) : m_size(size), m_resources(std::move(resources))
	{
		start(midi::makeBackends(), audio::makeBackends());
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
		// The dummy device, stopped: tests render offline and start it when they need real time.
		const bool ready = app::setUpEngine(*m_engine, app::Options{}, problems);
		INFO(problems.str());
		REQUIRE(ready);

		useTestDisplay();
		ui::WindowSpec main;
		main.size = m_size;
		main.hidden = true; // tests don't flash windows on the desktop
		try
		{
			m_ui = std::make_unique<app::AppUi>(*m_engine, m_resources, main, std::cout, 4);
		}
		catch (const ui::GpuUnavailable& error)
		{
			gpuUnavailable(error);
		}
		keepVulkanDriversLoaded();
		m_ui->shell().showOsFileDialogs(false); // tests answer them (Shell::answerFileDialog)
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

	namespace
	{
		Rml::Context& contextOf(UiApp& app, std::optional<ui::WindowId> window)
		{
			const ui::Window* found = window ? app.shell().find(*window) : &app.main();
			if (found == nullptr)
				throw std::invalid_argument("no such window");
			return found->context();
		}
	}

	double controlValue(UiApp& app, std::optional<ui::WindowId> window, const std::string& selector)
	{
		const engine::Json found = app.ui().dom(window, selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		return std::stod(found.at(0).at("attributes").at("value").get<std::string>());
	}

	double lateSteps(UiApp& app, std::optional<ui::WindowId> window, const std::string& selector, int steps,
		int direction, const std::function<void()>& step)
	{
		double value = controlValue(app, window, selector);
		for (int taken = 0; taken < steps; ++taken)
		{
			step();
			const double stepped = controlValue(app, window, selector);
			app.call("engine.version"); // after the commands that the step sent
			app.shell().step(std::chrono::milliseconds(0));
			const double now = controlValue(app, window, selector);
			INFO("step " << taken << ": from " << value << " to " << stepped << ", then " << now);
			CHECK((stepped - value) * direction < 0.0);
			CHECK(now == stepped);
			value = now;
		}
		return value;
	}

	LateDrag::LateDrag(UiApp& app, std::optional<ui::WindowId> window, std::string selector)
		: m_app(app), m_window(window), m_selector(std::move(selector)), m_context(contextOf(app, window))
	{
		const engine::Json found = app.ui().dom(window, m_selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		const engine::Json& box = found.at(0).at("box");
		m_at = {box.at("x").get<int>() + (box.at("width").get<int>() / 2),
			box.at("y").get<int>() + (box.at("height").get<int>() / 2)};
		m_context.ProcessMouseMove(m_at.x, m_at.y, 0);
		m_context.ProcessMouseButtonDown(0, 0);
	}

	double LateDrag::move(int steps, int direction, int perAnswer)
	{
		return lateSteps(m_app, m_window, m_selector, steps, direction,
			[this, direction, perAnswer]
			{
				for (int step = 0; step < perAnswer; ++step)
				{
					m_moved += 2 * direction;
					m_context.ProcessMouseMove(m_at.x, m_at.y + m_moved, 0);
				}
			});
	}

	void LateDrag::release(bool stays)
	{
		const double dragged = value();
		m_context.ProcessMouseButtonUp(0, 0);
		for (int answer = 0; stays && answer < 3; ++answer)
		{
			m_app.call("engine.version");
			m_app.shell().step(std::chrono::milliseconds(0));
			CHECK(value() == dragged);
		}
	}
}
