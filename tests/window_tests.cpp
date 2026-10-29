// SPDX-License-Identifier: GPL-3.0-or-later
// Real windows (#48): SDL3 windows drawn through SDL_GPU, screenshots, input and SDL's events. On Linux the tests use
// SDL's offscreen video driver unless SDL_VIDEO_DRIVER names another, so they need no display, only a Vulkan driver
// (Mesa's lavapipe in CI). Where SDL_GPU has no device, such as an Intel Mac whose GPU doesn't meet SDL_GPU's
// requirements, they are skipped, unless GNUBASE_REQUIRE_GPU=1 says this machine must have one, as in CI.
#include "testutils/testutils.h"
#include "ui/gpurenderer.h"
#include "ui/rmlui.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/SystemInterface.h>
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
			rmlui = std::make_unique<ui::RmlUi>(*windows);
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

	// An SDL event of the given type for the window, without data.
	SDL_Event windowEvent(Uint32 type, const ui::Window& window)
	{
		SDL_Event event{};
		event.type = type;
		event.window.windowID = window.id();
		return event;
	}

	// SDL reports positions in window coordinates, which are points on a Retina display.
	float toWindowCoordinates(const ui::Window& window, float pixels)
	{
		return pixels / SDL_GetWindowPixelDensity(ui::SdlWindowSystem::sdlWindow(window.id()));
	}

	// An event parameter of the type it was given, read without RmlUi's type conversion.
	template <typename T> T parameter(const Rml::Event& event, const char* name)
	{
		const Rml::Dictionary& parameters = event.GetParameters();
		const auto it = parameters.find(name);
		REQUIRE(it != parameters.end());
		return it->second.GetReference<T>();
	}

	struct DropListener final : Rml::EventListener
	{
		void ProcessEvent(Rml::Event& event) override
		{
			target = event.GetTargetElement()->GetId();
			files = parameter<Rml::String>(event, "files");
		}

		std::string target;
		std::string files;
	};

	Rml::ElementFormControlInput& field(const ui::Window& window, const char* id)
	{
		auto* input = dynamic_cast<Rml::ElementFormControlInput*>(window.context().GetDocument(0)->GetElementById(id));
		REQUIRE(input != nullptr);
		return *input;
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

TEST_CASE("files dropped on a window reach the element under the mouse", "[ui]")
{
	DropListener listener; // outlives the documents it listens to
	Gpu gpu;
	const ui::Window& window = gpu.open("drop.rml");
	gpu.shell->step(milliseconds(0));
	window.context().GetDocument(0)->AddEventListener(ui::g_fileDropEvent, &listener);

	// SDL reports a drop as its beginning, one event per file, and its end.
	const float x = toWindowCoordinates(window, 150.0F * window.dpRatio());
	const float y = toWindowCoordinates(window, 20.0F * window.dpRatio());
	push(windowEvent(SDL_EVENT_DROP_BEGIN, window));
	for (const char* file : {"/music/kick.wav", "/music/snare.flac"})
	{
		SDL_Event drop{};
		drop.type = SDL_EVENT_DROP_FILE;
		drop.drop.windowID = window.id();
		drop.drop.x = x;
		drop.drop.y = y;
		drop.drop.data = file;
		push(drop);
	}
	SDL_Event complete{};
	complete.type = SDL_EVENT_DROP_COMPLETE;
	complete.drop.windowID = window.id();
	complete.drop.x = x;
	complete.drop.y = y;
	push(complete);
	gpu.shell->step(milliseconds(0));
	CHECK(listener.target == "right");
	CHECK(listener.files == "/music/kick.wav\n/music/snare.flac");
}

TEST_CASE("a text field turns SDL's text input and IME on for its window", "[ui]")
{
	Gpu gpu;
	const ui::Window& window = gpu.open("input.rml");
	const ui::Window& other = gpu.open("input.rml");
	gpu.shell->step(milliseconds(0));
	SDL_Window* sdl = ui::SdlWindowSystem::sdlWindow(window.id());
	CHECK_FALSE(SDL_TextInputActive(sdl));

	Rml::ElementFormControlInput& name = field(window, "name");
	name.SetValue("");
	name.Focus();
	CHECK(SDL_TextInputActive(sdl));
	CHECK_FALSE(SDL_TextInputActive(ui::SdlWindowSystem::sdlWindow(other.id())));

	SDL_Event typed{};
	typed.type = SDL_EVENT_TEXT_INPUT;
	typed.text.windowID = window.id();
	typed.text.text = "ab";
	push(typed);
	gpu.shell->step(milliseconds(0));
	CHECK(name.GetValue() == "ab");

	// An input method composes text before the user commits it.
	SDL_Event editing{};
	editing.type = SDL_EVENT_TEXT_EDITING;
	editing.edit.windowID = window.id();
	editing.edit.text = "ka";
	editing.edit.start = 2;
	editing.edit.length = 0;
	push(editing);
	gpu.shell->step(milliseconds(0));
	CHECK(name.GetValue() == "abka");
	editing.edit.text = "";
	editing.edit.start = 0;
	push(editing);
	typed.text.text = "x";
	push(typed);
	gpu.shell->step(milliseconds(0));
	CHECK(name.GetValue() == "abx");

	name.Blur();
	CHECK_FALSE(SDL_TextInputActive(sdl));
}

TEST_CASE("text fields copy to and paste from SDL's clipboard", "[ui]")
{
	const Gpu gpu;
	gpu.open("input.rml");
	char* saved = SDL_GetClipboardText(); // the clipboard of whoever runs the tests
	Rml::SystemInterface* system = Rml::GetSystemInterface();
	REQUIRE(system != nullptr);

	system->SetClipboardText("copied in GNUbase");
	char* text = SDL_GetClipboardText();
	CHECK(std::string(text) == "copied in GNUbase");
	SDL_free(text);
	REQUIRE(SDL_SetClipboardText("copied elsewhere"));
	Rml::String pasted;
	system->GetClipboardText(pasted);
	CHECK(pasted == "copied elsewhere");

	SDL_SetClipboardText(saved);
	SDL_free(saved);
}

TEST_CASE("a file dialog's answer reaches its caller on the UI thread", "[ui]")
{
	Gpu gpu;
	const ui::Window& window = gpu.open("boxes.rml");
	// An unknown dialog driver makes SDL answer with an error at once, so no dialog opens during the tests.
	REQUIRE(SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "none"));
	int answers = 0;
	ui::FileDialogResult answer;
	ui::FileDialog dialog;
	dialog.parent = window.id();
	dialog.filters = {{"Audio files", "wav;flac"}};
	gpu.shell->showFileDialog(dialog,
		[&](const ui::FileDialogResult& result)
		{
			++answers;
			answer = result;
		});
	CHECK(answers == 0); // only in a step
	for (int step = 0; step < 100 && answers == 0; ++step)
		gpu.shell->step(milliseconds(10));
	SDL_ResetHint(SDL_HINT_FILE_DIALOG_DRIVER);
	CHECK(answers == 1);
	CHECK(answer.files.empty());
	CHECK_FALSE(answer.error.empty());
}
