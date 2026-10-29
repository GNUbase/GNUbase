// SPDX-License-Identifier: GPL-3.0-or-later
// Real windows (#48): SDL3 windows drawn through SDL_GPU, screenshots, input and SDL's events. On Linux the tests use
// SDL's offscreen video driver unless SDL_VIDEO_DRIVER names another, so they need no display, only a Vulkan driver
// (Mesa's lavapipe in CI). Where SDL_GPU has no device, such as CI's macOS Intel runner, they are skipped, unless
// GNUBASE_REQUIRE_GPU=1 says this machine must have one.
#include "testutils/testutils.h"
#include "ui/gpurenderer.h"
#include "ui/rmlui.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#if defined(__linux__) && defined(GNUBASE_SANITIZED)
#include <dlfcn.h>
#include <link.h>
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace gnubase;
using std::chrono::milliseconds;

namespace
{
	using Color = std::array<uint8_t, 4>;
	constexpr Color g_black{0, 0, 0, 255};
	constexpr Color g_red{255, 0, 0, 255};
	constexpr Color g_green{0, 255, 0, 255};
	constexpr Color g_blue{0, 0, 255, 255};

	std::string document(const char* name)
	{
		return std::string(GNUBASE_TEST_DATA_DIR) + "/ui/" + name;
	}

#if defined(__linux__) && defined(GNUBASE_SANITIZED)
	// LeakSanitizer can't see what an unloaded library still referenced, so once SDL_GPU's Vulkan driver is unloaded,
	// the memory Mesa keeps for the life of the process (allocated on its own threads) shows up as leaks. Loading the
	// drivers once more, and never closing them, keeps them loaded until the tests exit.
	void keepVulkanDriversLoaded()
	{
		dl_iterate_phdr(
			[](dl_phdr_info* info, size_t /*size*/, void* /*data*/)
			{
				if (std::string(info->dlpi_name).find("libvulkan_") != std::string::npos) [[maybe_unused]]
					void* kept = dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
				return 0;
			},
			nullptr);
	}
#endif

	// SDL's video, the GPU renderer, RmlUi and the shell, made and destroyed in the app's order. Skips the test if
	// this machine has no SDL_GPU device.
	struct Gpu
	{
		Gpu()
		{
#ifdef __linux__
			SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_DEFAULT); // SDL_VIDEO_DRIVER wins
#endif
			windows = std::make_unique<ui::SdlWindowSystem>();
			try
			{
				renderer = std::make_unique<ui::GpuRenderer>(*windows);
			}
			catch (const ui::GpuUnavailable& error)
			{
				const char* require = std::getenv("GNUBASE_REQUIRE_GPU");
				if (require != nullptr && std::string(require) == "1")
					FAIL(error.what());
				SKIP(error.what());
			}
#if defined(__linux__) && defined(GNUBASE_SANITIZED)
			keepVulkanDriversLoaded();
#endif
			rmlui = std::make_unique<ui::RmlUi>();
			shell = std::make_unique<ui::Shell>(*windows, *renderer);
		}

		~Gpu()
		{
			shell.reset();
			rmlui.reset();
			renderer.reset();
			windows.reset();
		}

		Gpu(const Gpu&) = delete;
		Gpu& operator=(const Gpu&) = delete;
		Gpu(Gpu&&) = delete;
		Gpu& operator=(Gpu&&) = delete;

		ui::Window& open(const char* name, int width = 400, int height = 300) const
		{
			ui::WindowSpec spec;
			spec.title = name;
			spec.size = {width, height};
			spec.hidden = true; // tests don't flash windows on the desktop
			ui::Window& window = shell->open(spec);
			Rml::ElementDocument* loaded = window.context().LoadDocument(document(name));
			REQUIRE(loaded != nullptr);
			loaded->Show();
			window.requestRender();
			return window;
		}

		// A screenshot of the window; written to the test output folder for a look when a check fails.
		ui::Image screenshot(const ui::Window& window, const std::string& name) const
		{
			const std::optional<ui::Image> captured = shell->screenshot(window.id());
			REQUIRE(captured.has_value());
			const ui::Image image = captured.value_or(ui::Image{});
			const std::filesystem::path folder = test::outputDirectory() / "ui";
			std::filesystem::create_directories(folder);
			CHECK(ui::writePng(image, folder / (name + ".png")));
			return image;
		}

		std::unique_ptr<ui::SdlWindowSystem> windows;
		std::unique_ptr<ui::GpuRenderer> renderer;
		std::unique_ptr<ui::RmlUi> rmlui;
		std::unique_ptr<ui::Shell> shell;
	};

	bool near(const Color& actual, const Color& expected)
	{
		for (size_t i = 0; i < actual.size(); ++i)
			if (std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i])) > 2)
				return false;
		return true;
	}

	// The pixel at (x, y) dp in the window, for a window with the given dp ratio.
	Color at(const ui::Image& image, float dpRatio, float x, float y)
	{
		const auto column = static_cast<int>(std::floor(x * dpRatio));
		const auto row = static_cast<int>(std::floor(y * dpRatio));
		REQUIRE(column < image.width);
		REQUIRE(row < image.height);
		return image.pixel(column, row);
	}

	void push(SDL_Event event)
	{
		REQUIRE(SDL_PushEvent(&event));
	}
}

TEST_CASE("windows draw their RmlUi documents through SDL_GPU at their dp ratio", "[ui]")
{
	Gpu gpu;
	const ui::Window& window = gpu.open("boxes.rml");
	gpu.shell->step(milliseconds(0));
	CHECK(window.frames() == 1);
	INFO("SDL_GPU " << gpu.renderer->driver() << " on SDL's " << ui::SdlWindowSystem::driver() << " video driver");

	for (const float uiScale : {1.0F, 2.0F})
	{
		gpu.shell->setUiScale(uiScale);
		const float ratio = window.dpRatio();
		INFO("dp ratio " << ratio);
		const ui::Image image = gpu.screenshot(window, "boxes-" + std::to_string(static_cast<int>(uiScale)) + "x");
		CHECK(image.width == window.pixelSize().width);
		CHECK(image.height == window.pixelSize().height);
		// The box covers 10 to 110 dp across and 20 to 70 dp down, sharp to the pixel at every ratio.
		CHECK(near(at(image, ratio, 60.0F, 45.0F), g_red));
		CHECK(near(at(image, ratio, 10.0F, 20.0F), g_red));
		CHECK(near(image.pixel(int(std::floor(110.0F * ratio)) - 1, int(std::floor(70.0F * ratio)) - 1), g_red));
		CHECK(near(image.pixel(int(std::floor(10.0F * ratio)) - 1, int(std::floor(45.0F * ratio))), g_black));
		CHECK(near(at(image, ratio, 110.0F, 45.0F), g_black));
		CHECK(near(at(image, ratio, 60.0F, 70.0F), g_black));
		CHECK(near(at(image, ratio, 180.0F, 120.0F), g_black));
	}
}

TEST_CASE("each window draws its own document, and closing one leaves the other", "[ui]")
{
	Gpu gpu;
	ui::Window& boxes = gpu.open("boxes.rml");
	const ui::Window& other = gpu.open("other.rml", 300, 200);
	const ui::WindowId otherId = other.id();
	gpu.shell->step(milliseconds(0));
	CHECK(boxes.frames() == 1);
	CHECK(other.frames() == 1);

	ui::Image image = gpu.screenshot(other, "other");
	CHECK(near(at(image, other.dpRatio(), 120.0F, 30.0F), g_blue));
	CHECK(near(at(image, other.dpRatio(), 60.0F, 45.0F), g_black));
	image = gpu.screenshot(boxes, "boxes");
	CHECK(near(at(image, boxes.dpRatio(), 60.0F, 45.0F), g_red));
	CHECK(near(at(image, boxes.dpRatio(), 120.0F, 30.0F), g_black));

	SDL_Event close{};
	close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
	close.window.windowID = otherId;
	push(close);
	gpu.shell->step(milliseconds(0));
	CHECK(gpu.shell->find(otherId) == nullptr);
	CHECK(ui::SdlWindowSystem::sdlWindow(otherId) == nullptr);
	REQUIRE(gpu.shell->running());

	boxes.requestRender();
	gpu.shell->step(milliseconds(0));
	CHECK(boxes.frames() == 2);
	image = gpu.screenshot(boxes, "boxes-after-close");
	CHECK(near(at(image, boxes.dpRatio(), 60.0F, 45.0F), g_red));
}

TEST_CASE("mouse input reaches the document of the window it happened in", "[ui]")
{
	Gpu gpu;
	const ui::Window& window = gpu.open("boxes.rml");
	gpu.shell->step(milliseconds(0));
	CHECK(near(at(gpu.screenshot(window, "hover-before"), window.dpRatio(), 60.0F, 45.0F), g_red));

	// SDL reports the mouse in window coordinates; the window may have more pixels than that.
	const ui::Rect bounds = gpu.windows->bounds(window.id());
	const float pixelsPerUnit = static_cast<float>(window.pixelSize().width) / static_cast<float>(bounds.width);
	SDL_Event motion{};
	motion.type = SDL_EVENT_MOUSE_MOTION;
	motion.motion.windowID = window.id();
	motion.motion.x = 60.0F * window.dpRatio() / pixelsPerUnit;
	motion.motion.y = 45.0F * window.dpRatio() / pixelsPerUnit;
	push(motion);
	const uint64_t frames = window.frames();
	gpu.shell->step(milliseconds(0));
	CHECK(window.frames() == frames + 1);
	CHECK(near(at(gpu.screenshot(window, "hover-after"), window.dpRatio(), 60.0F, 45.0F), g_green));
}

TEST_CASE("closing the main window or a quit request ends the shell", "[ui]")
{
	Gpu gpu;
	const ui::WindowId main = gpu.open("boxes.rml").id();
	gpu.open("other.rml");
	gpu.shell->step(milliseconds(0));

	SECTION("closing the main window")
	{
		SDL_Event close{};
		close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
		close.window.windowID = main;
		push(close);
	}
	SECTION("a quit request, as on Cmd+Q or SIGTERM")
	{
		SDL_Event quit{};
		quit.type = SDL_EVENT_QUIT;
		push(quit);
	}
	CHECK(gpu.shell->run() == 0);
	CHECK_FALSE(gpu.shell->running());
	CHECK(gpu.shell->windows().empty());
	CHECK(ui::SdlWindowSystem::sdlWindow(main) == nullptr);
}
